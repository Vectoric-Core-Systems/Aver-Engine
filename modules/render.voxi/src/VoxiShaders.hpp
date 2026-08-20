#pragma once

// Voxi's HLSL, compiled as the TAIL of rhi::sharedShaderPrelude() + the material prelude, which
// already declare the cbuffer layouts, VSIn/VSOut/SkyOut, the BRDF, the Aver* contract and
// VSMain/VSky/MSMain. Declaration order is load-bearing: HLSL has no forward declarations.
namespace aver::voxi {

inline constexpr const char* kVoxiHLSL = R"(
// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
#define AVER_SHADOW_CASCADES 4

cbuffer VoxiFrame : register(b4) {
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
    // The GI-ONLY shadow map's light view-projection: one box fitted to the GI VOLUME, not to the
    // camera. Read only by giShadowFactor (PSVoxel); the cascades above stay camera-fitted and are
    // what PSMainVoxi samples.
    float4x4 gGiShadowViewProj;
    // x = 1/kGiShadowSize, y = 1 once the GI-only map is usable (0 = fall back to unshadowed
    // indirect), z = normal-offset bias in world units, w unused.
    float4   gGiShadowParams;
    // The SPATIAL shadow denoiser. x = filter radius in pixels (0 = off); y = how much of the
    // filtered value to take (0 = none, so the taps still run and the result is discarded --
    // that is the cost-measurement configuration, and lerp(v, f, 0) is v exactly for any finite
    // f); z and w unused. RADIUS LIVES IN A CONSTANT, not a #define, so the tap loop is dynamic
    // and cannot be unrolled away when the host asks for zero taps -- a compile-time 0 would
    // measure nothing and report it as free.
    float4   gRtDenoiseParams;
    float4   gPtBounceParams;
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
#define AVER_VOX_MAXRAD 16.0

// Directional shadow map. Core feature level 11_0, so it works on every DX12 GPU.
Texture2D<float>          gShadowTex  : register(t1);
SamplerComparisonState    gShadowSamp : register(s1);

// The GI-only shadow map. OUTSIDE the AVER_RT guard below on purpose: its only reader is PSVoxel,
// which is compiled without ray tracing, so a declaration inside the guard would vanish exactly
// where it is needed. Shares gShadowSamp -- same comparison state, different texture.
Texture2D<float>          gGiShadowTex : register(t8);

#if AVER_RT
// DXR 1.1 inline ray tracing: traced from the pixel shader, no state objects or binding tables.
RaytracingAccelerationStructure gScene : register(t2);

// The flat geometry a reflection ray reads after it hits something. Three descriptors for the whole
// scene rather than one per mesh, because this RHI uses explicit descriptor tables and not bindless.
struct RtVertex   { float3 pos; float3 nrm; float2 uv; };
struct RtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo;
                    float metallic; float roughness; uint pad; };
StructuredBuffer<RtVertex>   gRtVerts     : register(t3);
StructuredBuffer<uint>       gRtIndices   : register(t4);
StructuredBuffer<RtInstance> gRtInstances : register(t5);

// The ray-traced sun-shadow history: LAST frame's resolved (visibility, depth) (t6, read this
// frame) and THIS frame's (u2, written this frame, becomes t6 next frame). Ping-ponged on the C++
// side -- VoxiRenderer.hpp rtShadowHist_ -- never the same texture in the same frame. x =
// visibility in [0,1], y = linear (view-space) depth in centimetres, for rtReprojectHistory's
// disocclusion test.
Texture2D<float2>   gRtShadowHist    : register(t6);
RWTexture2D<float2> gRtShadowHistOut : register(u2);

// The ray-traced reflection history: LAST frame's resolved (colour, depth) (t7, read this frame)
// and THIS frame's (u3, written this frame, becomes t7 next frame). Same ping-pong as the shadow
// history above, its own pair of textures. rgb = the reflection's own shaded colour, a = linear
// depth of the hit, OR NEGATIVE meaning the ray missed -- see rtReflectionTemporal.
Texture2D<float4>   gRtReflHist    : register(t7);
RWTexture2D<float4> gRtReflHistOut : register(u3);

// A hash of the pixel, for rotating each pixel's sample pattern.
//
// SPATIAL ONLY, and that is a hard requirement rather than a simplification: the gate oracle
// compares 8-bit probe codes BIT-EXACTLY, and two of its gates deliberately sample a penumbra. A
// seed that varied per frame would make those a coin flip run to run, and the engine's whole
// verification story rests on flat-neighbourhood probes being reproducible. THIS STAYS TRUE of
// rtHash itself even now that rtShadow accepts a separate, explicit `frameJitter` on top of it --
// see rtShadow's own comment. Nothing IN rtHash depends on the frame; a caller that wants frame
// variance adds it outside, deliberately, where it is a choice made once per call site rather than
// a property of the hash every caller inherits.
float rtHash(float2 p) {
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// The radical inverse of `i` in base 2 -- its bits reflected about the binary point -- in [0,1).
//
// THIS IS WHAT MAKES THE SAMPLE SEQUENCE NESTED, and the radius it replaced was not. `sqrt((k+0.5)/n)`
// puts sample k at a radius that depends on the TOTAL ray count, so asking for more rays MOVES every
// sample rather than adding to them: n=2 and n=4 are then two unrelated estimators of the same
// integral, each its own oracle, and no comparison between two ray counts is a refinement of the
// first. phi(k) depends on k alone, so the first m samples of an n-sample set ARE the m-sample set --
// raising the count keeps every ray already traced and fills in between them. That is what lets a
// ray-count sweep be read as convergence, and what lets one baseline serve every count.
//
// EXACT ON EVERY ADAPTER, which the sun disc's other option -- a hash -- is not. reversebits is
// integer bit manipulation, the uint-to-float conversion is IEEE round-to-nearest, and 2^-32 is a
// power of two so the multiply is exact. The gate oracle compares nine configurations bit-exactly,
// WARP among them, and a divide by a per-call ray count was one more thing that did not have to be.
float rtRadicalInverse2(uint i) {
    return (float)reversebits(i) * 2.3283064365386963e-10;   // 1 / 2^32
}

// Sample `k` of the disc sequence the shadow loop walks, as a point in the unit disc. `ang0` turns
// the whole pattern by a per-pixel angle so neighbouring pixels do not share one set of directions.
//
// THE SIGNATURE IS THE PROPERTY. There is no ray count in it, and there cannot be one: a sample is a
// function of its index alone, which is what "nested" means and what the version this replaced --
// sqrt((k + 0.5) / n) -- could not say. Kept as its own function rather than inlined into the loop
// so that the claim is checkable in one place rather than argued about in a loop body.
//
// The golden angle around and the radical inverse outward, with sqrt to map the radial coordinate
// onto AREA rather than radius -- without it the samples crowd the centre and every estimate is
// biased toward the middle of the sun.
float2 rtDiscSample(uint k, float ang0) {
    float rad = sqrt(rtRadicalInverse2(k + 1));
    float a   = ang0 + (float)k * 2.39996323;
    return float2(cos(a), sin(a)) * rad;
}

// Traces occlusion rays toward the sun's DISC and returns the fraction that reached it: 0 fully
// shadowed, 1 fully lit, and everything between is a real penumbra.
//
// The single ray this replaced returned exactly 0.0 or 1.0, so a ray-traced shadow had a hard
// aliased edge while the cascade path beside it did a 3x3 comparison filter -- turning ray tracing
// ON made shadows look worse, which is the wrong way round. The sun is not a point: it subtends
// about half a degree, and that angle is what sets how fast an edge softens. gRtParams.x carries
// its tangent so the softening is the SUN's property rather than a tuned constant.
//
// The bias scales with distance from the camera. A fixed 0.02 cm offset is roughly 300 float ulp
// at 1000 cm from the origin and under 3 at 100000 cm, so distant geometry self-intersects and
// speckles -- acne that looks like flickering rather than like a bias problem.
// `dpx`/`dpy` are the receiver's screen-space footprint, PASSED IN rather than taken here.
//
// They used to be ddx/ddy(wpos) computed inside. That is correct for a primary surface, where wpos
// varies smoothly across the quad, and WRONG for a reflected hit: neighbouring pixels' rays land on
// different triangles metres apart, so the "footprint" becomes a wild vector and the shadow rays
// scatter across the scene. A derivative taken inside divergent flow is undefined in HLSL as well.
// A reflected caller passes zero and gets a point sample, which is what it wants.
// `rays` is explicit rather than read from the constant buffer, so a SECONDARY ray can ask for
// fewer than a primary one. A reflection is already an approximation -- one bounce, no roughness
// lobe -- and spending a full disc sweep on the shadow of something seen IN a reflection buys
// detail nobody can resolve. Primary shading still passes the full count.
//
// `frameJitter` is ADDED to the per-pixel rotation, and is NOT part of rtHash: rtHash itself stays
// exactly what its own comment says it must -- a pure function of the pixel, for the gate oracle's
// bit-exact single-frame probes. Every existing caller passes 0.0 here and is completely unaffected.
// rtShadowTemporal is the one caller that passes something else, and only when pixel tiling is
// actually on -- see its own comment for why a NONZERO, per-frame value is what makes tiling
// converge at all instead of repeating one sample forever.
float rtShadow(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
               float frameJitter) {
    const uint  n    = max(rays, 1u);
    const float tanR = max(gRtParams.x, 0.0);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);

    // A frame around the light direction, to spread samples across the disc.
    float3 up = abs(L.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);

    // THE PIXEL'S OWN FOOTPRINT ON THE SURFACE, from the screen-space derivatives of the world
    // position. This is what actually fixes the jagged edge, and the sun's disc is not:
    //
    // the sun's angular RADIUS is about a quarter of a degree, so at contact distances the true
    // penumbra is far narrower than a pixel -- spreading rays across the disc alone gives the same
    // binary answer as one ray did, everywhere except a single edge pixel. Meanwhile the shadow
    // term is computed ONCE PER PIXEL while the geometry beside it is resolved at 8x MSAA, so the
    // shadow boundary stair-steps against smooth silhouettes. That mismatch is the visible defect.
    //
    // Jittering the ray ORIGIN across the footprint turns the per-pixel test into an area estimate,
    // which is antialiasing the shadow rather than blurring it: the result converges to the exact
    // fraction of the pixel that is occluded.
    const float ang0 = rtHash(pixel) * 6.2831853 + frameJitter;
    float vis = 0.0;

    [loop] for (uint k = 0; k < n; ++k) {
        // The sample the loop is at. NOTHING HERE DEPENDS ON n, which is the whole design: sample k
        // sits in the same place whatever the ray count, so raising the count refines the estimate
        // instead of replacing it with an unrelated one. The same rotated pattern serves both the
        // sun disc and the pixel footprint, so one sample covers both.
        float2 disc = rtDiscSample(k, ang0);

        float3 dir = normalize(L + (T * disc.x + B * disc.y) * tanR);
        // Half the footprint, so samples stay inside the pixel they are estimating.
        float3 org = wpos + (dpx * disc.x + dpy * disc.y) * 0.5;

        RayDesc r;
        // Offset along the NORMAL and along the ray. The normal alone leaves acne at grazing
        // angles, where the surface is nearly parallel to the ray and the offset barely separates
        // them.
        r.Origin    = org + N * bias + dir * bias;
        r.Direction = dir;
        r.TMin      = bias;
        r.TMax      = 100000.0;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, r);
        q.Proceed();
        vis += q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
    }
    return vis / (float)n;
}

// Reprojects wpos through LAST frame's camera to sample the ray-traced shadow history there. False
// when the reprojection is not usable at all: off-screen, behind last frame's near plane, or a
// DISOCCLUSION -- the stored depth at the reprojected texel does not match what this world point
// should have looked like last frame (see the depth-comparison block below). In every false case
// `hist`/`velocityPx` are left untouched.
//
// `pixel` is this frame's own screen position; the reprojection's screen-space displacement from
// it is handed back as `velocityPx` so the caller can discount an otherwise-valid sample that has
// moved -- camera motion (or the receiver surface itself moving) pushes a static world point
// across texels between frames, which the depth test alone does not catch: it only tells apart
// "still the same surface" from "now looking at something else", not "the same surface, but I have
// slid along it since last frame."
//
// NDC -> LAST frame's VIEWPORT rect, not [0,1] of the whole texture: the editor docks the 3D view
// in a sub-rect of the backbuffer (gSceneViewport), and a plain ndc*0.5+0.5 implicitly assumes the
// viewport covers the entire render target, which lands every reprojection on the wrong texel
// whenever it does not. Caught by the shadow-rt / penumbra-rt gates -- moved by a full shade, not a
// rounding difference -- not shipped.
//
// NEAREST, not bilinear -- and this is the OPPOSITE of what an earlier version of this function
// concluded, for a reason specific to who calls it now. That version was read every frame by every
// pixel, all of them always in sync (freshly traced that same frame), so a bilinear tap blending in
// a neighbour was blending in something almost identical -- cheap insurance against a reprojected
// coordinate landing a hair off a texel centre. rtShadowTemporal's tiled path is the opposite
// situation on purpose: neighbouring pixels are DELIBERATELY out of sync, each mid-way through its
// own turn cycle, so a bilinear tap mixes in a neighbour that can be many frames stale and on the
// other side of a penumbra -- measured to converge to a stable but WRONG value (a 4x4 tile settled
// at 46,46,47 against a real 23,27,32, unmoved between 300 and 1500 frames, so this was not slow
// convergence). Nearest guarantees this pixel reads its OWN last write, which is what the schedule
// in rtShadowTemporal actually assumes.
bool rtReprojectHistory(float3 wpos, float2 pixel, out float hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    // FLOOR, not round: `px` for a pixel that has not moved lands almost exactly on THAT pixel's own
    // centre (integer index + 0.5), which is precisely the .5 tie `round()` breaks inconsistently
    // (round-half-to-even) depending on whether the index is odd or even -- silently sending roughly
    // half of all pixels to their wrong neighbour instead of themselves. floor() of a centre at
    // index+0.5 is exactly `index`, matching how the WRITE side already indexes this same texture
    // (gRtShadowHistOut[uint2(pixel)] truncates SV_Position the same way). This is what a tiled
    // pixel schedule's "read my own last write" actually needs -- round() was measured to converge
    // to a stable but wrong value (penumbra-rt settled at 78,75,70 against a true ~23,27,32).
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float2 stored = gRtShadowHist.Load(int3(texel, 0));   // x = visibility, y = linear depth
    // clip.w IS the expected depth at this reprojected point: the same value VSMain's o.pos.w would
    // carry for a vertex sitting at wpos (RHIShaders.cpp's VSMain computes o.pos = mul(wp,
    // gViewProj) the identical way), just through LAST frame's camera instead of this one. Comparing
    // it to what the history actually stored there catches a disocclusion a screen-position check
    // alone cannot: a silhouette edge can reproject to an already-populated texel while the surface
    // now visible through it sits at a completely different depth.
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0;   // 3% relative, +1cm floor at grazing distances
    if (abs(clip.w - stored.y) > tol) return false;

    hist = stored.x;
    velocityPx = px - pixel;
    return true;
}

// The SPATIAL denoiser: average this pixel's shadow with its neighbours' from the history texture,
// weighted by how well each neighbour's surface agrees with this one's.
//
// WHY IT READS LAST FRAME'S TEXTURE AND WHY THAT IS FINE. t6 and u2 are different textures, ping-
// ponged per frame, and t6 rests in ShaderResource for the whole colour pass -- so an arbitrary
// neighbourhood read is a plain load of an immutable texture, needing no barrier and no reordering
// of anything. The neighbours are one frame old. That is a completely different proposition from
// the TEMPORAL path above, which reuses a value up to 2^(2*tileBits) frames old AS THE ANSWER:
// here the centre pixel always contributes its own freshly traced value, and a neighbour only ever
// adjusts the weighting. A neighbour that fails the plane test is DROPPED from the kernel, never
// substituted, so the worst case is that every neighbour is rejected and this returns the centre
// unchanged -- today's noisy-but-correct behaviour. The guide weights the blur; it never supplies
// the value.
//
// WHY AVERAGING NEIGHBOURS IS AN ESTIMATE RATHER THAN A BLUR. rtShadow jitters the ray ORIGIN
// across the pixel's own footprint (see its `org` line), so neighbouring pixels on one flat
// receiver are already sampling different points of the same surface. Their mean is a genuine area
// estimate of the same integral one pixel would need many rays to reach.
float rtShadowSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int radius = (int)gRtDenoiseParams.x;
    if (radius <= 0 || gRtHistParams.y < 0.5) return centre;

    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);

    // GATHER AROUND WHERE THIS PIXEL WAS LAST FRAME, NOT AROUND WHERE IT IS NOW. gRtShadowHist is
    // last frame's texture; while the camera moves, the image has shifted inside it, so a
    // neighbourhood centred on THIS frame's coordinate samples a patch of a different part of the
    // scene. Measured before this was added: still, the filter landed within one code of the
    // sixteen-ray answer -- and under a six-degree wobble it drifted 12 to 32 codes darker,
    // increasing with radius, which is the signature of a kernel walking off its own surface.
    //
    // Same arithmetic as rtReprojectHistory, and deliberately the same in every detail: last
    // frame's viewProj, mapped into last frame's VIEWPORT RECT rather than [0,1] of the whole
    // texture (the editor docks the 3D view in a sub-rect), and FLOOR rather than round -- a pixel
    // that has not moved lands on its own centre at index+0.5, which is exactly the tie round()
    // breaks half the time in the wrong direction. Both of those are documented landmines in that
    // function; this one inherits them rather than re-deriving them.
    float2 centrePx = pixel;
    if (gRtHistParams.y > 0.5) {
        const float4 pclip = mul(float4(wpos, 1.0), gPrevViewProj);
        if (pclip.w > 1e-4) {
            const float3 pndc = pclip.xyz / pclip.w;
            if (pndc.z >= 0.0 && pndc.z <= 1.0)
                centrePx = gSceneViewport.xy +
                           float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
        }
    }
    const int2 base = int2(floor(centrePx));

    // Plane-distance rejection, not a raw depth delta. Centre depth plus its screen-space gradient
    // defines the receiver's plane; a neighbour on that same plane is kept however far its depth
    // has slid, while one at the same depth on a DIFFERENT surface is dropped. On a grazing floor a
    // plain |dz| test rejects almost everything and the filter quietly does nothing.
    const float dzdx = ddx(curDepth);
    const float dzdy = ddy(curDepth);

    float acc = centre;
    float wsum = 1.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float2 st = gRtShadowHist.Load(int3(t, 0));
            // What this neighbour's depth WOULD be if it sat on the centre's plane.
            const float predicted = curDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.y - predicted) > tol) continue;
            acc  += st.x;
            wsum += 1.0;
        }
    }

    // gRtDenoiseParams.y is how much of the filtered value to take. At 0 the taps above still run --
    // radius comes from a constant, so the loop is dynamic and survives optimisation -- and this
    // returns `centre` EXACTLY, because lerp(v, f, 0) is v + 0*(f-v) for any finite f. That is the
    // configuration the tap cost is measured in, before the filter itself is trusted.
    return lerp(centre, acc / wsum, saturate(gRtDenoiseParams.y));
}

// The PRIMARY sun-shadow call only -- rtReflection's own inner rtShadow() call stays exactly as it
// always has, one ray with no footprint and frameJitter = 0.0, and never touches the history:
// blending in a reflected surface's shadow would overwrite this pixel's history with a value that
// has nothing to do with what a later frame's PRIMARY ray at this same pixel is estimating.
//
// gRtHistParams.w is the pixels-per-ray TILE EDGE, as its bit count (0 = off, every pixel traces
// every frame). At 0 this is BIT-FOR-BIT what rtShadow() alone gives: no jitter, no history blend,
// only a plain write so the buffer stays live for whenever a caller turns tiling on. Tiling is what
// actually cuts ray count -- not blending on its own, which only smooths flicker on something that
// is genuinely moving. Turning tiling on is what makes the (now adaptive, see below) blend cost
// worth paying: most pixels do not trace at all most frames.
float rtShadowTemporal(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays) {
    // gRtHistParams.x is 0 whenever t6/u2 are not bound to real textures this frame (see
    // VoxiRenderer::beginShadowHistory) -- an unbound slot is Tier 1 null-filled, and touching
    // either one here would read or write a null descriptor rather than skip cleanly.
    if (gRtHistParams.x < 0.5) return rtShadow(wpos, N, L, pixel, dpx, dpy, rays, 0.0);

    // THIS frame's own linear depth at wpos, computed the same way VSMain would (mul(wp, gViewProj)
    // .w) rather than read back from a depth buffer -- the shadow pass has none of its own. Written
    // into the history alongside visibility every single write below, regardless of which branch is
    // taken, so next frame's rtReprojectHistory always has a fresh depth to disocclusion-test
    // against, not one that is itself stale by however many frames since this pixel's last turn.
    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;

    const uint tileBits = (uint)gRtHistParams.w;
    if (tileBits == 0u) {
        const float fresh = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, 0.0);
        gRtShadowHistOut[uint2(pixel)] = float2(fresh, curDepth);
        // FILTERED HERE TOO, and this branch is the one that matters most. tileBits == 0 is
        // rtPixelsPerRayTile == 1, which is what Medium -- the DEFAULT tier -- runs, and it is the
        // only configuration in which the shadow term is a hard 0 or 1 with nothing whatsoever
        // smoothing it. Returning `fresh` straight from here, as this did, wired the spatial filter
        // into the amortised path alone and left the default one completely untouched: the penumbra
        // probe read an unchanged 61,59,59 at every radius, which is what caught it. Two returns,
        // two call sites -- an early return is exactly how a later edit loses one of them again.
        return rtShadowSpatial(fresh, wpos, N, pixel, curDepth);
    }

    // Which pixel in its tileBits x tileBits tile gets to trace THIS frame -- a bitmask against the
    // pixel coordinate and the frame index, not a modulo, since tileBits is always a power of two
    // (VoxiRenderer::setPixelsPerRayTile only ever rounds to one). Over 2^(2*tileBits) consecutive
    // frames every pixel in the tile gets exactly one turn, staggered so a whole tile is never
    // skipped or traced together -- a block pattern would show as visible tiles rather than noise.
    const uint frameIdx = (uint)gRtHistParams.z;
    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.5 && rtReprojectHistory(wpos, pixel, hist, velocityPx);

    float vis;
    if (myTurn || !haveHist) {
        // The golden-angle frame offset spends a DIFFERENT sample of the same low-discrepancy
        // sequence each turn, so 2^(2*tileBits) turns converge toward the same estimate that many
        // SPATIAL rays would give in one frame -- see rtDiscSample for why that composition works.
        // frameJitter = 0.0 (rtHash alone) would repeat the identical ray every turn, which never
        // converges past one sample; that is the whole reason tiling needs this and the tileBits==0
        // path above does not.
        const float frameJitter = (float)frameIdx * 2.39996323;
        vis = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
        if (haveHist) {
            // ADAPTIVE blend weight, not a flat constant: a reprojected sample that has barely
            // moved on screen is close to a repeated measurement of the same point and earns a high
            // weight; one that has moved several pixels is increasingly likely to be sampling
            // slightly the wrong part of the surface even though it passed the depth test above
            // (depth alone tells "same surface" from "different surface", not "same surface, but
            // I've slid along it"), so it is trusted less the faster it is moving. The budget the
            // falloff runs over SHRINKS as the tile grows: a bigger tile's history is on average
            // staler even before any motion is considered (up to 2^(2*tileBits) frames old), so the
            // same screen velocity should discount it over a shorter distance.
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            vis = lerp(vis, hist, weight);
        }
    } else {
        // Not this pixel's turn, and reprojection is valid: reuse it outright. No ray at all this
        // frame -- this is the actual saving tiling exists for.
        vis = hist;
    }

    // WRITE THE RAW VALUE, NEVER THE FILTERED ONE, and this is the single most important line in
    // the whole denoiser. gRtShadowHistOut is what next frame reprojects from; feeding a filtered
    // value back into it makes this an IIR filter with a spatial kernel -- a TEMPORAL filter by
    // another name -- and every artefact the spatial path exists to avoid comes straight back,
    // compounding a little more each frame. The filter is applied on READ, below, and the history
    // never learns it happened. A later reader will be tempted to "save work" by writing the
    // filtered value here; that is the bug, not the optimisation.
    gRtShadowHistOut[uint2(pixel)] = float2(vis, curDepth);
    return rtShadowSpatial(vis, wpos, N, pixel, curDepth);
}

// Traces one reflection ray and shades what it hits.
//
// THIS IS WHAT MAKES REFLECTIONS GLOBAL. The cone tracer it replaces walks the voxel volume and
// stops dead at its boundary -- `if (!insideVolume(uvw)) break;` -- so anything outside simply was
// not reflected and the result fell back to sky. Objects popped in and out of reflections as they
// crossed a boundary that has nothing to do with the scene. A ray has no such bound: it reaches
// whatever the acceleration structure holds, at any distance.
//
// The shading is one bounce of Lambertian direct light plus sky ambient, with the hit surface's own
// albedo from the instance table. No textures and no second bounce -- a reflected surface is
// slightly flatter than the same surface seen directly, which is a stated approximation rather than
// an accident.
float3 rtReflection(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, out bool hit) {
    hit = false;
    RayDesc r;
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + N * bias;
    r.Direction = R;
    r.TMin      = bias;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;

    RtInstance inst = gRtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0 = inst.firstVertex + gRtIndices[tri + 0];
    uint i1 = inst.firstVertex + gRtIndices[tri + 1];
    uint i2 = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    // Rotation only. The engine is row-vector, so a direction is the vector times the upper 3x3 --
    // and a non-uniform scale would need the inverse transpose, which this deliberately does not
    // carry: reflections here are of rigid instances.
    float3 nWS = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(nWS, R) > 0.0) nWS = -nWS;   // face the ray, so a back-facing hit is not lit from behind

    float3 hitPos = wpos + R * q.CommittedRayT();
    // Whether the SUN reaches the reflected surface. Without this every reflection is lit as if
    // nothing could shadow it, which is what makes cheap reflections look like they glow.
    // Seeded from the PIXEL, and with NO footprint. Seeding from hitPos.xy -- a world float derived
    // from CommittedRayT -- makes the sample pattern depend on a ray distance, which is exactly the
    // value most likely to differ between a hardware adapter and WARP, and the gate oracle compares
    // nine configurations bit-exactly.
    // ONE ray, not the full disc. This is the single largest saving available in the ray path:
    // a reflective pixel was firing one reflection ray plus a four-ray disc from its hit, so five
    // rays where two do. The penumbra of a reflected shadow is not resolvable in a one-bounce
    // mirror image.
    float shadow = rtShadow(hitPos, nWS, L, pixel, float3(0,0,0), float3(0,0,0), 1u, 0.0);

    // LAMBERTIAN EXITANT RADIANCE, and the /PI is the whole point. averGroundRadiance is the
    // engine's own reference for this and reads:
    //
    //     E = sunIrradiance*ndl + PI*skyRadiance*ambient;   return albedo * E / PI;
    //
    // so the sun's contribution to outgoing RADIANCE is albedo*sunIrradiance*ndl/PI, while the
    // sky's is albedo*skyRadiance*ambient with the PI cancelling. The first version of this
    // function omitted the divide on the direct term only, which made every SUNLIT reflection
    // 3.14x too bright while leaving shaded ones correct -- brightness that looks like an exposure
    // problem rather than a units one, and which pushed the lit ray-traced gates from 94,27,14 to
    // 131,58,40.
    //
    // THE WHITE FURNACE DOES NOT CATCH THIS, and that is worth knowing about the oracle: it turns
    // the sun OFF, so the direct term is zero and only the ambient half -- which was already right
    // -- is under test. A furnace with a sun is a second mode worth having.
    float3 direct = averSunRadiance() * saturate(dot(nWS, L)) * shadow / PI;
    float3 ambient = averSkyIrradiance(nWS) * gAmbient.r;
    hit = true;
    return inst.albedo * (direct + ambient);
}

// Reprojects wpos through LAST frame's camera to sample the reflection history there. False when
// not usable: off-screen, behind last frame's near plane, no hit recorded there (stored.a <= 0 --
// see gRtReflHist's own comment for the miss sentinel), or a depth mismatch (disocclusion) -- the
// exact same test rtReprojectHistory uses for the shadow, against the reflection's own depth
// channel instead.
//
// A MISS IS NEVER REPROJECTED, on purpose, and that is the one real difference from the shadow
// case: sky-by-direction is cheap to recompute (no ray, just an analytic model) and highly VIEW
// dependent, so reusing a stale miss sample as the camera rotates would show the wrong patch of
// sky through a still surface. A real hit's shading has no such problem -- it depends on the
// reflected surface and a mostly-static light, not on the viewing angle -- so only hits are worth
// the reprojection at all.
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

// Tiled/temporal wrapper around rtReflection(), mirroring rtShadowTemporal's structure exactly and
// sharing its tile schedule -- same tileBits, same frameIdx, same per-pixel turn -- so a pixel's
// shadow ray and reflection ray amortise on the same cadence instead of needing two separate
// knobs. Called ONLY when the caller has already gated on roughness (s.rough <= 0.5 in
// PSMainVoxi): a pixel that never qualifies for a reflection ray at all has nothing here to tile
// or reproject, and this function does not re-check that gate.
//
// `hit` means the same thing it does for rtReflection() -- true when there is a real reflection
// colour to use, false when the caller should fall back to the sky. It is true both for a fresh
// hit this frame AND for a reused hit from history (haveHist is already conditioned on the stored
// sample being a real hit, never a miss -- see rtReprojectReflection).
float3 rtReflectionTemporal(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, out bool hit) {
    if (gRtHistParams.x < 0.5) return rtReflection(wpos, N, R, L, pixel, hit);

    const float4 curClip = mul(float4(wpos, 1.0), gViewProj);
    const uint tileBits = (uint)gRtHistParams.w;
    if (tileBits == 0u) {
        float3 fresh = rtReflection(wpos, N, R, L, pixel, hit);
        gRtReflHistOut[uint2(pixel)] = hit ? float4(fresh, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
        return fresh;
    }

    const uint frameIdx = (uint)gRtHistParams.z;
    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float3 hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.5 && rtReprojectReflection(wpos, pixel, hist, velocityPx);

    float3 col;
    bool curHit;
    if (myTurn || !haveHist) {
        col = rtReflection(wpos, N, R, L, pixel, curHit);
        // Same adaptive-weight shape as rtShadowTemporal -- see its comment for the reasoning.
        // Only blends a HIT with history: a fresh miss stays a miss (the caller's own sky fallback
        // already handles that correctly) rather than being dragged toward a stale hit colour.
        if (curHit && haveHist) {
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            col = lerp(col, hist, weight);
        }
    } else {
        // Not this pixel's turn, and reprojection found a real hit: reuse it outright. No ray at
        // all this frame -- this is the actual saving tiling exists for.
        col = hist;
        curHit = true;
    }

    hit = curHit;
    gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
    return col;
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

// Sun visibility for LIGHT INJECTION, from the GI-only shadow map.
//
// A SEPARATE FUNCTION FROM shadowFactor BECAUSE IT ANSWERS A DIFFERENT QUESTION. shadowFactor asks
// "is this PIXEL in shadow", picks a cascade by distance from the CAMERA, and fades out past the
// last one -- all correct for something being drawn on screen. A voxel is not on screen. It exists
// wherever the GI volume is, the volume does not move with the camera, and a voxel the camera is
// not looking at must still be shadowed correctly or the bounce light it contributes is wrong.
//
// Feeding voxels through the cascades is what forced fitCascades to union its last cascade with the
// whole GI volume, which cost ~14x the area and handed that cascade every draw in the scene. One box
// over the volume answers it directly: no cascade selection, no camera distance, no fade.
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

// Mip level being read by CSMip (b3: b0/b1 are taken by the graphics root signature).
cbuffer MipCB : register(b3) { uint gSrcMip; uint3 _mipPad; };

// world -> [0,1] volume coords
float3 voxelUVW(float3 wp) { return (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w; }
bool insideVolume(float3 uvw) { return all(uvw >= 0.0) && all(uvw <= 1.0); }

// ---- cone tracing ----
// Marches a cone through the volume, widening with distance and reading a coarser mip each step.
// Returns front-to-back composited, premultiplied radiance; alpha is coverage.
float4 traceCone(float3 originWS, float3 dir, float aperture) {
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x); // one voxel, world units
    float dist = voxelWorld * 2.0;
    float4 acc = 0;
    [loop] for (int step = 0; step < 24; ++step) {
        if (acc.a >= 0.95 || dist > gVoxelParams.z) break;
        float diameter = max(voxelWorld, 2.0 * aperture * dist);
        float mip = log2(diameter / voxelWorld);
        float3 uvw = voxelUVW(originWS + dir * dist);
        if (!insideVolume(uvw)) break;
        float4 s = gVoxelTex.SampleLevel(gVoxelSamp, uvw, mip);
        acc += (1.0 - acc.a) * s;
        dist += diameter;
    }
    return acc;
}

// Cosine-weighted gather of six cones over the hemisphere: one along the normal, five in a ring.
// Returns the indirect diffuse radiance and, through `ao`, the ambient occlusion.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);
    const float aperture = 0.577;              // ~60 degree cone

    float4 sum = traceCone(wpos, N, aperture);
    float occ = sum.a;
    float wsum = 1.0;
    [unroll] for (int k = 0; k < 5; ++k) {
        float ang = 1.2566 * k;                // 2*pi/5
        float3 d = normalize(N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866);
        float w = saturate(dot(N, d));
        float4 c = traceCone(wpos, d, aperture);
        sum += c * w; occ += c.a * w; wsum += w;
    }
    sum /= wsum; occ /= wsum;
    ao = saturate(1.0 - occ);
    // THE SAME CEILING THE INJECTION ALREADY HAS, applied AFTER the intensity multiply. Every
    // voxel this cone gathered was clamped to AVER_VOX_MAXRAD on the way in (see PSVoxel), so
    // `sum` cannot exceed it either -- and then gVoxelParams.y, the authored giIntensity, is let
    // as high as 8 (Voxi.cpp clamps it there), which multiplies a bounded quantity straight back
    // out of its bound: 16 x 8 = 128 units of radiance, out of a volume in which nothing emits
    // more than 16.
    //
    // That is the runaway Voxi.hpp documents -- enough large, saturated, brightly-lit geometry and
    // the bounce floods the frame with that geometry's colour. The bound is a PHYSICAL statement
    // rather than a tuned number (a gather cannot hand back more radiance than the brightest thing
    // it gathered from emits), which is why it reuses the injection's own constant instead of
    // introducing a second one that would then have to be kept in step with it.
    //
    // A CLAMP IS NOT A LIGHTING MODEL. This stops a divergence; it does not make the answer right
    // at the ceiling, and a scene that reaches it is still asking for more light than the volume
    // holds. It fires in nothing shipped with this engine -- ElectricDreams, the gate scene and
    // the furnace were all measured bit-identical across this change.
    return min(sum.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
}

// The Voxi lit pixel shader. Voxi supplies light transport only — sun visibility, sky, bounce —
// and the material shades it. Returns linear radiance; the post chain tonemaps.
float4 PSMainVoxi(VSOut i) : SV_TARGET {
    float3 N = normalize(i.nrmWS);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));
#if AVER_RT
    float sunVis;
    if (gShadowParams.z > 0.5)
        sunVis = rtShadowTemporal(i.wpos, N, L, i.pos.xy, ddx(i.wpos), ddy(i.wpos),
                                  (uint)max(gRtParams.y, 1.0));
    else                       sunVis = shadowFactor(i.wpos, N, ndl);
#else
    const float sunVis = shadowFactor(i.wpos, N, ndl);
#endif
    float ao = 1.0;
    float3 ind = 0;
    if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);

    AverVertex vtx = averVertexOf(i);
    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;

    AverSurface s = averEvalMaterial(vtx, sun);
    float4 display;
    if (averDisplayColour(s, display)) return display;

    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 R = reflect(-V, averShadingNormal(s));
    AverIndirect ind4;
    ind4.ambient      = averSkyIrradiance(averShadingNormal(s));
    ind4.ambientScale = gAmbient.r;
    ind4.diffuse      = ind;
    ind4.occlusion    = ao;
#if AVER_RT
    // Ray traced when the acceleration structure and the geometry table are both there. Preferred
    // over the cone trace unconditionally: the cone is bounded by the voxel volume and this is not.
    // A MIRROR RAY IS ONLY RIGHT FOR A SMOOTH SURFACE. The cone path it replaced widened its
    // aperture with roughness; a single ray has no aperture at all, so applying it to a rough
    // surface hands back a sharp reflection where a blurred one belongs -- brighter than the truth
    // and visibly wrong. Above the cutoff the sky term is used, which is what a very rough surface
    // reflects anyway.
    if (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.5) {
        bool specHit = false;
        float3 refl = rtReflectionTemporal(i.wpos, N, R, L, i.pos.xy, specHit);
        // Faded out toward the cutoff so a surface does not pop between the two models as its
        // roughness crosses a threshold.
        ind4.specular = lerp(specHit ? refl : skyColor(R), skyColor(R),
                             saturate(s.rough * 2.0));
    } else
#endif
    if (gVoxelParams.w > 0.5) {
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(i.wpos, R, specAperture);
        // Bounded for the reason coneTracedIndirect is. The sky term is left alone: skyColor is
        // not a gather out of the volume and carries no runaway of its own.
        ind4.specular       = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD) +
                              skyColor(R) * (1.0 - sceneSpec.a);
    } else {
        ind4.specular       = skyColor(R);
    }

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind4);
    radiance = averApplyFog(radiance, i.wpos);
    return float4(radiance, averOpacity(s));
}

// ================= ray-driven primary visibility (experimental) =================
// THE ONLY THING THIS REPLACES IS "WHAT DID THIS PIXEL SEE". Everything after the first hit is
// the same work PSMainVoxi does -- a sun shadow ray, sky ambient, fog -- because the rasteriser
// was never doing any of that. It answered the visibility question and nothing else, and this
// answers the same question with a ray.
//
// WHAT IT GIVES UP, stated here because no amount of tuning recovers it: hardware early-Z. A
// rasterised fragment that turns out to be hidden is discarded before its shader ever runs; a ray
// pays the whole traversal to discover the same thing. That is the trade this mode exists to
// measure, and the number to beat is in Settings::rtRenderMode.
//
// UNTEXTURED, DELIBERATELY, in this first version. RtInstance carries a flat albedo and no UV (see
// its declaration near the top of this file), so a hit cannot sample a material texture -- exactly
// the approximation rtReflection already ships with. COLOUR WILL DIFFER from the raster image.
// GEOMETRY MUST NOT, and that is what the side-by-side capture is checking.
//
// BEHIND AVER_RT because RayQuery is: this entry point only compiles into the SM 6.5 variant, and
// VoxiRenderer refuses the mode outright when the device has no ray-query support.
#if AVER_RT
struct RayDrivenOut {
    float4 col   : SV_TARGET;
    float  depth : SV_DEPTH;
};

RayDrivenOut PSRayDriven(SkyOut i) {
    RayDrivenOut o;

    // The same NDC-to-world-ray reconstruction PSVoxelDebug does, through the same gInvViewProj,
    // so the primary ray and the debug raymarch cannot disagree about where a pixel looks.
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 dir = normalize(far.xyz / far.w - gCamPos.xyz);

    RayDesc r;
    r.Origin    = gCamPos.xyz;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = 1.0e7;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        // A miss is the sky at the far plane. Depth 1, not 0 -- this engine's projection is not
        // reversed, and writing 0 here would put the sky in front of everything drawn after it.
        o.col   = float4(skyColor(dir), 1.0);
        o.depth = 1.0;
        return o;
    }

    // Surface reconstruction: the same barycentric interpolation and the same ROTATION-ONLY normal
    // transform rtReflection performs. Its comment explains why the inverse transpose is
    // deliberately not carried (these are rigid instances); the same holds here, and the two must
    // agree or a surface would shade differently depending on whether it was seen directly or in a
    // mirror.
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

    float3 wpos = gCamPos.xyz + dir * q.CommittedRayT();
    float3 L    = normalize(gLightDir.xyz);

    // THE SHADOW RAY IS THE SAME CALL THE RASTER PATH MAKES, temporal wrapper and all, so the two
    // modes pay identical shadow cost and the timing difference between them is primary visibility
    // ALONE. It is keyed by pixel, and this pass covers the same pixel grid, so the history buffer
    // means the same thing here as it does there.
    //
    // NO FOOTPRINT DERIVATIVES. ddx/ddy of a ray-traced hit position are garbage across a
    // silhouette -- neighbouring lanes in the same quad can land on different surfaces entirely --
    // and feeding that to the disc sampler would widen the penumbra by whatever the depth
    // discontinuity happened to be. Passing zero asks for the un-spread disc, which is what
    // rtReflection already passes for the same reason.
    float sunVis = rtShadowTemporal(wpos, N, L, i.pos.xy, float3(0,0,0), float3(0,0,0),
                                     (uint)max(gRtParams.y, 1.0));

    // Lambertian exitant radiance, with the /PI on the direct term -- see rtReflection's own
    // comment for what omitting it cost last time (every sunlit surface 3.14x too bright, which
    // reads as an exposure bug rather than a units one, and which the white furnace does not catch
    // because it turns the sun off).
    // ---- the bounce loop ----------------------------------------------------------------
    // PATH TRACING HERE IS EXTRA RAYS ON THE LOOP ABOVE, not a second renderer. The first hit
    // has already been found and shaded the way rtReflection shades its own hit; every further
    // bounce repeats exactly that, carrying a throughput and adding what each surface emits
    // toward the previous one.
    //
    // COSINE-WEIGHTED, so the 1/PI of the Lambertian BRDF and the cosine of the rendering
    // equation cancel against the pdf and the throughput is a plain albedo multiply. Getting
    // this wrong is the /PI mistake rtReflection already paid for once, in the other direction.
    //
    // SCREEN-PINNED HASH, no per-frame jitter, matching rtShadow's own seeding: the gate oracle
    // compares nine configurations bit-exactly, and a frame counter in the seed makes every one
    // of them a different image. That means the noise is a fixed dither rather than something
    // that converges over time -- honest for a first cut, and the thing a temporal accumulator
    // would fix.
    // THE ENGINE'S OWN BRDF, not a second one written here. averShadeDirect is the same
    // Cook-Torrance GGX PSMainVoxi shades through; building an AverSurface by hand and handing it
    // over is what stops the ray image and the raster image disagreeing about what a material
    // looks like for reasons that are nobody's intent. The /PI lives inside it (kdAlbedo / PI),
    // which is the divide rtReflection had to learn the hard way.
    //
    // TWO CONSTANTS ARE DEFAULTED because they are per-MATERIAL and a ray hit has no material
    // constant buffer bound: reflectance 0.04 (the dielectric F0 every renderer starts from) and
    // f90 1.0. A material that authored either differently will shade slightly differently here
    // than it does under the rasteriser -- a real, bounded difference, and the same one that will
    // disappear when a hit can reach its own material.
    AverSurface s = (AverSurface)0;
    s.N        = N;
    s.V        = -dir;
    s.H        = normalize(s.V + L);
    s.albedo   = inst.albedo;
    s.metallic = saturate(inst.metallic);
    s.rough    = clamp(inst.roughness, 0.045, 1.0);   // the same floor averEvalMaterial clamps to
    s.ndv      = saturate(dot(s.N, s.V));
    s.f90      = 1.0;
    s.F0       = lerp((0.04).xxx, s.albedo, s.metallic);
    s.F        = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo;
    s.model    = AVER_MODEL_STANDARD;
    s.alpha    = 1.0;
    s.occlusion = 1.0;

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;

    float3 radiance = averShadeDirect(0.0, s, sun);
    radiance += s.kdAlbedo * averSkyIrradiance(N) * gAmbient.r;

    // THE BOUNCE CARRIES THE DIFFUSE RESPONSE, not the raw albedo. A metal reflects almost
    // nothing diffusely, so multiplying a path's throughput by base colour would light an
    // interior with bounced light off surfaces that do not bounce it.
    float3 throughput = s.kdAlbedo;

    const uint bounces = (uint)max(gPtBounceParams.x, 1.0);
    float3 bp = wpos;
    float3 bn = N;
    [loop] for (uint b = 1; b < bounces; ++b) {
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
        qb.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, rb);
        qb.Proceed();

        if (qb.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            // The ray escaped: the sky is the last thing it sees, and the path ends there.
            radiance += throughput * skyColor(dirB) * gAmbient.r;
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
        // Diffuse response again, for the reason stated at the primary hit: a cosine-weighted
        // bounce is sampling the DIFFUSE lobe, so the throughput is the diffuse albedo and a
        // metal correctly contributes almost nothing to it.
        throughput *= (1.0 - saturate(bi.metallic)) * bi.albedo;

        // ONE shadow ray per bounce, no disc -- the penumbra of a surface seen only through two
        // diffuse bounces is not resolvable, and this is the single largest cost in the loop.
        float bshadow = rtShadow(bp, bn, L, i.pos.xy, float3(0,0,0), float3(0,0,0), 1u, 0.0);
        float3 bdirect = averSunRadiance() * saturate(dot(bn, L)) * bshadow / PI;
        radiance += throughput * bdirect;
    }

    radiance = averApplyFog(radiance, wpos);

    // Depth for everything that draws AFTER the scene -- the deferred sky, transparentPass, the
    // particle pass. Without it they have nothing to test against and sort against a cleared
    // buffer, which puts smoke in front of walls.
    float4 clip = mul(float4(wpos, 1.0), gViewProj);
    o.depth = clip.w > 1e-6 ? saturate(clip.z / clip.w) : 1.0;
    o.col   = float4(radiance, 1.0);
    return o;
}
#endif  // AVER_RT

// ================= depth prepass =================
// Same-frame depth-only pass -- see VoxiRenderer.hpp's depthPrepassPipeline() and
// D3D12Device::drawMesh for the whole mechanism. Paired with VSMain (the SAME compiled vertex shader
// PSMainVoxi's own pipelines use, not a second copy of it), so this writes EXACTLY the depth the
// colour pass's own rasterisation would have produced for the identical triangle.
//
// WRITES NO COLOUR -- the pipeline this compiles into declares renderTargetCount = 0 -- and reads
// only enough of the material to answer one question: does this fragment survive alpha test. That
// is deliberately far short of averEvalMaterial(), which this does NOT call: averEvalMaterial also
// samples the metal-rough, normal, occlusion and emissive maps and does the Fresnel/GGX setup around
// them, none of which a depth-only fragment has any use for. Calling it here to reach one field
// (s.alpha) would make the "cheap prepass" pay four texture fetches instead of at most one.
//
// STILL NOT FREE, though, and the task this pass exists for says to be honest about the cost: EVERY
// covered pixel pays a branch on gMaterialFlags (that flag lives in the SAME AverMaterial cbuffer as
// everything else a material declares, so there is no way to know "is this alpha-tested" without at
// least reading it), and an alpha-tested material additionally pays one Sample() against
// gBaseColorMap plus the multiply/compare below. What it buys back is skipping PSMainVoxi entirely --
// a shadow-cascade lookup, up to eight cone traces, ray-traced-history blending and fog -- on every
// fragment this pass determines is hidden, which is the entire point: one cheap sample now instead
// of one expensive shader later, repeated for whatever overdraw sits behind it.
//
// DOES NOT EVALUATE AVER_MAT_SLOPE_BLEND's second layer (see averBlendLayers in PbrShaders.cpp): that
// flag is landscape-only in this codebase, and the landscape is drawn through a wholly separate call
// site (LandscapeRenderer::draw(), never through IDevice::drawMesh/drawMeshDepthPrepass) that this
// prepass never reaches in the first place -- see SandboxApp.cpp's own comment on why the landscape
// is one of this feature's three excluded paths. If a non-landscape material is ever authored with
// both AVER_MAT_SLOPE_BLEND and AVER_MAT_ALPHA_MASK set, this function's alpha would be the FIRST
// layer's alone; that combination does not exist in this tree today.
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

// VSShadow's instanced twin: one DrawIndexedInstanced call submits every surviving instance of one
// mesh in one cascade, instead of shadowPass calling drawMesh() once per instance. The world
// transform comes from gInstanceWorlds[instanceID] -- written by VoxiRenderer::shadowPass via
// IRenderContext::drawMeshInstanced -- rather than from PerObject's gWorld, which this entry point
// never reads. Everything else (the cascade's view-projection, the depth-only output) is identical
// to VSShadow.
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
// nr[k] went through averTransformNormal(v.nrm, gWorld) rather than a plain mul(float4(nrm,0),
// gWorld) so that this path actually matches VSVoxel below (and VSMain, ActorPreview's vertex
// shader and MSClusterMain) -- see 1856da1 "Render: a normal transform, two GPU races, and a seam
// along every terrain section", which fixed all four OTHER call sites of this exact bug and missed
// this fifth one because MSVoxel is compiled only behind AVER_MS, which nothing had turned on yet.
// A plain mul is correct only under rotation and uniform scale; every normal with components on
// more than one axis reads wrong the moment an entity's scale goes non-uniform, and this pass
// backs indirect lighting -- a mis-shaded normal here does not flicker on one triangle, it tints an
// entire surface's bounce light for as long as the volume holds it.
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
    // Exitant radiance, not radiosity: the sun term is an irradiance so it takes the 1/PI, the sky
    // term is already a radiance so it does not.
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility / PI
                                + averSkyIrradiance(N) * gAmbient.r);
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

// Zeroes the accumulator before injection.
[numthreads(4,4,4)]
void CSClear(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    [unroll] for (uint k = 0; k < 4; ++k) gVoxelAccum[a + uint3(k,0,0)] = 0;
}

// Turns the fixed-point sums into mip 0 of the filterable RGBA16F volume: the mean radiance of the
// fragments that covered each voxel.
[numthreads(4,4,4)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    uint n = gVoxelAccum[a + uint3(3,0,0)];
    if (n == 0) { gVoxelUAV[id] = 0.0; return; }
    float3 s = float3(gVoxelAccum[a], gVoxelAccum[a + uint3(1,0,0)], gVoxelAccum[a + uint3(2,0,0)]);
    gVoxelUAV[id] = float4(s / (AVER_VOX_FIXED * (float)n), 1.0);   // alpha = occupancy
}

// ================= Voxi: mip filtering =================
// Box-filters radiance and occupancy from one mip into the next. The mip binding set puts a
// SINGLE-MIP view of the source at t0 and the destination level at u0.
[numthreads(4,4,4)]
void CSMip(uint3 id : SV_DispatchThreadID) {
    int3 s = int3(id) * 2;
    float4 a = 0;
    [unroll] for (int x=0;x<2;++x)
    [unroll] for (int y=0;y<2;++y)
    [unroll] for (int z=0;z<2;++z)
        a += gVoxelTex.Load(int4(s + int3(x,y,z), gSrcMip));
    gVoxelUAV[id] = a * 0.125;
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
)";

} // namespace aver::voxi
