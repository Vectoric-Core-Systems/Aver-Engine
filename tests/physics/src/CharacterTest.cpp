// CharacterTest -- the CharacterVirtual settings physics_character_abi.h makes reachable.
//
// THE TEST THIS FILE EXISTS FOR IS testStairSteppingIsActuallyHonoured. Jolt takes the walk-stairs
// step-up and the stick-to-floor step-down as ARGUMENTS to ExtendedUpdate rather than as state on the
// character, so aver_phys_character_set_stair_stepping cannot forward to a Jolt setter -- it records
// two numbers, and the fixed-step loop has to read them back at the moment of the update. A setter
// that stores a value nothing ever reads is the exact shape this repository keeps shipping and then
// discovering unused, and the only way to tell the two apart is to WALK A CHARACTER AT A STEP and see
// whether it climbs.
//
// Everything else here follows the same rule as the other physics suites: assert observed motion, not
// return codes.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_character_abi.h"

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

static bool near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }
static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
static const f32 kDt = 1.0f / 60.0f;

// A character 180 cm tall standing on a floor whose top is z = 0. Its POSITION is the capsule's
// centre, so it starts at 90.
static int32_t worldWithCharacterOnFloor() {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50.0f, 4000.0f, 4000.0f, 50.0f);
    return aver_phys_character_create(30.0f, 180.0f, 0.0f, 0.0f, 90.0f);
}

// Walks the character forward along +X for `steps` fixed steps at `speed` cm/s and returns how far it
// actually travelled. A character that fails to climb a step stops dead against it, so the DISTANCE
// TRAVELLED is what distinguishes climbing from not.
static f32 walkForward(int32_t ch, f32 speed, int steps) {
    float start[3] = {0, 0, 0};
    aver_phys_character_position(ch, start);
    for (int i = 0; i < steps; ++i) {
        aver_phys_character_set_velocity(ch, speed, 0.0f, 0.0f);
        aver_phys_step(kDt);
    }
    float end[3] = {0, 0, 0};
    aver_phys_character_position(ch, end);
    return end[0] - start[0];
}

// ---------------------------------------------------------------------------------------------------

// THE ONE THAT PROVES THE WIRING. Same geometry, same walk, twice -- once with a step-up big enough to
// climb a 25 cm kerb and once with none at all. If the setter's numbers never reached ExtendedUpdate,
// both runs would use Jolt's own default and travel the same distance.
static void testStairSteppingIsActuallyHonoured() {
    AVER_INFO("=== the stair-stepping distances reach the step loop ===");

    const auto runWith = [](float stepUpCm) {
        const int32_t ch = worldWithCharacterOnFloor();
        // A 25 cm kerb across the character's path at x = 150.
        aver_phys_add_static_box(150.0f, 0.0f, 12.5f, 20.0f, 400.0f, 12.5f);
        aver_phys_character_set_stair_stepping(ch, stepUpCm, 50.0f);
        float up = -1.0f, down = -1.0f;
        aver_phys_character_stair_stepping(ch, &up, &down);
        const f32 travelled = walkForward(ch, 250.0f, 180);
        float p[3] = {0, 0, 0};
        aver_phys_character_position(ch, p);
        aver_phys_shutdown();
        return std::make_pair(travelled, up);
    };

    const auto climbing = runWith(40.0f);
    const auto blocked  = runWith(0.0f);

    check(near(climbing.second, 40.0f, 0.01f),
          "the step-up reads back as it was set, got " + f2s(climbing.second));
    check(climbing.first > 200.0f,
          "with a 40 cm step-up the character climbs the 25 cm kerb and keeps going, travelling " +
          f2s(climbing.first) + " cm");
    check(blocked.first < 150.0f,
          "with NO step-up the same character is stopped by the same kerb, travelling only " +
          f2s(blocked.first) + " cm (equal distances here would mean the setter's numbers never "
          "reached ExtendedUpdate)");
    check(climbing.first > blocked.first + 80.0f,
          "and the difference between the two is the feature working");
}

// ---------------------------------------------------------------------------------------------------

static void testMaxSlopeAngle() {
    AVER_INFO("=== max slope decides what counts as ground ===");
    const int32_t ch = worldWithCharacterOnFloor();

    float got = -1.0f;
    check(aver_phys_character_max_slope_angle(ch, &got) == 1, "the slope angle can be read");
    check(got > 0.5f && got < 1.5f,
          "and starts at the module's own 50 degrees, got " + f2s(got) + " rad");

    check(aver_phys_character_set_max_slope_angle(ch, 0.3f) == 1, "it can be set");
    check(aver_phys_character_max_slope_angle(ch, &got) == 1 && near(got, 0.3f, 0.01f),
          "and reads back, got " + f2s(got));

    aver_phys_shutdown();
}

static void testGroundInformation() {
    AVER_INFO("=== a character can ask what it is standing on ===");
    aver_phys_init();
    const int32_t floor = aver_phys_add_static_box(0, 0, -50.0f, 4000.0f, 4000.0f, 50.0f);
    aver_phys_set_entity(floor, 77);
    const int32_t ch = aver_phys_character_create(30.0f, 180.0f, 0.0f, 0.0f, 200.0f);

    // NO set_velocity IN THIS LOOP, deliberately. physics_abi.h says the vertical component is managed
    // by the simulation "unless it is set here" -- so zeroing the velocity every step, which is the
    // natural-looking way to write a settle loop, pins the character in mid-air and it never lands.
    for (int i = 0; i < 180; ++i) aver_phys_step(kDt);

    check(aver_phys_character_grounded(ch) == 1, "it has landed");
    check(aver_phys_character_ground_state(ch) == AVER_PHYS_GROUND_ON_GROUND,
          "and reports ON_GROUND, got " + std::to_string(aver_phys_character_ground_state(ch)));

    float n[3] = {0, 0, 0};
    check(aver_phys_character_ground_normal(ch, n) == 1, "the ground normal is readable");
    check(near(n[2], 1.0f, 0.05f),
          "and points straight up in ENGINE axes on a flat floor, got (" + f2s(n[0]) + ", " +
          f2s(n[1]) + ", " + f2s(n[2]) + ") -- a normal of (0,1,0) here would mean Jolt's axes leaked");

    check(aver_phys_character_ground_body(ch) == floor,
          "and it knows WHICH body it is standing on, got " +
          std::to_string(aver_phys_character_ground_body(ch)));

    float gp[3] = {0, 0, 0};
    check(aver_phys_character_ground_position(ch, gp) == 1 && near(gp[2], 0.0f, 6.0f),
          "the contact point is on the floor's surface, z = " + f2s(gp[2]));

    float gv[3] = {1, 1, 1};
    check(aver_phys_character_ground_velocity(ch, gv) == 1 &&
          near(gv[0], 0.0f, 1.0f) && near(gv[1], 0.0f, 1.0f) && near(gv[2], 0.0f, 1.0f),
          "and a static floor is not moving under it");

    aver_phys_shutdown();
}

// CROUCHING IS A SHAPE SWAP THAT CAN FAIL, and the failing case is the useful one: standing up under a
// low ceiling must be refused, or the character grows into the geometry above it.
static void testShapeSwapAndItsFailure() {
    AVER_INFO("=== the shape swap is crouching, and it refuses to stand up under a ceiling ===");
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50.0f, 4000.0f, 4000.0f, 50.0f);
    const int32_t ch = aver_phys_character_create(30.0f, 180.0f, 0.0f, 0.0f, 90.0f);
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);

    check(aver_phys_character_set_shape(ch, 30.0f, 90.0f, 5.0f) == 1,
          "crouching to half height succeeds in the open");
    check(aver_phys_character_set_shape(ch, 30.0f, 180.0f, 5.0f) == 1,
          "and standing back up succeeds with nothing overhead");

    // THE ORDER HERE IS THE TEST. Crouch FIRST and let the character settle to its new centre, THEN
    // put the ceiling just above its crouched head. Adding the ceiling while the character is still
    // standing would place the slab INSIDE it, and the crouch that follows would fail for that reason
    // rather than the one under test.
    check(aver_phys_character_set_shape(ch, 30.0f, 90.0f, 5.0f) == 1, "it crouches");
    for (int i = 0; i < 90; ++i) aver_phys_step(kDt);
    // Crouched it is 90 cm tall with its centre near 45, so it spans roughly 0..90. A slab spanning
    // 100..120 clears that and is well inside where its 180 cm standing self would reach.
    aver_phys_add_static_box(0, 0, 110.0f, 400.0f, 400.0f, 10.0f);
    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);
    check(aver_phys_character_set_shape(ch, 30.0f, 180.0f, 5.0f) == 0,
          "and standing up into the ceiling is REFUSED -- which is what stops the character growing "
          "into the geometry above it");

    check(aver_phys_character_set_shape(ch, 30.0f, 40.0f, 5.0f) == 0,
          "a shape too short for its own radius is refused, like character_create's own guard");

    aver_phys_shutdown();
}

static void testDeadHandles() {
    AVER_INFO("=== dead handles and null out-pointers ===");
    const int32_t ch = worldWithCharacterOnFloor();
    const int32_t dead = 999999;
    float tmp[3];
    check(aver_phys_character_set_max_slope_angle(dead, 0.5f) == 0, "a dead handle refuses a slope");
    check(aver_phys_character_ground_normal(dead, tmp) == 0, "and a ground normal");
    check(aver_phys_character_ground_body(dead) == 0, "and reports no ground body");
    check(aver_phys_character_set_shape(dead, 30.0f, 180.0f, 5.0f) == 0, "and a shape swap");
    check(aver_phys_character_ground_normal(ch, nullptr) == 0, "a null out-pointer is refused");
    check(aver_phys_character_max_slope_angle(ch, nullptr) == 0, "on every getter");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("character: slope, stairs, ground, crouch");

    testStairSteppingIsActuallyHonoured();
    testMaxSlopeAngle();
    testGroundInformation();
    testShapeSwapAndItsFailure();
    testDeadHandles();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
