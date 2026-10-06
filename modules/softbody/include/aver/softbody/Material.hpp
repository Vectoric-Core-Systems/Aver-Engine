// Soft-body material: the yield curve and the break limits of one constraint.
//
// Elastic below `bendForceN`, permanent set above it (the rest length creeps toward the stretched
// length at a rate set by `plasticStiffness`), broken above `breakForceN`, `breakStrain` or after
// `maxBend` cm of accumulated set. Units are the engine's: centimetres, Newtons, N per cm.
#pragma once

#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {

// How a material fails. Deform bends and tears; Fracture and Shatter never dent, they snap at the
// yield force (carbon, glass).
enum class Behavior : u8 { Deform = 0, Fracture = 1, Shatter = 2 };

inline bool isBrittle(Behavior b) { return b != Behavior::Deform; }

struct Material {
    f32 stiffness        = 0.7f;      // PBD relaxation, 0..1
    f32 axialStiffness   = 3500.0f;   // N/cm: force = axial * stretch
    f32 bendForceN       = 3000.0f;   // yield force: permanent set begins here
    f32 breakForceN      = 12000.0f;  // instant snap
    f32 plasticStiffness = 800.0f;    // N/cm of creep resistance after yield
    f32 maxBend          = 8.0f;      // accumulated set (cm) before the constraint tears
    f32 bendAbsorb       = 0.3f;      // energy soaked per cm of set, 0..1
    f32 breakAbsorb      = 0.6f;      // energy soaked on snap; the rest recoils
    f32 hardening        = 0.0f;      // raises the yield with accumulated set (0 = perfectly plastic)
    f32 breakStrain      = 0.0f;      // tears past this stretch of the ORIGINAL length; 0 = off
    Behavior behavior    = Behavior::Deform;
};

// ---- impact helpers (pure functions, so a test can pin each formula) -----------------------------

struct CrushParams {
    f32  knee        = 50.0f;   // cm: below this the depth passes through unchanged
    f32  ceilingMult = 1.8f;    // asymptote = knee * this
    bool nonlinear   = true;    // false: a hard clamp at the knee
};

// Soft-knee crush curve: identity to the knee, then C1-continuous and asymptotic to the ceiling.
inline f32 crushCurve(f32 raw, const CrushParams& p) {
    const f32 ceilCm = p.knee * std::max(1.0f, p.ceilingMult);
    if (!p.nonlinear || raw <= p.knee || ceilCm <= p.knee) return std::min(raw, p.knee);
    const f32 span = ceilCm - p.knee;
    return p.knee + span * (1.0f - std::exp(-(raw - p.knee) / span));
}

// Radius within which an impact snaps brittle constraints outright. 0 below `minDisp`.
inline f32 impactTearRadius(f32 disp, f32 radius, f32 maxStep, f32 radiusFrac = 0.5f,
                            f32 minDisp = 2.0f) {
    if (radiusFrac <= 0.0f || disp < minDisp || maxStep <= 0.0f) return 0.0f;
    const f32 depthFrac = std::clamp(disp / maxStep, 0.3f, 1.0f);
    return radius * std::clamp(radiusFrac, 0.0f, 1.0f) * depthFrac;
}

// Raw depth (cm) from a rigid-body normal impulse magnitude.
inline f32 depthFromImpulse(f32 impulse, f32 scale = 0.01f) { return impulse * scale; }
// Raw depth (cm) from a closing speed in km/h.
inline f32 depthFromSpeedKmh(f32 kmh, f32 scale = 0.01f) { return kmh * 27.78f * scale; }

} // namespace aver::softbody
