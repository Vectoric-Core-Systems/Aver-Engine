// GameApp: the aver::Application a shipped game runs, as opposed to the editor.
#pragma once
#include "aver/runtime/Application.hpp"
#include "aver/platform/InputState.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/pcg/PcgVolume.hpp"
#include "aver/neurafi/NeuraFI.hpp"

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
// UI font uploaded through this process's device, not the editor's (see GameApp.cpp).
#  include "aver/ui/UiFont.hpp"
#endif
#include "aver/game/GameContent.hpp"
#include "aver/game/GameLevel.hpp"
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
#  include "aver/synapse/CrowdGpu.hpp"
#endif
#if AVER_MODULE_SCENE
#  include "aver/prefab/PrefabAbiHost.hpp"
#  include "aver/prefab/PrefabSystem.hpp"
#  include "aver/prefab/prefab_abi.h"
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
#  include "aver/game/SceneDecalFeed.hpp"
#endif
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
#include "aver/game/PlayMobility.hpp"
#include "aver/game/LevelSequence.hpp"
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
#  include "aver/world/VehicleSystem.hpp"
#endif

#include <memory>
#include <string>

namespace aver {
// Borrowed from the engine for mouse capture. See GameApp::setMouseCaptured.
class Window;
}

namespace aver::game {

// Everything GameApp needs from the command line or game.json.
struct GameConfig {
    // Default window title, width and height.
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
    // The .ocproject to open. Empty means no world.
    std::string projectPath;
    // A bare .ocworld/.ocmap argument: the level to open instead of the project's STARTMAP.
    // Empty = STARTMAP. Same semantics as the editor's openMapPath_.
    std::string levelPath;
    // Logs the held-key set on every change (ImGui-free input path testing).
    bool inputEcho = false;
    // Logs every path the engine opens (package contents verification).
    bool traceOpens = false;
    // --scene-census: print scene contents in aver::world::formatSceneCensus format.
    // Sandbox.exe prints the identical line, and verify-game.ps1 compares them.
    bool sceneCensus = false;
    // --stats [seconds]: periodically log the per-pass GPU breakdown. D3D12Device::initGpuTiming
    // runs unconditionally; this flag enables logging. 0 means off. The interval is in seconds
    // because the report averages over frames since boot and changes slowly.
    f32 statsIntervalSec = 0.0f;
    // --pcg-volume-test: fills a GPU density volume and compares each voxel against the CPU mirror.
    bool pcgVolumeTest = false;
    // --chunk-stream [N] / --no-chunk-stream: PCG chunk streaming switches on N frames in.
    // Default 5 frames (on by default, matching the editor).
    int chunkStreamAutoFrames = 5;
    // Particle GI flag. See sandbox/src/SandboxApp.cpp::setNoParticleGi for the full contract.
    bool noParticleGi = false;
    // --particle-test: spawns the same dust cloud + ember burst as the editor (for parity verification).
    bool particleTest = false;
    // --screenshot [path]: captures to PNG. Empty = no screenshot.
    std::string screenshotPath;
    // --no-vsync: disables vsync once the device exists. False (DEFAULT) syncs to display.
    bool vsyncOff = false;
    // --no-mouse-capture: disables OS mouse capture (ClipCursor + hidden cursor).
    // Auto-disabled on bounded (--frames N) runs regardless of this flag.
    bool noMouseCapture = false;
    // --cam-wobble DEG PERIOD: measurement-only yaw swing (0 = no motion). See SandboxApp.cpp.
    f32 camWobbleDeg = 0.0f;      // yaw amplitude in degrees
    u32 camWobblePeriod = 0;      // period in FRAMES; sin() is 0 at every whole multiple

    // --aversr off|quality|balanced|performance|auto: render upscaler level (-1 = not given).
    // Parsed locally; GameApp never includes aver/sr/* (module boundary).
    int averSrArg = -1;   // -1 = unstated/auto, else the ladder's numbering 0..3
    // --frame-interp 0|1: frame interpolation over the project's RENDER.FRAMEINTERP. -1 = not given.
    int frameInterpArg = -1;
    // --frame-interp-trajectory linear|quadratic|neural: trajectory mode for frame interpolation.
    int frameInterpTrajectory = 2;
};

// Parses the arguments a game executable accepts. Unknown arguments are ignored.
GameConfig parseArgs(int argc, char** argv);

// The game-side Application. The editor lives in SandboxApp; this moves the game half here.
class GameApp final : public Application {
public:
    explicit GameApp(GameConfig cfg);

    // AverSR upscaler installer. Called once by createApplication, before onInit.
    // Left null is the legal default for builds without the SR module.
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

    // Aspect ratio: the editor's dockspace divides the viewport, but a game is fullscreen.
    f32 viewAspect(const Engine&) const;

private:
    // Backing store for the window title config() may take from the project (mutable for const config()).
    mutable std::string windowTitleOwned_;

    // Loads cfg_.projectPath. Logs and leaves project_ invalid on failure (a game with no world is diagnosable).
    void openProject(Engine&);
    // Installs level hooks (water and terrain setup) before the first load.
    void installLevelHooks(Engine&);
    // Switches chunk streaming on for the loaded level.
    void enableChunkStreaming();

    // Initialises and registers the Voxi renderer (also creates pbr::MaterialSystem).
    void attachVoxi(Engine&);

    // Creates and registers the skinning feature (registered first, so poses are available to other features).
    void attachSkinning(Engine&);

    // Initialises and registers the particle renderer.
    void attachParticles(Engine&);

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // --particle-test only. Spawns the same dust-cloud + ember-burst as the editor.
    void spawnParticleTestContent(rhi::IDevice& device);
#endif

#if AVER_WITH_UI_ABI
    // Creates and registers the HUD render feature. Non-fatal: null means no HUD.
    void attachGameUi(Engine& e);
    // Hands the UI ABI's draw list to the HUD render feature once per frame.
    void submitGameUi(Engine& e);
    // Loads and uploads Roboto-Regular.ocfont. Non-fatal: missing font leaves uiFont_ invalid.
    void loadGameUiFont(Engine& e);
#endif

    // --pcg-volume-test only. Attaches the density-volume builder and queues a build.
    void attachPcgTest(Engine&);
    // Compares the finished GPU field against pcg::sampleDensity and reports. Returns true when done.
    bool checkPcgVolume();

    // Starts the physics world. Must run before level load (levels build static bodies on load).
    void initPhysics();

    // Runs the gameplay tick groups around the physics step when playing.
    void tickGameplay(f32 dt);

    // Bootstraps the in-process CLR host for C# graphs. See initScripting.
    void initScripting();

    // Walks the project for *.ocgraph files and loads each through ScriptHost's graph API.
    void discoverProjectGraphs();

    // Ticks every graph discoverProjectGraphs found (called every frame, not gated on play state).
    void tickProjectGraphs(f32 dt);

    // Calls aver_fw_begin_play if a GameMode is declared. A shipped game has no Play button, so boot and play are simultaneous.
    void beginPlayIfGameModeDeclared();

    // Places the possessed pawn at the level's SPAWN record (if present). Called after aver_fw_begin_play.
    bool placePawnAtSpawn();

    // Drives the camera from the possessed pawn. Must run after World::flush and before view matrix is built.
    void drivePlayCamera();

    // ---- OS mouse capture (game::MouseCapture).

    // Gives the mouse to the game or hands it back.
    void setMouseCaptured(bool on);
    // Measures one frame of captured mouse movement and re-centres for the next.
    void pollCapturedMouse();

    // Pushes the camera, sky, fog and post settings to the device for this frame.
    void pushFrame(Engine&);

    // --screenshot only, bounded runs: requests a capture and writes to cfg_.screenshotPath when ready.
    void captureScreenshotIfDue(Engine&);

    // Applies the loaded level's sun and sky settings to the sky atmosphere.
    void applyLevelSky();
    void fitGiVolumeToLevel();

    // Applies the project's render settings to Voxi.
    void applyProjectRenderSettings();

    // Applies RENDER.EXPOSURE / BLOOM / AUTOEXPOSURE / TONEMAP to post_.
    void applyProjectPostSettings();

    // Depth proxy resolver for shadow/voxel passes using LOD data.
    static rhi::MeshHandle depthProxyLookup(rhi::MeshHandle mesh, void* user);

#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // particles DECIDED 4: the two halves of particles::ParticleRenderer::GiSeam.
    // See GameApp.cpp for the full contract (identical to SandboxApp.cpp).
    static bool particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                  std::string* outPrelude, std::string* outDefines, void* user);
    static void particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                               const void** outCbData, u32* outCbBytes, void* user);
#endif

    GameConfig cfg_;
    InputState input_;

    // Borrowed from the engine in onInit (HWND for mouse capture).
    Window* window_ = nullptr;

    // ---- mouse capture ---- cursor is hidden, confined and re-centred every frame.
    game::MouseCapture mouse_;

    // ---- camera ----
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32  yaw_ = 0.0f, pitch_ = 0.0f;
    Mat4 invVP_, viewProj_;
    Vec3 eye_{0, 0, 0};

#if AVER_MODULE_SCENE
    // The possessed pawn (when first-person). Reset to kInvalidEntity unconditionally each call.
    // Read in onRender to build DrawWorldOptions::ownerHideRoot (first-person body culling).
    scene::Entity firstPersonPawn_ = scene::kInvalidEntity;
    // What moves during the session, so Voxi keeps it out of the GI bake (PlayMobility.hpp).
    game::PlayMobility playMobility_;
    // The level's own sequence (.ocworld SEQ records): moves actors, the view and emissive while it plays.
    game::SequencePlayer sequencePlayer_;
    // DrawWorldOptions::emissiveScale sink; user is the GameApp.
    static bool sequenceEmissiveScale(scene::Entity e, f32 outRgb[3], void* user);
#  if AVER_MODULE_PHYSICS
    // The level's physics cars (world::VehicleSystem). Built once play begins.
    // Cars only advance while playing, so projects with no GameMode never build them.
    world::VehicleSystem vehicles_;
#  endif
#endif

    // ---- environment ----
    rhi::SkyAtmosphere sky_;
    rhi::PostSettings  post_;
    // Top-of-atmosphere colour (white; sky tints it by elevation).
    f32 sunColor_[3]  = {1.0f, 1.0f, 1.0f};
    f32 sunAmbient_   = 1.0f;
    f32 skyZenith_[3] = {0.24f, 0.45f, 0.85f};
    f32 skyHorizon_[3]= {0.72f, 0.83f, 0.95f};
    f32 fogColor_[3]  = {1.0f, 1.0f, 1.0f};
    f32 fogDensity_   = 4e-6f;
    f32 cloudTime_    = 0.0f;
    // Latches true once a script has owned the sky (prevents repeated logging).
    bool scriptSkyReported_ = false;

    // ---- GI volume tracking ----
    // The GI volume center and extent. Fitted to the level by fitGiVolumeToLevel().
    // Fallback matches the editor's defaults.
    Vec3 giCenter_{0.0f, 0.0f, 300.0f};
    f32 giExtent_ = 1200.0f;   // cm

    SceneDrawStats drawStats_;

    // AverSR upscaler. Detached before reset (see onShutdown). Null if unavailable or off.
    std::unique_ptr<rhi::IUpscaler> averSrUpscaler_;
    // Set once by setAverSrInstaller. Null is the ordinary "this build has no SR module" case.
    AverSrInstaller averSrInstaller_ = nullptr;

    // Frame interpolation. Detached from the device in onShutdown before it resets.
    std::unique_ptr<neurafi::NeuraFI> frameInterpolator_;

#if AVER_MODULE_VOXI
    // Registered NON-OWNING with addRenderFeature. Must outlive the device.
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_ = false;
#if AVER_MODULE_SCENE
    SceneDecalFeed sceneDecalFeed_;   // CDecal entities -> Voxi's projected-decal list, per frame
#endif
    u32  msaaPushed_ = 0;   // sample count last given to the device
    // Frame-budget state. Ticked once a frame and seeded from project_.frameBudgetMs.
    voxi::FrameBudgetState frameBudget_;
#endif
#if AVER_MODULE_SCENE
    // SkinnedScene. Null is a legal state: skinned entities draw at rest pose.
    std::unique_ptr<render::SkinnedScene> skinnedScene_;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registered NON-OWNING. Must outlive the device.
    particles::ParticleRenderer particleRenderer_;
    bool particlesAttached_ = false;
#endif
#if AVER_WITH_UI_ABI
    // The HUD render feature. Null means no HUD.
    render::ui::UiRenderer* gameUi_ = nullptr;
    // The game UI's font. Invalid until loadGameUiFont finds one. Re-lent to aver_ui_set_font every frame.
    ui::UiFont uiFont_;
    // The UI font atlas. Borrowed by the ABI; torn down before the device.
    rhi::TextureHandle uiFontTexture_ = 0;
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    // Loaded once per level after level_.loadStartMap(). Empty is a legal state (most levels have no nav).
    fmt::OcNavData gameNav_;
#endif
    // --screenshot bookkeeping. Latched true once the PNG is written.
    bool screenshotDone_ = false;
    // PCG volume members. Aver.Render.Pcg is linked unconditionally.
    pcg::VolumeBuilder pcgVolume_;
    pcg::VolumeSpec    pcgSpec_{};
    bool               pcgAttached_ = false;
    bool               pcgChecked_ = false;
    // Physics step counter (fixed steps, not frames).
    u64 physSteps_ = 0;
    u64 lastReportedSteps_ = 0;

    // ---- THE STATIC RELAYS ----
    // Each guard below is what that relay's body needs (for consistency with GameApp.cpp's definitions).

#if AVER_MODULE_SCENE
    // Raises an animation notify as a graph event.
    static void animNotify(scene::Entity e, const char* name, void* user);
    // Answers the framework's relayed animation-curve query.
    static i32 animCurve(i32 entity, i64 nameHash, f32* outValue, void* user);
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    // Answers the framework's relayed Synapse steering-target query.
    static i32 synapseTarget(i32 entity, f32* outX, f32* outY, f32* outZ, void* user);
#if AVER_MODULE_FRAMEWORK
    // PerceptionSystem's TargetResolverFn. Asks aver_fw_controlled_pawn who the player possesses.
    static scene::Entity synapseTargetResolver(void* user);
#endif
    // Answers the framework's relayed Synapse perception query.
    static i32 synapsePerception(i32 entity, i32* outCanSee, i32* outLastTarget,
                                 f32* outTimeSinceSeen, void* user);
#endif

#if AVER_MODULE_SCRIPTING
    // The in-process .NET runtime (one per process; never torn down and re-init).
    aver::scripting::ScriptHost scripts_;
    bool scriptsReady_ = false;

    // One discovered project-level .ocgraph: path, synthetic entity id, and load status.
    struct ProjectGraph { std::string path; i32 syntheticEntity; bool loaded; };
    std::vector<ProjectGraph> projectGraphs_;
#endif

    std::string echoHeld_, echoLast_;
    fmt::ProjectDesc project_;
    GameContent content_;
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    // The GPU crowd-avoidance backend, installed behind the Synapse ABI's one SynapseAi (game::installCrowdGpu).
    synapse::GpuCrowdBackend crowdGpu_;
    bool crowdGpuAttached_ = false;
#endif
#if AVER_MODULE_SCENE
    // Prefab instances (docs/PREFABS.md): the library loads .ocprefab out of the project content, the system
    // spawns level PREFABINST records and serves the scripting ABI. Built in init, after content_.
    prefab::PrefabLibrary prefabLib_;
    std::unique_ptr<prefab::PrefabSystem> prefabSys_;
    AverPrefabHost prefabHost_{};
#endif
    GameLevel level_;
#if AVER_MODULE_LANDSCAPE
    // The level's terrain. Loaded/unloaded with the level (LoadHooks).
    GameLandscape landscape_;
#endif
#if AVER_MODULE_FLUIDS
    // The level's water (LoadHooks) plus fluid volumes scripts spawn.
    GameWater water_;
#endif
#if AVER_MODULE_SCENE
    // PCG scatter streamed around the camera.
    GameStreaming streaming_;
    // Frames left before streaming switches on; 0 = off or already done.
    int chunkStreamFramesLeft_ = 0;
#endif
    u64 frames_ = 0;
    // Seconds since the last --stats dump.
    f32 statsTimer_ = 0.0f;
    // One-shot END-OF-RUN --stats dump latch (bounded runs only).
    // Mirrors SandboxApp::gpuTimingDone_.
    bool statsFinalDumped_ = false;

    // True once aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE) has found a GameMode.
    // Defaults to false (gate off, tick forever). A mirror, not a second call, so the two cannot disagree.
    bool gameModeDeclared_ = false;
    std::string levelGameMode_, levelDefaultPawn_;   // the loaded level's World Settings overrides
};

} // namespace aver::game
