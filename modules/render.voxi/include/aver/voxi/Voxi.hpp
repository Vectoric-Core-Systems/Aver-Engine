// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
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
enum class Feature : u32 { Msaa = 0, GlobalIllumination, RayTracing, PathTracing, MeshShaders,
                          LayeredBsdf, Count };

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
    // The denoiser can run on this backend (see RenderSettingsResolver.hpp's
    // DisableReason::RequiresDenoiserBackend): it reads the G-buffer, which only D3D12 provides.
    // Computed by the host as `backend() == rhi::Backend::D3D12` at the same two call sites
    // (SandboxApp.cpp, GameApp.cpp); this struct only carries the answer so it stays free of any RHI
    // dependency.
    bool denoiserSupported = false;
};

// The renderer quality settings, as requested. Clamped to the device by Renderer::setSettings.
struct Settings {
    Msaa    msaa               = Msaa::X4;
    Quality globalIllumination = Quality::Medium;
    // ON BY DEFAULT AT MEDIUM; rtShadowRays/rtPixelsPerRayTile below are Medium's rungs by construction.
    Quality rayTracing         = Quality::Medium;
    Quality pathTracing        = Quality::Off;

    // A layered BSDF alongside the standard BRDF (not instead). Quality, not bool, because GGX is not free.
    // NOT LIVE-SWITCHABLE: needs VoxiRenderer to recompile ~20 raster PSOs; changing it needs a project reload.
    Quality layeredBsdf        = Quality::Off;
    bool    meshShaders        = false;

    // Cubic voxel grid edge; memory and per-voxel GPU cost are O(this^3). Defaults to Medium's rung (128).
    // Derived from globalIllumination on a tier change when left unchanged.
    u32 voxelResolution = 128;

    // Total cones the diffuse gather traces, including the axial one. Defaults to Medium's rung (6).
    // Ladder: Low 3, Medium 6, High 9, Epic 13 (giConesForQuality in Voxi.cpp).
    u32 giCones         = 6;
    // Sky-visibility rays the AMBIENT term traces per pixel. 0 = estimate from the cone gather.
    // Derived from rayTracing on a tier change: 0 at Low, 1 at Medium/High/Epic.
    u32 giSkyOcclusionRays = 1;
    // Edge, in pixels, of the square tile sharing one sky-occlusion ray direction. 1 = fresh rotation per pixel.
    // Coherence reduces wave divergence but trades per-pixel noise for correlated tile noise.
    u32 giSkyOcclusionTile = 1;
    f32 giIntensity     = 1.0f;
    // Caustics: how strongly light focused by a water surface brightens what's beneath. 0 = off.
    // Not on the quality ladder -- it's a LOOK, not a fidelity rung.
    f32 causticStrength = 0.6f;
    f32 giMaxDistance   = 4000.0f;  // centimetres

    // GI radiance ceiling before tonemap; mirrored as AVER_VOX_MAXRAD in voxi.hlsl via FrameConstants::viewParams.y.
    // DEFAULT MUST STAY 16.0: byte-identical to every image before ReSTIR GI existed.
    f32 giRadianceCeiling = 16.0f;

    // ---- refraction: how a translucent surface BENDS what is behind it ----
    // Absorption (attenuationColor) decides colour; refraction decides where it comes FROM.
    //   Off (0)          background sampled straight through.
    //   ScreenSpace (1)  offset by the refracted view direction x ray-measured thickness.
    //   RayTraced (2)    refracted ray through the TLAS, hit point projected back to screen.
    u32 refractionMode     = 1;
    // Multiplies the offset. 1.0 is the physical bend; below trades correctness for calm, above exaggerates.
    f32 refractionStrength = 1.0f;
    // How far from the screen edge the offset is faded out, as a fraction of the smaller dimension.
    f32 refractionEdgeFade = 0.15f;

    // ---- ray-traced sun shadow: rays per trace, and how many pixels amortise one trace ----
    // Occlusion rays per pixel when this pixel traces this frame. Clamped to [1, VoxiRenderer::kMaxShadowRays].
    // Ladder: Low 1, Medium 1, High 4, Epic 8.
    u32 rtShadowRays = 1;
    // Edge length of the square tile one traced shadow pixel is amortised over via temporal history.
    // 1 = every pixel traces every frame. N>1 uses reprojected history. Must be a power of two.
    u32 rtPixelsPerRayTile = 1;

    // How many frames apart the GI volume is re-voxelised: 1 revoxelises every frame.
    // N>1 reuses the previous frame's volume for N-1 frames, amortising voxelizePass/filterMips.
    // Ladder: Low 4, Medium 2, High 1, Epic 1.
    u32 giUpdateInterval = 2;

    // ---- WHICH ESTIMATOR ANSWERS THE DIFFUSE BOUNCE: the voxel cone gather, or ReSTIR GI ----
    // 0 = cone gather (DEFAULT). 1 = ReSTIR GI: one traced candidate per pixel, reused via spatio-temporal resampling.
    // NOT ON THE QUALITY LADDER: this SWITCHES estimators, not a Low-to-Epic rung.
    // DEFAULT IS 0 AND MUST STAY 0: byte-identical to every image before ReSTIR GI existed.
    u32 giMode = 0;
    // ---- ReSTIR GI VISIBILITY: how much of F2/F3 (two per-pixel rays) each globalIllumination rung pays for ----
    // NoRay restores pre-fix over-brightness; Reconstructed replaces with one voxel-cone march;
    // HalfResolution traces 1-in-4 pixels/frame, reconstructing the rest; Full traces every pixel every frame.
    // Cached traces HalfResolution pixels and trains a world-space SH cache (D3D12-only, staged ray-driven).
    enum class RestirVisibility : u32 { NoRay = 0, Reconstructed = 1, HalfResolution = 2, Full = 3, Cached = 4 };
    u32 giRestirVisibility = 2;   // must equal ladder::giRestirVisibility(Quality::Medium)

    // ---- ReSTIR GI spatial reuse control: pin the tap count or disable spatial reuse ----
    // 15 = AUTO (2 taps at rest, fewer in motion). 0 disables spatial reuse. 1..8 pin the tap count.
    // DEFAULT 0 until checked with the denoiser on. GI-only, linear, NeonDistrict Day, denoiser off: two taps read
    // 1.60x the no-reuse GI before the blocked-sample and age fixes, 0.93x after (temporal-only 0.92x).
    u32 giRestirSpatialSamples = 0;

    // ---- ReSTIR GI history weighting: cap on the M a reused reservoir carries into the combine ----
    // 0 turns reuse off entirely (one fresh sample a pixel). DEFAULT 8 since the reuse pass keeps its
    // Jacobian and counts every neighbour domain in its normalisation: the old +8% overshoot at 1 and
    // the darkening that replaced it were those two bugs, not the history.
    u32 giRestirMaxHistory = 8;

    // ---- THE DENOISER OVER THE SKY OCCLUSION AND THE ReSTIR GI RADIANCE ----
    // AMD FidelityFX Denoiser (MIT) through Aver.Render.Denoise -- see modules/render.denoise.
    // Off by default. Requires MSAA 1 and D3D12 (the backend that provides the G-buffer it reads).
    bool denoiser = false;
    // Which denoiser runs while `denoiser` is on: 1 AMD FidelityFX, 2 NRD2 (docs/rendering/NRD2.md:
    // single-frame, no history; D3D12 staged ray-driven only). Read and set through denoiserMode()
    // below: RENDER.DENOISER, --denoiser and voxi.denoiserMode carry 0 (off), 1 or 2.
    u32 denoiserKind = 1;
    // NRD2's fixed per-tile parameters (nrd2_resolve.hlsli), diffuse then specular: level logits for
    // 1/2, 1/4, 1/8 (own pixel pinned at 0), then log2 depth / normal / luminance sensitivities.
    float nrd2Params[12] = {1.0f, 2.0f, 2.0f, 4.5f, 3.0f, -1.0f, 1.0f, 2.0f, 2.0f, 4.5f, 4.0f, -1.0f};
    // Developer: NRD2 recomposes its split without filtering (an A/B check of the split itself).
    bool nrd2Bypass = false;
    // NRD2 phase 4: the trained network sets the per-tile parameters when its weights exist and pass the
    // held-out gate (render::denoise::Nrd2Network); off, the nrd2Params above everywhere.
    bool nrd2Network = true;
    // NRD2's temporal stage (docs/rendering/NRD2.md, after FidelityFX's reflections denoiser): on jitter-free
    // frames (camera moving, or TAA off) the filtered D and S are reprojected, prefiltered and blended with
    // last frame's inside a min/max box of this frame's values. nrd2StabFrames is its history length at rest.
    bool nrd2Stab = true;
    u32 nrd2StabFrames = 32;
    // NRD2 keeps half-rate tracing and fills the skipped checkerboard half from this frame's traced
    // neighbours (CSRdHalfFill; docs/rendering/NRD2.md). Each applies where its base half rate is asked
    // for: GI under rayDrivenStages 2, reflections under rtReflectionHalfRate (glossy only), sky
    // occlusion under rtSkyOcclusionHalfRate. Lamps always (Stage B's 5x5 fills them). Off = full rate.
    bool nrd2HalfRateGi = true;
    bool nrd2HalfRateRefl = true;
    bool nrd2HalfRateAo = true;
    bool nrd2HalfRateLamps = true;
    // Ray-traced reflections through the denoiser's reflection pipeline while it runs (staged
    // ray-driven reflections only); off, they keep Voxi's own reflection history and filter.
    bool denoiseReflections = true;

    // ---- The denoiser's live dials (render::denoise::Denoiser::Tuning) ----
    // Applied every frame the denoiser records, so a console change takes effect next frame with no teardown.
    u32   denoiserMaxSamples = 32;
    // How tightly the reprojected history is clipped to this frame's neighbourhood statistics before blending.
    // 4 (was 0.5): the narrow box clipped bright GI samples and read the denoised GI dark.
    float denoiserHistoryClipWeight = 4.0f;
    // Caps the history length when the sun moves and the one after. Not re-measured with this denoiser.
    u32   denoiserSunMovingSamples = 4;

    // SPATIAL denoise radius for the ray-traced sun shadow, in pixels. 0 = off (unfiltered per-pixel rays).
    // Default 2. See comment history for design rationale and motion behaviour.
    u32 rtShadowDenoise = 2;

    // ---- ray-driven rendering ---------------------------------------------------------------
    // WHICH THING FINDS THE FIRST SURFACE: 0 = rasteriser, 1 = primary ray per pixel.
    // 1 FROM MEDIUM UP; LOW RASTERISES (by explicit product decision, D3).
    u32 rtRenderMode = 1;

    // ---- staged ray-driven passes (milestone 1 split; milestone 4 adds half-rate GI) -----------
    // 0 = SINGLE PASS. 1 = STAGED (split into visibility and shadow compute passes).
    // 2 = STAGED + HALF-RATE GI: traces only half the pixels/frame, denoiser reconstructs the untraced half.
    // DEFAULT 2 since 2026-09-27.
    u32 rayDrivenStages = 2;

    // DIAGNOSTIC ONLY: times each staged lighting pass in its own GPU span. No effect on the image.
    bool rayDrivenStageTiming = false;

    // SUB-STAGE SPLIT A: sun-shadow trace in two passes. MEASURED (staged mode 1, Epic): 4.47 ms saved.
    // NEAR-IDENTICAL IMAGE, not a quality trade.
    bool rayDrivenShadowTiles = true;

    // SUB-STAGE SPLIT B: CSRdGi's candidate trace in two passes. SAME IMAGE as unsplit.
    bool rayDrivenGiSplit = true;

    // SUB-STAGE SPLIT C: reflection stage split. SAME IMAGE as unsplit -- round-trips through same history texture.
    bool rayDrivenReflSplit = true;

    // LOCAL LIGHTS (LAMPS): a material with lightIntensity > 0 turns every draw using it into a small sphere light.
    // lightIntensity MULTIPLIES what the material's glow/size already cast, in sun units.
    // At most 32 lamps/frame (brightest-and-nearest by lit output / squared distance).
    bool localLights = true;

    // ---- staged ray-driven bit-field toggles (cb_.giShadowParams.w / gGiShadowParams.w) ---------
    // Four independent RUNTIME toggles VoxiRenderer::prePass packs into cb_.giShadowParams[3] every frame.

    // T1 (bit 1): secondary-hit rays accept first hit instead of tracing full transmittance. MEASURED alone: still MAD 0.09.
    bool rtSecondaryShadowOpaque = true;

    // T2 (bit 2): rtSkyOcclusionTemporal skips rtAmbientTraced for entire 8x8 TILE where history is valid. MEASURED: 0.25 ms saved.
    bool rtSkyOcclusionHalfRate = true;

    // T3 (bit 4): rtReflectionTemporalEx skips trace for ROUGH pixels on valid history tiles. MEASURED: 0.97 ms saved.
    bool rtReflectionHalfRate = true;

    // T4 (bit 8): sun visibility at ReSTIR GI's candidate HIT comes from GI-only shadow map. ON by default.
    bool rtGiHitShadowMap = true;

    // ---- BIT 16: REUSE THE STAGED RAY-DRIVEN LIGHTING FOR A TRANSLUCENT DRAW ON THE SAME SURFACE ----
    // A translucent pixel over an opaque surface the staged passes already lit THIS frame reads gRdSunVisTex/gRdGiTex/etc.
    // ON BY DEFAULT. MEASURED: blended replay 0.39 -> 0.17 ms at the saved camera.
    bool blendedReuseStagedLighting = true;

    // ---- TRANSLUCENCY IN THE PATH (staged ray-driven, D3D12) ----
    // Glass and every other translucent material is shaded at the primary ray's crossings, front to
    // back over the lit opaque surface, by the same surface builder and composite as everything else,
    // instead of a blended raster replay drawn over the finished ray-driven image. Off: the replay.
    bool translucencyInPath = true;

    // ---- the acceleration-structure "unchanged" gate ------------------------------------------
    // MEASURED: costs 0.42 ms every frame rebuilding identically on an unmoved static scene.
    // Hash what buildAccelerationStructures() reads; if nothing moved, leave TLAS/rtInstanceData unchanged.
    // Saves more than refit (rtRefitAccel). MOVER PATCH LANE allows movable draws to be patched instead of rebuilt.
    bool rtSkipUnchangedTlas = true;

    // ---- in-place update (refit) for the ray-tracing acceleration structures ------------------
    // ON: tlas_ and compute-skinned mesh BLAS are created updatable and refit in place instead of fully rebuilt.
    // Periodic full rebuilds every kDynamicBlasRefitsPerRebuild / kTlasRefitsPerRebuild refits.
    bool rtRefitAccel = true;

    // ---- path tracing -----------------------------------------------------------------------
    // WHERE RAY TRACING ENDS AND PATH TRACING BEGINS: ray tracing = discrete rays, one hit, direct lighting.
    // Path tracing = multi-bounce solve. Separate settings because separately useful, priced, and supported.
    // 1 = NO extra bounces (one hit = ray tracing). Above 1 is path tracing.
    u32 ptBounces = 1;
    // How a path-traced frame is estimated (only while pathTracing is above Off):
    //   0 ReSTIR: each pixel's ReSTIR GI sample is a whole path, reused across pixels and frames and
    //     denoised. Clean in motion, slightly biased.
    //   1 Reference: one independent path per pixel per frame, no reuse and no denoiser, averaged while
    //     the view holds still. Converges to the ground truth; grainy while moving.
    u32 ptMode = 0;

    // ---- occlusion-aware fog: the air sky-visibility volume -----------------------------------
    // Fog adds in-scattered SKY light along the camera-to-surface path with no regard for what's between.
    // COST: one more compute pass, CSAirVis, over a FIXED 32^3 volume, independent of voxelResolution.
    // DEVICE-GATED: needs SM 6.0 and DXC (VoxiRenderer::airVisWanted()).
    bool fogOcclusion = true;
};

// The denoiser as one value: 0 off, 1 AMD FidelityFX, 2 NRD2. Setting 0 keeps the kind, so switching
// the denoiser back on returns to the one last chosen.
inline u32 denoiserMode(const Settings& s) { return s.denoiser ? (s.denoiserKind == 2u ? 2u : 1u) : 0u; }
inline void setDenoiserMode(Settings& s, u32 mode) {
    s.denoiser = mode != 0u;
    if (mode != 0u) s.denoiserKind = mode >= 2u ? 2u : 1u;
}

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
    // Returns whether this feature's refusal was already logged by setSettings' refuse() lambda.
    bool refusalLogged(Feature f) const { return (refusalLogged_ & (1u << static_cast<u32>(f))) != 0; }

    // Returns true once after settings.msaa changes, then clears the flag.
    bool consumeMsaaDirty();

    // ---- per-history reset requests: EditorConsole.hpp raises these, SandboxApp.cpp consumes and
    // forwards them to VoxiRenderer once a frame. resetaohistory is an ALIAS of resetrthistory.
    void requestGiHistoryReset()  { giHistoryResetRequested_ = true; }
    void requestRtHistoryReset()  { rtHistoryResetRequested_ = true; }
    void requestAoHistoryReset()  { aoHistoryResetRequested_ = true; }
    void requestDenoiserHistoryReset() { denoiserHistoryResetRequested_ = true; }
    // Each returns true once after its matching request*Reset() call, then clears itself.
    bool consumeGiHistoryResetRequest();
    bool consumeRtHistoryResetRequest();
    bool consumeAoHistoryResetRequest();
    bool consumeDenoiserHistoryResetRequest();

    // Returns a feature's display name.
    static const char* featureName(Feature f);
    // Returns a quality level's display name.
    static const char* qualityName(Quality q);
    // Voxel grid edge a GI quality tier resolves to.
    static u32 voxelResolutionForQuality(Quality q);
    // Total cones for the diffuse gather, including the axial one. See Settings::giCones.
    static u32 giConesForQuality(Quality q);
    // How much of F2/F3's cost each GI tier pays for. See Settings::giRestirVisibility.
    static u32 giRestirVisibilityForQuality(Quality q);
    static u32 refractionForQuality(Quality q);
    // Revoxelisation interval a GI quality tier resolves to.
    static u32 giUpdateIntervalForQuality(Quality q);
    // The RT sun-shadow rungs, mirroring giUpdateIntervalForQuality.
    static u32 rtShadowRaysForQuality(Quality q);
    // Sky-visibility rays per pixel for a ray-tracing tier. See Settings::giSkyOcclusionRays.
    static u32 giSkyOcclusionRaysForQuality(Quality q);
    // Sky-occlusion ray coherence tile for a ray-tracing tier; see Settings::giSkyOcclusionTile.
    static u32 giSkyOcclusionTileForQuality(Quality q);
    static u32 rtPixelsPerRayTileForQuality(Quality q);
    static u32 rtShadowDenoiseForQuality(Quality q);
    // 1 for every RT-capable tier except Low, which rasterises by explicit product decision (D3).
    static u32 rtRenderModeForQuality(Quality q);
    // Derived from the PATH TRACING tier, not the ray-tracing one. See Settings::ptBounces.
    static u32 ptBouncesForQuality(Quality q);

private:
    Renderer() = default;
    Settings settings_{};
    DeviceInfo device_{};
    bool msaaDirty_ = true;
    // One-shot request flags for the five reset* console commands. False by default.
    bool giHistoryResetRequested_  = false;
    bool rtHistoryResetRequested_  = false;
    bool aoHistoryResetRequested_  = false;
    bool denoiserHistoryResetRequested_ = false;
    u32 refusalLogged_ = 0;   // one bit per Feature: its refusal has already been logged
};

} // namespace aver::voxi
