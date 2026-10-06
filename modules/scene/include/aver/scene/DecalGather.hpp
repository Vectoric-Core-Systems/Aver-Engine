// Projected decals from the scene: every active CDecal entity resolved against its world matrix, the
// zero-means-default readers for CDecal, and the lifetime clock. Header-only; the renderer side
// (voxi::SceneDecal) is filled from WorldDecal by the host (Runtime/include/aver/game/SceneDecalFeed.hpp).
// docs/rendering/DECALS.md.
#pragma once
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aver::scene {

// Defaults for the zero-means-unset CDecal fields.
inline constexpr f32 kDecalDefaultSizeCm        = 100.0f;
inline constexpr f32 kDecalDefaultEdgeFade      = 0.1f;
inline constexpr f32 kDecalDefaultAngleStartDeg = 60.0f;
inline constexpr f32 kDecalDefaultAngleEndDeg   = 85.0f;

inline f32 decalSizeCm(const CDecal& d, int axis) { return d.sizeCm[axis] > 0.0f ? d.sizeCm[axis] : kDecalDefaultSizeCm; }
inline f32 decalNormalStrength(const CDecal& d) { return d.normalStrength > 0.0f ? d.normalStrength : 1.0f; }
inline f32 decalEdgeFade(const CDecal& d) { return d.edgeFade > 0.0f ? std::min(d.edgeFade, 1.0f) : kDecalDefaultEdgeFade; }
inline f32 decalUvScale(const CDecal& d, int axis) { return d.uvScale[axis] != 0.0f ? d.uvScale[axis] : 1.0f; }
inline void decalTint(const CDecal& d, f32 out[3]) {
    const bool unset = d.tint[0] == 0.0f && d.tint[1] == 0.0f && d.tint[2] == 0.0f;
    for (int i = 0; i < 3; ++i) out[i] = unset ? 1.0f : std::max(d.tint[i], 0.0f);
}
// The cone of surface angles a decal covers, as degrees; end is kept at or past start.
inline f32 decalAngleStartDeg(const CDecal& d) {
    return std::min(d.angleFadeStartDeg > 0.0f ? d.angleFadeStartDeg : kDecalDefaultAngleStartDeg, 89.0f);
}
inline f32 decalAngleEndDeg(const CDecal& d) {
    const f32 e = std::min(d.angleFadeEndDeg > 0.0f ? d.angleFadeEndDeg : kDecalDefaultAngleEndDeg, 90.0f);
    return std::max(e, decalAngleStartDeg(d) + 0.5f);
}

// 1 while alive, ramping to 0 over fadeOutSec at the end of lifetimeSec; 0 once expired.
// A permanent decal (lifetimeSec <= 0) is always 1.
inline f32 decalLifeFade(f32 age, f32 lifetimeSec, f32 fadeOutSec) {
    if (!(lifetimeSec > 0.0f)) return 1.0f;
    if (age >= lifetimeSec) return 0.0f;
    if (!(fadeOutSec > 0.0f)) return 1.0f;
    const f32 left = lifetimeSec - age;
    return left >= fadeOutSec ? 1.0f : std::max(left / fadeOutSec, 0.0f);
}

// A decal in world space, defaults applied, lifetime fade folded into `opacity`.
struct WorldDecal {
    Entity entity = kInvalidEntity;
    f32 world[16] = {};            // row-vector world matrix of the projector (scale included)
    f32 halfExtentsCm[3] = {50, 50, 50};   // in the entity's local units (the matrix scales them)
    f32 tint[3] = {1, 1, 1};
    f32 opacity = 1.0f;
    f32 normalStrength = 1.0f;
    f32 roughness = 0.0f;          // 0 = leave to the ORM texture / do not touch (see rough flag)
    f32 metallic = 0.0f;
    f32 edgeFade = kDecalDefaultEdgeFade;
    f32 angleFadeStartDeg = kDecalDefaultAngleStartDeg;
    f32 angleFadeEndDeg = kDecalDefaultAngleEndDeg;
    f32 fadeDistanceCm = 0.0f;
    i32 sortOrder = 0;
    bool colour = true, normal = true, roughnessMetal = true;   // channels that may apply
    i64 baseTexture = 0, normalTexture = 0, ormTexture = 0;
    f32 uvScale[2] = {1, 1};
    f32 uvOffset[2] = {0, 0};
};

// Resolves one CDecal against its world matrix. False for a decal that cannot paint: disabled, fully
// transparent or expired, no channel left, or a degenerate matrix.
inline bool makeWorldDecal(const CDecal& c, const Mat4& m, WorldDecal& out) {
    if (c.flags & kDecalDisabled) return false;
    const f32 life = decalLifeFade(c.age, c.lifetimeSec, c.fadeOutSec);
    const f32 opacity = (1.0f - std::min(std::max(c.transparency, 0.0f), 1.0f)) * life;
    if (!(opacity > 0.0f) || !std::isfinite(opacity)) return false;
    const bool colour = (c.flags & kDecalNoColour) == 0;
    const bool normal = (c.flags & kDecalNoNormal) == 0 && c.normalTexture != 0;
    const bool rough  = (c.flags & kDecalNoRoughness) == 0 && (c.ormTexture != 0 || c.roughness > 0.0f || c.metallic > 0.0f);
    if (!colour && !normal && !rough) return false;
    for (int r = 0; r < 3; ++r) {   // a collapsed axis has no inverse
        const f32 l2 = m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2];
        if (!(l2 > 1e-10f) || !std::isfinite(l2)) return false;
    }
    WorldDecal w;
    std::copy(&m.m[0][0], &m.m[0][0] + 16, w.world);
    for (int i = 0; i < 3; ++i) w.halfExtentsCm[i] = 0.5f * decalSizeCm(c, i);
    decalTint(c, w.tint);
    w.opacity = opacity;
    w.normalStrength = decalNormalStrength(c);
    w.roughness = c.roughness > 0.0f ? c.roughness : 1.0f;
    w.metallic = std::max(c.metallic, 0.0f);
    w.edgeFade = decalEdgeFade(c);
    w.angleFadeStartDeg = decalAngleStartDeg(c);
    w.angleFadeEndDeg = decalAngleEndDeg(c);
    w.fadeDistanceCm = std::max(c.fadeDistanceCm, 0.0f);
    w.sortOrder = c.sortOrder;
    w.colour = colour; w.normal = normal; w.roughnessMetal = rough;
    w.baseTexture = c.baseTexture; w.normalTexture = c.normalTexture; w.ormTexture = c.ormTexture;
    w.uvScale[0] = decalUvScale(c, 0); w.uvScale[1] = decalUvScale(c, 1);
    w.uvOffset[0] = c.uvOffset[0]; w.uvOffset[1] = c.uvOffset[1];
    out = w;
    return true;
}

// Appends every paintable CDecal in `world`, in the pool's dense order.
inline void gatherDecals(World& world, std::vector<WorldDecal>& out) {
    ComponentPool* pool = world.pool(kComponentDecal);
    if (!pool) return;
    for (usize i = 0; i < pool->size(); ++i) {
        const Entity e = pool->entityAt(i);
        if (!world.valid(e) || world.destroyPending(e)) continue;
        const CDecal* c = static_cast<const CDecal*>(pool->dataAt(i));
        WorldDecal w;
        if (!c || !makeWorldDecal(*c, world.worldMatrix(e), w)) continue;
        w.entity = e;
        out.push_back(w);
    }
}

// Ages every CDecal with a lifetime by dt seconds. An expired decal is disabled (not destroyed);
// its entity goes into `expired` so an owner (a pool) can take it back. Safe on any world.
inline void tickDecalLifetimes(World& world, f32 dt, std::vector<Entity>* expired = nullptr) {
    ComponentPool* pool = world.pool(kComponentDecal);
    if (!pool || !(dt > 0.0f)) return;
    for (usize i = 0; i < pool->size(); ++i) {
        CDecal* c = static_cast<CDecal*>(pool->dataAt(i));
        if (!c || (c->flags & kDecalDisabled) || !(c->lifetimeSec > 0.0f)) continue;
        c->age += dt;
        if (c->age >= c->lifetimeSec) {
            c->flags |= kDecalDisabled;
            if (expired) expired->push_back(pool->entityAt(i));
        }
    }
}

} // namespace aver::scene
