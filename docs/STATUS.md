# Aver Engine — Status & Handoff

Living record of where the engine stands and what's next. Updated 2026-07-20.
**HEAD: `0bba4c2`** · 52 commits · 147 tracked files.

Read this first after a context compaction, then `docs/ARCHITECTURE.md` (module DAG),
`docs/MINIMUM_SPECS.md` (hardware requirements / launcher spec),
`docs/formats/DECISIONS.md` (formats), and `docs/PROJECTS.md` (engine⟂project).

---

## 0. What this is + direction

**Aver Engine** — a custom, modular 3D engine on permissively-licensed libraries only
(MIT/BSD/zlib/Apache-2.0/public-domain). Originally the successor runtime for the
**OpenConstructor** soft-body destructible racing sim; now being taken in a more
**general-purpose** direction (see §9). Engine ⟂ projects: the engine holds no game
content; games live in their own folders with a `.ocproject` manifest.

- **Polyglot:** C++ (core/RHI/renderer/physics), C (stable ABI), C# (.NET 10 editor +
  scripting — not built yet), Rust (asset pipeline — not built yet).
- **Backends:** DirectX 12 (implemented), DirectX 11 + Vulkan (stubs; Vulkan OFF, no SDK).
- **Coordinate contract:** centimeters, +Z up, +X forward, +Y right, left-handed,
  row-major/row-vector matrices (`v * M`).

## 1. Build & run

Requires Visual Studio 18 (C++ workload) — supplies CMake + Ninja + Windows SDK.
```powershell
cd "C:\Users\User\Documents\Aver Engine"
./scripts/build.ps1                 # configure + build (Debug) via vcvars->cmake->ninja
./scripts/run.ps1                   # build then launch the editor (Sandbox.exe)
./scripts/run.ps1 --frames 30 --screenshot out.png   # headless-ish capture for verification
./scripts/run.ps1 <path.ocbeam>     # also load a cage into the scene
```
Output: `build/bin/Sandbox.exe`. `scripts/build.bat` is the real build (PowerShell wraps it);
run `.bat` via the PowerShell tool, not Git Bash (`cmd //c` mangling). Add `/Zc:__cplusplus`
already set. Vulkan: `-DAVER_RHI_VULKAN=ON` once the LunarG SDK is installed.

## 2. Repo layout (key paths)

```
CMakeLists.txt              top-level (LANGUAGES C CXX RC; options AVER_RHI_*, AVER_ENABLE_UI)
cmake/AvModule.cmake        aver_add_module() helper
scripts/                    build.bat / build.ps1 / run.ps1
modules/
  core/      Aver.Core      types, Math (Vec/Mat/Quat/Transform/AABB + Mat4::inverse), Log, Time, Hash(fnv1a64)
  platform/  Aver.Platform  Win32 Window (+icon, message hook), Splash (layered win + stb_image), FileSystem (+executableDir)
  assets/    Aver.Assets    ObjectId (fnv1a64), AssetType
  formats/   Aver.Formats   .ocbeam + .ocmap loaders (+ detail/TextScan.hpp)
  rhi/       Aver.RHI        IDevice/ISwapchain + the generic render-feature surface
                             (RHIResources.hpp) + shared shader prelude + Null backend + uiWndProc
  rhi.d3d12/ Aver.RHI.D3D12  THE backend (device, swapchain, MSAA, PBR, sky, lines, mesh-shader
                             path, ImGui host, capture, the generic factory/context)
  rhi.d3d11/ rhi.vulkan/     stubs
  render.voxi/               Aver.Render.Voxi (SHARED: settings + C ABI, Core only) and
                             Aver.Render.Voxi.Renderer (STATIC: GI/shadow/RayQuery, drives Aver.RHI)
  runtime/   Aver.Runtime    Engine loop, Application, EntryPoint (splash + ImGui hooks)
  (skeleton, not yet wired: render, render.gi, scene, physics, softbody, aero, gpudeform,
   fracture, vehicle, net, netvehicle, match, audio, world, abi — each has a README)
sandbox/     Sandbox.exe      the editor app (SandboxApp.cpp) + Sandbox.rc (icon)
tests/formats/ FormatTest.exe golden test for the loaders
third_party/ imgui/ (docking, MIT) stb/ (stb_image + stb_image_write, PD)
branding/    master-lockup.png (HUMAN, source of truth) -> splash.png, icon.ico, logo*.png
             ai-generated/ (Claude's superseded vector concepts, shipped nowhere), ASSETS.md
docs/        ARCHITECTURE, formats/, rendering/, physics-net/, recon/, PROJECTS, EDITOR, this file
```
Content lives OUTSIDE the engine: example project at
`C:\Users\User\Documents\Aver Projects\OpenConstructor\` (`.ocproject` + `Content/`).

## 3. Renderer (Aver.RHI.D3D12) — implemented

- D3D12 device, DXGI FLIP_DISCARD swapchain (2 buffers), fences, resize.
- **MSAA is a runtime setting** (Off/2x/4x/8x) via `IDevice::setSampleCount`: rebuilds the scene
  targets and every PSO. 1x uses `CopyResource` (resolve is illegal at one sample).
- **PBR**: Cook-Torrance GGX, per-object metallic/roughness (root constants, 24 DWORDs),
  sky-hemisphere ambient + sky env reflection, sRGB→linear in, ACES tonemap + gamma out.
  `gMaterial.z>0.5` = unlit path.
- **Procedural sky + atmosphere**: fullscreen shader (`VSky/PSky`), ray from `gInvViewProj`,
  zenith/horizon gradient + sun disk/glow; distance fog in the mesh PS.
- **Lines**: unlit `createLineMesh`/`drawLines` (grid, gizmo), line PSO.
- **Wireframe**: second FillMode=WIREFRAME mesh PSO via `setWireframe`.
- **Mesh shader geometry path** (`IDevice::setMeshShaders`, dev flag `--ms`): `MSMain` replaces the
  input assembler for every draw. A generic capability, not an effect, which is why it lives here.
- Shaders: compiled at runtime by **DXC → DXIL, SM 6.0** baseline; SM 6.5 for the mesh-shader and
  RayQuery variants. Falls back to FXC/SM 5.1 if `dxcompiler.dll` is absent. `dxcompiler.dll`+
  `dxil.dll` are copied into `bin/` by CMake and MUST ship with the product (`docs/MINIMUM_SPECS.md` §5b).
- **The backend's own scene shading is `PSMainPlain`: unshadowed, no GI.** The shadow map, the
  radiance volume and the RayQuery occlusion ray belong to the Voxi render feature (§4b, §4c-2).
  A build with no feature registered renders that plain image by design, not by accident.
- **Generic render-feature surface**: `resources()` hands out an `IResourceFactory` (textures,
  buffers, shaders, pipelines, binding sets, BLAS/TLAS) and `addRenderFeature` registers a hook set
  driven per frame. Declared in `modules/rhi/include/aver/rhi/RHIResources.hpp`, with no vocabulary
  from any one feature in it.
- RHI API: `createDevice/createSwapchain/beginFrame/endFrame`, `setClearColor/setCamera
  (viewProj,invViewProj,camPos)/setLight/setSky`, `createMesh/drawMesh(mesh,world,color,
  metallic,roughness)/createLineMesh/drawLines/setWireframe/setLineDepth/setMeshShaders`,
  `resources/addRenderFeature/removeRenderFeature`, `uiInit/uiNewFrame/uiShutdown/
  uiWantsMouse/uiActive`, `requestCapture/getCapture/getFrameImage` (PNG via stb).
- Verified ~60 FPS on **AMD Radeon RX 7800 XT**.

## 4. Editor (sandbox) — implemented

Dear ImGui (docking) hosted inside the D3D12 backend; dark Unreal-style theme.
- **DPI-aware**: the process is per-monitor DPI aware (Win32Window), the window opens at a
  DPI-scaled logical size clamped to the monitor work area, and ImGui is scaled by
  `Window::dpiScale()` (`ScaleAllSizes` + `FontGlobalScale`). Fixes the soft/sluggish
  bitmap-upscaled viewport on hi-DPI displays (dev machine reports 300%).
- **Unreal-style docked shell** (ImGui DockSpace, `ImGuiDockNodeFlags_PassthruCentralNode`):
  menu bar → main toolbar (Save / Add▾ / Play·Pause·Stop / Settings▾) → dockspace → status bar.
  Default layout: **World Outliner** + **Details** stacked right (~22%), **Content Browser** and
  **Output Log** tabbed at the bottom (~26%), 3D viewport in the transparent central node.
  Panels are fully dockable/tabbable/floatable; *Window ▸ Reset Layout* rebuilds the default.
- **Viewport overlay bars** (transform tools live in the VIEWPORT, as in Unreal — not the window
  toolbar): left = Perspective▾ / Lit▾ / Show▾; right = Select·Move·Rotate·Scale icons with
  **snap dropdown carets**, World/Local space toggle, and camera speed. Keys 1-4 still work.
- **Per-mode 3D gizmos** at the selection, drawn as an always-on-top overlay
  (`setLineDepth(false)`): Move = axis arrows, Rotate = 3 rings, Scale = axis + end-boxes.
  Only the active tool's gizmo shows; hovered/active axis highlights amber.
- **Axis-constrained manipulation** (screen-projection drag): grab an axis handle to move/
  scale along it, the centre handle for screen-plane move / uniform scale; Rotate follows the
  cursor around the ring. Optional snapping. X=fwd(red), Y=right(green), Z=up(blue).
- Panels: World Outliner, Details (live transform/color/metallic/roughness, or sun & sky),
  Output Log, viewport HUD.
- **Free-fly camera** (Unreal-style, NOT orbit): RMB look + WASD/QE fly (wheel = speed);
  wheel dollies; MMB pans; F focuses selection. No auto-orbit.
- **Click-to-pick** selection (camera ray vs per-object local AABB via inverse world).
- Default scene ("blank map") = ground floor + cube + directional sun + sky + fog.
- **Project browser** (Unreal-style start screen), shown before the editor chrome in the SAME
  window and ImGui host — not a launcher exe. Recent projects (persisted to
  `%LOCALAPPDATA%\AverEngine\recent.txt`, most-recent first, capped at 10, dead entries dropped
  on read), **New Project…** (scaffolds the `docs/PROJECTS.md` layout under
  `Documents\Aver Projects`, refusing an existing folder), **Open Project…** (`IFileOpenDialog`
  from the Windows SDK, plus a typed-path field), and **Skip** — a project is additive, and the
  editor still runs perfectly with none. Loading one retitles the window and fills the status
  bar, the Content Browser mount line and Project Settings ▸ Description.
  **The browser never appears in automation**: it is armed only for an interactive launch with
  no project, i.e. suppressed by `--frames`, by a `<path>.ocproject` argument, or by `--headless`.
  That is deliberate and load-bearing — the oracle reads a probe pixel out of the viewport, and a
  full-screen chooser in front of it would break all 13 gates at once.
- **Tools menu** (between Window and Build, where Unreal puts it), in `sandbox/src/ToolsMenu.cpp`.
  It is split into two labelled groups because **C# and C++ are not symmetric here**, and that is
  the thing most likely to surprise someone arriving from Unreal:

  | Group | Item | Writes to | Rebuild? |
  |---|---|---|---|
  | PROJECT — C# | New C# Script… | `<project>/Content/Scripts/<Name>.cs`, behaviour + hooks | no |
  | PROJECT — C# | New C# Class… | same folder, plain class, no hooks | no |
  | ENGINE — C++ | New C++ Module… | `modules/<name>/` (CMakeLists, README, include, src) | **yes** |
  | ENGINE — C++ | New C++ Class… | `.hpp`/`.cpp` in a picked `modules/<m>/` | **yes** |
  | — | Compile Scripts | `dotnet build` on `Scripts.csproj`, transcript in a modal | — |
  | — | Open Project Folder / Open in Visual Studio | shell-out | — |

  `.ocproject` carries no build integration, so **game C++ has nowhere project-side to live** and
  a new module goes into the ENGINE. The item labels, the group headers and the modals all say so.
  The first C# file in a project also emits `Scripts.csproj`, referencing the engine's
  `Aver.Scripting.csproj` so the folder opens as a buildable project rather than a loose `.cs`.
  Nothing overwrites an existing file; names are validated as identifiers; every project-dependent
  item is **disabled with an explaining tooltip** rather than hidden. Both the C# modals and the
  generated file headers state plainly that **the script will not run** — see §4d.

  **Neither CMakeLists is auto-edited.** The top-level one is not, because wiring a module into
  the build is a deliberate act and every existing skeleton under `modules/` is deliberately
  unwired; a module's own is not, because `aver_add_module` takes an explicit `SOURCES` list and
  never globs. Both modals show the exact line to add and why it is manual.
- Dev/testing args: `--tool <select|move|rotate|scale>` opens straight into a tool; `--new-script`
  forces the New C# Script modal open, `--tools-menu` holds the Tools dropdown open, and
  `--compile-scripts` fires one build — all three for screenshot verification, since headless
  capture can't inject mouse input. Each is gated on the same predicate as the menu item it
  stands in for, so a run that produces nothing has demonstrated a real disabled state rather
  than merely asserting one. None changes the menu BAR's height, and no oracle gate passes them.

## 4b. Voxi — first optional module (`modules/render.voxi`)

`Aver.Render.Voxi`, gated by `-DAVER_MODULE_VOXI` (ON by default; OFF still builds/runs).
Owns the project-wide render quality settings and reports, per feature, whether it is
`Ready` / `NotImplemented` / `Unsupported` from the real device caps:

- **MSAA — implemented**: Off/2x/4x/8x applied at runtime via `IDevice::setSampleCount`
  (rebuilds scene targets + all PSOs; 1x uses `CopyResource` since resolve is illegal there).
- **Global Illumination — implemented (voxel cone tracing).** Voxelise+inject into an atomic
  accumulator (dominant-axis projection by GS or mesh shader, conservative raster, UAV-only pass)
  -> compute resolve into the radiance volume -> compute mip filter -> 6-cone diffuse gather + AO
  inside the lit pixel shader. Volume is one frame old (draws are replayed next frame). Debug
  raymarch via the viewport Lit dropdown. ~59 FPS at 128^3.
- **Ray Tracing — implemented (DXR 1.1 inline `RayQuery`).** Exact hard sun shadows traced from the
  pixel shader; per-mesh BLAS and a per-frame TLAS over the replayed draw list. Falls back to the
  shadow map when the structure was not built. **Path Tracing — declared, not implemented.**
- **Directional shadow map** (2048², 3x3 PCF), used by the lit pass AND by light injection — a
  shadowed surface must not emit sun radiance into the volume or bounce light leaks through walls.
- Exposed in the editor under **Edit > Project Settings > Rendering** (project-wide, so NOT in
  the per-actor Details panel).
- **Two targets.** `Aver.Render.Voxi` (the settings DLL) depends on **Aver.Core only** — the host
  pushes `DeviceInfo` in, so no RHI type reaches the P/Invoke boundary. `Aver.Render.Voxi.Renderer`
  (the GPU half) links **`Aver.RHI` and never `Aver.RHI.D3D12`**, and owns every GPU resource the
  effects need. See §4c-2 for why the split is a link line rather than a convention.
- Built **SHARED** for C# P/Invoke; C ABI in `include/aver/voxi/voxi_abi.h` (`aver_voxi_*`),
  bound by `scripting/csharp/Aver.Scripting`. Verify with
  `dotnet run --project scripting/csharp/Aver.Scripting.Sample`.
  **Caveat**: a separate C# process gets its own copy of the DLL (own settings, empty caps);
  scripting the live editor needs in-process CLR hosting, which does not exist yet.
- Dev flags: `--msaa N` (exercise the runtime switch), `--project-settings` (open the window),
  `--gi`, `--gi-debug`, `--rt`, `--ms`.
- This machine reports: MSAA to 8x, **DXR tier 1.1**, typed UAV loads, conservative raster,
  mesh-shader tier 1, SM 6.6 — i.e. everything Voxi uses.

## 4c. Session log — what was built, in order

Everything below is committed and verified. Listed so a reader knows what NOT to redo.

| Commit | What |
|---|---|
| `81b8675` | Per-mode Unreal gizmos, icon toolbar + snapping, DPI-correct viewport |
| `cf49f18` | First window-freeze fix: render during modal move/size loops |
| `7dedfdf` | Easier gizmo grabbing (thin 1px lines at hi-DPI); rotate direction flipped |
| `e08c51a` | **Root-cause** freeze fix: wait-before-reuse fence (ResizeBuffers desynced the old parity-based scheme) |
| `54f6365` | Window chrome dead: a `break` in WndProc swallowed `WM_NCLBUTTONDOWN` |
| `164f80a` | Unreal-style docked layout; scene scissored into the dockspace central node |
| `5868ea8` | Voxi module + render settings + C ABI + C# bindings |
| `7001958` | Voxel cone traced GI (voxelise+inject → mip filter → 6-cone gather) |
| `bb06de0` | Shadowed injection + 4 cross-vendor portability fixes |
| `c90b1cc` | `docs/MINIMUM_SPECS.md` + the DXR 1.1 decision |
| `a08a126` | DXC migration (SM 6.x) + mesh-shader setting |
| `1be5bf7` | DXR 1.1 inline RayQuery ray-traced sun shadows |
| `14284b4`..`0bba4c2` | The Voxi/HAL decoupling refactor, 12 steps — see §4c-2 for the per-step table |

### Verification tooling (reuse this — it works)
Headless capture reads the **backbuffer**, so it does NOT prove the window is visible or
responsive. For anything interaction- or hang-related, drive the real window with synthetic
input and watch a per-frame heartbeat instead.

Dev flags on `Sandbox.exe`: `--frames N`, `--screenshot out.png`, `--tool <select|move|rotate|scale>`,
`--project-settings`, `--new-script`, `--tools-menu`, `--compile-scripts`, `--start-screen`,
`--msaa N`, `--gi`, `--gi-debug`, `--rt`, `--ms`, `--probe X Y`.

`--probe X Y` is the oracle: it prints the pixel as floats AND as raw 8-bit codes, because a
one-code move hides completely inside `%.2f`. Compare the raw codes, never the floats. Count
`0x141` TDRs around any render batch:
`Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1001}` filtered for `LiveKernelEvent`.

**The probe is self-validating.** The line now carries the 3D viewport rect and a tag:

```
[Sandbox] probe (1375,819) px (0.35,0.36,0.42) raw (90,93,108) viewport (0,198 2750x1242) in-viewport
[Sandbox] probe (100,50)   px (0.10,0.10,0.11) raw (26,26,28)  viewport (0,198 2750x1242) OUTSIDE-VIEWPORT
[ERROR] [Sandbox] PROBE INVALID: (100,50) is outside the 3D viewport (...) -- the value above is
        editor chrome, not a shading result
```

A pixel outside the rect samples editor chrome — the dock clear colour reads `raw(14,14,16)` and had
already been mistaken once for a shading regression, because nothing in the line said where the
sample came from. The tag is on the SAME line as the raw codes, so the grep that reads the value
cannot miss it, and the value is still printed: suppressing it is just a different way to be misread.
Three states: `in-viewport`, `OUTSIDE-VIEWPORT` (ERROR), and `VIEWPORT-MOVED` (WARN) for a resize
between the capture request and the read, where the latched rect no longer describes what was drawn.
The rect is latched **at the request frame**, not at the report frame, since that is the state that
produced the value. The default probe was already the viewport centre expressed off the rect rather
than an absolute pixel, so it follows the layout; `--probe X Y` stays absolute for the cast-shadow
gate, which is exactly the case that needs the containment check.

Debug views that paid for themselves: **Voxel Radiance** (viewport `Lit` dropdown or `--gi-debug`)
separates "voxelisation broken" from "cone tracing broken"; the centre-pixel readout in the log is
a cheap A/B oracle (e.g. GI on/off showed red 0.70→0.73 with G/B fixed = orange bounce).

## 4c-2. Voxi/HAL decoupling refactor — COMPLETE, all 12 steps landed

Voxi's GI, shadow map and DXR 1.1 RayQuery code has moved out of the D3D12 backend into
`modules/render.voxi`, talking only to a generic backend-agnostic RHI. The backend no longer knows
what a voxel, a shadow map or an occlusion ray is.

### Final architecture

Three pieces, and the boundary between them is a link line, not a convention:

- **`Aver.RHI`** — the generic interface. `RHIResources.hpp` declares `IResourceFactory` (textures,
  buffers, shaders, graphics/compute pipelines, binding sets, BLAS/TLAS), `IRenderContext` (the
  recording surface) and `IRenderFeature` (the per-frame hook set: `beginScene`, `submitDraw`,
  `prePass`, `scenePipeline`, `sceneBindingSet`, `sceneConstants`, `scenePass`, `suppressesScene`,
  `onRenderTargetsChanged`). No vocabulary from any one feature appears in it. `RHIShaders.cpp`
  holds `sharedShaderPrelude()`: the cbuffer layouts, `VSIn`/`VSOut`/`SkyOut`, the BRDF helpers,
  `shadeSurface`, `VSMain`, `VSky` and `MSMain` — one owner for a cross-module ABI that has no
  compiler behind it.
- **`Aver.RHI.D3D12`** — device, swapchain, MSAA, sky, lines, wireframe, the ImGui host, capture,
  the mesh-shader geometry path, and `PSMainPlain`. That is the whole of the shading it owns:
  `shadeSurface` takes sun visibility and indirect radiance as ARGUMENTS, so a backend with no
  feature registered passes `(1, 0, 1)` and gets the unshadowed, GI-free image. It also implements
  the generic factory and context, including acceleration-structure creation and builds.
- **`Aver.Render.Voxi.Renderer`** — the feature. Owns the shadow map, the radiance volume, the
  injection accumulator, every binding set, the BLAS/TLAS, and eleven pipelines: shadow, voxelise
  (+MS variant), clear, resolve, mip filter, debug raymarch and the four scene lit variants
  (IA/MS x shadow-map/RayQuery). `PSMainVoxi` is a whole pixel shader rather than an extra pass,
  because the terms it contributes live INSIDE the shading, which no arrangement of passes can
  express.

`Aver.Render.Voxi` (the settings DLL, SHARED for C# P/Invoke) still depends on **Aver.Core only** —
no RHI type reaches the P/Invoke boundary. `Aver.Render.Voxi.Renderer` is a deliberately SEPARATE
static target that links **`Aver.RHI`, never `Aver.RHI.D3D12`**: the link line is the only place
that rule can actually be enforced.

### What moved, step by step

| # | Step | Commit |
|---|------|--------|
| 0 | Golden baseline + `--probe X Y` | `14284b4` |
| 1 | Generic interface (`RHIResources.hpp`) | `f846b3d` |
| 2 | D3D12 factory + command context | `6c46f2d` |
| 3 | Feature hooks wired, list empty | `53c6c37` |
| 4 | Per-frame constants split b0/b4 | `57e1e34` |
| 5 | Shared shader prelude + `shadeSurface()` | `20439ac` |
| 6 | `VoxiRenderer` registered but inert | `937cece` |
| 7+8 | Shadow map + volume move into the feature | `08cf5be` (mislabelled `docs:`) |
| 9 | BLAS/TLAS move into the feature | `7b069ac` |
| 10 | Scene lit pipelines + debug view move into the feature | `194760a` |
| 11 | The backend's dead copy deleted, boundary tightened | `808f591` |
| 12 | Geometry path made reachable without the module | `0bba4c2` |
| — | Root CBVs always bound (GI hang fix) | `e461425` |
| — | TLAS published into the bound table (RT shadow fix) | `d01a50f` |
| — | Debugging scaffolding removed | `93a23e3` |
| — | Atomic injection accumulator (GI probe wobble fix) | `e34e413` |

**Steps 7 and 8 landed by accident and the record needs reading carefully.** They were written,
then reverted, and the reverted work was swept back into `08cf5be` by a `git add -A` that picked up
a stopped agent's in-progress edits — a commit whose subject says `docs:` but which carries 256
lines across four source files. It cost two separate debugging sessions: it landed the binding-set
half of step 7 without step 9, so `buildRtScene` kept writing the acceleration-structure SRV into a
heap nothing bound any more (RayQuery then traced a null AS and silently reported no hit, fixed in
`d01a50f`), and it shipped `AVER_DIAG` scaffolding to main (removed in `93a23e3`). This is why
`git status` before staging is a hard rule, not a preference.

### What step 11 deleted

The whole Voxi half of `kShaderHLSL` — `VSShadow`, `VSVoxel`/`GSVoxel`/`MSVoxel`, a second
`PSVoxel`, `PSMainVoxi`, `PSVoxelDebug`, `CSClear`, `CSMip`, the `b4` block and
`rtShadow`/`shadowFactor`/`traceCone`/`coneTracedIndirect` — plus `createShadowResources`,
`shadowPass`, `createVoxelVolume`, `createGiPipelines`, `voxelizePass`, `voxelBarrier`,
`bindGiTables`, `sceneBindings`, the replayed draw list and every member behind them. Roughly 1000
lines. Every one was verified unreachable by grepping for callers first: `shadowPass` and
`voxelizePass` had already had none since step 8.

The RayQuery PSO went too. It had carried `PSMainPlain` since step 10, so `rtActive_` was choosing
between two pipelines that drew the same image. What DXR still needs is the
`ID3D12Device5`/`ID3D12GraphicsCommandList4` pair the generic factory builds acceleration
structures through, so `initRayTracing` shrank to acquiring those under the same DXR 1.1 gate the
feature applies to itself.

With no feature resources left to bind, both backend root signatures lost their descriptor tables,
their two static samplers and the `b4` root CBV. `rootSig_` is now `b0` + 24 root constants;
`msRootSig_` adds the three geometry parameters the prelude pins to `t3`/`t4`/`b5`. Root parameter
indices became named constants, because the recording side passes them as bare integers where a
stale number binds the WRONG parameter rather than failing.

`IDevice` lost `setGi`, `GiSettings`, `setRayTracing` and `rayTracingActive`. `setMeshShaders` and
`meshShadersActive` stay: the geometry path decides how every draw reaches the rasteriser, which is
a device property, not an effect. The C# bindings were checked first — they talk to the Voxi C ABI
(`aver_voxi_*`), never to `IDevice`, so nothing there moved.

**One mesh-vertex input layout site remains**, shared by the backend's scene pipeline and the
generic factory. There were five. The planned PBR vertex format (§4d) is now a single edit.

### `AVER_MODULE_VOXI=OFF` — a real behavioural change, with a recorded baseline

Before the refactor the backend shaded the scene itself, so building without the module still gave
shadows and GI. It cannot any more, and that is the point: the feature owns them. A build with
`-DAVER_MODULE_VOXI=OFF` configures, compiles, links and renders — **unshadowed, with no GI, by
design**. Treat these as the expected values for that configuration, not as a regression:

| Probe | OFF build | ON build, for comparison |
|---|---|---|
| centre `(1375,814)` | `0.34,0.36,0.42` raw(87,92,107) | identical |
| cast shadow `(1413,1042)` | `0.43,0.46,0.52` raw(109,117,132) | `0.25,0.31,0.40` raw(64,78,101) |
| penumbra `(1413,1150)` | `0.41,0.44,0.49` raw(105,112,126) | `0.25,0.31,0.39` raw(65,78,99) |

`--ms` gives the same raw codes as the IA path. `--gi`, `--gi-debug` and `--rt` are inert: there is
no feature to drive them. The **centre probe is identical between the two builds** because it lands
on the cube's unlit left face where `ndl` is ~0 — which is exactly why every shadow or ray-tracing
check must use `--probe`, never the default.

Step 12 also moved `setMeshShaders` outside `#if AVER_MODULE_VOXI`. It had been inside, so the OFF
build could never select the mesh-shader path — and with the module ON the feature overrides the
scene pipeline, so the backend's own mesh-shader draw was unreachable in BOTH configurations. The
OFF build now renders identically on the IA and mesh-shader paths, which is what proves the
backend's `dispatchMesh` binds the right root parameters.

### The feature-absent path

`IDevice::resources()` returns `nullptr` by default, and no stub backend overrides it. D3D11 and
Vulkan never construct a device at all — their factories return `nullptr` and `createDevice` falls
through to Null. `VoxiRenderer::init` checks the factory first, logs
`[Voxi] init declined: backend exposes no resource factory (no GPU support)` and returns `false`;
`shutdown()` is safe after a declined init and safe called twice. Verified by driving a Null device
directly, not only by reading the code.

### Step 9: who decides whether a ray may be traced

The backend used to answer this (`rtActive_`, recomputed inside `buildRtScene`). It cannot: it does
not know whether this frame's build produced any instances, and the RayQuery pipeline can
legitimately be selected on a frame that has no structure, because the feature's replay list runs
one frame behind. The FEATURE publishes the answer as `gShadowParams.z` in its own `b4` block, and
`PSMainVoxi` picks `shadowFactor()` over `rtShadow()` when it is clear. Tracing an unbuilt or empty
acceleration structure is not an error anyone can see — RayQuery reports no hit for every pixel,
i.e. a fully lit scene, and the debug layer has nothing to say — so the guard belongs where the
fact is known.

**The `--rt --probe 1413 1042` gate does NOT distinguish the two paths.** That pixel is fully
shadowed either way. `--probe 1413 1150` is a penumbra pixel where they genuinely disagree —
`0.23,0.28,0.37` raw(59,72,94) ray-traced against `0.25,0.31,0.39` raw(65,78,99) from the 3x3 PCF —
and is the cheapest positive proof that RayQuery is live. Re-measured identical after step 11, on
both the IA and mesh-shader paths.

**Acceleration structures cannot be destroyed through the generic RHI.** `IResourceFactory` has
`createBlas`/`createTlas` but no matching destroy, so they are released only when the factory is.
Harmless while the scene's meshes are static and Voxi is shut down with the device; it needs an
answer before geometry becomes dynamic. Listed in §4d.

### Oracle — all 13 gates, bit-exact

```
--frames 40                             -> (0.35,0.36,0.42) raw(90,93,108)
--frames 40 --ms                        -> (0.35,0.36,0.42) raw(90,93,108)
--frames 40 --rt                        -> (0.35,0.36,0.42) raw(90,93,108)
--frames 40 --ms --rt                   -> (0.35,0.36,0.42) raw(90,93,108)
--frames 40 --gi                        -> (0.41,0.36,0.41) raw(104,91,104)
--frames 40 --ms --gi                   -> (0.41,0.36,0.41) raw(104,91,104)
--frames 40 --ms --rt --gi              -> (0.41,0.36,0.41) raw(104,91,104)
--frames 40 --gi-debug                  -> (0.26,0.17,0.18) raw(66,44,45)
--frames 40 --ms --gi-debug             -> (0.26,0.17,0.18) raw(66,44,45)
--frames 40 --probe 1413 1042           -> (0.25,0.31,0.40) raw(64,79,102)
--frames 40 --rt --probe 1413 1042      -> (0.25,0.31,0.40) raw(64,79,102)
--frames 40 --ms --rt --probe 1413 1042 -> (0.25,0.31,0.40) raw(64,79,102)
--frames 40 --gi --probe 1413 1042      -> (0.26,0.31,0.38) raw(67,79,97)
```

**Re-baselined 2026-07-21 by PBR step 17(c), the diffuse-Fresnel correction, and by nothing else.**
The change was approved in advance and announced before the work. `kd` was
`(1 - F) * (1 - metallic)`, weighting the diffuse response by the SPECULAR Fresnel at HdotV; that
one `kd` is reused by the direct diffuse, the sky ambient and the GI bounce, so a single
over-darkening was applied three times per surface. Removing it lifts every gate and darkens none,
which is the signature the correction predicts:

| gate | before | after | delta |
|---|---|---|---|
| centre, no GI | raw(87,92,107) | raw(90,93,108) | +3,+1,+1 |
| centre, `--gi` | raw(99,90,103) | raw(104,91,104) | +5,+1,+1 |
| cast shadow | raw(64,78,101) | raw(64,79,102) | +0,+1,+1 |
| cast shadow, `--gi` | raw(67,78,97) | raw(67,79,97) | +0,+1,+0 |
| `--gi-debug` (both) | raw(66,44,45) | raw(66,44,45) | unchanged |

The two `--gi-debug` gates holding EXACTLY is the load-bearing part of this baseline, not a
convenience. The debug view raymarches the radiance volume, and the voxelisation pass injects
`averDiffuseAlbedo()` — which is `s.albedo`, never `s.kdAlbedo`. A `kd` change that had moved those
two would have meant it reached somewhere it has no business reaching. Equally, the cast-shadow
gates moving by only a green/blue code is what a shadowed pixel should do: it receives ambient and
bounce but no direct sun, so it sees the smallest share of the correction.

PBR steps 16, 17(a) and 17(b) were each announced as oracle-moving and each measured NEUTRAL at all
13 gates; see §PBR for why, and for the sunlit diagnostic probe that covers what these 13 cannot.

Compare the RAW CODES: a one-code move hides completely inside `%.2f`, which is why the probe line
prints both. All 13 were re-run after step 11 and again after step 12 with the **D3D12 debug layer
enabled**: zero CORRUPTION, zero ERROR. The only messages are two pre-existing benign warnings,
`#820 CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE` and `#1328 CREATERESOURCE_STATE_IGNORED`. Zero
new TDRs across the whole exercise.

### Barrier note from step 11

Voxi's radiance volume now leaves `ShaderResource` immediately before the resolve rather than at the
top of `voxelizePass`. Nothing earlier writes it — `CSClear` zeroes the accumulator and `PSVoxel`
only adds to it — so taking it early left the injection pass's own whole-chain SRV pointing at a
resource in `UnorderedAccess`. Legal only because that descriptor is never read, which is not a
property worth relying on.

### RESOLVED 2026-07-20: the GI probe wobble was a last-writer-wins race in voxel injection

`PSVoxel` ended with `gVoxelUAV[c] = float4(radiance, 1.0)` — a plain unordered UAV store. Several
fragments legitimately cover one voxel, and D3D12 promises nothing about which of them retires last,
so the volume was rebuilt to a *different* answer on most frames. Injection now sums into an atomic
fixed-point accumulator and a `CSResolve` pass divides by the fragment count, which depends only on
WHICH fragments covered a voxel and not on their order.

**How it was localised.** Four experiments, each per-frame rather than per-run (the value changes
every few frames *inside* one process, so sampling once per run at frame 37 only ever showed a
lottery, which is what made this look like a between-run problem):

| Experiment | Result | Conclusion |
|---|---|---|
| Inject for 8 frames, then freeze the volume and keep cone tracing | 108/108 frames bit-identical | the cone trace is **not** the cause |
| Freeze mip 0, re-run the mip filter every frame | 108/108 bit-identical | `CSMip` is **not** the cause |
| Skip `CSClear` entirely | still wobbles, same amplitude | the clear/inject barrier is **not** the cause |
| Inject the ground quad only | 108/108 bit-identical | a single flat mesh never contests |
| Inject the cube only | 98x `95`, 10x `98` (8-bit red) | the cube contests **with itself** |
| Full scene, conservative raster off | still wobbles | not a conservative-raster artefact |
| Full scene, `CullMode::Back` | 108/108 bit-identical | it is front-vs-back-face fragments racing |

The cube contests on two counts: its bottom face is exactly coplanar with the ground quad at `z=0`
(both truncate to the same voxel layer), and because voxelisation runs with `CullMode::None` and no
depth test, differently-lit faces meeting at every box edge land in the same cell. Back-face culling
made it deterministic but is NOT a fix — `GSVoxel` projects each triangle along its own dominant
axis, so culling silently discards half the geometry.

**"Red channel only" was a measurement artefact and should not be reasoned from again.** The probe
reads an 8-bit UNORM backbuffer and printed `%.2f`. Green moved too — 1 code, `90`↔`89`, in exact
lockstep with red's deepest excursions — and it was invisible at two decimals. Blue's excursion
never crossed half a code. Red simply had ~3x the amplitude because the only bounce source in the
scene is an orange cube. The probe line now prints the raw codes for exactly this reason.

**Verification.** All 13 oracle gates, 20 runs each (260 runs), every gate identical at the raw
8-bit code; plus 100 further runs of the three GI gates and 108 consecutive frames sampled inside
single runs of `--gi`, `--ms --rt --gi` and `--gi-debug`. `0x141` `LiveKernelEvent` count unchanged
across all 360 runs of the fixed binary.

**Cost.** One extra `res*4 x res x res` R32_UINT volume (32 MB at 128³) and one extra compute
dispatch per frame. Two oracle values moved, both in the GI path — see the Oracle block above.

### RESOLVED 2026-07-20: the GI "TDR" was an unbound root CBV, not a slow geometry shader

Both GI defects — `--gi` hanging the GPU, and `--ms --gi` injecting almost nothing — were one bug
with two faces. Fixed in `D3D12RenderContext::setPipeline`.

**What was actually wrong.** `SetGraphicsRootSignature`/`SetComputeRootSignature` discard every root
argument. The root-signature cache declares all `kMaxConstantSlots` as root parameters whether or not
the bound shader reads them, and slot 0 (register `b0`, the engine `PerFrame` block: `gViewProj`,
`gCamPos`, `gLightDir`, `gLightColor`, `gAmbient`, `gSky*`) is a root CBV that no feature owns or
could supply. The backend's own draw path bound it in `bindGraphicsRoot`; the generic context path
never did. `VoxiRenderer` sets only `b1` and `b4`, so every feature pipeline drew with `b0` pointing
nowhere — undefined behaviour the debug layer cannot see, because it is not API misuse.

The two paths then diverged under the same UB:
- **IA + GS voxelise** faulted on it outright. `DXGI_ERROR_DEVICE_HUNG` (`0x887A0006`) ->
  `LiveKernelEvent` `0x141`, `amdkmdag.sys`. The process still exits 0 and the probe prints nothing,
  because the capture's command list died in the reset.
- **Mesh-shader voxelise** survived it and merely read zeros. `PSVoxel` then wrote
  `float4(0,0,0,1)` into every voxel it covered: full occupancy, zero radiance. So the volume was not
  EMPTY, it was fully OPAQUE AND BLACK — which is why `--gi-debug` read `0.03,0.05,0.07` (the
  raymarch's alpha saturates and the sky is occluded) rather than showing sky.

Why one pipeline faults and the other tolerates it is not established; the mesh root signature has
three extra parameters, so the stale root-argument layout differs. It does not need to be, because
the unbound CBV is the defect either way.

**The fix.** `setPipeline` now gives EVERY root CBV the pipeline declares a valid address before the
feature records anything: slot 0 gets the frame constant buffer, the rest get a shared zero-filled
256-byte buffer that `setConstants`/`setConstantBuffer` overwrite as normal. It is done backend-side
precisely so no feature can forget it. The invariant is stated on `PipelineLayout::constantDwords`
and `kEngineFrameConstantRegister` in `RHIResources.hpp`.

An earlier revision of this fix instead LOGGED unwritten root CBVs at draw time. That was discarded:
because the cache declares all five slots unconditionally, it fired on every pipeline that does not
use all of them (the mip-filter compute reports `b1`,`b2`,`b4` every dispatch). Pure noise. Binding a
safe default makes the condition impossible instead of reporting it.

**The geometry-shader path is KEPT.** The prior diagnosis in this document — that GS emulation on
GCN/RDNA was too slow and blew the TDR window — was measurably WRONG, and it sent two debugging
attempts down the wrong path. It was disproved directly: dropping the volume to 32³ (16x fewer raster
targets) still hung, and 512³ on the mesh path completed in 2.0 s. The hang threshold did not move
with workload, so it was a fault, not a timeout. With `b0` bound the GS path runs clean and returns
the exact oracle.

**Verification (2026-07-20, RX 7800 XT, driver 32.0.23027.2005).** 20 consecutive GI runs
(`--gi`, `--ms --gi`, `--gi-debug`, `--ms --gi-debug` x5) with the `0x141` `LiveKernelEvent` count
read before and after: **65 -> 65, zero new TDRs.** Every oracle value restored on both paths,
including `--gi-debug` `0.19,0.15,0.17` and the cast-shadow probe `0.26,0.31,0.38` under `--gi`.

**Not the cause, so do not re-investigate:** `shadowFactor` over-occluding, a broken light matrix, an
inverted depth compare, or an uninitialised shadow depth texture. `VSShadow` reads only `b1` and
`b4`, both of which were always bound; the shadow pass was correct throughout. Also ruled out:
conservative rasterisation, the voxel clear/barrier sequence, `drawsPrev_` being empty on the probe
frame, and driver/adapter/reboot state.

Keep counting TDRs around any automated render-test loop regardless:
`Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1001}` filtered for `LiveKernelEvent`
and `141`, before and after the batch.

### RESOLVED 2026-07-20: `--rt` sun shadows stopped occluding — the TLAS went to the wrong heap

`--rt --probe 1413 1042` read `0.43,0.46,0.52` (fully lit) where the shadow-mapped path read
`0.25,0.31,0.40`. Every other oracle value, `--rt` included, was correct, so it was not a pipeline
failure — only the ray missed.

**Cause.** `08cf5be` — a commit whose message says "docs" but which carries 256 lines of code —
moved `bindGiTables()` from the backend's `giHeap_` onto the registered feature's binding set. That
is the binding-set half of step 7, and it landed WITHOUT step 9. `buildRtScene()` kept writing the
TLAS SRV into `giHeap_` slot 2, which nothing has bound for graphics since. The shader therefore
read Voxi's t2, which `nullFill()` had written as an acceleration structure at address zero and
which nothing ever overwrote — `setSrvTlas` is implemented but has no caller anywhere in the tree.
**RayQuery against a null AS reports "no hit"**, i.e. `sunVis == 1.0`, i.e. fully lit.

**Fix.** `buildRtScene()` now calls `publishTlasSrv()`, which writes the AS SRV into the set
`bindGiTables()` actually binds, checking `srvKinds[2]` rather than assuming the slot's dimension,
and skipping the write unless the set or the address moved (a shader-visible descriptor must not be
rewritten under an in-flight frame). The dead `giHeap_` slot-2 write is gone.

**This is NOT the `e461425` latent-UB class**, despite looking like it. It is a plain regression
with a clean bisect: correct at `937cece`, wrong from `08cf5be`. The reboot and the ~30 preceding
TDRs were red herrings — the value is deterministic, not memory-dependent. The real lesson is the
process one: **a commit labelled `docs:` contained the regression**, so the message was no guide to
where to look.

**Diagnostic that settled it in one build:** force `rtShadow()` to `return 0.0`. The probe went to
`0.25,0.31,0.40` on both `--rt` and `--ms --rt`, which proves the RayQuery pipeline *is* the one
drawing that pixel and the ray *is* being traced — so the fault had to be the AS or its binding,
not pipeline selection. Note the centre probe cannot see any of this: it lands on the cube's unlit
left face where `ndl` is ~0 and the sun term drops out entirely.

**Verification.** All 13 oracle gates pass; `0x141` `LiveKernelEvent` count 65 -> 65 across the
batch, zero new TDRs. The red-channel wobble below still reproduces (`--gi` 5 runs: 3x `0.38`,
1x `0.37`, 1x `0.36`), unchanged by this fix, confirming it really is a separate defect.

## 4e. Aver.Render.PBR — the material system, COMPLETE

**"PBR is a material system. Voxi is the thing that renders it."** That sentence is a link line, not
a slogan, and it is the whole design:

| target | kind | links | owns |
|---|---|---|---|
| `Aver.Render.PBR` | **SHARED** | `Aver.Core` ONLY | `MaterialDesc`, `MaterialLibrary`, `pbr_abi.h` |
| `Aver.Render.PBR.Materials` | STATIC | `Aver.Core`, `Aver.RHI` (**never** `Aver.RHI.D3D12`) | texture caches, fallback textures, per-material binding sets, the b2 block, `materialShaderPrelude()` |

`Aver.Render.PBR` must never include `aver/rhi/*` or the C# P/Invoke boundary breaks — the DLL is
what the scripting layer binds to, and a render-hardware type on that boundary cannot be marshalled.
Verified at the link line and by grep: the only `aver/rhi` and `D3D12` matches under `modules/render.pbr/`
are the comments forbidding them.

**PBR is NOT an `rhi::IRenderFeature`, and will not become one.** A material is not a pass. Voxi
stays the only registered feature; it gained a link edge to `Aver.Render.PBR.Materials` and composes
its shader as `rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + kVoxiHLSL`. Voxi supplies
visibility and irradiance and calls the shading model; the material owns the BRDF.

### What the shading model does now
Five maps sampled through one wrap + 8x-anisotropic sampler at `s2`, factors multiplying maps per
glTF, occlusion and emissive honoured, alpha-mask clipped inside the material, and a tangent frame
solved from `ddx`/`ddy`. **sRGB is done by the texture unit**: base colour and emissive get sRGB
VIEW formats, normal/metal-rough/occlusion stay UNORM, and `packMaterial()` decodes the authored
base-colour constant on the CPU. Decoding after `Sample()` would be wrong — filtering and mip
averaging have already happened in encoded space, which reads as slightly dark, slightly desaturated
midtones that worsen with distance and that nobody ever reports as a bug.

### Register map in force
`t0` volume, `t1` shadow, `t2` TLAS (table 0); `t3..t7` base colour / metal-rough / normal /
occlusion / emissive (table 1); `t8`/`t9` mesh-shader vertices and indices. `b0` engine frame,
`b1` object (32 dwords), `b2` material, `b4` Voxi frame, `b5` mesh triangle count. `s0` linear
clamp, `s1` comparison, `s2` material. The sampler array caps at four, so one spare remains.

### The per-material path is live, and that was proved rather than assumed
Before step 18 nothing had ever called `MaterialLibrary::create()`, so `bindingSet()`, `constants()`,
`entryFor()` and the dirty drain had never had a material to act on. The sandbox now creates one per
actor and applies it immediately before each `drawMesh`. Step 18 is oracle-NEUTRAL, and the reason
that is strong evidence rather than weak is the negative test: the actor's `metallic`/`roughness` are
pinned to the identity `1` in `b1` and the authored values live only in the material's factors, so
**disabling the `setDrawBinding` call collapses the image from `raw(90,93,108)` to `raw(65,77,92)`**
(the floor renders as a rough mirror). Bit-identical output with the bind on is therefore proof that
`b2` is being consumed, not proof that nothing happened.

Base colour deliberately stays in `b1`. `rhi::IRenderFeature::submitDraw` carries no material, so
Voxi's shadow and voxelisation loops bind the FALLBACK set; neutralising `b1`'s colour would inject
white albedo into the radiance volume and turn every bounce white. Base colour moves the day
`submitDraw` carries a material and not before.

### `AVER_MODULE_PBR=OFF` baseline
Voxi renders materials and cannot build without them, so `CMakeLists.txt` now forces
`AVER_MODULE_VOXI=OFF` with a status message when PBR is off. Previously this combination failed
with a `C1083` on a header four includes deep, which reads as a broken include path rather than as a
module combination that was never valid. The resulting no-feature build renders through the frozen
`PSMainPlain`:

```
--frames 40                   -> (0.34,0.36,0.42) raw(87,92,107)
--frames 40 --ms              -> (0.34,0.36,0.42) raw(87,92,107)
--frames 40 --probe 1413 1042 -> (0.43,0.46,0.52) raw(110,117,132)
```

`raw(87,92,107)` is the PRE-17(c) value, which is exactly right: the frozen path is untouched by
every BRDF correction in this phase, which is what "FROZEN, do not evolve" is for. The cast-shadow
pixel is fully lit because without Voxi there is no shadow map and no ray tracing at all. Both
values were confirmed **bit-identical to the same configuration built at `192fd55`**, before any of
this work, so the no-material path is provably unaffected. Note §4c-2 records `raw(109,117,132)` for
this pixel; that figure is stale and predates this phase — it was `110` before these commits too.

### Known gap
`MaterialLibrary::status()` now reports `Ready` for the factors, all five maps and the alpha mask —
but they only reach the LIT pass. The voxelisation pass shades with the fallback material, so an
authored base-colour map does not yet colour the GI bounce. Closing that is the `submitDraw` change
above. `AlphaBlend` stays `NotImplemented` on purpose: blending needs a pipeline blend state and a
back-to-front sort, both of which are the renderer's, not the shading model's.

## 4d. NOT DONE — open work, roughly in value order

**Closed since this list was written**
- **The Voxi/HAL decoupling refactor, all 12 steps** (`14284b4`..`0bba4c2`). The backend no longer
  contains any GI, shadow or ray-tracing code. Full write-up, final architecture and the
  `AVER_MODULE_VOXI=OFF` baseline in §4c-2.
- **The GI GPU hang, and the near-empty mesh-path volume.** One cause: the engine `PerFrame` block at
  `b0` was never bound for feature pipelines, so every Voxi draw ran against an unset root CBV. The
  backend now binds every declared root CBV in `setPipeline`. Full write-up in section 4c-2; 20 GI
  runs, zero new TDRs, all oracle values restored on both the GS and mesh paths.
- **Stale voxels** (`98d7406`). `CSClear` zeroes mip 0 before injection each frame; coarser mips
  are fully overwritten by `CSMip` so they need nothing. Verified by A/B on a moving cube under
  `--gi-debug`: with the clear off the cube's radiance stays frozen where it started
  (`0.19,0.15,0.17`, identical to the static scene); with it on the ray reaches the background
  (`0.27,0.28,0.33`). Static-scene GI unchanged at `0.38,0.35,0.40`.
- **Mesh shader path + GS-free voxelisation.** `MSMain` replaces the input-assembler vertex path;
  `MSVoxel` replaces `VSVoxel`+`GSVoxel`, so voxelisation no longer needs a geometry shader — the
  point of the exercise, since GS is emulated on every AMD GCN part. `Feature::MeshShaders` now
  reports `Ready`. Toggle: Project Settings ▸ Rendering ▸ Use mesh shaders, or `--ms`. Verified
  pixel-identical to the IA path (lit `0.34,0.36,0.42`; voxel volume `0.19,0.15,0.17` on both),
  across MSAA 1x/4x/8x rebuilds, and over a 300-frame soak.
- **Mesh shaders + RayQuery together.** The shader-compiler wrapper now takes a semicolon-separated
  define list, so `PSMain` builds with both `AVER_MS` and `AVER_RT` and gets its own PSO — the two
  settings are no longer mutually exclusive. Proven live, not merely built: with `rtShadow` forced
  to `1.0`, a pixel inside the cube's cast shadow goes from `0.25,0.31,0.40` (shadowed) to
  `0.43,0.46,0.52` on the MS path, matching the IA path's `0.43,0.46,0.51` to within 1/255.
  Note the viewport-centre probe CANNOT see the sun term — it lands on the cube's unlit left face
  where `ndl` is ~0, so any shadow A/B must probe a sunlit or cast-shadow pixel instead.

- **Textured PBR, the material system, and the BRDF corrections** (`f13553c`..`274848a` and step 18).
  Full write-up in §4e. The vertex format landed at 32 bytes and the tangent frame is derived in the
  pixel shader from `ddx`/`ddy` of world position and UV, exactly as this list argued it should be:
  nothing authored carries tangents, MikkTSpace reindexes the mesh so it belongs in an importer that
  does not exist, and an unfilled tangent through `normalize()` is the NaN that makes `traceCone`
  march forever — the documented `0x141` TDR mode. `averPerturbNormal()` falls back to the geometric
  normal when the UV gradient is degenerate, which is the same hazard answered at the other end.

**Renderer**
1. **No oracle gate covers the sun's SPECULAR term.** Found while landing PBR step 17(a): the
   centre probe sits on the cube's unlit left face (`ndl` ~0) and `(1413,1042)` sits inside the cast
   shadow (`visibility` ~0), so neither pixel receives direct specular at all. A change to the whole
   masking-shadowing formulation left all 13 gates bit-identical. `(2200,1400)` is a sunlit floor
   pixel that does see it — `raw(103,111,126)` at present — and something in that family should
   become a 14th gate. Until it is, BRDF work has no automated cover.
2. **RT ambient occlusion / reflections.** The TLAS already exists, so this is mostly shader work.
3. **Path tracing.** Declared only; would reuse the same acceleration structure.
4. **No temporal accumulation** on GI. With the volume rebuilt each frame this is the main remaining
   source of GI instability now that the injection race is fixed.
5. GI is a **single volume**, not cascaded — large scenes will not fit at useful resolution.
6. Shadow map is **one cascade** at 2048²; no CSM, so large scenes get coarse shadows.
7. Specular GI is not cone traced (diffuse + AO only).
8. **Acceleration structures cannot be destroyed through the generic RHI.** `IResourceFactory` has
   `createBlas`/`createTlas` and no matching destroy, so they are released only with the factory.
   Harmless while meshes are static; it needs an answer before geometry becomes dynamic.

**Portability (asked for explicitly: all AMD + NVIDIA DX12 GPUs)**
9. **Only ever run on one GPU (RX 7800 XT).** The DXR/GI/mesh paths are capability-gated and fall
    back, but have NOT been exercised on NVIDIA or Intel, nor on Resource-Binding-Tier-1 hardware.
    Test before shipping. The Tier 1 rules the code follows (every declared table bound on every
    pass, every heap slot null-filled by declared kind, every declared root CBV given an address)
    are therefore reasoned, not measured.
10. D3D11 and Vulkan backends are still **stubs** — D3D12 is the only working backend, so
    "supports DirectX 12" is a hard requirement. Both decline cleanly: `createDevice` falls through
    to Null, `resources()` is null, and `VoxiRenderer::init` logs and returns false.

**Scripting**
11. **C# cannot drive the live editor.** A standalone C# process P/Invokes its own copy of
    `Aver.Render.Voxi.dll`, so it gets its own settings and empty device caps. Needs in-process
    CLR hosting (hostfxr/CoreCLR). The C ABI is already shaped for it.
    This is also why **Tools ▸ New C# Script / New C# Class produce files that never execute**.
    The editor says so in the modals and in the generated file headers rather than letting someone
    find out by watching a script do nothing. **Tools ▸ Compile Scripts** genuinely builds them —
    `dotnet build` on the generated `Scripts.csproj`, with the full transcript surfaced in a
    scrollable modal — so the compile half is real and only execution is missing.
12. Same caveat blocks the **launcher hardware probe** in `docs/MINIMUM_SPECS.md` §7.

**Editor / engine**
13. Dock layout does **not persist** (`io.IniFilename` is null) — rebuilt from DockBuilder each run.
14. Output Log does not capture the real log. The Content Browser now reports the loaded project
    and where its `Content/` is mounted, but still **enumerates nothing** — it needs the asset
    pipeline (§9) before it can list files.
14b. `.ocproject` is **read-only** in the editor: New Project writes one, but nothing writes an
    existing manifest back, so Project Settings ▸ Description is a display. `STARTMAP` is
    recorded and shown but not acted on — there is no scene load yet (item 16).
15. Toolbar Save / Play / Pause / Stop are **non-functional stubs**.
16. No scene save/load, no `.ocmesh`, no asset import — the general-purpose roadmap in §9 is
    otherwise untouched.
17. `modules/abi` is still an empty skeleton (the C ABI lives in the Voxi module instead).

**Decisions taken (do not re-litigate without reason)**
- Ray tracing targets **DXR 1.1 inline RayQuery only**; DXR 1.0 would add only GPUs that emulate
  it without RT cores. Gate on `RaytracingTier >= 1.1`, NOT on feature level 12_2.
- Engine floor stays **FL 11_0**; "D3D12 Ultimate only" is a spec/marketing decision in
  `docs/MINIMUM_SPECS.md`, not something baked into the renderer.
- Transform tools live in the **viewport overlay bar** (as in Unreal), not the window toolbar.
- Render settings are **project-wide** → Edit ▸ Project Settings ▸ Rendering, not the Details panel.

## 5. Formats — implemented loaders

- `.ocbeam` (Aver.Formats/OcBeam): faithful to OCCompiler Main.java + VehicleDamage.cpp —
  MATERIAL (accept **10–13 fields**, Behavior at index 9), NODE/BEAM/PANEL/PART, skips
  GLB/rig/collision, applies SCALE/NORMALIZE; **added OBJECTID**. Tested on Ferrari (10-field)
  + Puegot (13-field).
- `.ocmap` (Aver.Formats/OcMap): faithful to OcMap.cs — identity/surface/env + PLACE/DEFORM;
  **positions as f64**; per-placement ObjectId. Verified `fnv1a64("demoworld")=0x376B85BC4D1A03BA`.
- `.ocproject` (Aver.Formats/OcProject): the project manifest from `docs/PROJECTS.md` — NAME,
  ENGINE, CONTENT, STARTMAP, AUTHOR, plus the manifest's own absolute directory. Unknown keys are
  ignored (the format is forward-compatible by contract), a leading UTF-8 BOM is stripped, and
  `ENGINE <name> <minVersion>` is **enforced** against `aver/core/Version.hpp` — a project needing
  a newer build is refused with a message naming both versions instead of half-loading.
- `FormatTest.exe` golden-tests `.ocbeam`/`.ocmap` against real files (0 failures).
- Authoritative sources (NOT in repo): `C:\Users\User\Documents\Unreal Projects\OpenConstructor27`
  and `C:\Users\User\Documents\OpenConstructorSupportAssets`. Recon extracts in `docs/recon/`.

## 6. Branding

**The shipped marks are human-authored.** `branding/master-lockup.png` (1920x1080) is the project
owner's artwork and the single source of truth; every shipped slot is a crop or rescale of it, and
nothing is redrawn. Regenerate with `python scripts/brand.py` after changing the master; do not
hand-edit the derived files.

Shipped: `splash.png` (1200x520, startup splash, shown at native size) and `icon.ico` (full
16-256 size ladder, window/exe icon via `Sandbox.rc`), plus `logo.png` / `logo256.png` /
`icon512.png` as transparent marks. Transparency is keyed by flood-filling from the corners, not by
a global colour replace -- the cube's outline is near-black and close enough to the #262626 banner
that a global replace punches holes through it.

Claude's earlier vector concepts now live in `branding/ai-generated/` and are referenced by nothing.
They are kept only because the master is raster-only, so they are the sole vector sources. See
`branding/ASSETS.md` for the full provenance table, including front-facing visual work that is
AI-authored but is not a file (the editor theme, default dock layout, gizmo colours, procedural sky).

Fixed alongside: staging the splash was a `POST_BUILD` command, so it only ran when the exe
relinked -- editing the artwork alone shipped the old splash with the build reporting success. It is
now an `OUTPUT`/`DEPENDS` rule, and `Sandbox.rc` declares `OBJECT_DEPENDS` on the icon so replacing
it actually re-runs the resource compiler.

## 7. Commit history

The first twelve, for the origin story only — this list stopped being maintained at `541d888` and
`git log --oneline` is the authority. §4c and §4c-2 carry the commits that matter.

```
541d888 Editor: Unreal-style free-fly camera (no auto-orbit)
0d3e00f Editor depth: grid, transform gizmo, click-pick, wireframe
0a63dd7 Renderer: PBR shading + 4x MSAA
6b1ac22 Remove "royalty-free" phrasing
3ca2af7 Phase 3.6: Unreal-style editor, default sky scene, window icon + splash
c51fd7e Brand: export PNG/ICO assets
2168df6 Brand: contain the A within the cube; geometric wordmark font
b3ef680 Brand: slant the AE to follow the isometric cube faces
e6e97ad Brand: refined Isocube AE logo + wordmark; editor design spec
a5e12cf Phase 3.5: in-window editor UI (Dear ImGui) + AE logo
9878925 Phase 3: functional shaded 3D rendering (D3D12)
ab2264a Aver Engine foundation: modular core + .oc* format loaders
```

## 8. Gotchas / do-not-regress

- Draws MUST be recorded in `Application::onRender` (after `beginFrame`), not `onUpdate`
  (beginFrame resets the command list).
- `.ocbeam` MATERIAL = 10–13 fields (Behavior at index 9); strict-10 drops newer files.
- D3D12 readback `Map` uses a nullptr read-range (RowPitch*height > footprint total when
  width isn't 256-aligned → E_INVALIDARG).
- ImGui 1.92 DX12 needs the `InitInfo` path with `CommandQueue` (font upload); forward-declare
  `ImGui_ImplWin32_WndProcHandler`; link `dwmapi imm32`.
- MSAA color AND depth must share the sample count; ImGui renders 1x on the resolved backbuffer.
- Editor camera is free-fly, NOT orbit — do not reintroduce auto-orbit.
- Process must stay per-monitor DPI aware (Win32Window `enableDpiAwareness`); window sizing
  is DPI-scaled + work-area-clamped. Don't create the window before awareness is set, and
  don't feed logical coords to the swapchain (window width/height are physical pixels).
- **Modal-loop rendering**: a window move/size/maximise runs the OS's own message pump inside
  `DefWindowProc`, starving the engine loop and freezing the viewport. `Engine::frameStep()`
  (the per-frame body, re-entrancy-guarded by `inFrame_`) is driven from a `WM_TIMER` set
  between `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE`, via `Window::setRenderTick`. Rules learned the
  hard way (do NOT regress):
  - **Never render (Present/ResizeBuffers) from inside a window-state-change message** (the
    `WM_SIZE` a maximise sends) — deadlocks the DWM. Let the main loop pick up the new size.
  - **Never Present during an active resize modal loop** — also a DWM deadlock. Detect a resize
    grab from the `WM_NCLBUTTONDOWN` hit-test (`Window::isResizeGrab()`) and start the render
    timer only for moves. A resize freezes-but-recovers (updates on release); a move renders live.
  - **Skip `ResizeBuffers` while `inModalSize()`**; resize once the drag ends.
  - **`break` inside the WndProc switch does NOT reach `default:`** — it exits the switch. A case
    that `break`s must still end at `DefWindowProcW` (there's now a trailing call after the
    switch). Swallowing `WM_NCLBUTTONDOWN` this way killed move/resize/maximise/**close**,
    because every caption/border interaction is driven by DefWindowProc from that message.
- **D3D12 frame sync = wait-before-reuse** (D3D12Device): one monotonic `nextFence_`, signalled
  after each Present; `fenceValues_[backbuffer]` records that frame's value; `beginFrame`
  reacquires `GetCurrentBackBufferIndex()` and waits for that buffer's fence. The old
  frame-buffering pattern assumed the backbuffer index alternates 0,1,0,1 — `ResizeBuffers`
  resets it to 0, desyncing the fence so the wait targeted a never-signalled value =>
  intermittent post-maximise/resize deadlock. Do NOT reintroduce a parity-based fence scheme.
  Waits are bounded (5 s) and log `GetDeviceRemovedReason` instead of hanging forever on a TDR.
- Gizmos render via a no-depth overlay line PSO (`setLineDepth(false)`); toggle depth back on
  after so the grid still occludes correctly. Line meshes are prebuilt in `onInit` (never
  per-frame — `createLineMesh` never frees).
- Gizmo manipulation is drag-anywhere-on-handle (screen-projection), not full 3D handle
  raycast. Rotate increments a world-axis Euler component (exact only when other Euler
  components are 0) — acceptable for now; revisit with quaternion objects.
- **Dockspace/viewport coupling**: the 3D scene is scissored into the dockspace's *central node*
  via `IDevice::setViewportRect` (D3D12 sets viewport+scissor from it). Consequences that MUST
  hold together: camera aspect, `project()`, `pick()` NDC and `applyMove`'s world-per-pixel all
  use the viewport rect (not the window); the colour clear is unconditional and full-surface
  (a scissored sky no longer covers every pixel); input is gated on `inViewport()` as well as
  `WantCaptureMouse` (the central node is a transparent hole, so capture is false over it).
- The dock layout is built once with DockBuilder because `io.IniFilename` is null (nothing
  persists). `DockBuilderSetNodeSize` asserts on a zero size — take the size from the host
  geometry, NOT `GetContentRegionAvail()` after `DockSpace()` (which reads 0).
- The default ImGui font only rasterises Latin-1: glyphs like ▾ (U+25BE) render as `?`. Use the
  `dropButton()` helper (draws a real triangle) rather than typing them.
- Gizmos draw as 1px lines (D3D12 has no wide lines); at hi-DPI they're thin, so `pickAxis`
  uses a generous grab tolerance (16px logical for axes/rings, 13px for the centre handle,
  scaled by dpi). A future thick-gizmo pass would build them from triangles.
- Rotate follow-cursor sign is `-sign(axis·camForward)` (screen-y-down handedness). If a
  ring ever feels reversed, that one factor is the knob.
- `.rc` needs `enable_language(RC)`; RC path is relative to the .rc file.
- Line endings: Git warns LF→CRLF (harmless).
- **No feature vocabulary in the generic RHI headers, and no backend type in a feature.** The second
  half is enforced by the link line (`Aver.Render.Voxi.Renderer` links `Aver.RHI`, never
  `Aver.RHI.D3D12`); the first half is not enforced by anything but review. Do NOT reintroduce a
  `setGi`-shaped call on `IDevice` — the app owns the feature instance and configures it directly.
- **`rhi::MeshVertex` is described to D3D12 in exactly two places**: `kMeshInputLayout` in
  `D3D12Device.cpp` (the input assembler) and `MeshVtx` in `sharedShaderPrelude()` (the
  mesh-shader path). They must change together; the prelude is a cross-module ABI with no compiler
  behind it, so a mismatch is silent.
- **Backend root parameter indices are named constants** (`kSceneFrameParam`, `kMeshVertexParam`,
  …). `SetGraphicsRoot*` takes a bare integer, so a stale number binds the WRONG parameter rather
  than failing. Never inline them again.
- **A pixel probe is not a screenshot.** `--probe` reads one pixel and misses overlays entirely; the
  centre probe in particular is blind to the sun term (see §4c-2). Shadow and ray-tracing checks
  must use `--probe 1413 1042` (cast shadow) or `--probe 1413 1150` (penumbra).

## 9. Next steps — including the general-purpose direction

**Make the engine general-purpose (new goal):** keep the generic core; treat OpenConstructor
(cages/vehicle/aero/`.ocbeam`) as an OPTIONAL game plugin, not the engine's identity.
Concrete steps:
1. **Generic scene layer** — implement `Aver.Scene` (data-oriented entities/components:
   transform, hierarchy, mesh-renderer, light) and move the sandbox's ad-hoc object list onto it.
2. **Primitives + generic mesh** — engine-provided cube/sphere/plane/cylinder; a real
   `.ocmesh` static-mesh format + loader (design in `docs/formats/FORMAT_SPECS.md`).
3. **Generic asset import** — Rust `aver-assetc` (or a C++ path): glTF/OBJ → `.ocmesh/.octex/.ocmat`.
4. **Scene save/load** — `.ocworld`/`.ocmap` write + load for arbitrary content (not just cages).
5. **Materials/textures** — `.ocmat` + `.octex`, texture sampling in the shader (currently
   flat PBR params only).
6. **Move OpenConstructor bits behind modules** — `Aver.SoftBody/Aver.Vehicle/Aver.Aero`
   as opt-in; `.ocbeam` load stays in `Aver.Formats` but the editor treats it as one importer.
7. **Editor generalization** — "Add" menu (spawn primitives/lights), open/save scene, an
   asset browser; not hardcode the OC default scene.

**Other queued work:** grab-able gizmo handles; dockable panels (ImGui DockBuilder);
shadow maps; then Phase 4 soft-body (make the cage deform), Phase 6 net (needs OCServer
Wire.cs), Phase 7 C# editor + Rust pipeline.

**Immediate (once VS finishes updating):** rebuild to confirm green, then start §9.1
(generic scene layer) or §9.2 (primitives + `.ocmesh`).
