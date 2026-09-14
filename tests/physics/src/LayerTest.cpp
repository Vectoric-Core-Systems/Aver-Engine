// LayerTest -- collision layers, the matrix, and the filtered queries.
//
// THE CLAIM THAT MATTERS MOST HERE IS A NEGATIVE ONE. Adding user layers changed how an object layer
// is encoded (`userLayer * 2 + moving`), which is the number Jolt uses to decide both which broad-phase
// tree a body lives in and which other bodies it is tested against. Every body this engine has ever
// created went through that encoding, so the first thing this file checks is that a project which
// never mentions layers behaves EXACTLY as it did: layer 0 collides with everything, static bodies
// still hold dynamic ones up, and sensors still fire.
//
// The second half checks the thing that is new, and checks it by OBSERVED COLLISION -- two bodies that
// should pass through each other, and the same two that should not.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
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

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
static const f32 kDt = 1.0f / 60.0f;

static f32 dropAndSettle(int32_t body, int steps = 240) {
    for (int i = 0; i < steps; ++i) aver_phys_step(kDt);
    float p[3] = {0, 0, 0};
    aver_phys_body_position(body, p);
    return p[2];
}

// ---------------------------------------------------------------------------------------------------

// THE BACK-COMPATIBILITY CHECK. Nothing here mentions a layer; this is the world every existing
// project builds, and it must be the world it always was.
static void testDefaultWorldIsUnchanged() {
    AVER_INFO("=== a project that never mentions layers is unchanged ===");
    aver_phys_init();

    aver_phys_add_static_box(0, 0, -50.0f, 2000.0f, 2000.0f, 50.0f);   // floor, top at z = 0
    const int32_t crate = aver_phys_add_dynamic_box(0, 0, 400.0f, 50.0f, 50.0f, 50.0f, 10.0f);

    const f32 z = dropAndSettle(crate);
    check(std::fabs(z - 50.0f) < 6.0f,
          "a crate still lands on a static floor and rests at its half-height, got " + f2s(z));

    check(aver_phys_body_layer(crate) == 0, "and it is on layer 0 without being told to be");
    check(aver_phys_layer_collision(0, 0) == 1, "layer 0 collides with itself by default");
    check(aver_phys_layer_collision(3, 9) == 1, "and every other pair starts colliding too");

    aver_phys_shutdown();
}

// SENSORS STILL FIRE. A sensor is the one body this module deliberately puts in the MOVING half while
// being static, so it is the case most likely to break when the encoding of that half changes.
static void testSensorsStillFire() {
    AVER_INFO("=== a sensor still notices a body falling through it ===");
    aver_phys_init();
    aver_phys_add_sensor_box(0, 0, 100.0f, 100.0f, 100.0f, 20.0f);
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 400.0f, 20.0f, 1.0f);

    int32_t seen = 0;
    for (int i = 0; i < 240 && !seen; ++i) {
        aver_phys_step(kDt);
        for (int32_t k = 0; k < aver_phys_overlap_count(); ++k) {
            int32_t s = 0, b = 0, entered = 0;
            if (aver_phys_overlap_get(k, &s, &b, &entered) && entered && b == ball) seen = 1;
        }
    }
    check(seen == 1, "the overlap queue reported the ball entering the sensor");
    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

// THE NEW BEHAVIOUR, checked as motion rather than as a flag. Same geometry twice: once with the two
// layers colliding, once not. The falling body either rests on the platform or goes straight past it.
static void testLayersActuallySeparate() {
    AVER_INFO("=== two layers that do not collide let bodies pass through ===");
    aver_phys_init();

    // A static platform on layer 1, and a crate on layer 2, both well above any floor.
    const int32_t platform = aver_phys_add_static_box(0, 0, 0, 300.0f, 300.0f, 20.0f);
    const int32_t crate    = aver_phys_add_dynamic_box(0, 0, 400.0f, 30.0f, 30.0f, 30.0f, 5.0f);
    check(aver_phys_body_set_layer(platform, 1) == 1, "the platform moves to layer 1");
    check(aver_phys_body_set_layer(crate, 2) == 1, "and the crate to layer 2");
    check(aver_phys_body_layer(platform) == 1 && aver_phys_body_layer(crate) == 2,
          "both read back on the layers they were put on");

    // Still colliding: it lands.
    const f32 landed = dropAndSettle(crate);
    check(landed > 20.0f, "with the layers colliding it rests on the platform at " + f2s(landed));

    // Now turn that pair off and drop it again from the same height.
    check(aver_phys_set_layer_collision(1, 2, 0) == 1, "layers 1 and 2 are told not to collide");
    check(aver_phys_layer_collision(1, 2) == 0, "which reads back");
    check(aver_phys_layer_collision(2, 1) == 0, "and is SYMMETRIC -- asking the other way round agrees");

    aver_phys_body_set_position(crate, 0, 0, 400.0f);
    aver_phys_body_set_velocity(crate, 0, 0, 0);
    const f32 fell = dropAndSettle(crate);
    check(fell < -100.0f,
          "and now it falls straight through, reaching " + f2s(fell) + " (it rested at " +
          f2s(landed) + " a moment ago with the same geometry)");

    aver_phys_shutdown();
}

// A BODY'S LAYER IS NOT ITS MOTION TYPE, and the encoding packs them into one number -- so the thing
// most likely to go wrong is that changing one silently changes the other.
static void testLayerDoesNotChangeMotionType() {
    AVER_INFO("=== moving a body between layers leaves it static or dynamic as it was ===");
    aver_phys_init();

    const int32_t stat = aver_phys_add_static_box(0, 0, 0, 100.0f, 100.0f, 100.0f);
    const int32_t dyn  = aver_phys_add_dynamic_box(500.0f, 0, 0, 50.0f, 50.0f, 50.0f, 1.0f);

    aver_phys_body_set_layer(stat, 7);
    aver_phys_body_set_layer(dyn, 7);
    check(aver_phys_body_motion_type(stat) == AVER_PHYS_MOTION_STATIC,
          "the static body is still static after changing layer");
    check(aver_phys_body_motion_type(dyn) == AVER_PHYS_MOTION_DYNAMIC,
          "and the dynamic one still dynamic");

    // And the static one still does not fall, which is the behavioural half of the same claim.
    const f32 z = dropAndSettle(stat, 60);
    check(std::fabs(z) < 1.0f, "the static body has not moved, z = " + f2s(z));

    check(aver_phys_body_set_layer(stat, 99) == 0, "an out-of-range layer is refused");
    check(aver_phys_body_set_layer(999999, 1) == 0, "and so is a dead handle");
    check(aver_phys_body_layer(999999) == -1, "whose layer reads -1, not 0 -- 0 is a real layer");

    aver_phys_shutdown();
}

// ---------------------------------------------------------------------------------------------------

static void testFilteredQueries() {
    AVER_INFO("=== the _ex queries filter by layer and skip a body ===");
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);

    // Two walls in a line ahead of the origin: the near one on layer 4, the far one on layer 5.
    const int32_t near_ = aver_phys_add_static_box(200.0f, 0, 0, 10.0f, 200.0f, 200.0f);
    const int32_t far_  = aver_phys_add_static_box(600.0f, 0, 0, 10.0f, 200.0f, 200.0f);
    aver_phys_body_set_layer(near_, 4);
    aver_phys_body_set_layer(far_, 5);
    aver_phys_set_entity(near_, 11);
    aver_phys_set_entity(far_, 22);

    float pt[3], nrm[3];
    int32_t ent = 0;

    // Unfiltered: the near wall.
    int32_t hit = aver_phys_raycast_ex(0, 0, 0, 1, 0, 0, 2000.0f,
                                       AVER_PHYS_LAYER_MASK_ALL, 0, pt, nrm, &ent);
    check(hit == near_, "an all-layers ray hits the nearest wall");
    check(ent == 11, "and reports its entity, got " + std::to_string(ent));

    // Layer 5 only: the ray must pass through the near wall entirely.
    hit = aver_phys_raycast_ex(0, 0, 0, 1, 0, 0, 2000.0f,
                               AVER_PHYS_LAYER_BIT(5), 0, pt, nrm, &ent);
    check(hit == far_, "a ray masked to layer 5 passes through the near wall and hits the far one");
    check(ent == 22, "reporting the far wall's entity, got " + std::to_string(ent));

    // ignoreBody does the same job for one specific body -- the first-person weapon case.
    hit = aver_phys_raycast_ex(0, 0, 0, 1, 0, 0, 2000.0f,
                               AVER_PHYS_LAYER_MASK_ALL, near_, pt, nrm, &ent);
    check(hit == far_, "and ignoreBody skips the near wall without needing a layer at all");

    // A mask with neither layer in it hits nothing.
    hit = aver_phys_raycast_ex(0, 0, 0, 1, 0, 0, 2000.0f,
                               AVER_PHYS_LAYER_BIT(9), 0, pt, nrm, &ent);
    check(hit == 0, "a mask naming no wall's layer hits nothing");

    // The sphere cast follows the same rules.
    hit = aver_phys_sphere_cast_ex(0, 0, 0, 1, 0, 0, 2000.0f, 20.0f,
                                   AVER_PHYS_LAYER_BIT(5), 0, pt, nrm);
    check(hit == far_, "a masked sphere cast also passes the near wall");

    // And so does the overlap. A sphere big enough to touch both, masked to one.
    int32_t bodies[8] = {0};
    int32_t n = aver_phys_overlap_sphere_ex(400.0f, 0, 0, 400.0f,
                                            AVER_PHYS_LAYER_MASK_ALL, 0, bodies, 8);
    check(n == 2, "an unmasked overlap finds both walls, got " + std::to_string(n));
    n = aver_phys_overlap_sphere_ex(400.0f, 0, 0, 400.0f,
                                    AVER_PHYS_LAYER_BIT(4), 0, bodies, 8);
    check(n == 1 && bodies[0] == near_, "and masked to layer 4 finds only the near one");

    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("collision layers, the matrix, and filtered queries");

    testDefaultWorldIsUnchanged();
    testSensorsStillFire();
    testLayersActuallySeparate();
    testLayerDoesNotChangeMotionType();
    testFilteredQueries();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
