#pragma once
// The particle preview's own logic, with no ImGui: the path a moving test emitter follows, and the projection the overlay uses to
// draw the bounds spheres and that path over the preview picture.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>

namespace aver::editor::pxmodel {

// A test emitter that moves, so trails and spawn.inheritvelocity have something to follow. Not saved with the effect.
enum class PreviewMotion : u8 { None = 0, Circle = 1, Line = 2 };

// Position and velocity of the moving emitter at time `t`. Circle: a circle of `radius` cm in the XY plane at `speed` cm/s.
// Line: back and forth along X between -radius and +radius at `speed` cm/s. None, or a degenerate radius or speed: at the origin, still.
inline void emitterMotion(PreviewMotion kind, f32 radius, f32 speed, f32 t, Vec3& pos, Vec3& vel) {
    pos = Vec3{0, 0, 0};
    vel = Vec3{0, 0, 0};
    if (kind == PreviewMotion::None || !(radius > 1e-3f) || !(speed > 1e-3f) || !std::isfinite(t)) return;
    if (kind == PreviewMotion::Circle) {
        const f32 w = speed / radius;
        pos = Vec3{radius * std::cos(w * t), radius * std::sin(w * t), 0.0f};
        vel = Vec3{-speed * std::sin(w * t), speed * std::cos(w * t), 0.0f};
        return;
    }
    // a triangle wave: one full period is 4 * radius / speed
    const f32 period = 4.0f * radius / speed;
    const f32 ph = std::fmod(t, period) / period;   // 0..1
    if (ph < 0.25f)      { pos.x = radius * (ph * 4.0f);          vel.x = speed; }
    else if (ph < 0.75f) { pos.x = radius * (2.0f - ph * 4.0f);   vel.x = -speed; }
    else                 { pos.x = radius * (ph * 4.0f - 4.0f);   vel.x = speed; }
}

// The preview background choices: the studio gradient or a flat display-value colour.
enum class PreviewBackdrop : u8 { Studio = 0, Dark = 1, Grey = 2, Light = 3 };
inline constexpr int kPreviewBackdropCount = 4;
inline const char* previewBackdropName(PreviewBackdrop b) {
    switch (b) { case PreviewBackdrop::Dark: return "Dark"; case PreviewBackdrop::Grey: return "Grey"; case PreviewBackdrop::Light: return "Light"; default: return "Studio"; }
}
// False for Studio (the gradient); otherwise fills `rgb` with the flat colour.
inline bool previewBackdropColor(PreviewBackdrop b, f32 rgb[3]) {
    f32 v;
    switch (b) { case PreviewBackdrop::Dark: v = 0.02f; break; case PreviewBackdrop::Grey: v = 0.35f; break; case PreviewBackdrop::Light: v = 0.80f; break; default: return false; }
    rgb[0] = rgb[1] = rgb[2] = v;
    return true;
}

// World point -> pixel inside a rectangle, with the preview's row-vector view-projection (clip = [x y z 1] * M, y up in NDC).
// False for a point behind the camera.
inline bool projectToRect(const f32 viewProj[16], const Vec3& p, f32 x0, f32 y0, f32 w, f32 h, f32& sx, f32& sy) {
    const f32 cw = p.x * viewProj[3] + p.y * viewProj[7] + p.z * viewProj[11] + viewProj[15];
    if (!(cw > 1e-4f)) return false;
    const f32 cx = (p.x * viewProj[0] + p.y * viewProj[4] + p.z * viewProj[8] + viewProj[12]) / cw;
    const f32 cy = (p.x * viewProj[1] + p.y * viewProj[5] + p.z * viewProj[9] + viewProj[13]) / cw;
    sx = x0 + (cx * 0.5f + 0.5f) * w;
    sy = y0 + (0.5f - cy * 0.5f) * h;
    return true;
}

} // namespace aver::editor::pxmodel
