// Entry point: createApplication and the command line it parses.
// Part of SandboxApp (declared in SandboxApp.hpp); split out of the 29,952-line SandboxApp.cpp
// on 2026-09-16 by moving method bodies verbatim.

#include "aver/runtime/EntryPoint.hpp"
#include "SandboxApp.hpp"

namespace aver {

// Defined in SandboxRender.cpp, beside the two drawWorld call sites it gates (the colour walk and
// the depth-prepass walk). A free function, not a SandboxApp::setXxx member like every other CLI
// toggle here, because this stage's brief scoped --no-walk-cache to SandboxMain.cpp/SandboxRender.cpp
// alone, outside SandboxApp.hpp. Forward-declared so main() below can call it without either file
// seeing the other's contents.
void setNoWalkCacheArg(bool on);

// True for a path ending in `ext` (which must be lower-case and include the dot),
// case-insensitively, with at least one character of stem before it.
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

// True for a path this editor will offer to open as a level (.ocmap or .ocworld -- a double-click on
// either means the same thing). The extension only decides whether to TRY, not what the file
// actually is: ElectricDreams/FirstPerson ship an .ocmap starting `OCMAP 1` that is pure OCWORLD
// content, while OpenConstructor's demoworld.ocmap -- same header, same extension -- genuinely
// needs the legacy grammar. loadLevel tells them apart by content, not extension
// (fmt::levelFileIsLegacyOcmap).
bool isLevelFile(const char* p) {
    return hasExtension(p, ".ocmap") || hasExtension(p, ".ocworld");
}

// The .ocproject owning `mapPath`, found by walking up from it (empty if none). A level names its
// mesh/material/class placements relative to the project's content dir, so a bare .ocmap with no
// project would resolve none of them, reading as a corrupt level rather than a missing project.
// Bounded to 8 levels -- far past any real Content/Maps/... nesting -- so a stray level can't
// walk to the drive root.
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

// Parses the command line and builds the editor application. Some flags do their work and exit.
Application* createApplication(int argc, char** argv) {
    u16 mcpPort=0;
    // mcpRequested: --mcp given at all. mcpPortExplicit: given WITH a numeric port (nothing may
    // override it); --mcp with no number defers to mcp.conf, resolved once engineRoot() can be asked.
    bool mcpRequested = false, mcpPortExplicit = false;
    // EVERY FLAG FROM HERE DOWN TO --gi-history-reset-at IS PARSED IN ITS OWN LOOP, not the else-if
    // chain further down (THE FLAG CHAIN, IN CHUNKS, once at MSVC's C1061 nesting limit): these loops
    // stay separate because several read a value without consuming it, which would confuse the
    // bare-path fallback if moved into the chain. Not repeated at each one below.
    // --rd-ablate N: removes one term from the ray-driven pixel shader so its cost can be attributed
    // by difference. Any non-zero value renders a deliberately wrong frame.
    int rdAblate = 0;
    // --gi-mode N: -1 is "not given" (0 is the real value, "voxel cones"), so an A/B against a
    // manifest that already picks an estimator (RENDER.GIMODE) can still be overridden.
    int giModeArg = -1;
    int rdStagesArg = -1;   // --rd-stages 0|1|2
    int denoiserArg = -1;   // --denoiser 0|1
    int reblurAccumArg = -1;   // --reblur-accum N
    // REFRACTION: the tier picks a mode, these override it. -1 is "not given" (every render override
    // here uses this sentinel, since `take()` tests for it) -- 0-means-absent would make
    // `--refraction 0` (OFF) silently undiscardable.
    int refraction = -1;
    f32 refractionStrength = -1.0f, refractionFade = -1.0f;
    // --resize-cycle N: resize the real window every N frames of a bounded run. Verification-only; it
    // reproduces a real crash -- see SandboxApp::resizeCheck() for which one and why nothing else could.
    int resizeCycleArg = 0;
    int pieCamArg = 0;
    int inputStuckArg = 0;
    int inputSourceArg = 0;
    int wheelSpeedArg = 0;
    int multiSelArg = 0;
    const char* cbMoveArg = nullptr;
    int saveDirtyArg = 0;
    int prefsWriteArg = 0;
    // --notify-test N: raise one notification of each severity N frames in, and lift the capture
    // suppression so the stack actually draws in a bounded run (otherwise unscreenshotable).
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
    // --gpu-timing takes no value, so it also gets its own full-length loop below (i+1<argc alone
    // would silently ignore a trailing --gpu-timing, the --no-rt/--no-gi bug this file already paid for).
    bool gpuTimingArg = false;
    f32 rtDenoiseMotionArg = 0.0f;   // --rt-denoise-motion, 0 = the shipped default (no taper)
    // --lighting-legacy <bits>: seeds editor::consoleLightingLegacySlot() directly (applied beside the
    // other console-slot seeds further down) rather than through a Settings field -- there is no
    // console to type `set voxi.legacyRestirSampleRing true` into on a --frames capture. 0 (default,
    // absent) means every lighting-contrast fix stays live.
    int lightingLegacyArg = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--rd-ablate"))            rdAblate = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--lighting-legacy"))      lightingLegacyArg = std::atoi(argv[i + 1]);
        // --gi-mode N: selects the indirect-diffuse estimator (0 = voxel cones, 1 = ReSTIR GI).
        if (!std::strcmp(argv[i], "--gi-mode"))              giModeArg = std::atoi(argv[i + 1]);
        // --rd-stages 0|1|2: single-pass (0), staged (1) or staged + half-rate GI (2).
        if (!std::strcmp(argv[i], "--rd-stages"))            rdStagesArg = std::atoi(argv[i + 1]);
        // --denoiser 0|1: NVIDIA NRD over the ReSTIR GI and the sky occlusion; needs the G-buffer,
        // which for most of this pass's life meant it was reachable only by also passing --gbuffer.
        if (!std::strcmp(argv[i], "--denoiser"))              denoiserArg = std::atoi(argv[i + 1]);
        // --reblur-accum N: REBLUR_DIFFUSE's history depth, the console's voxi.reblurMaxAccumulatedFrameNum.
        if (!std::strcmp(argv[i], "--reblur-accum"))          reblurAccumArg = std::atoi(argv[i + 1]);
        // --rt-denoise-motion F: see VoxiRenderer::setRtDenoiseMotionTaper.
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
    // --unlit: the Unlit view mode from the command line, so a bounded run can prove it shades
    // differently rather than a human picking it from a dropdown.
    bool unlitArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--unlit")) unlitArg = true;
    // --pt-legacy-env: seeds editor::consolePtLegacyEnvSlot() directly, applied beside that seed
    // further down -- same --frames-has-no-console reason as --lighting-legacy.
    bool ptLegacyEnvArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--pt-legacy-env")) ptLegacyEnvArg = true;
    // --clear-shader-cache: a maintenance action; a missed trailing flag would silently skip
    // clearing, the worst outcome for a "did it clear?" command.
    // --luma-sweep [STRIDE]: STRIDE is an optional frames-between-logged-samples value (default 1,
    // every frame; consumed only when present and not itself a flag). The MCP aver_run tool -- this
    // repo's preferred headless runner -- caps its returned grep matches at 60 lines (aver_mcp.py's
    // `matched[:60]`), so an unstrided sweep over a several-hundred-frame run would silently lose
    // samples past the 60th; stride keeps a long run under that cap. See lumaSweepCheck() for what
    // it measures.
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
    // --firefly-metric [MULT]: MULT is the outlier threshold multiplier over the local neighbourhood
    // (default 8.0, see lumaSweepCheck()). Shares --luma-sweep's own STRIDE (lumaSweepStride_) rather
    // than adding a second one: both flags drive the same per-frame readback cycle, so one cadence
    // knob is enough, and "--luma-sweep N --firefly-metric" already gives independent control.
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
            // Path is optional, consumed only when it doesn't itself look like a flag -- otherwise
            // "--clear-shader-cache --frames 4" would take "--frames" as the directory to clear,
            // find nothing there, and report a clean success.
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
    // --cam-translate SPEED: see SandboxApp::setCamTranslate's own comment -- translation through
    // the scene, not the rotation --cam-wobble gives.
    f32 camTranslateArg = 0.0f;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--cam-translate")) camTranslateArg = (f32)std::atof(argv[i + 1]);
    // --cam-wobble-stop N and --set NAME VALUE: see their setters' own comments. --set is the only
    // repeatable flag here, so it collects rather than overwrites -- a capture usually pins two or
    // three dials at once, and a bisection shouldn't need a rebuild to try the next pair.
    i32 camWobbleStopArg = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--cam-wobble-stop")) camWobbleStopArg = std::atoi(argv[i + 1]);
    std::vector<std::pair<std::string, std::string>> consoleSetArgs;
    for (int i = 1; i + 2 < argc; ++i)
        if (!std::strcmp(argv[i], "--set")) consoleSetArgs.emplace_back(argv[i + 1], argv[i + 2]);
    // --no-occlusion-cull: see setOcclusionCullForceOff -- a manifest recording OCCLUSIONCULL 1 (the
    // repro project this exists for) turns culling back on during load regardless of a one-shot CLI
    // setter, so this reapplies every frame rather than reusing --occlusion-cull's false case.
    bool noOcclusionCullArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-occlusion-cull")) noOcclusionCullArg = true;
    // --occlusion-waitidle / --no-occlusion-waitidle: see setOcclusionDebugForceWaitIdle -- forces or
    // releases OcclusionCuller::testBatch()'s res.waitIdle(). The member now defaults true, so
    // --occlusion-waitidle is redundant (kept for explicitness/scripts) while --no-occlusion-waitidle
    // is what actually changes behaviour, opting into the faster, unproven no-wait path.
    bool occlusionWaitIdleArg = false, occlusionNoWaitIdleArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--occlusion-waitidle")) occlusionWaitIdleArg = true;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-occlusion-waitidle")) occlusionNoWaitIdleArg = true;
    // --no-walk-cache: disables WalkLookup's four per-mesh-id GameContent probes (meshFor, boundsFor,
    // meshDefaultMaterial, partsFor), memoised per mesh id for one drawWorld call
    // (DrawWorldOptions::useMeshLookupCache). Names the negative, like --no-occlusion-cull, since
    // the cache defaults ON.
    bool noWalkCacheArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-walk-cache")) noWalkCacheArg = true;
    // --sun-set-at N ELEV AZIM / --gi-history-reset-at N: verification-only, see sunSetAtFrames_.
    int sunSetAtArg = 0, giHistoryResetAtArg = 0;
    f32 sunSetElevArg = 0.0f, sunSetAzimArg = 0.0f;
    for (int i = 1; i + 3 < argc; ++i)
        if (!std::strcmp(argv[i], "--sun-set-at")) {
            sunSetAtArg   = std::atoi(argv[i + 1]);
            sunSetElevArg = static_cast<f32>(std::atof(argv[i + 2]));
            sunSetAzimArg = static_cast<f32>(std::atof(argv[i + 3]));
        }
    // --gpu-validation: D3D12 GPU-based validation (with --debug-layer). Set straight on the RHI, like
    // --dred, before the device exists.
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--gpu-validation")) aver::rhi::setGpuValidationEnabled(true);
    // --sun-sweep START DEG_PER_FRAME: verification-only, see sunSweepFrames_. A slider DRAG rather
    // than --sun-set-at's single jump: azimuth turns DEG_PER_FRAME every frame from START on.
    // --sun-sweep-frames N: let go after N turns (sunSweepTurnsLeft_); absent = the old endless drag.
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
    // -1 is absent, not 0: 0 is a real value here ("estimate AO from the cone gather", every tier
    // below Epic) -- same reasoning --rt-shadow-denoise documents.
    int giSkyOccRays = -1;
    // Same -1 sentinel and the same reason: 1 is a real value here (a fresh direction per pixel), so
    // 0-means-absent could not express it.
    int giSkyOccTile = -1;
    f32 skyLightArg = -1.0f;   // --sky-light N; negative = flag absent
    f32 giIntensityArg = -1.0f;   // --gi-intensity F; negative = flag absent
    // -1 = flag absent. 0 is a real value for BOTH: tonemap 0 is the legacy per-channel curve, and
    // max-radiance 0 means "no clamp at all".
    int tonemapArg = -1; f32 maxRadianceArg = -1.0f;
    bool windowedArg = false;
    bool fullscreenArg = false;
    // engine-optimisation-plan, wave 1 (C-6). -1 is absent for the three GI dials, same sentinel as
    // giSkyOccRays/giSkyOccTile (0 is a real value, measurement forced OFF); meshHeapArg/lodShareVerticesArg use their own (empty string, -1).
    int giForceRebuildArg = -1;
    int giBoundedDispatchArg = -1;
    int giFreeAccumulatorArg = -1;
    std::string meshHeapArg;   // empty = absent; "default" or "upload" otherwise
    int lodShareVerticesArg = -1;
    // optimisation-wave-2, U1/section 4(a): --restir-visibility/--gi-vis-path-view/--blended-gi.
    // Strings/bools, not ints, parsed and applied after the loop -- same shape --aversr uses.
    std::string restirVisibilityArg;   // empty = absent; none|reconstructed|half|full otherwise
    bool giVisPathViewArg = false;     // --gi-vis-path-view
    std::string blendedGiArg;          // empty = absent; restir|cone otherwise
    // --view-mode: headless twin of the viewport dropdown's view-mode popup (SandboxViewport.cpp) --
    // see SandboxApp::setViewMode's own comment for the full name list. Applied straight to
    // app->setViewMode below, which does its own parsing/logging/error reporting.
    std::string viewModeArg;           // empty = absent
    for (int i=1;i<argc;++i){
        // HANDLED BEFORE THE else-if CHAIN BELOW: one more `else if` there hits MSVC's C1061 nesting
        // limit. Everything in this loop is `if`/`continue`, never `else if`.
        // --open-level-picker: open File > Open Level's modal on the first frame, so a --frames run
        // can screenshot it.
        if (!std::strcmp(argv[i],"--open-level-picker")) { openLevelPickerArg=true; continue; }
        // --gi-force-rebuild / --gi-bounded-dispatch / --gi-free-accumulator / --mesh-heap /
        // --lod-share-vertices: engine-optimisation-plan wave 1's dials (M1-M4/W3/W4/W11/W12). NOT in
        // the `i + 1 < argc` pre-chain loop above (the one seeding beam/shot/etc.): a value stored
        // there is never consumed, so it falls through and gets stored as `beam` instead.
        if (!std::strcmp(argv[i],"--gi-force-rebuild") && i+1<argc) {
            giForceRebuildArg = std::atoi(argv[++i]); continue;
        }
        if (!std::strcmp(argv[i],"--gi-bounded-dispatch") && i+1<argc) {
            giBoundedDispatchArg = std::atoi(argv[++i]); continue;
        }
        // gi-memory: the injection accumulator (~2048 MiB at Epic's 512^3) is freed after quiet GI
        // ticks BY DEFAULT now (consoleGiFreeAccumulatorSlot()'s own default, EditorConsole.hpp) --
        // pass 0 here to hold it allocated for the whole session instead, e.g. for an A/B against the
        // recreate-on-demand cost.
        if (!std::strcmp(argv[i],"--gi-free-accumulator") && i+1<argc) {
            giFreeAccumulatorArg = std::atoi(argv[++i]); continue;
        }
        // --restir-visibility none|reconstructed|half|full: optimisation-wave-2's U1
        // (Settings::giRestirVisibility). Parsed and applied after the loop, beside app->setGiMode(giModeArg).
        if (!std::strcmp(argv[i],"--restir-visibility") && i+1<argc) {
            restirVisibilityArg = argv[++i]; continue;
        }
        // --gi-vis-path-view: U1's path-debug view (2.10 I) -- see consoleGiVisPathViewSlot()'s own
        // comment (EditorConsole.hpp). Takes no value, so it needs no i+1<argc guard.
        if (!std::strcmp(argv[i],"--gi-vis-path-view")) { giVisPathViewArg = true; continue; }
        // --blended-gi restir|cone: section 4(a)'s W6/M5 pricing switch -- see
        // consoleBlendedGiConeSlot()'s own comment (EditorConsole.hpp).
        if (!std::strcmp(argv[i],"--blended-gi") && i+1<argc) { blendedGiArg = argv[++i]; continue; }
        // --view-mode lit|unlit|wireframe|rayhit-instance|rayhit-material|rayhit-distance|triangles|
        // undenoised: see viewModeArg's own comment above.
        if (!std::strcmp(argv[i],"--view-mode") && i+1<argc) { viewModeArg = argv[++i]; continue; }
        // --mesh-heap default|upload: W4. A string, not a 0/1 int, so an unrecognised spelling can be
        // reported by name at the application site below rather than silently misread as a number.
        if (!std::strcmp(argv[i],"--mesh-heap") && i+1<argc) {
            meshHeapArg = argv[++i]; continue;
        }
        // --lod-share-vertices 0|1: W11.
        if (!std::strcmp(argv[i],"--lod-share-vertices") && i+1<argc) {
            lodShareVerticesArg = std::atoi(argv[++i]); continue;
        }
        // --gi-sky-occlusion-rays N: how many sky-visibility rays the ambient term traces per pixel.
        // Shipped tier-derived with no flag/manifest key, so never swept. NOT the same question as
        // --rt-rays (rtSkyOcclusion, voxi.hlsl): sun rays are coherent, walking the same BVH nodes
        // (three more cost 0.05 ms); these are cosine-distributed, so every lane in a wave descends
        // a different part of the tree and one more costs 5.37 ms -- "ray count is nearly free" is a
        // measured property of the coherent ray and does not transfer here.
        if (!std::strcmp(argv[i],"--gi-sky-occlusion-rays") && i+1<argc) {
            giSkyOccRays = std::atoi(argv[++i]); continue;
        }
        // --gi-sky-occlusion-tile N: pixels per shared sky-occlusion ray direction, the lever the ray
        // names for its own cost; was a compile-time #define set nowhere, so the trade it governs
        // (cheaper rays against correlated noise) had never been measured.
        if (!std::strcmp(argv[i],"--gi-sky-occlusion-tile") && i+1<argc) {
            giSkyOccTile = std::atoi(argv[++i]); continue;
        }
        // --sky-light N: scales the sky ambient (SkyAtmosphere::skyLightIntensity, "Sky Light" slider,
        // 1 default; 0 removes the term entirely). Had a slider and no flag, so diffAmbient --
        // added to every surface without real occlusion, attenuated by a six-cone AO and nothing
        // else -- and its share of a washed-out picture (sky Rayleigh-blue, stone bounce warm,
        // ambient pulling grey) had never been measured.
        if (!std::strcmp(argv[i],"--sky-light") && i+1<argc) {
            skyLightArg = (f32)std::atof(argv[++i]); continue;
        }
        // --gi-intensity F: multiplier on the cone-traced bounce (1 default, 0 direct light only).
        // The editor has a slider and RENDER.GIINTENSITY the manifest key; this is the missing third way.
        if (!std::strcmp(argv[i],"--gi-intensity") && i+1<argc) {
            giIntensityArg = (f32)std::atof(argv[++i]); continue;
        }
        // --tonemap 0|1: 0 the original per-channel ACES approximation (every recorded gate baseline's
        // measurement), 1 the matrixed fit that keeps saturation (PostSettings::tonemap).
        if (!std::strcmp(argv[i],"--tonemap") && i+1<argc) { tonemapArg = std::atoi(argv[++i]); continue; }
        // --windowed: the DEFAULT now, kept so scripts and habits that pass it still work rather
        // than erroring on an unknown flag. --fullscreen is the one that changes anything.
        if (!std::strcmp(argv[i],"--windowed")) { windowedArg = true; continue; }
        // --fullscreen: borderless, sized to the monitor. Off by default (an editor sits beside other
        // windows -- covering the taskbar is a game's behaviour, not a tool's); a capture run stays
        // windowed regardless, so this cannot reshape the client area a recorded gate probe was
        // measured in.
        if (!std::strcmp(argv[i],"--fullscreen")) { fullscreenArg = true; continue; }
        // --max-radiance F: ceiling on scene radiance just before the tonemap; 0 disables it. The sun
        // disc is the thing this exists for -- see PostSettings::maxRadiance.
        if (!std::strcmp(argv[i],"--max-radiance") && i+1<argc) {
            maxRadianceArg = static_cast<f32>(std::atof(argv[++i])); continue;
        }
        // --no-editor-chrome: draw the scene, nothing the editor adds on top (grid, navmesh overlay,
        // gizmo, sculpt cursor, viewport icons). Lets scripts/verify-game.ps1 diff Sandbox.exe against
        // AverEngineRuntime.exe's probe codes without the grid (the game correctly omits) dominating
        // the diff -- the check whose absence got the packaged game deleted ("a second host rendered
        // a different subset of the scene"). Not routed through showGrid_ (persists to editor.ini,
        // and a comparison run must not change what the next interactive session looks like): a
        // run-scoped override the draw sites read directly.
        if (!std::strcmp(argv[i],"--no-editor-chrome")) { noEditorChrome=true; continue; }
        // --scene-census: print one canonical line describing what the loaded level put in the world.
        // AverEngineRuntime.exe prints the identical format for scripts/verify-game.ps1 to compare (world/SceneCensus.hpp).
        if (!std::strcmp(argv[i],"--scene-census")) { sceneCensus=true; continue; }
        // --open-level <name-or-relative-path>: open a level by name through the same requestOpenLevel
        // funnel the picker/Content Browser use. Two jobs: it's the convenient way to start on a
        // level that isn't the start map, and the only way a bounded --frames run can exercise that
        // funnel at all -- the picker itself needs a click.
        if (!std::strcmp(argv[i],"--open-level") && i+1<argc) { openLevelArg=argv[++i]; continue; }
        // --layered-bsdf N: 0=Off 1=Low 2=Medium 3=High 4=Epic. Alone this changes nothing visible,
        // correctly: it selects a shader variant that can evaluate a coat, and coatWeight defaults 0. Pair with --coat.
        if (!std::strcmp(argv[i],"--layered-bsdf") && i+1<argc) { layeredBsdf=std::atoi(argv[++i]); continue; }

        // --coat <weight> [roughness] [f0]: give the editor's own placeholder materials a coat. A flag,
        // not an authored asset: the scenes worth measuring (Floor/Cube, --furnace-grid) are built in
        // C++, and a .ocmat would test the parser (MaterialTest already does), not the shading.
        if (!std::strcmp(argv[i],"--coat") && i+1<argc) {
            coatWeight = (f32)std::atof(argv[++i]);
            if (i+1<argc && argv[i+1][0] != '-') coatRough = (f32)std::atof(argv[++i]);
            if (i+1<argc && argv[i+1][0] != '-') coatF0    = (f32)std::atof(argv[++i]);
            continue;
        }

        // --new-project <location> <name> scaffolds a project and exits, touching no device.
        // THE FLAG CHAIN, IN CHUNKS OF ~30: was one else-if chain of 119 branches until it hit MSVC's
        // C1061 block-nesting limit (the build fails at an unrelated line), so it was split into
        // chunks (each chunk still an else-if chain running its first match; its final `else` records
        // the miss, then the next chunk runs -- so exactly one branch runs, in the same order as
        // before). A new flag goes at the end of the last chunk; start a new chunk the same way past ~30.
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
        // --new-project-template <location> <name> <templateId> scaffolds a project from a shipped
        // template and exits, touching no device -- the exact --new-project precedent, for the New
        // Project modal's template picker. listTemplates()/scaffoldProjectFromTemplate() are pure filesystem.
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
        // --import-gltf <src.gltf|.glb> <destDir> imports a model and exits, touching no device -- the
        // Content Browser's Import button's other caller (same importGltfToDir SandboxApp::importModel
        // runs). destDir is created if missing, so this also works as a brand new project's first write.
        else if (!std::strcmp(argv[i],"--import-gltf") && i+2<argc) {
            const std::string src = argv[++i], destDir = argv[++i];
            // The owning project's content dir, when destDir sits inside one (destDir need not
            // itself be the Content root, only somewhere under a project) -- needed to cook
            // materials/textures (fmt::cookImportedMaterials). No owning project -- the
            // launcher-less/scratch-directory case this flag has always supported -- gets geometry only.
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
        // --upgrade-project <path.ocproject> applies what the prompt applies, and exits. BOTH HALVES:
        // inspectProject/applyProjectUpgrade repairs scaffold gaps (missing folder, stale .csproj
        // reference, version-independent), while migrateProject runs the version chain and stamps
        // the manifest -- a project can need either, both or neither (running only the first left a
        // 0.2 project stamped 0.2 but reporting "already current").
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
            // Version chain runs regardless of the scaffold (different questions); a failure here is
            // reported and NOT stamped, so the project is asked again next time.
            if (!editor::migrateProject(manifest, &why)) {
                AVER_ERROR("[Sandbox] version migration failed: {}", why);
                std::exit(1);
            }
            AVER_INFO("[Sandbox] '{}' upgraded and stamped {}", p.name, kEngineVersion);
            std::exit(0);
        }
        // --landscape-gen <path> [sampleCount] [spacingCm] writes a synthetic rolling-hill .ocland
        // through the real writeOcLand/loadOcLand round trip, exits, touches no device -- no .ocland
        // fixture exists anywhere else, and a hand-rolled binary fixture would prove nothing about
        // the real format. sampleCount must be (k*64)+1; default 257 is three LOD levels, small
        // enough to build in a --frames run.
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
            // CENTRED ON THE ORIGIN, so the section sits under wherever a level's own placements
            // already are rather than requiring the level to be authored around the terrain instead.
            d.originCm[0] = -extentCm * 0.5f; d.originCm[1] = -extentCm * 0.5f; d.originCm[2] = 0.0f;
            d.heights.resize(static_cast<usize>(samples) * samples);
            for (u32 iy = 0; iy < samples; ++iy) {
                for (u32 ix = 0; ix < samples; ++ix) {
                    const f32 fx = static_cast<f32>(ix), fy = static_cast<f32>(iy);
                    // Two overlapping sine fields, index-frequency (not world-frequency): real relief
                    // at any spacing, so LOD selection has something to refine against, not a flat plane.
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
        // --sculpt-test <in.ocland> <out.ocland> [mode] [radiusCm] [strengthCm] applies one brush
        // stroke through the exact functions the editor's own sculpt tools call (landscape::applyBrush,
        // fmt::saveOcLand), centred on the section's middle, exits touching no device -- "a sculpt
        // changes the stored heights" needs a real round trip, not a claim about code nobody ran.
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
            sp.flattenTargetCm = sd.heightAt(cix, ciy) + 500.0f;   // only read by Flatten
            // Ramp only: an arbitrary start point a quarter of the section's extent from centerCm, so
            // the two endpoints this mode interpolates between are actually distinct.
            sp.rampStartCm[0] = sp.centerCm[0] - sd.extentCm() * 0.25f;
            sp.rampStartCm[1] = sp.centerCm[1];
            sp.rampStartHeightCm = sd.heightAt(cix, ciy);
            // Noise only: a fixed literal, not the clock -- see BrushParams::noiseSeed -- so running
            // this flag twice on the same input is a determinism check, not a coin flip.
            sp.noiseSeed = 20260913u;
            const f32 sculptBefore = sd.heightAt(cix, ciy);
            // Eight ticks at amount 0.25 (~130ms held, handleSculpt's dt*6 rate), not one amount=1
            // jump, so the result depends on strength/radius, not just "did anything change".
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
        // --water <heightCm> puts a Gerstner surface and a buoyancy plane at that height. Opt-in, not
        // default: a water plane is an infinite sheet and would surprise every level ever opened.
        else if (!std::strcmp(argv[i],"--water") && i+1<argc) {
            waterOn = true;
            waterHeight = static_cast<f32>(std::atof(argv[++i]));
        }
        // --open-asset <path> opens a file through the same host a double-click goes through.
        else if (!std::strcmp(argv[i],"--open-asset") && i+1<argc) openAsset=argv[++i];
        // --select <substring>: see setSelectEntity for why this exists.
        else if (!std::strcmp(argv[i],"--select") && i+1<argc) selectEntity=argv[++i];
        // --open-legacy: open a project from an older series as-is, upgrading nothing. Without it
        // a frame-limited run against such a project silently measures an empty editor.
        else if (!std::strcmp(argv[i],"--open-legacy")) openLegacy=true;
        // --graph-tab viewport: front the graph editor's inner Viewport tab (component tree +
        // preview) so a capture run can prove it draws.
        else if (!std::strcmp(argv[i],"--graph-tab") && i+1<argc) graphTab=argv[++i];
        // --graph-select <nodeId>: select a node in the just-opened .ocgraph, one frame later -- see
        // SandboxApp::setGraphSelectNode's own comment for why.
        else if (!std::strcmp(argv[i],"--graph-select") && i+1<argc) graphSelectNode=argv[++i];
        // --actor-live brings an actor tab up with LIVE already on.
        else if (!std::strcmp(argv[i],"--actor-live")) editor::setActorEditorLiveByDefault(true);
        else if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--input-probe")) inputProbe=true;
        else if (!std::strcmp(argv[i],"--auto-compile")) autoCompile=true;
        else if (!std::strcmp(argv[i],"--focus-level-at") && i+1<argc) focusLevelAt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--project-settings")) focusVoxi=true;
        // --project-settings-page N: verification-only, see setProjectSettingsPage's own comment.
        else if (!std::strcmp(argv[i],"--project-settings-page") && i+1<argc) projectSettingsPage=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--editor-prefs")) showPrefs=true;
        // --scroll-prefs-to-keybinds: see buildEditorPrefs()'s own comment on the flag it sets.
        else if (!std::strcmp(argv[i],"--scroll-prefs-to-keybinds")) scrollPrefsToKeybinds=true;
        else if (!std::strcmp(argv[i],"--hud-preview") && i+1<argc) hudTest=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--save-project")) saveProject=true;
        else if (!std::strcmp(argv[i],"--import") && i+2<argc) { importSrc=argv[++i]; importDst=argv[++i]; }
        else if (!std::strcmp(argv[i],"--new-script")) focusScript=true;
        else if (!std::strcmp(argv[i],"--tools-menu")) focusTools=true;
        else if (!std::strcmp(argv[i],"--compile-menu")) focusCompileMenu=true;
        // --mcp [port] opens the editor control channel (opt-in listening socket). With no number,
        // the port comes from mcp.conf's editor_bridge.port or 45123, resolved after this loop.
        else if (!std::strcmp(argv[i],"--mcp")) {
            mcpRequested = true;
            if (i+1 < argc && argv[i+1][0] != '-') {
                // Range-checked, unlike the bare `(u16)std::atoi(...)` this replaces: that cast
                // silently truncated (`--mcp 99999` bound port 34463 mod 65536 and logged it as
                // though typed) and `--mcp 0` counted as explicit yet never opened a channel (0 is
                // also this parser's "absent" value).
                const char* raw = argv[++i];
                const long  p   = std::strtol(raw, nullptr, 10);
                if (p >= 1 && p <= 65535) {
                    mcpPort = (u16)p;
                    mcpPortExplicit = true;
                } else {
                    // Not fatal, deliberately: the channel is a debugging aid, and refusing to start
                    // the whole editor over a mistyped port would be worse than falling back like
                    // every unset value here.
                    AVER_WARN("[Mcp] --mcp {} is not a port (1-65535); falling back to mcp.conf or "
                              "the built-in default", raw);
                }
            }
        }
        else if (!std::strcmp(argv[i],"--compile-scripts")) focusCompile=true;
        // --reload-scripts [N] fires Tools > Reload Scripts once, N frames in (default 20).
        else if (!std::strcmp(argv[i],"--reload-scripts")) {
            reloadAt = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 20;
        }
        else if (!std::strcmp(argv[i],"--start-screen")) startScreen=true;
        else argMatched = false;
        if (!argMatched) { argMatched = true;   // flag chunk 2: only if no earlier chunk matched
        // --chunk-stream [N]: streaming on N frames in (default 5), the "wait to settle" pattern
        // --reload-scripts uses, so a --frames capture can prove streaming happened without a human
        // clicking the menu. --drone-graph <path>: the .ocgraph the drone runs; without it the drone
        // spawns and sits still -- honest for an engine that doesn't know any project's scripts.
        if (!std::strcmp(argv[i],"--drone-graph") && i+1<argc) droneGraph = argv[++i];
        // --landscape <path.ocland> overrides the level's LANDSCAPE record/levelname.ocland convention
        // (GameLandscape::setPathOverride) so --frames can prove LOD selection draws real terrain
        // without a level file naming one, and without a human clicking anything.
        else if (!std::strcmp(argv[i],"--landscape") && i+1<argc) landscapePath = argv[++i];
        else if (!std::strcmp(argv[i],"--chunk-stream")) {
            chunkStream = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --no-chunk-stream: the counterpart to streaming now being ON by default. Negative is
        // "explicitly off", distinct from 0's "flag not given" -- otherwise the default would be
        // unturnoffable, and a frame-time comparison against a still-climbing triangle count is not
        // a comparison.
        else if (!std::strcmp(argv[i],"--no-chunk-stream")) chunkStream = -1;
        // --fog-match [opacity] ties fog density to the streaming radius, the same thing the Height
        // Fog panel's checkbox does -- opt-in since the effect is markedly foggier, and a headless
        // run could never otherwise exercise it.
        else if (!std::strcmp(argv[i],"--fog-match")) {
            fogMatch = true;
            if (i+1 < argc && argv[i+1][0] != '-') fogMatchOpacity = static_cast<f32>(std::atof(argv[++i]));
        }
        // --drone [N] switches the graph-driven drone on N frames in (default 5), same shape and
        // reason as --chunk-stream just above: proves it headlessly without a human clicking the menu.
        else if (!std::strcmp(argv[i],"--drone")) {
            droneAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --undo-test [N]: fires runUndoTest() N frames in (default 10), then exits the process with
        // 0/1 -- longer than --chunk-stream/--drone's delay since it needs a live scene::World.
        else if (!std::strcmp(argv[i],"--undo-test")) {
            undoTestAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 10;
        }
        // --keybind-test write|read [N]: fires runKeybindPersistTest(mode) N frames in (default 10),
        // then EXITS THE PROCESS with 0/1 -- see that function's own comment.
        else if (!std::strcmp(argv[i],"--keybind-test") && i+1<argc) {
            keybindTestMode = argv[++i];
            keybindTestAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 10;
        }
        // --drawer log|content[:<sub>]|console[:<seed>] opens a bottom drawer, optionally in a
        // Content subfolder or (console, verification-only) with its input box pre-seeded -- a
        // screenshot script has no mouse to type with, so this is how it gets the live suggestion
        // popup into shot (see drawConsoleTranscriptTab/drawerStartSub_). FIXED here while adding
        // it: the kind is now compared BEFORE the colon -- "--drawer console:gi" used to fail the
        // "console" strcmp (v was the whole "console:gi") and silently fall through to Content.
        else if (!std::strcmp(argv[i],"--drawer") && i+1<argc) {
            const char* v = argv[++i];
            const char* colon = std::strchr(v, ':');
            const std::string kind = colon ? std::string(v, static_cast<size_t>(colon - v)) : std::string(v);
            drawerOpen = kind == "log" ? 2 : kind == "console" ? 3 : 1;
            if (colon) drawerSub = colon + 1;
        }
        else if (!std::strcmp(argv[i],"--msaa") && i+1<argc) msaa=std::atoi(argv[++i]);
        // --gi [tier], --rt [tier], --pt [tier]: quality 0..4 (Off..Epic); bare means High. All three
        // take an optional tier and use -1 for "flag absent", same shape as --rt-render-mode/
        // --rt-shadow-denoise -- fixed defects where --gi/--rt parsed no numeric form (`--gi 2` set
        // High, dropped "2" to the positional handler) and 0 (a real tier) could not be expressed,
        // blocking a raster-vs-ray-driven measurement with PT view off.
        else if (!std::strcmp(argv[i],"--gi") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            gi=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--gi")) gi=3;
        else if (!std::strcmp(argv[i],"--no-gi")) noGi=true;
        else if (!std::strcmp(argv[i],"--gi-debug")) { gi=3; giDbg=true; }
        // --no-gi-cone: the A/B toggle (VoxiRenderer::setConeTraceEnabled/setGiConeTraceOff). Distinct
        // from --no-gi (also stops the volume being built): this only stops PSMainVoxi/PSClusterMain reading it.
        else if (!std::strcmp(argv[i],"--no-gi-cone")) giConeOff=true;
        else if (!std::strcmp(argv[i],"--rt") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            rt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--rt")) rt=3;
        // --pt [tier]: path tracing quality, the switch ptBounces is spent under. `--pt 0` turns the
        // reference view OFF -- needed on any raster comparison against a RENDER.PATHTRACING manifest,
        // since it suppresses the raster scene whenever registered.
        else if (!std::strcmp(argv[i],"--pt") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            pt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt")) pt=3;
        // --no-gi / --no-rt: kept as the shorter spelling every existing script/gate config uses,
        // though no longer the only way to reach the Off rung now that the tier flags take a real 0.
        else if (!std::strcmp(argv[i],"--no-rt")) noRt=true;
        // --cam-wobble DEG PERIOD: see setCamWobble. Measurement-only; 0 degrees is no motion,
        // so every existing capture is bit-identical without it.
        else if (!std::strcmp(argv[i],"--cam-wobble") && i+2<argc) {
            camWobbleDeg=(f32)std::atof(argv[++i]); camWobblePeriod=std::atoi(argv[++i]);
        }
        // The sun occlusion rays per pixel, so the cost of ray-traced shadows can be MEASURED
        // instead of asserted: the sequence is nested, so 1, 2, 4, 8 is one converging series.
        else if (!std::strcmp(argv[i],"--rt-rays") && i+1<argc) rtRays=std::atoi(argv[++i]);
        // The ray-traced shadow's temporal amortisation tile edge -- how many pixels share one
        // traced ray, rounded to the nearest power of two. 1 (unset) traces every pixel every frame.
        else if (!std::strcmp(argv[i],"--rt-pixels-per-ray") && i+1<argc) rtPixelsPerRay=std::atoi(argv[++i]);
        // The SPATIAL filter radius in pixels, 0 = off. Distinct from the tile edge above: that one
        // amortises over TIME and lags the camera, this averages over SPACE with no history.
        else if (!std::strcmp(argv[i],"--rt-shadow-denoise") && i+1<argc) rtShadowDenoise=std::atoi(argv[++i]);
        // --rt-render-mode 0|1: which thing finds the first surface (0 raster, 1 primary rays) -- ray
        // tracing: one hit, direct lighting. --pt-bounces N: path tracing, bounces after that hit,
        // spent only while pathTracing is on (VoxiRenderer enforces this, not assumes it).
        else if (!std::strcmp(argv[i],"--rt-render-mode") && i+1<argc) rtRenderMode=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt-bounces") && i+1<argc) ptBounces=std::atoi(argv[++i]);
        // How many frames apart the GI volume is revoxelised -- 1 (unset) rebuilds every frame, the
        // original always-fresh behaviour. Measures the voxelise+filter amortisation on its own, per
        // the "measure each change, don't stack guesses" rule.
        else if (!std::strcmp(argv[i],"--gi-update-interval") && i+1<argc) giUpdateInterval=std::atoi(argv[++i]);
        // The scene's own render resolution as a fraction of the present/swapchain size -- 1.0 (unset)
        // reproduces the pre-existing 1:1 behaviour exactly. Clamped to [0.25,1] by the device;
        // isolates the render-scale/GI-cost tradeoff.
        else if (!std::strcmp(argv[i],"--render-scale") && i+1<argc) renderScale=static_cast<f32>(std::atof(argv[++i]));
        // --aversr LEVEL: off|quality|balanced|performance (case-insensitive, docs/AVERSR.md). Stored
        // as a string, parsed below, so a build with the module compiled out still recognises the flag
        // and explains why it did nothing rather than erroring as unknown.
        else if (!std::strcmp(argv[i],"--aversr") && i+1<argc) aversrArg=argv[++i];
        // --depth-prepass: same-frame depth-only pass ahead of the opaque colour walk, so an occluded
        // fragment skips PSMainVoxi's shadow lookup/cone trace/fog entirely (setDepthPrepassOverride).
        // Unset reproduces pre-existing behaviour exactly.
        else if (!std::strcmp(argv[i],"--depth-prepass")) depthPrepass=true;
        else argMatched = false;
        }   // end of flag chunk 2
        if (!argMatched) { argMatched = true;   // flag chunk 3: only if no earlier chunk matched
        // --gbuffer: the thin forward-pass G-buffer (IDevice::setGBufferEnabled) -- velocity, view-space
        // depth, world normal+roughness, written alongside the scene pass. Unset never calls it, so an
        // unmodified run stays bit-identical to a tree that never heard of it.
        if (!std::strcmp(argv[i],"--gbuffer")) gbuffer=true;
        // --gbuffer-debug MODE (velocity|viewz|normals): forces the G-buffer on (like --gi-debug forces
        // --gi), selects the channel GBufferDebugFeature draws -- lets --probe assert on a texture
        // nothing else samples yet.
        else if (!std::strcmp(argv[i],"--gbuffer-debug") && i+1<argc) { gbuffer=true; gbufferDebug=argv[++i]; }
        // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). Unset (default)
        // reproduces pre-existing behaviour bit for bit -- same contract as --depth-prepass/--edge-aa.
        else if (!std::strcmp(argv[i],"--occlusion-cull")) occlusionCull=true;
        // --edge-aa: FxaaResolve through the same rhi::IUpscaler seam --aversr uses (edgeAaEnabled_ --
        // the two share one slot). A setting, not a hard MSAA replacement: runs whatever sample count
        // --msaa asked for.
        else if (!std::strcmp(argv[i],"--edge-aa")) edgeAa=true;
        else if (!std::strcmp(argv[i],"--frame-time")) frameTime=true;
        else if (!std::strcmp(argv[i],"--ms")) ms=true;
        else if (!std::strcmp(argv[i],"--probe") && i+2<argc) { probeX=(u32)std::atoi(argv[++i]); probeY=(u32)std::atoi(argv[++i]); }
        // --force-caps clamps what the device reports; it can never raise a capability.
        else if (!std::strcmp(argv[i],"--force-caps") && i+1<argc) forceCaps=argv[++i];
        else if (!std::strcmp(argv[i],"--warp")) warp=true;
        else if (!std::strcmp(argv[i],"--backend") && i+1<argc) backendName=argv[++i];
        else if (!std::strcmp(argv[i],"--debug-layer")) debugLayer=true;
        // --dred: D3D12 Device Removed Extended Data (auto-breadcrumbs + page-fault reporting), dumped
        // on device loss (noteDeviceRemoved). Set via rhi::setDredEnabled below (like --device-lost-at)
        // before D3D12CreateDevice runs; works without --debug-layer.
        else if (!std::strcmp(argv[i],"--dred")) dred=true;
        else if (!std::strcmp(argv[i],"--scripts") && i+1<argc) scriptsDir=argv[++i];
        // --shader-source <dir>: read HLSL from this tree instead of bin/shaders, reloading on change.
        // Applied here during parsing since the first shaderFile() happens while the renderer builds
        // pipelines -- set later, it would be read after the fact and do nothing.
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
        // --no-particle-gi: see SandboxApp::setNoParticleGi's own comment.
        else if (!std::strcmp(argv[i],"--no-particle-gi")) noParticleGi=true;
        // --particle-stress <N> <M>: VERIFICATION-ONLY, see setParticleStress's own comment.
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
        // --pt-scene-toggle-on/-off [N]: verification-only (ptSceneToggleOnAutoFrames_), same
        // "[N] optional, default given" shape as --chunk-stream. --device-lost-at <N>: simulates GPU
        // loss after N presented frames, exercising the device-lost path without a real GPU disappearing.
        else if (!std::strcmp(argv[i],"--device-lost-at") && i+1<argc) deviceLostAt=std::atoi(argv[++i]);
        else argMatched = false;
        }   // end of flag chunk 3
        if (!argMatched) { argMatched = true;   // flag chunk 4: only if no earlier chunk matched
        // --crash-test <kind>: deliberately kill this process, to prove the crash handler works -- an
        // untested reporter is a hope, not a feature, found out on the worst day (UE has the same
        // thing, `debug crash`). Kinds share no code: `av` faults the SEH filter,
        // `assert`/`fatal` go through AVER_ASSERT/AVER_FATAL, `critical` logs and exits cleanly.
        if (!std::strcmp(argv[i],"--crash-test") && i+1<argc) crashTest=argv[++i];
        // --pt-quality-ramp [N]: raise the Path Tracing rung one step every N frames, on a LIVE
        // view. Verification-only, same "[N] optional, default given" shape as the toggles below.
        else if (!std::strcmp(argv[i],"--pt-quality-ramp")) {
            ptQualityRamp = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 20;
        }
        else if (!std::strcmp(argv[i],"--pt-scene-toggle-on")) {
            ptSceneToggleOn = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        else if (!std::strcmp(argv[i],"--pt-scene-toggle-off")) {
            ptSceneToggleOff = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 15;
        }
        // --aversr-cycle [N]: turn AverSR on and straight off again at frame N. Verification-only,
        // and it reproduces a real crash -- see SandboxApp::averSrCycleFrames_.
        else if (!std::strcmp(argv[i],"--aversr-cycle")) {
            aversrCycle = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 30;
        }
        else if (!std::strcmp(argv[i],"--skin-scene-test") && i+1<argc) skinSceneDir=argv[++i];
        // --bake-nav [cellCm]: bake this level's navigation once at startup, write its .ocnav.
        // Deferred to a frame, not init: the bake samples physics and applyProject, which builds the
        // level's bodies, hasn't run yet.
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
        // --lod-select [px]: virtualized-geometry per-instance LOD level selection
        // (aver::trifactor::ClusterAdapt). Optional pixel error budget, default 1.0px.
        else if (!std::strcmp(argv[i],"--lod-select")) {
            lodSelect=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --no-lod-select: every instance at LOD 0, whatever the Cook wrote (the old default, until
        // measured at 102.7ms median vs 76.7ms with selection, same camera). Kept as the escape hatch
        // for telling a selection artefact from a real one.
        else if (!std::strcmp(argv[i],"--no-lod-select")) lodSelect=false;
        // --lod-cluster-stats: turns on the informational per-meshlet frustum/cone-cull counters on
        // top of --lod-select. Separate flag on purpose -- see lodClusterStatsEnabled_'s own comment.
        else if (!std::strcmp(argv[i],"--lod-cluster-stats")) lodClusterStats=true;
        // --lod-per-cluster [px]: per-cluster virtualized-geometry selection (replaces --lod-select's
        // per-level choice when both are given). Same pixel-error-budget knob, default 1.0px.
        else if (!std::strcmp(argv[i],"--lod-per-cluster")) {
            lodPerCluster=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --lod-mesh-shader [px]: the GPU per-cluster path (amplification+mesh shader). Wins over
        // --lod-per-cluster/--lod-select for an instance with GPU cluster data and a live pipeline;
        // falls back per-instance otherwise. Same pixel-error-budget knob the other two share.
        else if (!std::strcmp(argv[i],"--lod-mesh-shader")) {
            lodMeshShader=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --no-lod-mesh-shader forces the GPU per-cluster path OFF. Needed because the path is now on
        // by default wherever supported (onInit) -- how its 2.8x speedup was measured.
        else if (!std::strcmp(argv[i],"--no-lod-mesh-shader")) lodMeshShader=0;
        else if (!std::strcmp(argv[i],"--ui-demo")) uiDemo=true;
        // Coverage is optional: `--clouds` alone takes the authored default.
        else if (!std::strcmp(argv[i],"--clouds")) {
            clouds=1;
            if (i+1 < argc && argv[i+1][0] != '-') cloudCover=static_cast<f32>(std::atof(argv[++i]));
        }
        // The derived sky, with an optional sun elevation in degrees.
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
        // --mode <select|landscape|foliage|simulate>: the editing mode to start in, the peer of --tool
        // -- automation/screenshots need it since a mode's panel can't be captured through a mouse click.
        // Applied after the project loads, since a mode can be refused for lack of terrain.
        else if (!std::strcmp(argv[i],"--mode") && i+1<argc) startMode=argv[++i];
        else if (!std::strcmp(argv[i],"--tool") && i+1<argc) {
            const char* t=argv[++i];
            tool = !std::strcmp(t,"move")?Tool::Move : !std::strcmp(t,"rotate")?Tool::Rotate :
                   !std::strcmp(t,"scale")?Tool::Scale :
#if AVER_MODULE_LANDSCAPE

#endif
                   Tool::Select;
        }
        // A BARE PATH IS WHATEVER ITS EXTENSION SAYS IT IS: this shell entry point must recognise every
        // type the engine registers. A .ocmap used to fall through to `beam`, where loadOcbeam refused
        // it and opened an empty editor.
        else if (argv[i][0]!='-') {
            if      (isOcproject(argv[i])) project = argv[i];
            else if (isLevelFile(argv[i])) openMap = argv[i];
            else                           beam    = argv[i];
        }
        }   // end of flag chunk 4
    }

    // SINGLE-INSTANCE FORWARDING, SENDER-SIDE GATE, placed after the bare-arg loop sets project/openMap.
    // The narrowest thing that could work: argv[1][0]!='-' excludes the engine's gate-sweep/import/
    // scaffold workflow (many flag-bearing processes at once); argc==2 excludes `Sandbox.exe
    // level.ocmap --frames 600` (argc==4), which would otherwise get forwarded to -- or become
    // eligible to receive forwards from -- another instance and interfere with its capture; the
    // project/openMap check excludes a bare .ocbeam, which has no file association to forward.
    if (argc == 2 && argv[1][0] != '-' && (!project.empty() || !openMap.empty())) {
        // Resolved to absolute before leaving this process: the receiver's working directory has no
        // reason to match this one's, and ownerProjectOf/equivalent() there need a matching path.
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(argv[1], ec);
        const std::string forwardPath = ec ? std::string(argv[1]) : abs.string();
        if (Window::forwardToSingleInstancePrimary(forwardPath)) {
            AVER_INFO("[Sandbox] '{}' handed to the running editor instance; this process exits",
                      forwardPath);
            std::exit(0);
        }
        // Every other outcome (no primary, one that declined because a different project is open, a
        // timed-out send) means the same thing: build this process's own Application/Engine exactly as
        // a bare launch would. Nothing below needs to know a forward was attempted.
    }

    // Before the engine creates a device: the backend queries the hardware inside Engine::run.
    if (forceCaps && !rhi::setCapsOverride(forceCaps))
        AVER_ERROR("[Sandbox] --force-caps '{}' was rejected; running on the UNCLAMPED device", forceCaps);

    // A LEVEL NAMED ON ITS OWN BRINGS ITS PROJECT WITH IT: it's what makes the level's own contents
    // resolvable (ownerProjectOf). An explicit .ocproject argument still wins.
    if (!openMap.empty() && project.empty()) {
        const std::string owner = ownerProjectOf(openMap);
        if (!owner.empty()) project = owner;
        else AVER_WARN("[Sandbox] '{}' is not inside a project (no .ocproject above it); its "
                       "placements will not resolve", openMap);
    }

    // --crash-test, acted on here: before the window, device, or project exist, so it exercises the
    // handler alone -- placed later, a failure could as easily be the renderer as the thing under test.
    if (!crashTest.empty()) {
        if (crashTest == "critical") {
            // The only kind that does NOT crash: logs one Critical (waking the standby reporter), then
            // exits cleanly -- verifying the quiet path matters as much as the loud one: a reporter
            // popping a window every GPU hiccup would get disabled within a day.
            AVER_CRITICAL("[Sandbox] --crash-test critical: a deliberate critical, not a real fault. "
                          "The crash reporter is now watching this process and should say nothing "
                          "when it exits cleanly.");
        } else {
            // Every other kind ends the process, so the reporter must not put a window on screen --
            // an automated run has nobody to close it.
            crash::setLaunchReporter(false);
            AVER_WARN("[Sandbox] --crash-test {}: about to end this process deliberately", crashTest);
            if (crashTest == "assert") {
                AVER_ASSERTM(false, "deliberate --crash-test assert");
            } else if (crashTest == "fatal") {
                AVER_FATAL("[Sandbox] deliberate --crash-test fatal");
            } else if (crashTest == "av") {
                // volatile so the null write cannot be optimised into unreachable-code elimination,
                // which is exactly what a release build does to an obvious null dereference.
                volatile int* p = nullptr;
                *p = 1;
            } else if (crashTest == "oom") {
                // Kind::OutOfMemory, through the new-handler (crash::install) -- a real allocation
                // failure, not a thrown bad_alloc: an earlier version threw one and it reported as
                // `Crash` (an escaping C++ exception on Windows raises SEH 0xE06D7363, taken before
                // std::terminate runs); that measurement is why the new-handler exists, keep this an
                // allocation. volatile so the result isn't optimised away as unused, which is exactly
                // what a release build does to a discarded `new`.
                volatile void* p = ::operator new(static_cast<size_t>(-1) / 2);
                (void)p;
            } else if (crashTest == "throw") {
                // Kind::Terminate, though the mechanism is not std::terminate: an escaping C++
                // exception on Windows raises SEH 0xE06D7363, taken first by the unhandled filter --
                // surprising enough to be worth a test rather than a comment.
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
    // SINGLE-INSTANCE FORWARDING, RECEIVER-SIDE GATE: a superset of the sender gate above (which also
    // requires project/openMap), so a bare .ocbeam can become a primary though excluded from forwarding
    // -- intentional, since this process still deserves to be what a later .ocmap/.ocproject double-
    // click reaches. Reading only argc/argv[1][0] keeps the two gates in lockstep automatically.
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
    // THE BACKEND, FROM THE PROJECT, READ BEFORE THE DEVICE EXISTS: unlike every other RENDER.* key
    // (reapplied per frame), the device is created during Engine::run before project_ is populated, so
    // the manifest is peeked here, once. THE COMMAND LINE STILL WINS (logged when they disagree): a
    // manifest silently outranking a render flag has corrupted measurements in this repo before.
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
    // The start screen: interactive launches with no project, or --start-screen. Never in a capture run.
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
    // Three states: >0 is an explicit delay, <0 is --no-chunk-stream, 0 is "flag never given" and now
    // leaves the member's own default alone rather than meaning off, as it used to (see
    // chunkStreamAutoFrames_'s declaration, default on).
    if (chunkStream > 0)      app->setChunkStreamAuto(chunkStream);
    else if (chunkStream < 0) app->setChunkStreamAuto(0);
    if (fogMatch) app->setFogMatchToStreamRadius(true, fogMatchOpacity);
    if (droneAuto > 0) app->setDroneAuto(droneAuto);
    if (undoTestAuto > 0) app->setUndoTestAuto(undoTestAuto);
    if (keybindTestAuto > 0) app->setKeybindTestAuto(keybindTestMode, keybindTestAuto);
#if AVER_MODULE_MCP
    // Precedence: explicit --mcp <port> > mcp.conf's editor_bridge.port > built-in default (45123).
    // mcp.conf only supplies the number; --mcp is still required to open the channel. A missing
    // mcp.conf resolves silently to the default; a malformed one still resolves to it but logs a
    // warning. The resolved port and its source are logged unconditionally, not only on the fallback
    // path.
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
    // --restir-visibility none|reconstructed|half|full: parsed here, same shape as --aversr/
    // --gbuffer-debug, so an unrecognised name gets a clear error rather than silently mapping to 0
    // (No ray).
    if (!restirVisibilityArg.empty()) {
        std::string m = restirVisibilityArg;
        for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (m == "none")               app->setRestirVisibility(0);
        else if (m == "reconstructed") app->setRestirVisibility(1);
        else if (m == "half")          app->setRestirVisibility(2);
        else if (m == "full")          app->setRestirVisibility(3);
        else AVER_ERROR("[Sandbox] --restir-visibility '{}' not recognised "
                        "(none|reconstructed|half|full)", restirVisibilityArg);
    }
    app->setDenoiser(denoiserArg);
    if (reblurAccumArg >= 0) app->setReblurAccum(reblurAccumArg);
    app->setRtForceOff(noRt);
    app->setRayDrivenAblation(rdAblate);
    app->setRtDenoiseMotionTaper(rtDenoiseMotionArg);
    // --lighting-legacy / --pt-legacy-env: seed the raw console slots directly (EditorConsole.hpp),
    // the same slots `set voxi.legacyRestirSampleRing`/etc. and `set pt.legacyEnvironment` write --
    // no SandboxApp member backs these; per-frame reasserts beside voxiRenderer_.setNrdLegacyCamera(...)/
    // syncPtSceneView(...) do the rest, from frame 1. consoleLightingLegacySlot() is guarded (lives in
    // EditorConsole.hpp's AVER_MODULE_VOXI block, feeding VoxiRenderer::setLightingLegacyBits and
    // nothing else); consolePtLegacyEnvSlot() is not, since its path tracer must keep working with
    // AVER_MODULE_VOXI off.
#if AVER_MODULE_VOXI
    editor::consoleLightingLegacySlot() = static_cast<u32>(lightingLegacyArg);
#endif
    editor::consolePtLegacyEnvSlot() = ptLegacyEnvArg;
    // engine-optimisation-plan wave 1 (C-6): seeded the same way (a per-frame reassert beside
    // voxiRenderer_.setLightingLegacyBits(...) in SandboxApp.cpp's onUpdate makes each one live).
    // Guarded on AVER_MODULE_VOXI because the slots live in EditorConsole.hpp's AVER_MODULE_VOXI
    // block (C-5) and feed only VoxiRenderer methods.
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
    // optimisation-wave-2, U1/section 4(a): seeded the same way as the three slots just above -- a
    // per-frame reassert beside voxiRenderer_.setGiVisPathView(...)/setBlendedGiCone(...) (onUpdate)
    // makes each one live for a --frames capture with no console.
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
    // --mesh-heap default|upload (W4): routed through SandboxApp's own member, not a device call, since
    // no rhi::IDevice exists yet in main() -- read at the top of loadProjectMeshes, the first place a
    // device is guaranteed to exist.
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
    // --lod-share-vertices 0|1 (W11): same reasoning as --mesh-heap just above -- staged on the app,
    // consumed inside loadProjectMeshes where a device actually exists.
    if (lodShareVerticesArg >= 0) {
        app->setLodShareVertices(lodShareVerticesArg != 0);
        AVER_INFO("[Sandbox] --lod-share-vertices {}", lodShareVerticesArg != 0 ? "1" : "0");
    }
    app->setRefractionOverrides(refraction, refractionStrength, refractionFade);
    app->setCamWobble(camWobbleDeg, camWobblePeriod);
    if (camWobbleStopArg > 0) app->setCamWobbleStop(camWobbleStopArg);
    if (!consoleSetArgs.empty()) app->setConsoleSets(std::move(consoleSetArgs));
    if (camTranslateArg != 0.0f) app->setCamTranslate(camTranslateArg);
    app->setRenderScale(renderScale);
    if (!aversrArg.empty()) {
#if AVER_MODULE_SR
        // "auto" is explicit CLI Auto (plan 3.3 A), distinct from never passing --aversr (averSrFromCli_
        // still goes true, so loadEditorPreferences leaves the stored Display choice alone -- "the
        // command line wins" everywhere else in this file). No single sr::Quality::Auto for
        // parseQuality, so checked before that call rather than added as a fifth value the enum has
        // no room for.
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
    // NOT app->setXxx(...) -- see setNoWalkCacheArg's forward-declaration comment at the top of this
    // file, and GameRender.hpp's DrawWorldOptions::useMeshLookupCache for what it gates. Called
    // unconditionally; with no --no-walk-cache this changes nothing, since the cache-on default here
    // is both hosts' behaviour today and DrawWorldOptions' own default.
    setNoWalkCacheArg(noWalkCacheArg);
    // --gbuffer-debug MODE: parsed here, same shape as --aversr above, so an unrecognised mode name
    // gets a clear error rather than silently mapping to Off.
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
    // Checked after --occlusion-waitidle so that if both are somehow passed, the one that actually
    // does something today -- opting OUT of the new default -- wins; see occlusionNoWaitIdleArg's own
    // comment above.
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
    // Unconditional, not inside the AVER_MODULE_SR block above (--gpu-timing and its neighbours sit
    // there for unrelated reasons): --luma-sweep has nothing to do with AverSR and must apply whether
    // or not that module is built.
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
