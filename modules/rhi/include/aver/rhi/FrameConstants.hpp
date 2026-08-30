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
    f32 invViewProj[16];
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
};

// A REAL SIZE, not just an alignment. The `% 16 == 0` check both backends carried is necessary and
// nowhere near sufficient: every legal edit to this struct keeps it a multiple of 16, so the one
// assertion guarding the layout could not fail for the change most likely to break it. This number
// moving is the signal that shared_prelude.hlsl's cbuffer has to move with it.
static_assert(sizeof(PerFrameCB) == 592, "cbuffer PerFrame in shaders/shared_prelude.hlsl mirrors this");
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
static_assert(offsetof(PerFrameCB, skySh)     == 448, "the SH block is last");

// Constants for every post pass. Mirrors `cbuffer AverPost : register(b0)` in rhi::postShaderSource().
struct PostCB {
    f32 tone[4];    // exposure, bloom intensity, bloom threshold, bloom knee
    f32 dst[4];     // destination width, height, 1/width, 1/height
    f32 src[4];     // source width, height, 1/width, 1/height
    f32 adapt[4];   // min log2 luminance, 1/log2 range, adaption alpha, unused
    f32 limit[4];   // exposure min, exposure max, histogram low cut, high cut
    f32 misc[4];    // middle grey, auto-exposure on, bloom filter radius, unused
};
static_assert(sizeof(PostCB) == 96, "the HLSL cbuffer mirrors this byte for byte");

} // namespace aver::rhi
