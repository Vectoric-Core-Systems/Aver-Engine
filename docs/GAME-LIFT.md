# Lifting the game half out of SandboxApp

## Status: Complete and Current

> **Read in order — the story reversed twice.**
>
> **1. What the plan did (2026-08-17).** Commits C1–C11 lifted the game rendering half out of `SandboxApp.cpp` into `modules/runtime.game` and created `AverGame.exe`.
>
> **2. Then the executable was deleted (`b262c73`, "The packaged game is gone").** The stated reason was real: a second host "rendered a different subset of the scene than the editor", and nothing in the tree could notice. The *library* never left — it kept being built and kept growing.
>
> **3. Then it came back, and that is the current state (2026-09-16).** The library `Aver.Runtime.Game` (`Runtime/include/aver/game/`, `Runtime/src/`) and a restored standalone host moved to the top-level `Runtime/`, and the executable is now **`AverEngineRuntime.exe`**. `modules/runtime.game/` and `AverGame.exe` no longer exist. **What is different this time is a check, not a promise:** `scripts/verify-game.ps1` runs a divergence gate that opens the same project and level in both hosts under `--frames` and compares them, so "the game draws what the editor draws" fails rather than being claimed. `Runtime/host/CMakeLists.txt:9-27` records the whole reversal in the source. The de-duplication this plan called "a later slice" has since happened too: the editor now **calls** the runtime library for content, level load, water, streaming, landscape, physics/audio/tick, camera, mouse capture, input publishing and the world draw walk, rather than keeping a parallel copy. **There is one runtime with two hosts.** `AVER_BUILD_GAME` is still the option, and it builds `Aver.Runtime.Game` **and** `AverEngineRuntime.exe`.
>
> **Every path and line number in C1–C11 below is historical.** They were accurate at 2026-08-02 and have not been re-resolved (`SandboxApp.cpp` alone went from 29,952 lines to ~2,500 in the 2026-09-16 file split; the lifted code now lives in `Runtime/src/`). Read this document for the design reasoning — the re-guarding strategy, the separation of concerns, the framework integration, and the traps section, which documents engine-wide patterns still relevant to any future lift — not for coordinates.
>
> Produced 2026-08-02 by five parallel readers over `SandboxApp.cpp:1,2000` plus a refutation pass: 57 risks raised, **2 survived**.

---

# Lift Plan — SandboxApp game half → Aver.Runtime.Game

**Scope of the slice.** The game opens a project, indexes content, registers meshes, loads the start map and draws the world. This was a **copy**, not a move: `sandbox/src/SandboxApp.cpp` was not edited by any commit. De-duplication was a later slice, proven by the gates being bit-identical.

## The guard decision, made once and up front

The bidirectional PBR/SCENE cross-contamination in `SandboxApp.cpp` was real and already broken at HEAD. A verbatim lift inherits it. **The destination re-guards instead**, and this is the rule every commit followed:

| Symbol | Guard at the destination | Was |
|---|---|---|
| `contentIndex_`, `rebuildContentIndex` (walk only), `resolveAnimAsset`, `projectPath`/`ProjectDesc` | **unguarded** | PBR |
| `rebuildContentIndex`'s `anim::animSystem().clear()/setResolver()` tail | `#if AVER_MODULE_SCENE` | PBR |
| `sceneMeshes_`, `meshBounds_`, `projectMeshIds_`, `resolveSceneMesh`, `loadProjectMeshes`, `releaseProjectMeshes`, `surfaceLooks_`, level load, draw walk | `#if AVER_MODULE_SCENE` | mixed PBR/SCENE |
| `textureFactory_`, `materialAssets_`, `resolveAssetPath`, `resolveMaterialTexture`, `materialForSurface`, `releaseProjectMaterials` | `#if AVER_MODULE_PBR` | PBR |
| `surfaceMaterials_`, `loadProjectMaterials` (they key on `aver_scene_material`) | `#if AVER_MODULE_PBR && AVER_MODULE_SCENE` | PBR |

`contentIndex_` becoming unguarded is what kills the cross-contamination at the root: it is a `std::unordered_map<u64, std::string>` and `fnv1a64` lives in `Aver.Core` (`modules/core/include/aver/core/Hash.hpp`), so nothing about it needs either module. `docs/PACKAGING.md` once claimed the relocation "fixes a bug"; it does not — only the re-guarding fixes it.

---

## Architecture

The shared runtime consists of:

- **`GameApp`** (`GameApp.hpp/.cpp`): the application host, managing project open, level load, frame loop, and lifecycle. Mirrors the editor's `applyProject` and `onUpdate`/`onRender` patterns.
- **`GameContent`** (`GameContent.hpp/.cpp`): content index, mesh registry, material resolvers, and asset paths. A plain class owned by `GameApp` and passed by reference to level and render code — do **not** reproduce the SandboxApp god-object: the static resolvers take `void* user = &content_`, not `&app`.
- **`GameLevel`** (`GameLevel.hpp/.cpp`): level instance with entity list, fog settings, GI bounds, physics bodies.
- **`GameRender`** (`GameRender.cpp`): the draw walk: frustum culling, entity filtering, material binding, and draw submission for the scene.
- **`GameInput`** (`GameInput.cpp`): ImGui-free input binding to `aver_fw_*` functions, using `InputState` (Win32 virtual key codes) rather than ImGui key enums.
- Later additions: `GameLandscape`, `GameStreaming`, `GameWater`, `GameFoliage`, `MouseCapture`, `PlayMobility`, and `GameTick.hpp` (the gameplay tick order).

---

## The slices, as planned (design reasoning kept; coordinates dropped)

**C1 — CMake: give `Aver.Runtime.Game` its link interface.** `AVER_MODULE_PBR=1`, `AVER_MODULE_SCENE=1` and `AVER_MODULE_VOXI=1` are PUBLIC on their own targets, and none reached `GameApp`'s TU, so every `#if` in the lift was false. Added, mirroring `sandbox/CMakeLists.txt`: unconditional `Aver.Formats` (carries `Aver.Assets` transitively) and `Aver.Render.Skin`; and under `if(TARGET …)`: `Aver.Render.PBR` + `.Materials` + `Aver.Formats.Material` + `Aver.Assets.Gpu`, `Aver.Render.Voxi` + `.Renderer`, `Aver.Scene`, `Aver.Anim.Scene`, `Aver.Physics`, `Aver.Framework`. The `if(TARGET …)` guards are mandatory: an unconditional `Aver.Scene` is a configure error in an `AVER_MODULE_SCENE=OFF` tree. `Aver.RHI` is **not** needed — it is already PUBLIC through `Aver.Runtime`. DONE-WHEN: a `[Game] modules: PBR=1 SCENE=1 VOXI=1 PHYSICS=1 FRAMEWORK=1 SCRIPTING=1` log line; the line is its own oracle, and a `0` in it means the link interface is still missing.

**C2 — Project open without `ProjectBrowser`.** The game calls `fmt::loadOcproject` directly, which sets `out.manifestPath` and `out.dir` before parsing and rejects a foreign or too-new `ENGINE` line. `GameConfig` gained `projectPath`; `parseArgs` takes any bare argument whose extension is `.ocproject` (case-insensitive) plus an explicit `--project <path>`. The editor-only parts of `applyProject` are dropped (actor/anim editor content roots, content watcher, upgrade offer, editor window title). DONE-WHEN: a nonexistent path logs the `loadOcproject` error string and the process still exits 0.

**C3 — Content index and asset resolvers.** The index walk (now `GameContent::adopt`) uses the `error_code` overload of `recursive_directory_iterator::increment`, computes `std::filesystem::relative`, rewrites `'\\'` → `'/'` (**FROZEN** to match C# `Assets.ObjectIdOf`), and stores `contentIndex_[fnv1a64(rel)] = native absolute path`. **Keep the `anim::animSystem().clear()`**: it is required on project adoption, because AnimSystem caches by id and a moved asset would otherwise keep resolving to its old path; it is safe because the index is rebuilt only from the project-adoption path, never per frame. DONE-WHEN: `[Content] indexed N asset(s) under ROOT` prints the same count and root in both hosts for the same project. The hash spelling is pinned by `tests/formats/src/FormatTest.cpp` (`fnv1a64("Meshes/sphere.ocmesh") == 672114764054563281`); no new test was required.

**C4 — Mesh registry: built-ins and `loadProjectMeshes`.** `loadProjectMeshes` filters on `assetTypeFromPath(full) == AssetType::Mesh`, repacks `fmt::OcMeshData` into `rhi::MeshVertex` (`{f32 px,py,pz,nx,ny,nz,u,v}`) copying **only** position/normal/uv — `rhi::MeshVertex` has nowhere to put joints and weights, so a skinned asset arrives as static geometry here — then `createMesh` and writes `sceneMeshes_[id]`, `meshBounds_[id]`, `projectMeshIds_`. In `GameContent` it takes `rhi::IDevice&` instead of `Engine&`. `releaseProjectMeshes` and `resolveSceneMesh` go under SCENE (in SandboxApp `releaseProjectMeshes`'s body was unguarded — one of the seven compile bugs). Built-in primitives: `Meshes/sphere.ocmesh` = `appendSphere(1.0f, 24, 48)`, `Meshes/cube.ocmesh` = `appendBox(…, 1.0f)` — half-extent 1 is FROZEN (see trap 13). `surfaceLooks_` is seeded with the seven `M_*` tokens and their exact colour/metallic/roughness. `appendBox`/`appendSphere` are file-statics: **copy**, do not promote. DONE-WHEN: the `[Mesh]` summary prints the same registered/failed counts as the editor, and `sceneMeshes_.size()` equals that count plus the 2 built-ins.

**C5 — Level load.** `loadLevel` calls `unloadLevel()` first, then `fmt::loadOcworld` (one parser for `.ocworld` and `.ocmap`), and per placement builds a `Transform` (position in cm, `quatFromEulerDeg({roll,pitch,yaw})`, scale `sx/sy/sz`) and `world.create(p.asset, scene::kInvalidEntity, xf)` — the entity's `CName` **is** the asset path — then adds `CMeshRenderer` and writes exactly three fields: `mr->mesh = p.objectId`, `mr->material = p.material.empty() ? 0 : aver_scene_material(0, p.material.c_str())`, `mr->flags |= kMeshRendererVisible`. It writes **no bounds**. The physics static-box branch was lifted verbatim and is a no-op until `aver_phys_init()` (it already tests `aver_phys_ready()`). `loadStartMap` returns unless `project_.valid() && !project_.startMap.empty()`; a missing start map is INFO, not an error. **`scene::World::instance().flush()` must run in `onUpdate`**, or `World::worldMatrix(ent)` in the draw walk reads stale matrices — `flush` retires deferred destroys, rebuilds topological order and recomposes stale world matrices. Load-bearing order from the editor's `applyProject`: index → meshes → start map (the index must precede the meshes because the resolvers read `contentIndex_`, and the meshes must precede the level because `mr->mesh` resolves through `sceneMeshes_`). Editor-only state dropped: entity labels, undo/redo, selection, `saveLevel`, and `hasLevelSun_`/`hasLevelSky_` (their only readers were `saveLevel` and `unloadLevel`). DONE-WHEN: `[Level]` logs an entity count equal to `grep -c '^\s*PLACE'` on the start map; `unloadLevel` then `loadStartMap` returns to the same count (no leak, no double-spawn).

**C6 — Camera and the per-frame device push (the aspect fix lands here).** Lifted: `camForward()`, the view/proj/`setCamera` block, the sky/fog/post push (re-deciding the clear colour, see "The aspect ratio"), and `frameCameraOn` — keeping `camPos_`/`yaw_`/`pitch_` and `giCenter_`/`giExtent_`, dropping `flySpeed_`. `viewAspect()` cannot be lifted and `setViewportRect` is not lifted at all. Dropped: `vpX_..vpH_`, `flySpeed_`/`lookSpeed_`, `levelHovered_`/`levelFocused_`/`browserActive_`. DONE-WHEN: `--frames 3 --width 800 --height 600` and `--width 1600 --height 900` print `aspect=1.333` and `aspect=1.778`, tracking a live resize — impossible to pass with the dockspace formula, and it needs no baseline.

**C7 — The world draw walk (first pixels in the game).** The whole `#if AVER_MODULE_SCENE` block, minus editor lines:
- **Frustum planes**, row-vector and **unnormalised**: `pl[0][i]=m[i][3]+m[i][0]` (left), `[1]` right, `[2]` bottom, `[3]` top, `pl[4][i]=m[i][2]` (near, `[0,1]` depth), `[5]` far; derived per frame from `viewProj_`.
- **Entity walk:** `w.at(i)`, skip `w.destroyPending(ent)`, fetch `CMeshRenderer`, skip on null / not visible / `mr->mesh == 0` / `sceneMeshes_` miss, then `w.worldMatrix(ent)` — non-const, so the walk needs a non-const `scene::World&`.
- **Bounds substitution:** for non-skinned entities only, `const_cast` and overwrite `mr->aabbMin/aabbMax` from `meshBounds_` every frame.
- **Positive-vertex frustum cull** with `xformPoint` on all 8 local corners. A degenerate box (`hi <= lo` on any axis) skips culling and is drawn.
- **Colour:** the `surfaceLooks_` fallback and the `{0.80,0.80,0.85,1.0}` default (the `surfaceMaterials_`/`MaterialSystem` half is C8); skinned substitution; then `drawMesh(mesh, &wm.m[0][0], col, metallic, roughness)`.
- **Counters** `lastSceneDrawn_`/`lastSceneCulled_` start at `-1`.
- Dropped: selection latch, skin-scene-test recolour, the `objects_` placeholder pass, selection outline, grid, gizmo, `buildUI`, and all five test harnesses including `captureCheck`, the gate probe.

DONE-WHEN: `[Game] scene-render: N drawn, M frustum-culled` with `N > 0` and `N + M` equal to the entity count C5 logged; turning the camera 180° must move entities from `drawn` to `culled` without changing `N + M` — a cull check that needs no reference image. (`scripts/gates.ps1` drives `Sandbox.exe`, not the game, so none of this could move an editor pixel.)

**C8 — Materials: the PBR half and the Voxi attach.**
- `resolveMaterialTexture` picks `assets::TextureUsage` **from the slot, never the filename** (BaseColor/Emissive → Colour, Normal → NormalMap, else Data) and calls `assets::uploadTexture`.
- `materialForSurface` tries three candidates **in this order**: `binariesDir()\Materials\<n>.ocmat`, `contentDir()\Materials\<n>.ocmat`, `contentDir()\<n>` (no extension appended on the third); the **first existing** candidate wins and a parse failure **breaks** rather than falling through. It caches `0` as a negative and never retries.
- `loadProjectMaterials` uses a **non-recursive** `directory_iterator` over `Content\Materials` only.
- > **CORRECTED 2026-09-13.** The runtime's lifted `loadProjectMaterials()` was never called and has been **removed**. It was not a dropped step: `GameApp` opens a project with `loadProjectMeshes` -> `loadProjectParticleEffects` -> `loadStartMap`, and `GameLevel::load` resolves each surface a level references through `materialForSurface`, lazily and Binaries-first, so eagerly loading every project material would only add load time and memory. The editor-order sequence `releaseProjectMaterials; rebuildContentIndex; loadProjectMaterials; releaseProjectMeshes; loadProjectMeshes; loadStartMap` is the **editor's** `applyProject`, not the runtime's. The editor keeps its own `loadProjectMaterials()`, which since `6875e36b` scans `Binaries\Materials` as well as `Content\Materials`.
- `pbr::MaterialSystem` is a member of `VoxiRenderer` and is initialised only in `VoxiRenderer::init`, so textured drawing requires attaching a `VoxiRenderer` as a render feature. Copy: `rhi::DeviceCaps` → `voxi::DeviceInfo`, `voxi::Renderer::get().setDeviceInfo/setSettings`, `voxiRenderer_.setSettings(s)` **before** `voxiRenderer_.init(*e.device())`, then `addRenderFeature`, `voxiAttached_ = true`, deferred `applyProjectRenderSettings()`, `textureFactory_ = e.device()->resources()` and `voxiRenderer_.materials().setTextureResolver(&GameContent::resolveMaterialTexture, &content_)`. **Keep those last two adjacent**, exactly as SandboxApp does — that adjacency is why the `!textureFactory_` guard in `resolveMaterialTexture` is unreachable. `voxiRenderer_` is held **by value** and registered non-owning; it must outlive the device.
- Restore the deferred pieces: `materialForSurface` at level load, and the `surfaceMaterials_` branch of the draw walk (`col = white, metallic = roughness = 1.0`, then `setDrawBinding(ms.bindingSet(authored), &ms.constants(authored), sizeof(pbr::MaterialConstants))` guarded by `ms.ready()`).

DONE-WHEN: the `[Material] <path> -> WxH, N mips (K KB) for slot '<s>'` line prints once per referenced texture with the same paths the editor prints; anything that fails prints `keeps its fallback`, so a silent pass is not possible. Additionally add the missing log at the genuine silent-fallback site (trap 4).

**C9 — Physics init and the tick groups.**
- `aver_phys_init()` and the ground static box (`groundBody_`, 5000 cm half-extent, 5 cm half-thickness) **must run before any level load** — `loadLevel` adds a static body per colliding placement, gated on `aver_phys_ready()`. Consider dropping `groundBody_`: a real game's level supplies its own collision. (Each colliding placement collides with its imported mesh's own TRIANGLES when one is available — `GameContent::collisionMeshFor` lazily reads the mesh's `.ocmesh`, picks the coarsest stored LOD whose geometric error is within `kCollisionMaxErrorCm` (2 cm), and hands it to `aver::world::addStaticMeshBody`; this is what makes NewSponza's per-material-merged walls and arches, which are concave, walkable rather than one solid box each. A mesh with no cached collision mesh — every built-in, or one whose `.ocmesh` failed to load — falls back to a box fitted to the placement's mesh's local bounds under its world transform — centre offset, scale, rotation — via `aver::world::fitStaticBox`/`addStaticBoxBody`, not the placement's scale used directly as a half-extent.)
- Tick groups, gated on `aver_fw_play_state() == AVER_FW_PLAY_PLAYING` (the editor's `spawnTestClass_` term of the gate is **dropped** — a `--spawn-test` CLI harness widening): PRE_PHYSICS, `aver_phys_step(dt)`, PHYSICS, POST_PHYSICS. **`aver_phys_step` sits between PRE_PHYSICS and PHYSICS**, not between PHYSICS and POST_PHYSICS — the comment describes the tick groups, not where the step lands. Copy the code order (now `tickGameplayGroups` in `Runtime/include/aver/game/GameTick.hpp`).
- Anim/skin/flush, in **code order**: `anim::animSystem().tick(World::instance(), dt)` → `skinnedScene_->update(…)` → `World::instance().flush()`. `SkinnedScene.hpp` claims `update` runs *after* `World::flush`; the code calls it **before** (`GameApp.cpp` still carries a comment noting the disagreement). Copy the code, not the comment. Not play-gated, deliberately.
- `SkinnedScene` is registered **first** of all render features: `init(*e.device())`, then `setResolvers(&GameContent::resolveAnimAsset, &GameContent::resolveSceneMesh, &content_)`, then `addRenderFeature`. Null is a legal state.
- `drivePlayCamera()` returns unless `PLAYING`, reads the pawn via `aver_fw_controlled_pawn(aver_fw_player_controller(0))`, prefers `aver_fw_view_entity()`, and writes **only** `camPos_`, `yaw_`, `pitch_`. It must run **after** `flush` and **before** the view matrix.
- `playSessionActive()`: `PAUSED` counts as active there, while the tick block and `drivePlayCamera` both require exactly `PLAYING`.
- Dropped: the `playTest_*` and `spawnTest*` harness members and `maybePlayTest`/`maybeSpawnTestActor` entirely.

DONE-WHEN: with no play session `aver_phys_step` is never called (log the step count once per change and assert it stays 0 in editor state); the loaded level logs one static body per colliding placement, matching `grep -vc nocollide` over the `PLACE` lines. Anim still ticks with no session — the intended asymmetry.

**C10 — Input: `InputState` → `aver_fw_*` (the input rewrite lands here).** Lift the **mechanism** verbatim — `setMouseCaptured(bool)`, `warpToAnchor()`, `pollCapturedMouse()`, with non-Win32 stubs. It is pure Win32 (`ShowCursor`/`ClipCursor`/`SetCursorPos`/`GetCursorPos`) with no ImGui, and needs the HWND from `e.window()->nativeHandle()`. Discard the **policy** and the whole body of `pushInput` — see "Input". Members: `mouseCaptured_`, `captureAnchorX_/Y_`, `captureDx_/Dy_`, `releasedByUser_`. A game draws its HUD at the full backbuffer, so `aver_ui_begin_frame` gets `(0, 0, w, h)` (the editor's HUD-preview members are dropped). DONE-WHEN: the `--input-echo` flag logs, once per change, the set of `AVER_FW_KEY_*` currently held; holding W then Shift then releasing prints exactly `W`, `W+LSHIFT`, `W`, `(none)`. That proves the ImGui-free path end to end with no baseline. Also, with `AVER_ENABLE_UI=OFF` the game must still respond — an `AVER_WITH_IMGUI=0` build having no input at all is the exact defect `InputState.hpp`'s header was written about.

**C11 — Shutdown mirror and packaging.** Mirror `onShutdown` in **exact reverse registration order**, dropping every editor line:
```
removeRenderFeature(gameUi_); delete gameUi_;                       // raw OWNING pointer
removeRenderFeature(skinnedScene_.get()); skinnedScene_.reset();
if (voxiAttached_) { removeRenderFeature(&voxiRenderer_); voxiAttached_ = false; }
voxiRenderer_.shutdown();
releaseProjectMaterials(); textureFactory_ = nullptr;
aver_phys_shutdown(); groundBody_ = 0;
scripts_.shutdown();
```
Dropped: `mcp_.stop()`, `setLogSink(nullptr,nullptr)`, `editor::flushEditorPrefs()`, `editor::shutdownActorEditors()`, and the `#if AVER_WITH_IMGUI` icon-texture block, which sits immediately before `releaseProjectMaterials` and must not be scooped up with it. DONE-WHEN: `scripts/stage-game.ps1` then `scripts/verify-game.ps1` both pass, **and** the sabotage `verify-game.ps1`'s own docstring describes now fails: deleting a staged `Aver.*.dll` must fail the run (see trap 16 — resolved).

---

## The aspect ratio — dockspace rect vs swapchain

`viewAspect()` in the editor is `vpH_ > 0.5f ? vpW_ / vpH_ : 1.777f`. `vpX_/vpY_/vpW_/vpH_` (defaults `{0, 0, 1600, 900}`) are written in **exactly one place**: inside `buildUI` under `#if AVER_WITH_IMGUI`, after an early return when `!e.device()->uiActive()`, from the ImGui "Level" tab's `GetCursorScreenPos()`/`GetContentRegionAvail()`. `buildUI` runs from `onRender`, so the rect written on frame N is consumed by frame N+1's `onUpdate`. A game has no dockspace: those four floats would sit at their constructor defaults forever, and every window size other than 1600x900 would render at the wrong aspect with no error.

**The replacement, in `GameApp::onUpdate`, sampled every frame:** `window()->width()/height()` (physical pixels), falling back to the configured size, and `16/9` if the height is zero. Never call `setViewportRect(0,0,w,h)`: `D3D12Device` interprets `w/h == 0` as "full backbuffer", and the setter exists only for editor chrome outside the viewport rect.

- **The window size *is* the backbuffer size.** `Engine::frameStep` resizes the swapchain to `window_->width()/height()` at the top of every frame. Sampling per frame rather than once in `onInit` is what makes an alt-tab resize correct instead of stretched.
- **Never call `setViewportRect` at all.** `D3D12Device` initialises `vpX_ = vpY_ = vpW_ = vpH_ = 0` and documents `w/h == 0 means full backbuffer`; not calling the setter is the correct full-backbuffer request, and calling it with `(0,0,w,h)` would work too but adds a way to be wrong.

There is no other route: `rhi::IDevice` exposes no backbuffer size (`width()/height()` exist only on `ISwapchain`; `D3D12Device::width()/height()` are non-virtual and unreachable through `IDevice*`), and `Engine` exposes only `window()`, `device()`, `time()` and `requestExit()`.

Also re-decide the clear colour rather than inheriting it: the editor's `setClearColor(0.055f, 0.055f, 0.062f, 1)` is documented as editor chrome grey for the region **outside** the viewport rect. A game filling the whole backbuffer has no such region.

## Input — ImGui path vs `InputState`

`SandboxApp::pushInput` is `aver_fw_input_new_frame()` followed by a body **entirely inside `#if AVER_WITH_IMGUI`** that reads `ImGui::IsKeyDown` / `io.MouseDelta` and steals Ctrl+Space and Escape for the editor. **Only the `aver_fw_input_new_frame()` call survives the lift.** Everything else is rewritten against `aver::InputState`, already bound in `GameApp::onInit` and keyed by **Win32 virtual key code**, not ImGui key enums.

The replacement sets: `A`–`Z` (`0x41..0x5A`) and `0`–`9` (`0x30..0x39`) in two loops (legal because `AVER_FW_KEY_A..Z` are 0..25 and `0..9` are 26..35 contiguously, `framework_abi.h`); Space, LShift, LCtrl, LAlt, Enter, Escape, Tab and the four arrows; mouse left/right/middle via `input_.mouseHeld(0..2)`; and `aver_fw_input_set_mouse(dx, dy, wheel)`. **The CAPTURED mouse delta wins** (`mouseCaptured_ ? captureDx_ : input_.mouseDX()`), exactly as the editor does.

**Frame protocol.** `InputState.hpp` demands `newFrame()` *before* `pumpEvents()`. `Engine::run` pumps at the top of the loop and `GameApp` rolls the edges as the **last** statement of `onRender`, which satisfies the same constraint from the other side. `pushInput()` goes in `onUpdate`, before the tick groups, so it reads the edges this frame's pump produced. **Do not move `newFrame()` to the top of `onUpdate`** — the comment in `GameApp.cpp` explains what that costs (every single tap ignored, held keys fine).

**Mouse-capture policy.** The editor's policy is ImGui-only (`ImGui::IsMouseClicked`, `io.WantCaptureMouse`, Shift+F1, `inViewport(io.MousePos)`, `io.WantTextInput`). The game's replacement is two rules: capture on window focus / first click, release on `VK_ESCAPE`, latched in `releasedByUser_`. **When captured, use `captureDx_/captureDy_` and never `input_.mouseDX()`** — `warpToAnchor` re-centres the cursor every frame, and each warp generates its own `WM_MOUSEMOVE`, so `InputState`'s delta double-counts. That is precisely why the editor polls `GetCursorPos` instead, and it is the trap this whole path exists to avoid.

---

## No pixel moves in Sandbox (gate-oracle exposure)

**No commit in this plan can move a pixel in `Sandbox.exe`.** The reason is structural, not a judgement call: no commit edits `sandbox/**`, any source or header that `Sandbox` compiles or links, any CMake option, PUBLIC compile definition, or compile flag. Every new file was under the new library, which `Sandbox` did not link (its link line named `Aver.Runtime`, `Aver.Formats`, `Aver.UI`, `Aver.Render.*`, `Aver.Scene`, `Aver.Physics`, `Aver.Framework`, `Aver.Scripting.Host` — never `Aver.Runtime.Game`). C1 added link deps only to `Aver.Runtime.Game`'s own interface. Two things would break that guarantee, and the plan deliberately avoided both:

1. **Promoting a file-static helper into `Aver.Core`.** `quatFromEulerDeg`, `eulerDegFromQuat`, `xformPoint`, `appendBox` and `appendSphere` existed **only** in `SandboxApp.cpp` — a repo-wide search found no other definition. Moving any of them into `Aver.Core` recompiles `SandboxApp.cpp` against different code, and floating-point codegen at a different inlining boundary is exactly the class of change the Release baseline exists to catch. **Copy them into `Runtime/src/GameMath.hpp`** rather than promoting them. Sharing them is a de-dup-slice decision, gated on the gates.
2. **Edits outside the runtime library and `game/`.** The only exception planned was a logging line in `modules/render.pbr/src/MaterialSystem.cpp` (trap 4). If it is not split into its own commit, C8 is the one commit that warrants a gate run.

Everything else needed no gate run. When running `./scripts/gates.ps1`, note that `-Exe` defaults to `build/bin/Sandbox.exe` and `-Release` switches **both** the executable and the baseline together — the game was never driven by the oracle in this slice.

---

## Traps

1. **`--probe-rel` is a fraction of the *editor viewport rect*, not the window.** `captureCheck` reads `vpX_..vpH_` and latches them as `capVpX_..capVpH_`. If a later slice adds a gate probe to the game, the same fraction lands on a *different* pixel because the game's rect is the whole backbuffer. `--probe-rel` survives a resize but not a change of aspect; screen every probe for a flat 7x7 neighbourhood.

2. **Both single-module configurations failed to compile at the editor baseline (2026-08-02).** PBR=ON/SCENE=OFF and PBR=OFF/SCENE=ON both broke in `SandboxApp.cpp`; all were non-template member functions, compiled whether called or not. The lift inherits this and does not create it; the re-guard table fixes it **at the destination only**. Do not use `-DAVER_MODULE_SCENE=OFF` (or the PBR equivalent) as a check on a lift commit against the editor — it fails in the editor for reasons that predate the work.

3. **`textureFactory_` has exactly one assignment, and in the editor it is three levels of `#if` deep**: inside `#if AVER_MODULE_VOXI` inside `#if AVER_MODULE_SCRIPTING`, behind `voxiRenderer_.init()` succeeding. Nothing enforces VOXI→SCRIPTING in CMake (only VOXI→PBR, root `CMakeLists.txt`), so a `-DAVER_MODULE_SCRIPTING=OFF` tree compiles out Voxi attachment and *all* material texturing in the editor. Do not reproduce that nesting (or the dependency) in `GameApp`; wire independently.

4. **A missing texture resolver is completely silent.** `MaterialSystem` returns 0 when `resolve_` is null (`resolveTexture`, `modules/render.pbr/src/MaterialSystem.cpp`) and, at the time of the lift, cached that 0 permanently; there was no diagnostic anywhere. The world still draws — the fallback identities (white / flat-normal / metal-rough / black) are bound — but authored-material entities render **white and fully metallic**, because the draw walk forces `col = white, metallic = roughness = 1.0`. That symptom reads like a shading regression and is a wiring bug. Add logging at the silent-fallback site.

5. **`releaseProjectMeshes` leaked in the editor's original** — it erased ids from `sceneMeshes_` and cleared `projectMeshIds_` but called no `destroyMesh` and never cleared `meshBounds_`, so stale bounds survived a project switch and GPU meshes leaked. **Fixed in the shared library:** `GameContent::releaseProjectMeshes` now destroys the meshes and erases `meshBounds_`. The lesson stands for any future verbatim lift: a game that reloads projects notices before the editor does.

6. **`materialForSurface` caches negatives and does not fall through.** It returns a cached `0` without retrying, and on the first *existing* candidate that fails to parse it `break`s rather than trying the next path (deliberately: a corrupt built material must not fall through to a stale hand-authored one). Fix a broken `.ocmat` and the game keeps returning 0 until the project is re-adopted.

7. **The content-index walk indexes every regular file**, with no `AssetType` filter — `.cs`, `.csproj`, `.pdb`, `bin/obj` output, everything. The key is the **forward-slash content-relative** spelling with no `Content/` prefix (`fnv1a64("Meshes/sphere.ocmesh")`), FROZEN to match C# `Assets.ObjectIdOf`; the value is the **native absolute path with backslashes**. Get either spelling wrong and every id silently misses.

8. **Two independent installations of the same resolver pair.** `anim::animSystem().setResolver(...)` and `SkinnedScene::setResolvers(...)` are separate. Install only one and skinned entities draw at rest with no error message. Worse: `AnimSystem::user_` is a raw owner pointer in a process-global function-local static, `clear()` does not reset it, and it is dereferenced in `AnimSystem.cpp`. Last-writer-wins is the existing contract — harmless while the two hosts are separate processes (`Sandbox.exe`, `AverEngineRuntime.exe`), a dangling pointer the day one process hosts both.

9. **`scene::World::instance()` is a process-global singleton** (`modules/scene/include/aver/scene/World.hpp`). Same caveat as above, same deadline.

10. **`SkinnedScene.hpp` is wrong about ordering.** It says `update` is called "AFTER AnimSystem::tick and World::flush". The code calls it **before** `flush` (still true in `GameApp.cpp`). Copy the code order. Similarly the tick-group comment describes the groups, not where `aver_phys_step` lands.

11. **`World::worldMatrix` is non-const**, so the draw walk cannot take a `const scene::World&`. Discovering this at the end of the render lift means rewriting the signature.

12. **`xformPoint` exists twice with reversed arguments.** `SandboxApp.cpp`'s is `xformPoint(const Mat4&, const Vec3&)`; `modules/render.voxi/src/VoxiRenderer.cpp`'s is `xformPoint(const Vec3&, const Mat4&)`. Both are file-static, so they do not collide today — but a "helpful" promotion to a shared header would silently pick one and transpose the other's culling.

13. **`unitCube` half-extent 1 is FROZEN** — `.ocworld` `PLACEG` scales are half-extents in cm applied directly to it. Registering a differently-sized built-in cube scales the entire level. This is now the general rule, not a cube-specific shortcut: a colliding placement's static box is fitted to its mesh's local bounds under the placement's world transform (centre offset, scale, rotation), and the cube's frozen `[-1,1]` cm bounds are just what that rule reduces to for the cube.

14. **`frameCameraOn` also places the GI volume.** It writes `giCenter_`/`giExtent_` as a side effect of framing the camera, consumed at frame render. A game that frames its camera differently must still set these or GI covers the wrong region.

15. **`saveLevel` forces `p.collide = true` on every placement**, so a `nocollide` flag does not round-trip. Not lifted here, but do not "fix" the load path to compensate for it.

16. **`verify-game.ps1` could not fail on a missing engine DLL** when the lift started, because the first game executable's import table named no `Aver.*.dll` — it linked only static libraries — so deleting a staged DLL could not fail the isolation run. **Resolved 2026-08-02 by C1–C11:** deleting `Aver.Scene.dll` now fails with exit `-1073741515` (`0xC0000135`, `STATUS_DLL_NOT_FOUND`), because the game finally uses what it ships. `verify-game.ps1`'s header keeps that paragraph deliberately: the next person to add a staged DLL should ask the same question about it.
