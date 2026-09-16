// Marquee selection: the pure 2D geometry, deliberately kept apart from SandboxApp.
//
// A PURE HEADER, for ViewportPick.hpp's exact reason (see that file's own top comment): no ImGui
// types, no SandboxApp state, no scene::World -- plain screen-space numbers in, a plain bool or a
// plain rect out, so a headless test can reach every one of these without a live editor behind it.
// Everything that DOES need SandboxApp state (which entities are eligible, how to project one to
// screen space, what to do with a hit) stays in SandboxViewport.cpp, which is what actually walks
// the scene and calls these once per candidate.
#pragma once
#include "aver/core/Types.hpp"
#include <cmath>

namespace aver::editor {

// True once (x, y) has moved more than `thresholdPx` from the point the drag started at -- the
// distance a click has to clear before it stops being "a click that wobbled a pixel or two" and
// starts being a marquee. ~4px matches the tolerance every other click-vs-drag test in this editor
// already uses at cursor scale (pickAxis's own hit radius is measured in the same units).
inline bool marqueeExceedsThreshold(f32 x0, f32 y0, f32 x, f32 y, f32 thresholdPx = 4.0f) {
    const f32 dx = x - x0, dy = y - y0;
    return dx * dx + dy * dy > thresholdPx * thresholdPx;
}

// A drag rectangle in whichever direction it was actually dragged, normalised to (lo, hi) per axis.
// Dragging bottom-right to top-left is exactly as valid a marquee as the other three directions, and
// every consumer below wants the box, not the gesture that drew it.
inline void normalizeMarqueeRect(f32 x0, f32 y0, f32 x1, f32 y1, f32& loX, f32& loY, f32& hiX, f32& hiY) {
    loX = std::fmin(x0, x1); hiX = std::fmax(x0, x1);
    loY = std::fmin(y0, y1); hiY = std::fmax(y0, y1);
}

// True when an entity's world AABB -- given as up to 8 corners already projected to screen space
// (a corner behind the camera is simply not among them, hence `cornerCount` rather than a fixed 8)
// -- intersects the marquee rectangle [loX,hiX] x [loY,hiY].
//
// THE CORNERS' OWN SCREEN-SPACE BOUNDING BOX IS WHAT IS TESTED, not each corner individually against
// the rect: a box far larger than the marquee, which the marquee only clips through with no corner
// of its own actually inside the rectangle (dragging a small rectangle across part of a big wall),
// must still count as caught -- exactly as it would in any editor this one takes its cues from. Two
// axis-aligned rectangles overlap unless one is entirely to a side of the other on some axis, which
// is the four-comparison test below.
inline bool projectedAabbIntersectsRect(const f32 cornerX[8], const f32 cornerY[8], int cornerCount,
                                         f32 loX, f32 loY, f32 hiX, f32 hiY) {
    if (cornerCount <= 0) return false;
    f32 mnX = cornerX[0], mxX = cornerX[0], mnY = cornerY[0], mxY = cornerY[0];
    for (int i = 1; i < cornerCount; ++i) {
        mnX = std::fmin(mnX, cornerX[i]); mxX = std::fmax(mxX, cornerX[i]);
        mnY = std::fmin(mnY, cornerY[i]); mxY = std::fmax(mxY, cornerY[i]);
    }
    return mnX <= hiX && mxX >= loX && mnY <= hiY && mxY >= loY;
}

} // namespace aver::editor
