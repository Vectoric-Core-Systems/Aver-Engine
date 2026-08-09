// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/voxi_abi.h"
#include "aver/core/Log.hpp"

#include <algorithm>

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
        AVER_INFO("[Voxi] {} was requested but this device cannot run it ({}); it stays off",
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
    }
    if (status(Feature::PathTracing) != Status::Ready) {
        if (n.pathTracing != Quality::Off) refuse(Feature::PathTracing);
        n.pathTracing = Quality::Off;
    }
    if (status(Feature::MeshShaders) != Status::Ready) {
        if (n.meshShaders) refuse(Feature::MeshShaders);
        n.meshShaders = false;
    }

    // Quality tiers used to leave voxelResolution completely alone -- selecting Epic cost nothing
    // extra because nothing read the tier to size the grid. When the caller changes the GI tier and
    // leaves voxelResolution exactly as it already was -- the common case: the editor's Quality
    // combo touched alone, or a project manifest that states giQuality but not voxelResolution (see
    // applyProjectRenderSettings in SandboxApp.cpp) -- derive the grid edge from the new tier. An
    // explicit voxelResolution request arriving in the SAME call (the incoming value differs from
    // what is currently active) always wins over the derived one, so the editor's "Voxel grid"
    // combo and a manifest's explicit voxelResolution keep overriding it exactly as before.
    if (n.globalIllumination != settings_.globalIllumination && n.voxelResolution == settings_.voxelResolution)
        n.voxelResolution = voxelResolutionForQuality(n.globalIllumination);

    n.voxelResolution = std::clamp(n.voxelResolution, 32u, 512u);
    n.giIntensity     = std::clamp(n.giIntensity, 0.0f, 8.0f);
    n.giMaxDistance   = std::clamp(n.giMaxDistance, 1.0f, 100000.0f);
    // Mirrors VoxiRenderer::kMaxShadowRays / kMaxPixelsPerRayTile, restated rather than shared: this
    // library is core-only and must not depend on the RHI-backed renderer that owns those constants.
    // The renderer's own setters are the authority on the exact contract (kMaxPixelsPerRayTile also
    // rounds to a power of two); this is just enough to keep a wild request off the wire to it.
    n.rtShadowRays       = std::clamp(n.rtShadowRays, 1u, 32u);
    n.rtPixelsPerRayTile = std::clamp(n.rtPixelsPerRayTile, 1u, 16u);
    // Mirrors VoxiRenderer::kMaxGiUpdateInterval for the same reason as rtPixelsPerRayTile above.
    n.giUpdateInterval   = std::clamp(n.giUpdateInterval, 1u, 8u);

    if (n.msaa != settings_.msaa) msaaDirty_ = true;
    settings_ = n;
}

// Returns whether a feature is usable on this device.
Status Renderer::status(Feature f) const {
    switch (f) {
        case Feature::Msaa:
            return device_.maxMsaaSamples > 1 ? Status::Ready : Status::Unsupported;
        case Feature::GlobalIllumination:
            if (!device_.computeShaders) return Status::Unsupported;
            return Status::Ready;
        case Feature::RayTracing:
            if (device_.rayTracingTier < 11 || device_.shaderModel < 65 || !device_.dxcAvailable)
                return Status::Unsupported;
            return Status::Ready;
        case Feature::PathTracing:
            if (device_.rayTracingTier == 0) return Status::Unsupported;
            return Status::NotImplemented;
        case Feature::MeshShaders:
            if (device_.meshShaderTier == 0 || device_.shaderModel < 65 || !device_.dxcAvailable)
                return Status::Unsupported;
            return Status::Ready;
        default: return Status::Unsupported;
    }
}

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
        default: return "?";
    }
}

// The voxel grid edge each GI quality tier resolves to (see setSettings for when this actually
// applies). Doubling the edge is an 8x jump in both memory and per-voxel GPU cost -- the volume is
// resolution CUBED -- so this ladder is deliberately conservative: Medium keeps the long-standing
// fixed default (128) so any project already tuned around Medium sees no change; Off and Low share
// the cheapest grid, since Off's volume is otherwise idle VRAM (voxelizePass/filterMips are skipped
// whenever GI is disabled -- see VoxiRenderer::prePass -- so a smaller grid there costs nothing in
// frame time, only in bytes reserved). Approximate VRAM for the radiance volume (RGBA16F, full mip
// chain) plus the R32_UINT injection accumulator that sits beside it, at each rung:
//   Off / Low (64):   ~6 MB
//   Medium    (128):  ~50 MB   (today's fixed default, unchanged)
//   High      (256):  ~400 MB
//   Epic      (512):  ~3.2 GB  -- by far the steepest rung; only for a GPU with gigabytes to spare
u32 Renderer::voxelResolutionForQuality(Quality q) {
    switch (q) {
        case Quality::Off:
        case Quality::Low:    return 64;
        case Quality::Medium: return 128;
        case Quality::High:   return 256;
        case Quality::Epic:   return 512;
        default:              return 128;
    }
}

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
