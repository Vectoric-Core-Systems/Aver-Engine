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

**Push-constant cbuffers other than `PerObject`.** The engine's model is that a non-zero
`constantDwords[slot]` means root constants, which this backend lowers to push constants — but
`patchPushConstants` handles exactly one block, `PerObject` at b1, by exact text. Every other such
slot (`SkinParams` at b3 today) still compiles as a descriptor and names a binding no layout
contains. Fixing it means emitting `[[vk::push_constant]]` for an arbitrary slot, which needs the
`PipelineLayout` threaded into shader compilation. **A cbuffer's kind is not a property of the shader
text**, so no source-only patch can decide it — one that guesses turns a diagnosable "wrong set" into
an equally broken "right set, wrong kind". That was tried and reverted; see `patchPerFrameSet`'s
comment in `VulkanCommon.hpp`.

**A pipeline layout referencing a destroyed `VkDescriptorSetLayout`**, reported three times during
fixed-pipeline creation. Not yet traced.

## Also worth knowing

`src/VulkanPipeline.cpp` is **on disk and deliberately not in the build**. It holds an unfinished
file split: six functions moved out of `VulkanResourceFactory.cpp` whose originals were never
deleted, and all six **differ**. The linker proved which side wins. See `CMakeLists.txt` for the
whole story before touching either file.
