// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/voxi_abi.h"
#include "aver/core/Log.hpp"
// QualityLadder.hpp: tier derivation logic. RenderSettingsResolver.hpp: resolve() step in setSettings.
#include "aver/voxi/QualityLadder.hpp"
#include "aver/voxi/RenderSettingsResolver.hpp"

#include <algorithm>
#include <cmath>

// The Voxi settings service and the C ABI the C# scripting layer P/Invokes.
namespace aver::voxi {

// Returns the process-wide instance.
Renderer& Renderer::get() {
    static Renderer inst;
    return inst;
}

// Records what the device can do and re-clamps the current settings against it.
void Renderer::setDeviceInfo(const DeviceInfo& info) {
    device_ = info;
    refusalLogged_ = 0;
    setSettings(settings_);
}

// Applies what is legal for this device; unsupported requests are clamped and logged once.
void Renderer::setSettings(const Settings& s) {
    Settings n = s;

    auto refuse = [&](Feature f) {
        const u32 bit = 1u << static_cast<u32>(f);
        if (refusalLogged_ & bit) return;
        refusalLogged_ |= bit;
        // Device unsupported vs engine NotImplemented: must distinguish in message to avoid misdirection.
        AVER_INFO(status(f) == Status::NotImplemented
                      ? "[Voxi] {} was requested but this engine does not implement it yet ({}); it stays off"
                      : "[Voxi] {} was requested but this device cannot run it ({}); it stays off",
                  featureName(f), statusText(f));
    };

    u32 samples = static_cast<u32>(n.msaa);
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) samples = 1;
    while (samples > 1 && !(device_.msaaMask & samples)) samples >>= 1;
    n.msaa = static_cast<Msaa>(samples);

    if (status(Feature::GlobalIllumination) != Status::Ready) {
        if (n.globalIllumination != Quality::Off) refuse(Feature::GlobalIllumination);
        n.globalIllumination = Quality::Off;
    }
    if (status(Feature::RayTracing) != Status::Ready) {
        if (n.rayTracing != Quality::Off) refuse(Feature::RayTracing);
        n.rayTracing = Quality::Off;
        // giMode stored unclamped, resolved at read time by RenderSettingsResolver.hpp (safe for shader flow).
    }
    if (status(Feature::PathTracing) != Status::Ready) {
        if (n.pathTracing != Quality::Off) refuse(Feature::PathTracing);
        n.pathTracing = Quality::Off;
    }
    if (status(Feature::MeshShaders) != Status::Ready) {
        if (n.meshShaders) refuse(Feature::MeshShaders);
        n.meshShaders = false;
    }

    // Unsupported features clamp to Off; downstream code needn't check validity.
    if (status(Feature::LayeredBsdf) != Status::Ready) {
        if (n.layeredBsdf != Quality::Off) refuse(Feature::LayeredBsdf);
        n.layeredBsdf = Quality::Off;
    }

    // Derive voxel resolution from tier if tier changed but voxelResolution was not explicitly set in this call.
    if (n.globalIllumination != settings_.globalIllumination && n.voxelResolution == settings_.voxelResolution)
        n.voxelResolution = voxelResolutionForQuality(n.globalIllumination);

    // Derive revoxelisation interval from tier (same rule).
    if (n.globalIllumination != settings_.globalIllumination && n.giUpdateInterval == settings_.giUpdateInterval)
        n.giUpdateInterval = giUpdateIntervalForQuality(n.globalIllumination);

    // Derive cone count from tier (same rule).
    if (n.globalIllumination != settings_.globalIllumination && n.giCones == settings_.giCones)
        n.giCones = giConesForQuality(n.globalIllumination);

    // Derive ReSTIR visibility setting from ray-tracing tier (same rule).
    if (n.globalIllumination != settings_.globalIllumination && n.giRestirVisibility == settings_.giRestirVisibility)
        n.giRestirVisibility = giRestirVisibilityForQuality(n.globalIllumination);

    // Refraction mode derives from ray-tracing tier, only if tier moved and field was not explicitly set.
    if (n.rayTracing != settings_.rayTracing && n.refractionMode == settings_.refractionMode)
        n.refractionMode = refractionForQuality(n.rayTracing);

    // RT sun-shadow knobs derive from ray-tracing tier.
    if (n.rayTracing != settings_.rayTracing && n.rtShadowRays == settings_.rtShadowRays)
        n.rtShadowRays = rtShadowRaysForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtPixelsPerRayTile == settings_.rtPixelsPerRayTile)
        n.rtPixelsPerRayTile = rtPixelsPerRayTileForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtShadowDenoise == settings_.rtShadowDenoise)
        n.rtShadowDenoise = rtShadowDenoiseForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtRenderMode == settings_.rtRenderMode)
        n.rtRenderMode = rtRenderModeForQuality(n.rayTracing);
    // giSkyOcclusionRays keyed on rayTracing (needs ray acceleration structure), not globalIllumination.
    if (n.rayTracing != settings_.rayTracing && n.giSkyOcclusionRays == settings_.giSkyOcclusionRays)
        n.giSkyOcclusionRays = giSkyOcclusionRaysForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.giSkyOcclusionTile == settings_.giSkyOcclusionTile)
        n.giSkyOcclusionTile = giSkyOcclusionTileForQuality(n.rayTracing);
    // ptBounces keyed on pathTracing, not rayTracing.
    if (n.pathTracing != settings_.pathTracing && n.ptBounces == settings_.ptBounces)
        n.ptBounces = ptBouncesForQuality(n.pathTracing);

    n.voxelResolution = std::clamp(n.voxelResolution, 32u, 512u);
    // At least the axial cone, or the gather returns nothing.
    n.giCones         = std::clamp(n.giCones, 1u, 16u);
    // 1 is per-pixel (no-op); 16 is well past usable range.
    n.giSkyOcclusionTile = std::clamp(n.giSkyOcclusionTile, 1u, 16u);
    n.giIntensity     = std::clamp(n.giIntensity, 0.0f, 8.0f);
    n.giMaxDistance   = std::clamp(n.giMaxDistance, 1.0f, 100000.0f);
    // Lower bound must be > 0 (sentinel; shader falls back to 16.0 when exactly 0).
    n.giRadianceCeiling = std::clamp(n.giRadianceCeiling, 0.1f, 256.0f);
    n.rtShadowRays       = std::clamp(n.rtShadowRays, 1u, 32u);
    // 3 is (2*3+1)^2 = 49-tap; kept low (runs per fragment, tap count multiplies by overdraw).
    n.rtShadowDenoise    = std::clamp(n.rtShadowDenoise, 0u, 3u);
    // Clamp to 1 if invalid; rtRenderMode=1 is ray-traced (0 is raster).
    n.rtRenderMode       = n.rtRenderMode > 1u ? 0u : n.rtRenderMode;
    // Valid modes: 0 (single pass), 1 (staged), 2 (staged + half-rate GI); clamp to default 2.
    n.rayDrivenStages    = n.rayDrivenStages > 2u ? 2u : n.rayDrivenStages;
    // Valid modes: 0 (cone gather), 1 (ReSTIR GI); clamp to default 0.
    n.giMode             = n.giMode > 1u ? 0u : n.giMode;
    // Valid values: 0-4; typo clamps to 3 (Full), not 0 (would reintroduce over-brightness).
    n.giRestirVisibility = n.giRestirVisibility > 4u ? 3u : n.giRestirVisibility;
    // 15 (AUTO) through 0 (temporal only) valid; packs to 4 bits.
    n.giRestirSpatialSamples = std::clamp(n.giRestirSpatialSamples, 0u, 15u);
    // Per-neighbour M cap; 31 is the 5-bit packed limit.
    n.giRestirMaxHistory     = std::clamp(n.giRestirMaxHistory, 0u, 31u);
    n.ptBounces          = std::clamp(n.ptBounces, 1u, 8u);
    n.ptMode             = n.ptMode > 1u ? 0u : n.ptMode;
    n.rtPixelsPerRayTile = std::clamp(n.rtPixelsPerRayTile, 1u, 16u);
    n.giUpdateInterval   = std::clamp(n.giUpdateInterval, 1u, 8u);
    // Zero history length would divide by zero in FidelityFX; 255 is defensive ceiling.
    n.denoiserMaxSamples        = std::clamp(n.denoiserMaxSamples, 1u, 255u);
    n.denoiserHistoryClipWeight = std::clamp(n.denoiserHistoryClipWeight, 0.01f, 4.0f);
    n.denoiserSunMovingSamples  = std::clamp(n.denoiserSunMovingSamples, 1u, 255u);
    n.denoiserKind = n.denoiserKind == 2u ? 2u : 1u;
    // The shader clamps too (nrd2SanitiseParams); a non-finite value falls back to the default.
    for (u32 i = 0; i < 12; ++i) {
        const float v = n.nrd2Params[i];
        n.nrd2Params[i] = std::isfinite(v) ? std::clamp(v, i % 6u < 3u ? -16.0f : -8.0f, i % 6u < 3u ? 16.0f : 8.0f)
                                           : Settings{}.nrd2Params[i];
    }

    // Clamp typos to ScreenSpace (1), not Off (0), to avoid silent behaviour change.
    n.refractionMode = n.refractionMode > 2u ? 1u : n.refractionMode;

    // Resolve feature interactions (see RenderSettingsResolver.hpp).
    const Resolution r = resolve(n, device_);
    n.rtRenderMode   = r.rtRenderMode.effective;
    n.refractionMode = r.refractionMode.effective;

    if (n.msaa != settings_.msaa) msaaDirty_ = true;
    settings_ = n;
}

// Returns whether a feature is usable on this device. See RenderSettingsResolver.hpp.
Status Renderer::status(Feature f) const { return featureStatus(f, device_); }

// Returns a readable reason for a feature's status.
const char* Renderer::statusText(Feature f) const {
    switch (status(f)) {
        case Status::Ready:          return "Ready";
        case Status::NotImplemented: return "Not implemented yet";
        case Status::Unsupported:
            switch (f) {
                case Feature::Msaa:               return "Device reports no MSAA";
                case Feature::GlobalIllumination: return "Device has no compute support";
                case Feature::MeshShaders:        return "Needs mesh-shader Tier 1 + SM 6.5 (D3D12 Ultimate)";
                default:                          return "Needs DXR 1.1 + SM 6.5 (D3D12 Ultimate)";
            }
        default: return "Unknown";
    }
}

// Returns a feature's display name.
const char* Renderer::featureName(Feature f) {
    switch (f) {
        case Feature::Msaa:               return "Anti-Aliasing (MSAA)";
        case Feature::GlobalIllumination: return "Global Illumination";
        case Feature::RayTracing:         return "Ray Tracing";
        case Feature::PathTracing:        return "Path Tracing";
        case Feature::MeshShaders:        return "Mesh Shaders";
        case Feature::LayeredBsdf:        return "Layered BSDF";
        default: return "?";
    }
}

// One-liners forwarding to QualityLadder.hpp (ladder:: functions contain reasoning).
u32 Renderer::refractionForQuality(Quality q) { return ladder::refraction(q); }
u32 Renderer::giConesForQuality(Quality q) { return ladder::giCones(q); }
u32 Renderer::giRestirVisibilityForQuality(Quality q) { return ladder::giRestirVisibility(q); }
u32 Renderer::voxelResolutionForQuality(Quality q) { return ladder::voxelResolution(q); }
u32 Renderer::giUpdateIntervalForQuality(Quality q) { return ladder::giUpdateInterval(q); }
u32 Renderer::giSkyOcclusionRaysForQuality(Quality q) { return ladder::giSkyOcclusionRays(q); }
u32 Renderer::giSkyOcclusionTileForQuality(Quality q) { return ladder::giSkyOcclusionTile(q); }
u32 Renderer::rtShadowRaysForQuality(Quality q) { return ladder::rtShadowRays(q); }
u32 Renderer::rtRenderModeForQuality(Quality q) { return ladder::rtRenderMode(q); }
u32 Renderer::ptBouncesForQuality(Quality q) { return ladder::ptBounces(q); }
u32 Renderer::rtShadowDenoiseForQuality(Quality q) { return ladder::rtShadowDenoise(q); }
u32 Renderer::rtPixelsPerRayTileForQuality(Quality q) { return ladder::rtPixelsPerRayTile(q); }

// Returns a quality level's display name.
const char* Renderer::qualityName(Quality q) {
    switch (q) {
        case Quality::Off:    return "Off";
        case Quality::Low:    return "Low";
        case Quality::Medium: return "Medium";
        case Quality::High:   return "High";
        case Quality::Epic:   return "Epic";
        default: return "?";
    }
}

// Returns true once after settings.msaa changes, then clears the flag.
bool Renderer::consumeMsaaDirty() {
    const bool d = msaaDirty_;
    msaaDirty_ = false;
    return d;
}

// Same one-shot shape as consumeMsaaDirty() above (see header for console command details).
bool Renderer::consumeGiHistoryResetRequest() {
    const bool d = giHistoryResetRequested_;
    giHistoryResetRequested_ = false;
    return d;
}
bool Renderer::consumeRtHistoryResetRequest() {
    const bool d = rtHistoryResetRequested_;
    rtHistoryResetRequested_ = false;
    return d;
}
bool Renderer::consumeAoHistoryResetRequest() {
    const bool d = aoHistoryResetRequested_;
    aoHistoryResetRequested_ = false;
    return d;
}
bool Renderer::consumeDenoiserHistoryResetRequest() {
    const bool d = denoiserHistoryResetRequested_;
    denoiserHistoryResetRequested_ = false;
    return d;
}

} // namespace aver::voxi

// ------------------------------------------------------------------ C ABI
using aver::voxi::Feature;
using aver::voxi::Quality;
using aver::voxi::Renderer;
using aver::voxi::Settings;

namespace {
// True when a feature id is in range.
bool validFeature(int32_t f) { return f >= 0 && f < AVER_VOXI_FEATURE_COUNT; }
// Returns the quality field a feature id names, or null.
Quality* qualitySlot(Settings& s, int32_t f) {
    switch (f) {
        case AVER_VOXI_FEATURE_GLOBAL_ILLUMINATION: return &s.globalIllumination;
        case AVER_VOXI_FEATURE_RAY_TRACING:         return &s.rayTracing;
        case AVER_VOXI_FEATURE_PATH_TRACING:        return &s.pathTracing;
        default: return nullptr;
    }
}
} // namespace

extern "C" {

// Returns the number of features.
int32_t aver_voxi_feature_count(void) { return AVER_VOXI_FEATURE_COUNT; }

// Returns a feature's display name.
const char* aver_voxi_feature_name(int32_t f) {
    return validFeature(f) ? Renderer::featureName(static_cast<Feature>(f)) : "?";
}
// Returns a feature's status id.
int32_t aver_voxi_feature_status(int32_t f) {
    return validFeature(f) ? static_cast<int32_t>(Renderer::get().status(static_cast<Feature>(f)))
                           : AVER_VOXI_STATUS_UNSUPPORTED;
}
// Returns a feature's status as readable text.
const char* aver_voxi_feature_status_text(int32_t f) {
    return validFeature(f) ? Renderer::get().statusText(static_cast<Feature>(f)) : "?";
}

// Returns the current MSAA sample count.
int32_t aver_voxi_get_msaa(void) { return static_cast<int32_t>(Renderer::get().settings().msaa); }

// Sets the MSAA sample count. Returns 0 if the request was rejected or clamped away.
int32_t aver_voxi_set_msaa(int32_t samples) {
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) return 0;
    Settings s = Renderer::get().settings();
    s.msaa = static_cast<aver::voxi::Msaa>(samples);
    Renderer::get().setSettings(s);
    return static_cast<int32_t>(Renderer::get().settings().msaa) == samples ? 1 : 0;
}

// Returns the supported sample counts as a bit mask.
int32_t aver_voxi_msaa_mask(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().msaaMask); }

// Returns a feature's quality level.
int32_t aver_voxi_get_quality(int32_t f) {
    Settings s = Renderer::get().settings();
    const Quality* q = qualitySlot(s, f);
    return q ? static_cast<int32_t>(*q) : AVER_VOXI_QUALITY_OFF;
}

// Sets a feature's quality level. Returns 0 if the request was rejected or clamped away.
int32_t aver_voxi_set_quality(int32_t f, int32_t quality) {
    if (quality < AVER_VOXI_QUALITY_OFF || quality > AVER_VOXI_QUALITY_EPIC) return 0;
    Settings s = Renderer::get().settings();
    Quality* slot = qualitySlot(s, f);
    if (!slot) return 0;
    *slot = static_cast<Quality>(quality);
    Renderer::get().setSettings(s);
    Settings after = Renderer::get().settings();
    const Quality* now = qualitySlot(after, f);
    return (now && static_cast<int32_t>(*now) == quality) ? 1 : 0;
}

// Returns the cubic voxel grid edge.
int32_t aver_voxi_get_voxel_resolution(void) { return static_cast<int32_t>(Renderer::get().settings().voxelResolution); }
// Sets the cubic voxel grid edge. Returns 0 if out of range.
int32_t aver_voxi_set_voxel_resolution(int32_t res) {
    if (res < 32 || res > 512) return 0;
    Settings s = Renderer::get().settings(); s.voxelResolution = static_cast<aver::u32>(res);
    Renderer::get().setSettings(s); return 1;
}
// Returns the indirect bounce multiplier.
float   aver_voxi_get_gi_intensity(void) { return Renderer::get().settings().giIntensity; }
// Sets the indirect bounce multiplier.
int32_t aver_voxi_set_gi_intensity(float v) {
    Settings s = Renderer::get().settings(); s.giIntensity = v; Renderer::get().setSettings(s); return 1;
}
// Returns the cone trace range in centimetres.
float   aver_voxi_get_gi_max_distance(void) { return Renderer::get().settings().giMaxDistance; }
// Sets the cone trace range, in centimetres.
int32_t aver_voxi_set_gi_max_distance(float cm) {
    Settings s = Renderer::get().settings(); s.giMaxDistance = cm; Renderer::get().setSettings(s); return 1;
}

// Returns how many frames apart the GI volume is re-voxelised (1 = every frame).
int32_t aver_voxi_get_gi_update_interval(void) { return static_cast<int32_t>(Renderer::get().settings().giUpdateInterval); }
// Sets the GI revoxelise interval, in frames. Returns 0 if out of range.
int32_t aver_voxi_set_gi_update_interval(int32_t frames) {
    if (frames < 1 || frames > 8) return 0;
    Settings s = Renderer::get().settings(); s.giUpdateInterval = static_cast<aver::u32>(frames);
    Renderer::get().setSettings(s); return 1;
}

// Returns the device's ray tracing tier.
int32_t aver_voxi_ray_tracing_tier(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().rayTracingTier); }
// Returns the device's highest supported MSAA sample count.
int32_t aver_voxi_max_msaa(void)         { return static_cast<int32_t>(Renderer::get().deviceInfo().maxMsaaSamples); }
// Returns the device's mesh shader tier.
int32_t aver_voxi_mesh_shader_tier(void) { return static_cast<int32_t>(Renderer::get().deviceInfo().meshShaderTier); }
// Returns the device's shader model.
int32_t aver_voxi_shader_model(void)     { return static_cast<int32_t>(Renderer::get().deviceInfo().shaderModel); }
// Returns 1 when the mesh shader submission path is on.
int32_t aver_voxi_get_mesh_shaders(void) { return Renderer::get().settings().meshShaders ? 1 : 0; }
// Turns the mesh shader submission path on or off. Returns 0 if the device refused it.
int32_t aver_voxi_set_mesh_shaders(int32_t on) {
    Settings s = Renderer::get().settings(); s.meshShaders = on != 0;
    Renderer::get().setSettings(s);
    return Renderer::get().settings().meshShaders == (on != 0) ? 1 : 0;
}

} // extern "C"
