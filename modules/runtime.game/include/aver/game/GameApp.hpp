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
#include "aver/game/GameContent.hpp"
#include "aver/game/GameLevel.hpp"
#include "aver/game/GameRender.hpp"
#include "aver/game/GameInput.hpp"

#include <memory>
#include <string>

namespace aver::game {

// Everything GameApp needs that comes off the command line or out of game.json.
struct GameConfig {
    std::string title = "Aver Game";
    u32 width = 1280;
    u32 height = 720;
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
    // Fills a density volume on the GPU and compares every voxel against the CPU mirror, then
    // exits. The only way to check the HLSL against its reference: HLSL compiles at RUNTIME, so a
    // green build says nothing about whether the shader agrees with anything.
    bool pcgVolumeTest = false;
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

    // Drives the camera from the possessed pawn. Must run AFTER World::flush and BEFORE the view
    // matrix is built, or the camera trails the pawn by one frame.
    void drivePlayCamera();

    // Pushes the camera, sky, fog and post settings to the device for this frame.
    void pushFrame(Engine&);

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
    std::string echoHeld_, echoLast_;
    fmt::ProjectDesc project_;
    GameContent content_;
    GameLevel level_;
    u64 frames_ = 0;
};

} // namespace aver::game
