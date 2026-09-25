// Voxi's GI prelude: shadowFactor, traceCone, coneTracedIndirect and the cascade sampler.
//
// A PRELUDE, NOT AN ENTRY POINT -- it declares no VS/PS/CS and is prepended to whichever shader
// asks for GI (PSVoxel, the particle pass, the sandbox cluster PS). Its registers arrive as -D from
// voxi::giShaderDefines(), so the same text binds at whatever slots the caller's layout left free.
// .hlsli rather than .hlsl for that reason: included, never compiled alone.
//
// THE CUSTOM R"HLSL( DELIMITER IT USED TO NEED IS GONE WITH THE LITERAL, and the reason it existed
// is worth keeping: the #error message below names giShaderDefines(), and that call's closing `()"`
// ends with exactly the two characters that terminate a bare R"(...)" -- which silently truncated
// the string there and turned every #if below into unbalanced preprocessor directives. In a file,
// text is just text. (That same trap caught the migration itself: a regex looking for the literal
// matched the R"(  written inside the comment explaining the trap.)

#if !defined(AVER_GI_SRV) || !defined(AVER_GI_SAMPLER) || !defined(AVER_GI_FRAME_REG)
#error "aver/voxi/VoxiGiShaders.hpp needs AVER_GI_SRV / AVER_GI_SAMPLER / AVER_GI_FRAME_REG from giShaderDefines()"
#endif
#define AVER_GI_JOIN2(a, b) a##b
#define AVER_GI_JOIN(a, b) AVER_GI_JOIN2(a, b)

// Cascade count is fixed at 4 on Voxi's own side too (VoxiShaders.hpp's identically-named define);
// never seen in the same compile as that one, so redefining it here is not a collision.
#define AVER_SHADOW_CASCADES 4

// The ceiling on voxel radiance, and the same arrangement as the line above: VoxiShaders.hpp
// defines this identically for the raster path, the two preludes are never in one compile, and
// the value has to match. It bounds BOTH ends of the volume -- what PSVoxel injects and what
// coneTracedIndirect hands back -- so a change here is a change to VoxiShaders.hpp too.
//
// NOW A LIVE PER-FRAME VALUE, not a compile-time constant -- rides gViewParams.y, exactly as
// voxi.hlsl's own copy of this macro does (that file's comment on the define has the full story:
// Settings::giRadianceCeiling, the 0-means-unset fallback to today's 16.0, and why every use site
// stays live without being rewritten). This prelude's own cbuffer VoxiFrame below mirrors
// voxi.hlsl's gViewParams byte for byte -- see its own header comment -- so the SAME per-frame
// value (VoxiRenderer::giFrameConstants() is one shared struct, not two) reaches every caller of
// coneTracedIndirect (the particle pass, the sandbox cluster PS) as it does PSVoxel/PSMainVoxi.
#define AVER_VOX_MAXRAD (gViewParams.y > 0.0 ? gViewParams.y : 16.0)

// BYTE FOR BYTE aver::voxi::VoxiRenderer::FrameConstants (VoxiRenderer.hpp), same as Voxi's own
// `cbuffer VoxiFrame` in VoxiShaders.hpp -- deliberately the FULL block, not a trimmed one, so a
// caller can bind VoxiRenderer::giFrameConstants()/giFrameConstantBytes() verbatim instead of the
// engine needing a second, easy-to-drift mirror of the same struct. shadowFactor()/
// coneTracedIndirect() below only ever read the first five fields; the rest (RT history, GI-only
// shadow) sit here unread, exactly as over-provisioned as the SRV table above them.
cbuffer VoxiFrame : register(AVER_GI_JOIN(b, AVER_GI_FRAME_REG)) {
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    float4x4 gCascadeViewProj[AVER_SHADOW_CASCADES];
    float4   gCascadeSplit[AVER_SHADOW_CASCADES];
    float4   gShadowParams; // x = 1/atlasSize, y = enabled, z = accel structure built, w = cascades
    float4   gShadowDraw;
    float4   gRtParams;
    float4   gRtHistParams;
    float4x4 gPrevViewProj;
    float4   gSceneViewport;
    // FOUR FIELDS THAT WERE MISSING HERE, and their absence was not cosmetic. This block is
    // bound from giFrameConstants(), which hands over &cb_ -- the WHOLE FrameConstants struct
    // -- so a declaration that skips a field does not skip the bytes: it shifts every field
    // after it. These four (added to voxi.hlsl and the C++ struct with the viewport-relative
    // NDC fix, the camera medium and caustics) were never mirrored here, leaving everything
    // below reading 64 bytes early. The live consequence was the cone gather at the bottom of
    // this file: gGiParams.x, its ring count, landed inside gGiShadowViewProj and read a
    // matrix element as a cone count.
    //
    // NOTHING HERE READS THEM. They are declared for their SIZE, exactly as gAmbientParams
    // below is, and the header comment already says this block is deliberately the full
    // struct rather than a trimmed one for precisely this reason.
    //
    // The static_assert in VoxiRenderer.hpp guards the C++ struct against voxi.hlsl and says
    // so by name. It cannot see THIS file, which is how the two drifted apart while the guard
    // stayed green -- so when you append there, append here too.
    float4   gSceneViewportCur;
    float4   gCameraMedium;
    float4   gCausticMin;
    float4   gCausticMax;
    float4x4 gGiShadowViewProj;
    // Mirrors voxi.hlsl's gGiShadowParams field for field, including w -- now a runtime bit-field
    // (T1/T2/T3's rtSecondaryShadowOpaque/rtSkyOcclusionHalfRate/rtReflectionHalfRate toggles; see
    // that file's own comment on the field for what each bit decodes to and who reads it: rtReflection
    // and giTraceInitialCandidate for bit 1, rtSkyOcclusionTemporal for bit 2, rtReflectionTemporalEx
    // for bit 4, all in voxi_rt.hlsli/voxi_restir.hlsli/voxi.hlsl). NOTHING IN THIS PRELUDE READS ANY
    // OF IT -- declared for layout only, same as gShadowDraw/gRtParams/gRtHistParams above.
    float4   gGiShadowParams;
    // THE TAIL THIS MIRROR WAS MISSING. The comment above calls this block byte for byte
    // FrameConstants, and it stopped three float4s short of being that -- harmless while nothing
    // here read past gGiShadowParams, and exactly the drift that comment exists to prevent. The
    // gather below now needs gGiParams, so the tail is declared rather than assumed.
    float4   gRtDenoiseParams;
    float4   gPtBounceParams;
    float4   gGiParams;
    // Mirrors gAmbientParams. NOTHING IN THIS PRELUDE READS IT -- it is here because this block is
    // declared as the FULL FrameConstants so a caller can bind giFrameConstants() verbatim (see the
    // header note above), and a shorter declaration would quietly stop being that. Appended, never
    // inserted: every field above keeps its offset.
    //
    // gAmbientParams.z BIT 16 IS NOW DECODED HERE TOO (follow-up to the contrast-fix plan's F5/R6):
    // this file's own coneTracedIndirect below was never mirrored byte-for-byte against voxi_cone.
    // hlsli's -- it kept the older fixed-elevation single ring (`N * 0.5 + tangent * 0.866`, aperture a
    // constant 0.577) after voxi_cone.hlsli moved to a cosine-stratified, golden-angle ring with a
    // cone-count-derived aperture, so the cluster-material and particle passes were getting a THIRD,
    // uncorrected diffuse-gather estimate next to voxi_cone.hlsli's legacy-off and legacy-on ones.
    // coneTracedIndirect below now carries the same corrected gather, reading the bit itself rather
    // than through a shared helper (same as every other reader of this bitmask). Its legacy branch
    // (bit ON) restores THIS FILE'S OWN pre-fix gather byte-for-byte, not voxi_cone.hlsli's: the two
    // files' "legacy" differs because this file's old aperture was a fixed constant threaded through
    // the whole gather (axial cone included), where voxi_cone.hlsli's aperture was already cone-count-
    // derived before R6 and stayed that way under its own bit 16 -- see coneTracedIndirect's own
    // comment below for why that forces the branch wider than a single ring-loop if/else here.
    // gAmbientParams.z bit 16 therefore now affects PSMainVoxi/PSVoxel/PSVoxelDebug/PSRayDriven
    // (voxi_cone.hlsli) AND the particle pass and PSClusterMain (this file) alike.
    //
    // gAmbientParams.z ALSO NOW CARRIES BIT 32 (W6/M5, optimisation-wave-2 plan section 4): a
    // blended-replay fragment's per-pixel history writes are suppressed by default and bit 32
    // restores the old unconditional writes for A/B. NOTHING IN THIS PRELUDE READS IT, same as every
    // other bit of z/w this file declares for SIZE only (see this comment's own opening paragraph) --
    // the cluster-material and particle passes this file serves have no per-draw blend state of their
    // own to gate on and no history textures of the kind gAverHistoryWrite protects.
    //
    // gAmbientParams.w NOW CARRIES U1's GI-VISIBILITY MODE (bits 0-1, 4, 8) and W6/M5's own
    // blended-history bits (16, 32), the giVisPathView debug bit (64), the moving-camera ReSTIR
    // reservoir-age cap (bits 7-11) and the ReSTIR spatial-reuse sample-count override (bits 12-15) --
    // see voxi.hlsl's own gAmbientParams comment (the field this cbuffer mirrors byte-for-byte) for
    // the full bit table.
    // NOTHING IN THIS PRELUDE READS ANY OF THEM: coneTracedIndirect below has no candidate ray, no
    // reused sample and no blended-replay concept of its own -- U1 and W6/M5 are both entirely
    // voxi.hlsl's PSMainVoxi/PSRayDriven and voxi_restir.hlsli's own concern.
    float4   gAmbientParams;
    // Editor view modes the ray-driven path honours itself. x = unlit. NOTHING IN THIS
    // PRELUDE READS x, same as gAmbientParams above it -- declared for size only.
    // Mirrors FrameConstants::viewParams -- appended at the END, so every offset above is
    // untouched. See VoxiRenderer.hpp's static_assert for the guard that makes that a rule.
    // y WAS SPARE; NOW the live GI radiance ceiling -- THIS PRELUDE DOES READ IT, via
    // AVER_VOX_MAXRAD above (coneTracedIndirect's own min(..., AVER_VOX_MAXRAD) call).
    // z/w WERE ALSO SPARE; NOW the RTXDI reuse-similarity tolerances (Settings::
    // giRestirDepthThreshold/giRestirNormalThreshold) -- see voxi.hlsl's own gViewParams comment (the
    // field this cbuffer mirrors byte-for-byte) for the full story. NOTHING IN THIS PRELUDE READS
    // EITHER, same as x: coneTracedIndirect below has no RTXDI reservoir of its own to reuse.
    float4   gViewParams;
    // RTXDI ReSTIR GI control -- mirrors gGiRestirParams in voxi.hlsl and FrameConstants::
    // giRestirParams (VoxiRenderer.hpp), appended at the end for the same reason gViewParams was.
    // NOTHING IN THIS PRELUDE READS IT, same as gAmbientParams/gViewParams above it: this cbuffer
    // is declared as the FULL FrameConstants block so a caller can bind giFrameConstants()
    // verbatim (see the header note at the top of this file), and this field only exists here to
    // keep that true. ReSTIR GI itself is voxi.hlsl's own PSMainVoxi/PSRayDriven entry points --
    // it needs the ray-tracing toolkit (gScene, RtInstance, gRtMaterials) this cone-only prelude's
    // callers never bind, so the actual switch never reaches this file.
    float4   gGiRestirParams;
};

// t(AVER_GI_SRV) the GI volume, t(AVER_GI_SRV_1) the shadow map -- the only two of Voxi's table-0
// union this prelude declares a symbol for; see giShaderDefines()'s own comment on why the rest of
// kGiSrvCount/kGiUavCount are reserved by the caller's layout but never named here.
Texture3D<float4>      gVoxelTex  : register(AVER_GI_JOIN(t, AVER_GI_SRV));
SamplerState            gVoxelSamp : register(AVER_GI_JOIN(s, AVER_GI_SAMPLER));
Texture2D<float>        gShadowTex  : register(AVER_GI_JOIN(t, AVER_GI_SRV_1));
SamplerComparisonState  gShadowSamp : register(AVER_GI_JOIN(s, AVER_GI_SAMPLER_1));

// 3x3 PCF inside ONE cascade's quadrant of the atlas. Returns 1 = lit, 0 = shadowed, -1 = outside
// this cascade so the caller can try the next one. VERBATIM from VoxiShaders.hpp's own
// shadowSampleCascade -- see that file for how it was arrived at; nothing about borrowing it at a
// different base register changes what it computes.
float shadowSampleCascade(float3 wpos, uint c) {
    float4 lp = mul(float4(wpos, 1.0), gCascadeViewProj[c]);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;

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
// sun visibility, 1 = fully lit. VERBATIM from VoxiShaders.hpp's own shadowFactor -- the SAME
// non-ray-traced fallback PSMainVoxi itself falls back to when AVER_RT is off or the acceleration
// structure is not built, so a cluster drawn through this prelude is shadowed exactly as an ordinary
// draw is whenever Voxi is not ray tracing.
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

// world -> [0,1] volume coords. VERBATIM from VoxiShaders.hpp.
float3 voxelUVW(float3 wp) { return (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w; }
bool insideVolume(float3 uvw) { return all(uvw >= 0.0) && all(uvw <= 1.0); }

// Marches a cone through the volume, widening with distance and reading a coarser mip each step.
// Returns front-to-back composited, premultiplied radiance; alpha is coverage. VERBATIM from
// VoxiShaders.hpp's own traceCone.
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
//
// NO LONGER ONE VERBATIM COPY of VoxiShaders.hpp's own coneTracedIndirect. gAmbientParams.z bit 16
// (voxi.legacyConeWeights -- the same bit voxi_cone.hlsli decodes, R6 in the contrast-fix plan) now
// selects between this file's OWN pre-fix gather (ON, restored byte-for-byte below) and the SAME
// corrected cosine-stratified gather voxi_cone.hlsli's own bit-16-OFF branch runs: identical
// cone-count-derived aperture, identical golden-angle stratified ring at weight 1, identical read of
// the cone count (gGiParams.x). Before this fix the cluster-material and particle passes ran a THIRD,
// uncorrected estimate here -- a fixed 60-degree ring at one elevation, no stratification at all --
// next to voxi_cone.hlsli's two; see ConeWeightTest's own section on this file for the moments that
// scheme produced.
//
// THE SPLIT IS AROUND THE WHOLE FUNCTION, not just the ring loop the way voxi_cone.hlsli's own bit 16
// is: that file's aperture was already cone-count-derived on BOTH sides of its bit, so only the ring's
// direction/weight scheme needed branching. This file's OLD aperture was a fixed constant threaded
// through the axial cone too, so restoring it byte-for-byte needs the branch to start before the axial
// cone is even traced -- two honest copies of the code each side claims to be, not a shared aperture
// variable that would make neither branch a real copy of anything.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);

    if (((uint)gAmbientParams.z & 16u) != 0u) {
        // LEGACY, VERBATIM: this file's own coneTracedIndirect exactly as it stood before this fix,
        // restored bit-for-bit for A/B comparison only. See voxi_cone.hlsli's own bit 16 for the
        // sibling toggle in the main (non-cluster, non-particle) pass -- a DIFFERENT old scheme,
        // because this file's old aperture was fixed where voxi_cone.hlsli's was already
        // cone-count-derived even under its own legacy bit.
        const float aperture = 0.577;              // ~60 degree cone

        float4 sum = traceCone(wpos, N, aperture);
        float wsum = 1.0;
        // AO IS AVERAGED ONLY OVER CONES THAT HAD VOLUME TO MARCH. traceCone breaks the instant a
        // sample leaves the voxel volume and returns whatever alpha it had, near zero for a cone that
        // exits early -- which a plain average reads as "nothing occluding this direction", crediting
        // the surface with full sky. A cone that left the volume has NO INFORMATION about occlusion,
        // which is not the same as information that nothing is there, so it is excluded from the
        // average rather than voting "open". The radiance sum still takes every cone: leaving the
        // volume does genuinely mean no more bounced light was found along that direction.
        float occW = insideVolume(voxelUVW(wpos + N * gVoxelParams.z)) ? 1.0 : 0.0;
        float occ = sum.a * occW;
        float occWsum = occW;
        const uint  ring = (uint)max(gGiParams.x, 1.0) - 1u;
        const float dphi = ring > 0u ? 6.2831853 / (float)ring : 0.0;
        [loop] for (uint k = 0; k < ring; ++k) {
            float ang = dphi * (float)k;
            float3 d = normalize(N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866);
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
        return min(sum.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
    }

    // CORRECTED (default). The math below is the same text as voxi_cone.hlsli's own corrected gather
    // (its coneTracedIndirect, legacyConeWeights == false branch), whitespace aside -- ConeWeightTest
    // checks the two are identical once indentation is normalised; they cannot be raw-byte-identical
    // because voxi_cone.hlsli's copy lives one nesting level deeper, inside its own ring-loop if/else,
    // than this top-level branch does.
    //
    // APERTURE FROM THE CONE COUNT, not a constant -- see voxi_cone.hlsli's own comment on this exact
    // formula for the derivation (tiling the hemisphere with N cones gives each cos(theta) = 1 - 1/N)
    // and why this file's OLD fixed 0.577 (the legacy branch above) went blurrier, not sharper, at
    // higher GI tiers.
    const float cones    = max(gGiParams.x, 1.0);
    const float cosHalf  = saturate(1.0 - 1.0 / cones);
    const float aperture = sqrt(max(1.0 - cosHalf * cosHalf, 1e-6)) / max(cosHalf, 1e-6);   // tan

    float4 sum = traceCone(wpos, N, aperture);
    float wsum = 1.0;
    // AO IS AVERAGED ONLY OVER CONES THAT HAD VOLUME TO MARCH -- see the legacy branch above (and
    // voxi_cone.hlsli's own copy) for the full rationale; unchanged by this fix.
    float occW = insideVolume(voxelUVW(wpos + N * gVoxelParams.z)) ? 1.0 : 0.0;
    float occ = sum.a * occW;
    float occWsum = occW;
    // A COSINE-DISTRIBUTED SPIRAL, NOT ONE RING, at weight 1: the cosine weighting already lives in how
    // `d` is drawn, so weighting it again by dot(N,d) would square the distribution the gather is
    // supposed to integrate against -- see voxi_cone.hlsli's own bit-16 comment for the arithmetic that
    // exposed it. The axial cone above is stratum 0 of `cones` equal solid-angle strata; this ring
    // covers strata 1..cones-1, so t's denominator is `cones` itself, not `ring` -- k+1 skips the
    // stratum the axial cone already took.
    const uint ring = (uint)cones - 1u;
    [loop] for (uint k = 0; k < ring; ++k) {
        float t    = ((float)(k + 1u) + 0.5) / cones;
        float cosT = sqrt(saturate(1.0 - t));
        float sinT = sqrt(saturate(t));
        float ang  = 2.39996323 * (float)k;
        float3 d = normalize(N * cosT + (T * cos(ang) + B * sin(ang)) * sinT);
        float4 c = traceCone(wpos, d, aperture);
        sum += c; wsum += 1.0;
        const float cw = insideVolume(voxelUVW(wpos + d * gVoxelParams.z)) ? 1.0 : 0.0;
        occ += c.a * cw; occWsum += cw;
    }
    sum /= wsum;
    // EVERY cone left the volume: there is genuinely nothing here to occlude against, so the old
    // answer (fully open) is right, and that case must not change.
    occ = occWsum > 1e-4 ? occ / occWsum : 0.0;
    ao = saturate(1.0 - occ);
    // Bounded exactly as voxi_cone.hlsli's coneTracedIndirect is -- see that file's own note for why
    // the ceiling is the injection's own constant. TWO copies of this gather exist (voxi_cone.hlsli,
    // included into voxi.hlsl for PSMainVoxi/PSVoxel/PSVoxelDebug/PSRayDriven; this file, for the
    // cluster-material and particle passes); clamping only one of them would be a difference between
    // the raster/ray-driven and cluster/particle paths that nothing in the build would report.
    return min(sum.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
}
