#include "GameAppInternal.hpp"

namespace aver::game {

namespace {

#if AVER_MODULE_SYNAPSE_SCENE
// A level's .ocnav sits beside it with the same stem (Arena.ocworld -> Arena.ocnav). Deliberately a
// separate copy of NavBakeCommand.cpp's navPathForLevel: that file is editor-only, and this
// composition root must not depend on it for a five-line string derivation.
std::string navPathForLevel(const std::string& levelPath) {
    if (levelPath.empty()) return {};
    const usize slash = levelPath.find_last_of("/\\");
    const usize dot = levelPath.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return (hasExt ? levelPath.substr(0, dot) : levelPath) + ".ocnav";
}
#endif

} // namespace

void GameApp::installLevelHooks(Engine& e) {
#if AVER_MODULE_SCENE
    GameLevel::LoadHooks hooks;
    // Water, then terrain (matches editor order)
    hooks.beforePlacements = [this, &e](const std::string& path, const fmt::OcWorldData& w) {
#  if AVER_MODULE_FLUIDS
        if (rhi::IDevice* dev = e.device()) water_.applyLevel(*dev, w);
#  endif
#  if AVER_MODULE_LANDSCAPE
        pbr::MaterialSystem* ms = nullptr;
#    if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (voxiAttached_) ms = &voxiRenderer_.materials();
#    endif
        landscape_.loadForLevel(e.device(), project_.contentDir(), path, w, &content_, ms);
#  endif
        (void)path; (void)w;
    };
#  if AVER_MODULE_LANDSCAPE
    hooks.groundHeightAt = [this](f64 x, f64 y, f64& outZ) { return landscape_.groundHeightAt(x, y, outZ); };
#  endif
    hooks.afterUnload = [this, &e] {
        postVolumes_.clear();
        postLuts_.clear(e.device());
        sequenceDirector_.restore(scene::World::instance());
        sequenceDirector_.clear();
#  if AVER_MODULE_FLUIDS
        water_.unload();
#  endif
#  if AVER_MODULE_LANDSCAPE
        // Clear last level's terrain
        landscape_.unload(e.device());
#  endif
#  if AVER_MODULE_VOXI
        // Clear foliage (outside entity list)
        if (voxiAttached_) levelFoliage_.clear();
#  endif
        (void)e;
    };
    // Display loading progress on startup splash
    hooks.progress = [&e](const std::string& stage, f32 fraction) {
        e.setLoadingStatus(stage);
        e.setLoadingProgress(fraction);
    };
    hooks.afterInstantiate = [this, &e](const GameLevel::LoadedLevel& loaded) {
        levelGameMode_    = loaded.world.gameMode;      // World Settings overrides, read at begin play
        levelDefaultPawn_ = loaded.world.defaultPawn;
        // Records the placement loop does not know: bare CDecal entities, and prefab instances rebuilt from
        // their assets with their overrides on top.
        game::spawnLevelDecals(scene::World::instance(), loaded.world.decals);
        // LIGHT records: the packaged game spawned none before this.
        const std::vector<scene::Entity> lightEntities = game::spawnLevelLights(scene::World::instance(), loaded.world.lights);
        postVolumes_ = loaded.world.postVolumes;
        if (rhi::IDevice* dev = e.device()) postLuts_.preload(*dev, project_.contentDir(), postVolumes_);
        if (prefabSys_) prefabSys_->instantiateLevelInstances(loaded.world.prefabInstances);
        // The level's sequences, bound to its placements' (and lights') entities; the autoplay ones start with the session.
        sequenceDirector_.restore(scene::World::instance());
        sequenceDirector_.clear();
        if (!loaded.world.sequences.empty()) {
            std::vector<scene::Entity> byPlacement(loaded.world.placements.size(), scene::kInvalidEntity);
            const auto& inst = loaded.instance;
            for (usize k = 0; k < inst.entities.size() && k < inst.placementIndex.size(); ++k)
                if (inst.placementIndex[k] < byPlacement.size()) byPlacement[inst.placementIndex[k]] = inst.entities[k];
            sequenceDirector_.setMode(game::SeqMode::Live);
            sequenceDirector_.setHooks(game::makeSequenceHooks(project_.contentDir(), [this](i32 ent, const std::string& name) {
#if AVER_MODULE_SCRIPTING
                return scripts_.graphFire(ent, name.c_str());
#else
                (void)ent; (void)name;
                return false;
#endif
            }));
            sequenceDirector_.setSequences(loaded.world.sequences);
            sequenceDirector_.setBindings(game::makeLevelBindings(loaded.world, byPlacement, lightEntities));
            sequenceDirector_.startAutoplay();
            for (const fmt::OcSequence& seq : loaded.world.sequences)
                AVER_INFO("[Game] level sequence '{}': {} track(s), {:.1f}s{}", seq.name, seq.tracks.size(), seq.length,
                          seq.autoplay ? ", autoplay" : "");
        }
#  if AVER_MODULE_VOXI
        // Load foliage after placements (matches editor)
        if (voxiAttached_) {
            const std::vector<std::string> tables = level_.foliageTablePaths(loaded.world.foliageFiles);
            levelFoliage_.load(loaded.world, content_, e.device(), &voxiRenderer_, project_.contentDir(), {}, camPos_,
                               &tables);
            if (!levelFoliage_.result().error.empty()) AVER_WARN("[Foliage] {}", levelFoliage_.result().error);
        }
#  else
        (void)loaded;
#  endif
    };
    level_.setLoadHooks(std::move(hooks));
    level_.setDevice(e.device());
    level_.setUseCookedChunks(true);   // the shipped path streams a level's cooked chunk set (LARGE_WORLD 2.4.6); the editor leaves it off
    // LEVELINST records (docs/LEVEL_INSTANCES.md): the prefab system, the framework and
    // the foliage path are the host's; LevelInstances itself depends on none of them.
    {
        LevelInstances::Hooks ih;
        ih.spawnPrefab = [this](const fmt::OcPrefabInstance& pr, const Transform& local, scene::Entity parent) -> scene::Entity {
            if (!prefabSys_) return scene::kInvalidEntity;
            return prefabSys_->instantiate(pr.prefab, local, parent, pr.overrides, pr.name);
        };
#  if AVER_MODULE_FRAMEWORK
        ih.spawnClass = [](const fmt::OcWorldPlacement& p, const Transform& world, scene::Entity) {
            return spawnInstanceClass(p, world);
        };
        ih.destroyClass = [](scene::Entity ce) { destroyInstanceClass(ce); };
#  endif
        level_.setInstanceHooks(std::move(ih));
#  if AVER_MODULE_VOXI
        LevelInstances::PackedSink ps;
        ps.set = [this](u64 owner, fmt::OcInstanceData rows, std::vector<std::string> mats) {
            if (voxiAttached_) levelFoliage_.setExtraTable(owner, std::move(rows), std::move(mats));
        };
        ps.clear = [this](u64 owner) { if (voxiAttached_) levelFoliage_.clearExtraTable(owner); };
        level_.setPackedSink(std::move(ps));
#  endif
#  if AVER_MODULE_FRAMEWORK
        level_.instances().onResidency = [](scene::Entity root, bool loaded, u32 count) {
            relayInstanceResidency(root, loaded, count);
        };
#  endif
        // The content index resolves a child level exactly as it resolves a prefab.
        level_.setInstanceLoader([this](const std::string& path, fmt::OcWorldData& out, std::string* why) {
            const std::string full = content_.pathFor(fnv1a64(std::string_view(path)));
            if (full.empty()) {
                if (why) *why = "'" + path + "' is not in the project content";
                return false;
            }
            return fmt::loadOcworld(full, out, why);
        });
    }
#  if AVER_MODULE_LANDSCAPE
    // Restart streaming on terrain change
    landscape_.setTerrainChangedHook([this] {
        if (!streaming_.enabled()) return;
        AVER_INFO("[ChunkWorld] terrain changed under a live stream -- restarting it so scatter "
                  "re-generates against the new surface");
        streaming_.disable();
        enableChunkStreaming();
    });
#  endif
#else
    (void)e;
#endif
}

void GameApp::enableChunkStreaming() {
#if AVER_MODULE_SCENE
    GameStreaming::HeightQueryFn height;
#  if AVER_MODULE_LANDSCAPE
    if (landscape_.loaded())
        height = [this](f32 x, f32 y, f32& outZ) { return landscape_.scatterHeightAt(x, y, outZ); };
#  endif
    GameStreaming::LayerWeightFn layers;
#  if AVER_MODULE_LANDSCAPE
    if (landscape_.loaded())
        layers = [this](std::string_view name, f32 x, f32 y) { return landscape_.layerWeightAtScene(name, x, y); };
#  endif
    streaming_.enable(project_, level_.pcgVolumes(), level_.scatterSpecies(), &content_, std::move(height), std::move(layers));
    streaming_.warnIfCameraOutsideGeneratedBand(camPos_);
#endif
}

void GameApp::openProject(Engine& e) {
    installLevelHooks(e);
    // Look beside executable for Game.ocproject (not working directory)
    std::string path = cfg_.projectPath;
    if (path.empty()) {
        const std::string beside = executableDir() + AVER_PATH_SEP "Game.ocproject";
        if (fileExists(beside)) { path = beside; }
    }
    if (path.empty()) {
        AVER_INFO("[Game] no project: pass one on the command line, or ship a Game.ocproject beside the executable");
#if AVER_MODULE_SCENE
        // Load level without project if named on command line
        initOrigin(e);
        if (!cfg_.levelPath.empty()) loadStartMapAtOrigin(cfg_.levelPath);
#endif
        return;
    }

    std::string err;
    if (!fmt::loadOcproject(path, project_, &err)) {
        AVER_ERROR("[Game] could not open '{}': {}", path, err);
        return;   // deliberately not fatal; see the header
    }
#if AVER_MODULE_FRAMEWORK
    game::publishProjectPaths(project_, /*editor=*/false);   // res:// and user:// for scripts
#endif
#if AVER_MODULE_PHYSICS
    // The project's solver settings and world limits, before any level builds a body (PhysicsProjectApply.hpp); an
    // empty world is rebuilt for the limits.
    game::configureProjectPhysics(project_, "Game");
#endif
    AVER_INFO("[Game] project '{}'  content={}  startMap='{}'",
              project_.name, project_.contentDir(),
              project_.startMap.empty() ? "<none>" : project_.startMap);
    ext::warnMissingProjectExtensions(project_);   // an EXTENSION line whose extension is not installed
#if AVER_MODULE_PBR
    // Set texture cache dir before any load
    assets::setTextureCacheDir(project_.dir + AVER_PATH_SEP "Saved" AVER_PATH_SEP "DerivedDataCache" AVER_PATH_SEP "Textures");
#endif
    if (project_.startMap.empty()) {
        AVER_WARN("[Game] the manifest names no STARTMAP, so there is no level to open");
    }
#if AVER_MODULE_VOXI
    // Read frame budget from manifest (<=0 disables it)
    frameBudget_.budgetMs = project_.frameBudgetMs;
#endif
#if AVER_MODULE_FRAMEWORK
    // WINDOW.VSYNC and WINDOW.MAXFPS: the project's present mode and frame cap, the same requests a script makes (taken in onUpdate).
    // (--no-vsync, a measurement flag, wins over the manifest.)
    if (!cfg_.vsyncOff && project_.windowVsync >= 0 && project_.windowVsync < AVER_FW_PRESENT_COUNT) aver_fw_request_present_mode(project_.windowVsync);
    if (project_.windowMaxFps > 0) aver_fw_set_max_fps(project_.windowMaxFps);
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // G4 R5: how many sub-viewports may exist and how many are drawn per frame (the defaults when the manifest is silent).
    game::SubViewportHost::configure(game::subViewportSettingsOf(project_.subViewports, project_.subViewRenders, project_.subViewPixels));
#endif
    game::configureRuntimeMeshBudget(project_);
    // Load order is critical: index -> meshes -> levels
    content_.adopt(project_);
    // The project's physical-surface table, before any level builds a shape: a material slot's PHYSSURFACE, a landscape layer or a
    // script names a surface and the registry already holds the authored friction, restitution and tag (a missing file is silent).
    game::loadPhysicalSurfaces(project_.contentDir());
#if AVER_MODULE_SCENE
    // adopt cleared AnimSystem; the project's .ocretarget assets say which clips play on which rig.
    anim::animSystem().registerRetargets(content_.idsWithExtension(".ocretarget"));
#endif
    {
        // String and data-table stores, and the active culture (--culture > saved setting > OS language > LOCALE.DEFAULT).
        game::LocalizationInit li;
        li.project = &project_;
        li.settingsPath = gameSettingsPath(project_);
        li.cultureOverride = cfg_.culture;
        game::initLocalization(li);
        // Accessibility (docs/ACCESSIBILITY.md): the options from Settings.ini before the first frame, the Captions folder,
        // narration of the UI, and the audio play observer that raises caption cues.
        game::AccessibilityInit ai;
        ai.settingsPath = gameSettingsPath(project_);
        ai.contentDir = project_.contentDir();
        game::initAccessibility(ai);
#if AVER_WITH_UI_ABI && AVER_MODULE_FRAMEWORK
        game::installAccessibilityUi();
#endif
#if AVER_WITH_AUDIO_ABI && AVER_MODULE_FRAMEWORK
        game::installAccessibilityAudio();
#endif
#if AVER_WITH_UI_ABI
        game::uiTextInit(project_.contentDir());   // loads Content/UI/Fonts.ocfonts (or the built-in set); follows the culture just initialised
        game::uiTextBindRenderer(gameUi_);         // lends the UI renderer the glyph atlas
        game::uiImagesSetContent(project_.contentDir());   // UI images are read from this Content folder
#endif
#if AVER_WITH_UI_ABI && AVER_MODULE_FRAMEWORK
        aver_ui_set_text_resolver(&aver_fw_loc_resolve, &aver_fw_loc_epoch);
#endif
    }
#if AVER_MODULE_SCENE
    if (rhi::IDevice* dev = e.device()) {
        content_.registerBuiltins(*dev);
        content_.loadProjectMeshes(*dev);
    }
#if AVER_MODULE_PARTICLES
    // Load particle effects before levels (CPU-only)
    content_.loadProjectParticleEffects();
#endif
    initOrigin(e);
    loadStartMapAtOrigin(cfg_.levelPath);
    loadLevelNav();
    applyLevelSky();
    // Manifest GI volume wins over auto-fit (mirrors editor precedence)
    if (project_.hasGiVolume) {
        giCenter_ = Vec3{project_.giCenter[0], project_.giCenter[1], project_.giCenter[2]};
        giExtent_ = project_.giExtent;
        AVER_INFO("[Voxi] GI volume from the manifest: centre[{:.0f} {:.0f} {:.0f}] extent {:.0f}cm",
                  giCenter_.x, giCenter_.y, giCenter_.z, giExtent_);
    }
    if (project_.giExtent <= 0.0f) fitGiVolumeToLevel();
#endif
    applyProjectRenderSettings();
    // RENDER.EXPOSURE/BLOOM/AUTOEXPOSURE/TONEMAP are per-device post state (like PHYSICS/AUDIO)
    applyProjectPostSettings();
#if AVER_MODULE_PHYSICS
    // Apply physics settings from manifest
    game::applyProjectPhysics(project_);
#endif
#if AVER_WITH_AUDIO_ABI
    // Apply audio mix from manifest
    game::applyProjectAudioMix(project_);
#endif
}

// The player's Settings.ini: scripting opens it for input bindings, localization reads the saved culture from it.
std::string app_detail::gameSettingsPath(const fmt::ProjectDesc& project) {
#if AVER_MODULE_FRAMEWORK
    return game::projectUserDir(project) + AVER_PATH_SEP "Settings.ini";
#else
    return userDataDir() + AVER_PATH_SEP + project.name + AVER_PATH_SEP "Settings.ini";
#endif
}

// The level's baked navigation, if it has one (most have none).
void GameApp::loadLevelNav() {
#if AVER_MODULE_SYNAPSE_SCENE
    gameNav_ = fmt::OcNavData{};
    std::string navErr;
    const std::string navPath = navPathForLevel(level_.path());
    if (!navPath.empty() && fmt::loadOcNav(navPath, gameNav_, &navErr)) {
        AVER_INFO("[Game] navigation: '{}' ({}x{} cells)", navPath, gameNav_.widthCells, gameNav_.heightCells);
    } else if (!navPath.empty()) {
        AVER_INFO("[Game] navigation: no baked '{}' ({}) -- Synapse agents will wait for a goal grid",
                  navPath, navErr);
    }
#endif
}

// Level travel (Game.ChangeLevel), between frames: the session's actors end
// (the GameInstance and persistent actors stay), the level is replaced, its placed actors spawn, and the
// GameMode, controller and pawn begin again at the new level's spawn.
void GameApp::travelTo(const std::string& requested) {
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    std::string path = requested;
    if (path.rfind("res://", 0) == 0) path = path.substr(6);
    std::error_code ec;
    if (!std::filesystem::path(path).is_absolute() && project_.valid()) path = project_.contentDir() + AVER_PATH_SEP + path;
    if (!std::filesystem::exists(path, ec)) {
        AVER_ERROR("[Game] level travel to '{}': no such level; staying", requested);
        return;
    }
    AVER_INFO("[Game] level travel to '{}'", path);
    aver_fw_travel_begin();
    scene::World::instance().flush();
    level_.unload();
    scene::World::instance().flush();
    loadStartMapAtOrigin(path);
    loadLevelNav();
    applyLevelSky();
    if (project_.giExtent <= 0.0f) fitGiVolumeToLevel();
    level_.spawnClassPlacements();
    auto named = [](const std::string& n, i32 flag) -> i32 {
        const i32 c = n.empty() ? 0 : aver_fw_class_find(n.c_str());
        return (c && (aver_fw_class_get_flags(c) & flag)) ? c : 0;
    };
    i32 modeClass = named(levelGameMode_, AVER_FW_CLASS_GAME_MODE);
    if (!modeClass) modeClass = named(project_.gameMode, AVER_FW_CLASS_GAME_MODE);
    if (aver_fw_travel_end(modeClass, named(levelDefaultPawn_, AVER_FW_CLASS_PAWN))) placePawnAtSpawn();
    else AVER_WARN("[Game] level travel: no GameMode could begin in '{}'", path);
#else
    (void)requested;
#endif
}

// Place pawn at level spawn (from beginPlayIfGameModeDeclared after aver_fw_begin_play succeeds)
bool GameApp::placePawnAtSpawn() {
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    const GameLevel::SpawnPoint& sp = level_.spawn();
    if (!sp.valid) return false;
    if (!game::placePossessedPawn(sp.position, sp.yawDeg)) return false;
    AVER_INFO("[Game] pawn placed at the level's Player Start ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}",
              sp.position.x, sp.position.y, sp.position.z, sp.yawDeg);
    return true;
#else
    return false;
#endif
}

void GameApp::applyLevelSky() {
#if AVER_MODULE_SCENE
    const fmt::OcWorldEnv& w = level_.env();
    if (!w.hasSun && !w.hasSky && !w.hasFog && !w.hasClouds) return;

    // Shared mapping (aver::assets::applyLevelEnv) ensures game and editor don't drift
    assets::applyLevelEnv(w, sky_);

    // Reseed from atmosphere (frame loop resets sunColor_ every frame)
    for (int i = 0; i < 3; ++i) sunColor_[i] = sky_.sunColor[i];
    if (w.hasFog) fogDensity_ = sky_.fogDensity;

    AVER_INFO("[Level] applied environment: sun={} sky={} fog={} clouds={}, intensity {:.2f}, "
              "model {}", w.hasSun, w.hasSky, w.hasFog, w.hasClouds, sky_.sunIntensity,
              w.skyPhysical ? "physical" : "authored");
#endif
}

// Fit GI volume to level (extent sets voxel size in 128^3 grid: ~9cm at 12m, ~15.6m at 2000m)
void GameApp::fitGiVolumeToLevel() {
#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
    Vec3 lo{}, hi{};
    f32 radius = 0.0f;
    if (!level_.placementBounds(lo, hi, radius)) return;
    giCenter_ = Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    giExtent_ = radius;
    AVER_INFO("[Voxi] GI volume fitted to the level: centre[{:.0f} {:.0f} {:.0f}] extent {:.0f}cm",
              giCenter_.x, giCenter_.y, giCenter_.z, giExtent_);
#endif
}

void GameApp::applyProjectRenderSettings() {
#if AVER_MODULE_VOXI
    if (!project_.valid() || !project_.hasRenderSettings()) return;
    if (!voxiAttached_) return;

    // Two-phase: tiers commit first, so derivation runs against new tier before manifest lands on top
    voxi::Renderer& vx = voxi::Renderer::get();
    voxi::Settings s = vx.settings();
    voxi::applyManifestTwoPhase(project_, s, [&]() {
        vx.setSettings(s);
        s = vx.settings();
    });
    voxiRenderer_.setSettings(vx.settings());
    AVER_INFO("[Project] applied render settings: gi={}, rt={}, pt={}, voxelRes={}, giIntensity={}, giDist={}",
              static_cast<int>(s.globalIllumination), static_cast<int>(s.rayTracing),
              static_cast<int>(s.pathTracing), s.voxelResolution, s.giIntensity, s.giMaxDistance);

    // Report manifest contradictions: requests that didn't survive RenderSettingsResolver prerequisites
    voxi::ManifestAsks asks;
    asks.giMode         = project_.giMode;
    asks.denoiser       = project_.denoiser;
    asks.rtRenderMode   = project_.rtRenderMode;
    asks.refractionMode = project_.refractionMode;
    voxi::FieldReport reports[4];
    const u32 reportCount = voxi::manifestContradictions(vx.settings(), vx.deviceInfo(), asks, reports);
    for (u32 i = 0; i < reportCount; ++i) {
        voxi::Feature alreadyReportedBy;
        if (voxi::refusalFeatureFor(reports[i].reason, alreadyReportedBy) &&
            vx.refusalLogged(alreadyReportedBy)) {
            continue;
        }
        AVER_WARN("[Project] RENDER.{} asked for {} but it is not in effect: {}",
                  reports[i].manifestKey, reports[i].requested,
                  voxi::disableReasonText(reports[i].reason));
    }
#endif
}

// Apply post-processing settings from manifest (decoupled from applyProjectRenderSettings like PHYSICS/AUDIO)
void GameApp::applyProjectPostSettings() {
    if (!project_.valid()) return;

    bool stated = false;
    if (project_.postExposure >= 0.0f)  { post_.exposure       = project_.postExposure;          stated = true; }
    if (project_.postBloom    >= 0.0f)  { post_.bloomIntensity = project_.postBloom;             stated = true; }
    if (project_.postAutoExposure >= 0) { post_.autoExposure   = project_.postAutoExposure != 0; stated = true; }
    if (rhi::applyProjectTonemapKeys(post_, project_.postTonemap, project_.postTonemapWhite, project_.postAdjustBrightness,
                                     project_.postAdjustContrast, project_.postAdjustSaturation, true))
        stated = true;
    if (!stated) return;

    AVER_INFO("[Project] applied post settings: exposure={:.3f} bloom={:.3f} autoExposure={} tonemap={}",
              post_.exposure, post_.bloomIntensity, post_.autoExposure ? 1 : 0, post_.tonemap);

    // With auto-exposure on, composite pass ignores RENDER.EXPOSURE (post.hlsl:216-220)
    if (project_.postExposure >= 0.0f && post_.autoExposure) {
        AVER_WARN("[Project] RENDER.EXPOSURE {:.3f} is not in effect: auto-exposure is on{} and "
                  "replaces the exposure every frame -- state RENDER.AUTOEXPOSURE 0 to use it",
                  project_.postExposure,
                  project_.postAutoExposure >= 0 ? " (RENDER.AUTOEXPOSURE 1)" : " by default");
    }
}

} // namespace aver::game
