# Aver.RHI.Vulkan  (`modules/rhi.vulkan`)

- **Language:** C++
- **Depends on:** Aver.RHI (only)
- **Option:** `AVER_RHI_VULKAN` — **ON by default**

Vulkan 1.3 behind the same `rhi::IDevice` the D3D12 backend implements. It presents a frame (since `7504a80`) with grid, cube, shadow, sky and world axes; the editor UI draws correctly on it (`modules/rhi.vulkan.imgui`), and the swapchain is filled — that symptom was the same gap as the missing UI: with no UI laying out a dockspace, `setViewportRect` kept the editor's default 1600x900. Ten validation errors remain (down from 156), all from the same known gap (detailed below); `vkDestroyDevice` reports no leaked objects.

## No SDK required to build or run

Headers are vendored (`third_party/vulkan-headers`, Apache-2.0). The **loader** is `C:\Windows\System32\vulkan-1.dll`, which ships with the GPU driver; every entry point is resolved at runtime via `LoadLibraryW` + `GetProcAddress`, with no link-time dependency on `vulkan-1.lib` or any LunarG SDK.

The **SPIR-V compiler** is the catch: the Windows SDK's `dxcompiler.dll` accepts `-spirv` but refuses at code generation because Microsoft built it without the SPIR-V backend. `third_party/dxc-spirv` vendors one that can, and `modules/rhi.d3d12/CMakeLists.txt` lists it **first** in its `find_file` HINTS so one DLL serves both backends. That `find_file` writes a **cache** variable — if `bin/dxcompiler.dll` is ~14 MB rather than ~28 MB, reconfigure with `-UAVER_DXCOMPILER`.

## Debugging it

```bash
Sandbox.exe --backend vulkan --debug-layer
```

`--debug-layer` enables `VK_LAYER_KHRONOS_validation` when installed (LunarG SDK) and warns when not. **Install the SDK first: AMD's discrete driver does not report shader/layout mismatches — LLPC calls `abort()`**, so the process dies at `0xC0000409` inside `amdvlk64.dll` with nothing printed. Every bug found here has been invisible without the validation layer, which names problems in a single run.

**Validation messages name their objects**: `VkImage 0x27b000000027b[scene resolve]` tells which resource is meant, not just a raw handle. This is `VK_EXT_debug_utils`. Debug labels (`pushMarker`/`popMarker`) worked only after fixing where the entry points were loaded: they were checked against the DEVICE extension list, but debug utils is an INSTANCE extension, so the guard was false on every run and the object names and labels were silently inert (RenderDoc and Nsight captures had no pass names at all).

`AVER_VK_DUMP_SPIRV=<dir>` writes every compiled module as `<entry>.spv`. SPIR-V carries `OpDecorate DescriptorSet`/`Binding` on each resource, so this answers "where did the compiler put it" without needing an SDK — which is how the post chain's bindings were verified before one was available.

## What is left

The ten remaining validation errors are all from the same known gap. Nine of them are the bind-map fallback shaders: 7 on the mesh stage (`MSVoxel`'s `gVerts`/`gIndices`/`MeshCB`) and 2 on the vertex stage (`gInstanceWorlds` at `t17`) — because `GraphicsPipelineDesc::instanced` and feature-module mesh geometry are both unimplemented here. One is not an error at all: the layer's own notice that a VUID hit `duplicate_message_limit`. The fallback note below discusses both.

**Nothing leaks at teardown** — four separate fixes made this work. `~VulkanDevice` did not call `uiShutdown()` (so the toolkit's objects outlived the device — the loader said `vkDestroyBuffer: Invalid device`); the factory's shutdown freed each shader's module but not its per-layout **variants**; `VulkanRenderContext` had **no destructor at all** (so its two constant rings and the shared zero CBV simply stayed); and four device-owned handles sat in anonymous-namespace statics outliving the device. The last is a pattern to watch: a file-scope `g_` holding a `Vk` handle leaks, and also outlives the device that filled it, so a second `VulkanDevice` in one process would use a destroyed handle.

## Descriptor lifetime: how it works

Two defects showed up as `VkDescriptorSet ... was destroyed or updated without UPDATE_AFTER_BIND`. That message puts the command buffer into an **INVALID state**, after which the driver **drops every call recorded later** — one bad write took the whole rest of the frame.

**Binding sets are ringed `kFrameCount` deep.** A set may not be rewritten while a command buffer that bound it is pending. `beginFrame()` waits the timeline value retiring the frame-in-flight slot, so that ring slot is provably free. A write lands in one slot, others go stale; each slot's descriptor is recorded (`BindingSlotState`) and replayed lazily by `bindingSetForFrame()`. Vulkan cannot copy a binding out of a set, so recording the write is the only way to bring a second set up to date.

**Collisions are measured to be cross-frame only**: a probe counting intra-frame write-after-bind found zero. `writeBindingSlot` still warns if it happens, because the ring cannot cover it.

**`setConstantBuffer` takes a fresh set** rather than rewriting the one `bindDeclaredDescriptors` already bound — the same allocate-and-retire per `setPipeline`, same cost, same lifetime rule. `g_constantsInfos` mirrors the set so the fresh one can be populated in full instead of inheriting only what one slot changed. A dynamic offset would have been cheaper but cannot work: these are `UNIFORM_BUFFER_DYNAMIC`, but the write points a slot at the per-frame constant *ring* (a different `VkBuffer`), and a dynamic offset cannot change which buffer a descriptor names.

**Descriptor-set handles are RECYCLED**: a freed set hands its handle straight back, making the validation layer's report read like nonsense. Logging every allocation, retire and free with its handle and frame made the sequence legible.

## Textures and samplers: how it works

`buildRegisterBinds` (`VulkanRegisterMap.hpp`) derives one `-fvk-bind-register` per register from the `PipelineLayout`, and `moduleForLayout` passes it to DXC. Four validation errors closed:

| Register | Was | Is |
| --- | --- | --- |
| `t9` `gBaseColorMap` | set 0, binding 9 — undeclared | set 1, binding 0 |
| `t14` `gL1BaseColorMap` | set 0, binding 14 — undeclared | set 1, binding 5 |
| `s1` `gShadowSamp` | set 0, binding 1 — a texture is there | set 3, binding 1 |
| `s2` `gMaterialSampler` | set 0, binding 2 — likewise | set 3, binding 2 |

HLSL numbers table 1 **continuously above table 0** (`t(srvCount)..`) because that is what D3D12 expects, and only this backend splits the two into separate sets that each restart at binding 0. No `-fvk-*-shift` can express that — a shift moves bindings within one space, not between sets.

**The map must be COMPLETE**: supply one `-fvk-bind-register` and DXC demands one for every resource (`error: missing -fvk-bind-register for resource`). Descriptor cbuffers go in the map too, and `patchCbuffersForLayout`'s `[[vk::binding]]` half stands down when it is in use — two answers to the same question is one more than DXC accepts.

**A layout does not always describe its whole shader** (unimplemented features above, plus resources a shared header declares and a shader does not use). Rather than turn a compile error into a validation error, `moduleForLayout` retries WITHOUT the map, restoring exactly the placement that shader had before — no regression, and a warning naming it.

**`descriptor set 3 is never bound` is the next error.** The immutable sampler set is written once at layout-build time; a pipeline whose shader names a sampler statically uses set 3, and Vulkan requires every such set to be bound before the draw. `VulkanRenderContext` binds it once per `setPipeline`.

`tests/rhi/src/RegisterBindMapTest.cpp` pins the mapping with no device and no SDK, against the exact layout that was failing (Voxi's GI: `srvCount` 9, `srvCount1` 8, `uavCount` 4, 3 samplers).

## Constant buffers: how it works

`patchCbuffersForLayout` (`VulkanResourceFactory.cpp`) lowers **every** cbuffer to whatever its `PipelineLayout` says it is:

- `constantDwords[N] != 0` → root constants, folded into one `[[vk::push_constant]]` struct.
- `constantDwords[N] == 0` → a descriptor, at binding `N` in `kVkSetConstants`.

It replaced a per-block needle that handled only `PerObject` at b1. **A cbuffer's kind is not a property of the shader** — `cbuffer SkinParams : register(b3)` reads identically either way — so the `PipelineLayout` is threaded down to compilation; `RhiShader` keeps its source and `moduleForLayout()` re-patches at *pipeline* creation.

**The padding is load-bearing**: `pushConstantLayout()` places the object block at bytes 0–128 whether or not a shader declares it, so a shader declaring only b3 needs fields at byte 128 — where the engine pushes them. Get it wrong and nothing crashes; every vertex simply skins by the top row of `gWorld`. `tests/rhi/src/CbufferLayoutPatchTest.cpp` pins this with no device and no Vulkan headers (41 `check()` calls today, up from 33), and was checked by deliberately removing the padding (exactly one check failed at the time this was written; not re-verified against the current, larger test).

## Also worth knowing

`src/VulkanPipeline.cpp` is **on disk and deliberately not in the build**. It holds an unfinished file split: six functions moved out of `VulkanResourceFactory.cpp` whose originals were never deleted, and all six **differ**. The linker proved which side wins. See `CMakeLists.txt` for the full story before touching either file.
