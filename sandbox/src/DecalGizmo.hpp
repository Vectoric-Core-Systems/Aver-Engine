#pragma once
// The decal box gizmo in the viewport: a wireframe projector box with a direction arrow for the
// selected decal (dim boxes for the rest while "Show Decals" is on), and face handles that resize
// the box by dragging. Math in DecalGizmoMath.hpp; this is the editor-lines side.
// Wiring: docs/rendering/DECALS.md ("Editor wiring").
#include "DecalGizmoMath.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/World.hpp"

#include <vector>

namespace aver::editor {

class DecalGizmo {
public:
    // Once per frame, in the overlay stage beside the other gizmos. `selected` may be invalid.
    // Skips everything when nothing is selected and showAll is off.
    void draw(rhi::IDevice& dev, scene::World& world, scene::Entity selected, bool showAll) {
        if (mesh_) { dev.destroyLineMesh(mesh_); mesh_ = 0; }   // last frame's, replayed by now
        verts_.clear();
        if (showAll) {
            if (scene::ComponentPool* pool = world.pool(scene::kComponentDecal)) {
                for (usize i = 0; i < pool->size(); ++i) {
                    const scene::Entity e = pool->entityAt(i);
                    if (e == selected || !world.valid(e)) continue;
                    const auto* c = static_cast<const scene::CDecal*>(pool->dataAt(i));
                    if (c->flags & (scene::kDecalDisabled | scene::kDecalPooled)) continue;
                    addBox(decalBoxOf(*c, world.worldMatrix(e)), 0.35f, 0.35f, 0.45f);
                }
            }
        }
        if (selected != scene::kInvalidEntity && world.valid(selected)) {
            if (const auto* c = world.component<scene::CDecal>(selected, scene::kComponentDecal)) {
                const DecalBox b = decalBoxOf(*c, world.worldMatrix(selected));
                addBox(b, 1.0f, 0.75f, 0.1f);
                for (int axis = 0; axis < 3; ++axis)
                    for (int sign = -1; sign <= 1; sign += 2) addHandle(decalHandlePos(b, {axis, sign}), b, dragging_ && handle_.axis == axis && handle_.sign == sign);
            }
        }
        if (verts_.empty()) return;
        mesh_ = dev.createLineMesh(verts_.data(), static_cast<u32>(verts_.size()));
        if (!mesh_) return;
        const Mat4 identity = Mat4::identity();   // vertices are already in world space
        dev.setLineDepth(false);                  // a decal box is usually inside the geometry it paints
        dev.drawLines(mesh_, &identity.m[0][0]);
        dev.setLineDepth(true);
    }

    // Left button went down with this world-space ray. True when it grabbed a handle (the caller
    // then skips its own pick). `pixelWorldCm` is the world size of one pixel at the decal, for a
    // pick radius that stays the same on screen.
    bool beginDrag(scene::World& world, scene::Entity e, const Vec3& o, const Vec3& d, f32 pixelWorldCm) {
        const auto* c = world.component<scene::CDecal>(e, scene::kComponentDecal);
        if (!c) return false;
        const DecalBox b = decalBoxOf(*c, world.worldMatrix(e));
        handle_ = pickDecalHandle(b, o, d, std::max(6.0f * pixelWorldCm, 2.0f));
        dragging_ = handle_.valid();
        entity_ = e;
        return dragging_;
    }

    // Mouse moved while dragging. Writes the new size; true when the decal changed.
    bool updateDrag(scene::World& world, const Vec3& o, const Vec3& d) {
        if (!dragging_) return false;
        auto* c = world.component<scene::CDecal>(entity_, scene::kComponentDecal);
        if (!c) { dragging_ = false; return false; }
        const DecalBox b = decalBoxOf(*c, world.worldMatrix(entity_));
        const f32 size = dragDecalHandleSize(b, handle_, o, d);
        if (size < 0.0f) return false;
        c->sizeCm[handle_.axis] = size;
        return true;
    }

    void endDrag() { dragging_ = false; handle_ = {}; }
    bool dragging() const { return dragging_; }

    // Releases the line mesh (shutdown, device reset).
    void release(rhi::IDevice& dev) { if (mesh_) { dev.destroyLineMesh(mesh_); mesh_ = 0; } }

private:
    void addSeg(const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
        verts_.push_back({a.x, a.y, a.z, r, g, bl});
        verts_.push_back({b.x, b.y, b.z, r, g, bl});
    }
    void addBox(const DecalBox& b, f32 r, f32 g, f32 bl) {
        segs_.clear();
        decalBoxSegments(b, segs_);
        for (usize i = 0; i + 1 < segs_.size(); i += 2) addSeg(segs_[i], segs_[i + 1], r, g, bl);
    }
    // A small cross at a handle; brighter while grabbed.
    void addHandle(const Vec3& p, const DecalBox& b, bool active) {
        const f32 r = 0.04f * std::min(b.half.x, std::min(b.half.y, b.half.z)) + 1.0f;
        const f32 c = active ? 1.0f : 0.6f;
        for (int i = 0; i < 3; ++i) addSeg(p - b.axis[i] * r, p + b.axis[i] * r, c, c, 0.2f);
    }

    rhi::LineHandle mesh_ = 0;
    std::vector<rhi::LineVertex> verts_;
    std::vector<Vec3> segs_;
    DecalHandle handle_;
    scene::Entity entity_ = scene::kInvalidEntity;
    bool dragging_ = false;
};

} // namespace aver::editor
