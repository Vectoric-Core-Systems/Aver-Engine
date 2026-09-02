# The Vulkan backend

> **STATUS, checked against source on 2026-08-25 (engine at v0.4.0): real, off by default, and one
> shadow map short of correct.** `modules/rhi.vulkan` is not a stub and has not been for two weeks.
> It builds a real `VkInstance`/`VkPhysicalDevice`/`VkDevice`, presents a swapchain, draws the scene
> — grid, cube, shadow, sky, world axes — with instanced draws, runs the whole editor UI (menus,
> toolbar, World Outliner, Details, dockspace, 3D viewport) through its own ImGui backend, and tears
> down with **zero leaked objects** and **10 non-fatal validation errors, down from 156**. It needs
> no Vulkan SDK to build or run. `AVER_RHI_VULKAN` is still `OFF` by default in CMake
> (`CMakeLists.txt:30`), so a default build of the engine contains no Vulkan code at all. And it has
> one confirmed, precisely-diagnosed rendering defect: **the cascaded shadow map has never been
> written on this backend**, for a structural reason recorded below.
>
> This replaces a 2026-08-02 draft that opened "STATUS: PLAN ONLY. No Vulkan code has been written."
> Everything below was checked against the tree as it stands, not carried over from that draft or
> from any commit message taken at face value — commit messages are cited as evidence of intent,
> source and `git log` dates are what settled every claim.

---

## What exists

Two modules, both new since the 2026-08-02 draft:

| Module | Files | Purpose |
|---|---|---|
| `Aver.RHI.Vulkan` (`modules/rhi.vulkan`) | `VulkanDevice.cpp` (3758 lines, was 3798), `VulkanResourceFactory.cpp` (3000, was 2964), `VulkanCommon.hpp` (2123, was 2136), `VulkanRenderContext.cpp` (1478, was 1463), `VulkanPipeline.cpp` (686, unchanged, **not built** — see below), `VulkanShaderCompiler.cpp` (293, was 286), `VulkanRegisterMap.hpp` (75, unchanged) | The `IDevice` implementation: instance/device bring-up, swapchain, the fixed scene/sky/line/mesh pipelines, resources, binding, barriers, acceleration structures, capture. |
| `Aver.RHI.Vulkan.ImGui` (`modules/rhi.vulkan.imgui`) | `ImGuiVulkanUiBackend.cpp` | The concrete Dear ImGui backend, structural twin of `modules/rhi.d3d12.imgui`, plugged into `Aver.RHI.Vulkan`'s `vkb::IUiBackend` seam. Only `Sandbox` links it. |

`modules/rhi.vulkan/src/VulkanCommon.hpp` declares `class VulkanDevice final : public IDevice` with
101 `override` method declarations — not the 5-method minimum the base class requires, and not the
13-line stub the 2026-08-02 draft described. That stub (`createVulkanDevice` tracing `"backend
stub — needs Vulkan SDK; real device later"` and returning `nullptr`) was the state of the file
before commit `895ba68` (2026-08-09, "Checkpoint: a Vulkan backend, 8,387 lines, deliberately never
compiled") and is gone.

**Building this took roughly 30 commits touching `modules/rhi.vulkan`** from `895ba68` through
`c97f092` (2026-08-23) — re-derived by walking `git log -- modules/rhi.vulkan/` over that range, not
assumed. Eleven of those, `df16c95` through `c97f092`, are the ones titled `Vulkan: ...` that took
the backend from "compiles, reaches a live device, does not yet render a correct frame" (`c7c6147`,
2026-08-10) to presenting, drawing the editor, and leaking nothing. The remaining commits in that
range are shared-code changes (a texture-copy RHI addition, a line-mesh leak fix, ambient-lighting
and cloud/atmosphere work) that happened to also touch a Vulkan file, or are the two 2026-08-10
commits (`c7c6147`, `8a3eaf5`) that got the module compiling and vendored a working shader compiler
for it.

## Why no SDK is needed

The 2026-08-02 draft's entire middle section was about acquiring the LunarG Vulkan SDK. That
blocker is resolved, and resolved differently than the draft assumed:

- **Headers** are vendored at `third_party/vulkan-headers` (Khronos, Apache-2.0), pinned to header
  version 1.3.296.
- **The loader** is not linked at all — `modules/rhi.vulkan/CMakeLists.txt` builds every translation
  unit with `VK_NO_PROTOTYPES=1`, and `VulkanCommon.hpp`'s `VulkanApi` table resolves every entry
  point at runtime via `LoadLibraryW(L"vulkan-1.dll")` + `GetProcAddress`, starting from
  `vkGetInstanceProcAddr` itself. `vulkan-1.dll` ships with the GPU driver on every Windows machine
  with a GPU, SDK or not.
- **The shader compiler** is the part that actually needed vendoring, and `8a3eaf5` (2026-08-10,
  "Vendor a SPIR-V-capable DXC, and fix the push-constant form it finally exposed") did it: the
  Windows SDK's `dxcompiler.dll` accepts `-spirv` and refuses at codegen (`SPIR-V CodeGen not
  available. Please recompile with -DENABLE_SPIRV_CODEGEN=ON.`) because Microsoft's redistributable
  is built without that backend. `third_party/dxc-spirv/dxcompiler.dll` is the official
  `microsoft/DirectXShaderCompiler` release `v1.9.2607`, built with it — confirmed present, 27 MB
  (`28,079,968` bytes), under MIT-over-NCSA licensing recorded in the same directory's
  `LICENCE-MIT.txt`/`LICENSE-LLVM.txt`. `modules/rhi.d3d12/CMakeLists.txt`'s copy step prefers this
  vendored DLL over the SDK's, so D3D12 keeps getting DXIL from the same file Vulkan gets SPIR-V
  from.

So the backend is buildable and runnable on a bare Windows machine with no SDK installed — but a
**LunarG SDK is still worth installing for development**, because `VK_LAYER_KHRONOS_validation` only
ships with it, and `modules/rhi.vulkan/README.md` records why that matters more here than it would
elsewhere: *"AMD's discrete driver does not report a shader/layout mismatch — LLPC calls `abort()`"*,
so a binding bug on that driver crashes the process with no diagnostic at all rather than a
validation message. Every bug fixed in this backend so far was found through the layer; none of them
were found by the driver telling you what was wrong.

One comment elsewhere in the tree has not caught up to this: `modules/rhi/CMakeLists.txt:32` still
says *"Vulkan stays off until the SDK is installed"*, which was true when written and is not the
reason it is off today (see "Why it is still off by default" below). That file is shared RHI, not
this module, and is out of scope for this document to fix.

---

## What actually renders

Confirmed by reading `VulkanDevice::init` (`VulkanDevice.cpp:521`, drifted from `658`) and the commits
that built it:

- A real `vkCreateInstance` → `vkEnumeratePhysicalDevices` → `vkCreateDevice` chain
  (`VulkanDevice.cpp:595`, `:617-618`, `:740` — all drifted down from `737`/`759-762`/`859` as the
  file's earlier sections grew), not a stub returning a null device.
- A live swapchain, presenting. `modules/rhi.vulkan/README.md` states it plainly: *"It presents a
  frame — grid, cube, shadow, sky and world axes — which it did not until `7504a80`."*
- **Instanced draws**, added in `c97f092` (2026-08-23): `drawMeshInstanced` issues one
  `vkCmdDrawIndexed` with a real `instanceCount`, reading per-instance world matrices from a
  `StructuredBuffer` at its own descriptor set (`kVkSetInstances`) rather than the raw GPU-address
  push constant D3D12 uses for the same data.
- **The whole editor UI**, added in `43a04c8` (2026-08-23, "Vulkan: the editor runs on it"):
  `modules/rhi.vulkan.imgui`'s `ImGuiVulkanUiBackend` is installed from `sandbox/src/SandboxApp.cpp`
  (`rhi::vkb::imgui_backend::create()` then `rhi::vkb::installUiBackend(e.device(), ...)`, around
  `SandboxApp.cpp:1411`, drifted again from `1436`, itself drifted from `1057-1058`) whenever the live device reports itself as the Vulkan backend. See "The
  two contradictions" below for how this reconciles with an older note claiming the opposite.
- **Ray tracing and mesh-shader capability queries are real, not deferred.** `VulkanDevice.cpp`
  queries `VK_EXT_mesh_shader` and the acceleration-structure extension set and reports
  `caps_.meshShaderTier = 1` / `caps_.rayTracingTier = 11` when the hardware and DXC both support
  them (`:1022-1052`, drifted from `:1136-1158`); `buildBlas`/`buildTlas` are implemented in
  `VulkanRenderContext.cpp` (`:926`, `:985` — these two have not moved), including the same
  `instanceCustomIndex` handling the D3D12 backend uses for RT reflection, and
  `requestCapture`/`getCapture`/`getFrameImage` exist and are wired to a real readback path
  (`requestCapture` inline in `VulkanCommon.hpp:1402`; `getCapture`/`getFrameImage` at
  `VulkanDevice.cpp:3607`/`:3612`, drifted from `:3691-3696`). None of this matches the 2026-08-02
  draft's plan to defer ray tracing and mesh shaders to a much later slice — they were built alongside everything
  else, not held back.
- **Teardown leaks nothing.** `972dac7` (2026-08-23) took `vkDestroyDevice`'s leaked-object count
  from 40 to 0 across four fixes: `~VulkanDevice` now calls `uiShutdown()` (it never did, so the UI
  toolkit's own pool/buffers died after the device that owned them, producing `vkDestroyBuffer:
  Invalid device` from the loader); the shutdown sweep now frees every shader *variant*, not just
  its base module; `VulkanRenderContext` gained a destructor (it had none, despite owning two
  `VkBuffer`+`VkDeviceMemory` pairs); and four device-owned Vulkan handles that lived in
  anonymous-namespace file statics (unreachable by any destructor, and a latent use-after-free if a
  second device were ever created in-process) became members.
- **Descriptor-set lifetime bugs are fixed: 156 validation errors down to 10, all in one known,
  named class.** `modules/rhi.vulkan/README.md`'s own "What is left" section — last edited by
  `57eb3e9` (2026-08-23), the most recent commit to touch that file — states the remaining 10 as *9
  bind-map fallback shaders (mesh-stage and instancing paths this backend does not fully cover) plus
  1 non-error: the validation layer's own `duplicate_message_limit` notice*. The fix that got there:
  binding sets are ringed `kFrameCount` deep so a set is never rewritten while a pending command
  buffer still references it, and `setConstantBuffer` allocates a fresh descriptor set instead of
  mutating one already bound — both because Vulkan's validation layer treats *"descriptor set …
  destroyed or updated without `UPDATE_AFTER_BIND`"* as fatal to the whole command buffer, not a
  warning: the driver silently drops every later call in that buffer.

  **Correction to the brief for this document:** the number to cite here is **10**, not "~14". 14
  was the count as of `6e343c0` (before `972dac7` and `57eb3e9` each found and fixed more); it moved
  twice more after that commit and the tree has not regressed since.

---

## The two contradictions, resolved

### 1. Does the editor run on Vulkan?

**Yes, as of `43a04c8` (2026-08-23).** A note elsewhere in this project's memory says there is *no
editor UI on Vulkan because `rhi.vulkan.imgui` does not exist* — that was correct for the roughly two
weeks between the backend first presenting a frame and `43a04c8` landing, and is now stale. Checked
directly:

- `modules/rhi.vulkan.imgui/` exists on disk (`CMakeLists.txt`,
  `include/aver/rhi/vulkan/ImGuiUiBackend.hpp`, `src/ImGuiVulkanUiBackend.cpp`).
- It is wired into the build: `CMakeLists.txt:264-272` adds it as a nested subdirectory of the
  `AVER_RHI_VULKAN` block, additionally gated on `AVER_ENABLE_UI` — the same two-level gating
  `modules/rhi.d3d12.imgui` uses.
- `sandbox/src/SandboxApp.cpp` installs it: the block, now around line 1398 (drifted again from the
  1420 previously recorded here, itself drifted from 1042 as the file grew), picks one of two
  `installUiBackend` calls "by what the device actually is" — `rhi::d3d12::installUiBackend`
  (`SandboxApp.cpp:1404`) for a D3D12 device, `rhi::vkb::installUiBackend`
  (`SandboxApp.cpp:1411`) for a Vulkan one.
- `43a04c8`'s own commit message claims the result was seen on screen ("menus, toolbar, World
  Outliner, Details, dockspace and the 3D viewport with the scene in it -- now draws on the Vulkan
  backend at 60 FPS"). That claim is not independently reproducible from this pass — this document
  was written without building or running anything, per the constraints of this task — but it is
  corroborated by two things a commit message alone would not be: `modules/rhi.vulkan/README.md`
  says, in its own voice and dated to the same day, **"THE EDITOR UI DRAWS"**, and the swapchain-fill
  symptom the draft-era backend had (*"the image fills only part of the swapchain"*) is explained
  away by the same fix (`972dac7`'s README diff: *"the swapchain is filled too — that symptom was the
  same gap, since with no UI laying out a dockspace `setViewportRect` kept the editor's default
  1600x900"*) — a mechanistic explanation, not just an assertion that it now works.

So: the survey claim that Vulkan "runs the full editor" is the one to believe. The note saying
otherwise was accurate when written and has not been updated since `rhi.vulkan.imgui` was built.

### 2. Does `VulkanShaderCompiler.cpp` still block a correct frame on a descriptor-set problem?

**No — that comment is stale by about two weeks, and the fix it says doesn't exist is written
elsewhere in the same file's own module.**

`modules/rhi.vulkan/src/VulkanShaderCompiler.cpp:1-44` carries a banner reading *"WHAT IT CANNOT DO
IS PUT DESCRIPTORS IN THE RIGHT SETS… THE FIX IS NOT IN THIS FILE… (b) `compile()` is given the
`PipelineLayout` and emits one explicit `-fvk-bind-register` per declared register, which needs a
signature change…"* and closes with *"It does not yet render a correct frame, and saying otherwise
because the build went green is exactly the kind of unbacked claim this repository has been burned by
before."*

`git log -p` on this file shows that entire banner was written in commit `c7c614720ee7eb8` at
**2026-08-10 14:03**, and the diff of every commit since that has touched this file (`8a3eaf5` same
day, then `26220c6`, `660639f`, `86013e1`, `fc35dcc`, `95cfbe1`, `9939940`, `7e6009d`, all on
2026-08-22) never re-touches those lines — they are original, unedited text from the file's second
commit.

Option (b), which the banner says is not implemented, **is implemented**, in this same file and its
neighbors:

- `modules/rhi.vulkan/src/VulkanRegisterMap.hpp:73` declares `buildRegisterBinds(const
  PipelineLayout&, VkRegisterBind*, u32, bool)` — "fills `out` with one `VkRegisterBind` per SRV /
  UAV / SAMPLER register a `PipelineLayout` declares."
- `VulkanShaderCompiler::compile` (`VulkanShaderCompiler.cpp`, `bool ... compile(...)`) takes an
  optional `const VkRegisterBind* binds, u32 bindCount` and, when supplied, emits one
  `-fvk-bind-register <class><number> <space> <binding> <set>` per entry instead of the blanket
  `-fvk-u-shift` it uses when there is no map — the two are mutually exclusive by DXC's own rule
  (*"`-fvk-u-shift` cannot be used together with `-fvk-bind-register`"*), which is recorded in the
  file as how this was discovered.
- `VulkanResourceFactory::moduleForLayout` (`VulkanResourceFactory.cpp:2056`) is the caller: it
  builds the register map from the pipeline's real `PipelineLayout` and passes it into `compile()`,
  falling back to the old blanket-shift behaviour only for the specific registers a layout does not
  describe (mesh geometry, and resources a shared header declares but a given shader does not use) —
  documented in the function itself as a deliberate, narrow fallback, not a general failure.
- `modules/rhi.vulkan/README.md`'s "Textures and samplers: done, and how" section documents this as
  finished, with a before/after table of four specific registers that moved from an undeclared
  collision to their correct set.

So the comment describing this as an open, structural blocker is describing a state that stopped
being true within the same day it was written. The register-space design problem it lays out
(DXC maps one HLSL `register space` to one Vulkan descriptor set; this engine's shared HLSL declares
every table in `space0`; two logical binding tables therefore need two different answers to "which
set is this register in", which no `-fvk-*-shift` argument alone can express) is real and worth
keeping as design rationale for *why* the per-register map exists — it is just no longer an open
problem.

---

## The known open defect: the shadow cascade map is never written

**Confirmed in source, and it is the one thing in this document that is still actually broken.**

`c97f092` (2026-08-23, "Vulkan: instanced draws, and the shadow-map defect found while verifying
them") found this while verifying that instanced draws changed no pixels, by forcing the raster
shadow path with `--no-rt` and probing the floor beside the cube in the default scene:

```
D3D12   darkest floor pixel (13, 15, 19)  -- a shadow
Vulkan  darkest floor pixel (66, 66, 66)  -- unshadowed floor, no shadow
```

**The cause is `pushRenderScope`** (`VulkanDevice.cpp:2524-2528` — moved down from the `2578-2583`
recorded here originally by a "KNOWN DEFECT, MEASURED, NOT YET FIXED" comment block added above it;
the function body itself is unchanged):

```cpp
bool VulkanDevice::pushRenderScope(VkCommandBuffer cmd, const VkRenderingInfo& ri) {
    if (renderScopeDepth_ != 0) return false;   // already inside one: join it rather than nest
    api_.CmdBeginRendering(cmd, &ri);
    ++renderScopeDepth_;
    return true;
}
```

Vulkan's dynamic-rendering scopes (`vkCmdBeginRendering`/`vkCmdEndRendering`) cannot nest, and this
function's join-don't-nest behaviour was written correctly for the case it was designed around: two
independent owners drawing into the *same* attachments on one command buffer. It is wrong when the
inner caller names **different** attachments. `VoxiRenderer::shadowPass` calls
`setRenderTargets(nullptr, 0, shadowTex_)` and issues its draws from inside the scene scope that
`beginFrame` already opened over the scene's colour/depth targets — so those draws join the *scene's*
attachments instead of the shadow map's. The shadow map is never written. It looks like nothing is
wrong because the light-space geometry lands outside the scene viewport and is simply clipped rather
than producing visible corruption. `giShadowPass` has the identical shape, so voxel GI light
injection is unshadowed on this backend too — the defect is not limited to the raster cascade.

It is masked in normal use because **ray-traced shadows are the default and do work** — this defect
only shows up with `--no-rt` or on hardware without ray-tracing support.

**Not fixed as of `c97f092`, and the attempt that was tried is recorded rather than hidden.** Making
`pushRenderScope` retarget to the inner attachments does get draws into the shadow map, but surfaces
three more mismatches at once: the bound pipeline's `rasterizationSamples` and
`VkPipelineRenderingCreateInfo` formats are still the scene's, and the shadow image is never
transitioned to `DEPTH_ATTACHMENT_OPTIMAL`. That attempt took validation errors from 10 to 40 and
broke the floor draw entirely, and was reverted. The real fix, per the comment left at
`VulkanDevice.cpp:2490-2521` for the next person to find, is architectural: the scope needs to
**follow the currently-bound render targets** (close on retarget, reopen lazily at the next draw,
persist across draws instead of opening and closing around each one), and pipelines need to be built
against the sample count and formats of the targets they are actually used with — a change to how
every pass on this backend acquires its render scope, not a local patch.

---

## Why it is still off by default

`AVER_RHI_VULKAN` is `OFF` in `CMakeLists.txt:30` (*"Vulkan backend (needs Vulkan SDK)"* — that
parenthetical is also now inaccurate, per "Why no SDK is needed" above). Turning it on:

- Adds `modules/rhi.vulkan` and, if `AVER_ENABLE_UI` is also on, `modules/rhi.vulkan.imgui`
  (`CMakeLists.txt:264-272`).
- Defines `AVER_HAS_VULKAN=1` publicly on `Aver.RHI` (`modules/rhi/CMakeLists.txt:40`) and links
  `Aver.RHI.Vulkan` into `Aver.Runtime` (`modules/runtime/CMakeLists.txt:19`).
- Does **not** change what a default `Sandbox.exe` run does. `modules/runtime/src/Engine.cpp`'s
  request-a-backend path still has the bug the 2026-08-02 draft found: when `--backend vulkan` is
  passed, `dd.preferred` becomes `{Vulkan, D3D12, Vulkan, Null}` (`Engine.cpp:65-68`) — Vulkan named
  twice, D3D11 dropped from the fallback list entirely — and `Engine.cpp:55`'s own comment (*"D3D11
  and Vulkan are 13-line stubs that return nullptr today, so the default order really means D3D12 or
  Null"*) is itself now wrong about Vulkan specifically, since Vulkan is no longer a stub. This file
  is shared runtime code, out of scope for this document to fix, but it means a `--backend vulkan`
  request that somehow failed would still silently fall through to D3D12 with only a warning, exactly
  as the draft described.
- `scripts/gates.ps1` still has no `-Backend`/`--backend` handling anywhere in it (confirmed:
  `grep -n backend scripts/gates.ps1` returns nothing), so the pixel-probe oracle cannot run
  against this backend at all yet, let alone assert that a gate run actually used it — 18 gates in
  `$Gates` when this line was written, 20 now, the count having grown since is itself evidence the
  oracle keeps changing under a backend that has never once run through it. This is the
  same hole the 2026-08-02 draft flagged, unchanged. Recording `gates.baseline.vulkan.txt` before
  this backend is turned on by default would be recording nothing, silently.

Turning it on today gets you a backend that presents, draws the editor, and has one known shadow bug
and 10 harmless validation warnings — a real option for someone who wants to develop against it — but
it has not been exercised by the gate suite even once, has no cross-backend pixel comparison, and
"off by default" is the honest state of a backend that has not yet been asked to prove itself against
the same harness D3D12 is held to.

---

## What is not done

- **The cascaded shadow map bug above.** The single confirmed rendering defect.
- **`VulkanPipeline.cpp` is dead code that still exists.** 686 lines, deliberately excluded from
  `modules/rhi.vulkan/CMakeLists.txt`'s `SOURCES` list. It holds an unfinished file split: six
  functions (`tableSetLayout`, `getOrCreateSampler`, `descriptorLayout`, both pipeline creators, the
  free `pushConstantLayout`) were moved out of `VulkanResourceFactory.cpp` and the originals were
  never deleted — all six now **differ** between the two files. `26220c6`'s own investigation proved
  by linker warning (`LNK4006 ... second definition ignored`, six times) that
  `VulkanResourceFactory.cpp`'s copies are the ones that link; `VulkanPipeline.cpp` compiles to
  nothing live. It stays on disk, unbuilt, as a trap for whoever edits the "wrong" copy next.
- **9 of the 10 remaining validation errors are a genuine gap, not noise**: 7 on the mesh-shader
  stage (`MSVoxel`'s `gVerts`/`gIndices`/`MeshCB` — the fixed compute-driven mesh-geometry path this
  backend has not implemented) and, as of the register-map work, some on the instancing path where a
  `PipelineLayout` does not fully describe every register a shared shader declares. `moduleForLayout`
  falls back to the pre-map placement for exactly these cases rather than inventing a binding, which
  keeps them from being wrong rather than making them right.
- **No suballocator.** `VulkanResourceFactory.cpp` calls `vkAllocateMemory` once per buffer and once
  per image (`:954`, `:997`) — a direct translation of D3D12's one-`CreateCommittedResource`-per-
  resource pattern. Neither VulkanMemoryAllocator nor any other suballocator is vendored. This has
  not caused a failure yet because the scenes exercised so far (a handful of meshes and textures) are
  nowhere near a driver's `maxMemoryAllocationCount` (commonly 4096), but it is a real ceiling a
  bigger scene would hit.
- **No cross-backend correctness oracle.** Nothing compares a Vulkan frame against a D3D12 one, by
  pixel or by relational assertion. `scripts/gates.ps1` has no backend switch at all (see above), and
  no Vulkan-specific baseline file exists in `scripts/`.
- **Validation layers require a LunarG SDK install this machine does not have**, so nothing in this
  pass — or apparently any pass since `43a04c8` — has actually run the validation layer against the
  current state; the "10 errors, from 156" figures are what earlier sessions with the layer available
  recorded and left in the README, not something re-verified here.
- **This document itself was written without building or running the code.** Every claim above about
  what compiles, what a debug session found, and what a frame looks like traces to source text and
  `git log`, not to a build performed in this pass — this task's own rules prohibit building or
  launching anything in this tree while other work is in flight. `VulkanDevice.cpp`'s own top-of-file
  banner still says *"UNVERIFIED. House rule: no build, no run… not compiled"* — but that banner
  dates to `895ba68`, the very first checkpoint commit (2026-08-09), before the module ever compiled,
  and has not been touched since despite eleven-plus commits of build/run/test iteration afterward.
  It is exactly as stale as the two contradictions resolved above, for the same reason: nobody
  returned to update a comment once the code around it changed. Take the specific, falsifiable claims
  in the commit history (frame counts, validation-error counts that visibly went down step by step,
  measured pixel values) as the stronger evidence; take neither that banner nor any single commit
  message as proof on its own.

---

## Third-party dependencies actually in the tree

| Component | Where | Licence | Status |
|---|---|---|---|
| Vulkan-Headers (Khronos) | `third_party/vulkan-headers` | Apache-2.0 | Vendored, header v1.3.296. |
| DirectXShaderCompiler, SPIR-V build | `third_party/dxc-spirv/dxcompiler.dll` | MIT (Microsoft) over NCSA (LLVM base) | Vendored, `v1.9.2607`, 27 MB. `dxil.dll` (proprietary MS EULA, DXIL signing) deliberately **not** vendored alongside it — SPIR-V needs no signing. |
| Dear ImGui Vulkan backend | `third_party/imgui/backends/imgui_impl_vulkan.{h,cpp}` | MIT (already-vendored ImGui) | Vendored at the docking branch matching the ImGui version already in tree, "with permission asked first" per `43a04c8`'s commit message. Built only under `AVER_RHI_VULKAN` — it includes `vulkan.h`. |
| VulkanMemoryAllocator | — | — | **Not vendored.** See "No suballocator" above. |
| Vulkan validation layers | — | Apache-2.0 (LunarG SDK) | Not vendored, not required to build or run — required only for `--debug-layer` to do anything, and this machine does not have them installed. |
| glslang / shaderc | — | — | Not needed and not used — DXC with `-spirv` is the whole shader pipeline, for both backends. |

---

## For anyone picking this up next

In rough order of leverage:

1. **Fix `pushRenderScope`** so it follows the currently-bound render targets instead of joining
   whatever scope happens to be open. This is the one confirmed rendering defect, it is precisely
   diagnosed, and the comment at `VulkanDevice.cpp:2490-2521` already describes the shape of the fix.
2. **Give `scripts/gates.ps1` a `-Backend` switch** that selects the executable and baseline file
   together, the way `-Release` already does. Without it, this backend cannot be regression-tested at
   all, and cannot ever be turned on by default responsibly.
3. **Install a LunarG SDK on a development machine** and re-run with `--debug-layer` before trusting
   the "10 errors" figure as current, since nothing in this pass could re-check it.
4. **Finish or delete the `VulkanPipeline.cpp` split.** Two files with six functions that disagree is
   a standing hazard, not a cosmetic issue — the linker resolves it silently today, but "silently" is
   exactly the failure mode this project has been burned by before.
5. **Implement the mesh-geometry compute path** (`gVerts`/`gIndices`/`MeshCB`) this backend currently
   falls back around, to close the 9 remaining validation errors for real rather than routing past
   them.
