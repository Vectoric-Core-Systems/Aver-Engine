// Entry point: createApplication and the command line it parses.
// Split from SandboxApp.cpp; some flags live in SandboxRender.cpp instead.

#include "aver/runtime/EntryPoint.hpp"
#include "SandboxApp.hpp"

namespace aver {

// Defined in SandboxRender.cpp; called by draw sites.
void setNoWalkCacheArg(bool on);

// True for a path ending in `ext` (lower-case, with dot), case-insensitively.
static bool hasExtension(const char* p, const char* ext) {
    const usize n = std::strlen(p), e = std::strlen(ext);
    if (n <= e) return false;
    const char* got = p + n - e;
    for (usize i = 0; i < e; ++i) {
        char a = got[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != ext[i]) return false;
    }
    return true;
}

// True for a path ending in ".ocproject", case-insensitively.
static bool isOcproject(const char* p) { return hasExtension(p, ".ocproject"); }

// True for .ocmap or .ocworld (same meaning for this editor).
bool isLevelFile(const char* p) {
    return hasExtension(p, ".ocmap") || hasExtension(p, ".ocworld");
}

// Find the .ocproject owning mapPath by walking up (up to 8 levels, then stop).
std::string ownerProjectOf(const std::string& mapPath) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(mapPath).parent_path();
    for (int up = 0; up < 8 && !dir.empty(); ++up) {
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (!it->is_directory(ec) && isOcproject(it->path().string().c_str()))
                return it->path().string();
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;                  // reached the root; parent_path() stops changing
        dir = parent;
    }
    return {};
}

// Parse command line and build the editor. Flags run in separate loops due to MSVC C1061 nesting limit.
Application* createApplication(int argc, char** argv) {
#ifdef NDEBUG
    AVER_INFO("[Sandbox] {} (Release build)", argc > 0 ? argv[0] : "Sandbox.exe");
#else
    AVER_WARN("[Sandbox] {} (DEBUG build: frame rates are not representative)", argc > 0 ? argv[0] : "Sandbox.exe");
#endif
    u16 mcpPort=0;
    // mcpRequested: --mcp given. mcpPortExplicit: given WITH a numeric port.
    bool mcpRequested = false, mcpPortExplicit = false;
    // Flags below run in own loops to avoid consuming values in fallback paths.
    int rdAblate = 0;
    // --gi-mode: -1 means "not given" (0 is the real value).
    int giModeArg = -1;
    int rdStagesArg = -1;   // --rd-stages 0|1|2
    int denoiserArg = -1;   // --denoiser 0|1
    // Refraction tier overrides; -1 is "not given".
    int refraction = -1;
    f32 refractionStrength = -1.0f, refractionFade = -1.0f;
    // --resize-cycle: resize window every N frames.
    int resizeCycleArg = 0;
    int frameInterpArg = -1;   // --frame-interp 0|1|2
    bool vsyncOn = false;   // --vsync
    f32 camWanderAmp = 0.0f, camWanderSpeed = 1.0f;   // --cam-wander AMP SPEED
    int frameInterpTrajectory = -1;  // --frame-interp-trajectory: 0 linear, 1 quadratic, 2 neural
    bool frameInterpTrain = false;   // --frame-interp-train
    int neurafiVizArg = -1;          // --neurafi-view 0-4
    bool neurafiGeneratedOnly = false;  // --neurafi-generated-only
    int neuracViewArg = -1;          // --neurac-view 0-4
    bool neuracGrid = false;         // --neurac-grid
    int pieCamArg = 0;
    int inputStuckArg = 0;
    int inputSourceArg = 0;
    int wheelSpeedArg = 0;
    int multiSelArg = 0;
    const char* cbMoveArg = nullptr;
    int saveDirtyArg = 0;
    int prefsWriteArg = 0;
    // --notify-test N: raise notifications N frames in.
    int notifyTestArg = 0;
    f32 autosaveTestArg = 0.0f;
    f32 frameBudgetArg = 0.0f;
    const char* findRefsArg = nullptr;
    int projectSwitchArg = 0;
    const char* validateGraphArg = nullptr;
    int graphPrintArg = 0;
    int assetAssignArg = 0;
    const char* graphHitsArg = nullptr;
    int renameRepointArg = 0;
    int recaptureArg = 0;
    int viewmodelArg = 0;
    // --gpu-timing: gets its own loop (trailing flag handling).
    bool gpuTimingArg = false;
    f32 rtDenoiseMotionArg = 0.0f;   // --rt-denoise-motion
    // --lighting-legacy: seed consoleLightingLegacySlot() directly (no console on --frames).
    int lightingLegacyArg = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--rd-ablate"))            rdAblate = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--lighting-legacy"))      lightingLegacyArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--gi-mode"))              giModeArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--rd-stages"))            rdStagesArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--denoiser"))              denoiserArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--rt-denoise-motion"))     rtDenoiseMotionArg = (f32)std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--resize-cycle"))         resizeCycleArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--pie-camera-test"))      pieCamArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--input-stuck-test"))     inputStuckArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--input-source-test"))    inputSourceArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--wheel-speed-test"))     wheelSpeedArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--multiselect-test"))     multiSelArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cbmove-test"))          cbMoveArg = argv[i + 1];
        if (!std::strcmp(argv[i], "--savedirty-test"))       saveDirtyArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--prefs-write-test"))     prefsWriteArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--notify-test"))          notifyTestArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--autosave-test"))        autosaveTestArg = (f32)std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--frame-budget"))         frameBudgetArg = (f32)std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--find-refs"))            findRefsArg = argv[i + 1];
        if (!std::strcmp(argv[i], "--project-switch-test")) projectSwitchArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--validate-graph"))      validateGraphArg = argv[i + 1];
        if (!std::strcmp(argv[i], "--graph-print-test"))   graphPrintArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--asset-assign-test")) assetAssignArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--graph-hits-test"))   graphHitsArg = argv[i + 1];
        if (!std::strcmp(argv[i], "--rename-repoint-test")) renameRepointArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--recapture-test"))       recaptureArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--viewmodel-test"))       viewmodelArg = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--refraction"))           refraction = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--refraction-strength"))  refractionStrength = (f32)std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--refraction-fade"))      refractionFade = (f32)std::atof(argv[i + 1]);
    }
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--gpu-timing")) gpuTimingArg = true;
    // --unlit: view mode from command line.
    bool unlitArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--unlit")) unlitArg = true;
    // --pt-legacy-env: seed consolePtLegacyEnvSlot() directly.
    bool ptLegacyEnvArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--pt-legacy-env")) ptLegacyEnvArg = true;
    // --clear-shader-cache: maintenance action.
    // --luma-sweep [STRIDE]: optional frames-between-samples (default 1).
    bool lumaSweepArg = false;
    int lumaSweepStrideArg = 1;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--luma-sweep")) {
            lumaSweepArg = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const int s = std::atoi(argv[i + 1]);
                lumaSweepStrideArg = s > 0 ? s : 1;
            }
        }
    // --firefly-metric [MULT]: outlier threshold multiplier (default 8.0).
    bool fireflyMetricArg = false;
    f32 fireflyMultArg = 8.0f;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--firefly-metric")) {
            fireflyMetricArg = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const f32 m = (f32)std::atof(argv[i + 1]);
                fireflyMultArg = m > 0.0f ? m : 8.0f;
            }
        }
    bool clearShaderCacheArg = false;
    std::string clearShaderCacheDirArg;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--clear-shader-cache")) {
            clearShaderCacheArg = true;
            // Path optional; must not look like a flag.
            if (i + 1 < argc && argv[i + 1][0] != '-') clearShaderCacheDirArg = argv[i + 1];
        }
    std::string saveLevelArg;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--save-level")) saveLevelArg = argv[i + 1];
    bool rayProbeArg = false; f32 rayProbeXArg = 0.0f, rayProbeYArg = 0.0f;
    for (int i = 1; i + 2 < argc; ++i)
        if (!std::strcmp(argv[i], "--ray-probe")) {
            rayProbeArg = true;
            rayProbeXArg = static_cast<f32>(std::atof(argv[i + 1]));
            rayProbeYArg = static_cast<f32>(std::atof(argv[i + 2]));
        }
    // --cam-translate SPEED: translation through the scene.
    f32 camTranslateArg = 0.0f;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--cam-translate")) camTranslateArg = (f32)std::atof(argv[i + 1]);
    // --cam-wobble-stop N and --set NAME VALUE: see their setters' comments.
    i32 camWobbleStopArg = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--cam-wobble-stop")) camWobbleStopArg = std::atoi(argv[i + 1]);
    std::vector<std::pair<std::string, std::string>> consoleSetArgs;
    for (int i = 1; i + 2 < argc; ++i)
        if (!std::strcmp(argv[i], "--set")) consoleSetArgs.emplace_back(argv[i + 1], argv[i + 2]);
    // --no-occlusion-cull: reapplied every frame (manifest may override).
    bool noOcclusionCullArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-occlusion-cull")) noOcclusionCullArg = true;
    // --occlusion-waitidle / --no-occlusion-waitidle: force or release testBatch()'s res.waitIdle().
    bool occlusionWaitIdleArg = false, occlusionNoWaitIdleArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--occlusion-waitidle")) occlusionWaitIdleArg = true;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-occlusion-waitidle")) occlusionNoWaitIdleArg = true;
    // --no-walk-cache: disables WalkLookup's mesh-id GameContent probes.
    bool noWalkCacheArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-walk-cache")) noWalkCacheArg = true;
    // --sun-set-at N ELEV AZIM / --gi-history-reset-at N: verification-only.
    int sunSetAtArg = 0, giHistoryResetAtArg = 0;
    f32 sunSetElevArg = 0.0f, sunSetAzimArg = 0.0f;
    for (int i = 1; i + 3 < argc; ++i)
        if (!std::strcmp(argv[i], "--sun-set-at")) {
            sunSetAtArg   = std::atoi(argv[i + 1]);
            sunSetElevArg = static_cast<f32>(std::atof(argv[i + 2]));
            sunSetAzimArg = static_cast<f32>(std::atof(argv[i + 3]));
        }
    // --gpu-validation: D3D12 GPU-based validation; set before device creation.
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--gpu-validation")) aver::rhi::setGpuValidationEnabled(true);
    // --sun-sweep START DEG_PER_FRAME: azimuth turns every frame.
    int sunSweepAtArg = 0, sunSweepTurnsArg = 0;
    f32 sunSweepDegArg = 0.0f;
    for (int i = 1; i + 2 < argc; ++i)
        if (!std::strcmp(argv[i], "--sun-sweep")) {
            sunSweepAtArg  = std::atoi(argv[i + 1]);
            sunSweepDegArg = static_cast<f32>(std::atof(argv[i + 2]));
        }
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--sun-sweep-frames")) sunSweepTurnsArg = std::atoi(argv[i + 1]);
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--gi-history-reset-at")) giHistoryResetAtArg = std::atoi(argv[i + 1]);

    u64 frames=0; bool headless=false, focusVoxi=false, focusScript=false, focusTools=false, focusCompileMenu=false, focusCompile=false, startScreen=false; int drawerOpen=0; std::string drawerSub; std::string beam, shot, project, scriptsDir, spawnTest; std::string shaderSourceDir; bool playTest=false; bool playWalk=false; bool skinTest=false; bool skinDrawTest=false; bool particleTest=false; bool noParticleGi=false; int particleStressEmitters=0; int particleStressMaxParticles=0; bool particleStressSecondEmitter=false; bool reflTest=false; bool furnaceTest=false; bool furnaceSun=false; bool furnaceGrid=false; f32 furnaceTilt=0.0f; bool ptFurnace=false; bool ptScene=false; int deviceLostAt=0; int ptQualityRamp=0; int ptSceneToggleOn=0; int ptSceneToggleOff=0; int aversrCycle=0; int projectSettingsPage=-1; f32 sunAngle=-1.0f; std::string skinSceneDir; Tool tool=Tool::Select; int msaa=0; int gi=-1; int rt=-1; int rtRays=0; int rtPixelsPerRay=0; int rtShadowDenoise=-1; int rtRenderMode=-1; int pt=-1; int ptBounces=-1; int layeredBsdf=-1; f32 coatWeight=0.0f; f32 coatRough=0.1f; f32 coatF0=0.04f; int giUpdateInterval=0; f32 renderScale=1.0f; std::string aversrArg; bool frameTime=false; bool noGi=false; bool noRt=false; bool giConeOff=false; f32 camWobbleDeg=0.0f; int camWobblePeriod=0; bool giDbg=false, ms=false; u32 probeX=0, probeY=0; f32 probeU=-1.0f, probeV=-1.0f; bool camSet=false; f32 camX=0, camY=0, camZ=0, camPitch=0, camYaw=0; int reloadAt=0; bool warp=false, debugLayer=false; bool dred=false; std::string backendName; const char* forceCaps=nullptr; f32 bloom=0.0f, exposure=1.0f; bool bloomSet=false, exposureSet=false; bool autoExposure=false; int clouds=0; f32 cloudCover=-1.0f; bool skyPhysical=false, skyAuthored=false; f32 skyElevation=-999.0f; bool vsyncOff=false; bool uiDemo=false; bool inputProbe=false; bool autoCompile=false; bool showPrefs=false; bool scrollPrefsToKeybinds=false; bool saveProject=false; std::string importSrc, importDst; int focusLevelAt=0; int hudTest=-1; std::string openAsset; std::string selectEntity; bool openLegacy=false; bool waterOn=false; f32 waterHeight=0.0f; std::string graphSelectNode; std::string graphTab; int chunkStream=0; int droneAuto=0; int undoTestAuto=0; int keybindTestAuto=0; std::string keybindTestMode; std::string droneGraph; std::string landscapePath; bool fogMatch=false; f32 fogMatchOpacity=-1.0f; bool lodSelect=true; f32 lodErrorPx=1.0f; bool lodClusterStats=false; bool lodPerCluster=false; int lodMeshShader=-1; bool depthPrepass=false; bool edgeAa=false; bool occlusionCull=false; bool bakeNav=false; f32 bakeNavCell=50.0f; std::string openMap; bool gbuffer=false; std::string gbufferDebug; std::string crashTest; std::string startMode;
    bool openLevelPickerArg=false; std::string openLevelArg; bool noEditorChrome=false; bool sceneCensus=false;
    // -1 is absent; 0 is a real value (estimate AO from cone gather).
    int giSkyOccRays = -1;
    // -1 is absent; 1 is real (fresh direction per pixel).
    int giSkyOccTile = -1;
    f32 skyLightArg = -1.0f;   // --sky-light N; negative = absent
    f32 giIntensityArg = -1.0f;   // --gi-intensity F; negative = absent
    // -1 = absent (0 is real for both: tonemap legacy curve, max-radiance no clamp).
    int tonemapArg = -1; f32 maxRadianceArg = -1.0f;
    bool windowedArg = false;
    bool fullscreenArg = false;
    // Optimisation dials; -1 = absent (0 is real: forced OFF).
    int giForceRebuildArg = -1;
    int giBoundedDispatchArg = -1;
    int giFreeAccumulatorArg = -1;
    std::string meshHeapArg;   // empty = absent
    int lodShareVerticesArg = -1;
    // U1 optimisation dials; parsed and applied after the loop.
    std::string restirVisibilityArg;   // empty = absent; none|reconstructed|half|full
    bool giVisPathViewArg = false;     // --gi-vis-path-view
    std::string blendedGiArg;          // empty = absent; restir|cone
    // --view-mode: headless twin of viewport dropdown.
    std::string viewModeArg;           // empty = absent
    for (int i=1;i<argc;++i){
        // Split into chunks to avoid MSVC C1061 nesting limit.
        // --open-level-picker: open modal on first frame.
        if (!std::strcmp(argv[i],"--open-level-picker")) { openLevelPickerArg=true; continue; }
        // Optimisation wave 1 dials.
        if (!std::strcmp(argv[i],"--gi-force-rebuild") && i+1<argc) {
            giForceRebuildArg = std::atoi(argv[++i]); continue;
        }
        if (!std::strcmp(argv[i],"--gi-bounded-dispatch") && i+1<argc) {
            giBoundedDispatchArg = std::atoi(argv[++i]); continue;
        }
        // --gi-free-accumulator: freed after quiet ticks by default; 0 holds allocated.
        if (!std::strcmp(argv[i],"--gi-free-accumulator") && i+1<argc) {
            giFreeAccumulatorArg = std::atoi(argv[++i]); continue;
        }
        // --restir-visibility: U1 setting.
        if (!std::strcmp(argv[i],"--restir-visibility") && i+1<argc) {
            restirVisibilityArg = argv[++i]; continue;
        }
        // --gi-vis-path-view: debug view.
        if (!std::strcmp(argv[i],"--gi-vis-path-view")) { giVisPathViewArg = true; continue; }
        // --blended-gi: pricing switch.
        if (!std::strcmp(argv[i],"--blended-gi") && i+1<argc) { blendedGiArg = argv[++i]; continue; }
        // --view-mode: view type.
        if (!std::strcmp(argv[i],"--view-mode") && i+1<argc) { viewModeArg = argv[++i]; continue; }
        // --mesh-heap: string, not int.
        if (!std::strcmp(argv[i],"--mesh-heap") && i+1<argc) {
            meshHeapArg = argv[++i]; continue;
        }
        // --lod-share-vertices: 0|1.
        if (!std::strcmp(argv[i],"--lod-share-vertices") && i+1<argc) {
            lodShareVerticesArg = std::atoi(argv[++i]); continue;
        }
        // --gi-sky-occlusion-rays: ambient visibility rays per pixel.
        if (!std::strcmp(argv[i],"--gi-sky-occlusion-rays") && i+1<argc) {
            giSkyOccRays = std::atoi(argv[++i]); continue;
        }
        // --gi-sky-occlusion-tile: pixels per shared ray direction.
        if (!std::strcmp(argv[i],"--gi-sky-occlusion-tile") && i+1<argc) {
            giSkyOccTile = std::atoi(argv[++i]); continue;
        }
        // --sky-light: scales sky ambient (1 default).
        if (!std::strcmp(argv[i],"--sky-light") && i+1<argc) {
            skyLightArg = (f32)std::atof(argv[++i]); continue;
        }
        // --gi-intensity: multiplier on cone bounce (1 default).
        if (!std::strcmp(argv[i],"--gi-intensity") && i+1<argc) {
            giIntensityArg = (f32)std::atof(argv[++i]); continue;
        }
        // --tonemap 0|1: per-channel ACES vs matrixed fit.
        if (!std::strcmp(argv[i],"--tonemap") && i+1<argc) { tonemapArg = std::atoi(argv[++i]); continue; }
        // --windowed: default now; kept for backwards compatibility.
        if (!std::strcmp(argv[i],"--windowed")) { windowedArg = true; continue; }
        // --fullscreen: borderless, sized to monitor.
        if (!std::strcmp(argv[i],"--fullscreen")) { fullscreenArg = true; continue; }
        // --max-radiance: ceiling on scene radiance before tonemap.
        if (!std::strcmp(argv[i],"--max-radiance") && i+1<argc) {
            maxRadianceArg = static_cast<f32>(std::atof(argv[++i])); continue;
        }
        // --no-editor-chrome: scene only, no grid/gizmo/overlay.
        if (!std::strcmp(argv[i],"--no-editor-chrome")) { noEditorChrome=true; continue; }
        // --scene-census: print level contents description.
        if (!std::strcmp(argv[i],"--scene-census")) { sceneCensus=true; continue; }
        // --open-level <name>: open level through standard funnel.
        if (!std::strcmp(argv[i],"--open-level") && i+1<argc) { openLevelArg=argv[++i]; continue; }
        // --layered-bsdf: shader variant (0-4 tier).
        if (!std::strcmp(argv[i],"--layered-bsdf") && i+1<argc) { layeredBsdf=std::atoi(argv[++i]); continue; }

        // --coat: placeholder materials with coat; weight/roughness/f0 optional.
        if (!std::strcmp(argv[i],"--coat") && i+1<argc) {
            coatWeight = (f32)std::atof(argv[++i]);
            if (i+1<argc && argv[i+1][0] != '-') coatRough = (f32)std::atof(argv[++i]);
            if (i+1<argc && argv[i+1][0] != '-') coatF0    = (f32)std::atof(argv[++i]);
            continue;
        }

        // --new-project <location> <name>: scaffold and exit.
        bool argMatched = true;
        if (!std::strcmp(argv[i],"--new-project") && i+2<argc) {
            const std::string loc = argv[++i], nm = argv[++i];
            fmt::ProjectDesc made;
            std::string why;
            if (editor::scaffoldProject(loc, nm, made, &why)) {
                AVER_INFO("[Sandbox] scaffolded '{}' at {}", nm, made.dir);
                std::exit(0);
            }
            AVER_ERROR("[Sandbox] could not scaffold '{}': {}", nm, why);
            std::exit(1);
        }
        // --new-project-template: scaffold from template and exit.
        else if (!std::strcmp(argv[i],"--new-project-template") && i+3<argc) {
            const std::string loc = argv[++i], nm = argv[++i], tmplId = argv[++i];
            const std::vector<editor::TemplateInfo> tmpls = editor::listTemplates();
            const editor::TemplateInfo* found = nullptr;
            for (const editor::TemplateInfo& t : tmpls) if (t.id == tmplId) { found = &t; break; }
            if (!found) {
                AVER_ERROR("[Sandbox] no template named '{}' ({} found)", tmplId, tmpls.size());
                std::exit(1);
            }
            fmt::ProjectDesc made;
            std::string why;
            if (editor::scaffoldProjectFromTemplate(loc, nm, *found, made, &why)) {
                AVER_INFO("[Sandbox] scaffolded '{}' from template '{}' at {}", nm, tmplId, made.dir);
                std::exit(0);
            }
            AVER_ERROR("[Sandbox] could not scaffold '{}' from template '{}': {}", nm, tmplId, why);
            std::exit(1);
        }
        // --import-gltf <src> <destDir>: import model and exit.
        else if (!std::strcmp(argv[i],"--import-gltf") && i+2<argc) {
            const std::string src = argv[++i], destDir = argv[++i];
            // Find owning project for material/texture cooking.
            std::string contentDir;
            const std::string ownerProject = ownerProjectOf(destDir);
            if (!ownerProject.empty()) {
                fmt::ProjectDesc proj;
                std::string projWhy;
                if (fmt::loadOcproject(ownerProject, proj, &projWhy)) contentDir = proj.contentDir();
                else AVER_WARN("[Sandbox] found '{}' but could not load it: {} - importing geometry only",
                              ownerProject, projWhy);
            }
            GltfImportSummary sum;
            std::string why;
            if (!importGltfToDir(src, destDir, contentDir, /*overwrite=*/false, sum, &why)) {
                AVER_ERROR("[Sandbox] could not import '{}': {}", src, why);
                std::exit(1);
            }
            if (sum.meshesWritten == 0 && sum.rigsWritten == 0 && sum.clipsWritten == 0) {
                AVER_ERROR("[Sandbox] '{}' produced nothing importable - see the Output Log above", src);
                std::exit(1);
            }
            AVER_INFO("[Sandbox] imported '{}' -> {} ({} mesh(es), {} skeleton(s), {} clip(s))",
                      src, destDir, sum.meshesWritten, sum.rigsWritten, sum.clipsWritten);
            std::exit(0);
        }
        // --upgrade-project: scaffold repair + version migration and exit.
        else if (!std::strcmp(argv[i],"--upgrade-project") && i+1<argc) {
            const std::string manifest = argv[++i];
            fmt::ProjectDesc p;
            std::string why;
            if (!fmt::loadOcproject(manifest, p, &why)) {
                AVER_ERROR("[Sandbox] could not load '{}': {}", manifest, why);
                std::exit(1);
            }
            const editor::ProjectUpgrade up = editor::inspectProject(p);
            for (const editor::ProjectFix& f : up.fixes)
                AVER_INFO("  {} : {}", f.summary, f.detail);
            if (!up.empty() && !editor::applyProjectUpgrade(p, up, &why)) {
                AVER_ERROR("[Sandbox] scaffold repair failed: {}", why);
                std::exit(1);
            }
            // Version migration reported but not stamped on failure.
            if (!editor::migrateProject(manifest, &why)) {
                AVER_ERROR("[Sandbox] version migration failed: {}", why);
                std::exit(1);
            }
            AVER_INFO("[Sandbox] '{}' upgraded and stamped {}", p.name, kEngineVersion);
            std::exit(0);
        }
        // --landscape-gen <path> [sampleCount] [spacingCm]: write synthetic terrain and exit.
        else if (!std::strcmp(argv[i],"--landscape-gen") && i+1<argc) {
            const std::string outPath = argv[++i];
            u32 samples = 257; f32 spacingCm = 400.0f;
            if (i+1 < argc && argv[i+1][0] != '-') samples = static_cast<u32>(std::atoi(argv[++i]));
            if (i+1 < argc && argv[i+1][0] != '-') spacingCm = static_cast<f32>(std::atof(argv[++i]));
#if AVER_MODULE_LANDSCAPE
            fmt::OcLandData d;
            d.sampleCount = samples;
            d.spacingCm = spacingCm;
            const f32 extentCm = static_cast<f32>(samples - 1) * spacingCm;
            // Centred on origin.
            d.originCm[0] = -extentCm * 0.5f; d.originCm[1] = -extentCm * 0.5f; d.originCm[2] = 0.0f;
            d.heights.resize(static_cast<usize>(samples) * samples);
            for (u32 iy = 0; iy < samples; ++iy) {
                for (u32 ix = 0; ix < samples; ++ix) {
                    const f32 fx = static_cast<f32>(ix), fy = static_cast<f32>(iy);
                    // Two overlapping sine fields: relief at any spacing.
                    const f32 h = 1400.0f * std::sin(fx * 0.045f) * std::cos(fy * 0.037f)
                                + 700.0f  * std::sin((fx + fy) * 0.021f);
                    d.heights[static_cast<usize>(iy) * samples + ix] = h;
                }
            }
            std::string why;
            if (fmt::saveOcLand(outPath, d, &why)) {
                AVER_INFO("[Sandbox] wrote {} ({}x{} samples, {:.0f} m across)",
                          outPath, samples, samples, extentCm / 100.0f);
                std::exit(0);
            }
            AVER_ERROR("[Sandbox] could not write '{}': {}", outPath, why);
            std::exit(1);
#else
            (void)samples; (void)spacingCm;
            AVER_ERROR("[Sandbox] --landscape-gen needs AVER_MODULE_LANDSCAPE (this build has it OFF): '{}' not written", outPath);
            std::exit(1);
#endif
        }
        // --sculpt-test <in> <out> [mode] [radiusCm] [strengthCm]: sculpt and exit.
        else if (!std::strcmp(argv[i],"--sculpt-test") && i+2<argc) {
            const std::string sculptIn = argv[++i], sculptOut = argv[++i];
            std::string sculptMode = "raise"; f32 sculptRadiusArg = 0.0f, sculptStrengthArg = 0.0f;
            if (i+1 < argc && argv[i+1][0] != '-') sculptMode = argv[++i];
            if (i+1 < argc && argv[i+1][0] != '-') sculptRadiusArg = static_cast<f32>(std::atof(argv[++i]));
            if (i+1 < argc && argv[i+1][0] != '-') sculptStrengthArg = static_cast<f32>(std::atof(argv[++i]));
#if AVER_MODULE_LANDSCAPE
            fmt::OcLandData sd;
            std::string sculptWhy;
            if (!fmt::loadOcLand(sculptIn, sd, &sculptWhy)) {
                AVER_ERROR("[Sandbox] --sculpt-test could not load '{}': {}", sculptIn, sculptWhy);
                std::exit(1);
            }
            const landscape::BrushMode sculptModeVal =
                sculptMode == "lower"   ? landscape::BrushMode::Lower
              : sculptMode == "smooth"  ? landscape::BrushMode::Smooth
              : sculptMode == "flatten" ? landscape::BrushMode::Flatten
              : sculptMode == "ramp"    ? landscape::BrushMode::Ramp
              : sculptMode == "noise"   ? landscape::BrushMode::Noise
                                        : landscape::BrushMode::Raise;
            landscape::BrushParams sp;
            sp.centerCm[0] = sd.originCm[0] + sd.extentCm() * 0.5f;
            sp.centerCm[1] = sd.originCm[1] + sd.extentCm() * 0.5f;
            sp.radiusCm = sculptRadiusArg > 0.0f ? sculptRadiusArg : sd.extentCm() * 0.2f;
            sp.strength = sculptStrengthArg > 0.0f ? sculptStrengthArg : 300.0f;
            sp.mode = sculptModeVal;
            const u32 cix = (sd.sampleCount - 1) / 2, ciy = cix;
            sp.flattenTargetCm = sd.heightAt(cix, ciy) + 500.0f;   // Flatten only.
            // Ramp only: start point a quarter extent from center.
            sp.rampStartCm[0] = sp.centerCm[0] - sd.extentCm() * 0.25f;
            sp.rampStartCm[1] = sp.centerCm[1];
            sp.rampStartHeightCm = sd.heightAt(cix, ciy);
            // Noise only: fixed literal for determinism.
            sp.noiseSeed = 20260913u;
            const f32 sculptBefore = sd.heightAt(cix, ciy);
            // Eight ticks at 0.25 amount (~130ms held).
            landscape::BrushRect sculptRect{};
            for (int tick = 0; tick < 8; ++tick) sculptRect = landscape::applyBrush(sd, sp, 0.25f);
            if (sculptRect.empty) {
                AVER_ERROR("[Sandbox] --sculpt-test: the brush touched no sample -- radius/centre "
                           "landed entirely off the section");
                std::exit(1);
            }
            const f32 sculptAfter = sd.heightAt(cix, ciy);
            if (!fmt::saveOcLand(sculptOut, sd, &sculptWhy)) {
                AVER_ERROR("[Sandbox] --sculpt-test could not write '{}': {}", sculptOut, sculptWhy);
                std::exit(1);
            }
            fmt::OcLandData sculptBack;
            if (!fmt::loadOcLand(sculptOut, sculptBack, &sculptWhy)) {
                AVER_ERROR("[Sandbox] --sculpt-test could not read back '{}': {}", sculptOut, sculptWhy);
                std::exit(1);
            }
            const f32 sculptSaved = sculptBack.heightAt(cix, ciy);
            AVER_INFO("[Sandbox] --sculpt-test {} on '{}': centre sample {:.2f} -> {:.2f} cm in "
                      "memory, {:.2f} cm after save+reload, touched rect [{},{}]-[{},{}]",
                      sculptMode, sculptIn, sculptBefore, sculptAfter, sculptSaved,
                      sculptRect.x0, sculptRect.y0, sculptRect.x1, sculptRect.y1);
            std::exit(0);
#else
            (void)sculptIn; (void)sculptMode; (void)sculptRadiusArg; (void)sculptStrengthArg;
            AVER_ERROR("[Sandbox] --sculpt-test needs AVER_MODULE_LANDSCAPE (this build has it OFF): "
                       "'{}' not written", sculptOut);
            std::exit(1);
#endif
        }
        // --water <heightCm>: add Gerstner surface and buoyancy plane.
        else if (!std::strcmp(argv[i],"--water") && i+1<argc) {
            waterOn = true;
            waterHeight = static_cast<f32>(std::atof(argv[++i]));
        }
        // --open-asset <path>: open file via standard host funnel.
        else if (!std::strcmp(argv[i],"--open-asset") && i+1<argc) openAsset=argv[++i];
        // --select <substring>: select entity by name.
        else if (!std::strcmp(argv[i],"--select") && i+1<argc) selectEntity=argv[++i];
        // --open-legacy: open old project without upgrading.
        else if (!std::strcmp(argv[i],"--open-legacy")) openLegacy=true;
        // --graph-tab viewport: front the graph viewport tab.
        else if (!std::strcmp(argv[i],"--graph-tab") && i+1<argc) graphTab=argv[++i];
        // --graph-select <nodeId>: select node in open graph.
        else if (!std::strcmp(argv[i],"--graph-select") && i+1<argc) graphSelectNode=argv[++i];
        // --actor-live: start with LIVE enabled.
        else if (!std::strcmp(argv[i],"--actor-live")) editor::setActorEditorLiveByDefault(true);

        else if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--input-probe")) inputProbe=true;
        else if (!std::strcmp(argv[i],"--auto-compile")) autoCompile=true;
        else if (!std::strcmp(argv[i],"--focus-level-at") && i+1<argc) focusLevelAt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--project-settings")) focusVoxi=true;
        // --project-settings-page N: verification-only.
        else if (!std::strcmp(argv[i],"--project-settings-page") && i+1<argc) projectSettingsPage=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--editor-prefs")) showPrefs=true;
        // --scroll-prefs-to-keybinds: see buildEditorPrefs().
        else if (!std::strcmp(argv[i],"--scroll-prefs-to-keybinds")) scrollPrefsToKeybinds=true;
        else if (!std::strcmp(argv[i],"--hud-preview") && i+1<argc) hudTest=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--save-project")) saveProject=true;
        else if (!std::strcmp(argv[i],"--import") && i+2<argc) { importSrc=argv[++i]; importDst=argv[++i]; }
        else if (!std::strcmp(argv[i],"--new-script")) focusScript=true;
        else if (!std::strcmp(argv[i],"--tools-menu")) focusTools=true;
        else if (!std::strcmp(argv[i],"--compile-menu")) focusCompileMenu=true;
        // --mcp [port]: open editor control channel with optional port.
        else if (!std::strcmp(argv[i],"--mcp")) {
            mcpRequested = true;
            if (i+1 < argc && argv[i+1][0] != '-') {
                // Range-checked: strtol avoids silent truncation.
                const char* raw = argv[++i];
                const long  p   = std::strtol(raw, nullptr, 10);
                if (p >= 1 && p <= 65535) {
                    mcpPort = (u16)p;
                    mcpPortExplicit = true;
                } else {
                    // Not fatal: channel is a debugging aid, not a requirement.
                    AVER_WARN("[Mcp] --mcp {} is not a port (1-65535); falling back to mcp.conf or "
                              "the built-in default", raw);
                }
            }
        }
        else if (!std::strcmp(argv[i],"--compile-scripts")) focusCompile=true;
        // --reload-scripts [N]: reload once, N frames in (default 20).
        else if (!std::strcmp(argv[i],"--reload-scripts")) {
            reloadAt = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 20;
        }
        else if (!std::strcmp(argv[i],"--start-screen")) startScreen=true;
        else argMatched = false;
        if (!argMatched) { argMatched = true;   // flag chunk 2
        // --chunk-stream [N]: stream N frames in (default 5).
        // --drone-graph <path>: graph the drone runs.
        if (!std::strcmp(argv[i],"--drone-graph") && i+1<argc) droneGraph = argv[++i];
        // --landscape <path>: override terrain.
        else if (!std::strcmp(argv[i],"--landscape") && i+1<argc) landscapePath = argv[++i];
        else if (!std::strcmp(argv[i],"--chunk-stream")) {
            chunkStream = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --no-chunk-stream: explicitly off.
        else if (!std::strcmp(argv[i],"--no-chunk-stream")) chunkStream = -1;
        // --fog-match [opacity]: tie fog to streaming radius.
        else if (!std::strcmp(argv[i],"--fog-match")) {
            fogMatch = true;
            if (i+1 < argc && argv[i+1][0] != '-') fogMatchOpacity = static_cast<f32>(std::atof(argv[++i]));
        }
        // --drone [N]: enable drone N frames in (default 5).
        else if (!std::strcmp(argv[i],"--drone")) {
            droneAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --undo-test [N]: run undo test N frames in (default 10), then exit.
        else if (!std::strcmp(argv[i],"--undo-test")) {
            undoTestAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 10;
        }
        // --keybind-test write|read [N]: test persistence N frames in (default 10), then exit.
        else if (!std::strcmp(argv[i],"--keybind-test") && i+1<argc) {
            keybindTestMode = argv[++i];
            keybindTestAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 10;
        }
        // --drawer log|content[:<sub>]|console[:<seed>]: open drawer with optional subfolder.
        else if (!std::strcmp(argv[i],"--drawer") && i+1<argc) {
            const char* v = argv[++i];
            const char* colon = std::strchr(v, ':');
            const std::string kind = colon ? std::string(v, static_cast<size_t>(colon - v)) : std::string(v);
            drawerOpen = kind == "log" ? 2 : kind == "console" ? 3 : 1;
            if (colon) drawerSub = colon + 1;
        }
        else if (!std::strcmp(argv[i],"--msaa") && i+1<argc) msaa=std::atoi(argv[++i]);
        // --gi/--rt/--pt [tier]: quality 0-4 (Off..Epic); bare = High. -1 = absent.
        else if (!std::strcmp(argv[i],"--gi") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            gi=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--gi")) gi=3;
        else if (!std::strcmp(argv[i],"--no-gi")) noGi=true;
        else if (!std::strcmp(argv[i],"--gi-debug")) { gi=3; giDbg=true; }
        // --no-gi-cone: disable cone trace read only.
        else if (!std::strcmp(argv[i],"--no-gi-cone")) giConeOff=true;
        else if (!std::strcmp(argv[i],"--rt") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            rt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--rt")) rt=3;
        // --pt [tier]: path tracing quality. 0 turns reference view OFF.
        else if (!std::strcmp(argv[i],"--pt") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            pt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt")) pt=3;
        else if (!std::strcmp(argv[i],"--no-rt")) noRt=true;
        // --cam-wobble DEG PERIOD: oscillating motion for measurement.
        else if (!std::strcmp(argv[i],"--cam-wobble") && i+2<argc) {
            camWobbleDeg=(f32)std::atof(argv[++i]); camWobblePeriod=std::atoi(argv[++i]);
        }
        // --cam-wander AMP SPEED: non-repeating drift for training.
        else if (!std::strcmp(argv[i],"--cam-wander") && i+2<argc) {
            camWanderAmp=(f32)std::atof(argv[++i]); camWanderSpeed=(f32)std::atof(argv[++i]);
        }
        // --rt-rays: sun occlusion rays per pixel.
        else if (!std::strcmp(argv[i],"--rt-rays") && i+1<argc) rtRays=std::atoi(argv[++i]);
        // --rt-pixels-per-ray: temporal amortisation tile edge.
        else if (!std::strcmp(argv[i],"--rt-pixels-per-ray") && i+1<argc) rtPixelsPerRay=std::atoi(argv[++i]);
        // --rt-shadow-denoise: spatial filter radius (0 = off).
        else if (!std::strcmp(argv[i],"--rt-shadow-denoise") && i+1<argc) rtShadowDenoise=std::atoi(argv[++i]);
        // --rt-render-mode 0|1: raster vs primary rays for first surface.
        else if (!std::strcmp(argv[i],"--rt-render-mode") && i+1<argc) rtRenderMode=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt-bounces") && i+1<argc) ptBounces=std::atoi(argv[++i]);
        // --gi-update-interval: frames between GI volume revoxelisation.
        else if (!std::strcmp(argv[i],"--gi-update-interval") && i+1<argc) giUpdateInterval=std::atoi(argv[++i]);
        // --render-scale: scene resolution as fraction of swapchain.
        else if (!std::strcmp(argv[i],"--render-scale") && i+1<argc) renderScale=static_cast<f32>(std::atof(argv[++i]));
        // --aversr LEVEL: off|quality|balanced|performance (case-insensitive).
        else if (!std::strcmp(argv[i],"--aversr") && i+1<argc) aversrArg=argv[++i];
        // --frame-interp: 0 off, 1 on, 2 on + generated image capture.
        else if (!std::strcmp(argv[i],"--frame-interp") && i+1<argc) { const int v=std::atoi(argv[++i]); frameInterpArg = v < 0 ? 0 : (v > 2 ? 2 : v); }
        // --frame-interp-trajectory: path the gather follows.
        // --frame-interp-train: train trajectory network in-engine.
        else if (!std::strcmp(argv[i],"--frame-interp-trajectory") && i+1<argc) {
            const char* t = argv[++i];
            frameInterpTrajectory = !std::strcmp(t,"quadratic") ? 1 : !std::strcmp(t,"neural") ? 2 : 0;
        }
        else if (!std::strcmp(argv[i],"--frame-interp-train")) frameInterpTrain=true;
        // Neural Visualiser: --neurafi-view 0-4, --neurafi-generated-only.
        // --neurac-view 0-4, --neurac-grid.
        else if (!std::strcmp(argv[i],"--neurafi-view") && i+1<argc) neurafiVizArg = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--neurafi-generated-only")) neurafiGeneratedOnly = true;
        else if (!std::strcmp(argv[i],"--neurac-view") && i+1<argc) neuracViewArg = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--neurac-grid")) neuracGrid = true;
        // --depth-prepass: same-frame depth-only pass ahead of opaque walk.
        else if (!std::strcmp(argv[i],"--depth-prepass")) depthPrepass=true;
        else argMatched = false;
        }   // end of flag chunk 2
        if (!argMatched) { argMatched = true;   // flag chunk 3
        // --gbuffer: forward-pass G-buffer (velocity, viewz, normals).
        if (!std::strcmp(argv[i],"--gbuffer")) gbuffer=true;
        // --gbuffer-debug MODE: force G-buffer, select debug channel.
        else if (!std::strcmp(argv[i],"--gbuffer-debug") && i+1<argc) { gbuffer=true; gbufferDebug=argv[++i]; }
        // --occlusion-cull: hierarchical-Z two-pass box culling.
        else if (!std::strcmp(argv[i],"--occlusion-cull")) occlusionCull=true;
        // --edge-aa: FXAA upscaler.
        else if (!std::strcmp(argv[i],"--edge-aa")) edgeAa=true;
        else if (!std::strcmp(argv[i],"--frame-time")) frameTime=true;
        else if (!std::strcmp(argv[i],"--ms")) ms=true;
        else if (!std::strcmp(argv[i],"--probe") && i+2<argc) { probeX=(u32)std::atoi(argv[++i]); probeY=(u32)std::atoi(argv[++i]); }
        // --force-caps: clamp device-reported capabilities.
        else if (!std::strcmp(argv[i],"--force-caps") && i+1<argc) forceCaps=argv[++i];
        else if (!std::strcmp(argv[i],"--warp")) warp=true;
        else if (!std::strcmp(argv[i],"--backend") && i+1<argc) backendName=argv[++i];
        else if (!std::strcmp(argv[i],"--debug-layer")) debugLayer=true;
        // --dred: D3D12 Device Removed Extended Data.
        else if (!std::strcmp(argv[i],"--dred")) dred=true;
        else if (!std::strcmp(argv[i],"--scripts") && i+1<argc) scriptsDir=argv[++i];
        // --shader-source <dir>: read HLSL from this tree, reload on change.
        else if (!std::strcmp(argv[i],"--shader-source") && i+1<argc) {
            shaderSourceDir = argv[++i];
            aver::rhi::setShaderSourceDir(shaderSourceDir);
        }
        else if (!std::strcmp(argv[i],"--spawn-test") && i+1<argc) spawnTest=argv[++i];
        else if (!std::strcmp(argv[i],"--play-test")) playTest=true;
        else if (!std::strcmp(argv[i],"--play-walk")) playWalk=true;
        else if (!std::strcmp(argv[i],"--skin-test")) skinTest=true;
        else if (!std::strcmp(argv[i],"--skin-draw-test")) skinDrawTest=true;
        else if (!std::strcmp(argv[i],"--particle-test")) particleTest=true;
        // --no-particle-gi: exclude particles from GI.
        else if (!std::strcmp(argv[i],"--no-particle-gi")) noParticleGi=true;
        // --particle-stress <N> <M>: verification-only.
        else if (!std::strcmp(argv[i],"--particle-stress") && i+2<argc) {
            particleStressEmitters=std::atoi(argv[++i]);
            particleStressMaxParticles=std::atoi(argv[++i]);
        }
        else if (!std::strcmp(argv[i],"--particle-stress2")) particleStressSecondEmitter=true;
        else if (!std::strcmp(argv[i],"--refl-test")) reflTest=true;
        else if (!std::strcmp(argv[i],"--furnace-test")) furnaceTest=true;
        else if (!std::strcmp(argv[i],"--furnace-sun")) furnaceSun=true;
        else if (!std::strcmp(argv[i],"--furnace-grid")) furnaceGrid=true;
        else if (!std::strcmp(argv[i],"--furnace-tilt") && i+1<argc) furnaceTilt=(f32)std::atof(argv[++i]);
        else if (!std::strcmp(argv[i],"--sun-angle") && i+1<argc) sunAngle=(f32)std::atof(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt-furnace")) ptFurnace=true;
        else if (!std::strcmp(argv[i],"--pt-scene")) ptScene=true;
        // --pt-scene-toggle-on/-off [N]: verification-only.
        // --device-lost-at <N>: simulate GPU loss after N frames.
        else if (!std::strcmp(argv[i],"--device-lost-at") && i+1<argc) deviceLostAt=std::atoi(argv[++i]);
        else argMatched = false;
        }   // end of flag chunk 3
        if (!argMatched) { argMatched = true;   // flag chunk 4
        // --crash-test <kind>: deliberately crash (av|assert|fatal|critical).
        if (!std::strcmp(argv[i],"--crash-test") && i+1<argc) crashTest=argv[++i];
        // --pt-quality-ramp [N]: raise PT rung every N frames.
        else if (!std::strcmp(argv[i],"--pt-quality-ramp")) {
            ptQualityRamp = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 20;
        }
        else if (!std::strcmp(argv[i],"--pt-scene-toggle-on")) {
            ptSceneToggleOn = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        else if (!std::strcmp(argv[i],"--pt-scene-toggle-off")) {
            ptSceneToggleOff = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 15;
        }
        // --aversr-cycle [N]: toggle AverSR at frame N.
        else if (!std::strcmp(argv[i],"--aversr-cycle")) {
            aversrCycle = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 30;
        }
        else if (!std::strcmp(argv[i],"--skin-scene-test") && i+1<argc) skinSceneDir=argv[++i];
        // --bake-nav [cellCm]: bake navigation on startup.
        else if (!std::strcmp(argv[i],"--bake-nav")) {
            bakeNav = true;
            if (i+1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                bakeNavCell = static_cast<f32>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) frames=std::strtoull(argv[++i],nullptr,10);
        else if (!std::strcmp(argv[i],"--screenshot") && i+1<argc) shot=argv[++i];
        else if (!std::strcmp(argv[i],"--bloom") && i+1<argc) { bloom=static_cast<f32>(std::atof(argv[++i])); bloomSet=true; }
        else if (!std::strcmp(argv[i],"--exposure") && i+1<argc) { exposure=static_cast<f32>(std::atof(argv[++i])); exposureSet=true; }
        else if (!std::strcmp(argv[i],"--auto-exposure")) autoExposure=true;
        else if (!std::strcmp(argv[i],"--no-vsync")) vsyncOff=true;
        else if (!std::strcmp(argv[i],"--vsync")) vsyncOn=true;
        // --lod-select [px]: virtualized geometry per-instance LOD (default 1.0px error).
        else if (!std::strcmp(argv[i],"--lod-select")) {
            lodSelect=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --no-lod-select: every instance at LOD 0.
        else if (!std::strcmp(argv[i],"--no-lod-select")) lodSelect=false;
        // --lod-cluster-stats: per-meshlet frustum/cone-cull counters.
        else if (!std::strcmp(argv[i],"--lod-cluster-stats")) lodClusterStats=true;
        // --lod-per-cluster [px]: per-cluster virtualized geometry selection.
        else if (!std::strcmp(argv[i],"--lod-per-cluster")) {
            lodPerCluster=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --lod-mesh-shader [px]: GPU per-cluster path (amplification+mesh shader).
        else if (!std::strcmp(argv[i],"--lod-mesh-shader")) {
            lodMeshShader=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --no-lod-mesh-shader: force GPU per-cluster path OFF.
        else if (!std::strcmp(argv[i],"--no-lod-mesh-shader")) lodMeshShader=0;
        else if (!std::strcmp(argv[i],"--ui-demo")) uiDemo=true;
        // --clouds: optional coverage value.
        else if (!std::strcmp(argv[i],"--clouds")) {
            clouds=1;
            if (i+1 < argc && argv[i+1][0] != '-') cloudCover=static_cast<f32>(std::atof(argv[++i]));
        }
        // --sky-physical: derived sky with optional sun elevation.
        else if (!std::strcmp(argv[i],"--sky-physical")) {
            skyPhysical=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                skyElevation=static_cast<f32>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i],"--sky-authored")) skyAuthored=true;
        else if (!std::strcmp(argv[i],"--probe-rel") && i+2<argc) {
            probeU=static_cast<f32>(std::atof(argv[++i]));
            probeV=static_cast<f32>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i],"--cam") && i+5<argc) {
            camSet=true;
            camX    =static_cast<f32>(std::atof(argv[++i]));
            camY    =static_cast<f32>(std::atof(argv[++i]));
            camZ    =static_cast<f32>(std::atof(argv[++i]));
            camPitch=static_cast<f32>(std::atof(argv[++i]));
            camYaw  =static_cast<f32>(std::atof(argv[++i]));
        }
        // --mode <select|landscape|foliage|simulate>: editing mode (automation needs UI-less start).
        else if (!std::strcmp(argv[i],"--mode") && i+1<argc) startMode=argv[++i];
        else if (!std::strcmp(argv[i],"--tool") && i+1<argc) {
            const char* t=argv[++i];
            tool = !std::strcmp(t,"move")?Tool::Move : !std::strcmp(t,"rotate")?Tool::Rotate :
                   !std::strcmp(t,"scale")?Tool::Scale :
#if AVER_MODULE_LANDSCAPE

#endif
                   Tool::Select;
        }
        // Bare paths resolve by extension; all registered types are handled here.
        else if (argv[i][0]!='-') {
            if      (isOcproject(argv[i])) project = argv[i];
            else if (isLevelFile(argv[i])) openMap = argv[i];
            else                           beam    = argv[i];
        }
        }   // end of flag chunk 4
    }

    // Single-instance forwarding: gate before creating the engine.
    // Narrowest gate: argc==2 and no flags (excludes multi-process gate-sweep/import/scaffold).
    if (argc == 2 && argv[1][0] != '-' && (!project.empty() || !openMap.empty())) {
        // Resolve to absolute: receiver's working directory may differ.
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(argv[1], ec);
        const std::string forwardPath = ec ? std::string(argv[1]) : abs.string();
        if (Window::forwardToSingleInstancePrimary(forwardPath)) {
            AVER_INFO("[Sandbox] '{}' handed to the running editor instance; this process exits",
                      forwardPath);
            std::exit(0);
        }
        // Failure (no primary, declined, timeout) means launch our own Application/Engine.
    }

    // Before the engine creates a device: the backend queries the hardware inside Engine::run.
    if (forceCaps && !rhi::setCapsOverride(forceCaps))
        AVER_ERROR("[Sandbox] --force-caps '{}' was rejected; running on the UNCLAMPED device", forceCaps);

    // A level's project is inferred from its path (ownerProjectOf); explicit --project still wins.
    if (!openMap.empty() && project.empty()) {
        const std::string owner = ownerProjectOf(openMap);
        if (!owner.empty()) project = owner;
        else AVER_WARN("[Sandbox] '{}' is not inside a project (no .ocproject above it); its "
                       "placements will not resolve", openMap);
    }

    // --crash-test: exercises the handler before window/device/project exist.
    if (!crashTest.empty()) {
        if (crashTest == "critical") {
            // Logs Critical (waking crash reporter), exits cleanly; verifies the quiet path.
            AVER_CRITICAL("[Sandbox] --crash-test critical: a deliberate critical, not a real fault. "
                          "The crash reporter is now watching this process and should say nothing "
                          "when it exits cleanly.");
        } else {
            // Other kinds end the process; reporter must not show a window in automated runs.
            crash::setLaunchReporter(false);
            AVER_WARN("[Sandbox] --crash-test {}: about to end this process deliberately", crashTest);
            if (crashTest == "assert") {
                AVER_ASSERTM(false, "deliberate --crash-test assert");
            } else if (crashTest == "fatal") {
                AVER_FATAL("[Sandbox] deliberate --crash-test fatal");
            } else if (crashTest == "av") {
                // Volatile so the null write cannot be optimised away.
                volatile int* p = nullptr;
                *p = 1;
            } else if (crashTest == "oom") {
                // Real allocation failure via new-handler, not thrown bad_alloc.
                volatile void* p = ::operator new(static_cast<size_t>(-1) / 2);
                (void)p;
            } else if (crashTest == "throw") {
                // Escaping C++ exception raises SEH 0xE06D7363, taken by unhandled filter first.
                throw std::runtime_error("deliberate --crash-test throw");
            } else {
                AVER_ERROR("[Sandbox] --crash-test: unknown kind '{}'. "
                           "Use critical, assert, fatal, av, oom or throw.",
                           crashTest);
                return nullptr;
            }
        }
    }

    auto* app = new SandboxApp(frames, headless, beam, shot, tool);
    // Single-instance forwarding, receiver side: superset of sender gate, allows bare .ocbeam to be primary.
    // Reading only argc/argv[1][0] keeps the two gates in lockstep.
    app->setSingleInstanceEligible(argc == 1 || (argc == 2 && argv[1][0] != '-'));
    if (!openMap.empty()) app->setOpenMap(openMap);
    app->setPost(exposure, exposureSet, bloom, bloomSet, autoExposure);
    app->applyCaptureExposureRule(autoExposure);
    app->setGiForceOff(noGi);
    app->setGiConeTraceOff(giConeOff);
    if (clouds) app->setClouds(cloudCover);
    if (skyPhysical) app->setSkyPhysical(skyElevation);
    if (skyAuthored) app->setSkyAuthored();
    app->setVSyncOff(vsyncOff);
    app->setVSyncOn(vsyncOn);
    app->setLodSelect(lodSelect, lodErrorPx);
    app->setLodClusterStats(lodClusterStats);
    app->setLodPerCluster(lodPerCluster, lodErrorPx);
    if (lodMeshShader >= 0) app->setLodMeshShader(lodMeshShader != 0, lodErrorPx);
    app->setUiDemo(uiDemo);
    app->setOpenAsset(openAsset);
    app->setSelectEntity(selectEntity);
    app->setWater(waterOn, waterHeight);
    app->setOpenLegacy(openLegacy);
    app->setGraphTab(graphTab);
    app->setGraphSelectNode(graphSelectNode);
    app->setInputProbe(inputProbe);
    app->setAutoCompile(autoCompile);
    app->setFocusLevelAt(focusLevelAt);
    app->setShowEditorPrefs(showPrefs);
    app->setScrollPrefsToKeybinds(scrollPrefsToKeybinds);
    app->setHudTest(hudTest);
    app->setSaveProject(saveProject);
    app->setImportOnce(importSrc, importDst);
    app->setUseWarp(warp);
    // Read the backend from the project before creating the device (command line wins, logged when they differ).
    std::string wantBackend = backendName;
    if (wantBackend.empty() && !project.empty()) {
        fmt::ProjectDesc peek;
        std::string perr;
        if (fmt::loadOcproject(project, peek, &perr) && !peek.backend.empty()) {
            wantBackend = peek.backend;
            AVER_INFO("[Sandbox] RENDER.BACKEND '{}' from the project manifest", wantBackend);
        }
    } else if (!backendName.empty() && !project.empty()) {
        fmt::ProjectDesc peek;
        std::string perr;
        if (fmt::loadOcproject(project, peek, &perr) && !peek.backend.empty() &&
            peek.backend != backendName)
            AVER_INFO("[Sandbox] --backend: the command line asked for {} and the project manifest "
                      "for {}; the command line wins", backendName, peek.backend);
    }
    if (!wantBackend.empty()) app->setBackend(wantBackend);
    app->setDebugLayer(debugLayer);
    app->setUnlitMode(unlitArg);
    if (!viewModeArg.empty()) app->setViewMode(viewModeArg);
    app->setSaveLevelTo(saveLevelArg);
    if (rayProbeArg) app->setRayProbe(rayProbeXArg, rayProbeYArg);
    app->setProjectPath(project);
    // Show start screen: interactive with no project, or --start-screen; never in capture runs.
    app->armBrowser(startScreen || (!headless && frames == 0 && project.empty()));
    app->setFocusVoxi(focusVoxi);
    if (projectSettingsPage >= 0) app->setProjectSettingsPage(projectSettingsPage);
    app->setDrawerOpen(drawerOpen, drawerSub);
    app->setFocusScript(focusScript);
    app->setFocusTools(focusTools);
    app->setOpenLevelPicker(openLevelPickerArg);
    app->setNoEditorChrome(noEditorChrome);
    app->setSceneCensus(sceneCensus);
    app->setOpenLevelByName(openLevelArg);
    app->setFocusCompileMenu(focusCompileMenu);
    if (!droneGraph.empty()) app->setDroneGraph(droneGraph);
    if (!landscapePath.empty()) app->setLandscapePath(landscapePath);
    // Chunk stream: >0 is delay, <0 disables, 0 (flag never given) leaves the member's default.
    if (chunkStream > 0)      app->setChunkStreamAuto(chunkStream);
    else if (chunkStream < 0) app->setChunkStreamAuto(0);
    if (fogMatch) app->setFogMatchToStreamRadius(true, fogMatchOpacity);
    if (droneAuto > 0) app->setDroneAuto(droneAuto);
    if (undoTestAuto > 0) app->setUndoTestAuto(undoTestAuto);
    if (keybindTestAuto > 0) app->setKeybindTestAuto(keybindTestMode, keybindTestAuto);
#if AVER_MODULE_MCP
    // Precedence: explicit --mcp > mcp.conf's editor_bridge.port > default (45123).
    if (mcpRequested) {
        const char* source = "command line";
        if (!mcpPortExplicit) {
            constexpr u16 kDefaultMcpPort = 45123;
            u16 confPort = 0;
            if (editor::readMcpConfPort(editor::engineRoot(), "editor_bridge.port", &confPort)) {
                mcpPort = confPort;
                source = "mcp.conf";
            } else {
                mcpPort = kDefaultMcpPort;
                source = "built-in default";
            }
        }
        AVER_INFO("[Mcp] control channel requested on port {} (source: {})", mcpPort, source);
    }
    app->setMcpPort(mcpPort);
#else
    if (mcpRequested) AVER_WARN("[Mcp] --mcp was given but this build has no control channel "
                                "(-DAVER_MODULE_MCP=ON to include it); the editor runs regardless");
#endif
    app->setFocusCompile(focusCompile);
    app->setFocusReload(reloadAt);
    app->setMsaaOverride(msaa);
    app->setGiOverride(gi, giDbg);
    app->setRtOverride(rt);
    app->setRtRays(rtRays);
    app->setGiSkyOcclusionRays(giSkyOccRays);
    app->setGiSkyOcclusionTile(giSkyOccTile);
    app->setSkyLight(skyLightArg);
    app->setGiIntensity(giIntensityArg);
    app->setWindowed(windowedArg);
    app->setFullscreen(fullscreenArg);
    app->setTonemap(tonemapArg);
    app->setMaxRadiance(maxRadianceArg);
    app->setRtPixelsPerRay(rtPixelsPerRay);
    app->setRtShadowDenoise(rtShadowDenoise);
    app->setRtRenderMode(rtRenderMode);
    app->setRdStages(rdStagesArg);
    app->setPtBounces(ptBounces);
    if (layeredBsdf >= 0) app->setLayeredBsdf(layeredBsdf);
    app->setPtOverride(pt);
    app->setGiUpdateInterval(giUpdateInterval);
    app->setGiMode(giModeArg);
    // --restir-visibility: unrecognised names get a clear error, not a silent 0.
    if (!restirVisibilityArg.empty()) {
        std::string m = restirVisibilityArg;
        for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (m == "none")               app->setRestirVisibility(0);
        else if (m == "reconstructed") app->setRestirVisibility(1);
        else if (m == "half")          app->setRestirVisibility(2);
        else if (m == "full")          app->setRestirVisibility(3);
        else if (m == "cached")        app->setRestirVisibility(4);
        else AVER_ERROR("[Sandbox] --restir-visibility '{}' not recognised "
                        "(none|reconstructed|half|full|cached)", restirVisibilityArg);
    }
    app->setDenoiser(denoiserArg);
    app->setRtForceOff(noRt);
    app->setRayDrivenAblation(rdAblate);
    app->setRtDenoiseMotionTaper(rtDenoiseMotionArg);
    // --lighting-legacy / --pt-legacy-env seed console slots; per-frame reasserts make them live.
#if AVER_MODULE_VOXI
    editor::consoleLightingLegacySlot() = static_cast<u32>(lightingLegacyArg);
#endif
    editor::consolePtLegacyEnvSlot() = ptLegacyEnvArg;
    // Engine-optimisation-plan wave 1 (C-6): seeded the same way; guarded on AVER_MODULE_VOXI.
#if AVER_MODULE_VOXI
    if (giForceRebuildArg >= 0) {
        editor::consoleGiForceRebuildSlot() = giForceRebuildArg != 0;
        AVER_INFO("[Voxi] --gi-force-rebuild {}", giForceRebuildArg != 0 ? "1" : "0");
    }
    if (giBoundedDispatchArg >= 0) {
        editor::consoleGiBoundedDispatchSlot() = giBoundedDispatchArg != 0;
        AVER_INFO("[Voxi] --gi-bounded-dispatch {}", giBoundedDispatchArg != 0 ? "1" : "0");
    }
    if (giFreeAccumulatorArg >= 0) {
        editor::consoleGiFreeAccumulatorSlot() = giFreeAccumulatorArg != 0;
        AVER_INFO("[Voxi] --gi-free-accumulator {}", giFreeAccumulatorArg != 0 ? "1" : "0");
    }
    // Optimisation-wave-2, U1/section 4(a): seeded similarly.
    if (giVisPathViewArg) {
        editor::consoleGiVisPathViewSlot() = true;
        AVER_INFO("[Voxi] --gi-vis-path-view on");
    }
    if (!blendedGiArg.empty()) {
        std::string m = blendedGiArg;
        for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (m == "restir") {
            editor::consoleBlendedGiConeSlot() = false;
            AVER_INFO("[Voxi] --blended-gi restir");
        } else if (m == "cone") {
            editor::consoleBlendedGiConeSlot() = true;
            AVER_INFO("[Voxi] --blended-gi cone");
        } else {
            AVER_ERROR("[Voxi] --blended-gi '{}' not recognised (restir|cone)", blendedGiArg);
        }
    }
#endif
    // --mesh-heap: routed through app member; read at loadProjectMeshes where device exists.
    if (!meshHeapArg.empty()) {
        if (meshHeapArg == "default") {
            app->setMeshHeapDefault(true);
            AVER_INFO("[Sandbox] --mesh-heap default: new static meshes go on the Default heap");
        } else if (meshHeapArg == "upload") {
            app->setMeshHeapDefault(false);
            AVER_INFO("[Sandbox] --mesh-heap upload: new static meshes stay on the Upload heap (today's behaviour)");
        } else {
            AVER_ERROR("[Sandbox] --mesh-heap '{}' not recognised (default|upload)", meshHeapArg);
        }
    }
    // --lod-share-vertices: staged on app, consumed at loadProjectMeshes.
    if (lodShareVerticesArg >= 0) {
        app->setLodShareVertices(lodShareVerticesArg != 0);
        AVER_INFO("[Sandbox] --lod-share-vertices {}", lodShareVerticesArg != 0 ? "1" : "0");
    }
    app->setRefractionOverrides(refraction, refractionStrength, refractionFade);
    app->setCamWobble(camWobbleDeg, camWobblePeriod);
    if (camWobbleStopArg > 0) app->setCamWobbleStop(camWobbleStopArg);
    if (!consoleSetArgs.empty()) app->setConsoleSets(std::move(consoleSetArgs));
    if (camTranslateArg != 0.0f) app->setCamTranslate(camTranslateArg);
    if (camWanderAmp > 0.0f) app->setCamWander(camWanderAmp, camWanderSpeed);
    app->setRenderScale(renderScale);
    if (!aversrArg.empty()) {
#if AVER_MODULE_SR
        // "auto" is explicit CLI Auto, distinct from never passing --aversr.
        std::string aversrLower = aversrArg;
        for (char& c : aversrLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (aversrLower == "auto") {
            app->setAverSrCliAuto();
        } else {
            aver::sr::Quality aversrQuality;
            if (aver::sr::parseQuality(aversrArg.c_str(), aversrQuality)) app->setAverSrQuality(aversrQuality);
            else AVER_ERROR("[AverSR] --aversr '{}' not recognised (off|quality|balanced|performance|auto)",
                            aversrArg);
        }
#else
        AVER_WARN("[AverSR] --aversr '{}' was given but this build has no AverSR module "
                  "(-DAVER_MODULE_SR=ON to include it); the editor renders at native resolution "
                  "regardless", aversrArg);
#endif
    }
    app->setDepthPrepassOverride(depthPrepass);
    app->setGBufferOverride(gbuffer);
    // Called unconditionally; with no --no-walk-cache, the cache-on default is the current behaviour.
    setNoWalkCacheArg(noWalkCacheArg);
    // --gbuffer-debug MODE: unrecognised modes get a clear error.
    if (!gbufferDebug.empty()) {
        std::string m = gbufferDebug;
        for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (m == "velocity")      app->setGBufferDebugView(GBufferDebugFeature::Mode::Velocity);
        else if (m == "viewz")    app->setGBufferDebugView(GBufferDebugFeature::Mode::ViewZ);
        else if (m == "normals")  app->setGBufferDebugView(GBufferDebugFeature::Mode::NormalRoughness);
        else AVER_ERROR("[Sandbox] --gbuffer-debug '{}' not recognised (velocity|viewz|normals)",
                        gbufferDebug);
    }
    if (noOcclusionCullArg) {
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        app->setOcclusionCullForceOff();
#else
        AVER_WARN("[Occlusion] --no-occlusion-cull was given but this build has no Occlusion module "
                  "(-DAVER_MODULE_OCCLUSION=ON to include it); there was nothing to turn off");
#endif
    }
    if (occlusionWaitIdleArg) {
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        app->setOcclusionDebugForceWaitIdle(true);
#else
        AVER_WARN("[Occlusion] --occlusion-waitidle was given but this build has no Occlusion module "
                  "(-DAVER_MODULE_OCCLUSION=ON to include it); there was nothing to force");
#endif
    }
    // Checked after waitidle so the opt-out flag wins if both are somehow passed.
    if (occlusionNoWaitIdleArg) {
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        app->setOcclusionDebugForceWaitIdle(false);
#else
        AVER_WARN("[Occlusion] --no-occlusion-waitidle was given but this build has no Occlusion "
                  "module (-DAVER_MODULE_OCCLUSION=ON to include it); there was nothing to release");
#endif
    }
    if (occlusionCull) {
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        app->setOcclusionCullOverride(true);
#else
        AVER_WARN("[Occlusion] --occlusion-cull was given but this build has no Occlusion module "
                  "(-DAVER_MODULE_OCCLUSION=ON to include it); every entity draws as it always did");
#endif
    }
    if (edgeAa) {
#if AVER_MODULE_SR
        app->setEdgeAaOverride(true);
#else
        AVER_WARN("[AverSR] --edge-aa was given but this build has no AverSR module "
                  "(-DAVER_MODULE_SR=ON to include it); MSAA (if any) is the only edge AA applied");
#endif
    }
    app->setFrameTimeReport(frameTime);
    app->setMsOverride(ms);
    app->setProbe(probeX, probeY);
    if (probeU >= 0.0f) app->setProbeRel(probeU, probeV);
    if (!startMode.empty()) app->setStartMode(startMode);
    if (camSet) app->setCamera(Vec3{camX, camY, camZ}, camPitch, camYaw);
    app->setScriptsDir(scriptsDir);
    app->setSpawnTest(spawnTest);
    if (playTest) app->setPlayTest();
    if (playWalk) app->setPlayWalk();
    if (skinTest) app->setSkinTest();
    if (skinDrawTest) app->setSkinDrawTest();
    if (particleTest) app->setParticleTest();
    if (noParticleGi) app->setNoParticleGi();
    if (particleStressEmitters > 0) app->setParticleStress(particleStressEmitters, particleStressMaxParticles);
    if (particleStressSecondEmitter) app->setParticleStressSecondEmitter();
    if (reflTest) app->setReflTest();
    if (furnaceTest) app->setFurnaceTest();
    if (furnaceSun) app->setFurnaceSun();
    if (furnaceGrid) app->setFurnaceGrid();
    if (coatWeight > 0.0f) app->setCoat(coatWeight, coatRough, coatF0);
    if (furnaceTilt != 0.0f) app->setFurnaceTilt(furnaceTilt);
    if (sunAngle > 0.0f) app->setSunAngle(sunAngle);
    if (ptFurnace) app->setPtFurnaceTest();
    if (ptScene) app->setPtSceneView();
    if (deviceLostAt > 0)     aver::rhi::setSimulatedDeviceLoss(static_cast<u32>(deviceLostAt));
    if (dred)                 aver::rhi::setDredEnabled(true);
    if (ptQualityRamp > 0)    app->setPtQualityRamp(ptQualityRamp);
    if (ptSceneToggleOn > 0)  app->setPtSceneToggleOnAuto(ptSceneToggleOn);
    if (ptSceneToggleOff > 0) app->setPtSceneToggleOffAuto(ptSceneToggleOff);
    if (sunSetAtArg > 0)         app->setSunSetAt(sunSetAtArg, sunSetElevArg, sunSetAzimArg);
    if (sunSweepAtArg > 0)       app->setSunSweep(sunSweepAtArg, sunSweepDegArg, sunSweepTurnsArg);
    if (giHistoryResetAtArg > 0) app->setGiHistoryResetAt(giHistoryResetAtArg);
#if AVER_MODULE_SR
    if (aversrCycle > 0) app->setAverSrCycleAuto(aversrCycle);
    if (resizeCycleArg > 0) app->setResizeCycle(resizeCycleArg);
    if (frameInterpArg >= 0) app->setFrameInterpCli(frameInterpArg);
    app->setFrameInterpTrajectory(frameInterpTrajectory, frameInterpTrain);
    app->setNeuralVisualiserCli(neurafiVizArg, neurafiGeneratedOnly, neuracViewArg, neuracGrid);
    if (gpuTimingArg) app->setGpuTiming(true);
    if (pieCamArg > 0) app->setPieCameraTest(pieCamArg);
    if (inputStuckArg > 0) app->setInputStuckTest(inputStuckArg);
    if (inputSourceArg > 0) app->setInputSourceTest(inputSourceArg);
    if (wheelSpeedArg > 0) app->setWheelSpeedTest(wheelSpeedArg);
    if (multiSelArg > 0) app->setMultiSelectTest(multiSelArg);
    if (cbMoveArg) app->setCbMoveTest(cbMoveArg);
    if (saveDirtyArg > 0) app->setSaveDirtyTest(saveDirtyArg);
    if (prefsWriteArg > 0) app->setPrefsWriteTest(prefsWriteArg);
    if (notifyTestArg > 0) app->setNotifyTest(notifyTestArg);
    if (autosaveTestArg > 0.0f) app->setAutosaveTest(autosaveTestArg);
    if (frameBudgetArg > 0.0f) app->setFrameBudget(frameBudgetArg);
    if (findRefsArg) app->setFindRefs(findRefsArg);
    if (projectSwitchArg) app->setProjectSwitchTest(projectSwitchArg);
    if (validateGraphArg) app->setValidateGraph(validateGraphArg);
    if (graphPrintArg) app->setGraphPrintTest(graphPrintArg);
    if (assetAssignArg) app->setAssetAssignTest(assetAssignArg);
    if (graphHitsArg) app->setGraphHitsTest(graphHitsArg);
    if (renameRepointArg) app->setRenameRepointTest(renameRepointArg);
    if (clearShaderCacheArg) app->setClearShaderCache(clearShaderCacheDirArg);
    if (recaptureArg > 0) app->setRecaptureTest(recaptureArg);
    if (viewmodelArg > 0) app->setViewmodelTest(viewmodelArg);
#endif
    if (!skinSceneDir.empty()) app->setSkinSceneDir(skinSceneDir);
    if (!shaderSourceDir.empty()) app->setShaderSourceDir(shaderSourceDir);
    // --luma-sweep: not in AVER_MODULE_SR block (applies whether or not that module is built).
    if (lumaSweepArg) app->setLumaSweep(true, lumaSweepStrideArg);
    if (fireflyMetricArg) app->setFireflyMetric(true, fireflyMultArg);
#if AVER_MODULE_SYNAPSE
    if (bakeNav) app->setBakeNavOnStart(bakeNavCell);
#else
    if (bakeNav) AVER_WARN("[Sandbox] --bake-nav ignored: this build has no navigation module");
#endif
    return app;
}

} // namespace aver
