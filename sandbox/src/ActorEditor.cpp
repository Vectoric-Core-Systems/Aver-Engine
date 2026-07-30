#include "ActorEditor.hpp"
#include "ToolGlyphs.hpp"
#include "EditorPrefs.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/ActorScript.hpp"
#if AVER_HAVE_ROSLYN
#  include "aver/formats/AverDesign.hpp"   // the Roslyn escalation, when the scanner declines
#endif
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
// Column widths in PHYSICAL pixels, shared by every actor tab and seeded from the prefs file on
// first use. Zero means "not seeded yet", which is why they are not simply defaulted here.
f32 g_leftColW = 0.0f, g_rightColW = 0.0f;
// The defaults, named once. Reset Layout and the first-use seed both need them, and two literals
// that must agree are two literals that eventually will not.
constexpr f32 kDefaultLeftColumn  = 200.0f;
constexpr f32 kDefaultRightColumn = 280.0f;
constexpr const char* kPrefLeft  = "actorEditor.leftColumn";
constexpr const char* kPrefRight = "actorEditor.rightColumn";
// Bumped by the app when the script assembly is swapped. Never reset: a tab compares values rather
// than testing a flag, so wrapping is the only failure and it takes 4 billion reloads.
u32 g_scriptGeneration = 1;

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

// WHAT TO CALL AN ACTOR ON SCREEN.
//
// One function because the rule was written out three times and each copy fell back to className --
// so an actor whose C# type the parser could not read showed as "BP_Gun", and the Live panel said
// "BP_Gun built nothing" even where the tab above it said "Gun". The BP_ prefix is a REGISTRY
// convention: it is what a level file stores and what aver_fw_class_find resolves. It is not the
// actor's name and it should never be presented as one.
//
// Preference order: the C# type, then the bound name with the template's prefix stripped, then the
// file. Never empty, because a nameless row in a picker is a row nobody can choose deliberately.
std::string actorDisplayName(const fmt::ActorClassInfo& k, std::string_view fileStem = {}) {
    if (!k.typeName.empty()) return k.typeName;
    if (!k.className.empty()) {
        // Only the prefix the templates actually use. Stripping any capitalised prefix would rename
        // a class somebody deliberately called AIGuard.
        if (k.className.rfind("BP_", 0) == 0 && k.className.size() > 3) return k.className.substr(3);
        return k.className;
    }
    return std::string(fileStem);
}

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

// ---------------------------------------------------------------- the component tree
//
// WHAT AN ACTOR IS MADE OF, as a hierarchy -- UE's Components panel, and for the same reason. An
// actor is not a flat list of meshes: it is a root with a transform and a tree of things attached to
// it, some of which draw and some of which do not. Until you can see that tree you are editing an
// actor by guessing which line in the file corresponds to the box you are looking at.
//
// It is built from BOTH sources and is the same shape either way, which is what lets the Live toggle
// be a toggle rather than a different editor:
//   * PARSED  -- the class's own declarations (mesh, capsule, camera, light) plus every b.Place row.
//   * LIVE    -- the spawned subtree, walked, so nesting that BuildModels created is real here.
//
// A FLAT VECTOR with child INDICES rather than pointers or owned children. Nodes are appended while
// walking, and a vector that reallocates would invalidate every pointer taken so far -- which is the
// standard way this shape gets written and then subtly broken by the first actor with enough parts.
enum class ComponentKind { Root, StaticMesh, Capsule, Camera, PointLight };

const char* componentKindName(ComponentKind k) {
    switch (k) {
        case ComponentKind::Root:       return "Root";
        case ComponentKind::StaticMesh: return "Static Mesh";
        case ComponentKind::Capsule:    return "Capsule";
        case ComponentKind::Camera:     return "Camera";
        case ComponentKind::PointLight: return "Point Light";
    }
    return "Component";
}

struct ComponentNode {
    std::string name;                 // what the author called it, or the component's own kind
    std::string detail;               // the mesh path, the capsule size -- the second line in the tree
    ComponentKind kind = ComponentKind::StaticMesh;

    // Which b.Place row this came from, or -1. It is what connects a click in the tree to the bytes
    // the gizmo writes back, so a node without one is a node the gizmo must not offer to drag.
    int modelIndex = -1;

    // Local, and the world it composes to. Both kept: the panel edits LOCAL (that is what the source
    // stores) while the viewport and the gizmo need WORLD.
    f32 local[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    f32 world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    int parent = -1;
    std::vector<int> children;

    // Does it put geometry on screen? A camera and a light do not, and the panel says so rather than
    // leaving somebody hunting for a mesh that was never going to be there.
    bool drawsGeometry = false;
};

// row-vector, translation in the last row: world = local * parentWorld.
void multiply4x4(const f32 a[16], const f32 b[16], f32 out[16]) {
    f32 t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] +
                       a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
    for (int i = 0; i < 16; ++i) out[i] = t[i];
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

    // The watcher saw it. Do NOT read the file here: this runs between frames, off the back of an
    // OS notification, and the editor's whole reload path wants a device and a preview that only
    // exist inside draw(). Latching a flag keeps every file read on the frame thread.
    void onFileChanged() override { externalChange_ = true; }

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

    // Ask the shared preview to match the panel, once the panel has stopped moving.
    //
    // DEBOUNCED, and that is the whole design rather than a refinement. Resizing the target destroys
    // a texture the UI is sampling, which needs a waitIdle -- a whole-GPU stall. Doing that on every
    // frame of a splitter drag is one stall per frame for as long as the drag lasts. Waiting for the
    // size to settle costs one stall per gesture.
    //
    // The 24-pixel deadband is the second half of it: without one, a layout that oscillates by a
    // pixel between frames (a scrollbar appearing and disappearing) would resize forever.
    void requestPreviewSize(f32 w, f32 h) {
        if (!g_preview) return;
        const u32 want [2] = {static_cast<u32>(w), static_cast<u32>(h)};
        const u32 have [2] = {g_preview->width(), g_preview->height()};
        const auto far_ = [](u32 a, u32 b) { return (a > b ? a - b : b - a) > 24u; };
        if (!far_(want[0], have[0]) && !far_(want[1], have[1])) { previewResizeAt_ = -1.0; return; }

        const double now = ImGui::GetTime();
        if (previewResizeAt_ < 0.0 || far_(want[0], pendingPreviewW_) || far_(want[1], pendingPreviewH_)) {
            // A NEW size restarts the timer, so a drag keeps deferring instead of firing mid-way.
            previewResizeAt_ = now + 0.25;
            pendingPreviewW_ = want[0];
            pendingPreviewH_ = want[1];
            return;
        }
        if (now < previewResizeAt_) return;
        previewResizeAt_ = -1.0;
        g_preview->resize(pendingPreviewW_, pendingPreviewH_);
    }
    double previewResizeAt_ = -1.0;
    u32 pendingPreviewW_ = 0, pendingPreviewH_ = 0;

    // Is the tree row currently selected THIS draw? Asked by kind rather than by index because the
    // draw list and the tree are built from the same data by two walks, and matching them on
    // position would break the first time either grew a row the other did not.
    bool nodeSelected(ComponentKind kind, int modelIndex) const {
        if (selectedNode_ < 0 || selectedNode_ >= static_cast<int>(tree_.size())) return false;
        const ComponentNode& n = tree_[static_cast<usize>(selectedNode_)];
        return n.kind == kind && n.modelIndex == modelIndex;
    }

    // ---- the component tree ----
    std::vector<ComponentNode> tree_;
    int selectedNode_ = -1;

    int addNode(ComponentNode n, int parent) {
        n.parent = parent;
        // Compose as we go. Every parent is appended before its children -- both builders walk
        // top-down -- so the parent's world is always final by the time a child needs it.
        if (parent >= 0) multiply4x4(n.local, tree_[static_cast<usize>(parent)].world, n.world);
        else             for (int i = 0; i < 16; ++i) n.world[i] = n.local[i];
        tree_.push_back(std::move(n));
        const int idx = static_cast<int>(tree_.size()) - 1;
        if (parent >= 0) tree_[static_cast<usize>(parent)].children.push_back(idx);
        return idx;
    }

    void buildTree();
    void drawComponentTree(bool ownColumn);
    void drawTreeNode(int idx);
    // Keep selected_ (an index into script_.models, which the gizmo and the rewriter use) in step
    // with selectedNode_ (an index into the tree, which the panel uses). One is the source of truth
    // for editing and the other for display; deriving rather than duplicating is what stops them
    // disagreeing about which thing is selected.
    void syncSelectionFromNode() {
        selected_ = (selectedNode_ >= 0 && selectedNode_ < static_cast<int>(tree_.size()))
                  ? tree_[static_cast<usize>(selectedNode_)].modelIndex : -1;
    }

    // ---- the gizmo ----
    //
    // A TRANSLATE gizmo, three axes, drawn as an ImGui overlay over the preview image rather than as
    // geometry in the pass. Two reasons, and neither is laziness: the preview feature must stay
    // drivable with no ImGui (that is what lets a test be the device), and a 3D gizmo has to be
    // pickable at a constant SCREEN size, which means it cannot be part of a scene that scales.
    //
    // Projection goes through the preview's own camera, so what is drawn is where the handle
    // actually points. Deriving it from anything else is how a gizmo ends up offset from its object.
    bool projectToScreen(const f32 world[3], ImVec2 imageSize, ImVec2& out) const;
    int  pickGizmoAxis(ImVec2 local, ImVec2 imageSize) const;
    void drawGizmo(ImVec2 imageTopLeft, ImVec2 imageSize, const fmt::ActorModel& m) const;
    // The components that have no mesh, drawn as wireframes over the image -- a camera's frustum and
    // a light's reach. UE draws both in its Blueprint viewport and an actor that is only a camera is
    // otherwise an empty box.
    void drawComponentWireframes(ImVec2 imageTopLeft, ImVec2 imageSize) const;
    void dragAlongAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const;
    // The rotate and scale halves of the same gesture, plus the projection all three share.
    bool dragAlong(const fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize,
                   f32& outAlong) const;
    void dragRotateAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const;
    void dragScaleAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const;

    // Which transform the gizmo applies. The SAME four tools as the level viewport, selected the same
    // way (the 1-4 keys) and drawn with the same icons out of ToolGlyphs.hpp -- an actor viewport whose
    // tools were a different set, or looked different, would be a second editor to learn.
    //
    // Per-tab rather than shared with the level's tool: the two viewports hold different selections
    // and a user switching tabs to nudge a placement does not expect the level's tool to change under
    // them.
    int tool_ = ToolMove;
    bool axisTip(const fmt::ActorModel& m, int axis, ImVec2 imageSize, ImVec2& out) const;

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
        if (classes_.empty()) return;

        // THE FILE'S OWN NAME FIRST, and this is a fix rather than a preference.
        //
        // The rule was "the first class in FILE order that draws something". Gun.cs declares
        // GunPart above Gun, and GunPart has a mesh -- so opening Gun.cs opened BP_GunPart, an
        // eight-line helper, and named the tab after it. The principal actor in a file is almost
        // always the one the file is named for; every actor in the SkyForge template follows that,
        // and so does every one anybody writes, because that is what naming a file after a class
        // means.
        //
        // Matched against typeName and className, and against the className with a BP_ prefix
        // stripped, because a class is free to bind under a different name than its C# type.
        std::string stem = std::filesystem::path(path_).stem().string();
        if (stem.size() > 9 && stem.compare(stem.size() - 9, 9, ".Designer") == 0)
            stem.erase(stem.size() - 9);
        const auto namesFile = [&](const fmt::ActorClassInfo& k) {
            if (!stem.empty() && k.typeName == stem) return true;
            if (!stem.empty() && k.className == stem) return true;
            // "BP_Gun" for Gun.cs. Only the prefix the templates use, not any prefix -- guessing
            // more broadly would start matching classes that merely end with the file's name.
            if (!stem.empty() && k.className.rfind("BP_", 0) == 0 &&
                k.className.compare(3, std::string::npos, stem) == 0) return true;
            return false;
        };
        for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
            if (namesFile(classes_[static_cast<usize>(i)])) { activeClass_ = i; break; }

        // Then something to LOOK at, because a file that declares a GameMode above three meshed
        // actors should open on one of the meshes rather than on the rules object.
        if (activeClass_ < 0)
            for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
                if (classes_[static_cast<usize>(i)].drawable()) { activeClass_ = i; break; }
        if (activeClass_ < 0)
            for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
                if (classes_[static_cast<usize>(i)].anything()) { activeClass_ = i; break; }
        // Nothing previewable but classes present: still name the first, so the panel says what the
        // file HOLDS rather than looking empty.
        if (activeClass_ < 0) activeClass_ = 0;
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
    // The class built nothing and declares no placements: whatever it looks like in game is
    // assembled by gameplay, which a construction-only preview cannot and must not show.
    bool liveAssemblesAtPlayTime_ = false;
    // The generation the current live snapshot was built against. Different from g_scriptGeneration
    // means the class that produced it has since been unloaded, so the picture is of dead code.
    u32 liveGeneration_ = 0;

    // Set by the host when the watcher saw this file change, cleared when the reload is done.
    //
    // Separate from the mtime stamp rather than folded into it: the stamp answers "has the file
    // moved on since I read it", which a background tab cannot ask because it is not drawn. This
    // answers "somebody told me it did", and the two agree in the common case and cost nothing when
    // they do -- reloadIfChanged still re-stats, so a spurious notification reloads nothing.
    bool externalChange_ = false;
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
    // The watcher's word, taken and cleared whatever happens below.
    //
    // It does not REPLACE the stamp check, it bypasses it. A safe save (write temp, replace target,
    // rename) can leave a modification time this tab has already seen -- the writer preserves it, or
    // the filesystem's resolution rounds two writes in the same tick to the same value -- and the
    // stamp then says "nothing happened" about a file whose bytes are entirely different. That is
    // the precise failure this whole watcher exists to fix, so an explicit notification has to win
    // over the stamp rather than be filtered by it.
    const bool told = externalChange_;
    externalChange_ = false;

    std::error_code ec;
    const std::filesystem::file_time_type now = std::filesystem::last_write_time(path_, ec);
    if (ec) return false;
    if (!told && haveStamp_ && now == stamp_) return false;
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
    // The tree is rebuilt from the new parse next frame, so an index into the old one names nothing.
    selectedNode_ = -1;
    // The source changed, so the LIVE view is stale too -- but only against the class that is loaded
    // now. A text edit does not reload C#, so the respawn shows the same thing until Compile C# runs;
    // that is honest, and it is what the panel says.
    liveStale_ = true;
    status_ = "Reloaded from disk.";
    AVER_INFO("[ActorEditor] {} changed on disk; reloaded {} placement(s)",
              path_, script_.models.size());
    return true;
}

// ---------------------------------------------------------------- the component tree

void ActorEditor::buildTree() {
    tree_.clear();
    const fmt::ActorClassInfo* info = activeInfo();

    // THE ROOT IS THE ACTOR ITSELF, always, even when it has nothing attached. UE shows it and so
    // does this: the root carries the transform everything else is relative to, and an empty tree
    // reads as "the editor failed" where a lone root reads as "this actor has no components yet",
    // which is a true and actionable thing to say.
    ComponentNode root;
    root.kind = ComponentKind::Root;
    // The class's own name where there is one. Falling back to the FILE means a `.Designer.cs`
    // would show as "Car.Designer" -- the generated half's filename rather than the actor -- so the
    // suffix comes off. A designer file's partial class carries no attribute and no base, so there
    // is genuinely no ActorClassInfo for it and the filename is the only name available.
    if (info) {
        root.name = actorDisplayName(*info);
    } else {
        std::string stem = std::filesystem::path(path_).stem().string();
        if (stem.size() > 9 && stem.compare(stem.size() - 9, 9, ".Designer") == 0)
            stem.erase(stem.size() - 9);
        root.name = std::move(stem);
    }
    if (info) root.detail = fmt::actorKindName(info->kind);
    const int rootIdx = addNode(std::move(root), -1);

    // ---- LIVE: the spawned subtree, which is the assembled truth ----
    //
    // The live draws already carry WORLD matrices read off real entities, so nesting BuildModels
    // created is preserved exactly. They are attached under the root as a flat set because the walk
    // that produced them flattened the parentage -- the transforms are still right, which is what
    // the viewport needs; recovering the shape as well is a job for the walk, not for this.
    if (live_) {
        for (usize i = 0; i < liveDraws_.size(); ++i) {
            ComponentNode n;
            n.kind = ComponentKind::StaticMesh;
            char nm[64];
            std::snprintf(nm, sizeof nm, "Model %zu", i);
            n.name = nm;
            n.detail = "built at run time";
            n.drawsGeometry = liveDraws_[i].mesh != 0;
            for (int k = 0; k < 16; ++k) n.local[k] = liveDraws_[i].world[k];
            const int idx = addNode(std::move(n), rootIdx);
            // The live world matrix is already absolute in the actor's frame, so overwrite the
            // composed one rather than letting it be multiplied by the root a second time.
            for (int k = 0; k < 16; ++k) tree_[static_cast<usize>(idx)].world[k] = liveDraws_[i].world[k];
        }
        return;
    }

    // ---- PARSED: what the class declares, then what the designer region places ----
    if (info) {
        if (info->hasMesh) {
            ComponentNode n;
            n.kind = ComponentKind::StaticMesh;
            n.name = "StaticMesh";
            n.detail = info->meshPath;
            n.drawsGeometry = true;
            addNode(std::move(n), rootIdx);
        }
        if (info->kind == fmt::ActorKind::Character) {
            ComponentNode n;
            n.kind = ComponentKind::Capsule;
            n.name = "Capsule";
            char d[96];
            std::snprintf(d, sizeof d, "%.0f x %.0f cm",
                          static_cast<double>(info->capsuleHeight > 1.0f ? info->capsuleHeight : 180.0f),
                          static_cast<double>(info->capsuleRadius > 0.1f ? info->capsuleRadius : 34.0f));
            n.detail = d;
            n.drawsGeometry = true;
            addNode(std::move(n), rootIdx);
        }
        // A CAMERA AND A LIGHT ARE COMPONENTS TOO, and they are the reason this panel exists rather
        // than a list of meshes. An actor that is only a camera previewed as nothing at all, and
        // "nothing" and "broken" are indistinguishable on screen. Here it has a row.
        if (info->hasCamera) {
            ComponentNode n;
            n.kind = ComponentKind::Camera;
            n.name = "Camera";
            char d[96];
            std::snprintf(d, sizeof d, "fov %.0f deg", static_cast<double>(info->cameraFovDeg));
            n.detail = d;
            // The eye height is where the camera SITS on a character, so the row is drawn there
            // rather than at the actor's feet -- a frustum at the origin of a 180 cm character is
            // pointing out of its ankles.
            if (info->eyeHeight > 0.1f) n.local[14] = info->eyeHeight;
            addNode(std::move(n), rootIdx);
        }
        if (info->hasPointLight) {
            ComponentNode n;
            n.kind = ComponentKind::PointLight;
            n.name = "PointLight";
            char d[96];
            std::snprintf(d, sizeof d, "%.0f lx, %.0f cm",
                          static_cast<double>(info->lightIntensityLux),
                          static_cast<double>(info->lightRangeCm));
            n.detail = d;
            addNode(std::move(n), rootIdx);
        }
    }

    for (int i = 0; i < static_cast<int>(script_.models.size()); ++i) {
        const fmt::ActorModel& m = script_.models[static_cast<usize>(i)];
        ComponentNode n;
        n.kind = ComponentKind::StaticMesh;
        n.name = m.property.empty() ? "Model" : m.property;
        n.detail = m.meshPath;
        n.modelIndex = i;
        n.drawsGeometry = true;
        composeTransform(m.pos, m.rot, m.scale, n.local);
        addNode(std::move(n), rootIdx);
    }
}

// ---------------------------------------------------------------- the Components panel

namespace {
// A colour per kind, so the tree is scannable without reading it. UE tints its component icons for
// exactly this reason: in a list of twenty rows the eye finds "the light" by colour long before it
// finds it by name.
ImVec4 kindColour(ComponentKind k) {
    switch (k) {
        case ComponentKind::Root:       return ImVec4(0.95f, 0.80f, 0.45f, 1.0f);
        case ComponentKind::StaticMesh: return ImVec4(0.62f, 0.78f, 0.95f, 1.0f);
        case ComponentKind::Capsule:    return ImVec4(0.45f, 0.85f, 0.70f, 1.0f);
        case ComponentKind::Camera:     return ImVec4(0.85f, 0.70f, 0.98f, 1.0f);
        case ComponentKind::PointLight: return ImVec4(0.98f, 0.88f, 0.45f, 1.0f);
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}
// A one-glyph stand-in for an icon. Deliberately ASCII: the editor ships no icon font for this panel
// yet, and a missing glyph renders as a box that looks like a bug rather than like a placeholder.
const char* kindGlyph(ComponentKind k) {
    switch (k) {
        case ComponentKind::Root:       return "*";
        case ComponentKind::StaticMesh: return "#";
        case ComponentKind::Capsule:    return "0";
        case ComponentKind::Camera:     return ">";
        case ComponentKind::PointLight: return "o";
    }
    return "-";
}
} // namespace

namespace {
// A DRAGGABLE DIVIDER between two columns.
//
// Written out because ImGui has no splitter widget -- the docking system has one, but that is for
// dock nodes, and these columns are children inside a single window rather than nodes. The idiom
// below is the one ImGui's own demo uses for the same problem: an invisible button is a hit region
// with press-and-hold state already tracked, so the drag is IsItemActive plus a mouse delta.
//
// `width` is read AND written: the caller owns the value across frames, which is what makes the
// split persist while a tab is open. Clamped every frame against the CURRENT available width rather
// than once when set, so shrinking the tab cannot leave a column wider than the tab itself.
bool columnSplitter(const char* id, f32 thickness, f32* width, f32 avail, f32 minSelf, f32 minOther,
                    bool* released = nullptr) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const f32 h = ImGui::GetContentRegionAvail().y;
    ImGui::InvisibleButton(id, ImVec2(thickness, h > 8.0f ? h : 8.0f));

    const bool hot = ImGui::IsItemActive() || ImGui::IsItemHovered();
    // The cursor is the only thing that tells a user a two-pixel gap is draggable at all.
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    bool moved = false;
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f) {
        *width += ImGui::GetIO().MouseDelta.x;
        moved = true;
    }
    // The RELEASE, not the movement, is when a width is worth writing to disk. Saving while dragging
    // would rewrite the file on every frame of the gesture for a value that is still changing.
    if (released) *released = ImGui::IsItemDeactivated();
    // Clamped here rather than at the drag, so a resize of the whole tab is corrected too.
    const f32 maxSelf = avail - minOther;
    if (*width < minSelf) *width = minSelf;
    if (maxSelf > minSelf && *width > maxSelf) *width = maxSelf;

    // Drawn only when hot. A permanent line between every column is chrome; a line that appears
    // under the cursor is an affordance.
    if (hot) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const f32 x = at.x + thickness * 0.5f;
        dl->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + (h > 8.0f ? h : 8.0f)),
                    ImGui::GetColorU32(ImGuiCol_SeparatorActive), 2.0f);
    }
    ImGui::SameLine(0.0f, 0.0f);
    return moved;
}
} // namespace

void ActorEditor::drawTreeNode(int idx) {
    if (idx < 0 || idx >= static_cast<int>(tree_.size())) return;
    const ComponentNode& n = tree_[static_cast<usize>(idx)];

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (n.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (idx == selectedNode_) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushStyleColor(ImGuiCol_Text, kindColour(n.kind));
    ImGui::TextUnformatted(kindGlyph(n.kind));
    ImGui::PopStyleColor();
    ImGui::SameLine();

    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(idx)), flags,
                                        "%s", n.name.c_str());
    // OpenOnArrow means a click on the LABEL is a selection rather than an expand, which is what
    // makes a tree usable as a picker. IsItemToggledOpen excludes the arrow itself.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selectedNode_ = idx;
        syncSelectionFromNode();
    }
    if (!n.detail.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", componentKindName(n.kind), n.detail.c_str());

    if (open && !n.children.empty()) {
        // A COPY of the child list, because a node's children can be reallocated out from under this
        // if anything appends to tree_ during the walk. Nothing does today; the copy costs a few
        // integers and removes the class of bug entirely.
        const std::vector<int> kids = n.children;
        for (const int c : kids) drawTreeNode(c);
        ImGui::TreePop();
    }
}

void ActorEditor::drawComponentTree(bool ownColumn) {
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    ImGui::TextDisabled("COMPONENTS");
    // In its own column the tree takes the height it is given, less the room the summary below it
    // needs. Folded into the shared column it must NOT, or it would push the class defaults and the
    // transform editor off the bottom -- which is the whole reason it is a fixed height there.
    const f32 h = ownColumn ? -(76.0f * dpi) : 190.0f * dpi;
    if (ImGui::BeginChild("##components", ImVec2(0.0f, h), ImGuiChildFlags_Borders)) {
        if (tree_.empty()) ImGui::TextDisabled("  nothing declared");
        else               drawTreeNode(0);
    }
    ImGui::EndChild();

    // What the selected row IS, spelled out under the tree. The tree gives names; this gives the
    // kind and the one fact that matters for that kind, which is what a Details panel is for.
    if (selectedNode_ >= 0 && selectedNode_ < static_cast<int>(tree_.size())) {
        const ComponentNode& n = tree_[static_cast<usize>(selectedNode_)];
        ImGui::TextColored(kindColour(n.kind), "%s", componentKindName(n.kind));
        if (!n.detail.empty()) ImGui::TextDisabled("%s", n.detail.c_str());
        if (!n.drawsGeometry && n.kind != ComponentKind::Root)
            ImGui::TextDisabled("(no geometry -- shown in the viewport as a wireframe)");
    }
}

// ---------------------------------------------------------------- the live view

void ActorEditor::rebuildLive(Engine& e) {
    liveStale_ = false;
    liveGeneration_ = g_scriptGeneration;
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
    // TWO NAMES, and they are not interchangeable. `reg` is what the registry knows the class by and
    // is the only thing aver_fw_class_find will resolve; `shown` is what a person is told. Mixing
    // them is how "BP_" ends up in a sentence a user reads.
    const std::string& reg = info->className.empty() ? info->typeName : info->className;
    const std::string shown = actorDisplayName(*info, std::filesystem::path(path_).stem().string());
    int32_t c = reg.empty() ? 0 : aver_fw_class_find(reg.c_str());
    if (!c && !info->typeName.empty() && info->typeName != reg)
        c = aver_fw_class_find(info->typeName.c_str());
    if (!c) {
        liveWhy_ = "'" + shown + "' is not loaded. Compile C#, then start the project's scripts.";
        live_ = false;
        return;
    }

    // At the ORIGIN with no overrides, so every world matrix that comes back IS the local transform
    // relative to the actor -- the same space the parsed view draws in, which is what lets the two
    // be compared by flicking the toggle rather than by reading numbers.
    const int32_t root = aver_fw_spawn_preview(c, "$ActorEditorPreview", nullptr, nullptr, nullptr);
    if (!root) {
        liveWhy_ = "'" + shown + "' would not spawn. Abstract classes cannot be instanced.";
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

    // WHAT ACTUALLY PRODUCED THE PICTURE, said precisely.
    //
    // "N model(s) built by X" was wrong for the most common case and wrong in the direction that
    // costs somebody an afternoon: a class whose only geometry is `b.Mesh` in Configure has that
    // mesh attached by the ARCHETYPE at spawn, before BuildModels is called at all. Crediting
    // BuildModels for it means a script that overrides no BuildModels still reads as though it ran
    // one, so an author looking for why their BuildModels seems to do nothing has been told it did.
    //
    // The distinction is knowable: the class's own declaration is what the parser found, and
    // anything beyond that count came from the spawn.
    const bool classDeclaresMesh = info->hasMesh;
    const usize fromBuild = liveDraws_.size() > (classDeclaresMesh ? 1u : 0u)
                          ? liveDraws_.size() - (classDeclaresMesh ? 1u : 0u) : 0u;
    char msg[256];
    if (liveCapsuleFromSource_)
        std::snprintf(msg, sizeof msg,
                      "%s built no models; the capsule shown is the source's, not the class's.",
                      shown.c_str());
    else if (fromBuild == 0 && classDeclaresMesh)
        std::snprintf(msg, sizeof msg,
                      "1 model from %s's class default. BuildModels built none.", shown.c_str());
    else if (fromBuild == 0)
        std::snprintf(msg, sizeof msg, "%s built nothing.", shown.c_str());
    else
        std::snprintf(msg, sizeof msg, "%zu model(s) built by %s's BuildModels%s.",
                      fromBuild, shown.c_str(), classDeclaresMesh ? ", plus its class default" : "");
    liveWhy_ = msg;

    // THE SENTENCE THAT PREVENTS THE BUG REPORT. A class that overrides no BuildModels and has no
    // designer region cannot show more than it declares, however long somebody stares at it -- and
    // SkyForge's Gun, whose five boxes are assembled in AttachTo at play time, is exactly that.
    // Saying so is the difference between "the editor is broken" and "that geometry is gameplay".
    liveAssemblesAtPlayTime_ = (fromBuild == 0 && script_.models.empty());
    // The LOG carries both: it is read when something is wrong, and "which registry row" is
    // exactly the question that arises then.
    AVER_INFO("[ActorEditor] live {} (registry '{}'): {} model(s), {} unnamed",
              shown, reg, liveDraws_.size(), liveUnnamed_);
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
        // A RELOAD invalidates the snapshot regardless of what the source did. The classes in the
        // registry are new types built by new code; a picture produced by the old ones is of code
        // that is no longer running, and it is the case a user is most likely to be looking at --
        // they pressed Compile precisely to see the difference.
        if (liveGeneration_ != g_scriptGeneration) liveStale_ = true;
        if (liveStale_) rebuildLive(e);
        if (live_) {
            std::vector<render::preview::PreviewDraw> copy = liveDraws_;
            g_preview->setDrawList(std::move(copy));
            if (!framed_) { g_preview->frameAll(); framed_ = true; }
            buildTree();
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
            d.selected = nodeSelected(ComponentKind::StaticMesh, -1);
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
            d.selected = nodeSelected(ComponentKind::Capsule, -1);
            draws.push_back(d);
        }
    }
    for (int i = 0; i < static_cast<int>(script_.models.size()); ++i) {
        const fmt::ActorModel& m = script_.models[static_cast<usize>(i)];
        render::preview::PreviewDraw d;
        d.mesh = g_meshes.resolve(*e.device(), m.meshPath, &d.boundsRadius);
        composeTransform(m.pos, m.rot, m.scale, d.world);
        d.selected = nodeSelected(ComponentKind::StaticMesh, i);
        draws.push_back(d);
    }
    g_preview->setDrawList(std::move(draws));
    if (!framed_) { g_preview->frameAll(); framed_ = true; }
    buildTree();
}

// ---------------------------------------------------------------- the gizmo

namespace {
// How long an axis handle is on screen, in pixels at the image's own scale. Constant in SCREEN space
// so a handle is grabbable whether the actor is a bolt or a building.
constexpr f32 kGizmoPixels = 64.0f;
constexpr f32 kGrabPixels = 10.0f;
} // namespace

bool ActorEditor::projectToScreen(const f32 world[3], ImVec2 imageSize, ImVec2& out) const {
    if (!g_preview) return false;
    f32 vp[16];
    g_preview->viewProj(vp);
    // Row-vector: v * M, the engine convention. Transposing here is the classic way to get a gizmo
    // that tracks the object until the camera turns.
    const f32 x = world[0]*vp[0] + world[1]*vp[4] + world[2]*vp[8]  + vp[12];
    const f32 y = world[0]*vp[1] + world[1]*vp[5] + world[2]*vp[9]  + vp[13];
    const f32 w = world[0]*vp[3] + world[1]*vp[7] + world[2]*vp[11] + vp[15];
    if (w <= 1e-4f) return false;   // behind the eye; there is no honest screen position
    // NDC -> PIXELS, each axis by its OWN extent.
    //
    // One extent was right only while the target was square. It no longer is: the viewport fills a
    // column of whatever shape the splitters leave it, and the projection matrix already carries
    // that aspect -- so NDC is correct and it is this mapping that was wrong. Scaling both axes by
    // the smaller extent left a handle sitting on its object at the centre of the view and drifting
    // further from it towards the edges, which reads as a gizmo that is subtly mis-calibrated rather
    // than as a projection bug.
    out.x = (x / w * 0.5f + 0.5f) * imageSize.x;
    out.y = (0.5f - y / w * 0.5f) * imageSize.y;   // screen +Y is down
    return true;
}

int ActorEditor::pickGizmoAxis(ImVec2 local, ImVec2 imageSize) const {
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

bool ActorEditor::axisTip(const fmt::ActorModel& m, int axis, ImVec2 imageSize, ImVec2& out) const {
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

void ActorEditor::drawComponentWireframes(ImVec2 topLeft, ImVec2 imageSize) const {
    if (!g_preview || tree_.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // A point in a node's own frame, through its world matrix, to the screen. Row-vector with the
    // translation in the last row, matching composeTransform and the engine everywhere else.
    auto projectLocal = [&](const ComponentNode& n, f32 lx, f32 ly, f32 lz, ImVec2& out) {
        const f32 w[3] = {
            lx*n.world[0] + ly*n.world[4] + lz*n.world[8]  + n.world[12],
            lx*n.world[1] + ly*n.world[5] + lz*n.world[9]  + n.world[13],
            lx*n.world[2] + ly*n.world[6] + lz*n.world[10] + n.world[14],
        };
        ImVec2 p;
        if (!projectToScreen(w, imageSize, p)) return false;
        out = ImVec2(topLeft.x + p.x, topLeft.y + p.y);
        return true;
    };

    for (usize i = 0; i < tree_.size(); ++i) {
        const ComponentNode& n = tree_[i];
        if (n.kind != ComponentKind::Camera && n.kind != ComponentKind::PointLight) continue;

        const bool sel = (static_cast<int>(i) == selectedNode_);
        const ImVec4 c = kindColour(n.kind);
        // Selected draws brighter and thicker rather than a different colour: the colour is the
        // component's identity in the tree, and changing it on selection would break that link.
        const ImU32 col = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, sel ? 1.0f : 0.55f));
        const f32 th = sel ? 2.0f : 1.0f;

        if (n.kind == ComponentKind::Camera) {
            // A FRUSTUM, pointing down +X, which is this engine's forward. Drawn at a fixed 60 cm
            // rather than at the real far plane: a far plane is tens of metres and would fill the
            // whole preview with lines, saying nothing about where the camera is or which way it
            // looks -- which is the entire question the wireframe answers.
            const fmt::ActorClassInfo* info = activeInfo();
            const f32 fov = (info && info->cameraFovDeg > 1.0f) ? info->cameraFovDeg : 60.0f;
            const f32 len = 60.0f;
            const f32 half = len * std::tan(fov * 0.5f * 3.14159265f / 180.0f);
            ImVec2 apex, corner[4];
            if (!projectLocal(n, 0, 0, 0, apex)) continue;
            const f32 cy[4] = {-half,  half,  half, -half};
            const f32 cz[4] = {-half, -half,  half,  half};
            bool ok = true;
            for (int k = 0; k < 4; ++k) ok = ok && projectLocal(n, len, cy[k], cz[k], corner[k]);
            if (!ok) continue;
            for (int k = 0; k < 4; ++k) {
                dl->AddLine(apex, corner[k], col, th);
                dl->AddLine(corner[k], corner[(k + 1) % 4], col, th);
            }
        } else {
            // A LIGHT'S REACH, as three orthogonal circles. One circle reads as a disc and hides
            // which plane it is in; three read as a sphere from any angle, which is what a range is.
            const fmt::ActorClassInfo* info = activeInfo();
            const f32 r = (info && info->lightRangeCm > 1.0f) ? info->lightRangeCm : 100.0f;
            constexpr int kSegments = 24;
            for (int plane = 0; plane < 3; ++plane) {
                ImVec2 prev;
                bool have = false;
                for (int k = 0; k <= kSegments; ++k) {
                    const f32 a = 6.2831853f * static_cast<f32>(k) / static_cast<f32>(kSegments);
                    const f32 u = std::cos(a) * r, v = std::sin(a) * r;
                    const f32 lx = plane == 0 ? u : (plane == 1 ? u : 0.0f);
                    const f32 ly = plane == 0 ? v : (plane == 1 ? 0.0f : u);
                    const f32 lz = plane == 0 ? 0.0f : (plane == 1 ? v : v);
                    ImVec2 p;
                    if (!projectLocal(n, lx, ly, lz, p)) { have = false; continue; }
                    if (have) dl->AddLine(prev, p, col, th);
                    prev = p;
                    have = true;
                }
            }
        }
    }
}

void ActorEditor::drawGizmo(ImVec2 topLeft, ImVec2 imageSize, const fmt::ActorModel& m) const {
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

void ActorEditor::dragAlongAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const {
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

// How far the mouse moved ALONG a handle, in fractions of that handle's screen length. Shared by the
// rotate and scale drags, and it is the same projection dragAlongAxis opens with -- dragging across a
// handle does nothing, which is what makes a single-axis gizmo feel like one.
//
// Returns false when the handle is degenerate on screen (edge-on to the camera), because a projection
// onto a zero-length direction is a division by nearly nothing and would send the value to infinity
// on a one-pixel wobble.
bool ActorEditor::dragAlong(const fmt::ActorModel& m, int axis, ImVec2 delta,
                            ImVec2 imageSize, f32& outAlong) const {
    ImVec2 origin, tip;
    if (!projectToScreen(m.pos, imageSize, origin)) return false;
    if (!axisTip(m, axis, imageSize, tip)) return false;
    const f32 dx = tip.x - origin.x, dy = tip.y - origin.y;
    const f32 len2 = dx*dx + dy*dy;
    if (len2 < 1e-4f) return false;
    outAlong = (delta.x * dx + delta.y * dy) / len2;
    return true;
}

void ActorEditor::dragRotateAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const {
    f32 along = 0.0f;
    if (!dragAlong(m, axis, delta, imageSize, along)) return;

    // WHICH component. `rot` is (yaw, pitch, roll) about (+Z, +Y, +X) applied Z-then-Y-then-X -- see
    // composeTransform at the top of this file, which is the one place that ordering is decided. So a
    // gizmo axis maps to rot[2 - axis]: X handle -> roll, Y -> pitch, Z -> yaw. Getting this backwards
    // gives a gizmo where dragging the blue handle tilts the object sideways, which reads as a broken
    // gizmo rather than as a wrong index.
    const int comp = 2 - axis;

    // A full handle length is a quarter turn. Angles are unbounded on purpose -- rot is degrees in the
    // source text and 370 and 10 are different things to a reader diffing the file, so this does not
    // wrap them behind the author's back.
    m.rot[comp] += along * 90.0f;
}

void ActorEditor::dragScaleAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const {
    f32 along = 0.0f;
    if (!dragAlong(m, axis, delta, imageSize, along)) return;

    // MULTIPLICATIVE, for the reason PreviewCamera::addZoom is: a drag should change the same
    // PROPORTION at every size, or the gesture that nudges a 2 m crate obliterates a 5 cm bolt.
    // A full handle length doubles.
    f32 factor = 1.0f + along;
    // Clamped away from zero and from negatives. A zero scale collapses the mesh to a plane and its
    // normals with it; a negative one mirrors the geometry, which inverts the winding and makes the
    // object render inside-out. Neither is something a drag should be able to reach by accident, and
    // an author who genuinely wants a mirrored placement can type it in the Details panel.
    if (factor < 0.02f) factor = 0.02f;
    if (factor > 50.0f) factor = 50.0f;
    m.scale[axis] *= factor;
    if (m.scale[axis] < 1e-4f) m.scale[axis] = 1e-4f;
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
        // The app's own split button, so this tab's Compile C# is the SAME control as the level
        // toolbar's -- status icon on its face, and the dropdown behind the arrow. It used to be a
        // plain button with the same words and none of that, which is the sort of difference that
        // makes an editor feel assembled from parts.
        if (g_hooks.drawCompileButton) {
            g_hooks.drawCompileButton();
        } else {
            // No hook installed (a test, or a build with no ToolsMenu). Better a working plain button
            // than a toolbar with a hole in it.
            const bool busy = g_hooks.compileBusy && g_hooks.compileBusy();
            ImGui::BeginDisabled(!g_hooks.compileScripts || busy);
            if (ImGui::Button(busy ? "Compiling..." : "Compile C#")) g_hooks.compileScripts();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(g_hooks.compileScripts
                    ? "Build this project's scripts, and bake its C# materials."
                    : "No project is open.");
        }

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

        // ---- the transform tools ----
        //
        // The same four as the level viewport, the same icons, the same 1-4 keys. Drawn here rather
        // than over the image because this tab already has a toolbar and a floating overlay would sit
        // on top of the preview, which is the one thing in the tab worth looking at.
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        {
            const f32 icon = ImGui::GetFrameHeight();
            const f32 dpi  = icon / 19.0f;   // the glyphs' line weights are authored against ~19px
            for (int k = ToolSelect; k <= ToolScale; ++k) {
                if (k != ToolSelect) ImGui::SameLine(0.0f, 2.0f);
                char id[16];
                std::snprintf(id, sizeof id, "##aeTool%d", k);
                if (toolButton(id, k, tool_ == k, icon, dpi)) tool_ = k;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s  (%d)", toolName(k), k + 1);
            }
        }

        // ---- Live ----
        //
        // Off by default, and deliberately. The parsed view works with nothing loaded, on a file that
        // has never compiled, in a build with no CLR; Live works only once the class is in the
        // registry. Defaulting to the fragile one would make the tab look broken in every case where
        // the robust one had something useful to show.
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Checkbox("Live", &live_)) { liveStale_ = true; framed_ = false; selected_ = -1; selectedNode_ = -1; }
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

    // ---- the layout ----
    //
    // THREE COLUMNS, the way UE's Blueprint editor lays one out: Components on the LEFT, the
    // viewport in the MIDDLE with everything that is left over, Details on the RIGHT. It was two --
    // viewport then everything else -- which put the component tree in the same column as the class
    // defaults and the transform editor, so the tree had to be short to leave them room and the
    // viewport was squeezed by a column carrying three unrelated things.
    //
    // DPI-SCALED, like everything else here. Raw pixels made the side panel a tenth of a 300%
    // display and the toolbar above it clip its own buttons.
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 avail = ImGui::GetContentRegionAvail().x;

    // SHARED by every actor tab, and PERSISTED.
    //
    // Shared rather than per-tab: dragging the split in one tab and finding a different one in the
    // next is the sort of inconsistency nobody reports as a bug and everybody finds irritating. UE
    // remembers a layout per editor TYPE, not per asset, for the same reason.
    //
    // Stored in DPI-INDEPENDENT units, which matters more than it looks: the widths are used in
    // physical pixels, so writing those to disk would make a layout set on a 300% display arrive
    // three times too wide on a 100% one. Dividing out here and multiplying back is the whole fix.
    if (g_leftColW <= 0.0f)  g_leftColW  = prefFloat(kPrefLeft,  kDefaultLeftColumn)  * dpi;
    if (g_rightColW <= 0.0f) g_rightColW = prefFloat(kPrefRight, kDefaultRightColumn) * dpi;
    f32 leftW  = g_leftColW;
    f32 rightW = g_rightColW;
    // NARROW TABS COLLAPSE RATHER THAN CLIP: below the point where the viewport would be squeezed
    // under about 260 units, the left column folds back into the right one and the tab is two
    // columns again -- which is what it was, and is still usable.
    //
    // The test states what it MEANS -- "is there still room for a viewport" -- rather than comparing
    // the side panels to some multiple of themselves, which is what it did first and which never
    // reached three columns at 300% DPI: the panels scale with DPI, so a rule phrased as a ratio
    // between them is a rule that ignores how much room there actually is.
    const bool threeColumns = (avail - leftW - rightW) >= 260.0f * dpi;
    if (!threeColumns) leftW = 0.0f;

    // The splitters are zero-spacing on both sides, so the gap the user grabs is the splitter itself
    // rather than the splitter plus ImGui's item spacing -- which would make the visible line and the
    // hit region disagree by a few pixels in a way that feels like a miss.
    const f32 split = 6.0f * dpi;
    const f32 minView = 200.0f * dpi;
    const f32 viewW = avail - leftW - rightW - (threeColumns ? split * 2.0f : split);

    // ---- the Components column ----
    if (threeColumns) {
        ImGui::BeginChild("##componentcol", ImVec2(leftW, 0.0f), ImGuiChildFlags_None);
        drawComponentTree(/*ownColumn=*/true);
        ImGui::EndChild();
        bool doneL = false;
        columnSplitter("##splitL", split, &g_leftColW, avail - rightW - split * 2.0f,
                       120.0f * dpi, minView, &doneL);
        if (doneL) { setPrefFloat(kPrefLeft, g_leftColW / dpi); flushEditorPrefs(); }
    }

    ImGui::BeginChild("##viewcol", ImVec2(viewW, 0.0f), ImGuiChildFlags_None);

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
        // FILLS THE COLUMN. The target is no longer square, so there is nothing to letterbox: the
        // image is the whole area and the projection's aspect follows the target. A square target in
        // a wide column spent most of a wide monitor's viewport on nothing.
        const ImVec2 box = ImGui::GetContentRegionAvail();
        const f32 iw = box.x < 64.0f ? 64.0f : box.x;
        const f32 ih = box.y < 64.0f ? 64.0f : box.y;
        requestPreviewSize(iw, ih);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(g_preview->uiTextureId()), ImVec2(iw, ih));
        // BOTH extents. The gizmo and the wireframes project through the same matrix the picture was
        // drawn with, and that matrix's aspect is the target's -- so the NDC-to-pixel mapping needs
        // the real width and the real height, not one number standing in for both.
        const ImVec2 s(iw, ih);

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
            // Tools 1-4, only while the pointer is over the viewport. Guarded that way because this
            // tab has text fields -- the Details panel's name and mesh boxes -- and a bare key handler
            // would eat a "2" somebody was typing into one.
            if (ImGui::IsItemHovered() && !ImGui::GetIO().WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_ = ToolSelect;
                if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_ = ToolMove;
                if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_ = ToolRotate;
                if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_ = ToolScale;
                // F frames, as it does in the level viewport.
                if (ImGui::IsKeyPressed(ImGuiKey_F) && g_preview) g_preview->frameAll();
            }

            // Select puts no handles on screen, so there is nothing to grab and a left-drag is always
            // an orbit -- which is what makes Select the tool you use to look at something.
            const bool haveSel = !live_ && tool_ != ToolSelect && selected_ >= 0
                              && selected_ < static_cast<int>(script_.models.size());
            if (haveSel && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered()) {
                const ImVec2 m = io.MousePos;
                draggingAxis_ = pickGizmoAxis(ImVec2(m.x - at.x, m.y - at.y), s);
            }
            if (!io.MouseDown[ImGuiMouseButton_Left]) draggingAxis_ = -1;

            if (draggingAxis_ >= 0 && haveSel) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                if (d.x != 0.0f || d.y != 0.0f) {
                    fmt::ActorModel& m = script_.models[static_cast<usize>(selected_)];
                    if      (tool_ == ToolMove)   dragAlongAxis(m, draggingAxis_, d, s);
                    else if (tool_ == ToolRotate) dragRotateAxis(m, draggingAxis_, d, s);
                    else if (tool_ == ToolScale)  dragScaleAxis(m, draggingAxis_, d, s);
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    dirty_ = true;
                }
            } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                g_preview->camera().addOrbit(-d.x * 0.4f, d.y * 0.4f);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            }

            // MMB PAN, the level viewport's own binding, and the motion this camera was missing: an
            // actor's interesting part is rarely its pivot, and without a pan the only way to centre a
            // muzzle or a head was to zoom out until it happened to be in frame. Right-drag pans too,
            // because on a trackpad there is no middle button.
            //
            // The RENDERED height is passed, not the widget's: they differ while a resize is still
            // being debounced, and the pixel-to-world scale must match the picture actually on screen.
            for (ImGuiMouseButton b : {ImGuiMouseButton_Middle, ImGuiMouseButton_Right}) {
                if (ImGui::IsMouseDragging(b)) {
                    const ImVec2 d = ImGui::GetMouseDragDelta(b);
                    g_preview->camera().panPixels(d.x, d.y, static_cast<f32>(g_preview->height()));
                    ImGui::ResetMouseDragDelta(b);
                }
            }

            if (io.MouseWheel != 0.0f && draggingAxis_ < 0)
                g_preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f);
        }

        // Before the gizmo, so a handle is never hidden behind a frustum line.
        drawComponentWireframes(at, s);
        // No handles under Select. A gizmo you can see but not use is worse than no gizmo, and it is
        // the only visual difference between Select and the other three.
        if (!live_ && tool_ != ToolSelect && selected_ >= 0
            && selected_ < static_cast<int>(script_.models.size()))
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

    ImGui::EndChild();   // ##viewcol
    // The RIGHT splitter moves the boundary the other way, so the delta has to be inverted: dragging
    // right must make the details column narrower, not wider. Done by splitting on a scratch value
    // and reflecting the movement, rather than by giving the widget a direction flag it would only
    // ever be passed once.
    {
        f32 mirrored = avail - g_rightColW;
        const f32 before = mirrored;
        bool doneR = false;
        // FULL avail, because `mirrored` is a POSITION measured from the left edge, not a width.
        //
        // Passing the width budget (avail - leftW - splitters) clamped a perfectly valid divider
        // position against a smaller span, so a right column narrower than about a third of the tab
        // was silently widened back out -- which is exactly what a persisted 190 turned into 512.
        // The bug was invisible while both columns sat at their defaults and appeared the moment a
        // width was restored from disk.
        //
        // The lower bound is what the columns to its LEFT need: the left column, both splitters and
        // a viewport. The upper bound is the minimum the details column itself may be, which
        // columnSplitter derives as avail - minOther.
        columnSplitter("##splitR", split, &mirrored, avail,
                       leftW + split * 2.0f + minView, 160.0f * dpi, &doneR);
        if (mirrored != before) g_rightColW = avail - mirrored;
        if (g_rightColW < 160.0f * dpi) g_rightColW = 160.0f * dpi;
        if (doneR) { setPrefFloat(kPrefRight, g_rightColW / dpi); flushEditorPrefs(); }
    }
    // EXPLICIT width, not "fill the rest".
    //
    // Filling was wrong in a way that only shows up once a width is persisted and no longer matches
    // the default: the viewport is sized as avail - left - right - splitters, but a details column
    // that takes ALL the remainder does not necessarily take `right`, so the two disagreed and the
    // viewport came out narrower than the arithmetic said. All three columns are now stated, and any
    // rounding slack lands at the right edge where nothing depends on it.
    ImGui::BeginChild("##detailscol", ImVec2(rightW, 0.0f), ImGuiChildFlags_None);
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
            std::string n = actorDisplayName(k);
            if (!k.anything()) n += "  (nothing to draw)";
            labels.push_back(std::move(n));
        }
        for (const std::string& n : labels) items.push_back(n.c_str());
        if (ImGui::Combo("Actor", &activeClass_, items.data(), static_cast<int>(items.size())))
            { selected_ = -1; selectedNode_ = -1; framed_ = false; liveStale_ = true; }
    } else if (const fmt::ActorClassInfo* k = activeInfo()) {
        // The C# type, which is what the author named the thing. The bound name is shown below as
        // bookkeeping.
        ImGui::Text("%s", actorDisplayName(*k).c_str());
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
            ImGui::TextDisabled("registry name: %s", k->className.c_str());
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

    // What this actor declares beyond geometry. It used to say "(not drawn)" of both, which was true
    // when it was written and stopped being true when the wireframes landed -- the frustum and the
    // light's three circles are on screen. A panel that describes the viewport wrongly is worse than
    // one that says nothing, because it sends somebody looking for a bug that is not there.
    if (const fmt::ActorClassInfo* k = activeInfo()) {
        if (k->hasCamera)
            ImGui::TextDisabled("camera: %.0f deg, %.0f-%.0f cm (frustum shown at a fixed 60 cm)",
                                static_cast<double>(k->cameraFovDeg),
                                static_cast<double>(k->cameraNearCm), static_cast<double>(k->cameraFarCm));
        if (k->hasPointLight)
            ImGui::TextDisabled("point light: %.0f lux, %.0f cm (range shown as three circles)",
                                static_cast<double>(k->lightIntensityLux),
                                static_cast<double>(k->lightRangeCm));
        // Only when there is genuinely nothing to show. A Character declares no mesh and is still
        // drawn -- its capsule IS its shape -- so saying "declares nothing" there is simply untrue.
        if (!k->anything() && !k->drawable() && k->hasViewport())
            ImGui::TextDisabled("This %s declares no mesh, camera or light.", fmt::actorKindName(k->kind));
    }
    ImGui::Separator();
    // ONLY when the tab is too narrow for three columns. With the left column present the tree is
    // already there, and drawing it twice would give two selections that could disagree.
    if (!threeColumns) { drawComponentTree(/*ownColumn=*/false); ImGui::Separator(); }
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
            // WRAPPED. The sentence is long by design -- it names the class and says what produced
            // the picture -- and an unwrapped one is clipped to the column, which cuts it off at
            // exactly the word that carries the meaning ("BuildModels built none").
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.60f, 1.0f));
            ImGui::TextWrapped("Live: %s", liveWhy_.c_str());
            ImGui::PopStyleColor();
            if (liveAssemblesAtPlayTime_)
                ImGui::TextWrapped("This class assembles itself at play time. Live runs the "
                                   "construction only, so it shows what the class declares and "
                                   "nothing a Begin Play would add.");
            if (liveUnnamed_ > 0)
                ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.35f, 1.0f),
                                   "%d model(s) name a mesh no file in this project matches.",
                                   liveUnnamed_);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.80f, 0.35f, 1.0f));
            ImGui::TextWrapped("Live off: %s", liveWhy_.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    ImGui::EndGroup();
    ImGui::EndChild();   // ##detailscol
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

// Window > Reset Layout, when an actor tab is the one on screen. See SandboxApp for why that menu
// item is scoped to the active tab rather than to the whole editor.
void resetActorEditorLayout() {
    // Zeroed so the next draw reseeds from the prefs -- which are being set to the defaults right
    // here, so the reseed lands on them. Writing the defaults rather than deleting the keys means a
    // reset is a value somebody can see in the file, not an absence they have to infer.
    g_leftColW = 0.0f;
    g_rightColW = 0.0f;
    setPrefFloat(kPrefLeft,  kDefaultLeftColumn);
    setPrefFloat(kPrefRight, kDefaultRightColumn);
    flushEditorPrefs();
    AVER_INFO("[ActorEditor] layout reset to the default columns");
}

void setActorEditorLiveByDefault(bool on) { g_liveDefault = on; }

void notifyActorEditorsScriptsReloaded() {
    ++g_scriptGeneration;
    AVER_INFO("[ActorEditor] scripts reloaded; live views are generation {}", g_scriptGeneration);
}

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

    // THE ESCALATION. Malformed means the region is real C# that has left the locked grammar -- a
    // named argument moved, an argument omitted, a coordinate written as an expression, a `#if`
    // around a placement. The scanner is right to decline it and Roslyn is right to read it, so the
    // retry is automatic and needs no setting: `Malformed` IS the signal.
    //
    // Only on Malformed. A file the scanner read is not re-read by a slower parser that would have
    // to agree with it anyway, and a file with no region has nothing for either to read.
#if AVER_HAVE_ROSLYN
    if (parsed.status == fmt::ActorParseStatus::Malformed && fmt::averDesignAvailable()) {
        fmt::RoslynParse rp;
        std::string why;
        if (fmt::parseActorFileRoslyn(path, rp, &why) &&
            rp.script.status == fmt::ActorParseStatus::Ok) {
            AVER_INFO("[ActorEditor] {} left the locked grammar; Roslyn read it: {} placement(s)",
                      p.filename().string(), rp.script.models.size());
            parsed = std::move(rp.script);
            // The class facts come from the same run, because they were parsed from the same bytes by
            // the same parser. Mixing one backend's classes with the other's models would be two
            // readings of one file presented as one.
            if (!rp.classes.empty()) classes = std::move(rp.classes);
        } else if (!why.empty()) {
            AVER_WARN("[ActorEditor] Roslyn could not read {} either: {}", p.filename().string(), why);
        }
    }
#endif

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
        // Declined LOUDLY, and now only after Roslyn has also been asked (or is not installed).
        // Opening a tab that shows nothing and cannot save would be worse than saying why.
#if AVER_HAVE_ROSLYN
        const char* note = fmt::averDesignAvailable() ? "" : " (averdesign is not installed)";
#else
        const char* note = " (this build has no Roslyn backend)";
#endif
        AVER_WARN("[ActorEditor] {} has a generated region neither backend could read: {}{}",
                  path, parsed.error, note);
        return nullptr;
    }
    return std::make_unique<ActorEditor>(path, std::move(parsed), std::move(classes), std::move(source));
}

} // namespace aver::editor
