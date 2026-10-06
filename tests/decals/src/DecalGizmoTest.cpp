// DecalGizmoTest -- the decal box gizmo's arithmetic (sandbox/src/DecalGizmoMath.hpp): the box a
// CDecal and its matrix describe, picking it with a ray (a decal draws nothing a mesh pick could hit),
// the wireframe, the face handles and the symmetric drag that resizes it. No device.
#include "DecalGizmoMath.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
static bool approxV(const Vec3& a, const Vec3& b, f32 tol = 1e-3f) { return approx(a.x, b.x, tol) && approx(a.y, b.y, tol) && approx(a.z, b.z, tol); }

int main() {
    AVER_INFO("=== DecalGizmoTest ===");

    scene::CDecal c{};
    c.sizeCm[0] = 40.0f; c.sizeCm[1] = 60.0f; c.sizeCm[2] = 80.0f;
    Transform xf;
    xf.position = Vec3{100, 0, 0};
    const DecalBox box = decalBoxOf(c, xf.toMatrix());

    AVER_INFO("=== the box ===");
    {
        check(approxV(box.centre, {100, 0, 0}) && approxV(box.half, {20, 30, 40}), "centre and half extents from the size");
        check(approxV(box.axis[0], {1, 0, 0}) && approxV(box.axis[1], {0, 1, 0}) && approxV(box.axis[2], {0, 0, 1}), "axes of an unrotated decal");

        Transform scaled = xf;
        scaled.scale = Vec3{2, 1, 3};
        scaled.rotation = Quat::fromAxisAngle({0, 0, 1}, radians(90.0f));
        const DecalBox b2 = decalBoxOf(c, scaled.toMatrix());
        check(approxV(b2.half, {40, 30, 120}), "the entity's scale scales the box");
        check(approxV(b2.axis[0], {0, 1, 0}) && approxV(b2.axis[1], {-1, 0, 0}), "and its rotation turns the axes");
        check(approx(b2.scale[0], 2.0f) && approx(b2.scale[2], 3.0f), "the per-axis scale is kept for turning sizes back");

        scene::CDecal zero{};
        check(approxV(decalBoxOf(zero, Mat4::identity()).half, {50, 50, 50}), "an unset size is the 100 cm default");
    }

    AVER_INFO("=== picking ===");
    {
        f32 t = 0.0f;
        check(rayHitsDecalBox({0, 0, 0}, {1, 0, 0}, box, t) && approx(t, 80.0f), "a ray down +X enters at the near face");
        check(!rayHitsDecalBox({0, 100, 0}, {1, 0, 0}, box, t), "a ray passing beside it misses");
        check(!rayHitsDecalBox({0, 0, 0}, {0, 1, 0}, box, t), "a ray parallel to a slab and outside it misses");
        check(!rayHitsDecalBox({200, 0, 0}, {1, 0, 0}, box, t), "a box behind the ray is not hit");
        check(rayHitsDecalBox({100, 0, 0}, {0, 0, 1}, box, t) && approx(t, 0.0f), "from inside, the hit is at the origin");
        const Vec3 d = Vec3{-1, 1, 0}.getSafeNormal();
        check(rayHitsDecalBox({180, -80, 0}, d, box, t) && t > 0.0f, "a diagonal ray finds it");
        check(rayHitsDecalBox({100, 29, 500}, {0, 0, -1}, box, t) && approx(t, 460.0f), "a ray from above lands on the top face");
    }

    AVER_INFO("=== the wireframe ===");
    {
        std::vector<Vec3> segs;
        decalBoxSegments(box, segs);
        check(segs.size() == 38 && segs.size() % 2 == 0, "12 edges, a 5-segment arrow and a 2-segment cross, as line pairs");
        bool inside = true;
        for (usize i = 0; i < 24; ++i) {
            const Vec3 r = segs[i] - box.centre;
            inside = inside && std::fabs(r.x) <= 20.01f && std::fabs(r.y) <= 30.01f && std::fabs(r.z) <= 40.01f;
        }
        check(inside, "the box edges stay on the box");
        check(approxV(segs[24], box.centre) && approxV(segs[25], box.centre + Vec3{20, 0, 0}), "the arrow runs from the centre to the +X face");
    }

    AVER_INFO("=== handles ===");
    {
        const DecalHandle hx{0, 1};
        check(approxV(decalHandlePos(box, hx), {120, 0, 0}) && approxV(decalHandlePos(box, {1, -1}), {100, -30, 0}) &&
                  approxV(decalHandlePos(box, {2, 1}), {100, 0, 40}), "handle positions are the face centres");
        const DecalHandle nearest = pickDecalHandle(box, {0, 0, 0}, {1, 0, 0}, 3.0f);
        check(nearest.valid() && nearest.axis == 0 && nearest.sign == -1, "a ray through two handles takes the nearer");
        const DecalHandle top = pickDecalHandle(box, {100, 0, 500}, {0, 0, -1}, 3.0f);
        check(top.valid() && top.axis == 2 && top.sign == 1, "from above it takes the top handle");
        check(!pickDecalHandle(box, {0, 0, 0}, Vec3{1, 1, 1}.getSafeNormal(), 3.0f).valid(), "a ray through empty space takes none");
        check(!pickDecalHandle(box, {100, 0, 500}, {0, 0, 1}, 3.0f).valid(), "a handle behind the ray is not picked");
        check(pickDecalHandle(box, {100, 4, 500}, {0, 0, -1}, 5.0f).valid() && !pickDecalHandle(box, {100, 4, 500}, {0, 0, -1}, 3.0f).valid(),
              "the pick radius decides what counts as on it");
    }

    AVER_INFO("=== dragging ===");
    {
        // A mouse ray that crosses the box's Y axis line at y = 375.
        const Vec3 o{100, 0, 500};
        const Vec3 d = Vec3{0, 0.6f, -0.8f};
        f32 s = 0.0f;
        check(decalAxisParameter(box, 1, o, d, s) && approx(s, 375.0f, 0.05f), "the closest point on the axis line");
        check(approx(dragDecalHandleSize(box, {1, 1}, o, d), 750.0f, 0.1f), "the full size is twice that distance, so the box stays centred");
        check(approx(dragDecalHandleSize(box, {1, -1}, o, d), 750.0f, 0.1f), "either face of the axis gives the same size");

        Transform scaled = xf;
        scaled.scale = Vec3{1, 2, 1};
        const DecalBox b2 = decalBoxOf(c, scaled.toMatrix());
        check(approx(dragDecalHandleSize(b2, {1, 1}, o, d), 375.0f, 0.1f), "a scaled entity turns the world distance back into sizeCm");

        check(dragDecalHandleSize(box, {1, 1}, {100, -50, 0}, {0, 1, 0}) < 0.0f, "a ray along the axis gives no answer");
        check(dragDecalHandleSize(box, {}, o, d) < 0.0f, "no handle, no answer");
        check(approx(dragDecalHandleSize(box, {1, 1}, {100, 0, 500}, Vec3{0, 0.001f, -1.0f}.getSafeNormal()), 1.0f, 0.1f),
              "the size never drops below 1 cm");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
