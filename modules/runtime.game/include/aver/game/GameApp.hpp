// GameApp: the aver::Application a shipped game runs, as opposed to the editor.
#pragma once
#include "aver/runtime/Application.hpp"
#include "aver/platform/InputState.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/pcg/PcgVolume.hpp"

#if AVER_MODULE_VOXI
#  include "aver/voxi/VoxiRenderer.hpp"
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
#include "aver/game/GameContent.hpp"
#include "aver/game/GameLevel.hpp"
#include "aver/game/GameRender.hpp"
#include "aver/game/GameInput.hpp"

#include <memory>
#include <string>

namespace aver::game {

// Everything GameApp needs that comes off the command line or out of game.json.
struct GameConfig {
    // NAMED CONSTANTS, so config() can tell "the caller asked for 1280" apart from "nobody said".
    // Without that distinction a project's WINDOW.SIZE could never win, because the field is never
    // empty -- it always holds something.
    static constexpr const char* kDefaultTitle = "Aver Game";
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
    // unconditionally, not behind a build flag or a CLI switch, so every AverGame.exe ever shipped
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
    // --no-vsync (M7): AverGame's own measurement parity with SandboxApp.cpp's identical flag. False
    // (DEFAULT) leaves vsync exactly as the backend opened it -- a shipped game syncs to the display
    // the way a player expects. True disables it once, the instant a device exists, the same
    // vsyncCanDisable()/setVSync(false) pair the editor uses; a display path that cannot tear (see
    // rhi::IDevice::vsyncCanDisable's own comment) logs a warning and is left synced rather than
    // silently ignored.
    bool vsyncOff = false;
    // --cam-wobble DEG PERIOD (M7): the SAME measurement-only yaw swing SandboxApp.cpp's own
    // --cam-wobble drives -- see its setCamWobble's comment for why a sine that returns to zero,
    // driven off the frame counter and never the clock. 0 (DEFAULT, either field) is no motion, so
    // every existing --frames capture through this executable is bit-identical without it.
    f32 camWobbleDeg = 0.0f;      // yaw amplitude in degrees
    u32 camWobblePeriod = 0;      // period in FRAMES; sin() is 0 at every whole multiple
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
    // (grep the tree: Aver.Scripting.Host was linked by modules/runtime.game/CMakeLists.txt but never
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

    // Drives the camera from the possessed pawn. Must run AFTER World::flush and BEFORE the view
    // matrix is built, or the camera trails the pawn by one frame.
    void drivePlayCamera();

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

    // --- camera ---
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32  yaw_ = 0.0f, pitch_ = 0.0f;
    Mat4 invVP_, viewProj_;
    Vec3 eye_{0, 0, 0};

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

#if AVER_MODULE_VOXI
    // BY VALUE, and registered NON-OWNING with addRenderFeature. The device holds a bare pointer to
    // it, so it must outlive the device -- which is why it is a member here and torn down in
    // onShutdown rather than being a local or a unique_ptr handed away.
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_ = false;
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
    // (modules/runtime.game/CMakeLists.txt), there is no AVER_MODULE_PCG switch, and this header
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
    u64 frames_ = 0;
    // Seconds since the last --stats dump. See GameConfig::statsIntervalSec.
    f32 statsTimer_ = 0.0f;
    // M7: true once the one-shot END-OF-RUN --stats dump has fired on a bounded (--frames N) run --
    // see onUpdate's own comment for why a bounded run needs this in addition to the periodic
    // statsTimer_ dump above (a short --frames run can end before statsIntervalSec ever elapses once).
    // Mirrors SandboxApp::gpuTimingDone_'s latch, same reasoning: fire exactly once, near the last
    // frame, never again.
    bool statsFinalDumped_ = false;
};

} // namespace aver::game
