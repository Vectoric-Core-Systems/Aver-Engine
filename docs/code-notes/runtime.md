# Code notes: runtime

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## Runtime/include/aver/game/GameApp.hpp

### GameConfig struct comments

- The `title`, `width`, `height` fields use NAMED CONSTANTS (static constexpr members) so `config()` can distinguish "the caller asked for 1280" from "nobody said nothing". Without this, a project's WINDOW.SIZE can never win since the field never stays empty.

- `levelPath`: a bare .ocworld/.ocmap argument is the level to open instead of the project's STARTMAP, falling back to STARTMAP when the file does not exist. Same semantics as the editor's `openMapPath_`.

- `--stats [seconds]`: periodically logs the per-pass GPU breakdown that D3D12Device::initGpuTiming collects unconditionally (not behind a build flag), so every shipped AverEngineRuntime.exe has always timestamped every pass but threw the numbers away. The editor's console `frametime` was the only reader; a packaged game cannot include the editor. Implemented as a log dump, not an overlay, because a profiler for a shipped build is typically captured from a session and read afterwards, often from a machine you don't have. An overlay would also need the game UI's text path and would change what a --screenshot capture contains, breaking verify-game's census comparison. The interval is in SECONDS because the report averages over frames since boot and changes slowly; logging per-frame would be a flood.

- `--aversr off|quality|balanced|performance|auto`: CLI override for the render upscaler level, parsed locally (GameApp.cpp::parseArgs) into QualityLadder numbering (0/1/2/3) not through aver::sr::parseQuality. GameApp never includes aver/sr/* (render.voxi and runtime.game must never depend on render.sr, per Scalability.hpp boundary). -1 (DEFAULT) means "not given" and falls through to the project's RENDER.AVERSR and then the ladder's per-rung default. The host's own frame cost at reduced internal resolution has not been measured.

- `--frame-interp-trajectory linear|quadratic|neural`: trajectory mode for frame interpolation, using neurafi::Trajectory numbering. Default neural uses the shipped weights, employed only where measured to beat quadratic.

### Why GameApp is separate from SandboxApp

GameApp exists as a separate class because Sandbox.exe is the editor (the only aver::Application in the tree). Roughly 2,000 lines of what a game needs—content indexing, level load, world draw walk, tick-group ordering, play camera—live inside SandboxApp interleaved with dockspaces and inspector panels. A game cannot link SandboxApp, so this class is where the game half moves. Currently the window and frame loop; subsystems land in later commits (one per slice) so a black screen never has two candidate causes.

### AverSrInstaller function type

The composition root (Runtime/host/RuntimeMain.cpp) hands GameApp a function that builds a concrete aver::sr::SpatialUpscaler and reports the render scale for a resolved level. GameApp itself never includes aver/sr/* (matching module boundary). Called once by createApplication before Engine::run calls onInit. Null is the legal default for builds without the SR module—onInit's apply step still resolves a level but has nothing to install it with, so the game renders at native resolution as it always did.

### THE ASPECT FIX

The editor divides its DOCKSPACE CENTRAL NODE (vpW_/vpH_, latched by buildUI the previous frame) because its 3D view is one panel among many. A game has no dockspace or panels: its scene is the whole backbuffer, so the aspect is the swapchain's. This is the one thing in the entire lift that cannot be copied verbatim—copying would need members that only exist because ImGui exists.

### Method comments

- `openProject`: logs and leaves project_ invalid on failure rather than aborting. A game with no world is a diagnosable state; a process that dies before its first frame tells the player nothing.

- `attachVoxi`: also creates pbr::MaterialSystem as a member of VoxiRenderer (initialised only inside VoxiRenderer::init), so a game with no Voxi feature has no material system and draws every surface with fallback.

- `attachSkinning`: registered FIRST of all render features, so its prePass stages this frame's bone matrices before anything asks for a posed handle.

- `loadGameUiFont`: loads and uploads Roboto-Regular.ocfont staged beside the exe. Non-fatal: a missing or bad font leaves uiFont_ invalid and every aver_ui_text call draws nothing, never a startup failure. Cannot call SandboxApp.cpp's own loadGameUiFont because that uploads through the EDITOR's device, not this process's.

- `tickGameplay`: runs the gameplay tick groups around the physics step when a session is playing.

- `initScripting`: bootstraps the in-process CLR host for C# graphs. ScriptHost links to Runtime/CMakeLists.txt but was never constructed by anything in it before—only SandboxApp (the editor) ever stood it up. Now initScripting starts it; discoverProjectGraphs and tickProjectGraphs are the graph-specific pieces built on top.

- `discoverProjectGraphs`: walks the project content for *.ocgraph files and loads each through ScriptHost's existing, UNCHANGED entity-scoped graph API. A synthetic id stands in for a real entity; see GameApp.cpp for why that is safe. A project with no graphs is a silent correct no-op.

- `tickProjectGraphs`: ticks every graph discovered (called every frame from onUpdate). NOT gated on the framework's play state the way tickGameplay is, because a scripting-layer feature... gating it would make it silently inert in exactly the configuration most likely to be the only gameplay a project has.

- `beginPlayIfGameModeDeclared`: calls aver_fw_begin_play if a GameMode is declared. A shipped game has no editor Play button, so boot and play are simultaneous. Does not know what a GameMode IS. Asks the framework by FLAG bits already defined (AVER_FW_CLASS_GAME_MODE / AVER_FW_CLASS_GAME_INSTANCE) and already resolved (aver_fw_find_class_with_flags) whether the loaded project declared one. A project with no GameMode gets the no-op this engine always gave: aver_fw_begin_play never called, play_state stays EDITOR, framework exactly as inert as before this existed.

- `placePawnAtSpawn`: places the possessed pawn at the level's authored Player Start via game::placePossessedPawn (GamePawn.hpp), minus the PlayerStart MARKER lookup (a shipped game has no equivalent; see GameLevel::spawn()). Called from beginPlayIfGameModeDeclared after aver_fw_begin_play succeeds.

- `drivePlayCamera`: drives the camera from the possessed pawn. Must run AFTER World::flush and BEFORE the view matrix is built, or camera trails pawn by one frame.

- `setMouseCaptured`: gives the mouse to the game or hands it back. ClipCursor + hidden cursor + re-centre every frame, deltas measured from the re-centre rather than WM_MOUSEMOVE, refusing to warp a background window. Absent from the Runtime before this (mouse-look stopped turning when OS cursor hit window edge). See GameConfig::noMouseCapture for the bounded-run gate.

- `captureScreenshotIfDue`: --screenshot only, bounded runs. Requests a capture a few frames before run ends and writes to cfg_.screenshotPath once RHI has it ready. Capture and readback cannot both happen on the same frame.

- `applyProjectPostSettings`: applies RENDER.EXPOSURE / BLOOM / AUTOEXPOSURE / TONEMAP to post_. Separate from applyProjectRenderSettings because none of that function's guards have anything to say about a tone curve.

### Member variables

- `window_`: borrowed from the engine in onInit (HWND for mouse capture). Mirrors SandboxApp's own `window_` member.

- `firstPersonPawn_`: the possessed pawn WHEN THE VIEW IS FIRST-PERSON. Set every call by drivePlayCamera (reset to kInvalidEntity unconditionally first, mirroring SandboxPlay.cpp). Read once a frame in onRender to build DrawWorldOptions::ownerHideRoot, so an entity flagged kMeshRendererHiddenFromOwner under the pawn is skipped in the raster pass—without this a first-person character's own body renders in front of the camera.

- `playMobility_`: what moves during the session, so Voxi keeps it out of the GI bake. Begun in onInit once the level is up (GameMode or not) and before aver_fw_begin_play spawns anything; ended in onShutdown before level unloads.

- `vehicles_`: the level's physics cars (world::VehicleSystem). Built in onInit once play session is PLAYING (after beginPlayIfGameModeDeclared), driven from tickGameplay around physics step, ended in onShutdown. Cars only advance while PLAYING (when physics steps), so projects with no GameMode never build them—cars stay ordinary meshes where placed, in the GI bake.

- `averSrUpscaler_`: AverSR upscaler. NON-OWNING on device side exactly like rhi::IDevice::setUpscaler's description. Device holds a bare pointer, detached (setUpscaler(nullptr)) before this unique_ptr resets—see onShutdown. Mirrors SandboxApp's ordering for identical crash class (--aversr-cycle). Null for the whole run whenever averSrInstaller_ is null or onInit resolves Off.

- `frameInterpolator_`: frame interpolation. Built on first use; detached from device in onShutdown before reset, like averSrUpscaler_.

- `voxiRenderer_`: registered NON-OWNING with addRenderFeature. Device holds bare pointer so it must outlive the device—which is why it is a member and torn down in onShutdown.

- `frameBudget_`: frame-budget controller state (FrameBudget.hpp). One instance for whole run, ticked once a frame, seeded from project_.frameBudgetMs when openProject succeeds. Mirrors SandboxApp.

- `skinnedScene_`: OWNED (unlike voxiRenderer_). SkinnedScene created only if init succeeds; failed init must leave nothing registered. Null is LEGAL: skinned entities draw at rest pose rather than not at all.

- `particleRenderer_`: BY VALUE and registered NON-OWNING, same reasoning as voxiRenderer_.

- `gameUi_`: HUD's render feature. OWNED (UiRenderer::create hands back ownership, null means no HUD).

- `uiFont_`: game UI's font. Invalid (default-constructed) until loadGameUiFont finds one—see that method. Re-lent to aver_ui_set_font every frame, mirroring SandboxApp: loading is non-fatal and can leave this invalid, so handing over same address every frame is the one call site a future reload would need.

- `uiFontTexture_`: BORROWED BY THE ABI, NOT BY gameUi_. This is the atlas loadGameUiFont uploads; uiFont_.atlasTexture points at it as raw u64. Torn down in onShutdown BEFORE device goes; aver_ui_set_font(nullptr) cleared first (ui_abi.h: font whose atlas died draws glyphs from dead descriptor).

- `gameNav_`: loaded once per level after level_.loadStartMap(). Mirrors SandboxApp's nav_. EMPTY is legal common state: most levels have no baked navigation, and AgentSystem::tick treats a grid that fails OcNavData::valid() as "not available yet" rather than error.

- `screenshotDone_`: --screenshot bookkeeping. Latched true once PNG is written, so capture requested near end of run is not re-requested every remaining frame.

- `pcgVolume_`, `pcgSpec_`, `pcgAttached_`, `pcgChecked_`: PCG volume members. Aver.Render.Pcg is linked UNCONDITIONALLY (Runtime/CMakeLists.txt), no AVER_MODULE_PCG switch. This header includes aver/pcg/PcgVolume.hpp outside every guard. Members had no business being scene-conditional. GameApp.cpp is correct unguarded; with AVER_MODULE_SCENE=0 the members vanished while users remained, a bug now fixed.

- `physSteps_`, `lastReportedSteps_`: no implicit physics ground, nothing for GameApp to own besides world (aver_phys_init/aver_phys_shutdown manage it globally). Counted so "did physics step at all" is answerable from log rather than debugger. FIXED STEPS, NOT FRAMES—frame catching up over three steps adds three; frame shorter than fixed step adds nothing. lastReportedSteps_ is the value the last log line carried, so onUpdate can report crossing 600-step boundary rather than landing exactly on one.

### Static relay functions

The five static relays (animNotify, animCurve, synapseTarget, synapseTargetResolver, synapsePerception) used to all be keyed on AVER_MODULE_SCRIPTING because initScripting() was the only thing installing them. But they sit at proximity, not dependency: not one of them touches scripts_. Each reads a SYSTEM (anim, synapse) and hands the answer through a plain function pointer. GameApp.cpp already defines each under the module that system belongs to. Declaration and definition disagreed in every -DAVER_MODULE_SCRIPTING=OFF build: definitions stayed, declarations vanished, causing errors like "'animCurve' is not a member". Each guard below is what that relay's BODY needs (what GameApp.cpp spells for the definition)—one answer stated in two places instead of two answers.

- `animNotify`: raises an animation notify as a graph event. scene::Entity is the reason this is keyed on the scene (in the signature, so declaration cannot parse without it). Body's own dependency on the host is separate, narrower, compiles to no-op with no scripting module.

- `animCurve`: answers the framework's relayed animation-curve query. Reads anim::animSystem() (Aver.Anim.Scene, which stands on the scene).

- `synapseTarget`: answers the framework's relayed Synapse steering-target query.

- `synapseTargetResolver`: PerceptionSystem's TargetResolverFn (who agents should perceive). The framework term is real here (not shared with neighbours): body asks aver_fw_controlled_pawn who the player possesses; neighbours only read a component.

- `synapsePerception`: answers the framework's relayed Synapse perception query.

### scripting module

- `scripts_`: owned here, not by GameContent or GameLevel. Lifetime is the WHOLE APPLICATION's, not the current project's. ScriptHost starts an in-process .NET runtime, which this engine has never supported tearing down and re-initialising within one process (ScriptHost.hpp: "one per process"), so this member is constructed once and lives exactly as long as GameApp—never reset on project change.

- `projectGraphs_`: one discovered project-level .ocgraph per element: absolute path, synthetic entity id (always-negative, see discoverProjectGraphs), and load status.

### Other members

- `gameModeDeclared_`: beginPlayIfGameModeDeclared's answer, latched at boot. True once aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE) found a concrete GameMode. Read once a frame by graph-class tick to tell "nothing here can ever begin play" (gate off, tick forever) apart from "play has not begun yet" (gate on). Defaults to false (the answer that ticks) so a build with framework compiled out behaves exactly as before. A mirror rather than a second call, so the two cannot disagree.

- `statsTimer_`: seconds since the last --stats dump. Periodic logging at GameConfig::statsIntervalSec interval.

- `statsFinalDumped_`: one-shot END-OF-RUN --stats dump latch (bounded runs only). A short --frames run can end before statsIntervalSec elapses once, so both periodic (statsTimer_) and final dump are needed. Mirrors SandboxApp::gpuTimingDone_ for identical crash class (--aversr-cycle): fire exactly once near last frame, never again.

- `landscape_`: level's terrain. Loaded/unloaded with level through level_'s LoadHooks (installLevelHooks) and drawn in onRender before entities, as in editor.

- `water_`: level's water through same hooks as landscape_, plus fluid volumes scripts spawn.

- `streaming_`: PCG scatter streamed around camera. See GameConfig::chunkStreamAutoFrames.

- `chunkStreamFramesLeft_`: frames left before streaming switches on; 0 = off or already done.

## Runtime/include/aver/game/GameContent.hpp

### Class design decisions
- DELIBERATELY UNGUARDED: GameContent needs no PBR or SCENE module
- In SandboxApp map/walk sit inside #if AVER_MODULE_PBR, accident of where code lived, not decision
- Moving code doesn't change which #if it's under; guard re-decided here per symbol
- Unordered_map of u64 to std::string; fnv1a64 lives in Aver.Core
- Not god object: resolvers take void* pointing at GameContent, not app

### Path discovery
- pathsWithExtension built by filtering contentIndex_ with hardened, error_code-based recursive walk
- Added for visual-scripting phase 2's graph discovery (GameApp::discoverProjectGraphs)
- Deliberately generic rather than named pathsToGraphs; walk already exists

### Level-scoped material residency (releaseMaterialsExcept)
- PARTIAL releaseProjectMaterials: destroys currently-loaded materials whose NAME not in keep
- Enables editor keeping every level's materials to hold only OPEN level's
- Motivating case: Sponza and Jungle Ruins together, 17.2 GB resident vs 13.1 GB budget
- keep is surface NAME caller decided must survive; SandboxApp calls from unloadLevel with keep = pinned resident
- Ordinary level change releases everything; doesn't gather NEXT level's surfaces itself
- pbr::MaterialLibrary::destroy called for handles not kept; textures freed by pbr::MaterialSystem on next update()
- FORGETS surfaceMaterials_ tokens naming destroyed handles; prevents stale warnings
- Safe even if next level needs name: materialForSurface + bindSurfaceMaterial rebind fresh

### Mesh default material convention
- Zero value means nothing (not "draw flat"); mesh already declares material
- Non-zero material is override, exactly as before
- Editor's rule held here too; divergence gate in scripts/verify-game.ps1 exists because of this

### Mesh lookup outside scene block
- sceneMeshes_ name is historical; table is content index's mesh id -> handle map
- Keyed by fnv1a64(relative path); nothing about lookup needs entity world
- Two editor callers: foliage loader asks if mesh loaded before accepting; Add > Primitive asks same about built-in
- scene-off and all-off both failed on those lines, found this way


### Forward declaration of OcMeshData
- Forward-declared instead of included to avoid widespread header inclusion. Only buildMeshParts' PRIVATE signature in GameContent.cpp needs the full type, and this header is included widely enough that the leaf forward declaration is justified.

### Material loading design decision
- loadProjectMaterials() was deliberately removed. The runtime carried a copy of the editor's version but nothing ever called it. Materials now load lazily per-surface through materialForSurface() and Binaries-first, eliminating unnecessary eager load time and memory cost.

### posedPartsFor implementation detail
- Skinned entities draw their POSED copy (a different handle from the base), so without this splitting they drew as ONE mesh under the entity's own material. Example: a character whose hair cards are their own material slot never drew them with the hair material. Posed parts are index-for-index with partsFor(id).

### collisionMeshFor caching behavior
- Exclusion rules: does not include LOD-0 triangles whose material slot is alpha-masked or translucent (leaves, glass - see collisionSlotCollides). On missing or unresolved slots, conservatively keeps triangles.
- Simplification: meshoptimizer simplifies to roughly 2 cm world error, subject to a triangle ceiling, UNLESS Trifactor's own coarsest-LOD-within-2cm pick (old behaviour) came out with fewer triangles - that is kept instead. See collisionMeshFor's own code for exact rule.
- Disk cache: under <project>/Saved/DerivedDataCache/Collision. Second load of same mesh (same path, size, last-write time) skips filter/simplify work entirely.
- nullptr results: for meshes with no .ocmesh (built-ins, unknown ids, failed loads, or ALL slots alpha-masked/translucent) are CACHED to avoid retry. Caller gets nullptr and keeps colliding as fitted box (world::addStaticBoxBody).

### loadProjectParticleEffects scope
- Recursive over whole content root, matching loadProjectMeshes (not Materials-folder convention .ocmat follows). Decided 3 gave .ocparticle no folder-based loading rule.
- Uses same id space (fnv1a64 of relative path) as contentIndex_, so CParticleEmitter::effect resolves identically to CMeshRenderer::mesh or CAnimator::clip.
- particles::particleEffects() is the same process-global table that SandboxApp.cpp's loadProjectParticleEffects() fills and --particle-test's hardcoded content calls set() on directly. No GameContent-owned cache to keep in sync.

### meshPartBaseIndices_ storage
- Stores per-part indices UNREMAPPED in the base mesh's vertex numbering, which skin targets share verbatim. Split parts are compacted and renumbered, so their indices cannot be reused over a posed buffer. This is the one piece of information the split used to throw away.

### collisionSlotCollides bool
- True when a material-slot NAME should collide. False only for slots that resolve to an authored .ocmat with alphaMode of Mask or Blend (leaves, glass - authors cut these out/translucent precisely so players are not blocked). True for empty names, ones with no PBR module to resolve them, or ones that resolve to nothing - "cannot resolve" keeps triangles, the conservative default collisionMeshFor already uses.

### resolveMaterialGraph porting note
- Ported from SandboxApp::resolveMaterialGraph (sandbox/src/SandboxAssets.cpp): same content-relative resolution, same cache-by-compiled-path through pbr::materialGraphs().idOf(), same compile-on-miss through fmt::loadOcgraph() + pbr::materialGraphs().add(), same 0 (stock shading) fallback on any failure.

## Runtime/src/GameApp.cpp

### Part 0 (208 -> 94 lines, 55% reduction)

Removed extensive technical context from comments:
- GPU timing tree formatter reference (moved to docs)
- Detailed STB_IMAGE_WRITE collision explanation (simplified to symbol safety note)
- Multi-line explanations of #include purposes (condensed to one-liners naming the function/purpose)
- Long essays on configuration read timing and manifest lookup order
- Detailed history comments like "A 100m floor used to be added here"
- Verbose explanations of edge cases ("bad/foreign argument is never fatal convention")
- Cross-references and "see X's comment for why" chains
- Measurement data and floating-point precision history

Kept:
- Critical ordering requirements ("load order: index -> meshes -> levels")
- What non-obvious blocks do ("Only tick when PLAYING, not PAUSED")
- Invariants and design decisions ("Bare .ocproject or .ocworld path is recognized")

### Part 1 (268 -> 212 lines, 21% reduction)

Removed:
- Multi-paragraph essays on particle test parity ("rebuilt over this executable's ECS entity + CMeshRenderer path rather than the editor's objects_ list")
- Detailed technical rationales for ordering ("process-global singletons; without this the system never resolves...")
- History of features ("used to be the one window key nothing read")
- Long cross-references to other code locations
- Verbose "why" explanations for separate load paths

Kept:
- Load order requirements ("Load order is critical: index -> meshes -> levels")
- What a non-obvious condition prevents ("Synthetic id must never collide with live entity")
- Design decisions constrained by outside factors ("Window-creation-time decision. No command-line override exists")

### Key Patterns Stripped

1. **Before/After comments** ("before this fix", "used to be", "Until now") - removed entirely
2. **Experiment/Measurement justifications** ("Measured GPU/CPU agreement is within 1.19e-07") - removed numbers and context
3. **Edge case essays** (5-10 line explanations of why something isn't fatal or why a guard exists) - condensed to 1-2 lines
4. **Cross-file references** ("see SandboxApp.cpp's copy of this comment") - removed; kept only "matches editor" if ordering matters
5. **Restated code** ("the seat sits beside the .ocfont") - removed when code is already clear
6. **Code review context** (why a previous approach was wrong) - removed

### Notable Findings

- `config()` function reads manifest twice (WINDOW.* before init, then full manifest in openProject) - justified by window needing creation-time data; cheapness of small text file read
- Particle test must match Sandbox.exe exactly for parity checking (not just similar)
- GI volume extent in 128^3 voxel grid means cell size scales proportionally with scene bounds
- Project-level graph ids are synthetic negative values (-1000 downward) to never collide with entity handles


- `attachParticles()`: particles library must be wired before effects can spawn.

- `spawnParticleTestContent()`: dust cloud and embers test must match SandboxApp.cpp --particle-test parity exactly (cubePos 600,0,50; dust emission 150/sec, 3-5s lifetime, 400 max; embers sphere 10cm radius, 80/sec, 1-1.6s, 200 max; camera at 0,0,50 looking +X).

- `particleGiPrepare()` / `particleGiBind()`: particles DECIDED 4 — GI seam boundary between particles (modules/particles) and Voxi, since particles module does not include voxi headers. Two halves mirror SandboxApp.cpp's pair.

- `emberFx.receivesGI = false`: ember is its own light source (particles DECIDED 4).

- `installLevelHooks()`: terrain changed hook restarts streaming so scatter regenerates against new surface.

- `openProject()`: load order critical: index → meshes → levels. Without this order, content lookups fail.

- `openProject()`: manifest GI volume (RENDER.GIVOLUME) wins over auto-fit, mirroring editor precedence. Without the guard, fitGiVolumeToLevel() runs unconditionally and discards hand-authored GI volume on every launch (see docs/RUNTIME-DEDUP.md C4).

- `openProject()`: RENDER.EXPOSURE/BLOOM/AUTOEXPOSURE/TONEMAP are per-device post state (like PHYSICS/AUDIO), applied separately from render settings.

- `initScripting()`: scripts dir formula: `<out>\Binaries\Scripts` (see ProjectScaffold.cpp, stage-game.ps1 LAYOUT comment).

- `initScripting()`: graph-as-class files (carrying CLASS record) are claimed by declareGraphClasses (in initScripting) and skipped here. Loading again would tick the file twice (harmlessly but noisily). See ocgraphDeclaresClass text scan.

- `initScripting()`: animation notifies must be installed after scripting host init but before any gameplay tick, since sink needs ScriptHost. Survives content reload: AnimSystem::clear() drops clips/playhead but not the sink. Installed unconditionally: C++ caller can ask for curve with no scripting host.

- `animCurve()`: declaration is guarded on SCENE, but definition stayed unguarded because the member was declared inside the guard while definition remained outside. A scripting-off build used to lose the declared member but keep this definition.

- `discoverProjectGraphs()`: project-level graphs use synthetic negative entity ids (-1000 decreasing) to avoid colliding with live scene entities and ensure accidental position-write side effects become safe no-ops (negative ids unresolvable; fieldAddr returns null).

- `discoverProjectGraphs()`: not every .ocgraph is a gameplay graph; material graphs handed to graphLoad would fail with "unknown node type" instead of being recognized as offered to wrong compiler (see ocgraphIsForeign).

- `tickProjectGraphs()`: ungated on aver_fw_play_state() because Aver.Graph has no reference to Aver.Framework. OnTick fires every frame from first successful load, not just during PLAYING. Project-level graphs not class instances — nothing places them in level, so ticking can't mutate contents.

- `beginPlayIfGameModeDeclared()`: gameModeDeclared_ latched to distinguish "no session begun yet" from "nothing can ever begin one". Set before early return so failure still counts as "HAS a GameMode" (failed, not undeclared).

- `beginPlayIfGameModeDeclared()`: aver_fw_begin_play already treats "no GameInstance class" as "skip that spawn" — GameMode with no GameInstance simply had nothing worth putting there.

- `beginPlayIfGameModeDeclared()`: pawn doesn't exist until GameMode spawned and possessed it. No fallback relocation: project's pawn spawns where GameMode chooses.

- `fitGiVolumeToLevel()`: extent is the point (Voxi voxelises 128³ grid). Voxel size ~9cm at 12m extent, ~15.6m at 2000m extent (entire tree in one cell → indirect light uniform mush). Editor slider stops at 100000cm for this reason. Fitting level matches shipped image to what author saw.

- `applyProjectRenderSettings()`: two-phase apply: tiers commit first, so derivation for unstated knobs (voxelResolution, giCones, etc.) runs against NEW tier before manifest's own values land on top. Single merged call reintroduces regression (see ProjectRenderApply.hpp).

- `applyProjectPostSettings()`: RENDER.EXPOSURE silently does nothing with auto-exposure on. Composite pass multiplies by adapted value and never reads PostSettings::exposure (post.hlsl:216-220). Easy to hit by accident since autoExposure defaults to true (RHI.hpp:158). Not "fixed" by forcing adaptation off: saying nothing about AUTOEXPOSURE ≠ saying zero.

- `viewAspect()`: reads from swapchain not viewport rect, because game's scene IS the backbuffer (unlike editor's dockspace panel that is latched earlier). Using same formula would need ImGui-only members and last frame's size.


### Camera wobble (--cam-wobble DEG PERIOD)
- Sine formula returns to zero at every whole period multiple.
- Driven off frames_ (incremented at top of onUpdate, before pushFrame) not clock, so path is identical every run.
- Applied as temporary offset to yaw_ for one camForward() call only, then restored.
- yaw_ is free camera's real orientation; drivePlayCamera overwrites it every frame, so no safe accumulation point.

### Aspect ratio logging (camera)
- Log once per run: oracle for correct calculation. Games asked for 800x600 must report 1.333, 1600x900 must report 1.778.
- Editor's dockspace formula cannot produce these values (verified in code).

### PCG sky field naming convention
- PCGVOLUME "Sky" drives cloud layer. Selected BY NAME, not "the first field".
- A level may declare other fields (cave mask, moisture); sampling wrong one looks like rendering bug, not lookup error.
- Coverage floor is INVERTED into cloud cover: high density floor = less material survives = less cloud.

### G-buffer and frame interpolation interaction
- N4: without G-buffer switch read from denoiserGBufferWanted, DENOISER 1 ran with no G-buffer allocated.
- RENDER.MSAA changes mid-session never reached device.
- Frame interpolation reads motion and depth from G-buffer, so wanting it turns that on too.
- Device pauses it (with one-shot log) when 1x anti-aliasing is missing.
- Without vsync, presents on display's refresh clock.

### Input focus handling
- No portable foreground query exists in this tree; Win32 fallback is GetForegroundWindow().
- Bounded (--frames N) runs: window opens UNACTIVATED so measurement never steals focus.
- GetForegroundWindow() would read "not foreground" every frame without special casing for bounded runs.

### Input eating
- inputPolicy.eaten stays all-false: only editor's drawer uses chords.

### HUD draw index
- Editor's hudPreviewIndex_ (HUD-picker selection) has no shipped-game equivalent.
- Index 0 used instead: first-declared [AverHud] class. Project with multiple HUDs gets whichever loaded first, not necessarily Default.

### Graph-as-class with no GameMode
- Graph class declares like C# GameMode and gets GAME_MODE bit via sealClass.
- A graph-only project DOES reach aver_fw_begin_play via beginPlayIfGameModeDeclared (corrects prior incorrect comment).
- Measured ungated cost in editor on PTTest over 1000 idle frames: graph's `elapsed` VAR climbed from 3.6e-05 to 12.31s over 4003 tick lines.
- Gated on PLAYING or !gameModeDeclared_ (latched by beginPlayIfGameModeDeclared, not re-queried here).

### Physics step counter
- Advances by real step count, not frame count: frame catching up over two steps can jump from 599 to 601 and skip exact-multiple test.
- lastReportedSteps_ == 0 keeps first-step-ever-announced rule.

### Frame budget behaviour
- FrameBudget.hpp's shared controller, seeded from project_.frameBudgetMs in openProject.
- Called right before pushFrame so voxiRenderer_ holds this frame's rung.
- Never on bounded (--frames N) run: mirrors frameBudgetTick's own maxFrames_ early-out.
- Throttled frame becomes new authored baseline; quality can only ratchet down without care.
- Controller must never write back into voxi::Renderer::get() itself (copies settings in/out locally).

### pushFrame timing: must be last in onUpdate
- Engine::frameStep: onUpdate -> beginFrame -> onRender -> endFrame.
- beginFrame takes ONE snapshot of PerFrameCB into GPU-visible buffer. Only sole write is D3D12Device::beginFrame -> frameCBPtr_.
- setCamera/setLight/setSkyAtmosphere only touch CPU-side shadow copy.
- If called from onRender (after beginFrame): rasterises frame N with frame N-1 constants.
- Worse than uniform lag: viewProj_ also what drawWorld culls against; culling uses THIS frame's matrix while GPU draws previous, one-frame divergence.
- Editor never had this bug: SandboxApp sets camera in onUpdate (SandboxApp.cpp:1115).

### Input roll timing in onRender
- Deliberately last thing onUpdate does (within onRender now).
- Engine::run pumps window at top of loop (pumpEvents -> frameStep{...}), so key pressed this frame is in InputState by onUpdate.
- Calling at onUpdate start would throw away edges that just arrived; game would ignore taps while handling held keys.
- Clearing here after last reader leaves accumulator empty for next pumpEvents to fill.

### Font teardown ordering
- Font must come out first: ui_abi.h comment on aver_ui_set_font explicit.
- A font outliving its atlas textures samples dead descriptor (uiFont_.atlasTexture is raw handle into texture destroyed shortly after).
- Unconditional and safe: aver_ui_set_font touches only Aver.UI module's held pointer, no RHI object.

### UI font atlas destruction
- waitIdle required: destroying a texture a still-in-flight command list references is GPU use-after-free.
- Debug layer catches immediately; release build does not.
- Atlas was bound for sampling as recently as this frame's HUD draw (SandboxApp.cpp's icon-texture teardown mirrors this pattern).

### Render feature registration order
- Skinning registered FIRST in onInit, so comes out LAST in onShutdown.
- Particles registered LAST, so removed FIRST.
- voxiRenderer_ is member by value, outlives device if unregistered first.
- Device holds bare pointers to all render features; wrong order = feature torn down while another references it.

### AverSR shipped-game setup
- Resolved and installed right after openProject (applyProjectRenderSettings committed project's GI/RT tiers into voxi::Renderer::get().settings()).
- Lets autoAverSrLevel's Custom -> max(GI tier, RT tier) fallback read PROJECT tiers, not compiled-in default.
- CLI (--aversr) beats RENDER.AVERSR beats Auto, same resolveAverSrLevel chain as editor.
- userLevel pinned to -1: packaged game has no Display page to prefer from.
- averSrInstaller_ null when SR module absent (sole TU allowed to name sr::anything is Runtime/host/RuntimeMain.cpp under AVER_MODULE_SR).
- Resolved level with no installer stays native 1.0; game must still build/run with SR module absent.

### Scripting bootstrap cost on graph-free project
- Measured cost unconditionally: 40-60ms extra startup (938-965ms against 890-908ms baseline).
- Plus new failure surface (missing bridge dir / nethost.dll).
- pathsWithExtension only reads content index, nearly free to ask first.
- Legacy Scripts assembly (C# gameplay) still forces bootstrap even on graph-free project.

### Backslash escaping in path test (C4129)
- "\\" not "\": MSVC drops unescaped backslash (warning C4129).
- Would test "...BinariesScripts" (path with no separator) that never exists.
- Project with gameplay compiled C# with no .ocgraph beside it would silently skip bootstrap.
- Line 686 (earlier in file) builds same path correctly; only this test was wrong.

## Runtime/src/GameContent.cpp

- **Include guards for AssetType/assetTypeFromPath**: Asset ID always included (not guarded) because it is used by both loadProjectMeshes (scene-guarded) and loadProjectParticleEffects (particles-guarded). Previous PBR-guarded loadProjectMaterials was removed as dead code.

- **appendBoxYaw**: Hand-written primitive generator for axis-aligned boxes with independent per-axis half-extents, yawed around Z. Copied (not shared) from SandboxApp.cpp's own appendBoxYaw because it is file-owned by another module. Face table and winding match appendBox exactly. yawDeg=0 makes this identical to appendBox; hx=hy=hz with yawDeg=0 reproduces appendBox exactly (true by construction).

- **appendCylinderZ**: Capped cylinder standing along +Z (for drone motor pods and rotor discs). Flat-shaded per face like appendBox, not smooth-shaded. At the segment counts a rotor pod uses (8-10), flat shading is visually indistinguishable from smooth. Second copy of SandboxApp.cpp's own appendCylinderZ for file-ownership reasons.

- **appendDrone**: Placeholder quadcopter (body, four arms with motor pods/rotors, landing skids/struts). 420 triangles. Normalised like unit cube/sphere: nothing past 1.0 from origin. Unlike isotropic cube/sphere, it is not the same size along every axis. Maximum reach (1.0) is at four rotor-tip diagonals (kArmROuter + kDiscRadius = 0.80 + 0.20). Per-axis reach: X/Y ~0.7657, Z up ~0.16, down ~0.22. Matched vertex-for-vertex (winding, normal style, units) with SandboxApp.cpp's appendDrone.

- **registerBuiltins / built-in mesh bounds**: Unit cube half-extent stays 1 (frozen constraint). A PLACEG scale is a half-extent in centimetres applied to this mesh; changing it silently resizes every placed box in every level ever authored. Bounds are recorded for built-ins, which SandboxApp does not do. A CMeshRenderer with degenerate (unfilled) bounds triggers frustum-cull exemption; every entity using a built-in primitive would be exempt from culling until bounds were recorded. Measured: a five-placement level reported "5 drawn, 0 culled" from every camera angle until bounds were recorded.

- **Named surfaces (M_Floor, M_Wall, M_Concrete, etc.)**: Table must stay in step with SandboxApp.cpp's look()/surfaceLooks_ block. Parity bug fixed: editor's table named ten surfaces, this one named only seven (M_Foliage, M_Bark, M_Rock missing). A level authored in editor using one of those three, with no backing .ocmat, rendered intended colour in editor but fell through to flat 0.80/0.80/0.85 gray fallback in packaged game. Whoever adds an eleventh name to editor's table and forgets this one reproduces exactly that bug, silently, again.

- **M_Concrete**: Generic surface in engine's default palette (common architectural surface, not project-specific). Added after a scene naming it fell through to flat {0.80,0.80,0.85} fallback and rendered as undifferentiated near-white (reported as lighting bug when it was content resolving to nothing in both render paths).

- **M_Glass**: Opaque fallback only. SurfaceLook has no alphaMode field; drawWorld only sets blended=true for authored .ocmat with alphaMode=Blend. Built-in look can never trigger blended path. A level using "M_Glass" with no backing .ocmat renders opaque near-white, not glass (improvement over flat gray fallback, but still opaque). Actual translucency requires authored M_Glass.ocmat with BLEND set.

- **loadProjectMeshes depth proxies**: Coarse LOD for shadow/GI/voxelise passes (depth-only passes that resolve silhouette, not surface). 20cm target error (about one shadow-map texel; below this the silhouette cannot change). Same rule, threshold, and reasoning as editor's ladder (see SandboxApp.cpp's kShadowErrorCm block): chosen on Trifactor's measured world error in centimetres, not on triangle ratio. Game differs from editor: editor uploads whole ladder and keys map on every level's handle (for runtime LOD selection with --lod-select). Game has no runtime LOD selection at all; GameRender.cpp's draw walk only submits meshFor(mr->mesh) itself (LOD 0) or one of ITS handles (for split meshes). Uploading unused LOD levels would be VRAM nothing can look up. Pick computed from OcMeshData (no upload needed), so exactly one extra level is uploaded. When game learns to select, this becomes editor's loop again. Split mesh parts have no proxy (map is keyed on whole mesh's handle).

- **buildMeshParts**: Splits mesh with multiple materials into one MeshHandle per slot. Ported from SandboxApp::buildMeshParts; see that function's comment for why splitting at load time (not drawing per-submesh ranges) is tractable. Ray path's BLAS carries one materialIndex per instance; range draw would still shade flat in renderer on screen. Each part is index buffer over whole mesh's vertex buffer (createMeshSharingVertices), so split mesh holds vertices once. Parts take whole mesh's bounds.

- **posedPartsFor**: Skin target shares source's index buffer verbatim (createSkinTargetMesh). After editor mesh reload they differ; equal index buffers and vertex counts prove posed mesh was cut from baseMesh. Without this proof, entity keeps single whole-mesh draw (safer than drawing indices against wrong vertices). Refusal is cached per posed handle (not retried per frame). Half-split character silently loses geometry of every part that failed; whole-mesh fallback draws all (all-or-nothing approach).

- **compactTriangles**: Shared by every collisionMeshFor candidate. Compacts indices down to only vertices they reference, dropping degenerate triangles (no area, nothing to collide with).

- **weldByPosition**: Welds compacted positions/indices by exact bit-pattern position. meshopt_simplify treats topological border (edge with one triangle) as border to preserve; every UV seam or hard-normal split leaves duplicate vertices read as false borders. Welding first lets simplifier see real, mostly-closed topology instead of surface of tiny false seams.

- **simplifyCollisionMesh**: Simplifies welded positions/indices toward ~2 cm world error. Never past triangle ceiling 65536 (hard limit for BVH and runtime cost). ERRORABSOLUTE option: error in same units as positions (centimetres; "2 cm" needs no extent-dependent conversion). LOCKBORDER option: preserves genuine open edges and ones cut by material-slot filtering. If 2 cm error insufficient to reach ceiling, error is allowed to grow.

- **Collision cache**: Derived data (not authored). Whole directory <project>/Saved/DerivedDataCache/Collision can be deleted for cost of one rebuild per mesh. Atomic write: record goes to "<cachePath>.tmp" first; successful rename publishes it. Reader never sees partially-written file. Writer crash/kill leaves old entry (or none) in place, not corrupt one. kCollisionCacheVersion bumped when filter rule, simplification target, or layout changes. Key covers mesh path, size, mtime, version; version prevents rule change serving previous rule's output forever for unchanged mesh file.


- `collisionMeshFor`: The function filters collision geometry in five steps: (1) drops alpha-masked/translucent material slots using LOD0 submesh ranges; (2) compacts the filtered triangle set to only referenced vertices; (3) welds by position then simplifies with meshoptimizer toward ~2cm; (4) tries Trifactor's coarsest LOD within 2cm (but only if nothing was filtered, since coarser LODs have no submesh table to filter through); (5) picks winner: fewer triangles wins, ties favor Trifactor (no further cost).

- `collisionMeshFor` caching: Failed lookups insert null entries so that subsequent asks for the same id are O(1) hash lookups instead of re-reading files or re-stating paths. This applies to both genuinely missing files and built-in ids that never indexed.

- `resolveMaterialTexture`: Colour space is slot-determined, never filename-inferred. Normal maps decoded as sRGB produce subtle shading bugs. Layer1 slots are material graph inputs: Layer1BaseColor must be sRGB (Colour usage), Layer1Normal must be NormalMap usage. Previously Layer1 slots fell through to Data, causing Layer1BaseColor uploads as LINEAR and Layer1Normal to lose normal-map-aware mip generation.

- `materialForSurface`: Search order: built .ocmat in Binaries (from avermatc) wins over hand-authored in Content. Built version is what the level ids reference. Parse failures break (corrupt built material not silently replaced by stale hand-authored).

- `loadProjectParticleEffects`: Effect id is `fnv1a64(relative path)`, matches `CParticleEmitter::effect`. The table is process-global; reload clears it to prevent stale entries across project reloads.
