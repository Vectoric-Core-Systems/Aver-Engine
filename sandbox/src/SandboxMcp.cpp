// Editor: the MCP control channel's commands and ABI registrations.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_MCP
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
            r = uiReg_.describe();
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
            if (!chunkWorld_) {
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
            const world::StreamStats& s = chunkStreamStats_;
            char buf[320];
            std::snprintf(buf, sizeof buf,
                "streaming=%s residentChunks=%u residentEntities=%u loadedThisUpdate=%u "
                "evictedThisUpdate=%u pendingLoads=%u failedLoads=%u totalLoads=%u "
                "lastLoadMs=%.3f totalLoadMs=%.3f",
                chunkWorld_ ? "on" : "off", s.residentChunks, s.residentEntities,
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
            chunkStreamHaveLastPos_ = false;
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
}

#endif

} // namespace aver
