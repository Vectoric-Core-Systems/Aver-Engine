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
| `Binding 2 ... is SAMPLED_IMAGE but ... trying to bind is ACCELERATION_STRUCTURE_KHR` | slot **kinds** disagree — see below |
| `vkCmdBeginRendering(): invalid inside an active render pass` | render passes are not being closed |
| `vkCmdDraw/EndRendering(): must be issued inside an active render pass` | the same, from the other side |
| `pColorAttachments[0].resolveMode` | MSAA resolve is not set up |
| `pImageMemoryBarriers[0].image Invalid` | a barrier on a handle that is not live |

**The slot-kind disagreement is the interesting one**, and it is structural rather than a slip. A
pipeline's table set layout gets its `SlotKind`s from **reflecting the shader**, while the
`BindingSet` bound into that same set gets them from **what the feature module declared**. When a
shader does not USE a slot, DXC eliminates it, reflection finds nothing and defaults the slot to
`Texture2D` — so a pipeline whose shaders never touch the TLAS declares `SAMPLED_IMAGE` at slot 2
while Voxi's binding set holds an acceleration structure there. Closing it means picking one source
of truth: either `BindingSetDesc` carries the kinds and the pipeline layout uses those, or
reflection is unioned across every pipeline that shares a binding set. That is a design decision,
not a patch, and `VulkanCommon.hpp`'s own note on `tableSetLayout` already flags the two halves as
"nothing at compile time tying the two together".

**Two features are unimplemented and now say so.** `GraphicsPipelineDesc::instanced` is unconsumed
here, so the instance SRV at `t(declaredSrvCount)` has nowhere to go; feature-module mesh geometry
is in the same position. Shaders in that position take the fallback described below.

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
