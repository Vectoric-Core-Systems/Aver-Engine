#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/ColourVision.hpp"
#include "aver/rhi/RHI.hpp"

#include <algorithm>
#include <cmath>

// The constant blocks BOTH backends upload, defined once.
//
// WHY THIS FILE EXISTS. PerFrameCB and PostCB used to be written out by hand in
// modules/rhi.d3d12/src/D3D12Device.cpp AND again in modules/rhi.vulkan/src/VulkanCommon.hpp, with
// the Vulkan copy's own comment admitting it was "copied here rather than re-derived". Between them
// and `cbuffer PerFrame` in shaders/shared_prelude.hlsl that made THREE hand-maintained spellings of
// one 23-field layout, and the only thing checking any of them was
// `static_assert(sizeof(PerFrameCB) % 16 == 0)` in each backend -- which passes for any multiple of
// 16, so adding or removing a float4 was invisible to it.
//
// The Vulkan file states the consequence exactly: "a single stray float here is a silent
// cross-backend shading divergence no compiler catches." A light direction quietly becomes an
// ambient colour, an atmosphere flag lands on a Rayleigh coefficient, and it is wrong for every
// pixel of every frame on one backend with nothing to grep for.
//
// TWO OF THE THREE ARE NOW ONE. Both backends depend on Aver.RHI, so the C++ side needs no mirror at
// all -- this is deletion rather than a guard, which is always the better answer when it is
// available. What remains is a genuine C++ <-> HLSL boundary, and it is pinned by size below rather
// than left to the alignment check that could not see a change.
//
// PLAIN POD, DELIBERATELY: no backend types, no methods, no constructors. That is what lets one
// definition serve a D3D12 root CBV and a Vulkan uniform buffer without either backend leaking into
// the other.
namespace aver::rhi {

// Per-frame constants. Mirrors `cbuffer PerFrame` in shaders/shared_prelude.hlsl, field for field
// and in this order. A field added here must be added THERE, at the same position.
struct PerFrameCB {
    f32 viewProj[16];
    // The inverse of viewProj with the VIEW'S TRANSLATION REMOVED (rotation only, eye at the
    // origin): clip space to a world-space OFFSET FROM camPos, not a world position. Every entry
    // stays O(1) wherever the camera is. The absolute inverse's third and fourth rows each carry
    // about +-eye/near (1e5 at 2 km with the 2 cm near plane), and the far-plane w is 1/far left
    // over from +-1/near; a ray unprojected through it and then differenced against the eye kept
    // a float32 residue that grows linearly with the eye's distance from the origin (estimated at
    // around a pixel by 2 km) and changes every frame the eye moves -- the whole image shaking.
    // Consumers take rays from averViewRayDir (shaders/shared_prelude.hlsl).
    f32 invViewProjRel[16];
    f32 camPos[4];
    f32 lightDir[4];
    f32 lightColor[4];
    f32 ambient[4];
    f32 skyZenith[4];
    f32 skyHorizon[4];
    f32 fogColor[4];     // a = density at fogHeight
    f32 skyParams[4];    // x atmosphere height, y sky-light intensity, z sun intensity, w cos(sun angular radius)
    f32 groundColor[4];  // rgb ground albedo below the horizon
    f32 fogParams[4];    // x height falloff, y fog height, z start distance, w max opacity
    f32 cloudParams[4];  // x coverage, y density, z layer bottom, w layer top
    f32 cloudMotion[4];  // xy wind offset in world units, z 1/feature size, w enabled
    f32 atmoRayleigh[4]; // rgb scattering per km, w scale height km
    f32 atmoMie[4];      // x scatter, y extinction, z scale height km, w phase g
    f32 atmoOzone[4];    // rgb absorption per km, w tent half-width km
    f32 atmoPlanet[4];   // x planet radius km, y atmosphere top radius km, z world->km, w on/off
    f32 atmoTune[4];     // x ozone centre km, y multi-scatter gain, z view steps, w aerial steps
    f32 atmoSunE0[4];    // rgb sun irradiance above the air, w ground albedo
    f32 fogInscatterRef[4]; // rgb averFogInscatterRef's answer, baked once per frame on the CPU
    f32 furnace[4];      // x on, y radiance -- the white-furnace energy oracle
    f32 skySh[9][4];     // nine L2 SH coefficients of the sky, rgb; w unused
    // THE CLOCK, and it lives HERE rather than anywhere more obvious for a reason worth stating.
    //
    // Animated materials need a time value, and there was none: a material graph is emitted into
    // EVERY shader that shades a surface, so it may only read constants that exist identically in
    // all of them. b2 is per-DRAW authored material data (and MaterialConstants has no slack left);
    // b4 is per-FEATURE and each feature declares a different struct there -- Voxi, Water and
    // ActorPreview disagree about what b4 even is. b0 is the one block the backend binds on every
    // pipeline bind, for every shader, which makes it the only correct home.
    //
    // x IS WRAPPED, and that is not a detail: sin(t) loses its meaning once t is large enough that
    // consecutive float32 values skip past a period. Wrapping to an hour keeps ~0.2 ms of
    // resolution forever, and any ripple whose period divides the wrap is seamless across it.
    // y is the raw unwrapped seconds for anything that genuinely wants monotonic time and can
    // accept the precision loss; z is the frame delta.
    f32 time[4];         // x seconds wrapped to 3600, y seconds raw, z delta seconds, w unused
    // THE WATER WAVE SET, and it is here for the same reason the clock is: it has TWO consumers that
    // must agree exactly, and they live in different shaders.
    //
    // A material graph shapes the water surface from these; the caustics term projects light through
    // that same surface onto whatever lies under it. Those are the bumps and the bright lines they
    // cast, so if the two ever disagree the lines drift away from the bumps making them -- which is
    // precisely what happened when each carried its own copy of the numbers. One array, read by both.
    //
    // Alongside fog, clouds and the atmosphere rather than in a renderer's own block because it is
    // the same KIND of thing: authored environment that any shader may need.
    //
    // Each wave: xy = unit direction in world XY, z = k (radians per cm, 2*pi/wavelength),
    // w = angular speed (radians per second). waveParams: x = amplitude, y = how many of the three
    // are live, z and w spare.
    f32 wave[3][4];
    f32 waveParams[4];
    // TEMPORAL AA. viewProj above carries the frame's sub-pixel jitter while a temporal upscaler is
    // set (the device applies it at upload; on the CPU, camera() stays unjittered). This is the same
    // camera WITHOUT it, for what draws after the resolve (editor lines, viewport icons), which must
    // not shimmer. Equal to viewProj when no jitter is applied.
    f32 viewProjNoJitter[16];
    f32 jitter[4];       // xy this frame's jitter in scene pixels (+x right, +y down); zw unused
    // THE FLOATING ORIGIN (LARGE_WORLD 2.2.6), appended so no earlier offset moves. Every position above and in
    // every draw is relative to the scene origin O; these two rows let a shader recover what depended on the
    // ABSOLUTE position. Both are zero at O = 0, which is what keeps a level that never rebases bit-identical.
    f32 worldOrigin[4];  // xyz = O in cm (a multiple of 1,638,400, exact in f32), w = the shift generation as a float
    f32 wavePhase[4];    // xyz = per-wave phase offset k * dot(dir, O.xy) mod 2*pi, added to the wave phase; w unused
};

// A REAL SIZE, not just an alignment. The `% 16 == 0` check both backends carried is necessary and
// nowhere near sufficient: every legal edit to this struct keeps it a multiple of 16, so the one
// assertion guarding the layout could not fail for the change most likely to break it. This number
// moving is the signal that shared_prelude.hlsl's cbuffer has to move with it.
static_assert(sizeof(PerFrameCB) == 784, "cbuffer PerFrame in shaders/shared_prelude.hlsl mirrors this");
static_assert(sizeof(PerFrameCB) % 16 == 0, "a constant buffer's rows are float4s");

// Spot checks at the boundaries a reader would look for, in the shape MaterialTest.cpp proved out:
// a size alone cannot catch two same-typed fields being swapped, and every field here is f32[4].
//
// These were worth writing for a reason that showed up immediately: two of the four were wrong on
// the first attempt (fogColor and atmoMie, off by one and two rows), and the compiler said so
// rather than the picture going subtly wrong months later. An assertion that catches its author
// on the day it is written is doing exactly the job it was added for.
static_assert(offsetof(PerFrameCB, camPos)    == 128, "camPos follows the two matrices");
static_assert(offsetof(PerFrameCB, fogColor)  == 224, "fogColor at 224");
static_assert(offsetof(PerFrameCB, atmoMie)   == 336, "atmoMie at 336");
static_assert(offsetof(PerFrameCB, skySh)     == 448, "the SH block sits at 448");
static_assert(offsetof(PerFrameCB, time)      == 592, "time follows the SH block");
static_assert(offsetof(PerFrameCB, wave)      == 608, "the wave set follows time");
static_assert(offsetof(PerFrameCB, viewProjNoJitter) == 672, "the unjittered camera follows the waves");
static_assert(offsetof(PerFrameCB, worldOrigin) == 752, "the floating origin follows the jitter row");
static_assert(offsetof(PerFrameCB, wavePhase)   == 768, "the wave phase offsets follow the origin");

// Constants for every post pass. Mirrors `cbuffer AverPost : register(b0)` in rhi::postShaderSource().
struct PostCB {
    f32 tone[4];    // exposure (compensation under auto-exposure), bloom intensity, threshold, knee
    f32 dst[4];     // destination width, height, 1/width, 1/height
    f32 src[4];     // source width, height, 1/width, 1/height
    f32 adapt[4];   // min log2 luminance, 1/log2 range, adaption alpha toward a BRIGHTER view, unused
    f32 limit[4];   // exposure min, exposure max, histogram low cut, high cut
    f32 misc[4];    // middle grey, auto-exposure on, bloom filter radius, TONEMAP MODE
    // x = the ceiling scene radiance is clamped to just before the tonemap (0 disables it entirely).
    // y = PostSettings::localExposureShadows, z = localExposureHighlights (both [0,1], local
    // exposure on when either > 0 -- see PostSettings' own comment). w = the adaption alpha toward a
    // DARKER view (PostSettings::exposureSpeedDark; adapt.z is the other direction's) -- the row's
    // last spare component, taken for the same reason y/z were.
    //
    // A NEW ROW RATHER THAN misc.w OR adapt.w. adapt.w looked free (this struct called it "unused")
    // but post.hlsl documents the same component as "pixels sampled", and a slot whose two sides
    // disagree about its meaning is exactly the kind of thing that gets read by one of them later.
    // misc.w takes the tonemap mode because both sides already agreed it was unused. y/z of THIS
    // row are where localExposureShadows/Highlights landed for the same reason -- an already-spare
    // slot on the same cbuffer row, rather than growing the struct for two more scalars.
    f32 clampRadiance[4];
    // THE DOCKED-VIEWPORT SUB-RECT, in the post chain's own normalised source UV space -- (0,0,1,1)
    // identity (the whole source texture) whenever no sub-rect is set, which is what keeps every
    // pass that does not opt in byte-for-byte unchanged from before this field existed. xy is the
    // sub-rect's uv origin, zw its uv size. Both backends derive this from vpX_/vpY_/vpW_/vpH_
    // (already SCENE-space -- see D3D12Device::setViewportRect's own comment) divided by
    // sceneWidth_/sceneHeight_, the same space the post chain's own source texture is sampled in.
    // Mirrors `gPostRegion` in shaders/post.hlsl, appended here for the same reason clampRadiance's
    // own fields were: an already-open cbuffer row is cheaper than growing the struct twice.
    f32 region[4];
    // EYE ADAPTATION REALISM (Krawczyk, Myszkowski & Seidel 2005, "Perceptual effects in real-time
    // tone mapping" -- see PostSettings::adaptationRealism for the model). x PostSettings::
    // adaptationRealism [0,1]. y kLuminanceToCdm2 (declared in RHI.hpp beside PostSettings): the
    // scene-linear-radiance-unit -> cd/m^2 constant the model needs a real luminance for, derived
    // from LevelSky.hpp's sunIntensity calibration (sunIntensity = lux / kLuminanceToCdm2, so one
    // engine radiance unit is kLuminanceToCdm2 cd/m^2 -- see that constant's own comment). z
    // PostSettings::nightVision [0,1]. w PostSettings::meteringCenterWeight [0,1]. A new row at the
    // END rather than a spare component: clampRadiance and region above already used up every slot
    // either side called "unused", and mirrors `gPostEye` in shaders/post.hlsl -- a field added here
    // must be added there too, at the same position.
    f32 eye[4];
    // LENS EFFECTS AND GRADING (PostSettings' lens block), eight rows appended for the same reason
    // `eye` was; `gPostLens`..`gPostDof` in shaders/post.hlsl mirror them. fillPostLens() below fills
    // them for both backends, so the packing lives once.
    f32 lens[4];    // x vignette, y grain, z chromatic aberration, w lens flare
    f32 lens2[4];   // x motion-blur shutter (0 = off), y DOF on, z grain frame seed, w unused
    f32 grade[4];   // x saturation, y contrast, z stage bitmask (1 WB, 2 CDL, 4 contrast, 8 saturation), w LUT size (0 = none)
    f32 wb[4];      // rgb white-balance multiplier, w LUT intensity
    f32 lift[4];    // rgb lift
    f32 gamma[4];   // rgb 1/gamma
    f32 gain[4];    // rgb gain
    f32 dof[4];     // x focus (cm), y CoC scale (px at the composite source's width), z max radius (half-res px), w unused
    // ACCESSIBILITY: x the packed colour-vision filter (ColourVision.hpp packColourVision; 0 = off, the frame is then
    // bit-identical), yzw unused. Mirrors `gPostAccess` in shaders/post.hlsl.
    f32 access[4];
};
static_assert(sizeof(PostCB) == 288, "the HLSL cbuffer mirrors this byte for byte");

// The linear-sRGB multiplier that makes a source of colour temperature `kelvin` read as the 6500 K
// reference, with `tint` (+ = magenta, - = green) on the green channel. Planckian xy from Kim et al.
// (2002), Rec.709 primaries, luminance-preserving, clamped to 2500..15000 K; exactly unit at 6500 K / tint 0.
inline void whiteBalanceGain(f32 kelvin, f32 tint, f32 out[3]) {
    auto rgbOf = [](f32 t, f32 c[3]) {
        t = std::clamp(t, 2500.0f, 15000.0f);
        const f32 t2 = t * t, t3 = t2 * t;
        const f32 x = t <= 4000.0f ? -0.2661239e9f / t3 - 0.2343589e6f / t2 + 0.8776956e3f / t + 0.179910f
                                   : -3.0258469e9f / t3 + 2.1070379e6f / t2 + 0.2226347e3f / t + 0.240390f;
        const f32 y = t <= 2222.0f ? -1.1063814f * x * x * x - 1.34811020f * x * x + 2.18555832f * x - 0.20219683f
                    : t <= 4000.0f ? -0.9549476f * x * x * x - 1.37418593f * x * x + 2.09137015f * x - 0.16748867f
                                   :  3.0817580f * x * x * x - 5.87338670f * x * x + 3.75112997f * x - 0.37001483f;
        const f32 X = x / y, Z = (1.0f - x - y) / y;   // Y = 1
        c[0] = std::max( 3.2404542f * X - 1.5371385f - 0.4985314f * Z, 1e-3f);
        c[1] = std::max(-0.9692660f * X + 1.8760108f + 0.0415560f * Z, 1e-3f);
        c[2] = std::max( 0.0556434f * X - 0.2040259f + 1.0572252f * Z, 1e-3f);
    };
    f32 src[3], ref[3];
    rgbOf(kelvin, src);
    rgbOf(6500.0f, ref);
    for (int i = 0; i < 3; ++i) out[i] = ref[i] / src[i];
    if (kelvin != 6500.0f) {
        // Keep a neutral input's luminance: white balance recolours, it does not change exposure.
        const f32 y = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
        for (int i = 0; i < 3; ++i) out[i] /= y;
    }
    out[1] *= 1.0f - 0.3f * std::clamp(tint, -1.0f, 1.0f);
}

// Fills PostCB's lens rows from PostSettings. `srcW/srcH`: the composite's source extent (the DOF
// circle of confusion is in pixels of it). `depthFx`: this backend has this frame's G-buffer view Z
// and velocity readable; without it DOF and motion blur are off. `lutSize`: the grading LUT's edge,
// 0 = none usable. `frameSeed` varies the grain per frame. Every default leaves each row at its
// identity so the shader branches skip the effect.
inline void fillPostLens(PostCB& cb, const PostSettings& p, u32 srcW, u32 srcH, bool depthFx, u32 lutSize, u32 frameSeed) {
    auto sat1 = [](f32 v) { return std::clamp(v, 0.0f, 1.0f); };
    cb.lens[0] = sat1(p.vignette);
    cb.lens[1] = sat1(p.grain);
    cb.lens[2] = sat1(p.chromaticAberration);
    cb.lens[3] = sat1(p.lensFlare);

    const bool mb = depthFx && p.motionBlur > 0.0f;
    const bool dof = depthFx && p.dofEnabled && srcW > 0 && srcH > 0;
    cb.lens2[0] = mb ? sat1(p.motionBlur) : 0.0f;
    cb.lens2[1] = dof ? 1.0f : 0.0f;
    cb.lens2[2] = static_cast<f32>(frameSeed & 63u);
    cb.lens2[3] = 0.0f;

    u32 mask = 0;
    f32 gain[3], lift[3], invGamma[3], wb[3] = {1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 3; ++i) {
        lift[i] = p.lift[i];
        gain[i] = p.gain[i];
        invGamma[i] = 1.0f / std::max(p.gamma[i], 1e-3f);
    }
    if (p.whiteBalanceK != 6500.0f || p.whiteBalanceTint != 0.0f) {
        whiteBalanceGain(p.whiteBalanceK, p.whiteBalanceTint, wb);
        mask |= 1u;
    }
    for (int i = 0; i < 3; ++i)
        if (lift[i] != 0.0f || gain[i] != 1.0f || p.gamma[i] != 1.0f) { mask |= 2u; break; }
    if (p.contrast != 1.0f) mask |= 4u;
    if (p.saturation != 1.0f) mask |= 8u;
    cb.grade[0] = std::max(p.saturation, 0.0f);
    cb.grade[1] = std::max(p.contrast, 0.0f);
    cb.grade[2] = static_cast<f32>(mask);
    cb.grade[3] = lutSize ? static_cast<f32>(lutSize) : 0.0f;
    for (int i = 0; i < 3; ++i) {
        cb.wb[i] = wb[i]; cb.lift[i] = lift[i]; cb.gamma[i] = invGamma[i]; cb.gain[i] = gain[i];
    }
    cb.wb[3] = lutSize ? sat1(p.gradingLutIntensity) : 0.0f;
    cb.lift[3] = cb.gamma[3] = cb.gain[3] = 0.0f;

    // Physical CoC: c = f^2 / (N (S - f)) * (d - S) / d on a 3.6 cm sensor, + far / - near, in pixels
    // of the composite source (f and the distances in cm, the engine's world unit).
    cb.dof[0] = std::max(p.dofFocusCm, 1.0f);
    const f32 f = std::max(p.dofFocalMm, 1.0f) * 0.1f;
    const f32 s = std::max(cb.dof[0], f * 1.5f);
    cb.dof[1] = dof ? f * f / (std::max(p.dofFStop, 0.5f) * (s - f)) / 3.6f * static_cast<f32>(srcW) : 0.0f;
    cb.dof[2] = std::clamp(static_cast<f32>(srcH) * 0.01f, 4.0f, 12.0f);
    cb.dof[3] = 0.0f;

    cb.access[0] = packColourVision(p);
    cb.access[1] = cb.access[2] = cb.access[3] = 0.0f;
}

} // namespace aver::rhi
