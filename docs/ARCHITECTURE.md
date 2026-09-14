# Aver Engine — Modular Architecture Specification

**Target:** `C:/Users/User/Documents/Aver Engine` (greenfield). **Mandate:** replace Unreal for the OpenConstructor soft-body destructible racing sim using only MIT/BSD/zlib/Apache-2.0/public-domain code, with UE5+Source2-class rendering, and a strict *pay-for-what-you-use* module DAG.

Symbol/naming convention adopted: C++ types `Av*` (e.g. `AvVec3`), C ABI functions `aver_*`, C ABI structs `Av*` POD, build targets `Aver.*`, native format extensions `.oc*` (carried) / new `.oc*` (added). Copyright header: `Aver Engine`.

**Plan versus tree, and how to tell them apart here.** This document began as a design and was for a
long time read as a description, which is how it came to name modules that have never existed.
`Aver.Render` and `Aver.Render.GI` are the clearest cases — both are cited throughout §8 as the home
of the rendering stack, and both are a `README.md` in an otherwise empty directory. So is the whole
Rust tool row, and so was `Aver.ABI`, which is not merely unbuilt but **deliberately dropped**
(`modules/abi/README.md`; `docs/ABI.md`). Everything below has been re-derived from `modules/*` and
the top-level `CMakeLists.txt`, and a module named **‹skeleton›** carries a README and nothing else:
no header, no source, no `CMakeLists.txt`, and no `add_subdirectory` line in the build. Anything not
so marked is a real target you can find by name in the generated solution.

---

## 1. Design principles (the anti-bloat contract)

| # | Principle | How it is enforced structurally |
|---|---|---|
| P1 | **Core knows nothing engine-specific.** | `Aver.Core` depends only on the C++ standard library + a math header. It has zero knowledge of RHI, render, physics, scene, ECS, or assets. Every arrow in the DAG points *toward* core, never away. |
| P2 | **No universal base object / no runtime reflection tax.** | There is no `UObject`. Entities are 32-bit handles; components are POD in packed arrays (data-oriented). Reflection is *offline codegen* used by the serializer/editor bridge only — it costs nothing at runtime and nothing for objects that don't opt in. |
| P3 | **Editor is a separate process that drives a headless runtime through the C ABI.** | The shipping runtime links **no** editor code. The editor (C#) and tools (Rust) are downstream consumers of the C ABI; the arrow points editor→runtime, never runtime→editor. |
| P4 | **Every subsystem is an opt-in module.** | The engine boots with an empty module registry. Physics, render, net, audio, softbody, etc. each self-register only if compiled in *and* enabled. No mandatory subsystems. |
| P5 | **Build only what you ship.** | Each module is an independent CMake target behind a `AVER_MODULE_*` option. A "cage-viewer" tool and a "full multiplayer client" are two different link sets from the same tree. |
| P6 | **The cook is a plain content-addressed file cache, not a service.** | Offline Rust compilers turn source → versioned binary asset; the cache key is `hash(source ⊕ compilerVersion ⊕ options)`. No monolithic DDC daemon, no network DDC. |
| P7 | **One RHI, swappable backends.** | Renderer and all GPU code target `Aver.RHI` only. DX12/DX11/Vulkan are backend plugins selected at runtime from the compiled-in set, via `--backend <name>` or `BootConfig::backend`. **Until `ca1ab59` this was aspiration, not fact:** `DeviceDesc::preferred` existed but nothing ever wrote it, so the backend was decided entirely by which ones were compiled in. A request now falls back if unavailable and **warns that it did**, because a run that silently used a different backend than the one asked for is worse than one that refused to start. |

These are the contract, not a report on it. P1, P2, P5 and P7 are held today and held by link lines
rather than by review. P4 is half-held and P6 has nothing behind it; **P3 is not held at all** — the
editor is a C++ application that links the modules directly. §6.4 gives each one a verdict and says
what it rests on, and it is worth reading before quoting a principle as though it were a property.

---

## 2. Layered dependency DAG (ASCII)

Arrows point to dependencies (A ──▶ B means "A depends on B"). No edge points up a tier. `[opt]` = optional pay-for-use module; everything else is core-tier (but only `Core`/`Platform` are *mandatory* to boot).

```
 LANGUAGE LEGEND:  (C++)  (C ABI)  [C#]  {Rust}
 STATUS  LEGEND:   ‹Name› = SKELETON. A README, no source, no target, not in the build.

 ── TIER 7: OTHER-LANGUAGE CONSUMERS (out of the runtime binary) ─────────────────────
    [Aver.Scripting] [Aver.Scene] [Aver.Framework] [Aver.UI] [Aver.Materials]
    [Aver.MaterialCompiler — avermatc, reflects an assembly and writes .ocmat]
                 │  P/Invoke                     {aver-assetc}(C++, not Rust — built, see §3) and
                 ▼                                 ‹{aver-ocbeamc / aver-aerobake / aver-mapc /
 ═════════════════ THE C ABI SEAMS ═════════════   aver-shaderc}› — no Rust in the tree
   ONE PER MODULE. THERE IS NO `Aver.ABI`, AND THERE IS NOT GOING TO BE.
   scene_abi.h  framework_abi.h  physics_abi.h  pbr_abi.h  voxi_abi.h  ui_abi.h
   audio_abi.h  settings_abi.h ── eight DLLs, each with its own export macro; only scene and
                framework carry a version. (This used to read "seven"; Aver.Settings and its
                seam are new since.) Physics also spreads across four more headers on the same
                DLL — physics_character_abi.h, physics_joints_abi.h, physics_layers_abi.h,
                physics_shapes_abi.h — which is still one seam, not five.
   scripting_abi.h ── the odd one out: STATIC, exports nothing, hands the managed
                 ▲   bridge a function-pointer table instead. See docs/ABI.md
                 │   each seam is implemented by the module below it
 ── TIER 6: COMPOSITION ─────────────────────────────────────────────────────────────
   Sandbox (sandbox/)  ── the real composition root: owns the frame loop and names every
                          optional module on its own link line, behind `if(TARGET …)`
   (Aver.Runtime)      ── window + device bring-up and the run loop. NOT an aggregator:
                          it links Core, Platform, RHI and the compiled-in backends only
                 ▼
 ── TIER 5: FEATURES — own GPU resources, drive the GENERIC RHI, name no backend ─────
 (Aver.Render.Voxi.Renderer)[opt,ON]  (Aver.Render.UI)  (Aver.Render.ActorPreview)
 (Aver.Render.PBR.Materials)[opt,ON]  (Aver.Assets.Gpu)
 ‹Aver.Render›  ‹Aver.Render.GI›  ‹Aver.GpuDeform›
 (Aver.World) — no longer a skeleton; it has a `CMakeLists.txt` now. Left out of this ASCII tier
 picture rather than mis-placed in it — see its §3 row for what it actually links.
                 ▼
 ── TIER 4: GAMEPLAY, CONTENT AND THE C SEAMS ───────────────────────────────────────
 (Aver.Framework)[opt,ON] (Aver.Scene)[opt,ON] (Aver.Formats.Material) (Aver.Formats.Audio)
 (Aver.UI.Abi)  (Aver.Audio.Abi)  ── a SHARED C seam over the module below it, nothing else
 (Aver.Anim)  ── .ocanim -> pose; Core+Formats, so it is arithmetic and needs no device
 (Aver.Deform)[opt,ON] ── sub-bone cage over a pose. The CPU parity half of ‹Aver.GpuDeform›
 ‹Aver.SoftBody› ‹Aver.Aero› ‹Aver.Fracture› ‹Aver.Vehicle› ‹Aver.Net› ‹Aver.NetVehicle›
 ‹Aver.Match›
                 ▼
 ── TIER 3: BACKENDS & FORMAT RUNTIME — the only code that knows real hardware exists ─
 (Aver.RHI.D3D12)[opt,ON] (Aver.RHI.D3D11)[opt,ON] (Aver.RHI.Vulkan)[opt,OFF]
 (Aver.Audio.Wasapi)[Windows only]  ── the sound card, and the only file that knows of one
 (Aver.Formats)  ── runtime loaders: native binary + legacy .oc* text fallback
                 ▼
 ── TIER 2: DEVICE-FREE SUBSYSTEM CORES — testable with no GPU and no device ─────────
 (Aver.RHI)  (Aver.Assets)  (Aver.UI)  (Aver.Audio)  (Aver.Physics)[opt,ON]
 (Aver.Render.PBR)[opt,ON]  (Aver.Render.Voxi)[opt,ON]  (Aver.Scripting.Host)[opt,ON]
                 ▼
 ── TIER 1: OS ──────────────────────────────────────────────────────────────────────
 (Aver.Platform)  ── window, events, files, a directory watcher, image decode, splash
                 ▼
 ── TIER 0: FOUNDATION (depends on NOTHING engine-specific) ──────────────────────────
 (Aver.Core)  ── types, math, fnv1a64, logging, assert, time, version.
                 No arenas, no slot map, no job system, no interning — see section 3.
```

Resolved edge list, transcribed from each module's `CMakeLists.txt` rather than from the picture.
Where the two disagree the list is right — a tier is a reading aid, a link line is the contract.

```
Core                 -> (nothing)
Platform             -> Core                                (+ user32 gdi32 shell32 ole32)
RHI                  -> Core, Platform
Assets               -> Core, Platform
UI                   -> Core                                (STATIC. No RHI — see below)
Audio                -> Core                                (STATIC. No device — see below)
Physics              -> Core PUBLIC, Jolt PRIVATE           (SHARED: Jolt is not on the seam)
Render.PBR           -> Core                                (SHARED: no RHI on the P/Invoke boundary)
Render.Voxi          -> Core                                (SHARED: settings + C ABI, same rule)
Scripting.Host       -> Core, Platform                      (deliberately NOT RHI)
RHI.D3D12            -> RHI            (+ d3d12 dxgi dxguid d3dcompiler; imgui when AVER_ENABLE_UI)
RHI.D3D11            -> RHI
RHI.Vulkan           -> RHI                                 (compiled out; no SDK)
Audio.Wasapi         -> Core, Audio                         (+ avrt, ole32. WIN32 only)
Formats              -> Core, Platform, Assets
Scene                -> Core, Assets                        (SHARED: entities + C ABI, no RHI)
Framework            -> Core, Assets, Scene                 (SHARED: gameplay vocabulary; no RHI)
Formats.Material     -> Formats, Render.PBR                 (guarded on AVER_MODULE_PBR)
Formats.Audio        -> Formats, Audio     (+ mfplat mfreadwrite mfuuid ole32 on WIN32)
UI.Abi               -> Core, UI                     PRIVATE (SHARED)
Audio.Abi            -> Core, Audio, Audio.Wasapi, Formats.Audio  PRIVATE (SHARED, WIN32)
Render.UI            -> Core, UI, RHI                       (NEVER RHI.D3D12)
Render.PBR.Materials -> Core, RHI, Render.PBR               (NEVER RHI.D3D12)
Render.Voxi.Renderer -> Core, RHI, Render.Voxi, Render.PBR.Materials   (NEVER RHI.D3D12)
Render.ActorPreview  -> Core, RHI, Formats, Formats.Material          (NEVER RHI.D3D12)
Assets.Gpu           -> Core, Formats, RHI
Runtime              -> Core, Platform, RHI (+ each compiled-in backend, so the factory links)
Sandbox (exe)        -> Runtime, Formats, UI, Render.UI, UI.Abi, Render.ActorPreview,
                        and every optional module behind its own `if(TARGET …)`
```

No target appears in its own transitive closure → the graph is a DAG. `Core` is a sink. No RHI
backend appears on any feature's line. Three edges run sideways inside a tier rather than down one —
`Framework → Scene`, `Audio.Abi → Formats.Audio`, `Render.Voxi.Renderer → Render.PBR.Materials` —
and none points up.

Two things the list says that are worth saying in words. `Aver.Audio.Abi` is built but **nothing
links it**: `sandbox/CMakeLists.txt` names no audio target at all, so the editor is silent and the
seam is exercised only by its own tests. And `Aver.Runtime` is not the aggregator the earlier
revision of this diagram described; `sandbox/` is where the optional modules are actually composed.

**Scene and Framework, and the tree's first SHARED-links-SHARED edge.** `Aver.Framework` is a
shared library that links another shared library, which nothing else here does. The rule that shape
is measured against is the one `modules/render.pbr/CMakeLists.txt` states — no RHI type may sit
behind a P/Invoke DLL — and the transitive closure `{Core, Assets, Scene}` satisfies it, so the
separation is bought without giving up the property the rule protects. Collapsing the two into one
DLL would satisfy a narrower reading of "a SHARED module depends on Core only" and destroy the thing
the split exists for, which is that `scene_abi.h` can be read end to end without meeting the words
*actor*, *pawn*, *spawn* or *possess*.

The edge is verified rather than assumed, and the first attempt did **not** have it: the framework's
only reference to the scene was a header constant, so the linker emitted no import and
`dumpbin /dependents Aver.Framework.dll` listed no edge at all despite a green build and two DLLs on
disk. It became real only once a function genuinely called across. `aver_fw_scene_abi_matches()` is
that call and is documented to stay that way.

**Render features and P7.** `Aver.Render.Voxi.Renderer` is the first module built against the
generic render-feature surface (`modules/rhi/include/aver/rhi/RHIResources.hpp`:
`IResourceFactory`, `IRenderContext`, `IRenderFeature`). It owns real GPU resources — a shadow map,
a radiance volume, acceleration structures, a dozen pipelines — and names no backend type anywhere.
The rule is enforced by the LINK LINE rather than by review: it links `Aver.RHI` and never
`Aver.RHI.D3D12`. Correspondingly, no feature's vocabulary appears in the RHI headers; the backend
does not know what a voxel or a shadow map is. `docs/STATUS.md` §4c-2 records how the code got
there and what it cost.

`Aver.Render.UI` and `Aver.Render.ActorPreview` are the second and third modules built that way, and
each is guarded by the same link line. The actor preview is worth naming separately because it owns
a colour target and a depth target of its own and draws into them rather than into the frame, and
because of the register its camera is published at: `b4`, not `b0`. The backend rebinds slot 0 to the
engine's per-frame block on every `setPipeline`, so a camera published at `b0` is overwritten before
the draw lands and the preview silently renders through the previous frame's camera — a failure that
produces a picture rather than an error (`modules/render.actorpreview/src/ActorPreview.cpp`, and
`docs/ACTOR_EDITOR.md` §3). It also carries its own mesh registry (`PreviewMeshCache`), because the
alternative was reaching into the level editor's private table, which is exactly the seam an asset
editor must not cross.

**Core-only, and what the discipline is for.** `Aver.UI` and `Aver.Audio` link `Aver.Core` and
nothing else. In both cases that is the point of the module rather than an accident of what it
happened to need, and in both cases the split is drawn in the same place: the module decides, and a
second module downstream of it touches the hardware.

- `Aver.UI` produces a `UiDrawList` — vertices, indices, a texture id and a clip rect, partitioned
  into named layers — and `Aver.Render.UI` turns one into draw calls. Every decision a UI can get
  wrong is on the near side of that line: nested clips compose by intersection, adjacent commands
  merge when texture and clip match (a hundred glyphs in a string are one draw, not a hundred), and
  colours are premultiplied on the way in so an additive draw and an alpha draw share one pipeline.
  `tests/ui/src/UiTest.cpp` links `Aver.UI`, `Aver.Core` and `Aver.Platform` and asserts on the
  resulting vertex and command arrays with no device in the process. The renderer's own test needs
  no GPU either: `tests/render.ui/src/UiRenderTest.cpp` implements `IDevice`, `IResourceFactory` and
  `IRenderContext` itself and logs every call — which is only possible because `Aver.Render.UI`
  names no backend, so the test would fail to build if that ever stopped being true.
- `Aver.Audio` is a mixer whose `mix()` fills a buffer the caller supplies; `Aver.Audio.Wasapi` is
  the only file in the stack that knows a sound card exists. Audio needs the split more than the UI
  does, not less: a wrong pan law and a right one are the same waveform to a reader and the same
  silence to a screenshot, so if the mixer cannot be run headlessly and asserted on sample by
  sample it cannot be checked at all. `tests/audio/src/AudioTest.cpp` links `Aver.Audio`,
  `Aver.Core` and `Aver.Platform` and nothing else. `AudioProbe.cpp` beside it is deliberately *not*
  in the headless suite — it needs a real device and it makes a noise.

The same property is why neither module has a build option, which is a real departure from P4.
`modules/ui`, `modules/render.ui`, `modules/ui.abi`, `modules/audio` and
`modules/render.actorpreview` are added unconditionally by the top-level `CMakeLists.txt`. None
depends on anything optional, the generic RHI always exists, and a backend without GPU support
declines at `create()` rather than failing to link — so there is no configuration in which one of
these could fail to build, and an opt-in switch over a Core-only static library would buy nothing
and cost a combination nobody tests. What *is* guarded is the hardware: `modules/audio.wasapi` on
`WIN32`, and `modules/audio.abi` on `WIN32 AND TARGET Aver.Audio.Wasapi AND TARGET
Aver.Formats.Audio`. The device is optional; the mixer is not.

---

## 3. Full module table

Core-tier = compiled by default in a game client; only **Core** and **Platform** are strictly mandatory to instantiate an engine. `[opt]` modules default OFF unless noted.

**‹skeleton› means the module is NOT BUILT** — no header, no source, no `CMakeLists.txt`, and no
`add_subdirectory` in the top-level build. Most such rows still have a directory holding a single
`README.md` describing the eventual responsibility, which is the convention the root
`CMakeLists.txt` documents.

**Two of them no longer have even that.** `Aver.Render` and `Aver.Render.GI` — the two this document
previously leaned on hardest — had their directories deleted, because what they described has since
been built elsewhere under other names and their READMEs still promised "Phase 3". Their rows below
are now the only account of them, which is what the rows already were in substance: each says plainly
what does not exist and names what renders instead. A ‹skeleton› row therefore means "not built", and
says nothing about whether a directory is there. The **Optional?** column describes intent for those rows and
the actual CMake option for the rest; where a built module has no option at all, it says so, and §2
gives the reason. Depends-on for a built module is transcribed from its `CMakeLists.txt`, so it may
be shorter or longer than the plan in the same row's prose.

| Module (target) | Lang | Depends on | Optional? | Purpose | Carries recon subsystem |
|---|---|---|---|---|---|
| **Aver.Core** | C++ | — | Core (mandatory) | What is there: `Types.hpp` (`u8`…`f64`, `AvId` = u32 with 0 invalid), `Math.hpp`, `Hash.hpp`, `Log.hpp`, `Assert.hpp`, `Time.hpp`, `Version.hpp`. **Not there:** arena/pool/stack allocators, handle/slot-map, job system, fibers, string interning; and the only hash is `fnv1a64` — no xxHash, Blake3 or SHA-256. Every one of those was in this row before anyone checked. | `oc::Vec3`/`Vec3.h` upgrade (adds `Dist/DistSquared/GetSafeNormal/Size`) |
| **Aver.Platform** | C++ | Core (+ user32 gdi32 shell32 ole32) | Core (mandatory) | Window and events, filesystem, a directory watcher (the editor's per-frame reload rests on it), image decode, splash. **Not there:** UDP sockets, the dynamic-library loader that §6.1's plugin model assumes, and thread creation — the clock lives in `Aver.Core`, not here. | UE `RawInput` / `FPlatformProcess` replacements (partial) |
| **Aver.RHI** | C++ | Core, Platform | Core | Abstract render hardware interface: device, queues, command lists, PSO, root signature/descriptor model, typed & structured buffers, textures, resource-state/barrier model, `Buffer<float>`/`RWBuffer` semantics, feature-level query. Backend-agnostic. | The "ONE RHI abstraction"; GPU-deform buffer contract (typed R32) |
| **Aver.RHI.D3D12** | C++ | RHI | `[opt, default ON]` | DirectX 12 backend (primary). DXIL PSOs, D3D12 barriers, UAV↔vertex-buffer aliasing. | Primary backend |
| **Aver.RHI.D3D11** | C++ | RHI | `[opt, default ON]` | DirectX 11 backend (fallback for older HW). | Secondary backend |
| **Aver.RHI.Vulkan** | C++ | RHI | `[opt, default OFF]` | **No longer a stub — this row said "a 13-line stub that returns `nullptr`" for a long time and that is now false.** `VulkanDevice.cpp` alone is 3,819 lines (`VulkanResourceFactory.cpp` another 3,000, `VulkanRenderContext.cpp` 1,478), Vulkan headers are vendored and included, `IDevice` is implemented, and SPIR-V compiles through a vendored `dxcompiler.dll` (`third_party/dxc-spirv`) selected because the Windows SDK's own copy accepts `-spirv` and then refuses at codegen. Per `modules/rhi.vulkan/README.md` it **presents a frame** — grid, cube, shadow, sky, world axes — and the editor's own ImGui UI draws too, since `modules/rhi.vulkan.imgui` was written to mirror `modules/rhi.d3d12.imgui` (both gated on `AVER_ENABLE_UI`, both linked only by Sandbox). 10 of an original 156 validation-layer errors are left, 9 of them the same known gap (`GraphicsPipelineDesc::instanced` and feature-module mesh geometry are unimplemented on this backend) and the 10th not a real error at all. Nothing leaks at teardown. `AVER_RHI_VULKAN` still defaults **OFF** ("staying off until it presents a frame" — the module's own README, written before it did) — so the default build still proves nothing about it, but flipping the flag on now builds and runs rather than linking a stub. `Aver.RHI.D3D11`, unlike Vulkan, genuinely still is a 13-line stub that returns `nullptr` (`modules/rhi.d3d11/src/D3D11Device.cpp`) and is in the DEFAULT build, so the engine still ships one backend — D3D11, not Vulkan any more — that has never executed a GPU command. What a backend must actually implement is tiered — 9 pure virtuals to be a legal device (`resources()` defaults to `nullptr`, so a partial backend is legal and every render feature declines gracefully), 53 to be a complete one. `modules/rhi/src/null/NullDevice.cpp` is the existence proof at 43 lines. | Vulkan (SDK still not required — loader `vulkan-1.dll` ships with the GPU driver; SPIR-V codegen is vendored separately) |
| **Aver.Assets** | C++ | Core, Platform | Core | One source file, `AssetId.cpp` — content ids over `fnv1a64`. **Not there:** the asset registry, typed handles, ref-counting, async streaming, the binary container (that is `Avr1.hpp` in `Aver.Formats`), and the offline-cache reader P6 assumes. A leaf, and much smaller than this row used to imply. | Asset I/O runtime backbone (partial) |
| **Aver.Assets.Gpu** | C++ | Core, Formats, RHI | always built | The decode-to-GPU join (`TextureUpload.cpp`), deliberately a second target so `Aver.Assets` stays a leaf: a tool that only needs asset ids drags in neither a decoder nor the RHI. | Asset I/O (upload path) |
| **Aver.Formats** | C++ | Core, Platform, Assets | always built | Runtime *loaders*: `.ocmesh` (now with the JOINTS/WEIGHTS skin streams its `HasSkin` flag always promised), `.ocskel`/`.ocanim`, `.ocworld`, `.ocmap`, `.ocbeam`, `.ocproject`, glTF import **including skins and animation clips**, JSON, texture decode, and the AVR1 container every binary asset shares. Two sibling targets hang off it (below) for the same reason each: they parse into a type that lives outside this module. | Asset I/O (all carried-over text formats) |
| **Aver.Formats.Material** | C++ | Formats, Render.PBR | `AVER_MODULE_PBR` | `.ocmat` read/write — it parses straight into `pbr::MaterialDesc` rather than a duplicate struct, which is why it cannot live in `Aver.Formats` (that would put the PBR DLL behind every headless tool that wanted a map). Also the two C# source rewriters: `MaterialScript` rewrites a material's `Configure`, `ActorScript` rewrites placements in a `.Designer.cs` generated region. | — |
| **Aver.Formats.Audio** | C++ | Formats, Aver.Audio | built when `Aver.Audio` exists | The `.wav` reader and the `.ocaudio` container, filling an `audio::SoundData` directly. On Windows it also imports mp3/m4a/aac/wma/flac through Media Foundation — the OS already ships a correct MP3 decoder, so none is vendored. | — |
| **Aver.Anim** | C++ | Core, Formats | always built | Sampling `.ocanim` into a pose and posing a skeleton: rest pose, the parent-chain resolve, skinning matrices, linear/step/cubic-Hermite key interpolation, crossfade blending, additive layering, and a player that owns a clip's clock. Core and Formats only, so all of it is decidable with no GPU. **It is the first consumer `.ocskel`/`.ocanim` have ever had** — the containers round-tripped since the format work and nothing read them. It also carries `skinVertices`, the CPU linear-blend reference **whose comment is the contract `Aver.Render.Skin`'s shader implements term for term** — the two are checked against each other on a real device rather than asserted to agree. **Not there:** the dispatch itself, which needs a GPU and so lives one tier up. | animation (the sampling half) |
| **Aver.Deform** | C++ | Core, Formats, Anim | `AVER_MODULE_DEFORM` (ON) | **Sub-bone vertex deformation.** A cage of nodes finer than the skeleton, so two vertices sharing a bone can move differently — a dent, a crease, a bulge, none of which bone skinning can express. Generates a cage by subdividing bone segments (or a padded lattice for a mesh with no skeleton), binds vertices to the four nearest nodes with a falloff that reaches zero at its cutoff, and applies displacements through the CPU/GPU parity contract `blend → mirrorY → ×Gain → clamp-to-MaxD → rest+disp`. CPU only for now: the GPU half is `Aver.GpuDeform` below, still a skeleton. | **GPU deform** (the CPU half of it, and the parity reference) |
| **Aver.Anim.Scene** | C++ | Core, Anim, Scene | `AVER_MODULE_SCENE` | The join: advances every `CAnimator`'s clock, resolves its `CSkeletalMesh` rig through a host-supplied id-to-path callback, and keeps the pose and skinning matrices. A SECOND TARGET for the reason `Aver.Assets.Gpu` is one — `Aver.Scene` may not gain a Formats edge, so turning an opaque asset id into a loaded `.ocskel` cannot live there, and giving the sampler a scene would be worse. Ticked UNCONDITIONALLY, not from the gameplay tick groups, which are gated on PLAYING. | animation (the scene half) |
| **Aver.Render** ‹skeleton› | C++ | *(planned: RHI, Assets, Core)* | **not built** | *Planned:* render graph, render-scene, clustered deferred + forward+ hybrid, virtualized shadows, TAA/FSR, HDR/tonemap/post. **None of this exists.** What renders today is the feature set — `Aver.Render.Voxi.Renderer`, `Aver.Render.UI`, `Aver.Render.ActorPreview`, `Aver.Render.PBR.Materials` — each driving `Aver.RHI` directly with no graph between them. | Rendering stack (unstarted) |
| **Aver.Render.GI** ‹skeleton› | C++ | *(planned: Render)* | **not built** | *Planned:* surfel/probe GI + screen-space GI, optional DXR. **Not built.** The GI that exists is voxel cone tracing inside `Aver.Render.Voxi.Renderer`, which is a different technique in a different module. | Rendering quality (unstarted) |
| **Aver.Render.PBR** | C++ | Core | `AVER_MODULE_PBR` (ON) | The material system: `MaterialDesc`, the material library, and `pbr_abi.h` for C# P/Invoke. SHARED and Core-only, which is the rule the rest of the tree is measured against — no RHI type may sit behind a P/Invoke DLL. | material/texture pipeline |
| **Aver.Render.PBR.Materials** | C++ | Core, RHI, Render.PBR | `AVER_MODULE_PBR` (ON) | The GPU half: an authored material becomes a binding set and a constant block. STATIC, links the generic RHI, names no backend. | material/texture pipeline (GPU) |
| **Aver.Render.Voxi** | C++ | Core | `[opt, default ON]` | Project-wide render quality settings (MSAA / GI / ray tracing / mesh shaders / path tracing) with honest per-feature `Ready`/`NotImplemented`/`Unsupported` reporting from the real device caps. SHARED, plus a plain-C ABI (`aver_voxi_*`) for C# P/Invoke. | Implemented — the first optional module |
| **Aver.Render.Voxi.Renderer** | C++ | Core, RHI, Render.Voxi, Render.PBR.Materials | `AVER_MODULE_VOXI` (ON; forced OFF without PBR) | The GPU half: voxel cone traced GI, a 2048² directional shadow map, DXR 1.1 inline RayQuery sun shadows, and the scene lit pipelines that combine them. A `rhi::IRenderFeature`, so it drives the generic RHI and links no backend. | Implemented |
| **Aver.Scene** | C++ | Core, Assets | `AVER_MODULE_SCENE` (ON) | Minimal data-oriented entity/component world: transforms, hierarchy, component storage, generic field access by name. **Fifteen** built-in components now (`modules/scene/include/aver/scene/Components.hpp`) — this row said "twelve, the last four being `CSkeletalMesh`/`CAnimator`/`CParticleEmitter`/`CAttachment`" for a long time, and physics authoring landed three more since: `CSoftBody`, `CRigidBody` and `CJoint` are now the last three. SHARED, with `scene_abi.h` for C#. Render/physics-agnostic (P2) still holds as a link-line property despite the new components being physics data — `Aver.Scene`'s `CMakeLists.txt` still depends on only Core and Assets, so a rigid body or a joint is POD here and nothing more, the same way a material is an interned name and nothing more. | world/scene (entity layer) |
| **Aver.Framework** | C++ | Core, Assets, Scene | `AVER_MODULE_FRAMEWORK` (ON, forced OFF without Scene) | The gameplay vocabulary over the world: game instance, game mode, classes, actors, pawns, possession, play lifecycle, input. SHARED, `framework_abi.h`. The one SHARED-links-SHARED edge in the tree; §2 says why it is worth it. | — |
| **Aver.UI** | C++ | Core | always built | The retained **game** UI (not the editor's ImGui): a draw list of layers, batches, intersecting clip rects and premultiplied colour. Renders nothing itself, which is what makes the whole widget system assertable with no GPU. | — |
| **Aver.Render.UI** | C++ | Core, UI, RHI | always built | The GPU half: a `UiDrawList` onto the backbuffer. Links the generic RHI, never a backend. | — |
| **Aver.UI.Abi** | C | Core, UI | always built | SHARED. The plain-C seam for a game's HUD — 11 exports in `ui_abi.h`, no RHI, no device, no window, so the same seam serves a headless tool and the editor. Named apart from the managed `Aver.UI` on purpose: different file names mean `DllImport`'s default probe finds the right binary and no `NativeResolver` is needed. | — |
| **Aver.Physics** | C++ | Core (PUBLIC), Jolt (PRIVATE) | `AVER_MODULE_PHYSICS` (ON) | Rigid-body dynamics + collision (Jolt Physics, MIT) behind `physics_abi.h`: bodies, shapes, character controller, raycast/sweep/overlap, polled contact and trigger events. SHARED, and Jolt is PRIVATE so it is not on the seam. | Rigid chassis integrator; collision layer for `OnHit`/`ReportImpact` |
| **Aver.Audio** | C++ | Core | always built | The mixer, and nothing that touches a device: a fixed voice pool, generational voice handles, 3D pan and distance attenuation, per-slot atomics rather than a command queue, and a `mix()` that must never allocate, lock, block or log. Fills a buffer the caller supplies. | Audio (the mixer half) |
| **Aver.Audio.Wasapi** | C++ | Core, Audio (+ avrt, ole32) | Windows only | The device. WASAPI shared mode, chosen over XAudio2 because XAudio2 would own the mixing, the 3D and the DSP — which would make `Aver.Audio` a wrapper round a thing it could neither test nor port. The render thread runs in the Pro Audio scheduling class. | Audio (the device half) |
| **Aver.Audio.Abi** | C | Core, Audio, Audio.Wasapi, Formats.Audio | Windows, and only when both those targets exist | SHARED. The C seam for audio (`audio_abi.h`), linking the *device* rather than only the mixer, because "play a sound" means nothing until something is driving a sound card. **Nothing in the tree links it yet** — the editor names no audio target at all. | — |
| **Aver.Render.ActorPreview** | C++ | Core, RHI, Formats, Formats.Material | always built | The actor editor's 3D preview: its own colour and depth target, its own pipeline, its own mesh registry, and a camera published at `b4` rather than `b0`. A feature, not a second viewport — nothing about the scene's frame changes. | — |
| **Aver.Render.Skin** | C++ | Core, RHI, Anim, Formats | always built | **GPU linear-blend skinning**, one compute dispatch per skinned instance. Writes posed vertices in the engine's own `rhi::MeshVertex` layout, which is the whole design: a skinned entity gets its own `MeshHandle` whose vertex buffer IS the compute target (`IDevice::createSkinTargetMesh`), so the input assembler, the mesh shader's root SRV and a BLAS build all read it unchanged and **not one line of the draw path knows skinning exists**. The alternative — a substitute stream threaded through `submitDraw`, the feature's draw record and each replay pass — has a missed-site failure mode that looks like a rest-pose shadow beside a posed character. Bone ring and binding sets are **per instance**, because per-pass ones let two instances in one frame overwrite each other's descriptors. It also carries **SkinnedScene**, the join that walks the world and gives every entity with a `CSkeletalMesh` its own posed mesh -- **the first and only reader that component has ever had**. Residency is keyed by the FULL entity handle, generation included, because keying by index alone is the bug `AnimSystem::posed_` shipped with: a recycled slot inherits the previous occupant. A retired record is reused rather than freed, since the RHI has no `destroyMesh`. An entity with no pose is dispatched IDENTITY rather than skipped, which is what makes the bind pose, a failed rig load and a reused buffer all the same code path. **Not there:** posed bounds -- a character is culled and picked against its bind-pose box. | animation (the drawing half) |
| **Aver.Render.PathTracer** | C++ | Core, RHI | always built | A brute-force **path tracer** over DXR 1.1 inline `RayQuery`, and the reason it is here is that it can be CHECKED. Cosine-weighted hemisphere sampling makes the estimator exactly `albedo` -- the `1/PI` of the Lambertian BRDF, the cosine of the rendering equation and the `cos/PI` pdf all cancel -- but the shader spells all three out anyway, because a cancelled constant cannot be REMOVED and a check that has never failed proves nothing. Accumulates into an `RWStructuredBuffer<float4>` rather than a texture: `Format` has no RGBA32F and a structured buffer needs no typed-UAV-load support, so the result is full-precision linear radiance on every device and copies straight to a Readback heap. It owns its own geometry -- meshes and world matrices in, separate acceleration structures out -- so nothing about it needs a world, a draw list or a material system. Ships **`PtFurnaceTest`**, a white-furnace energy oracle read as LINEAR FLOATS (a missing `1/PI` is a global scale, which no ratio and no tonemapped pixel can see), with **two deliberately broken estimators run beside the correct one on every invocation**: they must read `PI*L` and `2L`, and if they do not, the oracle is announced as blind. The environment comes from the engine's own `skyColor()`, so the furnace measures the shipped path and not a rig. **Not there:** next-event estimation, so a delta sun contributes nothing and `furnaceSun` is not applicable to it. | — |
| **Aver.SoftBody** ‹skeleton› | C++ | *(planned: Core)* | **not built** | **Soft body for skeletal meshes is NOT this** — it is Jolt's, through `aver_phys_softbody_*` on `Aver.Physics`'s C ABI (Jolt 5.6 was already vendored and already compiling, and its `Skinned` constraints plus real world collision are the feature). What is left for this module is the half Jolt has no notion of: **plastic** deformation — permanent set, material yield, break/tear. The OpenConstructor cage solver, extracted UE-free: `AvVehicleMaterial/Node/Beam/Panel/Part`, `AvSolveConfig/Result`, `InitSolver`, `SolveStep` (Verlet + Gauss-Seidel PBD, plasticity, break/tear), impact injection (`OCCrushCurve`, tear radius), part detach/repair, active-set + async worker (`std::thread`/`condition_variable`/`atomic`). Exposes C ABI. | **soft-body solver** (whole `VehicleDamage` core) |
| **Aver.Aero** ‹skeleton› | C++ | *(planned: Core)* | **not built** | Aerodynamics: baked-table trilinear sampler (`SampleBaked`, `Bracket`), attitude derivation (`flowDir` inverse, `UnwindDegrees`), force application at CoP, ground-effect model, `Surfaces[]` fallback. Optionally the wind-tunnel re-bake (`AeroSim`) for in-engine baking. | **aerodynamics** (`VehicleAerodynamics` + `AeroSim`) |
| **Aver.GpuDeform** ‹skeleton› | C++ | *(planned: RHI, Render, SoftBody)* | **not built** | Compute cage-skin: per-vertex K-nearest node blend → mirrorY → gain → clamp → rest+disp, dispatched per section, UAV aliased as position vertex stream, tear/hide in-place updates. `VehicleDeform` compute shader ported to HLSL/DXC (DXIL/SPIR-V). **The CPU half of this now exists as `Aver.Deform` above, and is the parity reference the compute port has to reproduce byte for byte** — its operation order is asserted step by step in `tests/deform`, which is what makes "parity" checkable rather than asserted. | **GPU deform** (the compute pipeline; CPU half done) |
| **Aver.Fracture** ‹skeleton› | C++ | *(planned: Core)* | **not built** | Runtime debris: farthest-point cluster split, mass-from-area, launch impulse, secondary-shatter generations, live-debris cap. (Author-time Voronoi bake is a Rust tool, not here.) | **fracture/debris** (`OCDebrisChunk` runtime parts) |
| **Aver.Vehicle** ‹skeleton› | C++ | *(planned: Core, Physics)* | **not built** | Authoritative vehicle sim: powertrain (engine map, gearbox, driveline), tire model (slip/load/friction), suspension, chassis rigid-body coupling. The `oc_sim`/`UOCVehicleMovementComponent` successor. Deterministic; exposes the C ABI (`aver_world_step`, `aver_vehicle_set_input`). | **powertrain**, **tire model** (`OCSimCore` P2 realization) |
| **Aver.Net** ‹skeleton› | C++ | *(planned: Core, Platform)* | **not built** | Modular replication framework: `AvNetPayload`/channel/module/coordinator triad; raw little-endian UDP **protocol (A)** codec (exact MsgType byte layouts), pose paging, damage fragmentation, join-parity gate. Interop-exact with the existing C# OCServer. Exposes C ABI for the headless host. | **networking/replication** (protocol A + channel framework) |
| **Aver.NetVehicle** ‹skeleton› | C++ | *(planned: Net, Vehicle, SoftBody)* | **not built** | Vehicle net modules (Drive/Damage/Config): quantized input, crash-seed replay through the deterministic cage, durable damage end-state snapshots, detach commands. | networking (vehicle-specific channels) |
| **Aver.Match** ‹skeleton› | C++ | *(planned: Core)* | **not built** | Matchmaking core: coordinator **protocol (§6)** codec (CoordMsg byte layouts), deterministic greedy match forming (`oc_match` successor). Links into the client for queue and the C# coordinator host via the same source. | **coordinator/matchmaking** (`OCMatchCore`) |
| **Aver.World** | C++ | Core, Formats, Platform, Render.Pcg (+ Scene and Physics when their targets exist) | always built | **No longer a skeleton — this row said "not built" and that is now false.** `modules/world/` has a real `CMakeLists.txt` and eleven source files. What is here: `LevelInstance::instantiate()` turns parsed `.ocworld` placements into live scene entities and their static bodies, plus a real region/chunk streaming stack (`ChunkPayload`, `ChunkPartition`, `ChunkCodec`, `RegionFile`/`RegionIndex`, `ChunkSource`/`ChunkStreamer`/`ChunkGenerator`, `ScatterPalette`). It is unconditional — both the editor and a shipped game link it, which is the point (`modules/world/README.md`): a tree with `AVER_MODULE_SCENE=OFF` still configures it, `LevelInstance.cpp` just compiles to nothing. **This row said region streaming was "still ahead" for a long time — that stopped being true at slice 6 (`84178f6`).** Per its own README, `docs/CHUNKS.md`'s slices 0–9 are now all done: placement instantiation, the `.avrgn`/`.ocindex` region format, streaming residency, generation-as-you-go and runtime region writes. What is actually still open is slice 10, floating-origin rebasing — no rebasing code exists anywhere in the tree, so a level far from the coordinate origin still simulates unrebased. It deliberately does not link `Aver.Render.PBR` — material resolution stays a host job. | **world/scene** (placement and region streaming done; floating-origin rebasing next) |
| **Aver.Scripting.Host** | C++ | Core, Platform | `AVER_MODULE_SCRIPTING` (ON) | In-process CLR host: `nethost`/`hostfxr` resolved with `LoadLibraryW` at run time, so this builds on a machine with no .NET at all. Deliberately not the RHI. Exports nothing — it hands the managed bridge a function-pointer table at bootstrap (`scripting_abi.h`). | **scripting** (native half) |
| **Aver.Runtime** | C++ | Core, Platform, RHI (+ compiled-in backends) | always built | Window and device bring-up and the run loop. **Not** the module registry and tick scheduler this row used to claim: there is no registry, and the optional modules are composed by `sandbox/`. | — |
| **Aver.ABI** ‹skeleton› | C | — | **dropped, not deferred** | A single flat `extern "C"` seam with an `aver_abi_version()`. It is not coming: one seam has to link everything it exposes, so it could hold none of the properties the separate seams exist for. `modules/abi/README.md` records the decision; `docs/ABI.md` documents the seams that replaced it. | — |
| **Aver.Editor** ‹skeleton› | C# | *(planned: a C ABI)* | **not built** | *Planned:* a C# editor over the ABI, runtime hosted headless. **Not built, and the architecture moved.** The editor that exists is `sandbox/` — a C++ ImGui application that links the modules directly. P3's separation is therefore claimed and not held; it is the largest single gap between this document and the tree. | **editor** |
| **Aver.Scripting / Aver.Scene / Aver.Framework** | C# | the matching native DLL, by P/Invoke | staged with the bridge when `dotnet` is on PATH | The managed contract assemblies under `scripting/csharp/`, compiled by `modules/scripting/CMakeLists.txt` and copied beside the executable. `Aver.Scene` and `Aver.Framework` share a file name with their native halves and each needs a `NativeResolver` because of it — the newer seams are named apart precisely so they do not. | **scripting** (managed half) |
| **Aver.UI / Aver.Materials** | C# | `Aver.UI.Abi` (P/Invoke) · — | referenced by a game's `Scripts.csproj` | The two assemblies a *game* authors against. `Aver.UI` is `Layer`/`Colour`/`Rect`/`Hud`; `Aver.Materials` is `[AverMaterial]`, `MaterialBuilder` and the `.ocmat` emitter. Neither is staged next to the engine the way the bridge is — a new project gets a `ProjectReference` to the `.csproj` in this tree, so they are built by the game's compile. | — |
| **Aver.MaterialCompiler** (`avermatc`) | C# | Aver.Materials | built with the bridge | Reflects a compiled assembly and writes `.ocmat`. C# under `Content/Materials` is the source, `Binaries/Materials/*.ocmat` is the build output, and the engine reads `Binaries` first — so a material is authored in a language with a compiler rather than in a binary blob. Staged to `bin/Tools` and run by Compile C#. | asset I/O (material cook) |
| **aver-ocbeamc / aver-aerobake / aver-mapc / aver-shaderc** ‹skeleton› | Rust | *(planned: standalone + FFI)* | **not built** | *Planned:* `.ocbeam` compile, wind-tunnel bake, `.scene`→`.ocmap`, HLSL→DXIL. **There is still no Rust in this repository**: no `*.rs`, no `Cargo.toml`. Shaders are HLSL files compiled by the backend at run time, not by a `aver-shaderc` cook. | aero, world, shaders (offline work, unstarted) |
| **`aver-assetc`** | **C++** | Core-adjacent: `Aver.Formats`, `Aver.Core`, `Aver.Platform` (+ `Aver.Trifactor`/`Aver.Formats.Audio`/`Aver.Formats.Material`/`Aver.Render.PBR.Materials` when configured in) | always built (`tests/formats/CMakeLists.txt`, under `AVER_BUILD_TESTS`) | **This row used to be grouped with the four Rust tools above as unbuilt — that stopped being true.** `tools/AverAssetC.cpp` is the standalone asset compiler the launcher's "Aver Exchange" feature shells out to: glTF/OBJ/USDA import-and-cook (and, conditionally, audio and material-graph compile), per-mesh output by default, results as JSON-Lines (one object per artifact plus a `{"summary":true,...}` line), built on the same import/merge code `ConvertTool.cpp` proved. It is the job `aver-assetc` was planned to do, done in the language every other tool in `tools/` is written in rather than in Rust. `AverAssetCTest` pins its JSON contract. | asset I/O (import-and-cook, done — in C++, not Rust) |

---

## 4. Every carried-over subsystem → module (traceability matrix)

**This table is an assignment, not a report.** Eight of the twelve owning modules named below are
still ‹skeleton›: SoftBody, Aero, GpuDeform, Fracture, Vehicle, Net, NetVehicle and Match are a
README, and so is every Rust tool. **`Aver.World` is no longer one of them** — it gained a real
`CMakeLists.txt` and eleven source files (§3) — which this paragraph used to get wrong by counting
it among the nine. Of the twelve recon subsystems, the ones with code behind them
today are the entity layer (`Aver.Scene`), world placement (`Aver.World`, partial), the runtime half
of asset I/O (`Aver.Formats` and its two siblings), and scripting — plus audio and the material
pipeline, which the recon did not have and which are new work rather than a port. Read a row as
*where it will go*, and check §3 for
whether it has gone there yet.

| Recon subsystem | Owning module(s) | Notes on the port |
|---|---|---|
| Soft-body solver (`VehicleDamage`) | **Aver.SoftBody** | Keep exact phase order (Verlet → damage/plasticity → Gauss-Seidel), formulas, float eval order for MP determinism. Async worker via `std::thread`. |
| Aerodynamics (`VehicleAerodynamics`) | **Aver.Aero** | Trilinear sampler + attitude inverse must match writer `flowDir`. SI units (drop `NEWTONS_TO_UE`, CoP cm→m). Ride probe via **Aver.Physics** raycast. |
| GPU deform (`VehicleDeform.usf`) | **Aver.GpuDeform** (+ CPU parity in **Aver.Render**) | Byte-for-byte CPU/GPU parity contract preserved; UAV-as-vertex-stream via **Aver.RHI**. |
| Fracture/debris | **Aver.Fracture** (runtime) + a planned author-time Voronoi pre-dice tool | Runtime = clustering split/launch; author-time = pre-fractured shards (no runtime Voronoi). **Not the built `aver-assetc`** — this row used to name that tool here, but the real `tools/AverAssetC.cpp` does glTF/OBJ/USDA/audio/material import-and-cook and nothing about Voronoi fracture; the pre-dice tool remains unbuilt and unnamed. |
| Powertrain | **Aver.Vehicle** (Powertrain subsystem) | Extracted UE-free from `UOCVehicleMovementComponent` (recon P2). |
| Tire model | **Aver.Vehicle** (Tire subsystem) | Same sim core; shared client/server via C ABI. |
| Networking / replication | **Aver.Net** + **Aver.NetVehicle** | Protocol (A) LE UDP kept for OCServer interop; UE `OCNetWire`(B) and RPC(C) dropped. Channel/module/coordinator triad reimplemented in C++. |
| Coordinator / matchmaking | **Aver.Match** | CoordMsg codec + `oc_match` deterministic forming. C# coordinator host stays external, speaks the same wire. |
| World / scene | **Aver.Scene** (ECS) + **Aver.World** (levels, `.ocmap`/`.scene`/`.octrack`) | Object-reference world model (asset name + transform), surface/ground/killz env. `Aver.World` is now built (§3) — placement instantiation and region streaming are both real; floating-origin rebasing (slice 10) is what remains. |
| Asset I/O | **Aver.Assets** + **Aver.Formats** (runtime) + **`aver-assetc`** (offline, built — see §3) + **aver-ocbeamc / aver-aerobake / aver-mapc** (offline, still unbuilt) | Loose text = dev fallback; compiled binary = ship path. |
| Editor | *planned* **Aver.Editor** (C#) — **actually** `sandbox/` (C++) | The plan was a separate app over a C ABI (P3). What was built links the modules directly, so this is the one row where the tree contradicts a design principle rather than merely lagging it. |
| Scripting | **Aver.Scripting.Host** (C++) + `scripting/csharp/` (C#) | .NET P/Invoke, but over the per-module seams rather than one `Aver.ABI`; the host itself exports nothing and hands the bridge a function-pointer table. |
| Audio *(new — not from recon)* | **Aver.Audio** + **Aver.Audio.Wasapi** + **Aver.Audio.Abi** + **Aver.Formats.Audio** | Mixer / device / seam / container. The engine's own mixer rather than a vendored one, for the reason §2 gives: a mixer you cannot assert on sample by sample cannot be checked at all. |
| Game UI *(new — not from recon)* | **Aver.UI** + **Aver.Render.UI** + **Aver.UI.Abi** + `scripting/csharp/Aver.UI` | Draw list / renderer / seam / managed HUD API. Deliberately not Dear ImGui, which stays the editor's: a shipped game should not link an editor toolkit, and an immediate-mode HUD has nothing a designer can open or diff. |

---

## 5. Formats — ownership & fidelity

**Carried over (full fidelity, existing OpenConstructor27 content loads).** Runtime loaders in **Aver.Formats**.

The **Offline compiler** column below names Rust tools, and **none of them exists** (§3). Where a
format is read today it is read in-process; where the plan needed a cook there is no cook. That is
the honest state of this table, and it is left standing rather than deleted because the fidelity
notes in the right-hand column are still the specification whoever writes those tools must meet.

| Ext | Kind | Runtime reader | Offline compiler *(none built)* | Fidelity notes |
|---|---|---|---|---|
| `.ocbeam` | soft-body cage source (text) | Aver.Formats (tolerant parser) | aver-ocbeamc → `.ocbeamc` | Resolve the recon discrepancies: accept 10–13 material fields (default 10–12), `DETACH=` position-independent, 3 behaviors + `GLASS` alias, panel 4th-field override, discard PART leading id. |
| `.ocaero` | baked aero table (text) | Aver.Formats | aver-aerobake → `.ocaeroc` | Validate `OCAERO 1` magic; ascending-axis check; 9- and 10-field rows; density unused at runtime. |
| `.ocmap` / `.scene` | object-reference world (text) | Aver.Formats | aver-mapc | Invariant-locale, LF-normalized writer (fix Java locale + mixed-CRLF bugs). Upgrade ROOT to a real Merkle over placements+asset hashes. |
| `.octrack` | track variant (text) | Aver.Formats | aver-mapc | Treated as an `.ocmap`/`.scene` sibling profile. |

**New native formats (added for unaddressed asset types).** Binary, versioned, little-endian, magic-tagged. All of them sit on one container, **AVR1** (`modules/formats/include/aver/formats/Avr1.hpp`): a file is "an AVR1 with a different subtype and a different set of chunks", which is what lets a reader say *this is a `.octex` and you asked me to load a mesh* instead of *bad file*. Written and read in-process by **Aver.Formats** and its siblings for the engine's own loaders — that
part is still true — but **`aver-assetc` now exists**, this sentence's "and there never has been" is
wrong, and it is worth being precise about what changed: `tools/AverAssetC.cpp` is a real, built C++
executable (`build/bin/AverAssetC.exe`, registered in `tests/formats/CMakeLists.txt`) that the
launcher's "Aver Exchange" feature shells out to for glTF/OBJ/USDA (and, conditionally, audio and
material) import-and-cook, reporting results as JSON-Lines. It is the tool `aver-assetc` names, just
written in C++ rather than Rust — see §3's Rust-tools row and §9 for the fuller correction.

**Reader?** is the honest column. It says what has a reader and a writer in the tree today.

| Ext | Reader? | Payload | Fills recon gap |
|---|---|---|---|
| `.ocmesh` | yes — `OcMesh.hpp` | static mesh: positions, normals, tangents, UV sets, vertex colors, index buffer, per-submesh material binding, LODs, bounds | `MeshData` kept only position+index — everything else was discarded |
| `.ocmat` | yes — `OcMat.hpp` (in **Aver.Formats.Material**) | PBR material: base color, metallic, roughness, normal, emissive, alpha mode, texture refs, per-primitive assignment | no render-material model existed at all |
| `.ocaudio` | yes — `OcAudio.hpp` (in **Aver.Formats.Audio**) | AVR1 audio: an `AHDR` header chunk and an `APCM` sample chunk. Importers: this module's own WAV reader, plus Media Foundation on Windows for mp3/m4a/aac/wma/flac — the OS decoder rather than a vendored one | audio had no asset format at all |
| `.ocskel` | yes — `OcAnim.hpp` | skeleton: bone hierarchy, local rest TRS, inverse-bind matrices. The skinned-mesh half (`JOINTS`/`WEIGHTS`, the `SKIN` chunk) is **not** written yet, and a reader that later gains it finds it absent and says so | only text `BONE{}` rows; single-skin limit |
| `.ocanim` | yes — `OcAnim.hpp` | animation: TRS tracks, per-channel mask, native fps + keyframe timing, curve interp | fixed 30-fps resample + CUBICSPLINE/STEP loss — fixed here |
| `.ocworld` | yes — `OcWorld.hpp` | native scene/world: entity graph, environment (supersedes `.ocmap` for new content while `.ocmap` stays importable) | native world format |
| `.ocproject` | yes — `OcProject.hpp` | the project file the editor creates, opens and offers to upgrade | not a recon format; new |
| `.octex` | **no** | texture: pixel data (BCn), sampler/wrap/filter, sRGB flag, mips. Source images are decoded directly today (`Texture.hpp`), which is why nothing yet needs it | never parsed; only inside embedded glb |
| `.ocskm` | **no** | skeletal render mesh: bind pose, 4×(joint,weight), render attrs, explicit vertex-order | `PartSkin` had no render attrs + implicit external contract |
| `.ocprefab` | **no** | prefab: component graph + overrides for reusable actor templates | no prefab concept existed |
| `.ocpak` | **no** | the shipped archive the AVR1 header reserves a subtype for | — |

**Two file rewriters that are not formats.** `MaterialScript` and `ActorScript` live in
**Aver.Formats.Material** and read and write *C# source*, which is a strange thing to find in a
format module until you see what they are for: C# is the authoring source, and the editor must be
able to change what the designer typed without destroying it. `MaterialScript` rewrites a material's
`Configure` body from an edited `MaterialDesc`; `ActorScript` reads both a `.Designer.cs` generated
region and what a class declares in `Configure(ClassBuilder)`, and rewrites position, rotation and
scale only, matched by `ObjectId`. Its `canonicalMeshPath` reconciles the `Content/Meshes/X` versus
`Meshes/X` hash mismatch that otherwise makes the same mesh two different assets.

---

## 6. Plugin / module system

### 6.1 Linkage model — static by default, SHARED only for a P/Invoke seam

`aver_add_module()` in `cmake/AvModule.cmake` builds a **STATIC** library and offers no alternative;
the modules that are DLLs say `add_library(… SHARED)` by hand. So the split is not the planned
static-versus-plugin one, it is a different and simpler rule that the tree does follow:

- **A module is SHARED if and only if it carries a seam managed code binds to.** `Aver.Scene`,
  `Aver.Framework`, `Aver.Physics`, `Aver.Render.PBR`, `Aver.Render.Voxi`, `Aver.UI.Abi`,
  `Aver.Audio.Abi` and `Aver.Settings` are DLLs for that reason and no other; each has a
  `RUNTIME_OUTPUT_DIRECTORY` of `bin/` because that is where `DllImport` looks. Two of those seams
  are not bound from C# yet — physics is driven host-side, and nothing links the audio seam at all —
  so the shape is ahead of the use in both cases. `Aver.Settings` is bound: `scripting/csharp/
  Aver.Framework/Settings.cs` P/Invokes `aver_settings_*` directly. Everything else is static and
  collapses into the executable.
- **Being a DLL constrains the link line**, which is the point. No RHI type may sit behind a P/Invoke
  boundary, so a SHARED module's transitive closure must not reach `Aver.RHI` — which is exactly why
  `Aver.Render.PBR` and `Aver.Render.PBR.Materials` are two targets, and why `Aver.UI` and
  `Aver.Render.UI` are.
- **Nothing here was ever hot-swappable.** There is no `AvModule_*.dll` plugin, no `AVER_LINK`
  variable, no `AVER_MODULE_<NAME>_SHARED`, and no dynamic-library loader in `Aver.Platform` for one
  to be loaded by. This bullet used to describe all four.

### 6.2 Registration & lifecycle — not built

The plan was one C-linkage descriptor per module (`AvModuleDesc` with `on_register`/`on_startup`/
`on_tick`/`on_shutdown`), a CMake-generated `AvModuleManifest.cpp`, a topological sort by declared
dependency, and pull-registration of systems, loaders and net channels so that booting with zero
modules gave an empty valid engine (P4).

**None of it exists.** `AvModuleDesc`, `aver_module_entry` and `AvModuleManifest` appear nowhere in
the tree outside this document. What happens instead is that `sandbox/` names each module it wants
on its own link line behind an `if(TARGET …)`, constructs the ones it uses directly, and ticks them
in an order written by hand in `SandboxApp.cpp`. Compile-time opt-in (P5) is therefore real and
enforced by CMake; run-time self-registration (P4) is not, and an engine with "an empty module
registry" is a description of something that was never written.

### 6.3 CMake targets + feature options (compile in/out)

The real option set, from the top-level `CMakeLists.txt`. Note how much shorter it is than the
module table: an option exists where a module is genuinely severable, and nowhere else.

```cmake
option(AVER_RHI_D3D12  "DirectX 12 backend (primary)"       ON)
option(AVER_RHI_D3D11  "DirectX 11 backend"                 ON)
option(AVER_RHI_VULKAN "Vulkan backend (needs Vulkan SDK)"  OFF)  # no SDK yet
option(AVER_BUILD_SANDBOX "Build the sandbox sample app"    ON)
option(AVER_BUILD_TESTS   "Build test executables"          ON)
option(AVER_ENABLE_UI     "In-window editor UI (Dear ImGui)" ON)
option(AVER_MODULE_VOXI      "Voxi render module"           ON)
option(AVER_MODULE_PBR       "PBR material system"          ON)
option(AVER_MODULE_SCRIPTING "In-process .NET scripting host" ON)
option(AVER_MODULE_SCENE     "Entity/component world"       ON)
option(AVER_MODULE_FRAMEWORK "Gameplay framework"           ON)
option(AVER_MODULE_PHYSICS   "Physics module (Jolt backend)" ON)

# Two combinations are not valid, and the build says so instead of failing four includes deep:
if(AVER_MODULE_VOXI AND NOT AVER_MODULE_PBR)          set(AVER_MODULE_VOXI OFF)      endif()
if(AVER_MODULE_FRAMEWORK AND NOT AVER_MODULE_SCENE)   set(AVER_MODULE_FRAMEWORK OFF) endif()

# Unconditional: core, platform, assets, formats, rhi, runtime — and audio, ui, render.ui,
# ui.abi, render.actorpreview, for the reason section 2 gives (Core-only, or generic-RHI-only).
# Guarded on the OS rather than on a feature switch: audio.wasapi, audio.abi (WIN32).
```

Each module target declares only its own dependencies, so a violation is a link error rather than a
review comment — `Aver.UI` cannot quietly learn to render, because it would have to name `Aver.RHI`
to do it.

There is **no CI, and no "DAG lint"**. An earlier revision of this section claimed the DAG was
"machine-enforced, not just documented"; `cmake/AvModule.cmake` is more careful and says the explicit
`DEPS` list is what "lets a *future* CI DAG-lint step verify no edge points up a tier". What actually
holds the DAG today is that a bad edge usually will not link, plus the edge list in §2 being
re-derived by hand.

### 6.4 How this kills each specific UE bloat source

The **Held?** column is what this table was missing. A structural fix that is only described is a
statement of intent, and three of these six are still that.

| UE bloat source | Aver structural fix | Held? |
|---|---|---|
| **Monolithic build** (change one thing, rebuild the world) | Per-module CMake targets + feature options. A headless tool links `Core+Platform+Assets+Formats` and no renderer; the audio and UI tests link three targets each. | **Yes.** The test executables are the proof: `AudioTest` links `Aver.Audio`, `Aver.Core`, `Aver.Platform` and nothing else. |
| **Everything-is-a-UObject** (vtable + reflection + GC tax on every object) | No base object; handles + POD component arrays; `AvId` is a `u32`. | **Yes** for the storage model. The "offline codegen reflection" half is not built — `Aver.Scene` resolves component fields by *name* at run time instead (`aver_scene_field`), which is a different trade and a real one. |
| **Editor coupled to runtime** (UnrealEd bleeds into game modules) | Editor is a C# app over the C ABI; runtime links no editor code (P3). | **No.** The editor is `sandbox/`, a C++ ImGui application that links the modules directly. The seams it would have used exist and are good; nothing is on the far side of them. |
| **Giant DDC** (multi-GB shared cache, network DDC service) | Plain content-addressed file cache keyed by `hash(source⊕compilerVer⊕opts)` (P6). | **Not built.** There is no cook and therefore no cache. The nearest thing is `Binaries/Materials/*.ocmat`, written by `avermatc` and preferred over the C# source at load. |
| **Mandatory subsystems** (engine forces a fixed subsystem set) | Engine boots empty; every subsystem is an opt-in self-registering module (P4). | **Half.** Compile-time opt-in is real and CMake-enforced. Run-time self-registration is not built at all (§6.2), and five modules are unconditional by deliberate choice (§2). |
| **Plugin DLL sprawl / discovery cost** | Static-first collapse to one binary for ship; DLLs only where they earn their keep. | **Yes**, but for a different reason than planned: DLLs exist where a managed seam sits, not where hot-swap is wanted, and there is no plugin discovery to cost anything (§6.1). |

---

## 7. The C ABI boundary — where C# plugs in

### 7.1 Shape of the seams — there is no single one

**`Aver.ABI` was designed here, and then dropped.** Not deferred: dropped, with the reasoning
recorded in `modules/abi/README.md`. A single seam has to link everything it exposes, so it could
not simultaneously keep `scene_abi.h` free of the word *actor*, keep `Aver.Render.PBR` free of every
RHI type, and let one surface move while another is still settling. Those three properties are the
whole reason a seam exists, so the seam was split instead of the properties given up. **`docs/ABI.md`
is the reference for all of them and this section is only the shape.** Nothing named
`aver_abi_version` exists anywhere in the tree; if you are reading an older copy of this file that
declares one, that is the sketch and not the code.

What is real is one C surface per module, each exported by its own DLL, each with its own export
macro: `scene_abi.h`, `framework_abi.h`, `physics_abi.h`, `pbr_abi.h`,
`voxi_abi.h`, `ui_abi.h`, `audio_abi.h`, `settings_abi.h`. `scripting_abi.h` is the exception that proves the rule —
the script host exports nothing at all, because a P/Invoke would have to name the loaded module,
which is the *executable*, and that would tie a shipped bridge assembly to whatever host embeds it.
It hands the managed bridge a table of function pointers at bootstrap instead.

The conventions the original sketch fixed did survive, and every seam holds to them:

- **Opaque handles** — no C++ type crosses the line.
- **Struct returns via out-pointer** (`aver_scene_world_matrix(e, float* out16)`), to avoid a
  return-ABI mismatch between calling conventions.
- **POD-only structs**, laid out so C# can declare them `LayoutKind.Sequential`; strings are UTF-8
  `const char*` in both directions.
- **A version on TWO of the eight** — `aver_scene_abi_version()` and `aver_fw_abi_version()`. Physics, PBR, Voxi, UI, Audio and Settings declare none, and the "and so on" that used to end this sentence was the whole error: there is no third. Each
  governing only its own boundary. `docs/ABI.md` §14 records that nothing in shipping code currently
  calls them, which is a gap and is named as one there.
- **No ownership ambiguity** — create/destroy pairs; a seam never frees caller memory or vice versa.

### 7.2 Where each language plugs in

```
                 ┌────────────────────────────────────────────────────┐
   C# (.NET)     │ Aver.Scripting  Aver.Scene  Aver.Framework          │  staged beside the exe
                 │ Aver.UI  Aver.Materials                            │  built by a game's compile
                 │ avermatc — reflects an assembly, writes .ocmat     │  staged to bin/Tools
                 └──────┬──────┬──────┬──────┬──────┬──────┬──────────┘
                        │ P/Invoke, one seam per module, by DLL name
      ══════════════════▼══════▼══════▼══════▼══════▼══════▼═══════════
        scene_abi.h  framework_abi.h  physics_abi.h  pbr_abi.h
        voxi_abi.h   ui_abi.h         audio_abi.h    (+ scripting_abi.h,
      ══════════════════▲══════▲══════▲══════▲══════▲   which exports nothing)
                        │
                 C++ modules — each seam is that module's own DLL

   Rust          (still none — there is no .rs and no Cargo.toml anywhere in this repository — but
                  "tools/ holds a README" is no longer an accurate description of that directory:
                  it holds several real, built C++ executables, including `AverAssetC`, which does
                  the asset-cook job this row's boundary was drawn for. See §3/§9.)
```

- **C# — gameplay scripting and content authoring.** The CLR is hosted **in process** by
  `Aver.Scripting.Host` rather than the other way round: the C++ application owns the frame loop and
  the window, and managed code is called into it. That is the reverse of the arrangement this
  section originally described, where a C# editor hosted a headless runtime. C# also authors
  *content* now,
  not only behaviour — a material and an actor are C# source that the editor rewrites in place, and
  `avermatc` reflects the compiled assembly to produce `.ocmat`.
- **Rust — still nothing, but the asset pipeline itself is no longer in that "nothing."** The cage
  compiler, the wind-tunnel bake and the shader cook were all assigned to standalone Rust executables
  sharing *files* rather than linkage with the runtime, and none of those three is written. The asset
  pipeline is different: `tools/AverAssetC.cpp` builds and ships as a real, standalone executable that
  does that job — glTF/OBJ/USDA (and, conditionally, audio/material) import-and-cook, out of process,
  called by the launcher — just in C++ instead of Rust. Editor-side import still also happens
  in-process in `Aver.Formats`, which is a different boundary with different consequences: a broken
  importer there is a broken editor rather than a failed cook. The two are not the same code path.
- **C — the seams themselves.** Each module exports its own, which is what lets a headless tool link
  one and not the rest.

---

## 8. Rendering-quality posture (UE5 + Source 2 class, permissively-licensed)

This section named `Aver.Render` and `Aver.Render.GI` as the home of everything below. Neither
module exists (§3), so read the list as a target and not a description. What renders today is a set
of independent features, each driving `Aver.RHI` directly with no render graph between them.

- **Pipeline:** *planned* — clustered deferred + forward+ hybrid, GPU-driven culling, a virtualized
  shadow atlas. **Today:** `Aver.Render.Voxi.Renderer` owns the scene lit pipelines and a single
  directional shadow map; `Aver.Render.UI` and `Aver.Render.ActorPreview` own their own. There is no
  frame graph and no shared render-scene.
- **GI:** *planned* — surfel and irradiance-probe GI plus screen-space GI, in an `Aver.Render.GI`
  opt module. **Today:** voxel cone tracing and DXR 1.1 inline `RayQuery` sun shadows, both inside
  `Aver.Render.Voxi.Renderer`. Different technique, different module; the planned one is unstarted.
- **Upscaling/AA:** *planned* — TAA plus AMD FidelityFX FSR. **Not started**, and FSR is not vendored.
- **Materials:** real, and it landed differently from the sketch. `Aver.Render.PBR` is the material
  system and `Aver.Render.PBR.Materials` turns an authored material into a binding set. The
  authoring surface is **C#** rather than a runtime graph API — `[AverMaterial]` and
  `MaterialBuilder` under `scripting/csharp/Aver.Materials`, compiled to `.ocmat` by `avermatc`. The
  recon's headless-crash lesson still holds: nothing recompiles a shader to change a material.
- **Third-party actually vendored:** **four** things, and one of them is not under `third_party/`.
  Dear ImGui (MIT, editor only), stb (public domain) and the editor's fonts are; **Jolt Physics
  (MIT) is vendored at `modules/physics.jolt/`**, because it is the rigid-body backend behind
  `Aver.Physics` and is named like every other backend here (`rhi.d3d12`, `audio.wasapi`,
  `formats.roslyn`). The directory says what it is in the module graph; the README in it keeps the
  provenance — version, upstream archive, SHA-256, licence, and what upstream was left out.
  Vendored is still vendored: those sources are not edited, and `scripts/stage-payload.ps1`
  concatenates the `LICENSE` there into `THIRD-PARTY-NOTICES.txt`. Everything else this
  list used to claim is absent: no meshoptimizer, no DirectXTex, no FSR, no EnTT, no miniaudio, no
  cgltf, no xxHash, no Blake3, no Recast/Detour. Where a capability was needed and no permissive
  dependency was taken, the tree either wrote its own (the audio mixer, the WAV reader, the glTF
  import) or used what the OS already ships (Media Foundation for compressed audio; `d3dcompiler`,
  which tops out at SM 5.1, with `dxcompiler.dll` redistributed beside the executable and loaded at
  run time for SM 6.x). The licence constraint is intact — no GPL, no Unreal, no proprietary tech —
  but it has been met by writing code rather than by collecting libraries.

---

## 9. Repo layout as it actually is

Four of the top-level directories this section proposed hold a README and nothing else. They are
kept below with what they were for, because a directory that quietly disappears takes its decision
with it — but `‹›` means empty of code.

```
Aver Engine/
  CMakeLists.txt                 # top-level options + guarded add_subdirectory. No DAG-lint hook.
  CMakePresets.json
  cmake/AvModule.cmake           # aver_add_module(): STATIC lib, PUBLIC include dir, PUBLIC deps
  modules/
    # This list said "34 directories, 22 built, 12 README" for a long time; the tree has since
    # grown well past what §2/§3 individually describe, and the honest count is now 58 directories,
    # 49 of them carrying a CMakeLists.txt. Rather than let this list go stale line by line the way
    # the old one did, treat `modules/*/CMakeLists.txt` itself as the source of truth for BUILT vs
    # README-only — a directory listing is not a claim about what a module does, only that it links.
    BUILT (49):    core/ platform/ assets/ formats/ formats.roslyn/ formats.particles/ anim/
              anim.scene/ deform/ rhi/ rhi.d3d12/ rhi.d3d12.imgui/ rhi.d3d11/ rhi.vulkan/
              rhi.vulkan.imgui/ scene/ framework/ physics/ physics.jolt/ scripting/ runtime/
              runtime.game/ landscape/ mcp/ save/ settings/ upgrade/ synapse/ synapse.scene/
              render.pbr/ render.voxi/ render.ui/ render.actorpreview/ render.pcg/ render.pt/
              render.skin/ render.softbody/ render.sr/ fluids/ particles/ occlusion/ trifactor/
              sound/ ui/ ui.abi/ audio/ audio.wasapi/ audio.abi/ world/
    README-only (9): abi/ aero/ fracture/ gpudeform/ match/ net/ netvehicle/ softbody/ vehicle/
    # `render/` and `render.gi/` are not in either list above — see the DELETED note below, and §1/§3.
  sandbox/                       # the editor. C++ + Dear ImGui, links the modules directly.
  tests/                         # ui/ render.ui/ render.actorpreview/ audio/ formats/
                                 #   scene/ framework/ physics/ — each a plain exe in bin/
  scripting/csharp/              # Aver.Scripting(+.Bridge) Aver.Scene Aver.Framework
                                 #   Aver.UI Aver.Materials Aver.MaterialCompiler + samples
  third_party/                   # imgui/ stb/ fonts/ dxc-spirv/ vulkan-headers/ — and Jolt is
                                 #   NOT here: it is the physics backend, at modules/physics.jolt/
  branding/                      # splash, logo, icon sheets staged beside the exe
  scripts/                       # build.ps1, run.ps1, gates.ps1 + baselines, brand.py
  content/legacy/                # sample fixtures for golden tests
  docs/                          # this file, ABI.md, ACTOR_EDITOR.md, AUDIO.md, RENDERING.md, …
  ‹abi/›                         # was: Aver.ABI headers + wrapper TUs. Dropped; see §7.1. KEPT as a
                                 #   directory on purpose -- its README is the record of that drop.
  tools/                         # NOT a skeleton any more — this line used to mark it ‹tools/›
                                 #   as though it held only a README for the never-built Rust cook.
                                 #   It holds real, built C++: AverAssetC (the asset cook, standing
                                 #   in for the planned Rust aver-assetc), AverCrashReporter,
                                 #   ActorSweep, MakeFoliage, MakeRig, MakeSamples, RelodTool,
                                 #   DumpClusterPs. Still no Rust in it or anywhere else.
  # DELETED, not merely empty: modules/render/, modules/render.gi/, shaders/, editor/ and interop/
  # each held one README describing something never started or since built elsewhere, and this file
  # already said so more accurately than they did — an older revision of this tree still listed
  # modules/render/ and modules/render.gi/ under README-only, which was wrong the moment the
  # directories themselves were removed rather than merely left empty. HLSL is in files at
  # modules/<mod>/shaders/ and sandbox/shaders/; the editor is sandbox/; the C# bindings are
  # hand-written under scripting/csharp/.
```

---

## 10. Key risks / decisions to pin before build-out

1. **Sim determinism is a linkage guarantee, not a hope.** `Aver.SoftBody`/`Aver.Vehicle` must compile bit-identically for client, C# server (P/Invoke), and the Rust cage-compiler validator. Fix float eval order and phase order per recon; add a golden cross-platform determinism test in CI.
2. **`.ocbeam` material field-count discrepancy is load-bearing** (runtime wanted exactly 10, writer emitted 13). Canonicalize in `Aver.Formats` + `aver-ocbeamc` (accept 10–13, default 10–12) or existing content silently loses all materials.
3. **Map ROOT/locale/CRLF bugs** in the legacy Java writer must be *fixed* in `aver-mapc` (invariant locale, LF, real Merkle root) while `Aver.Formats` stays tolerant of the old mixed output for import.
4. **Scene↔Render dependency direction.** Kept one-way (Scene stays render-agnostic) to preserve the DAG — do not let a render module reach back into Scene. Held so far, and held by a link line: `Aver.Scene` does not link `Aver.Render.PBR`, which is why a material is an interned name there and nothing more.
5. **Vulkan stays compiled OFF** (`AVER_RHI_VULKAN=OFF`). This item used to say enabling it "produces nothing, because the backend is a stub" — that is no longer true (see §3's `Aver.RHI.Vulkan` row and `modules/rhi.vulkan/README.md`): the flag now builds a backend that presents a frame, including the editor's own UI, with 10 validation-layer messages left over an original 156. Two of the four obstacles this item used to list turned out to be exactly what the work went into, and are done:
   - ~~Root CBVs and root SRVs are in the GENERIC layout... Vulkan has no equivalent short of `VK_KHR_buffer_device_address`.~~ **Done for CBVs.** `patchCbuffersForLayout` (`VulkanResourceFactory.cpp`) lowers every cbuffer per `PipelineLayout::constantDwords[k]` — non-zero folds into one push-constant struct, zero becomes a descriptor at binding `N` in `kVkSetConstants` — computed at pipeline creation rather than assumed from the shader text. **Not done for the mesh path's root SRVs**: `gVerts`/`gIndices` on `MSVoxel` and `gInstanceWorlds` at `t17` are exactly the 9 remaining validation errors, because `GraphicsPipelineDesc::instanced` and feature-module mesh geometry are still unimplemented on this backend. This is the one real obstacle left of the four.
   - **The push-constant budget** is no longer an unaccounted-for design gap: `VulkanDevice` queries the real `maxPushConstantsSize` from the physical device and every pipeline's computed `PushConstantLayout::totalBytes` is checked against it at build time (`VulkanResourceFactory.cpp`, `VulkanPipeline.cpp`), failing loudly rather than overflowing silently. Whether 144 bytes fits a given GPU's guaranteed 128-byte minimum is still a real per-device question; it is now a checked one.
   - ~~Register spaces. HLSL here uses `b0`/`t0`/`u0`/`s0` simultaneously... need explicit remapping.~~ **Done.** `buildRegisterBinds` (`VulkanRegisterMap.hpp`) derives one `-fvk-bind-register` per resource from the `PipelineLayout` and hands the complete map to DXC, splitting HLSL's one continuous table into per-set bindings that each restart at 0 — `tests/rhi/RegisterBindMapTest.cpp` pins it against the layout that was originally failing.
   - **Buffer state decay.** Skinning's barrier contract is written around D3D12 decaying buffers to `COMMON` at the end of a command list; `VulkanCommon.hpp` does track a `ResourceState::Common` case in its own barrier code, but whether it reproduces the same decay semantics was not re-verified for this pass and the claim is left as this item's one still-unconfirmed obstacle.
6. **This document was believed for longer than it was true.** `Aver.Render` and `Aver.Render.GI` were cited across §8 and the DAG as though they held the rendering stack, and an `aver_abi_version()` was quoted in §7 as though it existed; a reader following either would have gone looking for a directory containing one README. The mitigation is not a warning, it is a habit: check a module against `modules/` and the top-level `CMakeLists.txt` before writing a sentence that depends on it, and mark rather than delete what turns out to be a plan, so the next reader inherits the decision instead of a silence.

---

This defines a strict-DAG, pay-for-what-you-use module graph — `Core` sinks to nothing
engine-specific, and the edge list in §2 is transcribed from the build rather than asserted. This
paragraph once said "thirty-four directories under `modules/`, twenty-two carry a `CMakeLists.txt`
and twelve carry only a README" — the tree has grown since, and as of this pass it is **58**
directories, **49** carrying a `CMakeLists.txt` and **9** carrying only a README (§9 names both
sets). Because that count will keep moving as modules are added, treat `modules/*/CMakeLists.txt`
as the actual source of truth rather than either number here; the two sets are marked apart
throughout, because an earlier revision of this file did not mark them apart at all and was read as
a description of a tree it did not match.

Three of the seven design principles are held as written. P1 (Core knows nothing engine-specific),
P2 (no universal base object) and P7 (one RHI, no backend on a feature's link line) are enforced by
link lines and would fail to build if broken. P5 (build only what you ship) is enforced by CMake.
P4 is half-held: compile-time opt-in is real, run-time self-registration was never written. P6 has
nothing behind it, because there is no cook. **P3 is not held** — the editor is a C++ application
that links the modules directly, and that is the largest gap in this document. The seams it would
need are built and documented in `docs/ABI.md`; what is missing is anything on the far side of them.