# Aver Engine — Status & Handoff

Living record of where the engine stands and what's next. Written 2026-07-19.
**HEAD: `541d888`** · 12 commits · 128 tracked files · working tree clean.

Read this first after a context compaction, then `docs/ARCHITECTURE.md` (module DAG),
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
- **4x MSAA**: scene → multisampled color+depth (`kSampleCount=4`), `ResolveSubresource`
  to backbuffer each frame; ImGui composits on the resolved (1x) backbuffer.
- **PBR**: Cook-Torrance GGX, per-object metallic/roughness (root constants, 24 DWORDs),
  sky-hemisphere ambient + sky env reflection, sRGB→linear in, ACES tonemap + gamma out.
  `gMaterial.z>0.5` = unlit path.
- **Procedural sky + atmosphere**: fullscreen shader (`VSky/PSky`), ray from `gInvViewProj`,
  zenith/horizon gradient + sun disk/glow; distance fog in the mesh PS.
- **Lines**: unlit `createLineMesh`/`drawLines` (grid, gizmo), line PSO.
- **Wireframe**: second FillMode=WIREFRAME mesh PSO via `setWireframe`.
- Shaders: HLSL compiled at runtime with **D3DCompile → SM5.1 DXBC** (DXC/SM6 later).
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
- **Icon toolbar** tucked top-centre just under the menu bar: vector-drawn Unreal-style
  icons (Select ▸ Move ▸ Rotate ▸ Scale), each transform tool with a **snap dropdown caret**
  (grid/angle/scale increments, toggleable). Keys 1-4. Grid/Wireframe right-aligned.
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
- **Modal-loop rendering**: a window drag/resize/maximise runs the OS's own message pump
  inside `DefWindowProc`, which starves the engine frame loop and freezes the viewport. Fixed
  by `Engine::frameStep()` (the extracted per-frame body) being driven from a `WM_TIMER` set
  on `WM_ENTERSIZEMOVE`..`WM_EXITSIZEMOVE` and from `WM_SIZE`, via `Window::setRenderTick`.
  `frameStep()` is re-entrancy guarded (`inFrame_`). Don't move the frame body back inline or
  the freeze returns; don't call `pumpEvents` from the tick (the modal loop already pumps).
- Gizmos render via a no-depth overlay line PSO (`setLineDepth(false)`); toggle depth back on
  after so the grid still occludes correctly. Line meshes are prebuilt in `onInit` (never
  per-frame — `createLineMesh` never frees).
- Gizmo manipulation is drag-anywhere-on-handle (screen-projection), not full 3D handle
  raycast. Rotate increments a world-axis Euler component (exact only when other Euler
  components are 0) — acceptable for now; revisit with quaternion objects.
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
