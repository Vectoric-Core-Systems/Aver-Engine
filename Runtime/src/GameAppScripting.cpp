#include "GameAppInternal.hpp"

namespace aver::game {

namespace {

// Lightweight scan for top-level records in .ocgraph text. Sets outValue to the token after the key.
bool ocgraphRecord(const std::string& text, std::string_view wanted, std::string* outValue) {
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + pos, nl - pos);
        pos = nl + 1;

        const usize start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) continue;
        line.remove_prefix(start);
        if (line.empty() || line[0] == '#') continue;

        const usize end = line.find_first_of(" \t\r");
        if (!equalsAsciiCI(end == std::string_view::npos ? line : line.substr(0, end), wanted)) continue;

        if (outValue) {
            outValue->clear();
            std::string_view rest = end == std::string_view::npos ? std::string_view() : line.substr(end);
            const usize vs = rest.find_first_not_of(" \t\r");
            if (vs != std::string_view::npos) {
                rest.remove_prefix(vs);
                const usize ve = rest.find_first_of(" \t\r");
                *outValue = std::string(ve == std::string_view::npos ? rest : rest.substr(0, ve));
            }
        }
        return true;
    }
    return false;
}

// True when an .ocgraph declares itself a spawnable class via a top-level CLASS record (Graph.
// ClassName). Lets discoverProjectGraphs skip a file already driven by the class-registration/spawn
// path, so it is never also ticked against a synthetic entity alongside its real spawned instances.
bool ocgraphDeclaresClass(const std::string& text) { return ocgraphRecord(text, "CLASS", nullptr); }

// Check if graph's DOMAIN is not gameplay
bool ocgraphIsForeign(const std::string& text) {
    std::string domain;
    if (!ocgraphRecord(text, "DOMAIN", &domain) || domain.empty()) return false;
    return !equalsAsciiCI(domain, "gameplay");
}

} // namespace

void GameApp::initScripting() {
#if AVER_MODULE_SCRIPTING
    // Initialize CLR host and scripting system
    scripting::HostDesc hd;
    hd.bridgeDir = executableDir() + AVER_PATH_SEP "Scripting";
    // Scripts dir: <out>\Binaries\Scripts (see ProjectScaffold.cpp/stage-game.ps1)
    hd.scriptsDir = project_.valid() ? (project_.binariesDir() + AVER_PATH_SEP "Scripts") : std::string();
    scriptsReady_ = scripts_.init(hd);
    if (scriptsReady_) {
        AVER_INFO("[Game] scripting host ready: bridge='{}' scripts='{}' ({} legacy behaviour(s) live)",
                  hd.bridgeDir, hd.scriptsDir, scripts_.behaviourCount());
        game::setAiContentRoot(project_.contentDir());   // AI: wave 3 #21 -- where .oceqs / .ocso / .ocst paths resolve
        // Graph-as-class: declare framework classes before discoverProjectGraphs (which skips claimed files)
        const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
        if (graphClasses > 0)
            AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());

        // Input scheme
        {
            // INPUT.SCHEME is relative to content root, like DRONE.GRAPH
            std::string schemePath = project_.inputScheme.empty()
                ? std::string() : project_.contentDir() + AVER_PATH_SEP + project_.inputScheme;
            if (!schemePath.empty() && !fileExists(schemePath)) {
                AVER_WARN("[Scripting] INPUT.SCHEME '{}' does not exist -- no input scheme will be loaded",
                          schemePath);
                schemePath.clear();
            }

            const std::string settingsPath = gameSettingsPath(project_);

            const i32 actions = scripts_.configureInput(schemePath, settingsPath);
            if (actions > 0)
                AVER_INFO("[Scripting] input scheme ready: {} action(s) from '{}', bindings persist to '{}'",
                          actions, schemePath, settingsPath);
            else if (actions == 0)
                AVER_INFO("[Scripting] no input scheme loaded (project has no INPUT.SCHEME, or its file "
                          "was missing) -- settings store opened at '{}'", settingsPath);
            else if (actions == -2)
                AVER_WARN("[Scripting] this build's scripting bridge predates ConfigureInput -- "
                          "rebindable input is unavailable");
            else
                AVER_WARN("[Scripting] input scheme '{}' failed to load (see the [Scripting] error line "
                          "just above)", schemePath);
        }

        // Animation notifies wired to the scripting host
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
        aver_fw_set_anim_curve_provider(&GameApp::animCurve, this);
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, &origin_);
#endif

#if AVER_MODULE_SYNAPSE_SCENE
#if AVER_MODULE_FRAMEWORK
        // GetSynapseTarget reaches CSynapseAgent waypoint (requires both modules)
        aver_fw_set_synapse_target_provider(&GameApp::synapseTarget, this);
        // GetSynapsePerception reaches sight state
        aver_fw_set_synapse_perception_provider(&GameApp::synapsePerception, this);
        // PerceptionSystem target resolver (not a framework relay; Aver.Synapse.Scene cannot link Aver.Framework)
        synapse::perceptionSystem().setTargetResolver(&GameApp::synapseTargetResolver, this);
#endif
#endif

        if (scripts_.graphFireAvailable()) {
#if AVER_MODULE_SCENE
            anim::animSystem().setNotifySink(&GameApp::animNotify, this);
#if AVER_MODULE_SOFTBODY_SCENE
            softbody::softBodySystem().setNotifySink(&GameApp::animNotify, this);
#endif

            game::setVehicleNotify(&GameApp::animNotify, this);   // VehicleCrash / VehicleSkid / VehicleLanded / VehicleFlipped / VehicleShift / VehicleEntered / VehicleExited

#if AVER_MODULE_CAMERA && AVER_MODULE_FRAMEWORK
            camera::cameraRigSystem().setNotifySink(&GameApp::animNotify, this);   // CameraViewTargetChanged / CameraBlendFinished / CameraCut
#endif
#endif
#if AVER_MODULE_SYNAPSE_SCENE
            // Same sink as animNotify (byte-for-byte identical signatures)
            synapse::perceptionSystem().setNotifySink(&GameApp::animNotify, this);
            // "FireEvent" BT action also fires through animNotify
            synapse::btSystem().setNotifySink(&GameApp::animNotify, this);
#  if AVER_WITH_SYNAPSE_AI
            game::installAiNotify(&GameApp::animNotify, this);   // AI: wave 3 #21 -- EqsFinished, SmartObject* ...
#  endif
#endif
            AVER_INFO("[Anim] animation notifies will be raised as graph events");
        } else {
            AVER_WARN("[Anim] this build's scripting bridge cannot be fired at; animation notifies "
                      "will be tracked but not delivered (see ScriptHost::graphFireAvailable)");
        }
    } else {
        AVER_INFO("[Game] scripting host unavailable ({}) -- graphs and any other C# gameplay will not run",
                  scripts_.declineReason());
    }
#endif
}

// Framework's curve provider (guarded on SCENE, not SCRIPTING: definition stayed unguarded while declaration was guarded)
#if AVER_MODULE_SCENE
i32 GameApp::animCurve(i32 entity, i64 nameHash, f32* outValue, void*) {
    f32 v = 0.0f;
    if (!anim::animSystem().curveValue(static_cast<scene::Entity>(entity),
                                       static_cast<u64>(nameHash), v)) return 0;
    *outValue = v;
    return 1;
}
#endif // AVER_MODULE_SCENE (animCurve)

#if AVER_MODULE_SYNAPSE_SCENE
i32 GameApp::synapseTarget(i32 entity, f32* outX, f32* outY, f32* outZ, void*) {
    scene::World& w = scene::World::instance();
    const auto* a = w.component<synapse::CSynapseAgent>(static_cast<scene::Entity>(entity),
                                                         synapse::agentSystem().componentType());
    if (!a || a->status != static_cast<i32>(synapse::AgentStatus::Pathing)) return 0;
    *outX = a->targetXCm;
    *outY = a->targetYCm;
    *outZ = a->targetZCm;
    return 1;
}

#if AVER_MODULE_FRAMEWORK
scene::Entity GameApp::synapseTargetResolver(void*) {
    return static_cast<scene::Entity>(aver_fw_controlled_pawn(aver_fw_player_controller(0)));
}
#endif

i32 GameApp::synapsePerception(i32 entity, i32* outCanSee, i32* outLastTarget, f32* outTimeSinceSeen, void*) {
    scene::World& w = scene::World::instance();
    const auto* p = w.component<synapse::CSynapsePerception>(
        static_cast<scene::Entity>(entity), synapse::perceptionSystem().componentType());
    if (!p) return 0;
    *outCanSee = p->canSeeTarget;
    *outLastTarget = p->lastKnownTargetEntity;
    *outTimeSinceSeen = p->timeSinceSeenSec;
    return 1;
}
#endif

// Animation-notify wire: clip marker names graph event, raised on the entity playing the clip
// graphFire returns false for entity with no graph or graph with no event (not an error)
#if AVER_MODULE_SCENE
void GameApp::animNotify(scene::Entity e, const char* name, void* user) {
#if AVER_MODULE_SCRIPTING
    auto* self = static_cast<GameApp*>(user);
    if (!self || !name) return;
    self->scripts_.graphFire(static_cast<i32>(e), name);
#else
    (void)e; (void)name; (void)user;
#endif
}
#endif // AVER_MODULE_SCENE (animNotify)


void GameApp::discoverProjectGraphs() {
#if AVER_MODULE_SCRIPTING
    if (!scriptsReady_) return;
    if (!scripts_.graphAvailable()) {
        AVER_INFO("[Graph] this build's scripting bridge has no graph hosting -- project .ocgraph "
                  "files, if any, will not run (see ScriptHost::graphAvailable's own doc)");
        return;
    }

    // No-graph project is a no-op: empty projectGraphs_, empty early-out loop
    const std::vector<std::string> paths = content_.pathsWithExtension(".ocgraph");
    if (paths.empty()) {
        AVER_INFO("[Graph] 0 .ocgraph file(s) under this project's content -- nothing to run");
        return;
    }

    // Synthetic negative entity ids (-1000, decreasing) for project-level graphs (not scoped to any entity)
    i32 nextId = -1000;
    u32 loaded = 0;
    u32 skippedAsClasses = 0;
    u32 skippedForeign = 0;
    for (const std::string& path : paths) {
        // Graph-as-class: file with CLASS record already claimed by declareGraphClasses (in initScripting)
        std::string text;
        const bool haveText = readFileText(path, text);
        // Not every .ocgraph is a gameplay graph; material graphs fail with "unknown node type"
        if (haveText && ocgraphIsForeign(text)) {
            ++skippedForeign;
            continue;
        }
        if (haveText && ocgraphDeclaresClass(text)) {
            ++skippedAsClasses;
            continue;
        }

        const i32 id = nextId--;
        const bool ok = scripts_.graphLoad(id, path);
        projectGraphs_.push_back(ProjectGraph{path, id, ok});
        if (ok) {
            ++loaded;
            AVER_INFO("[Graph] loaded '{}' (synthetic id {})", path, id);
        } else {
            AVER_WARN("[Graph] '{}' failed to load -- see the [GraphHost] reason above; continuing without it", path);
        }
    }
    AVER_INFO("[Graph] {} of {} project graph(s) loaded ({} skipped -- declared as classes instead, "
              "{} not gameplay graphs)",
              loaded, projectGraphs_.size(), skippedAsClasses, skippedForeign);
#endif
}

void GameApp::tickProjectGraphs(f32 dt) {
#if AVER_MODULE_SCRIPTING
    if (!scriptsReady_ || projectGraphs_.empty()) return;
    // Ungated on aver_fw_play_state(): Aver.Graph has no reference to Aver.Framework
    for (const ProjectGraph& g : projectGraphs_) {
        if (!g.loaded) continue;
        scripts_.graphTick(g.syntheticEntity, dt);
    }
#else
    (void)dt;
#endif
}

void GameApp::beginPlayIfGameModeDeclared() {
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCRIPTING
    if (!scriptsReady_) return;

    // The level's World Settings GAMEMODE, else the project's GAME.MODE, else (a shipped game has no
    // editor drone) the first concrete GameMode declared, as before those keys existed.
    auto named = [](const std::string& n, i32 flag) -> i32 {
        const i32 c = n.empty() ? 0 : aver_fw_class_find(n.c_str());
        return (c && (aver_fw_class_get_flags(c) & flag)) ? c : 0;
    };
    i32 modeClass = named(levelGameMode_, AVER_FW_CLASS_GAME_MODE);
    if (!modeClass) modeClass = named(project_.gameMode, AVER_FW_CLASS_GAME_MODE);
    if (!modeClass) modeClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE);
    const i32 pawnClass = named(levelDefaultPawn_, AVER_FW_CLASS_PAWN);
    // Latch for the rest of process (onUpdate's graph-class tick needs to distinguish "not begun" from "undeclared")
    gameModeDeclared_ = modeClass != 0;
    if (modeClass == 0) {
        AVER_INFO("[Game] no GameMode class declared -- play session not started (the framework stays "
                  "exactly as inert as it was before this existed; see discoverProjectGraphs for the "
                  "same shape decided by content instead of by class declarations)");
        return;
    }
    const i32 instanceClass = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);
    // (PlayMobility already began in onInit, before this call)
    if (aver_fw_begin_play_with_pawn(instanceClass, modeClass, pawnClass)) {
        AVER_INFO("[Game] play session begun automatically (GameMode class {}) -- a shipped game has no "
                  "editor Play button, so booting it IS beginning play", modeClass);
        // Pawn doesn't exist until GameMode spawned and possessed it
        placePawnAtSpawn();
    } else {
        AVER_WARN("[Game] aver_fw_begin_play declined for GameMode class {} -- the framework stays in "
                  "EDITOR state; the world still renders, nothing in it plays", modeClass);
    }
#endif
}

} // namespace aver::game
