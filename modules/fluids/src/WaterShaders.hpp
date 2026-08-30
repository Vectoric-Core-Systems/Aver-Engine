#pragma once

// The water surface's HLSL, following PbrShaders.cpp/RHIShaders.cpp's exact convention: a raw
// string literal compiled as the TAIL of rhi::sharedShaderPrelude(), which already declares
// PerFrame at b0 (gViewProj, gLightDir/gLightColor among its fields -- see RHIShaders.cpp's own
// cbuffer PerFrame block) plus srgbToLin/toGamma and the rest of kColorHlsl -- AND, since
// sharedShaderPrelude() is one shared string, everything else RHIShaders.cpp defines before its own
// #if AVER_MS block: skyColor/averSkyIrradiance (the sky every other reflective surface in this
// engine reads) and plainFresnelSchlick/plainDistGGX/plainGeomSchlick (this engine's one GGX
// implementation). PSWater below reaches for all of them without binding anything new -- see its own
// comment at envReflection for why that is a real fix and not a coincidence. Declaration order is
// load-bearing: HLSL has no forward declarations, so a helper used before it is written compiles in
// C++ and fails in DXC, AT RUNTIME, on a build that reported success (PtShaders.hpp's own comment
// makes the identical point).
namespace aver::fluids {

inline constexpr const char* kWaterHLSL = R"(
// ---- the per-feature frame block --------------------------------------------------------------
// Feature-owned frame constants at b4 (kFeatureFrameConstantRegister -- see RHIResources.hpp's own
// reserved-register table), a root CBV like PathTracer's PtFrame (PtShaders.hpp) and every other
// feature-frame block in this engine.
//
// MIRRORS aver::fluids::WaterRenderer.cpp's WaterFrameCB struct FIELD FOR FIELD -- that struct is
// the SOURCE OF TRUTH for this layout (the same discipline PerFrameCB/PostCB use, just in reverse:
// there the C++ struct comment points at the HLSL cbuffer it mirrors; here the HLSL cbuffer points
// back at the C++ struct, because WaterRenderer.cpp is what actually fills these bytes every frame).
// A field added, removed or reordered on ONE side without the other reads a camera basis as a
// wavelength -- silently, and only in the rendered image, exactly the FrameCB comment in
// PathTracer.cpp warns about for its own cbuffer.
cbuffer WaterFrame : register(b4) {
    // waveDirSteep[i] = {dirX, dirZ, steepness, unused}. dirX/dirZ are NOT pre-normalised (matches
    // GerstnerWave.hpp's own struct comment) -- normalised below, in HLSL, exactly where
    // GerstnerWave.hpp normalises them in C++.
    float4 gWaveDirSteep[AVER_WATER_MAX_WAVES];
    // waveLenAmp[i] = {wavelengthCm, amplitudeCm, unused, unused}.
    float4 gWaveLenAmp[AVER_WATER_MAX_WAVES];
    // {originX, originY, waveCount (as a float, cast to uint below), unused}. originX/originY are
    // WaterRenderer.cpp's snapWorldToGridCm(camPos, cellSizeCm) result for engine X and engine Y --
    // see this file's own axis-naming note below. waveCount is passed explicitly, rather than always
    // looping AVER_WATER_MAX_WAVES times over possibly-inert slots, so the steepness clamp's
    // denominator here MATCHES gerstner::detail::clampedSteepness's own `count` argument exactly:
    // GerstnerWave.hpp clamps each wave's Qi to 1/count using the ACTUAL wave count a caller passed
    // to gerstnerDisplaceCm, and a shader that instead divided by the compile-time maximum (4) would
    // clamp a 1- or 2-wave set harder than the CPU-side formula this is meant to mirror, silently
    // disagreeing with anything (buoyancy, a foam mask) that calls gerstnerHeightCm directly for the
    // same water body.
    float4 gGridOriginCount;
    // {waterLevelCm, elapsedSeconds, gridScaleX, gridScaleY}. z/w used to be unused; they now carry
    // the grid's per-axis scale, multiplied into localXZ below BEFORE gGridOriginCount.xy is added
    // (see VSWater) -- an infinite surface sets both to 1.0, which is exactly the unscaled sum this
    // line always computed before bounded water existed, while a bounded surface sets them to however
    // much the grid's fixed 25600cm extent must shrink or stretch to land on that surface's own
    // bounds. See WaterRenderer.cpp's transparentPass for why scaling the grid onto the bounds, rather
    // than clipping fragments outside them, is what makes a small pool look like a pool.
    float4 gWaterState;
    // A TINT on the real reflected sky, not a flat replacement for it -- see PSWater's own comment
    // at envReflection below for why this is a real change in KIND, not a repainted constant. rgb,
    // linear; a unused. Left at WaterRenderer.hpp's own unretouched default (that header's own
    // "unvalidated placeholders" note): this pass fixes what colour MEANS here, not what it IS.
    float4 gShallowColor;
    // The water BODY's own colour: what the eye reads looking straight through the surface, in the
    // absence of any actual refraction or scene-depth sampling (README.md's "No soft shoreline"
    // note). PSWater below weights this by (1 - fresnel) rather than using it at full strength --
    // see that function's own comment for the energy-split argument. rgb, linear; a unused.
    float4 gDeepColor;
};

// ---- the vertex stage ---------------------------------------------------------------------------

struct VSWaterIn  { float2 localXZ : POSITION0; };
struct VSWaterOut {
    float4 clipPos  : SV_Position;
    float3 worldPos : TEXCOORD0;   // for the pixel shader's Fresnel/specular, and a future fog tap
    float3 worldNrm : TEXCOORD1;
};

// PORTED BY HAND from modules/water/include/aver/fluids/GerstnerWave.hpp's gerstnerDisplaceCm --
// there is NO shared-source mechanism between C++ and HLSL anywhere in this engine (every shader in
// this codebase is a raw string literal compiled at runtime; see shaders/README.md's own admission
// that the aver-shaderc tool it describes does not exist, and RHIShaders.cpp's kColorHlsl for the
// established convention this file follows). Keeping this function and GerstnerWave.hpp's C++
// agreeing is consequently a MANUAL, DOCUMENTED OBLIGATION, not something a build step enforces --
// if you change the wave formula, the clamp, or the normal derivation on one side, you must change
// it here too, by hand, and re-read GerstnerWave.hpp's own comments for WHY each term is there
// before touching either copy.
float3 averGerstnerDisplace(float2 posXZ, float t, out float3 outNormal) {
    float dispX = 0.0, dispZ = 0.0, height = 0.0;
    float dHdx = 0.0, dHdz = 0.0, qWaSin = 0.0;
    uint waveCount = uint(gGridOriginCount.z + 0.5);

    for (uint i = 0; i < AVER_WATER_MAX_WAVES; ++i) {
        if (i >= waveCount) break;

        float2 dir = gWaveDirSteep[i].xy;
        float dirLenSq = dot(dir, dir);
        dir = dirLenSq > 1e-12 ? dir * rsqrt(dirLenSq) : float2(0.0, 0.0);

        float wavelengthCm = max(gWaveLenAmp[i].x, 1e-3);   // same floor as gerstnerWaveTerms
        float amplitudeCm = gWaveLenAmp[i].y;
        float omega = 2.0 * PI / wavelengthCm;
        // Deep-water dispersion, matching kGravityCmPerS2's own comment in GerstnerWave.hpp: 981.0
        // centimetres per second squared, NOT Jolt's 9.81 m/s^2 -- this file never crosses the Jolt
        // boundary at all, so there is no metres value to be tempted to reach for here either.
        float phaseSpeed = sqrt(981.0 * omega);
        float phase = omega * dot(dir, posXZ) + phaseSpeed * t;
        float s = sin(phase), c = cos(phase);

        float qiMax = waveCount > 0 ? 1.0 / float(waveCount) : 0.0;
        float qi = clamp(gWaveDirSteep[i].z, 0.0, qiMax);

        dispX  += qi * amplitudeCm * dir.x * c;
        dispZ  += qi * amplitudeCm * dir.y * c;
        height += amplitudeCm * s;

        float wa = omega * amplitudeCm;
        dHdx   += wa * dir.x * c;
        dHdz   += wa * dir.y * c;
        qWaSin += qi * wa * s;
    }

    float3 n = float3(-dHdx, -dHdz, 1.0 - qWaSin);
    outNormal = normalize(n);
    return float3(posXZ.x + dispX, posXZ.y + dispZ, height);
}

VSWaterOut VSWater(VSWaterIn i) {
    VSWaterOut o;
    // AXIS NOTE, stated here because GerstnerWave.hpp's own naming does not: the engine is +Z-up
    // (centimetres, +X forward, +Y right, +Z up -- physics_abi.h's own top-of-file convention), so
    // this grid's plane is XY, not XZ. averGerstnerDisplace's "posXZ" parameter is read as
    // (engine X, engine Y) below, and its returned float3 is read as (engine X offset, engine Y
    // offset, VERTICAL height) -- see GerstnerWave.hpp's gerstnerDisplaceCm doc comment for the
    // identical correction made on the C++ side.
    //
    // The multiply by gWaterState.zw is what places a bounded surface's grid onto its own bounds
    // instead of the ocean's full 25600cm (see gWaterState's own comment above and WaterRenderer.cpp's
    // transparentPass for where that scale comes from); for unbounded water it is always 1.0, so this
    // line is byte-for-byte the plain localXZ-plus-origin sum it was before bounds existed. localXY is
    // still a genuine WORLD-space position either way -- scaling the grid's local coordinates before
    // adding the world-space origin is indistinguishable, per vertex, from building a coarser or
    // finer grid at that same world position -- which is why feeding it straight into
    // averGerstnerDisplace below is correct rather than merely convenient: a wave's wavelength is
    // defined in world centimetres, and it must mean the same thing in a pool as it does in the
    // open ocean.
    float2 localXY = i.localXZ * gWaterState.zw + gGridOriginCount.xy;

    float3 normal;
    float3 disp = averGerstnerDisplace(localXY, gWaterState.y /* elapsedSeconds */, normal);

    float3 worldPos = float3(disp.x, disp.y, gWaterState.x /* waterLevelCm */ + disp.z);
    o.worldPos = worldPos;
    o.worldNrm = normal;
    o.clipPos = mul(float4(worldPos, 1.0), gViewProj);
    return o;
}

// ---- the pixel stage ------------------------------------------------------------------------------

float4 PSWater(VSWaterOut i) : SV_Target {
    float3 N = normalize(i.worldNrm);
    float3 V = normalize(gCamPos.xyz - i.worldPos);

    // ---- WHICH SIDE OF THE SURFACE THE EYE IS ON ---------------------------------------------------
    //
    // This pipeline is CullMode::None precisely so the plane is still drawn from below (see
    // WaterRenderer::buildPipeline's own comment, which says "underwater must still see the plane
    // from below"), so this shader really does run with the camera under the water -- and until now
    // it had no idea. The mesh normal always points UP, so from below dot(N, V) is NEGATIVE, the
    // saturate clamped it to 0, and Schlick at NdotV = 0 returns 1.
    //
    // THE CONSEQUENCE WAS A BUG, not merely an approximation: seen from underwater the surface read
    // as a perfect mirror AT EVERY ANGLE, including straight up. Looking up from under real water you
    // see the whole sky compressed into a bright cone -- Snell's window -- and a mirror everywhere
    // outside it. The window was missing entirely because the angle it depends on was being clamped
    // away before anything could ask about it.
    //
    // gWaterState.x is the water level in world Z (see VSWater, which builds worldPos as
    // float3(disp.x, disp.y, gWaterState.x + disp.z)), so this is a straight comparison, no new
    // constant and no cbuffer change.
    bool underwater = gCamPos.z < gWaterState.x;
    // Flipped to face the eye, so NdotV is the true cosine of the incidence angle on whichever side
    // is being looked at. Above water this is N unchanged, which is what keeps every above-water
    // pixel in the engine bit-identical to before this block existed.
    float3 Nv = underwater ? -N : N;
    float NdotV = saturate(dot(Nv, V));

    // Schlick's approximation, F0 = 0.02 -- the standard value for the air/water interface (water's
    // IOR of about 1.33 gives F0 = ((1-1.33)/(1+1.33))^2 ~= 0.02), NOT a value tuned against this
    // engine's own image: at grazing angles (NdotV -> 0) the surface reads almost fully reflective,
    // and looking straight down (NdotV -> 1) it reads close to F0 -- barely reflective at all,
    // mostly the colour underneath. UNCHANGED by this pass; only what fresnel WEIGHTS, below, changed.
    float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    // ---- TOTAL INTERNAL REFLECTION, i.e. Snell's window --------------------------------------------
    //
    // Leaving water for air, Snell gives sin(t2) = 1.333 * sin(t1), and beyond sin(t1) = 1/1.333 there
    // is no solution -- no refracted ray exists and every photon reflects. That critical angle is
    // 48.6 degrees from vertical, and it is why the underside of a water surface is a mirror except
    // for a circular window straight overhead.
    //
    // SCHLICK CANNOT PRODUCE THIS AND IS NOT ASKED TO. The approximation above is a fit for light
    // ENTERING the denser medium, where no critical angle exists; it approaches 1 only as the view
    // grazes. Real TIR switches to a perfect mirror ABRUPTLY at 48.6 degrees and stays there. So the
    // test is a separate one and it OVERRIDES the fit rather than blending with it.
    //
    // Above water this branch is not entered at all, so nothing that has ever been rendered changes.
    if (underwater) {
        const float kWaterIor = 1.333;
        // sin^2(t1) from cos(t1); past sin^2(t2) = 1 there is no transmitted direction.
        if ((kWaterIor * kWaterIor) * saturate(1.0 - NdotV * NdotV) > 1.0) fresnel = 1.0;
    }

    float3 L = normalize(gLightDir.xyz);
    float3 R = reflect(-V, N);

    // ---- THE MIRROR TERM: an ACTUAL reflection, not a constant -------------------------------------
    // This is the whole "cartoonish" complaint: skyColor(dir) is the identical dome function every
    // other reflective surface in this engine already reads for its environment term (PbrShaders.cpp's
    // envSpec, VoxiShaders.hpp's ind4.specular -- both `skyColor(R)`), and it was reachable from HERE
    // for free the entire time. WaterRenderer::init compiles this shader with sd.prelude =
    // rhi::sharedShaderPrelude() (WaterRenderer.cpp) -- the SAME string PbrShaders/VoxiShaders build
    // on, cbuffer PerFrame and skyColor/gSkyZenith/gSkyHorizon/gSkySh included -- so nothing new had to
    // be bound to reach it; the prior version of this function simply never called it. (The physical
    // atmosphere's marched sky, averSkyPhysical, is deliberately NOT what's reached for here: skyColor
    // is "the dome, as every shading path names it" per that function's own comment in RHIShaders.cpp,
    // and reflections everywhere else in this engine read that same name, not the marched variant --
    // matching that convention, not inventing a third.)
    //
    // gShallowColor TINTS this reflection instead of replacing it -- the one place this pass keeps
    // gShallowColor's original role (the colour a grazing view used to read) rather than discarding
    // it outright: a grazing view still reads gShallowColor's hue, it now reads it as a cast over the
    // real sky/sun instead of as the whole answer.
    float3 envReflection = skyColor(R) * gShallowColor.rgb * fresnel;

    // ---- sun glitter: GGX, not Blinn-Phong ----------------------------------------------------------
    // pow(saturate(dot(N,H)), 128.0) drew the identical highlight shape at every slope this surface's
    // waves ever take -- a fixed exponent has no roughness behind it, so it cannot narrow or widen with
    // anything. Real sun glitter on water is exactly a GGX/Trowbridge-Reitz lobe: brighter and tighter
    // looking straight into the sun's own reflection, broader and dimmer off-axis. Built from
    // plainDistGGX/plainGeomSchlick (RHIShaders.cpp) -- part of the SAME rhi::sharedShaderPrelude()
    // this file already compiles against, per the comment above -- rather than a second, hand-rolled
    // GGX: this is the one GGX implementation the furnace test already measures, not a shader-local
    // copy that could quietly drift from it.
    //
    // kWaterRoughness is not a free artistic knob: 0.045 is plainShadeSurface's own floor
    // (`clamp(gMaterial.y, 0.045, 1.0)`, RHIShaders.cpp) -- the smoothest surface this engine's own
    // shading already supports anywhere. Calm water is exactly that surface, not a smoother one
    // invented here -- reusing this engine's existing floor keeps the highlight's width consistent
    // with every other "very smooth" material already on screen, rather than picking a second,
    // unrelated tightness for this one surface.
    const float kWaterRoughness = 0.045;
    float3 H = normalize(L + V);
    float ndl = saturate(dot(N, L));
    float ndh = saturate(dot(N, H));
    float ggxA = kWaterRoughness * kWaterRoughness;
    float ggxK = kWaterRoughness + 1.0; ggxK = ggxK * ggxK / 8.0;
    float D = plainDistGGX(ndh, ggxA);
    float G = plainGeomSchlick(NdotV, ggxK) * plainGeomSchlick(ndl, ggxK);
    // Cook-Torrance direct specular, plainShadeSurface's own `spec` line -- fresnel (computed above,
    // F0=0.02) stands in for that line's coloured F term, since water has no albedo/metallic Fresnel
    // of its own to derive one from.
    float specBRDF = (D * G * fresnel) / max(4.0 * NdotV * ndl, 1e-4);
    float3 sunGlitter = specBRDF * averSunRadiance() * ndl;

    // Both reflection terms are the SPECULAR half of this surface: real light this dielectric bounces
    // straight back at the viewer, at whatever strength Fresnel/GGX say it should, regardless of how
    // transparent the surface is authored to be -- see this pipeline's blend-state comment
    // (WaterRenderer.cpp) for why that independence is the entire point of moving to premultiplied
    // alpha below.
    float3 specular = envReflection + sunGlitter;

    // gDeepColor is the water BODY's own colour -- what the eye reads looking straight through the
    // surface, in the absence of any actual refraction or scene-depth sampling (README.md's "No soft
    // shoreline" note). Weighted by (1 - fresnel), NOT used at full strength: fresnel is the fraction
    // of light this surface already sent back as the mirror term above, so what remains to carry the
    // body colour is whatever fresnel did NOT reflect -- the same energy split a real Fresnel
    // transmission makes, just without the refraction bend this engine does not model.
    float3 diffuse = gDeepColor.rgb * (1.0 - fresnel);

    // Alpha is STILL driven by fresnel, unchanged in shape from before this pass: a grazing view is
    // (per the mirror term above) almost entirely reflection with almost nothing transmitted, so the
    // background it would otherwise show through should be almost entirely replaced -- alpha -> 1.
    // Looking straight down, the surface is mostly transmissive, so alpha sits near its floor and the
    // background survives underneath the small amount of body tint and glint this pixel adds on top.
    float alpha = saturate(0.15 + 0.85 * fresnel);

    // Deliberately NOT calling averApplyFog here. Water is itself a blended surface sitting in front
    // of whatever fog already shaded the pixels BEHIND it through the normal opaque forward pass --
    // those pixels already went through averApplyFog before this draw ever ran. Fogging the water
    // plane's own colour on top of that would double-apply the same atmosphere to the one surface
    // that is, itself, meant to read as the atmosphere's reflective boundary. Considered and
    // rejected, not an oversight -- see modules/water/README.md's own note.

    // PREMULTIPLIED, not straight -- see WaterRenderer.cpp's blend-state comment for the full argument
    // this replaces (that comment used to argue the OPPOSITE choice, for the old straight-alpha output
    // this function no longer produces). rgb is the reflection at full strength plus the transmitted
    // body colour weighted by coverage; a is that same coverage. A straight-alpha composite would
    // multiply `specular` by alpha too, attenuating the one term that must not depend on how
    // transparent this water is authored to be -- exactly the defect this whole pass exists to fix.
    return float4(specular + diffuse * alpha, alpha);
}
)";

} // namespace aver::fluids
