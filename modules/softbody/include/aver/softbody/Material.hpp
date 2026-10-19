// Soft-body material: the yield curve and the break limits of one constraint.
//
// Elastic below the yield strain, permanent set above it (the rest length creeps toward the stretched
// length), broken above `breakStrain`, after `maxBend` cm / `maxSetFrac` of accumulated set, or past
// `toughnessJ` of deposited energy. Units are the engine's: centimetres, Newtons, N per cm, Joules.
// The physics itself is Jolt's; this is what the damage step reads. See docs/SOFTBODY.md.
#pragma once

#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace aver::softbody {

// How a material fails. Deform bends and tears; Fracture and Shatter never dent, they snap at the
// yield strain (wood, glass).
enum class Behavior : u8 { Deform = 0, Fracture = 1, Shatter = 2 };

inline bool isBrittle(Behavior b) { return b != Behavior::Deform; }

struct Material {
    f32 stiffness        = 0.7f;      // legacy relaxation weight, kept so .ocbeam imports keep meaning
    f32 axialStiffness   = 3500.0f;   // N/cm: force = axial * stretch
    f32 bendForceN       = 3000.0f;   // yield force: permanent set begins here (when yieldStrain is 0)
    f32 breakForceN      = 12000.0f;  // instant snap
    f32 plasticStiffness = 800.0f;    // N/cm of creep resistance after yield
    f32 maxBend          = 8.0f;      // accumulated set (cm) before the constraint tears
    f32 bendAbsorb       = 0.3f;      // energy soaked per cm of set, 0..1
    f32 breakAbsorb      = 0.6f;      // energy soaked on snap; the rest recoils
    f32 hardening        = 0.0f;      // raises the yield with accumulated set (0 = perfectly plastic)
    f32 breakStrain      = 0.0f;      // tears past this stretch of the ORIGINAL length; 0 = off
    Behavior behavior    = Behavior::Deform;

    // Jolt-driven fields. Zero means "off" or "derive from the force model above".
    f32 yieldStrain      = 0.0f;      // 0 = bendForceN / (axialStiffness * rest0), the old force model
    f32 toughnessJ       = 0.0f;      // energy per beam before it breaks (brittle or > 0 only)
    f32 compliance       = 0.0f;      // Jolt edge compliance, m/N; 0 = inextensible
    f32 bendCompliance   = 0.0f;      // Jolt dihedral compliance
    f32 areaDensityKgM2  = 1.0f;
    f32 friction         = 0.3f;
    f32 restitution      = 0.0f;
    f32 maxSetFrac       = 0.0f;      // accumulated set as a fraction of rest0 before the beam tears; 0 = off
};

// Values mirror the C# SoftPreset enum.
enum class SoftPreset : u8 { ClothLight = 0, ClothHeavy = 1, Metal = 2, Wood = 3, Glass = 4 };
inline constexpr u32 kSoftPresetCount = 5;

struct PresetInfo {
    const char* key;
    Material material;
    u32 iterations;
    f32 damping;
    f32 capThicknessCm = 0.0f;   // the default thickness of a break's walls and back skin; 0 = flat (cloth)
};

namespace detail {
inline Material makePresetMaterial(f32 density, f32 edgeCompliance, f32 bendCompliance, Behavior b,
                                   f32 yieldStrain, f32 maxSetFrac, f32 breakStrain, f32 toughness, f32 friction) {
    Material m;
    m.areaDensityKgM2 = density;
    m.compliance = edgeCompliance;
    m.bendCompliance = bendCompliance;
    m.behavior = b;
    m.yieldStrain = yieldStrain;
    m.maxSetFrac = maxSetFrac;
    m.breakStrain = breakStrain;
    m.toughnessJ = toughness;
    m.friction = friction;
    // The force model is superseded by strains for presets: never yield or snap on force.
    m.bendForceN = 1e9f;
    m.breakForceN = 1e9f;
    m.maxBend = 1e9f;
    return m;
}
} // namespace detail

// The preset table (docs/SOFTBODY.md). Starting values, tuned headed.
inline const PresetInfo& presetInfo(SoftPreset p) {
    static const PresetInfo table[kSoftPresetCount] = {
        {"cloth_light", detail::makePresetMaterial(0.25f, 0.0f, 0.05f, Behavior::Deform, 0.0f, 0.0f, 0.40f, 0.0f, 0.4f), 6, 0.30f, 0.0f},
        {"cloth_heavy", detail::makePresetMaterial(0.8f, 0.0f, 0.01f, Behavior::Deform, 0.0f, 0.0f, 0.35f, 0.0f, 0.5f), 6, 0.50f, 0.0f},
        {"metal", detail::makePresetMaterial(7.8f, 2e-7f, 1e-6f, Behavior::Deform, 0.010f, 0.25f, 0.30f, 40.0f, 0.3f), 8, 0.50f, 0.5f},
        {"wood", detail::makePresetMaterial(6.0f, 1e-6f, 5e-6f, Behavior::Fracture, 0.020f, 0.0f, 0.020f, 3.0f, 0.5f), 8, 0.50f, 2.0f},
        {"glass", detail::makePresetMaterial(10.0f, 2e-7f, 1e-6f, Behavior::Shatter, 0.006f, 0.0f, 0.006f, 0.4f, 0.3f), 16, 0.20f, 0.6f},
    };
    const u32 i = static_cast<u32>(p);
    return table[i < kSoftPresetCount ? i : 0];
}

inline const Material& presetMaterial(SoftPreset p) { return presetInfo(p).material; }
inline const char* presetKey(SoftPreset p) { return presetInfo(p).key; }

// "cloth_light" -> ClothLight. False for an unknown key.
inline bool presetFromKey(std::string_view key, SoftPreset& out) {
    for (u32 i = 0; i < kSoftPresetCount; ++i) {
        const SoftPreset p = static_cast<SoftPreset>(i);
        if (key == presetInfo(p).key) { out = p; return true; }
    }
    return false;
}

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
