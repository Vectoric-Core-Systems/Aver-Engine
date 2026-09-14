// Aver.RHI.Vulkan -- the HLSL-register -> (descriptor set, binding) map.
//
// SEPARATE FROM VulkanCommon.hpp ON PURPOSE, and it is the only header in this module a file
// outside modules/rhi.vulkan/src may include. Nothing here needs vulkan.h: a VkRegisterBind is
// four integers and a character describing what the SHADER COMPILER should be told, and
// buildRegisterBinds derives it from a PipelineLayout, which is public Aver.RHI. House rule 3 says
// no Vulkan type may escape this module, and this header keeps that true while still letting
// tests/rhi/src/RegisterBindMapTest.cpp check the mapping with no device and no SDK.
//
// The Vk prefix is this engine's, not Khronos's -- the struct describes a DXC command-line
// argument, not a Vulkan object.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

namespace aver::rhi::vkb {

// One explicit HLSL-register -> (set, binding) mapping, emitted as DXC's -fvk-bind-register.
//
// WHY THIS EXISTS, and why the -fvk-*-shift arguments beside it could not do the job: a shift moves
// binding NUMBERS within one space and cannot move anything into a different SET, nor permute two
// registers of the same class independently. The post chain needs both -- its six resources live in
// set kVkSetConstants in an order (b0, t0, t1, t2, u0, u1) that interleaves register classes -- so
// it needs a per-register map, which is exactly what this is. This is option (b) from the decision
// recorded at the top of VulkanShaderCompiler.cpp, chosen there precisely because it keeps the
// shared HLSL backend-neutral: no `space` annotation is added, and D3D12 reads the same source
// unchanged.
struct VkRegisterBind {
    char type;      // 'b', 't', 'u' or 's' -- the HLSL register class
    u32  number;    // the register index within that class
    u32  space;     // the HLSL register space, 0 for everything this engine declares
    u32  set;       // the Vulkan descriptor set it must land in
    u32  binding;   // and the binding within that set
};

// The most registers one PipelineLayout can declare: kMaxBindingSlots SRV + kMaxBindingSlots UAV
// per table (24 + 24 as of optimisation-wave-2, up from 16), 4 samplers, and every constant slot
// (the map has to be COMPLETE -- see buildRegisterBinds). Sized so
// buildRegisterBinds is never asked to truncate, which it would do silently.
constexpr u32 kMaxRegisterBinds = 4 * kMaxBindingSlots + 4 + kMaxConstantSlots + 1;   // +1: the instanced pipeline gInstanceWorlds register

// Fills `out` with one VkRegisterBind per SRV / UAV / SAMPLER register a PipelineLayout declares,
// mapping each to the set and binding section 4's scheme puts it in. Returns how many were written.
//
// THE OTHER HALF OF THE SAME PROBLEM patchCbuffersForLayout solves, and the reason both need the
// layout. DXC maps HLSL register SPACE onto the SPIR-V descriptor SET, and every shader in this
// engine declares everything in the default space0 -- deliberately, because a space is meaningless
// to the D3D12 root signature reading the same text. So without a map, EVERY t/u/s register lands
// in set 0 at binding == register number, and two things go wrong at once:
//
//   - TABLE 1's SRVs stay in set 0. Table 1 is based immediately above table 0 in HLSL's flat
//     register numbering (t(srvCount)..), but this backend gives it its OWN set, restarted at
//     binding 0. PBR's material textures are based at t9 with srvCount 9, so gBaseColorMap belongs
//     at set 1 binding 0 and was landing at set 0 binding 9, which table 0 does not declare.
//   - SAMPLERS collide with textures. s1/s2 land at set 0 bindings 1/2, where table 0 has already
//     declared SAMPLED_IMAGEs -- hence "VkDescriptorType mismatch" rather than "not declared".
//
// UAVs were already right, but only by luck of a shift: -fvk-u-shift kVkUavBindingBase 0 moves them
// clear of the SRVs within set 0. That shift is MUTUALLY EXCLUSIVE with -fvk-bind-register, so a
// caller supplying this map must map the UAVs too -- which is why they are included here rather
// than left to the shift.
//
// Note what is NOT here: constant buffers. They are placed by patchCbuffersForLayout instead,
// because a push-constant block is not a descriptor at all and has no set or binding to name.
// `instanced` is GraphicsPipelineDesc::instanced, which is a SIBLING of PipelineLayout rather than a
// field of it, so it has to be passed separately. When set, one extra mapping is emitted for
// gInstanceWorlds at register t(declaredSrvCount(layout)) -- the register the shared HLSL computes
// for it (VoxiRenderer.cpp derives AVER_INSTANCE_SRV from exactly that expression) -- landing in set
// kVkSetInstances at binding 0. Omitting it when the shader declares it is not a silent
// degradation: DXC rejects an INCOMPLETE map outright with "missing -fvk-bind-register for
// resource", the compile falls back to the layout-agnostic module, and the pipeline is then built
// from a shader whose gInstanceWorlds nothing has bound.
u32 buildRegisterBinds(const PipelineLayout& layout, VkRegisterBind* out, u32 maxOut, bool instanced = false);

}  // namespace aver::rhi::vkb
