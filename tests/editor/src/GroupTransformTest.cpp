// GroupTransform.hpp -- rotate / scale about a shared pivot, the math behind the multi-selection
// Rotate and Scale gizmos. Pure and header-only.
#include "../../../sandbox/src/GroupTransform.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near3(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}
static bool nearQ(const Quat& a, const Quat& b, f32 eps = 1e-4f) {
    const f32 d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);   // q and -q are one rotation
    return std::fabs(d - 1.0f) < eps;
}

int main() {
    AVER_INFO("=== GroupTransform ===");
    const f32 halfPi = 1.5707963f;
    const Vec3 up{0, 0, 1};

    {
        Transform t; t.position = Vec3{2, 0, 0};
        const Transform r = rotateAboutPivot(t, Vec3{1, 0, 0}, Quat::fromAxisAngle(up, halfPi));
        check(near3(r.position, Vec3{1, 1, 0}), "90 deg about Z around (1,0,0) swings (2,0,0) to (1,1,0)");
        check(nearQ(r.rotation, Quat::fromAxisAngle(up, halfPi)), "an identity orientation becomes the delta");
        check(near3(r.scale, Vec3{1, 1, 1}), "rotation leaves the scale alone");
    }
    {
        Transform t; t.position = Vec3{0, 3, 0}; t.rotation = Quat::fromAxisAngle(Vec3{1, 0, 0}, halfPi);
        const Quat dq = Quat::fromAxisAngle(up, halfPi);
        const Transform r = rotateAboutPivot(t, Vec3{0, 0, 0}, dq);
        check(near3(r.position, Vec3{-3, 0, 0}), "position rotates about the origin pivot");
        check(nearQ(r.rotation, (dq * t.rotation).normalized()), "orientation composes as delta * existing");
        // Two members keep their relative layout: distance between them is preserved.
        Transform a; a.position = Vec3{4, 0, 0}; Transform b; b.position = Vec3{0, 4, 0};
        const Vec3 pv{2, 2, 0};
        const Transform ra = rotateAboutPivot(a, pv, dq), rb = rotateAboutPivot(b, pv, dq);
        check(std::fabs(dist(ra.position, rb.position) - dist(a.position, b.position)) < 1e-3f, "pair distance preserved");
    }
    {
        Transform t; t.position = Vec3{3, 1, 0}; t.scale = Vec3{1, 2, 1};
        const Transform r = scaleAboutPivot(t, Vec3{1, 1, 0}, Quat::identity(), Vec3{2, 2, 2});
        check(near3(r.position, Vec3{5, 1, 0}), "uniform x2 about (1,1,0) doubles the offset");
        check(near3(r.scale, Vec3{2, 4, 2}), "and multiplies the object's scale");
    }
    {
        // Frame rotated 90 deg about Z: frame X is world Y, so a factor of 3 on frame X stretches world Y.
        Transform t; t.position = Vec3{0, 1, 0};
        const Transform r = scaleAboutPivot(t, Vec3{0, 0, 0}, Quat::fromAxisAngle(up, halfPi), Vec3{3, 1, 1});
        check(near3(r.position, Vec3{0, 3, 0}), "factors are measured along the gizmo frame");
    }
    {
        // Single selection: the pivot is the object's own position, so only orientation / scale change.
        Transform t; t.position = Vec3{7, -2, 5};
        const Transform rr = rotateAboutPivot(t, t.position, Quat::fromAxisAngle(up, 0.7f));
        check(near3(rr.position, t.position), "single selection: rotation does not move the position");
        const Transform rs = scaleAboutPivot(t, t.position, Quat::identity(), Vec3{1.5f, 1.5f, 1.5f});
        check(near3(rs.position, t.position) && near3(rs.scale, Vec3{1.5f, 1.5f, 1.5f}), "single selection: scale does not move the position");
    }
    {
        check(near3(scaleFactors(Vec3{1, 2, 4}, Vec3{2, 2, 2}, false), Vec3{2, 1, 0.5f}), "per-axis factors are after/before");
        const Vec3 u = scaleFactors(Vec3{1, 2, 3}, Vec3{2, 3, 4}, true);
        check(near3(u, Vec3{1.5f, 1.5f, 1.5f}), "uniform handle gives one factor from the summed scales");
        const Quat a = Quat::fromAxisAngle(Vec3{1, 0, 0}, 0.4f), d = Quat::fromAxisAngle(up, 0.9f);
        check(nearQ(deltaRotation(a, (d * a).normalized()), d), "deltaRotation recovers the applied delta");
    }

    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
