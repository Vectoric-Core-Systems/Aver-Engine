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

    // The getter reads the capsule the character HAS, so after the two refusals above it must still
    // say crouched -- a getter that echoed the last numbers asked for would say 40 here.
    float r = 0.0f, h = 0.0f;
    check(aver_phys_character_shape(ch, &r, &h) == 1 && near(r, 30.0f, 0.01f) && near(h, 90.0f, 0.01f),
          "the shape reads back as the crouched capsule both refusals left in place, radius " +
          f2s(r) + " height " + f2s(h));
    const int32_t fresh = aver_phys_character_create(34.0f, 180.0f, 900.0f, 0.0f, 90.0f);
    check(aver_phys_character_shape(fresh, &r, &h) == 1 && near(r, 34.0f, 0.01f) && near(h, 180.0f, 0.01f),
          "and a new character reads back the radius and TOTAL height it was created with, radius " +
          f2s(r) + " height " + f2s(h));

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
    check(aver_phys_character_shape(dead, tmp, tmp + 1) == 0, "a dead handle has no shape to read");
    check(aver_phys_character_inherited_velocity(dead, tmp) == 0 &&
          aver_phys_character_set_inherited_velocity(dead, 1.0f, 0.0f, 0.0f) == 0,
          "and no inherited velocity to read or set");
    check(aver_phys_character_inherited_velocity(ch, nullptr) == 0, "which refuses a null out-pointer too");
    check(aver_phys_character_shape(ch, nullptr, tmp) == 0 && aver_phys_character_shape(ch, tmp, nullptr) == 0,
          "and the shape getter refuses either null out-pointer");
    aver_phys_shutdown();
}

// A CharacterVirtual is not a rigid body, so no friction ever reaches it: riding a moving deck is the
// ground's velocity, added by aver_phys_step. This is the check the owner made by hand -- stand on the
// train, does it take you along -- plus the three ways that addition could go wrong: leaking into the
// velocity a driver reads back and writes again every frame, dropping a jumper behind the deck, and
// surviving a teleport.
static void testRidesAMovingDeck() {
    AVER_INFO("=== a character standing on a moving deck is carried with it ===");
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50.0f, 8000.0f, 8000.0f, 50.0f);   // still ground, top at z = 0
    // A 6 m square deck with its top at z = 200, made kinematic the way an animated collider is.
    const int32_t deck = aver_phys_add_dynamic_box(0.0f, 0.0f, 190.0f, 300.0f, 300.0f, 10.0f, 1000.0f);
    check(aver_phys_body_set_motion_type(deck, AVER_PHYS_MOTION_KINEMATIC) == 1, "the deck is kinematic");
    const int32_t rider = aver_phys_character_create(30.0f, 180.0f, 0.0f, 0.0f, 291.0f);
    const int32_t bystander = aver_phys_character_create(30.0f, 180.0f, 0.0f, 1500.0f, 91.0f);

    // A driver with no input, as driveDefaultPawnWalk and Character.Drive are: zero horizontal velocity,
    // the vertical part read back and written again.
    auto standStill = [](int32_t ch) {
        float v[3] = {0, 0, 0};
        aver_phys_character_velocity(ch, v);
        aver_phys_character_set_velocity(ch, 0.0f, 0.0f, v[2]);
    };
    float deck0[3] = {0, 0, 0};
    aver_phys_body_position(deck, deck0);
    float deckX = deck0[0];
    // One frame: the deck driven 4 m/s along +X, both characters standing still, one fixed step.
    auto frame = [&]() {
        deckX += 400.0f * kDt;
        aver_phys_body_move_kinematic(deck, deckX, deck0[1], deck0[2], 0.0f, 0.0f, 0.0f, 1.0f, kDt);
        standStill(rider);
        standStill(bystander);
        aver_phys_step(kDt);
    };

    for (int i = 0; i < 60; ++i) {
        standStill(rider);
        standStill(bystander);
        aver_phys_step(kDt);
    }
    check(aver_phys_character_grounded(rider) == 1, "the rider has settled on the still deck");

    float rider0[3] = {0, 0, 0}, by0[3] = {0, 0, 0};
    aver_phys_character_position(rider, rider0);
    aver_phys_character_position(bystander, by0);
    for (int i = 0; i < 120; ++i) frame();
    float deck1[3] = {0, 0, 0}, rider1[3] = {0, 0, 0}, by1[3] = {0, 0, 0}, rv[3] = {9, 9, 9};
    aver_phys_body_position(deck, deck1);
    aver_phys_character_position(rider, rider1);
    aver_phys_character_position(bystander, by1);
    const f32 deckMoved = deck1[0] - deck0[0], riderMoved = rider1[0] - rider0[0];
    check(deckMoved > 700.0f, "the deck moved " + f2s(deckMoved) + " cm in two seconds");
    check(near(riderMoved, deckMoved, 15.0f),
          "and the rider standing on it moved WITH it, " + f2s(riderMoved) + " cm -- without the ground's "
          "velocity it stays where it was and the deck slides out from under it");
    check(rider1[2] > 280.0f, "still standing on the deck rather than fallen off it, z = " + f2s(rider1[2]));
    check(aver_phys_character_velocity(rider, rv) == 1 && std::fabs(rv[0]) < 1.0f && std::fabs(rv[1]) < 1.0f,
          "while the velocity a driver reads back stays its OWN, (" + f2s(rv[0]) + ", " + f2s(rv[1]) +
          ") -- read back and written again each frame, a carried velocity would compound");
    check(near(by1[0], by0[0], 1.0f) && near(by1[1], by0[1], 1.0f),
          "and a character on the still ground beside it did not move");
    float inh[3] = {0, 0, 0};
    check(aver_phys_character_inherited_velocity(rider, inh) == 1 && near(inh[0], 400.0f, 20.0f) &&
          near(inh[1], 0.0f, 1.0f),
          "what it inherits from the deck reads as the deck's own velocity, x = " + f2s(inh[0]) +
          " -- own + inherited is the world velocity");

    // A jump on a moving deck lands back where it left: the deck's motion is momentum in the air.
    // The jump goes in through the same frame as everything else: standStill reads the 465 back and
    // keeps it, exactly as a driver keeps the vertical velocity it did not set.
    const f32 offset0 = rider1[0] - deck1[0];
    aver_phys_character_set_velocity(rider, 0.0f, 0.0f, 465.0f);
    frame();
    bool leftGround = false;
    for (int i = 0; i < 90; ++i) {
        if (aver_phys_character_grounded(rider) == 0) leftGround = true;
        frame();
    }
    float deck2[3] = {0, 0, 0}, rider2[3] = {0, 0, 0};
    aver_phys_body_position(deck, deck2);
    aver_phys_character_position(rider, rider2);
    const f32 offset1 = rider2[0] - deck2[0];
    check(leftGround, "the jump left the deck");
    check(rider2[2] > 280.0f && near(offset1, offset0, 20.0f),
          "and landed back on it at the same spot, " + f2s(offset1 - offset0) + " cm from where it took off "
          "-- dropping the deck's speed at take-off would land it about 4 m behind, off the end");

    // Teleported onto still ground, it stops riding: no carried speed, no launch.
    aver_phys_character_set_position(rider, 0.0f, -1500.0f, 91.0f);
    for (int i = 0; i < 60; ++i) frame();
    float rider3[3] = {0, 0, 0};
    aver_phys_character_position(rider, rider3);
    check(near(rider3[0], 0.0f, 5.0f) && near(rider3[1], -1500.0f, 5.0f),
          "teleported off the deck onto still ground, it stays put at (" + f2s(rider3[0]) + ", " +
          f2s(rider3[1]) + ")");
    float inh3[3] = {9, 9, 9};
    check(aver_phys_character_inherited_velocity(rider, inh3) == 1 && near(inh3[0], 0.0f, 0.5f) &&
          near(inh3[1], 0.0f, 0.5f), "and inherits nothing from still ground");

    // The setter: what it is given reads back, and on ground the next step measures the ground again.
    check(aver_phys_character_set_inherited_velocity(rider, 0.0f, 250.0f, 0.0f) == 1, "inherited velocity is settable");
    aver_phys_character_inherited_velocity(rider, inh3);
    check(near(inh3[1], 250.0f, 0.01f), "and reads back as set, y = " + f2s(inh3[1]));
    frame();
    aver_phys_character_inherited_velocity(rider, inh3);
    check(near(inh3[1], 0.0f, 0.5f), "standing on still ground, one step later it is the ground's again: y = " + f2s(inh3[1]));
    aver_phys_shutdown();
}

// aver_phys_character_of_entity is aver_phys_set_entity read backwards. What the editor does with the
// answer is teleport that capsule, so the last check here is that the handle it got moves the right one.
static void testEntityToCharacter() {
    AVER_INFO("=== an entity finds the character stamped with it ===");
    const int32_t a = worldWithCharacterOnFloor();
    const int32_t b = aver_phys_character_create(30.0f, 180.0f, 500.0f, 0.0f, 90.0f);
    const int32_t crate = aver_phys_add_dynamic_box(0.0f, 500.0f, 50.0f, 50.0f, 50.0f, 50.0f, 10.0f);

    check(aver_phys_character_of_entity(41) == 0, "nothing is stamped yet, so entity 41 has no character");
    check(aver_phys_character_of_entity(0) == 0,
          "and entity 0 never names one, though every unstamped character carries exactly that stamp");

    aver_phys_set_entity(a, 41);
    aver_phys_set_entity(b, 42);
    aver_phys_set_entity(crate, 43);
    check(aver_phys_character_of_entity(41) == a, "entity 41 finds the first character");
    check(aver_phys_character_of_entity(42) == b, "and entity 42 the second, not whichever came first");
    check(aver_phys_character_of_entity(43) == 0, "a BODY stamped with an entity is not a character");

    aver_phys_character_set_position(aver_phys_character_of_entity(42), 500.0f, 800.0f, 90.0f);
    float pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
    aver_phys_character_position(a, pa);
    aver_phys_character_position(b, pb);
    check(near(pb[1], 800.0f, 1.0f) && near(pa[1], 0.0f, 1.0f),
          "teleporting through the looked-up handle moves that character and leaves the other, y = " +
          f2s(pb[1]) + " and " + f2s(pa[1]));

    aver_phys_set_entity(a, 0);
    check(aver_phys_character_of_entity(41) == 0, "clearing the stamp un-finds it");
    aver_phys_character_destroy(b);
    check(aver_phys_character_of_entity(42) == 0, "and so does destroying the character");
    aver_phys_shutdown();
    check(aver_phys_character_of_entity(42) == 0, "with no world at all the answer is still 0");
}

// The flying default pawn: gravity factor 0 hovers where it is put, still collides (a wall stops it),
// and 1 gives gravity back.
static void testGravityFactor() {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -50.0f, 4000.0f, 4000.0f, 50.0f);       // floor, top at z = 0
    aver_phys_add_static_box(300.0f, 0, 500.0f, 50.0f, 4000.0f, 1000.0f); // wall, face at x = 250
    const int32_t ch = aver_phys_character_create(35.0f, 90.0f, 0.0f, 0.0f, 500.0f);
    float f = -1.0f;
    check(aver_phys_character_gravity_factor(ch, &f) == 1 && f == 1.0f, "the default factor is 1");
    check(aver_phys_character_set_gravity_factor(ch, -1.0f) == 0, "a negative factor is refused");
    check(aver_phys_character_set_gravity_factor(ch, 0.0f) == 1, "0 is accepted");
    aver_phys_character_set_stair_stepping(ch, 0.0f, 0.0f);
    for (int i = 0; i < 120; ++i) { aver_phys_character_set_velocity(ch, 0, 0, 0); aver_phys_step(kDt); }
    float p[3] = {0, 0, 0};
    aver_phys_character_position(ch, p);
    check(near(p[2], 500.0f, 0.5f), "with factor 0 it hovers (z " + f2s(p[2]) + ", wants 500)");

    const f32 moved = walkForward(ch, 600.0f, 120);   // 12 m of travel asked for
    aver_phys_character_position(ch, p);
    check(moved > 150.0f && p[0] <= 250.0f - 35.0f + 1.0f,
          "flying into the wall it stops at its face (x " + f2s(p[0]) + ", face 250, radius 35)");
    check(near(p[2], 500.0f, 1.0f), "and still holds its height (z " + f2s(p[2]) + ")");

    aver_phys_character_set_gravity_factor(ch, 1.0f);
    for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
    aver_phys_character_position(ch, p);
    check(p[2] < 100.0f, "factor 1 brings gravity back: it falls (z " + f2s(p[2]) + ")");
    aver_phys_character_destroy(ch);
    check(aver_phys_character_gravity_factor(ch, &f) == 0, "a destroyed character has no factor");
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
    testRidesAMovingDeck();
    testEntityToCharacter();
    testGravityFactor();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
