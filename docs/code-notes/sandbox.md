# Code notes: sandbox

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## sandbox/src/EditorConsole.hpp

### Console variable registry design (part00, lines ~2-16)
Mirrors GraphNodeDefs.hpp. Header-only: SandboxApp.cpp is only TU. Every function inline. Command and variable tables share file: get/set/vars walk variable table. Scope: reads/writes voxi::Renderer settings and one rhi::IDevice post/toggles (already-public).

### Case-insensitive string helpers (part00, lines ~47-61)
ciEquals for command/variable names. ciContains for completion and browser (substring in name or description, O(n*m) for small lists).

### VarValue tagged union (part00, lines ~83-86)
Five cases: U32, F32, Bool, Quality, Str. Str for read-only diagnostic text (voxi.status.*, etc.).

### String parsing conventions (part00, lines ~131-176)
U32 parsed signed first so negative literal reported as itself (not wrapped huge unsigned).
Quality accepts case-insensitive names or 0-4.
Bool: true/false, 1/0, on/off.

### ConsoleBatch two-phase commit (part00, lines ~178-200)
Setter closures, not two pre-seeded Settings structs. Tier-phase first, dial-phase second. Snapshot fresh at each phase's commit to avoid stale tier fields. Tier change detected by diffing incoming vs live settings.



### ConsoleBatch design: closures for tier/dial separation
The batch uses setter closures (not pre-seeded structs) to avoid stale tier fields when commits happen in multiple phases. Each phase re-reads the live renderer settings at commit time: tier-phase reads what was live before, dial-phase reads what the tier-phase just derived. This prevents a tier change from overwriting explicitly-set dial values in the same `set` line.

### Raw-slot idiom for debug/measurement toggles
VoxiRenderer has private debug views (giPoisonView, giVisPathView) and measurement toggles (giForceRebuild, giBoundedDispatch, giFreeAccumulator) with no path through voxi::Settings/setSettings. These use a raw-slot pattern: a static bool/u32 owned by this header, written by `set voxi.name` and reasserted every frame from SandboxApp's onUpdate to make the value live.

### GI measurement results on PTTest
- giBoundedDispatch: rebuilds only touch ~1.3% of the 512^3 grid instead of 100%. Measured on PTTest NewSponza, ray-driven, 400 moving frames: GPU total 30.64 → 30.25ms. Output verified 99.3% bit-identical, 0.0024 mean absolute difference (residual is GI temporal noise).
- giFreeAccumulator: frees ~2048 MiB accumulator at Epic's 512^3 after 240 quiet ticks, recreates on demand (one-tick latency cost).

### RT shadow optimizations and measurements
- rtSecondaryShadowOpaque: secondary (reflection, ReSTIR GI candidate) shadows use fast opaque-only rays instead of full glass-tinting walk. Measured GI trace 3.88 → 3.38 ms, reflection 3.14 → 2.73 ms, image MAD 0.09. Glass/water stops casting shadows for these two secondary rays only; primary shadows untouched. Default ON.
- rtSkyOcclusionHalfRate: 8x8 tiles skip traced samples where history reprojects validly. Measured 0.72 → 0.47 ms, still image MAD 0.40, no tile pattern in motion. Default ON.
- rtReflectionHalfRate: rough pixels skip retrace where history is valid. Measured 3.14 → 2.17 ms, MAD 0.04. Default ON.
- rtGiHitShadowMap: GI bounce hits shadow map instead of tracing ray. Measured whole frame 11.03 → 10.41 ms (gallery) and -0.57 ms (court), image MAD 0.18 still / 0.34 moving (earlier 1.61 under column capitals; no longer reproduces). Default ON.
- rtSkipUnchangedTlas: skips TLAS rebuild when draw list unchanged. Measured 0.42 ms/frame on static scene. Compute-skinned meshes force rebuild unless rtRefitAccel is on (then runs lighter refit-only pass). Moving draws patched into instance table/TLAS refit when rtRefitAccel is on, avoiding full per-draw rebuild.

### Ray-driven staged performance
- rayDrivenStages: 0 = single pass (baseline), 1 = staged visibility + lighting compute + shading (measured 40-45% faster), 2 = staged + half-rate GI (DEFAULT, further ~1.4 ms, image within 0.4% of 1, slightly noisier in motion). Mode 2 differs from 1 only while ReSTIR GI and denoiser are on.
- D3D12 only for modes 1 and 2; other backends run single pass.

### Legacy lighting-contrast fix A/B switches
Five bits of consoleLightingLegacySlot(), all default OFF (fixed behaviour):
- Bit 1 (legacyRestirSampleRing): sample fixed 45° ring instead of cosine hemisphere for ReSTIR candidate and sky-occlusion rays.
- Bit 2 (legacySkyDoubleCount): receiver counts sky twice (traced estimate + ambient term).
- Bit 4 (legacyRestirHitSky): ReSTIR candidate's second-bounce sky added with no visibility test.
- Bit 8 (legacyRestirReuseVisibility): reused sample shades with no visibility test between receiver and reused position.
- Bit 16 (legacyConeWeights): cone directions cos-weighted a second time (cos² instead of cos). Affects giMode 0, non-RT fallback, cluster and particle passes.
- Bit 32 (legacyBlendedHistoryWrite): blended fragments' history writes/readbacks no longer suppressed (D3D12 only).
- Bit 64 (legacyDenoisedReadback): read denoised GI at this frame's pixel instead of reprojected to last frame's surface (one-frame displacement in motion).

### ReSTIR GI history and reuse tuning
- giRestirMaxHistory: camera-motion fade fix. DEFAULT 0. Old value 1 overshoots ~8% for ~25 frames after camera stops, 8 overshoots ~104%. Measured no worse at rest or motion with 0 (same settled brightness, grain, flicker); denoiser is what smooths this. Packed into 5 bits, clamps to [0,31].
- giRestirVisibility: how much ReSTIR GI pays for at this tier (0=no ray pre-fix, 1=reconstructed no ray, 2=half res, 3=full, 4=cached NeuRaC D3D12 only else half). Tier-derived dial, re-derived on tier changes.
- giRestirSpatialSamples: overrides spatial-reuse tap count or leaves motion discount alone (15=auto). Fade bisection left only these two standing; everything else measured and removed.

### GI poison view colours (guards firing this frame)
- Magenta: store-time reservoir guard (most important).
- Cyan, yellow, orange, blue: candidate-radiance clamp, target-pdf guard, pre-existing final-estimate guard, denoiser-readback guard (all indicate non-finite/NaN corruption).
- Red: raw estimate hit giRadianceCeiling while still finite.
- Green: denoised readback hit same ceiling (these two show where white patch lives, not bugs; non-finite guard outranks).
- All seven above are giMode 1 (ReSTIR) only.
- Violet: ray-traced specular indirect's own ceiling hit (NOT giMode-gated, exists under either diffuse estimator). Paints in voxi.hlsl's PSMainVoxi/PSRayDriven.

### rtRenderMode and ray-driven internals
- 0 = rasteriser finds first surface (Low tier, product decision not hardware gap). 1 = primary ray per pixel (Medium/High/Epic; gives up hardware early-Z).
- Derived from RT tier on tier change unless set in same line.
- D3D12 only for staged modes 1 and 2.


### Post device slot and console integration
- Post is per-device state (IDevice::postProcess/setPostProcess), accessed once per frame via setConsoleDevice() to avoid threading a device pointer through every call site. A null device (no swapchain) makes every post.* read/stage a no-op, not a crash.

### Post clamping strategy
- setPostProcess does no clamping in D3D12Device.cpp and VulkanCommon.hpp. Post console entries clamp before calling setPostProcess, reporting the clamp through the same diff mechanism that Voxi uses. Ranges are inferred from RHI.hpp's field comments/defaults, not a documented contract (PostSettings has no formal range spec).

### Occlusion.debugForceWaitIdle investigation
- A follow-up investigation found that GI/lighting still depends on the unconditional res.waitIdle() call that the buffer-rotation fix made unnecessary for correctness of THAT specific fix. The root cause of this dependency has not been found. The current no-wait path is measurably worse versus path-traced ground truth in A/B tests, but which GI/lighting resource depends on the wait remains unknown. Pays the stall until fixed.

### Occlusion.showCulled false-cull finder
- Intended use: white-panel investigation, plan item 3B. Draws that chooseRoute() would frustum/occlusion-cull but are NOT owner-hidden appear tinted magenta instead of being skipped, making false culls visible. Cost when off: one bool test per culled entity, the same cost chooseRoute already pays for frustum/occlusion booleans.

### handleFrameTime formatter reuse
- One formatter (in aver/rhi/GpuTimingFormat.hpp) is shared between the editor and the packaged game. It originally lived in EditorConsole.hpp, which the shipped game could not include, preventing the game from displaying timings it was already paying to collect. Moved beside the report it formats so the two hosts cannot drift into printing different things from the same data.

### GPU/CPU frame time matching caveats
- GPU total is the sum of TOP-LEVEL marked spans only (collectGpuTiming's topLevelMs), excluding unmarked spans. CPU frame is THIS INSTANT's e.time().dt, not an average -- not a matched pair: different sources, different moments.

### Vars command substring filtering
- `vars` with no args: unchanged line-for-line (screenshots/muscle memory may depend on today's shape). `vars <text>` filters by substring over name or description, grouped by prefix (e.g., "vars gi" groups every voxi.gi* dial under one heading).

### Set command all-or-nothing validation
- Every name/value pair is looked up, checked for read-only status, and parsed BEFORE anything is staged. A bad token anywhere (unknown name, read-only, unparsable value) aborts the WHOLE line with one error and stages nothing, so a later typo cannot half-apply.

### Per-history reset command ordering
- TRY resetgihistory FIRST (the concrete NaN-storage gap was found there). If that alone does not clear a GI blotch/burn-in, try resetrthistory. If neither clears it, try resetdenoiserhistory. Do NOT run resetallhistory FIRST during bisection -- it clears everything at once and tells you nothing about which buffer was actually poisoned.

## sandbox/src/SandboxApp.cpp

- `onUpdate()` --set timing: applied at frame 5 not frame 1 because the project's own RENDER.* settings apply runs during startup and would overwrite anything staged earlier. A handful of frames costs nothing in a measurement run.

- `onUpdate()` --cam-wobble, --cam-translate, --cam-wander: wobble runs until frame N with --cam-wobble-stop N; translate and wander also stop at same frame. Wobble base yaw is latched (accumulated yaw drifts with float error and never returns exactly to start).

- `onUpdate()` camera synchronization: camera set first so downstream code (view matrix, reprojection, shadow history, streaming) sees one consistent frame. Latched base yaw rather than accumulated.

- `onUpdate()` --save-level bug history: used to run in Voxi-init one-shot hook (frame 1, before --open-level applied), so `--open-level Arena --save-level out.ocworld` always wrote the project's start map. Two runs went by before placement count was noticed wrong. Now waits for every pending open (--open-level, forwarded launch, unsaved-changes prompt) rather than just moving later.

- `onUpdate()` N8 fix (PT view follows tier changes): before, only 4 sites flipped ptSceneViewWantEnabled_ (CLI, Quality combo, toggle-test flags, manifest). None fired for console's voxi.pathTracing, Overall Quality (applyOverall), or voxi.scalability, which write the tier directly.

- `onUpdate()` input phasing bugs in flying camera: ImGui computes io.MouseDelta inside NewFrame, but Engine::frameStep runs onUpdate before uiNewFrame, so any read here sees the PREVIOUS frame's delta. InputState is fed by pumpEvents (before frameStep) and holds THIS frame's motion. Same phase bug with io.MouseWheel: Engine::frameStep runs onUpdate before uiNewFrame, and EndFrame zeroes io.MouseWheel at tail of previous frame, so onUpdate only ever sees stale reset. GraphEditor/AssetEditor wheel zoom work because they run from onRender after NewFrame. input_.wheel() (via pumpEvents/newFrame()) holds this frame's notches correctly.

- `onUpdate()` wheel fly-speed bug: this scroll-to-dolly read io.MouseWheel too and never moved the camera. Same phase bug as the mouseDX/DY note. Found by following that bug, not by a report, indicating control with no headless witness breaks silently when it does.

- `onUpdate()` MMB pan and look: reads frame-old delta (same phase argument as flying_ block). Same defect class as the input publisher that used to latch every key.

- `onUpdate()` --aversr-cycle verification bug history: turning AverSR on then off crashed the editor. Off's branch reset the unique_ptr while the device still held the raw pointer applyUpscalerSlot gave it, so the next composite called execute() on freed memory. --aversr only ever ATTACHES, so nothing exercised this path before.

- `onUpdate()` graph class instances tick decision: ungated, every class-placed graph ran OnTick while someone just looked around. Measured on PTTest over 1000 frames with Play never pressed: AN_FPRules' elapsed VAR climbed from 3.6e-05 to 12.31s, 4003 tick lines written, no begin_play. A graph is free to move entities, fire events and write VARs, so browsing a level mutated it. Same condition as framework tick groups (one spelling not two) used because this block needs AVER_MODULE_FRAMEWORK guard: aver_fw_play_state() comes from framework_abi.h, and with no framework there's no Play to gate on and no class instances to tick.

- `onUpdate()` frame interpolation: reads motion and depth from G-buffer (fourth reason G-buffer exists; denoiser is third, game AA/soft MSAA are others).

- `onUpdate()` --shader-source recompile count: "Only the first edit is ever delivered" was a measurement artifact. `--frames 900 --no-vsync` lasts ~12s (measured: 900 frames/12.0s/75fps), so 2/3 of a 21-second test's edits landed after the process had exited (the log's "stopped after 900 frame(s)" sits between append one and append two). Re-measured with `--frames 9000`, five edits nine seconds apart: all delivered. Only bumps an integer (rhi::reloadShaderFiles() drops text cache and increments revision; VoxiRenderer::prePass rebuilds pipelines there, where it's safe).

- `onUpdate()` Ctrl+S label: File menu has claimed the label for as long as it existed, but that "Ctrl+S" is just ImGui::MenuItem's shortcut-LABEL parameter wiring nothing. The only ImGuiKey_S in this file was the camera's strafe-left, so the reflex shortcut did nothing with no feedback.

- `onUpdate()` Frame Selected (F key) gating: Outliner/Details count too because levelFocused_ is true only while viewport holds ImGui's keyboard focus; clicking an Outliner row moves focus there. Scene-guarded as a whole: F frames a SELECTED ENTITY's bounds, and selectionBounds is `#if AVER_MODULE_SCENE`, walking the selection set through the world. anySelected() isn't guarded (sun/sky/post rows are selectable without a scene), but none of those has bounds to frame a camera on, so the binding just does nothing there.

- `onUpdate()` Shift+F (Pawn to Camera): would send pawn to camera AND fly that camera off to frame the selection (both acted on same press). The other command is asked rather than checking Shift key, so it still holds after either has been rebound. Checked with held() not pressed(): this row repeats while F is down, Pawn to Camera does not, so pressed() would let the second auto-repeat pulse through. Only while there is a pawn to send; without one, Shift+F frames as it always did.

- `onUpdate()` recapture test: safe now that re-centring refuses to move a background window's cursor.

- `onUpdate()` --wheel-speed-test overrides: stand in for right-drag state. Headless has no real cursor, so ImGui reports wantKb=1 wantMouse=1 ptrInViewport=0 and resolveInputOwnership correctly denies the tools (right precondition for an unhovered window, wrong for this test which is about what happens once the gate is OPEN).

- `onUpdate()` F8 (Play Eject) binding: possess while ejected, eject while possessed. One chord gated on a real session rather than on scope (KeybindRegistry::pressed() doesn't consult it either way).

- `onUpdate()` Pointer in viewport conversion: only place that can do it correctly. UI frame is laid out against the viewport (or HUD preview rect), not the window, so a window-relative cursor would miss every rect by the dockspace's offset. ImGui reports outside cursor as -FLT_MAX; pushed further negative to hit nothing (that's what "not here" should mean).


- `beginFrame()` voxi settings: vs is a copy to prevent throttled frames from becoming the new baseline. Modifications are never written back to the singleton; Project Settings still shows authored values.

- `auto-switch` debug views: ray-hit/triangles views can't draw through rasteriser; Wireframe and G-buffer can't draw through ray-driven primary visibility. They share one pass-level float in ViewDebug (see VoxiRenderer.hpp).

- `debugViewActiveThisFrame`: debug views paint flat diagnostic colours. Auto-exposure, bloom, and local exposure must be disabled to avoid smearing the unambiguous diagnostic colour. Uses a copy of post_ with postPushed_ tracking it, so the adopt-if-changed check never mistakes it for a user change.

- `Undenoised mode`: Known gap: no runtime knob exists for reflection spatial filter (rtReflectionSpatial) or sky-occlusion spatial filter. Neither is gated by Settings fields, so Undenoised leaves both running rather than claiming to handle them.

- `fogDensity_`: One member for one value. Previously, code read fogDensity_ then overwrote it with levelFog_, making Fog Density both inert and unsaved. Load now writes to the slider's own member; save reads it.

- `skyLightIntensity`: skyLightIntensity scales the unoccluded ambient one-frame term (diffAmbient, material_prelude.hlsl). The open question behind "washed out" colours: sky is Rayleigh-blue, stone bounce is warm, so large unoccluded ambient dilutes chroma toward grey.

- `applyLevelSky` precedence: applyLevelSky overwrites sky_ whenever a level opens, so --sky-light, --sky-physical, --sky-authored, and sun elevation overrides must be reapplied every frame, not just at startup.

- `skinSelfTest_`, `skinDraw_`: Verdicts latched before object reset. Engine::run calls exitCode() after onShutdown, so a verdict left inside the object is gone by then. Previously --skin-test exited 0 regardless because it never latched.

- `gbufferDebugFeature_` teardown: Must call removeRenderFeature() before the device tears down. GBufferDebugFeature's destructor calls releaseGpu() after onShutdown, when device and resource factory are gone, making res_ dangle. Measured: `--gbuffer-debug velocity --msaa 1` exited 0xC0000005 with 0 debug-layer errors—clean frame followed by CPU access violation.

- `water_.shutdown()` timing: Before aver_phys_shutdown, so retiring live volumes calls into a live solver. Same fix applies to gbufferDebugFeature: device pointer cleared before unique_ptr reset (see upscaler/frameInterpolator pattern).

- `projectDirty_` on exit: Project autosave has 0.5s debounce. An edit immediately followed by exit sits in projectDirty_ when the process exits. Neither exit path (requestExitChecked, onCloseGuard) checks it, so edits are lost with no prompt and no log line. Flush here closes all exit paths.

- `--frames` manifest guard: --frames sets render settings from CLI; applyProjectRenderSettings marks the project dirty. Unguarded flush would save capture run flags into the user's manifest. maxFrames_ guard is load-bearing.

- `frameInterpolation`: Weighted settings precedence: --frame-interp wins; otherwise Play follows project RENDER.FRAMEINTERP, editing follows Editor Preference. Device still declines frame-by-frame when it cannot run (MSAA, no G-buffer) and says why once.

## sandbox/src/SandboxApp.hpp

### Legacy OCMAP support
- `levelIsLegacyOcmap_`: distinguishes levels loaded via old OCMAP format from ordinary OCWORLD path
- `entityLegacyDeform_`, `entityLegacySurface_`, `entityLegacyMaterial_`: store legacy entity metadata that the new OCWORLD format has no field for; surface index -1 means "the asset's own"; deform material reaches nowhere (deliberately left empty per onLegacyOcmapInstantiated comment)

### Play state preservation
- Transform snapshot is deliberately NOT a full reload (would discard unsaved edits made before Play)
- Visibility included because graphs can call Entity.SetVisible during Play (same kMeshRendererVisible bit that Details panel saves)
- Only TRANSFORMS, VISIBILITY, and SPAWNED ENTITIES preserved; not full component snapshot

### Input ownership
- `own_` recomputed once per frame (not re-derived from ImGui flags each frame) to centralize "who owns keyboard/mouse" logic
- InputState input_ is tested aver::InputState, separate from ImGui (Win32 backend returns 0/not-consumed so both see messages)

### Revision control threading
- Git queries run on detached worker thread; frame draw only latches cached results
- Never spawns git from frame (ProcessRun.hpp waits INFINITE; would freeze editor)
- gitAvailable() asked only from worker (magic static, first caller pays for `git version` subprocess)

### Mesh pick geometry
- Lazy-loaded on first click (not pre-populated for all meshes)
- Empty entry means "tried and unavailable"; fallback is bounding box, prevents retry every frame
- Reason: loadProjectMeshes discards CPU-side OcMeshData after GPU upload

### Loading screen placement
- Loading screen IS NOT SCENE STATE; unguarded because project can open and stream assets without ECS
- All five use sites (log sink, applyProject, dismiss check) are unguarded; guard positioning doesn't move it relative to settle counters

### LOD cluster cache
- One cut-assembled MeshHandle per INSTANCE (not per mesh; different distances select different clusters)
- Rebuilt only when selected cluster-id set changes
- lastUsedFrame drives stale-entity sweep without explicit destruction hooks

### GPU mesh shaders (--lod-mesh-shader)
- ASMain/MSClusterMain are byte-for-byte CPU ports of selectClusterLocal/inLocalCut
- Exists alongside (never replaces) CPU per-cluster path; CPU path is correctness reference
- Pipeline tried EXACTLY ONCE: failed compile or tier-0 device degrades to CPU path (logged once, not retried)
- Tier-0 devices without mesh shaders still run correctness reference CPU path when flag is on

### Depth proxy map
- Empty if no Trifactor module; every lookup answers 0, draws mesh unchanged
- Guarded where BUILT (with LOD ladder) but declared unguarded (needed by unguarded Voxi resolver call)



- **MULTI-SELECTION anchor+set design**: anchor is sel_/selEntity_, kept as sole reference point (read by Details, gizmo, outliner, copy, rename, status line); multiSel_ contains the rest plus the anchor itself as invariant. Forgetting the anchor in multiSel_ causes "delete removed all but one" bugs.

- **multiSet_ purpose**: O(1) lookup for multiIsSelected, queried per drawn entity per frame. Vector (multiSel_) maintains order for Select All ranging; set (multiSet_) was added to fix O(entities x selected) slowdown on large levels.

- **AlsoMoved design**: gizmo multi-move applies anchor delta to other entities. Originally recorded nothing, so Ctrl+Z snapped only anchor home, leaving rest dragged and redo unable to repair. Now one command for the whole multi-move (one gesture, not N), stored as LOCAL transforms to avoid drift through conversions.

- **EditCmd serial**: monotonic identifier per edit, identifies document state for save point (levelHasUnsavedEdits). Never reused so undo/redo correctly cross save points.

- **hadBody/hadCollide/hadSnapZ**: authoring flags (nocollide, snapToGround) not stored in EntitySnapshot component. Held in entityCollide_/entitySnapZ_ maps because no component for captureEntity to find. Fixes destroy-undo bug where destroyEntity erased both maps and undo lost nocollide flag, silently re-solidifying entity.

- **hadVehicle not in snap**: vehicle preset (empty = not a car) held by level_, not EntitySnapshot. Only restored by recreateFrom; Copy and Duplicate make ordinary meshes.

- **hadAnim not in snap**: object animation not in EntitySnapshot because Play advances CAnimator clock, and entityAnim_ holds authored values; snap would go stale.

- **MaterialDesc whole payload**: EditCmd stores entire MaterialDesc before/after, not just the changed field. Controls interact (ior/reflectance, alpha/transmission), so replaying one field could restore invalid state that never existed. Undo depth 64 bounds the storage cost.

- **LandscapeStroke rect diff**: stores only touched sub-rectangle before/after, not whole 512x512 section (1 MB each). One entry per stroke, not per frame; rect unioned across stroke to collapse 60-second drag into one Ctrl+Z instead of 60 Ctrl+Z.

- **FoliageStroke batch**: every entity created/erased in one stroke as one entry, not per-instance. Brush places dozens/second; per-instance Create would cost a hundred Ctrl+Z per drag. recreateFrom() undoes erase from snapshots.

- **Destroy descendants order**: stored as parents-before-children vector. Fixes data loss: World::destroy retires subtree, but one snapshot could only restore one entity. Delete table with lamp parented, Ctrl+Z would return table but lamp gone permanently.

- **DestroyedNode parent indexing**: parent is vector index into subtree (-1 = root), not EditId. Whole subtree rebound in one go; EditId would have to be re-resolved mid-restore.

- **PlayerStart design**: Level format already had hasSpawn/spawnX/Y/Z/Yaw, unread. ONE RECORD, not a list (two sources of truth is bad). Transient entity, never pushed to levelEntities_ like drone.

- **cbRenameRepoint default ON**: rename dialog repoints found references (default behavior). Checkbox lets tools editing others' files opt out.

- **AutosaveState three states**: save is synchronous (onUpdate runs BEFORE onRender). "Saving..." notification raised and saved same tick would disappear before buildUI renders it. Pending state spends one presented frame before writing.

- **Autosave interval TEN MINUTES**: was 30s until countdown made cadence visible. Changed because saveLevel no longer calls markLevelSaved (sidecar ≠ real save), so timer restarts immediately; 10s warning on 30s = visible countdown a third of every minute. Ten minutes matches Unreal's interval.

- **Preference autosave 0.25s debounce**: maybeAutosavePrefs runs unguarded, persists editor settings (fly speed, wireframe, tile size), not level/world. Was 2s (too long), now 0.25s. Dragged slider dirties every frame; 0.25s collapses 60-frame drag to 4 writes.

- **--no-editor-chrome purpose**: suppress editor chrome for viewport screenshot comparison against AverEngineRuntime.exe. Run-scoped, never persisted.


- `snapSelectionToFloor` is guarded on both AVER_MODULE_SCENE and AVER_WITH_IMGUI despite being a declaration visible outside those guards. The original comment explained this: the declaration is visible at the call site (editor::CommandId::SnapToFloor in the viewport chord handler), so both the scene module and the editor UI gate it — one call away from an unresolved external if the guards diverged. Latent not live.

- Authored visibility stores OcWorldPlacement::visible, distinct from session-only H-hide (hideSelection/isolateSelection/unhideAll). An entity in editorHidden_ still saves/reopens as visible because the bit alone is the truth after an authored edit supersedes temporary H-hide.

- The `--autosave-test` latches live outside AVER_WITH_IMGUI because maybeAutosave (SandboxAutosave.cpp) reads autosaveTestLift_/autosaveTestWarned_ from a body guarded on AVER_MODULE_SCENE alone. The flag marks the level dirty once, then lets it run and lifts the capture guard loudly.

- `runGraphPrintTest` drives the on-screen graph-print feed's SINK. Not reachable from headless suite because logSink is a static member installed into the core logger, depending on this instance's deque. Logs through AVER_INFO rather than poking graphPrints_ directly, so the "[Graph] " prefix match is under test too — a mismatch with GraphInterop would write to the log and never reach the overlay silently.

- `runProjectSwitchTest` is synthetic: drives applyProjectRenderSettings with two hand-built ProjectDescs rather than real .ocproject files. Tests that a project with NO render settings must not inherit the previous project's settings. It falsified a regression: projectBackend_/frameBudgetMs_ used to be assigned BELOW the hasRenderSettings() early return, causing a bare project to inherit both and write the inherited backend into its own manifest on the next unrelated edit.

- `clearShaderCacheDir_` path is explicit to make the command checkable end-to-end without deleting real blobs (the dev's own 59MB cache would cost an unwanted recompile). CI/a build server needs this option.

- `runAssetAssignTest` tests the asset picker's three assignment helpers headlessly. THE MATERIAL CASE IS THE POINT: a material is an INTERNED NAME TOKEN while mesh/effect are fnv1a64 path hashes. A path hash written into mr->material fails SILENTLY — resolves to no surface or an unrelated one, and the entity renders wrong.

- `runGraphHitsTest` drives the node-hit chain end-to-end through the REAL bridge. The managed half (NodeHitTests.cs) has its own tests (branch arms, diamonds, graph bleed) which can't prove the ABI side: that the two new exports bind, a graph name marshals, the "nodeId:age;..." payload round-trips, and disarming stops it. Driven through graphLoad/graphTick (one graph, one entity, no class registry/Play state) — the smallest thing that runs real compiled IL.

- `pick()` returns true when click landed on something (entity or placeholder), including the Ctrl-toggle path. Returns false on Ctrl-click MISS only, which leaves sel_/selEntity_/the multi-selection untouched. The reason: if it returned true for a miss, a Ctrl-click would erase the selection AND return true (same as a hit), making them indistinguishable.

- Asset picker: Assigning an asset used to mean dragging it out of the Content Browser (closing that drawer made reassignment impossible), and only CParticleEmitter::effect even had a drop target. CMeshRenderer::mesh printed a hex id and offered nothing; CMeshRenderer::material offered nothing either. A button beside the field bypasses all of that. ONE GENERIC WIDGET over (label, id) candidates, not a picker per field: the three fields differ only in where candidates come from and what an id MEANS. Shaped after the graph editor's node palette (search-on-open, case-insensitive filter, capped list that says so). Candidates come from maps a project reload clears (meshPathById_, surfaceMaterials_), so they are never kept beyond the popup that asked for them. The caller builds them only while the popup is OPEN, and PickerCands holds them across consecutive frames the popup stays open — a closed popup costs nothing, and an open one does not re-read the project's Materials folders or re-sort every mesh path each frame.

- `cbRewriteReferences` rewrites oldRel to newRel in every text asset that anchored-matches it, in place. WHAT THIS CANNOT DO: A reference stored as a HASH with no path text (.ocmat TEX `{guid:0x...}`, save-game fnv1a64 ids) — nothing textual can find or fix those. Anything outside the five text formats the scan reads (.ocmesh material slots, .ocbt's string table, a C# file building a path in code). A material named by FILENAME STEM rather than path (materialForSurface probes three dirs). NOT TRANSACTIONAL: N separate file writes, each atomic (writeFileTextAtomic) so no individual file is torn, but a failure partway leaves some updated and some not — failure count is returned so the caller can say so.

- Outliner row cache was measured against a 12k+-entity import (Jungle Ruins: terrain tiles plus thousands of alpha-masked foliage instances). buildOutlinerPanel used to redo three passes over every entity PLUS a string-compare sort unconditionally, on every single ImGui frame, even an idle one — two std::string constructions per entity (w.name(ent) just to test emptiness, then outlinerLabelFor(ent) again) and two UNRESERVED hash containers that rehash repeatedly as they grow. outlinerRows_ owns the rows; outlinerChildren_/outlinerRoots_ point INTO it, so rebuildOutlinerCache() always replaces all three together (swaps a freshly-built vector into outlinerRows_) and never appends to outlinerRows_ in place, which would invalidate those pointers.

- Outliner cache staleness detection has two halves: outlinerSignature() is an O(1) stamp of what the editor itself can see change (entity count, filter, label-map size, the level's entity list); refreshOutlinerCache rebuilds when it moves. outlinerAuditDiverged() dense slots re-fingerprinted against outlinerSlotSigs_ (one fingerprint per dense slot, recorded by the rebuild): a SLICE per frame, so everything the stamp cannot see (a script rename, a reparent, a mesh added) is still caught within one audit cycle at a bounded cost per frame, and ALL of them the frame the editor's own edit stamp (outlinerEditMark) moves, so an edit made in the UI shows up at once.

- `initialTool_` defaults to Move rather than Select. Select draws no gizmo (drawGizmo's tool_ test), so opening in Select gave a freshly picked object nothing to grab and no hint that 2 would summon one — "I cannot move things" was accurate, not a misunderstanding. Unreal keeps a gizmo up too. 1 and --tool both still override this.

- Copy/Duplicate clause: ONE COPIED ENTITY, split out of EditorClipboard so the clipboard can hold a LIST. Ctrl+C used to read the anchor alone, so copying five props and pasting produced one — the same "applies to the set, acts on the anchor" shape that made multi-move unundoable, the bug Ctrl+D had.

- ClipboardEntity copies EVERYTHING UNDER IT TOO. Copying only the entity the selection names meant pasting a parent produced a childless copy — silently, since the paste looked like it worked.

- A pasted/duplicated ROOT came back solid, unsnapped and unanimated without hadCollide, hadSnapZ, snapZ, and hadAnim (the root's non-component flags and object animation, as EditCmd carries them; its children already keep theirs through `subtree`).

- EditorClipboard entities vector is empty when nothing was copied — this replaces the old hasScene bool outright. THE PLACEHOLDER PATH STAYS SINGLE-ITEM: objects_/MeshObj (no-project path) has no multi-selection concept — multiSel_ lives behind AVER_MODULE_SCENE, so there's no set to copy.

- Material edit snapshots desc before interaction (not when a control reports it was activated) so the timing (frame-start snapshot) is under test rather than the editor control's timing.

- KeybindRegistry is a REFERENCE to the one registry, not an instance: asset-editor tabs reach the same object via editor::keybinds(). Two registries would mean a Preferences-page rebind silently missing tabs — the "rebindable unless you are in a tab" split this promotion removes.


### Key invariants and design decisions preserved in code

- `makeEntityLabel()`: Creates entity labels from surface names or asset stems; `saveLevel` uses it to distinguish generated from renamed labels.
- `EditorMode`: Viewport mode selection. UNGUARDED even though Landscape is module-gated because mode switch/viewport hint/input dispatch all read it unconditionally.
- `firstPersonPawn_`: The pawn viewed in first person this frame. RESET UNCONDITIONALLY at top of drivePlayCamera() -- failure to reset creates a stale-handle bug shape (matching float-cannot-hold-handles hazard).
- `skyZenith_/skyHorizon_`: Copied OVER sky_ every frame, not from it. Prevents rhi::SkyAtmosphere's defaults from surviving frame 1 for levels without a SKY record.
- `postPushedValid_`: Invalid until the first push, so frame 1 never mistakes device defaults for an edit.
- `matchFogToStreamRadius_`: Recomputes fogDensity from the streaming load boundary (opt-in via Height Fog panel). Solves averFogFactor's k<=1e-8 branch exactly, not an approximation.
- `colliderMesh_` / `colliderMoverMesh_`: Two separate meshes because static and kinematic/dynamic bodies are tracked separately.
- `colliderOverlaySig_` / `colliderRev_`: The static audit covers all change paths; any body motion (script SetBodyMotionType or teleport) changes neither body count nor colliderRev_.
- `occlusionCullEnabled_`: MANIFEST MIRROR (RENDER.OCCLUSIONCULL round-trips through this bool). UNGUARDED alone on its side because OCCLUSION=OFF tree must not silently rewrite someone's OCCLUSIONCULL 1 to 0 on settings edit.
- `occlusionDebugForceWaitIdleArg_`: DEFAULTS TRUE; the no-wait path measured worse without a found cause.
- `occlusionBasisCamPos_` / `occlusionBasisForward_`: MOTION-SAFE CULLING. The pyramid was computed from (at best) one-frame-stale camera; stashed after buildPyramid() runs, same "camera value as of last look" idiom chunk streaming uses.
- `occlusionBasisRect_`: The scene's sub-rect that produced THIS basis's pyramid. A dock-layout drag between build and readback is a discontinuity like a teleport.
- `giOverride_` / `rtOverride_` / `ptOverride_`: All use -1 for "flag not given", NOT 0 (0 = Quality::Off, must be expressible). Changed from 0 to fix `--gi 0`/`--rt 0`/`--pt 0` being silent no-ops.
- `giSkyOccRaysOverride_`: -1 = flag not given, because 0 is a real value (fall back to cone gather's own occlusion).
- `skyLightOverride_`: NEGATIVE means absent; 0 is a real, meaningful request (no sky ambient). Flag was added to make this measurement possible.
- `giIntensityOverride_`: Negative means absent; 0 is a real request (bounce off, direct only).
- `averSrQuality_`: OFF (default) = no AverSR: no render-scale change beyond --render-scale, no SpatialUpscaler construction.
- `averSrFromCli_`: True when --aversr set the value, so loadEditorPreferences must not let stored preference overwrite the CLI flag.
- `averSrCliAuto_`: `--aversr auto` tells updateAverSrAuto to let manifest/ladder chain decide the level every frame, per "preferences are ignored; manifest and ladder apply" contract.
- `averSrChoice_`: User's own Display preference (Auto and Manual besides four named levels). Loaded once from display.aversrChoice, migratable if absent. Per-machine quality choice like AverSR's Display setting.
- `averSrCookieTripped_`: Set once when a stored display.renderScalePending cookie is found still armed (session's "a named level did not survive its own launch" case); forces Off for REST OF THIS SESSION, regardless of averSrChoice_/manifest/ladder.
- `averSrMigrationNoteArmed_`: Set true only when migrateAverSrChoice ran on genuinely ABSENT display.aversrChoice and landed on Auto (first session after U2's migration). Cleared when user picks any Display AverSR combo item.
- `averSrArmedNonOffOnce_`: Arms display.renderScalePending on first updateAverSrAuto applying a non-Off level THIS SESSION -- extends crash-cookie protection to Auto mid-session (Manual/named-level LOAD already get it).
- `averSrSource_`: Why the currently applied level is what it is (Auto/Manifest/User/Cli/ForcedOff). Read by Project Settings line and Display combo's "Auto (<level> from <source>)" label. Guarded on VOXI: no ladder without Voxi.
- `frameInterpCli_`: Who decides, highest first: --frame-interp, project RENDER.FRAMEINTERP, Editor Preference (off by default).
- `frameInterpTrajectory_`: The gather's path and in-engine training (neurafi::Trajectory: 0 straight, 1 quadratic, 2 learned).
- `ptSceneViewWantEnabled_` / `ptTierSeen_`: REQUESTED state vs. ACTUAL state. syncPtSceneView() reconciles only from onUpdate() before beginFrame(), deliberately not a read of voxi::Settings::pathTracing.
- `ptSceneViewFromCli_`: WAS GIVEN ON COMMAND LINE. Sticky for session, separate from want flag (written by CLI, settings combo, toggle-test flags, AND project manifest). Answers which ONE asked -- manifest used to silently outrank a flag.
- `ptSceneViewSuppressedByRayDriven_`: A1: true while syncPtSceneView() holds ptSceneViewWantEnabled_ down because ray-driven primary visibility is painting the scene. Read by PT page's Quality-combo tag.
- `sunSweepFrames_` / `sunSweepDeg_`: The DRAG, not the jump -- turns sun's azimuth every frame for the rest of run. A capture k frames into measurement shows how long lighting takes to catch up with a sun that has stopped.
- `meshHeapDefault_`: Applies to createMesh calls made after it's set, read once at top of loadProjectMeshes before first mesh upload (not reasserted per frame).
- `lodShareVertices_`: Coarser LODs share LOD0's vertex buffer (createMeshSharingVertices), falling back to their own copy on refusal. 0 gives each level its own copy.
- `engineForSplash_`: Set at top of onInit and left set. loadingScreenActive() gates the write; Engine clears it on close, so one owner of "is it still up".
- `undenoised_`: NOT PERSISTED: reasserted every frame. --view-mode undenoised / dropdown's independent toggle forces a bundle of existing runtime knobs off (see onUpdate's UNDENOISED comment for list).
- `viewModeFromCli_`: True when --view-mode or --unlit set the view for this run. Stored viewport.wireframe/viewport.unlit neither override it on load nor get overwritten by it on save.
- `lastEffectiveRtRenderMode_`: Last frame's EFFECTIVE rtRenderMode, after onUpdate's auto-switch (wireframe_/G-buffer forcing raster, ray-hit/triangles forcing ray-driven). -1 = no frame run yet (never equals real rtRenderMode 0 or 1).
- `waterEnabled_` / `waterHeightCm_`: OUTSIDE the AVER_MODULE_FLUIDS guard: flag is parsed either way, so a build without the module can say "this build has no water" rather than silently ignoring --water.
- `playerStartIcon_` / `viewportIconsReady_`: Flag gates only the SPRITE (needs icon renderer and texture). Line meshes (capsule/arrow) are ordinary line meshes, independent of viewportIconsReady_.
- `levelPcgVolumes_`: Every PCGVOLUME the loaded level carried, kept verbatim. saveLevel used to build fresh OcWorldData, resetting unmodelled fields to default; starting from file and overwriting only editor-owned fields fixes all of those.
- `entityAnim_`: Each entity's authored object animation. CAnimator on entity is only the live clock; saveLevel reads this map, never the component (Play must not leak into a save).
- `animatedBodies_`: driveAnimatedBodies' list, kept between frames. Rebuilt only when it can have changed; three counts below are what it was built from.

## sandbox/src/SandboxAssets.cpp

- `loadProjectMeshes`: W4 (--mesh-heap) flag semantics: setStaticMeshHeapDefault must run BEFORE any createMesh call, not after an early return. Built-ins are created earlier at 2058-2091 and predate this call, landing on the Upload heap regardless.

- `onMeshLoaded`: W11 (--lod-share-vertices) flag tradeoff measured: vertex-buffer sharing measured against LOD ladder totals. On per-mesh createMeshSharingVertices: device may refuse (unsupported backend or h's vertices are compute-written like skin targets), caller MUST fall back to full duplicate, exactly as if flag were off.

- `onMeshLoaded` depth proxy: depth/voxel passes use coarser LOD selected on world error (threshold ~20cm = shadow-map texel), not triangle ratio (e.g. fir_sapling 393k tris 1.1x LOD0 with near-zero saving; moss 116 tris). Coarser level keyed on EVERY level's handle because --lod-select picks different levels per instance.

- `ensureLodMeshPipeline`: Material graph reopens latch: if a new graph appears after pipeline compile, earlier pipeline has no arm for it, so cluster-drawn mesh would shade stock while other draw shows graph. Reopen latch to rebuild.

- `ensureLodMeshPipeline` flag ordering fix: THE FLAG IS TESTED BEFORE LATCH IS SET. Previously latching first meant a single call while flag=false burned the one attempt. Current order makes a future reordering merely late instead of fatal.

- `ensureLodMeshPipeline` stage 3 register map (checked against giLayout() VoxiRenderer.cpp before deployment): t0..t3 cluster geometry (ClusterBounds/ClusterMeshletDesc/verts/tris), t4..t12 Voxi GI (reserves t6..t12 for TLAS/RT even when never read), t13..t20 material (table 1), u0..u3 Voxi volume-mip/accumulator/RT-history (reserved, never written), s0 material sampler, s1..s2 Voxi samplers (linear-clamp, comparison-linear-clamp), b1 PerObject, b3 VoxiFrame (pixel-shader only), b4 ClusterFrameCB (AS/MS only).

- `ensureLodMeshPipeline` pixel shader: composed from shared prelude + material prelude + optional Voxi GI prelude + source. Not static: recomputed when material graph changes. AVER_MATERIAL_GRAPH goes in TEXT (not -D) so pipeline has ONE averEvalMaterial function. SM6.5 paired with lower model AS/MS has not been tried; release is not the place to discover incompatibility.

- `ensureLodMeshPipeline` layered BSDF: uses voxi::Renderer::get().settings().layeredBsdf but Voxi-off build has no voxi::Settings. ActorPreview also passes false for same reason; coated material shades WITHOUT coat on cluster path when AVER_MODULE_VOXI=OFF.

- `warnDeadMaterialHandle`: NOT KNOWN TO FIRE. Investigation of white-under-motion artefact could not trigger this path and found different cause. Latent hazard closed on inspection; if this appears in a log, that is new information worth chasing. Used to render as bright mirror (multiplicative identity baked in), now falls through to named look or flat fallback.

## sandbox/src/SandboxMain.cpp

- `createApplication`: split from SandboxApp.cpp on 2026-09-16; part contains entry point and command-line parsing.

- `setNoWalkCacheArg`: forward-declared because it is defined in SandboxRender.cpp beside the two drawWorld call sites it gates.

- `ownerProjectOf`: walks up to 8 levels; a stray level can't reach the drive root. Needed because a level names mesh/material/class placements relative to the project's content dir, so a bare .ocmap with no project would resolve none, reading as corrupt rather than missing-project.

- Command-line parsing split into four chunks to avoid MSVC C1061 block-nesting limit. The first wave of loops parses flags that consume values without consuming them (to avoid fallback path confusion). Later chunks run their first-match only if no earlier chunk matched.

- `--rd-ablate N`: removes one term from ray-driven pixel shader for cost attribution by difference. Non-zero values render deliberately wrong frames.

- `--gi-mode`: -1 means "not given" (0 is the real value, "voxel cones"), allowing A/B against manifest-set values.

- `--resize-cycle N`: reproduces a real crash (SandboxApp::resizeCheck). Verification-only.

- `--notify-test N`: raises one notification of each severity N frames in; lifts capture suppression so the stack draws.

- `--lighting-legacy`, `--pt-legacy-env`: seed console slots directly; needed because --frames has no console to type into.

- `--luma-sweep [STRIDE]`: stride keeps long runs under the MCP's 60-line grep match cap (aver_mcp.py's `matched[:60]`).

- `--firefly-metric [MULT]`: outlier threshold; shares --luma-sweep's stride since both drive the same per-frame readback.

- `--clear-shader-cache`: path optional; consumed only if it doesn't look like a flag (otherwise "--clear-shader-cache --frames 4" would take --frames as directory).

- `--mcp [port]`: uses strtol to avoid silent truncation (`--mcp 99999` would bind port 34463 mod 65536). Port 0 treated as explicit (distinct from absent). Not fatal if invalid; channel is a debugging aid.

- `--no-occlusion-cull`: reapplied every frame because manifest OCCLUSIONCULL may override. One-shot CLI setter insufficient.

- `--no-walk-cache`: disables WalkLookup's four per-mesh-id GameContent probes (meshFor, boundsFor, meshDefaultMaterial, partsFor), memoised per mesh for one drawWorld call (DrawWorldOptions::useMeshLookupCache).

- `--sun-set-at`, `--sun-sweep`: verification-only, see sunSetAtFrames_ and sunSweepFrames_.

- `--gpu-timing`: gets its own full loop to handle trailing flag (bare `i+1<argc` would silently ignore a trailing --gpu-timing, the --no-rt/--no-gi bug this file paid for).

- GI/RT/PT quality tiers 0..4 (Off..Epic); -1 = "flag absent", so 0 (a real tier) can be expressed. Fixed earlier defect where `--gi 2` set High and dropped "2" to positional handler.

- `--gi-cone` vs `--no-gi`: the former only stops PSMainVoxi/PSClusterMain reading the cone, the latter also prevents volume build.

- `--rt-shadow-denoise`: spatial filter radius (time is the tile edge above).

- `--rt-render-mode 0|1`: which thing finds first surface (0 raster, 1 primary rays).

- `--pt-bounces`: spent only while pathTracing is on (VoxiRenderer enforces).

- `--gi-update-interval`: 1 (unset) rebuilds every frame; measures voxelise+filter amortisation on its own per "measure each change, don't stack guesses".

- `--render-scale`: clamped to [0.25, 1] by device; isolates render-scale/GI-cost tradeoff.

- `--aversr LEVEL`: stored as string, so builds with module OFF still parse and explain why.

- `--frame-interp 0|1|2`: 2 = on and captures take GENERATED image. Needs 1x anti-aliasing; device logs why when it can't run.

- `--frame-interp-trajectory linear|quadratic|neural`: path the gather follows (NEURAFI.md §3.5).

- Neural Visualiser views: --neurafi-view 0 off, 1 sources, 2 confidence, 3 path bend, 4 network share; --neurac-view 0 off, 1 cached light, 2 coverage, 3 cascade, 4 cell state.

- `--depth-prepass`: same-frame depth-only pass ahead of opaque walk; occluded fragment skips PSMainVoxi's shadow/cone/fog.

- `--gbuffer`: thin forward-pass G-buffer (velocity, viewz, normals). Unset never calls it, so unmodified run stays bit-identical.

- `--occlusion-cull`: hierarchical-Z two-pass box culling. Unset reproduces pre-existing behaviour bit-for-bit.

- `--edge-aa`: FXAA upscaler through IUpscaler seam --aversr uses. A setting, not MSAA replacement: runs whatever sample count --msaa asked.

- `--drawer`: kind compared before colon; fixed bug where "--drawer console:gi" failed strcmp (v was whole "console:gi").

- `--gi`, `--rt`, `--pt`: bare flag = High; `-1` = absent. Fixed defect where bare form parsed no numeric.

- `--cam-wobble DEG PERIOD`: see setCamWobble. Measurement-only; 0 degrees = no motion, so existing captures are bit-identical without it.

- `--rt-rays`: nested sequence (1, 2, 4, 8) for convergence measurement.

- `--rt-pixels-per-ray`: amortisation tile edge; 1 (unset) traces every pixel every frame.

- `--landscape-gen`: sampleCount must be (k*64)+1; default 257 is three LOD levels, small enough for --frames run.

- `--landscape-gen` terrain: centred on origin so section sits under existing level placements rather than requiring level authored around terrain.

- `--sculpt-test`: eight ticks at 0.25 amount (~130ms held, handleSculpt's dt*6 rate), not one amount=1 jump, so result depends on strength/radius.

- `--water`: opt-in; water plane is infinite sheet and would surprise every level ever opened.

- `--no-editor-chrome`: draw scene only, no grid/gizmo/overlay. Not routed through showGrid_ (persists to editor.ini; comparison run must not change next interactive session). Run-scoped override read directly.

- `--scene-census`: AverEngineRuntime.exe prints identical format for scripts/verify-game.ps1 to compare.

- `--open-level`: convenient way to start on non-start-map; only way bounded --frames can exercise the picker funnel (picker itself needs click).

- `--coat`: scenes worth measuring (Floor/Cube, --furnace-grid) built in C++; .ocmat would test parser, not shading.

- `--landscape-gen`, `--sculpt-test`, `--import-gltf`, `--upgrade-project`: exit after completing their one task, touching no device.

- `--project-settings-page N`: verification-only; see setProjectSettingsPage comment.

- `--scroll-prefs-to-keybinds`: see buildEditorPrefs() comment.

- `--reload-scripts [N]`: fires once, N frames in (default 20).

- `--chunk-stream [N]`: stream N frames in (default 5); "wait to settle" pattern like --reload-scripts uses.

- `--drone-graph`: .ocgraph the drone runs; without it drone spawns and sits still.

- `--landscape`: overrides LANDSCAPE record/levelname.ocland convention so --frames can prove LOD without level file naming one, without human clicking.

- `--no-chunk-stream`: explicitly off, distinct from 0 (flag not given). Otherwise default would be unturnoffable.

- `--fog-match [opacity]`: ties fog density to streaming radius; opt-in since effect is markedly foggier.

- `--drone [N]`: switches drone on N frames in (default 5); proves it headlessly without human menu click.

- `--undo-test [N]`: runs runUndoTest() N frames in (default 10), then exits with 0/1; longer delay since needs live scene::World.

- `--keybind-test write|read [N]`: fires runKeybindPersistTest() N frames in (default 10), then EXITS THE PROCESS with 0/1.

- `--drawer log|content[:<sub>]|console[:<seed>]`: opens bottom drawer. Console (verification-only) with input pre-seeded because screenshot script has no mouse.

- `--actor-live`: start with LIVE enabled.

- `--project-settings`: opens project settings.

- `--editor-prefs`: show editor preferences.

- `--mcp [port]`: opens editor control channel. With no number, port from mcp.conf or 45123.

- `--shader-source <dir>`: set before device creation since first shaderFile() happens while renderer builds pipelines.

- `--pt-legacy-env`: seeds console slot; needed because --frames has no console.

- `--pt-scene`: visualization; different question from --pt (path tracer on) and render mode (raster vs ray-driven).

- `--particle-stress <N> <M>`: verification-only.

- `--no-particle-gi`: exclude particles from GI.

- `--pt-quality-ramp [N]`: raise PT rung every N frames.

- `--pt-scene-toggle-on`, `--pt-scene-toggle-off`: toggle scene view on/off; optional N (default 5 and 15).

- `--aversr-cycle [N]`: toggle AverSR at frame N; reproduces a real crash (SandboxApp::averSrCycleFrames_).

- `--bake-nav [cellCm]`: deferred to frame (not init) since bake samples physics and applyProject (builds level bodies) hasn't run yet.

- `--lod-select [px]`: virtualized geometry per-instance LOD; measured at 102.7ms median vs 76.7ms with selection (same camera).

- `--no-lod-select`: every instance at LOD 0; kept as escape hatch for selection artefact vs real one.

- `--lod-cluster-stats`: per-meshlet frustum/cone-cull counters; separate flag (see lodClusterStatsEnabled_).

- `--lod-per-cluster [px]`: replaces --lod-select when both given.

- `--lod-mesh-shader [px]`: GPU per-cluster path (amplification+mesh shader); wins over --lod-per-cluster/--lod-select if live; falls back per-instance otherwise. 2.8x speedup measured.

- `--no-lod-mesh-shader`: force GPU per-cluster OFF; needed because path now ON by default (where supported).

- `--clouds`: optional coverage value.

- `--sky-physical`: derived sky with optional sun elevation (degrees).

- `--probe-rel`: probe relative coordinates.

- `--cam`: camera position and orientation.

- `--mode`: editing mode to start in; automation/screenshots need UI-less start (mode's panel can't be captured via mouse click). Applied after project loads since mode can be refused for lack of terrain.

- `--tool`: paired with --mode; see continued parsing.


- **Bare-path argument handling**: The shell entry point must recognize every asset type the engine registers (e.g., .ocmap, .ocbeam, .ocproject) by extension. Previously .ocmap fell through to beam-mode and loadOcbeam refused it, opening an empty editor.

- **Single-instance forwarding, sender-side gate**: The narrowest gate that could work is `argc==2 && argv[1][0]!='-'`. This excludes gate-sweep/import/scaffold workflows (which have many flag-bearing processes) and `Sandbox.exe level.ocmap --frames 600` (which would interfere with capture). The project/openMap check excludes bare .ocbeam (which has no file association to forward). Resolved paths are absolute because the receiver's working directory may differ.

- **Single-instance forwarding, receiver-side gate**: The receiver gate is a superset of the sender gate (allowing bare .ocbeam to be eligible as primary, though excluded from forwarding). Reading only argc/argv[1][0] keeps the two gates in lockstep automatically. Failure paths (no primary, declined, timeout) fall through to launching the application as if forwarding was never attempted.

- **--crash-test mechanisms**: Critical kind logs Critical and exits cleanly (verifies the quiet path for the reporter). Other kinds end the process and use volatile to prevent null dereference from being optimized away in release builds. The oom kind uses operator new rather than throwing bad_alloc because an escaping exception on Windows raises SEH 0xE06D7363, taken before std::terminate. The throw kind deliberately tests that mechanism.

- **--lighting-legacy / --pt-legacy-env**: These flags seed raw console slots directly (EditorConsole.hpp), the same slots that `set voxi.legacyRestirSampleRing` / `set pt.legacyEnvironment` write to. Per-frame reasserts beside `voxiRenderer_.setLightingLegacyBits(...)` / `syncPtSceneView(...)` make them live. consoleLightingLegacySlot() is guarded on AVER_MODULE_VOXI; consolePtLegacyEnvSlot() is not, since its path tracer must keep working with AVER_MODULE_VOXI off.

- **Engine-optimisation-plan wave 1 (C-6)**: The giForceRebuild, giBoundedDispatch, and giFreeAccumulator slots are seeded the same way as lighting-legacy. They are guarded on AVER_MODULE_VOXI because the slots live in EditorConsole.hpp's AVER_MODULE_VOXI block (C-5) and feed only VoxiRenderer methods.

- **Optimisation-wave-2, U1/section 4(a)**: The giVisPathView and blendedGiCone slots are seeded similarly, with per-frame reasserts beside `voxiRenderer_.setGiVisPathView(...)` / `setBlendedGiCone(...)` in onUpdate making each one live for --frames captures with no console.

- **--mesh-heap and --lod-share-vertices**: Both are routed through SandboxApp members (not device calls) because no rhi::IDevice exists yet in main(). They are read at loadProjectMeshes, the first place a device is guaranteed to exist.

- **--aversr "auto"**: Explicit CLI Auto (plan 3.3 A), distinct from never passing --aversr (in which case averSrFromCli_ still goes true, and loadEditorPreferences leaves the stored Display choice alone). No single sr::Quality::Auto value exists for parseQuality, so this is checked before that call.

- **--no-walk-cache**: Not wrapped in app->setXxx() because setNoWalkCacheArg is a free function with a forward declaration at the top of this file. See GameRender.hpp's DrawWorldOptions::useMeshLookupCache for what it gates. With no --no-walk-cache, the cache-on default here is both hosts' current behaviour and DrawWorldOptions' own default.

- **--occlusion-waitidle / --no-occlusion-waitidle**: If both are somehow passed, the opt-out flag (no-waitidle) is checked after, so it wins; see occlusionNoWaitIdleArg's own comment in the argument-parsing section.

- **--luma-sweep**: Not inside the AVER_MODULE_SR block above (--gpu-timing and its neighbours sit there for unrelated reasons). --luma-sweep has nothing to do with AverSR and must apply whether or not that module is built.

## sandbox/src/SandboxSelfTests.cpp

### Part 1 (lines 1-900)
- `setLodSelect`: virtualized geometry LOD selection with pixel budget. Measured on Electric Dreams --no-vsync: 102.7/153.0ms (off) vs 76.7/127.3ms (on) median/p90.
- `setSkyPhysical`: recorded as OVERRIDE due to applyLevelSky overwriting it wholesale. Measured --sky-physical 20 vs 60 on Sponza produced byte-identical frames; this is the 6th instance of flag-vs-authored-state precedence bug.
- `setPost`: argv defaults are exposure 1.0, bloom 0.0 (NOT PostSettings' own 0.06 default), so sandbox has always run bloom-off unless asked and every gate image was taken that way; conditional assignment would move all 20 gate images.
- Synthetic input for play tests injected into input_ (where publisher reads), not ImGui queue. HELD buttons persist, EDGE is rolled before publish sees it.
- PIE camera test catches round-trip quaternion inversion through pawn transforms; drivePlayCamera derives yaw_/pitch_ from pawn forward vector, fly control writes back; if inversion isn't exact, angles walk.
- Wheel speed test: wheel read was in onUpdate before ImGui's NewFrame, and ImGui::EndFrame zeroes io.MouseWheel - so read was always 0.0f. Control was dead for months with no automated coverage.
- Input stuck test: pushInput() used to return after aver_fw_input_new_frame() (which doesn't clear cur[]) - keys froze at last value while aver_fw_tick kept running; weapon gated on MOUSE_LEFT fired forever.

### Part 2 (lines 901-1800)
- Viewmodel test: gun is rigidly parented to eye, so view-space position constant by construction; any deviation means transforms came from different data.
- Recapture test: release-mouse chord then click back into viewport = "give game mouse again" and must not ALSO reach gameplay.
- Copy/Paste multi-selection: copy was separate code from duplicate and read anchor alone; clipboard physically held only one entity, so Ctrl+C then Ctrl+V silently produced ONE.
- Parent+child copy: selected parent+child pastes as TWO not three - captureSubtree already carries child, separate clipboard entry would double-paste.
- Graph print test: don't log while logMutex_ held (deadlock with non-recursive mutex); read under lock, release, then report.
- Graph hits test: last entry must survive marshalling - GraphGetHits truncates at separator; first version did this unconditionally, silently dropping payload's final entry.

### Part 3 (lines 1801+)
[Incomplete due to brief size]

## sandbox/src/SandboxSettings.cpp

### Post-process is VIEW property (part00, lines ~55-69)
Post process (exposure, bloom, etc.) is a property of the viewer, not the level. Each observer should have independent eyes. This is why these settings persist in editor.ini and are gated off on capture runs (--frames), since stored exposure could change the rendered image machine-to-machine.

### Post-exposure versions and migrations (part00, lines ~84-94)
Version 2: post.exposure meaning changed from raw multiplier to compensation on Eye Adaptation. settingsVersion < 2 means .ini predates change.
Version 3 (2026-09-28): exposureKey was halved (owner request to make current -1.0 the default). Stored compensation doubles to keep picture the same.

### AverSR crash-cookie mechanism (part00, lines ~114-130)
Render scale below 1.0 can lose GPU device at startup. renderScalePending flag is written before risky call and cleared 30 frames after success. Scale resets to 1 (visible) rather than silently skipped. Detects device loss via crash cookie. Migration from old display.aversr/renderScale pair to display.aversrChoice (unit-tested in AverSrChoiceTest.cpp).

### NeonDistrict installed (part00, lines ~289-291)
Window size was expanded 460 -> 640 because six sections (Keybinds added sixth) no longer fit old height on typical monitor without scrolling. First-use-ever default; users who resized keep their own size.

### Output log filter sync (part00, lines ~268-271)
Same persisted variable as Output Log drawer's combo. Once lagged behind when Error+/Critical+ were added.

### View flags and snap (part00, lines ~34-39)
View flags gated to interactive runs only (not --frames runs). Snap is ungated since it can't alter pixels.

### sandbox/src/SandboxSettings.cpp (part01)

### TheConsole dirty mark signal (part01, lines ~2-3)
Without dirty mark, only signal was button on another page going grey->enabled (nobody watches). Added visible "unsaved changes" text.

### Five pages not guarded by VOXI (part01, lines ~31-39)
Window/Import/Streaming/Physics/Audio edit manifest keys (WINDOW.*, IMPORT.*, etc.) but used to sit in #if AVER_MODULE_VOXI just because buildRenderingSettings did. Guard now wraps only Rendering page where it belongs.

### Physics "not ready" state (part01, lines ~212)
Project can be open before aver_phys_init runs. "Not ready" is real state, not error. Setters silently no-op until ready.

### Tick rate conversion (part01, lines ~228-229)
Stored in seconds (ABI unit), displayed as Hz. Conversion happens here only, file stays in ABI unit.

### Denoiser prerequisites and soft/hard reasons (part01, lines ~35-43)
Hard reasons grey controls (hardware/tier/backend). Soft reasons leave them clickable (MSAA). RequiresMsaaOne never greys, needs dedicated warning line.

### GI grid built at init only (part01, lines ~86-92)
VoxiRenderer::createVoxelVolume builds grid once at init. Nothing resizes live. Change recorded immediately but reaches GPU only on reload. Compared against voxelResolutionBuilt() so note clears once reload catches up.

### ReSTIR GI rendering (part01, lines ~183-188)
Inner grey: everything below inert while RT tier is Off. Outer grey doesn't cover this. Quality combo stays clickable since it's only control that can turn tier back on.

### Tile edge powers of two (part01, lines ~206-207)
setPixelsPerRayTile only rounds to power of two. Options 1,2,4,8,16 (1..256 px/ray).

### modules/render.voxi/shaders/voxi.hlsl and other rendering files (part02)

### ReSTIR history and cache (part02, lines ~2-10)
Indirect light history: how much previous-frame reservoir weighs into ReSTIR GI combine. Shows REQUESTED, not raw field (in case effective diverges later). Cache flushes nothing vs 400 volumes look identical from button perspective, difference is reason to press it.

### Denoiser is only G-buffer consumer (part02, lines ~35-43)
Denoiser filters ReSTIR diffuse and RT sky occlusion. Only thing needing G-buffer in engine. Greyed for hard reasons, left clickable for soft (MSAA fixable below).

### VoxelResolution measurements (part02, lines ~74-92)
64~6MB, 128~50MB, 256~400MB, 512~3.2GB. Each step up is 8x jump (cubed). Grid built at init (VoxiRenderer::createVoxelVolume), resize only on reload.

### Denoiser comparisons (part02, lines ~224-241)
Temporal amortisation (above) reuses THIS pixel across time (converges still, collapses motion).
Spatial filter (below) averages neighbours with no history (motion can't poison it).
They compose.

### Ray-driven visibility vs rasteriser (part02, lines ~328-356)
Shipped default Medium and above. Low is deliberate exception kept rasteriser by product decision (D3), not hardware gap. Low/Medium/High/Epic: rtShadowRays 1/1/4/8, rtPixelsPerRayTile 1 flat, rtShadowDenoise 2/2/1/1, rtRenderMode 0/1/1/1.

### Staged ray-driven (part02, lines ~358-387)
Single pass baseline kept as fallback. Staged: 40-45% faster. Staged+half-rate GI (default): further ~1.4ms faster, within 0.4% of Staged at rest, noisier in motion. Needs ReSTIR GI and denoiser; without them behaves as Staged. D3D12-only; others fall back to Single pass.

### Path tracing tier (part02, lines ~400-407)
Status mirrors PathTracer::init() gates (DXR-1.1/SM-6.5/DXC/compute). Tier drives accumulator resolution (480x270 up to 1280x720). Low..Epic.


### buildRenderingSettings() design and history

- **Overall Quality UI logic (lines 318-319):** Displays "Custom" in amber when any group drifts off the quality ladder, groups disagree on their tier, or Path Tracing is above Off. The overallFromSettings() function detects these conditions.

- **AverSR module boundary (lines 332-335):** AverSR sits outside Overall/Custom detection by design (Scalability.hpp declares this). An Overall preset moves GI/RT/PT together but never touches AverSR. The UI surfaces hidden pinned levels only when the source is not Auto.

- **Layered BSDF reload requirement (lines 484-487):** Changes what every material-shaded draw computes. Doesn't take effect until reload because Off compiles the coat lobe out entirely, and VoxiRenderer builds and latches one shader set per setting, so a live toggle would double a 20+ PSO matrix at startup.

- **Refraction control architecture (lines 509-511, 515-518):** These three culling knobs (refraction + others) were CLI-only with no control anywhere. A CLI flag could set them but nothing recorded the choice. Each is now one manifest key. Uses BeginCombo/Selectable, not plain Combo, because only RayTraced entry has a prerequisite (RT hardware, RT tier not Off), and a plain Combo cannot grey out one entry while leaving Off/Screen-space selectable.

- **Post processing comment block (lines 547-562):** Exposure/bloom/tonemap/auto-exposure existed only as CLI flags and live Post Process sliders before being added to manifest. A project could not author these. Filed under "Post processing" (camera/image control) on General page, not beside GI page's ReSTIR sliders, to avoid filing a control under its implementation instead of its job. Every control writes the key AND the live post_ together so the viewport shows what is being dragged. A live edit outranks a flag (opposite of load time, where --exposure wins because it is the newest thing said).

- **Post float lambda ordering (lines 597-598):** Registry tracked before tooltip because IsItemHovered reads the LAST submitted item, and a tooltip submits its own (same ordering rule as ReSTIR history slider).

- **Auto exposure amber state (lines 652-654):** Displays amber, not greyed, when adaptation is on but exposure is recorded. The recorded value still applies wherever auto exposure is off. A project shipping with adaptation off next month still wants it recorded today (different from live value adaptation is about to overwrite).

- **GI estimator placement (lines 774-788):** "Indirect diffuse" lives on Global Illumination page deliberately, not Ray Tracing. Used to sit on Ray Tracing because ReSTIR needs RayQuery, which looked like the dependency that mattered. It is not. Someone looking for the diffuse GI algorithm looks under Global Illumination and finds only cone-gather knobs—a control filed under its implementation instead of its job, reported as "I cannot find that project setting". Sits above the cone knobs since it decides whether they apply at all. giMode round-trips as RENDER.GIMODE; absent from an older manifest it stays -1 and the engine default (cones) applies, which is what every pre-existing project already meant. Displays er.giMode.EFFECTIVE, not the raw stored request, because giMode is not clamped inside Settings itself. A request ReSTIR cannot run right now is remembered (s.giMode untouched) rather than rewritten to 0, so RenderSettingsResolver.hpp's resolve() decides. Showing the raw value while cones ran instead would read as dead; the effective one switches back on its own once the reason clears.

- **ReSTIR visibility rays (lines 812-818):** Implementation detail: "optimisation-wave-2's U1" measured how much of F2 (candidate-hit sky) and F3 (reuse visibility)—the contrast fix's two per-pixel rays from commit cb4b48df—this ReSTIR estimator pays for. Shows er.giRestirVisibility.REQUESTED, deliberately not EFFECTIVE, per RenderSettingsResolver.hpp's own comment on this field: effective always equals requested for it (clamping to 0 on a failed prerequisite would read "No ray (over-bright)" while no ReSTIR runs at all). Inertness is shown by greying the control and naming the reason below, never by the combo silently jumping.

## sandbox/src/SandboxShell.cpp

### Part 1 (lines 1-~900)
- File header: removed history about splitting from SandboxApp.cpp and date
- resetPlayProfile: condensed explanation of profiler restart behavior
- IMGUI guard comment: simplified explanation of guard placement reasoning
- References panel: condensed description of on-demand scan capability
- Neural Visualiser: condensed two-line summary of NeuraFI and NeuRaC
- Profiler panel: condensed long explanation of panel contents to single line
- CPU phases loop: removed "Same thresholds" comment (obvious from adjacent GPU code)
- CPU timing: removed comment about division by framesAccumulated (clear from context)
- Accel structure loop: condensed tooltip comment
- GPU timing report: removed "Two different no data cases" explanation
- Exclusive time: removed comment about calculation method (clear from code)
- GPU avg timing: condensed tooltip about averaging window
- ScrollY: removed comment about height calculation (self-evident)
- Parent-before-child order: removed comment about depth computation (obvious algorithm)
- Exclusive column color: removed comment (code plainly shows color thresholds)
- Bake navigation: simplified to single line (details in RevisionControl.cpp)
- Revision control intro: condensed 10-line explanation to 2 lines
- Diffable assets: condensed function purpose statement
- Path flattening: condensed explanation of separator/case normalization
- History depth constant: shortened comment
- Folder rank function: shortened comment about status summarization
- Status bar face: condensed from 7 lines to 1 line
- revisionControlTick: removed project-switch explanation
- rcStatusJob reap: removed stale state explanation
- Badge table: removed 5-line explanation of status priority (obvious from code)
- rcDiffSide comparison: removed explanatory comment
- rcKeyFor: condensed description
- rcMarkFor separator check: removed 3-line explanation
- rcMarkFor folder summary: condensed to 1 line
- rcStatusColour: condensed documentation
- Revision control panel states: removed "three different sentences" comment
- Ahead/behind counts: removed comment about sign handling
- Git status two-letter codes: condensed comment
- Oldpath for renames: simplified comment


### Part 2 (lines ~900-1800)
- Untracked files history: condensed to single purpose statement
- Date printing: removed comment about ISO-8601 format (code is clear)
- Diff viewer: removed "said in words" comment about empty pane distinction
- Diff sides offered: condensed explanation of RadioButton pattern
- BeginChild/EndChild: removed 5-line explanation of return semantics
- Diff clipping: condensed comment about avoiding off-screen AddText calls
- Unified diff coloring: condensed comment about +++ vs - being headers
- Status bar widgets: condensed 6-line comment to 2 lines
- statusBarWidget: removed comment about SmallButton and row height
- Track by id: removed 2-line comment about stable tracking
- rcMoodColour palette: condensed 6-line documentation to 1 line
- Return disabled color: removed "Nothing to report isn't an alarm" explanation
- drawRevisionControlStatusWidget: condensed 2-line comment
- hudPreviewActive: condensed description
- setHudPreview: condensed comment about publishing behavior
- dropButton: removed comment about matched controls
- setUpgradeStatus: removed 17-line history of status bar implementation
- onCloseGuardThunk: condensed comment about message thread
- onCloseGuard: removed "already asking" explanation
- levelHasUnsavedEdits: condensed to single-line purpose
- markLevelSaved: condensed description
- markLevelUnsaved: condensed description
- requestExitChecked: condensed from 4 lines to 1
- saveAll: condensed from 7 lines to 1 line
- About dialog: removed kEngineName/kEngineVersion comment
- drawSaveLevelAsPrompt: condensed from 4 lines to 1 line
- Save As: copy ID comment condensed to essential info
- Lanes sidecar: condensed comment about lane file travel
- levelPath_ under AVER_MODULE_SCENE: removed redundant comment
- openLevelPickerNow: condensed to single line
- Pre-select level: condensed comment
- drawOpenLevelPrompt: condensed from 6 lines to 1 line
- --open-level-picker and --open-level: removed explanatory comments
- Current level marked: condensed comment about re-opening behavior



- `buildUI`: New Level now asks first instead of silently calling `unloadLevel()`, which used to discard the undo stack and every entity with no prompt on an accidental click.

- `buildUI`: Open Level menu item now opens a picker showing all levels. It previously called `loadStartMap()` (the project's ONE start level), making a project with three levels unreachable in two of them.

- `buildUI`: Reload Start Level is distinct from Open Level — it reopens the start map to discard edits. This is a real workflow but kept separate since Open Level now means something different.

- `buildUI`: Save Level As — the saveLevel() function already takes an arbitrary path parameter; this menu item provides the missing UI to name one.

- `buildUI`: Packaging menu item is disabled with a SPECIFIC reason rather than generic greying-out. Providing a reason prevents ambiguous feedback (greyed out with no explanation wastes time).

- `buildUI`: ImGui MenuItem() return value was previously discarded in favour of panels without `p_open`, making items consume clicks but never activate. This pattern prevented panels (like World Outliner and Details) from being reopened via menu.

- `buildUI`: Shortcut hints (Edit menu) are read from the same registry that handleManip() uses for keypress dispatch, so menu and keyboard always agree on what a chord does.

- `buildUI`: dockspace restoration: ImGui rebuilds the dock tree from io.IniFilename (editor-layout.ini) before the first frame. Rebuilding over a restored layout would mean the ini is written on every exit but ignored on launch — worse than not saving. The log message helps detect regressions (default layout == restored layout only on first launch, so invisible regression is a risk).

- `buildUI`: dock builder algorithm: split order matters. The central node must be split AFTER the right-hand splits, so the centre remains the passthrough node; otherwise the scene stops rendering.

- `buildUI`: ImGui::SetDragDropPayload() overwrites the type on each call (cond 0 = ImGuiCond_Always). The Content Browser used to set kAssetDragDropType then kCbMoveDragDropType on the same drag, so only the move payload delivered and asset-only drop targets silently ignored every drop. Now it sends the whole selection under kCbMoveDragDropType; kAssetDragDropType is still accepted from single-asset source.

- `buildUI`: Multiple paths from a multi-selection drag are separated by newlines. All land at the SAME point (cursor), not fanned out — moving them apart is a separate drag, and guessing a layout isn't undoable in one.

- `buildUI`: Mode panel (sculpting tools) goes LEFT, opposite Unreal's Details panel (right). Left answers "what am I doing", right "what is this object" — mixing terrain tools into the selected-entity panel would confuse the layout.


- `--select` capture verification: the message follows the result, not the call. Old code unconditionally reported "selected node", which let a capture run against a non-existent node pass verification. Now the message is printed only on success.

- `drawGraphPrintOverlay` submission order: writes into the Level window's draw list before ImGui::Image appends the viewport texture, so it gets painted then covered. Separate Begin() avoids this by using its own window.

- `drawConsoleTranscriptTab` history: was the whole of drawConsole() before a console rework split it into drawConsoleTranscriptTab and drawConsoleBrowserTab; the function shape is unchanged.

- Console variable suggestion ordering: shortest-match-first, not table order. Observed on real data: typing "voxi.gi" listed six specialisations (giCones, giSkyOcclusionRays, giSkyOcclusionTile, giIntensity, giMaxDistance, giUpdateInterval) and pushed voxi.giMode (the one most people actually want, since it chooses the GI estimator) off the end. Shortest-first needs no relevance scoring: within a shared prefix, the shortest name IS the most general one.

- Console tab bar focus: forced to Transcript front exactly on the frame consoleFocusPending_ is set. Not gated on ImGui::IsWindowAppearing() because the ##drawer window doesn't newly "appear" switching FROM Content/Log TO Console -- the flag specifically targets "just chose Console".

- VRAM over-budget measurement: GPU paging to RAM via PCIe measured denoiser performance impact: 0.63 ms (under budget) -> 3.40 ms (over budget).

- Notification opacity: set to 1.0 (fully opaque), not 0.92 or lower. Viewport bars sit at 0.62 (translucency reads as depth there), but notifications land on the Details panel. At 0.92 measured, text like "Transform", "Rotation", "Base Color" was legible straight through an error message. A notification competing with the text beneath it is one you misread.

- Log line styling palette rationale: one severity palette shared by Output Log and Console (not duplicated). "Four flat colours weren't worth factoring out, but six levels with two carrying a row background made the two copies' agreement load-bearing -- disagreeing means the same line reads as a different severity per drawer." Palette: Warn = bright orange NOT kAverOrange (product accent); selected control would be worse than yellow. Error = red. Critical = dark red on dark red row (hard to scroll past unnoticed). Fatal = black on red (black alone invisible on dark background; filled row makes it readable).

- Output Log filtering optimization: ImGuiListClipper optimization. Used to style and draw all kMaxLogLines (4000) every frame under logMutex_, which every logging thread queues on. Now filter is applied first to a list of indices (level compare per line), and clipper draws just the visible slice, holding the lock for a few dozen rows instead of four thousand. Multiline lines (e.g. shader errors) fall back to the old path: exact for any height, costs what it did while such lines are in buffer.

- Cluster button width computation: not hardcoded. Measured via ImGui's own layout to ensure all buttons fit and stay in stable relative positions. "Constants broke twice before."
