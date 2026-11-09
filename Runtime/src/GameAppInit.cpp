#include "GameAppInternal.hpp"

namespace aver::game {

namespace {

// Logs one line per file the engine opens. Installed only under --trace-opens. The "[open]" prefix
// is machine-readable: verify-game.ps1 greps it and asserts every path is under the package root --
// a packaged game falling back to a dev-tree asset would run fine on the build machine and fail
// everywhere else otherwise silently.
void onFileOpen(const char* path, void*) {
    AVER_INFO("[open] {}", path ? path : "(null)");
}

// The one event sink the game installs. Everything the window produces lands in InputState.
void onWindowEvent(void* user, const Event& e) {
    static_cast<InputState*>(user)->onEvent(e);
}

} // namespace

void GameApp::enableFileTrace() {
    // First, before any file is read: catches package boundaries.
    if (cfg_.traceOpens) {
        setFileTrace(&onFileOpen, nullptr);
        AVER_INFO("[Game] --trace-opens: every engine file read is logged with an [open] prefix");
    }
}

void GameApp::attachCrowdGpu(Engine& e) {
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    // GPU crowd backend: selectable through the Synapse ABI once installed; CPU stays the default.
    if (rhi::IDevice* gdev = e.device()) crowdGpuAttached_ = game::installCrowdGpu(*gdev, crowdGpu_);
#else
    (void)e;
#endif
}

void GameApp::bindWindowAndInput(Engine& e) {
    // Platform-side InputState: games read the window's event stream, not ImGui.
    window_ = e.window();
    if (Window* w = e.window()) {
        w->setEventCallback(&onWindowEvent, &input_);
        AVER_INFO("[Game] input bound to the window event stream ({}x{})", w->width(), w->height());
    } else {
        AVER_INFO("[Game] headless: no window, no input");
    }
}

void GameApp::logModuleSet() {
    // Verify link interface: if wrong, lifted code compiles to nothing and the world is empty.
    AVER_INFO("[Game] modules: PBR={} SCENE={} VOXI={} PHYSICS={} FRAMEWORK={} SCRIPTING={} PARTICLES={}",
              AVER_MODULE_PBR, AVER_MODULE_SCENE, AVER_MODULE_VOXI,
              AVER_MODULE_PHYSICS, AVER_MODULE_FRAMEWORK, AVER_MODULE_SCRIPTING, AVER_MODULE_PARTICLES);
}

void GameApp::applyVsyncOverride(Engine& e) {
    // Applied once at start: mirrors SandboxApp's vsyncOffRequested_ handling.
    if (cfg_.vsyncOff) {
        if (rhi::IDevice* dev = e.device()) {
            if (dev->vsyncCanDisable()) { dev->setVSync(false); AVER_INFO("[Game] vsync OFF (--no-vsync)"); }
            else AVER_WARN("[Game] --no-vsync ignored: this display path cannot tear");
        } else {
            AVER_WARN("[Game] --no-vsync ignored: no device");
        }
    }
}

void GameApp::initWaterAndLandscape(Engine& e) {
    (void)e;
#if AVER_MODULE_FLUIDS
    // Before Voxi: its acceleration-structure build reads the fluid's prePass vertex buffer.
    if (rhi::IDevice* dev = e.device()) water_.init(*dev);
#if AVER_MODULE_LANDSCAPE
    // river / lake depth reads the ground; C# and graphs read the landscape layers.
    water_.setGroundHeightSource([this](f64 x, f64 y, f64& z) { return landscape_.groundHeightAt(x, y, z); });
#endif
#endif
#if AVER_MODULE_LANDSCAPE
    landscape_.installProviders();
#endif
}

void GameApp::initGameUi(Engine& e) {
#if AVER_WITH_UI_ABI
    attachGameUi(e);
    // loadGameUiFont only needs a device; grouped with UI-ABI setup.
    loadGameUiFont(e);
#else
    (void)e;
#endif
}

void GameApp::attachSubViews(Engine& e) {
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Sub-viewports (G4 R5): no GPU object exists until a view is asked for. The UI is told when a view's texture is rebuilt, so it forgets its cached binding.
    if (rhi::IDevice* svDev = e.device()) {
#if AVER_WITH_UI_ABI
        subViews_.attach(*svDev, [](rhi::TextureHandle t, void* self) {
            if (auto* ui = static_cast<GameApp*>(self)->gameUi_) ui->dropBinding(t);
        }, this);
#else
        subViews_.attach(*svDev);
#endif
    }
#else
    (void)e;
#endif
}

void GameApp::startAudioDevice() {
#if AVER_WITH_AUDIO_ABI
    // Opens audio device; gates on started flag. Before openProject: mix settings need a started device.
    // 0 is not an error (no output device); unlike physics, which requires success.
    game::startAudio("Game");
#endif
}

void GameApp::registerSynapseComponents() {
#if AVER_MODULE_SYNAPSE_SCENE
    // Before openProject: registration needs no level/physics. Class placements must find components already registered.
    synapse::agentSystem().registerComponents(scene::World::instance());
    synapse::perceptionSystem().registerComponents(scene::World::instance());
    synapse::btSystem().registerComponents(scene::World::instance());
    // After registerComponents: built-in behaviors read these types directly.
    synapse::registerBuiltinBehaviors(synapse::btSystem());
#endif
}

void GameApp::installControlRig() {
#if AVER_MODULE_SCENE
    // Control rig: registered in editor onInit but never before in shipped game. Class placements must find the type.
    anim::controlRigSystem().registerComponents(scene::World::instance());
    anim::controlRigSystem().install(anim::animSystem(), scene::World::instance());
    game::installIkGroundProbe();   // foot IK casts through physics; a no-op without it
#endif
}

void GameApp::installRagdoll() {
#if AVER_MODULE_RAGDOLL
    // Ragdolls: installed AFTER the control rig so it chains it (the rig runs first, the ragdoll sees its pose).
    if (ragdoll::ragdollSystem().install(anim::animSystem(), scene::World::instance())) {
#if AVER_MODULE_FRAMEWORK
        aver_fw_set_ragdoll_provider(&ragdoll::providerStart, &ragdoll::providerStop, &ragdoll::providerMode,
                                     &ragdoll::providerSetStrength, &ragdoll::providerAddImpulse,
                                     &ragdoll::ragdollSystem());
        aver_fw_set_physanim_provider(&ragdoll::physAnimProvider, &ragdoll::ragdollSystem());
#endif
    }
#endif
}

void GameApp::installAnimationProviders() {
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Montages: the framework relays PlayMontage and friends to AnimSystem's montage players.
    aver_fw_set_anim_provider(&anim::montageProvider, &anim::animSystem());
    // IK goals, foot-IK weights and rig inputs.
    aver_fw_set_ik_provider(&anim::ikProvider, &anim::controlRigSystem());
    // Bone poses a script reads and writes: the pose seam in the control rig's modifier.
    aver_fw_set_pose_provider(&anim::poseProvider, &anim::controlRigSystem().poseSeam());

    // Blend-shape weights, face poses and lip-sync.
    aver_fw_set_morph_provider(&anim::morphProvider, &anim::animSystem().morphs());
    // Level sequences: the framework relays LevelSequences.Play and friends to the director.
    aver_fw_set_sequence_provider(&game::sequenceProvider, &sequenceDirector_);
#endif
}

void GameApp::installGameplayHosts() {
    // Animation state machines, reverb zones, blackboard relay, crowd/hearing/cover behaviours.
    game::registerGameSystems();
#if AVER_MODULE_SOFTBODY_SCENE
    game::installSoftBodyHost(&GameContent::resolveAnimAsset, &content_, nullptr, nullptr);
    game::installSoftBodyEffects(&GameContent::resolveAnimAsset, &content_);   // break effects: particles and sounds per .ocsoft
#endif
    game::installVehicleHost(&GameContent::resolveAnimAsset, &content_);
#if AVER_MODULE_VEHICLE_SCENE
    // Traffic: `vehicle Vehicles/Bus.ocvehicle` names an asset (a preset name still means a preset).
    vehicles_.setSpecSource([](const std::string& path) { return game::vehicleSpecByPath(path); });
#endif
}

void GameApp::initPrefabs() {
#if AVER_MODULE_SCENE
    // Prefab assets come out of the project's content index (the same id space every other asset uses).
    prefabLib_.setLoader([this](const std::string& path, fmt::OcPrefabData& out, std::string* why) {
        const std::string full = content_.pathFor(fnv1a64(std::string_view(path)));
        if (full.empty()) {
            if (why) *why = "'" + path + "' is not in the project content";
            return false;
        }
        return fmt::loadOcPrefab(full, out, why);
    });
    prefabSys_ = std::make_unique<prefab::PrefabSystem>(scene::World::instance(), prefabLib_);
#  if AVER_MODULE_FRAMEWORK
    // A prefab node naming a class becomes that actor (now, or when its class is declared), and destroying
    // one runs the actor teardown for it and every actor below it.
    {
        prefab::Hooks h;
        h.classBound = [](scene::Entity e, const std::string& cls) {
            aver_fw_adopt_by_name(static_cast<int32_t>(e), cls.c_str());
        };
        h.destroying = [](scene::Entity e) {
            if (aver_fw_class_of(static_cast<int32_t>(e)) != 0) aver_fw_destroy(static_cast<int32_t>(e));
        };
        prefabSys_->setHooks(std::move(h));
    }
#  endif
    prefabHost_ = prefab::makeAbiHost(*prefabSys_);
    aver_prefab_set_host(&prefabHost_);
#  ifdef AVER_WITH_LEVELINST_ABI
    // Scripts place, stream and query level instances through this seam.
    levelInstHost_ = game::makeAbiHost(level_.instances());
    aver_levelinst_set_host(&levelInstHost_);
#  endif
#endif
}

void GameApp::spawnParticleTest(Engine& e) {
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // After openProject: needs registerBuiltins' unit cube and overrides camera state.
    if (cfg_.particleTest) {
        if (rhi::IDevice* dev = e.device()) spawnParticleTestContent(*dev);
        else AVER_WARN("[Game] --particle-test: no device, nothing spawned");
    }
#else
    (void)e;
#endif
}

void GameApp::startScripting() {
#if AVER_MODULE_SCRIPTING
    // After openProject: scripts directory and graph discovery need state openProject populated.
    // Scan before bootstrapping to avoid cost on graph-free projects (measured: 40-60ms extra).
    const bool haveGraphs = !content_.pathsWithExtension(".ocgraph").empty();
    // "\\" not "\": MSVC drops unescaped backslash (C4129), otherwise tests "...BinariesScripts" (no separator).
    const bool haveScriptAssembly = std::filesystem::exists(project_.binariesDir() + AVER_PATH_SEP "Scripts");
    if (haveGraphs || haveScriptAssembly) {
        initScripting();
        discoverProjectGraphs();
    } else {
        AVER_INFO("[Game] no .ocgraph content and no Scripts assembly -- scripting host not started");
    }
#endif
}

void GameApp::spawnPlacedClasses() {
#if AVER_MODULE_FRAMEWORK
    // Deliberately after initScripting: class placement's class isn't declared until this runs.
    // See GameLevel.hpp's classPlacements_ comment.
    level_.spawnClassPlacements();
#endif
}

void GameApp::logSceneCensus() {
#if AVER_MODULE_SCENE
    // Census taken after level load and class spawns, before beginPlay.
    if (cfg_.sceneCensus) {
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(world::takeSceneCensus(scene::World::instance())));
    }
#endif
}

void GameApp::beginObjectPlay() {
#if AVER_MODULE_SCENE
    // Shipped game: booting the level starts objects (editor turns this on at Play). Marks objects movable.
    playMobility_.begin(scene::World::instance());
    // The sequence's actors are going to move: movable from the first frame, like the physics cars.
    if (!sequenceDirector_.empty()) playMobility_.seedMovable(sequenceDirector_.boundEntities());
    anim::animSystem().setObjectAnimationLive(true);
#endif
}

void GameApp::beginLevelVehicles() {
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS && AVER_MODULE_FRAMEWORK
    // LEVEL'S CARS BECOME REAL HERE, once there is a play session: physics vehicle per `vehicle` placement.
    // Physics only steps while PLAYING, so a project with no GameMode never starts one.
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING && level_.beginVehicles(vehicles_, content_) > 0)
        playMobility_.seedMovable(vehicles_.entities());
#endif
}


} // namespace aver::game
