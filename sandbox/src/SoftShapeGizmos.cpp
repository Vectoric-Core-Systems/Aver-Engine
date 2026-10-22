// The editor half of jelly and rope (docs/SOFTBODY.md "Jelly, ropes and cables"): the Show > Soft Constraints overlay, the rope end handles,
// the Jelly and Rope Details sections, and the Solver / Inflate / Group subsections of a Cloth. The rules are in SoftShapeDetailsModel.hpp and
// SoftBodyDetailsModel.hpp (ImGui-free, tested); this file draws them and reads the live SoftShapeSystem. One translation unit for all of it
// because SandboxComponentDetails.cpp and SandboxRender.cpp are at the Debug COFF section cap: each reaches this file through a one-line call.
#include "SoftBodyDebugOverlay.hpp"
#include "SoftShapeDetailsModel.hpp"

#if AVER_MODULE_SOFTBODY_SCENE

#include "aver/scene/World.hpp"
#include "aver/softbody/SoftBodySystem.hpp"
#include "aver/softbody/SoftShapeSystem.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {
namespace {

namespace shm = shapemodel;

void line(SoftBodyDebugFrame& out, const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
    out.lines.push_back(rhi::LineVertex{a.x, a.y, a.z, r, g, bl});
    out.lines.push_back(rhi::LineVertex{b.x, b.y, b.z, r, g, bl});
}

// Three short axes through a point.
void cross3(SoftBodyDebugFrame& out, const Vec3& p, f32 size, f32 r, f32 g, f32 bl) {
    line(out, p - Vec3{size, 0, 0}, p + Vec3{size, 0, 0}, r, g, bl);
    line(out, p - Vec3{0, size, 0}, p + Vec3{0, size, 0}, r, g, bl);
    line(out, p - Vec3{0, 0, size}, p + Vec3{0, 0, size}, r, g, bl);
}

}  // namespace

// ---- rope end handles ------------------------------------------------------------------------------------------

bool ropeGizmoHandles(void* worldPtr, u32 entity, Vec3 out[2]) {
    auto& w = *static_cast<scene::World*>(worldPtr);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    const u32 type = softbody::SoftShapeSystem::ropeType();
    const auto* c = type && w.valid(e) ? w.component<softbody::CSoftRope>(e, type) : nullptr;
    if (!c) return false;
    for (int end = 0; end < 2; ++end) {
        const scene::Entity anchor = end == 0 ? c->startEntity : c->endEntity;
        const f32* o = end == 0 ? c->startOffset : c->endOffset;
        const Vec3 off{o[0], o[1], o[2]};
        if (anchor != scene::kInvalidEntity && w.valid(anchor)) {
            const Mat4 m = w.worldMatrix(anchor);
            out[end] = shm::ropeEndWorld(&m, off);
        } else {
            out[end] = shm::ropeEndWorld(nullptr, off);
        }
    }
    return true;
}

bool ropeGizmoDrag(void* worldPtr, u32 entity, int end, const Vec3& worldPoint) {
    auto& w = *static_cast<scene::World*>(worldPtr);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    const u32 type = softbody::SoftShapeSystem::ropeType();
    auto* c = type && w.valid(e) ? w.component<softbody::CSoftRope>(e, type) : nullptr;
    if (!c || end < 0 || end > 1 || !std::isfinite(worldPoint.x + worldPoint.y + worldPoint.z)) return false;
    const scene::Entity anchor = end == 0 ? c->startEntity : c->endEntity;
    f32* o = end == 0 ? c->startOffset : c->endOffset;
    Vec3 off;
    if (anchor != scene::kInvalidEntity && w.valid(anchor)) {
        const Mat4 m = w.worldMatrix(anchor);
        off = shm::ropeEndOffsetFor(&m, worldPoint);
    } else {
        off = shm::ropeEndOffsetFor(nullptr, worldPoint);
    }
    o[0] = off.x; o[1] = off.y; o[2] = off.z;
    return true;
}

// ---- dragging a rope end in the viewport ------------------------------------------------------------------------------

namespace {

// The grab in progress. One rope end at a time, so file-static state is enough and the host needs no member for it.
struct RopeHandleDrag {
    bool active = false;
    u32 entity = 0;
    int end = 0;
    Vec3 planePoint;        // the handle where it was grabbed
    Vec3 planeNormal;       // facing the camera
    Vec3 grabOffset;        // handle minus the ray's hit on the plane, so the end does not jump to the cursor
} g_ropeDrag;

// Where a ray meets the plane, false when it is parallel to it or the hit is behind the origin.
bool rayPlane(const Vec3& ro, const Vec3& rd, const Vec3& planePoint, const Vec3& planeNormal, Vec3& hit) {
    const f32 denom = dot(rd, planeNormal);
    if (std::fabs(denom) < 1.0e-6f) return false;
    const f32 t = dot(planePoint - ro, planeNormal) / denom;
    if (t <= 0.0f) return false;
    hit = ro + rd * t;
    return true;
}

}  // namespace

bool ropeHandleBegin(void* worldPtr, u32 entity, const Vec3& rayOrigin, const Vec3& rayDir, f32 pixelWorldCm) {
    g_ropeDrag = RopeHandleDrag{};
    Vec3 handles[2];
    if (!ropeGizmoHandles(worldPtr, entity, handles)) return false;
    const f32 radius = std::max(12.0f * pixelWorldCm, 2.0f);       // a 12 pixel grab radius, never thinner than 2 cm
    const Vec3 rd = rayDir.getSafeNormal();
    const int end = shm::pickHandle(handles, 2, rayOrigin, rd, radius);
    if (end < 0) return false;
    Vec3 hit;
    const Vec3 normal = rd * -1.0f;
    if (!rayPlane(rayOrigin, rd, handles[end], normal, hit)) return false;
    g_ropeDrag.active = true;
    g_ropeDrag.entity = entity;
    g_ropeDrag.end = end;
    g_ropeDrag.planePoint = handles[end];
    g_ropeDrag.planeNormal = normal;
    g_ropeDrag.grabOffset = handles[end] - hit;
    return true;
}

bool ropeHandleActive() { return g_ropeDrag.active; }

bool ropeHandleUpdate(void* worldPtr, const Vec3& rayOrigin, const Vec3& rayDir) {
    if (!g_ropeDrag.active) return false;
    Vec3 hit;
    if (!rayPlane(rayOrigin, rayDir.getSafeNormal(), g_ropeDrag.planePoint, g_ropeDrag.planeNormal, hit)) return false;
    return ropeGizmoDrag(worldPtr, g_ropeDrag.entity, g_ropeDrag.end, hit + g_ropeDrag.grabOffset);
}

void ropeHandleEnd() { g_ropeDrag = RopeHandleDrag{}; }

// ---- the overlay -----------------------------------------------------------------------------------------------

void softShapeDebugCollect(const SoftBodyDebugOptions& opts, softbody::SoftShapeSystem& sys, SoftBodyDebugFrame& out) {
    scene::World& w = scene::World::instance();

    // The selected rope's end handles are always drawn, simulated or not: they are what a viewport drag grabs. A rope that is not built
    // yet (the editor, outside Play) also shows its planned curve between them.
    if (opts.selected != 0) {
        Vec3 handle[2];
        if (ropeGizmoHandles(&w, opts.selected, handle)) {
            cross3(out, handle[0], 10.0f, 0.2f, 0.9f, 1.0f);
            cross3(out, handle[1], 10.0f, 0.2f, 0.9f, 1.0f);
            const Vec3* built = nullptr;
            u32 builtCount = 0;
            if (!sys.ropeNodes(static_cast<scene::Entity>(opts.selected), built, builtCount)) {
                softbody::RopeSpec spec;
                if (const auto* c = w.component<softbody::CSoftRope>(static_cast<scene::Entity>(opts.selected), softbody::SoftShapeSystem::ropeType()))
                    spec.lengthCm = c->lengthCm;
                softbody::RopeBuild plan;
                if (softbody::planRope(handle[0], handle[1], spec, plan))
                    for (u32 k = 0; k + 1 < plan.nodes; ++k) line(out, plan.rest[k], plan.rest[k + 1], 0.55f, 0.45f, 0.15f);
            }
        }
    }

    if (!opts.constraints) return;

    // Jelly: the lattice edges, coloured by how far each is from its rest length.
    if (const u32 vt = softbody::SoftShapeSystem::volumeType())
        if (scene::ComponentPool* pool = w.pool(vt))
            for (usize i = 0; i < pool->size(); ++i) {
                const scene::Entity e = pool->entityAt(i);
                const softbody::Lattice* lat = nullptr;
                const Vec3* nodes = nullptr;
                const f32* rest = nullptr;
                u32 restCount = 0;
                if (!w.valid(e) || !sys.volumeLattice(e, lat, nodes) || !sys.volumeEdgeRest(e, rest, restCount)) continue;
                for (u32 k = 0; k < restCount && (static_cast<usize>(k) * 2 + 1) < lat->edges.size(); ++k) {
                    const Vec3& a = nodes[lat->edges[static_cast<usize>(k) * 2]];
                    const Vec3& b = nodes[lat->edges[static_cast<usize>(k) * 2 + 1]];
                    const f32 strain = rest[k] > 1e-4f ? dist(a, b) / rest[k] - 1.0f : 0.0f;
                    f32 r, g, bl;
                    shm::stretchColour(strain, r, g, bl);
                    line(out, a, b, r, g, bl);
                }
            }

    // Rope: the polyline, each rod's three axes at its middle, the pinned ends as crosses, the selected rope's end handles.
    if (const u32 rt = softbody::SoftShapeSystem::ropeType())
        if (scene::ComponentPool* pool = w.pool(rt))
            for (usize i = 0; i < pool->size(); ++i) {
                const scene::Entity e = pool->entityAt(i);
                const Vec3* nodes = nullptr;
                u32 n = 0;
                if (!w.valid(e) || !sys.ropeNodes(e, nodes, n)) continue;
                for (u32 k = 0; k + 1 < n; ++k) line(out, nodes[k], nodes[k + 1], 0.95f, 0.75f, 0.2f);
                const Quat* rods = nullptr;
                u32 rc = 0;
                if (sys.ropeRods(e, rods, rc)) {
                    for (u32 k = 0; k < rc && k + 1 < n; ++k) {
                        const Vec3 mid = (nodes[k] + nodes[k + 1]) * 0.5f;
                        line(out, mid, mid + rods[k].rotate(Vec3{1, 0, 0}) * 4.0f, 1.0f, 0.2f, 0.2f);
                        line(out, mid, mid + rods[k].rotate(Vec3{0, 1, 0}) * 4.0f, 0.2f, 1.0f, 0.2f);
                        line(out, mid, mid + rods[k].rotate(Vec3{0, 0, 1}) * 4.0f, 0.3f, 0.5f, 1.0f);
                    }
                }
                bool startAttached = false, endAttached = false;
                if (sys.ropeAnchors(e, startAttached, endAttached)) {
                    if (startAttached) cross3(out, nodes[0], 6.0f, 1.0f, 1.0f, 0.2f);
                    if (endAttached) cross3(out, nodes[n - 1], 6.0f, 1.0f, 1.0f, 0.2f);
                }
            }
}

// ---- Details ---------------------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI
namespace {

const ImVec4 kWarn{1.0f, 0.55f, 0.35f, 1.0f};

// A tuning slider/drag on a component value: one undo step per gesture through the host's helper.
struct AssetCache {
    std::string loadedFor;
    softbody::SoftAsset asset;
    bool ok = false;
    int lastLoad = 0;
};

// The asset behind the row, re-read on a change of path and about twice a second when no widget is busy (the Cloth section's rule).
bool refreshAsset(AssetCache& cache, const std::string& abs) {
    const int frame = ImGui::GetFrameCount();
    if (cache.loadedFor != abs || (!ImGui::IsAnyItemActive() && frame - cache.lastLoad > 30)) {
        cache.loadedFor = abs;
        cache.lastLoad = frame;
        cache.ok = !abs.empty() && softbody::loadSoftAsset(abs, cache.asset, nullptr);
    }
    return cache.ok;
}

void drawJelly(const SoftShapeEnv& env, scene::World& w, scene::Entity e) {
    const u32 type = softbody::SoftShapeSystem::volumeType();
    auto* c = type ? w.component<softbody::CSoftVolume>(e, type) : nullptr;
    if (!c) return;
    if (!ImGui::CollapsingHeader("Jelly", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (env.drawAssetRow) env.drawAssetRow(c->asset);
    const std::string assetRel = env.pathOfId ? env.pathOfId(c->asset) : std::string();
    const std::string absAsset = assetRel.empty() ? std::string() : env.contentDir + "/" + assetRel;
    static AssetCache cache;
    const bool ok = refreshAsset(cache, absAsset);
    const std::string problem = softmodel::assetProblem(c->asset, assetRel, ok);
    if (!problem.empty()) ImGui::TextColored(kWarn, "%s", problem.c_str());

    const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    const std::string meshRel = mr && env.pathOfId ? env.pathOfId(mr->mesh) : std::string();
    if (c->asset == 0) {
        ImGui::BeginDisabled(env.playing || meshRel.empty());
        if (ImGui::SmallButton("Create .ocsoft")) {
            const std::string rel = shm::jellyAssetPath(meshRel);
            const std::filesystem::path target = std::filesystem::path(env.contentDir) / rel;
            std::error_code ec;
            if (std::filesystem::exists(target, ec)) {
                if (env.setStatus) env.setStatus(rel + " already exists; assign it instead.");
            } else {
                std::string why;
                if (!softbody::saveSoftAsset(target.string(), shm::starterJelly(), &why)) {
                    if (env.setStatus) env.setStatus("Could not write " + rel + ": " + why);
                } else {
                    if (env.adoptNewAsset) env.adoptNewAsset(target.string());
                    if (env.assignAsset) env.assignAsset(rel);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Writes %s beside the mesh. Without one the jelly preset is used.", meshRel.empty() ? "an asset" : shm::jellyAssetPath(meshRel).c_str());
    }

    // Per-component tuning (scales: 0 reads as 1, and 1 is shown).
    ImGui::BeginDisabled(env.playing);
    f32 stiff = c->stiffnessScale > 0.0f ? c->stiffnessScale : 1.0f;
    env.gestureEdit(ImGui::SliderFloat("Stiffness scale", &stiff, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic), [&] { c->stiffnessScale = stiff; });
    f32 cell = c->cellCm;
    env.gestureEdit(ImGui::DragFloat("Cell (cm)", &cell, 0.5f, 0.0f, 200.0f, cell > 0.0f ? "%.1f" : "asset default"), [&] { c->cellCm = cell; });
    f32 gravity = c->gravityScale > 0.0f ? c->gravityScale : 1.0f;
    env.gestureEdit(ImGui::SliderFloat("Gravity scale", &gravity, 0.0f, 4.0f, "%.2f"), [&] { c->gravityScale = gravity; });
    f32 damping = c->dampingScale > 0.0f ? c->dampingScale : 1.0f;
    env.gestureEdit(ImGui::SliderFloat("Damping scale", &damping, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic), [&] { c->dampingScale = damping; });
    bool collides = (c->flags & softbody::kVolNoWorld) == 0;
    env.instantEdit(ImGui::Checkbox("Collides with the world", &collides), [&] { c->flags = collides ? (c->flags & ~softbody::kVolNoWorld) : (c->flags | softbody::kVolNoWorld); });
    bool gravityOn = (c->flags & softbody::kVolNoGravity) == 0;
    env.instantEdit(ImGui::Checkbox("Gravity", &gravityOn), [&] { c->flags = gravityOn ? (c->flags & ~softbody::kVolNoGravity) : (c->flags | softbody::kVolNoGravity); });
    ImGui::EndDisabled();

    // The lattice and the stiffness numbers, on the loaded asset.
    const softbody::SoftAsset shown = ok && c->asset != 0 ? cache.asset : shm::starterJelly();
    const bool editable = ok && c->asset != 0 && !env.playing && shown.kind == softbody::SoftKind::Jelly;
    if (ImGui::TreeNode("Lattice and material")) {
        softbody::SoftAsset edit = shown;
        bool changed = false, finished = false;
        ImGui::BeginDisabled(!editable);
        const auto slider = [&](const char* label, f32& v, f32 lo, f32 hi, const char* format, bool log) {
            changed = ImGui::SliderFloat(label, &v, lo, hi, format, log ? ImGuiSliderFlags_Logarithmic : 0) || changed;
            finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
        };
        slider("Cell (cm)", edit.jelly.cellCm, 2.0f, 60.0f, "%.1f", true);
        slider("Snap to surface (cm)", edit.jelly.snapCm, 0.0f, 20.0f, "%.1f", false);
        slider("Volume compliance", edit.jelly.volumeCompliance, 0.0f, 1e-2f, "%.2e", false);
        slider("Edge compliance", edit.jelly.edgeCompliance, 0.0f, 1e-2f, "%.2e", false);
        slider("Density (kg/m3)", edit.jelly.densityKgM3, 10.0f, 3000.0f, "%.0f", true);
        slider("Damping", edit.sim.damping, 0.0f, 5.0f, "%.2f", false);
        int iterations = static_cast<int>(edit.sim.iterations);
        if (ImGui::SliderInt("Iterations", &iterations, 1, 24)) { edit.sim.iterations = static_cast<u32>(iterations); changed = true; }
        finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
        int maxNodes = static_cast<int>(edit.jelly.maxNodes);
        if (ImGui::SliderInt("Node cap", &maxNodes, 8, 8000)) { edit.jelly.maxNodes = static_cast<u32>(maxNodes); changed = true; }
        finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
        bool radiusOverride = edit.jelly.vertexRadiusCm >= 0.0f;
        if (ImGui::Checkbox("Own vertex radius", &radiusOverride)) {
            edit.jelly.vertexRadiusCm = radiusOverride ? 0.35f * edit.jelly.cellCm : -1.0f;
            changed = finished = true;
        }
        if (radiusOverride) slider("Vertex radius (cm)", edit.jelly.vertexRadiusCm, 0.0f, 20.0f, "%.2f", false);
        ImGui::EndDisabled();
        if (changed && editable) cache.asset = edit;
        if (finished && editable) {
            std::string why;
            if (!softbody::saveSoftAsset(absAsset, cache.asset, &why) && env.setStatus) env.setStatus("Could not save the asset: " + why);
        }
        if (!editable) ImGui::TextDisabled("%s", c->asset == 0 ? "Create an .ocsoft to override the preset." : (env.playing ? "Stop Play to edit." : "The asset is not a jelly asset."));
        ImGui::TreePop();
    }

    const bool built = (c->outState & (softbody::kShapeFailed)) == 0 && c->outNodes != 0;
    const std::string advice = shm::jellyAdvice(mr != nullptr, (c->outState & softbody::kShapeFailed) != 0, (c->outState & softbody::kShapeOverCap) != 0,
                                                (c->outState & softbody::kShapeAssetStale) != 0, shown.jelly.maxNodes);
    if (!advice.empty()) ImGui::TextColored(kWarn, "%s", advice.c_str());
    if (env.playing && built)
        ImGui::TextDisabled("%s", shm::jellyStateText((c->outState & softbody::kShapeAwake) != 0, (c->outState & softbody::kShapeFrozen) != 0, false, c->outNodes,
                                                      c->outVolumeRatio, c->outMaxStretch).c_str());
}

void drawRope(const SoftShapeEnv& env, scene::World& w, scene::Entity e) {
    const u32 type = softbody::SoftShapeSystem::ropeType();
    auto* c = type ? w.component<softbody::CSoftRope>(e, type) : nullptr;
    if (!c) return;
    if (!ImGui::CollapsingHeader("Rope", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (env.drawAssetRow) env.drawAssetRow(c->asset);
    const std::string assetRel = env.pathOfId ? env.pathOfId(c->asset) : std::string();
    const std::string absAsset = assetRel.empty() ? std::string() : env.contentDir + "/" + assetRel;
    static AssetCache cache;
    const bool ok = refreshAsset(cache, absAsset);
    const std::string problem = softmodel::assetProblem(c->asset, assetRel, ok);
    if (!problem.empty()) ImGui::TextColored(kWarn, "%s", problem.c_str());
    if (c->asset == 0) {
        ImGui::BeginDisabled(env.playing);
        if (ImGui::SmallButton("Create .ocsoft")) {
            const std::string rel = shm::ropeAssetPath(w.name(e));
            const std::filesystem::path target = std::filesystem::path(env.contentDir) / rel;
            std::error_code ec;
            if (std::filesystem::exists(target, ec)) {
                if (env.setStatus) env.setStatus(rel + " already exists; assign it instead.");
            } else {
                std::filesystem::create_directories(target.parent_path(), ec);
                std::string why;
                if (!softbody::saveSoftAsset(target.string(), shm::starterRope(), &why)) {
                    if (env.setStatus) env.setStatus("Could not write " + rel + ": " + why);
                } else {
                    if (env.adoptNewAsset) env.adoptNewAsset(target.string());
                    if (env.assignAsset) env.assignAsset(rel);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Without an asset the rope preset is used.");
    }

    // The two ends. An end pinned to an entity says which; its offset is in that entity's space.
    ImGui::BeginDisabled(env.playing);
    const char* endLabel[2] = {"Start", "End"};
    for (int end = 0; end < 2; ++end) {
        ImGui::PushID(end);
        scene::Entity& anchor = end == 0 ? c->startEntity : c->endEntity;
        f32* o = end == 0 ? c->startOffset : c->endOffset;
        const u32 pinBit = end == 0 ? softbody::kRopeNoStartPin : softbody::kRopeNoEndPin;
        float v[3] = {o[0], o[1], o[2]};
        const bool anchored = anchor != scene::kInvalidEntity && w.valid(anchor);
        char label[64];
        std::snprintf(label, sizeof label, "%s %s", endLabel[end], anchored ? "(in the anchor's space)" : "(world cm)");
        env.gestureEdit(ImGui::DragFloat3(label, v, 1.0f, -1.0e6f, 1.0e6f, "%.1f"), [&] { o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; });
        if (anchored) {
            ImGui::SameLine();
            ImGui::TextDisabled("pinned to %s", w.name(anchor));
            ImGui::SameLine();
            if (ImGui::SmallButton("Detach")) {
                const scene::Entity was = anchor;
                env.instantEdit(true, [&] {
                    const Mat4 m = w.worldMatrix(was);
                    const Vec3 world = shm::ropeEndWorld(&m, Vec3{o[0], o[1], o[2]});
                    anchor = scene::kInvalidEntity;
                    o[0] = world.x; o[1] = world.y; o[2] = world.z;
                });
            }
        }
        bool pinned = (c->flags & pinBit) == 0;
        env.instantEdit(ImGui::Checkbox("Pinned", &pinned), [&] { c->flags = pinned ? (c->flags & ~pinBit) : (c->flags | pinBit); });
        ImGui::PopID();
    }
    f32 length = c->lengthCm;
    env.gestureEdit(ImGui::DragFloat("Length (cm)", &length, 1.0f, 0.0f, 100000.0f, length > 0.0f ? "%.0f" : "distance x slack"), [&] { c->lengthCm = length; });
    f32 radius = c->radiusCm;
    env.gestureEdit(ImGui::DragFloat("Radius (cm)", &radius, 0.05f, 0.0f, 50.0f, radius > 0.0f ? "%.2f" : "asset default"), [&] { c->radiusCm = radius; });
    bool collides = (c->flags & softbody::kRopeNoWorld) == 0;
    env.instantEdit(ImGui::Checkbox("Collides with the world", &collides), [&] { c->flags = collides ? (c->flags & ~softbody::kRopeNoWorld) : (c->flags | softbody::kRopeNoWorld); });
    ImGui::EndDisabled();

    // The tube and the rods, on the loaded asset.
    const softbody::SoftAsset shown = ok && c->asset != 0 ? cache.asset : shm::starterRope();
    const bool editable = ok && c->asset != 0 && !env.playing && shown.kind == softbody::SoftKind::Rope;
    if (ImGui::TreeNode("Tube and rods")) {
        softbody::SoftAsset edit = shown;
        bool changed = false, finished = false;
        ImGui::BeginDisabled(!editable);
        const auto slider = [&](const char* label, f32& v, f32 lo, f32 hi, const char* format, bool log) {
            changed = ImGui::SliderFloat(label, &v, lo, hi, format, log ? ImGuiSliderFlags_Logarithmic : 0) || changed;
            finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
        };
        slider("Radius (cm)", edit.rope.radiusCm, 0.1f, 20.0f, "%.2f", true);
        slider("Segment (cm)", edit.rope.segmentCm, 2.0f, 100.0f, "%.1f", true);
        slider("Mass per metre (kg)", edit.rope.massPerMeterKg, 0.01f, 20.0f, "%.2f", true);
        slider("Slack", edit.rope.slack, 1.0f, 2.0f, "%.3f", false);
        slider("Stretch compliance", edit.rope.stretchCompliance, 0.0f, 1e-4f, "%.2e", false);
        slider("Bend-twist compliance", edit.rope.bendTwistCompliance, 0.0f, 1e-2f, "%.2e", false);
        slider("Texture tile (cm)", edit.rope.uvCmPerTile, 1.0f, 200.0f, "%.0f", true);
        slider("Damping", edit.sim.damping, 0.0f, 5.0f, "%.2f", false);
        int sides = static_cast<int>(edit.rope.sides);
        if (ImGui::SliderInt("Tube sides", &sides, 3, 32)) { edit.rope.sides = static_cast<u32>(sides); changed = true; }
        finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
        ImGui::EndDisabled();
        if (changed && editable) cache.asset = edit;
        if (finished && editable) {
            std::string why;
            if (!softbody::saveSoftAsset(absAsset, cache.asset, &why) && env.setStatus) env.setStatus("Could not save the asset: " + why);
        }
        if (!editable) ImGui::TextDisabled("%s", c->asset == 0 ? "Create an .ocsoft to override the preset." : (env.playing ? "Stop Play to edit." : "The asset is not a rope asset."));
        ImGui::TreePop();
    }

    const u32 nodes = (c->outState >> softbody::kShapeCountShift) + 1;
    const std::string advice = shm::ropeAdvice((c->outState & softbody::kShapeFailed) != 0, (c->outState & softbody::kShapeOverCap) != 0,
                                               (c->outState & softbody::kShapeAssetStale) != 0, (c->flags & softbody::kRopeNoStartPin) == 0,
                                               (c->flags & softbody::kRopeNoEndPin) == 0, nodes, softbody::softShapeSystem().options().maxRopeNodes);
    if (!advice.empty()) ImGui::TextColored(kWarn, "%s", advice.c_str());
    if (env.playing) {
        ImGui::TextDisabled("%s", shm::ropeStateText((c->outState & softbody::kShapeAwake) != 0, (c->outState & softbody::kShapeFrozen) != 0,
                                                     (c->outState & softbody::kShapeFailed) != 0, (c->outState & softbody::kShapeCut) != 0,
                                                     c->outState >> softbody::kShapeCountShift, c->outLengthCm, c->outStrain).c_str());
        if (ImGui::SmallButton("Cut at the middle")) {
            const Vec3* n = nullptr;
            u32 count = 0;
            if (softbody::softShapeSystem().ropeNodes(e, n, count) && count > 3) {
                scene::Entity other = scene::kInvalidEntity;
                softbody::softShapeSystem().cutRope(w, e, n[count / 2], other);
            }
        }
    }
}

}  // namespace

void drawSoftShapeSection(const SoftShapeEnv& env, void* worldPtr, u32 entity, bool rope) {
    auto& w = *static_cast<scene::World*>(worldPtr);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    if (rope) drawRope(env, w, e);
    else drawJelly(env, w, e);
}

bool drawClothShapeSections(const SoftSectionEnv& env, softbody::SoftAsset& asset, const std::string& absAssetPath, bool editable, bool closedKnown, bool closed) {
    namespace sm = softmodel;
    bool saved = false;
    if (!ImGui::TreeNode("Solver, inflate and group")) return false;
    softbody::SoftAsset edit = asset;
    bool changed = false, finished = false;
    ImGui::BeginDisabled(!editable);

    // Solver
    if (ImGui::BeginCombo("Solver", sm::solverLabel(edit.cloth.solver))) {
        for (const sm::SolverRow& row : sm::kSolverRows) {
            const bool picked = ImGui::Selectable(row.label, row.solver == edit.cloth.solver);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tip);
            if (picked && row.solver != edit.cloth.solver) { edit.cloth.solver = row.solver; changed = finished = true; }
        }
        ImGui::EndCombo();
    }
    const std::string solverNote = sm::solverAdvice(edit.cloth.solver, edit.cloth.tearable);
    if (!solverNote.empty()) ImGui::TextDisabled("%s", solverNote.c_str());
    if (edit.cloth.solver == softbody::ClothSolver::Fast) {
        if (ImGui::BeginCombo("Bend", sm::bendLabel(edit.cloth.bend))) {
            for (const sm::BendRow& row : sm::kBendRows)
                if (ImGui::Selectable(row.label, row.bend == edit.cloth.bend) && row.bend != edit.cloth.bend) { edit.cloth.bend = row.bend; changed = finished = true; }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo("Long range attachments", sm::lraLabel(edit.cloth.lra))) {
            for (const sm::LraRow& row : sm::kLraRows)
                if (ImGui::Selectable(row.label, row.lra == edit.cloth.lra) && row.lra != edit.cloth.lra) { edit.cloth.lra = row.lra; changed = finished = true; }
            ImGui::EndCombo();
        }
        if (edit.cloth.lra != softbody::ClothLra::Off) {
            changed = ImGui::SliderFloat("LRA slack", &edit.cloth.lraSlack, 1.0f, 1.5f, "%.3f") || changed;
            finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
        }
    }

    // Inflate
    changed = ImGui::SliderFloat("Pressure (kPa)", &edit.inflate.pressureKPa, 0.0f, 50.0f, "%.2f") || changed;
    finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
    changed = ImGui::SliderFloat("Leak (kPa/s)", &edit.inflate.leakKPaPerS, 0.0f, 20.0f, "%.2f") || changed;
    finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
    const std::string inflateNote = sm::inflateAdvice(edit.inflate.pressureKPa, closedKnown, closed, true);
    if (!inflateNote.empty()) ImGui::TextColored(kWarn, "%s", inflateNote.c_str());

    // Group
    if (ImGui::Checkbox("Ignore the owner's ragdoll", &edit.group.ragdollOwner)) changed = finished = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A cape never collides with the proxy ragdoll of the character that wears it.");
    ImGui::EndDisabled();

    if (changed && editable) asset = edit;
    if (finished && editable) {
        std::string why;
        if (!softbody::saveSoftAsset(absAssetPath, asset, &why)) {
            if (env.setStatus) env.setStatus("Could not save the asset: " + why);
        } else {
            saved = true;
        }
    }
    if (!editable) ImGui::TextDisabled("Create or assign an .ocsoft (and stop Play) to edit these.");
    ImGui::TreePop();
    return saved;
}

void drawClothPressureLive(const SoftSectionEnv& env, void* worldPtr, u32 entity) {
    (void)worldPtr;
    if (!env.playing) return;
    const scene::Entity e = static_cast<scene::Entity>(entity);
    softbody::SoftBodySystem& sys = softbody::softBodySystem();
    const f32 now = sys.pressureOf(e);
    if (now <= 0.0f && sys.bodyOf(e) == 0) return;
    f32 kPa = now;
    if (ImGui::SliderFloat("Pressure now (kPa)", &kPa, 0.0f, 50.0f, "%.2f") && softmodel::pressureOk(kPa)) sys.setPressure(e, kPa);
}
#endif  // AVER_WITH_IMGUI

}  // namespace aver::editor

#endif  // AVER_MODULE_SOFTBODY_SCENE
