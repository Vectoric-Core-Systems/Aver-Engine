// Entry point: createApplication and the command line it parses.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "aver/runtime/EntryPoint.hpp"
#include "SandboxApp.hpp"

namespace aver {


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

// True for a path this editor will OFFER to open as a level. BOTH spellings, since a double-click on
// either means the same thing.
// EXTENSION ONLY DECIDES WHETHER TO TRY, NOT WHAT THE FILE ACTUALLY IS: ElectricDreams and FirstPerson
// both ship an .ocmap starting with `OCMAP 1` that is nonetheless pure OCWORLD content, while
// OpenConstructor's demoworld.ocmap -- same header, same extension -- genuinely needs the legacy
// grammar. loadLevel tells those apart by which records the file contains
// (fmt::levelFileIsLegacyOcmap), not by extension or header.
bool isLevelFile(const char* p) {
    return hasExtension(p, ".ocmap") || hasExtension(p, ".ocworld");
}

// The .ocproject that owns `mapPath`, found by walking up from it, or empty if there is none.
// WHY WALKING UP: a level is not self-contained -- every mesh/material/class it places is named
// RELATIVE to its project's content directory, built when a project opens. Opening a bare .ocmap with
// no project would resolve none of them, reading as a corrupt level rather than a missing project.
// Bounded to eight levels, far past any real Content/Maps/... nesting, so a stray level can't walk to
// the drive root looking for one.
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
    // --mcp was given at all, vs. given WITH an explicit port. Precedence (CLI > mcp.conf > built-in
    // default) needs to tell those apart: a numeric port resolves right here and nothing may override
    // it; --mcp with no number defers to mcp.conf, resolved once engineRoot() can be asked.
    bool mcpRequested = false, mcpPortExplicit = false;
    // --rd-ablate N, PARSED IN ITS OWN LOOP RATHER THAN THE CHAIN BELOW. When it was added the else-if
    // chain that handles every other flag was AT MSVC's block-nesting limit (C1061), so this flag and
    // every one after it went into a loop of its own; the C1061 notes on those loops record that. The
    // chain was split into chunks on 2026-09-16 (see THE FLAG CHAIN, IN CHUNKS below), so a new flag
    // can go there again. These loops stay as they are: several read a value without consuming it,
    // and moving one into the chain would change which argument the bare-path fallback sees.
    // Removes ONE term from the ray-driven pixel shader so its cost can be attributed by difference. Every non-zero value renders a deliberately WRONG frame.
    int rdAblate = 0;
    // --gi-mode N: same C1061 reason as every other flag in this loop -- the else-if chain below is
    // already at MSVC's nesting limit. -1 is "not given" (0 is the real value "voxel cones"), so an
    // A/B against a manifest that already picks an estimator (RENDER.GIMODE) can be overridden at all.
    int giModeArg = -1;
    int denoiserArg = -1;   // --denoiser 0|1
    int reblurAccumArg = -1;   // --reblur-accum N
    // REFRACTION: the tier picks a mode, these override it. -1 is "not given", the sentinel every
    // other render override here uses, since `take()` tests for exactly that -- a 0-means-absent
    // sentinel would make `--refraction 0` (OFF) silently undiscardable.
    int refraction = -1;
    f32 refractionStrength = -1.0f, refractionFade = -1.0f;
    // --resize-cycle N: resize the real window every N frames of a bounded run. Here for the same
    // C1061 reason as everything else in this loop. Verification-only, and it reproduces a real
    // crash -- see SandboxApp::resizeCheck() for which one and why nothing else could.
    int resizeCycleArg = 0;
    int pieCamArg = 0;
    // --input-stuck-test N: in THIS loop and not the else-if chain below, for the same C1061 reason
    // as every other flag here -- that chain is at MSVC's nesting limit and one more `else if` is a
    // build failure, not a warning.
    int inputStuckArg = 0;
    int inputSourceArg = 0;
    int wheelSpeedArg = 0;
    int multiSelArg = 0;
    const char* cbMoveArg = nullptr;
    int saveDirtyArg = 0;
    int prefsWriteArg = 0;
    // --notify-test N: raise one notification of each severity N frames in, AND lift the capture
    // suppression so the stack is actually drawn in a bounded run. Without the lift there would be
    // no way to screenshot the one piece of this feature a headless test cannot judge.
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
    // --gpu-timing takes no value, so it's matched in the i+1<argc loop below only incidentally --
    // checked in its own full-length loop too, or a trailing --gpu-timing would be silently ignored --
    // the exact shape of the --no-rt/--no-gi bug this file already paid for once.
    bool gpuTimingArg = false;
    f32 rtDenoiseMotionArg = 0.0f;   // --rt-denoise-motion, 0 = the shipped default (no taper)
    // --lighting-legacy <bits>: seeds editor::consoleLightingLegacySlot() directly (see where this
    // is applied, beside the other console-slot seeds after `app` exists, further down) rather than
    // routing through a Settings field -- there is no console to type `set voxi.legacyRestirSampleRing
    // true` into on a --frames capture. 0 (the default, flag absent) means every lighting-contrast
    // fix stays live. IN THIS LOOP for the usual C1061 reason: the else-if chain below is already at
    // MSVC's nesting limit and one more branch does not compile.
    int lightingLegacyArg = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--rd-ablate"))            rdAblate = std::atoi(argv[i + 1]);
        // --lighting-legacy N: same C1061 reason as --rd-ablate above.
        if (!std::strcmp(argv[i], "--lighting-legacy"))      lightingLegacyArg = std::atoi(argv[i + 1]);
        // --gi-mode N: selects the indirect-diffuse estimator (0 = voxel cones, 1 = RTXDI ReSTIR GI).
        // In THIS loop for the same C1061 reason as --rd-ablate above.
        if (!std::strcmp(argv[i], "--gi-mode"))              giModeArg = std::atoi(argv[i + 1]);
        // --denoiser 0|1: NVIDIA NRD over the ReSTIR GI and the sky occlusion. In THIS loop for the
        // same C1061 reason, and it exists at all because the pass needs the G-buffer -- which for
        // most of this pass's life meant it was reachable ONLY by also passing --gbuffer by hand.
        if (!std::strcmp(argv[i], "--denoiser"))              denoiserArg = std::atoi(argv[i + 1]);
        // --reblur-accum N: REBLUR_DIFFUSE's history depth, the console's voxi.reblurMaxAccumulatedFrameNum.
        if (!std::strcmp(argv[i], "--reblur-accum"))          reblurAccumArg = std::atoi(argv[i + 1]);
        // --rt-denoise-motion F: see VoxiRenderer::setRtDenoiseMotionTaper. In THIS loop rather than
        // the chain below for the reason stated at the top of it -- that chain is at MSVC's nesting
        // limit and one more else-if there is a hard compile error.
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
    // --unlit: the Unlit view mode from the command line, so a bounded run can PROVE it shades
    // differently rather than a human having to pick it from a dropdown. Takes no value, so it
    // gets its own full-length loop like --gpu-timing above -- matched only in the i+1<argc loop,
    // a trailing --unlit would be silently ignored.
    bool unlitArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--unlit")) unlitArg = true;
    // --pt-legacy-env: seeds editor::consolePtLegacyEnvSlot() directly, for the identical
    // --frames-has-no-console reason --lighting-legacy above does -- see where this is applied,
    // beside that seed, further down. Takes no value, so its own full-length loop like --unlit just
    // above: matched only in the i+1<argc loop, a trailing "--pt-legacy-env" would be silently
    // ignored.
    bool ptLegacyEnvArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--pt-legacy-env")) ptLegacyEnvArg = true;
    // --clear-shader-cache: a maintenance action, valueless, so it gets its own full-length loop
    // for --gpu-timing's reason -- matched only in the i+1<argc loop, a trailing one would be
    // silently ignored, which for a "did it clear?" command is the worst possible failure.
    // --luma-sweep [STRIDE]: its own full-length loop for the same reason as --gpu-timing/--unlit
    // above, EXTENDED to accept an optional numeric STRIDE the way --clear-shader-cache's directory
    // arg does just below -- consumed only when present and not itself another flag, so a trailing
    // "--luma-sweep --frames 300" is not misread as stride "--frames". STRIDE is frames between
    // logged samples (default 1, every frame): the MCP aver_run tool that is this repo's preferred
    // way to run headlessly caps its returned grep match list at 60 lines (aver_mcp.py's
    // `matched[:60]`), so an unstrided sweep over a several-hundred-frame run would silently lose
    // every sample past the 60th with no error -- stride lets a long run stay under that cap instead
    // of the caller having to discover the truncation by counting. See lumaSweepCheck() for what it
    // measures.
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
    // --firefly-metric [MULT]: its own full-length loop for the same C1061 reason as --luma-sweep
    // just above, with the same optional-numeric-argument handling (MULT, the outlier threshold
    // multiplier over the local neighbourhood -- default 8.0, see lumaSweepCheck()). Shares
    // --luma-sweep's own STRIDE (lumaSweepStride_) rather than adding a second one: both flags drive
    // the SAME per-frame readback cycle, so one cadence knob for it is enough, and combining
    // "--luma-sweep N --firefly-metric" already gives independent control of it when wanted.
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
            // The path is OPTIONAL, and is only consumed when it does not itself look like a flag --
            // otherwise "--clear-shader-cache --frames 4" would silently take "--frames" as the
            // directory to clear and then find nothing there, reporting a clean success.
            if (i + 1 < argc && argv[i + 1][0] != '-') clearShaderCacheDirArg = argv[i + 1];
        }
    std::string saveLevelArg;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--save-level")) saveLevelArg = argv[i + 1];
    // PARSED IN ITS OWN LOOP, like --save-level above: the main else-if chain is at MSVC's
    // C1061 nesting limit and one more branch does not compile.
    bool rayProbeArg = false; f32 rayProbeXArg = 0.0f, rayProbeYArg = 0.0f;
    for (int i = 1; i + 2 < argc; ++i)
        if (!std::strcmp(argv[i], "--ray-probe")) {
            rayProbeArg = true;
            rayProbeXArg = static_cast<f32>(std::atof(argv[i + 1]));
            rayProbeYArg = static_cast<f32>(std::atof(argv[i + 2]));
        }
    // --cam-translate SPEED: see SandboxApp::setCamTranslate's own comment -- TRANSLATION through
    // the scene, not the rotation --cam-wobble gives. PARSED IN ITS OWN LOOP, like --ray-probe just
    // above: the main else-if chain is at MSVC's C1061 nesting limit and one more branch does not compile.
    f32 camTranslateArg = 0.0f;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--cam-translate")) camTranslateArg = (f32)std::atof(argv[i + 1]);
    // --no-occlusion-cull: see SandboxApp::setOcclusionCullForceOff's own comment -- a level whose
    // own manifest records OCCLUSIONCULL 1 (the repro project this exists for) turns culling back on
    // during project/level load no matter what a one-shot CLI setter did at construction, so this
    // needs to be a distinct flag applied every frame rather than reusing --occlusion-cull's false
    // case. Takes no value, so its own FULL-LENGTH loop like --unlit/--gpu-timing above -- matched
    // only in the i+1<argc loop, a trailing "--no-occlusion-cull" would be silently ignored.
    bool noOcclusionCullArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-occlusion-cull")) noOcclusionCullArg = true;
    // --occlusion-waitidle / --no-occlusion-waitidle: see SandboxApp::setOcclusionDebugForceWaitIdle's
    // own comment -- forces (or releases) OcclusionCuller::testBatch()'s res.waitIdle(). The member
    // this seeds now DEFAULTS true, so --occlusion-waitidle is redundant with a fresh build but kept
    // for explicitness/scripts; --no-occlusion-waitidle is the flag that actually changes behaviour
    // today, opting into the faster, not-yet-proven-correct no-wait path. Both take no value, so both
    // get their own FULL-LENGTH loop, same reasoning as --no-occlusion-cull above.
    bool occlusionWaitIdleArg = false, occlusionNoWaitIdleArg = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--occlusion-waitidle")) occlusionWaitIdleArg = true;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-occlusion-waitidle")) occlusionNoWaitIdleArg = true;
    // --sun-set-at N ELEV AZIM / --gi-history-reset-at N: verification-only, see sunSetAtFrames_. Their
    // own loops rather than the long else-if chain further down, which is at the compiler's nesting limit.
    int sunSetAtArg = 0, giHistoryResetAtArg = 0;
    f32 sunSetElevArg = 0.0f, sunSetAzimArg = 0.0f;
    for (int i = 1; i + 3 < argc; ++i)
        if (!std::strcmp(argv[i], "--sun-set-at")) {
            sunSetAtArg   = std::atoi(argv[i + 1]);
            sunSetElevArg = static_cast<f32>(std::atof(argv[i + 2]));
            sunSetAzimArg = static_cast<f32>(std::atof(argv[i + 3]));
        }
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--gi-history-reset-at")) giHistoryResetAtArg = std::atoi(argv[i + 1]);

    u64 frames=0; bool headless=false, focusVoxi=false, focusScript=false, focusTools=false, focusCompileMenu=false, focusCompile=false, startScreen=false; int drawerOpen=0; std::string drawerSub; std::string beam, shot, project, scriptsDir, spawnTest; std::string shaderSourceDir; bool playTest=false; bool skinTest=false; bool skinDrawTest=false; bool particleTest=false; bool noParticleGi=false; int particleStressEmitters=0; int particleStressMaxParticles=0; bool particleStressSecondEmitter=false; bool reflTest=false; bool furnaceTest=false; bool furnaceSun=false; bool furnaceGrid=false; f32 furnaceTilt=0.0f; bool ptFurnace=false; bool ptScene=false; int deviceLostAt=0; int ptQualityRamp=0; int ptSceneToggleOn=0; int ptSceneToggleOff=0; int aversrCycle=0; int projectSettingsPage=-1; f32 sunAngle=-1.0f; std::string skinSceneDir; Tool tool=Tool::Select; int msaa=0; int gi=-1; int rt=-1; int rtRays=0; int rtPixelsPerRay=0; int rtShadowDenoise=-1; int rtRenderMode=-1; int pt=-1; int ptBounces=-1; int layeredBsdf=-1; f32 coatWeight=0.0f; f32 coatRough=0.1f; f32 coatF0=0.04f; int giUpdateInterval=0; f32 renderScale=1.0f; std::string aversrArg; bool frameTime=false; bool noGi=false; bool noRt=false; bool giConeOff=false; f32 camWobbleDeg=0.0f; int camWobblePeriod=0; bool giDbg=false, ms=false; u32 probeX=0, probeY=0; f32 probeU=-1.0f, probeV=-1.0f; bool camSet=false; f32 camX=0, camY=0, camZ=0, camPitch=0, camYaw=0; int reloadAt=0; bool warp=false, debugLayer=false; std::string backendName; const char* forceCaps=nullptr; f32 bloom=0.0f, exposure=1.0f; bool bloomSet=false, exposureSet=false; bool autoExposure=false; int clouds=0; f32 cloudCover=-1.0f; bool skyPhysical=false, skyAuthored=false; f32 skyElevation=-999.0f; bool vsyncOff=false; bool uiDemo=false; bool inputProbe=false; bool autoCompile=false; bool showPrefs=false; bool scrollPrefsToKeybinds=false; bool saveProject=false; std::string importSrc, importDst; int focusLevelAt=0; int hudTest=-1; std::string openAsset; std::string selectEntity; bool openLegacy=false; bool waterOn=false; f32 waterHeight=0.0f; std::string graphSelectNode; std::string graphTab; int chunkStream=0; int droneAuto=0; int undoTestAuto=0; int keybindTestAuto=0; std::string keybindTestMode; std::string droneGraph; std::string landscapePath; bool fogMatch=false; f32 fogMatchOpacity=-1.0f; bool lodSelect=true; f32 lodErrorPx=1.0f; bool lodClusterStats=false; bool lodPerCluster=false; int lodMeshShader=-1; bool depthPrepass=false; bool edgeAa=false; bool occlusionCull=false; bool bakeNav=false; f32 bakeNavCell=50.0f; std::string openMap; bool gbuffer=false; std::string gbufferDebug; std::string crashTest; std::string startMode;
    bool openLevelPickerArg=false; std::string openLevelArg; bool noEditorChrome=false; bool sceneCensus=false;
    // -1 IS ABSENT, NOT 0. Zero is a MEANING for this knob -- "estimate ambient occlusion from the
    // cone gather", which is what every tier below Epic does -- so the usual 0-means-absent sentinel
    // could not express turning the rays off. Same reasoning --rt-shadow-denoise already documents.
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
    // engine-optimisation-plan, wave 1 (C-6). -1 IS ABSENT for the three GI dials, same sentinel and
    // reasoning as giSkyOccRays/giSkyOccTile above: each is a plain on/off, but 0 is a REAL value (the
    // measurement forced OFF explicitly), so a 0-default could not tell "not given" apart from "given
    // as 0". meshHeapArg/lodShareVerticesArg use their own sentinels (empty string, -1) for the same
    // reason -- see their own application sites below for what each actually does.
    int giForceRebuildArg = -1;
    int giBoundedDispatchArg = -1;
    int giFreeAccumulatorArg = -1;
    std::string meshHeapArg;   // empty = absent; "default" or "upload" otherwise
    int lodShareVerticesArg = -1;
    // optimisation-wave-2, U1/section 4(a): --restir-visibility/--gi-vis-path-view/--blended-gi.
    // Strings/bools, not ints -- parsed and applied after the loop, the same "stored as a string here
    // so a build with the module compiled out can still recognise the flag" shape --aversr already
    // uses (aversrArg's own comment).
    std::string restirVisibilityArg;   // empty = absent; none|reconstructed|half|full otherwise
    bool giVisPathViewArg = false;     // --gi-vis-path-view
    std::string blendedGiArg;          // empty = absent; restir|cone otherwise
    for (int i=1;i<argc;++i){
        // HANDLED BEFORE THE else-if CHAIN BELOW, AND NOT BY PREFERENCE: one more `else if` there
        // hits MSVC's nesting limit (C1061). Anything added from here on wants this shape instead:
        // match, consume, `continue`.
        // --open-level-picker: open File > Open Level's modal on the first frame, so a --frames run
        // can screenshot it. In THIS loop for the C1061 reason above, like every flag added since.
        if (!std::strcmp(argv[i],"--open-level-picker")) { openLevelPickerArg=true; continue; }
        // --gi-force-rebuild / --gi-bounded-dispatch / --gi-free-accumulator / --mesh-heap /
        // --lod-share-vertices: engine-optimisation-plan wave 1's measurement and opt-in dials
        // (M1-M4/W3/W4/W11/W12). NOT in the `i + 1 < argc` pre-chain loop above this one (the one
        // that seeds beam/shot/etc.): a value stored there is never CONSUMED, so it falls through to
        // the bare-path branch at the bottom of the main argv loop (`argv[i][0]!='-'`) and gets
        // stored as `beam` instead of being read as this flag's argument -- exactly the failure mode
        // that branch's own comment warns about. In THIS loop for the C1061 reason above, like every
        // flag added since, and as a plain `if`/`continue`, never an `else if` (the chain below this
        // loop is already at MSVC's nesting limit).
        if (!std::strcmp(argv[i],"--gi-force-rebuild") && i+1<argc) {
            giForceRebuildArg = std::atoi(argv[++i]); continue;
        }
        if (!std::strcmp(argv[i],"--gi-bounded-dispatch") && i+1<argc) {
            giBoundedDispatchArg = std::atoi(argv[++i]); continue;
        }
        if (!std::strcmp(argv[i],"--gi-free-accumulator") && i+1<argc) {
            giFreeAccumulatorArg = std::atoi(argv[++i]); continue;
        }
        // --restir-visibility none|reconstructed|half|full: optimisation-wave-2's U1
        // (Settings::giRestirVisibility). In THIS block for the C1061 reason at its top, like every
        // flag added since -- parsed and applied after the loop, beside app->setGiMode(giModeArg).
        if (!std::strcmp(argv[i],"--restir-visibility") && i+1<argc) {
            restirVisibilityArg = argv[++i]; continue;
        }
        // --gi-vis-path-view: U1's path-debug view (2.10 I) -- see consoleGiVisPathViewSlot()'s own
        // comment (EditorConsole.hpp). Takes no value, so it needs no i+1<argc guard.
        if (!std::strcmp(argv[i],"--gi-vis-path-view")) { giVisPathViewArg = true; continue; }
        // --blended-gi restir|cone: section 4(a)'s W6/M5 pricing switch -- see
        // consoleBlendedGiConeSlot()'s own comment (EditorConsole.hpp).
        if (!std::strcmp(argv[i],"--blended-gi") && i+1<argc) { blendedGiArg = argv[++i]; continue; }
        // --mesh-heap default|upload: W4. A string, not a 0/1 int, so an unrecognised spelling can be
        // reported by name at the application site below rather than silently misread as a number.
        if (!std::strcmp(argv[i],"--mesh-heap") && i+1<argc) {
            meshHeapArg = argv[++i]; continue;
        }
        // --lod-share-vertices 0|1: W11.
        if (!std::strcmp(argv[i],"--lod-share-vertices") && i+1<argc) {
            lodShareVerticesArg = std::atoi(argv[++i]); continue;
        }
        // --gi-sky-occlusion-rays N: how many sky-visibility rays the AMBIENT term traces per pixel.
        //
        // IT EXISTED AS A SETTING WITH NO WAY TO SET IT. Settings::giSkyOcclusionRays shipped
        // tier-derived and nothing else -- no flag, no manifest key -- so nobody could sweep the one
        // dial the shader itself says is the lever for this ray. rtSkyOcclusion's own comment
        // (voxi.hlsl) spells out why it matters and why it is NOT the same question as --rt-rays:
        // the sun rays are coherent, all pointing one way and walking the same BVH nodes, so three
        // more cost 0.05 ms; these are cosine-distributed over the hemisphere, so every lane in a
        // wave descends a different part of the tree and ONE of them costs 5.37 ms. "Ray count is
        // nearly free" is a measured property of the coherent ray and does not transfer here -- and
        // the only way to find out where it does land was a knob that did not exist.
        //
        // In THIS loop for the C1061 reason above, like every flag added since.
        if (!std::strcmp(argv[i],"--gi-sky-occlusion-rays") && i+1<argc) {
            giSkyOccRays = std::atoi(argv[++i]); continue;
        }
        // --gi-sky-occlusion-tile N: pixels per shared sky-occlusion ray direction. The lever the ray
        // itself names for its own cost, and it was a compile-time #define set nowhere until now --
        // so the trade it governs (cheaper rays against correlated noise) had never been measured.
        if (!std::strcmp(argv[i],"--gi-sky-occlusion-tile") && i+1<argc) {
            giSkyOccTile = std::atoi(argv[++i]); continue;
        }
        // --sky-light N: scales the sky ambient (SkyAtmosphere::skyLightIntensity, which is the
        // editor's "Sky Light" slider). 1 is the default; 0 removes the term entirely.
        //
        // IT HAD A SLIDER AND NO FLAG, so the one term added to every surface without real occlusion
        // -- diffAmbient, attenuated by a six-cone AO and nothing else -- could not be swept from a
        // script, and its share of the image had therefore never been measured. That share is the
        // open question behind a washed-out picture: the sky is Rayleigh-blue, a bounce off stone is
        // warm, and a large unoccluded ambient pulls the result toward grey.
        // In THIS loop for the C1061 reason above, like every flag added since.
        if (!std::strcmp(argv[i],"--sky-light") && i+1<argc) {
            skyLightArg = (f32)std::atof(argv[++i]); continue;
        }
        // --gi-intensity F: multiplier on the cone-traced bounce. 1 is the default; 0 is direct
        // light only. The editor has a slider and the manifest has RENDER.GIINTENSITY; this is the
        // missing third way, and the one a measurement script can use.
        // In THIS loop for the C1061 reason above, like every flag added since.
        if (!std::strcmp(argv[i],"--gi-intensity") && i+1<argc) {
            giIntensityArg = (f32)std::atof(argv[++i]); continue;
        }
        // --tonemap 0|1: 0 the original per-channel ACES approximation (what every recorded gate
        // baseline was measured through), 1 the matrixed fit that keeps saturation. See
        // PostSettings::tonemap.
        if (!std::strcmp(argv[i],"--tonemap") && i+1<argc) { tonemapArg = std::atoi(argv[++i]); continue; }
        // --windowed: the DEFAULT now, kept so scripts and habits that pass it still work rather
        // than erroring on an unknown flag. --fullscreen is the one that changes anything.
        if (!std::strcmp(argv[i],"--windowed")) { windowedArg = true; continue; }
        // --fullscreen: borderless fullscreen on an interactive run, sized to the monitor. Off by
        // default because an editor sits BESIDE other windows -- covering the taskbar and whatever is
        // behind it is a game's behaviour, not a tool's. A capture run stays windowed regardless, so
        // this cannot reshape the client area a recorded gate probe was measured in.
        if (!std::strcmp(argv[i],"--fullscreen")) { fullscreenArg = true; continue; }
        // --max-radiance F: ceiling on scene radiance just before the tonemap; 0 disables it. The sun
        // disc is the thing this exists for -- see PostSettings::maxRadiance.
        if (!std::strcmp(argv[i],"--max-radiance") && i+1<argc) {
            maxRadianceArg = static_cast<f32>(std::atof(argv[++i])); continue;
        }
        // --no-editor-chrome: draw the SCENE and nothing the editor adds on top of it -- no grid, no
        // navmesh overlay, no gizmo, no sculpt cursor, no viewport icons.
        //
        // IT EXISTS SO TWO HOSTS CAN BE COMPARED. scripts/verify-game.ps1 opens the same project and
        // level in Sandbox.exe and AverEngineRuntime.exe and diffs their probe codes, which is the check whose
        // absence got the packaged game deleted in the first place ("a second host rendered a
        // different subset of the scene"). Without this the diff would be dominated by the grid the
        // game correctly does not draw, and would prove nothing about the scene.
        //
        // NOT A VIEW PREFERENCE and deliberately not routed through showGrid_: that one persists to
        // editor.ini, and a comparison run must not change what the next interactive session looks
        // like. This is a run-scoped override the draw sites read alongside their own flags.
        if (!std::strcmp(argv[i],"--no-editor-chrome")) { noEditorChrome=true; continue; }
        // --scene-census: print one canonical line describing what the loaded level put in
        // the world. AverEngineRuntime.exe accepts the identical flag and prints the identical format,
        // and scripts/verify-game.ps1 compares the two -- the divergence check whose absence
        // is why the packaged game was deleted. See world/SceneCensus.hpp for why a census
        // rather than a frame comparison.
        if (!std::strcmp(argv[i],"--scene-census")) { sceneCensus=true; continue; }
        // --open-level <name-or-relative-path>: open a level of this project BY NAME, through the
        // same requestOpenLevel funnel the picker and the Content Browser use. Two jobs: it is the
        // convenient way to start on a level that is not the start map, and it is the only way a
        // bounded --frames run can exercise that funnel at all -- the picker itself needs a click.
        if (!std::strcmp(argv[i],"--open-level") && i+1<argc) { openLevelArg=argv[++i]; continue; }
        // --layered-bsdf N: 0=Off 1=Low 2=Medium 3=High 4=Epic. Off is the standard BRDF, unchanged.
        // ON ITS OWN THIS FLAG CHANGES NOTHING VISIBLE, correctly: it selects a shader variant that
        // can evaluate a coat, and every existing project authors coatWeight 0. Pair with --coat below.
        if (!std::strcmp(argv[i],"--layered-bsdf") && i+1<argc) { layeredBsdf=std::atoi(argv[++i]); continue; }

        // --coat <weight> [roughness] [f0]: give the EDITOR'S OWN placeholder materials a coat.
        // WHY A FLAG RATHER THAN AN AUTHORED ASSET: the two things worth measuring are both scenes the
        // editor builds in C++ (the default Floor/Cube, the --furnace-grid plates), not loaded from
        // disk. Authoring a .ocmat would test the parser, which MaterialTest already does, not the shading.
        if (!std::strcmp(argv[i],"--coat") && i+1<argc) {
            coatWeight = (f32)std::atof(argv[++i]);
            if (i+1<argc && argv[i+1][0] != '-') coatRough = (f32)std::atof(argv[++i]);
            if (i+1<argc && argv[i+1][0] != '-') coatF0    = (f32)std::atof(argv[++i]);
            continue;
        }

        // --new-project <location> <name> scaffolds a project and exits, touching no device.
        // THE FLAG CHAIN, IN CHUNKS OF ~30. It was one else-if chain of 119 branches, which is
        // MSVC's block-nesting limit: one more branch failed the build with C1061 at an unrelated line, and
        // every flag added for months went into a separate loop above instead. Each chunk is still an
        // else-if chain that runs its FIRST matching branch; its final `else` records that nothing in it
        // matched, and only then does the next chunk run -- so exactly one branch runs, as before, in the
        // same order. A new flag can go at the end of the last chunk; start a new chunk the same way if
        // that one grows past ~30.
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
        // --import-gltf <src.gltf|.glb> <destDir> imports a model and exits, touching no device --
        // the exact --new-project precedent, for the Content Browser's Import button. importGltfToDir
        // is the SAME function the button's click handler runs through SandboxApp::importModel; this
        // is the other caller. destDir is created if missing, so this also works as the first write into a brand new project.
        else if (!std::strcmp(argv[i],"--import-gltf") && i+2<argc) {
            const std::string src = argv[++i], destDir = argv[++i];
            // THE OWNING PROJECT'S CONTENT DIR, when destDir sits inside one -- a textured import
            // needs it to cook the file's materials and textures (importGltfToDir's own call to
            // fmt::cookImportedMaterials). ownerProjectOf walks UP from destDir looking for the
            // .ocproject beside it, exactly as it does for a level path; destDir need not itself be
            // the Content root, only somewhere under a project. A destDir with no owning project --
            // the launcher-less / scratch-directory case this flag has always supported -- gets
            // geometry only, same as before this existed.
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
        // --upgrade-project <path.ocproject> applies what the prompt applies, and exits.
        // BOTH HALVES, which the flag used to promise and not deliver: inspectProject/
        // applyProjectUpgrade repairs SCAFFOLD gaps (a missing folder, a stale .csproj reference,
        // version-independent), while migrateProject runs the VERSION chain and stamps the manifest.
        // A project can need either, both or neither; running only the first left a 0.2 project reporting "already current" while still stamped 0.2.
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
            // The version chain runs whether or not the scaffold needed anything: they answer
            // different questions. A failure here is reported and NOT stamped, so the project is
            // asked again next time rather than being recorded as current.
            if (!editor::migrateProject(manifest, &why)) {
                AVER_ERROR("[Sandbox] version migration failed: {}", why);
                std::exit(1);
            }
            AVER_INFO("[Sandbox] '{}' upgraded and stamped {}", p.name, kEngineVersion);
            std::exit(0);
        }
        // --landscape-gen <path> [sampleCount] [spacingCm] writes a synthetic rolling-hill .ocland
        // through the real writeOcLand/loadOcLand round trip and exits, touching no device -- exists
        // because no .ocland fixture exists anywhere else, and a hand-rolled binary fixture would
        // prove nothing about the real format. sampleCount must be (k*64)+1; default 257 is three LOD levels, small enough to build in a --frames run.
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
                    // Two overlapping sine fields, index-frequency (not world-frequency): rolling
                    // hills with real relief at any spacing, so --frames LOD selection has something
                    // to refine against, not a plane a first-time reader could mistake for a bug.
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
        // stroke through the EXACT functions the editor's own sculpt tools call
        // (landscape::applyBrush, fmt::saveOcLand), centred on the section's own middle, then exits
        // touching no device -- "a sculpt changes the stored heights" needs a real round trip, not a claim about code nobody ran.
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
            // Eight ticks at amount 0.25, like ~130ms of a held mouse button at the dt*6 rate
            // handleSculpt uses -- not one amount=1 jump, so the result actually depends on
            // strength/radius rather than degenerating to "did anything change at all".
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
        // --water <heightCm> puts a Gerstner surface and a buoyancy plane at that height. OPT-IN
        // rather than a default, because a water plane is an infinite sheet and turning it on for
        // every level ever opened would be a surprise, not a feature.
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
        // --mcp [port] opens the editor control channel. Opt-in: it is a listening socket. With no
        // number, the port comes from mcp.conf's editor_bridge.port, or 45123 if that is absent too
        // -- resolved after this loop, once mcpRequested/mcpPortExplicit are both known for good.
        else if (!std::strcmp(argv[i],"--mcp")) {
            mcpRequested = true;
            if (i+1 < argc && argv[i+1][0] != '-') {
                // RANGE-CHECKED, unlike the bare `(u16)std::atoi(...)` this replaces: that cast
                // silently truncated -- `--mcp 99999` bound port 34463 (mod 65536) and logged it as
                // though typed, and `--mcp 0` counted as explicit yet never opened a channel, since 0
                // is also this parser's "never asked for" value. A port that is not a port should say so, not quietly become a different one.
                const char* raw = argv[++i];
                const long  p   = std::strtol(raw, nullptr, 10);
                if (p >= 1 && p <= 65535) {
                    mcpPort = (u16)p;
                    mcpPortExplicit = true;
                } else {
                    // Not fatal, and deliberately not: the channel is a debugging aid, and refusing
                    // to start the whole editor over a mistyped port would be worse than falling
                    // back the way every other unset value here does.
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
        // --chunk-stream [N] switches chunk streaming on N frames in (default 5), the same "wait for
        // the project/scene to settle" pattern --reload-scripts uses, so a --frames capture can prove
        // streaming happened without a human clicking the menu.
        // --drone-graph <path> names the .ocgraph the drone runs. Without it the drone spawns and sits still -- honest for an engine that doesn't know any project's scripts.
        if (!std::strcmp(argv[i],"--drone-graph") && i+1<argc) droneGraph = argv[++i];
        // --landscape <path.ocland> overrides the level's LANDSCAPE record and the levelname.ocland
        // convention (GameLandscape::setPathOverride). Exists so a --frames capture can prove the LOD-selection path draws real
        // terrain without a level file naming one and without a human clicking anything.
        else if (!std::strcmp(argv[i],"--landscape") && i+1<argc) landscapePath = argv[++i];
        else if (!std::strcmp(argv[i],"--chunk-stream")) {
            chunkStream = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --no-chunk-stream: the counterpart to streaming now being ON by default. A negative value
        // is the "explicitly off" signal, distinct from the 0 meaning "flag not given" -- without it
        // the default would be unturnoffable, and a frame-time comparison with a still-climbing triangle count is not a comparison.
        else if (!std::strcmp(argv[i],"--no-chunk-stream")) chunkStream = -1;
        // --fog-match [opacity] ties fog density to the streaming radius, the same thing the Height
        // Fog panel's checkbox does -- a flag as well, since the feature is opt-in by design (markedly
        // foggier) and a headless run could never otherwise exercise it.
        else if (!std::strcmp(argv[i],"--fog-match")) {
            fogMatch = true;
            if (i+1 < argc && argv[i+1][0] != '-') fogMatchOpacity = static_cast<f32>(std::atof(argv[++i]));
        }
        // --drone [N] switches the graph-driven drone on N frames in (default 5), same shape and
        // reason as --chunk-stream just above: proves it headlessly without a human clicking the menu.
        else if (!std::strcmp(argv[i],"--drone")) {
            droneAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --undo-test [N]: fires runUndoTest() N frames in (default 10), then EXITS THE PROCESS with
        // 0/1 (see runUndoTest()). Same "wait a few frames to settle" shape as --chunk-stream/--drone,
        // just longer: it needs a live scene::World, and extra startup frames cost nothing here.
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
        // Content subfolder or (console, verification-only) with the console's input box pre-seeded
        // -- see drawConsoleTranscriptTab's own comment on drawerStartSub_ for what console does
        // with it; a screenshot script has no mouse to type with, so this is how it gets the live
        // suggestion popup into shot. FIXED here while adding it: the kind is now compared BEFORE
        // the colon, not against the whole argument -- "--drawer console:gi" used to fail the
        // "console" strcmp outright (v was the whole "console:gi", never equal to "console") and
        // silently fall through to Content, which nobody typing a colon after "console" could have meant.
        else if (!std::strcmp(argv[i],"--drawer") && i+1<argc) {
            const char* v = argv[++i];
            const char* colon = std::strchr(v, ':');
            const std::string kind = colon ? std::string(v, static_cast<size_t>(colon - v)) : std::string(v);
            drawerOpen = kind == "log" ? 2 : kind == "console" ? 3 : 1;
            if (colon) drawerSub = colon + 1;
        }
        else if (!std::strcmp(argv[i],"--msaa") && i+1<argc) msaa=std::atoi(argv[++i]);
        // --gi [tier], --rt [tier], --pt [tier]: quality 0..4 (Off..Epic); bare means High.
        // ALL THREE TAKE AN OPTIONAL TIER AND USE -1 FOR "FLAG ABSENT" -- both were defects until now:
        // --gi/--rt parsed no numeric form (`--gi 2` set High and dropped "2" to the positional
        // handler), and the old 0 sentinel meant --gi 0/--rt 0/--pt 0 could never be expressed,
        // blocking a raster-versus-ray-driven measurement with the path-traced view off.
        // Same shape as --rt-render-mode/--rt-shadow-denoise, already -1.
        else if (!std::strcmp(argv[i],"--gi") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            gi=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--gi")) gi=3;
        else if (!std::strcmp(argv[i],"--no-gi")) noGi=true;
        else if (!std::strcmp(argv[i],"--gi-debug")) { gi=3; giDbg=true; }
        // --no-gi-cone: the A/B measurement toggle -- see VoxiRenderer::setConeTraceEnabled and
        // SandboxApp::setGiConeTraceOff's own comments. Distinct from --no-gi, which also stops the
        // volume from being built; this only stops PSMainVoxi/PSClusterMain from READING it.
        else if (!std::strcmp(argv[i],"--no-gi-cone")) giConeOff=true;
        else if (!std::strcmp(argv[i],"--rt") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            rt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--rt")) rt=3;
        // --pt [tier]: path tracing quality, the switch ptBounces is spent under. `--pt 0` turns the
        // path-traced reference view OFF, which matters because it suppresses the raster scene
        // whenever registered -- any raster-versus-anything comparison on a manifest with RENDER.PATHTRACING set must pass it.
        else if (!std::strcmp(argv[i],"--pt") && i+1<argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
            pt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt")) pt=3;
        // --no-gi / --no-rt: kept, and no longer the ONLY way to reach the Off rung now that the
        // three tier flags take a real 0. They remain the shorter spelling and every existing
        // script and gate config uses them.
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
        // The SPATIAL filter radius in pixels, 0 = off. Distinct from the tile edge above in every
        // way that matters: that one amortises over TIME and lags the camera, this one averages
        // over SPACE and keeps no history at all.
        else if (!std::strcmp(argv[i],"--rt-shadow-denoise") && i+1<argc) rtShadowDenoise=std::atoi(argv[++i]);
        // --rt-render-mode 0|1: which thing finds the first surface -- 0 raster, 1 primary rays. That
        // is RAY tracing: one hit, direct lighting.
        // --pt-bounces N: PATH tracing -- bounces after that first hit, spent only while pathTracing is on (VoxiRenderer enforces this, not assumes it).
        else if (!std::strcmp(argv[i],"--rt-render-mode") && i+1<argc) rtRenderMode=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt-bounces") && i+1<argc) ptBounces=std::atoi(argv[++i]);
        // How many frames apart the GI volume is revoxelised -- 1 (unset) rebuilds every frame, the
        // original always-fresh behaviour. Measures the voxelise+filter amortisation independently of
        // everything else, per the "measure each change, do not stack guesses" rule.
        else if (!std::strcmp(argv[i],"--gi-update-interval") && i+1<argc) giUpdateInterval=std::atoi(argv[++i]);
        // The scene's own render resolution as a fraction of the present/swapchain size -- 1.0
        // (unset) reproduces the pre-existing 1:1 behaviour exactly. Clamped to [0.25,1] by the
        // device; measures the render-scale/GI-cost tradeoff independently of everything else.
        else if (!std::strcmp(argv[i],"--render-scale") && i+1<argc) renderScale=static_cast<f32>(std::atof(argv[++i]));
        // --aversr LEVEL: off|quality|balanced|performance (case-insensitive), the docs/AVERSR.md
        // table. Stored as a string here and parsed/applied below rather than inline, so a build with
        // the module compiled out can still recognise the flag and explain why it did nothing rather than erroring as unknown.
        else if (!std::strcmp(argv[i],"--aversr") && i+1<argc) aversrArg=argv[++i];
        // --depth-prepass: same-frame depth-only pass ahead of the opaque colour walk, so an
        // occluded fragment skips PSMainVoxi's shadow lookup/cone trace/fog entirely. Unset (the
        // default) reproduces pre-existing behaviour exactly -- see setDepthPrepassOverride's comment.
        else if (!std::strcmp(argv[i],"--depth-prepass")) depthPrepass=true;
        else argMatched = false;
        }   // end of flag chunk 2
        if (!argMatched) { argMatched = true;   // flag chunk 3: only if no earlier chunk matched
        // --gbuffer: the thin forward-pass G-buffer (IDevice::setGBufferEnabled) -- velocity, view-
        // space depth, world normal+roughness, written ALONGSIDE the ordinary scene pass. Unset
        // (default) never calls it, so an unmodified run stays bit-identical to a tree that never heard of it.
        if (!std::strcmp(argv[i],"--gbuffer")) gbuffer=true;
        // --gbuffer-debug MODE (velocity|viewz|normals): forces the G-buffer on -- like --gi-debug
        // forces --gi on -- and selects which channel GBufferDebugFeature draws. THIS is the flag that
        // makes `--frames N --probe X Y` able to assert anything about a texture nothing else samples yet.
        else if (!std::strcmp(argv[i],"--gbuffer-debug") && i+1<argc) { gbuffer=true; gbufferDebug=argv[++i]; }
        // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). Unset (the
        // default) reproduces pre-existing behaviour exactly, bit for bit -- see
        // setOcclusionCullOverride's own comment, same "off is today's frame back, byte for byte"
        // contract --depth-prepass and --edge-aa both already carry.
        else if (!std::strcmp(argv[i],"--occlusion-cull")) occlusionCull=true;
        // --edge-aa: FxaaResolve through the SAME rhi::IUpscaler seam --aversr uses -- see
        // edgeAaEnabled_'s own comment for how the two share one slot. A SETTING, not a hard
        // replacement for MSAA: it runs whatever sample count --msaa already asked for.
        else if (!std::strcmp(argv[i],"--edge-aa")) edgeAa=true;
        else if (!std::strcmp(argv[i],"--frame-time")) frameTime=true;
        else if (!std::strcmp(argv[i],"--ms")) ms=true;
        else if (!std::strcmp(argv[i],"--probe") && i+2<argc) { probeX=(u32)std::atoi(argv[++i]); probeY=(u32)std::atoi(argv[++i]); }
        // --force-caps clamps what the device reports; it can never raise a capability.
        else if (!std::strcmp(argv[i],"--force-caps") && i+1<argc) forceCaps=argv[++i];
        else if (!std::strcmp(argv[i],"--warp")) warp=true;
        else if (!std::strcmp(argv[i],"--backend") && i+1<argc) backendName=argv[++i];
        else if (!std::strcmp(argv[i],"--debug-layer")) debugLayer=true;
        else if (!std::strcmp(argv[i],"--scripts") && i+1<argc) scriptsDir=argv[++i];
        // --shader-source <dir>: read HLSL from this tree instead of bin/shaders, reloading on
        // change. Applied HERE, during parsing, since the first shaderFile() happens while the
        // renderer builds its pipelines -- setting it later would be read after the fact and do nothing.
        else if (!std::strcmp(argv[i],"--shader-source") && i+1<argc) {
            shaderSourceDir = argv[++i];
            aver::rhi::setShaderSourceDir(shaderSourceDir);
        }
        else if (!std::strcmp(argv[i],"--spawn-test") && i+1<argc) spawnTest=argv[++i];
        else if (!std::strcmp(argv[i],"--play-test")) playTest=true;
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
        // --pt-scene-toggle-on/-off [N]: verification-only, see ptSceneToggleOnAutoFrames_ for what
        // this proves. Same "[N] optional, default given" shape as --chunk-stream.
        // --device-lost-at <N>: SIMULATE the GPU being taken away after N presented frames, exercising the device-lost path without a real GPU disappearing.
        else if (!std::strcmp(argv[i],"--device-lost-at") && i+1<argc) deviceLostAt=std::atoi(argv[++i]);
        else argMatched = false;
        }   // end of flag chunk 3
        if (!argMatched) { argMatched = true;   // flag chunk 4: only if no earlier chunk matched
        // --crash-test <kind>: deliberately kill this process, to prove the crash handler works.
        // A CRASH REPORTER THAT HAS NEVER BEEN SEEN TO FIRE IS NOT A FEATURE, it is a hope: the whole
        // mechanism only runs on the worst possible day to discover a typo in it. UE has the same
        // thing (`debug crash`).
        // Kinds map to distinct paths that do NOT share code: `av` faults the SEH filter, `assert`/
        // `fatal` go through AVER_ASSERT/AVER_FATAL, and `critical` logs and exits cleanly, testing the
        // standby-reporter wake path with no corpse.
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
        // --bake-nav [cellCm]: bake this level's navigation once at startup and write its
        // .ocnav. Deferred to a frame rather than run at init, because the bake samples PHYSICS
        // and a level's bodies are built by applyProject, which has not run yet at construction.
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
        // --no-lod-select: draw every instance at LOD 0, whatever the Cook wrote -- what the editor
        // did by default until the cost was measured (102.7ms median vs 76.7ms with selection on, same
        // camera). Kept only as the escape hatch for telling a selection artefact from a real one.
        else if (!std::strcmp(argv[i],"--no-lod-select")) lodSelect=false;
        // --lod-cluster-stats: turns on the informational per-meshlet frustum/cone-cull counters on
        // top of --lod-select. Separate flag on purpose -- see lodClusterStatsEnabled_'s own comment.
        else if (!std::strcmp(argv[i],"--lod-cluster-stats")) lodClusterStats=true;
        // --lod-per-cluster [px]: PER-CLUSTER virtualized-geometry selection (replaces --lod-select's
        // per-level choice for an instance when both are given). Optional pixel error budget, default
        // 1.0px, same knob --lod-select uses.
        else if (!std::strcmp(argv[i],"--lod-per-cluster")) {
            lodPerCluster=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --lod-mesh-shader [px]: the GPU per-cluster path (amplification+mesh shader). Wins over
        // --lod-per-cluster and --lod-select for an instance whose mesh has GPU cluster data AND this
        // device's pipeline came up; falls back per-instance otherwise. Optional pixel error budget,
        // default 1.0px, same knob the other two share.
        else if (!std::strcmp(argv[i],"--lod-mesh-shader")) {
            lodMeshShader=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --no-lod-mesh-shader forces the GPU per-cluster path OFF. It exists because the path is
        // now ON by default wherever the device supports it (see onInit), so "compare against not
        // having it" needs a way to say so -- which is exactly how its 2.8x speedup was measured.
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
        // --mode <select|landscape|foliage|simulate>: the EDITING MODE to start in, the peer of --tool:
        // automation/screenshots need it since a mode's panel can't be captured through a mouse click.
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
        // A BARE PATH IS WHATEVER ITS EXTENSION SAYS IT IS: this is the shell's entry point, so every
        // type the engine registers itself for has to be recognised here. A .ocmap used to fall
        // through to `beam`, where loadOcbeam refused it and the editor opened empty -- worse than no association at all.
        else if (argv[i][0]!='-') {
            if      (isOcproject(argv[i])) project = argv[i];
            else if (isLevelFile(argv[i])) openMap = argv[i];
            else                           beam    = argv[i];
        }
        }   // end of flag chunk 4
    }

    // SINGLE-INSTANCE FORWARDING, SENDER-SIDE GATE, placed after the bare-arg loop classifies
    // argv[1] so the condition reads `project`/`openMap` exactly as it left them:
    //     argc == 2 && argv[1][0] != '-' && (!project.empty() || !openMap.empty())
    // DELIBERATELY THE NARROWEST THING THAT COULD WORK: this engine's gate-sweep/import/scaffold
    // workflow launches MANY Sandbox.exe processes concurrently, all flag-bearing, so argv[1][0]!='-'
    // excludes them -- but argc==2 is load-bearing too: without it, `Sandbox.exe level.ocmap --frames
    // 600` (argc==4) would satisfy a laxer check and get forwarded to -- or treated as eligible to
    // receive forwards from -- another running instance, making two concurrent captures interfere.
    // (!project.empty() || !openMap.empty()) excludes a bare .ocbeam, which has no file association to
    // honor and nothing here to forward TO.
    if (argc == 2 && argv[1][0] != '-' && (!project.empty() || !openMap.empty())) {
        // Resolved to absolute before it ever leaves this process: the receiving process's working
        // directory has no reason to match this one's, and ownerProjectOf/equivalent() on the far
        // side need a path that names the same file there as it does here.
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(argv[1], ec);
        const std::string forwardPath = ec ? std::string(argv[1]) : abs.string();
        if (Window::forwardToSingleInstancePrimary(forwardPath)) {
            AVER_INFO("[Sandbox] '{}' handed to the running editor instance; this process exits",
                      forwardPath);
            std::exit(0);
        }
        // Every other outcome -- no primary running, one that DECLINED because a different project is
        // open, or a timed-out send -- means the same thing: build this process's own Application/
        // Engine exactly as a bare launch would. Nothing below needs to know a forward was attempted.
    }

    // Before the engine creates a device: the backend queries the hardware inside Engine::run.
    if (forceCaps && !rhi::setCapsOverride(forceCaps))
        AVER_ERROR("[Sandbox] --force-caps '{}' was rejected; running on the UNCLAMPED device", forceCaps);

    // A LEVEL NAMED ON ITS OWN BRINGS ITS PROJECT WITH IT. Opening the project is not a bonus --
    // it is what makes the level's own contents resolvable (see ownerProjectOf). An explicit
    // .ocproject argument still wins: if someone named both, they meant both.
    if (!openMap.empty() && project.empty()) {
        const std::string owner = ownerProjectOf(openMap);
        if (!owner.empty()) project = owner;
        else AVER_WARN("[Sandbox] '{}' is not inside a project (no .ocproject above it); its "
                       "placements will not resolve", openMap);
    }

    // --crash-test, acted on HERE: before the window, the device, or the project exist, so a crash
    // test exercises the handler and nothing else. Placing it later would mean a failure could just
    // as easily be the renderer as the thing under test.
    if (!crashTest.empty()) {
        if (crashTest == "critical") {
            // The only kind that does NOT crash: it logs one Critical (waking the standby reporter)
            // and lets the run continue to a clean exit, exactly where the reporter must stay silent.
            // Verifying the quiet path matters as much as the loud one: a reporter that pops a window on every GPU hiccup would be turned off within a day.
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
                // Kind::OutOfMemory, through the hook that actually sees an allocation failure: the
                // new-handler (crash::install). A REAL request that cannot be satisfied, not a thrown
                // bad_alloc -- the first version of this test DID throw one, and the report came back
                // as `Crash`, because on Windows an escaping C++ exception raises SEH 0xE06D7363 and
                // the unhandled-exception filter takes it before std::terminate ever runs. That
                // measurement is why the new-handler exists; keep this an allocation.
                //
                // volatile so the result cannot be optimised away as an unused allocation, which is
                // exactly what a release build does to a `new` whose value is discarded.
                volatile void* p = ::operator new(static_cast<size_t>(-1) / 2);
                (void)p;
            } else if (crashTest == "throw") {
                // Kind::Terminate. The name says std::terminate and the mechanism is NOT it: on
                // Windows an escaping C++ exception raises SEH 0xE06D7363, which the unhandled filter
                // takes first. That is a surprising enough fact to be worth a test rather than a
                // comment, since the obvious reading of set_terminate says otherwise.
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
    // SINGLE-INSTANCE FORWARDING, RECEIVER-SIDE GATE: a SUPERSET of the sender gate by construction
    // (that one additionally requires project/openMap non-empty), so a bare .ocbeam can BECOME a
    // primary here even though excluded from forwarding above -- intentional, since this process
    // still deserves to be what a LATER .ocmap/.ocproject double-click reaches. Reading only
    // argc/argv[1][0] keeps the two gates in lockstep automatically.
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
    // THE BACKEND, FROM THE PROJECT, READ BEFORE THE DEVICE EXISTS.
    //
    // Every other RENDER.* key is applied from the loaded project once per frame, which works
    // because those settings can change at any time. A backend cannot: the device is created during
    // Engine::run, long before the editor opens a project, so by the time project_ is populated the
    // choice has already been made. So the manifest is peeked here -- parsed once, for one key,
    // before anything is constructed.
    //
    // THE COMMAND LINE STILL WINS, same precedence every other flag/manifest pair follows here, and
    // it is logged when they disagree. That precedence is not cosmetic: a manifest silently
    // outranking a render flag has corrupted measurements in this repo before, so the rule is that
    // the explicit thing a person just typed beats the stored one, out loud.
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
    // Three states, not two: >0 is an explicit delay, <0 is --no-chunk-stream, and 0 is "the flag was
    // never given" -- which now LEAVES THE MEMBER'S OWN DEFAULT ALONE rather than meaning off. See
    // chunkStreamAutoFrames_'s declaration for why the default is on.
    if (chunkStream > 0)      app->setChunkStreamAuto(chunkStream);
    else if (chunkStream < 0) app->setChunkStreamAuto(0);
    if (fogMatch) app->setFogMatchToStreamRadius(true, fogMatchOpacity);
    if (droneAuto > 0) app->setDroneAuto(droneAuto);
    if (undoTestAuto > 0) app->setUndoTestAuto(undoTestAuto);
    if (keybindTestAuto > 0) app->setKeybindTestAuto(keybindTestMode, keybindTestAuto);
#if AVER_MODULE_MCP
    // Precedence: explicit --mcp <port> > mcp.conf's editor_bridge.port > the built-in default
    // (45123). mcp.conf ONLY supplies the NUMBER -- it cannot turn the channel on itself; --mcp is
    // still required. A missing/malformed mcp.conf resolves silently to the default, with a warning
    // already logged if the value was unparseable rather than simply absent.
    // The resolved port and its source are logged UNCONDITIONALLY, not only on the fallback path, so
    // nobody has to infer which happened from silence.
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
    app->setPtBounces(ptBounces);
    if (layeredBsdf >= 0) app->setLayeredBsdf(layeredBsdf);
    app->setPtOverride(pt);
    app->setGiUpdateInterval(giUpdateInterval);
    app->setGiMode(giModeArg);
    // --restir-visibility none|reconstructed|half|full: parsed here, after the arg loop, the same
    // "stored as a string, parsed alongside every other app->setXxx call" shape --aversr and
    // --gbuffer-debug already use -- so an unrecognised name gets a clear error rather than silently
    // mapping to 0 (No ray).
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
    // there is no app->setXxx for these because there is no SandboxApp member behind them, only the
    // per-frame reasserts beside voxiRenderer_.setNrdLegacyCamera(...) and syncPtSceneView(...)
    // already do the rest. Applied here, after `app` exists but before Engine::run's first frame, so
    // the very first onUpdate() already reasserts whatever a --frames capture asked for.
    //
    // consoleLightingLegacySlot() ITSELF IS GUARDED, unlike consolePtLegacyEnvSlot() just below --
    // it is declared inside EditorConsole.hpp's AVER_MODULE_VOXI block (it feeds
    // VoxiRenderer::setLightingLegacyBits and nothing else), so seeding it has to be guarded here the
    // same way, while --lighting-legacy itself is still parsed unconditionally above like every other
    // flag in that loop. consolePtLegacyEnvSlot() carries no such guard (see its own comment) because
    // the path tracer it feeds has to keep working with AVER_MODULE_VOXI off.
#if AVER_MODULE_VOXI
    editor::consoleLightingLegacySlot() = static_cast<u32>(lightingLegacyArg);
#endif
    editor::consolePtLegacyEnvSlot() = ptLegacyEnvArg;
    // engine-optimisation-plan wave 1 (C-6 application): seeded the SAME way --lighting-legacy is
    // seeded just above -- straight into the raw console slot, since a per-frame reassert beside
    // voxiRenderer_.setLightingLegacyBits(...) is what actually makes each one live (SandboxApp.cpp's
    // onUpdate, right beside the Voxi settings block). Guarded on AVER_MODULE_VOXI because the slots
    // themselves live inside EditorConsole.hpp's AVER_MODULE_VOXI block (C-5) -- they feed
    // VoxiRenderer methods and nothing else, so a build with Voxi off has nowhere for them to go.
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
    // optimisation-wave-2, U1/section 4(a): seeded the SAME way the three console slots just above
    // are -- straight into the raw console slot, since a per-frame reassert beside
    // voxiRenderer_.setGiVisPathView(...)/setBlendedGiCone(...) (onUpdate) is what actually makes each
    // one live for a --frames capture with no console.
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
    // --mesh-heap default|upload (W4): routed through SandboxApp's own member rather than a device
    // call made here directly, because no rhi::IDevice exists yet at this point in main() -- the
    // window and device are created inside Engine::run, well after argv parsing finishes. The member
    // is read at the top of loadProjectMeshes, the first place a device is guaranteed to exist.
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
    if (camTranslateArg != 0.0f) app->setCamTranslate(camTranslateArg);
    app->setRenderScale(renderScale);
    if (!aversrArg.empty()) {
#if AVER_MODULE_SR
        // "auto" is EXPLICIT CLI Auto (plan 3.3 A) -- distinct from never passing --aversr at all:
        // averSrFromCli_ still goes true (so loadEditorPreferences leaves the stored Display choice
        // alone, "the command line wins" everywhere else in this file), but there is no single
        // sr::Quality::Auto to hand aver::sr::parseQuality, so this is checked before it rather than
        // added as a fifth value that enum does not have room for.
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
    // --gbuffer-debug MODE: parsed here, after the arg loop, the same "stored as a string, parsed
    // alongside every other app->setXxx call" shape --aversr uses just above -- so an unrecognised
    // mode name gets a clear error rather than silently mapping to Off.
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
    // Checked AFTER --occlusion-waitidle so that if both are somehow passed, the flag that actually
    // does something today (opting OUT of the new default) wins -- see occlusionNoWaitIdleArg's own
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
    if (ptQualityRamp > 0)    app->setPtQualityRamp(ptQualityRamp);
    if (ptSceneToggleOn > 0)  app->setPtSceneToggleOnAuto(ptSceneToggleOn);
    if (ptSceneToggleOff > 0) app->setPtSceneToggleOffAuto(ptSceneToggleOff);
    if (sunSetAtArg > 0)         app->setSunSetAt(sunSetAtArg, sunSetElevArg, sunSetAzimArg);
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
    // Unconditional (not inside the AVER_MODULE_SR block above, which --gpu-timing and its
    // neighbours happen to sit in for reasons unrelated to this flag): --luma-sweep has nothing to
    // do with AverSR and must apply whether or not that module is built.
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
