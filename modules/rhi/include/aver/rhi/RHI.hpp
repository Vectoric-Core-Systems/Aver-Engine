// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Aver RHI â€” the single render-hardware abstraction every GPU consumer targets. Backends
// (D3D12/D3D11/Vulkan) implement these interfaces; a Null backend is always available as a fallback.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/Atmosphere.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aver::rhi {

// Which graphics API a device is built on.
enum class Backend { Null, D3D12, D3D11, Vulkan };
// Human-readable name for a backend.
const char* backendName(Backend b);

// Parses a backend name -- "d3d12", "d3d11", "vulkan", "null", case-insensitively. False when the
// name is not one of those, leaving `out` untouched, so a typo is a diagnosable error rather than a
// silent fallback to whatever happened to be first.
bool parseBackendName(const char* name, Backend& out);

// How to create a swapchain for a native window.
struct SwapchainDesc {
    void* windowHandle = nullptr; // HWND
    u32 width = 0;
    u32 height = 0;
    u32 bufferCount = 2;
};

// The presentable back buffers for one window.
class ISwapchain {
public:
    virtual ~ISwapchain() = default;
    virtual void present() = 0;
    virtual void resize(u32 width, u32 height) = 0;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
};

// Interleaved mesh vertex: position + normal (engine space, cm) + UV0. No tangent field; the
// material shading derives its tangent frame from ddx/ddy of world position and UV.
struct MeshVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
};

// The layout is a cross-file ABI: the D3D12 input layout, HLSL `struct MeshVtx` and the raw root
// SRV stride all name it independently and no compiler checks them.
static_assert(sizeof(MeshVertex) == 32, "MeshVertex is the HLSL MeshVtx / kMeshInputLayout ABI");
static_assert(offsetof(MeshVertex, px) == 0,
              "position must stay first: the DXR BLAS description points at the vertex buffer base");

// Line vertex: position + colour (unlit), for grids/gizmos/debug.
struct LineVertex {
    f32 px, py, pz;
    f32 r, g, b;
};

// How to create a device: backend preference order and development switches.
//
// The preference order is the actual selection mechanism: a backend never asked for cannot be
// exercised or developed against. Until now nothing wrote it -- backend choice was decided by
// which ones were compiled in (a `#if` in RHI.cpp). ARCHITECTURE.md's P7 ("selected at runtime
// from the compiled-in set") depends on callers populating this rather than taking the default,
// which was true of no build before now.
struct DeviceDesc {
    Backend preferred[4] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan, Backend::Null};
    u32 preferredCount = 4;
    bool enableDebug = false;
    bool useWarp = false;   // prefer the software rasteriser (D3D12: WARP) over any hardware adapter
};

// What the physical device can actually do. Queried once at init.
struct DeviceCaps {
    u32 msaaMask = 1;            // bit N set => N samples supported (bits 1,2,4,8)
    u32 maxMsaaSamples = 1;      // highest supported sample count (1 = no MSAA)
    u32 rayTracingTier = 0;      // 0 = none, 10 = DXR 1.0, 11 = DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;
    bool conservativeRaster = false;
    u32 shaderModel = 50;            // 51 = SM 5.1, 60 = SM 6.0, 65 = SM 6.5, ...
    u32 meshShaderTier = 0;          // 0 = none, 1 = Tier 1 (D3D12 Ultimate)
    bool dxcAvailable = false;       // DXIL compiler present (needed for SM 6.x)
    u32 resourceBindingTier = 0;     // 0 = unknown, 1/2/3 = D3D12_RESOURCE_BINDING_TIER_N

    // Whether a shader may index a large texture array by a computed value -- needed by the
    // ray-traced path (one fullscreen pass shades every material, unlike raster's per-draw tables).
    //
    // Separate from resourceBindingTier, not derived: raster stays "explicit descriptor tables, not
    // bindless" (RHIResources.hpp, floor FL 11_0/tier 1); this covers only ray tracing (DXR 1.1 +
    // SM 6.5, always tier 3). Deriving from resourceBindingTier would also be wrong on Vulkan (that
    // field is hardcoded 0 there).
    bool rtBindlessTextures = false;

    // Whether a shader may perform 64-bit atomics on a buffer -- needed by a lock-free hash map
    // (64-bit compare-exchange), which is what a world-space radiance cache is.
    //
    // Queried from the device, not derived from shaderModel: SM 6.6+ does not imply this (unlike
    // RTXGI SHaRC's auto-detect from DXC target alone) -- two independent D3D12 queries
    // (OPTIONS1::Int64ShaderOps, OPTIONS9 for typed-resource). Getting it wrong either corrupts the
    // map (assumed true, actually false) or wastes a 16 MiB spin-lock fallback buffer (assumed
    // false); hence asked, not inferred.
    bool shaderInt64Atomics = false;
};

// A development clamp on what a device REPORTS, so capability-gated fallback paths can be run on
// hardware that does not need them. Every field only ever reduces a capability.
struct CapsOverride {
    bool active = false;
    bool noRayTracing = false;
    bool noMeshShaders = false;
    bool noConservativeRaster = false;
    bool noTypedUavLoads = false;
    bool noDxc = false;            // suppresses DXC in the shader compiler too, not just the caps
    u32  maxShaderModel = 0;       // 0 = no ceiling; 51/60/65/66 pin the reported model
    u32  maxMsaaSamples = 0;       // 0 = no ceiling
    u32  maxResourceBindingTier = 0; // 0 = no clamp; 1 = report Tier 1
};

// Parses a comma-separated list: no-rt, no-ms, no-cons-raster, no-typed-uav, no-dxc,
// sm=<51|60|65|66>, msaa=<1|2|4|8>, tier1. Returns false and applies nothing on a bad token.
bool setCapsOverride(const char* commaSeparatedList);
// The active override.
const CapsOverride& capsOverride();

// Applied by each backend at the end of its own capability query. Monotonically reducing.
void clampCaps(DeviceCaps& caps);

// Simulated device loss, after this many presented frames. 0 (default) never fires.
//
// Not a CapsOverride token, though --force-caps is the obvious neighbour: that struct's fields
// only ever reduce a capability, but losing a device is an event, not a capability. Exists
// because real device loss (driver timeout/update, hardware fault) is rarely exercised honestly,
// so the recovery path would otherwise rot silently until the one day it happens for real.
void setSimulatedDeviceLoss(u32 afterPresentedFrames);
u32  simulatedDeviceLoss();

// D3D12 DRED (Device Removed Extended Data), forced on via --dred. Same shape of problem as
// setSimulatedDeviceLoss above: a pre-device flag rather than a DeviceDesc field, since D3D12
// must enable DRED on the debug interface BEFORE D3D12CreateDevice runs, earlier than DeviceDesc
// is populated/read. Header-only -- no matching definition in RHI.cpp; the D3D12 backend is the
// only reader.
inline bool g_dredEnabled = false;
inline void setDredEnabled(bool enabled) { g_dredEnabled = enabled; }
inline bool dredEnabled() { return g_dredEnabled; }
// --gpu-validation: D3D12 GPU-based validation (checks every descriptor and resource state a shader
// actually touches). Needs --debug-layer as well, and is far slower; for diagnosing device loss only.
inline bool g_gpuValidationEnabled = false;
inline void setGpuValidationEnabled(bool enabled) { g_gpuValidationEnabled = enabled; }
inline bool gpuValidationEnabled() { return g_gpuValidationEnabled; }

// Engine radiance units -> cd/m^2. LevelSky.hpp sets sky.sunIntensity = w.sunLux / (100000/3) so
// the default sunIntensity 3.0 agrees with OcWorldEnv's default 100000 lux; inverted, one engine
// irradiance unit is 100000/3 cd/m^2, and since the renderer treats irradiance and the pre-exposure
// scene-linear pixel value (radiance) as the same unit throughout, one engine radiance unit is too.
// Named once here because PostSettings' eye adaptation needs real cd/m^2 (Krawczyk et al.'s
// formulas are fit to measured luminance).
constexpr f32 kLuminanceToCdm2 = 100000.0f / 3.0f;

// Camera post-processing: exposure, bloom and eye adaptation.
struct PostSettings {
    // Linear multiplier on scene radiance, applied BEFORE the tonemap. With autoExposure on it
    // multiplies the adapted value instead, as exposure compensation (1 = as metered, 2 = one stop
    // brighter); exposureMin/Max clamp the adaptation before it is applied.
    f32 exposure = 1.0f;

    // Bloom. Zero intensity builds no pyramid and records no pass at all.
    f32 bloomIntensity = 0.06f;
    // Luminance above which a pixel contributes, and the width of the soft knee below it.
    //
    // 4, not 1, since the Unreal calibration (2026-09-28, see exposureKey): exposing for the shade
    // puts sunlit stone at 2-6 after exposure, so at 1 a sunlit floor bloomed into a white haze
    // across half the frame at the owner's Bloom 0.366, where UE's stays crisp with a slight glow.
    // At 4 the sun disc, lamp bulbs and specular glints still bloom; diffuse sunlight barely does.
    f32 bloomThreshold = 4.0f;
    f32 bloomKnee      = 0.5f;

    // Eye adaptation, from a luminance histogram of the frame.
    bool autoExposure   = true;
    // Clamps on the computed multiplier, not on scene luminance. 0.01 so a bright view (open sky,
    // sunlit stone) can still be brought down to the target. Not persisted (a tuned engine value).
    f32  exposureMin    = 0.01f;
    // A ceiling of 8 (copied from maxRadiance's radiance clamp below) is wrong for an exposure
    // MULTIPLIER: it lets adaptation scale the AVERAGE pixel onto the tonemap's white shoulder,
    // killing chroma. MEASURED on PTTest Sponza (--frames 244, viewport mean/chroma R-B): exposure
    // 1->19.38/3.32, 2->29.79/4.47 (peak), 3.2->38.66/4.34, 5->48.76/3.28, 8->61.61/1.40 (what
    // auto-exposure picked) -- chroma collapses by 8, blue overtakes green above 5. `--exposure 8`
    // reproduced the auto-exposed image within 0.02/channel, confirming the clamp was the culprit.
    //
    // Capping lower (tried 3) removed the washout but read "too dark": the scene needs the gain,
    // just can't survive the CURVE -- fixed at the source via tonemap mode 2 (acesLumaTonemap:
    // chroma 3.09 at exposure 8 vs mode 1's 1.41, same brightness). Ceiling went back to 8; range
    // stays wide on purpose since a tighter ceiling pins the exposure across light/shade. MEASURED
    // on NewSponza (exposureKey 0.18): adaptation wanted x73 (courtyard noon), x99 (arcade), x120
    // (dusk), x151 (night, lamps only) -- a 16 ceiling (2026-09-26) pinned all four. 256 = 8 stops
    // above 1, inside a real camera's range. Not persisted.
    f32  exposureMax    = 256.0f;
    // Adaptation speed, e-folds/s of log exposure, asymmetric like real eyes: brighter (exposure
    // falling) is fast; darker (exposure rising) is slower, so leaving shade for sun flares then
    // settles, but leaving sun for shade stays dim briefly.
    f32  exposureSpeed     = 3.0f;   // toward a brighter view (exposure falling)
    // 1.0 -> 0.5: the scotopic slowdown below (rods up to 4x, adaptationRealism 1) stacks on this
    // base speed, so it was lowered to keep the same real dark-adaptation time rather than compound.
    f32  exposureSpeedDark = 0.5f;   // toward a darker view (exposure rising), e-folds/s

    // Perceptual eye adaptation (Krawczyk, Myszkowski & Seidel 2005). console post.adaptationRealism
    // (not in panel). [0,1]: 0 = full adaptation (every view drives to exposureKey, today's default
    // behaviour exactly); 1 = perceptual model. Scales two effects:
    //
    //   Partial adaptation (sec. 4): keyEff = exposureKey * lerp(1, alpha(Y)/alpha(Yref),
    //   adaptationRealism), alpha(Y) = 1.03 - 2/(2 + log10(Y+1)), Yref = 100 cd/m^2 (keyEff ==
    //   exposureKey there, matching how exposureKey was tuned). alpha(0.01)=0.032, alpha(1)=0.161,
    //   alpha(100)=0.530, alpha(2000)=0.653.
    //
    //   Rod-slowed dark adaptation (sec. 4): darkening gets up to 4x slower in true darkness (see
    //   exposureSpeedDark's own comment).
    //
    // Implemented in post.hlsl's CSExposure (gPostEye.x); see that file for both formulas in full.
    f32  adaptationRealism = 1.0f;   // [0,1]

    // Scotopic night vision (Krawczyk et al. sec. 5, after Kim et al.), "Night Vision" in panel.
    // [0,1]: below ~1 cd/m^2 rods take over from cones, draining colour and blue-shifting (Purkinje
    // effect) as this rises toward 1; 0 leaves pixels as rendered. Runs per pixel on scene-linear
    // radiance before exposure (post.hlsl PSComposite), so it applies regardless of auto-exposure.
    f32  nightVision = 1.0f;   // [0,1]

    // Centre-weighted metering (console post.meteringCenterWeight). [0,1]: 0 meters the region
    // equally (today's default); higher weights the centre up to 4x an edge pixel, like a camera's
    // centre-weighted meter, so a bright sky or floor at the region's edge can't dominate the
    // reading. Implemented in post.hlsl's CSHistogram (gPostEye.w).
    f32  meteringCenterWeight = 0.5f;   // [0,1]

    // Target brightness -- the log-average of the histogram's middle band (see
    // histogramLow/HighPercent) that adaptation drives every view toward, before panel Brightness
    // (stops) on top. Not persisted or in panel; console post.exposureKey for a session.
    //
    // 0.25, CALIBRATED AGAINST UNREAL 5 + LUMEN (2026-09-28, owner: "calibrate ... to look like the
    // lighting in" a UE5 Sponza video) together with the metering band, tonemap and local exposure
    // below. Display-space statistics (luma percentiles 5/25/50/75/95, mean HSV saturation) of the
    // video's daylight shots vs headless captures of NewSponza at matching poses:
    //   balcony  UE [30,72,109,150,204] .27   ours [42,67,90,130,250] .25
    //   arch     UE [ 3,17, 42, 95,228] .33   ours [20,29,50,115,225] .42 (red curtains fill ours)
    //   gallery  UE [25,46, 63, 93,190] .36   ours [34,60,77,104,157] .41
    // (tuned before that at 0.20 for "everything should be dimmer", no clipping anywhere; UE exposes
    // for the shade and lets sunlit stone clip, which is the look asked for now). Night level:
    // median 53 -> 56 balcony, 60 -> 64 gallery, colour unchanged.
    //
    // Then HALVED to 0.125, one stop down (owner, same day, looking at it in the editor: "make the
    // current eye exposure -1.0 the default so it would be +0.0 because default currently is too
    // bright"). The figures above are at 0.25. SandboxSettings' post.settingsVersion 3 doubles a
    // stored Brightness once, so an editor already at -1.0 keeps its picture and reads +0.0.
    f32  exposureKey    = 0.125f;
    // Fraction of the histogram discarded at each end before averaging.
    //
    // 0.10 / 0.90, Unreal's own auto-exposure defaults. The old 0.30 / 0.95 kept a sunlit floor in
    // the average, so it set the exposure and the shaded 90% of the view sat dim around it; dropping
    // the brightest tenth exposes for the shade, and sunlit stone clips the way it does in UE. A sun
    // disc, lamp bulb, glint or firefly still can't darken the whole view. Not persisted.
    f32  histogramLowPercent  = 0.10f;
    f32  histogramHighPercent = 0.90f;

    // Which tone curve. 0 = original per-channel Narkowicz/Hill approximation; 1 = the same curve
    // between the ACES input/output matrices (colour.hlsli's acesFittedTonemap); 2 = acesLumaTonemap
    // (tonemaps luminance, restores original chromaticity).
    //
    // 1 is the default since the Unreal calibration (2026-09-28, see exposureKey): UE's filmic curve
    // runs per channel in the ACES AP1 space, which is what mode 1's matrices do, and it matched
    // UE's saturation (balcony .285 vs UE .27; mode 0 .304) with a firmer toe. Mode 0 (the owner's
    // 2026-09-24 choice, gentlest toe so dim indirect light stays visible) is post.tonemap 0.
    // Mode 2's Hill RRT/ODT fit (x2 gain) has a hard black point at ~0.0016 and crushes anything
    // below ~0.02 by ~0.2, reading physically-correct bounce light (NewSponza stone albedo ~0.1-0.2)
    // as "no GI".
    //
    // Mode 1 still greys out at big exposure: MEASURED chroma (mean R-B) vs exposure, 1x->3.32,
    // 2x->4.47, 5x->3.28, 8x->1.40 ("colours are washed out"); mode 2 at 8x holds 3.09 at the same
    // brightness, and above 5x mode 1 also lets blue overtake green.
    //
    // The recorded gate baselines in scripts/ were measured through mode 0; they need re-recording.
    u32  tonemap = 1;

    // Ceiling on scene radiance immediately before the tonemap; 0 disables it.
    //
    // Nothing else here bounds radiance: the sun disc (sunColour * sunIntensity * 14, scene.hlsl
    // PSky) reaches ~42x at default intensity 3 with no clamp. The tonemap flattens it to white
    // regardless, but the unclamped value pulls the auto-exposure histogram (and thus the whole
    // frame) darker.
    //
    // 8 is derived: acesTonemap(8) = 1.003 (saturates to pure white), so clamping there changes no
    // pixel's tonemapped colour -- only bloom, which thresholds the unclamped value and so bleeds an
    // unbounded halo off a ~42x disc. Clamping bounds the halo while the disc stays blazing white.
    //
    // Raise it for more bloom from very bright sources; 0 restores the unclamped behaviour exactly.
    f32  maxRadiance = 8.0f;

    // Local exposure (as in Unreal). Auto-exposure above picks one multiplier for the whole frame,
    // so a sunlit arcade's stone sets it and the shaded interior sits 4-5 stops down, near-black.
    // This brightens dark regions and tames bright ones toward exposureKey (middle grey), via an
    // edge-aware bilateral grid of log-luminance to avoid haloing at region edges.
    //
    // [0,1]: fraction of a region's deviation from middle grey removed -- 0 off, 1 fully flattens
    // local contrast. shadows applies below middle grey, highlights above; on when either is > 0.
    // Both ride PostCB.clampRadiance[1]/[2] (gPostClamp.y/z, post.hlsl) instead of new cbuffer rows
    // (see that struct's comment on where a new post scalar lands first).
    //
    // OFF by default since the Unreal calibration (2026-09-28, see exposureKey), matching UE, whose
    // local exposure is also off unless asked for: its images have deep shade under a sunlit
    // courtyard, which shadows lifting works against (balcony darkest 5% 57 -> 52 turning both
    // off, UE 30), and a sunlit wall is allowed to clip. Neither is in the panel or persisted;
    // console post.localExposureShadows / post.localExposureHighlights turn them back on (the
    // previous tuning was 0.10 / 0.5, for a no-clipping look).
    f32  localExposureShadows    = 0.0f;
    f32  localExposureHighlights = 0.0f;
};

// Field by field, not memcmp: the bool leaves padding whose bytes a copy need not preserve. The size
// check is the reminder -- a new PostSettings field changes it, and must be added here too.
inline bool postSettingsEqual(const PostSettings& a, const PostSettings& b) {
    static_assert(sizeof(PostSettings) == 76, "a PostSettings field was added: compare it below too");
    return a.exposure == b.exposure && a.bloomIntensity == b.bloomIntensity &&
           a.bloomThreshold == b.bloomThreshold && a.bloomKnee == b.bloomKnee &&
           a.autoExposure == b.autoExposure && a.exposureMin == b.exposureMin &&
           a.exposureMax == b.exposureMax && a.exposureSpeed == b.exposureSpeed &&
           a.exposureSpeedDark == b.exposureSpeedDark &&
           a.adaptationRealism == b.adaptationRealism && a.nightVision == b.nightVision &&
           a.meteringCenterWeight == b.meteringCenterWeight &&
           a.exposureKey == b.exposureKey && a.histogramLowPercent == b.histogramLowPercent &&
           a.histogramHighPercent == b.histogramHighPercent && a.tonemap == b.tonemap &&
           a.maxRadiance == b.maxRadiance &&
           a.localExposureShadows == b.localExposureShadows &&
           a.localExposureHighlights == b.localExposureHighlights;
}

// Which sky the engine draws. Authored is a two-colour dome; Physical derives the dome, the direct
// sun's colour and the aerial perspective from Rayleigh/Mie/ozone scattering.
enum class SkyModel : u32 { Authored = 0, Physical = 1 };

// The sky, the sun and the air between them, as a level authors them.
struct SkyAtmosphere {
    bool enabled = false;

    // ---- which model ----
    // Physical is the DEFAULT. The dome, its exponent and the sun's colour below are then derived
    // from the sun's elevation and are inert as authored values; --sky-authored restores them.
    SkyModel          model = SkyModel::Physical;
    AtmosphereProfile air{};   // only read when model is Physical

    // ---- the dome ----
    f32 zenith[3]  = {0.24f, 0.45f, 0.85f};   // authored sRGB, decoded in the shader; Authored only
    f32 horizon[3] = {0.72f, 0.83f, 0.95f};
    f32 atmosphereHeight = 0.65f;   // exponent on the horizon-to-zenith blend
    // What the world below the horizon reflects back into the lower half of the dome.
    f32 groundAlbedo[3] = {0.24f, 0.23f, 0.21f};
    f32 groundBlend     = 1.0f;   // how much of the ground replaces the sky below the horizon
    // Multiplier on the sky-hemisphere irradiance every surface receives; 1.0 is "as bright as the
    // sky actually is".
    f32 skyLightIntensity = 1.0f;

    // ---- the sun ----
    // The authoritative field, pointing TOWARD the light. Need not be normalised. Degrees are the
    // editing form only; setSunAngles / sunAngles convert.
    f32 sunDirection[3] = {-0.5481f, 0.3838f, 0.7431f};
    // Light arriving at the top of the atmosphere, before any air. White is the physical default:
    // the physical sky (atmoFitDome / dome.sunTransmittance, packAtmosphere) already tints toward
    // orange via Rayleigh/Mie/ozone as elevation drops, so a warm bias here would double-count it.
    f32 sunColor[3]     = {1.0f, 1.0f, 1.0f};
    f32 sunIntensity    = 3.0f;   // scales direct light, GI injection and the sun disk alike
    f32 sunTemperatureK = 0.0f;   // Kelvin; 0 means use sunColor as authored
    f32 sunAngularDiameterDeg = 0.545f;   // disk size, and how fast a shadow edge softens

    // White furnace radiance. 0 = off (the only value content should use). Non-zero replaces sky,
    // ground and sun with a uniform environment of this radiance -- the standard energy-conservation
    // test: an albedo-1 surface inside it must read exactly this value, any orientation. Lives on the
    // sky (not a debug flag) since it IS a statement about the environment, and every radiance query
    // gets it for free.
    f32 furnaceRadiance = 0.0f;

    // Keeps the sun on inside the furnace, with the uniform environment at zero.
    //
    // A separate mode because plain furnace (sun off) only exercises the ambient term, and was
    // blind to a missing /PI that made every sunlit reflection 3.14x too bright.
    //
    // Contract: an albedo-1 Lambertian surface at angle theta to the sun must read exactly
    // E*cos(theta)/PI (E = sun irradiance) -- an absolute claim, needed because a missing 1/PI is a
    // global scale no ratio or equality between configurations can reveal; only comparison against a
    // known number can.
    bool furnaceSun = false;

    // ---- the air ----
    f32 fogColor[3] = {1.0f, 1.0f, 1.0f};   // a TINT on the in-scattered sky, not a replacement
    f32 fogDensity = 4e-6f;     // extinction per world unit (cm), at fogHeight
    f32 fogHeight  = 0.0f;      // world Z at which density is exactly fogDensity
    f32 fogFalloff = 0.0f;      // how fast it thins going up; 0 is uniform fog
    f32 fogStart   = 0.0f;      // distance in front of the camera before any fog accumulates
    f32 fogMaxOpacity = 1.0f;   // so distance never fully erases the world

    // ---- clouds ----
    bool cloudsEnabled = false;
    f32  cloudCoverage = 0.45f;   // 0 clear, 1 overcast
    f32  cloudDensity  = 1.0f;
    f32  cloudBottom   = 150000.0f;   // world Z of the layer's base and top
    f32  cloudTop      = 280000.0f;
    f32  cloudScale    = 0.00002f;    // 1 / the width of one noise feature, in world units
    f32  cloudWind[2]  = {900.0f, 260.0f};   // world units per second
    f32  cloudTime     = 0.0f;               // accumulated seconds; the app owns the clock
    // Which sky this is: two levels with different seeds get different cloud fields from the same
    // settings (lets a PCGVOLUME's seed reach the sky). 0 is the unseeded field and reproduces
    // previous output exactly -- deliberate, so this lands without moving any recorded gate probe,
    // and so anyone can bisect a sky change without wondering whether the seed did it.
    i32  cloudSeed     = 0;

    // Writes sunDirection from an elevation above the horizon and an azimuth bearing about +Z
    // from +X, both in degrees.
    void setSunAngles(f32 elevationDeg, f32 azimuthDeg);
    // Reads sunDirection back as elevation and azimuth in degrees.
    void sunAngles(f32& elevationDeg, f32& azimuthDeg) const;
};

// One node of a per-pass GPU timing report -- the public mirror of D3D12Device's private GpuAccum
// tree (see its own comment for why it's keyed by (label, parent), not a flat list). Flat and
// parent-indexed here since the source data is already this shape; a caller wanting indentation
// walks it with the same O(n) children-list pass collectGpuTiming builds.
//
// `ms` is INCLUSIVE (itself + everything nested); a caller wanting the pass's own time subtracts
// its direct children's `ms`, as collectGpuTiming's Appender does for "(excl ...)".
struct GpuTimingNode {
    // Sentinel for "top-level", matching D3D12Device's private kNoAccumParent so copying GpuAccum
    // needs no remapping.
    static constexpr u32 kNoParent = 0xFFFFFFFFu;
    std::string label;
    f64 ms = 0;              // inclusive, averaged across framesAccumulated frames
    u32 parent = kNoParent;  // index into the SAME report's `nodes`, or kNoParent
};

// A snapshot of one device's per-pass GPU timing, as of the last frame it collected one.
//
// Two independent "no data" axes, not collapsed into one:
//   - `supported` is the CAPABILITY axis: false means this backend cannot report timings at all
//     (Vulkan: no machinery yet; D3D12 with timing disabled). `nodes` is then always empty.
//   - `nodes` empty (or `framesAccumulated` 0) with `supported` true is the CONTENT axis: enabled
//     but nothing accumulated yet (frame 0, or every span this frame dropped -- see tsDropped_).
//     Lets a caller tell "ask again later" from "will never answer", which one bool couldn't.
struct GpuTimingReport {
    bool supported = false;
    // Averaged over this many frames since boot, or since the last resetGpuTiming() (see
    // D3D12Device::tsAccumFrames_ for why an average, not one sample). 0 when nothing collected yet.
    u32 framesAccumulated = 0;
    std::vector<GpuTimingNode> nodes;
};

// A snapshot of the adapter's video memory budget and usage, split as both backends' own APIs
// split it: LOCAL is memory on the GPU's own bus (VRAM discrete, whole pool on UMA); NON_LOCAL is
// everything else spillable (system memory over PCIe on discrete, unused on UMA). "Budget" is the
// OS's current per-process ceiling, not the physical total -- it moves as other processes/the
// compositor claim their share, hence polled rather than read once.
//
// `supported`: same two-state shape as GpuTimingReport::supported -- false means the backend could
// not answer this call (no IDXGIAdapter3, Vulkan extension absent, or the query failed), and every
// numeric field is then 0 rather than stale/guessed; check before trusting zero as "no memory used".
struct VideoMemoryInfo {
    bool supported = false;
    u64  localBudgetBytes = 0;      // D3D12 DXGI_MEMORY_SEGMENT_GROUP_LOCAL Budget / Vulkan: sum of heapBudget over DEVICE_LOCAL memory heaps
    u64  localUsageBytes = 0;       // ...CurrentUsage / sum of heapUsage over DEVICE_LOCAL memory heaps
    u64  nonLocalBudgetBytes = 0;   // ...NON_LOCAL Budget / sum over every other heap
    u64  nonLocalUsageBytes = 0;    // ...NON_LOCAL CurrentUsage / sum over every other heap
};

// One GPU device: frame loop, scene state, immediate drawing, capture and in-window UI.
class IDevice {
public:
    virtual ~IDevice() = default;
    virtual Backend backend() const = 0;
    virtual const char* adapterName() const = 0;
    virtual DeviceCaps caps() const { return {}; }

    // Generic resource creation for render-feature modules. nullptr on backends without GPU
    // support, which is how a feature declines to initialise.
    virtual IResourceFactory* resources() { return nullptr; }

    // The SAME context drawMesh() uses internally so a registered IRenderFeature can override
    // scene draws (D3D12Device::drawMesh's overridesScenePipeline). Exposed here so a CALLER too
    // can interleave setPipeline/dispatchMeshClusters calls with ordinary drawMesh() calls, for
    // the SUBSET of instances wanting a different draw path (per-cluster GPU LOD is the first
    // consumer; most instances still go through drawMesh()).
    // setPipeline() invalidates cached root-signature/PSO state as a side effect, keeping drawMesh()
    // safe right after -- same contract IRenderFeature's override relies on. nullptr with no GPU
    // support, like resources().
    virtual IRenderContext* renderContext() { return nullptr; }

    // Render-feature registration. NON-owning: the caller keeps the feature alive.
    virtual void addRenderFeature(IRenderFeature* f) { (void)f; }
    virtual void removeRenderFeature(IRenderFeature* f) { (void)f; }

    // The upscaler turning scene-resolution colour into the present-resolution image, or null for
    // none (default; must stay bit-identical to a build with no upscaler module -- docs/AVERSR.md's
    // invariant for quality Off).
    //
    // Non-owning like addRenderFeature: caller keeps it alive, composition root is the only place
    // that knows the concrete type -- lets Aver.Render.Sr be linked by the HOST alone (docs/AVERSR.md).
    //
    // Defaulted no-op so every IDevice implementation compiles unchanged: Vulkan implements this
    // same interface and is mid-bring-up, and a pure virtual here would break its build for a
    // feature it does not yet have.
    virtual void setUpscaler(IUpscaler* u) { (void)u; }
    virtual IUpscaler* upscaler() const { return nullptr; }

    // The SCENE colour target's format, which a backend running a post chain does not present
    // directly. Pipelines drawing into the scene must match it.
    virtual Format backbufferFormat() const { return Format::Unknown; }
    virtual Format depthFormat() const { return Format::Unknown; }

    // Anti-aliasing sample count. Changing it rebuilds the scene targets and every PSO. setter
    // returns false if the count is unsupported.
    virtual u32 sampleCount() const { return 1; }
    virtual bool setSampleCount(u32 samples) { (void)samples; return false; }
    // Creates the swapchain for a native window.
    virtual ISwapchain* createSwapchain(const SwapchainDesc& desc) = 0;
    virtual void beginFrame() = 0; // acquires + clears the current backbuffer
    virtual void endFrame() = 0;   // finalizes the frame's command list

    // Frame clear colour (linear RGBA, 0..1).
    virtual void setClearColor(f32 r, f32 g, f32 b, f32 a) { (void)r; (void)g; (void)b; (void)a; }

    // ---- vertical sync ----
    // Tearing has to be enabled when the swapchain is created, so a backend that cannot tear
    // reports vsyncCanDisable() false and setVSync(false) is a no-op.
    virtual void setVSync(bool on) { (void)on; }
    virtual bool vsync() const { return true; }
    virtual bool vsyncCanDisable() const { return false; }

    // Confines scene rendering to a sub-rectangle of the backbuffer, in physical pixels with a
    // top-left origin. (0,0,0,0) = full backbuffer.
    virtual void setViewportRect(u32 x, u32 y, u32 w, u32 h) { (void)x; (void)y; (void)w; (void)h; }

    // The aspect ratio scene rendering is confined to: the sub-rect above if set, else the whole
    // scene target. A RATIO not the rect: it's what a camera-deriving consumer needs, and it's
    // invariant under setViewportRect's present-to-scene conversion (no caller needs renderScale).
    // 0 = "not known yet", read as "make no correction".
    //
    // Exists because a wrong-aspect camera is invisible until measured: PtSceneView built rays for
    // a fixed 16:9 accumulator while the editor docks at any ratio, and the straight NDC blit
    // stretched the traced image by dstAspect/srcAspect -- 1.06x on the dockspace it was found on.
    virtual f32 viewportAspect() const { return 0.0f; }

    // Decouples the scene's render targets from the swapchain's: scene renders at
    // round(present * scale), post-chain composite upscales back to present size (editor UI and
    // backbuffer/viewport texture never see this value). Clamped [0.25, 1.0]; 1.0 default reproduces
    // 1:1 sizing exactly, permanently for a backend that never implements this.
    //
    // Deferred to the next beginFrame() when a swapchain exists (optimisation-wave-2, C2-13):
    // D3D12Device/VulkanDevice PARK the value and apply via their own applyPendingRenderScale()
    // (first statement of beginFrame) -- an immediate mid-frame rebuild loses the device (see
    // either implementation's own comment; aver-render-scale-device-loss).
    virtual void setRenderScale(f32 scale) { (void)scale; }
    virtual f32  renderScale() const { return 1.0f; }

    // Sends the post chain's output to an offscreen texture instead of the backbuffer, so the UI
    // can draw the scene as an ordinary image. The texture is the FULL backbuffer size.
    virtual void setViewportToTexture(bool on) { (void)on; }
    virtual bool viewportToTexture() const { return false; }
    // The UI identifier for that texture, or 0 when the mode is off or unsupported.
    virtual u64 viewportTextureId() { return 0; }

    // True once this device has been REMOVED and can no longer execute anything.
    //
    // Needed because a GPU can vanish underneath a running process (driver timeout/update, hardware
    // fault) without the API reporting it at the call that caused it -- every later call just fails
    // quietly, and the process carries on issuing work into a device that will never run any of it
    // until something finally faults hard, which is the shape of "the engine crashed with no
    // message" unless the layer that detects removal can tell the layer driving the frame.
    //
    // One-way and sticky: nothing here recovers a lost device (that means recreating every resource
    // every module owns); this is the honest minimum -- stop, say so, keep the last good frame.
    //
    // Defaults to false so a backend that can't lose its device, and every test mock, is unaffected.
    virtual bool deviceLost() const { return false; }

    // True when some registered feature has taken the scene over -- the path-traced reference view
    // is the one that does today. Lets a CALLER skip a draw entirely, a different question from
    // what drawMesh already answers internally.
    //
    // drawMesh submits to every feature BEFORE honouring suppression (a suppressing feature is
    // usually building its own scene from those submissions, so skipping submitDraw would starve
    // it) -- so an EDITOR-ONLY draw (outline, gizmo, chrome not scene) still gets baked into that
    // feature's output; drawMesh can't filter it out after the fact.
    //
    // Defaults to false, like deviceLost() above, so a backend with no features, and every test
    // mock, is unaffected.
    virtual bool sceneSuppressed() const { return false; }

    // Per-pass GPU timing (the command console's frame-time breakdown is the first consumer)
    // without going through the periodic AVER_INFO log a backend may print on its own.
    //
    // Two frames old, on purpose: timestamps are resolved from a readback slice only readable once
    // the GPU has caught up, which beginFrame fences on before collecting (see D3D12Device's own
    // per-pass-timing comment) -- reading "this frame's" own timings would stall on the GPU to ask
    // how fast the GPU was, creating the stall it reports. A caller polling once a frame reads a
    // rolling average a couple of frames behind, not a live number.
    //
    // Returned by value, not a reference: source data mutates every beginFrame (spans folded in,
    // occasionally reallocated), so a handed-out reference would go stale. This snapshot (a dozen or
    // so short-label nodes) is cheap to copy and safe to hold.
    //
    // Defaults to an unsupported/empty report (supported == false, `nodes` empty) so Vulkan (no
    // machinery yet), D3D11, Null, and every test mock are unaffected, same as deviceLost() above.
    virtual GpuTimingReport gpuTiming() const { return {}; }

    // Throws away what gpuTiming() has accumulated, so the next report averages only the frames
    // from now on. The since-boot average is the right shape for a benchmark run and the wrong one
    // for a question like "what does Play cost": a pass that only runs in Play is divided by every
    // edit frame before it, so a +10 ms pass reads +0.3 ms after ten minutes of editing. The
    // editor calls this when Play starts and stops, and from the profiler panel's Reset button.
    // The log cadence restarts too, so the next "[RHI.D3D12] GPU ..." line lands soon after.
    //
    // Defaulted to a no-op for the same reason gpuTiming() defaults to unsupported: Vulkan, D3D11,
    // Null and every test mock have nothing accumulated to clear.
    virtual void resetGpuTiming() {}

    // GPU self-test: clears a tiny offscreen target to `in` and reads the pixel back into
    // `outRGBA`. True if the read-back matches.
    virtual bool selfTest(const f32 inRGBA[4], f32 outRGBA[4]) { (void)inRGBA; (void)outRGBA; return false; }

    // Uploads a static mesh (positions+normals+indices). Returns a handle, 0 on failure.
    virtual MeshHandle createMesh(const MeshVertex* verts, u32 vertexCount,
                                  const u32* indices, u32 indexCount) {
        (void)verts; (void)vertexCount; (void)indices; (void)indexCount; return 0;
    }

    // Polls the adapter's current video memory budget and usage (see VideoMemoryInfo's own comment
    // for the LOCAL/NON_LOCAL split and what `supported` false means). Defaults to an unsupported,
    // all-zero report so D3D11, Null, and every test mock compile and behave unchanged without
    // implementing this.
    virtual VideoMemoryInfo videoMemory() const { return {}; }

    // Chooses which GPU heap createMesh() uploads to, for calls AFTER this one (existing meshes
    // keep their heap). Default false = BufferKind::Upload (CPU-visible; correct everywhere, but
    // every draw/shadow/voxelisation/BLAS-build re-fetches it across the bus on discrete cards).
    // True moves new meshes to the Default heap, trading a one-shot sync upload copy (D3D12 impl's
    // comment) for that traffic -- generalising VoxiRenderer.cpp's BLAS-buffer precedent. Permanent
    // default on D3D11/Null/mocks.
    virtual void setStaticMeshHeapDefault(bool onDefaultHeap) { (void)onDefaultHeap; }
    virtual bool staticMeshHeapDefault() const { return false; }

    // Creates a mesh SHARING `source`'s vertex buffer with its own index buffer -- inverse of
    // createSkinTargetMesh's split below, for an LOD ladder: coarser levels reuse the same vertex
    // stream and only thin the triangle list (W11 in the optimisation plan). REFCOUNTED across
    // every sharer; destroyMesh on any is refused while shares remain outstanding.
    //
    // `source` must be alive and not compute-written (a skin target would make every sharer jitter
    // with its last pose). Bounds (centre/radius/AABB) are copied from `source` since a coarser
    // index list can't exceed its extents.
    //
    // 0 on any refusal (unsupported backend, dead/invalid/compute-written source -- see
    // createPosedPartMesh for the posed case); caller MUST fall back to createMesh with its own
    // full vertex array. Permanent fallback on D3D11, Null, every mock, and Vulkan until implemented.
    virtual MeshHandle createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) {
        (void)source; (void)indices; (void)indexCount; return 0;
    }

    // Releases a mesh's GPU memory. False if the handle is invalid, already dead, or still shared.
    //
    // Needed because meshes used to live until the device did (LandscapeRenderer capped its cache
    // and drew a coarser ancestor once full -- its own warning said "there is no destroyMesh, so
    // residency cannot be reclaimed" -- and SkinnedScene recycled dead entries, both working around
    // the missing free) -- fatal for a streaming world: chunks in without chunks out is a leak with
    // a camera attached.
    //
    // Handle NOT recycled: the slot is cleared and kept, so a stale handle addresses a dead mesh,
    // not a different live one that would silently draw wrong geometry -- a few dozen dead-slot
    // bytes against the megabytes reclaimed (a generation in the handle is the fix if churn ever
    // matters). Acceleration structures go with it: a BLAS holds the mesh's GPU addresses, so the
    // backend destroys any built from this mesh too.
    virtual bool destroyMesh(MeshHandle mesh) { (void)mesh; return false; }

    // Creates a mesh whose VERTEX BUFFER IS A COMPUTE TARGET, sharing `source`'s index buffer.
    //
    // How skinning reaches the rasteriser -- deliberately a creation entry point rather than a draw
    // modifier: threading a substitute stream through submitDraw, draw records, replay passes and
    // both backend draw verbs risks ONE MISSED SITE giving a rest-pose shadow beside a posed
    // character. Substituting the handle instead leaves all of that untouched, and gives each
    // instance its own acceleration structure for free -- which a shared MeshHandle could never do.
    //
    // Returned buffer is where compute writes rhi::MeshVertex elements, SEEDED with `source`'s
    // vertices so an unposed draw shows the bind pose, not uninitialised (plausible-looking) memory.
    //
    // Zero on failure; `outVertices` then untouched.
    virtual MeshHandle createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) {
        (void)source; (void)outVertices; return 0;
    }

    // A POSED PART: a mesh SHARING a compute-written mesh's vertex buffer (createSkinTargetMesh
    // result) with its OWN index buffer -- the per-material split of a skinned mesh, cut over its
    // live pose. `indices` are in the SOURCE's vertex numbering (a slice of the base mesh's index
    // list).
    //
    // Exists because a multi-material mesh is split at load into one mesh per slot, but a skinned
    // entity draws a different (posed) handle the split was never cut from, falling back to ONE
    // draw under the entity's material (e.g. hair-card slots never got the hair material's alpha
    // cut-out). This builds the split over the pose instead.
    //
    // Unlike createMeshSharingVertices: REQUIRES a compute-written source, range-checks every
    // index, and marks the result compute-written too, so meshVertexBuffer() is non-zero for it and
    // every consumer (prepass exclusion, BLAS rebuild, GI-voxelisation/PT-scene exclusion,
    // drawMeshDepthOnly) treats it as the whole posed mesh. Refcounted the same way: destroyMesh on
    // the source is refused while a part lives, and destroying the part itself never frees the
    // shared buffer (gated on ownership, not compute-written). 0 on refusal; caller keeps the
    // whole-mesh draw (permanent fallback on D3D11/Null/mocks).
    virtual MeshHandle createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) {
        (void)posedSource; (void)indices; (void)indexCount; return 0;
    }

    // Non-zero (the buffer they live in) when a mesh's vertices are WRITTEN BY COMPUTE rather than
    // uploaded once; zero for every ordinary mesh.
    //
    // Lets a consumer caching something derived from the vertices know its cache expires every
    // frame. Forced by acceleration structures: memoised once per mesh, right for static geometry,
    // but a skinned character's ray-traced shadow would keep the silhouette from when it was built
    // -- the character moves, its shadow doesn't -- without this.
    virtual BufferHandle meshVertexBuffer(MeshHandle mesh) const { (void)mesh; return 0; }

    // A mesh's geometry as BUFFERS a shader can get descriptors over, plus element counts. False
    // when the backend can't express it -- the signal to fall back, not to read nothing.
    //
    // What a RAY needs that a raster draw never did: a hit gives back only a primitive index and
    // barycentrics, so reconstructing position/normal/uv means indexing these streams from the
    // shader. Without them, ray tracing could answer "is something there" -- enough for a shadow,
    // not for a reflection.
    virtual bool meshGeometry(MeshHandle mesh, BufferHandle* vb, BufferHandle* ib,
                              u32* vertexCount, u32* indexCount) const {
        (void)mesh; (void)vb; (void)ib; (void)vertexCount; (void)indexCount; return false;
    }
    // Mesh's LOCAL-SPACE bounding sphere (meshBounds, below) -- centre and radius before any world
    // transform, computed once at createMesh from the vertex AABB (centre = midpoint, radius =
    // distance to a corner): contains the AABB (which contains every vertex), so it's conservative,
    // not tight -- false "might be visible", never false "definitely not" under culling. False when
    // the backend has no bounds, signalling skip culling rather than treating an all-zero sphere as
    // a real point.
    // Mesh's LOCAL-space axis-aligned extents (meshBoundsAabb, immediately below), or false if never
    // measured.
    //
    // The sphere is right for a frustum cull (one centre/radius/dot product) but wrong for
    // containment: a sphere around a wide shallow pool bulges above its surface, so "is the camera
    // in the water" wrongly reads yes from the poolside. Use the AABB for point-in-volume or
    // "where's the top" questions instead.
    virtual bool meshBoundsAabb(MeshHandle mesh, f32 outMin[3], f32 outMax[3]) const {
        (void)mesh; (void)outMin; (void)outMax; return false;
    }
    virtual bool meshBounds(MeshHandle mesh, f32 outCentre[3], f32* outRadius) const {
        (void)mesh; (void)outCentre; (void)outRadius; return false;
    }
    // Per-frame camera (row-major, row-vector viewProj = view*proj). invViewProjRel is the inverse
    // of the SAME view*proj with the view's translation removed, mapping clip space to a
    // world-space OFFSET FROM cameraPos -- precise however far the camera is from the world origin,
    // which the absolute inverse is not (PerFrameCB::invViewProjRel). Shaders take view rays from
    // it through averViewRayDir (shaders/shared_prelude.hlsl).
    virtual void setCamera(const f32 viewProj[16], const f32 invViewProjRel[16], const f32 cameraPos[3]) {
        (void)viewProj; (void)invViewProjRel; (void)cameraPos;
    }
    // Reads the camera back, for a feature fitting its own frustum to the view. False when the
    // backend has no camera to give. Any output may be null. invViewProjRel is the same
    // camera-relative inverse setCamera takes; a caller that has no use for the inverse passes
    // nullptr for it.
    virtual bool camera(f32 viewProj[16], f32 invViewProjRel[16], f32 cameraPos[3]) const {
        (void)viewProj; (void)invViewProjRel; (void)cameraPos; return false;
    }
    // The scene's own viewport rect in target pixels -- {x, y, w, h} -- for a feature reprojecting
    // a screen-space position between frames. NOT necessarily the whole render target: the editor
    // docks the 3D view in a sub-rect of the backbuffer, and NDC alone does not say where that
    // sub-rect sits. False when the backend has no viewport to give.
    virtual bool sceneViewport(f32 rect[4]) const { (void)rect; return false; }
    // Sets the directional light and the ambient term.
    virtual void setLight(const f32 dirToLight[3], const f32 color[3], f32 ambient) { (void)dirToLight; (void)color; (void)ambient; }
    // Sets the sky, the sun and the air. Supersedes setLight for the sun.
    // Engine clock, forwarded into PerFrameCB::time so any shader can animate.
    //
    // Separate from SkyAtmosphere::cloudTime (the cloud layer's own authored-weather drift, not a
    // wall clock) -- a paused sky must not freeze animated materials. `seconds` is raw and
    // monotonic; the backend wraps it as the shader needs (see PerFrameCB::time).
    // Sets both the surface shader's and the caustics' wave read (see PerFrameCB::wave for why one
    // array, not a copy each). `waves`: up to 3 entries of {dirX, dirY, k (rad/cm), speed (rad/s)};
    // `count` 0 disables the surface.
    //
    // Caller may generate these however it likes (authored records, PCG, gameplay reacting to
    // weather) -- the renderer doesn't know or care where they came from.
    virtual void setWaterWaves(const f32 (*waves)[4], u32 count, f32 amplitude) {
        (void)waves; (void)count; (void)amplitude;
    }
    virtual void setFrameTime(f32 seconds, f32 deltaSeconds) { (void)seconds; (void)deltaSeconds; }
    virtual void setSkyAtmosphere(const SkyAtmosphere& s) { (void)s; }
    virtual SkyAtmosphere skyAtmosphere() const { return {}; }
    // Sets the camera post-processing chain.
    virtual void setPostProcess(const PostSettings& p) { (void)p; }
    virtual PostSettings postProcess() const { return {}; }
    // The auto-exposure multiplier CSExposure (post.hlsl) last produced, read back from the GPU a
    // few frames late -- a live UI number, not something a draw call may depend on. False when
    // auto-exposure hasn't run yet or the backend can't read it back. THE METERED VALUE, before
    // PostSettings::exposure (the manual compensation multiplier) and before local exposure --
    // combine both for "what multiplies the scene".
    virtual bool postExposureReadout(f32& adaptedExposure) const { (void)adaptedExposure; return false; }
    // Records one draw of `mesh` with a world matrix (row-major), base colour, and PBR
    // metallic/roughness (0..1).
    virtual void drawMesh(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
    }

    // Sets binding table 1 and its b2 constant block, sticky until changed and consumed by every
    // subsequent drawMesh. `constants` is COPIED; the caller may reuse its buffer immediately.
    virtual void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }
    // Sets what beginFrame resets the above to. The identity/fallback set belongs here.
    virtual void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }

    // ---- same-frame depth prepass (see docs/RENDERING.md and D3D12Device::drawMesh) ----
    //
    // Off (default) is the unchanged pre-existing path: drawMesh shades immediately,
    // drawMeshDepthPrepass is a no-op, setNextDrawPrepassed's flag is never read. Defaulted no-op
    // like setUpscaler above, so Vulkan's mid-bring-up keeps compiling without this yet.
    virtual void setDepthPrepassEnabled(bool on) { (void)on; }
    virtual bool depthPrepassEnabled() const { return false; }

    // Draws `mesh`'s depth ONLY, via whichever registered feature both overridesScenePipeline() and
    // returns non-zero from depthPrepassPipeline() -- no-op otherwise, or when
    // depthPrepassEnabled() is false. `world` matches an equivalent drawMesh call; colour/metallic/
    // roughness aren't needed since nothing here is shaded, only tested and written.
    //
    // A CALLER MUST NOT PASS A COMPUTE-WRITTEN (SKINNED) MESH HERE (see meshVertexBuffer). This
    // checks and silently declines defensively, but the primary contract is the caller not calling
    // this for such a mesh at all.
    //
    // `color` matches the paired drawMesh()'s array and is not decoration: gBaseColor is the
    // per-object constant that array fills, and the depth shader's alpha test is
    // `gBaseColor.a * gBaseColorFactor.a * texture.a` (voxi.hlsl PSDepthPrepass). This used
    // to leave that slot zero, so every alpha-masked material clipped every pixel and wrote NO
    // depth -- and its "prepassed" colour draw then wrote none either. Found by adversarial review;
    // latent only because the prepass itself was broken.
    virtual void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
        (void)mesh; (void)world; (void)color;
    }

    // drawMeshDepthPrepass's body WITHOUT the frame-wide depthPrepassEnabled() gate or per-frame
    // census -- for a SINGLE draw needing depth written by the alpha-testing depth-only shader
    // immediately before its colour draw, prepass on or off. Follow with
    // setNextDrawPrepassed(true), as for a whole-frame prepass.
    //
    // Exists for an ALPHA-MASKED material under forced early depth: PSMainVoxi is
    // [earlydepthstencil] (see its own comment), so depth writes BEFORE the shader runs and the
    // alpha clip() in averEvalMaterial can't take it back, hiding whatever's behind a cut-out.
    // Writing depth first through PSDepthPrepass (clips before its write), then colour with the
    // LessEqual/no-write twin, restores the clip.
    //
    // A COMPUTE-WRITTEN (skinned/soft-body) MESH IS ACCEPTED HERE, unlike drawMeshDepthPrepass: its
    // posed buffer IS its vertex buffer (createPosedPartMesh shares it) and skinning already ran
    // this frame, so the following colour draw reads identical vertices. drawMesh only honours the
    // flag for that exact handle, else the colour draw rejects its own equal depth and vanishes.
    //
    // Returns whether depth was written; caller marks the colour draw prepassed only on true. False
    // in wireframe, when a feature suppresses the rasterised scene (ray-driven primary visibility, a
    // debug view -- there's no raster colour pass to consume the depth, and writing over the
    // ray-driven pass's own depth would be wrong), or when no feature offers a depth-only pipeline.
    // `color` as for drawMeshDepthPrepass, same alpha-test reason.
    virtual bool drawMeshDepthOnly(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
        (void)mesh; (void)world; (void)color; return false;
    }

    // Marks the VERY NEXT drawMesh() call as one whose depth a prior drawMeshDepthPrepass() call
    // already wrote for the identical mesh/world THIS SAME FRAME, so the backend uses the scene
    // feature's depth-tested-only pipeline variant (depthPrepassed=true) instead of the ordinary one.
    //
    // Auto-consumed, not sticky, unlike setDrawBinding above (whose material stays correct until
    // changed): this flag concerns only the one upcoming draw, and dozens of call sites (landscape,
    // skinned characters, gizmos, selection outline) have no idea it exists. Staying set after being
    // read would silently corrupt every later draw's depth state if one caller forgot to clear it.
    // drawMesh always resets it to false, so never calling this reproduces today's behaviour exactly.
    virtual void setNextDrawPrepassed(bool prepassed) { (void)prepassed; }

    // ---- translucency: the blended-mesh path ----
    //
    // Marks every subsequent drawMesh() as TRANSLUCENT until changed. Sticky like setDrawBinding
    // (unlike setNextDrawPrepassed above): a property of the current MATERIAL, not one draw.
    // beginFrame resets it to false.
    //
    // A true flag is more than a blend state: a blended draw leaves the opaque path entirely, at
    // the top of drawMesh, BEFORE IRenderFeature's submitDraw loop -- not voxelised, not in the
    // acceleration structure, not shadow-cast, not depth-prepassed. The device captures it and
    // replays every draw BACK-TO-FRONT after the deferred sky and before
    // IRenderFeature::transparentPass, via scenePipeline(..., blended = true).
    //
    // Ordering is not negotiable: drawn with the opaque pass it would land BEFORE the deferred sky,
    // whose depth-EQUAL fill would overwrite it (the failure the particle pass was moved to fix; see
    // D3D12Device::endFrame's sky-draw comment) -- sorting the caller's own draw list can't fix
    // this since the sky isn't in it, hence the capture.
    //
    // Cost: excluded from the acceleration structure, glass casts no ray-traced shadow and shows in
    // no reflection; excluded from voxelisation, no GI bounce. Correct FIRST answers (a solid black
    // shadow is worse than none) but approximations -- coloured transmission shadows need
    // translucent draws back in with an any-hit-readable flag.
    virtual void setDrawBlended(bool blended) { (void)blended; }
    // What setDrawBlended last set, for a caller that saves and restores it around a nested draw.
    virtual bool drawBlended() const { return false; }

    // Unlit line geometry (grid, gizmos, selection outlines, collider/nav overlays): per-vertex
    // DISPLAY colour, drawn as a line list.
    virtual LineHandle createLineMesh(const LineVertex* verts, u32 count) { (void)verts; (void)count; return 0; }
    // EDITOR CHROME, DRAWN AFTER THE CAMERA POST CHAIN, the way Unreal composites its editor
    // primitives: the call QUEUES (mesh, world, the current setLineDepth / setLineWidth state) and
    // the device replays the queue once per frame right after the tonemap, into the display-
    // resolution target the overlay features use, before any overlayPass and the UI. So a line shows
    // exactly its authored colour -- no exposure, tonemap, bloom or local exposure touches it, and
    // AverSR does not resample it -- and is crisp at display resolution.
    //
    // Depth-tested lines (setLineDepth(true), the default) are occluded by sampling the scene depth
    // (sceneDepthTexture) in the pixel shader, since the display target and the scene depth differ
    // in size under a render scale: a small linear-depth tolerance keeps a line lying ON a surface
    // (the grid on a floor, an outline on its mesh) visible. Overlay lines (false) draw on top.
    //
    // Used to write inverse-tonemapped radiance into the HDR scene target, which auto-exposure then
    // multiplied (x70-150 in a lit level) and bloom haloed: every gizmo, outline and marker glowed.
    virtual void drawLines(LineHandle mesh, const f32 world[16]) { (void)mesh; (void)world; }
    // Releases a line mesh's GPU memory. False for a stale or already-released handle.
    //
    // Every line mesh was a committed UPLOAD-heap buffer with no way to free it -- createLineMesh
    // appended to a vector that only ever grew. Fine for the handful the editor builds once at
    // startup (the grid, the gizmo, the sculpt ring), a leak per rebuild for anything that CHANGES
    // (a navmesh, an agent's path, a perception cone). docs/CHUNKS.md raised this for meshes (B6)
    // and destroyMesh was added; line meshes were missed until now.
    //
    // Slot kept, not recycled, same reason as destroyMesh: a stale handle must address a DEAD mesh,
    // not silently hand a new caller's handle 3 to whoever still held the old one -- a bug that
    // looks like corruption and can't be traced back to here.
    virtual bool destroyLineMesh(LineHandle mesh) { (void)mesh; return false; }

    // Mesh shader geometry path (mesh-shader Tier 1 + SM 6.5), replacing the input-assembler vertex
    // path for every draw. Ignored when unavailable.
    virtual void setMeshShaders(bool enabled) { (void)enabled; }
    virtual bool meshShadersActive() const { return false; }

    // THE WIREFRAME VIEW, Unreal's: while on, drawMesh shades nothing. Each mesh is queued for the
    // overlay stage and drawn there as unlit edges (EditorLines::queueWire) after the post chain,
    // every edge visible, static meshes cyan and compute-written ones magenta. The sky, particles
    // (transparentPass) and auto-exposure metering are skipped; features still receive submitDraw.
    // Sticky until toggled off.
    virtual void setWireframe(bool on) { (void)on; }
    // Draws the next mesh with NO LIGHTING -- flat gBaseColor, no sun, no ambient, no fog.
    //
    // Turns on a path that already existed but was unreachable: plainShadeSurface's
    // `if (gMaterial.z > 0.5) return float4(gBaseColor.rgb, gBaseColor.a)` (shared_prelude.hlsl,
    // slot documented as "z=unlit(0/1)") was dead because every call site hardcoded that constant
    // to 0.
    //
    // A setter, not a wider drawMesh, matching setWireframe/setDrawBlended/setLineDepth: widening
    // would drag IRenderFeature::submitDraw's signature along for a flag no feature needs to see.
    //
    // Sticky like setWireframe: nothing resets it per frame; the caller brackets its own draw.
    virtual void setUnlit(bool on) { (void)on; }

    // Line depth testing. Default true; false draws subsequent lines as an always-on-top overlay.
    // Sticky; captured per drawLines call, since the draw itself is deferred (see drawLines).
    virtual void setLineDepth(bool testDepth) { (void)testDepth; }

    // Line thickness in DISPLAY pixels for subsequent drawLines calls. Default 1; sticky like
    // setLineDepth and captured per call the same way. The caller scales by its own DPI -- the
    // device has no idea what a pixel means to the person looking at it. Unreal draws its gizmo
    // handles and selection outline a few pixels wide; a 1px line at 300% DPI all but vanishes.
    // (Replaces setLineGlow: lines no longer go through the HDR target, so there is no bloom to
    // make them glow, which is what was asked for.)
    virtual void setLineWidth(f32 pixels) { (void)pixels; }

    // Captures the backbuffer pixel at (x,y) during the next presented frame; poll getCapture().
    virtual void requestCapture(u32 x, u32 y) { (void)x; (void)y; }
    virtual bool getCapture(f32 outRGBA[4]) { (void)outRGBA; return false; }
    // Full captured frame (tight RGBA8, top-to-bottom) after a requestCapture completes.
    virtual bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) { (void)outRGBA; (void)w; (void)h; return false; }

    // Initialises in-window UI on this device. False if the backend has no UI support, or has
    // support but nothing installed to host (D3D12: aver::rhi::d3d12::IUiBackend/installUiBackend,
    // UiBackend.hpp) -- deliberately silent about which toolkit. Widgets build between uiNewFrame()
    // and endFrame().
    virtual bool uiInit(void* windowHandle) { (void)windowHandle; return false; }
    virtual void uiNewFrame() {}
    virtual void uiShutdown() {}
    virtual bool uiActive() const { return false; }
    virtual bool uiWantsMouse() const { return false; }    // true when the cursor is over UI
    virtual bool uiWantsKeyboard() const { return false; }

    // Makes a texture drawable by the UI, returning the identifier the UI layer expects as a plain
    // integer. Cached on the texture. 0 where the backend hosts no UI.
    virtual u64 uiTextureId(TextureHandle t) { (void)t; return 0; }

    // The backend's OWN scene depth target, registered as an ordinary TextureHandle through the
    // SAME resource-factory table createTexture() populates, so a caller reaches it with the
    // generic setSrv/textureBarrier vocabulary rather than a bespoke accessor (deliberate
    // design-review correction -- see modules/occlusion/include/aver/occlusion/Occlusion.hpp's top
    // comment, point (c)). Declares its own
    // sample count via sampleCount() above (multisampled needs SlotKind::Texture2DMS, not
    // Texture2D).
    //
    // Defaulted no-op, same shape as setUpscaler/setDepthPrepassEnabled: Vulkan is mid-bring-up,
    // Null has no depth buffer at all. 0 before the first swapchain resize, like every other
    // size-dependent target here.
    virtual TextureHandle sceneDepthTexture() { return 0; }

    // The opaque scene, copied, so a translucent surface can read what's behind it.
    //
    // Hardware alpha blending attenuates the destination by one scalar (1-src.a), but volume
    // absorption (Beer-Lambert) is per-channel and grows with path length -- glass is green because
    // iron passes green, eats red. So a blended surface can go DARKER with depth via alpha, but
    // never TINT what's behind it -- the whole reason glass here couldn't show real glass's green
    // edge. Handing the shader the background as a texture lets it composite itself instead. Returns a
    // copy of the scene colour taken just BEFORE blended draws replay -- the same trick AverSR uses
    // for the upscaler's raw render target.
    //
    // One copy, taken once: a second translucent layer samples a background missing the first
    // (glass over water reads water's backdrop, not the water) -- the standard trade (UE's
    // distortion pass makes it too), cheaper than per-channel destination blending, which D3D12
    // can't offer here (four render targets bound with G-buffer on, dual-source blending needs
    // exactly one).
    //
    // 0 when unavailable (before first resize, unimplemented backend, or under MSAA where
    // CopyResource into single-sample is invalid) -- shader falls back to scalar composite.
    virtual TextureHandle sceneColorBackdropTexture() { return 0; }

    // ---------------------------------------------------------------------------------------
    // G-buffer: velocity, view-space depth, and world normal+roughness, written ALONGSIDE the
    // forward scene pass at scene resolution -- three extra render targets, nothing else changed.
    //
    // Aver is a FORWARD renderer (PSMainVoxi returns one SV_TARGET): a shaded pixel's
    // normal/roughness/depth live only in that invocation's registers, unreadable by any later
    // pass. Blocks the vendored FidelityFX Denoiser (third_party/fidelityfx-denoiser's README lists
    // ReadDepth/ReadNormals/ReadVelocity/ReadPreviousDepth as callbacks the host must supply; today
    // the honest answer is "no" to each), FSR2/3, TAA, and SSR alike -- each needs to read what a
    // previous pass saw, which until now nothing could hand it. Also why
    // temporal reprojection is wrong for moving objects today (reprojects THIS frame's position
    // through LAST frame's camera, gPrevViewProj in VoxiShaders.hpp -- correct only for static
    // geometry; the full fix needs a per-instance previous transform this G-buffer doesn't carry,
    // see RtInstance in VoxiRenderer.hpp). The per-pixel velocity below is what every consumer
    // above needs regardless. Fuller writeup: docs/rendering/DENOISING.md.
    //
    // Additive and defaulted throughout: setGBufferEnabled defaults OFF, and every accessor below
    // defaults to a safe "nothing here" value (0 for a texture, false/true for the bools, whichever
    // is the safe reading), so never enabling it allocates none of the three targets, records no
    // extra writes, and renders BIT-IDENTICAL to a build without this declaration.
    //
    // OPEN: a prior "nothing yet enables this" claim here went false within a day of being written
    // (2026-08-29) -- SandboxApp.cpp passes `gbufferOverride_ || gbufferDebugView_ != Mode::Off`,
    // driven by --gbuffer/--gbuffer-debug, and viewport debug reads it back; only the packaged
    // runtime abstains. Left in as a reminder this file has a history of
    // stale absolutes outliving what they described; a render-gate oracle (18x9 configs) and 89
    // headless suites both assume the accessors hold their defaults while gBufferEnabled() is false
    // -- a backend that allocates or writes any of this while reporting false would fail both
    // without pointing at why.
    //
    // This class only declares the surface; populating the three targets is the backend's call --
    // every method here defaults inert so an unimplemented backend compiles and behaves unchanged.
    virtual void setGBufferEnabled(bool on) { (void)on; }
    virtual bool gBufferEnabled() const { return false; }

    // Scene-resolution screen-space motion, Format::RG16F. UNITS: TEXELS PER FRAME, DESTINATION
    // TEXEL MINUS SOURCE TEXEL -- for a point shaded at THIS frame's pixel (x,y), stored (vx,vy)
    // satisfies (x,y) - (vx,vy) == where that surface point was LAST frame. Matches
    // UpscalerNeeds::MotionVectors' convention (RHIResources.hpp) exactly, since FSR2/3, DLSS, and
    // FFX_DNSR_Shadows_ReadVelocity all read texel-space motion this direction -- a mismatch here
    // would silently reproject to the wrong pixel with no compile error or crash.
    //
    // 0 when gBufferEnabled() is false or unimplemented -- same "absent, not garbage" contract as
    // sceneDepthTexture() above; a caller must check gBufferEnabled() rather than assume non-zero.
    virtual TextureHandle gBufferVelocityTexture() { return 0; }

    // Scene-resolution depth, Format::R32Float. UNITS: VIEW-SPACE LINEAR DEPTH (shaded point's Z in
    // view space, i.e. clip-space W pre-divide) -- deliberately NOT sceneDepthTexture()'s
    // post-projection [0,1] value; the two relate by a non-linear "depth precision" remapping, so
    // treating this as [0,1] depth is quietly wrong at every pixel, not a crash. FFX_DNSR_Shadows_
    // ReadDepth/ReadPreviousDepth and any SSR pass want this linear form because it makes
    // reconstructing a view-space position from a screen UV a single division, not a full unproject.
    //
    // 0 when gBufferEnabled() is false or unimplemented, matching gBufferVelocityTexture() above.
    virtual TextureHandle gBufferViewZTexture() { return 0; }

    // Scene-resolution normal and roughness, Format::RGB10A2Unorm -- packed to NRD's OWN
    // NRD_NORMAL_ENCODING_R10G10B10A2_UNORM layout (third_party/nrd/Shaders/NRDConfig.hlsli), NOT a
    // plain n*0.5+0.5-with-roughness-in-w scheme (that's NRD's #else layout for encodings 0/3, and
    // wrong here -- an earlier version of this contract documented it, and REBLUR decoded garbage
    // normals and view-angle-dependent roughness as a result). xyz JOINTLY encode both: an
    // improved-octahedral fold puts N into x/y, z carries roughness's MAGNITUDE with the SIGN OF
    // N.z riding on z's own sign (roughness is never exactly 0, so that sign bit always has
    // something to carry). Encode: averPackNormalRoughness (modules/render.voxi/shaders/voxi.hlsl,
    // transcribed byte-exact from NRD's _NRD_EncodeNormalRoughness101010); decode:
    // sandbox/shaders/gbuffer_debug.hlsl. w: materialID/3 in NRD's
    // convention, always 0 here (no material-ID concept yet), NOT roughness. A raw n*2-1 sample of
    // xyz is wrong -- needs the full decode (NRD_FrontEnd_UnpackNormalAndRoughness or the pair
    // above); assuming the OLD contract gives a plausible-looking but per-pixel-wrong vector --
    // exactly the shape of bug a casual visual check misses, and exactly how this one shipped.
    //
    // 0 when gBufferEnabled() is false or unimplemented, matching the two accessors above.
    virtual TextureHandle gBufferNormalRoughnessTexture() { return 0; }

    // Previous frame's view-projection -- ROW-MAJOR, ROW-VECTOR, same convention as setCamera's
    // `viewProj` -- valid whenever gBufferEnabled() is true. Every temporal consumer of the three
    // textures above needs this to reproject a one-frame snapshot against accumulated history
    // (FFX_DNSR_Shadows_GetReprojectionMatrix's job). False when the G-buffer is off or
    // unimplemented, leaving `outPrevViewProj` untouched -- same "ask before you trust it" contract
    // as camera() above; skipping the check gives a plausible-looking wrong reprojection, not a crash.
    virtual bool gBufferPrevViewProj(f32 outPrevViewProj[16]) const { (void)outPrevViewProj; return false; }

    // True when the three textures above -- and gBufferPrevViewProj, meaningless without them --
    // do NOT describe a continuous previous frame: first frame enabled, a camera cut, a level load,
    // a resolution change, or anything else making "reproject against last frame" nonsense rather
    // than merely stale. Every temporal consumer must ask this, not infer it: getting it wrong is
    // ONE BAD FRAME right after every cut -- invisible to a still-frame review, always visible once
    // the camera actually moves (see this engine's own per-object reprojection bug above).
    //
    // Defaults to TRUE (conservative: "assume invalid"), so a caller talking to a backend without
    // the G-buffer drops one frame of temporal reuse rather than accumulating against a frame that
    // was never rendered into these targets.
    virtual bool gBufferHistoryInvalid() const { return true; }
};

// Converts a colour temperature in Kelvin to LINEAR sRGB, normalised so the brightest channel is 1.
// Clamped to 1000..15000 K. LINEAR: a caller storing this into a display-encoded field (sunColor,
// lightColor) must re-encode with pow(x, 1/2.2) first, or every downstream pow(x, 2.2) decode
// reads it twice.
void blackbodySrgb(f32 kelvin, f32 outRgb[3]);

// Creates the first available device in the desc's preference order.
IDevice* createDevice(const DeviceDesc& desc = {});
// Destroys a device created by createDevice.
void destroyDevice(IDevice* device);

// Handler a backend hosting ImGui registers, so the platform Window can forward raw messages.
using UiWndProcFn = bool (*)(void* hwnd, u32 msg, u64 wparam, i64 lparam);
// Registers the handler raw window messages are routed to.
void registerUiWndProc(UiWndProcFn fn);
// Forwards one window message to the registered handler. False when there is none.
bool uiWndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam);

} // namespace aver::rhi
