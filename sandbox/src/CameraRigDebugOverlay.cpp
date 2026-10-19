// Camera rig viewport overlay. See the header for the wiring.

#include "CameraRigDebugOverlay.hpp"

#if AVER_MODULE_CAMERA && AVER_MODULE_SCENE

#include "aver/scene/World.hpp"

#include <cmath>

namespace aver::editor {

namespace {

void line(CameraDebugFrame& f, const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
    f.lines.push_back(rhi::LineVertex{a.x, a.y, a.z, r, g, bl});
    f.lines.push_back(rhi::LineVertex{b.x, b.y, b.z, r, g, bl});
}

void mark(CameraDebugFrame& f, const Vec3& c, f32 size, f32 r, f32 g, f32 b) {
    line(f, c - Vec3{size, 0, 0}, c + Vec3{size, 0, 0}, r, g, b);
    line(f, c - Vec3{0, size, 0}, c + Vec3{0, size, 0}, r, g, b);
    line(f, c - Vec3{0, 0, size}, c + Vec3{0, 0, size}, r, g, b);
}

// A circle of `radius` around `c` in the plane spanned by the unit vectors `u` and `v`.
void circle(CameraDebugFrame& f, const Vec3& c, const Vec3& u, const Vec3& v, f32 radius, f32 r, f32 g, f32 b, int segments = 24) {
    Vec3 prev = c + u * radius;
    for (int i = 1; i <= segments; ++i) {
        const f32 a = kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
        const Vec3 p = c + u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
        line(f, prev, p, r, g, b);
        prev = p;
    }
}

void sphere(CameraDebugFrame& f, const Vec3& c, f32 radius, f32 r, f32 g, f32 b) {
    circle(f, c, Vec3{1, 0, 0}, Vec3{0, 1, 0}, radius, r, g, b, 16);
    circle(f, c, Vec3{1, 0, 0}, Vec3{0, 0, 1}, radius, r, g, b, 16);
    circle(f, c, Vec3{0, 1, 0}, Vec3{0, 0, 1}, radius, r, g, b, 16);
}

}  // namespace

void cameraDebugFeed(const camera::CameraRigSystem::DebugView& d, const CameraDebugOptions& opts, CameraDebugFrame& out) {
    if (opts.pivot) {
        mark(out, d.pivot, 10.0f, 1.0f, 1.0f, 1.0f);
        mark(out, d.laggedPivot, 8.0f, 1.0f, 0.9f, 0.2f);
        if (dist(d.pivot, d.laggedPivot) > 1.0f) line(out, d.pivot, d.laggedPivot, 1.0f, 0.9f, 0.2f);     // how far the lag trails
        if (d.lead.size() > 1.0f) line(out, d.pivot, d.pivot + d.lead, 0.2f, 0.9f, 0.9f);                // the look-ahead lead
    }
    if (opts.arm && d.hasProbe) {
        const bool shortened = d.armResolved < d.armWanted - 0.01f;
        if (shortened) {
            line(out, d.camera, d.desiredEnd, 0.5f, 0.5f, 0.5f);                                          // the part the probe took away
            line(out, d.laggedPivot, d.camera, 1.0f, 0.55f, 0.1f);
        } else {
            line(out, d.laggedPivot, d.camera, 0.3f, 0.9f, 0.3f);
        }
        mark(out, d.desiredEnd, 6.0f, 0.5f, 0.5f, 0.5f);
        sphere(out, d.camera, d.probeRadius, shortened ? 1.0f : 0.3f, shortened ? 0.55f : 0.9f, shortened ? 0.1f : 0.3f);
        if (d.didHit) mark(out, d.hit, 8.0f, 1.0f, 0.2f, 0.2f);
    } else if (opts.arm) {
        line(out, d.laggedPivot, d.camera, 0.3f, 0.9f, 0.3f);                                             // no arm: the pivot to the eye
    }
    if (opts.fade && d.hasProbe) circle(out, d.laggedPivot, Vec3{1, 0, 0}, Vec3{0, 1, 0}, d.fadeEndCm, 0.9f, 0.3f, 0.9f, 32);
    if (opts.limits) {
        // The arc the pitch may sweep in the vertical plane through the camera's heading, around the lagged pivot.
        const Vec3 h = Vec3{d.fwd.x, d.fwd.y, 0.0f}.getSafeNormal();
        if (h.sizeSquared() > 0.0f) {
            const f32 radius = 150.0f;
            const int steps = 16;
            Vec3 prev = d.laggedPivot + (h * std::cos(d.pitchMinRad) + Vec3{0, 0, 1} * std::sin(d.pitchMinRad)) * radius;
            line(out, d.laggedPivot, prev, 0.6f, 0.6f, 0.9f);
            for (int i = 1; i <= steps; ++i) {
                const f32 a = d.pitchMinRad + (d.pitchMaxRad - d.pitchMinRad) * static_cast<f32>(i) / static_cast<f32>(steps);
                const Vec3 p = d.laggedPivot + (h * std::cos(a) + Vec3{0, 0, 1} * std::sin(a)) * radius;
                line(out, prev, p, 0.6f, 0.6f, 0.9f);
                prev = p;
            }
            line(out, d.laggedPivot, prev, 0.6f, 0.6f, 0.9f);
        }
    }
}

void cameraDebugCollect(const CameraDebugOptions& opts, CameraDebugFrame& out) {
    out.clear();
    if (!opts.any()) return;
    scene::World& w = scene::World::instance();
    const scene::Entity live = camera::cameraRigSystem().activeRig();
    camera::CameraRigSystem::DebugView d;
    if (live != scene::kInvalidEntity && w.valid(live) && camera::cameraRigSystem().debug(live, d)) cameraDebugFeed(d, opts, out);
}

} // namespace aver::editor

#endif  // AVER_MODULE_CAMERA && AVER_MODULE_SCENE
