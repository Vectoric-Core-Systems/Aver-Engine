#pragma once
#include "aver/core/Types.hpp"

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
};

// A REAL SIZE, not just an alignment. The `% 16 == 0` check both backends carried is necessary and
// nowhere near sufficient: every legal edit to this struct keeps it a multiple of 16, so the one
// assertion guarding the layout could not fail for the change most likely to break it. This number
// moving is the signal that shared_prelude.hlsl's cbuffer has to move with it.
static_assert(sizeof(PerFrameCB) == 672, "cbuffer PerFrame in shaders/shared_prelude.hlsl mirrors this");
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
};
static_assert(sizeof(PostCB) == 144, "the HLSL cbuffer mirrors this byte for byte");

} // namespace aver::rhi
