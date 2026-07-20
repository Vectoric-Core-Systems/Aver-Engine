# Aver Engine — Status & Handoff

Living record of where the engine stands and what's next. Updated 2026-07-20.
**HEAD: `9182cff`+** · 27 commits · 139 tracked files.

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
  rhi/       Aver.RHI        IDevice/ISwapchain interface + Null backend + uiWndProc registry
  rhi.d3d12/ Aver.RHI.D3D12  THE renderer (device, swapchain, MSAA, PBR, sky, lines, ImGui host, capture)
  rhi.d3d11/ rhi.vulkan/     stubs
  runtime/   Aver.Runtime    Engine loop, Application, EntryPoint (splash + ImGui hooks)
  (skeleton, not yet wired: render, render.gi, scene, physics, softbody, aero, gpudeform,
   fracture, vehicle, net, netvehicle, match, audio, world, abi — each has a README)
sandbox/     Sandbox.exe      the editor app (SandboxApp.cpp) + Sandbox.rc (icon)
tests/formats/ FormatTest.exe golden test for the loaders
third_party/ imgui/ (docking, MIT) stb/ (stb_image + stb_image_write, PD)
branding/    logo.svg, ae-mark-*.svg, wordmark.svg, logo.png, icon.ico, splash.png
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
- Shaders: compiled at runtime by **DXC → DXIL, SM 6.0** baseline; SM 6.5 variants for RayQuery.
  Falls back to FXC/SM 5.1 if `dxcompiler.dll` is absent. `dxcompiler.dll`+`dxil.dll` are
  copied into `bin/` by CMake and MUST ship with the product (see `docs/MINIMUM_SPECS.md` §5b).
- **Directional shadow map** (2048², 3x3 PCF) used by the lit pass AND by GI light injection.
- **Voxel cone traced GI** and **DXR 1.1 RayQuery shadows** — see §4b.
- RHI API: `createDevice/createSwapchain/beginFrame/endFrame`, `setClearColor/setCamera
  (viewProj,invViewProj,camPos)/setLight/setSky`, `createMesh/drawMesh(mesh,world,color,
  metallic,roughness)/createLineMesh/drawLines/setWireframe/setLineDepth`, `uiInit/uiNewFrame/uiShutdown/
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
- Dev/testing arg: `--tool <select|move|rotate|scale>` opens straight into a tool (used for
  screenshot verification since headless capture can't inject mouse input).

## 4b. Voxi — first optional module (`modules/render.voxi`)

`Aver.Render.Voxi`, gated by `-DAVER_MODULE_VOXI` (ON by default; OFF still builds/runs).
Owns the project-wide render quality settings and reports, per feature, whether it is
`Ready` / `NotImplemented` / `Unsupported` from the real device caps:

- **MSAA — implemented**: Off/2x/4x/8x applied at runtime via `IDevice::setSampleCount`
  (rebuilds scene targets + all PSOs; 1x uses `CopyResource` since resolve is illegal there).
- **Global Illumination — implemented (voxel cone tracing).** Voxelise+inject into a 3D
  radiance volume (GS dominant-axis projection, conservative raster, UAV-only pass) -> compute
  mip filter -> 6-cone diffuse gather + AO in the lit pass. Volume is one frame old (draws are
  replayed next frame). Debug raymarch via the viewport Lit dropdown. ~59 FPS at 128^3.
- **Ray Tracing / Path Tracing — declared, not implemented.** The editor greys them out and the
  setters refuse them. Only `Renderer::status()` changes when they land.
- Exposed in the editor under **Edit > Project Settings > Rendering** (project-wide, so NOT in
  the per-actor Details panel).
- Depends on **Aver.Core only** — the host pushes `DeviceInfo` in, so no RHI leaks into the DLL.
- Built **SHARED** for C# P/Invoke; C ABI in `include/aver/voxi/voxi_abi.h` (`aver_voxi_*`),
  bound by `scripting/csharp/Aver.Scripting`. Verify with
  `dotnet run --project scripting/csharp/Aver.Scripting.Sample`.
  **Caveat**: a separate C# process gets its own copy of the DLL (own settings, empty caps);
  scripting the live editor needs in-process CLR hosting, which does not exist yet.
- Dev flags: `--msaa N` (exercise the runtime switch), `--project-settings` (open the window).
- This machine reports: MSAA to 8x, **DXR tier 1.1**, typed UAV loads, conservative raster —
  i.e. everything the voxel GI will need.

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

### Verification tooling (reuse this — it works)
Headless capture reads the **backbuffer**, so it does NOT prove the window is visible or
responsive. For anything interaction- or hang-related, drive the real window with synthetic
input and watch a per-frame heartbeat instead.

Dev flags on `Sandbox.exe`: `--frames N`, `--screenshot out.png`, `--tool <select|move|rotate|scale>`,
`--project-settings`, `--msaa N`, `--gi`, `--gi-debug`, `--rt`.

Debug views that paid for themselves: **Voxel Radiance** (viewport `Lit` dropdown or `--gi-debug`)
separates "voxelisation broken" from "cone tracing broken"; the centre-pixel readout in the log is
a cheap A/B oracle (e.g. GI on/off showed red 0.70→0.73 with G/B fixed = orange bounce).

## 4c-2. Voxi/HAL decoupling refactor — IN PROGRESS, paused at step 6 of 12

Moving Voxi's GI/shadow/RayQuery code out of the D3D12 backend into `modules/render.voxi`,
against a new generic backend-agnostic RHI. Twelve-step plan; **steps 0-6 are committed and
green**, steps 7-12 remain.

| # | Step | Commit |
|---|------|--------|
| 0 | Golden baseline + `--probe X Y` | `14284b4` |
| 1 | Generic interface (`RHIResources.hpp`) | `f846b3d` |
| 2 | D3D12 factory + command context | `6c46f2d` |
| 3 | Feature hooks wired, list empty | `53c6c37` |
| 4 | Per-frame constants split b0/b4 | `57e1e34` |
| 5 | Shared shader prelude + `shadeSurface()` | `20439ac` |
| 6 | `VoxiRenderer` registered but inert | `937cece` |

**Oracle** (`sandbox --frames 40`): lit `0.34,0.36,0.42` · GI `0.38,0.35,0.40` ·
`--gi-debug` `0.19,0.15,0.17` · cast-shadow `--probe 1413 1042` `0.25,0.31,0.40`
(`0.26,0.31,0.38` under `--gi`). **The centre probe is BLIND to the sun term** — it lands on the
cube's unlit left face where ndl~=0, so every shadow/ray-tracing check must use `--probe`.
GI-enabled combinations have a pre-existing intermittent red-channel blip (~1 run in 5-10 reads
0.36/0.37); verify them by majority over 6-8 runs, not a single exact match.

### Step 7-8 was attempted and REVERTED — read before retrying
Patch of the attempt: `scratchpad/step78-attempt.patch` (299 lines, does not apply cleanly as-is).

What it got right, and would be needed again:
- Steps 7, 8 and **9 must land together**. Step 7 alone requires deleting the backend's `giHeap_`,
  but the backend's `voxelizePass` still needs it until 8 moves the volume — AND once the backend
  binds Voxi's binding set, its RayQuery reads Voxi's t2, which is null until 9 moves the TLAS.
  Confirmed empirically: `--rt` cast-shadow went `0.25,0.31,0.40` -> `0.43,0.46,0.52`, i.e. fully
  lit, because the null TLAS reports no occlusion.
- Do NOT route GI settings through `IDevice::setGi` to the feature — that puts feature vocabulary
  back into the generic interface. The app owns the `VoxiRenderer` instance; configure it directly.
- `D3D12ResourceFactory` needs `friend class D3D12Device` so the frame path can bind a feature's
  descriptor table for the scene draws the backend itself records (Tier 1 requires every declared
  table bound on every pass).
- The backend must copy the feature's `sceneConstants()` into its b4 upload buffer AFTER `prePass`,
  because the light matrix is not known until the shadow pass has run.

**Unresolved failure, this is where to start:** with the feature's `prePass` doing the shadow and
voxelise work, `--gi` and `--gi-debug` completed 40-60 frames and exited 0 with NO validation error
and NO probe line at all — the backbuffer capture never became ready. `--ms --gi` DID produce a
value, but a wrong one (`0.31,0.34,0.40` vs `0.38,0.35,0.40`). The `--ms` path uses
`dispatchMeshFor` and the non-`--ms` path uses `ctx.drawMesh`, so the divergence is somewhere in
the IA voxelisation path or in a barrier the debug layer did not flag. Diagnose that FIRST, with
the D3D12 debug layer and `AVER_RHI_TRACK_STATE` on, before writing more of the migration.

## 4d. NOT DONE — open work, roughly in value order

**Closed since this list was written**
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

**Renderer**
1. **RT ambient occlusion / reflections.** The TLAS already exists, so this is mostly shader work.
2. **Path tracing.** Declared only; would reuse the same acceleration structure.
3. **No temporal accumulation** on GI. With the volume now cleared each frame this is the main
   remaining source of GI instability; there is also an occasional one-frame blip in the shaded
   result (~1 run in 10 reads `0.36` instead of `0.38`) present on BOTH the IA and MS paths, so it
   predates the mesh-shader work — most likely the one-frame-delayed volume replay.
4. GI is a **single volume**, not cascaded — large scenes will not fit at useful resolution.
5. Shadow map is **one cascade** at 2048²; no CSM, so large scenes get coarse shadows.
6. Specular GI is not cone traced (diffuse + AO only).

**Portability (asked for explicitly: all AMD + NVIDIA DX12 GPUs)**
7. **Only ever run on one GPU (RX 7800 XT).** The DXR/GI/mesh paths are capability-gated and fall
    back, but have NOT been exercised on NVIDIA or Intel, nor on Resource-Binding-Tier-1 hardware.
    Test before shipping.
8. D3D11 and Vulkan backends are still **stubs** — D3D12 is the only working backend, so
    "supports DirectX 12" is a hard requirement.

**Scripting**
9. **C# cannot drive the live editor.** A standalone C# process P/Invokes its own copy of
    `Aver.Render.Voxi.dll`, so it gets its own settings and empty device caps. Needs in-process
    CLR hosting (hostfxr/CoreCLR). The C ABI is already shaped for it.
10. Same caveat blocks the **launcher hardware probe** in `docs/MINIMUM_SPECS.md` §7.

**Editor / engine**
11. Dock layout does **not persist** (`io.IniFilename` is null) — rebuilt from DockBuilder each run.
12. Output Log does not capture the real log; Content Browser is a placeholder.
13. Toolbar Save / Play / Pause / Stop are **non-functional stubs**.
14. No scene save/load, no `.ocmesh`, no asset import — the general-purpose roadmap in §9 is
    otherwise untouched.
15. `modules/abi` is still an empty skeleton (the C ABI lives in the Voxi module instead).

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
- `FormatTest.exe` golden-tests both against real files (0 failures).
- Authoritative sources (NOT in repo): `C:\Users\User\Documents\Unreal Projects\OpenConstructor27`
  and `C:\Users\User\Documents\OpenConstructorSupportAssets`. Recon extracts in `docs/recon/`.

## 6. Branding

Isocube AE mark (A on left face, E on right face, both following the isometric slant;
geometric-sans "Aver Engine" wordmark). Exported via headless Chrome + Pillow.
`branding/icon.ico` = window/exe icon (Sandbox.rc); `branding/splash.png` = startup splash
(no tagline). "royalty-free" phrasing removed project-wide → "permissively-licensed".

## 7. Commit history

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
