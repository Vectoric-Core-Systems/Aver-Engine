# Aver Engine — In-Window Editor: Implementation-Ready Spec

> **READ THIS FIRST: most of this document is a PLAN, and the editor that got built diverged from
> it.** It was written before the editor existed and has never been reconciled. Later sections
> describing what shipped are marked; everything else is design intent, not a description.
>
> Where it differs from the tree, verified:
>
> | This spec says | What exists |
> |---|---|
> | Panels `Outliner`, `Inspector`, `Console` | `World Outliner` and `Details` only. `Begin("Inspector")` and `Begin("Console")` appear nowhere; the Content Browser and Output Log are drawers, not docked panels. |
> | An **orbit** camera (`target`, `yaw`, `pitch`, `distance`) | **Free-fly**, and the code says so: *"Free-fly editor camera (Unreal-style): position + yaw/pitch, no auto-orbit"* (`sandbox/src/SandboxApp.cpp`). Only the actor preview orbits. |
> | `EditorState`, `SceneObject`, `ViewState`, `ToolMode` as the state | None of those types exist in `sandbox/src/`. |
> | ImGuizmo (MIT) dropped in later | Never vendored. All three handle sets are hand-built line geometry drawn through `drawLines`. |
>
> The value left in the plan is its *reasoning*. Do not treat any struct or panel name in it as an
> API.

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

**0.3 D3D12 impl** (moved since this was written — see the correction below)
- `imguiInit`: create a SHADER_VISIBLE `CBV_SRV_UAV` heap; `ImGui_ImplDX12_Init(device, kFrameCount, kBackbufferFormat, srvHeap, cpuHandle, gpuHandle)`. It holds **512** descriptors (`UiSrvPool::kCount` in `modules/rhi.d3d12.imgui/src/ImGuiUiBackend.cpp`) — the whole ImGui/DX12 binding now lives in a separate module, `Aver.RHI.D3D12.ImGui`, not inline in `D3D12Device.cpp` as this phase originally described; there is now a Vulkan counterpart too, `Aver.RHI.Vulkan.ImGui`, unmentioned below because it postdates this document. Every texture the UI samples comes out of that one pool: the editor's own icons, and any offscreen target a panel draws. That budget is why the actor preview (§Phase 8) is shared rather than one per tab.
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
- Default layout via `DockBuilder`, rebuilt **every launch** — Left 0.18 → **Outliner**; Right 0.24 → **Inspector**; Down 0.22 → **Console**; central node = Viewport (never covered).
- **The layout persists**, to `%LOCALAPPDATA%\AverEngine\editor-layout.ini` — window sizes, dock
  arrangement, table column widths and collapsing-header state. Both UI backends point
  `io.IniFilename` at the SAME file, so a layout is not lost by launching with `--backend vulkan`.
  This section used to say the opposite and argue for it ("Turning ImGui's own persistence on is a
  separate decision, not an oversight"). It was a decision, and it was the wrong one: rebuilding the
  default every launch is paid by everyone on every start, and an editor that forgets where you put
  the Outliner is one nobody trusts with anything larger.
  - The one-shot `DockBuilder` pass (guarded by `dockBuilt_`) now runs only when there is **no**
    restored layout — a dockspace node with children is one that came from the ini. Rebuilding over
    it would mean the file is written faithfully on exit and ignored on load, which is worse than
    not saving at all, because it would look like it worked. A `[Editor] dock layout restored from
    editor-layout.ini` line makes which path ran observable.
  - **View ▸ Reset Layout** sets `dockResetRequested_` as well as clearing `dockBuilt_`, so it beats
    the restore and rebuilds the default. That escape hatch is what makes persisting safe: a layout
    that ends up unusable is one menu item from being fixed.
  - Per-widget state that ImGui does not model still goes through `EditorPrefs` into `editor.ini` —
    `actorEditor.leftColumn` is the pattern. The two files are separate on purpose: one is ImGui's
    own format and disposable, the other is ours.

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

**Tool → manipulation**: Select = pick/orbit. Move/Rotate/Scale draw a gizmo at `xform.position`; left-drag on an axis edits the matching field (Move += axis·worldDelta; Rotate `Quat::fromAxisAngle`·rotation; Scale += axis·dScale, center = uniform). Hold `Shift` = snap (0.25 cm / 15° / 0.1). **ImGuizmo was never vendored**: all three handle sets are hand-built line geometry in `SandboxApp.cpp` (`buildMoveAxis`, `buildRotRing`, `buildScaleAxis` — unit-size in local space, scaled by the world matrix) drawn through `drawLines`, so the gizmo is in the 3D pass rather than in an ImGui draw list. The actor editor's gizmo is the opposite choice for a reason given in §Phase 8.

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

**Viewport overlay** (no window; `GetForegroundDrawList()` clipped to central rect): top-left mode badge (`"MOVE"` in accent), bottom-right axis triad + `"cm | +Z up | LH"`. The gizmo is **not** part of this overlay — see the correction in Phase 5.

---

## Phase 7 — Asset editors: a second kind of window

`sandbox/src/AssetEditor.{hpp,cpp}`. Everything above describes ONE window, and that stopped scaling at the second asset type: every panel the editor grew had to live in the single dockspace and be about whatever the level had selected, so an asset with no representation in the level had nowhere to be edited at all. Unreal splits the Level Editor from per-asset editors for that reason, and so does this now.

An `AssetEditor` is `path()` / `title()` / `dirty()` / `draw(Engine&)` / `save(std::string* why)` — a set of panels, not a window. The **host** begins the window, which is what lets the host decide tabs versus real OS windows later without any editor caring. `AssetEditorHost::registerFactory` takes a bare function pointer and the FIRST factory that accepts a path wins; `SandboxApp::onInit` registers the `.ocmesh` editor, then the actor editor.

| What the host does | Why |
|---|---|
| Window id is `title() + "###assetEditor:" + path()` | the title carries the dirty marker and changes on the first edit; an ImGui window whose id changes loses its size, position and docking |
| Opening a path that is already open focuses it | two editors on one file each hold a dirty flag, and the last save silently wins |
| `ImGuiWindowFlags_UnsavedDocument`, 720×520 on first use | |
| Drawn after the docked panels, before the status bar | an asset editor floats over the level editor rather than being clipped by a dockspace it does not belong to |
| Closes deferred until after the draw loop | erasing mid-iteration frees an editor whose ImGui window is still on this frame's draw list |

**One of the two things the header promises, the code still does not do — the other now landed.** `AssetEditor::dirty()`'s comment says the host asks before closing and asks again before the application exits. Closing a single dirty editor tab is still silent: it logs `AVER_WARN("[Editor] closed '{}' with unsaved changes", …)` and drops the edits, no prompt. But the exit half now exists — `SandboxApp::requestExitChecked` calls `assetEditors_.anyDirty()` and, if it is true, raises an "Unsaved changes" modal (`drawExitPrompt`) naming every dirty editor rather than exiting straight through; a comment at the call site says plainly that this used to be the exact bug this section originally described, with `anyDirty()` having no caller at all.

**Superseded — a `.cs` now routes here.** `cbOpenEntry` tests `cbIsSourceFile` first and `.cs` is in that list, so a double-click in the Content Browser hands a designer file to the IDE and `assetEditors_.open` is never reached for one. The actor editor below is registered and drawn, and its two halves have tests (`ActorScriptTest` over the parse and rewrite, `ActorPreviewTest` over the feature against a recording device rather than a GPU), but the tab itself has no test and today **no route from the UI**; a `.ocmesh` is the only thing a double-click opens in a tab.

---

## Phase 8 — The actor editor tab

`sandbox/src/ActorEditor.cpp`, over `modules/formats/…/ActorScript.hpp` and `modules/render.actorpreview/`. The design reasoning is docs/ACTOR_EDITOR.md; this is what a user meets.

**What opens.** A `.cs` that either carries a generated region (`// <aver-generated region="models" schema="1">`) or whose class declares a mesh, a camera or a point light in `Configure(ClassBuilder)`. Both, because the multi-part actor is the *rarer* shape — most actors in most games are one mesh declared on the class with no designer file at all, and an editor that only understood placements would show an empty view for all of them. Anything else is declined and falls through, since a plain `.cs` is not an actor and opening it in a tab that can show nothing would be taking something away. A region that is present but has left the locked grammar is declined **loudly**, with the reason logged, rather than opened as an empty tab that cannot save.

**What you see.** Left, a square 3D preview. Right, in a 320 px column: an actor picker when the file declares more than one class (SkyForge's `FpsGameMode.cs` declares five), the class name, a `camera: … (not drawn)` or `point light: … (not drawn)` line for what the class declares and the preview does not draw, the placement count, the placement list, and — for the selected placement — its mesh path, its material, its `ObjectId` in hex, and `DragFloat3` for position (cm), rotation (deg) and scale. The id is on screen because it is the key a save matches on, so it is the first thing anyone wants when a save writes the wrong row. Meshes the source names and the project does not have are listed by name in amber: an actor whose meshes are all missing renders an empty view, and empty is indistinguishable from broken.

**The preview deliberately does not match the level viewport**, and the panel says so in as many words — *"Preview lighting is fixed and does not match the level viewport."* **Fixed exposure, no bloom, no eye adaptation, no GI, no cascaded shadows, no MSAA.** Two of those are structural rather than unfinished:

- `IDevice::setCamera` writes a CPU-side struct uploaded once at the top of `beginFrame`, so a second camera set after that is a no-op *for the current frame*. Two rects in the scene pass would both draw with the previous frame's camera and present as a matrix bug.
- Exposure is one histogram over the whole target reducing to one scalar, so two viewports cannot hold different exposures. Sharing the scene pass would make the LEVEL viewport ramp its brightness for a second every time this tab opened.

So it is a render feature that owns its colour and depth targets and runs in `prePass`, before the scene binds the backbuffer. **Its camera is published at `b4`, not `b0`**: the backend rebinds slot 0 to the engine's per-frame block on every `setPipeline`, and `averSkyAbove` reads the sky out of b0 — which is why the preview writes its own backdrop instead of sampling the level's atmosphere. One key light plus a hemisphere fill leaning on world +Z, so an unlit side still reads as a silhouette; ACES and gamma in the pixel shader; its own depth buffer cleared to 1 with a Less test (**not** reversed-Z, stated so nobody later "fixes" it to match the scene). Selection is a rim light, added rather than replacing the colour, so the highlight does not hide the material the author is looking at.

**One preview, shared, driven by whichever tab is active.** Not one per tab, and the reason is a hard limit rather than thrift: the UI SRV pool is 512 descriptors (§Phase 0.3) and the editor already spends several on its own icons, so a target per tab exhausts it at about 500 and the failure is a black image, not an assert. A second tab still shows its model list and its numbers; it just does not get the 3D until it is focused.

It is created on **first draw** — `AssetEditorFactory` is a bare function pointer with no device to hand it — registered as a render feature there, and removed before it is deleted, because the device holds features non-owningly and a deleted one leaves it calling `prePass` on freed memory, which nothing reports. The content root arrives the same way, pushed by `applyProject` through `setActorEditorContentRoot`. Meshes resolve on demand through the preview's **own** registry (`PreviewMeshCache`), not the level editor's table: an asset editor that reached into level state could not exist without a level open, could not be tested without one, and would quietly make "this tab is independent of the level" false. A miss is cached as handle 0, or an actor naming an absent mesh re-reads the disk once per model per frame and the editor's frame time becomes a function of how wrong the file is. Every lookup goes through `fmt::canonicalMeshPath`, so `Content/Meshes/X.ocmesh` and `Meshes/X.ocmesh` collapse to one entry and one upload.

**The gizmo translates, and that is all.** Three axes, drawn as an ImGui overlay on top of the image rather than as geometry in the pass — the opposite of the level viewport's choice, for two reasons: the preview feature has to stay drivable with no ImGui at all (that is what lets a test be the device), and a handle has to be pickable at a constant SCREEN size, which it cannot be as part of a scene that scales. It projects through the preview's own `viewProj()`, because deriving that matrix independently is how a handle ends up a few pixels off its object and then a few more as the camera turns. Shaft 64 px, grab radius 10 px, distance measured to the *segment* so the near end of an axis is not dead. The axis is latched on mouse-down and held for the whole gesture: deciding per frame would turn a drag that began on the handle into an orbit the moment the cursor left it, which is exactly when the user is dragging fastest. A drag anywhere else orbits; the wheel zooms multiplicatively, so a notch moves the same proportion whether the subject is a bolt or a vehicle; pitch is clamped to ±85° short of the pole.

**There are no rotate or scale handles.** Rotation and scale are the drag-floats and nothing else. There is no snapping, no numeric readout during a drag, and no undo of any kind in this tab — the level editor's undo stack does not reach it.

**Reload on mtime, refused while dirty.** Every frame the tab draws, it stats the file. A watcher would be the general answer; a stat is the honest small one, since a visible tab is polled anyway and a thread plus an OS handle plus an overflow case is more to get wrong. Changed → re-read, re-parse, re-pick the previewable class, drop the selection if it no longer indexes a row, status `Reloaded from disk.` A parse that returns `Malformed` or `UnknownSchema` keeps the previous good parse and shows the reason, so a file caught mid-write by an IDE does not blank the tab.

If the tab is dirty the reload is **refused**: *"The file changed on disk. Save or discard to pick it up."* Silently replacing somebody's in-progress drag with what a background tool wrote is the one behaviour a live-sync feature must never have. The stamp is taken *before* the refusal, so the message appears once rather than every frame.

**What a save does.** `Save to C#` (disabled unless dirty) calls `fmt::rewriteActorScript`, which matches placements **by ObjectId** — never by position in the file, so a reordered region, an inserted row or a renamed property still finds the right line — and rewrites the three coordinate tuples of each. Every other byte is left alone: the marker lines, the usings, the namespace, the class header, every field of a placement other than pos/rot/scale, and the hand-written half of the actor, which this never even reads. Models the file does not contain are ignored rather than appended, because adding a placement has to mint an id and declare a property, and doing that silently from a coordinate save would be surprising. It writes the `.cs`, and only the `.cs`. On success the in-memory source becomes what is now on disk, so a second save rewrites from the file as it stands rather than from the text the tab was opened with.

A file with no region — the single-mesh actor declared on the class — is a **viewer**. It has no placements to edit, so nothing can mark it dirty and the save button never enables. That is the honest state rather than a save with nowhere to write.

**Rough edges worth knowing.** The save does not re-stamp the write time, so the next frame's reload fires on the tab's own write; it is harmless, the text being identical, but it replaces `Saved.` with `Reloaded from disk.` immediately. And the selection is an *index* into the parsed models rather than an ObjectId, so a reload that reorders the region moves the selection to a different placement.

**The Roslyn backend exists.** `averdesign` is a console tool at
`scripting/csharp/Aver.Design/`, staged to `bin/Tools/`, built on `Microsoft.CodeAnalysis.CSharp`
(MIT, pinned at 5.6.0 and **checked in** at `third_party/nuget`, so it restores offline). It is the
repo's only NuGet consumer. This used to say the package was "already in the SDK's package cache" —
the SDK ships Roslyn as compiler DLLs, never as a package, so that was wrong; see
[ACTOR_EDITOR.md §4d](ACTOR_EDITOR.md).

It runs **only** when the builtin scanner returns `Malformed`, which is exactly the signal that the
text has left the locked grammar of `docs/DESIGNER_REWRITE.md` rather than that the text is wrong.
So a named argument moved, an argument omitted, or a coordinate written as an expression now opens
instead of being declined. A file the scanner reads is never re-read by the slower parser.

The C++ side is `Aver.Formats.Roslyn`, deliberately a module ABOVE `Aver.Formats`: the base module
reads bytes and depends on nothing, which is what lets the scanner run on a machine with no .NET at
all, and process spawning does not belong below that line. A build without the module has no
escalation and still opens every conforming actor.

`RoslynTest` pins the property that matters: on a file both backends read they must produce identical
ids, values and **byte spans**. A fallback that quietly disagreed would write a coordinate into the
wrong byte of somebody's source. It also covers a non-ASCII file, because Roslyn counts UTF-16
characters while the C++ side slices UTF-8 bytes, and the two agree only until the first accented
letter.

---

## Phase 9 — The material authoring loop

**C# is the source of a surface.** A `.cs` under `Content\Materials` declares `[AverMaterial("M_Name")]` and configures it; `avermatc` reflects the built assembly and writes `<project>\Binaries\Materials\<name>.ocmat`, which is what the engine reads. `materialForSurface` tries `Binaries\Materials\<name>.ocmat`, then `Content\Materials\<name>.ocmat`, then the name as a content-relative path. Binaries wins rather than merging, because the generated file is the newer of the two by construction — it is rewritten from source on every build — and a stale hand-authored file left beside the source must not shadow it.

**What you see.** `Details → Material`, for a selected object: Metallic, Roughness, Normal Scale, Occlusion, Reflectance (stopping at 0.2, which is gemstone — a 0..1 slider would spend four fifths of its travel above anything real), Grazing (f90), Emissive, a UV Mapping combo (Mesh UVs / World Aligned) with a logarithmic tile size in centimetres, and one path field per texture slot. Every control edits the `pbr::MaterialDesc` through the library and then calls `touch()`, because `mutableDesc()` hands out a raw pointer and marks nothing — an edit that forgot it would show in the panel and never reach the GPU. The consequence a user sees is that the viewport changes the same frame, and that two objects sharing a material change together, which is the point of editing the material rather than the actor.

**What a save does.** `Save to C#` writes the `.cs` and **never** the `.ocmat`: the `.ocmat` under `Binaries` is a build artefact the next compile overwrites, so writing there is a change that appears to work and then silently vanishes — the worst behaviour a save button can have. The declaring file is *found* rather than recorded, because one `.cs` may declare several materials and the `.ocmat` says nothing about which: the editor walks `Content\Materials\**\*.cs` and tries `fmt::rewriteMaterialScript` on each, and since a file that does not declare the name declines, trying each in turn is both the search and the check. A rewrite that changes nothing returns without touching the mtime. What it rewrites is the body of `Configure` and nothing else — usings, namespace, other classes, attributes, the file's own indentation of the chain, and the `.Comment(...)` calls all survive. Those comments are authored prose that no `MaterialDesc` can regenerate, and losing them on the first save would lose the reasoning behind the numbers. Only values that differ from the defaults are written, so a material that sets little produces an almost empty `Configure` rather than eighteen lines restating defaults.

Then `Compile C#` — the toolbar button; `Tools → Compile Scripts` is the same job — runs `dotnet build Scripts.csproj -o <project>\Binaries\Scripts` and, on success, `bin\Tools\avermatc.dll` over the built `Scripts.dll`. Baking happens on the build thread right after the compile that produced the assembly it reflects: a separate button would be a button somebody forgets, and a surface one build behind looks like a material bug rather than a missing step. A bake failure does **not** fail the compile — the C# built, the assembly is good, the previous `.ocmat` stays, and the log gets a `[materials]` section. Turning a bad `Configure` into a red build light would read as a compile error, which it is not.

**What is not there.** Nothing re-reads a `.ocmat` after a bake: materials load when a project opens and are released when it closes, so the round trip only shows on the next open. The sliders being live hides this until the source and the loaded material disagree. There is no *New Material* menu item, although `fmt::newMaterialScript` exists and will write the whole file. And the Material header appears only for the editor's placeholder objects — once a level has placed entities, or a play session is running, those are hidden and unpickable, and a scene entity's Details offers Transform and Mesh only.

---

## Phase 9b — The material graph editor tab

`sandbox/src/GraphEditor.cpp`, over `modules/render.pbr/src/MaterialGraphHlsl.cpp`. This is the
**same** `.ocgraph` editor Aver Node already uses for gameplay graphs (`assetEditors_.registerFactory
(&editor::makeGraphEditor)`, Phase 7) — not a second tool — reading the open file's own `DOMAIN`
record (`GraphEditor::openGraphDomain()`) to decide which of two unrelated vocabularies it is
currently editing. The full design reasoning for that vocabulary, the `DOMAIN` record itself, and
`GRAPHREF` is **`docs/MATERIALS.md`**; this section covers only what changes about the *tab* once the
file it has open says `DOMAIN material`.

**What opens.** Any `.ocgraph`, exactly as Phase 7 already describes. There is no second asset type
and no second factory — the tab only starts behaving differently once the file is parsed and its
`DOMAIN` read, which is also why the editor is never wrong about which vocabulary to offer: it asks
the same record the compiler will.

**The palette follows the domain, node for node.** The add-node popup offers only the open graph's
own vocabulary — a material graph's palette has no `Branch` or `CharacterMove`, a gameplay graph's
has none of the 56 material nodes — because offering a node the graph's own compiler would then
refuse is worse than offering nothing. Sixteen names exist in **both** vocabularies with genuinely
different pins (gameplay's `Add` takes two scalars, since `PinType` has no vector types at all; the
material `Add` takes two `float3`s), so spawning a node resolves by *(type, domain)* together, never
by name alone — dropping `Add` into a material graph by name alone would have handed it the scalar
shape, a node whose pins fit nothing around it.

**Wiring is checked against the domain's own type rules, not one global rule.** A gameplay graph
keeps exact-match pin typing, unchanged. A material graph's link check mirrors the compiler's own
widening rules (`docs/MATERIALS.md` §7) exactly: a scalar splats, a wider vector truncates, and a
`float2` into a `float3` stays refused on both sides, because inventing the third component is the
compiler guessing. **The editor refuses exactly what the compiler refuses, and no more** — accepting
a wire the compiler would then fail on is a worse trap than refusing one the compiler would happily
accept.

**The Viewport tab is a sphere, not the component tree.** A material graph has no `CLASS` record and
no components, so it gets neither the component toolbar nor the component tree — both would be
furniture for a thing this file cannot contain, and the toolbar's own warning ("no `CLASS` record,
so nothing spawns them") is actively misleading on a file that is not supposed to have one. What
fills the tab instead — the sphere, how it behaves under a compile error, under `AVER_MODULE_PBR=OFF`,
and while the graph is mid-edit — is `docs/MATERIALS.md` §8; not repeated here.

**Built without the PBR module, the tab still opens.** Editing the text and wiring nodes needs no
renderer, but the Viewport tab says plainly that there is nothing to preview rather than showing a
blank or stale image. `render.actorpreview` links `Aver.Render.PBR.Materials` **conditionally** for
exactly this reason — it is built in every configuration, and an earlier attempt at this feature
named a PBR-only target unconditionally and broke the `AVER_MODULE_PBR=OFF` build outright before an
agent building that specific configuration caught it.

---

## Phase 10 — Projects: what New, Open and Upgrade write

`sandbox/src/ProjectScaffold.{hpp,cpp}`; the layout itself is docs/PROJECTS.md's.

**New Project** creates `<location>\<name>\` and refuses an existing folder — a half-scaffolded project on top of someone's data is worse than a failed button. It writes the manifest, `Content\{Maps,Meshes,Materials,Textures,Sounds,Scripts}`, `Content\Scripts\Scripts.csproj`, and `Content\Materials\Surfaces.cs` declaring `M_Default`. The `.csproj` is written **at creation** rather than lazily on the first script, which is where it used to appear: a project without one cannot Compile C#, Compile C# is what bakes materials, so a project whose first authored thing was a material had no way to build it — and the button that would have said so was disabled for want of the file it was about to create. It carries four engine references (`Aver.Scripting`, `Aver.Framework`, `Aver.UI`, `Aver.Materials`) and a `<Compile Include="..\Materials\**\*.cs" />` item. The starter material exists for the reason the script templates do: the shape of a material is not guessable, and an empty `Materials` folder teaches nothing.

The manifest names `STARTMAP Maps/Default.ocmap` and **no map file is written**, so a fresh project opens with no level — `loadStartMap` says so in the log and the world starts empty.

**Opening an older project asks.** `inspectProject` reads and writes nothing; if it finds anything, a modal names every fix — a folder that will appear, an engine reference that will be added, a `ProjectReference` whose target has moved. A list, not a reassurance: the one thing an author has to decide is whether they mind, and "upgrade your project" gives them nothing to decide with. *Not now* leaves every file untouched and is remembered for the session, because a deliberately minimal project is a legitimate project and an editor that asks on every open is one people learn to dismiss without reading. The `.csproj` is **merged, never regenerated** — missing items are appended as a new `ItemGroup` before `</Project>`, which MSBuild merges with whatever is already there — because a project may carry a PropertyGroup, a PackageReference or a target somebody added by hand, and replacing the file to add one reference would throw all of it away. Nothing is modified without a yes.

---

## Phase 11 — Revision control, and what the status bar reports

`sandbox/src/RevisionControl.{hpp,cpp}`, over the shared `sandbox/src/ProcessRun.hpp` (also used by `ToolsMenu.cpp` for `dotnet build` and `avermatc`), threaded from `sandbox/src/SandboxShell.cpp`'s `revisionControlTick/Refresh/Select` and drawn from three places: `buildRevisionControlPanel` (Window → Revision Control), a status-bar widget (`drawRevisionControlStatusWidget`), and a per-asset badge in the Content Browser (`rcMarkFor`, called from `SandboxContentBrowser.cpp`). Tested at `tests/editor/src/RevisionControlTest.cpp`, 136 assertions — **the parse and the summary only**; see below for what that leaves untested.

**The editor structurally cannot change a repository.** Every process this feature ever starts goes through one function, `runGit` in `RevisionControl.cpp`, and `runGit` asks `isReadOnlyGitSubcommand()` — declared in the header, where a reviewer reads it before any of the I/O — before it will build a command line at all. Failing that check is not a quiet no-op: it is an `AVER_ERROR` log plus a `*why` set to `"internal error: ..."`, on the theory that reaching it at all means a call site asked this module to do something it is documented not to do, which is a programming error worth being loud about rather than a runtime condition to swallow.

| What is on the list, and what is not | Why |
|---|---|
| `status`, `log`, `diff`, `rev-parse` — the four subcommands this feature's public functions actually call | `gitStatus`, `gitLog`, `gitDiff` and `gitRepositoryRoot` (`rev-parse --show-toplevel`) are the whole read surface today |
| `show`, `ls-files`, `cat-file`, `blame`, `version` also pass the check, though nothing calls them yet except `version` (`gitAvailable`'s probe) | headroom for read-only features that have not been built. **The header's own top comment still describes the surface as "status, log, diff and rev-parse"**, which undercounts the actual whitelist — nine entries, not four |
| `add`, `commit`, `checkout`, `restore`, `reset`, `clean`, `stash`, `revert`, `rm`, `push`, `pull`, `fetch`, `config`, `branch` — refused, one by one, in the test | each can discard work, rewrite history, touch a remote or rewrite the user's own git config, and belongs behind its own entry point with its own confirmation. Widening this list instead would make every one of them reachable from every existing call site at once, which is the mistake the list exists to make hard |

`status` is on the list despite writing to disk: `--no-optional-locks` keeps it from taking the index lock, but git still refreshes its on-disk stat cache regardless, and the header calls that a cache write rather than a change to tracked content — excluding it "would leave no way to ask the question this file exists to ask." **Nothing on the list ever contacts a remote**, which is also why authentication is not a concern this code has to hold: a future remote operation would run as git's own child process with git's own credential helper, and a failure would reach the user as git's own error text, the same way `*why` already carries one verbatim today. The list is explicitly a statement of scope a compiler can be pointed at, not a security boundary — nothing stops a future call site writing its own `CreateProcessW`, and the list could not stop it if it tried.

**The split: decide in the header, spawn in the .cpp.** `RevisionControl.hpp` is a pure header, deliberately, following `InputOwnership.hpp` and `Runtime/include/aver/game/SceneSubmission.hpp` line for line — no ImGui types, no `SandboxApp` state, no globals, no `<windows.h>`, no process anywhere near it, plain text in and plain values out over `aver/core/Types.hpp` and the standard library. `RevisionControl.cpp` owns every `CreateProcessW`, every command line and every log line the feature produces. The reason is `RevisionControlTest.cpp`: it includes only the header and links `Aver.Core` alone, which is what lets `parsePorcelainV2`, `parseGitLog`, `changedCount`, `summariseForStatusBar` and `isReadOnlyGitSubcommand` be pinned with 136 assertions in a codebase where almost nothing about the editor can be tested at all. What that number does **not** cover: `buildRevisionControlPanel`, the status-bar widget, `revisionControlTick/Refresh/Select`'s threading, the Content Browser badge, and the `runGit` spawn itself — none of those has a test.

Porcelain v2 with `-z` is the wire format, chosen over the v1 short format for reasons the header spells out: v2 is documented as stable for machine reading and versioned, so a future v3 is a new number rather than a changed meaning of today's records; it carries rename information in its own field with a similarity score, where v1's `R  new -> old` text is ambiguous the moment a filename contains " -> "; and `-z` means `core.quotepath` can never mangle a path — no quoting to reverse, no CRLF to confuse a line-oriented reader. A malformed or short record is skipped rather than treated as fatal (a git that has learned a new record type is not a failure to report), and an unmerged (`u`) record is decoded as its own case rather than run through the ordinary staged/unstaged table — doing that naively would read `UU` as "unmodified, unmodified," which the test file itself calls "the most dangerous wrong answer available in a program that will later offer to discard work."

**Git never runs in a frame.** `revisionControlRefresh` and `revisionControlSelect` hand their question to a `std::thread` that captures its job by `shared_ptr` and a couple of plain values — never `this`, never a reference into `SandboxApp` — detaches, and returns immediately; `revisionControlTick`, called once a frame from `buildUI`, reaps whatever finished through an `std::atomic<bool> done` flag and latches the answer. A worker that outlives the editor writes into memory it co-owns and exits quietly; nothing waits on it. The worker is also forbidden from logging — `AVER_*` reaches `logSink`, which writes into `SandboxApp`'s own `logLines_`, exactly the reach back into the editor the capture-by-value rule exists to forbid — so every ordinary failure comes back through the job's `why` string and is reported from the frame thread instead; the one `AVER_ERROR` a worker's call graph can reach is `runGit`'s refusal above, unreachable in practice because every call site passes one of the four hardcoded, correct subcommand strings. A timer refreshes the status every four seconds, measured from the last **latch** rather than the last request start (so a slow status can't queue a second one before the first lands), and only while the panel or the Content Browser drawer is open — nobody pays for a git process to keep a closed panel current. Two answers the timer never retries on its own — git absent, or the project not a repository — because neither changes while the editor watches; the Refresh button forces a re-ask, since the user is the one who would know they just installed git or run `git init`. A project switch drops the in-flight job's `shared_ptr` rather than cancelling it (a detached worker cannot be recalled) and clears every other piece of latched state, so a stray answer that lands after the switch is caught by a directory mismatch on reap and thrown away rather than mislabelling the new project with the old one's changes.

**Two surfaces onto one latched answer.** `buildRevisionControlPanel` and the status-bar widget both read the exact same latch, but the panel already needed five different sentences for its various "nothing to show" states, and a second surface re-deriving those states by hand from `RepoStatus` would be a second surface that can disagree with the first — the failure `summariseForStatusBar` exists to make structurally impossible: the status bar asks that one function for a `RepoMood` and a colour and never reads `RepoStatus::files` itself. There are seven moods — `Unknown`, `NoProject`, `NoGit`, `NotARepo`, `Clean`, `Dirty`, `Conflicted` — and the test proves all seven reachable and pairwise distinct from one call each. The load-bearing distinction is `Unknown` versus `NotARepo`: both currently describe an equally empty `RepoStatus`, but one means "the worker thread has not answered yet, so this may still turn out to be a repository" and the other means "git answered, and there genuinely is none" — a status bar that could not tell them apart would paint a freshly opened project the identical colour and label of a project someone deliberately keeps outside git. `Conflicted` outranks `Dirty` regardless of how many files are merely modified alongside the conflict, because an unresolved merge rendered as an ordinary "12 changed" is, in the header's words, "the most dangerous wrong answer this widget can give, because the number looks ordinary." `StatusBarSummary::changed` and `RepoStatus::clean()` use the identical rule — ignored paths never count — and the test checks both on three different trees so the two functions can never quietly drift apart. A detached HEAD is carried as its own bool on `StatusBarSummary` rather than folded into the mood enum, because a detached tree can independently be clean or dirty, and squeezing that into one enum would force a choice between two facts that do not imply each other.

**The status bar's right-hand cluster is measured, not guessed.** A literal here had already been wrong twice — 250, then 340 "widened for the third button" — because a constant is a guess at what a fixed set of labels will render as, and it stops being even that the moment a DPI scale grows every face past what the guess allowed for. `buildUI` now sums what ImGui will actually spend on each button before drawing any of them: `CalcTextSize` for the face's own text, `FramePadding.x * 2` for the padding `SmallButton` puts around it, and `ItemSpacing.x` for the gap an unadorned `SameLine` leaves before the next item — the same two style fields ImGui itself reads when it lays the row out, not an estimate of them. The revision-control face is computed once for this purpose through the identical `rcStatusFace` the widget calls again to draw it, so measuring it twice a frame costs a few string operations over data that is already latched. The MCP face is drawn by a different translation unit entirely and cannot be measured from here, so it substitutes the documented ceiling `ICON_LINK " MCP :65535"` — an icon glyph, "MCP" and a five-digit port — wide enough for whatever the widget actually draws. Placement is `ImGui::SameLine(std::fmax(GetCursorPosX(), wsize.x - clusterW - 8*dpi_))`: `fmax` means a window too narrow for the whole cluster lets it start immediately after the status text and run off the right edge, rather than backing the cluster up over text already painted — overflow is recoverable by widening the window, two runs of text drawn through each other are not.

**The MCP widget carries its own honesty requirement.** It has nothing to do with git — it shares the status bar's right-hand row only because both widgets are the same shape, an icon and a word tinted by state that answers a question on hover and opens a menu on click, drawn through the identical `statusBarWidget` helper — and it sits one slot INSIDE Revision Control, which holds the end of the row where Unreal's own status bar puts the same widget. The row is laid out left to right by `SameLine`, so the last call is the rightmost control and MCP is therefore drawn first; it was briefly the other way round, under a comment claiming revision control was on the end. Idle — the default, since nobody has to opt into a control channel and most sessions never pass `--mcp` — is drawn as the ordinary state it is, with no alarm colour. The MODULE is on by default as of 2026-09-20 so that this widget exists at all; the CHANNEL is not, and the distinction is the whole reason that default could move: compiling the bridge in opens no socket, and `mcp.conf` can only name the port a later `--mcp` would use rather than turning anything on. Live, the tooltip and the menu both say the same thing without being asked twice: this is a loopback control channel, 127.0.0.1 only, and anything on the machine that can open a socket to that port can drive the editor with real input — move the mouse, type, click — and call any registered engine ABI. `mcpStart` registers the ABI dispatchers exactly once (`mcpAbisRegistered_` latches it, since `registerAbi` replaces rather than appends, and a stop/start cycle re-registering everything is exactly the habit that stops being harmless the day one registration is not idempotent), installs the widget-lookup hooks unconditionally on every call (`uiReg_.centreOf` / `uiReg_.describe`, both stateless closures over `this`, so reinstalling costs nothing), then starts listening. `mcpStop` only stops the listener — there is no unregister call on `McpBridge`'s surface, and nothing would be gained by adding one.

**The Content Browser's per-asset mark** is the same latch's third reader. `rcMarkFor(absolute, isDir, out)` turns an editor-absolute path into a repo-relative key through `rcKeyFor` — lower-cased and forward-slashed once at latch time (`rcRootKey_`), because the browser asks this once per visible card per frame and Windows paths differ in case without differing at all — then looks it up in `rcMarks_`, a sorted `(path, FileStatus)` vector built once per latched answer, not per card. A file's mark takes the worktree half over the staged half (what is actually on disk in front of the user) unless the entry is conflicted, which wins outright. A folder's mark is a rank over every entry under its prefix — conflicted (3) beats any tracked change (2) beats untracked (1) beats ignored (0), stopping early the moment a conflict is found — because the panel is where per-file detail belongs, and picking one child's specific status to paint on a folder would be a claim about a file the folder itself is not making.

**What this does not do**, stated plainly because that is this file's own register: there is no commit, no stage/`add`, no push, no pull, and no diff against a remote — `gitDiff` only ever compares the worktree to the index or the index to `HEAD`, both entirely local, and it hands back raw unified-diff text rather than parsed hunks, on the reasoning that a diff is what a person reads, and inventing a hunk model before anything needs one is guessing at a viewer that does not exist yet. Nothing that could discard work — `checkout`, `restore`, `reset`, `clean`, `stash`, `revert`, `rm` — is reachable through this module at all, by design rather than by omission; adding any of them means a new entry point with its own confirmation, not an addition to `isReadOnlyGitSubcommand`. `buildRevisionControlPanel` has no test of its own, nor does the status-bar widget, the Content Browser badge, `revisionControlTick/Refresh/Select`'s threading, or the `runGit` spawn point itself — `RevisionControlTest.cpp` deliberately does not link `RevisionControl.cpp`, so none of the process-facing half is exercised by anything that runs without a person watching a window. An untracked directory of a thousand new files still renders as the one collapsed row git's own `--untracked-files=normal` default produces, both in the panel and as a single folder badge — there is no per-file view of it without changing that flag, which nothing here does.

---

## Phase 12 — Object animation: a transform clip on a placed mesh

A placed mesh can play a transform animation in Play — a car driving a route, a boat on a river, a fan spinning — authored in a DCC such as Blender and imported through glTF as an ordinary `.ocanim` flagged `kOcAnimObject` (one track on bone 0, engine space). It is the counterpart of a Level Sequence transform track, and like one it only plays in Play: outside Play the mesh stays at its placement.

**The rule, in one sentence.** The placement is where the object is at its start time `t0`, and the clip moves it relative to that: `F(t) = B · A(t0)⁻¹ · A(t)`, with `B` the entity's own placement and `A` the clip's track. Only relative motion is used, so the frame the clip was authored in never matters — a car placed at the route pose for `t0` follows the route, and a fan placed anywhere spins about its own pivot.

**Details panel → Mesh → Animation.** *Change Clip…* lists the project's `.ocanim` files that carry `kOcAnimObject` (skeletal clips are left out), plus *(None)*. *Speed*, *Start time (s)* and *Play once* tune it. All four apply to the whole selection, one undo entry per change (a drag is one entry). The section is absent on a skeletal mesh, whose `CAnimator` is a skeletal clock, and read-only while Play is running. A level saved as the legacy `.ocmap` cannot store an animation, and the panel says so.

**In the file.** Four tokens on a `PLACE` record, written only when set: `anim <clip>` (content-relative path with the extension, forward slashes, percent-encoded like `name`), `animspeed <f>` (default 1), `animtime <f>` (`t0`, default 0) and `animonce` (hold the last pose; the default is loop). The editor keeps the authored values in a side map and saves from that, never from the entity's `CAnimator`, so a Play session never leaks its clock into a save; a save that lands mid-Play writes an animated entity's placement from Play's own snapshot.

**Play and Stop.** Play turns `AnimSystem`'s object animation live right after the level snapshot; Stop turns it off before the level's transforms are restored, then puts each animator's authored time, speed and flags back. An animated placement that collides gets a kinematic body instead of a static one and is driven by `world::driveKinematicBodies` after each animation tick, so a character standing on a moving deck is carried. Stop puts those bodies back at the restored transforms.

---

## Phase 13 — Ejected play: bringing the pawn to the camera

Eject (F8, or the toolbar button) keeps the session running and hands the camera, selection and gizmos back to the editor; F8 again possesses, and the view snaps back to the pawn. In a large level the point of ejecting is to fly somewhere else, so **Shift+F while ejected moves the possessed pawn to the editor camera** (`play.pawnToCamera`, rebindable like the rest; `SandboxApp::teleportPawnToCamera`). F8 then resumes play there. The chord does nothing in any other state, and nothing while a text field has the keyboard.

**Where the pawn lands.** The rule is the Play option *spawn at the camera*'s. A flying default pawn is the camera, so it takes the camera's position, yaw and pitch. A pawn that stands gets its feet one eye height below the camera — 160 cm for the walking default pawn, the session's published eye height for a project pawn — so a first-person view comes back where the editor camera was. A ray straight down over that eye height stops the feet 5 cm above any surface it finds, so a camera hovering close over a floor does not put the capsule under it. A pawn left in mid-air falls.

**The capsule moves with it.** A walking pawn's position belongs to its physics character, which is copied back onto the entity every tick, so moving the entity alone would be undone a frame later. The default pawn's capsule is the editor's own. A project pawn's belongs to its managed `AverCharacter`; it is found through the entity stamp that class puts on it (`aver_phys_character_of_entity`) and measured with `aver_phys_character_shape`, because a character's position is its capsule's centre. A possessed level entity with a `CRigidBody` has its body moved, stood upright and brought to rest. Every moved capsule or body is left with zero velocity.

**With no GameMode the default pawn is a drone** (Play options > *Fly (drone, collides)*), like Unreal's DefaultPawn: a 35 x 90 cm character capsule centred on the camera with gravity factor 0 (`aver_phys_character_set_gravity_factor`) and no stair stepping, so it flies freely but stops at walls and slides along them. WASD moves along the view, Q/E down/up, and the game's own input drives it too (`--play-test` holds W). It is given zero velocity on any frame nothing drives it (ejected, or the keyboard is the UI's), since with no gravity it would otherwise drift. If physics cannot create the capsule, it flies without collision as before. *Walk* is the gravity capsule described above.

**The facing.** The pawn takes the camera's yaw, and a pawn with a view node (a character's head) takes its pitch there. An `AverCharacter` keeps yaw and pitch privately and rewrites its rotation from them every tick; `Character.Drive` now adopts a facing it did not write itself (the entity's yaw, the view node's pitch), so a project character turns to the camera too, and so does one a graph or a script turns.

**Where the chord works.** Like Frame Selected, which shares the F key: with the level viewport focused or under the pointer, or the Outliner or Details focused, and never while a text field, the console or a modal has the keyboard. In an asset tab Shift+F is that tab's own. Frame Selected never checked Shift, so while ejected it stands down for as long as the Pawn to Camera chord is held (`KeybindRegistry::held`), and only when there is a pawn to move; with none, Shift+F frames the selection as before.

---

## Phase 14 — Vehicles that drive

A placed car can be a real vehicle in Play: a Jolt wheeled vehicle with suspension, tyres, an engine and gravity, that collides with the world and the player, driven by an AI that follows the level's road lanes. It is the physics counterpart of Phase 12's transform clip — and it replaces one, for road traffic: trains, boats and flyers stay on their animated paths. Like a clip, it plays only in Play; outside Play the placement is a mesh at its pose.

**In the file.** One token on a `PLACE` record, `vehicle <preset>`, with `<preset>` one of `car`, `van`, `truck`, `bus` or `sports`; and one sidecar, `<level>.oclanes`, beside the level under the same stem, holding the directed lanes the traffic follows (`docs/formats/FORMAT_SPECS.md`). A placement carries `vehicle` and no `anim`. The lanes are read only when the level has a vehicle placement, so a level without traffic never looks for the file. A level with vehicles and no lane file is legal: its cars park, brakes on. A recovered autosave finds its level's lane file (the sidecar of `<level>.ocworld.autosave` is `<level>.oclanes`), and Save As copies the lane file beside the new level, so a copy's cars still have roads. **A save writes the token back**: the editor keeps a record of which entities are cars, and a save that rebuilt a placement without it would turn every car into a plain mesh with no collider. A save or an autosave DURING Play writes each car's placed pose, as it does for an animated placement, not the point on the road it has driven to.

**Details panel → Mesh.** A vehicle placement shows one read-only line, `vehicle <preset>`. There is no editing UI for it: the generator writes the token, the preset's physical parameters live in `world::VehicleSystem`, and the car is sized from the mesh's own bounds. A placement the level file declared as a vehicle has one, and so has one added with a `vehicle` token through the MCP `level::place`; a delete that is undone brings the car back as a car, under its new entity. A copy made with Copy or Duplicate is an ordinary mesh until the file says otherwise.

**No body at load.** A placement with a `vehicle` token gets no static or kinematic collider from `world::instantiate`, whether or not it says `nocollide`: Play builds its dynamic chassis, and a second solid car at the placement would be a wall for the first. The editor's own paths agree: a car never gets a static body from the Collides toggle or from a drag commit either (`rebuildEntityBody` returns early for it).

**Play and Stop in the editor.** The cars are built in `capturePlayWorld`, right after `editor::syncPhysicsFromScene` and after the level's transform snapshot — the CRigidBody rule: a car settling on its suspension while the level is only being looked at is the complaint that rule exists to prevent. `GameLevel::beginVehicles` builds one vehicle per `vehicle` placement that can be built, at the entity's **current** world transform, so a car the user nudged drives from its new position, and each is assigned to the nearest lane that agrees with its heading. A placement is skipped, with one counted log line per reason, when its entity has been deleted, when its mesh has no known bounds (read from the entity's mesh at Play start, so Change Mesh is honoured), when it is not at scale 1, or when it also carries an object animation, whose clip would overwrite the physics pose every frame; a skipped placement stays where it was placed, without collision. Their entities are seeded as movable in `PlayMobility`, so they cost the GI bake nothing for moving. Every frame the Play gate lets through, `VehicleSystem::prePhysics(dt, camera position)` runs immediately before `game::tickGameplayGroups` — each driver sets its car's throttle, brake and steering for the physics step inside it — and `postPhysics` right after it writes each entity's transform from its body, before `driveAnimatedBodies` and the world flush. Pause and Frame Skip hold the traffic with everything else, because they hold the gate. A car whose entity is destroyed during Play (by a graph or a script) is destroyed with it, and leaves the lane queues. Stop ends the vehicles first, then `restorePlayWorld` puts every placement back where Play found it. A `begin_play` that fails ends them too, since no Stop follows it.

**The runtime.** `AverEngineRuntime.exe` builds the same cars in `GameApp::onInit` right after `beginPlayIfGameModeDeclared`, and only when that left a play session PLAYING; it drives them from `GameApp::tickGameplay` around `tickGameplayGroups`, and ends them in `onShutdown` before the level unloads. `GameLevel` is the one loader for both hosts, so the placements and the lane file reach the two the same way. The cars advance only while a play session is PLAYING, because that is when physics steps: a packaged project that declares no GameMode never builds them, so its cars are ordinary meshes at their placements and stay in the GI bake.

**The mesh's origin is the bottom centre of the car.** The chassis box is centred on it and lifted by the ground clearance, and the wheels hang a radius above it, so a car mesh must have its origin at the middle of its footprint at the height of its tyres' lowest point. Jolt rests each suspension spring at full length, so the weight compresses it by its static sag (about 6 cm at 2 Hz, 11 cm at 1.5 Hz) and the origin would settle that far into the road; the wheel attachments are therefore lowered by `g / (2 pi f)^2`, which puts the settled origin on the road to within a centimetre or two (worked by hand from Jolt's spring formula, not measured on a full level). `VehicleSystem::begin` logs one warning counting meshes whose bounds are centred more than 15% off the origin sideways, or whose lowest point is more than 5% of their height off it.

**Cost.** A wheeled vehicle casts one shape per wheel per fixed step, per car. `VehicleSystem` therefore runs the AI every frame only for cars within a fixed distance of the focus (the camera, in both hosts) and every fourth frame for the rest, which keep their last input; the distance is a constant in that class. The traffic cost has not been measured on a full city; do not quote a number for it before it is.

---

## Driving it from the command line

Flags on `Sandbox.exe`, alongside `--frames`, `--screenshot`, `--probe` and the rest. Those below exist for one reason: each names a path whose only proof was that somebody had clicked it once.

| Flag | What it does |
|---|---|
| `--ui-demo` | turns on `View → Game UI Demo` — the hand-written `Aver.UI` draw list, submitted through the C seam and drawn onto the backbuffer by `Aver.Render.UI`. Animated, so a stale vertex buffer cannot look identical to a live one |
| `--new-project <location> <name>` | scaffolds a Blank project and exits, touching no device. Creation was reachable only from the browser's modal, so a generated `Scripts.csproj` that MSBuild refuses to load is exactly the kind of thing that ships silently — nobody creates a project on the day they change the generator |
| `--new-project-template <location> <name> <templateId>` | the same, from one of the shipped `templates\` directories (see the New Project modal's template picker) instead of Blank. `templateId` is the template's folder name, e.g. `FirstPerson`. Exits 1 with a clear message if no template by that id is found — the same discovery `listTemplates()` does for the modal, so this is what a test drives instead of clicking a card |
| `--upgrade-project <path.ocproject>` | applies what the prompt would apply, logs each fix, and exits. The prompt is how a person does this; a flag is the only way a test does, and the apply path edits somebody's build file |
| `--save-level <out.ocmap>` | writes the **open level** to `<out>` once, on the first frame after the renderer attaches, and logs the path. `saveLevel` had no caller but a mouse — Ctrl+S, `File ▸ Save Level`, the toolbar button — so no round-trip could be checked without a person in front of the window. It writes **elsewhere**, never over `levelPath_`, so proving the save costs nothing it proved on. Pair it with `--open-legacy` to save a level exactly as it exists on disk |
| `--ray-probe <sx> <sy>` | reports what `viewportRay` returns for one screen point, on the same late frame `--gpu-timing` uses. `pick`, `handleSculpt`, `handleFoliage` and `dropWorldPoint` all go through that one function and none of them is reachable without a mouse, so the editor's screen-to-world conversion had no headless witness at all. It prints the origin's distance **in front of the eye** along the view axis -- which must equal the near plane, and was exactly 0 while the origin was hardcoded to `eye_` -- and the reprojection of a point on the ray, which must come back to the pixel that was asked for |

The four project flags — `--new-project`, `--new-project-template`, `--upgrade-project` and
`--save-project` — are matched before every other argument and call `std::exit`; none opens a
window. `--save-level` is the exception and has to be: it needs a level open and a renderer
attached, so it rides a normal `--frames` run rather than short-circuiting one.

---

## Theme & branding

Dark base `#1e1e1e`, neutral chrome, **one accent = Aver Amber `ImVec4(1.0,0.42,0.17,1)`** (same family as the mark's lit face and the sandbox object color; reads on dark and light). `StyleColorsDark()` + rounding 3–4, `FramePadding{8,4}`, `ItemSpacing{8,6}`; accent on CheckMark, SliderGrab, ButtonActive, TabActive, DockingPreview (0.5α). Ship a Light variant on `View → Theme`. Logo at far-left of the menu bar (the Isocube AE rasterized into the ImGui font atlas / an SRV, drawn via `ImGui::Image`) and again in `Help → About`. Accent is the only saturated color anywhere in the chrome.

---

## Build wiring

**This section described a target that was never built, and it is worth saying so plainly.** There is no `Aver.Editor`, no `AVER_BUILD_EDITOR`, and `add_subdirectory(editor)` appears nowhere. `editor/` used to hold a README for a C# editor with no source behind it; that directory has since been **deleted**, because a folder whose only content describes something that was never started is a second, worse copy of this paragraph. The editor **is** `Sandbox`, and there is no second binary: the same executable runs the oracle gates under `--frames`.

What actually builds it is `sandbox/CMakeLists.txt` behind `option(AVER_BUILD_SANDBOX ON)`: `SandboxApp.cpp` plus `AssetEditor`, `ActorEditor`, `ProjectBrowser`, `ProjectScaffold`, `EngineScaffold`, `IdeIntegration`, `ShellIntegration` and `ToolsMenu`, linking `Aver.Runtime Aver.Formats Aver.UI Aver.Render.UI Aver.UI.Abi Aver.Render.ActorPreview` unconditionally and each optional module when its target exists. **The next sentence has been overtaken by a split.** ImGui is still `third_party/imgui`, added under `option(AVER_ENABLE_UI ON)`, but it no longer reaches the executable "transitively through `Aver.RHI.D3D12`" — `Aver.RHI.D3D12` never links imgui or defines `AVER_WITH_IMGUI` itself any more. The concrete Dear ImGui binding is its own module, `Aver.RHI.D3D12.ImGui` (`modules/rhi.d3d12.imgui`, DEPS on `Aver.RHI.D3D12`, nested under `if(AVER_ENABLE_UI)`), and `sandbox/CMakeLists.txt` defines `AVER_WITH_IMGUI=1` itself, gated on `if(TARGET Aver.RHI.D3D12.ImGui)` — which the file's own comment says "reproduces exactly the pre-split behaviour" seen from Sandbox's side. A Vulkan counterpart now exists too, `Aver.RHI.Vulkan.ImGui`, wired the same way behind its own `AVER_WITH_IMGUI_VULKAN` macro; this document predates it entirely. ImGuizmo is not vendored and never was.

## One-line data flow
`WndProc → (ImGui hook | Event)` → **onUpdate**: ImGui builds panels, mutates `EditorState` (tool/selected/camera/per-object Transform+baseColor), pushes `setCamera/setLight/setClearColor`, `ImGui::Render()` → **beginFrame** → **onRender**: `setFillMode` + grid + `drawMesh(toMatrix, baseColor[+tint])` + cage + `imguiRender()` → **endFrame/present**.

The actor editor rides the same frame but not the same pass: **onUpdate** stats the `.cs`, re-parses it if it moved, rebuilds a `PreviewDraw` list from the parsed rows and hands it to the feature; **prePass** (before the scene binds the backbuffer) renders that list into the preview's own target with its own camera at `b4`; the panel then samples the result as a UI texture and draws the gizmo over it. Nothing is spawned, no world is touched, and no play state is involved — what is on screen is what the source says, which is what makes editing the text and dragging in the view the same operation.
