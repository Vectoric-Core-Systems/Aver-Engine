#include "ModelingMode.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#if AVER_WITH_IMGUI
#include "EditorIcons.hpp"
#include "imgui.h"
#endif

namespace aver::editor {

namespace md = modeling;
namespace em = fmt::em;

md::Projector ModelingMode::localProjector(const WorldProjector& project, const Vec3& eye) const {
    const Vec3 o = origin;
    return [project, eye, o](const em::V3& p, f32& sx, f32& sy, f32& depth) {
        const Vec3 w{o.x + static_cast<f32>(p.x), o.y + static_cast<f32>(p.y), o.z + static_cast<f32>(p.z)};
        if (!project(w, sx, sy)) return false;
        const f32 dx = w.x - eye.x, dy = w.y - eye.y, dz = w.z - eye.z;
        depth = std::sqrt(dx * dx + dy * dy + dz * dz);
        return true;
    };
}

bool ModelingMode::handleMouse(const ModelingMouse& m, const WorldProjector& project, const Vec3& eye) {
    if (m.clicked && m.overScene) {
        marqueeArmed_ = true;
        marqueeActive_ = false;
        x0_ = m.x;
        y0_ = m.y;
    }
    if (!marqueeArmed_) return false;
    if (!marqueeActive_ && marqueeExceedsThreshold(x0_, y0_, m.x, m.y)) marqueeActive_ = true;
#if AVER_WITH_IMGUI
    if (marqueeActive_) {
        const md::Rect r = md::rectOf(x0_, y0_, m.x, m.y);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(r.loX, r.loY), ImVec2(r.hiX, r.hiY), IM_COL32(235, 163, 10, 40));
        dl->AddRect(ImVec2(r.loX, r.loY), ImVec2(r.hiX, r.hiY), IM_COL32(235, 163, 10, 220));
    }
#endif
    if (m.released || !m.down) {
        const auto sv = md::projectMesh(model.doc.mesh, localProjector(project, eye));
        const bool additive = m.ctrl || m.shift;
        if (marqueeActive_) md::applyMarquee(model, sv, md::rectOf(x0_, y0_, m.x, m.y), additive);
        else md::applyClick(model, sv, m.x, m.y, 10.0f, additive);
        marqueeArmed_ = false;
        marqueeActive_ = false;
    }
    return true;
}

void ModelingMode::collectLines(std::vector<ModelingLine>& out) const {
    const em::EditMesh& mesh = model.doc.mesh;
    if (mesh.faces.empty()) return;
    const md::Selection& sel = model.doc.sel;
    const auto world = [this](u32 v) { const em::V3& p = model.doc.mesh.pos[v]; return Vec3{origin.x + static_cast<f32>(p.x), origin.y + static_cast<f32>(p.y), origin.z + static_cast<f32>(p.z)}; };

    std::set<em::Edge> hot;
    if (model.mode() == md::SelectMode::Edge) hot = sel.edges;
    if (model.mode() == md::SelectMode::Face) {
        const std::vector<u32> faces(sel.faces.begin(), sel.faces.end());
        for (const em::Edge& e : em::edgesOfFaces(mesh, faces)) hot.insert(e);
    }
    for (const em::Edge& e : md::edgeList(mesh)) {
        ModelingLine l;
        l.a = world(e.first);
        l.b = world(e.second);
        if (hot.count(e)) { l.r = 1.0f; l.g = 0.6f; l.bl = 0.1f; }
        else { l.r = 0.62f; l.g = 0.66f; l.bl = 0.72f; }
        out.push_back(l);
    }
    if (model.mode() == md::SelectMode::Vertex) {
        const f32 h = 2.5f;
        for (const u32 v : md::usedVertexList(mesh)) {
            const bool on = sel.verts.count(v) != 0;
            const Vec3 c = world(v);
            const f32 s = on ? h * 1.8f : h;
            ModelingLine l;
            l.r = on ? 1.0f : 0.85f; l.g = on ? 0.6f : 0.85f; l.bl = on ? 0.1f : 0.9f;
            l.a = Vec3{c.x - s, c.y, c.z}; l.b = Vec3{c.x + s, c.y, c.z}; out.push_back(l);
            l.a = Vec3{c.x, c.y - s, c.z}; l.b = Vec3{c.x, c.y + s, c.z}; out.push_back(l);
            l.a = Vec3{c.x, c.y, c.z - s}; l.b = Vec3{c.x, c.y, c.z + s}; out.push_back(l);
        }
    }
}

#if AVER_WITH_IMGUI

void ModelingMode::drawPanel(const std::string& contentDir, f32 dpi) {
    (void)dpi;
    md::ModelingModel& m = model;

    // ---- undo / status ----
    ImGui::BeginDisabled(!m.canUndo());
    if (ImGui::Button("Undo")) m.undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m.canRedo());
    if (ImGui::Button("Redo")) m.redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu verts  %zu faces  %zu tris", m.vertexCount(), m.faceCount(), m.triangleCount());
    if (!m.status().empty()) ImGui::TextWrapped("%s", m.status().c_str());

    // ---- new ----
    if (ImGui::CollapsingHeader("New Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* names[] = {"Box", "Sphere", "Cylinder", "Cone", "Torus", "Plane", "Stairs", "Arch"};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##prim", &primitive_, names, 8);
        ImGui::DragFloat3("Size (cm)", size_, 1.0f, 1.0f, 100000.0f, "%.0f");
        ImGui::SliderInt("Detail", &detail_, 4, 64);
        ImGui::Checkbox("Pivot at base", &m.params.primitive.pivotAtBase);
        if (ImGui::Button("Create", ImVec2(-1, 0))) m.newPrimitive(static_cast<md::Primitive>(primitive_), em::V3{size_[0], size_[1], size_[2]}, detail_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces the document. Unsaved edits are lost; Undo does not bring the old mesh back.");
        ImGui::DragFloat3("Sits at (cm)", &origin.x, 1.0f, -1.0e6f, 1.0e6f, "%.0f");
    }

    // ---- select ----
    if (ImGui::CollapsingHeader("Select", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* labels[] = {"Vertex (1)", "Edge (2)", "Face (3)"};
        const md::SelectMode modes[] = {md::SelectMode::Vertex, md::SelectMode::Edge, md::SelectMode::Face};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            const bool on = m.mode() == modes[i];
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.1f, 1.0f));
            if (ImGui::Button(labels[i])) m.setMode(modes[i]);
            if (on) ImGui::PopStyleColor();
        }
        if (ImGui::Button("All")) m.selectAll();
        ImGui::SameLine();
        if (ImGui::Button("None")) m.clearSelection();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu selected", m.selectionCount());
        ImGui::TextDisabled("Click picks, Ctrl+click adds, drag is a marquee.");
    }

    // ---- transform ----
    if (ImGui::CollapsingHeader("Transform")) {
        const char* pivots[] = {"Selection centre", "World origin"};
        int pv = static_cast<int>(m.params.pivot);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##pivot", &pv, pivots, 2)) m.params.pivot = static_cast<md::PivotMode>(pv);
        ImGui::DragFloat3("##move", move_, 1.0f, -1.0e5f, 1.0e5f, "%.1f");
        ImGui::SameLine();
        if (ImGui::Button("Move")) m.move(em::V3{move_[0], move_[1], move_[2]});
        const char* axes[] = {"X", "Y", "Z"};
        ImGui::SetNextItemWidth(50.0f);
        ImGui::Combo("##rotaxis", &rotAxis_, axes, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("##rotdeg", &rotDeg_, 1.0f, -360.0f, 360.0f, "%.0f deg");
        ImGui::SameLine();
        if (ImGui::Button("Rotate")) m.rotate(em::V3{rotAxis_ == 0 ? 1.0 : 0.0, rotAxis_ == 1 ? 1.0 : 0.0, rotAxis_ == 2 ? 1.0 : 0.0}, static_cast<double>(rotDeg_) * 3.14159265358979 / 180.0);
        ImGui::DragFloat3("##scale", scale_, 0.01f, 0.01f, 100.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("Scale")) m.scale(em::V3{scale_[0], scale_[1], scale_[2]});
    }

    // ---- edit ----
    if (ImGui::CollapsingHeader("Edit")) {
        md::ToolParams& p = m.params;
        float d = static_cast<float>(p.extrudeDistance);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::DragFloat("##ext", &d, 0.5f, -10000.0f, 10000.0f, "%.1f cm")) p.extrudeDistance = d;
        ImGui::SameLine();
        if (ImGui::Button("Extrude")) m.extrude();
        d = static_cast<float>(p.insetAmount);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::DragFloat("##ins", &d, 0.2f, 0.0f, 10000.0f, "%.1f cm")) p.insetAmount = d;
        ImGui::SameLine();
        if (ImGui::Button("Inset")) m.inset();
        d = static_cast<float>(p.bevelWidth);
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::DragFloat("##bev", &d, 0.2f, 0.0f, 10000.0f, "%.1f")) p.bevelWidth = d;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50.0f);
        ImGui::DragInt("##bevseg", &p.bevelSegments, 0.2f, 1, 16);
        ImGui::SameLine();
        if (ImGui::Button("Bevel")) m.bevel();
        if (ImGui::Button("Delete")) m.deleteSelection();
        ImGui::SameLine();
        if (ImGui::Button("Weld")) m.weld();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Two or more selected vertices merge at their centre; with fewer, every vertex closer than the tolerance is welded.");
        ImGui::SameLine();
        if (ImGui::Button("Flip")) m.flip();
        ImGui::SameLine();
        if (ImGui::Button("Subdivide")) m.subdivide();
        const char* axes[] = {"X", "Y", "Z"};
        ImGui::SetNextItemWidth(50.0f);
        ImGui::Combo("##mirax", &p.mirrorAxis, axes, 3);
        ImGui::SameLine();
        d = static_cast<float>(p.mirrorAt);
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::DragFloat("##mirat", &d, 1.0f, -1.0e5f, 1.0e5f, "%.0f")) p.mirrorAt = d;
        ImGui::SameLine();
        if (ImGui::Button("Mirror")) m.mirror();
        ImGui::Separator();
        ImGui::TextDisabled("Boolean with another saved mesh");
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s", booleanPath_.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##boolpath", "path to an .ocmesh", buf, sizeof buf)) booleanPath_ = buf;
        const struct { const char* label; fmt::csg::Op op; } ops[] = {{"Union", fmt::csg::Op::Union}, {"Subtract", fmt::csg::Op::Subtraction}, {"Intersect", fmt::csg::Op::Intersection}};
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(ops[i].label)) {
                em::EditMesh other;
                bool side = false;
                std::string why;
                if (em::loadAuthoredMesh(booleanPath_, other, &side, &why)) m.booleanWith(other, ops[i].op);
                else AVER_WARN("[Modeling] boolean: {}", why);
            }
        }
    }

    // ---- UV ----
    if (ImGui::CollapsingHeader("UV")) {
        const struct { const char* label; em::UvProjection kind; } kinds[] = {{"Box", em::UvProjection::Box}, {"Planar", em::UvProjection::Planar},
                                                                               {"Cylinder", em::UvProjection::Cylindrical}, {"Sphere", em::UvProjection::Spherical}};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(kinds[i].label)) m.projectUv(kinds[i].kind);
        }
        float tile = static_cast<float>(m.params.uvTile);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("Tile (cm per 1.0)", &tile, 1.0f, 1.0f, 100000.0f, "%.0f")) m.params.uvTile = tile;
        ImGui::TextDisabled("Texel density at %.0f px: %.2f px/cm", m.params.textureSize, m.texelDensity());
    }

    // ---- file ----
    if (ImGui::CollapsingHeader("File")) {
        if (savePath_.empty() && !contentDir.empty()) savePath_ = contentDir + "/Meshes/NewMesh.ocmesh";
        char buf[512];
        std::snprintf(buf, sizeof buf, "%s", (m.path().empty() ? savePath_ : m.path()).c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##savepath", buf, sizeof buf)) savePath_ = buf;
        if (ImGui::Button("Save")) m.save(savePath_);
        ImGui::SameLine();
        if (ImGui::Button("Open")) m.load(savePath_);
        ImGui::TextDisabled("Save writes the .ocmesh with its edit data, so quads reopen as quads.");
        const em::Validation v = m.validation();
        if (!v.message.empty()) ImGui::TextWrapped("Mesh: %s", v.message.c_str());
        ImGui::TextDisabled("Volume %.0f cm^3", m.volume());
    }
}

#else

void ModelingMode::drawPanel(const std::string&, f32) {}

#endif

}  // namespace aver::editor
