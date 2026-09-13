// voxi_cone.hlsli -- the voxel cone tracer: traceCone/coneTracedIndirect and the two small
// world<->volume helpers they (and one other file) share, voxelUVW/insideVolume. Split out of
// voxi.hlsl's own untouched tail, UNCONDITIONALLY -- no #if AVER_RT of its own, because this IS the
// non-RT path: the diffuse GI estimator PSMainVoxi/PSVoxel/PSVoxelDebug fall back to (and that
// PSRayDriven still uses too, gated only by Settings::giMode, never by AVER_RT) on hardware with no
// ray tracing at all. It must stay textually AFTER the #if AVER_RT / #endif region closes, exactly
// where it always compiled -- see "WHAT MUST PRECEDE" below for why moving it earlier would be a
// behaviour change, not a pure move.
//
// WHAT THIS FILE HOLDS, IN DECLARATION ORDER:
//   - voxelUVW, insideVolume: world position <-> [0,1] volume-space helpers.
//   - traceCone: marches one cone through the volume, widening with distance and reading a
//     coarser mip each step.
//   - coneTracedIndirect: the cosine-weighted multi-cone hemisphere gather built on traceCone,
//     returning diffuse indirect radiance and (via `ao`) ambient occlusion.
//
// WHAT MUST PRECEDE THIS FILE'S #include LINE IN voxi.hlsl:
//   - cbuffer VoxiFrame (gVoxelOrigin, gVoxelParams, gGiParams -- all read here) and
//     gVoxelTex/gVoxelSamp (t0/s0), all declared far above in voxi.hlsl, before the #if AVER_RT
//     guard even opens. NOT moved into this file, even though they look at a glance like "the
//     volume resource declarations" that belong with the cone tracer: gVoxelTex/gVoxelSamp are
//     ALSO sampled directly inside voxi_rt.hlsli (its AVER_AO_UNIFIED hit branch), which is
//     #include'd long before this file's own #include point. Moving their declaration down here
//     would undeclare them there. gVoxelUAV/gVoxelAccum (u0/u1), declared in the same original
//     block, belong to CSClear/CSResolve/CSMip -- the voxelisation WRITE side -- not to this
//     file's read-only gather, and are left where they are for the same reason. AVER_VOX_FIXED/
//     AVER_VOX_MAXRAD/AVER_VOX_INJECT_APERTURE/AVER_VOX_FEEDBACK are defined with that same block
//     and stay there too; this file uses only AVER_VOX_MAXRAD, already visible by the time this
//     file is reached. AVER_VOX_MAXRAD itself now expands to a read of cbuffer VoxiFrame's own
//     gViewParams.y (the live GI radiance ceiling -- see that macro's own comment in voxi.hlsl),
//     which is the SAME cbuffer this file already depends on, so nothing new needs to precede this
//     #include beyond what already does.
//   - the entire #if AVER_RT region (voxi_rt.hlsli, voxi_restir.hlsli, and the rest of voxi.hlsl's
//     own RT-only code between them) and its #endif, plus the unconditional shadow helpers
//     immediately after it (shadowSampleCascade/shadowFactor/giShadowFactor) and cbuffer MipCB
//     (b3, CSMip's own constant, unrelated to this file and deliberately left where it sits).
//     THIS FILE MUST NOT BE PULLED EARLIER THAN THAT #endif: voxi_restir.hlsli's own comments
//     mention coneTracedIndirect (giRestirIndirect calls itself "a drop-in replacement for
//     coneTracedIndirect's own contract") but never actually CALL it -- there is no real ordering
//     dependency there, only a documented one -- so satisfying it literally by including this file
//     ahead of voxi_restir.hlsli would put traceCone/coneTracedIndirect INSIDE the AVER_RT guard
//     and silently delete the entire non-RT diffuse-GI path the first time someone builds with
//     ray tracing off. Reported rather than done; see the split agent's own note.
//
// WHAT DEPENDS ON THIS FILE:
//   - PSMainVoxi, PSVoxel, PSVoxelDebug (further down voxi.hlsl): call coneTracedIndirect/
//     traceCone/voxelUVW/insideVolume directly.
//   - PSRayDriven: calls coneTracedIndirect for its own cone-gather diffuse term whenever ReSTIR GI
//     is off (gGiRestirParams.x at its call site).
//   - voxi_rt.hlsli's rtAmbientTraced, upstream of this file's own #include point: it forward-
//     declares voxelUVW/insideVolume itself (see that file's own header comment for why) and is
//     satisfied by this file's real definitions appearing later in the same translation unit --
//     ordinary declare-then-define, unaffected by which file the definition now lives in.

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

    // APERTURE FROM THE CONE COUNT, not a constant -- and this is what made a higher GI tier look
    // BLURRIER rather than sharper.
    //
    // It was a fixed 0.577 (a 60-degree cone) at every tier. Six of those roughly tile a hemisphere,
    // which is the classic VXGI configuration and where the number came from. Thirteen of them --
    // Epic's rung, and what this project runs -- cover it more than twice over, so raising GI
    // quality bought overlap instead of detail. Worse, a 60-degree cone's diameter grows about 2.15x
    // per march step, so it is sampling mip 7-8 within a couple of metres: texels tens of metres
    // across, i.e. a scene-wide average. That is precisely an ambient term, and it is why bright
    // sunlit floor never showed up on the column standing next to it.
    //
    // Tiling the hemisphere ONCE with N cones gives each a solid angle of 2pi/N, so
    // 2pi(1 - cos(theta)) = 2pi/N and cos(theta) = 1 - 1/N. Narrower cones climb the mip chain more
    // slowly and therefore keep near-field detail, which is the whole of what bounced light looks
    // like. N=6 lands at 33.6 degrees, N=13 at 22.6.
    const float cones    = max(gGiParams.x, 1.0);
    const float cosHalf  = saturate(1.0 - 1.0 / cones);
    const float aperture = sqrt(max(1.0 - cosHalf * cosHalf, 1e-6)) / max(cosHalf, 1e-6);   // tan

    float4 sum = traceCone(wpos, N, aperture);
    float wsum = 1.0;
    // AO IS AVERAGED ONLY OVER CONES THAT HAD VOLUME TO MARCH. traceCone breaks the instant a sample
    // leaves the voxel volume and returns whatever alpha it had, near zero for a cone that exits
    // early -- which a plain average reads as "nothing occluding this direction", crediting the
    // surface with full sky. A cone that left the volume has NO INFORMATION about occlusion, which
    // is not the same as information that nothing is there, so it is excluded from the average
    // rather than voting "open". The radiance sum still takes every cone: leaving the volume does
    // genuinely mean no more bounced light was found along that direction.
    //
    // The far endpoint is the test rather than a flag out of traceCone, because this function is
    // mirrored byte-for-byte in voxi.hlsl and voxi_gi.hlsli and a signature change is two files and
    // a new way for them to drift. A cone whose FAR end is inside the volume never broke early.
    float occW = insideVolume(voxelUVW(wpos + N * gVoxelParams.z)) ? 1.0 : 0.0;
    float occ = sum.a * occW;
    float occWsum = occW;
    // THE RING COMES FROM THE QUALITY TIER NOW, not a hardcoded 5: the axial cone along N is always
    // traced, so gGiParams.x is the TOTAL and the ring is one fewer.
    // A DYNAMIC [loop], not [unroll]: an unrolled loop with a dynamic bound is predicated, not
    // skipped, so every tier would still pay for six cones and the ladder would be a lie.
    // Angle is 2*pi/ring computed, not the 1.2566 literal (a rounded 2*pi/5) that stood here -- the
    // six-cone case shifts ~3e-5 radians, no longer bit-identical, in the more correct direction.
    // A COSINE-DISTRIBUTED SPIRAL, NOT ONE RING. Every extra cone used to land at the SAME
    // elevation -- `N * 0.5 + tangent * 0.866` is a fixed 60-degree tilt -- so Epic's thirteen cones
    // were one axial plus twelve crammed into a single band. More cones bought more samples of one
    // elevation rather than coverage of the hemisphere, which is the other half of why the gather
    // behaved like an ambient term no matter how high the tier went.
    //
    // t is stratified over [0,1) and cos(theta) = sqrt(1-t) gives the cosine-weighted hemisphere
    // distribution a diffuse gather actually wants; the golden angle in azimuth keeps successive
    // directions from clumping the way a constant step does. The `w = dot(N,d)` cosine weight below
    // is kept -- it is what makes this a weighted average rather than a plain mean, and the axial
    // cone still carries weight 1.
    const uint ring = (uint)cones - 1u;
    [loop] for (uint k = 0; k < ring; ++k) {
        float t    = ((float)k + 0.5) / (float)ring;
        float cosT = sqrt(saturate(1.0 - t));
        float sinT = sqrt(saturate(t));
        float ang  = 2.39996323 * (float)k;
        float3 d = normalize(N * cosT + (T * cos(ang) + B * sin(ang)) * sinT);
        float w = saturate(dot(N, d));
        float4 c = traceCone(wpos, d, aperture);
        sum += c * w; wsum += w;
        const float cw = w * (insideVolume(voxelUVW(wpos + d * gVoxelParams.z)) ? 1.0 : 0.0);
        occ += c.a * cw; occWsum += cw;
    }
    sum /= wsum;
    // EVERY cone left the volume: there is genuinely nothing here to occlude against, so the old
    // answer (fully open) is right, and that case must not change.
    occ = occWsum > 1e-4 ? occ / occWsum : 0.0;
    ao = saturate(1.0 - occ);
    // THE SAME CEILING THE INJECTION ALREADY HAS, applied AFTER the intensity multiply. Every voxel
    // is clamped to AVER_VOX_MAXRAD on the way in (PSVoxel), but gVoxelParams.y (giIntensity, up to
    // 8 per Voxi.cpp) multiplies that bounded quantity straight back out: 16 x 8 = 128, out of a
    // volume where nothing emits more than 16 -- the runaway Voxi.hpp documents (enough large,
    // saturated geometry floods the frame with its colour). PHYSICAL bound, not tuned: a gather can't
    // hand back more radiance than the brightest thing it gathered emits, so it reuses the
    // injection's own constant rather than a second one to keep in step.
    // A CLAMP IS NOT A LIGHTING MODEL: it stops divergence, not correctness at the ceiling. Fires in
    // nothing shipped -- ElectricDreams, the gate scene, the furnace all measured bit-identical.
    return min(sum.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
}
