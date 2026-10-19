#pragma once
// Camera rig viewport overlay (Show > Camera Rigs): for the live rig the pivot cross, the lagged pivot (Unreal's debug lag
// markers), the arm segment (green when clear, orange when the probe shortened it), the probe sphere at the camera and at the hit,
// the desired end point, the look-ahead vector, the collision-fade radius and the pitch limits as an arc.
//
// Pure feed -> rhi::LineVertex, drawn like the motion overlay (see MotionDebugOverlay.hpp): once per frame while an option is on,
// cameraDebugCollect(opts, frame), upload frame.lines as a line mesh and draw it with an identity matrix. Only the live rig has
// anything to show, and only while Play runs the camera system.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#if AVER_MODULE_CAMERA && AVER_MODULE_SCENE
#  include "aver/camera/CameraRigSystem.hpp"
#endif

#include <vector>

namespace aver::editor {

struct CameraDebugOptions {
    bool pivot = false;       // the pivot and lagged-pivot crosses, and the look-ahead vector
    bool arm = false;         // the arm segment, the desired end and the probe sphere / hit
    bool fade = false;        // the collision-fade radius
    bool limits = false;      // the pitch limits as an arc
    bool any() const { return pivot || arm || fade || limits; }
};

struct CameraDebugFrame {
    std::vector<rhi::LineVertex> lines;   // two vertices per line, colours 0..1, world space
    void clear() { lines.clear(); }
};

#if AVER_MODULE_CAMERA && AVER_MODULE_SCENE
// Appends one rig's debug data. Exposed so a test can feed a hand-built view.
void cameraDebugFeed(const camera::CameraRigSystem::DebugView& d, const CameraDebugOptions& opts, CameraDebugFrame& out);

// Clears `out`, then feeds the live rig of the process-global camera system.
void cameraDebugCollect(const CameraDebugOptions& opts, CameraDebugFrame& out);
#endif

} // namespace aver::editor
