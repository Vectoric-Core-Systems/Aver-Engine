// Editor: the MCP control channel's commands and ABI registrations.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/OcAnim.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace aver {
#if AVER_MODULE_MCP
#if AVER_MODULE_SCENE
namespace {

// JSON pieces for the level ABI's replies. The bridge escapes the whole reply once more on the wire,
// so a client parses twice; these only have to make the result string itself valid JSON.
std::string jq(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string jb(bool b) { return b ? "true" : "false"; }

// Fixed point, four places: enough for centimetres and degrees. A non-finite value reads 0.
std::string jnum(f64 v) {
    if (!std::isfinite(v)) v = 0.0;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

std::string jv3(f32 x, f32 y, f32 z) { return "[" + jnum(x) + "," + jnum(y) + "," + jnum(z) + "]"; }

std::string jid(AvId e) { return std::to_string(static_cast<u32>(e)); }

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(const std::string& s) {
    usize b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// An entity's animation as the list and reply rows spell it: the authored values, or null.
std::string jAnim(const EntityAnim* a) {
    if (!a) return "null";
    return "{\"clip\":" + jq(a->clip) + ",\"speed\":" + jnum(a->speed) + ",\"time\":" + jnum(a->time) +
           ",\"once\":" + jb(a->once) + "}";
}

// True when every component of `rel` (forward slashes) is an entry under `root` spelled exactly so. The
// runtime finds a clip by the hash of its spelling, so "anims/Door.ocanim" for a file named
// Anims/door.ocanim exists on Windows and still never resolves in the game.
bool spelledAsOnDisk(const std::filesystem::path& root, const std::string& rel) {
    namespace fs = std::filesystem;
    fs::path dir = root;
    usize pos = 0;
    while (pos < rel.size()) {
        const usize slash = rel.find('/', pos);
        const std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        pos = slash == std::string::npos ? rel.size() : slash + 1;
        if (part.empty() || part == "." || part == "..") return false;
        std::error_code ec;
        bool found = false;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().filename().string() == part) { found = true; break; }
        if (!found) return false;
        dir /= part;
    }
    return true;
}

// A clip path as the `anim` token spells it -- content-relative, forward slashes, extension kept --
// checked the way the runtime will resolve it: through the content index (id = fnv1a64 of the spelling)
// and AnimSystem's own clip(), which must hand back an OBJECT clip (a skeletal one would load and then
// never move a placed mesh). Checking the file on disk instead accepted a clip the index had never seen,
// which then never played.
//
// A clip written after the project opened is not in the index (nothing rescans it short of reopening the
// project), so that one file is indexed here, under the spelling it was named with -- the same single-file
// call the editor's other late-bound assets use -- provided the spelling is the file's own.
bool checkObjectClip(game::GameContent& content, const std::string& contentDir, std::string& clip,
                     std::string& why) {
    namespace fs = std::filesystem;
    for (char& c : clip) if (c == '\\') c = '/';
    if (contentDir.empty()) { why = "no project is open, so clip '" + clip + "' cannot be resolved"; return false; }
    std::error_code ec;
    if (fs::path(clip).is_absolute()) {
        const fs::path rel = fs::relative(fs::path(clip), fs::path(contentDir), ec);
        if (ec || rel.empty() || rel.begin()->string() == "..") {
            why = "clip '" + clip + "' is outside the project's Content directory";
            return false;
        }
        clip = rel.generic_string();
    }
    if (lowered(fs::path(clip).extension().string()) != ".ocanim") {
        why = "clip '" + clip + "' must be a content-relative .ocanim path, extension included";
        return false;
    }

    const u64 id = fnv1a64(std::string_view(clip));
    if (content.pathFor(id).empty()) {
        const fs::path file = fs::path(contentDir) / clip;
        if (!fs::exists(file, ec)) { why = "no clip file at " + file.string(); return false; }
        if (!spelledAsOnDisk(fs::path(contentDir), clip)) {
            why = "clip '" + clip + "' is not spelled exactly as its file is named under Content (case "
                  "included): the runtime finds a clip by that exact path";
            return false;
        }
        content.indexAsset(id, file.string());
        AVER_INFO("[Mcp] clip '{}' was not in the content index (added after the project opened); indexed it", clip);
    }

    const fmt::OcAnimation* data = anim::animSystem().clip(id);
    if (!data) {
        // Say which half failed: an unreadable file, or a clip the animation system cannot be handed.
        const std::string path = content.pathFor(id);
        fmt::OcAnimation probe;
        std::string err;
        if (!fmt::loadOcAnim(path, probe, &err))
            why = "could not read clip " + path + (err.empty() ? std::string() : ": " + err);
        else
            why = "the animation system cannot load clip '" + clip + "' although the file reads: it keeps "
                  "the answer it gave before the file existed or changed, until the project is reopened";
        return false;
    }
    if (!(data->flags & fmt::kOcAnimObject)) {
        why = "clip '" + clip + "' is not an object clip (kOcAnimObject): it poses a skeleton, so it "
              "cannot move a placed mesh";
        return false;
    }
    return true;
}

// What a place text held besides the placements the parser read. The parser skips a record it does not
// know without a word, so a misspelt PLCAE or a pasted SUN line would otherwise vanish from the reply.
// BEGIN/END, blank lines and comments are structure, not records, and are not counted.
struct PlaceLines {
    usize records = 0;                                      // lines that asked for something
    std::vector<std::pair<usize, std::string>> ignored;     // 1-based line in the text, and its text
};

PlaceLines scanPlaceLines(const std::string& text) {
    PlaceLines out;
    usize lineNo = 0, pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (const usize hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trimmed(line);
        if (line.empty()) continue;
        usize sp = 0;
        while (sp < line.size() && !std::isspace(static_cast<unsigned char>(line[sp]))) ++sp;
        const std::string key = lowered(line.substr(0, sp));
        if (key == "begin" || key == "end") continue;
        ++out.records;
        if (key == "place" || key == "placeg" || key == "child" || key == "childg") continue;
        out.ignored.emplace_back(lineNo, line.size() > 80 ? line.substr(0, 80) + "..." : line);
    }
    return out;
}

// The first few ignored lines as a JSON array, for the reply.
std::string ignoredJson(const PlaceLines& scan) {
    std::string j = "[";
    for (usize i = 0; i < scan.ignored.size() && i < 8; ++i) {
        if (i) j += ",";
        j += "{\"line\":" + std::to_string(scan.ignored[i].first) + ",\"text\":" + jq(scan.ignored[i].second) + "}";
    }
    return j + "]";
}

} // namespace
#endif // AVER_MODULE_SCENE

// Applies one MCP command: an ABI call, or synthetic input posted to the window as Win32 messages.
void SandboxApp::applyMcpCommand(const mcp::Command& c) {
    HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
    if (!hwnd) return;

    if (c.name == "abi") {
        std::string result, why;
        if (mcp_.callAbi(c.abi, result, why))
            AVER_INFO("[Mcp] {}::{} -> {}", c.abi.module, c.abi.fn, result);
        else
            AVER_WARN("[Mcp] {}::{} refused: {}", c.abi.module, c.abi.fn, why);
        return;
    }

    for (const mcp::InputEvent& e : c.events) {
        const LPARAM lp = MAKELPARAM(e.x, e.y);
        switch (e.kind) {
            case mcp::InputEvent::Kind::MouseMove: {
                POINT pt{ e.x, e.y };
                ::ClientToScreen(hwnd, &pt);
                ::SetCursorPos(pt.x, pt.y);
                ::PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
                if (::GetForegroundWindow() != hwnd) ::SetForegroundWindow(hwnd);
                break;
            }
            case mcp::InputEvent::Kind::MouseDown:
                ::PostMessageW(hwnd,
                    e.button == 1 ? WM_RBUTTONDOWN : e.button == 2 ? WM_MBUTTONDOWN : WM_LBUTTONDOWN,
                    e.button == 1 ? MK_RBUTTON : e.button == 2 ? MK_MBUTTON : MK_LBUTTON, lp);
                break;
            case mcp::InputEvent::Kind::MouseUp:
                ::PostMessageW(hwnd,
                    e.button == 1 ? WM_RBUTTONUP : e.button == 2 ? WM_MBUTTONUP : WM_LBUTTONUP,
                    0, lp);
                break;
            case mcp::InputEvent::Kind::KeyDown:
                ::PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(e.key), 0);
                break;
            case mcp::InputEvent::Kind::KeyUp:
                ::PostMessageW(hwnd, WM_KEYUP, static_cast<WPARAM>(e.key), 0);
                break;
            case mcp::InputEvent::Kind::Text:
                // WM_CHAR per code unit: WM_KEYDOWN carries a virtual key, not a character.
                for (char ch : e.text)
                    ::PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(ch)), 0);
                break;
        }
    }
}

// Registers every built module's plain-C seam with the MCP bridge, under its own name.
void SandboxApp::registerMcpAbis() {
    mcp_.registerAbi("editor", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
        if (a.fn == "tool") {
            // kToolNames[4..7] are the sculpt tools, present only where AVER_MODULE_LANDSCAPE is
            // -- the bound below must track its size or an in-range request would read past it.
#if AVER_MODULE_LANDSCAPE
            constexpr int kMaxTool = 7;
#else
            constexpr int kMaxTool = 3;
#endif
            if (a.args.empty()) { w = "tool needs a tool index 0-" + std::to_string(kMaxTool); return false; }
            const int t = static_cast<int>(a.args[0]);
            if (t < 0 || t > kMaxTool) { w = "tool index out of range 0-" + std::to_string(kMaxTool); return false; }
            tool_ = static_cast<Tool>(t);
            r = kToolNames[t];
            return true;
        }
        if (a.fn == "screenshot") {
            if (a.text.empty()) { w = "screenshot needs a path in \"text\""; return false; }
            shot_ = a.text;
            capDone_ = false;
            r = a.text;
            return true;
        }
        if (a.fn == "mouse") {
#if AVER_WITH_IMGUI
            const ImGuiIO& io = ImGui::GetIO();
            char b[160];
            std::snprintf(b, sizeof b, "imgui pos=(%.0f,%.0f) display=(%.0f,%.0f) down=%d focus=%d",
                          io.MousePos.x, io.MousePos.y, io.DisplaySize.x, io.DisplaySize.y,
                          io.MouseDown[0] ? 1 : 0, io.AppFocusLost ? 0 : 1);
            r = b;
            return true;
#else
            w = "this build has no UI";
            return false;
#endif
        }
        if (a.fn == "widgets") {
            // THE REGISTRY IS THE EDITOR UI'S OWN. uiReg_ records which ImGui widget answers to which
        // name, so there is nothing for it to describe in a build with no editor UI -- and the
        // member itself is compiled out, which is what `no-ui` failed on. The command stays,
        // because an MCP client should get an answer rather than a closed socket.
#if AVER_WITH_IMGUI
        r = uiReg_.describe();
#else
        r = "{\"widgets\":[],\"note\":\"this build has no editor UI\"}";
#endif
            if (r.empty()) { w = "nothing tracked yet -- no UI frame has completed"; return false; }
            return true;
        }
        w = "editor has no entry point '" + a.fn + "'";
        return false;
    });
#if AVER_MODULE_PHYSICS
    mcp_.registerAbi("physics", [](const mcp::AbiCall& a, std::string& r, std::string& w) {
        if (a.fn == "bodyCount") { r = std::to_string(aver_phys_body_count()); return true; }
        if (a.fn == "ready")     { r = aver_phys_ready() ? "1" : "0"; return true; }
        w = "aver_phys_" + a.fn + " is not exposed";
        return false;
    });
#endif

    // ALWAYS REGISTERED, even when this build has no scene module: a client asking `modules` sees
    // "world" either way, and a call into it gets a clear "no scene module" refusal rather than
    // the generic "no ABI registered" one -- degrade the entry points, don't compile them out.
    mcp_.registerAbi("world", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
        if (a.fn == "stream_on") {
            setChunkStreamingEnabled(true);
            if (!streaming_.enabled()) {
                w = "streaming did not turn on -- see the editor log (no project open, or "
                    "ChunkWorld::open failed)";
                return false;
            }
            r = "on";
            return true;
        }
        if (a.fn == "stream_off") {
            setChunkStreamingEnabled(false);
            r = "off";
            return true;
        }
        if (a.fn == "stream_stats") {
            const world::StreamStats& s = streaming_.stats();
            char buf[320];
            std::snprintf(buf, sizeof buf,
                "streaming=%s residentChunks=%u residentEntities=%u loadedThisUpdate=%u "
                "evictedThisUpdate=%u pendingLoads=%u failedLoads=%u totalLoads=%u "
                "lastLoadMs=%.3f totalLoadMs=%.3f",
                streaming_.enabled() ? "on" : "off", s.residentChunks, s.residentEntities,
                s.loadedThisUpdate, s.evictedThisUpdate, s.pendingLoads, s.failedLoads,
                s.totalLoads, s.lastLoadMs, s.totalLoadMs);
            r = buf;
            return true;
        }
        if (a.fn == "warp") {
            if (a.args.size() < 3) { w = "warp needs 3 args: x y z (world centimetres)"; return false; }
            camPos_ = Vec3{static_cast<f32>(a.args[0]), static_cast<f32>(a.args[1]),
                           static_cast<f32>(a.args[2])};
            // A teleport, not a move: the next chunk-streaming update must not see this as a
            // huge one-frame velocity computed against wherever the camera used to be -- the
            // same rule frameCameraOnLevel's own comment states for the same reason.
            streaming_.resetVelocityTracking();
            char buf[96];
            std::snprintf(buf, sizeof buf, "camPos=(%.1f,%.1f,%.1f)", camPos_.x, camPos_.y, camPos_.z);
            r = buf;
            return true;
        }
        w = "world has no entry point '" + a.fn + "'";
        return false;
#else
        (void)a;
        w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- chunk streaming and the "
            "world ABI are unavailable";
        return false;
#endif
    });

    // ALWAYS REGISTERED too, for the same reason as "world" above.
    mcp_.registerAbi("graph", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
        if (a.fn == "entity_pos") {
            if (a.args.empty()) { w = "entity_pos needs an entity id"; return false; }
            const scene::Entity ent =
                static_cast<scene::Entity>(static_cast<u32>(static_cast<i32>(a.args[0])));
            if (!scene::World::instance().valid(ent)) { w = "no live entity with that id"; return false; }
            const Vec3 p = scene::World::instance().localTransform(ent).position;
            char buf[96];
            std::snprintf(buf, sizeof buf, "(%.3f,%.3f,%.3f)", p.x, p.y, p.z);
            r = buf;
            return true;
        }
#if AVER_MODULE_SCRIPTING
        // load/attach/tick: GraphHost is now hosted from native code via ScriptHost::graphLoad/
        // graphTick (same seam the drone entity uses). graphLoad binds a path AND an entity in one
        // call, so "load" just stages the path and "attach" is what actually calls it.
        if (a.fn == "load") {
            if (a.text.empty()) { w = "load needs a path in \"text\""; return false; }
            if (!scripts_.ready()) {
                w = "the scripting host is not running: " + scripts_.declineReason();
                return false;
            }
            if (!scripts_.graphAvailable()) {
                w = "this build's staged bridge exports no Graph entry points -- rebuild with the "
                    ".NET SDK present so Aver.Scripting.Bridge picks up GraphLoad/GraphTick/GraphUnload";
                return false;
            }
            mcpGraphPath_ = a.text;
            r = "path staged: " + mcpGraphPath_ + " -- call graph::attach <entityId> to load, "
                "compile and bind it";
            return true;
        }
        if (a.fn == "attach") {
            if (a.args.empty()) { w = "attach needs an entity id"; return false; }
            if (mcpGraphPath_.empty()) { w = "call graph::load <path> first"; return false; }
            const scene::Entity ent =
                static_cast<scene::Entity>(static_cast<u32>(static_cast<i32>(a.args[0])));
            if (!scene::World::instance().valid(ent)) { w = "no live entity with that id"; return false; }
            if (!scripts_.graphLoad(static_cast<i32>(ent), mcpGraphPath_)) {
                w = "graph failed to load/compile from '" + mcpGraphPath_ + "' -- see the [Graph] "
                    "error line just above in the editor log for why";
                return false;
            }
            mcpGraphEntity_ = ent;
            mcpGraphTimeSeconds_ = 0.0f;
            r = "attached entity #" + std::to_string(static_cast<u32>(ent)) + " to " + mcpGraphPath_;
            return true;
        }
        if (a.fn == "tick") {
            if (a.args.empty()) { w = "tick needs a seconds value (absolute time, not a delta)"; return false; }
            if (mcpGraphEntity_ == scene::kInvalidEntity) {
                w = "no entity attached -- call graph::attach <entityId> first";
                return false;
            }
            if (!scene::World::instance().valid(mcpGraphEntity_)) {
                w = "the attached entity no longer exists";
                return false;
            }
            mcpGraphTimeSeconds_ = static_cast<f32>(a.args[0]);
            scripts_.graphTick(static_cast<i32>(mcpGraphEntity_), mcpGraphTimeSeconds_);
            const Vec3 p = scene::World::instance().localTransform(mcpGraphEntity_).position;
            char buf[128];
            std::snprintf(buf, sizeof buf, "t=%.3f pos=(%.3f,%.3f,%.3f)",
                          mcpGraphTimeSeconds_, p.x, p.y, p.z);
            r = buf;
            return true;
        }
#else
        if (a.fn == "load" || a.fn == "attach" || a.fn == "tick") {
            w = "this build has no scripting module (AVER_MODULE_SCRIPTING=OFF) -- graph hosting is "
                "unavailable; entity_pos still works because it reads scene::World directly";
            return false;
        }
#endif
        w = "graph has no entry point '" + a.fn + "'";
        return false;
#else
        (void)a;
        w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- the graph ABI is unavailable";
        return false;
#endif
    });

    // ALWAYS REGISTERED too, for the same reason as "world" above; mcpLevelAbi says why itself when
    // there is no scene to edit.
    mcp_.registerAbi("level", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
        return mcpLevelAbi(a, r, w);
    });
}

// ---- THE "level" ABI ------------------------------------------------------------------------------
// One entry point per thing a person does to a level in the editor, each going through the code the UI
// goes through (the comment on each names it), so a scripted edit is undoable, dirties the level and
// saves exactly as a click does. Ids are the numeric scene::Entity, the same number the Outliner's
// entity ids and the `graph` ABI use.

// An entity id from a request, checked to be something the open level owns. levelEntities_ only, plus
// the Player Start marker the level keeps apart: streamed scatter, class instances and the drone are
// not written by a save, so an edit to one would silently vanish with the session. The marker is let
// through HERE for every caller; mcpLevelAbi narrows it to the ops that mean something for it.
bool SandboxApp::mcpLevelEntity(f64 id, AvId& out, std::string& why) const {
#if AVER_MODULE_SCENE
    if (!(id >= 1.0 && id <= 4294967295.0) || id != std::floor(id)) {
        why = "entity ids are whole numbers >= 1 (got " + jnum(id) + ")";
        return false;
    }
    const AvId e = static_cast<AvId>(id);
    const scene::World& wd = scene::World::instance();
    if (!wd.valid(e) || wd.destroyPending(e)) { why = "no live entity with id " + jid(e); return false; }
    if (e != playerStart_ && !isLevelOwned(e)) {
        why = "entity " + jid(e) + " is not part of the open level (streamed, spawned by a class, or transient)";
        return false;
    }
    out = e;
    return true;
#else
    (void)id; (void)out;
    why = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- there is no level to edit";
    return false;
#endif
}

// Moves one entity to a WORLD transform as one undoable command, by the gizmo's own recipe (select it,
// beginTransformEdit, write, endTransformEdit -- see the Details panel and snapSelectionToFloor). The
// selection the caller had is put back afterwards: a scripted move is not a click on the entity.
bool SandboxApp::mcpLevelMove(AvId e, const EditXform& x, std::string& why) {
#if AVER_MODULE_SCENE
    // A person's drag (gizmo, a Details field, a held nudge) keeps editBefore_ open across frames. Borrowing
    // it here would overwrite that before-state and close the gesture, so their real end would find nothing
    // to record and the drag's undo entry would be lost.
    if (editBeforeValid_ || dragging_) {
        why = "a drag is in flight in the editor (gizmo, Details field or nudge) -- let go of it and retry";
        return false;
    }
    // A snapped placement's file position is its authored ground offset, written back VERBATIM by saveLevel
    // (the same rule the gizmo lives with), so a move reported here would be silently undone on save.
    if (entitySnapZ_.find(static_cast<u32>(e)) != entitySnapZ_.end()) {
        why = "entity " + jid(e) + " is a snap-to-ground placement: a save writes back its authored offset "
              "above the ground, not the height it is moved to, so the move would not survive -- remove it "
              "and place it again with the offset you want";
        return false;
    }
    const int prevSel = sel_;
    const AvId prevEnt = selEntity_;
    const std::vector<scene::Entity> prevMulti = multiSel_;
    multiSetSingle(e);
    const bool ok = beginTransformEdit();
    if (ok) {
        setSelectedXform(x);
        endTransformEdit();
    } else {
        why = "entity " + jid(e) + " has no transform to edit";
    }
    sel_ = prevSel;
    selEntity_ = prevEnt;
    multiSel_ = prevMulti;
    multiRebuildSet();                   // multiSet_ must mirror multiSel_ (see its declaration)
    return ok;
#else
    (void)e; (void)x;
    why = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- there is no level to edit";
    return false;
#endif
}

// SAVE, and SAVE AS when `targetIn` is given. Plain save is saveLevelInteractive's write without its
// prompt. Save As is drawSaveLevelAsPrompt's own sequence (SandboxShell.cpp) -- name the copy before
// the write, zero the stored ID so the writer derives it from the new name, put both back on failure --
// because that lives inside the ImGui modal and cannot be called from here.
//
// `targetIn` is a bare name (saved as <project>/Content/Maps/<name>.ocworld, the prompt's rule) or a
// path, absolute or relative to the Content directory.
bool SandboxApp::mcpLevelSave(const std::string& targetIn, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
    namespace fs = std::filesystem;
    const std::string target = trimmed(targetIn);

    if (target.empty()) {
        if (levelPath_.empty()) {
            w = "this level has never been saved -- pass a name (saved as Maps/<name>.ocworld) or a path to save it as";
            return false;
        }
        const std::string name = fs::path(levelPath_).filename().string();
        if (!saveLevel(levelPath_)) {
            setUpgradeStatus("Could not save " + name, editor::NotifySeverity::Error);
            w = "could not write " + levelPath_ + " -- see the editor log";
            return false;
        }
        setUpgradeStatus("Saved " + name);
        r = "{\"path\":" + jq(levelPath_) + ",\"saved\":true,\"savedAs\":false}";
        return true;
    }

    constexpr const char* kBadChars = "\\/:*?\"<>|";
    fs::path dest(target);
    if (target.find_first_of("\\/") == std::string::npos && !dest.has_extension()) {
        if (target.find_first_of(kBadChars) != std::string::npos) {
            w = "a level name cannot contain any of \\ / : * ? \" < > |";
            return false;
        }
        if (!project_.valid()) {
            w = "a bare name saves into <project>/Content/Maps and no project is open -- pass a full path";
            return false;
        }
        dest = fs::path(project_.contentDir()) / "Maps" / (target + ".ocworld");
    } else {
        if (!dest.is_absolute()) {
            if (!project_.valid()) {
                w = "a relative path is resolved against the project's Content directory and no project is open";
                return false;
            }
            dest = fs::path(project_.contentDir()) / dest;
        }
        const std::string ext = lowered(dest.extension().string());
        if (ext.empty()) dest += ".ocworld";
        else if (ext != ".ocworld" && ext != ".ocmap") {
            w = "a level file is .ocworld or .ocmap (got '" + dest.extension().string() + "')";
            return false;
        }
    }
    const std::string stem = dest.stem().string();
    if (stem.empty() || stem.find_first_of(kBadChars) != std::string::npos) {
        w = "'" + stem + "' is not a usable level name";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    const bool renamed = stem != levelName_;
    const std::string prevName = levelName_;
    const u64 prevId = levelHeader_.contentId, prevLegacyId = legacyMapHeader_.contentId;
    if (renamed) {
        levelName_ = stem;
        levelHeader_.contentId = 0;
        legacyMapHeader_.contentId = 0;
    }
    if (!saveLevel(dest.string())) {
        if (renamed) {
            levelName_ = prevName;
            levelHeader_.contentId = prevId;
            legacyMapHeader_.contentId = prevLegacyId;
        }
        setUpgradeStatus("Could not save " + dest.filename().string(), editor::NotifySeverity::Error);
        w = "could not write " + dest.string() + " -- see the editor log";
        return false;
    }
    levelPath_ = dest.string();
#if AVER_WITH_IMGUI
    cbInvalidate(dest.parent_path().string());
#endif
    setUpgradeStatus("Saved " + dest.filename().string());
    r = "{\"path\":" + jq(levelPath_) + ",\"saved\":true,\"savedAs\":true,\"name\":" + jq(levelName_) + "}";
    return true;
#else
    (void)targetIn; (void)r;
    w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- there is no level to save";
    return false;
#endif
}

// PLACE: one or more .ocworld PLACE/PLACEG lines (CHILD lines inside BEGIN/END too) become entities.
//
// The text goes through the level file's own parser and world::instantiate, the same two calls a level
// open makes, so every token means what it means in a file (scale, material, nocollide, hidden, snap,
// name, anim, vehicle...). The bookkeeping after it mirrors onLevelInstantiated (SandboxLevelLoad.cpp) for
// the entities added instead of replaced: label, collide flag, snap offset, authored animation, vehicle
// preset and body.
// One Create undo entry per entity -- the undo stack has no compound command (see deleteSelection).
bool SandboxApp::mcpLevelPlace(const std::string& text, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
    namespace fs = std::filesystem;

    fmt::OcWorldData doc;
    std::string perr;
    if (!fmt::parseOcworld("OCWORLD 1\n" + text + "\n", doc, &perr)) {
        w = "the placement lines do not parse: " + perr;
        return false;
    }
    const PlaceLines scan = scanPlaceLines(text);
    if (doc.placements.empty()) {
        w = "text holds no PLACE, PLACEG or CHILD line, e.g. \"PLACE Meshes/cube.ocmesh 0 0 0  0 0 0  100\"";
        if (!scan.ignored.empty())
            w += " (ignored line " + std::to_string(scan.ignored[0].first) + ": '" + scan.ignored[0].second + "')";
        return false;
    }

    // The parser reads "nan" and "inf" as numbers, and the finite-argument rule at the top of the ABI only
    // sees `args`, so text that names one would reach physics, the renderer and the saved file. A value
    // past f32 is refused too: every consumer narrows to f32, and that narrowing is where it turns infinite.
    const auto sane = [](f64 v) {
        return std::isfinite(v) && std::fabs(v) <= static_cast<f64>(std::numeric_limits<f32>::max());
    };
    const std::string contentDir = project_.valid() ? project_.contentDir() : std::string();
    for (fmt::OcWorldPlacement& p : doc.placements) {
        if (!sane(p.x) || !sane(p.y) || !sane(p.z) || !sane(p.yaw) || !sane(p.pitch) || !sane(p.roll) ||
            !sane(p.sx) || !sane(p.sy) || !sane(p.sz) || !sane(p.animSpeed) || !sane(p.animTime)) {
            w = "placement '" + p.asset + "' has a NaN, infinite or out-of-range number (position, rotation, "
                "scale or animation value) -- nothing was placed";
            return false;
        }
        if (!p.className.empty()) {
            // saveLevel writes a class instance back by pairing it with the level file's own class
            // record (level_.classPlacements()), which a placement made here does not have.
            w = "placement '" + p.asset + "' names class '" + p.className + "': a class placement cannot be "
                "added through place, because a save pairs class instances with the level file's own "
                "records and would lose it -- put the line in the .ocworld and open that";
            return false;
        }
        if (p.asset.empty()) { w = "a placement line has no asset"; return false; }
        if (!p.animClip.empty()) {
            // saveLevelAsOcmap has no record for an animation: the entity would play in this session and
            // be gone from the file. (An .ocworld level is the only kind that stores one.)
            if (levelIsLegacyOcmap_) {
                w = "placement '" + p.asset + "' has an anim clip, but the open level is a legacy .ocmap, whose "
                    "save cannot store an animation -- it would be lost on save";
                return false;
            }
            if (!checkObjectClip(content_, contentDir, p.animClip, w)) return false;
        }
    }

    // Every mesh must already be resident, or the entity would exist and draw nothing. One imported
    // moments ago is loaded the way a Content Browser drop loads it; a name with no file behind it is
    // refused without reloading the whole project for a typo.
    const auto unloaded = [&]() {
        std::vector<std::string> out;
        std::unordered_set<u64> seen;
        for (const fmt::OcWorldPlacement& p : doc.placements)
            if (content_.meshFor(p.objectId) == 0 && seen.insert(p.objectId).second) out.push_back(p.asset);
        return out;
    };
    std::vector<std::string> missing = unloaded();
    if (!missing.empty() && engineForSplash_) {
        std::error_code ec;
        bool onDisk = false;
        for (const std::string& m : missing)
            if (!contentDir.empty() && fs::exists(fs::path(contentDir) / m, ec)) onDisk = true;
        if (onDisk) {
            releaseProjectMeshes(*engineForSplash_);
            loadProjectMeshes(*engineForSplash_);
            missing = unloaded();
        }
    }
    if (!missing.empty()) {
        std::string list;
        for (usize i = 0; i < missing.size() && i < 8; ++i) list += (i ? ", " : "") + missing[i];
        w = "mesh asset(s) not loaded: " + list + (missing.size() > 8 ? ", ..." : "") +
            " -- use the content-relative path with forward slashes and the .ocmesh extension "
            "(e.g. Meshes/cube.ocmesh); the file must be under the project's Content directory";
        return false;
    }

    // The options GameLevel::load hands world::instantiate, rebuilt here because that class keeps
    // them private: a `snap` placement asks the landscape, a body is fitted from the mesh's own
    // triangles then bounds, and a surface named by the placement or its mesh gets its .ocmat bound.
    world::InstantiateOptions opt;
#if AVER_MODULE_LANDSCAPE
    opt.groundHeightAt = [this](f64 x, f64 y, f64& outZ) { return landscape_.groundHeightAt(x, y, outZ); };
#endif
    opt.localBoundsFor = [this](u64 meshId, Vec3& outLocalMin, Vec3& outLocalMax) {
        const std::pair<Vec3, Vec3>* b = content_.boundsFor(meshId);
        if (!b) return false;
        outLocalMin = b->first;
        outLocalMax = b->second;
        return true;
    };
    opt.localTrianglesFor = [this](u64 meshId, const f32*& outPositions, u32& outVertexCount,
                                   const u32*& outIndices, u32& outIndexCount) {
        const game::GameContent::CollisionMesh* cm = content_.collisionMeshFor(meshId);
        if (!cm || cm->indices.size() < 3) return false;
        outPositions = cm->positions.data();
        outVertexCount = static_cast<u32>(cm->positions.size() / 3);
        outIndices = cm->indices.data();
        outIndexCount = static_cast<u32>(cm->indices.size());
        return true;
    };
#if AVER_MODULE_PBR
    opt.bindMaterial = [this](i32 token, const std::string& surface) {
        const pbr::MaterialHandle h = content_.materialForSurface(surface);
        if (h) content_.bindSurfaceMaterial(token, h);
    };
    std::unordered_set<u64> meshMaterialsBound;
    for (const fmt::OcWorldPlacement& p : doc.placements) {
        if (!meshMaterialsBound.insert(p.objectId).second) continue;
        const i32 slot0Token = content_.meshDefaultMaterial(p.objectId);
        if (slot0Token) {
            const std::string& name = content_.meshSlot0Name(p.objectId);
            if (!name.empty()) {
                const pbr::MaterialHandle h = content_.materialForSurface(name);
                if (h) content_.bindSurfaceMaterial(slot0Token, h);
            }
        }
        if (const std::vector<game::GameContent::MeshPart>* parts = content_.partsFor(p.objectId)) {
            for (const game::GameContent::MeshPart& part : *parts) {
                if (!part.material) continue;
                const char* partName = aver_scene_material_name(part.material);
                if (!partName || !*partName) continue;
                const pbr::MaterialHandle h = content_.materialForSurface(partName);
                if (h) content_.bindSurfaceMaterial(part.material, h);
            }
        }
    }
#endif

    const world::LevelInstance inst = world::instantiate(doc, opt);
    if (inst.entities.empty()) {
        w = "the world created no entity for any of the " + std::to_string(doc.placements.size()) +
            " placement(s) -- see the editor log";
        return false;
    }
#if AVER_MODULE_PHYSICS
    levelBodies_.insert(levelBodies_.end(), inst.bodies.begin(), inst.bodies.end());
#endif

    std::vector<u32> idFor(doc.placements.size(), 0u);
    u32 animated = 0;
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const scene::Entity e = inst.entities[k];
        const fmt::OcWorldPlacement& p = doc.placements[inst.placementIndex[k]];
        idFor[inst.placementIndex[k]] = static_cast<u32>(e);
        levelEntities_.push_back(e);
        // Called for a named placement too, and its result thrown away: the ordinal it hands back
        // advances labelCounts_, which saveLevel's shadow copy of the same arithmetic relies on.
        const std::string generatedLabel = makeEntityLabel(p.material, p.asset);
        entityLabels_[static_cast<u32>(e)] = p.name.empty() ? generatedLabel : p.name;
        entityCollide_[static_cast<u32>(e)] = p.collide;
        if (!p.animClip.empty()) {
            entityAnim_[static_cast<u32>(e)] = EntityAnim{p.animClip, p.animSpeed, p.animTime, p.animOnce};
            ++animated;
        }
        // A `vehicle` PLACEMENT: world::instantiate gave it no body (Play builds the chassis), so level_ has
        // to know it is a car -- or it would be a collider-less prop in Play and lose its token on save.
        if (!p.vehiclePreset.empty()) level_.setVehiclePreset(e, p.vehiclePreset);
        if (p.snapToGround) entitySnapZ_[static_cast<u32>(e)] = p.z;
#if AVER_MODULE_PHYSICS
        if (inst.entityBody[k] >= 0) entityBodies_[static_cast<u32>(e)] = inst.entityBody[k];
#endif
    }

    // Only the last kUndoDepth entities get an entry: pushEdit drops the oldest past that anyway, and
    // a snapshot built to be dropped at once is wasted work on a big batch.
    const usize n = inst.entities.size();
    const usize firstUndo = n > kUndoDepth ? n - kUndoDepth : 0;
    for (usize k = firstUndo; k < n; ++k) {
        EditCmd c = describeEntity(inst.entities[k]);
        c.kind = EditCmd::Kind::Create;
        pushEdit(std::move(c));
    }

    std::string ids = "[";
    for (usize i = 0; i < idFor.size(); ++i) {
        if (i) ids += ",";
        ids += std::to_string(idFor[i]);
    }
    ids += "]";
    // requested = the record lines the text held; parsed = the placements the parser read from them; placed
    // = the entities made. A misspelt record is requested but never parsed, and is listed rather than dropped.
    r = "{\"ids\":" + ids + ",\"placed\":" + std::to_string(n) + ",\"requested\":" +
        std::to_string(scan.records) + ",\"parsed\":" + std::to_string(doc.placements.size()) +
        ",\"ignored\":" + std::to_string(scan.ignored.size()) + ",\"ignoredLines\":" + ignoredJson(scan) +
        ",\"animated\":" + std::to_string(animated) +
        ",\"undoEntries\":" + std::to_string(n - firstUndo) + "}";
    AVER_INFO("[Mcp] level::place put {} of {} placement(s) into the level", n, doc.placements.size());
    if (!scan.ignored.empty())
        AVER_WARN("[Mcp] level::place ignored {} line(s) that are not placements (first: line {}, '{}')",
                  scan.ignored.size(), scan.ignored[0].first, scan.ignored[0].second);
    return true;
#else
    (void)text; (void)r;
    w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- there is no level to place into";
    return false;
#endif
}

// The "level" ABI's dispatcher. Reads (info, list) answer any time; everything else refuses while a
// session plays, with no level open, or while an open is pending, because each of those makes the edit
// land somewhere the author did not mean.
bool SandboxApp::mcpLevelAbi(const mcp::AbiCall& a, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
    namespace fs = std::filesystem;
    scene::World& wd = scene::World::instance();
    const std::string& fn = a.fn;
#if AVER_MODULE_FRAMEWORK
    const bool playing = anyPlayActive();
#else
    const bool playing = false;
#endif

    const bool known = fn == "open" || fn == "info" || fn == "list" || fn == "place" ||
                       fn == "set_transform" || fn == "set_material" || fn == "set_collide" ||
                       fn == "set_visible" || fn == "set_anim" || fn == "remove" || fn == "select" ||
                       fn == "save" || fn == "player_start";
    if (!known) {
        w = "level has no entry point '" + fn + "' (it has: open, info, list, place, set_transform, "
            "set_material, set_collide, set_visible, set_anim, remove, select, save, player_start)";
        return false;
    }
    for (const f64 v : a.args)
        if (!std::isfinite(v)) { w = "an argument is NaN or infinite"; return false; }

    // ---- info: the state every other refusal is about ---------------------------------------------
    if (fn == "info") {
        std::string j = "{\"level\":" + jq(levelPath_) + ",\"name\":" + jq(levelName_) +
                        ",\"entities\":" + std::to_string(levelEntities_.size()) +
                        ",\"dirty\":" + jb(levelHasUnsavedEdits()) +
                        ",\"format\":" + jq(levelIsLegacyOcmap_ ? "ocmap" : "ocworld") +
                        ",\"playing\":" + jb(playing) +
                        ",\"loading\":" + jb(projectLoading_ != nullptr) +
                        ",\"pendingOpen\":" + jq(pendingOpenPath_) +
                        ",\"pendingOpenPrompt\":" + jb(pendingOpenPrompt_) +
                        ",\"content\":" + jq(project_.valid() ? project_.contentDir() : std::string());
        Vec3 sp{};
        f32 sy = 0.0f;
        if (playerStartTransform(sp, sy)) {
            const bool marker = playerStart_ != scene::kInvalidEntity && wd.valid(playerStart_);
            j += ",\"playerStart\":{\"id\":" + jid(marker ? playerStart_ : 0u) + ",\"pos\":" +
                 jv3(sp.x, sp.y, sp.z) + ",\"yaw\":" + jnum(sy) + "}";
        } else {
            j += ",\"playerStart\":null";
        }
        j += ",\"camera\":{\"pos\":" + jv3(camPos_.x, camPos_.y, camPos_.z) + ",\"yaw\":" +
             jnum(degrees(yaw_)) + ",\"pitch\":" + jnum(degrees(pitch_)) + "},\"selection\":[";
        bool firstSel = true;
        for (const scene::Entity e : selectedEntities()) {
            if (!firstSel) j += ",";
            firstSel = false;
            j += jid(e);
        }
        j += "]}";
        r = j;
        return true;
    }

    if (playing && fn != "list") {
        w = "the editor is playing -- stop Play first: a level cannot be edited, opened or saved while a session runs";
        return false;
    }

    // ---- open: queued through the editor's own pending-open funnel ------------------------------
    if (fn == "open") {
        const std::string want = trimmed(a.text);
        if (want.empty()) {
            w = "open needs the level path in text (relative to the project's Content directory, e.g. "
                "Maps/Arena.ocworld, or absolute)";
            return false;
        }
        std::error_code ec;
        std::vector<fs::path> tries;
        const fs::path given(want);
        if (given.is_absolute() || !project_.valid()) {
            tries.push_back(given);
        } else {
            const fs::path content(project_.contentDir());
            tries.push_back(content / given);
            tries.push_back(content / "Maps" / given);
        }
        fs::path found;
        for (const fs::path& t : tries) {
            std::vector<fs::path> variants{t};
            if (!t.has_extension()) { variants.push_back(fs::path(t).concat(".ocworld")); variants.push_back(fs::path(t).concat(".ocmap")); }
            for (const fs::path& v : variants)
                if (fs::is_regular_file(v, ec)) { found = v; break; }
            if (!found.empty()) break;
        }
        if (found.empty()) {
            w = "no level file at '" + want + "' (looked at";
            for (const fs::path& t : tries) w += " " + t.string();
            w += ")";
            return false;
        }
        const std::string path = fs::absolute(found, ec).string();
        // The editor asks before discarding edits; nothing here can answer that prompt, so the caller
        // says so up front (args[0] = 1) or the open is refused.
        const bool discard = !a.args.empty() && a.args[0] != 0.0;
        if (levelHasUnsavedEdits() && !discard) {
            w = "the open level has unsaved edits -- save first (op save), or pass args[0] = 1 to discard them";
            return false;
        }
        if (!requestOpenLevel(path, "opened over MCP")) {
            w = openLevelError_.empty() ? std::string("that level could not be opened") : openLevelError_;
            return false;
        }
        if (discard && levelHasUnsavedEdits()) {
            AVER_WARN("[Mcp] level::open discards the unsaved edits to '{}'", levelPath_);
            markLevelSaved();   // else applyPendingOpen stops at its unsaved-changes prompt
        }
        r = "{\"queued\":true,\"path\":" + jq(path) + "}";
        return true;
    }

    if (levelPath_.empty() && fn != "save") {
        w = "no level is open -- open one first (op open)";
        return false;
    }

    // ---- list: what is in the level ----------------------------------------------------------------
    if (fn == "list") {
        const std::string needle = lowered(a.text);
        usize maxRows = 500;
        if (!a.args.empty() && a.args[0] >= 1.0) maxRows = static_cast<usize>(std::fmin(a.args[0], 1000000.0));
        usize total = 0, shown = 0;
        std::string rows;
        for (const scene::Entity e : levelEntities_) {
            if (!wd.valid(e)) continue;
            if (!wd.component<scene::CLocal>(e, scene::kComponentLocal)) continue;
            const std::string asset = std::string(wd.name(e));
            const auto labelIt = entityLabels_.find(static_cast<u32>(e));
            const std::string label = labelIt == entityLabels_.end() ? std::string() : labelIt->second;
            if (!needle.empty() && lowered(asset).find(needle) == std::string::npos &&
                lowered(label).find(needle) == std::string::npos) continue;
            ++total;
            if (shown >= maxRows) continue;
            ++shown;
            const Transform t = worldTransformOf(wd, e);
            const Vec3 eul = eulerDegFromQuat(t.rotation);   // (roll, pitch, yaw)
            const auto* mr = wd.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
            const char* matName = (mr && mr->material) ? aver_scene_material_name(mr->material) : "";
            const auto collideIt = entityCollide_.find(static_cast<u32>(e));
            const bool collide = collideIt == entityCollide_.end() ? true : collideIt->second;
            if (!rows.empty()) rows += ",";
            rows += "{\"id\":" + jid(e) + ",\"asset\":" + jq(asset) + ",\"label\":" + jq(label) +
                    ",\"pos\":" + jv3(t.position.x, t.position.y, t.position.z) +
                    ",\"rot\":" + jv3(eul.z, eul.y, eul.x) +
                    ",\"scale\":" + jv3(t.scale.x, t.scale.y, t.scale.z) +
                    ",\"material\":" + jq(matName ? matName : "") +
                    ",\"collide\":" + jb(collide) + ",\"visible\":" + jb(authoredVisible(e)) +
                    ",\"parent\":" + jid(wd.parent(e)) + ",\"anim\":" + jAnim(entityAnim(e)) + "}";
        }
        r = "{\"total\":" + std::to_string(total) + ",\"returned\":" + std::to_string(shown) +
            ",\"entities\":[" + rows + "]}";
        return true;
    }

    if (!pendingOpenPath_.empty() || pendingOpenPrompt_) {
        w = "a level open is pending (info reports it) -- wait for it, or the edit would land on the level being replaced";
        return false;
    }
    if (fn == "save") return mcpLevelSave(a.text, r, w);
    if (projectLoading_) {
        w = "the level is still loading -- retry in a moment (info reports loading)";
        return false;
    }

    // The read-back the transform ops reply with.
    const auto xformJson = [&](AvId e) {
        const Transform t = worldTransformOf(wd, e);
        const Vec3 eul = eulerDegFromQuat(t.rotation);
        return "{\"id\":" + jid(e) + ",\"pos\":" + jv3(t.position.x, t.position.y, t.position.z) +
               ",\"rot\":" + jv3(eul.z, eul.y, eul.x) + ",\"scale\":" + jv3(t.scale.x, t.scale.y, t.scale.z) + "}";
    };
    // An entity id for this op. The Player Start marker passes mcpLevelEntity, but it is not a placement --
    // no save reads its material, visibility, collision or animation -- so only the ops that move, select
    // or delete it may name it; the rest would make an edit that silently vanishes.
    const bool markerOp = fn == "set_transform" || fn == "select" || fn == "remove";
    const auto levelEntityFor = [&](f64 id, AvId& out) {
        if (!mcpLevelEntity(id, out, w)) return false;
        if (out == playerStart_ && !markerOp) {
            w = fn + " does not apply to the Player Start marker (entity " + jid(out) + "): it is not a "
                "placement, and a save never writes that state -- only set_transform, select, remove and "
                "player_start take it";
            return false;
        }
        return true;
    };
    // ids from args[from..], each checked, duplicates dropped.
    const auto idList = [&](usize from, std::vector<scene::Entity>& out) {
        for (usize i = from; i < a.args.size(); ++i) {
            AvId e = scene::kInvalidEntity;
            if (!levelEntityFor(a.args[i], e)) return false;
            if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
        }
        return true;
    };
    // The same selection the outliner's multi paths build: the first id single (the anchor), the rest toggled in.
    const auto selectIds = [this](const std::vector<scene::Entity>& ids) {
        multiClear();
        sel_ = -1;
        selEntity_ = scene::kInvalidEntity;
        if (ids.empty()) return;
        multiSetSingle(ids[0]);
        for (usize i = 1; i < ids.size(); ++i) multiToggle(ids[i]);
    };

    if (fn == "place") return mcpLevelPlace(a.text, r, w);

    // ---- set_transform: the gizmo's undoable edit --------------------------------------------------
    if (fn == "set_transform") {
        if (a.args.size() != 7 && a.args.size() != 10) {
            w = "set_transform needs args: id, x, y, z, yaw, pitch, roll [, sx, sy, sz] (world space, cm and degrees)";
            return false;
        }
        AvId e = scene::kInvalidEntity;
        if (!levelEntityFor(a.args[0], e)) return false;
        EditXform x;
        x.pos = Vec3{static_cast<f32>(a.args[1]), static_cast<f32>(a.args[2]), static_cast<f32>(a.args[3])};
        x.rotDeg = Vec3{static_cast<f32>(a.args[6]), static_cast<f32>(a.args[5]), static_cast<f32>(a.args[4])};   // (roll, pitch, yaw)
        x.scale = a.args.size() == 10
                ? Vec3{static_cast<f32>(a.args[7]), static_cast<f32>(a.args[8]), static_cast<f32>(a.args[9])}
                : worldTransformOf(wd, e).scale;
        if (!mcpLevelMove(e, x, w)) return false;
        // The marker's heading is what the save and playerStartTransform read, not its entity's rotation
        // (player_start does the same after its own move), so a yaw given here would otherwise not stick.
        if (e == playerStart_) playerStartYaw_ = x.rotDeg.z;
        r = xformJson(e);
        return true;
    }

    // ---- set_material: the Details panel's material picker (not undoable there either) -----------
    if (fn == "set_material") {
#if AVER_MODULE_PBR && AVER_WITH_IMGUI
        const std::string name = trimmed(a.text);
        if (a.args.size() != 1 || name.empty()) {
            w = "set_material needs args: id, and the material name in text (an .ocmat stem, e.g. M_Wood)";
            return false;
        }
        AvId e = scene::kInvalidEntity;
        if (!levelEntityFor(a.args[0], e)) return false;
        if (!wd.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
            w = "entity " + jid(e) + " has no mesh renderer, so it has no material to set";
            return false;
        }
        if (!content_.materialForSurface(name)) {
            w = "no material named '" + name + "' under Content/Materials or Binaries/Materials";
            return false;
        }
        if (!assignMaterialToken(e, aver_scene_material(0, name.c_str()))) {
            w = "the material could not be assigned to entity " + jid(e);
            return false;
        }
        r = "{\"id\":" + jid(e) + ",\"material\":" + jq(name) + "}";
        return true;
#else
        w = "this build has no material system or editor UI, which set_material goes through";
        return false;
#endif
    }

    // ---- set_visible / set_collide: the Details panel's two checkboxes, one undo entry each ------
    if (fn == "set_visible" || fn == "set_collide") {
        if (a.args.size() != 2) { w = fn + " needs args: id, 0|1"; return false; }
        AvId e = scene::kInvalidEntity;
        if (!levelEntityFor(a.args[0], e)) return false;
        if (!wd.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
            w = "entity " + jid(e) + " has no mesh renderer, so it has nothing to show, hide or collide";
            return false;
        }
        const bool want = a.args[1] != 0.0;
        bool changed = false;
        if (fn == "set_visible") {
            const bool before = authoredVisible(e);
            if (before != want) {
                setAuthoredVisible(e, want);
                EditCmd c;
                c.kind = EditCmd::Kind::Visibility;
                c.visibility.push_back({editIdFor(e), before, want});
                pushEdit(std::move(c));
                changed = true;
            }
            r = "{\"id\":" + jid(e) + ",\"visible\":" + jb(authoredVisible(e)) + ",\"changed\":" + jb(changed) + "}";
        } else {
            const auto it = entityCollide_.find(static_cast<u32>(e));
            const bool before = it == entityCollide_.end() ? true : it->second;   // absent = the default
            if (before != want) {
                setEntityCollide(e, want);
                EditCmd c;
                c.kind = EditCmd::Kind::Collision;
                c.collide.push_back({editIdFor(e), before, want});
                pushEdit(std::move(c));
                changed = true;
            }
            r = "{\"id\":" + jid(e) + ",\"collide\":" + jb(want) + ",\"changed\":" + jb(changed) + "}";
        }
        return true;
    }

    // ---- set_anim: the Details panel's Animation section (applyEntityAnim + a Kind::Animation entry) ----
    if (fn == "set_anim") {
        if (a.args.empty() || a.args.size() > 4) {
            w = "set_anim needs args: id [, speed [, time [, once]]] and the clip in text (empty text clears it)";
            return false;
        }
        AvId e = scene::kInvalidEntity;
        if (!levelEntityFor(a.args[0], e)) return false;
        if (wd.hasComponent(e, scene::kComponentSkeletalMesh)) {
            w = "entity " + jid(e) + " is a skeletal mesh: its animator is a skeletal clock, not an object animation";
            return false;
        }
        EntityAnim after;
        after.clip = trimmed(a.text);
        if (!after.clip.empty()) {
            // The Details panel's message for the same case: the legacy save has no animation record.
            if (levelIsLegacyOcmap_) {
                w = "the open level is a legacy .ocmap, whose save cannot store an animation -- it would be "
                    "lost on save";
                return false;
            }
            // f32 is what the animator holds; a finite f64 past its range turns infinite on the narrowing.
            for (usize i = 1; i < a.args.size() && i < 3; ++i)
                if (std::fabs(a.args[i]) > static_cast<f64>(std::numeric_limits<f32>::max())) {
                    w = "set_anim's speed and time must fit a 32-bit float (got " + jnum(a.args[i]) + ")";
                    return false;
                }
            after.speed = a.args.size() > 1 ? static_cast<f32>(a.args[1]) : 1.0f;
            after.time  = a.args.size() > 2 ? static_cast<f32>(a.args[2]) : 0.0f;
            after.once  = a.args.size() > 3 && a.args[3] != 0.0;
            if (!checkObjectClip(content_, project_.valid() ? project_.contentDir() : std::string(), after.clip, w))
                return false;
        }
        const EntityAnim* cur = entityAnim(e);
        const EntityAnim before = cur ? *cur : EntityAnim{};
        const bool changed = !(before == after);
        if (changed) {
            applyEntityAnim(e, after);
            EditCmd c;
            c.kind = EditCmd::Kind::Animation;
            c.animation.push_back({editIdFor(e), before, after});
            pushEdit(std::move(c));
        }
        r = "{\"id\":" + jid(e) + ",\"anim\":" + jAnim(entityAnim(e)) + ",\"changed\":" + jb(changed) + "}";
        return true;
    }

    // ---- remove: the selection's Delete, one undo entry per entity (deleteSelection) ---------------
    if (fn == "remove") {
#if AVER_WITH_IMGUI
        if (a.args.empty()) { w = "remove needs args: one or more entity ids"; return false; }
        std::vector<scene::Entity> ids;
        if (!idList(0, ids)) return false;
        selectIds(ids);
        deleteSelection();
        usize gone = 0;
        for (const scene::Entity e : ids)
            if (!wd.valid(e) || wd.destroyPending(e)) ++gone;
        r = "{\"removed\":" + std::to_string(gone) + ",\"requested\":" + std::to_string(ids.size()) + "}";
        return true;
#else
        w = "this build has no editor UI, which remove goes through (deleteSelection)";
        return false;
#endif
    }

    // ---- select: the outliner's selection, then F's framing ----------------------------------------
    if (fn == "select") {
        std::vector<scene::Entity> ids;
        if (!idList(0, ids)) return false;
        selectIds(ids);
        bool framed = false;
        if (!ids.empty() && trimmed(a.text) != "noframe") {
            Vec3 center{};
            f32 rad = 0.0f;
            if (selectionBounds(center, rad)) {
                // F's own formula (onUpdate): stand back far enough that the whole selection's sphere fits.
                const f32 d = std::fmax(50.0f, rad / std::tan(radians(30.0f)) * 1.6f);
                camPos_ = center - camForward() * d;
                flySpeed_ = std::fmax(flySpeed_, rad * 0.4f);
                streaming_.resetVelocityTracking();   // a teleport, as frameCameraOnLevel says
                framed = true;
            }
        }
        std::string list = "[";
        for (usize i = 0; i < ids.size(); ++i) list += (i ? "," : "") + jid(ids[i]);
        list += "]";
        r = "{\"selected\":" + list + ",\"framed\":" + jb(framed) + "}";
        return true;
    }

    // ---- player_start: Add > Player Start, at a given place ---------------------------------------
    if (fn == "player_start") {
        if (a.args.size() != 4) { w = "player_start needs args: x, y, z, yaw (cm, degrees)"; return false; }
        const Vec3 at{static_cast<f32>(a.args[0]), static_cast<f32>(a.args[1]), static_cast<f32>(a.args[2])};
        const f32 yaw = static_cast<f32>(a.args[3]);
        bool created = false;
        if (playerStart_ != scene::kInvalidEntity && wd.valid(playerStart_)) {
            // One per level: an existing marker is moved, by the same undoable edit as any entity.
            EditXform x;
            x.pos = at;
            x.rotDeg = Vec3{0.0f, 0.0f, yaw};
            x.scale = worldTransformOf(wd, playerStart_).scale;
            if (!mcpLevelMove(playerStart_, x, w)) return false;
            playerStartYaw_ = yaw;   // what playerStartTransform and the save read; the entity's rotation is not
        } else {
            playerStart_ = makePlayerStart(at, yaw);
            if (playerStart_ == scene::kInvalidEntity) { w = "the world refused a new Player Start"; return false; }
            EditCmd c = describeEntity(playerStart_);
            c.kind = EditCmd::Kind::Create;
            pushEdit(std::move(c));
            created = true;
        }
        r = "{\"id\":" + jid(playerStart_) + ",\"pos\":" + jv3(at.x, at.y, at.z) + ",\"yaw\":" + jnum(yaw) +
            ",\"created\":" + jb(created) + "}";
        return true;
    }

    w = "level has no entry point '" + fn + "'";
    return false;
#else
    (void)a; (void)r;
    w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- there is no level to edit";
    return false;
#endif
}

// STARTS THE CHANNEL, WHOLE. See SandboxApp.hpp's own comment on this declaration for why the
// three steps below -- ABI registration, the widget hooks, and the listen -- moved here together
// rather than staying as onInit's private copy: a button that did only the last of those would
// bring up a channel that answers `ping` and nothing else.
bool SandboxApp::mcpStart(u16 port) {
    // REGISTERED ONCE. registerAbi replaces a module's dispatcher rather than appending to it, so
    // calling registerMcpAbis a second time would not itself be wrong -- but a stop/start cycle
    // (the status-bar widget's Stop then Start, or any future caller) running through every
    // module's registration again for no reason is exactly the kind of habit that stops being
    // harmless the day one of those registrations is not idempotent. mcpAbisRegistered_ is what
    // makes "once" true rather than "true so far".
    if (!mcpAbisRegistered_) {
        registerMcpAbis();
        mcpAbisRegistered_ = true;
    }
    // Widget hooks are UI-only: uiReg_ (the ImGui widget registry) doesn't exist with
    // AVER_ENABLE_UI=OFF, and there's nothing for MCP to resolve. Left unregistered -- MCP's
    // other ABIs still work, so a UI-less editor just reports no widgets. Installed on every call
    // rather than gated behind mcpAbisRegistered_ above: both closures are stateless captures of
    // `this`, so re-installing costs nothing, and doing it unconditionally is what keeps a
    // restarted channel's widget lookups working even if a future McpBridge::stop() ever clears
    // its own hooks.
#if AVER_WITH_IMGUI
    mcp_.setWidgetResolver([this](const std::string& n, f32& x, f32& y) {
        return uiReg_.centreOf(n, x, y);
    });
    mcp_.setWidgetLister([this] { return uiReg_.describe(); });
#endif
    if (!mcp_.start(port)) return false;
    // THE PORT THAT ACTUALLY STARTED, read back off the bridge rather than echoed from `port`:
    // today start() binds exactly what it is given or fails outright, so the two never differ, but
    // asking the bridge is what stays correct the day that stops being true.
    mcpPort_ = mcp_.port();
    return true;
}

// Stops the listener and nothing else. DOES NOT UNREGISTER ANYTHING -- there is no unregister call
// on McpBridge's surface to make even if this wanted to, and there would be nothing to gain by
// adding one: the ABI dispatchers and the widget hooks are stateless closures over `this`, so
// leaving them in place costs nothing while the socket is down, and mcpAbisRegistered_ above is
// what stops a later mcpStart from registering them a second time rather than this function having
// to undo them now.
void SandboxApp::mcpStop() {
    mcp_.stop();
}

#if AVER_WITH_IMGUI
// THE STATUS BAR'S CONTROL-CHANNEL WIDGET. Its default state is simply off -- nobody has to opt
// into a control channel, --mcp does, and most sessions never pass that flag -- which is the
// opposite of revision control's default state of "waiting for an answer". So idle is drawn as the
// ordinary case it is, and the one thing this widget insists on saying, in the tooltip and again in
// the menu, is what a live channel actually lets happen: it drives the editor with real input and
// can call any registered engine ABI, from anything on this machine that can reach the loopback
// port. A one-click Start that did not say that would be the wrong thing to ship.
void SandboxApp::drawMcpStatusWidget() {
    const bool live = mcp_.listening();
    char face[40];
    if (live) std::snprintf(face, sizeof face, ICON_LINK " MCP :%u", static_cast<unsigned>(mcp_.port()));
    else      std::snprintf(face, sizeof face, ICON_LINK " MCP");

    // NOT AN ALARM COLOUR WHEN OFF. Idle is the normal state for this widget, so painting it red or
    // amber would tell somebody something is wrong when nothing is -- it reads as live when live,
    // and otherwise no different from any other disabled label in the bar.
    const ImVec4 tint = live ? ImVec4(0.38f, 0.78f, 0.43f, 1.0f)
                              : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

    const char* tooltip =
        live ? "MCP is listening on 127.0.0.1 only.\n"
               "Anything on this machine that can open a socket to that port can drive this "
               "editor -- move the mouse, type, click, and call a registered engine ABI.\n"
               "Click to stop it."
             : "MCP: a loopback control channel (127.0.0.1 only) that drives the editor with real "
               "input events and can call registered engine ABIs.\n"
               "Off by default. Click to start it.";

    // NAMED LIKE ITS NEIGHBOUR, and tracked by statusBarWidget itself rather than here: that
    // helper already publishes the control under the id it is handed, so a uiReg_.track() at this
    // call site would put one rect in the registry twice under two different names.
    if (statusBarWidget("statusbar.mcp", face, tint, tooltip))
        ImGui::OpenPopup("mcpStatusMenu");

    if (ImGui::BeginPopup("mcpStatusMenu")) {
        if (live) {
            if (ImGui::MenuItem("Stop")) mcpStop();
            uiReg_.track("statusbar.mcp.stop");
        } else {
            // mcpPort_ carries the last port asked for, INCLUDING a port a previous attempt failed
            // to bind -- so a retry from here offers that same value back rather than silently
            // falling to the default and binding somewhere the caller did not ask for.
            const u16 wantPort = mcpPort_ ? mcpPort_ : 45123;
            if (ImGui::MenuItem("Start")) {
                if (!mcpStart(wantPort))
                    AVER_WARN("[Mcp] Start (from the status bar) did not bring the channel up; "
                              "see the editor log above for why");
            }
            uiReg_.track("statusbar.mcp.start");
        }
        ImGui::Separator();
        // GREYED AND SPELLED OUT, not just implied by the icon: this is the same honesty as the
        // tooltip, kept visible without a hover for whoever already has the menu open.
        ImGui::TextDisabled("A loopback control channel (127.0.0.1 only).");
        ImGui::TextDisabled("Drives real input and calls registered engine ABIs.");
        if (live) ImGui::TextDisabled("Listening on 127.0.0.1:%u", static_cast<unsigned>(mcp_.port()));
        else      ImGui::TextDisabled("Not listening.");
        ImGui::EndPopup();
    }
}
#endif // AVER_WITH_IMGUI

#endif

} // namespace aver
