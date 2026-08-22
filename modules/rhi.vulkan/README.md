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

**Textures and samplers in the fixed scene pipeline.** Four bindings, and they are the only
validation errors remaining:

| Variable | What the layer says |
| --- | --- |
| `gBaseColorMap` (set 0, binding 9) | not declared in the pipeline layout |
| `gL1BaseColorMap` (set 0, binding 14) | not declared in the pipeline layout |
| `gMaterialSampler` (set 0, binding 2) | `VkDescriptorType` mismatch |
| `gShadowSamp` (set 0, binding 1) | `VkDescriptorType` mismatch |

The shape is the same one the cbuffers had — DXC leaves a space-less `t#`/`s#` register in set 0 —
but the fix is not, because samplers and SRVs are *colliding* there rather than merely misplaced: a
sampler at binding 1 lands where the table set already declares a texture, which is what turns
"undeclared" into "wrong type". Whatever the answer is, it is a statement about `tableSetLayout()`
and the `-fvk-s-shift`/`-fvk-t-shift` scheme, not about the shader text.

With these outstanding the process still dies, now at `0xC0000005` inside the driver rather than the
`0xC0000409` abort that every earlier defect produced.

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
