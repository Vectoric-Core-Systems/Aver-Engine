#include "GameAppInternal.hpp"

// OK to define twice: separate binaries don't collide on symbols (unlike Image.cpp's STB_IMAGE_IMPLEMENTATION).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace aver::game {

GameApp::GameApp(GameConfig cfg) : cfg_(std::move(cfg)) {
#if AVER_MODULE_SCENE
    chunkStreamFramesLeft_ = cfg_.chunkStreamAutoFrames;
#endif
}

BootConfig GameApp::config() const {
    BootConfig b;
    b.windowTitle      = cfg_.title.c_str();
    b.windowWidth      = cfg_.width;
    b.windowHeight     = cfg_.height;
    b.maxFrames        = cfg_.maxFrames;
    b.headless         = cfg_.headless;
    b.useWarp          = cfg_.useWarp;
    b.enableDebugLayer = cfg_.debugLayer;
    b.backend          = cfg_.backend.empty() ? nullptr : cfg_.backend.c_str();

    // Read WINDOW.* from manifest (must be done before window is created).
    // Command line wins: --width/--height/--title override manifest.
    {
        std::string manifest = cfg_.projectPath;
        if (manifest.empty()) {
            const std::string beside = executableDir() + AVER_PATH_SEP "Game.ocproject";
            std::error_code ec;
            if (std::filesystem::exists(beside, ec)) manifest = beside;
        }
        if (!manifest.empty()) {
            fmt::ProjectDesc d;
            std::string why;
            if (fmt::loadOcproject(manifest, d, &why)) {
                if (cfg_.title.empty() || cfg_.title == GameConfig::kDefaultTitle) {
                    // Use WINDOW.TITLE, else NAME
                    windowTitleOwned_ = !d.windowTitle.empty() ? d.windowTitle : d.name;
                    if (!windowTitleOwned_.empty()) b.windowTitle = windowTitleOwned_.c_str();
                }
                if (cfg_.width  == GameConfig::kDefaultWidth  && d.windowWidth  > 0) b.windowWidth  = static_cast<u32>(d.windowWidth);
                if (cfg_.height == GameConfig::kDefaultHeight && d.windowHeight > 0) b.windowHeight = static_cast<u32>(d.windowHeight);
                // Fullscreen is a window-creation-time decision. No command-line override exists.
                if (d.windowFullscreen >= 0) b.fullscreen = d.windowFullscreen != 0;
                // Resizable window setting from manifest
                if (d.windowResizable >= 0) b.resizable = d.windowResizable != 0;
            }
        }
    }
    return b;
}

void GameApp::initPhysics() {
#if AVER_MODULE_PHYSICS
    // No implicit ground; level supplies its own collision
    game::startPhysics("Game");
#endif
}

void GameApp::tickGameplay(f32 dt) {
#if AVER_MODULE_FRAMEWORK
    // PAUSED runs only the pumps; anything else but PLAYING runs nothing.
    if (aver_fw_play_state() == AVER_FW_PLAY_PAUSED) { game::tickPausedFrame(dt); return; }
    if (aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;

#if AVER_MODULE_PHYSICS
#if AVER_MODULE_SCENE
    // Vehicle logic: input -> step -> output. LOD based on camera distance. In game time.
    const f32 gameDt = dt * game::gameTimeScale();
    vehicles_.prePhysics(gameDt, camPos_);

    game::tickVehiclesPre(gameDt, camPos_);
#endif
    // Sum step count; a frame can run 0-8 fixed steps. Real time: the time scale is applied inside.
    physSteps_ += static_cast<u64>(game::tickGameplayGroups(dt));
#if AVER_MODULE_SCENE
    vehicles_.postPhysics(scene::World::instance());

    game::tickVehiclesPost(gameDt, playMobility_);
#endif
#else
    game::tickGameplayGroups(dt);
#endif
#else
    (void)dt;
#endif
}

#if AVER_MODULE_SCENE
bool GameApp::sequenceEmissiveScale(scene::Entity e, f32 outRgb[3], void* user) {
    return static_cast<const GameApp*>(user)->sequenceDirector_.emissiveScale(e, outRgb);
}
#endif

void GameApp::drivePlayCamera(f32 dt) {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    // The sequencer's camera goes to the rig system as the external pose, so a rig blends back from it when the sequence lets go.
    game::ExternalPose external;
    game::SeqCameraPose seqCam;
    if (sequenceDirector_.camera(seqCam)) {
        external.active = true;
        external.pos = seqCam.position;
        external.yaw = seqCam.yaw;
        external.pitch = seqCam.pitch;
    }
    firstPersonPawn_ = game::drivePlayCamera(camPos_, yaw_, pitch_, playView_, dt, scene::kInvalidEntity, &external);
    if (playView_.cut) streaming_.resetVelocityTracking();   // a teleport, not a fast move
#else
    (void)dt;
#endif
}

// Gives the mouse to the game or hands it back, through game::MouseCapture (MouseCapture.hpp).
void GameApp::setMouseCaptured(bool on) {
    mouse_.set(on, window_, "Game");
}

// Measures one frame of captured mouse movement and re-centres for the next, through
// game::MouseCapture.
void GameApp::pollCapturedMouse() {
    mouse_.poll(window_);
}

Vec3 GameApp::camForward() const {
    return game::cameraForward(yaw_, pitch_);
}

f32 GameApp::viewAspect(const Engine& e) const {
    // From swapchain (game scene is the backbuffer, unlike editor's dockspace panel)
    if (const Window* w = e.window()) {
        const u32 h = w->height();
        if (h > 0) return static_cast<f32>(w->width()) / static_cast<f32>(h);
    }
    return 16.0f / 9.0f;
}

void GameApp::onInit(Engine& e) {
    // First, before any file is read: catches package boundaries.
    enableFileTrace();
    attachCrowdGpu(e);
    bindWindowAndInput(e);
    logModuleSet();
    applyVsyncOverride(e);

    // Render features, in registration order; onShutdown removes them in reverse.
    attachSkinning(e);          // skinning prePass stages bone matrices; the scene pass then draws
    initWaterAndLandscape(e);   // before Voxi: its acceleration-structure build reads the fluid's prePass vertex buffer
    attachVoxi(e);
    initGameUi(e);
    attachSubViews(e);
    attachParticles(e);
    if (cfg_.pcgVolumeTest) attachPcgTest(e);

    // Simulation and gameplay seams, before openProject: level load builds static bodies, the audio mix needs a
    // started device, and class placements must find their components registered.
    initPhysics();
    startAudioDevice();
    registerSynapseComponents();
    installControlRig();
    installRagdoll();           // after the control rig: it chains it
    installAnimationProviders();
    installGameplayHosts();
    initPrefabs();

    openProject(e);

    // After openProject: these read what it populated.
    resolveAverSr(e);
    spawnParticleTest(e);
    startScripting();
    spawnPlacedClasses();       // after scripting: a class placement's class is not declared until it runs
    logSceneCensus();           // after level load and class spawns, before beginPlay
    beginObjectPlay();
    beginPlayIfGameModeDeclared();
    beginLevelVehicles();
    AVER_INFO("[Game] ready");
}

void GameApp::onUpdate(Engine& e, const Timestep& t) {
    beginFrame(e, t);
    dumpStatsIfDue(e, t);
    updateInput(e);
    updateUi(e, t);

    // Play PAUSED holds object animation. Sampled BEFORE gameplay tick, tested for PAUSED (not "not PLAYING").
    bool objectsHeld = false;
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    objectsHeld = aver_fw_play_state() == AVER_FW_PLAY_PAUSED;
#endif
    // Game time: the frame scaled by the time scale. tickGameplay takes real time (physics keeps a real-time
    // cadence and scales each step); the gameplay systems after it advance in game time.
#if AVER_MODULE_FRAMEWORK
    const f32 gameDt = t.dt * game::gameTimeScale();
#else
    const f32 gameDt = t.dt;
#endif
    tickGameplay(t.dt);
    tickWater(e, gameDt);
    // Beside tickGameplay, not gated on PLAYING.
    tickProjectGraphs(gameDt);
    tickGraphClasses(gameDt);
    tickScene(e, t, gameDt, objectsHeld);
    updateCamera(t);
    pollDiagnostics();
    tickFrameBudget(e, t);

    // MUST BE LAST: Engine::frameStep does onUpdate -> beginFrame -> onRender -> endFrame.
    // beginFrame takes ONE snapshot of PerFrameCB. setCamera/setLight/setSkyAtmosphere touch CPU shadow copy.
    // Calling from onRender would render frame N with frame N-1 constants and culling divergence.
    pushFrame(e);
    syncRuntimeAssets(e);
}

void GameApp::onShutdown(Engine& e) {
    // Release cursor first: an OS cursor left hidden/clipped outlives this process and annoys the user.
    setMouseCaptured(false);
    // Exact reverse registration order: device holds bare pointers to render features.
    // voxiRenderer_ is a MEMBER by value, so it outlives the device if unregistered first.
    rhi::IDevice* dev = e.device();
    detachSubViews();           // registered after everything below
    detachCrowdGpu(dev);
    detachUpscalers(dev);       // upscaler, frame generator and the PCG test feature
    shutdownParticles(dev);
    shutdownGameUi(dev);
    removeSceneFeatures(dev);
    shutdownVoxi(dev);
    releaseContentMaterials();  // after Voxi: the material system lives inside it
    shutdownSceneState(dev);    // streamed chunks, vehicles and the level, while physics still exists
    postLuts_.clear(dev);
    shutdownWater(dev);
    shutdownPhysics();
    shutdownAudio();
    shutdownScripting();

    // Reported unconditionally, even when zero: silent zero is indistinguishable from broken counter.
    setFileTrace(nullptr, nullptr);
    AVER_INFO("[Game] shutdown after {} frame(s), {} physics step(s)", frames_, physSteps_);
    (void)dev;
}

#if AVER_WITH_UI_ABI
void GameApp::submitGameUi(Engine&) {
#if AVER_MODULE_SCENE
    // 2D sprites and the queued Control draws join the UI list (docs/plans/RENDER_2D.md), under the game UI's own direct draws.
    if (gameUi_) game::scene2dSubmit(scene::World::instance());
    else game::scene2dDrop();
#endif
    if (!gameUi_) return;

#if AVER_MODULE_VOXI
    // World labels (Label3D) are not drawn while ray-driven primary visibility owns the frame, unless the game opts in.
    {
        const voxi::Renderer& vxr = voxi::Renderer::get();
        gameUi_->setRayDrivenActive(voxiAttached_ && voxi::resolve(vxr.settings(), vxr.deviceInfo()).rtRenderMode.effective == 1u);
    }
#endif
    const auto* dl = static_cast<const aver::ui::UiDrawList*>(aver_ui_draw_list());
    if (dl) gameUi_->submit(*dl);
    game::uiImagesTick();   // images nobody names any more go a few frames after their last use
}
#endif

void GameApp::onRender(Engine& e) {
#if AVER_MODULE_SCENE
    // First pixels: pushFrame set camera before beginFrame uploaded frame constants.
    if (rhi::IDevice* dev = e.device()) {
        pbr::MaterialSystem* ms = nullptr;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) ms = &voxiRenderer_.materials();
#endif
#if AVER_MODULE_VOXI
        // CDecal entities -> Voxi's projected-decal list. A decal's image id resolves through the project's
        // content index, like every other asset.
        // CLight entities -> Voxi's lamp list (no reference path tracer in the game).
        if (voxiAttached_) sceneLightFeed_.update(scene::World::instance(), voxiRenderer_, nullptr, project_.contentDir());
        if (voxiAttached_) envFxFeed_.update(voxiRenderer_, eye_);   // G4 R8: local fog volumes
        if (voxiAttached_)
            sceneDecalFeed_.update(scene::World::instance(), voxiRenderer_,
                                   [this](u64 id, voxi::DecalImageKind, ImageData& out) {
                                       const std::string full = content_.pathFor(id);
                                       std::string err;
                                       return !full.empty() && decodeImage(full, out, &err) && out.valid();
                                   });
#endif
#if AVER_MODULE_FLUIDS
        // Simulated fluid volumes, drawn first (editor's pattern).
        water_.draw(*dev, content_, ms);
#endif
#if AVER_MODULE_LANDSCAPE
        // Terrain before entities. LOD scale uses window height (whole backbuffer).
        if (landscape_.loaded()) {
            landscape_.updateRingTiles(dev, eye_.x, eye_.y, &content_, ms);
            const u32 viewH = e.window() ? e.window()->height() : cfg_.height;
            landscape_.draw(*dev, eye_, viewProj_, static_cast<f32>(viewH), &content_, ms, playView_.fovYRad);
        }
#endif
        // Owner-hide: hides possessed first-person pawn's body from raster pass. firstPersonPawn_ set every frame by drivePlayCamera().
        DrawWorldOptions opts;
        opts.ownerHideRoot = firstPersonPawn_;

        opts.ownerHideAll = playView_.ownerHideAll;   // the collision fade hides every mesh under the target, not only the flagged ones
        opts.cameraCullInv = playView_.cullInv;       // G4 R4: Camera3D.cull_mask; 0 (every layer) unless a camera node asks
        opts.viewerPos = eye_; opts.haveViewerPos = true;   // visibility ranges
        // Possessed pawn's tree is movable from first frame; everything else by PlayMobility.
        {
            scene::Entity movableRoot = scene::kInvalidEntity;
#if AVER_MODULE_FRAMEWORK
            const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
            if (pn > 0) movableRoot = static_cast<scene::Entity>(static_cast<u32>(pn));
#endif
            playMobility_.beginFrame(movableRoot);
            opts.mobility = &playMobility_;
        }
        if (!sequenceDirector_.empty()) {
            opts.emissiveScale = &GameApp::sequenceEmissiveScale;
            opts.user = this;
        }
#if AVER_MODULE_VOXI
        // Culled and owner-hidden entities reach Voxi too, so off-screen casters keep shadow and GI.
        if (voxiAttached_) opts.voxiRenderer = &voxiRenderer_;
#endif
#if AVER_MODULE_RENDER_SOFTBODY
        opts.softBodies = softBodyScene_.get();   // GameRender.cpp substitutes the simulated mesh for a soft entity
#if AVER_MODULE_RENDER_HAIR
        opts.hair = hairScene_.get();
#endif
#endif
        drawWorld(*dev, viewProj_, content_, drawStats_, ms, skinnedScene_.get(), opts);
#if AVER_MODULE_FRAMEWORK
        game::drawScriptDebugLines(dev, scriptDebugMesh_, e.time().dt);   // scripts' DebugDraw lines
#endif
    }
#endif
#if AVER_WITH_UI_ABI
    // Before screenshot request, so capture includes the HUD.
    submitGameUi(e);
#endif
    captureScreenshotIfDue(e);

    // Rolling input edges: last thing onUpdate does. Engine::run pumps window at loop top, so a key pressed this frame is in InputState by onUpdate.
    // Calling at onUpdate start would throw away edges; calling here after last reader leaves accumulator empty for next pumpEvents.
    input_.newFrame();
}

void GameApp::captureScreenshotIfDue(Engine& e) {
    if (cfg_.screenshotPath.empty() || screenshotDone_ || cfg_.maxFrames == 0) return;
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // requestCapture/getFrameImage are request/poll pair; both cannot happen same frame.
    const u64 f = e.time().frame;
    const u64 sf = cfg_.maxFrames > 8 ? cfg_.maxFrames - 3 : (cfg_.maxFrames > 1 ? cfg_.maxFrames - 1 : 0);
    if (f == sf) { dev->requestCapture(0, 0); return; }
    if (f <= sf) return;

    std::vector<u8> img; u32 iw = 0, ih = 0;
    if (dev->getFrameImage(img, iw, ih) && iw && ih &&
        stbi_write_png(cfg_.screenshotPath.c_str(), static_cast<int>(iw), static_cast<int>(ih), 4,
                       img.data(), static_cast<int>(iw) * 4)) {
        AVER_INFO("[Game] screenshot: {} ({}x{})", cfg_.screenshotPath, iw, ih);
        screenshotDone_ = true;
    }
}

} // namespace aver::game
