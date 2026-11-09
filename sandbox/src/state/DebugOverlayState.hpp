// Debug overlays and diagnostics panels: overlay line meshes, nav/collider overlay caches, profiler/VRAM/neural-viz toggles.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct DebugOverlayState {
#if AVER_MODULE_SCENE
    editor::AiDebugOptions aiDebug_;          // Show > AI: sight cones, hearing, path, steering, BT state, cover
    rhi::LineHandle aiDebugMesh_ = 0;         // this frame's AI debug lines (rebuilt every frame an option is on)
    editor::MotionDebugOptions motionDebug_;   // Show > Motion Matching: trajectories, matched row, labels
    editor::MotionDebugFrame motionDebugFrame_;
    rhi::LineHandle motionDebugMesh_ = 0;     // this frame's motion-matching lines
#if AVER_MODULE_CAMERA && AVER_MODULE_SCENE
    editor::CameraDebugOptions cameraDebug_;    // Show > Camera Rigs
    editor::CameraDebugFrame cameraDebugFrame_;
    rhi::LineHandle cameraDebugMesh_ = 0;       // this frame's camera-rig lines
    scene::Entity cameraLookRig_ = scene::kInvalidEntity;   // Details > Camera Rig > Look through: pilots the viewport with the rig's preview pose until Esc
#endif
    rhi::LineHandle classMarkerMesh_ = 0;     // this frame's graph-class root markers
#endif
#if AVER_MODULE_SYNAPSE
    // The baked grid and the line mesh drawn from it. Rebuilt on every bake/load; mesh destruction needed to avoid upload buffer leak.
    fmt::OcNavData  nav_;
    rhi::LineHandle navMesh_=0;
    bool showNav_=false;
    bool navRegionColours_=true;
    // --bake-nav: bake once at startup, then carry on. Deferred to a frame because bake reads PHYSICS BODIES, built by applyProject at init.
    bool navBakeOnStart_=false;
    bool navBakeDone_=false;
    bool navLoadPending_=false;
    f32  navBakeCell_=50.0f;
#endif
    // ---- the state belonging to the three non-navigation features above ----
    // Collider overlay: the world-space AABB of every physics body. TOGGLE UNGUARDED; MESH guarded on AVER_MODULE_PHYSICS.
    // Toggle persisted through editor.ini (runs in every config); mesh genuinely built from bodies.
    bool showColliders_=false;
#if AVER_MODULE_PHYSICS
    // Two meshes: static bodies in colliderMesh_, kinematic/dynamic in colliderMoverMesh_.
    rhi::LineHandle colliderMesh_=0;
    rhi::LineHandle colliderMoverMesh_=0;
    // What colliderMesh_ was built from, so rebuildColliderOverlay can skip unchanged frames.
    // AllKnown: every STATIC body is one the editor tracks, so the sig covers all change paths.
    bool colliderOverlayBuilt_ = false;
    bool colliderOverlayAllKnown_ = false;
    u64  colliderOverlaySig_ = 0;
    u64  colliderRev_ = 0;
    // The moving mesh's state: handles, tracking, post-play refresh, and AABBs for rebuild comparison.
    std::vector<int32_t> colliderMovers_;
    std::vector<f32> colliderMoverBoxes_, colliderMoverBoxesNext_;
    std::vector<rhi::LineVertex> colliderMoverLines_;
    bool colliderMoversTracked_ = false;
    bool colliderMoversWereLive_ = false;
    // The static audit: the handles colliderMesh_ drew, with the AABB each was drawn at. In Play, a rotating slice is re-read each frame.
    std::vector<int32_t> colliderStatics_;
    std::vector<f32> colliderStaticBoxes_;
    usize colliderAuditCursor_ = 0;
#endif
#if AVER_WITH_IMGUI
    // The profiler panel (Window > GPU Profiler): GPU pass table and frame time/Play CPU phases.
    bool showProfiler_=false;
    // The status bar's VRAM readout: the last videoMemory() answer, polled twice a second, and over-budget warning flag.
    rhi::VideoMemoryInfo vram_{};
    f32  vramPollS_ = 0.0f;
    bool vramOverLogged_ = false;
    // Window > Neural Visualiser. Choices applied every frame by onUpdate. Closing the window doesn't turn them off (its "Off" entries do).
    bool showNeuralViz_ = false;
    int  neurafiVizMode_ = 0;          // neurafi::Visualisation
    f32  neurafiVizOpacity_ = 0.85f;
    f32  neurafiVizScalePx_ = 0.5f;    // path bend at full heat
    bool neurafiShowGeneratedOnly_ = false;
    int  neuracViewMode_ = 0;          // VoxiRenderer::setNeuRaCView's mode
    bool neuracViewGrid_ = false;
    // The References panel. refPanelScanned_ distinguishes "opened but never asked" from "asked and found nothing".
    bool showReferences_ = false;
    bool refPanelScanned_ = false;
    std::string refPanelAsset_;
    std::vector<std::string> refPanelResults_;
#endif
#if AVER_MODULE_SOFTBODY_SCENE
    // Show > Soft Bodies: strain / pins / pieces / wind / budget overlay. Not persisted. The line mesh is rebuilt each frame.
    editor::SoftBodyDebugOptions softBodyDebug_;
    editor::SoftBodyDebugFrame softBodyDebugFrame_;
    rhi::LineHandle softBodyDebugMesh_ = 0;
#endif
#if AVER_MODULE_VEHICLE_SCENE
    // Show > Vehicles: suspension / forces / slip / chassis / velocity / labels, plus the .ocvehicle editor's spec preview. Not persisted. The line mesh is rebuilt each frame.
    editor::VehicleDebugOptions vehicleDebug_;
    editor::VehicleDebugFrame vehicleDebugFrame_;
    rhi::LineHandle vehicleDebugMesh_ = 0;
#endif
#if AVER_MODULE_PHYSICS
    // Show > Physics: Jolt's debug renderer (bodies, constraints, contacts, static triangles, labels ...). Not persisted. The line mesh is rebuilt each frame.
    editor::PhysicsDebugOptions physicsDebug_;
    editor::PhysicsDebugFrame physicsDebugFrame_;
    rhi::LineHandle physicsDebugMesh_ = 0;
    rhi::LineHandle scriptDebugMesh_ = 0;   // DebugDraw's lines, replaced each frame (ScriptDebugLines.hpp)
#endif
    bool viewportIconsReady_ = false;
    editor::ViewportIconRenderer::IconHandle playerStartIcon_ = editor::ViewportIconRenderer::kNoIcon;
    // The capsule/arrow line meshes, built once and placed with a world matrix per frame. Independent of viewportIconsReady_ (that flag gates only the SPRITE).
    rhi::LineHandle playerStartCapsule_ = 0, playerStartCapsuleSel_ = 0, playerStartArrow_ = 0;
#if AVER_MODULE_SCENE
#if AVER_MODULE_SCENE
    bool showWorldOrigin_ = false;
    rhi::LineHandle seqPathMesh_ = 0;
    u64 seqPathStamp_ = 0;
#endif
#endif
};
