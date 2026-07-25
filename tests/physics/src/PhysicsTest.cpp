// Hand-run test for Aver.Physics: the Aver<->Jolt conversion, then the simulation itself.
// Exit code = failure count. Same shape as tests/scene and tests/formats.
//
// The conversion half matters more than it looks. Every one of the four conventions differs
// (centimetres vs metres, +Z up vs +Y up, left- vs right-handed, row- vs column-major), and a
// handedness mistake does not crash or look obviously wrong -- it MIRRORS the world, so a character
// strafes the wrong way and everything else still seems fine. So rather than checking a few
// hand-computed numbers, this checks the property that DEFINES a correct change of basis:
//
//     convert(rotate(q, v))  ==  rotate(convert(q), convert(v))
//
// which cannot hold by accident for a wrong axis map or a wrong rotation sense.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"

#include "../../../modules/physics/src/Convert.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::physics;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool vnear(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
static std::string str(const Vec3& v) {
    char b[96]; std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f)", v.x, v.y, v.z); return b;
}

// Rotate a vector by a quaternion, in AVER's own maths -- deliberately not reusing anything from the
// conversion, so the test does not check the code against itself.
static Vec3 averRotate(const Quat& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const f32 s = q.w;
    const Vec3 uxv(u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x);
    const f32 dot = u.x*v.x + u.y*v.y + u.z*v.z;
    const f32 uu  = u.x*u.x + u.y*u.y + u.z*u.z;
    return Vec3(2.0f*dot*u.x + (s*s - uu)*v.x + 2.0f*s*uxv.x,
                2.0f*dot*u.y + (s*s - uu)*v.y + 2.0f*s*uxv.y,
                2.0f*dot*u.z + (s*s - uu)*v.z + 2.0f*s*uxv.z);
}

static void testAxisMap() {
    AVER_INFO("-- axis map --");
    // The engine's three basis directions must land on Jolt's.
    check(toJoltUnit(Vec3(0,0,1)) == JPH::Vec3(0,1,0),  "Aver up (+Z) -> Jolt up (+Y)");
    check(toJoltUnit(Vec3(0,1,0)) == JPH::Vec3(1,0,0),  "Aver right (+Y) -> Jolt right (+X)");
    check(toJoltUnit(Vec3(1,0,0)) == JPH::Vec3(0,0,-1), "Aver forward (+X) -> Jolt forward (-Z)");

    // Units: 100cm is 1m, in both directions.
    check(near(toJolt(Vec3(0,0,100)).GetY(), 1.0f), "100cm up converts to 1m");
    check(vnear(fromJolt(JPH::Vec3(0,1,0)), Vec3(0,0,100)), "1m up converts back to 100cm");

    // Round trip on an asymmetric vector, so a transposed or partially-correct map cannot pass.
    const Vec3 v(12.0f, -34.0f, 56.0f);
    check(vnear(fromJolt(toJolt(v)), v), "vector round trip: " + str(fromJolt(toJolt(v))));

    // Handedness: the map must REVERSE it. Under a left-handed basis Cross(fwd, right) is up; a
    // proper (det +1) map would preserve that, and this one must not.
    const JPH::Vec3 jf = toJoltUnit(Vec3(1,0,0)), jr = toJoltUnit(Vec3(0,1,0));
    check(jf.Cross(jr) == JPH::Vec3(0,-1,0),
          "handedness flips: Jolt fwd x right is DOWN, not up (det = -1)");
}

static void testRotationMap() {
    AVER_INFO("-- rotation map --");
    // The defining property, over a spread of axes and angles rather than one lucky case.
    const Vec3 axes[] = {Vec3(0,0,1), Vec3(1,0,0), Vec3(0,1,0), Vec3(0.577f,0.577f,0.577f)};
    const f32  angles[] = {0.3f, 1.0f, 2.4f, -0.8f};
    const Vec3 probes[] = {Vec3(1,0,0), Vec3(0,1,0), Vec3(0,0,1), Vec3(3,-4,5)};

    int mismatches = 0;
    for (const Vec3& a : axes) {
        for (f32 ang : angles) {
            const Quat q = Quat::fromAxisAngle(a, ang);
            for (const Vec3& p : probes) {
                const Vec3 want = averRotate(q, p);                     // rotate in Aver, then convert
                const JPH::Vec3 got = toJolt(q) * toJoltUnit(p);        // convert, then rotate in Jolt
                if (!vnear(fromJoltUnit(got), want, 2e-3f)) ++mismatches;
            }
        }
    }
    check(mismatches == 0,
          "rotate-then-convert == convert-then-rotate over 64 cases (" +
          std::to_string(mismatches) + " mismatched)");

    // A yaw in Aver must stay a yaw about the same physical axis in Jolt.
    const Quat yaw = Quat::fromAxisAngle(Vec3(0,0,1), 1.2f);
    const Quat back = fromJolt(toJolt(yaw));
    check(near(back.x, yaw.x) && near(back.y, yaw.y) && near(back.z, yaw.z),
          "quaternion round trip preserves the axis");
}

static void testSimulation() {
    AVER_INFO("-- simulation --");
    check(aver_phys_init() == 1, "world starts");
    check(aver_phys_ready() == 1, "world reports ready");

    // A floor at z = 0 (10m x 10m x 20cm slab, centred just below the surface).
    const int32_t floor = aver_phys_add_static_box(0, 0, -10.0f, 500.0f, 500.0f, 10.0f);
    check(floor != 0, "static floor created");
    check(aver_phys_body_count() == 1, "one body in the world");

    // A sphere dropped from 3m. It must FALL, and then it must STOP -- a physics integration that
    // only proves things move is happy with a body sinking through the floor forever.
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 300.0f, 25.0f, 10.0f);
    check(ball != 0, "dynamic sphere created");

    float p[3] = {0,0,0};
    aver_phys_body_position(ball, p);
    const f32 startZ = p[2];

    int steps = 0;
    for (int i = 0; i < 240; ++i) steps += aver_phys_step(1.0f / 60.0f);
    check(steps > 200, "stepping ran " + std::to_string(steps) + " fixed steps");

    aver_phys_body_position(ball, p);
    check(p[2] < startZ - 100.0f, "sphere fell under gravity (z " + std::to_string(startZ) +
                                  " -> " + std::to_string(p[2]) + ")");
    // Radius 25 resting on a surface at z=0 puts the centre near z=25. Generous bound: the point is
    // that it settled ON the floor, not through it.
    check(p[2] > 0.0f && p[2] < 60.0f,
          "sphere came to rest ON the floor, not through it (z=" + std::to_string(p[2]) + ")");

    // Gravity must act along the ENGINE's down, not Jolt's -- this is the check that catches an
    // up-axis mix-up, since a Y-up mistake sends the ball sideways instead of down.
    check(near(p[0], 0.0f, 5.0f) && near(p[1], 0.0f, 5.0f),
          "it fell straight down, not sideways (x=" + std::to_string(p[0]) +
          " y=" + std::to_string(p[1]) + ")");

    // A ray straight down from above the floor must hit it.
    float hit[3] = {0,0,0}, nrm[3] = {0,0,0};
    const int32_t rayHit = aver_phys_raycast(200.0f, 200.0f, 200.0f, 0, 0, -1, 400.0f, hit, nrm);
    check(rayHit == floor, "downward ray hit the floor body");
    check(near(hit[2], 0.0f, 2.0f), "hit point is at the floor surface (z=" + std::to_string(hit[2]) + ")");
    check(nrm[2] > 0.9f, "surface normal points UP in engine axes (z=" + std::to_string(nrm[2]) + ")");

    // The character: it must land on the floor rather than sinking, and report grounded.
    const int32_t ch = aver_phys_character_create(30.0f, 180.0f, 0, 200.0f, 250.0f);
    check(ch != 0, "character created");
    for (int i = 0; i < 180; ++i) aver_phys_step(1.0f / 60.0f);
    float cp[3] = {0,0,0};
    aver_phys_character_position(ch, cp);
    check(cp[2] > -10.0f && cp[2] < 120.0f,
          "character settled on the floor (z=" + std::to_string(cp[2]) + ")");
    check(aver_phys_character_grounded(ch) == 1, "character reports grounded");

    aver_phys_shutdown();
    check(aver_phys_ready() == 0, "world stops");
}

int main() {
    testAxisMap();
    testRotationMap();
    testSimulation();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
