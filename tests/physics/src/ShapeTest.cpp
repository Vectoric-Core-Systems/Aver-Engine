// ShapeTest -- the rigid shapes physics_shapes_abi.h adds: capsule, cylinder, tapered capsule, and a
// compound of boxes.
//
// A SHAPE'S SIZE IS ITS WHOLE CONTRACT, and a size test is not the formality it sounds like. Every one
// of these creators takes dimensions in the engine's centimetres, permutes them onto Jolt's axes and
// halves or doubles some of them; the resulting body still falls, still collides and still returns a
// handle whichever of those steps is wrong. So the assertions below are almost all of the same kind:
// DROP THE SHAPE ON A FLOOR AT A KNOWN HEIGHT AND ASK WHERE ITS CENTRE COMES TO REST. That number is
// the shape's half-height, measured rather than declared, and it is the only check that can tell a
// capsule built to the right size from one built to twice or half of it.
//
// THE CONVENTION THIS PINS DOWN. physics_shapes_abi.h says a capsule's `height` is TOTAL -- both caps
// included -- matching aver_phys_character_create rather than Jolt's own CapsuleShape, which takes the
// half-height of the cylindrical middle. That was a decision with a real alternative, so it is checked
// here rather than trusted: a 200 cm capsule must rest with its centre 100 cm up, not 200.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_shapes_abi.h"

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

// A floor whose TOP SURFACE is exactly z = 0, so a resting body's centre height IS its half-height.
static void floorAtZeroWithWorld() {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
}

// Drops `body` and returns where its centre settles. Long enough to come to rest from 300 cm, and the
// caller asserts the height rather than this helper, so a shape that never settles fails as a wrong
// number rather than as a hang.
static f32 restHeight(int32_t body) {
    for (int i = 0; i < 400; ++i) aver_phys_step(kDt);
    float p[3] = {0, 0, 0};
    aver_phys_body_position(body, p);
    return p[2];
}

// ---------------------------------------------------------------------------------------------------

static void testCapsuleHeightIsTotal() {
    AVER_INFO("=== a capsule's height is TOTAL, caps included ===");
    floorAtZeroWithWorld();

    // 200 cm tall, 40 cm radius: the cylinder in the middle is 200 - 2*40 = 120 cm, and the whole
    // thing rests with its centre 100 cm up.
    const int32_t cap = aver_phys_add_dynamic_capsule(0, 0, 300.0f, 40.0f, 200.0f, 10.0f);
    check(cap != 0, "a dynamic capsule is created");

    const f32 z = restHeight(cap);
    check(near(z, 100.0f, 6.0f),
          "a 200 cm capsule rests with its centre at 100 cm, got " + f2s(z) +
          " (200 would mean `height` was read as a HALF-height, 60 as Jolt's own cylinder half-height)");

    // A CAPSULE SHORTER THAN ITS OWN CAPS IS NOT A SHAPE. Total height 50 with radius 40 leaves a
    // cylinder of -30 cm; aver_phys_character_create refuses exactly this and so must these.
    check(aver_phys_add_dynamic_capsule(0, 0, 300.0f, 40.0f, 50.0f, 1.0f) == 0,
          "a capsule too short for its own radius is refused, not silently repaired");
    check(aver_phys_add_static_capsule(0, 0, 300.0f, 40.0f, 80.0f) == 0,
          "and exactly-two-radii is refused too -- that is a sphere, with no cylinder left");

    aver_phys_shutdown();
}

static void testStaticCapsuleHoldsThingsUp() {
    AVER_INFO("=== a static capsule is solid ===");
    aver_phys_init();
    // 100 cm radius and 400 cm TOTAL height, so the cylindrical middle is 400/2 - 100 = 100 cm and the
    // capsule's top is 200 cm up. Radius 100 with height 200 would leave NO cylinder at all -- that is
    // a sphere, and the creator refuses it, which the previous test asserts on purpose.
    check(aver_phys_add_static_capsule(0, 0, 0, 100.0f, 400.0f) != 0, "a static capsule exists");
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 600.0f, 20.0f, 1.0f);
    const f32 z = restHeight(ball);
    check(z > 150.0f, "a ball dropped on it does not fall through, resting at " + f2s(z));
    aver_phys_shutdown();
}

static void testCylinderHeightIsFull() {
    AVER_INFO("=== a cylinder's height is end to end ===");
    floorAtZeroWithWorld();

    // 160 cm tall: centre rests 80 cm up. A cylinder has no caps to add, so unlike the capsule there
    // is only one reading -- which is exactly why this is a separate check from the capsule's.
    const int32_t cyl = aver_phys_add_dynamic_cylinder(0, 0, 300.0f, 50.0f, 160.0f, 10.0f);
    check(cyl != 0, "a dynamic cylinder is created");
    const f32 z = restHeight(cyl);
    check(near(z, 80.0f, 6.0f),
          "a 160 cm cylinder rests with its centre at 80 cm, got " + f2s(z));

    check(aver_phys_add_dynamic_cylinder(0, 0, 300.0f, 50.0f, 0.0f, 1.0f) == 0,
          "a zero-height cylinder is refused");
    check(aver_phys_add_dynamic_cylinder(0, 0, 300.0f, 0.0f, 100.0f, 1.0f) == 0,
          "and a zero-radius one");

    aver_phys_shutdown();
}

// A TAPERED CAPSULE IS NOT SYMMETRIC, and that is the only thing worth testing about it beyond its
// height: which end is which. The header says topRadius is the +Z cap. Resting a heavily tapered one
// on a floor puts the FAT end down and the thin end up, so its centre sits nearer the fat end's radius
// than the thin one's -- which distinguishes the two orderings without needing the exact figure.
static void testTaperedCapsule() {
    AVER_INFO("=== a tapered capsule's height is total, and its ends differ ===");
    floorAtZeroWithWorld();

    const int32_t t = aver_phys_add_dynamic_tapered_capsule(0, 0, 300.0f, 15.0f, 60.0f, 200.0f, 10.0f);
    check(t != 0, "a dynamic tapered capsule is created");
    const f32 z = restHeight(t);
    check(near(z, 100.0f, 12.0f),
          "a 200 cm tapered capsule rests near 100 cm, got " + f2s(z));

    // Total 200 with caps of 60 and 150 leaves a negative middle.
    check(aver_phys_add_dynamic_tapered_capsule(0, 0, 300.0f, 150.0f, 60.0f, 200.0f, 1.0f) == 0,
          "one whose caps exceed its height is refused");

    aver_phys_shutdown();
}

// THE COMPOUND IS THE ONE THAT EARNS ITS PLACE MOST, because a chair really is one body and the ABI
// had no way to say so. Two checks matter: the parts are actually THERE at their offsets (not all
// collapsed onto the centre), and the whole thing behaves as ONE body rather than several.
static void testCompoundOfBoxes() {
    AVER_INFO("=== a compound of boxes is one body with parts in the right places ===");
    floorAtZeroWithWorld();

    // A table: a top at +40, and two legs reaching down to -40. Local offsets, so the body's own
    // origin is its centre and the shape spans -60..+60 in z.
    const float offsets[9] = {
        0.0f,   0.0f,  40.0f,     // the top
        -50.0f, 0.0f, -20.0f,     // a leg
        50.0f,  0.0f, -20.0f,     // and another
    };
    const float halves[9] = {
        60.0f, 60.0f, 20.0f,
        10.0f, 10.0f, 40.0f,
        10.0f, 10.0f, 40.0f,
    };
    // Dropped from just above its resting height rather than from 300: a 240 cm fall lands hard
    // enough to tip a table with a wide top on two narrow legs, and where a shape RESTS should not
    // also be a test of how it survives being thrown at the floor.
    const int32_t table = aver_phys_add_dynamic_compound_boxes(0, 0, 100.0f, offsets, halves, 3, 20.0f);
    check(table != 0, "a three-box compound is created");

    const f32 z = restHeight(table);
    // THE EXPECTED NUMBER IS 94, AND WORKING OUT WHY IS WHAT CAUGHT THE BUG UNDER THIS TEST.
    //
    // aver_phys_body_position reports a DYNAMIC body's centre of mass, not its shape origin, and a
    // compound's centre of mass is wherever its parts put it. Slab 120x120x40 at z = +40 is 576000
    // cm^3; each leg 20x20x80 at z = -20 is 32000. So
    //     com_z = (576000*40 + 2*32000*-20) / 640000 = +34
    // and the legs reach local z = -60, which is 94 below that. Resting, the reported height is 94.
    //
    // This test used to assert 60 -- and 60 is exactly what you get if all three parts are the LAST
    // box repeated: three identical legs at z = +40, -20, -20 put the centre of mass at 0, and 0 is
    // 60 above the lowest point. The number this test checked was therefore a fingerprint of the
    // defect it was meant to rule out. It only ever passed because the sub-shapes were pointers to a
    // destroyed stack local (PhysicsShapes.cpp), and the assert Jolt raises for that is compiled out
    // of the Release build this suite is normally run in.
    check(near(z, 94.0f, 3.0f),
          "its centre of mass rests at 94 cm, which is the slab-weighted value derived above -- got " +
          f2s(z) + " (60 would mean all three parts came out as copies of the last box)");

    // THE SLAB IS ACTUALLY A SLAB, which the rest height above cannot tell you.
    //
    // The top spans x in [-60, 60]; the legs sit at x = +/-50 with a half-extent of 10, so they cover
    // only [-60, -40] and [40, 60]. x = +30 is therefore covered by the TOP AND BY NOTHING ELSE, and a
    // ray dropped there either lands on the table or falls through it to the floor.
    //
    // This exists because the rest height did not distinguish the shapes. Every sub-shape used to be
    // handed to the compound as a pointer to a stack local that had already been destroyed, so all
    // three parts came out as copies of the last one -- three legs, no top. Three legs reach the same
    // lowest point as two legs and a slab, so `restHeight` was satisfied by the broken shape, and
    // asserts are compiled out of the Release build this suite is normally run in.
    {
        float hit[3] = {0, 0, 0}, n[3] = {0, 0, 0};
        int32_t ent = -1;
        // Straight down the +30 line from above the table, which is resting with its origin at z = 60.
        const int32_t h = aver_phys_raycast(30.0f, 0.0f, 400.0f, 0.0f, 0.0f, -1.0f, 1000.0f, hit, n, &ent);
        check(h == table,
              "a ray down x = +30 hits the TABLE, not the floor past it -- only the top slab covers "
              "that line, so hitting the floor means the slab was built as another leg");
        check(h != table || hit[2] > 40.0f,
              "and it lands on the slab's upper face near z = 120, got " + f2s(hit[2]));
    }

    // ONE BODY, NOT THREE. Every part shares a single handle, so the body count rose by exactly one
    // over the floor that was already there.
    check(aver_phys_body_count() == 2,
          "and the world holds 2 bodies -- the floor and this one -- not 4, got " +
          std::to_string(aver_phys_body_count()));

    check(aver_phys_add_dynamic_compound_boxes(0, 0, 0, offsets, halves, 0, 1.0f) == 0,
          "a compound of zero boxes is refused");
    check(aver_phys_add_dynamic_compound_boxes(0, 0, 0, nullptr, halves, 3, 1.0f) == 0,
          "and a null offsets array");

    aver_phys_shutdown();
}

// The new shapes must be ordinary bodies in every other respect -- the point of routing them through
// the module's own addBody rather than a second creation path. If any of these fails, the shape was
// built but not registered the way every other body is.
static void testNewShapesAreOrdinaryBodies() {
    AVER_INFO("=== a capsule body is a body like any other ===");
    aver_phys_set_gravity(0, 0, 0);
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);

    const int32_t cap = aver_phys_add_dynamic_capsule(0, 0, 0, 30.0f, 200.0f, 5.0f);
    check(aver_phys_set_entity(cap, 4242) == 1, "it takes an entity stamp");
    check(aver_phys_body_add_impulse(cap, 500.0f, 0, 0) == 1, "and an impulse");
    float v[3] = {0, 0, 0};
    aver_phys_body_velocity(cap, v);
    check(near(v[0], 100.0f, 1.0f), "which moves it by impulse/mass, got " + f2s(v[0]));

    float outPoint[3], outNormal[3];
    int32_t outEntity = 0;
    const int32_t hit = aver_phys_raycast(-400.0f, 0, 0, 1, 0, 0, 1000.0f,
                                          outPoint, outNormal, &outEntity);
    check(hit == cap, "a raycast finds it");
    check(outEntity == 4242, "and reports the entity stamped on it, got " + std::to_string(outEntity));

    check(aver_phys_remove_body(cap) == 1, "and it can be removed like any other body");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("rigid shapes: capsule, cylinder, tapered capsule, compound");

    testCapsuleHeightIsTotal();
    testStaticCapsuleHoldsThingsUp();
    testCylinderHeightIsFull();
    testTaperedCapsule();
    testCompoundOfBoxes();
    testNewShapesAreOrdinaryBodies();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
