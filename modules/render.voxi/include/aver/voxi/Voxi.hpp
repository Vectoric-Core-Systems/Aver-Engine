// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/core/Types.hpp"

// Voxi's render-feature settings: anti-aliasing, global illumination, ray tracing, path tracing.
// Core-only, no RHI dependency: the host pushes device capabilities in and reads settings back out.
namespace aver::voxi {

#if defined(_WIN32)
#  if defined(AVER_VOXI_BUILD)
#    define AVER_VOXI_API __declspec(dllexport)
#  else
#    define AVER_VOXI_API __declspec(dllimport)
#  endif
#else
#  define AVER_VOXI_API
#endif

// Multisample count.
enum class Msaa : u32 { Off = 1, X2 = 2, X4 = 4, X8 = 8 };

// Shared quality ladder for the trace-based features. Off means "don't run this pass".
enum class Quality : u32 { Off = 0, Low = 1, Medium = 2, High = 3, Epic = 4 };

// The features Voxi owns settings for.
enum class Feature : u32 { Msaa = 0, GlobalIllumination, RayTracing, PathTracing, MeshShaders, Count };

// Whether a feature is usable: implemented and supported, declared but not implemented yet, or
// refused by the device.
enum class Status : u32 {
    Ready = 0,
    NotImplemented,
    Unsupported,
};

// What the host's GPU can do. Pushed in by the runtime; mirrors rhi::DeviceCaps.
struct DeviceInfo {
    u32 msaaMask = 1;         // bit N set => N samples supported
    u32 maxMsaaSamples = 1;
    u32 rayTracingTier = 0;   // 0 none, 10 DXR 1.0, 11 DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;
    bool conservativeRaster = false;
    u32 shaderModel = 50;      // 60 = SM 6.0, 65 = SM 6.5
    u32 meshShaderTier = 0;    // 0 = none, 1 = Tier 1
    bool dxcAvailable = false; // DXIL compiler present
};

// The renderer quality settings, as requested. Clamped to the device by Renderer::setSettings.
struct Settings {
    Msaa    msaa               = Msaa::X4;
    Quality globalIllumination = Quality::Medium;
    // ON BY DEFAULT, AT MEDIUM, AND THE TWO KNOBS BELOW ARE MEDIUM'S RUNGS BY CONSTRUCTION -- see
    // rtShadowRays/rtPixelsPerRayTile for why that sentence is load-bearing rather than decorative.
    //
    // MEASURED COST. Release build, ElectricDreams, windowed at the editor's default 1600x900,
    // --no-vsync, --frames 200, whole-frame median: 11.86 ms with --no-rt, 18.44 ms at this tier's
    // rungs. +55%, and the cheapest honest way to have ray-traced sun shadows at all -- the naive
    // version of this change (flip the tier, leave the knobs at Epic's 4 rays) measures 23.39 ms,
    // almost exactly double the Off baseline. One machine, one GPU, one window size: the RELATIVE
    // ladder should hold anywhere, the absolute milliseconds are this card's (RX 7800 XT) and they
    // move with resolution, so quote the window when quoting the number.
    //
    // HOW TO MEASURE THIS WITHOUT MEASURING NOTHING, because that is the trap and it has now been
    // fallen into twice. The .ocproject path is POSITIONAL -- there is no --project flag, so
    // `Sandbox.exe --project <path>` silently opens no project at all. And even given the path
    // correctly, a project whose CREATEDWITH names an older series raises a modal and waits, so a
    // --frames run scores an EMPTY editor. Both failures look exactly like a successful benchmark:
    // plausible milliseconds, no error, a screenshot nobody opened. The tell is in the log --
    // "scene walk ... over 0 entities" means nothing loaded, and the honest baseline here is 14
    // entities and about 18 ms. Read that line before believing any number out of this ladder.
    //
    // A KNOWN WAY TO MAKE THIS LOOK BROKEN, recorded because it cost an afternoon to bisect: brighter
    // direct light on a surface means more INDIRECT light bounced off it, and the GI here does not
    // clamp what it gathers. Enough large, saturated, brightly-lit geometry and the bounce runs away
    // and floods the frame with that surface's colour -- three 1.8-metre pure-red spheres under a
    // 100,000-lux sun did exactly that, and turning RT on was merely what pushed it over, since it
    // lights those spheres more brightly than the voxel-cone path did. Scaling them down fixed it.
    // The scene was unreasonable; that the renderer answers it with a red screen rather than a clamp
    // is still the renderer's defect, and it is tracked. Nothing about it is hardware-specific.
    Quality rayTracing         = Quality::Medium;
    Quality pathTracing        = Quality::Off;
    bool    meshShaders        = false;

    // Cubic voxel grid edge; the volume's memory and per-voxel GPU cost are both O(this^3). Defaults
    // to Medium's rung (128) below. Renderer::setSettings derives this from globalIllumination
    // whenever the tier changes and this field arrives unchanged -- see voxelResolutionForQuality
    // and setSettings. Set it explicitly (a different value than what's currently active, in the
    // same call that changes the tier) to override the tier's rung.
    u32 voxelResolution = 128;
    f32 giIntensity     = 1.0f;
    f32 giMaxDistance   = 4000.0f;  // centimetres

    // ---- ray-traced sun shadow: rays per trace, and how many pixels amortise one trace ----
    // Occlusion rays per pixel, when this pixel traces this frame. Clamped to
    // [1, VoxiRenderer::kMaxShadowRays].
    //
    // DERIVED FROM rayTracing on a tier change, exactly as giUpdateInterval is derived from
    // globalIllumination: Low 1, Medium 1, High 2, Epic 4. THE DEFAULT IS 1 BECAUSE THE DEFAULT TIER
    // IS Medium -- the derivation only fires when the tier CHANGES, so a struct whose defaults
    // contradict its own tier never reaches the rung it claims. That is not hypothetical here: this
    // field defaulted to 4 while rayTracing defaulted to Off, so the moment RT was switched on by
    // default it would have run Epic's ray count under Medium's name.
    u32 rtShadowRays = 1;
    // Edge length of the square tile a single traced pixel is amortised over via the ray-traced
    // shadow's temporal history: 1 = every pixel traces every frame (bit-identical to no denoiser
    // at all); N>1 = one pixel in each NxN tile traces per frame, rotating which one so every pixel
    // gets its own turn every N*N frames, and every OTHER pixel reuses a reprojected history sample
    // instead of tracing. MUST be a power of two -- VoxiRenderer::setPixelsPerRayTile rounds to the
    // nearest one -- so the per-pixel schedule is a bitmask against the pixel coordinate rather than
    // a modulo, and the pixel COUNT one ray covers (N*N) is a clean power of two throughout: 1, 4,
    // 16, 64, 256 for tile edges 1, 2, 4, 8, 16. Clamped to [1, VoxiRenderer::kMaxPixelsPerRayTile].
    // NOTE: this governs the RT (DXR RayQuery) sun-shadow/reflection history and only has any effect
    // while rayTracing != Quality::Off; it does nothing to the voxel cone-trace GI cost below, which
    // is governed instead by giUpdateInterval.
    //
    // ALSO DERIVED FROM rayTracing on a tier change: Low 4, Medium 1, High 1, Epic 1. Defaulting to
    // 1 for the same by-construction reason rtShadowRays defaults to 1 -- Medium's rung, because
    // Medium is the default tier.
    //
    // MEDIUM IS 1, WHICH MEANS NO DENOISING BY DEFAULT. 1 is "every pixel traces every frame", which
    // the paragraph above calls bit-identical to no denoiser at all: no tiling, no reprojected
    // history, no temporal blend. What you see is what was traced this frame. It costs more than the
    // amortised rungs and it is the honest default for a renderer people are evaluating, because a
    // temporal denoiser hides its own artefacts as readily as the tracer's. Low still amortises, at
    // tile 4, for anyone who wants the frame back.
    //
    // Measured, so the ladder is not guesswork. Release build, ElectricDreams, windowed at 1600x900,
    // --no-vsync, --frames 200, whole-frame median: rays 1 / tile 4 = 18.26 ms, rays 1 / tile 2 =
    // 18.49 ms, rays 1 / tile 1 = 18.44 ms, rays 2 / tile 1 = 19.99 ms, rays 4 / tile 1 = 23.39 ms,
    // against 11.86 ms with --no-rt.
    //
    // NOTE THE SHAPE, because it is what makes tile 1 defensible as the default rather than merely
    // preferable: the three tile widths at one ray span 0.23 ms -- they are the same number inside
    // the run-to-run noise -- while going from one ray to four costs 4.95 ms. Amortisation saturates
    // immediately and the ray count is where all of the money is, so the temporal history was buying
    // no frame time in exchange for the latency it introduced. One scene at one resolution; a heavier
    // one may well disagree, which is what Low's tile 4 is still there for.
    u32 rtPixelsPerRayTile = 1;

    // How many frames apart the GI volume is re-voxelised: 1 (the default) revoxelises and re-filters
    // every frame, identical to the original always-fresh behaviour. N>1 reuses the previous frame's
    // voxelised+filtered volume for the N-1 frames in between, amortising the voxelise-rasterise pass
    // and the mip filter chain (VoxiRenderer::voxelizePass / filterMips) at the cost of the indirect
    // lighting lagging scene changes by up to N-1 frames -- a visible latency trade, not a resolution
    // one. Clamped to [1, VoxiRenderer::kMaxGiUpdateInterval].
    //
    // DERIVED FROM globalIllumination on a tier change, exactly as voxelResolution above is: Low 8,
    // Medium 1, High 1, Epic 1. Set it explicitly in the same call that changes the tier to override
    // the derived value.
    //
    // THE DEFAULT IS 1 BECAUSE THE DEFAULT TIER IS Medium, and the two have to agree by construction
    // -- the derivation only fires when the tier CHANGES, so a struct whose defaults contradict each
    // other never reaches the rung it claims. voxelResolution's 128 is Medium's rung for exactly this
    // reason. This field has now been wrong in BOTH directions for that same reason: it was 1 while
    // Medium derived to 4, and it would be 4 now that Medium derives to 1.
    //
    // MEDIUM MOVED 4 -> 1 BECAUSE 4 IS WHAT MAKES LIGHTING TRAIL THE CAMERA, and the frame time it was
    // buying is not there to buy. An earlier revision of this comment claimed interval 1 left 187.9 ms
    // on the table against 104.5 ms at interval 4. Re-measured on the same scene (Release,
    // ElectricDreams, 1600x900, --no-vsync, --frames 200): intervals 1, 2, 4 and 8 give medians of
    // 18.54, 18.47, 18.50 and 18.46 ms -- a 0.09 ms spread across the whole range, which is noise.
    // Whatever made revoxelisation the bottleneck when that pair of numbers was taken is no longer
    // true, and the figure outlived it; it is quoted here as refuted rather than quietly deleted.
    // Low still amortises at 8, which is where the trade belongs: not on the default tier.
    u32 giUpdateInterval = 1;

    // SPATIAL denoise radius for the ray-traced sun shadow, in pixels. 0 (the default) is off and
    // is exactly today's behaviour: the shadow term is whatever this pixel's own rays returned,
    // unfiltered. N > 0 averages a (2N+1)^2 neighbourhood of the shadow history, weighted by how
    // well each neighbour's stored depth agrees with this pixel's surface plane.
    //
    // WHY THIS EXISTS, AND WHY IT IS NOT THE TILE KNOB ABOVE. rtPixelsPerRayTile amortises over
    // TIME: a pixel reuses a reprojected value it computed frames ago. That converges beautifully
    // on a still camera and falls apart the moment one moves -- measured, at a penumbra probe: the
    // soft edge collapses to flat fully-shadowed under about one degree of yaw over forty frames.
    // This averages over SPACE instead, and keeps no history at all, so there is nothing to go
    // stale and camera motion cannot poison it. The two are independent and can be combined, but
    // they fail in completely different ways and should not be reasoned about as one setting.
    //
    // THE PROBLEM IT IS FOR. At one ray per pixel -- which is what Low and Medium both run -- the
    // shadow term is a hard 0 or 1, so a penumbra is not soft, it is dithered. Measured at a probe
    // whose converged answer is 34,36,40: one ray reads 61,59,59 and never improves, because the
    // ray is a pure function of the pixel and repeats forever. Sixteen rays reach 34,36,40 and cost
    // 30.55 ms against 18.66. Averaging the neighbours instead is the cheap way to the same place,
    // because rtShadow jitters the ray ORIGIN across the pixel footprint -- so neighbouring pixels
    // are already sampling different parts of the same receiver, and their mean is a real area
    // estimate rather than a blur.
    //
    // DERIVED FROM rayTracing on a tier change, like the two knobs above, and 0 for every tier
    // today so the default tier's rung and this default agree by construction. That agreement is
    // the whole contract: derivation only fires when the tier CHANGES, so a struct default that
    // contradicts its own tier never reaches the rung it claims -- a trap this file has already
    // fallen into in both directions with giUpdateInterval.
    u32 rtShadowDenoise = 0;

    // ---- ray-driven rendering (experimental) -----------------------------------------------
    // WHICH THING FINDS THE FIRST SURFACE: 0 = the rasteriser (every version of this engine so
    // far), 1 = a primary ray per pixel. Everything downstream of that first hit is unchanged --
    // PSMainVoxi already traces the sun shadow, evaluates the material and traces a reflection in
    // ONE pixel-shader invocation (VoxiShaders.hpp:781-826), so this is not "fusing passes", it is
    // swapping out the one stage that is still fixed-function.
    //
    // MEASURED BEFORE IT WAS BUILT, which is why the number to beat is written down here:
    // ElectricDreams at 4x MSAA, 2750x1639, Release -- raster primary visibility plus material
    // shading is 9.2 ms of `scene draw` with RT and GI off, and one additional shadow ray costs
    // 1.6 ms at the same resolution. A primary ray has to fit inside that difference to be worth
    // having. It also gives up hardware early-Z, which discards an occluded fragment before the
    // expensive shader ever runs and which a ray has no equivalent of -- you pay the traversal to
    // find out the hit was hidden.
    //
    // 0 FOR EVERY TIER, deliberately: this is opt-in while it is experimental, and the struct
    // default agrees with every rung by construction. See rtShadowDenoise above for why that
    // agreement is load-bearing rather than tidy.
    u32 rtRenderMode = 0;

    // ---- path tracing -----------------------------------------------------------------------
    // WHERE RAY TRACING ENDS AND PATH TRACING BEGINS, because this file already draws that line
    // and this setting was on the wrong side of it. RAY TRACING is discrete rays answering a
    // specific question -- is this point in shadow, what does this mirror see, what surface does
    // this pixel see -- and every one of those is one hit and direct lighting. PATH TRACING is the
    // multi-bounce light-transport solve. They are separate settings (rayTracing / pathTracing
    // above) because they are separately useful, separately priced and separately supported.
    //
    // This was `rtBounces`, derived from the rayTracing tier, which meant a project with
    // `pathTracing = Off` could be running a path tracer -- a setting reading "off" while the
    // thing it names is on. Bounces belong to pathTracing and are derived from it.
    //
    // 1 means NO extra bounces: one hit, direct lighting, which is ray tracing. Above 1 is path
    // tracing, and VoxiRenderer refuses to spend it while pathTracing is Off regardless of what
    // is stored here -- see ptBounceParams, which is where that is enforced rather than trusted.
    u32 ptBounces = 1;
};

// Process-wide settings service. Single instance shared by the editor, the runtime and the C ABI.
class AVER_VOXI_API Renderer {
public:
    // Returns the process-wide instance.
    static Renderer& get();

    // Records what the device can do and re-clamps the current settings against it.
    void setDeviceInfo(const DeviceInfo& info);
    const DeviceInfo& deviceInfo() const { return device_; }

    const Settings& settings() const { return settings_; }
    // Applies what is legal for this device; unsupported requests are clamped, not silently kept.
    void setSettings(const Settings& s);

    // Returns whether a feature is usable on this device.
    Status status(Feature f) const;
    // Returns a readable reason for a feature's status.
    const char* statusText(Feature f) const;
    bool available(Feature f) const { return status(f) == Status::Ready; }

    // Returns true once after settings.msaa changes, then clears the flag.
    bool consumeMsaaDirty();

    // Returns a feature's display name.
    static const char* featureName(Feature f);
    // Returns a quality level's display name.
    static const char* qualityName(Quality q);
    // Returns the voxel grid edge a GI quality tier resolves to when setSettings derives
    // voxelResolution from a tier change -- see setSettings and Voxi.cpp for the ladder and why it
    // only ever applies when the caller left voxelResolution untouched.
    static u32 voxelResolutionForQuality(Quality q);
    // Returns the revoxelisation interval a GI quality tier resolves to, derived by setSettings on a
    // tier change under exactly the same "only if the caller left it untouched" rule as the grid edge
    // above. Epic is 1 -- always fresh -- so the top tier's indirect light is unchanged by this.
    static u32 giUpdateIntervalForQuality(Quality q);
    // The RT sun-shadow rungs, mirroring giUpdateIntervalForQuality: applied by setSettings when the
    // rayTracing tier changes and the field arrives unchanged.
    static u32 rtShadowRaysForQuality(Quality q);
    static u32 rtPixelsPerRayTileForQuality(Quality q);
    static u32 rtShadowDenoiseForQuality(Quality q);
    // 0 for every tier -- ray-driven PRIMARY VISIBILITY is opt-in, not something a quality preset
    // turns on behind the author's back while it is still experimental.
    static u32 rtRenderModeForQuality(Quality q);
    // Derived from the PATH TRACING tier, not the ray-tracing one. See Settings::ptBounces.
    static u32 ptBouncesForQuality(Quality q);

private:
    Renderer() = default;
    Settings settings_{};
    DeviceInfo device_{};
    bool msaaDirty_ = true;
    u32 refusalLogged_ = 0;   // one bit per Feature: its refusal has already been logged
};

} // namespace aver::voxi
