// Render-settings mirror: CLI/menu overrides of GI/RT/PT/denoiser/upscaler/frame-interpolation settings, sky/sun/fog/post values.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct RenderSettingsState {
    // ---- FRAME BUDGET ----
    // Scales GI work to hit target frame time (RENDER.FRAMEBUDGETMS).
    f32  frameBudgetMs_ = 0.0f;       // RENDER.FRAMEBUDGETMS; <= 0 disables
    bool frameBudgetForced_ = false;  // --frame-budget: run even during capture
    // Derived Data Cache write-behind budget, mirrored for slider binding.
    f32 ddcRamBudgetMb_ = 256.0f;
    // ---- Place Actors / Message Log / Undo History panels (SandboxToolPanels.cpp) ----
    // Where the next Add lands: a drop point while one is set, else in front of the camera.
    std::optional<Vec3> spawnAtOverride_;
    bool vsyncOffRequested_ = false;
    bool vsyncOnRequested_ = false;
    // Top-of-atmosphere colour, mirroring rhi::SkyAtmosphere::sunColor's default (white).
    f32 sunColor_[3]={1.0f,1.0f,1.0f}, sunAmbient_=1.0f;
    // Editor's mirror of sky_.zenith/horizon, copied OVER sky_ every frame so rhi::SkyAtmosphere's defaults don't survive frame 1.
    f32 skyZenith_[3]={0.24f,0.45f,0.85f}, skyHorizon_[3]={0.72f,0.83f,0.95f};
    // A tint on the in-scattered sky (white = clear air), and an extinction per cm.
    f32 fogColor_[3]={1.0f,1.0f,1.0f}, fogDensity_=4e-6f;
#if AVER_MODULE_SCENE
    // OPT-IN (Height Fog panel): recomputes fogDensity from the streaming load boundary rather than a fixed density.
    bool matchFogToStreamRadius_ = false;
    f32  fogMatchTargetOpacity_ = 0.9f;   // opacity WANTED at the load boundary itself
#endif
    rhi::PostSettings post_{};
    // What this editor last handed the device, so per-frame push can tell when something ELSE (console post.* vars) changed it.
    rhi::PostSettings postPushed_{};
    bool postPushedValid_ = false;
    // Which post values came from argv, so a stored preference cannot outrank a flag the caller typed.
    bool postExposureFromCli_ = false, postBloomFromCli_ = false, postAutoExpFromCli_ = false;
    // The authored sky, sun and air. Sole owner of the sun's direction.
    rhi::SkyAtmosphere sky_{};
    f32 cloudTime_ = 0.0f;   // seconds of accumulated wind
    // File > Save Level As...
    bool unlit_=false;                 // View mode: Unlit (no shading, authored colour only)
    bool neuraaDebugView_=false;       // View mode: Edge Classes (NeuRAA); forces ray-driven
    int  msaaOverride_=0;            // --msaa N: apply a sample count at startup
    // --depth-prepass: depth-only pass ahead of ordinary opaque walk, OFF (default) never calls setDepthPrepassEnabled/drawMeshDepthPrepass.
    bool depthPrepassOverride_ = false;
    // --gbuffer: debug view override, OR'd together every frame with the view-mode dropdown.
    bool gbufferOverride_ = false;
    // --gbuffer-debug velocity|viewz|normals: which channel (if any) GBufferDebugFeature draws over the 3D viewport.
    GBufferDebugFeature::Mode gbufferDebugView_ = GBufferDebugFeature::Mode::Off;
    bool gbufferDebugAttached_ = false;
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    bool crowdGpuAttached_ = false;
#endif
    // All three use -1 for "flag not given", NOT 0 (0 = Quality::Off, must be expressible).
    int  giOverride_=-1;             // --gi [tier]: GI quality to apply at startup
    bool giForceOff_=false;          // --no-gi: force it off, whatever the default is
    int  rtOverride_=-1;             // --rt [tier]: ray tracing quality at startup
    int  ptOverride_=-1;             // --pt [tier]: PATH tracing quality at startup
    bool rtForceOff_=false;          // --no-rt: force it off, whatever the default is
    int  rtRaysOverride_=0;          // --rt-rays N: sun occlusion rays per pixel (0 = flag not given)
    // --gi-sky-occlusion-rays N: AMBIENT sky-visibility rays per pixel. -1 = flag not given (0 is a real value).
    int  giSkyOccRaysOverride_=-1;
    int  giSkyOccTileOverride_=-1;   // --gi-sky-occlusion-tile N
    // --sky-light N. NEGATIVE means absent: 0 is a real, meaningful request (no sky ambient).
    f32  skyLightOverride_=-1.0f;
    // --gi-intensity F. Negative means absent; 0 is a real request (bounce off, direct only).
    f32  giIntensityOverride_=-1.0f;
    // --sky-physical / --sky-authored: -1 absent, 0 authored, 1 physical. Sun elevation in degrees (-999 when absent).
    int  skyModelOverride_=-1;
    f32  sunElevationOverride_=-999.0f;
    int  rtPixelsPerRayOverride_=0;  // --rt-pixels-per-ray N: shadow tile edge (0 = flag not given)
    int  rtShadowDenoiseOverride_=-1; // --rt-shadow-denoise N: spatial radius (-1 = flag not given)
    int  rtRenderModeOverride_=-1;    // --rt-render-mode 0|1 (-1 = flag not given)
    int  rdStagesOverride_=-1;        // --rd-stages 0|1|2 (-1 = flag not given)
    int  ptBouncesOverride_=-1;       // --pt-bounces N (-1 = flag not given)
    int  layeredBsdfOverride_=-1;     // --layered-bsdf N (-1 = flag not given)
    f32  coatWeight_=0.0f;            // --coat W [R] [F0]: 0 = flag not given, and no coat anywhere
    f32  coatRough_=0.1f;
    f32  coatF0_=0.04f;
    int  giUpdateIntervalOverride_=0; // --gi-update-interval N: GI revoxelise interval (0 = flag not given)
    // --gi-mode N: indirect-diffuse estimator (0 = voxel cones, 1 = ReSTIR GI). Sentinel is -1 (0 is a real value).
    int  giModeOverride_=-1;
    // --restir-visibility none|reconstructed|half|full (Settings::giRestirVisibility). Sentinel is -1 (0 is a real value).
    int  restirVisibilityOverride_=-1;
    int  denoiserOverride_=-1;       // --denoiser 0|1|2: -1 is "flag not given"; see setDenoiser
    f32  renderScaleOverride_=1.0f;  // --render-scale F: scene render resolution as a fraction of present, clamped [0.25,1]
#if AVER_MODULE_SR
    // --aversr LEVEL / the render-settings quality combo. Off (default) = no AverSR.
    aver::sr::Quality averSrQuality_ = aver::sr::Quality::Off;
    // True when --aversr set the value above, so loadEditorPreferences must not let a stored preference overwrite the CLI flag.
    bool averSrFromCli_ = false;
    bool averSrCliAuto_ = false;      // --aversr auto: CLI wins even though auto has no single level to pin.
    int  averSrCliLevel_ = -1;        // --aversr LEVEL (not auto): the pinned level.
    // User's Display preference, superset of averSrQuality_ (Auto and Manual besides the four named levels).
    editor::AverSrChoice averSrChoice_ = editor::AverSrChoice::Auto;
    // Set once when a stored display.renderScalePending cookie is found still armed at load; forces Off for the REST OF THIS SESSION.
    bool averSrCookieTripped_ = false;
    // Set true when migrateAverSrChoice ran on a genuinely ABSENT display.aversrChoice and landed on Auto.
    bool averSrMigrationNoteArmed_ = false;
    // Set the first time updateAverSrAuto is about to apply a non-Off level THIS SESSION.
    bool averSrArmedNonOffOnce_ = false;
    // Fires the "[AverSR] ... scene WxH -> present WxH" line exactly once per process.
    bool averSrStartupLogged_ = false;
#if AVER_MODULE_VOXI
    voxi::AverSrSource averSrSource_ = voxi::AverSrSource::Auto;
#endif
    // Project's own AverSR default combo's live edit state. Unconditionally reset to -1 on project open (not inherit from previous project).
    int  averSrProjectDefault_ = -1;
    std::string neuraaCaptureDir_;          // --neuraa-capture DIR COUNT (training data)
    u32  neuraaCaptureCount_ = 0;
    bool neuraaCaptureStarted_ = false;
    bool temporalAaEnabled_ = true;   // the project's RENDER.TAA (unstated = on); --no-taa / --taa win over it
    bool taaFromCli_ = false;
    int particleGpuArg_ = -1;   // --particles-cpu / --particles-gpu; -1 follows RENDER.PARTICLEGPU
    bool neuraaEnabled_ = false;      // the project's RENDER.NEURAA (unstated = off)
    bool neuraaFromCli_ = false;
    // Edge AA (FXAA-class) in FSR's first pass: --edge-aa or Display > Edge anti-aliasing.
    bool edgeAaEnabled_ = false;
    f32  fsrSharpness_ = 0.2f;   // RCAS stops (0 = sharpest); Display > Sharpening
    // ---- frame interpolation (docs/rendering/NEURAFI.md) ----
    // Who decides, highest first: --frame-interp, project RENDER.FRAMEINTERP, Editor Preference (off by default).
    int  frameInterpCli_ = -1;
    bool frameInterpWhileEditing_ = false;   // Editor Preferences, display.frameInterpWhileEditing
    u32  msaaPushed_ = 0;                    // sample count last given to the device (onUpdate)
    // The gather's path and in-engine training (Editor Preferences display.frameInterpTrajectory / display.frameInterpTrain).
    int  frameInterpTrajectory_ = 2;         // neurafi::Trajectory (0 straight, 1 quadratic, 2 learned)
    bool frameInterpTrain_ = false;
    int  frameInterpTrajectoryCli_ = -1;     // --frame-interp-trajectory; -1 = not given
    bool frameInterpTrainCli_ = false;       // --frame-interp-train
    // The status bar's frame rate counts interpolated frames or real frames only; chosen by clicking it.
    bool fpsCountsInterpolated_ = true;
#endif
    int  rdAblate_=0;                // --rd-ablate: AVER_RD_ABLATE for PSRayDriven, 0 = normal
    f32  rtDenoiseMotionTaper_=0.0f; // --rt-denoise-motion: 0 = no taper, the shipped default
    bool ptSceneViewYieldLogged_=false;   // say once, per mode change, that the PT view yielded
    int  refractionOverride_=-1;     // --refraction: -1 not given, else the mode
    f32  refractionStrengthOverride_=-1.0f;
    f32  refractionFadeOverride_=-1.0f;
    bool msOverride_=false;          // --ms: force the mesh shader geometry path
    // --mesh-heap default|upload (W4): true (default) = static mesh vertex/index buffers on the Default heap (VRAM);
    // false keeps them on the Upload heap (see rhi::IDevice::setStaticMeshHeapDefault).
    bool meshHeapDefault_ = true;
    // --lod-share-vertices 0|1 (W11): true (default) shares LOD0's vertex buffer with every coarser LOD via
    // createMeshSharingVertices, falling back to independent buffers on refusal (unsupported backend,
    // compute-written vertices); false gives each LOD its own copy of LOD0's untouched data (VRAM).
    // Read inside loadProjectMeshes' LOD-ladder loop, once per mesh at load time.
    bool lodShareVertices_ = true;
    bool nrd2MenuWired_ = false;
    bool giDebugView_=false; Vec3 giCenter_{0,0,300}; f32 giExtent_=1200.0f;   // cm
    bool giConeTraceOff_=false;   // --no-gi-cone: see setGiConeTraceOff's own comment
    // --view-mode undenoised / the dropdown's independent "Undenoised" toggle: forces a bundle of existing runtime knobs off.
    // NOT PERSISTED: reasserted every frame.
    bool undenoised_=false;
    // True when --view-mode or --unlit set the view for this run: stored viewport.wireframe/viewport.unlit don't override it on load.
    bool viewModeFromCli_=false;
#if AVER_MODULE_VOXI
    bool voxiAttached_=false;
    // Viewport's ray-hit/triangles debug view (Ray Hit: Instances/Materials/Distance, Triangles), or None. NOT PERSISTED: reasserted every frame.
    voxi::VoxiRenderer::ViewDebug debugView_ = voxi::VoxiRenderer::ViewDebug::None;
    // Last frame's EFFECTIVE voxi::Settings::rtRenderMode, after onUpdate's auto-switch. Lets onUpdate tell an actual mode transition from an ordinary frame.
    i32 lastEffectiveRtRenderMode_ = -1;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    bool particlesAttached_=false;
#endif
    // These two stay OUTSIDE the guard: the flag is parsed either way.
    bool  waterEnabled_  = false;
    f32   waterHeightCm_ = 0.0f;
    // REQUESTED state (--pt-scene at startup, or the settings-page Quality combo later); ptSceneView_ != nullptr is the ACTUAL one.
    bool ptSceneViewWantEnabled_ = false;
    // --pt-scene WAS GIVEN ON THE COMMAND LINE. Sticky for the session, separate from the want flag above.
    bool ptSceneViewFromCli_ = false;
    bool ptSceneViewUnavailable_ = false;   // init() refused once this session -- stop re-asking
    // A1: true while syncPtSceneView() holds ptSceneViewWantEnabled_ down because ray-driven primary visibility is painting the scene.
    bool ptSceneViewSuppressedByRayDriven_ = false;
    // Wireframe/a G-buffer debug view withdrew the path-traced view's want; remembered so leaving the view mode gives it back.
    bool ptSceneViewSuppressedByViewMode_ = false;
    // True while a stored non-unity render scale is on trial this session.
    bool renderScaleCookieArmed_ = false;
    // Landscape CLI override path. Must exist unconditionally for command-line parsing.
    std::string landscapeCliOverride_;
#if AVER_MODULE_SCENE
    bool hasLevelSun_ = false;
    bool hasLevelSky_ = false;
    bool hasLevelFog_ = false;
    // True if level record mentioned clouds (not just fmt::OcWorldEnv::hasClouds).
    bool hasLevelClouds_ = false;
#endif
};
