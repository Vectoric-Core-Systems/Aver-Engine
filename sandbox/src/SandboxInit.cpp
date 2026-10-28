// SandboxApp::onInit: builds the editor (asset editors, physics, scene, gizmos, render features). Split from
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
#include "OcShapeEditor.hpp"   // makeOcShapeEditor (.ocshape); not pulled in by SandboxApp.hpp
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameIkProbe.hpp"   // [wave5/ik-retarget] the foot-IK ground probe
#include "aver/game/GameTick.hpp"
#include "aver/game/GameSystemsWiring.hpp"
#include "aver/game/SoftBodyEffects.hpp"
#include "aver/game/SequenceHost.hpp"   // [wave5/sequencer] the ABI provider
#include "aver/game/GameUiInput.hpp"
#include "aver/game/GameUiText.hpp"
#include "aver/platform/FileSystem.hpp"   // userDataDir: where the trajectory network's weights live
#if AVER_MODULE_VOXI
#include "ModeSwitchNotice.hpp"
#endif

namespace aver {

// Build editor: asset editors, physics, scene, gizmos, render features.
void SandboxApp::onInit(Engine& e)  {
    // FIRST: set engineForSplash_ before anything logs (logSink uses it to update splash).
    engineForSplash_ = &e;
    mainThreadId_ = std::this_thread::get_id();
    AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());
    // Latched for Project Settings display (no Engine& to re-ask).
    runningBackend_ = rhi::backendName(e.device()->backend());
#if AVER_MODULE_SR
    // AverSR quality drives renderScaleOverride_; --render-scale wins if both set.
    if (averSrQuality_ != aver::sr::Quality::Off && renderScaleOverride_ == 1.0f)
        renderScaleOverride_ = aver::sr::renderScaleFor(averSrQuality_);
#endif
    // Apply render scale once, before device sizes are determined.
    if (renderScaleOverride_ != 1.0f) {
        e.device()->setRenderScale(renderScaleOverride_);
        AVER_INFO("[Sandbox] render scale {:.2f} (--render-scale)", e.device()->renderScale());
    }
#if AVER_MODULE_SR
    // Build GPU upscaler when non-Off quality is set.
    if (averSrQuality_ != aver::sr::Quality::Off) {
        ensureAverSrUpscaler(e.device());
        logAverSrActive(e.device());
    }
    applyUpscalerSlot(e.device());   // a manual scale or edge AA wants FSR too
#endif
    // Depth-only pass before opaque walk. See entity loop for two-walk mechanism.
    if (depthPrepassOverride_) {
        e.device()->setDepthPrepassEnabled(true);
        AVER_INFO("[Sandbox] depth prepass enabled (--depth-prepass)");
    }

    // Register GBuffer debug unconditionally (mode defaults to Off). Per-frame write gated in onUpdate.
    e.device()->addRenderFeature(&gbufferDebugFeature_);
    gbufferDebugAttached_ = true;
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    crowdGpuAttached_ = game::installCrowdGpu(*e.device(), crowdGpu_);
#endif
    // NeuraFI viz draws nothing until Neural Visualiser picks a view.
    e.device()->addRenderFeature(&neurafiViz_);

    // Registration order is precedence: first factory that accepts path wins.
    assetEditors_.registerFactory(&editor::makeMeshEditor);
    assetEditors_.registerFactory(&editor::makeActorEditor);
    assetEditors_.registerFactory(&editor::makeAnimEditor);
    // Append graph editor (order matters; would change which editor claims shared file types).
    assetEditors_.registerFactory(&editor::makeGraphEditor);
    // Wire graph validator via lambda to avoid .NET dependency in GraphEditor.
#if AVER_MODULE_SCRIPTING
    editor::setGraphValidator([this](const std::string& text, std::string& err) {
        if (!scripts_.graphValidateAvailable()) { err = "the .NET bridge exports no GraphValidate"; return false; }
        return scripts_.graphValidate(text, err);
    });
    // Arm node-hit recording (keyed by graph name; off costs one bool test).
    editor::setGraphNodeHitSource([this](const std::string& graphName, f32 maxAge,
                                         std::vector<std::pair<std::string, f32>>& out) {
        scripts_.graphNodeHits(graphName, maxAge, out);
    });
    scripts_.graphSetHitRecording(true);
#endif  // AVER_MODULE_SCRIPTING
    // How Find References across the project, and a breakpoint in a graph with no tab open, reach a file.
    editor::setGraphOpener([this](const std::string& path) { return assetEditors_.open(path); });
    // The visual behaviour-tree editor claims .ocbt ahead of the plain list editor (registration order is
    // precedence); BtEditor stays registered as the fallback.
    assetEditors_.registerFactory(&editor::makeBtGraphEditor);
    // AI: wave 3 #21 -- state trees (.ocst) and environment queries (.oceqs). A smart object (.ocso) has no tab.
    assetEditors_.registerFactory(&editor::makeStateTreeEditor);
    assetEditors_.registerFactory(&editor::makeEqsEditor);
    editor::setEqsSelectionSource([this]() {
        std::vector<u32> picked;
        for (const scene::Entity e : selectedEntities()) picked.push_back(e);
        return picked;
    });
    // Append BtEditor (claims .ocbt).
    assetEditors_.registerFactory(&editor::makeBtEditor);
    assetEditors_.registerFactory(&editor::makeUiLayoutEditor);            // .ocui
    assetEditors_.registerFactory(&editor::makeFontSetEditor);             // .ocfonts
    assetEditors_.registerFactory(&editor::makeBlendSpaceEditor);          // .ocblend
    assetEditors_.registerFactory(&editor::makeAnimStateMachineEditor);    // .ocasm
    assetEditors_.registerFactory(&editor::makeMontageEditor);             // .ocmontage
    // [wave5/ik-retarget] IK Rig, IK Retargeter, Control Rig; the skeleton tab offers to create the first two.
    assetEditors_.registerFactory(&editor::makeFaceRigEditor);             // .ocface  [wave9/morph]
    assetEditors_.registerFactory(&editor::makeIkRigEditor);               // .ocikrig
    assetEditors_.registerFactory(&editor::makeRetargetEditor);            // .ocretarget
    assetEditors_.registerFactory(&editor::makeRigEditor);                 // .ocrig
#if AVER_WITH_IMGUI   // the Content Browser's create members are ImGui-side
    {
        editor::AnimEditorRigHooks hooks;
        hooks.createIkRig = [this](const std::string& skel) { cbCreateIkRig(skel); };
        hooks.createRetarget = [this](const std::string& skel) { cbCreateRetarget(skel); };
        hooks.openRetargetFor = [this](const std::string& skel) { return cbOpenRetargetFor(skel); };
        editor::setAnimEditorRigHooks(std::move(hooks));
    }
#endif
    assetEditors_.registerFactory(&editor::makeMotionDbEditor);            // .ocmotion
    // Append SoundEditor (claims .ocsnd).
    assetEditors_.registerFactory(&editor::makeSoundEditor);
#if AVER_MODULE_PARTICLES
    assetEditors_.registerFactory(&editor::makeParticleEditor);  // .ocparticle
#endif
    assetEditors_.registerFactory(&editor::makeFoliageTypeEditor);  // .ocfoliage
    assetEditors_.registerFactory(&editor::makeHairGroomEditor);    // .ochair
    assetEditors_.registerFactory(&editor::makeOcShapeEditor);      // .ocshape  [wave11/shapes]

#if AVER_MODULE_VEHICLE
    assetEditors_.registerFactory(&editor::makeVehicleEditor);      // .ocvehicle
#endif
    assetEditors_.registerFactory(&editor::makeInstanceSetEditor);  // .ocinst
    assetEditors_.registerFactory(&editor::makeInputSchemeEditor);  // .ocinput
    assetEditors_.registerFactory(&editor::makeCaptionEditor);      // .occaptions
    // The string-table editor first: it claims the CSVs under Localization/<culture>/ that the data table editor declines.
    assetEditors_.registerFactory(&editor::makeStringTableEditor);  // .ocstrings, Localization/<culture>/*.csv
    assetEditors_.registerFactory(&editor::makeDataTableEditor);    // .csv, table-shaped .json
    assetEditors_.registerFactory(&editor::makeTextureEditor);  // image formats, read-only
    {
        // Hooks to query/set project's Input Scheme.
        editor::InputSchemeEditorHooks hooks;
        hooks.contentDir = [this] { return project_.contentDir(); };
        hooks.projectInputScheme = [this] { return project_.inputScheme; };
        hooks.useAsProjectInputScheme = [this](const std::string& contentRelativePath) {
            if (!project_.valid()) return false;
            // Write happens on autosave timer or Project Settings Save button.
            project_.inputScheme = contentRelativePath;
            projectDirty_ = true;
            return true;
        };
        editor::setInputSchemeEditorHooks(std::move(hooks));
        // Captions tab: the project's audio files (pattern tester, From Sound), Preview through the engine, resolved text under each field.
        editor::CaptionEditorHooks ch;
        ch.soundPaths = [this] {
            std::vector<std::string> out;
            std::string root = project_.contentDir();
            for (char& c : root) if (c == '\\') c = '/';
            for (std::string p : content_.pathsWithExtension(".ocaudio")) {
                for (char& c : p) if (c == '\\') c = '/';
                if (p.size() > root.size() && p.compare(0, root.size(), root) == 0) p.erase(0, root.size() + (p[root.size()] == '/' ? 1 : 0));
                out.push_back(std::move(p));
            }
            return out;
        };
#if AVER_MODULE_FRAMEWORK
        ch.previewCue = [](const std::string& cue) { return aver_fw_caption_show(cue.c_str(), 0, 0.0f, 0.0f, 0.0f) != 0; };
        ch.resolveText = [](const std::string& raw) {
            char buf[512];
            const int32_t n = aver_fw_loc_resolve(raw.c_str(), buf, static_cast<int32_t>(sizeof buf));
            return n < 0 ? raw : std::string(buf, static_cast<usize>(std::min<int32_t>(n, static_cast<int32_t>(sizeof buf) - 1)));
        };
#endif
        editor::setCaptionEditorHooks(std::move(ch));
        accessPreview_.load();   // the editor's colour-vision preview choice (editor prefs only)
    }
#if AVER_MODULE_FRAMEWORK
    {
        // The UI layout editor's preview resolves text references through the framework's string store.
        editor::UiLayoutEditorLoc loc;
        loc.resolve = [](std::string_view raw, std::string& out) {
            const std::string ref(raw);
            char shortBuf[512];
            int32_t n = aver_fw_loc_resolve(ref.c_str(), shortBuf, static_cast<int32_t>(sizeof shortBuf));
            if (n < 0) return false;
            if (n < static_cast<int32_t>(sizeof shortBuf)) { out.assign(shortBuf, static_cast<usize>(n)); return true; }
            std::string big(static_cast<usize>(n) + 1, '\0');
            n = aver_fw_loc_resolve(ref.c_str(), big.data(), static_cast<int32_t>(big.size()));
            out.assign(big.data(), static_cast<usize>(std::max<int32_t>(n, 0)));
            return n >= 0;
        };
        loc.epoch = [] { return static_cast<u32>(aver_fw_loc_epoch()); };
        loc.cultures = [] {
            std::vector<std::string> v;
            for (int32_t i = 0; i < aver_fw_loc_culture_count(); ++i) v.emplace_back(aver_fw_loc_culture_at(i));
            return v;
        };
        loc.culture = [] { return std::string(aver_fw_loc_culture()); };
        loc.setCulture = [](const std::string& tag) {
            editor::setPrefString("loc.previewCulture", tag);
            editor::flushEditorPrefs();
            aver_fw_loc_set_culture(tag.c_str());
        };
        editor::setUiLayoutEditorLoc(std::move(loc));
    }
#endif
    {
        editor::StringTableEditorHooks hooks;
        hooks.defaultCulture = [this] { return project_.localeDefault; };
        hooks.openAsset = [this](const std::string& abs) { assetEditors_.open(abs); };
        editor::setStringTableEditorHooks(std::move(hooks));
    }
#if AVER_MODULE_SCENE
    {
        // The .ocinst tab asks the level whether it uses the file, and for the streaming rebake.
        editor::InstanceSetEditorHooks hooks;
        hooks.levelReferences = [this](const std::string& abs) {
            std::error_code ec;
            for (const std::string& f : levelHeader_.foliageFiles)
                if (std::filesystem::equivalent(std::filesystem::path(project_.contentDir()) / f, abs, ec) && !ec) return true;
            return false;
        };
        hooks.regenerateStreaming = [this](std::string& report) { return regenerateStreamData(report); };
        editor::setInstanceSetEditorHooks(std::move(hooks));
    }
#endif
    // Locate AverDesign before ActorEditor (caches Roslyn availability).
    locateAverDesign();
    {
        editor::ActorEditorHooks hooks;
        hooks.compileScripts = [this] { tools_.triggerToolbarCompile(project_); };
        hooks.compileBusy    = [this] { return tools_.compiling(); };
        hooks.drawCompileButton = [this] { tools_.drawCompileButton(project_, dpi_, compileIconUiId_); };
#if AVER_WITH_IMGUI
        hooks.openInIde = [this](const std::string& p) {
            const editor::IdeInfo& ide = cbIde();
            if (!editor::openInIde(ide, p)) AVER_WARN("[Editor] could not open {} in {}", p, ide.name);
        };
        hooks.ideName = cbIde().name;
#endif
        editor::setActorEditorHooks(std::move(hooks));
    }
    window_ = e.window();
    // Set event sink. Everything lands in accumulator unfiltered.
    if (window_) window_->setEventCallback(&sandboxWindowEvent, &input_);
    // Enable drag-drop from Explorer (polled once per frame in onUpdate).
    if (window_) window_->setAcceptDroppedFiles(true);

    // Single-instance forwarding receiver (singleInstanceEligible_ gates both registration and hook).
    if (window_ && window_->valid() && singleInstanceEligible_) {
        Window::registerAsSingleInstancePrimary(window_->nativeHandle());
#if AVER_MODULE_SCENE
        window_->setOpenRequestHook(&SandboxApp::onOpenRequestThunk, this);
#endif
    }
    // Window X button goes through same unsaved-changes check as File > Exit.
    if (window_ && window_->valid()) window_->setCloseGuard(&SandboxApp::onCloseGuardThunk, this);

#if AVER_MODULE_MCP
    // Start MCP channel if port is set (ABI registration, widget hooks, listen shared with status bar).
    if (mcpPort_ && !mcpStart(mcpPort_))
        AVER_WARN("[Mcp] --mcp was given but the channel did not start; the editor is "
                  "unaffected and carries on");
#endif

    browser_.init();

#if AVER_MODULE_PHYSICS
    // Must start before any level loads: loading builds a static body per colliding placement.
    game::startPhysics("Sandbox");
#endif

#if AVER_WITH_AUDIO_ABI
    // Opens the audio device -- see GameTick.hpp for why "no output device" stays quiet.
    game::startAudio("Sandbox");
#endif

#if AVER_MODULE_SYNAPSE_SCENE
    // Register synapse components before project opens (needs no level, no physics).
    synapse::agentSystem().registerComponents(scene::World::instance());
    synapse::perceptionSystem().registerComponents(scene::World::instance());
    synapse::btSystem().registerComponents(scene::World::instance());
    synapse::registerBuiltinBehaviors(synapse::btSystem());
#endif

#if AVER_MODULE_SCENE
    // Control rig: register and install before project opens (needs no level).
    anim::controlRigSystem().registerComponents(scene::World::instance());
    anim::controlRigSystem().install(anim::animSystem(), scene::World::instance());
    // Root motion Apply moves the entity only while Play is live: the animation system ticks in edit mode too, and a merely
    // selected character must not walk off its placement (the packaged game is always live and does not opt in).
    anim::animSystem().setRootMotionRequiresLive(true);
    game::installIkGroundProbe();   // [wave5/ik-retarget] foot IK casts through physics; a no-op without it
#endif
#if AVER_MODULE_RAGDOLL
    // Ragdolls: installed AFTER the control rig so it chains it (the rig runs first, the ragdoll sees its pose).
    if (ragdoll::ragdollSystem().install(anim::animSystem(), scene::World::instance())) {
#if AVER_MODULE_FRAMEWORK
        aver_fw_set_ragdoll_provider(&ragdoll::providerStart, &ragdoll::providerStop, &ragdoll::providerMode,
                                     &ragdoll::providerSetStrength, &ragdoll::providerAddImpulse,
                                     &ragdoll::ragdollSystem());
#endif
    }
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Montages: the framework relays PlayMontage and friends to AnimSystem's montage players.
    aver_fw_set_anim_provider(&anim::montageProvider, &anim::animSystem());
    // Model import: ImportModel / ModelImport.Import reach the editor's importers (wave 9, docs/FBX_IMPORT.md). Editor only; a
    // packaged game installs none and answers "unavailable".
    {
        ModelImportHost host;
        host.contentDir = [this] { return project_.valid() ? project_.contentDir() : std::string(); };
        host.projectDir = [this] { return project_.valid() ? project_.dir : std::string(); };
        host.imported = [this](const std::string& destDir) { cbInvalidate(destDir); wantMeshReload_ = true; };
        installModelImportProvider(host);
    }
    // [wave5/ik-retarget] IK goals, foot-IK weights and rig inputs.
    aver_fw_set_ik_provider(&anim::ikProvider, &anim::controlRigSystem());

    // [wave9/morph] Blend-shape weights, face poses and lip-sync.
    aver_fw_set_morph_provider(&anim::morphProvider, &anim::animSystem().morphs());
    // [wave5/sequencer] Level sequences: LevelSequences.Play and friends reach the director Play runs.
    aver_fw_set_sequence_provider(&game::sequenceProvider, seqEditor_.director());
#endif
    // Animation state machines, reverb zones, blackboard relay, crowd/hearing/cover behaviours.
    game::registerGameSystems();
#if AVER_MODULE_SOFTBODY_SCENE
    // Path resolver for the soft-body mesh/.ocsoft decode caches (the content index, as for animation).
    game::installSoftBodyHost(&game::GameContent::resolveAnimAsset, &content_, nullptr, nullptr);
    game::installSoftBodyEffects(&game::GameContent::resolveAnimAsset, &content_);   // break effects, as GameApp
#endif
    game::installVehicleHost(&game::GameContent::resolveAnimAsset, &content_);
#if AVER_MODULE_VEHICLE_SCENE
    vehicles_.setSpecSource([](const std::string& path) { return game::vehicleSpecByPath(path); });   // traffic: `vehicle Vehicles/Bus.ocvehicle`
#endif
#if AVER_MODULE_SCENE
    installPrefabHooks();
    installLevelInstanceHooks();   // [wave3/level-instances]
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Off unless --lod-mesh-shader (opt-in). D3D12 only; needs AVER_MODULE_VOXI for GI/shadows.
    lodMeshShaderEnabled_ = lodMeshShaderRequest_ > 0;
    if (lodMeshShaderEnabled_) {
        const rhi::DeviceCaps mcaps = e.device()->caps();
        // D3D12 only (Voxi table-0 merge is D3D12-specific).
        const bool ok = mcaps.meshShaderTier > 0 && mcaps.shaderModel >= 65 && mcaps.dxcAvailable
#if AVER_MODULE_VOXI
                        && e.device()->backend() == rhi::Backend::D3D12
#endif
                        ;
        lodMeshShaderEnabled_ = ok;
        if (ok) {
#if AVER_MODULE_VOXI
            AVER_INFO("[LOD] per-cluster mesh-shader path ON by request (mesh tier {}, SM {}) "
                      "-- faster and textured, and casts cascade shadows and voxel-cone GI through "
                      "its own depth proxy (via voxiRenderer_.submit()), though PSClusterMain's own "
                      "shading is never ray traced even when the project has ray tracing on",
                      mcaps.meshShaderTier, mcaps.shaderModel);
#else
            AVER_WARN("[LOD] per-cluster mesh-shader path ON by request (mesh tier {}, SM {}) "
                      "-- faster and textured, but NO SHADOWS AND NO GI on these draws (built "
                      "without AVER_MODULE_VOXI)",
                      mcaps.meshShaderTier, mcaps.shaderModel);
#endif
        }
        else    AVER_INFO("[LOD] per-cluster mesh-shader path unavailable (mesh tier {}, SM {}, "
                          "DXC {}); drawing without it", mcaps.meshShaderTier, mcaps.shaderModel,
                          mcaps.dxcAvailable);
    }
#endif

    if (!projectPath_.empty()) {
        // --open-legacy skips upgrade prompt and opens old project directly.
        std::string err;
        const bool opened = openLegacy_ ? browser_.open(projectPath_, &err)
                                        : browser_.openOrOfferUpgrade(projectPath_, &err);
        if (opened) applyProject(e);
        else if (!openLegacy_ && browser_.upgradePending()) {
            armBrowser(true);
            if (maxFrames_ != 0) {
                AVER_ERROR("[Sandbox] '{}' was made by an older series and this run has a frame "
                           "limit, so nothing can answer the upgrade prompt. THE PROJECT WILL NOT "
                           "OPEN and every timing, probe and screenshot from this run measures an "
                           "empty editor. Pass --open-legacy to open it as-is without upgrading.",
                           projectPath_);
            } else {
                AVER_INFO("[Sandbox] '{}' was made by an older series; asking before opening it",
                          projectPath_);
            }
        } else {
            AVER_WARN("[Sandbox] '{}' not loaded: {}", projectPath_, err);
        }
    } else if (!openMapPath_.empty()) {
        // Load level with no project. Everything else hangs off applyProject.
#if AVER_MODULE_SCENE
        loadStartMap(e);
#else
        AVER_WARN("[Sandbox] '{}' not opened: this build has no scene to instantiate a level into",
                  openMapPath_);
#endif
    }
#if AVER_WITH_IMGUI
    if (e.device()->uiActive()) {
        applyDpi(e.window() ? e.window()->dpiScale() : 1.0f);
        AVER_INFO("[Sandbox] DPI scale {:.2f}, UI font rasterised at {:.0f}px", dpi_, 16.0f * dpi_);
        if (browserActive_) loadLogo(e);
        loadCompileIcon(e);
        loadIconSheet(e, "file-icons.png",   kFileIconTiles, "FileTypeIcons", fileIconsTexture_,   fileIconsUiId_,   fileIconAspect_);
        loadIconSheet(e, "folder-icons.png", kFolderIconTiles, "FolderIcons",  folderIconsTexture_, folderIconsUiId_, folderIconAspect_);
        loadIconSheet(e, "asset-icons.png",  kAssetIconTiles,  "AssetTypeIcons", assetIconsTexture_,  assetIconsUiId_,  assetIconAspect_);
        loadGameUiFont(e);
        if (const std::string er = editor::engineRoot(); !er.empty()) {
            std::error_code ec;
            const std::filesystem::path cs = std::filesystem::path(er) / "scripting" / "csharp";
            if (std::filesystem::is_directory(cs, ec)) cbEngineRoot_ = cs.string();
        }
        AVER_INFO("[Sandbox] Content Browser engine root: {}",
                  cbEngineRoot_.empty() ? "(none - shipped build)" : cbEngineRoot_.c_str());
    }
#endif
    // Default blank map: ground + cube + sun + sky + atmosphere.
    std::vector<rhi::MeshVertex> gv, gi_v; std::vector<u32> gi, ci;
    appendGround(gv, gi, kEditorFloorHalf);
    rhi::MeshHandle ground = e.device()->createMesh(gv.data(), (u32)gv.size(), gi.data(), (u32)gi.size());
    // Unit cube stays half-extent 1 (.ocworld PLACEG applies cm scales to it).
    appendBox(gi_v, ci, 0,0,0, kEditorCubeHalf);
    rhi::MeshHandle cube = e.device()->createMesh(gi_v.data(), (u32)gi_v.size(), ci.data(), (u32)ci.size());

#if AVER_MODULE_SCENE
    // Register built-in primitive meshes and seed per-mesh pick geometry.
    {
        MeshLoadPass pass; pass.app = this; pass.engine = &e;
        content_.setMeshLoadedHook(&SandboxApp::onMeshLoaded, &pass);
        content_.registerBuiltins(*e.device());
        content_.setMeshLoadedHook(nullptr, nullptr);
        unitCubeMesh_ = content_.meshFor(fnv1a64(std::string_view("Meshes/cube.ocmesh")));
    }
#endif

    MeshObj floor; floor.name="Floor"; floor.mesh=ground; floor.tris=(u32)gi.size()/3;
    floor.color[0]=0.34f; floor.color[1]=0.35f; floor.color[2]=0.37f;
    floor.metallic=0.0f; floor.roughness=0.9f;
    floor.aabbMin=Vec3{-kEditorFloorHalf,-kEditorFloorHalf,-5.0f};
    floor.aabbMax=Vec3{ kEditorFloorHalf, kEditorFloorHalf, 5.0f};
    objects_.push_back(floor);
    cubeMesh_ = cube; cubeTris_ = (u32)ci.size()/3;
    MeshObj c; c.name="Cube"; c.mesh=cube; c.tris=(u32)ci.size()/3; c.pos=Vec3{0,0,kEditorCubeHalf};
    c.color[0]=0.85f; c.color[1]=0.36f; c.color[2]=0.22f;
    c.metallic=0.1f; c.roughness=0.35f;
    objects_.push_back(c);

    if (!beamPath_.empty()) {
        fmt::OcBeamData beam; std::string err;
        if (fmt::loadOcbeam(beamPath_, beam, &err) && !beam.nodes.empty()) {
            std::vector<rhi::MeshVertex> bv; std::vector<u32> bi;
            AABB bb; bb.min=bb.max=Vec3{beam.nodes[0].x,beam.nodes[0].y,beam.nodes[0].z};
            for (auto& n : beam.nodes) bb.expand(Vec3{n.x,n.y,n.z});
            Vec3 ex=bb.extent(); f32 dot=(ex.x+ex.y+ex.z)*0.004f+0.5f;
            for (auto& n : beam.nodes) appendBox(bv,bi,n.x,n.y,n.z,dot);
            MeshObj cg; cg.name="Vehicle Cage"; cg.mesh=e.device()->createMesh(bv.data(),(u32)bv.size(),bi.data(),(u32)bi.size());
            cg.tris=(u32)bi.size()/3; cg.color[0]=0.85f;cg.color[1]=0.36f;cg.color[2]=0.22f;
            cg.aabbMin=bb.min; cg.aabbMax=bb.max;
            objects_.push_back(cg);
        }
    }

    for (MeshObj& o : objects_) makeMaterialFor(o);

    std::vector<rhi::LineVertex> gl; buildGrid(gl, kEditorGridHalf, kEditorGridCell);
    gridMesh_ = e.device()->createLineMesh(gl.data(), (u32)gl.size());

    // Per-mode gizmos: normal + yellow-highlight variant of each axis.
    for (int a = 0; a < 3; ++a) {
        auto mv=buildMoveAxis(a,kAxisCol[a]);   gzMove_[a]  =e.device()->createLineMesh(mv.data(),(u32)mv.size());
        auto mh=buildMoveAxis(a,kAxisHi);       gzMoveHi_[a]=e.device()->createLineMesh(mh.data(),(u32)mh.size());
        auto rr=buildRotRing(a,kAxisCol[a]);    gzRot_[a]   =e.device()->createLineMesh(rr.data(),(u32)rr.size());
        auto rh=buildRotRing(a,kAxisHi);        gzRotHi_[a] =e.device()->createLineMesh(rh.data(),(u32)rh.size());
        auto sc=buildScaleAxis(a,kAxisCol[a]);  gzScale_[a] =e.device()->createLineMesh(sc.data(),(u32)sc.size());
        auto sh=buildScaleAxis(a,kAxisHi);      gzScaleHi_[a]=e.device()->createLineMesh(sh.data(),(u32)sh.size());
    }

    // The Player Start's capsule and facing arrow, built once (like the gizmo handles just above) and
    // placed with a world matrix per frame (SandboxRender.cpp). The selected-state capsule is a
    // second, differently-coloured mesh rather than a per-draw tint -- drawLines has no colour
    // parameter, same reason gzMove_/gzMoveHi_ are two meshes instead of one recoloured in place.
    {
        auto cap = buildCapsuleWire(kPlayerStartCapsuleRadius, kPlayerStartCapsuleHalfHeight, kPlayerStartColor);
        playerStartCapsule_ = e.device()->createLineMesh(cap.data(), (u32)cap.size());
        auto capSel = buildCapsuleWire(kPlayerStartCapsuleRadius, kPlayerStartCapsuleHalfHeight,
                                        kSelectionColor);
        playerStartCapsuleSel_ = e.device()->createLineMesh(capSel.data(), (u32)capSel.size());
        auto arrow = buildPlayerStartArrow(kPlayerStartCapsuleHalfHeight, kPlayerStartArrowLength,
                                            kPlayerStartArrowColor);
        playerStartArrow_ = e.device()->createLineMesh(arrow.data(), (u32)arrow.size());
    }

#if AVER_MODULE_LANDSCAPE
    // Sculpt brush footprint ring (flat in XY plane; reused, radius/position vary per-frame).
    { auto bring = buildRotRing(2, kAxisHi); brushRing_ = e.device()->createLineMesh(bring.data(), (u32)bring.size()); }
#endif

#if AVER_MODULE_SCENE
    // Scene join, registered before Voxi (reads vertex buffers in prePass).
    skinnedScene_ = std::make_unique<aver::render::SkinnedScene>();
    if (skinnedScene_->init(*e.device())) {
        skinnedScene_->setResolvers(&game::GameContent::resolveAnimAsset, &game::GameContent::resolveSceneMesh, &content_);
        e.device()->addRenderFeature(skinnedScene_.get());
    } else {
        skinnedScene_.reset();   // skinned entities draw at rest
    }
#if AVER_MODULE_RENDER_SOFTBODY
    // Reuse skinnedScene_'s resolver pair.
    softBodyScene_ = std::make_unique<aver::render::SoftBodyScene>();
    if (softBodyScene_->init(*e.device())) {
        softBodyScene_->setResolvers(&game::GameContent::resolveSceneMesh, &game::GameContent::resolveAnimAsset, &content_);
        e.device()->addRenderFeature(softBodyScene_.get());
    } else {
        softBodyScene_.reset();   // soft entities draw their authored mesh
    }
#if AVER_MODULE_RENDER_HAIR && AVER_MODULE_HAIR_SCENE
    hairScene_ = std::make_unique<aver::render::HairScene>();
    if (hairScene_->init(*e.device())) {
        aver::hair::hairSystem().installComputeHost(&hairScene_->hostTable());   // BEFORE the first groom builds
        aver::hair::hairSystem().setPathResolver(&game::GameContent::resolveAnimAsset, &content_);
        hairScene_->setMaterialBinder([](i32 token, aver::pbr::MaterialHandle h, void* u) { static_cast<game::GameContent*>(u)->bindSurfaceMaterial(token, h); }, &content_);
        e.device()->addRenderFeature(hairScene_.get());
    } else {
        hairScene_.reset();   // hair entities stay invisible and inert
    }
#endif
#endif

    // Content browser thumbnails (inits own features; failed init -> textureId is 0).
    thumbnails_.init(*e.device());

    // Test rigged mesh through AnimSystem and SkinnedScene (--skin-scene-test <dir>).
    if (!skinSceneDir_.empty() && skinnedScene_) {
        skinScene_ = std::make_unique<aver::editor::SkinSceneTest>();
        u64 meshId = 0, skelId = 0, clipId = 0;
        u32 meshHandle = 0;
        if (skinScene_->setup(e, skinSceneDir_, &meshId, &skelId, &clipId, &meshHandle)) {
            std::pair<Vec3, Vec3> bounds;
            skinScene_->restBounds(bounds.first, bounds.second);
            content_.registerMesh(meshId, meshHandle, bounds);
            std::string d = skinSceneDir_;
            while (!d.empty() && (d.back() == 92 || d.back() == '/')) d.pop_back();
            content_.indexAsset(meshId, d + "/Rig.ocmesh");
            content_.indexAsset(skelId, d + "/Rig.ocskel");
            content_.indexAsset(clipId, d + "/Rig_Bend.ocanim");
            anim::animSystem().setResolver(&game::GameContent::resolveAnimAsset, &content_);
            objects_.clear();   // nothing unrelated on screen
            sel_ = -1;
        } else {
            skinScene_.reset();
        }
    } else if (!skinSceneDir_.empty()) {
        AVER_ERROR("[Skin] --skin-scene-test needs the scene join, which did not initialise");
    }
#endif

#if AVER_MODULE_FLUIDS
    // Register fluid (spawned only if level asks for it). Must precede Voxi (reads vertex buffer).
    water_.init(*e.device());
#if AVER_MODULE_LANDSCAPE
    // wave3: river / lake depth reads the ground; C# and graphs read the landscape layers.
    water_.setGroundHeightSource([this](f64 x, f64 y, f64& z) { return landscape_.groundHeightAt(x, y, z); });
#endif
#endif
#if AVER_MODULE_LANDSCAPE
    landscape_.installProviders();
#endif

#if AVER_FLUIDS_SIMULATED
    // 3D-viewport icon renderer (overlayPass; must exist before level loading creates Player Start).
    viewportIconsReady_ = viewportIcons_.init(*e.device());
    if (viewportIconsReady_) {
        e.device()->addRenderFeature(&viewportIcons_);
        playerStartIcon_ = viewportIcons_.loadIcon(executableDir() + AVER_PATH_SEP + "player-start-icon.png",
                                                   "PlayerStartIcon");
        if (playerStartIcon_ == editor::ViewportIconRenderer::kNoIcon) viewportIconsReady_ = false;
    }
#endif

    // Furnace test: shading model energy conservation with uniform white diffuse (--furnace-test).
    if (furnaceTest_) {
        objects_.clear();
        sel_ = -1;
        sky_.furnaceRadiance = 0.25f;
        sky_.furnaceSun = furnaceSun_;
        sunAmbient_ = 1.0f;

        // Albedo 1, fully rough, non-metallic white surfaces.
        const auto white = [&](const char* nm, Vec3 pos, Vec3 scale) {
            MeshObj o;
            o.name = nm;
            o.mesh = unitCubeMesh_;
            o.pos = pos; o.scale = scale;
            o.color[0] = o.color[1] = o.color[2] = 1.0f;
            o.metallic = 0.0f; o.roughness = 1.0f;
            o.aabbMin = Vec3{-scale.x*1.1f, -scale.y*1.1f, -scale.z*1.1f};
            o.aabbMax = Vec3{ scale.x*1.1f,  scale.y*1.1f,  scale.z*1.1f};
            objects_.push_back(o);
        };
        white("FurnaceFloor", Vec3{0, 0, -30}, Vec3{900.0f, 900.0f, 20.0f});
        white("FurnaceTall",  Vec3{0, 0, 200}, Vec3{160.0f, 160.0f, 220.0f});
        white("FurnaceCaveBack",  Vec3{-520, -520, 160}, Vec3{20.0f, 200.0f, 200.0f});
        white("FurnaceCaveLeft",  Vec3{-330, -700, 160}, Vec3{200.0f, 20.0f, 200.0f});
        white("FurnaceCaveTop",   Vec3{-330, -520, 350}, Vec3{200.0f, 200.0f, 20.0f});

        // Furnace grid: specular half (6 roughness x 3 metallic at albedo 1).
        if (furnaceGrid_) {
            objects_.clear();
            const f32 rough[6] = {0.05f, 0.25f, 0.45f, 0.65f, 0.85f, 1.0f};
            const f32 metal[3] = {0.0f, 0.5f, 1.0f};
            for (int mi = 0; mi < 3; ++mi) {
                for (int ri = 0; ri < 6; ++ri) {
                    MeshObj o;
                    char nm[64];
                    std::snprintf(nm, sizeof(nm), "Cell_m%.1f_r%.2f", metal[mi], rough[ri]);
                    o.name = nm;
                    o.mesh = unitCubeMesh_;
                    // Columns march along +Y (right), rows up +Z, on one plane at +X.
                    o.pos   = Vec3{700.0f, -450.0f + 180.0f * (f32)ri, 60.0f + 140.0f * (f32)mi};
                    // Thin plate; tilt shows glancing incidence (--furnace-tilt).
                    o.scale = Vec3{0.5f, 60.0f, 60.0f};
                    o.rotDeg = Vec3{furnaceTilt_, 0.0f, 0.0f};
                    o.color[0] = o.color[1] = o.color[2] = 1.0f;
                    o.metallic  = metal[mi];
                    o.roughness = rough[ri];
                    o.aabbMin = Vec3{-o.scale.x*1.1f, -o.scale.y*1.1f, -o.scale.z*1.1f};
                    o.aabbMax = Vec3{ o.scale.x*1.1f,  o.scale.y*1.1f,  o.scale.z*1.1f};
                    objects_.push_back(o);
                }
            }
            // Give plates materials only when a coat is asked for (normally use per-draw fallback).
            if (coatWeight_ > 0.0f) {
                for (MeshObj& o : objects_) makeMaterialFor(o);
                AVER_INFO("[Furnace] grid plates given materials so the coat has somewhere to "
                          "live (coat {:.2f} rough {:.2f} f0 {:.3f}); an uncoated run still uses "
                          "the per-draw fallback path and reads exactly what it always did.",
                          coatWeight_, coatRough_, coatF0_);
            }
            AVER_INFO("[Furnace] grid: 6 roughness x 3 metallic at albedo 1, L = {}",
                      sky_.furnaceRadiance);
        }
    }

    // Reflection test: mirror and beacon to verify ray-traced reflection is global (--refl-test).
    if (reflTest_) {
        refl_ = std::make_unique<aver::editor::ReflTest>();
        objects_.clear();
        sel_ = -1;

        // Metallic near-smooth mirror.
        MeshObj m;
        m.name = "ReflMirror";
        m.mesh = unitCubeMesh_;
        m.pos = Vec3{0, 0, -20.0f};
        m.scale = Vec3{1800.0f, 1800.0f, 20.0f};
        m.color[0] = 0.02f; m.color[1] = 0.02f; m.color[2] = 0.02f;
        m.metallic = 1.0f; m.roughness = 0.03f;
        m.aabbMin = Vec3{-1900, -1900, -60}; m.aabbMax = Vec3{1900, 1900, 20};
        objects_.push_back(m);

        const Vec3 bp = aver::editor::ReflTest::beaconPosition(camPos_, Vec3{0, 0, 0});
        MeshObj bcn;
        bcn.name = "ReflBeacon";
        bcn.mesh = unitCubeMesh_;
        bcn.pos = bp;
        // Bright green beacon (unique color; reflection lands outside voxel volume).
        bcn.scale = Vec3{700.0f, 700.0f, 700.0f};
        bcn.color[0] = 0.02f; bcn.color[1] = 0.95f; bcn.color[2] = 0.05f;
        bcn.roughness = 1.0f;
        bcn.aabbMin = Vec3{-950, -950, -950}; bcn.aabbMax = Vec3{950, 950, 950};
        objects_.push_back(bcn);
        reflBeaconIndex_ = static_cast<int>(objects_.size()) - 1;

        refl_->setBeaconPosition(bp);
        refl_->setVolumeExtent(giExtent_);
    }

    // Test GPU skinning: register before scene renderer (reads vertex buffer this writes).
    if (skinDrawTest_) {
        skinDraw_ = std::make_unique<aver::editor::SkinDrawTest>();
        if (skinDraw_->init(*e.device())) {
            e.device()->addRenderFeature(skinDraw_.get());
            objects_.clear();
            sel_ = -1;
            // Ground pale/rough (measures light reach).
            MeshObj g;
            g.name = "SkinDrawTest.Ground";
            g.mesh = skinDraw_->ground();
            g.color[0] = 0.75f; g.color[1] = 0.75f; g.color[2] = 0.72f;
            g.roughness = 0.9f;
            g.aabbMin = Vec3{-1700, -1700, -300};
            g.aabbMax = Vec3{ 1700,  1700, -160};
            objects_.push_back(g);

            MeshObj o;
            o.name = "SkinDrawTest";
            o.mesh = skinDraw_->mesh();
            o.color[0] = 0.9f; o.color[1] = 0.15f; o.color[2] = 0.9f;   // magenta: unlike ground and sky in every channel
            o.roughness = 0.6f;
            o.aabbMin = Vec3{-260, -260, -260};
            o.aabbMax = Vec3{ 260,  260,  260};
            objects_.push_back(o);

            // Ground probe is only an acceleration-structure test when RT draws the shadow --
            // told rather than guessed, so the report names which question it answered. Whether
            // RT is on is the test's own schedule to drive: the experiment IS the toggle.
            skinDraw_->setRayTracingAvailable(e.device()->caps().rayTracingTier >= 11);
        } else {
            AVER_ERROR("[Skin] draw test unavailable on this device");
            skinDraw_.reset();
        }
    }
#if AVER_MODULE_SCRIPTING
    {
        scripting::HostDesc hd;
        hd.bridgeDir = executableDir() + AVER_PATH_SEP "Scripting";
        hd.scriptsDir = resolveScriptsDir();
        scripts_.init(hd);

        // The Problems panel and the graph debugger, when the staged bridge has them: tabs opened from
        // here on get the hooks. The armed flag is a user preference, pushed to the bridge by the first
        // pumpGraphDebugger() (it applies to graphs compiled afterwards, i.e. the next Play).
        if (scripts_.graphDiagnoseAvailable()) {
            editor::setGraphDiagnoser([this](const std::string& text, std::string& wire) {
                return scripts_.graphDiagnose(text, wire);
            });
        }
        if (scripts_.graphDebugAvailable()) {
            editor::setGraphDebugValueSource([this](const std::string& graphName) {
                return scripts_.graphDebugValues(graphName);
            });
            editor::debugSession().setArmed(editor::prefBool("graphEditor.debugArmed", false));
        }

        // Install animation notify sink (survives level reload; needs AVER_MODULE_SCENE).
#if AVER_MODULE_SCENE
        if (scripts_.graphFireAvailable()) {
            anim::animSystem().setNotifySink(&SandboxApp::animNotify, this);
#if AVER_MODULE_SOFTBODY_SCENE
            softbody::softBodySystem().setNotifySink(&SandboxApp::animNotify, this);   // SoftBodyTorn / Detached / Dust / Impact / Repaired
#endif

            game::setVehicleNotify(&SandboxApp::animNotify, this);   // VehicleCrash / VehicleSkid / VehicleLanded / VehicleFlipped / VehicleShift / VehicleEntered / VehicleExited

#if AVER_MODULE_CAMERA && AVER_MODULE_FRAMEWORK
            camera::cameraRigSystem().setNotifySink(&SandboxApp::animNotify, this);   // CameraViewTargetChanged / CameraBlendFinished / CameraCut
#endif
#if AVER_MODULE_SYNAPSE_SCENE
            // Same sink for synapse (SynapsePerception.hpp).
            synapse::perceptionSystem().setNotifySink(&SandboxApp::animNotify, this);
            synapse::btSystem().setNotifySink(&SandboxApp::animNotify, this);
#if AVER_WITH_SYNAPSE_AI
            game::installAiNotify(&SandboxApp::animNotify, this);   // AI: wave 3 #21 -- EqsFinished, SmartObject* ...
#endif
#endif
        }
#endif

        // Install animation curve provider (framework relay).
#if AVER_MODULE_FRAMEWORK
        aver_fw_set_anim_curve_provider(&SandboxApp::animCurve, this);
#endif

#if AVER_MODULE_SYNAPSE_SCENE
#if AVER_MODULE_FRAMEWORK
        // Install synapse target and perception providers (Aver Node queries).
        aver_fw_set_synapse_target_provider(&SandboxApp::synapseTarget, this);
        aver_fw_set_synapse_perception_provider(&SandboxApp::synapsePerception, this);
        // Perception resolver (composition root link only).
        synapse::perceptionSystem().setTargetResolver(&SandboxApp::synapseTargetResolver, this);
#endif
#endif

        // Install save/load providers (guarded on both SCENE and FRAMEWORK).
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, &origin_);
#endif

        // Declare graph classes (retry after scripting host bootstraps).
#if AVER_MODULE_FRAMEWORK
        if (scripts_.ready() && project_.valid()) {
            game::setAiContentRoot(project_.contentDir());   // AI: wave 3 #21 -- where .oceqs / .ocso / .ocst paths resolve
            const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
            if (graphClasses > 0)
                AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());
            spawnClassPlacements();
        }
#endif

#if AVER_MODULE_VOXI
    // Set GPU capabilities and Voxi device info.
    {
        const rhi::DeviceCaps caps = e.device()->caps();

        voxi::DeviceInfo di;
        di.msaaMask = caps.msaaMask; di.maxMsaaSamples = caps.maxMsaaSamples;
        di.rayTracingTier = caps.rayTracingTier; di.computeShaders = caps.computeShaders;
        di.typedUavLoads = caps.typedUavLoads; di.conservativeRaster = caps.conservativeRaster;
        di.shaderModel = caps.shaderModel; di.meshShaderTier = caps.meshShaderTier;
        di.dxcAvailable = caps.dxcAvailable;
        // Denoiser support (D3D12-specific; mirrored in GameApp::attachVoxi).
        di.denoiserSupported = e.device()->caps().gBuffer;
        voxi::Renderer::get().setDeviceInfo(di);

        // Apply project Voxi settings BEFORE flags and BEFORE init() (createVoxelVolume uses them).
        // Before flags so flag overwrites preserve "flag > manifest" precedence.
        applyProjectVoxiSettings();

        voxi::Settings s = voxi::Renderer::get().settings();
        s.msaa = static_cast<voxi::Msaa>(e.device()->sampleCount());
        if (msaaOverride_) s.msaa = static_cast<voxi::Msaa>(msaaOverride_);
        // --no-gi/no-rt win over --gi/--rt (force-off take precedence).
        if (giForceOff_)          s.globalIllumination = voxi::Quality::Off;
        else if (giOverride_ >= 0) s.globalIllumination = static_cast<voxi::Quality>(giOverride_);
        if (rtForceOff_)          s.rayTracing = voxi::Quality::Off;
        else if (rtOverride_ >= 0) s.rayTracing = static_cast<voxi::Quality>(rtOverride_);
        if (ptOverride_ >= 0) s.pathTracing = static_cast<voxi::Quality>(ptOverride_);
        if (msOverride_) s.meshShaders = true;
        // Apply tier settings first, then knobs in second call (setSettings re-runs each frame).
        voxi::Renderer::get().setSettings(s);
        auto k = voxi::Renderer::get().settings();
        if (rtRaysOverride_ > 0) k.rtShadowRays = static_cast<u32>(rtRaysOverride_);
        else if (rtRaysOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-rays {} is not a ray count; the default of {} stands",
                      rtRaysOverride_, k.rtShadowRays);
        if (rtShadowDenoiseOverride_ >= 0) k.rtShadowDenoise = static_cast<u32>(rtShadowDenoiseOverride_);
        if (rtRenderModeOverride_    >= 0) k.rtRenderMode    = static_cast<u32>(rtRenderModeOverride_);
        if (ptBouncesOverride_       >= 0) k.ptBounces       = static_cast<u32>(ptBouncesOverride_);
        if (layeredBsdfOverride_     >= 0) k.layeredBsdf     = static_cast<voxi::Quality>(layeredBsdfOverride_);
        // GI sky occlusion rays derive from RT tier change (set in second call only).
        if (giSkyOccRaysOverride_ >= 0) k.giSkyOcclusionRays = static_cast<u32>(giSkyOccRaysOverride_);
        if (giSkyOccTileOverride_ >= 0) k.giSkyOcclusionTile = static_cast<u32>(giSkyOccTileOverride_);
        if (rtPixelsPerRayOverride_ > 0) k.rtPixelsPerRayTile = static_cast<u32>(rtPixelsPerRayOverride_);
        else if (rtPixelsPerRayOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-pixels-per-ray {} is not a tile edge; the default of {} stands",
                      rtPixelsPerRayOverride_, k.rtPixelsPerRayTile);
        // Refraction mode is tier-derived (set in second call only).
        if (refractionOverride_ >= 0)          k.refractionMode = static_cast<u32>(refractionOverride_);
        if (refractionStrengthOverride_ >= 0.0f) k.refractionStrength = refractionStrengthOverride_;
        if (refractionFadeOverride_ >= 0.0f)     k.refractionEdgeFade = refractionFadeOverride_;
        if (giUpdateIntervalOverride_ > 0) k.giUpdateInterval = static_cast<u32>(giUpdateIntervalOverride_);
        else if (giUpdateIntervalOverride_ < 0)
            AVER_WARN("[Sandbox] --gi-update-interval {} is not a frame count; the default of {} stands",
                      giUpdateIntervalOverride_, k.giUpdateInterval);
        if (giModeOverride_ >= 0) k.giMode = static_cast<u32>(giModeOverride_);
        if (denoiserOverride_ >= 0) voxi::setDenoiserMode(k, denoiserOverride_ > 2 ? 2u : static_cast<u32>(denoiserOverride_));
        voxi::Renderer::get().setSettings(k);
        AVER_INFO("[Voxi] attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}", caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);

        // VoxiRenderer non-owning; torn down in onShutdown. Read back settings to see clamping.
        voxiRenderer_.setSettings(voxi::Renderer::get().settings());
        // NRD2's network: the user's trained weights over the shipped ones.
        voxiRenderer_.setNrd2WeightsPaths(
            aver::userDataDir().empty() ? std::string() : aver::userDataDir() + AVER_PATH_SEP + render::denoise::kNrd2WeightsFileName,
            aver::executableDir() + AVER_PATH_SEP "data" AVER_PATH_SEP + render::denoise::kNrd2WeightsFileName);
        if (frameTimeReport_) voxiRenderer_.setFrameTimeReport(true);
        // Set measurement dials before init().
        if (rdAblate_) {
            voxiRenderer_.setRayDrivenAblation(static_cast<u32>(rdAblate_));
            AVER_WARN("[Sandbox] --rd-ablate {}: the ray-driven frame is DELIBERATELY WRONG. "
                      "This is a stopwatch for attributing PSRayDriven's cost, not a quality "
                      "setting -- see AVER_RD_ABLATE in voxi.hlsl.", rdAblate_);
        }
        if (rtDenoiseMotionTaper_ > 0.0f) {
            voxiRenderer_.setRtDenoiseMotionTaper(rtDenoiseMotionTaper_);
            AVER_INFO("[Sandbox] --rt-denoise-motion {}: the shadow denoiser will fade out "
                      "as the gather centre reprojects. 0 (the default) is no taper.",
                      rtDenoiseMotionTaper_);
        }
        if (voxiRenderer_.init(*e.device())) {
            e.device()->addRenderFeature(&voxiRenderer_);
            voxiAttached_ = true;
            if (foliagePending_) {
                foliagePending_ = false;
                levelFoliage_.load(foliagePendingWorld_, content_, e.device(), &voxiRenderer_, project_.contentDir(),
                                   {}, camPos_, &foliagePendingTables_);
                if (!levelFoliage_.result().error.empty()) AVER_WARN("[Foliage] {}", levelFoliage_.result().error);
            }
            if (projectRenderPending_) applyProjectRenderSettings();
            if (saveProject_ && !saveProjectDone_) { saveProjectDone_ = true; seedAndSaveProject(); }
#if AVER_WITH_IMGUI
            // Deferred import handshake (UI-only; needs browser).
            if (!importSrc_.empty() && !importDone_) {
                importDone_ = true;
                importAsset(importSrc_, importDst_);
            }
#endif
#if AVER_MODULE_PBR
            content_.setTextureFactory(e.device()->resources());
            content_.setMaterialSystem(&voxiRenderer_.materials());
            voxiRenderer_.materials().setTextureResolver(&game::GameContent::resolveMaterialTexture, &content_);
            voxiRenderer_.materials().setTerrainResolver(&game::GameContent::resolveTerrainAtlas, &content_);   // wave3 terrain layers
            // Same resolver for asset tabs' preview (consistent look across tabs and level).
            editor::setPreviewTextureResolver(&game::GameContent::resolveMaterialTexture, &content_);
            editor::setPreviewMaterialLookup(
                [](const std::string& surface, void* user) -> u32 {
                    return static_cast<u32>(static_cast<game::GameContent*>(user)->materialForSurface(surface));
                }, &content_);
#endif
            // Depth proxy installed unconditionally (empty on builds without Trifactor).
            voxiRenderer_.setDepthProxy(&SandboxApp::depthProxyLookup, this);
        }
    }
#endif
    // The game UI's render feature: overlay pass only, so ordering against scene features is free.
    gameUi_ = aver::render::ui::UiRenderer::create(*e.device());
    if (gameUi_) e.device()->addRenderFeature(gameUi_);
    game::uiTextBindRenderer(gameUi_);   // the glyph atlas for Play/preview text (the font set itself loads when a project opens)
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // Sub-viewports (G4 R5): nothing is created until a Play session asks for a view. The UI forgets its cached binding when a view's texture is rebuilt.
    subViews_.attach(*e.device(), [](rhi::TextureHandle t, void* self) {
        if (auto* ui = static_cast<SandboxApp*>(self)->gameUi_) ui->dropBinding(t);
    }, this);
#endif

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // Occlusion culler (not IRenderFeature; calls renderSceneEntities() directly).
    if (rhi::IResourceFactory* occRes = e.device()->resources()) {
        occluder_ = aver::occlusion::createOcclusionCuller(*occRes);
        // Sync --occlusion-waitidle flag (console and CLI stay in sync).
        editor::consoleOcclusionForceWaitIdleSlot() = occlusionDebugForceWaitIdleArg_;
        occluder_->setDebugForceWaitIdle(occlusionDebugForceWaitIdleArg_);
        // Warm up compute pipelines before frame loop (required for pointer stability).
        rhi::TextureDesc occSceneDesc;
        if (const rhi::TextureHandle occDepth = e.device()->sceneDepthTexture();
            occDepth && occRes->textureInfo(occDepth, occSceneDesc)) {
            occluder_->ensureSized(*occRes, occSceneDesc.width, occSceneDesc.height, e.device()->sampleCount());
        }
    }
#endif

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Particle renderer (registered unconditionally; system/effects are singletons).
    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    {
        const int pm = particleGpuArg_ >= 0 ? particleGpuArg_ : (project_.particleGpu >= 0 ? project_.particleGpu : 1);
        particles::particleSystem().setGpuMode(pm);
        particleRenderer_.setGpuEnabled(pm != 0);
        particleRenderer_.setTextureResolver(&game::GameContent::resolveParticleTexture, &content_);
        particleRenderer_.setMeshResolver(&game::GameContent::resolveParticleMesh, &content_);
    }
    if (particleRenderer_.init(*e.device())) {
        particleRenderer_.setSystem(&particles::particleSystem());
        e.device()->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // Install GI seam once Voxi has attached (gated on --no-particle-gi).
        if (voxiAttached_ && !noParticleGi_) {
            particles::ParticleRenderer::GiSeam seam;
            seam.prepare = &SandboxApp::particleGiPrepare;
            seam.bind = &SandboxApp::particleGiBind;
            seam.user = this;
            particleRenderer_.setGiSeam(seam);
        }
#endif
    } else {
        AVER_ERROR("[Particles] renderer unavailable on this device");
    }

#if AVER_MODULE_FLUIDS
    // Water renderer (failed init means no water; HLSL compiles at runtime).
    if (waterEnabled_) {
        // Default swell: 4 non-harmonic waves.
        fluids::GerstnerWave waves[4];
        const f32 dirs[4][2] = {{1.0f, 0.15f}, {0.6f, -0.8f}, {-0.3f, 0.95f}, {-0.85f, -0.5f}};
        const f32 lengths[4] = {1450.0f, 890.0f, 520.0f, 310.0f};
        const f32 amps[4]    = {34.0f, 19.0f, 9.0f, 4.5f};
        for (int i = 0; i < 4; ++i) {
            waves[i].dirX = dirs[i][0];
            waves[i].dirZ = dirs[i][1];
            waves[i].wavelengthCm = lengths[i];
            waves[i].amplitudeCm  = amps[i];
            waves[i].steepness    = 0.75f;
        }
        if (water_.attachSurface(*e.device(), waterHeightCm_, waves, 4)) {
            // Set physics plane from water level (rendering and buoyancy are separate capabilities).
#if AVER_MODULE_PHYSICS
            const f32 normal[3]  = {0.0f, 0.0f, 1.0f};
            const f32 current[3] = {0.0f, 0.0f, 0.0f};
            aver_phys_set_water_plane(water_.waterLevelCm(), normal,
                                      1.0f, 0.5f, 0.05f, current);
#endif
            AVER_INFO("[Water] surface and buoyancy plane at z = {} cm", waterHeightCm_);
        } else {
            AVER_ERROR("[Water] renderer unavailable on this device");
        }
#else
    if (waterEnabled_)
        AVER_WARN("[Water] --water was given, but this build has no water module");
#endif
    }

    // Particle test: dust cloud overlapping cube (shows transparent pass depth test).
    if (particleTest_) {
        objects_.clear();
        sel_ = -1;

        MeshObj occluderCube;
        occluderCube.name = "ParticleTest.Occluder";
        occluderCube.mesh = unitCubeMesh_;
        occluderCube.pos = Vec3{600.0f, 0.0f, 50.0f};
        occluderCube.scale = Vec3{80.0f, 80.0f, 80.0f};
        occluderCube.color[0] = 0.55f; occluderCube.color[1] = 0.5f; occluderCube.color[2] = 0.45f;
        occluderCube.roughness = 0.8f;
        occluderCube.aabbMin = Vec3{512.0f, -88.0f, -38.0f};
        occluderCube.aabbMax = Vec3{688.0f,  88.0f, 138.0f};
        objects_.push_back(occluderCube);

        // Dust cloud spans well in front of AND behind the cube along the camera's forward axis
        // (+X) -- that spread makes occlusion visible in one static frame, no motion needed.
        particles::ParticleEffect fx;
        fx.shape = particles::EmitterShape::Box;
        fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
        fx.emissionRate = 150.0f;
        fx.burstCount = 0;
        fx.maxParticles = 400;
        fx.lifetimeMin = 3.0f;
        fx.lifetimeMax = 5.0f;
        fx.direction = Vec3{0.0f, 0.0f, 1.0f};
        fx.spreadDeg = 60.0f;
        fx.speedMin = 5.0f;
        fx.speedMax = 15.0f;
        fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
        fx.damping = 0.3f;
        fx.sizeStart = 25.0f;
        fx.sizeEnd = 45.0f;
        fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
        fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
        fx.blend = rhi::BlendMode::PremultipliedAlpha;
        constexpr u64 kDustCloudEffectId = 0x50415254'44550001ull;   // arbitrary, non-zero
        particles::particleEffects().set(kDustCloudEffectId, fx);

        scene::World& world = scene::World::instance();
        Transform xf;
        xf.position = occluderCube.pos;
        const scene::Entity emitter = world.create("ParticleTest.DustCloud", scene::kInvalidEntity, xf);
        if (auto* emitterComp = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emitter, scene::kComponentParticleEmitter))) {
            emitterComp->effect = kDustCloudEffectId;
        }
        AVER_INFO("[Particles] --particle-test: dust cloud entity {} around effect 0x{:016X}",
                  emitter, kDustCloudEffectId);

        // Ember emitter (receivesGI=false; additive; non-GI part of seam test).
        particles::ParticleEffect emberFx;
        emberFx.shape = particles::EmitterShape::Sphere;
        emberFx.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
        emberFx.emissionRate = 80.0f;
        emberFx.burstCount = 0;
        emberFx.maxParticles = 200;
        emberFx.lifetimeMin = 1.0f;
        emberFx.lifetimeMax = 1.6f;
        emberFx.direction = Vec3{0.0f, 0.0f, 1.0f};
        emberFx.spreadDeg = 35.0f;
        emberFx.speedMin = 40.0f;
        emberFx.speedMax = 80.0f;
        emberFx.gravity = Vec3{0.0f, 0.0f, -25.0f};
        emberFx.damping = 0.1f;
        emberFx.sizeStart = 22.0f;
        emberFx.sizeEnd = 6.0f;
        emberFx.colorStart[0] = 1.0f; emberFx.colorStart[1] = 0.55f; emberFx.colorStart[2] = 0.12f; emberFx.colorStart[3] = 1.0f;
        emberFx.colorEnd[0]   = 1.0f; emberFx.colorEnd[1]   = 0.15f; emberFx.colorEnd[2]   = 0.02f; emberFx.colorEnd[3] = 0.0f;
        emberFx.blend = rhi::BlendMode::Additive;
        emberFx.receivesGI = false;   // DECIDED 4: an ember is its own light source
        constexpr u64 kEmberEffectId = 0x50415254'45420001ull;   // arbitrary, non-zero
        particles::particleEffects().set(kEmberEffectId, emberFx);

        // High above dust cloud, open sky (tests transparent-pass/sky ordering).
        Transform emberXf;
        emberXf.position = occluderCube.pos + Vec3{0.0f, 0.0f, 220.0f};
        const scene::Entity emberEmitter =
            world.create("ParticleTest.Embers", scene::kInvalidEntity, emberXf);
        if (auto* emberComp = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emberEmitter, scene::kComponentParticleEmitter))) {
            emberComp->effect = kEmberEffectId;
        }
        AVER_INFO("[Particles] --particle-test: ember entity {} around effect 0x{:016X} (receivesGI=false)",
                  emberEmitter, kEmberEffectId);
    }
    // Particle stress: N grid emitters (measures cost, tests parity/price pass).
    else if (particleStressEmitters_ > 0) {
        objects_.clear();
        sel_ = -1;

        particles::ParticleEffect fx;
        fx.shape = particles::EmitterShape::Box;
        fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
        fx.emissionRate = 150.0f;   // steady fog/mist emission rate
        fx.maxParticles = static_cast<u32>(particleStressMaxParticles_ > 0 ? particleStressMaxParticles_ : 400);
        // Burst whole cap on frame 1 to reach steady-state cost immediately.
        fx.burstCount = fx.maxParticles;
        // Long lifetime keeps burst-filled population steady across capture window.
        fx.lifetimeMin = 120.0f;
        fx.lifetimeMax = 120.0f;
        fx.direction = Vec3{0.0f, 0.0f, 1.0f};
        fx.spreadDeg = 60.0f;
        fx.speedMin = 5.0f;
        fx.speedMax = 15.0f;
        fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
        fx.damping = 0.3f;
        fx.sizeStart = 25.0f;
        fx.sizeEnd = 45.0f;
        fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
        fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
        fx.blend = rhi::BlendMode::PremultipliedAlpha;
        constexpr u64 kStressEffectId = 0x50415254'53545301ull;   // "PART" + "STS\1", arbitrary
        particles::particleEffects().set(kStressEffectId, fx);

        scene::World& world = scene::World::instance();
        const int n = particleStressEmitters_;
        const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<f64>(n))));
        const f32 spacing = 140.0f;
        for (int idx = 0; idx < n; ++idx) {
            const int col = idx % cols;
            const int row = idx / cols;
            Transform xf;
            xf.position = Vec3{600.0f,
                                (static_cast<f32>(col) - static_cast<f32>(cols - 1) * 0.5f) * spacing,
                                50.0f + static_cast<f32>(row) * spacing};
            const scene::Entity emitter =
                world.create("ParticleStress.Emitter", scene::kInvalidEntity, xf);
            if (auto* stressComp = static_cast<scene::CParticleEmitter*>(
                    world.addComponent(emitter, scene::kComponentParticleEmitter))) {
                stressComp->effect = kStressEffectId;
            }
        }
        AVER_INFO("[Particles] --particle-stress: {} emitter(s), {} max particles each, effect 0x{:016X}",
                  n, fx.maxParticles, kStressEffectId);

        // Add second additive emitter to cover both blend pipelines.
        if (particleStressSecondEmitter_) {
            particles::ParticleEffect fx2;
            fx2.shape = particles::EmitterShape::Sphere;
            fx2.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
            fx2.emissionRate = 80.0f;
            fx2.burstCount = 0;
            fx2.maxParticles = 200;
            fx2.lifetimeMin = 1.0f;
            fx2.lifetimeMax = 1.6f;
            fx2.direction = Vec3{0.0f, 0.0f, 1.0f};
            fx2.spreadDeg = 35.0f;
            fx2.speedMin = 40.0f;
            fx2.speedMax = 80.0f;
            fx2.gravity = Vec3{0.0f, 0.0f, -25.0f};
            fx2.damping = 0.1f;
            fx2.sizeStart = 22.0f;
            fx2.sizeEnd = 6.0f;
            fx2.colorStart[0] = 1.0f; fx2.colorStart[1] = 0.55f; fx2.colorStart[2] = 0.12f; fx2.colorStart[3] = 1.0f;
            fx2.colorEnd[0]   = 1.0f; fx2.colorEnd[1]   = 0.15f; fx2.colorEnd[2]   = 0.02f; fx2.colorEnd[3] = 0.0f;
            fx2.blend = rhi::BlendMode::Additive;
            fx2.receivesGI = false;
            constexpr u64 kStressEffect2Id = 0x50415254'53545302ull;
            particles::particleEffects().set(kStressEffect2Id, fx2);

            Transform xf2;
            xf2.position = Vec3{600.0f, 0.0f, 270.0f};
            const scene::Entity emitter2 =
                world.create("ParticleStress.Emitter2", scene::kInvalidEntity, xf2);
            if (auto* c2 = static_cast<scene::CParticleEmitter*>(
                    world.addComponent(emitter2, scene::kComponentParticleEmitter))) {
                c2->effect = kStressEffect2Id;
            }
            AVER_INFO("[Particles] --particle-stress: diag second emitter {} effect 0x{:016X}",
                      emitter2, kStressEffect2Id);
        }
    }
#endif

    // GPU skinning self-test (--skin-test; costs waitIdle).
    if (skinTest_) {
        skinSelfTest_ = std::make_unique<aver::render::SkinSelfTest>();
        if (skinSelfTest_->init(*e.device())) e.device()->addRenderFeature(skinSelfTest_.get());
        else { AVER_ERROR("[Skin] self-test unavailable on this device"); skinSelfTest_.reset(); }
    }

    // Path tracer furnace test (energy conservation; --pt-furnace).
    if (ptFurnaceTest_) {
        ptFurnace_ = std::make_unique<aver::pt::PtFurnaceTest>();
        if (ptFurnace_->init(*e.device())) e.device()->addRenderFeature(ptFurnace_.get());
        else { AVER_ERROR("[PT] furnace unavailable on this device"); ptFurnace_.reset(); }
    }

    // Path tracer scene view (progressive still-camera reference; --pt-scene).
    syncPtSceneView(e.device());

        tools_.setAutoCompileFlag(autoCompileFlag());
        tools_.setReloader([this](const std::string& binDir, std::string* status) {
            return reloadScripts(binDir, status);
        });
    }
#endif
#if AVER_MODULE_SCENE
    initOrigin(e);   // LARGE_WORLD: the floating origin's bindings and participants (SandboxOrigin.cpp)
#endif
    tool_ = initialTool_;
    sel_ = 1;
    bool framedByLevel = false;
#if AVER_MODULE_SCENE
    framedByLevel = !levelEntities_.empty();
#endif
    // Restored camera counts as framed (level can have CAMERA record with no placements).
    if (levelCameraRestored_) framedByLevel = true;
    if (!framedByLevel) {
        camPos_ = Vec3{700.0f, 700.0f, 450.0f};
        const Vec3 d = (Vec3{0,0,1} - camPos_).getSafeNormal();
        yaw_ = std::atan2(d.y, d.x);
        pitch_ = std::asin(d.z);
    }
    // Apply --cam override last (outranks level framing and default).
    if (camOverride_) {
        camPos_ = camPosOverride_;
        pitch_  = pitchOverride_;
        yaw_    = yawOverride_;
    }
}

} // namespace aver
