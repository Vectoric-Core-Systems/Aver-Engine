// The HLSL for ParticleRenderer's one draw shape: a camera-facing quad, already positioned in world
// space by the CPU (ParticleRenderer::transparentPass bakes each corner from the particle's centre
// plus the camera's own right/up axes -- see that file's cameraBasis() for why that is exact). The
// vertex shader therefore only has to project; no billboard math happens on the GPU at all.
//
// UNLIT BY DEFAULT. Every particle this pass draws is exactly its own authored colour,
// srgb-to-linear-corrected nowhere and shaded by nothing -- honest about what it is rather than
// pretending to a lighting model it does not have. DECIDED 4 (sampling the Voxi GI volume) adds
// EXACTLY ONE optional multiplicative term on top of that, gated at compile time by
// AVER_PARTICLES_GI: with it undefined (no GI seam installed -- see ParticleRenderer::GiSeam),
// ParticleVS/ParticlePS below compile and behave bit-for-bit as they did before this slice.
#pragma once

namespace aver::particles {

// gViewProj/gCamPos come from rhi::sharedShaderPrelude(), prepended by ParticleRenderer::init -- see
// RHIShaders.cpp's `cbuffer PerFrame : register(b0)`. STRUCT NAMES ARE PREFIXED
// (ParticleVSIn/ParticleVSOut), NOT THE GENERIC VSIn/VSOut, because rhi::sharedShaderPrelude() ITSELF
// already declares a `struct VSIn`/`struct VSOut` pair (the base mesh-draw shader's own -- see
// modules/rhi/src/RHIShaders.cpp:693-695) as part of the text this file's shader is prepended with.
// Redeclaring those two exact names silently shadowed them for DXC rather than erroring, and
// ParticleVS ended up compiled against the WRONG struct (the base pipeline's three-float MeshVertex
// position, with no posZ member at all) -- caught by createShader failing outright with "no member
// named 'posZ' in 'VSIn'", not by anything at review time. Every other prelude consumer in this
// engine (VoxOut in VoxiShaders.hpp, for one) already names its own IO structs uniquely for exactly
// this reason.

} // namespace aver::particles
