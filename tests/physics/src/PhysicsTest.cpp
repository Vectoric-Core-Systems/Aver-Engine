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
#include <vector>

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

// Sensors, contact/overlap events, and the shape queries.
//
// These are the parts a game reaches for first and the parts an automated run can actually prove:
// none of them can be checked by looking at a screenshot, and all of them fail silently when wrong
// (a trigger that never fires and a trigger that does not exist look identical from gameplay).
static void testEventsAndQueries() {
    AVER_INFO("-- sensors, events, queries --");
    check(aver_phys_init() == 1, "world starts");

    const int32_t floor = aver_phys_add_static_box(0, 0, -10.0f, 5000.0f, 5000.0f, 10.0f);
    check(floor != 0, "floor created");

    // A sensor straddling the fall line, well above the floor.
    const int32_t gate = aver_phys_add_sensor_box(0, 0, 200.0f, 100.0f, 100.0f, 40.0f);
    check(gate != 0, "sensor created");

    // Dropped from above it, so it must pass THROUGH the sensor and land on the floor: entering and
    // leaving are both exercised, and a sensor that wrongly blocked movement would strand it.
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 400.0f, 20.0f, 5.0f);
    check(ball != 0, "falling body created");

    bool sawEnter = false, sawExit = false;
    int32_t contactsSeen = 0;
    for (int i = 0; i < 240; ++i) {
        aver_phys_step(1.0f / 60.0f);
        for (int32_t k = 0, n = aver_phys_overlap_count(); k < n; ++k) {
            int32_t s = 0, b = 0, entered = 0;
            if (!aver_phys_overlap_get(k, &s, &b, &entered)) continue;
            if (s == gate && b == ball) { if (entered) sawEnter = true; else sawExit = true; }
        }
        contactsSeen += aver_phys_contact_count();
    }
    check(sawEnter, "sensor reported the body ENTERING");
    check(sawExit,  "sensor reported the body LEAVING");
    check(contactsSeen > 0, "solid contact reported when it hit the floor");

    // Passing through means it is on the floor, not perched on the trigger.
    float p[3] = {0,0,0};
    aver_phys_body_position(ball, p);
    check(p[2] > 0.0f && p[2] < 60.0f,
          "body passed THROUGH the sensor and landed (z=" + std::to_string(p[2]) + ")");

    // Overlap query: a generous sphere at the resting point must find the ball and the floor.
    int32_t found[16] = {0};
    const int32_t n = aver_phys_overlap_sphere(p[0], p[1], p[2], 80.0f, found, 16);
    bool foundBall = false;
    for (int32_t i = 0; i < n; ++i) if (found[i] == ball) foundBall = true;
    check(n > 0, "overlap sphere found " + std::to_string(n) + " body(ies)");
    check(foundBall, "overlap sphere found the resting body");

    // Shape cast: sweeping DOWN from high above must hit something, and stop above the floor
    // surface by roughly the sweep radius -- which is the whole difference from a ray.
    float hp[3] = {0,0,0}, hn[3] = {0,0,0};
    const int32_t sweptHit = aver_phys_sphere_cast(0, 0, 500.0f, 0, 0, -1.0f, 1000.0f, 30.0f, hp, hn);
    check(sweptHit != 0, "sphere cast hit something on the way down");
    check(hn[2] > 0.5f, "sphere cast normal points UP (z=" + std::to_string(hn[2]) + ")");

    aver_phys_shutdown();
}

// Convex hulls, triangle meshes and heightfields.
//
// The mesh case is the one worth having: the engine-to-Jolt axis map has determinant -1, so it
// MIRRORS geometry, and a mesh whose winding is not corrected collides on its back face. That does
// not crash or look obviously wrong -- things simply fall through a floor that is visibly there.
static void testShapes() {
    AVER_INFO("-- convex hull, mesh, heightfield --");
    check(aver_phys_init() == 1, "world starts");

    // A triangle-mesh floor: two triangles spanning 40m, wound counter-clockwise when seen from
    // above in ENGINE space, i.e. normals up.
    const float mv[12] = {
        -2000.0f, -2000.0f, 0.0f,
         2000.0f, -2000.0f, 0.0f,
         2000.0f,  2000.0f, 0.0f,
        -2000.0f,  2000.0f, 0.0f,
    };
    const int32_t mi[6] = {0, 1, 2, 0, 2, 3};
    const int32_t meshFloor = aver_phys_add_mesh(mv, 4, mi, 6, 0, 0, 0);
    check(meshFloor != 0, "triangle-mesh floor created");

    // A convex hull box, dropped onto it.
    const float hp[24] = {
        -20,-20,-20,  20,-20,-20,  20, 20,-20, -20, 20,-20,
        -20,-20, 20,  20,-20, 20,  20, 20, 20, -20, 20, 20,
    };
    const int32_t hull = aver_phys_add_convex_hull(hp, 8, 0, 0, 400.0f, /*dynamic*/1, 8.0f);
    check(hull != 0, "convex hull created");

    for (int i = 0; i < 300; ++i) aver_phys_step(1.0f / 60.0f);

    float p[3] = {0,0,0};
    aver_phys_body_position(hull, p);
    // Landing ON the mesh is the assertion. Falling through would put it far below zero, and is
    // exactly what a wrong winding produces.
    check(p[2] > 0.0f && p[2] < 80.0f,
          "hull landed ON the triangle mesh, not through it (z=" + std::to_string(p[2]) + ")");

    // A flat heightfield at z=0, and a body dropped on it.
    float hf[64];
    for (int i = 0; i < 64; ++i) hf[i] = 0.0f;
    const int32_t field = aver_phys_add_heightfield(hf, 8, 500.0f, -2000.0f, -2000.0f, 0.0f);
    check(field != 0, "heightfield created");

    aver_phys_shutdown();
}

// Does a heightfield keep EVERY sample it was given?
//
// This exists because the 8x8 field above is exactly a multiple of Jolt's block size, so it could
// never have caught what it was hiding: aver_phys_add_heightfield used to crop the grid DOWN to a
// multiple of 8, on a comment claiming Jolt required that. Jolt rounds UP and pads the remainder
// itself (HeightFieldShape.cpp:441 and :531-558), so the crop was pure data loss -- a 9x9 field
// became 8x8 and lost its last row and column, which for tiled terrain is exactly the shared edge
// where two chunks are supposed to meet.
//
// DIFFERENTIAL, on purpose, so it asserts nothing about the axis mapping. Two 9x9 fields identical
// except in the final row cannot be told apart by any query if that row is discarded. Comparing them
// tests precisely "the last row survives" and stays true no matter which engine axis a grid row runs
// along -- which matters, because that mapping is genuinely confusing here (the field is built in
// Jolt's own Y-up axes and only its centre goes through the axis map).
void testHeightfieldKeepsEverySample() {
    AVER_INFO("-- heightfield sample retention --");

    const int32_t N = 9;                  // deliberately NOT a multiple of 8
    const float   spacing = 200.0f;       // cm
    const float   raised = 400.0f;        // cm, well clear of the flat rows

    // Sweep the whole footprint plus a margin, straight down, and collect every hit height.
    // Swept over BOTH signs of both axes, which is what keeps this test axis-agnostic in practice as
    // well as in intention. The first version marched ix,iy from -1..N assuming a grid row ran along
    // engine +X; it does not. The field is built in Jolt's own axes and only its centre goes through
    // the axis map, so composing that map (Convert.hpp: Aver +X -> Jolt -Z) puts a row along engine
    // -X: a 9x9 field pinned at the origin occupies x in [-1600, 0], and the old sweep looked at
    // x in [-200, 1800] and missed almost all of it. Every ray reported a miss, the two fields
    // "agreed", and the test failed for a reason that had nothing to do with what it was testing.
    //
    // Covering +-(N+1) in both axes costs 361 rays and removes the need to be right about any of that.
    auto sweep = [&]() {
        std::vector<float> zs;
        for (int32_t iy = -(N + 1); iy <= N + 1; ++iy) {
            for (int32_t ix = -(N + 1); ix <= N + 1; ++ix) {
                const float x = static_cast<float>(ix) * spacing;
                const float y = static_cast<float>(iy) * spacing;
                float p[3] = {0,0,0}, nrm[3] = {0,0,0};
                const int32_t hit = aver_phys_raycast(x, y, 5000.0f, 0.0f, 0.0f, -1.0f,
                                                      20000.0f, p, nrm);
                zs.push_back(hit ? p[2] : -99999.0f);
            }
        }
        return zs;
    };

    std::vector<float> flatHits, raisedHits;

    std::vector<float> flat(static_cast<size_t>(N) * N, 0.0f);
    check(aver_phys_init() == 1, "physics up for the flat field");
    check(aver_phys_add_heightfield(flat.data(), N, spacing, 0.0f, 0.0f, 0.0f) != 0,
          "9x9 heightfield created (not a multiple of 8)");
    flatHits = sweep();
    aver_phys_shutdown();

    // The same field with only its LAST ROW raised. Under the old crop this is byte-identical to the
    // flat one after cropping, so every ray would agree and the difference below would be zero.
    std::vector<float> bumped = flat;
    for (int32_t x = 0; x < N; ++x)
        bumped[static_cast<size_t>(N - 1) * N + x] = raised;

    check(aver_phys_init() == 1, "physics up for the bumped field");
    check(aver_phys_add_heightfield(bumped.data(), N, spacing, 0.0f, 0.0f, 0.0f) != 0,
          "9x9 heightfield with a raised last row created");
    raisedHits = sweep();
    aver_phys_shutdown();

    check(flatHits.size() == raisedHits.size(), "both sweeps cast the same number of rays");

    int differing = 0, raisedSeen = 0;
    for (size_t i = 0; i < flatHits.size() && i < raisedHits.size(); ++i) {
        if (std::fabs(flatHits[i] - raisedHits[i]) > 1.0f) ++differing;
        if (raisedHits[i] > raised * 0.5f) ++raisedSeen;
    }
    // THE ASSERTION THAT WOULD HAVE FAILED BEFORE THE FIX. A discarded last row is unobservable, so
    // the two fields would have been indistinguishable and `differing` would be 0.
    check(differing > 0,
          "the last row of a 9x9 heightfield is not discarded (" + std::to_string(differing) +
          " ray(s) differ from the flat field)");
    check(raisedSeen > 0,
          "a ray actually landed on the raised last row (" + std::to_string(raisedSeen) + " hit(s))");
}

int main() {
    testAxisMap();
    testRotationMap();
    testSimulation();
    testEventsAndQueries();
    testShapes();
    testHeightfieldKeepsEverySample();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
