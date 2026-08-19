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

    // The same derivation for HOW OFTEN the volume is rebuilt, by exactly the same rule and for the
    // same reason: the tier sized the grid but said nothing about the revoxelisation rate, so every
    // tier paid the full always-fresh cost. Revoxelising measured at 108 ms of a 229 ms frame -- 47%,
    // the single largest item in it -- and dropping to one rebuild in four took the frame from 121.1
    // to 104.5 ms on the Electric Dreams scene.
    //
    // EPIC STAYS AT 1, which is the point. Epic means "do not compromise", so its indirect light
    // remains bit-identical to the always-fresh behaviour every tier used to have; only the cheaper
    // tiers buy speed with latency. The trade is temporal, not spatial -- indirect light lags scene
    // changes by up to N-1 frames and a static scene converges to exactly the same image.
    if (n.globalIllumination != settings_.globalIllumination && n.giUpdateInterval == settings_.giUpdateInterval)
        n.giUpdateInterval = giUpdateIntervalForQuality(n.globalIllumination);

    // The RT sun-shadow knobs follow their own tier the same way, and for a sharper reason: with RT
    // on by default there is no longer any configuration in which these are inert, so a tier change
    // that left them alone would advertise Medium while running whatever the last tier paid for.
    // Same "only if the caller did not set it explicitly in this call" rule as GI above.
    if (n.rayTracing != settings_.rayTracing && n.rtShadowRays == settings_.rtShadowRays)
        n.rtShadowRays = rtShadowRaysForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtPixelsPerRayTile == settings_.rtPixelsPerRayTile)
        n.rtPixelsPerRayTile = rtPixelsPerRayTileForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtShadowDenoise == settings_.rtShadowDenoise)
        n.rtShadowDenoise = rtShadowDenoiseForQuality(n.rayTracing);
    if (n.rayTracing != settings_.rayTracing && n.rtRenderMode == settings_.rtRenderMode)
        n.rtRenderMode = rtRenderModeForQuality(n.rayTracing);
    // KEYED ON pathTracing, not rayTracing. A bounce budget is a path-tracing quantity; deriving
    // it from the ray-tracing tier is what let the two run out of step.
    if (n.pathTracing != settings_.pathTracing && n.ptBounces == settings_.ptBounces)
        n.ptBounces = ptBouncesForQuality(n.pathTracing);

    n.voxelResolution = std::clamp(n.voxelResolution, 32u, 512u);
    n.giIntensity     = std::clamp(n.giIntensity, 0.0f, 8.0f);
    n.giMaxDistance   = std::clamp(n.giMaxDistance, 1.0f, 100000.0f);
    // Mirrors VoxiRenderer::kMaxShadowRays / kMaxPixelsPerRayTile, restated rather than shared: this
    // library is core-only and must not depend on the RHI-backed renderer that owns those constants.
    // The renderer's own setters are the authority on the exact contract (kMaxPixelsPerRayTile also
    // rounds to a power of two); this is just enough to keep a wild request off the wire to it.
    n.rtShadowRays       = std::clamp(n.rtShadowRays, 1u, 32u);
    // 3 is a (2*3+1)^2 = 49-tap neighbourhood, which is already past the point where a wider
    // kernel buys anything a second iteration would not buy more cheaply. Kept low deliberately:
    // this runs per FRAGMENT inside the shading shader, so the tap count multiplies by overdraw.
    n.rtShadowDenoise    = std::clamp(n.rtShadowDenoise, 0u, 3u);
    // 1 is the only mode that exists besides raster; anything else is a manifest typo, and
    // clamping to 1 rather than 0 would turn a typo into a silent renderer swap.
    n.rtRenderMode       = n.rtRenderMode > 1u ? 0u : n.rtRenderMode;
    // 8 is arbitrary but finite: an unbounded bounce count in a shader loop is a hang, and the
    // useful range for a real-time path tracer is nowhere near it.
    n.ptBounces          = std::clamp(n.ptBounces, 1u, 8u);
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
            // MIRRORS aver::pt::PathTracer::init()'s own gate field for field (kRayTracingTier=11,
            // kShaderModel=65, caps.dxcAvailable, caps.computeShaders -- see PathTracer.cpp): this IS
            // the capability check for modules/render.pt's reference view (PtSceneView), the only
            // path tracer this engine has ever built. Used to read `return Status::NotImplemented;`
            // unconditionally here -- true the day this enum was declared, and left true long after
            // SandboxApp grew a real PtSceneView, so the Path Tracing settings-page combo that reads
            // this status stayed permanently grey and setSettings() below clamped whatever
            // Settings::pathTracing held back to Off, on every device, forever, regardless of
            // hardware -- a persisted, C#-scriptable setting with no relationship whatsoever to the
            // real path tracer. See SandboxApp.cpp's buildRenderingSettings(page==4), the only reader
            // of a Ready status here, for what actually reconciles this against PtSceneView now.
            if (device_.rayTracingTier < 11 || device_.shaderModel < 65 || !device_.dxcAvailable ||
                !device_.computeShaders)
                return Status::Unsupported;
            return Status::Ready;
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

// Returns how many frames apart a GI tier re-voxelises. See setSettings for why Epic is 1.
u32 Renderer::giUpdateIntervalForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // GI is not running; the value is inert either way
        case Quality::Low:    return 8;   // kMaxGiUpdateInterval, the cheapest the clamp allows
        // MEDIUM IS 1 -- ALWAYS FRESH -- BECAUSE 4 IS WHAT MAKES LIGHTING TRAIL THE CAMERA. At 4 the
        // volume is revoxelised every fourth frame and the three in between reuse it, so indirect
        // light and the shadowing that comes with it lag scene and camera movement by up to three
        // frames. Standing still it converges to exactly the same image, which is why this reads as a
        // subtle "shadows lagging behind when moving around" rather than as an obvious fault, and why
        // it survived being measured: a still-camera benchmark cannot see it at all.
        //
        // The trade it was buying is real but belongs a rung down: Low still amortises at 8. Medium is
        // the default, the default is what people judge the renderer by, and latency in the lighting
        // is a worse first impression than a few milliseconds.
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;   // always fresh -- bit-identical to the old behaviour
        default:              return 1;   // an unknown tier must not silently degrade lighting
    }
}

// The RT sun-shadow rungs. MEASURED, not chosen by feel: ElectricDreams, windowed, --no-vsync, 200
// frames, whole-frame median, against 11.73 ms with rayTracing Off --
//   Low    (1 ray,  tile 4) 18.07 ms
//   Medium (1 ray,  tile 2) 18.36 ms
//   High   (2 rays, tile 2) 19.84 ms
//   Epic   (4 rays, tile 1) 23.15 ms
// Ray-traced sun shadows are not cheap on this hardware at any rung; the ladder buys back what it
// can. Tile 4 over tile 2 saves only 0.29 ms, so Low differs from Medium mostly in noise rather than
// cost -- which is why Low takes the wider tile and the same single ray, rather than pretending a
// meaningful gap exists.
u32 Renderer::rtShadowRaysForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // RT is not running; the value is inert either way
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 2;
        case Quality::Epic:   return 4;
        default:              return 1;   // an unknown tier must not silently cost more
    }
}

// ZERO ON EVERY RUNG, ON PURPOSE, and this is not a placeholder that someone forgot to fill in.
//
// The spatial filter this selects is being landed in stages, and this stage is the SETTING ALONE:
// nothing reads the value yet, so the whole change is provably incapable of moving a pixel. The
// rung that turns it on arrives with the filter it selects, in the same commit, measured against
// the penumbra probe -- not before it, where it would be an untested default.
//
// It still has to exist NOW rather than later, because Settings::rtShadowDenoise defaults to 0 and
// the derivation only fires on a tier CHANGE. A field whose default disagrees with its default
// tier's rung never reaches that rung, and this file has already shipped that bug twice (giUpdate-
// Interval, in both directions). Declaring the mapping at 0 everywhere keeps the two in agreement
// by construction from the first commit, so the day a rung becomes non-zero is a one-line change
// with nothing else to remember.
// RAY-DRIVEN RENDERING IS OFF AT EVERY RUNG, and will stay that way until it is measured against
// the 9.2 ms raster baseline recorded in Settings::rtRenderMode's own comment. A quality preset
// that silently switched which thing finds the first surface would change every pixel of a
// project that only asked for prettier shadows.
//
// It exists NOW rather than later for the same reason rtShadowDenoiseForQuality does: the
// derivation only fires on a tier CHANGE, so the function and the struct default have to agree
// from the first commit or the field never reaches the rung it claims.
u32 Renderer::rtRenderModeForQuality(Quality) { return 0; }

// THE PATH-TRACING LADDER. Off is 1 -- one hit and direct lighting, which is ray tracing and not
// path tracing at all -- and every rung above it buys bounces. The struct default is 1 and the
// default pathTracing tier is Off, so the two agree by construction; derivation only fires on a
// tier CHANGE, and a default contradicting its own rung never reaches it.
//
// MEASURED before these were chosen, ElectricDreams at 2750x1639: 1 bounce 4.7ms, 2 bounces
// 5.9ms, 4 bounces 6.3ms. The ladder stops at 4 because bounces beyond the second cost 0.4ms
// and changed nothing measurable outdoors -- most paths escape to sky and terminate. A closed
// interior would price them differently, and this engine has none to test against yet.
u32 Renderer::ptBouncesForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // one hit, direct lighting: ray tracing, not path tracing
        case Quality::Low:    return 2;
        case Quality::Medium: return 2;
        case Quality::High:   return 3;
        case Quality::Epic:   return 4;
        default:              return 1;   // an unknown tier must not silently start bouncing
    }
}

u32 Renderer::rtShadowDenoiseForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // RT is not running; the filter has nothing to filter
        case Quality::Low:    return 0;
        case Quality::Medium: return 0;
        case Quality::High:   return 0;
        case Quality::Epic:   return 0;
        default:              return 0;
    }
}
// See rtShadowRaysForQuality for the measurements behind these.
u32 Renderer::rtPixelsPerRayTileForQuality(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;
        case Quality::Low:    return 4;   // one traced pixel per 4x4, the widest amortisation that pays
        // 1 FROM MEDIUM UPWARDS: every pixel traces every frame, which is bit-identical to no denoiser
        // at all -- no tiling, no reprojected history, no temporal blend. Anything above 1 reuses a
        // reprojected sample for most pixels, and reprojection is what makes a shadow appear to trail
        // the thing casting it while the camera moves. It is nearly free here anyway (18.19 ms at
        // tile 1 against 18.36 ms at tile 2, inside the noise), because amortisation saturates early
        // at one ray per pixel. Low is the only rung that still trades lag for frame time.
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 1;   // an unknown tier must not silently reintroduce the history
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
