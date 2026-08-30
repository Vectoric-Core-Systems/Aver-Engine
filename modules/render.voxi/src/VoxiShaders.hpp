#pragma once

// Voxi's HLSL, compiled as the TAIL of rhi::sharedShaderPrelude() + the material prelude, which
// already declare the cbuffer layouts, VSIn/VSOut/SkyOut, the BRDF, the Aver* contract and
// VSMain/VSky/MSMain. Declaration order is load-bearing: HLSL has no forward declarations.
namespace aver::voxi {

// THE HLSL THAT USED TO BE HERE IS NOW modules/render.voxi/shaders/voxi.hlsl.
//
// 169,883 bytes of shader source lived in this header as a raw string literal. It is a file now:
// VoxiRenderer.cpp reads it through rhi::shaderFile(), and tests/render.voxi/src/VoxiRtSeqTest.cpp
// reads the same file to assert its C++ mirror still matches the shader.
//
// This header is kept, rather than deleted, because it is the natural home for anything Voxi needs
// to say about its shaders in C++ -- and because deleting it would silently break any include that
// has not been noticed yet. If it is still empty next time you are here, delete it.
} // namespace aver::voxi
