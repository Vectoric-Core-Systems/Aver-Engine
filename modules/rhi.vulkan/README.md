# Aver.RHI.Vulkan  (`modules/rhi.vulkan`)

- **Language:** C++
- **Depends on:** Aver.RHI (only)
- **Option:** `AVER_RHI_VULKAN` — **OFF by default**, and staying off until it presents a frame.

Vulkan 1.3 behind the same `rhi::IDevice` the D3D12 backend implements. **It presents a frame** --
grid, cube, shadow, sky and world axes -- which it did not until 7504a80. It is not yet correct:
the image fills only part of the swapchain and the editor UI does not draw at all. Both are
symptoms of the one defect named below.

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

**One defect drops the entire overlay, and the editor UI with it.**

    VkDescriptorSet ... was destroyed or updated without UPDATE_AFTER_BIND

This backend rewrites a binding set that a *recording* command buffer has already bound. Vulkan
forbids that, and the command buffer goes to an INVALID state -- so every call recorded afterwards
is dropped by the driver, which in one run was 133 further errors and every UI draw in the frame.
It is the only thing that invalidates the command buffer now.

Two ways out, and it is a decision rather than a patch:

- declare the pool and the set layouts `UPDATE_AFTER_BIND` (`VK_EXT_descriptor_indexing`, core since
  1.2 and this backend already requires 1.3), or
- ring the binding sets `kFrameCount` deep, the way the constant ring already is.

**Also outstanding**

- The presented image fills only part of the swapchain. The scene extent and the present extent
  disagree; with no UI to lay out a viewport rect, `setViewportRect` is left holding whatever the
  editor last computed.
- The two shaders on the bind-map fallback -- `gInstanceWorlds` at t17, and `MSVoxel`'s
  `gVerts`/`gIndices`/`MeshCB` -- because `GraphicsPipelineDesc::instanced` and feature-module mesh
  geometry are both unimplemented here. See the fallback note below.
- 13 leaked objects at `vkDestroyDevice`.

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
