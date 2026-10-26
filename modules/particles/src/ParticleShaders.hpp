// The HLSL for the particle module is FILES, deployed beside the executable (cmake aver_deploy_shaders):
//   particles.hlsl         the draw shaders: ParticleSpriteVS / ParticleRibbonVS / ParticleMeshVS and ParticlePS
//   particles_common.hlsli the RNG, noise, shape / cone sampling and the 13 modules (shared by the two compute files)
//   particles_sim.hlsl     the GPU simulation kernels (CSInit, CSBegin, CSSpawn, CSArgsUpdate, CSUpdate, CSFinish, CSProbe)
//   particles_sort.hlsl    the bitonic sort kernels
//
// This header only holds the constants the C++ side needs to agree with them on.
//
// The graphics shaders are compiled as rhi::sharedShaderPrelude() + (with a GI seam installed) Voxi's GI prelude + the file.
// Struct names are prefixed (PVSIn / PVSOut), not the generic VSIn / VSOut: sharedShaderPrelude() already declares that
// pair (modules/rhi/shaders/shared_prelude.hlsl), and redeclaring it silently shadows it in DXC instead of erroring.
//
// Table 0 of every particle draw: t0 particles, t1 order, t2 trail points, t3 texture, then the GI pair at t4 / t5. The compute
// layout is 9 UAVs, root CBV b3 (GpuSimCB), no SRV.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"

namespace aver::particles {

// Draw table 0 (graphics).
inline constexpr u32 kParticleSrvCount = 4;                          // t0 particles, t1 order, t2 trail, t3 texture
inline constexpr u32 kGiSrvBase = kParticleSrvCount;                 // Voxi's volume and shadow map follow at t4 / t5
inline constexpr u32 kGiSamplerBase = 1;                             // s0 is the particle texture sampler; the GI pair follows
inline constexpr u32 kDrawSrvCount = kGiSrvBase + 2;
static_assert(kGiSrvBase == kParticleSrvCount, "the GI pair sits right after the particle slots");
static_assert(kDrawSrvCount <= rhi::kMaxBindingSlots, "the draw table must fit the per-table slot cap");

// Compute table 0: nine UAVs (particles, dead list, alive lists, counters, args, events, parent events, sort keys, trail).
inline constexpr u32 kSimUavCount = 9;
static_assert(kSimUavCount <= rhi::kMaxBindingSlots, "the compute layout must fit the per-table slot cap");
inline constexpr u32 kSimConstantRegister = 3;                       // b3: GpuSimCB / GpuSortCB (root CBV)
static_assert(kSimConstantRegister < rhi::kMaxConstantSlots && kSimConstantRegister != rhi::kFeatureFrameConstantRegister,
              "b3 is a declarable slot and not the GI frame constants");

} // namespace aver::particles
