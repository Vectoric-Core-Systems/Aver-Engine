// SandboxApp::onUpdate: one editor frame (MCP commands, camera, gameplay tick, physics, render state). Split from
// SandboxApp.cpp, which was nearing the COFF section cap in Debug (sandbox/CMakeLists.txt: split, never /bigobj).
#include "aver/core/HitchMarks.hpp"
#include <chrono>
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "SandboxApp.hpp"
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_anim_abi.h"   // aver_fw_set_anim_provider (montages)
#  include "aver/framework/framework_ik_abi.h"     // [wave5/ik-retarget] aver_fw_set_ik_provider

#  include "aver/framework/framework_morph_abi.h"   // [wave9/morph] aver_fw_set_morph_provider
#  include "aver/framework/framework_worldfx_abi.h"   // [wave9/worldfx] aver_fw_worldfx_tick
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_abi.h"          // aver_fw_window_publish: the Play viewport as the game's window
#  include "aver/framework/framework_render_abi.h"   // P17: script render settings and statistics
#  if AVER_MODULE_VOXI
#    define AVER_SCRIPTRENDER_HAS_ABI 1
#    include "aver/voxi/ScriptRenderSettings.hpp"
#  endif
#  include "aver/framework/framework_loc_abi.h"    // the UI layout editor's culture preview
#endif
#if AVER_MODULE_RAGDOLL
#  include "aver/ragdoll/RagdollSystem.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
#include "TextureEditor.hpp"   // makeTextureEditor; SandboxApp.hpp does not pull this one in
#include "InstanceSetEditor.hpp"   // makeInstanceSetEditor (.ocinst); not pulled in by SandboxApp.hpp
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameRender.hpp"   // DrawWorldOptions, for the sub-viewports' walk
#include "aver/game/GameIkProbe.hpp"   // [wave5/ik-retarget] the foot-IK ground probe
#include "aver/game/GameTick.hpp"
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_world_abi.h"
#endif
#include "aver/game/GameSystemsWiring.hpp"
#include "aver/game/SequenceHost.hpp"   // [wave5/sequencer] the ABI provider
#include "aver/game/GameUiInput.hpp"
#include "aver/game/GameDisplay.hpp"   // resolvePointerPlan: the game's pointer request in Play
#include "aver/game/GameUiText.hpp"
#include "aver/game/ContentPrefetch.hpp"
#include "aver/platform/FileSystem.hpp"   // userDataDir: where the trajectory network's weights live
#if AVER_MODULE_VOXI
#include "ModeSwitchNotice.hpp"
#endif

namespace aver {

// Advances one frame: MCP commands, camera, gameplay tick, physics, and the render state.
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_VOXI
// A script's render keys (framework_render_abi.h) at the frame boundary. The Settings keys go through the singleton (the MSAA push and the per-frame forward
// below read it); the LOD threshold and the occlusion switch are this host's own state; the render scale is the editor viewport's and is not taken. A released
// key restores what the editor held. Everything is dropped when a Play session ends (aver_fw_render_revision).
#if AVER_MODULE_FRAMEWORK
// A gameplay tick (scripts, then every fixed physics step) over 250 ms says what physics did, at most every 5 s: the scripts name
// themselves ([Scripting] slow frame), so the rest of a slow frame is here (steps, the step's own cost, bodies, contacts).
void SandboxApp::reportSlowGameplay(f64 ms) {
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (ms < 250.0 || (last.time_since_epoch().count() != 0 && now - last < std::chrono::seconds(5))) return;
    last = now;
#if AVER_MODULE_PHYSICS
    // The first report turns the physics profiler on; the later ones say where the steps went (the largest scopes, "FixedStep" being the
    // stages that have no scope of their own: areas, contact monitors, custom integrators).
    char scopes[256] = {};
    if (aver_phys_profile_is_enabled()) aver_phys_profile_summary(reinterpret_cast<uint8_t*>(scopes), static_cast<int32_t>(sizeof scopes));
    else aver_phys_profile_enable(1);
    if (scopes[0]) AVER_WARN("[Play] slow physics, the largest scopes per step: {}", scopes);
    double st[AVER_PHYS_ST_COUNT] = {};
    aver_phys_stats(st, AVER_PHYS_ST_COUNT);
    const game::PhysicsHookCost& hook = game::physicsHookCost();
    AVER_WARN("[Play] slow gameplay tick: {:.1f} ms; physics ran {:.0f} step(s) in {:.1f} ms (last step {:.1f} ms, average {:.1f}; between steps: "
              "PHYSICS pass {:.1f} ms, pose capture {:.1f} ms); {:.0f} bodies ({:.0f} dynamic, {:.0f} awake; {:.0f} kinematic, {:.0f} static), "
              "{:.0f} contact(s), {:.0f} character(s)",
              ms, st[AVER_PHYS_ST_STEPS_LAST_CALL], st[AVER_PHYS_ST_CALL_MS_LAST], st[AVER_PHYS_ST_STEP_MS_LAST], st[AVER_PHYS_ST_STEP_MS_AVG],
              hook.physicsPassMs, hook.captureMs,
              st[AVER_PHYS_ST_BODIES], st[AVER_PHYS_ST_DYNAMIC], st[AVER_PHYS_ST_ACTIVE_DYNAMIC], st[AVER_PHYS_ST_KINEMATIC], st[AVER_PHYS_ST_STATIC],
              st[AVER_PHYS_ST_CONTACTS_LAST_CALL], st[AVER_PHYS_ST_CHARACTERS]);
#else
    AVER_WARN("[Play] slow gameplay tick: {:.1f} ms", ms);
#endif
}
#endif

void SandboxApp::applyScriptRender(voxi::Renderer& vx) {
    static voxi::ScriptRenderState st;
    voxi::Settings s = vx.settings();
    if (voxi::applyScriptRenderSettings<voxi::FrameworkRenderApi>(s, st)) vx.setSettings(s);
    float v = 0.0f;
    static bool lodTaken = false, occTaken = false;
    static f32 lodSaved = 0.0f;
    static bool occSaved = false;
    if (aver_fw_render_get(AVER_FW_RENDER_LOD_THRESHOLD, &v)) {
        if (!lodTaken) { lodTaken = true; lodSaved = lod_.lodErrorThresholdPx_; }
        lod_.lodErrorThresholdPx_ = v;
    } else if (lodTaken) {
        lodTaken = false;
        lod_.lodErrorThresholdPx_ = lodSaved;
    }
#  if AVER_MODULE_OCCLUSION
    if (aver_fw_render_get(AVER_FW_RENDER_OCCLUSION, &v)) {
        if (!occTaken) { occTaken = true; occSaved = occl_.occlusionCullEnabled_; }
        occl_.occlusionCullEnabled_ = v >= 0.5f;
    } else if (occTaken) {
        occTaken = false;
        occl_.occlusionCullEnabled_ = occSaved;
    }
#  endif
    (void)occTaken; (void)occSaved;
    if (aver_fw_render_stats_wanted()) {
        const voxi::VoxiRenderer::RenderStats rs = voxiRenderer_.renderStats();
        const int32_t out[AVER_FW_RENDER_STAT_COUNT] = {static_cast<int32_t>(rs.objects), static_cast<int32_t>(rs.triangles), static_cast<int32_t>(rs.drawCalls),
                                                        static_cast<int32_t>(rs.translucent), static_cast<int32_t>(rs.shadowDraws), static_cast<int32_t>(frameNo_)};
        aver_fw_render_publish_stats(out);
    }
}
#endif

// --set NAME VALUE, applied at frame 5 (after the project's RENDER.* apply).
void SandboxApp::applyConsoleSets(Engine& e, const Timestep& t) {
    // --set NAME VALUE applied at frame 5 (after project's RENDER.* apply).
#if AVER_WITH_IMGUI
    if (!harness_.consoleSetsApplied_ && !launch_.consoleSets_.empty() && t.frame >= 5 && e.device()) {
        harness_.consoleSetsApplied_ = true;
        for (const auto& kv : launch_.consoleSets_) {
            AVER_INFO("[Sandbox] --set {} {}", kv.first, kv.second);
            runConsoleLine(e, "set " + kv.first + " " + kv.second);
        }
    }
#else
    if (!harness_.consoleSetsApplied_ && !launch_.consoleSets_.empty()) {
        harness_.consoleSetsApplied_ = true;
        AVER_WARN("[Sandbox] --set ignored: this build has no editor console to route {} pair(s) through",
                  launch_.consoleSets_.size());
    }
#endif
}

// The view mode reaches Voxi; --cam-wobble and --cam-translate move the camera.
void SandboxApp::tickViewModeAndCameraDrift(const Timestep& t, bool& debugViewActiveThisFrame) {
#if AVER_MODULE_VOXI
    // View mode reaches Voxi from onUpdate (before prePass), unlike unlit which rides per-draw state.
    voxiRenderer_.setUnlit(rset_.unlit_);
    // Debug views need rasteriser; reasserted here before prePass for the same reason.
    const bool viewModeNeedsRaster = viewState_.wireframe_ || rset_.gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
    // Raster-only mode clears ray-hit/triangles view (wireframe_ can be ticked from Settings too).
    if (viewModeNeedsRaster) rset_.debugView_ = voxi::VoxiRenderer::ViewDebug::None;
    const bool viewModeNeedsRayDriven = !viewModeNeedsRaster &&
        rset_.debugView_ != voxi::VoxiRenderer::ViewDebug::None && voxiRenderer_.rayDrivenAvailable();
    voxiRenderer_.setViewDebug(viewModeNeedsRayDriven ? rset_.debugView_ : voxi::VoxiRenderer::ViewDebug::None);
    debugViewActiveThisFrame = viewModeNeedsRayDriven;
    // Wireframe disables shadows, GI, ray tracing and denoising (see setPaused).
    voxiRenderer_.setPaused(viewState_.wireframe_);
#endif
    // --cam-wobble-stop N: wobble runs only below frame N, then holds. Measuring artifact appearance/decay needs both.
    if (harness_.camWobbleDeg_ != 0.0f && harness_.camWobblePeriod_ > 0 &&
        (harness_.camWobbleStopFrame_ <= 0 || t.frame < (u64)harness_.camWobbleStopFrame_)) {
        if (!harness_.camWobbleBased_) { harness_.camWobbleBaseYaw_ = viewState_.yaw_; harness_.camWobbleBased_ = true; }
        const f32 phase = 6.2831853f * (f32)(t.frame - 1) / (f32)harness_.camWobblePeriod_;
        viewState_.yaw_ = harness_.camWobbleBaseYaw_ + harness_.camWobbleDeg_ * 0.01745329252f * std::sin(phase);
    }
    // --cam-translate SPEED: one fixed step per frame along camForward(), composed with wobble.
    if (harness_.camTranslateSpeed_ != 0.0f &&
        (harness_.camWobbleStopFrame_ <= 0 || t.frame < (u64)harness_.camWobbleStopFrame_)) {
        viewState_.camPos_ += camForward() * harness_.camTranslateSpeed_;
    }
}

// Neural-denoiser setup and capture, camera teleports, --cam-wander and --cam-fly.
void SandboxApp::tickCameraAutomation(Engine& e, const Timestep& t, HitchMarks& hm) {
    // --cam-wander AMP SPEED: follows sum of sines at irrational frequencies (NeuraFI training trajectory).
    // NeuRAA's training capture holds it still while it captures a pose, then it resumes where it was.
#if AVER_MODULE_SR
    bool neuraaHold = neuraa_ && neuraa_->captureHolding();
#else
    bool neuraaHold = false;
#endif
#if AVER_MODULE_VOXI
    // NRD2 training (Tools > Train Neural Denoiser, --nrd2-train): its GPU hook goes on or off here, before beginFrame.
    if (!rset_.nrd2MenuWired_ && nrd2Session_.available()) {
        tools_.setNeuralDenoiserOpener([this] { nrd2Session_.openWindow(); });
        rset_.nrd2MenuWired_ = true;
    }
    nrd2Session_.tick(e.device());
    shaderWarmup_.poll();
    tickGraphShaderWarm(e);
    hm.mark("head");
    // --nrd2-capture: handed over once the renderer is attached and the level asked for (--open-level) is the
    // one loaded, plus ~10 s for streaming to bring in the scene (the startup level used to be captured
    // instead); it starts stepping on NRD2 frames and holds the camera the same way.
    const bool nrd2LevelReady = drawerUi_.openLevelByName_.empty() && panels_.pendingOpenPath_.empty() && !panels_.pendingOpenPrompt_ &&
                                !projSess_.levelName_.empty();
    harness_.nrd2CaptureSettle_ = nrd2LevelReady ? harness_.nrd2CaptureSettle_ + 1u : 0u;
    if (harness_.nrd2CapturePoses_ && rset_.voxiAttached_ && !harness_.nrd2CaptureStarted_ && (harness_.nrd2CaptureSettle_ > 600u || t.frame > 6000)) {
        render::denoise::Nrd2CaptureConfig c;
        c.scene = projSess_.levelName_.empty() ? std::string("unnamed") : projSess_.levelName_;
        // Path Tracing feeds NRD2 different noise: its poses form their own scene for stratified training.
        if (voxi::Renderer::get().settings().pathTracing != voxi::Quality::Off) c.scene += "_PT";
        for (char& ch : c.scene)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) ch = '_';
        c.dir = (harness_.nrd2CaptureDir_.empty() || harness_.nrd2CaptureDir_ == "default" || harness_.nrd2CaptureDir_ == "-")
              ? userDataDir() + AVER_PATH_SEP "nrd2_dataset" AVER_PATH_SEP + c.scene : harness_.nrd2CaptureDir_;
        c.poses = harness_.nrd2CapturePoses_;
        c.hold = harness_.nrd2CaptureHold_;
        c.heldOutFrom = harness_.nrd2CaptureHeldOut_;
        c.oracle = harness_.nrd2CaptureGrid_ ? render::denoise::Nrd2OracleMode::Grid : render::denoise::Nrd2OracleMode::Grad;
        if (harness_.camWanderAmp_ <= 0.0f)
            AVER_WARN("[NRD2] --nrd2-capture without --cam-wander: every pose is the same view");
        voxiRenderer_.startNrd2Capture(c);
        harness_.nrd2CaptureStarted_ = true;
    }
    if (rset_.voxiAttached_ && voxiRenderer_.nrd2CaptureHolding()) neuraaHold = true;
#endif
    for (const auto& [f, p] : harness_.camTeleports_) if (f > 0 && t.frame == static_cast<u64>(f)) teleportCamera(p);
    if (neuraaHold) ++harness_.camWanderHeld_;
    if (harness_.camWanderAmp_ > 0.0f && !neuraaHold && (harness_.camWobbleStopFrame_ <= 0 || t.frame < (u64)harness_.camWobbleStopFrame_)) {
        if (!harness_.camWanderBased_) {
            harness_.camWanderBasePos_ = viewState_.camPos_; harness_.camWanderBaseYaw_ = viewState_.yaw_; harness_.camWanderBasePitch_ = viewState_.pitch_;
            harness_.camWanderBased_ = true;
        }
        const f32 s = static_cast<f32>(t.frame - 1 - harness_.camWanderHeld_) * harness_.camWanderSpeed_;
        const f32 a = harness_.camWanderAmp_;
        viewState_.yaw_ = harness_.camWanderBaseYaw_ + a * 0.4363f *   // +-25 degrees at AMP 1
               (std::sin(0.0131f * s) + 0.6f * std::sin(0.0317f * s + 1.1f) + 0.3f * std::sin(0.0719f * s + 2.3f)) / 1.9f;
        viewState_.pitch_ = harness_.camWanderBasePitch_ + a * 0.1396f *   // +-8 degrees
                 (std::sin(0.0173f * s + 0.5f) + 0.5f * std::sin(0.0437f * s + 1.3f)) / 1.5f;
        viewState_.camPos_ = harness_.camWanderBasePos_ + Vec3{a * 300.0f * std::sin(0.0091f * s + 0.2f),    // +-3 m across
                                           a * 300.0f * std::sin(0.0113f * s + 1.9f),
                                           a * 60.0f * std::sin(0.0213f * s + 0.7f)};    // +-0.6 m up/down (Z up)
    }
    if (harness_.camFlyX_ != 0.0f || harness_.camFlyY_ != 0.0f) { viewState_.camPos_.x += harness_.camFlyX_; viewState_.camPos_.y += harness_.camFlyY_; }
    if (harness_.camFlyYaw_ != 0.0f) viewState_.yaw_ += harness_.camFlyYaw_ * 0.0174533f;
}

// Occlusion flags that --no-occlusion-cull and the console re-assert every frame.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
void SandboxApp::reassertOcclusionConsole() {
    // --no-occlusion-cull: reasserted every frame (applyProjectVoxiSettings rewrites it during level load).
    if (occl_.occlusionCullForceOff_) occl_.occlusionCullEnabled_ = false;
    // --occlusion-waitidle: reasserted every frame so console `set` is live before this frame's testBatch().
    if (occl_.occluder_) occl_.occluder_->setDebugForceWaitIdle(editor::consoleOcclusionForceWaitIdleSlot());
    // occlusion.showCulled/occlusion.cullUnderSuppression read every frame (live for this frame's onRender walk).
    occl_.occlusionShowCulled_ = editor::consoleOcclusionShowCulledSlot();
    occl_.occlusionCullUnderSuppression_ = editor::consoleOcclusionCullUnderSuppressionSlot();
}
#endif

// Autosave polling, window title, selection anchor sync and project settings auto-save.
void SandboxApp::pollAutosave(Engine& e, const Timestep& t) {
    // Autosave polling (drained unconditionally every frame, safe since pumpEvents runs before onUpdate).
    maybeAutosave(t.dt);
    maybeAutosavePrefs(t.dt);
    // Refresh window title when built string differs from what's shown.
    refreshWindowTitle(e);
#if AVER_MODULE_SCENE
    // Collapse selection to one if anchor moved without touching the set (see multiStale()).
    multiSyncToAnchor();
#endif
    // Project settings auto-save (like preferences).
    maybeAutosaveProject(t.dt);
}

// Scene census, --save-level, --bake-level-pack, forwarded open requests and dropped files.
void SandboxApp::tickLaunchRequests(const Timestep& t) {
    // Census at frame 2+ (after applyProject's scripting and spawnClassPlacements).
#if AVER_MODULE_SCENE
    if (harness_.sceneCensus_ && !harness_.sceneCensusDone_ && t.frame >= 2) {
        harness_.sceneCensusDone_ = true;
        // playerStart_ excluded (editor synthesises it but shipped game doesn't).
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(
                      world::takeSceneCensus(scene::World::instance(), playerStart_)));
    }
#endif
#if AVER_MODULE_SCENE
    // --save-level <out>: writes the OPEN level to another path (not over levelPath_).
    // Waits for every pending open (--open-level, forwarded launch, unsaved-changes prompt) before acting.
    if (!projSess_.saveLevelTo_.empty() && !harness_.saveLevelDone_ && !projSess_.levelPath_.empty() &&
        drawerUi_.openLevelByName_.empty() && panels_.pendingOpenPath_.empty() && !panels_.pendingOpenPrompt_) {
        harness_.saveLevelDone_ = true;
        if (saveLevel(projSess_.saveLevelTo_))
            AVER_INFO("[Level] --save-level wrote '{}' ({}) to {}",
                      projSess_.levelName_, projSess_.levelPath_, projSess_.saveLevelTo_);
        else
            AVER_ERROR("[Level] --save-level failed for {}", projSess_.saveLevelTo_);
    }
#endif
#if AVER_MODULE_SCENE
    // --bake-level-pack <level>: bakes that level's pack once the project's meshes are in.
    if (!projSess_.bakeLevelPackAsset_.empty() && !harness_.bakeLevelPackDone_ && t.frame >= 2 && panels_.pendingOpenPath_.empty() && !panels_.pendingOpenPrompt_) {
        harness_.bakeLevelPackDone_ = true;
        std::string bakeWhy;
        world::LevelAssetLibrary& lib = level_.assets();
        lib.setLoader(world::LevelAssetLibrary::diskLoader(projSess_.project_.contentDir()));
        world::PackBakeReport rep;
        if (game::bakeLevelPackWithContent(lib, projSess_.bakeLevelPackAsset_, content_, projSess_.project_.contentDir(), projSess_.project_.dir, rep, &bakeWhy))
            AVER_INFO("[LevelPack] --bake-level-pack: {} row(s) in {} group(s), {} kept as entities", rep.rows, rep.groups, rep.kept);
        else
            AVER_ERROR("[LevelPack] --bake-level-pack failed for {}: {}", projSess_.bakeLevelPackAsset_, bakeWhy);
    }
#endif
    // Forwarded open request (guarded as a whole; queue filled only by setOpenRequestHook).
#if AVER_MODULE_SCENE
    if (window_ && window_->hasPendingOpenRequest()) {
        const std::string path = window_->takePendingOpenRequest();
        if (isLevelFile(path.c_str())) {
            // Same funnel as Content Browser (unsaved-changes check and class-placement spawn).
            if (!requestOpenLevel(path, "opened, forwarded from another launch"))
                AVER_WARN("[Sandbox] forwarded level '{}': {}", path, drawerUi_.openLevelError_);
        }
    }
#endif  // AVER_MODULE_SCENE
    // Dropped files from Explorer (drained unconditionally; Window keeps accepting WM_DROPFILES regardless).
    if (window_ && window_->hasPendingDroppedFiles()) {
        const std::vector<std::string> dropped = window_->takePendingDroppedFiles();
#if AVER_WITH_IMGUI
        if (projSess_.project_.valid()) {
            importDroppedFiles(dropped);
        } else {
            AVER_WARN("[Import] {} file(s) dropped with no project open; ignored", dropped.size());
            notifyOutcome(editor::NotifySeverity::Warning, "Nothing to import into",
                          "Open a project before dropping files into the editor.");
        }
#else
        AVER_WARN("[Import] {} file(s) dropped; this build has no Content Browser to import into",
                  dropped.size());
#endif
    }
}

// Command-line actions that fire after a set number of frames.
void SandboxApp::tickAutoFrameCommands(Engine& e, const Timestep& t) {
    // --pt-scene-toggle-on/-off: checked before syncPtSceneView() so the same onUpdate() that flips the flag also acts on it.
    if (harness_.ptSceneToggleOnAutoFrames_  > 0 && --harness_.ptSceneToggleOnAutoFrames_  == 0) rset_.ptSceneViewWantEnabled_ = true;
    if (harness_.ptSceneToggleOffAutoFrames_ > 0 && --harness_.ptSceneToggleOffAutoFrames_ == 0) rset_.ptSceneViewWantEnabled_ = false;
    // --sun-set-at: writes the same setting as Directional Light panel's sliders.
    if (harness_.sunSetAtFrames_ > 0 && --harness_.sunSetAtFrames_ == 0) {
        rset_.sky_.setSunAngles(harness_.sunSetElevDeg_, harness_.sunSetAzimDeg_);
        AVER_INFO("[Sandbox] --sun-set-at: sun moved to elevation {:.1f} deg, azimuth {:.1f} deg",
                  harness_.sunSetElevDeg_, harness_.sunSetAzimDeg_);
    }
    // --sun-sweep: turns the sun every frame until turnsLeft reaches 0.
    if (harness_.sunSweepFrames_ > 0) {
        if (harness_.sunSweepFrames_ > 1) {
            --harness_.sunSweepFrames_;
        } else {
            f32 el = 0.0f, az = 0.0f;
            rset_.sky_.sunAngles(el, az);
            rset_.sky_.setSunAngles(el, az + harness_.sunSweepDeg_);
            if (harness_.sunSweepTurnsLeft_ > 0 && --harness_.sunSweepTurnsLeft_ == 0) {
                harness_.sunSweepFrames_ = 0;
                rset_.sky_.sunAngles(el, az);
                AVER_INFO("[Sandbox] --sun-sweep-frames: the drag let go at frame {}, sun at elevation "
                          "{:.3f} deg, azimuth {:.3f} deg", t.frame, el, az);
            }
        }
    }
#if AVER_MODULE_VOXI
    // --gi-history-reset-at: resets GI reservoir and denoiser history.
    if (harness_.giHistoryResetAtFrames_ > 0 && --harness_.giHistoryResetAtFrames_ == 0) {
        voxi::Renderer::get().requestGiHistoryReset();
        voxi::Renderer::get().requestDenoiserHistoryReset();
        AVER_INFO("[Sandbox] --gi-history-reset-at: GI reservoir and denoiser history reset requested");
    }
#endif
    // Clear render-scale crash cookie after 30 frames (device loss noticed at Present).
    // --shader-source: pick up an HLSL edit without restarting (lazy start).
    if (!shaderSourceDir_.empty()) {
        if (!shaderWatch_.watching()) {
            if (shaderWatch_.start(shaderSourceDir_))
                AVER_INFO("[Sandbox] watching {} for shader edits", shaderSourceDir_);
            else
                shaderSourceDir_.clear();   // start() logged why; do not retry every frame
        } else {
            std::vector<aver::FileEvent> events;
            const bool rescan = shaderWatch_.poll(events);
            if (rescan || !events.empty()) {
                // `evt`, not `e`: onUpdate's own Engine& parameter is named `e`, and this loop
                // variable shadowed it (C4457).
                for (const aver::FileEvent& evt : events)
                    AVER_INFO("[Sandbox] shader file changed: {}", evt.path);
                if (aver::rhi::reloadShaderFiles() > 0)
                    AVER_INFO("[Sandbox] shaders will recompile on the next frame's prePass");
            }
        }
    }
    if (rset_.renderScaleCookieArmed_ && t.frame >= 30) {
        rset_.renderScaleCookieArmed_ = false;
        editor::setPrefBool("display.renderScalePending", false);
        editor::flushEditorPrefs();
    }
#if AVER_MODULE_SR
    // --aversr-cycle: On/Off/On to exercise the transition that used to crash.
    if (harness_.averSrCycleFrames_ > 0 && --harness_.averSrCycleFrames_ == 0) {
        AVER_INFO("[AverSR] --aversr-cycle: Performance -> Off, the transition that used to crash");
        applyAverSrQuality(e.device(), aver::sr::Quality::Performance);
        applyAverSrQuality(e.device(), aver::sr::Quality::Off);
        AVER_INFO("[AverSR] --aversr-cycle: survived the round trip");
    }
#endif
}

// The path tracer's legacy environment switch and the --pt-quality-ramp / --gi-method-cycle drivers.
void SandboxApp::tickPathTracingAutomation() {
    // Path tracer's matched-environment legacy switch (contrast-fix F6/F7, root cause R5).
    if (ptSceneView_) ptSceneView_->setLegacyEnvironment(editor::consolePtLegacyEnvSlot());
#if AVER_MODULE_VOXI
    // --pt-quality-ramp: verification-only, raises PT rung on live view every N frames.
    if (harness_.ptQualityRampEvery_ > 0 && --harness_.ptQualityRampCountdown_ <= 0) {
        harness_.ptQualityRampCountdown_ = harness_.ptQualityRampEvery_;
        voxi::Settings ramped = voxi::Renderer::get().settings();
        const u32 rung = static_cast<u32>(ramped.pathTracing);
        // Off is never raised INTO (the ramp exercises rung changes on a live view).
        if (ramped.pathTracing != voxi::Quality::Off && rung < static_cast<u32>(voxi::Quality::Epic)) {
            ramped.pathTracing = static_cast<voxi::Quality>(rung + 1);
            voxi::Renderer::get().setSettings(ramped);
            AVER_INFO("[PT] --pt-quality-ramp: Path Tracing raised to quality {}",
                      static_cast<u32>(ramped.pathTracing));
        }
    }
    // --gi-method-cycle: verification-only, steps the GI page's Method combo every N frames
    // (cones -> ReSTIR GI -> ReSTIR PT -> reference PT), exactly as a click would; nothing is saved.
    if (harness_.giMethodCycleEvery_ > 0 && --harness_.giMethodCycleCountdown_ <= 0) {
        harness_.giMethodCycleCountdown_ = harness_.giMethodCycleEvery_;
        voxi::Settings m = voxi::Renderer::get().settings();
        const bool ptOn = m.pathTracing != voxi::Quality::Off;
        const int cur  = ptOn ? (m.ptMode == 1u ? 3 : 2) : (m.giMode != 0 ? 1 : 0);
        const int next = (cur + 1) % 4;
        m.giMode = next == 0 ? 0u : 1u;
        if (next < 2) {
            m.pathTracing = voxi::Quality::Off;
        } else {
            m.ptMode = static_cast<u32>(next - 2);
            if (!ptOn) { m.pathTracing = voxi::Quality::High; m.ptBounces = voxi::ladder::ptBounces(m.pathTracing); }
        }
        voxi::Renderer::get().setSettings(m);
        static const char* kNames[] = {"Voxel cones", "ReSTIR GI", "ReSTIR path tracing", "reference path tracing"};
        AVER_INFO("[Sandbox] --gi-method-cycle: Method -> {}", kNames[next]);
    }
    // Rung reconciled on same cadence as registration (idempotent setQuality call every frame).
    if (ptSceneView_) {
        const voxi::Quality q = voxi::Renderer::get().settings().pathTracing;
        if (q != voxi::Quality::Off) ptSceneView_->setQuality(static_cast<u32>(q) - 1);
    }
#endif
}

// One MCP command per frame, then the pointer hold.
#if AVER_MODULE_MCP
void SandboxApp::pumpMcp() {
    // One event per frame (click needs press and release).
    if (mcp_.listening()) mcp_.pump([this](const mcp::Command& c) { applyMcpCommand(c); });
    holdMcpPointer();
}
#endif

// Cloud clock, world-fx tick and the UI demo animation.
void SandboxApp::tickAmbientState(const Timestep& t) {
    if (rset_.sky_.cloudsEnabled) rset_.cloudTime_ += t.dt;
#if AVER_MODULE_FRAMEWORK
    aver_fw_worldfx_tick(t.dt);   // every frame, Play or not: advances the script post blends, and the first tick after Stop drops the session's overrides, layers and scales
#endif
    if (panels_.showUiDemo_) {
        panels_.uiDemoHealth_  = 0.5f + 0.5f * std::sin(panels_.uiDemoClock_ * 0.7f);
        panels_.uiDemoStamina_ = 0.5f + 0.5f * std::sin(panels_.uiDemoClock_ * 1.6f + 1.0f);
        panels_.uiDemoScroll_  = std::fmod(panels_.uiDemoScroll_ + t.dt * 24.0f, 216.0f);   // px, wraps at 12 rows
        panels_.uiDemoClock_  += t.dt;
    }
}

// Re-rasterises the UI font on a DPI change, then updates the play window.
#if AVER_WITH_IMGUI
void SandboxApp::tickDpiAndPlayWindow(Engine& e) {
    if (e.device()->uiActive()) {
        // Must stay outside the ImGui frame (font atlas cannot be touched inside one).
        const f32 dpi = e.window() ? e.window()->dpiScale() : viewState_.dpi_;
        if (std::fabs(dpi - viewState_.dpi_) > 0.01f) {
            AVER_INFO("[Sandbox] DPI changed {:.2f} -> {:.2f}, re-rasterising the UI font", viewState_.dpi_, dpi);
            applyDpi(dpi);
        }
    }
    updatePlayWindow(e);
}
#endif

// Who gets the keyboard and mouse this frame: the editor tools, the game or the UI.
#if AVER_WITH_IMGUI
void SandboxApp::resolveEditorInputOwnership(Engine& e) {
    // Engine's default pawn is an exception: it must not stand down (leaving it out broke PIE feel).
    // One arbitration computed once instead of eleven spellings.
#if AVER_WITH_IMGUI
    // Headless run never calls uiInit(), so CreateContext() never ran and GImGui is null (GetIO() would fault).
    if (e.device()->uiActive()) {
        const ImGuiIO& oio = ImGui::GetIO();
        editor::InputConditions ic;
        ic.uiActive          = e.device()->uiActive();
        ic.browserActive     = browserActive_;
        ic.textInput         = oio.WantTextInput;
        ic.uiWantsKeyboard   = oio.WantCaptureKeyboard;
        ic.uiWantsMouse      = oio.WantCaptureMouse;
        // FALSE WHILE EJECTED: ejecting hands viewport tools back like leaving Play; toolsLive's defaultPawnPlay exception exists to keep spectator pawn flyable pre-eject.
        ic.playing           = playSessionActive() && !playEjected();
        ic.releasedByUser    = releasedByUser_;
        // Left at default without the scene (defaultPawnPlay_ is `#if AVER_MODULE_SCENE`).
#if AVER_MODULE_SCENE
        ic.defaultPawnPlay   = play_.defaultPawnPlay_ && !playEjected();
#endif
        ic.mouseCaptured     = mouse_.captured();
        ic.pointerInViewport = viewState_.levelHovered_ && inViewport(oio.MousePos.x, oio.MousePos.y);
        ic.drawerOpen        = drawerUi_.drawer_ != Drawer::None;
        ic.landscapeMode     = viewState_.mode_ == EditorMode::Landscape;
        ic.pointerOnScene    = viewState_.levelImageHovered_;
        ic.sceneFocused      = viewState_.levelFocused_;
        // ImGui sees only the editor window: the play window's keys and mouse are the game's.
        if (playWindowFocused()) {
            ic.textInput = ic.uiWantsKeyboard = ic.uiWantsMouse = false;
            ic.pointerInViewport = true;
        }
        // --wheel-speed-test overrides stand in for right-drag state (headless has no real cursor).
#if AVER_MODULE_FRAMEWORK
        if (harness_.wheelTestForceFly_) {
            ic.uiWantsKeyboard = false;
            ic.uiWantsMouse = false;
            ic.pointerInViewport = true;
        }
#endif
        own_ = editor::resolveInputOwnership(ic);
    }
#endif
    // Leaving fly mode is unconditional (flying_ used to clear only in narrower gate, so focused text fields left camera stuck flying).
#if AVER_WITH_IMGUI
    // uiActive() first (headless has no ImGui context). Nothing lost skipping it there.
    // The drone's right-drag look reads InputState too (captured mouse, play window), so both must release.
    if (e.device()->uiActive() && !ImGui::GetIO().MouseDown[1] && !input_.mouseHeld(1)) viewState_.flying_ = false;
#endif
}
#endif

// Keyboard and wheel driving of the editor camera or the possessed default pawn.
#if AVER_WITH_IMGUI
void SandboxApp::tickEditorFlyCamera(const Timestep& t) {
    if (own_.keyboardToTool || own_.mouseToTool) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool inPlayWindow = playWindowFocused();
        const bool overUI = !inPlayWindow && (!viewState_.levelHovered_ || !inViewport(io.MousePos.x, io.MousePos.y));
        // The play window's keys arrive through input_ only (ImGui sees the editor window).
        auto flyKey = [&](ImGuiKey k, i32 vk) { return ImGui::IsKeyDown(k) || (inPlayWindow && input_.keyHeld(vk)); };
        if (inputProbe_ && (ImGui::GetFrameCount() % 30) == 0)
            AVER_INFO("[input-probe] mouse=({},{}) wantCaptureMouse={} hovered={} inViewport={} "
                      "focused={} -> overUI={} | flying={} cam=({:.0f},{:.0f},{:.0f}) yaw={:.2f}",
                      (int)io.MousePos.x, (int)io.MousePos.y, (int)io.WantCaptureMouse,
                      (int)viewState_.levelHovered_, (int)inViewport(io.MousePos.x, io.MousePos.y),
                      (int)viewState_.levelFocused_, (int)overUI, (int)viewState_.flying_,
                      viewState_.camPos_.x, viewState_.camPos_.y, viewState_.camPos_.z, viewState_.yaw_);

        // Right mouse enters fly mode; leaving is handled unconditionally above (outside this gate).
        if (ImGui::IsMouseClicked(1) && !overUI) viewState_.flying_ = true;
        // The drone pawn looks while the right button is held, read from InputState: ImGui sees
        // neither a captured mouse's clicks over the viewport reliably nor the play window at all.
#if AVER_MODULE_SCENE
        if (play_.defaultPawnPlay_ && !playEjected() && input_.mouseHeld(1) &&
            (!overUI || inPlayWindow || mouse_.captured()))
            viewState_.flying_ = true;
#endif
        // --wheel-speed-test drives this directly (forces state rather than synthesising right-drag).
#if AVER_MODULE_FRAMEWORK
        if (harness_.wheelTestForceFly_) viewState_.flying_ = true;
#endif
        if (viewState_.flying_) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

        // Wheel changes fly speed, up = faster (Unreal's direction), 1.25x per notch so a notch feels
        // the same at any speed. The flying drone pawn takes it without RMB too: in Play the mouse
        // is the game's, and the drone moves at this same speed.
        auto wheelFlySpeed = [&] {
            const f32 wheel = input_.wheel();
            if (wheel == 0.0f) return;
            viewState_.flySpeed_ *= std::pow(1.25f, wheel);
            viewState_.flySpeed_ = viewState_.flySpeed_ < 20.0f ? 20.0f : (viewState_.flySpeed_ > 40000.0f ? 40000.0f : viewState_.flySpeed_);
        };
#if AVER_MODULE_SCENE
        if (!viewState_.flying_ && play_.defaultPawnPlay_ && !playEjected() && !play_.walkCapsule_ && (!overUI || inPlayWindow))
            wheelFlySpeed();
#endif

        if (viewState_.flying_) {
            // input_.mouseDX/DY not io.MouseDelta: same phase bug as wheel below.
            // ImGui computes io.MouseDelta in NewFrame, but Engine::frameStep runs onUpdate before uiNewFrame.
            // InputState holds THIS frame's motion (fed by pumpEvents before frameStep).
            // CAPTURED (Play): the cursor is re-centred every frame, and that warp arrives as a
            // WM_MOUSEMOVE straight back, so input_'s delta nets to zero -- the view turned, then
            // snapped back. MouseCapture measures from its anchor instead (last frame's poll).
            const bool cap = mouse_.captured();
            const f32 mdx = cap ? mouse_.dx() : static_cast<f32>(input_.mouseDX());
            const f32 mdy = cap ? mouse_.dy() : static_cast<f32>(input_.mouseDY());
            viewState_.yaw_   += mdx * viewState_.lookSpeed_;
            viewState_.pitch_ -= mdy * viewState_.lookSpeed_;
            viewState_.pitch_ = viewState_.pitch_ < -1.54f ? -1.54f : (viewState_.pitch_ > 1.54f ? 1.54f : viewState_.pitch_);
            wheelFlySpeed();
        }

        const Vec3 fwd = camForward();
        const Vec3 up{0, 0, 1};
        const Vec3 right = cross(up, fwd).getSafeNormal();

        // Spectator Play flies without holding right button (mouse look still wants it).
        // Synthetic look injection point matches real look (--pie-camera-test).
#if AVER_MODULE_FRAMEWORK
        if (harness_.pieCamPendingLook_) {
            harness_.pieCamPendingLook_ = false;
            viewState_.yaw_   += 0.5f;
            viewState_.pitch_ += 0.3f;
            harness_.pieCamWantYaw_ = viewState_.yaw_; harness_.pieCamWantPitch_ = viewState_.pitch_;
        }
#endif
        // defaultPawnPlay_ is scene-guarded. Without the module, camera flies on right button alone.
        // EJECTED DROPS THE EXCEPTION: spectator pawn stops flying without RMB, block below moves camPos_ instead.
        bool flyPawnDriven = false;
#if AVER_MODULE_SCENE
        if ((viewState_.flying_ || (play_.defaultPawnPlay_ && !playEjected())) && (!io.WantCaptureKeyboard || inPlayWindow)) {
#else
        if (viewState_.flying_ && (!io.WantCaptureKeyboard || inPlayWindow)) {
#endif
            const f32 sp = viewState_.flySpeed_ * t.dt;
            // The possessed default pawn also takes the game's own input (AVER_FW_KEY_A.. is A-Z in
            // order), like the walking one, so --play-test and a game's input mapping can fly it.
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
            const bool pawnInput = play_.defaultPawnPlay_ && !playEjected();
            auto moveKey = [&](ImGuiKey k, i32 vk) {
                return flyKey(k, vk) || (pawnInput && aver_fw_input_key(vk - 'A') != 0);
            };
#else
            auto moveKey = flyKey;
#endif
            Vec3 step{0, 0, 0};
            if (moveKey(ImGuiKey_W, 'W')) step += fwd * sp;
            if (moveKey(ImGuiKey_S, 'S')) step -= fwd * sp;
            if (moveKey(ImGuiKey_D, 'D')) step += right * sp;
            if (moveKey(ImGuiKey_A, 'A')) step -= right * sp;
            if (moveKey(ImGuiKey_E, 'E')) step += up * sp;
            if (moveKey(ImGuiKey_Q, 'Q')) step -= up * sp;
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
            // Move the PAWN (not camera) while default pawn is possessed and not ejected (drivePlayCamera rewrites view every frame).
            if (play_.defaultPawnPlay_ && !playEjected() && play_.walkCapsule_) {
                driveDefaultPawnWalk(fwd, right);    // the walking default pawn (Play options > Walk)
            } else if (play_.defaultPawnPlay_ && !playEjected() && play_.flyCapsule_) {
                driveDefaultPawnFly(t.dt > 0.0f ? step * (1.0f / t.dt) : Vec3{0, 0, 0}, t.dt);   // collides, drifts
                flyPawnDriven = true;
            } else if (play_.defaultPawnPlay_ && !playEjected()) {
                const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
                if (pn) {
                    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
                    scene::World& pw = scene::World::instance();
                    if (pw.valid(pe)) {
                        const Transform& cur = pw.localTransform(pe);
                        pw.setLocalPosition(pe, cur.position + step);
                        // Yaw about world up, pitch about pawn's right (same as viewport camera).
                        const Quat qy = Quat::fromAxisAngle(Vec3{0, 0, 1}, viewState_.yaw_);
                        const Quat qp = Quat::fromAxisAngle(Vec3{0, 1, 0}, -viewState_.pitch_);
                        pw.setLocalRotation(pe, qy * qp);
                    }
                }
            } else
#endif
            viewState_.camPos_ += step;
        } else if (!overUI) {
            // Scroll-to-dolly (uses input_.wheel() not io.MouseWheel, which is always zero here).
            const f32 wheel = input_.wheel();
            if (wheel != 0.0f) viewState_.camPos_ += fwd * wheel * (viewState_.flySpeed_ * 0.15f);
            // MMB pan (uses input_.mouseDX/DY, not frame-old delta).
            if (io.MouseDown[2]) {
                viewState_.camPos_ -= right * static_cast<f32>(input_.mouseDX()) * 0.02f;
                viewState_.camPos_ += up    * static_cast<f32>(input_.mouseDY()) * 0.02f;
            }
        }
        // Nothing drove the flying pawn: it glides to rest (the keyboard is the UI's for a moment), or
        // stops dead while ejected, where it must stay where Pawn to Camera puts it.
        if (play_.flyCapsule_ && !flyPawnDriven) driveDefaultPawnFly(Vec3{0, 0, 0}, playEjected() ? 0.0f : t.dt);
        // Ctrl+S saves the level (the File menu's label for this shortcut wires nothing).
        // Not gated on levelFocused_ (saving isn't a viewport gesture) but gated on WantTextInput.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::LevelSave, io))
            saveLevelInteractive();

        // Ctrl+Shift+S saves everything (level plus every dirty asset tab). Same site as LevelSave.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::SaveAll, io))
            saveAll();

        // Ctrl+Shift+B / Ctrl+Shift+R mirror the Tools menu's Compile/Reload Scripts items. Same site.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::CompileScripts, io))
            tools_.requestCompileScripts(projSess_.project_);
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::ReloadScripts, io))
            tools_.requestReloadScripts(projSess_.project_);

        // F frames the selection at a distance derived from its radius.
        // Outliner/Details count too (levelFocused_ is false when those panels have focus).
        // Scene-guarded: F frames a SELECTED ENTITY's bounds (selectionBounds is `#if AVER_MODULE_SCENE`).
        // STANDS DOWN FOR PAWN TO CAMERA: Shift+F in ejected session both teleports pawn and frames the camera.
#if AVER_MODULE_SCENE
        if ((viewState_.levelFocused_ || viewState_.outlinerFocused_ || viewState_.detailsFocused_) && !io.WantCaptureKeyboard &&
            keybinds_.pressed(editor::CommandId::ViewFrameSelected, io) && anySelected() &&
            !(playEjected() && hasPossessedPawn() &&
              keybinds_.held(editor::CommandId::PlayPawnToCamera, io))) {
            // selectionBounds: union of whole selection (not just anchor, which put multi-selection outside view).
            Vec3 center; f32 r;
            if (selectionBounds(center, r)) {
                const f32 d = std::fmax(50.0f, r / std::tan(radians(30.0f)) * 1.6f);
                viewState_.camPos_ = center - fwd * d;
                viewState_.flySpeed_ = std::fmax(viewState_.flySpeed_, r * 0.4f);
                // Teleport (see frameCameraOnLevel for why streaming_.resetVelocityTracking() is needed).
                streaming_.resetVelocityTracking();
            }
        }
#endif  // AVER_MODULE_SCENE
    }
}
#endif

// Click-to-capture of the mouse for the play viewport.
#if AVER_MODULE_FRAMEWORK
void SandboxApp::tickPlayMouseCapture(Engine& e) {
    // --recapture-test opts in (gesture lives entirely inside this block).
    const bool interactive = (maxFrames_ == 0 && !harness_.playTest_) || harness_.recapFrames_ > 0;
    // The #if isn't redundant with uiActive(): runtime flag doesn't make ImGuiIO names COMPILE on UI-less build.
#if AVER_WITH_IMGUI
    if (e.device()->uiActive() && interactive) {
        // Clicking viewport puts mouse back in game (tested before wantCapture below).
        {
            // levelHovered_ asks if pointer is over the LEVEL window (true precondition for capture).
            const ImGuiIO& mio = ImGui::GetIO();
            // Not while ejected (that click is a viewport selection, mouse stays free).
            if (playSessionActive() && !playEjected() && releasedByUser_ && ImGui::IsMouseClicked(0) &&
                viewState_.levelHovered_ && inViewport(mio.MousePos.x, mio.MousePos.y)) {
                releasedByUser_ = false;
                // Click means "give the mouse back" not a trigger pull. Eaten until button-up (see pushInput).
                eatRecaptureClick_ = true;
            }
        }
        // The play window's chords come through input_ (Win32 virtual keys): Esc stops, Shift+F1
        // releases the mouse, a click takes it back.
        if (playWindowFocused()) {
            if (input_.keyPressed(0x1B /*VK_ESCAPE*/)) { stopPlay(); }
            else if (playSessionActive() && !playEjected()) {
                if (input_.keyHeld(0x10 /*VK_SHIFT*/) && input_.keyPressed(0x70 /*VK_F1*/))
                    releasedByUser_ = !releasedByUser_;
                else if (releasedByUser_ && input_.mousePressed(0)) {
                    releasedByUser_ = false;
                    eatRecaptureClick_ = true;
                }
            }
        }
        // A game-UI menu that wants the cursor frees it, exactly like releasing the mouse by hand. So does the game's own pointer request,
        // as in the shipped game (GameApp): a free cursor asked for, or a pointer mode other than AUTO and CAPTURED. Play knows only
        // captured or free, so the hidden and confined modes are free here.
        const bool cursorFreed = aver_fw_cursor_requested() != 0 || game::uiWantsCursor();
        const bool wantCapture = playSessionActive() && !releasedByUser_ && !playEjected() &&
                                 game::resolvePointerPlan(aver_fw_pointer_mode(), true, cursorFreed).captured;
        // Alt+P/Alt+S start Play (same anyPlayActive() precondition the toolbar Play button uses).
        if (!ImGui::GetIO().WantTextInput && !anyPlayActive() &&
            keybinds_.pressed(editor::CommandId::PlayStart, ImGui::GetIO()))
            launchPlay(e, PlayMode::SelectedViewport);
        if (!ImGui::GetIO().WantTextInput && !anyPlayActive() &&
            keybinds_.pressed(editor::CommandId::PlaySimulate, ImGui::GetIO()))
            launchPlay(e, PlayMode::Simulate);
        if (keybinds_.pressed(editor::CommandId::PlayReleaseMouse, ImGui::GetIO()) && playSessionActive() && !playEjected())
            releasedByUser_ = !releasedByUser_;
        // F8: possess while ejected, eject while possessed (one chord, gated on real session).
        if (keybinds_.pressed(editor::CommandId::PlayEject, ImGui::GetIO()) && playSessionActive())
            togglePlayEject();
        // Shift+F (ejected only): pawn comes to editor camera, then F8 possesses it there.
        // GATED LIKE FRAME SELECTED (level viewport, Outliner, Details), not asset tabs or with WantTextInput.
        {
            const ImGuiIO& pio = ImGui::GetIO();
            if ((viewState_.levelFocused_ || viewState_.levelHovered_ || viewState_.outlinerFocused_ || viewState_.detailsFocused_) &&
                !pio.WantTextInput && !pio.WantCaptureKeyboard && playEjected() &&
                keybinds_.pressed(editor::CommandId::PlayPawnToCamera, pio) &&
                // Only warn about missing pawn if nothing else answered (Frame Selected shares this press).
                (hasPossessedPawn() || !anySelected()))
                teleportPawnToCamera();
        }
        if (keybinds_.pressed(editor::CommandId::PlayPause, ImGui::GetIO()) && playSessionActive())
            aver_fw_set_paused(aver_fw_play_state() != AVER_FW_PLAY_PAUSED ? 1 : 0);
        if (keybinds_.pressed(editor::CommandId::PlayFrameSkip, ImGui::GetIO()))
            requestPlayFrameStep();
        // Escape stops Play-in-Editor (same as Stop). Checked here, not pushInput, so this wins over game seeing the keypress.
        // Escape ends a drone stand-in too (Play started it, Play's exit ends it).
        if (keybinds_.pressed(editor::CommandId::PlayStop, ImGui::GetIO()) && (playSessionActive() || dronePlayActive() || spectatorPlayActive()))
            stopPlay();
        // F9 screenshots viewport (edit or Play). Checked here, not beside LevelSave: covers both modes with one check.
        // (Not while a graph canvas has focus: F9 there is the debugger's breakpoint.)
        if (!ImGui::GetIO().WantTextInput && !editor::graphCanvasHasFocus() &&
            keybinds_.pressed(editor::CommandId::Screenshot, ImGui::GetIO()))
            requestViewportScreenshot();
        if (!playSessionActive()) releasedByUser_ = false;
        setMouseCaptured(wantCapture && !ImGui::GetIO().WantTextInput);
    }
#else
    (void)interactive;   // no ImGui to arbitrate mouse/keyboard capture with
#endif
}
#endif

// --hud-preview: names every HUD once, then previews one over the level viewport.
#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_SCRIPTING
void SandboxApp::reportHudPreview() {
    // UI frame opens before gameplay ticks (ticking is when a game draws its HUD).
    // --hud-preview <n>: name every HUD once, then preview one over the level viewport.
    if (harness_.hudTest_ >= 0 && !harness_.hudTestReported_ && scripts_.ready()) {
        harness_.hudTestReported_ = true;
        const i32 n = scripts_.hudCount();
        AVER_INFO("[HUD] {} declared", n);
        for (i32 i = 0; i < n; ++i) AVER_INFO("[HUD]   {}: '{}'", i, scripts_.hudName(i));
        if (harness_.hudTest_ >= n) AVER_WARN("[HUD] no HUD at index {}", harness_.hudTest_);
    }
    if (harness_.hudTest_ >= 0 && harness_.hudTest_ < scripts_.hudCount())
        setHudPreview(harness_.hudTest_, viewState_.vpX_, viewState_.vpY_, viewState_.vpW_, viewState_.vpH_);
}
#endif
#endif

// The window size, scale and focus a game reads during Play.
#if AVER_MODULE_FRAMEWORK
void SandboxApp::publishWindowState(Engine& e) {
    // What a game reads as its window during Play: the level viewport's size, the display's scale and the focus. Published only (the editor owns its
    // window and takes no requests); the shipped game publishes its real window (GameDisplay.cpp). Before this a game read zeros: no size, a scale of 1.
    {
        i32 s[AVER_FW_WINDOW_STATE_COUNT] = {};
        s[AVER_FW_WINDOW_WIDTH] = static_cast<i32>(viewState_.vpW_);
        s[AVER_FW_WINDOW_HEIGHT] = static_cast<i32>(viewState_.vpH_);
        const f32 dpi = e.window() && e.window()->dpiScale() > 0.0f ? e.window()->dpiScale() : 1.0f;
        s[AVER_FW_WINDOW_DPI_PERCENT] = static_cast<i32>(dpi * 100.0f + 0.5f);
        s[AVER_FW_WINDOW_FOCUSED] = viewState_.levelFocused_ ? 1 : 0;
        aver_fw_window_publish(s);
    }
}
#endif

// Sub-viewports shown over the window, first in the UI frame's list.
#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
void SandboxApp::submitSubViewports() {
    // Sub-viewports shown over the window (G4 R5): first in the frame's list, so the HUD draws over them; the rectangle is in the UI frame's own space.
    {
        game::SubViewportHost::OnScreenView sv[16];
        const u32 svCount = subViews_.onScreen(sv, 16);
        float uiRect[4] = {0, 0, 0, 0};
        if (svCount) aver_ui_viewport(uiRect);
        for (u32 i = 0; i < svCount; ++i)
            aver_ui_textured_rect(uiRect[0] + sv[i].x, uiRect[1] + sv[i].y, sv[i].w, sv[i].h, aver::render::ui::UiRenderer::textureId(sv[i].texture),
                                  0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
    }
}
#endif
#endif

// Pointer position and buttons for the game UI, converted to the UI frame's space.
#if AVER_MODULE_FRAMEWORK
void SandboxApp::feedUiPointer() {
    // Pointer converted here (only place that can do it correctly): UI frame is laid out against viewport, not window.
    {
        const f32 ox = hudPreviewActive() ? harness_.hudRectX_ : static_cast<f32>(viewState_.vpX_);
        const f32 oy = hudPreviewActive() ? harness_.hudRectY_ : static_cast<f32>(viewState_.vpY_);
        f32 px = 0.0f, py = 0.0f;
        u32 buttons = 0;
#if AVER_WITH_IMGUI
        // No ImGui context when headless (--headless): GetIO() would read through null.
        if (ImGui::GetCurrentContext()) {
            const ImGuiIO& uiIo = ImGui::GetIO();
            px = uiIo.MousePos.x; py = uiIo.MousePos.y;
            // ImGui reports outside cursor as -FLT_MAX; pushed further negative to hit nothing.
            if (px < -1.0e6f || py < -1.0e6f) { px = -1.0e6f; py = -1.0e6f; }
            else { px -= ox; py -= oy; }
            for (int b = 0; b < 3; ++b)
                if (ImGui::IsMouseDown(static_cast<ImGuiMouseButton>(b))) buttons |= (1u << b);
        } else {
            px = py = -1.0e6f;
        }
        // In the play window the HUD is laid out at its origin, and only input_ sees its pointer.
        if (playWindowFocused() && !hudPreviewActive()) {
            px = static_cast<f32>(input_.mouseX());
            py = static_cast<f32>(input_.mouseY());
            buttons = 0;
            for (int b = 0; b < 3; ++b)
                if (input_.mouseHeld(b)) buttons |= (1u << b);
        }
#else
        (void)ox; (void)oy;
#endif
        aver_ui_set_pointer(px, py, buttons);
    }
}
#endif

// Accessibility, UI input, retained widgets and the self-test drivers.
#if AVER_MODULE_FRAMEWORK
void SandboxApp::tickGameUi(Engine& e, const Timestep& t) {
    // Retained game-UI widgets: Play feeds them this frame's keyboard/pad/wheel; the tree itself lays out,
    // updates and draws every frame (it is empty unless a script or graph made a widget).
#if AVER_MODULE_FRAMEWORK
    // Accessibility: options, captions and narration for this frame (unscaled time), pushed to the UI before it lays out.
    game::accessibilityFrame(t.dt, viewPos(), viewState_.yaw_, viewState_.pitch_, access_);
#endif
    game::pushAccessibilityToUi(access_);
    if (gameUi_) gameUi_->setColourVision(access_.snap.colourVisionPacked);
    if (playSessionActive()) game::uiFeedInput(input_, t.dt, !mouse_.captured());
    aver_ui_widgets_frame(t.dt);
    maybeSpawnTestActor();
    maybePlayTest(e);
    maybePieCameraTest();
    maybeInputStuckTest();
    maybeInputSourceTest();
    maybeWheelSpeedTest();
    maybeRecaptureTest();
    maybeViewmodelTest();
}
#endif

// The frame order: OnPhysicsTick before each fixed step, then OnTick; level travel.
#if AVER_MODULE_FRAMEWORK
void SandboxApp::tickGameplay(Engine& e, const Timestep& t, const f32 gameDt) {
    // The frame order: OnPhysicsTick before each fixed step, then OnTick (GameTick.hpp).
    if (!harness_.spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
        // Level's cars: each driver sets input before physics step, each entity written after.
        // Inside this gate so Pause and Frame Skip hold traffic with everything else.
        // Focus is the view (pawn's eye while possessed, free camera while ejected): cars far from it think less often.
        playProf_.begin(editor::PlayPhase::Vehicles);
        vehicles_.prePhysics(gameDt, viewState_.camPos_);

        game::tickVehiclesPre(gameDt, viewState_.camPos_);
        playProf_.end(editor::PlayPhase::Vehicles);
#endif
        playProf_.begin(editor::PlayPhase::Gameplay);
        const auto gameplayStart = std::chrono::steady_clock::now();
        playProf_.addPhysicsSteps(game::tickGameplayGroups(t.dt));
        reportSlowGameplay(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - gameplayStart).count());
        playProf_.end(editor::PlayPhase::Gameplay);
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
        playProf_.begin(editor::PlayPhase::Vehicles);
        vehicles_.postPhysics(scene::World::instance());

        game::tickVehiclesPost(gameDt, play_.playMobility_);
        playProf_.end(editor::PlayPhase::Vehicles);
#endif
    } else if (aver_fw_play_state() == AVER_FW_PLAY_PAUSED) {
        game::tickPausedFrame(t.dt);
    }
    // Level travel: the session goes on in the requested level, and Stop brings the edited one back (travelDuringPlay).
    {
        char travel[1024];
        if (aver_fw_take_travel_request(travel, static_cast<int32_t>(sizeof travel)) > 0) travelDuringPlay(e, travel);
    }
    game::pumpContentPrefetch(content_);   // a script's "this is about to be shown": its textures start decoding now
}
#endif

// Water simulation, after the physics step and before any prePass.
#if AVER_MODULE_FLUIDS
void SandboxApp::tickWater(Engine& e, HitchMarks& hm, const f32 gameDt) {
    // After physics step, before any prePass (not gated on Play).
    // The path-traced reference traces the displaced water only while it holds still.
    water_.setFrozen(ptSceneView_ != nullptr);
    if (ptSceneView_)
        ptSceneView_->setIncludeDynamicMeshes(water_.displacedSurfaceActive(), water_.displacedSurfaceRevision());
    water_.update(*e.device(), gameDt);
    hm.mark("water");
}
#endif

// Sequence pre-phase, animation graphs, ragdoll and the animation clock.
#if AVER_MODULE_SCENE
void SandboxApp::tickObjectAnimation(Engine& e, HitchMarks& hm, const f32 gameDt) {
    // Retires deferred destroys and propagates world matrices once, after gameplay and before onRender.
    // Animation clock runs UNCONDITIONALLY (not gated on PLAYING; hanging it there would freeze previews outside Play mode).
#if AVER_MODULE_FRAMEWORK
    // Object animation follows Play's pause: held while PAUSED, lifted by Frame Skip.
    // Set every frame from play state so Stop or level change never leaves it stuck.
    anim::animSystem().setObjectAnimationPaused(aver_fw_play_state() == AVER_FW_PLAY_PAUSED);
#endif
    // [wave5/sequencer] The sequence's Pre phase first: a pose chosen by the render queue, the clock, and the
    // property / visibility / animation / audio / event tracks, so a driven montage lands in the tick below.
    tickSequencePre(e, gameDt);
    playProf_.begin(editor::PlayPhase::ObjectAnim);
    game::tickAnimGraphs(gameDt);   // state machines write the pose the clip sampler would
#if AVER_MODULE_RAGDOLL
    ragdoll::ragdollSystem().tick(gameDt);   // after the physics step, before the pose it writes into
#endif
    anim::animSystem().tick(scene::World::instance(), gameDt);
    playProf_.end(editor::PlayPhase::ObjectAnim);
    hm.mark("objectAnim");
}
#endif

// Soft-body tick, after the pose it reads.
#if AVER_MODULE_SCENE
#if AVER_MODULE_SOFTBODY_SCENE
void SandboxApp::tickSoftBodySim(HitchMarks& hm, const f32 gameDt) {
    // After the pose it reads (skin palette), before the draw scene copies the result (plan 2.5.2).
    playProf_.begin(editor::PlayPhase::SoftBody);
    game::tickSoftBodies(gameDt, viewPos());
    playProf_.end(editor::PlayPhase::SoftBody);
    hm.mark("softBodies");
}
#endif
#endif

// Animated placements' kinematic bodies follow the tick that moved them.
#if AVER_MODULE_SCENE
#if AVER_MODULE_PHYSICS
void SandboxApp::tickDriveBodies(HitchMarks& hm, const f32 gameDt) {
    // After the tick that moved them: animated placement's kinematic body follows it (carries standing characters).
    playProf_.begin(editor::PlayPhase::DriveBodies);
    driveAnimatedBodies(gameDt);
    playProf_.end(editor::PlayPhase::DriveBodies);
    hm.mark("driveBodies");
}
#endif
#endif

// Particle system tick, previews included.
#if AVER_MODULE_SCENE
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
void SandboxApp::tickParticles(const f32 gameDt) {
    // Unconditionally (like animation clock): previews outside Play should still show effects.
    {
        const auto tickStart = std::chrono::steady_clock::now();
        playProf_.begin(editor::PlayPhase::Particles);
        particles::particleSystem().tick(scene::World::instance(), gameDt);
        playProf_.end(editor::PlayPhase::Particles);
#if AVER_MODULE_FRAMEWORK
        for (scene::Entity fe : particles::particleSystem().finished())
            aver_fw_event_post("Particles.EffectFinished", static_cast<int32_t>(fe), static_cast<int32_t>(fe));
#endif
        particles::particleSystem().finished().clear();
        particleTickAccumSec_ += std::chrono::duration<f64>(std::chrono::steady_clock::now() - tickStart).count();
        ++particleTickFrames_;
    }
}
#endif
#endif

// Skinned, soft-body and hair scenes, after the tick and before anything draws.
#if AVER_MODULE_SCENE
void SandboxApp::updateRenderScenes(Engine& e, HitchMarks& hm) {
    // After the tick, before anything draws: update() creates per-entity skin targets and copies matrices out of AnimSystem.
    playProf_.begin(editor::PlayPhase::Skinned);
    if (skinnedScene_)
        skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());
    playProf_.end(editor::PlayPhase::Skinned);
    hm.mark("skinned");
#if AVER_MODULE_RENDER_SOFTBODY
    // After physics and World::flush, before draw (drawHandle() must exist).
    if (softBodyScene_) softBodyScene_->update(scene::World::instance(), *e.device());
#endif
#if AVER_MODULE_RENDER_HAIR
    // Strand hair: HairSystem ticked inside game::tickSoftBodies (GameSystemsWiring.hpp); this reconciles the GPU side with what it published.
    if (hairScene_) hairScene_->update(scene::World::instance(), *e.device());
#endif
}
#endif

// Thumbnail requests and the frame-countdown test hooks.
#if AVER_MODULE_SCENE
void SandboxApp::tickThumbnailsAndTestHooks(Engine& e) {
    // Once per frame, before render features run (ThumbnailCache.hpp): update() points preview at next pending request.
    if (thumbnails_.ready()) {
        thumbnails_.setDiskCache(projSess_.project_.valid() ? (std::filesystem::path(projSess_.project_.dir) / "Saved" / "ThumbnailCache").string() : std::string(),
                                 static_cast<u64>(std::max(0, drawerUi_.thumbDiskCapMB_)));
        thumbnails_.update();
    }
    // --drone: switches drone on N frames in (mirroring --chunk-stream so a capture can prove it without clicking Window > Drone).
    if (harness_.droneAutoFrames_ > 0 && --harness_.droneAutoFrames_ == 0) setDroneEnabled(true);
    // --undo-test: fires runUndoTest() N frames in. Lives inside `#if AVER_WITH_IMGUI` block (same as commands it proves).
#if AVER_WITH_IMGUI
    if (harness_.undoTestAutoFrames_ > 0 && --harness_.undoTestAutoFrames_ == 0) runUndoTest(e);
    if (harness_.classPlaceTestAutoFrames_ > 0 && --harness_.classPlaceTestAutoFrames_ == 0) runClassPlaceTest(e);
    if (harness_.multiSelTestFrames_ > 0 && --harness_.multiSelTestFrames_ == 0) runMultiSelectTest(e);
    if (harness_.cbMoveTestFrames_ > 0 && --harness_.cbMoveTestFrames_ == 0) runCbMoveTest(harness_.cbMoveTestDir_);
    if (harness_.saveDirtyTestFrames_ > 0 && --harness_.saveDirtyTestFrames_ == 0) runSaveDirtyTest();
    if (harness_.findRefsFrames_ > 0 && --harness_.findRefsFrames_ == 0) runFindRefs();
    if (harness_.projectSwitchFrames_ > 0 && --harness_.projectSwitchFrames_ == 0) runProjectSwitchTest();
    if (harness_.validateGraphFrames_ > 0 && --harness_.validateGraphFrames_ == 0) runValidateGraph();
    if (harness_.graphPrintTestFrames_ > 0 && --harness_.graphPrintTestFrames_ == 0) runGraphPrintTest();
    if (harness_.clearShaderCacheFrames_ > 0 && --harness_.clearShaderCacheFrames_ == 0) runClearShaderCache();
    if (harness_.assetAssignTestFrames_ > 0 && --harness_.assetAssignTestFrames_ == 0) runAssetAssignTest();
    if (harness_.graphHitsTestFrames_ > 0 && --harness_.graphHitsTestFrames_ == 0) runGraphHitsTest();
    if (harness_.renameRepointFrames_ > 0 && --harness_.renameRepointFrames_ == 0) runRenameRepointTest();
    if (harness_.prefsWriteTestFrames_ > 0 && --harness_.prefsWriteTestFrames_ == 0) runPrefsWriteTest();
    if (harness_.notifyTestFrames_ > 0 && --harness_.notifyTestFrames_ == 0) runNotifyTest();
    // Armed once: markLevelUnsaved gives autosave timer something to do; re-marking would loop forever.
    if (harness_.autosaveTestArm_) { harness_.autosaveTestArm_ = false; markLevelUnsaved(); }
    if (harness_.keybindTestAutoFrames_ > 0 && --harness_.keybindTestAutoFrames_ == 0) runKeybindPersistTest(harness_.keybindTestMode_);
#endif
}
#endif

// The drone graph and the Play graph ticks.
#if AVER_MODULE_SCENE
#if AVER_MODULE_SCRIPTING
void SandboxApp::tickGraphs(const Timestep& t, HitchMarks& hm, const f32 gameDt) {
    // Ticks the graph-driven drone if live. Runs before chunk streaming so it sees this frame's drone position/velocity.
    if (drone_.droneEntity_ != scene::kInvalidEntity && drone_.droneGraphLoaded_) {
        drone_.droneTimeSeconds_ += t.dt;
        scripts_.graphTick(static_cast<i32>(drone_.droneEntity_), drone_.droneTimeSeconds_);
        scene::World& dw = scene::World::instance();
        if (dw.valid(drone_.droneEntity_)) {
            drone_.dronePos_ = dw.localTransform(drone_.droneEntity_).position;
            const f32 droneInvDt = t.dt > 1e-6f ? 1.0f / t.dt : 0.0f;
            drone_.droneVel_ = drone_.droneHaveLastPos_ ? (drone_.dronePos_ - drone_.droneLastPos_) * droneInvDt
                                          : Vec3{0.0f, 0.0f, 0.0f};
            drone_.droneLastPos_ = drone_.dronePos_;
            drone_.droneHaveLastPos_ = true;
            // Greppable proof the drone MOVED across several frames (not just spawned).
            if (drone_.droneLogsLeft_ > 0) {
                --drone_.droneLogsLeft_;
                AVER_INFO("[Drone] t={:.3f}s pos=({:.1f},{:.1f},{:.1f}) vel=({:.1f},{:.1f},{:.1f})cm/s",
                          drone_.droneTimeSeconds_, drone_.dronePos_.x, drone_.dronePos_.y, drone_.dronePos_.z,
                          drone_.droneVel_.x, drone_.droneVel_.y, drone_.droneVel_.z);
            }
        }
    }
    // Graph-as-class instances gated on Play. HostBridge registers graph classes into native class registry.
    // Ungated: every class-placed graph ran OnTick while browsing, mutating level. Need FRAMEWORK guard for aver_fw_play_state().
#if AVER_MODULE_FRAMEWORK
    playProf_.begin(editor::PlayPhase::GraphTicks);
    if (!harness_.spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING)
        scripts_.tickGraphClassInstances(gameDt);
    playProf_.end(editor::PlayPhase::GraphTicks);
    hm.mark("graphTicks");
#endif
}
#endif
#endif

// Chunk and level streaming.
#if AVER_MODULE_SCENE
void SandboxApp::tickChunkAndLevelStreaming(const Timestep& t, HitchMarks& hm) {
    // --chunk-stream: switches streaming on N frames in so a capture can prove it happened.
    if (harness_.chunkStreamAutoFrames_ > 0 && --harness_.chunkStreamAutoFrames_ == 0) setChunkStreamingEnabled(true);
    // Chunk streaming. Runs here so it sees THIS frame's camPos_ and evictions land in flush() below.
    // Runs while idling outside Play too (exactly who this feature is for).
    if (streaming_.enabled()) {
        const game::GameStreaming::TriangleLookupFn tris = [this](u64 id) -> u32 {
            const auto it = lvl_.meshTris_.find(id);
            return it != lvl_.meshTris_.end() ? it->second : 0u;
        };
#if AVER_MODULE_SCRIPTING
        // Drone keeps its own corridor resident while flying (both are StreamSource entries).
        if (drone_.droneEntity_ != scene::kInvalidEntity && drone_.droneGraphLoaded_) {
            const world::StreamSource drone{drone_.dronePos_, drone_.droneVel_};
            streaming_.tick(viewState_.camPos_, t.dt, &drone, tris);
        } else
#endif
        streaming_.tick(viewState_.camPos_, t.dt, nullptr, tris);
    }
#if AVER_MODULE_SCENE
    tickLevelStreaming(t.dt);
    hm.mark("levelStreaming");
#endif
}
#endif

// Play camera, sequence camera and camera-rig look-through for this frame.
void SandboxApp::resolveViewOverride(const Timestep& t) {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    // Ejected: leave camPos_/yaw_/pitch_ where the user flew them (drivePlayCamera snaps view to pawn every frame) and clear owner-hide root.
    // Re-possessing calls drivePlayCamera() again next frame.
    if (playEjected()) { firstPersonPawn_ = scene::kInvalidEntity; playView_ = game::PlayView{}; }
    else {
        drivePlayCamera(t.dt);
#if AVER_MODULE_FRAMEWORK
        if (playSessionActive()) game::applyCameraFx(viewState_.camPos_, viewState_.yaw_, viewState_.pitch_, t.dt);   // the player's shake/bob scales, Play camera only (never the free-fly camera)
#endif
    }
#endif
    // A sequence camera (Animate's pilot view, or Play's camera track) replaces the view for this
    // frame only: camPos_/yaw_/pitch_ are the persisted editor camera and stay untouched.
    viewState_.viewOverride_ = false;
#if AVER_MODULE_SCENE
    {
        game::SeqCameraPose sp;
        if (seqEditor_.viewPose(playEjected(), sp)) {
            viewState_.viewOverride_ = true;
            viewState_.viewPosOv_ = sp.position;
            viewState_.viewFwdOv_ = game::cameraForward(sp.yaw, sp.pitch);
            // The pawn is in view now, not behind the eye.
            if (seqEditor_.playRunning()) firstPersonPawn_ = scene::kInvalidEntity;
        }
    }
#endif

#if AVER_MODULE_CAMERA && AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    // Details > Camera Rig > Look through: the viewport shows the rig's stateless preview pose (no lag, no blend, no modifiers) until Esc.
    if (dbgOv_.cameraLookRig_ != scene::kInvalidEntity) {
        scene::World& lookWorld = scene::World::instance();
        if (!lookWorld.valid(dbgOv_.cameraLookRig_)) {
            dbgOv_.cameraLookRig_ = scene::kInvalidEntity;
        } else {
            const camera::RigResult look = camera::cameraRigSystem().preview(lookWorld, dbgOv_.cameraLookRig_, camera::RigFrame{});
            if (look.active) {
                viewState_.viewOverride_ = true;
                viewState_.viewPosOv_ = look.pov.pos;
                viewState_.viewFwdOv_ = game::cameraForward(look.pov.yaw, look.pov.pitch);
            }
        }
    }
#endif
}

// G-buffer, mesh-shader and Voxi settings pushed to the device.
void SandboxApp::applyDeviceRenderSettings(Engine& e, const Timestep& t) {
    // G-buffer: pushed every frame so live dropdown click or --gbuffer-debug takes effect immediately.
    // OR'd together: --gbuffer alone must write with no view selected.
    // Voxi's denoiser reads these targets every frame it runs.
    // Frame interpolation reads motion and depth from G-buffer.
#if AVER_MODULE_VOXI
    voxi::Renderer& vx = voxi::Renderer::get();
    const voxi::Resolution gbufRes = voxi::resolve(vx.settings(), vx.deviceInfo());
    const bool wantGbufForDenoiser = gbufRes.denoiserGBufferWanted;
    // NeuRAA runs only on ray-found first surfaces (it needs the visibility buffer); raster frames use MSAA.
    [[maybe_unused]] const bool rayPrimaryFrame = gbufRes.rtRenderMode.effective == 1u || vx.settings().pathTracing != voxi::Quality::Off;
#else
    const bool wantGbufForDenoiser = false;
    [[maybe_unused]] const bool rayPrimaryFrame = true;
#endif
    const bool wantGbufForFrameInterp = updateFrameInterpolation(e.device());
#if AVER_MODULE_SR
    // TAA's resolve reads velocity and view Z; NeuRAA reads view Z and normals.
    const bool wantGbufForTaa = rset_.temporalAaEnabled_ || (rset_.neuraaEnabled_ && rayPrimaryFrame);
#else
    const bool wantGbufForTaa = false;
#endif
    e.device()->setGBufferEnabled(rset_.gbufferOverride_ || wantGbufForDenoiser || wantGbufForFrameInterp ||
                                  wantGbufForTaa || rset_.neuraaDebugView_ ||
                                  rset_.gbufferDebugView_ != GBufferDebugFeature::Mode::Off);
    gbufferDebugFeature_.setDevice(e.device());
    // vpX_/vpY_/vpW_/vpH_: this frame's 3D-viewport rect (frame stale at worst on first draw).
    gbufferDebugFeature_.setViewportRect(static_cast<u32>(viewState_.vpX_), static_cast<u32>(viewState_.vpY_),
                                         static_cast<u32>(viewState_.vpW_), static_cast<u32>(viewState_.vpH_));
    gbufferDebugFeature_.setMode(rset_.gbufferDebugView_);
#if AVER_MODULE_VOXI
    e.device()->setMeshShaders(voxi::Renderer::get().settings().meshShaders);
#else
    e.device()->setMeshShaders(rset_.msOverride_);
#endif
#if AVER_MODULE_VOXI
#  if AVER_MODULE_FRAMEWORK
    applyScriptRender(vx);   // P17/P18: a Play session's render keys land before the settings below are read; the statistics go out
#  endif
    // Voxi owns the AA setting; push it when it changes (rebuilds targets+PSOs). Ray-driven frames run
    // at 1x whatever MSAA says (voxi::Resolution::sampleCount).
    {
        const u32 samples = voxi::resolve(vx.settings(), vx.deviceInfo()).sampleCount;
        if (vx.consumeMsaaDirty() || samples != rset_.msaaPushed_) {
            e.device()->setSampleCount(samples);
            rset_.msaaPushed_ = samples;
        }
    }

    if (rset_.voxiAttached_) {
        // Copy, load-bearing: must never write back to the singleton.
        voxi::Settings vs = voxi::Renderer::get().settings();
        frameBudgetTick(t.dt, vs);
        const f32 c[3] = {rset_.giCenter_.x, rset_.giCenter_.y, rset_.giCenter_.z};
        // Auto-switch: ray-hit/triangles debug views can't draw through the rasteriser; Wireframe/G-buffer can't draw through ray-driven.
        const bool needRaster = viewState_.wireframe_ || rset_.gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
        const bool needRayDriven = !needRaster &&
            (rset_.debugView_ != voxi::VoxiRenderer::ViewDebug::None || rset_.neuraaDebugView_) &&
            voxiRenderer_.rayDrivenAvailable();
        // Path Tracing forces ray-driven primary (VoxiRenderer::setSettings), so a raster view mode or
        // the --pt-scene reference view turns it off for the frame.
        if (needRaster || ptTakesViewport()) { vs.rtRenderMode = 0; vs.pathTracing = voxi::Quality::Off; }
        else if (needRayDriven) vs.rtRenderMode = 1;
        // Reset history when renderer mode changes (compared against last frame's effective mode).
        if (static_cast<i32>(vs.rtRenderMode) != rset_.lastEffectiveRtRenderMode_) {
            voxiRenderer_.resetRtHistory(true);
            voxiRenderer_.resetAoHistory();
            voxiRenderer_.resetGiHistory(true);
            voxiRenderer_.resetDenoiserHistory(true);
            rset_.lastEffectiveRtRenderMode_ = static_cast<i32>(vs.rtRenderMode);
        }
        // Undenoised mode: toggles denoiser, ray-tile amortisation, shadow denoise, GI spatial reuse.
        if (rset_.undenoised_) {
            vs.denoiser = false;
            vs.rtPixelsPerRayTile = 1;
            vs.rtShadowDenoise = 0;
            vs.giRestirSpatialSamples = 0;
        }
        // A heavy mode switch is announced a frame before it is applied (ModeSwitchNotice.hpp).
        static editor::ModeSwitchNotice modeNotice;
        // A held switch keeps last frame's mode; a raster frame never ray-traces (VoxiRenderer::buildAccelerationStructures).
        if (modeNotice.shouldApply(vs, e.window() != nullptr && maxFrames_ == 0, &shaderWarmup_))
            voxiRenderer_.setSettings(vs);
        // Consume-and-forward: console reset commands routed through singleton flags to the renderer instance.
        if (voxi::Renderer::get().consumeGiHistoryResetRequest())  voxiRenderer_.resetGiHistory();
        if (voxi::Renderer::get().consumeRtHistoryResetRequest())  voxiRenderer_.resetRtHistory();
        if (voxi::Renderer::get().consumeAoHistoryResetRequest())  voxiRenderer_.resetAoHistory();
        if (voxi::Renderer::get().consumeDenoiserHistoryResetRequest()) voxiRenderer_.resetDenoiserHistory();
        // Reset GI/RT/denoiser history every frame if console flag set or Undenoised active.
        const u32 everyFrame = editor::consoleResetHistoryEveryFrameSlot();
        if (everyFrame || rset_.undenoised_) {
            if ((everyFrame & 1u) || rset_.undenoised_) voxiRenderer_.resetGiHistory(/*quiet=*/true);
            if ((everyFrame & 2u) || rset_.undenoised_) voxiRenderer_.resetRtHistory(/*quiet=*/true);
            if ((everyFrame & 4u) || rset_.undenoised_) voxiRenderer_.resetDenoiserHistory(/*quiet=*/true);
        }
        voxiRenderer_.setVolume(c, rset_.giExtent_);
        // Baked volume cache directory; empty when no project open.
        voxiRenderer_.setGiCacheDir(projSess_.project_.valid() ? fmt::giCacheDir(projSess_.project_.dir) : std::string());
        voxiRenderer_.setDebugView(rset_.giDebugView_);
        // Console knob voxi.giPoisonView reasserted each frame.
        voxiRenderer_.setGiPoisonView(editor::consoleGiPoisonViewSlot());
        // Lighting-contrast legacy bitmask.
        voxiRenderer_.setLightingLegacyBits(editor::consoleLightingLegacySlot());
        // Console knobs for GI optimisations.
        voxiRenderer_.setGiForceRebuild(editor::consoleGiForceRebuildSlot());
        voxiRenderer_.setGiBoundedDispatch(editor::consoleGiBoundedDispatchSlot());
        voxiRenderer_.setGiSettleTicks(editor::consoleGiSettleTicksSlot());
        voxiRenderer_.setGiFreeAccumulator(editor::consoleGiFreeAccumulatorSlot());
        // Console knobs for GI vis and blended cone.
        voxiRenderer_.setGiVisPathView(editor::consoleGiVisPathViewSlot());
        voxiRenderer_.setBlendedGiCone(editor::consoleBlendedGiConeSlot());
        // Neural Visualiser NeuRaC view.
        voxiRenderer_.setNeuRaCView(static_cast<u32>(dbgOv_.neuracViewMode_ < 0 ? 0 : dbgOv_.neuracViewMode_), dbgOv_.neuracViewGrid_);
        // See setGiConeTraceOff's comment; applied every frame.
        voxiRenderer_.setConeTraceEnabled(!rset_.giConeTraceOff_);
#if AVER_MODULE_SR
        // AverSR level resolved fresh each frame from precedence chain.
        updateAverSrAuto(e);
        applyUpscalerSlot(e.device());
#endif
#if AVER_MODULE_FRAMEWORK
        // Play only: a script's sun direction reaches the GI too (assets/LevelSky.hpp); authoring is untouched.
        rhi::SkyAtmosphere skySunScratch;
        const rhi::SkyAtmosphere& skySun = playSessionActive() ? assets::withScriptEnv<assets::FrameworkWorldFxApi>(rset_.sky_, skySunScratch) : rset_.sky_;
#else
        const rhi::SkyAtmosphere& skySun = rset_.sky_;
#endif
        const Vec3 sd = Vec3{skySun.sunDirection[0], skySun.sunDirection[1],
                             skySun.sunDirection[2]}.getSafeNormal();
        if (harness_.sunAngle_ > 0.0f) rset_.sky_.sunAngularDiameterDeg = harness_.sunAngle_;
        // Direction only; color/ambient reach shaders through device frame constants.
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif
}

// Viewport rect, camera matrices, fog, sky and runtime assets.
void SandboxApp::setupCameraAndSky(Engine& e) {
    // Confine the scene to the dockspace's central node (latched by buildUI last frame).
    e.device()->setViewportRect((u32)viewState_.vpX_, (u32)viewState_.vpY_, (u32)std::fmax(1.0f, viewState_.vpW_), (u32)std::fmax(1.0f, viewState_.vpH_));

    const Vec3 fwd = viewForward();
    const f32 aspect = viewAspect();
    const game::CameraMatrices cam = game::pushCamera(*e.device(), viewPos(), fwd, aspect, playView_);
    viewState_.invVP_ = cam.invVP; viewState_.viewProj_ = cam.viewProj; viewState_.eye_ = viewPos();

    // One member for one value; fixes dead code in fog handling.
    f32 fog = rset_.fogDensity_;
#if AVER_MODULE_SCENE
    // Opt-in: match fog density to streaming load boundary (foggier, only when asked).
    if (rset_.matchFogToStreamRadius_ && streaming_.enabled()) {
        const world::StreamSettings& st = streaming_.settings().stream;
        const f32 boundaryCm = static_cast<f32>(st.loadRadius) * static_cast<f32>(st.chunkSizeCm);
        const f32 matched = fogDensityForOpacityAt(boundaryCm, rset_.fogMatchTargetOpacity_);
        if (matched > 0.0f) fog = matched;
    }
#endif
    // sunDirection stays unnormalised; shaders normalise it.
    rset_.sky_.enabled = viewState_.showAtmosphere_;   // Show > Atmosphere; the backend already gates the sky draw on this
    for (int i = 0; i < 3; ++i) {
        rset_.sky_.sunColor[i] = rset_.sunColor_[i];
        rset_.sky_.zenith[i]   = rset_.skyZenith_[i];
        rset_.sky_.horizon[i]  = rset_.skyHorizon_[i];
        rset_.sky_.fogColor[i] = rset_.fogColor_[i];
    }
    // --sky-light overrides the level, reapplied here since applyLevelSky overwrites sunAmbient_ on level open.
    if (rset_.skyLightOverride_ >= 0.0f) rset_.sunAmbient_ = rset_.skyLightOverride_;
    // --sky-physical/--sky-authored/sun elevation, reapplied here for the same reason.
    if (rset_.skyModelOverride_ >= 0)
        rset_.sky_.model = rset_.skyModelOverride_ ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
    if (rset_.sunElevationOverride_ > -90.0f) {
        f32 elev = 0.0f, azim = 0.0f;
        rset_.sky_.sunAngles(elev, azim);
        // Azimuth unchanged; flag names elevation only, silently rotating the sun would make runs differ.
        rset_.sky_.setSunAngles(rset_.sunElevationOverride_, azim);
    }
    rset_.sky_.skyLightIntensity = rset_.sunAmbient_;
#if AVER_MODULE_SCENE
    // A streamed level that hides its load edge in fog (World Settings > Level Streaming).
    if (level_.streaming().active()) fog = assets::fogForStreamEdge(lvl_.levelHeader_.stream, rset_.sky_, fog, viewState_.eye_.z);
#endif
    rset_.sky_.fogDensity = fog;
    rset_.sky_.cloudTime = rset_.cloudTime_;
    // Underwater fog applied to a copy; sky_ is the authored sky and must stay unchanged.
    // Script sun / fog / sky-light overrides ride a copy, in a Play session only: sky_ stays the authored sky.
#if AVER_MODULE_FRAMEWORK
    rhi::SkyAtmosphere skyScriptScratch;
    const rhi::SkyAtmosphere& skyNow = playSessionActive() ? assets::withScriptEnv<assets::FrameworkWorldFxApi>(rset_.sky_, skyScriptScratch) : rset_.sky_;
#else
    const rhi::SkyAtmosphere& skyNow = rset_.sky_;
#endif
#if AVER_MODULE_FLUIDS
    e.device()->setSkyAtmosphere(water_.applyUnderwaterFog(skyNow, viewPos().z));
#else
    e.device()->setSkyAtmosphere(skyNow);
#endif
    // Outside the viewport rect is editor chrome, not sky.
    e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Runtime meshes (G4 R7): meshes a Play session's scripts built, or a MultiMesh baked, since last frame are uploaded before the draw walk. None = nothing to do.
    content_.syncRuntimeMeshes(*e.device());
    // Runtime textures (E8): the pixels a Play session's scripts built since last frame are uploaded and handed to the materials that name them.
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
    content_.syncRuntimeTextures(*e.device(), &voxiRenderer_.materials());
#else
    content_.syncRuntimeTextures(*e.device(), nullptr);
#endif
    // Sub-viewports (G4 R5): a Play session's render-to-texture views. Their draw lists and cameras are set here because the views' features draw in the device's
    // beginFrame, which follows. With no view in the registry (every editor session that is not playing one) this returns at once.
    {
        game::SubViewportHost::Inputs svIn;
        svIn.frame = e.time().frame;
        svIn.content = &content_;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        svIn.materials = &voxiRenderer_.materials();
#endif
        svIn.skinning = skinnedScene_.get();
        game::DrawWorldOptions svWalk;
#if AVER_MODULE_RENDER_SOFTBODY
        svWalk.softBodies = softBodyScene_.get();
#if AVER_MODULE_RENDER_HAIR
        svWalk.hair = hairScene_.get();
#endif
#endif
        svIn.walk = &svWalk;
        svIn.sky = &skyNow;
        subViews_.update(svIn);
    }
#endif
}

// The post-process settings, with the level's volumes and accessibility on top.
void SandboxApp::pushPostSettings(Engine& e, bool debugViewActiveThisFrame) {
    if (rset_.postPushedValid_ && !rhi::postSettingsEqual(e.device()->postProcess(), rset_.postPushed_))
        rset_.post_ = e.device()->postProcess();
    // Debug views paint flat diagnostic colours; disable auto-exposure/bloom/local exposure.
    if (debugViewActiveThisFrame) {
        rhi::PostSettings dbg = rset_.post_;
        dbg.autoExposure = false;
        dbg.exposure = 1.0f;
        dbg.bloomIntensity = 0.0f;
        dbg.localExposureShadows = 0.0f;
        dbg.localExposureHighlights = 0.0f;
        e.device()->setPostProcess(dbg);
        rset_.postPushed_ = dbg;
    } else {
        // The level's post volumes blend onto a copy: post_ stays the base the panels edit (POST_VOLUMES.md).
        rhi::PostSettings shown = postVolumeTools_.blend(*e.device(), projSess_.project_.contentDir(),
                                                              lvl_.levelHeader_.postVolumes, rset_.post_, viewPos());
        // The player's accessibility choices win over the level's volumes, in a Play session only (a saved option must not change the
        // authoring view); the editor's colour-vision preview fills in when no filter is set. Scene only: the editor chrome draws after.
        if (playSessionActive()) game::applyAccessibilityToPost(shown, access_.snap);
        editor::applyPreviewToPost(accessPreview_, shown);
        e.device()->setPostProcess(shown);
        rset_.postPushed_ = shown;
    }
    rset_.postPushedValid_ = true;
}

void SandboxApp::onUpdate(Engine& e, const Timestep& t)  {
    HitchMarks hm("update");
#if AVER_MODULE_SCENE
    tickOrigin(e);   // S0: the only place the floating origin shifts (LARGE_WORLD 2.2.4); before input, gameplay, physics, streaming, flush
#endif
    // Forced unpause on scope exit (holds through all exit paths).
    struct FrameStepGuard {
        bool active = false;
        ~FrameStepGuard() {
#if AVER_MODULE_FRAMEWORK
            if (active) aver_fw_set_paused(1);
#endif
        }
    } frameStepGuard;
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Stop asked for the level back as it was when Play began (stopPlay; it cannot load from inside the UI).
    if (play_.pieReloadPending_ && !anyPlayActive()) reloadAfterPlay(e);
#endif
    applyConsoleSets(e, t);
    // Camera set first so downstream (view matrix, reprojection, shadow history) sees one consistent frame.
    bool debugViewActiveThisFrame = false;
    tickViewModeAndCameraDrift(t, debugViewActiveThisFrame);
    tickCameraAutomation(e, t, hm);
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    reassertOcclusionConsole();
#endif
    pollAutosave(e, t);
    hm.mark("autosave");
    tickLaunchRequests(t);
    tickAutoFrameCommands(e, t);
    // The Path Tracing tier drives Voxi's path-traced frame (voxi_pt.hlsli); the standalone reference
    // view below is --pt-scene only. Add/remove render feature (only safe before device_->beginFrame; see syncPtSceneView()).
    syncPtSceneView(e.device());
    hm.mark("ptSceneView");
    tickPathTracingAutomation();
#if AVER_MODULE_MCP
    pumpMcp();
#endif
    tickAmbientState(t);
#if AVER_WITH_IMGUI
    tickDpiAndPlayWindow(e);
    hm.mark("playWindow");
    resolveEditorInputOwnership(e);
    tickEditorFlyCamera(t);
#endif
#if AVER_MODULE_SCRIPTING
    scripts_.update(t.dt);
    hm.mark("scripts");
#endif
    // Game time: the frame scaled by the time scale (1 outside a session). tickGameplayGroups takes real time
    // (physics keeps a real-time cadence and scales each step); the gameplay systems below advance in game time.
#if AVER_MODULE_FRAMEWORK
    const f32 gameDt = t.dt * game::gameTimeScale();
#else
    const f32 gameDt = t.dt;
#endif
#if AVER_MODULE_FRAMEWORK
    tickPlayMouseCapture(e);
    pollCapturedMouse();
    pushInput(e.device()->uiActive());
    hm.mark("input");
#if AVER_MODULE_SCRIPTING
    reportHudPreview();
#endif
#if AVER_MODULE_FRAMEWORK
    publishWindowState(e);
#endif
    if (hudPreviewActive()) aver_ui_begin_frame(harness_.hudRectX_, harness_.hudRectY_, harness_.hudRectW_, harness_.hudRectH_);
    else                    aver_ui_begin_frame(viewState_.vpX_, viewState_.vpY_, viewState_.vpW_, viewState_.vpH_);
    // Font and pointer lent per frame, right after frame opens. Font re-lent every frame (loadGameUiFont can leave it invalid).
    aver_ui_set_font(&uiFont_);
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    submitSubViewports();
#endif
    feedUiPointer();
#if AVER_MODULE_SCRIPTING
    if (hudPreviewActive()) scripts_.hudDraw(harness_.hudPreviewIndex_, t.dt);
#endif
    tickGameUi(e, t);
    hm.mark("ui+tests");
#if AVER_MODULE_SCRIPTING
    pumpGraphDebugger();
#endif
    // Frame Skip: lift pause for exactly this frame's tick groups. frameStepGuard's destructor puts it back.
    if (play_.playFrameStepPending_ && aver_fw_play_state() == AVER_FW_PLAY_PAUSED) {
        play_.playFrameStepPending_ = false;
        aver_fw_set_paused(0);
        frameStepGuard.active = true;
    }
    tickGameplay(e, t, gameDt);
    hm.mark("gameplay");
#endif
#if AVER_MODULE_FLUIDS
    tickWater(e, hm, gameDt);
#endif
#if AVER_MODULE_SCENE
    tickObjectAnimation(e, hm, gameDt);
#if AVER_MODULE_SOFTBODY_SCENE
    tickSoftBodySim(hm, gameDt);
#endif
    // The level sequence's Post phase (Animate preview, Play), after the clips so a sequenced actor wins.
    tickSequencePost();
    hm.mark("sequence");
#if AVER_MODULE_PHYSICS
    tickDriveBodies(hm, gameDt);
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    tickParticles(gameDt);
#endif
    updateRenderScenes(e, hm);
    tickThumbnailsAndTestHooks(e);
#if AVER_MODULE_SCRIPTING
    tickGraphs(t, hm, gameDt);
#endif
    tickChunkAndLevelStreaming(t, hm);
    playProf_.begin(editor::PlayPhase::WorldFlush);
    scene::World::instance().flush();
    playProf_.end(editor::PlayPhase::WorldFlush);
    hm.mark("worldFlush");
#if AVER_WITH_AUDIO_ABI
    // Reclaims finished voices every frame (Play or not). Gap: aver_audio_init had single caller, so voices leaked until mixer ran out.
    // Not gated on Play: voices outlive a session, so gating would leak voices that finish after Stop.
    aver_audio_collect();
    hm.mark("audio");
#endif
#if AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_FRAMEWORK
    // Gated on Play (like physics/fw_tick above): pathing is gameplay, not authoring preview.
    // nav_ may be empty or frame stale; AgentSystem::tick treats that as "wait for a grid".
    if (!harness_.spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        game::tickAi(gameDt, &dbgOv_.nav_);
    }
#endif
#endif
    resolveViewOverride(t);
    applyDeviceRenderSettings(e, t);
    setupCameraAndSky(e);
    hm.mark("deviceSetup");
    // Console post.* vars write the device, not post_; adopt device changes into post_ before pushing.
    pushPostSettings(e, debugViewActiveThisFrame);
}

} // namespace aver
