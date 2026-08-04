// The actor editor tab: opens a C# actor file, previews it in 3D, and writes placements back.

#include "ActorEditor.hpp"
#include "EditorEuler.hpp"
#include "ToolGlyphs.hpp"
#include "EditorPrefs.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/ActorScript.hpp"
#if AVER_HAVE_ROSLYN
#  include "aver/formats/AverDesign.hpp"
#endif
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/runtime/Engine.hpp"

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

// State shared by every open actor tab.
render::preview::ActorPreview* g_preview = nullptr;
render::preview::PreviewMeshCache g_meshes;
std::string g_contentRoot;
bool g_previewTried = false;
rhi::IDevice* g_device = nullptr;
ActorEditorHooks g_hooks;
bool g_liveDefault = false;
f32 g_leftColW = 0.0f, g_rightColW = 0.0f;   // physical pixels; 0 means not seeded yet
constexpr f32 kDefaultLeftColumn  = 200.0f;
constexpr f32 kDefaultRightColumn = 280.0f;
constexpr const char* kPrefLeft  = "actorEditor.leftColumn";
constexpr const char* kPrefRight = "actorEditor.rightColumn";
// Bumped when the script assembly is swapped. Never reset.
u32 g_scriptGeneration = 1;

// Reads a whole file into out. Returns false if it cannot be opened.
bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
// Hashes a mesh path to an ObjectId. Must match Aver.Scene's ObjectIdOf: fnv1a64 over raw bytes.
inline u64 objectIdOf(std::string_view path) { return aver::fnv1a64(path); }

// Maps a mesh ObjectId back to a path the mesh cache can open.
class MeshNameTable {
public:
    // Points the table at a content root, discarding what was built for the old one.
    void reset(std::string root) {
        if (root == root_ && built_) return;
        root_ = std::move(root);
        byId_.clear();
        built_ = false;
    }
    // Builds the table once: the built-in meshes plus every .ocmesh under the content root.
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
            add("Content/" + rel);
        }
    }
    // Adds every string literal in the given text that names a .ocmesh.
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
    // Returns a path for an id, or null.
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

// The CMeshRenderer scene field ids.
struct MeshFields {
    int32_t mesh = 0, flags = 0;
    bool ok() const { return mesh != 0 && flags != 0; }
};

// Resolves the CMeshRenderer field ids once and returns them.
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

// Returns what to call an actor on screen: the C# type, else the bound name, else the file stem.
std::string actorDisplayName(const fmt::ActorClassInfo& k, std::string_view fileStem = {}) {
    if (!k.typeName.empty()) return k.typeName;
    if (!k.className.empty()) {
        if (k.className.rfind("BP_", 0) == 0 && k.className.size() > 3) return k.className.substr(3);
        return k.className;
    }
    return std::string(fileStem);
}

// Composes position, yaw/pitch/roll degrees and scale into a row-vector matrix.
void composeTransform(const f32 pos[3], const f32 rotDeg[3], const f32 scale[3], f32 out[16]) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 y = rotDeg[0] * kPi / 180.0f, p = rotDeg[1] * kPi / 180.0f, r = rotDeg[2] * kPi / 180.0f;
    const f32 cy = std::cos(y), sy = std::sin(y);
    const f32 cp = std::cos(p), sp = std::sin(p);
    const f32 cr = std::cos(r), sr = std::sin(r);

    // Z then Y then X, the order the framework applies them.
    const f32 m00 = cy * cp,  m01 = sy * cp,  m02 = -sp;
    const f32 m10 = cy * sp * sr - sy * cr, m11 = sy * sp * sr + cy * cr, m12 = cp * sr;
    const f32 m20 = cy * sp * cr + sy * sr, m21 = sy * sp * cr - cy * sr, m22 = cp * cr;

    out[0]  = m00 * scale[0]; out[1]  = m01 * scale[0]; out[2]  = m02 * scale[0]; out[3]  = 0.0f;
    out[4]  = m10 * scale[1]; out[5]  = m11 * scale[1]; out[6]  = m12 * scale[1]; out[7]  = 0.0f;
    out[8]  = m20 * scale[2]; out[9]  = m21 * scale[2]; out[10] = m22 * scale[2]; out[11] = 0.0f;
    out[12] = pos[0];         out[13] = pos[1];         out[14] = pos[2];         out[15] = 1.0f;
}

// The world-space direction of the placement's own local axis 0/1/2 (X/Y/Z). Deliberately the exact
// same rotation arithmetic as composeTransform's row0/row1/row2 -- that is what "local axis" means
// here, and there must be only one place that answers the question or the gizmo and the mesh it is
// drawn over can disagree again the way the rotate handle used to.
void localAxisWorldDir(const f32 rotDeg[3], int axis, f32 out[3]) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 y = rotDeg[0] * kPi / 180.0f, p = rotDeg[1] * kPi / 180.0f, r = rotDeg[2] * kPi / 180.0f;
    const f32 cy = std::cos(y), sy = std::sin(y);
    const f32 cp = std::cos(p), sp = std::sin(p);
    const f32 cr = std::cos(r), sr = std::sin(r);
    if (axis == 0)      { out[0] = cy * cp;                out[1] = sy * cp;                out[2] = -sp; }
    else if (axis == 1) { out[0] = cy*sp*sr - sy*cr;        out[1] = sy*sp*sr + cy*cr;        out[2] = cp*sr; }
    else                { out[0] = cy*sp*cr + sy*sr;        out[1] = sy*sp*cr - cy*sr;        out[2] = cp*cr; }
}

// ---------------------------------------------------------------- the component tree

// What one node in the component tree is.
enum class ComponentKind { Root, StaticMesh, Capsule, Camera, PointLight };

// Returns the display name of a component kind.
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

// One node of an actor's component tree. Children are indices into the flat node vector.
struct ComponentNode {
    std::string name;
    std::string detail;
    ComponentKind kind = ComponentKind::StaticMesh;

    int modelIndex = -1;              // index into script_.models, or -1

    f32 local[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    f32 world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    int parent = -1;
    std::vector<int> children;

    bool drawsGeometry = false;
};

// Row-vector 4x4 multiply: out = a * b.
void multiply4x4(const f32 a[16], const f32 b[16], f32 out[16]) {
    f32 t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] +
                       a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
    for (int i = 0; i < 16; ++i) out[i] = t[i];
}

// An open actor tab: the parsed source, an optional live spawn, the 3D preview and the panels.
class ActorEditor final : public AssetEditor {
public:
    // Opens a tab over an already-parsed actor file.
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

    // Latches an external change; the file is re-read on the next draw.
    void onFileChanged() override { externalChange_ = true; }

    // Draws the whole tab.
    void draw(Engine& e) override;

    // Writes the edited class defaults and placements back to the file. Returns false with a reason.
    bool save(std::string* why) override {
        if (!dirty_) return true;
        std::string out = source_;

        // ONE PASS OVER EVERY CLASS, not a loop chaining rewriteActorClass. Each class's spans are
        // byte offsets into source_ as it was last parsed; feeding class N's rewrite the ALREADY
        // rewritten output of class N-1 left class N's spans pointing at the wrong bytes the moment
        // an earlier class's edit changed the text's length -- a save touching only the first class
        // could splice the second class's values into an unrelated string literal.
        if (!classes_.empty()) {
            std::string next;
            if (!fmt::rewriteActorClasses(out, classes_, next, why)) return false;
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
        source_ = std::move(out);
        dirty_ = false;
        AVER_INFO("[ActorEditor] wrote {} placement(s) back to {}", script_.models.size(), path_);
        return true;
    }

private:
    // Rebuilds the preview's draw list from the live snapshot or the parsed source.
    void buildDrawList(Engine& e);

    // Asks the shared preview to match the panel, debounced until the size settles.
    void requestPreviewSize(f32 w, f32 h) {
        if (!g_preview) return;
        const u32 want [2] = {static_cast<u32>(w), static_cast<u32>(h)};
        const u32 have [2] = {g_preview->width(), g_preview->height()};
        const auto far_ = [](u32 a, u32 b) { return (a > b ? a - b : b - a) > 24u; };
        if (!far_(want[0], have[0]) && !far_(want[1], have[1])) { previewResizeAt_ = -1.0; return; }

        const double now = ImGui::GetTime();
        if (previewResizeAt_ < 0.0 || far_(want[0], pendingPreviewW_) || far_(want[1], pendingPreviewH_)) {
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

    // Is the selected tree row this kind and model index?
    bool nodeSelected(ComponentKind kind, int modelIndex) const {
        if (selectedNode_ < 0 || selectedNode_ >= static_cast<int>(tree_.size())) return false;
        const ComponentNode& n = tree_[static_cast<usize>(selectedNode_)];
        return n.kind == kind && n.modelIndex == modelIndex;
    }

    // ---- the component tree ----
    std::vector<ComponentNode> tree_;
    int selectedNode_ = -1;

    // Appends a node under parent, composing its world matrix. Returns its index.
    int addNode(ComponentNode n, int parent) {
        n.parent = parent;
        if (parent >= 0) multiply4x4(n.local, tree_[static_cast<usize>(parent)].world, n.world);
        else             for (int i = 0; i < 16; ++i) n.world[i] = n.local[i];
        tree_.push_back(std::move(n));
        const int idx = static_cast<int>(tree_.size()) - 1;
        if (parent >= 0) tree_[static_cast<usize>(parent)].children.push_back(idx);
        return idx;
    }

    // Rebuilds the component tree from the live snapshot or the parsed source.
    void buildTree();
    // Draws the COMPONENTS panel and the summary of the selected row.
    void drawComponentTree(bool ownColumn);
    // Draws one tree row and its children.
    void drawTreeNode(int idx);
    // Points selected_ at the model index the selected tree node carries.
    void syncSelectionFromNode() {
        selected_ = (selectedNode_ >= 0 && selectedNode_ < static_cast<int>(tree_.size()))
                  ? tree_[static_cast<usize>(selectedNode_)].modelIndex : -1;
    }

    // ---- the gizmo ----

    // Projects a world point to image pixels. Returns false when it is behind the eye.
    bool projectToScreen(const f32 world[3], ImVec2 imageSize, ImVec2& out) const;
    // Returns the gizmo axis under an image-local point, or -1.
    int  pickGizmoAxis(ImVec2 local, ImVec2 imageSize) const;
    // Draws the three axis handles over the preview image.
    void drawGizmo(ImVec2 imageTopLeft, ImVec2 imageSize, const fmt::ActorModel& m) const;
    // Draws the components that have no mesh -- a camera's frustum, a light's reach -- as wireframes.
    void drawComponentWireframes(ImVec2 imageTopLeft, ImVec2 imageSize) const;
    // Moves a placement along one axis by a mouse delta.
    void dragAlongAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const;
    // Projects a mouse delta onto an axis handle. Returns false when the handle is edge-on.
    bool dragAlong(const fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize,
                   f32& outAlong) const;
    // Rotates a placement about one axis by a mouse delta.
    void dragRotateAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const;
    // Scales a placement on one axis by a mouse delta.
    void dragScaleAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const;

    int tool_ = ToolMove;
    // Screen position of an axis handle's tip. Returns false when the axis points at the eye.
    bool axisTip(const fmt::ActorModel& m, int axis, ImVec2 imageSize, ImVec2& out) const;

    int draggingAxis_ = -1;   // -1 when not dragging

    // Records the file's current write time.
    void stamp() {
        std::error_code ec;
        stamp_ = std::filesystem::last_write_time(path_, ec);
        haveStamp_ = !ec;
    }
    // Re-reads the file when it has changed underneath. Returns true if the view was rebuilt.
    bool reloadIfChanged();

    std::string path_, title_, source_;
    fmt::ActorScript script_;
    std::vector<fmt::ActorClassInfo> classes_;
    int activeClass_ = -1;

    // Picks the class to show: the one the file is named for, else the first that draws something.
    void pickFirstPreviewable() {
        activeClass_ = -1;
        if (classes_.empty()) return;

        std::string stem = std::filesystem::path(path_).stem().string();
        if (stem.size() > 9 && stem.compare(stem.size() - 9, 9, ".Designer") == 0)
            stem.erase(stem.size() - 9);
        const auto namesFile = [&](const fmt::ActorClassInfo& k) {
            if (!stem.empty() && k.typeName == stem) return true;
            if (!stem.empty() && k.className == stem) return true;
            if (!stem.empty() && k.className.rfind("BP_", 0) == 0 &&
                k.className.compare(3, std::string::npos, stem) == 0) return true;
            return false;
        };
        for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
            if (namesFile(classes_[static_cast<usize>(i)])) { activeClass_ = i; break; }

        if (activeClass_ < 0)
            for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
                if (classes_[static_cast<usize>(i)].drawable()) { activeClass_ = i; break; }
        if (activeClass_ < 0)
            for (int i = 0; i < static_cast<int>(classes_.size()); ++i)
                if (classes_[static_cast<usize>(i)].anything()) { activeClass_ = i; break; }
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
    bool live_ = g_liveDefault;
    bool liveStale_ = true;             // rebuild on the next draw
    std::string liveWhy_;               // why it is off, or what the last rebuild found
    int liveUnnamed_ = 0;               // models whose mesh id resolved to no path this could open
    bool liveCapsuleFromSource_ = false;   // the capsule came from the source, not the spawn
    bool liveAssemblesAtPlayTime_ = false;
    u32 liveGeneration_ = 0;            // g_scriptGeneration the snapshot was built against

    bool externalChange_ = false;       // set by the watcher, cleared when the reload is done
    std::vector<render::preview::PreviewDraw> liveDraws_;
    // Spawns the class, reads the models it built back out, and destroys it.
    void rebuildLive(Engine& e);

    bool haveStamp_ = false;
    bool dirty_ = false;
    int selected_ = -1;
    bool framed_ = false;
    std::string status_;
};

// Re-reads the file when it has changed on disk. Refused while the tab is dirty.
bool ActorEditor::reloadIfChanged() {
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
    selectedNode_ = -1;
    liveStale_ = true;
    status_ = "Reloaded from disk.";
    AVER_INFO("[ActorEditor] {} changed on disk; reloaded {} placement(s)",
              path_, script_.models.size());
    return true;
}

// ---------------------------------------------------------------- the component tree

// Rebuilds the component tree: a root for the actor, then the live models or the parsed ones.
void ActorEditor::buildTree() {
    tree_.clear();
    const fmt::ActorClassInfo* info = activeInfo();

    ComponentNode root;
    root.kind = ComponentKind::Root;
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

    // ---- LIVE: the spawned subtree, flattened under the root ----
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
        if (info->hasCamera) {
            ComponentNode n;
            n.kind = ComponentKind::Camera;
            n.name = "Camera";
            char d[96];
            std::snprintf(d, sizeof d, "fov %.0f deg", static_cast<double>(info->cameraFovDeg));
            n.detail = d;
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
// Returns the tint a component kind is drawn in.
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
// Returns the one ASCII character standing in for a component kind's icon.
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
// A draggable divider between two columns. Reads and writes *width, clamped against avail.
bool columnSplitter(const char* id, f32 thickness, f32* width, f32 avail, f32 minSelf, f32 minOther,
                    bool* released = nullptr) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const f32 h = ImGui::GetContentRegionAvail().y;
    ImGui::InvisibleButton(id, ImVec2(thickness, h > 8.0f ? h : 8.0f));

    const bool hot = ImGui::IsItemActive() || ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    bool moved = false;
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f) {
        *width += ImGui::GetIO().MouseDelta.x;
        moved = true;
    }
    if (released) *released = ImGui::IsItemDeactivated();
    const f32 maxSelf = avail - minOther;
    if (*width < minSelf) *width = minSelf;
    if (maxSelf > minSelf && *width > maxSelf) *width = maxSelf;

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

// Draws one tree row and its children.
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
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selectedNode_ = idx;
        syncSelectionFromNode();
    }
    if (!n.detail.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", componentKindName(n.kind), n.detail.c_str());

    if (open && !n.children.empty()) {
        const std::vector<int> kids = n.children;
        for (const int c : kids) drawTreeNode(c);
        ImGui::TreePop();
    }
}

// Draws the COMPONENTS panel and the summary of the selected row.
void ActorEditor::drawComponentTree(bool ownColumn) {
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    ImGui::TextDisabled("COMPONENTS");
    const f32 h = ownColumn ? -(76.0f * dpi) : 190.0f * dpi;
    if (ImGui::BeginChild("##components", ImVec2(0.0f, h), ImGuiChildFlags_Borders)) {
        if (tree_.empty()) ImGui::TextDisabled("  nothing declared");
        else               drawTreeNode(0);
    }
    ImGui::EndChild();

    if (selectedNode_ >= 0 && selectedNode_ < static_cast<int>(tree_.size())) {
        const ComponentNode& n = tree_[static_cast<usize>(selectedNode_)];
        ImGui::TextColored(kindColour(n.kind), "%s", componentKindName(n.kind));
        if (!n.detail.empty()) ImGui::TextDisabled("%s", n.detail.c_str());
        if (!n.drawsGeometry && n.kind != ComponentKind::Root)
            ImGui::TextDisabled("(no geometry -- shown in the viewport as a wireframe)");
    }
}

// ---------------------------------------------------------------- the live view

// Spawns the class, reads the models it built back out, and destroys it.
void ActorEditor::rebuildLive(Engine& e) {
    liveStale_ = false;
    liveGeneration_ = g_scriptGeneration;
    liveDraws_.clear();
    liveUnnamed_ = 0;
    liveCapsuleFromSource_ = false;
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    const fmt::ActorClassInfo* info = activeInfo();
    if (!info || !e.device()) { liveWhy_ = "No class selected."; live_ = false; return; }

    // Registry name must match HostBridge.ResolveClassIdentity: the attribute's, else the type name.
    const std::string& reg = info->className.empty() ? info->typeName : info->className;
    const std::string shown = actorDisplayName(*info, std::filesystem::path(path_).stem().string());
    int32_t c = reg.empty() ? 0 : aver_fw_class_find(reg.c_str());
    if (!c && !info->typeName.empty() && info->typeName != reg)
        c = aver_fw_class_find(info->typeName.c_str());
    if (!c) {
        liveWhy_ = "'" + shown + "' is not loaded. Compile .NET, then start the project's scripts.";
        live_ = false;
        return;
    }

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
    std::vector<int32_t> stack{root};
    while (!stack.empty()) {
        const int32_t ent = stack.back();
        stack.pop_back();
        for (int32_t k = aver_scene_first_child(ent); k; k = aver_scene_next_sibling(k))
            stack.push_back(k);
        if (!mf.ok()) continue;

        const i64 id = aver_scene_get_i64(ent, mf.mesh);

        // Clears the visible bit so nothing else can draw the preview actor.
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

    // A character that built nothing still has a shape: the capsule, taken from the source.
    if (liveDraws_.empty() && info->kind == fmt::ActorKind::Character) {
        render::preview::PreviewDraw d;
        d.mesh = g_meshes.capsule(*e.device(), info->capsuleHeight, info->capsuleRadius,
                                  &d.boundsRadius);
        d.baseColor[0] = 0.45f; d.baseColor[1] = 0.62f; d.baseColor[2] = 0.85f;
        liveDraws_.push_back(d);
        liveCapsuleFromSource_ = true;
    }

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

    liveAssemblesAtPlayTime_ = (fromBuild == 0 && script_.models.empty());
    AVER_INFO("[ActorEditor] live {} (registry '{}'): {} model(s), {} unnamed",
              shown, reg, liveDraws_.size(), liveUnnamed_);
#else
    (void)e;
    liveWhy_ = "This build has no framework or scene module.";
    live_ = false;
#endif
}

// Rebuilds the preview's draw list from the live snapshot or the parsed source.
void ActorEditor::buildDrawList(Engine& e) {
    if (!g_preview || !e.device()) return;
    g_meshes.setContentRoot(*e.device(), g_contentRoot);

    if (live_) {
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

    if (const fmt::ActorClassInfo* info = activeInfo()) {
        if (info->hasMesh) {
            render::preview::PreviewDraw d;
            d.mesh = g_meshes.resolve(*e.device(), info->meshPath, &d.boundsRadius);
            d.selected = nodeSelected(ComponentKind::StaticMesh, -1);
            draws.push_back(d);
        } else if (info->kind == fmt::ActorKind::Character) {
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
// How long an axis handle is on screen, in pixels.
constexpr f32 kGizmoPixels = 64.0f;
// How near the cursor must be to grab a handle, in pixels.
constexpr f32 kGrabPixels = 10.0f;
} // namespace

// Projects a world point to image pixels. Returns false when it is behind the eye.
bool ActorEditor::projectToScreen(const f32 world[3], ImVec2 imageSize, ImVec2& out) const {
    if (!g_preview) return false;
    f32 vp[16];
    g_preview->viewProj(vp);
    const f32 x = world[0]*vp[0] + world[1]*vp[4] + world[2]*vp[8]  + vp[12];
    const f32 y = world[0]*vp[1] + world[1]*vp[5] + world[2]*vp[9]  + vp[13];
    const f32 w = world[0]*vp[3] + world[1]*vp[7] + world[2]*vp[11] + vp[15];
    if (w <= 1e-4f) return false;   // behind the eye; there is no honest screen position
    // NDC to pixels, each axis by its own extent.
    out.x = (x / w * 0.5f + 0.5f) * imageSize.x;
    out.y = (0.5f - y / w * 0.5f) * imageSize.y;   // screen +Y is down
    return true;
}

// Returns the gizmo axis under an image-local point, or -1.
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

// Screen position of an axis handle's tip. Returns false when the axis points at the eye.
bool ActorEditor::axisTip(const fmt::ActorModel& m, int axis, ImVec2 imageSize, ImVec2& out) const {
    ImVec2 origin;
    if (!projectToScreen(m.pos, imageSize, origin)) return false;
    // A world offset whose screen length is kGizmoPixels, found by projecting a unit step.
    f32 probe[3] = {m.pos[0], m.pos[1], m.pos[2]};
    if (tool_ == ToolScale) {
        // SCALE IS ALWAYS LOCAL, whatever the placement's rotation. composeTransform multiplies
        // row `axis` of the rotation matrix by scale[axis], so scale[axis] stretches along the
        // OBJECT'S OWN axis, not the world one. Drawing and picking along the world axis let the
        // arrow you dragged and the direction that actually stretched be different lines on any
        // rotated placement -- confirmed: yaw 90 degrees and the red (X) handle stretched the
        // model along world Y instead. Reusing this same function for picking (pickGizmoAxis) and
        // for the drag measurement (dragAlong) fixes all three at once, because they all call here.
        f32 dir[3];
        localAxisWorldDir(m.rot, axis, dir);
        probe[0] += dir[0]; probe[1] += dir[1]; probe[2] += dir[2];
    } else {
        probe[axis] += 1.0f;
    }
    ImVec2 unit;
    if (!projectToScreen(probe, imageSize, unit)) return false;
    const f32 dx = unit.x - origin.x, dy = unit.y - origin.y;
    const f32 len = std::sqrt(dx*dx + dy*dy);
    if (len < 1e-5f) return false;   // the axis points at the eye; there is no direction to draw
    out.x = origin.x + dx / len * kGizmoPixels;
    out.y = origin.y + dy / len * kGizmoPixels;
    return true;
}

// Draws the components that have no mesh -- a camera's frustum, a light's reach -- as wireframes.
void ActorEditor::drawComponentWireframes(ImVec2 topLeft, ImVec2 imageSize) const {
    if (!g_preview || tree_.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Projects a point in a node's own frame to the screen.
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
        const ImU32 col = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, sel ? 1.0f : 0.55f));
        const f32 th = sel ? 2.0f : 1.0f;

        if (n.kind == ComponentKind::Camera) {
            // A frustum pointing down +X, the engine's forward, drawn at a fixed 60 cm.
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
            // A light's reach, as three orthogonal circles.
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

// Draws the three axis handles over the preview image.
void ActorEditor::drawGizmo(ImVec2 topLeft, ImVec2 imageSize, const fmt::ActorModel& m) const {
    ImVec2 origin;
    if (!projectToScreen(m.pos, imageSize, origin)) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // +X red, +Y green, +Z blue.
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

// Moves a placement along one axis by a mouse delta.
void ActorEditor::dragAlongAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const {
    ImVec2 origin, tip;
    if (!projectToScreen(m.pos, imageSize, origin)) return;
    if (!axisTip(m, axis, imageSize, tip)) return;
    const f32 dx = tip.x - origin.x, dy = tip.y - origin.y;
    const f32 len2 = dx*dx + dy*dy;
    if (len2 < 1e-4f) return;

    const f32 along = (delta.x * dx + delta.y * dy) / len2;

    f32 probe[3] = {m.pos[0], m.pos[1], m.pos[2]};
    probe[axis] += 1.0f;
    ImVec2 unit;
    if (!projectToScreen(probe, imageSize, unit)) return;
    const f32 unitLen = std::sqrt((unit.x - origin.x) * (unit.x - origin.x) +
                                  (unit.y - origin.y) * (unit.y - origin.y));
    if (unitLen < 1e-5f) return;
    m.pos[axis] += along * (kGizmoPixels / unitLen);
}

// Projects a mouse delta onto an axis handle. Returns false when the handle is edge-on.
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

// Rotates a placement about one axis by a mouse delta.
//
// ABOUT THE WORLD AXIS THE HANDLE IS DRAWN ALONG, composed as a quaternion. This used to be
// `m.rot[2 - axis] += degrees`, adding straight to one Euler component -- and that is only the
// rotation the user asked for while the other two components are zero.
//
// The handles are WORLD-aligned: axisTip probes m.pos[axis] + 1, so the arrow points along world X,
// Y or Z. But rot is (yaw, pitch, roll) applied Z then Y then X, so roll turns about the object's
// OWN x axis. Yaw the placement 90 degrees, drag the world-X handle, and the model turns about
// world Y instead -- the arrow you pulled and the axis it spun about were different lines.
//
// Composing `dq * cur` applies the existing rotation first and then the new one about the world
// axis, which is the same order and the same reasoning as the level viewport's gizmo.
//
// The index swap is real and worth stating: ActorModel::rot is (yaw, pitch, roll) while
// EditorEuler works in Vec3 (roll, pitch, yaw) -- x is roll at one end and yaw at the other. The
// composition itself is identical to Rot.ToQuat on the C# side, so preview and runtime agree.
void ActorEditor::dragRotateAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const {
    f32 along = 0.0f;
    if (!dragAlong(m, axis, delta, imageSize, along)) return;
    if (axis < 0 || axis > 2) return;

    // A full handle length is a quarter turn.
    const f32 deg = along * 90.0f;
    if (std::fabs(deg) < 1e-6f) return;

    Vec3 worldAxis{0.0f, 0.0f, 0.0f};
    (&worldAxis.x)[axis] = 1.0f;

    const Quat cur = editor::quatFromEulerDeg(Vec3{m.rot[2], m.rot[1], m.rot[0]});
    const Quat dq  = Quat::fromAxisAngle(worldAxis, radians(deg));
    const Vec3 e   = editor::eulerDegFromQuat((dq * cur).normalized());
    m.rot[0] = e.z;   // yaw
    m.rot[1] = e.y;   // pitch
    m.rot[2] = e.x;   // roll
}

// Scales a placement on one axis by a mouse delta.
void ActorEditor::dragScaleAxis(fmt::ActorModel& m, int axis, ImVec2 delta, ImVec2 imageSize) const {
    f32 along = 0.0f;
    if (!dragAlong(m, axis, delta, imageSize, along)) return;

    // Multiplicative: a full handle length doubles.
    f32 factor = 1.0f + along;
    if (factor < 0.02f) factor = 0.02f;
    if (factor > 50.0f) factor = 50.0f;
    m.scale[axis] *= factor;
    if (m.scale[axis] < 1e-4f) m.scale[axis] = 1e-4f;
}

// Draws the whole tab.
void ActorEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    sharedPreview(e);

    reloadIfChanged();

    buildDrawList(e);

    // ---- the toolbar ----
    {
        if (g_hooks.drawCompileButton) {
            g_hooks.drawCompileButton();
        } else {
            const bool busy = g_hooks.compileBusy && g_hooks.compileBusy();
            ImGui::BeginDisabled(!g_hooks.compileScripts || busy);
            if (ImGui::Button(busy ? "Compiling..." : "Compile .NET")) g_hooks.compileScripts();
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
                ImGui::SetTooltip("Spawn it again. Do this after a Compile .NET, which replaces the class.");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|  preview lighting is fixed and does not match the level viewport");
    }
    ImGui::Separator();

    // ---- the layout ----
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 avail = ImGui::GetContentRegionAvail().x;

    // Column widths are shared by every actor tab and persisted in DPI-independent units.
    if (g_leftColW <= 0.0f)  g_leftColW  = prefFloat(kPrefLeft,  kDefaultLeftColumn)  * dpi;
    if (g_rightColW <= 0.0f) g_rightColW = prefFloat(kPrefRight, kDefaultRightColumn) * dpi;
    f32 leftW  = g_leftColW;
    f32 rightW = g_rightColW;
    // Below the room a viewport needs, the left column folds into the right one.
    const bool threeColumns = (avail - leftW - rightW) >= 260.0f * dpi;
    if (!threeColumns) leftW = 0.0f;

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
        ImGui::TextDisabled("This %s declares no mesh yet. Add one with b.Mesh(\"...\") in Configure.",
                            fmt::actorKindName(active->kind));
    }

    if (!noViewport && g_preview && g_preview->uiTextureId()) {
        const ImVec2 box = ImGui::GetContentRegionAvail();
        const f32 iw = box.x < 64.0f ? 64.0f : box.x;
        const f32 ih = box.y < 64.0f ? 64.0f : box.y;
        requestPreviewSize(iw, ih);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(g_preview->uiTextureId()), ImVec2(iw, ih));
        const ImVec2 s(iw, ih);

        // Orbit, zoom, and the gizmo.
        if (ImGui::IsItemHovered() || draggingAxis_ >= 0) {
            const ImGuiIO& io = ImGui::GetIO();

            // Tools 1-4 and F, only while the pointer is over the viewport.
            if (ImGui::IsItemHovered() && !ImGui::GetIO().WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_ = ToolSelect;
                if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_ = ToolMove;
                if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_ = ToolRotate;
                if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_ = ToolScale;
                if (ImGui::IsKeyPressed(ImGuiKey_F) && g_preview) g_preview->frameAll();
            }

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

            // MMB or right-drag pans. The rendered height is passed, not the widget's.
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

        drawComponentWireframes(at, s);
        if (!live_ && tool_ != ToolSelect && selected_ >= 0
            && selected_ < static_cast<int>(script_.models.size()))
            drawGizmo(at, s, script_.models[static_cast<usize>(selected_)]);
    } else if (!noViewport) {
        ImGui::TextDisabled("No 3D preview on this backend.");
    }

    // Meshes the source names and the project does not have.
    if (!g_meshes.missing().empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1.0f), "%zu mesh(es) not found:",
                           g_meshes.missing().size());
        for (const std::string& m : g_meshes.missing()) ImGui::BulletText("%s", m.c_str());
    }

    ImGui::EndChild();   // ##viewcol
    // The right splitter is mirrored: dragging right narrows the details column.
    {
        f32 mirrored = avail - g_rightColW;
        const f32 before = mirrored;
        bool doneR = false;
        columnSplitter("##splitR", split, &mirrored, avail,
                       leftW + split * 2.0f + minView, 160.0f * dpi, &doneR);
        if (mirrored != before) g_rightColW = avail - mirrored;
        if (g_rightColW < 160.0f * dpi) g_rightColW = 160.0f * dpi;
        if (doneR) { setPrefFloat(kPrefRight, g_rightColW / dpi); flushEditorPrefs(); }
    }
    ImGui::BeginChild("##detailscol", ImVec2(rightW, 0.0f), ImGuiChildFlags_None);
    ImGui::BeginGroup();

    // ---- the models ----
    // Which actor in this file, shown whenever there is more than one.
    if (classes_.size() > 1) {
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
        ImGui::Text("%s", actorDisplayName(*k).c_str());
    }

    // Class defaults: what the class states about itself.
    if (const fmt::ActorClassInfo* k = activeInfo()) {
        ImGui::TextDisabled("%s%s%s", fmt::actorKindName(k->kind),
                            k->baseType.empty() ? "" : "  :  ", k->baseType.c_str());
        if (!k->className.empty() && k->className != k->typeName)
            ImGui::TextDisabled("registry name: %s", k->className.c_str());
    }

    // ---- class defaults, EDITABLE ----
    // A control is shown only where the parse found a span, which is the byte a save writes back.
    if (fmt::ActorClassInfo* k = activeInfoMutable()) {
        bool edited = false;
        ImGui::Separator();
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

        ImGui::PopItemWidth();
        if (edited) { dirty_ = true; framed_ = false; }

        if (k->kind == fmt::ActorKind::Character && !k->capsuleHeightSpan.valid())
            ImGui::TextDisabled("capsule: %.0f x %.0f cm (framework default; this class states none)",
                                180.0, 34.0);
    }

    // What this actor declares beyond geometry.
    if (const fmt::ActorClassInfo* k = activeInfo()) {
        if (k->hasCamera)
            ImGui::TextDisabled("camera: %.0f deg, %.0f-%.0f cm (frustum shown at a fixed 60 cm)",
                                static_cast<double>(k->cameraFovDeg),
                                static_cast<double>(k->cameraNearCm), static_cast<double>(k->cameraFarCm));
        if (k->hasPointLight)
            ImGui::TextDisabled("point light: %.0f lux, %.0f cm (range shown as three circles)",
                                static_cast<double>(k->lightIntensityLux),
                                static_cast<double>(k->lightRangeCm));
        if (!k->anything() && !k->drawable() && k->hasViewport())
            ImGui::TextDisabled("This %s declares no mesh, camera or light.", fmt::actorKindName(k->kind));
    }
    ImGui::Separator();
    // Only when the tab is too narrow for three columns.
    if (!threeColumns) { drawComponentTree(/*ownColumn=*/false); ImGui::Separator(); }
    if (selected_ >= 0 && selected_ < static_cast<int>(script_.models.size())) {
        fmt::ActorModel& m = script_.models[static_cast<usize>(selected_)];
        ImGui::TextDisabled("%s", m.meshPath.c_str());
        ImGui::TextDisabled("material: %s", m.material.c_str());
        // The match key a rewrite finds the line by.
        ImGui::TextDisabled("id: 0x%016llX", static_cast<unsigned long long>(m.objectId));

        bool changed = false;
        changed |= ImGui::DragFloat3("Position (cm)", m.pos, 1.0f);
        changed |= ImGui::DragFloat3("Rotation (deg)", m.rot, 0.5f);
        changed |= ImGui::DragFloat3("Scale", m.scale, 0.01f, 0.001f, 1000.0f);
        if (changed) dirty_ = true;
    } else {
        ImGui::TextDisabled("Select a placement to edit it.");
    }

    ImGui::Separator();
    if (!liveWhy_.empty()) {
        if (live_) {
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

// Creates the shared preview on first ask, and hands it back thereafter.
render::preview::ActorPreview* sharedPreview(Engine& e) {
    if (!g_previewTried && e.device()) {
        g_previewTried = true;
        g_preview = render::preview::ActorPreview::create(*e.device(), 1024);
        if (g_preview) {
            // Registered non-owning; shutdownActorEditors removes it before deleting.
            g_device = e.device();
            g_device->addRenderFeature(g_preview);
        } else {
            AVER_WARN("[AssetEditor] no preview on this backend; the tab shows numbers only");
        }
    }
    return g_preview;
}

render::preview::PreviewMeshCache& sharedPreviewMeshes() { return g_meshes; }


// Sets the content root that mesh paths in a designer file are relative to.
void setActorEditorContentRoot(std::string root) {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    g_meshNames.reset(root);
#endif
    g_contentRoot = std::move(root);
}

void setActorEditorHooks(ActorEditorHooks hooks) { g_hooks = std::move(hooks); }

// Restores the tab's columns to their defaults and persists that.
void resetActorEditorLayout() {
    g_leftColW = 0.0f;
    g_rightColW = 0.0f;
    setPrefFloat(kPrefLeft,  kDefaultLeftColumn);
    setPrefFloat(kPrefRight, kDefaultRightColumn);
    flushEditorPrefs();
    AVER_INFO("[ActorEditor] layout reset to the default columns");
}

void setActorEditorLiveByDefault(bool on) { g_liveDefault = on; }

// Bumps the reload generation, telling every actor tab its Live view was built by dead code.
void notifyActorEditorsScriptsReloaded() {
    ++g_scriptGeneration;
    AVER_INFO("[ActorEditor] scripts reloaded; live views are generation {}", g_scriptGeneration);
}

// Releases the shared preview and its meshes. Called before the device goes.
void shutdownActorEditors() {
    if (g_device && g_preview) g_device->removeRenderFeature(g_preview);
    g_device = nullptr;
    delete g_preview;
    g_preview = nullptr;
    g_previewTried = false;
}

// Creates an actor editor for a C# actor file, else nullptr.
std::unique_ptr<AssetEditor> makeActorEditor(const std::string& path) {
    const std::filesystem::path p(path);
    if (p.extension() != ".cs") return nullptr;

    std::string source;
    if (!readFile(path, source)) return nullptr;

    fmt::ActorScript parsed = fmt::parseActorScript(source);
    std::vector<fmt::ActorClassInfo> classes = fmt::parseActorClasses(source);

    // Roslyn retries only a Malformed region: real C# that has left the locked grammar.
#if AVER_HAVE_ROSLYN
    if (parsed.status == fmt::ActorParseStatus::Malformed && fmt::averDesignAvailable()) {
        fmt::RoslynParse rp;
        std::string why;
        if (fmt::parseActorFileRoslyn(path, rp, &why) &&
            rp.script.status == fmt::ActorParseStatus::Ok) {
            AVER_INFO("[ActorEditor] {} left the locked grammar; Roslyn read it: {} placement(s)",
                      p.filename().string(), rp.script.models.size());
            parsed = std::move(rp.script);
            if (!rp.classes.empty()) classes = std::move(rp.classes);
        } else if (!why.empty()) {
            AVER_WARN("[ActorEditor] Roslyn could not read {} either: {}", p.filename().string(), why);
        }
    }
#endif

    // Any recognised actor opens, not only one with geometry.
    bool previewable = false;
    for (const fmt::ActorClassInfo& k : classes)
        if (k.anything() || k.kind != fmt::ActorKind::Unknown) previewable = true;

    // Openable with either a designer region or something the class itself declares.
    if (parsed.status == fmt::ActorParseStatus::NoRegion && !previewable) return nullptr;
    if (parsed.status == fmt::ActorParseStatus::NoRegion) {
        // No region to rewrite, so the tab is a viewer.
        return std::make_unique<ActorEditor>(path, fmt::ActorScript{}, std::move(classes), std::move(source));
    }
    if (parsed.status != fmt::ActorParseStatus::Ok) {
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
