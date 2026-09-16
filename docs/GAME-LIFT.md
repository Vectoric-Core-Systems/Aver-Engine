# Lifting the game half out of SandboxApp

> **WHERE IT LIVES NOW (2026-09-16):** the library and the restored standalone host moved to `Runtime/`;
> the executable is `AverEngineRuntime.exe` ("Aver Engine Runtime"). Paths below are as they were.
>
> **STATUS: HISTORY, 2026-08-17. THIS DOCUMENT DESCRIBES COMPLETED WORK THAT HAS BEEN REMOVED.**
> The plan was executed in commits C1–C11 and lifted the game rendering half from `SandboxApp.cpp`
> to `modules/runtime.game`, creating `AverGame.exe`. That executable and the staging scripts have
> since been deleted in commit b262c73 ("The packaged game is gone"), leaving only the library.
> The file paths in sections C1–C11 below no longer exist as an executable; the design reasoning
> — the re-guarding strategy, the separation of concerns, the framework integration — informed the
> library that survives and is tested by `tests/game/InputBridgeTest`.
>
> **What replaced it: nothing, deliberately.** A second host that rendered a different subset of
> the scene than the editor was a standing source of confusion; the editor is how a project is run.
> The library `modules/runtime.game` is still built (controlled by `AVER_BUILD_GAME`) and driven by
> tests, which is what the plan's architecture enabled.
>
> Produced 2026-08-02 by five parallel readers over `SandboxApp.cpp:1,2000` plus a refutation pass:
> 57 risks raised, **2 survived**. Every line number was read, not assumed. Restored here because
> the file describes a completed slice, its design still lives in the runtime library, and the
> traps section (below) documents engine-wide patterns still relevant to any future lift.

---

# Lift Plan — SandboxApp game half → Aver.Runtime.Game

**Scope of the slice.** `AverGame.exe` opens a project, indexes content, registers meshes, loads the start map and draws the world. This is a **copy**, not a move: `sandbox/src/SandboxApp.cpp` is not edited by any commit below. De-duplication is a later slice, proven by the gates being bit-identical.

**Verified starting state.** `modules/runtime.game/src/GameApp.cpp` is 106 lines: `parseArgs` (:34-49), `config()` (:53-64), `onInit` binding `input_` to the window event stream (:66-78), an empty `onUpdate` (:80-83), an `onRender` that only calls `input_.newFrame()` (:85-100), and a frame-count log in `onShutdown` (:102-104). `GameApp` has exactly three members: `GameConfig cfg_; InputState input_; u64 frames_;` (`GameApp.hpp:49-51`). `GameConfig` has no project field (`GameApp.hpp:11-20`).

---

## The guard decision, made once and up front

The bidirectional PBR/SCENE cross-contamination in `SandboxApp.cpp` is real and already broken at HEAD. A verbatim lift inherits it. **The destination re-guards instead**, and this is the rule every commit below follows:

| Symbol | Guard at the destination | Was |
|---|---|---|
| `contentIndex_`, `rebuildContentIndex` (walk only), `resolveAnimAsset`, `projectPath`/`ProjectDesc` | **unguarded** | PBR |
| `rebuildContentIndex`'s `anim::animSystem().clear()/setResolver()` tail | `#if AVER_MODULE_SCENE` | PBR |
| `sceneMeshes_`, `meshBounds_`, `projectMeshIds_`, `resolveSceneMesh`, `loadProjectMeshes`, `releaseProjectMeshes`, `surfaceLooks_`, level load, draw walk | `#if AVER_MODULE_SCENE` | mixed PBR/SCENE |
| `textureFactory_`, `materialAssets_`, `resolveAssetPath`, `resolveMaterialTexture`, `materialForSurface`, `releaseProjectMaterials` | `#if AVER_MODULE_PBR` | PBR |
| `surfaceMaterials_`, `loadProjectMaterials` (they key on `aver_scene_material`) | `#if AVER_MODULE_PBR && AVER_MODULE_SCENE` | PBR |

`contentIndex_` becoming unguarded is what kills the cross-contamination at the root: it is a `std::unordered_map<u64, std::string>` and `fnv1a64` lives in `Aver.Core` (`modules/core/include/aver/core/Hash.hpp:9-11`), so nothing about it needs either module.

`docs/PACKAGING.md:64` claims the relocation "fixes a bug". It does not. Only the re-guarding above fixes it, and `PACKAGING.md:64`'s line numbers (1130/1345) are two low against HEAD (real: `1132`/`1347`) because the doc was written against `ae47a2f`, not the `5714ba3` its header claims.

## Destination file layout

```
modules/runtime.game/include/aver/game/GameApp.hpp       edited by most commits
modules/runtime.game/src/GameApp.cpp                     edited by most commits
modules/runtime.game/src/GameMath.hpp        C5  quatFromEulerDeg / xformPoint / appendBox / appendSphere copies
modules/runtime.game/include/aver/game/GameContent.hpp   C3
modules/runtime.game/src/GameContent.cpp                 C3
modules/runtime.game/include/aver/game/GameLevel.hpp     C5
modules/runtime.game/src/GameLevel.cpp                   C5
modules/runtime.game/src/GameRender.cpp                  C7
modules/runtime.game/src/GameInput.cpp                   C10
```

`GameContent` is a plain class owned by `GameApp` and passed by reference to the level and render code. Do **not** reproduce the SandboxApp god-object: the static resolvers take `void* user = &content_`, not `&app`.

---

## C1 — CMake: give Aver.Runtime.Game its link interface

**Files:** `modules/runtime.game/CMakeLists.txt`, `game/CMakeLists.txt`, `modules/runtime.game/src/GameApp.cpp` (one log line).

Today `modules/runtime.game/CMakeLists.txt:12-19` declares `DEPS Aver.Runtime Aver.Platform Aver.Core`. `AVER_MODULE_PBR=1` is PUBLIC on `Aver.Render.PBR` (`modules/render.pbr/CMakeLists.txt:19`), `AVER_MODULE_SCENE=1` PUBLIC on `Aver.Scene` (`modules/scene/CMakeLists.txt:26`), `AVER_MODULE_VOXI=1` PUBLIC on `Aver.Render.Voxi` (`modules/render.voxi/CMakeLists.txt:16`). None reach GameApp's TU, so every `#if` in the lift is currently false.

Add, mirroring `sandbox/CMakeLists.txt:17,62,66,69-73,75-78,81-84`:

- unconditional: `Aver.Formats` (OcProject/OcMesh/OcWorld; carries `Aver.Assets` transitively per `modules/formats/CMakeLists.txt:20`), `Aver.Render.Skin`
- `if(TARGET Aver.Render.PBR)`: `Aver.Render.PBR Aver.Render.PBR.Materials Aver.Formats.Material Aver.Assets.Gpu`
- `if(TARGET Aver.Render.Voxi)`: `Aver.Render.Voxi Aver.Render.Voxi.Renderer`
- `if(TARGET Aver.Scene)`: `Aver.Scene`
- `if(TARGET Aver.Anim.Scene)`: `Aver.Anim.Scene`
- `if(TARGET Aver.Physics)`: `Aver.Physics`
- `if(TARGET Aver.Framework)`: `Aver.Framework`

The `if(TARGET …)` guards are mandatory: an unconditional `Aver.Scene` is a configure error in an `AVER_MODULE_SCENE=OFF` tree. `Aver.RHI` is **not** needed — it is already PUBLIC through `Aver.Runtime`. Subdirectory order already permits all of these (`runtime.game` is added last, at root `CMakeLists.txt:193-195`).

Add to `GameApp::onInit`, immediately after the existing `[Game] ready` at `GameApp.cpp:77`:

```cpp
AVER_INFO("[Game] modules: PBR={} SCENE={} VOXI={} PHYSICS={} FRAMEWORK={} SCRIPTING={}",
          AVER_MODULE_PBR_ON, AVER_MODULE_SCENE_ON, ...);   // small #if-defined helpers
```

**Members moved:** none.

**DONE-WHEN:** `cmake --build build --target AverGame` succeeds, and `build/bin/AverGame.exe --headless --frames 1` prints `[Game] modules: PBR=1 SCENE=1 VOXI=1 PHYSICS=1 FRAMEWORK=1 SCRIPTING=1`. No baseline: the line is its own oracle, and a `0` in it means the link interface is still missing.

**Pixel risk: none.** `Sandbox` links none of these targets *through* `Aver.Runtime.Game`; its own link line is unchanged and its TU sees the same macros it saw before.

---

## C2 — Project open without ProjectBrowser

**Files:** `GameApp.hpp`, `GameApp.cpp`.

`SandboxApp.cpp:1796` is `project_ = browser_.project();` — a `fmt::ProjectDesc` copy, not an ImGui type. `ProjectBrowser::open` (`sandbox/src/ProjectBrowser.cpp:78-90`) is `fmt::loadOcproject` plus recent-list bookkeeping; only line 80 is game-relevant. The game calls `fmt::loadOcproject` directly (`modules/formats/include/aver/formats/OcProject.hpp:55`), which sets `out.manifestPath` and `out.dir` before parsing and rejects a foreign or too-new `ENGINE` line (`modules/formats/src/OcProject.cpp:107-117`).

- `GameConfig` gains `std::string projectPath;` after `GameApp.hpp:19`.
- `parseArgs` (`GameApp.cpp:36-47`) gains a bare-argv branch: any argument whose extension is `.ocproject` case-insensitively becomes `c.projectPath` — copy the test from `SandboxApp.cpp:5562-5573` (`isOcproject`), plus an explicit `--project <path>`.
- `GameApp` gains `fmt::ProjectDesc project_;` (`SandboxApp.cpp:5025`).
- New `void GameApp::openProject(Engine& e)` replacing `applyProject` (`SandboxApp.cpp:1795-1830`) with lines **1797, 1799, 1801, 1802-1807, 1808-1809 dropped** (actor/anim editor content roots, content watcher, upgrade offer, editor window title). Body is a stub for now: load, log, return.

**Members becoming GameApp members:**
| SandboxApp | Type | Destination |
|---|---|---|
| `project_` (:5025) | `aver::fmt::ProjectDesc` | `GameApp::project_` |
| `projectPath_` (:5026) | `std::string` | `GameConfig::projectPath` |
| `browser_` (:5024) | `editor::ProjectBrowser` | **dropped** |

**DONE-WHEN:** `AverGame.exe --headless --frames 1 <any>.ocproject` logs name, `contentRoot`, `contentDir()`, `startMap`; a path that does not exist logs the `loadOcproject` error string and the process still exits 0. Both are self-evidencing.

**Pixel risk: none** — no editor file touched.

---

## C3 — Content index and the asset resolvers

**Files created:** `GameContent.hpp`, `GameContent.cpp`. **Edited:** `GameApp.hpp/.cpp`.

Copy from the `#if AVER_MODULE_PBR` block at `SandboxApp.cpp:1132-1347`:

| Function | Source lines | Destination guard |
|---|---|---|
| `rebuildContentIndex()` | 1189-1210 | unguarded; the `animSystem()` tail (1208-1209) under `#if AVER_MODULE_SCENE` |
| `static std::string resolveAnimAsset(u64 id, void* user)` | 1212-1219 | unguarded |
| `std::string resolveAssetPath(const pbr::TextureRef&) const` | 1168-1187 | `#if AVER_MODULE_PBR` |

`rebuildContentIndex` walks `project_.contentDir()` with the `error_code` overload of `recursive_directory_iterator::increment`, computes `std::filesystem::relative`, rewrites `'\\'` → `'/'` (line 1202, marked FROZEN at 1201 to match C# `Assets.ObjectIdOf`), and stores `contentIndex_[fnv1a64(rel)] = it->path().string()` — key is the forward-slash relative spelling, value is the native absolute path. **Keep the `anim::animSystem().clear()`**: it is required on project adoption, because AnimSystem caches by id and a moved asset would otherwise keep resolving to its old path. It is safe because, exactly as in the editor, `rebuildContentIndex` will be called only from the project-adoption path — never per frame.

**Members:**
```cpp
class GameContent {
    fmt::ProjectDesc project_;                              // ref/copy of GameApp::project_
    std::unordered_map<u64, std::string> contentIndex_;     // was SandboxApp.cpp:5107 (PBR) -> UNGUARDED
};
```

**DONE-WHEN:** run `AverGame.exe --headless --frames 1 <proj>.ocproject` and the existing editor for the same project; the `[Content] indexed {} asset(s) under {}` line (source `:1205`) prints the same count and the same root in both. Diffing two live runs needs no stored baseline. The hash spelling is already pinned by `tests/formats/src/FormatTest.cpp:87-88` (`fnv1a64("Meshes/sphere.ocmesh") == 672114764054563281`); no new test is required.

**Pixel risk: none.**

---

## C4 — Mesh registry: built-ins and `loadProjectMeshes`

**Files:** `GameContent.hpp/.cpp`, new `GameMath.hpp` (for `appendBox`/`appendSphere`), `GameApp.cpp`.

| Function | Source lines | Note |
|---|---|---|
| `loadProjectMeshes(Engine& e)` | 1262-1309 | body already `#if AVER_MODULE_SCENE` at 1264; at the destination the **whole definition** goes under SCENE |
| `releaseProjectMeshes()` | 1311-1315 | at the destination goes under SCENE (in SandboxApp its body is unguarded — one of the seven compile bugs) |
| `static rhi::MeshHandle resolveSceneMesh(u64, void*)` | 1221-1229 | moves from PBR to SCENE |
| built-in primitives | 592-602 | `Meshes/sphere.ocmesh` = `appendSphere(sv,si,1.0f,24,48)`; `Meshes/cube.ocmesh` = `appendBox(uv_,ui_,0,0,0,1.0f)` — half-extent 1 is FROZEN (comment at :585) because `.ocworld` `PLACEG` scales are half-extents in cm applied to it |
| `surfaceLooks_` seeding | 604-616 | the seven `M_*` tokens with their exact colour/metallic/roughness |
| `appendBox`, `appendSphere` | 125-137, 145-… | file-static in `SandboxApp.cpp`; **copy**, do not promote (see Traps) |

`loadProjectMeshes` filters on `assetTypeFromPath(full) == AssetType::Mesh`, repacks `fmt::OcMeshData` into `rhi::MeshVertex` (`{f32 px,py,pz,nx,ny,nz,u,v}`, `modules/rhi/include/aver/rhi/RHI.hpp:43-51`) copying **only** position/normal/uv — joints and weights are dropped, so a skinned asset arrives here as static geometry — then `e.device()->createMesh(...)` and writes `sceneMeshes_[id]`, `meshBounds_[id]`, `projectMeshIds_.push_back(id)`. In `GameContent` it takes `rhi::IDevice&` instead of `Engine&`: `Engine` is used at `:1292-1293` for nothing but `e.device()->createMesh`.

**Members added to `GameContent` (all `#if AVER_MODULE_SCENE`):**
```cpp
std::unordered_map<u64, rhi::MeshHandle>                 sceneMeshes_;    // SandboxApp.cpp:5549
std::unordered_map<u64, std::pair<Vec3, Vec3>>           meshBounds_;     // SandboxApp.cpp:5554
std::vector<u64>                                         projectMeshIds_; // SandboxApp.cpp:4932
struct SurfaceLook { f32 col[3]; f32 metallic; f32 roughness; };          // SandboxApp.cpp:5102
std::unordered_map<i32, SurfaceLook>                     surfaceLooks_;   // SandboxApp.cpp:5103
```

**DONE-WHEN:** the `[Mesh]` summary line from `:1301`-ish prints the same registered/failed counts as the editor for the same project, and `sceneMeshes_.size()` equals that count + 2 built-ins. Both self-checking against a second live run.

**Pixel risk: none.**

---

## C5 — Level load

**Files created:** `GameLevel.hpp`, `GameLevel.cpp`, `GameMath.hpp` gains `quatFromEulerDeg`. **Edited:** `GameApp.cpp`.

| Function | Source lines |
|---|---|
| `loadLevel(const std::string&)` | 5115-5174, **minus** 5144 (`entityLabels_`/`makeEntityLabel`), 5168-5169 (`sel_`/`selEntity_`) |
| `applyLevelSky(const fmt::OcWorldData&)` | 5176-5206 |
| `loadStartMap()` | 5237-5249 |
| `unloadLevel()` | 5251-5273, **minus** 5256-5264 (labels, undo/redo, `editToEntity_`, `entityToEdit_`, selection) |
| `quatFromEulerDeg(const Vec3&)` | 168-171 — file-static, exists nowhere else in the tree |

`loadLevel` calls `unloadLevel()` first, then `fmt::loadOcworld` (one parser for `.ocworld` and `.ocmap`, `modules/formats/src/OcWorld.cpp:149-156`), and per placement builds a `Transform` (position in cm, `quatFromEulerDeg({roll,pitch,yaw})`, scale `sx/sy/sz`), `world.create(p.asset, scene::kInvalidEntity, xf)` — the entity's `CName` **is** the asset path — then `addComponent(e, scene::kComponentMeshRenderer)` and writes exactly three fields: `mr->mesh = p.objectId`, `mr->material = p.material.empty() ? 0 : aver_scene_material(0, p.material.c_str())`, `mr->flags |= scene::kMeshRendererVisible` (5133-5135). It writes **no bounds**. The `#if AVER_MODULE_PBR` `materialForSurface` call at 5136-5141 is deferred to C8. The `#if AVER_MODULE_PHYSICS` static-box branch at 5147-5152 is lifted **verbatim** and is a no-op until C9 calls `aver_phys_init()` — the branch already tests `aver_phys_ready()`.

`loadStartMap` returns unless `project_.valid() && !project_.startMap.empty()`; a missing start map is INFO, not an error (5241-5246).

Add `scene::World::instance().flush()` to `GameApp::onUpdate` (from `SandboxApp.cpp:1070`). Without it, `World::worldMatrix(ent)` in C7 reads stale/uncomposed matrices — `flush` retires deferred destroys, rebuilds topological order and recomposes stale world matrices (`modules/scene/src/World.cpp:789-802`).

Wire into `openProject` in the load-bearing order from `applyProject`: `rebuildContentIndex()` → `loadProjectMeshes()` → `loadStartMap()`. The index must precede the meshes (resolvers read `contentIndex_`) and the meshes must precede the level (`mr->mesh` is resolved through `sceneMeshes_`).

**Members added to `GameLevel` (all `#if AVER_MODULE_SCENE`):**
```cpp
std::vector<scene::Entity> levelEntities_;   // SandboxApp.cpp:5320
std::string                levelPath_, levelName_;  // SandboxApp.cpp:5321
bool                       hasLevelFog_ = false;    // SandboxApp.cpp:5324
f32                        levelFog_ = 0.0002f;     // SandboxApp.cpp:5325
std::vector<int32_t>       levelBodies_;    // SandboxApp.cpp:5327  (#if AVER_MODULE_PHYSICS)
```
**Dropped:** `entityLabels_` (:4934), `labelCounts_` (:4935), `entityBodies_` (:4936), `undoStack_`/`redoStack_`/`editToEntity_`/`entityToEdit_`/`editBeforeValid_` (:4939-4944), `sel_`/`selEntity_`, `hasLevelSun_`/`hasLevelSky_` (:5322-5323 — their only readers are `saveLevel` and `unloadLevel`), and the whole of `saveLevel` (5275-5318).

**DONE-WHEN:** `[Level]` logs an entity count equal to `grep -c '^\s*PLACE' <startmap>` on the same file; a second run of `unloadLevel` then `loadStartMap` returns to the same count (no leak, no double-spawn). No baseline.

**Pixel risk: none.**

---

## C6 — Camera and the per-frame device push (**the aspect fix lands here**)

**Files:** `GameApp.hpp/.cpp`, `GameLevel.cpp` (`frameCameraOn`).

| Function | Source lines | Change |
|---|---|---|
| `camForward()` | 2197-2199 | verbatim |
| view/proj/`setCamera` block | 1100-1108 | verbatim except the aspect source |
| sky/fog/post push | 1110-1129 | verbatim except the clear colour (see below) |
| `frameCameraOn(const fmt::OcWorldData&)` | 5208-5235 | keep `camPos_`/`yaw_`/`pitch_` (5225-5228) and `giCenter_`/`giExtent_` (5231-5234); drop `flySpeed_` (5229) |
| `viewAspect()` | 2194 | **cannot be lifted — see below** |
| `setViewportRect(vpX_,vpY_,vpW_,vpH_)` | 1098 | **not lifted at all — see below** |

**Members:**
```cpp
Vec3 camPos_{7.0f, 7.0f, 4.5f};             // SandboxApp.cpp:4962
f32  yaw_ = 0.0f, pitch_ = 0.0f;            // SandboxApp.cpp:4963
Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};   // SandboxApp.cpp:5558
rhi::SkyAtmosphere sky_;                    // SandboxApp.cpp:4973
f32  sunColor_[3]{1.0f,0.96f,0.9f}, sunAmbient_ = 1.0f;   // :4967
f32  skyZenith_[3], skyHorizon_[3];         // :4968
f32  fogColor_[3]{1,1,1}, fogDensity_ = 4e-6f;            // :4970
rhi::PostSettings post_;                    // :4971
f32  cloudTime_ = 0.0f;                     // :4974
Vec3 giCenter_{0,0,300}; f32 giExtent_ = 1200.0f;         // :5056 (#if AVER_MODULE_VOXI)
```
**Dropped:** `vpX_/vpY_/vpW_/vpH_` (:4997), `flySpeed_`/`lookSpeed_` (:4963), `levelHovered_`/`levelFocused_`/`browserActive_` (:5089-5090).

**DONE-WHEN:** run `AverGame.exe --frames 3 --width 800 --height 600` and again at `--width 1600 --height 900`; a new one-shot log line prints `aspect=1.333` and `aspect=1.778` respectively, and the values track a live window resize. This check is impossible to pass with the dockspace formula and needs no baseline.

**Pixel risk: none** (nothing the editor compiles or links changes).

---

## C7 — The world draw walk — **first pixels in AverGame**

**Files created:** `GameRender.cpp`. **Edited:** `GameApp.cpp`, `GameMath.hpp` (`xformPoint`).

Copy `SandboxApp.cpp:1386-1510`, the whole `#if AVER_MODULE_SCENE` block, minus the editor lines listed below:

- frustum plane derivation, 1396-1407 — row-vector, **unnormalised**: `pl[0][i]=m[i][3]+m[i][0]` (left), `[1]` right, `[2]` bottom, `[3]` top, `pl[4][i]=m[i][2]` (near, `[0,1]` depth), `[5]` far. Derived per frame from `viewProj_`.
- entity walk 1409-1417: `w.at(i)`, skip `w.destroyPending(ent)`, fetch `w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer)`, skip on null / `!(mr->flags & kMeshRendererVisible)` / `mr->mesh == 0` / `sceneMeshes_.find` miss, then `const Mat4& wm = w.worldMatrix(ent)`. **`World::worldMatrix` is non-const (`World.hpp:149`)**, so the walk needs a non-const `scene::World&`.
- bounds substitution 1419-1430: for non-skinned entities only, `const_cast` and overwrite `mr->aabbMin/aabbMax` from `meshBounds_` every frame.
- positive-vertex frustum cull 1432-1460, using `xformPoint` (`SandboxApp.cpp:190-195`) on all 8 local corners. A degenerate box (`hi <= lo` on any axis) skips culling and is drawn.
- colour selection 1461-1481; in this commit keep only the `surfaceLooks_` fallback at 1473-1476 and the `{0.80,0.80,0.85,1.0}` default at 1462 — the `surfaceMaterials_`/`MaterialSystem` half arrives in C8.
- skinned substitution 1491-1497 and `e.device()->drawMesh(mesh, &wm.m[0][0], col, metallic, roughness)` at 1498.
- counters 1503-1508 with `lastSceneDrawn_`/`lastSceneCulled_` starting at `-1`.

**Dropped:** 1383 and 1499-1500 (selection latch), 1482-1489 (skin-scene-test recolour), 1370-1384 (`objects_` placeholder pass), 1511-1523 (selection outline), 1526-1530 (grid, gizmo), 1531-1532 (`buildUI`, `uiReg_.endFrame`), 1536-1555 (all five test harnesses including `captureCheck`, the gate probe).

**Members:** `int lastSceneDrawn_ = -1, lastSceneCulled_ = -1;` (`SandboxApp.cpp:5555-5556`), `bool wireframe_ = false;` (:4984) if `setWireframe` is kept.

**DONE-WHEN:** `[Game] scene-render: N drawn, M frustum-culled` with `N > 0` and `N + M ==` the entity count C5 logged; and a `--frames 120` run visibly shows the start map. Turning the camera by 180° must move entities from `drawn` to `culled` without changing `N + M` — a cull check that needs no reference image.

**Pixel risk: none for the editor.** This is the first commit where `AverGame` renders anything, and `AverGame` is not what `scripts/gates.ps1` drives (`-Exe` defaults to `build/bin/Sandbox.exe`).

---

## C8 — Materials: the PBR half and the Voxi attach

**Files:** `GameContent.hpp/.cpp`, `GameApp.cpp`, `GameRender.cpp`, `GameLevel.cpp`.

| Function | Source lines | Guard |
|---|---|---|
| `static rhi::TextureHandle resolveMaterialTexture(const pbr::TextureRef&, pbr::TextureSlot, void*)` | 1133-1166 | PBR |
| `materialForSurface(const std::string&)` | 1231-1260 | PBR |
| `loadProjectMaterials()` | 1317-1339 | PBR && SCENE |
| `releaseProjectMaterials()` | 1341-1346 | PBR |
| Voxi attach + material wiring | 836-886 | VOXI |
| `applyProjectRenderSettings()` | 1852-1868 | VOXI |

`resolveMaterialTexture` picks `assets::TextureUsage` **from the slot, never the filename** (1146-1153: BaseColor/Emissive → Colour, Normal → NormalMap, else Data) and calls `assets::uploadTexture` (`modules/assets/include/aver/assets/TextureUpload.hpp:27-29`). `materialForSurface` tries three candidates **in this order** (1241-1245): `binariesDir()\Materials\<n>.ocmat`, `contentDir()\Materials\<n>.ocmat`, `contentDir()\<n>` (no extension appended on the third); the **first existing** candidate wins and a parse failure **breaks** rather than falling through. It caches `0` as a negative and never retries. `loadProjectMaterials` uses a **non-recursive** `directory_iterator` over `Content\Materials` only.

> **CORRECTED 2026-09-13.** The runtime's lifted `loadProjectMaterials()` was never called and has been
> **removed**. It was not a dropped step: `GameApp` opens a project with `loadProjectMeshes` ->
> `loadProjectParticleEffects` -> `loadStartMap`, and `GameLevel::load` resolves each surface a level
> references through `materialForSurface`, lazily and Binaries-first, so eagerly loading every project
> material would only add load time and memory. The `openProject` order quoted below is the **editor's**
> `applyProject`, not the runtime's. The editor keeps its own `loadProjectMaterials()`, which since
> `6875e36b` scans `Binaries\Materials` as well as `Content\Materials`.

`pbr::MaterialSystem` is a member of `VoxiRenderer` (`modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp:83`), initialised only in `VoxiRenderer::init` (`modules/render.voxi/src/VoxiRenderer.cpp:129`), so textured drawing requires attaching a `VoxiRenderer` as a render feature. Copy `SandboxApp.cpp:836-886`: `rhi::DeviceCaps` → `voxi::DeviceInfo` (841-846), `voxi::Renderer::get().setDeviceInfo/setSettings`, `voxiRenderer_.setSettings(s)` **before** `voxiRenderer_.init(*e.device())`, then `addRenderFeature`, `voxiAttached_ = true`, deferred `applyProjectRenderSettings()`, `textureFactory_ = e.device()->resources()` (:882) and `voxiRenderer_.materials().setTextureResolver(&GameContent::resolveMaterialTexture, &content_)` (:883). **Keep 882 and 883 adjacent**, exactly as SandboxApp does — that adjacency is why the `!textureFactory_` guard at :1137 is unreachable.

Restore the deferred pieces: `materialForSurface` at level load (`SandboxApp.cpp:5136-5141`) and the `surfaceMaterials_` branch of the draw walk (`:1466-1481`, including `col = white, metallic = roughness = 1.0` at 1470-1472 and the `setDrawBinding(ms.bindingSet(authored), &ms.constants(authored), sizeof(pbr::MaterialConstants))` at 1477-1480 guarded by `ms.ready()`).

**Members:**
```cpp
rhi::IResourceFactory* textureFactory_ = nullptr;                       // :5105 (#if AVER_MODULE_PBR)
std::unordered_map<std::string, pbr::MaterialHandle> materialAssets_;   // :5109 (#if AVER_MODULE_PBR)
std::unordered_map<i32, pbr::MaterialHandle> surfaceMaterials_;         // :5111 (PBR && SCENE)
voxi::VoxiRenderer voxiRenderer_;  bool voxiAttached_ = false;          // :5058-5059 (#if AVER_MODULE_VOXI)
bool projectRenderPending_ = false;                                     // :1916
```
`voxiRenderer_` is held **by value** and registered non-owning (`:873`); the comment at `:859` states it must outlive the device.

Order in `openProject`, from `applyProject:1811-1817`: `releaseProjectMaterials(); rebuildContentIndex(); loadProjectMaterials(); releaseProjectMeshes(); loadProjectMeshes(); loadStartMap();`

**DONE-WHEN:** the `[Material] <path> -> WxH, N mips (K KB) for slot '<s>'` line (`:1163`) prints once per referenced texture, with the same paths the editor prints for the same project. Anything that fails prints `keeps its fallback` at `:1141`/`:1159` — a silent pass here is not possible. Additionally, add the missing log at the genuine silent-fallback site: `modules/render.pbr/src/MaterialSystem.cpp:102` returns 0 when `resolve_` is null and `:109` caches that 0 permanently, with **no diagnostic anywhere** today.

**Pixel risk: none for the editor** — `MaterialSystem.cpp` is only touched to add a log line, which changes no pixel; if that makes anyone nervous, split it into its own commit and run the gates once.

---

## C9 — Physics init and the tick groups

**Files:** `GameApp.cpp`.

- `aver_phys_init()` and `groundBody_ = aver_phys_add_static_box(0,0,-kGroundHalfThickCm, kGroundHalfExtentCm, kGroundHalfExtentCm, kGroundHalfThickCm)` from `SandboxApp.cpp:546-557` (constants at 5508-5509: 5000 cm extent, 5 cm thickness). **Must run before any level load** — `loadLevel:5147` adds a static box per colliding placement, gated on `aver_phys_ready()`. This is the hard order constraint the comment at `:547` states. Consider dropping `groundBody_`: a real game's level supplies its own collision.
- tick-group block from `SandboxApp.cpp:1049-1057`, with the `spawnTestClass_` term of the gate at `:1050` **dropped** (it is a `--spawn-test` CLI harness widening): `if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) { aver_fw_tick(AVER_FW_TICK_PRE_PHYSICS, dt); aver_phys_step(dt); aver_fw_tick(AVER_FW_TICK_PHYSICS, dt); aver_fw_tick(AVER_FW_TICK_POST_PHYSICS, dt); }`. **`aver_phys_step` sits between PRE_PHYSICS and PHYSICS**, not between PHYSICS and POST_PHYSICS — the comment at `:1049` describes the tick groups, not where the step lands. Copy the code order.
- anim/skin/flush block from `1059-1071`, in **code order**: `anim::animSystem().tick(World::instance(), dt)` → `skinnedScene_->update(World::instance(), animSystem(), *e.device())` → `World::instance().flush()`. `SkinnedScene.hpp:44` claims `update` runs *after* `World::flush`; the code calls it **before**. Copy the code, not the comment. Not play-gated, deliberately.
- `SkinnedScene` creation from `661-670`, registered **first** of all render features: `init(*e.device())`, then `setResolvers(&GameContent::resolveAnimAsset, &GameContent::resolveSceneMesh, &content_)`, then `addRenderFeature`. Null is a legal state.
- `drivePlayCamera()` from `2153-2190`, verbatim: it returns unless `PLAYING`, reads the pawn via `aver_fw_controlled_pawn(aver_fw_player_controller(0))`, prefers `aver_fw_view_entity()`, and writes **only** `camPos_` (2179/2184), `yaw_` (2188), `pitch_` (2189). It must run **after** `flush` and **before** the view matrix.
- `playSessionActive()` from `5499-5505`. Note `PAUSED` counts as active there, while the tick block and `drivePlayCamera` both require exactly `PLAYING`.

**Members:** `std::unique_ptr<aver::render::SkinnedScene> skinnedScene_;` (:5073), `int32_t groundBody_ = 0;` (:5510). **Dropped:** `playTest_`/`playTestBegun_`/`playTestWait_`/`playTestFrames_` (:5020-5023), `spawnTestClass_`/`spawnTestDone_`/`spawnTestEntity_`/`spawnTestFrames_` (:5016-5019) and `maybePlayTest`/`maybeSpawnTestActor` entirely.

**DONE-WHEN:** with no play session, `aver_phys_step` is never called (log the step count once per change and assert it stays 0 in editor state); the loaded level logs one static body per colliding placement, matching `grep -vc nocollide` over the `PLACE` lines. Anim still ticks with no session, which is the intended asymmetry.

**Pixel risk: none.**

---

## C10 — Input: `InputState` → `aver_fw_*` (**the input rewrite lands here**)

**Files created:** `GameInput.cpp`. **Edited:** `GameApp.hpp/.cpp`.

Lift the **mechanism** from `SandboxApp.cpp:5447-5492` verbatim — `setMouseCaptured(bool)` (5447-5463), `warpToAnchor()` (5467-5481), `pollCapturedMouse()` (5484-5492), non-Win32 stubs at 5494-5495. It is pure Win32 (`ShowCursor`/`ClipCursor`/`SetCursorPos`/`GetCursorPos`) with no ImGui, and needs the HWND from `e.window()->nativeHandle()`.

Discard the **policy** above it (`1013-1026`) and the whole body of `pushInput` (`2122-2148`) — see the section below.

**Members:** `bool mouseCaptured_ = false; i32 captureAnchorX_, captureAnchorY_; f32 captureDx_, captureDy_;` (`SandboxApp.cpp:5332-5334`), `bool releasedByUser_ = false;` (:5444). **Dropped:** `hudPreviewIndex_`/`hudTest_`/`hudTestReported_`/`hudRectX_..H_` (:1934-1935, 1940-1941) — a game draws its HUD at the full backbuffer, so `aver_ui_begin_frame` gets `(0, 0, w, h)`.

**DONE-WHEN:** a new `--input-echo` flag logs, once per change, the set of `AVER_FW_KEY_*` currently held. Holding W then Shift then releasing must print exactly `W`, `W+LSHIFT`, `W`, `(none)`. This proves the ImGui-free path end to end and needs no baseline. Also: with `AVER_ENABLE_UI=OFF`, the game must still respond — an `AVER_WITH_IMGUI=0` build having no input at all is the exact defect `InputState.hpp:12-16` was written about.

**Pixel risk: none.**

---

## C11 — Shutdown mirror and packaging

**Files:** `GameApp.cpp`, `scripts/game.allowlist`, `scripts/stage-game.ps1` (if new DLLs must be staged), `docs/PACKAGING.md`.

Mirror `onShutdown` (`SandboxApp.cpp:1643-1706`), keeping **exact reverse registration order** and dropping every editor line:

```
removeRenderFeature(gameUi_); delete gameUi_;                       // 1675-1679, raw OWNING pointer
removeRenderFeature(skinnedScene_.get()); skinnedScene_.reset();    // 1692
if (voxiAttached_) { removeRenderFeature(&voxiRenderer_); voxiAttached_ = false; }
voxiRenderer_.shutdown();                                           // 1697-1698
releaseProjectMaterials(); textureFactory_ = nullptr;               // 1672-1673
aver_phys_shutdown(); groundBody_ = 0;                              // 1653-1654
scripts_.shutdown();                                                // 1703
```
**Dropped:** `mcp_.stop()` (1646), `setLogSink(nullptr,nullptr)` (1648), `editor::flushEditorPrefs()` (1649), `editor::shutdownActorEditors()` (1650), and the `#if AVER_WITH_IMGUI` icon-texture block at 1656-1670 — which sits immediately before `releaseProjectMaterials` and must not be scooped up with it.

Fix `docs/PACKAGING.md` while here: header should name `ae47a2f` (the tree its line numbers actually match), `:3`'s "STATUS: PLAN ONLY. Nothing below is built" is now false, and `:64` should say the guard defect is bidirectional, is a **compile failure** rather than a runtime loss of asset resolution, and is fixed by re-guarding rather than by relocation.

**DONE-WHEN:** `scripts/stage-game.ps1` then `scripts/verify-game.ps1` both pass, **and** the sabotage `verify-game.ps1`'s own docstring says should start failing now does: deleting a staged `Aver.*.dll` must fail the run. If it still passes, the package is shipping DLLs nothing uses.

**Pixel risk: none.**

---

## 1. The aspect ratio — dockspace rect vs swapchain

`viewAspect()` (`SandboxApp.cpp:2194`) is `vpH_ > 0.5f ? vpW_ / vpH_ : 1.777f`. `vpX_/vpY_/vpW_/vpH_` (`:4997`, defaults `{0, 0, 1600, 900}`) are written in **exactly one place**: `SandboxApp.cpp:3083`, inside `buildUI` under `#if AVER_WITH_IMGUI` (`:2868`) after an early return when `!e.device()->uiActive()` (`:2869`), from the ImGui "Level" tab's `GetCursorScreenPos()`/`GetContentRegionAvail()` (`:3078-3081`). `buildUI` is called from `onRender:1531`, so the rect written on frame N is consumed by frame N+1's `onUpdate`. A game has no dockspace: those four floats would sit at their constructor defaults forever, and every window size other than 1600x900 would render at the wrong aspect with no error.

**The replacement, in `GameApp::onUpdate`, sampled every frame:**

```cpp
f32 GameApp::viewAspect(Engine& e) const {
    const Window* w = e.window();
    const u32 ww = w ? w->width()  : cfg_.width;    // Window.hpp:41-42, PHYSICAL pixels (:43-44)
    const u32 wh = w ? w->height() : cfg_.height;
    return wh ? static_cast<f32>(ww) / static_cast<f32>(wh) : 16.0f / 9.0f;
}
```

Two supporting facts, both verified:

- **The window size *is* the backbuffer size.** `Engine::frameStep` resizes the swapchain to `window_->width()/height()` at the top of every frame (`modules/runtime/src/Engine.cpp:124-129`). Sampling per frame rather than once in `onInit` is what makes an alt-tab resize correct instead of stretched.
- **Never call `setViewportRect` at all.** `SandboxApp.cpp:1098` confines the scene to the dockspace's central node. `D3D12Device` initialises `vpX_ = vpY_ = vpW_ = vpH_ = 0` and documents `w/h == 0 means full backbuffer` (`modules/rhi.d3d12/src/D3D12Device.cpp:895`), consumed at `:2034-2037` (`rw = vpW_ ? vpW_ : width_`). Not calling the setter is therefore the correct full-backbuffer request, and calling it with `(0,0,w,h)` would work too but adds a way to be wrong.

There is no other route: `rhi::IDevice` exposes no backbuffer size (`width()/height()` exist only on `ISwapchain`, `RHI.hpp:37-38`; `D3D12Device::width()/height()` at `D3D12Device.cpp:762-763` are non-virtual and unreachable through `IDevice*`), and `Engine` exposes only `window()`, `device()`, `time()` and `requestExit()` (`Engine.hpp:21-27`).

Also re-decide the clear colour rather than inheriting it: `SandboxApp.cpp:1128`'s `setClearColor(0.055f, 0.055f, 0.062f, 1)` is documented at `:1127` as editor chrome grey for the region **outside** the viewport rect. A game filling the whole backbuffer has no such region.

## 2. Input — ImGui path vs `InputState`

`SandboxApp::pushInput` (`:2120-2149`) is `aver_fw_input_new_frame()` at `:2121` followed by a body **entirely inside `#if AVER_WITH_IMGUI`** (`:2122-2148`) that reads `ImGui::IsKeyDown` / `io.MouseDelta`, and steals Ctrl+Space and Escape for the editor (`:2127-2128, 2131, 2136`). **Only line 2121 survives the lift.** Everything else is rewritten against `aver::InputState`, which is already bound in `GameApp::onInit` (`GameApp.cpp:71-73`) and is keyed by **Win32 virtual key code** (`InputState.hpp:50-52`), not ImGui key enums.

```cpp
// GameInput.cpp — the ImGui-free replacement for SandboxApp::pushInput:2120-2149.
void GameApp::pushInput() {
    aver_fw_input_new_frame();                                   // framework_abi.h:179
    for (i32 i = 0; i < 26; ++i)                                 // 'A'..'Z' = 0x41..0x5A
        aver_fw_input_set_key(AVER_FW_KEY_A + i, input_.keyHeld(0x41 + i));
    for (i32 i = 0; i < 10; ++i)                                 // '0'..'9' = 0x30..0x39
        aver_fw_input_set_key(AVER_FW_KEY_0 + i, input_.keyHeld(0x30 + i));
    aver_fw_input_set_key(AVER_FW_KEY_SPACE,  input_.keyHeld(VK_SPACE));
    aver_fw_input_set_key(AVER_FW_KEY_LSHIFT, input_.keyHeld(VK_LSHIFT));
    aver_fw_input_set_key(AVER_FW_KEY_LCTRL,  input_.keyHeld(VK_LCONTROL));
    aver_fw_input_set_key(AVER_FW_KEY_LALT,   input_.keyHeld(VK_LMENU));
    aver_fw_input_set_key(AVER_FW_KEY_ENTER,  input_.keyHeld(VK_RETURN));
    aver_fw_input_set_key(AVER_FW_KEY_ESCAPE, input_.keyHeld(VK_ESCAPE));
    aver_fw_input_set_key(AVER_FW_KEY_TAB,    input_.keyHeld(VK_TAB));
    aver_fw_input_set_key(AVER_FW_KEY_LEFT,   input_.keyHeld(VK_LEFT));
    aver_fw_input_set_key(AVER_FW_KEY_RIGHT,  input_.keyHeld(VK_RIGHT));
    aver_fw_input_set_key(AVER_FW_KEY_UP,     input_.keyHeld(VK_UP));
    aver_fw_input_set_key(AVER_FW_KEY_DOWN,   input_.keyHeld(VK_DOWN));
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT,   input_.mouseHeld(0));
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_RIGHT,  input_.mouseHeld(1));
    aver_fw_input_set_key(AVER_FW_KEY_MOUSE_MIDDLE, input_.mouseHeld(2));

    // MOUSE DELTA: the CAPTURED path wins, exactly as SandboxApp.cpp:2146-2147 does.
    const f32 dx = mouseCaptured_ ? captureDx_ : static_cast<f32>(input_.mouseDX());
    const f32 dy = mouseCaptured_ ? captureDy_ : static_cast<f32>(input_.mouseDY());
    aver_fw_input_set_mouse(dx, dy, input_.wheel());              // framework_abi.h:183
}
```

Key codes verified at `modules/framework/include/aver/framework/framework_abi.h:164-176`: A..Z are 0..25 and 0..9 are 26..35 contiguously, which is what makes the two loops legal.

**Frame protocol.** `InputState.hpp:22-30` demands `newFrame()` *before* `pumpEvents()`. `Engine::run` pumps at the top of the loop (`Engine.cpp:90`) and `GameApp` already rolls the edges as the **last** statement of `onRender` (`GameApp.cpp:99`), which satisfies the same constraint from the other side. `pushInput()` goes in `onUpdate`, before the tick groups, so it reads the edges this frame's pump produced. Do not move `newFrame()` to the top of `onUpdate` — the comment at `GameApp.cpp:89-98` explains exactly what that costs (every single tap ignored, held keys fine).

**Mouse-capture policy.** The editor's policy (`:1013-1026`) is ImGui-only (`ImGui::IsMouseClicked`, `io.WantCaptureMouse`, Shift+F1, `inViewport(io.MousePos)`, `io.WantTextInput`). The game's replacement is two rules: capture on window focus / first click, release on `VK_ESCAPE`, latched in `releasedByUser_`. **When captured, use `captureDx_/captureDy_` and never `input_.mouseDX()`** — `warpToAnchor` re-centres the cursor every frame, and each warp generates its own `WM_MOUSEMOVE`, so `InputState`'s delta double-counts. That is precisely why the editor polls `GetCursorPos` instead, and it is the trap this whole path exists to avoid.

---

## Gate-oracle exposure

**No commit in this plan can move a pixel in `Sandbox.exe`.** The reason is structural, not a judgement call:

- No commit edits `sandbox/**`.
- No commit edits any source or header that `Sandbox` compiles or links. Every new file is under `modules/runtime.game/`, a static library `Sandbox` does not link (`sandbox/CMakeLists.txt:17` and following name `Aver.Runtime`, `Aver.Formats`, `Aver.UI`, `Aver.Render.*`, `Aver.Scene`, `Aver.Physics`, `Aver.Framework`, `Aver.Scripting.Host` — never `Aver.Runtime.Game`).
- No commit changes a CMake option, a PUBLIC compile definition, or a compile flag. C1 adds link deps **to `Aver.Runtime.Game`'s own interface**, which reaches only `AverGame`.

Two things would break that guarantee, and the plan deliberately avoids both:

1. **Promoting a file-static helper into `Aver.Core`.** `quatFromEulerDeg` (`:168-171`), `eulerDegFromQuat` (`:173-186`), `xformPoint` (`:190-195`), `appendBox` (`:125`), `appendSphere` (`:145`) exist **only** in `SandboxApp.cpp` — a repo-wide search found no other definition. Moving any of them into `Aver.Core` recompiles `SandboxApp.cpp` against different code, and floating-point codegen at a different inlining boundary is exactly the class of change the Release baseline exists to catch. **Copy them into `modules/runtime.game/src/GameMath.hpp`.** Sharing them is a de-dup-slice decision, gated on the gates.
2. **The `MaterialSystem.cpp:102` log line in C8.** It is the only edit outside `modules/runtime.game/` and `game/` in the entire plan. If it is not split into its own commit, C8 is the one commit that warrants a gate run.

Everything else needs no gate run at all. When the user does run `./scripts/gates.ps1`, note that `-Exe` defaults to `build/bin/Sandbox.exe` and `-Release` switches **both** the executable and the baseline together — `AverGame.exe` is never driven by the oracle in this slice.

---

## Traps

1. **`--probe-rel` is a fraction of the *editor viewport rect*, not the window.** `captureCheck` (`SandboxApp.cpp:4878-…`) reads `vpX_..vpH_` at `:4881-4884` and latches `capVpX_..capVpH_` at `:4887`. If a later slice adds a gate probe to `AverGame`, the same fraction lands on a *different* pixel because the game's rect is the whole backbuffer. `--probe-rel` survives a resize but not a change of aspect; screen every probe for a flat 7x7 neighbourhood.

2. **Both single-module configurations already fail to compile at HEAD.** PBR=ON/SCENE=OFF breaks at `:1227-1228` and `:1313`; PBR=OFF/SCENE=ON breaks at `:666`, `:690-692`, `:693`, `:1816-1817` and `:2859-2860`. All are non-template member functions, compiled whether called or not. The lift inherits this and does not create it; the re-guard table above fixes it **at the destination only**. Do not use `-DAVER_MODULE_SCENE=OFF` as a check on any commit here — it fails in `SandboxApp.cpp` for reasons that predate this work.

3. **`textureFactory_` has exactly one assignment, at `:882`, and it is three levels of `#if` deep**: inside `#if AVER_MODULE_VOXI` (opened `:835`) inside `#if AVER_MODULE_SCRIPTING` (opened `:828`), behind `voxiRenderer_.init()` succeeding (`:872`). Nothing enforces VOXI→SCRIPTING in CMake (only VOXI→PBR, root `CMakeLists.txt:134-137`), so a `-DAVER_MODULE_SCRIPTING=OFF` tree compiles out Voxi attachment and *all* material texturing in the editor. Do not reproduce that nesting in `GameApp`.

4. **A missing texture resolver is completely silent.** `modules/render.pbr/src/MaterialSystem.cpp:102` returns 0 when `resolve_` is null and `:109` caches that 0 **permanently**. The world still draws — `writeSlots` (`:114-125`) binds white/flat-normal/metalRough/black identities and `VoxiRenderer.cpp:181-183` sets a default binding — but authored-material entities render **white and fully metallic**, because the draw walk forces `col = white, metallic = roughness = 1.0` at `SandboxApp.cpp:1470-1472`. That symptom reads like a shading regression and is a wiring bug.

5. **`releaseProjectMeshes` leaks.** `:1313-1314` erases the ids from `sceneMeshes_` and clears `projectMeshIds_`, but calls no `destroyMesh` and never clears `meshBounds_`. Stale bounds survive a project switch and the GPU meshes leak. Copying it verbatim copies the leak; a game that reloads projects will notice before the editor does.

6. **`materialForSurface` caches negatives and does not fall through.** `:1234-1235` returns a cached `0` without retrying, and on the first *existing* candidate that fails to parse it `break`s (`:1246-1256`) rather than trying the next path. Fix a broken `.ocmat` and the game keeps returning 0 until the project is re-adopted.

7. **`rebuildContentIndex` indexes every regular file**, with no `AssetType` filter — `.cs`, `.csproj`, `.pdb`, `bin/obj` output, everything (`:1196-1203`). The key is the **forward-slash content-relative** spelling with no `Content/` prefix (`fnv1a64("Meshes/sphere.ocmesh")`), FROZEN at `:1201` to match C# `Assets.ObjectIdOf`; the value is the **native absolute path with backslashes**. Get either spelling wrong and every id silently misses.

8. **Two independent copies of the same resolver pair.** `anim::animSystem().setResolver(...)` at `:1209` and `SkinnedScene::setResolvers(...)` at `:666` are separate installations. Install only one and skinned entities draw at rest with no error message. Worse: `AnimSystem::user_` is a raw owner pointer in a process-global function-local static (`AnimSystem.hpp:74-75`, `AnimSystem.cpp:140-143`), `clear()` does not reset it, and it is dereferenced at `AnimSystem.cpp:22`. Last-writer-wins is the existing contract — harmless while `AverGame.exe` is a separate process, a dangling pointer the day a de-dup slice hosts both apps in one.

9. **`scene::World::instance()` is a process-global singleton** (`modules/scene/include/aver/scene/World.hpp:19`). Same caveat as above, same de-dup-slice deadline.

10. **`SkinnedScene.hpp:44` is wrong about ordering.** It says `update` is called "AFTER AnimSystem::tick and World::flush". The code calls it **before** `flush` (`:1069` vs `:1070`). Copy the code order. Similarly the comment at `:1049` describes the tick groups, not where `aver_phys_step` lands.

11. **`World::worldMatrix` is non-const** (`World.hpp:149`), so the draw walk cannot take a `const scene::World&`. Discovering this at the end of the render lift means rewriting the signature.

12. **`xformPoint` exists twice with reversed arguments.** `SandboxApp.cpp:190-195` is `xformPoint(const Mat4&, const Vec3&)`; `modules/render.voxi/src/VoxiRenderer.cpp:468` is `xformPoint(const Vec3&, const Mat4&)`. Both are file-static, so they do not collide today — but a "helpful" promotion to a shared header would silently pick one and transpose the other's culling.

13. **`unitCube` half-extent 1 is FROZEN** (`:585`): `.ocworld` `PLACEG` scales are half-extents in cm applied directly to it. Registering a differently-sized built-in cube scales the entire level.

14. **`frameCameraOn` also places the GI volume.** `:5231-5234` writes `giCenter_`/`giExtent_` as a side effect of framing the camera, consumed at `:1089`. A game that frames its camera differently must still set these or GI covers the wrong region.

15. **`saveLevel` forces `p.collide = true` on every placement** (`:5309`), so a `nocollide` flag does not round-trip. Not lifted here, but do not "fix" the load path to compensate for it.

16. **`verify-game.ps1` currently cannot fail on a missing engine DLL**, because `AverGame.exe`'s import table names no `Aver.*.dll` — it links four *static* libraries. Its own docstring says to re-run that sabotage after these lifts land and that the check should start failing. If it still passes after C11, the package is shipping DLLs nothing uses, and the isolation guarantee is weaker than the script claims.