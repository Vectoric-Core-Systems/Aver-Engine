# One runtime: reconciling the editor's runtime copy with Runtime/

> **STATUS, 2026-09-17 (later): C5, C6a, C8a and C9 landed; C7 and the rest of C6/C8 wait on runs and
> decisions.** Shared by both hosts now: `GameTick.hpp` (physics/audio start, PHYSICS.*/AUDIO.* apply,
> gameplay tick groups, AI tick -- `6302191f`), `GameCamera.hpp` (forward axis, view/projection push,
> play camera -- `ec5bd155`, `0943915a`), `MouseCapture` and `GamePawn.hpp` (`73cfcb8f`). The editor
> has File > Launch in Aver Engine Runtime (`86f9262a`), built but not run and not yet gated on
> `scripts/verify-game.ps1`. All built in Release; none of C5-C9 has been run.
>
> Not done, and why:
> - **C7 (render walk).** The editor's depth prepass is interleaved with its LOD/cluster selection
>   and with the colour pass marking draws as prepassed; occlusion needs a two-pass walk; LOD needs a
>   component whose producer side lives at mesh load. Each changes which pixels draw, so porting them
>   or switching the editor onto `game::drawWorld` needs frame captures before and after.
> - **C8 input publisher.** Unifying `SandboxApp::pushInput` with `game::publishInput` moves the
>   editor's key source from ImGui key state to `InputState`, and needs the recapture, input-stuck,
>   wheel-speed, pie-camera and viewmodel self-tests run.
> - **C6b (sky/fog/post/AverSR push).** The hosts' precedence chains differ by design; not shared.
>
> Decisions for the project owner, found while scoping:
> - **SETTLED 2026-09-19 -- the editor was fixed, not the runtime.** It applied
>   PHYSICS.GRAVITY/FIXEDSTEP and AUDIO.* only from inside `applyProjectRenderSettings`, so they
>   needed a RENDER.* key and an attached Voxi; a project stating only `PHYSICS.GRAVITY` had it
>   silently ignored. Gravity and audio mix have nothing to do with Voxi, and the runtime was
>   already right, so "the editor is the reference" did not apply. Both calls now run from
>   `SandboxApp::applyProject`, outside every Voxi and `hasRenderSettings()` gate, at the position
>   the runtime uses. Note the side effect: in a build with `AVER_MODULE_VOXI` off these keys were
>   discarded entirely before and now apply.
> - **SETTLED 2026-09-19 -- the runtime now gates on "PLAYING, or no GameMode was declared".**
>   The old justification for being ungated was that a graph-only project never reaches
>   `aver_fw_begin_play` because there is no C# GameMode to find. **That premise was stale.** A
>   graph class parented to "GameMode" gets the GAME_MODE bit through `sealClass`'s inherited kind
>   flags, so `aver_fw_find_class_with_flags` finds it -- verified against the packaged runtime on
>   graph-only PTTest, which logs `[Graph] GameMode 'AN_FPRules' begins play` and `[Game] play
>   session begun automatically (GameMode class 10)`. So the ungated tick was mutating worlds that
>   simply had not begun play. The second term keeps the one case the old premise really protected:
>   a project declaring no GameMode in any language, where nothing ever begins play. What the
>   editor measured when ITS equivalent was ungated: on PTTest over 1000 frames with Play never
>   pressed, a class-placed graph's own VAR climbed from 3.6e-05 to 12.31s across 4003 tick lines.
> - **SETTLED 2026-09-19 -- sky defaults with no SKY record: the editor moved to the format's.** The
>   editor's mirror read (0.19,0.42,0.78)/(0.72,0.80,0.90) while the runtime (`GameApp.hpp:420`), the
>   format (`OcWorld.hpp:401`) and `rhi::SkyAtmosphere` itself (`RHI.hpp:253`) all read
>   (0.24,0.45,0.85)/(0.72,0.83,0.95) -- the editor was the odd one out of three, so "the editor's
>   behaviour is the reference" did not apply: it cannot be the reference for a value the file format
>   itself declares. `SandboxApp.hpp` now holds the format's numbers. **This moves pixels in the
>   editor**, for a level with no SKY record under the AUTHORED sky model (`--sky-authored`, the Sky
>   panel's dropdown, or a level that authored `skyPhysical=false` and nothing else): `applyLevelEnv`
>   is guarded on `w.hasSky` (`LevelSky.hpp:56`) and the frame loop copies the editor's mirror over
>   `sky_` every frame, so the mirror is what such a level renders. Under the default Physical model
>   the dome fit overwrites both in the frame constants (`D3D12Device.cpp:4211`,
>   `VulkanDevice.cpp:2356`, both gated on `model == Physical`) and the authored numbers are inert --
>   which is how the editor kept a wrong dome this long unnoticed.
> - **SETTLED 2026-09-19 -- `--cam-wobble` during Play: the difference is kept, deliberately.** It
>   shows in the runtime and is overwritten by the play camera in the editor. `--cam-wobble` exists to
>   make a still frame move for verification, nothing combines it with a play camera, and the runtime
>   applies its offset only to the view matrix because it has no movement yaw to corrupt (the reasoning
>   is under "Camera wobble" below). Unifying it would be churn on a flag no shipped path uses, so the
>   code is left alone and this line is the record.
> - **SETTLED 2026-09-19 -- the manifest gained a post-process channel.** `RENDER.EXPOSURE`,
>   `RENDER.BLOOM`, `RENDER.AUTOEXPOSURE` and `RENDER.TONEMAP` now reach `rhi::PostSettings` in both
>   hosts, built on the pattern `RENDER.RESTIRHISTORY` established. Each carries a "not stated"
>   sentinel so an absent key leaves the compiled default alone -- negative rather than zero,
>   because a bloom intensity of exactly 0 is a legal authored value meaning "no bloom pass at
>   all", and `FormatTest` pins that round trip. An explicit `--exposure`/`--bloom` on the command
>   line still outranks the manifest, which is the precedence an earlier defect inverted.
> - **SETTLED 2026-09-19 -- a backgrounded game no longer receives input.** `publishInput`'s
>   `focused` argument was `e.window() != nullptr`, which answers "a window exists", not "the
>   window has focus". The Win32 foreground test the mouse-capture block already computed is now
>   hoisted and passed instead. `publishInput` already publishes an explicit release for every
>   slot when unfocused, so nothing can latch. A bounded `--frames` run deliberately keeps the old
>   answer: a capture's window is opened unactivated, so a real foreground query would report "not
>   foreground" for the whole run and turn every gate's input into releases. Off Win32 the old
>   answer stands, for want of a portable foreground query.
>
> **STATUS, 2026-09-17: C1-C4 are swapped -- the editor uses the library for content, level, water,
> streaming and landscape.** `SandboxApp` holds `game::GameContent content_` (`b97644fe`),
> `game::GameLevel level_` (`f3fcd628`), `game::GameWater water_` (`d822ef64`),
> `game::GameStreaming streaming_` (`85194d6b`) and `game::GameLandscape landscape_` (`2e6dcc89`,
> which also moved the editor's sculpt/undo/save authoring into the library as an editor-only API).
> The frame-budget controller is shared (`044818ed`). All built in Release; the level-load swap was
> checked by hand in the editor, the water/streaming/landscape swaps are not yet. Still open: C5-C8
> (physics/tick/audio, camera and device push, the render walk, input and the play session), the CLI
> render-override collapse, the GPU cluster/LOD-selection path (editor-only for now), and C9.
>
> **STATUS, 2026-09-16: the runtime-only catch-up is implemented.**
> Every gap in the table below is closed in `Runtime/` (commits `696c3666`, `2d3856ec`, `e539f30f`,
> `6f079f41`, `b956a501`, `40456852`, `88683c93`, `30db84f4`, `23bd0ba4`), built but not yet run
> side by side with the editor. The rest of this
> document is the original plan, kept as written. It is the reconcile
> map for "Phase C" of the editor/runtime split: the editor (`Sandbox.exe`) still carries a private copy
> of the game runtime inside `SandboxApp` (now split across `sandbox/src/Sandbox*.cpp`), and the standalone
> `AverEngineRuntime.exe` runs its own copy from `Runtime/`. The goal is ONE runtime -- the editor calls
> the `Runtime/` library for each subsystem and deletes its copy -- with **the editor's behaviour as the
> reference**, the library still linking no ImGui, and editor-only needs entering through explicit hooks.
>
> Produced by four read-only Sonnet readers, one per slice group. Line numbers are as they read the tree
> at `385a5e8e` and will drift; function names are the durable locators. Claims here are the readers'
> and have not all been re-derived by hand -- treat them as the starting point for each slice, and
> re-read the code before acting on any one of them.

## What the readers found, in short

**The shipped runtime is missing behaviour the editor already has**, which is exactly what two copies
produce. Several of these are player-visible in a packaged game today:

| Gap in `Runtime/` | Where the editor has it | Player-visible effect |
|---|---|---|
| A frustum- or occlusion-culled entity is dropped entirely, never submitted to Voxi | `SandboxRender.cpp` direct route / `emitEntityDraws` | an off-screen shadow caster's shadow and GI vanish as it leaves the view |
| No multi-material split (`buildMeshParts` / `planEntityDraws`) | `SandboxAssets.cpp`, `SandboxRender.cpp` | every part of a multi-material mesh paints with slot 0's material |
| `materialForSurface` never resolves an `.ocmat`'s `GRAPHREF` (`resolveMaterialGraph`) | `SandboxAssets.cpp` | node-graph materials render as plain stock PBR |
| No owner-hide (`kMeshRendererHiddenFromOwner`) | `SandboxRender.cpp` | a first-person pawn's own body renders in front of the camera |
| No Player Start / SPAWN placement | `SandboxPlay.cpp` `placePawnAtPlayerStart` | the pawn starts wherever the GameMode puts it, usually the origin |
| No OS mouse capture | `SandboxPlay.cpp` `setMouseCaptured` / `warpToAnchor` / `pollCapturedMouse` | mouse-look stops turning at the window edge |
| No HUD renderer (`Aver.Render.UI` not linked) | `SandboxRender.cpp` `submitGameUi` | a HUD that draws in Play draws nothing in the package |
| Landscape, water, chunk streaming, legacy `.ocmap` dispatch have no runtime counterpart | `SandboxLevelLoad.cpp` | levels using them load differently or not at all |
| `PHYSICS.GRAVITY` / `FIXEDSTEP` and `RENDER.GIVOLUME` manifest keys ignored; no frame-budget controller | `SandboxProject.cpp` | physics and GI placement differ from the editor |

**Already at parity:** the content index and asset resolvers (byte-identical logic), the camera and view
push math, the animation curve/notify relays, level placement (`modules/world` already shares the loop).

**Order the readers recommend.** Land the runtime-only fixes first -- they cannot move a pixel in
`Sandbox.exe`, because the editor does not link `Aver.Runtime.Game` yet -- then add the hook plumbing with
defaults that reproduce today's runtime exactly, and only then switch editor code to call the library,
one subsystem per commit, each with a before/after comparison of the SAME project in both hosts
(`scripts/verify-game.ps1`'s divergence gate, screenshots, and the `[Occlusion]` / `[LOD-*]` log counts
where the render walk is involved). The occlusion, depth-prepass and Trifactor LOD machinery in
`SandboxRender.cpp` is the riskiest piece: it is interleaved with the draw walk and has no runtime
counterpart at all.

**Decisions that belong to the project owner** (raised by the readers, not settled here):
- the editor's own `#if AVER_MODULE_VOXI` / `hasRenderSettings()` coupling around physics-gravity and
  audio-mix apply looks like a bug in the reference itself -- fix it in the editor too, or preserve it?
- how a project names its default HUD for the runtime.
- whether landscape / water / streaming come into `Runtime/` now or as later slices.

---

## C1 (content index + asset resolvers) and C2 (mesh registry + materials)

**Scope as read:** C1 (content index + asset resolvers) and C2 (mesh registry + materials). Editor side: sandbox/src/SandboxAssets.cpp (all named functions) + the Voxi/PBR attach block in sandbox/src/SandboxApp.cpp:onInit (~1265-1429) + resolveMaterialTexture (SandboxApp.cpp:2950-3000) + project-open orchestration in sandbox/src/SandboxProject.cpp:applyProject. Runtime side: Runtime/src/GameContent.cpp + Runtime/include/aver/game/GameContent.hpp + the equivalent attach block in Runtime/src/GameApp.cpp:attachVoxi (707-800) and GameApp.cpp:openProject (961-1039).

IMPORTANT CORRECTION TO THE BRIEF: docs/GAME-LIFT.md is explicitly marked "STATUS: HISTORY... THIS DOCUMENT DESCRIBES COMPLETED WORK THAT HAS BEEN REMOVED" (its own header, 2026-08-17 snapshot, modules/runtime.game paths that no longer exist). Its C1-C11 plan has already been executed and gone further than it describes: Runtime/src/GameContent.cpp is a mature, independently-designed class, not a raw copy of SandboxApp. Do not use GAME-LIFT.md line numbers or per-function bodies as current truth; I read live source throughout. GAME-LIFT.md's high-level traps (the two survivors: xformPoint's reversed-argument twin, and the file-static-promotion warning) still generalize but are not load-bearing for this slice.

### Reconcile map

Legend: E=editor (sandbox/src/SandboxAssets.cpp unless noted), R=Runtime/src/GameContent.cpp (class `aver::game::GameContent`) unless noted.

**resolveAssetPath** — E:10-28 `SandboxApp::resolveAssetPath` (PBR-guarded). R:502-517 `GameContent::resolveAssetPath` (PBR-guarded). Byte-identical logic (absolute-path sniff `p[1]==':'||p[0]=='\\'||p[0]=='/'`, else content-relative-then-fallback, else contentIndex_ lookup by id). No divergence.

**rebuildContentIndex / adopt** — E:60-84 `rebuildContentIndex()` (unguarded; reads `project_` member already set elsewhere). R:40-80 `GameContent::adopt(const fmt::ProjectDesc&)` (unguarded; takes+stores the project itself — a cleaner API, same walk). Identical walk (error_code recursive_directory_iterator, forward-slash rel-path, fnv1a64, FROZEN comment). One real divergence: R warns `AVER_WARN("[Content] the project's content root does not exist...")` when the dir is missing (adopt():48-51); E silently returns (rebuildContentIndex:65, no log). Low risk, additive-only; keep R's warning when unifying. Both install `anim::animSystem().setResolver(...)` under `#if AVER_MODULE_SCENE` — E guards this only on the *tail* of an otherwise-unguarded PBR-adjacent function (historically PBR, now correctly re-decided to SCENE per GAME-LIFT.md's own "guard decision" table); R guards the whole call the same way. Match.

**resolveAnimAsset** — E:88-93, R:112-115. Identical (`static std::string(u64,void*)`, `self->contentIndex_.find`).

**resolveSceneMesh** — E:96-101 (SCENE-guarded). R:488-491 `GameContent::resolveSceneMesh` (SCENE-guarded). Identical.

**resolveMaterialGraph — MISSING ON THE RUNTIME SIDE.** E:110-134, PBR-guarded: resolves an .ocmat's `GRAPHREF` (content-relative) through `pbr::materialGraphs()`, caches by compiled path via `idOf`, compiles via `fmt::loadOcgraph` + `pbr::materialGraphs().add(...)` on miss, returns 0 (stock shading) on any failure. Called from E's `materialForSurface` at line 156: `d.graphId = resolveMaterialGraph(extras.graphRef);`. **R's `materialForSurface` (GameContent.cpp:572-606) reads `fmt::OcMatExtras extras` off `loadOcmat` but never reads `extras.graphRef` and never sets `d.graphId`** — `pbr::MaterialDesc::graphId` (modules/render.pbr/include/aver/pbr/Material.hpp:206) defaults to 0. `Aver.Render.PBR.Materials` (which owns `pbr::materialGraphs()`, modules/render.pbr/include/aver/pbr/MaterialGraphRegistry.hpp) is already PUBLIC-linked into `Aver.Runtime.Game` whenever `Aver.Render.PBR` exists (Runtime/CMakeLists.txt:71-73) — this is not a module-boundary gap, it is an un-ported function. **Behavior: any .ocmat with a GRAPHREF renders with its authored node-graph shading in the editor and with plain stock PBR shading in the packaged runtime, silently — no warning either side.** Concrete regression risk if/when the editor swaps to calling `GameContent::materialForSurface` as-is.

**materialForSurface** — E:137-165 (PBR-guarded), uses `editor::resolveMaterialPath(project_.binariesDir(), content, name)` from sandbox/src/MaterialResolve.hpp (header-only, filesystem-only, no Aver.Pbr dependency). R:572-606, PBR-guarded, carries its **own inline copy** of the identical 3-candidate array (`Binaries\Materials\<n>.ocmat`, `Content\Materials\<n>.ocmat`, `Content\<n>`), same precedence (first-existing wins, parse failure `break`s rather than falling through, negative-result caching with no retry). MaterialResolve.hpp's own header comment (lines 6-9) explicitly documents this as an intentional, verified-matching duplication across the sandbox/Runtime build-target boundary — confirmed byte-for-byte identical candidate order on both sides. The ONE functional gap is resolveMaterialGraph above (E sets `d.graphId`, R never does); everything else (caching, log lines, the `break` on parse failure) matches.

**loadProjectMaterials / releaseProjectMaterials — DELIBERATELY ASYMMETRIC, already reconciled by design.** E:681-712 (`#if AVER_MODULE_PBR`, body `#if AVER_MODULE_SCENE`): eagerly walks Binaries\Materials + Content\Materials (non-recursive, via `editor::projectMaterialStems`) and resolves every stem through `materialForSurface`, populating `surfaceMaterials_` for every *authored* material regardless of whether the current level references it — feeds SandboxPanels.cpp's material picker (iterates `surfaceMaterials_` directly, lines 1437-1471) and the drag-drop assign flow (SandboxViewport.cpp:584-585). R has **no such function at all** — GameContent.hpp:79-84 documents why: "the runtime carried a copy of the editor's, nothing ever called it, and it was removed... a level resolves each surface it references through materialForSurface(), lazily and Binaries-first." Confirmed: Runtime/src/GameLevel.cpp:42-49 binds `content.materialForSurface(surface)` → `content.bindSurfaceMaterial(token, h)` per placement inside the SHARED `world::instantiate` placement loop (modules/world) that both `GameLevel::load` and `sandbox/src/SandboxLevelLoad.cpp:1053-1058`'s `loadLevel` now call — so the actual level-load material-binding mechanism is **already unified** between editor and runtime via `aver::world::instantiate`'s `opt.bindMaterial` callback; only the *eager, level-independent preload* (for editor tooling, not gameplay) is editor-only. `releaseProjectMaterials()` matches: E:706-710 clears `materialAssets_`+`surfaceMaterials_` (destroying each `pbr::MaterialLibrary` handle first); R:608-613 does the same (guarded PBR / PBR&&SCENE respectively) — same shape, R just never had anything eager to release.

**loadProjectMeshes — real functional gap: multi-material split.** E:169-434 (`loadProjectMeshes(Engine&)`, SCENE-guarded) vs R:380-476 (`loadProjectMeshes(rhi::IDevice&)`, unconditionally SCENE since the whole class is). Shared core (identical): non-recursive filter via `assetTypeFromPath==Mesh`... no, recursive walk, `fmt::loadOcMesh`, position/normal/uv-only `rhi::MeshVertex` repack (joints/weights dropped — both sides note skinning uploads separately through `resolveSceneMesh`), `sceneMeshes_[id]=createMesh(...)`, `meshBounds_[id]`, `meshSlot0Material_[id]` from `materialSlots[0]`, `projectMeshIds_.push_back(id)`. **Divergence 1 (biggest in this slice): per-submesh material split.** E additionally calls `buildMeshParts(e, id, md, verts, rel)` (SandboxAssets.cpp:816-874, `#if AVER_MODULE_LANDSCAPE`) for every mesh whose `md.submeshes.size() > 1`: it re-indexes each submesh into its OWN compacted `MeshHandle` + `material` token (`MeshPart{mesh, material}`), stored in `meshParts_[id]`. The editor's draw path (SandboxRender.cpp: multiple `drawMesh(..., parts.data(), parts.size(), ...)` call sites, e.g. 410-415, 1066-1071, 1546-1551) passes this parts array through `rhi::IDevice::drawMesh`'s optional parts overload so each submesh draws with its own material — SandboxApp.hpp:1658-1668 explains *why* (ray-tracing/BLAS constraints force per-part splitting rather than a per-geometry material index). **R has no `MeshPart`, no `meshParts_`, no split at all.** Runtime/src/GameRender.cpp:209 calls the plain `device.drawMesh(drawHandle, &wm.m[0][0], col, metallic, roughness)` with no parts argument — every project mesh, however many materials its .ocmesh names, draws as ONE mesh with ONE material (`meshDefaultMaterial`/entity override, i.e. effectively submesh[0]'s material paints the whole mesh). Geometry itself is intact (the full concatenated `md.indices` still gets uploaded — nothing is missing geometrically), only per-submesh material assignment is lost. Editor's own comment (SandboxAssets.cpp:790-798) states this was found via "Importing 53 foliage meshes... 37 of them name exactly one material" — meaning ~16/53 in that one test set (any tree/rock/prop with bark+leaves, e.g. a multi-slot palm) WOULD visibly regress if the runtime's mesh registry is adopted as-is. **Divergence 2 (guard mismatch, likely latent):** `buildMeshParts` and `meshDefaultMaterial` are both declared/defined inside `#if AVER_MODULE_LANDSCAPE` in the editor (SandboxAssets.cpp:790/805, SandboxApp.hpp:1674-1677) but the *call site* to `buildMeshParts` inside `loadProjectMeshes` (SandboxAssets.cpp:234) sits only under `#if AVER_MODULE_SCENE`, unguarded by LANDSCAPE — if LANDSCAPE can ever be OFF while SCENE is ON, that is a link error in the editor today; I did not verify LANDSCAPE's CMake dependency chain to rule this out. R's `meshDefaultMaterial` (GameContent.hpp:99, .cpp:618-621) is guarded only `PBR && SCENE`, no LANDSCAPE dependency — so in an editor build where this guard combination were actually exercised, R's fallback would apply and E's would not. Flagged for a hand-check, not confirmed exploitable in practice. **Divergence 3 (documented, intentional, low risk):** the coarser-LOD depth-proxy. E builds the FULL LOD ladder (`MeshLodLadder`, every level uploaded, `meshLods_`/`meshClusterData_`/optionally `meshClusterGpu_` under `--lod-mesh-shader`) because its lit pass can runtime-select among levels (`--lod-select`). R (GameContent.cpp:441-469) computes and uploads exactly ONE extra depth-only proxy level (`depthProxyMap_`), with an explicit comment explaining why: "The game has no runtime LOD selection at all... uploading the rest of the ladder would be VRAM that nothing can ever look up." This is a considered, documented simplification — not a bug — and matches R's `GameApp::depthProxyLookup` reading `content_.depthProxyMap()` (GameApp.cpp:1351) vs E's `SandboxApp::depthProxyLookup` reading its own `depthProxy_` member (SandboxApp.hpp:4676, populated at SandboxAssets.cpp ~300-318 inside the full-ladder path). **Divergence 4:** virtualized-geometry / GPU-cluster mesh-shader pipeline — see `ensureLodMeshPipeline` below, entirely missing on the runtime side.

**ensureLodMeshPipeline — entirely missing on the runtime side, large feature, do not fold into this slice.** E:440-640 (`#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR`), called once from `onInit` (SandboxApp.cpp:707-719, gated on device caps: `meshShaderTier>0 && shaderModel>=65 && dxcAvailable`, D3D12-only when Voxi is linked) and again from `applyProject` (SandboxProject.cpp:66). Builds an AS/MS/PS mesh-shader pipeline (`cluster_material.hlsl`) that merges Voxi's GI/shadow register map into table 0 (a whole separate register-allocation contract documented at SandboxAssets.cpp:498-521) so huge meshes (the doc's own example: a 17.18M-triangle tree) draw through GPU-driven clusters instead of full LOD0 detail. `grep`-confirmed **zero** occurrences of `ensureLodMeshPipeline`/`lodMeshPipeline`/`ClusterMaterialShader`/`lodMeshShaderEnabled` anywhere under Runtime/. `Aver.Trifactor` is linked into `Aver.Runtime.Game` already (Runtime/CMakeLists.txt:106-108), so this is a genuine unported feature, not a structural block — but it is large (shader compilation, a documented Voxi register-map coupling, D3D12-only, its own latch/relatch-on-graph-revision logic) and orthogonal to content-index/material reconciliation. **Recommend treating as its own future slice** ("GPU virtualized geometry for the packaged runtime"), not part of C1/C2's reconcile.

**releaseProjectMeshes — E has a live, confirmed GPU-handle leak; do not mirror it into R.** E:645-678 (`SandboxApp::releaseProjectMeshes(Engine&)`). For every id in `projectMeshIds_` it destroys `meshParts_` handles (line 655: `e.device()->destroyMesh(p.mesh)`) and `selOutlineLines_` line meshes, erases `sceneMeshes_`/`meshTris_`/`meshSlot0Material_`/`pickGeometry_` map entries, and (TRIFACTOR) `meshLods_`/`meshClusterData_`/`clusterCutCache_` (with its own `destroyMesh`, line 673). **It never calls `e.device()->destroyMesh()` on the base `sceneMeshes_[id]` handle itself** — confirmed by `grep -n destroyMesh sandbox/src/*.cpp`: the only `destroyMesh` calls inside this function are for split parts and the cluster-cut cache, never the plain single-material mesh every non-split project mesh gets. `meshBounds_` and `meshPathById_` are also never erased here (stale entries survive a reload; harmless bloat unless the same id is reused, in which case they're overwritten). **Net effect: every project reload (or project switch) leaks one GPU mesh per non-multi-material asset in the outgoing project — the common case, since the editor's own comment says most meshes are single-slot.** This is a live, current bug (I read today's SandboxAssets.cpp, not the stale GAME-LIFT.md trap list), not something to preserve. **R has no `releaseProjectMeshes` at all** — GameContent.hpp:113-124 explains why ("There was one, it had zero callers, and it was wrong... erased entries without ever calling IDevice::destroyMesh") and explicitly sketches the correct shape any future one should take, including "mirroring the editor's SandboxApp::releaseProjectMeshes" — **that guidance comment is itself now slightly wrong**: mirroring E's current function verbatim would reproduce the same base-handle leak just described. Whoever writes GameContent's `releaseProjectMeshes(rhi::IDevice&)` should destroy every `sceneMeshes_[id]` handle too, plus mirror the split-part and (once ported) LOD-proxy destruction, and should NOT copy E's omission forward. Today this doesn't matter for the runtime (single `openProject` call per process, per GameContent.hpp's own note) but matters the moment either side gains a reload path.

**loadProjectParticleEffects** — E:715-746 (`#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE`). R:630-664 (`#if AVER_MODULE_PARTICLES`, GameContent.hpp:158 notes it needs no SCENE guard at all — CPU-only data). Identical walk/id-space/log shape; both write into the same process-global `particles::particleEffects()`. No behavioral divergence found; R's guard is slightly looser (correctly — particle effects are not a SCENE concept) which is an improvement, not a regression, worth carrying back to E if/when unified.

**makeMaterialFor(MeshObj&) — editor-only, no runtime concept, no lift needed.** SandboxAssets.cpp:751-769. `MeshObj`/`objects_` (SandboxApp.hpp:805-816) is the editor's own synthetic placeholder/test-scene object system (e.g. `--furnace-grid`), entirely disjoint from project content (`contentIndex_`/`sceneMeshes_`). Called only from SandboxApp.cpp:921,1112. Stays in SandboxApp permanently.

**warnDeadMaterialHandle — editor-only diagnostic, currently unreachable on the runtime side (not a gap yet).** SandboxAssets.cpp:781-788, called once from SandboxRender.cpp:2131 (`if (rs.look.warnDeadHandle) warnDeadMaterialHandle(mat);`) inside the draw-resolution path (`aver/game/SceneSubmission.hpp`) when a token in `surfaceMaterials_` maps to a `pbr::MaterialHandle` that `pbr::MaterialLibrary` no longer resolves (stale after a project reload destroyed it). R's `drawWorld` used to read `content.authoredFor(mat)` and, if non-zero, call `pbr::MaterialLibrary::get().desc(authored)` for the `blended` check while putting **no liveness test on the branch that then painted the surface** — so a stale handle took the authored branch and came out as pure white metal, a mirror.

**FIXED 2026-09-19 as part of C7 step 1**, by construction rather than by adding a guard: `drawWorld` no longer carries its own ladder at all, it calls the shared `resolveSurfaceLook`, which requires `authored && authoredLive` and otherwise falls through to the next rung. The shared header's test already pinned that fall-through, which is why the editor never had this bug. **Being accurate about the severity: this was unreachable in the shipped host** — `GameApp::openProject` runs exactly once per process, so `surfaceMaterials_` cannot go stale mid-session there. It mattered because the editor, which *does* reload, is to call this same walk at C7 step 2, and because a rung that reads correctly only for want of a reload path is not a rung anybody should rely on. The diagnostic half (a `[Game]`-worded warning when the ladder rejects a dead handle) rides along; each host still words its own line from the flags the shared resolver returns.

**meshDefaultMaterial** — E: SandboxAssets.cpp:805-808 (`#if AVER_MODULE_LANDSCAPE`). R: GameContent.cpp:618-621 (`#if AVER_MODULE_PBR && AVER_MODULE_SCENE`, GameContent.hpp:99). Same logic (`meshSlot0Material_[id]` or 0 = "ask nothing"). Guard mismatch noted above under loadProjectMeshes divergence 2.

**buildMeshParts** — see loadProjectMeshes above; no runtime counterpart at all.

**Voxi/PBR attach (onInit vs attachVoxi)** — E: SandboxApp.cpp:1265-1429 (inline in `onInit`). R: `GameApp::attachVoxi(Engine&)`, GameApp.cpp:707-800, called from `onInit` at GameApp.cpp:1691, well before `openProject` (line 1741) — matching E's ordering (Voxi attach happens in `onInit`, before `applyProject` is ever reached). Core sequence matches almost exactly: build `voxi::DeviceInfo` from `caps` (same field list, same `nrdSupported` expression — both sides' own comments cross-reference each other, e.g. GameApp.cpp:720-721 explicitly says "same expression" as SandboxApp.cpp:2508-2514, though I found the actual current line is ~1288, i.e. that comment's line reference is stale too), `voxi::Renderer::get().setDeviceInfo(di)`, seed the manifest's render settings BEFORE `voxiRenderer_.init()` (both sides document the identical N2/"why before init" reasoning almost verbatim), `voxiRenderer_.init(*device)` → `addRenderFeature` → `voxiAttached_=true` → **THE SAME TWO ADJACENT LINES** `textureFactory_ = device->resources(); voxiRenderer_.materials().setTextureResolver(&resolveMaterialTexture, self);` (E: SandboxApp.cpp:1420-1422; R: GameContent-owned via `content_.setTextureFactory(...)` + same resolver, GameApp.cpp:784-785) — both sides' comments independently call out that this adjacency is load-bearing (makes the `!textureFactory_` null-check in `resolveMaterialTexture` unreachable rather than merely unlikely) → `setDepthProxy(&depthProxyLookup, self)`. **Real divergence:** E resolves the manifest via the already-loaded `project_` member (manifest applied elsewhere in the same `onInit`/`applyProject` flow, no separate re-read); R (attachVoxi, GameApp.cpp:746-766) does its own STANDALONE manifest re-read (`fmt::loadOcproject` into a **local** `fmt::ProjectDesc seed`, not `project_`) specifically because `attachVoxi` runs before `openProject` populates the real `project_` — R's own comment (734-740) explains this is deliberate and idempotent (whatever `attachVoxi`'s seed derives, `openProject`'s later `applyProjectRenderSettings()` re-derives identically from the real `project_`). This is a genuine, intentional architectural difference driven by call-order (R attaches Voxi before opening a project at all — it needs *some* manifest to size the voxel volume correctly even for `--frames`-only smoke runs with no project — whereas E always has a project open, or about to be, by the time this code runs). Not a bug; just note it as "two reads of the same file by design, not two sources of truth."

**resolveMaterialTexture** — technically lives in SandboxApp.cpp (2950-3000), not SandboxAssets.cpp, but is the direct PBR-texture-upload counterpart to GameContent's copy (GameContent.cpp:519-570). Logic is identical line-for-line (slot→`TextureUsage` switch including the Layer1BaseColor/Layer1Normal fix, `assets::uploadTexture`, same log lines, same `ResolvedTexture{handle, averageLinear[3]}` shape). One cosmetic-only difference: E's warn string at line 2990 contains a mis-encoded em-dash (`â€”`, apparent double-UTF-8-encoding artifact) where R's equivalent (GameContent.cpp:558) uses a plain `-`. Log text only, zero behavioral effect — flagged only because "identical logic" claims should be exact; not worth a commit on its own, fix opportunistically if touching that line for other reasons.

### Proposed library API

GameContent (Runtime/include/aver/game/GameContent.hpp) is already the right shape for the editor to call into — non-owning static resolvers keyed on `void* user`, `rhi::IDevice&`/`rhi::IResourceFactory*` instead of `Engine&`, no ImGui, no `SandboxApp*` anywhere. The additions needed before the editor can actually swap its own content/mesh/material members for a `GameContent` instance, in priority order:

1. `void materialForSurface`'s implementation gains the `resolveMaterialGraph` port (private free function or private static method on GameContent, PBR-guarded, ported near-verbatim from SandboxAssets.cpp:110-134) and sets `d.graphId` before `MaterialLibrary::get().create(d)`. No public API change — same signature, corrected behavior.

2. `loadProjectMeshes` gains the multi-material split: a public `struct MeshPart { rhi::MeshHandle mesh; i32 material; }` (SCENE-guarded, mirrors SandboxApp.hpp:1669-1672), a private `meshParts_` map, a `partsFor(u64 id) const -> std::pair<const MeshPart*, u32>`-style accessor (or reuse the same `const MeshPart*, count` calling convention `IDevice::drawMesh`'s existing parts overload already expects, since GameRender.cpp would need to pass it through), and the split-building logic ported from `buildMeshParts` (SandboxAssets.cpp:816-874) called unconditionally from `loadProjectMeshes` when `md.submeshes.size()>1` (no LANDSCAPE guard — see the guard-mismatch note in the map; recommend dropping the LANDSCAPE dependency entirely here since nothing about material-slot splitting is landscape-specific).

3. `void releaseProjectMeshes(rhi::IDevice& device)` — new public method, SCENE-guarded. Must destroy every `sceneMeshes_[id]` handle (the gap in E's current version — do not mirror that omission), every `meshParts_[id]` handle, clear `meshBounds_`/`meshSlot0Material_`/`projectMeshIds_`/`meshParts_`, and once (2) above lands, the depth-proxy map entries for removed ids too.

4. An editor-only eager preload: `void preloadProjectMaterials()` (or similarly named), PBR&&SCENE-guarded, doing what E's `loadProjectMaterials()` does today (walk Binaries\Materials + Content\Materials via the existing sandbox/src/MaterialResolve.hpp helpers — which stay in sandbox/, GameContent still cannot include them, so this method re-derives the same stem list inline or GameContent grows its own copy of `projectMaterialStems`-equivalent logic guarded the same way `materialForSurface`'s candidate array already is duplicated) and calling `materialForSurface` per stem, populating `surfaceMaterials_` even for stems no loaded level references yet. Called ONLY from the editor's own project-open path, never from `GameApp::openProject` — this is the explicit hook the task's "editor-only needs enter through hooks, never #if EDITOR" rule calls for: a plain extra public method the runtime simply never invokes, not a compile-time branch inside `GameContent` itself.

5. Read-only accessors for editor UI that currently reach into private members directly: an iterator or snapshot over `surfaceMaterials_` (for SandboxPanels.cpp's material picker, SandboxPanels.cpp:1437-1471) and confirmation that `pathFor(id)`/`pathsWithExtension(ext)` (already public, GameContent.hpp:44,53) fully subsume what E's separate `meshPathById_` member is used for (Content Browser display, foliage palette naming) — if so, `meshPathById_` can simply not be ported; the editor gets the same information through `content.pathFor(meshId)` since `contentIndex_` already carries every asset (unguarded, includes meshes).

Everything else in GameContent's current public surface (`adopt`, `pathFor`, `pathsWithExtension`, `resolveAnimAsset`, `resolveMaterialTexture`, `resolveAssetPath`, `materialForSurface`, `bindSurfaceMaterial`/`authoredFor`, `meshDefaultMaterial`, `registerBuiltins`, `loadProjectMeshes`, `meshFor`/`meshCount`/`projectMeshCount`, `boundsFor`, `depthProxyMap`, `resolveSceneMesh`, `lookFor`, `loadProjectParticleEffects`) is already a faithful superset-shaped match for what the editor needs and can be adopted as-is once (1)-(5) land.

### What the editor keeps

- `MeshObj`/`objects_`/`makeMaterialFor` — the synthetic placeholder-scene test-object system (`--furnace-grid` etc.). Not project content, no runtime concept exists or should exist for it.
- `ensureLodMeshPipeline` and the whole virtualized-geometry/GPU-cluster path (`meshLods_`, `meshClusterData_`, `meshClusterGpu_`, `lodMeshPipeline_`/`lodMeshAsShader_`/`lodMeshMsShader_`/`lodMeshPsShader_`, `lodMeshShaderEnabled_`/`lodMeshPipelineTried_`/`lodMeshPipelineReady_`) — left entirely on the editor side per this slice's recommendation; a future slice's concern, not this one's.
- `loadProjectMaterials()` (the eager scan) stays conceptually editor-owned even after (4) above lands in GameContent — the editor calls the new `GameContent::preloadProjectMaterials()` hook explicitly; the runtime never calls it, matching today's already-deliberate asymmetry (GameContent.hpp:79-84).
- `pickGeometry_`, `selOutlineLines_`, `meshPathById_` (pending confirmation it's redundant per proposedApi item 5), `skinnedMeshIds_` — pure editor-tooling bookkeeping (viewport picking, selection outlines, Content Browser labels) with no gameplay meaning; stay as SandboxApp members reading through GameContent's public accessors rather than migrating into GameContent itself.
- `MaterialResolve.hpp`'s header-only, filesystem-only helpers stay in sandbox/ (documented reason: it must stay includable from a headless test with no Aver.Pbr link) even after GameContent gains its own equivalent inline logic for hook (4) — the duplication is deliberate and already verified matching, not technical debt.

### Hooks needed

Explicit, non-#if-EDITOR hooks GameContent needs to gain for the editor to route through it without losing capability:
1. `preloadProjectMaterials()` — eager material-picker preload (proposedApi #4). Editor calls it once right after `adopt()`; runtime never calls it.
2. A `surfaceMaterials_` read accessor for the material picker/drag-drop-assign UI (SandboxPanels.cpp, SandboxViewport.cpp:584-585) — either a const iteration accessor or a `snapshot()` returning `const std::unordered_map<i32,pbr::MaterialHandle>&`.
3. `releaseProjectMeshes(rhi::IDevice&)` (proposedApi #3) — needed for the editor's project-switch flow (`applyProject` calls `releaseProjectMeshes(e)` before every reload); the runtime has no reload path today so never calls it, but the method must exist and be correct (fixing E's current leak) for the editor to depend on it.
4. `warnDeadMaterialHandle`-equivalent — only becomes necessary once (3) is wired up on the runtime side too (i.e., if/when a runtime reload path is ever added); not urgent for this slice, but should ride along with whichever commit adds runtime-side `releaseProjectMeshes` calls, since that's what makes stale handles reachable.
5. Confirm whether `meshPathById_`'s Content-Browser/foliage-palette use can be fully served by the already-public `pathFor`/`pathsWithExtension` (proposedApi #5) — if yes, no new hook needed there at all, just a call-site change in the editor.
None of the above require any `#if EDITOR`/`#if AVER_SANDBOX`-style branch inside GameContent itself — every one is a plain additional public method the runtime binary simply never calls, consistent with the stated architecture rule.

### Risks and what to check

Ranked by how directly they'd change what the editor renders if GameContent were adopted today, as-is:

1. **HIGH — multi-material mesh split missing (loadProjectMeshes divergence 1).** Any project mesh with >1 material slot (the editor's own measured example: 16/53 foliage meshes in one test set) would silently flatten to a single material across the whole mesh the moment the editor's mesh registry is swapped for GameContent without proposedApi item 2 landing first. Hand-check: load a known multi-slot mesh (search the editor's own asset set for one with `md.submeshes.size()>1`, or author one — e.g. a two-material prop) in the editor before and after the swap; visually compare per-submesh materials. This must land in the SAME commit/PR as any commit that actually deletes the editor's own `meshParts_`/`buildMeshParts` — never ship a window where the editor has stopped using its own split path but GameContent doesn't have one yet.

2. **HIGH — material graphs (GRAPHREF) silently downgrade.** Any .ocmat with a node-graph shader renders correctly today only because the editor still calls its own `materialForSurface`; the moment it's rerouted through unmodified `GameContent::materialForSurface`, every graph-backed material goes stock-shaded with zero log line (resolveMaterialGraph's failure path DOES warn, but simply never calling it warns about nothing). Hand-check: open any project with at least one GRAPHREF-bearing .ocmat (search Content or Binaries for OCMAT files with a GRAPHREF field, or the material graph editor's own test content) before/after, compare rendered appearance. Must land with proposedApi item 1 before or in the same change as any editor mesh/material-registry swap.

3. **MEDIUM — GPU mesh-handle leak carried forward if `releaseProjectMeshes` is naively ported.** If whoever writes GameContent's new `releaseProjectMeshes` copies E's current body (destroys only `meshParts_`/`clusterCutCache_` handles, never the base `sceneMeshes_[id]` handle), every future project-switch in the unified editor leaks one GPU mesh per non-split asset — same bug that exists in the editor TODAY, just newly load-bearing once GameContent's version becomes the single implementation both hosts share. Hand-check: switch projects several times in the editor (or a to-be-written one in the runtime, once it gains reload) and watch for growing mesh-resource counts via whatever GPU diagnostic exists (`aver_gates`/`aver_inspect_image` are agent tools I am not to run per standing rules — flag for the user to check with whatever RHI resource-count diagnostic already exists, e.g. a debug HUD or PIX capture).

4. **LOW — LANDSCAPE guard mismatch on `meshDefaultMaterial`/`buildMeshParts`** (editor gates both behind `AVER_MODULE_LANDSCAPE`, GameContent's port should not) could change which fallback applies in a `SCENE=ON, PBR=ON, LANDSCAPE=OFF` configuration — I did not verify whether that module combination is ever actually built/shipped; worth a 30-second CMake-dependency check before assuming it's unreachable.

5. **LOW/COSMETIC — mis-encoded em-dash in one editor log string** (resolveMaterialTexture's fallback warning). Zero behavioral effect, log text only; fix opportunistically.

6. **NONE — level-load material binding.** Already unified via `aver::world::instantiate`'s shared `opt.bindMaterial` callback (modules/world), called identically from `sandbox/src/SandboxLevelLoad.cpp:1053-1058` and `Runtime/src/GameLevel.cpp:42-49`. No action needed here; flagged only so the implementing engineer doesn't waste time re-reconciling something already done.

7. **NONE — resolveAssetPath, resolveAnimAsset, resolveSceneMesh, loadProjectParticleEffects, resolveMaterialTexture (modulo the cosmetic dash), the whole Voxi-attach sequence's shared core.** Verified line-for-line matching; safe to treat as already-reconciled reference implementations.

### Effort and commit breakdown

Medium slice, roughly 4 focused commits, sized to land independently and gate-verifiable one at a time (though per standing project rules the implementing engineer should have the user run any gates/tests, not run them unsupervised):

1. Port `resolveMaterialGraph` into `GameContent::materialForSurface` (GameContent.cpp), PBR-guarded. Small, isolated, no editor-side change needed yet since the editor keeps calling its own copy until it's actually rerouted. ~30-60 min of work, low review risk given it's a near-verbatim port of already-working code.

2. Add `MeshPart`/`meshParts_`/split-building to `GameContent::loadProjectMeshes`, plus a parts accessor, plus wiring `Runtime/src/GameRender.cpp`'s `drawMesh` call to pass parts through (mirroring SandboxRender.cpp's existing call sites). Medium size — touches GameContent.hpp/.cpp and GameRender.cpp; the main risk is verifying `IDevice::drawMesh`'s parts overload behaves identically when driven from the runtime's simpler draw walk (no depth-prepass exclusion list, no cluster-dispatch interplay the editor's version has to route around) — read `rhi::IDevice::drawMesh`'s contract carefully before wiring this, it wasn't in this slice's file list so I did not verify its parts-handling internals.

3. Write `GameContent::releaseProjectMeshes(rhi::IDevice&)` correctly (destroy every handle, including the base one E currently leaks), plus the eager-preload hook (`preloadProjectMaterials`) and the `surfaceMaterials_` read accessor for editor UI. Medium size, mostly additive, no existing behavior to preserve incorrectly.

4. (Separate, larger, explicitly NOT part of this slice's recommended scope) Port `ensureLodMeshPipeline`/virtualized geometry to the runtime, if/when that's prioritized — flagged here only so it isn't silently dropped from the overall reconciliation backlog.

Commits 1-3 are the minimum needed before the editor could safely delete its own `SandboxAssets.cpp` content/mesh/material code and call `GameContent` instead, per the stated end goal. Doing the swap itself (deleting SandboxApp's private members, rewiring every call site listed under "editorKeeps"/"hooksNeeded" above) is a further, separate effort this slice's map is meant to feed into, not part of the map itself.

---

## C3 (project apply + render settings) and C5 (physics init, tick groups, audio init)

**Scope as read:** C3 (project apply + render settings) and C5 (physics init, tick groups, audio init). Editor reference: sandbox/src/SandboxProject.cpp (applyProject, applyProjectVoxiSettings, applyProjectRenderSettings, frameBudgetTick) plus physics/audio/synapse/control-rig init and tick-group ordering inline in sandbox/src/SandboxApp.cpp::onInit/onUpdate. Runtime: Runtime/src/GameApp.cpp (openProject, attachVoxi, applyProjectRenderSettings, initPhysics, tickGameplay, drivePlayCamera, onInit, onUpdate). Shared today: modules/render.voxi/include/aver/voxi/ProjectRenderApply.hpp (applyManifestTiers/Knobs/TwoPhase — already de-duplicated, both hosts call it correctly).

### Reconcile map

##### 1. applyProject / openProject (top-level project-apply orchestrator)
- Editor: `SandboxApp::applyProject(Engine&)` — SandboxProject.cpp:8-105.
- Runtime: `GameApp::openProject(Engine&)` — GameApp.cpp:961-1039 (plus `attachVoxi`/`attachSkinning`/`attachParticles`/`initPhysics`/audio-init already run earlier in `onInit`, GameApp.cpp:1690-1711, before `openProject` is called at :1741).
- Order load-bearing in both: content index → meshes → level, but Editor also eagerly `loadProjectMaterials()` (SandboxProject.cpp:46-51, `#if AVER_MODULE_PBR`); Runtime deliberately has **no** eager material load — materials resolve lazily per-surface through `materialForSurface` during level load (GAME-LIFT.md's 2026-09-13 correction, confirmed intentional, not a gap).
- Editor-only steps inside applyProject with no Runtime analogue at all (must become hooks or stay dropped, see "hooks" section): `LoadingScreen`/`loading.stage(...)` progress UI (SandboxProject.cpp:22-36,47,53,58,65,69,73); `editor::setActorEditorContentRoot`/`setAnimEditorContentRoot` (:33,35); `pendingUpgrade_`/`editor::inspectProject` upgrade-offer flow (:39-43); `startContentWatch()` (:38); `e.window()->setTitle(...)` from `project_.name` on every (re)open (:44-45) — Runtime instead pre-scans the manifest once in `GameApp::config()` (GameApp.cpp:485-522) before the window exists, since a game's title must be known pre-boot; these solve different problems and neither needs the other's code.
- `applyProjectRenderSettings()` call site: Editor calls it from inside `applyProject`, before meshes (SandboxProject.cpp:37); Runtime calls it from inside `openProject`, **after** level load (GameApp.cpp:1028). Immaterial for Voxi settings (they don't depend on level state) but material for **ordering relative to Voxi attach**, see #3 below.
- Scripting/graph-class declaration: Editor does it inside `applyProject` itself (SandboxProject.cpp:72-104); Runtime does it in `onInit`, **after** `openProject` returns (GameApp.cpp:1806-1825, `initScripting()`/`discoverProjectGraphs()`) and spawns class placements even later (`level_.spawnClassPlacements()`, GameApp.cpp:1831, `#if AVER_MODULE_FRAMEWORK`). Net relative order (graph classes declared before class placements spawn) is preserved in both — just split across different functions. Not a defect; note for whoever writes the shared boundary that Runtime's separation (open vs. script-init as two calls) is the shape to keep, not Editor's monolith.

##### 2. applyProjectVoxiSettings (pre-init manifest seed into voxi::Settings)
- Editor: `SandboxApp::applyProjectVoxiSettings()` — SandboxProject.cpp:122-147, thin two-phase wrapper via the shared `voxi::applyManifestTwoPhase` (ProjectRenderApply.hpp:166-172). Called both from inside `applyProjectRenderSettings` (:186, "idempotent... mid-session path") and, per its own header comment, from Editor's own pre-`VoxiRenderer::init()` startup block (mirrors GameApp's inline seed, not literally shared).
- Runtime: **no separate named function** — the identical two-phase seed-before-init logic is inlined directly in `GameApp::attachVoxi(Engine&)` (GameApp.cpp:746-766), reading a local `fmt::ProjectDesc seed` from a manifest peek rather than `project_` (not yet populated at that point — correct, since `attachVoxi` runs before `openProject`). Same effect, duplicated code shape rather than a shared function. Low risk but worth collapsing: both call sites do "load/peek manifest → `voxi::Settings x = vx.settings(); applyManifestTwoPhase(seed, x, [&]{ vx.setSettings(x); x = vx.settings(); });`" — a candidate for one shared `seedVoxiSettingsFromManifest(const fmt::ProjectDesc&)` helper.

##### 3. applyProjectRenderSettings — CLI-override precedence engine (N7 "Phase A/B")
- Editor: SandboxProject.cpp:149-512, entirely inside `#if AVER_MODULE_VOXI` (opened :107, closes :514). Shape: (a) mirrors `projectBackend_`/`frameBudgetMs_`/`averSrProjectDefault_` unconditionally (:163-177, before any guard — "mirrors, not a delta"); (b) early-return on `!hasRenderSettings()` (:179); (c) early-return/defer on `!voxiAttached_` via `projectRenderPending_`, re-driven from `SandboxApp.cpp:1409` once Voxi finishes `init()` later in the **same** `onInit` (:180-181); (d) re-seed via `applyProjectVoxiSettings()` (:186, idempotent); (e) LOD-select/occlusion-cull/depth-prepass non-Voxi-Settings flags (:193-201); (f) **Phase A** — tier CLI overrides (`--gi/--rt/--pt`) + force-offs (`--no-gi/--no-rt`) committed alone (:268-308); (g) **Phase B** — every other knob CLI override (`--msaa`, `--rt-rays`, `--rt-render-mode`, `--refraction`, `--rt-shadow-denoise`, `--pt-bounces`, `--layered-bsdf`, `--gi-sky-occlusion-rays/tile`, `--gi-intensity`, `--rt-pixels-per-ray`, `--gi-update-interval`, `--gi-mode`, `--restir-visibility`, `--denoiser`) against the **post-Phase-A** settings (:310-404); (h) A2 self-contradiction check — RTRENDERMODE=1 *and* PATHTRACING both stated, `PtRtConflict`/`checkPtRtConflict` (:406-452); (i) manifest-contradiction WARN report, 4 fields via `voxi::manifestContradictions` (:454-478); (j) **PHYSICS.GRAVITY/FIXEDSTEP** apply, gated `aver_phys_ready()` (:209-221); (k) **AUDIO.MASTER/BUS** apply (:230-235); (l) PtSceneView want-enabled reconciliation with its own CLI precedence (:494-510).
- Runtime: `GameApp::applyProjectRenderSettings()` — GameApp.cpp:1427-1476, entirely inside `#if AVER_MODULE_VOXI`. Has **only**: early-return on `!project_.valid() || !hasRenderSettings()` and `!voxiAttached_` (:1429-1430, but see below — this branch is structurally unreachable in practice); the shared two-phase manifest apply (:1441-1446, same header as Editor — already correctly de-duplicated); the manifest-contradiction WARN report (:1452-1474, same 4 fields, near-identical logic, cosmetic log-text diff: no `project_.manifestPath` printed, and phrasing differs — `"[Project] RENDER.{} asked for {} but it is not in effect: {}"` vs Editor's `"[Project] {} sets RENDER.{} {}, but {} The recorded value is not what is actually running."`).
- **Confirmed missing from Runtime, not present anywhere in Runtime/**: (i) ALL ~20 CLI-override fields/Phase-A/B logic — `GameApp::parseArgs` (GameApp.cpp:401-470) has none of `--gi/--rt/--pt/--no-gi/--no-rt/--msaa/--rt-rays/--rt-render-mode/--refraction/--rt-shadow-denoise/--pt-bounces/--layered-bsdf/--gi-sky-occlusion-*/--gi-intensity/--rt-pixels-per-ray/--gi-update-interval/--gi-mode/--restir-visibility/--denoiser`; (ii) PHYSICS.GRAVITY/FIXEDSTEP apply — confirmed via repo-wide grep, zero occurrences of `aver_phys_set_gravity`/`aver_phys_set_fixed_step` anywhere under `Runtime/`; (iii) RENDER.GIVOLUME — confirmed via grep, `project_.hasGiVolume`/`giCenter`/`giExtent` never read in `Runtime/`; GameApp instead **always** auto-fits via `fitGiVolumeToLevel()` (GameApp.cpp:1415-1425, called from `openProject` at :1025, before `applyProjectRenderSettings` at :1028) — an author's explicit `RENDER.GIVOLUME` placement is silently discarded in the shipped build even though ordering would let it win if applied. (iv) A2/PtRtConflict check and PtSceneView reconciliation — **confirmed intentionally absent, correct**: `modules/render.pt/include/aver/pt/PtSceneView.hpp:11` states outright "a reference view, not a render mode" — an opt-in diagnostic accumulator (`--pt-scene`/editor Quality combo) for validating GI convergence, not something a shipped game renders. Do not lift.
- `hasRenderSettings()`/`voxiAttached_` double-gate structural note: Editor needs the defer/retry (`projectRenderPending_`) because Voxi attaches **after** `applyProject` is first called in `onInit` (Voxi-attach block is later in the same function, ~SandboxApp.cpp:1300-1410, re-driving `applyProjectRenderSettings()` at :1409 once `voxiRenderer_.init()` succeeds). Runtime's `onInit` calls `attachVoxi(e)` (GameApp.cpp:1691) **before** `openProject(e)` (:1741), so by the time `applyProjectRenderSettings()` runs, `voxiAttached_` is already true — the `if (!voxiAttached_) return;` branch at GameApp.cpp:1430 is dead in practice. **Keep this ordering; do not import Editor's defer/retry mechanism into the shared function** — it exists only because of Editor's own onInit ordering, and Runtime's ordering is simpler and already correct.
- **Guard bug in the reference (Editor) itself**: because physics-gravity and audio-mix application live *inside* `applyProjectRenderSettings`, which is wholly `#if AVER_MODULE_VOXI`, an Editor built with `AVER_MODULE_VOXI=0` never applies `PHYSICS.GRAVITY`/`FIXEDSTEP`/`AUDIO.MASTER`/`AUDIO.BUS` either — despite none of the four having anything to do with Voxi. Same function's `hasRenderSettings()` early-return (:179) means a project stating **only** `PHYSICS.*`/`AUDIO.*` keys and no `RENDER.*` key never gets them applied in the Editor at all. **Runtime's audio path is already correctly decoupled** (see #5 below) — on this one point Runtime is *more* correct than the reference, and "editor is the reference" should be treated as **do not silently copy this coupling**, flag to the task owner.

##### 4. frameBudgetTick (adaptive-quality controller under a frame-time budget)
- Editor: `SandboxApp::frameBudgetTick(f32 dt, voxi::Settings& vs)` — SandboxProject.cpp:516-574. Pure function of `dt`+`Settings&` plus 8 `SandboxApp` members (`frameBudgetMs_`, `frameBudgetForced_`, `frameBudgetRung_`, `frameBudgetAvgMs_`, `frameBudgetFrames_`, `frameBudgetUnder_`, `frameBudgetAppliedInterval_`, `frameBudgetAppliedCones_`; declared SandboxApp.hpp:1796-1809, `kFrameBudgetRungs=5`). Called every frame from `SandboxApp::onUpdate` (SandboxApp.cpp:2799) on a **copy** of `voxi::Renderer::get().settings()` (:2798, "the controller must never write back into the singleton"), then `voxiRenderer_.setSettings(vs)` (:2801) — never mutates `vx.settings()` itself. 30-frame warm-up, 250ms hitch rejection, EMA(0.9/0.1), 15%-band hysteresis (drop immediately, climb only after 60 comfortable frames), 5-rung ladder over `giUpdateInterval`/`giCones` (SandboxProject.cpp:560-561).
- Runtime: **missing entirely.** No members on `GameApp`/`GameConfig` (confirmed: no `frameBudget*` symbol anywhere in Runtime/include or Runtime/src), no per-frame call in `onUpdate`. `RENDER.FRAMEBUDGETMS` is parsed into `project_.frameBudgetMs` (OcProject.hpp:121) and then never read by Runtime/. A project authored to throttle GI cost under load runs at full authored quality forever once shipped.
- No ImGui/editor dependency in the function body itself — ideal candidate for a straight lift into a shared header/struct (see proposedApi), with Editor's `frameBudgetForced_`/`--frame-budget` "only run in a bounded capture if forced" rule (:520-521) staying Editor-only (a shipped game has no `--frames`-bounded measurement mode to protect, so it would just always run the controller).

##### 5. Audio init and manifest apply
- Init: Editor `SandboxApp::onInit`, SandboxApp.cpp:653-664 (`#if AVER_WITH_AUDIO_ABI`, `aver_audio_init()`, INFO log, "0 is not an error" comment) vs Runtime `GameApp::onInit` inline block, GameApp.cpp:1695-1711 — **same guard, same call, same log shape, same "0 is not an error" reasoning verbatim.** Already reconciled; only cosmetic difference is Runtime doesn't factor it into a named `initAudio()` method the way it has `initPhysics()` (GameApp.cpp:526-540) — worth normalizing for symmetry while lifting.
- Ordering: both run audio init after physics init, before the project opens (Editor: :653 after :642-651, before `applyProject` at :753; Runtime: :1695 after `initPhysics()` at :1694, before `openProject` at :1741). Consistent.
- Per-frame `aver_audio_collect()`: Editor calls it unconditionally every frame inside the `AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_FRAMEWORK` guard block but explicitly **not** gated on play state (SandboxApp.cpp:2732-2737, "Not gated on Play: voices outlive a session"); Runtime calls it unconditionally at the very top of `onUpdate` (GameApp.cpp:1863-1869). Same effect, different position in the frame; harmless, no ordering dependency either way.
- **AUDIO.MASTER/BUS apply**: Editor applies it *inside* `applyProjectRenderSettings` (SandboxProject.cpp:230-235), so it inherits that function's `#if AVER_MODULE_VOXI`/`hasRenderSettings()`/`voxiAttached_` gates even though it has nothing to do with rendering (see the guard-bug note in #3). **Runtime applies it correctly decoupled**, directly inside `openProject`, gated only on `#if AVER_WITH_AUDIO_ABI` and `project_.hasAudioMix` (GameApp.cpp:1029-1038) — no Voxi/hasRenderSettings coupling at all. **This is the one place in the slice where Runtime is right and Editor (the nominal reference) has the bug** — recommend fixing Editor to match Runtime's decoupling rather than porting Editor's coupling into a shared function.

##### 6. Physics init
- Editor: SandboxApp.cpp:642-651 (`#if AVER_MODULE_PHYSICS`, `aver_phys_init()`, INFO/WARN, "NO IMPLICIT GROUND... a level supplies its own collision now").
- Runtime: `GameApp::initPhysics()` — GameApp.cpp:526-540, **near-identical code and comment text** ("NO IMPLICIT GROUND", same rationale, same log shape). Already well reconciled.
- Ordering: both run physics init before the project opens/level loads (Editor :646 before `applyProject` :753; Runtime :1694 before `openProject` :1741) — load-bearing in both, since level load adds a static body per colliding placement gated on `aver_phys_ready()`.
- **Shared, pre-existing gap (not a divergence)**: `PHYSICS.MAXBODIES/MAXBODYPAIRS/MAXCONTACTS/TEMPALLOCMB` are parsed and serialized (`OcProject.hpp:247-256`, `hasPhysicsSettings()`) but `aver_phys_init(void)` takes **zero** parameters (`modules/physics/include/aver/physics/physics_abi.h:24`) and there is no `aver_phys_set_max_*` setter anywhere in the tree (confirmed via repo-wide grep — every call site across `tests/physics/**`, `Sandbox`, and `Runtime` calls the bare, parameterless `aver_phys_init()`). Neither host has ever honored these four keys. Out of scope for a same-behavior reconcile (there is no Editor code to "port"), but flag explicitly so it isn't assumed closed by this slice.

##### 7. Tick groups (PRE_PHYSICS → physics step → PHYSICS → POST_PHYSICS)
- Editor: SandboxApp.cpp:2437-2445, gate `!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING`.
- Runtime: `GameApp::tickGameplay(f32 dt)` — GameApp.cpp:542-567, gate `aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;` — the **same** gate minus the `spawnTestClass_` CLI-harness term, which Runtime's own header comment (GameApp.hpp:not shown, GameApp.cpp:547-548) *correctly and explicitly* documents as an intentional drop ("The editor widens this gate with a --spawn-test term; that is a CLI harness and has no place in a game"). Tick order is byte-identical in both: `AVER_FW_TICK_PRE_PHYSICS` → `aver_phys_step` → `AVER_FW_TICK_PHYSICS` → `AVER_FW_TICK_POST_PHYSICS`, and both hosts carry the same correcting note that the step sits between PRE_PHYSICS and PHYSICS (not after PHYSICS, despite an adjacent comment's wording). **This piece is already well reconciled** — no functional work needed, just confirm byte-for-byte during implementation.
- **CONFIRMED DIVERGENCE (new finding, not previously documented anywhere I found), squarely in this slice's scope**: Synapse AI ticking (`agentSystem`/`perceptionSystem`/`btSystem`) is gated on the **same** PLAYING-or-spawnTestClass_ condition as the physics tick group in the Editor (SandboxApp.cpp:2739, `if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) { synapse::agentSystem().tick(...); synapse::perceptionSystem().tick(...); synapse::btSystem().tick(...); }`), but is **completely ungated** in Runtime (GameApp.cpp:1966-1977, inside `#if AVER_MODULE_SYNAPSE_SCENE`, right after `scene::World::instance().flush()`, with no play-state check at all). Concretely: for a project that declares no `GameMode` (`aver_fw_play_state()` then never leaves `AVER_FW_PLAY_EDITOR`, per `GameApp::beginPlayIfGameModeDeclared`'s own header comment, GameApp.hpp), `tickGameplay`/`drivePlayCamera` correctly stay inert in the shipped build, but Synapse agents/perception/behaviour-trees will **still run every frame** against a "not playing" world — an inconsistency the Editor does not have. This needs an explicit fix-or-keep decision, not a silent port.
- `anim::animSystem().tick` / `particles::particleSystem().tick` / `skinnedScene_->update` / `World::flush()` ordering: byte-identical order in both (anim tick → particle tick → skinned update → flush → [synapse] → drivePlayCamera), confirmed via direct grep of both files' line numbers; anim ticks **unconditionally** (not gated on Play) in both, "deliberate asymmetry, copied from the editor" per Runtime's own comment. No divergence.
- `drivePlayCamera()`: `SandboxApp.cpp` (~:2264+, :2752 call site) vs `GameApp::drivePlayCamera()` (GameApp.cpp:569-608). Both gate on PLAYING, run after flush, read the pawn via `aver_fw_controlled_pawn(aver_fw_player_controller(0))`, prefer the view entity over the pawn. Shapes and reasoning match closely from what was read; recommend a line-by-line diff during implementation rather than assuming full parity — lower priority/risk than the items above.
- `beginPlayIfGameModeDeclared()` (GameApp.cpp:1312-1345): Runtime auto-begins play at boot when a project declares a `[AverGameMode]` subclass ("a shipped game has no editor Play button, so booting it IS beginning play" — GameApp.cpp:1333-1334). Editor has no equivalent — Play is a manual, UI-triggered action (`startPlay()`, referenced SandboxApp.cpp:2387 `keybinds_.pressed(...PlayStart...)`). This is a **correct, deliberate, already-documented divergence** — Editor keeps its manual trigger; no hook needed, note only for completeness since it's the moment C5's ordering (physics/audio/synapse/control-rig init → project open → scripting → framework tick) actually starts mattering behaviorally.

### Proposed library API

Extend the existing header-only, pure-function pattern from `modules/render.voxi/include/aver/voxi/ProjectRenderApply.hpp` rather than inventing a new shape — it already proves the model (no `AVER_WARN`, no singleton access, unit-testable in `tests/formats/src/ProjectRenderApplyTest.cpp`).

1. **CLI-override precedence** (extend `ProjectRenderApply.hpp`, since it's Voxi-`Settings`-specific):
```cpp
// Collapses SandboxProject.cpp's ~20 *Override_ members into one struct; Editor populates it
// from argv, Runtime default-constructs it empty (every field's un-set sentinel already matches
// SandboxApp's own: -1 for tiers/knobs, false for force-offs, 0 for "flag not given" ints).
struct RenderCliOverrides {
    int gi=-1, rt=-1, pt=-1; bool giForceOff=false, rtForceOff=false;
    int msaa=0, rtRays=0, rtRenderMode=-1, refraction=-1, rtShadowDenoise=-1, ptBounces=-1,
        layeredBsdf=-1, giSkyOccRays=-1, giSkyOccTile=-1, rtPixelsPerRay=0, giUpdateInterval=0,
        giMode=-1, restirVisibility=-1, denoiser=-1;
    f32 giIntensity=-1.0f;
};
// Phase A (tiers+force-offs, committed alone) then Phase B (every knob, against post-tier
// settings) — the exact two-commit split SandboxProject.cpp:268-404 already proves necessary
// (N7's own regression history). `warn(name, cliValue, manifestValue)` is a caller-supplied sink
// so log prefixes ([Sandbox] vs [Game]) stay host-specific with no #ifdef inside this function.
bool applyCliOverrides(const RenderCliOverrides&, Settings&,
                        void (*warn)(const char* name, int cliValue, int manifestValue, void* user), void* user);
```
2. **Physics/audio manifest apply** — new sibling header, NOT under render.voxi (fixes the Editor's own `#if AVER_MODULE_VOXI` mis-guard by construction): e.g. `modules/game/include/aver/game/ProjectSubsystemApply.hpp` or beside `physics_abi.h`/`audio_abi.h`:
```cpp
// PHYSICS.GRAVITY / PHYSICS.FIXEDSTEP. Internally gated on aver_phys_ready() (SandboxProject.cpp:210);
// returns false (caller may log "will apply once the world exists") when physics isn't up yet.
bool applyProjectPhysicsSettings(const fmt::ProjectDesc&);
// AUDIO.MASTER / AUDIO.BUS. No physics-readiness-style gate needed: every audio_abi.h setter is
// already a documented no-op before aver_audio_init() succeeds.
void applyProjectAudioSettings(const fmt::ProjectDesc&);
```
3. **Frame-budget controller** — lift SandboxProject.cpp:516-574 near-verbatim, state passed by reference instead of implicit `this`:
```cpp
struct FrameBudgetState {
    f32 ms=0.0f; bool forced=false; i32 rung=0; f32 avgMs=0.0f;
    u32 frames=0; i32 under=0; u32 appliedInterval=0; u32 appliedCones=0;
};
static constexpr i32 kFrameBudgetRungs = 5;
// `capture` is Editor's maxFrames_!=0 && !forced early-out (SandboxProject.cpp:521); Runtime
// always passes false (no bounded-capture concept to protect) and can drop the flag argument
// entirely at its call site by wrapping with capture=false.
void frameBudgetTick(FrameBudgetState&, f32 dt, bool capture, Settings& vs,
                      void (*log)(const char* fmt, f32 ms, f32 avgMs, i32 rung, u32 interval, u32 cones, void* user),
                      void* user);
```
4. **Voxi pre-init seed** — collapse GameApp::attachVoxi's inline block (GameApp.cpp:746-766) and Editor's own pre-init seed call site into one helper both hosts call:
```cpp
// Loads `manifestPath` (or does nothing if empty/unreadable), two-phase-applies it into
// voxi::Renderer::get(), and returns the resolved MSAA request (-1 if unstated) so a caller's
// own device-derived MSAA fallback (attachVoxi's `if (manifestMsaa < 0) s.msaa = ...`) knows
// whether it's allowed to write.
int seedVoxiSettingsFromManifest(const std::string& manifestPath);
```
Each of these mirrors an existing pattern already in the codebase (GameContent's static-resolver-plus-`void* user` shape for the resolvers; ProjectRenderApply.hpp's pure-function-of-`Settings&` shape for the manifest apply) rather than introducing a new one.

### What the editor keeps

- `LoadingScreen`/`loading.stage(...)` progress UI (SandboxProject.cpp:22-36 and every `loading.stage(...)` call through `applyProject`) — no Runtime equivalent, no user expects a progress bar from `AverEngineRuntime.exe`.
- `editor::setActorEditorContentRoot`/`setAnimEditorContentRoot` (SandboxProject.cpp:33,35) — Actor/Anim editor tab content roots, pure editor chrome.
- `pendingUpgrade_`/`upgradeAsked_`/`editor::inspectProject` upgrade-offer flow (SandboxProject.cpp:39-43) — a shipped game's manifest is always current (generated by `stage-game.ps1` at package time); this whole flow is meaningless outside authoring.
- `startContentWatch()` (SandboxProject.cpp:38) — hot-reload file watching, authoring-only.
- `e.window()->setTitle(...)` from `project_.name` on every (re)open (SandboxProject.cpp:44-45) — Runtime already solves the "title must be known before the window exists" problem differently via `GameApp::config()`'s pre-scan (GameApp.cpp:485-522); no shared code needed, they're different mechanisms for different constraints.
- `voxiRenderer_.setFrameTimeReport(true)`, `--rd-ablate`, `--rt-denoise-motion` and the whole neighboring "gate/measurement CLI dial" family in Editor's Voxi-attach block (SandboxApp.cpp:1388-1405) — no manifest key, no shipped-game need.
- The 20-odd CLI-override *parsing* (argv → the proposed `RenderCliOverrides` struct) stays in `SandboxMain`/`SandboxApp`'s own arg parser; only the *apply* logic (Phase A/B `take()`) is proposed to move to a shared function Runtime's own (empty) struct also happens to satisfy.
- PtSceneView + the A2 self-contradiction check (`checkPtRtConflict`, `ptSceneViewFromCli_`/`ptSceneViewWantEnabled_`/`ptSceneViewSuppressedByRayDriven_`) — confirmed via `PtSceneView.hpp`'s own header comment to be a diagnostic reference-render accumulator, "not a render mode." Stays 100% in the editor; do not lift any part of it.
- `projectBackend_` (SandboxApp.hpp:3676) — pure Project-Settings "Renderer" dropdown backing field, written back to the manifest on save (SandboxLevelEdit.cpp:119); confirmed via grep to have zero effect on actual backend selection (that happens at boot from `--backend`/BootConfig on both hosts, independent of the live project). No hook needed — the shared apply function need not even accept a backend parameter.
- `averSrProjectDefault_` mirror (SandboxApp.hpp:3615) — same shape as `projectBackend_`; Runtime reads `project_.averSr` directly at its own AverSR-resolve call site (GameApp.cpp:1753) instead of keeping a mirror. No hook needed.
- `frameBudgetForced_`/`--frame-budget` and its "only run the controller during a bounded capture if forced" rule (SandboxProject.cpp:520-521) — Editor-only measurement-parity concern; Runtime has no `--frames`-bounded capture mode whose numbers this would otherwise pollute, so it would just always run the controller (`capture=false` at its call site, per the proposed API).
- Manual Play trigger (`startPlay()`, keybind-driven) vs. Runtime's automatic `beginPlayIfGameModeDeclared()` (GameApp.cpp:1312-1345) — both are correct and intentional for their host; no hook needed, already reconciled by design.

### Hooks needed

- **Log sink, not `AVER_WARN` calls baked into shared code.** Every proposed shared function (`applyCliOverrides`, `applyProjectPhysicsSettings`/`applyProjectAudioSettings`, `frameBudgetTick`) should take a caller-supplied function-pointer sink (`void(*)(...); void* user`) so `[Sandbox]`/`[Project]` vs `[Game]`/`[Project]` log prefixes stay host-specific — matches the existing `GameContent`/`GameLevel` resolver pattern (static function + `void* user`), not a new idiom.
- **"Physics/audio ready" signal, not a hard dependency on call order.** `applyProjectPhysicsSettings` needs to internally check `aver_phys_ready()` exactly as SandboxProject.cpp:210 does and return a bool the caller can log against ("will apply once the world exists") — this keeps both hosts safe regardless of whether physics init happened to fail, without the shared function needing to know which host is calling.
- **No hook needed for content-root/anim-root/upgrade-offer/content-watch/LoadingScreen** — these stay entirely inside `SandboxApp::applyProject`, called around (not through) the shared library functions; the shared functions themselves must not grow parameters for any of them.
- **No hook needed for PtSceneView/A2 conflict** — confirmed diagnostic-only, must not enter the shared function's signature at all (not even as an optional/no-op parameter for Runtime).
- **A "fix Synapse tick gate" decision, not a hook** — this is a straight bug-parity fix (add the missing `aver_fw_play_state() == AVER_FW_PLAY_PLAYING` check around GameApp.cpp:1966-1977), no editor-only concept is being separated out here, so no hook is needed — just get explicit sign-off since it's a shipped-behavior change (see risks).
- **SETTLED 2026-09-19 -- the editor was fixed; `applyProjectPhysics`/`applyProjectAudioMix` now
  run from `SandboxApp::applyProject`, outside every Voxi and `hasRenderSettings()` gate, matching
  the runtime.** The original note, kept for the reasoning: a "fix Editor's physics/audio
  Voxi-coupling" decision, not a hook — same shape: this is Editor's own bug (physics-gravity and audio-mix trapped inside `#if AVER_MODULE_VOXI` and behind `hasRenderSettings()`), not something the runtime needs a hook to avoid inheriting, since Runtime's audio path is already correctly decoupled. Recommend fixing Editor to call the new `applyProjectPhysicsSettings`/`applyProjectAudioSettings` unconditionally (outside `applyProjectRenderSettings`'s Voxi/hasRenderSettings gates) rather than threading a hook through the old structure.

### Risks and what to check

1. **Lifting `frameBudgetTick` changes Runtime's steady-state behavior under load for the first time ever** (today: none — quality is always exactly as authored). Hand-check: run `AverEngineRuntime.exe` on a heavy scene with `RENDER.FRAMEBUDGETMS` set; confirm `giUpdateInterval`/`giCones` visibly drop under load and recover after ~60 comfortable frames, matching an Editor Play session at the same settings and scene. Should move zero `Sandbox.exe` pixels (Editor's own controller already exists and is untouched by the extraction if done as a pure refactor).
2. **Applying `PHYSICS.GRAVITY`/`FIXEDSTEP` in Runtime for the first time changes physics behavior** in any shipped/tested project that already states one (previously silently ignored — default Earth gravity ran regardless). Hand-check: grep existing test/demo `.ocproject` files for `PHYSICS.GRAVITY`/`PHYSICS.FIXEDSTEP` and confirm none were accidentally depending on the old (wrong) always-default behavior once fixed.
3. **Fixing the Synapse AI tick gate (adding the missing PLAYING check to `GameApp::onUpdate`) changes shipped behavior** for any project using Synapse agents with no declared `GameMode`. Hand-check: build a minimal no-GameMode project with a Synapse agent, confirm it now correctly freezes in the shipped build instead of silently pathing against a "not playing" world, matching Editor's non-Play state.
4. **SETTLED 2026-09-19: fixed, in its own commit.** The original note: deciding to fix vs. preserve Editor's own `#if AVER_MODULE_VOXI`/`hasRenderSettings()` coupling around physics-gravity and audio-mix is a product decision, not a mechanical port — "editor is the reference" is ambiguous here because the reference itself has the bug. Flag to the task owner before silently changing Editor behavior as a side effect of this slice; at minimum, land it as its own isolated, clearly-labeled commit (see effort) so it can be reviewed/reverted independently of the pure lift work.
5. **Applying `RENDER.GIVOLUME` in Runtime (previously silently discarded in favor of `fitGiVolumeToLevel()`'s auto-fit) will visibly move the GI volume** for any shipped project that explicitly authors one. Hand-check with a screenshot compare (Editor vs. `AverEngineRuntime.exe`, same project, same camera) for at least one project stating `RENDER.GIVOLUME`.
6. **`PHYSICS.MAXBODIES`/`MAXBODYPAIRS`/`MAXCONTACTS`/`TEMPALLOCMB` remain dead in both hosts** without an `aver_phys_init` ABI change (it currently takes zero parameters, and every one of 30+ call sites across `tests/physics/**` assumes that). Explicitly out of scope for this slice; note it so nobody assumes "port the editor's code" closes this — there is no editor code to port.
7. **The CLI-override collapse (`RenderCliOverrides`/`applyCliOverrides`) touches the single most bug-scarred function in the file** — SandboxProject.cpp's own N7 comment documents four separate historical regressions of this exact precedence pattern. Do this as its own isolated, behavior-preserving-only commit, extend `tests/formats/src/ProjectRenderApplyTest.cpp`-style coverage to pin Phase A/B ordering, and run the gates specifically on this commit even though it shouldn't move any `Sandbox.exe` pixel (refactor, not a settings change) — this is the one commit in the slice where "should be a no-op" is worth independently verifying rather than assuming from the diff.
8. **`applyProjectRenderSettings`'s early-return chain (`!valid()`, `!hasRenderSettings()`, `!voxiAttached_`) must be re-derived for the shared function's Runtime call site**, not assumed identical — Runtime's `!voxiAttached_` branch (GameApp.cpp:1430) is dead in practice today (attachVoxi always runs first), but a future change to `onInit`'s ordering could silently resurrect Editor's defer/retry need; leave a comment at the call site naming this invariant explicitly so it isn't lost.
9. **Log-text drift is cosmetic but worth normalizing while touching this code**: Runtime's manifest-contradiction WARN (GameApp.cpp:1471-1473) omits `project_.manifestPath` that Editor's equivalent (SandboxProject.cpp:473-476) includes — harmless for behavior, but a script/human comparing logs between hosts (as `verify-game.ps1`/gates already do for other lines) will see different text for the same condition; align the format string while lifting rather than leaving two spellings.

### Effort and commit breakdown

Medium slice: mostly additive to Runtime (frame-budget controller, physics/audio manifest apply, GI-volume apply, Synapse gate fix), one true refactor (CLI-override collapse) that must be isolated and gate-verified, plus one editor-behavior-change decision that needs owner sign-off before proceeding. Suggested commit breakdown, in dependency order:

1. **Decouple physics/audio manifest apply into the shared header, fix Editor's own `#if AVER_MODULE_VOXI`/`hasRenderSettings()` coupling, wire GameApp to call it.** Touches: SandboxProject.cpp (extract+call), new `ProjectSubsystemApply.hpp`, GameApp.cpp (`openProject`, new `applyProjectPhysicsSettings`/`applyProjectAudioSettings` calls decoupled from Voxi). Medium risk — see risk #2/#4; needs owner sign-off on the Editor-behavior-change piece before landing, could be split into 1a (pure extraction, Editor-only, no behavior change) + 1b (wire Runtime + fix Editor's coupling, behavior change) if the owner wants the risky half isolated further.
2. **Extract `frameBudgetTick` to the shared `FrameBudgetState`/function, wire GameApp's `onUpdate` to call it every frame.** Touches: SandboxProject.cpp (extraction, should be a pure refactor for Editor), new header, GameApp.hpp (+8 members)/.cpp (per-frame call near `pushFrame`). Low-medium risk, additive-only for Runtime.
3. **Fix the Synapse AI tick play-state gate in `GameApp::onUpdate`.** Touches: GameApp.cpp only, ~3-5 lines. Isolated on purpose so it can be reverted independently if the hand-check (risk #3) disagrees with the fix.
4. **Apply `RENDER.GIVOLUME` in `GameApp::openProject`/`applyProjectRenderSettings`, ahead of or instead of `fitGiVolumeToLevel()`'s fallback.** Touches: GameApp.cpp only, small. Isolated, low risk, easy to screenshot-verify (risk #5).
5. **Collapse the ~20 CLI-override members into `RenderCliOverrides`/`applyCliOverrides`, land in the shared header, repoint SandboxProject.cpp's Phase A/B blocks at it.** Editor-only refactor (Runtime doesn't need to populate the struct yet — passes a default-constructed empty one, or nothing, if the API instead makes this call optional). Largest diff by line count (N7's comment density), highest care needed despite being "just a refactor" — do this **last**, once 1-4 have proven the shared-header pattern compiles and links cleanly into both `Aver.Runtime.Game` and `Sandbox`, and get a gate run on this commit specifically.
6. **(Not scoped into this slice, flag only)** `aver_phys_init` ABI change to honor `PHYSICS.MAXBODIES/MAXBODYPAIRS/MAXCONTACTS/TEMPALLOCMB` — touches the physics ABI signature and 30+ test call sites; recommend its own separate slice/task rather than folding into this one.

Each of 2-4 is small (roughly 50-150 line diffs including comments); 1 and 5 are the two that carry real risk and warrant their own review pass and, for 5 specifically, a gate run.

---

## C4 — Level load (editor: sandbox/src/SandboxLevelLoad

**Scope as read:** C4 — Level load (editor: sandbox/src/SandboxLevelLoad.cpp + PlayerStart fns in SandboxPlay.cpp/SandboxViewport.cpp; runtime: Runtime/src/GameLevel.cpp + level/spawn code in Runtime/src/GameApp.cpp). This slice is much bigger than "editor calls an existing library function": roughly half the item list (landscape, water, chunk streaming, Player-Start/SPAWN resolution) has ZERO counterpart in Runtime today — not a divergent copy, an absence. The other half (the placement loop, sky application, class-placement spawn, level teardown, GI-volume fit) is already de-duplicated through modules/world (LevelInstance) and modules/assets (LevelSky), so that part really is "swap the call and delete the copy." I verified every claim below by reading the cited files directly, not from docs/GAME-LIFT.md's history (which describes an earlier, now-superseded AverGame.exe state and says so in its own banner).

### Reconcile map

#### Already shared (safe to swap SandboxApp onto Runtime, low risk)

**Placement loop.** Editor `SandboxApp::loadLevel` (SandboxLevelLoad.cpp:972-1198) and runtime `GameLevel::load` (Runtime/src/GameLevel.cpp:32-208) both call `world::instantiate(w, opt)` (modules/world/include/aver/world/LevelInstance.hpp) for entity/mesh-renderer/material-token/physics-body creation — already ONE implementation, order-preserving, gate-compared. Divergence is only in what each host does with the *result*: editor builds `entityLabels_`/`entityCollide_`/`entitySnapZ_`/`entityBodies_` (undo/label/save bookkeeping, SandboxLevelLoad.cpp:1063-1091); runtime just keeps `levelEntities_`/`levelBodies_` (GameLevel.cpp:55-58). No action needed beyond keeping this shared.

**Sky/sun/fog/clouds.** Editor `SandboxApp::applyLevelSky` (SandboxLevelLoad.cpp:1361-1393) and runtime `GameApp::applyLevelSky` (Runtime/src/GameApp.cpp:1384-1405) both call `assets::applyLevelEnv` (modules/assets/include/aver/assets/LevelSky.hpp:42-105) — one mapping, per-field-guarded internally. Editor unconditionally re-mirrors `sunColor_/skyZenith_/skyHorizon_/fogColor_/fogDensity_/sunAmbient_` from `sky_` after calling it and logs a below-horizon-sun warning; runtime early-returns when no record at all was present (`!hasSun && !hasSky && !hasFog && !hasClouds`, :1387) and mirrors only `sunColor_`/`fogDensity_` (the two it actually keeps as members). Functionally equivalent — `applyLevelEnv` is a no-op on fields whose `has*` flag is false, so the early return skips nothing real. **No risk**, just a smaller mirror set because the runtime keeps fewer duplicate members.

**GI-volume fit math.** Editor `SandboxApp::levelBounds`/`fitGiVolumeTo` (SandboxLevelLoad.cpp:1411-1467) and runtime `GameLevel::placementBounds` (GameLevel.cpp:271-280, called from `GameApp::fitGiVolumeToLevel`, GameApp.cpp:1415-1425) use the **identical** scaled/rotated-corner bounding-box math — the runtime's comment at GameLevel.cpp:143-162 explicitly says it is kept identical to the editor's. Already reconciled.

**spawnClassPlacements.** Editor (SandboxLevelLoad.cpp:1308-1349) and runtime (`GameLevel::spawnClassPlacements`, GameLevel.cpp:224-263) are near-duplicates: same `aver_fw_class_find`/`aver_fw_spawn` calls, same deferred-collection-then-spawn ordering rationale (both hosts load the level before the scripting host declares classes). One real behaviour gap, already called out **in the runtime's own comment** (GameLevel.cpp:231-235): the editor's version ground-snaps a class placement via `landscape::surfaceHeightAt` (SandboxLevelLoad.cpp:1318-1327) when `p.snapToGround`; the runtime's never wires `groundHeightAt` because it has no landscape at all (see below), so a snapped class placement keeps its raw authored Z in a shipped game.

**unloadLevel (the shared part).** Editor `SandboxApp::unloadLevel` (SandboxLevelLoad.cpp:1510-1604) and runtime `GameLevel::unload` (GameLevel.cpp:282-304) agree on: destroy `levelClassInstances_` via `aver_fw_destroy` (not `world.destroy`, so the managed unbind hook fires), clear `classPlacements_`, destroy `levelEntities_`, remove `levelBodies_` physics bodies, clear `pcgFields_`/`env_`. Reconciled for what both sides have.

---

#### Missing entirely on the runtime side (net-new library work, not a swap)

**Legacy `.ocmap` dispatch.** Editor: `SandboxApp::loadLevel` checks `fmt::levelFileIsLegacyOcmap(path)` (SandboxLevelLoad.cpp:981-984) and routes to `SandboxApp::loadLegacyOcmapLevel` (SandboxLevelLoad.cpp:1206-1298), which parses via `fmt::loadOcmap`, synthesizes an `OcWorldData` (translating `OcPlacement`→`OcWorldPlacement`, forcing `collide=true`, preserving `entityLegacyDeform_/entityLegacySurface_/entityLegacyMaterial_` for save round-trip), then calls the same `world::instantiate`. Runtime: **no counterpart at all** — grep of Runtime/ for `levelFileIsLegacyOcmap|loadOcmap|OcMapData` returns nothing. `GameLevel::load` always calls `fmt::loadOcworld` directly (GameLevel.cpp:35-37). Per the editor's own comment, `loadOcworld` "succeeds" on a legacy file's `OCMAP 1` header while silently dropping ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM records — so a shipped game opening a genuinely-legacy map (e.g. OpenConstructor's demoworld.ocmap) loads an incomplete level with no warning. The synth-and-translate block (SandboxLevelLoad.cpp:1219-1251) has no editor-only content (no ImGui, no undo) except the `entityLegacy*_` save-round-trip maps, which a shipped game doesn't need — it can move to `aver::world` or `GameLevel` close to verbatim.

**Landscape — loadLandscape, applyLandscapeToStreaming, rebuildLandscapeCollision, unloadLandscape, applyLandscapeSurface/ToAll, loadLandscapeForLevel, updateLandscapeRingTiles** (SandboxLevelLoad.cpp:28-354, all `#if AVER_MODULE_LANDSCAPE`). Runtime: **zero references to `AVER_MODULE_LANDSCAPE` anywhere in Runtime/**, and `Runtime/CMakeLists.txt` never links `Aver.Landscape` even conditionally (no `if(TARGET Aver.Landscape)` block exists, unlike the PBR/Voxi/Scene/Physics/Framework blocks it does have). This isn't a logic gap, it's a missing link dependency plus ~330 lines of unlifted subsystem: section load, quadtree build, ring-tile streaming around the camera, Jolt heightfield collision, and the material/UV-tiling binding into `pbr::MaterialSystem`. `loadLevel` calls `loadLandscapeForLevel(eng.device(), path, w)` (SandboxLevelLoad.cpp:1036) **unconditionally as part of every level load**, not behind a toggle — so this is a core, always-on part of what "load a level" means in the editor, entirely absent from the shipped game. A project authoring a `LANDSCAPE` record or a `levelname.ocland` sidecar renders **no terrain** in `AverEngineRuntime.exe`.

**Water — applyLevelWater** (SandboxLevelLoad.cpp:362-563, `#if AVER_MODULE_FLUIDS`). Runtime: no `waterRenderer_`/`fluidScene_`/`fluidHandle_` members anywhere in `Runtime/include/aver/game/GameApp.hpp`, no `AVER_MODULE_FLUIDS` reference in Runtime/, no `Aver.Fluids` link. Also called unconditionally from `loadLevel` (SandboxLevelLoad.cpp:1011, right after `levelHeader_` is populated) whenever `levelHeader_.waters` is non-empty — so again a core, not opt-in, part of level load. Covers both the simulated-softbody path (`AVER_FLUIDS_SIMULATED`) and the analytic Gerstner-wave surface + buoyancy-plane path (`aver_phys_set_water_plane`). A shipped game with an authored `WATER` record has **no water surface and no buoyancy** at all.

**Chunk/PCG streaming — setChunkStreamingEnabled, warnIfCameraOutsideGeneratedBand, residentTriangleCount, anyChunkWorldOwns** (SandboxLevelLoad.cpp:567-822, 946-966). Runtime: `Aver.World` **is** linked (for `LevelInstance`/`quatFromEulerDeg`), but `GameApp.cpp` never constructs a `world::ChunkWorld` or reads `levelPcgVolumes_`/`OcScatterSpecies` for streaming purposes — grep for `ChunkWorld|chunkWorld_|streaming` in Runtime/ returns nothing. Unlike landscape/water, this one genuinely *is* opt-in even in the editor (Window menu item, MCP command, `--chunk-stream N` CLI flag — SandboxShell.cpp:1006, SandboxMcp.cpp:121/131, SandboxSelfTests.cpp:96), so it's lower risk than the other two, but there is currently **no way at all** — no CLI flag, no manifest key — to turn PCG-scattered foliage/props on in a packaged game.

**Player Start / SPAWN resolution — the single highest-value gap in this slice.** Editor: `SandboxApp::playerStartTransform` (SandboxViewport.cpp:671-685) returns the live `PlayerStart` marker entity's transform if one exists, else falls back to the level's `hasSpawn/spawnX/Y/Z/spawnYaw` record; `makePlayerStart`/`addPlayerStart`/`refreshPlayerStart` (SandboxViewport.cpp:657-758) manage the visible marker (undo entry, Outliner selection, re-derivation after undo/redo/delete via `editor::refreshPlayerStart` in sandbox/src/PlayerStartRefresh.hpp); `SandboxApp::placePawnAtPlayerStart` (SandboxPlay.cpp:126-143) is called from `startPlay()` (SandboxPlay.cpp:189, 219) **after** `aver_fw_begin_play` succeeds, and repositions the possessed pawn's local transform to it. Runtime: **none of this exists.** Confirmed three independent ways: (1) `hasSpawn`/`spawnX/Y/Z/spawnYaw` are fields of `OcWorldData` itself, not of the `OcWorldEnv` base `GameLevel::load`'s `env_ = w` (GameLevel.cpp:74) slices off — so the SPAWN record is silently dropped at load, `GameLevel` exposes no accessor for it. (2) `GameApp::beginPlayIfGameModeDeclared` (GameApp.cpp:1312-1345) calls `aver_fw_begin_play` and does nothing else — no equivalent of `placePawnAtPlayerStart` exists anywhere in Runtime/ (grep for `PlayerStart|playerStart` across Runtime/ returns zero matches). (3) `FrameworkAbi.cpp::aver_fw_begin_play` (:948-949) spawns the default pawn with `pos3=quat4=scale3=nullptr`, i.e. at the origin. The intended contract is even documented elsewhere in the tree: `modules/world/include/aver/world/SceneCensus.hpp:13` states as an architectural fact "the game spawns at a PlayerStart" — a comment nothing in Runtime currently makes true. **Every playtest through `AverEngineRuntime.exe` today starts the player at the world origin, regardless of what the level authored or where the editor's own Player Start marker sits.**

**Authored GI-volume guard — a real, standing bug in Runtime, not a lift gap.** Editor only auto-fits the GI volume when `project_.giExtent <= 0.0f` — i.e. when the manifest's `RENDER.GIVOLUME` authored nothing (guard at call sites SandboxLevelLoad.cpp:1189 and :1293; `project_.giExtent` itself is seeded from the manifest at SandboxProject.cpp:145, `giExtent_ = project_.giExtent`). The editor's own comment (SandboxLevelLoad.cpp:1182-1189) documents exactly the regression an unconditional fit causes, measured on PTTest: a real 5659cm authored half-extent silently shrunk to a fitted 2356cm. Runtime: `GameApp::openProject` calls `fitGiVolumeToLevel()` **unconditionally** (GameApp.cpp:1025), and grep confirms `project_.giExtent` is never read anywhere in `Runtime/src/GameApp.cpp` — `giCenter_`/`giExtent_` only ever get the compiled default (GameApp.hpp:346-347) or the fitted value. **This is today's live behaviour in `AverEngineRuntime.exe`, independent of anything else in this slice** — any project with a hand-authored `RENDER.GIVOLUME` renders with the wrong GI volume in the shipped game right now.

**CAMERA-record precedence.** Editor's `loadLevel` has a three-way precedence (`--cam` CLI override > level's `CAMERA` record > `frameCameraOn` auto-fit fallback, SandboxLevelLoad.cpp:1150-1174) plus `frameCameraOn` itself (SandboxLevelLoad.cpp:1473-1485). Runtime has none of this — no `frameCameraOn` counterpart, and `hasCamera/camX/camY/camZ/camYaw/camPitch/camSpeed` are OcWorldData-only fields the same way SPAWN's are, so `GameLevel` cannot see them either. Low priority: a shipped game's camera comes from `drivePlayCamera()` reading the possessed pawn (GameApp.cpp:569-601) once play begins, not from an edit camera — this matters only for a no-GameMode "spectator" fallback, which Runtime also doesn't have (see below), so it's plausibly fine to leave unimplemented, but flag it as a conscious decision rather than an oversight.

**loadStartMap CLI override.** Editor `SandboxApp::loadStartMap` (SandboxLevelLoad.cpp:1487-1507) lets `--open-level <path>` (`openMapPath_`) outrank the project's own start map, with a fallback to the start map if the named file doesn't exist. Runtime `GameLevel::loadStartMap` (GameLevel.cpp:210-221) only ever loads `project.startMap` — `GameConfig` (GameApp.hpp:35-54) has no per-level override field at all. Low priority for a shipped game (which has one level path baked into its manifest) but worth a CLI flag for QA/testing parity with the editor's `--open-level`.

**No-GameMode fallback ("spectator"/drone Play).** Editor's `SandboxApp::startPlay` (SandboxPlay.cpp:145-223) declares an engine-default `AverDefaultPawn`/`AverDefaultController`/`AverDefaultGameMode` triad (`engineDefaultGameMode`, SandboxPlay.cpp:97-116) so an unauthored project is still playable, first-person, spawned via `placePawnAtPlayerStart` with a camera-position fallback. Runtime: grep for `AverDefaultPawn|AverDefaultGameMode|engineDefaultGameMode` in Runtime/ returns nothing; `beginPlayIfGameModeDeclared` just logs a warning and returns when no GameMode is declared (GameApp.cpp:1322-1327), leaving the world rendered but nothing playing. This is very plausibly *correct* for a shipped game (an unauthored GameMode is a content bug, not something a packaged build should paper over) — flag as an intentional, not accidental, divergence, but confirm that reading with the user/design owner rather than assuming.

---

#### Guard / structural differences worth a second look

**loadNavForLevel.** Editor: named function `SandboxApp::loadNavForLevel` (SandboxLevelLoad.cpp:11-20), guarded `#if AVER_MODULE_SYNAPSE`, calls `editor::navPathForLevel` (declared in sandbox/src/NavBakeCommand.hpp:61, an editor-only header) then `rebuildNavOverlay(e)` (ImGui-adjacent debug draw — correctly editor-only). Runtime: an **inline, unnamed block** inside `GameApp::openProject` (GameApp.cpp:1004-1021), guarded by the narrower/different macro `AVER_MODULE_SYNAPSE_SCENE` (not `AVER_MODULE_SYNAPSE` — these are genuinely different compile definitions, from `modules/synapse/CMakeLists.txt:23` vs `modules/synapse.scene/CMakeLists.txt:31`), storing into `gameNav_` instead of `nav_`. **`navPathForLevel` is duplicated**, not shared: Runtime/src/GameApp.cpp:253-255 carries its own copy with a comment explicitly saying so ("that file is editor-only (sandbox/), and this composition..."). The function is pure path manipulation (levelPath with extension swapped to `.ocnav`) with zero host dependency — a good candidate to promote into a shared module (e.g. next to `OcNav.hpp` in Aver.Formats) rather than staying duplicated a third time once this reconciliation adds a call from SandboxApp too.

---

#### PlayerStart marker vs SPAWN record — what's genuinely editor-only vs what the library needs

The editor's Player Start is TWO things layered together, and only one needs a library counterpart:
1. **The resolved spawn transform** (position + yaw) — this is what a shipped game needs, and it is currently unreachable from `GameLevel` because the SPAWN record fields never survive the `env_ = w` slice.
2. **The visible marker entity** (a cube mesh, `entityLabels_["Player Start"]`, Outliner selection, undo/redo re-derivation via `editor::refreshPlayerStart`, `addPlayerStart` toolbar command) — this is pure editor authoring UX. A shipped game has no marker, no selection, nothing to click; it only ever needs (1).

### Proposed library API

Land these in roughly this order — the bugfix first (ships alone, no new subsystem), then the small captures, then Player Start (highest gameplay value), then the three genuinely new subsystems (landscape/water/streaming, each sized like its own slice):

```cpp
// -- 0. Pure bugfix, Runtime/src/GameApp.cpp, no header change --
void GameApp::openProject(Engine& e) {
    ...
    applyLevelSky();
    if (project_.giExtent <= 0.0f) fitGiVolumeToLevel();   // was: called unconditionally
    ...
}

// -- 1. GameLevel gains the SPAWN + CAMERA + legacy-format captures (GameLevel.hpp/.cpp) --
class GameLevel {
public:
    void load(const std::string& path, GameContent& content);   // now dispatches on
                                                                  // fmt::levelFileIsLegacyOcmap(path)
                                                                  // internally, same as the editor

    // The level's authored spawn point. False when the level declares no SPAWN record --
    // caller decides the fallback (shipped game: leave the framework-spawned pawn where
    // aver_fw_begin_play put it; a future spectator fallback: camera position).
    bool spawnTransform(Vec3& outPos, f32& outYawDeg) const;

    // The level's authored CAMERA record, for anything that wants an edit-camera-style view
    // (tooling, a future in-game debug camera). False when the level declares none.
    bool cameraTransform(Vec3& outPos, f32& outYawDeg, f32& outPitchDeg, f32& outSpeed) const;

private:
    bool hasSpawn_ = false; Vec3 spawnPos_{}; f32 spawnYawDeg_ = 0.0f;
    bool hasCamera_ = false; Vec3 camPos_{}; f32 camYawDeg_=0, camPitchDeg_=0, camSpeed_=0;
};

// -- 2. GameApp places the pawn after begin_play (GameApp.cpp) --
// Mirrors SandboxPlay.cpp's placePawnAtPlayerStart, minus the marker lookup (GameLevel::
// spawnTransform IS the marker's replacement -- a shipped game has no marker to prefer).
bool GameApp::placePawnAtSpawn() {
###if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    Vec3 sp{}; f32 sy = 0.0f;
    if (!level_.spawnTransform(sp, sy)) return false;
    const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
    if (!pn) return false;
    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
    scene::World& w = scene::World::instance();
    if (!w.valid(pe)) return false;
    w.setLocalPosition(pe, sp);
    w.setLocalRotation(pe, quatFromEulerDeg(Vec3{0.0f, 0.0f, sy}));
    AVER_INFO("[Game] pawn placed at the level's Player Start ({:.0f},{:.0f},{:.0f}) yaw {:.0f}",
              sp.x, sp.y, sp.z, sy);
    return true;
###else
    return false;
###endif
}
// called from beginPlayIfGameModeDeclared(), right after aver_fw_begin_play succeeds --
// same "AFTER begin_play, the pawn doesn't exist until now" ordering the editor already documents.

// -- 3. New GameLandscape (Runtime/include/aver/game/GameLandscape.hpp + src), owned by GameApp --
// Mirrors loadLandscape/rebuildLandscapeCollision/unloadLandscape/applyLandscapeSurface(ToAll)/
// loadLandscapeForLevel/updateLandscapeRingTiles almost verbatim -- none of that code touches
// ImGui, selection or undo. Needs: Aver.Landscape linked into Aver.Runtime.Game (new
// if(TARGET Aver.Landscape) block in Runtime/CMakeLists.txt, matching the existing PBR/Voxi/
// Scene/Physics/Framework pattern), and a bindMaterial-shaped hook for the UV-tiling/material
// binding (already the same shape opt.bindMaterial uses in GameLevel::load).
class GameLandscape {
public:
    bool loadForLevel(rhi::IDevice* device, const std::string& levelPath, const fmt::OcWorldData& w,
                       const fmt::ProjectDesc& project);
    void rebuildCollision();                       // #if AVER_MODULE_PHYSICS
    void updateRingTiles(rhi::IDevice* device, f32 camXCm, f32 camYCm);
    void unload(rhi::IDevice* device);
    // Same seam GameLevel::load's opt.groundHeightAt wants -- GameLevel and GameLandscape both
    // live in GameApp, so GameApp wires this exactly the way SandboxApp wires it inline today.
    bool heightAt(f32 x, f32 y, f32& outZ) const;
};

// -- 4. New GameWater (Runtime/include/aver/game/GameWater.hpp + src) --
// Mirrors applyLevelWater almost verbatim (both the simulated-softbody and analytic-Gerstner
// paths). Needs Aver.Fluids linked into Aver.Runtime.Game, and the same "attach the render
// feature on first use" pattern GameApp::attachVoxi already has.
class GameWater {
public:
    void applyLevel(Engine& e, const fmt::OcWorldData& w);   // no-op if w.waters is empty
    void unload();                                            // teardown, called from GameLevel::unload
};

// -- 5. New GameStreaming (Runtime/include/aver/game/GameStreaming.hpp + src) --
// Mirrors setChunkStreamingEnabled's ChunkWorld-per-PCGVOLUME-field construction. Aver.World is
// already linked. Needs a manifest key or CLI flag (e.g. GameConfig::streamingEnabled, or
// STREAM.ENABLED in OcProject) since a packaged game has no Window menu to toggle it from.
class GameStreaming {
public:
    bool enable(const fmt::ProjectDesc& project, const std::vector<fmt::OcPcgVolume>& fields,
                const std::vector<fmt::OcScatterSpecies>& species);
    void disable();
    void tick(f32 camXCm, f32 camYCm);   // warnIfCameraOutsideGeneratedBand's check belongs here
};
```

### What the editor keeps

- The Player Start **marker entity** end to end: `makePlayerStart`/`addPlayerStart`/`refreshPlayerStart` (SandboxViewport.cpp:657-758), its undo entry, its Outliner label/selection, and `editor::refreshPlayerStart`'s undo/redo re-derivation (sandbox/src/PlayerStartRefresh.hpp) — a shipped game never needs a clickable, savable marker, only `GameLevel::spawnTransform`'s resolved value.
- All undo/redo and entity-label bookkeeping built on top of `world::instantiate`'s result: `entityLabels_`/`labelCounts_`/`entityCollide_`/`entitySnapZ_`/`entityBodies_`, `undoStack_`/`redoStack_`, `editToEntity_`/`entityToEdit_`, `markLevelSaved`/dirty tracking, `sel_`/`selEntity_` resets (all in `loadLevel`/`unloadLevel`, SandboxLevelLoad.cpp:1063-1091, 1537-1561).
- `saveLevel` and the legacy `.ocmap` writer's `entityLegacyDeform_/entityLegacySurface_/entityLegacyMaterial_` bookkeeping — a shipped game never writes levels back out.
- `frameCameraOn`'s role as the **edit**-camera auto-fit, and the `--cam` CLI override that outranks a level's `CAMERA` record — both are authoring conveniences; the runtime's camera comes from gameplay (`drivePlayCamera`), not from framing a level for a human to look at. Same for `chunkStreamHaveLastPos_`'s "this is a teleport, not a move" bookkeeping tied to `frameCameraOn`.
- The landscape create-tool state (`landCreateSamples_`/`landCreateSpacingCm_`, sculpting flags `sculpting_`/`sculptCursorValid_`/`landscapeDirty_`), and `rebuildNavOverlay`'s debug line-mesh draw (ImGui-adjacent, feeds the viewport overlay only).
- `--no-editor-chrome`, `--scene-census`, `--chunk-stream N`, `--landscape <path>` (`landscapeCliOverride_`), `--open-level` (`openMapPath_`) as authoring/QA CLI surface, and the `SandboxMcp.cpp` commands that toggle chunk streaming and drone spawn.
- `cbStatus_`/notification strings on every one of these paths ("This level already has a Player Start...", autosave countdown, etc.) — editor chrome by definition.
- `anyChunkWorldOwns`/`residentTriangleCount` — World Outliner and save-path bookkeeping ("is this entity streamed, so don't offer it for editing or save it"), meaningless without an Outliner.

### Hooks needed

- **bindMaterial** — already the right shape (`world::InstantiateOptions::bindMaterial`, `std::function<void(i32 token, const std::string& surface)>`); reuse the same signature for `GameLandscape`'s UV-tiling/material bind instead of inventing a second one.
- **groundHeightAt** — already the right shape (`world::InstantiateOptions::groundHeightAt`); once `GameLandscape` exists, `GameApp` wires it exactly the way `SandboxApp::loadLevel` wires its own lambda (SandboxLevelLoad.cpp:1043-1050) — no new hook type needed, just a second real implementation of the existing seam.
- **Device-scoped teardown** — already the right shape (`unloadLandscape(rhi::IDevice* device)` takes the device so it can free GPU resources, `nullptr` meaning "nothing resident yet"); `GameLandscape::unload`/`GameWater::unload` should take the same shape rather than assuming a device always exists.
- **Spawn-point hook, not a marker hook** — the one genuinely new hook this slice needs. The editor's Player Start is a *visible, selectable entity*; the runtime only ever needs the *value*. Do not build a "marker" abstraction shared between hosts — build `GameLevel::spawnTransform()` (pure data) and let the editor's own marker CRUD keep reading/writing the underlying SPAWN record through `saveLevel` as it does today. The two must agree on the record, not on an entity.
- **Record-present capture, not live mirrored sliders** — follow the `env_ = w` precedent (GameLevel.cpp:74): capture `hasSpawn/spawnX../spawnYaw` and `hasCamera/camX../camSpeed` as plain fields on `GameLevel` the way `env()`/`hasFog()`/`hasSky()` already do, rather than re-deriving them per-call the way the editor's live `sel_`/`playerStart_` handle does.
- **Streaming enable switch** — needs a genuinely new seam (`GameConfig` field or manifest key), since the editor's own trigger (ImGui menu item / MCP command / `--chunk-stream N` CLI flag) has no equivalent in a packaged game that ships with no console and one fixed project.

### Risks and what to check

- **GI-volume guard (item above) is a live bug today, not something this slice introduces** — fix and ship it first, independent of everything else. Hand-check: open a project that authors `RENDER.GIVOLUME` in both `Sandbox.exe` and `AverEngineRuntime.exe`, compare the `[Voxi] GI volume` log line's centre/extent between the two.
- **Player Start is the highest-value, highest-visibility fix in this slice.** Every current `AverEngineRuntime.exe` playtest starts the pawn at the origin. Hand-check on a level that has both a Player Start marker AND a raw SPAWN-only level (no marker) — confirm the shipped game's spawn position matches the editor's Play-mode spawn position in both cases, and that the pawn's initial yaw matches (Character.cs seeding its yaw from the placed transform, per SandboxPlay.cpp:124-125's comment, is a managed-side detail this native change must not break).
- **Legacy `.ocmap` dispatch**: hand-check by loading a genuinely-legacy map (one using ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM) in both hosts and confirming placement counts and `[Level] ... legacy .ocmap` logging agree — today the shipped game would load such a file through the wrong parser with no error, which is the worst kind of divergence (looks like it worked).
- **Landscape/water/streaming are the largest, riskiest, and most pixel-visible pieces of this slice** — they are net-new subsystem wiring (physics heightfield, render-feature attach, device-resource lifetime), not a rename. Sequence them last and verify each independently in `AverEngineRuntime.exe` with `--frames N` renders/screenshots and, where the level's own content changes, `--scene-census` (modules/world/include/aver/world/SceneCensus.hpp) before wiring `SandboxApp` to call them.
- **Once `SandboxApp` itself is edited to call into any of this new library code, GAME-LIFT.md's original "no commit can move a pixel in Sandbox.exe" guarantee no longer holds by construction** — that guarantee depended entirely on never editing `sandbox/**`, which this whole reconciliation project explicitly does. Each editor-side swap (SandboxLevelLoad.cpp's own functions replaced by calls into GameLevel/GameLandscape/GameWater) needs its own before/after gate run against the SAME project, the discipline the repo's `aver_gates`/`scripts/test.ps1` already exist for — per the standing rule in memory, the user runs gates/tests, not the agent implementing this.
- **spawnClassPlacements' ground-snap gap** compounds with the landscape gap: once landscape support lands in the library, `GameLevel::spawnClassPlacements` should also gain the `p.snapToGround` branch the editor has (SandboxLevelLoad.cpp:1318-1327) — easy to land landscape and forget this one caller, since it lives in a different function than the ordinary-placement snap in `world::instantiate`'s `groundHeightAt`.
- **`navPathForLevel` duplication**: low risk on its own (pure string function, two copies already agree), but worth folding into one shared definition (e.g. Aver.Formats, next to `OcNav.hpp`) at the same time SandboxApp is touched for anything else in this slice, rather than letting a third divergent copy appear.

### Effort and commit breakdown

Large — one of the bigger slices in this reconciliation, because roughly half the item list is net-new library code rather than consolidation. Suggested commit breakdown, in dependency order:
1. **Bugfix, ships alone**: guard `GameApp::fitGiVolumeToLevel()`'s call site with `project_.giExtent <= 0.0f`. Trivial, one line, no header change, immediately corrects live shipped-game behaviour.
2. **GameLevel captures**: add `spawnTransform()`/`cameraTransform()` (slice the OcWorldData fields the way `env_` already slices OcWorldEnv) and legacy-`.ocmap` dispatch inside `GameLevel::load`, reusing the editor's already-isolated synth-to-`OcWorldData` translation. Small, mechanical, no new subsystem.
3. **Player Start placement**: `GameApp::placePawnAtSpawn()` wired into `beginPlayIfGameModeDeclared()`. Medium size, but the highest gameplay value in the slice — budget real hand-playtesting time (spawn position AND yaw, with and without a marker) rather than treating it as done once it compiles.
4. **Landscape**: new `GameLandscape` class + `Aver.Landscape` CMake link. Large — comparable in scope to a full lift slice on its own (physics heightfield, ring-tile streaming, material/UV-tiling bind), not a quick port.
5. **Water**: new `GameWater` class + `Aver.Fluids` CMake link. Medium — smaller surface than landscape but touches physics (buoyancy plane) and a render feature.
6. **Chunk streaming**: new `GameStreaming` class + a CLI/manifest enable switch. Medium-large — the ChunkWorld-per-PCGVOLUME-field construction (SandboxLevelLoad.cpp:616-796) is the single densest function in this file.
7. **Editor-side swap**, once each of 2-6 is gate-verified standalone in `AverEngineRuntime.exe`: replace the corresponding `SandboxApp` functions with calls into the library, one subsystem per commit, each verified against the same project before/after (screenshots / `--scene-census`, run by the user per the standing "user verifies, I write code" rule) before the editor's own copy is deleted.

---

## C6 (camera + per-frame device push), C7 (the world draw walk + every piece of editor chrome interleaved in onRender), an

**Scope as read:** C6 (camera + per-frame device push), C7 (the world draw walk + every piece of editor chrome interleaved in onRender), and C8 (input publishing + the play session lifecycle). Editor side: sandbox/src/SandboxRender.cpp (SandboxApp::onRender and its helpers resolveSurface, posedHandle, emitEntityDraws, occlusion machinery, syncPtSceneView, submitGameUi) and sandbox/src/SandboxPlay.cpp (startPlay/stopPlay, pushInput, drivePlayCamera, anim curve/notify relays, Win32 mouse capture), plus the camera/sky push block that actually lives inline in SandboxApp::onUpdate at sandbox/src/SandboxApp.cpp:1815-2930 rather than in SandboxRender.cpp. Runtime side: Runtime/src/GameRender.cpp (drawWorld), Runtime/src/GameApp.cpp (onUpdate/onRender/pushFrame/drivePlayCamera/beginPlayIfGameModeDeclared), Runtime/src/GameInput.cpp (publishInput). Net finding: the camera math itself (C6) is already at parity — formulas, defaults and ordering all match line-for-line; the real work and the real risk are in C7 (two correctness bugs the Runtime has that the editor doesn't — no off-screen shadow-caster submission, no multi-material draw split — plus owner-hide entirely missing) and C8 (Player Start placement and OS mouse capture entirely absent from the Runtime, not merely un-hooked), with the HUD/UiRenderer gap confirmed as total (not linked, not called, anywhere in Runtime/).

### Reconcile map

##### C6 — Camera + per-frame device push
- **Camera defaults & formula.** Editor `camPos_{7,7,4.5}` (SandboxApp.hpp:3269), `yaw_/pitch_` — vs Runtime identical defaults (GameApp.hpp:321-322, comment explicitly ports them). `camForward()` — SandboxPlay.cpp:442-444 vs GameApp.cpp:1478-1480, byte-identical formula. View/proj push — editor onUpdate SandboxApp.cpp:2861-2879 vs `GameApp::pushFrame` GameApp.cpp:1518-1527: `Mat4::lookAtLH`, `perspectiveLH(radians(60), aspect, 2.0f, 200000.0f)`, `setCamera`. **Parity confirmed**, no drift.
- **viewAspect.** Editor divides the dockspace central-node rect (`vpW_/vpH_`, latched by buildUI a frame earlier) — SandboxPlay.cpp:437. Runtime divides the swapchain — GameApp.cpp:1482-1493. Deliberately different by design (both headers say so); this is the one piece the library comment already calls out as uncopyable. No hook needed beyond what already exists — this is the shape a hook (SkyPushOptions.viewportRect, item 3 above) should take for the *rest* of pushFrame.
- **Camera wobble (--cam-wobble).** Editor: SandboxApp.cpp:1830-1834, mutates `yaw_` itself every frame from a latched `camWobbleBaseYaw_`, so the wobbled yaw ALSO steers WASD movement direction (`camForward()` in the fly block reads the wobbled yaw_). Runtime: `pushFrame` GameApp.cpp:1511-1519, applies the offset only for the `camForward()` call feeding the view matrix and restores `yaw_` immediately — never touches movement (there is none to touch). Divergence is deliberate (Runtime's own comment: "nowhere safe to accumulate drift"); verification-only flag, low risk unless camTranslateSpeed_ and wobble are combined, which nothing does today.
- **Sky/fog/post push.** Editor SandboxApp.cpp:2879-2929 vs Runtime `pushFrame` GameApp.cpp:1537-1647. Same fields (`sky_.sunColor/zenith/horizon/fogColor`, cloud fields from a level's PCG "Sky" field, script-owned clouds via `aver_fw_sky_clouds`). Differences: (a) editor gates `sky_.enabled` on `showAtmosphere_` (an editor Show-menu debug toggle, SandboxApp.cpp:2896) — Runtime always `true` (GameApp.cpp:1541), correct for a game with no such menu; (b) editor applies `--sky-light`/`--sky-authored`/`--sun-elevation` CLI overrides every frame (SandboxApp.cpp:2903-2916) — these have no GameConfig equivalent in Runtime, and are verification-only so likely fine to leave editor-only; (c) editor wraps the pushed sky in `fluids::applyUnderwaterFog` when `waterAttached_` (SandboxApp.cpp:2919-2926) — Runtime has no fluids at all, see editorKeeps; (d) editor's clear colour is chrome-grey `(0.055,0.055,0.062,1)` outside the viewport rect (SandboxApp.cpp:2929) vs Runtime's pure black (GameApp.cpp:1645), both correct for their own host and explicitly documented as such.
- **Voxi per-frame settings reassert.** Editor: a large block (SandboxApp.cpp:2793-2852) pushing `frameBudgetTick`, GI/NRD/AverSR-auto and five console debug-reset slots — none of this exists in Runtime's `pushFrame` Voxi block (GameApp.cpp:1610-1639, which only does G-buffer/MSAA/volume/cache-dir/sun-direction). This is GI/Voxi-slice territory, not C6's — flagged as adjacent coupling only (same onUpdate region, same per-frame-push concept).

##### C7 — The world draw walk
- **Core walk.** Editor: the whole entity loop is `SandboxApp::onRender` SandboxRender.cpp:241-1727 (~1500 lines). Runtime: `aver::game::drawWorld` Runtime/src/GameRender.cpp:27-218 (~190 lines) — deliberately reduced per its own header comment ("no selection latch... no objects_ placeholder pass, and no capture/gate harness"). Shared shape: frustum-plane derivation from viewProj (GameRender.cpp:36-47 == SandboxRender.cpp:284-297, byte-identical formula), per-entity `CMeshRenderer` fetch + visible-bit check, static-mesh-bounds override guarded on `!skinned` (GameRender.cpp:67-73 == SandboxRender.cpp:942-950), world-space AABB frustum cull with "degenerate box is drawn, not culled" (GameRender.cpp:75-101 == SandboxRender.cpp:952-988), material resolution with the authored/look/fallback-with-once-per-name-warning ladder (GameRender.cpp:104-181 == `resolveSurface` SandboxRender.cpp:2112-2151, functionally equivalent but editor's goes through `aver::editor::resolveSurfaceLook`/`SurfaceInputs` — a shared free function, GameRender.cpp reimplements the same rule inline rather than calling it), skinned-handle substitution (GameRender.cpp:186-189 == `posedHandle` SandboxRender.cpp:2235-2242, editor's version additionally checks `softBodyScene_`), sticky `setDrawBlended` before every draw (GameRender.cpp:191-208 == emitEntityDraws SandboxRender.cpp:2189).
- **Culled/hidden entities: the "direct route" gap.** Editor: a frustum- or occlusion-culled OR owner-hidden entity is NOT dropped — it is still handed to `voxiRenderer_.submit()` (and `ptSceneView_->submitDraw` when not owner-hidden) so shadows/GI/RT TLAS never depend on camera visibility (SandboxRender.cpp:1015-1080, `emitEntityDraws`'s else-branch SandboxRender.cpp:2195-2226). Runtime: `if (outside) { ++culled; continue; }` (GameRender.cpp:100) — the entity is dropped entirely, no Voxi submission of any kind. **This is a correctness bug in the Runtime, not a chrome difference**: an off-screen shadow caster's shadow will disappear/pop the moment it leaves the camera frustum in a shipped game, something the editor's own occlusion-fix-plan.md (F4) explicitly fixed and the Runtime never received.
- **Multi-part/multi-material draws.** Editor: `PlannedDraw`/`aver::editor::planEntityDraws` (SandboxRender.cpp:1546-1553, also used in the depth-prepass walk and the direct route) splits a mesh with several material slots into one draw per part, each independently material-resolved. Runtime: `GameContent` has no `meshParts_`/`PlannedDraw` concept at all (grepped `Runtime/include/aver/game/GameContent.hpp` clean) — `drawWorld` issues exactly one `drawMesh` per entity with one resolved material (`mr->material ?: content.meshDefaultMaterial(mr->mesh)`, GameRender.cpp:125). **Also a correctness bug**: a multi-material asset paints every part with slot 0's colour in a shipped game.
- **Owner-hide.** Editor: `ownerHiddenHere` computed via an ancestor walk against `firstPersonPawn_` (set each frame by `drivePlayCamera`, SandboxPlay.cpp:404) for any entity carrying `kMeshRendererHiddenFromOwner` (SandboxRender.cpp:920-937), feeding `chooseRoute`'s `hiddenFromOwner` and excluding it from the raster route. Runtime: no such flag check anywhere in `drawWorld` — `firstPersonPawn_` does not exist as a concept in GameApp at all (confirmed: `drivePlayCamera` GameApp.cpp:569-608 never sets anything like it). **A first-person character's own body mesh will render in front of the camera in a shipped game.**
- **PlayerStart marker.** Editor: skipped from the mesh pass by identity (`ent == playerStart_`, SandboxRender.cpp:897) and drawn instead as a billboard icon via `viewportIcons_` chrome (SandboxRender.cpp:1773-1785). Runtime: no PlayerStart *entity* concept exists at all (GameLevel never creates one; grepped clean) — moot for drawWorld, but see C8 for the SPAWN-record data gap this reveals.
- **Depth prepass, occlusion culling, Trifactor LOD (discrete/per-cluster/mesh-shader).** SandboxRender.cpp:250-282 (LOD bookkeeping), :298-438 (a whole second walk that writes depth-only, excluding skinned/GPU-cluster/CPU-cluster/translucent per SandboxRender.cpp:306-315, :421-430), :449-847 (occlusion box collection, motion-safe trust gate, pyramid test, two-pass reorder). **None of this exists in Runtime/src/GameRender.cpp** — confirmed by its own header comment. Out of scope for C6/C7/C8's function set (drawWorld/onRender/pushFrame); flagged as its own future slice since it changes both performance and, for LOD, the actual geometry drawn at distance.
- **Editor chrome enumerated (all must become hooks or stay editor-only, per the task's explicit ask):**
  - Selection outline: `selectionOutline_`/`selectionOutlines_`/`hasSelection_`, captured inline at SandboxRender.cpp:1559-1566 during the walk, drawn via `drawLines` after it (SandboxRender.cpp:1756-1764). → DrawWorldOptions selection-sink hook (item 1).
  - PlayerStart icon — SandboxRender.cpp:1773-1785 (queued every frame from `viewportIcons_`, gated on `!anyPlayActive()`). → stays editor-only; only the entity-skip-by-identity half (SandboxRender.cpp:897) needs the hook (item 1's skipEntity field).
  - Owner hide — SandboxRender.cpp:920-937. → DrawWorldOptions.ownerHideRoot (item 1) — and this one is NOT chrome, it is gameplay-visible behaviour missing from the Runtime (see above).
  - Grid, nav mesh overlay, collider overlay (`showGrid_`/`showNav_`/`showColliders_`/`rebuildColliderOverlay`) — SandboxRender.cpp:1793-1821, drawn via `drawLines` entirely after the entity walk closes. Pure editor chrome, no hook needed — already sits outside drawWorld's natural boundary.
  - Gizmo + sculpt cursor (`drawGizmo`, `drawSculptCursor`) — SandboxRender.cpp:1822-1829. Same as above, already outside the boundary.
  - `noEditorChrome_`/`anyPlayActive()`/`maxFrames_` gates threaded through every chrome draw call above — pure editor state, no library concern.
  - occlusion.showCulled debug tint (`route.tint`, green-channel knockdown) — SandboxRender.cpp:2182, part of `emitEntityDraws`. Editor-only debug view; would ride the DrawWorldOptions sink if occlusion is ever lifted, otherwise stays out.
  - HUD (`submitGameUi`/`gameUi_`/`drawUiDemo`) — SandboxRender.cpp:2024-2033, SandboxApp.cpp:1431 (creation). **Confirmed the Runtime has none of this**: `Aver.Render.UI` is linked only by `sandbox/CMakeLists.txt:114`, not `Runtime/CMakeLists.txt` (grepped clean); zero references to `gameUi_`/`UiRenderer`/`aver_ui_begin_frame`/`aver_ui_draw_list` anywhere under `Runtime/`. `Aver.Render.UI`'s own CMakeLists/README confirm it depends only on `Aver.Core`/`Aver.UI`/`Aver.RHI` — no ImGui, so it is link-safe for the Runtime. This is the single largest concrete gap in the "editor's HUD renderer the game lacks" category the task asked about.

##### C8 — Input + play session
- **publishInput vs pushInput.** Runtime `publishInput` (GameInput.cpp:45-119) is explicitly the MECHANISM lifted from editor `pushInput` (SandboxPlay.cpp:282-367) with the POLICY replaced (own header comment says so): editor's gate is `!uiActive || (releasedByUser_ && playSessionActive())` plus per-consumer `own_.keyboardToGame`/`own_.mouseToGame` from `InputOwnership.cpp` (kb wants `!uiWantsKeyboard`, mouse additionally allows `mouseCaptured_` to override `uiWantsMouse`); Runtime's gate is the single bool `focused` (`e.window() != nullptr`). Both correctly implement the "unfocused/suppressed publishes an explicit release for every key, never an early return" invariant (GameInput.cpp:57-64 == SandboxPlay.cpp:284-303) — this specific defect class was already fixed on both sides. Editor additionally publishes the raw-VK twin from `input_.keyHeld(vk)` gated on `kb` (SandboxPlay.cpp:329-330) where Runtime gates the raw-VK loop on `focused` alone (GameInput.cpp:84-85) — same shape, narrower gate vocabulary.
- **Mouse delta source.** Editor: `mouseCaptured_ ? {captureDx_, captureDy_, io.MouseWheel} : {input_.mouseDX(), input_.mouseDY(), input_.wheel()}` (SandboxPlay.cpp:343-350) — i.e. during Play the mouse is OS-captured (`ClipCursor`+`SetCursorPos` re-centre every frame, SandboxPlay.cpp:594-664) and the delta comes from that, not from raw window deltas. Runtime: always `in.mouseDX()/mouseDY()` (GameInput.cpp:101), which are themselves accumulated `WM_MOUSEMOVE`-style position deltas (`modules/platform/src/InputState.cpp:64-65`), **with no capture mechanism anywhere in Runtime/** (grepped `ClipCursor`/`SetCursorPos`/`mouseCaptured`/`warpToAnchor`/`pollCapturedMouse` — zero hits). A shipped first-person game's mouse-look runs out of travel the instant the OS cursor reaches the window edge.
- **Play session lifecycle.** Editor `startPlay`/`stopPlay` (SandboxPlay.cpp:145-278): snapshot/restore the level's local transforms (`capturePlayWorld`/`restorePlayWorld`), sync physics bodies from authored `CRigidBody`s at Play (`editor::syncPhysicsFromScene`)/tear down at Stop, resolve a project GameMode or fall back to the engine's own default GameMode+Pawn+Controller (`engineDefaultGameMode`, spectator fly camera) or leave the standalone drone running, place the pawn at the level's Player Start (`placePawnAtPlayerStart`) with a fallback to wherever the editor camera was standing. Runtime `beginPlayIfGameModeDeclared` (GameApp.cpp:1312-1345), called once from `onInit`: finds a project GameMode via `aver_fw_find_class_with_flags`, calls `aver_fw_begin_play`, and stops — **no snapshot/restore (nothing to restore to — correct for a one-shot boot), no default-GameMode/spectator fallback (a content-only project with no GameMode simply never begins play and the framework stays permanently inert — the comment at GameApp.cpp:1322-1326 states this is deliberate), and no Player Start placement of any kind.** Confirmed: `GameLevel.hpp` keeps no `spawn`/`hasSpawn` field at all (grepped clean) — the data the editor's `levelHeader_.spawnX/Y/Z/spawnYaw` carries is never even parsed into anything GameApp can reach. **A project WITH a GameMode ships with its pawn spawning wherever that GameMode's own code puts it (commonly the world origin), not at the level's authored Player Start — a real, easily-hit parity gap** since the editor's Play button visibly does the opposite.
- **drivePlayCamera.** Editor (SandboxPlay.cpp:371-432) handles three cases: a real `PLAYING` session's possessed pawn, the standalone drone as a pawn stand-in (`dronePlayActive()`), and returns (camera frozen) otherwise; also latches `firstPersonPawn_` for the owner-hide check (item above). Runtime (GameApp.cpp:569-608) handles only the `PLAYING` case; no drone/spectator equivalent (none exists to drive), and does not set any owner-hide state. Given a GameMode is present, the pawn-follow math itself is byte-identical between the two (`aver_fw_view`/`aver_fw_view_entity`/first-person-vs-third-person pivot+boom formula, line-for-line the same).
- **Animation curves/notifies.** `animCurve`/`animNotify`/Synapse target-and-perception relays — SandboxPlay.cpp:453-459, 536-579, 585-589 vs GameApp.cpp's own static methods (declared GameApp.hpp:413-425, installed the same way per its own comments "same one-liner as GameApp::synapseTargetResolver"). **Already at parity** — both sides install the identical callback shapes onto `anim::animSystem()`/`aver_fw_set_anim_curve_provider`/the Synapse systems; no divergence found.

### Proposed library API

```cpp
// GameRender.hpp — widen drawWorld with an options struct; default value reproduces today's Runtime exactly.
struct DrawWorldOptions {
    // Skip this entity from the mesh pass entirely (PlayerStart drawn as chrome instead).
    scene::Entity skipEntity = kInvalidEntity;
    // Ancestor-walk root for kMeshRendererHiddenFromOwner (firstPersonPawn_ today). kInvalidEntity = off.
    scene::Entity ownerHideRoot = kInvalidEntity;
    // Called for every entity actually submitted (post-cull), with its route info — lets a caller
    // capture a selection outline transform or apply an occlusion-tint colour multiply without the
    // library knowing what "selected" or "occlusion debug" mean.
    using EntitySink = void(*)(scene::Entity, const Mat4& worldMatrix, u64 meshId,
                               bool frustumCulled, bool occlusionCulled, bool ownerHidden, void* user);
    EntitySink sink = nullptr;
    void* sinkUser = nullptr;
    // BUG FIX, not a hook: on by default. A culled/hidden entity still reaches Voxi's submit() (and
    // ptSceneView's submitDraw) so shadows/GI/the RT TLAS never pop when a caster leaves the frustum.
    bool submitCulledToVoxi = true;
    pbr::MaterialSystem* materials = nullptr;
    render::SkinnedScene* skinning = nullptr;
    // BUG FIX, not a hook: per-part/per-material planning (today editor-only via planEntityDraws).
    // When GameContent carries meshParts_, drawWorld uses it unconditionally for both hosts.
};
void drawWorld(rhi::IDevice&, const Mat4& viewProj, GameContent&, SceneDrawStats&, const DrawWorldOptions&);

// GameApp.hpp — one hook per concern, all optional (null = today's Runtime behaviour verbatim).
using DrawWorldOptionsHook = void(*)(DrawWorldOptions&, void* user);
void setDrawWorldOptionsHook(DrawWorldOptionsHook fn, void* user);

struct SkyPushOptions {                       // built once per frame inside pushFrame
    bool hasViewportRect = false;              // editor: confine to the dockspace central node
    u32 vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    bool showAtmosphere = true;                // editor's Show > Atmosphere debug toggle
    f32 clearColor[4] = {0, 0, 0, 1};           // game: black; editor: 0.055 chrome grey
    // Applied to a COPY of sky_ right before setSkyAtmosphere; null = no wrap (no fluids linked).
    using SkyWrapFn = rhi::SkyAtmosphere(*)(const rhi::SkyAtmosphere&, f32 camZ, void* user);
    SkyWrapFn wrapSky = nullptr; void* wrapUser = nullptr;
};
using SkyPushOptionsHook = void(*)(SkyPushOptions&, void* user);
void setSkyPushOptionsHook(SkyPushOptionsHook fn, void* user);

// GameInput.hpp — widen the single `focused` bool.
//
// LANDED 2026-09-20, BUT NOT IN THIS SHAPE. What was proposed here carried the editor's RAW
// conditions -- releasedByUser, uiWantsKeyboard, uiWantsMouse -- into the library. That is an
// ImGui-shaped question, and answering it is exactly what sandbox/src/InputOwnership.cpp already
// does, in a pure header with its own headless test. Passing the raw terms across would have put a
// second copy of that arbitration inside an ImGui-free bridge, which is the duplication this slice
// exists to remove. The landed struct carries the RESOLVED answers instead:
//
//   struct InputPublishPolicy {
//       bool focused, keyboardToGame, mouseToGame, gamepadActive, captured;
//       f32  capturedDx, capturedDy;
//       bool eaten[AVER_FW_KEY_COUNT];   // slots a host chord is claiming; still published, as releases
//   };
//
// gamepadActive is its own field rather than following the keyboard, for the reason SandboxPlay.cpp
// already documents: a text field steals a keystroke, never a controller button. `eaten` replaces
// the proposal's silence about the drawer chords, and an eaten slot is PUBLISHED as a release
// rather than skipped -- skipping is how a key latches down forever.
void publishInput(const InputState&, const InputPublishPolicy&, std::string* echo = nullptr);

// GameApp — mouse capture becomes real library code (currently 100% absent from Runtime).
void GameApp::setMouseCaptured(bool on);   // ClipCursor/SetCursorPos/ShowCursor, gated by GameConfig
void GameApp::pollCapturedMouse();         // called from onUpdate before publishInput when a session plays

// GameLevel.hpp — the SPAWN record GameLevel::load already reads for the file format, kept for once.
struct SpawnPoint { Vec3 position{}; f32 yawDeg = 0.0f; bool valid = false; };
const SpawnPoint& GameLevel::spawn() const;
// GameApp::beginPlayIfGameModeDeclared, after a successful aver_fw_begin_play, gets:
//     if (level_.spawn().valid) placePawnAtSpawn(level_.spawn());   // mirrors placePawnAtPlayerStart
```

### What the editor keeps

Stays 100% in SandboxApp (chrome/tools), fed into the new hooks rather than inlined in the walk:
- Free-fly camera: WASD/QE, mouse-look, wheel dolly/pan, F-frame-selection, Ctrl+S/Ctrl+Shift+S/compile-scripts chords, --wheel-speed-test/--recapture-test harnesses — SandboxApp.cpp:2153-2349 (onUpdate). No library business; a shipped game has no camera to hand-fly.
- InputOwnership arbitration (own_/resolveInputOwnership, ImGui WantCapture*, browserActive_, drawer/landscape scopes) — InputOwnership.cpp/.hpp, SandboxApp.cpp:2108-2162. Editor-only; feeds the new InputPublishPolicy instead of being duplicated in the library.
- Selection outline (selectionOutline_/selectionOutlines_/selectionOutlineLines, drawn AFTER the walk via drawLines), PlayerStart icon (viewportIcons_), grid, nav overlay, collider overlay (rebuildColliderOverlay), gizmo (drawGizmo), sculpt cursor — SandboxRender.cpp:1728-1829. Already structurally separate from the entity loop; just needs its capture point (today inline at SandboxRender.cpp:1559-1566 and :888-897) moved into the DrawWorldOptions hook's callback.
- objects_ editor-placeholder pass (Floor/Cube), skin-scene-test recolour, capture/gate harnesses (captureCheck, gpuTimingCheck, rayProbeCheck, skinDrawCheck) — SandboxRender.cpp:62-91, :2005-2021. Pure editor/CLI verification, no runtime equivalent needed.
- HUD demo widget (drawUiDemo/showUiDemo_) — SandboxRender.cpp:2036+. Editor debug preview, stays; distinct from the real HUD-rendering gap noted below.
- Occlusion culling, Trifactor LOD (discrete + per-cluster + GPU mesh-shader), the separate depth-prepass walk, showCulled debug tint, GPU stat scopes, scene-walk timing/log-on-change reports — SandboxRender.cpp:250-282, :298-438, :449-847, :1664-1725. NOT in this hook set; flagged as an open scope question, not attempted here.
- Voxi console/CLI reassert block (frameBudgetTick, 5 console reset-request slots, lighting/NRD-legacy bits, GI vis-path/blended-cone toggles, updateAverSrAuto) — SandboxApp.cpp:2793-2852. Owned by a GI/Voxi slice; only noted as adjacent coupling inside the same onUpdate region as the camera push.
- Fluids/water draw + underwater fog wrap, landscape draw/ring-tiles — SandboxRender.cpp:93-240. Whole subsystems the Runtime does not link at all (confirmed: no AVER_FLUIDS_SIMULATED/FluidScene/landscape reference anywhere in Runtime/). Own slices; called out because they sit inside the exact onRender region and onUpdate sky-push region this slice touches.

### Hooks needed

1. DrawWorldOptions (GameRender.hpp): replaces the editor's inline selection/PlayerStart/owner-hide checks inside the entity loop (SandboxRender.cpp:888-897, :920-937, :1559-1566) with data the caller supplies once per frame — an entity-to-skip (PlayerStart), an owner-hide root entity (firstPersonPawn_), and a per-entity callback/sink the editor uses to capture selection transforms and apply the occlusion-tint colour multiply (today SandboxRender.cpp:1013, :2182). Default-constructed (all off) reproduces the Runtime's current drawWorld exactly.
2. GameApp::setDrawWorldOptionsHook(fn, user) — called once per frame from onRender before drawWorld, mirroring setAverSrInstaller's shape (GameApp.hpp:157). Editor's hook populates DrawWorldOptions from sel_/selEntity_/multiIsSelected/playerStart_/firstPersonPawn_; a shipped game passes null.
3. GameApp::setPreSkyPushHook / an explicit SkyPushOptions{viewportRect, clearColor, showAtmosphere, underwaterFogFn} parameter on pushFrame — lets the editor confine to vpX_/vpY_/vpW_/vpH_ (SandboxApp.cpp:2861), keep its 0.055 chrome-grey clear colour vs the game's pure black (SandboxApp.cpp:2929 vs GameApp.cpp:1645), and wrap sky_ in fluids::applyUnderwaterFog on a copy (SandboxApp.cpp:2919-2926) without GameApp itself knowing what a FluidScene is.
4. InputPublishPolicy (GameInput.hpp) — widens publishInput's single `focused` bool into {focused, releasedByUser, uiWantsKeyboard, uiWantsMouse, mouseCaptured, capturedDx, capturedDy}, replacing the ad hoc `suppressed`/`kb`/`m` locals SandboxApp::pushInput computes today (SandboxPlay.cpp:296-350) with named fields the editor fills from own_/releasedByUser_/mouseCaptured_, and the Runtime fills with {focused, false, false, false, false, 0, 0} — bit-identical to today's publishInput.
5. Mouse capture as LIBRARY code, not an editor-only hook: GameApp needs its own setMouseCaptured/warpToAnchor/pollCapturedMouse (Win32 ClipCursor/SetCursorPos/ShowCursor, SandboxPlay.cpp:594-664) gated on a play session being active, because AverEngineRuntime.exe has NONE today and its mouse-look is a raw WM_MOUSEMOVE delta with no cursor confinement (modules/platform/src/InputState.cpp:64-65) — this is a correctness gap, not a chrome hook; the editor keeps its own release-mouse chord (Shift+F1/PlayReleaseMouse) layered on top via InputPublishPolicy.releasedByUser.
6. GameLevel::spawn()/SpawnPoint accessor — GameLevel::load already reads the file's SPAWN record for the editor (via levelHeader_ in SandboxApp, not GameLevel) but GameLevel.hpp/.cpp keeps none of it today (grepped clean). Needs to parse and expose {position, yawDeg, valid} the way SandboxApp::playerStartTransform's second branch does (SandboxViewport.cpp:680-684), so GameApp::beginPlayIfGameModeDeclared can place the pawn the way SandboxApp::placePawnAtPlayerStart does (SandboxPlay.cpp:126-143) — today it does not, and GameApp.cpp has zero references to any spawn concept.
7. NOT a hook, a bug fix both hosts need: drawWorld's "unified direct route" (frustum/occlusion-culled entities still submit(...) to Voxi so off-screen casters keep shadowing/GI-bouncing — SandboxRender.cpp:1015-1080, emitEntityDraws' else-branch at SandboxRender.cpp:2195-2226) has NO counterpart in Runtime/src/GameRender.cpp, which does a bare `continue` on frustum-cull (GameRender.cpp:100) and never calls voxiRenderer_.submit() at all. Port this into the shared drawWorld so both hosts get it, not just the editor.
8. Also not a hook, also a bug fix: per-part/per-material draw planning (PlannedDraw/planEntityDraws/meshParts_, SandboxRender.cpp:1546-1553) is entirely absent from GameContent/GameRender — every entity in the Runtime draws as ONE mesh with ONE material (mr->material ?: content.meshDefaultMaterial(mr->mesh), GameRender.cpp:125). A multi-material asset renders correctly in the editor and wrong (one material painted over every part) in a shipped game. Port the split into GameContent + drawWorld.
9. HUD: GameApp needs to link Aver.Render.UI (it does not — grepped Runtime/CMakeLists.txt, only Sandbox links it), create+register a render::ui::UiRenderer the way SandboxApp::attachVoxi's sibling does (SandboxApp.cpp:1431), call aver_ui_begin_frame every onUpdate the way SandboxApp.cpp:2424-2425 does, and call submitGameUi's equivalent (SandboxRender.cpp:2024-2033) from onRender. Zero of this exists in GameApp.cpp/GameInput.cpp/GameRender.cpp today — any project's script-driven HUD (scripts_.hudDraw) silently renders nothing in AverEngineRuntime.exe. Flagged per the task's explicit ask; not designed in depth here (own slice — needs its own reconcile pass on GameContent/scripting's HUD API, out of C6/C7/C8's function set).

### Risks and what to check

1. **Shadow/GI popping fix (submitCulledToVoxi).** First time it lands, a shipped game's image changes for any level with an off-screen shadow caster. By hand: load a level with a strong caster near the frustum edge in both `Sandbox.exe` (Play) and `AverEngineRuntime.exe`; rotate the camera until the caster leaves the frustum; confirm its shadow no longer disappears/pops in either host, and screenshot-diff the two.
2. **Multi-material draw split.** Changes any asset with >1 material slot from "every part paints slot 0's material" to correct per-part materials in the shipped game. By hand: load a known multi-material prop (e.g. one of PTTest's) in both hosts and screenshot-diff; confirm the part colours/textures match, not just the silhouette.
3. **Owner-hide port.** By hand: possess a first-person GameMode's pawn in `AverEngineRuntime.exe`, confirm the pawn's own body mesh (flagged `kMeshRendererHiddenFromOwner`) no longer renders in the forward view, matching Play-mode behaviour in the editor.
4. **Player Start placement.** By hand: `--open-level <X>` through `AverEngineRuntime.exe` with a project that HAS both a GameMode and an authored Player Start; compare the pawn's spawn position/yaw against pressing Play on the identical level in the editor — they should now match (today they almost certainly do not, since the Runtime has no spawn data at all).
5. **Mouse capture.** By hand: launch a first-person project through `AverEngineRuntime.exe`, look continuously in one direction for several seconds (today's bug: hits the window edge and stops turning) and confirm unbounded rotation; alt-tab away and back mid-look and confirm no single-frame camera spin (mirrors `warpToAnchor`'s own foreground-window guard, SandboxPlay.cpp:621-625).
6. **Cam-wobble semantics.** Low risk — verification-only flag. If the Runtime's "render-only offset" behaviour is kept as the single shared implementation (recommended, since it's simpler and already correct for a still camera), no visible capture should change; only matters if wobble and free-fly translation ever run together, which nothing does today. No by-hand check needed unless someone later adds camera-drive to a shipped game.
7. **Scope gap: LOD/occlusion/Trifactor, fluids/water, landscape, HUD are NOT covered by this hook set.** "Editor's behaviour as reference" is not fully reachable by C6/C7/C8 alone — a project using water, landscape, occlusion culling or distance LOD will still look/perform measurably differently between `Sandbox.exe` Play and `AverEngineRuntime.exe` after this slice lands. Surface this explicitly to whoever is sequencing the remaining slices; do not let "C6/C7/C8 done" read as "the two renderers match."
8. **HUD is its own slice.** Wiring `Aver.Render.UI` into `GameApp` (link it, register a `UiRenderer`, call `aver_ui_begin_frame`/submit the draw list every frame, decide how a project names its default HUD for `scripts_.hudDraw`) is real, uninvestigated design work beyond this reconcile map's function set — flagged per the task's explicit ask about the HUD gap, not designed in depth here.
9. **Process risk for the SandboxRender.cpp side of the eventual migration:** the file is ~2900 lines with occlusion/LOD/depth-prepass machinery deeply interleaved with the parts this slice touches (selection capture sits between LOD logging blocks, e.g. SandboxRender.cpp:1540-1568). Whoever implements the DrawWorldOptions hook must NOT let the occlusion/LOD pre-walks silently stop calling `resolveSurface`/`planEntityDraws` the same way — regression here would be invisible until an occlusion or LOD counter (`[Occlusion]`/`[LOD-SELECT]` log lines) goes quiet or wrong, so by-hand: run a `--occlusion-cull`/`--lod-select` capture before and after, diff both the rendered image and the log line counts.

### Effort and commit breakdown

Moderate-to-large, six commits, increasing risk:
1. **Small, low risk, independent of the SandboxApp lift:** land the two Runtime correctness bugs directly — submitCulledToVoxi in drawWorld (port the "direct route" from SandboxRender.cpp:1015-1080/2195-2226) and multi-part/multi-material draw planning (port PlannedDraw/planEntityDraws + meshParts_ into GameContent). Worth doing even before any hook exists, since both hosts benefit and the editor's own code is the reference implementation to copy.
2. **Small-moderate, low risk:** GameLevel::spawn()/SpawnPoint + wire GameApp::beginPlayIfGameModeDeclared to place the pawn — mechanical, mirrors placePawnAtPlayerStart's existing contract, high value (fixes spawn-position parity for every GameMode project).
3. **Moderate, moderate risk (Win32 focus/alt-tab edge cases):** port mouse capture (setMouseCaptured/warpToAnchor/pollCapturedMouse) into GameApp/GameInput, gated by a new GameConfig flag; wire InputPublishPolicy and switch publishInput's call site.
4. **Small, low risk:** add DrawWorldOptionsHook + SkyPushOptionsHook to GameApp; this commit only adds the plumbing (defaults reproduce today's Runtime exactly) — no editor code changes yet.
5. **Large, highest risk:** switch SandboxRender.cpp's onRender scene-entity pass to call the shared drawWorld with populated DrawWorldOptions, and switch SandboxApp::onUpdate's camera/sky block to call pushFrame with SkyPushOptions — while KEEPING occlusion/LOD/depth-prepass exactly as they are today (as a pre-pass feeding already-resolved handles into the shared walk, or left as a separate pre-selection step; needs its own design pass before this commit starts). By-hand test occlusion (`[Occlusion]` log lines) and LOD (`[LOD-SELECT]`/`[LOD-CLUSTER]` log lines) counts before/after, plus a full screenshot diff across a handful of gate scenes.
6. **Follow-up, out of this slice:** HUD/UiRenderer wiring into GameApp (link Aver.Render.UI, aver_ui_begin_frame, submit, decide the default-HUD-naming question for scripts_.hudDraw) — its own reconcile pass, not attempted here.

