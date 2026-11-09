// Editor: the Landscape mode's Water tab -- rivers and lakes as spline records in the level (RIVER / RIVERPT, LAKE /
// LAKEPT), their list and parameters, the viewport tool that places and drags their control points on the terrain, and the
// one-shot terrain carve. Part of SandboxApp (declared in SandboxApp.hpp); the pure helpers are in WaterSplineTool.hpp.

#include "SandboxApp.hpp"
#include "WaterSplineTool.hpp"

#include <algorithm>
#include <cmath>

#if AVER_MODULE_LANDSCAPE && AVER_MODULE_FLUIDS
#include "aver/fluids/WaterBody.hpp"

namespace aver {

// ---------------------------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------------------------
namespace editor {

void reverseRiver(fmt::OcRiver& r) { std::reverse(r.points.begin(), r.points.end()); }

void snapRiverToGround(fmt::OcRiver& r, const GroundFn& ground) {
    for (fmt::OcRiverPoint& p : r.points) {
        f64 z = 0.0;
        if (ground && ground(p.x, p.y, z)) p.z = z;
    }
}

bool fitLakeLevel(fmt::OcLake& l, const GroundFn& ground) {
    std::vector<f32> xy;
    lakeOutlineXY(l, 200.0f, xy);
    bool any = false;
    f64 lo = 0.0;
    for (usize i = 0; i + 1 < xy.size(); i += 2) {
        f64 z = 0.0;
        if (!ground || !ground(xy[i], xy[i + 1], z)) continue;
        if (!any || z < lo) lo = z;
        any = true;
    }
    if (any) l.levelCm = lo;
    return any;
}

void riverCarvePoints(const fmt::OcRiver& r, f32 stepCm, std::vector<landscape::CarvePoint>& out) {
    out.clear();
    if (r.points.size() < 2) return;
    std::vector<fluids::SplinePoint> pts;
    for (const fmt::OcRiverPoint& p : r.points)
        pts.push_back({static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z), static_cast<f32>(p.widthCm), static_cast<f32>(p.flowCmS)});
    fluids::Spline sp;
    sp.set(std::move(pts), false);
    std::vector<fluids::Spline::Sample> s;
    sp.sample(stepCm, s);
    for (const fluids::Spline::Sample& q : s) out.push_back({q.pos[0], q.pos[1], q.pos[2], 0.5f * q.width});
}

void lakeOutlineXY(const fmt::OcLake& l, f32 stepCm, std::vector<f32>& out) {
    out.clear();
    if (l.points.size() < 3) return;
    std::vector<fluids::SplinePoint> pts;
    for (const fmt::OcLakePoint& p : l.points) pts.push_back({static_cast<f32>(p.x), static_cast<f32>(p.y), 0.0f, 0.0f, 0.0f});
    fluids::Spline sp;
    sp.set(std::move(pts), true);
    std::vector<fluids::Spline::Sample> s;
    sp.sample(stepCm, s);
    for (const fluids::Spline::Sample& q : s) { out.push_back(q.pos[0]); out.push_back(q.pos[1]); }
}

bool sameBodies(const std::vector<fmt::OcRiver>& ra, const std::vector<fmt::OcRiver>& rb,
                const std::vector<fmt::OcLake>& la, const std::vector<fmt::OcLake>& lb) {
    if (ra.size() != rb.size() || la.size() != lb.size()) return false;
    for (usize i = 0; i < ra.size(); ++i) {
        const fmt::OcRiver &a = ra[i], &b = rb[i];
        if (a.name != b.name || a.material != b.material || a.depthCm != b.depthCm || a.bankCm != b.bankCm ||
            a.bankSlow != b.bankSlow || a.flowScale != b.flowScale || a.points.size() != b.points.size()) return false;
        for (usize k = 0; k < a.points.size(); ++k) {
            const fmt::OcRiverPoint &p = a.points[k], &q = b.points[k];
            if (p.x != q.x || p.y != q.y || p.z != q.z || p.widthCm != q.widthCm || p.flowCmS != q.flowCmS) return false;
        }
    }
    for (usize i = 0; i < la.size(); ++i) {
        const fmt::OcLake &a = la[i], &b = lb[i];
        if (a.name != b.name || a.material != b.material || a.levelCm != b.levelCm || a.depthCm != b.depthCm ||
            a.bankCm != b.bankCm || a.maxEdgeCm != b.maxEdgeCm || a.points.size() != b.points.size()) return false;
        for (usize k = 0; k < a.points.size(); ++k)
            if (a.points[k].x != b.points[k].x || a.points[k].y != b.points[k].y) return false;
    }
    return true;
}

int pickPoint(const std::vector<f32>& xy, const std::vector<u8>& valid, f32 mx, f32 my, f32 radiusPx) {
    int best = -1;
    f32 bestD = radiusPx;
    for (usize i = 0; i < valid.size(); ++i) {
        if (!valid[i]) continue;
        const f32 d = std::hypot(xy[i * 2] - mx, xy[i * 2 + 1] - my);
        if (d <= bestD) { bestD = d; best = static_cast<int>(i); }
    }
    return best;
}

int pickSegment(const std::vector<f32>& xy, const std::vector<u8>& valid, bool closed, f32 mx, f32 my, f32 radiusPx,
                f32& outT) {
    const usize n = valid.size();
    int best = -1;
    f32 bestD = radiusPx;
    const usize segs = n < 2 ? 0 : (closed ? n : n - 1);
    for (usize i = 0; i < segs; ++i) {
        const usize j = (i + 1) % n;
        if (!valid[i] || !valid[j]) continue;
        const f32 ax = xy[i * 2], ay = xy[i * 2 + 1], bx = xy[j * 2], by = xy[j * 2 + 1];
        const f32 vx = bx - ax, vy = by - ay;
        const f32 l2 = vx * vx + vy * vy;
        const f32 t = l2 > 1e-6f ? std::clamp(((mx - ax) * vx + (my - ay) * vy) / l2, 0.0f, 1.0f) : 0.0f;
        const f32 d = std::hypot(mx - (ax + vx * t), my - (ay + vy * t));
        if (d <= bestD) { bestD = d; best = static_cast<int>(i); outT = t; }
    }
    return best;
}

} // namespace editor

#if AVER_WITH_IMGUI

// ---------------------------------------------------------------------------------------------------------------
// The tab
// ---------------------------------------------------------------------------------------------------------------

namespace {
std::string uniqueName(const std::string& base, const std::vector<fmt::OcRiver>& rivers, const std::vector<fmt::OcLake>& lakes) {
    for (u32 k = 1;; ++k) {
        const std::string nm = base + std::to_string(k);
        bool dup = false;
        for (const fmt::OcRiver& r : rivers) dup |= r.name == nm;
        for (const fmt::OcLake& l : lakes) dup |= l.name == nm;
        if (!dup) return nm;
    }
}
} // namespace

// The "before" of an edit gesture: refreshed every idle frame, held while one is open.
void SandboxApp::waterSnapshotIfIdle() {
    if (landTool_.waterGestureOpen_) return;
    landTool_.waterGestureRivers_ = lvl_.levelHeader_.rivers;
    landTool_.waterGestureLakes_ = lvl_.levelHeader_.lakes;
}

// A record changed: the gesture is open, the level is unsaved, and the meshes rebuild before the next draw.
void SandboxApp::waterEdited() {
    landTool_.waterGestureOpen_ = true;
    landTool_.waterBodiesStale_ = true;
    markLevelUnsaved();
}

// Ends the gesture and pushes ONE undo entry for it, none when nothing differs from the snapshot.
void SandboxApp::waterGestureEnd(const char* label) {
    if (!landTool_.waterGestureOpen_) return;
    landTool_.waterGestureOpen_ = false;
    if (editor::sameBodies(landTool_.waterGestureRivers_, lvl_.levelHeader_.rivers, landTool_.waterGestureLakes_, lvl_.levelHeader_.lakes)) return;
    EditCmd c;
    c.kind = EditCmd::Kind::WaterBodies;
    c.label = label;
    c.riversBefore = landTool_.waterGestureRivers_;
    c.riversAfter = lvl_.levelHeader_.rivers;
    c.lakesBefore = landTool_.waterGestureLakes_;
    c.lakesAfter = lvl_.levelHeader_.lakes;
    pushEdit(std::move(c));
}

// Lowers the terrain along the selected river or inside the selected lake: one undoable height stroke.
void SandboxApp::carveSelectedBody(Engine& e) {
    if (!landscape_.loaded()) return;
    const int nr = static_cast<int>(lvl_.levelHeader_.rivers.size());
    landscape::CarveParams cp;
    if (landTool_.waterSel_ >= 0 && landTool_.waterSel_ < nr) {
        const fmt::OcRiver& r = lvl_.levelHeader_.rivers[static_cast<usize>(landTool_.waterSel_)];
        std::vector<landscape::CarvePoint> pts;
        editor::riverCarvePoints(r, 250.0f, pts);
        if (pts.empty()) return;
        cp.depthCm = static_cast<f32>(r.depthCm);
        cp.bankCm = static_cast<f32>(r.bankCm);
        beginSculptStroke();
        landscape_.carveRiver(e.device(), pts.data(), static_cast<u32>(pts.size()), cp);
        endSculptStroke();
        setUpgradeStatus("Carved the bed of " + r.name);
    } else if (landTool_.waterSel_ >= nr && landTool_.waterSel_ < nr + static_cast<int>(lvl_.levelHeader_.lakes.size())) {
        const fmt::OcLake& l = lvl_.levelHeader_.lakes[static_cast<usize>(landTool_.waterSel_ - nr)];
        std::vector<f32> xy;
        editor::lakeOutlineXY(l, 300.0f, xy);
        if (xy.size() < 6) return;
        cp.depthCm = static_cast<f32>(l.depthCm);
        cp.bankCm = static_cast<f32>(l.bankCm);
        beginSculptStroke();
        landscape_.carveBasin(e.device(), xy.data(), static_cast<u32>(xy.size() / 2), static_cast<f32>(l.levelCm), cp);
        endSculptStroke();
        setUpgradeStatus("Carved the basin of " + l.name);
    }
}

void SandboxApp::buildLandscapeWaterTab(Engine& e) {
    waterSnapshotIfIdle();
    auto& rivers = lvl_.levelHeader_.rivers;
    auto& lakes = lvl_.levelHeader_.lakes;
    const int nr = static_cast<int>(rivers.size());
    const int total = nr + static_cast<int>(lakes.size());
    if (landTool_.waterSel_ >= total) { landTool_.waterSel_ = -1; landTool_.waterPt_ = -1; }

    ImGui::TextDisabled("RIVERS AND LAKES");
    for (int i = 0; i < nr; ++i) {
        ImGui::PushID(i);
        const std::string label = rivers[static_cast<usize>(i)].name + "  (river, " + std::to_string(rivers[static_cast<usize>(i)].points.size()) + " pts)";
        if (ImGui::Selectable(label.c_str(), landTool_.waterSel_ == i)) { landTool_.waterSel_ = i; landTool_.waterPt_ = -1; }
        ImGui::PopID();
    }
    for (int j = 0; j < static_cast<int>(lakes.size()); ++j) {
        ImGui::PushID(1000 + j);
        const std::string label = lakes[static_cast<usize>(j)].name + "  (lake, " + std::to_string(lakes[static_cast<usize>(j)].points.size()) + " pts)";
        if (ImGui::Selectable(label.c_str(), landTool_.waterSel_ == nr + j)) { landTool_.waterSel_ = nr + j; landTool_.waterPt_ = -1; }
        ImGui::PopID();
    }
    if (total == 0) ImGui::TextDisabled("None yet. Add one, then click on the terrain to place its points.");

    const std::string defaultMaterial = lvl_.levelHeader_.waters.empty() ? std::string() : lvl_.levelHeader_.waters.front().material;
    if (ImGui::Button("New River")) {
        fmt::OcRiver r;
        r.name = uniqueName("River", rivers, lakes);
        r.material = defaultMaterial;
        rivers.push_back(std::move(r));
        landTool_.waterSel_ = static_cast<int>(rivers.size()) - 1;
        landTool_.waterPt_ = -1;
        waterEdited();
        waterGestureEnd("New river");
    }
    ImGui::SameLine();
    if (ImGui::Button("New Lake")) {
        fmt::OcLake l;
        l.name = uniqueName("Lake", rivers, lakes);
        l.material = defaultMaterial;
        lakes.push_back(std::move(l));
        landTool_.waterSel_ = static_cast<int>(rivers.size() + lakes.size()) - 1;
        landTool_.waterPt_ = -1;
        waterEdited();
        waterGestureEnd("New lake");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(landTool_.waterSel_ < 0);
    if (ImGui::Button("Delete") && landTool_.waterSel_ >= 0) {
        if (landTool_.waterSel_ < nr) rivers.erase(rivers.begin() + landTool_.waterSel_);
        else                lakes.erase(lakes.begin() + (landTool_.waterSel_ - nr));
        landTool_.waterSel_ = -1;
        landTool_.waterPt_ = -1;
        waterEdited();
        waterGestureEnd("Delete water body");
    }
    ImGui::EndDisabled();

    // ---- the selected body ----
    if (landTool_.waterSel_ >= 0) {
        ImGui::Spacing();
        ImGui::Separator();
        const bool isRiver = landTool_.waterSel_ < nr;
        bool changed = false;

        const auto materialPicker = [&](std::string& material) {
            ImGui::SetNextItemWidth(-1.0f);
#if AVER_MODULE_PBR
            if (ImGui::BeginCombo("Material", material.empty() ? "(fallback look)" : material.c_str())) {
                pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
                if (ImGui::Selectable("(fallback look)", material.empty())) { material.clear(); changed = true; }
                for (u32 i = 0; i < lib.count(); ++i) {
                    const pbr::MaterialDesc* d = lib.desc(lib.at(i));
                    if (!d || d->name.empty() || d->name.find('#') != std::string::npos) continue;
                    if (ImGui::Selectable(d->name.c_str(), d->name == material)) { material = d->name; changed = true; }
                }
                ImGui::EndCombo();
            }
#else
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s", material.c_str());
            if (ImGui::InputText("Material", buf, sizeof buf)) { material = buf; changed = true; }
#endif
        };
        const auto nameField = [&](std::string& name) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s", name.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("Name", buf, sizeof buf)) {
                std::string nm;
                for (const char* c = buf; *c; ++c) if (*c != ' ') nm += *c;   // names are single tokens in the file
                bool dup = nm.empty();
                for (const fmt::OcRiver& r : rivers) dup |= (&r.name != &name && r.name == nm);
                for (const fmt::OcLake& l : lakes) dup |= (&l.name != &name && l.name == nm);
                if (!dup) { name = nm; changed = true; }
            }
        };

        if (isRiver) {
            fmt::OcRiver& r = rivers[static_cast<usize>(landTool_.waterSel_)];
            nameField(r.name);
            materialPicker(r.material);
            f32 v;
            v = static_cast<f32>(r.depthCm);  if (ImGui::DragFloat("Depth (cm)", &v, 5.0f, 10.0f, 5000.0f, "%.0f")) { r.depthCm = v; changed = true; }
            v = static_cast<f32>(r.bankCm);   if (ImGui::DragFloat("Bank (cm)", &v, 5.0f, 0.0f, 5000.0f, "%.0f")) { r.bankCm = v; changed = true; }
            v = static_cast<f32>(r.bankSlow); if (ImGui::SliderFloat("Bank slow-down", &v, 0.0f, 1.0f, "%.2f")) { r.bankSlow = v; changed = true; }
            v = static_cast<f32>(r.flowScale); if (ImGui::DragFloat("Flow scale", &v, 0.02f, 0.0f, 10.0f, "%.2f")) { r.flowScale = v; changed = true; }
            ImGui::TextDisabled("Points run downstream. Click terrain to add, drag a handle to move (Shift: vertical),");
            ImGui::TextDisabled("Ctrl+click a segment to insert, Alt+click or Del to remove.");
            for (int k = 0; k < static_cast<int>(r.points.size()); ++k) {
                ImGui::PushID(k);
                const fmt::OcRiverPoint& p = r.points[static_cast<usize>(k)];
                const std::string label = "Point " + std::to_string(k) + "  (" + std::to_string(static_cast<int>(p.x)) + ", " + std::to_string(static_cast<int>(p.y)) + ")";
                if (ImGui::Selectable(label.c_str(), landTool_.waterPt_ == k)) landTool_.waterPt_ = k;
                ImGui::PopID();
            }
            if (landTool_.waterPt_ >= 0 && landTool_.waterPt_ < static_cast<int>(r.points.size())) {
                fmt::OcRiverPoint& p = r.points[static_cast<usize>(landTool_.waterPt_)];
                f32 w = static_cast<f32>(p.widthCm), fl = static_cast<f32>(p.flowCmS), z = static_cast<f32>(p.z);
                if (ImGui::DragFloat("Width (cm)", &w, 5.0f, 10.0f, 20000.0f, "%.0f")) { p.widthCm = w; changed = true; }
                if (ImGui::DragFloat("Flow (cm/s)", &fl, 1.0f, -2000.0f, 2000.0f, "%.0f")) { p.flowCmS = fl; changed = true; }
                if (ImGui::DragFloat("Height z (cm)", &z, 1.0f)) { p.z = z; changed = true; }
            }
            if (ImGui::Button("Snap Points To Terrain", ImVec2(-1, 0))) {
                editor::snapRiverToGround(r, [this](f64 x, f64 y, f64& z) { return landscape_.groundHeightAt(x, y, z); });
                changed = true;
            }
            if (ImGui::Button("Reverse Direction", ImVec2(-1, 0))) { editor::reverseRiver(r); landTool_.waterPt_ = -1; changed = true; }
        } else {
            fmt::OcLake& l = lakes[static_cast<usize>(landTool_.waterSel_ - nr)];
            nameField(l.name);
            materialPicker(l.material);
            f32 v;
            v = static_cast<f32>(l.levelCm);   if (ImGui::DragFloat("Level z (cm)", &v, 1.0f)) { l.levelCm = v; changed = true; }
            v = static_cast<f32>(l.depthCm);   if (ImGui::DragFloat("Depth (cm)", &v, 5.0f, 10.0f, 10000.0f, "%.0f")) { l.depthCm = v; changed = true; }
            v = static_cast<f32>(l.bankCm);    if (ImGui::DragFloat("Bank (cm)", &v, 5.0f, 0.0f, 5000.0f, "%.0f")) { l.bankCm = v; changed = true; }
            v = static_cast<f32>(l.maxEdgeCm); if (ImGui::DragFloat("Max mesh edge (cm)", &v, 25.0f, 200.0f, 20000.0f, "%.0f")) { l.maxEdgeCm = v; changed = true; }
            ImGui::TextDisabled("Click terrain to add outline points (3 or more close the shape).");
            for (int k = 0; k < static_cast<int>(l.points.size()); ++k) {
                ImGui::PushID(k);
                const std::string label = "Point " + std::to_string(k) + "  (" + std::to_string(static_cast<int>(l.points[static_cast<usize>(k)].x)) + ", " +
                                          std::to_string(static_cast<int>(l.points[static_cast<usize>(k)].y)) + ")";
                if (ImGui::Selectable(label.c_str(), landTool_.waterPt_ == k)) landTool_.waterPt_ = k;
                ImGui::PopID();
            }
            if (ImGui::Button("Fit Level To Lowest Point", ImVec2(-1, 0))) {
                if (editor::fitLakeLevel(l, [this](f64 x, f64 y, f64& z) { return landscape_.groundHeightAt(x, y, z); })) changed = true;
            }
        }
        if (changed) waterEdited();

        ImGui::Spacing();
        ImGui::BeginDisabled(!landscape_.loaded());
        if (ImGui::Button("Carve Terrain", ImVec2(-1, 0))) carveSelectedBody(e);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Lowers the terrain to a bed under the river (or a basin under the lake). One undo step.\n"
                              "One-shot: moving the water later does not move the bed.");
    }

    if (landTool_.waterGestureOpen_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(0)) waterGestureEnd("Edit water");
}

// The viewport half: LMB places and drags the selected body's control points on the terrain.
void SandboxApp::handleWaterSpline(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my) {
    (void)e;
    landTool_.sculptCursorValid_ = false;
    waterSnapshotIfIdle();
    if (!landscape_.loaded()) return;
    auto& rivers = lvl_.levelHeader_.rivers;
    auto& lakes = lvl_.levelHeader_.lakes;
    const int nr = static_cast<int>(rivers.size());
    if (landTool_.waterSel_ < 0 || landTool_.waterSel_ >= nr + static_cast<int>(lakes.size())) {
        landTool_.waterDrag_ = false;
        if (landTool_.waterGestureOpen_ && !io.MouseDown[0]) waterGestureEnd("Edit water");
        return;
    }
    const bool isRiver = landTool_.waterSel_ < nr;
    fmt::OcRiver* river = isRiver ? &rivers[static_cast<usize>(landTool_.waterSel_)] : nullptr;
    fmt::OcLake* lake = isRiver ? nullptr : &lakes[static_cast<usize>(landTool_.waterSel_ - nr)];
    const auto count = [&]() { return static_cast<int>(isRiver ? river->points.size() : lake->points.size()); };
    const auto world = [&](int k) {
        if (isRiver) { const fmt::OcRiverPoint& p = river->points[static_cast<usize>(k)]; return Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)}; }
        const fmt::OcLakePoint& p = lake->points[static_cast<usize>(k)];
        return Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(lake->levelCm)};
    };
    const auto removePoint = [&](int k) {
        if (k < 0 || k >= count()) return;
        if (isRiver) river->points.erase(river->points.begin() + k);
        else         lake->points.erase(lake->points.begin() + k);
        landTool_.waterPt_ = -1;
        landTool_.waterDrag_ = false;
        waterEdited();
    };

    Vec3 ro, rd;
    viewportRay(mx, my, ro, rd);
    const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
    landscape::HeightfieldHit hit;
    const bool haveHit = overScene && landscape_.raycast(roA, rdA, hit);

    std::vector<f32> sxy(static_cast<usize>(count()) * 2);
    std::vector<u8> valid(static_cast<usize>(count()), 0);
    for (int k = 0; k < count(); ++k) {
        f32 sx = 0, sy = 0;
        valid[static_cast<usize>(k)] = project(world(k), sx, sy) ? 1 : 0;
        sxy[static_cast<usize>(k) * 2] = sx;
        sxy[static_cast<usize>(k) * 2 + 1] = sy;
    }

    if (ImGui::IsMouseClicked(0) && overScene) {
        const int hover = editor::pickPoint(sxy, valid, mx, my, 10.0f);
        f32 t = 0.0f;
        if (hover >= 0) {
            if (io.KeyAlt) removePoint(hover);
            else { landTool_.waterPt_ = hover; landTool_.waterDrag_ = true; }
        } else if (io.KeyCtrl) {
            const int seg = editor::pickSegment(sxy, valid, !isRiver, mx, my, 12.0f, t);
            if (seg >= 0 && haveHit) {
                const int at = seg + 1;
                if (isRiver) {
                    fmt::OcRiverPoint p = river->points[static_cast<usize>(seg)];
                    p.x = hit.posCm[0]; p.y = hit.posCm[1]; p.z = hit.posCm[2];
                    river->points.insert(river->points.begin() + at, p);
                } else {
                    lake->points.insert(lake->points.begin() + at, fmt::OcLakePoint{hit.posCm[0], hit.posCm[1]});
                }
                landTool_.waterPt_ = at;
                waterEdited();
            }
        } else if (haveHit) {
            if (isRiver) {
                fmt::OcRiverPoint p;
                if (!river->points.empty()) { p.widthCm = river->points.back().widthCm; p.flowCmS = river->points.back().flowCmS; }
                p.x = hit.posCm[0]; p.y = hit.posCm[1]; p.z = hit.posCm[2];
                // Water does not run uphill: a new point is no higher than the one before it unless Shift is held.
                if (!river->points.empty() && !io.KeyShift) p.z = std::min<f64>(p.z, river->points.back().z);
                river->points.push_back(p);
                landTool_.waterPt_ = static_cast<int>(river->points.size()) - 1;
            } else {
                if (lake->points.empty()) lake->levelCm = hit.posCm[2];   // the first click sets the level
                lake->points.push_back(fmt::OcLakePoint{hit.posCm[0], hit.posCm[1]});
                landTool_.waterPt_ = static_cast<int>(lake->points.size()) - 1;
            }
            waterEdited();
        }
    }

    if (landTool_.waterDrag_ && io.MouseDown[0] && landTool_.waterPt_ >= 0 && landTool_.waterPt_ < count()) {
        if (io.KeyShift) {
            // Vertical: pixels to centimetres at the point's distance from the camera.
            if (isRiver && io.MouseDelta.y != 0.0f) {
                fmt::OcRiverPoint& p = river->points[static_cast<usize>(landTool_.waterPt_)];
                const Vec3 wp = world(landTool_.waterPt_);
                const f32 dist = std::sqrt((wp.x - viewState_.eye_.x) * (wp.x - viewState_.eye_.x) + (wp.y - viewState_.eye_.y) * (wp.y - viewState_.eye_.y) + (wp.z - viewState_.eye_.z) * (wp.z - viewState_.eye_.z));
                p.z -= static_cast<f64>(io.MouseDelta.y) * std::max(dist, 100.0f) * 0.002;
                waterEdited();
            }
        } else if (haveHit) {
            if (isRiver) {
                fmt::OcRiverPoint& p = river->points[static_cast<usize>(landTool_.waterPt_)];
                p.x = hit.posCm[0]; p.y = hit.posCm[1]; p.z = hit.posCm[2];
            } else {
                fmt::OcLakePoint& p = lake->points[static_cast<usize>(landTool_.waterPt_)];
                p.x = hit.posCm[0]; p.y = hit.posCm[1];
            }
            waterEdited();
        }
    }
    if (!io.MouseDown[0]) landTool_.waterDrag_ = false;

    if (viewState_.levelFocused_ && landTool_.waterPt_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !io.WantTextInput) removePoint(landTool_.waterPt_);

    if (landTool_.waterGestureOpen_ && !io.MouseDown[0] && !ImGui::IsAnyItemActive()) waterGestureEnd("Edit water");
}

#endif // AVER_WITH_IMGUI

// Rebuilds edited bodies before the frame draws them, whatever mode the editor is in (an undo can change them from anywhere),
// and draws the Water tab's chrome: each body's centreline and bank edges, and a handle on every control point.
void SandboxApp::drawWaterBodies(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;
    if (landTool_.waterBodiesStale_) {
        water_.rebuildBodies(*dev, lvl_.levelHeader_);
        landTool_.waterBodiesStale_ = false;
    }
#if AVER_WITH_IMGUI
    const bool show = !noEditorChrome_ && editorModeIsLandscape() && landTool_.landscapeTab_ == LandscapeTab::Water;
#else
    const bool show = false;
#endif
    if (!show) {
        if (landTool_.waterLines_) { dev->destroyLineMesh(landTool_.waterLines_); landTool_.waterLines_ = 0; landTool_.waterLinesKey_ = 0; }
        return;
    }

    // Hash of what the lines show, so the mesh is rebuilt only when it changes.
    u64 key = 1469598103934665603ull;
    const auto mix = [&key](const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) { key ^= b[i]; key *= 1099511628211ull; }
    };
    mix(&landTool_.waterSel_, sizeof landTool_.waterSel_);
    mix(&landTool_.waterPt_, sizeof landTool_.waterPt_);
    for (const fmt::OcRiver& r : lvl_.levelHeader_.rivers) for (const fmt::OcRiverPoint& p : r.points) mix(&p, sizeof p);
    for (const fmt::OcLake& l : lvl_.levelHeader_.lakes) { mix(&l.levelCm, sizeof l.levelCm); for (const fmt::OcLakePoint& p : l.points) mix(&p, sizeof p); }

    if (key != landTool_.waterLinesKey_) {
        landTool_.waterLinesKey_ = key;
        if (landTool_.waterLines_) { dev->destroyLineMesh(landTool_.waterLines_); landTool_.waterLines_ = 0; }
        std::vector<rhi::LineVertex> v;
        const auto seg = [&](const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
            v.push_back({a.x, a.y, a.z + 15.0f, r, g, bl});
            v.push_back({b.x, b.y, b.z + 15.0f, r, g, bl});
        };
        const auto handle = [&](const Vec3& p, bool selected) {
            const f32 s = 60.0f, cr = selected ? 1.0f : 0.95f, cg = selected ? 0.55f : 0.95f, cb = selected ? 0.1f : 0.95f;
            seg({p.x - s, p.y, p.z}, {p.x + s, p.y, p.z}, cr, cg, cb);
            seg({p.x, p.y - s, p.z}, {p.x, p.y + s, p.z}, cr, cg, cb);
            seg({p.x, p.y, p.z}, {p.x, p.y, p.z + 2.0f * s}, cr, cg, cb);
        };
        const int nr = static_cast<int>(lvl_.levelHeader_.rivers.size());
        for (int i = 0; i < nr; ++i) {
            const fmt::OcRiver& r = lvl_.levelHeader_.rivers[static_cast<usize>(i)];
            const bool sel = i == landTool_.waterSel_;
            std::vector<fluids::SplinePoint> pts;
            for (const fmt::OcRiverPoint& p : r.points)
                pts.push_back({static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z), static_cast<f32>(p.widthCm), static_cast<f32>(p.flowCmS)});
            if (pts.size() >= 2) {
                fluids::Spline sp;
                sp.set(std::move(pts), false);
                std::vector<fluids::Spline::Sample> s;
                sp.sample(150.0f, s);
                for (usize k = 0; k + 1 < s.size(); ++k) {
                    const fluids::Spline::Sample &a = s[k], &b = s[k + 1];
                    seg({a.pos[0], a.pos[1], a.pos[2]}, {b.pos[0], b.pos[1], b.pos[2]}, 0.3f, 0.8f, 1.0f);
                    if (!sel) continue;
                    const auto edge = [](const fluids::Spline::Sample& q, f32 side) {
                        return Vec3{q.pos[0] - q.tangent[1] * 0.5f * q.width * side, q.pos[1] + q.tangent[0] * 0.5f * q.width * side, q.pos[2]};
                    };
                    seg(edge(a, 1.0f), edge(b, 1.0f), 0.2f, 0.55f, 0.85f);
                    seg(edge(a, -1.0f), edge(b, -1.0f), 0.2f, 0.55f, 0.85f);
                }
            }
            if (sel)
                for (int k = 0; k < static_cast<int>(r.points.size()); ++k) {
                    const fmt::OcRiverPoint& p = r.points[static_cast<usize>(k)];
                    handle({static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)}, k == landTool_.waterPt_);
                }
        }
        for (int j = 0; j < static_cast<int>(lvl_.levelHeader_.lakes.size()); ++j) {
            const fmt::OcLake& l = lvl_.levelHeader_.lakes[static_cast<usize>(j)];
            const bool sel = nr + j == landTool_.waterSel_;
            std::vector<f32> xy;
            editor::lakeOutlineXY(l, 200.0f, xy);
            const f32 z = static_cast<f32>(l.levelCm);
            const usize n = xy.size() / 2;
            for (usize k = 0; k < n; ++k) {
                const usize m = (k + 1) % n;
                seg({xy[k * 2], xy[k * 2 + 1], z}, {xy[m * 2], xy[m * 2 + 1], z}, 0.3f, 0.8f, 1.0f);
            }
            if (sel)
                for (int k = 0; k < static_cast<int>(l.points.size()); ++k)
                    handle({static_cast<f32>(l.points[static_cast<usize>(k)].x), static_cast<f32>(l.points[static_cast<usize>(k)].y), z}, k == landTool_.waterPt_);
        }
        if (!v.empty()) landTool_.waterLines_ = dev->createLineMesh(v.data(), static_cast<u32>(v.size()));
    }
    if (landTool_.waterLines_) {
        const Mat4 w = Mat4::identity();
        dev->setLineDepth(false);
        dev->setLineWidth(2.0f * viewState_.dpi_);
        dev->drawLines(landTool_.waterLines_, &w.m[0][0]);
        dev->setLineWidth(1.0f);
        dev->setLineDepth(true);
    }
}

} // namespace aver

#else
namespace aver {}
#endif // AVER_MODULE_LANDSCAPE && AVER_MODULE_FLUIDS
