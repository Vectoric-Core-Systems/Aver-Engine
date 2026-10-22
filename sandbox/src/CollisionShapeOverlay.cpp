// Show > Collision Shapes. See CollisionShapeOverlay.hpp for what is drawn and how the host hooks it in.
#include "CollisionShapeOverlay.hpp"

#include "aver/runtime/Engine.hpp"

#include <algorithm>
#include <cmath>

#if AVER_MODULE_SCENE
#  include "aver/scene/Components.hpp"
#  include "aver/scene/World.hpp"
#endif
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_shape_builder_abi.h"
#endif

namespace aver::editor {

CollisionShapeOverlayOptions& collisionShapeOverlayOptions() {
    static CollisionShapeOverlayOptions o;
    return o;
}

CollisionShapePreview& collisionShapePreview() {
    static CollisionShapePreview p;
    return p;
}

void surfaceColour(i32 surface, f32& r, f32& g, f32& b) {
    if (surface == 0) { r = 0.62f; g = 0.62f; b = 0.66f; return; }
    // A multiplicative hash spreads neighbouring ids across the hue circle; saturation and value are fixed so every surface reads well.
    const u32 h = static_cast<u32>(surface) * 2654435761u;
    const f32 hue = static_cast<f32>((h >> 8) % 360u) / 60.0f;   // 0..6
    const f32 s = 0.65f, v = 1.0f;
    const int sector = static_cast<int>(hue) % 6;
    const f32 f = hue - std::floor(hue);
    const f32 p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

namespace {
void line(CollisionShapeFrame& f, const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
    f.lines.push_back(rhi::LineVertex{a.x, a.y, a.z, r, g, bl});
    f.lines.push_back(rhi::LineVertex{b.x, b.y, b.z, r, g, bl});
}

Vec3 carried(const Vec3& p, const OverlayPose* pose) { return pose ? pose->position + pose->rotation.rotate(p) : p; }
} // namespace

void collisionShapeFeedTriangles(const f32* tris, const i32* surfaces, usize triCount, const OverlayPose* pose, CollisionShapeFrame& out) {
    if (!tris) return;
    for (usize t = 0; t < triCount; ++t) {
        const f32* v = tris + t * 9;
        const Vec3 a = carried(Vec3{v[0], v[1], v[2]}, pose), b = carried(Vec3{v[3], v[4], v[5]}, pose), c = carried(Vec3{v[6], v[7], v[8]}, pose);
        f32 r, g, bl;
        surfaceColour(surfaces ? surfaces[t] : 0, r, g, bl);
        line(out, a, b, r, g, bl);
        line(out, b, c, r, g, bl);
        line(out, c, a, r, g, bl);
    }
}

void collisionShapeFeedCross(const Vec3& at, f32 size, f32 r, f32 g, f32 b, CollisionShapeFrame& out) {
    line(out, at - Vec3{size, 0, 0}, at + Vec3{size, 0, 0}, r, g, b);
    line(out, at - Vec3{0, size, 0}, at + Vec3{0, size, 0}, r, g, b);
    line(out, at - Vec3{0, 0, size}, at + Vec3{0, 0, size}, r, g, b);
}

#if AVER_MODULE_PHYSICS
namespace {

// One body: its triangles, tinted by surface, and the two crosses.
void feedBody(int32_t body, const CollisionShapeOverlayOptions& opts, std::vector<f32>& triBuf, std::vector<i32>& surfBuf, CollisionShapeFrame& out) {
    const int cap = std::max(1, opts.maxTrianglesPerBody);
    triBuf.resize(static_cast<usize>(cap) * 9);
    surfBuf.resize(static_cast<usize>(cap));
    const int n = aver_phys_body_triangles(body, nullptr, nullptr, triBuf.data(), surfBuf.data(), cap);
    if (n > 0) collisionShapeFeedTriangles(triBuf.data(), surfBuf.data(), static_cast<usize>(n), nullptr, out);
    if (!opts.crosses) return;
    float origin[3] = {}, com[3] = {};
    if (aver_phys_body_origin(body, origin)) collisionShapeFeedCross(Vec3{origin[0], origin[1], origin[2]}, 6.0f, 0.2f, 0.9f, 0.95f, out);
    if (aver_phys_body_position(body, com)) collisionShapeFeedCross(Vec3{com[0], com[1], com[2]}, 4.0f, 1.0f, 0.85f, 0.2f, out);
}

} // namespace

void collisionShapeCollect(const CollisionShapeOverlayOptions& opts, int selectedBody, CollisionShapeFrame& out) {
    out.clear();
    if (!aver_phys_ready()) return;
    std::vector<f32> triBuf;
    std::vector<i32> surfBuf;
    if (opts.selected && selectedBody > 0) feedBody(selectedBody, opts, triBuf, surfBuf, out);
    if (!opts.movers) return;
    const int32_t count = aver_phys_body_count();
    if (count <= 0) return;
    std::vector<int32_t> handles(static_cast<usize>(count));
    const int32_t got = aver_phys_body_handles(handles.data(), count);
    int drawn = 0;
    for (int32_t i = 0; i < got && drawn < opts.maxBodies; ++i) {
        const int32_t body = handles[static_cast<usize>(i)];
        if (body == selectedBody || aver_phys_body_motion_type(body) == AVER_PHYS_MOTION_STATIC) continue;
        feedBody(body, opts, triBuf, surfBuf, out);
        ++drawn;
    }
}
#endif

void collisionShapeOverlayRender(Engine& e, u32 selectedEntity, bool noEditorChrome) {
    static rhi::LineHandle s_mesh = 0;
    static CollisionShapeFrame s_frame;
    if (s_mesh) { e.device()->destroyLineMesh(s_mesh); s_mesh = 0; }
    const CollisionShapeOverlayOptions& opts = collisionShapeOverlayOptions();
    const CollisionShapePreview& preview = collisionShapePreview();
    s_frame.clear();
    if (noEditorChrome || (!opts.enabled && !preview.active)) return;

    int selectedBody = 0;
    OverlayPose pose;
    bool havePose = false;
#if AVER_MODULE_SCENE
    scene::World& w = scene::World::instance();
    if (selectedEntity != scene::kInvalidEntity && w.valid(static_cast<scene::Entity>(selectedEntity))) {
        const scene::Entity se = static_cast<scene::Entity>(selectedEntity);
        if (const auto* rb = w.component<scene::CRigidBody>(se, scene::kComponentRigidBody)) selectedBody = rb->body;
        const Transform xf = transformFromMatrix(w.worldMatrix(se));
        pose.position = xf.position;
        pose.rotation = xf.rotation;
        havePose = true;
    }
#else
    (void)selectedEntity;
#endif
#if AVER_MODULE_PHYSICS
    if (opts.enabled) collisionShapeCollect(opts, selectedBody, s_frame);
#else
    (void)selectedBody;
#endif
    if (preview.active && !preview.triangles.empty())
        collisionShapeFeedTriangles(preview.triangles.data(), preview.surfaces.empty() ? nullptr : preview.surfaces.data(), preview.triangles.size() / 9,
                                    havePose ? &pose : nullptr, s_frame);
    if (s_frame.lines.empty()) return;
    s_mesh = e.device()->createLineMesh(s_frame.lines.data(), static_cast<u32>(s_frame.lines.size()));
    const Mat4 id = Mat4::identity();
    if (s_mesh) e.device()->drawLines(s_mesh, &id.m[0][0]);
}

} // namespace aver::editor
