#include "ActorEditor.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/ActorScript.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/runtime/Engine.hpp"

// The LIVE view's two dependencies, and the only place this tab reaches past its own source text.
// Guarded because a build without them still gets the whole parsed editor -- Live is the extra, not
// the editor.
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
#  include "aver/framework/framework_abi.h"
#  include "aver/scene/scene_abi.h"
#endif

#if AVER_WITH_IMGUI
#  include <imgui.h>
#endif

#include <cstdio>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>

namespace aver::editor {
namespace {

// ONE preview, shared by every open actor tab, and only the ACTIVE tab drives it.
//
// Not one per tab, and the reason is a hard limit rather than thrift: the UI descriptor heap holds
// sixteen slots and the editor already spends five, so a target per tab exhausts it at about eleven
// and the failure is a black image rather than an assert. A second tab shows its model list and its
// numbers; it just does not get the 3D until it is focused.
render::preview::ActorPreview* g_preview = nullptr;
render::preview::PreviewMeshCache g_meshes;
std::string g_contentRoot;
bool g_previewTried = false;
// Held so shutdown can unregister before the feature goes. A device that still holds a pointer to
// a deleted feature calls prePass on freed memory, which is not an error anything reports.
rhi::IDevice* g_device = nullptr;
ActorEditorHooks g_hooks;
bool g_liveDefault = false;

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
// ObjectId as the managed side mints it: aver::fnv1a64 over the path's bytes AS WRITTEN.
//
// core's, not a copy: FormatTest already pins fnv1a64 against Aver.Scene's ObjectIdOf by value, so
// borrowing it means the agreement stays tested. A private FNV here would be a second constant pair
// nothing checks, and it would drift silently the first time either side changed.
//
// AS WRITTEN matters and is not a detail. ObjectIdOf does not canonicalise, so a script that says
// "Content/Meshes/Car.ocmesh" and one that says "Meshes/Car.ocmesh" produce different ids for the
// same file. Reading an id back therefore cannot be undone by hashing the canonical form -- the
// table below has to hold every spelling that could have produced it.
inline u64 objectIdOf(std::string_view path) { return aver::fnv1a64(path); }

// id -> a path the mesh cache can open. Built once per content root, because a Live rebuild that
// walked the content tree every time would stat the whole project on every toggle.
//
// Three sources, and all three are needed. The BUILT-INS are not files and no walk would find them,
// yet nearly every template actor names one. The DISK WALK covers meshes the script names through a
// variable or a constant, which no amount of reading this one file would recover. The SOURCE
// LITERALS cover the reverse case -- a path written in this file for a mesh that has not been built
// yet, where the id is real and the file is not.
class MeshNameTable {
public:
    void reset(std::string root) {
        if (root == root_ && built_) return;
        root_ = std::move(root);
        byId_.clear();
        built_ = false;
    }
    void ensure() {
        if (built_) return;
        built_ = true;
        add("Meshes/cube.ocmesh");
        add("Meshes/sphere.ocmesh");
        if (root_.empty()) return;
        std::error_code ec;
        const std::filesystem::path base(root_);
        for (std::filesystem::recursive_directory_iterator it(base, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() != ".ocmesh") continue;
            const std::string rel = std::filesystem::relative(it->path(), base, ec).generic_string();
            if (ec || rel.empty()) continue;
            add(rel);
            // Both spellings, because a script may write either and they hash differently. The value
            // stored is the same file either way, so the cache resolves the same mesh.
            add("Content/" + rel);
        }
    }
    // Every string literal in the tab's own source that names a `.ocmesh`. Cheap, and it is what
    // makes a freshly written Place call resolve before anything has been cooked.
    void addLiteralsFrom(std::string_view text) {
        for (usize i = 0; i + 1 < text.size(); ++i) {
            if (text[i] != '"') continue;
            const usize end = text.find('"', i + 1);
            if (end == std::string_view::npos) break;
            const std::string_view lit = text.substr(i + 1, end - i - 1);
            if (lit.size() > 7 && lit.substr(lit.size() - 7) == ".ocmesh") add(std::string(lit));
            i = end;
        }
    }
    const std::string* find(u64 id) const {
        const auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : &it->second;
    }
private:
    void add(std::string path) { byId_.emplace(objectIdOf(path), std::move(path)); }
    std::unordered_map<u64, std::string> byId_;
    std::string root_;
    bool built_ = false;
};
MeshNameTable g_meshNames;

// The scene field ids, resolved once. aver_scene_field is a lookup by qualified name and the ids are
// stable for the process, so caching them keeps the per-entity walk to two integer reads.
struct MeshFields {
    int32_t mesh = 0, flags = 0;
    bool ok() const { return mesh != 0 && flags != 0; }
};
const MeshFields& meshFields() {
    static const MeshFields f = [] {
        MeshFields m;
        m.mesh  = aver_scene_field("CMeshRenderer.mesh");
        m.flags = aver_scene_field("CMeshRenderer.flags");
        return m;
    }();
    return f;
}
#endif

// Degrees (yaw, pitch, roll) about +Z, +Y, +X and a scale, into the engine's row-vector matrix with
// the translation in the LAST ROW. Written out rather than borrowed from the scene, because this
// module must not depend on the world to draw something that is not in it.
void composeTransform(const f32 pos[3], const f32 rotDeg[3], const f32 scale[3], f32 out[16]) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 y = rotDeg[0] * kPi / 180.0f, p = rotDeg[1] * kPi / 180.0f, r = rotDeg[2] * kPi / 180.0f;
    const f32 cy = std::cos(y), sy = std::sin(y);
    const f32 cp = std::cos(p), sp = std::sin(p);
    const f32 cr = std::cos(r), sr = std::sin(r);

    // Z (yaw) then Y (pitch) then X (roll), which is the order the framework applies them.
    const f32 m00 = cy * cp,  m01 = sy * cp,  m02 = -sp;
    const f32 m10 = cy * sp * sr - sy * cr, m11 = sy * sp * sr + cy * cr, m12 = cp * sr;
    const f32 m20 = cy * sp * cr + sy * sr, m21 = sy * sp * cr - cy * sr, m22 = cp * cr;

    out[0]  = m00 * scale[0]; out[1]  = m01 * scale[0]; out[2]  = m02 * scale[0]; out[3]  = 0.0f;
    out[4]  = m10 * scale[1]; out[5]  = m11 * scale[1]; out[6]  = m12 * scale[1]; out[7]  = 0.0f;
    out[8]  = m20 * scale[2]; out[9]  = m21 * scale[2]; out[10] = m22 * scale[2]; out[11] = 0.0f;
    out[12] = pos[0];         out[13] = pos[1];         out[14] = pos[2];         out[15] = 1.0f;
}

class ActorEditor final : public AssetEditor {
public:
    ActorEditor(std::string path, fmt::ActorScript parsed,
                std::vector<fmt::ActorClassInfo> classes, std::string source)
        : path_(std::move(path)), script_(std::move(parsed)), classes_(std::move(classes)),
          source_(std::move(source)) {
        pickFirstPreviewable();
        title_ = std::filesystem::path(path_).filename().string();
        stamp();
    }

    const std::string& path() const override { return path_; }
    std::string title() const override { return dirty_ ? title_ + " *" : title_; }
    bool dirty() const override { return dirty_; }

    void draw(Engine& e) override;

    bool save(std::string* why) override {
        if (!dirty_) return true;
        std::string out = source_;

        // CLASS DEFAULTS FIRST, then the generated region. Both rewrite the same text, and the
        // region's spans were measured against the source as read -- so doing the class edits second
        // would apply them to byte offsets the region rewrite has already moved.
        //
        // In practice a file has one or the other: a designer region is generated and its class
        // values are not usually spanned. Ordering is stated because "in practice" is not a
        // guarantee, and the failure would be an edit landing in the middle of a different line.
        for (const fmt::ActorClassInfo& k : classes_) {
            std::string next;
            if (!fmt::rewriteActorClass(out, k, next, why)) return false;
            out = std::move(next);
        }
        if (!script_.models.empty()) {
            std::string next;
            if (!fmt::rewriteActorScript(out, script_.models, next, why)) return false;
            out = std::move(next);
        }
        std::ofstream os(path_, std::ios::binary | std::ios::trunc);
        if (!os) { if (why) *why = "could not open " + path_ + " for writing"; return false; }
        os.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!os) { if (why) *why = "the write failed part way through"; return false; }
        // The in-memory source becomes what is now on disk, so a second save rewrites from the file
        // as it stands rather than from the text this tab was opened with.
        source_ = std::move(out);
        dirty_ = false;
        AVER_INFO("[ActorEditor] wrote {} placement(s) back to {}", script_.models.size(), path_);
        return true;
    }

private:
    void buildDrawList(Engine& e);

    // ---- the gizmo ----
    //
    // A TRANSLATE gizmo, three axes, drawn as an ImGui overlay over the preview image rather than as
    // geometry in the pass. Two reasons, and neither is laziness: the preview feature must stay
    // drivable with no ImGui (that is what lets a test be the device), and a 3D gizmo has to be
    // pickable at a constant SCREEN size, which means it cannot be part of a scene that scales.
    //
    // Projection goes through the preview's own camera, so what is drawn is where the handle
    // actually points. Deriving it from anything else is how a gizmo ends up offset from its object.
    bool projectToScreen(const f32 world[3], f32 imageSize, ImVec2& out) const;
    int  pickGizmoAxis(ImVec2 local, f32 imageSize) const;
    void drawGizmo(ImVec2 imageTopLeft, f32 imageSize, const fmt::ActorModel& m) const;
    void dragAlongAxis(fmt::ActorModel& m, int axis, ImVec2 delta, f32 imageSize) const;
    bool axisTip(const fmt::ActorModel& m, int axis, f32 imageSize, ImVec2& out) const;

    // -1 when not dragging. Latched on mouse-down and held for the whole gesture.
    int draggingAxis_ = -1;

    // The file's last write time when this tab last read it. A watcher would be the general answer
    // and this is the honest small one: an editor tab is polled every frame it is visible anyway, so
    // a stat is cheaper than a thread, an OS handle and an overflow case to get wrong.
    void stamp() {
        std::error_code ec;
        stamp_ = std::filesystem::last_write_time(path_, ec);
        haveStamp_ = !ec;
    }
    // Re-read the file when it has changed underneath. Returns true if the view was rebuilt.
    bool reloadIfChanged();

    std::string path_, title_, source_;
    fmt::ActorScript script_;
    // EVERY actor the file declares, not the first. A real project puts several in one file --
    // SkyForge's FpsGameMode.cs declares five -- and previewing whichever happens to be first would
    // show one class while the panel named another.
    std::vector<fmt::ActorClassInfo> classes_;
    int activeClass_ = -1;

    void pickFirstPreviewable() {
        activeClass_ = -1;
        // Something to LOOK at first, because a file that declares a GameMode above three meshed
        // actors should open on one of the meshes rather than on the rules object.
        for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
            if (classes_[static_cast<usize>(i)].drawable()) { activeClass_ = i; break; }
        if (activeClass_ < 0)
            for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
                if (classes_[static_cast<usize>(i)].anything()) { activeClass_ = i; break; }
        // Nothing previewable but classes present: still name the first, so the panel says what the
        // file HOLDS rather than looking empty.
        if (activeClass_ < 0 && !classes_.empty()) activeClass_ = 0;
    }
    fmt::ActorClassInfo* activeInfoMutable() {
        return (activeClass_ >= 0 && activeClass_ < static_cast<int>(classes_.size()))
             ? &classes_[static_cast<usize>(activeClass_)] : nullptr;
    }
    const fmt::ActorClassInfo* activeInfo() const {
        return (activeClass_ >= 0 && activeClass_ < static_cast<int>(classes_.size()))
             ? &classes_[static_cast<usize>(activeClass_)] : nullptr;
    }
    std::filesystem::file_time_type stamp_{};
    // ---- the LIVE view ----
    //
    // What the class BUILDS, as against what the file SAYS. The parsed view is always available and
    // needs nothing loaded; this one spawns the real class, lets its BuildModels run, reads the child
    // transforms back out and destroys it. The two disagree exactly when BuildModels does something
    // the parser cannot see -- a loop, a constant, a branch on a field -- which is precisely the case
    // where an author needs to look rather than guess.
    //
    // It is NOT a play session and must never become one: aver_fw_spawn_preview withholds the BEGIN
    // edge, so no OnBeginPlay runs, nothing is possessed and nothing ticks. See framework_abi.h.
    bool live_ = g_liveDefault;
    bool liveStale_ = true;             // rebuild on the next draw
    std::string liveWhy_;               // why it is off, or what the last rebuild found
    int liveUnnamed_ = 0;               // models whose mesh id resolved to no path this could open
    bool liveCapsuleFromSource_ = false;   // the one figure in the live view that the spawn did not give
    std::vector<render::preview::PreviewDraw> liveDraws_;
    void rebuildLive(Engine& e);

    bool haveStamp_ = false;
    bool dirty_ = false;
    int selected_ = -1;
    bool framed_ = false;
    std::string status_;
};

// Source-to-view live sync. A file changed outside the editor -- by an IDE, by a Compile C#, by a
// git checkout -- is re-read and the preview follows.
//
// AN UNSAVED EDIT WINS NOTHING. If this tab is dirty the reload is refused and the tab says so,
// because silently replacing somebody's in-progress drag with what a background tool wrote is the
// one behaviour a live-sync feature must never have. Saving, or closing without saving, resolves it.
bool ActorEditor::reloadIfChanged() {
    std::error_code ec;
    const std::filesystem::file_time_type now = std::filesystem::last_write_time(path_, ec);
    if (ec) return false;
    if (haveStamp_ && now == stamp_) return false;
    stamp_ = now;
    haveStamp_ = true;

    if (dirty_) {
        status_ = "The file changed on disk. Save or discard to pick it up.";
        return false;
    }

    std::string text;
    if (!readFile(path_, text)) { status_ = "The file could not be re-read."; return false; }

    fmt::ActorScript parsed = fmt::parseActorScript(text);
    // A file mid-write, or one an IDE has left in a state the grammar does not cover, must not blank
    // the tab. The previous good parse is kept and the reason is shown.
    if (parsed.status != fmt::ActorParseStatus::Ok &&
        parsed.status != fmt::ActorParseStatus::NoRegion) {
        status_ = "Reloaded, but the region could not be read: " + parsed.error;
        return false;
    }
    source_ = std::move(text);
    script_ = std::move(parsed);
    classes_ = fmt::parseActorClasses(source_);
    pickFirstPreviewable();
    if (selected_ >= static_cast<int>(script_.models.size())) selected_ = -1;
    // The source changed, so the LIVE view is stale too -- but only against the class that is loaded
    // now. A text edit does not reload C#, so the respawn shows the same thing until Compile C# runs;
    // that is honest, and it is what the panel says.
    liveStale_ = true;
    status_ = "Reloaded from disk.";
    AVER_INFO("[ActorEditor] {} changed on disk; reloaded {} placement(s)",
              path_, script_.models.size());
    return true;
}

// ---------------------------------------------------------------- the live view

void ActorEditor::rebuildLive(Engine& e) {
    liveStale_ = false;
    liveDraws_.clear();
    liveUnnamed_ = 0;
    liveCapsuleFromSource_ = false;
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    const fmt::ActorClassInfo* info = activeInfo();
    if (!info || !e.device()) { liveWhy_ = "No class selected."; live_ = false; return; }

    // The REGISTRY name, which is the attribute's when there is one and the C# type name otherwise --
    // HostBridge.ResolveClassIdentity decides it that way and this has to agree, or a class declared
    // [AverClass("Crate")] on `class WoodenCrate` would be looked up under the wrong one. Both are
    // tried because the parser cannot always tell which the bridge chose.
    const std::string& reg = info->className.empty() ? info->typeName : info->className;
    int32_t c = reg.empty() ? 0 : aver_fw_class_find(reg.c_str());
    if (!c && !info->typeName.empty() && info->typeName != reg)
        c = aver_fw_class_find(info->typeName.c_str());
    if (!c) {
        liveWhy_ = "'" + (reg.empty() ? info->typeName : reg) +
                   "' is not loaded. Compile C#, then start the project's scripts.";
        live_ = false;
        return;
    }

    // At the ORIGIN with no overrides, so every world matrix that comes back IS the local transform
    // relative to the actor -- the same space the parsed view draws in, which is what lets the two
    // be compared by flicking the toggle rather than by reading numbers.
    const int32_t root = aver_fw_spawn_preview(c, "$ActorEditorPreview", nullptr, nullptr, nullptr);
    if (!root) {
        liveWhy_ = "'" + reg + "' would not spawn. Abstract classes cannot be instanced.";
        live_ = false;
        return;
    }

    g_meshNames.reset(g_contentRoot);
    g_meshNames.ensure();
    g_meshNames.addLiteralsFrom(source_);

    const MeshFields& mf = meshFields();
    // Depth-first over the spawned subtree, iteratively -- BuildModels is authored code and nothing
    // stops it nesting, so a recursive walk here would put the depth limit in the wrong place.
    std::vector<int32_t> stack{root};
    while (!stack.empty()) {
        const int32_t ent = stack.back();
        stack.pop_back();
        for (int32_t k = aver_scene_first_child(ent); k; k = aver_scene_next_sibling(k))
            stack.push_back(k);
        if (!mf.ok()) continue;

        const i64 id = aver_scene_get_i64(ent, mf.mesh);

        // HIDDEN before anything else can draw it. world().flush retires a destroy on the NEXT frame
        // boundary, so between this call and that flush the preview actor is a live entity sitting at
        // the world origin -- and the level viewport would render it. Clearing the visible bit makes
        // that impossible regardless of where in the frame this tab happens to be drawn.
        aver_scene_set_i32(ent, mf.flags, aver_scene_get_i32(ent, mf.flags) & ~0x1);

        if (id == 0) continue;
        const std::string* path = g_meshNames.find(static_cast<u64>(id));
        if (!path) { ++liveUnnamed_; continue; }

        render::preview::PreviewDraw d;
        d.mesh = g_meshes.resolve(*e.device(), *path, &d.boundsRadius);
        aver_scene_world_matrix(ent, d.world);
        liveDraws_.push_back(d);
    }

    aver_fw_destroy_preview(root);

    // A CHARACTER THAT BUILT NOTHING still has a shape, and it is the capsule.
    //
    // AverCharacter keeps Height and Radius as plain managed fields -- there is no component and no
    // scene field, so a native walk of the spawned actor cannot see them at all. The only place those
    // numbers are legible is the source, which is where the parsed view already gets them. Falling
    // back to it here is the difference between opening a project's central actor and finding an
    // empty box; the panel says outright that this one figure did not come from the spawn.
    if (liveDraws_.empty() && info->kind == fmt::ActorKind::Character) {
        render::preview::PreviewDraw d;
        d.mesh = g_meshes.capsule(*e.device(), info->capsuleHeight, info->capsuleRadius,
                                  &d.boundsRadius);
        d.baseColor[0] = 0.45f; d.baseColor[1] = 0.62f; d.baseColor[2] = 0.85f;
        liveDraws_.push_back(d);
        liveCapsuleFromSource_ = true;
    }

    char msg[192];
    if (liveCapsuleFromSource_)
        std::snprintf(msg, sizeof msg,
                      "%s built no models; the capsule shown is the source's, not the class's.",
                      reg.c_str());
    else
        std::snprintf(msg, sizeof msg, "%zu model(s) built by %s.", liveDraws_.size(), reg.c_str());
    liveWhy_ = msg;
    AVER_INFO("[ActorEditor] live {}: {} model(s), {} unnamed", reg, liveDraws_.size(), liveUnnamed_);
#else
    (void)e;
    liveWhy_ = "This build has no framework or scene module.";
    live_ = false;
#endif
}

void ActorEditor::buildDrawList(Engine& e) {
    if (!g_preview || !e.device()) return;
    g_meshes.setContentRoot(*e.device(), g_contentRoot);

    // LIVE replaces the parsed list rather than adding to it. Drawing both would put two copies of
    // every model that agrees on top of each other and read as z-fighting, which is the opposite of
    // what a comparison is for.
    if (live_) {
        if (liveStale_) rebuildLive(e);
        if (live_) {
            std::vector<render::preview::PreviewDraw> copy = liveDraws_;
            g_preview->setDrawList(std::move(copy));
            if (!framed_) { g_preview->frameAll(); framed_ = true; }
            return;
        }
    }

    std::vector<render::preview::PreviewDraw> draws;
    draws.reserve(script_.models.size() + 1);

    // THE COMMON CASE FIRST. Most actors in most games are one mesh declared on the class, with no
    // designer region at all -- every actor in the SkyForge template is that shape. An editor that
    // only understood placements would show an empty view for all of them.
    if (const fmt::ActorClassInfo* info = activeInfo()) {
        if (info->hasMesh) {
            render::preview::PreviewDraw d;
            d.mesh = g_meshes.resolve(*e.device(), info->meshPath, &d.boundsRadius);
            d.selected = (selected_ == -1);
            draws.push_back(d);
        } else if (info->kind == fmt::ActorKind::Character) {
            // A first-person character has NO mesh on purpose -- you are inside your own head, and a
            // body drawn at the eye fills the screen. Its shape is the physics capsule, and drawing
            // that is the difference between opening the most important actor in a project and
            // showing a blank box for it.
            render::preview::PreviewDraw d;
            d.mesh = g_meshes.capsule(*e.device(), info->capsuleHeight, info->capsuleRadius,
                                      &d.boundsRadius);
            d.baseColor[0] = 0.45f; d.baseColor[1] = 0.62f; d.baseColor[2] = 0.85f;
            d.selected = (selected_ == -1);
            draws.push_back(d);
        }
    }
    for (int i = 0; i < static_cast<int>(script_.models.size()); ++i) {
        const fmt::ActorModel& m = script_.models[static_cast<usize>(i)];
        render::preview::PreviewDraw d;
        d.mesh = g_meshes.resolve(*e.device(), m.meshPath, &d.boundsRadius);
        composeTransform(m.pos, m.rot, m.scale, d.world);
        d.selected = (i == selected_);
        draws.push_back(d);
    }
    g_preview->setDrawList(std::move(draws));
    if (!framed_) { g_preview->frameAll(); framed_ = true; }
}

// ---------------------------------------------------------------- the gizmo

namespace {
// How long an axis handle is on screen, in pixels at the image's own scale. Constant in SCREEN space
// so a handle is grabbable whether the actor is a bolt or a building.
constexpr f32 kGizmoPixels = 64.0f;
constexpr f32 kGrabPixels = 10.0f;
} // namespace

bool ActorEditor::projectToScreen(const f32 world[3], f32 imageSize, ImVec2& out) const {
    if (!g_preview) return false;
    f32 vp[16];
    g_preview->viewProj(vp);
    // Row-vector: v * M, the engine convention. Transposing here is the classic way to get a gizmo
    // that tracks the object until the camera turns.
    const f32 x = world[0]*vp[0] + world[1]*vp[4] + world[2]*vp[8]  + vp[12];
    const f32 y = world[0]*vp[1] + world[1]*vp[5] + world[2]*vp[9]  + vp[13];
    const f32 w = world[0]*vp[3] + world[1]*vp[7] + world[2]*vp[11] + vp[15];
    if (w <= 1e-4f) return false;   // behind the eye; there is no honest screen position
    out.x = (x / w * 0.5f + 0.5f) * imageSize;
    out.y = (0.5f - y / w * 0.5f) * imageSize;   // screen +Y is down
    return true;
}

int ActorEditor::pickGizmoAxis(ImVec2 local, f32 imageSize) const {
    if (selected_ < 0 || selected_ >= static_cast<int>(script_.models.size())) return -1;
    const fmt::ActorModel& m = script_.models[static_cast<usize>(selected_)];
    ImVec2 origin;
    if (!projectToScreen(m.pos, imageSize, origin)) return -1;

    int best = -1;
    f32 bestD = kGrabPixels;
    for (int a = 0; a < 3; ++a) {
        ImVec2 tip;
        if (!axisTip(m, a, imageSize, tip)) continue;
        // Distance from the click to the SEGMENT, not to the tip: a user grabs anywhere along a
        // handle, and picking by tip alone makes the near end of every axis dead.
        const f32 dx = tip.x - origin.x, dy = tip.y - origin.y;
        const f32 len2 = dx*dx + dy*dy;
        f32 t = len2 > 1e-4f ? ((local.x - origin.x) * dx + (local.y - origin.y) * dy) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const f32 px = origin.x + dx * t, py = origin.y + dy * t;
        const f32 d = std::sqrt((local.x - px) * (local.x - px) + (local.y - py) * (local.y - py));
        if (d < bestD) { bestD = d; best = a; }
    }
    return best;
}

bool ActorEditor::axisTip(const fmt::ActorModel& m, int axis, f32 imageSize, ImVec2& out) const {
    ImVec2 origin;
    if (!projectToScreen(m.pos, imageSize, origin)) return false;
    // A world offset whose SCREEN length is kGizmoPixels, found by projecting a unit step and
    // scaling. A fixed world length would make the handle vanish when zoomed out and swamp the
    // object when zoomed in.
    f32 probe[3] = {m.pos[0], m.pos[1], m.pos[2]};
    probe[axis] += 1.0f;
    ImVec2 unit;
    if (!projectToScreen(probe, imageSize, unit)) return false;
    const f32 dx = unit.x - origin.x, dy = unit.y - origin.y;
    const f32 len = std::sqrt(dx*dx + dy*dy);
    if (len < 1e-5f) return false;   // the axis points at the eye; there is no direction to draw
    out.x = origin.x + dx / len * kGizmoPixels;
    out.y = origin.y + dy / len * kGizmoPixels;
    return true;
}

void ActorEditor::drawGizmo(ImVec2 topLeft, f32 imageSize, const fmt::ActorModel& m) const {
    ImVec2 origin;
    if (!projectToScreen(m.pos, imageSize, origin)) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // +X red, +Y green, +Z blue -- the engine's axes in the order every tool colours them, so a
    // handle means the same thing here as it does in the level viewport.
    const ImU32 colours[3] = {
        IM_COL32(230, 70, 70, 255), IM_COL32(90, 210, 90, 255), IM_COL32(80, 140, 245, 255)
    };
    const ImVec2 o(topLeft.x + origin.x, topLeft.y + origin.y);
    for (int a = 0; a < 3; ++a) {
        ImVec2 tip;
        if (!axisTip(m, a, imageSize, tip)) continue;
        const ImVec2 t(topLeft.x + tip.x, topLeft.y + tip.y);
        const bool hot = draggingAxis_ == a;
        dl->AddLine(o, t, colours[a], hot ? 4.0f : 2.5f);
        dl->AddCircleFilled(t, hot ? 6.0f : 4.5f, colours[a]);
    }
    dl->AddCircleFilled(o, 3.0f, IM_COL32(240, 240, 240, 255));
}

void ActorEditor::dragAlongAxis(fmt::ActorModel& m, int axis, ImVec2 delta, f32 imageSize) const {
    ImVec2 origin, tip;
    if (!projectToScreen(m.pos, imageSize, origin)) return;
    if (!axisTip(m, axis, imageSize, tip)) return;
    const f32 dx = tip.x - origin.x, dy = tip.y - origin.y;
    const f32 len2 = dx*dx + dy*dy;
    if (len2 < 1e-4f) return;

    // The mouse motion PROJECTED onto the axis, in units of the handle's screen length -- so
    // dragging along the handle moves the object along that axis and dragging across it does
    // nothing, which is what makes a single-axis gizmo feel like one.
    const f32 along = (delta.x * dx + delta.y * dy) / len2;

    // ...times the world length that handle represents. Recovered from the same projection, so the
    // conversion holds at any zoom and any distance without a scale factor anybody has to tune.
    f32 probe[3] = {m.pos[0], m.pos[1], m.pos[2]};
    probe[axis] += 1.0f;
    ImVec2 unit;
    if (!projectToScreen(probe, imageSize, unit)) return;
    const f32 unitLen = std::sqrt((unit.x - origin.x) * (unit.x - origin.x) +
                                  (unit.y - origin.y) * (unit.y - origin.y));
    if (unitLen < 1e-5f) return;
    m.pos[axis] += along * (kGizmoPixels / unitLen);
}

void ActorEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    // The preview is created on FIRST DRAW rather than at registration, because the factory is a
    // bare function pointer with no device to hand it and the device is what this needs.
    if (!g_previewTried && e.device()) {
        g_previewTried = true;
        g_preview = render::preview::ActorPreview::create(*e.device(), 1024);
        if (g_preview) {
            // Registered so its prePass runs. NON-OWNING on the device's side, which is why
            // shutdownActorEditors removes it before deleting -- see there.
            g_device = e.device();
            g_device->addRenderFeature(g_preview);
        } else {
            AVER_WARN("[ActorEditor] no preview on this backend; the tab shows numbers only");
        }
    }

    // Source-to-view, every frame this tab is visible. Cheap: one stat, and only for the tab you
    // are looking at.
    reloadIfChanged();

    buildDrawList(e);

    // ---- the toolbar ----
    //
    // At the top, and carrying the two actions this tab's work actually needs: a build, because
    // editing an actor is editing C# and the whole point is to see the result; and the IDE, because
    // a preview is for placement and the behaviour half is still text. Without them the tab is a
    // dead end -- you would look at an actor, then go elsewhere to do anything about it.
    {
        const bool busy = g_hooks.compileBusy && g_hooks.compileBusy();
        ImGui::BeginDisabled(!g_hooks.compileScripts || busy);
        if (ImGui::Button(busy ? "Compiling..." : "Compile C#")) g_hooks.compileScripts();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(g_hooks.compileScripts
                ? "Build this project's scripts, and bake its C# materials."
                : "No project is open.");

        ImGui::SameLine();
        ImGui::BeginDisabled(!g_hooks.openInIde);
        const std::string ideLabel = g_hooks.ideName.empty() ? std::string("Open in IDE")
                                                             : ("Open in " + g_hooks.ideName);
        if (ImGui::Button(ideLabel.c_str())) g_hooks.openInIde(path_);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(g_hooks.openInIde
                ? "The behaviour half of this actor is text, and belongs in a text editor."
                : "No IDE was found on this machine.");

        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Save")) {
            std::string why;
            status_ = save(&why) ? "Saved." : ("Save failed: " + why);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Frame All") && g_preview) g_preview->frameAll();

        // ---- Live ----
        //
        // Off by default, and deliberately. The parsed view works with nothing loaded, on a file that
        // has never compiled, in a build with no CLR; Live works only once the class is in the
        // registry. Defaulting to the fragile one would make the tab look broken in every case where
        // the robust one had something useful to show.
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Checkbox("Live", &live_)) { liveStale_ = true; framed_ = false; selected_ = -1; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Spawn the compiled class, run its BuildModels and show what it actually "
                              "builds.\nNo OnBeginPlay, no ticking, nothing possessed -- the actor is "
                              "destroyed the same frame.");
        if (live_) {
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) { liveStale_ = true; framed_ = false; }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Spawn it again. Do this after a Compile C#, which replaces the class.");
        }

        // The one thing a reader of this panel must not have to discover for themselves.
        ImGui::SameLine();
        ImGui::TextDisabled("|  preview lighting is fixed and does not match the level viewport");
    }
    ImGui::Separator();

    // ---- the view ----
    // DPI-SCALED, like everything else the editor lays out. Raw pixels here made the side panel a
    // tenth of a 300% display and the toolbar above it clip its own buttons.
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 side = 300.0f * dpi;
    const f32 viewW = avail > side * 1.6f ? avail - side : avail;

    // ---- classes with no viewport ----
    //
    // A GameMode is rules, a GameInstance is process-wide state, a PlayerController is input and a
    // possession policy. None has a transform, so a 3D view of one shows nothing -- and an empty
    // viewport reads as a broken editor rather than as a category that simply has no geometry. UE
    // solves the same problem by opening these on their defaults with a route to the full editor;
    // this does the same, and the route is the IDE because the behaviour half is text.
    const fmt::ActorClassInfo* active = activeInfo();
    const bool noViewport = active && !active->hasViewport();
    if (noViewport) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.14f, 0.10f, 1.0f));
        if (ImGui::BeginChild("##noviewport", ImVec2(viewW, 132.0f * dpi), ImGuiChildFlags_Borders)) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.35f, 1.0f),
                               "  A %s has no viewport.", fmt::actorKindName(active->kind));
            ImGui::TextWrapped("  It has no transform and nothing to place, so there is nothing for a "
                               "3D view to show. Its settings are below; its behaviour is code.");
            ImGui::Spacing();
            ImGui::Indent();
            ImGui::BeginDisabled(!g_hooks.openInIde);
            const std::string open = g_hooks.ideName.empty() ? std::string("Open in IDE")
                                                             : ("Open in " + g_hooks.ideName);
            if (ImGui::Button(open.c_str(), ImVec2(260.0f * dpi, 0.0f))) g_hooks.openInIde(path_);
            ImGui::EndDisabled();
            ImGui::Unindent();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else if (active && !active->drawable()) {
        // Spatial, but nothing declared to draw. Distinct from the case above and said differently:
        // this one CAN have geometry and does not yet, which is a thing the author can fix.
        ImGui::TextDisabled("This %s declares no mesh yet. Add one with b.Mesh(\"...\") in Configure.",
                            fmt::actorKindName(active->kind));
    }

    if (!noViewport && g_preview && g_preview->uiTextureId()) {
        // SQUARE, and bounded by the HEIGHT as well as the width: the target is square, so sizing on
        // width alone makes a wide short panel draw an image taller than the panel and the model list
        // beside it disappears below the fold.
        const f32 availH = ImGui::GetContentRegionAvail().y;
        f32 s = viewW < 64.0f ? 64.0f : viewW;
        if (availH > 64.0f && s > availH) s = availH;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(g_preview->uiTextureId()), ImVec2(s, s));

        // Orbit, zoom, and the gizmo. Input lives HERE rather than in the preview feature, because
        // the feature must stay drivable with no ImGui at all -- that is what lets a test be the
        // device.
        if (ImGui::IsItemHovered() || draggingAxis_ >= 0) {
            const ImGuiIO& io = ImGui::GetIO();

            // A DRAG ON A SELECTED PLACEMENT moves it; a drag anywhere else orbits. Which one is
            // decided on mouse-DOWN and held for the whole gesture: deciding per frame would let a
            // drag that started on the gizmo become an orbit the moment the cursor left the handle,
            // which is exactly when a user is dragging fastest.
            // NO DRAGGING IN THE LIVE VIEW. What is on screen there was produced by code, and there
            // is no byte in the file to write a new coordinate back to -- a handle that moved a model
            // and then lost the move on the next Refresh would be worse than no handle.
            const bool haveSel = !live_ && selected_ >= 0
                              && selected_ < static_cast<int>(script_.models.size());
            if (haveSel && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered()) {
                const ImVec2 m = io.MousePos;
                draggingAxis_ = pickGizmoAxis(ImVec2(m.x - at.x, m.y - at.y), s);
            }
            if (!io.MouseDown[ImGuiMouseButton_Left]) draggingAxis_ = -1;

            if (draggingAxis_ >= 0 && haveSel) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                if (d.x != 0.0f || d.y != 0.0f) {
                    dragAlongAxis(script_.models[static_cast<usize>(selected_)], draggingAxis_, d, s);
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    dirty_ = true;
                }
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                g_preview->camera().addOrbit(-d.x * 0.4f, d.y * 0.4f);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            }
            if (io.MouseWheel != 0.0f && draggingAxis_ < 0)
                g_preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f);
        }

        if (!live_ && selected_ >= 0 && selected_ < static_cast<int>(script_.models.size()))
            drawGizmo(at, s, script_.models[static_cast<usize>(selected_)]);
    } else if (!noViewport) {
        // Only when a viewport WAS expected. Saying "no preview on this backend" about a Game Mode
        // blames the hardware for a category that never had geometry, and sends somebody looking for
        // a driver problem that is not there.
        ImGui::TextDisabled("No 3D preview on this backend.");
    }

    // Meshes the source names and the project does not have. Said out loud, because an actor whose
    // meshes are all missing renders an EMPTY view, and an empty view with no explanation is
    // indistinguishable from a broken preview.
    if (!g_meshes.missing().empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1.0f), "%zu mesh(es) not found:",
                           g_meshes.missing().size());
        for (const std::string& m : g_meshes.missing()) ImGui::BulletText("%s", m.c_str());
    }

    ImGui::SameLine();
    ImGui::BeginGroup();

    // ---- the models ----
    // Which actor in this file. Shown whenever there is more than one, because a file with five and
    // a picker that is not there is a file four of whose actors are invisible.
    if (classes_.size() > 1) {
        // A pointer array rather than ImGui's NUL-separated string form. The same widget, and the
        // labels are rebuilt each frame from data a reload can change -- a packed buffer would have
        // to be rebuilt anyway, and this one cannot be got subtly wrong.
        std::vector<std::string> labels;
        std::vector<const char*> items;
        labels.reserve(classes_.size());
        items.reserve(classes_.size());
        for (const fmt::ActorClassInfo& k : classes_) {
            std::string n = k.typeName.empty() ? k.className : k.typeName;
            if (!k.anything()) n += "  (nothing to draw)";
            labels.push_back(std::move(n));
        }
        for (const std::string& n : labels) items.push_back(n.c_str());
        if (ImGui::Combo("Actor", &activeClass_, items.data(), static_cast<int>(items.size())))
            { selected_ = -1; framed_ = false; liveStale_ = true; }
    } else if (const fmt::ActorClassInfo* k = activeInfo()) {
        // The C# type, which is what the author named the thing. The bound name is shown below as
        // bookkeeping.
        ImGui::Text("%s", (k->typeName.empty() ? k->className : k->typeName).c_str());
    }

    // CLASS DEFAULTS. What the class states about itself, which for a GameMode or a Controller is
    // the whole of what an editor can usefully show.
    if (const fmt::ActorClassInfo* k = activeInfo()) {
        ImGui::TextDisabled("%s%s%s", fmt::actorKindName(k->kind),
                            k->baseType.empty() ? "" : "  :  ", k->baseType.c_str());
        // The BOUND name, small and secondary. It is what a level and a GameMode reference, so it
        // cannot be hidden -- but it is bookkeeping, not the thing being edited, and leading it with
        // a BP_ prefix made the panel read as if the prefix were the actor's name.
        if (!k->className.empty() && k->className != k->typeName)
            ImGui::TextDisabled("binds as \"%s\"", k->className.c_str());
    }

    // ---- class defaults, EDITABLE ----
    //
    // Each control is shown only when the parse found a SPAN for it -- the byte the value came from.
    // A field with no span is a value written in a form this cannot put back (a constant, an
    // expression, a computed default), and showing a box that silently would not save is worse than
    // showing nothing. So the panel offers exactly what it can honour.
    if (fmt::ActorClassInfo* k = activeInfoMutable()) {
        bool edited = false;
        ImGui::Separator();
        // Room for the LABEL. ImGui puts a widget's label to its right, and a full-width widget
        // pushes it off the panel -- which left a column of bare numbers with nothing saying which
        // was the height and which the radius.
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);

        if (k->meshPathSpan.valid()) {
            char buf[260];
            std::snprintf(buf, sizeof buf, "%s", k->meshPath.c_str());
            if (ImGui::InputText("Mesh", buf, sizeof buf)) { k->meshPath = buf; edited = true; }
        }
        if (k->materialSpan.valid()) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s", k->material.c_str());
            if (ImGui::InputText("Material", buf, sizeof buf)) { k->material = buf; edited = true; }
        }
        if (k->capsuleHeightSpan.valid())
            edited |= ImGui::DragFloat("Height (cm)", &k->capsuleHeight, 1.0f, 1.0f, 1000.0f, "%.0f");
        if (k->capsuleRadiusSpan.valid())
            edited |= ImGui::DragFloat("Radius (cm)", &k->capsuleRadius, 0.5f, 1.0f, 500.0f, "%.0f");
        if (k->eyeHeightSpan.valid())
            edited |= ImGui::DragFloat("Eye height (cm)", &k->eyeHeight, 1.0f, 0.0f, 1000.0f, "%.0f");
        if (k->cameraSpan[0].valid())
            edited |= ImGui::DragFloat("FOV (deg)", &k->cameraFovDeg, 0.5f, 5.0f, 170.0f, "%.0f");
        if (k->lightSpan[0].valid())
            edited |= ImGui::DragFloat("Intensity (lux)", &k->lightIntensityLux, 10.0f, 0.0f, 100000.0f, "%.0f");
        if (k->lightSpan[1].valid())
            edited |= ImGui::DragFloat("Light range (cm)", &k->lightRangeCm, 5.0f, 1.0f, 100000.0f, "%.0f");

        // The capsule the preview draws is rebuilt from these, so a drag moves the shape on screen
        // immediately -- the same property the placement gizmo has, and for the same reason: the
        // picture is built from the values every frame rather than from a spawned copy of them.
        ImGui::PopItemWidth();
        if (edited) { dirty_ = true; framed_ = false; }

        if (k->kind == fmt::ActorKind::Character && !k->capsuleHeightSpan.valid())
            ImGui::TextDisabled("capsule: %.0f x %.0f cm (framework default; this class states none)",
                                180.0, 34.0);
    }

    // What this actor declares beyond geometry. Said out loud because NEITHER is drawn: an actor
    // that is only a camera previews as an empty view, and empty is indistinguishable from broken.
    if (const fmt::ActorClassInfo* k = activeInfo()) {
        if (k->hasCamera)
            ImGui::TextDisabled("camera: %.0f deg, %.0f-%.0f cm (not drawn)",
                                static_cast<double>(k->cameraFovDeg),
                                static_cast<double>(k->cameraNearCm), static_cast<double>(k->cameraFarCm));
        if (k->hasPointLight)
            ImGui::TextDisabled("point light: %.0f lux, %.0f cm (not drawn)",
                                static_cast<double>(k->lightIntensityLux),
                                static_cast<double>(k->lightRangeCm));
        // Only when there is genuinely nothing to show. A Character declares no mesh and is still
        // drawn -- its capsule IS its shape -- so saying "declares nothing" there is simply untrue.
        if (!k->anything() && !k->drawable() && k->hasViewport())
            ImGui::TextDisabled("This %s declares no mesh, camera or light.", fmt::actorKindName(k->kind));
    }
    ImGui::Separator();
    ImGui::Text("%zu placement(s)", script_.models.size());
    ImGui::TextDisabled("Preview lighting is fixed and does not match the level viewport.");
    ImGui::Separator();

    for (int i = 0; i < static_cast<int>(script_.models.size()); ++i) {
        const fmt::ActorModel& m = script_.models[static_cast<usize>(i)];
        char label[192];
        std::snprintf(label, sizeof label, "%s##model%d", m.property.c_str(), i);
        if (ImGui::Selectable(label, selected_ == i)) selected_ = i;
    }

    ImGui::Separator();
    if (selected_ >= 0 && selected_ < static_cast<int>(script_.models.size())) {
        fmt::ActorModel& m = script_.models[static_cast<usize>(selected_)];
        ImGui::TextDisabled("%s", m.meshPath.c_str());
        ImGui::TextDisabled("material: %s", m.material.c_str());
        // The match key, shown because it is what a rewrite finds the line by -- when a save goes to
        // the wrong row this is the first thing anyone will want to see.
        ImGui::TextDisabled("id: 0x%016llX", static_cast<unsigned long long>(m.objectId));

        bool changed = false;
        changed |= ImGui::DragFloat3("Position (cm)", m.pos, 1.0f);
        changed |= ImGui::DragFloat3("Rotation (deg)", m.rot, 0.5f);
        changed |= ImGui::DragFloat3("Scale", m.scale, 0.01f, 0.001f, 1000.0f);
        // The preview is rebuilt from these every frame, so it follows the drag with no extra
        // plumbing -- which is the whole point of drawing the source rather than a spawned actor.
        if (changed) dirty_ = true;
    } else {
        ImGui::TextDisabled("Select a placement to edit it.");
    }

    ImGui::Separator();
    // The live line is kept SEPARATE from status_ and outlives it: status_ is the last thing that
    // happened, and "why Live is off" is a standing condition that has to stay readable while the
    // user goes and compiles.
    if (!liveWhy_.empty()) {
        if (live_) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.0f), "Live: %s", liveWhy_.c_str());
            if (liveUnnamed_ > 0)
                ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.35f, 1.0f),
                                   "%d model(s) name a mesh no file in this project matches.",
                                   liveUnnamed_);
        } else {
            ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.35f, 1.0f), "Live off: %s", liveWhy_.c_str());
        }
    }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    ImGui::EndGroup();
#else
    (void)e;
#endif
}

} // namespace

void setActorEditorContentRoot(std::string root) {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    g_meshNames.reset(root);   // a different project is a different set of meshes under the same names
#endif
    g_contentRoot = std::move(root);
}

void setActorEditorHooks(ActorEditorHooks hooks) { g_hooks = std::move(hooks); }

void setActorEditorLiveByDefault(bool on) { g_liveDefault = on; }

void shutdownActorEditors() {
    if (g_device && g_preview) g_device->removeRenderFeature(g_preview);
    g_device = nullptr;
    delete g_preview;
    g_preview = nullptr;
    g_previewTried = false;
}

std::unique_ptr<AssetEditor> makeActorEditor(const std::string& path) {
    // Only a `.cs`, and only one that actually carries a generated region. A hand-written actor with
    // no designer file is not an editing surface this can offer anything for, and opening it here
    // instead of in the IDE would be taking something away.
    const std::filesystem::path p(path);
    if (p.extension() != ".cs") return nullptr;

    std::string source;
    if (!readFile(path, source)) return nullptr;

    fmt::ActorScript parsed = fmt::parseActorScript(source);
    std::vector<fmt::ActorClassInfo> classes = fmt::parseActorClasses(source);

    // ANY recognised actor opens, not only one with geometry. A GameMode declaring nothing at all is
    // still an actor the editor can say something about -- its kind, its base, and a route to the
    // code -- and refusing it would send the most structural classes in a project to the IDE with no
    // sign the editor knows what they are. A class with no recognised base and nothing declared is
    // not an actor and still goes to the IDE.
    bool previewable = false;
    for (const fmt::ActorClassInfo& k : classes)
        if (k.anything() || k.kind != fmt::ActorKind::Unknown) previewable = true;

    // Openable if it has EITHER a designer region or something the class itself declares. A plain
    // .cs with neither is not an actor and belongs in the IDE -- opening it here would be taking
    // something away rather than adding a view.
    if (parsed.status == fmt::ActorParseStatus::NoRegion && !previewable) return nullptr;
    if (parsed.status == fmt::ActorParseStatus::NoRegion) {
        // A single-mesh actor. No region to rewrite, so the tab is a viewer -- which is the honest
        // state rather than offering a save that would have nowhere to write.
        return std::make_unique<ActorEditor>(path, fmt::ActorScript{}, std::move(classes), std::move(source));
    }
    if (parsed.status != fmt::ActorParseStatus::Ok) {
        // Declined LOUDLY. Malformed is the signal that the text has left the locked grammar, which
        // is the case a Roslyn backend would pick up; until that exists, saying so beats opening a
        // tab that shows nothing and cannot save.
        AVER_WARN("[ActorEditor] {} has a generated region this build cannot read: {}",
                  path, parsed.error);
        return nullptr;
    }
    return std::make_unique<ActorEditor>(path, std::move(parsed), std::move(classes), std::move(source));
}

} // namespace aver::editor
