#pragma once
// Where a world label's plane ends up for a camera (parity E6): billboarding and constant-screen-size, as plain CPU maths so
// the renderer, a test and any other host agree. All matrices are row-major with row vectors (p * M), the convention of
// rhi::PerFrameCB; a plane point (x, y, 0, 1) times the label's matrix is its position in the scene frame.
#include "aver/core/Types.hpp"
#include "aver/ui/UiDrawList.hpp"

namespace aver::render::ui {

// What a camera gives IDevice::camera(): the view-projection, the inverse of the same with the view's translation removed
// (clip space to a world OFFSET FROM the eye), and the eye. viewportHeight is the scene target's height in pixels.
struct WorldTextCamera {
    f32 viewProj[16] = {};
    f32 invViewProjRel[16] = {};
    f32 eye[3] = {};
    u32 viewportHeight = 0;
};

// One placed vertex, as the world-text pipeline reads it: position, texture coordinate, premultiplied colour (24 bytes).
struct WorldTextVertex {
    f32 x = 0, y = 0, z = 0;
    f32 u = 0, v = 0;
    u32 rgba = 0;
};
static_assert(sizeof(WorldTextVertex) == 24, "the world-text input layout names these offsets");

// The camera's world right, up and forward (away from the eye) axes, unit length, recovered exactly from invViewProjRel.
// False for a degenerate camera.
bool worldTextCameraAxes(const WorldTextCamera& cam, f32 right[3], f32 up[3], f32 forward[3]);

// World units one screen pixel spans at `point` (the plane through it facing the camera). 0 when the point is behind the
// camera, the viewport is empty, or the camera is degenerate.
f32 worldTextUnitsPerPixel(const WorldTextCamera& cam, const f32 point[3]);

// The matrix a label is drawn with: its own, or, for billboards, turned to the camera (keeping its scale), and for a fixed
// pixel size, scaled so a plane unit spans `fixedPxPerUnit` pixels at its origin. False when the label cannot be placed
// (behind the camera for a billboard or fixed size, or a degenerate camera): it is skipped for the frame.
bool worldTextMatrix(const aver::ui::UiWorldText& t, const WorldTextCamera& cam, f32 out[16]);

// The label's origin depth along the camera's view (clip w): larger is farther. Used to sort labels back to front.
f32 worldTextDepth(const f32 world[16], const WorldTextCamera& cam);

// Places `vertexCount` plane vertices through `matrix` (the plane's x, y; z = 0).
void worldTextPlace(const aver::ui::UiVertex* in, usize vertexCount, const f32 matrix[16], WorldTextVertex* out);

} // namespace aver::render::ui
