// LayerFilterTest -- 32-bit layer/mask, per-pair collision exceptions, joint exclusion and the mask queries.
//
// The first test is the negative claim: nothing here changes a world that never calls the new functions. The rest check the rule by
// OBSERVED MOTION (a body rests or falls through) and by aver_phys_bodies_can_collide, which reads the same answer Jolt's broad phase gets.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_joints_abi.h"
#include "aver/physics/physics_layers_abi.h"

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

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
static const float kDt = 1.0f / 60.0f;

static float zAfter(int32_t body, int steps) {
    for (int i = 0; i < steps; ++i) aver_phys_step(kDt);
    float p[3] = {0, 0, 0};
    aver_phys_body_position(body, p);
    return p[2];
}

static int32_t floorBox() { return aver_phys_add_static_box(0, 0, -50.0f, 2000.0f, 2000.0f, 50.0f); }   // top at z = 0

// ---------------------------------------------------------------------------------------------------

static void testDefaultsUnchanged() {
    AVER_INFO("=== a world that never calls the new functions is unchanged ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t crate = aver_phys_add_dynamic_box(0, 0, 300.0f, 50.0f, 50.0f, 50.0f, 10.0f);
    check(aver_phys_bodies_can_collide(fl, crate) == 1, "floor and crate can collide");
    uint32_t l = 7, m = 7;
    check(aver_phys_body_collision_filter(crate, &l, &m) == 0 && l == 7 && m == 7, "no layer filter is reported, outputs untouched");
    check(aver_phys_body_has_collision_exception(fl, crate) == 0, "no exceptions");
    const float z = zAfter(crate, 240);
    check(std::fabs(z - 50.0f) < 6.0f, "the crate rests on the floor, z = " + f2s(z));
    aver_phys_shutdown();
}

// The rule, as a truth table over bodies_can_collide.
static void testRule() {
    AVER_INFO("=== A collides with B when A.mask & B.layer or B.mask & A.layer ===");
    aver_phys_init();
    const int32_t a = aver_phys_add_dynamic_box(0, 0, 100.0f, 10, 10, 10, 1.0f);
    const int32_t b = aver_phys_add_dynamic_box(500.0f, 0, 100.0f, 10, 10, 10, 1.0f);
    const int32_t plain = aver_phys_add_dynamic_box(1000.0f, 0, 100.0f, 10, 10, 10, 1.0f);
    const int32_t st1 = aver_phys_add_static_box(2000.0f, 0, 0, 10, 10, 10);
    const int32_t st2 = aver_phys_add_static_box(2100.0f, 0, 0, 10, 10, 10);

    check(aver_phys_body_set_collision_filter(a, 1u, 2u) == 1 && aver_phys_body_set_collision_filter(b, 2u, 0u) == 1, "A layer 1 mask 2, B layer 2 mask 0");
    check(aver_phys_bodies_can_collide(a, b) == 1 && aver_phys_bodies_can_collide(b, a) == 1, "A wants B: they collide, in both orders (one side is enough)");
    aver_phys_body_set_collision_filter(a, 1u, 4u);
    check(aver_phys_bodies_can_collide(a, b) == 0, "A.mask 4 misses B.layer 2 and B.mask 0 wants nothing: no collision");
    aver_phys_body_set_collision_filter(b, 2u, 1u);
    check(aver_phys_bodies_can_collide(a, b) == 1, "B.mask 1 now hits A.layer 1: they collide again");
    aver_phys_body_set_collision_filter(a, 0u, 0u);
    aver_phys_body_set_collision_filter(b, 0u, 0xFFFFFFFFu);
    check(aver_phys_bodies_can_collide(a, b) == 0, "layer 0 is hit by nobody, mask 0 wants nobody: no collision even against an all-ones mask");
    uint32_t l = 0, m = 0;
    check(aver_phys_body_collision_filter(b, &l, &m) == 1 && l == 0u && m == 0xFFFFFFFFu, "the bits read back");
    check(aver_phys_bodies_can_collide(a, plain) == 1, "a filtered body against an unfiltered one follows the 16-layer matrix alone");

    aver_phys_set_layer_collision(0, 3, 0);
    aver_phys_body_set_layer(plain, 3);
    aver_phys_body_set_collision_filter(a, 1u, 1u);
    aver_phys_body_set_collision_filter(b, 1u, 1u);
    check(aver_phys_bodies_can_collide(a, b) == 1, "layers 1/1 collide");
    aver_phys_body_set_layer(b, 3);
    check(aver_phys_bodies_can_collide(a, b) == 0, "but the matrix still has the first word: user layers 0 and 3 are off");
    aver_phys_reset_layer_collisions();
    check(aver_phys_bodies_can_collide(a, b) == 1, "reset, and the mask rule decides again");

    aver_phys_body_set_collision_filter(st1, 1u, 1u);
    aver_phys_body_set_collision_filter(st2, 1u, 1u);
    check(aver_phys_bodies_can_collide(st1, st2) == 0, "static against static never collides, whatever the masks say");

    check(aver_phys_body_clear_collision_filter(a) == 1, "a filter can be cleared");
    check(aver_phys_body_collision_filter(a, &l, &m) == 0, "and then reads as absent");
    check(aver_phys_body_set_collision_filter(999999, 1u, 1u) == 0, "a dead handle is refused");
    aver_phys_shutdown();
}

static void testObservedMotion() {
    AVER_INFO("=== a body rests on a floor or falls through it, by layer and mask ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 300.0f, 30.0f, 5.0f);
    aver_phys_body_set_collision_filter(fl, 2u, 0u);
    aver_phys_body_set_collision_filter(ball, 1u, 3u);
    float z = zAfter(ball, 240);
    check(std::fabs(z - 30.0f) < 6.0f, "ball.mask hits floor.layer: it rests on the floor, z = " + f2s(z));

    aver_phys_body_set_collision_filter(ball, 1u, 1u);
    aver_phys_body_set_position(ball, 0, 0, 300.0f);
    aver_phys_body_set_velocity(ball, 0, 0, 0);
    z = zAfter(ball, 240);
    check(z < -100.0f, "ball.mask misses floor.layer and the floor wants nothing: it falls through, z = " + f2s(z));

    aver_phys_body_set_collision_filter(fl, 2u, 1u);   // now the floor wants layer 1
    aver_phys_body_set_position(ball, 0, 0, 300.0f);
    aver_phys_body_set_velocity(ball, 0, 0, 0);
    z = zAfter(ball, 240);
    check(std::fabs(z - 30.0f) < 6.0f, "the floor's mask alone is enough (the OR): it rests again, z = " + f2s(z));
    aver_phys_shutdown();
}

// A body asleep on the floor must wake when the floor stops being solid for it.
static void testChangeWakesSleepers() {
    AVER_INFO("=== changing a filter wakes a resting body ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t crate = aver_phys_add_dynamic_box(0, 0, 60.0f, 50.0f, 50.0f, 50.0f, 10.0f);
    aver_phys_body_set_collision_filter(fl, 1u, 1u);
    aver_phys_body_set_collision_filter(crate, 1u, 1u);
    float z = zAfter(crate, 300);
    check(aver_phys_body_is_active(crate) == 0, "after five seconds at rest the crate sleeps");
    aver_phys_body_set_collision_filter(crate, 2u, 2u);
    aver_phys_body_set_collision_filter(fl, 1u, 1u);
    check(aver_phys_bodies_can_collide(fl, crate) == 0, "the floor and the crate no longer meet");
    z = zAfter(crate, 120);
    check(z < -20.0f, "and the sleeper fell through, z = " + f2s(z));
    aver_phys_shutdown();
}

static void testExceptions() {
    AVER_INFO("=== collision exceptions ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t lower = aver_phys_add_dynamic_box(0, 0, 50.0f, 50.0f, 50.0f, 50.0f, 10.0f);
    const int32_t upper = aver_phys_add_dynamic_box(0, 0, 400.0f, 50.0f, 50.0f, 50.0f, 10.0f);
    check(aver_phys_body_add_collision_exception(upper, lower) == 1, "an exception is added between two unfiltered bodies");
    check(aver_phys_body_has_collision_exception(lower, upper) == 1, "it is symmetric");
    check(aver_phys_bodies_can_collide(upper, lower) == 0, "the pair no longer collides");
    check(aver_phys_bodies_can_collide(upper, fl) == 1 && aver_phys_bodies_can_collide(lower, fl) == 1, "but each still collides with the floor");
    check(aver_phys_body_add_collision_exception(upper, upper) == 0, "a body cannot be excepted from itself");
    check(aver_phys_body_add_collision_exception(upper, lower) == 1, "adding twice is accepted");
    float z = zAfter(upper, 240);
    check(std::fabs(z - 50.0f) < 6.0f, "the upper crate falls through the lower one and rests on the floor, z = " + f2s(z));

    check(aver_phys_body_remove_collision_exception(upper, lower) == 1, "removing it once is enough");
    check(aver_phys_body_has_collision_exception(upper, lower) == 0 && aver_phys_bodies_can_collide(upper, lower) == 1, "the pair collides again");
    check(aver_phys_body_remove_collision_exception(upper, lower) == 0, "removing a missing exception reports 0");

    const int32_t other = aver_phys_add_dynamic_box(900.0f, 0, 60.0f, 20, 20, 20, 1.0f);
    aver_phys_body_add_collision_exception(upper, lower);
    aver_phys_body_add_collision_exception(upper, other);
    check(aver_phys_body_clear_collision_exceptions(upper) == 1, "an exception list can be cleared");
    check(aver_phys_bodies_can_collide(upper, lower) == 1 && aver_phys_bodies_can_collide(upper, other) == 1, "every pair collides again");

    // A removed body takes its exceptions with it, and a later body does not inherit them.
    aver_phys_body_add_collision_exception(lower, other);
    aver_phys_remove_body(other);
    check(aver_phys_body_has_collision_exception(lower, other) == 0, "nothing is left of an exception to a removed body");
    const int32_t fresh = aver_phys_add_dynamic_box(900.0f, 0, 60.0f, 20, 20, 20, 1.0f);
    check(aver_phys_bodies_can_collide(lower, fresh) == 1 && aver_phys_body_has_collision_exception(lower, fresh) == 0, "and a new body starts clean");

    // An exception composes with a layer filter on the same body.
    aver_phys_body_set_collision_filter(lower, 1u, 1u);
    aver_phys_body_set_collision_filter(upper, 1u, 1u);
    aver_phys_body_add_collision_exception(upper, lower);
    check(aver_phys_bodies_can_collide(upper, lower) == 0, "an exception beats a matching mask");
    aver_phys_body_remove_collision_exception(upper, lower);
    check(aver_phys_bodies_can_collide(upper, lower) == 1, "and removing it hands the pair back to the mask");
    aver_phys_shutdown();
}

static void testJointExclusion() {
    AVER_INFO("=== a joint can exclude its two bodies from colliding ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 60.0f, 50.0f, 50.0f, 50.0f, 10.0f);
    const float pa[3] = {0, 0, 60.0f}, pb[3] = {0, 0, 0};
    // A rope loose enough to constrain nothing in this test.
    const int32_t j = aver_phys_joint_distance(box, fl, pa, pb, 0.0f, 100000.0f);
    check(j != 0, "a loose distance joint between the box and the floor");
    check(aver_phys_joint_exclude_collision(j) == 0, "a new joint excludes nothing, as before");
    check(aver_phys_bodies_can_collide(box, fl) == 1, "the pair collides");
    check(aver_phys_joint_set_exclude_collision(j, 1) == 1 && aver_phys_joint_exclude_collision(j) == 1, "the exclusion is switched on");
    check(aver_phys_bodies_can_collide(box, fl) == 0, "the pair no longer collides");
    float z = zAfter(box, 240);
    check(z < -100.0f, "the box falls through the floor it is jointed to, z = " + f2s(z));

    check(aver_phys_joint_set_exclude_collision(j, 0) == 1 && aver_phys_bodies_can_collide(box, fl) == 1, "switched off, the pair collides again");
    aver_phys_joint_set_exclude_collision(j, 1);
    check(aver_phys_body_has_collision_exception(box, fl) == 0, "a joint's exclusion is not a user exception");
    check(aver_phys_joint_remove(j) == 1 && aver_phys_bodies_can_collide(box, fl) == 1, "removing the joint ends its exclusion");

    // Two joints on one pair: both must go.
    const int32_t j1 = aver_phys_joint_distance(box, fl, pa, pb, 0.0f, 100000.0f);
    const int32_t j2 = aver_phys_joint_distance(box, fl, pa, pb, 0.0f, 100000.0f);
    aver_phys_joint_set_exclude_collision(j1, 1);
    aver_phys_joint_set_exclude_collision(j2, 1);
    aver_phys_joint_remove(j1);
    check(aver_phys_bodies_can_collide(box, fl) == 0, "one of two excluding joints is gone and the pair still does not collide");
    aver_phys_joint_remove(j2);
    check(aver_phys_bodies_can_collide(box, fl) == 1, "the second goes and it does");

    // A joint to the world has no pair to exclude.
    const int32_t jw = aver_phys_joint_distance(box, AVER_PHYS_WORLD_BODY, pa, pa, 0.0f, 100000.0f);
    check(aver_phys_joint_set_exclude_collision(jw, 1) == 1 && aver_phys_joint_exclude_collision(jw) == 1, "a joint to the world takes the flag and excludes nothing");
    check(aver_phys_joint_set_exclude_collision(999999, 1) == 0, "a dead joint is refused");
    aver_phys_shutdown();
}

static void testSensors() {
    AVER_INFO("=== filters apply to sensors ===");
    for (int pass = 0; pass < 2; ++pass) {
        aver_phys_init();
        const int32_t sensor = aver_phys_add_sensor_box(0, 0, 100.0f, 100.0f, 100.0f, 20.0f);
        const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 400.0f, 20.0f, 1.0f);
        aver_phys_body_set_collision_filter(sensor, 2u, 2u);
        aver_phys_body_set_collision_filter(ball, 1u, pass == 0 ? 1u : 2u);
        int32_t seen = 0;
        for (int i = 0; i < 240; ++i) {
            aver_phys_step(kDt);
            for (int32_t k = 0; k < aver_phys_overlap_count(); ++k) {
                int32_t s = 0, b = 0, entered = 0;
                if (aver_phys_overlap_get(k, &s, &b, &entered) && entered && b == ball) seen = 1;
            }
        }
        check(seen == (pass == 0 ? 0 : 1), pass == 0 ? "a ball whose mask misses the sensor's layer is not noticed" : "and one whose mask hits it is");
        aver_phys_shutdown();
    }
}

// The filter lives in the body's collision group, so a body rebuilt under the same handle keeps it.
static void testSurvivesRebuild() {
    AVER_INFO("=== a filter survives a static body becoming movable ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t plat = aver_phys_add_static_box(0, 0, 500.0f, 100.0f, 100.0f, 20.0f);
    aver_phys_body_set_collision_filter(plat, 8u, 16u);
    aver_phys_body_add_collision_exception(plat, fl);
    check(aver_phys_body_set_motion_type(plat, AVER_PHYS_MOTION_KINEMATIC) == 1, "the platform becomes kinematic");
    uint32_t l = 0, m = 0;
    check(aver_phys_body_collision_filter(plat, &l, &m) == 1 && l == 8u && m == 16u, "its layer and mask are still there");
    check(aver_phys_body_has_collision_exception(plat, fl) == 1, "and so is its exception");
    aver_phys_shutdown();
}

static void testMaskQueries() {
    AVER_INFO("=== mask queries ===");
    aver_phys_init();
    const int32_t nearWall = aver_phys_add_static_box(300.0f, 0, 0, 20.0f, 200.0f, 200.0f);
    const int32_t farWall  = aver_phys_add_static_box(600.0f, 0, 0, 20.0f, 200.0f, 200.0f);
    const int32_t plain    = aver_phys_add_static_box(900.0f, 0, 0, 20.0f, 200.0f, 200.0f);
    aver_phys_body_set_collision_filter(nearWall, 1u << 4, 0u);
    aver_phys_body_set_collision_filter(farWall, 1u << 20, 0u);   // a layer a 16-layer mask cannot name
    aver_phys_body_set_layer(plain, 5);

    float pt[3], nrm[3];
    int32_t ent = 0;
    check(aver_phys_raycast_mask(0, 0, 0, 1, 0, 0, 2000.0f, 0xFFFFFFFFu, 0, pt, nrm, &ent) == nearWall, "an all-ones mask hits the near wall");
    check(aver_phys_raycast_mask(0, 0, 0, 1, 0, 0, 2000.0f, 1u << 20, 0, pt, nrm, &ent) == farWall, "a mask naming layer bit 20 passes the near wall and hits the far one");
    check(aver_phys_raycast_mask(0, 0, 0, 1, 0, 0, 2000.0f, 1u << 5, 0, pt, nrm, &ent) == plain, "an unfiltered body is addressed by the bit of its user layer (5)");
    check(aver_phys_raycast_mask(0, 0, 0, 1, 0, 0, 2000.0f, 1u << 6, 0, pt, nrm, &ent) == 0, "a mask naming nothing hits nothing");
    check(aver_phys_raycast_mask(0, 0, 0, 1, 0, 0, 2000.0f, 0xFFFFFFFFu, nearWall, pt, nrm, &ent) == farWall, "ignoreBody skips the near wall");
    check(aver_phys_sphere_cast_mask(0, 0, 0, 1, 0, 0, 2000.0f, 20.0f, 1u << 20, 0, pt, nrm) == farWall, "the sphere cast follows the mask");

    int32_t out[8] = {0};
    int n = aver_phys_overlap_sphere_mask(450.0f, 0, 0, 400.0f, 0xFFFFFFFFu, 0, out, 8);
    check(n == 2, "an unmasked overlap finds both walls in reach, got " + std::to_string(n));
    n = aver_phys_overlap_sphere_mask(450.0f, 0, 0, 400.0f, 1u << 4, 0, out, 8);
    check(n == 1 && out[0] == nearWall, "masked to bit 4 it finds only the near wall");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("layer/mask, exceptions, joint exclusion");

    testDefaultsUnchanged();
    testRule();
    testObservedMotion();
    testChangeWakesSleepers();
    testExceptions();
    testJointExclusion();
    testSensors();
    testSurvivesRebuild();
    testMaskQueries();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
