// World-space lights from the scene: every CLight entity resolved against its world matrix.
// Header-only; the renderer side (voxi::SceneLight) is filled from this by the host.
#pragma once
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <vector>

namespace aver::scene {

// Defaults for the zero-means-default CLight fields.
inline constexpr f32 kLightDefaultRectCm   = 100.0f;
inline constexpr f32 kLightDefaultRadiusCm = 1.0f;

// One light in world space. Axis and right are unit and perpendicular; the rect's height axis is
// cross(axis, right) (sign is immaterial for a symmetric panel).
struct WorldLight {
    Entity entity = kInvalidEntity;
    i32    kind   = kLightPoint;
    f32    pos[3]    = {0, 0, 0};
    f32    axis[3]   = {1, 0, 0};
    f32    right[3]  = {0, 1, 0};
    f32    colour[3] = {1, 1, 1};
    f32    intensityCd = 0.0f;
    f32    rangeCm = 0.0f;
    f32    innerCos = 1.0f, outerCos = 0.7f;
    f32    widthCm = kLightDefaultRectCm, heightCm = kLightDefaultRectCm;
    f32    sourceRadiusCm = kLightDefaultRadiusCm;
    bool   castShadows = true;
    bool   iesPeak = false;
    i64    iesProfile = 0, cookie = 0;
    // G4 R2 (CLight's Godot block, resolved): godot = kLightGodot set; the rest are the stored values (0 = default).
    bool   godot = false, negative = false;
    f32    attenuation = 0.0f, spotAttenuation = 0.0f, specularScale = 1.0f;
    f32    shadowBias = 0.0f, shadowNormalBias = 0.0f;
    f32    fadeBeginCm = 0.0f, fadeLengthCm = 0.0f, fadeShadowCm = 0.0f;
    // G4 R4: CLight::cullInv / shadowCasterInv as stored (inverted masks, 0 = every layer; RenderMasks.hpp).
    u32    cullInv = 0, shadowCasterInv = 0;
};

namespace detail {
inline bool unit3(const f32 in[3], f32 out[3]) {
    const f32 l2 = in[0] * in[0] + in[1] * in[1] + in[2] * in[2];
    if (!(l2 > 1e-12f) || !std::isfinite(l2)) return false;
    const f32 inv = 1.0f / std::sqrt(l2);
    out[0] = in[0] * inv; out[1] = in[1] * inv; out[2] = in[2] * inv;
    return true;
}
} // namespace detail

// Resolves one CLight against its world matrix (row-vector: rows 0/1 are local +X/+Y, row 3 the
// translation). False for a light that cannot contribute: directional (the sun owns those), no
// intensity, or a degenerate matrix.
inline bool makeWorldLight(const CLight& c, const Mat4& m, WorldLight& out) {
    if (c.kind != kLightPoint && c.kind != kLightSpot && c.kind != kLightRect) return false;
    if (!(c.intensityLux > 0.0f) || !std::isfinite(c.intensityLux)) return false;

    WorldLight w;
    w.kind = c.kind;
    w.pos[0] = m.m[3][0]; w.pos[1] = m.m[3][1]; w.pos[2] = m.m[3][2];
    const f32 x[3] = {m.m[0][0], m.m[0][1], m.m[0][2]};
    if (!detail::unit3(x, w.axis)) return false;

    // Gram-Schmidt on local +Y so a sheared parent cannot leave right off the axis.
    f32 y[3] = {m.m[1][0], m.m[1][1], m.m[1][2]};
    const f32 d = y[0] * w.axis[0] + y[1] * w.axis[1] + y[2] * w.axis[2];
    for (int i = 0; i < 3; ++i) y[i] -= d * w.axis[i];
    if (!detail::unit3(y, w.right)) {
        const f32 up[3] = {std::fabs(w.axis[2]) < 0.9f ? 0.0f : 1.0f, 0.0f,
                           std::fabs(w.axis[2]) < 0.9f ? 1.0f : 0.0f};
        const f32 cr[3] = {up[1] * w.axis[2] - up[2] * w.axis[1],
                           up[2] * w.axis[0] - up[0] * w.axis[2],
                           up[0] * w.axis[1] - up[1] * w.axis[0]};
        if (!detail::unit3(cr, w.right)) return false;
    }

    for (int i = 0; i < 3; ++i) w.colour[i] = c.colour[i] > 0.0f ? c.colour[i] : 0.0f;
    w.intensityCd = c.intensityLux;
    w.rangeCm = c.rangeCm > 0.0f ? c.rangeCm : 0.0f;
    w.innerCos = c.innerCos;
    w.outerCos = c.outerCos;
    w.widthCm  = c.widthCm  > 0.0f ? c.widthCm  : kLightDefaultRectCm;
    w.heightCm = c.heightCm > 0.0f ? c.heightCm : kLightDefaultRectCm;
    w.sourceRadiusCm = c.sourceRadiusCm > 0.0f ? c.sourceRadiusCm : kLightDefaultRadiusCm;
    w.castShadows = (c.flags & kLightNoShadows) == 0;
    w.iesPeak = (c.flags & kLightIesPeak) != 0;
    w.iesProfile = c.iesProfile;
    w.cookie = c.cookie;
    w.godot = (c.flags & kLightGodot) != 0;
    w.negative = (c.flags & kLightNegative) != 0;
    if (w.godot) {
        w.attenuation = c.attenuation >= 0.0f ? c.attenuation : 0.0f;
        w.spotAttenuation = c.spotAttenuation > 0.0f ? c.spotAttenuation : 1.0f;
        w.specularScale = c.specularScale >= 0.0f ? c.specularScale : 0.0f;
    }
    w.shadowBias = c.shadowBias > 0.0f ? c.shadowBias : 0.0f;
    w.shadowNormalBias = c.shadowNormalBias > 0.0f ? c.shadowNormalBias : 0.0f;
    w.fadeBeginCm = c.fadeBeginCm > 0.0f ? c.fadeBeginCm : 0.0f;
    w.fadeLengthCm = c.fadeLengthCm > 0.0f ? c.fadeLengthCm : 0.0f;
    w.fadeShadowCm = c.fadeShadowCm > 0.0f ? c.fadeShadowCm : 0.0f;
    w.cullInv = c.cullInv & 0xFFFFFu;
    w.shadowCasterInv = c.shadowCasterInv & 0xFFFFFu;
    out = w;
    return true;
}

// Appends every contributing CLight in `world`, in the pool's dense order.
inline void gatherLights(World& world, std::vector<WorldLight>& out) {
    ComponentPool* pool = world.pool(kComponentLight);
    if (!pool) return;
    for (usize i = 0; i < pool->size(); ++i) {
        const Entity e = pool->entityAt(i);
        if (!world.valid(e) || world.destroyPending(e)) continue;
        const CLight* c = static_cast<const CLight*>(pool->dataAt(i));
        WorldLight w;
        if (!c || !makeWorldLight(*c, world.worldMatrix(e), w)) continue;
        w.entity = e;
        out.push_back(w);
    }
}

} // namespace aver::scene
