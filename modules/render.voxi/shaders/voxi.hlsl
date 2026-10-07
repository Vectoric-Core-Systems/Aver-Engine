
// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
#define AVER_SHADOW_CASCADES 4

// Use AVER_CB_JOIN, not a literal register (rhi::kFeatureFrameConstantRegister is the sole definition).
cbuffer VoxiFrame : register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)) {
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    // One per cascade, tightest first: world space into that cascade's own [-1,1] clip box.
    float4x4 gCascadeViewProj[AVER_SHADOW_CASCADES];
    // x = the RADIUS at which this cascade stops applying, y = its normal-offset bias in world units
    float4   gCascadeSplit[AVER_SHADOW_CASCADES];
    float4   gShadowParams; // x = 1/atlasSize, y = enabled, z = accel structure built, w = cascades
    float4   gShadowDraw;   // x = the cascade the depth-only pass is filling right now
    // x = tan of the sun's ANGULAR RADIUS, which is what sets how fast a shadow edge softens;
    // y = occlusion rays per pixel; z = ray bias in world units at the near plane;
    // w = 1 once the flat geometry table is ready for a reflection ray's hit lookup.
    float4   gRtParams;
    // x = 1 while shadow hist is bound; y = history validity (0=new, 1=valid, 0.5=sun moved);
    // z = frame index; w = tile edge size as bit count (0=no tiling).
    float4   gRtHistParams;
    // Last frame's camera view-projection (for reprojection; valid only when gRtHistParams.y is set).
    float4x4 gPrevViewProj;
    // Last frame's scene viewport rect (x, y, w, h in pixels). Same validity as gPrevViewProj.
    float4   gSceneViewport;
    // This frame's scene viewport rect (x, y, w, h in pixels). Paired with gViewProj.
    float4   gSceneViewportCur;
    // x = 1 when eye is inside blended volume; y = medium's ior; z/w = local light count and history bits.
    float4   gCameraMedium;
    // Caustic caster's world box: min.xyz/max.xyz, min.w = enabled, max.w = strength.
    float4   gCausticMin;
    float4   gCausticMax;
    // GI-only shadow map's view-projection (box fitted to GI volume, not camera).
    float4x4 gGiShadowViewProj;
    // x = 1/kGiShadowSize, y = GI-only map enabled, z = normal-offset bias.
    // w = runtime bit-field: bits 1/2/4/8/16/32 control ray-driven optimization settings (see voxi_rt.hlsli).
    float4   gGiShadowParams;
    // Shadow denoiser: x = filter radius (0=off), y = blend weight, z/w unused.
    float4   gRtDenoiseParams;
    // x = Path Tracing's path vertices after the primary hit (voxi_pt.hlsli); y/z/w unused.
    float4   gPtBounceParams;
    // x = total cones for diffuse gather; y/z/w = refraction mode, strength, edge fade.
    float4   gGiParams;
    // x = sky-visibility rays the ambient term traces per pixel; 0 = use the cone gather's own occlusion
    // instead (every tier below the top; the pre-existing behaviour).
    // y = COHERENCE TILE EDGE those rays share a direction across (1 = per-pixel). Claimed for another
    // use within days of being marked spare -- the fate gGiParams above warns about.
    // z = LIGHTING LEGACY BITS (VoxiRenderer::setLightingLegacyBits, u32 as float; 0 = everything
    // corrected). Each bit restores one pre-fix behaviour for A/B only; decoded inline at each reader as
    // `((uint)gAmbientParams.z & <bit>u) != 0u`, never via a shared helper, so every reader stays
    // grep-able:
    //   bit 1  (R0) ReSTIR candidate/sky-occlusion ray samples a fixed 45-deg ring instead of a cosine
    //          hemisphere (voxi_restir.hlsli, voxi_rt.hlsli).
    //   bit 2  (R1) a ReSTIR receiver's own sky is double-counted (traced miss + ind.ambient). THIS FILE
    //          reads this bit for the sky-ownership subtraction in PSMainVoxi/PSRayDriven (F4).
    //   bit 4  (R2) a ReSTIR candidate hit reads sky with no visibility test or cosine weight
    //          (voxi_restir.hlsli).
    //   bit 8  (R3) a reused ReSTIR sample is shaded with no visibility test to the sample
    //          (voxi_restir.hlsli).
    //   bit 16 (R6) cone gather directions are cosine-weighted twice, i.e. cos^2 (voxi_cone.hlsli,
    //          voxi_gi.hlsli).
    //   bit 32 (W6) a blended fragment (glass/water) writes its own history again -- reservoir, surface
    //          history, denoiser GI input, RT shadow/AO/reflection history -- instead of leaving the opaque
    //          surface's alone (voxi_restir.hlsli, voxi_rt.hlsli, rtReflectionTemporal below).
    //          voxi.legacyBlendedHistoryWrite; --lighting-legacy 32.
    //
    // w carries GI-VISIBILITY (U1) and BLENDED-HISTORY (W6/M5) bits, decoded the same inline way, written
    // only by VoxiRenderer::beginShadowHistory (aver::voxi::givis::packAmbientW):
    //   bits 0-1 RestirVisibility mode (Settings::giRestirVisibility, 0..3 on the wire: No ray/Reconstructed/
    //          HalfRes/Full). Decoded once in giRestirIndirect into f2Path/f3Path; other readers use those.
    //   bit 4  half-res visibility pair (gGiVisHist/gGiVisHistOut, t16/u10) bound this frame; mode 2 with
    //          this clear means the pair failed to allocate (2.11) and behaves as Full, not a
    //          null-descriptor read.
    //   bit 8  gGiVisHist (t16) holds a real previous frame (sibling of gGiRestirParams.y).
    //   bit 16 (W6/M5) blended fragment takes the voxel-cone gather instead of ReSTIR for diffuse --
    //          voxi.blendedGiCone / --blended-gi cone (PSMainVoxi's M5). Clear = default, unchanged image.
    //   bit 32 backend replayed translucent draws blended THIS frame (D3D12 only -- Vulkan never marks a
    //          draw blended, VulkanDevice.cpp C10); gates averDrawIsTranslucent() meaning anything --
    //          without it every fragment behaves as opaque for history writes, correct on a backend with
    //          no blended replay pass to protect history from.
    //   bit 64 voxi.giVisPathView: paints giRestirIndirect's F2 path colour instead of shading, suppressed
    //          under the poison view (gGiRestirParams.w).
    //   bits 12-15 Settings::giRestirSpatialSamples (0..15): overrides the spatial-reuse tap count after
    //          the motion discount. 15 = AUTO (unchanged image), 0 = temporal only, 1..8 pin the count.
    //          See Settings::giRestirSpatialSamples (Voxi.hpp).
    //   bits 18-22 Settings::giRestirMaxHistory (0..31): the M cap on a reused reservoir; 0 (default)
    //          turns reuse off and resolves each pixel from its fresh candidate.
    //   bit 128 radiance cache live this frame (RestirVisibility::Cached). The CPU packs Cached as wire
    //          mode 2 (HalfResolution) PLUS this bit, because bits 0-1 cannot hold a value of 4. Only the
    //          four AVER_NEURAC twin compiles (staged CSRdGi/CSRdGiTrace, voxi_restir.hlsli) read it:
    //          giDecodePaths then sends untraced half-res pixels to f2Path 4 (cache read) and the traced
    //          pixels scatter into the cache. Every other compile ignores it, i.e. behaves as HalfResolution.
    //   bit POSITIONS 8-10 (values 256-1024) the NeuRaC visualiser's mode, 0 = off; position 11 (2048) its
    //          cell grid. Set only beside bit 128. The twin's giRestirIndirect returns rcDebugColour
    //          (voxi_neurac_io.hlsli) and Stage B shows it unshaded.
    float4   gAmbientParams;
    // x = debug mode; y = GI radiance ceiling; z = ReSTIR GI is chosen estimator; w = RD visibility row pitch.
    float4   gViewParams;
    // x = ReSTIR GI running; y = history valid; z = reservoir slice; w = poison debug view mode.
    float4   gGiRestirParams;
    // x = projected decals this frame (voxi_decal.hlsli; 0 skips every decal call); y = light-grid header record
    // in t18 (0 = none, voxi_rt.hlsli rdLightsAt); z, w = shadow rays per pixel / per hit for the light list.
    float4   gDecalParams;
};

// ---- Voxi: voxel cone traced GI ----
RWTexture3D<float4> gVoxelUAV : register(u0);
Texture3D<float4>   gVoxelTex : register(t0);
SamplerState        gVoxelSamp : register(s0);

// Injection accumulator (R32_UINT for atomic guarantees on D3D12).
RWTexture3D<uint> gVoxelAccum : register(u1);

// Fixed-point scale radiance is multiplied by before accumulation and divided by in CSResolve.
#define AVER_VOX_FIXED 16384.0
// Voxel/GI radiance ceiling (per-frame from gViewParams.y, falls back to 16.0 if 0).
#define AVER_VOX_MAXRAD (gViewParams.y > 0.0 ? gViewParams.y : 16.0)
// Single cone aperture (tan of half-angle): 0.577 = tan(30°) = 60° cone.
#define AVER_VOX_INJECT_APERTURE 0.577
// Re-emission gain for next bake (compensation constant for cone under-counting).
#define AVER_VOX_FEEDBACK 3.0

// Ceiling on albedo x AVER_VOX_FEEDBACK per channel (prevents runaway in iterative bakes).
#define AVER_VOX_MAX_BOUNCE_GAIN 0.8

// Tile edge size sharing one sky-occlusion ray direction (1=per-pixel, bit-identical to baseline).
#ifndef AVER_AO_COHERENCE_TILE
#define AVER_AO_COHERENCE_TILE 1.0
#endif


// Roughness threshold for mirror treatment (no cone/history/filter below this).
#define AVER_REFL_MIRROR_ROUGH 0.1

// gRtReflHist alpha: view depth scale in metres (cm * this).
#define AVER_REFL_HIST_DEPTH_SCALE 0.01

// TLAS instance-mask lanes. MUST MATCH kRtMask* in VoxiRenderer.cpp (silent image bug if mismatched).
#define AVER_RT_MASK_OPAQUE      0x01
#define AVER_RT_MASK_TRANSLUCENT 0x02
// Viewer's own body: opaque instance, excluded only from ray-driven primary (prevents first-person head).
#define AVER_RT_MASK_OWNER_HIDDEN 0x04
// Opaque geometry for secondary rays (includes viewer's body).
#define AVER_RT_MASK_OPAQUE_ALL  (AVER_RT_MASK_OPAQUE | AVER_RT_MASK_OWNER_HIDDEN)
#define AVER_RT_MASK_ALL         0xFF

// Directional shadow map. Core feature level 11_0, so it works on every DX12 GPU.
Texture2D<float>          gShadowTex  : register(t1);
SamplerComparisonState    gShadowSamp : register(s1);

// The GI-only shadow map. OUTSIDE the AVER_RT guard below on purpose: its only reader is PSVoxel,
// which is compiled without ray tracing, so a declaration inside the guard would vanish exactly
// where it is needed. Shares gShadowSamp -- same comparison state, different texture.
Texture2D<float>          gGiShadowTex : register(t8);
// Opaque scene before translucency (enables per-channel tinting). May be null; check averBlendBackdropValid().
Texture2D<float4>         gBlendBackdrop : register(t10);

// ---- Occlusion-aware fog: the air sky-visibility volume ----
// World-space volume answering "how much sky can the air see" per cell, covering the GI voxel volume.
// 32^3 resolution independent of gVoxelParams.x. If not yet built, treated as "no volume" and returns 1.0.
Texture3D<float>          gAirVis    : register(t17);
RWTexture3D<float>        gAirVisOut : register(u16);

// ---- CAUSTICS: light focused by the water surface onto what lies under it ----
// Pure arithmetic on the clock and box, no rays. Laplacian approximation of height field.
float averCausticFocus(float3 wpos) {
    if (gCausticMin.w < 0.5 || gCausticMax.w <= 0.0) return 0.0;
    // Inside the footprint, and below the surface. A point above the water gets nothing.
    if (wpos.x < gCausticMin.x || wpos.x > gCausticMax.x ||
        wpos.y < gCausticMin.y || wpos.y > gCausticMax.y ||
        wpos.z > gCausticMax.z) return 0.0;

    float focus = averWaveFocus(wpos.xy);

    // Sharpened: real caustics are thin bright lines, not a broad glow. The power turns a smooth
    // curvature field into the filigree the eye recognises.
    focus = pow(focus, 4.0);

    // Deeper water spreads the focus out and dims it. 200 cm is a soft falloff, not a physical depth.
    const float depth = max(gCausticMax.z - wpos.z, 0.0);
    focus *= exp(-depth / 200.0);

    // Fade out at the box edge so the pattern doesn't end on a hard line.
    const float2 edge = min(wpos.xy - gCausticMin.xy, gCausticMax.xy - wpos.xy);
    focus *= saturate(min(edge.x, edge.y) / 20.0);

    return focus * gCausticMax.w;
}

// ---- W6/M5: THE PER-FRAGMENT HISTORY-WRITE DISCRIMINATOR (D3: on by default, no tier drop) ----
// Declared unconditionally (PSMainVoxi is not guarded by AVER_RT). Static storage, set once per entry point.
static bool gAverHistoryWrite = true;

// ---- SUBSURFACE: where the primary sun-shadow rays start (averSubsurfaceShadowPush) ----
// Added to ray origin in rtShadowEx only (temporal wrapper operates at real surface). Reset after each call.
static float3 gAverShadowOriginPush = float3(0.0, 0.0, 0.0);
// THE LIGHT THE SHADOW KERNELS TRACE TOWARD (docs/rendering/UNIFIED_LIGHTS.md). rtShadowEx, rtShadowOpaque and
// rtShadowTemporalEx serve every light: a light's disc (tan of its angular radius), how far a ray may go before it
// reaches the light, and the key its history is stored under. Defaults: a directional light (the sun's disc from
// gRtParams.x, unbounded rays, key 0). rdSetShadowLight sets them for one list entry.
static float gAverShadowTanR    = -1.0;      // < 0: gRtParams.x
static float gAverShadowTMax    = 100000.0;
static float gAverShadowLightId = 0.0;

// ---- OBJECT MOTION: this pixel's surface point, last frame's position minus this frame's ----
// History reprojection projects wpos + this through last frame's camera, so an animated object (or the
// viewmodel) keeps its own history. Rigid instance motion only; 0 for static geometry and in raster.
static float3 gAverReprojDelta = float3(0.0, 0.0, 0.0);
// n turns of the golden angle, mod 2 pi, in integer fixed point: `n * 2.39996323` in float drifts once
// the frame count is large.
float averGoldenTurns(uint n) { return (float)((n * 0x9E3779B9u) >> 8) * (6.2831853 / 16777216.0); }

// Is this fragment a translucent (glass/water) draw, replayed blended?
bool averDrawIsTranslucent() { return (gMaterialFlags & AVER_MAT_ALPHA_BLEND) != 0u || gTransmission > 0.0; }

// The blended (glass) PSMainVoxi variant compiles without decals (VoxiRenderer's blended pipeline).
#ifndef AVER_BLENDED_PASS
#define AVER_BLENDED_PASS 0
#endif
#if !AVER_RT
#include "voxi_decal.hlsli"   // the RT build gets it through voxi_rt.hlsli, ahead of rtHitSurface
#endif
#if AVER_RT
#include "voxi_rt.hlsli"

#include "voxi_restir.hlsli"

// ---- STAGED RAY-DRIVEN PASSES (milestone 1): the visibility record and resolved sun visibility ----
// Settings::rayDrivenStages (0/1/2) splits PSRayDriven: CSRdVisibility traces, CSRdShadow resolves shadow.
// 0 (default) disables the split (AVER_RD_SPLIT=0 is unchanged). Milestone 2 adds CSRdGi checkerboard.
RWStructuredBuffer<uint4> gRdVisBuf    : register(u11);
// Path Tracing's progressive accumulation, one float4 per pixel at the same row pitch: rgb = running mean,
// w = asfloat(half(view depth in m) << 16 | frame count). Read and written by Stage B only, while
// gPtBounceParams.y > 0.5 (VoxiRenderer::ensurePtAccum sizes it; a one-element placeholder otherwise).
RWStructuredBuffer<float4> gPtAccum    : register(u22);
// This frame's resolved sun visibility: rgb=tinted transmittance, alpha=linear view depth (proof of surface).
RWTexture2D<float4>       gRdSunVisTex : register(u12);

// A: SUN SHADOW SPLIT (Settings::rayDrivenShadowTiles) -- CSRdShadowProbe's output: one uint per 8x8 tile
// of the full render target, classifying its shadow answer from one probe ray per pixel -- bit 1 = every
// probed pixel fully blocked, bit 2 = every one fully lit, bit 4 = disagreement (penumbra/tinted hit/mix),
// 0 = no surface in the tile. CSRdShadow's AVER_RD_SHADOW_TILES compile ORs a 3x3 tile neighbourhood of
// these and skips its ray loop only where that OR is EXACTLY 1 or 2 (see that compile's header for 3x3).
//
// u17/u18 (kVoxiUavCount 17->19, VoxiRenderer.cpp) -- gRdGiCand (voxi_restir.hlsli, B1) takes u17 first.
#ifndef AVER_RD_SHADOW_TILES
#define AVER_RD_SHADOW_TILES 0
#endif
RWStructuredBuffer<uint> gRdShadowTiles : register(u18);

// gRdGiTex / gRdAoTex -- MILESTONE 2's pair, splitting diffuse-GI and sky-occlusion the same way
// milestone 1 split shadow: one RGBA16F texel/pixel, each written by its own compute stage (CSRdGi /
// CSRdSkyOcc) and read once by PSRayDriven's AVER_RD_SPLIT branch. Like gRdSunVisTex, NEITHER is a
// history buffer -- the real state (gGiReservoirs/gGiSurfPosHist/gGiSurfNrmHist; gAoHist/gAoHitDistOut)
// is untouched; these just ferry one frame's answer to Stage B.
//
// u13/u14 (kVoxiUavCount 13->15, VoxiRenderer.cpp) -- next two free slots after gRdSunVisTex's u12.
//
// gRdGiTex: rgb = giRestirIndirect's diffuse radiance, alpha unused. Written by CSRdGi only when GI is
// actually ReSTIR this frame (gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5); cone-traced GI has no
// per-pixel history to split out and stays in Stage B unchanged.
RWTexture2D<float4>       gRdGiTex     : register(u13);
// gRdAoTex: r = rtSkyOcclusionTemporal's occlusion, g/b/a unused (a=1 always, avoiding a meaningless
// alpha). Written by CSRdSkyOcc only when PSRayDriven's rdAo is still its initial 1.0 (ReSTIR supplies
// diffuse, or there's no voxel GI) -- cone-GI mode rides the cone gather's own accumulator instead. See
// CSRdSkyOcc's header for the exact gate.
RWTexture2D<float4>       gRdAoTex     : register(u14);

// gRdReflTex -- MILESTONE 3: splits PSRayDriven's ray-traced REFLECTION answer the same way, one
// RGBA16F texel/pixel, written by CSRdRefl and read by PSRayDriven's AVER_RD_SPLIT branch. NOT a history
// buffer -- gRtReflHist/gRtReflHistOut (t7/u3, rtReflectionTemporal) own the real history; this only
// ferries one frame's answer to Stage B.
//
// u15 (kVoxiUavCount 15->16, VoxiRenderer.cpp) -- next free slot after gRdAoTex's u14.
//
// rgb = the clamped specular colour (mirror/glossy hit, atmosphere-blended sky weight at a rough
// surface, or plain sky -- see CSRdRefl's body for which).
//
// alpha is the STAGE'S OWN DECISION, not spare: 1.0 where CSRdRefl actually traced (its copy of
// PSMainVoxi's `gShadowParams.z > 0.5 && gRtParams.w > 0.5 && rough <= 0.75` gate), 2.0 if that traced
// value also hit the radiance ceiling (poison view), 0.0 otherwise (roughness routed to the cone/sky
// fallback, or no surface). Stage B reads this alpha rather than recomputing the roughness test (see
// PSRayDriven's AVER_RD_SPLIT branch, rdSurfaceRoughness's header).
//
// Under AVER_RD_REFL_SPLIT (Settings::rayDrivenReflSplit) only: alpha < -0.5 is PENDING -- CSRdRefl's R1
// compile defers rtReflectionSpatial's gather to CSRdReflFilter's R2 pass; rgb carries skyR meanwhile.
// CSRdReflFilter overwrites every PENDING texel before Stage B reads this texture (see both functions'
// own comments for the full two-pass contract).
RWTexture2D<float4>       gRdReflTex   : register(u15);

// ---- LOCAL LIGHTS (lamps): the light list, their visibility history, and the two halves that light ----
// gRdLocalLights (t18, the light list) is declared in voxi_rt.hlsli; gRdLocalHist/gRdLocalOut are
// ping-ponged with sun shadow history.
Texture2D<float4>              gRdLocalHist   : register(t19);
RWTexture2D<float4>            gRdLocalOut    : register(u19);

// AVER_RD_LAMPS (voxi_rt.hlsli): every compile but a single-pass one with AVER_RD_SINGLE_PASS_LAMPS 0.
#if AVER_RD_LAMPS
// One neighbour of rdLocalVisFiltered's 5x5. Weight falls linearly to zero at the reprojection depth
// test's tolerance (3% of depth + 1 cm, rtReprojectTexel), so a tap across a silhouette contributes
// nothing; a sky tap (alpha <= 0) is skipped outright. Clamped into THIS frame's viewport -- texels
// outside it may hold stale, unwritten values.
void rdLocalVisTap(int2 p, int2 lo, int2 hi, float zc, inout float sum, inout float wsum) {
    const uint2 q  = uint2(clamp(p, lo, hi));
    const float zt = gRdSunVisTex[q].a;
    const float w  = (zt > 0.0) ? saturate(1.0 - abs(zt - zc) / (zc * 0.03 + 1.0)) : 0.0;
    // Skipped, not multiplied by a zero weight: a rejected neighbour can hold any value, and one that
    // is not finite would turn `* 0` into NaN. Negative: not traced this frame (NRD2 half rate).
    const float v = gRdLocalOut[q].a;
    if (w > 0.0 && v >= 0.0) {
        sum  += v * w;
        wsum += w;
    }
}

// Staged read of lamp visibility: 5x5 around pixel, weighted by view-depth similarity. Returns 1 if no surface.
float rdLocalVisFiltered(uint2 pixel) {
    const float zc = gRdSunVisTex[pixel].a;
    if (zc <= 0.0) return 1.0;
    const int2 lo = int2(gSceneViewportCur.xy);
    const int2 hi = lo + max(int2(gSceneViewportCur.zw), int2(1, 1)) - 1;
    const int2 c  = int2(pixel);
    const float vc = gRdLocalOut[pixel].a;   // < 0: skipped by NRD2's half rate, the taps fill it
    float sum  = max(vc, 0.0);
    float wsum = vc >= 0.0 ? 1.0 : 0.0;
    [unroll] for (int oy = -2; oy <= 2; ++oy) {
        [unroll] for (int ox = -2; ox <= 2; ++ox) {
            if (ox != 0 || oy != 0) rdLocalVisTap(c + int2(ox, oy), lo, hi, zc, sum, wsum);
        }
    }
    return wsum > 0.0 ? sum / wsum : 1.0;
}

// ---- lamp HISTORY reads, at the continuous reprojected position ----
// Against last frame's stored depth (validated against previous viewport bounds).
void rdLocalHistBounds(out int2 lo, out int2 hi) {
    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);
    lo = max(int2(gSceneViewport.xy), int2(0, 0));
    hi = min(int2(gSceneViewport.xy) + max(int2(gSceneViewport.zw), int2(1, 1)) - 1,
             int2((int)texW, (int)texH) - 1);
}

// One history tap, weighted by `area` (its share of the caller's footprint) times depth agreement with zc
// (same shape/tolerance as rdLocalVisTap). Skipped, not multiplied by zero: see rdLocalVisTap.
void rdLocalHistTap(int2 q, int2 lo, int2 hi, float zc, float area, inout float sum, inout float wsum) {
    if (area <= 0.0 || any(q < lo) || any(q > hi)) return;
    const float zt = gRtShadowHist.Load(int3(q, 0)).y;
    const float w  = (zt > 0.0) ? area * saturate(1.0 - abs(zt - zc) / (zc * 0.03 + 1.0)) : 0.0;
    const float v  = gRdLocalHist.Load(int3(q, 0)).a;   // < 0: a texel the quarter-res tail pass left untraced
    if (w > 0.0 && v >= 0.0) {
        sum  += v * w;
        wsum += w;
    }
}

// The value CARRIED forward (prevVisC): bilinear at pxPrev over depth-agreeing taps. `texel` is
// rtReprojectTexel's validated one, the fallback if no tap agrees.
float rdLocalHistBilinear(float2 pxPrev, int2 texel) {
    int2 lo, hi;
    rdLocalHistBounds(lo, hi);
    const float  zc = gRtShadowHist.Load(int3(texel, 0)).y;
    const float2 f  = pxPrev - 0.5;
    const int2   q0 = int2(floor(f));
    const float2 t  = f - float2(q0);
    float sum = 0.0, wsum = 0.0;
    rdLocalHistTap(q0,              lo, hi, zc, (1.0 - t.x) * (1.0 - t.y), sum, wsum);
    rdLocalHistTap(q0 + int2(1, 0), lo, hi, zc, t.x * (1.0 - t.y),         sum, wsum);
    rdLocalHistTap(q0 + int2(0, 1), lo, hi, zc, (1.0 - t.x) * t.y,         sum, wsum);
    rdLocalHistTap(q0 + int2(1, 1), lo, hi, zc, t.x * t.y,                 sum, wsum);
    return wsum > 0.0 ? sum / wsum : gRdLocalHist.Load(int3(texel, 0)).a;   // may be -1: no history
}

// History read blended by rdLocalLightsVisibility: depth-weighted box centred on pxPrev, area-weighted taps.
float rdLocalHistFiltered(float2 pxPrev, int2 texel) {
    int2 lo, hi;
    rdLocalHistBounds(lo, hi);
    const float  zc = gRtShadowHist.Load(int3(texel, 0)).y;
    const float2 a  = pxPrev - 1.5, b = pxPrev + 1.5;
    const int2   q0 = int2(floor(a));
    float sum = 0.0, wsum = 0.0;
    [unroll] for (int oy = 0; oy < 4; ++oy) {
        [unroll] for (int ox = 0; ox < 4; ++ox) {
            const int2   q  = q0 + int2(ox, oy);
            const float2 ov = saturate(min(float2(q) + 1.0, b) - max(float2(q), a));
            rdLocalHistTap(q, lo, hi, zc, ov.x * ov.y, sum, wsum);
        }
    }
    return wsum > 0.0 ? sum / wsum : gRdLocalHist.Load(int3(texel, 0)).a;   // may be -1: no history
}

// ---- THE VISIBILITY HALF: one shadow ray for every lamp, accumulated the way the sun's is ----
//
// At one surface point: summed diffuse irradiance of every light in range (rdLocalIrradiance,
// voxi_rt.hlsli), ONE light picked in proportion to its luminance share, ONE shadow ray toward a jittered
// point on it (rdLocalShadow), accumulated over frames as a 0/1 answer. Returns accumulated visibility (1
// where no light reaches, so a caller that shades anyway takes nothing away); stores to gRdLocalOut[pixel]
// when writeHistory && gAverHistoryWrite allow (staged, also feeds Stage B's filter). wpos/N: the surface
// and its viewer-facing normal; pixelC: pixel centre (velocityPx measured from it, as rtShadowTemporal's is).
//
// ONE VISIBILITY STANDS FOR EVERY LIGHT -- sound in expectation (picking i w.p. w_i/wsum gives expected
// luminance sum_i E_i v_i, one ray regardless of lamp count). Approximates COLOUR only: a spot shadowed by
// one lamp but lit by a differently-coloured one comes out as a dimmed mix of both, not the second
// lamp's own colour (the first lamp's highlight stays, dimmed, too).
//
// HISTORY: uses the sun's own reprojection/depth test (rtReprojectTexel, its arithmetic twin -- see its
// header for why a twin) against the sun history's depth (a property of the surface, not the light);
// gRdLocalHist ping-pongs with gRtShadowHist on the same index. That one texel is noisy alone --
// rdLocalHistFiltered widens it to a depth-weighted 3x3 first.
//
// DERIVATIVES: rtReprojectTexel takes ddx/ddy of depth, so it must run first, behind only a
// constant-buffer condition -- the raster pixel shaders (its only callers; the ray-driven path lights through
// CSRdShadow's list kernel) call it behind rdLocalLampsLive().
float rdLocalLightsVisibility(float3 wpos, float3 N, float2 pixelC, uint2 pixel, bool writeHistory) {
    // gRtHistParams.x: t6/u2 are bound this frame. gRtHistParams.y > 0.25, not the shadow's > 0.75: t6
    // holds a real previous frame, and its DEPTH stays valid on a frame only the sun moved (the
    // sun-independent test sky occlusion uses) -- the lamps did not move. rdLocalHistValid(): t19 holds
    // the same light set.
    int2   texel      = int2(0, 0);
    float2 velocityPx = float2(0.0, 0.0);
    bool   haveHist   = false;
    if (gRtHistParams.x > 0.5 && gRtHistParams.y > 0.25 && rdLocalHistValid())
        haveHist = rtReprojectTexel(wpos, pixelC, texel, velocityPx);
    // prevVisC: the reprojected texel alone -- what gets STORED, so the spatial filter below is folded in
    // once per turn rather than compounding on every carried frame. prevVisF: rdLocalHistFiltered's
    // depth-weighted average around that same texel -- what gets RETURNED to shading and what this
    // pixel's turn blends its fresh sample into.
    float prevVisC = 1.0;
    float prevVisF = 1.0;
    if (haveHist) {
        // Where this surface point sat last frame, unsnapped (rtReprojectTexel: velocityPx = px - pixelC).
        const float2 pxPrev = pixelC + velocityPx;
        prevVisC = rdLocalHistBilinear(pxPrev, texel);
        prevVisF = rdLocalHistFiltered(pxPrev, texel);
    }

    // HALF RATE: a pixel with usable history traces on alternate frames (checkerboard swapping every
    // frame), carrying STORED visibility (prevVisC) forward untouched on the other -- no ray, no filter
    // tap in the compute stage. Raster/single-pass PSRayDriven still shade the off turn with prevVisF
    // (rdLocalHistFiltered's 3x3; see HISTORY READ above for why a bare texel isn't enough). MEASURED on
    // NewSponza's 22 lamps: tracing every pixel every frame cost ~1 ms (less coherent than the sun's rays).
    // A pixel WITHOUT history always traces, so disocclusion never waits a frame.
    //
    // THE PIXEL'S OWN TURN COUNT drives its sequences, not the frame index: its turns fall on frames of
    // ONE parity, so rtRadicalInverse2(frameIdx + 1) would see only odd (always >= 0.5) or only even
    // (always < 0.5) arguments -- each pixel's pick confined to half the lights' weight (a lamp that
    // shadows it never picked, the neighbour that does pick it shadowed every turn): a fixed per-pixel
    // speckle no accumulation removes. frameIdx >> 1 counts this pixel's turns one by one.
    const uint frameIdx = (uint)gRtHistParams.z;
    const bool myTurn   = !haveHist || (((pixel.x + pixel.y + frameIdx) & 1u) == 0u);
    const uint turn     = haveHist ? (frameIdx >> 1) : frameIdx;
    float vis     = prevVisF;
    float histVis = prevVisC;
    if (myTurn) {
        // LOOP 1: unshadowed sum, each light's luminance as its pick weight. Recomputed in loop 2 rather
        // than cached in a 32-entry array: loop-indexed arrays spill registers (rtShadowEx measured a 25%
        // regression), and a light's irradiance is a few ALU ops.
        const uint n = min(rdLocalLightCount(), AVER_LIGHT_LIST_MAX);
        float wsum    = 0.0;
        uint  lastLit = 0u;
        [loop] for (uint i = 0u; i < n; ++i) {
            const float w = rdLocalPickWeight(gRdLocalLights[i], wpos, N);
            wsum += w;
            if (w > 0.0) lastLit = i;
        }

        // No light reaches this point (or a non-finite sum, which failing `> 0` also catches): nothing
        // to trace, visibility 1 for the shading (there is nothing to shade), and the accumulated
        // visibility carried forward so a lamp coming back into range this texel does not restart from
        // one ray.
        vis = 1.0;
        if (wsum > 0.0) {
            // LOOP 2: walk cumulative weights to the light u lands in. u = pixel hash (salted apart from
            // rdLocalShadow's disc angle) rotated by the radical inverse of its turn -- spreads picks
            // evenly across turns rather than in runs. lastLit, not n-1: u*wsum can round up to wsum, and
            // the last light may contribute nothing here.
            // NRD2 frames have no visibility history, so Stage B's 5x5 filter is the whole estimate: the pick is
            // STRATIFIED there, each pixel of a 5x5 block taking its own 25th of the weight CDF (a bijection of
            // (x mod 5, y mod 5), so every 5x5 window holds each stratum once). The filter then averages an even
            // sample of the lamps rather than 25 random ones, the noise that showed as spots only where lamps
            // light (the sun is one light). docs/rendering/NRD2.md "Stratified lamp picks". Compiled only into the
            // bindless staged passes (where NRD2 frames compute lamp visibility): in the non-bindless ray-traced
            // raster variants and the glass variant it hung the RX 7800 XT, as decals did (DECALS.md).
#if defined(AVER_RT_BINDLESS) && !AVER_BLENDED_PASS && !AVER_RD_SINGLE_PASS
            const uint  sa     = pixel.x % 5u, sb = pixel.y % 5u;
            const float uPix   = rtNrd2Frame() ? ((float)(5u * ((sa + 2u * sb) % 5u) + (2u * sa + sb) % 5u) + 0.5) / 25.0
                                               : rtHash(pixelC + float2(0.37, 11.0));
#else
            const float uPix   = rtHash(pixelC + float2(0.37, 11.0));
#endif
            const float u      = frac(uPix + rtRadicalInverse2(turn + 1u));
            const float target = u * wsum;
            uint  pick = lastLit;
            float acc  = 0.0;
            [loop] for (uint j = 0u; j < n; ++j) {
                acc += rdLocalPickWeight(gRdLocalLights[j], wpos, N);
                if (target < acc) { pick = j; break; }
            }

            // The sun's golden-angle jitter (rtShadowTemporalEx's untiled branch), stepped per TURN so the
            // disc sample advances one golden angle each time this pixel traces.
            const float frameJitter = averGoldenTurns(turn);
            const float v = rdLocalShadow(wpos, N, gRdLocalLights[pick], pixelC, frameJitter,
                                          rdLocalRectSample(pixelC, turn));

            // Exponential accumulation: 0.95 history at rest (~20 turns), rising only to 0.2 fresh by 32
            // px/frame (the sun's own measured budget, rtShadowTemporalEx). Was 0.5 by 8 px/frame: an
            // ordinary pan then averaged ~2 frames of a 0/1 ray and the filters smeared that into boiling
            // blotches. MEASURED on NewSponza_Night (moving vs settled MAD, fixed exposure): pan 6.43 ->
            // 3.48, fast pan 7.02 -> 5.00. A static lamp's shadow does not change when the CAMERA moves --
            // reprojection carries it, and the depth test already drops history on a changed surface.
            //
            // Blended into prevVisF, not bare prevVisC: an EMA fed a Bernoulli 0/1 trace never settles
            // (steady-state stddev sqrt(alpha/(2-alpha)*p(1-p)), ~11% at a half-lit penumbra with alpha
            // 0.1, 8% with 0.05); folding rdLocalHistFiltered's ~9 neighbours in divides that variance by
            // ~9, at the cost of a penumbra widened a pixel or two. MEASURED on NewSponza's lantern vault
            // at dusk (high-pass RMS, AverSR on): 11.0 with neither filter, 3.7 with this filter alone,
            // 2.0 with all three -- matching the scene's own 2.0 with lamps off.
            vis = v;
            if (haveHist) {
                const float alpha = lerp(0.05, 0.2, saturate(length(velocityPx) / 32.0));
                vis = lerp(prevVisF, v, alpha);
            }
            histVis = vis;   // carried forward as next frame's prevVisC, to fold into rdLocalHistFiltered's 3x3 again
        }
    }
    // Only .a is ever read back (rdLocalVisFiltered this frame; prevVisC and prevVisF, through
    // rdLocalHistFiltered, next frame); the shading re-evaluates each lamp itself, so the irradiance sum
    // is weights for the pick and nothing more.
    if (writeHistory && gAverHistoryWrite) gRdLocalOut[pixel] = float4(0.0, 0.0, 0.0, histVis);
    return vis;
}

// ---- THE SHADING HALF: every lamp through the sun's own BRDF ----
//
// One lamp as the BRDF sees it from wpos: an AverLight toward its centre with rdLocalIrradiance's falloff
// WITHOUT N.L (averDirectTerms applies that), visibility 1 (caller scales by the shared one after), and
// `s` RE-AIMED at it (s.H/s.F were built for the SUN; unaimed, a highlight from them would sit where the
// sun's does). False past range, tested before any sqrt/BRDF work so an out-of-range lamp costs one dot
// product -- `l`/`sL` then hold nothing to shade.
//
// THE SPHERE WIDENS THE LOBE: roughness + r/(2d) from the sphere's angular size, clamped to [rough, 1], so
// a large close bulb spreads its highlight instead of a point light's pinpoint.
bool rdLocalLightAt(RdLocalLight ll, AverSurface s, float3 wpos, out AverLight l, out AverSurface sL) {
    sL = s;
    l.direction  = float3(0.0, 0.0, 1.0);
    l.radiance   = float3(0.0, 0.0, 0.0);
    l.visibility = float3(1.0, 1.0, 1.0);
    float r;
    // False past range (also the zero-range guard, as in rdLocalIrradiance) or off a rectangle's lit side.
    if (!aversLightEval(ll, wpos, s.N, l.direction, l.radiance, r)) return false;
    const float invD = rsqrt(max(dot(ll.posRadius.xyz - wpos, ll.posRadius.xyz - wpos), 1e-8));
    sL.rough = clamp(s.rough + r * 0.5 * invD, s.rough, 1.0);
    sL.H     = normalize(s.V + l.direction);
    sL.F     = fresnelSchlick(saturate(dot(sL.H, s.V)), s.F0, s.f90);
    return true;
}

// Every lamp in range through averShadeDirect (same Cook-Torrance GGX/multiscatter/subsurface terms as
// the sun) times the ONE visibility rdLocalLightsVisibility resolved for this point (see its header for
// why one stands for all). Diffuse+specular radiance, for the caller to add beside the sun's.
float3 rdLocalLightsShade(AverSurface s, float3 wpos, float vis) {
    float3 acc = float3(0.0, 0.0, 0.0);
    float3 accUnshadowed = float3(0.0, 0.0, 0.0);   // lights flagged no-shadow skip the shared visibility
    const uint n = min(rdLocalLightCount(), AVER_LIGHT_LIST_MAX);
    [loop] for (uint i = 0u; i < n; ++i) {
        AverLight   l;
        AverSurface sL;
        // UNIFIED_LIGHTS phase 1: the visible surface still lights the directional entry its old way.
        if (aversLightKind(gRdLocalLights[i]) == AVER_LIGHT_DIRECTIONAL) continue;
        if (!rdLocalLightAt(gRdLocalLights[i], s, wpos, l, sL)) continue;
        if (aversLightNoShadow(gRdLocalLights[i])) accUnshadowed = averShadeDirect(accUnshadowed, sL, l);
        else                                       acc = averShadeDirect(acc, sL, l);
    }
    return acc * vis + accUnshadowed;
}

// Same lamps for a BLENDED surface, split into averShadeSplit's two buckets instead of summed: specular
// full-strength (premultiplied blend), diffuse/subsurface weighted by coverage -- how averShadeSplit
// apportions the sun's direct term too, so a pane composites lamps like it composites the sun. Nothing
// for unlit surfaces, matching averShadeSplit's own direct half.
void rdLocalLightsShadeSplit(AverSurface s, float3 wpos, float vis,
                             inout float3 diffuse, inout float3 specular) {
    if (s.model == AVER_MODEL_UNLIT) return;
    float3 dAcc = float3(0.0, 0.0, 0.0);
    float3 sAcc = float3(0.0, 0.0, 0.0);
    float3 dAccU = float3(0.0, 0.0, 0.0);
    float3 sAccU = float3(0.0, 0.0, 0.0);
    const uint n = min(rdLocalLightCount(), AVER_LIGHT_LIST_MAX);
    [loop] for (uint i = 0u; i < n; ++i) {
        if (aversLightKind(gRdLocalLights[i]) == AVER_LIGHT_DIRECTIONAL) continue;   // phase 1, as above
        AverLight   l;
        AverSurface sL;
        if (!rdLocalLightAt(gRdLocalLights[i], s, wpos, l, sL)) continue;
        float3 dDiffuse, dSpecular, dSubsurface;
        float  ndl;
        averDirectTerms(sL, l, dDiffuse, dSpecular, dSubsurface, ndl);
        const float3 lightTerm = l.radiance * ndl;
        if (aversLightNoShadow(gRdLocalLights[i])) {
            dAccU += dDiffuse * lightTerm + dSubsurface * l.radiance;
            sAccU += dSpecular * lightTerm;
        } else {
            dAcc += dDiffuse * lightTerm + dSubsurface * l.radiance;
            sAcc += dSpecular * lightTerm;
        }
    }
    diffuse  += dAcc * vis + dAccU;
    specular += sAcc * vis + sAccU;
}

// One list entry on the visible surface with the visibility CSRdShadow resolved for it (UNIFIED_LIGHTS.md):
// re-aimed through rdLocalLightAt (a directional entry keeps its roughness), caustic focus for a directional light.
float3 rdListLightVis(uint i, float3 wpos, float3 vis) {
    return aversLightKind(gRdLocalLights[i]) == AVER_LIGHT_DIRECTIONAL ? vis * (1.0 + averCausticFocus(wpos)) : vis;
}
float3 rdListLight(uint i, AverSurface s, float3 wpos, float3 vis) {
    AverLight   l;
    AverSurface sL;
    if (!rdLocalLightAt(gRdLocalLights[i], s, wpos, l, sL)) return float3(0.0, 0.0, 0.0);
    l.visibility = rdListLightVis(i, wpos, vis);
    return averShadeDirect(float3(0.0, 0.0, 0.0), sL, l);
}
void rdListLightSplit(uint i, AverSurface s, float3 wpos, float3 vis, inout float3 diffuse, inout float3 specular) {
    if (s.model == AVER_MODEL_UNLIT) return;
    AverLight   l;
    AverSurface sL;
    if (!rdLocalLightAt(gRdLocalLights[i], s, wpos, l, sL)) return;
    l.visibility = rdListLightVis(i, wpos, vis);
    float3 dD, dS, dSss;
    float  ndl;
    averDirectTerms(sL, l, dD, dS, dSss, ndl);
    const float3 lt = l.radiance * ndl * l.visibility;
    diffuse  += dD * lt + dSss * l.radiance * l.visibility;
    specular += dS * lt;
}

// ---- THE TAIL: every light but the exact one, lit the way the sun is (UNIFIED_LIGHTS.md "Tail") ----
// Shading is exact: every tail light through the BRDF. The strongest tail lights (the ray budget) get their own
// shadow ray every frame, as the sun does; their answers are stored as one irradiance-weighted unblocked fraction
// (one texel per pixel) that the rest share. No light is picked at random. Colour is approximate where tail lights
// of different colours are blocked differently.

// The tail's light weight at a point: the exact light and no-shadow lights excluded (those need no ray).
float rdTailWeight(uint j, uint e0, float3 wpos, float3 N) {
    if (j == e0 || aversLightNoShadow(gRdLocalLights[j])) return 0.0;
    return rdLightWeight(gRdLocalLights[j], wpos, N);
}

// Stage B: every tail light shaded exactly, the shadowable ones times the tail fraction `vis`.
// A tail light under 1/128 of the exact light's irradiance is dropped; on a rough, non-metal, non-subsurface surface
// the rest are shaded diffuse-only (the GGX lobe of a dim light is a few percent).
float rdTailFloor(uint e0, float3 wpos, float3 N) {
    return averShadowLum(aversLightIrradiance(gRdLocalLights[e0], wpos, N)) * (1.0 / 128.0);
}
bool rdTailCheap(AverSurface s) {
    return s.rough >= 0.5 && max(s.F0.x, max(s.F0.y, s.F0.z)) <= 0.08 && s.sssWeight <= 0.0;
}
float3 rdTailLights(AverSurface s, float3 wpos, uint e0, float vis) {
    float3 acc = float3(0.0, 0.0, 0.0);
    if (s.model == AVER_MODEL_UNLIT) return acc;
    const RdLightRange lr = rdLightsAt(wpos);
    const float floorW = rdTailFloor(e0, wpos, s.N);
    const bool  cheap  = rdTailCheap(s);
    [loop] for (uint k = 0u; k < lr.count; ++k) {
        const uint j = rdLightIndex(lr, k);
        if (j == e0) continue;
        AverLight   l;
        AverSurface sL;
        if (cheap) {
            float r;
            if (!aversLightEval(gRdLocalLights[j], wpos, s.N, l.direction, l.radiance, r)) continue;
            const float ndl = saturate(dot(s.N, l.direction));
            if (averShadowLum(l.radiance) * ndl <= floorW) continue;
            const float3 v = rdListLightVis(j, wpos, (aversLightNoShadow(gRdLocalLights[j]) ? 1.0 : vis).xxx);
            acc += s.kdAlbedo * (1.0 / PI) * l.radiance * v * ndl;
            continue;
        }
        if (!rdLocalLightAt(gRdLocalLights[j], s, wpos, l, sL)) continue;
        if (averShadowLum(l.radiance) * saturate(dot(s.N, l.direction)) <= floorW) continue;
        l.visibility = rdListLightVis(j, wpos, (aversLightNoShadow(gRdLocalLights[j]) ? 1.0 : vis).xxx);
        acc = averShadeDirect(acc, sL, l);
    }
    return acc;
}
void rdTailLightsSplit(AverSurface s, float3 wpos, uint e0, float vis, inout float3 diffuse, inout float3 specular) {
    if (s.model == AVER_MODEL_UNLIT) return;
    const RdLightRange lr = rdLightsAt(wpos);
    const float floorW = rdTailFloor(e0, wpos, s.N);
    const bool  cheap  = rdTailCheap(s);
    [loop] for (uint k = 0u; k < lr.count; ++k) {
        const uint j = rdLightIndex(lr, k);
        if (j == e0) continue;
        AverLight   l;
        AverSurface sL;
        if (cheap) {
            float r;
            if (!aversLightEval(gRdLocalLights[j], wpos, s.N, l.direction, l.radiance, r)) continue;
            const float ndl = saturate(dot(s.N, l.direction));
            if (averShadowLum(l.radiance) * ndl <= floorW) continue;
            const float3 v = rdListLightVis(j, wpos, (aversLightNoShadow(gRdLocalLights[j]) ? 1.0 : vis).xxx);
            diffuse += s.kdAlbedo * (1.0 / PI) * l.radiance * v * ndl;
            continue;
        }
        if (!rdLocalLightAt(gRdLocalLights[j], s, wpos, l, sL)) continue;
        if (averShadowLum(l.radiance) * saturate(dot(s.N, l.direction)) <= floorW) continue;
        l.visibility = rdListLightVis(j, wpos, (aversLightNoShadow(gRdLocalLights[j]) ? 1.0 : vis).xxx);
        float3 dD, dS, dSss;
        float  ndl;
        averDirectTerms(sL, l, dD, dS, dSss, ndl);
        const float3 lt = l.radiance * ndl * l.visibility;
        diffuse  += dD * lt + dSss * l.radiance * l.visibility;
        specular += dS * lt;
    }
}
#endif

// gViewParams.w carries the staged buffers' row pitch as an exact integer (see gRdVisBuf's header), PLUS
// (milestone 4+) flag bits above it: bit 16 = CSRdGi's half-rate-GI checkerboard parity
// (AVER_GI_CHECKERBOARD, voxi_restir.hlsli), set only for that dispatch's own upload and restored to plain
// pitch right after (recordStagedRayDriven) -- every OTHER decode must mask flag bits away. After the
// staged passes, on a frame CSRdGi traced GI at half rate, the field holds bit 17 (+ that parity) and no
// pitch -- giRestirIndirect's non-checkerboard denoiser-input write reads it to write only the traced
// half. Declared here so
// it's in scope before every stage that decodes a pitch.
uint rdRowPitch() { return (uint)gViewParams.w & 0xFFFFu; }

// The surface PSRayDriven reconstructs from a ray hit, minus what the hit itself already computed (bary,
// dir, pre-flip N): what Stage S/Stage B's AVER_RD_SPLIT branch both need, nothing more -- dpx/dpy (the
// shadow-ray footprint) and L (the light direction) are cheap and hit-independent, so each caller computes
// those itself.
struct RdSurface {
    RtInstance inst;
    uint       i0, i1, i2;
    float3     w;
    float3     N;
    float3     Ng;   // the flat triangle's normal, facing the ray like N (rtReflection's origin/clamp)
    float2     hitUV;
    RtMaterial mat;
    float      hitT;
    float3     wpos;
    float3     reprojDelta;   // last frame's world position minus this frame's (gAverReprojDelta)
};

// Rebuilds a PSRayDriven-shaped surface from CSRdVisibility's record instead of a live RayQuery.
// TRANSCRIBED FROM PSRayDriven'S OWN POST-TRACE STATEMENTS (AVER_RD_SPLIT==0 branch, below) --
// inst/tri/i0/i1/i2/w/nObj/N/hitUV/mat copied verbatim. KEEP THE TWO IN STEP: no shared code, so a
// change to PSRayDriven's reconstruction not mirrored here silently diverges the staged path.
//
// `hitT`/`wpos` can't be copied verbatim (PSRayDriven's q.CommittedRayT() doesn't exist here): instead
// rebuilds world hit point P by the same barycentric interpolation, projects (P - camera) onto `dir` to
// recover hitT, then forms wpos the same way (gCamPos + dir * hitT) -- a caller holding wpos/hitT can't
// tell which path produced them.
RdSurface rdSurfaceFromRecord(uint4 rec, float3 dir) {
    RdSurface o;
    o.inst = rtLoadInstance(rec.x);
    const uint tri = o.inst.firstIndex + rec.y * 3;
    o.i0 = o.inst.firstVertex + gRtIndices[tri + 0];
    o.i1 = o.inst.firstVertex + gRtIndices[tri + 1];
    o.i2 = o.inst.firstVertex + gRtIndices[tri + 2];

    const float2 bary = float2(asfloat(rec.z), asfloat(rec.w));
    o.w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    const float3 nObj = normalize(gRtVerts[o.i0].nrm * o.w.x + gRtVerts[o.i1].nrm * o.w.y + gRtVerts[o.i2].nrm * o.w.z);
    o.N = normalize(mul(float4(nObj, 0.0), o.inst.objectToWorld).xyz);
    if (dot(o.N, dir) > 0.0) o.N = -o.N;   // face the ray, same flip PSRayDriven's own copy applies

    o.hitUV = gRtVerts[o.i0].uv * o.w.x + gRtVerts[o.i1].uv * o.w.y + gRtVerts[o.i2].uv * o.w.z;

    o.mat = gRtMaterials[o.inst.materialIndex];

    // World hit point P: same three vertices, transformed the way VSMain transforms i.pos, blended by
    // the same barycentric weights `w` (see this function's header for why hitT/wpos derive from P).
    const float3 p0 = mul(float4(gRtVerts[o.i0].pos, 1.0), o.inst.objectToWorld).xyz;
    const float3 p1 = mul(float4(gRtVerts[o.i1].pos, 1.0), o.inst.objectToWorld).xyz;
    const float3 p2 = mul(float4(gRtVerts[o.i2].pos, 1.0), o.inst.objectToWorld).xyz;
    const float3 P  = p0 * o.w.x + p1 * o.w.y + p2 * o.w.z;
    o.hitT = dot(P - gCamPos.xyz, dir);
    o.wpos = gCamPos.xyz + dir * o.hitT;
    // A degenerate (zero-area) triangle has no plane of its own; the shading normal stands in.
    const float3 ng = cross(p1 - p0, p2 - p0);
    o.Ng = dot(ng, ng) > 1e-12 ? normalize(ng) : o.N;
    if (dot(o.Ng, dir) > 0.0) o.Ng = -o.Ng;
    // Every caller reconstructs only its own pixel, so this pass's history reprojection follows it.
    const float3 objPos = gRtVerts[o.i0].pos * o.w.x + gRtVerts[o.i1].pos * o.w.y + gRtVerts[o.i2].pos * o.w.z;
    o.reprojDelta = mul(float4(objPos, 1.0), o.inst.prevObjectToWorld).xyz -
                    mul(float4(objPos, 1.0), o.inst.objectToWorld).xyz;
    gAverReprojDelta = o.reprojDelta;
    return o;
}

// How far the reflecting surface's view depth changes per pixel along the neighbour's primary ray
// `dirN`: that ray intersected with THIS pixel's triangle plane, not the ray itself scaled by hitT (which
// ignored the surface's slope, so rtReflectionSpatial's depth test rejected or kept neighbours by each
// facet's tilt). 0 when the neighbour ray runs parallel to the plane.
float rdPlaneDepthStep(float3 wpos, float3 Ng, float3 dirN) {
    const float denom = dot(dirN, Ng);
    if (abs(denom) < 1e-4) return 0.0;
    const float t = dot(wpos - gCamPos.xyz, Ng) / denom;
    const float3 Pn = gCamPos.xyz + dirN * t;
    return mul(float4(Pn, 1.0), gViewProj).w - mul(float4(wpos, 1.0), gViewProj).w;
}

// The surface at a staged pixel, through the one builder every ray hit uses (voxi_rt.hlsli's
// rtHitSurface): what CSRdRefl needs before tracing -- the roughness for its gate and the shading
// (normal-mapped) normal to reflect about -- is exactly what Stage B shades with.
AverSurface rdHitSurface(RdSurface s, float3 dir, float3 rdRayDx, float3 rdRayDy, uint detail) {
    RtHit h;
    h.inst   = s.inst;
    h.mat    = s.mat;
    h.i0 = s.i0; h.i1 = s.i1; h.i2 = s.i2;
    h.w      = s.w;
    h.pos    = s.wpos;
    h.N      = s.N;
    h.meshUV = s.hitUV;
    h.t      = s.hitT;
    float2 gx, gy;
    rtHitGrad(h, rdRayDx, rdRayDy, gx, gy);
    return rtHitSurface(h, -dir, normalize(gLightDir.xyz), gx, gy, detail);
}
// Is a real backdrop bound? Null-filled Texture2D reports zero dimensions.
bool averBlendBackdropValid(out float2 invSize) {
    uint w = 0, h = 0;
    gBlendBackdrop.GetDimensions(w, h);
    const bool ok = (w > 0u && h > 0u);
    invSize = ok ? (1.0 / float2((float)w, (float)h)) : float2(0.0, 0.0);
    return ok;
}

// Volume composite using backdrop as correction. Handles absorption and refraction.
float2 averRefractedBackdropUV(AverSurface s, float3 wpos, float thicknessCm,
                               float2 invSize, float2 screenPos, out bool tir) {
    tir = false;
    const float2 uv0  = screenPos * invSize;
    const uint   mode = (uint)(gGiParams.y + 0.5);
    if (mode == 0u || gGiParams.z <= 0.0) return uv0;

    // Light direction (eye inside or outside) decides eta.
    const float  ior      = max(gIor, 1.0001);
    const bool   eyeInside = gCameraMedium.x > 0.5;
    const float  eta      = eyeInside ? ior : (1.0 / ior);
    float3 R = refract(-s.V, s.N, eta);
    if (dot(R, R) < 1e-6) {
        // refract() returns 0 to say no transmitted ray exists.
        if (!eyeInside) return uv0;   // from outside, unreachable; sample straight through.
        // TIR past critical angle (at n=1.33: 48.75 degrees) the underside becomes a mirror.
        tir = true;
        R = reflect(-s.V, s.N);
    }

    // Target: one thickness along the BENT path, not straight.
    float3 target = wpos + R * max(thicknessCm, 0.0);

#if AVER_RT
    // Ray-traced: follow the bent ray to what it actually reaches.
    if (mode >= 2u) {
        RayDesc rr;
        rr.Origin = target;
        rr.Direction = R;
        rr.TMin = 0.0;
        rr.TMax = 100000.0;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        rq.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, rr);
        averRtProceedSolid(rq);   // cutouts, not cards.
        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            target = target + R * rq.CommittedRayT();
    }
#endif

    const float4 clip = mul(float4(target, 1.0), gViewProj);
    if (clip.w <= 1e-4) return uv0;               // behind eye.
    const float2 ndc = clip.xy / clip.w;

    // NDC to viewport rect (not [0,1] of whole target).
    if (gSceneViewportCur.z <= 0.0 || gSceneViewportCur.w <= 0.0) return uv0;
    const float2 pxR = gSceneViewportCur.xy +
                       float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewportCur.zw;
    float2 uvR = pxR * invSize;

    // Edge fade: prevents off-screen offsets and foreground objects showing through.
    const float fadePx = max(gGiParams.w, 0.0);
    float edge = 1.0;
    if (fadePx > 0.0) {
        const float2 vpMin = gSceneViewportCur.xy * invSize;
        const float2 vpMax = (gSceneViewportCur.xy + gSceneViewportCur.zw) * invSize;
        const float2 d = min(uvR - vpMin, vpMax - uvR);
        edge = saturate(min(d.x, d.y) / fadePx);
    }
    return lerp(uv0, uvR, saturate(gGiParams.z) * edge);
}

// dstTerm: backdrop scene light. bgWeight: how much fogged scene the pixel holds.
float4 averBlendedOutputBackdrop(AverSurface s, float3 diffuse, float3 specular, float3 T,
                                 float2 screenPos, float3 wpos, float thicknessCm,
                                 out float3 dstTerm, out float bgWeight) {
    dstTerm  = float3(0.0, 0.0, 0.0);
    bgWeight = -1.0;
    float2 invSize;
    if (!averBlendBackdropValid(invSize))
        return averBlendedOutputVolume(s, diffuse, specular, T);

    // Two samples: refracted and straight, used in correction blend.
    bool tir = false;
    const float2 uvR   = averRefractedBackdropUV(s, wpos, thicknessCm, invSize, screenPos, tir);
    const float2 uv0   = screenPos * invSize;
    const float3 bgR   = gBlendBackdrop.SampleLevel(gMaterialSampler, uvR, 0).rgb;
    const float3 bg0   = gBlendBackdrop.SampleLevel(gMaterialSampler, uv0, 0).rgb;
    const float  alpha = saturate(s.alpha);

    // TIR: reflects past critical angle, alpha=1 (eye is inside volume).
    if (tir) {
        dstTerm  = bgR * T;
        bgWeight = 1.0;
        return float4(specular + dstTerm, 1.0);
    }

    dstTerm  = (bgR * T - bg0) * (1.0 - alpha);
    bgWeight = 1.0 - alpha;
    return float4(specular + diffuse * alpha + dstTerm, alpha);
}

// averVolumeThickness lives in voxi_pt.hlsli (every translucent composite uses it).

#if AVER_RT && AVER_RD_SPLIT
// ---- TRANSLUCENCY IN THE PATH (Settings::translucencyInPath; gPtBounceParams.w) ----
// The primary ray's translucent crossings in front of the opaque surface at `hitT`, front to back.
// Each is its material's own surface (rtHitSurface), lit like any hit (sun through the transmittance-
// aware shadow ray, the lamps, sky ambient, a traced reflection) and composited as the blended replay
// does (averShadeSplit: reflection at full strength, diffuse by coverage). What gets through is
// (1 - coverage) times the volume's absorption over its measured thickness -- PER CHANNEL, since here
// the background is known, which the replay's single blend alpha could not express. `background` is
// the opaque surface Stage B already lit.
#define AVER_RD_GLASS_LAYERS 4
float3 rdTranslucentPath(float3 dir, float hitT, float3 background, float2 pixel, out float3 throughput) {
    float3 acc = float3(0.0, 0.0, 0.0);
    float3 thr = float3(1.0, 1.0, 1.0);
    const float3 L = normalize(gLightDir.xyz);
    const uint   sampleFrame = ptReferenceMode() ? (uint)gRtHistParams.z : 0u;
    // One pixel's angular footprint, for the crossings' mips.
    const float  pixelCone = 2.0 / max(gSceneViewportCur.w, 1.0);
    float tmin = 0.0;
    [loop] for (uint k = 0u; k < AVER_RD_GLASS_LAYERS; ++k) {
        RayDesc r;
        r.Origin    = gCamPos.xyz;
        r.Direction = dir;
        r.TMin      = tmin;
        r.TMax      = hitT;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_TRANSLUCENT, r);
        averRtProceedSolid(q);
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) break;

        const RtHit h = rtHitCommitted(q, gCamPos.xyz, dir);
        float2 gx, gy;
        rtHitConeGrad(h, pixelCone, gx, gy);
        const AverSurface s = rtHitSurface(h, -dir, L, gx, gy, AVER_RT_HIT_FULL);

        // Nothing averages these layers over frames (no history, not denoised), so every ray here is the
        // same each frame: a fixed shadow and reflection sample per pixel, every lamp unshadowed. Reference
        // path tracing does average them (its accumulation), so there the samples move.
        AverLight sun;
        sun.direction  = L;
        sun.radiance   = averSunRadiance();
        sun.visibility = rdSunLit() ? rtShadow(h.pos, s.N, L, pixel, float3(0, 0, 0), float3(0, 0, 0), 1u,
                                               averGoldenTurns(sampleFrame + k))
                                    : float3(0.0, 0.0, 0.0);
        AverIndirect ind;
        ind.ambient      = averSkyIrradiance(s.N);
        ind.ambientScale = gAmbient.r;
        ind.diffuse      = float3(0.0, 0.0, 0.0);
        ind.occlusion    = 1.0;
        ind.specularTraced = 1.0;
        bool reflHit = false;
        ind.specular = rtReflection(h.pos, s.N, h.N, reflect(dir, s.N), L, pixel,
                                    s.rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : s.rough, sampleFrame, reflHit);
        float3 diffuse, specular;
        averShadeSplit(s, sun, ind, diffuse, specular);
#if AVER_RD_LAMPS
        if (rdLocalLampsLive()) rdLocalLightsShadeSplit(s, h.pos, 1.0, diffuse, specular);
#endif
        float3 T = float3(1.0, 1.0, 1.0);
        if (s.attenuationDistance > 0.0)
            T = averVolumeTransmittance(s.attenuationColor, s.attenuationDistance,
                                        averVolumeThickness(h.pos, s.N, dir));
        const float a = saturate(s.alpha);
        acc += thr * min(specular + diffuse * a, AVER_VOX_MAXRAD);
        thr *= (1.0 - a) * T;
        if (max(thr.r, max(thr.g, thr.b)) < 1e-3) break;
        tmin = h.t + ptBias(h.pos);
    }
    throughput = thr;   // NRD2 remodulates the denoised lighting by it
    return acc + thr * background;
}
#endif
// Reprojects wpos through LAST frame's camera to sample reflection history. False when off-screen,
// behind near plane, untraced (stored.a <= 0), or disocclusion.
bool rtReprojectReflection(float3 wpos, float2 pixel, out float3 hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float4 stored = gRtReflHist.Load(int3(texel, 0));
    if (stored.a <= 0.0) return false;   // not traced there.
    const float depthM = clip.w * AVER_REFL_HIST_DEPTH_SCALE;
    const float tol = max(depthM, stored.a) * 0.03 + 0.01;
    if (abs(depthM - stored.a) > tol) return false;

    hist = stored.rgb;
    velocityPx = px - pixel;
    return true;
}

// Spatial denoiser for reflections: reprojected gather, plane-distance rejection.
// Differs from rtShadowSpatial: radius from roughness, untraced neighbors skipped, no luminance weight.
float3 rtReflectionSpatial(float3 centre, float3 wpos, float3 N, float2 pixel, float curDepth,
                           float rough, float dzdx, float dzdy) {
    // Radius tracks the lobe; roughness 0 returns centre untouched, no gather.
    const int radius = rough < AVER_REFL_MIRROR_ROUGH ? 0 : (int)clamp(round(rough * 5.0), 1.0, 3.0);
    if (radius <= 0 || gRtHistParams.y < 0.75) return centre;

    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);

    // Gather around where this pixel was last frame, same arithmetic as rtReprojectHistory.
    float2 centrePx = pixel;
    const float4 pclip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    if (pclip.w > 1e-4) {
        const float3 pndc = pclip.xyz / pclip.w;
        if (pndc.z >= 0.0 && pndc.z <= 1.0)
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
    }
    const int2 base = int2(floor(centrePx));

    // Gaussian falloff: sigma = radius/2.
    const float sigma2 = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma2 * sigma2);

    float3 acc  = centre;
    float  wsum = 1.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float4 st = gRtReflHist.Load(int3(t, 0));
            if (st.a <= 0.0) continue;
            // Depths in metres.
            const float predicted = (curDepth + dzdx * (float)ox + dzdy * (float)oy) * AVER_REFL_HIST_DEPTH_SCALE;
            const float tol = max(abs(predicted), 0.01) * 0.02 + 0.01;
            if (abs(st.a - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.rgb * w;
            wsum += w;
        }
    }
    return acc / wsum;
}

// Tiled/temporal wrapper around rtReflection(), mirroring rtShadowTemporal's structure.
// Called only after roughness gating (s.rough <= 0.5). `hit`: true when return value is valid.
float3 rtReflectionTemporalEx(float3 wpos, float3 N, float3 Ng, float3 R, float3 L, float2 pixel, float rough,
                              float dzdx, float dzdy, bool doSpatial, out bool hit) {
    // Mirror cutoff: below AVER_REFL_MIRROR_ROUGH, no jitter, no temporal history, no spatial filter.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // No history texture: lobe stays closed. rough=0 reduces to exact mirror ray.
    if (gRtHistParams.x < 0.5) return rtReflection(wpos, N, Ng, R, L, pixel, 0.0, 0u, hit);

    const float4 curClip = mul(float4(wpos, 1.0), gViewProj);
    const uint frameIdx  = (uint)gRtHistParams.z;
    const uint tileBits = (uint)gRtHistParams.w;
    const float histDepth = curClip.w * AVER_REFL_HIST_DEPTH_SCALE;

    if (tileBits == 0u) {
        // Shipped path: rtPixelsPerRayTileForQuality returns 1 at every tier.
        bool   skipTrace = false;
        float3 skipCol   = 0.0;
        if ((rtGiShadowBits() & 4u) != 0u && lobeRough > 0.0) {
            const uint2 tile = (uint2(pixel) - (uint2)gSceneViewportCur.xy) / 8u;
            if (((tile.x ^ tile.y ^ (uint)gRtHistParams.z) & 1u) != 0u) {
                float2 skipVelocityPx = 0.0;
                skipTrace = gRtHistParams.y > 0.75 &&
                            rtReprojectReflection(wpos, pixel, skipCol, skipVelocityPx);
            }
        }

        bool   curHit;
        float3 col;
        if (skipTrace) {
            col    = skipCol;
            curHit = true;
        } else {
            float3 fresh = rtReflection(wpos, N, Ng, R, L, pixel, lobeRough, frameIdx, curHit);
            col = fresh;

            if (lobeRough > 0.0 && curHit) {
                float3 hist = 0.0;
                float2 velocityPx = 0.0;
                if (gRtHistParams.y > 0.75 && rtReprojectReflection(wpos, pixel, hist, velocityPx)) {
                    // Velocity-discounted weight: far-slid sample is same surface but different point.
                    // A rough lobe barely shows that parallax, so it keeps history at speed.
                    const float t = saturate(length(velocityPx) / 6.0);
                    const float fastFresh = lerp(1.0, 0.35, saturate(lobeRough * 2.0));
                    col = lerp(hist, fresh, lerp(0.15, fastFresh, t));
                }
            }
        }

        // Write raw temporal value, never filtered (prevents compounding IIR).
        col = clamp(col, 0.0, AVER_VOX_MAXRAD);
        if (gAverHistoryWrite)
            gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, histDepth) : float4(0.0, 0.0, 0.0, -1.0);
        hit = curHit;
        if (curHit && doSpatial) {
            return rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy);
        } else {
            return col;
        }
    }

    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float3 hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.75 && rtReprojectReflection(wpos, pixel, hist, velocityPx);

    float3 col;
    bool curHit;
    if (myTurn || !haveHist) {
        col = rtReflection(wpos, N, Ng, R, L, pixel, lobeRough, frameIdx, curHit);
        // Adaptive weight.
        if (curHit && haveHist) {
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            col = lerp(col, hist, weight);
        }
    } else {
        // Not this pixel's turn, reprojection found a hit: reuse, no ray this frame.
        col = hist;
        curHit = true;
    }

    hit = curHit;
    // Raw, not filtered.
    col = clamp(col, 0.0, AVER_VOX_MAXRAD);
    if (gAverHistoryWrite)
        gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, histDepth) : float4(0.0, 0.0, 0.0, -1.0);
    if (curHit && doSpatial) {
        return rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy);
    } else {
        return col;
    }
}

// Wrapper that always requests spatial gather.
float3 rtReflectionTemporal(float3 wpos, float3 N, float3 Ng, float3 R, float3 L, float2 pixel, float rough,
                            float dzdx, float dzdy, out bool hit) {
    return rtReflectionTemporalEx(wpos, N, Ng, R, L, pixel, rough, dzdx, dzdy, true, hit);
}

// Mark pixels gated away from reflection tracing (rough > 0.75).
void rtReflectionHistoryVacate(float2 pixel) {
    if (gRtHistParams.x >= 0.5 && gAverHistoryWrite)
        gRtReflHistOut[uint2(pixel)] = float4(0.0, 0.0, 0.0, -1.0);
}
#endif

// 3x3 PCF inside ONE cascade's quadrant of the atlas.
float shadowSampleCascade(float3 wpos, uint c) {
    float4 lp = mul(float4(wpos, 1.0), gCascadeViewProj[c]);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;

    // 2x2 atlas layout: quadrant x = c&1, y = c>>1. Must match the C++ side.
    float2 quad = float2(c & 1u, c >> 1u) * 0.5;
    float inset = gShadowParams.x;
    uv = quad + clamp(uv * 0.5, float2(inset, inset), float2(0.5 - inset, 0.5 - inset));

    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z);
    return s / 9.0;
}

// Where shadowing starts fading to unshadowed.
#define AVER_SHADOW_FADE_START 0.84

// Picks a cascade by distance and samples it. Returns sun visibility, 1 = fully lit.
float shadowFactor(float3 wpos, float3 N, float ndl) {
    if (gShadowParams.y < 0.5) return 1.0;
    uint count = (uint)gShadowParams.w;
    if (count == 0) return 1.0;
    count = min(count, (uint)AVER_SHADOW_CASCADES);

    float slope = saturate(1.0 - ndl);
    float dist  = distance(wpos, gCamPos.xyz);

    float fadeSpan = max(gCascadeSplit[count - 1].x * (1.0 - AVER_SHADOW_FADE_START), 1e-3);
    float fade = saturate((dist - gCascadeSplit[count - 1].x * AVER_SHADOW_FADE_START) / fadeSpan);

    [loop] for (uint c = 0; c < count; ++c) {
        if (dist > gCascadeSplit[c].x) continue;
        float bias = gCascadeSplit[c].y * (1.0 + slope);
        float s = shadowSampleCascade(wpos + N * bias, c);
        if (s >= 0.0) return lerp(s, 1.0, fade);
    }
    return 1.0;
}

// Sun visibility from GI-only shadow map (box-fitted to GI volume, not camera-fitted).
float giShadowFactor(float3 wpos, float3 N, float ndl) {
    // Missing map: fully lit (soft dependency).
    if (gGiShadowParams.y < 0.5) return 1.0;

    float slope = saturate(1.0 - ndl);
    float3 p0 = wpos + N * (gGiShadowParams.z * (1.0 + slope));

    float4 lp = mul(float4(p0, 1.0), gGiShadowViewProj);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    // Outside the box: fully lit (voxel is outside the volume).
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return 1.0;

    // This map is one box filling the whole texture.
    float t = gGiShadowParams.x;
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gGiShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * t, p.z);
    return s / 9.0;
}

// Per-dispatch constants for CSClear/CSResolve/CSMip (b3: b0/b1 taken by the graphics root signature).
// 32 B: gSrcMip@0, gBoxLo@4 (12B), gBoxHi@16 (12B), _boxPad@28 -- mirrored byte-for-byte by
// aver::voxi::GiDispatchConstants (GiDispatchBounds.hpp, static_asserts its size against this layout so
// the two cannot drift apart silently).
// gSrcMip is CSMip's source-level index, unused by CSClear/CSResolve (always mip 0). gBoxLo/gBoxHi is a
// half-open voxel-space box in the DESTINATION mip's coordinate space, uploaded by VoxiRenderer per
// dispatch. See aver-voxi-cbuffer-three-mirrors.md and the W3 spec for why its own cbuffer, not VoxiFrame.
cbuffer MipCB : register(b3) { uint gSrcMip; uint3 gBoxLo; uint3 gBoxHi; uint _boxPad; };

#include "voxi_cone.hlsli"

// ---- CSAirVis: bakes gAirVisOut from gVoxelTex's occupancy ----
// Scheduled round-robin per slab of z-layers; whole volume refreshes every few frames at flat cost.
#define AVER_AIRVIS_RES 32            // gAirVis resolution (~1.8 m cells over a 56 m GI volume)
#define AVER_AIRVIS_DIRS 16           // directions marched per cell, tiling the upper hemisphere
#define AVER_AIRVIS_MIN_ELEV_DEG 3.0  // Fibonacci hemisphere floor.
#define AVER_AIRVIS_OCC_GAIN 4.0      // occupancy multiplier for coarse-mip blocking.
#define AVER_AIRVIS_MIN_T 0.01        // stop marching when transmittance falls below this.
#define AVER_AIRVIS_MAX_STEPS 48      // per-direction step ceiling.

[numthreads(8,8,1)]
void CSAirVis(uint3 tid : SV_DispatchThreadID) {
    // The dispatch covers one slab (or whole volume); gBoxLo/gBoxHi say which cells.
    const uint3 id = tid + gBoxLo;
    if (any(id >= gBoxHi) || any(id >= (uint3)AVER_AIRVIS_RES)) return;

    // Cell centre in world space.
    const float3 uvwCell = (float3(id) + 0.5) / (float)AVER_AIRVIS_RES;
    const float3 p = gVoxelOrigin.xyz + uvwCell / gVoxelOrigin.w;

    // One voxel of the GI radiance volume in world units.
    const float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);

    // Half-angle from direction count.
    const float cosHalf = saturate(1.0 - 1.0 / (float)AVER_AIRVIS_DIRS);
    const float halfAngleTan = sqrt(max(1.0 - cosHalf * cosHalf, 1e-6)) / max(cosHalf, 1e-6);
    const float sinMinElev = sin(radians(AVER_AIRVIS_MIN_ELEV_DEG));

    float Tsum = 0.0;
    // Deterministic Fibonacci hemisphere, no jitter, z stratified over [sin(minElev), 1].
    [loop] for (uint dirIdx = 0; dirIdx < AVER_AIRVIS_DIRS; ++dirIdx) {
        const float z = sinMinElev + (1.0 - sinMinElev) * ((float)dirIdx + 0.5) / (float)AVER_AIRVIS_DIRS;
        const float r = sqrt(saturate(1.0 - z * z));
        const float phi = 2.39996323 * (float)dirIdx;
        const float3 d = float3(r * cos(phi), r * sin(phi), z);

        // Widening cone through the volume, reading occupancy, starting from cell centre.
        float T = 1.0;
        float dist = voxelWorld * 0.5;
        [loop] for (uint step = 0; step < AVER_AIRVIS_MAX_STEPS; ++step) {
            if (T < AVER_AIRVIS_MIN_T) break;
            const float3 uvwGi = voxelUVW(p + d * dist);
            if (!insideVolume(uvwGi)) break;   // beyond volume = open sky.
            const float footprint = max(voxelWorld, dist * halfAngleTan);
            const float mip = log2(footprint / voxelWorld);
            const float occupancy = gVoxelTex.SampleLevel(gVoxelSamp, uvwGi, mip).a;
            // One-voxel-thick roof, OCC_GAIN pushes thin occluders toward fully blocking.
            T *= 1.0 - saturate(occupancy * AVER_AIRVIS_OCC_GAIN);
            dist += footprint;
        }
        Tsum += T;
    }

    gAirVisOut[id] = Tsum / (float)AVER_AIRVIS_DIRS;
}

// ---- SHADE-PASS READ: sky visibility along ray from camera to wpos ----
// Called from PSMainVoxi/PSRayDriven's fog sites only.
// Deterministic: 8 fixed points, no jitter, no per-pixel history.
float voxiAirVisibility(float3 wpos) {
    // Voxel GI off, or volume doesn't exist: unoccluded air.
    if (gVoxelParams.w <= 0.5) return 1.0;
    uint dimX, dimY, dimZ;
    gAirVis.GetDimensions(dimX, dimY, dimZ);
    // Placeholder is 1x1x1.
    if (dimX <= 1u) return 1.0;

    const float3 camPos = gCamPos.xyz;
    const float segLen = length(wpos - camPos);
    if (segLen <= 1e-4) return 1.0;
    const float3 dir = (wpos - camPos) / segLen;

    // Sample starts at fog's start distance, not camera (fog before contributes nothing).
    const float start = min(gFogParams.z, segLen);
    const float span = segLen - start;

    // Height-fog density weighting: sample deep under fog outvotes one near its ceiling.
    const float fogK = gFogParams.x;
    const float fogHeight = gFogParams.y;

    float wSum = 0.0, vSum = 0.0;
    [unroll] for (uint i = 0; i < 8u; ++i) {
        const float t = start + (((float)i + 0.5) / 8.0) * span;
        const float3 p = camPos + dir * t;
        const float3 uvw = voxelUVW(p);
        // Outside volume counts as open sky.
        const float v = insideVolume(uvw) ? gAirVis.SampleLevel(gVoxelSamp, uvw, 0.0) : 1.0;
        const float w = fogK <= 1e-8 ? 1.0 : exp(-(p.z - fogHeight) * fogK);
        wSum += w;
        vSum += w * v;
    }
    return wSum > 1e-6 ? vSum / wSum : 1.0;
}

// ================= additive G-buffer: velocity, view-space depth, normal+roughness =================
// Gated on AVER_GBUFFER, following AVER_RT's convention: compile-time define, off by default.
// ADDITIVE: without the define, PSMainVoxi returns one SV_TARGET unchanged.
#if AVER_GBUFFER
struct GBufferOut {
    float4 col              : SV_TARGET0;   // exactly PSMainVoxi's own colour.
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
};

// Packs world-space unit normal and roughness into RGB10A2.
// xy: octahedral normal (Cigolle et al. 2014), z: roughness, w: spare.
// 10 bits per axis keeps angular error ~0.1 degrees.
// Decode in modules/render.denoise/shaders/aver_denoise.hlsl (dnsrDecodeNormal).
float4 averPackNormalRoughness(float3 N, float roughness) {
    N /= abs(N.x) + abs(N.y) + abs(N.z);
    float2 p = N.xy;
    if (N.z < 0.0) p = (1.0 - abs(N.yx)) * float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
    return float4(p * 0.5 + 0.5, saturate(roughness), 0.0);
}

// Screen-space motion for velocity, in the scene viewport rect.
// Clip positions from this frame's camera and last frame's in, destination-minus-source pixels out.
float2 averClipToVelocity(float4 curClip, float4 prevClip) {
    // Either transform can put a point behind its near plane; zero is "no motion known".
    if (curClip.w <= 1e-4 || prevClip.w <= 1e-4) return float2(0.0, 0.0);

    const float2 curNdc  = curClip.xy  / curClip.w;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    // THIS frame's clip must land in THIS frame's viewport rect (gViewProj with gSceneViewportCur).
    const float4 curRect = gSceneViewportCur.w > 0.0 ? gSceneViewportCur : gSceneViewport;
    const float2 curPx  = curRect.xy +
                          float2(curNdc.x * 0.5 + 0.5, 0.5 - curNdc.y * 0.5) * curRect.zw;
    const float2 prevPx = gSceneViewport.xy +
                          float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    // Destination (curPx) minus source (prevPx).
    return curPx - prevPx;
}

// Motion of a surface point: wpos through this frame's camera minus wposPrev through last frame's.
float2 averGBufferVelocityMoved(float3 wpos, float3 wposPrev) {
    return averClipToVelocity(mul(float4(wpos, 1.0), gViewProj),
                              mul(float4(wposPrev, 1.0), gPrevViewProj));
}

// Camera-only motion: surface stays in place. PSMainVoxi (raster) uses this only.
float2 averGBufferVelocity(float3 wpos) {
    return averGBufferVelocityMoved(wpos, wpos);
}

// Sky pixel: rotation-only reprojection of view direction (a point at infinity).
float2 averGBufferVelocitySky(float3 dir) {
    return averClipToVelocity(mul(float4(dir, 0.0), gViewProj),
                              mul(float4(dir, 0.0), gPrevViewProj));
}

// Expansion point for PSMainVoxi's returns, so extra channels stay identical everywhere.
#define AVER_GBUF_RETURN(colorExpr) \
    { GBufferOut aver_gbuf_o; aver_gbuf_o.col = (colorExpr); aver_gbuf_o.velocity = gbufVelocity; \
      aver_gbuf_o.viewZ = gbufViewZ; aver_gbuf_o.normalRoughness = gbufNormalRough; return aver_gbuf_o; }
#else
// Disabled: expands to a plain return.
#define AVER_GBUF_RETURN(colorExpr) return (colorExpr)
#endif

// ---- B1: DOES A COLOUR ALREADY MARK THIS PIXEL'S DIFFUSE CHANNEL AS POISONED? ----
// giRestirIndirect (voxi_restir.hlsli) paints sentinel colours on its return value.
// PSMainVoxi/PSRayDriven add VIOLET for the specular term's ceiling clamp.
// Precedence: giRestirIndirect colours win over violet (seven already ranked internally).
bool aver_IsGiRestirPoisonColour(float3 c) {
    return (c.r == 1.0 && c.g == 0.0 && c.b == 1.0)    // magenta: store-time reservoir guard
        || (c.r == 0.0 && c.g == 1.0 && c.b == 1.0)    // cyan: candidate-radiance clamp
        || (c.r == 1.0 && c.g == 1.0 && c.b == 0.0)    // yellow: target-pdf guard
        || (c.r == 1.0 && c.g == 0.5 && c.b == 0.0)    // orange: final-estimate guard
        || (c.r == 0.0 && c.g == 0.0 && c.b == 1.0)    // blue: denoiser-readback non-finite guard
        || (c.r == 1.0 && c.g == 0.0 && c.b == 0.0)    // red: raw estimate ceiling
        || (c.r == 0.0 && c.g == 1.0 && c.b == 0.0);   // green: denoised readback ceiling
}

// The Voxi lit pixel shader. Voxi supplies light transport only – sun visibility, sky, bounce – and the material shades it.
#if AVER_GBUFFER
[earlydepthstencil]
GBufferOut PSMainVoxi(VSOut i) {
#else
[earlydepthstencil]
float4 PSMainVoxi(VSOut i) : SV_TARGET {
#endif
    float3 N = normalize(i.nrmWS);
    // Face-forwarded by averVertexOf's rule (dot(N,V) < 0 flips).
    if (dot(N, gCamPos.xyz - i.wpos) < 0.0) N = -N;

    // History write gate for blended-replay fragments.
    const bool blendedFragment = ((uint)gAmbientParams.w & 32u) != 0u && averDrawIsTranslucent();
    gAverHistoryWrite = !blendedFragment || ((uint)gAmbientParams.z & 32u) != 0u;

    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));
    // Subsurface lit from behind: shadow query starts on light-facing side.
    const float3 sssShadowPush = averSubsurfaceShadowPush(gMaterialFlags, gSubsurfaceRadius, N, L);
    const bool   sssShadowBack = any(sssShadowPush != 0.0);
    const float3 shadowMapP    = i.wpos + sssShadowPush;
    const float3 shadowMapN    = sssShadowBack ? -N : N;
    const float  shadowMapNdl  = sssShadowBack ? saturate(dot(-N, L)) : ndl;
#if AVER_RT
    // View-space depth taken in uniform control flow for derivatives.
    const float rtViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
    const float rtDzdx  = ddx(rtViewZ);
    const float rtDzdy  = ddy(rtViewZ);
    // Shadow-ray footprint computed in uniform flow.
    const float3 rtDpx = ddx(i.wpos);
    const float3 rtDpy = ddy(i.wpos);

    // Reuse staged ray-driven lighting for a translucent pixel on an opaque surface.
    const bool rdReuseCandidate = blendedFragment
                                && (rtGiShadowBits() & 16u) != 0u
                                && gShadowParams.z > 0.5
                                && !(gAttenuationDistance > 0.0)
                                && gMaterialGraphId == 0u;
    // Depth proof: gRdSunVisTex alpha now holds opaque surface's linear view depth (or 0.0 on miss).
    const float4 rdSunVisTexel = rdReuseCandidate ? gRdSunVisTex[uint2(i.pos.xy)] : float4(0.0, 0.0, 0.0, 0.0);
    // Tolerance: 1 cm flat plus 0.4% of depth (RGBA16F mantissa steps ~2 cm at 30 m).
    bool rdOnPlane = false;
    if (rdReuseCandidate) {
        const float3 rdV   = i.wpos - gCamPos.xyz;
        const float3 rdPf  = gCamPos.xyz + rdV * (rdSunVisTexel.a / max(rtViewZ, 1e-3));
        const float3 rdNs  = cross(QuadReadAcrossX(rdPf) - rdPf, QuadReadAcrossY(rdPf) - rdPf);
        const float3 rdNd  = cross(rtDpx, rtDpy);
        const float  rdNsL = length(rdNs), rdNdL = length(rdNd);
        rdOnPlane = rdNsL > 1e-20 && rdNdL > 1e-20
                 && abs(dot(rdNs, rdNd)) > 0.95 * rdNsL * rdNdL
                 && abs(dot(i.wpos - rdPf, rdNd)) <= rdNdL + 0.004 * abs(dot(rdV, rdNd));
    }
    const bool rdReusePx = rdReuseCandidate && rdSunVisTexel.a > 0.0
                         && (abs(rdSunVisTexel.a - rtViewZ) <= 1.0 + 0.004 * rtViewZ || rdOnPlane);
    // Quad-uniform: all four pixels must take the same branch for derivatives.
    const uint rdReuseBit = rdReusePx ? 1u : 0u;
    const bool rdReuse = (rdReuseBit & QuadReadAcrossX(rdReuseBit) & QuadReadAcrossY(rdReuseBit) &
                          QuadReadAcrossDiagonal(rdReuseBit)) != 0u;
    // Pane lit on its own: blended fragment that cannot reuse staged lighting.
    const bool paneOwnLight = blendedFragment && !rdReuse;

    float3 sunVis;
    if (rdReuse)
        sunVis = rdSunVisTexel.rgb;
    else if (gShadowParams.z > 0.5) {
        gAverShadowOriginPush = sssShadowPush;
        sunVis = rtShadowTemporal(i.wpos, N, L, i.pos.xy, rtDpx, rtDpy,
                                  (uint)max(gRtParams.y, 1.0));
        gAverShadowOriginPush = float3(0.0, 0.0, 0.0);
    }
    else                       sunVis = shadowFactor(shadowMapP, shadowMapN, shadowMapNdl);
#else
    const float3 sunVis = shadowFactor(shadowMapP, shadowMapN, shadowMapNdl);
#endif
#if AVER_GBUFFER
    // View-space linear depth for G-buffer (reuses rtViewZ when AVER_RT).
#if AVER_RT
    const float gbufViewZ = rtViewZ;
#else
    const float gbufViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
#endif
#endif
    float ao = 1.0;
    float3 ind = 0;
    bool giDiffusePoisoned = false;
    bool restirSuppliedDiffuse = false;
    bool aoGathered = false;
    // Switches diffuse bounce estimator (cone gather vs ReSTIR).
#if AVER_RT
    if (gVoxelParams.w > 0.5) {
        if (gGiRestirParams.x > 0.5 && !paneOwnLight && !(blendedFragment && ((uint)gAmbientParams.w & 16u) != 0u)) {
            if (rdReuse) {
                // Stage G's answer (ao left at 1.0 as giRestirIndirect would set it).
                ind = gRdGiTex[uint2(i.pos.xy)].rgb;
            } else {
                ind = giRestirIndirect(i.wpos, N, rtViewZ, i.pos.xy, (uint)gRtHistParams.z, ao);
            }
            giDiffusePoisoned = aver_IsGiRestirPoisonColour(ind);
            restirSuppliedDiffuse = true;
        } else {
            ind = coneTracedIndirect(i.wpos, N, ao);
            aoGathered = true;
        }
    }
#else
    if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);
#endif

    AverVertex vtx = averVertexOf(i);

    // Single-sided blended surface draws one layer.
    const bool eyeInside = gCameraMedium.x > 0.5;
    if ((gMaterialFlags & AVER_MAT_ALPHA_BLEND) && !(gMaterialFlags & AVER_MAT_TWO_SIDED) &&
        (eyeInside ? !vtx.backFace : vtx.backFace))
        clip(-1);

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;
    // Caustics scaled with the sun, not as their own glow.
    // Opaque only: water's own top face would self-light.
    if (!(gMaterialFlags & AVER_MAT_ALPHA_BLEND))
        sun.visibility *= 1.0 + averCausticFocus(vtx.wpos);

    AverSurface s = averEvalMaterial(vtx, sun);
    // Decals repaint the surface before anything lights it (voxi_decal.hlsli); opaque draws only.
    // Plain raster only: with decals compiled into the ray-traced PSMainVoxi variants (here or through
    // rtHitSurface) NewSponza Night's default view hung the RX 7800 XT (docs/rendering/DECALS.md).
#if !AVER_RT
    if (gDecalParams.x > 0.5 && !averDrawIsTranslucent()) averApplyDecals(s, i.wpos, N, true);
#endif
#if AVER_GBUFFER
    // Velocity and packed normal/roughness computed once, shared by all return sites.
    const float2 gbufVelocity    = averGBufferVelocity(i.wpos);
    const float4 gbufNormalRough = averPackNormalRoughness(averShadingNormal(s), s.rough);
#endif
    float4 display;
    if (averDisplayColour(s, display)) AVER_GBUF_RETURN(display);

    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 R = reflect(-V, averShadingNormal(s));
    AverIndirect ind4;
    bool giPoisonSpecCeilHit = false;
    ind4.specularTraced = 0.0;
    ind4.ambient      = averSkyIrradiance(averShadingNormal(s));
    ind4.ambientScale = gAmbient.r;
    // Traced sky visibility when tier supports it, else cone gather estimate.
#if AVER_RT
    ind4.occlusion    = gAmbientParams.x > 0.5
                      && !paneOwnLight
                      ? ((rdReuse && (gGiRestirParams.x > 0.5 || gVoxelParams.w <= 0.5))
                         ? gRdAoTex[uint2(i.pos.xy)].r
                         : rtSkyOcclusionTemporal(i.wpos, N, i.pos.xy, (uint)gAmbientParams.x, ao,
                                                  aoGathered, true))
                      : ao;
#else
    ind4.occlusion    = ao;
#endif
    // One owner for sky at giMode 1: subtract sky contribution from ReSTIR diffuse to avoid double-counting.
    ind4.diffuse      = ind;
    if (restirSuppliedDiffuse && !giDiffusePoisoned && ((uint)gAmbientParams.z & 2u) == 0u)
        ind4.diffuse = ind - ind4.ambient * ind4.ambientScale * ind4.occlusion * s.occlusion * gVoxelParams.y;
#if AVER_RT
    // Ray traced when acceleration structure exists; preferred over cone (bounded by voxel volume).
    // Roughly past 0.75 rough the lobe is wide enough that one ray cannot close the integral.
    float3 rNg = cross(ddx(i.wpos), ddy(i.wpos));
    rNg = dot(rNg, rNg) > 1e-12 ? normalize(rNg) : N;
    if (dot(rNg, N) < 0.0) rNg = -rNg;
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(i.pos.xy);
    if (rtReflTraced) {
        // Stage R's answer when reusing staged lighting AND CSRdRefl traced for this pixel.
        const float4 rdReflTexel = rdReuse ? gRdReflTex[uint2(i.pos.xy)] : float4(0.0, 0.0, 0.0, 0.0);
        if (rdReuse && rdReflTexel.a > 0.5) {
            // CSRdRefl's ceiling test in alpha (not recomputed after clamping/rounding).
            giPoisonSpecCeilHit = rdReflTexel.a > 1.5;
            ind4.specular = rdReflTexel.rgb;
            ind4.specularTraced = 1.0;
        } else {
            bool specHit = false;
            float3 refl = rtReflectionTemporal(i.wpos, N, rNg, R, L, i.pos.xy, s.rough,
                                               rtDzdx, rtDzdy, specHit);
            const float skyW = 0.0;
            float3 skyR = float3(0.0, 0.0, 0.0);
            if (!specHit) skyR = skyColor(R);
            // Raster twin of clamp at PSRayDriven: both primary-visibility paths must agree.
            // PRE-clamp value tested against same ceiling as clamp below.
            const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
            giPoisonSpecCeilHit = any(specRaw >= AVER_VOX_MAXRAD);
            ind4.specular = clamp(specRaw, 0.0, AVER_VOX_MAXRAD);
            ind4.specularTraced = 1.0;
        }
    } else
#endif
    if (gVoxelParams.w > 0.5) {
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(i.wpos, R, specAperture);
        // Sky skipped where cone already saw a wall (avoids paying for values multiplied away).
        const float skyWeight = 1.0 - sceneSpec.a;
        ind4.specular       = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
        if (skyWeight > 0.004) ind4.specular += skyColor(R) * skyWeight;
    } else {
        ind4.specular       = skyColor(R);
    }

    // Translucent materials take a separate early-returning path.
    if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) {
        // averShadeSplit keeps direct/indirect separate: blended draws get same energy as opaque.
        float3 dif, spc;
        averShadeSplit(s, sun, ind4, dif, spc);
#if AVER_RT && AVER_RD_LAMPS
        // Local lights on a pane, shadowed where reusing staged surface underneath.
        if (rdLocalLampsLive()) {
            float lampVis = 1.0;
            if (rdReuse) lampVis = rdLocalVisFiltered(uint2(i.pos.xy));
            rdLocalLightsShadeSplit(s, i.wpos, lampVis, dif, spc);
        }
#endif
        // rgb = specular + diffuse*alpha, a = alpha; PremultipliedAlpha blend expects this.
        float4 outc;
        float3 dstTerm  = float3(0.0, 0.0, 0.0);
        float  bgWeight = -1.0;
#if AVER_RT
        // Front faces only: averVolumeThickness traces along view direction to nearest hit.
        if (s.attenuationDistance > 0.0 && !s.backFace) {
            const float volThick = averVolumeThickness(i.wpos, N, -s.V);
            const float3 volT = averVolumeTransmittance(
                s.attenuationColor, s.attenuationDistance, volThick);
        outc = averBlendedOutputBackdrop(s, dif, spc, volT, i.pos.xy, i.wpos, volThick, dstTerm, bgWeight);
        } else
#endif
        outc = averBlendedOutput(s, dif, spc);

        // Fog applied after blend, unlike water (which reads as air itself).
        if (bgWeight < 0.0) bgWeight = 1.0 - outc.a;
        float3 airExt, airIn;
        averFogTermsAirVis(i.wpos, true, voxiAirVisibility(i.wpos), airExt, airIn);
        outc.rgb = (outc.rgb - dstTerm) * airExt + airIn * (1.0 - bgWeight) + dstTerm;
        // Poison marker applied last (after fog).
        if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
            outc.rgb = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
        AVER_GBUF_RETURN(outc);
    }

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
#if AVER_RT && AVER_RD_LAMPS
    // Local lights on opaque surface.
    if (rdLocalLampsLive())
        radiance += rdLocalLightsShade(s, i.wpos,
                                       rdLocalLightsVisibility(i.wpos, vtx.N, i.pos.xy, uint2(i.pos.xy), true));
#endif
    radiance = averShadeIndirect(radiance, s, ind4);
    radiance = averApplyFogAirVis(radiance, i.wpos, true, voxiAirVisibility(i.wpos));
    // Poison marker.
    if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
        radiance = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
    AVER_GBUF_RETURN(float4(radiance, averOpacity(s)));
}

// Ray-driven primary visibility (experimental).
#if AVER_RT
struct RayDrivenOut {
    float4 col   : SV_TARGET;
    float  depth : SV_DEPTH;
};

// PSRayDriven VIEW-DEBUG colour helpers (vmode 2-5).
uint viewDebugHash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint viewDebugHashCombine(uint a, uint b) {
    return viewDebugHash(viewDebugHash(a) ^ b);
}

float3 viewDebugHueColor(uint h) {
    const float hue = (float)(h & 0xFFFFu) * (6.0 / 65536.0);   // [0, 6)
    const float s = 0.65;
    const float v = 0.85;
    const float c = v * s;
    const float x = c * (1.0 - abs(fmod(hue, 2.0) - 1.0));
    const float m = v - c;
    float3 rgb;
    if      (hue < 1.0) rgb = float3(c, x, 0.0);
    else if (hue < 2.0) rgb = float3(x, c, 0.0);
    else if (hue < 3.0) rgb = float3(0.0, c, x);
    else if (hue < 4.0) rgb = float3(0.0, x, c);
    else if (hue < 5.0) rgb = float3(x, 0.0, c);
    else                rgb = float3(c, 0.0, x);
    return rgb + m;
}

float3 viewDebugHeatRamp(float t) {
    const float3 stops[5] = {
        float3(0.0, 0.0, 1.0),   // blue
        float3(0.0, 1.0, 1.0),   // cyan
        float3(0.0, 1.0, 0.0),   // green
        float3(1.0, 1.0, 0.0),   // yellow
        float3(1.0, 0.0, 0.0),   // red
    };
    const float scaled = saturate(t) * 4.0;
    const uint  i0 = (uint)floor(scaled);
    const uint  i1 = min(i0 + 1u, 4u);
    return lerp(stops[i0], stops[i1], scaled - (float)i0);
}

float3 viewDebugDistanceColor(float hitTCm) {
    const float kNearCm = 50.0;
    const float kFarCm  = 20000.0;
    const float t = saturate(log2(max(hitTCm, kNearCm) / kNearCm) / log2(kFarCm / kNearCm));
    return viewDebugHeatRamp(t);
}

float3 viewDebugColor(uint vmode, uint instanceIndex, uint materialIndex, uint primIndex,
                       float hitTCm, float3 N, float3 rayDir) {
    if (vmode == 4u)
        return viewDebugDistanceColor(hitTCm);   // RayHitDistance

    const float shapeCue = 0.55 + 0.45 * saturate(dot(N, -rayDir));
    if (vmode == 2u)      return viewDebugHueColor(viewDebugHash(instanceIndex)) * shapeCue;              // RayHitInstance
    else if (vmode == 3u) return viewDebugHueColor(viewDebugHash(materialIndex)) * shapeCue;              // RayHitMaterial
    else                  return viewDebugHueColor(viewDebugHashCombine(instanceIndex, primIndex)) * shapeCue; // Triangles (5)
}

#if AVER_NRD2
#if !(AVER_RD_SPLIT && AVER_GBUFFER)
#error AVER_NRD2 is a Stage B (AVER_RD_SPLIT) G-buffer variant
#endif
// NRD2 (docs/rendering/NRD2.md): Stage B's targets ride table-0 UAV slots an NRD2 frame does not use
// (VoxiRenderer::bindNrd2Targets): u3 D, u23 S (CSRdRefl's raw sample + hit distance first),
// u9 Rd + Rs.r, u2 Rs.gb.
#define gNrd2DiffOut   gRtReflHistOut
#define gNrd2SpecOut   gRdReflDnIn
#define gNrd2RemodAOut gGiRadianceOut
#define gNrd2RemodBOut gRtShadowHistOut

// Sun + indirect into NRD2's diffuse and specular buckets: averShadeSplit's lobes, emissive left out,
// and the multiple-scatter share of the ambient term (FmsEms, a specular quantity) routed to specular
// so a metal's diffuse bucket stays empty.
void nrd2ShadeSplit(AverSurface s, AverLight l, AverIndirect ind, inout float3 dif, inout float3 spec) {
    if (s.model == AVER_MODEL_UNLIT) return;
    float3 dD, dS, dSss;
    float  ndl;
    averDirectTerms(s, l, dD, dS, dSss, ndl);
    const float3 lt = l.radiance * ndl * l.visibility;
    dif  += dD * lt + dSss * l.radiance * l.visibility;
    spec += dS * lt;
    float3 specEnv, diffAmbient, diffBounce, FssEss, FmsEms, kD;
    averIndirectTerms(s, ind, specEnv, diffAmbient, diffBounce);
    averIndirectFactors(s, FssEss, FmsEms, kD);
    const float3 ms = FmsEms / max(FmsEms + kD, 1e-6);
    dif  += diffAmbient * (1.0 - ms) + diffBounce;
    spec += specEnv + diffAmbient * ms;
    if (s.sssWeight > 0.0) dif += averSubsurfaceAmbient(s, ind);
}
#endif

#if AVER_GBUFFER
struct RayDrivenGBufferOut {
    float4 col              : SV_TARGET0;
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
    float  depth            : SV_DEPTH;
};
#endif

// This entry point must fill the G-buffer (ray-driven is now default).
#if AVER_GBUFFER
RayDrivenGBufferOut PSRayDriven(SkyOut i) {
    RayDrivenGBufferOut o;
#else
RayDrivenOut PSRayDriven(SkyOut i) {
    RayDrivenOut o;
#endif

    gAverHistoryWrite = true;

    float3 dir = averViewRayDir(i.ndc);

    RayDesc r;
    r.Origin    = gCamPos.xyz;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = 1.0e7;

    const uint vmode = (uint)(gViewParams.x + 0.5);

#if AVER_RD_SPLIT
    // Stage B: read CSRdVisibility's record instead of tracing.
    const uint  rdPitch = rdRowPitch();
    const uint2 rdPixel = uint2(i.pos.xy);
    const uint4 rdRec   = gRdVisBuf[rdPixel.y * rdPitch + rdPixel.x];

    if (rdRec.x == 0xFFFFFFFFu) {
        // Miss: paint flat non-surface colour for debug views.
        if (vmode >= 2u) {
            o.col = float4(0.02, 0.02, 0.04, 1.0);
        } else {
            o.col = float4(skyColorFull(dir), 1.0);
        }
        o.depth = 1.0;
#if AVER_GBUFFER
        // Sky velocity is camera's rotation-only reprojection; viewZ is far sentinel.
        o.velocity        = averGBufferVelocitySky(dir);
        o.viewZ            = 1.0e7;
        o.normalRoughness  = averPackNormalRoughness(-dir, 1.0);
#endif
        if (gGiRestirParams.x > 0.5)
            gGiSurfNrmHistOut[uint2(i.pos.xy)] = float2(0.0, asfloat(0u));
#if AVER_NRD2
        gNrd2DiffOut[rdPixel]   = float4(0.0, 0.0, 0.0, 0.0);
        gNrd2SpecOut[rdPixel]   = float4(0.0, 0.0, 0.0, -1.0);
        gNrd2RemodAOut[rdPixel] = float4(0.0, 0.0, 0.0, 0.0);
        gNrd2RemodBOut[rdPixel] = float2(0.0, 0.0);
#endif
        return o;
    }

    RdSurface rdS  = rdSurfaceFromRecord(rdRec, dir);
    RtInstance inst = rdS.inst;
    uint i0 = rdS.i0, i1 = rdS.i1, i2 = rdS.i2;
    float3 w       = rdS.w;
    float3 N       = rdS.N;
    float2 hitUV   = rdS.hitUV;
    RtMaterial mat = rdS.mat;
    const float hitT = rdS.hitT;
    float3 wpos    = rdS.wpos;
    const uint rdInstanceIndex = rdRec.x;
    const uint rdPrimIndex     = rdRec.y;
#else
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Opaque lane (AVER_RT_MASK_OPAQUE, not _OPAQUE_ALL).
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE, r);
    averRtProceedSolid(q);

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        // Miss is sky at far plane.
        if (vmode >= 2u) {
            o.col = float4(0.02, 0.02, 0.04, 1.0);
        } else {
            o.col = float4(skyColorFull(dir), 1.0);
        }
        o.depth = 1.0;
#if AVER_GBUFFER
        // No real surface; velocity is sky's rotation-only reprojection.
        o.velocity        = averGBufferVelocitySky(dir);
        o.viewZ            = 1.0e7;
        o.normalRoughness  = averPackNormalRoughness(-dir, 1.0);
#endif
        if (gGiRestirParams.x > 0.5)
            gGiSurfNrmHistOut[uint2(i.pos.xy)] = float2(0.0, asfloat(0u));
        return o;
    }

    // Surface reconstruction: barycentric interpolation and rotation-only normal transform.
    RtInstance inst = rtLoadInstance(rtPackCommitted(q));
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0  = inst.firstVertex + gRtIndices[tri + 0];
    uint i1  = inst.firstVertex + gRtIndices[tri + 1];
    uint i2  = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w    = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    float3 N    = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(N, dir) > 0.0) N = -N;

    // UV same barycentric pattern; not sampled here yet (no texture array).
    float2 hitUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;

    // Hit's material keyed by RtInstance::materialIndex.
    RtMaterial mat = gRtMaterials[inst.materialIndex];

    const float hitT = q.CommittedRayT();
    float3 wpos = gCamPos.xyz + dir * hitT;
    const uint rdInstanceIndex = rtPackCommitted(q);
    const uint rdPrimIndex     = q.CommittedPrimitiveIndex();
    {
        const float3 objPos = gRtVerts[i0].pos * w.x + gRtVerts[i1].pos * w.y + gRtVerts[i2].pos * w.z;
        gAverReprojDelta = mul(float4(objPos, 1.0), inst.prevObjectToWorld).xyz -
                           mul(float4(objPos, 1.0), inst.objectToWorld).xyz;
    }
#endif
    float3 L    = normalize(gLightDir.xyz);

    // Ray differential footprint (Igehy 1999) from neighbouring pixels to avoid grazing-angle aliasing.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float3 dirDx = averViewRayDir(i.ndc + float2(ndcPixelStep.x, 0.0));
    float3 dirDy = averViewRayDir(i.ndc + float2(0.0, ndcPixelStep.y));
    // Neighbour ray displacement at hit distance, flattened to tangent plane.
    const float3 rdRayDx = (dirDx - dir) * hitT;
    const float3 rdRayDy = (dirDy - dir) * hitT;
    const float3 dpx = rdRayDx - N * dot(rdRayDx, N);
    const float3 dpy = rdRayDy - N * dot(rdRayDy, N);

    // Same shadow call as raster path (temporal wrapper included).
#if AVER_RD_SPLIT
    // Stage B reads Stage S's already-resolved sun visibility.
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float3 sunVis = float3(1.0, 1.0, 1.0);   // ablated: fully lit, no ray
#else
    float3 sunVis = gRdSunVisTex[uint2(i.pos.xy)].rgb;
#endif
#else
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float sunVis = 1.0;   // ablated: fully lit, no ray
#else
    // Subsurface hit lit from its light-facing side (PSMainVoxi explains why).
    gAverShadowOriginPush = averSubsurfaceShadowPush(mat.flags, mat.subsurfaceRadius, N, L);
    float3 sunVis = rtShadowTemporal(wpos, N, L, i.pos.xy, dpx, dpy, (uint)max(gRtParams.y, 1.0));
    gAverShadowOriginPush = float3(0.0, 0.0, 0.0);
#endif
#endif

    // THE HIT'S SURFACE, built like every ray hit's (voxi_rt.hlsli's rtHitSurface): all eight maps and
    // the slope-blended second layer through the shared composition, with the footprint from the ray
    // differentials above.
    RtHit hit;
    hit.inst   = inst;
    hit.mat    = mat;
    hit.i0 = i0; hit.i1 = i1; hit.i2 = i2;
    hit.w      = w;
    hit.pos    = wpos;
    hit.N      = N;
    hit.meshUV = hitUV;
    hit.t      = hitT;
    float2 uvGx, uvGy;
    rtHitGrad(hit, rdRayDx, rdRayDy, uvGx, uvGy);
    AverSurface s = rtHitSurface(hit, -dir, L, uvGx, uvGy, AVER_RT_HIT_FULL);
#if AVER_RD_SPLIT
    // Path Tracing, Reference mode: the pixel is one fresh path (CSRdPtRef, read below); the staged
    // lighting it would otherwise read was not traced this frame.
    const bool ptRef = ptReferenceMode();
#else
    const bool ptRef = false;
#endif
#if AVER_RD_SPLIT && !AVER_RD_SINGLE_PASS
    // THE PIXEL'S LIGHTS (UNIFIED_LIGHTS.md), as CSRdShadow chose them: the exact light with its full visibility
    // (sunVis, from gRdSunVisTex) and every other light exact times the tail's filtered shadow fraction.
    // `sun` stays only as nrd2ShadeSplit's argument for the indirect terms, with no radiance of its own.
    AverLight sun;
    sun.direction  = L;
    sun.radiance   = 0.0;
    sun.visibility = 0.0;
    const float4 lightSlots = gRdLocalOut[uint2(i.pos.xy)];
#if AVER_NRD2
    // The lit terms go to NRD2's buckets; `radiance` keeps only what is never denoised.
    float3 nrdD = 0.0, nrdS = 0.0;
    float4 nrdDOut = 0.0, nrdSOut = 0.0;   // what was written to NRD2's D and S (Path Tracing accumulates them)
    float3 radiance = 0.0;
    if (!ptRef && lightSlots.x >= 0.0) {
        rdListLightSplit((uint)lightSlots.x, s, wpos, sunVis, nrdD, nrdS);
        rdTailLightsSplit(s, wpos, (uint)lightSlots.x, rdLocalVisFiltered(uint2(i.pos.xy)), nrdD, nrdS);
    }
#else
    float3 radiance = 0.0;
    if (!ptRef && lightSlots.x >= 0.0) {
        radiance += rdListLight((uint)lightSlots.x, s, wpos, sunVis);
        radiance += rdTailLights(s, wpos, (uint)lightSlots.x, rdLocalVisFiltered(uint2(i.pos.xy)));
    }
#endif
#else
    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;
    // Caustic term, ray-driven twin.
    sun.visibility *= 1.0 + averCausticFocus(wpos);
#if AVER_NRD2
    float3 nrdD = 0.0, nrdS = 0.0;
    float4 nrdDOut = 0.0, nrdSOut = 0.0;
    float3 radiance = 0.0;
#else
    float3 radiance = averShadeDirect(0.0, s, sun);
#endif
#endif

#if !AVER_RD_SPLIT && AVER_RD_LAMPS
    // Single pass resolves on its own hit.
    if (rdLocalLampsLive())
        radiance += rdLocalLightsShade(s, wpos,
                                       rdLocalLightsVisibility(wpos, N, i.pos.xy, uint2(i.pos.xy), true));
#endif

    // Environment through engine's indirect term, not diffuse-only. The SHADING normal, as raster
    // (averShadingNormal): a normal map moves the reflection and the sky it sees.
    float3 R = reflect(dir, s.N);
    AverIndirect ind;
    ind.ambient      = averSkyIrradiance(s.N);
    ind.ambientScale = gAmbient.r;
    ind.specularTraced = 0.0;

    // ---- Diffuse indirect: cone trace as PSMainVoxi does ----
    float rdAo  = 1.0;
    ind.diffuse = 0.0;
    bool giDiffusePoisoned = false;
    bool rdRestirSuppliedDiffuse = false;
    bool rdAoGathered = false;
#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no cone gather
#else
    // GI mode switch: gGiRestirParams.x says whether ReSTIR is wanted.
    if (gVoxelParams.w > 0.5 && !ptRef) {
        if (gGiRestirParams.x > 0.5) {
#if AVER_RD_SPLIT
            // Stage B: read CSRdGi's already-resolved estimate.
            ind.diffuse = gRdGiTex[uint2(i.pos.xy)].rgb;
#else
            ind.diffuse = giRestirIndirect(wpos, N, mul(float4(wpos, 1.0), gViewProj).w,
                                           i.pos.xy, (uint)gRtHistParams.z, rdAo);
#endif
            giDiffusePoisoned = aver_IsGiRestirPoisonColour(ind.diffuse);
            rdRestirSuppliedDiffuse = true;
        } else {
            ind.diffuse = coneTracedIndirect(wpos, N, rdAo);
            rdAoGathered = true;
        }
    }
#endif

    // ---- Environment specular: mirror ray gated as PSMainVoxi gates one ----
    // Gate: gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75
    // Both terms always true when this pass runs (rayDrivenActive() requires rtActive_).
    bool giPoisonSpecCeilHit = false;
#if AVER_RD_SPLIT
    // Stage B: read CSRdRefl's already-resolved reflection.
    // Gate lacks roughness term: rdRefl.a encodes CSRdRefl's own gate decision.
    const float4 rdRefl = (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && !ptRef)
                         ? gRdReflTex[uint2(i.pos.xy)] : float4(0.0, 0.0, 0.0, 0.0);
    if (rdRefl.a > 0.5) {
        // CSRdRefl's PRE-clamp ceiling test in alpha (2.0 = over ceiling).
        giPoisonSpecCeilHit = rdRefl.a > 1.5;
        ind.specular = rdRefl.rgb;
        ind.specularTraced = 1.0;
    } else if (gVoxelParams.w > 0.5 && !ptRef) {
        // Voxel-cone fallback (rough > 0.75 or RT unavailable).
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
#if AVER_RD_ABLATE == AVER_RD_ABL_SPECCONE
        // ablated: no specular cone (fully opaque so skyWeight measures cone alone).
        float4 sceneSpec    = float4(0.0, 0.0, 0.0, 1.0);
#else
        float4 sceneSpec    = traceCone(wpos, R, specAperture);
#endif
        // Skip march when occluded (HLSL doesn't short-circuit).
        const float skyWeight = 1.0 - sceneSpec.a;
        ind.specular        = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
#if AVER_RD_ABLATE == AVER_RD_ABL_ROUGHSKY
        // ablated: atmosphere march
#else
        if (skyWeight > 0.004) ind.specular += skyColor(R) * skyWeight;
#endif
    } else {
        ind.specular        = skyColor(R);
    }
#else
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(i.pos.xy);
    if (rtReflTraced) {
        const float rdReflDzdx = mul(float4(rdRayDx, 0.0), gViewProj).w;
        const float rdReflDzdy = mul(float4(rdRayDy, 0.0), gViewProj).w;
        bool specHit = false;
#if AVER_RD_ABLATE == AVER_RD_ABL_REFL || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        float3 refl = float3(0.0, 0.0, 0.0);   // ablated: no mirror ray
#else
        // N stands in: geometric normal rebuild costs vertices; CSRdRefl uses real one.
        float3 refl = rtReflectionTemporal(wpos, N, N, R, L, i.pos.xy, s.rough,
                                           rdReflDzdx, rdReflDzdy, specHit);
#endif
        const float skyW = 0.0;
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march
#else
        if (!specHit) skyR = skyColor(R);
#endif
        // Clamped: this was the only unbounded term left in shadowed pixels.
        // clamp() not min(): min bounds above only, and negative radiance historically rendered wrong.
        const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
        giPoisonSpecCeilHit = any(specRaw >= AVER_VOX_MAXRAD);
        ind.specular = clamp(specRaw, 0.0, AVER_VOX_MAXRAD);
        ind.specularTraced = 1.0;
    } else if (gVoxelParams.w > 0.5) {
        // Voxel-cone fallback (rough > 0.75 or RT unavailable).
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
#if AVER_RD_ABLATE == AVER_RD_ABL_SPECCONE
        // ablated: no specular cone (fully opaque so this mode measures cone alone).
        float4 sceneSpec    = float4(0.0, 0.0, 0.0, 1.0);
#else
        float4 sceneSpec    = traceCone(wpos, R, specAperture);
#endif
        // Skip march when occluded (HLSL doesn't short-circuit).
        const float skyWeight = 1.0 - sceneSpec.a;
        ind.specular        = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
#if AVER_RD_ABLATE == AVER_RD_ABL_ROUGHSKY
        // ablated: atmosphere march (mode 4 covers only reflection branch).
#else
        if (skyWeight > 0.004) ind.specular += skyColor(R) * skyWeight;
#endif
    } else {
        ind.specular        = skyColor(R);
    }
#endif
    // Real AO from cone march, used to be hardcoded 1.0.
#if AVER_RT && AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    ind.occlusion    = gAmbientParams.x > 0.5
#if AVER_RD_SPLIT
                     // Stage B: read CSRdSkyOcc's resolved answer if it ran (ReSTIR supplies diffuse or no voxel GI).
                     ? ((gGiRestirParams.x > 0.5 || gVoxelParams.w <= 0.5)
                        ? gRdAoTex[uint2(i.pos.xy)].r
                        : rtSkyOcclusionTemporal(wpos, N, i.pos.xy, (uint)gAmbientParams.x, rdAo,
                                                 rdAoGathered, false))
#else
                     // false: G-buffer is OFF, gDenoisedAo is stale. Reading it overrode correct traces.
                     ? rtSkyOcclusionTemporal(wpos, N, i.pos.xy, (uint)gAmbientParams.x, rdAo,
                                              rdAoGathered, false)
#endif
                     : rdAo;
#else
    // ablated (or no ray tracing): cone gather's own occlusion.
    ind.occlusion    = rdAo;
#endif
    // Apply ReSTIR correction: subtract sky ambient where ReSTIR supplied diffuse.
    if (rdRestirSuppliedDiffuse && !giDiffusePoisoned && ((uint)gAmbientParams.z & 2u) == 0u)
        ind.diffuse -= ind.ambient * ind.ambientScale * ind.occlusion * s.occlusion * gVoxelParams.y;
    const float aoView = ind.occlusion * s.occlusion;   // ViewDebug::AmbientOcclusion (vmode 6)
#if AVER_NRD2
    nrd2ShadeSplit(s, sun, ind, nrdD, nrdS);
    radiance = s.emissive;
    {
        // Demodulated and written now, so only the clean colour stays live through fog and glass.
        // a: D's albedo is usable (0 for metals, black and unlit, which the pyramid skips); S's hit
        // distance this frame (cm, 0 = none; < 0 = unlit, skipped).
        float3 FssEss, FmsEms, kD;
        averIndirectFactors(s, FssEss, FmsEms, kD);
        const bool  lit   = s.model != AVER_MODEL_UNLIT && !ptRef;
        const float hitS  = rdRefl.a > 0.5 ? gNrd2SpecOut[rdPixel].a : 0.0;
        const float kdMax = max(s.kdAlbedo.r, max(s.kdAlbedo.g, s.kdAlbedo.b));
        nrdDOut = lit ? float4(nrdD / max(s.kdAlbedo, 1e-3), kdMax > 0.01 ? 1.0 : 0.0) : 0.0;
        nrdSOut = lit ? float4(nrdS / max(FssEss, 1e-3), max(hitS, 0.0)) : float4(0.0, 0.0, 0.0, -1.0);
        gNrd2DiffOut[rdPixel] = nrdDOut;
        gNrd2SpecOut[rdPixel] = nrdSOut;
    }
#else
    radiance = averShadeIndirect(radiance, s, ind);
#endif
#if AVER_RD_SPLIT
    if (ptRef) radiance = gRdGiTex[uint2(i.pos.xy)].rgb;   // CSRdPtRef's path for this pixel
#endif

#if AVER_NRD2
    // Fog is affine (c * ext + in, averFogTermsAirVis): the denoised lighting takes ext at compose.
    float3 nrdFogExt = 1.0, nrdFogIn = 0.0;
#if AVER_RD_ABLATE == AVER_RD_ABL_FOG
#elif AVER_RD_ABLATE == AVER_RD_ABL_AERIAL
    averFogTermsAirVis(wpos, false, voxiAirVisibility(wpos), nrdFogExt, nrdFogIn);
#else
    averFogTermsAirVis(wpos, true, voxiAirVisibility(wpos), nrdFogExt, nrdFogIn);
#endif
    radiance = radiance * nrdFogExt + nrdFogIn;
#elif AVER_RD_ABLATE == AVER_RD_ABL_FOG
    // ablated: no aerial perspective and no fog inscatter march.
#elif AVER_RD_ABLATE == AVER_RD_ABL_AERIAL
    // ablated: aerial march only (height fog still runs).
    radiance = averApplyFogAirVis(radiance, wpos, false, voxiAirVisibility(wpos));
#else
    radiance = averApplyFogAirVis(radiance, wpos, true, voxiAirVisibility(wpos));
#endif
#if AVER_RD_SPLIT
    // Glass and every other translucent material, inside the path (rdTranslucentPath).
    float3 rdGlassThr = 1.0;
    if (gPtBounceParams.w > 0.5 && vmode == 0u) radiance = rdTranslucentPath(dir, hitT, radiance, i.pos.xy, rdGlassThr);
#endif
#if AVER_NRD2
    // 0 wherever the colour below is replaced (unlit, debug views, overrides): nothing is added there.
    float nrdKeep = (vmode == 0u && s.model != AVER_MODEL_UNLIT && !ptRef) ? 1.0 : 0.0;
#endif

    // Depth for deferred sky, transparentPass, particles (otherwise they sort against cleared buffer).
    float4 clip = mul(float4(wpos, 1.0), gViewProj);
    o.depth = clip.w > 1e-6 ? saturate(clip.z / clip.w) : 1.0;
    // Unlit: substitutes colour only. Handled here, not through gShadingModel, because ray hit has no per-draw cbuffer.
    // Override: all AVER_GBUFFER channels must be written. s.albedo is sampled base colour.
    // Unlit is albedo plus emissive (removes lighting, not surface's own light).
    o.col   = float4(vmode == 1u ? s.albedo + s.emissive : radiance, 1.0);
    // Applied last, after unlit and fog, so unconditionally final when fires (see PSMainVoxi for precedence).
    if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned) {
        o.col.rgb = float3(0.55, 0.0, 1.0);   // Violet: ray-traced specular ceiling hit
#if AVER_NRD2
        nrdKeep = 0.0;
#endif
    }
    // vmode 2-5 (ViewDebug): replace colour last, after overrides above.
    if (vmode == 6u)
        o.col.rgb = aoView.xxx;
    else if (vmode >= 2u)
        o.col.rgb = viewDebugColor(vmode, rdInstanceIndex, inst.materialIndex, rdPrimIndex, hitT, N, dir);
#if AVER_RD_SPLIT
    // NeuRaC visualiser (gAmbientParams.w bits 8-10, set with live-cache bit 128).
    if (((uint)gAmbientParams.w & 128u) != 0u && (((uint)gAmbientParams.w >> 8) & 7u) != 0u &&
        gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5) {
        o.col.rgb = gRdGiTex[uint2(i.pos.xy)].rgb;
#if AVER_NRD2
        nrdKeep = 0.0;
#endif
    }
#if AVER_NRD2
    {
        // Remodulation: the denoised D' and S' come back as D' * Rd + S' * Rs at compose, under the
        // same fog extinction and glass throughput the clean colour got.
        float3 FssEss, FmsEms, kD;
        averIndirectFactors(s, FssEss, FmsEms, kD);
        const float3 M = nrdFogExt * rdGlassThr * nrdKeep;
        gNrd2RemodAOut[rdPixel] = float4(M * s.kdAlbedo, M.x * FssEss.x);
        gNrd2RemodBOut[rdPixel] = M.yz * FssEss.yz;
    }
#endif
    // PATH TRACING: progressive accumulation. While the CPU key holds (gPtBounceParams.y == 2) each
    // pixel keeps a running mean of its frames, capped at gPtBounceParams.z frames (a moving average
    // after that). A pixel restarts on its own when the surface under it moves or its depth changes.
#if AVER_NRD2
    // Under NRD2 the noisy halves accumulate instead, before NRD2 reads them: demodulated D in u22's first
    // plane (with the count and depth in w), S in its second; the composed colour is assembled at compose.
    // Same keep rule as below. While a pixel keeps (gPtBounceParams.y == 2) the network is off for the frame
    // (NEURAA_NRD.md rule 10: it never sees history). docs/rendering/NRD2.md "Path Tracing accumulation".
    if (gPtBounceParams.y > 0.5) {
        uint accCount, accStride;
        gPtAccum.GetDimensions(accCount, accStride);
        const uint   plane  = accCount / 2u;
        const uint   accIdx = rdPixel.y * rdPitch + rdPixel.x;
        const float  depthM = clip.w * 0.01;
        const float4 prevD  = gPtAccum[accIdx];
        const float4 prevS  = gPtAccum[plane + accIdx];
        const uint   packed = asuint(prevD.w);
        const float  n      = (float)(packed & 0xFFFFu);
        const float  prevZ  = f16tof32(packed >> 16);
        const bool   moved  = dot(gAverReprojDelta, gAverReprojDelta) > 1e-4;
        const bool   use    = nrdKeep > 0.0 && accIdx < plane;
        const bool   keep   = use && gPtBounceParams.y > 1.5 && !moved && n > 0.0 && all(isfinite(prevD.rgb)) &&
                              all(isfinite(prevS.rgb)) && abs(prevZ - depthM) <= depthM * 0.01 + 0.01;
        const float  nn     = use ? (keep ? min(n + 1.0, gPtBounceParams.z) : 1.0) : 0.0;
        const float3 accD   = keep ? lerp(prevD.rgb, nrdDOut.rgb, 1.0 / nn) : nrdDOut.rgb;
        const float3 accS   = keep ? lerp(prevS.rgb, nrdSOut.rgb, 1.0 / nn) : nrdSOut.rgb;
        if (accIdx < plane && all(isfinite(accD)) && all(isfinite(accS))) {
            gPtAccum[accIdx]         = float4(accD, asfloat((f32tof16(depthM) << 16) | (uint)nn));
            gPtAccum[plane + accIdx] = float4(accS, 0.0);
            if (keep) {
                gNrd2DiffOut[rdPixel] = float4(accD, nrdDOut.a);
                gNrd2SpecOut[rdPixel] = float4(accS, nrdSOut.a);
            }
        }
    }
#else
    if (gPtBounceParams.y > 0.5 && vmode == 0u) {
        const uint   accIdx = rdPixel.y * rdPitch + rdPixel.x;
        const float  depthM = clip.w * 0.01;
        const float4 prev   = gPtAccum[accIdx];
        const uint   packed = asuint(prev.w);
        const float  n      = (float)(packed & 0xFFFFu);
        const float  prevZ  = f16tof32(packed >> 16);
        const bool   moved  = dot(gAverReprojDelta, gAverReprojDelta) > 1e-4;
        const bool   keep   = gPtBounceParams.y > 1.5 && !moved && n > 0.0 && all(isfinite(prev.rgb)) &&
                              abs(prevZ - depthM) <= depthM * 0.01 + 0.01;
        const float  nn     = keep ? min(n + 1.0, gPtBounceParams.z) : 1.0;
        const float3 acc    = keep ? lerp(prev.rgb, o.col.rgb, 1.0 / nn) : o.col.rgb;
        if (all(isfinite(acc))) {
            gPtAccum[accIdx] = float4(acc, asfloat((f32tof16(depthM) << 16) | (uint)nn));
            o.col.rgb = acc;
        }
    }
#endif
#endif
#if AVER_GBUFFER
    // clip.w is view-space linear depth (reused from o.depth divide).
    // Velocity carries object motion: hit's object-space point through this frame and last frame's objectToWorld.
    // Rigid motion only (skinned/soft-body deformation not in prev).
    // Normal is ray-hit N, not interpolated vertex normal.
    const float3 objPos  = gRtVerts[i0].pos * w.x + gRtVerts[i1].pos * w.y + gRtVerts[i2].pos * w.z;
    const float3 curObjW = mul(float4(objPos, 1.0), inst.objectToWorld).xyz;
    const float3 prvObjW = mul(float4(objPos, 1.0), inst.prevObjectToWorld).xyz;
    o.velocity        = averGBufferVelocityMoved(wpos, wpos + (prvObjW - curObjW));
    o.viewZ            = clip.w;
    // The surface's own normal, not the normal-mapped one: the GI the denoiser filters is gathered over
    // the geometric hemisphere, and per-texel normal-map detail broke its neighbour weights into speckle.
    o.normalRoughness  = averPackNormalRoughness(N, s.rough);
#endif
    return o;
}

// Pixel-centre NDC (inverse of ndc->pixel mapping used everywhere).
// gSceneViewportCur, not gSceneViewport (latter is last frame's rect for reprojection).
// Factored so future mapping changes update one place.
float3 rdPrimaryRayDir(uint2 pixel, out float2 ndc) {
    const float2 pxC = float2(pixel) + 0.5;
    ndc.x = (pxC.x - gSceneViewportCur.x) / max(gSceneViewportCur.z, 1.0) * 2.0 - 1.0;
    ndc.y = 1.0 - (pxC.y - gSceneViewportCur.y) / max(gSceneViewportCur.w, 1.0) * 2.0;

    // NDC-to-world-ray reconstruction as PSRayDriven's primary ray.
    return averViewRayDir(ndc);
}

// Cutout policy per staged ray type: alpha-test budget and whether cutouts are solid.
#define AVER_RD_CUTOUTS_PRIMARY   24u
#define AVER_RD_CUTOUTS_SHADOW     8u
#define AVER_RD_CUTOUTS_DIFFUSE    4u
#define AVER_RD_CUTOUTS_REFL       8u
#define AVER_RD_REFL_SOLID_CUTOUT_ROUGH 0.3

// ---- Stage A: CSRdVisibility -- trace primary ray, write visibility record ----
// Visibility-only half of PSRayDriven's trace block (AVER_RD_SPLIT==0 branch).
// D3D12 only; VoxiRenderer decides whether to dispatch (falls back to single pass otherwise).
[numthreads(8, 8, 1)]
void CSRdVisibility(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    // Bail if record buffer has no pitch (VoxiRenderer only writes nonzero pitch when using staged path).
    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RayDesc r;
    r.Origin    = gCamPos.xyz;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = 1.0e7;

    // Primary ray draws the leaf: every cutout tested up to budget.
    averRtCutoutPolicy(AVER_RD_CUTOUTS_PRIMARY, false);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Same lane as PSRayDriven's primary ray (AVER_RT_MASK_OPAQUE, leaves viewer's head).
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE, r);
    averRtProceedSolid(q);

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        gRdVisBuf[idx] = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
        return;
    }

    const float2 bary = q.CommittedTriangleBarycentrics();
    gRdVisBuf[idx] = uint4(rtPackCommitted(q), q.CommittedPrimitiveIndex(),
                           asuint(bary.x), asuint(bary.y));
}

// Shadow-ray footprint factored out so CSRdShadowProbe can build same dpx/dpy as fresh probe.
// gSceneViewport matches PSRayDriven's copy of this step.
void rdShadowFootprint(float2 ndc, float3 dir, RdSurface s, out float3 dpx, out float3 dpy) {
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float3 dirDx = averViewRayDir(ndc + float2(ndcPixelStep.x, 0.0));
    float3 dirDy = averViewRayDir(ndc + float2(0.0, ndcPixelStep.y));
    const float3 rdRayDx = (dirDx - dir) * s.hitT;
    const float3 rdRayDy = (dirDy - dir) * s.hitT;
    dpx = rdRayDx - s.N * dot(rdRayDx, s.N);
    dpy = rdRayDy - s.N * dot(rdRayDy, s.N);
}

// Sun shadow split: one mask per 8x8 thread group (gRdShadowTiles tile), zeroed and OR'd by CSRdShadowProbe.
groupshared uint gRdShadowProbeMask;

// ---- Stage S0: CSRdShadowProbe -- one probe ray per pixel, reduced to one mask per tile ----
// Dispatched over same grid as CSRdShadow immediately before: one thread group is one 8x8 tile.
// Each thread traces at most one probe ray, group reduces 64 answers to one mask.
// CSRdShadow's tiled compile ORs this tile's mask with 3x3 neighbourhood and skips rays where neighbours agree.
// No early return before or between barriers: undefined behaviour. Out-of-viewport/pitch-0/sky cases leave bit at 0.
[numthreads(8, 8, 1)]
void CSRdShadowProbe(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gidx : SV_GroupIndex) {
    uint bit = 0u;   // 0 = this thread has no vote (out of viewport / pitch 0 / sky pixel)

    const bool inView = all(tid.xy < (uint2)gSceneViewportCur.zw);
    const uint pitch   = rdRowPitch();
    if (inView && pitch != 0u) {
        const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;
        const uint  idx   = pixel.y * pitch + pixel.x;
        const uint4 rec   = gRdVisBuf[idx];
        if (rec.x != 0xFFFFFFFFu) {
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
            bit = 2u;   // ablated: fully lit, no ray
#else
            if (!rdSunLit()) {
                bit = 1u;   // sun radiance 0: visibility cannot show; CSRdShadow skips its rays too
            } else {
                float2 ndc;
                const float3 dir = rdPrimaryRayDir(pixel, ndc);
                const RdSurface s = rdSurfaceFromRecord(rec, dir);
                averRtCutoutPolicy(AVER_RD_CUTOUTS_SHADOW, false);
                float3 dpx, dpy;
                rdShadowFootprint(ndc, dir, s, dpx, dpy);

                const float3 L = normalize(gLightDir.xyz);
                // Same jitter rtShadowTemporal's non-tiled branch passes to agree with CSRdShadow's fresh trace.
                const float jitter = (gRtHistParams.x < 0.5) ? 0.0
                                    : averGoldenTurns((uint)gRtHistParams.z);
                // Which sample the probe traces: rotated by pixel/frame to cover all radii in every tile.
                // Rotating keeps row/column neighbours on different samples.
                const uint rays   = (uint)max(gRtParams.y, 1.0);
                const uint kProbe = (pixel.x + 3u * pixel.y + (uint)gRtHistParams.z) % rays;
                // From same side CSRdShadow will trace from (subsurface handling).
                gAverShadowOriginPush = averSubsurfaceShadowPush(s.mat.flags, s.mat.subsurfaceRadius, s.N, L);
                const float3 fresh = rtShadowEx(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy, 1u, jitter,
                                                kProbe);
                gAverShadowOriginPush = float3(0.0, 0.0, 0.0);

                if (all(fresh == 0.0))      bit = 1u;   // fully blocked
                else if (all(fresh == 1.0)) bit = 2u;   // fully lit
                else                        bit = 4u;   // penumbra or tinted hit
            }
#endif
        }
    }

    if (gidx == 0u) gRdShadowProbeMask = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (bit != 0u) InterlockedOr(gRdShadowProbeMask, bit);
    GroupMemoryBarrierWithGroupSync();
    if (gidx == 0u) {
        const uint tilesX = ((uint)gSceneViewportCur.z + 7u) / 8u;
        gRdShadowTiles[gid.y * tilesX + gid.x] = gRdShadowProbeMask;
    }
}

// ---- Stage S: CSRdShadow -- reconstruct surface, resolve sun shadow ----
// Reads CSRdVisibility's record, rebuilds surface, runs same rtShadowTemporal as PSRayDriven's single pass.
// Same pixel-centre, footprint, ray count: history buffer means same thing either way.
// Compiled at SM 6.6: rtShadowTemporal needs ddx/ddy of depth (derivatives from 6.6+ in compute).
[numthreads(8, 8, 1)]
void CSRdShadow(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // Sky pixel: no surface. Set alpha 0.0 as miss sentinel (not a depth). No lights.
        gRdSunVisTex[pixel] = float4(1.0, 1.0, 1.0, 0.0);
        gRdLocalOut[pixel]  = float4(-1.0, -1.0, 0.0, 1.0);
        return;
    }

    // Pixel-centre NDC/primary-ray reconstruction for shadow ray footprint.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_SHADOW, false);

    // Shadow-ray footprint: same as PSRayDriven's, factored for CSRdShadowProbe.
    float3 dpx, dpy;
    rdShadowFootprint(ndc, dir, s, dpx, dpy);

    // THE VISIBLE SURFACE'S LIGHTS (docs/rendering/UNIFIED_LIGHTS.md). Every emitter is a list entry, the sun its
    // directional one. The entry that delivers the most here is the pixel's EXACT light and gets the sun's full shadow
    // kernel (disc, history keyed by light, probe tiles). Every other light (the tail) is shaded exactly in Stage B
    // times one shadow fraction, which CSRdTailVis traces next at quarter resolution. By day the exact light is the
    // sun because it delivers the most.
    // STABLE EXACT LIGHT: last frame's exact light at this pixel (by its key, gRdLocalOut.y, not its list index, which
    // moves as the list reorders) stays exact while it delivers at least 80% of the strongest, so near-equal candles do
    // not trade places (each swap restarts the exact light's shadow history).
    const float prevKey = rdLocalHistValid() ? gRdLocalHist.Load(int3(pixel, 0)).y : -1.0;
    uint  prevE0 = 0xFFFFFFFFu;
    uint  e0 = 0xFFFFFFFFu;
    float w0 = 0.0, wPrev = 0.0;
    {
        const RdLightRange lr = rdLightsAt(s.wpos);
        [loop] for (uint k = 0u; k < lr.count; ++k) {
            const uint  j = rdLightIndex(lr, k);
            const float w = rdLightWeight(gRdLocalLights[j], s.wpos, s.N);
            if (!(w > 0.0)) continue;
            if (w > wPrev && prevKey >= 0.0 && rdLightKey(gRdLocalLights[j]) == prevKey) { prevE0 = j; wPrev = w; }
            if (w > w0) { e0 = j; w0 = w; }
        }
        if (prevE0 != 0xFFFFFFFFu && wPrev >= 0.8 * w0) { e0 = prevE0; w0 = wPrev; }
    }
    const bool  haveE0 = e0 != 0xFFFFFFFFu;
    const float3 L = haveE0 ? rdSetShadowLight(gRdLocalLights[e0], s.wpos) : normalize(gLightDir.xyz);
    const bool  e0Directional = haveE0 && aversLightKind(gRdLocalLights[e0]) == AVER_LIGHT_DIRECTIONAL;
    const bool  e0NoShadow    = haveE0 && aversLightNoShadow(gRdLocalLights[e0]);
    // The tier's ray count (up to 8) is the sun's; a lamp's small penumbra takes at most 2.
    const uint  e0Rays = (uint)max(gRtParams.y, 1.0) > 2u && haveE0 && !e0Directional ? 2u : (uint)max(gRtParams.y, 1.0);

    // History writes always live for this pass: blended draws never reach ray-driven primary.
    gAverHistoryWrite = true;

#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    const float3 sunVis = float3(1.0, 1.0, 1.0);   // ablated: fully lit
#else
    // Subsurface hit lit from behind pushes the origin.
    gAverShadowOriginPush = averSubsurfaceShadowPush(s.mat.flags, s.mat.subsurfaceRadius, s.N, L);
#if AVER_RD_SHADOW_TILES
    // Probe-guided skip: classify 3x3 tile neighbourhood, skip ray if uniform (all lit or all blocked).
    const uint tilesX = ((uint)gSceneViewportCur.z + 7u) / 8u;
    const uint tilesY = ((uint)gSceneViewportCur.w + 7u) / 8u;
    const uint2 tile  = tid.xy / 8u;
    uint m = 0u;
    [unroll] for (int oy = -1; oy <= 1; ++oy) {
        [unroll] for (int ox = -1; ox <= 1; ++ox) {
            const uint nx = (uint)clamp((int)tile.x + ox, 0, (int)tilesX - 1);
            const uint ny = (uint)clamp((int)tile.y + oy, 0, (int)tilesY - 1);
            m |= gRdShadowTiles[ny * tilesX + nx];
        }
    }
    // One call with verdict as runtime flag, not ?: between two calls (avoids double-inlining).
    // The probe traced the directional light, so its verdict only stands for a directional exact light. No exact
    // light (nothing reaches this point) or a no-shadow one: no ray; the stand-in keeps the history writes.
    const bool   probeAgrees = !haveE0 || e0NoShadow || (e0Directional && (m == 2u || m == 1u));
    const float3 sunVis = rtShadowTemporalEx(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy,
                                             e0Rays, probeAgrees,
                                             float3(1.0, 1.0, 1.0) * ((e0NoShadow || (e0Directional && m == 2u)) ? 1.0 : 0.0));
#else
    const bool   noRay  = !haveE0 || e0NoShadow;
    const float3 sunVis = rtShadowTemporalEx(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy,
                                             e0Rays, noRay,
                                             float3(1.0, 1.0, 1.0) * (e0NoShadow ? 1.0 : 0.0));
#endif
    gAverShadowOriginPush = float3(0.0, 0.0, 0.0);
#endif
    rdResetShadowLight();
    // gRdLocalOut = (exact light's index, its key, unused, tail shadow fraction). .a is filled by CSRdTailVis at
    // quarter resolution; -1 until then (and where it leaves a pixel untraced, Stage B's 5x5 fills it).
    gRdLocalOut[pixel] = float4(haveE0 ? (float)e0 : -1.0, haveE0 ? rdLightKey(gRdLocalLights[e0]) : -1.0, 0.0, -1.0);
    // Primary surface linear view depth (for blended-replay reuse test).
    const float rdSunVisViewZ = mul(float4(s.wpos, 1.0), gViewProj).w;
    gRdSunVisTex[pixel] = float4(sunVis, rdSunVisViewZ);
    // NRD2 half-rate fill (rtGiShadowBits 64): CSRdHalfFill's normal guide, in u3 (NRD2's D target,
    // unused until Stage B).
    if ((rtGiShadowBits() & 64u) != 0u) gRtReflHistOut[pixel] = float4(s.N, 1.0);
}


// ---- STAGE S2: CSRdTailVis -- the tail lights' shadow fraction at quarter resolution (UNIFIED_LIGHTS.md) ----
// One thread per 2x2 block, so a wave's rays go to the same lights in the same order (coherent). The block's first
// surface pixel ranks the lights reaching it (all but its exact light, gRdLocalOut.x from CSRdShadow); the strongest
// lightRaysPerBlock get one ray each, every frame, nothing picked at random. Their irradiance-weighted unblocked share
// is the tail's fraction, blended with history by the sun's rule (FidelityFX mode; NRD2 frames have none), and
// written to every pixel of the block at the same depth; a pixel across an edge gets -1 and Stage B's depth-aware
// 5x5 (rdLocalVisFiltered) fills it. Lights under 1/128 of the tail's total are skipped.
[numthreads(8, 8, 1)]
void CSRdTailVis(uint3 tid : SV_DispatchThreadID) {
    const uint2 vp = (uint2)gSceneViewportCur.zw;
    if (any(tid.xy * 2u >= vp)) return;
    const uint2 base  = (uint2)gSceneViewportCur.xy + tid.xy * 2u;
    const uint  pitch = rdRowPitch();
    if (pitch == 0u) return;
    // The representative: the block's first pixel with a surface.
    uint2 rep = base;
    bool  found = false;
    [unroll] for (uint q = 0u; q < 4u; ++q) {
        const uint2 p = base + uint2(q & 1u, q >> 1u);
        if (!found && all(p - (uint2)gSceneViewportCur.xy < vp) && gRdVisBuf[p.y * pitch + p.x].x != 0xFFFFFFFFu) {
            rep = p;
            found = true;
        }
    }
    float2 ndc;
    const float3 dir = rdPrimaryRayDir(rep, ndc);
    const RdSurface s = rdSurfaceFromRecord(gRdVisBuf[rep.y * pitch + rep.x], dir);
    const float2 repC = float2(rep) + 0.5;
    // History first, behind constant-buffer conditions only (rtReprojectTexel takes derivatives). > 0.25, not the
    // sun's 0.75: the "sun moved" state leaves lamp shadows valid.
    int2   texel = int2(0, 0);
    float2 velocityPx = float2(0.0, 0.0);
    bool   haveHist = false;
    if (gRtHistParams.x > 0.5 && gRtHistParams.y > 0.25 && rdLocalHistValid())
        haveHist = rtReprojectTexel(s.wpos, repC, texel, velocityPx);
    float hist = -1.0;
    if (haveHist) hist = rdLocalHistBilinear(repC + velocityPx, texel);
    if (!found) return;

    averRtCutoutPolicy(AVER_RD_CUTOUTS_SHADOW, false);
    const float e0f = gRdLocalOut[rep].x;
    const uint  e0  = e0f >= 0.0 ? (uint)e0f : 0xFFFFFFFFu;
    const RdLightRange lr = rdLightsAt(s.wpos);
    RdTopWeights top = rdTopInit();
    float twS = 0.0;
    [loop] for (uint k = 0u; k < lr.count; ++k) {
        const float w = rdTailWeight(rdLightIndex(lr, k), e0, s.wpos, s.N);
        if (!(w > 0.0)) continue;
        twS += w;
        rdTopAdd(top, w);
    }
    float vis = 1.0;
    if (twS > 0.0) {
        const float floorW = twS * (1.0 / 128.0);
        const uint  budget = rdLightRaysPerBlock();
        const uint  turn   = (uint)gRtHistParams.z;
        float sumW = 0.0, sumV = 0.0;
        [loop] for (uint k = 0u; k < lr.count; ++k) {
            const uint  j = rdLightIndex(lr, k);
            const float w = rdTailWeight(j, e0, s.wpos, s.N);
            if (!(w > floorW) || rdTopCountAbove(top, w) >= budget) continue;
            const float3 Lt = rdSetShadowLight(gRdLocalLights[j], s.wpos);
            const float  v  = averShadowLum(rtShadowEx(s.wpos, s.N, Lt, repC, float3(0, 0, 0), float3(0, 0, 0), 1u,
                                                       averGoldenTurns(turn), 0u));
            sumW += w;
            sumV += w * v;
        }
        rdResetShadowLight();
        if (sumW > 0.0) vis = sumV / sumW;
        // The sun's history rule (rtShadowTemporalEx): 0.9 at rest, 0.5 by 32 px/frame, 0.35 where the shadow changed.
        if (haveHist && hist >= 0.0) {
            const float t = saturate(length(velocityPx) / 32.0);
            vis = lerp(vis, hist, rtShadowChanged(vis, hist) ? kAverShadowChangeHistory : lerp(0.9, 0.5, t));
        }
    }
    // Every pixel of the block on the representative's surface (the depth test rdLocalVisTap uses); sky keeps 1.
    const float zc = gRdSunVisTex[rep].a;
    [unroll] for (uint q = 0u; q < 4u; ++q) {
        const uint2 p = base + uint2(q & 1u, q >> 1u);
        if (any(p - (uint2)gSceneViewportCur.xy >= vp)) continue;
        const float zt = gRdSunVisTex[p].a;
        if (zt <= 0.0) continue;
        float4 o = gRdLocalOut[p];
        o.a = abs(zt - zc) <= zc * 0.03 + 1.0 ? vis : -1.0;
        gRdLocalOut[p] = o;
    }
}

// ---- STAGE G0: CSRdGiTrace -- trace ReSTIR GI's fresh candidate for CSRdGi's resample --------
//
// GI candidate trace/resample split (Settings::rayDrivenGiSplit). Runs giTraceInitialCandidate for
// every pixel CSRdGi would trace -- same surface reconstruction, frameJitter, and f2Path/rho2
// (giDecodePaths) -- storing out params and the visibility reconstruction in gRdGiCand. CSRdGi's
// AVER_GI_SPLIT=1 compile reads that record back inside giRestirIndirect, agreeing bit-for-bit.
//
// NON-CHECKERBOARD: one thread per pixel. CHECKERBOARD (milestone 4): compacted, only half-res
// threads. gGiCbSkip forced false here since dispatch by construction covers only the traced half.
[numthreads(8, 8, 1)]
void CSRdGiTrace(uint3 tid : SV_DispatchThreadID) {
#if AVER_GI_CHECKERBOARD
    const uint parity = ((uint)gViewParams.w >> 16) & 1u;
    const uint y       = (uint)gSceneViewportCur.y + tid.y;
    const uint xBase   = (uint)gSceneViewportCur.x + 2u * tid.x;
    const uint x       = xBase + ((xBase ^ y ^ parity) & 1u);
    if (x >= (uint)gSceneViewportCur.x + (uint)gSceneViewportCur.z ||
        y >= (uint)gSceneViewportCur.y + (uint)gSceneViewportCur.w) return;
    const uint2 pixel = uint2(x, y);
    gGiCbSkip = false;
#else
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;
#endif

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    if (!(gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5)) return;

#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    return;   // ablated: no ReSTIR GI candidate
#else
    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) return;   // sky pixel: no surface for CSRdGi to read

    float2 ndc;
    const float3 dir = rdPrimaryRayDir(pixel, ndc);
    const RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_DIFFUSE, true);

    // No gAverHistoryWrite: this dispatch only calls giTraceInitialCandidate (plain rtShadow, no history gate).
    const uint frameIdx = (uint)gRtHistParams.z;
    const GiPathDecode gd = giDecodePaths(s.wpos, s.N, float2(pixel) + 0.5, frameIdx);

    float3 pos, nrm, rad;
    bool   nonFinite   = false;
    float  f2LumTraced = 0.0, f2LumSky = 0.0;
    bool   f2Observed  = false;
    const bool ok = giTraceInitialCandidate(s.wpos, s.N, float2(pixel) + 0.5, averGoldenTurns(frameIdx),
                                            pos, nrm, rad, nonFinite, gd.f2Path, gd.rho2,
                                            f2LumTraced, f2LumSky, f2Observed);

    // Write every out param and the bool: CSRdGi's split read needs a defined record for every pixel.
    RdGiCand cand;
    cand.pos         = pos;
    cand.flags       = (ok ? 1u : 0u) | (nonFinite ? 2u : 0u) | (f2Observed ? 4u : 0u) | (gd.rec.valid ? 8u : 0u);
    cand.nrm         = nrm;
    cand.f2LumTraced = f2LumTraced;
    cand.rad         = rad;
    cand.f2LumSky    = f2LumSky;
    cand.vis         = float4(gd.rec.v3, gd.rec.g, gd.rec.b, gd.rec.motionPx);
    gRdGiCand[idx] = cand;
#endif
}

// ---- STAGE G: CSRdGi -- reconstruct surface, resolve ReSTIR GI's diffuse estimate -----------
//
// Reads CSRdVisibility's record, rebuilds the surface, and runs the same giRestirIndirect call
// PSRayDriven's single pass makes when ReSTIR GI is active -- same pixel-centre and history buffers.
//
// Only dispatched when ReSTIR GI is the chosen estimator. Cone-traced branch runs inside Stage B.
// Compiled at SM 6.6 (same as CSRdShadow).
//
// MILESTONE 4: Also compiled with AVER_GI_CHECKERBOARD=1 (half-rate GI).
// Skip decided here (gGiCbSkip); changes live inside giRestirIndirect.
[numthreads(8, 8, 1)]
void CSRdGi(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

#if AVER_GI_CHECKERBOARD
    // Half-rate GI parity from cbuffer bit 16. Denoiser treats (x ^ y ^ parity) & 1 == 0 as traced.
    const uint giCbParity = ((uint)gViewParams.w >> 16) & 1u;
    gGiCbSkip = ((pixel.x ^ pixel.y ^ giCbParity) & 1u) != 0u;
#endif

    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // Sky: no surface for ReSTIR to bounce off. Stage B already writes GI surface-history sentinel.
        gRdGiTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Same pixel-centre NDC/primary-ray reconstruction as CSRdVisibility.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_DIFFUSE, true);

    // History writes always live for this pass: every write giRestirIndirect makes is live.
    gAverHistoryWrite = true;

#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no ReSTIR GI candidate. gRdGiTex never read back under this ablation.
#else
    // Same arguments as PSRayDriven's non-split copy. `ao` discarded (set to 1.0 first line, never touched).
    if (gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5) {
        float ao;
#if AVER_GI_SPLIT
        // Same row-pitch index CSRdGiTrace wrote -- static, not parameter, keeps signature shared.
        gGiCandIdx = idx;
#endif
        const float3 d = giRestirIndirect(s.wpos, s.N, mul(float4(s.wpos, 1.0), gViewProj).w,
                                          float2(pixel) + 0.5, (uint)gRtHistParams.z, ao);
        gRdGiTex[pixel] = float4(d, 1.0);
    }
#endif
}

// ---- STAGE O: CSRdSkyOcc -- reconstruct surface, resolve traced sky-occlusion ------
//
// Reads CSRdVisibility's record, rebuilds surface, runs same rtSkyOcclusionTemporal as PSRayDriven.
// Same pixel-centre argument, same AO history pair and denoiser hand-off.
// Compiled at SM 6.6 (derivative intrinsics in rtAoSpatial require 8x8 threads in 2x2 quads).
[numthreads(8, 8, 1)]
void CSRdSkyOcc(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;

    // Gate: ReSTIR supplies diffuse (rdAo stays 1.0) or no voxel GI. Cone-GI mode already gathered AO.
    if (!(gAmbientParams.x > 0.5 && (gGiRestirParams.x > 0.5 || gVoxelParams.w <= 0.5))) return;

    const uint idx = pixel.y * pitch + pixel.x;
    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // Sky pixel: unoccluded (same sentinel as rdAo's initial 1.0).
        gRdAoTex[pixel] = float4(1.0, 1.0, 1.0, 1.0);
        return;
    }
    // NRD2 half rate (rtGiShadowBits 128): the checkerboard half CSRdRefl traces is skipped here, a = -1
    // for CSRdHalfFill.
    if ((rtGiShadowBits() & 128u) != 0u && ((pixel.x ^ pixel.y ^ (uint)gRtHistParams.z) & 1u) == 0u) {
        gRdAoTex[pixel] = float4(1.0, 0.0, 0.0, -1.0);
        return;
    }

    // Same pixel-centre NDC/primary-ray reconstruction.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_DIFFUSE, true);

    // History writes always live (same reason as CSRdShadow and CSRdGi).
    gAverHistoryWrite = true;

    // Honour sky-occlusion ablation same as PSRayDriven's branch.
#if AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    // Same arguments as PSRayDriven: coneAo=1.0, coneAoIsGather=false, denoisedAoUsable=false.
    const float occ = rtSkyOcclusionTemporal(s.wpos, s.N, float2(pixel) + 0.5, (uint)gAmbientParams.x,
                                             1.0, false, false);
    gRdAoTex[pixel] = float4(occ, 0.0, 0.0, 1.0);
#else
    // ablated: matches PSRayDriven's fallback (rdAo's initial 1.0).
    gRdAoTex[pixel] = float4(1.0, 0.0, 0.0, 1.0);
#endif
}

// ---- STAGE P: CSRdPtRef -- Path Tracing, Reference mode ----------------------------
//
// One independent path per pixel from the visibility record's surface (ptReferencePixel), into
// gRdGiTex, which Reference mode's skipped GI stage leaves free; Stage B shades the pixel with it. Its
// own pass so the path loop stays out of Stage B's pixel shader (register pressure lost the device on
// this GPU once already: aver-single-pass-tdr).
[numthreads(8, 8, 1)]
void CSRdPtRef(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;
    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint4 rec = gRdVisBuf[pixel.y * pitch + pixel.x];
    if (rec.x == 0xFFFFFFFFu) {
        gRdGiTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);   // sky: Stage B paints it without reading this
        return;
    }
    float2 ndc;
    const float3 dir = rdPrimaryRayDir(pixel, ndc);
    const RdSurface s = rdSurfaceFromRecord(rec, dir);
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0), 2.0 / max(gSceneViewport.w, 1.0));
    const float3 rdRayDx = (averViewRayDir(ndc + float2(ndcPixelStep.x, 0.0)) - dir) * s.hitT;
    const float3 rdRayDy = (averViewRayDir(ndc + float2(0.0, ndcPixelStep.y)) - dir) * s.hitT;
    const AverSurface hs = rdHitSurface(s, dir, rdRayDx, rdRayDy, AVER_RT_HIT_FULL);
    gRdGiTex[pixel] = float4(ptReferencePixel(hs, s.wpos, float2(pixel) + 0.5), 1.0);
}

// ---- STAGE R: CSRdRefl -- reconstruct surface, resolve ray-traced reflection -------
//
// Reads CSRdVisibility's record, rebuilds surface, runs same rtReflectionTemporal as PSRayDriven.
// Same pixel-centre and shadow-ray-shaped footprint, same reflection history pair.
//
// Needs roughness before deciding whether to trace: see rdSurfaceRoughness's header.
// Compiled at SM 6.6 (ddx/ddy in rtReflectionSpatial needs 2x2 quads).
//
// Last frame's denoised reflection at this surface, read where the reflected point was on last frame's
// screen (the denoiser's own parallax reprojection). Leaves `refl` alone when that is off screen.
void rdDenoisedReflection(float3 wpos, float hitT, inout float3 refl) {
    uint w = 0, h = 0;
    gDenoisedRefl.GetDimensions(w, h);
    if (w == 0u || h == 0u || gGiRestirParams.y < 0.5) return;
    const float3 v = wpos - gCamPos.xyz;
    const float  d = max(length(v), 1e-3);
    const float4 c = mul(float4(gCamPos.xyz + v * ((d + hitT) / d), 1.0), gPrevViewProj);
    if (c.w <= 1e-4) return;
    const float2 ndc = c.xy / c.w;
    const float2 px  = gSceneViewport.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    if (any(px < gSceneViewport.xy) || any(px >= gSceneViewport.xy + gSceneViewport.zw)) return;
    const float2 f = px - 0.5;
    const int2   b = int2(floor(f));
    const float2 t = f - float2(b);
    float3 sum = 0.0;
    float  wsum = 0.0;
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        const int2 o = int2(k & 1u, k >> 1u);
        const int2 q = clamp(b + o, int2(0, 0), int2(w, h) - 1);
        const float wk = (o.x ? t.x : 1.0 - t.x) * (o.y ? t.y : 1.0 - t.y);
        const float3 c4 = gDenoisedRefl.Load(int3(q, 0)).rgb;
        if (all(isfinite(c4))) { sum += c4 * wk; wsum += wk; }
    }
    if (wsum > 1e-3) refl = clamp(sum / wsum, 0.0, AVER_VOX_MAXRAD);
}

// Reflection trace/filter split (Settings::rayDrivenReflSplit / AVER_RD_REFL_SPLIT):
// R1 (split compile) traces, shades, writes gRtReflHistOut, defers spatial gather.
// R2 (CSRdReflFilter) gathers against that write, finishes compose. Default (no split) byte-for-byte today's.
#ifndef AVER_RD_REFL_SPLIT
#define AVER_RD_REFL_SPLIT 0
#endif
[numthreads(8, 8, 1)]
void CSRdRefl(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    const uint4 rec = gRdVisBuf[idx];
    uint dnW = 0, dnH = 0;
    gRdReflDnIn.GetDimensions(dnW, dnH);
    const bool dnIn = pixel.x < dnW && pixel.y < dnH;
    if (rec.x == 0xFFFFFFFFu) {
        // Sky: no surface to reflect off. Stage B never reads this texel for this pixel. NRD2's S there
        // is Stage B's own to write.
        if (dnIn && !rtNrd2Frame()) gRdReflDnIn[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        gRdReflTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Same pixel-centre NDC/primary-ray reconstruction.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // Footprint reconstruction: same as CSRdShadow. gSceneViewport (not Cur): differentials vs LAST frame.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float3 dirDx = averViewRayDir(ndc + float2(ndcPixelStep.x, 0.0));
    float3 dirDy = averViewRayDir(ndc + float2(0.0, ndcPixelStep.y));
    const float3 rdRayDx = (dirDx - dir) * s.hitT;
    const float3 rdRayDy = (dirDy - dir) * s.hitT;

    const float3 L = normalize(gLightDir.xyz);
    // The surface Stage B shades: its roughness gates the ray, its shading normal reflects it.
    const AverSurface hs = rdHitSurface(s, dir, rdRayDx, rdRayDy, AVER_RT_HIT_ROUGHNORMAL);
    const float3 R = reflect(dir, hs.N);
    const float rough = hs.rough;
    averRtCutoutPolicy(AVER_RD_CUTOUTS_REFL, rough > AVER_RD_REFL_SOLID_CUTOUT_ROUGH);

    // History writes live (blended draws never reach ray-driven primary).
    gAverHistoryWrite = true;

    // Gate: PSRayDriven's predicate. Unlike Stage S/G/O, can't defer to Stage B: reads outcome off gRdReflTex's alpha.
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(float2(pixel) + 0.5);
#if AVER_RD_ABLATE != AVER_RD_ABL_REFL && AVER_RD_ABLATE != AVER_RD_ABL_ALL
    const bool dnTraced = rtReflTraced && dnIn;
#else
    const bool dnTraced = false;
#endif
    // The traced denoiser branch writes its own S; every other pixel takes the zero.
    if (dnIn && !dnTraced) gRdReflDnIn[pixel] = float4(0.0, 0.0, 0.0, 0.0);
#if AVER_RD_ABLATE != AVER_RD_ABL_REFL && AVER_RD_ABLATE != AVER_RD_ABL_ALL
    if (dnTraced) {
        // Reflection denoising: the denoiser owns history and filtering, so this is one raw sample.
        rtReflectionHistoryVacate(float2(pixel) + 0.5);   // Voxi's own history is not used meanwhile
        const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;
        const uint  frameIdx  = (uint)gRtHistParams.z;
        uint dW = 0, dH = 0;
        gDenoisedRefl.GetDimensions(dW, dH);
        // Half rate as Voxi's own path (glossy only), on a pixel checkerboard the denoiser rebuilds:
        // a skipped pixel marks a = -1 and shows last frame's result at its surface. Under NRD2 (bit 64)
        // CSRdHalfFill fills it from this frame's traced neighbours instead.
        const uint hrBits = rtGiShadowBits();
        const bool skip = (hrBits & 4u) != 0u && lobeRough > 0.0 && (dW > 0u || (hrBits & 64u) != 0u) &&
                          ((pixel.x ^ pixel.y ^ frameIdx) & 1u) != 0u;
        float3 refl = 0.0;
        if (skip) {
            gRdReflDnIn[pixel] = float4(0.0, 0.0, 0.0, -1.0);
            rdDenoisedReflection(s.wpos, 0.0, refl);
        } else {
            bool specHit = false;
            const float3 fresh = clamp(rtReflection(s.wpos, s.N, s.Ng, R, L, float2(pixel) + 0.5, lobeRough,
                                                    frameIdx, specHit), 0.0, AVER_VOX_MAXRAD);
            const float hitT = gAverReflHitT;
            gRdReflDnIn[pixel] = float4(fresh, hitT);
            refl = fresh;
            rdDenoisedReflection(s.wpos, hitT, refl);
        }
        gRdReflTex[pixel] = float4(refl, any(refl >= AVER_VOX_MAXRAD) ? 2.0 : 1.0);
        return;
    }
#endif
    if (rtReflTraced) {
        const float rdReflDzdx = rdPlaneDepthStep(s.wpos, s.Ng, dirDx);
        const float rdReflDzdy = rdPlaneDepthStep(s.wpos, s.Ng, dirDy);
        bool specHit = false;
        // Pending iff R1 and history bound; with none bound, rtReflectionTemporalEx skips spatial gather.
#if AVER_RD_REFL_SPLIT && AVER_RD_ABLATE != AVER_RD_ABL_REFL && AVER_RD_ABLATE != AVER_RD_ABL_ALL
        const bool pending = gRtHistParams.x >= 0.5;
#else
        const bool pending = false;
#endif
#if AVER_RD_ABLATE == AVER_RD_ABL_REFL || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        float3 refl = float3(0.0, 0.0, 0.0);   // ablated: no mirror ray
#elif AVER_RD_REFL_SPLIT
        // R1: doSpatial=false traces and blends history, skips dense spatial gather (deferred to R2).
        float3 refl;
        if (pending) {
            refl = rtReflectionTemporalEx(s.wpos, s.N, s.Ng, R, L, float2(pixel) + 0.5, rough,
                                          rdReflDzdx, rdReflDzdy, false, specHit);
        } else {
            refl = rtReflectionTemporal(s.wpos, s.N, s.Ng, R, L, float2(pixel) + 0.5, rough,
                                        rdReflDzdx, rdReflDzdy, specHit);
        }
#else
        float3 refl = rtReflectionTemporal(s.wpos, s.N, s.Ng, R, L, float2(pixel) + 0.5, rough,
                                           rdReflDzdx, rdReflDzdy, specHit);
#endif
        // No fade: traced estimate carries the sky its rays reached. skyR is only ablation/no-ray fallback.
        const float skyW = 0.0;
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march
#else
        if (!specHit) skyR = skyColor(R);
#endif
        if (pending) {
            // Pending marker (alpha < -0.5): CSRdReflFilter finishes after gathering rtReflectionSpatial.
            gRdReflTex[pixel] = float4(skyR, -1.0);
        } else {
            // Same ceiling as PSRayDriven (guards unbounded-term incident). clamp(), not min().
            const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
            // .a is the stage decision: nonzero marks traced pixel. 2.0 vs 1.0 carries pre-clamp ceiling test.
            gRdReflTex[pixel] = float4(clamp(specRaw, 0.0, AVER_VOX_MAXRAD),
                                       any(specRaw >= AVER_VOX_MAXRAD) ? 2.0 : 1.0);
        }
    } else {
        // Roughness routed this pixel to Stage B's fallback: all zero, alpha included.
        gRdReflTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    }
}

// ---- STAGE R2: CSRdReflFilter -- finish PENDING reflection with spatial history gather -----
//
// R1's other half (AVER_RD_REFL_SPLIT): runs after C++ UAV barrier on gRtReflHistOut.
// Every PENDING pixel here was marked by R1 this same frame; same-frame round trip through gRtReflHistOut.
//
// SM 6.6 (same layout/defines as CSRdRefl). Takes dzdx/dzdy as params, never touches gAverHistoryWrite.
[numthreads(8, 8, 1)]
void CSRdReflFilter(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    // Only PENDING pixels (alpha < -0.5) need gather. Sky, roughness-gated, or already composed pixels left alpha 0/1/2.
    const float4 t = gRdReflTex[pixel];
    if (t.a > -0.5) return;
    const float3 skyR  = t.rgb;

    // Same pixel-centre NDC/primary-ray/record reconstruction. PENDING pixel guaranteed surface by R1.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    const uint4 rec = gRdVisBuf[idx];
    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // Footprint reconstruction (same as CSRdRefl's). `R` not rebuilt: ray-tracing already ran in R1.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float3 dirDx = averViewRayDir(ndc + float2(ndcPixelStep.x, 0.0));
    float3 dirDy = averViewRayDir(ndc + float2(0.0, ndcPixelStep.y));
    const float3 rdRayDx = (dirDx - dir) * s.hitT;
    const float3 rdRayDy = (dirDy - dir) * s.hitT;
    const float rdReflDzdx = rdPlaneDepthStep(s.wpos, s.Ng, dirDx);
    const float rdReflDzdy = rdPlaneDepthStep(s.wpos, s.Ng, dirDy);

    // Roughness and depth recomputed, not read. Pending marker is plain -1.0 flag.
    const float rough    = rdHitSurface(s, dir, rdRayDx, rdRayDy, AVER_RT_HIT_ROUGHNORMAL).rough;
    const float curDepth = mul(float4(s.wpos, 1.0), gViewProj).w;

    // Mirror cutoff same as rtReflectionTemporal(Ex): agrees with R1's filter radius.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // R1's write to gRtReflHistOut (u3), barriered against this read. Stands in for the col/curHit.
    const float4 h = gRtReflHistOut[pixel];
    const bool specHit = h.a > 0.0;

    float3 refl;
    if (specHit) {
        refl = rtReflectionSpatial(h.rgb, s.wpos, s.N, float2(pixel) + 0.5, curDepth, lobeRough,
                                   rdReflDzdx, rdReflDzdy);
    } else {
        refl = float3(0.0, 0.0, 0.0);
    }

    // Exact same compose as CSRdRefl's non-split tail: repeated verbatim, mirrors R1's control flow.
    const float skyW = 0.0;
    const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
    gRdReflTex[pixel] = float4(clamp(specRaw, 0.0, AVER_VOX_MAXRAD),
                               any(specRaw >= AVER_VOX_MAXRAD) ? 2.0 : 1.0);
}

// ---- STAGE F: CSRdHalfFill -- NRD2's half-rate tracing, filled from this frame only ----------------
//
// NRD2 frames have no history to rebuild a skipped pixel (docs/rendering/NRD2.md, "Half rate"). A skipped
// pixel's four edge neighbours are the other checkerboard half, so traced; each counts by its agreement
// with the centre: view depth within 2% + 1 cm of the centre's, or of the plane through the centre and
// the opposite neighbour (grazing surfaces), times cos^8 between CSRdShadow's normal guides (u3). A
// floor weight keeps the plain mean where no neighbour agrees. In-place: every read is of the traced
// half, every write to the skipped one.
// gViewParams.w for this dispatch: row pitch, bit 16 GI parity, bits 17/18/19 fill GI / AO / reflections.
float rdHalfFillWeight(uint2 q, float zq, float zc, float zOpp, float3 nc) {
    if (zq <= 0.0) return 0.0;   // sky or outside the viewport
    float err = abs(zq - zc);
    if (zOpp > 0.0) err = min(err, abs(zq - (2.0 * zc - zOpp)));
    const float wz = saturate(1.0 - err / (zc * 0.02 + 1.0));
    float c = saturate(dot(nc, gRtReflHistOut[q].xyz));
    c *= c; c *= c; c *= c;
    return wz * c + 1e-3;
}

[numthreads(8, 8, 1)]
void CSRdHalfFill(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;
    const uint  word  = (uint)gViewParams.w;
    const float zc    = gRdSunVisTex[pixel].a;
    if (zc <= 0.0) return;   // sky: every stage wrote its own sentinel
    const bool gi   = (word & (1u << 17)) != 0u && ((pixel.x ^ pixel.y ^ (word >> 16)) & 1u) != 0u;
    const bool ao   = (word & (1u << 18)) != 0u && gRdAoTex[pixel].a < 0.0;
    const bool refl = (word & (1u << 19)) != 0u && gRdReflDnIn[pixel].a < -0.5;
    if (!(gi || ao || refl)) return;

    const int2 lo = int2(gSceneViewportCur.xy);
    const int2 hi = lo + max(int2(gSceneViewportCur.zw), int2(1, 1)) - 1;
    const int2 offs[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    uint2 q[4];
    float z[4];
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        const int2 p = int2(pixel) + offs[k];
        q[k] = uint2(clamp(p, lo, hi));
        z[k] = (all(p >= lo) && all(p <= hi)) ? gRdSunVisTex[q[k]].a : 0.0;
    }
    const float3 nc = gRtReflHistOut[pixel].xyz;
    float w[4];
    [unroll] for (uint k2 = 0u; k2 < 4u; ++k2) w[k2] = rdHalfFillWeight(q[k2], z[k2], zc, z[k2 ^ 1u], nc);

    // Rejected taps are skipped, never multiplied by 0 (a value there may not be finite).
    if (gi) {
        float3 sum = 0.0;
        float  ws  = 0.0;
        [unroll] for (uint k = 0u; k < 4u; ++k) {
            if (w[k] <= 0.0) continue;
            const float4 g = gRdGiTex[q[k]];
            if (g.a > 0.5) { sum += g.rgb * w[k]; ws += w[k]; }
        }
        if (ws > 0.0) gRdGiTex[pixel] = float4(sum / ws, 1.0);   // else CSRdGi's own (reuse-only) answer
    }
    if (ao) {
        float sum = 0.0, ws = 0.0;
        [unroll] for (uint k = 0u; k < 4u; ++k) {
            if (w[k] <= 0.0) continue;
            const float4 a = gRdAoTex[q[k]];
            if (a.a > 0.0) { sum += a.r * w[k]; ws += w[k]; }
        }
        gRdAoTex[pixel] = float4(ws > 0.0 ? sum / ws : 1.0, 0.0, 0.0, 1.0);
    }
    if (refl) {
        // Traced neighbours only (a > 0.5; rough ones Stage B shades from cones are a = 0); their hit
        // distance fills NRD2's S alpha too.
        float3 sum = 0.0;
        float  hit = 0.0, ws = 0.0;
        [unroll] for (uint k = 0u; k < 4u; ++k) {
            if (w[k] <= 0.0) continue;
            const float4 r = gRdReflTex[q[k]];
            const float  h = gRdReflDnIn[q[k]].a;
            if (r.a > 0.5 && h >= 0.0) { sum += r.rgb * w[k]; hit += h * w[k]; ws += w[k]; }
        }
        if (ws > 0.0) {
            gRdReflTex[pixel]  = float4(sum / ws, 1.0);
            gRdReflDnIn[pixel] = float4(0.0, 0.0, 0.0, hit / ws);
        } else {
            gRdReflDnIn[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        }
    }
}
#endif  // AVER_RT

// ================= depth prepass =================
// Paired with VSMain: same compiled vertex shader as PSMainVoxi, so depth matches exactly.
// Writes no colour; reads only alpha-test logic (not full material: 4 fetches vs 1).
// Skips PSMainVoxi (shadow, cone traces, history, fog) entirely.
// Does NOT evaluate AVER_MAT_SLOPE_BLEND's second layer (landscape-only, excluded path).
void PSDepthPrepass(VSOut i) {
#ifdef AVER_MATERIAL_SRV
    if (gMaterialFlags & AVER_MAT_ALPHA_MASK) {
        AverVertex v = averVertexOf(i);
        float2 uv = averSurfaceUV(v);
        float alpha = gBaseColor.a * gBaseColorFactor.a * gBaseColorMap.Sample(gMaterialSampler, uv).a;
        clip(alpha - gAlphaCutoff);
    }
#endif
}

// Depth-only vertex shader for one shadow cascade (selected by gShadowDraw.x).
float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gCascadeViewProj[(uint)gShadowDraw.x]);
}

#ifdef AVER_INSTANCE_SRV
// AVER_INSTANCE_SRV is the t-register VoxiRenderer computed for this pipeline's layout.
#define AVER_INST_JOIN2(a, b) a##b
#define AVER_INST_JOIN(a, b) AVER_INST_JOIN2(a, b)
StructuredBuffer<float4x4> gInstanceWorlds : register(AVER_INST_JOIN(t, AVER_INSTANCE_SRV));

// VSShadow's instanced twin: one DrawIndexedInstanced submits every surviving instance of one mesh.
// World from gInstanceWorlds[instanceID] (VoxiRenderer::shadowPass), not gWorld.
float4 VSShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gCascadeViewProj[(uint)gShadowDraw.x]);
}
#endif

// Depth-only pair for GI-only shadow map: identical to VSShadow/VSShadowInstanced
// except transforming into gGiShadowViewProj (one box over the GI volume).
float4 VSGiShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gGiShadowViewProj);
}

#ifdef AVER_INSTANCE_SRV
float4 VSGiShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gGiShadowViewProj);
}
#endif

// ================= Voxi: voxelisation =================
// Rasterise the scene once per frame with no render target; pixel shader writes lit radiance into volume.
struct VoxOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; float2 uv : TEXCOORD1; };

// Adapts VoxOut to AverVertex. V is exactly zero: no camera in voxelisation pass.
// Distinct name (not overload): FXC converts compatible structs, makes calls ambiguous.
AverVertex voxelVertexOf(VoxOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrm);
    v.V    = float3(0, 0, 0);
    // FALSE: no camera here (V=0), so "is eye inside" has no answer. Must be written.
    v.backFace = false;
    v.uv   = i.uv;
    return v;
}

// Voxelisation vertex shader: outputs world space for geometry shader to project.
VoxOut VSVoxel(VSIn i) {
    VoxOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.nrm  = averTransformNormal(i.nrm, gWorld);
    o.uv   = i.uv;
    o.pos  = wp;
    return o;
}

// Projects each triangle along its dominant axis to cover the most voxels.
[maxvertexcount(3)]
void GSVoxel(triangle VoxOut inp[3], inout TriangleStream<VoxOut> os) {
    float3 n = abs(cross(inp[1].wpos - inp[0].wpos, inp[2].wpos - inp[0].wpos));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    [unroll] for (int k = 0; k < 3; ++k) {
        VoxOut o = inp[k];
        float3 v = voxelUVW(o.wpos);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0);
        os.Append(o);
    }
}

#if AVER_MS
// Voxelisation without geometry shader: same dominant-axis projection, per primitive.
// Use averTransformNormal (not plain mul): correct under rotation/uniform scale only.
[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSVoxel(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
             out vertices VoxOut verts[AVER_MS_TRIS * 3],
             out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    float3 wp[3], nr[3];
    float2 uv[3];
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        wp[k] = mul(float4(v.pos, 1.0), gWorld).xyz;
        nr[k] = averTransformNormal(v.nrm, gWorld);
        uv[k] = v.uv;
    }
    float3 n = abs(cross(wp[1] - wp[0], wp[2] - wp[0]));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        float3 v = voxelUVW(wp[k]);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        VoxOut ov;
        ov.wpos = wp[k];
        ov.nrm  = nr[k];
        ov.uv   = uv[k];
        ov.pos  = float4(p * 2.0 - 1.0, 0.5, 1.0);
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS

// Shades the fragment with shadowed sun plus sky, adds exitant radiance to the accumulator.
void PSVoxel(VoxOut i) {
    float3 uvw = voxelUVW(i.wpos);
    if (!insideVolume(uvw)) return;
    float3 N = normalize(i.nrm);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    // Use GI-only map, not cascades: voxels cannot use them (see giShadowFactor).
    sun.visibility = giShadowFactor(i.wpos, N, ndl);
    AverSurface s = averEvalMaterial(voxelVertexOf(i), sun);
    float3 albedo = averDiffuseAlbedo(s);

    // ---- ONE AXIAL CONE INTO THE PREVIOUS BAKE ------------------------------------------------
    //
    // One cone fixes two defects (volume stored AMBIENT instead of bounced light):
    //   1. Sky was unoccluded: sun.visibility gates sun, but averSkyIrradiance(N) depends only on N.
    //   2. No second bounce: only mip filter, forward-pass gather, debug raymarch read the volume.
    //
    // Legal here: voxelTex_ stays ShaderResource through raster draws (UnorderedAccess only before
    // resolve); bindings_ has the full SRV at t0. Mip 0 cleared; cone starts at dist 2, mip ~1.2.
    const float4 room = traceCone(i.wpos, N, AVER_VOX_INJECT_APERTURE);
    // Cone terminated on solid: no sky. Ran out of volume: all sky (traceCone treats left volume as unoccluded).
    const float skyVis = saturate(1.0 - room.a);
    // While ReSTIR GI is estimator, volume carries no sky (it traces real rays). Keyed on gViewParams.z.
    const float skyInject = gViewParams.z > 0.5 ? 0.0 : 1.0;

    // Exitant radiance: sun term is irradiance (1/PI); sky and feedback are radiance.
    const float3 bounceGain = min(albedo * AVER_VOX_FEEDBACK, AVER_VOX_MAX_BOUNCE_GAIN);
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility / PI
                                + averSkyIrradiance(N) * gAmbient.r * skyVis * skyInject)
                    + room.rgb * bounceGain;
    // Lamp's own glow: without it, emissive surface injects nothing. After albedo multiply (emission is exiting light).
    radiance += s.emissive;
    radiance = clamp(radiance, 0.0, AVER_VOX_MAXRAD);

    // insideVolume() inclusive of 1.0; conservative raster produces uvw == 1.0 exactly.
    uint3 c = min(uint3(uvw * gVoxelParams.x), (uint)gVoxelParams.x - 1);
    uint3 a = uint3(c.x * 4, c.y, c.z);
    uint prev;
    InterlockedAdd(gVoxelAccum[a],                (uint)(radiance.r * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(1,0,0)], (uint)(radiance.g * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(2,0,0)], (uint)(radiance.b * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(3,0,0)], 1u, prev);   // fragments covering this voxel
}

// All kernels dispatch over destination mip's full extent; skip per-thread if outside [gBoxLo,gBoxHi).
// With bounded dispatch off, gBoxLo=0/gBoxHi=(level dim), guard rejects edge threads.

// Zeroes the accumulator before injection.
[numthreads(4,4,4)]
void CSClear(uint3 id : SV_DispatchThreadID) {
    uint3 v = id + gBoxLo; if (any(v >= gBoxHi)) return;
    uint3 a = uint3(v.x * 4, v.y, v.z);
    [unroll] for (uint k = 0; k < 4; ++k) gVoxelAccum[a + uint3(k,0,0)] = 0;
}

// Turns fixed-point sums into mip 0: mean radiance of fragments covering each voxel.
[numthreads(4,4,4)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    uint3 v = id + gBoxLo; if (any(v >= gBoxHi)) return;
    uint3 a = uint3(v.x * 4, v.y, v.z);
    uint n = gVoxelAccum[a + uint3(3,0,0)];
    if (n == 0) { gVoxelUAV[v] = 0.0; return; }
    float3 s = float3(gVoxelAccum[a], gVoxelAccum[a + uint3(1,0,0)], gVoxelAccum[a + uint3(2,0,0)]);
    gVoxelUAV[v] = float4(s / (AVER_VOX_FIXED * (float)n), 1.0);   // alpha = occupancy
}

// ================= Voxi: mip filtering =================
// Box-filters radiance and occupancy from one mip into the next (source t0, destination u0).
[numthreads(4,4,4)]
void CSMip(uint3 id : SV_DispatchThreadID) {
    uint3 v = id + gBoxLo; if (any(v >= gBoxHi)) return;
    int3 s = int3(v) * 2;
    float4 a = 0;
    [unroll] for (int x=0;x<2;++x)
    [unroll] for (int y=0;y<2;++y)
    [unroll] for (int z=0;z<2;++z)
        a += gVoxelTex.Load(int4(s + int3(x,y,z), gSrcMip));
    gVoxelUAV[v] = a * 0.125;
}

// Debug view: raymarches the volume to screen over the sky. Returns linear radiance.
float4 PSVoxelDebug(SkyOut i) : SV_TARGET {
    float3 ray = averViewRayDir(i.ndc);
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
    float4 acc = 0;
    float t = 0;
    [loop] for (int s = 0; s < 256; ++s) {
        if (acc.a >= 0.98) break;
        float3 uvw = voxelUVW(gCamPos.xyz + ray * t);
        t += voxelWorld;
        if (t > gVoxelParams.z * 2.0) break;
        if (!insideVolume(uvw)) continue;
        float4 v = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
        acc += (1.0 - acc.a) * v;
    }
    float3 bg = skyColor(ray);
    float3 col = acc.rgb + bg * (1.0 - acc.a);
    return float4(col, 1.0);
}
