// The .ocgraph editor's Viewport tab: the component tree, its details and the actor/material preview.
// Split from GraphEditor.cpp, which crossed the COFF section cap in Debug (sandbox/CMakeLists.txt explains why
// that means a split, not /bigobj). Same class, same includes.
#include "EditorTransform.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "EditorKeybinds.hpp"
#include "EditorPrefs.hpp"
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"
#if AVER_MODULE_FLUIDS
#  include "aver/fluids/FluidVolume.hpp"   // the seed shell a Fluid component previews
#endif
#if AVER_WITH_IMGUI
// Viewport tab only; guarded because tests/editor compiles this file with no ImGui/preview module and a three-library link (tests/editor/CMakeLists.txt).
#  include "ActorEditor.hpp"   // sharedPreview / sharedPreviewMeshes / actorEditorContentRoot
#endif

#include "aver/core/Log.hpp"
#include "aver/formats/OcInput.hpp"       // loadOcinput -- refreshSchemeActionsIfNeeded's action= picker
#include "aver/formats/OcProject.hpp"     // loadOcproject -- resolves INPUT.SCHEME for the same picker
#include "aver/platform/FileSystem.hpp"   // writeFileTextAtomic -- see save() below
#if AVER_WITH_IMGUI
#  include "aver/runtime/Engine.hpp"
#  include "aver/render/preview/ActorPreview.hpp"
#  include "aver/render/preview/PreviewMeshCache.hpp"
#  include "PreviewChrome.hpp"   // drawPreviewToolbar/drawPreviewStats/drawPreviewAxes/formatCount
#  if AVER_MODULE_PBR
// Material-graph registry: a material graph's Viewport preview shades via the same compiled graph the renderer uses, so they can't drift.
// Nested inside the ImGui guard too, since tests/editor compiles this file with neither.
#    include "aver/pbr/MaterialGraphRegistry.hpp"
#  endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#  include "imgui_internal.h"   // GImGui/ImGuiLastItemData -- see drawMaterialShapeSelector below
#endif

#include "GraphEditorInternal.hpp"

namespace aver::editor {

using namespace gedetail;

// The Viewport tab: a class graph's COMPONENT TREE and a preview of the actor it assembles. Lives
// beside the node canvas, not its own editor, since a .ocgraph with a CLASS record answers two
// questions (what the actor DOES, what it IS) in one file sharing a save path/undo/dirty flag; two
// editors would need all three duplicated.
//
// The preview is the Actor Editor's own (sharedPreview() lets every asset tab draw through one render
// feature; see ActorEditor.hpp for why a second would draw into this one's target), and
// previewComposeTransform is exported from there so a COMP-assembled actor and a designer-placed one
// cannot disagree about what "yaw 90" means.

namespace {

// The kinds this tab can create, in the order the Add menu lists them. `blurb` is the tooltip: what
// the kind is FOR, not what it is called, since the name is already on the row.
struct GraphComponentKind {
    const char* name;
    const char* blurb;
};
constexpr GraphComponentKind kGraphComponentKinds[] = {
    {"Scene",        "A named transform and nothing else -- a muzzle, a socket, an attach point."},
    {"Mesh",         "A drawn mesh. `mesh=` is content-relative, e.g. Meshes/cube.ocmesh."},
    {"SkeletalMesh", "A mesh plus the skeleton that poses it."},
    // Not "on whatever this is parented to" (a prior, false version of this blurb): GraphComponentTree.Build
    // creates ONE ENTITY PER COMP RECORD, and AnimSystem::tick poses an entity only when CAnimator and
    // CSkeletalMesh sit on the SAME one -- a parented Animator is a sibling of the mesh it means to
    // drive, so its clock ticks and nothing moves.
    {"Animator",     "A clip and the clock running it. Each COMP is its own entity, so this cannot "
                     "pose a SkeletalMesh beside it -- use the PlayAnimation node for that."},
    {"Particles",    "One emitter instance, playing a .ocparticle effect."},
    {"Camera",       "Camera parameters. Stored correctly; no renderer reads CCamera yet."},
    {"Light",        "A light. Stored correctly; no renderer reads CLight yet."},
    {"Fluid",        "A simulated fluid volume. Size comes from scale=; 1,1,1 is a 2x2x1 m pool."},
};

// The kind-specific attribute rows the details panel shows, per kind. Everything not listed here is
// still preserved in the file (extraTokens keeps it verbatim) -- this table only decides what gets a
// labelled row, so an attribute this build has never heard of survives being edited around.
struct GraphComponentAttrRow {
    const char* key;
    const char* label;
    const char* hint;
};
struct GraphComponentAttrSet {
    const char* kind;
    const GraphComponentAttrRow* rows;
    int count;
};
// Engine built-ins, not anything a template ships (these two used to read "Meshes/Blaster.ocmesh" and
// "M_Gun", FirstPerson's own gun/material, suggesting an asset only that template has). Every other row
// is an invented illustrative name; only Mesh had drifted this way, since the editor must not know a
// template exists. cube.ocmesh and M_Crate ARE real built-ins (PreviewMeshCache's
// primitive; a material SandboxApp registers), so -- unlike invented names -- they resolve in a blank project.
constexpr GraphComponentAttrRow kMeshRows[] = {
    {"mesh", "Mesh", "Meshes/cube.ocmesh"},
    {"material", "Material", "M_Crate"},
};
constexpr GraphComponentAttrRow kSkelRows[] = {
    {"mesh", "Mesh", "Meshes/Hero.ocmesh"},
    {"material", "Material", "M_Hero"},
    {"skeleton", "Skeleton", "Skeletons/Hero.ocskel"},
};
constexpr GraphComponentAttrRow kAnimRows[] = {
    {"clip", "Clip", "Anims/Idle.ocanim"},
    {"speed", "Speed", "1"},
    {"weight", "Blend weight", "1"},
};
constexpr GraphComponentAttrRow kParticleRows[] = {
    {"effect", "Effect", "Effects/Muzzle.ocparticle"},
    {"seed", "Seed", "0"},
};
constexpr GraphComponentAttrRow kCameraRows[] = {
    {"fov", "FoV (deg)", "60"},
    {"near", "Near (cm)", "10"},
    {"far", "Far (cm)", "100000"},
    {"priority", "Priority", "0"},
};
constexpr GraphComponentAttrRow kLightRows[] = {
    {"kind", "Kind", "point | spot | directional"},
    {"r", "Red", "1"}, {"g", "Green", "1"}, {"b", "Blue", "1"},
    {"intensity", "Intensity (lux)", "100000"},
    {"range", "Range (cm)", "800"},
    {"inner", "Inner cone (deg)", "0"},
    {"outer", "Outer cone (deg)", "45"},
};
// A fluid's SIZE is deliberately not here: it comes from the generic `scale=` row every component
// already has (GraphComponentTree.ApplyFluid reads 100/100/50 cm per unit).
//
// Ordered preset, then real-world values, then raw solver knobs -- the order an author reaches for them.
// `preset` resolves to the density/viscosity pair shown below it (seeing what `water` means is the
// point); the raw four are the escape hatch, listed last: `damping` alongside a preset or
// density/viscosity is REFUSED at spawn (fluids::FluidScene::spawn), not silently overridden.
constexpr GraphComponentAttrRow kFluidRows[] = {
    {"preset",     "Preset (water/oil/honey/lava)", ""},
    {"density",    "Density (kg/m^3)",              "998"},
    {"viscosity",  "Viscosity (Pa*s)",              "0.001"},
    {"compliance", "Compliance (raw)",              "1e-4"},
    {"damping",    "Damping (raw)",                 "0.1"},
    {"iterations", "Solver iterations (raw)",       "5"},
    {"pressure",   "Pressure (raw, -1 = derived)",  "-1"},
};
constexpr GraphComponentAttrSet kGraphComponentAttrs[] = {
    {"Mesh", kMeshRows, 2},
    {"SkeletalMesh", kSkelRows, 3},
    {"Animator", kAnimRows, 3},
    {"Particles", kParticleRows, 2},
    {"Camera", kCameraRows, 4},
    {"Light", kLightRows, 8},
    {"Fluid", kFluidRows, 7},
};

// The rows for a kind, or an empty set. Case-insensitive, because the parser does not care and an
// author typing `mesh` rather than `Mesh` should still get a details panel.
const GraphComponentAttrSet* attrSetFor(const std::string& kind) {
    for (const auto& set : kGraphComponentAttrs) {
        if (kind.size() != std::strlen(set.kind)) continue;
        bool same = true;
        for (usize i = 0; i < kind.size(); ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(kind[i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(set.kind[i])));
            if (a != b) { same = false; break; }
        }
        if (same) return &set;
    }
    return nullptr;
}

// Shortest round-tripping text for a number, not "%f": a component transform is written straight back,
// so `pos=12,0,-8` must stay that, not become `pos=12.000000,0.000000,-8.000000` on the first open --
// a diff on every component of every graph anyone looks at.
std::string compNum(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

std::string compVec3(const f32 v[3]) {
    return compNum(v[0]) + "," + compNum(v[1]) + "," + compNum(v[2]);
}

// Reads `x,y,z` into `out`, leaving any component it cannot read alone. Same fallback direction as
// the C# parser's own ParseVec3, and for the same reason: `out` arrives holding the identity value,
// so a half-typed scale gives 2,1,2 rather than an actor flattened on one axis.
void compParseVec3(std::string_view text, f32 out[3]) {
    int axis = 0;
    usize at = 0;
    while (axis < 3 && at <= text.size()) {
        const usize comma = text.find(',', at);
        const std::string_view part = text.substr(at, comma == std::string_view::npos ? std::string_view::npos
                                                                                       : comma - at);
        if (!part.empty()) {
            char buf[64];
            const usize n = part.size() < sizeof buf - 1 ? part.size() : sizeof buf - 1;
            std::memcpy(buf, part.data(), n);
            buf[n] = '\0';
            char* endp = nullptr;
            const double parsed = std::strtod(buf, &endp);
            if (endp != buf) out[axis] = static_cast<f32>(parsed);
        }
        ++axis;
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
}


// The three transform attributes, read off a component into arrays already holding the identity.
void compReadTransform(const fmt::OcGraphComponent& c, f32 pos[3], f32 rot[3], f32 scale[3]) {
    pos[0] = pos[1] = pos[2] = 0.0f;
    rot[0] = rot[1] = rot[2] = 0.0f;
    scale[0] = scale[1] = scale[2] = 1.0f;
    compParseVec3(fmt::componentAttr(c, "pos"), pos);
    compParseVec3(fmt::componentAttr(c, "rot"), rot);
    compParseVec3(fmt::componentAttr(c, "scale"), scale);
}

} // namespace

bool GraphEditor::componentIsAncestorOf(const std::string& maybeAncestor, const std::string& id) const {
    if (maybeAncestor == id) return true;
    std::string at = id;
    // Bounded by the component count, the same guard the parser's own cycle check uses: this runs on
    // a tree that is ALREADY valid, but it also runs mid-edit, in the frame where a reparent combo is
    // being evaluated -- which is exactly when a cycle would exist if this were the thing allowing it.
    for (usize hops = 0; hops <= graph_.components.size(); ++hops) {
        const fmt::OcGraphComponent* c = nullptr;
        for (const auto& o : graph_.components) if (o.id == at) { c = &o; break; }
        if (!c) return false;
        const std::string_view parent = fmt::componentAttr(*c, "parent");
        if (parent.empty()) return false;
        if (parent == maybeAncestor) return true;
        at = std::string(parent);
    }
    return false;
}

void GraphEditor::componentWorldMatrix(const std::string& id, float out[16]) const {
    constexpr f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(out, kIdentity, sizeof kIdentity);

    const fmt::OcGraphComponent* c = nullptr;
    for (const auto& o : graph_.components) if (o.id == id) { c = &o; break; }
    if (!c) return;

    f32 pos[3], rot[3], scale[3];
    compReadTransform(*c, pos, rot, scale);
    composeEditorTransform(pos, rot, scale, out);

    const std::string_view parent = fmt::componentAttr(*c, "parent");
    if (parent.empty()) return;
    // Recursive rather than iterative because the depth is a handful and the recursive form is the
    // one that reads as the definition: a component's world matrix is its local matrix times its
    // parent's world matrix. The parser has already proved the chain terminates.
    f32 parentWorld[16];
    componentWorldMatrix(std::string(parent), parentWorld);
    f32 combined[16];
    multiplyEditorTransform(out, parentWorld, combined);
    std::memcpy(out, combined, sizeof combined);
}

void GraphEditor::addComponent(const std::string& kind) {
    // A unique id without asking: the parser refuses duplicates, so offering `Mesh` twice and
    // producing two `mesh` records would author a file it cannot then open.
    std::string base;
    for (char ch : kind) base += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string id = base;
    for (int n = 1; n < 10000; ++n) {
        bool taken = false;
        for (const auto& c : graph_.components) if (c.id == id) { taken = true; break; }
        if (!taken) break;
        id = base + std::to_string(n);
    }

    pushUndo();
    fmt::OcGraphComponent c;
    c.id = id;
    c.kind = kind;
    // Parented to whatever is selected, which is what a component tree editor is expected to do:
    // building a hierarchy means adding under the thing just clicked, not at the root every time.
    if (!selectedComponent_.empty()) fmt::setComponentAttr(c, "parent", selectedComponent_);
    graph_.components.push_back(std::move(c));
    selectedComponent_ = id;
    dirty_ = true;
    previewFramed_ = false;
}

void GraphEditor::deleteComponentSubtree(const std::string& id) {
    if (id.empty()) return;
    bool found = false;
    for (const auto& c : graph_.components) if (c.id == id) { found = true; break; }
    if (!found) return;

    pushUndo();
    // The whole subtree, not just the one record: a child left behind would name a parent that no
    // longer exists, which the parser refuses on load. Blueprint deletes the subtree too.
    std::vector<fmt::OcGraphComponent> kept;
    kept.reserve(graph_.components.size());
    for (const auto& c : graph_.components)
        if (!componentIsAncestorOf(id, c.id)) kept.push_back(c);
    graph_.components = std::move(kept);
    if (selectedComponent_ == id || !std::any_of(graph_.components.begin(), graph_.components.end(),
                                                  [&](const fmt::OcGraphComponent& c) { return c.id == selectedComponent_; }))
        selectedComponent_.clear();
    dirty_ = true;
    previewFramed_ = false;
}

void GraphEditor::setComponentParent(const std::string& id, const std::string& parentId) {
    // Refusing rather than silently correcting: the combo below never OFFERS a descendant, so
    // reaching this with one means a caller went around the UI, and the honest answer is nothing.
    if (!parentId.empty() && componentIsAncestorOf(id, parentId)) return;
    for (auto& c : graph_.components) {
        if (c.id != id) continue;
        pushUndo();
        fmt::setComponentAttr(c, "parent", parentId);
        dirty_ = true;
        previewFramed_ = false;
        return;
    }
}

void GraphEditor::setComponentAttribute(const std::string& id, const std::string& key,
                                         const std::string& value) {
    // Same no-quoting guard the NODE attribute editor uses, and for the identical reason -- see
    // setAttribute's comment. A COMP line splits on whitespace exactly as a NODE line does, so a
    // value with a space in it truncates silently and leaves junk tokens behind.
    if (containsWhitespace(value)) return;
    for (auto& c : graph_.components) {
        if (c.id != id) continue;
        pushUndo();
        fmt::setComponentAttr(c, key, value);
        dirty_ = true;
        if (key == "pos" || key == "rot" || key == "scale") previewFramed_ = false;
        return;
    }
}

void GraphEditor::renameComponent(const std::string& id, const std::string& newId) {
    if (newId.empty() || newId == id || containsWhitespace(newId)) return;
    for (const auto& c : graph_.components) if (c.id == newId) return;   // taken

    bool found = false;
    for (const auto& c : graph_.components) if (c.id == id) { found = true; break; }
    if (!found) return;

    pushUndo();
    for (auto& c : graph_.components) {
        if (c.id == id) c.id = newId;
        // EVERY CHILD FOLLOWS. A rename that left children pointing at the old id would produce a
        // file the parser refuses -- the same unopenable-file failure deleting a subtree avoids.
        if (fmt::componentAttr(c, "parent") == id) fmt::setComponentAttr(c, "parent", newId);
    }
    if (selectedComponent_ == id) selectedComponent_ = newId;
    dirty_ = true;
}

#if AVER_WITH_IMGUI

namespace {

// The mesh Meshes/<shape>.ocmesh names for the Viewport tab's shape dropdown. Sphere/cube already
// exist in PreviewMeshCache; plane/cylinder are generated there the same way.
const char* materialPreviewMeshPath(MaterialPreviewShape shape) {
    switch (shape) {
        case MaterialPreviewShape::Cube:     return "Meshes/cube.ocmesh";
        case MaterialPreviewShape::Plane:    return "Meshes/plane.ocmesh";
        case MaterialPreviewShape::Cylinder: return "Meshes/cylinder.ocmesh";
        default:                             return "Meshes/sphere.ocmesh";
    }
}
const char* materialPreviewShapeName(MaterialPreviewShape shape) {
    switch (shape) {
        case MaterialPreviewShape::Cube:     return "Cube";
        case MaterialPreviewShape::Plane:    return "Plane";
        case MaterialPreviewShape::Cylinder: return "Cylinder";
        default:                             return "Sphere";
    }
}

// The parent window's layout and last item, put back after an out-of-flow overlay child so it adds
// no content size and leaves the image (not the overlay) as the last item for IsItemHovered below.
// Mirrors PreviewChrome.cpp's own LayoutSnapshot, which is private to that file.
struct LayoutSnapshot {
    ImVec2 cursorPos, cursorPosPrevLine, cursorMaxPos, idealMaxPos, currLineSize, prevLineSize;
    f32 currLineBase, prevLineBase;
    bool isSameLine, isSetPos;
    ImGuiLastItemData lastItem;
    explicit LayoutSnapshot(const ImGuiWindow& w)
        : cursorPos(w.DC.CursorPos), cursorPosPrevLine(w.DC.CursorPosPrevLine), cursorMaxPos(w.DC.CursorMaxPos),
          idealMaxPos(w.DC.IdealMaxPos), currLineSize(w.DC.CurrLineSize), prevLineSize(w.DC.PrevLineSize),
          currLineBase(w.DC.CurrLineTextBaseOffset), prevLineBase(w.DC.PrevLineTextBaseOffset),
          isSameLine(w.DC.IsSameLine), isSetPos(w.DC.IsSetPos), lastItem(GImGui->LastItemData) {}
    void restore(ImGuiWindow& w) const {
        w.DC.CursorPos = cursorPos; w.DC.CursorPosPrevLine = cursorPosPrevLine;
        w.DC.CursorMaxPos = cursorMaxPos; w.DC.IdealMaxPos = idealMaxPos;
        w.DC.CurrLineSize = currLineSize; w.DC.PrevLineSize = prevLineSize;
        w.DC.CurrLineTextBaseOffset = currLineBase; w.DC.PrevLineTextBaseOffset = prevLineBase;
        w.DC.IsSameLine = isSameLine; w.DC.IsSetPos = isSetPos;
        GImGui->LastItemData = lastItem;
    }
};

// A translucent "Shape: X" dropdown at the image's top-right, styled like drawPreviewToolbar's own
// mode/Show buttons -- the one control that shared toolbar has no slot for. Returns true on a change.
bool drawMaterialShapeSelector(ImVec2 imageMin, ImVec2 imageMax, f32 dpi, MaterialPreviewShape& shape) {
    if (imageMax.x - imageMin.x < 80.0f * dpi || imageMax.y - imageMin.y < 60.0f * dpi) return false;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (!win || win->SkipItems) return false;

    const ImGuiStyle& st = ImGui::GetStyle();
    char label[24];
    std::snprintf(label, sizeof label, "Shape: %s", materialPreviewShapeName(shape));
    const f32 btnW = ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2.0f + 16.0f * dpi;
    const ImVec2 size(btnW + st.WindowPadding.x * 2.0f, ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f);
    const f32 inset = 8.0f * dpi;

    const LayoutSnapshot saved(*win);
    ImGui::SetCursorScreenPos(ImVec2(imageMax.x - inset - size.x, imageMin.y + inset));
    ImVec4 bg = st.Colors[ImGuiCol_WindowBg];
    bg.w = 0.62f;   // the level viewport overlay's background alpha
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, st.WindowRounding);
    const bool open = ImGui::BeginChild("##matShapeDrop", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                            ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    bool changed = false;
    if (open) {
        if (ImGui::Button(label, ImVec2(btnW, 0.0f))) ImGui::OpenPopup("matShape");
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const f32 cx = mx.x - 8.0f * dpi - 2.0f * dpi, cy = (mn.y + mx.y) * 0.5f, s = 3.0f * dpi;
        ImGui::GetWindowDrawList()->AddTriangleFilled(
            ImVec2(cx - s, cy - s * 0.55f), ImVec2(cx + s, cy - s * 0.55f), ImVec2(cx, cy + s * 0.8f),
            ImGui::GetColorU32(ImGuiCol_Text));
        if (ImGui::BeginPopup("matShape")) {
            static constexpr MaterialPreviewShape kShapes[] = {
                MaterialPreviewShape::Sphere, MaterialPreviewShape::Cube,
                MaterialPreviewShape::Plane, MaterialPreviewShape::Cylinder,
            };
            for (MaterialPreviewShape opt : kShapes)
                if (ImGui::Selectable(materialPreviewShapeName(opt), shape == opt) && shape != opt) {
                    shape = opt;
                    changed = true;
                }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    saved.restore(*win);
    return changed;
}

} // namespace

// The Viewport tab for a material graph: a shape (Sphere/Cube/Plane/Cylinder via the dropdown over the
// image) and what the graph currently compiles to. Deliberately not a second editing surface -- changes
// happen on the Event Graph tab; this tab only answers "what does it look like" (and why, when the
// answer is "nothing").
void GraphEditor::drawMaterialViewport(Engine& e, float dpi) {
    buildComponentPreview(e);

    render::preview::ActorPreview* preview = sharedPreview(e);
#if AVER_MODULE_PBR
    // The compile error, if any, goes above the picture, not instead of it: a graph that stopped
    // compiling mid-edit still shows the last surface that worked (the id is kept) -- the message says
    // what to fix, the preview says what they had.
    if (materialPreviewGraphId_ == 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                            "This graph does not compile yet -- see the log for which node.");
    } else {
        ImGui::TextDisabled("Material graph %u -- the same compiled surface a placed .ocmat gets.",
                            materialPreviewGraphId_);
    }
#else
    ImGui::TextDisabled("Built without the PBR module, so there is no material to preview.");
#endif

    if (!preview || !preview->uiTextureId()) {
        ImGui::TextDisabled(preview ? "The preview has not rendered a frame yet."
                                     : "This backend has no GPU preview.");
        return;
    }
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const f32 texW = static_cast<f32>(preview->width());
    const f32 texH = static_cast<f32>(preview->height());
    const f32 fit = std::min(region.x / std::max(texW, 1.0f), region.y / std::max(texH, 1.0f));
    const f32 w = std::max(texW * fit, 16.0f * dpi);
    const f32 h = std::max(texH * fit, 16.0f * dpi);
    ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), ImVec2(w, h));
    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();

    render::preview::PreviewViewMode mode = preview->viewMode();
    render::preview::PreviewShowFlags show = preview->showFlags();
    if (drawPreviewToolbar("##matViewportToolbar", imageMin, imageMax, dpi, mode, show)) {
        preview->setViewMode(mode);
        preview->setShowFlags(show);
    }
    // A shape change needs a fresh frameAll(): the new mesh's bounds are not the old one's.
    if (drawMaterialShapeSelector(imageMin, imageMax, dpi, materialPreviewShape_)) previewFramed_ = false;

    std::vector<std::string> stats{std::string("Shape: ") + materialPreviewShapeName(materialPreviewShape_)};
    if (materialPreviewGraphId_ != 0) stats.push_back("Nodes: " + formatCount(graph_.nodes.size()));
    drawPreviewStats(imageMin, dpi, stats);
    drawPreviewAxes(imageMin, imageMax, dpi, preview->camera());

    // The shape turns now: without this the preview sat at whatever frameAll() picked once and never
    // moved, but a material is judged by moving it (roughness, anisotropy, clear-coat only declare
    // themselves as the highlight travels). Every sibling preview in this file and in AssetEditor
    // already wires this; it was missing here.
    //
    // Safe from the onUpdate staleness trap: an asset editor's draw runs in the UI pass, after
    // ImGui::NewFrame, so io.MouseWheel holds this frame's real value -- unlike from onUpdate, always
    // zero (see the fly-camera's note).
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f)
            preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            preview->camera().addOrbit(-d.x * 0.4f, d.y * 0.4f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            preview->camera().panPixels(d.x, d.y, static_cast<f32>(preview->height()));
        }
    }
}

void GraphEditor::drawViewport(Engine& e, float dpi) {
    // A material graph has no components, so it gets neither the toolbar nor the tree -- both would be
    // furniture for something this file cannot contain, and the toolbar's warning ("no CLASS record,
    // so nothing spawns them") would be actively misleading (a material graph isn't supposed to have one).
    const bool material = openGraphDomain() == kDomainMaterial;
    if (material) {
        drawMaterialViewport(e, dpi);
        return;
    }
    drawComponentToolbar(dpi);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float panelW = std::clamp(300.0f * dpi, 200.0f * dpi, std::max(avail.x * 0.5f, 140.0f * dpi));
    const float previewW = std::max(avail.x - panelW - ImGui::GetStyle().ItemSpacing.x, 40.0f * dpi);
    const float rowH = std::max(avail.y, 120.0f * dpi);

    ImGui::BeginChild("##compPanel", ImVec2(panelW, rowH), true);
    drawComponentTree(dpi);
    ImGui::Separator();
    drawComponentDetails(dpi);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##compPreview", ImVec2(previewW, rowH), true);
    buildComponentPreview(e);

    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !preview->uiTextureId()) {
        // NOT AN ERROR, and it says which of the two reasons it is. A headless or software backend
        // has no preview feature at all; a fresh one has no texture until its first pass runs.
        ImGui::TextDisabled(preview ? "The preview has not rendered a frame yet."
                                     : "This backend has no GPU preview -- the tree above still edits the file.");
    } else {
        const ImVec2 region = ImGui::GetContentRegionAvail();
        const f32 texW = static_cast<f32>(preview->width());
        const f32 texH = static_cast<f32>(preview->height());
        // Fit, never stretch: the preview target's aspect is its own, and letting ImGui scale it to
        // the panel would make a wide panel report a shape the actor does not have.
        const f32 fit = std::min(region.x / std::max(texW, 1.0f), region.y / std::max(texH, 1.0f));
        const ImVec2 size(std::max(texW * fit, 16.0f), std::max(texH * fit, 16.0f));
        ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), size);

        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.MouseWheel != 0.0f) preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                preview->camera().addOrbit(-d.x * 0.4f, d.y * 0.4f);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
                preview->camera().panPixels(d.x, d.y, static_cast<f32>(preview->height()));
            }
        }
    }
    ImGui::EndChild();
}

void GraphEditor::drawComponentToolbar(float dpi) {
    if (ImGui::Button("Add Component")) ImGui::OpenPopup("##addComponent");
    if (ImGui::BeginPopup("##addComponent")) {
        for (const auto& k : kGraphComponentKinds) {
            if (ImGui::MenuItem(k.name)) addComponent(k.name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.blurb);
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(selectedComponent_.empty());
    if (ImGui::Button("Delete")) deleteComponentSubtree(selectedComponent_);
    ImGui::EndDisabled();
    if (!selectedComponent_.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("Deletes '%s' and everything under it.", selectedComponent_.c_str());

    ImGui::SameLine();
    if (ImGui::Button("Frame All")) previewFramed_ = false;

    ImGui::SameLine();
    // THE CLASS RECORD IS THE POINT OF THIS TAB, stated here rather than left for someone to discover
    // when their carefully built tree spawns nothing. C++ doesn't model CLASS (it rides through as an
    // unknown record), so this asks the raw text -- the same question GameApp's ocgraphDeclaresClass asks.
    const bool declaresClass = originalText_.rfind("CLASS ", 0) == 0 ||
                               originalText_.find("\nCLASS ") != std::string::npos;
    if (declaresClass) {
        ImGui::TextDisabled("%zu component(s)", graph_.components.size());
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                            "%zu component(s) -- but this graph has no CLASS record, so nothing spawns them",
                            graph_.components.size());
    }
    (void)dpi;
}

void GraphEditor::drawComponentTree(float dpi) {
    ImGui::TextDisabled("Components");
    if (graph_.components.empty()) {
        ImGui::TextWrapped("No components. Add one to give this class a body: a Mesh to draw, a Scene "
                            "node to hang things off, a Camera to look through.");
        return;
    }

    // The actor's own entity is the root every parentless component hangs off. Drawn as a real row
    // rather than implied, because "attached to the actor itself" is a choice an author makes and a
    // tree with no visible root makes that choice look like an accident.
    const bool rootOpen = ImGui::TreeNodeEx("##compRoot", ImGuiTreeNodeFlags_DefaultOpen |
                                             ImGuiTreeNodeFlags_SpanAvailWidth, "Actor (self)");
    if (ImGui::IsItemClicked()) selectedComponent_.clear();
    if (rootOpen) {
        for (const auto& c : graph_.components)
            if (fmt::componentAttr(c, "parent").empty()) drawComponentTreeNode(c.id, dpi);
        ImGui::TreePop();
    }
}

void GraphEditor::drawComponentTreeNode(const std::string& id, float dpi) {
    const fmt::OcGraphComponent* c = nullptr;
    for (const auto& o : graph_.components) if (o.id == id) { c = &o; break; }
    if (!c) return;

    bool hasChild = false;
    for (const auto& o : graph_.components)
        if (fmt::componentAttr(o, "parent") == id) { hasChild = true; break; }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
                                ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChild) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selectedComponent_ == id) flags |= ImGuiTreeNodeFlags_Selected;

    const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", id.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selectedComponent_ = id;
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", c->kind.c_str());

    if (open && hasChild) {
        for (const auto& o : graph_.components)
            if (fmt::componentAttr(o, "parent") == id) drawComponentTreeNode(o.id, dpi);
        ImGui::TreePop();
    }
}

void GraphEditor::drawComponentDetails(float dpi) {
    if (selectedComponent_.empty()) {
        ImGui::TextDisabled("Select a component to edit it.");
        return;
    }
    const fmt::OcGraphComponent* sel = nullptr;
    for (const auto& o : graph_.components) if (o.id == selectedComponent_) { sel = &o; break; }
    if (!sel) { selectedComponent_.clear(); return; }
    const fmt::OcGraphComponent& c = *sel;

    ImGui::PushItemWidth(150.0f * dpi);

    // Same activate / apply-on-enter / discard-on-deactivate shape the node attribute rows use, keyed
    // by field and id so switching selection never bleeds one row's in-flight text into another.
    {
        const std::string rowKey = "compid\x1f" + c.id;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", (compEditRowKey_ == rowKey) ? compEditBuf_ : c.id.c_str());
        if (ImGui::InputText("Name", buf, sizeof buf)) {
            compEditRowKey_ = rowKey;
            std::snprintf(compEditBuf_, sizeof compEditBuf_, "%s", buf);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            renameComponent(c.id, compEditBuf_);
            compEditRowKey_.clear();
        } else if (ImGui::IsItemDeactivated()) {
            compEditRowKey_.clear();
        }
    }

    if (ImGui::BeginCombo("Kind", c.kind.c_str())) {
        for (const auto& k : kGraphComponentKinds) {
            const bool selected = c.kind == k.name;
            if (ImGui::Selectable(k.name, selected) && !selected) {
                // Changing a kind KEEPS the old kind's attributes (this panel just stops showing them,
                // switching back brings them straight back) -- beats destroying a mesh path over one
                // wrong combo click.
                for (auto& target : graph_.components) {
                    if (target.id != c.id) continue;
                    pushUndo();
                    target.kind = k.name;
                    dirty_ = true;
                    break;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.blurb);
        }
        ImGui::EndCombo();
    }

    {
        const std::string_view parent = fmt::componentAttr(c, "parent");
        const std::string label = parent.empty() ? "Actor (self)" : std::string(parent);
        if (ImGui::BeginCombo("Attach to", label.c_str())) {
            if (ImGui::Selectable("Actor (self)", parent.empty())) setComponentParent(c.id, "");
            for (const auto& o : graph_.components) {
                // A descendant is not offered -- cycles are prevented, not detected: the parser refuses
                // one on load, so allowing it here would author a file that can't reopen.
                if (componentIsAncestorOf(c.id, o.id)) continue;
                if (ImGui::Selectable(o.id.c_str(), parent == o.id)) setComponentParent(c.id, o.id);
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // Written back only on release (IsItemDeactivatedAfterEdit), NOT every frame of the drag: each
    // write pushes an undo entry, so a live write would fill the undo stack with one entry per frame
    // and make Ctrl-Z useless for exactly the edit most likely to need it.
    f32 pos[3], rot[3], scale[3];
    compReadTransform(c, pos, rot, scale);
    const struct { const char* label; f32* v; const char* fmtStr; const char* key; } rows[] = {
        {"Position", pos, "%.1f", "pos"},
        {"Rotation", rot, "%.1f", "rot"},
        {"Scale", scale, "%.3f", "scale"},
    };
    ImGui::PushItemWidth(190.0f * dpi);
    for (const auto& row : rows) {
        ImGui::DragFloat3(row.label, row.v, 0.5f, 0.0f, 0.0f, row.fmtStr);
        if (ImGui::IsItemDeactivatedAfterEdit()) setComponentAttribute(c.id, row.key, compVec3(row.v));
    }
    ImGui::PopItemWidth();
    ImGui::TextDisabled("cm  |  degrees, yaw/pitch/roll  |  multiplier");

    if (const GraphComponentAttrSet* set = attrSetFor(c.kind)) {
        ImGui::Separator();
        for (int i = 0; i < set->count; ++i) {
            const GraphComponentAttrRow& row = set->rows[i];
            const std::string rowKey = std::string("compattr\x1f") + c.id + "\x1f" + row.key;
            const std::string_view current = fmt::componentAttr(c, row.key);
            char buf[256];
            std::snprintf(buf, sizeof buf, "%.*s",
                          (compEditRowKey_ == rowKey) ? 0 : static_cast<int>(current.size()),
                          current.data());
            if (compEditRowKey_ == rowKey) std::snprintf(buf, sizeof buf, "%s", compEditBuf_);

            ImGui::PushID(row.key);
            if (ImGui::InputTextWithHint(row.label, row.hint, buf, sizeof buf)) {
                compEditRowKey_ = rowKey;
                std::snprintf(compEditBuf_, sizeof compEditBuf_, "%s", buf);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                setComponentAttribute(c.id, row.key, compEditBuf_);
                compEditRowKey_.clear();
            } else if (ImGui::IsItemDeactivated()) {
                compEditRowKey_.clear();
            }
            ImGui::PopID();
        }
    }

    ImGui::PopItemWidth();
}

// The seed shell for a Fluid component, uploaded once per distinct size and cached. Shows the SEED
// SHELL, not the simulated surface: the solver advances only inside aver_phys_step, gated on Play
// (docs/GAME-LIFT.md's tested invariant: no play session -> step count 0), so it never runs in edit mode.
// generateFluidSeedShell is pure arithmetic over the desc -- no solver, device or gate -- so it shows
// exactly what the component chose, without misrepresenting motion it isn't simulating.
//
// Drawn through the component matrix, unlike the runtime path (drawn at IDENTITY): FluidScene's buffer
// holds ABSOLUTE WORLD positions from the solver, while a seed shell spans -halfExtent..+halfExtent about
// the LOCAL origin. The desc passes centre zero so the component matrix is the only place position is applied.
bool GraphEditor::buildFluidPreviewMesh(Engine& e, const fmt::OcGraphComponent& c,
                                        render::preview::PreviewDraw& out) {
#if AVER_MODULE_FLUIDS
    fluids::FluidVolumeDesc fd;
    fd.centreCm[0] = fd.centreCm[1] = fd.centreCm[2] = 0.0f;
    // The same cm-per-unit ApplyFluid uses (GraphComponentTree.cs), so what the preview shows and
    // what the game spawns are one number, not two that can drift.
    f32 pos[3], rot[3], scale[3];
    compReadTransform(c, pos, rot, scale);
    fd.halfExtentCm[0] = 100.0f * scale[0];
    fd.halfExtentCm[1] = 100.0f * scale[1];
    fd.halfExtentCm[2] = 50.0f  * scale[2];

    char key[96];
    std::snprintf(key, sizeof key, "$fluid/%.2f/%.2f/%.2f/%d/%d/%d",
                  static_cast<double>(fd.halfExtentCm[0]), static_cast<double>(fd.halfExtentCm[1]),
                  static_cast<double>(fd.halfExtentCm[2]),
                  fd.subdivisions[0], fd.subdivisions[1], fd.subdivisions[2]);
    const std::string k = key;

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    // The shell is built INSIDE the cache's callback, so a size already uploaded never generates one
    // again -- which is what keeps this off the per-frame cost of rebuilding the draw list.
    out.mesh = meshes.generated(*e.device(), k,
        [&fd](std::vector<rhi::MeshVertex>& verts, std::vector<u32>& indices) {
            fluids::FluidVolume vol(fd);
            vol.generateSeedShell();
            const std::vector<f32>& vp = vol.positionsCm();
            const std::vector<f32>& nrm = vol.normals();
            const std::vector<i32>& idx = vol.indices();
            verts.resize(vp.size() / 3);
            for (usize i = 0; i < verts.size(); ++i) {
                verts[i].px = vp[i * 3 + 0]; verts[i].py = vp[i * 3 + 1]; verts[i].pz = vp[i * 3 + 2];
                verts[i].nx = nrm[i * 3 + 0]; verts[i].ny = nrm[i * 3 + 1]; verts[i].nz = nrm[i * 3 + 2];
                verts[i].u = 0.0f; verts[i].v = 0.0f;
            }
            indices.assign(idx.begin(), idx.end());
        }, &out.boundsRadius);
    if (out.mesh == 0) return false;
    // The same still-water blue the transparent pass settles on, so the preview and the level read
    // as the same substance even though one of them is a static shell.
    out.baseColor[0] = 0.10f; out.baseColor[1] = 0.30f; out.baseColor[2] = 0.36f; out.baseColor[3] = 1.0f;
    out.roughness = 0.12f;   // the value the fluid's own transparent pass shades with
    return true;
#else
    // No fluids module, no seed shell: fluids::FluidVolumeDesc/FluidVolume aren't on this configuration's
    // path (see the include guard above). buildComponentPreview treats a false return as "nothing to
    // draw", same as an unresolved Mesh file -- a Fluid component just draws nothing here.
    (void)e; (void)c; (void)out;
    return false;
#endif
}

// The Viewport tab for a MATERIAL graph: one primitive, shaded by this graph, picked by the tab's own
// shape dropdown (materialPreviewShape_; Sphere by default) -- not the component tree, since a material
// graph has no entities, only a surface to see. The default sphere shows every normal-to-view angle at
// once (Fresnel, roughness, normal maps); the other three matter for a graph whose look depends on flat
// faces or a seam.
//
// Compiled through the SAME registry the renderer uses, keyed on this file's path -- the preview is the
// renderer's own generated function, not a separate approximation that could drift. Recompiled only when
// dirty AND the emitted text differ (an idle editor asks nothing of the shader compiler); a graph that
// fails to compile mid-edit keeps the last good id, so the preview never goes black between valid states.
bool GraphEditor::buildMaterialPreview(Engine& e, render::preview::PreviewDraw& out) {
#if AVER_MODULE_PBR
    if (path_.empty()) return false;

    // Re-register only when edited since last time; the registry then decides if anything changed, so
    // a save-less edit emitting identical text costs one graph compile, no shader compile.
    //
    // Undo-stack depth is the edit counter: every edit path calls pushUndo() first, so it moves exactly
    // when the graph does. A plain dirty_ flag wouldn't -- it latches true once and never resets, so
    // the preview would recompile once and never again.
    const i64 mark = static_cast<i64>(history_.undoCount()) - static_cast<i64>(history_.redoCount());
    if (materialPreviewDirtyMark_ != mark) {
        materialPreviewDirtyMark_ = mark;
        const u32 id = pbr::materialGraphs().add(path_, graph_.name, graph_);
        if (id != 0) materialPreviewGraphId_ = id;
    }
    if (materialPreviewGraphId_ == 0) return false;

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    out.mesh = meshes.resolve(*e.device(), materialPreviewMeshPath(materialPreviewShape_), &out.boundsRadius);
    if (out.mesh == 0) return false;

    // Every preview primitive is unit-sized (the sphere's original convention); 60 matches the level
    // editor's placed cubes, the scale the orbit camera's framing was tuned against.
    for (int i = 0; i < 16; ++i) out.world[i] = 0.0f;
    out.world[0] = out.world[5] = out.world[10] = 60.0f;
    out.world[15] = 1.0f;

    // White, because the GRAPH decides the colour. A tint here would multiply into everything the
    // author sees (averBuildSurface applies gBaseColor) and quietly misreport their own values.
    out.baseColor[0] = out.baseColor[1] = out.baseColor[2] = out.baseColor[3] = 1.0f;
    out.materialGraphId = materialPreviewGraphId_;
    return true;
#else
    (void)e; (void)out;
    return false;
#endif
}

void GraphEditor::buildComponentPreview(Engine& e) {
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !e.device()) return;

    // A MATERIAL GRAPH TAKES A DIFFERENT PREVIEW ENTIRELY -- see buildMaterialPreview. It shares
    // this feature and this tab, and nothing else with the component tree below.
    if (openGraphDomain() == kDomainMaterial) {
        std::vector<render::preview::PreviewDraw> draws;
        render::preview::PreviewDraw d;
        const bool havePrimitive = buildMaterialPreview(e, d);
        if (havePrimitive) draws.push_back(d);
        preview->setDrawList(std::move(draws));
        // Framed only once there is something to frame: frameAll() fits the camera to the current draw
        // list, empty on the tab's first frames before the graph registers and its shape resolves.
        // Latching previewFramed_ on an empty list would burn the one automatic framing on nothing,
        // leaving the camera aimed at empty space for the rest of the session -- reading as "broken"
        // rather than "not yet aimed".
        if (!previewFramed_ && havePrimitive) {
            preview->frameAll();
            previewFramed_ = true;
        }
        return;
    }

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    setSharedMeshRoot(*e.device(), actorEditorContentRoot());

    std::vector<render::preview::PreviewDraw> draws;
    draws.reserve(graph_.components.size());
    for (const auto& c : graph_.components) {
        render::preview::PreviewDraw d;

        // A fluid has no mesh to name, so it can't come through the path below: its geometry is
        // generated arithmetic (fluids::generateFluidSeedShell), not a file -- the one component kind
        // the editor has to build itself.
        if (detail::ciEquals(c.kind, "Fluid")) {
            if (!buildFluidPreviewMesh(e, c, d)) continue;
        } else {
            const std::string_view meshPath = fmt::componentAttr(c, "mesh");
            // Only what has geometry: a Scene node, Camera or Animator has nothing to draw, and a
            // placeholder box would make the preview disagree with the game. Their transforms are
            // still real, just invisible here too.
            if (meshPath.empty()) continue;
            d.mesh = meshes.resolve(*e.device(), meshPath, &d.boundsRadius);
            if (d.mesh == 0) continue;   // missing or unloadable; meshes.missing() already records it
        }
        componentWorldMatrix(c.id, d.world);
        d.selected = (c.id == selectedComponent_);
        draws.push_back(d);
    }

    preview->setDrawList(std::move(draws));
    if (!previewFramed_) {
        preview->frameAll();
        previewFramed_ = true;
    }
}

#else   // !AVER_WITH_IMGUI

void GraphEditor::drawViewport(Engine&, float) {}
void GraphEditor::drawMaterialViewport(Engine&, float) {}
void GraphEditor::drawComponentToolbar(float) {}
void GraphEditor::drawComponentTree(float) {}
void GraphEditor::drawComponentTreeNode(const std::string&, float) {}
void GraphEditor::drawComponentDetails(float) {}
void GraphEditor::buildComponentPreview(Engine&) {}
bool GraphEditor::buildMaterialPreview(Engine&, render::preview::PreviewDraw&) { return false; }

#endif  // AVER_WITH_IMGUI

} // namespace aver::editor
