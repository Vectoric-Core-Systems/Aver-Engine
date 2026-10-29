// Godot's AudioStreamPlayer3D maths; see Spatial.hpp.
#include "aver/audio/Spatial.hpp"

#include <algorithm>
#include <cmath>

namespace aver::audio {
namespace {
constexpr f32 kEps = 0.00001f;   // Godot's CMP_EPSILON
constexpr f32 kSpeedOfSound = 343.0f;

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
f32 dot3(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
f32 len3(const f32 a[3]) { return std::sqrt(dot3(a, a)); }
} // namespace

void Spatial3D::pack(f32 o[kFloats]) const {
    o[0] = static_cast<f32>(model); o[1] = unitSize; o[2] = maxDb; o[3] = maxDistance; o[4] = panningStrength; o[5] = globalPanningStrength;
    o[6] = coneEnabled ? 1.0f : 0.0f; o[7] = coneAngleDeg; o[8] = coneFilterDb; o[9] = filterCutoffHz; o[10] = filterDb;
    o[11] = emitDir[0]; o[12] = emitDir[1]; o[13] = emitDir[2]; o[14] = doppler ? 1.0f : 0.0f;
    o[15] = velocity[0]; o[16] = velocity[1]; o[17] = velocity[2];
}

Spatial3D Spatial3D::unpack(const f32 i[kFloats]) {
    Spatial3D s;
    s.model = static_cast<i32>(i[0]); s.unitSize = i[1]; s.maxDb = i[2]; s.maxDistance = i[3]; s.panningStrength = i[4]; s.globalPanningStrength = i[5];
    s.coneEnabled = i[6] > 0.5f; s.coneAngleDeg = i[7]; s.coneFilterDb = i[8]; s.filterCutoffHz = i[9]; s.filterDb = i[10];
    s.emitDir[0] = i[11]; s.emitDir[1] = i[12]; s.emitDir[2] = i[13]; s.doppler = i[14] > 0.5f;
    s.velocity[0] = i[15]; s.velocity[1] = i[16]; s.velocity[2] = i[17];
    return s;
}

f32 attenuationDb(AttenuationModel model, f32 d, f32 unitSize) {
    const f32 u = unitSize > 1.0e-6f ? unitSize : 1.0e-6f;
    switch (model) {
        case AttenuationModel::InverseDistance: return linToDb(1.0f / (d / u + kEps));
        case AttenuationModel::InverseSquareDistance: { const f32 x = d / u; return linToDb(1.0f / (x * x + kEps)); }
        case AttenuationModel::Logarithmic: return -20.0f * std::log(d / u + kEps);
        default: return 0.0f;
    }
}

void godotPanGains(f32 cosx, f32 panningStrength, f32 globalStrength, f32& left, f32& right) {
    const f32 s = panningStrength * globalStrength;
    const f32 g = clampf((1.0f - s) * (1.0f - s), 0.0f, 1.0f);
    const f32 f = (1.0f - g) / (1.0f + g);
    const f32 fc = clampf(cosx, -1.0f, 1.0f) * f;
    left = std::sqrt((1.0f - fc) * 0.5f);
    right = std::sqrt((1.0f + fc) * 0.5f);
}

SpatialResult spatialize(const Spatial3D& s, f32 volume, const f32 L[3], const f32 fwd[3], const f32 right[3], const f32 src[3]) {
    SpatialResult r;
    const f32 d[3] = {src[0] - L[0], src[1] - L[1], src[2] - L[2]};
    const f32 distM = len3(d) * 0.01f;

    f32 attDb = attenuationDb(static_cast<AttenuationModel>(s.model), distM, s.unitSize) + linToDb(volume);
    if (attDb > s.maxDb) attDb = s.maxDb;
    f32 mult = dbToLin(attDb);
    if (s.maxDistance > 0.0f) {
        if (distM > s.maxDistance) { r.silent = true; r.distanceGain = 0.0f; return r; }
        mult *= std::max(0.0f, 1.0f - distM / s.maxDistance);
    }
    r.distanceGain = mult;

    // Pan: the lateral offset over the horizontal distance, in the listener's frame (Godot: +X right, +Z behind).
    const f32 lateral = dot3(d, right), back = -dot3(d, fwd);
    const f32 radius = std::sqrt(lateral * lateral + back * back);
    const f32 cosx = radius > 1.0e-6f ? clampf(lateral / radius, -1.0f, 1.0f) : 0.0f;
    f32 pl, pr;
    godotPanGains(cosx, s.panningStrength, s.globalPanningStrength, pl, pr);
    r.gainL = pl * mult;
    r.gainR = pr * mult;

    // The attenuation filter: a high shelf that falls as the distance gain falls, and further outside the cone.
    f32 filterDb = (1.0f - std::min(1.0f, mult)) * s.filterDb;
    if (s.coneEnabled) {
        const f32 dist = len3(d);
        f32 c = 1.0f;
        if (dist > 1.0e-6f) {
            const f32 toListener[3] = {-d[0] / dist, -d[1] / dist, -d[2] / dist};
            c = clampf(dot3(toListener, s.emitDir), -1.0f, 1.0f);
        }
        r.coneAngleDeg = std::acos(c) * 57.2957795f;
        if (r.coneAngleDeg > s.coneAngleDeg) filterDb += s.coneFilterDb;
    }
    r.filterGain = (s.filterCutoffHz >= 20500.0f) ? 1.0f : dbToLin(filterDb);
    return r;
}

f32 dopplerPitch(const f32 L[3], const f32 lv[3], const f32 src[3], const f32 sv[3]) {
    const f32 rel[3] = {(sv[0] - lv[0]) * 0.01f, (sv[1] - lv[1]) * 0.01f, (sv[2] - lv[2]) * 0.01f};
    const f32 speed = len3(rel);
    if (speed < 1.0e-6f) return 1.0f;
    const f32 d[3] = {src[0] - L[0], src[1] - L[1], src[2] - L[2]};
    const f32 dist = len3(d);
    if (dist < 1.0e-6f) return 1.0f;
    const f32 approaching = (rel[0] * d[0] + rel[1] * d[1] + rel[2] * d[2]) / (speed * dist);
    return clampf(kSpeedOfSound / (kSpeedOfSound + speed * approaching), 0.125f, 8.0f);
}

} // namespace aver::audio
