#include "GameAppInternal.hpp"

namespace aver::game {

namespace {

// M7 (C-7): one shared [Stats] video-memory line for both the periodic and one-shot end-of-run dumps
// in onUpdate, so they can't drift apart. Mirrors SandboxApp.cpp's gpuTimingCheck [GPU] line: same MB
// rounding (bytes/1048576) as the RHI backends' own init-time lines. `supported` false (D3D11, no
// VK_EXT_memory_budget, mocks) prints a distinct sentence rather than a row of zeros that could read
// as "nothing in use".
void logStatsVideoMemory(rhi::IDevice& dev) {
    const rhi::VideoMemoryInfo vm = dev.videoMemory();
    if (vm.supported) {
        AVER_INFO("[Stats] video memory: local {} MB used of {} MB budget, non-local {} MB used of {} MB budget",
                  vm.localUsageBytes / 1048576, vm.localBudgetBytes / 1048576,
                  vm.nonLocalUsageBytes / 1048576, vm.nonLocalBudgetBytes / 1048576);
    } else {
        AVER_INFO("[Stats] video memory: not reported by this backend");
    }
}

} // namespace

void GameApp::pushFrame(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // No setViewportRect: games render to the whole backbuffer, unlike the editor's dockspace.
    //
    // --cam-wobble DEG PERIOD: sinusoidal offset, applied temporarily to yaw_ for this call only.
    f32 wobble = 0.0f;
    if (cfg_.camWobbleDeg != 0.0f && cfg_.camWobblePeriod != 0) {
        wobble = cfg_.camWobbleDeg * 0.01745329252f *
                 std::sin(6.2831853f * static_cast<f32>(frames_ - 1) / static_cast<f32>(cfg_.camWobblePeriod));
    }
    const f32 yawBeforeWobble = yaw_;
    yaw_ += wobble;
    const Vec3 fwd = camForward();
    yaw_ = yawBeforeWobble;
    const f32  aspect = viewAspect(e);
    const game::CameraMatrices cam = game::pushCamera(*dev, camPos_, fwd, aspect, playView_);
    invVP_ = cam.invVP; viewProj_ = cam.viewProj; eye_ = camPos_;

    // Log once: verifies aspect ratio calculation.
    if (frames_ <= 1) {
        AVER_INFO("[Game] camera: aspect={:.3f} fov={:.1f}deg near={} far={} (from the swapchain, not a viewport rect)",
                  aspect, degrees(playView_.fovYRad), playView_.nearCm, playView_.farCm);
    }

    // One member for one value: applyLevelSky seeds fogDensity_.
    const f32 fog = fogDensity_;
    // sunDirection stays unnormalised; shaders normalise it.
    sky_.enabled = true;
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
#if AVER_MODULE_SCENE
    // PCGVOLUME "Sky" drives clouds: seed picks the sky, coverage floor becomes cloud cover (inverted).
    if (const GameLevel::PcgField* skyField = level_.pcgField("Sky")) {
        sky_.cloudsEnabled = true;
        sky_.cloudSeed     = skyField->infinite ? skyField->infiniteSpec.seed
                                                : skyField->boundedSpec.seed;
        const f32 floorV = skyField->infinite ? skyField->infiniteSpec.coverageFloor
                                              : skyField->boundedSpec.coverageFloor;
        sky_.cloudCoverage = 1.0f - (floorV < 0.0f ? 0.0f : (floorV > 1.0f ? 1.0f : floorV));
        if (frames_ <= 1)
            AVER_INFO("[PCG] sky field '{}' parameterises the cloud layer: seed {}, coverage {:.2f}",
                      skyField->name, sky_.cloudSeed, sky_.cloudCoverage);
    }
#endif
#if AVER_MODULE_FRAMEWORK
    // Script sky wins over level sky when published; read every frame for hot-reload.
    {
        i32 seed = 0;
        f32 coverage = 0.0f, density = 0.0f, bottom = 0.0f, top = 0.0f, scale = 0.0f;
        f32 windX = 0.0f, windY = 0.0f;
        if (aver_fw_sky_clouds(&seed, &coverage, &density, &bottom, &top, &scale, &windX, &windY)) {
            sky_.cloudsEnabled = true;
            sky_.cloudSeed     = seed;
            sky_.cloudCoverage = coverage;
            sky_.cloudDensity  = density;
            sky_.cloudBottom   = bottom;
            sky_.cloudTop      = top;
            sky_.cloudScale    = scale;
            sky_.cloudWind[0]  = windX;
            sky_.cloudWind[1]  = windY;
            if (!scriptSkyReported_) {
                scriptSkyReported_ = true;
                AVER_INFO("[Sky] a script owns the cloud layer: seed {}, coverage {:.2f}, "
                          "{:.0f}..{:.0f} cm", seed, coverage, bottom, top);
            }
        } else if (scriptSkyReported_) {
            scriptSkyReported_ = false;
            AVER_INFO("[Sky] the script released the cloud layer; the level's sky is back");
        }
    }
#endif
    sky_.skyLightIntensity = sunAmbient_;
    sky_.fogDensity        = assets::fogForStreamEdge(level_.streamSettings(), sky_, fog, camPos_.z);
    // Cloud clock: advanced by real time so wind moves. App owns it; a clock that never advances gives a painted backdrop.
    sky_.cloudTime         = cloudTime_;

    // Script sun / fog / sky-light overrides ride a copy: sky_ stays the level's authored atmosphere, so a released key
    // is the level's own value again with nothing to restore. The editor's Play path does the same (assets::withScriptEnv).
#if AVER_MODULE_FRAMEWORK
    const rhi::SkyAtmosphere& skyNow = assets::withScriptEnv<assets::FrameworkWorldFxApi>(sky_, skyScript_);
#else
    const rhi::SkyAtmosphere& skyNow = sky_;
#endif
    skyForViews_ = skyNow;   // the sun and fill the sub-viewports shade with (G4 R5)

#if AVER_MODULE_VOXI
    if (voxiAttached_) {
        voxi::Renderer& vx = voxi::Renderer::get();
        // G-buffer switch reads denoiserGBufferWanted; MSAA push consumes consumeMsaaDirty().
        // Frame interpolation: wants motion and depth from G-buffer. Reads --frame-interp over RENDER.FRAMEINTERP.
        bool wantFrameInterp = cfg_.frameInterpArg >= 0 ? cfg_.frameInterpArg == 1 : project_.frameInterp == 1;
        if (wantFrameInterp && !frameInterpolator_) {
            if (dev->resources()) {
                auto fg = std::make_unique<neurafi::NeuraFI>(*dev);
                // --frame-interp-trajectory; neural reads per-machine file or shipped weights.
                fg->setTrajectory(static_cast<neurafi::Trajectory>(cfg_.frameInterpTrajectory));
                fg->setWeightsPath(userDataDir() + AVER_PATH_SEP + neurafi::NeuraFI::kWeightsFileName);
                fg->setShippedWeightsPath(executableDir() + AVER_PATH_SEP "data" AVER_PATH_SEP +
                                          neurafi::NeuraFI::kWeightsFileName);
                frameInterpolator_ = std::move(fg);
                dev->setFrameInterpolator(frameInterpolator_.get());
            } else {
                wantFrameInterp = false;
            }
        }
        dev->setFrameInterpolation(wantFrameInterp);
        // TAAU's resolve reads velocity and view Z.
        dev->setGBufferEnabled(voxi::resolve(vx.settings(), vx.deviceInfo()).denoiserGBufferWanted ||
                               wantFrameInterp || temporalAa_);
        // Ray-driven frames run at 1x whatever MSAA says (voxi::Resolution::sampleCount).
        {
            const u32 samples = voxi::resolve(vx.settings(), vx.deviceInfo()).sampleCount;
            if (vx.consumeMsaaDirty() || samples != msaaPushed_) {
                dev->setSampleCount(samples);
                msaaPushed_ = samples;
            }
        }

        // LEVEL's GI volume, fitted once at load; not camera-following (would re-voxelise every move).
        voxiRenderer_.setVolume(&giCenter_.x, giExtent_);
        // Reads editor's cache; never writes a miss back. A level without a baked volume voxelises as usual.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        const Vec3 sd = Vec3{skyNow.sunDirection[0], skyNow.sunDirection[1],
                             skyNow.sunDirection[2]}.getSafeNormal();
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif

#if AVER_MODULE_FLUIDS
    // Apply underwater fog (sky stays authored).
    dev->setSkyAtmosphere(water_.applyUnderwaterFog(skyNow, camPos_.z));
#else
    dev->setSkyAtmosphere(skyNow);
#endif
    // Black: a game has no outside; anything the sky doesn't cover should read as a bug, not a colour.
    dev->setClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // Order of a frame: the level's volumes (each weight scaled by the script), then the script's post layers as infinite
    // volumes, then blendPostVolumes, then the player's accessibility override below, which wins by design.
    const std::vector<fmt::OcPostVolume>* postList = &postVolumes_;
#if AVER_MODULE_FRAMEWORK
    if (assets::buildScriptPostVolumes<assets::FrameworkWorldFxApi>(postVolumes_, postScript_)) postList = &postScript_;
#endif
    rhi::PostSettings postNow = postList->empty()
                                    ? post_
                                    : blendPostVolumes(*postList, post_, camPos_, postLuts_.resolver(*dev, project_.contentDir()));
    game::applyAccessibilityToPost(postNow, access_.snap);   // the player's colour filter and motion switches win over the level's volumes
    dev->setPostProcess(postNow);
}

void GameApp::beginFrame(Engine& e, const Timestep& t) {
    (void)e;
#if AVER_MODULE_SCENE
    tickOrigin(e);   // S0: the only place the floating origin shifts (LARGE_WORLD 2.2.4); before input, gameplay, physics, streaming, flush
#endif
    ++frames_;
    cloudTime_ += t.dt;
#if AVER_MODULE_FRAMEWORK
    aver_fw_worldfx_tick(t.dt);   // advances the script post layers' and volume scales' blends
#endif

#if AVER_WITH_AUDIO_ABI
    // Reclaim finished voice slots. aver_audio_collect had one caller (editor); needed here to avoid leaks in shipped game.
    aver_audio_collect();
#endif
}

void GameApp::dumpStatsIfDue(Engine& e, const Timestep& t) {
    // Profiler dump: see GameConfig::statsIntervalSec. First dump at full interval, not immediately.
    //
    if (cfg_.statsIntervalSec > 0.0f && e.device()) {
        statsTimer_ += t.dt;
        if (statsTimer_ >= cfg_.statsIntervalSec) {
            statsTimer_ = 0.0f;
            const f64 gpuMs = rhi::formatGpuTiming(e.device()->gpuTiming(),
                                                   [](const std::string& line) { AVER_INFO("[Stats] {}", line); });
            // Rough GPU-bound/CPU-bound signal: different sources, not a matched pair.
            if (gpuMs > 0.0)
                AVER_INFO("[Stats] GPU total (marked passes): {:.2f}ms  |  CPU frame (this instant): "
                          "{:.2f}ms -- a rough bound signal, not a matched pair.",
                          gpuMs, static_cast<f64>(t.dt) * 1000.0);
            logStatsVideoMemory(*e.device());
        }
    }
    // End-of-run dump: distinct from periodic one. Bounded run (--frames N) may end before statsIntervalSec elapses.
    if (cfg_.statsIntervalSec > 0.0f && cfg_.maxFrames > 0 && !statsFinalDumped_ && e.device()) {
        const u64 want = cfg_.maxFrames > 8 ? cfg_.maxFrames - 2 : cfg_.maxFrames - 1;
        if (frames_ >= want) {
            statsFinalDumped_ = true;
            const f64 gpuMs = rhi::formatGpuTiming(e.device()->gpuTiming(),
                                                   [](const std::string& line) { AVER_INFO("[Stats] {}", line); });
            if (gpuMs > 0.0)
                AVER_INFO("[Stats] GPU total (marked passes): {:.2f}ms  |  CPU frame (this instant): "
                          "{:.2f}ms -- a rough bound signal, not a matched pair.",
                          gpuMs, static_cast<f64>(t.dt) * 1000.0);
            logStatsVideoMemory(*e.device());
        }
    }
}

void GameApp::updateInput(Engine& e) {
    (void)e;
    // Input is READ here, never rolled here. See onRender for why.
#if AVER_MODULE_FRAMEWORK
    // Quit is a REQUEST, not immediate exit: allows device/audio/physics to stay live. requestExit() sets flag Engine::run checks.
    if (aver_fw_quit_requested()) e.requestExit();
    // A script's level change, between frames: before this frame's gameplay sees the old level.
    {
        char travel[1024];
        if (aver_fw_take_travel_request(travel, static_cast<int32_t>(sizeof travel)) > 0) travelTo(travel);
    }
    game::pumpContentPrefetch(content_);   // a script's "this is about to be shown": its textures start decoding now
    // A script's present mode (Display.VSync / Display.PresentMode), the frame cap, and its window requests, applied where the device and
    // the window allow it. The state is published back for the script to read.
    if (rhi::IDevice* dev = e.device()) game::applyPresentMode(*dev);
    e.setMaxFps(static_cast<f32>(aver_fw_max_fps()));
    if (window_) game::applyWindowRequests(*window_);
    // Foreground: asked once for cursor confinement and player drive input. Falls back to "a window exists" (no portable Win32 query).
    bool foreground = e.window() != nullptr;
#if defined(_WIN32)
    if (window_) {
        HWND hwnd = static_cast<HWND>(window_->nativeHandle());
        foreground = hwnd && ::GetForegroundWindow() == hwnd;
    }
#endif
    // Mouse capture: BEFORE publishInput (SandboxPlay order). Only while playing AND foreground. Never on bounded/--no-mouse-capture runs.
    {
        bool wantCapture = false;
#if defined(_WIN32)
        wantCapture = foreground && window_ && cfg_.maxFrames == 0 && !cfg_.noMouseCapture &&
                      aver_fw_play_state() == AVER_FW_PLAY_PLAYING;
#endif
        // Framework ABI: free cursor when aver_fw_cursor_requested()==1 regardless of play state.
        if (aver_fw_cursor_requested()) wantCapture = false;
        // An open menu frees the cursor so the player can reach it (retained widgets, docs/GAME_UI.md).
        const bool cursorFreed = aver_fw_cursor_requested() != 0 || game::uiWantsCursor();
        if (cursorFreed) wantCapture = false;
        // A script's pointer mode replaces the rule above (AUTO keeps it): the same gates -- a foreground window, no
        // bounded run, no --no-mouse-capture -- and the same two things that free the cursor.
        const bool pointerGate = foreground && window_ && cfg_.maxFrames == 0 && !cfg_.noMouseCapture;
        const game::PointerPlan plan = pointerGate ? game::resolvePointerPlan(aver_fw_pointer_mode(), wantCapture, cursorFreed)
                                                   : game::PointerPlan{};
        setMouseCaptured(pointerGate ? plan.captured : wantCapture);
        pollCapturedMouse();
        if (window_) cursorHost_.apply(*window_, plan);
    }
    // Before gameplay tick: PrePhysics actors read THIS frame's input. Bounded run keeps old answer (window opens unactivated).
    //
    const bool inputFocused = cfg_.maxFrames > 0 ? (e.window() != nullptr) : foreground;
    InputPublishPolicy inputPolicy;
    inputPolicy.focused = inputFocused;
    // All three device gates: shipped game has no panels, so "window in front" answers all at once.
    inputPolicy.keyboardToGame = inputFocused;
    inputPolicy.mouseToGame    = inputFocused;
    inputPolicy.gamepadActive  = inputFocused;
    inputPolicy.captured   = mouse_.captured();
    inputPolicy.capturedDx = mouse_.dx();
    inputPolicy.capturedDy = mouse_.dy();
    // inputPolicy.eaten stays all-false: only editor's drawer uses chords.
    // An open menu takes the keyboard, mouse and pad away from gameplay until it closes.
    game::uiGateInput(inputPolicy);
    publishInput(input_, inputPolicy, cfg_.inputEcho ? &echoHeld_ : nullptr);
    if (cfg_.inputEcho && echoHeld_ != echoLast_) {
        AVER_INFO("[Game] input: {}", echoHeld_);
        echoLast_ = echoHeld_;
    }
#endif
}

void GameApp::updateUi(Engine& e, const Timestep& t) {
    (void)e;
    (void)t;
#if AVER_WITH_UI_ABI
    // UI frame opens before gameplay ticks. Rect is the whole window. Opened even with no gameUi_.
    {
        const u32 uiW = e.window() ? e.window()->width()  : cfg_.width;
        const u32 uiH = e.window() ? e.window()->height() : cfg_.height;
        aver_ui_begin_frame(0.0f, 0.0f, static_cast<f32>(uiW), static_cast<f32>(uiH));
    }
    // Font lent per frame (mirrors SandboxApp). loadGameUiFont is non-fatal.
    aver_ui_set_font(&uiFont_);
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Sub-viewports shown over the window (SubViewportContainer, split screen): first in the frame's list, so the HUD draws over them.
    {
        game::SubViewportHost::OnScreenView sv[16];
        const u32 svCount = subViews_.onScreen(sv, 16);
        float uiRect[4] = {0, 0, 0, 0};
        if (svCount) aver_ui_viewport(uiRect);   // a view's rectangle is in the UI frame's own space
        for (u32 i = 0; i < svCount; ++i)
            aver_ui_textured_rect(uiRect[0] + sv[i].x, uiRect[1] + sv[i].y, sv[i].w, sv[i].h, render::ui::UiRenderer::textureId(sv[i].texture),
                                  0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
    }
#endif
    // Pointer: no offset needed. input_.mouseX()/mouseY() are already window-relative. Captured session has no pause menu.
    {
        u32 buttons = 0;
        if (input_.mouseHeld(0)) buttons |= 1u;   // left
        if (input_.mouseHeld(1)) buttons |= 2u;   // right
        if (input_.mouseHeld(2)) buttons |= 4u;   // middle
        aver_ui_set_pointer(static_cast<f32>(input_.mouseX()), static_cast<f32>(input_.mouseY()), buttons);
    }
#if AVER_MODULE_SCRIPTING
    // HUD draw call: "Draw(dt) into whatever rect aver_ui_begin_frame last established". Index 0 is first-declared [AverHud].
    if (scriptsReady_ && scripts_.hudCount() > 0) scripts_.hudDraw(0, t.dt);
#endif
    // Retained widgets: this frame's keyboard/pad/wheel in, then lay out, update, raise events, draw.
#if AVER_MODULE_FRAMEWORK
    // Accessibility: this frame's options, captions (unscaled time: they follow audio) and narration, pushed to the UI before it lays out.
    game::accessibilityFrame(t.dt, camPos_, yaw_, pitch_, access_);
#endif
    game::pushAccessibilityToUi(access_);
    if (gameUi_) gameUi_->setColourVision(access_.snap.colourVisionPacked);
    game::uiFeedInput(input_, t.dt, !mouse_.captured());
    aver_ui_widgets_frame(t.dt);
#endif
}

void GameApp::tickWater(Engine& e, f32 gameDt) {
#if AVER_MODULE_FLUIDS
    // After physics step: spawns, reads solver particles, pushes player into pool. Not gated on Play.
    if (rhi::IDevice* dev = e.device()) water_.update(*dev, gameDt);
#else
    (void)e;
    (void)gameDt;
#endif
}

void GameApp::tickGraphClasses(f32 gameDt) {
#if AVER_MODULE_SCRIPTING
    // Graph-as-class instances: gated on PLAY. Graph class declares like C# one and finds GAME_MODE bit.
    // Project with no GameMode never calls aver_fw_begin_play; gating on PLAYING alone would freeze class-placed graphs.
    // gameModeDeclared_ is latched by beginPlayIfGameModeDeclared.
#if AVER_MODULE_FRAMEWORK
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING || !gameModeDeclared_) {
#endif
    scripts_.tickGraphClassInstances(gameDt);
#if AVER_MODULE_FRAMEWORK
    }
#endif
#else
    (void)gameDt;
#endif
}

void GameApp::tickScene(Engine& e, const Timestep& t, f32 gameDt, bool objectsHeld) {
    (void)e;
    (void)t;
    (void)gameDt;
    (void)objectsHeld;
#if AVER_MODULE_SCENE
    // Animation clock ticks UNCONDITIONALLY: hanging off gameplay gates would freeze it when no session is running.
    anim::animSystem().setObjectAnimationPaused(objectsHeld);
    // Level sequences, Pre phase: the clock (not while paused), then property, visibility, animation,
    // event, audio and sub-sequence tracks, so driven poses land in the animation tick below.
    if (!sequenceDirector_.empty()) {
        if (!objectsHeld) sequenceDirector_.advance(gameDt);
        sequenceDirector_.evaluate(scene::World::instance(), game::SeqPhase::Pre);
    }
    game::tickAnimGraphs(gameDt);   // state machines write the pose the clip sampler would
#if AVER_MODULE_RAGDOLL
    ragdoll::ragdollSystem().tick(gameDt);   // after the physics step, before the pose it writes into
#endif
    anim::animSystem().tick(scene::World::instance(), gameDt);
#if AVER_MODULE_SOFTBODY_SCENE
    game::tickSoftBodies(gameDt, camPos_);   // after the pose it reads, before softBodyScene_->update copies the result
#endif
    // Level sequences: Post phase (transform, material, camera); advanced and Pre-evaluated before the animation tick.
    if (!sequenceDirector_.empty()) sequenceDirector_.evaluate(scene::World::instance(), game::SeqPhase::Post);
    // Animated placements' kinematic bodies follow poses the tick just wrote.
    if (!objectsHeld)
        world::driveKinematicBodies(scene::World::instance(), level_.animatedBodies(), gameDt);
#if AVER_MODULE_PARTICLES
    // Particle system: same unconditional reasoning.
    particles::particleSystem().tick(scene::World::instance(), gameDt);
#if AVER_MODULE_FRAMEWORK
    for (scene::Entity fe : particles::particleSystem().finished())
        aver_fw_event_post("Particles.EffectFinished", static_cast<int32_t>(fe), static_cast<int32_t>(fe));   // needs framework_timers_abi.h
#endif
    particles::particleSystem().finished().clear();
#endif
    // After animation tick, before draw pass: creates per-entity skin targets and copies matrices from AnimSystem.
    // Before World::flush per this code's history; SkinnedScene.hpp says "after" -- disagreement noted.
    if (skinnedScene_) skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());
#if AVER_MODULE_RENDER_SOFTBODY
    if (softBodyScene_) softBodyScene_->update(scene::World::instance(), *e.device());
#if AVER_MODULE_RENDER_HAIR
    if (hairScene_) hairScene_->update(scene::World::instance(), *e.device());   // after HairSystem::tick (tickSoftBodies) above
#endif
#endif

#if AVER_MODULE_SCENE
    // Chunk streaming: switched on after level camera settles, stepped every frame, evictions retired by flush.
    if (chunkStreamFramesLeft_ > 0 && --chunkStreamFramesLeft_ == 0) enableChunkStreaming();
    if (streaming_.enabled()) streaming_.tick(camPos_, t.dt);
    level_.tickStreaming({camPos_});
#  if AVER_MODULE_VOXI
    if (voxiAttached_) levelFoliage_.update(camPos_, t.dt);
    // Packed instance rows are ray-traced geometry: they show only under ray-driven primary
    // visibility (rtRenderMode effective 1); otherwise those instances realise unpacked.
    if (voxiAttached_) {
        const voxi::Renderer& vxr = voxi::Renderer::get();
        level_.setPackedRenderOk(voxi::resolve(vxr.settings(), vxr.deviceInfo()).rtRenderMode.effective == 1u);
    }
#  endif
#endif
    // Retires deferred destroys, rebuilds topological order, recomposes stale world matrices.
    scene::World::instance().flush();
#if AVER_MODULE_SYNAPSE_SCENE
    // AI: gated on PLAYING, like the editor's condition. Ungated when AVER_MODULE_FRAMEWORK absent.
#if AVER_MODULE_FRAMEWORK
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
#endif
    game::tickAi(gameDt, &gameNav_);
#if AVER_MODULE_FRAMEWORK
    }
#endif
#endif
#endif
}

void GameApp::updateCamera(const Timestep& t) {
    (void)t;
    // AFTER flush, BEFORE view matrix build. Reading pawn transforms before flush gives last frame's.
    drivePlayCamera(t.dt);
#if AVER_MODULE_SCENE
    // A sequence's camera track takes the view while it plays (the pawn's body shows, as in a cutscene).
    {
        game::SeqCameraPose seqCam;
        if (sequenceDirector_.camera(seqCam)) {
            camPos_ = seqCam.position;
            yaw_ = seqCam.yaw;
            pitch_ = seqCam.pitch;
            firstPersonPawn_ = scene::kInvalidEntity;
        }
    }
#endif

#if AVER_MODULE_FRAMEWORK
    // Camera shake and head bob with the player's scales over the final camera (pawn or sequencer). Identity unless a script uses CameraFx.
    game::applyCameraFx(camPos_, yaw_, pitch_, t.dt);
#endif
}

void GameApp::pollDiagnostics() {
    // Polled after prePass. Build takes three frames by design.
    if (cfg_.pcgVolumeTest) checkPcgVolume();

    // Physics step counter: advancing by real step count, so skip test detects jumps.
    if (physSteps_ != lastReportedSteps_ &&
        (lastReportedSteps_ == 0 || physSteps_ / 600 != lastReportedSteps_ / 600)) {
        AVER_INFO("[Game] physics: {} step(s) taken", physSteps_);
        lastReportedSteps_ = physSteps_;
    }
}

void GameApp::tickFrameBudget(Engine& e, const Timestep& t) {
    (void)e;
    (void)t;
#if AVER_MODULE_VOXI
    // Frame budget: shared controller seeded from project_.frameBudgetMs in openProject.
    // Called right before pushFrame so voxiRenderer_ holds this frame's rung.
    // Never on bounded (--frames N) run: automated capture/gate must not retune quality.
    if (voxiAttached_) {
        voxi::Settings vs = voxi::Renderer::get().settings();
#  if AVER_MODULE_FRAMEWORK
        // P17: a script's render keys, at the frame boundary. Settings keys go through the renderer; the render scale is the device's.
        {
            static voxi::ScriptRenderState scriptRender;
            if (voxi::applyScriptRenderSettings<voxi::FrameworkRenderApi>(vs, scriptRender)) {
                voxi::Renderer::get().setSettings(vs);
                vs = voxi::Renderer::get().settings();
            }
            float scriptScale = 1.0f;
            if (rhi::IDevice* sdev = e.device()) {
                if (aver_fw_render_get(AVER_FW_RENDER_SCALE, &scriptScale)) {
                    sdev->setRenderScale(scriptScale);
                    scriptScaleTaken_ = true;
                } else if (scriptScaleTaken_) {
                    sdev->setRenderScale(baseRenderScale_);
                    scriptScaleTaken_ = false;
                }
            }
            // P18: the last frame's counters, counted only once a script has asked.
            if (aver_fw_render_stats_wanted()) {
                const voxi::VoxiRenderer::RenderStats rs = voxiRenderer_.renderStats();
                const int32_t out[AVER_FW_RENDER_STAT_COUNT] = {static_cast<int32_t>(rs.objects), static_cast<int32_t>(rs.triangles), static_cast<int32_t>(rs.drawCalls),
                                                                static_cast<int32_t>(rs.translucent), static_cast<int32_t>(rs.shadowDraws), static_cast<int32_t>(frames_)};
                aver_fw_render_publish_stats(out);
            }
        }
#  endif
        if (cfg_.maxFrames == 0 && voxi::frameBudgetTick(frameBudget_, t.dt, vs)) {
            AVER_INFO("[Game] frame budget {:.1f}ms: {:.1f}ms average -> rung {} "
                      "(GI every {} frame(s), {} cone(s))",
                      frameBudget_.budgetMs, frameBudget_.avgMs, frameBudget_.rung,
                      vs.giUpdateInterval, vs.giCones);
        }
        voxiRenderer_.setSettings(vs);
    }
#endif
}

void GameApp::syncRuntimeAssets(Engine& e) {
    (void)e;
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Runtime meshes (G4 R7): meshes a script built or a MultiMesh baked since last frame are uploaded before the draw walk. With none in the registry this does nothing.
    if (rhi::IDevice* rtDev = e.device()) {
        content_.syncRuntimeMeshes(*rtDev);
        // Runtime textures (E8): the pixels scripts built since last frame are uploaded and handed to the materials that name them.
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        content_.syncRuntimeTextures(*rtDev, voxiAttached_ ? &voxiRenderer_.materials() : nullptr);
#else
        content_.syncRuntimeTextures(*rtDev, nullptr);
#endif
    }
    // Sub-viewports (G4 R5): the views' draw lists and cameras are set here because their features draw in the device's beginFrame, which follows. With no
    // view in the registry this returns at once.
    {
        game::SubViewportHost::Inputs svIn;
        svIn.frame = e.time().frame;
        svIn.content = &content_;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) svIn.materials = &voxiRenderer_.materials();
#endif
        svIn.skinning = skinnedScene_.get();
        DrawWorldOptions svWalk;
#if AVER_MODULE_RENDER_SOFTBODY
        svWalk.softBodies = softBodyScene_.get();
#if AVER_MODULE_RENDER_HAIR
        svWalk.hair = hairScene_.get();
#endif
#endif
        svIn.walk = &svWalk;
        svIn.sky = &skyForViews_;
        subViews_.update(svIn);
    }
#endif
}


} // namespace aver::game
