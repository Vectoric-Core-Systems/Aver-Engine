
// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
#define AVER_SHADOW_CASCADES 4

// register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)), never a literal b4: rhi::kFeatureFrameConstantRegister
// is the one definition (shaderConstantsHlsl() emits this #define; ClusterFrameCB binds the same way). A
// literal would keep compiling against a renumbered slot while the C++ side moved to the new one.
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
    // x = 1 while t6/u2 (gRtShadowHist / gRtShadowHistOut) are bound to real textures this frame;
    // y = 1 once gRtShadowHist ALSO holds a real previous frame (0 right after creation/resize);
    //     0.5 = it does, but the sun changed this frame (VoxiRenderer::beginShadowHistory), so the
    //     sun-DEPENDENT histories (shadow, reflection) test > 0.75 and skip it while the sun-INDEPENDENT
    //     one (sky occlusion) tests > 0.25 and keeps accumulating;
    // z = the current frame index, a pure per-frame count, never wall-clock;
    // w = the pixels-per-ray tile edge as its BIT COUNT (0 = no tiling, every pixel traces).
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
    // x = 1 when the eye is inside a blended single-sided volume, y = that medium's ior.
    // Computed once per frame on the CPU -- a pixel cannot know whether its own volume encloses the
    // camera. See VoxiRenderer's own comment for why a loose bounding-sphere test is safe here.
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
    // w WAS UNUSED, and is now a RUNTIME BIT-FIELD, not a float value -- decode with
    // `uint bits = (uint)gGiShadowParams.w`, never compared or lerped as a float. C++ assembles it
    // every frame (VoxiRenderer.cpp, after fitGiShadow -- see that function's own comment for why it
    // no longer zeroes this field) from three independent Settings toggles, each OFF by default and
    // each an A/B-measured trade against the staged ray-driven cost breakdown, not a correctness fix:
    //   bit 1 (Settings::rtSecondaryShadowOpaque, console voxi.rtSecondaryShadowOpaque): rtReflection's
    //     hit and giTraceInitialCandidate's hit (voxi_restir.hlsli) call rtShadowOpaque (voxi_rt.hlsli)
    //     instead of rtShadow for their own sun-shadow ray -- one first-hit-terminated ray against the
    //     opaque-including-cutouts lane, trading away the tinted shadow a translucent pane would cast
    //     on a SECONDARY hit. Primary shadows (rtShadowTemporalEx / CSRdShadow / CSRdShadowProbe) are
    //     untouched.
    //   bit 2 (Settings::rtSkyOcclusionHalfRate, console voxi.rtSkyOcclusionHalfRate):
    //     rtSkyOcclusionTemporal (voxi_rt.hlsli) skips its rtAmbientTraced ray on half of this frame's
    //     8x8 tiles (alternating which half by frame), wherever that pixel's reprojected AO history is
    //     valid -- the reprojected value stands in as this frame's fresh estimate.
    //   bit 4 (Settings::rtReflectionHalfRate, console voxi.rtReflectionHalfRate): rtReflectionTemporalEx
    //     (voxi.hlsl) skips its rtReflection ray the same tiled way, for a ROUGH pixel only -- a mirror
    //     (lobeRough == 0) always retraces, since a reprojected mirror reflection is wrong under motion.
    // With all three bits clear this field reads 0.0, exactly as it always has, and every shader above
    // computes exactly what it did before these existed.
    float4   gGiShadowParams;
    // The SPATIAL shadow denoiser. x = filter radius in pixels (0 = off); y = how much of the
    // filtered value to take (0 = none: taps still run and the result is discarded -- the
    // cost-measurement configuration; lerp(v, f, 0) is v exactly for any finite f); z, w unused.
    // Radius lives in a CONSTANT, not a #define, so the tap loop stays dynamic and can't be
    // unrolled away at zero taps -- a compile-time 0 would measure nothing and call it free.
    float4   gRtDenoiseParams;
    float4   gPtBounceParams;
    // x = total cones the diffuse gather traces, including the axial one.
    // y/z/w are REFRACTION, not spare: y = mode (Settings::refractionMode, 0 = off),
    // z = strength, w = edge fade. averRefractedBackdropUV below reads all three. They said
    // "unused" for as long as refraction has existed -- the feature landed in a row this
    // comment still described as free, which is exactly how the next person adding a field
    // near here would have overwritten it.
    float4   gGiParams;
    // x = sky-visibility rays the ambient term traces per pixel; 0 means "use the cone gather's own
    // occlusion instead", which is every tier below the top and is what this shader did before the
    // rays existed.
    // y = the COHERENCE TILE EDGE those rays share a direction across (1 = per-pixel). Claimed within
    // days of this row being written down as spare, which is what gGiParams directly above warns
    // happens.
    // z = LIGHTING LEGACY BITS (VoxiRenderer::setLightingLegacyBits; a u32 bitmask stored as a float,
    // 0 meaning everything corrected -- a block that has never been written is all zeros, which is
    // exactly the "corrected" state). Each bit restores one pre-fix behaviour for A/B comparison only;
    // decoded INLINE at each reader as `((uint)gAmbientParams.z & <bit>u) != 0u`, never through a
    // shared helper, so every reader stays independently grep-able:
    //   bit 1  (R0) the ReSTIR candidate ray and the sky-occlusion ray sample a fixed 45-degree ring
    //          again, instead of a cosine hemisphere (voxi_restir.hlsli, voxi_rt.hlsli).
    //   bit 2  (R1) a ReSTIR-supplied receiver's own sky is counted twice again -- once through the
    //          traced miss, once more through ind4.ambient/ind.ambient. THIS FILE reads this bit: see
    //          the sky-ownership subtraction in PSMainVoxi and PSRayDriven below (F4).
    //   bit 4  (R2) a ReSTIR candidate's hit point reads the sky again with no visibility test and no
    //          cosine weighting (voxi_restir.hlsli).
    //   bit 8  (R3) a reused ReSTIR sample is shaded again with no visibility test between the shading
    //          point and the sample (voxi_restir.hlsli).
    //   bit 16 (R6) the cone gather's directions are weighted by cosine again on top of an already
    //          cosine-distributed direction, i.e. cos^2 (voxi_cone.hlsli, voxi_gi.hlsli).
    //   bit 32 (W6) a blended-replay fragment (glass/water) writes its own per-pixel history --
    //          reservoir, surface history, NRD GI input, RT shadow/AO/reflection history -- again,
    //          instead of leaving the opaque surface's own history alone (voxi_restir.hlsli,
    //          voxi_rt.hlsli, PSMainVoxi's rtReflectionTemporal below). Console
    //          voxi.legacyBlendedHistoryWrite; --lighting-legacy 32.
    //
    // w carries GI-VISIBILITY (U1) and BLENDED-HISTORY (W6/M5) bits, decoded the same inline way as z
    // above -- NOT through packAmbientW's own inverse at the read site, only at the one C++ writer
    // (VoxiRenderer::beginShadowHistory, aver::voxi::givis::packAmbientW):
    //   bits 0-1 RestirVisibility mode (Settings::giRestirVisibility, clamped 0..3): 0 No ray,
    //          1 Reconstructed, 2 HalfResolution, 3 Full. Decoded once, at the top of
    //          giRestirIndirect (voxi_restir.hlsli), into f2Path/f3Path -- every other reader of
    //          this mode reads those two derived values, never this field a second time.
    //   bit 4  the half-resolution visibility pair (gGiVisHist/gGiVisHistOut, t16/u10,
    //          voxi_restir.hlsli) is bound THIS frame. Mode 2 with this bit clear means the pair
    //          failed to allocate (2.11) and behaves as Full, not a null-descriptor read.
    //   bit 8  gGiVisHist (t16) holds a REAL previous frame, not just-created or just-resized
    //          storage -- the sibling of gGiRestirParams.y for this pair.
    //   bit 16 (W6/M5) a blended-replay fragment takes the voxel-cone gather instead of ReSTIR for
    //          its diffuse term -- voxi.blendedGiCone / --blended-gi cone, PSMainVoxi's own M5
    //          branch below. Default restir (this bit clear): today's image, unchanged.
    //   bit 32 the RENDERING BACKEND replayed translucent draws blended THIS frame (D3D12 only --
    //          Vulkan never marks a draw blended, VulkanDevice.cpp, C10) -- what actually enables
    //          W6's per-fragment discriminator (averDrawIsTranslucent() below) to mean anything;
    //          without it every fragment behaves as opaque for history-write purposes regardless of
    //          material, which is the correct answer on a backend that has no blended replay pass to
    //          protect history from in the first place.
    //   bit 64 voxi.giVisPathView (2.10 I): paints giRestirIndirect's own F2 path colour in place of
    //          shading, suppressed while the poison view (gGiRestirParams.w) above is also on.
    //   bits 7-11 (>> 7 & 31u) Settings::giRestirMovingAge (0..31, clamped): the moving-camera
    //          ReSTIR reservoir-age cap, decoded by giRestirIndirect (voxi_restir.hlsli) alongside
    //          visMode and applied to stparams.maxReservoirAge AFTER the motion discount below it is
    //          computed, not beside this decode. 0 (THE DEFAULT) means legacy -- always the existing
    //          30-frame cap, byte-identical to every image this renderer produced before this field
    //          existed. Was meant to remove a fade left behind by a moving camera's reservoirs keeping
    //          stale, often brighter reprojected radiance for up to the full 30-frame cap after the
    //          camera stops -- BUT THE OWNER TESTED A NONZERO DEFAULT (3, bd6e2045) BY HAND AND THE
    //          FADE CAME BACK UNCHANGED, so this cap was never the carrier either (see
    //          Settings::giRestirMovingAge's own comment for the full account); the field, its bits
    //          and the console variable all stay as a legitimate dial, only the silent nonzero default
    //          is gone. NOT the spatial-reuse motion discount 3dbc9a42 targeted and 8daed7f1 reverted,
    //          which freed these five bits first -- a moving-vs-still discount, a moving-vs-still
    //          RESERVOIR AGE, and the spatialSamples/reuse-tolerance split just below are three
    //          different attempts at the same still-open fade, tried in that order.
    //   bits 12-15 (>> 12 & 15u) Settings::giRestirSpatialSamples (0..15, clamped): overrides the
    //          spatial-reuse tap count (stparams.numSamples) the motion discount above would otherwise
    //          compute, decoded by giRestirIndirect alongside visMode/movingAge and applied AFTER that
    //          same discount, splitting spatial reuse from temporal reuse to localise the fade neither
    //          of the two bit ranges above it fixed. 15 means AUTO -- leave the discount alone, byte-
    //          identical to today's image; 0 disables spatial reuse outright (temporal only); 1..8 pin
    //          the count. See Settings::giRestirSpatialSamples's own comment (Voxi.hpp) for the
    //          bisection this is one half of; gViewParams.z/.w below are the other half (the RTXDI
    //          reuse-similarity tolerances, too continuous a value to pack into bits here).
    float4   gAmbientParams;
    // Editor view modes the ray-driven path honours itself. x = unlit.
    // Mirrors FrameConstants::viewParams -- appended at the END, so every offset above is
    // untouched. See VoxiRenderer.hpp's static_assert for the guard that makes that a rule.
    // y WAS SPARE; NOW the live GI radiance ceiling (Settings::giRadianceCeiling) -- see
    // AVER_VOX_MAXRAD below, which reads this field with a fallback to today's 16.0 literal.
    // z = 1 when ReSTIR GI is the CHOSEN diffuse estimator (VoxiRenderer::giRestirWanted(), a
    // settings-level answer), 0 otherwise -- read by PSVoxel to leave the sky out of the volume. NOT
    // gGiRestirParams.x, which also drops to 0 on a frame the estimator merely cannot run (the GI
    // debug view, an empty TLAS): keying the bake on that rebuilt the volume twice per debug-view
    // toggle. w = the staged ray-driven passes' visibility-record row pitch while they record, 0 for
    // every other pass (see gRdVisBuf). (z/w briefly carried RTXDI's reuse tolerances during the
    // ReSTIR fade bisection; those went back to literals in voxi_restir.hlsli.)
    float4   gViewParams;
    // RTXDI ReSTIR GI control (Settings::giMode) -- mirrors FrameConstants::giRestirParams, also
    // appended at the end for the same reason gViewParams was. x = 1 while giMode==1 is ACTUALLY
    // running this frame (VoxiRenderer::giRestirWanted(), never the raw setting -- see that
    // member's own comment for why touching t12/t13/u6/u7/u8 on the raw setting alone would be a
    // null-descriptor read on hardware that cannot run this). y = 1 once gGiSurfPosHist/
    // gGiSurfNrmHist ALSO hold a real previous frame. z = which of RTXDI's two reservoir-array
    // slices THIS frame writes (the other is last frame's, read as this frame's temporal source).
    // w = the ReSTIR-GI poison debug view (voxi.giPoisonView / VoxiRenderer::setGiPoisonView): >0.5
    // makes voxi_restir.hlsli's giRestirIndirect paint an unmistakable colour per non-finite guard
    // instead of the real indirect diffuse -- see that function's own POISON DEBUG VIEW comment for
    // the legend. ALSO READ DIRECTLY IN THIS FILE, in BOTH PSMainVoxi and PSRayDriven, for an EIGHTH
    // colour (violet) that has nothing to do with giRestirIndirect: the ray-traced SPECULAR indirect
    // term (ind4.specular / ind.specular) has its own AVER_VOX_MAXRAD ceiling clamp, and this flag
    // makes a pinned pixel there paint violet too (B1/F5) -- see aver_IsGiRestirPoisonColour's own
    // comment, just above PSMainVoxi, for the precedence chosen between the two families. UNLIKE the
    // seven giRestirIndirect colours, this eighth one is NOT giMode-gated: the RT specular ray runs
    // under either diffuse estimator, so it fires on giMode 0 too. Was "spare"; this is a repurposed
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
// THE CEILING ON VOXEL/GI RADIANCE -- NOW A LIVE PER-FRAME VALUE, not a compile-time constant. Rides
// gViewParams.y (Settings::giRadianceCeiling -> VoxiRenderer::prePass -> cb_.viewParams[1], see that
// field's cbuffer comment above and Voxi.hpp's own comment on the field for the full story of what
// this caps and why 16.0 is not a headroom number). Falls back to today's literal 16.0 whenever the
// field reads exactly 0 -- an UNSET FrameConstants block (giFrameConstants() read before this
// renderer's first prePass ever ran; SandboxApp.cpp's cluster-GI binder documents exactly that
// all-zero-block window) -- so this macro is BYTE-IDENTICAL to the #define it replaces until a
// project or the console (voxi.giRadianceCeiling) actually asks for a different number.
// Parenthesised as a single expression so every existing `AVER_VOX_MAXRAD` use site (a runtime
// function argument in every case in this file, never a static const initialiser) stays live without
// being rewritten.
#define AVER_VOX_MAXRAD (gViewParams.y > 0.0 ? gViewParams.y : 16.0)
// The aperture of the single cone PSVoxel traces into the previous bake, as tan(half-angle).
// 0.577 is tan(30), a 60-degree cone -- the same shape the forward gather's own axial cone uses, so
// the two agree about how much of the hemisphere a voxel can see rather than being two different
// estimates of the same quantity. Wide on purpose: this is a hemisphere-coverage question, not a
// directional one, and a narrow cone would answer it with a single corridor.
#define AVER_VOX_INJECT_APERTURE 0.577
// How much of the light already in the room is re-emitted on the next bake. THIS IS A COMPENSATION
// CONSTANT, not a physical quantity: this cone estimates radiance along the surface normal from a
// single 60-degree cone, while the forward gather it feeds integrates thirteen cones over the
// hemisphere and normalises by their cosine weights -- one cone under-counts what the gather
// actually integrates, and the multiplier makes up the difference. 1.0 and 8.0 were measured (8.0
// ran far too hot); 3.0 was checked once against the path tracer on Sponza (commit c6d1a750,
// scripts/pt-compare.ps1) and closed most of the gap to the reference. It is not derived
// analytically -- a proper sweep would likely move it. Kept as a named constant because it is the
// first dial anyone will reach for if a scene ever blows up, and because 0 turns the second bounce
// off for an A/B without touching anything else.
#define AVER_VOX_FEEDBACK 3.0

// Edge, in pixels, of the tile that shares one sky-occlusion ray direction. See rtSkyOcclusion for
// why coherence rather than ray count is the lever here. 1 = a fresh rotation per pixel, which is
// what this shader did before the dial existed and is bit-identical to it.
#ifndef AVER_AO_COHERENCE_TILE
#define AVER_AO_COHERENCE_TILE 1.0
#endif

// AVER_AO_UNIFIED: let the ambient ray answer more than one question.
//
// THE OBSERVATION. rtSkyOcclusion fires a full hemisphere BVH traversal per sample and keeps ONE BIT
// of what it learns -- `open += 1.0` on a miss. On a miss it has just seen the sky, in a direction it
// knows, and throws that away while skyColor() marches the atmosphere 32 steps elsewhere in this same
// shader for the same information. On a hit it carries ACCEPT_FIRST_HIT_AND_END_SEARCH, so it does
// not even learn WHICH surface stopped it -- while thirteen cones march the voxel volume estimating
// the bounced light off exactly those surfaces.
//
// So one ray is paying for three answers and returning a third of one. At 1 this file behaves exactly
// as it always has; at 1 the ray keeps all three:
//
//   MISS   -> averSkyRadianceCheap(dir), the SH sky evaluated along the ray. This is strictly better
//             than what it replaces, and not only cheaper: the current term is an unoccluded
//             hemispherical mean scaled by a scalar, so a room with one window is lit by an average
//             of the whole sky dimmed to taste. Per-direction sampling knows WHICH sky got in.
//   HIT    -> the voxel volume sampled AT THE HIT POINT, mip 0. The cone gather's answer to the same
//             question is a widening cone that starts leaking through thin walls as it climbs mips;
//             a ray that actually traversed the geometry cannot leak, because it stopped.
//
// WHAT IT COSTS, AND WHY THIS IS A HYPOTHESIS RATHER THAN AN IMPROVEMENT. Dropping
// ACCEPT_FIRST_HIT_AND_END_SEARCH turns an any-hit query into a closest-hit one, which is typically
// 1.5-2x per ray -- the query can no longer stop at the first thing it touches. The bet is that this
// buys the removal of the GI cone gather (4.05 ms), the specular cone (2.63 ms) and both atmosphere
// marches (2.69 + 2.21 ms). If the closest-hit ray costs more than the ~11.6 ms of marching it
// replaces, the idea is simply wrong and this define should be deleted rather than defaulted on.
// MEASURE IT ON A MOVING CAMERA: every share quoted above is a still-camera share.
//
// AT 1 THE TWO PRIMARY-VISIBILITY PATHS DELIBERATELY DISAGREE, and that is the one thing about this
// switch that must not surprise anyone. Only PSRayDriven is wired to the unified gather; PSMainVoxi
// still takes the cone gather plus a scalar occlusion. Elsewhere this file insists the two must
// match ("they currently agree to 2.23 MAD and that is worth keeping") and that is still the rule --
// it is simply suspended inside a measurement mode that is off in every shipped configuration.
// Wiring PSMainVoxi follows once the trade is measured, NOT before: doing both at once would mean
// the first number came from a build with no unchanged path left to compare against.
//
// AND IT IS NOT ON A QUALITY TIER, for the same reason AVER_RD_ABLATE is not: this changes what the
// image MEANS, not how much of it there is. A tier that silently swapped the estimator would make
// two tiers of the same scene incomparable.
#ifndef AVER_AO_UNIFIED
#define AVER_AO_UNIFIED 0
#endif


// The roughness below which a reflective surface is treated as a MIRROR: no cone, no temporal
// history, no spatial filter. See rtReflectionTemporal's own comment for why all three must be
// derived from this one number rather than each choosing its own threshold.
#define AVER_REFL_MIRROR_ROUGH 0.1

// TLAS instance-mask lanes. MUST MATCH kRtMaskOpaque/kRtMaskTranslucent in VoxiRenderer.cpp -- no
// shared-source mechanism ties C++ and HLSL, so a value changed on one side only is an image bug
// with no build error, like the cbuffer mirrors this file already warns about.
//
// Every ray but the shadow ray asks for OPAQUE only. Translucent panes sit in the structure marked
// FORCE_NON_OPAQUE; a RayQuery meeting one does not commit it, so a single Proceed()+CommittedStatus
// traversal (every non-shadow ray here) would stop AT the pane and miss whatever is behind it.
// Narrowing the mask keeps those rays from ever seeing translucent geometry, so their single-Proceed
// stays valid.
#define AVER_RT_MASK_OPAQUE      0x01
#define AVER_RT_MASK_TRANSLUCENT 0x02
// THE VIEWER'S OWN BODY: an opaque instance every ray may hit EXCEPT the ray-driven primary one. A
// first-person camera sits inside its own character's head, so an unfiltered primary ray hits the
// inward-facing mesh and fills the screen with the character's own skin.
//
// scene::kMeshRendererHiddenFromOwner already exists for this; the raster walk in SandboxApp skips
// drawMesh() for it but deliberately keeps it in the acceleration structure (still a shadow caster,
// still in the GI volume) so raster primary visibility is unaffected. Ray-driven primary visibility
// -- the DEFAULT -- traces the TLAS directly, so "hidden" never reaches it. Measured: identical pose
// and log, probe 43,33,28 (ray-driven) vs 206,215,218 (--rt-render-mode 0).
//
// A third lane rather than removing the instance: every ray except the primary one still wants this
// geometry (shadow, GI bounce, mirrors, glass). The shadow ray masks AVER_RT_MASK_ALL and picks up
// this lane for free.
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
// THE OPAQUE SCENE, COPIED BEFORE TRANSLUCENCY REPLAYS -- see IDevice::sceneColorBackdropTexture.
// It exists so a blended surface can tint what is behind it PER CHANNEL. Hardware blending gives one
// scalar (1 - src.a) for the destination, and volume absorption is per-channel by definition, so
// without this a pane of glass can only get darker with depth, never greener.
//
// MAY BE NULL-FILLED: before the first resize, under MSAA (the copy is invalid from a multisampled
// target), or on a backend that does not implement it. averBlendBackdropValid() below is how a
// caller asks, and every use falls back to the scalar composite when it says no.
Texture2D<float4>         gBlendBackdrop : register(t10);

// ---- Occlusion-aware fog: the air sky-visibility volume ----
//
// PROBLEM: height fog (averFogFactor/averFogInscatter/averApplyFog, shared_prelude.hlsl) and the
// aerial-perspective term (averApplyFogAirVis's own `aerial` branch) both add in-scattered SKY light
// along every view ray with NO occlusion. The air inside an enclosed room sees almost none of the
// sky, yet fog adds sky radiance as though it stood in the open -- washing bounce-lit walls into a
// flat blue veil. Gating this on the surface's own screen-space AO history was tried and reverted
// (aver-fog-skyvis-failed.md): fog is a property of the CAMERA-TO-SURFACE PATH, not the surface's
// hemisphere, and a per-pixel, temporally-accumulated signal flashes open on camera motion
// (disocclusion falls back to "open"). This volume is PATH-based and WORLD-SPACE instead -- every
// cell answers "how much of the upper hemisphere of sky can the air HERE see" -- with NO per-pixel
// history and NO jitter, so camera motion cannot make it flash.
//
// gAirVis covers EXACTLY the GI voxel volume, the SAME mapping voxelUVW/insideVolume already use
// (uvw = (p - gVoxelOrigin.xyz) * gVoxelOrigin.w, voxi_cone.hlsli) -- just at its own fixed 32x32x32
// resolution, independent of gVoxelParams.x (the GI radiance volume's own, tier-dependent, resolution;
// see AVER_AIRVIS_RES, further down this file, by CSAirVis). gAirVis (t17) is what the shade passes
// read (voxiAirVisibility, further down); gAirVisOut (u16) is CSAirVis's write target -- the same
// SRV/UAV split gVoxelTex/gVoxelUAV already use for the radiance volume, and for the same reason: one
// resource can't be bound as both in the same descriptor table slot.
//
// kVoxiSrvCount 17 -> 18, kVoxiUavCount 16 -> 17 (VoxiRenderer.cpp, not this file) -- the next free
// slot after this table's t16/u15 (voxi_restir.hlsli's gGiVisHist, and gRdReflTex above), so no other
// SRV/UAV register moves and no material texture register (based at t(kVoxiSrvCount)) is touched.
//
// WHEN THE FEATURE IS OFF, OR THE VOLUME DOESN'T EXIST YET (VoxiRenderer.cpp, not this file): BOTH
// slots stay bound to a 1x1x1 placeholder. voxiAirVisibility treats "gAirVis dimensions <= 1" as "no
// volume" and returns 1.0 -- today's unoccluded behaviour -- so a build or a frame with the setting
// off is bit-identical to what stood here before this feature existed.
Texture3D<float>          gAirVis    : register(t17);
RWTexture3D<float>        gAirVisOut : register(u16);

// Placed above AVER_RT (used to sit inside it, silently losing the definition for the shadow and
// GI-shadow pipelines and breaking 4 of them): pure arithmetic on the clock and a box, no rays needed.
// ---- CAUSTICS: light focused by the water surface onto what lies under it ----
// Projected from the volume (gCausticMin/Max), not painted into a material, so it stops exactly
// where the water stops rather than bleeding onto any surface sharing that material.
// Approximates caustic brightness as the Laplacian of the height field, analytic for a sum of sines
// (-k^2*sin(phase) per term) -- differentiates the same three sines the ripple graph uses, twice.
// NOT real caustics: no light-path/sun-angle/wall dependence, just a focus term under a flat pool.
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
// DECLARED UNCONDITIONALLY, ABOVE THE #if AVER_RT BELOW, EVEN THOUGH EVERY READER OF IT LIVES INSIDE
// THAT GUARD -- because PSMainVoxi, which SETS it (see this function's own top, after N is built), is
// NOT itself behind #if AVER_RT: a rasteriser-only build (AVER_RT 0) must still compile that
// assignment, even though gAverHistoryWrite then goes unread all the way down. The alternative --
// declaring it inside the guard, beside its only readers -- would fail that build on an undeclared
// identifier, exactly the kind of "compiles on one variant, breaks the other silently at a DIFFERENT
// build's compile time" trap this file's own header comments warn about elsewhere (voxi_restir.hlsli's
// ordering-contract note, most explicitly).
//
// `static`, NOT a cbuffer field or a new gAmbientParams bit: like gGiPoisonPdfHit (voxi_restir.hlsli),
// this is per-invocation storage, fresh for every pixel-shader thread, set once near the top of
// whichever entry point runs (PSMainVoxi below; PSRayDriven sets it unconditionally true, see that
// function's own comment) and read by every gated history write between here and the end of the
// translation unit. true is the correct default: an entry point that never touches it (VSMain, VSky,
// PSVoxel, ...) behaves exactly as if the write were unconditional, which is what every one of them
// was before this task.
static bool gAverHistoryWrite = true;

// Is THIS fragment a translucent (glass/water) draw, replayed blended? Mirrors pbr::isTranslucent
// (Material.cpp:70-72: `alphaMode == Blend || transmission > 0`) exactly, and for the same reason
// that function checks both: AVER_MAT_ALPHA_BLEND alone is set only for AlphaMode::Blend
// (MaterialGpu.cpp:94), so an opaque-flagged material authored with transmission > 0 (frosted glass
// with no alpha blending, say) would otherwise fall through this test and keep writing history it
// should not. gTransmission is a material constant (material_prelude.hlsl:66); AVER_MAT_ALPHA_BLEND
// is defined at material_prelude.hlsl:113. Called only from behind gAmbientParams.w bit 32 (PSMainVoxi,
// below) -- on Vulkan, where that bit is never set, this function is declared but never evaluated at
// runtime for any fragment, matching C10's own finding that Vulkan never marks a draw blended.
bool averDrawIsTranslucent() { return (gMaterialFlags & AVER_MAT_ALPHA_BLEND) != 0u || gTransmission > 0.0; }

#if AVER_RT
#include "voxi_rt.hlsli"

#include "voxi_restir.hlsli"

// ---- STAGED RAY-DRIVEN PASSES (milestone 1): the visibility record and resolved sun visibility ----
//
// voxi.rayDrivenStages (Settings::rayDrivenStages, RENDER.RDSTAGES, --rd-stages, u32 0/1/2) splits the
// single PSRayDriven fullscreen draw below into three GPU passes when set to 1 (or 2, below): a compute
// pass traces the primary ray and writes one record per pixel here (CSRdVisibility, further down this
// file, under this same AVER_RT region); a second compute pass reads it, reconstructs the surface and
// resolves the sun shadow into gRdSunVisTex (CSRdShadow); and PSRayDriven itself, compiled a second time
// with AVER_RD_SPLIT=1, reads both instead of tracing and calling rtShadowTemporal -- everything after
// that point is the SAME shading code the single pass already runs, unchanged. At 0 (the default) none
// of this exists at runtime: no new resource is bound, no new pipeline is built, and PSRayDriven's own
// AVER_RD_SPLIT=0 compile is byte-for-byte the single pass this file already had.
//
// VALUE 2 (milestone 4) is 1 PLUS one more thing: CSRdGi, further down this file, is compiled a second
// time with AVER_GI_CHECKERBOARD=1 and traces ReSTIR GI's candidate for only half the pixels each frame,
// in NRD's own checkerboard pattern, leaving REBLUR to reconstruct the rest -- see that compile's own
// header comment and voxi_restir.hlsli's AVER_GI_CHECKERBOARD section for the full contract. Every OTHER
// stage (CSRdVisibility/CSRdShadow/CSRdSkyOcc/CSRdRefl, PSRayDriven both variants) runs unchanged at 2;
// only CSRdGi's own dispatch gains the extra compile.
//
// TWO NEW UAV REGISTERS, u11/u12 -- the next two free slots after gGiVisHistOut's u10 (voxi_restir.
// hlsli). Bound in every stage through the SAME descriptor table the single pass already uses
// (kVoxiUavCount 11->13, VoxiRenderer.cpp, not this file), so no SRV slot moves and no material
// texture register is touched.
//
// gRdVisBuf: one uint4 per pixel, index = pixel.y * pitch + pixel.x, pitch = rdRowPitch() -- this
// cbuffer's own w field, "spare, written 0" until this feature -- VoxiRenderer::recordStagedRayDriven
// sets it to the row pitch in pixels for the staged uploads and back to 0 after, so every other pass
// still reads 0. A HIT is uint4(instanceID, primitiveIndex, asuint(bary.x),
// asuint(bary.y)); a MISS is x == 0xFFFFFFFFu (y/z/w undefined, never read on that path).
RWStructuredBuffer<uint4> gRdVisBuf    : register(u11);
// gRdSunVisTex: this frame's resolved sun visibility, one RGBA16F texel per pixel, rgb = the same
// tinted transmittance rtShadowTemporal returns (already through its own temporal/spatial filters),
// alpha unused. Written once by CSRdShadow, read once by PSRayDriven's AVER_RD_SPLIT branch. Not
// itself a history buffer -- rtShadowTemporal's own gRtShadowHist/gRtShadowHistOut pair still owns the
// actual frame-to-frame history underneath it; this texture only ferries one frame's answer from
// Stage S to Stage B.
RWTexture2D<float4>       gRdSunVisTex : register(u12);

// A: SUN SHADOW SPLIT (Settings::rayDrivenShadowTiles) -- CSRdShadowProbe's OWN OUTPUT, further down
// this file: one uint per 8x8 tile of the FULL render target (same W/H/pitch ensureRdStagedResources
// sizes rdVisBuf_ from), classifying that tile's shadow answer as it looked to one probe ray per pixel
// -- bit 1 set if EVERY probed pixel in the tile was fully blocked, bit 2 if EVERY one was fully lit,
// bit 4 if any pixel disagreed (a real penumbra, an alpha-tinted hit, or a mix of the two). 0 means
// nothing in the tile had a surface to test (every pixel out of viewport or sky). CSRdShadow's own
// AVER_RD_SHADOW_TILES compile ORs its 3x3 tile neighbourhood's worth of these and skips its ray loop
// only where that OR is EXACTLY 1 or EXACTLY 2 -- see that compile's own header for why 3x3, not 1x1.
//
// kVoxiUavCount 17 -> 19, u17/u18 (VoxiRenderer.cpp, not this file) -- gRdGiCand (voxi_restir.hlsli, B1)
// takes u17 first since that file is #included above this point; this is the next free slot after it.
#ifndef AVER_RD_SHADOW_TILES
#define AVER_RD_SHADOW_TILES 0
#endif
RWStructuredBuffer<uint> gRdShadowTiles : register(u18);

// gRdGiTex / gRdAoTex -- MILESTONE 2's pair, splitting PSRayDriven's own diffuse-GI and sky-occlusion
// answers the same way milestone 1 split its shadow answer above: one RGBA16F texel per pixel, each
// written once by its own dedicated compute stage (CSRdGi / CSRdSkyOcc, further down this file, under
// this same AVER_RT region) and read once by PSRayDriven's AVER_RD_SPLIT branch, further down still.
// Like gRdSunVisTex, NEITHER is itself a history buffer: the real frame-to-frame state each answer
// depends on (gGiReservoirs/gGiSurfPosHist/gGiSurfNrmHist for GI, gAoHist/gAoHitDistOut for sky
// occlusion, all voxi_restir.hlsli/voxi_rt.hlsli-owned) is untouched by this pair -- these two textures
// only ferry ONE frame's answer from their own compute stage to Stage B.
//
// kVoxiUavCount 13 -> 15 (VoxiRenderer.cpp, not this file) -- the next two free slots after
// gRdSunVisTex's u12, same descriptor table every other stage already uses, so no SRV slot moves.
//
// gRdGiTex: rgb = giRestirIndirect's diffuse radiance, alpha unused. Written by CSRdGi only when GI is
// actually ReSTIR this frame (gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5) -- the cone-traced
// branch has no per-pixel history to split out this way and stays inside Stage B itself, unchanged.
RWTexture2D<float4>       gRdGiTex     : register(u13);
// gRdAoTex: r = rtSkyOcclusionTemporal's occlusion, g/b/a unused (a is 1 on every write so the texel is
// never left at a defined-but-meaningless alpha). Written by CSRdSkyOcc only in the two cases where
// PSRayDriven's own rdAo is still its un-gathered initial 1.0 -- ReSTIR supplies diffuse, or there is
// no voxel GI at all -- because in cone-GI mode the occlusion rides the cone gather's own accumulator
// and stays in the shade pass. See CSRdSkyOcc's own header for the exact gate.
RWTexture2D<float4>       gRdAoTex     : register(u14);

// gRdReflTex -- MILESTONE 3's addition: splitting PSRayDriven's own ray-traced REFLECTION answer the
// same way milestones 1/2 split shadow/GI/sky-occlusion above. One RGBA16F texel per pixel, written
// once by its own dedicated compute stage (CSRdRefl, further down this file, under this same AVER_RT
// region) and read once by PSRayDriven's AVER_RD_SPLIT branch, further down still. Like gRdSunVisTex/
// gRdGiTex/gRdAoTex, NOT itself a history buffer: the real frame-to-frame state (gRtReflHist/
// gRtReflHistOut, t7/u3, this file's own rtReflectionTemporal) is untouched by this texture -- it only
// ferries ONE frame's answer from CSRdRefl to Stage B.
//
// kVoxiUavCount 15 -> 16 (VoxiRenderer.cpp, not this file) -- the next free slot after gRdAoTex's u14,
// same descriptor table every other stage already uses, so no SRV slot moves.
//
// rgb = the same clamped specular colour PSRayDriven's own reflection branch computes today (a real
// mirror/glossy hit, the atmosphere march blended in at a rough surface's sky weight, or plain sky --
// see CSRdRefl's own body for which of those three a given pixel took).
//
// ALPHA IS NOT "unused" THE WAY THE OTHER THREE STAGED TEXTURES' SPARE CHANNELS ARE: it is THE STAGE'S
// OWN DECISION, exactly the phrase this field's contract uses -- 1.0 where CSRdRefl actually traced
// this pixel (its own copy of PSMainVoxi's `gShadowParams.z > 0.5 && gRtParams.w > 0.5 && rough <=
// 0.75` gate; 2.0 when that traced value also hit the radiance ceiling before its clamp, for the poison
// view), 0.0 everywhere else (roughness routed the pixel to the cone/sky fallback instead, or
// there was no surface at all). Stage B reads this alpha, not a recomputed roughness test, to choose
// between the traced answer and its OWN cone/sky fallback -- see PSRayDriven's AVER_RD_SPLIT branch,
// further down this file, and rdSurfaceRoughness's own header for why the roughness test cannot simply
// be repeated there instead.
//
// A FOURTH ALPHA, ONLY UNDER AVER_RD_REFL_SPLIT (Settings::rayDrivenReflSplit): alpha < -0.5 is PENDING,
// written by CSRdRefl's own R1 compile in place of composing, when a history was bound to defer
// rtReflectionSpatial's gather into CSRdReflFilter's own R2 pass rather than pay it here -- rgb carries
// that pixel's skyR and alpha carries `-1.0 - rough` (rough is always in [0, 0.75], so this is always
// < -0.5, never colliding with a real 0.0/1.0/2.0 output). CSRdReflFilter overwrites every PENDING texel
// with a real one before Stage B ever reads this texture; see CSRdRefl's and CSRdReflFilter's own
// comments, further down this file, for the full two-pass contract.
RWTexture2D<float4>       gRdReflTex   : register(u15);

// gViewParams.w carries the staged buffers' row pitch as an exact integer (see gRdVisBuf's own header
// comment above) PLUS, from milestone 4 on, per-dispatch flag bits above it -- bit 16 is CSRdGi's
// half-rate-GI checkerboard parity (AVER_GI_CHECKERBOARD, voxi_restir.hlsli), set only for that one
// dispatch's own constant-buffer upload; VoxiRenderer::recordStagedRayDriven restores the plain pitch
// immediately afterward, so every OTHER decode of this field must mask the flag bits away rather than
// read them as part of the pitch. After the staged passes, on a frame CSRdGi packed NRD's input, the
// field holds bit 17 (plus that parity in bit 16) and no pitch for the rest of the frame --
// giRestirIndirect's non-checkerboard NRD-input write reads it to follow the packing. Declared here, before every stage that decodes a pitch (PSRayDriven's
// AVER_RD_SPLIT branch is the first, further down), so it is in scope everywhere it is needed.
uint rdRowPitch() { return (uint)gViewParams.w & 0xFFFFu; }

// The surface PSRayDriven reconstructs from a ray hit, minus everything that hit computed for itself
// (bary, dir, N before its face-the-ray flip): what Stage S and Stage B's AVER_RD_SPLIT branch both
// need afterward, and nothing they don't -- dpx/dpy (the shadow-ray footprint) and L (the light
// direction) are cheap and hit-independent, so each caller keeps computing those itself.
struct RdSurface {
    RtInstance inst;
    uint       i0, i1, i2;
    float3     w;
    float3     N;
    float2     hitUV;
    RtMaterial mat;
    float      hitT;
    float3     wpos;
};

// Rebuilds a PSRayDriven-shaped surface from CSRdVisibility's record instead of a live RayQuery.
// TRANSCRIBED FROM PSRayDriven'S OWN POST-TRACE STATEMENTS (this file, the AVER_RD_SPLIT==0 branch of
// PSRayDriven below): inst/tri/i0/i1/i2/w/nObj/N/hitUV/mat are copied verbatim, substituting the
// record's fields for the RayQuery accessors they came from. KEEP THE TWO IN STEP -- a change to
// PSRayDriven's own reconstruction that isn't mirrored here silently diverges the staged path from the
// single pass it exists to reproduce.
//
// `hitT`/`wpos` are the one part that cannot be copied verbatim: PSRayDriven reads hitT straight off
// its own RayQuery (q.CommittedRayT()), which no longer exists here. Instead this rebuilds the world
// hit point P by the SAME barycentric interpolation already used for the normal and UV above, projects
// (P - camera) onto `dir` to recover hitT, then forms wpos the same way PSRayDriven's own line does
// (gCamPos + dir * hitT) -- so a caller holding `wpos`/`hitT` cannot tell which path produced them.
RdSurface rdSurfaceFromRecord(uint4 rec, float3 dir) {
    RdSurface o;
    o.inst = gRtInstances[rec.x];
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

    // World hit point P, from the SAME three vertices already fetched above, transformed to world
    // space the way this file's own gWorld transforms (voxi.hlsl, VSMain) transform i.pos, then
    // blended by the SAME barycentric weights `w` -- see this function's own header for why hitT/wpos
    // derive from P rather than a stored ray distance.
    const float3 p0 = mul(float4(gRtVerts[o.i0].pos, 1.0), o.inst.objectToWorld).xyz;
    const float3 p1 = mul(float4(gRtVerts[o.i1].pos, 1.0), o.inst.objectToWorld).xyz;
    const float3 p2 = mul(float4(gRtVerts[o.i2].pos, 1.0), o.inst.objectToWorld).xyz;
    const float3 P  = p0 * o.w.x + p1 * o.w.y + p2 * o.w.z;
    o.hitT = dot(P - gCamPos.xyz, dir);
    o.wpos = gCamPos.xyz + dir * o.hitT;
    return o;
}

// MILESTONE 3. The one number CSRdRefl needs out of PSRayDriven's own material block before it can
// even decide whether to trace a reflection ray: the hit's ROUGHNESS. TRANSCRIBED FROM PSRayDriven'S
// OWN MATERIAL BLOCK (this file, the AVER_RT_BINDLESS branch of "THE STOCK MATERIAL AT A RAY HIT",
// further down) -- uvS/averRtUvGrad/the slot-1 metal-rough sample/the AVER_MAT_SLOPE_BLEND layer-1
// blend/the final clamp are copied verbatim, keeping only the statements roughness actually depends
// on. NOT a full rebuild of AverSurface: mapBase, occlusion, emissive and the normal map are read by
// PSMainVoxi/PSRayDriven for shading but never feed `s.rough`, so a caller that only wants the gate
// value (this one) has nothing to do with them -- and CSRdRefl never reads them back, so computing
// them here would be dead work every one of this pass's pixels pays for nothing.
//
// KEEP IN STEP WITH PSRayDriven's OWN COPY BY HAND -- the same rule rdSurfaceFromRecord's own header
// states just above, and for the same reason: no shared statement, so a change to one does not update
// the other. A future edit to PSRayDriven's roughness computation (a new map, a different clamp) that
// isn't mirrored here silently gives CSRdRefl's gate and rtReflectionTemporal call a stale roughness
// while the shade pass's own s.rough (still computed there in full, and still what the cone/sky
// fallback below uses) moves on without it.
//
// WHY THIS CAN'T JUST RE-CHECK `rough <= 0.75` A SECOND TIME AT STAGE B INSTEAD OF READING gRdReflTex's
// alpha: Stage B has no cheap way to know CSRdRefl's own roughness without redoing this same
// reconstruction itself, which is exactly the register-pressure cost splitting the reflection out of
// the shade pass exists to remove (see CSRdRefl's own header). Reading gRdReflTex[pixel].a instead asks
// the stage that already paid for this answer, once.
float rdSurfaceRoughness(RdSurface s, float3 rdRayDx, float3 rdRayDy) {
#ifdef AVER_RT_BINDLESS
    // THE EFFECTIVE UV and its footprint, same two calls PSRayDriven's own copy makes -- see that
    // block's own comments for why averRtSurfaceUV/averRtUvGrad are the right pair and why the
    // gradient rides the shadow-ray footprint rather than a screen-space derivative (undefined on a
    // ray hit).
    const float2 uvS = averRtSurfaceUV(s.mat, s.inst, s.wpos, s.N, s.hitUV);
    float2 uvGx, uvGy;
    averRtUvGrad(s.mat, s.inst, s.N,
                 gRtVerts[s.i0].pos, gRtVerts[s.i1].pos, gRtVerts[s.i2].pos,
                 gRtVerts[s.i0].uv,  gRtVerts[s.i1].uv,  gRtVerts[s.i2].uv,
                 rdRayDx, rdRayDy, uvGx, uvGy);

    // glTF packs roughness in G, metallic in B -- the same unpack PSRayDriven's own copy does. Only
    // slot 1 (MetalRough) is sampled: slots 0/2/3/4 (base colour, normal, occlusion, emissive) feed
    // shading channels this function has no use for.
    const float4 mapMR      = averRtSampleSlot(s.mat, 1, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float2       metalRough = float2(mapMR.g, mapMR.b);

    // THE SECOND LAYER, blended by SLOPE off the GEOMETRIC normal -- same predicate and lerp weight as
    // PSRayDriven's own copy. Only the texIndex[6] (Layer1MetalRough) branch is transcribed: the
    // sibling texIndex[5]/texIndex[7] branches blend mapBase/normalTS, neither of which this function
    // returns.
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

// How far a view ray travels INSIDE a volume before something stops it, in centimetres --
// averVolumeTransmittance needs a path length, and a blended surface has no idea how thick it is.
// The old fluid shader guessed with `depthCm / max(abs(V.z), 0.15)` from a CONSTANT floor height
// (FluidShaders.hpp), right only on a box's TOP face; on a side face at pitch 0 the clamp gave an
// ~8.7m path, so a pool's side came out fully opaque and bright (the PTTest pit's blown-out white
// slab). A ray doesn't have to guess; it measures.
//
// COMMITS EVERY CANDIDATE EXCEPT A FAILED CUTOUT, which is what FORCE_OPAQUE used to do here and
// why it stood: translucent instances sit as FORCE_NON_OPAQUE, so ordinary traversal would stop AT
// a candidate uncommitted. averRtProceedSolid commits any non-alpha-masked candidate on sight, so
// this ray still answers "what is the first thing along it, of any kind" -- with the one refinement
// that a hole in a cutout material is no longer a thing.
//
// BOTH LANES, because both can end the path: the volume's own BACK FACE, or an opaque object inside
// it (a rock, the pool floor). Nearest-of-the-two makes absorption respond to real geometry instead
// of an authored box height.
//
// Returns 0 when nothing is hit; averVolumeTransmittance reads that as "no path through the medium"
// and answers with full transmission -- an unbounded volume should absorb nothing until given a
// boundary, not absorb infinitely.
// Is a real backdrop bound? A null-filled Texture2D reports zero dimensions -- the only signal
// available in-shader, avoiding a spare cbuffer component for something the descriptor already
// tells us. Tested, not assumed: forcing the null case (MSAA on) and confirming glass falls back
// instead of going black is part of this feature's verification.
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
// THE LAST TERM IS THE TRICK: (T-1) is negative, subtracting exactly the light the medium absorbed
// per channel -- what one blend alpha can't express. Hardware still adds the REAL destination after.
// T==1 makes the correction exactly zero (bit-for-bit identity, what the no-absorption regression
// check leans on). STACKED TRANSLUCENCY DEGRADES GENTLY: `bg` is the scene copied BEFORE any
// translucent draw (stale for a second layer, used only in the correction, base composite still
// blends against the true `dst`) -- replacing the background outright would let a nearer pane erase
// a farther one, and glass over water makes that not hypothetical.
// Falls back to the scalar composite when no backdrop is bound -- the honest answer, not black.
// WHERE THE BACKGROUND IS READ FROM, once the surface bends it. Absorption decides what COLOUR
// survives the medium; refraction decides where it comes FROM -- gIor's first reader (uploaded and
// unread since it was added). Returns the UV to sample the backdrop at; gGiParams.y is the mode,
// .z strength, .w edge fade (see Settings::refractionMode).
float2 averRefractedBackdropUV(AverSurface s, float3 wpos, float thicknessCm,
                               float2 invSize, float2 screenPos, out bool tir) {
    tir = false;
    const float2 uv0  = screenPos * invSize;
    const uint   mode = (uint)(gGiParams.y + 0.5);
    if (mode == 0u || gGiParams.z <= 0.0) return uv0;

    // WHICH WAY THE LIGHT IS CROSSING decides everything below. eta = index LEFT / index ENTERED:
    // air->medium is 1/n, medium->air is n -- reversed, TIR becomes unreachable (needs eta > 1).
    // GATED ON gCameraMedium, NOT s.backFace -- already cost this engine once. material_prelude.hlsl's
    // post-mortem: a TIR override gated on backFace "turned every pane of glass into a dark slab at
    // 41 degrees off normal", since backFace is also true for a two-sided pane's FAR surface seen
    // from outside (where Snell forbids TIR), indistinguishable from a pixel shader at the time.
    // gCameraMedium.x is now computed CPU-side against the volume's bounds, which a pixel can't
    // recover; glass is twosided=1 and never a medium by that test, so it can't reach this branch.
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

    // Where the bent ray leaves the medium: one thickness along the BENT path, not the straight one
    // -- why this needs ray-measured thickness rather than an authored constant.
    float3 target = wpos + R * max(thicknessCm, 0.0);

#if AVER_RT
    // RAY-TRACED: follow the bent ray to what it ACTUALLY reaches and project THAT, unlike the
    // screen-space mode below which can only offset within the image already captured. Still reads
    // colour from the backdrop rather than shading the hit (a second full material eval on the
    // frame's most expensive pass) -- an off-screen/occluded hit falls back to whatever's there.
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

    // NDC -> THE VIEWPORT RECT, NOT [0,1] OF THE WHOLE TARGET -- the bug that made refraction look
    // like a wrecked image, not a bent one. The editor docks the 3D view in a SUB-RECT, so NDC maps
    // to gSceneViewportCur while uv0 (screenPos*invSize) is a full-target UV; plain ndc*0.5+0.5 mixes
    // the spaces, an affine error that walks the pane's image off-screen near the right edge -- the
    // black block in the glass rail.
    // MEASURED by forcing target = wpos (answer HAD to be uv0 exactly): 100% of the rail's pixels
    // still landed >60px away, proving this was the projection, not refraction (an 8cm pane can't
    // bend 60px). The file already knew: rtReprojectHistory and three other sites do this conversion
    // correctly, one warning the plain form "lands every reprojection on the wrong texel" --
    // refraction was written later and missed it.
    //
    // A ZERO-WIDTH RECT means no viewport was reported this frame -- sample straight through, the
    // same fallback used for TIR and a target behind the eye.
    if (gSceneViewportCur.z <= 0.0 || gSceneViewportCur.w <= 0.0) return uv0;
    const float2 pxR = gSceneViewportCur.xy +
                       float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewportCur.zw;
    float2 uvR = pxR * invSize;

    // THE EDGE FADE, not cosmetic: an offset walking off-screen samples nothing meaningful, and one
    // onto a foreground object shows that object through the glass. Fades to zero near the border
    // instead of a hard wrong pixel.
    // FADED AGAINST THE VIEWPORT RECT, NOT THE TARGET: outside the 3D view the backdrop holds
    // whatever the rest of the frame is -- "still on the part the camera drew" is the real test.
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

float4 averBlendedOutputBackdrop(AverSurface s, float3 diffuse, float3 specular, float3 T,
                                 float2 screenPos, float3 wpos, float thicknessCm) {
    float2 invSize;
    if (!averBlendBackdropValid(invSize))
        return averBlendedOutputVolume(s, diffuse, specular, T);

    // TWO SAMPLES, AND THE SECOND IS WHAT MAKES THE CORRECTION CANCEL. The HARDWARE adds
    // dst*(1-alpha) after this returns; a correction only works if it subtracts EXACTLY that, at uv0.
    //
    // The single-sample version subtracted the REFRACTED sample instead, so nothing cancelled and the
    // residue (dst - bgRefracted) went NEGATIVE wherever the bent ray landed on something BRIGHTER
    // than what's really behind -- a negative channel through the tonemap reads as a hue, not dark.
    // That's what the magenta blocks in the pool were. MEASURED: 41773 magenta pixels at the
    // 45-degree pool camera with refraction on, 0 with --refraction 0, 0 after this fix.
    //
    // The algebra, with a == alpha:
    //     final = specular + diffuse*a + (bgRefr*T - bgStraight)*(1-a) + dst*(1-a)
    //   and dst IS bgStraight, so the last two collapse and leave
    //     final = specular + diffuse*a + bgRefr*T*(1-a)
    //   the bent background, absorbed over the path, behind a Fresnel-weighted surface. Refraction
    //   OFF makes bgRefr == bgStraight, collapsing to bg*(T-1)*(1-a) (bit-identical to before); T==1
    //   leaves (bgRefr - bgStraight)*(1-a) -- pure bending, so refraction with no volume isn't
    //   silently wrong any more.
    //
    // dst == bgStraight only for the FIRST translucent surface over a pixel; a second one behind
    // glass composites against a backdrop not yet containing the first -- the known cost of capturing
    // the backdrop once per frame. Bounded to that overlap, unlike the misregistration above.
    bool tir = false;
    const float2 uvR   = averRefractedBackdropUV(s, wpos, thicknessCm, invSize, screenPos, tir);
    const float2 uv0   = screenPos * invSize;
    const float3 bgR   = gBlendBackdrop.SampleLevel(gMaterialSampler, uvR, 0).rgb;
    const float3 bg0   = gBlendBackdrop.SampleLevel(gMaterialSampler, uv0, 0).rgb;
    const float  alpha = saturate(s.alpha);

    // TIR IS NOT A WINDOW WITH A DIFFERENT UV -- treating it as one made it black. Past the critical
    // angle nothing transmits, so everything the eye gets is REFLECTED; the window form's (1-alpha)
    // background weight, with alpha pushed toward 1 by view Fresnel at TIR's grazing angles, was
    // multiplying the mirror by roughly zero. MEASURED underwater at 25 degrees: mirrored region
    // read 0.59 mean against the 18.8 of the pit wall it should show -- dark because cancelled, not
    // because the pool is dark.
    //
    // alpha=1 is correct HERE, unlike material_prelude.hlsl's post-mortem case: that slammed alpha
    // to 1 on a PANE seen from outside (where Snell forbids TIR) with no reflected image, leaving a
    // dark slab. This fires only when gCameraMedium says the eye is inside a single-sided volume,
    // handing back the reflected scene as the surface's radiance.
    if (tir) return float4(specular + bgR * T, 1.0);

    return float4(specular + diffuse * alpha + (bgR * T - bg0) * (1.0 - alpha), alpha);
}

float averVolumeThickness(float3 wpos, float3 N, float3 viewDir) {
    RayDesc r;
    // SAME bias as the reflection ray, same reason: starting exactly on the shaded surface re-hits
    // it at t~0. Pushed along the VIEW direction, not N: this ray heads INTO the surface, and
    // offsetting along the normal would push it out of the volume being measured.
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + viewDir * bias;
    r.Direction = viewDir;
    r.TMin      = 0.0;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // BOTH LANES, and every candidate committed on sight EXCEPT a failed cutout: this measures the
    // distance to the far side of a medium, so a pane is a real boundary here. averRtProceedSolid
    // commits any non-alpha-masked candidate, which preserves exactly what FORCE_OPAQUE did.
    q.TraceRayInline(gScene, RAY_FLAG_NONE,
                     AVER_RT_MASK_OPAQUE_ALL | AVER_RT_MASK_TRANSLUCENT, r);
    averRtProceedSolid(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;
    return q.CommittedRayT() + bias;
}

// Reprojects wpos through LAST frame's camera to sample the reflection history. False when unusable:
// off-screen, behind last frame's near plane, a recorded miss (stored.a <= 0), or a disocclusion --
// same test as rtReprojectHistory, against the reflection's own depth channel.
// A MISS IS NEVER REPROJECTED, the one real difference from the shadow case: sky-by-direction is
// cheap and highly VIEW dependent, so a stale miss would show the wrong patch of sky through a still
// surface. A real hit depends on the surface and a mostly-static light, not viewing angle.
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
    if (stored.a <= 0.0) return false;   // a miss was recorded there -- nothing to reuse
    const float tol = max(clip.w, stored.a) * 0.03 + 1.0;
    if (abs(clip.w - stored.a) > tol) return false;

    hist = stored.rgb;
    velocityPx = px - pixel;
    return true;
}

// The SPATIAL denoiser for REFLECTIONS -- there was none before: reprojected/blended in time but
// never filtered in space, harmless while the ray was a deterministic mirror but not once
// rtReflection widens into a lobe (shipping the lobe alone trades wrong-but-clean for right-but-noisy).
//
// STRUCTURE IS rtShadowSpatial's (reprojected gather centre, plane-distance rejection -- read that
// first). Differs in three ways:
//  1. RADIUS FROM ROUGHNESS, not a host constant: a fixed width either blurs a mirror or
//     under-filters a rough surface. Roughness 0 returns centre untouched, no texel loaded.
//  2. A MISSED NEIGHBOUR IS SKIPPED, not counted black: gRtReflHist's miss sentinel is a NEGATIVE
//     alpha, and averaging it in would drag every reflected edge toward black.
//  3. NO LUMINANCE WEIGHT: SVGF's colour-similarity term needs a per-pixel VARIANCE estimate this
//     engine doesn't track (history is (rgb, depth), no second moment); without variance a luminance
//     weight preserves exactly the noise it's meant to remove. Geometry weights only, for now.
// The AVER_GBUFFER_HISTORY crease term in the tap loop is copied from rtShadowSpatial verbatim.
// `N` is a new parameter for that crease term only -- rtReflectionTemporal's own normal, unchanged.
float3 rtReflectionSpatial(float3 centre, float3 wpos, float3 N, float2 pixel, float curDepth,
                           float rough, float dzdx, float dzdy) {
    // Radius tracks the lobe (tan(cone)=rough^2 grows quadratically, this grows linearly --
    // deliberately conservative: too wide smears detail, too narrow just leaves noise for the
    // temporal history). Capped at 3 (7x7 gather): cost is quadratic in radius.
    const int radius = (int)clamp(floor(rough * 6.0), 0.0, 3.0);
    if (radius <= 0 || gRtHistParams.y < 0.75) return centre;

    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);

    // Gather around where this pixel WAS last frame (gRtReflHist is last frame's). Same arithmetic
    // as rtReprojectHistory/rtShadowSpatial, including their two landmines: last frame's VIEWPORT
    // rect not [0,1], and floor not round.
    float2 centrePx = pixel;
    const float4 pclip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (pclip.w > 1e-4) {
        const float3 pndc = pclip.xyz / pclip.w;
        if (pndc.z >= 0.0 && pndc.z <= 1.0)
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
    }
    const int2 base = int2(floor(centrePx));

    // A GAUSSIAN falloff, where rtShadowSpatial uses a flat box: box filters ring in frequency
    // response, visible as square-edged plateaus around a highlight. sigma = radius/2 keeps the
    // kernel's support near the requested radius, widening the blur smoothly.
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
            if (st.a <= 0.0) continue;   // that neighbour's ray missed; see (2) above
            const float predicted = curDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.a - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // THE CREASE TERM -- copied from rtShadowSpatial's identical block (see there for the
            // reasoning and cos(60 deg)). ASSUMES gGBufNormalHist matches gRtReflHist's resolution,
            // same assumption made at gRtShadowHist -- both histories share the scene render target.
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

// Tiled/temporal wrapper around rtReflection(), mirroring rtShadowTemporal's structure/tile schedule
// so shadow and reflection rays amortise on the same cadence. Called ONLY after the caller gates on
// roughness (s.rough <= 0.5 in PSMainVoxi) -- not re-checked here.
// `hit`: true for a real reflection colour (fresh or reused from history; haveHist is already
// conditioned on a real hit, never a miss), false when the caller should fall back to sky.
//
// SUB-STAGE SPLIT C (Settings::rayDrivenReflSplit): `doSpatial` lets CSRdRefl (Register, R1) call this
// for the ray/history half alone and leave rtReflectionSpatial's dense 7x7 history gather to
// CSRdReflFilter (Filter, R2) in its own pass -- the register-heavy trace and the bandwidth-heavy
// filter no longer share one thread. false ONLY from CSRdRefl's own AVER_RD_REFL_SPLIT branch; every
// other caller goes through the rtReflectionTemporal() wrapper just below, which always asks for both.
float3 rtReflectionTemporalEx(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, float rough,
                              float dzdx, float dzdy, bool doSpatial, out bool hit) {
    // ---- THE MIRROR CUTOFF: ONE predicate, three consumers ----
    // Below AVER_REFL_MIRROR_ROUGH a surface is a mirror throughout: no jitter, no temporal history,
    // no spatial filter. tan(cone)=rough^2, so at 0.1 the ray is displaced one part in a hundred of
    // its own length -- under a pixel, with no variance for a filter to remove.
    //
    // A CORRECTNESS FIX, NOT A TUNING KNOB: the first version gated the temporal blend on
    // `rough > 0.0` while claiming a smooth surface "still takes the fresh value outright" -- those
    // disagree for every near-mirror (glass at 0.05), both getting an 85%-history blend that can't
    // reduce a variance already at zero and can only add lag, smearing glass behind a moving camera.
    //
    // Deriving all three behaviours from ONE value is the point: a jittered-but-unfiltered lobe is
    // noise, a filtered-but-unjittered ray is blur -- they must agree, which only works reading the
    // same number.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // NO HISTORY TEXTURE: the lobe stays closed too -- widening it with nothing to converge into
    // would trade a biased-but-stable reflection for one that flickers every frame. rough=0 reduces
    // rtReflection to the exact mirror ray it traced before this change.
    if (gRtHistParams.x < 0.5) return rtReflection(wpos, N, R, L, pixel, 0.0, 0.0, hit);

    const float4 curClip = mul(float4(wpos, 1.0), gViewProj);
    const uint frameIdx  = (uint)gRtHistParams.z;
    // Per-frame rotation turning one ray per frame into a converging lobe estimate. A pure per-frame
    // count, never wall-clock, so a capture at frame N is reproducible; the golden-angle multiplier
    // keeps successive rotations from landing near each other the way a fixed increment would.
    const float frameJitter = (float)frameIdx * 2.39996323;
    const uint tileBits = (uint)gRtHistParams.w;

    if (tileBits == 0u) {
        // THE SHIPPED PATH: rtPixelsPerRayTileForQuality returns 1 at every tier, so this always
        // runs. Used to trace/write/return with NO temporal blend -- correct for a deterministic
        // mirror, wrong once the lobe opened. Blend added HERE, only where variance was introduced:
        // rough=0 still takes the fresh value outright, unchanged for glass, chrome and water.

        // ---- T3 (Settings::rtReflectionHalfRate, console voxi.rtReflectionHalfRate) -- DECIDED
        // BEFORE THE TRACE, the same shape as T2's own comment (voxi_rt.hlsli's
        // rtSkyOcclusionTemporal) ----
        //
        // Gated on lobeRough > 0.0 up front: a MIRROR (lobeRough == 0, tanCone == 0 inside
        // rtReflection) always retraces, because a reprojected mirror reflection is wrong the instant
        // the camera moves -- there is no lobe variance for a stand-in history to be trading against,
        // only a wrong answer.
        //
        // THE REPROJECTION CALL BELOW IS GATED ON THE BIT, UNLIKE T2's: T2 hoisted its own
        // rtReprojectAo call unconditionally because that function already ran every frame regardless
        // of whether the trace hit anything. This one does not -- today it only runs AFTER a
        // successful trace (`if (lobeRough > 0.0 && curHit)`, in the `else` branch below), so hoisting
        // it here unconditionally would add a reprojection lookup to every rough pixel whether or not
        // this bit is even set. Gating it behind the bit keeps this branch's cost identical to today's
        // whenever the bit is clear, at the price of one possible SECOND rtReprojectReflection call
        // below on the rare pixel whose tile is skip-eligible but whose history has just gone invalid
        // (a fresh disocclusion): a texture lookup, not a ray, and the non-skip path already pays for
        // one of these every frame it runs anyway.
        //
        // Tile math is T2's, verbatim: an 8x8, viewport-relative tile (one staged compute thread
        // group), parity alternating by gRtHistParams.z so a tile that skips this frame traces next.
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
            // Exactly what the tiled branch's own "not my turn" case (below, the tileBits != 0u
            // path) already does: reuse the reprojected history outright, no ray this frame. The
            // unchanged history write and spatial filter, further down, take it from here.
            col    = skipCol;
            curHit = true;
        } else {
            float3 fresh = rtReflection(wpos, N, R, L, pixel, lobeRough, frameJitter, curHit);
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
        // write. Feeding a filtered value back makes the spatial pass a compounding IIR filter,
        // the reflection slowly dissolving.
        //
        // W6/M5: GATED ON gAverHistoryWrite, ON BY DEFAULT (D3 decision) -- see this file's own
        // gAverHistoryWrite/averDrawIsTranslucent comment, above the #if AVER_RT region this function
        // lives in, for the flag's definition, and PSMainVoxi below for where a blended fragment
        // clears it. Before this gate, a blended (glass/water) replay fragment overwrote this texel
        // with the PANE's own reflection, the same class of double write this task's C9 finding
        // names for the RT shadow/AO histories (voxi_rt.hlsli) and the ReSTIR GI histories
        // (voxi_restir.hlsli). voxi.legacyBlendedHistoryWrite (gAmbientParams.z bit 32) restores the
        // old unconditional write, byte-identical, for A/B.
        //
        // CLAMPED BEFORE IT IS STORED, not only where the caller composes the answer. rtReflection
        // returns reflAlbedo * (direct + ambient) with nothing bounding it (PSRayDriven's own "ONLY
        // UNBOUNDED TERM" comment), and before this line the raw value went into the history: next
        // frame's reprojection takes it back at 85% weight and rtReflectionSpatial gathers it into a
        // 7x7 neighbourhood, so one runaway ray -- a spike, a negative ambient undershoot, a NaN --
        // stayed on screen for many frames and spread. clamp(), not min(), for the reason the callers'
        // own clamp gives (a negative radiance is a recorded incident class here; clamp also maps NaN
        // to a bound on this hardware). The spatial centre below uses the same bounded value, so the
        // split CSRdReflFilter (which reads this texel back) and the unsplit path agree.
        col = clamp(col, 0.0, AVER_VOX_MAXRAD);
        if (gAverHistoryWrite)
            gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
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
        col = rtReflection(wpos, N, R, L, pixel, lobeRough, frameJitter, curHit);
        // Same adaptive weight as rtShadowTemporal. Only blends a HIT with history: a fresh miss
        // stays a miss (caller's sky fallback handles it) rather than dragged toward a stale colour.
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
    // Raw, not filtered -- see the untiled branch's own note on why feeding the spatial result back
    // into the history makes it a compounding filter.
    //
    // W6/M5: same gate, same reason, as the untiled branch's own copy of this write above -- and the
    // same clamp before it, for the same reason.
    col = clamp(col, 0.0, AVER_VOX_MAXRAD);
    if (gAverHistoryWrite)
        gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
    if (curHit && doSpatial) {
        return rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy);
    } else {
        return col;
    }
}

// The wrapper every caller but CSRdRefl's own split branch uses -- always asks for the spatial gather,
// so PSRayDriven's single pass, PSMainVoxi and CSRdRefl's own unsplit path stay byte-for-byte the same
// call they made before rtReflectionTemporalEx existed.
float3 rtReflectionTemporal(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, float rough,
                            float dzdx, float dzdy, out bool hit) {
    return rtReflectionTemporalEx(wpos, N, R, L, pixel, rough, dzdx, dzdy, true, hit);
}

// THE "NO DATA" SENTINEL FOR A PIXEL THE REFLECTION GATE ROUTED AWAY FROM TRACING THIS FRAME.
// Every caller traces only where `rough <= 0.75` (and RT reflections are on), and rtReflectionTemporalEx
// is the only writer of gRtReflHistOut -- so a pixel that failed the gate used to leave its texel
// holding whatever it last traced, however many frames ago. Roughness is resampled every frame from a
// footprint-chosen mip (and can be animated), so a pixel can flip across 0.75. When it requalified,
// rtReprojectReflection found a positive depth that passed its tolerance, and blended that old colour
// in at up to 85% weight as though it were last frame's. The same miss sentinel rtReflectionTemporalEx
// already writes makes both the reprojection and rtReflectionSpatial's neighbour gather skip it.
// Guarded exactly as that function's own writes are: only while the history pair is bound, and only
// for a fragment allowed to write history (PSMainVoxi's blended replay is not).
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
// shadowFactor: that asks "is this PIXEL in shadow" by camera distance, correct on-screen but wrong
// for a voxel, which exists wherever the (camera-independent) GI volume is. Feeding voxels through
// the cascades forced fitCascades to union its last cascade with the whole GI volume (~14x the area,
// every draw). One box over the volume answers directly: no cascade, no camera distance, no fade.
float giShadowFactor(float3 wpos, float3 N, float ndl) {
    // Unusable map: fully lit. The volume is still injected, just without sun occlusion -- the same
    // degradation shadowFactor performs when the atlas is missing, and the reason giShadowTex_ is a
    // soft dependency rather than an init() failure.
    if (gGiShadowParams.y < 0.5) return 1.0;

    float slope = saturate(1.0 - ndl);
    float3 p0 = wpos + N * (gGiShadowParams.z * (1.0 + slope));

    float4 lp = mul(float4(p0, 1.0), gGiShadowViewProj);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    // OUTSIDE THE BOX IS LIT, NOT SHADOWED. The box covers the whole GI volume by construction, so
    // landing outside it means the voxel is outside the volume too and its radiance is never read.
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return 1.0;

    // No atlas quadrant to inset into: this map is one box filling the whole texture.
    float t = gGiShadowParams.x;
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gGiShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * t, p.z);
    return s / 9.0;
}

// Per-dispatch constants for CSClear/CSResolve/CSMip (b3: b0/b1 are taken by the graphics root
// signature). 32 B total, packed gSrcMip@0, gBoxLo@4 (12 B), gBoxHi@16 (12 B), _boxPad@28 -- mirrored
// byte-for-byte by aver::voxi::GiDispatchConstants (GiDispatchBounds.hpp), which static_asserts its own
// size against this layout so the two cannot drift apart silently. gSrcMip is CSMip's source-level
// index, unused by CSClear/CSResolve (always mip 0). gBoxLo/gBoxHi is a half-open voxel-space box
// [gBoxLo,gBoxHi) in the DESTINATION mip's own coordinate space; the C++ caller (VoxiRenderer, not this
// file) computes and uploads the box for whichever level a given dispatch targets before every one of
// the three kernels' Dispatch calls -- see aver-voxi-cbuffer-three-mirrors.md and the W3 spec for why
// this lives in its own cbuffer rather than growing VoxiFrame.
cbuffer MipCB : register(b3) { uint gSrcMip; uint3 gBoxLo; uint3 gBoxHi; uint _boxPad; };

#include "voxi_cone.hlsli"

// ---- CSAirVis: bakes gAirVisOut from gVoxelTex's own occupancy (see gAirVis/gAirVisOut's own header
// comment, above, for what this volume is and why it exists) ----
//
// PLACED HERE, AFTER voxi_cone.hlsli, on purpose: it reuses voxelUVW/insideVolume rather than
// reimplementing world<->volume-space conversion a third time, so it must sit textually after that
// file's #include the same way this file's own PSMainVoxi/PSRayDriven do. UNCONDITIONAL -- not inside
// #if AVER_RT -- because the air-vis volume backs cone-traced GI (PSMainVoxi, no ray tracing needed)
// just as much as ray-driven shading, and gVoxelTex/gVoxelSamp/cbuffer VoxiFrame it reads are all
// already unconditional themselves (declared above the first #if AVER_RT guard opens).
//
// COMPILED WITH THE FULL giLayout(), the SAME descriptor table and VoxiFrame (b4) binding as
// CSRdVisibility/CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl (further down this file) -- NOT the tiny MipCB
// (b3) layout CSClear/CSResolve/CSMip use (just above), which has no VoxiFrame to read gVoxelOrigin/
// gVoxelParams from. (VoxiRenderer.cpp's own dispatch chooses that root signature; this file has no
// syntax for "layout," only the resource declarations both layouts happen to share.)
//
// SCHEDULED AS A ROUND-ROBIN, NOT PER VOXEL REBUILD: every frame VoxiRenderer::prePass dispatches ONE
// slab of z-layers, the half-open cell box [gBoxLo, gBoxHi) in MipCB (b3, the same root-constant block
// the three voxel kernels above read, declared by this pipeline's layout too), so the whole volume
// refreshes every few frames at a flat, small cost. A full box is dispatched once when the volume is
// created. MEASURED, the reason: a full 48^3 pass cost ~10 ms, and the voxel volume rebuilds often
// under camera motion, while sky visibility only changes when geometry does. Nothing is accumulated
// across frames -- a cell is simply recomputed from the current voxels -- so a still scene writes the
// same value every refresh and nothing can flicker.
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

    // One voxel of the GI RADIANCE volume, world units -- what CSAirVis is marching THROUGH, and a
    // different grid from the fixed-48 one it is WRITING (gVoxelParams.x is the GI volume's own,
    // tier-dependent resolution; see QualityLadder.hpp). Same formula traceCone uses (voxi_cone.hlsli).
    const float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);

    // Half-angle from the direction count, same "tile the hemisphere with N cones" derivation
    // coneTracedIndirect uses (voxi_cone.hlsli): 2*pi(1-cosHalf) = 2*pi/N covers the hemisphere.
    const float cosHalf = saturate(1.0 - 1.0 / (float)AVER_AIRVIS_DIRS);
    const float halfAngleTan = sqrt(max(1.0 - cosHalf * cosHalf, 1e-6)) / max(cosHalf, 1e-6);
    const float sinMinElev = sin(radians(AVER_AIRVIS_MIN_ELEV_DEG));

    float Tsum = 0.0;
    // A DETERMINISTIC FIBONACCI HEMISPHERE, not a random/blue-noise set: z stratified evenly over
    // [sin(minElev), 1] (equal-area per step, since dz is proportional to solid angle) and azimuth
    // stepped by the golden angle (2.39996323, the SAME constant coneTracedIndirect's own ring uses)
    // so successive directions don't clump. No frame index anywhere in this function -- the volume is
    // rebaked, not re-jittered, so it carries no temporal signal to flash on camera motion.
    [loop] for (uint dirIdx = 0; dirIdx < AVER_AIRVIS_DIRS; ++dirIdx) {
        const float z = sinMinElev + (1.0 - sinMinElev) * ((float)dirIdx + 0.5) / (float)AVER_AIRVIS_DIRS;
        const float r = sqrt(saturate(1.0 - z * z));
        const float phi = 2.39996323 * (float)dirIdx;
        const float3 d = float3(r * cos(phi), r * sin(phi), z);

        // Widening cone through the volume, same shape as traceCone (voxi_cone.hlsli) but reading
        // OCCUPANCY (alpha, CSResolve's fragment-covered fraction, box-filtered into coarser mips by
        // CSMip) instead of radiance, and starting HALF a voxel out rather than traceCone's two --
        // this marches from a volume CELL CENTRE, not a lit surface point, so there is no coplanar
        // voxel to avoid self-hitting.
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
            // barely-there occupancy (CSMip's box filter dilutes it 8x per level) -- OCC_GAIN pushes a
            // thin-but-real occluder back toward fully blocking rather than letting mip climb erase it.
            T *= 1.0 - saturate(occupancy * AVER_AIRVIS_OCC_GAIN);
            dist += footprint;
        }
        Tsum += T;
    }

    gAirVisOut[id] = Tsum / (float)AVER_AIRVIS_DIRS;
}

// ---- SHADE-PASS READ: how much of the sky the air between the camera and wpos can actually see ----
//
// Called from voxi.hlsl's own PSMainVoxi/PSRayDriven fog call sites, further down this file, and
// nowhere in shared_prelude.hlsl -- water.hlsl, scene.hlsl and every other averApplyFog(Ex) caller
// keep passing airVis == 1.0 (that function's own wrapper, shared_prelude.hlsl) and stay unaffected.
//
// DETERMINISTIC, LIKE CSAirVis ABOVE: 8 FIXED points, no jitter, no per-pixel history -- see
// gAirVis/gAirVisOut's own header comment, further up this file, for why a temporally-accumulated
// per-pixel signal was tried here first (surface AO) and reverted for flashing open on camera motion.
float voxiAirVisibility(float3 wpos) {
    // Voxel GI off, or the volume doesn't exist yet: today's behaviour, unoccluded air. Matches the
    // `gVoxelParams.w > 0.5` idiom this file already uses everywhere else for the same question.
    if (gVoxelParams.w <= 0.5) return 1.0;
    uint dimX, dimY, dimZ;
    gAirVis.GetDimensions(dimX, dimY, dimZ);
    // The placeholder is 1x1x1 and cubic, so one axis is enough to tell it apart from a real volume.
    if (dimX <= 1u) return 1.0;

    const float3 camPos = gCamPos.xyz;
    const float segLen = length(wpos - camPos);
    if (segLen <= 1e-4) return 1.0;
    const float3 dir = (wpos - camPos) / segLen;

    // The sampled sub-segment STARTS at the fog's own start distance, not at the camera: fog before
    // that distance contributes nothing at all (averFogFactor's `len <= start` early-out,
    // shared_prelude.hlsl), so a sample there would pull the average toward the near-camera answer
    // (usually open) for a surface that is entirely inside the fogged range.
    const float start = min(gFogParams.z, segLen);
    const float span = segLen - start;

    // Height-fog density weighting, mirroring averFogFactor's own exponential (shared_prelude.hlsl):
    // a sample deep under the fog should outvote one near its ceiling, or the volume's answer at the
    // top of a tall atrium would wash out the occluded answer down at street level.
    const float fogK = gFogParams.x;
    const float fogHeight = gFogParams.y;

    float wSum = 0.0, vSum = 0.0;
    [unroll] for (uint i = 0; i < 8u; ++i) {
        const float t = start + (((float)i + 0.5) / 8.0) * span;
        const float3 p = camPos + dir * t;
        const float3 uvw = voxelUVW(p);
        // Outside the volume counts as open sky, same rule CSAirVis's own march uses.
        const float v = insideVolume(uvw) ? gAirVis.SampleLevel(gVoxelSamp, uvw, 0.0) : 1.0;
        const float w = fogK <= 1e-8 ? 1.0 : exp(-(p.z - fogHeight) * fogK);
        wSum += w;
        vSum += w * v;
    }
    return wSum > 1e-6 ? vSum / wSum : 1.0;
}

// ================= additive G-buffer: velocity, view-space depth, normal+roughness =================
// Gated on AVER_GBUFFER, following AVER_RT's convention: a compile-time define (never a runtime
// branch), off by default, so PSMainVoxi's/PSRayDriven's ORIGINAL variants fall through unchanged --
// THE FEATURE IS ADDITIVE: with the define off the frame must be bit-identical, checked by the
// render gate oracle (18 gates x 9 configurations).
//
// WHY THIS EXISTS: nothing produces motion vectors and there's no G-buffer -- PSMainVoxi returns one
// SV_TARGET, normal/roughness/albedo living only in shader registers. That single gap blocks the
// vendored FidelityFX denoiser (third_party/fidelityfx-denoiser/README.md), FSR 2/3, TAA and
// screen-space reflections at once, and is WHY temporal reprojection is wrong for moving geometry:
// rtReprojectHistory and siblings transform THIS frame's wpos through LAST frame's camera, only
// valid for a surface that didn't move.
//
// THIS SLICE ONLY WRITES THE TARGETS -- wiring a consumer (FFX denoiser, FSR3, TAA) is later work,
// out of scope on purpose: unread-but-written is this codebase's recurring shape, deliberately here.
//
// ---- HOW TO DECODE EACH CHANNEL, so encode and decode don't drift apart ----
//   SV_TARGET1 velocity:        RG16F. Texels/frame, DESTINATION (this frame's) minus SOURCE (last
//                                frame's) texel: `prevPixel = thisPixel - velocity`, matching
//                                rhi::UpscalerNeeds::MotionVectors' documented convention.
//   SV_TARGET2 viewZ:           R32F. VIEW-SPACE LINEAR depth (clip.w), NOT the post-projective
//                                [0,1] SV_Position.z/SV_DEPTH a hardware depth buffer stores.
//   SV_TARGET3 normalRoughness: RGB10A2, packed to NRD's OWN encoding (NRD_NORMAL_ENCODING_
//                                R10G10B10A2_UNORM, third_party/nrd/Shaders/NRDConfig.hlsli), NOT a
//                                plain n*0.5+0.5-with-roughness-in-w scheme -- see
//                                averPackNormalRoughness below for the layout and why. w =
//                                materialID/3 in NRD's convention; this engine has no material-ID
//                                concept yet, so it is always 0, NOT roughness.
#if AVER_GBUFFER
struct GBufferOut {
    float4 col              : SV_TARGET0;   // exactly PSMainVoxi's own colour -- unchanged by this define
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
};

// Packs a world-space unit normal and roughness into the RGB10A2 convention above. Shared by
// PSMainVoxi and PSRayDriven (both its sky-hit and sky-miss branches, further down this same file)
// so the encode is written once, not risking divergence.
//
// THIS IS NRD'S NORMAL_ENCODING_R10G10B10A2_UNORM LAYOUT, TRANSCRIBED BYTE-EXACT FROM
// _NRD_EncodeNormalRoughness101010 (third_party/nrd/Shaders/NRD.hlsli) -- NOT the naive
// N*0.5+0.5-with-roughness-in-w scheme this function used to compute, which is NRD's #else layout
// for encodings 0/3 and is WRONG for the format this texture actually uses. Confined-vendoring
// terms keep NRD's headers out of modules/render.voxi, so the maths is transcribed here rather than
// included, the same pattern the YCoCg pair in voxi_restir.hlsli already follows; the matching
// decode is transcribed a second time in sandbox/shaders/gbuffer_debug.hlsl for the debug view, and
// a third time (for a CPU-side round-trip test, no GPU needed since it's pure arithmetic) in
// tests/render.nrd/src/NrdNormalRoughnessEncodingTest.cpp.
//
// THE LAYOUT: an improved-octahedral encode folds N into x/y (L1-normalize, then a fold that always
// lands both channels in [0,1] -- see the two lines below, or work the algebra: r.x = 0.5 +
// 0.5*(n.x+n.y) and |n.x+n.y| <= |n.x|+|n.y|+|n.z| = 1 after the L1-normalize, so r.x can never
// leave [0,1], and the same argument covers r.y). z carries roughness's MAGNITUDE with the SIGN OF
// n.z riding on z's own sign -- which is why roughness is clamped away from exactly 0 below: a zero
// magnitude has no sign to carry n.z's, and the decoder recovers that sign from `t < 0`.
float4 averPackNormalRoughness(float3 N, float roughness) {
    N /= abs(N.x) + abs(N.y) + abs(N.z);

    float3 r;
    r.y = N.y * 0.5 + 0.5;
    r.x = N.x * 0.5 + r.y;
    r.y -= N.x * 0.5;

    // Can't be exactly 0, or it erases n.z's sign bit -- NRD's own comment on the line this
    // transcribes, and the reason a caller passing an unclamped/zero roughness still gets a
    // decodable normal back.
    roughness = max(saturate(roughness), 1.5 / 512.0);
    const float s = N.z < 0.0 ? -roughness : roughness;
    r.z = s * 0.5 + 0.5;

    // w: NRD's materialID/3 slot. This engine has no material-ID concept, so always 0 -- see the
    // G-buffer's own header comment above and RHI.hpp's gBufferNormalRoughnessTexture for why a
    // reader must not mistake this for roughness.
    return float4(r, 0.0);
}

// Screen-space motion for the velocity channel: `wpos` reprojected through THIS frame's camera minus
// the SAME wpos through LAST frame's, in the SCENE VIEWPORT RECT (same landmine as
// rtReprojectHistory: the editor docks the 3D view in a sub-rect, so plain ndc*0.5+0.5 is wrong).
//
// STATIC-GEOMETRY ONLY, A STATED DELIBERATE GAP: using `wpos` in both projections is correct only if
// the surface didn't move. A moving instance needs its OWN previous-frame transform (previous
// `gWorld` for raster, previous `RtInstance.objectToWorld` for ray-driven), and NEITHER EXISTS YET.
// Consequence: correct motion for a stationary object under a moving camera, ZERO motion for an
// object that is itself animating -- SILENTLY. Fix: thread a previous-transform through RtInstance
// (or the per-draw cbuffer) and reproject through it instead of `wpos` twice.
float2 averGBufferVelocity(float3 wpos) {
    const float4 curClip  = mul(float4(wpos, 1.0), gViewProj);
    const float4 prevClip = mul(float4(wpos, 1.0), gPrevViewProj);
    // Either transform can put this point behind its own near plane -- prevClip routinely does (first
    // frame, or anything that just entered the frustum). Zero is "no motion known", same fallback as
    // rtReprojectHistory's velocityPx -- the least wrong answer when the maths is undefined, rather
    // than an Inf/NaN divide.
    if (curClip.w <= 1e-4 || prevClip.w <= 1e-4) return float2(0.0, 0.0);

    const float2 curNdc  = curClip.xy  / curClip.w;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    // B2 (F6): THIS frame's clip position (curClip, above, built from gViewProj) has to land in THIS
    // frame's viewport rect, not last frame's -- the cbuffer's own field comments pair gViewProj with
    // gSceneViewportCur and gPrevViewProj with gSceneViewport (see the VoxiFrame struct at the top of
    // this file), and the editor can resize/redock the 3D view between frames, so the two rects are
    // not interchangeable even when they happen to agree most frames. THIS WAS WRONG: curPx used to
    // map through gSceneViewport (last frame's rect) exactly like prevPx does, four lines below --
    // paired with the CURRENT clip position it should never have shared prevPx's rect at all. Falls
    // back to gSceneViewport when the device had no current rect yet this frame (gSceneViewportCur.w
    // == 0 is that field's own documented sentinel, set at VoxiRenderer.cpp's beginShadowHistory)
    // rather than mapping into a zero-sized rect and dividing by zero.
    const float4 curRect = gSceneViewportCur.w > 0.0 ? gSceneViewportCur : gSceneViewport;
    const float2 curPx  = curRect.xy +
                          float2(curNdc.x * 0.5 + 0.5, 0.5 - curNdc.y * 0.5) * curRect.zw;
    const float2 prevPx = gSceneViewport.xy +
                          float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    // DESTINATION (curPx) minus SOURCE (prevPx) -- see header and UpscalerNeeds::MotionVectors
    // (RHIResources.hpp) for why that order is the contract a consumer can assume.
    return curPx - prevPx;
}

// One expansion point for PSMainVoxi's several `return` statements, so the three extra channels stay
// identical everywhere instead of by hand per site. gbufVelocity/gbufViewZ/gbufNormalRough are
// computed once, after `s` is built -- this macro only assembles values that already exist.
#define AVER_GBUF_RETURN(colorExpr) \
    { GBufferOut aver_gbuf_o; aver_gbuf_o.col = (colorExpr); aver_gbuf_o.velocity = gbufVelocity; \
      aver_gbuf_o.viewZ = gbufViewZ; aver_gbuf_o.normalRoughness = gbufNormalRough; return aver_gbuf_o; }
#else
// Disabled: PSMainVoxi's own `return` sites expand to a plain return, exactly what stood at each site
// before this feature existed -- see the #if branch above for what they become when it is on.
#define AVER_GBUF_RETURN(colorExpr) return (colorExpr)
#endif

// ---- B1: DOES A COLOUR ALREADY MARK THIS PIXEL'S DIFFUSE CHANNEL AS POISONED? ----
//
// giRestirIndirect (voxi_restir.hlsli) paints one of SEVEN sentinel colours over its own return
// value -- never the real indirect diffuse -- whenever gGiRestirParams.w > 0.5 and one of its own
// guards fired THIS frame (see that function's own POISON DEBUG VIEW comment for the legend and the
// precedence among those seven). PSMainVoxi/PSRayDriven below add an EIGHTH colour, VIOLET, for a
// DIFFERENT guard entirely -- the ray-traced SPECULAR term's own ceiling clamp (F5) -- and it is
// computed and applied in THIS file, not inside giRestirIndirect, so it cannot sit inside that
// function's own if/else-if precedence ladder and cannot silently pre-empt one of its seven returns.
//
// It CAN still collide at the pixel level: both markers are gated by the SAME flag
// (gGiRestirParams.w), and a pixel's diffuse and specular channels are independent, so nothing stops
// both guards firing together. THE PRECEDENCE CHOSEN: a giRestirIndirect colour on the diffuse
// channel always wins over violet. Rationale -- those seven already carry their OWN internal
// precedence (a non-finite guard always outranks a mere ceiling hit, per that function's own
// comment), so they are the more carefully arbitrated signal and diffuse corruption/ceiling is the
// established diagnostic this view exists for; violet is new and narrower (specular only), and
// overwriting an existing colour with it would destroy information the seven already spent effort
// ranking. Each PSMainVoxi/PSRayDriven call site below reads this back as `giDiffusePoisoned`.
//
// EXACT EQUALITY IS SAFE AND DELIBERATE, not a fragile float compare: every one of the seven colours
// below is built from the literals 0.0/0.5/1.0 alone, each exactly representable in IEEE754, and none
// is a value real shaded radiance can produce by coincidence -- the whole reason they were chosen as
// "unmistakable, scene-lighting-cannot-produce-this" sentinels in giRestirIndirect's own words.
// Comparing for exact equality is the same design already at work there, applied by the reader
// instead of the writer. Only meaningful right after a giRestirIndirect call (giMode 0's
// coneTracedIndirect never produces one of these by construction), so every call site below only
// tests it there.
bool aver_IsGiRestirPoisonColour(float3 c) {
    return (c.r == 1.0 && c.g == 0.0 && c.b == 1.0)    // magenta: store-time reservoir guard
        || (c.r == 0.0 && c.g == 1.0 && c.b == 1.0)    // cyan: candidate-radiance clamp guard
        || (c.r == 1.0 && c.g == 1.0 && c.b == 0.0)    // yellow: target-pdf guard
        || (c.r == 1.0 && c.g == 0.5 && c.b == 0.0)    // orange: final-estimate guard
        || (c.r == 0.0 && c.g == 0.0 && c.b == 1.0)    // blue: NRD-readback non-finite guard
        || (c.r == 1.0 && c.g == 0.0 && c.b == 0.0)    // red: raw estimate hit the ceiling
        || (c.r == 0.0 && c.g == 1.0 && c.b == 0.0);   // green: NRD-denoised readback hit the ceiling
}

// The Voxi lit pixel shader. Voxi supplies light transport only â€” sun visibility, sky, bounce â€”
// and the material shades it. Returns linear radiance; the post chain tonemaps.
//
// ---- [earlydepthstencil] IS LOAD-BEARING, NOT AN OPTIMISATION ----
//
// This shader WRITES UAVs (gAoHistOut u4, gAoHitDistOut u5, via rtSkyOcclusionTemporal) and calls
// clip(). Both of those individually defeat hardware early-Z, so without this attribute D3D12 moves
// the depth test AFTER the shader: a fragment that is hidden behind a nearer surface still runs,
// still traces its hemisphere, and still lands its store. Those stores are plain RWTexture2D --
// not ROV, not atomic -- so which fragment owns a texel is decided by DRAW ORDER, not by depth.
// The scene pass rasterises with CullMode::None (VoxiRenderer.cpp, `scene.cull`) and PTTest ships
// with its depth prepass off, so in an enclosed scene the last writer is routinely a surface BEHIND
// the wall, whose hemisphere is open to the sky and whose fresh trace is 1.0. The visible fragment
// then reads that poisoned texel back at history weight 0.97 and renders a fully-lit floor.
//
// MEASURED, PTTest NewSponza, fog off, fixed exposure, against a converged path-traced reference
// corrected for the PT view's own 1.06x horizontal stretch (without that correction 43% of the
// apparent error is misalignment and none of these numbers mean anything):
//
//   the RAW trace is identical on both primary-visibility paths and it is CORRECT --
//   raster 1.50, ray-driven 1.51 (MAD 0.05 between the two fields) against a truth of 1.32.
//   Let the accumulator run and the same quantity reads 4.62 on ray-driven (a fullscreen pass, one
//   invocation per pixel) and 132.76 here. An 88x lift, entirely manufactured downstream.
//
//   with this attribute:  occlusion 132.76 -> 21.27 and the gradient comes back (21.27 shadowed vs
//   59.78 open, where before it was saturated flat); the shaded image moves 6.43 -> 3.77 MAD from
//   truth, and raster-vs-ray-driven PARITY moves 6.05 -> 0.98. JungleRuins, a second and much more
//   open scene, moves the other way for the same reason and also improves: 14.06 -> 12.93.
//
// THE DEPTH PREPASS -- THE "WHY NOT" THAT STOOD HERE WAS MEASURED ON A BROKEN PREPASS. It recorded
// --depth-prepass regressing the image (MAD 6.43 -> 21.66, parity -> 20.82) with bit-identical
// results NRD on and off, and concluded the prepass was not the fix. The cause was not the prepass:
// with the device on mesh shaders (RENDER.MESHSHADERS 1) the backends handed every prepassed draw the
// ORDINARY mesh-shader pipeline -- Less, depth write on -- which rejected the equal depth the prepass
// had just written, so the colour pass was almost entirely discarded (and 47x cheaper for it). The
// "bit-identical NRD on and off" was the tell: nothing was shading. Fixed in D3D12Device/VulkanDevice::
// drawMesh (a prepassed draw now takes the input-assembler path its depth was written through).
// With that fixed, the prepass is expected to be the stronger form of THIS change -- one shaded
// fragment per pixel, so the AO history is written only by the visible surface -- and roughly 8x
// cheaper on the raster path (47.51ms scene draws measured without it). NOT YET MEASURED FIXED.
//
// THE clip() INTERACTION -- THIS PARAGRAPH USED TO SAY IT WAS SAFE, AND IT WAS WRONG.
//
// It claimed the only clip() here was the translucent ONE-LAYER selection below and that "there is
// no alpha-cutout discard here; the cutout lives in PSDepthPrepass". That was checked by searching
// THIS FILE ONLY. averEvalMaterial (called below) comes from the material prelude, and
// material_prelude.hlsl does `if (gMaterialFlags & AVER_MAT_ALPHA_MASK) clip(s.alpha - a.alphaCutoff);`.
// Under forced early depth the depth WRITE happens before this shader runs, so that clip discards the
// colour of a cut-out texel but not its depth: every hole in a leaf or a fence wrote opaque depth and
// hid whatever was drawn behind it afterwards. "Verified by capture on both scenes; neither lost
// geometry" was true and did not test it -- a whole-image comparison cannot see a few percent of
// foliage pixels. Found by adversarial review, not by a capture.
//
// HOW IT IS HANDLED NOW: an alpha-masked draw never reaches this shader with depth write on. Either
// the frame-wide depth prepass already wrote its depth (PSDepthPrepass does not force early depth and
// clips before it writes), or the colour walk writes that one draw's depth first through
// IDevice::drawMeshDepthOnly -- and in both cases the colour draw then uses the LessEqual/NO-WRITE
// twin, which leaves early depth nothing to write. See GameRender.cpp's colour loop for the residuals
// (cluster-dispatched geometry, material graphs driving opacity; skinned and soft-body meshes ARE
// covered, via their posed handle).
//
// The translucent ONE-LAYER clip is still harmless for the reason originally given: the blended PSO
// sharing this shader sets depth.write = false (VoxiRenderer.cpp), so there is no depth to leak.
//
// TEMPORAL VALIDATION, because this feeds an accumulated term and still frames cannot judge one:
// matched-pose A/B (--cam-translate 3 --cam-wobble 8 40 --cam-wobble-stop 100, 103 vs 220 frames)
// puts settle-time sensitivity at MAD 0.43 with the attribute against 0.62 without -- it REDUCES
// temporal dependence. High-frequency energy rises 0.39 -> 0.46, which is the occlusion field
// regaining real structure rather than sitting saturated.
#if AVER_GBUFFER
[earlydepthstencil]
GBufferOut PSMainVoxi(VSOut i) {
#else
[earlydepthstencil]
float4 PSMainVoxi(VSOut i) : SV_TARGET {
#endif
    float3 N = normalize(i.nrmWS);

    // ---- W6/M5: DOES THIS FRAGMENT'S HISTORY WRITE BELONG TO IT? ----
    //
    // blendedFragment is true only when BOTH are true: the backend actually replayed translucent
    // draws blended THIS frame (gAmbientParams.w bit 32 -- D3D12 only, C10; the bit is simply clear
    // on every other backend, so this reduces to always-false there with no second #if needed), AND
    // this specific fragment's own material is translucent (averDrawIsTranslucent, above the #if
    // AVER_RT region this file opens further down -- see that function's own comment for why it
    // checks both AVER_MAT_ALPHA_BLEND and gTransmission).
    //
    // gAverHistoryWrite is the gate everything downstream reads: false only when this fragment is a
    // blended replay AND the legacy override (gAmbientParams.z bit 32, voxi.legacyBlendedHistoryWrite)
    // is NOT forcing the old unconditional behaviour back on. Computed HERE, unconditionally (this
    // line runs whether or not AVER_RT is compiled in), because the flag has to be correct before ANY
    // of this function's own history writes are reached -- and the FIRST of them is not the diffuse-GI
    // block further down, it is rtShadowTemporal, called within a few lines of this one to build
    // sunVis, which writes gRtShadowHistOut on the very path this fragment's shading starts with.
    // Setting the flag at this function's own top, before that first call, is what makes every later
    // gate (reflection, GI diffuse, RT shadow/AO history) see the right value without each of them
    // needing to recompute it.
    const bool blendedFragment = ((uint)gAmbientParams.w & 32u) != 0u && averDrawIsTranslucent();
    gAverHistoryWrite = !blendedFragment || ((uint)gAmbientParams.z & 32u) != 0u;

    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));
#if AVER_RT
    // View-space depth/gradient taken HERE, at the top where control flow is uniform, and carried
    // down to the roughness-gated reflection block: a derivative inside divergent flow is undefined
    // in HLSL (the same landmine rtShadow's dpx/dpy step around) and would present as
    // adapter-specific corruption. rtShadowSpatial can afford its own ddx since its caller is uniform.
    const float rtViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
    const float rtDzdx  = ddx(rtViewZ);
    const float rtDzdy  = ddy(rtViewZ);

    // float3 NOW: rtShadowTemporal carries the medium's colour. shadowFactor returns a scalar and
    // promotes, so the non-RT path is unchanged.
    float3 sunVis;
    if (gShadowParams.z > 0.5)
        sunVis = rtShadowTemporal(i.wpos, N, L, i.pos.xy, ddx(i.wpos), ddy(i.wpos),
                                  (uint)max(gRtParams.y, 1.0));
    else                       sunVis = shadowFactor(i.wpos, N, ndl);
#else
    const float3 sunVis = shadowFactor(i.wpos, N, ndl);
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
        // unchanged. blendedFragment is computed once, above, at this function's own top.
        if (gGiRestirParams.x > 0.5 && !(blendedFragment && ((uint)gAmbientParams.w & 16u) != 0u)) {
            ind = giRestirIndirect(i.wpos, N, rtViewZ, i.pos.xy, (uint)gRtHistParams.z, ao);
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
    // THE SCENE PIPELINE DOES NOT CULL (`scene.cull = CullMode::None`, VoxiRenderer.cpp:3133), so a
    // closed volume rasterises all faces; opaque hides this behind the depth test, but a blended draw
    // writes no depth and every surviving face's alpha multiplies. A water box came out ~4 coats
    // thick: MEASURED, alpha 0.02 darkened the pit from (50.7,51.4,49.2) to (36.6,41.9,46.1).
    //
    // GATED ON THE AUTHORED `twosided` FLAG (parseable since the format existed, never read until
    // now): M_Glass sets it (glass is routinely CULL none, walked around), so a pane keeps
    // compositing both faces; water sets twosided=0 and gets one layer. Author decides.
    //
    // BLENDED ONLY: opaque already gets correct single-layer results from the depth test, and
    // single-sided opaque content (a floor seen from below) has always relied on CullMode::None.
    //
    // dot(N,V), not SV_IsFrontFace: reuses averVertexOf's own backFace rather than a second notion
    // that could disagree with the normal flip beside it.
    //
    // INVERTED WHEN THE EYE IS INSIDE THE VOLUME, not switched off -- the whole reason this is safe.
    // Inside a closed volume every face is backFace, so the plain discard deleted the surface
    // outright (swimming under the pool showed no water, just concrete). One layer from below still
    // means keeping back faces and dropping front ones -- the same rule read from the other side.
    //
    // gCameraMedium.x is a bounding-SPHERE test, loose (a shallow pool's sphere bulges above its
    // surface). Inverting rather than disabling makes that safe: a false positive on the deck drops
    // the near face and keeps the far one, still ONE layer. Disabling the discard would resurrect
    // the four-coats bug. Do not "simplify" this into an early-out.
    const bool eyeInside = gCameraMedium.x > 0.5;
    if ((gMaterialFlags & AVER_MAT_ALPHA_BLEND) && !(gMaterialFlags & AVER_MAT_TWO_SIDED) &&
        (eyeInside ? !vtx.backFace : vtx.backFace))
        clip(-1);

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;
    // CAUSTICS go INTO THE SUN TERM: they're concentrated sunlight, not their own glow, so scaling
    // the sun means a caustic can't appear in shadow (sunVis already zero there) -- the obvious tell
    // of a caustic implementation done wrong.
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
    // TRACED SKY VISIBILITY WHEN THE TIER PAYS FOR IT, the cone gather's own estimate otherwise.
    // gAmbientParams.x is already 0 on any frame without an acceleration structure (VoxiRenderer
    // gates it on rtActive_), so this needs no second RUNTIME test -- but it does need a
    // COMPILE-TIME one: rtSkyOcclusion lives inside this file's `#if AVER_RT` region because it
    // names gScene, and this line does not. Without the guard every non-RT entry point in the file
    // (VSShadow, PSVoxel, CSResolve, ...) fails on an undeclared identifier -- which is exactly what
    // happened, and cost a build that reported OK because HLSL compiles at RUNTIME here.
#if AVER_RT
    ind4.occlusion    = gAmbientParams.x > 0.5
                      // true: THIS pass writes the G-buffer REBLUR_DIFFUSE_OCCLUSION reprojects
                      // against, so its denoised answer is about this frame's geometry. See
                      // rtSkyOcclusionTemporal's own header for the measurement, and for why the
                      // ray-driven twin below passes false.
                      ? rtSkyOcclusionTemporal(i.wpos, N, i.pos.xy, (uint)gAmbientParams.x, ao,
                                               aoGathered, true)
                      : ao;
#else
    ind4.occlusion    = ao;
#endif
    // F4 (R1): ind4.diffuse is set HERE, after ind4.occlusion exists, not beside ind4.ambient above --
    // the sky-ownership subtraction below needs the occlusion this receiver actually traced/gathered
    // THIS frame, and moving the assignment down is cheaper than caching ao a second time.
    //
    // ONE OWNER FOR THE SKY, at giMode 1: giRestirIndirect's own traced miss already gave this pixel
    // its sky (see gAmbientParams.z's cbuffer comment, R1). Adding ind4.ambient on top of `ind` counted
    // it twice -- exactly the FmsEms/kD identity material_prelude.hlsl's averIndirectTerms computes
    // (diffAmbient = (FmsEms + kD) * ambient, diffBounce = kD * ind4.diffuse, both summed): subtracting
    // g * A (A = ambient * scale * occlusion * matAO, g = gVoxelParams.y) from `ind` here turns
    // diffBounce's kD * ind4.diffuse into kD * (est - g * A), which combines with diffAmbient's
    // (FmsEms + kD) * A to read kD * est + FmsEms * A + (1 - g) * kD * A -- the sky counted exactly
    // once, through giRestirIndirect's own traced visibility, with FmsEms still keeping its full
    // irradiance (ind4.ambient itself is not zeroed -- see PSRayDriven's own "ONE OWNER FOR THE
    // ENVIRONMENT" comment below for why zeroing it measured worse: it throws away the multi-scatter
    // compensation FmsEms multiplies). Skipped whenever `ind` is a poison colour (subtracting from a
    // sentinel would corrupt the very debug view it exists to show) or `ind` never came from
    // giRestirIndirect at all (restirSuppliedDiffuse false: the cone gather's `ind` was never
    // double-counted, so there is nothing here to remove).
    ind4.diffuse      = ind;
    if (restirSuppliedDiffuse && !giDiffusePoisoned && ((uint)gAmbientParams.z & 2u) == 0u)
        ind4.diffuse = ind - ind4.ambient * ind4.ambientScale * ind4.occlusion * s.occlusion * gVoxelParams.y;
#if AVER_RT
    // Ray traced when the acceleration structure and geometry table both exist; preferred over the
    // cone trace unconditionally since the cone is bounded by the voxel volume and this is not.
    //
    // WHY 0.75, NOT 1.0 (see rtReflection's own comment for the aperture/lobe history): past ~0.75
    // rough the lobe is wide enough that one ray is estimating a near-hemispherical integral neither
    // filter can close (spatial kernel saturates at radius 3; temporal history rejects itself once
    // the camera moves). What that rough a surface reflects is close to its surroundings' average --
    // what the cone trace/sky term below already return. 0.75 is where the ray stops being the
    // better answer, not where it stops being affordable.
    //
    // The fade is only a seam-hider across the last quarter (0 at 0.5 rough, 1 at 0.75): nothing
    // pops crossing the cutoff, everything below 0.5 is the traced answer at full strength.
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(i.pos.xy);   // see that function: no stale history
    if (rtReflTraced) {
        bool specHit = false;
        float3 refl = rtReflectionTemporal(i.wpos, N, R, L, i.pos.xy, s.rough,
                                           rtDzdx, rtDzdy, specHit);
        // ONE skyColor(R), NOT TWO, AND NOT ALWAYS ONE: both lerp operands want the same value, and
        // skyColor is a 32-step atmosphere march (too expensive to trust DXC to CSE), so it's named
        // once and guarded below.
        // WHEN THE MARCH IS PURE WASTE: smoothstep(0.5,0.75,rough) is EXACTLY 0 at/below 0.5, so a
        // hit reduces the lerp to `refl` and the marched sky is multiplied by zero -- water, glass,
        // chrome, wet stone. MEASURED on PTTest pool: blended replay 18.9 -> 15.9ms, whole frame
        // 44.86 -> 41.78ms, nothing on screen changing.
        // STILL ONE CALL: an earlier split into two skyColor(R) branches cost 13% (8.15 -> 9.26ms) --
        // a divergent wave executes both sides, so branching around the march ran it twice instead.
        const float skyW = smoothstep(0.5, 0.75, s.rough);
        float3 skyR = float3(0.0, 0.0, 0.0);
        if (!specHit || skyW > 0.0) skyR = skyColor(R);
        // The raster twin of the clamp documented at PSRayDriven's own copy of this line. Change
        // one, change both -- the two primary-visibility paths must agree about how much radiance a
        // reflection may return, or they disagree about the brightness of the same surface.
        //
        // B1 (F5): PRE-clamp value tested against the SAME ceiling the clamp below enforces, so a
        // pinned pixel can be told apart from one that was always going to land under it. `>=`, not a
        // negated `<` -- NaN compares false either way in HLSL, so `any(specRaw >= AVER_VOX_MAXRAD)`
        // is false for a NaN component (this guard is about a FINITE value being too large, not about
        // corruption -- there is no non-finite guard on this term today, and clamp()'s own
        // min(max(x,lo),hi) already floors a NaN component to 0.0 by this codebase's documented
        // comparison semantics, so a NaN here goes quiet rather than pinned OR painted).
        const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
        giPoisonSpecCeilHit = any(specRaw >= AVER_VOX_MAXRAD);
        ind4.specular = clamp(specRaw, 0.0, AVER_VOX_MAXRAD);
    } else
#endif
    if (gVoxelParams.w > 0.5) {
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(i.wpos, R, specAperture);
        // Bounded for the reason coneTracedIndirect is; sky is left alone (not a volume gather, no
        // runaway of its own).
        // SKY IS SKIPPED WHERE THE CONE ALREADY SAW A WALL: HLSL doesn't short-circuit a multiply, so
        // skyColor(R)*(1-sceneSpec.a) ran the full 32-step march even fully occluded -- the common
        // rough>0.75 case (concrete, cloth, stone) paying for a value it then multiplied away.
        // SAME SHAPE/FIX AS averFogInscatter'S THRESHOLD ("scene draw 8.9ms -> 1.3ms, 85% of the
        // scene pass"): a [0,1] coverage scaling bounded radiance can be skipped under one 8-bit step
        // with no banding. 0.004, NOT 0.01, because this weight multiplies a sky far brighter than
        // the fog reference -- at 0.004 the dropped term is at most 0.4% of a sky sample.
        const float skyWeight = 1.0 - sceneSpec.a;
        ind4.specular       = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
        if (skyWeight > 0.004) ind4.specular += skyColor(R) * skyWeight;
    } else {
        ind4.specular       = skyColor(R);
    }

    // ---- TRANSLUCENT MATERIALS TAKE A SEPARATE, EARLY-RETURNING PATH ----
    // Gated on the flag, not averOpacity(s) < 1: alpha is an authored number that can legally
    // disagree with which PIPELINE/blend-state the draw runs under (a blended twin at alpha 1, or
    // vice versa). AVER_MAT_ALPHA_BLEND is set exactly when routed to the blended PSOs
    // (scenePipeline(), VoxiRenderer.cpp) -- the actual decider of PremultipliedAlpha vs. straight
    // composite. Reading the wrong signal packs the wrong kind of output silently: every value stays
    // a plausible colour, just wrong by however translucent the surface is that frame.
    if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) {
        // averShadeSplit is averShadeDirect + averShadeIndirect's IDENTICAL arithmetic (PbrShaders.cpp),
        // kept as two registers instead of summed -- a blended draw gets the same energy an opaque
        // one would, apportioned between "coverage-weighted" and "always full strength" before alpha.
        float3 dif, spc;
        averShadeSplit(s, sun, ind4, dif, spc);
        // rgb = specular + diffuse*alpha, a = alpha (averBlendedOutput's contract) -- straight alpha
        // would multiply `spc` too, so a pane at 0.2 opacity showed its reflection at a fifth
        // strength. sceneBlendedPso_'s PremultipliedAlpha blend state expects it packed this way.
        // THE VOLUME, where one is authored. gAttenuationDistance <= 0 is the off state every
        // material had before, so this compiles to a compare nobody takes for ordinary glass --
        // averBlendedOutputVolume reduces to averBlendedOutput exactly at T=1 either way.
        // MEASURED WITH A RAY, not an authored height: see averVolumeThickness for why a fragment
        // can't know its own thickness (the guessed formula made the PTTest pool a bright opaque
        // slab). Behind AVER_RT: without a ray there's no structure to measure against, so the
        // surface keeps today's volumeless composite instead of a fabricated thickness.
        float4 outc;
#if AVER_RT
        // FRONT FACES ONLY -- a correctness gate, not an optimisation. averVolumeThickness traces
        // ALONG THE VIEW DIRECTION and takes the nearest hit (the medium's exit), correct only when
        // the shaded point is where the ray ENTERS. A BACK face starts where the ray LEAVES, so it
        // measures the distance to whatever's behind the glass (the floor, metres away) instead of
        // the pane's few centimetres. M_Glass authors twosided=1 (walked around), so its back face
        // was absorbing over the scene's depth -- why an 8cm pane read like a metre of bottle glass.
        //
        // s.backFace, NOT SV_IsFrontFace -- A BUG I SHIPPED: SV_IsFrontFace is winding-dependent;
        // s.backFace is `dot(N,V)<0` (averVertexOf), the actual question of whether the ray is
        // entering. The fluid box winds opposite the cube, so the winding test called the pool's
        // visible top a BACK face and silently switched water absorption off -- despite this file
        // already stating the rule ("dot(N,V), not SV_IsFrontFace") a few hundred lines up.
        //
        // Absorbing once, on entry, is also physically right: a pane's thickness is crossed once.
        // FROM THE SURFACE, NOT THE CBUFFER: a material GRAPH can drive attenuationColor/Distance
        // per pixel; reading gAttenuationColor would silently discard whatever the graph decided.
        if (s.attenuationDistance > 0.0 && !s.backFace) {
            // Measured ONCE, used twice: absorption needs it for Beer-Lambert, refraction needs it
            // for how far the bent path travels -- a second trace would give the same answer.
            const float volThick = averVolumeThickness(i.wpos, N, -s.V);
            const float3 volT = averVolumeTransmittance(
                s.attenuationColor, s.attenuationDistance, volThick);
            // THE BACKDROP PATH: what lets attenuationColor's HUE reach the picture at all --
            // averBlendedOutputVolume's fallback can only darken absorbed channels, never tint the
            // background (one blend alpha is one number). i.pos.xy is the screen coord the copy uses.
        outc = averBlendedOutputBackdrop(s, dif, spc, volT, i.pos.xy, i.wpos, volThick);
        } else
#endif
        outc = averBlendedOutput(s, dif, spc);

        // ---- THE FOG DECISION ----
        // Fogged HERE, deliberately not following WaterShaders.hpp's PSWater precedent, which skips
        // averApplyFog because water's colour is already a Fresnel-blended palette standing in for
        // the atmosphere itself -- fogging it again would double-apply air to a surface meant to
        // read as air. Glass isn't that: `dif`/`spc` are ordinary unfogged PBR terms.
        //
        // WHAT'S ALREADY FOGGED AND MUST NOT BE TOUCHED: the scene behind the pane. Those pixels went
        // through their own averApplyFog(radiance, theirWpos), and PremultipliedAlpha's blend
        // (dst_new = outc.rgb + dst.rgb*(1-alpha)) carries dst through untouched -- this code never
        // reads dst, so only `outc.rgb` (this pane's own new light) gets fogged below.
        //
        // FOGGING THE PACKED SUM, NOT dif/spc SEPARATELY: the atmosphere in front of the glass is the
        // same distance for both lobes, so both want identical attenuation/inscatter. averApplyFog is
        // affine (color*T + inscatter) but NOT distributive over the alpha-weighted split -- fogging
        // dif and spc separately then combining as spc_fogged + dif_fogged*alpha would add the
        // inscatter term TWICE for one slab of air, the same double-counting the water comment warns
        // about. Fogging `outc.rgb` once is symmetric with the opaque branch's single fog call below.
        // Alpha is coverage, not radiance, and is untouched by fog either way.
        //
        // OCCLUSION-AWARE: airVis computed once, from i.wpos, same as the opaque branch below -- see
        // voxiAirVisibility's own header comment, above, and gAirVis/gAirVisOut's, further up this
        // file, for why this is a world-space volume lookup and not the surface's own AO.
        outc.rgb = averApplyFogAirVis(outc.rgb, i.wpos, true, voxiAirVisibility(i.wpos));
        // B1 (F5): applied LAST, after fog, so the marker is the true final colour and cannot be
        // fogged or blended away -- see aver_IsGiRestirPoisonColour's own comment for why a
        // giRestirIndirect colour on the diffuse channel (giDiffusePoisoned) outranks this one.
        if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
            outc.rgb = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
        AVER_GBUF_RETURN(outc);
    }

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind4);
    // OCCLUSION-AWARE: see the blended branch's identical comment, above.
    radiance = averApplyFogAirVis(radiance, i.wpos, true, voxiAirVisibility(i.wpos));
    // B1 (F5): same override, same precedence, as the translucent branch's copy above.
    if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
        radiance = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
    AVER_GBUF_RETURN(float4(radiance, averOpacity(s)));
}

// ================= ray-driven primary visibility (experimental) =================
// THE ONLY THING THIS REPLACES IS "WHAT DID THIS PIXEL SEE" -- everything after the first hit is
// the same work PSMainVoxi does (sun shadow, sky ambient, fog), since the rasteriser never did that.
//
// WHAT IT GIVES UP: hardware early-Z. A rasterised fragment discovered hidden is discarded before
// its shader runs; a ray pays the whole traversal to learn the same thing. The trade this mode
// exists to measure; the number to beat is in Settings::rtRenderMode.
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
// traces AVER_RT_MASK_OPAQUE only, so widening the mask would start hitting glass with no TLAS change.
// Glass is drawn only where raster mode draws it: D3D12Device::endFrame's blended-mesh flush, through
// VSMain+PSMainVoxi's premultiplied-alpha PSO, after this pass's scenePass() and the deferred sky --
// depth test, blend equation, vertex math and shading are IDENTICAL code paths regardless of which
// pass answered primary visibility for what's behind the pane; none of it reads rtRenderMode.
//
// WHAT DOES DIVERGE: what a translucent surface reveals, the picture already painted by whichever
// pass drew primary visibility. This pass's hit shading is a simpler material response than
// PSMainVoxi's -- maps ARE sampled (base colour, metal-rough, normal, occlusion, emissive, second
// layer), but no material GRAPH runs, normal-map perturbation is compiled out, and a stochastic
// path-traced bounce stands in for the voxel cone trace. These approximations, accepted for an
// opaque surface, stopped being invisible once glass could put that surface behind a window --
// see this pass's environment-specular block below for the one piece of that gap this change closes
// (voxel-cone GI instead of flat sky) and why the rest is untouched.
//
// BEHIND AVER_RT because RayQuery is: this entry point only compiles into the SM 6.5 variant, and
// VoxiRenderer refuses the mode outright when the device has no ray-query support.
#if AVER_RT
struct RayDrivenOut {
    float4 col   : SV_TARGET;
    float  depth : SV_DEPTH;
};

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

    // W6/M5: EXPLICITLY TRUE, NOT LEFT TO THE STATIC'S OWN DEFAULT. A blended (glass/water) draw
    // never reaches this entry point at all -- see this function's own header comment, "WHAT A
    // TRANSLUCENT SURFACE SHARES WITH THIS PASS": the primary ray here traces AVER_RT_MASK_OPAQUE
    // only, and glass is routed into the translucent lane and drawn by PSMainVoxi's own blended
    // replay instead. So PSRayDriven has no blendedFragment computation of its own and never needs
    // one; setting the flag explicitly here, rather than computing a test that could only ever read
    // false, says that plainly instead of leaving a reader to prove the negative from this function's
    // absence of one. Every history write below (the sky-miss surface-history sentinel just below,
    // and the AO hit-distance write further down) is therefore always live for this pass.
    gAverHistoryWrite = true;

    // The same NDC-to-world-ray reconstruction PSVoxelDebug does, through the same gInvViewProj,
    // so the primary ray and the debug raymarch cannot disagree about where a pixel looks.
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 dir = normalize(far.xyz / far.w - gCamPos.xyz);

    RayDesc r;
    r.Origin    = gCamPos.xyz;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = 1.0e7;

#if AVER_RD_SPLIT
    // STAGE B: read CSRdVisibility's record instead of tracing. Everything from here to the miss
    // check below mirrors the #else branch's shape; the two must be kept in step by hand since a
    // preprocessor switch, not a shared statement, is what makes the default (AVER_RD_SPLIT 0) compile
    // byte-for-byte unchanged -- see rdSurfaceFromRecord's own header, declared alongside gRdVisBuf/
    // gRdSunVisTex above, for the reconstruction itself.
    const uint  rdPitch = rdRowPitch();
    const uint2 rdPixel = uint2(i.pos.xy);   // truncates the pixel centre to its integer pixel, the
                                              // same convention CSRdVisibility indexes the buffer by
    const uint4 rdRec   = gRdVisBuf[rdPixel.y * rdPitch + rdPixel.x];

    if (rdRec.x == 0xFFFFFFFFu) {
        // THE SAME MISS HANDLING AS THE #else BRANCH'S OWN COPY BELOW -- see that copy for why each
        // field is set the way it is. Duplicated rather than shared for the reason given above.
        o.col   = float4(skyColorFull(dir), 1.0);
        o.depth = 1.0;
#if AVER_GBUFFER
        o.velocity        = float2(0.0, 0.0);
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
#else
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Opaque lane. THE ONE RAY USING THE NARROW LANE: AVER_RT_MASK_OPAQUE, not _OPAQUE_ALL -- it
    // starts inside the viewer's own head, so it is the one traversal that must not see
    // AVER_RT_MASK_OWNER_HIDDEN. Every other opaque query in this file asks for _ALL.
    //
    // THIS IS PRIMARY VISIBILITY, so it is the ray the cutout matters most on: whatever it commits
    // is literally what you see. FORCE_OPAQUE here made every leaf card a solid rectangle while the
    // raster path clipped the same material correctly -- and since ray-driven is the standing
    // default, the wrong one was the one on screen.
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE, r);
    averRtProceedSolid(q);

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        // A miss is the sky at the far plane. Depth 1, not 0: this projection isn't reversed, so 0
        // would put the sky in front of everything.
        // skyColorFull, NOT skyColor -- a whole-screen atmosphere march difference. This colour is
        // thrown away whenever a sky dome draws: the deferred dome (D3D12Device.cpp, gated on
        // skyEnabled_ && !frameSuppressed_) is an OPAQUE fullscreen triangle with DepthFunc EQUAL
        // against the 1.0 written below, so it overwrites every one of these pixels. The dome is the
        // richer sky (clouds, atmosphere, sun disc); this write is only the no-dome placeholder.
        // Paying for a per-pixel march to produce a value unconditionally overwritten is the most
        // expensive way to compute nothing -- MEASURED at up to 41% of a frame once skyColor started
        // honouring the physical model.
        // WHAT CHANGES: nothing, with a sky enabled (every normal frame, overwritten either way).
        // With the sky DISABLED, the background is the authored gradient, not a marched atmosphere.
        o.col   = float4(skyColorFull(dir), 1.0);
        o.depth = 1.0;
#if AVER_GBUFFER
        // No real surface for a miss, so no true velocity or normal. Velocity 0 (matches
        // averGBufferVelocity's own near-plane fallback); viewZ 1e7 (a sentinel past real geometry,
        // matching this ray's own TMax); normal -dir so renormalising gives a unit vector, not a NaN
        // from normalize(0,0,0). NOT A SKY MASK SUBSTITUTE: a consumer excluding sky pixels should
        // use viewZ's far-plane sentinel, not claim anything from this normal.
        o.velocity        = float2(0.0, 0.0);
        o.viewZ            = 1.0e7;
        o.normalRoughness  = averPackNormalRoughness(-dir, 1.0);
#endif
        // A SKY MISS HAS NO SURFACE FOR NEXT FRAME TO REPROJECT EITHER -- write the same 0-packed-
        // normal sentinel RAB_GetGBufferSurface tests for, rather than leaving THIS pixel's slot
        // holding whatever it held the last time this pixel WAS a surface (the camera panned away,
        // say). Only when the pair is actually bound this frame; see giRestirIndirect's own write
        // for the sentinel's exact contract. The position channel doesn't need writing too -- the
        // normal channel's 0 alone is what RAB_GetGBufferSurface tests before it ever reads position.
        if (gGiRestirParams.x > 0.5)
            gGiSurfNrmHistOut[uint2(i.pos.xy)] = float2(0.0, asfloat(0u));
        return o;
    }

    // Surface reconstruction: same barycentric interpolation and ROTATION-ONLY normal transform as
    // rtReflection (see there for why no inverse transpose) -- must agree, or a surface would shade
    // differently seen directly vs. in a mirror.
    RtInstance inst = gRtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0  = inst.firstVertex + gRtIndices[tri + 0];
    uint i1  = inst.firstVertex + gRtIndices[tri + 1];
    uint i2  = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w    = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    float3 N    = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(N, dir) > 0.0) N = -N;   // face the ray, so a back-facing hit is not lit from behind

    // UV, same barycentric pattern as `nObj` above -- RtVertex has always carried it, unread until
    // now. NOT sampled here (no texture array yet for it). WHOEVER ADDS THAT SAMPLE: this is a RAY
    // HIT, not a rasterised fragment -- ddx/ddy on it is undefined in HLSL (the same landmine
    // rtShadow's dpx/dpy step around), so the sample MUST be SampleLevel or SampleGrad, never plain
    // Sample(). Mip 0 costs minification aliasing at glancing angles/distance; a future SampleGrad
    // would want a footprint like rdRayDx/rdRayDy below, recomputed here since UV precedes wpos/hitT.
    float2 hitUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;

    // The hit's own material, keyed by RtInstance::materialIndex (see that field's own comment for
    // what it used to be and why repurposing it cost nothing). This is what makes the constants
    // below real instead of guessed -- see the comment just above where they are used.
    RtMaterial mat = gRtMaterials[inst.materialIndex];

    const float hitT = q.CommittedRayT();
    float3 wpos = gCamPos.xyz + dir * hitT;
#endif
    float3 L    = normalize(gLightDir.xyz);

    // ---- THE SHADOW-RAY FOOTPRINT: A RAY DIFFERENTIAL, NOT A SCREEN-SPACE DERIVATIVE ----
    // What stood here passed float3(0,0,0) for dpx/dpy -- a point sample, citing rtReflection's own
    // inner shadow call as precedent (correct THERE: a reflected hit's neighbours can land on
    // triangles metres apart, so ddx/ddy is undefined and meaningless). MEASURED (speckle audit): a
    // dense fine speckle on PTTest's pit far wall, ray-driven only, tied to nothing but this call --
    // a zero footprint fires every sample from the same origin, aliasing a sub-pixel shadow boundary
    // (a grazing self-shadow) into speckle instead of a true area fraction. PSMainVoxi never shows
    // it because its ddx(i.wpos)/ddy(i.wpos) footprint is real.
    //
    // UNLIKE THE REFLECTED CASE, a primary ray's DIRECTION is a smooth analytic function of its pixel
    // (`dir`, from i.ndc/gInvViewProj), so it can be evaluated for the NEIGHBOUR pixel directly --
    // no ddx/ddy, well-defined in any control flow. This is a RAY DIFFERENTIAL (Igehy 1999, the same
    // idea a ray-cone texture-LOD scheme uses): reconstruct the neighbour's primary-ray direction and
    // see how far it diverged by `hitT` -- a function of the CAMERA and pixel grid, never of what
    // either ray hit, unlike ddx(wpos) across a silhouette which would return a metres-wide gap.
    //
    // NOTE THE PUNCTUATION: this shader lives inside a C++ raw string literal, so close-paren
    // double-quote ENDS IT. An earlier draft closed a quote right after a bracket here and produced
    // forty lines of C++ syntax errors. Keep brackets and quotes apart in this file.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float4 farDx = mul(float4(i.ndc + float2(ndcPixelStep.x, 0.0), 1.0, 1.0), gInvViewProj);
    float3 dirDx = normalize(farDx.xyz / farDx.w - gCamPos.xyz);
    float4 farDy = mul(float4(i.ndc + float2(0.0, ndcPixelStep.y), 1.0, 1.0), gInvViewProj);
    float3 dirDy = normalize(farDy.xyz / farDy.w - gCamPos.xyz);
    // World-space displacement to the neighbour ray, at the SAME distance this ray travelled --
    // "pixel angular size times hit distance" -- widening with range, shrinking near the camera.
    const float3 rdRayDx = (dirDx - dir) * hitT;
    const float3 rdRayDy = (dirDy - dir) * hitT;
    // Flattened onto the hit's tangent plane before use as dpx/dpy: rtShadow jitters the ray ORIGIN
    // by these then offsets along N, and an un-flattened footprint could push that origin off-surface,
    // reopening the grazing-angle acne N*bias exists to close. PSMainVoxi's ddx/ddy(wpos) are real
    // surface points already and skip this; this reconstruction isn't, so it's projected onto N.
    const float3 dpx = rdRayDx - N * dot(rdRayDx, N);
    const float3 dpy = rdRayDy - N * dot(rdRayDy, N);

    // THE SHADOW RAY IS THE SAME CALL THE RASTER PATH MAKES, temporal wrapper included -- identical
    // shadow cost, the timing gap is only primary visibility plus this footprint's matrix multiplies.
    // Keyed by pixel, same grid as the raster pass, so the history buffer means the same thing here.
#if AVER_RD_SPLIT
    // STAGE B reads Stage S's already-resolved sun visibility instead of calling rtShadowTemporal
    // itself -- CSRdShadow ran that same call for this pixel (see its own body, further down this
    // file). The ablation check is repeated here rather than shared with CSRdShadow's copy, for the
    // same "no shared statement" reason the trace block above gives.
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float3 sunVis = float3(1.0, 1.0, 1.0);   // ablated: fully lit, no ray
#else
    float3 sunVis = gRdSunVisTex[uint2(i.pos.xy)].rgb;
#endif
#else
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float sunVis = 1.0;   // ablated: fully lit, no ray
#else
    float3 sunVis = rtShadowTemporal(wpos, N, L, i.pos.xy, dpx, dpy, (uint)max(gRtParams.y, 1.0));
#endif
#endif

    // Lambertian exitant radiance, /PI on the direct term -- see rtReflection for what omitting it
    // cost last time (every sunlit surface 3.14x too bright, an exposure-looking bug the white
    // furnace can't catch since it turns the sun off).
    // ---- the bounce loop ----
    // PATH TRACING HERE IS EXTRA RAYS ON THE LOOP ABOVE, not a second renderer: the first hit is
    // already shaded like rtReflection shades its own hit, and each further bounce repeats that,
    // carrying a throughput and adding emission toward the previous surface.
    // COSINE-WEIGHTED so the BRDF's 1/PI and the rendering equation's cosine cancel against the pdf,
    // leaving a plain albedo multiply -- the /PI mistake rtReflection already paid for, reversed.
    // SCREEN-PINNED HASH, no per-frame jitter, matching rtShadow's seeding: the gate oracle compares
    // nine configs bit-exactly, so a frame counter would make each one a different image. Noise is a
    // fixed dither, not something that converges -- honest for a first cut; a temporal accumulator
    // would fix it.
    // THE ENGINE'S OWN BRDF, not a second one: averShadeDirect is the same Cook-Torrance GGX
    // PSMainVoxi uses, so a hand-built AverSurface keeps ray and raster images agreeing on material
    // look. The /PI lives inside it (kdAlbedo/PI) -- the divide rtReflection learned the hard way.
    //
    // THREE CONSTANTS USED TO BE DEFAULTED: reflectance/f90/albedo are per-MATERIAL, but a ray hit
    // had only RtInstance -- inst.albedo is the raster path's flat per-DRAW colour (neutralised to
    // white for an AUTHORED material by the raster shader's multiply convention, a separate known
    // bug), and reflectance/f90 sat at textbook dielectric defaults (0.04/1.0) unconditionally.
    // `mat`, via the new materialIndex/gRtMaterials pair, is real per-material data -- the whole
    // reason that pair exists.
    //
    // METALLIC/ROUGHNESS DELIBERATELY LEFT ON inst.metallic/inst.roughness, NOT mat.metallicFactor/
    // roughnessFactor, despite the task brief calling the former "already real, per-instance". NOT
    // ALWAYS TRUE: for an AUTHORED material inst.metallic/roughness carry the same neutralised-to-1.0
    // placeholder inst.albedo used to (buildAccelerationStructures, VoxiRenderer.cpp, not owned here),
    // so an authored metal can still render fully rough/metallic regardless of authoring. C++-side
    // fix (source RtInstance.metallic/roughness from mat.metallicFactor/roughnessFactor at build
    // time), out of this change's scope. STATED KNOWN GAP, not believed fixed.
    //
    // WHAT THIS DOES NOT VERIFY: whether gRtMaterials is actually populated with each instance's
    // real constants for every draw path (authored .ocmat and the built-in SurfaceLook table) rather
    // than a fallback entry -- C++-side (VoxiRenderer.hpp/.cpp), not confirmable from this file.
    AverSurface s = (AverSurface)0;
    s.N        = N;
    s.V        = -dir;
    s.H        = normalize(s.V + L);
    // MULTIPLY THE PER-DRAW VALUE BY THE MATERIAL FACTOR, DO NOT REPLACE IT -- the raster path's
    // model (PbrShaders.cpp: `s.metallic = saturate(gMaterial.x * a.metallic)`); departing from it
    // turned this whole render white.
    // BOTH TERMS ARE LOAD-BEARING: an AUTHORED (.ocmat) draw has its per-draw colour/metal/rough
    // NEUTRALISED to 1.0 by the caller, so the material factor carries the value (per-draw alone =
    // white); an UNAUTHORED draw has no material, so its fallback factors are 1.0 and the per-draw
    // value carries the colour (factor alone = white). Each is the identity where the other carries
    // the data -- the product is right in both cases, why the raster path multiplies.
    // THE BUG THIS REPLACES: reading `mat.baseColorFactor.rgb` alone shaded every unauthored draw
    // (PTTest's floor, walls, crates, targets) as fallback white while the rasteriser drew them
    // correctly -- reported as "everything is white".
#ifdef AVER_RT_BINDLESS
    // THE STOCK MATERIAL AT A RAY HIT: six of eight maps, slope-blended second layer. Composed like
    // the raster path -- every factor MULTIPLIES its texel rather than replacing it, so a textureless
    // material reduces to the untextured branch and the paths agree. Fallbacks are averSampleMaps'
    // own identity values, so an unbound slot costs one compare.
    //
    // "ALL EIGHT MAPS" AND "NORMAL-MAPPED SHADING NORMAL" IS WHAT THIS COMMENT USED TO SAY, and both
    // halves were wrong. Slots 2 and 7 (normal, layer-1 normal) are sampled below and then DISCARDED:
    // the only reader is averRtPerturbNormal, behind `#define AVER_RT_NORMAL_MAPPING 0` further down.
    // So this path has no normal mapping at all, and a reader trusting the sentence above would look
    // for a bug in the tangent frame that never runs.
    //
    // The samples themselves are almost certainly free -- averRtSampleSlot is pure, so DXC dead-code
    // eliminates a result nothing reads -- which is why this is corrected rather than deleted. The
    // define is 0 because turning it on made ElectricDreams terrain WORSE by a measurement recorded
    // at that define; settling that needs a flat surface with a known-good normal map judged against
    // the path tracer (which gained working normal mapping with a derived tangent frame), not a
    // deletion here.
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
    float3 mapEmis  = averRtSampleSlot(mat, 4, uvS, uvGx, uvGy, float4(0, 0, 0, 1)).rgb;
    // glTF packs occlusion in R, roughness in G, metallic in B -- the same unpack averSampleMaps does.
    float2 metalRough = float2(mapMR.g, mapMR.b);
    float3 normalTS   = float3(mapNrm.xy * mat.normalScale, mapNrm.z);

    // THE SECOND LAYER, blended by SLOPE off the GEOMETRIC normal, not the normal-mapped one
    // (averBlendLayers): "is this a cliff" is a surface property, and a normal map would make the
    // layer choice flicker with every bump. N here is BEFORE perturbation, same as the raster path.
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

    // NORMAL MAPPING LAST, only when a map is bound: with none, normalTS is identity (0,0,1) and
    // perturbing is a no-op that still costs a tangent solve.
    // NORMAL MAPPING IS OFF -- A MEASURED DECISION, not an omission. Mean absolute difference against
    // the raster path, whole viewport:
    //                              base colour only   + these slots, no normals   + normal mapping
    //   PTTest (authored flats)          32.08                17.23                    18.21
    //   ElectricDreams (terrain)          6.06                 7.48                    17.19
    // Costs a little on authored surfaces, catastrophic on terrain -- worse than no textures at all.
    // RULED OUT: the frame IS orthonormal (nTS=(0,0,1) reproduces N exactly), the map decodes right
    // (Z saturated positive), normalScale is 1.0, and it isn't the layer-1 blend (no change alone).
    // LEADING SUSPECT: the frame is ROTATED WITHIN THE TANGENT PLANE (nTS=(0,0,1)->N only proves T/B
    // perpendicular to N, not T along +U); or raster doesn't perturb terrain either, in which case
    // raster is the wrong reference and this needs a flat surface with a known-good normal map.
    // Flip to 1 to measure; do not ship at 1 until terrain is explained.
#define AVER_RT_NORMAL_MAPPING 0
#if AVER_RT_NORMAL_MAPPING
    if (mat.texIndex[2] != AVER_TEX_UNBOUND || mat.texIndex[7] != AVER_TEX_UNBOUND) {
        s.N = averRtPerturbNormal(mat, inst, N, normalTS,
                                  gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                                  gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv);
        // The perturbed normal must still face the ray (same reason as the geometric flip above): a
        // normal map can tip a grazing normal past the horizon, shading from behind as a false shadow.
        if (dot(s.N, dir) > 0.0) s.N = -s.N;
    }
#endif

    s.albedo    = inst.albedo * mat.baseColorFactor.rgb * mapBase.rgb;
    s.emissive  = mat.emissiveFactor * mapEmis;
    // THROUGH occlusionStrength, as averBuildSurface does: s.occlusion = lerp(1.0, a.occlusion,
    // gOcclusionStrength). Full strength (tried first) darkened ElectricDreams terrain from
    // 111,101,96 to 73,72,76 against a raster reference of 104,102,100 -- moved further from raster.
    s.occlusion = lerp(1.0, mapOcc, mat.occlusionStrength);
#else
    s.albedo   = inst.albedo * mat.baseColorFactor.rgb;
#endif
#ifdef AVER_RT_BINDLESS
    // metalRough is the SAMPLED pair, unpacked glTF-style: .x roughness (green), .y metallic (blue).
    // Multiplied onto the factors as averStockAuthored does, so an unbound map contributes 1.0.
    s.metallic = saturate(inst.metallic  * mat.metallicFactor  * metalRough.y);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor * metalRough.x, 0.045, 1.0);
#else
    s.metallic = saturate(inst.metallic * mat.metallicFactor);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor, 0.045, 1.0);   // averEvalMaterial's own floor
#endif
    s.ndv      = saturate(dot(s.N, s.V));
    s.f90      = mat.f90;
    s.reflectance = mat.reflectance;
    // HAND-SET: this surface is hand-built, with no material cbuffer to read from (same reason
    // reflectance is carried above rather than read off gMatReflectance). HLSL doesn't
    // zero-initialise a struct, so omitting these would feed averDirectTerms garbage off the stack.
    // A PRIMARY RAY LEAVES THE EYE, so its first hit is always a front face: no refracted ray, no
    // exit interface, TIR cannot arise -- stated here rather than an uninitialised bool reading false.
    s.backFace  = false;
    s.sssWeight = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceWeight) : 0.0;
    s.sssRadius = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceRadius) : 0.0;
#ifdef AVER_LAYERED_BSDF
    // THE COAT NEEDS EXPLICIT LINES: this pass zero-inits then hand-sets every field, so a new field
    // silently defaults to 0 (the right OFF state) -- which would have looked correct while meaning
    // "no material has a coat in ray-driven mode". Read from the hit's own material, like the lines
    // above, since this pass has no material cbuffer.
    s.coatWeight = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatWeight)    : 0.0;
    s.coatRough  = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatRoughness) : 0.0;
    s.coatF0     = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatF0)        : 0.0;
#endif
    // Identical shape to averBuildSurface's F0 (PbrShaders.cpp: `lerp(gMatReflectance.xxx, s.albedo,
    // s.metallic)`), mat.reflectance standing in for gMatReflectance -- same value, this pass's own
    // material buffer instead of the per-draw cbuffer a ray hit has no binding for.
    s.F0       = lerp(mat.reflectance.xxx, s.albedo, s.metallic);
    s.F        = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
    // (1 - transmission), as averBuildSurface does (PbrShaders.cpp, kdAlbedo line): light passing
    // THROUGH the substrate can't also scatter back out, or the material invents energy. Kept
    // identical on both paths deliberately -- a rule honoured by raster but not primary rays is the
    // exact defect shape this tree keeps rediscovering.
    // A BLENDED pane never arrives here (glass is drawn by PSMainVoxi in both modes), so this fires
    // only for an OPAQUE material authoring transmission -- why the rule is against the material
    // field, not the blend mode.
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
    // The ray-driven twin of the caustic term above. No blended test here: this pass shades opaque
    // primary hits only -- translucency is drawn by the blended replay through PSMainVoxi.
    sun.visibility *= 1.0 + averCausticFocus(wpos);

    const uint bounces = (uint)max(gPtBounceParams.x, 1.0);

    float3 radiance = averShadeDirect(0.0, s, sun);

    // THE ENVIRONMENT THROUGH THE ENGINE'S OWN INDIRECT TERM, not a diffuse-only line. What stood
    // here (`radiance += s.kdAlbedo * averSkyIrradiance(N) * gAmbient.r`) had two faults, found by
    // the white furnace:
    //   - A WHITE METAL RENDERED BLACK (0.003 vs correct 1.000): averShadeIndirect was never called,
    //     so the environment SPECULAR term (FssEss*ind.specular) didn't exist here at all; a metal's
    //     kdAlbedo=0 gets nothing from a diffuse-only line, so roughness couldn't matter either.
    //   - PATH TRACING DOUBLED THE ENERGY (1.000 -> 1.977): this line added the sky once, then the
    //     bounce loop added it again whenever a ray escaped -- every path escapes on bounce one in
    //     an open scene, so a bounce-depth sweep came back flat and hid the double-count.
    //
    // ONE OWNER FOR THE ENVIRONMENT: this call. The bounce loop no longer adds sky on escape, only
    // surface-to-surface light. Zeroing ind.ambient instead (tried first) measured worse: FmsEms
    // multiplies IRRADIANCE, so zeroing it threw the compensation away and a white metal fell back
    // to the single-scatter curve (0.971 at roughness 0.05 to 0.450 at 1.0) -- FmsEms is specular
    // energy a path-traced bounce doesn't carry.
    // Also makes the ray path structurally match raster: full unoccluded sky ambient plus bounced
    // light on top, without subtracting the sky that bounce occludes. Same approximation, same place.
    float3 R = reflect(dir, N);
    AverIndirect ind;
    ind.ambient      = averSkyIrradiance(N);
    ind.ambientScale = gAmbient.r;

    // ---- DIFFUSE INDIRECT: THE CONE TRACE, exactly as PSMainVoxi does it ----
    // Replaces a ONE-SAMPLE STOCHASTIC BOUNCE THAT COULD NOT CONVERGE -- FROZEN noisy, not merely
    // noisy: the bounce loop drew its direction from `rtHash(i.pos.xy + ...)`, a PURE FUNCTION OF THE
    // PIXEL with no frame term (needed for the gate oracle), so every pixel picked one direction and
    // kept it forever -- a fixed wrong answer, not something a denoiser could average over.
    // WHAT IT LOOKED LIKE: dense static salt-and-pepper on enclosed surfaces, none in the open (an
    // escaped ray adds nothing, variance zero; inside a room every ray lands on a different wall,
    // variance enormous) -- the concrete pit reported "still broken" while the sky nearby was clean.
    // coneTracedIndirect is what PSMainVoxi has always used: DETERMINISTIC (prefiltered clipmap
    // march, no variance), and it hands back a real ambient-occlusion factor the bounce loop never
    // provided (`ind.occlusion = 1.0` below was a stated gap). Makes both paths answer this the same
    // way -- the point of a mode meant to differ only in HOW THE FIRST SURFACE IS FOUND.
    //
    // COST, MEASURED RATHER THAN ASSUMED, and it went the other way from the guess: an earlier
    // version of this comment claimed the cone trace was "cheaper as well as cleaner". It is not --
    // on PTTest (voxelResolution 512, GI Epic) "Voxi ray-driven primary" went 1.3-1.4ms -> 3.0-4.0ms,
    // whole frame 4.13ms median -> 8.86ms. A 6-cone gather over a 512^3 clipmap costs more than three
    // ray-query traversals on hardware with idle ray-query units. Stated because a wrong performance
    // claim in a comment outlives the person who wrote it.
    //
    // STILL THE RIGHT TRADE, for parity not speed: this is the cost PSMainVoxi always paid for the
    // same term. Ray-driven wasn't cheaper before, it was doing something worse and charging less --
    // the two paths now cost the same, the only footing on which "is ray-driven faster" is a
    // question worth asking.
    //
    // WHERE THE COST GOES, AND WHERE IT DOESN'T ANY MORE: this used to say giCones was six,
    // hardcoded. Already false when written -- Renderer::giConesForQuality (Voxi.cpp) has covered
    // Off/Medium=6, Low=3, High=9, Epic=13 since commit fbb3aad, wired through gGiParams.x into the
    // dynamic loop. The ladder already exists and belongs to GI settings. PTTest pins RENDER.GI 4
    // (Epic, 13 cones), so any cost number there is this pass's worst rung.
    //
    // BUT THE CONE COUNT IS *NOT* WHERE THE TIME GOES -- MEASURED, not reasoned. Arithmetic predicted
    // dropping Epic to a lower rung would recover 1.5-2ms of ~3.7ms; it does not. Same camera, only
    // tier differing:
    //     Epic, 13 cones -- ray-driven primary 3.7-3.8ms, whole frame 6.498ms
    //     High,  9 cones -- ray-driven primary 3.5-3.8ms, whole frame 6.066ms
    // Four fewer cones bought ~0.4ms of 6.5ms. The gather is real but not dominant, and **the 3x gap
    // against the rasteriser remains unexplained**. Already tested; don't shave cones on this theory.
    //
    // WHAT TO DO INSTEAD -- AND THE MARKER THIS ASKED FOR ALREADY EXISTS. This used to say "there is
    // NO GPU-timed marker for the raster path's own pixel shading" and call for one to be added. It
    // is there and always was: D3D12Device::beginGpuSpan("scene draw") opens at the tail of
    // beginFrame and closes at the top of endFrame, bracketing every drawMesh the raster path issues
    // -- and "Voxi ray-driven primary" is a CHILD of that same span, so the two are already
    // like-for-like. Nothing needed adding.
    //
    // THE 0.1ms WAS NOT A MISSING MARKER, IT WAS AN EMPTY SPAN. "scene draw" measured 0.1ms because
    // no drawMesh ran at all: the path-traced scene view was suppressing the scene and painting the
    // frame itself, so the "rasteriser" being timed had drawn nothing. D3D12Device.cpp's own
    // suppression warning documents exactly that run. The fix was never instrumentation, it was
    // pinning `--pt 0` so the rasteriser is the thing on screen.
    //
    // SO THE COMPARISON IS: --gpu-timing twice on ONE scene at ONE resolution with a MOVING camera,
    // `--rt-render-mode 1 --pt 0` against `--rt-render-mode 0 --pt 0`, reading "scene draw" from
    // each. Both runs must be checked against the "is painting the scene" log line before either
    // number is believed -- that line, not the timing, is what says which renderer ran.
    //
    // AND THE GAP IS SMALLER THAN THE 3x ABOVE SUGGESTS. This file's own AVER_RD_ABLATE header
    // records ray-driven primary at ~6.7ms of 14.55ms against raster's 7.82ms on PTTest with RT on
    // in both -- so what rasterising primary visibility can recover is that GAP, not the whole
    // "Voxi ray-driven primary" span. That span contains the entire deferred shade (shadow, cones,
    // reflection, sky-occlusion, both sky marches, fog), and the shadow ray in particular is THE SAME
    // CALL the raster path makes, as the comment beside it says.
    float rdAo  = 1.0;
    ind.diffuse = 0.0;
    // B1: mirrors PSMainVoxi's own copy (search aver_IsGiRestirPoisonColour) -- true only when the
    // ReSTIR branch below actually painted one of giRestirIndirect's own seven colours over
    // ind.diffuse.
    bool giDiffusePoisoned = false;
    // F4 (R1): mirrors PSMainVoxi's own restirSuppliedDiffuse -- see that copy's comment (search
    // gAmbientParams.z's cbuffer entry, R1) for the full identity. True only when the ReSTIR branch
    // below actually supplied ind.diffuse, which is the only estimator that double-counts this
    // receiver's own sky.
    bool rdRestirSuppliedDiffuse = false;
    // Mirrors PSMainVoxi's aoGathered: true only once coneTracedIndirect has written rdAo.
    bool rdAoGathered = false;
#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no cone gather
#elif AVER_AO_UNIFIED
    // NOT TRACED HERE AT ALL under the unified ambient ray -- the hemisphere gather below supplies
    // this term from its own hits, and running both would double every interior's bounce light. The
    // cones and the ray answer the SAME question (how much light arrives from the surfaces around
    // this point); the difference is that the ray stopped on geometry while the cone averaged across
    // it. See the ambient block further down, which assigns ind.diffuse.
#else
    // GIMODE'S OWN SWITCH, exactly as PSMainVoxi's copy of this branch: gGiRestirParams.x (never the
    // raw Settings::giMode -- see giRestirIndirect's own header comment) says whether
    // VoxiRenderer::giRestirWanted() actually bound t12/u6/u7 this frame, and this whole function
    // only exists inside `#if AVER_RT` already, so there is no non-RT variant to keep bit-identical
    // here the way PSMainVoxi's copy has to guard for.
    if (gVoxelParams.w > 0.5) {
        if (gGiRestirParams.x > 0.5) {
#if AVER_RD_SPLIT
            // STAGE B: read CSRdGi's already-resolved diffuse estimate instead of calling
            // giRestirIndirect itself -- CSRdGi ran that same call for this pixel (see its own body,
            // further down this file). rdAo is left untouched, at its own initial 1.0: that is exactly
            // what giRestirIndirect's own `ao` out-param would have set it to too (see that function's
            // header), so this branch and the #else below leave rdAo in the same state either way.
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
    // an opaque hit under glass in ray-driven mode was a flatter answer than PSMainVoxi's own sharp
    // mirror ray for a qualifying surface -- not a compositing defect (blend/depth/Fresnel unchanged),
    // but the "broad, flat sheet" look the glass/reflection audit traced to this branch.
    //
    // THE OLD COST OBJECTION ("a fourth ray on every pixel") doesn't survive gating identically to
    // PSMainVoxi (`s.rough <= 0.75`): this pays the SAME ray, on the SAME subset of surfaces, the
    // rasteriser already prices into the comparison this mode exists to make. Rough surfaces (most of
    // a scene) still take the cheap cone/sky path unmodified.
    //
    // THE GATE IS PSMainVoxi's PREDICATE VERBATIM (`gShadowParams.z > 0.5 && gRtParams.w > 0.5 &&
    // s.rough <= 0.75`), so the two passes agree on which surfaces earn a mirror ray -- both terms
    // are already true whenever this pass runs at all (rayDrivenActive() requires rtActive_, which
    // sets gShadowParams.z; this pass already reads the geometry table unconditionally above). Kept
    // anyway as the one place either pass is told "no" if the geometry table ever legitimately fails
    // while the TLAS still exists.
    //
    // rtReflectionTemporal's dzdx/dzdy reuse the shadow footprint's ray differential rather than
    // rederiving one: they only predict a REPROJECTED NEIGHBOUR's depth for rtReflectionSpatial's
    // plane-rejection (a filter-quality term, not one that can misplace a ray), so they skip the
    // tangent-plane projection dpx/dpy needed. `rdRayDx`/`rdRayDy` (already built for the shadow call)
    // give the exact displacement wanted; `mul(float4(rdRayDx,0.0), gViewProj).w` reads the
    // DIRECTIONAL part of the same clip.w PSMainVoxi's ddx(viewZ) approximates -- exact here, one
    // extra directional matrix multiply, far cheaper than a second ray differential.
    //
    // EXPECTED COST, STATED not measured (no build/launch under this task's rule): rtReflectionTemporal
    // shares its tile schedule with the shadow ray this pass already pays for, so a smooth pixel's
    // reflection amortises the same way, only where `s.rough <= 0.75` (a minority of most scenes).
    // Nearest reference: the wave-bound-shadow finding's ~2.0ms for one tile-amortised full-screen
    // ray-query pass on ElectricDreams at full coverage; gated to a roughness minority, real cost
    // should land well under that.
    //
    // B1 (F5): set below, inside this branch only -- see PSMainVoxi's identical copy for why only the
    // RAY-TRACED specular term gets this marker, and aver_IsGiRestirPoisonColour's own comment for
    // the precedence against giDiffusePoisoned above.
    bool giPoisonSpecCeilHit = false;
#if AVER_RD_SPLIT
    // MILESTONE 3, STAGE B: read CSRdRefl's already-resolved reflection instead of re-deciding
    // roughness and calling rtReflectionTemporal here -- CSRdRefl ran that same gate and that same
    // call for this pixel, from its own roughness-only reconstruction (rdSurfaceRoughness, declared
    // alongside rdSurfaceFromRecord above). THE SPLIT COMPILE MUST CONTAIN NO CALL TO
    // rtReflectionTemporal -- removing it from this pass is the point (register pressure) -- so this
    // branch reads a texel instead of tracing.
    //
    // THE GATE HERE IS DELIBERATELY MISSING ITS ROUGHNESS TERM: it repeats only
    // `gShadowParams.z > 0.5 && gRtParams.w > 0.5` (never s.rough <= 0.75) because rdRefl.a already
    // encodes that CSRdRefl's OWN gate (which does include roughness) passed for this pixel -- see
    // gRdReflTex's own header comment, "THE STAGE'S OWN DECISION", for why reading that alpha is not
    // an approximation of the roughness test but IS the roughness test's already-computed answer.
    // What is repeated here only guards the texture itself: on a frame where the CPU never dispatched
    // CSRdRefl at all (this same condition false), gRdReflTex may hold a stale or placeholder texel,
    // exactly the reason CSRdGi's and CSRdSkyOcc's own AVER_RD_SPLIT reads above repeat their own
    // dispatch conditions the same way.
    const float4 rdRefl = (gShadowParams.z > 0.5 && gRtParams.w > 0.5)
                         ? gRdReflTex[uint2(i.pos.xy)] : float4(0.0, 0.0, 0.0, 0.0);
    if (rdRefl.a > 0.5) {
        // B1 (F5): CSRdRefl's own PRE-clamp ceiling test, carried in alpha (2.0 = traced AND over the
        // ceiling) -- NOT recomputed from rdRefl.rgb, which is already clamped and half-float rounded,
        // so a test against it could disagree with the single pass's test against the unclamped value.
        giPoisonSpecCeilHit = rdRefl.a > 1.5;
        ind.specular = rdRefl.rgb;
    } else if (gVoxelParams.w > 0.5) {
        // PSMainVoxi's OWN voxel-cone fallback, for the surfaces PSMainVoxi itself falls back for
        // (rough > 0.75, or RT unavailable): past that roughness a one-ray estimate can't resolve a
        // near-hemispherical lobe regardless of which pass is asking. DUPLICATED FROM THE #else
        // BRANCH'S IDENTICAL COPY below rather than shared across the #endif, for the same "no shared
        // statement" reason PSRayDriven's own trace block above gives (search "no shared statement")
        // -- this whole chain must stay easy to prove byte-identical to the pre-milestone-3 shape when
        // AVER_RD_SPLIT is 0, which a statement straddling this preprocessor boundary would not be.
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
        float3 refl = rtReflectionTemporal(wpos, N, R, L, i.pos.xy, s.rough,
                                           rdReflDzdx, rdReflDzdy, specHit);
#endif
        // Same guard as PSMainVoxi's twin (see there for the 13% regression an earlier two-call
        // version cost): smoothstep(0.5,0.75,rough) is 0 at/below 0.5, so a hit reduces the lerp to
        // `refl` and the march is multiplied by zero.
        // THIS ONE IS THE WHOLE SCREEN: PSRayDriven answers primary visibility for every pixel by
        // default, so every smooth surface was paying a march it then discarded.
        const float skyW = smoothstep(0.5, 0.75, s.rough);
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march
#else
        if (!specHit || skyW > 0.0) skyR = skyColor(R);
#endif
        // CLAMPED, AND THIS WAS THE ONLY UNBOUNDED TERM LEFT IN A SHADOWED PIXEL. Its cone-traced
        // twin eighteen lines below already does min(sceneSpec.rgb * gVoxelParams.y,
        // AVER_VOX_MAXRAD); the ray branch had no ceiling at all, and rtReflection returns
        // reflAlbedo * (direct + ambient) with nothing bounding it.
        //
        // WHY IT SHOWS IN SHADOW SPECIFICALLY: where the sun does not reach, diffAmbient is
        // multiplied by the occlusion and goes to zero, and diffBounce is MAXRAD-clamped on both of
        // its routes. specEnv is the term left standing, and averIndirectTerms' FssEss and specOcc
        // are both bounded by ~1, so it inherits this magnitude unchanged. One reflection ray
        // escaping a dark interior through a window was the entire pixel -- which is exactly the
        // measured shape: the residual outliers are predominantly BRIGHT, and their rate is 49x
        // higher in dim regions than with ray tracing off.
        //
        // clamp() rather than the twin's min(), deliberately. min bounds above only, and this tree
        // has a recorded incident class where a NEGATIVE radiance rendered wrong -- historically
        // BRIGHT, because acesTonemap(-1) used to equal 1.0; acesTonemap now floors input at zero
        // (color.hlsli, since ded8784a) so the same mistake would instead render as confident BLACK,
        // not white -- silent rather than alarming, which is if anything a stronger reason to floor
        // it HERE, at the point this value is computed, rather than leave it to whatever the tonemap
        // happens to do with it. The voxel injection's own write already uses this two-sided form.
        //
        // B1 (F5): same PRE-clamp ceiling test as PSMainVoxi's copy -- see there for why `>=` (not a
        // negated `<`) is the NaN-safe form and why a NaN component here needs no separate guard.
        const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
        giPoisonSpecCeilHit = any(specRaw >= AVER_VOX_MAXRAD);
        ind.specular = clamp(specRaw, 0.0, AVER_VOX_MAXRAD);
    } else if (gVoxelParams.w > 0.5) {
        // PSMainVoxi's OWN voxel-cone fallback, for the surfaces PSMainVoxi itself falls back for
        // (rough > 0.75, or RT unavailable): past that roughness a one-ray estimate can't resolve a
        // near-hemispherical lobe regardless of which pass is asking.
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
#endif
    // REAL AMBIENT OCCLUSION NOW, from the same cone march as the diffuse term. Used to be a
    // hardcoded 1.0 ("a traced bounce is its own occlusion" -- true for a converged path tracer, not
    // for one fixed sample per pixel). Cone trace supplies both now, matching the raster path.
    //
    // AT EPIC `rdAo` IS COMPUTED AND THEN DISCARDED, and that is deliberate rather than an oversight
    // left lying around. The line below prefers the traced sky visibility, so the cone gather's own
    // occlusion goes unused on exactly the tier that pays most for it. It is NOT worth restructuring
    // coneTracedIndirect to skip: the cones still have to be traced for `ind.diffuse`, so the only
    // saving available is the per-cone `occ += c.a * w` accumulation -- roughly two FMAs times
    // thirteen cones, against thirteen cone MARCHES of up to 24 volume samples each. Measured
    // context: the whole cone gather is 4.05 ms of a 50.16 ms frame, and this is a rounding error
    // inside that. Threading a `wantAo` flag through a function mirrored in two files (voxi.hlsl and
    // voxi_gi.hlsli, byte-for-byte) to save it would cost more than it returns.
    // Same substitution as PSMainVoxi's, and it has to be the same or the two primary-visibility
    // paths would disagree about how much sky reaches a surface -- they currently agree to 2.23 MAD
    // and that is worth keeping. Guarded for the same compile-time reason, and kept even though this
    // pass only exists under ray tracing: a reader should not have to prove that to know this builds.
#if AVER_RT && AVER_AO_UNIFIED && AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    // ONE GATHER, THREE ANSWERS. See AVER_AO_UNIFIED at the top of this file for the argument.
    if (gAmbientParams.x > 0.5) {
        const AverAmbientTraced amb = rtAmbientTraced(wpos, N, i.pos.xy, (uint)gAmbientParams.x);
        ind.occlusion = amb.open;
        // The same write rtSkyOcclusionTemporal makes, because this branch REPLACES that call
        // rather than wrapping it -- a build with AVER_AO_UNIFIED on would otherwise leave the hit
        // distance target holding whatever the last frame with it off had written. Guarded
        // explicitly since, unlike there, no early-out has already tested the flag here.
        if (gRtDenoiseParams.w > 0.5) gAoHitDistOut[uint2(i.pos.xy)] = amb.hitDist;
        // THE BOUNCE REPLACES THE CONE GATHER, and carries its own occlusion already: averIndirectTerms
        // computes diffBounce as kD * ind.diffuse with NO occlusion factor, which is exactly right for
        // a quantity gathered by rays that were themselves occluded.
        ind.diffuse   = amb.bounce;
        // DIVIDED BY `open` ON PURPOSE, and getting this wrong would darken every shadowed pixel by
        // squaring the occlusion. amb.sky is ALREADY the occluded sky -- the mean over all n samples,
        // where a blocked sample contributed zero -- but averIndirectTerms then multiplies ind.ambient
        // by diffOcc = ind.occlusion * s.occlusion. Pre-dividing here means that multiply puts the
        // occlusion back exactly once: (sky/open) * open == sky.
        //
        // WHY NOT SET ind.occlusion = 1 INSTEAD, which would look simpler: ind.occlusion is also read
        // by averSpecularOcclusion for the specular lobe, and by the material's own s.occlusion map.
        // Flattening it would silently unocclude both.
        //
        // The guard is for the fully-enclosed pixel: open == 0 means sky == 0 too, so the quotient is
        // 0/0 and any finite stand-in gives the correct 0 after the multiply back.
        ind.ambient   = amb.sky / max(amb.open, 1e-4);
    } else {
        ind.occlusion = rdAo;
    }
#elif AVER_RT && AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    ind.occlusion    = gAmbientParams.x > 0.5
#if AVER_RD_SPLIT
                     // STAGE B: in the two cases CSRdSkyOcc actually ran for (ReSTIR supplies diffuse,
                     // or there is no voxel GI at all -- CSRdSkyOcc's own header has the exact gate),
                     // read its already-resolved answer instead of tracing again. In CONE-GI mode
                     // CSRdSkyOcc never wrote this pixel's texel (occlusion rides the cone gather's own
                     // accumulator instead), so this falls through to the SAME rtSkyOcclusionTemporal
                     // call the non-split branch below makes -- the shade pass, not a dedicated stage,
                     // is what answers sky occlusion for that estimator.
                     ? ((gGiRestirParams.x > 0.5 || gVoxelParams.w <= 0.5)
                        ? gRdAoTex[uint2(i.pos.xy)].r
                        : rtSkyOcclusionTemporal(wpos, N, i.pos.xy, (uint)gAmbientParams.x, rdAo,
                                                 rdAoGathered, false))
#else
                     // false: THIS pass runs with the G-buffer OFF -- that is what selects it -- so
                     // gNrdAo was reprojected against motion vectors and depth this pass never
                     // wrote, and its answer is not about this frame. Reading it anyway was the
                     // whole of the washed-out ray-driven shadows: it overrode a correctly traced
                     // "fully occluded" with ~0.83 "open", and full sky ambient then landed on every
                     // interior surface. Measured in rtSkyOcclusionTemporal's own header.
                     ? rtSkyOcclusionTemporal(wpos, N, i.pos.xy, (uint)gAmbientParams.x, rdAo,
                                              rdAoGathered, false)
#endif
                     : rdAo;
#else
    // ablated (or no ray tracing): the cone gather's own occlusion, which is what every tier below
    // Epic uses anyway -- so this mode measures the RAY, not the presence of ambient occlusion.
    ind.occlusion    = rdAo;
#endif
    // F4 (R1): PSMainVoxi's twin, applied here after ind.occlusion is final and before
    // averShadeIndirect reads ind -- see ind4's copy above (search "ONE OWNER FOR THE SKY") for the
    // full identity and why it is a subtraction rather than zeroing ind.ambient. Never set inside the
    // AO_UNIFIED branch above: that branch's ind.diffuse (amb.bounce) is the traced bounce ALONE, with
    // its own occlusion already folded in and no sky term riding along with it, so
    // rdRestirSuppliedDiffuse stays false there and this is a no-op for that tier.
    if (rdRestirSuppliedDiffuse && !giDiffusePoisoned && ((uint)gAmbientParams.z & 2u) == 0u)
        ind.diffuse -= ind.ambient * ind.ambientScale * ind.occlusion * s.occlusion * gVoxelParams.y;
    radiance = averShadeIndirect(radiance, s, ind);

    // THE BOUNCE CARRIES THE DIFFUSE RESPONSE, not raw albedo: a metal reflects almost nothing
    // diffusely, so throughput*basecolour would light an interior off surfaces that don't bounce it.
    float3 throughput = s.kdAlbedo;
    float3 bp = wpos;
    float3 bn = N;
    // SKIPPED ENTIRELY WHENEVER THE CONE TRACE ALREADY ANSWERED THIS -- a correctness requirement:
    // both compute the SAME surface-to-surface bounce, so running both would double every interior's
    // brightness. Left reachable for GI-off and for measuring the estimator, but it is ONE cosine
    // sample per pixel per bounce from a hash that can't vary by frame, with no history to accumulate
    // into -- correct but far too undersampled to be an image until it gets per-frame decorrelation
    // and a history buffer it doesn't have. Until then the cone trace above is the better answer.
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

        RtInstance bi = gRtInstances[qb.CommittedInstanceID()];
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

    // Depth for everything that draws AFTER the scene -- the deferred sky, transparentPass, the
    // particle pass. Without it they have nothing to test against and sort against a cleared
    // buffer, which puts smoke in front of walls.
    float4 clip = mul(float4(wpos, 1.0), gViewProj);
    o.depth = clip.w > 1e-6 ? saturate(clip.z / clip.w) : 1.0;
    // UNLIT SUBSTITUTES THE COLOUR AND NOTHING ELSE. Handled here rather than through
    // gShadingModel because a ray hit has no per-draw cbuffer -- the raster path carries the mode in
    // the b1 block PSMainVoxi reads, and this pass never binds it; that asymmetry is why the mode
    // reached the rasteriser and not the renderer that draws the scene by default.
    //
    // AN OVERRIDE RATHER THAN AN EARLY RETURN, deliberately: every AVER_GBUFFER channel below still
    // has to be written, and returning above them would leave velocity, viewZ, normal-roughness and
    // SV_DEPTH unwritten -- the exact "fills only some outputs" fault the struct's own comment warns
    // both return sites about. s.albedo is the SAMPLED base colour; shading a base-colour CONSTANT
    // is what made this mode pure white on every textured mesh.
    o.col   = float4(gViewParams.x > 0.5 ? s.albedo : radiance, 1.0);
    // B1 (F5): applied LAST, after the unlit substitution just above and after fog/shading upstream,
    // so this is unconditionally the final colour whenever it fires -- see PSMainVoxi's identical
    // override for the precedence against giDiffusePoisoned (a giRestirIndirect colour on the diffuse
    // channel wins), and aver_IsGiRestirPoisonColour's own comment for why. Takes precedence over the
    // unlit substitution too: giPoisonView is an explicit diagnostic the user turned on by hand, and
    // it should not go dark just because unlit view is also active.
    if (gGiRestirParams.w > 0.5 && giPoisonSpecCeilHit && !giDiffusePoisoned)
        o.col.rgb = float3(0.55, 0.0, 1.0);   // VIOLET: ray-traced specular hit AVER_VOX_MAXRAD
#if AVER_GBUFFER
    // clip.w IS the view-space linear depth viewZ wants, reused from o.depth's divide above rather
    // than a second mul. Velocity uses the SAME static-geometry function as PSMainVoxi (see
    // averGBufferVelocity for what it doesn't yet handle); normal is this pass's ray-hit N, not an
    // interpolated vertex normal -- what this feature's task asked for.
    o.velocity        = averGBufferVelocity(wpos);
    o.viewZ            = clip.w;
    o.normalRoughness  = averPackNormalRoughness(N, s.rough);
#endif
    return o;
}

// ---- STAGED RAY-DRIVEN COMPUTE STAGES: pixel -> NDC -> primary-ray direction, ONE PLACE ------------
//
// Pixel-centre NDC, the exact inverse of the ndc->pixel mapping this file and voxi_rt.hlsli already
// use everywhere (this file's own rtReprojectReflection; voxi_rt.hlsli's rtReprojectHistory/
// rtReprojectAo/rtAoSpatial/rtShadowSpatial): px = viewport.xy + float2(ndc.x*0.5+0.5,
// 0.5-ndc.y*0.5) * viewport.zw. Solved for ndc at this pixel's CENTRE (pixel + 0.5), it equals what
// VSky/SkyOut (modules/rhi/shaders/shared_prelude.hlsl:967-975) interpolates there: VSky emits
// o.ndc = uv*2-1 with o.pos = float4(o.ndc, 1, 1), so the rasteriser's own NDC-to-viewport transform
// is exactly the forward direction of the formula above, and this is its inverse.
//
// gSceneViewportCur, NOT gSceneViewport: the latter is LAST frame's rect (paired with gPrevViewProj,
// for reprojection), and on any frame the docked view is resized or rescaled it would map a stage onto
// the old pixel grid while Stage B shades the new one. The rect the rasteriser used for i.ndc/i.pos.xy
// THIS frame is this one. (A shadow-ray FOOTPRINT still needs LAST frame's grid on purpose -- see
// CSRdShadow's own neighbour-ray step, which keeps gSceneViewport separately and is untouched by this
// helper.)
//
// FACTORED OUT OF CSRdVisibility AND CSRdShadow, which each inlined this exact sequence before this
// task -- ONE function now, used by all four staged compute stages (CSRdVisibility, CSRdShadow,
// CSRdGi, CSRdSkyOcc, all further down this file), so a future change to the mapping cannot update
// three of the four and silently disagree in the fourth. The math is byte-for-byte what each of the
// first two already computed. `ndc` comes back alongside `dir` because CSRdShadow's own shadow-ray
// footprint (and PSRayDriven's) needs it for the neighbour-ray reconstruction, not because this
// function does anything with it itself.
float3 rdPrimaryRayDir(uint2 pixel, out float2 ndc) {
    const float2 pxC = float2(pixel) + 0.5;
    ndc.x = (pxC.x - gSceneViewportCur.x) / max(gSceneViewportCur.z, 1.0) * 2.0 - 1.0;
    ndc.y = 1.0 - (pxC.y - gSceneViewportCur.y) / max(gSceneViewportCur.w, 1.0) * 2.0;

    // Same NDC-to-world-ray reconstruction as PSRayDriven's own primary ray (and PSVoxelDebug's),
    // through the same gInvViewProj.
    float4 far = mul(float4(ndc, 1.0, 1.0), gInvViewProj);
    return normalize(far.xyz / far.w - gCamPos.xyz);
}

// ---- STAGE A: CSRdVisibility -- trace the primary ray, write the visibility record -----------------
//
// The visibility-only half of PSRayDriven's own trace block above (AVER_RD_SPLIT==0 branch): same
// mask, same cutout handling, same ray. Nothing past a committed hit or a miss is computed here --
// Stage S (CSRdShadow, immediately below) and Stage B (PSRayDriven's AVER_RD_SPLIT branch, above) do
// the reconstruction, from this dispatch's own gRdVisBuf write, through rdSurfaceFromRecord.
//
// D3D12 ONLY FOR NOW, per this feature's own interface contract -- VoxiRenderer decides whether to
// dispatch this at all (falls back to the single pass otherwise), not this file.
[numthreads(8, 8, 1)]
void CSRdVisibility(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    // pitch 0 means the record buffer has nowhere well-defined to put this pixel (VoxiRenderer writes
    // a nonzero pitch only while it actually means to run the staged path this frame) -- bail rather
    // than guess an index.
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

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Same lane as PSRayDriven's own primary ray -- see that function's header comment for why
    // AVER_RT_MASK_OPAQUE, not _OPAQUE_ALL, is right for a primary ray leaving the viewer's own head.
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE, r);
    averRtProceedSolid(q);

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        gRdVisBuf[idx] = uint4(0xFFFFFFFFu, 0u, 0u, 0u);
        return;
    }

    const float2 bary = q.CommittedTriangleBarycentrics();
    gRdVisBuf[idx] = uint4(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                           asuint(bary.x), asuint(bary.y));
}

// THE SHADOW-RAY FOOTPRINT, factored out of CSRdShadow so CSRdShadowProbe (below) can build the exact
// same dpx/dpy a fresh probe ray needs, rather than a third copy of this derivation (PSRayDriven's own
// "THE SHADOW-RAY FOOTPRINT" comment, and CSRdShadow's copy below it before this factoring, are the
// other two). gSceneViewport, not Cur -- matches PSRayDriven's own copy of this step exactly.
void rdShadowFootprint(float2 ndc, float3 dir, RdSurface s, out float3 dpx, out float3 dpy) {
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float4 farDx = mul(float4(ndc + float2(ndcPixelStep.x, 0.0), 1.0, 1.0), gInvViewProj);
    float3 dirDx = normalize(farDx.xyz / farDx.w - gCamPos.xyz);
    float4 farDy = mul(float4(ndc + float2(0.0, ndcPixelStep.y), 1.0, 1.0), gInvViewProj);
    float3 dirDy = normalize(farDy.xyz / farDy.w - gCamPos.xyz);
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
// A (Settings::rayDrivenShadowTiles). Dispatched over the SAME (gx, gy) grid as CSRdShadow, immediately
// before it: one thread GROUP is one 8x8 tile of the viewport (tile = SV_GroupID.xy, matching
// gRdShadowTiles' own tileIdx layout), and every thread in it traces at most one probe ray -- always
// ONE ray, never the Epic-tier disc CSRdShadow's own AVER_RD_SHADOW_TILES compile traces per pixel --
// then the group reduces its 64 answers to one mask. CSRdShadow's tiled compile ORs this tile's mask
// together with its 3x3 neighbourhood and skips its own ray loop wherever every probe in that
// neighbourhood agrees: see that compile's own header comment for why 3x3, not this tile alone.
//
// NO EARLY RETURN ANYWHERE IN THIS FUNCTION, BEFORE OR BETWEEN THE TWO GroupMemoryBarrierWithGroupSync
// CALLS BELOW: an out-of-viewport thread, a pitch-0 frame, and a sky pixel are all real cases (the
// viewport is rarely an exact multiple of 8), and a `return` before a barrier every OTHER thread in the
// group still executes is undefined behaviour, not merely "this thread's own contribution is skipped".
// Each of those cases instead leaves `bit` at its 0 default and falls through to the same reduction as
// every other thread -- 0 ORs into the group mask as a no-op, so it costs nothing but a branch.
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
            float3 dpx, dpy;
            rdShadowFootprint(ndc, dir, s, dpx, dpy);

            const float3 L = normalize(gLightDir.xyz);
            // THE SAME JITTER rtShadowTemporal's OWN NON-TILED BRANCH PASSES (voxi_rt.hlsli) for this
            // exact (gRtHistParams.x, frame index) pair -- this probe ray has to agree with what
            // CSRdShadow's own fresh trace would have drawn, or a tile's "every probe agrees" verdict
            // would be classifying a DIFFERENT sample than the one it is standing in for.
            const float jitter = (gRtHistParams.x < 0.5) ? 0.0
                                : (float)((uint)gRtHistParams.z) * 2.39996323;
            // WHICH OF THE PIXEL'S OWN SAMPLES THE PROBE TRACES -- ROTATED, NOT ALWAYS THE FIRST.
            // Sample 0 sits at a FIXED radius, sqrt(0.5) of the sun disc, whatever the pixel or frame
            // (rtDiscSample; the same trap the F1 comment in voxi_rt.hlsli records for the GI and sky
            // rays). A probe that only ever traced it could not see an occluder covering less than
            // ~9% of the disc at the ends of a soft penumbra. Every probe in a 3x3-tile neighbourhood
            // would then agree, and at High/Epic CSRdShadow would skip rays that DO see it,
            // hardening the edge tile-by-tile. Rotating through samples 0..rays-1 by pixel and frame
            // puts every radius the real trace uses (0.25 to 0.94 of the disc at 8 rays) into every
            // tile. The probe is then always one of the real trace's own rays: identical to it at 1
            // ray, and covering its radii at 4 and 8. The (x + 3y) step keeps neighbours in a row and
            // in a column on different samples.
            const uint rays   = (uint)max(gRtParams.y, 1.0);
            const uint kProbe = (pixel.x + 3u * pixel.y + (uint)gRtHistParams.z) % rays;
            const float3 fresh = rtShadowEx(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy, 1u, jitter,
                                            kProbe);

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
// Reads CSRdVisibility's record, rebuilds the surface through rdSurfaceFromRecord (the same
// reconstruction Stage B uses), and runs the SAME rtShadowTemporal call PSRayDriven's single pass
// makes -- same pixel-centre argument, same footprint, same ray count -- so the history buffer
// rtShadowTemporal reads and writes means the same thing whichever path is running.
//
// COMPILED AT SM 6.6 (VoxiRenderer), because rtShadowTemporal's reprojection and spatial filter take
// ddx/ddy of depth and compute shaders only have derivatives from 6.6 on. 8x8 threads make 6.6 form
// 2x2 quads, the pixel shader's own neighbourhood. As in the single pass, a quad with a lane that
// returned early (a sky pixel, the viewport's last odd row) has undefined derivatives.
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
        // (its own miss check returns before reaching the sunVis read), but a defined, fully-lit
        // value here costs nothing and leaves no uninitialised texel behind.
        gRdSunVisTex[pixel] = float4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    // Same pixel-centre NDC and primary-ray reconstruction as CSRdVisibility -- see rdPrimaryRayDir's
    // own header for the derivation. Needed again here (not carried in the record) to rebuild `dir`
    // for rdSurfaceFromRecord's face-the-ray normal flip and for this shadow ray's own footprint.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // THE SAME SHADOW-RAY FOOTPRINT PSRayDriven builds for its own shadow call -- see rdShadowFootprint's
    // own header for the derivation (factored out, above, so CSRdShadowProbe can build the identical
    // footprint for its own probe ray without a third copy of this block).
    float3 dpx, dpy;
    rdShadowFootprint(ndc, dir, s, dpx, dpy);

    const float3 L = normalize(gLightDir.xyz);

    // W6/M5: EXPLICITLY TRUE -- same reason PSRayDriven's own copy of this line gives (this function's
    // header comment, just above PSRayDriven): a blended (glass/water) draw never reaches the
    // ray-driven primary at all, so every history write this call makes is always live for this pass.
    gAverHistoryWrite = true;

#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    const float3 sunVis = float3(1.0, 1.0, 1.0);   // ablated: fully lit, no ray -- matches PSRayDriven's own ablated branch
#else
#if AVER_RD_SHADOW_TILES
    // A3: PROBE-GUIDED SKIP -- OR the 3x3 tile neighbourhood CSRdShadowProbe already classified around
    // THIS pixel's own tile (tid.xy is viewport-local, so /8 is the tile the primary dispatch grid --
    // shared with CSRdShadowProbe's own -- already puts this thread in). The mask depends only on
    // SV_GroupID, not on any per-thread data, so this branch is wave-uniform: every lane in the tile
    // takes the same side of it. Exactly 2 (every probe in the neighbourhood lit) or exactly 1 (every
    // probe blocked) skips the ray loop and hands the probes' own verdict straight to the SAME temporal
    // accumulation, history write and spatial filter every other pixel still runs -- see
    // rtShadowTemporalEx's own header for why that is safe. Anything else (a probe disagreed, or the
    // 3x3 OR mixes lit and blocked tiles) falls through to today's unabridged path.
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
    // inline the whole temporal/spatial/trace body twice into a pass that is already register-bound.
    const bool   probeAgrees = (m == 2u || m == 1u);
    const float3 sunVis = rtShadowTemporalEx(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy,
                                             (uint)max(gRtParams.y, 1.0), probeAgrees,
                                             float3(1.0, 1.0, 1.0) * (m == 2u ? 1.0 : 0.0));
#else
    const float3 sunVis = rtShadowTemporal(s.wpos, s.N, L, float2(pixel) + 0.5, dpx, dpy,
                                           (uint)max(gRtParams.y, 1.0));
#endif
#endif
    gRdSunVisTex[pixel] = float4(sunVis, 1.0);
}

// ---- STAGE G0: CSRdGiTrace -- trace ReSTIR GI's fresh candidate for CSRdGi's own resample to read back
//
// B: GI CANDIDATE TRACE/RESAMPLE SPLIT (Settings::rayDrivenGiSplit). Runs giTraceInitialCandidate for
// every pixel CSRdGi's own (non-split) copy would have traced it for -- same surface reconstruction,
// same frameJitter, and the SAME f2Path/rho2 giDecodePaths (voxi_restir.hlsli, B2) computes for this
// pixel there too -- and stores every one of its out params in gRdGiCand (voxi_restir.hlsli, B1).
// CSRdGi's own AVER_GI_SPLIT=1 compile then reads that record back inside giRestirIndirect instead of
// tracing again: see that compile's own header, and giRestirIndirect's AVER_GI_SPLIT branch, for why
// the two agree bit-for-bit (full float precision through the hand-off, no quantisation).
//
// NON-CHECKERBOARD COMPILE: one thread per pixel, the same (gx, gy) grid and pixel mapping CSRdGi
// itself dispatches over.
//
// AVER_GI_CHECKERBOARD COMPILE (milestone 4): COMPACTED, not the full grid with half its lanes idle --
// CSRdGi's own checkerboard branch only traces the pixels satisfying (x ^ y ^ parity) & 1 == 0, so this
// dispatch is issued over ceil(w/2) x h threads instead and reconstructs exactly that half's pixel
// coordinates from tid, the SAME `parity` bit CSRdGi reads out of gViewParams.w (see that compile's own
// comment on why it is packed there). gGiCbSkip is forced false for every dispatched thread: unlike
// CSRdGi, which runs over every pixel and reads gGiCbSkip to decide whether IT traces, this dispatch by
// construction only ever covers the half that does.
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

    // NO gAverHistoryWrite HERE, UNLIKE CSRdShadow/CSRdGi's OWN COPIES OF THIS LINE: this dispatch
    // never calls giRestirIndirect (only giTraceInitialCandidate, whose one shadow ray is a plain
    // rtShadow call and reads no history), so the flag has nothing to gate in this function. CSRdGi's
    // own AVER_GI_SPLIT compile still sets it, unconditionally, before ITS call to giRestirIndirect --
    // see that call site for why.
    const uint frameIdx = (uint)gRtHistParams.z;
    const GiPathDecode gd = giDecodePaths(s.wpos, s.N, float2(pixel) + 0.5, frameIdx);

    float3 pos, nrm, rad;
    bool   nonFinite   = false;
    float  f2LumTraced = 0.0, f2LumSky = 0.0;
    bool   f2Observed  = false;
    const bool ok = giTraceInitialCandidate(s.wpos, s.N, float2(pixel) + 0.5, frameIdx * 2.39996323,
                                            pos, nrm, rad, nonFinite, gd.f2Path, gd.rho2,
                                            f2LumTraced, f2LumSky, f2Observed);

    // EVERY OUT PARAM PLUS THE BOOL, ALWAYS -- whatever giTraceInitialCandidate returned, so CSRdGi's
    // own AVER_GI_SPLIT read (voxi_restir.hlsli) has a defined record for every pixel it might read,
    // not only the ones that produced a usable candidate.
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
// single pass makes when ReSTIR GI is the active diffuse estimator -- same pixel-centre argument, so
// the reservoir and surface-history buffers giRestirIndirect reads and writes (gGiReservoirs/
// gGiSurfPosHist/gGiSurfNrmHist, u6/u7/u8, plus the NRD GI pair u9/t15 and the half-res visibility pair
// u10/t16) mean the same thing whichever path is running.
//
// ONLY MEANINGFULLY DISPATCHED WHEN ReSTIR GI IS ACTUALLY THE CHOSEN ESTIMATOR THIS FRAME
// (VoxiRenderer::recordStagedRayDriven mirrors the same `gVoxelParams.w > 0.5 && gGiRestirParams.x >
// 0.5` test on the CPU before issuing this dispatch, from the SAME cb_ values uploaded to this shader).
// The cone-traced branch of PSRayDriven's GI block is UNTOUCHED by this stage and still runs inside
// Stage B itself: coneTracedIndirect has no per-pixel history of its own to split out this way.
//
// COMPILED AT SM 6.6 (VoxiRenderer), same defines as CSRdShadow. giRestirIndirect and everything it
// calls (giTraceInitialCandidate, the RAB_* adapter, the vendored RTXDI resampling headers) use no
// derivative intrinsic themselves -- every texture fetch on a traced hit goes through averRtSampleSlot
// with an explicit SampleLevel/gradient, never implicit ddx/ddy -- so this stage does not strictly need
// 6.6 for that reason the way CSRdShadow does; it is dispatched through the same staged pipeline object
// as the other three stages regardless, so it shares their shader model rather than inventing a fourth.
//
// MILESTONE 4 (voxi.rayDrivenStages == 2): this stage alone is ALSO compiled with AVER_GI_CHECKERBOARD=1
// -- half-rate ReSTIR GI, tracing a fresh candidate for only the pixel half NRD's REBLUR expects fresh
// data from this frame (giCbParity below), and leaving REBLUR to reconstruct the other half. The
// checkerboard skip itself is decided here (gGiCbSkip) but everything it changes lives inside
// giRestirIndirect (voxi_restir.hlsli, its own AVER_GI_CHECKERBOARD sections) -- this stage's own call
// below is unchanged either way.
[numthreads(8, 8, 1)]
void CSRdGi(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

#if AVER_GI_CHECKERBOARD
    // HALF-RATE GI'S OWN PARITY, NOT THE ROW PITCH -- read bit 16 of the raw cbuffer field directly
    // (rdRowPitch() above already masked it away). CONTRACT: NRD's REBLUR, checkerboardMode BLACK, has
    // data where Sequence::CheckerBoard(pixelPos, frameIndex) == (x ^ y ^ frameIndex) & 1 == 0; `pixel`
    // here IS pixelPos (this engine's NRD rect is the full render target at origin 0), so tracing
    // exactly where that expression is 0 is what makes this dispatch's write the half REBLUR expects
    // fresh data for. `giCbParity` is not this frame's own NRD frameIndex -- prePass (beginShadowHistory)
    // already ran earlier this frame and denoised LAST frame's write -- it is the frameIndex NEXT
    // frame's NRD dispatch will use to denoise THIS frame's write, which VoxiRenderer::
    // recordStagedRayDriven derives and packs into bit 16 for this one dispatch's upload only.
    const uint giCbParity = ((uint)gViewParams.w >> 16) & 1u;
    gGiCbSkip = ((pixel.x ^ pixel.y ^ giCbParity) & 1u) != 0u;
#endif

    const uint4 rec = gRdVisBuf[idx];
    if (rec.x == 0xFFFFFFFFu) {
        // A sky pixel has no surface for ReSTIR to bounce a candidate off. Stage B's own miss branch
        // (PSRayDriven's #if AVER_RD_SPLIT trace block, above) already writes the GI surface-history
        // sentinel (gGiSurfNrmHistOut) for a miss -- this stage only has to leave its own texel in a
        // defined state, not whatever the previous frame's HIT left there.
        gRdGiTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Same pixel-centre NDC and primary-ray reconstruction as CSRdVisibility -- see rdPrimaryRayDir's
    // own header for the derivation.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // W6/M5: EXPLICITLY TRUE -- same reason CSRdShadow's own copy of this line gives (search "a
    // blended (glass/water) draw never reaches the ray-driven primary"): every history write
    // giRestirIndirect makes below is always live for this pass.
    gAverHistoryWrite = true;

#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no ReSTIR GI candidate -- matches PSRayDriven's own ablated GI block (a comment, no
    // assignment: search "ablated: no cone gather"). gRdGiTex is never read back under this ablation
    // either -- PSRayDriven's own AVER_RD_SPLIT read of it lives INSIDE that same outer ablation guard
    // (further up this file), so leaving this texel untouched costs nothing.
#else
    // EXACTLY THE ARGUMENTS PSRayDriven'S OWN (non-split) COPY PASSES -- see that call, further up
    // this file, for why each one is what it is. `ao` is discarded here the same way PSRayDriven
    // discards `rdAo` for this branch: giRestirIndirect sets it to 1.0 on its first line and never
    // touches it again (see that function's own header), so Stage B's rdAo stays at its own initial
    // 1.0 whether it calls giRestirIndirect itself or reads this texture instead.
    if (gVoxelParams.w > 0.5 && gGiRestirParams.x > 0.5) {
        float ao;
#if AVER_GI_SPLIT
        // B4: THE SAME ROW-PITCH INDEX CSRdGiTrace WROTE gRdGiCand UNDER -- a `static`, not a
        // parameter, so giRestirIndirect's signature stays shared with PSMainVoxi/PSRayDriven's own
        // non-split call sites (voxi_restir.hlsli's own header comment on gGiCandIdx, right beside
        // gGiCbSkip's identical contract just above).
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
// pair (gAoHist/gAoHistOut, t11/u4) and the NRD AO hand-off (gAoHitDistOut, u5) mean the same thing
// whichever path is running.
//
// COMPILED AT SM 6.6 (VoxiRenderer), same defines as CSRdShadow: rtSkyOcclusionTemporal's own spatial
// filter (rtAoSpatial) takes ddx/ddy of depth exactly as rtShadowTemporal's does, so this stage needs
// the same derivative-capable compute shader model, 8x8 threads forming 2x2 quads.
[numthreads(8, 8, 1)]
void CSRdSkyOcc(uint3 tid : SV_DispatchThreadID) {
#if AVER_AO_UNIFIED
    // A NO-OP IN THIS BUILD. PSRayDriven's AVER_AO_UNIFIED branch computes occlusion itself through
    // rtAmbientTraced (and writes u5 itself) and never reads gRdAoTex, so running rtSkyOcclusionTemporal
    // here would only race that branch on u4/u5. The CPU still dispatches; this makes it empty.
    return;
#endif
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

    // Same pixel-centre NDC and primary-ray reconstruction as CSRdVisibility -- see rdPrimaryRayDir's
    // own header for the derivation.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // W6/M5: EXPLICITLY TRUE -- same reason CSRdShadow's and CSRdGi's own copies of this line give.
    gAverHistoryWrite = true;

    // HONOURS THE SKY-OCCLUSION ABLATION EXACTLY AS PSRayDriven's OWN #elif AVER_RT && AVER_RD_ABLATE
    // != AVER_RD_ABL_SKYOCC BRANCH DOES (further up this file, same condition, deliberately not also
    // excluding AVER_RD_ABL_ALL -- that asymmetry is PSRayDriven's existing behaviour, not introduced
    // here): this stage skips tracing in precisely the build where that branch's AVER_RD_SPLIT read of
    // gRdAoTex is itself compiled out and falls back to `ind.occlusion = rdAo`. AVER_AO_UNIFIED's own
    // sky-occlusion branch (the #if just above that #elif) is untouched by this stage -- AVER_AO_UNIFIED
    // is 0 by default and this task leaves whatever it compiles to alone.
#if AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    // EXACTLY THE ARGUMENTS PSRayDriven'S OWN (non-split) COPY PASSES for this case: coneAo=1.0,
    // coneAoIsGather=false (rdAo was never gathered on this branch), nrdAoUsable=false (same reason
    // that call gives -- this pass runs with the G-buffer off, so gNrdAo was reprojected against
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

    // Same pixel-centre NDC and primary-ray reconstruction as CSRdVisibility -- see rdPrimaryRayDir's
    // own header for the derivation.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // THE SAME FOOTPRINT RECONSTRUCTION CSRdShadow BUILDS FOR ITS OWN SHADOW CALL, byte-for-byte --
    // see that stage's own comment for the derivation. gSceneViewport, not Cur: this is a ray
    // DIFFERENTIAL (the neighbour pixel's own primary ray), the same quantity PSRayDriven's single
    // pass and CSRdShadow both reconstruct against LAST frame's grid on purpose, not this stage's own
    // dispatch rect.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float4 farDx = mul(float4(ndc + float2(ndcPixelStep.x, 0.0), 1.0, 1.0), gInvViewProj);
    float3 dirDx = normalize(farDx.xyz / farDx.w - gCamPos.xyz);
    float4 farDy = mul(float4(ndc + float2(0.0, ndcPixelStep.y), 1.0, 1.0), gInvViewProj);
    float3 dirDy = normalize(farDy.xyz / farDy.w - gCamPos.xyz);
    const float3 rdRayDx = (dirDx - dir) * s.hitT;
    const float3 rdRayDy = (dirDy - dir) * s.hitT;

    const float3 L = normalize(gLightDir.xyz);
    // PSRayDriven's OWN R -- reflect the primary ray about the (already face-the-ray-flipped) surface
    // normal rdSurfaceFromRecord produced, same as that function's own `float3 R = reflect(dir, N);`.
    const float3 R = reflect(dir, s.N);
    // THE ONE VALUE THIS STAGE NEEDS BEFORE IT CAN EVEN GATE -- see rdSurfaceRoughness's own header.
    const float rough = rdSurfaceRoughness(s, rdRayDx, rdRayDy);

    // W6/M5: EXPLICITLY TRUE -- same reason CSRdShadow's, CSRdGi's and CSRdSkyOcc's own copies of this
    // line give: a blended (glass/water) draw never reaches the ray-driven primary at all, so every
    // history write rtReflectionTemporal makes below is always live for this pass.
    gAverHistoryWrite = true;

    // THE GATE IS PSRayDriven's OWN PREDICATE, roughness included -- see that function's "ENVIRONMENT
    // SPECULAR" comment for why these three terms are the right ones. Unlike Stage S/G/O, this gate
    // cannot be dropped from the compute stage and left for Stage B to re-apply: Stage B reads this
    // gate's OUTCOME off gRdReflTex's alpha channel instead of repeating the roughness test itself --
    // see PSRayDriven's AVER_RD_SPLIT reflection branch, above, and gRdReflTex's own header comment.
    const bool rtReflTraced = gShadowParams.z > 0.5 && gRtParams.w > 0.5 && rough <= 0.75;
    if (!rtReflTraced) rtReflectionHistoryVacate(float2(pixel) + 0.5);   // see that function
    if (rtReflTraced) {
        const float rdReflDzdx = mul(float4(rdRayDx, 0.0), gViewProj).w;
        const float rdReflDzdy = mul(float4(rdRayDy, 0.0), gViewProj).w;
        bool specHit = false;
        // PENDING iff this is R1 (AVER_RD_REFL_SPLIT) AND a history is actually bound to defer the
        // gather against -- with none bound rtReflectionTemporalEx's own early-out already skips
        // rtReflectionSpatial (see that function's own comment), so there is nothing for R2 to add and
        // R1 composes here exactly as the non-split compile does. FALSE under the refl ablation
        // (AVER_RD_ABL_REFL/ALL) even when split: this contract's own "no mirror ray" branch just below
        // has no ray to defer either, so the ablated pixel keeps today's behaviour -- compose in R1, no
        // pending marker.
#if AVER_RD_REFL_SPLIT && AVER_RD_ABLATE != AVER_RD_ABL_REFL && AVER_RD_ABLATE != AVER_RD_ABL_ALL
        const bool pending = gRtHistParams.x >= 0.5;
#else
        const bool pending = false;
#endif
#if AVER_RD_ABLATE == AVER_RD_ABL_REFL || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        float3 refl = float3(0.0, 0.0, 0.0);   // ablated: no mirror ray -- matches PSRayDriven's own copy
#elif AVER_RD_REFL_SPLIT
        // R1's own half of rtReflectionTemporal: doSpatial=false still traces, blends against history
        // and WRITES gRtReflHistOut exactly as the wrapper below does -- only the dense spatial gather
        // is skipped, left for CSRdReflFilter to run against this same write. if/else, not `?:` across
        // the two calls -- same reason rtReflectionTemporalEx's own two returns spell it that way.
        float3 refl;
        if (pending) {
            refl = rtReflectionTemporalEx(s.wpos, s.N, R, L, float2(pixel) + 0.5, rough,
                                          rdReflDzdx, rdReflDzdy, false, specHit);
        } else {
            refl = rtReflectionTemporal(s.wpos, s.N, R, L, float2(pixel) + 0.5, rough,
                                        rdReflDzdx, rdReflDzdy, specHit);
        }
#else
        float3 refl = rtReflectionTemporal(s.wpos, s.N, R, L, float2(pixel) + 0.5, rough,
                                           rdReflDzdx, rdReflDzdy, specHit);
#endif
        // Same guard, same reasoning, as PSRayDriven's own copy -- see that function's "ENVIRONMENT
        // SPECULAR" comment block for the full account of why this is a lerp against skyW rather than
        // a hard branch.
        const float skyW = smoothstep(0.5, 0.75, rough);
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march -- matches PSRayDriven's own copy
#else
        if (!specHit || skyW > 0.0) skyR = skyColor(R);
#endif
        if (pending) {
            // PENDING MARKER, NOT A COMPOSE: CSRdReflFilter finishes this pixel once it has gathered
            // rtReflectionSpatial against gRtReflHistOut's write just above -- see gRdReflTex's own
            // header comment for why alpha < -0.5 is unambiguous against the three real outcomes
            // (0/1/2) a fully-composed pixel ever carries. `rough`, not `refl`, rides the alpha channel
            // (R2 gets its own `refl` back off gRtReflHistOut) -- `specHit` doesn't need to travel
            // either, R2 derives the same fact from that texel's own alpha (see its own comment).
            gRdReflTex[pixel] = float4(skyR, -1.0 - rough);
        } else {
            // CLAMPED, same ceiling PSRayDriven's own copy applies and for the same reason (see that
            // function's own comment, just above its identical line, for the unbounded-term incident
            // this guards against) -- clamp() rather than min(), so a NEGATIVE radiance floors to 0
            // rather than reading through unclamped on the low side.
            const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
            // THE .a CHANNEL IS THE STAGE'S DECISION (gRdReflTex's own header comment) -- nonzero here,
            // and ONLY here, marks this pixel as one CSRdRefl actually traced. 2.0 rather than 1.0
            // additionally carries B1 (F5)'s PRE-clamp ceiling test, `>=` for PSMainVoxi's NaN-safe
            // reason: the clamp below discards specRaw, so Stage B cannot recompute the test from rgb (a
            // clamped, half-float value) and still agree with the single pass's own test against the
            // unclamped one.
            gRdReflTex[pixel] = float4(clamp(specRaw, 0.0, AVER_VOX_MAXRAD),
                                       any(specRaw >= AVER_VOX_MAXRAD) ? 2.0 : 1.0);
        }
    } else {
        // Roughness (or the outer gate) routed this pixel to Stage B's own cone/sky fallback instead --
        // 0 in every channel, alpha included, so PSRayDriven's AVER_RD_SPLIT branch takes that fallback
        // rather than reading a stale or zeroed colour as if it were a traced miss.
        gRdReflTex[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    }
}

// ---- STAGE R2: CSRdReflFilter -- finish a PENDING reflection with the spatial history gather --------
//
// R1's other half (Settings::rayDrivenReflSplit, AVER_RD_REFL_SPLIT -- see CSRdRefl's own header,
// immediately above, for the full split contract). Dispatched over the SAME (gx, gy) grid and the SAME
// pixel mapping as CSRdRefl, after a C++ UAV barrier on the reflection history texture R1 just wrote
// (gRtReflHistOut) and before Stage B reads gRdReflTex: every pixel this pass finishes was marked
// PENDING by R1 a moment ago, on the SAME frame -- there is no cross-frame reasoning here, only a
// same-frame round trip through gRtReflHistOut for the pixels wide enough to need the spatial gather.
//
// COMPILED AT SM 6.6 (VoxiRenderer), same layout and defines as CSRdRefl -- see gRtReflHist/
// gRtReflHistOut's own declarations (voxi_rt.hlsli, unconditional inside this same #if AVER_RT region)
// for why nothing here needs its own compile guard. rtReflectionSpatial takes dzdx/dzdy as plain
// parameters rather than calling ddx/ddy() itself, and neither it nor anything it calls touches
// gAverHistoryWrite (that flag only gates rtReflectionTemporal(Ex)'s OWN write to gRtReflHistOut, which
// already happened in R1) -- so, unlike CSRdRefl, this stage has no static to set up before it starts.
[numthreads(8, 8, 1)]
void CSRdReflFilter(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= (uint2)gSceneViewportCur.zw)) return;
    const uint2 pixel = (uint2)gSceneViewportCur.xy + tid.xy;

    const uint pitch = rdRowPitch();
    if (pitch == 0u) return;
    const uint idx = pixel.y * pitch + pixel.x;

    // NOTHING TO DO for every pixel but a PENDING one: a sky pixel, a roughness-gated-out pixel, and a
    // pixel R1 already composed in full (no history bound, or the refl ablation) all left one of the
    // three real alphas here (0, 1 or 2 -- gRdReflTex's own header comment, above) and this stage leaves
    // them untouched. Only alpha < -0.5, R1's PENDING marker (CSRdRefl's own comment on that write),
    // means a gather is still owed.
    const float4 t = gRdReflTex[pixel];
    if (t.a > -0.5) return;
    const float3 skyR  = t.rgb;

    // Same pixel-centre NDC/primary-ray/record reconstruction as CSRdRefl -- see rdPrimaryRayDir's own
    // header for the derivation. gRdVisBuf's record is re-read rather than carried through gRdReflTex:
    // a PENDING pixel is by construction one CSRdRefl already found a surface for, so this can't itself
    // turn up a miss.
    float2 ndc;
    float3 dir = rdPrimaryRayDir(pixel, ndc);

    const uint4 rec = gRdVisBuf[idx];
    RdSurface s = rdSurfaceFromRecord(rec, dir);

    // THE SAME FOOTPRINT RECONSTRUCTION CSRdRefl BUILDS FOR ITS OWN REFLECTION CALL, byte-for-byte --
    // see that stage's own comment for the derivation. `R` is not rebuilt: rtReflectionSpatial never
    // reads it, only rtReflection/rtReflectionTemporal(Ex)'s own ray-tracing half does, and that half
    // already ran, in R1.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float4 farDx = mul(float4(ndc + float2(ndcPixelStep.x, 0.0), 1.0, 1.0), gInvViewProj);
    float3 dirDx = normalize(farDx.xyz / farDx.w - gCamPos.xyz);
    float4 farDy = mul(float4(ndc + float2(0.0, ndcPixelStep.y), 1.0, 1.0), gInvViewProj);
    float3 dirDy = normalize(farDy.xyz / farDy.w - gCamPos.xyz);
    const float3 rdRayDx = (dirDx - dir) * s.hitT;
    const float3 rdRayDy = (dirDy - dir) * s.hitT;
    const float rdReflDzdx = mul(float4(rdRayDx, 0.0), gViewProj).w;
    const float rdReflDzdy = mul(float4(rdRayDy, 0.0), gViewProj).w;

    // ROUGHNESS AND DEPTH ARE RECOMPUTED, NOT READ BACK. Both reached this stage only through RGBA16F
    // texels: the marker's alpha (-1 - rough, a ~0.001 step, enough to move floor(rough * 6) -- the
    // filter radius -- across a boundary) and the history's alpha (curClip.w in half precision, which
    // overflows to inf past 65504 cm and then disables the filter's depth test outright). R1 had both
    // in full float, so this stage rebuilds them from the same inputs: the same roughness sample CSRdRefl
    // gated on, and the same mul(wpos, gViewProj).w rtReflectionTemporalEx wrote as curClip.w.
    const float rough    = rdSurfaceRoughness(s, rdRayDx, rdRayDy);
    const float curDepth = mul(float4(s.wpos, 1.0), gViewProj).w;

    // SAME MIRROR CUTOFF rtReflectionTemporal(Ex) applies before it ever calls rtReflectionSpatial --
    // see that function's own "THE MIRROR CUTOFF" comment -- from the same `rough` R1 traced with, so
    // the filter radius rtReflectionSpatial derives from it agrees.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // R1's OWN WRITE TO THIS SAME UAV, THIS SAME FRAME: gRtReflHistOut (u3) is normally read as last
    // frame's history through the gRtReflHist SRV (t7) -- rtReprojectReflection/rtReflectionSpatial's
    // own gather both read that copy -- but R1 (CSRdRefl's AVER_RD_REFL_SPLIT compile) just wrote THIS
    // pixel's fresh answer straight into the RWTexture2D itself, and the C++ side barriers R1's write
    // against this read (recordStagedRayDriven's own comment) before dispatching this stage. Reading it
    // back here stands in for the `col`/`curHit` rtReflectionTemporalEx would otherwise still be holding
    // in registers, had R1 not already returned.
    //
    // THE INVARIANT THIS DEPENDS ON: rtReflectionTemporalEx writes gRtReflHistOut[pixel] as
    // float4(col, curClip.w) on a hit or float4(0,0,0,-1) on a miss, IDENTICALLY in its untiled and
    // tiled branches (this file, above -- both of that function's own `if (gAverHistoryWrite)` writes).
    // curClip.w is a positive perspective-divide w for any wpos in front of the camera, so alpha > 0
    // means hit and alpha <= 0 (exactly -1 on a miss) means miss, with no value written in between.
    const float4 h = gRtReflHistOut[pixel];
    const bool specHit = h.a > 0.0;

    float3 refl;
    if (specHit) {
        refl = rtReflectionSpatial(h.rgb, s.wpos, s.N, float2(pixel) + 0.5, curDepth, lobeRough,
                                   rdReflDzdx, rdReflDzdy);
    } else {
        refl = float3(0.0, 0.0, 0.0);
    }

    // THE EXACT SAME COMPOSE CSRdRefl's OWN NON-SPLIT TAIL WRITES -- see that block's own comments,
    // above, for the skyW lerp and the clamp/ceiling-alpha reasoning; repeated verbatim here rather than
    // factored out so this stage's control flow reads the same as R1's, pixel for pixel.
    const float skyW = smoothstep(0.5, 0.75, rough);
    const float3 specRaw = lerp(specHit ? refl : skyR, skyR, skyW);
    gRdReflTex[pixel] = float4(clamp(specRaw, 0.0, AVER_VOX_MAXRAD),
                               any(specRaw >= AVER_VOX_MAXRAD) ? 2.0 : 1.0);
}
#endif  // AVER_RT

// ================= depth prepass =================
// Same-frame depth-only pass paired with VSMain (VoxiRenderer.hpp's depthPrepassPipeline(),
// D3D12Device::drawMesh) -- same compiled vertex shader as PSMainVoxi's pipelines, so it writes
// EXACTLY the depth the colour pass would produce for the identical triangle.
//
// WRITES NO COLOUR (renderTargetCount=0) and reads only enough to answer "does this survive alpha
// test" -- deliberately short of averEvalMaterial(), which also samples metal-rough/normal/
// occlusion/emissive for Fresnel/GGX setup a depth-only fragment has no use for; calling it just for
// s.alpha would cost four texture fetches instead of at most one.
//
// STILL NOT FREE: every covered pixel pays a gMaterialFlags branch, and an alpha-tested material
// pays one Sample() against gBaseColorMap plus a compare. What it buys is skipping PSMainVoxi
// entirely (shadow lookup, up to eight cone traces, history blending, fog) on every hidden fragment.
//
// DOES NOT EVALUATE AVER_MAT_SLOPE_BLEND's second layer: that flag is landscape-only, and the
// landscape draws through LandscapeRenderer::draw(), never IDevice::drawMesh/drawMeshDepthPrepass
// (one of this feature's three excluded paths, SandboxApp.cpp). If a non-landscape material is ever
// authored with both AVER_MAT_SLOPE_BLEND and AVER_MAT_ALPHA_MASK, alpha would be the FIRST layer's
// alone -- that combination doesn't exist in this tree today.
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
// in one cascade, instead of shadowPass calling drawMesh() per instance. World transform comes from
// gInstanceWorlds[instanceID] (VoxiRenderer::shadowPass via drawMeshInstanced), not PerObject's
// gWorld. Everything else is identical to VSShadow.
float4 VSShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gCascadeViewProj[(uint)gShadowDraw.x]);
}
#endif

// The same depth-only pair again, for the GI-ONLY shadow map. Identical to VSShadow/
// VSShadowInstanced except that they transform into gGiShadowViewProj -- one box over the GI
// volume -- rather than into a cascade selected by gShadowDraw.x. Separate entry points rather than
// a branch because the matrix is picked at pipeline level, not per draw, and a depth-only vertex
// shader is far too hot to spend a dynamic index on.
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
    // FALSE, not merely unset: no camera in this pass (V=0 above), so "is the eye inside" has no
    // answer -- false makes the TIR test below inert, correct for a bake with no viewpoint. HLSL
    // leaves a struct member uninitialised, so this must be written.
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
// Voxelisation without a geometry shader: the same dominant-axis projection, per primitive.
//
// nr[k] uses averTransformNormal(v.nrm, gWorld), not a plain mul(float4(nrm,0),gWorld), to match
// VSVoxel/VSMain/etc -- see 1856da1, which fixed four OTHER call sites of this bug and missed this
// fifth because MSVoxel compiles only behind AVER_MS, unused at the time. A plain mul is only
// correct under rotation/uniform scale; wrong here would tint an entire surface's bounce light for
// as long as the volume holds it, not just flicker one triangle.
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
    // This injection had two defects that between them made the volume store an AMBIENT TERM rather
    // than bounced light, which is what "there's no bounce lighting" actually looks like from the
    // outside. Both are fixed by the same single cone, so it is traced once and used twice.
    //
    // 1. THE SKY WENT IN UNOCCLUDED. sun.visibility gates the sun term; averSkyIrradiance(N) took
    //    no visibility term at all, and it cannot compute one -- it is a function of the NORMAL and
    //    nothing else, with no world position anywhere in it. So a voxel three walls deep inside the
    //    arcade was handed exactly the same open-sky irradiance as one standing in the courtyard
    //    with the same normal. The stored volume was therefore dominated by a flat,
    //    position-independent term, and a gather over it returns albedo x constant. MEASURED before
    //    this change: isolating the GI's contribution on Sponza gave an image that is essentially a
    //    copy of the albedo texture -- bright where the plaster is white, dark where the stone is
    //    dark -- with no pooling near lit surfaces at all.
    //
    // 2. THERE WAS NO SECOND BOUNCE. Nothing in this module ever read the volume back into the
    //    injection; the only readers of gVoxelTex were the mip filter, the forward-pass gather and
    //    the debug raymarch. Light reflected off exactly one surface and stopped. Indoors that is
    //    most of the light missing -- an arcade is lit by its own walls -- which is why the scene is
    //    dark enough that the eye adaptation pegs at its ceiling every frame.
    //
    // WHY THIS IS LEGAL HERE, checked rather than assumed: voxelTex_ is in ShaderResource for the
    // whole of voxelizePass's raster draws (VoxiRenderer.cpp transitions it to UnorderedAccess only
    // just before the resolve), and bindings_ -- the set this pass binds -- already carries the SRV
    // over the full mip chain at t0. So this needs no new binding, no new descriptor and no barrier
    // change. The clear at the top of the pass zeroes mip 0 ONLY, and the cone's first sample lands
    // at mip ~1.2 (dist starts at two voxels, and a 60-degree aperture is already wider than one
    // voxel there), so what it reads is the previous bake, still intact in mips 1..N.
    //
    // WHAT IT COSTS AND WHY IT CONVERGES: one cone per injected fragment, against the thirteen the
    // forward gather already runs per pixel. Each rebuild adds albedo x (light already in the room),
    // and albedo < 1, so the series is geometric and settles -- the same argument radiosity has
    // always rested on. AVER_VOX_MAXRAD still bounds it if a scene ever tries to break that.
    const float4 room = traceCone(i.wpos, N, AVER_VOX_INJECT_APERTURE);
    // A cone that terminated on solid geometry saw no sky; one that ran out of volume saw all of it.
    // traceCone accumulates occlusion in .a and treats leaving the volume as unoccluded, which is
    // the correct reading of "this voxel is open to the sky" for exactly this purpose.
    //
    // ...BUT ONLY APPROXIMATELY, AND BADLY INDOORS AT A FINE VOLUME. The volume holds one-voxel shells
    // of surfaces, and a 60-degree cone a few metres out samples mip 5+, where a shell covers ~2% of
    // a cell -- so the cone sees through a roof and a voxel deep inside an arcade is handed nearly the
    // open sky. MEASURED at 512^3 on PTTest's gallery (sun 85.6 deg), painting each visible surface
    // with its own voxel: sunlit surfaces within 1.3x of the path tracer, shadowed interior ones
    // 30-350x too bright. Taking the sky term out alone brought the whole frame from 0.55 to 0.011
    // (path tracer 0.015).
    const float skyVis = saturate(1.0 - room.a);
    // SO WHILE ReSTIR GI IS THE ESTIMATOR, THE VOLUME CARRIES NO SKY. That estimator traces the sky's
    // visibility with real rays -- its candidate's miss, and at Half/Full the candidate hit's own
    // second ray -- and reads this volume only for SURFACE-bounced light, where the leaked sky above
    // counted 3-7x over the path tracer. Keyed on gViewParams.z, the SETTING (see its cbuffer
    // comment for why not gGiRestirParams.x); VoxiRenderer::voxelSkyInjected() is the same test, and
    // the rebuild gate and the GI cache key both carry it, so switching giMode rebakes rather than
    // reusing a volume baked under the other rule.
    //
    // WHAT STILL READS THE SKY FROM HERE and so loses it under ReSTIR: GI-lit particles (their whole
    // lighting is this volume), the rough-specular cone fallback, and the cone gather on a frame
    // ReSTIR is chosen but cannot run (the GI debug view). All were reading the leaked sky above;
    // indoors they get darker and closer to right, in open shade darker than right.
    const float skyInject = gViewParams.z > 0.5 ? 0.0 : 1.0;

    // Exitant radiance, not radiosity: the sun term is an irradiance so it takes the 1/PI, the sky
    // term is already a radiance so it does not, and the feedback term is already exitant radiance
    // gathered from surfaces that have themselves been through this same shader.
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility / PI
                                + averSkyIrradiance(N) * gAmbient.r * skyVis * skyInject
                                + room.rgb * AVER_VOX_FEEDBACK);
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

// All three kernels below dispatch over their destination mip level's FULL extent -- the group count
// the C++ side requests never shrinks -- and instead skip work per-thread with this guard, so a thread
// whose voxel falls outside [gBoxLo,gBoxHi) returns before touching any resource. With bounded dispatch
// off, the C++ side sets gBoxLo=0 and gBoxHi=(this level's dimension), so the guard only ever rejects
// threads at or beyond the texture edge -- exactly the threads whose writes D3D12/Vulkan already drop
// silently out-of-bounds today. Every tier's voxel resolution is a power of two (QualityLadder.hpp), so
// at mip 0 the dispatch already divides evenly by the 4-wide group and no thread id reaches that edge;
// deeper mips can, since ceil(mipDim/4)*4 can overshoot mipDim, and this guard is what already made
// that safe before bounded dispatch existed -- it is not new behaviour, only named and reused here.

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
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
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
