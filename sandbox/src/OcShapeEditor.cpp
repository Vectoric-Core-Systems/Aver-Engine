// The .ocshape editor tab. See the header for the SandboxApp hook and OcShapeEditorModel.hpp for the rules the widgets draw.
#include "OcShapeEditor.hpp"

#include "CollisionShapeOverlay.hpp"
#include "ProjectScaffold.hpp"

#include "EditorKeybinds.hpp"
#include "aver/core/Log.hpp"
#include "aver/game/ColliderShapes.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_shape_builder_abi.h"
#endif

#if AVER_WITH_IMGUI
#  include "EditorIcons.hpp"
#  include "imgui.h"
#endif

namespace aver::editor {

namespace sm = shapemodel;
using fmt::OcShapeData;
using fmt::ShapeNode;
using fmt::ShapeNodeKind;

bool writeNewShapeStarter(const std::string& path, std::string* why) {
    return writeNewFile(path, sm::starterText(), why);
}

OcShapeEditor::OcShapeEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

OcShapeEditor::~OcShapeEditor() {
    CollisionShapePreview& p = collisionShapePreview();
    if (p.owner == this) { p.active = false; p.owner = nullptr; p.triangles.clear(); p.surfaces.clear(); p.note.clear(); }
}

void OcShapeEditor::loadFromDisk() {
    std::string text, err;
    OcShapeData parsed;
    if (!readFileText(path_, text)) {
        loaded_ = false;
        loadError_ = "could not read the file";
        return;
    }
    if (!fmt::parseOcShape(text, parsed, &err)) {
        loaded_ = false;
        loadError_ = err;
        return;
    }
    shape_ = std::move(parsed);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();
    gestureOpen_ = false;
    ++revision_;
    if (!shape_.find(selectedId_)) selectedId_ = shape_.root;
}

std::string OcShapeEditor::title() const {
    // No manual dirty marker: the host applies ImGuiWindowFlags_UnsavedDocument for every editor whose dirty() is true (AssetEditor.cpp).
    return std::filesystem::path(path_).filename().string() + "###ocshape:" + path_;
}

bool OcShapeEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // Never write what the loader would refuse: the game would drop the shape with one log line.
    const std::vector<std::string> problems = fmt::validateOcShape(shape_);
    if (!problems.empty()) {
        if (why) *why = problems.front();
        return false;
    }
    if (!writeFileTextAtomic(path_, fmt::writeOcShape(shape_))) {
        if (why) *why = "could not write the file";
        return false;
    }
    dirty_ = false;
    return true;
}

void OcShapeEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS: reloading here would discard what the author typed because something else touched the file.
    if (dirty_) {
        AVER_WARN("[OcShapeEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void OcShapeEditor::undo() {
    if (!history_.undo(shape_)) return;
    markDirty();
    gestureOpen_ = false;
    if (!shape_.find(selectedId_)) selectedId_ = shape_.root;
}

void OcShapeEditor::redo() {
    if (!history_.redo(shape_)) return;
    markDirty();
    gestureOpen_ = false;
    if (!shape_.find(selectedId_)) selectedId_ = shape_.root;
}

// The shape built into a throw-away static body, its triangles read back, the body and the handle released again. No physics step runs
// in between, so the body is never simulated or seen by anything else. Rebuilt only when the shape changed.
void OcShapeEditor::publishPreview() {
    CollisionShapePreview& p = collisionShapePreview();
    if (p.owner == this && previewRevision_ == revision_) { p.active = loaded_ && !p.triangles.empty(); return; }
    previewRevision_ = revision_;
    p.owner = this;
    p.active = false;
    p.triangles.clear();
    p.surfaces.clear();
    p.note.clear();
    if (!loaded_) return;
#if AVER_MODULE_PHYSICS
    if (!aver_phys_ready()) { p.note = "No preview: the physics world is not running."; return; }
    if (!fmt::validateOcShape(shape_).empty()) { p.note = "No preview while the shape has issues."; return; }
    int32_t handle = 0;
    std::string why;
    if (!game::buildColliderShape(shape_, game::colliderShapeHost().meshes, handle, why)) { p.note = "No preview: " + why; return; }
    const int32_t body = aver_phys_body_from_shape(handle, nullptr, nullptr, AVER_PHYS_MOTION_STATIC, 0.0f, nullptr, 0);
    aver_phys_shape_release(handle);   // the body keeps the shape alive
    if (!body) { p.note = "No preview: the physics module refused a body."; return; }
    constexpr int kMax = 6000;
    p.triangles.resize(static_cast<usize>(kMax) * 9);
    p.surfaces.resize(static_cast<usize>(kMax));
    const int n = aver_phys_body_triangles(body, nullptr, nullptr, p.triangles.data(), p.surfaces.data(), kMax);
    aver_phys_remove_body(body);
    p.triangles.resize(static_cast<usize>(std::max(n, 0)) * 9);
    p.surfaces.resize(static_cast<usize>(std::max(n, 0)));
    p.active = n > 0;
    if (n <= 0) p.note = "The shape has no triangles to draw.";
    else if (n >= kMax) p.note = "The preview shows the first 6000 triangles.";
#else
    p.note = "No preview: this build has no physics module.";
#endif
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

namespace {

const ImVec4 kRed{0.95f, 0.35f, 0.25f, 1.0f};
const ImVec4 kDim{0.6f, 0.6f, 0.65f, 1.0f};

const ShapeNodeKind kLeafKinds[] = {
    ShapeNodeKind::Box, ShapeNodeKind::Sphere, ShapeNodeKind::Capsule, ShapeNodeKind::Cylinder, ShapeNodeKind::TaperedCapsule,
    ShapeNodeKind::TaperedCylinder, ShapeNodeKind::Triangle, ShapeNodeKind::Plane, ShapeNodeKind::Empty, ShapeNodeKind::HullPoints,
    ShapeNodeKind::HullMesh, ShapeNodeKind::Mesh,
};
const ShapeNodeKind kDecoratorKinds[] = {ShapeNodeKind::RotatedTranslated, ShapeNodeKind::OffsetCom, ShapeNodeKind::Scaled};

} // namespace

void OcShapeEditor::gesture(bool changed, const std::function<void()>& write) {
    if (ImGui::IsItemActivated() && !gestureOpen_) {
        gestureBefore_ = shape_;
        gestureOpen_ = true;
    }
    bool oneShot = false;
    if (changed) {
        if (!gestureOpen_) { gestureBefore_ = shape_; oneShot = true; }   // a typed value or a keyboard step: no activation seen
        write();
        markDirty();
        if (oneShot) history_.push(gestureBefore_);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && gestureOpen_) {
        history_.push(gestureBefore_);
        gestureOpen_ = false;
    } else if (ImGui::IsItemDeactivated() && gestureOpen_) {
        gestureOpen_ = false;   // clicked, changed nothing
    }
}

void OcShapeEditor::instant(bool changed, const std::function<void()>& write) {
    if (!changed) return;
    history_.push(shape_);
    write();
    markDirty();
}

bool OcShapeEditor::dragF(const char* label, f32& field, f32 speed, f32 lo, f32 hi, const char* format) {
    f32 v = field;
    const bool c = ImGui::DragFloat(label, &v, speed, lo, hi, format);
    gesture(c, [&] { field = v; });
    return c;
}

bool OcShapeEditor::text(const char* label, std::string& field, const char* hint) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", field.c_str());
    const bool c = ImGui::InputText(label, buf, sizeof buf);
    gesture(c, [&] { field = buf; });
    if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
    return c;
}

void OcShapeEditor::draw(Engine&) {
    if (!loaded_) {
        ImGui::TextColored(kRed, "Could not load %s", path_.c_str());
        ImGui::TextWrapped("%s", loadError_.c_str());
        return;
    }
    if (!shape_.find(selectedId_)) selectedId_ = shape_.root;
    drawToolbar();
    drawHeader();
    drawIssues();
    ImGui::Separator();
    drawOutline();
    publishPreview();
    drawPreviewNote();
}

void OcShapeEditor::drawToolbar() {
    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[OcShapeEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && editor::keybinds().pressed(editor::CommandId::EditUndo, io)) undo();
            if (canRedo() && editor::keybinds().pressed(editor::CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu node(s)   edits apply to bodies built after the save (the next Play)", shape_.nodes.size());
}

void OcShapeEditor::drawHeader() {
    text("Name", shape_.name);
    dragF("Mass (kg)", shape_.massKg, 0.5f, 0.0f, 100000.0f, shape_.massKg > 0.0f ? "%.2f" : "density x volume");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = the shape's density times its volume. A Rigid Body with its own mass wins over this.");
    text("Default surface", shape_.surface, "A physical surface name (Project Settings > Physics > Surfaces) for leaves that name none.");
}

void OcShapeEditor::drawIssues() {
    const std::vector<std::string> issues = fmt::validateOcShape(shape_);
    char header[64];
    std::snprintf(header, sizeof header, "Validation (%zu)###issues", issues.size());
    if (!ImGui::CollapsingHeader(header, issues.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (issues.empty()) { ImGui::TextDisabled("No problems."); return; }
    for (const std::string& i : issues) ImGui::TextColored(kRed, "error: %s", i.c_str());
    ImGui::TextColored(kRed, "Save is refused until the errors are fixed.");
}

void OcShapeEditor::drawPreviewNote() {
    const CollisionShapePreview& p = collisionShapePreview();
    if (!p.note.empty()) ImGui::TextDisabled("%s", p.note.c_str());
    else if (p.active) ImGui::TextDisabled("The wire in the viewport is this shape, at the selected entity (or the world origin). Show > Collision Shapes is not needed.");
}

void OcShapeEditor::drawOutline() {
    const f32 total = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##outline", ImVec2(total * 0.42f, 0.0f), true);

    if (ImGui::Button("Add node")) ImGui::OpenPopup("##addnode");
    if (ImGui::BeginPopup("##addnode")) {
        for (const ShapeNodeKind k : kLeafKinds)
            if (ImGui::MenuItem(sm::kindLabel(k))) {
                std::string id;
                edit([&](OcShapeData& d) { id = sm::addLeaf(d, k); });
                if (!id.empty()) selectedId_ = id;
            }
        ImGui::Separator();
        if (ImGui::MenuItem("Compound (static)")) { std::string id; edit([&](OcShapeData& d) { id = sm::addCompound(d, false); }); if (!id.empty()) selectedId_ = id; }
        if (ImGui::MenuItem("Compound (mutable)")) { std::string id; edit([&](OcShapeData& d) { id = sm::addCompound(d, true); }); if (!id.empty()) selectedId_ = id; }
        ImGui::EndPopup();
    }
    ImGui::Separator();

    const std::vector<sm::OutlineRow> rows = sm::outline(shape_);
    for (usize i = 0; i < rows.size(); ++i) {
        const sm::OutlineRow& r = rows[i];
        const ShapeNode& n = shape_.nodes[r.node];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Indent(static_cast<f32>(r.depth) * 14.0f);
        std::string label = n.id + "   " + sm::describe(shape_, n);
        if (n.id == shape_.root) label += "   [root]";
        if (r.repeated) label += "   (again)";
        if (r.orphan) label += "   (not used)";
        if (r.orphan) ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (ImGui::Selectable(label.c_str(), n.id == selectedId_)) selectedId_ = n.id;
        if (r.orphan) ImGui::PopStyleColor();
        ImGui::Unindent(static_cast<f32>(r.depth) * 14.0f);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##props", ImVec2(0.0f, 0.0f), true);
    if (ShapeNode* sel = shape_.find(selectedId_)) drawNode(*sel);
    else ImGui::TextDisabled("Select a node.");
    ImGui::EndChild();
}

void OcShapeEditor::drawNode(ShapeNode& n) {
    ImGui::PushID(n.id.c_str());
    const std::string id = n.id;   // `n` may dangle after a structural edit: use the copy from here on
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", sm::kindLabel(n.kind));

    // Rename (Enter applies): references follow.
    static char s_rename[80] = {};
    static std::string s_renameFor;
    if (s_renameFor != id) { s_renameFor = id; std::snprintf(s_rename, sizeof s_rename, "%s", id.c_str()); }   // the box keeps what is typed until the selection moves
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputText("Id", s_rename, sizeof s_rename, ImGuiInputTextFlags_EnterReturnsTrue) && id != s_rename) {
        bool ok = false;
        const std::string to = s_rename;
        OcShapeData probe = shape_;
        if (sm::renameNode(probe, id, to)) edit([&](OcShapeData& d) { ok = sm::renameNode(d, id, to); });
        if (ok) selectedId_ = to;
        ImGui::PopID();
        return;
    }

    if (fmt::shapeKindIsLeaf(n.kind)) {
        if (n.kind == ShapeNodeKind::HullPoints) {
            int removeAt = -1;
            for (usize p = 0; p < n.nums.size() / 3; ++p) {
                ImGui::PushID(static_cast<int>(p));
                f32 v[3] = {n.nums[p * 3], n.nums[p * 3 + 1], n.nums[p * 3 + 2]};
                char label[32];
                std::snprintf(label, sizeof label, "Point %zu", p + 1);
                const bool c = ImGui::DragFloat3(label, v, 0.5f, -100000.0f, 100000.0f, "%.1f");
                gesture(c, [&] { n.nums[p * 3] = v[0]; n.nums[p * 3 + 1] = v[1]; n.nums[p * 3 + 2] = v[2]; });
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) removeAt = static_cast<int>(p);
                ImGui::PopID();
            }
            if (ImGui::SmallButton("Add point")) { instant(true, [&] { sm::addHullPoint(n); }); }
            if (removeAt >= 0) {
                OcShapeData probe = shape_;
                if (sm::removeHullPoint(*probe.find(id), static_cast<usize>(removeAt))) instant(true, [&] { sm::removeHullPoint(n, static_cast<usize>(removeAt)); });
            }
        } else if (n.kind == ShapeNodeKind::HullMesh || n.kind == ShapeNodeKind::Mesh) {
            text("Mesh (content path)", n.path, "A content-relative .ocmesh, e.g. Meshes/floor.ocmesh. No spaces.");
        } else {
            for (usize i = 0; i < n.nums.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                dragF(sm::numberLabel(n.kind, i).c_str(), n.nums[i], 0.5f, -100000.0f, 100000.0f, "%.2f");
                ImGui::PopID();
            }
        }
        ImGui::SeparatorText("Options");
        const bool convex = n.kind != ShapeNodeKind::Mesh && n.kind != ShapeNodeKind::Plane && n.kind != ShapeNodeKind::Empty;
        if (convex) {
            dragF("Convex radius (cm)", n.radius, 0.05f, -1.0f, 100.0f, n.radius < 0.0f ? "default" : "%.2f");
            dragF("Density (kg/m3)", n.density, 5.0f, -1.0f, 30000.0f, n.density < 0.0f ? "1000" : "%.0f");
        }
        if (n.kind == ShapeNodeKind::HullPoints || n.kind == ShapeNodeKind::HullMesh) dragF("Hull tolerance (cm)", n.tolerance, 0.01f, -1.0f, 10.0f, n.tolerance < 0.0f ? "default" : "%.2f");
        if (n.kind == ShapeNodeKind::Mesh) {
            bool setEdge = n.hasEdge;
            if (ImGui::Checkbox("Set active-edge angle", &setEdge)) instant(true, [&] { n.hasEdge = setEdge; if (setEdge && n.edge == 0.0f) n.edge = 5.0f; });
            if (n.hasEdge) {
                dragF("Edge angle (deg)", n.edge, 0.2f, -1.0f, 90.0f, "%.1f");
                ImGui::TextDisabled("Negative = every edge active (then bodies touching it want enhanced edges).");
            }
            int q = n.quality + 1;
            static const char* const kQ[] = {"Default (runtime performance)", "Runtime performance", "Build speed"};
            if (ImGui::Combo("Build quality", &q, kQ, 3)) instant(true, [&] { n.quality = q - 1; });
        }
        text("Surface", n.surface, "A physical surface name; empty = the file's default.");
    } else if (n.kind == ShapeNodeKind::Compound) {
        bool mut = n.mutableCompound;
        if (ImGui::Checkbox("Mutable (children can be added and removed at run time; one body only)", &mut)) instant(true, [&] { n.mutableCompound = mut; });
        drawChildRecords(n);
    } else {
        // A decorator: which node it wraps, then its own numbers.
        std::vector<std::string> candidates;
        for (const ShapeNode& c : shape_.nodes)
            if (c.id != id && !sm::reaches(shape_, c.id, id)) candidates.push_back(c.id);
        if (ImGui::BeginCombo("Wraps", n.child.c_str())) {
            for (const std::string& c : candidates)
                if (ImGui::Selectable(c.c_str(), c == n.child)) instant(c != n.child, [&] { n.child = c; });
            ImGui::EndCombo();
        }
        if (n.kind == ShapeNodeKind::RotatedTranslated || n.kind == ShapeNodeKind::OffsetCom) {
            f32 p[3] = {n.pos[0], n.pos[1], n.pos[2]};
            const bool c = ImGui::DragFloat3(n.kind == ShapeNodeKind::OffsetCom ? "Centre-of-mass offset (cm)" : "Position (cm)", p, 0.5f, -100000.0f, 100000.0f, "%.1f");
            gesture(c, [&] { n.pos[0] = p[0]; n.pos[1] = p[1]; n.pos[2] = p[2]; });
        }
        if (n.kind == ShapeNodeKind::RotatedTranslated) {
            f32 q[4] = {n.rot[0], n.rot[1], n.rot[2], n.rot[3]};
            const bool c = ImGui::DragFloat4("Rotation (x y z w)", q, 0.005f, -1.0f, 1.0f, "%.4f");
            gesture(c, [&] { for (int k = 0; k < 4; ++k) n.rot[k] = q[k]; });
            if (ImGui::SmallButton("Normalise")) {
                const f32 len = std::sqrt(n.rot[0] * n.rot[0] + n.rot[1] * n.rot[1] + n.rot[2] * n.rot[2] + n.rot[3] * n.rot[3]);
                if (len > 1.0e-6f) instant(true, [&] { for (int k = 0; k < 4; ++k) n.rot[k] /= len; });
            }
        }
        if (n.kind == ShapeNodeKind::Scaled) {
            f32 s[3] = {n.scale[0], n.scale[1], n.scale[2]};
            const bool c = ImGui::DragFloat3("Scale", s, 0.01f, -100.0f, 100.0f, "%.3f");
            gesture(c, [&] { n.scale[0] = s[0]; n.scale[1] = s[1]; n.scale[2] = s[2]; });
            ImGui::TextDisabled("A scale the shape cannot wrap is baked by the physics module when it can and refused when it cannot.");
        }
    }

    // ---- node-level actions (structural: nothing below uses `n`) ----
    ImGui::Separator();
    if (id != shape_.root && ImGui::SmallButton("Make root")) { instant(true, [&] { sm::setRoot(shape_, id); }); ImGui::PopID(); return; }
    if (id != shape_.root) ImGui::SameLine();
    if (ImGui::SmallButton("Wrap in...")) ImGui::OpenPopup("##wrap");
    if (ImGui::BeginPopup("##wrap")) {
        for (const ShapeNodeKind k : kDecoratorKinds)
            if (ImGui::MenuItem(sm::kindLabel(k))) {
                std::string made;
                edit([&](OcShapeData& d) { made = sm::wrap(d, id, k); });
                if (!made.empty()) selectedId_ = made;
                ImGui::EndPopup();
                ImGui::PopID();
                return;
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add to compound...")) ImGui::OpenPopup("##addto");
    if (ImGui::BeginPopup("##addto")) {
        for (const ShapeNode& c : shape_.nodes) {
            if (c.kind != ShapeNodeKind::Compound) continue;
            OcShapeData probe = shape_;
            ImGui::BeginDisabled(!sm::addChild(probe, c.id, id));
            if (ImGui::MenuItem(c.id.c_str())) {
                const std::string compound = c.id;
                edit([&](OcShapeData& d) { sm::addChild(d, compound, id); });
                ImGui::EndDisabled();
                ImGui::EndPopup();
                ImGui::PopID();
                return;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
        edit([&](OcShapeData& d) { sm::removeNode(d, id); });
        selectedId_ = shape_.root;
        ImGui::PopID();
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Deletes the node, its place in compounds, and any decorator wrapped around it.");
    ImGui::PopID();
}

void OcShapeEditor::drawChildRecords(const ShapeNode& compound) {
    const std::string cid = compound.id;
    ImGui::SeparatorText("Children (in order)");
    int removeAt = -1, moveAt = -1, moveBy = 0;
    for (usize r = 0; r < shape_.children.size(); ++r) {
        fmt::ShapeChildRec& rec = shape_.children[r];
        if (rec.compound != cid) continue;
        ImGui::PushID(static_cast<int>(r));
        ImGui::TextUnformatted(rec.node.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("^")) { moveAt = static_cast<int>(r); moveBy = -1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) { moveAt = static_cast<int>(r); moveBy = 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(r);
        ImGui::SameLine();
        if (ImGui::SmallButton("Select")) selectedId_ = rec.node;
        f32 p[3] = {rec.pos[0], rec.pos[1], rec.pos[2]};
        bool c = ImGui::DragFloat3("At (cm)", p, 0.5f, -100000.0f, 100000.0f, "%.1f");
        gesture(c, [&] { rec.pos[0] = p[0]; rec.pos[1] = p[1]; rec.pos[2] = p[2]; });
        f32 q[4] = {rec.rot[0], rec.rot[1], rec.rot[2], rec.rot[3]};
        c = ImGui::DragFloat4("Rotation (x y z w)", q, 0.005f, -1.0f, 1.0f, "%.4f");
        gesture(c, [&] { for (int k = 0; k < 4; ++k) rec.rot[k] = q[k]; });
        int user = rec.user;
        c = ImGui::DragInt("User data", &user, 1.0f);
        gesture(c, [&] { rec.user = user; });
        ImGui::Separator();
        ImGui::PopID();
    }
    if (moveAt >= 0) instant(true, [&] { sm::moveChild(shape_, static_cast<usize>(moveAt), moveBy); });
    else if (removeAt >= 0) instant(true, [&] { sm::removeChild(shape_, static_cast<usize>(removeAt)); });

    if (ImGui::SmallButton("Add child...")) ImGui::OpenPopup("##addchild");
    if (ImGui::BeginPopup("##addchild")) {
        for (const ShapeNode& c : shape_.nodes) {
            OcShapeData probe = shape_;
            ImGui::BeginDisabled(!sm::addChild(probe, cid, c.id));
            if (ImGui::MenuItem(c.id.c_str())) {
                const std::string node = c.id;
                edit([&](OcShapeData& d) { sm::addChild(d, cid, node); });
                ImGui::EndDisabled();
                ImGui::EndPopup();
                return;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}

#else   // AVER_WITH_IMGUI

// The headless build still gets load / save / dirty / undo; only the window is absent.
void OcShapeEditor::draw(Engine& e) { (void)e; publishPreview(); }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeOcShapeEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocshape") return nullptr;
    return std::make_unique<OcShapeEditor>(path);
}

} // namespace aver::editor
