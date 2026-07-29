# Aver Engine — Status & Handoff

Living record of where the engine stands and what's next. Updated 2026-07-28 at `d04fe1a`
(195 commits). `git log --oneline | wc -l` and `git rev-parse HEAD` are the authority. The phase
recorded here gave a game its own UI, its own audio and its own materials — three C seams and the
modules behind them — and opened an asset editor for actors (§4o–§4s).

**This document has a hole in it, and reading it as complete will mislead you.** §4n was written at
`d8fc062`; §4o picks up at `083d687`. The **twenty** commits in between — `Aver.Scene`,
`Aver.Framework`, `Aver.Physics`, `.ocmesh`/`.ocskel`/`.ocanim`, the JSON reader, the glTF importer,
the asset-editor shell, `.ocproject` shell association — landed and are covered by
`docs/SCENE_FRAMEWORK.md`, `docs/ABI.md` and `docs/SCRIPTING_API.md`, but **were never written up
here**. Several statements below still describe the tree as it was before them; the ones found while
writing §4o–§4s are corrected in place and say so, and the rest have not been audited. Treat any
claim in §4–§4n about what does *not* exist as suspect until checked against the tree.

Read this first after a context compaction, then `docs/ARCHITECTURE.md` (module DAG),
`docs/ABI.md` (every C seam, export by export), `docs/SCENE_FRAMEWORK.md` (scene + gameplay),
`docs/MINIMUM_SPECS.md` (hardware requirements / launcher spec),
`docs/formats/DECISIONS.md` (formats), `docs/PROJECTS.md` (engine⟂project),
`docs/AUDIO.md` (the audio plan §4p implements) and `docs/ACTOR_EDITOR.md` +
`docs/DESIGNER_REWRITE.md` (the actor editor and the region-rewrite grammar, §4r).

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
./scripts/build.ps1 -Release        # the SAME, in Release, into a SEPARATE tree (§4i)
./scripts/run.ps1                   # build then launch the editor (Sandbox.exe)
./scripts/run.ps1 --frames 30 --screenshot out.png   # headless-ish capture for verification
./scripts/run.ps1 <path.ocbeam>     # also load a cage into the scene
./scripts/gates.ps1                 # the oracle: 9 device configurations x 17 gates (§4h)
./scripts/gates.ps1 -Config baseline # just the primary path, ~30 s
./scripts/gates.ps1 -Release        # the Release binary against its OWN baseline (§4i)
```
Output: `build/bin/Sandbox.exe`, and `build-release/bin/Sandbox.exe` for `-Release`. **The two trees
coexist and neither clobbers the other**, because `scripts/gates.baseline.txt` is a Debug measurement
and `scripts/gates.baseline.release.txt` is the Release one, so both binaries have to exist at once
for either baseline to mean anything. `scripts/build.bat` is the real build (PowerShell wraps it);
run `.bat` via the PowerShell tool, not Git Bash (`cmd //c` mangling). Add `/Zc:__cplusplus`
already set. Vulkan: `-DAVER_RHI_VULKAN=ON` once the LunarG SDK is installed.

## 2. Repo layout (key paths)

```
CMakeLists.txt              top-level (LANGUAGES C CXX RC; options AVER_RHI_*, AVER_ENABLE_UI)
cmake/AvModule.cmake        aver_add_module() helper
scripts/                    build.bat / build.ps1 / run.ps1 / gates.ps1
                            + gates.baseline.txt (Debug) and gates.baseline.release.txt (Release)
modules/
  core/      Aver.Core      types, Math (Vec/Mat/Quat/Transform/AABB + Mat4::inverse), Log, Time, Hash(fnv1a64)
  platform/  Aver.Platform  Win32 Window (+icon, message hook), Splash (layered win + stb_image), FileSystem (+executableDir),
                            DirectoryWatcher (ReadDirectoryChangesW, debounced — modules/platform/README.md)
  assets/    Aver.Assets    ObjectId (fnv1a64), AssetType (+ Aver.Assets.Gpu: decoded mips -> texture)
  formats/   Aver.Formats   .ocbeam/.ocmap/.ocproject/.ocmesh/.ocskel/.ocanim/.ocworld, JSON, glTF
                             import, .ocaudio (Aver.Formats.Audio), .ocmat (Aver.Formats.Material),
                             MaterialScript + ActorScript (the C#-source rewriters, §4q/§4r)
  rhi/       Aver.RHI        IDevice/ISwapchain + the generic render-feature surface
                             (RHIResources.hpp) + shared shader prelude + Null backend + uiWndProc
  rhi.d3d12/ Aver.RHI.D3D12  THE backend (device, swapchain, MSAA, PBR, sky, lines, mesh-shader
                             path, ImGui host, capture, the generic factory/context)
  rhi.d3d11/ rhi.vulkan/     stubs
  render.voxi/               Aver.Render.Voxi (SHARED: settings + C ABI, Core only) and
                             Aver.Render.Voxi.Renderer (STATIC: GI/shadow/RayQuery, drives Aver.RHI)
  render.pbr/                Aver.Render.PBR (SHARED, Core only) + .Materials (STATIC, Aver.RHI) — §4e
  ui/        Aver.UI        STATIC, Core only. The retained game-UI draw list — §4o
  render.ui/ Aver.Render.UI STATIC, Aver.RHI. Turns a UiDrawList into draw calls — §4o
  ui.abi/    Aver.UI.Abi    SHARED. The 11-export C seam for a game's HUD — §4o
  audio/     Aver.Audio     STATIC, Core only. The mixer; no device — §4p
  audio.wasapi/ Aver.Audio.Wasapi  Windows only. The device (WASAPI shared mode) — §4p
  audio.abi/ Aver.Audio.Abi SHARED. The 24-export C seam for audio — §4p
  render.actorpreview/       Aver.Render.ActorPreview (STATIC, Aver.RHI + Aver.Formats): the actor
                             editor's own colour+depth target, pipeline, camera at b4 — §4r
  scene/ framework/ physics/ Aver.Scene, Aver.Framework, Aver.Physics — built, tested, and NOT
                             written up in this file (see the hole named at the top)
  physics.jolt/              Jolt 5.6.0 (MIT), VENDORED. The rigid-body backend, linked PRIVATE by
                             Aver.Physics and aliased Aver.Physics.Jolt. Was third_party/JoltPhysics
  scripting/ Aver.Scripting.Host (STATIC: in-process CLR host via nethost/hostfxr, Core+Platform,
                             never the RHI) + the managed bridge under scripting/csharp/
  runtime/   Aver.Runtime    Engine loop, Application, EntryPoint (splash + ImGui hooks)
  (skeleton, not yet wired: render, render.gi, softbody, aero, gpudeform,
   fracture, vehicle, net, netvehicle, match, world, abi — each has a README)
sandbox/     Sandbox.exe      the editor app (SandboxApp.cpp) + Sandbox.rc (icon), the asset-editor
                              shell (AssetEditor.*), ActorEditor.*, ProjectScaffold.*, ToolsMenu.*
tools/       ActorSweep.exe   opens every actor .cs in a real project and reports what parses (§4r)
tests/       formats/ scene/ framework/ physics/ ui/ render.ui/ audio/ render.actorpreview/
third_party/ imgui/ (docking, MIT) stb/ (stb_image + stb_image_write, PD) fonts/ (Roboto)
             Jolt is vendored too, but at modules/physics.jolt/ — it is the physics BACKEND
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
  | PROJECT — C# | New C# Script… | `<project>/Content/Scripts/<Name>.cs`, `: AverBehaviour` + hooks | no |
  | PROJECT — C# | New C# Class… | same folder, plain class, no hooks | no |
  | ENGINE — C++ | New C++ Module… | `modules/<name>/` (CMakeLists, README, include, src) | **yes** |
  | ENGINE — C++ | New C++ Class… | `.hpp`/`.cpp` in a picked `modules/<m>/` | **yes** |
  | — | Compile Scripts | `dotnet build -o <project>/Binaries/Scripts`, transcript in a modal | — |
  | — | Reload Scripts | the same build, then unload + reload in the running editor | — |
  | — | Open Project Folder | shell-out to Explorer | — |
  | — | Open Scripts In ▸ | whichever IDEs are installed; shell association always last | — |

  `.ocproject` carries no build integration, so **game C++ has nowhere project-side to live** and
  a new module goes into the ENGINE. The item labels, the group headers and the modals all say so.
  `Scripts.csproj` is now written **at project creation** with four engine references, not emitted by
  the first C# file with one — that changed in §4s, and the sentence that used to stand here described
  the older behaviour.
  Nothing overwrites an existing file; names are validated as identifiers; every project-dependent
  item is **disabled with an explaining tooltip** rather than hidden. The C# modals and the
  generated file headers now say that **the script DOES run**, and say what it can and cannot
  reach — the log and the render modules' live settings, never the scene. See §4f.

  **Compile errors are clickable.** `sandbox/src/IdeIntegration.*` locates the code editors that
  are actually on the machine — Visual Studio through `vswhere.exe`, VS Code and Rider through
  their install locations — and parses MSBuild's diagnostics out of the build transcript into
  file/line/column. Clicking one in the Compile Scripts modal opens that position: `code --goto
  <file>:<line>:<col>`, or `devenv /edit <file> /command "Edit.GoTo <line>"`. Detection runs once
  on a worker thread and is cached, so nothing in the frame loop ever waits on a process launch.
  A line the parser did not understand is still printed verbatim, and a machine where nothing is
  detected still gets the shell association it always had.

  Measured caveat on the Visual Studio jump: against an instance that is **already running** the
  caret lands exactly on the requested line. Against a **cold** devenv the file opens but the
  caret does not move, because `/command` runs before the document has loaded and devenv offers
  no command-line synchronisation to wait on.

  **Neither CMakeLists is auto-edited.** The top-level one is not, because wiring a module into
  the build is a deliberate act and every existing skeleton under `modules/` is deliberately
  unwired; a module's own is not, because `aver_add_module` takes an explicit `SOURCES` list and
  never globs. Both modals show the exact line to add and why it is manual.
- Dev/testing args: `--tool <select|move|rotate|scale>` opens straight into a tool; `--new-script`
  forces the New C# Script modal open, `--tools-menu` holds the Tools dropdown open,
  `--compile-scripts` fires one build, and `--reload-scripts [N]` fires one Reload Scripts N frames
  in (default 20) — all for screenshot verification, since headless
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
  A separate C# process gets its own copy of the DLL (own settings, empty caps), which is why
  scripting the live editor needs the CLR hosted in-process — that host now exists, see §4f.
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
`--project-settings`, `--new-script`, `--tools-menu`, `--compile-scripts`, `--reload-scripts [N]`,
`--start-screen`, `--msaa N`, `--gi`, `--no-gi`, `--gi-debug`, `--rt`, `--ms`, `--probe X Y`,
`--probe-rel U V` (§4n), `--scripts <dir>`, `--force-caps <list>`, `--warp` (§4g),
`--debug-layer` (§4i), `--no-vsync`, `--ui-demo` (§4o). From the phase this document never wrote up:
`--headless`, `--drawer`, `--play-test`, `--spawn-test`, `--bloom`, `--exposure`, `--auto-exposure`,
`--clouds`. That is the complete set as of `d04fe1a`, taken from the argument parser rather than from
this list, which had drifted.

Two more scaffold and **exit without touching a device**, so project creation is reachable from a
script: `--new-project <location> <name>` and `--upgrade-project <path.ocproject>` (§4s).

`--debug-layer` turns the D3D12 debug layer on. It is OFF by default in every build type, because it
validates every API call; `scripts/gates.ps1` passes it so the per-gate corruption/error/warning
counters still exist.

`--scripts <dir>` points the CLR host at a directory of user script assemblies (relative to the
executable unless absolute) and **overrides everything else**. Without it the host reads
`<project>\Binaries\Scripts` when a project is open, and `<exe>\Scripts` otherwise — which a clean
build does not create, so **no oracle gate loads a script** (no gate opens a project either).
`--scripts SampleScripts` picks up the staged sample behaviours.

**`./scripts/gates.ps1` runs the whole oracle** — 9 device configurations × 17 gates against
`scripts/gates.baseline.txt`, TDRs counted, launches spaced. `-Config baseline` is the 30-second
version. See §4h. Everything below describes the machinery it drives.

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

> These thirteen are still exactly right and still the primary path. They are now gates 1–13 of
> `scripts/gates.ps1`, which adds four more (penumbra ×2, sunlit ×2) and runs the set against nine
> device configurations. `scripts/gates.baseline.txt` is the machine-readable copy. See §4h.

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
new TDRs across the whole exercise. **Both warnings are fixed as of §4i** — the totals now read
`0 corruption, 0 error, 0 warning` — and the layer itself is opt-in behind `--debug-layer`, which the
gate runner passes. This paragraph is left as the historical record of what those runs reported.

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

Base colour still rides in `b1` in the sandbox's current scene — but no longer because it has to.
`rhi::IRenderFeature::submitDraw` now forwards the sticky per-draw binding (table 1 + the b2 block)
alongside the b1 data, so Voxi's shadow and voxelisation loops bind the SAME authored material set the
lit pass does rather than the fallback (see "Voxi consumes the material in the GI path" below). An
authored `baseColorFactor` or base-colour map therefore reaches the radiance volume through
`averDiffuseAlbedo` (view-independent by contract) and injects the surface's own colour. The sandbox
keeps base colour in `b1` only because it has not moved that authoring onto the material's factor —
a sandbox choice now, not a Voxi limitation.

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

### Voxi consumes the material in the GI path — the `submitDraw` gap is closed
`MaterialLibrary::status()` reports `Ready` for the factors, all five maps and the alpha mask, and
they now reach BOTH the lit pass AND the voxelisation pass. `rhi::IRenderFeature::submitDraw` was
widened to forward the sticky per-draw binding (binding table 1 + the b2 block) alongside the b1
block; the backend hands it its current `drawBinding_`, Voxi COPIES the b2 bytes per replayed draw
(the pointer is per-draw scratch, and the replay runs a frame behind), and the injection loop binds
that authored material instead of the fallback. So an authored `baseColorFactor`/base-colour map now
colours the GI bounce, shaded from the SAME b2 the lit pass uses — `averDiffuseAlbedo` is
view-independent by contract, so the zero view vector the voxelisation pass hands the material is
legal and the two passes agree exactly. The RHI stays feature-agnostic: `submitDraw` forwards a
binding table and a constant block by their RHI names and still never learns the word "material".

**Verified oracle-NEUTRAL across all nine device configurations** (`scripts/gates.ps1`, Debug, D3D12
debug layer on): every one of the 153 gates bit-identical to `scripts/gates.baseline.txt`,
`0 corruption / 0 error / 0 warning` throughout, `0x141` LiveKernelEvent count `67 → 67`. The current
sandbox scene authors no base-colour map or non-default `baseColorFactor` and keeps base colour in
`b1`, so the injected albedo is byte-identical either way — the neutrality is expected, and the gap
closes for the day a material authors base colour into its own factor. The mechanism is verified by
construction (the voxelisation pass now binds the identical b2 the lit pass binds, whose consumption
`§4e`'s negative test already proves); there is no gate that positively authors a non-default
base colour into a material because the sandbox authors it into `b1`. `AlphaBlend` stays
`NotImplemented` on purpose: blending needs a pipeline blend state and a back-to-front sort, both of
which are the renderer's, not the shading model's.

## 4f. Aver.Scripting.Host — the in-process CLR, WORKING

C# can now drive the live editor. The CLR is hosted **inside the engine process**, so a P/Invoke
from a hosted assembly resolves to the module the editor has already loaded — the same
`Aver.Render.Voxi.dll`, the same settings singleton, the same real device caps. That was the whole
of the defect recorded as §4d item 11: a standalone C# process gets its own copy of everything.

Gated by `-DAVER_MODULE_SCRIPTING=OFF`; that build configures, compiles, links and runs, `--scripts`
is accepted and ignored, and the gates are unchanged (measured, see below). Full write-up:
`modules/scripting/README.md`.

**Naming diverges from `docs/ARCHITECTURE.md` §142/§246 on purpose.** That document plans
`Aver.Scripting.NET` behind `AVER_SCRIPTING`, reaching the engine through a central `Aver.ABI`. The
central ABI module is still an empty skeleton (§4d item 17) and the C ABIs that exist live in the
feature modules that own them, so the host was built to match what is actually there: option
`AVER_MODULE_SCRIPTING` and target `Aver.Scripting.Host`, consistent with `AVER_MODULE_VOXI` /
`AVER_MODULE_PBR`. ARCHITECTURE.md is the design intent and has not been rewritten.

| Target | Kind | Links | Holds |
|---|---|---|---|
| `Aver.Scripting.Host` | STATIC | `Aver.Core`, `Aver.Platform` | `ScriptHost`, the hostfxr sequence, `scripting_abi.h` |
| `Aver.Scripting.Bridge` | C# | `Aver.Scripting` | the five entry points, the collectible ALC, the exception boundary |
| `Aver.Scripting` | C# | — | `AverBehaviour`, `Log`, the Voxi/PBR bindings — what a *user's* scripts reference |

**Never the RHI.** Scripting is not a rendering concern, and a link edge to `Aver.RHI` here would be
the same mistake §4c-2 spent twelve steps undoing. **Nothing links nethost or hostfxr either** —
both are `LoadLibraryW`'d at run time, because a missing import in the executable's table fails the
*process* at load time, before any code could decline. That is the one outcome this module may not
produce.

The hostfxr declarations are written out in `src/ScriptHost.cpp` rather than included from
`nethost.h`/`hostfxr.h`: those ship in the .NET host pack, which only exists on a machine with the
SDK, so including them would make the engine unbuildable without .NET.

### Declining, verified per branch

`ScriptHost::init` mirrors `VoxiRenderer::init` — one log line, a recorded reason, `false`, and the
editor runs exactly as it does today. Each branch was made to fail and the resulting line recorded;
**all thirteen gates returned their exact raw codes in every one of these runs**:

| Made to fail | Line |
|---|---|
| `nethost.dll` removed from `bin/` | `nethost.dll could not be loaded — the .NET runtime is unavailable` |
| `nethost.dll` replaced with an unrelated DLL | `nethost.dll exports no get_hostfxr_path` |
| bridge assembly not staged | `the managed bridge was not staged next to the executable …` |
| runtimeconfig demanding framework 99.0.0 | `hostfxr_initialize_for_runtime_config failed (0x80008096) — the framework the bridge targets is not installed` |
| bridge rebuilt at a different contract | `the staged Aver.Scripting.Bridge.dll speaks a different host contract than this build (host v1)` |

That last line was measured when the host was at v1; the host is now at **v2** and the message
carries whatever `AVER_SCRIPTING_CONTRACT_VERSION` says. The other four are unaffected by the bump
and were not re-driven for this phase — they fail before the contract is ever compared.

Note `DOTNET_ROOT` pointing at nothing does **not** trigger a decline — nethost still finds the
global install. Use one of the five above to test this path, not that.

### Hot reload, project scripts, and a script that changes the image — DONE

Three things landed together because each is useless without the others: a script nobody compiles
into the right place cannot be loaded, a script that cannot be reloaded cannot be iterated on, and
neither is worth doing for a script that can only write to the log.

**1. A project's scripts load with no flag.** `Tools ▸ Compile Scripts` now passes
`-o <project>\Binaries\Scripts` and the host is pointed at that same string, resolved by
`editor::scriptsBinaryDir`. The directory is a FUNCTION rather than an `OutputPath` in the generated
`.csproj`, because both ends have to agree and only one of them is ours to edit — every project
scaffolded before this change would otherwise build somewhere the host does not look, with nothing
anywhere saying why. It sits outside `Content\`, which is the asset mount root a shipped game reads;
build output is neither content nor something to ship. Measured:

```
build\bin\Sandbox.exe "...\SkyForge\SkyForge.ocproject" --frames 60
[INFO ] [Heartbeat] VERSION A - started
[INFO ] [Scripting] loaded Scripts.dll: 1 behaviour(s)
[INFO ] [Sandbox] probe (1375,819) ... raw (90,93,108) ... in-viewport
```

Priority is `--scripts <dir>` > `<project>\Binaries\Scripts` > `<exe>\Scripts`. The override wins so
the staged sample stays reachable with a project open, and so **no gate can be made to load a
project's scripts** — no oracle gate opens a project in the first place.

**2. `Tools ▸ Reload Scripts`.** Rebuild, unload, reload, re-instantiate. Three steps, each where it
belongs: the rebuild is NATIVE (the host already owns the `dotnet build` shell-out; a managed side
spawning compilers would be doing a job it has no business knowing about), and the swap runs on the
MAIN thread, because `OnShutdown`/`OnStart` are behaviour hooks and behaviours are a main-thread
thing. The build thread only sets a flag the frame loop reaps.

- **A failed build does not unload.** Unloading first would leave the editor with no scripts at all
  because of a typo — worse than carrying on with the ones already running. Verified by breaking
  `Heartbeat.cs`: `[ERROR] [Editor] Reload Scripts: dotnet build exited 1`, and the running
  behaviour kept logging to frame 700 and got its normal `OnShutdown` at exit.
- **Unloading is asynchronous and is treated as such.** A collectible ALC is gone only once every
  reference is dropped and a GC has run, so `UnloadScripts` returns 1 for "collected" and 0 for
  "still finalising" and **both are success**. The collect loop is bounded at two cycles: a
  behaviour that parked a reference somewhere the engine still holds keeps the old context alive
  forever, and blocking the main thread on that turns a leak into a hang. A 0 is a WARN naming what
  it costs (memory) and the new scripts are live regardless. Nothing on disk is locked either way —
  assemblies are loaded from memory streams — so the rebuild never has to wait for the answer.
- `DrainAndUnload` is `[MethodImpl(MethodImplOptions.NoInlining)]`, and that is load-bearing: the
  context cannot be collected while a stack frame holds it, so inlining it into the caller would
  report a leak that only the inlining had created.
- **No state is carried across.** A behaviour's fields start again from their initialisers.
  Carrying them needs a serialisation contract, and fixing that shape before the scene layer exists
  would be designing for an owner that does not exist yet.

Reload is a SEPARATE menu item, not a checkbox on Compile: they fail differently, and a user reaches
for them at different moments — Compile answers "does it build", Reload answers "does it do what I
meant" and swaps live behaviours out from under a running editor.

The contract goes to **v2** (`UnloadScripts` is a new entry point). A v1 bridge next to a v2 host
would bind everything it does have and then simply not reload, which is the failure the constant
exists to turn into a message.

Measured in ONE process, with the `.cs` overwritten on disk between the load and the swap
(`--reload-scripts 500`, a background job editing the file at t+4s):

```
[INFO ] [Heartbeat] VERSION A - started
[INFO ] [Heartbeat] VERSION A - update 400
[INFO ] [Editor] Reload Scripts: ...\SkyForge\Content\Scripts\Scripts.csproj built cleanly
[INFO ] [Heartbeat] VERSION A - shutting down after 564 update(s)
[INFO ] [Heartbeat] VERSION B - started, and this class did not exist when the editor launched
[INFO ] [Scripting] loaded Scripts.dll: 1 behaviour(s)
[INFO ] [Editor] Reload Scripts: 1 behaviour(s) live from ...\SkyForge\Binaries\Scripts
[INFO ] [Heartbeat] VERSION B - update 600
```

No editor restart, no "still finalising" warning (the old context was collected), and version B's
counter starts from 1 — which is the no-state-carried rule being visible rather than asserted.

**Reproducing it.** The scratch behaviour and the `Binaries\` it built into were removed from
`Aver Projects\SkyForge` afterwards, because engine work must not leave debris in someone's project.
To redo it: Tools ▸ New C# Script (any name) in a project, run
`Sandbox.exe <proj>.ocproject --frames 200 --compile-scripts` once to build it, then
`Sandbox.exe <proj>.ocproject --frames 1200 --reload-scripts 500` while another process overwrites
the `.cs` a few seconds in. `--reload-scripts` takes a frame count precisely so there is room to
edit the file between the initial load and the swap.

**3. A script that changes what the GPU draws.** `GiSwitchBehaviour`, staged alongside
`HelloBehaviour` in `bin/SampleScripts/`, turns global illumination on at update 5 through the same
`aver_voxi_*` C ABI the editor's own Project Settings panel drives. Because the CLR is in-process
the P/Invoke resolves to the `Aver.Render.Voxi.dll` the editor has already loaded — same settings
singleton, same frame — and `SandboxApp::onUpdate` ticks scripts BEFORE it composes the frame's
render state, so the change lands on the next frame.

```
build\bin\Sandbox.exe --frames 40 --scripts SampleScripts
[INFO ] [GiSwitch] global illumination is Off at startup (status: Ready)
[INFO ] [Scripting] loaded Aver.Scripting.SampleBehaviour.dll: 2 behaviour(s)
[INFO ] [GiSwitch] set global illumination to High from managed code at update 5
[INFO ] [Sandbox] probe (1375,819) px (0.41,0.36,0.41) raw (104,91,104) ... in-viewport
```

**`raw(104,91,104)` is the engine's own `--gi` oracle value, bit for bit**, on a run that passes no
`--gi`. That is the whole proof of this phase in one line: managed code did not merely log, it
changed what the GPU drew, and it is verifiable at the raw 8-bit code rather than by eye. The same
run previously read `raw(90,93,108)`, the no-GI value. **This changes what `--scripts SampleScripts`
reports and nothing else — no oracle gate passes `--scripts`.**

**The scene stays out of scope, deliberately.** No actor, transform, component, input or asset API
was invented here. `Aver.Scene` is separately designed and unbuilt (§9.1), and an interim object
model would be exactly the throwaway ABI that design exists to avoid — every script written against
it would have to be rewritten. The generated templates, both C# modals and `modules/scripting/README.md`
all say so in those terms.

### Collectible load context — the decision that could not be deferred

User assemblies go into a collectible `AssemblyLoadContext`, loaded from a memory stream so the DLL
on disk is not locked. An assembly in a non-collectible context can **never** be unloaded, so hot
reload is not something that can be layered on later; it is decided at the first load or not at all.
Done now even though reload lands next phase.

`ScriptLoadContext.Load` delegates to **the bridge's own context, not Default**.
`load_assembly_and_get_function_pointer` loads a hosted component into an isolated context driven by
its `deps.json`, so `Aver.Scripting` is not in Default at all — the obvious "return null and fall
through" implementation produced `Could not load file or assembly 'Aver.Scripting'` with the
assembly loaded and sitting next to the executable. This cost one build to find and is the single
least obvious thing in the module.

### Lifecycle — `AverBehaviour`, a base class (decision recorded)

`OnStart()` / `OnUpdate(float dt)` / `OnShutdown()`, discovered by reflection, constructed through a
public parameterless constructor, called on the main thread from the frame loop. A base class rather
than an attribute because the compiler then **checks the hooks**: an attribute design lets a
misspelt `OnUpate` compile cleanly and never run, which is a failure with no error message anywhere.
Cost is single inheritance, acceptable for a leaf type.

### A managed exception never crosses back into C++

An exception escaping an `[UnmanagedCallersOnly]` method does not become a C++ exception the engine
could catch — it **terminates the process**. Every entry point is wrapped whole, and each hook call
is wrapped individually *inside* the loop so one throwing behaviour does not stop the ones after it.
A behaviour that throws is logged and disabled for the session. Measured with a deliberately
throwing assembly:

```
[ERROR] [Scripting] ThrowsOnStart.OnStart threw: InvalidOperationException: deliberate OnStart failure - the behaviour has been disabled
[INFO ] [ThrowsOnUpdate] update 1
[INFO ] [ThrowsOnUpdate] update 2
[ERROR] [Scripting] ThrowsOnUpdate.OnUpdate threw: InvalidOperationException: deliberate OnUpdate failure - the behaviour has been disabled
[INFO ] [Survivor] still running at update 20
```

### Two versioned contracts, each checked at its own boundary

- **host ↔ bridge**: `AVER_SCRIPTING_CONTRACT_VERSION` in `scripting_abi.h`, plus `sizeof` the
  struct. Checked by the bridge in `Bootstrap`.
- **bridge ↔ user assembly**: the assembly version of `Aver.Scripting`, read out of the user
  assembly's own **reference table** rather than from an attribute the author must remember to
  apply. An assembly that does not reference `Aver.Scripting` cannot hold a behaviour and is skipped
  silently — a scripts folder legitimately holds support libraries. Measured by building the test
  assembly against a temporarily-bumped `Aver.Scripting` 2.0.0:

```
[ERROR] [Scripting] BadScripts.dll was built against Aver.Scripting 2.0.0.0 but this engine provides 1.0.0.0
        - the assembly was rejected. Rebuild it against this engine.
```

### Ownership and staging

The **app** owns the `ScriptHost` and ticks it from `SandboxApp::onUpdate`, exactly as it owns the
Voxi feature. `Aver.Runtime` is deliberately untouched: routing scripting through the composition
root would make it non-optional there, and §8 already records that the app configures its
subsystems directly.

`Aver.Scripting.Bridge.dll`, its `.runtimeconfig.json`, `Aver.Scripting.dll` and `nethost.dll` are
staged next to the exe by `OUTPUT`/`DEPENDS` custom commands, never `POST_BUILD` — a `POST_BUILD`
rule only fires on relink, and shipping a stale asset from a clean build was a real bug here (§6).
All of it is optional: with no `dotnet` on `PATH`, CMake says so at configure time, skips the rules,
and the host declines at run time.

### Proof it runs — from managed code

`scripting/csharp/Aver.Scripting.SampleBehaviour` is staged to `bin/SampleScripts/`, deliberately
**not** `bin/Scripts/` (the default), so a normal editor run loads nothing and no demo script runs
unasked in the product. `build\bin\Sandbox.exe --frames 40 --scripts SampleScripts`:

```
[INFO ] [Scripting] managed bridge online (contract v2, 10.0.10, API v1.0.0.0)
[INFO ] [GiSwitch] global illumination is Off at startup (status: Ready)
[INFO ] [HelloBehaviour] OnStart from managed code - hosted in-process on 10.0.10
[INFO ] [Scripting] loaded Aver.Scripting.SampleBehaviour.dll: 2 behaviour(s)
[INFO ] [Scripting] .NET runtime hosted in-process; 2 behaviour(s) live
[INFO ] [GiSwitch] set global illumination to High from managed code at update 5
[INFO ] [HelloBehaviour] OnUpdate has run 10 times (0.191s of frame time)
[INFO ] [Sandbox] probe (1375,819) ... raw (104,91,104) ... in-viewport
[INFO ] [GiSwitch] leaving global illumination at High
[INFO ] [HelloBehaviour] OnShutdown after 40 update(s)
```

Every one of those lines originates in managed code and reaches the console through the engine's own
log. The probe read `raw(90,93,108)` before `GiSwitchBehaviour` existed and reads the `--gi` oracle
value now — see the hot-reload section above for why that is the strongest line in the block.

### Verification

All 13 oracle gates re-run on the final binary, bit-exact at the raw 8-bit codes, spaced 800 ms
apart, 0/13 harness misfires. `0x141` `LiveKernelEvent` count **67 → 67, zero new TDRs**. The gates
also hold under every decline branch above and under `-DAVER_MODULE_SCRIPTING=OFF` — that build was
configured and built again for this phase, and `--frames 40` / `--gi` / `--probe 1413 1042` with
`--scripts SampleScripts` passed alongside returned `raw(90,93,108)` / `raw(104,91,104)` /
`raw(64,79,102)`, i.e. the flag is accepted and ignored exactly as before.

## 4g. Degraded-device testing — the fallbacks became EXECUTABLE, and three were broken

> **Historical.** This section records the phase that made the lesser paths runnable and catalogued
> what they do; its "three defects" are fixed and its baseline table is superseded by §4h and by
> `scripts/gates.baseline.txt`. The description of `--force-caps`, `--warp` and the debug-layer drain
> below is still current and still the place to read about them.

The engine has only ever run on an RX 7800 XT, so every capability gate in it was a reasoned claim.
This phase built the two things that make those paths runnable here and then **catalogued** what they
produce. It deliberately fixes nothing: diagnosis and repair are separate so the diagnosis stays
honest. The three defects found are listed in §4d as items 18–20.

### The two switches

- **`--force-caps <list>`** — a clamp on what `DeviceCaps` reports. Tokens, comma-separated:
  `no-rt`, `no-ms`, `no-cons-raster`, `no-typed-uav`, `no-dxc`, `sm=<51|60|61|65|66>`,
  `msaa=<1|2|4|8>`, `tier1`. An unrecognised token is an ERROR and the **whole** override is
  discarded (running half-clamped would test a device the command line did not name).
- **`--warp`** — selects the D3D12 software rasteriser via `IDXGIFactory::EnumWarpAdapter`. If WARP
  is unavailable the search falls through to hardware rather than failing, per the engine's
  decline-and-carry-on rule.

Two properties keep the clamp safe to leave in the product build. `clampCaps()` is **monotonically
reducing** — it takes minimums and clears flags, and finishes by re-clamping every field against the
hardware value it was handed, so no override can raise a capability. And it is applied **once, at the
end of `D3D12Device::queryCaps()`**, so every consumer (the backend's own pipeline selection, Voxi's
feature status, the settings UI) sees one reduced device and nothing anywhere branches on "was this
overridden". The single exception is `no-dxc`, which must also stop `ShaderCompiler::init()` loading
`dxcompiler.dll`: a device reporting no DXC while still compiling DXIL would exercise nothing.

`clampCaps` also derives the implications rather than asking the caller to spell them out — no DXC
means no DXIL means SM 5.1, and SM < 6.5 means no mesh shaders and no RayQuery. So each token
describes a machine that could exist.

### The debug layer now reaches the log

`dd.enableDebug` was already true, but the debug layer writes to the Win32 debug output, which
nothing outside a debugger reads — for an automated matrix that is the same as not having it. The
D3D12 device now drains `ID3D12InfoQueue` after every `ExecuteCommandLists` and logs each **distinct
message ID once** (the two known-benign warnings fire every frame and would bury the one that
matters), with running totals printed at device destruction:

```
[RHI.D3D12] debug layer totals: 0 corruption, 0 error, 41 warning
```

Those 41 warnings are the two pre-existing benign ones, `#820
CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE` and `#1328 CREATERESOURCE_STATE_IGNORED`, repeated per
frame. **A missing totals line means the process died before shutdown** — that is how the WARP crash
below was first seen.

### Baselines — every configuration, all 13 gates, measured 2026-07-21

Values differ where a feature is off; that is expected. What matters is that each configuration is
correct, stable and silent to the debug layer. Every row below is `--frames 40`, spaced 800 ms.

| gate | baseline | `no-rt` | `no-ms` | `sm=60` | `tier1,no-typed-uav` | `no-cons-raster` | ALL OFF¹ | `no-dxc` | WARP |
|---|---|---|---|---|---|---|---|---|---|
| (centre) | 90,93,108 | 90,93,108 | 90,93,108 | 90,93,108 | 90,93,108 | 90,93,108 | 90,93,108 | **65,77,92** | 90,93,108 |
| `--ms` | 90,93,108 | = | = | = | = | = | = | **65,77,92** | 90,93,108 |
| `--rt` | 90,93,108 | = | = | = | = | = | = | **65,77,92** | 90,93,108 |
| `--ms --rt` | 90,93,108 | = | = | = | = | = | = | **65,77,92** | 90,93,108 |
| `--gi` | 104,91,104 | = | = | = | = | **96,91,104** | **96,91,104** | **65,77,92** | 104,91,104 |
| `--ms --gi` | 104,91,104 | = | = | = | = | **96,91,104** | **96,91,104** | **65,77,92** | **CRASH** |
| `--ms --rt --gi` | 104,91,104 | = | = | = | = | **96,91,104** | **96,91,104** | **65,77,92** | **CRASH** |
| `--gi-debug` | 66,44,45 | = | = | = | = | **132,76,65** | **132,76,65** | **65,77,92** | 66,44,45 |
| `--ms --gi-debug` | 66,44,45 | = | = | = | = | **132,76,65** | **132,76,65** | **65,77,92** | **CRASH** |
| `--probe 1413 1042` | 64,79,102 | = | = | = | = | = | = | **82,92,104** | 64,79,102 |
| `--rt --probe …` | 64,79,102 | = | = | = | = | = | = | **82,92,104** | 64,79,102 |
| `--ms --rt --probe …` | 64,79,102 | = | = | = | = | = | = | **82,92,104** | 64,79,102 |
| `--gi --probe …` | 67,79,97 | = | = | = | = | **66,78,97** | **66,78,97** | **82,92,104** | 67,79,97 |

¹ `no-rt,no-ms,no-cons-raster,no-typed-uav,tier1,msaa=1`. `=` means bit-identical to baseline.
Debug-layer totals were `0 corruption, 0 error` in **every** completed run of every configuration.
`0x141` `LiveKernelEvent` count **67 → 67 across the whole exercise, zero new TDRs.**

### The positive proof that `no-rt` really turns the ray off

Every one of the 13 gates is identical under `no-rt`, which proves nothing on its own — §4c-2 already
records that `--probe 1413 1042` is fully shadowed on both paths. The penumbra pixel is the one that
distinguishes them, and it does:

| `--probe 1413 1150` | no flag | `--rt` | `--ms --rt` |
|---|---|---|---|
| baseline | 60,73,96 (PCF) | **59,72,95** (RayQuery) | **59,72,95** |
| `--force-caps no-rt` | 60,73,96 | 60,73,96 | 60,73,96 |

So under the clamp `--rt` demonstrably falls back to the 3×3 PCF shadow map. (§4c-2 quotes
`59,72,94` / `65,78,99` for this pixel; those predate the PBR 17(c) re-baseline. The values above
supersede them.) `no-ms` is confirmed the same way from the log rather than the image, since the mesh
path is pixel-identical by design: `mesh shader path ready` is absent and Voxi reports `mesh-shader
variants absent`.

### `sm=60` is the good news

**All 13 gates bit-identical to baseline with zero debug-layer errors.** A device with DXC, DXIL and
SM 6.0 but no mesh shaders and no RayQuery renders the engine's reference image exactly. That is the
single most valuable result here: the SM 6.0 baseline the renderer claims in §3 is real, and it
covers every DX12 GPU that is not D3D12 Ultimate.

### WARP — it works, and it found a crash

WARP reports the **same** tiers as the RX 7800 XT (RT 1.1, mesh tier 1, SM 6.6, binding tier 3), so
it is not the source of different capabilities the plan assumed it would be. It is ~19× slower
(`--frames 40 --gi`: 2.0 s hardware, 38.0 s WARP) and it is bit-identical to hardware at every gate
it completes — including under `no-cons-raster`, where it returns the same `96,91,104` /
`132,76,65`. That is a strong independent check on the renderer: two completely different
implementations of D3D12 agree to the raw 8-bit code.

It also crashes, hard and deterministically, on **mesh-shader voxelisation with conservative raster**.
See §4d item 18 — the crash is the finding, not a WARP caveat to be worked around.

### What `tier1` and `no-typed-uav` actually do: nothing, and that is the finding

Both clamp the reported value correctly and change no gate, because **nothing in the engine reads
either**. `resourceBindingTier` was added to `DeviceCaps` by this phase and has no consumer at all;
`typedUavLoads` is plumbed into Voxi's `DeviceInfo` and never read there. So the Tier-1 rules §4d
item 9 says the code follows are **still** reasoned rather than measured — a clamp on a number no
branch reads cannot exercise them, and the D3D12 runtime validates against the *real* device tier,
which is 3 on both adapters available here. Recorded as §4d item 20 rather than papered over.

## 4i. Build configurations, the debug layer, and the two per-frame warnings

Three interlocking items, done together because each one was hiding the next.

### Release is reachable, and it has its own baseline

`scripts/build.bat` hardcoded `-DCMAKE_BUILD_TYPE=Debug`, so `msvc-ninja-release` in
`CMakePresets.json` could not be reached through the normal script and **every measurement this
project had ever taken was a Debug measurement**.

Configuration and build tree now come from two environment variables (`AVER_BUILD_CONFIG`,
`AVER_BUILD_DIR`) rather than from the command line, so everything after the script name is still
forwarded verbatim to CMake configure and `./scripts/build.ps1 -DAVER_MODULE_VOXI=OFF` keeps working.
`build.ps1` gained `-Release` (and `-Config` / `-BuildDir` for anything else), sets those two, and
**restores them afterwards** — leaving them set would have made the next `scripts/run.ps1` in the
same shell build the Release tree while launching the Debug binary.

Trees are separate by default (`build/`, `build-release/`) because the two baselines are separate.

### The Release oracle: 152 gates, BIT-IDENTICAL to Debug

Small LSB-scale differences were expected and none appeared. `scripts/gates.baseline.release.txt`
therefore holds the same numbers as `scripts/gates.baseline.txt`, and that is a result rather than a
shortcut: **nothing in this renderer's shading happens on the host CPU.** The probe reads an 8-bit
backbuffer written by HLSL that the GPU's own compiler produces from source strings at run time —
identical bytes in both builds — and the C++ in front of it only assembles matrices and constants.
Host optimisation had nothing to move. A change to shading that DID move under `/O2` would have to
have come through those constants, which is worth knowing.

**No `/fp:fast` anywhere.** Both configurations compile at MSVC's default `/fp:precise`
(`CMAKE_CXX_FLAGS_RELEASE` is exactly `/O2 /Ob2 /DNDEBUG`; the project adds no `/fp:` flag at all).
That is the right default for a renderer whose verification is bit-exactness, and it is now recorded
rather than inherited silently — if anyone ever adds `/fp:fast`, the Release baseline is what will
notice.

One gate, `no-rt/gi-debug`, missed twice inside the Release sweep and then measured correct **8 times
out of 8** in isolation and correct again through the runner. That is §4d item 22 — the unexplained
plausible-wrong-pixel flake — not a Release difference. It is recorded here because it is the second
time item 22 has cost a sweep, and because two consecutive misses defeat the runner's single retry.

### Timing: 400 frames, and why the number is not about the engine

| build | 40 frames | 400 frames | derived per-frame | derived FPS |
|---|---|---|---|---|
| Debug | 2.010 s | 8.024 s | **16.71 ms** | 59.9 |
| Release | 1.996 s | 8.008 s | **16.70 ms** | 59.9 |

Medians of three runs; per-frame is `(t400 - t40) / 360`, which cancels the fixed startup cost (the
splash is held 1100 ms and the shaders compile at run time).

**Both numbers are the display's refresh rate, not the engine's.** `present()` calls
`swapChain_->Present(1, 0)` — sync interval 1 — so every frame blocks on vblank at 60 Hz and both
builds have idle time to spare. **The brief's premise that "nothing about performance is currently
knowable" survives this phase**, for a reason we can now name precisely: it was never the missing
Release build, it is the vsync-locked present. An unthrottled present or a CPU/GPU timer per frame
is the next thing needed, and neither belongs in this phase — see §4d item 23.

Where the frame is genuinely not vsync-bound the difference is measurable and small. On WARP, whose
frames run ~100 ms, Debug is **102.3 ms/frame** against Release's **101.5 ms** (`(t100 - t20) / 80`,
two runs each) — **0.8 %**. That is what "the host code is not the cost" looks like from the other
side.

### 1b. The D3D12 debug layer is opt-in

`Engine.cpp` set `dd.enableDebug = true` unconditionally. It is now `cfg.enableDebugLayer`, fed by
`--debug-layer` on `Sandbox.exe` and **off in every build type**. A Debug-build default was
considered and rejected: a plain `Sandbox.exe` would then still pay for whole-API validation, and
Debug is the configuration every gate and every timing in this document was measured in.

`drainDebugMessages()` ran at the end of `endFrame()` on every frame. The test moved to the call
site — `if (infoQueue_) drainDebugMessages();` — so a run without the layer makes no call at all.

`scripts/gates.ps1` passes `--debug-layer` on every launch, so the per-gate C/E/W counters it
reports are unchanged. The capability moved to the runner rather than being dropped: a gate that
reported no corruption because nothing was watching would be worse than no gate.

**The layer's own cost is NOT claimed here, because it was not measurable.** On hardware it hides
inside vsync; on WARP the A/B is 101.5 ms/frame off against 102.2 ms on, which is inside the run-to-
run spread for a scene of a few dozen draws. The change is correct on its own terms — an
unconditional validation tax and an unconditional per-frame drain are both gone — and that is all it
is claimed to be.

### 1c. Both per-frame warnings are fixed; the totals are now `0 corruption, 0 error, 0 warning`

A 40-frame `--debug-layer` run reported **41 warnings** before this phase and reports **0** after.

**#820 CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE — 40 of the 41, one per frame.** The MSAA scene
colour target was created with a hardcoded optimised clear value of `(0.10, 0.12, 0.16, 1)` while the
editor pushes `(0.055, 0.055, 0.062, 1)` through `setClearColor`, and the debug layer states outright
that the clear is slower as a result.

Hardcoding the editor's colour at creation would have been a fix for one caller. The two values are
reconciled instead: `createMsaaColor()` uses whatever `clear_` currently holds and remembers it in
`msaaClear_`, and `beginFrame()` calls `reconcileClearValue()`, which rebuilds the target when — and
only when — the two have actually diverged. The reconciliation is deliberately NOT in `setClearColor`:
a setter that recreated a render target would stall the GPU from inside the app's update, and the app
pushes the same colour every frame. In practice it rebuilds exactly once, on the frame after the
editor first sets its colour, and the log says so at TRACE. If the rebuild ever fails the previous
clear value is restored and the target rebuilt with that, because a device with no scene colour
target cannot draw at all.

**#1328 CREATERESOURCE_STATE_IGNORED — the remaining 1, at startup.** The DXR acceleration-structure
**scratch** buffers were created in `UNORDERED_ACCESS`. D3D12 ignores it: every buffer is created in
`COMMON` whatever is asked for. (It fires in the baseline configuration and not only under `--rt`
because Voxi builds the structures every frame regardless — the flag chooses which shading path
consumes them, not whether they exist.)

This was not silenced, because the state that is ignored is exactly the state something else is
tracking. Scratch is now created `COMMON` and relies on the implicit promotion a buffer gets from
`COMMON` on its first GPU use, which is what puts it in `UNORDERED_ACCESS` for the build. The
result buffer keeps `RAYTRACING_ACCELERATION_STRUCTURE`, which is the one buffer state D3D12 does
honour and does require.

**`BufferDesc::initialState` is GONE from `RHIResources.hpp`,** and that is the part that touches the
module-owns-resource-state contract. A buffer has no initial state to give: D3D12 creates it in
`COMMON`, it is implicitly promoted out of `COMMON` by its first use and **decays back to `COMMON` at
the end of every command list**, so `COMMON` is not merely where a buffer starts — it is where it is
at the top of every frame. Keeping the field would have meant a field the backend must ignore and a
tracker seeded from a state the resource was never in. `createBuffer` now creates every non-upload,
non-AS buffer in `COMMON` and seeds `RhiBuffer::state` to `Common`, so the tracker and the resource
agree and a module's first barrier is checked against something true. Nothing set the field, so no
caller changed. `TextureDesc::initialState` stays — an image layout is real.

## 4d. Known gaps and defects

Every numbered item below is referenced elsewhere in this document as `§4d item N`. The numbering is
stable: items are struck off into the closed list rather than renumbered, which is why the surviving
numbers have gaps in them.

**Closed since this list was written**
- **A Release build could not be reached through the normal script**, so every gate and every timing
  ever recorded was a Debug measurement. `./scripts/build.ps1 -Release` builds into its own tree and
  `./scripts/gates.ps1 -Release` runs against its own baseline. All 152 Release gates are
  bit-identical to Debug; no `/fp:fast` anywhere. §4i.
- **The D3D12 debug layer was on for every run** (`Engine.cpp:40`, unconditional), and
  `drainDebugMessages()` ran every frame. Both are opt-in behind `--debug-layer` now, which
  `scripts/gates.ps1` passes so the per-gate counters survive. §4i.
- **Two debug-layer warnings fired on every frame** — #820 (the clear value the scene colour target
  was created with never matched the one the editor clears to) and #1328 (acceleration-structure
  scratch created in a buffer state D3D12 ignores). A 40-frame run went from **41 warnings to 0**;
  `BufferDesc::initialState` was removed rather than worked around, because a buffer has none. §4i.
- **The FXC / SM 5.1 fallback did not decline cleanly, and its image was wrong** (was item 19). It
  does not decline at all any more — it renders the reference image. Two causes, both found only
  because the SM 5.1 path was finally executable: every Voxi shader asked for SM 6.0 when none of
  them needs it, and Voxi's `averVertexOf(VoxOut)` overload made **every** call ambiguous under FXC's
  looser overload resolution (`X3067`) while DXC accepted it. §4h.
- **WARP crashed on mesh-shader voxelisation with conservative raster** (was item 18). Established
  as data-independent — one fixed, tiny, in-range triangle faults identically — and worked around at
  the pipeline-state level for software adapters only, with the reason logged once. §4h.
- **`--ms` on a device without mesh shaders was silently ignored** (was item 21), and so was every
  other refused render setting. `Renderer::setSettings` now says so once per feature per device,
  naming the missing capability, and only for a feature that was actually asked for. §4h.
- **No oracle gate covered the sun's SPECULAR term** (was Renderer item 1). `--probe 2200 1400` is a
  sunlit floor pixel that does receive direct specular, and it is gates 16–17 of `scripts/gates.ps1`.
  The centre probe sits on the cube's unlit left face (`ndl` ~0) and `(1413,1042)` sits inside the
  cast shadow (`visibility` ~0), so a change to the whole masking-shadowing formulation once left all
  13 gates bit-identical. BRDF work has automated cover now. §4h.
- **C# could not drive the live editor** (was item 11). The CLR is now hosted in-process, so a
  P/Invoke from a script resolves to the module the editor has already loaded. Full write-up,
  the five verified decline branches and the managed-code proof in §4f.
- **Hot reload was not implemented** (was item 11). `Tools ▸ Reload Scripts` rebuilds, unloads the
  collectible context, reloads and re-instantiates, in a running editor. Demonstrated in one
  process with the `.cs` edited on disk between the load and the swap; a failed build deliberately
  does not unload. §4f.
- **A project's scripts did not load without a flag** (was item 11b). `Tools ▸ Compile Scripts`
  builds into `<project>\Binaries\Scripts` and the host reads exactly that directory when a project
  is open. §4f.
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
9b. **The fallbacks are RUNNABLE here, and now VERIFIED here.** `--force-caps` and `--warp` (§4g)
    execute the no-RT, no-mesh-shader, no-conservative-raster, SM-6.0 and FXC/SM-5.1 paths on this
    machine; §4h turned all nine configurations into a standing gate set that any change has to pass.
    Items 18, 19 and 21 are fixed. Item 9 stays open for everything a clamp cannot reach — real
    Tier 1 hardware, NVIDIA/Intel drivers, vendor-specific behaviour.
20. **`resourceBindingTier` and `typedUavLoads` have no consumer.** The first was added by §4g and
    is only logged; the second reaches Voxi's `DeviceInfo` and is never read. `--force-caps tier1`
    and `no-typed-uav` therefore change no gate and prove nothing. The Tier-1 discipline in item 9
    remains unmeasured: the D3D12 runtime validates against the real device tier, which is 3 on both
    adapters on this machine, so no software clamp can exercise it. **Deliberately still open** —
    inventing a branch on the tier purely so the clamp has something to move would test the branch,
    not the hardware.
22. **The editor window intermittently starts at 45x45, and some runs return a plausible wrong
    pixel.** The first half is measured: `viewport (0,198 45x45) OUTSIDE-VIEWPORT` twice inside ten
    otherwise identical runs of one gate, 1.2 s apart. The probe catches that case by design. The
    second half is not explained — three runs across this phase returned a wrong pixel with a
    full-size, correct-looking rect and a silent debug layer (`65,83,117` on `baseline/shadow-ms-rt`;
    sky on two WARP GI gates). Plausibly the same event seen before the rect settles, but that is a
    hypothesis. **Start at `Win32Window`'s startup sizing, not at the renderer.** Full write-up and
    the reason not to assume either answer in §4h.
    **It has now cost two more sweeps** (§4i): `warp/gi-debug` flaked once in Debug with a visibly
    different rect (`3058x1348` against `2750x1242`, i.e. the launch race), and `no-rt/gi-debug`
    missed TWICE in a row in Release with the correct rect and then measured right 8/8 in isolation.
    Two consecutive misses defeat the runner's single retry, so this defect can still turn a clean
    sweep red. That is an argument for fixing it, not for retrying harder.
23. **Frame timing measures the display, not the engine.** `present()` calls
    `swapChain_->Present(1, 0)`, so every frame blocks on vblank: Debug and Release both come out at
    16.7 ms/frame = 59.9 FPS on a 60 Hz panel, with idle time to spare in both (§4i). Nothing about
    how long a frame actually TAKES is knowable until there is either an unthrottled present or a
    per-frame CPU/GPU timer, and the engine has neither. This is the real reason the project has no
    performance data — it was never the missing Release build. Whoever picks this up should add the
    timer rather than only flipping the sync interval: a `--no-vsync` run measures a frame rate,
    a timer measures a frame, and the second is what a renderer needs. **New, deliberately open.**
10. D3D11 and Vulkan backends are still **stubs** — D3D12 is the only working backend, so
    "supports DirectX 12" is a hard requirement. Both decline cleanly: `createDevice` falls through
    to Null, `resources()` is null, and `VoxiRenderer::init` logs and returns false.

**Scripting**
11c. **`AverBehaviour` cannot reach the SCENE.** The lifecycle, the load context, the exception
    boundary, both version contracts, hot reload and the project script path are all done, and a
    script can drive the render modules' live settings — `GiSwitchBehaviour` turning GI on is
    measurable at the oracle's own raw codes (§4f). What is still missing is any actor, transform,
    component, input or asset handle. That waits on the generic scene layer (§9.1) and is
    deliberately not stubbed: an interim object model would be the throwaway ABI that design exists
    to avoid, and every script written against it would have to be rewritten.
11d. **Reload is manual and stateless.** `Aver.Platform` now HAS a `DirectoryWatcher`, but nothing
    calls it yet, so `Tools ▸ Reload Scripts` is still the only trigger. Nothing carries a
    behaviour's fields across a swap either: the state carry-over needs a serialisation contract,
    and fixing that shape before §9.1 exists would be designing for an owner that does not exist
    yet.
11e. **A `.cs` generated before this phase still does not run** — but it now SAYS so. The old
    template produced a plain class with `OnStart`/`OnUpdate` and no base type, and discovery is
    `IsAssignableFrom(AverBehaviour)`, so those files compile and are skipped. The template is
    fixed and existing files are the user's, so they are not rewritten; what closed the sharp edge
    is that an assembly which yields NO behaviours is now re-scanned for lifecycle-shaped types and
    each one is named in a WARN telling the author to add `: AverBehaviour`. Silence was the actual
    defect, not the skip.
12. The **launcher hardware probe** in `docs/MINIMUM_SPECS.md` §7 is now unblocked (it was waiting
    on in-process hosting) but is not written.

**Editor / engine**
13. Dock layout does **not persist** (`io.IniFilename` is null) — rebuilt from DockBuilder each run.
14. Output Log does not capture the real log. The Content Browser now reports the loaded project
    and where its `Content/` is mounted, but still **enumerates nothing** — it needs the asset
    pipeline (§9) before it can list files.
14b. `.ocproject` is **read-only** in the editor: New Project writes one, but nothing writes an
    existing manifest back, so Project Settings ▸ Description is a display. `STARTMAP` is
    recorded and shown but not acted on — there is no scene load yet (item 16).
15. Toolbar Save / Play / Pause / Stop are **non-functional stubs**.
16. ~~No scene save/load, no `.ocmesh`, no asset import~~ — **WRONG as written, corrected 2026-07-28.**
    `fmt::loadOcworld`/`saveOcworld`, `.ocmesh`/`.ocskel`/`.ocanim` and a glTF/GLB importer behind the
    Content Browser's Import button all exist. They landed in the commits this document never wrote up
    (see the hole named at the top) and this item was simply never struck off. What is genuinely
    missing is `.octex`, the cooked block-compressed texture form — §9.5.
17. `modules/abi` was DROPPED, not deferred (its README records that). The C ABI lives in eight modules — scene, framework, physics, render.pbr, render.voxi, ui.abi, audio.abi, scripting — which is the thesis of docs/ABI.md.

**New this phase (§4o–§4t)**
24. **The actor editor has never been opened by a human.** Compiles, links, registers; its parser is
    checked against two real projects by `tools/ActorSweep.cpp`; `ActorPreviewTest` drives the preview
    headlessly. No pixel of it has been seen. Everything an editor can be wrong about that a compiler
    and a headless test cannot see — layout, input, whether the image appears at all — is unmeasured.
    Also: no rotate or scale handles, and no Roslyn backend.
25. **The Media Foundation audio import path is covered by nothing.** `OcAudioTest` SKIPS it for want
    of a file and still exits 0, so mp3/m4a/aac/wma/flac decode is asserted by no test at all. A
    checked-in short sample of each, or a suite that FAILS rather than skips when none is present,
    would close it; a skip that passes is how an untested path stays untested.
26. **No sound has been heard.** `AudioTest` is 70 headless assertions on the mixer and `AudioProbe.exe`
    exists, but nothing recorded a listening test. Silence passes every one of those assertions.
27. **The full oracle has not been run since `d8fc062`, thirty-nine commits ago.** §8's own rule says
    to run `./scripts/gates.ps1` before calling a renderer change safe, and this phase changed the
    blend-mode enum, the resize path and the pipeline-rebuild gating. Nothing here claims a gate
    result, which is the honest position and not a substitute for the sweep.
28. **The renderer is not bit-deterministic between runs, and the oracle assumes it is** (measured at
    `083d687`, and it is a finding about the engine rather than about that change). The same binary run
    twice, nothing altered, differed by 17714 bytes of 28072336 with a maximum delta of 188; removing
    `--auto-exposure`, whose histogram adaptation is frame-timing dependent, drops the floor to 2406
    and does not reach zero. `scripts/gates.ps1` compares single probe values exactly, and single
    pixels in stable regions mostly survive — which is why it passes at all — but the last full run
    recorded two gates as FLAKY-passed-on-retry, which is exactly what this looks like under an exact
    oracle. Likeliest suspects: the voxel volume's atomic injection order, and Voxi's one-frame-delayed
    draw replay. **This is the same class of thing as item 22 and they should be investigated
    together**; a retry that hides both is not a fix.
29. **`docs/ABI.md` documents seven C seams and 201 exports. There are eight and 225.** The audio seam
    (`modules/audio.abi`, 24 exports) is absent from it entirely — the string "audio" does not appear
    in that file. Nothing else in the tree cross-checks the count, so it will not self-correct.
30. **The game UI has no text and no widget tree.** Text is blocked on an unmade font decision
    (vendoring `stb_truetype.h`, or bitmap fonts); the demo HUD is a hand-written draw list standing in
    for what a widget tree would produce, and is meant to be deleted. There is also no gate covering
    the UI — `--ui-demo` is in no configuration in `scripts/gates.ps1`, so the one pixel-probe result
    in §4o protects nothing going forward.

**Decisions taken (do not re-litigate without reason)**
- Ray tracing targets **DXR 1.1 inline RayQuery only**; DXR 1.0 would add only GPUs that emulate
  it without RT cores. Gate on `RaytracingTier >= 1.1`, NOT on feature level 12_2.
- Engine floor stays **FL 11_0**; "D3D12 Ultimate only" is a spec/marketing decision in
  `docs/MINIMUM_SPECS.md`, not something baked into the renderer.
- Transform tools live in the **viewport overlay bar** (as in Unreal), not the window toolbar.
- Render settings are **project-wide** → Edit ▸ Project Settings ▸ Rendering, not the Details panel.

## 4h. The degraded paths are FIXED, and they are now standing verification

§4g catalogued what the lesser paths do and deliberately fixed nothing. This section is the repair,
and the answer to two of the three defects turned out to be that the gate was wrong rather than that
the fallback was: a feature was being withheld from hardware perfectly capable of running it.

### `./scripts/gates.ps1` — the oracle as a runner

The single most useful thing here. Nine device configurations × seventeen gates, compared against
`scripts/gates.baseline.txt`, TDRs counted before and after, launches spaced 800 ms, exit code = the
number of failures.

```powershell
./scripts/gates.ps1                        # everything (allow ~25 min; WARP is ~19x slower)
./scripts/gates.ps1 -Config baseline       # just the primary path (~30 s)
./scripts/gates.ps1 -Config baseline,warp
./scripts/gates.ps1 -Record                # re-record, ONLY with a stated reason
```

A gate fails on a moved raw code, a non-zero exit, a probe that landed outside the viewport, a
debug-layer corruption or error, a **missing** debug-layer totals line (the process died before
shutdown — how the WARP fault was first seen) or a new `0x141` `LiveKernelEvent`. Recording rewrites
only the configurations actually run, so re-recording one cannot quietly erase another.

**WARP flakes, twice, and it is NOT explained — see §4d item 22.** The runner therefore re-runs a
missed gate once and prints BOTH results with BOTH viewport rects, reporting `FLAKY` rather than
either failing or hiding it. A gate that misses twice still fails. This is not a retry that buries a
defect: a flake is printed, counted and called out in the summary line, which is strictly more
information than the single wrong number a plain re-run would have produced.

**Four gates are new**, and each closes a hole the thirteen had:

| gate | probe | what only it can see |
|---|---|---|
| `penumbra` / `penumbra-rt` | `1413,1150` | RayQuery vs the 3×3 PCF shadow map. `(1413,1042)` is fully shadowed on both paths, so without these `no-rt` returns thirteen identical numbers and proves nothing about the fallback it exists to test |
| `sunlit` / `sunlit-gi` | `2200,1400` | direct SPECULAR. The centre probe is on the cube's unlit left face (`ndl` ~0) and the shadow probe has `visibility` ~0, so neither receives any. This was §4d Renderer item 1 |

### Baselines — 9 configurations × 17 gates, measured 2026-07-21

The authoritative copy is `scripts/gates.baseline.txt`; this is the shape of it. `=` means
bit-identical to baseline.

| gate | baseline | `no-rt` | `no-ms` | `sm=60` | `tier1,no-typed-uav` | `no-cons-raster` | ALL OFF¹ | `no-dxc` | WARP |
|---|---|---|---|---|---|---|---|---|---|
| (centre), `--ms`, `--rt`, `--ms --rt` | 90,93,108 | = | = | = | = | = | = | = | = |
| `--gi` | 104,91,104 | = | = | = | = | **96,91,104** | **96,91,104** | = | = |
| `--ms --gi`, `--ms --rt --gi` | 104,91,104 | = | = | = | = | **96,91,104** | **96,91,104** | = | **96,91,104**² |
| `--gi-debug` | 66,44,45 | = | = | = | = | **132,76,65** | **132,76,65** | = | = |
| `--ms --gi-debug` | 66,44,45 | = | = | = | = | **132,76,65** | **132,76,65** | = | **132,76,65**² |
| shadow ×3 (`1413,1042`) | 64,79,102 | = | = | = | = | = | = | = | = |
| shadow `--gi` | 67,79,97 | = | = | = | = | **66,78,97** | **66,78,97** | = | = |
| penumbra (`1413,1150`) | 60,73,96 | = | = | = | = | = | = | = | = |
| penumbra `--rt` | **59,72,95** | 60,73,96 | = | 60,73,96 | = | = | 60,73,96 | 60,73,96 | = |
| sunlit (`2200,1400`) | 103,111,126 | = | = | = | = | = | = | = | = |
| sunlit `--gi` | 105,111,123 | = | = | = | = | = | = | = | = |

¹ `no-rt,no-ms,no-cons-raster,no-typed-uav,tier1,msaa=1`.
² WARP's mesh-shader voxelisation runs without conservative raster — see the fix below.
Debug-layer totals were `0 corruption, 0 error` in **every** run of every configuration, no run
crashed, and the `0x141` `LiveKernelEvent` count was **67 → 67 across the whole exercise**.

**`no-dxc` is now bit-identical to baseline at all 17 gates**, which is the headline result of this
phase and is explained next.

### Fixed: the FXC / SM 5.1 path renders the reference image (was §4d item 19)

The brief was to ungate what a working fallback can carry, and this is the case where the gate was
simply wrong. Two independent causes, and the second could only ever have been found by running FXC:

1. **Every Voxi shader asked for SM 6.0, and none of them needs it.** The shadow map, the
   voxelisation pass, the atomic accumulator, the mip filter and the cone-traced lit pass use nothing
   shader model 6 introduced. `createShader` refused all eleven pipelines, `VoxiRenderer::init`
   reported `init FAILED: shadow pipeline has a zero handle` after seven ERROR lines, and the editor
   fell back to `PSMainPlain`, which reads `b1` — where the sandbox pins metallic/roughness to the
   identity because the authored values live in the material's `b2`. Hence `raw(65,77,92)`: every
   surface at metallic 1 / roughness 1, the floor a rough mirror. **The wrong image was a symptom of
   the wrong gate, not a separate defect.** Voxi now asks for `kBaseSm = 51`, which changes nothing
   at all where DXC is present (the backend derives the SM 6.0 target from it and compiles the same
   bytes) and is the whole feature where it is not. Mesh shaders and RayQuery still ask for 65 at
   their own call sites, because they genuinely need it.
2. **`averVertexOf` was overloaded, and FXC could not resolve it.** Voxi declared
   `AverVertex averVertexOf(VoxOut)` alongside the material prelude's `averVertexOf(VSOut)`. FXC
   resolves overloads through implicit conversion between structurally compatible structs, so the
   second declaration made **every** call ambiguous — `error X3067`, in every shader in the
   translation unit including the ones that never call it. DXC is stricter and accepted it. Renamed
   to `voxelVertexOf`; that is the entire fix.

Two supporting changes: `ShaderCompiler::compile` now passes `-D` macros to FXC as well
(`D3D_SHADER_MACRO`), because the material prelude tells every raster shader which registers its
tables landed at and a compiler that could not take macros could not build the scene at all; and the
factory's `CSSelfTest` asks for 5.1 too, so the one path most in need of an end-to-end check stops
being the one path that skipped it.

`--force-caps no-dxc` now logs no ERROR at all. `19(c)` went with it: the
acceleration-structure-slot substitution is stated **once per device, as INFO**, since it is the
normal state of affairs on every GPU without DXR and a WARN per binding set read as a fault on
exactly the hardware the fallback exists for.

### Fixed: WARP no longer faults on conservative mesh-shader rasterisation (was §4d item 18)

§4g left open whether this was WARP's defect or ours and said not to assume. It is **not the
engine's geometry**, and that was established rather than argued, by narrowing inside the shader:

| experiment | result |
|---|---|
| `PSVoxel` body replaced with `return` | still faults → not the pixel shader |
| `MSVoxel` emits nothing (`SetMeshOutputCounts(0,0)`) | clean → the fault is in rasterising what it emits |
| every emitted vertex forced to one fixed, tiny, well-inside-NDC triangle | still faults → **data-independent** |
| that, reduced to ONE primitive per group | still faults |

A single fixed triangle from a mesh shader into a conservative-raster pipeline faults
`d3d10warp.dll` at `0xC0000005`, offset `0x132d9`, every time. WARP executes shaders on the CPU in
this process, so its faults are ours to survive whatever their origin.

The fix is at the pipeline state and is as narrow as the fact: `D3D12Device` records whether it is on
a **software adapter**, and `createGraphicsPipeline` drops `ConservativeRaster` for pipelines that
use a **mesh shader** on such an adapter, logging once with the reason. Hardware is untouched — this
cannot fire on a real GPU. It costs WARP a little voxel coverage on that one path, which is why its
`--ms --gi` gates read the no-conservative-raster values while its GS gates stay bit-identical to
hardware. Keeping the GS path conservative was the point: two completely independent implementations
of D3D12 agreeing to the raw 8-bit code is the strongest check this project has, and blanket-dropping
the flag on WARP would have thrown it away to fix a combination it does not affect.

**`softwareAdapter_` is a device property, not a `DeviceCaps` field**, deliberately. It is not a
capability the adapter reports being without; it is which *implementation* of D3D12 is executing.
Putting it in `DeviceCaps` would have let `--force-caps` clamp it, which would mean nothing.

### Also fixed: an out-of-bounds voxel write hardware was silently discarding

`insideVolume()` is inclusive of 1.0, so a fragment landing exactly on the far face of the volume
truncated to index `res` — one past the last cell — and `PSVoxel` did four `InterlockedAdd`s there.
Conservative rasterisation is what makes it reachable: it generates fragments for partly-covered
pixels and extrapolates their attributes to the pixel centre, and the ground quad's extent matches
the volume's own bounds. A typed-UAV write out of bounds is discarded by the hardware, so this was
invisible on RDNA3 and moved no gate when fixed. Found while investigating the WARP fault; it is
**not** its cause (the fault survives `PSVoxel` being emptied entirely) and is recorded separately so
nobody later reads the two as one.

### Also fixed: a refused render setting says so

`Renderer::setSettings` silently turned off anything the device cannot run, so `--ms` on a device
without mesh shaders left the input assembler running with nothing anywhere saying the flag had been
refused — a run that proved a fallback works looked identical to a run that ignored its own command
line. It now logs once per feature per device, naming the missing capability, and **only for a
feature that was actually asked for**: a device that cannot ray trace and was never asked to has
nothing to say. `IDevice::setMeshShaders` says the same thing at its own level, for a build with no
Voxi at all.

```
[INFO ] [Voxi] Mesh Shaders was requested but this device cannot run it
        (Needs mesh-shader Tier 1 + SM 6.5 (D3D12 Ultimate)); it stays off
```

### Found while doing this, and left OPEN: the editor window sometimes starts at 45x45

Recorded as §4d item 22, and it is the one thing in this section that is not finished.

The "known harness flaw" this project has been carrying — *"launching the gates back-to-back
occasionally yields `raw(14,14,16)`, the editor clear colour, because the window came up at a
different size"* — has now been **measured** rather than inferred. The rect is in the log, and the
runner records it:

```
run 2 : raw (64,79,102) viewport (0,198 2750x1242) in-viewport
run 3 : raw (14,14,16)  viewport (0,198 45x45)     OUTSIDE-VIEWPORT
run 4 : raw (14,14,16)  viewport (0,198 45x45)     OUTSIDE-VIEWPORT
run 5 : raw (64,79,102) viewport (0,198 2750x1242) in-viewport
```

Ten consecutive runs of one gate, no arguments changed. The window comes up **45x45** — not merely
"a different size" — twice in a row, then recovers. Spacing does not prevent it: these were 1.2 s
apart, and the WARP configuration reproduced its own variant at 4 s. In this state the probe's own
self-validation works exactly as designed: it reports `OUTSIDE-VIEWPORT` and an ERROR, and the runner
treats it as a miss rather than a pixel.

**The unexplained part is the other shape it takes**: three times across this phase a gate returned a
plausible but WRONG pixel with a full-size, correct-looking rect — `baseline/shadow-ms-rt`
`raw(65,83,117)` for `64,79,102` (twice in a row, then twelve clean runs), and on WARP
`warp/ms-gi-debug` `raw(64,144,213)` and `warp/gi` `raw(54,138,213)`, both the SKY where geometry or
voxel radiance belongs. Every one exited 0 with `0 corruption, 0 error` and `in-viewport`.

The obvious unified explanation is that the window is still growing when the frame is composed, so
the image is real but framed differently — which the latched rect cannot see, because it is latched
at the request frame and by then already reads full size. That is a **hypothesis**, not a diagnosis.
The alternative is a genuine non-determinism in the render path, and §4c-2 already contains one
worked example of a "wobble" in this exact pass that turned out to be a real last-writer-wins race.

Whoever picks this up: start at the window, not at the shader. `Win32Window`'s DPI-aware,
work-area-clamped sizing is the code that can produce a 45x45 client area, and it runs before the
swapchain exists. Establish whether the small window and the wrong-pixel runs are the same event
before touching anything in the renderer.

Until then the runner repeats a missed gate once and reports `FLAKY` with both values and both
rects, which is loud, does not hide anything, and gives the next occurrence somewhere to be seen.

### What was NOT done, and why

- **Resource-binding Tier 1 is still unmeasured** (§4d item 20). Nothing in the engine branches on
  the tier and the D3D12 runtime validates against the *real* device tier, which is 3 on both
  adapters here. Adding a branch purely so the clamp has something to move would test the branch, not
  the hardware. Left open and honest rather than closed and hollow.
- **The WARP fault is worked around, not diagnosed to a root cause.** Nothing here can see inside
  `d3d10warp.dll`. What is established is that the engine's input is not the variable.

## 4k. Textures reach materials, and the frame gets a camera post chain

Two changes, and the first is smaller than it looks.

### The material system was complete except for one uninstalled callback

`pbr::MaterialSystem` was built to resolve a `TextureRef` through a host-installed
`TextureResolver`, `fmt::loadTexture` already decoded and mip-filtered images, and
`averSampleMaps()` already sampled all five glTF slots. **Nothing ever called
`setTextureResolver()`**, so every slot fell back to its 1×1 identity texture and every surface in
the engine was a flat colour. That was the entire gap: not a missing feature, an unconnected wire.

What landed with it:

- **`assets::uploadTexture`** (`Aver.Assets.Gpu`, a new target) — decoded mip chain → GPU texture.
  Its own target because `Aver.Formats` must not name the RHI and the RHI must not know what a
  normal map is, so the join has to sit above both.
- **The resolver now carries the `TextureSlot`.** Nothing in an image file says whether its pixels
  are colour or data, and the slot is the only thing that knows: base colour and emissive decode
  sRGB, metal-rough and occlusion are linear, a normal map is a vector field that filters
  differently. A resolver given only the reference would have to guess, and guessing wrong is
  invisible — an sRGB-decoded roughness map is merely a bit shinier than authored, everywhere.
- **`.ocmat`** (`Aver.Formats.Material`, a third target) — FORMAT_SPECS §7, parsed straight into
  `pbr::MaterialDesc` with no intermediate struct. A `GRAPH{}` block is detected, reported and
  skipped rather than refused. A third target because `Aver.Render.PBR` is a Core-only DLL the
  scripting layer P/Invokes and cannot link `Aver.Formats`, and pushing the PBR DLL onto every
  headless map tool would be the only alternative.
- **`UvMode::WorldAligned`** — the thing a blockout actually needs. A level built from one unit cube
  scaled to a floor, a wall and a crate has the same 0..1 UVs on all three, so mesh UVs stretch one
  tile across a sixteen-metre floor and cram the same tile into a fifty-centimetre crate. World
  space projected onto the dominant axis of the normal, at a fixed centimetres-per-tile. Dominant
  axis rather than triplanar blending: a blockout is axis-aligned boxes, where projection is exact
  and seamless, and blending would cost three samples per map instead of one.
- The flag and its reciprocal tiling **fit in `MaterialConstants`' last padding word**, so the b2
  block is still 64 bytes and no consumer of that cross-module layout had to change size.

### The post chain — the scene target is HDR now

The scene renders linear radiance into an RGBA16F target and the tonemap is the last thing that
happens to the frame. Eight bits of gamma-encoded colour cannot carry a sun disk worth ~14 or a
specular highlight several times white; both used to clamp to 1 at the moment they were written,
taking with them exactly the range bloom and eye adaptation exist to read.

The chain, all of it skippable:

| Stage | When it runs | Notes |
|---|---|---|
| MSAA resolve | `sampleCount > 1` | in linear HDR; at one sample the scene target IS the resolved image and no copy is made |
| Bloom | `bloomIntensity > 0` | half-res 6-mip pyramid, Karis-weighted prefilter, Jimenez 13-tap down, 9-tap tent up **added by the blender** |
| Histogram + exposure | `autoExposure` | 256 bins of log2 luminance at quarter res, groupshared atomics, single-threaded reduction, log-space damping |
| Composite | always | exposure × bloom → ACES → gamma → backbuffer, one pass, four permutations |

The bloom pyramid is **graphics passes, not compute**, and that is portability rather than taste:
an additive compute upsample has to read its own `RWTexture2D`, and a typed UAV load of RGBA16F is
an optional D3D12 feature this engine has a whole `no-typed-uav` gate configuration for. The
fixed-function blender adds for free and is universal.

**Display-referred colours are inverted back through the chain.** Editor lines, gizmos and the clear
colour name the pixel they want on screen, so `PSLine` writes `averInverseTonemap(srgbToLin(c))` and
`createMsaaColor` bakes the same transform into the target's optimised clear value. The inverse is
exact (the ACES fit is a ratio of quadratics, so inverting it is a quadratic), not an approximation
— a grid whose grey drifts every time the post chain is touched is worse than no grid.

**The defaults are the identity.** Exposure 1, no bloom, no adaptation. A post chain whose default
state changed the image would invalidate the whole oracle for a feature nobody had switched on.

## 4l. Cascaded shadow maps, and the fence wait that was lying

### Four cascades in one atlas

The single 2048² map this replaces was fitted to the **GI volume**, not to the view. That had two
consequences and the second is the serious one: its texels were spread over whatever the volume
happened to be, and there were no shadows AT ALL outside it. In SkyForge — where the GI volume was
still the editor's 44-unit default and the level is centimetres — that box was forty-four
centimetres across, so the arena had no shadows and no GI worth the name.

Now: four cascades, each fitted to a slice of the camera frustum, packed 2×2 into one 4096² depth
texture. An atlas rather than a texture array because the RHI exposes no array dimension and an
atlas needs no interface change; the price is one clamp so a PCF tap cannot wander into the
neighbouring quadrant.

Three details carry the quality:

- **Bounding spheres, not boxes.** A box fitted to a frustum slice changes SIZE as the camera
  rotates, so every texel lands somewhere new every frame and every shadow edge crawls. A sphere is
  rotation-invariant, so the box around it only ever translates.
- **Texel snapping.** Having made the box a constant size, its translation is quantised to whole
  shadow texels. Without this the edges still crawl, just smoothly.
- **Normal-offset bias, scaled per cascade.** A constant depth bias cannot work across cascades
  whose texels differ by two orders of magnitude — tuned for the near one it does nothing far away,
  tuned for the far one it detaches near shadows from their casters. Offsetting the sample position
  along the normal by a fraction of that cascade's own world texel is scale-correct by construction.

The last cascade is **unioned with the GI volume**, because the voxelisation pass samples this same
map for every voxel it injects and the volume is not tied to the camera. A voxel outside every
cascade would inject unshadowed radiance and the bounce would leak through walls.

The cascade RANGE is a multiple of the camera's NEAR PLANE, not an absolute distance. The engine's
contract is centimetres but the editor's placeholder scene is authored at roughly a unit per metre,
so an absolute 200 m would put that whole scene inside the first cascade's near clip and produce no
shadows at all — the same class of mistake the fog density made once. A real `shadowDistance` on
`voxi::Settings` is the honest long-term answer and is a named follow-up, not a silent omission.

`IDevice::camera()` was added for this: the backend already owned the matrices and simply never
offered them back, and having the app push them a second time would be two sources of truth for one
camera.

### The fence wait was treating a timeout as success

The post chain made WARP's debug-view frame slow enough to cross a five-second fence wait, which
exposed a latent bug far worse than the slowness: **on timeout the wait logged an error and carried
on**, and the caller then reset a command allocator the GPU was still reading. That is D3D12 error
#541 followed immediately by device removal — guaranteed corruption, not a risk of it.

A longer timeout would only move the cliff. `waitFence` now waits in one-second slices and ends only
when the fence is reached or `GetDeviceRemovedReason()` says the device is gone, saying so once at
five seconds so a genuine hang is still visible. A slow frame is now slow, not fatal.

## 4m. The sky, the sun and the air became authored — and GI came on

**GI is the default now.** `Settings::globalIllumination` was `Off`, and that one line was the
largest gap between what this engine rendered and what it was already capable of rendering: no
bounce light and no ambient occlusion anywhere, just direct sun plus a flat constant. `--no-gi` is
new and the ten non-GI gates pass it — without that, those ten would silently start measuring the
same path the seven `--gi` gates measure and the oracle would report a confident 153/153 having lost
half its coverage.

**`rhi::SkyAtmosphere`** replaces `setSky`'s five loose arguments and supersedes `setLight` for the
sun. It carries the dome (zenith, horizon, atmosphere height, ground albedo and how much of it
shows), the sun (direction, colour or a colour TEMPERATURE, intensity, angular diameter) and the
air (colour, density, **height** and falloff, start distance, max opacity), plus the cloud layer.

Two things in it are worth knowing:

- **The sun's DIRECTION is the stored field; degrees are the editing form.** `setSunAngles` /
  `sunAngles` convert, and the editor calls them only when a slider actually moves. Deriving the
  vector from angles every frame would push it through two transcendentals and back, and the result
  differs from an authored vector in the last few bits — invisible to a person, extremely visible to
  a pixel-exact oracle.
- **Height fog is solved analytically**, not marched. For `d(z) = d0·exp(-(z-h)·k)` the optical depth
  along a segment has a closed form, so it costs one `exp` and one divide and is exact. `k = 0`
  collapses it to the uniform distance fog this engine had, which is the default — so nothing moves
  until somebody authors a falloff.

**Volumetric clouds** are a single raymarched layer evaluated only on sky pixels: 24 steps, a
3-step light march, Henyey-Greenstein phase, Beer plus a powder term, and analytic value noise
rather than a 3D texture (a texture would need an SRV in a scene root signature that today declares
no descriptor table at all). Two scale bugs were found and fixed by rendering it:

- The marched span is capped by the **noise's feature size**, not by a multiple of the layer
  thickness. A fixed step count over an unbounded grazing span eventually steps further than one
  whole feature, at which point consecutive samples are uncorrelated and the layer renders as
  speckle. It did exactly that first time.
- Extinction is derived from the layer's **thickness**, so `cloudDensity` is a unitless dial that
  means the same thing in a centimetre world and a metre one. Authored per-unit, a density of 1
  through a 1.3 km layer is an optical depth of 130 000 — opaque by four orders of magnitude, which
  is what the first version rendered.

Every default reproduces the previous image: verified oracle-neutral on the `baseline` and `all-off`
configurations, the only failures being the `penumbra` pair that cascaded shadow maps already moved.

**The ground blend defaults to ZERO.** A sky dome that stops at the horizon is more correct than one
that continues under the camera, but a downward reflection vector picking up a ground colour changes
every glancing highlight in the scene — so the ground is something a level author turns on.

## 4n. The oracle stops rotting: relative probes and stated intent

The gates broke three times in one working session, and only the first was a renderer regression.

| what moved | what it did to the oracle |
|---|---|
| the Content Browser became a drawer | the viewport grew, so every absolute probe sampled a different surface — 86 gates failed |
| cascaded shadow maps landed | the PCF path got sharp enough to AGREE with RayQuery at the `penumbra` pixel — the gate kept passing while proving nothing |
| the editor scene was rescaled to centimetres | the scene's composition changed, so `shadow` started reading a lit floor |

Those are **two different failure modes**, and conflating them is why it kept happening.

**Coordinate staleness.** A probe stored an absolute backbuffer pixel, which is only valid for one
window layout AND one scene. The tell was the same both times: the centre probes kept passing while
the hard-coded ones failed, because the engine has always derived the centre from the viewport rect
(`probeX_ ? probeX_ : vpX_ + vpW_*0.5f`) and never derived the others. Fixed: `--probe-rel U V`
takes fractions of the rect, and all ten explicit gates use it. This class cannot recur.

**Premise dissolution.** A probe can stay valid, stable and reproducible while the thing it was
chosen to discriminate stops existing. `penumbra` exists to be the pixel where the shadow map and
RayQuery disagree — proving `--force-caps no-rt` tests something. When the two paths converged there,
the gate reported two identical numbers and would have gone on passing forever. **Recorded values
cannot catch this, because the values were right.** Only a statement of intent can, so `gates.ps1`
now asserts after every run:

- `shadow` must be meaningfully darker than `sunlit` — if they converge, both are sampling the same
  lighting condition and neither brackets anything.
- `penumbra` must differ from `penumbra-rt` wherever ray tracing is actually available.

It caught a live instance immediately: tightening the shadow bias moved the edge and left the
penumbra pair one code apart, which would otherwise have been re-recorded and frozen in.

The picking method is now written into the script rather than reconstructed each time: capture the
scene with `--no-gi` and with `--no-gi --rt`, take the darkest agreeing floor pixel, the brightest
agreeing one, and the largest disagreement, each screened for a flat 7x7 neighbourhood. `penumbra`
unavoidably sits on a shadow edge — that is the only place the two paths ever differ — which is
exactly why it needs the invariant watching it.

## 4o. The game UI — a draw list, a renderer, a C seam, and two RHI bugs it exposed

Dear ImGui is the EDITOR's and stays the editor's. A shipped game should not link an editor UI
toolkit, and a HUD authored as immediate-mode C++ has nothing a designer can open, diff or hand to an
artist. So there are three targets, split on the same line `Aver.Render.Voxi` is split on:

| target | kind | links | owns |
|---|---|---|---|
| `Aver.UI` | STATIC | `Aver.Core` ONLY | `UiDrawList`: five named layers, one shared vertex/index buffer, clip stack, batching |
| `Aver.Render.UI` | STATIC | `Aver.RHI` (**never** the backend) | the pipeline, the per-frame upload, the draw loop |
| `Aver.UI.Abi` | **SHARED** | `Aver.Core` + `Aver.UI` statically | 11 exports, no handles, no version |

Keeping the RHI out of `Aver.UI` is the point of the module rather than an accident of what it
happens to need: the whole UI system is then testable with no GPU and no device, and it can never
quietly become a second place that knows how to render. `tests/ui` (26 checks) and `tests/render.ui`
(60) both run headless, the second against a **recording device** rather than a screenshot — which is
what let it assert the blend mode, a thing no capture can see.

**Layers are named bands, not numbers** (`Background`, `Content`, `Overlay`, `Tooltip`, `Debug`), and
`set_layer` IGNORES an out-of-range band rather than clamping. Clamping would silently move a widget
to a band its author did not choose, and `Debug` becoming `Tooltip` is a shipped debug overlay.
Vertices and indices are shared across layers — one upload rather than five — and the layers
partition only the COMMANDS.

**The frame belongs to the host.** `aver_ui_begin_frame` clears the list and records the rectangle
the UI is laid out against; the host calls it once and a game must never. There is ONE list, not one
per caller: a per-caller list would let two systems each build a HUD and neither see the other's,
which is not composition but two UIs racing for the same screen. The viewport is PASSED IN rather
than queried, because the module has no device and no window, and it is deliberately not the window —
in the editor the game draws into a dockspace panel, and a HUD anchored to the window would sit
partly under the editor's own chrome.

`Aver.Render.UI` runs from `IRenderFeature::overlayPass`, which is downstream of the tonemap and
after the backbuffer is bound: a white panel composited into the HDR scene target would watch eye
adaptation stop down the entire frame because the UI is the brightest thing in it. It runs BEFORE the
editor's own ImGui, so editor chrome composites over a game HUD rather than under it.

C# half: `scripting/csharp/Aver.UI` (`Layer`, `Colour`, `Rect`, `Hud`), P/Invoking `"Aver.UI.Abi"`.
The native file name and the managed assembly name differ **on purpose** — `Aver.Scene` and
`Aver.Framework` each ship a native DLL and a managed assembly with the SAME file name, so `DllImport`
probes the calling assembly's directory, finds the managed one and tries to load it as a native
library; both work around it with a `NativeResolver`. Naming these apart removes the problem instead.

**`--ui-demo` turns the game UI on for a capture run, and it is OFF by default.** That is not
politeness: the gates compare backbuffer pixels and a HUD over the viewport would move every one of
them. The demo is hand-written and meant to be deleted — there is no widget tree yet, so it is the
draw list a widget tree will eventually produce.

### Two RHI bugs, and the one that had been crashing the editor since vsync-off landed

**Every resize failed, on any machine that supports tearing.** `ResizeBuffers` was passed `flags=0`,
which was right for as long as the swapchain was created with no flags and stopped being right the
moment vsync-off added `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` at creation. DXGI does not reinterpret a
resize as a request to drop a capability — it returns `E_INVALIDARG`.

The failure was not the crash. This was:

```cpp
for (auto& rt : renderTargets_) rt.Reset();     // required before ResizeBuffers
if (!hrOk(swapChain_->ResizeBuffers(...), "...")) return;
```

The back buffers are released BEFORE the call that can fail, because `ResizeBuffers` requires it. The
early return therefore left `renderTargets_` full of nulls, `width_`/`height_` unchanged and no views
rebuilt; the next `beginFrame` handed a null resource to a barrier, which is not a reported error but
a fault inside the driver. That is why six crashes across four hours all landed at the identical
offset inside `amdxc64.dll` and nothing in the engine's log ever named a cause. A failed
`ResizeBuffers` leaves the swapchain UNCHANGED, so re-acquiring its buffers puts the device back where
it was: the window is then the wrong size for the swapchain, which is a stretched frame — visibly
wrong, recoverable on the next resize, and what an error path should cost. `modules/rhi.d3d12/src/D3D12Device.cpp`,
`D3D12Device::resize`.

**Reproduced, not reasoned about**: driving the window through 3840x2160, 900x600, 2560x1080,
1600x900 and a maximise killed the editor at the first before, and survives all five after. Maximise
alone never reproduced it — at this DPI the maximised size already equalled the swapchain size, so
`resize()` early-returned and the bug never ran.

**The UI was applying alpha twice.** `BlendMode::AlphaBlend` is `SRC_ALPHA`/`INV_SRC_ALPHA` — straight
alpha — and `UiDrawList` premultiplies every colour on the way in. An opaque draw is unaffected, which
is why the demo's bars looked right; a 38%-alpha white lands at 31/255 instead of 83/255, less than
half the intended brightness, on something that still looks like a plausible translucent panel.
`BlendMode::PremultipliedAlpha` (`ONE`/`INV_SRC_ALPHA`) is the correct pairing and is what the UI
pipeline asks for. `UiRenderTest` asserts the mode specifically, because the wrong one is invisible in
a screenshot.

**A resize no longer rebuilds every pipeline.** `notifyRenderTargetsChanged` fired from both
`setSampleCount` and `resize`, and a resize changes none of the three things a pipeline BAKES (sample
count, the two target formats). Voxi's rebuild recompiles every scene shader from HLSL source, so
dragging a window edge recompiled the entire renderer once per resize message. Now gated on the values
actually changing. Found while hunting the crash; not its cause.

### VERIFIED ON SCREEN — the one thing in this phase that was

Recorded in `6f8516a`. A capture run with `--ui-demo` was probed for the colours the demo draws, at a
tolerance of 2/255, and each was found at its authored value **and** at the coordinate the layout
predicts:

```
health   (232,76,46)   558 hits, bbox (34,1670)-(218,1680)
stamina  (232,200,63)  732 hits, bbox (34,1690)-(276,1700)
crosshair(220,228,232)  14 hits, bbox (1364,1042)-(1384,1062)
tooltip  (106,196,106) 368 hits, bbox (2460,548)-(2550,562)
```

The first attempt found only two of them, because the demo was laid out underneath the editor's own
status overlay — which composites over the game UI by design — so the demo moved rather than the rule.

**What this does NOT establish.** It is a one-off manual run, not a gate: `scripts/gates.ps1` has no
`--ui-demo` configuration and `scripts/gates.baseline.txt` records nothing about the UI, so nothing
would catch a regression here. There is also **no text and no widget tree** — text is blocked on a
font decision (vendor `stb_truetype.h`, or bitmap fonts) that has not been made.

## 4p. Audio — a mixer that can be tested, and a device that cannot

Same split, and audio needs it more than the UI does: a wrong pan law and a right one are the same
waveform to a reader and the same silence to a screenshot, so a mixer that cannot be run headlessly
and asserted on sample by sample cannot be checked at all. `Aver.Audio` is **Core only** — `mix()`
fills a buffer the caller supplies. `Aver.Audio.Wasapi` is the only thing in the stack that knows a
sound card exists. `Aver.Audio.Abi` is SHARED and links the DEVICE rather than just the mixer, because
"play a sound" only means anything once something is driving hardware. Plan: `docs/AUDIO.md`.

- **WASAPI shared mode over XAudio2**, and the reason is testability rather than taste: XAudio2 would
  own the mixing, the 3D and the DSP, which would make `Aver.Audio` a wrapper around a thing it can
  neither test nor port. The render thread joins the Pro Audio scheduling class (`avrt`) — an ordinary
  thread preempted for 10 ms is a gap somebody hears.
- Voices (64 by default), buses, master volume, per-voice volume/pitch/position, voice stealing with a
  `stolenVoices` counter, and an `underruns` counter. Positional voices are panned and attenuated;
  non-positional ones play at `volume` in both ears.
- **Constant-power pan** (`l*l + r*r == 1`), so a source swept across the field holds its energy.
  Attenuation is EXACTLY 1 at or inside the inner radius and EXACTLY 0 at or beyond the outer — every
  game clamps it somewhere, because inverse-square goes infinitely loud as the listener arrives.
- The listener is nine atomics rather than a struct behind a lock: written once per frame, read by the
  render thread, and the worst a torn read costs is very slightly wrong panning, which is inaudible,
  where a lock costs a priority inversion on an audio thread.

`AudioTest` is 70 headless checks. **No sound has been heard.** There is a `build/bin/AudioProbe.exe`,
but nothing in this document records a listening test, and a mixer that passes 70 assertions and
outputs silence would pass them all the same way.

### `.ocaudio`

An AVR1 container (`AHDR` header chunk + `APCM` sample chunk), `modules/formats/src/OcAudio.cpp`.
Importers: **an own WAV reader** (`Wav.hpp`/`.cpp`), plus **Media Foundation** for mp3/m4a/aac/wma/flac
— so mp3 and the rest arrive without vendoring a decoder, which the permissive-licence rule would
otherwise have made a research project. `Aver.Formats.Audio` is its own target linking
`mfplat mfreadwrite mfuuid ole32`, so nothing else in the tree pays for Media Foundation.

**`OcAudioTest` passes but SKIPS the Media Foundation check.** It says so:
`skip  Media Foundation NOT exercised: pass a .mp3/.m4a/.flac/.wma path to do it`. The WAV path and the
container are tested; **the mp3/m4a/aac/wma/flac path is exercised by nothing in CI**, and the skip is
silent in an exit code. That is a real gap, not a formality — see §4d item 25.

### `docs/ABI.md` is now stale, and it is the one place that counts

It opens with "**seven** separate C surfaces" and "201 exported C functions declared across seven
headers". There are **eight**: `modules/audio.abi/include/aver/audio/audio_abi.h` declares 24 exports
and the word "audio" does not appear anywhere in `docs/ABI.md`. Fixing it is a separate edit to a
separate file and has not been made.

## 4q. Materials are authored in C#, and the `.ocmat` becomes build output

The direction of the arrow is the whole decision. **A `.cs` under `Content/Materials` is the SOURCE**;
`Binaries/Materials/*.ocmat` is the build output; the engine reads Binaries FIRST. Resolution order,
`sandbox/src/SandboxApp.cpp`:

```cpp
project_.binariesDir() + "\\Materials\\" + name + ".ocmat",   // build output, authoritative
content + "\\Materials\\" + name + ".ocmat",                  // a stale hand-authored file
content + "\\" + name,
```

Binaries first so a stale hand-authored `.ocmat` left beside the source cannot shadow the thing that
is rewritten from source on every build.

- `scripting/csharp/Aver.Materials` — `[AverMaterial]`, `MaterialBuilder`, the `.ocmat` emitter.
- `scripting/csharp/Aver.MaterialCompiler` — **`avermatc`**: reflects over a built assembly, finds
  every `[AverMaterial]` type, runs its `Configure` and writes the `.ocmat`. Staged to `bin/Tools/`
  and run automatically by **Tools ▸ Compile C#**, so a surface becomes an `.ocmat` without anybody
  running a command (`modules/scripting/CMakeLists.txt`, `sandbox/src/ToolsMenu.cpp`).
- `fmt::rewriteMaterialScript` (`modules/formats/MaterialScript.hpp/.cpp`) rewrites a C# material's
  `Configure` from an edited `MaterialDesc`.

**The Details panel's "Save to C#" writes the `.cs` and never the `.ocmat`.** The `.ocmat` under
Binaries is an artefact the next compile overwrites, so writing there is a change that appears to work
and then silently vanishes — the worst possible behaviour for a save button. The sliders above it are
already live (they edit the material the renderer is using, so the viewport shows the change
immediately); the button is what makes it PERSIST, and Compile C# then regenerates the `.ocmat` from
the source just written.

One trap recorded for whoever touches the formatter: `MaterialScript.cpp`'s float writer walks
`%.1g`..`%.9g`, so values outside `%g`'s fixed range come back as `1e+07f`. That is fine here — C#
accepts exponent literals — and is **not** fine for the actor rewriter, whose locked grammar has no
exponent form (§4r, `docs/DESIGNER_REWRITE.md`).

## 4r. The actor editor — built, linked, registered, and (as of §4u) SEEN

**This heading used to read "NEVER OPENED BY A HUMAN", and at `d04fe1a` that was exact.** It is not
any more: §4u opened the tab repeatedly, screenshotted it against real projects, and found nine
defects by doing so.

The original warning is kept because the lesson is not: a subsystem that compiles, links, registers
and passes tests can still never have been looked at, and this one was the largest such surface in
the tree. The section below describes the architecture as built; its current state is §4u.

An asset editor, separate from the level editor the way Unreal separates them. A `.cs` opens as an
ASSET rather than as text: the placements it declares, drawn in local space, with the numbers beside
them.

### `Aver.Render.ActorPreview` — a feature, not a second viewport

It owns a colour target, a depth target, one pipeline and one camera, and **publishes that camera at
`b4`, not `b0`** (`rhi::kFeatureFrameConstantRegister`). `b0` is the engine's `PerFrame` block and
belongs to the scene's frame; a preview that wrote it would either fight the scene for it or draw with
the previous frame's camera, silently. Reasoning in `docs/ACTOR_EDITOR.md` §3.

It links the generic RHI and never a backend, so a recording device can drive it with no GPU — the
same property that made `Aver.Render.UI` testable. `ActorPreviewTest`: 51 checks, headless.

**Its own mesh registry** (`PreviewMeshCache`), on purpose. The editor already has one — a private
member of the app, filled by a sweep of the content root when a project opens — and the preview
deliberately does not use it. An asset editor that reached into the level editor's state cannot exist
without a level open, cannot be tested without one, and makes "this tab is independent of the level"
false. Loaded ON DEMAND (a preview needs the three to six meshes one actor names; the sweep exists
because a LEVEL may reference anything), and **a miss is cached too, as handle 0** — without that, an
actor naming a mesh that is not there re-reads the disk once per model per frame and the editor's
frame time becomes a function of how wrong the file is.

**ONE preview, shared, driven by the active tab**, and the reason is a hard limit rather than thrift:
the UI descriptor heap holds sixteen slots and the editor already spends five, so a target per tab
exhausts it at about eleven and the failure is a black image, not an assert.

### `ActorScript` — reads two halves, writes one

`modules/formats/ActorScript.hpp/.cpp`.

- **Reads** the `.Designer.cs` generated region (the placements) AND what a class declares in
  `Configure(ClassBuilder)` — mesh, camera, point light. `docs/DESIGNER_REWRITE.md` used to say the
  editor never reads or writes a byte of the hand-written half; that is **amended, in that document,
  to never WRITES**. The write rule is untouched and absolute. A rule about writing had been stated as
  a rule about looking, and honouring it literally would have meant refusing to preview the common
  case — most actors in most games are one mesh declared with `b.Mesh(...)` and no designer file at
  all, and every actor in the SkyForge template is that shape.
- **Writes** position, rotation and scale only, matched by `ObjectId`. Mesh, material and id are
  untouched by a save.
- `canonicalMeshPath()` reconciles the `"Content/Meshes/X"` vs `"Meshes/X"` spellings the tree
  disagrees on, so the two collapse to one cache entry and one upload rather than hashing differently.

**Two parser bugs a fixture would never have found**, both from running against a real project:
attribute kinds were searched in turn, so the parser found whichever kind came first in the SEARCH
rather than first in the FILE (`FpsGameMode.cs` came back named `BP_FpsController`); and a file was
assumed to declare ONE actor, where SkyForge's `FpsGameMode.cs` declares five and `Gun.cs` declares
two. Each entry now carries only what appears between its own attribute and the next, so two actors in
one file cannot borrow each other's mesh, and the tab shows a picker. That sweep is now
`tools/ActorSweep.cpp` and a build target, **because a fixture is written by the same person who wrote
the parser and agrees with it by construction.**

### The tab

- The factory DECLINES anything that is neither a `.cs` carrying a generated region nor a class
  declaring a mesh, camera or point light. A hand-written actor with nothing previewable belongs in the
  IDE and opening it here would be taking something away. A region it cannot READ is declined
  **loudly** rather than opened as an empty tab that cannot save.
- **A translate gizmo**, drawn as an ImGui overlay projected through the preview's OWN camera, not as
  geometry in the pass — the preview feature must stay drivable with no ImGui (that is what lets a test
  be the device), and a handle has to be pickable at a constant SCREEN size, which it cannot be if it
  scales with the scene. The axis is latched on mouse-down and held for the whole gesture: deciding per
  frame lets a drag that began on a handle become an orbit the moment the cursor leaves it, which is
  exactly when a user is dragging fastest. Picking is against the whole SEGMENT, or the near end of
  every axis is dead.
- **Source-to-view reload is one `stat` per visible tab, NOT a watcher.** `docs/ACTOR_EDITOR.md`
  planned `DirectoryWatcher`; that was rejected on contact. It has zero consumers and zero tests in
  this tree, its `poll()` returns true to mean "the OS dropped records, rescan yourself" — a case
  nothing handles — and `dotnet build` runs with its working directory inside `Content\Scripts`, so a
  recursive watch covers that project's own `obj\` and `bin\` and **a build is exactly the burst that
  overflows it**. One stat is cheaper than a thread, an OS handle, a filter list and an overflow path,
  and it cannot lose an event.
- **A reload is REFUSED while the tab is dirty, and says so.** Silently replacing somebody's
  in-progress drag with what a background tool wrote is the one behaviour a live sync must never have.
  A file mid-write that fails to parse leaves the previous good state on screen rather than blanking
  the tab.
- Missing meshes are named in the panel: an actor whose meshes are all absent renders an EMPTY view,
  and an empty view with no explanation is indistinguishable from a broken preview.

**NOT DONE:** no rotate or scale handles, only translate. No Roslyn backend — the grammar is the
hand-written scanner's, and `Malformed` is the signal a Roslyn backend would pick up. No pixel seen.

## 4s. Projects: a new one is complete, an old one is merged rather than regenerated

`sandbox/src/ProjectScaffold.cpp`.

A new project gets `Content/{Maps,Meshes,Materials,Textures,Sounds,Scripts}`, a `Scripts.csproj`
written **at creation** rather than on the first `.cs` (the old behaviour left the folder as a loose
file with every `Aver.Scripting` symbol underlined red), a starter `M_Default` material, and **four**
engine references:

| reference | why it is there by default |
|---|---|
| `Aver.Scripting` | behaviours |
| `Aver.Framework` | actors — and it pulls `Aver.Scene` in behind it |
| `Aver.UI` | the game's HUD; a leaf that references nothing |
| `Aver.Materials` | the material authoring surface — `Scripts.csproj` globs `Content\Materials` |

`Aver.Materials` is referenced even though a new project has no materials yet, for the same reason
`Aver.Framework` is: the alternative is an author who has to add a `ProjectReference` by hand before
the second thing they try works.

**Opening an older project offers an upgrade, and the `.csproj` is MERGED, never regenerated.**
Regenerating would discard whatever the author had added to their own build file — package
references, analysers, a target — which is a data-loss bug wearing the costume of a convenience. The
upgrade adds only the missing directories, the missing references and the missing starter material.

Two new CLI entry points, both of which scaffold and EXIT without touching a device, so project
creation is reachable without a mouse: `--new-project <location> <name>` and
`--upgrade-project <path.ocproject>`.

## 4t. What this phase verified, and what it did not

The rule this project keeps re-learning is that "the tests pass" and "it works" are different claims.
Both are recorded here.

**Verified — every headless suite re-run from `build/bin` on 2026-07-28 with the tree at `d04fe1a`,
each exiting 0.** Only the first three print a total; the rest print one line per check and the counts
below are of those lines, which is why they are stated as checks rather than assertions.

| suite | result |
|---|---|
| `SceneTest` | 538 assertions, 0 failed |
| `FrameworkTest` | 218 assertions, 0 failed |
| `PhysicsTest` | 42 assertions, 0 failed |
| `AudioTest` | 70 checks |
| `UiRenderTest` | 60 checks |
| `ActorScriptTest` | 51 checks |
| `ActorPreviewTest` | 51 checks |
| `UiTest` | 26 checks |
| `MaterialTest`, `JsonTest`, `MeshTest`, `GltfTest`, `FormatTest` | pass, no per-check count printed |
| `OcAudioTest` | passes, **1 check SKIPPED** — Media Foundation not exercised |

**Verified on a screen:** the game UI, once, by pixel probe (§4o). That is the whole list.

**NOT verified, and each of these is a specific hole rather than a general caveat:**

- **The actor editor tab has never been opened.** §4r.
- **No sound has been played.** §4p — the mixer is asserted sample by sample and has never driven a
  speaker in a way anyone recorded.
- **The Media Foundation import path is not covered by any suite.** §4p.
- **`./scripts/gates.ps1` has not been run across any of this work.** `scripts/gates.baseline.txt` was
  last touched at `d8fc062`, thirty-nine commits ago. §8 says in as many words to run the whole oracle
  before calling a renderer change safe, and this phase changed renderer code — the blend-mode enum,
  the resize path, the pipeline-rebuild gating. A sweep is owed. Two things make it awkward and
  neither excuses it: the probes are layout-fragile and the editor gained an asset-editor tab strip,
  and the renderer has been measured NOT to be bit-deterministic run to run, which the pixel-exact
  oracle assumes. Both belong in §4d.

## 4u. The editor phase — the tab opened, and nearly everything it touched was wrong somewhere

Seventeen commits, `8e62406` to `e9ba7f5`. The theme is not a feature. It is that **opening the tab
and looking at it** found defects that compiling, linking and passing tests had not.

### The steps, in order

| # | commit | what |
|---|---|---|
| 1 | `8e62406` | **Live view.** `aver_fw_spawn_preview` / `aver_fw_destroy_preview` split the spawn edge: bind + `build_models`, and no `OnBeginPlay`. Ten new assertions in `FrameworkTest`. |
| 2 | `c96b51d` | **The watcher gets a consumer.** `DirectoryWatcher` had shipped complete with zero callers. Wired to the content root; `AssetEditor` gains `onFileChanged` / `onWatchLost`. `WatcherTest` is new. |
| 3 | `44de451` | **Roslyn.** `averdesign`, the repo's first NuGet consumer, invoked from `Aver.Formats.Roslyn`. Runs only on `Malformed`. `RoslynTest` pins agreement with the built-in scanner. |
| 4 | `6970f55` | **Viewport input.** Every gate tested `!io.WantCaptureMouse`, which stopped discriminating anything once the level became a window. |
| 5 | `55c7bcf` | **Auto-compile on save**, debounced, with the `bin/` + `obj/` exclusion that stops it building forever. |
| 6 | `7b15fe8` | **Component tree**, UE-style, plus camera-frustum and light-range wireframes. |
| 7 | `78b4246` | **Three columns**, and a resizable non-square preview target. |
| 8 | `b2f854f` | **Draggable dividers**, and an actor tab that hides the level's panels. |
| 9 | `1d01471` | **Gizmo projection** takes both extents rather than one. |
| 10 | `b6c4717` | **`EditorPrefs`** — the editor had nowhere at all to store UI state. Reset Layout scoped to the active tab. |
| 11 | `ff9e782` | **Editor Preferences persist**, and the IDE choice stops being a scan-order index. |
| 12 | `760f053` | **`writeOcproject`** — project settings into the manifest. `Gun.cs` opens on `Gun`. |
| 13 | `dc1c886` | **`[AverHud]`** — scripting contract v2 to v3: `HudCount` / `HudName` / `HudDraw`. |
| 14 | `c62e76a` | **`--save-project`**, writing declared defaults rather than device-clamped ones. |
| 15 | `035b889` | **The game UI had never been visible.** A composition-order fix. |
| 16 | `b288c70` | **Audio import** behind the Content Browser's button; `MakeSamples` generates a template's sounds. |
| 17 | `e9ba7f5` | **Any valid actor opens**, and `BP_` stops being presented as a name. |

### The nine defects, and how each was found

Every one was invisible to the suites, which is the point of listing them.

1. **The game UI had never reached a screen.** `overlayPass` drew to the backbuffer; ImGui then
   painted the viewport texture over that exact region. Found because `--ui-demo` drew nothing, then
   confirmed by stashing every change and screenshotting a clean HEAD build. `035b889`
2. **Viewport input was entirely dead** — fly, dolly, pan, picking and the gizmo. `WantCaptureMouse`
   is `1` over a docked window, so the gate meaning "is the mouse on the scene" always answered no.
   Measured with a real cursor driven over a running editor. `6970f55`
3. **`DirectoryWatcher::start()` dropped every change** until its worker's first read was
   outstanding, while `watching()` said true. Found by `WatcherTest` on its first run. `c96b51d`
4. **`parseActorClasses` only saw attributed classes**, while the runtime registers unattributed ones
   perfectly well. Found by writing a nine-shape test; five shapes failed. `e9ba7f5`
5. **`Gun.cs` opened on `BP_GunPart`** — the first *drawable* class in file order rather than the one
   the file is named for. `760f053`
6. **`writeOcproject` appended a blank line per save.** A `<=` loop consuming a phantom segment after
   the trailing newline. Caught by asserting byte-for-byte idempotence rather than "it parses".
   `760f053`
7. **The manifest applied before the device attached**, so `setSettings` clamped GI against
   capabilities the renderer did not yet know it had — quality went to Off while the two settings
   that are not capability-gated survived. `760f053`
8. **The right splitter clamped a divider POSITION against a WIDTH budget**, silently widening any
   right column narrower than about a third of the tab. Only surfaced once widths persisted.
   `b6c4717`
9. **The three-column threshold never fired at 300% DPI** — it compared the side panels to a multiple
   of themselves instead of asking whether a viewport still had room. `78b4246`

### Standing traps this phase re-confirmed

- **`voxi::Renderer::setSettings` CLAMPS to the device.** Never persist what you read back; keep the
  requested value separately, or one open on a weaker machine downgrades the project for the team.
- **MSBuild rewrites `.cs` under `obj/` on every build.** Any watcher-driven build must exclude it,
  or it compiles in a loop at whatever rate `dotnet` manages.
- **A `--` inside an XML comment breaks a `.csproj`** (`MSB4025`). Hit twice this phase.
- **`atof` is locale-dependent.** `EditorPrefs` and the `.ocproject` writer both use
  `from_chars` / `to_chars`; a comma-locale machine would otherwise write `230.5` and read back 230.
- **A HUD or actor preview must not run `OnBeginPlay`.** SkyForge's game mode spawns eleven actors
  there. That is what the preview spawn edge exists to prevent.

### Verified this phase — 2026-07-29, tree at `e9ba7f5`

| suite | result |
|---|---|
| `SceneTest` | 538 assertions, 0 failed |
| `FrameworkTest` | **228** assertions, 0 failed (was 218; the preview edge added ten) |
| `PhysicsTest` | 42 assertions, 0 failed |
| `WatcherTest` | **new** — 33 checks against a real filesystem |
| `EditorPrefsTest` | **new** — 22 checks |
| `RoslynTest` | **new** — agreement with the scanner, and the cases it declines |
| `ActorScriptTest` | 51, plus nine-shape coverage of what the tab accepts |
| `ActorPreviewTest` | 51, plus the resize path |
| `AudioTest`, `UiRenderTest`, `UiTest`, `MaterialTest`, `JsonTest`, `MeshTest`, `GltfTest`, `FormatTest` | pass |
| `OcAudioTest` | passes, **1 check still SKIPPED** — Media Foundation not exercised |

**Verified on a screen, by screenshot:** the actor tab against `Car.Designer.cs`, `FpsCharacter.cs`,
`Gun.cs` and a camera/light probe actor; the three-column layout at 300% DPI; the level's panels
hiding and returning; Project Settings applying a manifest; Editor Preferences round-tripping;
`--ui-demo`; and SkyForge's `PlayerHud` drawing through `[AverHud]`.

**STILL NOT verified, and each is specific rather than a caveat:**

- **No sound has been played.** The template now holds four `.ocaudio` files and nothing references
  them. §4p stands unchanged.
- **The Media Foundation import path** remains uncovered; the `.wav` arm runs on every invocation.
- **`./scripts/gates.ps1` fails 19 of 19**, and was measured failing *identically* on a clean HEAD
  build with every change stashed — so the breakage predates this phase. Two are **invariant**
  failures: five gates defined to differ return one value, which says the probes sample the wrong
  pixels rather than that shading regressed. **Not re-recorded.** See §4d and the memory note
  `aver-gates-red-at-head`.
- **No editor tab hosts a HUD yet.** `--hud-preview` proves the seam; the UI editor is not built.
- **The actor tab's own Save does not compile** unless Auto-compile on Save is on. That is by design,
  but it is the first thing somebody will expect otherwise.

---

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
- **A hosted component is NOT in the default `AssemblyLoadContext`.**
  `load_assembly_and_get_function_pointer` gives the bridge its own isolated context, driven by its
  `deps.json`. A collectible child context whose `Load` returns null therefore falls through to
  Default and does **not** find `Aver.Scripting` — the error reads as a missing file. Delegate to
  `AssemblyLoadContext.GetLoadContext(typeof(HostBridge).Assembly)` instead, which also keeps
  `AverBehaviour` to one runtime identity. Do not "simplify" that back to `return null`.
- **Nothing in the engine may link nethost or hostfxr.** Both are `LoadLibraryW`'d at run time. An
  import in the executable's table fails the PROCESS at load time on a machine with no .NET, which
  is precisely the case `ScriptHost::init` exists to decline for.
- **A managed exception must not escape an `[UnmanagedCallersOnly]` method** — it terminates the
  process rather than becoming a C++ exception. Every bridge entry point is wrapped whole, and each
  behaviour hook is wrapped individually inside the loop.
- **`HostBridge.DrainAndUnload` must stay `[MethodImpl(MethodImplOptions.NoInlining)]`.** A
  collectible `AssemblyLoadContext` is only collected once no stack frame holds a reference to it,
  so inlining it into the caller keeps the local alive for the whole calling frame and the collect
  that follows reports a leak the inlining alone created. The method returns a `WeakReference` for
  the same reason: nothing strong may cross back out.
- **Never unload scripts before the rebuild succeeds.** `Tools ▸ Reload Scripts` builds first and
  drains only on exit code 0. Draining first leaves the editor with no scripts at all because of a
  typo, which is worse than carrying on with the ones already running.
- **`ScriptHost::loadScripts` is ADDITIVE.** Calling it twice without an `unloadScripts()` between
  gives every behaviour in the directory two live instances, both ticking. This is why
  `applyProject` is guarded on `scripts_.ready()` — the command-line project path already loaded
  through `resolveScriptsDir()` before the host existed.
- **A pixel probe is not a screenshot.** `--probe` reads one pixel and misses overlays entirely; the
  centre probe in particular is blind to the sun term (see §4c-2). Shadow and ray-tracing checks
  must use `--probe 1413 1042` (cast shadow) or `--probe 1413 1150` (penumbra), and BRDF work must
  use `--probe 2200 1400` (sunlit floor, the only gate that receives direct specular).
- **Run `./scripts/gates.ps1` before calling a renderer change safe**, not just the primary
  configuration. Nine device configurations pass through code the primary path never reaches — the
  FXC/SM 5.1 compiler, the geometry-shader voxelise path, the shadow-map fallback for `--rt` — and
  all three of those were broken at some point while every hardware gate stayed green.
- **A shader that does not need shader model 6 must not ask for it.** `ShaderDesc::minShaderModel`
  is a floor, not a preference: where DXC is present the backend derives the SM 6.0 target from
  whatever is asked, so requesting 51 costs nothing there and is the difference between a working
  renderer and no renderer at all where DXC is absent. Only mesh shaders and inline RayQuery
  genuinely need 6.5.
- **Do not overload a function the material or RHI prelude already declares.** FXC resolves overloads
  through implicit conversion between structurally compatible structs, so a second
  `averVertexOf` made every call in the translation unit ambiguous (`X3067`) while DXC accepted it
  silently. Preludes are a shared namespace with two compilers behind them; give new helpers new
  names.
- **`insideVolume()` is inclusive of 1.0.** Anything turning volume UVW into an integer voxel index
  must clamp to `res - 1`. Hardware discards an out-of-bounds typed-UAV write, so getting this wrong
  is invisible on a GPU and an access violation on WARP.
- **`ResizeBuffers` must be passed the SAME flags the swapchain was created with.** DXGI returns
  `E_INVALIDARG` rather than reinterpreting the call as a request to drop a capability, so adding a
  creation flag (`ALLOW_TEARING`, for vsync-off) and leaving the resize at 0 breaks EVERY resize. And
  because the back buffers must be released before the call, **a failed `ResizeBuffers` must not
  early-return** — re-acquire the old buffers and rebuild the views, or the next `beginFrame` hands a
  null resource to a barrier and faults inside the driver with nothing in the engine's log. §4o.
- **Premultiplied colour needs `BlendMode::PremultipliedAlpha`** (`ONE`/`INV_SRC_ALPHA`), not
  `AlphaBlend` (`SRC_ALPHA`/`INV_SRC_ALPHA`). `UiDrawList` premultiplies on the way in. The wrong
  pairing applies alpha twice, leaves opaque draws untouched, and produces a translucent panel that
  still looks plausible at less than half its authored brightness — so it is invisible in a
  screenshot and has to be asserted in a test.
- **A game HUD is drawn from `IRenderFeature::overlayPass`, downstream of the tonemap.** Compositing
  UI into the HDR scene target makes eye adaptation stop down the whole frame, because the UI is
  reliably the brightest thing in it.
- **`aver_ui_begin_frame` belongs to the HOST, and there is exactly one draw list.** A game that
  cleared the list would erase whatever another system had already contributed, and the last caller to
  run would win with nothing anywhere to say so.
- **The actor editor never writes the hand-written half of a `.cs`.** It READS `Configure(ClassBuilder)`
  to know what to preview; it writes only pos/rot/scale inside the generated region, matched by
  `ObjectId`. And do not reuse `MaterialScript.cpp`'s float formatter there — it emits `1e+07f` for
  values outside `%g`'s fixed range, and the locked designer grammar has no exponent form.

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
5. ~~**Materials/textures** — `.ocmat` + `.octex`, texture sampling in the shader~~ **DONE**, see
   §4k: `.ocmat` loads, textures resolve and upload, and world-aligned UVs give a blockout a
   constant texel density. `.octex` (the cooked, block-compressed form) is still not a thing —
   PNG/JPEG/TGA/BMP decode straight to RGBA8 with a CPU-generated mip chain, so a shipping title
   would want the cooker before it wanted anything else here.
6. **Move OpenConstructor bits behind modules** — `Aver.SoftBody/Aver.Vehicle/Aver.Aero`
   as opt-in; `.ocbeam` load stays in `Aver.Formats` but the editor treats it as one importer.
7. **Editor generalization** — "Add" menu (spawn primitives/lights), open/save scene, an
   asset browser; not hardcode the OC default scene.

**Other queued work:** grab-able gizmo handles; dockable panels (ImGui DockBuilder);
shadow maps; then Phase 4 soft-body (make the cage deform), Phase 6 net (needs OCServer
Wire.cs), Phase 7 C# editor + Rust pipeline.

**Items 1–4 are stale and were never struck off.** `Aver.Scene` exists and `SceneTest` is 538
assertions; `.ocmesh`/`.ocskel`/`.ocanim` exist with `MeshTest` behind them; the glTF/GLB importer
exists as the C++ path rather than Rust, behind the Content Browser's Import button; `.ocworld` has
both `loadOcworld` and `saveOcworld`. None of that is written up in this document — see the hole
named at the top. What survives from this list is `.octex` (item 5), engine-provided primitives
(item 2), moving the OpenConstructor pieces behind opt-in modules (item 6) and the editor's Add menu
(item 7).

**Immediate, in the order the evidence argues for:**
1. **Open the actor editor.** It is the only substantial surface in the tree that has never been run,
   and every hour spent adding to it before somebody looks at it compounds. §4r.
2. **Run `./scripts/gates.ps1`.** Thirty-nine commits of renderer-adjacent change are unswept, and the
   layout has moved, so expect to re-record with a stated reason rather than to pass clean. §4d item 27.
3. **Investigate items 22 and 28 together** — the intermittent 45x45 startup and the renderer's
   run-to-run non-determinism are both "the oracle sometimes reads a plausible wrong number", and a
   retry that hides both is not a fix.
4. Then: a font for the game UI (item 30), a Media Foundation fixture (item 25), and `docs/ABI.md`'s
   missing eighth seam (item 29).
