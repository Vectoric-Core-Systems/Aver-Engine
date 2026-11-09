// Viewport and camera: fly camera, view overrides, gizmo handles and drag state, snapping, viewport rect, focus/hover flags, active tool and editor mode.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct ViewportState {
#if AVER_WITH_IMGUI
    struct ViewportCtx { bool armed = false; bool hasHit = false; Vec3 camPos{}; Vec3 hit{}; };
    ViewportCtx vpCtx_;
#endif
#if AVER_MODULE_SCENE
    bool showDecals_ = true;                  // Show > Decals: dim boxes for every decal, not just the selected one
#endif
    Tool tool_ = Tool::Move;   // see initialTool_ for why this is Move and not Select
    // Which mode the viewport is in. UNGUARDED: Landscape is AVER_MODULE_LANDSCAPE-only, but mode switch/dispatch read it unconditionally.
    EditorMode mode_ = EditorMode::Select;
#if AVER_MODULE_LANDSCAPE
    SculptTool sculptTool_ = SculptTool::Raise;
#endif
    EditorMode lastNonSelectMode_ = EditorMode::Landscape;
    // Free-fly editor camera: position plus yaw/pitch.
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32 yaw_ = 0.0f, pitch_ = 0.0f, flySpeed_ = 800.0f, lookSpeed_ = 0.005f;   // cm/s
    // A sequence camera showing instead of the free-fly one this frame (set where the camera is
    // pushed; camPos_/yaw_/pitch_ are persisted and stay the editor's own).
    bool viewOverride_ = false;
    Vec3 viewPosOv_{0.0f, 0.0f, 0.0f}, viewFwdOv_{1.0f, 0.0f, 0.0f};
    // What "1" on the camera-speed dial means, in cm/s (movement integration uses cm/s).
    // DISPLAY scale only: divide by it so default speed reads "1" not "800".
    static constexpr f32 kCamSpeedUnit = 800.0f;
    bool flying_ = false;
    // editor viewport aids
    rhi::LineHandle gridMesh_=0;
    rhi::LineHandle gzMove_[3]={0,0,0}, gzMoveHi_[3]={0,0,0};
    rhi::LineHandle gzRot_[3]={0,0,0}, gzRotHi_[3]={0,0,0};
    rhi::LineHandle gzScale_[3]={0,0,0}, gzScaleHi_[3]={0,0,0};
    bool showGrid_=true, wireframe_=false;
    // Show > Camera Coordinates, and the Show > Go To fields (cm).
    bool showCamCoords_=false;
    f32 goToPos_[3]={0.0f, 0.0f, 0.0f};
    // gizmo interaction
    bool dragging_=false; int activeAxis_=-1, hoverAxis_=-1;
    // Rotation drag accumulators for snap working on total swept angle rather than per-frame delta.
    f32 rotDragDeg_=0.0f, rotAppliedDeg_=0.0f;
    f32 prevMouseX_=0, prevMouseY_=0;
    // snapping (off by default; toggled from the toolbar carets)
    bool snapMove_=false, snapRot_=false, snapScale_=false;
    f32  vpBarRightW_ = 0.0f;   // width the viewport options bar's right cluster measured last frame
    f32 moveSnap_=1.0f, rotSnap_=15.0f, scaleSnap_=0.25f;
    // frame state
    f32 dpi_=1.0f;
    // 3D viewport rect, in backbuffer pixels. Latched by buildUI, consumed the next frame.
    f32 vpX_=0, vpY_=0, vpW_=1600, vpH_=900;
    // Viewport Show flags. Both default ON.
    bool showStaticMeshes_=true;
    bool showAtmosphere_=true;
    // Gizmo coordinate space. Honoured by drawGizmo, pickAxis, applyMove and applyRotate; SCALE tool always uses local.
    bool worldSpace_=true;
    unsigned centralDock_ = 0;    // the dock node an opened asset editor lands in
    bool levelFocused_ = true;    // the Level tab holds the keyboard
    // The two panels a selection is also made from, so the edit verbs reach a selection made there.
    bool outlinerFocused_ = false;
    bool detailsFocused_  = false;
    bool levelHovered_ = true;    // the cursor is over the Level tab and it is topmost there
    bool levelImageHovered_ = false;   // ... and on the scene picture, not on a viewport toolbar or popup
    bool levelVisible_ = true;    // the Level tab is the selected tab
    Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};
};
