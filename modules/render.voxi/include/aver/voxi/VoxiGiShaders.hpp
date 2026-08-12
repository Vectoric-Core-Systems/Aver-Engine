// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>

// The NARROW slice of Voxi's HLSL a foreign pipeline is allowed to borrow: the cascaded-shadow
// lookup and the voxel cone trace, and nothing else of Voxi's -- no PSMainVoxi, no compute passes,
// no ray-traced history, none of the ~950 lines of VoxiShaders.hpp (which stays private to
// modules/render.voxi/src/ and is never included outside this module). Exists so the GPU
// per-cluster mesh-shader path (sandbox/src/ClusterMaterialShader.hpp) can shade with real shadows
// and real GI by MERGING Voxi's table-0 resources into its own table 0, rather than reaching for a
// third binding table rhi::kBindingTableCount does not offer -- see D3D12Device.cpp's nullFill for
// why that merge is D3D12-only.
//
// SAME SHAPE AS pbr::materialShaderPrelude()/materialShaderDefines() ON PURPOSE: a prelude with
// static storage duration, and a defines function that takes the caller's OWN register numbers
// rather than assuming table 0 base 0 -- Voxi's own pipelines use base 0 (see giLayout()), a
// merged caller does not, and nothing about this text may hard-code that difference away.
namespace aver::voxi {

// Voxi's OWN table-0 shape: t0 the GI volume, t1 the shadow map, t2 the ray-tracing acceleration
// structure, t3..t5 the flat RT geometry table, t6/t7 the ray-traced shadow/reflection history, t8
// the GI-only shadow map (9 SRVs); u0/u1 the volume/injection-accumulator UAVs, u2/u3 the ray-traced
// history write targets (4 UAVs). NAMED so the count is typed ONCE: before this constant existed,
// giLayout()'s l.srvCount and createVoxelVolume()'s BindingSetDesc::srvCount repeated "9" as two
// literals nothing checked agreed (see giLayout()'s own long-standing comment on exactly that risk).
// A caller merging Voxi's table 0 into its own -- see giShaderDefines() below -- reserves the SAME
// 9 SRVs / 4 UAVs at ITS base so the register math giShaderDefines() emits stays honest even though
// this prelude only ever DECLARES two of them (see its own comment on why the rest are reserved,
// not read).
inline constexpr u32 kGiSrvCount = 9;
inline constexpr u32 kGiUavCount = 4;
static_assert(kGiSrvCount <= rhi::kMaxBindingSlots && kGiUavCount <= rhi::kMaxBindingSlots,
              "Voxi's table-0 union must fit the per-range slot cap a BindingSetDesc enforces");

// The GI/shadow HLSL, with static storage duration: the pointer is good for the whole process.
// Compiled as rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + this + the caller's own
// source -- the same composition order VoxiRenderer's own pipelines use for kVoxiHLSL, and the
// order PbrShaders.hpp documents for its own prelude, so VSOut/gCamPos/gLightDir/averSkyIrradiance
// and the rest of the shared and material contracts already exist by the time this text is reached.
const char* giShaderPrelude();

// The semicolon-separated -D list pinning giShaderPrelude()'s registers to wherever the caller's
// root signature actually put them:
//   srvBase              -- t(srvBase) the GI volume (Texture3D), t(srvBase+1) the shadow map
//                            (Texture2D). ONLY these two: giShaderPrelude() declares no symbol for
//                            srvBase+2..srvBase+8 (Voxi's TLAS/RT-geometry/RT-history/GI-only-
//                            shadow slots) because the functions it exposes -- shadowFactor(),
//                            coneTracedIndirect() -- never read ray-traced state; they are Voxi's
//                            OWN fallback path when ray tracing is off. A root signature may
//                            over-provision what a shader actually reads (see
//                            rhi::meshGeometryDefines's own comment on the same rule), so the
//                            caller's layout still reserves the full kGiSrvCount/kGiUavCount --
//                            keeping every consumer's table-0 union the SAME shape, whether or not
//                            a given shader reads all of it -- without this prelude needing to
//                            declare registers nothing in it ever samples.
//   samplerBase           -- s(samplerBase) the volume's linear-clamp sampler, s(samplerBase+1) the
//                            shadow map's comparison sampler.
//   frameConstantRegister -- b(frameConstantRegister), the `cbuffer VoxiFrame` register. NOT
//                            assumed to be b4 (kFeatureFrameConstantRegister): a caller whose own
//                            pipeline already claims b4 for something else -- the cluster path's
//                            amplification/mesh shaders read their own per-dispatch cbuffer there --
//                            must give this a register of its own instead. Bind the SAME bytes
//                            VoxiRenderer::giFrameConstants() returns at this register every draw
//                            that uses this prelude, or the shadow cascades and volume placement it
//                            reads are garbage.
std::string giShaderDefines(u32 srvBase, u32 samplerBase, u32 frameConstantRegister);

} // namespace aver::voxi
