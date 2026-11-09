// Automation and self-test state: scripted CLI test drivers, frame counters, capture/probe requests and camera scripts.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct HarnessState {
    int hudPreviewIndex_ = -1;
    int hudTest_ = -1;
    bool saveProject_ = false;
    bool bakeLevelPackDone_ = false;
    bool saveLevelDone_ = false;
    bool rayProbe_ = false, rayProbeDone_ = false;   // --ray-probe <sx> <sy>
    f32  rayProbeX_ = 0.0f, rayProbeY_ = 0.0f;
    bool importDone_ = false;
    bool saveProjectDone_ = false;
    bool hudTestReported_ = false;
    f32 hudRectX_ = 0, hudRectY_ = 0, hudRectW_ = 0, hudRectH_ = 0;
    u8  viewportShotState_ = 0;    // 0 idle, 1 requested, 2 capture requested and awaiting the image
    u64 viewportShotFrame_ = 0;    // engine frame the capture was requested on
    u32 viewportShotTries_ = 0;    // frames waited for the image; gives up rather than waiting forever
    bool autoCompile_ = false;
    // True when --auto-compile enabled.
    bool autoCompileFromCli_ = false;
    int focusLevelAt_ = 0;
    // Frames before chunk streaming auto-enables.
    int chunkStreamAutoFrames_ = 5;
    int droneAutoFrames_ = 0;        // --drone: frames left before auto-enabling, 0 = off
    int undoTestAutoFrames_ = 0;     // --undo-test: frames left before runUndoTest() fires, 0 = off
    int classPlaceTestAutoFrames_ = 0;   // --class-place-test: frames left before runClassPlaceTest() fires, 0 = off
    int keybindTestAutoFrames_ = 0;  // --keybind-test: frames left before it fires, 0 = off
    std::string keybindTestMode_;    // "write" or "read"
    static constexpr int kAutoCompileQuietMs = 500;
    std::chrono::steady_clock::time_point autoCompileDue_{};
    int autoCompilePending_ = 0;
    std::string autoCompileReason_;
#if AVER_MODULE_FRAMEWORK
    int32_t engineDefaultGm_ = 0;
    // The view request before the editor overrode it (see startPlay).
    bool    savedView_ = false;
    int32_t savedViewMode_ = AVER_FW_VIEW_THIRD_PERSON;
    float   savedViewEye_ = 160.0f, savedViewBoom_ = 450.0f;
    Vec3 pieCamMoveFrom_{0, 0, 0}, pieCamAfterMove_{0, 0, 0};
    f32  pieCamMoved_ = 0.0f, pieCamMoveDot_ = 0.0f, pieCamCoast_ = 0.0f;
    int pieCamFrames_ = 0, pieCamFrame_ = 0;
    Vec3 pieCamRef_{0, 0, 0};
    f32  pieCamRefYaw_ = 0.0f, pieCamRefPitch_ = 0.0f;
    f32  pieCamMaxPos_ = 0.0f, pieCamMaxYaw_ = 0.0f, pieCamMaxPitch_ = 0.0f;
    bool pieCamKeptYaw_ = false, pieCamKeptPitch_ = false;
    bool pieCamPendingLook_ = false;
    f32  pieCamWantYaw_ = 0.0f, pieCamWantPitch_ = 0.0f;
    int  inputSrcFrames_ = 0, inputSrcFrame_ = 0, inputSrcDisagree_ = 0, inputSrcStuck_ = 0;
    int  wheelTestFrames_ = 0, wheelTestFrame_ = 0;
    f32  wheelTestSpeedBefore_ = 0.0f;
    bool wheelTestForceFly_ = false, wheelTestSawImGui_ = false, wheelTestSawInput_ = false;
    bool wheelTestOwnAtWheel_ = false, wheelTestFlyAtWheel_ = false;
    bool inputSrcStateSaw_ = false, inputSrcImguiSaw_ = false;
    int  vmFrames_ = 0, vmFrame_ = 0;
    Vec3 vmRef_{0, 0, 0};
    f32  vmStill_ = 0.0f, vmMoving_ = 0.0f;
    u32  vmKids_ = 0;
    int recapFrames_ = 0, recapFrame_ = 0, recapLeaked_ = 0, recapNotRecaptured_ = 0;
    int  inputStuckFrames_ = 0, inputStuckFrame_ = 0, inputStuckLatched_ = 0, inputStuckFirstKey_ = -1;
    bool inputStuckSawDown_ = false;
#endif
#if AVER_MODULE_SCENE
    // --scene-census + the latch that fires it exactly once. Emitted from onUpdate, not the load:
    // class placements spawn LATER than loadLevel, and a census at load would miss them -- the exact
    // divergence this exists to catch.
    bool sceneCensus_ = false;
    bool sceneCensusDone_ = false;
#endif
    // --autosave-test latches (outside AVER_WITH_IMGUI since maybeAutosave reads them).
    bool autosaveTestArm_ = false;
    bool autosaveTestLift_ = false;
    bool autosaveTestWarned_ = false;
#if AVER_WITH_IMGUI
    std::string cbMoveTestDir_;
    int cbMoveTestFrames_ = 0;
    int prefsWriteTestFrames_ = 0;
    int  notifyTestFrames_ = 0;
    bool notifyTestLift_ = false;
    // Opening a project with no render settings must not inherit the previous project's settings.
    int projectSwitchFrames_ = 0;
    // Validates one .ocgraph end-to-end through the managed GraphValidate export.
    int graphPrintTestFrames_ = 0;
    // --clear-shader-cache: clears DXIL blob cache. Path empty = user data directory.
    int clearShaderCacheFrames_ = 0;
    std::string clearShaderCacheDir_;
    std::string validateGraphPath_;
    int validateGraphFrames_ = 0;
    // --rename-repoint-test: the rename/repoint chain over real project files.
    int renameRepointFrames_ = 0;
    std::string findRefsPath_;
    int findRefsFrames_ = 0;
    int saveDirtyTestFrames_ = 0;
    int multiSelTestFrames_ = 0;
    // Asset picker's assignment helpers: the material case uses INTERNED NAME TOKEN vs path hashes.
    std::string graphHitsTestPath_;
    int graphHitsTestFrames_ = 0;
    int assetAssignTestFrames_ = 0;
#endif
    u64 resizeCycle_ = 0;
    bool gpuTiming_ = false;
    bool gpuTimingDone_ = false;
    bool gpuTimingStartupReset_ = false;   // GPU average restarted once load work is done.
    u32 resizeStep_ = 0;
    bool lumaSweep_ = false;
    int  lumaSweepStride_ = 1;
    bool lumaSweepPending_ = false;
    u64  lumaSweepFrame_ = 0;
    f32  lumaSweepYaw_ = 0.0f;
    f32  lumaSweepVpX_=0, lumaSweepVpY_=0, lumaSweepVpW_=0, lumaSweepVpH_=0;
    bool fireflyMetric_ = false;
    // Last sampled frame's luminance grid for flicker detection.
    std::vector<f32> fireflyPrevGrid_;
    u32 fireflyPrevW_ = 0, fireflyPrevH_ = 0;
    f32  fireflyMult_ = 8.0f;
    bool capDone_=false;
    u64 capFrame_=~0ull;   // the frame the pending capture was requested on
    // The pixel requested and the viewport rect it was requested against, latched at request time.
    u32 capX_=0, capY_=0;
    f32 capVpX_=0, capVpY_=0, capVpW_=0, capVpH_=0;
    bool frameTimeReport_=false;     // --frame-time: report the frame period, to price the above
    u32  probeX_=0, probeY_=0;       // --probe X Y: absolute capture pixel (0 = viewport centre)
    f32  probeU_=-1.0f, probeV_=-1.0f;   // --probe-rel U V: a FRACTION of the viewport rect
    f32  camWobbleDeg_=0.0f;         // --cam-wobble DEG PERIOD: yaw amplitude, 0 = no motion
    i32  camWobblePeriod_=0;         // ...and its period in FRAMES; sin is 0 at every multiple
    f32  camWobbleBaseYaw_=0.0f;     // the yaw to swing about, latched on the first wobbled frame
    bool camWobbleBased_=false;
    std::vector<std::pair<i32, Vec3>> camTeleports_;   // --cam-teleport FRAME X Y Z (repeatable)
    i32  camWobbleStopFrame_=0;      // --cam-wobble-stop N: wobble only while frame < N; 0 = never stop
    bool consoleSetsApplied_=false;
    f32  camTranslateSpeed_=0.0f;    // --cam-translate SPEED: forward-flight, cm/frame, 0 = no motion
    f32  camWanderAmp_=0.0f;         // --cam-wander AMP SPEED: non-repeating drift, 0 = off
    f32  camWanderSpeed_=1.0f;
    f32  camFlyX_=0.0f, camFlyY_=0.0f, camFlyYaw_=0.0f;   // --cam-fly: cm and degrees per frame
    bool camWanderBased_=false;
    u64  camWanderHeld_=0;           // frames the wander was paused (NeuRAA capture holding a pose)
    std::string nrd2CaptureDir_;     // --nrd2-capture (see setNrd2Capture)
    u32 nrd2CaptureSettle_ = 0;      // frames the requested level has been loaded
    u32  nrd2CapturePoses_=0, nrd2CaptureHold_=256, nrd2CaptureHeldOut_=~0u;
    bool nrd2CaptureGrid_=false, nrd2CaptureStarted_=false;
    Vec3 camWanderBasePos_{};
    f32  camWanderBaseYaw_=0.0f, camWanderBasePitch_=0.0f;
    std::string spawnTestClass_;     // --spawn-test <ClassName>: headless actor-loop test trigger
    bool spawnTestDone_=false;       // the spawn is one-shot, done on the first frame scripts are ready
    int32_t spawnTestEntity_=0;      // the spawned test entity, destroyed a few frames later
    int spawnTestFrames_=0;          // frames since the test spawn, so the destroy is one-shot too
    bool playTest_=false;            // --play-test: headless begin_play -> tick -> end_play trigger
    bool playTestBegun_=false;       // begin_play has fired (one-shot, once a GameMode class is declared)
    u32 playTestSettle_ = 0;   // frames the requested level has been loaded (play-test waits ~1 s)
    int  playTestWait_=0;            // frames spent waiting for a GameMode class before giving up
    int  playTestFrames_=0;          // frames since begin_play, so the Stop is one-shot too
    int  playTestRound_=0;           // Plays finished (AVER_PLAYTEST_PLAYS runs more than one)
    int  playTestShotFrame_=-1;      // the frame a mid-Play capture was requested on (AVER_PLAYTEST_SHOT)
    bool skinTest_ = false;       // --skin-test: GPU skinning against its CPU reference, then exit
    // VERDICTS LATCHED OUT OF THE TWO TESTS ABOVE: onShutdown destroys them before exitCode() is read. -1 = never asked, 0 = passed, 1 = failed.
    int skinSelfTestExit_ = -1;
    int skinDrawExit_     = -1;
    bool particleTest_ = false;   // --particle-test: a dust cloud straddling an opaque occluder.
    bool noParticleGi_ = false;   // --no-particle-gi: see setNoParticleGi's own comment
    // VERIFICATION-ONLY: --particle-stress <N> <M> spawns N grid-arranged emitters capped at M particles.
    int  particleStressEmitters_ = 0;
    int  particleStressMaxParticles_ = 0;
    bool particleStressSecondEmitter_ = false;   // --particle-stress2: diag, see its own comment
    bool reflTest_ = false;       // --refl-test: are ray-traced reflections global?
    bool furnaceTest_ = false;    // --furnace-test: does the shading model conserve energy?
    bool furnaceGrid_ = false;    // --furnace-grid: the same question across roughness/metallic
    f32  furnaceTilt_ = 0.0f;     // --furnace-tilt DEG: read that grid at grazing incidence
    bool furnaceSun_ = false;     // --furnace-sun: the variant where only the DIRECT term is lit
    f32  sunAngle_ = -1.0f;       // --sun-angle: negative leaves the sky's own value alone
    bool ptFurnaceTest_ = false;  // --pt-furnace: the same question asked of the path tracer
    // --pt-scene-toggle-on/--pt-scene-toggle-off [N]: VERIFICATION ONLY. Simulates flipping the Path Tracing Quality combo N frames into a bounded run.
    int ptQualityRampEvery_ = 0;
    int ptQualityRampCountdown_ = 0;
    int giMethodCycleEvery_ = 0;       // --gi-method-cycle N
    int giMethodCycleCountdown_ = 0;
    int ptSceneToggleOnAutoFrames_ = 0;
    int ptSceneToggleOffAutoFrames_ = 0;
    // --sun-set-at N ELEV AZIM: VERIFICATION ONLY. Simulates dragging the Directional Light panel's sliders, N frames into a --frames run.
    int sunSetAtFrames_ = 0;
    f32 sunSetElevDeg_ = 0.0f, sunSetAzimDeg_ = 0.0f;
    // --sun-sweep START DEG: the DRAG, not the jump -- turns the sun's azimuth by sunSweepDeg_ every frame for the rest of the run.
    int sunSweepFrames_ = 0;
    f32 sunSweepDeg_ = 0.0f;
    // --sun-sweep-frames N: the drag LETS GO after N turns (0 = never), and the final angles are logged.
    int sunSweepTurnsLeft_ = 0;
    int giHistoryResetAtFrames_ = 0;
#if AVER_MODULE_SR
    // --aversr-cycle [N]: drive the on-then-off transition. Verification-only; 0 means never.
    int averSrCycleFrames_ = 0;
#endif
};
