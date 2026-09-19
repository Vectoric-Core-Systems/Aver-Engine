// GameApp: the aver::Application a shipped game runs, as opposed to the editor.
#pragma once
#include "aver/runtime/Application.hpp"
#include "aver/platform/InputState.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/pcg/PcgVolume.hpp"

#if AVER_MODULE_VOXI
#  include "aver/voxi/VoxiRenderer.hpp"
#  include "aver/voxi/FrameBudget.hpp"
#endif
#if AVER_MODULE_SCENE
#  include "aver/render/SkinnedScene.hpp"
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
#  include "aver/particles/ParticleRenderer.hpp"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/formats/OcNav.hpp"
#endif
#if AVER_MODULE_SCRIPTING
#  include "aver/scripting/ScriptHost.hpp"
#endif
#if AVER_WITH_UI_ABI
#  include "aver/render/ui/UiRenderer.hpp"
#endif
#include "aver/game/GameContent.hpp"
#include "aver/game/GameLevel.hpp"
#if AVER_MODULE_LANDSCAPE
#  include "aver/game/GameLandscape.hpp"
#endif
#if AVER_MODULE_FLUIDS
#  include "aver/game/GameWater.hpp"
#endif
#include "aver/game/GameStreaming.hpp"
#include "aver/game/GameRender.hpp"
#include "aver/game/GameInput.hpp"
#include "aver/game/MouseCapture.hpp"

#include <memory>
#include <string>

namespace aver {
// Forward-declared rather than included: GameApp only ever stores a bare, borrowed pointer (set
// once in onInit from Engine::window(), mirroring SandboxApp.hpp's own `window_` member) for the
// HWND that mouse capture needs -- see GameApp::setMouseCaptured's own comment.
class Window;
}

namespace aver::game {

// Everything GameApp needs that comes off the command line or out of game.json.
struct GameConfig {
    // NAMED CONSTANTS, so config() can tell "the caller asked for 1280" apart from "nobody said".
    // Without that distinction a project's WINDOW.SIZE could never win, because the field is never
    // empty -- it always holds something.
    // "Aver Engine Runtime", the executable's own name (Runtime/host/AverEngineRuntime.rc). A project
    // that states a WINDOW.TITLE or a NAME replaces it in config(), so a shipped game shows the game.
    static constexpr const char* kDefaultTitle = "Aver Engine Runtime";
    static constexpr u32 kDefaultWidth  = 1280;
    static constexpr u32 kDefaultHeight = 720;

    std::string title = kDefaultTitle;
    u32 width = kDefaultWidth;
    u32 height = kDefaultHeight;
    u64 maxFrames = 0;      // 0 = run until the window closes
    bool headless = false;
    bool useWarp = false;
    bool debugLayer = false;
    std::string backend;    // empty = the compiled-in default order
    // The .ocproject to open. A packaged game passes Game.ocproject; empty means no world.
    std::string projectPath;
    // A bare .ocworld/.ocmap argument: the level to open instead of the project's STARTMAP, falling
    // back to STARTMAP when the file does not exist. Same as the editor's bare level argument
    // (openMapPath_), including finding the owning .ocproject when none was named. Empty = STARTMAP.
    std::string levelPath;
    // Logs the held-key set on every change. Proves the ImGui-free input path without a debugger.
    bool inputEcho = false;
    // Logs every path the engine opens, so verify-game.ps1 can assert none is outside the package.
    bool traceOpens = false;
    // --scene-census: print one line describing what the loaded level put in the world, in the
    // format aver::world::formatSceneCensus defines. Sandbox.exe prints the identical line for the
    // identical project, and scripts/verify-game.ps1 compares the two. See SceneCensus.hpp for why
    // this is a census and not a pixel diff.
    bool sceneCensus = false;
    // --stats [seconds]: periodically log the per-pass GPU breakdown a shipped game has ALWAYS been
    // paying to collect and never had any way to look at. D3D12Device::initGpuTiming runs
    // unconditionally, not behind a build flag or a CLI switch, so every AverEngineRuntime.exe ever shipped
    // has been timestamping every pass and throwing the numbers away -- the editor was the only host
    // with a reader (its console's `frametime`), and a packaged game cannot include the editor.
    //
    // A LOG DUMP RATHER THAN AN OVERLAY, at least first: a profiler for a shipped build is something
    // you capture from a session and read afterwards, often from a machine you do not have. An
    // overlay would also need the game UI's text path and would change what a --screenshot capture
    // contains, which would break verify-game's census comparison for a diagnostic feature.
    //
    // 0 means off. The interval is in SECONDS because the report is an average over frames since
    // boot and only moves slowly (see GpuTimingReport::framesAccumulated); logging it per frame
    // would be a flood of nearly identical trees.
    f32 statsIntervalSec = 0.0f;
    // Fills a density volume on the GPU and compares every voxel against the CPU mirror, then
    // exits. The only way to check the HLSL against its reference: HLSL compiles at RUNTIME, so a
    // green build says nothing about whether the shader agrees with anything.
    bool pcgVolumeTest = false;
    // --chunk-stream [N] / --no-chunk-stream: PCG chunk streaming switches on N frames in, 0 = off.
    // ON BY DEFAULT (5 frames), as in the editor (SandboxApp::chunkStreamAutoFrames_): without it
    // nothing runs the scatter generator and a level shows only its hand-placed content.
    int chunkStreamAutoFrames = 5;
    // particles DECIDED 4's A/B toggle -- see sandbox/src/SandboxApp.cpp's setNoParticleGi for the
    // full contract; this is the same flag, just read here instead of set through a member function
    // (GameApp has no other CLI setters -- see parseArgs, which fills this struct directly).
    bool noParticleGi = false;
    // Slice 5's own proof content: the SAME dust cloud + ember burst sandbox/src/SandboxApp.cpp's
    // --particle-test spawns (same shapes, same effect data, same relative placement), rebuilt over
    // this executable's own ECS entity + CMeshRenderer path since GameApp has no editor-only object
    // list to borrow. Exists so "particles render in both executables" has a screenshot from each
    // that shows the same effect rather than two different scenes that merely both have particles.
    bool particleTest = false;
    // AverGame.exe had no screenshot mechanism at all before this -- sandbox/src/SandboxApp.cpp's own
    // --screenshot (its shot_ member) is editor-only, built on top of viewport/probe bookkeeping a
    // game does not have. This is the minimal equivalent: e.device()->requestCapture/getFrameImage
    // are plain RHI calls, not an editor feature, so a --frames run can write a PNG here the same way.
    // Empty = no screenshot requested (the default, and the only behaviour before this field existed).
    std::string screenshotPath;
    // --no-vsync (M7): the runtime's own measurement parity with SandboxApp.cpp's identical flag. False
    // (DEFAULT) leaves vsync exactly as the backend opened it -- a shipped game syncs to the display
    // the way a player expects. True disables it once, the instant a device exists, the same
    // vsyncCanDisable()/setVSync(false) pair the editor uses; a display path that cannot tear (see
    // rhi::IDevice::vsyncCanDisable's own comment) logs a warning and is left synced rather than
    // silently ignored.
    bool vsyncOff = false;
    // --no-mouse-capture: the editor's own OS mouse capture (ClipCursor + hidden cursor + re-centre
    // every frame -- see GameApp::setMouseCaptured) is ported into the shipped game and engages
    // automatically once a session is playing and the window is foreground. It NEVER engages on a
    // bounded (--frames N) run regardless of this flag, mirroring SandboxApp.cpp's own "interactive"
    // gate (maxFrames_ == 0): an automated capture/gate run has no business hijacking the machine's
    // cursor. This flag additionally disables it on an open-ended (--frames 0, the default) run, for
    // anyone driving the window with their own mouse automation and needing the OS cursor left alone.
    bool noMouseCapture = false;
    // --cam-wobble DEG PERIOD (M7): the SAME measurement-only yaw swing SandboxApp.cpp's own
    // --cam-wobble drives -- see its setCamWobble's comment for why a sine that returns to zero,
    // driven off the frame counter and never the clock. 0 (DEFAULT, either field) is no motion, so
    // every existing --frames capture through this executable is bit-identical without it.
    f32 camWobbleDeg = 0.0f;      // yaw amplitude in degrees
    u32 camWobblePeriod = 0;      // period in FRAMES; sin() is 0 at every whole multiple

    // --aversr off|quality|balanced|performance|auto (3.3 C, contract C2-12): the CLI's own AverSR
    // level, parsed LOCALLY (GameApp.cpp's parseArgs) into the quality ladder's own numbering
    // (QualityLadder.hpp's kAverSrOff=0/kAverSrQuality=1/kAverSrBalanced=2/kAverSrPerformance=3,
    // section 3.2) rather than through aver::sr::parseQuality -- this struct, and GameApp behind it,
    // must never include aver/sr/* (the module boundary Scalability.hpp's own header comment states:
    // render.voxi and runtime.game must never depend on render.sr), so this stays a raw int the same
    // way the engine's other pre-enum CLI overrides do. -1 (DEFAULT) means "the CLI said nothing" and
    // is read identically to an explicit "auto": GameApp::onInit's resolveAverSrLevel call then falls
    // through to the open project's RENDER.AVERSR and, failing that, the quality ladder's per-rung
    // default (U2). UNMEASURED: this host's own frame cost at a reduced internal resolution has not
    // been run.
    int averSrArg = -1;   // -1 = unstated/auto, else the ladder's own numbering 0..3
};

// Parses the arguments a game executable accepts. Unknown arguments are ignored rather than fatal:
// a launcher or a store client can append its own, and refusing to start because of one would be a
// bad trade for a shipped product.
GameConfig parseArgs(int argc, char** argv);

// The game-side Application.
//
// WHY THIS EXISTS SEPARATELY FROM SandboxApp. Sandbox.exe IS the editor -- it is the only
// aver::Application in the tree, and roughly two thousand lines of what a game needs (content
// indexing, level load, the world draw walk, the tick-group ordering, the play camera) live inside
// it, interleaved with dockspaces and inspector panels. A game cannot link that, and this class is
// where the game half moves to. Right now it is the window and the frame loop; the lifts land in
// later slices, one subsystem per commit, so that a black screen never has two candidate causes.
class GameApp final : public Application {
public:
    explicit GameApp(GameConfig cfg);

    // AverSR (3.3 C, contract C2-12): the composition root (Runtime/host/RuntimeMain.cpp, the one
    // translation unit in this executable allowed to name sr::anything -- see its own header comment)
    // hands this a function that builds a concrete aver::sr::SpatialUpscaler against a device and
    // reports the render scale for a resolved level. GameApp itself never includes aver/sr/* and
    // never names sr::Quality, matching the module boundary render.voxi and runtime.game must both
    // respect (Scalability.hpp's own header comment: "render.voxi must never include render.sr").
    // Called once by createApplication, before Engine::run ever calls onInit. LEFT NULL is the legal
    // default for an AverEngineRuntime.exe built with the SR module absent: onInit's apply step (below) still
    // resolves a level through resolveAverSrLevel, it simply has nothing to install it with, and the
    // game renders at native resolution exactly as it always did -- the SR module being absent from a
    // build must never be a build failure or a run failure.
    using AverSrInstaller = bool (*)(rhi::IDevice& dev, u32 level, std::unique_ptr<rhi::IUpscaler>& out,
                                      f32& renderScale);
    void setAverSrInstaller(AverSrInstaller fn) { averSrInstaller_ = fn; }

    BootConfig config() const override;
    void onInit(Engine&) override;
    void onUpdate(Engine&, const Timestep&) override;
    void onRender(Engine&) override;
    void onShutdown(Engine&) override;

    // The accumulated keyboard and mouse state for this frame.
    const InputState& input() const { return input_; }

    // The open project. Invalid until openProject succeeds.
    const fmt::ProjectDesc& project() const { return project_; }

    // The project's asset index.
    const GameContent& content() const { return content_; }

    // The loaded level.
    const GameLevel& level() const { return level_; }

    // The camera's forward axis, built from yaw and pitch.
    Vec3 camForward() const;

    // THE ASPECT FIX. The editor divides its DOCKSPACE CENTRAL NODE (vpW_/vpH_, latched by buildUI
    // the previous frame) because its 3D view is one panel among many. A game has no dockspace and
    // no panels: its scene is the whole backbuffer, so the aspect is the swapchain's. This is the
    // one thing in the entire lift that cannot be copied verbatim -- copying it would need
    // members that only exist because ImGui does.
    f32 viewAspect(const Engine&) const;

private:
    // Backing store for the window title config() may take from the project. BootConfig holds a
    // `const char*`, so the string it points at has to outlive the call -- and config() is const,
    // which is why this is mutable rather than a local.
    mutable std::string windowTitleOwned_;

    // Loads cfg_.projectPath. Logs and leaves project_ invalid on failure rather than aborting: a
    // game with no world is a diagnosable state, and a process that dies before its first frame
    // tells the player nothing.
    void openProject(Engine&);
    // Gives level_ the work the editor does around a level load (water and terrain), before the first load.
    void installLevelHooks(Engine&);
    // Switches chunk streaming on for the loaded level, with the terrain as its height source when
    // there is one -- SandboxApp::setChunkStreamingEnabled(true).
    void enableChunkStreaming();

    // Initialises and registers the Voxi renderer. Also what makes pbr::MaterialSystem exist:
    // it is a MEMBER of VoxiRenderer and is initialised only inside VoxiRenderer::init, so a game
    // with no Voxi feature has no material system and draws every surface with its fallback.
    void attachVoxi(Engine&);

    // Creates and registers the skinning feature. Registered FIRST of all render features, so its
    // prePass stages this frame's bone matrices before anything asks for a posed handle.
    void attachSkinning(Engine&);

    // Initialises and registers the particle renderer, and points it at the process-global
    // ParticleSystem (matching aver::anim::animSystem()) -- the same seam SandboxApp.cpp wires, so a
    // project's CParticleEmitter placements draw identically in the editor and the shipped game.
    void attachParticles(Engine&);

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // --particle-test only. Spawns the same dust-cloud + ember-burst content SandboxApp.cpp's own
    // --particle-test does (an occluder cube plus two emitters) as scene entities, and points the
    // default camera at it. See GameApp.cpp's definition for why the ECS shape differs from the
    // editor's objects_-based version despite the effect data being identical.
    void spawnParticleTestContent(rhi::IDevice& device);
#endif

#if AVER_WITH_UI_ABI
    // Creates and registers the HUD's render feature. A create() failure is non-fatal: the game
    // runs on with gameUi_ null and no HUD.
    void attachGameUi(Engine& e);
    // Hands the UI ABI's draw list to the HUD render feature, once per frame. The editor's
    // submitGameUi also draws its UI demo widget first; that stays in the editor.
    void submitGameUi(Engine& e);
#endif

    // Attaches the density-volume builder and queues a build. --pcg-volume-test only.
    void attachPcgTest(Engine&);
    // Compares the finished GPU field against pcg::sampleDensity and reports. Returns true when the
    // comparison has run, so the caller can stop.
    bool checkPcgVolume();

    // Starts the physics world. MUST run before any level loads: loading builds a static body per
    // colliding placement, gated on aver_phys_ready(), so a level loaded first silently gets no
    // collision and the player falls through the floor.
    void initPhysics();

    // Runs the gameplay tick groups around the physics step, when a session is playing.
    void tickGameplay(f32 dt);

    // VISUAL SCRIPTING PHASE 2: a packaged game running any C# at all -- graphs included -- needs the
    // in-process CLR host bootstrapped somewhere, and nothing did that for AverGame.exe before this
    // (grep the tree: Aver.Scripting.Host was linked by Runtime/CMakeLists.txt but never
    // constructed by anything in it -- only sandbox/src/SandboxApp.cpp, the editor, ever stood up a
    // ScriptHost). initScripting starts it; discoverProjectGraphs and tickProjectGraphs are the
    // graph-specific pieces built on top of it. See GameApp.cpp's onInit/onUpdate for where each is
    // called and why, and the phase-2 report for the fuller "where does this belong" reasoning the
    // task asked for.
    void initScripting();

    // Walks the open project's content for *.ocgraph files (via GameContent::pathsWithExtension) and
    // loads each one through ScriptHost's existing, UNCHANGED entity-scoped graph API -- see its own
    // definition in GameApp.cpp for why a synthetic id stands in for a real entity here, and why that
    // is safe. A project with none is a silent, correct no-op: "a no-graph project behaves exactly as
    // before this feature existed" is one of the two things visual-scripting phase 2 has to prove.
    void discoverProjectGraphs();

    // Ticks every graph discoverProjectGraphs found. Called every frame from onUpdate(), beside
    // tickGameplay() rather than in a loop of its own -- see its own definition for why it is NOT
    // gated on the framework's play state the way tickGameplay is.
    void tickProjectGraphs(f32 dt);

    // THE OTHER HALF OF THE GAP initScripting's header comment names and deliberately leaves open:
    // "nothing here calls ... aver_fw_begin_play() (which is what would let an actor class ever get
    // bound to an entity and ticked)". Without it, EVERY piece of the actor framework a project might
    // author -- AverGameMode, AverCharacter's capsule/mouse-look/view-entity camera, AverPlayerController
    // possession, Actor.OnTick via the PrePhysics/Physics/PostPhysics groups -- is dead code in a
    // shipped game: aver_fw_play_state() never leaves AVER_FW_PLAY_EDITOR, so tickGameplay() and
    // drivePlayCamera() both return on their first line, forever, regardless of what a project declares.
    //
    // WHY THIS IS THE RIGHT SIZE OF FIX rather than un-gating tickGameplay/drivePlayCamera themselves:
    // those two functions are correct AS WRITTEN -- a session should exist before the world simulates
    // or the camera follows a pawn. What was missing is the one native call that ever MAKES a session
    // exist. A shipped game has no editor Play button to press, so "boot the game" and "begin playing"
    // are the same moment for it -- exactly the reasoning tickProjectGraphs' own comment already uses
    // for why graphs run ungated here ("a scripting-layer feature ... gating it ... would make it
    // silently inert in exactly the configuration most likely to be the only gameplay a project has").
    //
    // WHY THIS IS GENERIC, NOT A FEATURE IN DISGUISE: this function does not know what a GameMode IS,
    // let alone what any project's GameMode does. It asks the framework, by FLAG bits already defined
    // in framework_abi.h (AVER_FW_CLASS_GAME_MODE / AVER_FW_CLASS_GAME_INSTANCE) and already resolved
    // by an existing, general query (aver_fw_find_class_with_flags -- built for exactly this "does
    // anything declare one of these" question, and unused by any caller in this tree until now),
    // whether the CURRENTLY LOADED PROJECT declared one. A project with no GameMode gets exactly the
    // same no-op this engine has always given it: aver_fw_begin_play is never called, play_state stays
    // EDITOR, and the framework is exactly as inert as it was before this function existed -- the same
    // "no-graph project is unaffected" shape discoverProjectGraphs already established for graphs.
    void beginPlayIfGameModeDeclared();

    // Places the possessed pawn at the level's authored Player Start / SPAWN record, through
    // game::placePossessedPawn (GamePawn.hpp) -- minus the PlayerStart MARKER lookup half of the
    // editor's playerStartTransform, which a shipped game has no equivalent of (see
    // GameLevel::spawn()'s own comment): the raw SPAWN record GameLevel::load already captured is
    // the whole answer here. Called from beginPlayIfGameModeDeclared(), AFTER aver_fw_begin_play
    // succeeds -- same ordering reason as the editor's own call site: the pawn does not exist until
    // then. Returns false (pawn left wherever the GameMode's own spawn logic put it) when the level
    // declares no SPAWN record.
    bool placePawnAtSpawn();

    // Drives the camera from the possessed pawn. Must run AFTER World::flush and BEFORE the view
    // matrix is built, or the camera trails the pawn by one frame.
    void drivePlayCamera();

    // ---- OS mouse capture, through game::MouseCapture (MouseCapture.hpp) -- ClipCursor + hidden
    // cursor + re-centre every frame, deltas measured from the re-centre rather than from
    // WM_MOUSEMOVE, refusing to warp a background window. Absent from the Runtime before this:
    // mouse-look stopped turning the instant the OS cursor reached the window edge. See
    // GameConfig::noMouseCapture for the bounded-run gate.

    // Gives the mouse to the game or hands it back: mouse_.set(on, window_, "Game").
    void setMouseCaptured(bool on);
    // Measures one frame of captured mouse movement and re-centres for the next: mouse_.poll(window_).
    void pollCapturedMouse();

    // Pushes the camera, sky, fog and post settings to the device for this frame.
    void pushFrame(Engine&);

    // --screenshot only, and only on a bounded (--frames N) run: requests a capture a few frames
    // before the run ends and writes it to cfg_.screenshotPath once the RHI has it ready. See
    // GameApp.cpp's definition for why capture and readback cannot both happen on the same frame.
    void captureScreenshotIfDue(Engine&);

    // Applies the loaded level's sun and sky settings to the sky atmosphere.
    void applyLevelSky();
    void fitGiVolumeToLevel();

    // Applies the project's render settings to Voxi.
    void applyProjectRenderSettings();

    // Applies RENDER.EXPOSURE / BLOOM / AUTOEXPOSURE / TONEMAP to post_, the struct pushFrame hands
    // the device every frame. Separate from applyProjectRenderSettings because none of that
    // function's three guards -- AVER_MODULE_VOXI, hasRenderSettings(), voxiAttached_ -- has anything
    // to say about a tone curve; see the definition.
    void applyProjectPostSettings();

    // Depth proxy resolver for shadow/voxel passes using LOD data.
    static rhi::MeshHandle depthProxyLookup(rhi::MeshHandle mesh, void* user);

#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // particles DECIDED 4: the two halves of particles::ParticleRenderer::GiSeam, installed in
    // attachParticles once Voxi has already attached -- see GameApp.cpp's definitions for the whole
    // contract (identical to sandbox/src/SandboxApp.cpp's own pair of the same name; this module has
    // no shared home for them to live in without either side gaining a dependency the other must
    // not have, so each composition root repeats this small amount of glue).
    static bool particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                  std::string* outPrelude, std::string* outDefines, void* user);
    static void particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                               const void** outCbData, u32* outCbBytes, void* user);
#endif

    GameConfig cfg_;
    InputState input_;

    // Borrowed from the engine in onInit, for the HWND mouse capture needs -- mirrors
    // SandboxApp.hpp's own `window_` member (same comment there: "borrowed from the engine in
    // onInit, for the HWND").
    Window* window_ = nullptr;

    // --- mouse capture --- the cursor is hidden, confined and re-centred every frame while
    // captured; see MouseCapture.hpp.
    game::MouseCapture mouse_;

    // --- camera ---
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32  yaw_ = 0.0f, pitch_ = 0.0f;
    Mat4 invVP_, viewProj_;
    Vec3 eye_{0, 0, 0};

#if AVER_MODULE_SCENE
    // The possessed pawn, WHEN THE VIEW IS FIRST-PERSON -- set every call by drivePlayCamera()
    // (reset to kInvalidEntity unconditionally first, mirroring SandboxPlay.cpp's own
    // firstPersonPawn_: a value left over from a previous call is not merely wrong but dangerous).
    // Read once a frame in onRender to build DrawWorldOptions::ownerHideRoot, so an entity flagged
    // kMeshRendererHiddenFromOwner under the pawn is skipped in the raster pass -- without this a
    // first-person character's own body renders in front of the camera.
    scene::Entity firstPersonPawn_ = scene::kInvalidEntity;
#endif

    // --- environment ---
    rhi::SkyAtmosphere sky_;
    rhi::PostSettings  post_;
    f32 sunColor_[3]  = {1.0f, 0.96f, 0.90f};
    f32 sunAmbient_   = 1.0f;
    f32 skyZenith_[3] = {0.24f, 0.45f, 0.85f};
    f32 skyHorizon_[3]= {0.72f, 0.83f, 0.95f};
    f32 fogColor_[3]  = {1.0f, 1.0f, 1.0f};
    f32 fogDensity_   = 4e-6f;
    f32 cloudTime_    = 0.0f;
    // Whether a script currently owns the sky. Only so the handover is logged ONCE each way rather
    // than every frame; nothing reads it to decide anything.
    bool scriptSkyReported_ = false;

    // --- GI volume tracking ---
    // The center and extent of the volume for indirect lighting computation. Tracked from the camera/player position.
    // The GI volume, fitted to the level by fitGiVolumeToLevel(). The fallback is the EDITOR's own
    // compiled default (SandboxApp.cpp's `Vec3 giCenter_{0,0,300}; f32 giExtent_=1200.0f;`), so a
    // level with no placements to fit lands where the editor would have put it rather than on a
    // number invented here.
    Vec3 giCenter_{0.0f, 0.0f, 300.0f};
    f32 giExtent_ = 1200.0f;   // cm

    SceneDrawStats drawStats_;

    // AverSR (3.3 C). NON-OWNING on the device's side, exactly like rhi::IDevice::setUpscaler's own
    // comment describes for the editor's identical member: the device holds a bare pointer into this,
    // so it is detached (setUpscaler(nullptr)) before this unique_ptr ever resets -- see onShutdown,
    // first statement after `dev`, mirroring the ordering SandboxApp.cpp's own clearAverSrUpscaler /
    // applyAverSrQuality already guard for the identical crash class (--aversr-cycle). Stays null for
    // the whole run whenever averSrInstaller_ is null or onInit resolves Off.
    std::unique_ptr<rhi::IUpscaler> averSrUpscaler_;
    // Set once, from the composition root, by setAverSrInstaller -- see that method's own comment.
    // Null is the ordinary, legal "this build has no SR module linked" case, not an error state.
    AverSrInstaller averSrInstaller_ = nullptr;

#if AVER_MODULE_VOXI
    // BY VALUE, and registered NON-OWNING with addRenderFeature. The device holds a bare pointer to
    // it, so it must outlive the device -- which is why it is a member here and torn down in
    // onShutdown rather than being a local or a unique_ptr handed away.
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_ = false;
    // The frame-budget controller's own state (FrameBudget.hpp) -- one instance for this app's
    // whole run, ticked once a frame from onUpdate and seeded from project_.frameBudgetMs the moment
    // openProject knows it. Mirrors SandboxApp.hpp's own frameBudget* member group.
    voxi::FrameBudgetState frameBudget_;
#endif
#if AVER_MODULE_SCENE
    // OWNED, unlike voxiRenderer_ which is a member by value -- SkinnedScene is created only if
    // init succeeds, and a failed init must leave nothing registered rather than an inert member.
    // Null is a LEGAL state: skinned entities then draw at their rest pose rather than not at all.
    std::unique_ptr<render::SkinnedScene> skinnedScene_;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // BY VALUE and registered NON-OWNING, same reasoning as voxiRenderer_ just above.
    particles::ParticleRenderer particleRenderer_;
    bool particlesAttached_ = false;
#endif
#if AVER_WITH_UI_ABI
    // The HUD's render feature. Owned (UiRenderer::create hands back ownership, and there is no
    // value to hold when it fails); null means no HUD.
    render::ui::UiRenderer* gameUi_ = nullptr;
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    // Loaded once per level, right after level_.loadStartMap() -- mirrors SandboxApp's own nav_
    // member. EMPTY (default-constructed) is a legal, common state: most levels have no baked
    // navigation, and AgentSystem::tick treats a grid that fails OcNavData::valid() as "not
    // available yet" rather than an error.
    fmt::OcNavData gameNav_;
#endif
    // --screenshot bookkeeping (captureScreenshotIfDue). Latched true once the PNG is written, so a
    // capture requested near the end of a run is not re-requested every remaining frame.
    bool screenshotDone_ = false;
    // OUTSIDE the scene guard, and it was inside it. Aver.Render.Pcg is linked UNCONDITIONALLY
    // (Runtime/CMakeLists.txt), there is no AVER_MODULE_PCG switch, and this header
    // already includes aver/pcg/PcgVolume.hpp outside every guard -- so the members had no business
    // being scene-conditional. GameApp.cpp agreed with the CMake rather than with the header:
    // attachPcgTest, checkPcgVolume and the onShutdown cleanup are all correctly unguarded, so with
    // AVER_MODULE_SCENE=0 the members vanished while their users remained.
    pcg::VolumeBuilder pcgVolume_;
    pcg::VolumeSpec    pcgSpec_{};
    bool               pcgAttached_ = false;
    bool               pcgChecked_ = false;
    // No physics members here any more: there is no implicit ground, so nothing for GameApp to
    // own besides the world itself, which aver_phys_init/aver_phys_shutdown manage globally.
    // Counted so "did physics step at all" is answerable from a log rather than a debugger.
    u64 physSteps_ = 0;
    u64 lastReportedSteps_ = 0;

#if AVER_MODULE_SCRIPTING
    // Owned here, not by GameContent or GameLevel: its lifetime is the WHOLE APPLICATION's, not the
    // current project's. ScriptHost starts an in-process .NET runtime, which this engine has never
    // supported tearing down and re-initialising within one process (see ScriptHost.hpp's own "one
    // per process" phrasing), so this member is constructed once and lives exactly as long as GameApp
    // does -- never reset on a project change the way content_/level_ are.
    // Installed onto anim::animSystem() once the scripting host is up -- see its own definition.
    static void animNotify(scene::Entity e, const char* name, void* user);
    // Answers the framework's relayed animation-curve query -- see its own definition.
    static i32 animCurve(i32 entity, i64 nameHash, f32* outValue, void* user);
#if AVER_MODULE_SYNAPSE_SCENE
    // Answers the framework's relayed Synapse steering-target query -- see its own definition.
    static i32 synapseTarget(i32 entity, f32* outX, f32* outY, f32* outZ, void* user);
#if AVER_MODULE_FRAMEWORK
    // PerceptionSystem's own TargetResolverFn -- who agents should perceive. See its own definition.
    static scene::Entity synapseTargetResolver(void* user);
#endif
    // Answers the framework's relayed Synapse perception query -- see its own definition.
    static i32 synapsePerception(i32 entity, i32* outCanSee, i32* outLastTarget,
                                 f32* outTimeSinceSeen, void* user);
#endif

    aver::scripting::ScriptHost scripts_;
    bool scriptsReady_ = false;

    // One discovered project-level .ocgraph: its absolute path, the synthetic (always-negative, see
    // discoverProjectGraphs) entity id it was bound to, and whether that load succeeded.
    struct ProjectGraph { std::string path; i32 syntheticEntity; bool loaded; };
    std::vector<ProjectGraph> projectGraphs_;
#endif

    std::string echoHeld_, echoLast_;
    fmt::ProjectDesc project_;
    GameContent content_;
    GameLevel level_;
#if AVER_MODULE_LANDSCAPE
    // The level's terrain, loaded and unloaded with the level through level_'s LoadHooks (see
    // installLevelHooks) and drawn in onRender before the entities, as in the editor.
    GameLandscape landscape_;
#endif
#if AVER_MODULE_FLUIDS
    // The level's water, through the same hooks as landscape_, plus the fluid volumes scripts spawn.
    GameWater water_;
#endif
#if AVER_MODULE_SCENE
    // PCG scatter streamed around the camera; see GameConfig::chunkStreamAutoFrames.
    GameStreaming streaming_;
    // Frames left before streaming switches on; 0 = off or already done.
    int chunkStreamFramesLeft_ = 0;
#endif
    u64 frames_ = 0;
    // Seconds since the last --stats dump. See GameConfig::statsIntervalSec.
    f32 statsTimer_ = 0.0f;
    // M7: true once the one-shot END-OF-RUN --stats dump has fired on a bounded (--frames N) run --
    // see onUpdate's own comment for why a bounded run needs this in addition to the periodic
    // statsTimer_ dump above (a short --frames run can end before statsIntervalSec ever elapses once).
    // Mirrors SandboxApp::gpuTimingDone_'s latch, same reasoning: fire exactly once, near the last
    // frame, never again.
    bool statsFinalDumped_ = false;

    // beginPlayIfGameModeDeclared's own answer, latched at boot: true once
    // aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE) has found a concrete GameMode in this
    // project, in whatever language declared it -- a graph class reaches that registry exactly as a
    // C# one does, which is the premise onUpdate's graph-class tick used to get wrong. Read once a
    // frame by that tick, which needs "nothing here can ever begin play" (gate off, tick forever)
    // told apart from "play has not begun yet" (gate on); see its call site for the measurement
    // that made it a gate. A mirror rather than a second call of that query, so the two cannot
    // disagree. Defaults to false -- the answer that ticks -- so a build with the framework
    // compiled out, or a boot that never got as far as asking, behaves exactly as it did before
    // this member existed.
    bool gameModeDeclared_ = false;
};

} // namespace aver::game
