# Adding a Vulkan backend to Aver Engine

> **STATUS: PLAN ONLY. No Vulkan code has been written.** Produced 2026-08-02 by a workflow of
> five parallel readers over the tree, followed by an adversarial refutation pass on every
> load-bearing claim. Of 63 claims sent to verification, **47 were refuted or materially
> corrected and 16 survived** — so treat the corrected wording here as the authority, not any
> earlier summary. Every load-bearing statement carries a `file:line` that an agent actually
> opened. Re-check before relying on one.

---

## The honest starting position

**What exists:** one backend. `modules/rhi.d3d12/src/D3D12Device.cpp` is 4771 lines and is the only thing that draws a pixel.

**What the project says it has:** three. `CMakeLists.txt:28-30` declares `AVER_RHI_D3D12` (ON), `AVER_RHI_D3D11` (ON), `AVER_RHI_VULKAN` (OFF). Two of those three are 13-line files that return `nullptr`:

- `modules/rhi.vulkan/src/VulkanDevice.cpp:8-11` — `createVulkanDevice` traces `"[RHI.Vulkan] backend stub — needs Vulkan SDK; real device later"` (`:9`) and returns null.
- `modules/rhi.d3d11/src/D3D11Device.cpp:8-11` — identical shape, and **this one is ON by default**. `AVER_HAS_D3D11=1` is defined (`modules/rhi/CMakeLists.txt:17`), `Aver.RHI.D3D11` is linked into the runtime (`modules/runtime/CMakeLists.txt:15`), the fallback list at `modules/rhi/include/aver/rhi/RHI.hpp:72` names it — and it produces nothing. `modules/runtime/src/Engine.cpp:44-45` says so in a comment: the default order "really means D3D12 or Null".

**What the docs claim that is false today** (verified line by line — do not trust these while planning):

- `docs/rendering/RENDERING.md:82` and `:373` state the Vulkan module is "built only if VULKAN_SDK found". Nothing in the build tests `VULKAN_SDK`; `CMakeLists.txt:122` tests only the option, and `modules/rhi.vulkan/CMakeLists.txt` (9 lines) has no `find_package(Vulkan)` at all — line 9 is a comment deferring it to "Phase 3+".
- `docs/rendering/RENDERING.md:106`, `:280-281`, `:375` describe an on-disk PSO cache, an offline shader database and an `Aver.ShaderSystem` module. None exist.
- `shaders/README.md:3` and `tools/README.md:38` describe a tool `aver-shaderc` ("HLSL → DXIL/SPIR-V via DXC"). It does not exist; `tools/` contains `ActorSweep.cpp`, `MakeRig.cpp`, `MakeSamples.cpp`, `README.md`, `mcp`.
- `docs/rendering/RENDERING.md:272` claims shader reflection auto-generates pipeline layouts so bindings "can't drift between backends". Layouts are hand-declared in C++ (`modules/render.pt/src/PathTracer.cpp:79`, `modules/render.actorpreview/src/ActorPreview.cpp:220`) and registers are hand-written in HLSL.
- `docs/rendering/RENDERING.md:15` and `:123` say barriers are inserted only by a frame graph. There is no frame graph; feature modules call `ctx.textureBarrier`/`bufferBarrier` directly (`modules/render.voxi/src/VoxiRenderer.cpp:709-808`, `PathTracer.cpp:203-332`).
- `modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp:140-148` claims the mesh-vertex-buffer predicate "IS CURRENTLY TRUE OF EVERY MESH". Commit `5714ba3` made it false (`D3D12Device.cpp:703-706` gates on `computeWritten`).

The one document that is accurate is `docs/ARCHITECTURE.md:592-596`. Its four Vulkan obstacles (root CBVs/SRVs in the generic layout, the 144-byte constant budget, register-space collision, buffer state decay) each check out against code. This plan is largely an expansion of that list with the mechanism attached.

**What `--backend vulkan` does today:** it parses (`sandbox/src/SandboxApp.cpp:5656`, `modules/rhi/src/RHI.cpp:71`), `Engine.cpp:54-57` writes `preferred = {Vulkan, D3D12, Vulkan, Null}` (Vulkan twice, D3D11 dropped), the Vulkan branch is preprocessed out entirely when the flag is OFF (`RHI.cpp:53-58`), `RHI.cpp:85-87` warns "this run is NOT using the backend that was asked for", and `Engine::run` returns 0 (`Engine.cpp:114`). You get D3D12 pixels and a zero exit code.

### What "adding Vulkan support" costs

The interface surface is 54 pure virtuals across five interfaces (`IDevice` 5, `IResourceFactory` 22, `IRenderContext` 22, `ISwapchain` 4, `IRenderFeature` 1) plus 61 non-pure virtuals that already have bodies. **The split is the trap.** `IDevice` (`modules/rhi/include/aver/rhi/RHI.hpp:219-389`) has 53 `virtual` tokens: 1 defaulted destructor (`:221`), 5 pure virtuals — `backend()` `:222`, `adapterName()` `:223`, `createSwapchain()` `:244`, `beginFrame()` `:245`, `endFrame()` `:246` — and 47 with inline default bodies. A subclass overriding only those 5 compiles, links, and is accepted by `createDevice()` with no completeness check anywhere: `selfTest()` (`RHI.hpp:271`) has no call site outside the D3D12 backend, and `Engine.cpp:64-77` validates nothing. This is demonstrated in-tree — `modules/rhi/src/null/NullDevice.cpp:23-34` overrides exactly those 5 and is the factory's terminal fallback (`RHI.cpp:95`).

That wide-default base is deliberate and load-bearing (it is what makes headless/CI possible), but it means **a Vulkan device that runs without crashing can be missing 47 methods**, and several defaults return plausible values rather than errors:

- `meshVertexBuffer()` → 0 (`RHI.hpp:306`): the sole signal for per-frame BLAS rebuild (`VoxiRenderer.cpp:369`, feeding the only rebuild call site `:384`). A skinned character animates and its ray-traced shadow does not.
- `camera()` → false (`RHI.hpp:328-330`): `fitCascades` returns 0 (`VoxiRenderer.cpp:489`), `shadowPass` sets `shadowParams[1]=0` with no log (`:701-702`), and `VoxiShaders.hpp:279` lights every surface. Silent loss of *all* cascaded raster shadows, gated by no caps check.
- `IRenderContext::setDrawBinding()` → no-op (`RHIResources.hpp:440-442`): leaves binding table 1 and the b2 block unwritten (`D3D12Device.cpp:1216-1219`, `:4457-4463`), which `VoxiRenderer.cpp:729` calls out as a Tier-1 hazard.
- `skyAtmosphere()` → `{}` (`RHI.hpp:335`): `VoxiRenderer.cpp:426` takes the 0.545° default sun diameter and produces a wrong penumbra.
- The backend's own obligation on `IRenderFeature` is to **call** all 11 hooks at the right points (`prePass` `D3D12Device.cpp:2026`, `scenePass` `:2048`, `overlayPass` `:2808`). A backend that never invokes them silently disables every feature module.

Realistic scale, stated plainly: **4000–6000 lines of new backend code, plus ~1000 lines of shared-RHI and harness change, across roughly 15–25 focused sessions.** The 4771-line D3D12 reference already includes ImGui hosting and GPU capture, which are separate slices again. This is not a weekend, and it is not a port — it is a second implementation.

---

## The blocker: no Vulkan SDK

**Nothing Vulkan can be compiled on this machine.** No headers, no `glslc`, no validation layers, no `VULKAN_SDK` environment variable. `C:\Windows\System32\vulkan-1.dll` exists but that is the driver-shipped ICD loader — it gives a running program an entry point, not a build a header. `third_party/` vendors only ImGui, stb and Roboto; nothing Vulkan-related (volk, VMA, Vulkan-Headers) is anywhere in the tree.

Note that `-DAVER_RHI_VULKAN=ON` **configures and builds cleanly right now**, because `modules/rhi.vulkan/CMakeLists.txt` asserts nothing. That is worse than a hard error: the flag reads as "Vulkan enabled" and delivers a factory returning null.

### Cannot be done without the SDK
- Compile any Vulkan code (no `vulkan.h`, no `vk_platform.h`).
- Enable validation layers — which for a from-scratch backend is not a nicety, it is the entire debugging strategy.
- Verify anything about descriptor-set layouts, barrier correctness, or SPIR-V module validity.

### Can be done without the SDK
Everything in Slices 1–3 below: the backend-assertion work in the gate harness, a conformance harness for `IDevice`, the shared-RHI vocabulary changes, the push-constant budget fix, the comment/doc corrections, and the `Common`-state contract decision. That is a meaningful fraction of the total risk, and every item of it is rework avoided later.

### Exact install required

1. **LunarG Vulkan SDK for Windows** (`vulkan.lunarg.com`). Provides headers, the static loader (`vulkan-1.lib`), validation layers, `glslc`/`glslangValidator`, and `vkconfig`. Sets `VULKAN_SDK`. The SDK is a developer tool, not redistributed with the engine, so its aggregate installer licence does not conflict with the permissive-only rule — but the individual components you *vendor* do, and are listed below.

2. **A separate SPIR-V-capable DXC.** This is the part that needs care.

### Can the Windows-SDK dxcompiler.dll emit SPIR-V?

**No. PROVEN BY EXECUTION on 2026-08-02, not inferred.**

```
> dxc.exe -T ps_6_0 -E main spv_probe.hlsl -Fo out.dxil      # control
  (succeeds — 2800 bytes of DXIL)

> dxc.exe -T ps_6_0 -E main -spirv spv_probe.hlsl -Fo out.spv
  dxc failed : SPIR-V CodeGen not available. Please recompile with -DENABLE_SPIRV_CODEGEN=ON.
  (no output file written)
```

Run against `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`, the same SDK
build as the `dxcompiler.dll` this repo copies. The control proves the invocation is well-formed
and the compiler works; only the SPIR-V backend is absent. **A SPIR-V-capable DXC is a hard
prerequisite for slice 5, and the one on this machine will not do.**

The static evidence that originally motivated this, now corroborated:

The DLL this repo copies and loads is resolved at `build/CMakeCache.txt:27` to `C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxcompiler.dll`, copied beside the exe by `modules/rhi.d3d12/CMakeLists.txt:22-26`, and loaded by bare name at `modules/rhi.d3d12/src/D3D12Device.cpp:70`. It reports FileVersion 1.8.2502.11, "DirectX Compiler - Out Of Band", SHA256 `97ABD3B3083607DC29CC3F8EAE56C39C349B76EE3C20DEEB2F66258708E55BD8` (byte-identical to the SDK copy). A binary string scan finds:

- Present: `"SPIR-V CodeGen not available. Please recompile with -DENABLE_SPIRV_CODEGEN=ON."`
- Absent (zero matches): `GLSL.std.450`, `OpTypeInt`, `OpEntryPoint`, `spv_target_env`, `SPV_KHR_`
- Present but meaningless: `-spirv` / `-fspv-*` help strings — the option table is compiled in regardless of whether the backend is.

Positive control: a known SPIR-V-capable DXC on this machine (an Unreal-shipped ShaderConductor build) lacks the "not available" diagnostic and contains all of the above symbols. `OpCapability` is absent from both and is not a valid probe.

*(The original version of this section correctly flagged that no one had actually run the compile,
and said to treat the conclusion as likely rather than proven until someone did. That command has
now been run — see above — and it confirmed the inspection.)*

You therefore need a DXC built with `ENABLE_SPIRV_CODEGEN=ON` — either a Khronos/LunarG-distributed `dxcompiler.dll` or a self-build from the DirectXShaderCompiler source (NCSA licence, permissive). Expect ~27–31 MB, not the ~14 MB the SDK ships. SPIR-V-capable copies **do** exist on this machine under `C:\Program Files\Epic Games\UE_5.5` and `UE_5.8` (`ThirdParty\ShaderConductor\Win64`), but those ship under the Unreal EULA and are excluded by the permissive-only rule. Do not use them, not even "just to test".

Note also that `modules/rhi.d3d12/CMakeLists.txt:23` resolves the DLL from `CMAKE_RC_COMPILER`'s Windows SDK bin directory, and `scripts/payload.allowlist:51-52` lists `dxcompiler.dll`/`dxil.dll` unconditionally with none of the `?AVER_MODULE_*` gating used at `:40-44`. A second, differently-named SPIR-V DXC needs its own staging step outside `modules/rhi.d3d12` and its own allowlist entry.

---

## What must change in the SHARED RHI before any Vulkan code is worth writing

Ordered by rework saved. Items marked **[shared]** cannot be absorbed by the backend; items marked **[backend]** can be, and are listed because knowing they are absorbable prevents a premature and expensive refactor.

### 1. [shared] The gate harness must assert which backend actually ran — before anything else

This is first because everything downstream is unverifiable without it, and because it closes a hole that exists *today*.

`scripts/gates.ps1` (510 lines) contains no `--backend` token anywhere. `Invoke-Gate` builds args at `:279` and parses only three things from child stdout: the probe line (`:284`), the viewport rect, and the debug-layer totals (`:301`). The verdict chain (`:340-351`) tests exit code, probe placement, rect size, totals, and pixel value — never the device. The rest of stdout is discarded at `:280`, so `RHI.cpp:85-87`'s fall-through warning never reaches the runner, `record-gates.ps1` (params `:33-38`), or the MCP gates tool.

Meanwhile `Engine.cpp:54-57` puts D3D12 at `preferred[1]` behind any explicit request and `RHI.cpp:77-95` falls through with only an `AVER_WARN`. Consequence: a `-Backend vulkan` sweep that silently fell back would run all 162 gates on D3D12, satisfy every guard, and **pass** — and under `-Record` it would write D3D12 numbers into the baseline. The same hole is already live for `warp`: `D3D12Device.cpp:1256` falls back to hardware with only a warning when WARP is unavailable, so 18 hardware gates can be recorded and passed as `warp`.

Parsing `RHI.cpp:84` alone is insufficient — `backendName()` (`RHI.cpp:14-22`) reports "D3D12" for hardware and WARP alike. The line to parse is `sandbox/src/SandboxApp.cpp:507`, `[Sandbox] backend={} adapter='{}'`, which covers both the backend and the `--warp` claim.

**Change:** add `-Backend` to `gates.ps1`/`record-gates.ps1`, pass it through, parse `SandboxApp.cpp:507`, and fail the gate on any mismatch of backend *or* adapter class. Add `--backend-strict` to the app so a fall-through is a non-zero exit rather than a warning (`Engine.cpp:114` currently returns 0 unconditionally). Fix the fallback list at `Engine.cpp:54-57` while you are there.

### 2. [shared] Shader source: `ShaderDesc` has no artefact path

`ShaderDesc` (`modules/rhi/include/aver/rhi/RHIResources.hpp:163-172`) carries `const char* source` documented "HLSL text", an optional `prelude`, `-D` `defines`, and `u32 minShaderModel = 60`. There is no bytecode or SPIR-V member. `createShader(const ShaderDesc&)` (`RHIResources.hpp:371`) is pure virtual and is the only shader-creation entry point in the RHI, so **every backend is contractually obliged to accept HLSL text and compile it at runtime.**

There is no build-time compile step, no `.cso`/`.spv` artefacts, and no PSO or bytecode disk cache anywhere. ~2016 lines of HLSL across 8 files provide ~31 entry points, all compiled during device or feature init — and not only at init: `setSampleCount` (`D3D12Device.cpp:1425-1434`) recompiles six scene shaders plus the mesh pair from source on any live MSAA change.

Seven modules outside the backend author HLSL inline: `modules/rhi/src/RHIShaders.cpp:96` (`sharedShaderPrelude`) and `:656` (`postShaderSource`), `render.voxi/src/VoxiShaders.hpp:8`, `render.pbr/src/PbrShaders.cpp:11`, `render.pt/src/PtShaders.hpp:9`, `render.skin/src/SkinningPass.cpp:24`, `render.ui/src/UiShaders.hpp:7`, `render.actorpreview/src/ActorPreview.cpp:94`.

**Decision required, and it is a fork:**
- **(a) Keep runtime HLSL→SPIR-V.** No RHI change. Requires shipping a second, SPIR-V-capable DXC. Recommended.
- **(b) Grow `ShaderDesc` a bytecode/SPIR-V variant.** An RHI ABI change touching the 5 feature modules that build `ShaderDesc` plus the D3D12 backend, and it needs an offline build step that does not exist. Not recommended, but if you ever want a PSO cache it is the same work.

Either way, there is no shared compile path to reuse: `ShaderCompiler` lives in an anonymous namespace private to `D3D12Device.cpp` (`:30`, `:60`, `:157`) and is DXIL-only (args at `:127-135`, output `DXC_OUT_OBJECT`). The Vulkan backend writes its own wrapper regardless. Also fix `RHI.cpp:169-173`, where backend-agnostic code hardcodes `shaderModel = 51` — a DXBC concept — whenever `dxcAvailable` is false.

### 3. [shared] Capability vocabulary is D3D12's enums re-spelled, and five backend-neutral modules branch on it

`DeviceCaps` (`RHI.hpp:79-90`): `rayTracingTier` is DXR tier numbers (`:82`, set from `D3D12_RAYTRACING_TIER` at `D3D12Device.cpp:1409-1410`), `shaderModel` is HLSL model codes (`:86`, `60 + (HighestShaderModel & 0x0F)` at `:1395`, valid set hard-coded at `RHI.cpp:106`), `dxcAvailable` means the DXIL compiler specifically (`:88`), `meshShaderTier` is "D3D12 Ultimate" (`:87`).

The leak is not confined to the backend. Five backend-agnostic sites open-code the same D3D12-shaped conjunction:

- `modules/render.pt/src/PathTracer.cpp:47-48`
- `modules/render.voxi/src/Voxi.cpp:74` and `:81`
- `modules/render.voxi/src/VoxiRenderer.cpp:968` and `:1065-1066`

Every consumer of `rayTracingTier` tests `>= 11` to mean "inline ray query available" — which is already a predicate wearing an integer's clothes.

**Change:** add capability predicates (`canRayQuery()`, `canMeshShade()`, `canComputeSkin()`) to `DeviceCaps` or `IDevice`, migrate those five sites, and demote the tier integers to reporting-only. This is cheap now and expensive after a Vulkan backend has been written against the integers.

Two things that make it cheaper than it looks:
- `resourceBindingTier` has **no consumer anywhere** — written (`D3D12Device.cpp:1390`), clamped (`RHI.cpp:161-162`, `:181`), logged (`:1417`, `:1421`), never read for a decision. Corroborated at `docs/STATUS.md:1173`. Report 0 and ignore it.
- The published C ABI does **not** force a break. `aver_voxi_ray_tracing_tier`/`_mesh_shader_tier`/`_shader_model` (`voxi_abi.h:79`, `:83`, `:85`, `Voxi.cs:141`/`:145`/`:147`) read `voxi::DeviceInfo`, an independent mirror struct (`Voxi.hpp:36-46`), and `Aver.Render.Voxi` links only `Aver.Core` (`modules/render.voxi/CMakeLists.txt:11`). The single conversion is a hand-written field copy at `sandbox/src/SandboxApp.cpp:838-845`. That adapter absorbs a `DeviceCaps` redesign. The real exposure is that `voxi_abi.h` declares no version constant and exports no version function (`docs/ABI.md:941`), so if the *meaning* of those three integers ever changes, no caller can detect it. Decide the Vulkan→tier mapping now and write it down, or add a Voxi ABI version.

`clampCaps` warning: its only definition is `RHI.cpp:150` and its only call site in the entire repo is `D3D12Device.cpp:1413`. It is a no-op unless `--force-caps` is active (`RHI.cpp:152`). A Vulkan backend that copies D3D12's `queryCaps` and calls it would have its RT and mesh tiers zeroed under the `no-dxc` gate config (`gates.ps1:246`), because `RHI.cpp:169-174` encodes "no DXC ⇒ no SM 6.x ⇒ no RT". Either don't call it, or give `--force-caps no-dxc` a Vulkan meaning, or reject the token on Vulkan.

### 4. [shared, small] Device creation happens before there is a surface

`createDevice` (`RHI.hpp:396`) accepts no surface, and `Engine.cpp:64` builds the device before the swapchain at `:71`. Vulkan physical-device selection cannot test present-queue support against a `VkSurfaceKHR` that does not exist yet.

The window handle itself is fine: `SwapchainDesc::windowHandle` is a bare `void*` documented "HWND" (`RHI.hpp:23-29`), no Win32 type appears anywhere in `modules/rhi/include`, and D3D12 already does the same cast (`D3D12Device.cpp:1759`). `VkWin32SurfaceCreateInfoKHR` also wants an `HINSTANCE` the platform layer does not expose, but `Win32Window.cpp` registers its class against `GetModuleHandleW(nullptr)`, which a backend can reproduce.

**Change:** add an optional native window handle to `DeviceDesc` so a backend may create a surface during device creation, or accept deferring queue selection to `createSwapchain` and re-validating there. Pick one before writing physical-device selection.

Related but **[backend]**: `SwapchainDesc::bufferCount` is dead on both ends (`D3D12Device.cpp:1754` hardcodes `kFrameCount = 2` at `:32`; the only production caller never sets it, `Engine.cpp:67-70`), format is hardcoded (`kBackbufferFormat`, `:34`), and present mode is only expressible post-creation via `setVSync`/`vsyncCanDisable` (`RHI.hpp:251-256`) — so a Vulkan backend mapping vsync to FIFO/IMMEDIATE must recreate the swapchain on toggle. `ISwapchain` (`RHI.hpp:32-39`) has no acquire step, no image index, no semaphores, and `present()` returns void, so `vkAcquireNextImageKHR` hides inside `beginFrame` (documented at `RHI.hpp:245` as "acquires + clears"; D3D12 does `GetCurrentBackBufferIndex` there at `:2008`) and `VK_ERROR_OUT_OF_DATE_KHR` is handled by the backend recreating internally — the engine only calls `resize()` on a size change it polls for, and skips that poll during a modal resize (`Engine.cpp:121`, `:125`).

### 5. [shared] Resolve the `ResourceState::Common` contradiction

`RHIResources.hpp:66` states: *"Nothing transitions implicitly, and a resource's state carries across frames."* `modules/render.skin/include/aver/render/SkinningPass.hpp:74-78` states the opposite for its own buffers: a frame must end with the buffer back in `Common` because *"D3D12 decays every buffer to the common state when a command list finishes"*. The same D3D12 promotion/decay reasoning appears in `PathTracer.cpp:199-204` and `:322-323` and `VoxiRenderer.cpp:669-676`, and end-of-frame resets are performed at `SkinnedScene.cpp:239-240`, `SkinSelfTest.cpp:166`, `sandbox/src/SkinDrawTest.cpp:156`.

`Common` is the default `initialState` (`RHIResources.hpp:103`), the header asserts buffers are `Common` at the top of every frame (`:125`), and it maps to `D3D12_RESOURCE_STATE_COMMON` (`D3D12Device.cpp:539-541`). Vulkan has no promotion/decay rule. `docs/ARCHITECTURE.md:596` already names this as an obstacle.

**Change:** pick one semantics and make every module say it. The workable definition is *"`Common` means backend-tracked: a transition **to** `Common` is a release/no-op, and a transition **from** `Common` resolves against the backend's own tracked last-writer."* That is implementable in Vulkan and preserves D3D12 behaviour, but it must be written into `RHIResources.hpp:66` and the three modules must stop justifying themselves with D3D12's rule.

The rest of the barrier model ports mechanically and needs **no** shared change:
- `from`/`to` are supplied by the caller at every site (`RHIResources.hpp:477-484`), so a pure `ResourceState → (VkPipelineStageFlags, VkAccessFlags, VkImageLayout)` function gives exact masks with no per-resource tracking — structurally identical to the existing pure switch `toResourceStates` (`D3D12Device.cpp:524-542`). Nothing forces `ALL_COMMANDS`.
- `GeometryRead` ORs two D3D12 states (`:536-537`) but is buffer-only in all production code (`SkinningPass.cpp:293` → `:86` `bufferBarrier`), and Vulkan buffer barriers carry no layout and take OR-able access masks. Natural encoding, not a problem.
- `AccelerationStructure` is terminal and *enforced*, not just commented: `rejectAsState` (`D3D12Device.cpp:4657-4662`) is the first statement of both `textureBarrier` (`:4711`) and `bufferBarrier` (`:4722`), active in release builds. It needs no Vulkan mapping at all.
- Every actual `textureBarrier` call site (`ActorPreview.cpp:306`, `:354`; `VoxiRenderer.cpp:709`, `737`, `778`, `794`, `806`, `808`) uses only the five states that map 1:1 onto standard Vulkan image layouts.
- `ResourceState::VertexBuffer` is **dead** — it exists only at `RHIResources.hpp:79` and `D3D12Device.cpp:533`, with no call site transitioning to it. Consider deleting it rather than mapping it.
- Minor: `stateName()` (`D3D12Device.cpp:578-591`) has no case for `VertexBuffer` or `GeometryRead`, so a debug state-tracking mismatch prints `<unknown>`.

### 6. [shared, mechanical] Move the b5 triangle count out of push-constant space

`kObjectConstantDwords = 32` (`RHIResources.hpp:324`) is 128 bytes at b1, ABI-locked by `static_assert` at `RHIShaders.cpp:92-93` and indexed by feature stack arrays (`VoxiRenderer.cpp:726`, `:761`; `ActorPreview.cpp:345`). On the mesh path the backend adds 4 more dwords at b5 (`D3D12Device.cpp:1676-1678`, generically `:3360-3362`), total 144 bytes — over Vulkan's guaranteed 128-byte `maxPushConstantsSize` floor.

The cheap fix: the b5 block is backend-private. `kMeshGeometryConstantRegister` appears at exactly five sites repo-wide (`RHIResources.hpp:320`, `:333`, `RHIShaders.cpp:90`, `D3D12Device.cpp:1677`, `:3361`), no feature module names it, and its value is written only inside `dispatchMeshFor` (`D3D12Device.cpp:2120`, `:4513-4514`) as `{tris, 0, 0, 0}` from `indexCount / 3` — data the backend already holds. No `static_assert` covers its *size* (`RHIShaders.cpp:90` pins only its register number). Move it to a UBO or derive it in-shader and the budget is exactly 128.

Standing hazard worth recording: b1 then sits at exactly 128 with **zero headroom**, and ~20 of those bytes are already dead (`RHIShaders.cpp:134-137` declares `gReflectance`, `gF90`, `_objPad` as unread padding solely so the constant does not move; `fc[22]`/`fc[23]` are written as 0.0f at `D3D12Device.cpp:2082`).

### 7. [shared, cheap] A backend-neutral validation-summary hook

`gates.ps1:301` requires a line matching `debug layer totals`; its absence is verdict `NO-TOTALS` (`:347`), counted as a failure (`:408`), never retried (`:378` retries only `FAIL*`/`BAD-PROBE*`), and refused for recording (`:482`). The only producer in the tree is `D3D12Device.cpp:1345`, inside `~D3D12Device()` (`:1341`) guarded by `if (infoQueue_)` (`:1343`), which itself requires `desc.enableDebug && device_.As(&infoQueue_)` (`:1280`). `IDevice` has no validation-summary virtual.

**Change:** one virtual on `IDevice` returning (errors, corruptions, warnings), printed in one format by the shared layer; D3D12 routes its info-queue counts through it, Vulkan routes its debug-utils messenger counts through it. One virtual and one printf. Note this also fixes a D3D12-only failure: a machine without the Graphics Tools optional feature currently fails all 162 gates.

### 8. [shared, documentation] The header describes D3D12 mechanisms where it should describe contracts

These are comment changes, not struct changes, and doing them first prevents the next reader from concluding a refactor is needed when it is not:

- `RHIResources.hpp:206-207` — "a non-zero word count makes it root constants, zero a root CBV". Restate as a contract: non-zero = inline constants of that size, zero = a backend-allocated transient constant buffer.
- `RHIResources.hpp:281-284` — "Tier 1 requires every declared slot to hold a valid descriptor". True as a policy the engine imposes on itself; `nullFill` (`D3D12Device.cpp:3183`, called unconditionally at `:3862`) never consults `caps_.resourceBindingTier`. Say "the engine always null-fills" and cite Tier 1 as the origin, not the condition.
- `RHIResources.hpp:436-437` and `:439`, `RHI.hpp:346-347` — "upload ring", "root CBV", "its b2 constant block". The signatures are register-free and take CPU bytes; the register names belong in a parenthetical.
- `RHIResources.hpp:34`, `:140`, `:154` — `R32Uint` UAV atomics, `SampleCmpLevelZero`, "D3D12 rejects zero". All describe semantics Vulkan supports natively; the anisotropy quirk is already clamped inside the backend (`D3D12Device.cpp:3374`).
- `RHIResources.hpp:36` — `R32Typeless` is the one genuinely D3D-shaped *value* (resource `R32_TYPELESS` at `D3D12Device.cpp:402`/`3491`/`3527`, DSV `D32_FLOAT` at `:3565`, SRV `R32_FLOAT` at `:4010`, one consumer: the Voxi shadow atlas at `VoxiRenderer.cpp:861-863`). Vulkan is *simpler* here — one `VK_FORMAT_D32_SFLOAT` image, two views with the same format and the same depth aspect. Keep the enum value, fix the comment.
- `RHIResources.hpp:444` is stale: it describes root SRVs but sits above `drawMesh` (`:445`), whose implementation (`D3D12Device.cpp:4491-4499`) uses the input assembler and touches no root SRV.
- `RHIResources.hpp:207` says slot 0 is reserved and unenforced — the backend's own self-test violates it (`D3D12Device.cpp:4222`).

### 9. [backend] Things that look like shared problems and are not

Do **not** refactor these preemptively. Each is absorbable inside a Vulkan backend, and each has been checked:

- **Root CBVs.** No GPU address crosses the RHI boundary: `setConstantBuffer(slot, data, bytes)` (`RHIResources.hpp:437`) takes a CPU pointer, and `D3D12_GPU_VIRTUAL_ADDRESS` is created and consumed entirely inside the backend (`D3D12Device.cpp:4439-4442`); a grep of `modules/rhi/include` for GPU-address symbols returns nothing. Map a zero-dword slot to `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` with a dynamic offset into a host-visible ring. Two things you must replicate: the never-unbound invariant (`bindDeclaredRootCbvs`, `D3D12Device.cpp:4306`, `:4309-4324`, `:4327-4341` — every declared zero-dword slot gets a valid address on every `setPipeline`, slot 0 from the frame CB and the rest from a shared zero buffer, because `:4310` records that an unset root CBV can hang the GPU — and this repo has TDR history), and `minUniformBufferOffsetAlignment` instead of the hardcoded 256 (`:4479-4480`).
- **Register collisions under `-spirv`.** All HLSL is `space0` and b/t/u/s numbers overlap; `CSHistogram` (`RHIShaders.cpp:771-790`) uses b0, t0, s0 and u0 in one compiled entry point. The fix is `-fvk-{b,t,u,s}-shift` in the Vulkan backend's own compiler args, mirrored in its descriptor-set-layout construction. Nothing shared moves. The shift can be derived from `PipelineLayout` — the cumulative base arithmetic already exists at `D3D12Device.cpp:3312-3337`, and the `-D` injection channel already exists and works (`meshGeometryDefines`, `RHIShaders.cpp:649-653`; `materialShaderDefines`, `PbrShaders.cpp:278-283`, called from `VoxiRenderer.cpp:975`, `:1075`). Cross-table SRV identity is not recoverable from register numbers alone — the two tables (`kBindingTableCount = 2`, `RHIResources.hpp:213-214`; bound via `setBindingSet(set, table)`, `:432-433`) share one contiguous register range — so compute the shift per pipeline layout, not globally.
- **Binding-set shape.** `BindingSetDesc` (`RHIResources.hpp:298-306`) is a contiguous SRV run plus a contiguous UAV run. That maps directly onto a `VkDescriptorSetLayout` and is the struct's *most* portable property. `srvBaseRegister` only drives a diagnostic warn (`D3D12Device.cpp:4396-4398`); `uavBaseRegister` is never read anywhere. The "right dimension" rule is a contract, not an invariant — only `setSrvBuffer` (`:4066-4069`) and `setUavBuffer` (`:4087-4090`) validate the declared kind; `setSrv`, `setUav` and `setSrvTlas` derive it from the resource.
- **Render passes.** There is no render-pass concept, and `setRenderTargets` requires the caller to have pre-transitioned (`RHIResources.hpp:428-430`, honoured — `D3D12Device.cpp:4358-4375` does zero transitions). Use dynamic rendering with a **lazy open-at-first-draw / close-at-first-non-draw** scheme. Deferral is mandatory, not optional: `VoxiRenderer.cpp:757-759` sets render targets to *zero attachments* (a UAV-only raster pass) before setting the viewport, so `renderArea` is unknown at bind time. That scheme covers 100% of existing call sites with no barrier relocation — no feature issues a barrier between `setRenderTargets` and a draw, no `scenePass`/`overlayPass` implementation issues any barrier at all, and every `clearDepth` is trivially first-use so it folds into `loadOp = CLEAR` (`VoxiRenderer.cpp:712-713`, `ActorPreview.cpp:311-314`).
- **The backend's private transitions.** Invisible from the header and easy to miss: swapchain PRESENT↔RENDER_TARGET (`D3D12Device.cpp:2573`), MSAA resolve (`:2606-2615`), bloom mips (`:2667-2671`), composite (`:2737`), the viewport texture (`:2748-2756`, `:2798-2800`), and mesh vertex buffers after upload (`:1996-1998`). These must be reproduced.
- **Y axis and NDC.** The engine contract is centimetres, +Z up, +X forward, left-handed, row-major/row-vector. Vulkan's framebuffer Y direction differs from D3D12's; the standard remedy is a negative-height viewport (`VK_KHR_maintenance1`, core in 1.1) inside the backend. *(External API knowledge — not derived from this repo; verify against the spec when the SDK is installed.)*

---

## Third-party dependencies

Nothing Vulkan-related is vendored today. `third_party/` contains ImGui, stb and Roboto; Jolt lives under `modules/physics.jolt`. **Every licence below must be re-read from the vendored source at the time it is added** — these are stated from general knowledge, not from files in this tree.

| Component | Licence | Needed? | Notes |
|---|---|---|---|
| **Vulkan-Headers** (Khronos) | Apache-2.0 OR MIT | **Required** | Ships with the SDK. Vendor a pinned copy only if you want SDK-independent builds. |
| **volk** (Kapoulkine) | MIT | **Optional, recommended** | Meta-loader: dynamic entry-point loading, no link against `vulkan-1.lib`, faster device-level dispatch. This repo already dynamically loads `dxcompiler.dll` by bare name (`D3D12Device.cpp:70`), so the pattern is familiar. Two files. Not required — linking the static loader works. |
| **VulkanMemoryAllocator** (AMD) | MIT | **Required in practice** | Single header. The D3D12 backend uses `CreateCommittedResource` per resource; the naive Vulkan translation is one `vkAllocateMemory` per resource, which hits `maxMemoryAllocationCount` (commonly 4096) on any real scene. Writing a suballocator by hand is a multi-session detour for no benefit. |
| **DirectXShaderCompiler**, SPIR-V-enabled build | NCSA / University of Illinois (permissive) | **Required** (assuming §2 holds) | Prebuilt release from the DXC GitHub releases, or self-built with `-DENABLE_SPIRV_CODEGEN=ON`. ~27–31 MB. Needs its own staging step outside `modules/rhi.d3d12/CMakeLists.txt:19-30` and its own `scripts/payload.allowlist` entry (currently `:51-52` lists the DXIL DLLs unconditionally). |
| **Vulkan validation layers** | Apache-2.0 | **Required for development, not shipped** | Comes with the SDK. Do not vendor. |
| **imgui_impl_vulkan.{h,cpp}** | MIT (already-vendored ImGui) | **Only for Slice 9** | `third_party/imgui/CMakeLists.txt:2-9` builds only `imgui_impl_win32.cpp` and `imgui_impl_dx12.cpp`; `backends/` contains only those four files. |
| **glslang / shaderc** | BSD-3-Clause / Apache-2.0 | **No** | Permissive, but wrong tool. `glslc`'s HLSL mode cannot handle the SM 6.5 mesh-shader syntax at `RHIShaders.cpp:620-642`, and adopting it means rewriting ~2016 lines of HLSL to GLSL. |
| **SPIRV-Reflect** | Apache-2.0 | **No** | Pipeline layouts are hand-declared in C++; there is nothing to reflect against that is not already known. |
| Unreal's ShaderConductor DXC | **Unreal EULA** | **Excluded** | Exists on this machine (`UE_5.5`, `UE_5.8` under `ThirdParty\ShaderConductor\Win64`) and *is* SPIR-V-capable. Violates the permissive-only rule. Do not use it, including for one-off testing. |

Build-system note: `imgui` has exactly one *direct* consumer, `Aver.RHI.D3D12` (`modules/rhi.d3d12/CMakeLists.txt:14-17`). Also, `CMakeLists.txt:69-71` gates the imgui subdirectory on `AVER_ENABLE_UI` alone, so a `UI=ON, D3D12=OFF` configuration compiles `imgui_impl_dx12.cpp` into a library nothing links.

> **Correction, 2026-08-02.** An earlier draft of this section added "so it does not contaminate
> anyone's link line". That is wrong, and it contradicts [PACKAGING.md](PACKAGING.md). Line 15 is
> `target_link_libraries(Aver.RHI.D3D12 PUBLIC imgui)` and line 16 is
> `target_compile_definitions(Aver.RHI.D3D12 PUBLIC AVER_WITH_IMGUI=1)` — **PUBLIC**, so ImGui and
> that define propagate to *everything* linking the D3D12 backend. It looks uncontaminating today
> only because the sole consumer is the editor. The moment a game executable links
> `Aver.RHI.D3D12` it inherits ImGui, which is exactly why packaging needs either a second build
> tree configured `-DAVER_ENABLE_UI=OFF` or the ImGui/RHI split (PACKAGING.md slice 4).

---

## The oracle problem

**The existing 18-gate oracle does not port, and it is not a matter of loosening a number.**

`scripts/gates.ps1:351` is a PowerShell **string inequality**: `elseif ($r.raw -ne $want) { $verdict = "FAIL expected $want" }`. `$r.raw` is the whitespace-stripped triple captured at `:285`; `$want` is the third pipe-field of the baseline (`:266`). The engine produces those codes as `(int)(px*255.0f + 0.5f)` (`sandbox/src/SandboxApp.cpp:4895-4897`). Both baseline files contain only 3-field rows, `Read-Baseline` (`:263-268`) reads only `$f[0..2]`, and a 4th column would be **silently ignored** — so there is no tolerance column and no code path that would read one. A first-run mismatch is retried once (`:378-409`) and promoted to `FLAKY` if the retry matches exactly, which keeps it out of `$failures` (`:408-409`) — still bit-exact acceptance.

A second backend also cannot *reach* the pixel comparison: `NO-TOTALS` (`:347`) is evaluated before the value branch, and only `D3D12Device.cpp:1345` produces that line.

### The oracle is red at HEAD, for reasons unrelated to Vulkan

`gates.ps1` defines 18 gates (`:123-131`, `:199-206`, `:231`) against 9 configs (`:238-248`) = 162 expected rows. Both baselines hold 153 = 17 × 9. Commit `ae47a2f` added `rt-penumbra` (`gates.ps1:231`) after the last record, so exactly those 9 rows are missing and each scores `NO-BASELINE` (`:350`), counted as a failure (`:408`), returned by `exit $failures` (`:510`).

The Debug tree is redder still, from a second cause: `gates.baseline.txt` was last recorded at `79f6f04`, before `119d420`'s reflection changes; only `gates.baseline.release.txt` was re-recorded (`2d4e1a1`). The two files, recorded bit-identical at `b26700e`, now disagree on 15 rows — `gates.baseline.txt:53,54,57` hold `94,27,14` / `94,27,14` / `87,20,11` where `gates.baseline.release.txt:30` holds `99,31,17`, across the 5 RT-capable configs.

Expected exit codes on a clean D3D12 run: **~24 Debug, ~9 Release.** *(Derived from the code path and recorded values; the suite was not run.)* **Any Vulkan work must not read this as its own regression, and the oracle needs a `-Record` pass covering both trees before it can serve as a before-picture. Only the user can run that.**

### Two of the nine configs are empirically inert

`no-ms` and `tier1-no-typed-uav` produce 0 of 17 differing gates in both trees. For `tier1`/`no-typed-uav` the reason is traced: neither `resourceBindingTier` nor `typedUavLoads` is read anywhere to change behaviour.

### Three of the nine configs have no Vulkan meaning at all

- `warp` (`:247`) is not a second backend — `useWarp` is consumed inside `D3D12Device.cpp:1243-1245` as `EnumWarpAdapter`. It is a different Microsoft *driver* for the same `D3D12Device.cpp`. It also has a live behavioural branch: `softwareAdapter_` disables conservative raster on mesh PSOs (`:3723-3730`), and the recorded pixels prove it — warp's `ms-gi`/`ms-rt-gi` (`gates.baseline.release.txt:169-170`) equal the `no-cons-raster` values (`:118-119`), not baseline (`:33-34`).
- `no-dxc` (`:246`) clamps a DirectX-shader-compiler capability (`RHI.cpp:128`).
- `tier1-no-typed-uav` clamps `D3D12_RESOURCE_BINDING_TIER` (`RHI.hpp:89`).

### What the correct strategy is

**Two layers, and they answer different questions.**

**Layer A — per-backend self-regression (bit-exact, cheap, already supported).** Baselines are keyed `config/gate` (`:266`, `:337`, `:411`), so a Vulkan run enters as its own config rows and is never compared against D3D12 codes. Add a `-Backend` switch that moves `$Exe` and `$BaselineFile` together exactly as `-Release` already does (`:53-54`), and record `gates.baseline.vulkan.txt` / `gates.baseline.vulkan.release.txt`. This catches "Vulkan changed since yesterday" and nothing more.

Do **not** attempt this by adding a 4th baseline column. `Read-Baseline` accepts `$f.Count -ge 3` and would key the row on the wrong fields; every gate would then report `NO-BASELINE` (not a value mismatch), and the `-Record` filter at `:495` would *keep* the stale 4-column rows while appending the new 3-column ones.

**Layer B — cross-backend agreement (relational and analytic, with tolerance).** This is what actually answers "does Vulkan render the same scene correctly". The repo already contains both forms of it:

- **Relational, already written:** the three `sandbox/src/*Test.cpp` suites are relational with a shared `kMinDelta = 0.05f`. They assert things like "the shadowed probe is darker than the lit probe by at least 0.05". These **port for free** the moment capture works on Vulkan.
- **Analytic, already written and baseline-free:** `modules/render.pt/src/PtFurnaceTest.cpp` asserts against analytic values with a relative tolerance (1e-4 / 10%) and needs no recorded baseline at all. It also already refuses to report a wrong number when the input is absent — `:197-204` detects `furnaceRadiance == 0` and reports INCONCLUSIVE. This is the model to copy.
- **The invariant layer already exists inside `gates.ps1`:** `:417-458` computes luminance (`Lum`, `:432`) and applies two numeric thresholds — `:443` `$d -lt 20`, `:453` `$diff -lt 8` — and it already runs `foreach ($c in $selected)` (`:436`), so a new config picks it up automatically. Extend it; do not invent it. (Add `vulkan` to the `:451` exclusion list if RT is not yet available there.)

**A cross-backend oracle should assert ordering and magnitude, not equality.** Concretely: shadowed < lit by ≥ some margin; GI-on brighter than GI-off in the bounce region; the RT penumbra wider at 8° sun angle than at 0.5°; the furnace test's albedo≈radiance within 10%; a skinned character's BLAS rebuild count non-zero across frames.

**Why a tolerance is not a cop-out here.** Bit-exactness on this repo measures floating-point codegen determinism, not correctness — the repo already concedes this for Debug vs Release, where `gates.baseline.release.txt:3-7` states that a shared baseline would have to "tolerate that (and stop being bit-exact)". Whole frames already wobble by ~2400 differing bytes run to run; what is bit-stable is specifically a flat-neighbourhood 7×7 probe, over 25 runs. Between two independent rasterisers with different sample positions, different filtering precision, and different FMA contraction, bit-equality is not merely hard — it is physically unavailable, so demanding it is demanding a test that can only fail. A relational assertion is a *stronger* statement about correctness than a frozen triple, because when it fails you can name what broke; when a triple changes by one LSB you can only name that it changed.

---

## Staged plan

Sizes are rough and should be read as order-of-magnitude.

### Slice 1 — Truth in reporting (no SDK required)
**~200–400 lines. 1–2 sessions.**

- `gates.ps1` / `record-gates.ps1`: add `-Backend`, pass it to the app, parse `[Sandbox] backend={} adapter='{}'` (`SandboxApp.cpp:507`) in `Invoke-Gate`, and fail on backend or adapter mismatch. Pair `$Exe`/`$BaselineFile`/`$Backend` the way `-Release` already pairs at `:53-54`.
- Fix `Engine.cpp:54-57` so the fallback list is `{requested}` + the remaining default order, with no duplicate and no dropped backend.
- Add `--backend-strict`: a fall-through past the requested backend becomes a non-zero exit instead of `Engine.cpp:114`'s unconditional 0.
- Add a configure-time `message()` in `modules/rhi.vulkan/CMakeLists.txt` stating the module is a stub. Do **not** add `find_package(Vulkan REQUIRED)` yet — see "What I would NOT do", item 12.
- Correct the stale docs enumerated in §"The honest starting position", plus `VoxiRenderer.hpp:140-148`.

**DONE-WHEN:** `gates.ps1 -Backend d3d12` runs with the assertion active and its failure count is unchanged from before the change; `Sandbox.exe --backend vulkan --backend-strict` exits non-zero and names both the requested and the obtained backend; `--backend vulkan` without `--backend-strict` produces a gate verdict that is a hard failure rather than a pass; `--warp` on a machine without WARP fails the gate instead of recording hardware numbers; `grep -rn VULKAN_SDK docs/` returns nothing claiming a build gate.

### Slice 2 — Conformance harness for `IDevice` (no SDK required)
**~300–500 lines. 1 session.**

The type system will not tell you a backend is half-implemented; `selfTest()` (`RHI.hpp:271`) has no call site outside D3D12 and `Engine.cpp:64-77` validates nothing. Build the check that does not exist.

- A headless `--rhi-conformance` mode that reports, per method, whether the device overrides it or inherits the default. (A vtable-slot comparison against a base-class instance is the mechanical route; a per-method probe with known-safe arguments is the honest one.)
- Cross-checks that catch the specific silent traps: a device reporting `rayTracingTier >= 11` that returns false from `meshGeometry()`; a device implementing `createSkinTargetMesh` but returning 0 from `meshVertexBuffer()`; `camera()` returning false while shadow cascades are enabled; `IRenderFeature` hooks that are never invoked over a frame.

**DONE-WHEN:** the harness prints a per-method table and exits non-zero on any inconsistency; D3D12 is all-green; `NullDevice` prints exactly 5 implemented / 47 defaulted with no crash and exit 0 (a GPU-less device is a valid device); and the `meshVertexBuffer`-without-`camera` style cross-checks are demonstrated to fire against a deliberately-crippled test device.

### Slice 3 — Shared-RHI changes (no SDK required)
**~400–700 lines. 2–3 sessions.**

Everything in §"What must change in the SHARED RHI", items 1(remainder), 3, 5, 6, 7, 8. Specifically:

- Capability predicates; migrate `PathTracer.cpp:47-48`, `Voxi.cpp:74`, `:81`, `VoxiRenderer.cpp:968`, `:1065-1066`.
- The validation-summary virtual; reroute `D3D12Device.cpp:1345` through it; drop `gates.ps1`'s dependence on D3D12-specific text.
- Move the b5 triangle count out of push-constant space.
- Resolve the `Common` contract contradiction across `RHIResources.hpp:66`, `SkinningPass.hpp:74-78`, `PathTracer.cpp:199-204`, `VoxiRenderer.cpp:669-676`.
- Rewrite the D3D12-mechanism comments as contracts.
- Optional native window handle on `DeviceDesc`.
- Decide and record the Vulkan→tier mapping for `voxi_abi.h`, or add a version function there.

**DONE-WHEN:** a `-Record` of both trees followed by a clean run gives 0 failures on D3D12; `grep -rn 'rayTracingTier\|shaderModel\|dxcAvailable' modules/render.*` returns no *decision* sites, only reporting; b1 + b5 totals 128 bytes on the mesh path; the conformance harness from Slice 2 still reports D3D12 all-green.

### Slice 4 — SDK acquisition and the build seam (SDK required)
**~50–150 lines of CMake + ~300 lines of device bring-up. 1 session.**

- Install the LunarG SDK. Run `dxc -T ps_6_0 -spirv` on a trivial shader against the Windows-SDK DLL and record the result in this file, replacing §"Can the Windows-SDK dxcompiler.dll emit SPIR-V?" with a fact.
- `find_package(Vulkan REQUIRED)` in `modules/rhi.vulkan/CMakeLists.txt`, plus the `if(TARGET ...)` conversion described in item 12 below.
- Vendor volk + VMA with licence files.
- `VulkanDevice` overriding only the 5 pure virtuals: instance, physical-device selection, queues, swapchain, clear-and-present. `caps()` reports everything off (`rayTracingTier = 0`, `meshShaderTier = 0`).

**DONE-WHEN:** `--backend vulkan --backend-strict` starts, `adapterName()` logs the real physical device, a cleared swapchain presents for 1000 frames, exit code 0; validation layers enabled in Debug report zero errors from startup through shutdown; Slice 2's harness reports 5/52 with no crash; `AVER_RHI_VULKAN=ON` now fails to configure on a machine without the SDK.

### Slice 5 — Shader front end (SDK + SPIR-V DXC required)
**~500–800 lines. 2–3 sessions.**

- A `ShaderCompiler` inside `modules/rhi.vulkan` (the D3D12 one is TU-private at `D3D12Device.cpp:30`/`:60`/`:157` and DXIL-only).
- `-fvk-{b,t,u,s}-shift` computed per `PipelineLayout`, consistent with `meshGeometryDefines`' `declaredSrvCount(layout)`/`+1` (`RHIShaders.cpp:649-653`) and `materialShaderDefines`' table base (`PbrShaders.cpp:278-283`).
- Staging step for the SPIR-V DXC outside `modules/rhi.d3d12`, plus `scripts/payload.allowlist` gating.

**DONE-WHEN:** a `--shader-selftest` run compiles every engine entry point (~31 across 8 corpora) to SPIR-V and reports pass/fail per entry point with zero failures; the `CSHistogram` case (`RHIShaders.cpp:771-790`, b0/t0/s0/u0 in one entry point) creates a pipeline with no validation complaint; the SM 6.5 mesh entry points are *skipped by configuration*, not by failure.

### Slice 6 — Resources, binding, barriers, and a scene (SDK required)
**Largest slice. ~1500–2500 lines. 4–6 sessions.**

`IResourceFactory`'s 22 pure virtuals; descriptor-set layouts derived from `PipelineLayout`; the dynamic-UBO ring with `minUniformBufferOffsetAlignment`; the never-unbound CBV invariant; the deferred rendering-scope scheme; the `ResourceState` → (stage, access, layout) function; and the dozen private transitions the D3D12 backend performs invisibly.

**DONE-WHEN:** the sandbox renders the scene on Vulkan with RT and mesh shaders reported unavailable; validation is clean over 1000 frames including a window resize and an MSAA change (`setSampleCount` recompiles, `D3D12Device.cpp:1425-1434`); the relational sandbox `*Test.cpp` suites (`kMinDelta = 0.05f`) pass unmodified.

### Slice 7 — Capture and the relational oracle (SDK required)
**~300 lines. 1–2 sessions.**

`requestCapture`/`getCapture`/`getFrameImage` — built *on* the working swapchain, command list and fence, exactly as D3D12 does (`:1797-1805`, `:2829-2836`, `:2860-2883`). Then extend `gates.ps1`'s invariant block (`:417-458`) into a backend-portable relational suite and record a Vulkan-only baseline for self-regression.

**DONE-WHEN:** `gates.ps1 -Backend vulkan` asserts the backend, produces a probe value for all 18 gates (no `NO-PROBE`, no `BAD-PROBE`), and the relational assertions pass; `PtFurnaceTest` passes on Vulkan inside its existing 1e-4/10% tolerance with no new baseline; a screenshot via `--frames` is written without stealing focus.

### Slice 8 — Ray query (SDK required)
**~600–1000 lines. 2–3 sessions.**

BLAS/TLAS, `VK_KHR_ray_query`. The load-bearing detail: `buildTlas` compacts the instance array, skipping instances with an invalid BLAS (`D3D12Device.cpp:4617`) or an out-of-24-bit id (`:4627-4631`), advancing the write cursor only on success (`:4635`, feeding `NumDescs` at `:4642`), and `id.InstanceID = instances[i].instanceId` (`:4633`) is the value that survives. **That id must be written into `VkAccelerationStructureInstanceKHR::instanceCustomIndex` identically**, because `PtShaders.hpp:148` reads `CommittedInstanceID` (not `CommittedInstanceIndex`), which is `InstanceCustomIndexKHR` in SPIR-V — get it wrong and every reflection reads its neighbour's geometry.

Implement `meshVertexBuffer` in the same commit as `createSkinTargetMesh`, coupling them the way D3D12 does (`:1959` sets `computeWritten`, `:706` gates on it).

**DONE-WHEN:** the RT gates produce a shadow darker than the unshadowed reference by the same relational margin D3D12 produces; the furnace test still passes; a skinned character's per-frame BLAS rebuild line (`VoxiRenderer.cpp:455-461`) reports a non-zero "rebuilt" count and its RT shadow tracks the animation.

### Slice 9 — Editor UI on Vulkan (optional, SDK required)
**~400 lines plus vendoring. 1–2 sessions.**

Move `AVER_WITH_IMGUI` out of `modules/rhi.d3d12/CMakeLists.txt:16` to a backend-neutral owner; vendor `imgui_impl_vulkan`; extract the ImGui lifecycle from `D3D12Device.cpp` (`:2963` CreateContext, `:2969` Win32 init, `:2979` DX12 init, `:2999` NewFrame, `:2820` Render, `:3005-3012` shutdown, `:3034-3035` uiTextureId) behind a per-backend hook.

**DONE-WHEN:** the sandbox compiles and links with `AVER_RHI_D3D12=OFF` (today it does not — `SandboxApp.cpp:1530` uses `uiReg_` outside any guard while the member `:5335-5337` and its class `UiRegistry.hpp:4-81` exist only under the macro), and the editor renders on Vulkan.

**Explicitly deferred, possibly forever: mesh shaders.** Report `meshShaderTier = 0`. The entire path is gated at `D3D12Device.cpp:1662`, `Voxi.cpp:52-55` and `:80-83`, `VoxiRenderer.cpp:968`/`:1065`, defaults off (`Voxi.hpp:54`), the HLSL sits behind `#if AVER_MS` (`RHIShaders.cpp:597`/`:643`) which only D3D12 and VoxiRenderer ever define, and the input-assembler fallback is live (`D3D12Device.cpp:2097-2110`, `VoxiRenderer.cpp:849-850`). A Vulkan backend reporting tier 0 renders correctly and never compiles the block.

---

## What I would NOT do

1. **Would not start by writing Vulkan code.** Slices 1–3 need no SDK, and each one prevents rework that would otherwise be discovered halfway through Slice 6.

2. **Would not treat flipping `AVER_RHI_VULKAN=ON` as a milestone.** It changes the build (adds the subdirectory `CMakeLists.txt:122-123`, defines `AVER_HAS_VULKAN=1` PUBLIC on `Aver.RHI` at `modules/rhi/CMakeLists.txt:20-22`, links a stub at `modules/runtime/CMakeLists.txt:18-19`) and changes *nothing* observable at runtime — in a default run the preference order returns on D3D12 at `RHI.cpp:88` and the stub's trace at `VulkanDevice.cpp:9` never even fires.

3. **Would not add a `vulkan` config row to `gates.ps1` before the backend assertion of Slice 1 lands.** A fall-through would record clean D3D12 numbers under the `vulkan` key and produce a permanently green configuration that never touched Vulkan. This is the single most expensive mistake available here, because it is silent and self-reinforcing.

4. **Would not try to make Vulkan match the bit-exact baseline, and would not add a tolerance column to it.** `Read-Baseline` (`:263-268`) reads only fields 0–2; a 4th field is silently ignored and mis-keys the row (see §"The oracle problem"). Tolerance belongs in the relational layer, not in a widened baseline.

5. **Would not port the shader corpus to GLSL.** ~2016 lines, ~31 entry points, seven modules — and the per-layout register-rebasing machinery already exists and works (`meshGeometryDefines` `RHIShaders.cpp:649-653`; `materialShaderDefines` `PbrShaders.cpp:278-283`). `-fvk-*-shift` is a strictly smaller change with a strictly smaller blast radius.

6. **Would not use the Unreal-shipped SPIR-V DXC**, including "just to unblock testing". It works; it is licensed under the Unreal EULA; the project's rule is permissive-only.

7. **Would not redesign `PipelineLayout` into a Vulkan-shaped descriptor-set struct up front.** No GPU address crosses the RHI boundary — `setConstantBuffer` takes CPU bytes (`RHIResources.hpp:437`) and the `D3D12_GPU_VIRTUAL_ADDRESS` is manufactured and consumed inside `D3D12Device.cpp:4439-4442`. Root-CBV slots map to dynamic UBOs behind the existing signature. Fix the comments in Slice 3; change the struct only if a real Vulkan implementation proves it cannot be honoured.

8. **Would not implement mesh shaders.** See above. Reporting `meshShaderTier = 0` is a supported, tested configuration.

9. **Would not implement capture "first, before rendering work".** The D3D12 implementation derives from an existing swapchain backbuffer, command list, barriers and fence (`D3D12Device.cpp:1797-1805`, `:2853-2863`). Capture is the first thing possible *after* a frame presents, not a prerequisite to one.

10. **Would not treat the current red gate run as a Vulkan regression, or as a signal at all** until a `-Record` has been done on both trees. `rt-penumbra` has no baseline row and the Debug baseline is additionally stale on 15 reflection rows. Only the user can run the re-record.

11. **Would not remove D3D12, make Vulkan the default, or reorder the preference list to favour Vulkan.** D3D12 is the reference implementation and the only thing the 18 gates describe.

12. **Would not add `find_package(Vulkan REQUIRED)` to `modules/rhi.vulkan/CMakeLists.txt` on its own as the "make ON mean something" fix.** The forced-off consistency pattern the repo uses for modules (`CMakeLists.txt:133-136`, `:148-151`) relies on `set(VAR OFF)` shadowing the cache for subdirectories added *after* that line — but backends do not use the `if(TARGET ...)` half of that pattern (`grep 'if(TARGET Aver.RHI'` returns nothing repo-wide; both consumers re-test the option variable at `modules/rhi/CMakeLists.txt:14,17,20` and `modules/runtime/CMakeLists.txt:12,15,18`). Since `add_subdirectory(modules/rhi)` is root line 108 and the forcing rules sit at 133 and 148, a naively-placed Vulkan rule would leave `AVER_HAS_VULKAN=1` defined while skipping the link, producing an unresolved external at `RHI.cpp:54-55`. Convert the two backend consumers to `if(TARGET ...)` first, or hoist the rule above line 108.

13. **Would not "finish" D3D11 as a warm-up.** It is a 13-line stub that is ON by default (`CMakeLists.txt:29`) and advertised in the fallback list. Either implement it deliberately or make the build stop claiming it exists — but do not let a second dead backend accumulate while adding a third.

14. **Would not rely on any comment in `docs/rendering/RENDERING.md` about shaders, PSO caching, frame graphs, or the Vulkan build gate.** Every one of those checked in this pass was stale. `docs/ARCHITECTURE.md:592-596` is the exception and is accurate.