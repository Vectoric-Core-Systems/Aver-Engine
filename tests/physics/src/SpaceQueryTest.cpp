// SpaceQueryTest -- space-state queries and body test motion on the plain-C ABI: the ray with explicit options, intersect_shape,
// collide_shape, get_rest_info, cast_motion and aver_phys_body_test_motion.
//
// EVERY SCENE IS BUILT FROM BODIES THAT NEVER STEP, so each expected number is arithmetic on the positions below. Scenes are off-axis (walls on +X,
// +Y and below) and box extents unequal, because a swapped axis in the engine-to-Jolt basis change still answers "something is ahead" and only
// shows when the thing is somewhere the wrong axis does not look. Distances carry ~1.5 cm of tolerance: a cast is a root find, not exact arithmetic.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_queries_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"

#include <cmath>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static bool near(float a, float b, float tol = 1.5f) { return std::fabs(a - b) <= tol; }

static const float kS = 0.70710678f;
static const float kYaw90[7] = {0, 0, 0, 0, 0, kS, kS};   // pose7 tail: a quarter turn about +Z

static const int32_t kBodies = AVER_PHYS_SQ_BODIES;
static const uint32_t kAll = 0xFFFFFFFFu;

static int32_t wall(float x, float y, float z, float hx, float hy, float hz) { return aver_phys_add_static_box(x, y, z, hx, hy, hz); }

struct Ray { int32_t body; float dist, pt[3], n[3]; int32_t entity, sub; };

static Ray ray(float ox, float oy, float oz, float dx, float dy, float dz, uint32_t mask, int32_t flags,
               const int32_t* ex = nullptr, int32_t exN = 0) {
    Ray r{};
    r.dist = -1;
    const float o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz};
    r.body = aver_phys_space_raycast(o, d, 5000.0f, mask, flags, ex, exN, &r.dist, r.pt, r.n, &r.entity, &r.sub);
    return r;
}

static void pose(float* p, float x, float y, float z, const float* q = nullptr) {
    p[0] = x; p[1] = y; p[2] = z;
    p[3] = q ? q[3] : 0; p[4] = q ? q[4] : 0; p[5] = q ? q[5] : 0; p[6] = q ? q[6] : 1;
}

// ---------------------------------------------------------------------------------------------------

static void testRay() {
    AVER_INFO("=== ray: bodies / areas / mask / exclude ===");
    aver_phys_init();
    const int32_t nearW = wall(500, 0, 0, 10, 200, 200);    // face at x = 490
    const int32_t farW  = wall(1000, 0, 0, 10, 200, 200);   // face at x = 990
    const int32_t right = wall(0, 800, 0, 200, 10, 200);    // face at y = 790
    const int32_t sensor = aver_phys_add_sensor_box(300, 0, 0, 10, 50, 50);   // face at x = 290

    Ray r = ray(0, 0, 0, 1, 0, 0, kAll, kBodies);
    check(r.body == nearW && near(r.dist, 490.0f), "a ray along +X hits the near wall at 490, got " + f2s(r.dist));
    check(near(r.pt[0], 490.0f) && near(r.n[0], -1.0f, 0.05f), "with point x = 490 and normal (-1,0,0)");
    r = ray(0, 0, 0, 0, 5, 0, kAll, kBodies);
    check(r.body == right && near(r.dist, 790.0f) && near(r.n[1], -1.0f, 0.05f), "along +Y (any direction length) it hits the right wall at 790");
    r = ray(0, 0, 0, 1, 0, 0, kAll, 0);
    check(r.body == 0, "flags 0 means no bodies and no areas: nothing is hit");
    r = ray(0, 0, 0, 1, 0, 0, kAll, AVER_PHYS_SQ_AREAS);
    check(r.body == sensor && near(r.dist, 290.0f), "areas only: the sensor is hit at 290 and the walls are not");
    r = ray(0, 0, 0, 1, 0, 0, kAll, kBodies | AVER_PHYS_SQ_AREAS);
    check(r.body == sensor, "bodies and areas: the sensor is the nearest");

    const int32_t ex1[1] = {nearW};
    r = ray(0, 0, 0, 1, 0, 0, kAll, kBodies, ex1, 1);
    check(r.body == farW && near(r.dist, 990.0f), "excluding the near wall hits the far one at 990");
    const int32_t ex2[3] = {nearW, farW, 9999};
    r = ray(0, 0, 0, 1, 0, 0, kAll, kBodies, ex2, 3);
    check(r.body == 0, "excluding both (and a dead handle, ignored) hits nothing");
    r = ray(0, 0, 0, 1, 0, 0, kAll, kBodies, ex1, 0);
    check(r.body == nearW, "a count of 0 excludes nothing");

    aver_phys_body_set_collision_filter(nearW, 1u << 3, 0);
    aver_phys_body_set_collision_filter(farW, 1u << 7, 0);
    r = ray(0, 0, 0, 1, 0, 0, 1u << 7, kBodies);
    check(r.body == farW, "the 32-bit mask selects the far wall's layer");
    r = ray(0, 0, 0, 1, 0, 0, 1u << 3, kBodies);
    check(r.body == nearW, "and the near wall's");
    r = ray(0, 0, 0, 1, 0, 0, 1u << 9, kBodies);
    check(r.body == 0, "a mask naming neither layer hits nothing");

    Ray n = ray(0, 0, 0, 1, 0, 0, kAll, kBodies);
    check(n.entity == 0, "the entity stamp reads back (0 for an unstamped body)");
    aver_phys_shutdown();
}

static void testRayBackFacesAndInside() {
    AVER_INFO("=== ray: hit back faces / hit from inside ===");
    aver_phys_init();
    const int32_t box = wall(0, 3000, 0, 100, 100, 100);   // y in [2900, 3100]
    const int32_t B = AVER_PHYS_SQ_BODIES;

    Ray r = ray(0, 3000, 0, 0, 1, 0, kAll, B);
    check(r.body == 0, "from inside a convex body, leaving it: no back faces and no inside means no hit");
    r = ray(0, 3000, 0, 0, 1, 0, kAll, B | AVER_PHYS_SQ_BACK_FACES);
    check(r.body == box && near(r.dist, 100.0f), "with back faces the exit face is hit at 100, got " + f2s(r.dist));
    check(near(r.n[1], 1.0f, 0.05f), "its normal is the outward normal (0,1,0)");
    r = ray(0, 3000, 0, 0, 1, 0, kAll, B | AVER_PHYS_SQ_FROM_INSIDE);
    check(r.body == box && r.dist < 0.01f, "hit from inside reports the body at distance 0");
    check(std::fabs(r.n[0]) + std::fabs(r.n[1]) + std::fabs(r.n[2]) < 1e-4f, "with a zero normal");
    r = ray(0, 2000, 0, 0, 1, 0, kAll, B | AVER_PHYS_SQ_BACK_FACES | AVER_PHYS_SQ_FROM_INSIDE);
    check(r.body == box && near(r.dist, 900.0f) && near(r.n[1], -1.0f, 0.05f), "from outside the front face is hit first in every mode");

    // A triangle mesh: a quad in the plane x = 2000. Which side is the front depends on the winding, so the test asks for a one-sided
    // result from one side and the other from the other, and both from both sides with back faces.
    const float verts[12] = {2000, -200, -200,  2000, 200, -200,  2000, 200, 200,  2000, -200, 200};
    const int32_t idx[6] = {0, 1, 2, 0, 2, 3};
    const int32_t shape = aver_phys_shape_mesh(verts, 4, idx, 6, nullptr, nullptr, 0, nullptr, nullptr, 0);
    const float origin[3] = {0, 0, 0};
    const int32_t mesh = aver_phys_body_from_shape(shape, origin, nullptr, AVER_PHYS_MOTION_STATIC, 0, nullptr, 0);
    check(shape != 0 && mesh != 0, "the mesh body builds");
    Ray fromMinus = ray(1000, 0, 0, 1, 0, 0, kAll, B);
    Ray fromPlus = ray(3000, 0, 0, -1, 0, 0, kAll, B);
    check((fromMinus.body != 0) != (fromPlus.body != 0), "without back faces exactly one side of a triangle is hit");
    fromMinus = ray(1000, 0, 0, 1, 0, 0, kAll, B | AVER_PHYS_SQ_BACK_FACES);
    fromPlus = ray(3000, 0, 0, -1, 0, 0, kAll, B | AVER_PHYS_SQ_BACK_FACES);
    check(fromMinus.body == mesh && near(fromMinus.dist, 1000.0f) && fromPlus.body == mesh && near(fromPlus.dist, 1000.0f),
          "with back faces both sides are hit, 1000 cm away");
    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

static void testIntersectCollideRest() {
    AVER_INFO("=== intersect_shape / collide_shape / get_rest_info ===");
    aver_phys_init();
    const int32_t A = wall(500, 0, 0, 10, 200, 200);          // x in [490, 510]
    const int32_t B2 = wall(1000, 0, 0, 10, 200, 200);
    const int32_t S = aver_phys_add_sensor_box(300, 0, 0, 10, 50, 50);
    const int32_t sphere = aver_phys_shape_sphere(30, 0, nullptr, 0);
    float p[7];
    int32_t bodies[8] = {0}, subs[8] = {0};

    pose(p, 470, 0, 0);
    int n = aver_phys_space_intersect_shape(sphere, p, 0, kAll, kBodies, nullptr, 0, 8, bodies, subs);
    check(n == 1 && bodies[0] == A, "a sphere 10 cm into the near wall overlaps only that wall");
    n = aver_phys_space_intersect_shape(sphere, p, 0, kAll, kBodies, nullptr, 0, 0, bodies, subs);
    check(n == 0, "maxResults 0 returns nothing");
    pose(p, 440, 0, 0);
    n = aver_phys_space_intersect_shape(sphere, p, 0, kAll, kBodies, nullptr, 0, 8, bodies, subs);
    check(n == 0, "20 cm short of the wall there is no overlap");
    n = aver_phys_space_intersect_shape(sphere, p, 25.0f, kAll, kBodies, nullptr, 0, 8, bodies, subs);
    check(n == 1 && bodies[0] == A, "a 25 cm margin reaches it");
    const int32_t ex[1] = {A};
    n = aver_phys_space_intersect_shape(sphere, p, 25.0f, kAll, kBodies, ex, 1, 8, bodies, subs);
    check(n == 0, "unless it is excluded");
    pose(p, 300, 0, 0);
    n = aver_phys_space_intersect_shape(sphere, p, 0, kAll, kBodies, nullptr, 0, 8, bodies, subs);
    check(n == 0, "a sensor is not a body");
    n = aver_phys_space_intersect_shape(sphere, p, 0, kAll, AVER_PHYS_SQ_AREAS, nullptr, 0, 8, bodies, subs);
    check(n == 1 && bodies[0] == S, "but it is an area");

    // Rotation and a long thin box: 100 x 10 x 10 cm at x = 482. Unrotated it spans x 432..532 and hits; yawed it spans 477..487 and does not.
    const int32_t bar = aver_phys_shape_box(50, 5, 5, 0, nullptr, 0);
    pose(p, 482, 0, 0);
    n = aver_phys_space_intersect_shape(bar, p, 0, kAll, kBodies, nullptr, 0, 8, bodies, subs);
    check(n == 1 && bodies[0] == A, "the unrotated bar reaches the wall");
    pose(p, 482, 0, 0, kYaw90);
    n = aver_phys_space_intersect_shape(bar, p, 0, kAll, kBodies, nullptr, 0, 8, bodies, subs);
    check(n == 0, "the bar yawed 90 degrees has 5 cm along X and falls short");

    // collide_shape: a box 5 cm into the wall. An offset centre of mass must not move the geometry.
    const int32_t box = aver_phys_shape_box(20, 30, 40, 0, nullptr, 0);
    const float comOff[3] = {0, 0, 10};
    const int32_t boxCom = aver_phys_shape_offset_com(box, comOff);
    pose(p, 475, 0, 0);
    float pts[6], pto[6], nrm[6], dep[2];
    int32_t cb[2] = {0};
    for (const int32_t sh : {box, boxCom}) {
        n = aver_phys_space_collide_shape(sh, p, 0, kAll, kBodies, nullptr, 0, 2, cb, pts, pto, nrm, dep);
        check(n == 1 && cb[0] == A, std::string("collide_shape finds the wall") + (sh == boxCom ? " with an offset centre of mass" : ""));
        check(near(dep[0], 5.0f, 1.0f), "depth 5 cm, got " + f2s(dep[0]));
        check(near(nrm[0], -1.0f, 0.05f) && std::fabs(nrm[1]) < 0.05f, "normal points from the wall to the box, (-1,0,0)");
        check(near(pto[0], 490.0f, 1.0f) && near(pts[0], 495.0f, 1.0f), "points: on the wall x = 490, on the box x = 495");
    }

    // rest_info: the deepest of two, with the dynamic one's velocity.
    const int32_t D = aver_phys_add_dynamic_box(0, 1500, 0, 50, 50, 50, 10);
    aver_phys_body_set_velocity(D, 100, 0, 0);
    const int32_t sph = aver_phys_shape_sphere(20, 0, nullptr, 0);
    pose(p, 0, 1445, 0);   // 15 cm into D
    float rpt[3], rn[3], rv[3], rd = 0;
    int32_t ent = -1, sub = -1;
    int32_t hit = aver_phys_space_rest_info(sph, p, 0, kAll, kBodies, nullptr, 0, rpt, rn, rv, &rd, &ent, &sub);
    check(hit == D, "rest_info names the body it rests in");
    check(near(rd, 15.0f, 1.0f) && near(rn[1], -1.0f, 0.05f) && near(rpt[1], 1450.0f, 1.0f), "depth 15, normal (0,-1,0), point y = 1450");
    check(near(rv[0], 100.0f, 1.0f) && std::fabs(rv[1]) < 1.0f, "and the collider's velocity (100,0,0), got " + f2s(rv[0]));
    pose(p, 480, 0, 0);
    hit = aver_phys_space_rest_info(sph, p, 0, kAll, kBodies, nullptr, 0, rpt, rn, rv, &rd, &ent, &sub);
    check(hit == A && std::fabs(rv[0]) + std::fabs(rv[1]) + std::fabs(rv[2]) < 1e-3f, "a static collider has zero velocity");
    pose(p, 0, 0, 3000);
    check(aver_phys_space_rest_info(sph, p, 0, kAll, kBodies, nullptr, 0, rpt, rn, rv, &rd, &ent, &sub) == 0, "nothing in reach returns 0");
    (void)B2;
    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

static void testCastMotion() {
    AVER_INFO("=== cast_motion ===");
    aver_phys_init();
    const int32_t A = wall(500, 0, 0, 10, 200, 200);          // face at x = 490
    const int32_t B2 = wall(1000, 0, 0, 10, 200, 200);        // face at x = 990
    const int32_t right = wall(0, 800, 0, 200, 10, 200);      // face at y = 790
    const int32_t box = aver_phys_shape_box(20, 20, 20, 0, nullptr, 0);
    float p[7], safe = -1, unsafe = -1, pt[3], nrm[3];
    int32_t body = -1;

    pose(p, 0, 0, 0);
    float m[3] = {2000, 0, 0};
    int32_t ok = aver_phys_space_cast_motion(box, p, m, 0, kAll, kBodies, nullptr, 0, &safe, &unsafe, &body, pt, nrm);
    check(ok == 1 && body == A, "the box meets the near wall");
    check(near(unsafe * 2000, 470.0f) && near(safe * 2000, 470.0f), "at 470 cm (fraction 0.235), safe equals unsafe with no margin: " + f2s(safe) + " " + f2s(unsafe));
    check(near(pt[0], 490.0f) && near(nrm[0], -1.0f, 0.05f), "contact point x = 490, normal (-1,0,0)");
    aver_phys_space_cast_motion(box, p, m, 10.0f, kAll, kBodies, nullptr, 0, &safe, &unsafe, &body, pt, nrm);
    check(near(unsafe * 2000, 470.0f) && near(safe * 2000, 460.0f), "a 10 cm margin keeps the safe point 10 cm back: " + f2s(safe * 2000));
    m[0] = -2000;
    aver_phys_space_cast_motion(box, p, m, 0, kAll, kBodies, nullptr, 0, &safe, &unsafe, &body, pt, nrm);
    check(safe == 1.0f && unsafe == 1.0f && body == 0, "moving away from everything is [1, 1]");
    m[0] = 0; m[1] = 2000;
    aver_phys_space_cast_motion(box, p, m, 0, kAll, kBodies, nullptr, 0, &safe, &unsafe, &body, pt, nrm);
    check(body == right && near(unsafe * 2000, 770.0f), "off-axis: along +Y the right wall is met at 770");

    // Already overlapping A (30 cm in): A is ignored at fraction 0, and the far wall is what stops it.
    pose(p, 480, 0, 0);
    m[0] = 2000; m[1] = 0;
    aver_phys_space_cast_motion(box, p, m, 0, kAll, kBodies, nullptr, 0, &safe, &unsafe, &body, pt, nrm);
    check(body == B2 && near(unsafe * 2000, 990.0f - 20.0f - 480.0f), "a collider already overlapped is ignored: the far wall stops it at 490 cm");
    const int32_t ex[1] = {A};
    pose(p, 0, 0, 0);
    aver_phys_space_cast_motion(box, p, m, 0, kAll, kBodies, ex, 1, &safe, &unsafe, &body, pt, nrm);
    check(body == B2 && near(unsafe * 2000, 970.0f), "an excluded wall is passed through");
    check(aver_phys_space_cast_motion(0, p, m, 0, kAll, kBodies, nullptr, 0, &safe, &unsafe, &body, pt, nrm) == 0, "a dead shape handle is refused");
    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

struct Motion { int32_t r; float f[AVER_PHYS_TM_F_COUNT]; int32_t i[AVER_PHYS_TM_I_COUNT]; float cf[8 * AVER_PHYS_TC_F_COUNT]; int32_t ci[8 * AVER_PHYS_TC_I_COUNT]; };

static Motion testMotion(int32_t body, const float* from, float mx, float my, float mz, float margin, int32_t flags,
                         const int32_t* ex = nullptr, int32_t exN = 0) {
    Motion m{};
    const float mo[3] = {mx, my, mz};
    m.r = aver_phys_body_test_motion(body, from, mo, margin, flags, ex, exN, 8, m.f, m.i, m.cf, m.ci);
    return m;
}

static void testBodyTestMotion() {
    AVER_INFO("=== body_test_motion ===");
    aver_phys_init();
    const int32_t A = wall(500, 0, 0, 10, 200, 200);          // face at x = 490
    const int32_t P = aver_phys_add_dynamic_box(0, 0, 0, 20, 30, 20, 10);
    const float kMargin = 0.1f;

    Motion m = testMotion(P, nullptr, 2000, 0, 0, kMargin, kBodies);
    check(m.r == 1 && m.i[AVER_PHYS_TM_I_COLLIDER] == A, "a body moving into a wall collides with it");
    check(near(m.f[AVER_PHYS_TM_F_TRAVEL], 469.9f, 0.6f), "it travels to the margin: 470 - 0.1, got " + f2s(m.f[AVER_PHYS_TM_F_TRAVEL]));
    check(near(m.f[AVER_PHYS_TM_F_REMAINDER], 2000.0f - 469.9f, 0.6f), "the remainder is the rest of the motion");
    check(near(m.f[AVER_PHYS_TM_F_NORMAL], -1.0f, 0.05f) && near(m.f[AVER_PHYS_TM_F_POINT], 490.0f, 0.6f), "normal (-1,0,0), point x = 490");
    check(m.f[AVER_PHYS_TM_F_SAFE] < m.f[AVER_PHYS_TM_F_UNSAFE] + 1e-6f && near(m.f[AVER_PHYS_TM_F_UNSAFE] * 2000, 470.0f), "unsafe 0.235, safe just below");
    check(m.i[AVER_PHYS_TM_I_CONTACTS] >= 1, "at least one contact is reported");
    float pos[3];
    aver_phys_body_position(P, pos);
    check(std::fabs(pos[0]) < 1e-3f, "the body itself did not move");

    m = testMotion(P, nullptr, 2000, 0, 0, 0, kBodies);
    check(near(m.f[AVER_PHYS_TM_F_TRAVEL], 470.0f, 0.6f), "with no margin it travels the whole 470");
    m = testMotion(P, nullptr, 300, 0, 0, kMargin, kBodies);
    check(m.r == 0 && near(m.f[AVER_PHYS_TM_F_TRAVEL], 300.0f, 0.05f) && m.f[AVER_PHYS_TM_F_SAFE] == 1.0f && m.i[AVER_PHYS_TM_I_COLLIDER] == 0,
          "a motion that stops short of the wall is clear and travels in full");
    check(near(m.f[AVER_PHYS_TM_F_REMAINDER], 0.0f, 0.001f), "with no remainder");

    const int32_t ex[1] = {A};
    m = testMotion(P, nullptr, 2000, 0, 0, kMargin, kBodies, ex, 1);
    check(m.r == 0 && near(m.f[AVER_PHYS_TM_F_TRAVEL], 2000.0f, 0.05f), "an excluded wall does not block");
    m = testMotion(P, nullptr, 2000, 0, 0, kMargin, 0);
    check(m.r == 0, "flags 0 collides with neither bodies nor areas");

    // From another pose: the body is tested there, not where it is.
    float from[7];
    pose(from, 700, 0, 0);
    m = testMotion(P, from, 300, 0, 0, kMargin, kBodies);
    check(m.r == 0, "tested from x = 700 the wall at 490 is behind it");
    pose(from, 0, 150, 0);
    m = testMotion(P, from, 2000, 0, 0, kMargin, kBodies);
    check(m.r == 1 && near(m.f[AVER_PHYS_TM_F_TRAVEL], 469.9f, 0.6f), "tested from y = 150 it still meets the wall after 470");

    // Recovery: 5 cm into the wall (the box ends at x = 495).
    pose(from, 475, 0, 0);
    m = testMotion(P, from, 0, 0, 0, kMargin, kBodies);
    check(m.r == 0, "recovery alone is not a collision");
    check(near(m.f[AVER_PHYS_TM_F_RECOVERY], -5.05f, 0.4f) && std::fabs(m.f[AVER_PHYS_TM_F_RECOVERY + 1]) < 0.1f,
          "it is pushed back 5 cm plus half the margin, got " + f2s(m.f[AVER_PHYS_TM_F_RECOVERY]));
    check(near(m.f[AVER_PHYS_TM_F_TRAVEL], m.f[AVER_PHYS_TM_F_RECOVERY], 0.001f), "and the travel includes it");
    m = testMotion(P, from, 0, 0, 0, kMargin, kBodies | AVER_PHYS_SQ_RECOVERY_AS_COLLISION);
    check(m.r == 1 && m.i[AVER_PHYS_TM_I_COLLIDER] == A && m.i[AVER_PHYS_TM_I_CONTACTS] >= 1, "as a collision when asked");
    check(near(m.f[AVER_PHYS_TM_F_NORMAL], -1.0f, 0.05f), "with the wall's normal");
    // Moving away after recovery is clear.
    m = testMotion(P, from, -100, 0, 0, kMargin, kBodies);
    check(m.r == 0 && near(m.f[AVER_PHYS_TM_F_TRAVEL], -105.05f, 0.5f), "a motion away from the wall after recovery is clear");

    // Filtering: the body's own layer and mask decide, as in the simulation.
    aver_phys_body_set_collision_filter(P, 1u << 0, 1u << 1);
    aver_phys_body_set_collision_filter(A, 1u << 2, 0);
    m = testMotion(P, nullptr, 2000, 0, 0, kMargin, kBodies);
    check(m.r == 0, "layers that do not meet: the body passes through the wall");
    aver_phys_body_set_collision_filter(A, 1u << 1, 0);
    m = testMotion(P, nullptr, 2000, 0, 0, kMargin, kBodies);
    check(m.r == 1, "the wall on a layer the body's mask names blocks it");
    aver_phys_body_clear_collision_filter(P);
    aver_phys_body_clear_collision_filter(A);
    aver_phys_body_set_layer(P, 3);
    aver_phys_set_layer_collision(3, 0, 0);
    m = testMotion(P, nullptr, 2000, 0, 0, kMargin, kBodies);
    check(m.r == 0, "the 16-layer matrix applies too");
    aver_phys_reset_layer_collisions();
    aver_phys_body_set_layer(P, 0);

    // Refusals.
    m = testMotion(99999, nullptr, 1, 0, 0, kMargin, kBodies);
    check(m.r == -1, "a dead handle is refused");
    check(aver_phys_body_test_motion(P, nullptr, nullptr, kMargin, kBodies, nullptr, 0, 1, nullptr, nullptr, nullptr, nullptr) == -1, "a null motion is refused");
    aver_phys_shutdown();
}

static void testMotionFloorAndCorner() {
    AVER_INFO("=== body_test_motion: resting on a floor, sliding, corners, moving colliders ===");
    aver_phys_init();
    const int32_t floorB = wall(0, 0, -30, 500, 500, 10);     // top at z = -20
    const int32_t A = wall(500, 0, 0, 10, 200, 200);          // face at x = 490
    const int32_t P = aver_phys_add_dynamic_box(0, 0, 0, 20, 30, 20, 10);   // bottom at z = -20: resting on the floor
    const float kMargin = 0.1f;

    Motion m = testMotion(P, nullptr, 100, 0, 0, kMargin, kBodies);
    check(m.r == 0 && near(m.f[AVER_PHYS_TM_F_TRAVEL], 100.0f, 0.1f), "sliding along the floor is not a collision, travel 100");
    check(m.f[AVER_PHYS_TM_F_RECOVERY + 2] > 0.0f && m.f[AVER_PHYS_TM_F_RECOVERY + 2] < 0.1f,
          "resting in contact the body is lifted by half the margin: " + f2s(m.f[AVER_PHYS_TM_F_RECOVERY + 2]));
    m = testMotion(P, nullptr, 0, 0, -10, kMargin, kBodies);
    check(m.r == 1 && m.i[AVER_PHYS_TM_I_COLLIDER] == floorB && near(m.f[AVER_PHYS_TM_F_NORMAL + 2], 1.0f, 0.05f),
          "moving into the floor collides with it, normal (0,0,1)");
    check(near(m.f[AVER_PHYS_TM_F_TRAVEL + 2], 0.05f, 0.1f), "and travels nothing beyond the recovery");

    // A slight slope into the floor: it must stop on the floor's margin, never below it.
    m = testMotion(P, nullptr, 100, 0, -1, kMargin, kBodies);
    check(m.r == 1 && m.i[AVER_PHYS_TM_I_COLLIDER] == floorB && near(m.f[AVER_PHYS_TM_F_NORMAL + 2], 1.0f, 0.05f),
          "a 100 cm run that dips 1 cm collides with the floor");
    check(m.f[AVER_PHYS_TM_F_TRAVEL + 2] > -0.05f && m.f[AVER_PHYS_TM_F_SAFE] <= m.f[AVER_PHYS_TM_F_UNSAFE] + 1e-6f,
          "and never travels below the floor's margin, got dz " + f2s(m.f[AVER_PHYS_TM_F_TRAVEL + 2]));

    // A long, shallow approach to a flat face is still found (a cast without the shrunken-shape mode misses these).
    {
        const int32_t bx = aver_phys_shape_box(20, 30, 20, 0, nullptr, 0);
        const float mo[3] = {100, 0, -1};
        float pp[7], s = -1, u = -1, pt[3], nr[3];
        int32_t b = -1;
        pose(pp, 0, 0, 0.5f);
        aver_phys_space_cast_motion(bx, pp, mo, 0, kAll, kBodies, nullptr, 0, &s, &u, &b, pt, nr);
        check(b == floorB && near(u, 0.5f, 0.01f), "0.5 cm above the floor, a 1 cm dip over 100 cm touches at fraction 0.5, got " + f2s(u));
    }

    // Corner: floor under it, wall ahead. Both are contacts, the wall that stopped it first.
    m = testMotion(P, nullptr, 2000, 0, 0, kMargin, kBodies);
    check(m.r == 1 && m.i[AVER_PHYS_TM_I_COLLIDER] == A, "into the wall with a floor under it: the wall is the primary");
    bool floorSeen = false;
    for (int32_t i = 0; i < m.i[AVER_PHYS_TM_I_CONTACTS]; ++i)
        if (m.ci[i * AVER_PHYS_TC_I_COUNT + AVER_PHYS_TC_I_BODY] == floorB) floorSeen = true;
    check(m.i[AVER_PHYS_TM_I_CONTACTS] == 2 && floorSeen, "and the floor is the second contact, got " + std::to_string(m.i[AVER_PHYS_TM_I_CONTACTS]));
    check(near(m.cf[AVER_PHYS_TC_F_NORMAL], -1.0f, 0.05f), "contact 0 carries the wall's normal");
    const float ahead[3] = {2000, 0, 0};
    int32_t one = aver_phys_body_test_motion(P, nullptr, ahead, kMargin, kBodies, nullptr, 0, 1, m.f, m.i, m.cf, m.ci);
    check(one == 1 && m.i[AVER_PHYS_TM_I_CONTACTS] == 1, "maxContacts 1 reports one");

    // A moving collider: its velocity comes back.
    const int32_t D = aver_phys_add_dynamic_box(0, 900, 0, 50, 50, 50, 10);   // y in [850, 950]
    aver_phys_body_set_velocity(D, 0, 40, 0);
    float from[7];
    pose(from, 0, 600, 0);
    m = testMotion(P, from, 0, 2000, 0, kMargin, kBodies);
    check(m.r == 1 && m.i[AVER_PHYS_TM_I_COLLIDER] == D, "a dynamic box in the way blocks");
    check(near(m.f[AVER_PHYS_TM_F_VELOCITY + 1], 40.0f, 1.0f), "and its velocity is reported, got " + f2s(m.f[AVER_PHYS_TM_F_VELOCITY + 1]));
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("space-state queries and body_test_motion");

    testRay();
    testRayBackFacesAndInside();
    testIntersectCollideRest();
    testCastMotion();
    testBodyTestMotion();
    testMotionFloorAndCorner();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
