# Aver.RHI.Vulkan  (`modules/rhi.vulkan`)

- **Language:** C++
- **Depends on:** Aver.RHI (only)
- **Option:** `AVER_RHI_VULKAN` — **OFF by default**, and staying off until it presents a frame.

Vulkan 1.3 behind the same `rhi::IDevice` the D3D12 backend implements. **Not a stub.** It creates a
real device and swapchain, compiles the engine's shared HLSL to SPIR-V, brings the whole engine up —
and does not yet draw. What remains is named below rather than left to be rediscovered.

## No SDK required to build or run

Headers are vendored (`third_party/vulkan-headers`, Apache-2.0). The **loader** is
`C:\Windows\System32\vulkan-1.dll`, which ships with the GPU driver, and every entry point is
resolved at runtime through `LoadLibraryW` + `GetProcAddress` — there is no `vulkan-1.lib` and no
link-time dependency on anything LunarG ships.

The **SPIR-V compiler** is the catch, and it cost this backend its whole life once: the Windows SDK's
`dxcompiler.dll` accepts `-spirv` and then refuses at code generation, because Microsoft build it
without the SPIR-V backend. `third_party/dxc-spirv` vendors one that can, and
`modules/rhi.d3d12/CMakeLists.txt` lists it **first** in its `find_file` HINTS so one DLL serves both
backends. That `find_file` writes a **cache** variable — if `bin/dxcompiler.dll` is ~14 MB rather
than ~28 MB, reconfigure with `-UAVER_DXCOMPILER`.

## Debugging it

```bash
Sandbox.exe --backend vulkan --debug-layer
```

`--debug-layer` enables `VK_LAYER_KHRONOS_validation` **when it is installed** (LunarG SDK) and warns
when it is not. Install the SDK before touching anything here: **AMD's discrete driver does not
report a shader/layout mismatch — LLPC calls `abort()`**, so the process dies at `0xC0000409` inside
`amdvlk64.dll` with nothing printed. Every bug found here so far was invisible without the layer, and
each one the layer named in a single run.

`AVER_VK_DUMP_SPIRV=<dir>` writes every compiled module as `<entry>.spv`. SPIR-V carries
`OpDecorate DescriptorSet`/`Binding` on each resource, so this answers "where did the compiler put
it" with no SDK at all — which is how the post chain's bindings were verified before one was
available.

## What is left

Every shader now compiles and every pipeline is created; what fails is **drawing**. The remaining
validation errors, all of them at command-record time and none about where a resource is bound:

| What the layer says | Shape of it |
| --- | --- |
| `pImageMemoryBarriers[0].image Invalid` | a barrier on a handle that is not live |

Render-pass scoping and the MSAA resolve were the other two and are **fixed**: one
`renderScopeDepth_` counter in `VulkanDevice` now owns every `vkCmdBeginRendering`, so a feature
drawing through the render context from inside one of the device's own passes joins that scope
instead of nesting a second one inside it.

**The slot-kind disagreement is FIXED**, and the shape of the fix is the same one the cbuffers and
the register map got: stop inferring what the caller can declare. A pipeline's table set layout used
to take its `SlotKind`s from REFLECTING the shader, which cannot see a slot the shader does not use
-- DXC eliminates it, nothing is found, and the slot silently becomes a `Texture2D`. Voxi's t2 TLAS
is exactly that slot, declared for the table but used by only some of the pipelines sharing it, so
the pipeline declared `SAMPLED_IMAGE` where the binding set held an `ACCELERATION_STRUCTURE` and the
layer only said so at the draw.

`PipelineLayout` now carries the kinds `BindingSetDesc` always carried, behind
`slotKindsDeclared`. Both sides go through the one `tableSetLayout()` cache with the same kinds, so
they land on the same `VkDescriptorSetLayout` and are compatible by construction. Voxi declares
them from a single `giTableKinds()` read by both its layout and its set -- the treatment
`kGiSrvCount` already got, for the same reason. A layout that declares nothing still reflects, and
now **warns naming each slot it had to guess** rather than only when it finds nothing at all.

## Textures and samplers: done, and how

`buildRegisterBinds` (`VulkanRegisterMap.hpp`) derives one `-fvk-bind-register` per register from
the `PipelineLayout`, and `moduleForLayout` passes it to DXC. Four validation errors closed:

| Register | Was | Is |
| --- | --- | --- |
| `t9` `gBaseColorMap` | set 0, binding 9 — undeclared | set 1, binding 0 |
| `t14` `gL1BaseColorMap` | set 0, binding 14 — undeclared | set 1, binding 5 |
| `s1` `gShadowSamp` | set 0, binding 1 — a texture is there | set 3, binding 1 |
| `s2` `gMaterialSampler` | set 0, binding 2 — likewise | set 3, binding 2 |

Table 1 is the crux: HLSL numbers it **continuously above table 0** (`t(srvCount)..`) because that
is what D3D12's root signature wants, and only this backend splits the two into separate sets that
each restart at binding 0. No amount of `-fvk-*-shift` can express that — a shift moves binding
numbers within one space and cannot move anything into another set.

Three things fell out of it that are worth knowing:

- **The map must be COMPLETE.** Supply one `-fvk-bind-register` and DXC demands one for every
  resource: `error: missing -fvk-bind-register for resource`. Descriptor cbuffers therefore go in
  the map too, and `patchCbuffersForLayout`'s `[[vk::binding]]` half stands down when it is in use —
  two answers to the same question is one more than DXC accepts.
- **A layout does not always describe its whole shader** (the two unimplemented features above, plus
  resources a shared header declares and a given shader does not use). Rather than invent a binding
  and turn a compile error into a validation error, `moduleForLayout` retries WITHOUT the map,
  restoring exactly the placement that shader had before — no regression, and a warning naming it.
- **`descriptor set 3 is never bound` is what you get next.** The immutable sampler set is written
  once at layout-build time and it is tempting to conclude it never needs binding. A pipeline whose
  shader names a sampler *statically uses* set 3, and Vulkan requires every such set to be bound
  before the draw. `VulkanRenderContext` binds it once per `setPipeline`.

`tests/rhi/RegisterBindMapTest.cpp` pins the mapping with no device and no SDK, against the exact
layout that was failing (Voxi's GI: `srvCount` 9, `srvCount1` 8, `uavCount` 4, 3 samplers).

## Constant buffers: done, and how

`patchCbuffersForLayout` (`VulkanResourceFactory.cpp`) lowers **every** cbuffer to whatever its
`PipelineLayout` says it is, and this is the whole of that class:

- `constantDwords[N] != 0` → root constants, folded into one `[[vk::push_constant]]` struct.
- `constantDwords[N] == 0` → a descriptor, at binding `N` in `kVkSetConstants`.

It replaced a per-block exact-text needle that handled only `PerObject` at b1. **A cbuffer's kind is
not a property of the shader text** — `cbuffer SkinParams : register(b3)` reads identically either
way — so the `PipelineLayout` is threaded down to compilation, which is why `RhiShader` keeps its
source and `moduleForLayout()` re-patches at *pipeline* creation rather than at `createShader`.

The part worth not breaking is the **padding**. `pushConstantLayout()` places the object block at
bytes 0–128 whether or not a shader declares it, so a shader declaring only b3 needs its fields to
begin at byte 128 — where the engine actually pushes them. Get it wrong and nothing crashes; every
vertex is simply skinned by the top row of `gWorld`. `tests/rhi/CbufferLayoutPatchTest.cpp` pins
this with no device and no Vulkan headers, and was checked by deliberately removing the padding
(exactly one of its 33 checks fails).

## Also worth knowing

`src/VulkanPipeline.cpp` is **on disk and deliberately not in the build**. It holds an unfinished
file split: six functions moved out of `VulkanResourceFactory.cpp` whose originals were never
deleted, and all six **differ**. The linker proved which side wins. See `CMakeLists.txt` for the
whole story before touching either file.
