#pragma once

// The water surface's HLSL, following PbrShaders.cpp/RHIShaders.cpp's exact convention: a raw
// string literal compiled as the TAIL of rhi::sharedShaderPrelude(), which already declares
// PerFrame at b0 (gViewProj, gLightDir/gLightColor among its fields -- see RHIShaders.cpp's own
// cbuffer PerFrame block) plus srgbToLin/toGamma and the rest of kColorHlsl. Declaration order is
// load-bearing: HLSL has no forward declarations, so a helper used before it is written compiles in
// C++ and fails in DXC, AT RUNTIME, on a build that reported success (PtShaders.hpp's own comment
// makes the identical point).
namespace aver::water {

inline constexpr const char* kWaterHLSL = R"(
// ---- the per-feature frame block --------------------------------------------------------------
// Feature-owned frame constants at b4 (kFeatureFrameConstantRegister -- see RHIResources.hpp's own
// reserved-register table), a root CBV like PathTracer's PtFrame (PtShaders.hpp) and every other
// feature-frame block in this engine.
//
// MIRRORS aver::water::WaterRenderer.cpp's WaterFrameCB struct FIELD FOR FIELD -- that struct is
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
    float4 gShallowColor;   // rgb, linear; a unused
    float4 gDeepColor;      // rgb, linear; a unused
};

// ---- the vertex stage ---------------------------------------------------------------------------

struct VSWaterIn  { float2 localXZ : POSITION0; };
struct VSWaterOut {
    float4 clipPos  : SV_Position;
    float3 worldPos : TEXCOORD0;   // for the pixel shader's Fresnel/specular, and a future fog tap
    float3 worldNrm : TEXCOORD1;
};

// PORTED BY HAND from modules/water/include/aver/water/GerstnerWave.hpp's gerstnerDisplaceCm --
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
    float NdotV = saturate(dot(N, V));

    // Schlick's approximation, F0 = 0.02 -- the standard value for the air/water interface (water's
    // IOR of about 1.33 gives F0 = ((1-1.33)/(1+1.33))^2 ~= 0.02), NOT a value tuned against this
    // engine's own image: at grazing angles (NdotV -> 0) the surface reads almost fully reflective,
    // and looking straight down (NdotV -> 1) it reads close to F0 -- barely reflective at all,
    // mostly the colour underneath.
    float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    // Straight-down views read toward deepColor (transparent-reading, little reflected sky); grazing
    // views read toward shallowColor (more reflective-reading) -- shallowColor/deepColor are named
    // for the visual effect this lerp produces, not for an actual depth measurement, since this
    // slice does not sample scene depth at all (see modules/water/README.md's own "No soft
    // shoreline" note).
    float3 colorLinear = lerp(gDeepColor.rgb, gShallowColor.rgb, fresnel);

    // A single specular highlight from the already-available directional light (gLightDir/
    // gLightColor, both declared in the shared PerFrame block at b0 -- no new light data needed,
    // exactly as this file's own top-of-class WaterRenderer.hpp comment promises). Blinn-Phong, not
    // a full BRDF: water's specular lobe is what the eye actually reads as "sun glitter", and this
    // surface has no roughness/metallic authoring of its own to drive anything more elaborate.
    float3 L = normalize(gLightDir.xyz);
    float3 H = normalize(L + V);
    float specular = pow(saturate(dot(N, H)), 128.0) * fresnel;
    colorLinear += averSunRadiance() * specular;

    // Alpha ALSO comes from the Fresnel term -- near-grazing angles read more opaque/reflective,
    // straight-down views read more transparent toward deepColor -- and is a STRAIGHT (not
    // premultiplied) alpha: WaterRenderer.cpp built its pipeline with BlendMode::AlphaBlend, not
    // ParticleRenderer's PremultipliedAlpha, and this is WHY -- see that file's own comment at the
    // blend-state line for the full explanation of why copy-pasting particles' convention here would
    // be wrong, not merely different.
    float alpha = saturate(0.15 + 0.85 * fresnel);

    // Deliberately NOT calling averApplyFog here. Water is itself a blended surface sitting in front
    // of whatever fog already shaded the pixels BEHIND it through the normal opaque forward pass --
    // those pixels already went through averApplyFog before this draw ever ran. Fogging the water
    // plane's own colour on top of that would double-apply the same atmosphere to the one surface
    // that is, itself, meant to read as the atmosphere's reflective boundary. Considered and
    // rejected, not an oversight -- see modules/water/README.md's own note.
    return float4(colorLinear, alpha);
}
)";

} // namespace aver::water
