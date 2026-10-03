
// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
#define AVER_SHADOW_CASCADES 4

// Use AVER_CB_JOIN, never a literal register (e.g. b4): rhi::kFeatureFrameConstantRegister is the sole
// definition (shaderConstantsHlsl() emits it; ClusterFrameCB binds the same way); a literal keeps
// compiling against a stale slot if it moves.
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
    // x = 1 while t6/u2 (shadow hist/histOut) are bound this frame;
    // y = 1 once t6 holds a real previous frame (0 right after create/resize); 0.5 = valid but sun moved
    //     this frame (beginShadowHistory) -- sun-dependent histories (shadow, reflection) test > 0.75 and
    //     skip it, sky occlusion (sun-independent) tests > 0.25 and keeps accumulating;
    // z = current frame index (per-frame count, not wall-clock);
    // w = pixels-per-ray tile edge as its BIT COUNT (0 = no tiling).
    float4   gRtHistParams;
    // LAST frame's camera view-projection, for reprojecting a pixel's world position into
    // gRtShadowHist. Only meaningful while gRtHistParams.y is set.
    float4x4 gPrevViewProj;
    // LAST frame's scene viewport rect (x, y, w, h in target pixels): the reprojected NDC lands
    // here, not at [0,1] of the whole history texture -- the editor docks the 3D view in a sub-rect
    // of the backbuffer. Same validity as gPrevViewProj.
    float4   gSceneViewport;
    // THIS frame's scene viewport rect, same (x, y, w, h) in target pixels. Paired with gViewProj,
    // where gSceneViewport above is paired with gPrevViewProj. w == 0 means the device had none.
    float4   gSceneViewportCur;
    // x = 1 when the eye is inside a blended single-sided volume, y = that medium's ior. Computed once per
    // frame on the CPU (a pixel can't know if its own volume encloses the camera; see VoxiRenderer for
    // why a loose bounding-sphere test is safe here).
    // z/w are unrelated LOCAL LIGHTS (lamps, voxi_rt.hlsli's RdLocalLight) sharing this row for space: z =
    // light count in gRdLocalLights/t18 as a float (0 = off/unavailable); w = bits as a float: 1 =
    // gRdLocalHist (t19) has a valid previous frame for the same light set, 2 = every lamp-flagged draw is
    // listed (GI may drop emitters' own emission). Decode via
    // rdLocalLightCount()/rdLocalHistValid()/rdLocalCarriesEmitters() (voxi_rt.hlsli).
    float4   gCameraMedium;
    // The caustic caster's world box: min.xyz / max.xyz, min.w = 1 when one exists, max.w strength.
    // max.z is the surface light refracts through. See VoxiRenderer for why a box and not a sphere.
    float4   gCausticMin;
    float4   gCausticMax;
    // The GI-ONLY shadow map's light view-projection: one box fitted to the GI VOLUME, not to the
    // camera. Read only by giShadowFactor (PSVoxel); the cascades above stay camera-fitted and are
    // what PSMainVoxi samples.
    float4x4 gGiShadowViewProj;
    // x = 1/kGiShadowSize, y = 1 once the GI-only map is usable (0 = fall back to unshadowed
    // indirect), z = normal-offset bias in world units.
    //
    // w WAS UNUSED; NOW A RUNTIME BIT-FIELD (decode via `(uint)gGiShadowParams.w`, never as a float).
    // Assembled every frame by VoxiRenderer::prePass from four Settings toggles (bits 1/2/4 default ON
    // since 896c5187, bit 8 since 2026-09-27), each an A/B-measured trade against ray-driven cost, not a
    // correctness fix:
    //   bit 1  Settings::rtSecondaryShadowOpaque -- rtReflection/giTraceInitialCandidate's secondary-hit
    //          sun-shadow ray uses rtShadowOpaque (opaque-including-cutouts lane) instead of rtShadow,
    //          trading away a translucent pane's tinted shadow on a secondary hit. Primary shadows
    //          (rtShadowTemporalEx / CSRdShadow / CSRdShadowProbe) untouched.
    //   bit 2  Settings::rtSkyOcclusionHalfRate -- rtSkyOcclusionTemporal skips its ray on half of this
    //          frame's 8x8 tiles (alternating by frame) wherever the reprojected AO history is valid.
    //   bit 4  Settings::rtReflectionHalfRate -- rtReflectionTemporalEx skips its ray the same tiled way,
    //          except for a mirror (lobeRough == 0), which always retraces (a reprojected mirror is wrong
    //          under motion).
    //   bit 8  Settings::rtGiHitShadowMap -- giTraceInitialCandidate's hit reads sun visibility from the
    //          GI-only shadow map (giHitShadowMapVisibility) instead of a ray, falling back to the ray
    //          where the map can't answer.
    // Bit 16 is OR'd in separately, AFTER prePass's bits 1/2/4/8 assembly, by
    // VoxiRenderer::recordStagedRayDriven itself:
    //   bit 16 Settings::blendedReuseStagedLighting -- set once Stage B has written this frame's
    //          gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex (+ gRdLocalOut). Tells PSMainVoxi's blended
    //          replay that a translucent fragment on the opaque surface Stage B lit may reuse those
    //          textures instead of retracing (see PSMainVoxi's rdReuse). Clears itself on any frame the
    //          staged path doesn't run (RT off, or a tier below Ray-Driven), since prePass never touches
    //          it -- no separate flag needed.
    // Bit 32 is no Settings toggle: prePass ORs it in right after the acceleration-structure build
    // whenever that structure holds no translucent-lane instance:
    //   bit 32 rtShadowEx traces the first-hit query (ACCEPT_FIRST_HIT_AND_END_SEARCH, opaque lanes)
    //          instead of the transmittance walk -- the same answer when nothing can tint a shadow, see
    //          its FIRST-HIT FAST PATH (voxi_rt.hlsli). Reaches every rtShadowEx caller: CSRdShadow,
    //          CSRdShadowProbe, PSMainVoxi, and rtShadow's secondary-hit callers while bit 1 is off.
    //          Never the single-pass megakernel (rtGiShadowBits() is a constant there).
    // All bits clear -> field reads 0.0, exactly as before this existed.
    float4   gGiShadowParams;
    // The SPATIAL shadow denoiser. x = filter radius in px (0 = off); y = blend weight of the filtered
    // value (0 = taps still run, discarded -- cost-measurement config; lerp(v,f,0)==v exactly); z,w unused.
    // Radius is a CONSTANT not a #define so the tap loop stays dynamic and isn't unrolled away at 0.
    float4   gRtDenoiseParams;
    float4   gPtBounceParams;
    // x = total cones the diffuse gather traces (incl. axial). y/z/w are REFRACTION, not spare: y = mode
    // (Settings::refractionMode, 0 = off), z = strength, w = edge fade -- read by averRefractedBackdropUV.
    // Warning from experience: this row was called "spare" while unused for years, then overwritten when
    // refraction landed -- keep field comments here current.
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
    //   bits 0-1 RestirVisibility mode (Settings::giRestirVisibility, 0..3: No ray/Reconstructed/
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
    float4   gAmbientParams;
    // x = VIEW-DEBUG MODE (VoxiRenderer::ViewDebug): 0 normal, 1 Unlit, 2 RayHitInstance, 3 RayHitMaterial,
    // 4 RayHitDistance, 5 Triangles -- PSRayDriven's debug visualisations only (search "vmode");
    // PSMainVoxi never reads this field. Mirrors FrameConstants::viewParams, appended at the end (see
    // VoxiRenderer.hpp's static_assert).
    // y = GI radiance ceiling (Settings::giRadianceCeiling); AVER_VOX_MAXRAD below falls back to 16.0 if 0.
    // z = 1 when ReSTIR GI is the CHOSEN estimator (VoxiRenderer::giRestirWanted()), read by PSVoxel to
    // leave sky out of the volume -- NOT gGiRestirParams.x, which also drops to 0 when the estimator
    // merely can't run this frame (GI debug view, empty TLAS), which would rebuild the volume twice per
    // toggle. w = staged RD passes' visibility-record row pitch while recording, 0 otherwise (gRdVisBuf).
    // (z/w briefly carried the ReSTIR reuse tolerances during the fade bisection; now literals in
    // voxi_restir.hlsli.)
    float4   gViewParams;
    // ReSTIR GI control (Settings::giMode); mirrors FrameConstants::giRestirParams, appended at the
    // end. x = 1 while giMode==1 is ACTUALLY running (VoxiRenderer::giRestirWanted(), never the raw
    // setting -- touching t12/t13/u6/u7/u8 on the raw setting alone would null-descriptor-read on
    // hardware that can't run this). y = 1 once gGiSurfPosHist/gGiSurfNrmHist hold a real previous frame.
    // z = which of the two reservoir slices this frame writes (the other is last frame's temporal
    // source). w = ReSTIR-GI poison debug view (voxi.giPoisonView): >0.5 makes giRestirIndirect paint a
    // colour per non-finite guard (see that function's POISON DEBUG VIEW legend) instead of shading. Also
    // read directly in PSMainVoxi/PSRayDriven for an eighth (violet) colour on the ray-traced SPECULAR
    // indirect term's own AVER_VOX_MAXRAD clamp (B1/F5, not giMode-gated -- see
    // aver_IsGiRestirPoisonColour for precedence between the two families). Was "spare"; a repurposed
    // bit, not a new field -- packing/size unchanged.
    float4   gGiRestirParams;
};

// ---- Voxi: voxel cone traced GI ----
RWTexture3D<float4> gVoxelUAV : register(u0);
Texture3D<float4>   gVoxelTex : register(t0);
SamplerState        gVoxelSamp : register(s0);

// Injection accumulator: channel k of voxel c lives at (c.x * 4 + k, c.y, c.z), k == 3 being the
// fragment count. R32_UINT is the only typed format D3D12 guarantees UAV atomics on.
RWTexture3D<uint> gVoxelAccum : register(u1);

// Fixed-point scale radiance is multiplied by before accumulation and divided by in CSResolve.
#define AVER_VOX_FIXED 16384.0
// Ceiling on voxel/GI radiance -- now a LIVE per-frame value (gViewParams.y, Settings::giRadianceCeiling
// -> VoxiRenderer::prePass; see Voxi.hpp for what this caps). Falls back to the old literal 16.0 when the
// field reads exactly 0 (an unset FrameConstants block before the first prePass -- SandboxApp.cpp's
// cluster-GI binder). Parenthesised as a single expression so every use site (always a runtime function
// argument here, never a static const initialiser) stays valid unrewritten.
#define AVER_VOX_MAXRAD (gViewParams.y > 0.0 ? gViewParams.y : 16.0)
// Aperture of the single cone PSVoxel traces into the previous bake, as tan(half-angle). 0.577 = tan(30),
// a 60-deg cone matching the forward gather's own axial cone, so both agree on hemisphere coverage
// rather than being two different estimates. Wide on purpose: a coverage question, not a directional one.
#define AVER_VOX_INJECT_APERTURE 0.577
// Re-emission gain for the next bake -- a COMPENSATION CONSTANT, not physical: this single 60-deg cone
// under-counts what the 13-cone hemisphere gather it feeds actually integrates, and this multiplier makes
// up the difference. 1.0 and 8.0 were measured (8.0 too hot); 3.0, checked once against the Sponza path
// tracer (c6d1a750, scripts/pt-compare.ps1), closed most of the gap -- not derived analytically. First
// dial to reach for if a scene blows up; 0 turns the second bounce off for an A/B.
#define AVER_VOX_FEEDBACK 3.0

// Ceiling on albedo x AVER_VOX_FEEDBACK, per channel. The bake re-reads the previous one, so per-bounce
// gain must stay < 1/channel or it runs away: with x3 compensation, anything reflecting over a third of a
// channel gained energy each rebuild -- a sun drag (rebuild/frame) drove NewSponza's curtains room to
// solid red, kept by the GI cache. Below the cap (stone, most albedos) the calibrated gain is unchanged.
#define AVER_VOX_MAX_BOUNCE_GAIN 0.8

// Edge, in px, of the tile sharing one sky-occlusion ray direction (see rtSkyOcclusion for why coherence
// is the lever here, not ray count). 1 = fresh rotation per pixel, bit-identical to before this existed.
#ifndef AVER_AO_COHERENCE_TILE
#define AVER_AO_COHERENCE_TILE 1.0
#endif


// The roughness below which a reflective surface is treated as a MIRROR: no cone, no temporal
// history, no spatial filter. See rtReflectionTemporal's own comment for why all three must be
// derived from this one number rather than each choosing its own threshold.
#define AVER_REFL_MIRROR_ROUGH 0.1

// gRtReflHist's alpha holds the reflecting surface's view depth in METRES (cm * this). The texture is
// RGBA16F, whose largest finite value is 65504: in centimetres that overflowed past 655 m and switched
// the reprojection and spatial depth tests off for every distant hill.
#define AVER_REFL_HIST_DEPTH_SCALE 0.01

// TLAS instance-mask lanes. MUST MATCH kRtMaskOpaque/kRtMaskTranslucent in VoxiRenderer.cpp -- no
// shared-source mechanism ties C++ and HLSL, so a mismatch is a silent image bug, not a build error.
//
// Every ray but the shadow ray asks for OPAQUE only: translucent panes are FORCE_NON_OPAQUE, and a
// single Proceed()+CommittedStatus traversal (every non-shadow ray) would stop AT an uncommitted hit --
// narrowing the mask keeps those rays from ever meeting translucent geometry.
#define AVER_RT_MASK_OPAQUE      0x01
#define AVER_RT_MASK_TRANSLUCENT 0x02
// THE VIEWER'S OWN BODY: an opaque instance every ray may hit EXCEPT the ray-driven primary one -- an
// unfiltered primary ray inside a first-person head would fill the screen with the character's own skin.
//
// scene::kMeshRendererHiddenFromOwner skips drawMesh() in the raster walk but keeps the instance in the
// TLAS (still a shadow caster, still in the GI volume), so only ray-driven primary visibility (the
// default, which traces the TLAS directly) needs this mask. Measured: probe 43,33,28 (ray-driven) vs
// 206,215,218 (--rt-render-mode 0), same pose.
//
// A third lane rather than removing the instance: every other ray (shadow, GI bounce, mirrors, glass)
// still wants this geometry; the shadow ray masks AVER_RT_MASK_ALL and picks it up for free.
#define AVER_RT_MASK_OWNER_HIDDEN 0x04
// Opaque geometry as a SECONDARY ray sees it: solid surfaces including the viewer's own body. Every
// opaque traversal but the ray-driven primary ray uses this.
#define AVER_RT_MASK_OPAQUE_ALL  (AVER_RT_MASK_OPAQUE | AVER_RT_MASK_OWNER_HIDDEN)
#define AVER_RT_MASK_ALL         0xFF

// Directional shadow map. Core feature level 11_0, so it works on every DX12 GPU.
Texture2D<float>          gShadowTex  : register(t1);
SamplerComparisonState    gShadowSamp : register(s1);

// The GI-only shadow map. OUTSIDE the AVER_RT guard below on purpose: its only reader is PSVoxel,
// which is compiled without ray tracing, so a declaration inside the guard would vanish exactly
// where it is needed. Shares gShadowSamp -- same comparison state, different texture.
Texture2D<float>          gGiShadowTex : register(t8);
// THE OPAQUE SCENE, COPIED BEFORE TRANSLUCENCY REPLAYS (IDevice::sceneColorBackdropTexture). Lets a
// blended surface tint what's behind it PER CHANNEL -- hardware blending gives only one scalar
// (1 - src.a), so without this glass could only darken with depth, never turn greener.
//
// MAY BE NULL-FILLED: before the first resize, under MSAA (copy invalid from a multisampled target), or
// on a backend without it. averBlendBackdropValid() below is the check; every use falls back to the
// scalar composite when it says no.
Texture2D<float4>         gBlendBackdrop : register(t10);

// ---- Occlusion-aware fog: the air sky-visibility volume ----
//
// PROBLEM: height fog and the aerial-perspective term (shared_prelude.hlsl, averApplyFogAirVis) add
// in-scattered SKY light along every view ray with NO occlusion, washing bounce-lit interiors into a flat
// blue veil. Gating on the surface's own screen-space AO history was tried and reverted
// (aver-fog-skyvis-failed.md): fog is a property of the CAMERA-TO-SURFACE PATH, not the surface's
// hemisphere, and a per-pixel accumulated signal flashes open on camera motion (disocclusion falls back
// to "open"). This volume is instead
// PATH-based and WORLD-SPACE -- each cell answers "how much sky can the air HERE see" -- with no
// per-pixel history or jitter, so camera motion can't make it flash.
//
// Covers EXACTLY the GI voxel volume (same uvw = (p - gVoxelOrigin.xyz) * gVoxelOrigin.w mapping as
// voxelUVW/insideVolume, voxi_cone.hlsli), at
// its own fixed 32x32x32 resolution independent of gVoxelParams.x (see AVER_AIRVIS_RES, by CSAirVis).
// gAirVis (t17) is read by voxiAirVisibility (below); gAirVisOut (u16) is CSAirVis's write target -- same
// SRV/UAV split as gVoxelTex/gVoxelUAV, for the same reason (one resource can't be both in one slot).
//
// kVoxiSrvCount 17->18, kVoxiUavCount 16->17 (VoxiRenderer.cpp) -- next free slot after t16/u15.
//
// OFF, OR VOLUME NOT YET BUILT: both slots stay bound to a 1x1x1 placeholder. voxiAirVisibility treats
// dimensions <= 1 as "no volume" and returns 1.0 -- bit-identical to before this feature existed.
Texture3D<float>          gAirVis    : register(t17);
RWTexture3D<float>        gAirVisOut : register(u16);

// ---- CAUSTICS: light focused by the water surface onto what lies under it ----
// Placed above AVER_RT (used to sit inside it, silently breaking 4 of the shadow/GI-shadow pipelines
// that don't define it): pure arithmetic on the clock and a box, no rays needed.
// Projected from the volume (gCausticMin/Max), not painted into a material, so it stops exactly where
// the water stops. Approximates brightness as the Laplacian of the height field (analytic for a sum of
// sines, -k^2*sin(phase) per term) -- differentiates the same three sines the ripple graph uses, twice.
// NOT real caustics (no light-path/sun-angle/wall dependence), just a focus term under a flat pool.
// Shares one wave set with the ripple graph via averWaveFocus/averWaveNormal (shared_prelude.hlsl).
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
//
// Declared UNCONDITIONALLY, above #if AVER_RT, even though every reader is inside that guard: PSMainVoxi
// (which SETS it) is not itself guarded, so a rasteriser-only build (AVER_RT 0) must still compile the
// assignment even though the variable then goes unread. The alternative -- declaring it inside the guard,
// beside its only readers -- would fail that build on an undeclared identifier; see voxi_restir.hlsli's
// ordering-contract note for this compiles-one-variant-breaks-the-other trap.
//
// `static`, not a cbuffer field: per-invocation storage like gGiPoisonPdfHit (voxi_restir.hlsli), set once
// near the top of whichever entry point runs (PSRayDriven sets it unconditionally true) and read by every
// gated history write below. Default true = unconditional write, matching every entry point that never
// touches it (VSMain, VSky, PSVoxel, ...).
static bool gAverHistoryWrite = true;

// ---- SUBSURFACE: where the primary sun-shadow rays start (averSubsurfaceShadowPush) ----
// Added to the ray origin in rtShadowEx, and to nothing else, so the temporal wrapper keeps reprojecting and
// filtering at the real surface. Zero except around a subsurface pixel's own primary sun-shadow call, which
// sets it and puts it back straight after -- every other shadow ray (reflection hits, GI hits, lamps) must
// start where it always did. Declared unconditionally for gAverHistoryWrite's reason just above.
static float3 gAverShadowOriginPush = float3(0.0, 0.0, 0.0);

// Is THIS fragment a translucent (glass/water) draw, replayed blended? Mirrors pbr::isTranslucent
// (Material.cpp:70-72) exactly: AVER_MAT_ALPHA_BLEND alone is set only for AlphaMode::Blend
// (MaterialGpu.cpp:94), so frosted glass (transmission > 0, no alpha blend) needs the OR too, or it
// would keep writing history it shouldn't. gTransmission: material_prelude.hlsl:66; AVER_MAT_ALPHA_BLEND:
// material_prelude.hlsl:113. Called only behind gAmbientParams.w bit 32 -- on Vulkan, never set (C10),
// so this is declared but never evaluated at runtime there.
bool averDrawIsTranslucent() { return (gMaterialFlags & AVER_MAT_ALPHA_BLEND) != 0u || gTransmission > 0.0; }

#if AVER_RT
#include "voxi_rt.hlsli"

#include "voxi_restir.hlsli"

// ---- STAGED RAY-DRIVEN PASSES (milestone 1): the visibility record and resolved sun visibility ----
//
// voxi.rayDrivenStages (Settings::rayDrivenStages, --rd-stages, u32 0/1/2) splits the single PSRayDriven
// fullscreen draw into GPU passes at 1+: CSRdVisibility traces the primary ray into a record per pixel
// here; CSRdShadow reads it, reconstructs the surface, resolves sun shadow into gRdSunVisTex; PSRayDriven
// (compiled again with AVER_RD_SPLIT=1) reads both instead of tracing -- shading after that point is
// unchanged. At 0 (default): no new resource/pipeline, AVER_RD_SPLIT=0 is byte-identical to before.
//
// VALUE 2 (milestone 4) adds: CSRdGi compiled again with AVER_GI_CHECKERBOARD=1, tracing ReSTIR GI's
// candidate for half the pixels/frame on a checkerboard, the denoiser reconstructing the rest (see
// that compile's header and voxi_restir.hlsli's AVER_GI_CHECKERBOARD). Every other stage is unchanged.
//
// u11/u12: next free UAV slots after gGiVisHistOut's u10 (kVoxiUavCount 11->13, VoxiRenderer.cpp).
//
// gRdVisBuf: one uint4/pixel, index = pixel.y * pitch + pixel.x, pitch = rdRowPitch() (this cbuffer's w
// field, else 0). HIT = uint4(ref, primitiveIndex, asuint(bary.x), asuint(bary.y)), ref being the packed
// instance reference rtPackCommitted gives and rtLoadInstance reads back (voxi_rt.hlsli); MISS = x ==
// 0xFFFFFFFFu (y/z/w undefined), a value no packed reference takes.
RWStructuredBuffer<uint4> gRdVisBuf    : register(u11);
// gRdSunVisTex: this frame's resolved sun visibility, one RGBA16F texel/pixel. rgb = the tinted
// transmittance rtShadowTemporal returns (post temporal/spatial filters). alpha = primary surface's
// LINEAR VIEW DEPTH (mul(float4(wpos,1), gViewProj).w, same formula as PSMainVoxi's rtViewZ), 0.0 for
// sky -- lets a blended-replay fragment (gGiShadowParams.w bit 16) PROVE it sits on the surface Stage S
// lit before reusing rgb instead of retracing (see PSMainVoxi's rdReuse). Written once by CSRdShadow
// (both channels together, every write site); read by PSRayDriven's AVER_RD_SPLIT branch (rgb) and
// PSMainVoxi's blended replay (rgb+alpha). NOT a history buffer itself -- gRtShadowHist/gRtShadowHistOut
// still own the real frame-to-frame history; this only ferries one frame's answer to its readers.
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
//
// gRdLocalLights: this frame's sphere lights, at most 32, rdLocalLightCount() live (gCameraMedium.z);
// struct/fields in voxi_rt.hlsli's RdLocalLight.
// gRdLocalHist/gRdLocalOut: one RGBA16F texel/pixel, sized like and PING-PONGED WITH the sun's shadow
// history (same index, rtHistWriteIdx_), so last frame's lamp texel matches the sun's reprojected one.
// a = accumulated visibility (the only channel read back); rgb written 0. Written by
// rdLocalLightsVisibility from whichever pass lights opaque surfaces (CSRdLocalLights staged,
// single-pass PSRayDriven, or PSMainVoxi raster); read back via rdLocalVisFiltered by Stage B and by a
// blended pane reusing its surface -- so t19 is read from both compute and pixel shaders. Unlike
// gRdSunVisTex this pair IS a history and carries no depth of its own: validated against the sun
// history's depth at the same texel (rtReprojectTexel).
//
// t18/t19 (kVoxiSrvCount 18->20) and u19 (kVoxiUavCount 19->20) -- next free slots after gAirVis's t17
// and gRdShadowTiles' u18. Every slot holds a placeholder when absent, so a 0-count read is never null.
StructuredBuffer<RdLocalLight> gRdLocalLights : register(t18);
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
    // is not finite would turn `* 0` into NaN.
    if (w > 0.0) {
        sum  += gRdLocalOut[q].a * w;
        wsum += w;
    }
}

// The staged read of CSRdLocalLights' VISIBILITY, for Stage B and a blended pane reusing its surface: a
// 5x5 around this pixel weighted by view-depth similarity (gRdSunVisTex.a). One stochastic shadow ray per
// pixel per turn leaves grain the temporal accumulation hasn't averaged yet, worse under camera motion.
// 5x5 not 3x3: MEASURED on a lantern's vault penumbra, 3x3 left sparse dots, 5x5 reaches the lamps-off
// noise floor (see rdLocalLightsVisibility's accumulation comment for numbers) -- 48 texel reads/pixel.
// STAGED ONLY: gRdSunVisTex's depth exists only when the staged passes ran. Returns 1 (unshadowed) where
// Stage S found no surface -- a branch neither caller reaches.
float rdLocalVisFiltered(uint2 pixel) {
    const float zc = gRdSunVisTex[pixel].a;
    if (zc <= 0.0) return 1.0;
    const int2 lo = int2(gSceneViewportCur.xy);
    const int2 hi = lo + max(int2(gSceneViewportCur.zw), int2(1, 1)) - 1;
    const int2 c  = int2(pixel);
    float sum  = gRdLocalOut[pixel].a;
    float wsum = 1.0;
    [unroll] for (int oy = -2; oy <= 2; ++oy) {
        [unroll] for (int ox = -2; ox <= 2; ++ox) {
            if (ox != 0 || oy != 0) rdLocalVisTap(c + int2(ox, oy), lo, hi, zc, sum, wsum);
        }
    }
    return sum / wsum;
}

// ---- lamp HISTORY reads, at the continuous reprojected position ----
// Against LAST frame's stored depth (gRtShadowHist.y at gRdLocalHist's ping-ponged texel), not this frame's
// gRdSunVisTex.a -- depth belongs to the surface, not the light (rtReprojectTexel), which is why
// gRdLocalHist carries none of its own. Bounds: gRtShadowHist's dimensions intersected with the PREVIOUS
// viewport (gSceneViewport); a tap outside either is skipped.
//
// SUB-PIXEL, NOT SNAPPED (2026-09-28): both reads used to centre on rtReprojectTexel's floor()ed texel. Flying
// FORWARD magnifies the image, so several pixels snapped to one history texel and read identical values --
// a noisy texel became a blob that grew as the camera kept moving ("noise in the night view when in
// motion": vault MAD 11.0 moving vs settled, lamps off 1.6). Weights now follow the exact reprojected
// point `pxPrev`; at rest it sits on a texel centre, so both reduce to the old weights exactly.
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
    if (w > 0.0) {
        sum  += gRdLocalHist.Load(int3(q, 0)).a * w;
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
    return wsum > 0.0 ? sum / wsum : gRdLocalHist.Load(int3(texel, 0)).a;
}

// THE HISTORY READ rdLocalLightsVisibility blends its fresh sample into (all modes) -- unrelated to Stage
// B's THIS-frame filter above: a depth-weighted 3x3-texel BOX centred on pxPrev, each texel weighted by its
// area overlap (up to 4x4 taps while moving, exactly the old 3x3 at rest). Why an average, not the bare
// texel: see rdLocalLightsVisibility's accumulation comment below.
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
    return wsum > 0.0 ? sum / wsum : gRdLocalHist.Load(int3(texel, 0)).a;
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
// constant-buffer condition -- callers (CSRdLocalLights, SM 6.6 8x8=2x2 quads per CSRdShadow's header,
// after only CSRdShadow's own early-outs; and the pixel shaders behind rdLocalLightCount()) hold that
// same ordering.
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
        const uint n = min(rdLocalLightCount(), 32u);
        float wsum    = 0.0;
        uint  lastLit = 0u;
        [loop] for (uint i = 0u; i < n; ++i) {
            const float w = averShadowLum(rdLocalIrradiance(gRdLocalLights[i], wpos, N));
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
            const float u      = frac(rtHash(pixelC + float2(0.37, 11.0)) + rtRadicalInverse2(turn + 1u));
            const float target = u * wsum;
            uint  pick = lastLit;
            float acc  = 0.0;
            [loop] for (uint j = 0u; j < n; ++j) {
                acc += averShadowLum(rdLocalIrradiance(gRdLocalLights[j], wpos, N));
                if (target < acc) { pick = j; break; }
            }

            // The sun's golden-angle jitter (rtShadowTemporalEx's untiled branch), stepped per TURN so the
            // disc sample advances one golden angle each time this pixel traces.
            const float frameJitter = (float)turn * 2.39996323;
            const float v = rdLocalShadow(wpos, N, gRdLocalLights[pick], pixelC, frameJitter);

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
    const float3 toC   = ll.posRadius.xyz - wpos;
    const float  d2    = dot(toC, toC);
    const float  range = ll.radianceRange.w;
    if (d2 >= range * range) return false;   // also the zero-range guard, as in rdLocalIrradiance
    const float r    = ll.posRadius.w;
    const float x2   = d2 / (range * range);
    const float win  = saturate(1.0 - x2 * x2);
    const float invD = rsqrt(max(d2, 1e-8));
    l.direction = toC * invD;
    l.radiance  = ll.radianceRange.rgb * (1e4 / max(d2, r * r)) * (win * win);
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
    const uint n = min(rdLocalLightCount(), 32u);
    [loop] for (uint i = 0u; i < n; ++i) {
        AverLight   l;
        AverSurface sL;
        if (rdLocalLightAt(gRdLocalLights[i], s, wpos, l, sL))
            acc = averShadeDirect(acc, sL, l);
    }
    return acc * vis;
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
    const uint n = min(rdLocalLightCount(), 32u);
    [loop] for (uint i = 0u; i < n; ++i) {
        AverLight   l;
        AverSurface sL;
        if (!rdLocalLightAt(gRdLocalLights[i], s, wpos, l, sL)) continue;
        float3 dDiffuse, dSpecular, dSubsurface;
        float  ndl;
        averDirectTerms(sL, l, dDiffuse, dSpecular, dSubsurface, ndl);
        const float3 lightTerm = l.radiance * ndl;
        dAcc += dDiffuse * lightTerm + dSubsurface * l.radiance;
        sAcc += dSpecular * lightTerm;
    }
    diffuse  += dAcc * vis;
    specular += sAcc * vis;
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

// MILESTONE 3. The one number CSRdRefl needs from PSRayDriven's material block before deciding whether
// to trace a reflection ray: the hit's ROUGHNESS. TRANSCRIBED FROM PSRayDriven'S OWN MATERIAL BLOCK
// (AVER_RT_BINDLESS branch of "THE STOCK MATERIAL AT A RAY HIT", below) -- uvS/averRtUvGrad/the slot-1
// metal-rough sample/the AVER_MAT_SLOPE_BLEND layer-1 blend/the final clamp copied verbatim, keeping only
// the statements roughness depends on. NOT a full AverSurface rebuild: mapBase/occlusion/emissive/normal
// feed PSMainVoxi/PSRayDriven's shading but never `s.rough`, and CSRdRefl never reads them back, so
// computing them here would be dead work.
//
// KEEP IN STEP WITH PSRayDriven's OWN COPY BY HAND (same rule as rdSurfaceFromRecord above, same reason:
// no shared statement). A future roughness edit not mirrored here silently gives CSRdRefl's gate and
// rtReflectionTemporal a stale value, while the shade pass's own s.rough (still computed in full; still
// what the cone/sky fallback below uses) moves on without it.
//
// WHY STAGE B CAN'T JUST RE-CHECK `rough <= 0.75` INSTEAD OF READING gRdReflTex's alpha: it has no cheap
// way to know CSRdRefl's roughness without redoing this reconstruction -- exactly the register-pressure
// cost splitting reflection out of the shade pass exists to remove (see CSRdRefl's header). Reading
// gRdReflTex[pixel].a instead asks the stage that already paid for this answer, once.
float rdSurfaceRoughness(RdSurface s, float3 rdRayDx, float3 rdRayDy) {
#ifdef AVER_RT_BINDLESS
    // THE EFFECTIVE UV and its footprint, same two calls PSRayDriven makes (see that block for why
    // averRtSurfaceUV/averRtUvGrad are the right pair and why the gradient rides the shadow-ray
    // footprint, not a screen-space derivative -- undefined on a ray hit).
    const float2 uvS = averRtSurfaceUV(s.mat, s.inst, s.wpos, s.N, s.hitUV);
    float2 uvGx, uvGy;
    averRtUvGrad(s.mat, s.inst, s.N,
                 gRtVerts[s.i0].pos, gRtVerts[s.i1].pos, gRtVerts[s.i2].pos,
                 gRtVerts[s.i0].uv,  gRtVerts[s.i1].uv,  gRtVerts[s.i2].uv,
                 rdRayDx, rdRayDy, uvGx, uvGy);

    // glTF packs roughness in G, metallic in B, same unpack as PSRayDriven. Only slot 1 (MetalRough) is
    // sampled -- slots 0/2/3/4 feed shading channels this function has no use for.
    const float4 mapMR      = averRtSampleSlot(s.mat, 1, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float2       metalRough = float2(mapMR.g, mapMR.b);

    // SECOND LAYER, blended by SLOPE off the geometric normal -- same predicate/weight as PSRayDriven.
    // Only texIndex[6] (Layer1MetalRough) is transcribed; sibling texIndex[5]/[7] blend mapBase/normalTS,
    // neither returned here.
    if (s.mat.flags & AVER_MAT_SLOPE_BLEND) {
        const float flat01 = saturate(abs(s.N.z));
        const float lw = 1.0 - smoothstep(s.mat.slopeBlendLo, s.mat.slopeBlendHi, flat01);
        if (lw > 0.001) {
            if (s.mat.texIndex[6] != AVER_TEX_UNBOUND) {
                const float2 uv1 = uvS * s.mat.layer1UvScale;
                const float4 mr1 = averRtSampleSlot(s.mat, 6, uv1,
                                                    uvGx * s.mat.layer1UvScale,
                                                    uvGy * s.mat.layer1UvScale, mapMR);
                metalRough = lerp(metalRough, float2(mr1.g, mr1.b), lw);
            }
        }
    }

    return clamp(s.inst.roughness * s.mat.roughnessFactor * metalRough.x, 0.045, 1.0);
#else
    return clamp(s.inst.roughness * s.mat.roughnessFactor, 0.045, 1.0);   // averEvalMaterial's own floor
#endif
}

// How far a view ray travels INSIDE a volume before something stops it, in cm -- averVolumeTransmittance
// needs a path length and a blended surface doesn't know its own thickness. The old fluid shader guessed
// `depthCm / max(abs(V.z), 0.15)` from a constant floor height (FluidShaders.hpp), right only on a box's
// top face; on a side face at pitch 0 the clamp gave an ~8.7m path, blowing the PTTest pit's side out
// white. This measures instead of guessing.
//
// COMMITS EVERY CANDIDATE EXCEPT A FAILED CUTOUT (what FORCE_OPAQUE did here): translucent instances are
// FORCE_NON_OPAQUE, so ordinary traversal would stop AT an uncommitted candidate; averRtProceedSolid
// commits any non-alpha-masked one, so "first thing along the ray" still works, minus cutout holes.
//
// BOTH LANES: either the volume's own back face or an opaque object inside it (rock, pool floor) can end
// the path -- nearest-of-two responds to real geometry, not an authored box height.
//
// Returns 0 when nothing is hit; averVolumeTransmittance reads that as full transmission (an unbounded
// volume shouldn't absorb infinitely).
// Is a real backdrop bound? A null-filled Texture2D reports zero dimensions -- the only in-shader signal,
// avoiding a spare cbuffer component for what the descriptor already tells us. Tested (forcing MSAA's
// null case, confirming glass falls back instead of going black), not assumed.
bool averBlendBackdropValid(out float2 invSize) {
    uint w = 0, h = 0;
    gBlendBackdrop.GetDimensions(w, h);
    const bool ok = (w > 0u && h > 0u);
    invSize = ok ? (1.0 / float2((float)w, (float)h)) : float2(0.0, 0.0);
    return ok;
}

// THE VOLUME COMPOSITE, background as a CORRECTION not a replacement. Want physically:
// final = specular + diffuse*alpha + dst*T*(1-alpha). Hardware premultiplied blend gives
// final = src.rgb + dst*(1-src.a); setting src.a = alpha and solving:
//     src.rgb = specular + diffuse*alpha + bg * (1 - alpha) * (T - 1)
// THE LAST TERM IS THE TRICK: (T-1) is negative, subtracting exactly the light the medium absorbed per
// channel -- what one blend alpha can't express; hardware still adds the REAL destination after. T==1
// zeroes the correction exactly (bit-for-bit identity; the no-absorption regression check leans on it).
// STACKED TRANSLUCENCY DEGRADES GENTLY: `bg` is the scene copied BEFORE any translucent draw (stale for a
// second layer, used only in the correction, base composite still blends against the true `dst`) --
// replacing it outright would let a nearer pane erase a farther one (glass over water). Falls back to
// the scalar composite when no backdrop is bound.
// WHERE THE BACKGROUND IS READ FROM, once the surface bends it. Absorption decides COLOUR; refraction
// decides where it comes FROM -- gIor's first reader (uploaded, unread until now). Returns the UV to
// sample the backdrop at; gGiParams.y is the mode, .z strength, .w edge fade (Settings::refractionMode).
float2 averRefractedBackdropUV(AverSurface s, float3 wpos, float thicknessCm,
                               float2 invSize, float2 screenPos, out bool tir) {
    tir = false;
    const float2 uv0  = screenPos * invSize;
    const uint   mode = (uint)(gGiParams.y + 0.5);
    if (mode == 0u || gGiParams.z <= 0.0) return uv0;

    // WHICH WAY THE LIGHT IS CROSSING decides everything below. eta = index LEFT / index ENTERED:
    // air->medium is 1/n, medium->air is n -- reversed, TIR becomes unreachable (needs eta > 1).
    // GATED ON gCameraMedium, NOT s.backFace -- already cost this engine once (material_prelude.hlsl's
    // post-mortem: a TIR override gated on backFace turned every glass pane into a dark slab at 41
    // degrees off normal, since backFace is also true for a two-sided pane's far surface seen from
    // outside, where Snell forbids TIR and a pixel shader can't tell the difference). gCameraMedium.x is
    // computed CPU-side against the volume's bounds instead; glass (twosided=1) never reaches this branch.
    const float  ior      = max(gIor, 1.0001);
    const bool   eyeInside = gCameraMedium.x > 0.5;
    const float  eta      = eyeInside ? ior : (1.0 / ior);
    float3 R = refract(-s.V, s.N, eta);
    if (dot(R, R) < 1e-6) {
        // refract() returns 0 to say "no transmitted ray exists". Normalising it would be a NaN.
        if (!eyeInside) return uv0;   // from outside this is unreachable; sampling straight through
                                      // stays the honest fallback rather than a fabricated bend.
        // GENUINE TIR: past the critical angle (48.75 degrees at n=1.33) the underside of the water
        // stops being a window and becomes a MIRROR, showing the pool floor instead of the sky.
        // Reflect about the same normal and let the machinery below project it like a refracted target.
        tir = true;
        R = reflect(-s.V, s.N);
    }

    // Where the bent ray leaves the medium: one thickness along the BENT path, not the straight one --
    // why this needs ray-measured thickness, not an authored constant.
    float3 target = wpos + R * max(thicknessCm, 0.0);

#if AVER_RT
    // RAY-TRACED: follow the bent ray to what it ACTUALLY reaches and project that, unlike the
    // screen-space mode which can only offset within the already-captured image. Still reads colour from
    // the backdrop rather than shading the hit (a second full material eval on the priciest pass) -- an
    // off-screen/occluded hit falls back to whatever's there.
    if (mode >= 2u) {
        RayDesc rr;
        rr.Origin = target;
        rr.Direction = R;
        rr.TMin = 0.0;
        rr.TMax = 100000.0;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        rq.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, rr);
        averRtProceedSolid(rq);   // cutouts, not cards -- see averRtProceedSolid
        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            target = target + R * rq.CommittedRayT();
    }
#endif

    const float4 clip = mul(float4(target, 1.0), gViewProj);
    if (clip.w <= 1e-4) return uv0;               // behind the eye: nothing sensible to sample
    const float2 ndc = clip.xy / clip.w;

    // NDC -> THE VIEWPORT RECT, NOT [0,1] OF THE WHOLE TARGET -- the bug that made refraction look like
    // a wrecked image, not a bent one. The editor docks the 3D view in a SUB-RECT (gSceneViewportCur)
    // while uv0 is a full-target UV; plain ndc*0.5+0.5 mixes the spaces, walking the image off-screen
    // near the right edge (the black block in the glass rail). MEASURED by forcing target = wpos
    // (answer had to be uv0 exactly): 100% of the rail's pixels still landed >60px away, proving this was
    // the projection, not refraction (an 8cm pane can't bend 60px) -- rtReprojectHistory and three other
    // sites already did this conversion correctly, one warning the plain form "lands every reprojection
    // on the wrong texel"; refraction was written later and missed it.
    //
    // A ZERO-WIDTH RECT means no viewport was reported this frame -- sample straight through, same
    // fallback as TIR and a target behind the eye.
    if (gSceneViewportCur.z <= 0.0 || gSceneViewportCur.w <= 0.0) return uv0;
    const float2 pxR = gSceneViewportCur.xy +
                       float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewportCur.zw;
    float2 uvR = pxR * invSize;

    // THE EDGE FADE, not cosmetic: an offset walking off-screen samples nothing meaningful, and one onto
    // a foreground object shows that object through the glass -- fades to zero near the border instead.
    // FADED AGAINST THE VIEWPORT RECT, NOT THE TARGET: outside the 3D view the backdrop holds whatever
    // the rest of the frame is -- the real test is "still on the part the camera drew".
    const float fadePx = max(gGiParams.w, 0.0);
    float edge = 1.0;
    if (fadePx > 0.0) {
        const float2 vpMin = gSceneViewportCur.xy * invSize;
        const float2 vpMax = (gSceneViewportCur.xy + gSceneViewportCur.zw) * invSize;
        const float2 d = min(uvR - vpMin, vpMax - uvR);   // to the nearest viewport border, in UV
        edge = saturate(min(d.x, d.y) / fadePx);
    }
    return lerp(uv0, uvR, saturate(gGiParams.z) * edge);
}

// `dstTerm` is the part of the returned rgb that is NOT this surface's own light but the scene behind it,
// sampled from the backdrop copy -- already fogged, so the caller must not fog it again (see THE AIR'S
// OWN LIGHT IS WEIGHTED BY COVERAGE at the caller). Zero when the backdrop is not used. `bgWeight` is how
// much of the already-fogged scene the final pixel holds -- dstTerm's plus the hardware blend's
// dst*(1-alpha) -- or negative for "the blend's own 1 - alpha".
float4 averBlendedOutputBackdrop(AverSurface s, float3 diffuse, float3 specular, float3 T,
                                 float2 screenPos, float3 wpos, float thicknessCm,
                                 out float3 dstTerm, out float bgWeight) {
    dstTerm  = float3(0.0, 0.0, 0.0);
    bgWeight = -1.0;
    float2 invSize;
    if (!averBlendBackdropValid(invSize))
        return averBlendedOutputVolume(s, diffuse, specular, T);

    // TWO SAMPLES, AND THE SECOND IS WHAT MAKES THE CORRECTION CANCEL. Hardware adds dst*(1-alpha)
    // after this returns; the correction must subtract EXACTLY that, at uv0.
    //
    // The single-sample version subtracted the REFRACTED sample instead, so nothing cancelled and the
    // residue (dst - bgRefracted) went NEGATIVE wherever the bent ray landed on something BRIGHTER than
    // what's really behind -- reading as a hue through the tonemap, not dark. That's what the magenta
    // blocks in the pool were. MEASURED: 41773 magenta pixels at the 45-degree pool camera with
    // refraction on, 0 with --refraction 0, 0 after this fix.
    //
    // The algebra, with a == alpha (dst IS bgStraight, so the last two terms collapse):
    //     final = specular + diffuse*a + (bgRefr*T - bgStraight)*(1-a) + dst*(1-a)
    //           = specular + diffuse*a + bgRefr*T*(1-a)
    //   Refraction OFF: bgRefr == bgStraight, collapsing to bg*(T-1)*(1-a) (bit-identical to before);
    //   T==1: (bgRefr - bgStraight)*(1-a), pure bending -- no longer silently wrong with no volume.
    //
    // dst == bgStraight only for the FIRST translucent surface over a pixel; a second one behind glass
    // composites against a backdrop not yet containing the first (backdrop captured once per frame) --
    // bounded to that overlap, unlike the misregistration above.
    bool tir = false;
    const float2 uvR   = averRefractedBackdropUV(s, wpos, thicknessCm, invSize, screenPos, tir);
    const float2 uv0   = screenPos * invSize;
    const float3 bgR   = gBlendBackdrop.SampleLevel(gMaterialSampler, uvR, 0).rgb;
    const float3 bg0   = gBlendBackdrop.SampleLevel(gMaterialSampler, uv0, 0).rgb;
    const float  alpha = saturate(s.alpha);

    // TIR IS NOT A WINDOW WITH A DIFFERENT UV -- treating it as one made it black. Past the critical
    // angle everything is REFLECTED; the window form's (1-alpha) background weight, with alpha pushed
    // toward 1 by grazing-angle Fresnel, multiplied the mirror by roughly zero. MEASURED underwater at
    // 25 degrees: mirrored region read 0.59 mean against the 18.8 of the pit wall it should show -- dark
    // from cancellation, not a dark pool.
    //
    // alpha=1 is correct HERE, unlike material_prelude.hlsl's post-mortem case (a PANE seen from outside,
    // where Snell forbids TIR, with no reflected image -> dark slab): this fires only when gCameraMedium
    // says the eye is inside a single-sided volume, handing back the reflected scene as radiance.
    if (tir) {
        dstTerm  = bgR * T;
        bgWeight = 1.0;   // the reflected scene IS the pixel's scene: alpha 1, but none of it is the pane's
        return float4(specular + dstTerm, 1.0);
    }

    dstTerm  = (bgR * T - bg0) * (1.0 - alpha);
    bgWeight = 1.0 - alpha;
    return float4(specular + diffuse * alpha + dstTerm, alpha);
}

float averVolumeThickness(float3 wpos, float3 N, float3 viewDir) {
    RayDesc r;
    // SAME bias as the reflection ray, same reason (avoid re-hitting the origin surface at t~0). Pushed
    // along the VIEW direction, not N: this ray heads INTO the surface, so an N offset would push it out
    // of the volume being measured.
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + viewDir * bias;
    r.Direction = viewDir;
    r.TMin      = 0.0;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // BOTH LANES, every candidate committed on sight EXCEPT a failed cutout: measures distance to the
    // far side of a medium, so a pane is a real boundary here (averRtProceedSolid preserves FORCE_OPAQUE).
    q.TraceRayInline(gScene, RAY_FLAG_NONE,
                     AVER_RT_MASK_OPAQUE_ALL | AVER_RT_MASK_TRANSLUCENT, r);
    averRtProceedSolid(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;
    return q.CommittedRayT() + bias;
}

// Reprojects wpos through LAST frame's camera to sample the reflection history. False when unusable:
// off-screen, behind last frame's near plane, a texel nothing traced (stored.a <= 0: the vacate
// sentinel), or a disocclusion -- same test as rtReprojectHistory, against the reflection's own depth.
// A RAY THAT ESCAPED TO THE SKY IS REPROJECTED LIKE A HIT: it is one sample of a rough lobe, and keeping
// misses out of the history is what made a semi-rough surface a speckle of raw sky (rtReflection).
bool rtReprojectReflection(float3 wpos, float2 pixel, out float3 hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    int2 texel = int2(floor(px));   // see rtReprojectHistory for why floor, not round
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float4 stored = gRtReflHist.Load(int3(texel, 0));
    if (stored.a <= 0.0) return false;   // not traced there (vacated) -- nothing to reuse
    const float depthM = clip.w * AVER_REFL_HIST_DEPTH_SCALE;
    const float tol = max(depthM, stored.a) * 0.03 + 0.01;
    if (abs(depthM - stored.a) > tol) return false;

    hist = stored.rgb;
    velocityPx = px - pixel;
    return true;
}

// The SPATIAL denoiser for REFLECTIONS -- there was none before (reprojected/blended in time but never
// filtered in space; harmless for a deterministic mirror, not once rtReflection widens into a lobe;
// shipping the lobe alone would trade wrong-but-clean for right-but-noisy).
//
// STRUCTURE IS rtShadowSpatial's (reprojected gather centre, plane-distance rejection -- read that
// first). Differs in three ways:
//  1. RADIUS FROM ROUGHNESS, not a host constant: a fixed width either blurs a mirror or under-filters
//     a rough surface. Roughness 0 returns centre untouched, no texel loaded.
//  2. AN UNTRACED NEIGHBOUR IS SKIPPED, not counted black: gRtReflHist's vacate sentinel is a NEGATIVE
//     alpha, and averaging it in would drag every reflected edge toward black. A neighbour whose ray
//     reached the sky is a real sample and is averaged in.
//  3. NO LUMINANCE WEIGHT: SVGF's colour-similarity term needs a per-pixel VARIANCE this engine doesn't
//     track (history is rgb+depth, no second moment); without it a luminance weight would preserve
//     exactly the noise it's meant to remove. Geometry weights only, for now.
// The AVER_GBUFFER_HISTORY crease term is copied from rtShadowSpatial verbatim. `N` is new, for that
// term only -- rtReflectionTemporal's own normal, unchanged.
float3 rtReflectionSpatial(float3 centre, float3 wpos, float3 N, float2 pixel, float curDepth,
                           float rough, float dzdx, float dzdy) {
    // Radius tracks the lobe (tan(cone)=rough^2 grows quadratically, this grows linearly --
    // deliberately conservative: too wide smears detail, too narrow leaves noise for the temporal
    // history). Capped at 3 (7x7 gather): cost is quadratic in radius.
    // Rounded, not floored, so roughness 0.5 (a common authored value) does not sit on a step edge where
    // the per-triangle mip choice of roughness flips the kernel between 5x5 and 7x7 facet by facet.
    const int radius = rough < AVER_REFL_MIRROR_ROUGH ? 0 : (int)clamp(round(rough * 5.0), 1.0, 3.0);
    if (radius <= 0 || gRtHistParams.y < 0.75) return centre;

    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);

    // Gather around where this pixel WAS last frame (gRtReflHist is last frame's). Same arithmetic as
    // rtReprojectHistory/rtShadowSpatial, including their two landmines: last frame's VIEWPORT rect not
    // [0,1], and floor not round.
    float2 centrePx = pixel;
    const float4 pclip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (pclip.w > 1e-4) {
        const float3 pndc = pclip.xyz / pclip.w;
        if (pndc.z >= 0.0 && pndc.z <= 1.0)
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
    }
    const int2 base = int2(floor(centrePx));

    // A GAUSSIAN falloff, where rtShadowSpatial uses a flat box: box filters ring in frequency response,
    // visible as square-edged plateaus around a highlight. sigma = radius/2 keeps the kernel's support
    // near the requested radius.
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
            if (st.a <= 0.0) continue;   // not traced there; see (2) above
            // Depths in METRES, as the history stores them (AVER_REFL_HIST_DEPTH_SCALE). dzdx/dzdy are
            // the surface PLANE's depth step per pixel (rdPlaneDepthStep), not the ray's.
            const float predicted = (curDepth + dzdx * (float)ox + dzdy * (float)oy) * AVER_REFL_HIST_DEPTH_SCALE;
            const float tol = max(abs(predicted), 0.01) * 0.02 + 0.01;
            if (abs(st.a - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // THE CREASE TERM -- copied from rtShadowSpatial's identical block (see there for the
            // reasoning and cos(60 deg)). ASSUMES gGBufNormalHist matches gRtReflHist's resolution, as
            // gRtShadowHist does -- all three share the scene render target.
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

// Tiled/temporal wrapper around rtReflection(), mirroring rtShadowTemporal's structure/tile schedule so
// shadow and reflection rays amortise on the same cadence. Called ONLY after the caller gates on
// roughness (s.rough <= 0.5 in PSMainVoxi) -- not re-checked here.
// `hit`: true when the return value is the reflection estimate (fresh or history-reused, a surface or
// the sky its rays reached -- see rtReflection), false only when no ray was traced and the caller should
// fall back to sky itself.
//
// SUB-STAGE SPLIT C (Settings::rayDrivenReflSplit): `doSpatial` lets CSRdRefl (R1) call this for the
// ray/history half alone and leave rtReflectionSpatial's dense 7x7 gather to CSRdReflFilter (R2) --
// register-heavy trace and bandwidth-heavy filter no longer share a thread. false ONLY from CSRdRefl's
// AVER_RD_REFL_SPLIT branch; every other caller goes through rtReflectionTemporal() below, always both.
float3 rtReflectionTemporalEx(float3 wpos, float3 N, float3 Ng, float3 R, float3 L, float2 pixel, float rough,
                              float dzdx, float dzdy, bool doSpatial, out bool hit) {
    // ---- THE MIRROR CUTOFF: ONE predicate, three consumers ----
    // Below AVER_REFL_MIRROR_ROUGH a surface is a mirror throughout: no jitter, no temporal history, no
    // spatial filter. tan(cone)=rough^2, so at 0.1 the ray is displaced one part in a hundred of its own
    // length -- under a pixel, with no variance for a filter to remove.
    //
    // A CORRECTNESS FIX, NOT A TUNING KNOB: an earlier version gated the temporal blend on `rough > 0.0`
    // while claiming a smooth surface "still takes the fresh value outright" -- those disagreed for
    // every near-mirror (glass at 0.05), getting an 85%-history blend that can't reduce a variance
    // already at zero and can only add lag, smearing glass behind a moving camera.
    //
    // Deriving all three behaviours from ONE value is the point: a jittered-but-unfiltered lobe is
    // noise, a filtered-but-unjittered ray is blur -- they must agree, which only works reading the same
    // number.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // NO HISTORY TEXTURE: the lobe stays closed too -- widening it with nothing to converge into would
    // trade a biased-but-stable reflection for one that flickers every frame. rough=0 reduces
    // rtReflection to the exact mirror ray it traced before this change.
    if (gRtHistParams.x < 0.5) return rtReflection(wpos, N, Ng, R, L, pixel, 0.0, 0u, hit);

    const float4 curClip = mul(float4(wpos, 1.0), gViewProj);
    // The frame count drives rtReflection's per-frame lobe sample -- a pure count, never wall-clock
    // (reproducible capture).
    const uint frameIdx  = (uint)gRtHistParams.z;
    const uint tileBits = (uint)gRtHistParams.w;
    // WHAT THE HISTORY HOLDS FOR A TRACED PIXEL, hit or miss alike: rtReflection returns the sky along
    // an escaping ray, so a miss is reprojected and filtered with its neighbours instead of standing
    // out as raw sky.
    const float histDepth = curClip.w * AVER_REFL_HIST_DEPTH_SCALE;

    if (tileBits == 0u) {
        // THE SHIPPED PATH: rtPixelsPerRayTileForQuality returns 1 at every tier, so this always runs.
        // Used to trace/write/return with NO temporal blend -- correct for a mirror, wrong once the
        // lobe opened. Blend added HERE, only where variance was introduced: rough=0 still takes the
        // fresh value outright, unchanged for glass, chrome and water.

        // ---- T3 (Settings::rtReflectionHalfRate, console voxi.rtReflectionHalfRate) -- DECIDED BEFORE
        // THE TRACE, same shape as T2 (voxi_rt.hlsli's rtSkyOcclusionTemporal) ----
        //
        // Gated on lobeRough > 0.0 up front: a MIRROR (lobeRough == 0, tanCone == 0 inside rtReflection)
        // always retraces (a reprojected mirror reflection is wrong the instant the camera moves -- no
        // lobe variance to trade against, only a wrong answer).
        //
        // THE REPROJECTION CALL BELOW IS GATED ON THE BIT, UNLIKE T2's: T2's rtReprojectAo already ran
        // every frame regardless of a hit; this one only runs after a successful trace (`if (lobeRough >
        // 0.0 && curHit)`, the `else` branch below), so hoisting it unconditionally would cost every
        // rough pixel whether or not the bit is set. Gating it keeps the clear-bit cost identical to
        // today's, at the price of one possible SECOND rtReprojectReflection call on a skip-eligible
        // pixel whose history just went invalid (a texture lookup, not a ray; the non-skip path already
        // pays this every frame).
        //
        // Tile math is T2's, verbatim: an 8x8, viewport-relative tile (one staged compute thread group),
        // parity alternating by gRtHistParams.z so a tile that skips this frame traces next.
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
            // Same as the tiled branch's "not my turn" case below (the tileBits != 0u path): reuse the
            // reprojected history outright, no ray this frame.
            col    = skipCol;
            curHit = true;
        } else {
            float3 fresh = rtReflection(wpos, N, Ng, R, L, pixel, lobeRough, frameIdx, curHit);
            col = fresh;

            if (lobeRough > 0.0 && curHit) {
                float3 hist = 0.0;
                float2 velocityPx = 0.0;
                if (gRtHistParams.y > 0.75 && rtReprojectReflection(wpos, pixel, hist, velocityPx)) {
                    // Same velocity-discounted shape as rtShadowTemporal: a far-slid sample is the same
                    // surface but not the same point, and full trust smears a comet tail behind motion.
                    // Still camera: full weight. Fast pan: falls back to this frame's spatial filter.
                    const float t = saturate(length(velocityPx) / 6.0);
                    col = lerp(hist, fresh, lerp(0.15, 1.0, t));
                }
            }
        }

        // WRITE THE RAW TEMPORAL VALUE, NEVER FILTERED -- same rule as rtShadowTemporal's history
        // write, else the spatial pass becomes a compounding IIR filter, the reflection slowly
        // dissolving.
        //
        // W6/M5: GATED ON gAverHistoryWrite (on by default, D3) -- see gAverHistoryWrite/
        // averDrawIsTranslucent above and PSMainVoxi below. Before this gate, a blended (glass/water)
        // replay fragment overwrote this texel with the PANE's own reflection (the same double-write
        // class this task's C9 finding names for the RT shadow/AO histories (voxi_rt.hlsli) and the
        // ReSTIR GI histories (voxi_restir.hlsli)). voxi.legacyBlendedHistoryWrite
        // (gAmbientParams.z bit 32) restores the old unconditional write, byte-identical, for A/B.
        //
        // CLAMPED BEFORE IT IS STORED, not only where composed: rtReflection returns reflAlbedo *
        // (direct + ambient) with nothing bounding it (PSRayDriven's own "ONLY UNBOUNDED TERM" comment),
        // and the raw value feeds next frame's 85%-weight reprojection and a 7x7 spatial gather, so one
        // runaway ray (spike, negative-ambient undershoot, NaN) would stay on screen for many frames and
        // spread. clamp(), not min() (a negative radiance is a recorded incident class; clamp also maps
        // NaN to a bound here). The spatial centre below uses this same bounded value, so the split
        // CSRdReflFilter (which reads this texel back) and the unsplit path agree.
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
        // Same adaptive weight as rtShadowTemporal.
        if (curHit && haveHist) {
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            col = lerp(col, hist, weight);
        }
    } else {
        // Not this pixel's turn, reprojection found a real hit: reuse outright, no ray this frame.
        col = hist;
        curHit = true;
    }

    hit = curHit;
    // Raw, not filtered (see the untiled branch's note above). W6/M5: same gate, same clamp, same
    // reason as the untiled branch's copy above.
    col = clamp(col, 0.0, AVER_VOX_MAXRAD);
    if (gAverHistoryWrite)
        gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, histDepth) : float4(0.0, 0.0, 0.0, -1.0);
    if (curHit && doSpatial) {
        return rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy);
    } else {
        return col;
    }
}

// The wrapper every caller but CSRdRefl's split branch uses -- always asks for the spatial gather, so
// PSRayDriven, PSMainVoxi and CSRdRefl's unsplit path stay byte-for-byte the same call as before
// rtReflectionTemporalEx existed.
float3 rtReflectionTemporal(float3 wpos, float3 N, float3 Ng, float3 R, float3 L, float2 pixel, float rough,
                            float dzdx, float dzdy, out bool hit) {
    return rtReflectionTemporalEx(wpos, N, Ng, R, L, pixel, rough, dzdx, dzdy, true, hit);
}

// THE "NO DATA" SENTINEL FOR A PIXEL THE REFLECTION GATE ROUTED AWAY FROM TRACING THIS FRAME. Every
// caller traces only where `rough <= 0.75` (and RT reflections are on), and rtReflectionTemporalEx is
// the only writer of gRtReflHistOut -- so a pixel that failed the gate used to leave its texel holding a
// stale trace from however many frames ago. Roughness is resampled every frame from a footprint-chosen
// mip (and can be animated), so it can flip across 0.75 frame to frame, and when it did,
// rtReprojectReflection found a positive depth that passed its tolerance and blended that stale colour
// back in at up to 85% weight. The same miss sentinel rtReflectionTemporalEx writes makes reprojection
// and rtReflectionSpatial skip it. Guarded the same way as that function's writes: history pair bound,
// and history-write allowed (not PSMainVoxi's blended replay).
void rtReflectionHistoryVacate(float2 pixel) {
    if (gRtHistParams.x >= 0.5 && gAverHistoryWrite)
        gRtReflHistOut[uint2(pixel)] = float4(0.0, 0.0, 0.0, -1.0);
}
#endif

// 3x3 PCF inside ONE cascade's quadrant of the atlas. Returns 1 = lit, 0 = shadowed, -1 = outside
// this cascade so the caller can try the next one.
float shadowSampleCascade(float3 wpos, uint c) {
    float4 lp = mul(float4(wpos, 1.0), gCascadeViewProj[c]);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;

    // The 2x2 atlas layout: quadrant x = c&1, y = c>>1. Must match the C++ side.
    float2 quad = float2(c & 1u, c >> 1u) * 0.5;
    float inset = gShadowParams.x;
    uv = quad + clamp(uv * 0.5, float2(inset, inset), float2(0.5 - inset, 0.5 - inset));

    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z);
    return s / 9.0;
}

// Where shadowing starts fading to unshadowed, as a fraction of the last cascade's reach.
#define AVER_SHADOW_FADE_START 0.84

// Picks a cascade by distance and samples it, offsetting along N by that cascade's bias. Returns
// sun visibility, 1 = fully lit.
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

// Sun visibility for LIGHT INJECTION, from the GI-only shadow map. A SEPARATE FUNCTION from
// shadowFactor: that asks "is this PIXEL in shadow" by camera distance, correct on-screen but wrong for
// a voxel that exists wherever the camera-independent GI volume is. Feeding voxels through the cascades
// forced fitCascades to union its last cascade with the whole GI volume (~14x the area, every draw); one
// box over the volume answers directly instead: no cascade, no camera distance, no fade.
float giShadowFactor(float3 wpos, float3 N, float ndl) {
    // Unusable map: fully lit (still injected, just without sun occlusion) -- same degradation
    // shadowFactor performs when the atlas is missing; giShadowTex_ is a soft dependency, not an
    // init() failure.
    if (gGiShadowParams.y < 0.5) return 1.0;

    float slope = saturate(1.0 - ndl);
    float3 p0 = wpos + N * (gGiShadowParams.z * (1.0 + slope));

    float4 lp = mul(float4(p0, 1.0), gGiShadowViewProj);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    // OUTSIDE THE BOX IS LIT, NOT SHADOWED: the box covers the whole GI volume by construction, so
    // outside it means the voxel is outside the volume too and its radiance is never read.
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return 1.0;

    // No atlas quadrant to inset into: this map is one box filling the whole texture.
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

// ---- CSAirVis: bakes gAirVisOut from gVoxelTex's own occupancy (see gAirVis/gAirVisOut's header,
// above, for what this volume is) ----
//
// PLACED HERE, AFTER voxi_cone.hlsli: reuses voxelUVW/insideVolume rather than reimplementing world<->
// volume-space conversion a third time, so it must sit textually after that file's #include the same
// way this file's PSMainVoxi/PSRayDriven do. UNCONDITIONAL -- not inside #if AVER_RT -- because it backs
// cone-traced GI (no ray tracing needed) as much as ray-driven shading, and its resources are already
// unconditional.
//
// COMPILED WITH THE FULL giLayout(), the same descriptor table/VoxiFrame (b4) binding as
// CSRdVisibility/CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl -- NOT the tiny MipCB (b3) layout
// CSClear/CSResolve/CSMip use above, which has no VoxiFrame to read gVoxelOrigin/gVoxelParams from.
// (VoxiRenderer.cpp's dispatch chooses that root signature; this file has no syntax for "layout," only
// the resource declarations both layouts happen to share.)
//
// SCHEDULED AS A ROUND-ROBIN, NOT PER VOXEL REBUILD: every frame VoxiRenderer::prePass dispatches ONE
// slab of z-layers (the half-open box [gBoxLo, gBoxHi) in MipCB), so the whole volume refreshes every
// few frames at flat, small cost; a full box dispatches once at creation. MEASURED: a full 48^3 pass
// cost ~10 ms, and the voxel volume rebuilds often under camera motion while sky visibility only
// changes with geometry. Nothing accumulates across frames -- a cell is simply recomputed from the
// current voxels -- so a still scene can't flicker.
#define AVER_AIRVIS_RES 32            // gAirVis/gAirVisOut's own resolution (~1.8 m cells over a 56 m GI volume)
#define AVER_AIRVIS_DIRS 16           // directions marched per cell, tiling the upper hemisphere
#define AVER_AIRVIS_MIN_ELEV_DEG 3.0  // Fibonacci hemisphere floor: nothing marches near the horizon
#define AVER_AIRVIS_OCC_GAIN 4.0      // occupancy multiplier so a one-voxel roof averaged into a coarse mip still blocks
#define AVER_AIRVIS_MIN_T 0.01        // stop marching one direction once its transmittance falls below this
#define AVER_AIRVIS_MAX_STEPS 48      // per-direction step ceiling, in case neither of the above fires first

[numthreads(8,8,1)]
void CSAirVis(uint3 tid : SV_DispatchThreadID) {
    // The dispatch covers one slab (or the whole volume); gBoxLo/gBoxHi say which cells it is.
    const uint3 id = tid + gBoxLo;
    if (any(id >= gBoxHi) || any(id >= (uint3)AVER_AIRVIS_RES)) return;

    // Cell centre, world space -- the inverse of voxelUVW (voxi_cone.hlsli): wp = origin + uvw/scale.
    const float3 uvwCell = (float3(id) + 0.5) / (float)AVER_AIRVIS_RES;
    const float3 p = gVoxelOrigin.xyz + uvwCell / gVoxelOrigin.w;

    // One voxel of the GI RADIANCE volume, world units -- what this marches THROUGH, a different grid
    // from the fixed-48 one it WRITES (gVoxelParams.x is the GI volume's own tier-dependent resolution;
    // QualityLadder.hpp). Same formula as traceCone (voxi_cone.hlsli).
    const float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);

    // Half-angle from direction count, same "tile the hemisphere with N cones" derivation as
    // coneTracedIndirect: 2*pi(1-cosHalf) = 2*pi/N covers the hemisphere.
    const float cosHalf = saturate(1.0 - 1.0 / (float)AVER_AIRVIS_DIRS);
    const float halfAngleTan = sqrt(max(1.0 - cosHalf * cosHalf, 1e-6)) / max(cosHalf, 1e-6);
    const float sinMinElev = sin(radians(AVER_AIRVIS_MIN_ELEV_DEG));

    float Tsum = 0.0;
    // A DETERMINISTIC FIBONACCI HEMISPHERE, not random/blue-noise: z stratified evenly over
    // [sin(minElev), 1] (equal-area per step) and azimuth stepped by the golden angle (2.39996323, same
    // constant coneTracedIndirect's ring uses) so directions don't clump. No frame index anywhere: the
    // volume is rebaked, not re-jittered, so it carries no temporal signal to flash on camera motion.
    [loop] for (uint dirIdx = 0; dirIdx < AVER_AIRVIS_DIRS; ++dirIdx) {
        const float z = sinMinElev + (1.0 - sinMinElev) * ((float)dirIdx + 0.5) / (float)AVER_AIRVIS_DIRS;
        const float r = sqrt(saturate(1.0 - z * z));
        const float phi = 2.39996323 * (float)dirIdx;
        const float3 d = float3(r * cos(phi), r * sin(phi), z);

        // Widening cone through the volume, same shape as traceCone but reading OCCUPANCY (alpha,
        // CSResolve's fragment-covered fraction, box-filtered into coarser mips by CSMip) instead of
        // radiance, starting HALF a voxel out rather than traceCone's two -- marching from a cell
        // CENTRE, not a lit surface point, so there's no coplanar voxel to self-hit.
        float T = 1.0;
        float dist = voxelWorld * 0.5;
        [loop] for (uint step = 0; step < AVER_AIRVIS_MAX_STEPS; ++step) {
            if (T < AVER_AIRVIS_MIN_T) break;
            const float3 uvwGi = voxelUVW(p + d * dist);
            if (!insideVolume(uvwGi)) break;   // beyond the volume = open sky: stop, keep T as-is
            const float footprint = max(voxelWorld, dist * halfAngleTan);
            const float mip = log2(footprint / voxelWorld);
            const float occupancy = gVoxelTex.SampleLevel(gVoxelSamp, uvwGi, mip).a;
            // A ONE-VOXEL-THICK ROOF, averaged into a coarse mip alongside open neighbours, reads as
            // barely-there occupancy (CSMip's box filter dilutes it 8x/level) -- OCC_GAIN pushes a
            // thin-but-real occluder back toward fully blocking.
            T *= 1.0 - saturate(occupancy * AVER_AIRVIS_OCC_GAIN);
            dist += footprint;
        }
        Tsum += T;
    }

    gAirVisOut[id] = Tsum / (float)AVER_AIRVIS_DIRS;
}

// ---- SHADE-PASS READ: how much of the sky the air between the camera and wpos can actually see ----
//
// Called from PSMainVoxi/PSRayDriven's fog sites only -- water.hlsl, scene.hlsl and every other
// averApplyFog(Ex) caller keep passing airVis == 1.0 and stay unaffected.
//
// DETERMINISTIC, LIKE CSAirVis ABOVE: 8 FIXED points, no jitter, no per-pixel history (see
// gAirVis/gAirVisOut's header for why a temporally-accumulated per-pixel signal was tried here first
// (surface AO) and reverted for flashing open on camera motion).
float voxiAirVisibility(float3 wpos) {
    // Voxel GI off, or the volume doesn't exist yet: unoccluded air (matches the `gVoxelParams.w >
    // 0.5` idiom used everywhere else in this file for the same question).
    if (gVoxelParams.w <= 0.5) return 1.0;
    uint dimX, dimY, dimZ;
    gAirVis.GetDimensions(dimX, dimY, dimZ);
    // The placeholder is 1x1x1 and cubic, so one axis is enough to tell it apart from a real volume.
    if (dimX <= 1u) return 1.0;

    const float3 camPos = gCamPos.xyz;
    const float segLen = length(wpos - camPos);
    if (segLen <= 1e-4) return 1.0;
    const float3 dir = (wpos - camPos) / segLen;

    // The sampled sub-segment STARTS at the fog's own start distance, not the camera: fog before that
    // distance contributes nothing (averFogFactor's `len <= start` early-out), so a sample there would
    // pull the average toward the near-camera (usually open) answer.
    const float start = min(gFogParams.z, segLen);
    const float span = segLen - start;

    // Height-fog density weighting, mirroring averFogFactor's exponential: a sample deep under the fog
    // should outvote one near its ceiling, or a tall atrium's top would wash out street-level occlusion.
    const float fogK = gFogParams.x;
    const float fogHeight = gFogParams.y;

    float wSum = 0.0, vSum = 0.0;
    [unroll] for (uint i = 0; i < 8u; ++i) {
        const float t = start + (((float)i + 0.5) / 8.0) * span;
        const float3 p = camPos + dir * t;
        const float3 uvw = voxelUVW(p);
        // Outside the volume counts as open sky, same rule CSAirVis's march uses.
        const float v = insideVolume(uvw) ? gAirVis.SampleLevel(gVoxelSamp, uvw, 0.0) : 1.0;
        const float w = fogK <= 1e-8 ? 1.0 : exp(-(p.z - fogHeight) * fogK);
        wSum += w;
        vSum += w * v;
    }
    return wSum > 1e-6 ? vSum / wSum : 1.0;
}

// ================= additive G-buffer: velocity, view-space depth, normal+roughness =================
// Gated on AVER_GBUFFER, following AVER_RT's convention: compile-time define, off by default, so
// PSMainVoxi's/PSRayDriven's ORIGINAL variants fall through unchanged. ADDITIVE: with the define off
// the frame must be bit-identical (checked by the render gate oracle, 18 gates x 9 configurations).
//
// WHY THIS EXISTS: no motion vectors, no G-buffer -- PSMainVoxi returns one SV_TARGET, normal/
// roughness/albedo living only in shader registers. Blocks the vendored FidelityFX denoiser
// (third_party/fidelityfx-denoiser/README.md), FSR 2/3, TAA and screen-space reflections at once, and is
// why temporal reprojection is wrong for moving geometry (rtReprojectHistory and siblings transform
// THIS frame's wpos through LAST frame's camera, valid only for a surface that didn't move).
//
// THIS SLICE ONLY WRITES THE TARGETS -- wiring a consumer (FFX denoiser, FSR3, TAA) is later work, out
// of scope on purpose: unread-but-written is this codebase's recurring shape, deliberately here.
//
// ---- HOW TO DECODE EACH CHANNEL, so encode and decode don't drift apart ----
//   SV_TARGET1 velocity:        RG16F. Texels/frame, DESTINATION (this frame's) minus SOURCE (last
//                                frame's) texel: `prevPixel = thisPixel - velocity`, matching
//                                rhi::UpscalerNeeds::MotionVectors' documented convention.
//   SV_TARGET2 viewZ:           R32F. VIEW-SPACE LINEAR depth (clip.w), NOT the post-projective
//                                [0,1] SV_Position.z/SV_DEPTH a hardware depth buffer stores.
//   SV_TARGET3 normalRoughness: RGB10A2. xy = octahedral world normal, z = roughness, w = 0 --
//                                see averPackNormalRoughness below for the layout.
#if AVER_GBUFFER
struct GBufferOut {
    float4 col              : SV_TARGET0;   // exactly PSMainVoxi's own colour -- unchanged by this define
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
};

// Packs a world-space unit normal and roughness into the RGB10A2 convention above. Shared by
// PSMainVoxi and PSRayDriven (both its sky-hit and sky-miss branches) so the encode is written once,
// not risking divergence.
//
// THE LAYOUT: an octahedral normal (Cigolle et al. 2014, "A Survey of Efficient Representations for
// Independent Unit Vectors") in x/y -- L1-normalise, fold the lower hemisphere over the diagonals,
// then map [-1,1] to [0,1] -- and roughness in z, alone. 10 bits per axis keeps the normal's angular
// error near a tenth of a degree, far below anything the denoiser's edge-stopping or the debug view
// can see. w is spare (always 0).
// The decode is written out again in modules/render.denoise/shaders/aver_denoise.hlsl
// (dnsrDecodeNormal) and sandbox/shaders/gbuffer_debug.hlsl; change the three together.
float4 averPackNormalRoughness(float3 N, float roughness) {
    N /= abs(N.x) + abs(N.y) + abs(N.z);
    float2 p = N.xy;
    if (N.z < 0.0) p = (1.0 - abs(N.yx)) * float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
    return float4(p * 0.5 + 0.5, saturate(roughness), 0.0);
}

// Screen-space motion for the velocity channel, in the SCENE VIEWPORT RECT (same landmine as
// rtReprojectHistory: plain ndc*0.5+0.5 is wrong once the editor docks the 3D view in a sub-rect).
// This is the ONE mapping every velocity helper below shares: a clip position from THIS frame's camera
// (gViewProj) and one from LAST frame's (gPrevViewProj) in, destination-minus-source pixels out.
float2 averClipToVelocity(float4 curClip, float4 prevClip) {
    // Either transform can put this point behind its own near plane -- prevClip routinely does (first
    // frame, or anything that just entered the frustum). Zero is "no motion known", same fallback as
    // rtReprojectHistory's velocityPx -- the least wrong answer when the maths is undefined, rather than
    // an Inf/NaN divide.
    if (curClip.w <= 1e-4 || prevClip.w <= 1e-4) return float2(0.0, 0.0);

    const float2 curNdc  = curClip.xy  / curClip.w;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    // B2 (F6): THIS frame's clip position (curClip, from gViewProj) must land in THIS frame's viewport
    // rect, not last frame's (gViewProj pairs with gSceneViewportCur, gPrevViewProj with
    // gSceneViewport -- see the VoxiFrame struct top of file; the editor can redock the 3D view between
    // frames). THIS WAS WRONG: curPx used to map through gSceneViewport (last frame's rect) like prevPx
    // does below, paired with the CURRENT clip position it should never have shared prevPx's rect at
    // all. Falls back to gSceneViewport when the device had no current rect yet this frame
    // (gSceneViewportCur.w == 0 is that field's documented sentinel, set at VoxiRenderer.cpp's
    // beginShadowHistory) rather than dividing by zero.
    const float4 curRect = gSceneViewportCur.w > 0.0 ? gSceneViewportCur : gSceneViewport;
    const float2 curPx  = curRect.xy +
                          float2(curNdc.x * 0.5 + 0.5, 0.5 - curNdc.y * 0.5) * curRect.zw;
    const float2 prevPx = gSceneViewport.xy +
                          float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    // DESTINATION (curPx) minus SOURCE (prevPx) -- the contract UpscalerNeeds::MotionVectors
    // (RHIResources.hpp) documents.
    return curPx - prevPx;
}

// Motion of a surface point that sat at `wposPrev` last frame and sits at `wpos` now: `wpos` through
// THIS frame's camera minus `wposPrev` through LAST frame's. Camera motion AND object motion both land
// in the result, so a moving instance gets its true velocity instead of the camera-only one.
float2 averGBufferVelocityMoved(float3 wpos, float3 wposPrev) {
    return averClipToVelocity(mul(float4(wpos, 1.0), gViewProj),
                              mul(float4(wposPrev, 1.0), gPrevViewProj));
}

// Camera-only motion: the surface is taken to have stayed where it is (wposPrev == wpos). PSMainVoxi
// (raster) uses this and ONLY this: RASTER REMAINS STATIC-ONLY, a moving mesh drawn by the raster path
// still gets camera-only motion. The per-draw root constants have no room for a previous world matrix
// (docs/rendering/FRAME_INTERPOLATION.md section 4). The ray-driven path, the default, reads
// RtInstance::prevObjectToWorld instead (PSRayDriven via averGBufferVelocityMoved), and the sky has
// averGBufferVelocitySky below.
float2 averGBufferVelocity(float3 wpos) {
    return averGBufferVelocityMoved(wpos, wpos);
}

// A sky pixel: ROTATION-ONLY reprojection of the view direction, a point at infinity. w = 0 in
// float4(dir, 0.0) drops the translation rows of both view-projections, which is exactly right for the
// sky -- moving the camera does not shift the stars, turning it does. Same mapping and the same
// behind-the-near-plane zero fallback as every other velocity, so a sky pixel carries the camera's
// rotation instead of the old hard zero.
float2 averGBufferVelocitySky(float3 dir) {
    return averClipToVelocity(mul(float4(dir, 0.0), gViewProj),
                              mul(float4(dir, 0.0), gPrevViewProj));
}

// One expansion point for PSMainVoxi's several `return` statements, so the three extra channels stay
// identical everywhere instead of by hand per site. Only assembles values already computed once, after
// `s` is built (gbufVelocity/gbufViewZ/gbufNormalRough).
#define AVER_GBUF_RETURN(colorExpr) \
    { GBufferOut aver_gbuf_o; aver_gbuf_o.col = (colorExpr); aver_gbuf_o.velocity = gbufVelocity; \
      aver_gbuf_o.viewZ = gbufViewZ; aver_gbuf_o.normalRoughness = gbufNormalRough; return aver_gbuf_o; }
#else
// Disabled: expands to a plain return, exactly what stood at each site before this feature existed.
#define AVER_GBUF_RETURN(colorExpr) return (colorExpr)
#endif

// ---- B1: DOES A COLOUR ALREADY MARK THIS PIXEL'S DIFFUSE CHANNEL AS POISONED? ----
//
// giRestirIndirect (voxi_restir.hlsli) paints one of SEVEN sentinel colours over its return value --
// never the real indirect diffuse -- whenever gGiRestirParams.w > 0.5 and one of its own guards fired
// this frame (see that function's POISON DEBUG VIEW comment for the legend/precedence). PSMainVoxi/
// PSRayDriven add an EIGHTH, VIOLET, for a different guard -- the ray-traced SPECULAR term's ceiling
// clamp (F5) -- computed in THIS file so it can't sit inside giRestirIndirect's own precedence ladder or
// pre-empt one of its seven returns.
//
// It CAN still collide at the pixel level (same gate, independent channels). PRECEDENCE: a
// giRestirIndirect colour on diffuse always wins over violet -- the seven already carry their own
// internal precedence (a non-finite guard always outranks a mere ceiling hit, per that function's own
// comment) and are the established diagnostic; violet is newer and narrower (specular only), so
// overwriting an existing colour with it would destroy information the seven already spent effort
// ranking. Each call site below reads this back as `giDiffusePoisoned`.
//
// EXACT EQUALITY IS SAFE AND DELIBERATE, not a fragile float compare: all seven colours are built from
// 0.0/0.5/1.0 alone (exact in IEEE754), and none is a value real shaded radiance can produce by
// coincidence -- the same "unmistakable, scene-lighting-cannot-produce-this" design giRestirIndirect
// uses, applied by the reader. Only meaningful right after a giRestirIndirect call (giMode 0's
// coneTracedIndirect never produces one by construction).
bool aver_IsGiRestirPoisonColour(float3 c) {
    return (c.r == 1.0 && c.g == 0.0 && c.b == 1.0)    // magenta: store-time reservoir guard
        || (c.r == 0.0 && c.g == 1.0 && c.b == 1.0)    // cyan: candidate-radiance clamp guard
        || (c.r == 1.0 && c.g == 1.0 && c.b == 0.0)    // yellow: target-pdf guard
        || (c.r == 1.0 && c.g == 0.5 && c.b == 0.0)    // orange: final-estimate guard
        || (c.r == 0.0 && c.g == 0.0 && c.b == 1.0)    // blue: denoiser-readback non-finite guard
        || (c.r == 1.0 && c.g == 0.0 && c.b == 0.0)    // red: raw estimate hit the ceiling
        || (c.r == 0.0 && c.g == 1.0 && c.b == 0.0);   // green: denoised readback hit the ceiling
}

// The Voxi lit pixel shader. Voxi supplies light transport only â€” sun visibility, sky, bounce â€”
// and the material shades it. Returns linear radiance; the post chain tonemaps.
//
// ---- [earlydepthstencil] IS LOAD-BEARING, NOT AN OPTIMISATION ----
//
// This shader WRITES UAVs (gAoHistOut u4, gAoHitDistOut u5, via rtSkyOcclusionTemporal) and calls
// clip(), both of which defeat hardware early-Z, so without this attribute D3D12 moves the depth test
// AFTER the shader: a hidden fragment still runs, traces, and stores. Those stores are plain
// RWTexture2D (not ROV/atomic), so texel ownership is decided by DRAW ORDER, not depth. The scene pass
// rasterises with CullMode::None (VoxiRenderer.cpp, `scene.cull`) and no depth prepass (PTTest), so the
// last writer in an enclosed scene is routinely a surface BEHIND the wall with an open hemisphere (fresh
// trace 1.0), and the visible fragment reads that poisoned texel back at 0.97 history weight, rendering
// a fully-lit floor.
//
// MEASURED, PTTest NewSponza, fog off, fixed exposure, vs a converged path-traced reference (PT view's
// 1.06x stretch corrected; uncorrected, 43% of the apparent error is misalignment): raw trace is CORRECT
// and identical on both primary-visibility paths (raster 1.50, ray-driven 1.51 -- MAD 0.05 between the
// two fields, truth 1.32); let the accumulator run and the same quantity reads 4.62 on ray-driven (a
// fullscreen pass, one invocation per pixel) vs 132.76 here -- an 88x lift, entirely manufactured
// downstream. WITH this attribute: occlusion 132.76 -> 21.27 (gradient returns: 21.27 shadowed vs 59.78
// open, was saturated flat); shaded image 6.43 -> 3.77 MAD from truth; raster-vs-ray-driven PARITY
// 6.05 -> 0.98. JungleRuins, a second and much more open scene, moves the other way for the same reason
// and also improves: 14.06 -> 12.93.
//
// THE DEPTH PREPASS: an earlier "why not use it instead" was measured on a BROKEN prepass
// (--depth-prepass regressed MAD 6.43 -> 21.66, parity -> 20.82, bit-identical denoiser on/off -- nothing was
// shading). Cause: with the device on mesh shaders (RENDER.MESHSHADERS 1), prepassed draws got the
// ORDINARY Less/depth-write pipeline, rejecting the prepass's equal depth and discarding the colour pass
// (47x cheaper for it). Fixed in D3D12Device/VulkanDevice::drawMesh (a prepassed draw now takes the
// input-assembler path its depth was written through). Expected to be the stronger form of this fix once
// verified -- one shaded fragment/pixel, so the AO history is written only by the visible surface, and
// ~8x cheaper (47.51ms scene draws measured without it) -- NOT YET MEASURED FIXED.
//
// THE clip() INTERACTION -- an earlier version of this comment claimed safety here and was WRONG. It
// claimed "there is no alpha-cutout discard here; the cutout lives in PSDepthPrepass", checked only by
// searching this file for clip(); it missed material_prelude.hlsl's alpha-cutout clip
// (`AVER_MAT_ALPHA_MASK`) inside averEvalMaterial. Under forced early depth, the depth WRITE happens
// before the shader runs, so that clip discarded a cutout texel's colour but not its depth -- every
// leaf/fence hole wrote opaque depth and hid what was behind it. "Verified by capture on both scenes;
// neither lost geometry" was true and did not test it -- a whole-image comparison can't see a few
// percent of foliage pixels; found by adversarial review instead.
// FIXED: an alpha-masked draw never reaches this shader with depth write on -- either the frame-wide
// depth prepass already wrote it (PSDepthPrepass does not force early depth and clips before it
// writes), or the colour walk writes that draw's depth first via IDevice::drawMeshDepthOnly, and either
// way the colour draw uses the LessEqual/NO-WRITE twin, leaving early depth nothing to write (see
// GameRender.cpp's colour loop for residuals -- cluster-dispatched geometry, material graphs driving
// opacity; skinned and soft-body meshes ARE covered, via their posed handle). The translucent ONE-LAYER
// clip stays harmless for the reason originally given: that PSO sets depth.write = false
// (VoxiRenderer.cpp), so there's no depth to leak.
//
// TEMPORAL VALIDATION, because this feeds an accumulated term and still frames cannot judge one:
// matched-pose A/B (--cam-translate 3 --cam-wobble 8 40 --cam-wobble-stop 100, 103 vs 220 frames) puts
// settle-time sensitivity at MAD 0.43 with the attribute vs 0.62 without -- REDUCES temporal dependence.
// High-frequency energy rises 0.39 -> 0.46: the occlusion field regaining real structure, not sitting
// saturated.
#if AVER_GBUFFER
[earlydepthstencil]
GBufferOut PSMainVoxi(VSOut i) {
#else
[earlydepthstencil]
float4 PSMainVoxi(VSOut i) : SV_TARGET {
#endif
    float3 N = normalize(i.nrmWS);
    // FACE-FORWARDED, by averVertexOf's own rule (dot(N,V) < 0 flips), so the ray queries below --
    // sun-shadow origin, AO/GI hemisphere, reflection origin and clamp -- work on the side the eye
    // sees, as the shading normal does. The scene pipeline culls nothing, so a two-sided pane (glass
    // is CULL none) seen from its back used to cast those rays into the half-space behind itself.
    if (dot(N, gCamPos.xyz - i.wpos) < 0.0) N = -N;

    // ---- W6/M5: DOES THIS FRAGMENT'S HISTORY WRITE BELONG TO IT? ----
    //
    // blendedFragment is true only when BOTH hold: the backend replayed translucent draws blended THIS
    // frame (gAmbientParams.w bit 32 -- D3D12 only, C10; clear on every other backend, so this reduces
    // to always-false there with no second #if needed), AND this fragment's material is translucent
    // (averDrawIsTranslucent, above the #if AVER_RT region below; checks AVER_MAT_ALPHA_BLEND and
    // gTransmission).
    //
    // gAverHistoryWrite is the gate everything downstream reads: false only when this fragment is a
    // blended replay AND the legacy override (gAmbientParams.z bit 32, voxi.legacyBlendedHistoryWrite)
    // isn't forcing the old unconditional write back on. Computed HERE, unconditionally (runs whether
    // or not AVER_RT is compiled in), because it must be correct before this function's FIRST history
    // write -- rtShadowTemporal, called a few lines below to build sunVis, writes gRtShadowHistOut
    // immediately. Setting it at the top lets every later gate
    // (reflection, GI diffuse, RT shadow/AO history) see the right value without recomputing it.
    const bool blendedFragment = ((uint)gAmbientParams.w & 32u) != 0u && averDrawIsTranslucent();
    gAverHistoryWrite = !blendedFragment || ((uint)gAmbientParams.z & 32u) != 0u;

    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));
    // SUBSURFACE LIT FROM BEHIND: this pixel's one sun-shadow query starts on the light-facing side,
    // pushed through the surface (averSubsurfaceShadowPush) -- zero for every other material, which then
    // queries exactly as before. The ray-traced query takes the push as a ray-origin offset only, so its
    // history stays at the surface; the shadow map samples the pushed point, facing the light.
    const float3 sssShadowPush = averSubsurfaceShadowPush(gMaterialFlags, gSubsurfaceRadius, N, L);
    const bool   sssShadowBack = any(sssShadowPush != 0.0);
    const float3 shadowMapP    = i.wpos + sssShadowPush;
    const float3 shadowMapN    = sssShadowBack ? -N : N;
    const float  shadowMapNdl  = sssShadowBack ? saturate(dot(-N, L)) : ndl;
#if AVER_RT
    // View-space depth/gradient taken HERE, where control flow is uniform, and carried down to the
    // roughness-gated reflection block: a derivative inside divergent flow is undefined in HLSL (the
    // same landmine rtShadow's dpx/dpy step around) and would present as adapter-specific corruption.
    const float rtViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
    const float rtDzdx  = ddx(rtViewZ);
    const float rtDzdy  = ddy(rtViewZ);
    // M6: rtShadowTemporal's shadow-ray footprint, hoisted here for the same reason: the rdReuse branch
    // below is now PER-PIXEL (a decal a cm off its floor takes it, one a metre off doesn't, and the two
    // can share a quad), so taking ddx/ddy(i.wpos) inside it would be undefined. Taken here instead and
    // carried down as plain float3 arguments.
    const float3 rtDpx = ddx(i.wpos);
    const float3 rtDpy = ddy(i.wpos);

    // ---- M6: REUSE THE STAGED RAY-DRIVEN LIGHTING FOR A TRANSLUCENT PIXEL ON AN OPAQUE SURFACE ----
    // gGiShadowParams.w bit 16 (see that field's cbuffer comment): the staged passes already lit this
    // pixel once for the opaque surface underneath, before this blended fragment ran -- reusing that
    // is strictly cheaper than retracing for the same point. Four gates:
    //   blendedFragment                 -- only a blended-replay fragment (glass/water/decal) qualifies.
    //   bit 16 set                      -- staged textures hold THIS frame's answers and the setting
    //                                       is on; otherwise they're stale/unwritten.
    //   gShadowParams.z > 0.5           -- ray tracing active this frame (same gate the non-reuse call
    //                                       below uses).
    //   !(gAttenuationDistance > 0.0)   -- a VOLUME surface (glass, water) needs its OWN lighting
    //                                       (refracts/attenuates through its own medium); read off the
    //                                       per-draw constant since averEvalMaterial hasn't run yet.
    //                                       VoxiRenderer::blendedDrawReadsBackdrop re-captures the
    //                                       backdrop for exactly this case.
    //   gMaterialGraphId == 0           -- a material GRAPH can drive attenuation per pixel invisibly
    //                                       to the constant above, so graph materials keep their own
    //                                       (same rule blendedDrawReadsBackdrop applies).
    // The last gate -- proving this pixel SITS ON that opaque surface -- needs gRdSunVisTex's texel and
    // rtViewZ, so it's folded into the depth test below instead.
    const bool rdReuseCandidate = blendedFragment
                                && (rtGiShadowBits() & 16u) != 0u
                                && gShadowParams.z > 0.5
                                && !(gAttenuationDistance > 0.0)
                                && gMaterialGraphId == 0u;
    // THE DEPTH PROOF. gRdSunVisTex's alpha is no longer a constant 1.0 -- CSRdShadow now writes the
    // opaque surface's linear view depth there (see that texture's declaration comment), 0.0 where
    // Stage S found no surface. A decal a cm or two above its floor reads a near-identical view depth
    // to the floor Stage S/G/O/R already lit for the SAME pixel; anything further off fails the test
    // and falls back to tracing its own rays. One fetch, reused below for both the depth test and (on a
    // match) the shadow visibility itself.
    const float4 rdSunVisTexel = rdReuseCandidate ? gRdSunVisTex[uint2(i.pos.xy)] : float4(0.0, 0.0, 0.0, 0.0);
    // Tolerance: 1 cm flat plus 0.4% of depth. gRdSunVisTex is RGBA16F, whose mantissa alone steps
    // roughly 2 cm at 30 m -- a tighter flat term would reject a true match to the texture's own
    // storage error, not to a real gap between surfaces.
    //
    // PLUS A PLANE TEST, for grazing views. The depth gap between a decal h above its floor and the
    // floor grows as distance * h / camera height, so from a standing player's eye (~170 cm) the depth
    // test failed every floor-decal pixel past a few metres and each ran the full lighting instead:
    // blended replay 0.28 ms in the editor vs 4.09 ms in PIE on NewSponza (2026-09-28). Measured along
    // the decal's normal the gap is h from any angle. The staged surface point on this pixel's ray is
    // rdPf; its quad neighbours rebuild that surface's plane, which must be parallel to the decal's --
    // along a grazing ray a differently oriented surface can be near along the decal's normal while
    // far away. Same tolerance, projected on the normal. Unnormalised normals, so a degenerate one
    // fails the test instead of producing a NaN.
    bool rdOnPlane = false;
    if (rdReuseCandidate) {   // draw-uniform, so quad-uniform: the quad reads below stay defined
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
    // QUAD-UNIFORM, NOT PER PIXEL: rtShadowTemporal/rtSkyOcclusionTemporal take screen-space
    // derivatives internally, undefined unless all four pixels of a 2x2 quad take the same branch --
    // so a quad reuses only when all four pass; one straddling a decal's edge traces as a whole. The
    // quad reads run here, in uniform control flow, before any branch on the result.
    const uint rdReuseBit = rdReusePx ? 1u : 0u;
    const bool rdReuse = (rdReuseBit & QuadReadAcrossX(rdReuseBit) & QuadReadAcrossY(rdReuseBit) &
                          QuadReadAcrossDiagonal(rdReuseBit)) != 0u;
    // A PANE LIT ON ITS OWN (a blended fragment that cannot reuse the staged lighting -- window glass in
    // front of a room or a street gap) gets the voxel-cone gather's smooth GI and occlusion, not traced
    // ones. It writes no history and reads no denoiser output, and its own history reads are depth-rejected against
    // the opaque surface behind it, so rtSkyOcclusionTemporal returned ONE binary cosine ray re-aimed
    // every frame and giRestirIndirect one raw candidate (RESTIRHISTORY 0). Through
    // averSpecularOcclusion that binary AO is exactly 0 or 1 at glass roughness, so a pane's whole
    // mirror reflection switched on and off per pixel per frame, and rare emitter hits sparkled.
    const bool paneOwnLight = blendedFragment && !rdReuse;

    // float3 NOW: rtShadowTemporal carries the medium's colour. shadowFactor returns a scalar and
    // promotes, so the non-RT path is unchanged.
    float3 sunVis;
    if (rdReuse)
        // Stage S's own answer, VERBATIM (PSRayDriven's AVER_RD_SPLIT copy makes the identical read --
        // search gRdSunVisTex): rgb is CSRdShadow's already temporally/spatially filtered, already
        // tinted sun visibility for the opaque surface this fragment sits on, not a fresh value of its
        // own.
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
    // View-space linear depth for the G-buffer. REUSED, not recomputed, when AVER_RT already computed
    // it as rtViewZ for the reflection block -- both agree on mul(wpos,1,gViewProj).w.
#if AVER_RT
    const float gbufViewZ = rtViewZ;
#else
    const float gbufViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
#endif
#endif
    float ao = 1.0;
    float3 ind = 0;
    // B1: true only when the call below actually took the ReSTIR branch AND it painted one of
    // giRestirIndirect's own seven poison colours over `ind` -- see aver_IsGiRestirPoisonColour's own
    // comment (just above this function) for why exact-equality detection is safe here and what
    // precedence this buys the new specular-ceiling marker further down.
    bool giDiffusePoisoned = false;
    // F4 (R1): true only when the branch below actually took the ReSTIR path, so `ind` is
    // giRestirIndirect's own traced-sky estimate rather than the cone gather's. Only that estimate
    // double-counts the receiver's sky (once through its own traced miss, once more through
    // ind4.ambient below) -- the cone gather never did, so this flag gates the sky-ownership
    // subtraction after the occlusion block to exactly the case that needs it.
    bool restirSuppliedDiffuse = false;
    // True only once coneTracedIndirect has actually written `ao`. Otherwise (ReSTIR GI supplied
    // the bounce, or GI is off) `ao` is still the 1.0 it was initialised with, a constant saying
    // "fully open", and rtSkyOcclusionTemporal must not hand that back as an occlusion answer.
    bool aoGathered = false;
    // GIMODE SWITCHES THE DIFFUSE BOUNCE ESTIMATOR -- see Settings::giMode (Voxi.hpp) for the full
    // contract. Only reachable in the AVER_RT-compiled variant: ReSTIR GI's candidate ray needs the
    // ray-tracing toolkit (gScene, RtInstance, gRtMaterials -- all declared inside this file's own
    // `#if AVER_RT` region) that the non-RT variant never compiles in, so THE DEFAULT PATH (cones,
    // and every image this shader produced before this field existed) IS BIT-IDENTICAL regardless of
    // whether that variant exists -- there is no branch here for it to take.
    // gGiRestirParams.x, NOT the raw Settings::giMode, exactly as giRestirIndirect's own header
    // comment requires: it is 1 only once VoxiRenderer::giRestirWanted() has actually bound t12/u6/u7
    // this frame, so a project requesting ReSTIR GI on hardware that cannot run it falls back to the
    // cone gather here instead of reading a null-filled slot.
#if AVER_RT
    if (gVoxelParams.w > 0.5) {
        // ---- M5: PRICE THE BLENDED PASS'S RESTIR SHARE ----
        // `--blended-gi cone` (voxi.blendedGiCone, gAmbientParams.w bit 16) drops a blended-replay
        // fragment to the voxel-cone gather instead of ReSTIR, purely to MEASURE what ReSTIR's own
        // cost is on the blended pass against the "blended replay" GPU span (D3D12Device.cpp) -- see
        // section 4(a) of the optimisation-wave-2 plan. Default restir (this added test false): the
        // condition collapses to the original `gGiRestirParams.x > 0.5` and today's image is
        // unchanged. blendedFragment is computed once, above, at this function's own top. A pane lit
        // on its own (paneOwnLight) always takes the cone gather: see that flag.
        if (gGiRestirParams.x > 0.5 && !paneOwnLight && !(blendedFragment && ((uint)gAmbientParams.w & 16u) != 0u)) {
            if (rdReuse) {
                // M6: Stage G's own answer, VERBATIM (PSRayDriven's AVER_RD_SPLIT copy -- search
                // gRdGiTex): `ao` is left untouched at its own initial 1.0, exactly what
                // giRestirIndirect's own out-param would have set it to here too (see that function's
                // header) -- so this branch and the traced one below leave `ao` in the same state
                // either way.
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

    // ---- A single-sided BLENDED surface draws ONE layer, not every face it owns ----
    // The scene pipeline does not cull (`scene.cull = CullMode::None`, VoxiRenderer.cpp:3133), so a
    // closed volume rasterises all faces; a blended draw writes no depth, so every surviving face's
    // alpha multiplies. MEASURED: a water box came out ~4 coats thick, alpha 0.02 darkening the pit
    // from (50.7,51.4,49.2) to (36.6,41.9,46.1).
    //
    // GATED ON THE AUTHORED `twosided` FLAG: M_Glass sets it (walked around, keeps both faces), water
    // leaves it 0 (one layer). Author decides. BLENDED ONLY: opaque already gets correct single-layer
    // results from the depth test.
    //
    // dot(N,V), not SV_IsFrontFace: reuses averVertexOf's own backFace, not a second notion that could
    // disagree with the normal flip beside it.
    //
    // INVERTED WHEN THE EYE IS INSIDE THE VOLUME, not switched off: inside a closed volume every face
    // is backFace, so a plain discard deleted the surface outright (swimming under the pool showed no
    // water). gCameraMedium.x is a loose bounding-SPHERE test; inverting rather than disabling keeps a
    // false positive safe (drops the near face, keeps the far one, still ONE layer) -- disabling would
    // resurrect the four-coats bug. Do not "simplify" this into an early-out.
    const bool eyeInside = gCameraMedium.x > 0.5;
    if ((gMaterialFlags & AVER_MAT_ALPHA_BLEND) && !(gMaterialFlags & AVER_MAT_TWO_SIDED) &&
        (eyeInside ? !vtx.backFace : vtx.backFace))
        clip(-1);

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;
    // CAUSTICS go INTO THE SUN TERM: concentrated sunlight, not their own glow, so scaling the sun
    // means a caustic can't appear in shadow (sunVis already zero there) -- the obvious tell of a
    // caustic implementation done wrong.
    // OPAQUE ONLY: the water's own top face sits at the box's max.z and would light itself otherwise.
    if (!(gMaterialFlags & AVER_MAT_ALPHA_BLEND))
        sun.visibility *= 1.0 + averCausticFocus(vtx.wpos);

    AverSurface s = averEvalMaterial(vtx, sun);
#if AVER_GBUFFER
    // Velocity and packed normal/roughness computed once here, after `s` exists but before the first
    // return, so every AVER_GBUF_RETURN site shares identical values. See averGBufferVelocity for
    // what this doesn't yet handle (a moving instance's own motion vs. camera motion).
    const float2 gbufVelocity    = averGBufferVelocity(i.wpos);
    const float4 gbufNormalRough = averPackNormalRoughness(averShadingNormal(s), s.rough);
#endif
    float4 display;
    if (averDisplayColour(s, display)) AVER_GBUF_RETURN(display);

    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 R = reflect(-V, averShadingNormal(s));
    AverIndirect ind4;
    // B1: set below, inside the RT reflection branch only -- see that branch's own comment for why
    // only the RAY-TRACED specular term gets this marker (F5), not the voxel-cone/flat-sky fallbacks.
    bool giPoisonSpecCeilHit = false;
    ind4.ambient      = averSkyIrradiance(averShadingNormal(s));
    ind4.ambientScale = gAmbient.r;
    // TRACED SKY VISIBILITY WHEN THE TIER PAYS FOR IT, the cone gather's estimate otherwise.
    // gAmbientParams.x is already 0 without an acceleration structure (VoxiRenderer gates it on
    // rtActive_), but still needs a COMPILE-TIME guard: rtSkyOcclusion names gScene and lives inside
    // `#if AVER_RT`. Without the guard every non-RT entry point in the file (VSShadow, PSVoxel,
    // CSResolve, ...) fails on an undeclared identifier -- which happened, and a build reported OK
    // because HLSL compiles at RUNTIME here.
#if AVER_RT
    ind4.occlusion    = gAmbientParams.x > 0.5
                      // true: this pass writes the G-buffer the occlusion denoiser reprojects
                      // against (see rtSkyOcclusionTemporal's header; the ray-driven twin passes false).
                      //
                      // M6: Stage O's own answer, when reusing staged lighting AND CSRdSkyOcc ran for
                      // THIS estimator (mirrors CSRdSkyOcc's own condition -- ReSTIR supplies diffuse,
                      // or no voxel GI -- the same way PSRayDriven's AVER_RD_SPLIT read of gRdAoTex
                      // does). In CONE-GI mode CSRdSkyOcc never wrote this texel (occlusion rides the
                      // cone accumulator instead), so this falls through to the same call.
                      //
                      // A pane lit on its own keeps the cone gather's `ao` (1.0, fully open, when voxel
                      // GI is off): see paneOwnLight.
                      && !paneOwnLight
                      ? ((rdReuse && (gGiRestirParams.x > 0.5 || gVoxelParams.w <= 0.5))
                         ? gRdAoTex[uint2(i.pos.xy)].r
                         : rtSkyOcclusionTemporal(i.wpos, N, i.pos.xy, (uint)gAmbientParams.x, ao,
                                                  aoGathered, true))
                      : ao;
#else
    ind4.occlusion    = ao;
#endif
    // F4 (R1): set HERE, after ind4.occlusion exists (not beside ind4.ambient above), since the
    // sky-ownership subtraction needs this frame's traced/gathered occlusion and caching ao twice is
    // wasted work.
    //
    // ONE OWNER FOR THE SKY, at giMode 1: giRestirIndirect's traced miss already gave this pixel its
    // sky (see gAmbientParams.z's cbuffer comment, R1); adding ind4.ambient on top of `ind`
    // double-counts it. Given diffAmbient = (FmsEms+kD)*A and diffBounce = kD*ind4.diffuse
    // (material_prelude.hlsl's averIndirectTerms), subtracting g*A (A = ambient*scale*occlusion*matAO,
    // g = gVoxelParams.y) from `ind` here leaves the total kD*est + FmsEms*A + (1-g)*kD*A -- sky
    // counted once, FmsEms keeping its full irradiance (not zeroed: see PSRayDriven's "ONE OWNER FOR
    // THE ENVIRONMENT" for why zeroing measured worse -- it throws away FmsEms's multi-scatter
    // compensation).
    // Skipped when `ind` is a poison colour (would corrupt the debug view) or never came from
    // giRestirIndirect (restirSuppliedDiffuse false: nothing to remove).
    ind4.diffuse      = ind;
    if (restirSuppliedDiffuse && !giDiffusePoisoned && ((uint)gAmbientParams.z & 2u) == 0u)
        ind4.diffuse = ind - ind4.ambient * ind4.ambientScale * ind4.occlusion * s.occlusion * gVoxelParams.y;
#if AVER_RT
    // Ray traced when the acceleration structure and geometry table both exist; preferred over the
    // cone trace unconditionally since the cone is bounded by the voxel volume and this is not.
    //
    // WHY 0.75, NOT 1.0 (see rtReflection's aperture/lobe history): past ~0.75 rough the lobe is wide
    // enough that one ray estimates a near-hemispherical integral neither filter can close (spatial
    // kernel saturates at radius 3; temporal history rejects itself under motion) -- what that rough a
    // surface reflects is close to its surroundings' average, which the cone/sky term below already
    // returns. 0.75 is where the ray stops being the better answer, not where it stops being affordable.
    //
    // The fade is only a seam-hider across the last quarter (0 at 0.5 rough, 1 at 0.75): nothing pops
    // crossing the cutoff, everything below 0.5 is the traced answer at full strength.
    // The rasterised triangle's own plane (rtReflection's Ng), from the screen derivatives of its
    // position -- taken here, in uniform control flow, not inside the per-pixel roughness branch.
    float3 rNg = cross(ddx(i.wpos), ddy(i.wpos));
    rNg = dot(rNg, rNg) > 1e-12 ? normalize(rNg) : N;
    if (dot(rNg, N) < 0.0) rNg = -rNg;
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(i.pos.xy);   // see that function: no stale history
    if (rtReflTraced) {
        // M6: Stage R's own answer, when reusing staged lighting AND CSRdRefl actually traced for THIS
        // pixel -- rdReflTexel.a > 0.5 is CSRdRefl's OWN decision (gRdReflTex's "THE STAGE'S OWN
        // DECISION"), not re-derived from s.rough. A smooth decal on a surface CSRdRefl did NOT trace
        // (its roughness test can disagree pixel-for-pixel, e.g. the surface below is rougher than
        // 0.75) still falls through to its own ray below.
        const float4 rdReflTexel = rdReuse ? gRdReflTex[uint2(i.pos.xy)] : float4(0.0, 0.0, 0.0, 0.0);
        if (rdReuse && rdReflTexel.a > 0.5) {
            // B1 (F5): CSRdRefl's own PRE-clamp ceiling test, carried in alpha -- not recomputed from
            // rdReflTexel.rgb, which is already clamped and half-float rounded (see PSRayDriven's
            // identical AVER_RD_SPLIT read).
            giPoisonSpecCeilHit = rdReflTexel.a > 1.5;
            ind4.specular = rdReflTexel.rgb;
        } else {
            bool specHit = false;
            float3 refl = rtReflectionTemporal(i.wpos, N, rNg, R, L, i.pos.xy, s.rough,
                                               rtDzdx, rtDzdy, specHit);
            // skyColor(R) only when no ray was traced: the traced estimate already carries the sky its
            // rays reached (rtReflection), so there is no fade toward mirror sky -- see CSRdRefl.
            const float skyW = 0.0;
            float3 skyR = float3(0.0, 0.0, 0.0);
            if (!specHit) skyR = skyColor(R);
            // The raster twin of the clamp at PSRayDriven's copy of this line. Change one, change both:
            // the two primary-visibility paths must agree how much radiance a reflection may return.
            //
            // B1 (F5): PRE-clamp value tested against the SAME ceiling the clamp below enforces, so a
            // pinned pixel is told apart from one that would have landed under it anyway. `>=`, not a
            // negated `<`: NaN compares false either way in HLSL, so this guard is about a FINITE value
            // too large, not corruption (there is no non-finite guard on this term today) --
            // clamp()'s min(max(x,lo),hi) already floors a NaN to 0.0, so a NaN here goes quiet
            // rather than pinned or painted.
            const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
            giPoisonSpecCeilHit = any(specRaw >= AVER_VOX_MAXRAD);
            ind4.specular = clamp(specRaw, 0.0, AVER_VOX_MAXRAD);
        }
    } else
#endif
    if (gVoxelParams.w > 0.5) {
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(i.wpos, R, specAperture);
        // Bounded for the reason coneTracedIndirect is; sky is left alone (not a volume gather, no
        // runaway of its own).
        // SKY IS SKIPPED WHERE THE CONE ALREADY SAW A WALL: HLSL doesn't short-circuit a multiply, so
        // skyColor(R)*(1-sceneSpec.a) ran the full 32-step march even fully occluded -- the common
        // rough>0.75 case (concrete, cloth, stone) paying for a value then multiplied away. Same
        // shape/fix as averFogInscatter's threshold (scene draw 8.9ms -> 1.3ms, 85% of the scene
        // pass): a [0,1]-scaled bounded
        // radiance can be skipped under one 8-bit step with no banding. 0.004 not 0.01, since this
        // weight multiplies a sky far brighter than the fog reference (dropped term <= 0.4% there).
        const float skyWeight = 1.0 - sceneSpec.a;
        ind4.specular       = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
        if (skyWeight > 0.004) ind4.specular += skyColor(R) * skyWeight;
    } else {
        ind4.specular       = skyColor(R);
    }

    // ---- TRANSLUCENT MATERIALS TAKE A SEPARATE, EARLY-RETURNING PATH ----
    // Gated on the flag, not averOpacity(s) < 1: alpha is an authored number that can legally disagree
    // with which PIPELINE/blend-state the draw runs under (a blended twin at alpha 1, or vice versa).
    // AVER_MAT_ALPHA_BLEND is set exactly when routed to the blended PSOs (scenePipeline()) -- the
    // actual decider of PremultipliedAlpha vs. straight composite. Reading the wrong signal packs the
    // wrong kind of output silently: every value stays a plausible colour, just wrong by however
    // translucent the surface is that frame.
    if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) {
        // averShadeSplit is averShadeDirect + averShadeIndirect's IDENTICAL arithmetic, kept as two
        // registers instead of summed -- a blended draw gets the same energy an opaque one would,
        // apportioned between "coverage-weighted" and "always full strength" before alpha.
        float3 dif, spc;
        averShadeSplit(s, sun, ind4, dif, spc);
#if AVER_RT && AVER_RD_LAMPS
        // LOCAL LIGHTS (lamps) on a pane, apportioned as the sun's direct term just was
        // (rdLocalLightsShadeSplit). Shadowed only where this fragment reuses the staged surface under
        // it (rdReuse, same depth proof); UNSHADOWED elsewhere, since a pane has no lamp history of its
        // own and must never write the opaque surfaces' one (u19).
        if (rdLocalLightCount() > 0u) {
            float lampVis = 1.0;
            if (rdReuse) lampVis = rdLocalVisFiltered(uint2(i.pos.xy));
            rdLocalLightsShadeSplit(s, i.wpos, lampVis, dif, spc);
        }
#endif
        // rgb = specular + diffuse*alpha, a = alpha (averBlendedOutput's contract) -- straight alpha
        // would multiply `spc` too, showing a 0.2-opacity pane's reflection at a fifth strength.
        // sceneBlendedPso_'s PremultipliedAlpha blend state expects it packed this way.
        // THE VOLUME, where one is authored: gAttenuationDistance <= 0 is the off state every material
        // had before (averBlendedOutputVolume reduces to averBlendedOutput exactly at T=1).
        // MEASURED WITH A RAY, not an authored height: see averVolumeThickness (the guessed formula
        // made the PTTest pool a bright opaque slab). Behind AVER_RT: without a ray, keeps today's
        // volumeless composite instead of a fabricated thickness.
        float4 outc;
        float3 dstTerm  = float3(0.0, 0.0, 0.0);   // the backdrop's share of outc.rgb; see the fog below
        float  bgWeight = -1.0;                     // the scene behind's weight; < 0: 1 - outc.a
#if AVER_RT
        // FRONT FACES ONLY -- a correctness gate, not an optimisation. averVolumeThickness traces ALONG
        // THE VIEW DIRECTION and takes the nearest hit (the medium's exit), correct only when the
        // shaded point is where the ray ENTERS. A BACK face measures to whatever's behind the glass
        // instead of the pane's few cm -- M_Glass authors twosided=1, so its back face was absorbing
        // over the scene's depth (why an 8cm pane read like a metre of bottle glass).
        //
        // s.backFace, NOT SV_IsFrontFace -- A BUG I SHIPPED: SV_IsFrontFace is winding-dependent;
        // s.backFace is `dot(N,V)<0`, the real question of whether the ray is entering. The fluid box
        // winds opposite the cube, so the winding test called the pool's visible top a BACK face and
        // silently switched water absorption off -- despite this file already stating the rule a few
        // hundred lines up. Absorbing once, on entry, is also physically right.
        // FROM THE SURFACE, NOT THE CBUFFER: a material GRAPH can drive attenuationColor/Distance per
        // pixel; reading gAttenuationColor would silently discard whatever the graph decided.
        if (s.attenuationDistance > 0.0 && !s.backFace) {
            // Measured ONCE, used twice: absorption needs it for Beer-Lambert, refraction for how far
            // the bent path travels.
            const float volThick = averVolumeThickness(i.wpos, N, -s.V);
            const float3 volT = averVolumeTransmittance(
                s.attenuationColor, s.attenuationDistance, volThick);
            // THE BACKDROP PATH: lets attenuationColor's HUE reach the picture -- averBlendedOutputVolume's
            // fallback can only darken absorbed channels, never tint the background (one blend alpha is
            // one number). i.pos.xy is the screen coord the copy uses.
        outc = averBlendedOutputBackdrop(s, dif, spc, volT, i.pos.xy, i.wpos, volThick, dstTerm, bgWeight);
        } else
#endif
        outc = averBlendedOutput(s, dif, spc);

        // ---- THE FOG DECISION ----
        // Fogged HERE, unlike WaterShaders.hpp's PSWater which skips averApplyFog because water's
        // colour is already a Fresnel-blended stand-in for the atmosphere -- fogging it again would
        // double-apply air to a surface meant to read as air. Glass isn't that: `dif`/`spc` are
        // ordinary unfogged PBR terms.
        //
        // WHAT'S ALREADY FOGGED AND MUST NOT BE TOUCHED: the scene behind the pane (its own
        // averApplyFog already ran; PremultipliedAlpha's blend, dst_new = outc.rgb + dst.rgb*(1-alpha),
        // carries `dst` through untouched, and this code never reads dst) -- only `outc.rgb` (this
        // pane's own new light) is fogged below.
        //
        // FOGGING THE PACKED SUM, NOT dif/spc SEPARATELY: both lobes cross the same atmosphere, and
        // averApplyFog is affine (color*T + inscatter) but not distributive over the alpha split --
        // fogging separately then combining would add the inscatter term twice for one slab of air,
        // the same double-counting the water comment above warns about. Alpha (coverage) is untouched
        // by fog either way.
        //
        // OCCLUSION-AWARE: airVis computed once from i.wpos, same as the opaque branch below (see
        // voxiAirVisibility/gAirVis's headers for why this is a world-space volume lookup, not AO).
        //
        // THE AIR'S OWN LIGHT IS WEIGHTED BY COVERAGE, and this is a fix: fogging outc.rgb whole added
        // the in-scatter at FULL weight here, while the hardware blend adds dst*(1-alpha) -- and dst,
        // the scene behind, already carries its own in-scatter. Composited, a pane read
        //     (pane + (1-a)*bg)*T + inscatter*(1 + (1-a))
        // so at alpha 0.12 nearly twice the haze of the wall beside it: distant windows went milky
        // and lighter than the facades they sit in (NeonDistrict, 750 glass placements). averApplyFog
        // is affine, fog(x) = x*T + inscatter, so subtracting (1-a) * fog(0) leaves the extinction on
        // the pane's own light untouched and scales only the in-scatter by its coverage:
        //     pane*T + a*inscatter + (1-a)*(bg*T + inscatter) = (pane + (1-a)*bg)*T + inscatter.
        // AND THE BACKDROP PATH'S SHARE (dstTerm) IS ADDED BACK UNFOGGED: it is the already-fogged scene
        // behind tinted glass or water, re-sampled and bent, not light the pane made -- fogged with the
        // rest it lost a second dose of extinction. In general the in-scatter is weighted by 1 - bgWeight,
        // the share of the pixel that is NOT already-fogged scene: 1 - alpha, except on a TIR return,
        // whose reflected scene is the whole pixel (bgWeight 1) and already carries its own.
        if (bgWeight < 0.0) bgWeight = 1.0 - outc.a;
        float3 airExt, airIn;
        averFogTermsAirVis(i.wpos, true, voxiAirVisibility(i.wpos), airExt, airIn);
        outc.rgb = (outc.rgb - dstTerm) * airExt + airIn * (1.0 - bgWeight) + dstTerm;
        // B1 (F5): applied LAST, after fog, so the marker can't be fogged or blended away -- see
        // aver_IsGiRestirPoisonColour for why a giRestirIndirect diffuse colour outranks this one.
        if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
            outc.rgb = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
        AVER_GBUF_RETURN(outc);
    }

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
#if AVER_RT && AVER_RD_LAMPS
    // LOCAL LIGHTS (lamps) on an opaque surface: resolves/accumulates their visibility itself
    // (rdLocalLightsVisibility, same pixel centre/texel/history gate as the sun's) and shades beside
    // the sun. vtx.N, not raw N: the normal FACING THE EYE, as the ray-driven paths' face-the-ray flip
    // gives theirs, so a two-sided surface's visible side takes the lamp's light and shadow ray. A
    // constant test ahead of the reprojection's derivatives, as rdLocalLightsVisibility requires.
    if (rdLocalLightCount() > 0u)
        radiance += rdLocalLightsShade(s, i.wpos,
                                       rdLocalLightsVisibility(i.wpos, vtx.N, i.pos.xy, uint2(i.pos.xy), true));
#endif
    radiance = averShadeIndirect(radiance, s, ind4);
    // OCCLUSION-AWARE: see the blended branch's identical comment above.
    radiance = averApplyFogAirVis(radiance, i.wpos, true, voxiAirVisibility(i.wpos));
    // B1 (F5): same override/precedence as the translucent branch's copy above.
    if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
        radiance = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
    AVER_GBUF_RETURN(float4(radiance, averOpacity(s)));
}

// ================= ray-driven primary visibility (experimental) =================
// THE ONLY THING THIS REPLACES IS "WHAT DID THIS PIXEL SEE" -- everything after the first hit is the
// same work PSMainVoxi does (sun shadow, sky ambient, fog), since the rasteriser never did that.
//
// WHAT IT GIVES UP: hardware early-Z. A rasterised fragment discovered hidden is discarded before its
// shader runs; a ray pays the whole traversal to learn the same thing (the trade this mode exists to
// measure; see Settings::rtRenderMode).
//
// TEXTURED -- a correction record: this comment claimed the opposite long after it stopped being
// true. Under AVER_RT_BINDLESS a hit samples base colour, metal-rough, normal, occlusion, emissive
// and the slope-blended second layer (RtInstance::materialIndex/gRtMaterials, real gradient
// footprint). The Texture2DArray this used to call "not yet bound" is in register space 1 below.
//
// WHAT STILL DIFFERS, narrower than before: no material GRAPH runs on any ray path (graphId declared,
// never read), and normal-map PERTURBATION is compiled out (AVER_RT_NORMAL_MAPPING 0, built then
// unused). GEOMETRY MUST NOT DIFFER -- what the side-by-side capture checks.
//
// ---- WHAT A TRANSLUCENT SURFACE SHARES WITH THIS PASS, AND WHAT IT STILL DOES NOT ----
// A blended (glass) draw never reaches PSRayDriven, but not because it's missing from the TLAS --
// it's there, routed into the TRANSLUCENT LANE (kRtMaskTranslucent, FORCE_NON_OPAQUE) so a shadow
// ray can attenuate through it. What excludes it from THIS pass is the MASK: the primary ray here
// traces AVER_RT_MASK_OPAQUE only, so widening the mask would start hitting glass with no TLAS
// change. Glass is drawn only where raster mode draws it (D3D12Device::endFrame's blended-mesh
// flush through VSMain+PSMainVoxi's premultiplied-alpha PSO, after this pass's scenePass()) -- depth
// test, blend equation, vertex math and shading are IDENTICAL code paths either way; none of it
// reads rtRenderMode.
//
// WHAT DOES DIVERGE: what a translucent surface reveals. This pass's hit shading is a simpler material
// response than PSMainVoxi's -- maps ARE sampled (base colour, metal-rough, normal, occlusion,
// emissive, second layer), but no material GRAPH runs, normal-map perturbation is compiled out, and a
// stochastic path-traced bounce stands in for the voxel cone trace. These
// approximations, fine for an opaque surface, stopped being invisible once glass put that surface
// behind a window -- see the environment-specular block below for the one piece this change closes
// (voxel-cone GI instead of flat sky) and why the rest is untouched.
//
// BEHIND AVER_RT because RayQuery is: this entry point only compiles into the SM 6.5 variant, and
// VoxiRenderer refuses the mode outright when the device has no ray-query support.
#if AVER_RT
struct RayDrivenOut {
    float4 col   : SV_TARGET;
    float  depth : SV_DEPTH;
};

// ---- PSRayDriven's VIEW-DEBUG COLOUR HELPERS (vmode 2-5, ViewDebug in VoxiRenderer.hpp) ----------
//
// Small, self-contained, kept next to the one entry point that calls them (PSMainVoxi never reads
// gViewParams.x past 1.0/Unlit). NO DERIVATIVES (ddx/ddy/fwidth) ANYWHERE BELOW: PSRayDriven can
// return before these run (the sky-miss branches, above every trace), so a derivative here would read
// whatever neighbouring lane last computed -- undefined at best, a hang at worst on some drivers.
// Every input is a scalar/vector already in hand at the call site instead.

// A well-mixing 32-bit integer hash (Chris Wellons' "lowbias32", a small PCG/Wang-family mix): three
// xorshift/multiply rounds are enough that adjacent instance/material/triangle indices (packed tightly
// by an importer or authoring order) land on unrelated hues instead of a misleadingly gradient-like
// ramp.
uint viewDebugHash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

// Combines two hashed ids into one (Triangles mode: instance index and primitive index need to both
// move the colour, not just the coarser of the two). Feeding the first hash's OUTPUT back through
// viewDebugHash with the second id XORed in is the same "hash the running state" shape rtHash's own
// callers use for combining a pixel with a per-call salt, just on integers instead of rtHash's floats.
uint viewDebugHashCombine(uint a, uint b) {
    return viewDebugHash(viewDebugHash(a) ^ b);
}

// Hash -> hue (fixed saturation/value) -> linear RGB in [0,1]. Saturation/value are constants, not
// hashed: only the hue varies per id, so every id is equally legible instead of some hashing near-
// black or near-white. Standard six-sector HSV->RGB; no derivatives, no dynamic array indexing.
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

// RayHitDistance's log ramp: blue (near) -> cyan -> green -> yellow -> red (far) over ~50 cm to
// ~20000 cm (this file's units are centimetres -- see hitT's own declaration, further down). Log,
// not linear, because a linear ramp over that 400x span would crush every near-camera surface (a
// character, a prop) into the same blue with nothing left to distinguish them.
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

// The one dispatch every hit-pixel call site in PSRayDriven uses (vmode already checked >= 2 by the
// caller). RayHitInstance/RayHitMaterial/Triangles share a hashed-hue shape cue,
// 0.55 + 0.45 * saturate(dot(N, -rayDir)) -- cheap, derivative-free, and just enough of a normal-
// facing term that neighbouring same-hue triangles/instances still read as separate faces instead of
// a flat poster. RayHitDistance (4) ignores N/rayDir entirely: a heat ramp already reads as depth
// without a shading cue, and modulating it would make the ramp's own colours ambiguous with lighting.
float3 viewDebugColor(uint vmode, uint instanceIndex, uint materialIndex, uint primIndex,
                       float hitTCm, float3 N, float3 rayDir) {
    if (vmode == 4u)
        return viewDebugDistanceColor(hitTCm);   // RayHitDistance

    const float shapeCue = 0.55 + 0.45 * saturate(dot(N, -rayDir));
    if (vmode == 2u)      return viewDebugHueColor(viewDebugHash(instanceIndex)) * shapeCue;              // RayHitInstance
    else if (vmode == 3u) return viewDebugHueColor(viewDebugHash(materialIndex)) * shapeCue;              // RayHitMaterial
    else                  return viewDebugHueColor(viewDebugHashCombine(instanceIndex, primIndex)) * shapeCue; // Triangles (5)
}

// PSRayDriven's own G-buffer bundle: same contract as GBufferOut (field comments up by PSMainVoxi)
// plus SV_DEPTH, which this pass writes itself since it answers visibility with a ray and has no
// rasteriser depth to inherit -- same as RayDrivenOut already does without this define.
#if AVER_GBUFFER
struct RayDrivenGBufferOut {
    float4 col              : SV_TARGET0;
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
    float  depth            : SV_DEPTH;
};
#endif

// THIS ENTRY POINT MUST FILL THE G-BUFFER, NOT OPTIONAL: ray-driven primary visibility is now the
// DEFAULT (Settings::rtRenderMode=1), so a rasteriser-only G-buffer would sit empty by default --
// the "built through every layer and nothing fills it" failure this codebase keeps producing. Both
// `return` sites below fill every AVER_GBUFFER channel.
#if AVER_GBUFFER
RayDrivenGBufferOut PSRayDriven(SkyOut i) {
    RayDrivenGBufferOut o;
#else
RayDrivenOut PSRayDriven(SkyOut i) {
    RayDrivenOut o;
#endif

    // W6/M5: EXPLICITLY TRUE, not left to the static's default. A blended (glass/water) draw never
    // reaches this entry point (primary ray traces AVER_RT_MASK_OPAQUE only; glass is drawn by
    // PSMainVoxi's blended replay instead -- see this function's header). Every history write below
    // (the sky-miss surface-history sentinel just below, and the AO hit-distance write further down)
    // is therefore always live for this pass.
    gAverHistoryWrite = true;

    // Same NDC-to-world-ray reconstruction as PSVoxelDebug, through averViewRayDir, so the primary ray
    // and the debug raymarch agree on where a pixel looks.
    float3 dir = averViewRayDir(i.ndc);

    RayDesc r;
    r.Origin    = gCamPos.xyz;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = 1.0e7;

    // gViewParams.x as a small integer mode (see that field's cbuffer comment for the 0-5 legend).
    // Read once, before either trace branch: both branches' own miss handling needs it (a debug view
    // paints its own flat non-surface colour instead of the sky), not only the hit-pixel colour
    // override near this function's return, further down.
    const uint vmode = (uint)(gViewParams.x + 0.5);

#if AVER_RD_SPLIT
    // STAGE B: read CSRdVisibility's record instead of tracing. Mirrors the #else branch's shape by
    // hand (no shared statement), so AVER_RD_SPLIT 0 compiles byte-for-byte unchanged -- see
    // rdSurfaceFromRecord's header for the reconstruction.
    const uint  rdPitch = rdRowPitch();
    const uint2 rdPixel = uint2(i.pos.xy);   // truncates to the integer pixel, CSRdVisibility's own
                                              // buffer-indexing convention
    const uint4 rdRec   = gRdVisBuf[rdPixel.y * rdPitch + rdPixel.x];

    if (rdRec.x == 0xFFFFFFFFu) {
        // Same miss handling as the #else branch's copy below (duplicated, not shared, per above).
        // vmode >= 2 (ray-hit/triangle debug view): paint one flat, obviously-not-a-surface colour
        // instead of the sky, so a miss reads as "no hit" rather than looking like real geometry did
        // hit and happened to sample sky-blue. Explicit if/else, not a ternary, so this branch
        // actually skips the atmosphere march below instead of leaving the compiler free to evaluate
        // both sides.
        if (vmode >= 2u) {
            o.col = float4(0.02, 0.02, 0.04, 1.0);
        } else {
            o.col = float4(skyColorFull(dir), 1.0);
        }
        o.depth = 1.0;
#if AVER_GBUFFER
        // Sky velocity is the camera's rotation-only reprojection of `dir` (averGBufferVelocitySky), no
        // longer a hard zero; viewZ stays the 1e7 far sentinel, which is what a consumer masking sky uses.
        o.velocity        = averGBufferVelocitySky(dir);
        o.viewZ            = 1.0e7;
        o.normalRoughness  = averPackNormalRoughness(-dir, 1.0);
#endif
        if (gGiRestirParams.x > 0.5)
            gGiSurfNrmHistOut[uint2(i.pos.xy)] = float2(0.0, asfloat(0u));
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
    // vmode 2/5's instance/triangle ids: CSRdVisibility packs rtPackCommitted(q) into rdRec.x and
    // q.CommittedPrimitiveIndex() into rdRec.y verbatim -- the packed reference IS the instance's
    // identity (a foliage part's included), RtInstance has no id field of its own to read instead.
    const uint rdInstanceIndex = rdRec.x;
    const uint rdPrimIndex     = rdRec.y;
#else
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Opaque lane. THE ONE RAY USING THE NARROW LANE: AVER_RT_MASK_OPAQUE, not _OPAQUE_ALL -- it
    // starts inside the viewer's own head, so it must not see AVER_RT_MASK_OWNER_HIDDEN. Every other
    // opaque query in this file asks for _ALL.
    //
    // THIS IS PRIMARY VISIBILITY, so the cutout matters most here: whatever it commits is literally
    // what you see. FORCE_OPAQUE made every leaf card a solid rectangle while raster clipped correctly
    // -- and since ray-driven is the standing default, the wrong one was the one on screen.
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE, r);
    averRtProceedSolid(q);

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        // A miss is the sky at the far plane. Depth 1, not 0 (this projection isn't reversed).
        // skyColorFull, NOT skyColor: thrown away whenever a sky dome draws (the deferred dome,
        // D3D12Device.cpp, gated on skyEnabled_ && !frameSuppressed_, is an OPAQUE fullscreen triangle,
        // DepthFunc EQUAL against the 1.0 written below, overwriting every one of these pixels) -- this
        // write is only the no-dome placeholder. MEASURED: paying for the
        // per-pixel march unconditionally overwritten cost up to 41% of a frame once skyColor started
        // honouring the physical model. With the sky disabled, this IS the background instead.
        // vmode >= 2: same flat non-surface colour/march-skipping as the #if AVER_RD_SPLIT branch above.
        if (vmode >= 2u) {
            o.col = float4(0.02, 0.02, 0.04, 1.0);
        } else {
            o.col = float4(skyColorFull(dir), 1.0);
        }
        o.depth = 1.0;
#if AVER_GBUFFER
        // No real surface for a miss, so no true normal. Velocity is the sky's rotation-only
        // reprojection of `dir` (averGBufferVelocitySky; zero only via its near-plane fallback); viewZ
        // 1e7 (sentinel past real geometry, matching this ray's TMax); normal -dir so renormalising
        // gives a unit vector, not a NaN from normalize(0,0,0). NOT A SKY MASK SUBSTITUTE: a consumer
        // excluding sky pixels should use viewZ's far-plane sentinel, not this normal and not a zero
        // velocity (sky velocity is no longer zero).
        o.velocity        = averGBufferVelocitySky(dir);
        o.viewZ            = 1.0e7;
        o.normalRoughness  = averPackNormalRoughness(-dir, 1.0);
#endif
        // A SKY MISS HAS NO SURFACE FOR NEXT FRAME TO REPROJECT EITHER -- write the same 0-packed-normal
        // sentinel giLoadPrevSurface tests for, rather than leaving this pixel's slot holding a
        // stale surface from before the camera panned away (only when the pair is bound this frame;
        // see giRestirIndirect's write for the sentinel's contract). Position doesn't need writing too
        // -- the normal channel's 0 alone is what giLoadPrevSurface tests before it ever reads
        // position.
        if (gGiRestirParams.x > 0.5)
            gGiSurfNrmHistOut[uint2(i.pos.xy)] = float2(0.0, asfloat(0u));
        return o;
    }

    // Surface reconstruction: same barycentric interpolation and ROTATION-ONLY normal transform as
    // rtReflection (why no inverse transpose) -- must agree or a surface shades differently direct
    // vs. in a mirror.
    RtInstance inst = rtLoadInstance(rtPackCommitted(q));
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0  = inst.firstVertex + gRtIndices[tri + 0];
    uint i1  = inst.firstVertex + gRtIndices[tri + 1];
    uint i2  = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w    = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    float3 N    = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(N, dir) > 0.0) N = -N;   // face the ray, so a back-facing hit is not lit from behind

    // UV, same barycentric pattern as `nObj` above; not sampled here yet (no texture array for it).
    // WHOEVER SAMPLES THIS: it's a RAY HIT, not a rasterised fragment -- ddx/ddy is undefined here
    // (same landmine rtShadow's dpx/dpy step around), so use SampleLevel or SampleGrad, never plain
    // Sample().
    float2 hitUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;

    // The hit's own material, keyed by RtInstance::materialIndex (repurposed at no cost -- see that
    // field's own comment) -- what makes the constants below real instead of guessed.
    RtMaterial mat = gRtMaterials[inst.materialIndex];

    const float hitT = q.CommittedRayT();
    float3 wpos = gCamPos.xyz + dir * hitT;
    // vmode 2/5's instance/triangle ids -- same pair the #if AVER_RD_SPLIT branch pulls from its
    // visibility record, read here directly off the live RayQuery.
    const uint rdInstanceIndex = rtPackCommitted(q);
    const uint rdPrimIndex     = q.CommittedPrimitiveIndex();
#endif
    float3 L    = normalize(gLightDir.xyz);

    // ---- THE SHADOW-RAY FOOTPRINT: A RAY DIFFERENTIAL, NOT A SCREEN-SPACE DERIVATIVE ----
    // A zero footprint (point sample, citing rtReflection's inner shadow call as precedent) MEASURED as
    // dense fine speckle on PTTest's pit far wall: aliasing a sub-pixel grazing self-shadow into
    // speckle instead of a true area fraction (PSMainVoxi never shows it -- its ddx/ddy footprint is
    // real).
    //
    // UNLIKE THE REFLECTED CASE, a primary ray's DIRECTION is a smooth analytic function of its pixel
    // (`dir`, via averViewRayDir), so it can be evaluated for the NEIGHBOUR pixel directly -- no
    // ddx/ddy needed. This is a RAY DIFFERENTIAL (Igehy 1999): reconstruct the neighbour's primary-ray
    // direction and see how far it diverged by `hitT` -- a function of the CAMERA/pixel grid, never of
    // what either ray hit, unlike ddx(wpos) across a silhouette.
    //
    // ONLY AS CLEAN AS THE TWO DIRECTIONS: averViewRayDir builds them camera-relative, with nothing
    // eye-sized to cancel however far the camera sits from the world origin.
    //
    // NOTE THE PUNCTUATION: this shader lives inside a C++ raw string literal, so close-paren
    // double-quote ENDS IT -- an earlier draft broke this and cost forty lines of C++ syntax errors.
    // Keep brackets and quotes apart in this file.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float3 dirDx = averViewRayDir(i.ndc + float2(ndcPixelStep.x, 0.0));
    float3 dirDy = averViewRayDir(i.ndc + float2(0.0, ndcPixelStep.y));
    // World-space displacement to the neighbour ray, at the SAME distance this ray travelled -- pixel
    // angular size times hit distance, widening with range.
    const float3 rdRayDx = (dirDx - dir) * hitT;
    const float3 rdRayDy = (dirDy - dir) * hitT;
    // Flattened onto the hit's tangent plane before use as dpx/dpy: rtShadow jitters the ray ORIGIN by
    // these then offsets along N, and an un-flattened footprint could push it off-surface, reopening
    // the grazing-angle acne N*bias exists to close. PSMainVoxi's ddx/ddy(wpos) are real surface points
    // already and skip this; this reconstruction isn't, so it's projected onto N.
    const float3 dpx = rdRayDx - N * dot(rdRayDx, N);
    const float3 dpy = rdRayDy - N * dot(rdRayDy, N);

    // THE SHADOW RAY IS THE SAME CALL THE RASTER PATH MAKES, temporal wrapper included -- identical
    // shadow cost; the timing gap is only primary visibility plus this footprint's matrix multiplies.
    // Keyed by pixel, same grid as the raster pass, so the history buffer means the same thing here.
#if AVER_RD_SPLIT
    // STAGE B reads Stage S's already-resolved sun visibility instead of calling rtShadowTemporal
    // itself (CSRdShadow ran that same call for this pixel). Ablation check repeated, not shared, for
    // the same "no shared statement" reason above.
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float3 sunVis = float3(1.0, 1.0, 1.0);   // ablated: fully lit, no ray
#else
    float3 sunVis = gRdSunVisTex[uint2(i.pos.xy)].rgb;
#endif
#else
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float sunVis = 1.0;   // ablated: fully lit, no ray
#else
    // A subsurface hit lit from behind asks from its light-facing side (PSMainVoxi's twin says why).
    gAverShadowOriginPush = averSubsurfaceShadowPush(mat.flags, mat.subsurfaceRadius, N, L);
    float3 sunVis = rtShadowTemporal(wpos, N, L, i.pos.xy, dpx, dpy, (uint)max(gRtParams.y, 1.0));
    gAverShadowOriginPush = float3(0.0, 0.0, 0.0);
#endif
#endif

    // Lambertian exitant radiance, /PI on the direct term -- see rtReflection for what omitting it
    // cost last time (every sunlit surface 3.14x too bright, an exposure-looking bug the white
    // furnace can't catch since it turns the sun off).
    // ---- the bounce loop ----
    // PATH TRACING HERE IS EXTRA RAYS ON THE LOOP ABOVE, not a second renderer: the first hit is
    // already shaded like rtReflection shades its own hit; each further bounce repeats that, carrying
    // a throughput and adding emission toward the previous surface. COSINE-WEIGHTED so the BRDF's
    // 1/PI and the rendering equation's cosine cancel against the pdf, leaving a plain albedo multiply.
    // SCREEN-PINNED HASH, no per-frame jitter, matching rtShadow's seeding: the gate oracle compares
    // nine configs bit-exactly, so a frame counter would make each one a different image -- noise is a
    // fixed dither here, not something that converges. THE ENGINE'S OWN BRDF: averShadeDirect is the
    // same Cook-Torrance GGX PSMainVoxi uses, so a hand-built AverSurface keeps ray and raster
    // material look agreeing.
    //
    // THREE CONSTANTS USED TO BE DEFAULTED: reflectance/f90/albedo are per-MATERIAL, but a ray hit had
    // only RtInstance (inst.albedo is the raster path's flat per-draw colour; reflectance/f90 sat at
    // textbook dielectric defaults 0.04/1.0). `mat`, via materialIndex/gRtMaterials, is real
    // per-material data.
    //
    // METALLIC/ROUGHNESS DELIBERATELY LEFT ON inst.metallic/inst.roughness, NOT
    // mat.metallicFactor/roughnessFactor, despite the task brief calling the former "already real,
    // per-instance": NOT ALWAYS RIGHT -- for an AUTHORED material inst.metallic/roughness carry the
    // same neutralised-to-1.0 placeholder inst.albedo used to (buildAccelerationStructures,
    // VoxiRenderer.cpp), so an authored metal can render fully rough/metallic regardless of
    // authoring. C++-side fix (source from mat.*Factor at build time) is out of scope here --
    // STATED KNOWN GAP, not believed fixed.
    //
    // ALSO UNVERIFIED: whether gRtMaterials is populated with real per-draw constants (authored
    // .ocmat and the built-in SurfaceLook table alike, vs. a fallback entry) for every path --
    // C++-side (VoxiRenderer.hpp/.cpp), not confirmable from this file.
    AverSurface s = (AverSurface)0;
    s.N        = N;
    s.V        = -dir;
    s.H        = normalize(s.V + L);
    // MULTIPLY THE PER-DRAW VALUE BY THE MATERIAL FACTOR, DO NOT REPLACE IT -- the raster path's
    // model (PbrShaders.cpp: `s.metallic = saturate(gMaterial.x * a.metallic)`); departing from it
    // turned this whole render white.
    // BOTH TERMS ARE LOAD-BEARING: an AUTHORED draw has its per-draw colour/metal/rough NEUTRALISED to
    // 1.0 (factor carries the value); an UNAUTHORED draw has fallback factors of 1.0 (per-draw value
    // carries the colour) -- each is the identity where the other carries data, so the product is
    // right both ways. THE BUG THIS REPLACES: reading `mat.baseColorFactor.rgb` alone shaded every
    // unauthored draw (PTTest's floor, walls, crates) as fallback white -- reported "everything is
    // white".
#ifdef AVER_RT_BINDLESS
    // THE STOCK MATERIAL AT A RAY HIT: six of eight maps, slope-blended second layer. Composed like
    // the raster path -- every factor MULTIPLIES its texel, so a textureless material reduces to the
    // untextured branch and the paths agree. Fallbacks are averSampleMaps' identity values.
    //
    // NO NORMAL MAPPING HERE: this comment used to say "all eight maps" and "normal-mapped shading
    // normal" -- both wrong. Slots 2/7 are sampled below and then DISCARDED: their only reader,
    // averRtPerturbNormal, is behind `#define AVER_RT_NORMAL_MAPPING 0` further down (DXC dead-code
    // eliminates the unread samples, which is why this is corrected rather than deleted). The define
    // is 0 because turning it on measurably made ElectricDreams terrain WORSE; see that #if for the
    // numbers and the leading suspect.
    // THE EFFECTIVE UV, nothing like the mesh's own UV for a world-aligned material.
    const float2 uvS = averRtSurfaceUV(mat, inst, wpos, N, hitUV);
    // ...and the footprint one pixel covers in that same UV space, from the ray differentials this
    // shader already built for the shadow disc.
    float2 uvGx, uvGy;
    averRtUvGrad(mat, inst, N,
                 gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                 gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv,
                 rdRayDx, rdRayDy, uvGx, uvGy);

    float4 mapBase  = averRtSampleSlot(mat, 0, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float4 mapMR    = averRtSampleSlot(mat, 1, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float3 mapNrm   = averRtSampleSlot(mat, 2, uvS, uvGx, uvGy, float4(0.5, 0.5, 1, 1)).xyz * 2.0 - 1.0;
    float  mapOcc   = averRtSampleSlot(mat, 3, uvS, uvGx, uvGy, float4(1, 1, 1, 1)).r;
    // WHITE, NOT BLACK, like every slot above: s.emissive = mat.emissiveFactor * mapEmis multiplies,
    // so an unbound map must be the identity. Black made every emissiveFactor-only material (a lamp
    // bulb with no texture) render no glow.
    float3 mapEmis  = averRtSampleSlot(mat, 4, uvS, uvGx, uvGy, float4(1, 1, 1, 1)).rgb;
    // glTF packs occlusion in R, roughness in G, metallic in B -- the same unpack averSampleMaps does.
    float2 metalRough = float2(mapMR.g, mapMR.b);
    float3 normalTS   = float3(mapNrm.xy * mat.normalScale, mapNrm.z);

    // THE SECOND LAYER, blended by SLOPE off the GEOMETRIC normal, not the normal-mapped one: "is
    // this a cliff" is a surface property, and a normal map would make the choice flicker per bump.
    if (mat.flags & AVER_MAT_SLOPE_BLEND) {
        const float flat01 = saturate(abs(N.z));
        const float lw = 1.0 - smoothstep(mat.slopeBlendLo, mat.slopeBlendHi, flat01);
        if (lw > 0.001) {
            const float2 uv1 = uvS * mat.layer1UvScale;
            if (mat.texIndex[5] != AVER_TEX_UNBOUND)
                mapBase = lerp(mapBase, averRtSampleSlot(mat, 5, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, mapBase), lw);
            if (mat.texIndex[6] != AVER_TEX_UNBOUND) {
                const float4 mr1 = averRtSampleSlot(mat, 6, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, mapMR);
                metalRough = lerp(metalRough, float2(mr1.g, mr1.b), lw);
            }
            if (mat.texIndex[7] != AVER_TEX_UNBOUND) {
                const float3 n1 = averRtSampleSlot(mat, 7, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, float4(0.5, 0.5, 1, 1)).xyz * 2.0 - 1.0;
                normalTS = normalize(lerp(normalTS, float3(n1.xy * mat.normalScale, n1.z), lw));
            }
        }
    }

    // NORMAL MAPPING LAST, only when a map is bound (identity normalTS is a no-op costing a tangent
    // solve otherwise).
    // OFF -- A MEASURED DECISION. MAD against raster, whole viewport:
    //                              base colour only   + these slots, no normals   + normal mapping
    //   PTTest (authored flats)          32.08                17.23                    18.21
    //   ElectricDreams (terrain)          6.06                 7.48                    17.19
    // Costs a little on authored surfaces, catastrophic on terrain -- worse than no textures at all.
    // RULED OUT: frame orthonormality (nTS=(0,0,1) reproduces N exactly), map decode (Z saturated
    // positive), normalScale (1.0), layer-1 blend (no change alone). LEADING SUSPECT: tangent frame
    // rotated WITHIN the tangent plane (nTS=(0,0,1)->N only proves T/B perpendicular to N, not T along
    // +U); or raster doesn't perturb terrain either, in which case raster is the wrong reference and
    // this needs a flat surface with a known-good normal map judged against the path tracer (which
    // gained working normal mapping with a derived tangent frame). Flip to 1 to measure; do not ship
    // at 1 until terrain is explained.
#define AVER_RT_NORMAL_MAPPING 0
#if AVER_RT_NORMAL_MAPPING
    if (mat.texIndex[2] != AVER_TEX_UNBOUND || mat.texIndex[7] != AVER_TEX_UNBOUND) {
        s.N = averRtPerturbNormal(mat, inst, N, normalTS,
                                  gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                                  gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv);
        // The perturbed normal must still face the ray: a normal map can tip a grazing normal past
        // the horizon, shading from behind as a false shadow.
        if (dot(s.N, dir) > 0.0) s.N = -s.N;
    }
#endif

    s.albedo    = inst.albedo * mat.baseColorFactor.rgb * mapBase.rgb;
    s.emissive  = mat.emissiveFactor * mapEmis;
    // THROUGH occlusionStrength, as averBuildSurface does. Full strength (tried first) darkened
    // ElectricDreams terrain from 111,101,96 to 73,72,76 against a raster reference of 104,102,100 --
    // moved further from raster.
    s.occlusion = lerp(1.0, mapOcc, mat.occlusionStrength);
#else
    s.albedo   = inst.albedo * mat.baseColorFactor.rgb;
    // The factor alone: no texture table on this compile. This branch never set it, so
    // averShadeIndirect added whatever the register held -- see the HAND-SET rule below.
    s.emissive = mat.emissiveFactor;
#endif
#ifdef AVER_RT_BINDLESS
    // metalRough is the SAMPLED pair, glTF-unpacked (.x roughness/green, .y metallic/blue), multiplied
    // onto the factors as averStockAuthored does, so an unbound map contributes 1.0.
    s.metallic = saturate(inst.metallic  * mat.metallicFactor  * metalRough.y);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor * metalRough.x, 0.045, 1.0);
#else
    s.metallic = saturate(inst.metallic * mat.metallicFactor);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor, 0.045, 1.0);   // averEvalMaterial's own floor
#endif
    s.ndv      = saturate(dot(s.N, s.V));
    s.f90      = mat.f90;
    s.reflectance = mat.reflectance;
    // HAND-SET: HLSL doesn't zero-initialise a struct, so omitting these feeds averDirectTerms
    // garbage off the stack. A PRIMARY RAY LEAVES THE EYE, so its first hit is always a front face --
    // no refracted ray, no exit interface, TIR cannot arise.
    s.backFace  = false;
    s.sssWeight = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceWeight) : 0.0;
    s.sssRadius = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceRadius) : 0.0;
    // Not left at the (AverSurface)0 above: a zero tint would switch subsurface off on this path alone.
    s.sssColor  = max(mat.subsurfaceColor, 0.0);
#ifdef AVER_LAYERED_BSDF
    // THE COAT NEEDS EXPLICIT LINES: a new field silently defaults to 0 (the right OFF state) if
    // omitted here, which would look correct while meaning "no coat in ray-driven mode ever".
    s.coatWeight = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatWeight)    : 0.0;
    s.coatRough  = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatRoughness) : 0.0;
    s.coatF0     = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatF0)        : 0.0;
#endif
    // Identical shape to averBuildSurface's F0, mat.reflectance standing in for gMatReflectance (this
    // pass's own material buffer, no per-draw cbuffer binding here).
    s.F0       = lerp(mat.reflectance.xxx, s.albedo, s.metallic);
    s.F        = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
    // (1 - transmission), as averBuildSurface does (PbrShaders.cpp, kdAlbedo line): light passing
    // THROUGH the substrate can't also scatter back out, or the material invents energy. Kept
    // identical on both paths deliberately -- a rule honoured by raster but not primary rays is the
    // exact defect shape this tree keeps rediscovering. A BLENDED pane never arrives here (glass is
    // drawn by PSMainVoxi in both modes), so this fires only for an OPAQUE material authoring
    // transmission -- why the rule is against the material field, not the blend mode.
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo * (1.0 - saturate(mat.transmission));
    s.model    = AVER_MODEL_STANDARD;
    s.alpha    = 1.0;
#ifndef AVER_RT_BINDLESS
    // The textured variant sampled a real occlusion map above; this default would overwrite it.
    s.occlusion = 1.0;
#endif

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;
    // The ray-driven twin of the caustic term above. No blended test: this pass shades opaque primary
    // hits only.
    sun.visibility *= 1.0 + averCausticFocus(wpos);

    const uint bounces = (uint)max(gPtBounceParams.x, 1.0);

    float3 radiance = averShadeDirect(0.0, s, sun);

    // LOCAL LIGHTS (lamps): every lamp in range through the sun's own BRDF (rdLocalLightsShade), diffuse
    // and specular, times the lamps' accumulated shadow-ray visibility. Inside `radiance` only, so it
    // stays out of the denoiser/AO/ind terms below, and Unlit (vmode 1), which replaces `radiance`
    // wholesale, drops it with the rest of the lighting. The count is a constant-buffer value, so each
    // branch is uniform and costs nothing with no lamps.
#if AVER_RD_SPLIT && !AVER_RD_SINGLE_PASS
    // Stage B: the visibility CSRdLocalLights already resolved for this pixel, filtered across its
    // neighbours on the same surface.
    if (rdLocalLightCount() > 0u)
        radiance += rdLocalLightsShade(s, wpos, rdLocalVisFiltered(uint2(i.pos.xy)));
#elif !AVER_RD_SPLIT && AVER_RD_LAMPS
    // The single pass resolves (and accumulates) it here, on its own hit. `N` faces the ray; the pixel
    // centre/texel are the sun history's own (rtShadowTemporal above).
    if (rdLocalLightCount() > 0u)
        radiance += rdLocalLightsShade(s, wpos,
                                       rdLocalLightsVisibility(wpos, N, i.pos.xy, uint2(i.pos.xy), true));
#endif

    // THE ENVIRONMENT THROUGH THE ENGINE'S OWN INDIRECT TERM, not a diffuse-only line. A diffuse-only
    // line here had two faults, found by the white furnace: a white metal rendered black (0.003 vs
    // correct 1.000, since averShadeIndirect was never called, so the specular env term didn't exist),
    // and path tracing doubled the energy (1.000 -> 1.977: this line added the sky once, the bounce
    // loop added it again on every escape) -- every path escapes on bounce one in an open scene, so a
    // bounce-depth sweep came back flat and hid the double-count.
    //
    // ONE OWNER FOR THE ENVIRONMENT: this call; the bounce loop no longer adds sky on escape, only
    // surface-to-surface light. Zeroing ind.ambient instead (tried first) measured worse: FmsEms
    // multiplies irradiance, so zeroing it threw the multi-scatter compensation away (white metal fell
    // to 0.971@rough0.05 -> 0.450@rough1.0) -- FmsEms is specular energy a path-traced bounce doesn't
    // carry. Also matches raster structurally: full unoccluded sky ambient plus bounced light on top,
    // without subtracting the sky that bounce occludes -- same approximation, same place.
    float3 R = reflect(dir, N);
    AverIndirect ind;
    ind.ambient      = averSkyIrradiance(N);
    ind.ambientScale = gAmbient.r;

    // ---- DIFFUSE INDIRECT: THE CONE TRACE, exactly as PSMainVoxi does it ----
    // Replaces a one-sample stochastic bounce that FROZE noisy, not merely noisy: its direction came
    // from a pure function of the pixel with no frame term (needed for the gate oracle), so every
    // pixel kept one wrong direction forever -- dense static speckle on enclosed surfaces, none in the
    // open (an escaped ray adds nothing, variance zero; inside a room every ray lands on a different
    // wall, variance enormous) -- the concrete pit reported "still broken" while the sky nearby was
    // clean. coneTracedIndirect (what PSMainVoxi always used) is DETERMINISTIC (prefiltered clipmap
    // march, no variance) and hands back a real AO factor the bounce loop never provided (`ind.occlusion
    // = 1.0` below was a stated gap), making both paths answer this the same way.
    //
    // COST, MEASURED: an earlier version of this comment claimed the cone trace was "cheaper as well as
    // cleaner". It is not. PTTest (voxelRes 512, GI Epic): ray-driven primary 1.3-1.4ms -> 3.0-4.0ms,
    // whole frame 4.13ms median -> 8.86ms -- a 6-cone gather over a 512^3 clipmap costs more than three
    // ray-query traversals with idle ray-query units.
    //
    // STILL THE RIGHT TRADE, for parity not speed: this is the cost PSMainVoxi always paid for the
    // same term -- ray-driven wasn't cheaper before, it was doing something worse and charging less.
    //
    // WHERE THE COST GOES: this used to say giCones was six, hardcoded -- already false when written.
    // Renderer::giConesForQuality (Voxi.cpp) has covered Off/Medium=6, Low=3, High=9, Epic=13 since
    // commit fbb3aad, wired through gGiParams.x into the dynamic loop. PTTest pins RENDER.GI 4 (Epic,
    // 13 cones), so any cost number here is this pass's worst rung.
    //
    // CONE COUNT IS NOT WHERE THE TIME GOES -- MEASURED: arithmetic predicted dropping Epic to a lower
    // rung would recover 1.5-2ms of ~3.7ms; it does not. Same camera, dropping Epic (13 cones) to High
    // (9 cones): Epic 3.7-3.8ms/6.498ms whole frame, High 3.5-3.8ms/6.066ms -- four fewer cones
    // bought ~0.4ms of 6.5ms. Real but not dominant; **the 3x gap against the rasteriser remains
    // unexplained**. Already tested; don't shave cones on this theory.
    //
    // THE MARKER THIS ASKED FOR ALREADY EXISTS: this used to say there is no GPU-timed marker for the
    // raster path's own pixel shading and call for one to be added. It is there and always was:
    // D3D12Device::beginGpuSpan("scene draw") brackets every drawMesh the raster path issues, and
    // "Voxi ray-driven primary" is a CHILD of that same span, so the two are already like-for-like.
    //
    // THE GAP IS SMALLER THAN 3x SUGGESTS: this file's AVER_RD_ABLATE header records ray-driven
    // primary at ~6.7ms of 14.55ms against raster's 7.82ms with RT on in both -- rasterising primary
    // visibility can only recover that gap, not the whole ray-driven-primary span (which is the entire
    // deferred shade: shadow, cones, reflection, sky-occlusion, both sky marches, fog). Compare with
    // `--gpu-timing` on `--rt-render-mode 1 --pt 0` vs `--rt-render-mode 0 --pt 0`, checked against
    // the "is painting the scene" log line first (a suppressed scene reads "scene draw" ~0.1ms because
    // nothing drew, not because nothing costs -- D3D12Device.cpp's own suppression warning documents
    // exactly that run).
    float rdAo  = 1.0;
    ind.diffuse = 0.0;
    // B1: mirrors PSMainVoxi's copy (aver_IsGiRestirPoisonColour) -- true only when the ReSTIR branch
    // below painted one of giRestirIndirect's seven colours over ind.diffuse.
    bool giDiffusePoisoned = false;
    // F4 (R1): mirrors PSMainVoxi's restirSuppliedDiffuse -- true only when the ReSTIR branch supplied
    // ind.diffuse, the only estimator that double-counts this receiver's own sky.
    bool rdRestirSuppliedDiffuse = false;
    // Mirrors PSMainVoxi's aoGathered: true only once coneTracedIndirect has written rdAo.
    bool rdAoGathered = false;
#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no cone gather
#else
    // GIMODE'S OWN SWITCH, exactly as PSMainVoxi's copy: gGiRestirParams.x (never the raw
    // Settings::giMode) says whether giRestirWanted() bound t12/u6/u7 this frame; no non-RT variant to
    // guard here since this whole function is already `#if AVER_RT`.
    if (gVoxelParams.w > 0.5) {
        if (gGiRestirParams.x > 0.5) {
#if AVER_RD_SPLIT
            // STAGE B: read CSRdGi's already-resolved diffuse estimate instead of calling
            // giRestirIndirect itself. rdAo is left at its initial 1.0 -- exactly what
            // giRestirIndirect's own `ao` out-param would set it to, so both branches agree.
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

    // ---- ENVIRONMENT SPECULAR: A REAL MIRROR RAY NOW, GATED THE SAME WAY PSMainVoxi GATES ONE ----
    // Used to always fall through to PSMainVoxi's cone/flat-sky fallback. Found while chasing a
    // translucency bug: a glass pane's blended replay composites over whatever this pass painted, so
    // an opaque hit under glass in ray-driven mode was a flatter answer than PSMainVoxi's sharp mirror
    // ray for a qualifying surface -- not a compositing defect (blend/depth/Fresnel unchanged), but the
    // "broad, flat sheet" look the glass/reflection audit traced to this branch.
    //
    // THE OLD COST OBJECTION ("a fourth ray every pixel") doesn't survive gating identically to
    // PSMainVoxi (`s.rough <= 0.75`): same ray, same subset of surfaces the rasteriser already prices
    // into the comparison this mode exists to make. Rough surfaces (most of a scene) stay on the cheap
    // cone/sky path.
    //
    // THE GATE IS PSMainVoxi's PREDICATE VERBATIM (`gShadowParams.z > 0.5 && gRtParams.w > 0.5 &&
    // s.rough <= 0.75`), so the two passes agree on which surfaces earn a mirror ray. Both terms are
    // already true whenever this pass runs at all (rayDrivenActive() requires rtActive_, which sets
    // gShadowParams.z); kept anyway as the one place either pass is told "no" if the geometry table
    // ever legitimately fails while the TLAS still exists.
    //
    // rtReflectionTemporal's dzdx/dzdy reuse the shadow footprint's ray differential (`rdRayDx`/
    // `rdRayDy`) rather than rederiving one: they only predict a REPROJECTED NEIGHBOUR's depth for
    // rtReflectionSpatial's plane-rejection, so the tangent-plane projection isn't needed --
    // `mul(float4(rdRayDx,0.0), gViewProj).w` reads the directional part of clip.w exactly, one extra
    // matrix multiply, cheaper than a second ray differential.
    //
    // EXPECTED COST, STATED not measured: rtReflectionTemporal shares its tile schedule with the
    // shadow ray this pass already pays for, gated to a roughness minority. Nearest reference: the
    // wave-bound-shadow finding's ~2.0ms for one tile-amortised full-screen ray-query pass on
    // ElectricDreams at full coverage; gated to a minority, real cost should land well under that.
    // B1 (F5): set below, inside this branch only -- see PSMainVoxi's identical copy for why only the
    // RAY-TRACED specular term gets this marker, and aver_IsGiRestirPoisonColour's own comment for
    // the precedence against giDiffusePoisoned above.
    bool giPoisonSpecCeilHit = false;
#if AVER_RD_SPLIT
    // MILESTONE 3, STAGE B: read CSRdRefl's already-resolved reflection instead of re-deciding
    // roughness and calling rtReflectionTemporal here (register pressure -- the split compile must
    // contain no such call).
    //
    // THE GATE HERE IS DELIBERATELY MISSING ITS ROUGHNESS TERM: rdRefl.a already encodes that
    // CSRdRefl's OWN gate (which includes roughness) passed for this pixel -- see gRdReflTex's "THE
    // STAGE'S OWN DECISION". What's repeated here only guards against a frame CSRdRefl never
    // dispatched (stale/placeholder texel), same reason CSRdGi/CSRdSkyOcc's reads repeat theirs.
    const float4 rdRefl = (gShadowParams.z > 0.5 && gRtParams.w > 0.5)
                         ? gRdReflTex[uint2(i.pos.xy)] : float4(0.0, 0.0, 0.0, 0.0);
    if (rdRefl.a > 0.5) {
        // B1 (F5): CSRdRefl's own PRE-clamp ceiling test, carried in alpha (2.0 = traced AND over the
        // ceiling) -- NOT recomputed from rdRefl.rgb, which is already clamped and half-float rounded,
        // so a test against it could disagree with the single pass's test against the unclamped value.
        giPoisonSpecCeilHit = rdRefl.a > 1.5;
        ind.specular = rdRefl.rgb;
    } else if (gVoxelParams.w > 0.5) {
        // PSMainVoxi's OWN voxel-cone fallback (rough > 0.75, or RT unavailable): past that roughness
        // a one-ray estimate can't resolve a near-hemispherical lobe. Duplicated from the #else
        // branch's identical copy below, not shared across the #endif, for the same "no shared
        // statement" reason PSRayDriven's trace block gives above.
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
#if AVER_RD_ABLATE == AVER_RD_ABL_SPECCONE
        // ablated: no specular cone. FULLY OPAQUE (alpha 1) rather than empty, so skyWeight below
        // goes to 0 and this mode measures the CONE ALONE -- an alpha of 0 would instead hand the
        // whole branch to skyColor and measure a march this mode is not trying to price.
        float4 sceneSpec    = float4(0.0, 0.0, 0.0, 1.0);
#else
        float4 sceneSpec    = traceCone(wpos, R, specAperture);
#endif
        // Same fix as PSMainVoxi's identical branch above: HLSL doesn't short-circuit the multiply,
        // so skyColor(R)*(1-sceneSpec.a) wastes a 32-step march when occluded. 0.004 threshold, same
        // reasoning as averFogInscatter's (measured there: "8.9ms -> 1.3ms, 85% of the scene pass").
        const float skyWeight = 1.0 - sceneSpec.a;
        ind.specular        = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
#if AVER_RD_ABLATE == AVER_RD_ABL_ROUGHSKY
        // ablated: the rough branch's atmosphere march. Mode 4 covers only the REFLECTION branch's;
        // this is the call an enclosed scene actually reaches.
#else
        if (skyWeight > 0.004) ind.specular += skyColor(R) * skyWeight;
#endif
    } else {
        ind.specular        = skyColor(R);
    }
#else
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(i.pos.xy);   // see that function: no stale history
    if (rtReflTraced) {
        const float rdReflDzdx = mul(float4(rdRayDx, 0.0), gViewProj).w;
        const float rdReflDzdy = mul(float4(rdRayDy, 0.0), gViewProj).w;
        bool specHit = false;
#if AVER_RD_ABLATE == AVER_RD_ABL_REFL || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        float3 refl = float3(0.0, 0.0, 0.0);   // ablated: no mirror ray
#else
        // N stands in for the geometric normal here: this single-pass compile sits at the AMD driver's
        // register limit (rtGiShadowBits' note), and rebuilding the triangle plane costs three more
        // transformed vertices. The staged CSRdRefl, the default, passes the real one.
        float3 refl = rtReflectionTemporal(wpos, N, N, R, L, i.pos.xy, s.rough,
                                           rdReflDzdx, rdReflDzdy, specHit);
#endif
        // skyColor(R) only when no ray was traced: see CSRdRefl's compose.
        const float skyW = 0.0;
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march
#else
        if (!specHit) skyR = skyColor(R);
#endif
        // CLAMPED: THIS WAS THE ONLY UNBOUNDED TERM LEFT IN A SHADOWED PIXEL (rtReflection returns
        // reflAlbedo*(direct+ambient) with nothing bounding it; the cone-traced twin below already does
        // min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD)). WHY IT SHOWS IN SHADOW: diffAmbient/
        // diffBounce are both bounded there, and averIndirectTerms' FssEss/specOcc are both bounded by
        // ~1, so specEnv is the term left standing and inherits this magnitude unchanged -- one
        // reflection ray escaping a dark interior through a window was the entire pixel. MEASURED:
        // residual outliers predominantly BRIGHT, 49x more frequent in dim regions than with ray
        // tracing off.
        //
        // clamp() not min(), deliberately: min bounds above only, and a NEGATIVE radiance historically
        // rendered wrong here (BRIGHT, since acesTonemap(-1) used to equal 1.0; it now floors at zero,
        // color.hlsli since ded8784a, so the same mistake would render confident BLACK instead --
        // silent, a stronger reason to floor it HERE). The voxel injection's own write already uses
        // this two-sided form.
        //
        // B1 (F5): same PRE-clamp ceiling test as PSMainVoxi's copy (see there for the NaN-safe `>=`).
        const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
        giPoisonSpecCeilHit = any(specRaw >= AVER_VOX_MAXRAD);
        ind.specular = clamp(specRaw, 0.0, AVER_VOX_MAXRAD);
    } else if (gVoxelParams.w > 0.5) {
        // PSMainVoxi's OWN voxel-cone fallback (rough > 0.75, or RT unavailable): past that roughness
        // a one-ray estimate can't resolve a near-hemispherical lobe.
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
#if AVER_RD_ABLATE == AVER_RD_ABL_SPECCONE
        // ablated: no specular cone. FULLY OPAQUE (alpha 1) rather than empty, so this mode measures
        // the CONE ALONE (alpha 0 would hand the branch to skyColor instead).
        float4 sceneSpec    = float4(0.0, 0.0, 0.0, 1.0);
#else
        float4 sceneSpec    = traceCone(wpos, R, specAperture);
#endif
        // Same fix as PSMainVoxi's branch above: HLSL doesn't short-circuit, so skyColor(R)*(1-sceneSpec.a)
        // would waste a 32-step march when occluded. 0.004 threshold, same reasoning as
        // averFogInscatter's (measured there: 8.9ms -> 1.3ms, 85% of the scene pass).
        const float skyWeight = 1.0 - sceneSpec.a;
        ind.specular        = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
#if AVER_RD_ABLATE == AVER_RD_ABL_ROUGHSKY
        // ablated: the rough branch's atmosphere march (mode 4 covers only the REFLECTION branch's);
        // this is the call an enclosed scene actually reaches.
#else
        if (skyWeight > 0.004) ind.specular += skyColor(R) * skyWeight;
#endif
    } else {
        ind.specular        = skyColor(R);
    }
#endif
    // REAL AMBIENT OCCLUSION NOW, from the same cone march as the diffuse term -- used to be a
    // hardcoded 1.0 ("a traced bounce is its own occlusion", true only for a converged path tracer).
    //
    // AT EPIC `rdAo` IS COMPUTED AND THEN DISCARDED, deliberately: the line below prefers traced sky
    // visibility there, so the cone's own occlusion goes unused on the tier that pays most for it.
    // NOT worth restructuring to skip: the cones still trace for `ind.diffuse`, so the only saving is
    // the per-cone `occ += c.a * w` accumulation (~2 FMAs x 13) against 13 marches of up to 24 samples
    // each. MEASURED: the whole cone gather is 4.05ms of a 50.16ms frame, a rounding error inside that.
    // Threading a `wantAo` flag through a function mirrored in two files (voxi.hlsl and voxi_gi.hlsli,
    // byte-for-byte) to save it would cost more than it returns.
    // Same substitution as PSMainVoxi's, and must be: the two primary-visibility paths currently
    // agree to 2.23 MAD, worth keeping.
#if AVER_RT && AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    ind.occlusion    = gAmbientParams.x > 0.5
#if AVER_RD_SPLIT
                     // STAGE B: in the two cases CSRdSkyOcc actually ran for (ReSTIR supplies diffuse,
                     // or no voxel GI -- see its header), read its resolved answer instead of tracing
                     // again. In CONE-GI mode CSRdSkyOcc never wrote this texel (occlusion rides the
                     // cone accumulator instead), so this falls through to the same call below.
                     ? ((gGiRestirParams.x > 0.5 || gVoxelParams.w <= 0.5)
                        ? gRdAoTex[uint2(i.pos.xy)].r
                        : rtSkyOcclusionTemporal(wpos, N, i.pos.xy, (uint)gAmbientParams.x, rdAo,
                                                 rdAoGathered, false))
#else
                     // false: this pass runs with the G-buffer OFF, so gDenoisedAo was reprojected against
                     // motion vectors/depth this pass never wrote. Reading it anyway was the whole of
                     // the washed-out ray-driven shadows: it overrode a correctly traced "fully
                     // occluded" with ~0.83 "open", and full sky ambient then landed on every interior
                     // surface (measured in rtSkyOcclusionTemporal's header).
                     ? rtSkyOcclusionTemporal(wpos, N, i.pos.xy, (uint)gAmbientParams.x, rdAo,
                                              rdAoGathered, false)
#endif
                     : rdAo;
#else
    // ablated (or no ray tracing): the cone gather's own occlusion -- so this mode measures the RAY,
    // not the presence of ambient occlusion.
    ind.occlusion    = rdAo;
#endif
    // F4 (R1): PSMainVoxi's twin, applied after ind.occlusion is final and before averShadeIndirect
    // reads ind -- see ind4's copy above ("ONE OWNER FOR THE SKY") for the full identity.
    if (rdRestirSuppliedDiffuse && !giDiffusePoisoned && ((uint)gAmbientParams.z & 2u) == 0u)
        ind.diffuse -= ind.ambient * ind.ambientScale * ind.occlusion * s.occlusion * gVoxelParams.y;
    const float aoView = ind.occlusion * s.occlusion;   // ViewDebug::AmbientOcclusion (vmode 6)
    radiance = averShadeIndirect(radiance, s, ind);

    // THE BOUNCE CARRIES THE DIFFUSE RESPONSE, not raw albedo: a metal reflects almost nothing
    // diffusely, so throughput*basecolour would light an interior off surfaces that don't bounce it.
    float3 throughput = s.kdAlbedo;
    float3 bp = wpos;
    float3 bn = N;
    // SKIPPED ENTIRELY WHENEVER THE CONE TRACE ALREADY ANSWERED THIS (both compute the same
    // surface-to-surface bounce; running both would double every interior's brightness). Left
    // reachable for GI-off and for measuring the estimator, but it's one cosine sample/pixel/bounce
    // from a per-pixel-fixed hash with no history to accumulate into -- correct but far too
    // undersampled until it gets per-frame decorrelation and a history buffer. Until then the cone
    // trace above is the better answer.
    const bool rdConeSuppliedDiffuse = gVoxelParams.w > 0.5;
    [loop] for (uint b = 1; b < bounces && !rdConeSuppliedDiffuse; ++b) {
        // A cosine-weighted direction about the surface normal, from the same rtHash the shadow
        // disc uses. Two hashes for the two dimensions, decorrelated by offsetting the pixel.
        float u1 = rtHash(i.pos.xy + float2(b * 17.0, 0.0));
        float u2 = rtHash(i.pos.xy + float2(0.0, b * 23.0));
        float r   = sqrt(u1);
        float phi = 2.0 * PI * u2;
        float3 t  = normalize(abs(bn.z) < 0.999 ? cross(float3(0,0,1), bn) : cross(float3(1,0,0), bn));
        float3 bt = cross(bn, t);
        float3 dirB = normalize(t * (r * cos(phi)) + bt * (r * sin(phi)) + bn * sqrt(max(0.0, 1.0 - u1)));

        RayDesc rb;
        const float bbias = max(gRtParams.z, 1e-4) * (1.0 + length(bp - gCamPos.xyz) * 5e-4);
        rb.Origin = bp + bn * bbias;
        rb.Direction = dirB;
        rb.TMin = bbias;
        rb.TMax = 1.0e7;

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qb;
        // Opaque lane only -- see AVER_RT_MASK_OPAQUE.
        // Same proof as the primary ray above: this mask cannot see a non-opaque candidate.
        qb.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, rb);
        averRtProceedSolid(qb);

        if (qb.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            // Escaped: path ends, deliberately without adding sky (averShadeIndirect already gave
            // this surface the full environment; re-adding it made path tracing read 1.977 vs the
            // furnace's 1.000). The loop carries bounced light off SURFACES only.
            break;
        }

        RtInstance bi = rtLoadInstance(rtPackCommitted(qb));
        uint btri = bi.firstIndex + qb.CommittedPrimitiveIndex() * 3;
        uint b0 = bi.firstVertex + gRtIndices[btri + 0];
        uint b1 = bi.firstVertex + gRtIndices[btri + 1];
        uint b2 = bi.firstVertex + gRtIndices[btri + 2];
        float2 bbary = qb.CommittedTriangleBarycentrics();
        float3 bw = float3(1.0 - bbary.x - bbary.y, bbary.x, bbary.y);
        float3 bnObj = normalize(gRtVerts[b0].nrm * bw.x + gRtVerts[b1].nrm * bw.y + gRtVerts[b2].nrm * bw.z);
        float3 bnWS = normalize(mul(float4(bnObj, 0.0), bi.objectToWorld).xyz);
        if (dot(bnWS, dirB) > 0.0) bnWS = -bnWS;

        bp = bp + dirB * qb.CommittedRayT();
        bn = bnWS;
        // The hit's own emission, weighted by the path so far and NOT by this hit's albedo (emitted
        // light doesn't bounce off the surface it leaves), so a bounce landing on a lamp bulb carries
        // its glow. The factor alone, no emissive map. NOT IN THE SINGLE-PASS COMPILE: that megakernel
        // is at the AMD driver's register limit (see rtGiShadowBits() in voxi_rt.hlsli), and this loop
        // only runs with GI off anyway. A lamp the lamp term above already lights directly adds
        // nothing here (rdLocalCarriesEmitters), or the first bounce would count its light twice --
        // same rule as giTraceInitialCandidate's.
#if !AVER_RD_SINGLE_PASS
        const RtMaterial bmat = gRtMaterials[bi.materialIndex];
        if (!((bmat.flags & AVER_MAT_LIGHT) != 0u && rdLocalCarriesEmitters()))
            radiance += throughput * bmat.emissiveFactor;
#endif
        // Diffuse response again: a cosine-weighted bounce samples the DIFFUSE lobe, so a metal
        // correctly contributes almost nothing.
        throughput *= (1.0 - saturate(bi.metallic)) * bi.albedo;

        // ONE shadow ray per bounce, no disc -- the penumbra of a surface seen only through two
        // diffuse bounces is not resolvable, and this is the single largest cost in the loop.
        float bshadow = rtShadow(bp, bn, L, i.pos.xy, float3(0,0,0), float3(0,0,0), 1u, 0.0);
        float3 bdirect = averSunRadiance() * saturate(dot(bn, L)) * bshadow / PI;
        radiance += throughput * bdirect;
    }

#if AVER_RD_ABLATE == AVER_RD_ABL_FOG
    // ablated: no aerial perspective and no fog inscatter march -- and no gAirVis lookup either,
    // so this branch still measures the true zero-fog-work cost the ablation exists to isolate.
#elif AVER_RD_ABLATE == AVER_RD_ABL_AERIAL
    // ablated: the aerial march only. Height fog still runs, so the delta against mode 0 is
    // this one term and not the pair. OCCLUSION-AWARE: airVis computed once, just for this branch's
    // own call -- see voxiAirVisibility's own header comment for why this is a world-space volume
    // lookup and not the surface's own AO.
    radiance = averApplyFogAirVis(radiance, wpos, false, voxiAirVisibility(wpos));
#else
    radiance = averApplyFogAirVis(radiance, wpos, true, voxiAirVisibility(wpos));
#endif

    // Depth for everything that draws AFTER the scene (deferred sky, transparentPass, particles) --
    // without it they sort against a cleared buffer, putting smoke in front of walls.
    float4 clip = mul(float4(wpos, 1.0), gViewProj);
    o.depth = clip.w > 1e-6 ? saturate(clip.z / clip.w) : 1.0;
    // UNLIT SUBSTITUTES THE COLOUR AND NOTHING ELSE. Handled here, not through gShadingModel, because
    // a ray hit has no per-draw cbuffer to carry the mode in (the raster path carries it in the b1
    // block PSMainVoxi reads; this pass never binds it).
    //
    // AN OVERRIDE RATHER THAN AN EARLY RETURN: every AVER_GBUFFER channel below still has to be
    // written (the struct's "fills only some outputs" warning) -- returning above them would leave
    // velocity, viewZ, normal-roughness and SV_DEPTH unwritten. s.albedo is the SAMPLED base colour;
    // shading a base-colour CONSTANT made this mode pure white on every textured mesh.
    // Unlit is albedo PLUS emissive: removes the lighting, not the surface's own light, so a lamp
    // bulb still glows (same rule as displayColor for the raster Unlit view, averBuildSurface).
    o.col   = float4(vmode == 1u ? s.albedo + s.emissive : radiance, 1.0);   // vmode 1 == Unlit (see
                                                                  // gViewParams's own cbuffer comment
                                                                  // for the legend)
    // B1 (F5): applied LAST, after the unlit substitution and fog/shading upstream, so this is
    // unconditionally the final colour whenever it fires (see PSMainVoxi's identical override for the
    // precedence against giDiffusePoisoned -- a giRestirIndirect colour on the diffuse channel wins;
    // aver_IsGiRestirPoisonColour's own comment has why). Takes precedence over unlit too: giPoisonView
    // is an explicit diagnostic and shouldn't go dark just because unlit is also active.
    if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
        o.col.rgb = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
    // vmode 2-5 (ViewDebug's ray-hit/triangle views): replace the final colour LAST, after both
    // overrides above, so selecting one always shows exactly that debug encoding. Every other output
    // below (G-buffer MRTs, depth, history writes) is untouched: the denoiser/history still see valid geometry,
    // only what's on screen changes (see viewDebugColor's header).
    if (vmode == 6u)
        o.col.rgb = aoView.xxx;
    else if (vmode >= 2u)
        o.col.rgb = viewDebugColor(vmode, rdInstanceIndex, inst.materialIndex, rdPrimIndex, hitT, N, dir);
#if AVER_GBUFFER
    // clip.w IS the view-space linear depth viewZ wants, reused from o.depth's divide above rather
    // than a second mul. Velocity carries OBJECT motion: the hit's object-space point is mapped through
    // this frame's and last frame's objectToWorld, and the DELTA is applied to wpos, so the current
    // projection is exactly today's wpos and an instance with prevObjectToWorld == objectToWorld (static,
    // foliage, first frame) gets a delta of exactly 0 and today's camera-only result bit for bit.
    // RIGID MOTION ONLY: skinned/soft-body deformation is not in prev (gRtVerts holds the CURRENT pose),
    // raster stays static-only (see averGBufferVelocity), and translucent layers write no velocity by
    // design (VoxiRenderer.cpp). Normal is this pass's ray-hit N, not an interpolated vertex normal --
    // what this feature's task asked for.
    const float3 objPos  = gRtVerts[i0].pos * w.x + gRtVerts[i1].pos * w.y + gRtVerts[i2].pos * w.z;
    const float3 curObjW = mul(float4(objPos, 1.0), inst.objectToWorld).xyz;
    const float3 prvObjW = mul(float4(objPos, 1.0), inst.prevObjectToWorld).xyz;
    o.velocity        = averGBufferVelocityMoved(wpos, wpos + (prvObjW - curObjW));
    o.viewZ            = clip.w;
    o.normalRoughness  = averPackNormalRoughness(N, s.rough);
#endif
    return o;
}

// ---- STAGED RAY-DRIVEN COMPUTE STAGES: pixel -> NDC -> primary-ray direction, ONE PLACE ------------
//
// Pixel-centre NDC, the exact inverse of the ndc->pixel mapping this file and voxi_rt.hlsli use
// everywhere (this file's rtReprojectReflection; voxi_rt.hlsli's rtReprojectHistory/rtReprojectAo/
// rtAoSpatial/rtShadowSpatial): px = viewport.xy + float2(ndc.x*0.5+0.5, 0.5-ndc.y*0.5) * viewport.zw.
// Solved for ndc at the pixel CENTRE, it equals what VSky/SkyOut (modules/rhi/shaders/
// shared_prelude.hlsl:967-975) interpolates there -- this is that transform's inverse.
//
// gSceneViewportCur, NOT gSceneViewport: the latter is LAST frame's rect (paired with gPrevViewProj,
// for reprojection), and on a resize/redock would map a stage onto the old pixel grid while Stage B
// shades the new one. The rect the rasteriser used for i.ndc/i.pos.xy THIS frame is this one. (A
// shadow-ray FOOTPRINT still needs LAST frame's grid on purpose -- CSRdShadow keeps gSceneViewport
// separately.)
//
// FACTORED OUT OF CSRdVisibility AND CSRdShadow (each inlined this before this task) into one
// function used by all four staged stages (CSRdVisibility, CSRdShadow, CSRdGi, CSRdSkyOcc), so a
// future mapping change can't update three and disagree in the fourth. The math is byte-for-byte
// what each of the first two already computed. `ndc` comes back alongside `dir` because the
// shadow-ray footprint (CSRdShadow's and PSRayDriven's) needs it.
float3 rdPrimaryRayDir(uint2 pixel, out float2 ndc) {
    const float2 pxC = float2(pixel) + 0.5;
    ndc.x = (pxC.x - gSceneViewportCur.x) / max(gSceneViewportCur.z, 1.0) * 2.0 - 1.0;
    ndc.y = 1.0 - (pxC.y - gSceneViewportCur.y) / max(gSceneViewportCur.w, 1.0) * 2.0;

    // Same NDC-to-world-ray reconstruction as PSRayDriven's primary ray (and PSVoxelDebug's).
    return averViewRayDir(ndc);
}

// THE CUTOUT POLICY PER STAGED RAY TYPE (averRtCutoutPolicy, voxi_rt.hlsli): an alpha-test budget, and
// whether cutouts are solid. Primary visibility and the sun's shadow draw the leaf, so they test every
// cutout (within a budget); diffuse GI and sky occlusion only average what they hit, so cutouts are
// solid for them; a reflection is solid-cutout only once it is rough enough to blur a leaf's outline.
#define AVER_RD_CUTOUTS_PRIMARY   24u
#define AVER_RD_CUTOUTS_SHADOW     8u
#define AVER_RD_CUTOUTS_DIFFUSE    4u
#define AVER_RD_CUTOUTS_REFL       8u
#define AVER_RD_REFL_SOLID_CUTOUT_ROUGH 0.3

// ---- STAGE A: CSRdVisibility -- trace the primary ray, write the visibility record -----------------
//
// The visibility-only half of PSRayDriven's trace block above (AVER_RD_SPLIT==0 branch): same mask,
// same cutout handling, same ray. Nothing past a hit/miss is computed here -- Stage S (CSRdShadow)/
// Stage B (PSRayDriven's AVER_RD_SPLIT branch) reconstruct from this dispatch's gRdVisBuf write,
// through rdSurfaceFromRecord.
//
// D3D12 ONLY FOR NOW -- VoxiRenderer decides whether to dispatch this at all (falls back to the
// single pass otherwise), not this file.
[numthreads(8, 8, 1)]
void CSRdVisibility(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    // pitch 0 means the record buffer has nowhere well-defined to put this pixel (VoxiRenderer writes
    // a nonzero pitch only while it means to run the staged path this frame) -- bail rather than
    // guess an index.
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

    // The primary ray draws the leaf: every cutout it crosses is tested, up to AVER_RD_CUTOUTS_PRIMARY.
    averRtCutoutPolicy(AVER_RD_CUTOUTS_PRIMARY, false);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Same lane as PSRayDriven's primary ray (AVER_RT_MASK_OPAQUE, not _OPAQUE_ALL: leaves the
    // viewer's own head).
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

// THE SHADOW-RAY FOOTPRINT, factored out of CSRdShadow so CSRdShadowProbe (below) can build the exact
// same dpx/dpy a fresh probe ray needs, rather than a third copy (PSRayDriven's copy, and CSRdShadow's
// own copy before this factoring, are the other two). gSceneViewport, not Cur -- matches PSRayDriven's
// copy of this step exactly.
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

// A: SUN SHADOW SPLIT'S OWN GROUP REDUCTION -- one mask per 8x8 thread group (== one gRdShadowTiles
// tile), zeroed and OR'd by CSRdShadowProbe below. groupshared storage has to sit at file scope in
// HLSL, not inside the function that uses it.
groupshared uint gRdShadowProbeMask;

// ---- STAGE S0: CSRdShadowProbe -- one ray per pixel, reduced to one lit/blocked/mixed mask per tile ---
//
// A (Settings::rayDrivenShadowTiles). Dispatched over the SAME grid as CSRdShadow, immediately before
// it: one thread GROUP is one 8x8 tile (tile = SV_GroupID.xy, matching gRdShadowTiles' own tileIdx
// layout), every thread traces at most ONE probe ray (never the Epic-tier disc CSRdShadow's
// AVER_RD_SHADOW_TILES compile traces), then the group reduces 64 answers to one mask. CSRdShadow's
// tiled compile ORs this tile's mask with its 3x3 neighbourhood and skips its ray loop wherever every
// probe in that neighbourhood agrees (see that compile's header for why 3x3).
//
// NO EARLY RETURN ANYWHERE IN THIS FUNCTION, before or between the two barrier calls below: an
// out-of-viewport thread, a pitch-0 frame, and a sky pixel are all real (the viewport is rarely an
// exact multiple of 8), and a `return` before a barrier every OTHER thread still executes is
// undefined behaviour, not merely "this thread's own contribution is skipped". Out-of-viewport/
// pitch-0/sky cases instead leave `bit` at its 0 default and fall through to the same reduction (0
// ORs in as a no-op, so it costs nothing but a branch).
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
            bit = 2u;   // ablated: fully lit, no ray -- matches CSRdShadow's own ablated branch
#else
            float2 ndc;
            const float3 dir = rdPrimaryRayDir(pixel, ndc);
            const RdSurface s = rdSurfaceFromRecord(rec, dir);
            averRtCutoutPolicy(AVER_RD_CUTOUTS_SHADOW, false);
            float3 dpx, dpy;
            rdShadowFootprint(ndc, dir, s, dpx, dpy);

            const float3 L = normalize(gLightDir.xyz);
            // THE SAME JITTER rtShadowTemporal's non-tiled branch passes (voxi_rt.hlsli) for this
            // (gRtHistParams.x, frame index) pair -- this probe has to agree with what CSRdShadow's
            // fresh trace would have drawn, or a tile's "every probe agrees" verdict classifies the
            // wrong sample.
            const float jitter = (gRtHistParams.x < 0.5) ? 0.0
                                : (float)((uint)gRtHistParams.z) * 2.39996323;
            // WHICH OF THE PIXEL'S OWN SAMPLES THE PROBE TRACES -- ROTATED, NOT ALWAYS THE FIRST.
            // Sample 0 sits at a FIXED radius (sqrt(0.5) of the sun disc, rtDiscSample; the same trap
            // the F1 comment in voxi_rt.hlsli records for the GI and sky rays); always tracing it would
            // miss an occluder covering < ~9% of the disc at a soft penumbra's edge, and every probe in
            // a neighbourhood would then agree while High/Epic's real rays DO see it -- hardening the
            // edge tile-by-tile. Rotating through samples 0..rays-1 by pixel and frame puts every
            // radius the real trace uses (0.25 to 0.94 of the disc at 8 rays) into every tile, and the
            // probe is always one of the real trace's own rays: identical to it at 1 ray, and covering
            // its radii at 4 and 8. The (x + 3y) step keeps row/column neighbours on different samples.
            const uint rays   = (uint)max(gRtParams.y, 1.0);
            const uint kProbe = (pixel.x + 3u * pixel.y + (uint)gRtHistParams.z) % rays;
            // From the same side CSRdShadow will trace from, or a back-lit subsurface tile's probes would
            // all read "blocked" and let CSRdShadow skip the rays that make it glow.
            gAverShadowOriginPush = averSubsurfaceShadowPush(s.mat.flags, s.mat.subsurfaceRadius, s.N, L);
            const float3 fresh = rtShadowEx(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy, 1u, jitter,
                                            kProbe);
            gAverShadowOriginPush = float3(0.0, 0.0, 0.0);

            if (all(fresh == 0.0))      bit = 1u;   // fully blocked
            else if (all(fresh == 1.0)) bit = 2u;   // fully lit
            else                        bit = 4u;   // a real penumbra, or a tinted (glass/water) hit
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

// ---- STAGE S: CSRdShadow -- reconstruct the surface, resolve the sun shadow -------------------------
//
// Reads CSRdVisibility's record, rebuilds the surface through rdSurfaceFromRecord (same
// reconstruction Stage B uses), and runs the SAME rtShadowTemporal call PSRayDriven's single pass
// makes -- same pixel-centre, footprint, ray count -- so the history buffer means the same thing
// either way.
//
// COMPILED AT SM 6.6: rtShadowTemporal's reprojection/spatial filter take ddx/ddy of depth, and
// compute shaders only get derivatives from 6.6 on (8x8 threads form 2x2 quads, the pixel shader's
// own neighbourhood). As in the single pass, a quad with an early-returned lane (a sky pixel, the
// viewport's last odd row) has undefined derivatives.
[numthreads(8, 8, 1)]
void CSRdShadow(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // A sky pixel has no surface to shadow-test; Stage B never reads this texel for one either
        // (its own miss check returns before reaching the sunVis read), but a defined, fully-lit value
        // costs nothing and leaves no uninitialised texel behind. Alpha 0.0, NOT a depth -- M6:
        // PSMainVoxi's blended-replay reuse test (gGiShadowParams.w bit 16) treats <= 0 as "no surface
        // to reuse", a sentinel a real hit's positive view-space w can never produce.
        gRdSunVisTex[pixel] = float4(1.0, 1.0, 1.0, 0.0);
        return;
    }

    // Same pixel-centre NDC/primary-ray reconstruction as CSRdVisibility. Needed again here (not
    // carried in the record) for rdSurfaceFromRecord's face-the-ray flip and this shadow ray's
    // footprint.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_SHADOW, false);

    // THE SAME SHADOW-RAY FOOTPRINT PSRayDriven builds for its own shadow call (factored out, above,
    // so CSRdShadowProbe can build the identical footprint without a third copy).
    float3 dpx, dpy;
    rdShadowFootprint(ndc, dir, s, dpx, dpy);

    const float3 L = normalize(gLightDir.xyz);

    // W6/M5: EXPLICITLY TRUE -- same reason as PSRayDriven's copy: a blended (glass/water) draw never
    // reaches the ray-driven primary, so every history write this call makes is always live for this
    // pass.
    gAverHistoryWrite = true;

#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    const float3 sunVis = float3(1.0, 1.0, 1.0);   // ablated: fully lit, no ray -- matches PSRayDriven's own ablated branch
#else
    // A subsurface hit lit from behind asks from its light-facing side (PSMainVoxi's twin says why);
    // cleared after the call below, whichever compile it is.
    gAverShadowOriginPush = averSubsurfaceShadowPush(s.mat.flags, s.mat.subsurfaceRadius, s.N, L);
#if AVER_RD_SHADOW_TILES
    // A3: PROBE-GUIDED SKIP -- OR the 3x3 tile neighbourhood CSRdShadowProbe already classified around
    // this pixel's tile (tid.xy/8 is the same tile grid CSRdShadowProbe dispatches over; wave-uniform:
    // depends only on SV_GroupID). Exactly 2 (every probe lit) or 1 (every probe blocked) skips the
    // ray loop and hands the probes' verdict straight to the same temporal accumulation/history/filter
    // every other pixel runs (see rtShadowTemporalEx's header for why that is safe). Anything else
    // (disagreement, or a mixed 3x3 OR) falls through to the unabridged path.
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
    // ONE call with the verdict as a runtime flag, not a ?: between two calls: two call sites would
    // inline the whole temporal/spatial/trace body twice into an already register-bound pass.
    const bool   probeAgrees = (m == 2u || m == 1u);
    const float3 sunVis = rtShadowTemporalEx(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy,
                                             (uint)max(gRtParams.y, 1.0), probeAgrees,
                                             float3(1.0, 1.0, 1.0) * (m == 2u ? 1.0 : 0.0));
#else
    const float3 sunVis = rtShadowTemporal(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy,
                                           (uint)max(gRtParams.y, 1.0));
#endif
    gAverShadowOriginPush = float3(0.0, 0.0, 0.0);
#endif
    // M6: THE PRIMARY SURFACE'S OWN LINEAR VIEW DEPTH, same formula as PSMainVoxi's rtViewZ
    // (mul(wpos, 1, gViewProj).w) -- carried in alpha so a later blended-replay fragment can prove it
    // sits on THIS surface before reusing `sunVis` instead of tracing (see gGiShadowParams.w bit 16
    // and gRdSunVisTex's own comments).
    const float rdSunVisViewZ = mul(float4(s.wpos, 1.0), gViewProj).w;
    gRdSunVisTex[pixel] = float4(sunVis, rdSunVisViewZ);
}

#if !AVER_RD_SINGLE_PASS
// ---- STAGE L: CSRdLocalLights -- lamps lit the way the sun is -----------------------------------------
//
// Dispatched right after CSRdShadow, only on a frame with lights (zero count skips the dispatch, so a
// scene with no lamps pays nothing). Per pixel: rdLocalLightsVisibility for the surface
// CSRdVisibility found -- one shadow ray toward one lamp, accumulated into gRdLocalOut; Stage B
// (PSRayDriven's AVER_RD_SPLIT branch) reads it back via rdLocalVisFiltered and shades every lamp
// with rdLocalLightsShade.
//
// DERIVATIVES AS IN CSRdShadow (SM 6.6, 8x8 threads = 2x2 quads): rdLocalLightsVisibility's
// rtReprojectTexel takes ddx/ddy of depth for every non-sky pixel before any data-dependent branch.
//
// NOT IN THE SINGLE-PASS COMPILE, which has no staged surface record to light: the single-pass
// PSRayDriven calls rdLocalLightsVisibility on its own hit instead.
[numthreads(8, 8, 1)]
void CSRdLocalLights(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    // W6/M5: EXPLICITLY TRUE -- no blended draw reaches a staged pass, so this history write is
    // always live.
    gAverHistoryWrite = true;

    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // Sky: nothing to light, but every HISTORY texel must be written each frame or the ping-pong
        // hands a two-frames-old value back later. Visibility 1, so a near-miss reprojection starts lit.
        if (gAverHistoryWrite) gRdLocalOut[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float2 ndc;
    const float3 dir = rdPrimaryRayDir(pixel, ndc);
    const RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_SHADOW, false);
    // The pixel centre, as CSRdShadow hands rtShadowTemporal. Stage B reads the visibility back from
    // gRdLocalOut, so the return value isn't needed here.
    rdLocalLightsVisibility(s.wpos, s.N, float2(pixel) + 0.5, pixel, true);
}
#endif

// ---- STAGE G0: CSRdGiTrace -- trace ReSTIR GI's fresh candidate for CSRdGi's own resample to read back
//
// B: GI CANDIDATE TRACE/RESAMPLE SPLIT (Settings::rayDrivenGiSplit). Runs giTraceInitialCandidate for
// every pixel CSRdGi's non-split copy would have traced -- same surface reconstruction, frameJitter,
// and f2Path/rho2 (giDecodePaths, voxi_restir.hlsli B2) -- storing every out param in gRdGiCand
// (voxi_restir.hlsli B1). CSRdGi's AVER_GI_SPLIT=1 compile then reads that record back inside
// giRestirIndirect instead of retracing, agreeing bit-for-bit (full float precision, no
// quantisation).
//
// NON-CHECKERBOARD COMPILE: one thread per pixel, same grid/mapping CSRdGi dispatches over.
//
// AVER_GI_CHECKERBOARD COMPILE (milestone 4): COMPACTED, not the full grid with half idle --
// CSRdGi's own checkerboard branch only traces pixels satisfying (x ^ y ^ parity) & 1 == 0, so this
// dispatch is issued over ceil(w/2) x h threads, reconstructing the traced half's coordinates from tid
// using the same `parity` bit CSRdGi reads from gViewParams.w. gGiCbSkip is forced false here since
// this dispatch by construction only covers the half that traces.
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
    return;   // ablated: no ReSTIR GI candidate -- matches CSRdGi's own ablated branch
#else
    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) return;   // sky pixel: no surface, nothing for CSRdGi to read back

    float2 ndc;
    const float3 dir = rdPrimaryRayDir(pixel, ndc);
    const RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_DIFFUSE, true);

    // NO gAverHistoryWrite HERE, unlike CSRdShadow/CSRdGi: this dispatch never calls giRestirIndirect
    // (only giTraceInitialCandidate, whose shadow ray is a plain rtShadow call reading no history), so
    // the flag has nothing to gate.
    const uint frameIdx = (uint)gRtHistParams.z;
    const GiPathDecode gd = giDecodePaths(s.wpos, s.N, float2(pixel) + 0.5, frameIdx);

    float3 pos, nrm, rad;
    bool   nonFinite   = false;
    float  f2LumTraced = 0.0, f2LumSky = 0.0;
    bool   f2Observed  = false;
    const bool ok = giTraceInitialCandidate(s.wpos, s.N, float2(pixel) + 0.5, frameIdx * 2.39996323,
                                            pos, nrm, rad, nonFinite, gd.f2Path, gd.rho2,
                                            f2LumTraced, f2LumSky, f2Observed);

    // EVERY OUT PARAM PLUS THE BOOL, ALWAYS -- so CSRdGi's AVER_GI_SPLIT read has a defined record for
    // every pixel it might read, not only the ones that produced a usable candidate.
    RdGiCand cand;
    cand.pos         = pos;
    cand.flags       = (ok ? 1u : 0u) | (nonFinite ? 2u : 0u) | (f2Observed ? 4u : 0u);
    cand.nrm         = nrm;
    cand.f2LumTraced = f2LumTraced;
    cand.rad         = rad;
    cand.f2LumSky    = f2LumSky;
    gRdGiCand[idx] = cand;
#endif
}

// ---- STAGE G: CSRdGi -- reconstruct the surface, resolve ReSTIR GI's diffuse estimate ---------------
//
// MILESTONE 2. Reads CSRdVisibility's record, rebuilds the surface through rdSurfaceFromRecord (the
// same reconstruction Stage S and Stage B use), and runs the SAME giRestirIndirect call PSRayDriven's
// single pass makes when ReSTIR GI is active -- same pixel-centre, so the reservoir/surface-history
// buffers (gGiReservoirs/gGiSurfPosHist/gGiSurfNrmHist, u6/u7/u8, denoiser GI pair u9/t15, half-res
// visibility u10/t16) mean the same thing either way.
//
// ONLY MEANINGFULLY DISPATCHED WHEN ReSTIR GI IS THE CHOSEN ESTIMATOR (VoxiRenderer::
// recordStagedRayDriven mirrors the same `gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5` test on the
// CPU before dispatch). The cone-traced branch is UNTOUCHED and still runs inside Stage B:
// coneTracedIndirect has no per-pixel history to split out.
//
// COMPILED AT SM 6.6, same as CSRdShadow, though giRestirIndirect uses no derivative intrinsic itself
// (every ray-hit texture fetch goes through averRtSampleSlot with an explicit SampleLevel/gradient,
// never implicit ddx/ddy; that also covers giTraceInitialCandidate and the reuse pass,
// giSpatioTemporalReuse) -- shares the staged pipeline's shader model rather than
// inventing a fourth.
//
// MILESTONE 4 (voxi.rayDrivenStages == 2): this stage alone is ALSO compiled with
// AVER_GI_CHECKERBOARD=1 -- half-rate ReSTIR GI, tracing a fresh candidate only for one checkerboard
// half this frame, leaving the denoiser to reconstruct the other half. The
// skip is decided here (gGiCbSkip); everything it changes lives inside giRestirIndirect
// (voxi_restir.hlsli).
[numthreads(8, 8, 1)]
void CSRdGi(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

#if AVER_GI_CHECKERBOARD
    // HALF-RATE GI'S OWN PARITY, NOT THE ROW PITCH -- read bit 16 of the raw cbuffer field directly
    // (rdRowPitch() already masked it away). CONTRACT: the denoiser treats a pixel as traced where
    // (x ^ y ^ parity) & 1 == 0 (aver_denoise.hlsl's dnsrLoadInput) and reconstructs the rest; `pixel`
    // here is the same render-target pixel it reads. `giCbParity` is the parity VoxiRenderer::
    // recordStagedRayDriven latched for THIS frame's write (giCbParityWritten_) and hands to NEXT
    // frame's denoiser dispatch with it, packed into bit 16 for this one upload.
    const uint giCbParity = ((uint)gViewParams.w >> 16) & 1u;
    gGiCbSkip = ((pixel.x ^ pixel.y ^ giCbParity) & 1u) != 0u;
#endif

    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // A sky pixel has no surface for ReSTIR to bounce a candidate off. Stage B's miss branch
        // already writes the GI surface-history sentinel (gGiSurfNrmHistOut) -- this stage only
        // leaves its own texel defined, not whatever the previous frame's HIT left there.
        gRdGiTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Same pixel-centre NDC/primary-ray reconstruction as CSRdVisibility.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_DIFFUSE, true);

    // W6/M5: EXPLICITLY TRUE -- same reason as CSRdShadow's copy: every history write
    // giRestirIndirect makes below is always live for this pass.
    gAverHistoryWrite = true;

#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no ReSTIR GI candidate (matches PSRayDriven's ablated GI block -- a comment only, no
    // assignment). gRdGiTex is never read back under this ablation either: PSRayDriven's own
    // AVER_RD_SPLIT read of it lives inside that same outer ablation guard, so leaving this texel
    // untouched costs nothing.
#else
    // EXACTLY THE ARGUMENTS PSRayDriven'S non-split COPY PASSES. `ao` is discarded here the same way
    // PSRayDriven discards `rdAo`: giRestirIndirect sets it to 1.0 on its first line and never touches
    // it again, so Stage B's rdAo stays at its initial 1.0 either way.
    if (gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5) {
        float ao;
#if AVER_GI_SPLIT
        // B4: THE SAME ROW-PITCH INDEX CSRdGiTrace WROTE gRdGiCand UNDER -- a `static`, not a
        // parameter, so giRestirIndirect's signature stays shared with PSMainVoxi/PSRayDriven's
        // non-split call sites.
        gGiCandIdx = idx;
#endif
        const float3 d = giRestirIndirect(s.wpos, s.N, mul(float4(s.wpos, 1.0), gViewProj).w,
                                          float2(pixel) + 0.5, (uint)gRtHistParams.z, ao);
        gRdGiTex[pixel] = float4(d, 1.0);
    }
#endif
}

// ---- STAGE O: CSRdSkyOcc -- reconstruct the surface, resolve the traced sky-occlusion answer --------
//
// MILESTONE 2. Reads CSRdVisibility's record, rebuilds the surface through rdSurfaceFromRecord, and
// runs the SAME rtSkyOcclusionTemporal call PSRayDriven's single pass makes in the two cases where its
// own cone gather did not already measure occlusion -- same pixel-centre argument, so the AO history
// pair (gAoHist/gAoHistOut, t11/u4) and the denoiser's AO hand-off (gAoHitDistOut, u5) mean the same thing
// whichever path is running.
//
// COMPILED AT SM 6.6 (VoxiRenderer), same defines as CSRdShadow: rtSkyOcclusionTemporal's own spatial
// filter (rtAoSpatial) takes ddx/ddy of depth exactly as rtShadowTemporal's does, so this stage needs
// the same derivative-capable compute shader model, 8x8 threads forming 2x2 quads.
[numthreads(8, 8, 1)]
void CSRdSkyOcc(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;

    // MIRRORS PSRayDriven's OWN GATE FOR READING THIS TEXTURE (its sky-occlusion ternary, further up
    // this file): gGiRestirParams.x > 0.5 (ReSTIR supplies diffuse, so rdAo is still its un-gathered
    // initial 1.0) or gVoxelParams.w <= 0.5 (no voxel GI at all, same reason). In CONE-GI mode
    // (gGiRestirParams.x <= 0.5 && gVoxelParams.w > 0.5) the cone gather already measured occlusion and
    // PSRayDriven never reads this texture for that pixel, so returning without writing here is safe,
    // not merely cheap -- that texel is never read back either. VoxiRenderer::recordStagedRayDriven
    // mirrors this same test on the CPU before issuing the dispatch at all; this check is the shader's
    // own defence, not a duplicate of work the CPU has already decided.
    if (!(gAmbientParams.x > 0.5 && (gGiRestirParams.x > 0.5 || gVoxelParams.w <= 0.5))) return;

    const uint idx = pixel.y * pitch + pixel.x;
    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // A sky pixel is unoccluded by definition -- the same sentinel meaning as rdAo's own initial
        // 1.0, and Stage B's own miss branch returns before ever reaching the occlusion read.
        gRdAoTex[pixel] = float4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    // Same pixel-centre NDC/primary-ray reconstruction as CSRdVisibility.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_DIFFUSE, true);

    // W6/M5: EXPLICITLY TRUE -- same reason as CSRdShadow's and CSRdGi's copies.
    gAverHistoryWrite = true;

    // HONOURS THE SKY-OCCLUSION ABLATION EXACTLY AS PSRayDriven's OWN #elif AVER_RT && AVER_RD_ABLATE
    // != AVER_RD_ABL_SKYOCC BRANCH DOES (further up this file, same condition, deliberately not also
    // excluding AVER_RD_ABL_ALL -- that asymmetry is PSRayDriven's existing behaviour, not introduced
    // here): this stage skips tracing in precisely the build where that branch's AVER_RD_SPLIT read of
    // gRdAoTex is itself compiled out and falls back to `ind.occlusion = rdAo`.
#if AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    // EXACTLY THE ARGUMENTS PSRayDriven'S OWN (non-split) COPY PASSES for this case: coneAo=1.0,
    // coneAoIsGather=false (rdAo was never gathered on this branch), denoisedAoUsable=false (same
    // reason that call gives -- this pass runs with the G-buffer off, so gDenoisedAo was reprojected against
    // motion vectors and depth this pass never wrote).
    const float occ = rtSkyOcclusionTemporal(s.wpos, s.N, float2(pixel) + 0.5, (uint)gAmbientParams.x,
                                             1.0, false, false);
    gRdAoTex[pixel] = float4(occ, 0.0, 0.0, 1.0);
#else
    // ablated: matches PSRayDriven's own fallback for this same condition (`ind.occlusion = rdAo`,
    // rdAo's un-gathered initial 1.0) -- never read back under this ablation, same reasoning as the
    // miss case above.
    gRdAoTex[pixel] = float4(1.0, 0.0, 0.0, 1.0);
#endif
}

// ---- STAGE R: CSRdRefl -- reconstruct the surface, resolve the ray-traced reflection --------------
//
// MILESTONE 3. Reads CSRdVisibility's record, rebuilds the surface through rdSurfaceFromRecord (the
// same reconstruction every other stage uses), and runs the SAME rtReflectionTemporal call PSRayDriven's
// single pass makes for a qualifying surface -- same pixel-centre argument, same shadow-ray-shaped
// footprint, so the reflection history pair (gRtReflHist/gRtReflHistOut, t7/u3, this file's own
// rtReflectionTemporal) means the same thing whichever path is running.
//
// UNLIKE Stage S/G/O, this stage's OWN gate includes roughness -- CSRdShadow/CSRdGi/CSRdSkyOcc all
// answer a question every surface has (is it lit? what bounces off it? how open is its sky?), but a
// reflection ray is only ever traced for `rough <= 0.75` surfaces in the first place, so this stage has
// to reconstruct that same roughness before it can even decide whether to trace -- see
// rdSurfaceRoughness's own header for why that is a dedicated helper rather than reading `s.rough` off
// a full AverSurface this stage never builds.
//
// COMPILED AT SM 6.6 (VoxiRenderer), same defines as CSRdShadow/CSRdGi/CSRdSkyOcc: rtReflectionTemporal
// calls rtReflectionSpatial, which takes ddx/ddy of depth exactly as rtShadowTemporal's own spatial
// filter does, so this stage needs the same derivative-capable compute shader model, 8x8 threads
// forming 2x2 quads.
//
// C: REFLECTION TRACE/FILTER SPLIT (Settings::rayDrivenReflSplit), the same shape as A/B above. This
// entry point compiled a second time with AVER_RD_REFL_SPLIT=1 becomes R1: when a reflection history is
// actually bound (gRtHistParams.x >= 0.5, rtReflectionTemporalEx's own no-history early-out otherwise
// has nothing for a filter pass to defer), it calls rtReflectionTemporalEx with doSpatial=false --
// tracing the ray, shading the hit and writing this frame's gRtReflHistOut exactly as the non-split
// compile does, but skipping rtReflectionSpatial's dense 7x7 history gather -- and writes gRdReflTex a
// PENDING marker (alpha < -0.5, unreachable by any real output; see gRdReflTex's own header comment for
// the 0/1/2 alphas this leaves unambiguous) instead of composing. CSRdReflFilter, immediately below, is
// R2: it reruns rtReflectionSpatial against gRtReflHistOut's write from R1 -- the two passes round-trip
// through the SAME RWTexture2D a C++ UAV barrier separates -- then finishes the exact same compose R1
// would have. Splitting the register-heavy trace from the bandwidth-heavy gather is the point (see
// this file's own rtReflectionTemporalEx comment); the default (AVER_RD_REFL_SPLIT undefined) compile
// takes none of this and stays byte-for-byte today's CSRdRefl.
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
    if (rec.x == 0xFFFFFFFFu) {
        // A sky pixel has no surface to reflect off. Stage B's own miss branch (PSRayDriven's
        // #if AVER_RD_SPLIT trace block, above) already returns before ever reaching the reflection
        // read, so this texel is never read back for this pixel either -- written anyway, the same
        // "leave no uninitialised texel behind" reasoning CSRdShadow's own miss branch gives, and 0 in
        // alpha reads as "not traced" if anything ever does read it.
        gRdReflTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Same pixel-centre NDC/primary-ray reconstruction as CSRdVisibility.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // THE SAME FOOTPRINT RECONSTRUCTION CSRdShadow builds, byte-for-byte. gSceneViewport, not Cur:
    // this is a ray DIFFERENTIAL (the neighbour pixel's own primary ray), reconstructed against LAST
    // frame's grid on purpose (as PSRayDriven and CSRdShadow do), not this stage's own dispatch rect.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float3 dirDx = averViewRayDir(ndc + float2(ndcPixelStep.x, 0.0));
    float3 dirDy = averViewRayDir(ndc + float2(0.0, ndcPixelStep.y));
    const float3 rdRayDx = (dirDx - dir) * s.hitT;
    const float3 rdRayDy = (dirDy - dir) * s.hitT;

    const float3 L = normalize(gLightDir.xyz);
    // PSRayDriven's OWN R -- reflect the primary ray about the (already face-the-ray-flipped) surface
    // normal rdSurfaceFromRecord produced, same as that function's own `float3 R = reflect(dir, N);`.
    const float3 R = reflect(dir, s.N);
    // THE ONE VALUE THIS STAGE NEEDS BEFORE IT CAN EVEN GATE -- see rdSurfaceRoughness's own header.
    const float rough = rdSurfaceRoughness(s, rdRayDx, rdRayDy);
    averRtCutoutPolicy(AVER_RD_CUTOUTS_REFL, rough > AVER_RD_REFL_SOLID_CUTOUT_ROUGH);

    // W6/M5: EXPLICITLY TRUE, same reason CSRdShadow/CSRdGi/CSRdSkyOcc's own copies -- a blended
    // (glass/water) draw never reaches the ray-driven primary, so every history write below is live.
    gAverHistoryWrite = true;

    // THE GATE IS PSRayDriven's OWN PREDICATE (see its "ENVIRONMENT SPECULAR" comment for why these
    // three terms). Unlike Stage S/G/O this can't be left for Stage B to re-apply: Stage B reads the
    // OUTCOME off gRdReflTex's alpha instead (PSRayDriven's AVER_RD_SPLIT branch; gRdReflTex's header).
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(float2(pixel) + 0.5);   // see that function
    if (rtReflTraced) {
        const float rdReflDzdx = rdPlaneDepthStep(s.wpos, s.Ng, dirDx);
        const float rdReflDzdy = rdPlaneDepthStep(s.wpos, s.Ng, dirDy);
        bool specHit = false;
        // PENDING iff R1 (AVER_RD_REFL_SPLIT) AND a history is bound to gather against; with none
        // bound, rtReflectionTemporalEx's early-out already skips rtReflectionSpatial, so R2 has
        // nothing to add and R1 composes exactly as the non-split compile does. FALSE under the refl
        // ablation too -- no ray to defer either way; compose in R1, no pending marker.
#if AVER_RD_REFL_SPLIT && AVER_RD_ABLATE != AVER_RD_ABL_REFL && AVER_RD_ABLATE != AVER_RD_ABL_ALL
        const bool pending = gRtHistParams.x >= 0.5;
#else
        const bool pending = false;
#endif
#if AVER_RD_ABLATE == AVER_RD_ABL_REFL || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        float3 refl = float3(0.0, 0.0, 0.0);   // ablated: no mirror ray -- matches PSRayDriven's own copy
#elif AVER_RD_REFL_SPLIT
        // R1's half of rtReflectionTemporal: doSpatial=false still traces, blends history and writes
        // gRtReflHistOut; only the dense spatial gather is skipped, left for CSRdReflFilter. if/else,
        // not `?:`, for the same reason rtReflectionTemporalEx's own two returns spell it that way.
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
        // No fade toward unoccluded mirror sky between roughness 0.5 and the gate: the traced estimate
        // already carries the sky its rays reached (rtReflection), and the fade put sharp, unshadowed
        // sky under the pyramid on every semi-rough surface. skyR is only the ablation/no-ray fallback.
        const float skyW = 0.0;
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march -- matches PSRayDriven's own copy
#else
        if (!specHit) skyR = skyColor(R);
#endif
        if (pending) {
            // PENDING MARKER, NOT A COMPOSE: CSRdReflFilter finishes this pixel after gathering
            // rtReflectionSpatial against gRtReflHistOut's write above -- alpha < -0.5 is unambiguous
            // vs the three composed outcomes (gRdReflTex's header). R2 rebuilds roughness itself
            // (rdSurfaceRoughness), gets refl off gRtReflHistOut not this texel, and specHit off that
            // texel's own alpha.
            gRdReflTex[pixel] = float4(skyR, -1.0);
        } else {
            // Same ceiling as PSRayDriven's own copy (see its comment above the identical line for the
            // unbounded-term incident this guards against). clamp(), not min(): negative radiance floors to 0.
            const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
            // .a is the stage's decision (gRdReflTex's header): nonzero, ONLY here, marks a pixel
            // CSRdRefl traced. 2.0 vs 1.0 carries B1(F5)'s PRE-clamp ceiling test (`>=` for
            // PSMainVoxi's NaN-safety) -- Stage B can't recompute it post-clamp from a half-float rgb.
            gRdReflTex[pixel] = float4(clamp(specRaw, 0.0, AVER_VOX_MAXRAD),
                                       any(specRaw >= AVER_VOX_MAXRAD) ? 2.0 : 1.0);
        }
    } else {
        // Roughness (or the outer gate) routed this pixel to Stage B's cone/sky fallback instead -- all
        // zero, alpha included, so PSRayDriven's AVER_RD_SPLIT branch takes the fallback, not a stale miss.
        gRdReflTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    }
}

// ---- STAGE R2: CSRdReflFilter -- finish a PENDING reflection with the spatial history gather --------
//
// R1's other half (Settings::rayDrivenReflSplit / AVER_RD_REFL_SPLIT; see CSRdRefl's header for the
// split contract), run over the same grid as CSRdRefl after a C++ UAV barrier on gRtReflHistOut and
// before Stage B reads gRdReflTex -- every pixel here was marked PENDING by R1 this same frame; no
// cross-frame reasoning, just a same-frame round trip through gRtReflHistOut.
//
// SM 6.6 (VoxiRenderer), same layout/defines as CSRdRefl; gRtReflHist/gRtReflHistOut (voxi_rt.hlsli)
// are unconditional here so no compile guard is needed. Takes dzdx/dzdy as params instead of
// ddx/ddy(), and never touches gAverHistoryWrite (already spent by R1's write) -- no static setup here.
[numthreads(8, 8, 1)]
void CSRdReflFilter(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    // NOTHING TO DO except for a PENDING pixel: a sky pixel, a roughness-gated-out one, or one R1
    // already composed in full (no history bound, or the refl ablation) all left alpha 0/1/2 here
    // (gRdReflTex's header) and are left untouched. Only alpha < -0.5, R1's PENDING marker, means a
    // gather is still owed.
    const float4 t = gRdReflTex[pixel];
    if (t.a > -0.5) return;
    const float3 skyR  = t.rgb;

    // Same pixel-centre NDC/primary-ray/record reconstruction as CSRdRefl (rdPrimaryRayDir's header).
    // gRdVisBuf is re-read rather than carried through gRdReflTex: a PENDING pixel, by construction, is
    // one CSRdRefl already found a surface for, so this can't itself turn up a miss.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    const uint4 rec = gRdVisBuf[idx];
    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // Same footprint reconstruction as CSRdRefl's own reflection call (see that stage's comment). `R`
    // is not rebuilt: only rtReflection/rtReflectionTemporal(Ex)'s ray-tracing half reads it, and that
    // half already ran, in R1.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float3 dirDx = averViewRayDir(ndc + float2(ndcPixelStep.x, 0.0));
    float3 dirDy = averViewRayDir(ndc + float2(0.0, ndcPixelStep.y));
    const float3 rdRayDx = (dirDx - dir) * s.hitT;
    const float3 rdRayDy = (dirDy - dir) * s.hitT;
    const float rdReflDzdx = rdPlaneDepthStep(s.wpos, s.Ng, dirDx);
    const float rdReflDzdy = rdPlaneDepthStep(s.wpos, s.Ng, dirDy);

    // ROUGHNESS AND DEPTH ARE RECOMPUTED, NOT READ BACK. The PENDING marker (gRdReflTex's header) is
    // a plain -1.0 flag with nothing else encoded, so roughness must be rebuilt. Depth reached this
    // stage only through the history alpha (curClip.w in half precision, overflowing to inf past
    // 65504 cm and then disabling the filter's depth test outright), so it's rebuilt too, from R1's
    // same roughness sample and the same mul(wpos,gViewProj).w.
    const float rough    = rdSurfaceRoughness(s, rdRayDx, rdRayDy);
    const float curDepth = mul(float4(s.wpos, 1.0), gViewProj).w;

    // Same mirror cutoff rtReflectionTemporal(Ex) applies before calling rtReflectionSpatial (see its
    // "THE MIRROR CUTOFF" comment), from the same `rough` R1 traced with, so the filter radius agrees.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // R1's OWN WRITE TO THIS UAV, THIS SAME FRAME: gRtReflHistOut (u3) is normally last frame's
    // history via gRtReflHist SRV (t7) -- rtReprojectReflection/rtReflectionSpatial's own gather read
    // that copy -- but R1 (AVER_RD_REFL_SPLIT) just wrote THIS pixel's fresh answer straight into the
    // RWTexture2D, barriered against this read (recordStagedRayDriven). Reading it back stands in for
    // the `col`/`curHit` rtReflectionTemporalEx would otherwise still hold in registers, had R1 not
    // already returned.
    //
    // INVARIANT: rtReflectionTemporalEx writes gRtReflHistOut[pixel] as float4(col, depth in metres) for
    // a traced pixel -- surface hit or sky alike -- identically in its untiled and tiled branches, and
    // float4(0,0,0,-1) only when nothing was traced. The depth is > 0 in front of the camera, so alpha > 0
    // means an estimate is there.
    const float4 h = gRtReflHistOut[pixel];
    const bool specHit = h.a > 0.0;

    float3 refl;
    if (specHit) {
        refl = rtReflectionSpatial(h.rgb, s.wpos, s.N, float2(pixel) + 0.5, curDepth, lobeRough,
                                   rdReflDzdx, rdReflDzdy);
    } else {
        refl = float3(0.0, 0.0, 0.0);
    }

    // Exact same compose as CSRdRefl's non-split tail (see its comments for the skyW/clamp reasoning)
    // -- repeated verbatim, not factored out, so this stage's control flow mirrors R1's.
    const float skyW = 0.0;
    const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
    gRdReflTex[pixel] = float4(clamp(specRaw, 0.0, AVER_VOX_MAXRAD),
                               any(specRaw >= AVER_VOX_MAXRAD) ? 2.0 : 1.0);
}
#endif  // AVER_RT

// ================= depth prepass =================
// Same-frame depth-only pass paired with VSMain (VoxiRenderer.hpp's depthPrepassPipeline(),
// D3D12Device::drawMesh) -- same compiled vertex shader as PSMainVoxi's, so depth matches exactly.
//
// Writes no colour (renderTargetCount=0); reads only enough for "does this survive alpha test", short
// of averEvalMaterial() (unused metal-rough/normal/occlusion/emissive, 4 fetches instead of 1 just for
// s.alpha). NOT FREE: pays a gMaterialFlags branch plus Sample()+compare if alpha-tested -- buys
// skipping PSMainVoxi (shadow, up to eight cone traces, history, fog) entirely.
//
// Does NOT evaluate AVER_MAT_SLOPE_BLEND's second layer (landscape-only, never drawMesh/
// drawMeshDepthPrepass -- landscape draws through LandscapeRenderer::draw(), one of this feature's
// three excluded paths, SandboxApp.cpp); with AVER_MAT_ALPHA_MASK too, alpha comes from the FIRST
// layer only -- none exists in this tree today.
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

// Depth-only vertex shader for one shadow cascade; which one is in gShadowDraw.x.
float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gCascadeViewProj[(uint)gShadowDraw.x]);
}

#ifdef AVER_INSTANCE_SRV
// AVER_INSTANCE_SRV is the t-register VoxiRenderer.cpp computed for THIS pipeline's layout when it
// compiled this entry point -- see GraphicsPipelineDesc::instanced in RHIResources.hpp. Two macro
// layers so the register NUMBER (not the literal text "AVER_INSTANCE_SRV") gets pasted after "t".
#define AVER_INST_JOIN2(a, b) a##b
#define AVER_INST_JOIN(a, b) AVER_INST_JOIN2(a, b)
StructuredBuffer<float4x4> gInstanceWorlds : register(AVER_INST_JOIN(t, AVER_INSTANCE_SRV));

// VSShadow's instanced twin: one DrawIndexedInstanced submits every surviving instance of one mesh
// in one cascade, instead of shadowPass calling drawMesh() per instance. World comes from
// gInstanceWorlds[instanceID] (VoxiRenderer::shadowPass via drawMeshInstanced), not PerObject's gWorld.
// Everything else is identical to VSShadow.
float4 VSShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gCascadeViewProj[(uint)gShadowDraw.x]);
}
#endif

// The same depth-only pair, for the GI-ONLY shadow map: identical to VSShadow/VSShadowInstanced
// except transforming into gGiShadowViewProj (one box over the GI volume) instead of a cascade
// selected by gShadowDraw.x. Separate entry points: the matrix is picked at pipeline level, and a
// depth-only vertex shader is too hot to spend a dynamic index on.
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
// The scene is rasterised once per frame with no render target; the pixel shader writes lit
// radiance straight into the volume.
struct VoxOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; float2 uv : TEXCOORD1; };

// Adapts a VoxOut to AverVertex. V is exactly zero: there is no camera in a voxelisation pass.
// A distinct name, NOT an overload of averVertexOf: FXC converts between compatible structs and
// makes every call ambiguous (error X3067).
AverVertex voxelVertexOf(VoxOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrm);
    v.V    = float3(0, 0, 0);
    // FALSE, not merely unset: no camera here (V=0 above), so "is the eye inside" has no answer --
    // false keeps the TIR test below inert. HLSL leaves the member uninitialised, so it must be written.
    v.backFace = false;
    v.uv   = i.uv;
    return v;
}

// Voxelisation vertex shader: outputs world space for the geometry shader to project.
VoxOut VSVoxel(VSIn i) {
    VoxOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.nrm  = averTransformNormal(i.nrm, gWorld);
    o.uv   = i.uv;
    o.pos  = wp;
    return o;
}

// Projects each triangle along its dominant axis so it covers the most voxels.
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
// Voxelisation without a geometry shader: same dominant-axis projection, per primitive.
//
// nr[k] uses averTransformNormal(v.nrm, gWorld), not a plain mul, matching VSVoxel/VSMain (1856da1
// fixed four other sites, missed this one since MSVoxel then compiled only behind unused AVER_MS) --
// a plain mul is only correct under rotation/uniform scale; wrong here tints a whole surface's bounce
// for as long as the volume holds it, not a one-frame flicker.
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

// Shades the fragment with shadowed sun plus sky and adds its exitant radiance to the accumulator.
void PSVoxel(VoxOut i) {
    float3 uvw = voxelUVW(i.wpos);
    if (!insideVolume(uvw)) return;
    float3 N = normalize(i.nrm);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    // THE GI-ONLY MAP, not the cascades -- see giShadowFactor for why a voxel cannot use them.
    sun.visibility = giShadowFactor(i.wpos, N, ndl);
    AverSurface s = averEvalMaterial(voxelVertexOf(i), sun);
    float3 albedo = averDiffuseAlbedo(s);

    // ---- ONE AXIAL CONE INTO THE PREVIOUS BAKE ------------------------------------------------
    //
    // One cone (traced once, used twice) fixes two defects that made the volume store an AMBIENT
    // TERM instead of bounced light:
    //   1. SKY WAS UNOCCLUDED: sun.visibility gates the sun term, but averSkyIrradiance(N) depends
    //      only on N, so every voxel got open-sky irradiance regardless of occlusion -- gather ~=
    //      albedo x constant (MEASURED: GI-only Sponza looked like the albedo texture, no pooling
    //      near lit surfaces).
    //   2. NO SECOND BOUNCE: the only readers of gVoxelTex were the mip filter, the forward-pass
    //      gather and the debug raymarch, so light died after one surface -- most of an arcade's
    //      light, which comes from its own walls, dark enough to peg eye adaptation at its ceiling.
    //
    // LEGAL HERE (checked): voxelTex_ stays ShaderResource through voxelizePass's raster draws
    // (UnorderedAccess only just before the resolve), and bindings_ already carries the full-chain
    // SRV at t0 -- no new binding/barrier. Mip 0 alone is cleared; the cone's first sample (dist
    // starts at 2 voxels, 60-deg aperture already wider than 1 voxel there) lands at mip ~1.2, in
    // the intact previous bake.
    //
    // COST/CONVERGENCE: one cone per fragment vs the gather's thirteen. gain = min(albedo x
    // AVER_VOX_FEEDBACK, AVER_VOX_MAX_BOUNCE_GAIN) < 1/channel keeps the series geometric (albedo < 1
    // alone does not guarantee that, once the x3 gain applies).
    const float4 room = traceCone(i.wpos, N, AVER_VOX_INJECT_APERTURE);
    // A cone that terminated on solid geometry saw no sky; one that ran out of volume saw all of it --
    // traceCone treats "left the volume" as unoccluded in .a, correct for "open to the sky" here.
    //
    // ONLY APPROXIMATE, BAD INDOORS AT A FINE VOLUME: one-voxel shells cover ~2% of a cell at the mip
    // 5+ a 60-degree cone samples a few metres out, so it sees through a roof. MEASURED (512^3, PTTest
    // gallery, sun 85.6 deg, each surface painted with its own voxel): sunlit within 1.3x of the path
    // tracer, shadowed interior 30-350x too bright; sky term alone: 0.55 -> 0.011 (path tracer 0.015).
    const float skyVis = saturate(1.0 - room.a);
    // WHILE ReSTIR GI IS THE ESTIMATOR, THE VOLUME CARRIES NO SKY: ReSTIR traces sky visibility with
    // real rays (candidate miss, plus the hit's own second ray at Half/Full) and reads this volume
    // only for surface-bounced light -- leaked sky above counted 3-7x over the path tracer. Keyed on
    // gViewParams.z, the SETTING (see its cbuffer comment for why not gGiRestirParams.x) --
    // VoxiRenderer::voxelSkyInjected() is the same test; rebuild gate and GI cache key both carry it
    // too, so switching giMode rebakes rather than reusing.
    //
    // STILL READS THE SKY FROM HERE: GI-lit particles, the rough-specular cone fallback, and the cone
    // gather when ReSTIR can't run (GI debug view) -- darker/closer to right indoors, darker than
    // right in open shade.
    const float skyInject = gViewParams.z > 0.5 ? 0.0 : 1.0;

    // Exitant radiance, not radiosity: the sun term is an irradiance (takes the 1/PI); the sky and
    // feedback terms are already radiance/exitant (gathered from surfaces run through this shader).
    const float3 bounceGain = min(albedo * AVER_VOX_FEEDBACK, AVER_VOX_MAX_BOUNCE_GAIN);
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility / PI
                                + averSkyIrradiance(N) * gAmbient.r * skyVis * skyInject)
                    + room.rgb * bounceGain;
    // A LAMP'S OWN GLOW: without it, an emissive surface injected nothing and could never light a room
    // through voxel GI. Added after the albedo multiply (emission is light the surface makes, not
    // reflects; s.emissive is already exitant radiance). Clamped to Settings::giRadianceCeiling (default 16).
    radiance += s.emissive;
    radiance = clamp(radiance, 0.0, AVER_VOX_MAXRAD);

    // insideVolume() is inclusive of 1.0, and conservative raster does produce uvw == 1.0 exactly.
    uint3 c = min(uint3(uvw * gVoxelParams.x), (uint)gVoxelParams.x - 1);
    uint3 a = uint3(c.x * 4, c.y, c.z);
    uint prev;
    InterlockedAdd(gVoxelAccum[a],                (uint)(radiance.r * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(1,0,0)], (uint)(radiance.g * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(2,0,0)], (uint)(radiance.b * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(3,0,0)], 1u, prev);   // fragments covering this voxel
}

// All three kernels dispatch over their destination mip's FULL extent (the C++ group count never
// shrinks) and skip per-thread instead: a thread whose voxel falls outside [gBoxLo,gBoxHi) returns
// before touching any resource. With bounded dispatch off, gBoxLo=0/gBoxHi=(level dim), so the guard
// only rejects threads at/beyond the edge -- writes D3D12/Vulkan already drop silently out-of-bounds.
// Not new: deeper mips can overshoot (ceil(mipDim/4)*4 > mipDim; mip 0 never does since resolutions
// are power-of-two, QualityLadder.hpp) and this guard already made that safe.

// Zeroes the accumulator before injection.
[numthreads(4,4,4)]
void CSClear(uint3 id : SV_DispatchThreadID) {
    uint3 v = id + gBoxLo; if (any(v >= gBoxHi)) return;
    uint3 a = uint3(v.x * 4, v.y, v.z);
    [unroll] for (uint k = 0; k < 4; ++k) gVoxelAccum[a + uint3(k,0,0)] = 0;
}

// Turns the fixed-point sums into mip 0 of the filterable RGBA16F volume: the mean radiance of the
// fragments that covered each voxel.
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
// Box-filters radiance and occupancy from one mip into the next. The mip binding set puts a
// SINGLE-MIP view of the source at t0 and the destination level at u0.
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

// Debug view: raymarches the volume straight to screen over the sky. Returns linear radiance.
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
