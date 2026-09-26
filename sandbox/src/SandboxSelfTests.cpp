// Verification harnesses: the maybe*Test / run*Test / *Check drivers and the command-line knobs they are set through.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
// --lod-select [px] / --no-lod-select: virtualized-geometry LOD selection
// (aver::trifactor::ClusterAdapt). `thresholdPx` is the pixel budget passed to
// chooseLevelCached/screenSpaceErrorPx.
// NOW ON BY DEFAULT: it was off, reproducing pre-existing behaviour of drawing every instance at
// LOD 0 always. Measured on Electric Dreams at --no-vsync: 102.7ms median / 153.0ms p90 off,
// 76.7ms median / 127.3ms p90 on.
void SandboxApp::setLodSelect(bool on, f32 thresholdPx) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    lodSelectEnabled_ = on;
    lodErrorThresholdPx_ = thresholdPx;
#else
    (void)on; (void)thresholdPx;
#endif
}

// --lod-cluster-stats: see lodClusterStatsEnabled_'s own comment for why this is a second,
// separately-gated flag rather than folded into setLodSelect.
void SandboxApp::setLodClusterStats(bool on) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    lodClusterStatsEnabled_ = on;
#else
    (void)on;
#endif
}

// --lod-per-cluster [px]: turns on PER-CLUSTER virtualized-geometry selection, replacing
// --lod-select's per-LEVEL choice for an instance entirely when both are given (this one wins).
// OFF (default) leaves --lod-select or no selection completely unaffected.
void SandboxApp::setLodPerCluster(bool on, f32 thresholdPx) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    lodPerClusterEnabled_ = on;
    if (on) lodErrorThresholdPx_ = thresholdPx;   // shares the one pixel-budget knob with --lod-select
#else
    (void)on; (void)thresholdPx;
#endif
}

// --lod-mesh-shader [px]: the GPU per-cluster path -- an amplification shader runs the SAME local
// cut test as --lod-per-cluster's CPU reference, one thread per cluster, and DispatchMesh's the
// survivors. Wins when the mesh has GPU cluster data AND this device built the pipeline; falls
// back to whichever other flag is set, per-instance, otherwise (house rule 6's "degrade, not
// crash"). --lod-mesh-shader / --no-lod-mesh-shader records an EXPLICIT choice, suppressing the
// caps-driven default in onInit.
void SandboxApp::setLodMeshShader(bool on, f32 thresholdPx) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    lodMeshShaderRequest_ = on ? 1 : 0;
    lodMeshShaderEnabled_ = on;
    if (on) lodErrorThresholdPx_ = thresholdPx;   // shares the one pixel-budget knob with the others
#else
    (void)on; (void)thresholdPx;
#endif
}

void SandboxApp::setUiDemo(bool on) { showUiDemo_ = on; }

void SandboxApp::setOpenAsset(std::string p) { openAsset_ = std::move(p); }

// --select <substring>: select the first entity whose name or outliner label contains it.
// EXISTS TO MAKE THE DETAILS PANEL CAPTURABLE: a bounded --frames run starts with nothing
// selected, so most panels were unreachable from a screenshot. --graph-select is the same idea
// for a node inside the graph editor; this is its counterpart for the world.
void SandboxApp::setSelectEntity(std::string p) { selectEntity_ = std::move(p); }

// --water <heightCm>. Stored whether or not the module is compiled in, so a build without it can
// say so rather than ignoring the flag in silence.
void SandboxApp::setWater(bool on, f32 heightCm) { waterEnabled_ = on; waterHeightCm_ = heightCm; }

// --open-legacy: open an older-series project WITHOUT upgrading it. Never migrates, so it
// cannot damage the project it is pointed at -- which is what makes it safe to hand a
// benchmark that must measure content exactly as it exists on disk.
void SandboxApp::setOpenLegacy(bool on) { openLegacy_ = on; }

// --graph-select <nodeId>: fired one frame after --open-asset opens, so a --frames/--screenshot
// capture can prove a details/inspector panel renders a real, populated node -- no human clicking.
// Same shape as every other CLI test-proof flag: a plain setter here, the work happens in onGui().
void SandboxApp::setGraphSelectNode(std::string id) { graphSelectNode_ = std::move(id); }

// --graph-tab viewport: brings the graph tab's inner Viewport tab to the front, on the same
// frame budget --graph-select uses. Only "viewport" does anything; Event Graph is already the
// one in front, so there is nothing for the other value to do.
void SandboxApp::setGraphTab(std::string tab) { graphTab_ = std::move(tab); }

void SandboxApp::setInputProbe(bool on) { inputProbe_ = on; }

void SandboxApp::setAutoCompile(bool on) { autoCompile_ = on; autoCompileFromCli_ = on; }

void SandboxApp::setFocusLevelAt(int frame) { focusLevelAt_ = frame; }

// --chunk-stream [N]: frames left before setChunkStreamingEnabled(true) fires on its own. Member
// and setter are unguarded (like focusLevelAt_) even though the effect is AVER_MODULE_SCENE-only;
// see the countdown in onUpdate.
void SandboxApp::setChunkStreamAuto(int framesIn) { chunkStreamAutoFrames_ = framesIn; }

#if AVER_MODULE_SCENE
void SandboxApp::setDroneGraph(std::string relPath) { droneGraphRel_ = std::move(relPath); }

#endif

#if AVER_MODULE_SCENE
#else
void SandboxApp::setDroneGraph(std::string) {}

#endif

// --landscape <path>: an explicit .ocland override. Wins over the level's LANDSCAPE record and the
// levelname.ocland convention the next time a level loads (handed to GameLandscape::setPathOverride) -- so a
// --frames capture can prove the LOD-selection path draws real terrain without a level file or a
// human clicking anything. Unguarded like setChunkStreamAuto/setDroneAuto above.
void SandboxApp::setLandscapePath(std::string path) { landscapeCliOverride_ = std::move(path); }

#if AVER_MODULE_SCENE
void SandboxApp::setFogMatchToStreamRadius(bool on, f32 targetOpacity) {
    matchFogToStreamRadius_ = on;
    if (targetOpacity > 0.0f) fogMatchTargetOpacity_ = std::clamp(targetOpacity, 0.05f, 0.99f);
}

#endif

#if AVER_MODULE_SCENE
#else
void SandboxApp::setFogMatchToStreamRadius(bool, f32) {}

#endif

// --drone [N]: frames left before setDroneEnabled(true) fires on its own, same shape as
// --chunk-stream. Member and setter are unguarded for the same reason chunkStreamAutoFrames_ is
// (the countdown in onUpdate is what's actually AVER_MODULE_SCENE-gated).
void SandboxApp::setDroneAuto(int framesIn) { droneAutoFrames_ = framesIn; }

// --undo-test [N]: frames left before runUndoTest() fires and the process exits, same shape as
// --drone/--chunk-stream. See runUndoTest()'s own comment for what it actually proves.
void SandboxApp::setUndoTestAuto(int framesIn) { undoTestAutoFrames_ = framesIn; }

// --keybind-test write|read [N]: frames left before runKeybindPersistTest(mode) fires and the
// process exits. See that function's own comment for what the two modes prove between them.
void SandboxApp::setKeybindTestAuto(std::string mode, int framesIn) {
    keybindTestMode_ = std::move(mode); keybindTestAutoFrames_ = framesIn;
}

void SandboxApp::setShowEditorPrefs(bool on) { if (on) showEditorPrefs_ = true; }

// --scroll-prefs-to-keybinds: see the one-shot flag's own comment in buildEditorPrefs().
void SandboxApp::setScrollPrefsToKeybinds(bool on) { scrollPrefsToKeybinds_ = on; }

void SandboxApp::setHudTest(int idx) { hudTest_ = idx; }

void SandboxApp::setSaveProject(bool on) { saveProject_ = on; }

void SandboxApp::setSaveLevelTo(std::string p) { saveLevelTo_ = std::move(p); }

void SandboxApp::setRayProbe(f32 x, f32 y) { rayProbe_ = true; rayProbeX_ = x; rayProbeY_ = y; }

// Queues one Content Browser import to run on startup. --import <src> <destDir>.
void SandboxApp::setImportOnce(std::string src, std::string dst) { importSrc_ = std::move(src); importDst_ = std::move(dst); }

bool* SandboxApp::autoCompileFlag() { return &autoCompile_; }

// Turns clouds on, optionally at the given coverage. --clouds [coverage].
void SandboxApp::setClouds(f32 coverage) {
    sky_.cloudsEnabled = true;
    if (coverage >= 0.0f) sky_.cloudCoverage = coverage;
}

// Selects the physical sky and optionally moves the sun's elevation. --sky-physical [elevation].
//
// RECORDED AS AN OVERRIDE RATHER THAN WRITTEN ONCE, and that is the whole fix. Writing sky_ here
// happens at startup, and applyLevelSky then overwrites sky_ wholesale the moment a level opens
// -- so on any run that loads a level, which is every interesting one, this did nothing at all.
// MEASURED: `--sky-physical 20` and `--sky-physical 60` on Sponza produced BYTE-IDENTICAL
// frames, and identical to passing no flag, while the log said nothing. It was found while
// trying to use the sun's elevation to tell bounced light apart from an ambient term, which is
// exactly the kind of measurement a silently-inert flag turns into a confident wrong answer.
//
// The sixth instance of the flag-versus-authored-state precedence bug this file records; see the
// --gi-update-interval block for the previous four and --sky-light for the fifth.
void SandboxApp::setSkyPhysical(f32 elevationDeg) {
    skyModelOverride_ = 1;
    if (elevationDeg > -90.0f) sunElevationOverride_ = elevationDeg;
}

void SandboxApp::setSkyAuthored() { skyModelOverride_ = 0; }

// Sets exposure, bloom intensity and auto-exposure. --exposure / --bloom / --auto-exposure.
//
// ASSIGNS UNCONDITIONALLY, AND HAS TO. argv's defaults are exposure 1.0 and bloom 0.0, and the
// second of those is NOT PostSettings' own default of 0.06 -- so the sandbox has always run with
// bloom off unless asked, and every recorded gate image was taken that way. Making the
// assignment conditional on the flag being present would quietly restore 0.06 and move all 20.
// The two `set` bools exist only to decide who wins over a stored preference; see
// loadEditorPreferences.
void SandboxApp::setPost(f32 exposure, bool exposureSet, f32 bloomIntensity, bool bloomSet, bool autoExposure) {
    post_.exposure = exposure;
    post_.bloomIntensity = bloomIntensity;
    if (autoExposure) post_.autoExposure = true;
    postExposureFromCli_ = exposureSet;
    postBloomFromCli_    = bloomSet;
    postAutoExpFromCli_  = autoExposure;
}

// --tonemap N / --max-radiance F. Both are POST settings rather than renderer ones, so they land
// straight in post_ and need no tier derivation; -1 / negative leaves the shipped default alone.
void SandboxApp::setTonemap(int mode)      { if (mode >= 0) post_.tonemap = static_cast<u32>(mode); }

void SandboxApp::setMaxRadiance(f32 ceil)  { if (ceil >= 0.0f) post_.maxRadiance = ceil; }

// Disables auto-exposure for a capture run unless the run asked for it.
void SandboxApp::applyCaptureExposureRule(bool explicitlyRequested) {
    if (maxFrames_ != 0 && !explicitlyRequested) post_.autoExposure = false;
}

void SandboxApp::setFocusVoxi(bool b) { focusVoxi_ = b ? 4 : 0; }

// --project-settings-page N: verification-only, jumps straight to sub-page N (see
// settingsPage_'s own comment for the index) instead of leaving a screenshot script to navigate
// a docked window it cannot click. Implies --project-settings.
void SandboxApp::setProjectSettingsPage(int page) { focusVoxi_ = 4; settingsPage_ = page; }

// Opens a drawer fully open on startup, optionally in a Content subfolder. --drawer.
void SandboxApp::setDrawerOpen(int which, std::string sub) {
    if (!which) return;
    drawer_ = drawerShown_ = which == 2 ? Drawer::Log : which == 3 ? Drawer::Console : Drawer::Content;
    drawerAnim_ = 1.0f;
    drawerStartSub_ = std::move(sub);
    // NOT routed through consoleFocusPending_ (toggleDrawer's own arm): this path is --drawer's
    // startup hook, whose only real caller is a screenshot/capture script that wants the panel
    // shown, not the input line stealing keyboard focus out from under it.
}

void SandboxApp::setFocusScript(bool b) { tools_.armNewScript(b); }

void SandboxApp::setFocusTools(bool b) { tools_.armToolsMenu(b); }

void SandboxApp::setOpenLevelPicker(bool b) { armOpenLevelPicker_ = b; }

// --no-editor-chrome sets a plain bool that suppresses viewport CHROME -- the grid, the gizmo, the
// selection outline (SandboxRender.cpp) -- and every one of those is drawn whether or not
// AVER_MODULE_SCENE is compiled in; none of them touch a scene::Entity. noEditorChrome_ only
// landed inside SandboxApp.hpp's AVER_MODULE_SCENE block because that is where the cursor was when
// multi-selection was being written next to it, not because the flag needs a world. Left unguarded
// here on purpose; see this task's headerChanges for moving the member out of that block instead of
// compiling this setter out.
void SandboxApp::setNoEditorChrome(bool b) { noEditorChrome_ = b; }

// --scene-census, unlike --no-editor-chrome above, has nothing to report without a scene to walk:
// its one reader (SandboxApp.cpp's onUpdate) calls world::takeSceneCensus(scene::World::instance(),
// playerStart_), and sceneCensus_ is declared `#if AVER_MODULE_SCENE` in SandboxApp.hpp for exactly
// that reason. This setter must be guarded the same way -- same shape as
// setDroneGraph/setFogMatchToStreamRadius above.
#if AVER_MODULE_SCENE
void SandboxApp::setSceneCensus(bool b) { sceneCensus_ = b; }
#else
void SandboxApp::setSceneCensus(bool) {}
#endif

void SandboxApp::setOpenLevelByName(std::string n) { openLevelByName_ = std::move(n); }

void SandboxApp::setFocusCompileMenu(bool b) { tools_.armCompileMenu(b); }

#if AVER_MODULE_MCP
void SandboxApp::setMcpPort(u16 p) { mcpPort_ = p; }

#endif

void SandboxApp::setSkinTest() { skinTest_ = true; }

void SandboxApp::setSkinDrawTest() { skinDrawTest_ = true; }

void SandboxApp::setParticleTest() { particleTest_ = true; }

// --no-particle-gi: the A/B toggle particles DECIDED 4's own proof needs -- same scene, same Voxi
// volume, only whether particleRenderer_.setGiSeam is ever called differs. Distinct from --no-gi
// (stops the volume being BUILT) and --no-gi-cone (stops the opaque scene's own cone-trace read).
void SandboxApp::setNoParticleGi() { noParticleGi_ = true; }

// --particle-stress <N> <M>: verification-only, see particleStressEmitters_'s own comment.
void SandboxApp::setParticleStress(int emitters, int maxParticles) {
    particleStressEmitters_ = emitters;
    particleStressMaxParticles_ = maxParticles;
}

void SandboxApp::setParticleStressSecondEmitter() { particleStressSecondEmitter_ = true; }

void SandboxApp::setReflTest() { reflTest_ = true; }

void SandboxApp::setFurnaceTest() { furnaceTest_ = true; }

void SandboxApp::setFurnaceSun() { furnaceTest_ = true; furnaceSun_ = true; }

// --furnace-grid: implies --furnace-test for the same reason --pt-furnace does -- the furnace
// is a property of the SKY, and a grid of plates lit by nothing measures nothing. Combines
// with --furnace-sun, which is how the DIRECT specular path gets the same treatment.
void SandboxApp::setFurnaceGrid() { furnaceTest_ = true; furnaceGrid_ = true; }

// --furnace-tilt DEG: implies the grid, since it is the grid it rotates.
void SandboxApp::setFurnaceTilt(f32 d) { furnaceTest_ = true; furnaceGrid_ = true; furnaceTilt_ = d; }

// --sun-angle DEG: the sun's ANGULAR DIAMETER, not a look control: it sets how wide a ray-traced
// penumbra is, and at the real 0.545 degrees that penumbra is narrower than a pixel at contact
// distances. A gate sampling a partially-occluded pixel has to widen the source until the transition spans several pixels.
void SandboxApp::setSunAngle(f32 deg) { sunAngle_ = deg; }

// --pt-furnace: the furnace measured through the PATH TRACER rather than the raster shading
// model. Implies --furnace-test since the furnace is a property of the SKY, and the tracer reads the same averFurnaceL() every other shading path does.
void SandboxApp::setPtFurnaceTest() { furnaceTest_ = true; ptFurnaceTest_ = true; }

// --pt-scene seeds the WANT flag syncPtSceneView() reconciles every frame; see that function's
// own comment for why the actual registration happens there and not here.
void SandboxApp::setPtSceneView() { ptSceneViewWantEnabled_ = true; ptSceneViewFromCli_ = true; }

// --pt-scene-toggle-on/--pt-scene-toggle-off [N]: see ptSceneToggleOnAutoFrames_'s own comment.
// --pt-quality-ramp [N]: see the ramp itself in onUpdate for what it is for.
void SandboxApp::setPtQualityRamp(int everyFrames) {
    ptQualityRampEvery_ = everyFrames;
    ptQualityRampCountdown_ = everyFrames;
}

void SandboxApp::setPtSceneToggleOnAuto(int framesIn)  { ptSceneToggleOnAutoFrames_  = framesIn; }

void SandboxApp::setPtSceneToggleOffAuto(int framesIn) { ptSceneToggleOffAutoFrames_ = framesIn; }

// --sun-set-at N ELEV AZIM / --gi-history-reset-at N: see sunSetAtFrames_'s own comment.
void SandboxApp::setSunSetAt(int framesIn, f32 elevDeg, f32 azimDeg) {
    sunSetAtFrames_ = framesIn; sunSetElevDeg_ = elevDeg; sunSetAzimDeg_ = azimDeg;
}

void SandboxApp::setSunSweep(int framesIn, f32 degPerFrame, int turns) {
    sunSweepFrames_ = framesIn; sunSweepDeg_ = degPerFrame; sunSweepTurnsLeft_ = turns;
}

void SandboxApp::setGiHistoryResetAt(int framesIn) { giHistoryResetAtFrames_ = framesIn; }

#if AVER_MODULE_SR
void SandboxApp::setAverSrCycleAuto(int framesIn) { averSrCycleFrames_ = framesIn; }

#endif

void SandboxApp::setResizeCycle(int n) { resizeCycle_ = n < 0 ? 0 : (u64)n; }

void SandboxApp::setGpuTiming(bool on) { gpuTiming_ = on; }

// --luma-sweep [STRIDE]: see lumaSweepCheck() for what it measures and setGiMode/setCamWobble
// above for the two dials it is meant to be read alongside. FLAG-ONLY and additive -- default
// off, and it only affects what gets READ BACK and logged, never what the renderer draws.
void SandboxApp::setLumaSweep(bool on, int stride) { lumaSweep_ = on; lumaSweepStride_ = stride > 0 ? stride : 1; }

// --firefly-metric [MULT]: OUTLIER pixels, not mean luminance -- see lumaSweepCheck() for the
// measurement itself (it shares --luma-sweep's own readback/decode rather than opening a second
// one). FLAG-ONLY, default off, additive: a run that never passes this is bit-for-bit the run it
// always was, exactly like --luma-sweep beside it.
void SandboxApp::setFireflyMetric(bool on, f32 mult) { fireflyMetric_ = on; fireflyMult_ = mult > 0.0f ? mult : 8.0f; }

// pieCamFrames_/inputStuckFrames_/inputSrcFrames_/wheelTestFrames_ (and recapFrames_/vmFrames_
// further below) are all declared `#if AVER_MODULE_FRAMEWORK` in SandboxApp.hpp: maybePieCameraTest
// and its three siblings here live entirely inside that same block, because there is no
// Play-in-Editor camera or input to drive a proof against without Framework's play-mode plumbing.
// These setters were left unguarded, so a FRAMEWORK=OFF tree (module-matrix.ps1's scene-off row,
// which forces FRAMEWORK off with it) failed on an undeclared identifier at every one of them.
// Guarded the same way setDroneGraph/setFogMatchToStreamRadius are guarded above, with a
// same-signature no-op stub so the unconditional --pie-camera-test/--input-stuck-test/... argv
// parsing in SandboxMain.cpp keeps compiling either way.
#if AVER_MODULE_FRAMEWORK
void SandboxApp::setPieCameraTest(int n) { pieCamFrames_ = n; }
#else
void SandboxApp::setPieCameraTest(int) {}
#endif

#if AVER_MODULE_FRAMEWORK
void SandboxApp::setInputStuckTest(int n) { inputStuckFrames_ = n; }
#else
void SandboxApp::setInputStuckTest(int) {}
#endif

#if AVER_MODULE_FRAMEWORK
void SandboxApp::setInputSourceTest(int n) { inputSrcFrames_ = n; }
#else
void SandboxApp::setInputSourceTest(int) {}
#endif

#if AVER_MODULE_FRAMEWORK
void SandboxApp::setWheelSpeedTest(int n) { wheelTestFrames_ = n; }
#else
void SandboxApp::setWheelSpeedTest(int) {}
#endif

// multiSelTestFrames_ is declared `#if AVER_WITH_IMGUI` in SandboxApp.hpp -- its one reader,
// runMultiSelectTest further down this file, is already guarded the same way, and so is its
// onUpdate call site in SandboxApp.cpp. Only this setter was left unguarded, so a no-ui or
// d3d12-off tree (the two module-matrix.ps1 rows that turn AVER_WITH_IMGUI off) failed on it.
// Every setter below down to setClearShaderCache shares this exact shape -- a member guarded
// `#if AVER_WITH_IMGUI` whose only reader is a run*Test also guarded that way further down this
// file -- so each gets the same fix without repeating the explanation.
#if AVER_WITH_IMGUI
void SandboxApp::setMultiSelectTest(int n) { multiSelTestFrames_ = n; }
#else
void SandboxApp::setMultiSelectTest(int) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setCbMoveTest(const std::string& dir) { cbMoveTestDir_ = dir; cbMoveTestFrames_ = 10; }
#else
void SandboxApp::setCbMoveTest(const std::string&) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setSaveDirtyTest(int n) { saveDirtyTestFrames_ = n; }
#else
void SandboxApp::setSaveDirtyTest(int) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setPrefsWriteTest(int n) { prefsWriteTestFrames_ = n; }
#else
void SandboxApp::setPrefsWriteTest(int) {}
#endif

// --notify-test N. The lift is the point: drawNotifications refuses to draw during a bounded
// run so a capture never has a toast in shot, and this is the one run where the toast IS the
// thing being captured. notifyTestFrames_/notifyTestLift_ are both `#if AVER_WITH_IMGUI` (the
// toast itself is drawNotifications' own, in the ImGui half of this app).
#if AVER_WITH_IMGUI
void SandboxApp::setNotifyTest(int n) { notifyTestFrames_ = n; notifyTestLift_ = true; }
#else
void SandboxApp::setNotifyTest(int) {}
#endif

// --autosave-test <sec>: shorten the interval, mark the level dirty so the timer has a reason to
// run, and lift the capture suppression. The countdown is otherwise unreachable from a bounded
// run -- it needs thirty seconds of unsaved edits, which no capture has. NEEDS BOTH MACROS, not
// just one: autosaveIntervalSec_ is `#if AVER_MODULE_SCENE` (maybeAutosave in SandboxAutosave.cpp
// is a whole-function AVER_MODULE_SCENE block, since autosave saves the level), while
// notifyTestLift_/autosaveTestArm_/autosaveTestLift_ are `#if AVER_WITH_IMGUI` (the toast and the
// capture-suppression lift they drive both live there). Missing either one leaves one of the four
// assignments below reaching a member that does not exist in that configuration.
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
void SandboxApp::setAutosaveTest(f32 sec) {
    autosaveIntervalSec_ = sec;
    notifyTestLift_ = true;
    autosaveTestArm_ = true;
    autosaveTestLift_ = true;
}
#else
void SandboxApp::setAutosaveTest(f32) {}
#endif

// --find-refs <content-relative-path>: print what references an asset and exit. The delete
// confirm's scan, reachable without a modal -- so the thing that stops someone destroying a
// shared asset can be tested rather than eyeballed once.
#if AVER_WITH_IMGUI
void SandboxApp::setFindRefs(const std::string& p) { findRefsPath_ = p; findRefsFrames_ = 8; }
#else
void SandboxApp::setFindRefs(const std::string&) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setRenameRepointTest(int frames) { renameRepointFrames_ = frames > 0 ? frames : 8; }
#else
void SandboxApp::setRenameRepointTest(int) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setProjectSwitchTest(int frames) { projectSwitchFrames_ = frames > 0 ? frames : 8; }
#else
void SandboxApp::setProjectSwitchTest(int) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setValidateGraph(const std::string& p) { validateGraphPath_ = p; validateGraphFrames_ = 8; }
#else
void SandboxApp::setValidateGraph(const std::string&) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setGraphPrintTest(int frames) { graphPrintTestFrames_ = frames > 0 ? frames : 8; }
#else
void SandboxApp::setGraphPrintTest(int) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setAssetAssignTest(int frames) { assetAssignTestFrames_ = frames > 0 ? frames : 8; }
#else
void SandboxApp::setAssetAssignTest(int) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setGraphHitsTest(const std::string& p) { graphHitsTestPath_ = p; graphHitsTestFrames_ = 8; }
#else
void SandboxApp::setGraphHitsTest(const std::string&) {}
#endif

#if AVER_WITH_IMGUI
void SandboxApp::setClearShaderCache(const std::string& dir) {
    clearShaderCacheDir_ = dir; clearShaderCacheFrames_ = 4;
}
#else
void SandboxApp::setClearShaderCache(const std::string&) {}
#endif

// Same reason as pieCamFrames_ et al. at the top of this run: recapFrames_/vmFrames_ are
// `#if AVER_MODULE_FRAMEWORK` in SandboxApp.hpp (maybeRecaptureTest/maybeViewmodelTest are
// Play-in-Editor-only).
#if AVER_MODULE_FRAMEWORK
void SandboxApp::setRecaptureTest(int n)   { recapFrames_ = n; }
#else
void SandboxApp::setRecaptureTest(int)     {}
#endif

#if AVER_MODULE_FRAMEWORK
void SandboxApp::setViewmodelTest(int n)   { vmFrames_ = n; }
#else
void SandboxApp::setViewmodelTest(int)     {}
#endif

void SandboxApp::setSkinSceneDir(std::string d) { skinSceneDir_ = std::move(d); }

// --shader-source <dir>: watch a shader source tree and reload without restarting.
void SandboxApp::setShaderSourceDir(std::string d) { shaderSourceDir_ = std::move(d); }

#if AVER_MODULE_SYNAPSE
void SandboxApp::setBakeNavOnStart(f32 cellCm) { navBakeOnStart_ = true; navBakeCell_ = cellCm; }

#endif

// --cam X Y Z PITCH YAW: places the viewport camera outright, cm/degrees -- a capture tool since
// every other way into this camera either frames the level (always pitch -31 deg, horizon just off
// the top edge) or needs a real mouse, so nothing headless could aim at the sky, and the sky is the
// one thing no gate covers. Applied last, after level framing.
// --cam-wobble DEG PERIOD: swing the yaw sinusoidally about wherever the camera is aimed.
// A SINE THAT RETURNS TO ZERO, not a one-way pan: comparing a moving run against a still one AT
// THE SAME PIXEL needs the probe to land on the same geometry, and sin() is zero at every whole
// multiple of the period, so a run whose frame count lands on one ends aimed where it started.
// Driven off the engine's frame COUNTER, never the clock, so the path is identical every run.
void SandboxApp::setCamWobble(f32 degrees, i32 periodFrames) {
    camWobbleDeg_ = degrees;
    camWobblePeriod_ = periodFrames > 0 ? periodFrames : 0;
}

// --cam-wobble-stop N: the frame the wobble above stops at. See its own use site in onUpdate.
void SandboxApp::setCamWobbleStop(i32 frame) { camWobbleStopFrame_ = frame > 0 ? frame : 0; }

// --set NAME VALUE, repeatable: console variables applied once, on the first frame that has a
// device. Console syntax is exactly what the drawer's own input takes, so a capture can A/B any
// dial the console exposes without a new flag per dial -- which is the whole point, since a
// bisection invents dials faster than argv can grow spellings for them.
void SandboxApp::setConsoleSets(std::vector<std::pair<std::string, std::string>> sets) {
    consoleSets_ = std::move(sets);
}

// --mesh-heap default|upload (W4): read at the top of loadProjectMeshes, the first point in the
// frame a device is guaranteed to exist -- argv parsing happens before Engine::run creates one.
// false (DEFAULT, "upload") is today's behaviour, untouched by this flag's absence.
void SandboxApp::setMeshHeapDefault(bool on) { meshHeapDefault_ = on; }

// --lod-share-vertices 0|1 (W11): read inside loadProjectMeshes' LOD-ladder loop. false (DEFAULT)
// is today's behaviour -- every LOD level gets its own independent vertex buffer.
void SandboxApp::setLodShareVertices(bool on) { lodShareVertices_ = on; }

// --cam-translate SPEED: fly the camera forward along camForward() by SPEED world-cm every
// frame, starting at frame 1. UNLIKE --cam-wobble (pure rotation about a fixed point, which
// barely changes what-occludes-what -- the same walls occlude the same objects, just at
// different screen positions), this is TRANSLATION through the scene: occlusion relationships
// change continuously, the way WASD forward-flight does. Driven off the frame counter (see
// onUpdate), never the clock, so the path is identical every run at a given --frames count.
// Composes with --cam-wobble if both are given: wobble only ever touches yaw_, this only ever
// touches camPos_.
void SandboxApp::setCamTranslate(f32 speedCmPerFrame) { camTranslateSpeed_ = speedCmPerFrame; }

void SandboxApp::setCamera(Vec3 pos, f32 pitchDeg, f32 yawDeg) {
    camOverride_ = true;
    camPosOverride_ = pos;
    pitchOverride_ = pitchDeg * 0.01745329252f;
    yawOverride_   = yawDeg   * 0.01745329252f;
}

void SandboxApp::setScriptsDir(std::string d) { scriptsDir_ = std::move(d); }

void SandboxApp::setSpawnTest(std::string cls) { spawnTestClass_ = std::move(cls); }

// Called on every launch (SandboxMain passes the parsed flag, false when absent), so only a real
// --unlit latches viewModeFromCli_.
void SandboxApp::setUnlitMode(bool on) { unlit_ = on; if (on) viewModeFromCli_ = true; }

// --view-mode: see its own declaration comment (SandboxApp.hpp) for the full contract. Lowercased
// here, case-insensitively, the same courtesy every other string flag in this file gets (--aversr,
// --gbuffer-debug, --restir-visibility, ...). "undenoised" is independent of everything else this
// sets and returns early; every other name mirrors what the matching viewport dropdown Selectable
// does (SandboxViewport.cpp's "viewMode" popup) -- Lit/Unlit/Wireframe clear debugView_, since a
// debug view did not exist as a concept before this flag and there is no old behaviour of leaving
// it set to preserve; a ray-hit/triangles name clears wireframe_ and gbufferDebugView_, mirroring
// that Selectable's own "needs the ray-driven path" clear.
void SandboxApp::setViewMode(const std::string& mode) {
    std::string m = mode;
    for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (m == "undenoised") {
        undenoised_ = true;
        AVER_INFO("[Sandbox] --view-mode undenoised");
        return;
    }
    if (m == "lit" || m == "unlit" || m == "wireframe") {
        unlit_ = (m == "unlit");
        wireframe_ = (m == "wireframe");
#if AVER_MODULE_VOXI
        debugView_ = voxi::VoxiRenderer::ViewDebug::None;
#endif
    } else if (m == "rayhit-instance" || m == "rayhit-material" || m == "rayhit-distance" || m == "triangles" ||
               m == "ambient-occlusion") {
#if AVER_MODULE_VOXI
        if (m == "rayhit-instance")      debugView_ = voxi::VoxiRenderer::ViewDebug::RayHitInstance;
        else if (m == "rayhit-material") debugView_ = voxi::VoxiRenderer::ViewDebug::RayHitMaterial;
        else if (m == "rayhit-distance") debugView_ = voxi::VoxiRenderer::ViewDebug::RayHitDistance;
        else if (m == "ambient-occlusion") debugView_ = voxi::VoxiRenderer::ViewDebug::AmbientOcclusion;
        else                             debugView_ = voxi::VoxiRenderer::ViewDebug::Triangles;
        wireframe_ = false;
        gbufferDebugView_ = GBufferDebugFeature::Mode::Off;
#else
        AVER_WARN("[Sandbox] --view-mode {} was given but this build has no Voxi module "
                  "(-DAVER_MODULE_VOXI=ON to include it); there is no ray-driven path to show it through", m);
        return;
#endif
    } else {
        AVER_ERROR("[Sandbox] --view-mode '{}' not recognised (lit|unlit|wireframe|rayhit-instance|"
                   "rayhit-material|rayhit-distance|triangles|undenoised)", mode);
        return;
    }
    viewModeFromCli_ = true;
    AVER_INFO("[Sandbox] --view-mode {}", m);
}

void SandboxApp::setPlayTest() { playTest_ = true; }

void SandboxApp::setProjectPath(std::string p) { projectPath_ = std::move(p); }

// --mode <name>. Stored, not applied: setEditorMode refuses a mode whose preconditions aren't met,
// and at parse time no project is open and no terrain is loaded, so applying it here would refuse
// every mode but Select. Consumed once, on the first frame that has a level (applyStartMode).
void SandboxApp::setStartMode(std::string m) { startMode_ = std::move(m); }

// <path>.ocmap given on the command line: opened INSTEAD of the project's start map.
void SandboxApp::setOpenMap(std::string p) { openMapPath_ = std::move(p); }

void SandboxApp::armBrowser(bool on) { browserActive_ = on; }

// Whether THIS launch's command line looked like the shell's own shape (see createApplication's
// computation of this, right beside the sender-side gate it is the superset of). Read once, at
// window_ = e.window() in onInit, to decide whether to register as a single-instance primary.
void SandboxApp::setSingleInstanceEligible(bool b) { singleInstanceEligible_ = b; }

#if AVER_MODULE_FRAMEWORK
// Spawns one instance of the --spawn-test class, then destroys it a few frames later.
void SandboxApp::maybeSpawnTestActor() {
    if (spawnTestClass_.empty()) return;

    if (!spawnTestDone_) {
        spawnTestDone_ = true;
        const int32_t c = aver_fw_class_find(spawnTestClass_.c_str());
        if (c == 0) {
            AVER_WARN("[spawn-test] no class named '{}' is declared - is the script assembly loaded? "
                      "(pass --scripts <dir> pointing at the built actor assembly)", spawnTestClass_);
            return;
        }
        spawnTestEntity_ = aver_fw_spawn(c, "spawn-test-instance", nullptr, nullptr, nullptr);
        if (spawnTestEntity_ == 0)
            AVER_WARN("[spawn-test] class '{}' failed to spawn", spawnTestClass_);
        else
            AVER_INFO("[spawn-test] spawned '{}' as entity {} - watch for its OnBeginPlay/OnTick lines",
                      spawnTestClass_, spawnTestEntity_);
        return;
    }

    if (spawnTestEntity_ != 0 && ++spawnTestFrames_ == 3) {
        AVER_INFO("[spawn-test] destroying entity {} - watch for its OnEndPlay line", spawnTestEntity_);
        aver_fw_destroy(spawnTestEntity_);
        spawnTestEntity_ = 0;
    }
}

namespace {

// ---- SYNTHETIC INPUT FOR THE PLAY SELF-TESTS, INTO THE ACCUMULATOR THE PUBLISHER READS --------
//
// WHY THESE EXIST AS OF THIS COMMIT. Until SandboxApp::pushInput was folded into
// game::publishInput, an io.AddKeyEvent / io.AddMouseButtonEvent was enough to reach gameplay,
// because the editor's publish read ImGui and ImGui's own queue was therefore on the path. It is
// not any more: the publish reads input_, which is fed from Win32Window::dispatch. This file
// already recorded the underlying fact next to --input-source-test -- io.AddKeyEvent "injects
// directly into ImGui's queue, never reaching Win32Window::dispatch()" -- it simply did not matter
// for the framework half until now. These put the event where the publisher will look for it.
//
// TIMING, because it is what makes an injection from HERE land at all. These drivers run from
// onUpdate AFTER pushInput has already published this frame (sandbox/src/SandboxApp.cpp:2363 for
// the publish, :2383 onwards for the drivers), and input_.newFrame() at the tail of onRender rolls
// the press/release edges and zeroes the deltas but LEAVES HELD STATE ALONE (InputState::newFrame,
// whose own header calls that "the point"). So a synthetic HELD button or key injected here is
// still held when the NEXT frame's pushInput reads it: one frame of latency, then it stays down
// until something says otherwise. An EDGE injected here, by contrast, is rolled away before any
// publish sees it, which is why every caller below asserts a hold rather than a tap.
#if AVER_WITH_IMGUI
// Guarded because BOTH its callers are: --recapture-test and --input-stuck-test need own_ resolved,
// which only happens in a run with a UI context. At /W4 an internal-linkage function nobody calls
// is C4505, so the guard is what keeps the no-ui row of module-matrix.ps1 quiet.
void injectMouseButton(InputState& in, i32 button, bool pressed) {
    Event e;
    e.type    = EventType::MouseButton;
    e.button  = button;
    e.pressed = pressed;
    // THE POSITION IS WHATEVER THE ACCUMULATOR ALREADY HOLDS, and stating it is not redundant:
    // InputState::onEvent takes mouseX/mouseY off a BUTTON event as well as off a move
    // (modules/platform/src/InputState.cpp), so a button left at the default 0,0 would move its
    // idea of the cursor to the corner and manufacture a large bogus delta out of the next real
    // WM_MOUSEMOVE. Restating the current position makes this a button event and nothing else.
    e.mouseX  = in.mouseX();
    e.mouseY  = in.mouseY();
    in.onEvent(e);
}
#endif

// The keyboard twin, by RAW WIN32 VIRTUAL KEY -- which is what InputState is keyed by, and what
// frameworkKeyFromVk (aver/framework/InputKeys.hpp) turns back into a named AVER_FW_KEY_* slot
// inside the publisher. Not an ImGuiKey: the two enums are unrelated, which is the same reason the
// raw-VK twin has always been published from input_ rather than from ImGui.
void injectKey(InputState& in, i32 vk, bool pressed) {
    Event e;
    e.type    = EventType::Key;
    e.key     = vk;
    e.pressed = pressed;
    in.onEvent(e);
}

} // namespace

// --pie-camera-test [N]: press Play, touch NOTHING, prove the camera holds perfectly still -- a
// PIE camera with no input has exactly one correct behaviour, and any drift is a bug, needing no
// eyes or judgement call.
// It exists because the loop it catches is invisible elsewhere: drivePlayCamera derives
// yaw_/pitch_ back OUT of the pawn's forward vector every frame while fly control writes rotation
// IN from yaw_/pitch_ -- if the quaternion doesn't exactly invert camForward(), the angles walk
// further every frame and the view slowly tumbles. Nothing else asserts the round trip.
void SandboxApp::maybePieCameraTest() {
    if (pieCamFrames_ <= 0) return;
// AND ON AVER_WITH_IMGUI, the same pairing maybeInputSourceTest below carries and for a related
// reason: this test drives the camera by writing keys and mouse deltas into ImGuiIO, so
// AVER_MODULE_FRAMEWORK -- which proves only that there is a pawn to possess -- is not enough. With
// AVER_ENABLE_UI=OFF there is no io to write into and nothing for the assertions to observe.
#if AVER_MODULE_FRAMEWORK && AVER_WITH_IMGUI
    ++pieCamFrame_;
    if (pieCamFrame_ == 10) { startPlay(); return; }   // let startup settle, as --play-test does
    if (pieCamFrame_ < 11) return;

    ImGuiIO& io = ImGui::GetIO();

    // ---- phase 1: idle. Nothing touches anything; the view must not move. ----
    if (pieCamFrame_ == 11) {
        pieCamRef_ = camPos_; pieCamRefYaw_ = yaw_; pieCamRefPitch_ = pitch_;
        return;
    }
    if (pieCamFrame_ < 40) {
        pieCamMaxPos_   = std::fmax(pieCamMaxPos_,   (camPos_ - pieCamRef_).size());
        pieCamMaxYaw_   = std::fmax(pieCamMaxYaw_,   std::fabs(yaw_   - pieCamRefYaw_));
        pieCamMaxPitch_ = std::fmax(pieCamMaxPitch_, std::fabs(pitch_ - pieCamRefPitch_));
        return;
    }

    // ---- phase 2: one look, then settle. ----
    if (pieCamFrame_ == 40) { pieCamPendingLook_ = true; return; }
    if (pieCamFrame_ == 41) return;                    // the look lands in the fly block here
    if (pieCamFrame_ == 42) {
        // DID THE LOOK SURVIVE THE ROUND TRIP? drivePlayCamera recomputes yaw_/pitch_ from the
        // possessed pawn every frame, so if nothing wrote the look INTO the pawn the angles are
        // simply restored and a drift check re-baselined afterwards reports a steady camera that ignores every input -- exactly what this test's first version did.
        pieCamKeptYaw_   = std::fabs(yaw_   - pieCamWantYaw_)   < 1e-3f;
        pieCamKeptPitch_ = std::fabs(pitch_ - pieCamWantPitch_) < 1e-3f;
        pieCamRef_ = camPos_;
        return;
    }

    // ---- phase 3: hold W, and check the camera actually travels. ----
    // THROUGH THE REAL KEY PATHS, not by shortcutting to the movement maths: a private flag would
    // pass while the path a hand actually uses stayed broken. Both are re-asserted every frame the
    // key should be held, not pressed once.
    //
    // BOTH READERS, BECAUSE ONE PHYSICAL W FEEDS BOTH. Which path carries this test's W depends
    // entirely on the project: with no GameMode the editor's own fly block moves the possessed
    // default pawn and asks ImGui::IsKeyDown(ImGuiKey_W) (sandbox/src/SandboxApp.cpp:2206-2211),
    // while with a real GameMode the pawn is moved by gameplay reading the published AVER_FW_KEY_W
    // slot -- which, since pushInput started calling game::publishInput, comes from input_ and no
    // longer from ImGui. A real keypress reaches both readers off the one Win32 stream
    // (--input-source-test below asserts exactly that, "one stream, two readers"), so the
    // synthetic one has to as well or this test covers whichever project it was last run against.
    //
    // THIS IS NOT A FIX FOR THE MOVE PHASE'S CURRENT FAILURE, and must not be read as one. The W
    // was already reaching the named slot through ImGui before this commit, so "the named slot was
    // fed from the wrong place" cannot be why the phase reports 0.0 cm travelled; all this does is
    // keep it reaching that slot now that the slot is fed from input_ instead. The open lead is
    // the HARNESS, not the publisher: measured 2026-09-19, --input-stuck-test passes on PTTest
    // unbounded and fails on the same project under --frames 900, and this test prints "THE VIEW
    // IGNORES W" in exactly those bounded runs. Whatever --frames costs these drivers has not been
    // pinned to a line, so run this one WITHOUT --frames before believing either result.
    if (pieCamFrame_ >= 50 && pieCamFrame_ < 90) {
        io.AddKeyEvent(ImGuiKey_W, true);
        injectKey(input_, 'W', true);          // VK_W is the ASCII code; see InputKeys.hpp
        if (pieCamFrame_ == 50) pieCamMoveFrom_ = camPos_;
        return;
    }
    if (pieCamFrame_ == 90) {
        io.AddKeyEvent(ImGuiKey_W, false);
        injectKey(input_, 'W', false);
        pieCamMoved_    = (camPos_ - pieCamMoveFrom_).size();
        // ALONG THE VIEW, not merely somewhere. A pawn shoved by gravity or by a stray physics
        // impulse would also register distance; only a forward component proves it was W.
        const Vec3 d = (camPos_ - pieCamMoveFrom_).getSafeNormal();
        pieCamMoveDot_  = dot(d, camForward());
        return;
    }
    // ---- phase 4: released. It must COAST TO A STOP, not keep going. ----
    // BASELINED TWO FRAMES AFTER THE RELEASE IS ASKED FOR, not slack: events queue and apply at
    // the next NewFrame, so a release requested on frame 90 is still down for 90 and only clears
    // on 91. Baselining at 90 counted one more legitimate frame as "still moving" -- 6.89cm coast
    // against 6.7cm/frame of real motion, the same number wearing a disguise. The ruler started early.
    if (pieCamFrame_ == 92) { pieCamAfterMove_ = camPos_; return; }
    if (pieCamFrame_ > 92)
        pieCamCoast_ = std::fmax(pieCamCoast_, (camPos_ - pieCamAfterMove_).size());

    if (pieCamFrame_ >= 92 + pieCamFrames_) {
        const bool idleOk  = pieCamMaxPos_ < 0.5f && pieCamMaxYaw_ < 1e-3f && pieCamMaxPitch_ < 1e-3f;
        const bool lookOk  = pieCamKeptYaw_ && pieCamKeptPitch_;
        const bool moveOk  = pieCamMoved_ > 1.0f && pieCamMoveDot_ > 0.9f;
        const bool stopOk  = pieCamCoast_ < 0.5f;
        AVER_INFO("[pie-camera] idle  : max drift pos {:.4f} cm, yaw {:.6f}, pitch {:.6f} -- {}",
                  pieCamMaxPos_, pieCamMaxYaw_, pieCamMaxPitch_, idleOk ? "STEADY" : "DRIFTING");
        AVER_INFO("[pie-camera] look  : +0.5 yaw / +0.3 pitch -> yaw {}, pitch {} -- {}",
                  pieCamKeptYaw_ ? "kept" : "REVERTED", pieCamKeptPitch_ ? "kept" : "REVERTED",
                  lookOk ? "the view answers the mouse" : "THE VIEW IGNORES THE MOUSE");
        AVER_INFO("[pie-camera] move  : W held 40 frames -> travelled {:.1f} cm, {:.3f} along the "
                  "view -- {}", pieCamMoved_, pieCamMoveDot_,
                  moveOk ? "the view answers W" : "THE VIEW IGNORES W");
        AVER_INFO("[pie-camera] release: drift after W let go {:.4f} cm -- {}", pieCamCoast_,
                  stopOk ? "stops" : "STILL MOVING (the key is stuck or nothing clears it)");
        AVER_INFO("[pie-camera] RESULT: {}",
                  (idleOk && lookOk && moveOk && stopOk) ? "PASS" : "FAIL");
        stopPlay();
        pieCamFrames_ = 0;
    }
#endif
}

// --input-source-test N: does the editor's own InputState see the real OS event stream, and does
// ImGui still see it too?
// WHAT IT ACTUALLY GUARDS: the editor's Window::setEventCallback and ImGui's Window::messageHook
// coexist only because imgui_impl_win32's handler returns 0 (not consumed) for every relevant
// message, letting it fall through to Window::dispatch() too. That's vendored third-party
// behaviour an ImGui upgrade could silently change with no compile error -- a comment cannot catch
// that, this can.
// WHY IT POSTS REAL WINDOW MESSAGES: io.AddKeyEvent injects directly into ImGui's queue, never
// reaching Win32Window::dispatch(), so it can't tell whether InputState received anything. The MCP
// bridge's PostMessageW is the only synthesis here that enters the real pump.
void SandboxApp::maybeInputSourceTest() {
    if (inputSrcFrames_ <= 0) return;
#if defined(_WIN32) && AVER_WITH_IMGUI
    ++inputSrcFrame_;
    HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
    if (!hwnd) { if (inputSrcFrame_ > 4) { AVER_INFO("[input-source] RESULT: SKIPPED (no window)"); inputSrcFrames_ = 0; } return; }

    const int   kVk = 0x57;              // VK_W
    const int   half = inputSrcFrames_;  // frames spent held, then the same again released

    if (inputSrcFrame_ == 5)        { ::PostMessageW(hwnd, WM_KEYDOWN, (WPARAM)kVk, 0); return; }
    if (inputSrcFrame_ == 6 + half) { ::PostMessageW(hwnd, WM_KEYUP,   (WPARAM)kVk, 0); return; }
    if (inputSrcFrame_ < 7) return;

    // Two frames of slack after each post: PostMessageW queues, pumpEvents delivers on the next
    // turn of the loop, and ImGui applies queued key events at its own NewFrame. Sampling the
    // frame after a post would be reading the answer before it was written.
    const bool inState = input_.keyHeld(kVk);
    const bool inImGui = ImGui::IsKeyDown(ImGuiKey_W);
    const bool wantDown = inputSrcFrame_ < 6 + half;
    if (inputSrcFrame_ >= 7 && inputSrcFrame_ != 6 + half + 1) {
        if (inState) inputSrcStateSaw_ = true;
        if (inImGui) inputSrcImguiSaw_ = true;
        if (inState != inImGui) ++inputSrcDisagree_;
        if (!wantDown && inputSrcFrame_ > 6 + half + 2 && (inState || inImGui)) ++inputSrcStuck_;
    }

    if (inputSrcFrame_ >= 6 + half * 2) {
        const bool stateOk = inputSrcStateSaw_;
        const bool imguiOk = inputSrcImguiSaw_;
        const bool agreeOk = inputSrcDisagree_ == 0;
        const bool clearOk = inputSrcStuck_ == 0;
        AVER_INFO("[input-source] InputState saw the OS key: {} -- {}", inputSrcStateSaw_ ? "yes" : "NO",
                  stateOk ? "setEventCallback is wired and receiving" : "THE EDITOR NEVER GOT THE EVENT");
        AVER_INFO("[input-source] ImGui still saw it too : {} -- {}", inputSrcImguiSaw_ ? "yes" : "NO",
                  imguiOk ? "the two do not compete" : "IMGUI STOPPED SEEING INPUT");
        AVER_INFO("[input-source] frames they disagreed  : {} -- {}", inputSrcDisagree_,
                  agreeOk ? "one stream, two readers" : "THE TWO VIEWS OF THE KEYBOARD HAVE DIVERGED");
        AVER_INFO("[input-source] held after WM_KEYUP    : {} -- {}", inputSrcStuck_,
                  clearOk ? "released" : "STILL HELD");
        AVER_INFO("[input-source] RESULT: {}",
                  (stateOk && imguiOk && agreeOk && clearOk) ? "PASS" : "FAIL");
        inputSrcFrames_ = 0;
    }
#endif
}

// --wheel-speed-test: does turning the wheel while flying actually change the fly speed?
//
// THIS EXISTS BECAUSE THE CONTROL WAS DEAD AND NOTHING NOTICED. The wheel read sat in onUpdate,
// which Engine::frameStep runs BEFORE ImGui's NewFrame, and ImGui::EndFrame zeroes io.MouseWheel
// at the tail of the previous frame -- so the read was 0.0f on every frame by construction, from
// the day it was written. It survived a rewrite of its own direction and a commit message
// describing how it behaved. A control with no headless witness is a control that can be dead
// for months while everyone reads the source and agrees it looks right.
//
// REAL WINDOW MESSAGES, for the same reason maybeInputSourceTest posts them: the whole defect
// lives in WHICH input path is read and WHEN, so a harness that injects straight into ImGui's
// queue (or straight into InputState) would pass while the editor stayed broken. Only a message
// through the real pump exercises the ordering that was wrong.
void SandboxApp::maybeWheelSpeedTest() {
    if (wheelTestFrames_ <= 0) return;
#if defined(_WIN32) && AVER_WITH_IMGUI
    ++wheelTestFrame_;
    HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
    if (!hwnd) {
        if (wheelTestFrame_ > 4) { AVER_INFO("[wheel-test] RESULT: SKIPPED (no window)"); wheelTestFrames_ = 0; }
        return;
    }
    // Both sources sampled EVERY frame, because the contrast is the evidence: the fix is not
    // "the speed moved", it is "the value this function reads is the live one".
    wheelTestSawImGui_ = wheelTestSawImGui_ || ImGui::GetIO().MouseWheel != 0.0f;
    wheelTestSawInput_ = wheelTestSawInput_ || input_.wheel() != 0.0f;
    // The gates between the wheel arriving and the speed changing, sampled too -- without these
    // a FAIL cannot be told apart from "the harness never reached the code".
    if (input_.wheel() != 0.0f) {
        wheelTestOwnAtWheel_ = own_.keyboardToTool || own_.mouseToTool;
        wheelTestFlyAtWheel_ = flying_;
    }

    if (wheelTestFrame_ == 5) {
        wheelTestSpeedBefore_ = flySpeed_;
        // Straight into fly mode rather than synthesising a right-drag: entering it needs the
        // pointer inside the viewport rect and levelHovered_ set by the docked window, neither of
        // which a posted WM_MOUSEMOVE reliably reproduces headlessly. What is under test is the
        // wheel READ, not the gesture that opens it, so forcing the state keeps the harness
        // measuring the thing that was broken instead of the thing that was not.
        wheelTestForceFly_ = true;
        return;
    }
    // One notch UP, which is the direction that speeds up. Positive WHEEL_DELTA is up; the
    // test posts the real message so it exercises the same path a hand does, sign included --
    // which is exactly what would catch this flipping back by accident.
    if (wheelTestFrame_ == 6) {
        ::PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        return;
    }
    // Slack for the same reason maybeInputSourceTest leaves it: PostMessageW queues, pumpEvents
    // delivers on the next turn of the loop. Sampling sooner reads the answer before it is written.
    if (wheelTestFrame_ < 9) return;

    wheelTestForceFly_ = false;
    const f32 after = flySpeed_;
    const f32 want = wheelTestSpeedBefore_ * 1.25f;
    const bool moved = std::fabs(after - wheelTestSpeedBefore_) > 0.01f;
    const bool right = std::fabs(after - want) < std::fmax(1.0f, want * 0.02f);
    AVER_INFO("[wheel-test] speed {:.1f} -> {:.1f} cm/s (one notch up, want {:.1f})",
              wheelTestSpeedBefore_, after, want);
    AVER_INFO("[wheel-test] sources during onUpdate: io.MouseWheel seen={} input_.wheel() seen={}",
              wheelTestSawImGui_ ? "yes" : "NO (always zero -- this is the bug)",
              wheelTestSawInput_ ? "yes" : "NO");
    AVER_INFO("[wheel-test] gates on the wheel frame: inputOwnedByTool={} flying={}",
              wheelTestOwnAtWheel_ ? "yes" : "NO", wheelTestFlyAtWheel_ ? "yes" : "NO");
    AVER_INFO("[wheel-test] RESULT: {}", (moved && right) ? "PASS"
                                       : moved ? "FAIL (speed moved, but not by one notch)"
                                               : "FAIL (speed did not change at all)");
    wheelTestFrames_ = 0;
#else
    AVER_INFO("[wheel-test] RESULT: SKIPPED (needs Win32 + ImGui)");
    wheelTestFrames_ = 0;
#endif
}

// --viewmodel-test N: does the gun sit STILL in the frame, or does it swim against the camera?
// THE INVARIANT: the viewmodel is rigidly parented to the eye, so its position in VIEW SPACE is
// constant by construction; any deviation means the gun and camera transforms were derived from
// different data -- what "the gun jitters" looks like.
// WHY VIEW SPACE AND NOT WORLD: the gun moves constantly and correctly in world space, so only
// relative-to-eye motion is the question.
// TWO PHASES, because the suspected cause only shows in one: drivePlayCamera derives
// camPos_/yaw_/pitch_ and rebuilds a basis from those, while the gun draws from the view entity's
// matrix directly -- a still camera and a turning one are measured separately, and the DIFFERENCE
// is the finding.
void SandboxApp::maybeViewmodelTest() {
    if (vmFrames_ <= 0) return;
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE && AVER_WITH_IMGUI
    ++vmFrame_;
    if (vmFrame_ == 10) { startPlay(); return; }
    if (vmFrame_ < 20) return;

    scene::World& w = scene::World::instance();
    const int32_t viewId = aver_fw_view_entity();
    const scene::Entity ve = static_cast<scene::Entity>(static_cast<uint32_t>(viewId));
    if (!w.valid(ve)) return;

    // The gun is the view entity's mesh-bearing child. Found by walking rather than remembered,
    // so the probe cannot go stale against a graph that spawns it differently.
    scene::Entity gun = scene::kInvalidEntity;
    vmKids_ = 0;
    const u32 vmN = w.count();
    for (u32 k = 0; k < vmN; ++k) {
        const scene::Entity c = w.at(k);
        if (!w.valid(c) || w.parent(c) != ve) continue;
        if (w.component<scene::CMeshRenderer>(c, scene::kComponentMeshRenderer)) { if (!w.valid(gun)) gun = c; ++vmKids_; }
    }
    if (!w.valid(gun)) { if (vmFrame_ == 25) AVER_INFO("[viewmodel] no viewmodel found under the view entity"); return; }

    const Mat4& gm = w.worldMatrix(gun);
    const Vec3 gunPos{gm.m[3][0], gm.m[3][1], gm.m[3][2]};

    // The exact basis the renderer uses: camForward() from yaw_/pitch_, not the view entity's
    // own axes. Using the entity's axes here would compare the gun against itself and always
    // report zero -- the whole point is to test the DECOMPOSED path the camera actually takes.
    const Vec3 fwd = camForward();
    const Vec3 wup{0, 0, 1};
    const Vec3 rgt = Vec3{fwd.y * wup.z - fwd.z * wup.y, fwd.z * wup.x - fwd.x * wup.z,
                          fwd.x * wup.y - fwd.y * wup.x}.getSafeNormal();
    const Vec3 up2{rgt.y * fwd.z - rgt.z * fwd.y, rgt.z * fwd.x - rgt.x * fwd.z,
                   rgt.x * fwd.y - rgt.y * fwd.x};
    const Vec3 rel = gunPos - camPos_;
    const Vec3 vs{rel.x * rgt.x + rel.y * rgt.y + rel.z * rgt.z,
                  rel.x * up2.x + rel.y * up2.y + rel.z * up2.z,
                  rel.x * fwd.x + rel.y * fwd.y + rel.z * fwd.z};

    // ---- phase 1: dead still ----
    // NOTHING IS MEASURED BEFORE THE REFERENCE EXISTS. The first version accumulated from frame
    // 20 while vmRef_ was still zero-initialised, comparing the gun against the ORIGIN and
    // reporting 39.56cm as drift -- a probe that fails loudly on a rigid viewmodel, sending the search somewhere real code is fine.
    if (vmFrame_ <= 25) { vmRef_ = vs; return; }
    if (vmFrame_ < 25 + vmFrames_) {
        vmStill_ = std::fmax(vmStill_, (vs - vmRef_).size());
        return;
    }
    // ---- phase 2: turning, one steady look per frame ----
    if (vmFrame_ <= 25 + vmFrames_) { vmRef_ = vs; return; }
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(vpX_ + vpW_ * 0.5f + (f32)(vmFrame_ % 20) * 2.0f, vpY_ + vpH_ * 0.5f);
    vmMoving_ = std::fmax(vmMoving_, (vs - vmRef_).size());

    if (vmFrame_ >= 25 + vmFrames_ * 2) {
        const bool stillOk  = vmStill_  < 0.05f;
        const bool moveOk   = vmMoving_ < 0.05f;
        AVER_INFO("[viewmodel] offset from the eye: ({:.3f}, {:.3f}, {:.3f}) cm right/up/forward",
                  vmRef_.x, vmRef_.y, vmRef_.z);
        AVER_INFO("[viewmodel] still  : drifts {:.4f} cm in view space -- {}", vmStill_,
                  stillOk ? "rigid" : "IT MOVES WITH NOBODY TOUCHING ANYTHING");
        AVER_INFO("[viewmodel] turning: drifts {:.4f} cm in view space -- {}", vmMoving_,
                  moveOk ? "rigid" : "IT SWIMS AGAINST THE CAMERA WHILE TURNING");
        AVER_INFO("[viewmodel] RESULT: {}", (stillOk && moveOk) ? "PASS" : "FAIL");
        stopPlay();
        vmFrames_ = 0;
    }
#endif
}

// --recapture-test N: does the click that takes the mouse BACK also fire the weapon?
// THE GESTURE: the release-mouse chord then a click back into the viewport means "give the game
// the mouse again" and must not ALSO reach gameplay -- in PTTest it did, since the fire gate is a
// LEVEL read of InputKey(MOUSE_LEFT) behind a cooldown.
// WHY --input-stuck-test DOES NOT COVER IT: that holds then releases; this releases then clicks --
// a live key arriving through a gesture never meant to be gameplay input.
void SandboxApp::maybeRecaptureTest() {
    if (recapFrames_ <= 0) return;
#if AVER_MODULE_FRAMEWORK && AVER_WITH_IMGUI
    ++recapFrame_;
    if (recapFrame_ == 10) { startPlay(); return; }
    if (recapFrame_ < 15) return;

    ImGuiIO& io = ImGui::GetIO();
    // Park the pointer in the middle of the 3D view for the whole test: the recapture path is
    // gated on inViewport(), so a click anywhere else would prove nothing either way.
    io.AddMousePosEvent(vpX_ + vpW_ * 0.5f, vpY_ + vpH_ * 0.5f);

    // Hand the mouse to the editor, exactly as the chord does.
    if (recapFrame_ == 15) { releasedByUser_ = true; return; }
    if (recapFrame_ < 20) return;

    // ...then click back into the viewport and HOLD. A real click lasts many frames; the bug
    // needs only the frames after the first, once own_ has refreshed with releasedByUser_ false
    // and mouse_.captured() true.
    //
    // INTO BOTH READERS, AND NEITHER HALF IS OPTIONAL HERE. The gesture under test is decided on
    // ImGui's side -- the recapture branch asks ImGui::IsMouseClicked(0) before it clears
    // releasedByUser_ and arms eatRecaptureClick_ (sandbox/src/SandboxApp.cpp:2324-2331) -- while
    // the LEAK this test counts is read back off the published AVER_FW_KEY_MOUSE_LEFT slot, which
    // pushInput now fills from input_ rather than from ImGui. Drop the ImGui half and no recapture
    // ever happens, so there is nothing to leak from; drop the input_ half and no button is ever
    // published, so a leak could not show up even if the eat were removed. A real click reaches
    // both off one Win32 stream, which is what makes injecting into both faithful rather than a
    // way of making the test agree with itself.
    io.AddMouseButtonEvent(0, true);
    injectMouseButton(input_, 0, true);
    // The recapture itself takes a frame or two to land -- the button event queues and applies
    // at ImGui's next NewFrame, and the capture block reads it the frame after. The leak is
    // counted only ONCE THE MOUSE IS ACTUALLY BACK, the window the bug lives in.
    if (!releasedByUser_) recapNotRecaptured_ = 1;          // 1 == "it did take the mouse back"
    if (recapNotRecaptured_ && aver_fw_input_key(AVER_FW_KEY_MOUSE_LEFT)) ++recapLeaked_;

    if (recapFrame_ >= 21 + recapFrames_) {
        const bool tookBack = recapNotRecaptured_ != 0;
        const bool clean    = recapLeaked_ == 0;
        // NOT-RECAPTURED IS A FINDING, NOT A FAILURE: the recapture branch requires
        // !WantCaptureMouse, and ImGui ALWAYS wants the mouse over the 3D dock window (measured
        // here), so it can't be taken -- clicking back into the viewport does nothing today, its
        // own bug and the REASON the weapon-fire defect cannot currently happen. The leak assertion
        // stays meaningful for whoever fixes the recapture gesture.
        AVER_INFO("[recapture] the click took the mouse back : {} -- {}",
                  tookBack ? "yes" : "no",
                  tookBack ? "the gesture works"
                           : "UNREACHABLE: the branch needs !WantCaptureMouse and the viewport is "
                             "an ImGui window, so it always wants the mouse");
        AVER_INFO("[recapture] frames it also reached the gun: {} of {} -- {}",
                  recapLeaked_, recapFrames_,
                  clean ? "no shot leaked" : "THE RECAPTURE CLICK IS FIRING THE WEAPON");
        AVER_INFO("[recapture] RESULT: {}",
                  !clean ? "FAIL" : (tookBack ? "PASS" : "PASS (recapture unreachable -- see above)"));
        io.AddMouseButtonEvent(0, false);
        injectMouseButton(input_, 0, false);   // both readers let go, for the reason both were told
        stopPlay();
        recapFrames_ = 0;
    }
#endif
}

// --input-stuck-test N: does releasing the mouse mid-session leave a key held forever?
// THE BUG: pushInput() used to `return` after aver_fw_input_new_frame(), which does NOT clear
// cur[] -- so every key froze at its last value while aver_fw_tick kept running, and a weapon
// gated on a LEVEL read of MOUSE_LEFT fired forever with the mouse untouched.
// IT ASSERTS EVERY KEY, NOT THE ONE IT PRESSED: the fix spans ~29 set_key call sites, and a test
// watching one slot would pass with 28 fixed and the 29th still latching.
// WHY IT DRIVES THE DEVICE STREAM RATHER THAN THE ABI: --play-test's aver_fw_input_set_key writes
// the very state the bug corrupts, so it cannot catch this; the injection has to enter upstream of
// pushInput, the function under test, and be carried by it.
// AND WHY THAT STREAM IS input_ AND NO LONGER ImGui. It was io.AddMouseButtonEvent while pushInput
// read ImGui; pushInput now hands game::publishInput the editor's policy and that publisher reads
// InputState, so an ImGui-only click stopped being on the path at all. Re-pointed rather than
// duplicated here, unlike --recapture-test just above, and the difference is worth stating: this
// test needs no ImGui-side gesture, and an ImGui-held button is actively in its way -- a held
// button makes ImGui report WantCaptureMouse, resolveInputOwnership then denies the mouse to the
// game unless mouse_.captured() overrides it, and setMouseCaptured is skipped in a bounded run
// (sandbox/src/SandboxApp.cpp:2311 and :2357), which is phase 1 failing on the test's own injection.
// STILL GUARDED ON AVER_WITH_IMGUI even though nothing in the body touches ImGui any more: own_ is
// only resolved in a run that has a UI context, and without one the publisher is correctly told
// that nobody owns the mouse -- phase 1 would report "no input arrived" and prove nothing, which is
// a worse outcome than not running.
void SandboxApp::maybeInputStuckTest() {
    if (inputStuckFrames_ <= 0) return;
#if AVER_MODULE_FRAMEWORK && AVER_WITH_IMGUI
    ++inputStuckFrame_;
    if (inputStuckFrame_ == 10) { startPlay(); return; }
    if (inputStuckFrame_ < 11) return;

    // ---- phase 1: hold the button and prove it actually reaches gameplay ----
    // Re-asserted every frame rather than pressed once: the point is that the BUTTON never goes
    // up, so a test that stopped saying so would be testing the wrong thing. (Re-asserting a hold
    // is also free -- InputState::onEvent only counts the 0->1 transition as a press, exactly so
    // Windows' own key auto-repeat cannot read as a fresh tap.)
    if (inputStuckFrame_ < 30) {
        injectMouseButton(input_, 0, true);
        if (inputStuckFrame_ > 12 && aver_fw_input_key(AVER_FW_KEY_MOUSE_LEFT)) inputStuckSawDown_ = true;
        return;
    }

    // ---- phase 2: release the mouse to the editor, WITHOUT letting the button up ----
    // Exactly what a person does mid-session to go and click something in the Outliner, and the
    // precise moment the old code stopped publishing.
    if (inputStuckFrame_ == 30) { injectMouseButton(input_, 0, true); releasedByUser_ = true; return; }

    // ---- phase 3: from here on gameplay must see NOTHING held ----
    injectMouseButton(input_, 0, true);   // still physically down; still must not reach the game
    for (int k = 0; k < AVER_FW_KEY_COUNT; ++k) {
        if (aver_fw_input_key(k)) { ++inputStuckLatched_; if (inputStuckFirstKey_ < 0) inputStuckFirstKey_ = k; }
    }

    if (inputStuckFrame_ >= 30 + inputStuckFrames_) {
        const bool downOk  = inputStuckSawDown_;
        const bool clearOk = inputStuckLatched_ == 0;
        AVER_INFO("[input-stuck] press  : MOUSE_LEFT held -> gameplay {} -- {}",
                  downOk ? "saw it" : "NEVER SAW IT",
                  downOk ? "the button reaches the game" : "THE TEST PROVED NOTHING (no input arrived)");
        AVER_INFO("[input-stuck] release: {} key-frames still held across {} frames after the "
                  "release chord, first offender key {} -- {}",
                  inputStuckLatched_, inputStuckFrames_, inputStuckFirstKey_,
                  clearOk ? "nothing latched" : "A KEY IS STUCK (pushInput returned without publishing)");
        AVER_INFO("[input-stuck] RESULT: {}", (downOk && clearOk) ? "PASS" : "FAIL");
        // LET GO BEFORE HANDING THE EDITOR BACK. Held state is the one thing InputState::newFrame
        // deliberately does not roll, so a synthetic button left down here stays down for the rest
        // of the process and the next thing to read input_ inherits it.
        injectMouseButton(input_, 0, false);
        releasedByUser_ = false;
        stopPlay();
        inputStuckFrames_ = 0;
    }
#endif
}

// Runs the --play-test session: begins play, drives synthetic input for 150 frames, then stops.
void SandboxApp::maybePlayTest() {
    if (!playTest_) return;
    if (!playTestBegun_) {
        // NO GameMode IS A CASE WORTH TESTING, NOT A REASON TO GIVE UP: it used to abandon the
        // run, leaving the single most broken Play path -- the one that used to possess a
        // stationary drone and strand the camera -- with no automated coverage. This drives it too.
        if (aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE) == 0) {
            if (++playTestWait_ <= 10) return;   // still give classes a moment to register
            playTestBegun_ = true;
            AVER_INFO("[play-test] no GameMode after 10 frames -- exercising the engine default "
                      "pawn fallback instead");
            startPlay();
            return;
        }
        playTestBegun_ = true;
        AVER_INFO("[play-test] starting - watch for GameMode/Controller/Pawn OnBeginPlay + Pawn OnTick");
        startPlay();
        return;
    }
    // SYNTHETIC INPUT ONLY WHERE THERE IS SOMETHING TO RECEIVE IT, but the COUNTDOWN runs either
    // way: it used to live entirely inside this branch, so the spectator fallback (which never
    // calls begin_play, so never reports PLAYING) started and ran forever without reaching Stop.
    const bool fwPlaying = aver_fw_play_state() == AVER_FW_PLAY_PLAYING;
    if (fwPlaying) {
        aver_fw_input_set_key(AVER_FW_KEY_W, 1);
        if (playTestFrames_ > 60) aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT, 1);
        if (playTestFrames_ == 100) aver_fw_input_set_key(AVER_FW_KEY_SPACE, 1);
    }
    {
        if (++playTestFrames_ == 150) {
            const int32_t pawn = fwPlaying ? aver_fw_controlled_pawn(aver_fw_player_controller(0)) : 0;
            if (pawn) {
                const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
                const Mat4& wm = scene::World::instance().worldMatrix(pe);
                AVER_INFO("[play-test] character walked to ({:.1f}, {:.1f}, {:.1f}) under synthetic W (spawned at origin)",
                          wm.m[3][0], wm.m[3][1], wm.m[3][2]);
            }
            AVER_INFO("[play-test] stopping - watch for OnEndPlay(reason=Stop) lines");
            // stopPlay(), NOT aver_fw_end_play() DIRECTLY: calling the framework straight
            // through skipped everything the Stop BUTTON does (taking down a play-started drone,
            // restoring the level's transforms), so the one automated run exercised a path no user
            // can take. A harness that bypasses the button does not test the button.
            stopPlay();
        }
    }
}

#endif

#if AVER_WITH_IMGUI
// --undo-test [N]: a headless, in-process proof that Copy/Paste/Duplicate/Delete/Undo/Redo
// actually work at runtime against a REAL scene::World and objects_ vector, printing one PASS/FAIL
// line per assertion and exiting 0/1 -- never returns, so it never leaves a window open past the
// check.
// Two phases: A forces the scene-entity branch (hideEditorScene_=true); B forces the placeholder-
// object branch (no level) against the SAME six commands -- the path that, before this change,
// silently pushed no undo entry for Create/Destroy at all. B runs even with SCENE off.
// --multiselect-test: drives the multi-selection MODEL directly and asserts its semantics.
//
// THE MODEL, NOT THE CLICKS. Shift and Ctrl reach it through the outliner's ImGui tree, which a
// headless run has no way to click; what CAN be checked headlessly is the part that actually
// decides behaviour -- what the set contains after each gesture, and above all the staleness
// rule, which is the subtle one. multiStale() exists so that ~40 places that assign selEntity_
// directly collapse the selection instead of leaving a set the user cannot see; if that ever
// silently stops working, Delete starts removing things nobody highlighted. That is worth a
// witness, and this file has learned twice today what an unwitnessed control costs.
// --cbmove-test <dir>: exercises the Content Browser's copy/move against a scratch directory.
//
// THE FILE OPERATIONS, NOT THE DRAG. Dropping onto a folder is an ImGui gesture no headless run
// can perform; what can be checked is the part that actually touches the disk, which is also the
// part that can lose somebody's work. Untested destructive file operations are not a thing to
// ship into a project full of assets.
//
// WRITES ONLY UNDER THE DIRECTORY IT IS GIVEN, and creates its own fixture there.
void SandboxApp::runCbMoveTest(const std::string& root) {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[cbmove-test] PASS: {}", what);
        else      { AVER_ERROR("[cbmove-test] FAIL: {}", what); ++failures; }
    };
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path base = fs::path(root) / "cbmove";
    fs::remove_all(base, ec);
    const fs::path src = base / "src";
    const fs::path dst = base / "dst";
    const fs::path sub = src / "sub";
    fs::create_directories(sub, ec);
    fs::create_directories(dst, ec);
    auto put = [&](const fs::path& p, const char* text) {
        std::ofstream f(p, std::ios::binary); f << text;
    };
    put(src / "a.ocmesh", "A");
    put(src / "b.ocmat", "B");
    put(sub / "inner.txt", "I");

    // ---- copy leaves the original ----
    check(cbCopyEntryTo((src / "a.ocmesh").string(), dst.string()), "copy reports success");
    check(fs::exists(dst / "a.ocmesh", ec), "the copy landed in the destination");
    check(fs::exists(src / "a.ocmesh", ec), "and the original is still there");

    // ---- a collision is refused, not overwritten ----
    check(!cbCopyEntryTo((src / "a.ocmesh").string(), dst.string()),
          "copying over an existing name is REFUSED");
    {
        std::ifstream f(dst / "a.ocmesh", std::ios::binary);
        std::string got; f >> got;
        check(got == "A", "and the existing file was not clobbered");
    }

    // ---- move takes the original with it ----
    check(cbMoveEntryTo((src / "b.ocmat").string(), dst.string()), "move reports success");
    check(fs::exists(dst / "b.ocmat", ec), "the moved file is in the destination");
    check(!fs::exists(src / "b.ocmat", ec), "and is GONE from the source");

    // ---- a folder copies whole ----
    check(cbCopyEntryTo(sub.string(), dst.string()), "a folder copies");
    check(fs::exists(dst / "sub" / "inner.txt", ec), "with its contents");

    // ---- the containment primitive the cycle guard rests on ----
    check(cbIsUnder(sub.string(), src.string()), "a child is under its parent");
    check(cbIsUnder(src.string(), src.string()), "a folder is under itself");
    check(!cbIsUnder(src.string(), sub.string()), "a parent is NOT under its child");
    check(!cbIsUnder(dst.string(), src.string()), "unrelated folders are not related");

    // ---- and the one that stops a double move ----
    {
        const std::vector<std::string> in = {src.string(), (sub / "inner.txt").string()};
        const std::vector<std::string> out = cbPruneNested(in);
        check(out.size() == 1 && out[0] == src.string(),
              "dragging a folder AND something inside it moves only the folder");
    }

    fs::remove_all(base, ec);
    AVER_INFO("[cbmove-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

// --savedirty-test: does "unsaved changes" actually track the file?
//
// THE BUG THIS PINS: levelHasUnsavedEdits() was `return canUndo()`, so the exit prompt fired on
// every close no matter how recently the level had been written -- it could only go quiet by
// undoing the entire session. A prompt that cries wolf every time trains the reflex that
// dismisses it, and one day it is telling the truth.
//
// THE CASES THAT MATTER ARE THE UNDO ONES. Anyone can make "save clears the flag" work; the
// interesting question is what happens when you save, undo past that point, and redo back to it.
// A stack-DEPTH watermark gets that wrong the moment pushEdit trims from the front at
// kUndoDepth; a per-edit serial does not, and that is the whole reason for the serial.
// --prefs-write-test: does a preference actually reach disk?
//
// editor.ini has not been written since 15:53 on the day this was added, across many sessions --
// so "preferences do not save" is a WRITE failing, not merely nothing having been dirtied. Every
// layer of that write reads correctly (setPrefString dirties on a real change, flushEditorPrefs
// writes through writeFileTextAtomic and only clears the dirty bit on success, the atomic write
// is write-temp-then-MoveFileEx). Reading further was not going to settle it, so this reports
// each step against the real file instead.
// Raises one notification of each severity, plus a progress one, so the stack's appearance can
// be screenshotted. This is the half of the feature a headless test genuinely cannot judge:
// EditorNotificationsTest owns every rule about WHEN a notification lives and dies, and nothing
// but a picture can say whether the result is legible.
void SandboxApp::runNotifyTest() {
    editor::NotificationQueue& q = editor::notifications();
    editor::Notification n;

    n = {}; n.severity = editor::NotifySeverity::Info;
    n.title = "Info"; n.body = "A routine outcome worth mentioning once.";
    n.ttlSec = 1.0e6; q.push(n);

    n = {}; n.severity = editor::NotifySeverity::Success;
    n.title = "Imported Rock_01.fbx"; n.body = "3 meshes, 1 skeleton, 2 clips.";
    n.ttlSec = 1.0e6; q.push(n);

    n = {}; n.severity = editor::NotifySeverity::Warning;
    n.title = "Already imported"; n.body = "Content/Props/Rock_01.ocmesh exists.";
    n.ttlSec = 1.0e6; q.push(n);

    // THROUGH THE LOG, not pushed directly, and that is the point of this one: it is the only
    // sample that exercises the production path -- logSink -> pushFromLog -> the queue -- so the
    // capture proves the wiring rather than just the drawing. Safe to log here because
    // runNotifyTest is called from the frame tick, not from inside the sink.
    //
    // Error and not Critical: AVER_CRITICAL wakes the crash reporter, and a diagnostic flag has
    // no business launching a second process. The Critical sample below is pushed directly for
    // exactly that reason.
    AVER_ERROR("[Import] could not read Textures/missing.png");

    n = {}; n.severity = editor::NotifySeverity::Critical;
    n.title = "Critical - RHI.D3D12"; n.body = "THE GPU DEVICE HAS BEEN LOST";
    n.sticky = true;
    n.actions[0] = editor::NotifyAction::ShowOutputLog; n.actionLabels[0] = "Show in Output Log";
    n.actions[1] = editor::NotifyAction::Dismiss;       n.actionLabels[1] = "Dismiss";
    q.push(n);

    AVER_INFO("[notify-test] raised {} sample notification(s)", q.size());
}

void SandboxApp::runPrefsWriteTest() {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[prefs-write-test] PASS: {}", what);
        else      { AVER_ERROR("[prefs-write-test] FAIL: {}", what); ++failures; }
    };

    const std::string path = editor::editorPrefsPath();
    AVER_INFO("[prefs-write-test] the store says its path is: '{}'", path);
    check(!path.empty(), "the store has a path at all");
    if (path.empty()) { AVER_INFO("[prefs-write-test] RESULT: FAIL"); return; }

    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    const auto sizeBefore = existed ? std::filesystem::file_size(path, ec) : 0u;
    AVER_INFO("[prefs-write-test] before: exists={} size={}", existed ? 1 : 0,
              static_cast<unsigned long long>(sizeBefore));

    // A key nothing else uses, with a value that cannot already be there.
    const std::string sentinel = "diagnostic.writeProbe";
    editor::setPrefString(sentinel, "probe-2026");
    editor::flushEditorPrefs();

    // Read the FILE back, not the in-memory store -- the store would report success even if the
    // write never happened, which is precisely the failure being hunted.
    std::string text;
    const bool read = readFileText(path, text);
    check(read, "the file can be read back after the flush");
    check(read && text.find("diagnostic.writeProbe=probe-2026") != std::string::npos,
          "and the probe value is IN the file on disk");

    const auto sizeAfter = std::filesystem::exists(path, ec)
                         ? std::filesystem::file_size(path, ec) : 0u;
    AVER_INFO("[prefs-write-test] after:  size={}", static_cast<unsigned long long>(sizeAfter));

    // Also prove the directory is writable at all, independently of the prefs store, so a
    // failure can be told apart from a permissions problem.
    const std::string probe = std::filesystem::path(path).parent_path().string() + "/writeprobe.tmp";
    const bool direct = writeFileTextAtomic(probe, "x");
    check(direct, "writeFileTextAtomic can write to that directory directly");
    std::filesystem::remove(probe, ec);

    AVER_INFO("[prefs-write-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

void SandboxApp::runProjectSwitchTest() {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[project-switch-test] PASS: {}", what);
        else      { AVER_ERROR("[project-switch-test] FAIL: {}", what); ++failures; }
    };

    const fmt::ProjectDesc saved = project_;
    const std::string savedBackend = projectBackend_;
    const f32 savedBudget = frameBudgetMs_;
    const bool savedForced = frameBudgetForced_;
    frameBudgetForced_ = false;   // the manifest path, not the --frame-budget override path

    fmt::ProjectDesc a;
    a.name = "SwitchTestA";
    a.dir = executableDir();
    a.backend = "vulkan";
    a.frameBudgetMs = 8.0f;
    check(a.valid() && a.hasRenderSettings(), "project A is valid and states render settings");
    project_ = a;
    applyProjectRenderSettings();
    check(projectBackend_ == "vulkan", "opening A puts its backend in the dropdown mirror");
    check(frameBudgetMs_ == 8.0f, "opening A arms its frame budget");

    fmt::ProjectDesc b;
    b.name = "SwitchTestB";
    b.dir = executableDir();
    check(b.valid() && !b.hasRenderSettings(), "project B is valid and states NO render settings");
    project_ = b;
    applyProjectRenderSettings();
    check(projectBackend_.empty(),
          "opening B clears the backend mirror instead of inheriting A's vulkan");
    check(frameBudgetMs_ == b.frameBudgetMs,
          "opening B resets the frame budget instead of inheriting A's 8ms");

    project_ = saved;
    projectBackend_ = savedBackend;
    frameBudgetMs_ = savedBudget;
    frameBudgetForced_ = savedForced;
    applyProjectRenderSettings();

    AVER_INFO("[project-switch-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

void SandboxApp::runGraphPrintTest() {
    // NOTHING IS LOGGED WHILE logMutex_ IS HELD, and the first draft of this test got that
    // wrong. Log.hpp's contract says a sink "must not itself log": logSink takes logMutex_, so a
    // check() that logs from inside a lock_guard on it deadlocks against itself -- std::mutex is
    // not recursive. It presented as the test stopping dead after its first PASS with no error,
    // which is exactly what a self-deadlock looks like from outside.
    //
    // So every phase below reads what it needs under the lock into plain locals, releases, and
    // only then reports. The lambda is deliberately not called anywhere a lock is held.
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[graph-print-test] PASS: {}", what);
        else      { AVER_ERROR("[graph-print-test] FAIL: {}", what); ++failures; }
    };
    auto clear = [this] { std::lock_guard<std::mutex> lk(logMutex_); graphPrints_.clear(); };

    // The case that decides whether this feature is usable at all: a PrintString on an OnTick
    // chain fires EVERY FRAME once Play starts (tickGraphClassInstances is gated on
    // aver_fw_play_state() == AVER_FW_PLAY_PLAYING), and every frame in a packaged game -- which
    // is exactly when someone is watching this feed.
    //
    // (This said "the editor ticks graph instances ungated on play state". That was wrong, and it
    // is the SECOND copy of the same wrong sentence: the first, beside logSink, was corrected in
    // 1f9507e and this one was missed because the correction fixed the site it was reading rather
    // than grepping for the claim.)
    clear();
    for (int i = 0; i < 200; ++i) AVER_INFO("[Graph] hello: reached_the_tick");
    usize rows = 0; u32 count = 0; std::string text;
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        rows = graphPrints_.size();
        if (!graphPrints_.empty()) { count = graphPrints_.back().count; text = graphPrints_.back().text; }
    }
    check(rows == 1, "200 identical prints collapse to ONE row, not 200");
    check(count == 200, "and the row counts every one of them");
    check(text == "hello: reached_the_tick", "with the [Graph] prefix stripped and the rest kept verbatim");

    // ALTERNATING PRINTS MUST NOT COLLAPSE. A Branch taking each arm in turn is exactly the
    // pattern an author is watching for; merging those would hide the alternation that IS the
    // signal. Six lines, two distinct, alternating -> six rows.
    clear();
    for (int i = 0; i < 3; ++i) {
        AVER_INFO("[Graph] branch: took_true");
        AVER_INFO("[Graph] branch: took_false");
    }
    { std::lock_guard<std::mutex> lk(logMutex_); rows = graphPrints_.size(); }
    check(rows == 6, "alternating prints stay separate rows");

    // A non-graph line must not reach the feed at all, or the overlay becomes a second log.
    clear();
    AVER_INFO("[Renderer] this is not a graph print");
    AVER_INFO("[Graph] real: yes");
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        rows = graphPrints_.size();
        text = graphPrints_.empty() ? std::string() : graphPrints_.back().text;
    }
    check(rows == 1, "only the [Graph] line is picked up");
    check(text == "real: yes", "and it is the right one");

    // The ring cap holds, so a long session cannot grow this without bound.
    clear();
    for (int i = 0; i < static_cast<int>(kMaxGraphPrints) + 20; ++i)
        AVER_INFO("[Graph] n{}: distinct", i);
    { std::lock_guard<std::mutex> lk(logMutex_); rows = graphPrints_.size(); }
    check(rows == kMaxGraphPrints, "the feed is capped, oldest dropped");
    clear();

    AVER_INFO("[graph-print-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

void SandboxApp::runClearShaderCache() {
    std::filesystem::path dir;
    if (!clearShaderCacheDir_.empty()) {
        dir = clearShaderCacheDir_;
    } else {
        const std::string udir = userDataDir();
        if (udir.empty()) {
            AVER_ERROR("[shader-cache] no user data directory, so there is no cache to clear");
            return;
        }
        dir = std::filesystem::path(udir) / "ShaderCache";
    }
    const auto before = rhi::sweepShaderCache(dir, ~0ull);   // a budget nothing can exceed: measure only
    AVER_INFO("[shader-cache] {} holds {} blob(s), {:.1f} MB", dir.string(), before.filesRemaining,
              static_cast<f64>(before.bytesRemaining) / (1024.0 * 1024.0));
    const auto r = rhi::sweepShaderCache(dir, 0);
    AVER_INFO("[shader-cache] cleared {} blob(s), {:.1f} MB freed", r.filesRemoved,
              static_cast<f64>(r.bytesRemoved) / (1024.0 * 1024.0));
    // Named because it is the reassurance that matters: the cache lives under the user's own
    // data directory, and the sweeper touches only .dxil, so anything else there is still there.
    AVER_INFO("[shader-cache] only .dxil blobs were touched; the shaders recompile on next launch");
}

void SandboxApp::runValidateGraph() {
    // scripts_ is a scripting::ScriptHost, and that TYPE is declared `#if AVER_MODULE_SCRIPTING` in
    // SandboxApp.hpp -- there is no member to call graphValidateAvailable/graphValidate on at all in
    // a scripting-off tree, unlike AVER_WITH_IMGUI above, which this whole function already sits
    // inside and which proves nothing about AVER_MODULE_SCRIPTING (module-matrix.ps1's scripting-off
    // row leaves the UI on and only this module off). The function's only reason to exist is to
    // drive the bridge, so the guard covers the body rather than one call inside it.
#if AVER_MODULE_SCRIPTING
    if (!scripts_.graphValidateAvailable()) {
        AVER_ERROR("[validate-graph] the staged bridge exports no GraphValidate");
        return;
    }
    const std::string abs = project_.valid() && validateGraphPath_.find(':') == std::string::npos
        ? (project_.contentDir() + "\\" + validateGraphPath_) : validateGraphPath_;
    std::string text;
    if (!readFileText(abs, text)) {
        AVER_ERROR("[validate-graph] could not read '{}'", abs);
        return;
    }
    std::string err;
    if (scripts_.graphValidate(text, err))
        AVER_INFO("[validate-graph] '{}' is VALID", validateGraphPath_);
    else
        AVER_INFO("[validate-graph] '{}' is INVALID: {}", validateGraphPath_, err);
#else
    AVER_ERROR("[validate-graph] this build has no scripting module; nothing to validate against");
#endif
}

void SandboxApp::runRenameRepointTest() {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[rename-repoint-test] PASS: {}", what);
        else      { AVER_ERROR("[rename-repoint-test] FAIL: {}", what); ++failures; }
    };
    if (!project_.valid()) {
        AVER_ERROR("[rename-repoint-test] FAIL: needs an open project (pass --project)");
        AVER_INFO("[rename-repoint-test] RESULT: FAIL");
        return;
    }

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(project_.contentDir()) / "RepointTmp";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    const std::filesystem::path asset   = root / "Cube.ocmesh";
    const std::filesystem::path referrer= root / "Uses.ocworld";
    const std::filesystem::path nearMiss= root / "NearMiss.ocworld";
    const std::filesystem::path unrelated = root / "Other.ocmesh";

    writeFileTextAtomic(asset.string(), "not a real mesh, only its name matters here");
    writeFileTextAtomic(unrelated.string(), "also not a real mesh");
    // A real reference, twice, in both separator styles.
    writeFileTextAtomic(referrer.string(),
                        "MESH RepointTmp/Cube.ocmesh\nMESH RepointTmp\\Cube.ocmesh\n");
    // THE NEAR MISS: a longer folder ending in the same segment. Anchoring must leave this alone,
    // and it is the file whose survival proves the whole exercise is safe.
    const std::string nearMissText = "MESH XRepointTmp/Cube.ocmesh\nMESH RepointTmp/Cube.ocmesh2\n";
    writeFileTextAtomic(nearMiss.string(), nearMissText);

    const std::vector<std::string> found = cbFindReferencesTo(asset.string());
    check(found.size() == 1, "the scan finds exactly the one real referrer, not the near-miss file");

    cbRenameEntry(asset.string(), "Box.ocmesh", /*repointRefs=*/true);
    check(std::filesystem::exists(root / "Box.ocmesh", ec), "the asset is renamed on disk");

    std::string after;
    check(readFileText(referrer.string(), after), "the referrer is readable after the rewrite");
    check(after.find("RepointTmp/Box.ocmesh") != std::string::npos,
          "the forward-slash reference was repointed");
    check(after.find("RepointTmp\\Box.ocmesh") != std::string::npos,
          "AND the backslash one, keeping its own separator style");
    check(after.find("Cube.ocmesh") == std::string::npos, "with no stale reference left behind");

    std::string nm;
    check(readFileText(nearMiss.string(), nm), "the near-miss file is readable");
    check(nm == nearMissText,
          "AND IS BYTE-IDENTICAL -- a longer folder and a longer filename were both left alone");

    std::string un;
    check(readFileText(unrelated.string(), un), "the unrelated asset still exists untouched");

    std::filesystem::remove_all(root, ec);
    AVER_INFO("[rename-repoint-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

void SandboxApp::runFindRefs() {
    const std::string abs = project_.valid()
        ? (project_.contentDir() + "\\" + findRefsPath_) : findRefsPath_;
    const std::vector<std::string> refs = cbFindReferencesTo(abs);
    AVER_INFO("[find-refs] '{}' is referenced by {} file(s)", findRefsPath_, refs.size());
    for (const std::string& r : refs) AVER_INFO("[find-refs]   {}", r);
}

void SandboxApp::runSaveDirtyTest() {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[savedirty-test] PASS: {}", what);
        else      { AVER_ERROR("[savedirty-test] FAIL: {}", what); ++failures; }
    };

    undoStack_.clear();
    redoStack_.clear();
    markLevelSaved();
    check(!levelHasUnsavedEdits(), "a freshly loaded level is not dirty");

    auto edit = [&]() { EditCmd c; c.kind = EditCmd::Kind::Transform; pushEdit(std::move(c)); };

    edit();
    check(levelHasUnsavedEdits(), "an edit makes it dirty");

    markLevelSaved();
    check(!levelHasUnsavedEdits(), "SAVING clears it -- the whole point");

    edit();
    check(levelHasUnsavedEdits(), "editing after a save makes it dirty again");

    undo();
    check(!levelHasUnsavedEdits(), "undoing back TO the save point is clean again");

    redo();
    check(levelHasUnsavedEdits(), "redoing away from it is dirty again");

    // Save, then undo PAST the save point: the document no longer matches the file, even though
    // the stack is shorter than it was when saved. Depth alone cannot tell this from clean.
    markLevelSaved();
    undo();
    check(levelHasUnsavedEdits(), "undoing PAST the save point is dirty, not clean");

    redo();
    check(!levelHasUnsavedEdits(), "and redoing back to it is clean");

    // A recovered sidecar is unsaved by construction, whatever the stack says.
    undoStack_.clear();
    redoStack_.clear();
    markLevelUnsaved();
    check(levelHasUnsavedEdits(), "recovered content reports unsaved even with an empty history");

    // AN EDIT WITH NO UNDO COMMAND MUST STILL BE DIRTY, and this is the case that was silently
    // wrong: the Details panel's sun/fog/sky/clouds writes, Add Component and the emitter's
    // effect assignment all push no EditCmd, and levelHasUnsavedEdits() is driven by the undo
    // serial alone. So changing the sun angle and closing the editor prompted nothing, autosaved
    // nothing, and lost the change. Asserted through markLevelUnsaved rather than by driving the
    // panel, because the panel needs ImGui state a headless run does not have -- what is being
    // pinned is the contract those call sites now rely on.
    undoStack_.clear();
    redoStack_.clear();
    markLevelSaved();
    check(!levelHasUnsavedEdits(), "a level with no history and no edits is clean");
    markLevelUnsaved();
    check(levelHasUnsavedEdits(),
          "a non-undoable edit (sun, fog, Add Component, emitter effect) reports unsaved");
    markLevelSaved();
    check(!levelHasUnsavedEdits(), "and only SAVING clears it -- there is no command to undo");

    undoStack_.clear();
    redoStack_.clear();
    markLevelSaved();
    AVER_INFO("[savedirty-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

void SandboxApp::runMultiSelectTest(Engine& eng) {
    (void)eng;
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[multiselect-test] PASS: {}", what);
        else      { AVER_ERROR("[multiselect-test] FAIL: {}", what); ++failures; }
    };
#if AVER_MODULE_SCENE
    scene::World& w = scene::World::instance();
    scene::Entity a = w.create("msA"), b = w.create("msB"), c = w.create("msC");
    // A fourth, deliberately never added to the set: staleness is about the anchor landing
    // OUTSIDE the selection, and pointing it at a member is correctly not stale.
    scene::Entity d = w.create("msD");

    multiSetSingle(a);
    check(selEntity_ == a && selectedEntities().size() == 1, "a plain click selects exactly one");

    multiToggle(b);
    check(selectedEntities().size() == 2 && multiIsSelected(a) && multiIsSelected(b),
          "ctrl+click adds a second without dropping the first");
    check(selEntity_ == b, "and the newly added one becomes the anchor");

    multiToggle(b);
    check(selectedEntities().size() == 1 && !multiIsSelected(b),
          "ctrl+click again removes it");
    check(selEntity_ == a, "and the anchor moves to something still selected");

    // The range walk needs a drawn order; supply one directly, which is what the outliner does.
    outlinerOrder_ = {a, b, c};
    multiSetSingle(a);
    multiRange(c);
    check(selectedEntities().size() == 3, "shift+click takes the whole range in DRAWN order");
    check(selEntity_ == a, "and the anchor stays put so a second shift re-ranges from it");

    // Pointing the anchor at something ALREADY selected is an ordinary act -- the right-click
    // menu does it -- and must NOT collapse the set. Asserted because getting this wrong is the
    // easy over-correction, and it would make right-clicking one of five selected rows silently
    // drop the other four.
    sel_ = kSelScene; selEntity_ = b;
    check(!multiStale(), "moving the anchor WITHIN the set is not stale");
    check(selectedEntities().size() == 3, "and the set survives it");

    // THE ONE THAT MATTERS. Simulate any of the ~40 sites that assign the anchor on their own --
    // a viewport pick, an undo, a paste, a spawn -- all of which mean "this one thing now".
    sel_ = kSelScene; selEntity_ = d;
    check(multiStale(), "an anchor assigned OUTSIDE the set marks it stale");
    check(selectedEntities().size() == 1 && selectedEntities()[0] == d,
          "and the selection collapses to that one entity rather than staying three");
    check(!multiIsSelected(a), "the stale set stops reporting membership");
    multiSyncToAnchor();
    check(multiSel_.empty(), "the once-a-frame sync then actually drops it");

    // A destroyed entity must not survive in the selection.
    multiSetSingle(a); multiToggle(b);
    w.destroy(b); w.flush();
    check(selectedEntities().size() == 1, "a destroyed entity leaves the reported selection");

    // ---- UNDOING A MULTI-MOVE RETURNS THE WHOLE SET, NOT JUST THE ANCHOR -------------------
    //
    // THE BUG THIS PINS. The gizmo moved every non-anchor entity with a bare setLocalTransform
    // and recorded nothing, while endTransformEdit pushed one command keyed on selEntity_. Drag
    // twenty props, Ctrl+Z, and nineteen stayed dragged -- silent, unrepairable scene damage on
    // an everyday gesture. Asserting the ANCHOR came back would have passed the whole time; the
    // assertion has to be about the others, which is why it reads them by name below.
    {
        scene::Entity m0 = w.create("mvA"), m1 = w.create("mvB"), m2 = w.create("mvC");
        auto place = [&](scene::Entity e, f32 x) {
            Transform t; t.position = Vec3{x, 0.0f, 0.0f};
            w.setLocalTransform(e, t);
        };
        place(m0, 0.0f); place(m1, 100.0f); place(m2, 200.0f);
        outlinerOrder_ = {m0, m1, m2};
        multiSetSingle(m0); multiToggle(m1); multiToggle(m2);
        // The anchor must be one of them for the move path to run at all.
        sel_ = kSelScene; selEntity_ = m0;

        check(beginTransformEdit(), "a multi-selection opens a transform gesture");
        // Move the anchor by hand, then the rest exactly as the gizmo does -- through the same
        // shared helper, so this exercises the real path rather than a copy of it.
        const Vec3 delta{0.0f, 0.0f, 500.0f};
        Transform at = w.localTransform(m0); at.position += delta; w.setLocalTransform(m0, at);
        forEachMultiMoved([&](scene::Entity e, const Transform& xf) {
            Transform t = xf; t.position += delta; w.setLocalTransform(e, t);
        });
        endTransformEdit();

        check(std::fabs(w.localTransform(m1).position.z - 500.0f) < 0.01f &&
              std::fabs(w.localTransform(m2).position.z - 500.0f) < 0.01f,
              "the non-anchor entities actually moved");

        undo();
        check(std::fabs(w.localTransform(m0).position.z) < 0.01f,
              "one undo returns the anchor");
        check(std::fabs(w.localTransform(m1).position.z) < 0.01f &&
              std::fabs(w.localTransform(m2).position.z) < 0.01f,
              "and THE SAME undo returns every other entity in the set");

        redo();
        check(std::fabs(w.localTransform(m1).position.z - 500.0f) < 0.01f &&
              std::fabs(w.localTransform(m2).position.z - 500.0f) < 0.01f,
              "redo takes the whole set forward again, not just the anchor");

        multiClear();
        sel_ = -1; selEntity_ = scene::kInvalidEntity;
        undoStack_.clear(); redoStack_.clear();
        w.destroy(m0); w.destroy(m1); w.destroy(m2); w.flush();
    }

    // ---- Select All takes the drawn order, and Ctrl+D copies the whole set ------------------
    {
        scene::Entity s0 = w.create("selA"), s1 = w.create("selB"), s2 = w.create("selC");
        outlinerOrder_ = {s0, s1, s2};
        multiClear();
        sel_ = -1; selEntity_ = scene::kInvalidEntity;

        selectAllInOutliner();
        check(selectedEntities().size() == 3, "Select All takes every row the outliner listed");
        check(selEntity_ == s0,
              "and anchors on the FIRST row, so a following shift-click ranges downward");

        // Duplicate used to read selEntity_ alone: five selected props, one copy. Counting the
        // WORLD is what catches that -- asserting the selection changed would not, because the
        // single-entity path also reselects.
        const usize beforeCount = w.count();
        duplicateSelection();
        check(w.count() == beforeCount + 3,
              "Ctrl+D on a set of three creates THREE copies, not one");
        check(selectedEntities().size() == 3, "and the copies become the selection");
        check(!multiIsSelected(s0), "leaving the originals deselected, so a drag moves the copies");

        multiClear();
        sel_ = -1; selEntity_ = scene::kInvalidEntity;
        undoStack_.clear(); redoStack_.clear();
        outlinerOrder_.clear();
    }

    // ---- Ctrl+C / Ctrl+V take the whole set too, and do not double-copy a subtree -----------
    //
    // Duplicate was fixed for the set; Copy was not, and the two are separate verbs with
    // separate code. Copy read the anchor alone and the clipboard could physically hold one
    // entity, so Ctrl+C on five props and Ctrl+V produced ONE -- silently, because the paste
    // looked like it worked.
    {
        scene::Entity c0 = w.create("cpA"), c1 = w.create("cpB"), c2 = w.create("cpC");
        multiClear();
        sel_ = -1; selEntity_ = scene::kInvalidEntity;
        multiSetSingle(c0); multiToggle(c1); multiToggle(c2);
        sel_ = kSelScene; selEntity_ = c0;

        // COUNTING THE WORLD is what catches an anchor-only copy: asserting the clipboard is
        // non-empty would pass on the broken version too.
        const usize beforeCopy = w.count();
        copySelection();
        pasteClipboard();
        check(w.count() == beforeCopy + 3,
              "Ctrl+C then Ctrl+V on a set of three creates THREE copies, not one");
        check(selectedEntities().size() == 3, "and the pastes become the selection");
        check(w.valid(c0) && w.valid(c1) && w.valid(c2), "with every original left alone");

        multiClear();
        sel_ = -1; selEntity_ = scene::kInvalidEntity;
        undoStack_.clear(); redoStack_.clear();

        // THE ANCESTOR-SKIP CASE, which is the one a naive loop gets wrong. A parent and its own
        // child both selected must paste TWO entities, not three: captureSubtree already carries
        // the child along inside the parent, so keeping the child as its own clipboard entry
        // would paste it twice -- once correctly parented, once orphaned beside it.
        scene::Entity par = w.create("cpParent");
        scene::Entity kid = w.create("cpChild", par, Transform{});
        w.flush();
        multiClear();
        multiSetSingle(par); multiToggle(kid);
        sel_ = kSelScene; selEntity_ = par;

        const usize beforeNest = w.count();
        copySelection();
        pasteClipboard();
        check(w.count() == beforeNest + 2,
              "a selected parent AND its selected child paste as two entities, not three -- "
              "the child is not copied a second time as an orphan");

        multiClear();
        sel_ = -1; selEntity_ = scene::kInvalidEntity;
        undoStack_.clear(); redoStack_.clear();
    }

    multiClear();
    sel_ = -1; selEntity_ = scene::kInvalidEntity;
    w.destroy(a); w.destroy(c); w.destroy(d); w.flush();
#endif
    AVER_INFO("[multiselect-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

void SandboxApp::runGraphHitsTest() {
    // Every failure branch and every assertion below reaches through scripts_ (scripting::ScriptHost,
    // graphLoad/graphTick/graphNodeHits/graphUnload), and that member is declared
    // `#if AVER_MODULE_SCRIPTING` in SandboxApp.hpp -- unlike the AVER_WITH_IMGUI this whole function
    // already sits inside (which proves nothing about AVER_MODULE_SCRIPTING; module-matrix.ps1's
    // scripting-off row leaves the UI on), a scripting-off tree has no ScriptHost to call any of
    // this on at all. The whole function is meaningless without it, so the guard covers the
    // function rather than each call, same as runValidateGraph just above.
#if AVER_MODULE_SCRIPTING
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[graph-hits-test] PASS: {}", what);
        else      { AVER_ERROR("[graph-hits-test] FAIL: {}", what); ++failures; }
    };

    if (!scripts_.graphHitsAvailable()) {
        AVER_ERROR("[graph-hits-test] FAIL: the staged bridge exports no node-hit entry points");
        AVER_INFO("[graph-hits-test] RESULT: FAIL");
        return;
    }
    check(true, "the bridge exports GraphSetHitRecording and GraphGetHits");

    std::string name;
    {   // The NAME is the key the managed table uses, so read it from the file rather than guess.
        std::string text;
        if (!readFileText(graphHitsTestPath_, text)) {
            AVER_ERROR("[graph-hits-test] FAIL: could not read '{}'", graphHitsTestPath_);
            AVER_INFO("[graph-hits-test] RESULT: FAIL");
            return;
        }
        fmt::OcGraphData g;
        std::string why;
        if (fmt::parseOcgraph(text, g, &why)) name = g.name;
    }
    check(!name.empty(), "the test graph declares a NAME for the hit table to key on");

    constexpr i32 kEnt = 424242;   // an id no scene entity here uses; graphLoad only keys a map by it
    std::vector<std::pair<std::string, f32>> hits;

    // DISARMED FIRST, and this is the assertion that would catch "it records unconditionally":
    // the instrumentation call is in the IL whether or not anyone is looking, so what must be
    // true is that it stores nothing.
    scripts_.graphSetHitRecording(false);
    check(scripts_.graphLoad(kEnt, graphHitsTestPath_), "the graph loads onto an entity");
    scripts_.graphTick(kEnt, 0.016f);
    scripts_.graphNodeHits(name, 5.0f, hits);
    check(hits.empty(), "with recording OFF a full tick records nothing");

    scripts_.graphSetHitRecording(true);
    scripts_.graphTick(kEnt, 0.032f);
    scripts_.graphNodeHits(name, 5.0f, hits);
    check(!hits.empty(), "with recording ON the same tick reports nodes that ran");
    bool aged = true;
    for (const auto& h : hits) if (h.second < 0.0f || h.second > 5.0f) aged = false;
    check(aged, "and every reported age is a plausible number of seconds, so the payload parsed");

    // THE LAST ENTRY MUST SURVIVE THE ROUND TRIP, and this assertion exists because it did not.
    // GraphGetHits truncates the payload at a separator so a node id is never cut in half -- and
    // the first version did that UNCONDITIONALLY, so a payload that fitted perfectly still lost
    // its final entry. The managed unit tests cannot see it (they call CollectNodeHits directly
    // and never cross the ABI), and the symptom was a node that provably ran -- it printed -- and
    // never lit up. Any probe graph reaching here has at least three exec nodes on its chain.
    check(hits.size() >= 3,
          "every node on the chain survives marshalling, including the LAST one");
    bool named = !hits.empty();
    for (const auto& h : hits) if (h.first.empty()) named = false;
    check(named, "and no entry came back with an empty node id, so nothing was cut mid-entry");
    for (const auto& h : hits) AVER_INFO("[graph-hits-test]   ran: {} ({:.3f}s ago)", h.first, h.second);

    // A DIFFERENT NAME MUST REPORT NOTHING -- the filter is what stops one canvas lighting another
    // graph's nodes, and it lives on the managed side, so it has to be checked from here too.
    scripts_.graphNodeHits(name + "_NotThisOne", 5.0f, hits);
    check(hits.empty(), "asking for a different graph name reports nothing");

    scripts_.graphSetHitRecording(false);
    scripts_.graphUnload(kEnt);
    AVER_INFO("[graph-hits-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
#else
    AVER_ERROR("[graph-hits-test] FAIL: this build has no scripting module; no bridge to hit-test");
    AVER_INFO("[graph-hits-test] RESULT: FAIL");
#endif
}

void SandboxApp::runAssetAssignTest() {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[asset-assign-test] PASS: {}", what);
        else      { AVER_ERROR("[asset-assign-test] FAIL: {}", what); ++failures; }
    };
#if AVER_MODULE_SCENE
    scene::World& w = scene::World::instance();
    const scene::Entity e = w.create("assignTarget");
    w.addComponent(e, scene::kComponentMeshRenderer);
    auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    check(mr != nullptr, "the fixture entity has a mesh renderer");
    if (mr) {
        *mr = scene::CMeshRenderer{};

        // A MESH ID IS A PATH HASH, and picking one must reach the field verbatim.
        const u64 meshId = fnv1a64(std::string_view("Meshes/Pick.ocmesh"));
        markLevelSaved();
        check(!levelHasUnsavedEdits(), "the level starts clean");
        // CLEARED FIRST, OR THE dirty ASSERTION BELOW IS VACUOUS. CMeshRenderer::dirty defaults
        // to 1, so a fresh component already satisfies it -- the first draft of this test passed
        // with the flag deliberately removed from assignMeshId, which is the exact shape of
        // assertion this codebase has been burned by before. Zeroing it is what makes the check
        // observe the assignment rather than the default.
        mr->dirty = 0;
        check(assignMeshId(e, meshId), "assignMeshId accepts an entity with a mesh renderer");
        check(mr->mesh == meshId, "the picked mesh id reaches the field unchanged");
        check(mr->dirty == 1,
              "AND THE RENDERER IS TOLD TO RE-UPLOAD -- without this the picture never changes");
        check(levelHasUnsavedEdits(),
              "the level is dirty afterwards (these writes have no EditCmd, so this is the only "
              "thing standing between the edit and silent loss on close)");

        // A MATERIAL IS A NAME TOKEN. Interning the same name twice must give the same token, and
        // that token -- not a hash of anything -- is what belongs in the field.
        const i32 token = aver_scene_material(0, "M_PickTest");
        check(token != 0, "a surface name interns to a non-zero token");
        check(aver_scene_material(0, "M_PickTest") == token, "and interning is stable");
        check(fnv1a64(std::string_view("M_PickTest")) != static_cast<u64>(static_cast<u32>(token)),
              "the token is NOT the name's hash -- which is exactly why the two id spaces cannot "
              "be used interchangeably");
        markLevelSaved();
        check(assignMaterialToken(e, token), "assignMaterialToken accepts the entity");
        check(mr->material == token, "the TOKEN reaches the field, not a path hash");
        check(levelHasUnsavedEdits(), "and it marks the level dirty too");
    }

    // An entity with no mesh renderer must be refused rather than silently doing nothing to a
    // component that is not there.
    const scene::Entity bare = w.create("noRenderer");
    check(!assignMeshId(bare, 1234), "assignMeshId refuses an entity with no mesh renderer");
    check(!assignMaterialToken(bare, 1), "assignMaterialToken refuses it too");

#if AVER_MODULE_PARTICLES
    const scene::Entity pem = w.create("emitter");
    w.addComponent(pem, scene::kComponentParticleEmitter);
    if (auto* pe = w.component<scene::CParticleEmitter>(pem, scene::kComponentParticleEmitter)) {
        *pe = scene::CParticleEmitter{};
        // THE EXTENSION GATE, which the shared helper owns so the drop target and the picker
        // cannot disagree about it.
        // A RELATIVE PATH, because SeparationTest forbids an absolute one anywhere in engine
        // source and was right to fail this test's first draft. The extension gate runs before
        // any path resolution, so nothing here needs a real location to exercise it.
        check(!assignParticleEffect(pem, "Meshes/Thing.ocmesh"),
              "assignParticleEffect refuses anything that is not a .ocparticle");
        check(pe->effect == 0, "and leaves the field alone when it refuses");
    }
    w.destroy(pem);
#endif
    w.destroy(e); w.destroy(bare); w.flush();
#endif
    AVER_INFO("[asset-assign-test] RESULT: {}", failures == 0 ? "PASS" : "FAIL");
}

void SandboxApp::runUndoTest(Engine& eng) {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[undo-test] PASS: {}", what);
        else      { AVER_ERROR("[undo-test] FAIL: {}", what); ++failures; }
    };

#if AVER_MODULE_SCENE
    {
        scene::World& w = scene::World::instance();
        // flush() is what actually retires a destroy() and makes count()/valid() see it: World.cpp
        // destroy() only sets a pending bit, and the slot stays "live" until the next flush() runs
        // (also why undoing the SAME destroy still works: recreateFrom() just creates a new one).
        // The normal loop calls flush() once a frame; this test crams several destroys into ONE
        // frame, so it calls flush() itself after each to see the same eventually-consistent state a human clicking Delete across real frames would.
        const u32 base = w.count();
        hideEditorScene_ = true;   // forces spawnCube()'s scene-entity branch, see its own `if`

        spawnCube(eng);
        w.flush();
        const scene::Entity a1 = selEntity_;
        check(sel_ == kSelScene && w.valid(a1) && w.count() == base + 1, "spawnCube creates one scene entity");

        // A custom object id, deliberately NOT the fnv1a64(asset name) a fresh create() assigns
        // on its own -- two entities sharing an asset name naturally share a default objectId.
        // Only a CUSTOM value distinguishes "this id was deliberately carried over" (undo/redo)
        // from "whatever a fresh create() computes" (paste/duplicate), so the test forces it into being observable.
        const u64 customId = 0x00A5EA55u;
        w.setObjectId(a1, customId);
        check(w.objectId(a1) == customId, "setObjectId sets the custom id the rest of this phase checks for");

        deleteSelection();
        w.flush();
        check(!w.valid(a1) && w.count() == base, "deleteSelection removes the scene entity");

        undo();
        w.flush();
        const scene::Entity a2 = selEntity_;
        check(sel_ == kSelScene && w.valid(a2) && w.count() == base + 1, "undo restores the deleted entity");
        check(w.valid(a2) && w.objectId(a2) == customId, "undo restores the SAME (custom) persisted object id");

        redo();
        w.flush();
        check(w.count() == base, "redo re-deletes the restored entity");

        undo();
        w.flush();
        const scene::Entity a3 = selEntity_;
        check(sel_ == kSelScene && w.valid(a3) && w.count() == base + 1, "a second undo restores it again");
        check(w.valid(a3) && w.objectId(a3) == customId, "the custom object id survives a second undo too");

        copySelection();
        pasteClipboard();
        w.flush();
        const scene::Entity b1 = selEntity_;   // pasteClipboard() selects the pasted copy
        check(w.valid(b1) && b1 != a3 && w.count() == base + 2, "paste creates a second, distinct entity");
        check(w.valid(b1) && w.objectId(b1) != customId,
              "paste's copy does NOT clone a's custom object id (see instantiateEntity's restoreObjectId)");

        duplicateSelection();   // duplicates b1, the current selection
        w.flush();
        const scene::Entity c1 = selEntity_;
        check(w.valid(c1) && c1 != b1 && w.count() == base + 3, "duplicate creates a third, distinct entity");
        check(w.valid(c1) && w.objectId(c1) != customId, "duplicate's copy also does not clone the custom object id");

        undo(); w.flush(); check(w.count() == base + 2, "unwind 1/3: undoes duplicate's Create");
        undo(); w.flush(); check(w.count() == base + 1, "unwind 2/3: undoes paste's Create");
        undo(); w.flush(); check(w.count() == base,     "unwind 3/3: undoes the original spawn's Create");
    }

    // ---- deleting a PARENT, and getting its children back ------------------------------------
    //
    // World::destroy retires the whole subtree, so a Destroy command carrying one snapshot could
    // only ever restore one entity: parent a lamp to a table, delete the table, Ctrl+Z, and the
    // table came back alone. That is data loss and it became reachable the moment a level file
    // could express a hierarchy.
    {
        scene::World& w = scene::World::instance();
        const u32 base = w.count();
        hideEditorScene_ = true;

        spawnCube(eng); w.flush();
        const scene::Entity parent = selEntity_;
        spawnCube(eng); w.flush();
        const scene::Entity child = selEntity_;
        spawnCube(eng); w.flush();
        const scene::Entity grand = selEntity_;
        check(w.count() == base + 3, "three entities for the hierarchy phase");

        // keepWorld = false: the local transform IS the parent-relative one, matching what a
        // level file's CHILD record stores and what LevelInstance applies on load.
        check(w.setParent(child, parent, false), "the child accepts the parent");
        check(w.setParent(grand, child, false),  "and the grandchild accepts the child");
        check(w.parent(child) == parent && w.parent(grand) == child, "the chain is two deep");

        // ---- the gizmo's frame ----------------------------------------------------------
        //
        // selectedXform/setSelectedXform used to return CLocal verbatim while the gizmo draws at
        // the returned position and drags it by a world-space delta -- so a child of an entity
        // 5000 cm out drew its manipulator 50 m from its own mesh.
        {
            Transform pxf; pxf.position = Vec3{5000.0f, 0.0f, 0.0f};
            w.setLocalTransform(parent, pxf);
            Transform cxf; cxf.position = Vec3{0.0f, 0.0f, 90.0f};
            w.setLocalTransform(child, cxf);

            sel_ = kSelScene; selEntity_ = child;
            EditXform gx{};
            check(selectedXform(gx), "the child's transform reads back");
            check(std::fabs(gx.pos.x - 5000.0f) < 0.01f && std::fabs(gx.pos.z - 90.0f) < 0.01f,
                  "and it is WORLD (5000,0,90), not the local (0,0,90) the gizmo would have drawn at");

            // THE ROUND TRIP HAS TO BE EXACT, or every selection would drift a little each time
            // the panel wrote back a value it had just read.
            setSelectedXform(gx);
            const auto* back = w.component<scene::CLocal>(child, scene::kComponentLocal);
            check(back && std::fabs(back->xf.position.x) < 0.01f
                       && std::fabs(back->xf.position.z - 90.0f) < 0.01f,
                  "writing that world transform straight back leaves the LOCAL one unchanged");

            // And a real world-space move lands where it was asked to, not 5000 cm away.
            gx.pos = Vec3{5000.0f, 0.0f, 140.0f};
            setSelectedXform(gx);
            EditXform again{};
            check(selectedXform(again) && std::fabs(again.pos.z - 140.0f) < 0.01f,
                  "and moving it in world space puts it where the drag asked");

                sel_ = kSelScene; selEntity_ = parent;
        }

        // ---- undo of a TRANSFORM on a child ---------------------------------------------
        //
        // endTransformEdit records before/after through selectedXform, which is WORLD, and
        // applyXformTo wrote them straight into CLocal. On a root the two frames coincide and
        // nothing shows; on a child, undo moved the object to its own world coordinates read as
        // an offset from its parent. This is the case the earlier hierarchy phase did not reach,
        // because it exercised selectedXform directly rather than the Transform COMMAND.
        {
            Transform pxf; pxf.position = Vec3{5000.0f, 0.0f, 0.0f};
            w.setLocalTransform(parent, pxf);
            Transform cxf; cxf.position = Vec3{0.0f, 0.0f, 90.0f};
            w.setLocalTransform(child, cxf);

            sel_ = kSelScene; selEntity_ = child;
            beginTransformEdit();
            EditXform moved{};
            selectedXform(moved);
            moved.pos = Vec3{5000.0f, 0.0f, 250.0f};   // a world-space drag, straight up
            setSelectedXform(moved);
            endTransformEdit();

            const auto* afterDrag = w.component<scene::CLocal>(child, scene::kComponentLocal);
            check(afterDrag && std::fabs(afterDrag->xf.position.z - 250.0f) < 0.01f,
                  "dragging a child in world space leaves the expected LOCAL z");

            undo();
            w.flush();
            const auto* afterUndo = w.component<scene::CLocal>(child, scene::kComponentLocal);
            check(afterUndo && std::fabs(afterUndo->xf.position.z - 90.0f) < 0.01f
                            && std::fabs(afterUndo->xf.position.x) < 0.01f,
                  "and UNDO puts the child back at local (0,0,90) -- not at its world x of 5000");

            redo();
            w.flush();
            const auto* afterRedo = w.component<scene::CLocal>(child, scene::kComponentLocal);
            check(afterRedo && std::fabs(afterRedo->xf.position.z - 250.0f) < 0.01f,
                  "and redo returns it to the dragged position");
            undo(); w.flush();
        }

        // ---- undo of DELETING A CHILD keeps it attached ---------------------------------
        //
        // The subtree capture restores everything BELOW the deleted entity. Nothing recorded
        // what was ABOVE it, so a deleted child came back as a root -- and, since its transform
        // is parent-relative, at that offset from the world origin instead of from its parent.
        {
            sel_ = kSelScene; selEntity_ = child;
            deleteSelection();
            w.flush();
            check(!w.valid(child), "the child is deleted");

            undo();
            w.flush();
            const scene::Entity back = selEntity_;
            check(w.valid(back), "and undo brings it back");
            check(w.parent(back) == parent,
                  "STILL PARENTED to the entity it hung from, rather than restored as a root");
            const auto* bl = w.component<scene::CLocal>(back, scene::kComponentLocal);
            check(bl && std::fabs(bl->xf.position.z - 90.0f) < 0.01f,
                  "at its original parent-relative transform");
        }

        // ---- Duplicate and Paste take the CHILDREN with them ----------------------------
        //
        // copySelection/duplicateSelection described ONE entity and spawned ONE entity, so
        // duplicating a table with a lamp on it produced a bare table -- and the paste looked
        // like it had worked, which is what made it worth a test rather than a glance. Delete
        // had carried its subtree since the hierarchy landed; these two never did.
        //
        // COUNTED, NOT INSPECTED, deliberately: the depth-2 chain below means a copy that took
        // only the direct children would come out one entity short, which counting catches and
        // "does it have a child" would not.
        {
            // A fresh depth-2 chain of its own, so this phase cannot be perturbed by, or
            // perturb, the parent/child/grand fixture the blocks above are still using.
            const u32 dbase = w.count();
            spawnCube(eng); w.flush(); const scene::Entity dp = selEntity_;
            spawnCube(eng); w.flush(); const scene::Entity dc = selEntity_;
            spawnCube(eng); w.flush(); const scene::Entity dg = selEntity_;
            check(w.setParent(dc, dp, false) && w.setParent(dg, dc, false),
                  "a fresh parent -> child -> grandchild chain for the copy phase");
            Transform dcx; dcx.position = Vec3{0.0f, 0.0f, 120.0f};
            w.setLocalTransform(dc, dcx);
            check(w.count() == dbase + 3, "three entities before any copying");

            // ---- Duplicate ----
            sel_ = kSelScene; selEntity_ = dp;
            duplicateSelection();
            w.flush();
            const scene::Entity dupRoot = selEntity_;
            check(w.valid(dupRoot) && dupRoot != dp, "duplicate made a NEW root entity");
            check(w.count() == dbase + 6,
                  "and brought BOTH descendants with it -- three more entities, not one");
            const scene::Entity dupChild = w.firstChild(dupRoot);
            check(dupChild != scene::kInvalidEntity, "the copy has a child");
            check(dupChild != scene::kInvalidEntity && w.firstChild(dupChild) != scene::kInvalidEntity,
                  "and the child has one too, so the whole depth-2 chain came across");
            // The copy is its own object, not an alias: rebinding the source's EditId to the
            // copy would make the undo below delete the ORIGINAL.
            const auto* dcl = dupChild != scene::kInvalidEntity
                ? w.component<scene::CLocal>(dupChild, scene::kComponentLocal) : nullptr;
            check(dcl && std::fabs(dcl->xf.position.z - 120.0f) < 0.01f,
                  "the copied child kept its parent-relative transform");

            undo(); w.flush();
            check(w.count() == dbase + 3 && w.valid(dp) && w.valid(dc) && w.valid(dg),
                  "undo removes the whole duplicate and leaves the ORIGINAL chain intact");

            redo(); w.flush();
            check(w.count() == dbase + 6,
                  "and REDO brings the descendants back, not just the root");
            undo(); w.flush();

            // ---- Copy / Paste ----
            sel_ = kSelScene; selEntity_ = dp;
            copySelection();
            pasteClipboard();
            w.flush();
            const scene::Entity pasteRoot = selEntity_;
            check(w.valid(pasteRoot) && pasteRoot != dp, "paste made a NEW root entity");
            check(w.count() == dbase + 6, "and pasted the descendants with it");
            const scene::Entity pasteChild = w.firstChild(pasteRoot);
            check(pasteChild != scene::kInvalidEntity &&
                  w.firstChild(pasteChild) != scene::kInvalidEntity,
                  "two levels deep, like the thing that was copied");

            undo(); w.flush();
            check(w.count() == dbase + 3 && w.valid(dp) && w.valid(dc) && w.valid(dg),
                  "undo of a paste removes the copy and leaves the original chain");

            // THE CLIPBOARD SURVIVES ITS OWN PASTE. Pasting twice is ordinary, and the second
            // one must produce a hierarchy too rather than a bare root.
            pasteClipboard(); w.flush();
            check(w.count() == dbase + 6, "a SECOND paste from the same clipboard is complete too");
            undo(); w.flush();

            // Leave the phase as it found it, so the reparent block below still counts from a
            // known base.
            sel_ = kSelScene; selEntity_ = dp; deleteSelection(); w.flush();
            check(w.count() == dbase, "the copy phase cleaned up after itself");
        }

        // ---- a material edit is an undoable COMMAND -------------------------------------
        //
        // materialPanel wrote straight through a MaterialDesc* and called touch(); EditCmd::Kind
        // had no Material case, so nothing was ever pushed. Ctrl+Z after darkening a wall undid
        // whatever the user did BEFORE the wall and left the wall dark -- the same shape as the
        // Player Start bug, and worse, because a material is shared: one slider changes every
        // entity drawing with it.
        //
        // THE PANEL ITSELF NEEDS ImGui AND A MOUSE, so what is exercised here is the half that
        // does not: the command, its two appliers, and the undo/redo stacks it lives on. The
        // bracketing (one entry per interaction, not per frame) is the panel's own and is
        // asserted by reading, not by this test -- see materialPanel's comment.
#if AVER_MODULE_PBR
        {
            pbr::MaterialDesc md;
            md.name = "M_UndoTestProbe";
            md.roughnessFactor = 0.20f;
            md.metallicFactor  = 0.00f;
            const pbr::MaterialHandle mh = pbr::MaterialLibrary::get().create(md);
            check(mh != 0, "a probe material was created");

            const usize stackBefore = undoStack_.size();

            // What the panel does on release: snapshot before, mutate, push one command.
            pbr::MaterialDesc* live = pbr::MaterialLibrary::get().mutableDesc(mh);
            check(live != nullptr, "and its desc is reachable");
            if (live) {
                const pbr::MaterialDesc beforeDesc = *live;
                live->roughnessFactor = 0.90f;
                live->metallicFactor  = 1.00f;
                EditCmd mc;
                mc.kind = EditCmd::Kind::Material;
                mc.matHandle = mh;
                mc.matBefore = beforeDesc;
                mc.matAfter  = *live;
                pushEdit(std::move(mc));
                check(undoStack_.size() == stackBefore + 1,
                      "a material edit puts exactly ONE entry on the undo stack");

                undo();
                const pbr::MaterialDesc* afterUndo = pbr::MaterialLibrary::get().desc(mh);
                check(afterUndo && std::fabs(afterUndo->roughnessFactor - 0.20f) < 1e-4f
                                && std::fabs(afterUndo->metallicFactor) < 1e-4f,
                      "undo restores BOTH sliders, not just the last one moved");
                // The name is identity, not an edited value: restoring a stale one would rename
                // a material as a side effect of undoing a roughness drag.
                check(afterUndo && afterUndo->name == "M_UndoTestProbe",
                      "and does NOT rewrite the material's name");

                // A SENTINEL BEFORE THE REDO, so the redo assertion can actually fail. Without
                // it the check reads "roughness is 0.90" -- which is still true if BOTH undo and
                // redo did nothing, because 0.90 is what the edit left behind. Poking a third
                // value in first means only a redo that really re-applies can restore it.
                if (pbr::MaterialDesc* poke = pbr::MaterialLibrary::get().mutableDesc(mh))
                    poke->roughnessFactor = 0.55f;
                redo();
                const pbr::MaterialDesc* afterRedo = pbr::MaterialLibrary::get().desc(mh);
                check(afterRedo && std::fabs(afterRedo->roughnessFactor - 0.90f) < 1e-4f
                                && std::fabs(afterRedo->metallicFactor - 1.00f) < 1e-4f,
                      "redo re-applies the edit");
                check(afterRedo && afterRedo->name == "M_UndoTestProbe",
                      "with the name still intact");

                undo();   // leave the library as this phase found it
            }
            pbr::MaterialLibrary::get().destroy(mh);
        }
#endif

        // ---- renaming an entity is an undoable COMMAND ---------------------------------
        //
        // Nothing anywhere in the editor could rename a placed entity: no F2, no context menu,
        // no field in Details. entityLabels_ was written at spawn/paste/duplicate/load and never
        // from anything a person did, so a level of "Cube 1..40" stayed that way.
        //
        // THE UI NEEDS ImGui; the command does not. What is exercised here is the half that can
        // be: renameEntity, its applier, and the stacks.
        {
            const u32 rnbase = w.count();
            spawnCube(eng); w.flush();
            const scene::Entity re = selEntity_;
            const std::string spawned = entityLabels_.count(static_cast<u32>(re))
                                      ? entityLabels_[static_cast<u32>(re)] : std::string();
            const usize stackBefore = undoStack_.size();

            renameEntity(re, "Doorway");
            check(entityLabels_[static_cast<u32>(re)] == "Doorway", "renameEntity sets the label");
            check(undoStack_.size() == stackBefore + 1, "and puts ONE entry on the undo stack");

            // A rename to the SAME name is not an edit; pushing for it would make Ctrl+Z do
            // nothing once, visibly.
            renameEntity(re, "Doorway");
            check(undoStack_.size() == stackBefore + 1, "renaming to the same name pushes nothing");

            undo();
            check((entityLabels_.count(static_cast<u32>(re))
                   ? entityLabels_[static_cast<u32>(re)] : std::string()) == spawned,
                  "undo restores the name it had before");
            redo();
            check(entityLabels_[static_cast<u32>(re)] == "Doorway", "redo re-applies the rename");

            // THE ASSET NAME IS NOT TOUCHED. CName is the placement's asset path -- what
            // saveLevel writes as the PLACE record and what resolves the mesh -- so a rename
            // that reached it would repoint the placement at a file that does not exist.
            // std::string, NOT ==: World::name returns a const char*, so comparing it to a
            // literal compares POINTERS and is false however equal the text is. The first
            // version of this assertion did exactly that and failed against a name that was
            // already correct -- a test that could not pass rather than one that caught a bug.
            check(std::string(w.name(re)) == "Meshes/cube.ocmesh",
                  "and the entity's ASSET name is untouched by any of it");

            undo();                       // put the label back
            sel_ = kSelScene; selEntity_ = re; deleteSelection(); w.flush();
            check(w.count() == rnbase, "the rename phase cleaned up after itself");
        }

        // ---- the Outliner's reparent, as a command --------------------------------------
        //
        // pushReparent is what a drag-and-drop in the World Outliner calls. Everything below
        // drives it directly: the drop itself cannot be exercised headlessly (ImGui's drag state
        // needs a real mouse), but every decision it makes can be.
        {
            const u32 rbase = w.count();
            spawnCube(eng); w.flush(); const scene::Entity ra = selEntity_;
            spawnCube(eng); w.flush(); const scene::Entity rb = selEntity_;
            spawnCube(eng); w.flush(); const scene::Entity rc = selEntity_;
            check(w.count() == rbase + 3, "three fresh roots for the reparent phase");

            Transform axf; axf.position = Vec3{1000.0f, 0.0f, 0.0f};
            w.setLocalTransform(ra, axf);
            Transform bxf; bxf.position = Vec3{1000.0f, 0.0f, 300.0f};
            w.setLocalTransform(rb, bxf);

            // KEEPWORLD ON THE LIVE DROP: the object must not jump out from under the mouse.
            const usize stackBefore = undoStack_.size();
            pushReparent(rb, ra);
            check(w.parent(rb) == ra, "a drop parents the dragged entity to the row it landed on");
            check(undoStack_.size() == stackBefore + 1, "and puts exactly one entry on the undo stack");
            const auto* bl = w.component<scene::CLocal>(rb, scene::kComponentLocal);
            check(bl && std::fabs(bl->xf.position.x) < 0.01f
                     && std::fabs(bl->xf.position.z - 300.0f) < 0.01f,
                  "keepWorld rewrote the local transform so it did not move on screen");

            undo(); w.flush();
            check(w.parent(rb) == scene::kInvalidEntity, "undo puts it back at the root");
            const auto* bu = w.component<scene::CLocal>(rb, scene::kComponentLocal);
            check(bu && std::fabs(bu->xf.position.x - 1000.0f) < 0.01f,
                  "with the exact local transform it had before the drop, not a recomputed one");

            redo(); w.flush();
            check(w.parent(rb) == ra, "redo re-parents it");

            // A CYCLE IS REFUSED, and pushes nothing. The UI never offers this target, but the
            // command has to hold the line on its own -- a refused drop that still lands an undo
            // entry would let Ctrl+Z 'restore' a state that never existed.
            pushReparent(rc, rb);   // rc under rb, so rb's chain is ra -> rb -> rc
            check(w.parent(rc) == rb, "a grandchild attaches");
            const usize beforeCycle = undoStack_.size();
            // reparentLegality DIRECTLY, because pushReparent alone cannot discriminate: it
            // also checks setParent's return, and World refuses a cycle on its own. What the
            // UI's own guard buys is that the target is never OFFERED -- a property only a real
            // mouse can observe, so this is the closest a headless check can get to it.
            check(reparentLegality(ra, rc) == ReparentLegality::SelfOrDescendant,
                  "the Outliner's own legality test calls an ancestor-under-descendant a cycle");
            check(reparentLegality(ra, ra) == ReparentLegality::SelfOrDescendant,
                  "and calls self-parenting one too");
            check(reparentLegality(rb, ra) == ReparentLegality::Ok,
                  "while an ordinary re-parent onto a non-descendant is allowed");
            pushReparent(ra, rc);   // ra is rc's ancestor: a cycle
            check(w.parent(ra) == scene::kInvalidEntity, "reparenting an ancestor under its own descendant is REFUSED");
            check(undoStack_.size() == beforeCycle, "and pushes no undo entry");
            pushReparent(ra, ra);
            check(w.parent(ra) == scene::kInvalidEntity, "so is parenting something to itself");
            check(undoStack_.size() == beforeCycle, "still no undo entry");

            // Dropping something back onto the parent it already has is a no-op, not an entry.
            pushReparent(rc, rb);
            check(undoStack_.size() == beforeCycle, "dropping onto the CURRENT parent adds nothing to the stack");

            // AN OFF-LEVEL ENDPOINT IS REFUSED. saveLevel writes a parent only for entities the
            // level owns, so this relationship would be gone on the next reload -- the UI shows
            // a reason, and the command refuses regardless of what the UI did.
            const scene::Entity stray = w.create("stray", scene::kInvalidEntity, Transform{});
            w.flush();
            check(!isLevelOwned(stray), "an entity outside levelEntities_ is not level-owned");
            const usize beforeStray = undoStack_.size();
            pushReparent(stray, ra);
            check(w.parent(stray) == scene::kInvalidEntity, "reparenting an entity the level does not own is REFUSED");
            pushReparent(rc, stray);
            check(w.parent(rc) == rb, "and so is parenting a level entity UNDER one it does not own");
            check(undoStack_.size() == beforeStray, "neither pushed an undo entry");
            w.destroy(stray); w.flush();

            // The root drop zone.
            pushReparent(rc, scene::kInvalidEntity);
            check(w.parent(rc) == scene::kInvalidEntity, "dropping on empty space unparents to the root");
            undo(); w.flush();
            check(w.parent(rc) == rb, "and undo re-attaches it");

            // Leave the world as this phase found it.
            sel_ = kSelScene; selEntity_ = ra; deleteSelection(); w.flush();
            check(w.count() == rbase, "teardown: the reparent phase leaves no entities behind");
            undoStack_.clear(); redoStack_.clear(); markLevelSaved();
        }

        sel_ = kSelScene; selEntity_ = parent;
        deleteSelection();
        w.flush();
        check(w.count() == base, "deleting the PARENT removes all three -- World::destroy takes the subtree");
        check(!w.valid(child) && !w.valid(grand), "the children are gone with it, not orphaned");

        undo();
        w.flush();
        check(w.count() == base + 3, "and undo brings all three back, not just the one that was selected");

        // THE RELATIONSHIP, not just the count. Restoring three loose entities where a hierarchy
        // was is the same data loss one step quieter -- every child would silently jump to its
        // parent-relative offset from the world origin.
        const scene::Entity p2 = selEntity_;
        check(w.valid(p2) && w.parent(p2) == scene::kInvalidEntity, "the restored parent is a root again");
        u32 kids = 0;
        scene::Entity firstKid = scene::kInvalidEntity;
        for (scene::Entity c = w.firstChild(p2); c != scene::kInvalidEntity; c = w.nextSibling(c)) {
            if (firstKid == scene::kInvalidEntity) firstKid = c;
            ++kids;
        }
        check(kids == 1, "with exactly one child under it again");
        u32 grandKids = 0;
        if (firstKid != scene::kInvalidEntity)
            for (scene::Entity g = w.firstChild(firstKid); g != scene::kInvalidEntity; g = w.nextSibling(g))
                ++grandKids;
        check(grandKids == 1, "and the grandchild back under THAT child, two deep as it was");

        redo();
        w.flush();
        check(w.count() == base, "redo re-deletes the whole subtree");
        undo();
        w.flush();
        check(w.count() == base + 3, "and a second undo restores all three again");

        // Leave the world as this phase found it, so the placeholder-object phase below starts
        // from a clean count the way it always has.
        sel_ = kSelScene; selEntity_ = selEntity_;
        deleteSelection();
        w.flush();
        check(w.count() == base, "teardown: the phase leaves no entities behind");
    }
#else
    AVER_INFO("[undo-test] AVER_MODULE_SCENE is off; skipping the scene-entity phase");
#endif
    {
        const usize objBase = objects_.size();
        hideEditorScene_ = false;   // forces spawnCube()'s placeholder-object branch instead
        sel_ = -1; selEntity_ = kInvalidId;

        spawnCube(eng);
        check(sel_ >= 0 && (usize)sel_ < objects_.size() && objects_.size() == objBase + 1,
              "spawnCube (placeholder branch) adds one object");

        deleteSelection();
        check(objects_.size() == objBase, "deleteSelection removes the placeholder object");

        undo();
        check(objects_.size() == objBase + 1 && sel_ >= 0 && (usize)sel_ < objects_.size(),
              "undo restores the deleted placeholder object -- THIS DID NOT EXIST before this change");

        redo();
        check(objects_.size() == objBase, "redo re-deletes the placeholder object");

        undo();
        check(objects_.size() == objBase + 1, "a second undo restores the placeholder object again");

        copySelection();
        pasteClipboard();
        check(objects_.size() == objBase + 2, "paste creates a second placeholder object");

        duplicateSelection();
        check(objects_.size() == objBase + 3, "duplicate creates a third placeholder object");

        undo(); check(objects_.size() == objBase + 2, "unwind 1/3: undoes duplicate's CreateObj");
        undo(); check(objects_.size() == objBase + 1, "unwind 2/3: undoes paste's CreateObj");
        undo(); check(objects_.size() == objBase,     "unwind 3/3: undoes the original spawn's CreateObj");
    }

    AVER_INFO("[undo-test] {} failure(s)", failures);
    std::exit(failures == 0 ? 0 : 1);
}

// --keybind-test write|read: a TWO-PROCESS proof that a rebind survives a restart and conflict
// detection refuses rather than silently stealing a chord -- a single process can't demonstrate
// this, since its in-memory KeybindRegistry works whether or not anything reached disk.
//   write: rebinds Edit.Copy to Ctrl+K, and attempts Edit.Paste onto Ctrl+Z (Undo's own default),
//          which conflictWith() must refuse.
//   read:  a FRESH process checks Edit.Copy comes back Ctrl+K (persisted) and Edit.Paste comes
//          back Ctrl+V (the refused rebind never reached disk).
void SandboxApp::runKeybindPersistTest(const std::string& mode) {
    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) AVER_INFO("[keybind-test] PASS: {}", what);
        else      { AVER_ERROR("[keybind-test] FAIL: {}", what); ++failures; }
    };
    using editor::CommandId;
    using editor::Chord;

    if (mode == "write") {
        check(editor::chordToString(keybinds_.chordFor(CommandId::EditCopy)) == "Ctrl+C",
              "Edit.Copy starts at its compiled-in default (Ctrl+C)");

        // SELECT ALL EXISTS AS A COMMAND AT ALL -- that is the regression this guards, and it is
        // not hypothetical. Select All shipped as a menu row whose "Ctrl+A" was a hardcoded hint
        // STRING with no registry entry and no dispatch behind it, so the menu advertised a key
        // that did nothing when pressed. Asserting the chord here is what keeps the menu label
        // (which now reads chordFor, like every other Edit row) and handleManip's dispatch
        // describing one binding instead of two independently-maintained ones.
        check(editor::chordToString(keybinds_.chordFor(CommandId::EditSelectAll)) == "Ctrl+A",
              "Edit.SelectAll exists as a real command and defaults to Ctrl+A");
        check(keybinds_.conflictWith(CommandId::EditSelectAll,
                                      editor::keybindDef(CommandId::EditSelectAll).def,
                                      editor::keybindDef(CommandId::EditSelectAll).scope) == CommandId::Count,
              "and Ctrl+A collides with nothing else in the viewport scope");

        const Chord ctrlZ{ImGuiKey_Z, true, false, false};   // Edit.Undo's own default chord
        const bool blocked = !keybinds_.rebind(CommandId::EditPaste, ctrlZ);
        check(blocked, "rebinding Edit.Paste to Ctrl+Z is REFUSED (Edit.Undo already holds it)");
        check(editor::chordToString(keybinds_.chordFor(CommandId::EditPaste)) == "Ctrl+V",
              "the refused rebind left Edit.Paste's chord unchanged");

        const Chord ctrlK{ImGuiKey_K, true, false, false};   // not any command's default
        check(keybinds_.conflictWith(CommandId::EditCopy, ctrlK,
                                      editor::keybindDef(CommandId::EditCopy).scope) == CommandId::Count,
              "Ctrl+K is free before the rebind");
        check(keybinds_.rebind(CommandId::EditCopy, ctrlK), "rebinding Edit.Copy to the free chord Ctrl+K succeeds");
        check(editor::chordToString(keybinds_.chordFor(CommandId::EditCopy)) == "Ctrl+K",
              "Edit.Copy now reads back as Ctrl+K in THIS process' memory");

        keybinds_.saveToPrefs();
        editor::flushEditorPrefs();
        AVER_INFO("[keybind-test] wrote keybind.edit.copy=Ctrl+K to {}", editor::editorPrefsPath());
    } else if (mode == "read") {
        // loadEditorPreferences() already ran earlier this frame (see prefsLoaded_) and called
        // keybinds_.loadFromPrefs() -- everything below checks what THAT load produced, in a
        // process that has never called rebind() at all.
        check(editor::chordToString(keybinds_.chordFor(CommandId::EditCopy)) == "Ctrl+K",
              "a FRESH process reads Edit.Copy back as Ctrl+K from editor.ini -- the rebind persisted");
        check(editor::chordToString(keybinds_.chordFor(CommandId::EditPaste)) == "Ctrl+V",
              "Edit.Paste is still Ctrl+V in a fresh process -- the REFUSED rebind never reached disk");
    } else {
        AVER_ERROR("[keybind-test] unknown mode '{}' (want write|read)", mode);
        ++failures;
    }

    AVER_INFO("[keybind-test] {} failure(s)", failures);
    std::exit(failures == 0 ? 0 : 1);
}

#endif

#if AVER_MODULE_SYNAPSE
// --bake-nav, fired once. Frame 5 rather than frame 0: applyProject's "Loading level" stage
// during startup is what builds the collision bodies the bake samples -- a bake at frame 0 would
// find no floor anywhere and write a valid file describing an empty world.
void SandboxApp::navBakeCheck(Engine& e) {
    if (navLoadPending_) { navLoadPending_ = false; loadNavForLevel(e); }
    if (!navBakeOnStart_ || navBakeDone_) return;
    if (e.time().frame < 5) return;
    navBakeDone_ = true;
    // PAIRED WITH bakeNavigationNow'S OWN GUARD. It samples scene::World::instance(), so it is
    // compiled only under AVER_MODULE_SCENE; AVER_MODULE_SYNAPSE above is the grid math and proves
    // nothing about there being a world. --bake-nav in a scene-less build has nothing to sample, and
    // says so rather than silently doing nothing.
#if AVER_MODULE_SCENE
    bakeNavigationNow(e);
#else
    AVER_WARN("[Editor] --bake-nav: this build has no scene module, so there is no world to sample");
#endif
}

#endif

// --gpu-timing: print the per-pass GPU breakdown once, near the end of a bounded run.
// WHY THIS EXISTS: D3D12Device already has a full hierarchical timestamp profiler, but the ONLY
// caller was the interactive `frametime` command -- a bounded `--frames N` run, how every
// measurement here is actually taken, got no breakdown, so attribution was answered by ablation,
// the exact method that already produced a false answer here (a sky march "costing 2.61ms" when a
// cheaper sky saved only 0.04ms, because the compiler eliminated everything feeding the term).
// CALLS handleFrameTime RATHER THAN REPRINTING THE TREE: a second copy would be a second thing to
// keep correct, and this codebase has been bitten by hand-kept mirrors drifting apart.
// LATE, at maxFrames_ - 2: numbers are averaged over accumulated frames and lag a readback buffer,
// so frame 0 would only say "supported but no data yet".
// --ray-probe <sx> <sy>: report what viewportRay returns for one screen point.
//
// pick, handleSculpt, handleFoliage and dropWorldPoint ALL go through that one function and
// NONE of them is reachable without a mouse, so the editor's entire screen-to-world conversion
// had no headless witness at all. What it prints is chosen to be checkable rather than merely
// informative: the origin's distance IN FRONT OF THE EYE along the view axis, which must equal
// the near plane and was exactly 0 while ro was hardcoded to eye_, and the reprojection of a
// point on the ray, which must come back to the pixel that was asked for.
//
// LATE, on gpuTimingCheck's frame and for the same kind of reason: vpX_/vpW_ and invVP_ are
// written by the frame that draws the viewport, so probing at attach time reported a 1600x900
// rect and an identity-ish camera -- self-consistent, reprojecting perfectly, and describing a
// view nobody was looking at.
void SandboxApp::rayProbeCheck(Engine& e) {
    if (!rayProbe_ || maxFrames_ == 0 || rayProbeDone_) return;
    const u64 want = maxFrames_ > 8 ? maxFrames_ - 2 : maxFrames_ - 1;
    if (e.time().frame < want) return;
    rayProbeDone_ = true;
    // viewportRay is declared `#if AVER_WITH_IMGUI` in SandboxApp.hpp (SandboxViewport.cpp defines
    // it the same way) -- pick/handleSculpt/handleFoliage/dropWorldPoint all go through that one
    // function, and none of them exist either without the interactive viewport, so a no-ui or
    // d3d12-off build has no screen-to-world conversion to report on at all. Guarding just this one
    // call and letting the log below print anyway would silently report a ray that was never
    // computed (ro/rd left at their zero-init), which is worse than not printing -- so the whole
    // body is guarded, and the flag is accepted and answered honestly instead.
#if AVER_WITH_IMGUI
    Vec3 ro{}, rd{};
    viewportRay(rayProbeX_, rayProbeY_, ro, rd);
    const Vec3 fwd = camForward();
    const Vec3 d   = ro - eye_;
    const f32 along = d.x*fwd.x + d.y*fwd.y + d.z*fwd.z;
    f32 bx = 0.0f, by = 0.0f;
    const bool ok = project(ro + rd * 0.5f, bx, by);
    AVER_INFO("[RayProbe] screen ({:.1f},{:.1f}) viewport ({:.0f},{:.0f} {:.0f}x{:.0f}) "
              "eye ({:.2f},{:.2f},{:.2f}) origin ({:.2f},{:.2f},{:.2f}) "
              "aheadOfEye {:.4f} dir ({:.3f},{:.3f},{:.3f}) reproject {} ({:.1f},{:.1f})",
              rayProbeX_, rayProbeY_, vpX_, vpY_, vpW_, vpH_,
              eye_.x, eye_.y, eye_.z, ro.x, ro.y, ro.z, along,
              rd.x, rd.y, rd.z, ok ? "ok" : "BEHIND", bx, by);
#else
    AVER_INFO("[RayProbe] this build has no editor viewport (AVER_WITH_IMGUI is off); nothing to probe");
#endif
}

void SandboxApp::gpuTimingCheck(Engine& e) {
    if (!gpuTiming_ || maxFrames_ == 0 || gpuTimingDone_) return;
    const u64 want = maxFrames_ > 8 ? maxFrames_ - 2 : maxFrames_ - 1;
    if (e.time().frame < want) return;
    gpuTimingDone_ = true;
    editor::handleFrameTime(*this, e, {}, [](LogLevel lvl, std::string msg) {
        if (lvl == LogLevel::Error) AVER_ERROR("[GPU] {}", msg);
        else                        AVER_INFO ("[GPU] {}", msg);
    });
    // M6: the SAME video-memory snapshot the [RHI.D3D12]/[RHI.Vulkan] init-time line reports
    // (C-1's rhi::IDevice::videoMemory), printed here too so a --frames capture's log carries a
    // budget/usage reading from near the END of the run, beside the frame-time breakdown just
    // above -- not only from device creation, before the run's own allocations exist. `supported`
    // false (D3D11, a Vulkan device with no VK_EXT_memory_budget, every mock) prints a distinct
    // sentence rather than a row of zeros a reader could mistake for "nothing in use".
    const rhi::VideoMemoryInfo vm = e.device() ? e.device()->videoMemory() : rhi::VideoMemoryInfo{};
    if (vm.supported) {
        AVER_INFO("[GPU] video memory: local {} MB used of {} MB budget, non-local {} MB used of {} MB budget",
                  vm.localUsageBytes / 1048576, vm.localBudgetBytes / 1048576,
                  vm.nonLocalUsageBytes / 1048576, vm.nonLocalBudgetBytes / 1048576);
    } else {
        AVER_INFO("[GPU] video memory: not reported by this backend");
    }
}

// --resize-cycle N: resize the real window every N frames during a bounded run.
// WHY THIS EXISTS: "Resizing the window crashed my GPU" was a report I could not reproduce.
// Every headless lever (--render-scale, --aversr-cycle) changes the SCENE size through
// rebuildSceneTargets, a different path from D3D12Device::resize, whose releasePostTargets() does
// NOT recreate targets in the same call -- handles read 0 for a frame while a binding set still
// points at the freed texture. Only a real window resize opens that path.
// SetWindowPos on the HWND, not an engine call: Engine::frameStep syncs to window_->width/height()
// every frame, so this reproduces the user's exact path. SWP_NOACTIVATE, since a capture run must
// not steal focus.
void SandboxApp::resizeCheck(Engine& e) {
    if (resizeCycle_ == 0 || !e.window()) return;
    const u64 f = e.time().frame;
    if (f == 0 || (f % resizeCycle_) != 0) return;
    HWND hwnd = static_cast<HWND>(e.window()->nativeHandle());
    if (!hwnd) return;
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return;
    // Alternate between two sizes rather than growing without bound: a run of any length stays
    // on screen, and both directions of the transition get exercised.
    const int w = (r.right - r.left), h = (r.bottom - r.top);
    const bool big = (resizeStep_++ & 1) == 0;
    const int nw = big ? (w - 137) : (w + 137);   // odd numbers on purpose -- an even split can
    const int nh = big ? (h -  83) : (h +  83);   // hide an off-by-one in a half-resolution target
    if (nw < 320 || nh < 240) return;
    SetWindowPos(hwnd, nullptr, 0, 0, nw, nh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    AVER_INFO("[Sandbox] --resize-cycle: frame {} resized the window to {}x{}", f, nw, nh);
}

void SandboxApp::captureCheck(Engine& e) {
    // --luma-sweep (and --firefly-metric, which shares its readback cycle -- see
    // lumaSweepCheck()) owns the device's single capture slot for the whole run (it requests a
    // new frame every tick, not once near the end) -- sharing it with the one-shot probe/--shot
    // logic below would have the two stomp each other's request on whichever frame they land on
    // the same tick. Not expected to matter to a measurement run, so refuse rather than guess.
    if (lumaSweep_ || fireflyMetric_) return;
    const u64 f = e.time().frame;
    const u64 sf = maxFrames_>8?maxFrames_-3:4;
    const u32 px_ = probeU_ >= 0.0f ? (u32)(vpX_ + vpW_ * probeU_)
                  : (probeX_ ? probeX_ : (u32)(vpX_ + vpW_*0.5f));
    const u32 py_ = probeV_ >= 0.0f ? (u32)(vpY_ + vpH_ * probeV_)
                  : (probeY_ ? probeY_ : (u32)(vpY_ + vpH_*0.5f));
    if (f==sf) {
        e.device()->requestCapture(px_, py_);
        capX_=px_; capY_=py_; capVpX_=vpX_; capVpY_=vpY_; capVpW_=vpW_; capVpH_=vpH_;
    }
    if (f>sf && !capDone_){
        const bool insideReq = (f32)capX_ >= capVpX_ && (f32)capX_ < capVpX_+capVpW_ &&
                               (f32)capY_ >= capVpY_ && (f32)capY_ < capVpY_+capVpH_;
        const bool rectStable = capVpX_==vpX_ && capVpY_==vpY_ && capVpW_==vpW_ && capVpH_==vpH_;
        const char* tag = !insideReq ? "OUTSIDE-VIEWPORT"
                        : !rectStable ? "VIEWPORT-MOVED"
                                      : "in-viewport";
        f32 px[4]; if (e.device()->getCapture(px)) {
            AVER_INFO("[Sandbox] probe ({},{}) px ({:.2f},{:.2f},{:.2f}) raw ({},{},{}) viewport ({},{} {}x{}) {}",
                      capX_, capY_, px[0],px[1],px[2],
                      (int)(px[0]*255.0f+0.5f), (int)(px[1]*255.0f+0.5f), (int)(px[2]*255.0f+0.5f),
                      (int)capVpX_, (int)capVpY_, (int)capVpW_, (int)capVpH_, tag);
            if (!insideReq)
                AVER_ERROR("[Sandbox] PROBE INVALID: ({},{}) is outside the 3D viewport ({},{} {}x{}) "
                           "-- the value above is editor chrome, not a shading result",
                           capX_, capY_, (int)capVpX_, (int)capVpY_, (int)capVpW_, (int)capVpH_);
            else if (!rectStable)
                AVER_WARN("[Sandbox] PROBE SUSPECT: the viewport moved to ({},{} {}x{}) after the "
                          "capture was requested -- re-run before trusting the value above",
                          (int)vpX_, (int)vpY_, (int)vpW_, (int)vpH_);
        }
        if (!shot_.empty()){ std::vector<u8> img; u32 iw=0,ih=0;
            if (e.device()->getFrameImage(img,iw,ih)&&iw&&ih && stbi_write_png(shot_.c_str(),(int)iw,(int)ih,4,img.data(),(int)iw*4))
                AVER_INFO("[Sandbox] screenshot: {} ({}x{})", shot_, iw, ih); }
        capDone_=true;
    }
}

// --luma-sweep: FRONT D's measurement flag, added to test (rather than assume) whether the GI
// estimator's output actually inflates while --cam-wobble moves the camera and settles back
// down once it is still. Logs one [LumaSweep] line per simulated frame with the 3D viewport's
// MEAN LINEAR LUMINANCE, decoded from the backbuffer. FLAG-ONLY: it reuses the exact
// requestCapture()/getFrameImage() readback --shot and --probe already call (see captureCheck
// above) -- no new GPU pass, no new pipeline, nothing added to what the renderer draws. Default
// off, so a run that never passes --luma-sweep is bit-for-bit the run it always was.
//
// ONE FRAME OF LAG IS INHERENT TO THE CAPTURE API, not a bug here: requestCapture() is serviced
// inside present(), so a request made while handling frame f is not ready to read back until the
// NEXT tick. captureCheck()'s probe has the same lag (see aver-capture-frame-offset); this
// function just pays it every frame instead of once -- read back the PENDING request (frame
// f-1's image) first, then issue a fresh one for frame f, so the line logged this tick always
// describes the PREVIOUS tick's frame number.
//
// SUBSAMPLED 4x4 (~170k samples over a 2750x1639 viewport): a mean does not need every pixel, and
// a full-resolution decode every frame of a multi-hundred-frame sweep is unnecessary cost for a
// diagnostic whose own CPU/GPU sync (waitForGpu inside present(), see RHI.D3D12/RHI.Vulkan
// getFrameImage) already makes every sampled frame slower than an unsampled one.
//
// sRGB -> linear BEFORE averaging, not after: the estimator's claimed defect is that its
// contribution to scene RADIANCE grows, and radiance is additive in LINEAR light, not in the
// gamma-encoded backbuffer. Averaging the raw 0-255 bytes would still trend the same direction
// but compress the low end relative to the claim being tested.
//
// ---- --firefly-metric, ADDED HERE RATHER THAN AS A SECOND READBACK ----
//
// A REAL ReSTIR GI FIREFLY IS A SPATIAL OUTLIER, NOT A MEAN: four rounds of fixes to giMode 1's
// fireflies shipped with nothing that could count one, because meanLinLuma above is exactly
// blind to them -- a handful of pixels pegged at the clamp are swamped by a multi-million-pixel
// average. This extends the SAME readback (one getFrameImage, one sRGB->linear decode, gated by
// its own flag so a run that never passes --firefly-metric costs nothing extra and changes no
// existing log line) to also report OUTLIER PIXELS: the count and the peak of pixels whose linear
// luminance exceeds a multiple of their LOCAL neighbourhood's, which is what marks a pixel as a
// firefly rather than a legitimately bright surface -- a sunlit floor is bright over a wide area,
// a firefly is bright against its own immediate surroundings.
//
// THE GRID IS THE EXISTING STRIDE-4 SUBSAMPLE, reused rather than a second, denser decode: the
// mean above already visits every 4th pixel in each direction, so storing that same per-sample
// luminance into a small 2D array is the only added cost -- no new pass over the image.
//
// OUTER RING MINUS INNER CORE, not a plain window mean, for the reference each candidate is
// judged against: the task this metric exists to serve reports CLUSTERS as well as lone pixels,
// and a plain window mean over a cluster is dragged upward by the very outlier it is meant to
// judge -- the more of the window the cluster fills, the more it hides itself. Excluding a small
// inner core (a candidate's own immediate neighbourhood) from its reference means a cluster up to
// that size cannot inflate the average it is compared to. Ro/Ri below are grid-cell radii,
// REASONED not measured (no sweep was run to fit them): Ro=3 is the smallest outer radius that
// still leaves a full ring of reference cells outside a 3x3 (Ri=1) core at every position.
void SandboxApp::lumaSweepCheck(Engine& e) {
    if ((!lumaSweep_ && !fireflyMetric_) || maxFrames_ == 0) return;
    // STRIDE: only sample every Nth simulation frame. The pending readback from the PREVIOUS
    // sampled frame is always collected first regardless of phase, since it was already
    // requested and costs nothing extra to pick up; only the decision to issue a NEW request is
    // strided, so a stride>1 run also does fewer GPU stalls, not just fewer log lines.
    const bool sampleThisFrame = (e.time().frame % (u64)lumaSweepStride_) == 0;
    if (lumaSweepPending_) {
        std::vector<u8> img; u32 iw = 0, ih = 0;
        if (e.device()->getFrameImage(img, iw, ih) && iw && ih) {
            const u32 x0 = (u32)std::fmax(0.0f, lumaSweepVpX_);
            const u32 y0 = (u32)std::fmax(0.0f, lumaSweepVpY_);
            const u32 x1 = (u32)std::fmin((f32)iw, lumaSweepVpX_ + lumaSweepVpW_);
            const u32 y1 = (u32)std::fmin((f32)ih, lumaSweepVpY_ + lumaSweepVpH_);
            auto toLin = [](u8 c) -> f32 {
                const f32 s = c / 255.0f;
                return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
            };
            // gridW/gridH: same stride-4 walk the mean loop below performs, sized only when
            // --firefly-metric actually needs the samples kept around -- --luma-sweep alone
            // allocates nothing extra, staying exactly as cheap as before this flag existed.
            // ---- STRIDE 1 WHEN HUNTING FIREFLIES, 4 WHEN JUST AVERAGING ----
            //
            // A FIREFLY IS OFTEN A SINGLE PIXEL, and a stride-4 walk inspects one pixel in
            // SIXTEEN -- so any given firefly had about a 1-in-16 chance of being looked at,
            // which is not a measurement, it is a lottery. That is why the first version of this
            // metric reported an unchanging count in a scene where fireflies were being reported
            // by eye: the handful of cells it did sample were large sunlit windows, which are
            // stable by nature, and the actual outliers fell between its samples.
            //
            // --luma-sweep keeps stride 4: a MEAN converges perfectly well on 1/16 of the pixels
            // and that flag exists to be cheap. The firefly pass is a diagnostic that runs only
            // when asked, so it pays full resolution to be able to see what it is looking for.
            const u32 step = fireflyMetric_ ? 1u : 4u;
            const u32 gridW = (fireflyMetric_ && x1 > x0) ? (x1 - 1 - x0) / step + 1 : 0;
            const u32 gridH = (fireflyMetric_ && y1 > y0) ? (y1 - 1 - y0) / step + 1 : 0;
            std::vector<f32> grid;
            if (gridW && gridH) grid.assign((size_t)gridW * gridH, 0.0f);

            f64 sum = 0.0; u64 n = 0;
            for (u32 y = y0; y + 1 < y1; y += step) {
                const u8* row = img.data() + static_cast<size_t>(y) * iw * 4;
                const u32 gy = (y - y0) / step;
                for (u32 x = x0; x + 1 < x1; x += step) {
                    const u8* px = row + static_cast<size_t>(x) * 4;
                    const f32 lin = 0.2126f * toLin(px[0]) + 0.7152f * toLin(px[1]) + 0.0722f * toLin(px[2]);
                    sum += lin;
                    ++n;
                    if (!grid.empty()) grid[(size_t)gy * gridW + (x - x0) / step] = lin;
                }
            }
            const f64 meanLin = n ? sum / (f64)n : -1.0;
            // lumaSweepYaw_ -- CACHED AT REQUEST TIME, not read live here. yaw_ by the time this
            // collection code runs has already been advanced by THIS tick's camWobble update
            // (that runs earlier in onUpdate, before lumaSweepCheck), so reading yaw_ live would
            // pair frame f's image with frame f+1's camera angle -- a one-frame mismatch that is
            // invisible near a wobble peak (slope ~0 there) but real at a zero-crossing (slope at
            // its max). Found by noticing matched-pose frames a period apart logged a suspiciously
            // EXACT repeated angle instead of the expected per-frame sinusoid value.
            if (lumaSweep_) {
                AVER_INFO("[LumaSweep] frame={} meanLinLuma={:.6f} samples={} viewport=({},{} {}x{}) "
                          "giMode={} camWobbleDeg={:.2f} camWobblePeriod={} yawDeg={:.3f}",
                          lumaSweepFrame_, meanLin, n, (int)lumaSweepVpX_, (int)lumaSweepVpY_,
                          (int)lumaSweepVpW_, (int)lumaSweepVpH_, giModeOverride_, camWobbleDeg_,
                          camWobblePeriod_, lumaSweepYaw_ * 57.29577951f);
            }

            if (fireflyMetric_ && !grid.empty()) {
                // Summed-area table over the grid, so a windowed sum is O(1) regardless of
                // window size instead of re-scanning a neighbourhood per candidate cell -- the
                // grid is already built above from the mean loop's own samples, so this is one
                // extra O(gridW*gridH) pass, not a second full-resolution image decode.
                std::vector<f64> sat((size_t)(gridW + 1) * (gridH + 1), 0.0);
                for (u32 gy = 0; gy < gridH; ++gy) {
                    f64 rowSum = 0.0;
                    for (u32 gx = 0; gx < gridW; ++gx) {
                        rowSum += grid[(size_t)gy * gridW + gx];
                        sat[(size_t)(gy + 1) * (gridW + 1) + (gx + 1)] =
                            rowSum + sat[(size_t)gy * (gridW + 1) + (gx + 1)];
                    }
                }
                auto boxSum = [&](int bx0, int by0, int bx1, int by1) -> f64 {
                    // Inclusive [bx0,bx1] x [by0,by1]; caller has already clamped to the grid.
                    return sat[(size_t)(by1 + 1) * (gridW + 1) + (bx1 + 1)]
                         - sat[(size_t)(by0)     * (gridW + 1) + (bx1 + 1)]
                         - sat[(size_t)(by1 + 1) * (gridW + 1) + (bx0)]
                         + sat[(size_t)(by0)     * (gridW + 1) + (bx0)];
                };
                // SCALED WITH THE STRIDE so the ring covers the same SCREEN neighbourhood it did
                // when a cell was 4 pixels wide -- otherwise going to stride 1 would silently
                // shrink the reference region 4x and compare a pixel against its immediate
                // neighbours, which a real firefly partly contaminates.
                const int Ro = (step == 1u) ? 12 : 3, Ri = (step == 1u) ? 4 : 1;
                // Absolute floor, not ratio alone: in a black region the local reference is
                // ~0, and any nonzero noise pixel would then clear an N-times-zero threshold
                // for free. A pixel below this linear luminance is not what anyone would call
                // a firefly regardless of what its neighbours read.
                const f64 kAbsFloor = 0.02;
                // ---- AND HOW MANY OF THOSE OUTLIERS ARE ACTUALLY FLICKERING ----
                //
                // A SPATIAL OUTLIER IS NOT THE SAME THING AS A FIREFLY, and conflating the two
                // is what made this metric unusable on its first outing: in converged Sponza it
                // sat at a rock-steady 56 outliers, IDENTICAL with the denoiser on and off, and
                // that was read as a stuck readback. It was not stuck. Those 56 are sunlight
                // through windows -- genuinely far brighter than their surroundings, genuinely
                // there every frame, and nothing a GI denoiser touches. The count was correct and
                // measuring the wrong population.
                //
                // A firefly is distinguished from a bright feature by INSTABILITY, so the subset
                // that matters is the outliers whose own luminance changed materially since the
                // previous sampled frame. A sunlit window sill does not; a reservoir that won a
                // freak sample for one frame does.
                //
                // MEANINGFUL WITH A STILL CAMERA, and only approximately under motion -- say so
                // rather than let someone trust it in the wrong regime. On a static scene with a
                // static camera every temporal change IS estimator noise, which is exactly the
                // quantity wanted. Under --cam-wobble a static highlight SLIDES across the grid,
                // so it changes cell to cell and inflates this count; isolating that properly
                // needs reprojection, which is more machinery than a diagnostic warrants. Measure
                // fireflies with the camera still; use the spatial count under motion.
                const bool havePrev = fireflyPrevW_ == gridW && fireflyPrevH_ == gridH &&
                                      fireflyPrevGrid_.size() == grid.size();
                u64 outlierCount = 0; f64 outlierMax = 0.0; u64 flickerCount = 0; f64 flickerMax = 0.0;
                for (u32 gy = 0; gy < gridH; ++gy) {
                    const int oy0 = std::max(0, (int)gy - Ro), oy1 = std::min((int)gridH - 1, (int)gy + Ro);
                    const int iy0 = std::max(0, (int)gy - Ri), iy1 = std::min((int)gridH - 1, (int)gy + Ri);
                    for (u32 gx = 0; gx < gridW; ++gx) {
                        const f64 lum = grid[(size_t)gy * gridW + gx];
                        if (lum < kAbsFloor) continue;
                        const int ox0 = std::max(0, (int)gx - Ro), ox1 = std::min((int)gridW - 1, (int)gx + Ro);
                        const int ix0 = std::max(0, (int)gx - Ri), ix1 = std::min((int)gridW - 1, (int)gx + Ri);
                        const f64 outerSum = boxSum(ox0, oy0, ox1, oy1);
                        const f64 innerSum = boxSum(ix0, iy0, ix1, iy1);
                        const f64 outerCnt = (f64)(ox1 - ox0 + 1) * (f64)(oy1 - oy0 + 1);
                        const f64 innerCnt = (f64)(ix1 - ix0 + 1) * (f64)(iy1 - iy0 + 1);
                        const f64 ringCnt  = outerCnt - innerCnt;
                        const f64 localRef = ringCnt > 0.0 ? (outerSum - innerSum) / ringCnt
                                                            : (outerCnt > 0.0 ? outerSum / outerCnt : 0.0);
                        if (lum > (f64)fireflyMult_ * std::max(localRef, kAbsFloor)) {
                            ++outlierCount;
                            outlierMax = std::max(outlierMax, lum);
                            // Relative to the LARGER of the two, so a cell going bright and a
                            // cell going dark are treated alike and neither divides by ~0.
                            if (havePrev) {
                                const f64 was = fireflyPrevGrid_[(size_t)gy * gridW + gx];
                                const f64 den = std::max(std::max(was, lum), kAbsFloor);
                                if (std::fabs(lum - was) / den > 0.5) {
                                    ++flickerCount;
                                    flickerMax = std::max(flickerMax, lum);
                                }
                            }
                        }
                    }
                }
                // flicker=-1 rather than 0 on the first sampled frame: there is no previous grid
                // to compare against, and reporting "no fireflies" for "could not tell" is the
                // kind of confident zero this metric already got wrong once.
                AVER_INFO("[FireflyMetric] frame={} outliers={} flicker={} maxLin={:.4f} "
                          "flickerMax={:.4f} meanLin={:.6f} mult={:.2f} grid={}x{} giMode={} "
                          "camWobbleDeg={:.2f} camWobblePeriod={}",
                          lumaSweepFrame_, outlierCount, havePrev ? (i64)flickerCount : -1,
                          outlierMax, flickerMax, meanLin, fireflyMult_,
                          gridW, gridH, giModeOverride_, camWobbleDeg_, camWobblePeriod_);
                fireflyPrevGrid_ = grid;
                fireflyPrevW_ = gridW;
                fireflyPrevH_ = gridH;
            }
        } else {
            AVER_WARN("[LumaSweep] frame={}: getFrameImage() returned nothing -- this backend may "
                      "not support a full-frame readback", lumaSweepFrame_);
        }
        lumaSweepPending_ = false;
    }
    if (!sampleThisFrame) return;
    e.device()->requestCapture((u32)(vpX_ + vpW_ * 0.5f), (u32)(vpY_ + vpH_ * 0.5f));
    lumaSweepVpX_ = vpX_; lumaSweepVpY_ = vpY_; lumaSweepVpW_ = vpW_; lumaSweepVpH_ = vpH_;
    lumaSweepFrame_ = e.time().frame;
    lumaSweepYaw_ = yaw_;   // THIS tick's angle, for THIS tick's image -- see the log site above.
    lumaSweepPending_ = true;
}

} // namespace aver
