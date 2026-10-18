// SandboxApp::onUpdate: one editor frame (MCP commands, camera, gameplay tick, physics, render state). Split from
// SandboxApp.cpp, which was nearing the COFF section cap in Debug (sandbox/CMakeLists.txt: split, never /bigobj).
#include "aver/core/HitchMarks.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "SandboxApp.hpp"
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_anim_abi.h"   // aver_fw_set_anim_provider (montages)
#  include "aver/framework/framework_ik_abi.h"     // [wave5/ik-retarget] aver_fw_set_ik_provider

#  include "aver/framework/framework_morph_abi.h"   // [wave9/morph] aver_fw_set_morph_provider
#  include "aver/framework/framework_worldfx_abi.h"   // [wave9/worldfx] aver_fw_worldfx_tick
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_loc_abi.h"    // the UI layout editor's culture preview
#endif
#if AVER_MODULE_RAGDOLL
#  include "aver/ragdoll/RagdollSystem.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
#include "TextureEditor.hpp"   // makeTextureEditor; SandboxApp.hpp does not pull this one in
#include "InstanceSetEditor.hpp"   // makeInstanceSetEditor (.ocinst); not pulled in by SandboxApp.hpp
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameIkProbe.hpp"   // [wave5/ik-retarget] the foot-IK ground probe
#include "aver/game/GameTick.hpp"
#include "aver/game/GameSystemsWiring.hpp"
#include "aver/game/SequenceHost.hpp"   // [wave5/sequencer] the ABI provider
#include "aver/game/GameUiInput.hpp"
#include "aver/game/GameUiText.hpp"
#include "aver/platform/FileSystem.hpp"   // userDataDir: where the trajectory network's weights live
#if AVER_MODULE_VOXI
#include "ModeSwitchNotice.hpp"
#endif

namespace aver {

// Advances one frame: MCP commands, camera, gameplay tick, physics, and the render state.
void SandboxApp::onUpdate(Engine& e, const Timestep& t)  {
    HitchMarks hm("update");
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
    if (pieReloadPending_ && !anyPlayActive()) reloadAfterPlay(e);
#endif
    // --set NAME VALUE applied at frame 5 (after project's RENDER.* apply).
#if AVER_WITH_IMGUI
    if (!consoleSetsApplied_ && !consoleSets_.empty() && t.frame >= 5 && e.device()) {
        consoleSetsApplied_ = true;
        for (const auto& kv : consoleSets_) {
            AVER_INFO("[Sandbox] --set {} {}", kv.first, kv.second);
            runConsoleLine(e, "set " + kv.first + " " + kv.second);
        }
    }
#else
    if (!consoleSetsApplied_ && !consoleSets_.empty()) {
        consoleSetsApplied_ = true;
        AVER_WARN("[Sandbox] --set ignored: this build has no editor console to route {} pair(s) through",
                  consoleSets_.size());
    }
#endif
    // Camera set first so downstream (view matrix, reprojection, shadow history) sees one consistent frame.
    bool debugViewActiveThisFrame = false;
#if AVER_MODULE_VOXI
    // View mode reaches Voxi from onUpdate (before prePass), unlike unlit which rides per-draw state.
    voxiRenderer_.setUnlit(unlit_);
    // Debug views need rasteriser; reasserted here before prePass for the same reason.
    const bool viewModeNeedsRaster = wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
    // Raster-only mode clears ray-hit/triangles view (wireframe_ can be ticked from Settings too).
    if (viewModeNeedsRaster) debugView_ = voxi::VoxiRenderer::ViewDebug::None;
    const bool viewModeNeedsRayDriven = !viewModeNeedsRaster &&
        debugView_ != voxi::VoxiRenderer::ViewDebug::None && voxiRenderer_.rayDrivenAvailable();
    voxiRenderer_.setViewDebug(viewModeNeedsRayDriven ? debugView_ : voxi::VoxiRenderer::ViewDebug::None);
    debugViewActiveThisFrame = viewModeNeedsRayDriven;
    // Wireframe disables shadows, GI, ray tracing and denoising (see setPaused).
    voxiRenderer_.setPaused(wireframe_);
#endif
    // --cam-wobble-stop N: wobble runs only below frame N, then holds. Measuring artifact appearance/decay needs both.
    if (camWobbleDeg_ != 0.0f && camWobblePeriod_ > 0 &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        if (!camWobbleBased_) { camWobbleBaseYaw_ = yaw_; camWobbleBased_ = true; }
        const f32 phase = 6.2831853f * (f32)(t.frame - 1) / (f32)camWobblePeriod_;
        yaw_ = camWobbleBaseYaw_ + camWobbleDeg_ * 0.01745329252f * std::sin(phase);
    }
    // --cam-translate SPEED: one fixed step per frame along camForward(), composed with wobble.
    if (camTranslateSpeed_ != 0.0f &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        camPos_ += camForward() * camTranslateSpeed_;
    }
    // --cam-wander AMP SPEED: follows sum of sines at irrational frequencies (NeuraFI training trajectory).
    // NeuRAA's training capture holds it still while it captures a pose, then it resumes where it was.
#if AVER_MODULE_SR
    bool neuraaHold = neuraa_ && neuraa_->captureHolding();
#else
    bool neuraaHold = false;
#endif
#if AVER_MODULE_VOXI
    // NRD2 training (Tools > Train Neural Denoiser, --nrd2-train): its GPU hook goes on or off here, before beginFrame.
    if (!nrd2MenuWired_ && nrd2Session_.available()) {
        tools_.setNeuralDenoiserOpener([this] { nrd2Session_.openWindow(); });
        nrd2MenuWired_ = true;
    }
    nrd2Session_.tick(e.device());
    shaderWarmup_.poll();
    hm.mark("head");
    // --nrd2-capture: handed over once the renderer is attached and the level asked for (--open-level) is the
    // one loaded, plus ~10 s for streaming to bring in the scene (the startup level used to be captured
    // instead); it starts stepping on NRD2 frames and holds the camera the same way.
    const bool nrd2LevelReady = openLevelByName_.empty() && pendingOpenPath_.empty() && !pendingOpenPrompt_ &&
                                !levelName_.empty();
    nrd2CaptureSettle_ = nrd2LevelReady ? nrd2CaptureSettle_ + 1u : 0u;
    if (nrd2CapturePoses_ && voxiAttached_ && !nrd2CaptureStarted_ && (nrd2CaptureSettle_ > 600u || t.frame > 6000)) {
        render::denoise::Nrd2CaptureConfig c;
        c.scene = levelName_.empty() ? std::string("unnamed") : levelName_;
        // Path Tracing feeds NRD2 different noise: its poses form their own scene for stratified training.
        if (voxi::Renderer::get().settings().pathTracing != voxi::Quality::Off) c.scene += "_PT";
        for (char& ch : c.scene)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) ch = '_';
        c.dir = (nrd2CaptureDir_.empty() || nrd2CaptureDir_ == "default" || nrd2CaptureDir_ == "-")
              ? userDataDir() + AVER_PATH_SEP "nrd2_dataset" AVER_PATH_SEP + c.scene : nrd2CaptureDir_;
        c.poses = nrd2CapturePoses_;
        c.hold = nrd2CaptureHold_;
        c.heldOutFrom = nrd2CaptureHeldOut_;
        c.oracle = nrd2CaptureGrid_ ? render::denoise::Nrd2OracleMode::Grid : render::denoise::Nrd2OracleMode::Grad;
        if (camWanderAmp_ <= 0.0f)
            AVER_WARN("[NRD2] --nrd2-capture without --cam-wander: every pose is the same view");
        voxiRenderer_.startNrd2Capture(c);
        nrd2CaptureStarted_ = true;
    }
    if (voxiAttached_ && voxiRenderer_.nrd2CaptureHolding()) neuraaHold = true;
#endif
    for (const auto& [f, p] : camTeleports_) if (f > 0 && t.frame == static_cast<u64>(f)) teleportCamera(p);
    if (neuraaHold) ++camWanderHeld_;
    if (camWanderAmp_ > 0.0f && !neuraaHold && (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        if (!camWanderBased_) {
            camWanderBasePos_ = camPos_; camWanderBaseYaw_ = yaw_; camWanderBasePitch_ = pitch_;
            camWanderBased_ = true;
        }
        const f32 s = static_cast<f32>(t.frame - 1 - camWanderHeld_) * camWanderSpeed_;
        const f32 a = camWanderAmp_;
        yaw_ = camWanderBaseYaw_ + a * 0.4363f *   // +-25 degrees at AMP 1
               (std::sin(0.0131f * s) + 0.6f * std::sin(0.0317f * s + 1.1f) + 0.3f * std::sin(0.0719f * s + 2.3f)) / 1.9f;
        pitch_ = camWanderBasePitch_ + a * 0.1396f *   // +-8 degrees
                 (std::sin(0.0173f * s + 0.5f) + 0.5f * std::sin(0.0437f * s + 1.3f)) / 1.5f;
        camPos_ = camWanderBasePos_ + Vec3{a * 300.0f * std::sin(0.0091f * s + 0.2f),    // +-3 m across
                                           a * 300.0f * std::sin(0.0113f * s + 1.9f),
                                           a * 60.0f * std::sin(0.0213f * s + 0.7f)};    // +-0.6 m up/down (Z up)
    }
    if (camFlyX_ != 0.0f || camFlyY_ != 0.0f) { camPos_.x += camFlyX_; camPos_.y += camFlyY_; }
    if (camFlyYaw_ != 0.0f) yaw_ += camFlyYaw_ * 0.0174533f;
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // --no-occlusion-cull: reasserted every frame (applyProjectVoxiSettings rewrites it during level load).
    if (occlusionCullForceOff_) occlusionCullEnabled_ = false;
    // --occlusion-waitidle: reasserted every frame so console `set` is live before this frame's testBatch().
    if (occluder_) occluder_->setDebugForceWaitIdle(editor::consoleOcclusionForceWaitIdleSlot());
    // occlusion.showCulled/occlusion.cullUnderSuppression read every frame (live for this frame's onRender walk).
    occlusionShowCulled_ = editor::consoleOcclusionShowCulledSlot();
    occlusionCullUnderSuppression_ = editor::consoleOcclusionCullUnderSuppressionSlot();
#endif
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
    hm.mark("autosave");
    // Census at frame 2+ (after applyProject's scripting and spawnClassPlacements).
#if AVER_MODULE_SCENE
    if (sceneCensus_ && !sceneCensusDone_ && t.frame >= 2) {
        sceneCensusDone_ = true;
        // playerStart_ excluded (editor synthesises it but shipped game doesn't).
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(
                      world::takeSceneCensus(scene::World::instance(), playerStart_)));
    }
#endif
#if AVER_MODULE_SCENE
    // --save-level <out>: writes the OPEN level to another path (not over levelPath_).
    // Waits for every pending open (--open-level, forwarded launch, unsaved-changes prompt) before acting.
    if (!saveLevelTo_.empty() && !saveLevelDone_ && !levelPath_.empty() &&
        openLevelByName_.empty() && pendingOpenPath_.empty() && !pendingOpenPrompt_) {
        saveLevelDone_ = true;
        if (saveLevel(saveLevelTo_))
            AVER_INFO("[Level] --save-level wrote '{}' ({}) to {}",
                      levelName_, levelPath_, saveLevelTo_);
        else
            AVER_ERROR("[Level] --save-level failed for {}", saveLevelTo_);
    }
#endif
#if AVER_MODULE_SCENE
    // [wave3/level-instances begin] --bake-level-pack <level>: bakes that level's pack once the project's meshes are in.
    if (!bakeLevelPackAsset_.empty() && !bakeLevelPackDone_ && t.frame >= 2 && pendingOpenPath_.empty() && !pendingOpenPrompt_) {
        bakeLevelPackDone_ = true;
        std::string bakeWhy;
        world::LevelAssetLibrary& lib = level_.assets();
        lib.setLoader(world::LevelAssetLibrary::diskLoader(project_.contentDir()));
        world::PackBakeReport rep;
        if (game::bakeLevelPackWithContent(lib, bakeLevelPackAsset_, content_, project_.contentDir(), project_.dir, rep, &bakeWhy))
            AVER_INFO("[LevelPack] --bake-level-pack: {} row(s) in {} group(s), {} kept as entities", rep.rows, rep.groups, rep.kept);
        else
            AVER_ERROR("[LevelPack] --bake-level-pack failed for {}: {}", bakeLevelPackAsset_, bakeWhy);
    }
    // [wave3/level-instances end]
#endif
    // Forwarded open request (guarded as a whole; queue filled only by setOpenRequestHook).
#if AVER_MODULE_SCENE
    if (window_ && window_->hasPendingOpenRequest()) {
        const std::string path = window_->takePendingOpenRequest();
        if (isLevelFile(path.c_str())) {
            // Same funnel as Content Browser (unsaved-changes check and class-placement spawn).
            if (!requestOpenLevel(path, "opened, forwarded from another launch"))
                AVER_WARN("[Sandbox] forwarded level '{}': {}", path, openLevelError_);
        }
    }
#endif  // AVER_MODULE_SCENE
    // Dropped files from Explorer (drained unconditionally; Window keeps accepting WM_DROPFILES regardless).
    if (window_ && window_->hasPendingDroppedFiles()) {
        const std::vector<std::string> dropped = window_->takePendingDroppedFiles();
#if AVER_WITH_IMGUI
        if (project_.valid()) {
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
    // --pt-scene-toggle-on/-off: checked before syncPtSceneView() so the same onUpdate() that flips the flag also acts on it.
    if (ptSceneToggleOnAutoFrames_  > 0 && --ptSceneToggleOnAutoFrames_  == 0) ptSceneViewWantEnabled_ = true;
    if (ptSceneToggleOffAutoFrames_ > 0 && --ptSceneToggleOffAutoFrames_ == 0) ptSceneViewWantEnabled_ = false;
    // --sun-set-at: writes the same setting as Directional Light panel's sliders.
    if (sunSetAtFrames_ > 0 && --sunSetAtFrames_ == 0) {
        sky_.setSunAngles(sunSetElevDeg_, sunSetAzimDeg_);
        AVER_INFO("[Sandbox] --sun-set-at: sun moved to elevation {:.1f} deg, azimuth {:.1f} deg",
                  sunSetElevDeg_, sunSetAzimDeg_);
    }
    // --sun-sweep: turns the sun every frame until turnsLeft reaches 0.
    if (sunSweepFrames_ > 0) {
        if (sunSweepFrames_ > 1) {
            --sunSweepFrames_;
        } else {
            f32 el = 0.0f, az = 0.0f;
            sky_.sunAngles(el, az);
            sky_.setSunAngles(el, az + sunSweepDeg_);
            if (sunSweepTurnsLeft_ > 0 && --sunSweepTurnsLeft_ == 0) {
                sunSweepFrames_ = 0;
                sky_.sunAngles(el, az);
                AVER_INFO("[Sandbox] --sun-sweep-frames: the drag let go at frame {}, sun at elevation "
                          "{:.3f} deg, azimuth {:.3f} deg", t.frame, el, az);
            }
        }
    }
#if AVER_MODULE_VOXI
    // --gi-history-reset-at: resets GI reservoir and denoiser history.
    if (giHistoryResetAtFrames_ > 0 && --giHistoryResetAtFrames_ == 0) {
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
    if (renderScaleCookieArmed_ && t.frame >= 30) {
        renderScaleCookieArmed_ = false;
        editor::setPrefBool("display.renderScalePending", false);
        editor::flushEditorPrefs();
    }
#if AVER_MODULE_SR
    // --aversr-cycle: On/Off/On to exercise the transition that used to crash.
    if (averSrCycleFrames_ > 0 && --averSrCycleFrames_ == 0) {
        AVER_INFO("[AverSR] --aversr-cycle: Performance -> Off, the transition that used to crash");
        applyAverSrQuality(e.device(), aver::sr::Quality::Performance);
        applyAverSrQuality(e.device(), aver::sr::Quality::Off);
        AVER_INFO("[AverSR] --aversr-cycle: survived the round trip");
    }
#endif
    // The Path Tracing tier drives Voxi's path-traced frame (voxi_pt.hlsli); the standalone reference
    // view below is --pt-scene only. Add/remove render feature (only safe before device_->beginFrame; see syncPtSceneView()).
    syncPtSceneView(e.device());
    hm.mark("ptSceneView");
    // Path tracer's matched-environment legacy switch (contrast-fix F6/F7, root cause R5).
    if (ptSceneView_) ptSceneView_->setLegacyEnvironment(editor::consolePtLegacyEnvSlot());
#if AVER_MODULE_VOXI
    // --pt-quality-ramp: verification-only, raises PT rung on live view every N frames.
    if (ptQualityRampEvery_ > 0 && --ptQualityRampCountdown_ <= 0) {
        ptQualityRampCountdown_ = ptQualityRampEvery_;
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
    if (giMethodCycleEvery_ > 0 && --giMethodCycleCountdown_ <= 0) {
        giMethodCycleCountdown_ = giMethodCycleEvery_;
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
#if AVER_MODULE_MCP
    // One event per frame (click needs press and release).
    if (mcp_.listening()) mcp_.pump([this](const mcp::Command& c) { applyMcpCommand(c); });
#endif
    if (sky_.cloudsEnabled) cloudTime_ += t.dt;
#if AVER_MODULE_FRAMEWORK
    aver_fw_worldfx_tick(t.dt);   // every frame, Play or not: advances the script post blends, and the first tick after Stop drops the session's overrides, layers and scales
#endif
    if (showUiDemo_) {
        uiDemoHealth_  = 0.5f + 0.5f * std::sin(uiDemoClock_ * 0.7f);
        uiDemoStamina_ = 0.5f + 0.5f * std::sin(uiDemoClock_ * 1.6f + 1.0f);
        uiDemoScroll_  = std::fmod(uiDemoScroll_ + t.dt * 24.0f, 216.0f);   // px, wraps at 12 rows
        uiDemoClock_  += t.dt;
    }
#if AVER_WITH_IMGUI
    if (e.device()->uiActive()) {
        // Must stay outside the ImGui frame (font atlas cannot be touched inside one).
        const f32 dpi = e.window() ? e.window()->dpiScale() : dpi_;
        if (std::fabs(dpi - dpi_) > 0.01f) {
            AVER_INFO("[Sandbox] DPI changed {:.2f} -> {:.2f}, re-rasterising the UI font", dpi_, dpi);
            applyDpi(dpi);
        }
    }
    updatePlayWindow(e);
    hm.mark("playWindow");
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
        ic.defaultPawnPlay   = defaultPawnPlay_ && !playEjected();
#endif
        ic.mouseCaptured     = mouse_.captured();
        ic.pointerInViewport = levelHovered_ && inViewport(oio.MousePos.x, oio.MousePos.y);
        ic.drawerOpen        = drawer_ != Drawer::None;
        ic.landscapeMode     = mode_ == EditorMode::Landscape;
        // ImGui sees only the editor window: the play window's keys and mouse are the game's.
        if (playWindowFocused()) {
            ic.textInput = ic.uiWantsKeyboard = ic.uiWantsMouse = false;
            ic.pointerInViewport = true;
        }
        // --wheel-speed-test overrides stand in for right-drag state (headless has no real cursor).
#if AVER_MODULE_FRAMEWORK
        if (wheelTestForceFly_) {
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
    if (e.device()->uiActive() && !ImGui::GetIO().MouseDown[1] && !input_.mouseHeld(1)) flying_ = false;
#endif
    if (own_.keyboardToTool || own_.mouseToTool) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool inPlayWindow = playWindowFocused();
        const bool overUI = !inPlayWindow && (!levelHovered_ || !inViewport(io.MousePos.x, io.MousePos.y));
        // The play window's keys arrive through input_ only (ImGui sees the editor window).
        auto flyKey = [&](ImGuiKey k, i32 vk) { return ImGui::IsKeyDown(k) || (inPlayWindow && input_.keyHeld(vk)); };
        if (inputProbe_ && (ImGui::GetFrameCount() % 30) == 0)
            AVER_INFO("[input-probe] mouse=({},{}) wantCaptureMouse={} hovered={} inViewport={} "
                      "focused={} -> overUI={} | flying={} cam=({:.0f},{:.0f},{:.0f}) yaw={:.2f}",
                      (int)io.MousePos.x, (int)io.MousePos.y, (int)io.WantCaptureMouse,
                      (int)levelHovered_, (int)inViewport(io.MousePos.x, io.MousePos.y),
                      (int)levelFocused_, (int)overUI, (int)flying_,
                      camPos_.x, camPos_.y, camPos_.z, yaw_);

        // Right mouse enters fly mode; leaving is handled unconditionally above (outside this gate).
        if (ImGui::IsMouseClicked(1) && !overUI) flying_ = true;
        // The drone pawn looks while the right button is held, read from InputState: ImGui sees
        // neither a captured mouse's clicks over the viewport reliably nor the play window at all.
#if AVER_MODULE_SCENE
        if (defaultPawnPlay_ && !playEjected() && input_.mouseHeld(1) &&
            (!overUI || inPlayWindow || mouse_.captured()))
            flying_ = true;
#endif
        // --wheel-speed-test drives this directly (forces state rather than synthesising right-drag).
#if AVER_MODULE_FRAMEWORK
        if (wheelTestForceFly_) flying_ = true;
#endif
        if (flying_) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

        // Wheel changes fly speed, up = faster (Unreal's direction), 1.25x per notch so a notch feels
        // the same at any speed. The flying drone pawn takes it without RMB too: in Play the mouse
        // is the game's, and the drone moves at this same speed.
        auto wheelFlySpeed = [&] {
            const f32 wheel = input_.wheel();
            if (wheel == 0.0f) return;
            flySpeed_ *= std::pow(1.25f, wheel);
            flySpeed_ = flySpeed_ < 20.0f ? 20.0f : (flySpeed_ > 40000.0f ? 40000.0f : flySpeed_);
        };
#if AVER_MODULE_SCENE
        if (!flying_ && defaultPawnPlay_ && !playEjected() && !walkCapsule_ && (!overUI || inPlayWindow))
            wheelFlySpeed();
#endif

        if (flying_) {
            // input_.mouseDX/DY not io.MouseDelta: same phase bug as wheel below.
            // ImGui computes io.MouseDelta in NewFrame, but Engine::frameStep runs onUpdate before uiNewFrame.
            // InputState holds THIS frame's motion (fed by pumpEvents before frameStep).
            // CAPTURED (Play): the cursor is re-centred every frame, and that warp arrives as a
            // WM_MOUSEMOVE straight back, so input_'s delta nets to zero -- the view turned, then
            // snapped back. MouseCapture measures from its anchor instead (last frame's poll).
            const bool cap = mouse_.captured();
            const f32 mdx = cap ? mouse_.dx() : static_cast<f32>(input_.mouseDX());
            const f32 mdy = cap ? mouse_.dy() : static_cast<f32>(input_.mouseDY());
            yaw_   += mdx * lookSpeed_;
            pitch_ -= mdy * lookSpeed_;
            pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
            wheelFlySpeed();
        }

        const Vec3 fwd = camForward();
        const Vec3 up{0, 0, 1};
        const Vec3 right = cross(up, fwd).getSafeNormal();

        // Spectator Play flies without holding right button (mouse look still wants it).
        // Synthetic look injection point matches real look (--pie-camera-test).
#if AVER_MODULE_FRAMEWORK
        if (pieCamPendingLook_) {
            pieCamPendingLook_ = false;
            yaw_   += 0.5f;
            pitch_ += 0.3f;
            pieCamWantYaw_ = yaw_; pieCamWantPitch_ = pitch_;
        }
#endif
        // defaultPawnPlay_ is scene-guarded. Without the module, camera flies on right button alone.
        // EJECTED DROPS THE EXCEPTION: spectator pawn stops flying without RMB, block below moves camPos_ instead.
        bool flyPawnDriven = false;
#if AVER_MODULE_SCENE
        if ((flying_ || (defaultPawnPlay_ && !playEjected())) && (!io.WantCaptureKeyboard || inPlayWindow)) {
#else
        if (flying_ && (!io.WantCaptureKeyboard || inPlayWindow)) {
#endif
            const f32 sp = flySpeed_ * t.dt;
            // The possessed default pawn also takes the game's own input (AVER_FW_KEY_A.. is A-Z in
            // order), like the walking one, so --play-test and a game's input mapping can fly it.
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
            const bool pawnInput = defaultPawnPlay_ && !playEjected();
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
            if (defaultPawnPlay_ && !playEjected() && walkCapsule_) {
                driveDefaultPawnWalk(fwd, right);    // the walking default pawn (Play options > Walk)
            } else if (defaultPawnPlay_ && !playEjected() && flyCapsule_) {
                driveDefaultPawnFly(t.dt > 0.0f ? step * (1.0f / t.dt) : Vec3{0, 0, 0}, t.dt);   // collides, drifts
                flyPawnDriven = true;
            } else if (defaultPawnPlay_ && !playEjected()) {
                const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
                if (pn) {
                    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
                    scene::World& pw = scene::World::instance();
                    if (pw.valid(pe)) {
                        const Transform& cur = pw.localTransform(pe);
                        pw.setLocalPosition(pe, cur.position + step);
                        // Yaw about world up, pitch about pawn's right (same as viewport camera).
                        const Quat qy = Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw_);
                        const Quat qp = Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch_);
                        pw.setLocalRotation(pe, qy * qp);
                    }
                }
            } else
#endif
            camPos_ += step;
        } else if (!overUI) {
            // Scroll-to-dolly (uses input_.wheel() not io.MouseWheel, which is always zero here).
            const f32 wheel = input_.wheel();
            if (wheel != 0.0f) camPos_ += fwd * wheel * (flySpeed_ * 0.15f);
            // MMB pan (uses input_.mouseDX/DY, not frame-old delta).
            if (io.MouseDown[2]) {
                camPos_ -= right * static_cast<f32>(input_.mouseDX()) * 0.02f;
                camPos_ += up    * static_cast<f32>(input_.mouseDY()) * 0.02f;
            }
        }
        // Nothing drove the flying pawn: it glides to rest (the keyboard is the UI's for a moment), or
        // stops dead while ejected, where it must stay where Pawn to Camera puts it.
        if (flyCapsule_ && !flyPawnDriven) driveDefaultPawnFly(Vec3{0, 0, 0}, playEjected() ? 0.0f : t.dt);
        // Ctrl+S saves the level (the File menu's label for this shortcut wires nothing).
        // Not gated on levelFocused_ (saving isn't a viewport gesture) but gated on WantTextInput.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::LevelSave, io))
            saveLevelInteractive();

        // Ctrl+Shift+S saves everything (level plus every dirty asset tab). Same site as LevelSave.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::SaveAll, io))
            saveAll();

        // Ctrl+Shift+B / Ctrl+Shift+R mirror the Tools menu's Compile/Reload Scripts items. Same site.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::CompileScripts, io))
            tools_.requestCompileScripts(project_);
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::ReloadScripts, io))
            tools_.requestReloadScripts(project_);

        // F frames the selection at a distance derived from its radius.
        // Outliner/Details count too (levelFocused_ is false when those panels have focus).
        // Scene-guarded: F frames a SELECTED ENTITY's bounds (selectionBounds is `#if AVER_MODULE_SCENE`).
        // STANDS DOWN FOR PAWN TO CAMERA: Shift+F in ejected session both teleports pawn and frames the camera.
#if AVER_MODULE_SCENE
        if ((levelFocused_ || outlinerFocused_ || detailsFocused_) && !io.WantCaptureKeyboard &&
            keybinds_.pressed(editor::CommandId::ViewFrameSelected, io) && anySelected() &&
            !(playEjected() && hasPossessedPawn() &&
              keybinds_.held(editor::CommandId::PlayPawnToCamera, io))) {
            // selectionBounds: union of whole selection (not just anchor, which put multi-selection outside view).
            Vec3 center; f32 r;
            if (selectionBounds(center, r)) {
                const f32 d = std::fmax(50.0f, r / std::tan(radians(30.0f)) * 1.6f);
                camPos_ = center - fwd * d;
                flySpeed_ = std::fmax(flySpeed_, r * 0.4f);
                // Teleport (see frameCameraOnLevel for why streaming_.resetVelocityTracking() is needed).
                streaming_.resetVelocityTracking();
            }
        }
#endif  // AVER_MODULE_SCENE
    }
#endif
#if AVER_MODULE_SCRIPTING
    scripts_.update(t.dt);
    hm.mark("scripts");
#endif
#if AVER_MODULE_FRAMEWORK
    // --recapture-test opts in (gesture lives entirely inside this block).
    const bool interactive = (maxFrames_ == 0 && !playTest_) || recapFrames_ > 0;
    // The #if isn't redundant with uiActive(): runtime flag doesn't make ImGuiIO names COMPILE on UI-less build.
#if AVER_WITH_IMGUI
    if (e.device()->uiActive() && interactive) {
        // Clicking viewport puts mouse back in game (tested before wantCapture below).
        {
            // levelHovered_ asks if pointer is over the LEVEL window (true precondition for capture).
            const ImGuiIO& mio = ImGui::GetIO();
            // Not while ejected (that click is a viewport selection, mouse stays free).
            if (playSessionActive() && !playEjected() && releasedByUser_ && ImGui::IsMouseClicked(0) &&
                levelHovered_ && inViewport(mio.MousePos.x, mio.MousePos.y)) {
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
        // A game-UI menu that wants the cursor frees it, exactly like releasing the mouse by hand.
        const bool wantCapture = playSessionActive() && !releasedByUser_ && !playEjected() && !game::uiWantsCursor();
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
            if ((levelFocused_ || levelHovered_ || outlinerFocused_ || detailsFocused_) &&
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
    pollCapturedMouse();
    pushInput(e.device()->uiActive());
    hm.mark("input");
    // UI frame opens before gameplay ticks (ticking is when a game draws its HUD).
#if AVER_MODULE_SCRIPTING
    // --hud-preview <n>: name every HUD once, then preview one over the level viewport.
    if (hudTest_ >= 0 && !hudTestReported_ && scripts_.ready()) {
        hudTestReported_ = true;
        const i32 n = scripts_.hudCount();
        AVER_INFO("[HUD] {} declared", n);
        for (i32 i = 0; i < n; ++i) AVER_INFO("[HUD]   {}: '{}'", i, scripts_.hudName(i));
        if (hudTest_ >= n) AVER_WARN("[HUD] no HUD at index {}", hudTest_);
    }
    if (hudTest_ >= 0 && hudTest_ < scripts_.hudCount())
        setHudPreview(hudTest_, vpX_, vpY_, vpW_, vpH_);
#endif
    if (hudPreviewActive()) aver_ui_begin_frame(hudRectX_, hudRectY_, hudRectW_, hudRectH_);
    else                    aver_ui_begin_frame(vpX_, vpY_, vpW_, vpH_);
    // Font and pointer lent per frame, right after frame opens. Font re-lent every frame (loadGameUiFont can leave it invalid).
    aver_ui_set_font(&uiFont_);
    // Pointer converted here (only place that can do it correctly): UI frame is laid out against viewport, not window.
    {
        const f32 ox = hudPreviewActive() ? hudRectX_ : static_cast<f32>(vpX_);
        const f32 oy = hudPreviewActive() ? hudRectY_ : static_cast<f32>(vpY_);
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
#if AVER_MODULE_SCRIPTING
    if (hudPreviewActive()) scripts_.hudDraw(hudPreviewIndex_, t.dt);
#endif
    // Retained game-UI widgets: Play feeds them this frame's keyboard/pad/wheel; the tree itself lays out,
    // updates and draws every frame (it is empty unless a script or graph made a widget).
#if AVER_MODULE_FRAMEWORK
    // Accessibility: options, captions and narration for this frame (unscaled time), pushed to the UI before it lays out.
    game::accessibilityFrame(t.dt, viewPos(), yaw_, pitch_, access_);
#endif
    game::pushAccessibilityToUi(access_);
    if (gameUi_) gameUi_->setColourVision(access_.snap.colourVisionPacked);
    if (playSessionActive()) game::uiFeedInput(input_, t.dt, !mouse_.captured());
    aver_ui_widgets_frame(t.dt);
    maybeSpawnTestActor();
    maybePlayTest();
    maybePieCameraTest();
    maybeInputStuckTest();
    maybeInputSourceTest();
    maybeWheelSpeedTest();
    maybeRecaptureTest();
    maybeViewmodelTest();
    hm.mark("ui+tests");
#if AVER_MODULE_SCRIPTING
    pumpGraphDebugger();
#endif
    // Frame Skip: lift pause for exactly this frame's tick groups. frameStepGuard's destructor puts it back.
    if (playFrameStepPending_ && aver_fw_play_state() == AVER_FW_PLAY_PAUSED) {
        playFrameStepPending_ = false;
        aver_fw_set_paused(0);
        frameStepGuard.active = true;
    }
    // Tick groups bracket physics step: PrePhysics -> Physics -> PostPhysics.
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
        // Level's cars: each driver sets input before physics step, each entity written after.
        // Inside this gate so Pause and Frame Skip hold traffic with everything else.
        // Focus is the view (pawn's eye while possessed, free camera while ejected): cars far from it think less often.
        playProf_.begin(editor::PlayPhase::Vehicles);
        vehicles_.prePhysics(t.dt, camPos_);
        playProf_.end(editor::PlayPhase::Vehicles);
#endif
        playProf_.begin(editor::PlayPhase::Gameplay);
        playProf_.addPhysicsSteps(game::tickGameplayGroups(t.dt));
        playProf_.end(editor::PlayPhase::Gameplay);
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
        playProf_.begin(editor::PlayPhase::Vehicles);
        vehicles_.postPhysics(scene::World::instance());
        playProf_.end(editor::PlayPhase::Vehicles);
#endif
    }
#endif
#if AVER_MODULE_FLUIDS
    // After physics step, before any prePass (not gated on Play).
    // The path-traced reference traces the displaced water only while it holds still.
    water_.setFrozen(ptSceneView_ != nullptr);
    if (ptSceneView_)
        ptSceneView_->setIncludeDynamicMeshes(water_.displacedSurfaceActive(), water_.displacedSurfaceRevision());
    water_.update(*e.device(), t.dt);
    hm.mark("water");
#endif
#if AVER_MODULE_SCENE
    // Retires deferred destroys and propagates world matrices once, after gameplay and before onRender.
    // Animation clock runs UNCONDITIONALLY (not gated on PLAYING; hanging it there would freeze previews outside Play mode).
#if AVER_MODULE_FRAMEWORK
    // Object animation follows Play's pause: held while PAUSED, lifted by Frame Skip.
    // Set every frame from play state so Stop or level change never leaves it stuck.
    anim::animSystem().setObjectAnimationPaused(aver_fw_play_state() == AVER_FW_PLAY_PAUSED);
#endif
    // [wave5/sequencer] The sequence's Pre phase first: a pose chosen by the render queue, the clock, and the
    // property / visibility / animation / audio / event tracks, so a driven montage lands in the tick below.
    tickSequencePre(e, t.dt);
    playProf_.begin(editor::PlayPhase::ObjectAnim);
    game::tickAnimGraphs(t.dt);   // state machines write the pose the clip sampler would
#if AVER_MODULE_RAGDOLL
    ragdoll::ragdollSystem().tick(t.dt);   // after the physics step, before the pose it writes into
#endif
    anim::animSystem().tick(scene::World::instance(), t.dt);
    playProf_.end(editor::PlayPhase::ObjectAnim);
    hm.mark("objectAnim");
#if AVER_MODULE_SOFTBODY_SCENE
    // After the pose it reads (skin palette), before the draw scene copies the result (plan 2.5.2).
    playProf_.begin(editor::PlayPhase::SoftBody);
    game::tickSoftBodies(t.dt, viewPos());
    playProf_.end(editor::PlayPhase::SoftBody);
    hm.mark("softBodies");
#endif
    // The level sequence's Post phase (Animate preview, Play), after the clips so a sequenced actor wins.
    tickSequencePost();
    hm.mark("sequence");
#if AVER_MODULE_PHYSICS
    // After the tick that moved them: animated placement's kinematic body follows it (carries standing characters).
    playProf_.begin(editor::PlayPhase::DriveBodies);
    driveAnimatedBodies(t.dt);
    playProf_.end(editor::PlayPhase::DriveBodies);
    hm.mark("driveBodies");
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Unconditionally (like animation clock): previews outside Play should still show effects.
    {
        const auto tickStart = std::chrono::steady_clock::now();
        particles::particleSystem().tick(scene::World::instance(), t.dt);
        particleTickAccumSec_ += std::chrono::duration<f64>(std::chrono::steady_clock::now() - tickStart).count();
        ++particleTickFrames_;
    }
#endif
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
    // Once per frame, before render features run (ThumbnailCache.hpp): update() points preview at next pending request.
    if (thumbnails_.ready()) thumbnails_.update();
    // --drone: switches drone on N frames in (mirroring --chunk-stream so a capture can prove it without clicking Window > Drone).
    if (droneAutoFrames_ > 0 && --droneAutoFrames_ == 0) setDroneEnabled(true);
    // --undo-test: fires runUndoTest() N frames in. Lives inside `#if AVER_WITH_IMGUI` block (same as commands it proves).
#if AVER_WITH_IMGUI
    if (undoTestAutoFrames_ > 0 && --undoTestAutoFrames_ == 0) runUndoTest(e);
    if (classPlaceTestAutoFrames_ > 0 && --classPlaceTestAutoFrames_ == 0) runClassPlaceTest(e);
    if (multiSelTestFrames_ > 0 && --multiSelTestFrames_ == 0) runMultiSelectTest(e);
    if (cbMoveTestFrames_ > 0 && --cbMoveTestFrames_ == 0) runCbMoveTest(cbMoveTestDir_);
    if (saveDirtyTestFrames_ > 0 && --saveDirtyTestFrames_ == 0) runSaveDirtyTest();
    if (findRefsFrames_ > 0 && --findRefsFrames_ == 0) runFindRefs();
    if (projectSwitchFrames_ > 0 && --projectSwitchFrames_ == 0) runProjectSwitchTest();
    if (validateGraphFrames_ > 0 && --validateGraphFrames_ == 0) runValidateGraph();
    if (graphPrintTestFrames_ > 0 && --graphPrintTestFrames_ == 0) runGraphPrintTest();
    if (clearShaderCacheFrames_ > 0 && --clearShaderCacheFrames_ == 0) runClearShaderCache();
    if (assetAssignTestFrames_ > 0 && --assetAssignTestFrames_ == 0) runAssetAssignTest();
    if (graphHitsTestFrames_ > 0 && --graphHitsTestFrames_ == 0) runGraphHitsTest();
    if (renameRepointFrames_ > 0 && --renameRepointFrames_ == 0) runRenameRepointTest();
    if (prefsWriteTestFrames_ > 0 && --prefsWriteTestFrames_ == 0) runPrefsWriteTest();
    if (notifyTestFrames_ > 0 && --notifyTestFrames_ == 0) runNotifyTest();
    // Armed once: markLevelUnsaved gives autosave timer something to do; re-marking would loop forever.
    if (autosaveTestArm_) { autosaveTestArm_ = false; markLevelUnsaved(); }
    if (keybindTestAutoFrames_ > 0 && --keybindTestAutoFrames_ == 0) runKeybindPersistTest(keybindTestMode_);
#endif
#if AVER_MODULE_SCRIPTING
    // Ticks the graph-driven drone if live. Runs before chunk streaming so it sees this frame's drone position/velocity.
    if (droneEntity_ != scene::kInvalidEntity && droneGraphLoaded_) {
        droneTimeSeconds_ += t.dt;
        scripts_.graphTick(static_cast<i32>(droneEntity_), droneTimeSeconds_);
        scene::World& dw = scene::World::instance();
        if (dw.valid(droneEntity_)) {
            dronePos_ = dw.localTransform(droneEntity_).position;
            const f32 droneInvDt = t.dt > 1e-6f ? 1.0f / t.dt : 0.0f;
            droneVel_ = droneHaveLastPos_ ? (dronePos_ - droneLastPos_) * droneInvDt
                                          : Vec3{0.0f, 0.0f, 0.0f};
            droneLastPos_ = dronePos_;
            droneHaveLastPos_ = true;
            // Greppable proof the drone MOVED across several frames (not just spawned).
            if (droneLogsLeft_ > 0) {
                --droneLogsLeft_;
                AVER_INFO("[Drone] t={:.3f}s pos=({:.1f},{:.1f},{:.1f}) vel=({:.1f},{:.1f},{:.1f})cm/s",
                          droneTimeSeconds_, dronePos_.x, dronePos_.y, dronePos_.z,
                          droneVel_.x, droneVel_.y, droneVel_.z);
            }
        }
    }
    // Graph-as-class instances gated on Play. HostBridge registers graph classes into native class registry.
    // Ungated: every class-placed graph ran OnTick while browsing, mutating level. Need FRAMEWORK guard for aver_fw_play_state().
#if AVER_MODULE_FRAMEWORK
    playProf_.begin(editor::PlayPhase::GraphTicks);
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING)
        scripts_.tickGraphClassInstances(t.dt);
    playProf_.end(editor::PlayPhase::GraphTicks);
    hm.mark("graphTicks");
#endif
#endif
    // --chunk-stream: switches streaming on N frames in so a capture can prove it happened.
    if (chunkStreamAutoFrames_ > 0 && --chunkStreamAutoFrames_ == 0) setChunkStreamingEnabled(true);
    // Chunk streaming. Runs here so it sees THIS frame's camPos_ and evictions land in flush() below.
    // Runs while idling outside Play too (exactly who this feature is for).
    if (streaming_.enabled()) {
        const game::GameStreaming::TriangleLookupFn tris = [this](u64 id) -> u32 {
            const auto it = meshTris_.find(id);
            return it != meshTris_.end() ? it->second : 0u;
        };
#if AVER_MODULE_SCRIPTING
        // Drone keeps its own corridor resident while flying (both are StreamSource entries).
        if (droneEntity_ != scene::kInvalidEntity && droneGraphLoaded_) {
            const world::StreamSource drone{dronePos_, droneVel_};
            streaming_.tick(camPos_, t.dt, &drone, tris);
        } else
#endif
        streaming_.tick(camPos_, t.dt, nullptr, tris);
    }
#if AVER_MODULE_SCENE
    tickLevelStreaming(t.dt);
    hm.mark("levelStreaming");
#endif
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
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        game::tickAi(t.dt, &nav_);
    }
#endif
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    // Ejected: leave camPos_/yaw_/pitch_ where the user flew them (drivePlayCamera snaps view to pawn every frame) and clear owner-hide root.
    // Re-possessing calls drivePlayCamera() again next frame.
    if (playEjected()) firstPersonPawn_ = scene::kInvalidEntity;
    else {
        drivePlayCamera();
#if AVER_MODULE_FRAMEWORK
        if (playSessionActive()) game::applyCameraFx(camPos_, yaw_, pitch_, t.dt);   // the player's shake/bob scales, Play camera only (never the free-fly camera)
#endif
    }
#endif
    // A sequence camera (Animate's pilot view, or Play's camera track) replaces the view for this
    // frame only: camPos_/yaw_/pitch_ are the persisted editor camera and stay untouched.
    viewOverride_ = false;
#if AVER_MODULE_SCENE
    {
        game::SeqCameraPose sp;
        if (seqEditor_.viewPose(playEjected(), sp)) {
            viewOverride_ = true;
            viewPosOv_ = sp.position;
            viewFwdOv_ = game::cameraForward(sp.yaw, sp.pitch);
            // The pawn is in view now, not behind the eye.
            if (seqEditor_.playRunning()) firstPersonPawn_ = scene::kInvalidEntity;
        }
    }
#endif
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
    const bool wantGbufForTaa = temporalAaEnabled_ || (neuraaEnabled_ && rayPrimaryFrame);
#else
    const bool wantGbufForTaa = false;
#endif
    e.device()->setGBufferEnabled(gbufferOverride_ || wantGbufForDenoiser || wantGbufForFrameInterp ||
                                  wantGbufForTaa || neuraaDebugView_ ||
                                  gbufferDebugView_ != GBufferDebugFeature::Mode::Off);
    gbufferDebugFeature_.setDevice(e.device());
    // vpX_/vpY_/vpW_/vpH_: this frame's 3D-viewport rect (frame stale at worst on first draw).
    gbufferDebugFeature_.setViewportRect(static_cast<u32>(vpX_), static_cast<u32>(vpY_),
                                         static_cast<u32>(vpW_), static_cast<u32>(vpH_));
    gbufferDebugFeature_.setMode(gbufferDebugView_);
#if AVER_MODULE_VOXI
    e.device()->setMeshShaders(voxi::Renderer::get().settings().meshShaders);
#else
    e.device()->setMeshShaders(msOverride_);
#endif
#if AVER_MODULE_VOXI
    // Voxi owns the AA setting; push it when it changes (rebuilds targets+PSOs). Ray-driven frames run
    // at 1x whatever MSAA says (voxi::Resolution::sampleCount).
    {
        const u32 samples = voxi::resolve(vx.settings(), vx.deviceInfo()).sampleCount;
        if (vx.consumeMsaaDirty() || samples != msaaPushed_) {
            e.device()->setSampleCount(samples);
            msaaPushed_ = samples;
        }
    }

    if (voxiAttached_) {
        // Copy, load-bearing: must never write back to the singleton.
        voxi::Settings vs = voxi::Renderer::get().settings();
        frameBudgetTick(t.dt, vs);
        const f32 c[3] = {giCenter_.x, giCenter_.y, giCenter_.z};
        // Auto-switch: ray-hit/triangles debug views can't draw through the rasteriser; Wireframe/G-buffer can't draw through ray-driven.
        const bool needRaster = wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
        const bool needRayDriven = !needRaster &&
            (debugView_ != voxi::VoxiRenderer::ViewDebug::None || neuraaDebugView_) &&
            voxiRenderer_.rayDrivenAvailable();
        // Path Tracing forces ray-driven primary (VoxiRenderer::setSettings), so a raster view mode or
        // the --pt-scene reference view turns it off for the frame.
        if (needRaster || ptTakesViewport()) { vs.rtRenderMode = 0; vs.pathTracing = voxi::Quality::Off; }
        else if (needRayDriven) vs.rtRenderMode = 1;
        // Reset history when renderer mode changes (compared against last frame's effective mode).
        if (static_cast<i32>(vs.rtRenderMode) != lastEffectiveRtRenderMode_) {
            voxiRenderer_.resetRtHistory(true);
            voxiRenderer_.resetAoHistory();
            voxiRenderer_.resetGiHistory(true);
            voxiRenderer_.resetDenoiserHistory(true);
            lastEffectiveRtRenderMode_ = static_cast<i32>(vs.rtRenderMode);
        }
        // Undenoised mode: toggles denoiser, ray-tile amortisation, shadow denoise, GI spatial reuse.
        if (undenoised_) {
            vs.denoiser = false;
            vs.rtPixelsPerRayTile = 1;
            vs.rtShadowDenoise = 0;
            vs.giRestirSpatialSamples = 0;
        }
        // A heavy mode switch is announced a frame before it is applied (ModeSwitchNotice.hpp).
        static editor::ModeSwitchNotice modeNotice;
        if (modeNotice.shouldApply(vs, e.window() != nullptr && maxFrames_ == 0, &shaderWarmup_))
            voxiRenderer_.setSettings(vs);
        // Consume-and-forward: console reset commands routed through singleton flags to the renderer instance.
        if (voxi::Renderer::get().consumeGiHistoryResetRequest())  voxiRenderer_.resetGiHistory();
        if (voxi::Renderer::get().consumeRtHistoryResetRequest())  voxiRenderer_.resetRtHistory();
        if (voxi::Renderer::get().consumeAoHistoryResetRequest())  voxiRenderer_.resetAoHistory();
        if (voxi::Renderer::get().consumeDenoiserHistoryResetRequest()) voxiRenderer_.resetDenoiserHistory();
        // Reset GI/RT/denoiser history every frame if console flag set or Undenoised active.
        const u32 everyFrame = editor::consoleResetHistoryEveryFrameSlot();
        if (everyFrame || undenoised_) {
            if ((everyFrame & 1u) || undenoised_) voxiRenderer_.resetGiHistory(/*quiet=*/true);
            if ((everyFrame & 2u) || undenoised_) voxiRenderer_.resetRtHistory(/*quiet=*/true);
            if ((everyFrame & 4u) || undenoised_) voxiRenderer_.resetDenoiserHistory(/*quiet=*/true);
        }
        voxiRenderer_.setVolume(c, giExtent_);
        // Baked volume cache directory; empty when no project open.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        voxiRenderer_.setDebugView(giDebugView_);
        // Console knob voxi.giPoisonView reasserted each frame.
        voxiRenderer_.setGiPoisonView(editor::consoleGiPoisonViewSlot());
        // Lighting-contrast legacy bitmask.
        voxiRenderer_.setLightingLegacyBits(editor::consoleLightingLegacySlot());
        // Console knobs for GI optimisations.
        voxiRenderer_.setGiForceRebuild(editor::consoleGiForceRebuildSlot());
        voxiRenderer_.setGiBoundedDispatch(editor::consoleGiBoundedDispatchSlot());
        voxiRenderer_.setGiFreeAccumulator(editor::consoleGiFreeAccumulatorSlot());
        // Console knobs for GI vis and blended cone.
        voxiRenderer_.setGiVisPathView(editor::consoleGiVisPathViewSlot());
        voxiRenderer_.setBlendedGiCone(editor::consoleBlendedGiConeSlot());
        // Neural Visualiser NeuRaC view.
        voxiRenderer_.setNeuRaCView(static_cast<u32>(neuracViewMode_ < 0 ? 0 : neuracViewMode_), neuracViewGrid_);
        // See setGiConeTraceOff's comment; applied every frame.
        voxiRenderer_.setConeTraceEnabled(!giConeTraceOff_);
#if AVER_MODULE_SR
        // AverSR level resolved fresh each frame from precedence chain.
        updateAverSrAuto(e);
        applyUpscalerSlot(e.device());
#endif
#if AVER_MODULE_FRAMEWORK
        // Play only: a script's sun direction reaches the GI too (assets/LevelSky.hpp); authoring is untouched.
        rhi::SkyAtmosphere skySunScratch;
        const rhi::SkyAtmosphere& skySun = playSessionActive() ? assets::withScriptEnv<assets::FrameworkWorldFxApi>(sky_, skySunScratch) : sky_;
#else
        const rhi::SkyAtmosphere& skySun = sky_;
#endif
        const Vec3 sd = Vec3{skySun.sunDirection[0], skySun.sunDirection[1],
                             skySun.sunDirection[2]}.getSafeNormal();
        if (sunAngle_ > 0.0f) sky_.sunAngularDiameterDeg = sunAngle_;
        // Direction only; color/ambient reach shaders through device frame constants.
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif
    // Confine the scene to the dockspace's central node (latched by buildUI last frame).
    e.device()->setViewportRect((u32)vpX_, (u32)vpY_, (u32)std::fmax(1.0f, vpW_), (u32)std::fmax(1.0f, vpH_));

    const Vec3 fwd = viewForward();
    const f32 aspect = viewAspect();
    const game::CameraMatrices cam = game::pushCamera(*e.device(), viewPos(), fwd, aspect);
    invVP_ = cam.invVP; viewProj_ = cam.viewProj; eye_ = viewPos();

    // One member for one value; fixes dead code in fog handling.
    f32 fog = fogDensity_;
#if AVER_MODULE_SCENE
    // Opt-in: match fog density to streaming load boundary (foggier, only when asked).
    if (matchFogToStreamRadius_ && streaming_.enabled()) {
        const world::StreamSettings& st = streaming_.settings().stream;
        const f32 boundaryCm = static_cast<f32>(st.loadRadius) * static_cast<f32>(st.chunkSizeCm);
        const f32 matched = fogDensityForOpacityAt(boundaryCm, fogMatchTargetOpacity_);
        if (matched > 0.0f) fog = matched;
    }
#endif
    // sunDirection stays unnormalised; shaders normalise it.
    sky_.enabled = showAtmosphere_;   // Show > Atmosphere; the backend already gates the sky draw on this
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
    // --sky-light overrides the level, reapplied here since applyLevelSky overwrites sunAmbient_ on level open.
    if (skyLightOverride_ >= 0.0f) sunAmbient_ = skyLightOverride_;
    // --sky-physical/--sky-authored/sun elevation, reapplied here for the same reason.
    if (skyModelOverride_ >= 0)
        sky_.model = skyModelOverride_ ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
    if (sunElevationOverride_ > -90.0f) {
        f32 elev = 0.0f, azim = 0.0f;
        sky_.sunAngles(elev, azim);
        // Azimuth unchanged; flag names elevation only, silently rotating the sun would make runs differ.
        sky_.setSunAngles(sunElevationOverride_, azim);
    }
    sky_.skyLightIntensity = sunAmbient_;
#if AVER_MODULE_SCENE
    // A streamed level that hides its load edge in fog (World Settings > Level Streaming).
    if (level_.streaming().active()) fog = assets::fogForStreamEdge(levelHeader_.stream, sky_, fog, eye_.z);
#endif
    sky_.fogDensity = fog;
    sky_.cloudTime = cloudTime_;
    // Underwater fog applied to a copy; sky_ is the authored sky and must stay unchanged.
    // Script sun / fog / sky-light overrides ride a copy, in a Play session only: sky_ stays the authored sky.
#if AVER_MODULE_FRAMEWORK
    rhi::SkyAtmosphere skyScriptScratch;
    const rhi::SkyAtmosphere& skyNow = playSessionActive() ? assets::withScriptEnv<assets::FrameworkWorldFxApi>(sky_, skyScriptScratch) : sky_;
#else
    const rhi::SkyAtmosphere& skyNow = sky_;
#endif
#if AVER_MODULE_FLUIDS
    e.device()->setSkyAtmosphere(water_.applyUnderwaterFog(skyNow, viewPos().z));
#else
    e.device()->setSkyAtmosphere(skyNow);
#endif
    // Outside the viewport rect is editor chrome, not sky.
    e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
    hm.mark("deviceSetup");
    // Console post.* vars write the device, not post_; adopt device changes into post_ before pushing.
    if (postPushedValid_ && !rhi::postSettingsEqual(e.device()->postProcess(), postPushed_))
        post_ = e.device()->postProcess();
    // Debug views paint flat diagnostic colours; disable auto-exposure/bloom/local exposure.
    if (debugViewActiveThisFrame) {
        rhi::PostSettings dbg = post_;
        dbg.autoExposure = false;
        dbg.exposure = 1.0f;
        dbg.bloomIntensity = 0.0f;
        dbg.localExposureShadows = 0.0f;
        dbg.localExposureHighlights = 0.0f;
        e.device()->setPostProcess(dbg);
        postPushed_ = dbg;
    } else {
        // The level's post volumes blend onto a copy: post_ stays the base the panels edit (POST_VOLUMES.md).
        rhi::PostSettings shown = postVolumeTools_.blend(*e.device(), project_.contentDir(),
                                                              levelHeader_.postVolumes, post_, viewPos());
        // The player's accessibility choices win over the level's volumes, in a Play session only (a saved option must not change the
        // authoring view); the editor's colour-vision preview fills in when no filter is set. Scene only: the editor chrome draws after.
        if (playSessionActive()) game::applyAccessibilityToPost(shown, access_.snap);
        editor::applyPreviewToPost(accessPreview_, shown);
        e.device()->setPostProcess(shown);
        postPushed_ = shown;
    }
    postPushedValid_ = true;
}

} // namespace aver
