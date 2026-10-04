#pragma once

// The path tracer's HLSL, compiled as the TAIL of rhi::sharedShaderPrelude(), which already
// declares PerFrame at b0 (gLightDir/gLightColor among its fields), PI, srgbToLin, skyColor,
// averSunRadiance and the averFurnace* contract. Declaration order is load-bearing: HLSL has no
// forward declarations, so a helper used before it is written compiles in C++ and fails in DXC, at
// RUNTIME, on a build that reported success. ptDirectSun below reads averSunRadiance/gLightDir from
// exactly this prelude -- the sun's own light reaches this file through the SAME shared declarations
// the sky already did, not a new binding: this module still links only Aver.RHI and Aver.Core.
namespace aver::pt {


} // namespace aver::pt
