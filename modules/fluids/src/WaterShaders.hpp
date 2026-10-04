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

// The ocean HLSL now lives in modules/fluids/shaders/water.hlsl, loaded through
// rhi::shaderFile(). This header keeps only the WaterFrame mirror below.

} // namespace aver::fluids
