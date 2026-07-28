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

**0.3 D3D12 impl** (`D3D12Device.cpp`)
- `imguiInit`: create a SHADER_VISIBLE `CBV_SRV_UAV` heap; `ImGui_ImplDX12_Init(device, kFrameCount, kBackbufferFormat, srvHeap, cpuHandle, gpuHandle)`. It holds **16** descriptors, not the 64 this line used to claim — `kUiSrvCount` in `modules/rhi.d3d12/src/D3D12Device.cpp`. Every texture the UI samples comes out of that one pool: the editor's own icons, and any offscreen target a panel draws. That budget is why the actor preview (§Phase 8) is shared rather than one per tab.
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

**Two things the header promises and the code does not do.** `AssetEditor::dirty()`'s comment says the host asks before closing and asks again before the application exits. Neither prompt exists: closing a dirty editor logs `AVER_WARN` and drops the edits, and `AssetEditorHost::anyDirty()` — the call exit would have to make — has no callers anywhere in the tree.

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

**One preview, shared, driven by whichever tab is active.** Not one per tab, and the reason is a hard limit rather than thrift: the UI SRV pool is sixteen descriptors (§Phase 0.3) and the editor already spends several on its own icons, so a target per tab exhausts it at about eleven and the failure is a black image, not an assert. A second tab still shows its model list and its numbers; it just does not get the 3D until it is focused.

It is created on **first draw** — `AssetEditorFactory` is a bare function pointer with no device to hand it — registered as a render feature there, and removed before it is deleted, because the device holds features non-owningly and a deleted one leaves it calling `prePass` on freed memory, which nothing reports. The content root arrives the same way, pushed by `applyProject` through `setActorEditorContentRoot`. Meshes resolve on demand through the preview's **own** registry (`PreviewMeshCache`), not the level editor's table: an asset editor that reached into level state could not exist without a level open, could not be tested without one, and would quietly make "this tab is independent of the level" false. A miss is cached as handle 0, or an actor naming an absent mesh re-reads the disk once per model per frame and the editor's frame time becomes a function of how wrong the file is. Every lookup goes through `fmt::canonicalMeshPath`, so `Content/Meshes/X.ocmesh` and `Meshes/X.ocmesh` collapse to one entry and one upload.

**The gizmo translates, and that is all.** Three axes, drawn as an ImGui overlay on top of the image rather than as geometry in the pass — the opposite of the level viewport's choice, for two reasons: the preview feature has to stay drivable with no ImGui at all (that is what lets a test be the device), and a handle has to be pickable at a constant SCREEN size, which it cannot be as part of a scene that scales. It projects through the preview's own `viewProj()`, because deriving that matrix independently is how a handle ends up a few pixels off its object and then a few more as the camera turns. Shaft 64 px, grab radius 10 px, distance measured to the *segment* so the near end of an axis is not dead. The axis is latched on mouse-down and held for the whole gesture: deciding per frame would turn a drag that began on the handle into an orbit the moment the cursor left it, which is exactly when the user is dragging fastest. A drag anywhere else orbits; the wheel zooms multiplicatively, so a notch moves the same proportion whether the subject is a bolt or a vehicle; pitch is clamped to ±85° short of the pole.

**There are no rotate or scale handles.** Rotation and scale are the drag-floats and nothing else. There is no snapping, no numeric readout during a drag, and no undo of any kind in this tab — the level editor's undo stack does not reach it.

**Reload on mtime, refused while dirty.** Every frame the tab draws, it stats the file. A watcher would be the general answer; a stat is the honest small one, since a visible tab is polled anyway and a thread plus an OS handle plus an overflow case is more to get wrong. Changed → re-read, re-parse, re-pick the previewable class, drop the selection if it no longer indexes a row, status `Reloaded from disk.` A parse that returns `Malformed` or `UnknownSchema` keeps the previous good parse and shows the reason, so a file caught mid-write by an IDE does not blank the tab.

If the tab is dirty the reload is **refused**: *"The file changed on disk. Save or discard to pick it up."* Silently replacing somebody's in-progress drag with what a background tool wrote is the one behaviour a live-sync feature must never have. The stamp is taken *before* the refusal, so the message appears once rather than every frame.

**What a save does.** `Save to C#` (disabled unless dirty) calls `fmt::rewriteActorScript`, which matches placements **by ObjectId** — never by position in the file, so a reordered region, an inserted row or a renamed property still finds the right line — and rewrites the three coordinate tuples of each. Every other byte is left alone: the marker lines, the usings, the namespace, the class header, every field of a placement other than pos/rot/scale, and the hand-written half of the actor, which this never even reads. Models the file does not contain are ignored rather than appended, because adding a placement has to mint an id and declare a property, and doing that silently from a coordinate save would be surprising. It writes the `.cs`, and only the `.cs`. On success the in-memory source becomes what is now on disk, so a second save rewrites from the file as it stands rather than from the text the tab was opened with.

A file with no region — the single-mesh actor declared on the class — is a **viewer**. It has no placements to edit, so nothing can mark it dirty and the save button never enables. That is the honest state rather than a save with nowhere to write.

**Rough edges worth knowing.** The save does not re-stamp the write time, so the next frame's reload fires on the tab's own write; it is harmless, the text being identical, but it replaces `Saved.` with `Reloaded from disk.` immediately. And the selection is an *index* into the parsed models rather than an ObjectId, so a reload that reorders the region moves the selection to a different placement.

**There is no Roslyn backend.** `ActorScript.hpp` describes two and declares `ActorParserBackend::Roslyn`, but nothing in the tree produces it: there is no `averdesign` tool, and every parse is the builtin scanner. So the escalation that header describes does not happen — a named argument moved, an argument omitted, a coordinate written as an expression, a `#if` around a placement, or any hand edit inside the region, and the file is declined rather than opened by a second parser.

---

## Phase 9 — The material authoring loop

**C# is the source of a surface.** A `.cs` under `Content\Materials` declares `[AverMaterial("M_Name")]` and configures it; `avermatc` reflects the built assembly and writes `<project>\Binaries\Materials\<name>.ocmat`, which is what the engine reads. `materialForSurface` tries `Binaries\Materials\<name>.ocmat`, then `Content\Materials\<name>.ocmat`, then the name as a content-relative path. Binaries wins rather than merging, because the generated file is the newer of the two by construction — it is rewritten from source on every build — and a stale hand-authored file left beside the source must not shadow it.

**What you see.** `Details → Material`, for a selected object: Metallic, Roughness, Normal Scale, Occlusion, Reflectance (stopping at 0.2, which is gemstone — a 0..1 slider would spend four fifths of its travel above anything real), Grazing (f90), Emissive, a UV Mapping combo (Mesh UVs / World Aligned) with a logarithmic tile size in centimetres, and one path field per texture slot. Every control edits the `pbr::MaterialDesc` through the library and then calls `touch()`, because `mutableDesc()` hands out a raw pointer and marks nothing — an edit that forgot it would show in the panel and never reach the GPU. The consequence a user sees is that the viewport changes the same frame, and that two objects sharing a material change together, which is the point of editing the material rather than the actor.

**What a save does.** `Save to C#` writes the `.cs` and **never** the `.ocmat`: the `.ocmat` under `Binaries` is a build artefact the next compile overwrites, so writing there is a change that appears to work and then silently vanishes — the worst behaviour a save button can have. The declaring file is *found* rather than recorded, because one `.cs` may declare several materials and the `.ocmat` says nothing about which: the editor walks `Content\Materials\**\*.cs` and tries `fmt::rewriteMaterialScript` on each, and since a file that does not declare the name declines, trying each in turn is both the search and the check. A rewrite that changes nothing returns without touching the mtime. What it rewrites is the body of `Configure` and nothing else — usings, namespace, other classes, attributes, the file's own indentation of the chain, and the `.Comment(...)` calls all survive. Those comments are authored prose that no `MaterialDesc` can regenerate, and losing them on the first save would lose the reasoning behind the numbers. Only values that differ from the defaults are written, so a material that sets little produces an almost empty `Configure` rather than eighteen lines restating defaults.

Then `Compile C#` — the toolbar button; `Tools → Compile Scripts` is the same job — runs `dotnet build Scripts.csproj -o <project>\Binaries\Scripts` and, on success, `bin\Tools\avermatc.dll` over the built `Scripts.dll`. Baking happens on the build thread right after the compile that produced the assembly it reflects: a separate button would be a button somebody forgets, and a surface one build behind looks like a material bug rather than a missing step. A bake failure does **not** fail the compile — the C# built, the assembly is good, the previous `.ocmat` stays, and the log gets a `[materials]` section. Turning a bad `Configure` into a red build light would read as a compile error, which it is not.

**What is not there.** Nothing re-reads a `.ocmat` after a bake: materials load when a project opens and are released when it closes, so the round trip only shows on the next open. The sliders being live hides this until the source and the loaded material disagree. There is no *New Material* menu item, although `fmt::newMaterialScript` exists and will write the whole file. And the Material header appears only for the editor's placeholder objects — once a level has placed entities, or a play session is running, those are hidden and unpickable, and a scene entity's Details offers Transform and Mesh only.

---

## Phase 10 — Projects: what New, Open and Upgrade write

`sandbox/src/ProjectScaffold.{hpp,cpp}`; the layout itself is docs/PROJECTS.md's.

**New Project** creates `<location>\<name>\` and refuses an existing folder — a half-scaffolded project on top of someone's data is worse than a failed button. It writes the manifest, `Content\{Maps,Meshes,Materials,Textures,Sounds,Scripts}`, `Content\Scripts\Scripts.csproj`, and `Content\Materials\Surfaces.cs` declaring `M_Default`. The `.csproj` is written **at creation** rather than lazily on the first script, which is where it used to appear: a project without one cannot Compile C#, Compile C# is what bakes materials, so a project whose first authored thing was a material had no way to build it — and the button that would have said so was disabled for want of the file it was about to create. It carries four engine references (`Aver.Scripting`, `Aver.Framework`, `Aver.UI`, `Aver.Materials`) and a `<Compile Include="..\Materials\**\*.cs" />` item. The starter material exists for the reason the script templates do: the shape of a material is not guessable, and an empty `Materials` folder teaches nothing.

The manifest names `STARTMAP Maps/Default.ocmap` and **no map file is written**, so a fresh project opens with no level — `loadStartMap` says so in the log and the world starts empty.

**Opening an older project asks.** `inspectProject` reads and writes nothing; if it finds anything, a modal names every fix — a folder that will appear, an engine reference that will be added, a `ProjectReference` whose target has moved. A list, not a reassurance: the one thing an author has to decide is whether they mind, and "upgrade your project" gives them nothing to decide with. *Not now* leaves every file untouched and is remembered for the session, because a deliberately minimal project is a legitimate project and an editor that asks on every open is one people learn to dismiss without reading. The `.csproj` is **merged, never regenerated** — missing items are appended as a new `ItemGroup` before `</Project>`, which MSBuild merges with whatever is already there — because a project may carry a PropertyGroup, a PackageReference or a target somebody added by hand, and replacing the file to add one reference would throw all of it away. Nothing is modified without a yes.

---

## Driving it from the command line

Flags on `Sandbox.exe`, alongside `--frames`, `--screenshot`, `--probe` and the rest. The three below exist for one reason: each names a path whose only proof was that somebody had clicked it once.

| Flag | What it does |
|---|---|
| `--ui-demo` | turns on `View → Game UI Demo` — the hand-written `Aver.UI` draw list, submitted through the C seam and drawn onto the backbuffer by `Aver.Render.UI`. Animated, so a stale vertex buffer cannot look identical to a live one |
| `--new-project <location> <name>` | scaffolds a project and exits, touching no device. Creation was reachable only from the browser's modal, so a generated `Scripts.csproj` that MSBuild refuses to load is exactly the kind of thing that ships silently — nobody creates a project on the day they change the generator |
| `--upgrade-project <path.ocproject>` | applies what the prompt would apply, logs each fix, and exits. The prompt is how a person does this; a flag is the only way a test does, and the apply path edits somebody's build file |

Both project flags are matched before every other argument and call `std::exit`; neither opens a window.

---

## Theme & branding

Dark base `#1e1e1e`, neutral chrome, **one accent = Aver Amber `ImVec4(1.0,0.42,0.17,1)`** (same family as the mark's lit face and the sandbox object color; reads on dark and light). `StyleColorsDark()` + rounding 3–4, `FramePadding{8,4}`, `ItemSpacing{8,6}`; accent on CheckMark, SliderGrab, ButtonActive, TabActive, DockingPreview (0.5α). Ship a Light variant on `View → Theme`. Logo at far-left of the menu bar (the Isocube AE rasterized into the ImGui font atlas / an SRV, drawn via `ImGui::Image`) and again in `Help → About`. Accent is the only saturated color anywhere in the chrome.

---

## Build wiring

**This section described a target that was never built, and it is worth saying so plainly.** There is no `Aver.Editor`, no `AVER_BUILD_EDITOR`, and `add_subdirectory(editor)` appears nowhere — `editor/` holds a README for a C# editor that has no source. The editor **is** `Sandbox`, and there is no second binary: the same executable runs the oracle gates under `--frames`.

What actually builds it is `sandbox/CMakeLists.txt` behind `option(AVER_BUILD_SANDBOX ON)`: `SandboxApp.cpp` plus `AssetEditor`, `ActorEditor`, `ProjectBrowser`, `ProjectScaffold`, `EngineScaffold`, `IdeIntegration`, `ShellIntegration` and `ToolsMenu`, linking `Aver.Runtime Aver.Formats Aver.UI Aver.Render.UI Aver.UI.Abi Aver.Render.ActorPreview` unconditionally and each optional module when its target exists. ImGui is `third_party/imgui`, added under `option(AVER_ENABLE_UI ON)` and reaching the executable transitively through `Aver.RHI.D3D12`, which links it `PUBLIC` and defines `AVER_WITH_IMGUI=1` — which is why every editor source guards on that macro rather than on a target. ImGuizmo is not vendored and never was.

## One-line data flow
`WndProc → (ImGui hook | Event)` → **onUpdate**: ImGui builds panels, mutates `EditorState` (tool/selected/camera/per-object Transform+baseColor), pushes `setCamera/setLight/setClearColor`, `ImGui::Render()` → **beginFrame** → **onRender**: `setFillMode` + grid + `drawMesh(toMatrix, baseColor[+tint])` + cage + `imguiRender()` → **endFrame/present**.

The actor editor rides the same frame but not the same pass: **onUpdate** stats the `.cs`, re-parses it if it moved, rebuilds a `PreviewDraw` list from the parsed rows and hands it to the feature; **prePass** (before the scene binds the backbuffer) renders that list into the preview's own target with its own camera at `b4`; the panel then samples the result as a UI texture and draws the gizmo over it. Nothing is spawned, no world is touched, and no play state is involved — what is on screen is what the source says, which is what makes editing the text and dragging in the view the same operation.