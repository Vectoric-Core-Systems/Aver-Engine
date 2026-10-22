// The widget half of the Collision Shape Details section. The rules (when it may be attached, what Create writes, the record keys, the
// read-out and probe text) are in CollisionShapeDetailsModel.hpp (ImGui-free, tested); this file only draws them. In its own translation
// unit because SandboxComponentDetails.cpp is at the Debug COFF section cap. docs/PHYSICS_SHAPES.md "Editor".
#include "CollisionShapeDetailsModel.hpp"

#if AVER_WITH_IMGUI && AVER_MODULE_SCENE

#include "aver/formats/OcSurf.hpp"
#include "aver/game/ColliderShapes.hpp"
#include "aver/game/PhysicalSurfaces.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_shape_builder_abi.h"
#  include "aver/physics/physics_surface_abi.h"
#endif

#include "imgui.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace aver::editor {
namespace {

namespace cm = csmodel;

const ImVec4 kWarn{1.0f, 0.55f, 0.35f, 1.0f};

// The project's surface names (and their ids), re-read when the project changes and about twice a second when no widget is busy.
struct SurfaceNames {
    std::string contentDir;
    int lastLoad = -1000;
    std::vector<std::pair<i32, std::string>> list;
};

SurfaceNames& surfaceNames(const std::string& contentDir) {
    static SurfaceNames s;
    const int frame = ImGui::GetFrameCount();
    if (s.contentDir != contentDir || (!ImGui::IsAnyItemActive() && frame - s.lastLoad > 30)) {
        s.contentDir = contentDir;
        s.lastLoad = frame;
        s.list.clear();
        fmt::OcSurfData d;
        if (!contentDir.empty() && fmt::loadOcSurf(game::physicalSurfacesPath(contentDir), d, nullptr))
            for (const fmt::OcSurfRecord& r : d.surfaces) s.list.emplace_back(aver::surfaceIdFromName(r.name), r.name);
    }
    return s;
}

std::string nameOfSurface(const SurfaceNames& names, i32 id) {
    for (const auto& p : names.list)
        if (p.first == id) return p.second;
    return {};
}

// The shape asset behind the row, re-read on a change of path and about twice a second when no widget is busy.
struct LoadedShape {
    std::string absPath;
    int lastLoad = -1000;
    bool ok = false;
    std::string error;
};

LoadedShape& loadedShape(const std::string& absPath) {
    static LoadedShape s;
    const int frame = ImGui::GetFrameCount();
    if (s.absPath != absPath || (!ImGui::IsAnyItemActive() && frame - s.lastLoad > 30)) {
        s.absPath = absPath;
        s.lastLoad = frame;
        s.ok = false;
        s.error.clear();
        if (!absPath.empty()) {
            fmt::OcShapeData d;
            s.ok = fmt::loadOcShape(absPath, d, &s.error);
        }
    }
    return s;
}

#if AVER_MODULE_PHYSICS
cm::ShapeReadout readoutOf(int32_t shape) {
    cm::ShapeReadout r;
    float f[AVER_PHYS_SI_COUNT] = {};
    int32_t i[AVER_PHYS_SII_COUNT] = {};
    if (shape == 0 || !aver_phys_shape_info(shape, f, AVER_PHYS_SI_COUNT, i, AVER_PHYS_SII_COUNT)) return r;
    r.valid = true;
    r.type = i[AVER_PHYS_SII_TYPE];
    r.children = i[AVER_PHYS_SII_CHILDREN];
    r.leaves = i[AVER_PHYS_SII_LEAVES];
    r.triangles = i[AVER_PHYS_SII_TRIANGLES];
    r.volumeCm3 = f[AVER_PHYS_SI_VOLUME_CM3];
    r.massKg = f[AVER_PHYS_SI_MASS_KG];
    for (int k = 0; k < 3; ++k) {
        r.com[k] = f[AVER_PHYS_SI_COM_X + k];
        r.boundsMin[k] = f[AVER_PHYS_SI_BOUNDS_MIN_X + k];
        r.boundsMax[k] = f[AVER_PHYS_SI_BOUNDS_MAX_X + k];
    }
    return r;
}

cm::Probe probeRay(const Vec3& o, const Vec3& d, const SurfaceNames& names) {
    cm::Probe p;
    float f[AVER_PHYS_HITF_COUNT] = {};
    int32_t i[AVER_PHYS_HITI_COUNT] = {};
    const int32_t body = aver_phys_raycast_detail(o.x, o.y, o.z, d.x, d.y, d.z, 200000.0f, 0xFFFFFFFFu, 0, 0, f, AVER_PHYS_HITF_COUNT, i, AVER_PHYS_HITI_COUNT);
    if (!body) return p;
    p.hit = true;
    p.surface = i[AVER_PHYS_HITI_SURFACE];
    p.name = nameOfSurface(names, p.surface);
    p.friction = f[AVER_PHYS_HITF_FRICTION];
    p.restitution = f[AVER_PHYS_HITF_RESTITUTION];
    p.tag = i[AVER_PHYS_HITI_TAG];
    p.child = i[AVER_PHYS_HITI_CHILD];
    p.triangle = i[AVER_PHYS_HITI_TRIANGLE];
    p.cellX = i[AVER_PHYS_HITI_CELL_X];
    p.cellY = i[AVER_PHYS_HITI_CELL_Y];
    p.distanceCm = f[AVER_PHYS_HITF_DISTANCE_CM];
    return p;
}
#endif

} // namespace

void drawCollisionShapeSection(const CollisionShapeSectionEnv& env, void* worldPtr, u32 entity) {
    auto& w = *static_cast<scene::World*>(worldPtr);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    const u32 type = game::collisionShapeType(w);
    auto* c = type ? w.component<game::CCollisionShape>(e, type) : nullptr;
    if (!c || !env.instantEdit || !env.gestureEdit) return;
    const auto* rb = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody);
    if (const char* reason = cm::attachRefusal(rb != nullptr)) ImGui::TextColored(kWarn, "%s", reason);

    // ---- the shape asset ----
    if (env.assetRow) env.assetRow("asset", "Shape (.ocshape)", c->asset);
    const std::string assetRel = c->asset != 0 && env.pathOfId ? env.pathOfId(c->asset) : std::string();
    const std::string absAsset = assetRel.empty() || env.contentDir.empty() ? std::string() : env.contentDir + "/" + assetRel;
    LoadedShape& shape = loadedShape(absAsset);
    const std::string problem = cm::assetProblem(c->asset, assetRel, shape.ok, shape.error);
    if (!problem.empty()) ImGui::TextColored(kWarn, "%s", problem.c_str());
    if (!assetRel.empty() && env.openEditor) {
        if (ImGui::SmallButton("Open in editor")) env.openEditor(assetRel);
        ImGui::SameLine();
    }
    if (c->asset == 0) {
        ImGui::TextDisabled("No asset: the Rigid Body's own box, sphere, capsule or cylinder stands.");
        ImGui::BeginDisabled(env.playing || !rb || !env.createAsset);
        if (ImGui::SmallButton("Create .ocshape from the Rigid Body") && rb) {
            fmt::OcShapeData d;
            std::string why;
            const std::string rel = cm::assetPathForEntity(env.entityName);
            if (env.fileExists && env.fileExists(rel)) {
                if (env.setStatus) env.setStatus(rel + " already exists; assign it instead.");
            } else if (!cm::shapeFromRigidBody(*rb, d, why)) {
                if (env.setStatus) env.setStatus(why);
            } else {
                d.name = env.entityName;
                env.createAsset(rel, fmt::writeOcShape(d));
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Writes the body's current shape as a one-node .ocshape under Content/Physics and assigns it, ready to grow into a compound.");
    } else {
        ImGui::TextDisabled("Edits to the asset apply to bodies built after the save (the next Play).");
    }

    // ---- the surface label ----
    SurfaceNames& names = surfaceNames(env.contentDir);
    const std::string current = c->surface == 0 ? std::string("(none)") : cm::surfaceLabel(c->surface, nameOfSurface(names, c->surface));
    ImGui::BeginDisabled(env.playing);
    if (ImGui::BeginCombo("Surface", current.c_str())) {
        if (ImGui::Selectable("(none)", c->surface == 0)) env.instantEdit(c->surface != 0, [&] { c->surface = 0; });
        for (const auto& p : names.list)
            if (ImGui::Selectable(p.second.c_str(), p.first == c->surface)) env.instantEdit(p.first != c->surface, [&] { c->surface = p.first; });
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The body's surface label: what a raycast or a contact reports for a shape that names none.\n"
                          "Leaves that name a surface (in the .ocshape) keep theirs and carry the friction.");
    static char s_free[80] = {};
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputTextWithHint("##csSurfaceFree", "other name + Enter", s_free, sizeof s_free, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (fmt::ocSurfNameValid(s_free)) {
            const i32 id = aver::surfaceIdFromName(s_free);
            env.instantEdit(id != c->surface, [&] { c->surface = id; });
            s_free[0] = 0;
        }
    }
    bool noScale = (c->flags & game::kCollisionShapeIgnoreScale) != 0;
    if (ImGui::Checkbox("Ignore entity scale", &noScale))
        env.instantEdit(true, [&] { c->flags = (c->flags & ~game::kCollisionShapeIgnoreScale) | (noScale ? game::kCollisionShapeIgnoreScale : 0u); });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Off: the shape is scaled by the entity's world scale when the body is built.");
    // The motion type is the one number of the Rigid Body the level record keeps (motion=): without it a shape in a level could only be static.
    if (auto* body = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody)) {
        static const char* const kMotion[] = {"Static", "Kinematic", "Dynamic"};
        const int cur = body->motionType >= 0 && body->motionType <= 2 ? body->motionType : 0;
        if (ImGui::BeginCombo("Motion", kMotion[cur])) {
            for (int m = 0; m < 3; ++m)
                if (ImGui::Selectable(kMotion[m], m == cur)) env.instantEdit(m != cur, [&] { body->motionType = m; });
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Static, kinematic or dynamic. A dynamic shape takes its mass from the .ocshape MASS (or its density times volume).\n"
                              "Friction, restitution and damping stay at the Rigid Body's defaults; scripts can change them.");
    }
    ImGui::EndDisabled();

#if AVER_MODULE_PHYSICS
    // ---- Play-time read-outs and the surface probe ----
    if (env.playing || c->outShape != 0) {
        if (c->outShape != 0) {
            const cm::ShapeReadout r = readoutOf(c->outShape);
            for (const std::string& line : cm::readoutLines(r)) ImGui::TextUnformatted(line.c_str());
        } else {
            ImGui::TextDisabled("No body was built from the asset (see the log).");
        }
    }
    if (aver_phys_ready()) {
        static cm::Probe s_probe;
        static bool s_probed = false;
        if (env.pickRay && ImGui::SmallButton("Probe surface at the viewport centre")) {
            Vec3 o, d;
            if (env.pickRay(o, d)) { s_probe = probeRay(o, d, names); s_probed = true; }
        }
        if (env.pickRay) ImGui::SameLine();
        if (ImGui::SmallButton("Probe below the entity")) {
            const Vec3 at = transformFromMatrix(w.worldMatrix(e)).position;
            s_probe = probeRay(Vec3{at.x, at.y, at.z + 50.0f}, Vec3{0.0f, 0.0f, -1.0f}, names);
            s_probed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fires aver_phys_raycast_detail and reports the surface, its numbers, and which compound child, mesh triangle or terrain cell was hit.");
        if (s_probed) ImGui::TextWrapped("%s", cm::formatProbe(s_probe).c_str());
    }
#endif
}

} // namespace aver::editor

#endif // AVER_WITH_IMGUI && AVER_MODULE_SCENE
