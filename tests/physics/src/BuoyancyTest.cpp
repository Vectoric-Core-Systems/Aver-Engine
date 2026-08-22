// Buoyancy through the plain-C ABI, and NOTHING ELSE -- the global water plane, a per-body override,
// and the one guarantee a static body gets: none of it moves it.
//
// LINKS Aver.Physics + Aver.Core ONLY, exactly like SoftBodyTest.cpp beside it, for the same reason:
// everything checked here is visible to a caller of the ABI, so it is checked the way a caller would
// see it -- water level, buoyancy, drag -- never by reaching for a JPH:: type this module keeps
// private to itself.
#include "aver/physics/physics_abi.h"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Advances by whole fixed steps, same helper shape as SoftBodyTest.cpp's step().
static void step(float seconds) {
    const float fixed = aver_phys_fixed_step();
    for (float t = 0.0f; t < seconds; t += fixed) aver_phys_step(fixed);
}

static float bodyZ(int32_t body) {
    float xyz[3] = {0, 0, 0};
    aver_phys_body_position(body, xyz);
    return xyz[2];
}

// ---- a buoyant body sinks into the water, then floats back up --------------------------------------
// Does NOT assert an exact resting height: Jolt's drag/impulse constants (BodyInterface.h:241-242,
// its own suggested defaults, used here) were not derived analytically for this slice, so the only
// claim that can be made without guessing a number is the qualitative one -- it arrests its fall and
// comes back up, rather than sinking through the surface as if the water were not there at all.
static void testFloatingBodyRisesBackUp() {
    AVER_INFO("-- a buoyancy > 1 body floats back up after it sinks in --");
    check(aver_phys_init() == 1, "physics started");
    aver_phys_set_gravity(0, 0, -981);   // engine convention: cm/s^2, +Z up, one g

    // A 1m cube, mass derived from its volume, starting 10m above a water plane at z = 0.
    const int32_t body = aver_phys_add_dynamic_box(0, 0, 1000, 50, 50, 50, 0.0f);
    check(body != 0, "the box was created");

    const float surfacePos[3] = {0, 0, 0};
    const float normal[3] = {0, 0, 1};
    const float current[3] = {0, 0, 0};
    const int32_t set = aver_phys_set_water_volume(body, surfacePos, normal,
                                                   1.4f, 0.5f, 0.01f, current);
    check(set == 1, "the water volume registered");

    step(2.0f);
    const float zMid = bodyZ(body);   // falling, likely already at or past the surface

    step(2.5f);
    const float zLate = bodyZ(body);   // enough time for buoyancy to arrest and reverse the descent

    check(zLate > zMid, "it rose back up after sinking in (mid " + std::to_string(zMid) +
                             "cm, late " + std::to_string(zLate) + "cm)");

    aver_phys_shutdown();
}

// ---- a buoyancy < 1 body sinks, undeterred -----------------------------------------------------
// Compares the SAME body, SAME step count, WITH a water volume registered against WITHOUT one at
// all, across two clean simulations -- per the plan's own risk note, this is the version of the
// check that does not require trusting a specific descent rate, only that a body documented to sink
// (Body.h: "< 1 sinks") is not, in practice, quietly floating.
static void testSinkingBodyIsNotArrested() {
    AVER_INFO("-- a buoyancy < 1 body keeps sinking, not arrested by drag --");

    check(aver_phys_init() == 1, "physics started");
    aver_phys_set_gravity(0, 0, -981);
    const int32_t withWater = aver_phys_add_dynamic_box(0, 0, 1000, 50, 50, 50, 0.0f);
    check(withWater != 0, "the box was created (with water)");
    const float surfacePos[3] = {0, 0, 0};
    const float normal[3] = {0, 0, 1};
    const float current[3] = {0, 0, 0};
    const int32_t set = aver_phys_set_water_volume(withWater, surfacePos, normal,
                                                    0.5f, 0.5f, 0.01f, current);
    check(set == 1, "the water volume registered");
    const float startWithWater = bodyZ(withWater);
    // Sampled ACROSS the run, not after it: aver_phys_buoyant_body_count reports the LAST step's
    // figure, and after shutdown there is no world left to report anything at all.
    int32_t buoyantDuringSink = 0;
    for (int i = 0; i < 270; ++i) {
        aver_phys_step(aver_phys_fixed_step());
        buoyantDuringSink += aver_phys_buoyant_body_count();
    }
    const float dropWithWater = startWithWater - bodyZ(withWater);
    aver_phys_shutdown();

    check(aver_phys_init() == 1, "physics restarted clean");
    aver_phys_set_gravity(0, 0, -981);
    const int32_t withoutWater = aver_phys_add_dynamic_box(0, 0, 1000, 50, 50, 50, 0.0f);
    check(withoutWater != 0, "the box was created (no water registered at all)");
    const float startWithoutWater = bodyZ(withoutWater);
    // EXACTLY the same step count as the run above. step(4.5f) in both would have been close enough
    // to look right and not close enough to be a comparison.
    for (int i = 0; i < 270; ++i) aver_phys_step(aver_phys_fixed_step());
    const float dropWithoutWater = startWithoutWater - bodyZ(withoutWater);
    aver_phys_shutdown();

    check(dropWithWater > 0.0f, "it fell at all (" + std::to_string(dropWithWater) + "cm)");

    // THE ASSERTION THAT USED TO BE HERE COULD NOT FAIL. It was
    //     check(dropWithWater > dropWithoutWater * 0.5f, ...)
    // and the two runs came back BIT-IDENTICAL (3202.577148cm each) while it passed, because any
    // positive drop satisfies "more than half of itself". It reported green in exactly the state it
    // existed to catch: buoyancy having no effect whatsoever.
    //
    // What replaces it is two checks that discriminate in opposite directions, because either one
    // alone is still satisfiable by a broken implementation:
    //
    //   (1) the impulse was really applied -- a count, straight from the simulation, so "nothing
    //       floats" and "nothing is in the water" can no longer look the same from out here;
    //   (2) the drag really slowed it -- STRICTLY, because linearDrag 0.5 over hundreds of
    //       submerged substeps cannot leave the descent bit-identical to an unimpeded one.
    check(buoyantDuringSink > 0,
          "the sinking body really was given buoyancy impulses (" +
              std::to_string(buoyantDuringSink) + " body-steps)");
    check(dropWithWater < dropWithoutWater,
          "drag slowed the descent (with water " + std::to_string(dropWithWater) +
              "cm vs without " + std::to_string(dropWithoutWater) + "cm)");
    // ...but did not arrest it: a body Jolt documents as sinking (buoyancy < 1) must still be going
    // down, unlike the buoyancy-1.4 box in the test above, which reversed.
    check(dropWithWater > dropWithoutWater * 0.25f,
          "and did not arrest it (" + std::to_string(dropWithWater) + "cm of "
              + std::to_string(dropWithoutWater) + "cm unimpeded)");
}

// ---- a static body is completely unmoved by buoyancy ------------------------------------------------
// Documents Jolt's own IsDynamic() gate (BodyInterface::ApplyBuoyancyImpulse silently returns false
// for a non-dynamic body) rather than assuming it -- see Buoyancy.hpp for why evaluate() itself is
// deliberately ungated and leaves this check to Jolt.
static void testStaticBodyIsUnmoved() {
    AVER_INFO("-- buoyancy on a static body is a no-op --");
    check(aver_phys_init() == 1, "physics started");
    aver_phys_set_gravity(0, 0, -981);

    const int32_t body = aver_phys_add_static_box(0, 0, 1000, 50, 50, 50);
    check(body != 0, "the static box was created");
    const float surfacePos[3] = {0, 0, 0};
    const float normal[3] = {0, 0, 1};
    const float current[3] = {0, 0, 0};
    const int32_t set = aver_phys_set_water_volume(body, surfacePos, normal,
                                                    1.4f, 0.5f, 0.01f, current);
    check(set == 1, "the water volume registered on it anyway -- registration does not know or care");

    const float before = bodyZ(body);
    step(3.0f);
    const float after = bodyZ(body);

    check(std::abs(after - before) < 1e-3f,
          "it did not move at all (before " + std::to_string(before) +
              "cm, after " + std::to_string(after) + "cm)");

    aver_phys_shutdown();
}

int main() {
    testFloatingBodyRisesBackUp();
    testSinkingBodyIsNotArrested();
    testStaticBodyIsUnmoved();

    AVER_INFO("BuoyancyTest: {}/{} checks passed", g_checks - g_failures, g_checks);
    return g_failures;
}
