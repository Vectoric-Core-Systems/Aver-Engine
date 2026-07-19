# Aver Engine — In-Window Editor: Implementation-Ready Spec

Dear ImGui (docking) composited over the existing DX12 viewport. The editor is a new `Application` subclass on the current loop; the runtime/RHI stay ImGui-free except for four bridge virtuals.

Conventions everywhere: **cm units, +Z up, left-handed, row-major matrices, row-vector `v*M`**; matrices passed as `&Mat4::m[0][0]`, colors as `f32[4]`.

---

## Phase 0 — Prerequisite plumbing (do first; small, real gaps)

**0.1 Platform input + raw hook** (`Event.hpp`, `Win32Window.cpp`)
- Add `MouseWheel` to `EventType`; add `f32 wheel = 0;` and reuse `button`/`pressed` fields for `MouseButton`.
- Emit in `wndProc`: `WM_*BUTTONDOWN/UP` → `MouseButton` (button 0/1/2, pressed), `WM_MOUSEWHEEL` → `MouseWheel` (`wheel = GET_WHEEL_DELTA_WPARAM/120.f`), and forward `WM_CHAR`.
- Add a raw-message hook so the editor installs `ImGui_ImplWin32_WndProcHandler` without platform ever including ImGui:
  `using RawMsgHook = bool(*)(void* user, void* hwnd, unsigned msg, unsigned long long w, long long l);`
  Call it first in `wndProc` after fetching `self`; if it returns true, skip default handling for that message.

**0.2 RHI ImGui/feature bridge** (`RHI.hpp` `IDevice`, default no-ops)
```cpp
virtual bool imguiInit(void* hwnd){ (void)hwnd; return false; } // SRV heap + ImGui_ImplDX12_Init
virtual void imguiNewFrame(){}                                  // ImGui_ImplDX12_NewFrame
virtual void imguiRender(){}                                    // bind SRV heap + RenderDrawData(open cmd list)
virtual void imguiShutdown(){}
enum class FillMode { Solid, Wireframe };
virtual void setFillMode(FillMode){}                            // second PSO, D3D12_FILL_MODE_WIREFRAME
virtual MeshHandle createLineMesh(const MeshVertex*,u32,const u32*,u32){ return 0; }
virtual void drawLines(MeshHandle,const f32 world[16],const f32 color[4]){}
virtual u32  pickId(u32 x,u32 y){ (void)x;(void)y; return 0; } // optional GPU id-pick
```

**0.3 D3D12 impl** (`D3D12Device.cpp`)
- `imguiInit`: create a SHADER_VISIBLE `CBV_SRV_UAV` heap (64 descriptors for future textures); `ImGui_ImplDX12_Init(device, kFrameCount, kBackbufferFormat, srvHeap, cpuHandle, gpuHandle)`.
- `imguiRender` (called from `onRender`, after 3D draws): `cmdList->SetDescriptorHeaps(1,&srvHeap); ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);`. Safe because the mesh PSO uses only root CBV b0 + root constants b1 (no descriptor tables), so binding ImGui's heap doesn't disturb already-recorded mesh draws, and ImGui rebinds its own root sig/PSO.
- `setFillMode`: pre-create a wireframe clone of `pso_` (identical except FILL_MODE); bind per draw.
- `pickId` (optional): reuse the existing `captureBuf_` readback pattern.

---

## Phase 1 — Loop integration (no change to `Engine.cpp`; editor is an `Application`)

| Loop phase | Editor work |
|---|---|
| `onInit` | `ImGui::CreateContext()`; `io.ConfigFlags |= DockingEnable`; apply theme (§Theme); `ImGui_ImplWin32_Init(hwnd)`; `device->imguiInit(hwnd)`; `window->setRawMsgHook(&ImGuiWin32Hook,…)`; build scene + upload meshes + grid/cage line meshes; layout built lazily on first `onUpdate`. |
| `onUpdate` (pre-beginFrame) | `device->imguiNewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();` → build dockspace/menu/toolbar/panels → process viewport input + tool logic → push frame state: `setClearColor / setLight / setCamera` (camera from orbit state, **aspect from central-node rect**) → `ImGui::Render()` (CPU only, legal pre-beginFrame). |
| `beginFrame()` | Unchanged: opens cmd list, clears full RTV+depth, binds mesh PSO. |
| `onRender` (post-beginFrame) | `setFillMode` → `drawLines(grid)` → `drawMesh` per visible object → `drawLines(cage)` for selection → **last** `device->imguiRender()` (UI composites on top). |
| `endFrame()` / `present()` | Unchanged. (Multi-viewport not recommended for v1.) |

Putting `NewFrame…Render` in `onUpdate` keeps all layout in the "state beginFrame consumes" phase; the single GPU call lands in `onRender`.

---

## Phase 2 — Dockspace

Fullscreen host window with a **pass-through central node** — the DX12 backbuffer (already cleared/drawn full-window) shows through the hole; panels dock around it.

- Host flags: `NoTitleBar|NoResize|NoMove|NoBringToFrontOnFocus|NoNavFocus|MenuBar|NoBackground`, `WindowRounding=0`, `WindowPadding={0,0}`.
- `DockSpace(id, {0,0}, ImGuiDockNodeFlags_PassthruCentralNode)`.
- First-run default layout via `DockBuilder`, then persisted by `imgui.ini`: Left 0.18 → **Outliner**; Right 0.24 → **Inspector**; Down 0.22 → **Console**; central node = Viewport (never covered).

Layout target:
```
Menu bar:  ▟ AVER | File Edit View Tools Render Help          FPS 143  6.9ms
Toolbar:   [Sel][Move][Rot][Scale] | [Solid/Wire][Grid][Orbit] | [Frame][Snap]
Body:      Outliner | VIEWPORT (passthru, gizmo+HUD overlay) | Inspector
Bottom:    Console / Status  (backend, adapter, tris, sel)
```

---

## Phase 3 — Camera + viewport interaction

Orbit camera state: `Vec3 target; f32 yaw, pitch, distance;`. Each `onUpdate`:
```cpp
Vec3 dir{cosf(pitch)*cosf(yaw), cosf(pitch)*sinf(yaw), sinf(pitch)};
Vec3 eye = target + dir*distance;
Mat4 view = Mat4::lookAtLH(eye, target, {0,0,1});   // +Z up
Mat4 proj = Mat4::perspectiveLH(radians(50.f), aspect, 1.f, distance*8.f+5000.f);
Mat4 vpm  = view*proj;                                // row-vector
device->setCamera(&vpm.m[0][0], &eye.x);
```
`aspect` and **all mouse math use the central-node rect** (`DockBuilderGetCentralNode(dock)->Pos/Size`) so 3D isn't stretched by side panels and picking maps correctly.

Input gated by ImGui (`overViewport = hovering central rect && central node focused && !io.WantCaptureMouse`):

| Input (only over viewport) | Action |
|---|---|
| Left-drag empty | Orbit: `yaw += dx*k; pitch = clamp(pitch - dy*k, -1.55, 1.55)` |
| Right/Middle-drag | Pan: move `target` along cam right/up by pixel delta × distance |
| Wheel | Zoom: `distance = clamp(distance*powf(0.9,wheel), near, far)` |
| Left-click no drag | Pick (ray/AABB) → set `selected` |
| Left-drag on gizmo axis (Move/Rot/Scale) | Manipulate selected transform |
| `F` | Frame selected (`target = aabb.center()`, `distance = fit(aabb)`) |

Letter/number shortcuts gated by `!io.WantCaptureKeyboard` so typing in a field never switches tools.

---

## Phase 4 — Menu bar + toolbar

**Menu bar** (`BeginMenuBar` inside host):
- File: New, Open `.ocmap`…, Open `.ocbeam`…, Save, Exit.
- Edit: Undo/Redo (grayed until command stack exists), Delete `Del`, Duplicate `Ctrl+D`, Deselect `Esc`.
- View: checkable Wireframe `W`, Grid `G`, panel toggles, Theme (Dark/Light).
- Tools: Select `1`, Move `2`, Rotate `3`, Scale `4` (checkmark on active).
- Render: backend readout (`backendName(device->backend())`), adapter name, clear-color `ColorEdit3`, light dir `DragFloat3`, ambient `SliderFloat`.
- Help: About (logo + `Aver Engine 0.1.0`).
- Right-aligned: FPS + frame-ms from `engine.time()`.

**Toolbar**: `SameLine`-packed buttons. Active tool = accent-filled button (`PushStyleColor(ImGuiCol_Button, kAccent)`), tooltip shows the shortcut. Plus Solid/Wire toggle, Grid checkbox, Orbit-auto checkbox, Frame, Snap toggle.

---

## Phase 5 — Tool/selection state (the core; single source of truth)

```cpp
enum class ToolMode { Select, Move, Rotate, Scale };
struct ViewState { bool wireframe=false, grid=true, orbitAuto=false, snap=false; };
struct SceneObject {
  std::string name; Transform xform; rhi::MeshHandle mesh=0;
  Vec4 baseColor{0.88f,0.36f,0.22f,1}; AABB localBounds; bool visible=true; u32 triCount=0;
};
struct EditorState {
  std::vector<SceneObject> objects; int selected=-1; ToolMode tool=ToolMode::Select; ViewState view;
  Vec3 camTarget{0,0,0}; f32 camYaw=0.6f, camPitch=0.5f, camDist=8.f;
  bool draggingCam=false, draggingGizmo=false; int gizmoAxis=-1;
};
```
`selected` (index, -1 = none) and `tool` are read by **both** the UI and the draw loop, so panels and viewport never disagree.

**Shortcuts** (only when `!io.WantCaptureKeyboard`): `1/2/3/4` tools, `Esc` deselect, `W` wireframe, `G` grid, `F` frame, `Del` delete, `Ctrl+D` duplicate, `F2` rename. Active mode shown three ways: accent toolbar button, `Tools` menu checkmark, viewport HUD badge.

**Selection input → state** (two writers of `selected`): Outliner `Selectable`; viewport click → world ray from central-node NDC, slab-test each object's transformed AABB, keep nearest:
```cpp
f32 ndcx = 2.f*(mx-vpMin.x)/vpSize.x - 1.f, ndcy = 1.f - 2.f*(my-vpMin.y)/vpSize.y;
f32 t = tanf(radians(50.f)*0.5f);
Vec3 fwd{cosf(pitch)*cosf(yaw),cosf(pitch)*sinf(yaw),sinf(pitch)};
Vec3 right = cross(Vec3{0,0,1},fwd).getSafeNormal(); Vec3 up = cross(fwd,right);
Vec3 rayDir = (fwd + right*(ndcx*t*aspect) + up*(ndcy*t)).getSafeNormal(); // origin = eye
```
GPU id-pick (`pickId`) is a clean later upgrade for overlapping/non-convex meshes.

**Tool → manipulation**: Select = pick/orbit. Move/Rotate/Scale draw a gizmo at `xform.position`; left-drag on an axis edits the matching field (Move += axis·worldDelta; Rotate `Quat::fromAxisAngle`·rotation; Scale += axis·dScale, center = uniform). Hold `Shift` = snap (0.25 cm / 15° / 0.1). v1 ships Inspector drag-floats + view-plane drag as the editing surface; drop in **ImGuizmo (MIT)** later — same row-major `view/proj/world`, decompose result into `Transform`.

**State → renderer** (`onRender`):
```cpp
device->setFillMode(ed.view.wireframe ? Wireframe : Solid);
if (ed.view.grid) device->drawLines(gridMesh_, &kIdentity.m[0][0], kGridColor);
for (int i=0;i<n;++i){ auto&o=ed.objects[i]; if(!o.visible)continue;
  Mat4 world=o.xform.toMatrix(); Vec4 col=o.baseColor;
  if(i==ed.selected) col=lerp(col, kAccent3, 0.35f);   // accent tint
  device->drawMesh(o.mesh,&world.m[0][0],&col.x); }
if(ed.selected>=0){ Mat4 cage=boxToMatrix(ed.objects[ed.selected]); device->drawLines(cageMesh_,&cage.m[0][0],kAccent); }
device->imguiRender();
```
Selection highlight = accent tint + AABB wire cage (12-edge box scaled to bounds, accent color) — fits the beam-cage heritage and works with today's `drawMesh`. Live editing: Inspector mutates `xform`/`baseColor` in place during `onUpdate`; next `onRender` reads them via `toMatrix()` — same-frame update, no sync step.

---

## Phase 6 — Per-panel widgets

**Outliner** `Begin("Outliner")`: header `+ / Dup / Del / InputTextWithHint("##filter")`; `TreeNodeEx("Scene", DefaultOpen)`; per object `Selectable(name, i==selected)` + `Checkbox("##vis",&visible)`; double-click = frame; `F2` = inline `InputText` rename; right-click `BeginPopupContextItem` (Rename/Duplicate/Delete/Focus).

**Inspector** `Begin("Inspector")`: empty state `TextDisabled("No selection")` when `selected<0`. `CollapsingHeader("Transform")`: `DragFloat3` Position (0.1), Rotation as Euler-deg (rebuild quat only when edited to avoid gimbal drift), Scale (0.01) + `Checkbox("Uniform")`, `Button("Reset")`. `CollapsingHeader("Material")`: `ColorEdit4("Base Color", Float)` (live-drives draw color). `CollapsingHeader("Object")`: name InputText, read-only `Mesh #`, `Tris`, Visible.

**Console/Status** `Begin("Console")`: left = scrolling log ring buffer fed by `AVER_INFO/WARN/ERROR`, `TextColored` per level in `BeginChild(ScrollY)`, auto-scroll when pinned, Clear + Autoscroll + level `Combo`; right strip = backend/adapter, tris, draw calls, `sel: <name>`, GPU self-test result.

**Viewport overlay** (no window; `GetForegroundDrawList()` clipped to central rect): top-left mode badge (`"MOVE"` in accent), bottom-right axis triad + `"cm | +Z up | LH"`, and the gizmo (`ImGuizmo::SetDrawlist()/SetRect(vpMin,vpSize)/Manipulate`).

---

## Theme & branding

Dark base `#1e1e1e`, neutral chrome, **one accent = Aver Amber `ImVec4(1.0,0.42,0.17,1)`** (same family as the mark's lit face and the sandbox object color; reads on dark and light). `StyleColorsDark()` + rounding 3–4, `FramePadding{8,4}`, `ItemSpacing{8,6}`; accent on CheckMark, SliderGrab, ButtonActive, TabActive, DockingPreview (0.5α). Ship a Light variant on `View → Theme`. Logo at far-left of the menu bar (the Isocube AE rasterized into the ImGui font atlas / an SRV, drawn via `ImGui::Image`) and again in `Help → About`. Accent is the only saturated color anywhere in the chrome.

---

## Build wiring

New target `Aver.Editor` (`add_subdirectory(editor)` behind `option(AVER_BUILD_EDITOR ON)`), compiling `third_party/imgui/{imgui,imgui_draw,imgui_tables,imgui_widgets}.cpp` + `backends/imgui_impl_win32.cpp` + `imgui_impl_dx12.cpp` + editor sources; link `Aver.Runtime Aver.RHI Aver.Platform Aver.Core d3d12 dxgi`. Optionally vendor ImGuizmo (MIT). Editor provides `createApplication` returning `EditorApp`; Sandbox stays as the headless render smoke-test.

## One-line data flow
`WndProc → (ImGui hook | Event)` → **onUpdate**: ImGui builds panels, mutates `EditorState` (tool/selected/camera/per-object Transform+baseColor), pushes `setCamera/setLight/setClearColor`, `ImGui::Render()` → **beginFrame** → **onRender**: `setFillMode` + grid + `drawMesh(toMatrix, baseColor[+tint])` + cage + `imguiRender()` → **endFrame/present**.