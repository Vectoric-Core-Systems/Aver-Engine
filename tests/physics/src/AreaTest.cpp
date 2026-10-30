// AreaTest -- areas on Aver.Physics (physics_areas_abi.h).
//
// Detection is the area's own query, so the claims worth testing are the ones Jolt's sensor events cannot make:
//   1. a world with areas that do nothing simulates bit-identically; refusals and defaults,
//   2. enter and exit per (area, body), one per body however many sub-shapes overlap, and STATIC and SLEEPING bodies are seen,
//   3. layer and mask, monitorable, area-vs-area, and the monitoring switch,
//   4. gravity: against a world with the same gravity, combine / replace by priority, point gravity, the body's gravity factor,
//   5. damping with the same modes, restored on leaving,
//   6. wind against the same force applied by hand, with attenuation,
//   7. a sleeper wakes when an area starts pushing it; an area that moves or is removed lets go,
//   8. a snapshot restore replays an area-driven run exactly.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_areas_abi.h"
#include "aver/physics/physics_body_ext_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_shapes_abi.h"
#include "aver/physics/physics_state_abi.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
static const float kDt = 1.0f / 60.0f;
static const int32_t OV_COMBINE = AVER_PHYS_AREA_OVERRIDE_COMBINE, OV_COMBINE_REPLACE = AVER_PHYS_AREA_OVERRIDE_COMBINE_REPLACE,
                     OV_REPLACE = AVER_PHYS_AREA_OVERRIDE_REPLACE, OV_REPLACE_COMBINE = AVER_PHYS_AREA_OVERRIDE_REPLACE_COMBINE,
                     OV_OFF = AVER_PHYS_AREA_OVERRIDE_DISABLED;

struct AEv { int32_t area, other, entered, isArea; };

struct Log {
    std::vector<AEv> ev;
    void step(int n) {
        for (int i = 0; i < n; ++i) {
            aver_phys_step(kDt);
            const int32_t c = aver_phys_area_event_count();
            for (int32_t k = 0; k < c; ++k) {
                AEv e{};
                aver_phys_area_event_get(k, &e.area, &e.other, &e.entered, &e.isArea);
                ev.push_back(e);
            }
        }
    }
    int count(int32_t area, int32_t other, int entered) const {
        int n = 0;
        for (const AEv& e : ev) if (e.area == area && e.other == other && e.entered == entered) ++n;
        return n;
    }
    void clear() { ev.clear(); }
};

static int32_t floorBox() { return aver_phys_add_static_box(0, 0, -50.0f, 4000.0f, 4000.0f, 50.0f); }   // top at z = 0
static float posOf(int32_t b, int axis) { float p[3] = {0, 0, 0}; aver_phys_body_position(b, p); return p[axis]; }

// A big sensor box centred on (0,0,zc), made an area.
static int32_t bigArea(float zc = 0.0f, float half = 3000.0f) {
    const int32_t s = aver_phys_add_sensor_box(0, 0, zc, half, half, half);
    aver_phys_area_create(s);
    return s;
}

static bool grav(int32_t body, float& x, float& y, float& z) {
    float g[3] = {0, 0, 0};
    const bool ok = aver_phys_body_area_gravity(body, g) == 1;
    x = g[0]; y = g[1]; z = g[2];
    return ok;
}

// ---------------------------------------------------------------------------------------------------

// The same stack with a sensor in the world either way; `asArea` registers it, `far` puts it where it holds nothing (and gives it overrides).
static std::vector<float> runStack(bool far, bool asArea) {
    aver_phys_init();
    floorBox();
    const int32_t a = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    const int32_t b = aver_phys_add_dynamic_box(10.0f, 5.0f, 260.0f, 50, 50, 50, 10.0f);
    const int32_t s = far ? aver_phys_add_sensor_box(90000.0f, 0, 0, 100, 100, 100) : aver_phys_add_sensor_box(0, 0, 0, 3000, 3000, 3000);
    if (asArea) {
        aver_phys_area_create(s);
        if (far) {
            aver_phys_area_set_gravity(s, OV_REPLACE, 0.0f, 0, 0, 1);
            aver_phys_area_set_damping(s, OV_REPLACE, 9.0f, OV_REPLACE, 9.0f);
            aver_phys_area_set_wind(s, 5000.0f, 1, 0, 0, 0, 0);
        }
    }
    for (int i = 0; i < 300; ++i) aver_phys_step(kDt);
    std::vector<float> out;
    for (int32_t h : {a, b}) { float p[3]; aver_phys_body_position(h, p); out.insert(out.end(), p, p + 3); }
    aver_phys_shutdown();
    return out;
}

static bool sameBits(const std::vector<float>& x, const std::vector<float>& y) {
    return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0;
}

static void testUnchangedAndRefusals() {
    AVER_INFO("=== areas that do nothing change nothing; refusals ===");
    check(sameBits(runStack(false, false), runStack(false, true)), "a monitoring area around everything, no overrides: every position is bit-identical to a plain sensor");
    check(sameBits(runStack(true, false), runStack(true, true)), "an area with overrides that holds no body: bit-identical too");

    aver_phys_init();
    const int32_t solid = aver_phys_add_dynamic_box(0, 0, 100.0f, 10, 10, 10, 1.0f);
    const int32_t s = aver_phys_add_sensor_box(0, 0, 0, 100, 100, 100);
    check(aver_phys_area_create(solid) == 0, "a solid body cannot become an area");
    check(aver_phys_area_create(999999) == 0, "a dead handle is refused");
    check(aver_phys_area_exists(s) == 0 && aver_phys_area_count() == 0, "a sensor is not an area until registered");
    check(aver_phys_area_create(s) == 1 && aver_phys_area_create(s) == 1 && aver_phys_area_count() == 1, "registering twice is one area");
    int32_t mon = 0, mab = 0;
    check(aver_phys_area_flags(s, &mon, &mab) == 1 && mon == 1 && mab == 1, "monitoring and monitorable default to on");
    uint32_t layer = 99, mask = 0;
    check(aver_phys_area_collision(s, &layer, &mask) == 1 && layer == 1u && mask == 0xFFFFFFFFu, "layer defaults to the sensor's user-layer bit, mask to every bit");
    check(aver_phys_area_set_gravity(s, 17, 1.0f, 0, 0, 1) == 0, "an unknown mode is refused");
    check(aver_phys_area_set_gravity(s, OV_REPLACE, NAN, 0, 0, 1) == 0, "a NaN is refused");
    check(aver_phys_area_set_gravity_point(s, 1, 0, 0, 0, -1.0f) == 0, "a negative unit distance is refused");
    check(aver_phys_area_set_flags(solid, 1, 1) == 0 && aver_phys_area_overlap_count(solid) == 0, "calls on a non-area are refused");
    check(aver_phys_body_area_gravity(solid, nullptr) == 0, "a null out pointer is refused");
    check(aver_phys_area_destroy(s) == 1 && aver_phys_area_exists(s) == 0, "destroy turns it back into a plain sensor");
    check(aver_phys_area_destroy(s) == 0, "and a second destroy is refused");
    aver_phys_shutdown();
}

static void testEnterExit() {
    AVER_INFO("=== enter and exit, once per body; static and sleeping bodies are found ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t area = aver_phys_add_sensor_box(0, 0, 300.0f, 200.0f, 200.0f, 100.0f);   // z 200..400
    aver_phys_area_create(area);
    const float offs[6] = {-40.0f, 0, 0, 40.0f, 0, 0};
    const float half[6] = {30.0f, 30.0f, 30.0f, 30.0f, 30.0f, 30.0f};
    const int32_t two = aver_phys_add_dynamic_compound_boxes(0, 0, 700.0f, offs, half, 2, 10.0f);   // two sub-shapes
    Log lg;
    lg.step(240);
    check(lg.count(area, two, 1) == 1, "a two-box compound falling through: one enter, not two");
    check(lg.count(area, two, 0) == 1, "and one exit");
    check(aver_phys_area_overlap_count(area) == 0, "nothing is left inside afterwards");

    // A static body and a sleeping one, with the area moved over them.
    const int32_t wall = aver_phys_add_static_box(1000.0f, 0, 100.0f, 50, 50, 100);   // z 0..200
    const int32_t sleeper = aver_phys_add_dynamic_box(-1000.0f, 0, 60.0f, 50, 50, 50, 10.0f);
    lg.step(300);
    check(aver_phys_body_is_active(sleeper) == 0, "the box on the floor fell asleep");
    lg.clear();
    aver_phys_body_set_position(area, 1000.0f, 0, 90.0f);   // z -10..190: overlaps the wall and sinks 10 cm into the floor
    lg.step(2);
    check(lg.count(area, wall, 1) == 1, "the area finds the STATIC wall it was moved onto");
    check(lg.count(area, fl, 1) == 1, "and the static floor it overlaps");
    aver_phys_body_set_position(area, -1000.0f, 0, 60.0f);
    lg.step(2);
    check(lg.count(area, wall, 0) == 1, "moving away exits the wall");
    check(lg.count(area, sleeper, 1) == 1, "the area finds the SLEEPING box");
    check(aver_phys_body_is_active(sleeper) == 0, "and did not wake it");
    int32_t other = 0, isArea = 1;
    bool listed = false;
    for (int32_t i = 0; i < aver_phys_area_overlap_count(area); ++i)
        if (aver_phys_area_overlap_get(area, i, &other, &isArea) == 1 && other == sleeper && isArea == 0) listed = true;
    check(listed, "the overlap list names it");

    lg.clear();
    aver_phys_remove_body(sleeper);
    lg.step(2);
    check(lg.count(area, sleeper, 0) == 1, "removing a body inside exits it");
    aver_phys_shutdown();
}

static void testMasksAndAreas() {
    AVER_INFO("=== layer and mask, monitorable, area-vs-area, monitoring ===");
    aver_phys_init();
    const int32_t a = bigArea(0.0f, 500.0f);
    const int32_t box = aver_phys_add_static_box(0, 0, 0, 50, 50, 50);
    Log lg;
    aver_phys_area_set_collision(a, 1u, 2u);
    aver_phys_body_set_collision_filter(box, 1u, 1u);
    lg.step(2);
    check(lg.count(a, box, 1) == 0, "area mask 2 against body layer 1: not detected");
    aver_phys_body_set_collision_filter(box, 2u, 0u);
    lg.step(2);
    check(lg.count(a, box, 1) == 1, "body layer 2 meets area mask 2: detected, whatever the body's own mask is");
    aver_phys_body_clear_collision_filter(box);
    lg.clear();
    lg.step(2);
    check(lg.count(a, box, 0) == 1, "back to the user-layer bit (1): mask 2 misses it again, an exit");

    aver_phys_area_set_collision(a, 1u, 0xFFFFFFFFu);
    lg.clear();
    const int32_t b = aver_phys_add_sensor_box(0, 0, 0, 100, 100, 100);
    aver_phys_area_create(b);
    aver_phys_area_set_flags(b, 0, 0);   // not monitoring, not monitorable
    lg.step(2);
    check(lg.count(a, b, 1) == 0, "an area that is not monitorable is invisible to other areas");
    aver_phys_area_set_flags(b, 0, 1);
    lg.step(2);
    check(lg.count(a, b, 1) == 1, "monitorable: A reports B");
    bool isAreaFlag = false;
    for (const AEv& e : lg.ev) if (e.area == a && e.other == b && e.entered) isAreaFlag = e.isArea == 1;
    check(isAreaFlag, "flagged as an area");
    check(lg.count(b, a, 1) == 0, "B is not monitoring, so it reports nothing");
    aver_phys_area_set_flags(b, 1, 1);
    lg.step(2);
    check(lg.count(b, a, 1) == 1, "monitoring on: B reports A as well");
    aver_phys_area_set_collision(b, 4u, 4u);   // B now has layer 4: A's mask (all) still sees it; B's mask 4 misses A (layer 1)
    lg.clear();
    lg.step(2);
    check(lg.count(b, a, 0) == 1 && lg.count(a, b, 0) == 0, "B's mask 4 stops seeing A; A still sees B");

    lg.clear();
    aver_phys_area_set_flags(a, 0, 1);
    lg.step(2);
    check(lg.count(a, box, 0) == 1 && lg.count(a, b, 0) == 1, "monitoring off ends everything it reported");
    check(aver_phys_area_overlap_count(a) == 0, "and the list is empty");
    lg.clear();
    aver_phys_area_set_flags(a, 1, 1);
    lg.step(2);
    check(lg.count(a, box, 1) == 1 && lg.count(a, b, 1) == 1, "monitoring back on: the enters come again");
    aver_phys_shutdown();
}

static void testGravity() {
    AVER_INFO("=== gravity ===");
    // Against a world with the same gravity: the force route equals changing gravity.
    float zWorld = 0, zArea = 0, zCombine = 0, zHalf = 0, zHalfWorld = 0;
    {
        aver_phys_init();
        aver_phys_set_gravity(0, 0, 500.0f);
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);
        for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
        zWorld = posOf(box, 2);
        aver_phys_shutdown();
    }
    {
        aver_phys_init();
        const int32_t area = bigArea();
        aver_phys_area_set_gravity(area, OV_REPLACE, 500.0f, 0, 0, 1);
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);
        for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
        zArea = posOf(box, 2);
        aver_phys_shutdown();
    }
    check(zWorld > 1200.0f && near(zArea, zWorld, 0.5f), "REPLACE +500 up equals world gravity +500: z " + f2s(zArea) + " against " + f2s(zWorld));

    float G = 0;
    {
        aver_phys_init();
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);
        float gx, gy;
        grav(box, gx, gy, G);
        check(near(G, -980.0f, 0.5f), "outside every area the total is the world's: " + f2s(G));
        aver_phys_shutdown();
    }
    {
        aver_phys_init();
        aver_phys_set_gravity(0, 0, G + 580.0f);
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);
        for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
        zHalfWorld = posOf(box, 2);
        aver_phys_shutdown();
    }
    {
        aver_phys_init();
        const int32_t area = bigArea();
        aver_phys_area_set_gravity(area, OV_COMBINE, 580.0f, 0, 0, 1);
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);
        for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
        zCombine = posOf(box, 2);
        aver_phys_shutdown();
    }
    check(zHalfWorld < 700.0f && near(zCombine, zHalfWorld, 0.5f), "COMBINE +580 on the world's gravity equals a world with that sum: " + f2s(zCombine) + " against " + f2s(zHalfWorld));

    {   // the body's own gravity factor still scales the result
        aver_phys_init();
        aver_phys_set_gravity(0, 0, 500.0f);
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);
        aver_phys_body_set_gravity_factor(box, 0.5f);
        for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
        zHalfWorld = posOf(box, 2);
        aver_phys_shutdown();
        aver_phys_init();
        const int32_t area = bigArea();
        aver_phys_area_set_gravity(area, OV_REPLACE, 500.0f, 0, 0, 1);
        const int32_t b2 = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);
        aver_phys_body_set_gravity_factor(b2, 0.5f);
        for (int i = 0; i < 120; ++i) aver_phys_step(kDt);
        zHalf = posOf(b2, 2);
        aver_phys_shutdown();
    }
    check(near(zHalf, zHalfWorld, 0.5f), "a gravity factor of 0.5 halves the area's gravity too: " + f2s(zHalf) + " against " + f2s(zHalfWorld));

    // Combining by priority, read through the total-gravity query (no stepping).
    aver_phys_init();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 0, 20, 20, 20, 10.0f);
    const int32_t a1 = bigArea(), a2 = bigArea();
    float gx = 0, gy = 0, gz = 0;
    aver_phys_area_set_priority(a1, 2);
    aver_phys_area_set_priority(a2, 1);
    aver_phys_area_set_gravity(a1, OV_COMBINE, 50.0f, 0, 0, 1);
    aver_phys_area_set_gravity(a2, OV_COMBINE_REPLACE, 100.0f, 0, 0, 1);
    grav(box, gx, gy, gz);
    check(near(gz, 150.0f, 0.1f) && near(gx, 0.0f, 0.1f), "COMBINE(+50, prio 2) then COMBINE_REPLACE(+100, prio 1): 150, the world ignored: " + f2s(gz));
    aver_phys_area_set_gravity(a1, OV_REPLACE_COMBINE, 80.0f, 0, 0, 1);
    aver_phys_area_set_gravity(a2, OV_COMBINE, 20.0f, 0, 0, 1);
    grav(box, gx, gy, gz);
    check(near(gz, 100.0f + G, 0.2f),"REPLACE_COMBINE(+80) then COMBINE(+20) then the world: " + f2s(gz));
    aver_phys_area_set_gravity(a1, OV_COMBINE, 50.0f, 0, 0, 1);
    aver_phys_area_set_gravity(a2, OV_REPLACE, 100.0f, 0, 0, 1);
    grav(box, gx, gy, gz);
    check(near(gz, 100.0f, 0.1f), "COMBINE(+50, prio 2) then REPLACE(+100, prio 1): REPLACE drops what was gathered, 100: " + f2s(gz));
    aver_phys_area_set_priority(a2, 3);
    grav(box, gx, gy, gz);
    check(near(gz, 100.0f, 0.1f), "REPLACE at the higher priority stops the walk: 100");
    aver_phys_area_set_priority(a2, 2);   // a tie: the lower handle goes first
    aver_phys_area_set_gravity(a1, OV_COMBINE_REPLACE, 30.0f, 0, 0, 1);
    aver_phys_area_set_gravity(a2, OV_COMBINE_REPLACE, 60.0f, 0, 0, 1);
    grav(box, gx, gy, gz);
    check(near(gz, 30.0f, 0.1f), "a priority tie goes to the lower handle: 30");
    aver_phys_area_set_gravity(a1, OV_OFF, 30.0f, 0, 0, 1);
    aver_phys_area_set_gravity(a2, OV_OFF, 60.0f, 0, 0, 1);
    grav(box, gx, gy, gz);
    check(near(gz, G, 0.5f), "DISABLED areas change nothing: " + f2s(gz));

    // Point gravity.
    aver_phys_area_set_gravity(a1, OV_REPLACE, 981.0f, 0, 0, 1);
    aver_phys_area_set_gravity_point(a1, 1, 0, 0, 0, 100.0f);
    aver_phys_area_set_priority(a1, 9);
    aver_phys_body_set_position(box, 300.0f, 0, 0);
    grav(box, gx, gy, gz);
    check(near(gx, -981.0f / 9.0f, 0.2f) && near(gy, 0, 0.1f) && near(gz, 0, 0.1f), "point gravity at 3 unit distances: 981/9 toward the centre: " + f2s(gx));
    aver_phys_body_set_position(box, 100.0f, 0, 0);
    grav(box, gx, gy, gz);
    check(near(gx, -981.0f, 0.5f), "at the unit distance the strength is the magnitude: " + f2s(gx));
    aver_phys_area_set_gravity_point(a1, 1, 0, 0, 0, 0.0f);
    aver_phys_body_set_position(box, 0, -500.0f, 0);
    grav(box, gx, gy, gz);
    check(near(gy, 981.0f, 0.5f) && near(gx, 0, 0.1f), "unit distance 0: constant strength, toward the centre (+y from y = -500): " + f2s(gy));
    aver_phys_area_set_gravity_point(a1, 1, 0, 200.0f, 0, 0.0f);   // the centre is 200 cm to the area's right
    aver_phys_body_set_position(box, 0, 0, 0);
    grav(box, gx, gy, gz);
    check(near(gy, 981.0f, 0.5f), "the point is in the area's local space: (0,200,0) from the origin pulls +y");
    aver_phys_shutdown();
}

static void testGravityDynamics() {
    AVER_INFO("=== point gravity pulls; leaving the area lets go ===");
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);
    const int32_t area = bigArea();
    aver_phys_area_set_gravity(area, OV_REPLACE, 400.0f, 0, 0, 1);
    aver_phys_area_set_gravity_point(area, 1, 0, 0, 0, 0.0f);
    const int32_t box = aver_phys_add_dynamic_box(600.0f, 0, 0, 20, 20, 20, 10.0f);
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    const float x1 = posOf(box, 0);
    check(x1 < 560.0f, "the box fell toward the centre of the area, x = " + f2s(x1));
    aver_phys_body_set_position(area, 90000.0f, 0, 0);   // the area leaves
    float gx, gy, gz;
    grav(box, gx, gy, gz);
    check(near(gz, 0.0f, 0.1f) && near(gx, 0.0f, 0.1f), "an area moved away no longer holds it: the total is the world's (zero here)");
    aver_phys_shutdown();
}

static void testDamping() {
    AVER_INFO("=== damping ===");
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 0, 20, 20, 20, 10.0f);
    aver_phys_body_set_damping(box, 0.25f, 0.5f);
    const int32_t a1 = bigArea(), a2 = bigArea();
    float lin = 0, ang = 0;
    aver_phys_area_set_priority(a1, 2);
    aver_phys_area_set_priority(a2, 1);
    aver_phys_area_set_damping(a1, OV_COMBINE, 3.0f, OV_OFF, 0.0f);
    aver_phys_body_area_damping(box, &lin, &ang);
    check(near(lin, 3.25f, 0.001f) && near(ang, 0.5f, 0.001f), "COMBINE +3 on the body's own 0.25 (angular untouched): " + f2s(lin) + ", " + f2s(ang));
    aver_phys_area_set_damping(a2, OV_REPLACE, 7.0f, OV_REPLACE, 9.0f);
    aver_phys_body_area_damping(box, &lin, &ang);
    check(near(lin, 7.0f, 0.001f) && near(ang, 9.0f, 0.001f), "REPLACE below it drops what was gathered: 7 and 9: " + f2s(lin) + ", " + f2s(ang));
    aver_phys_area_set_damping(a1, OV_COMBINE_REPLACE, 3.0f, OV_OFF, 0.0f);
    aver_phys_body_area_damping(box, &lin, &ang);
    check(near(lin, 3.0f, 0.001f) && near(ang, 9.0f, 0.001f), "COMBINE_REPLACE above it stops the linear walk at 3 (the body's own is ignored)");

    aver_phys_area_set_damping(a1, OV_OFF, 0.0f, OV_OFF, 0.0f);
    aver_phys_area_set_damping(a2, OV_REPLACE, 6.0f, OV_COMBINE, 2.0f);
    aver_phys_body_set_velocity(box, 1000.0f, 0, 0);
    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);
    float l = 0, a = 0;
    aver_phys_body_damping(box, &l, &a);
    check(near(l, 6.0f, 0.001f) && near(a, 2.5f, 0.001f), "inside, Jolt's damping is rewritten each step: " + f2s(l) + ", " + f2s(a));
    float v[3];
    aver_phys_body_velocity(box, v);
    check(v[0] < 1000.0f * std::pow(1.0f - 6.0f * kDt, 25.0f) * 1.2f + 1.0f, "and the body slows accordingly, vx = " + f2s(v[0]));

    aver_phys_body_set_position(box, 90000.0f, 0, 0);
    aver_phys_step(kDt);
    aver_phys_body_damping(box, &l, &a);
    check(near(l, 0.25f, 0.001f) && near(a, 0.5f, 0.001f), "after leaving, its own damping is back: " + f2s(l) + ", " + f2s(a));
    aver_phys_shutdown();
}

static void testWind() {
    AVER_INFO("=== wind ===");
    float xArea = 0, xHand = 0, xFar = 0, xFarHand = 0;
    for (int variant = 0; variant < 2; ++variant) {
        aver_phys_init();
        aver_phys_set_gravity(0, 0, 0);
        int32_t area = 0;
        if (variant == 0) { area = bigArea(); aver_phys_area_set_wind(area, 10000.0f, 1, 0, 0, 0.0f, 0.0f); }
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 0, 20, 20, 20, 10.0f);
        for (int i = 0; i < 60; ++i) {
            if (variant == 1) aver_phys_body_add_force(box, 10000.0f, 0, 0);
            aver_phys_step(kDt);
        }
        (variant == 0 ? xArea : xHand) = posOf(box, 0);
        aver_phys_shutdown();
    }
    check(xHand > 100.0f && near(xArea, xHand, 0.5f), "wind of 10000 along +X equals the same force added by hand: " + f2s(xArea) + " against " + f2s(xHand));

    for (int variant = 0; variant < 2; ++variant) {
        aver_phys_init();
        aver_phys_set_gravity(0, 0, 0);
        if (variant == 0) {
            const int32_t area = bigArea();
            aver_phys_area_set_wind(area, 10000.0f, 0, 1, 0, 2.0f, 500.0f);   // factor (500/d)^2
        }
        const int32_t box = aver_phys_add_dynamic_box(0, 0, 1000.0f, 20, 20, 20, 10.0f);   // d = 1000: one quarter
        for (int i = 0; i < 30; ++i) {
            if (variant == 1) aver_phys_body_add_force(box, 0, 2500.0f, 0);
            aver_phys_step(kDt);
        }
        (variant == 0 ? xFar : xFarHand) = posOf(box, 1);
        aver_phys_shutdown();
    }
    check(xFarHand > 5.0f && near(xFar, xFarHand, 0.2f), "attenuation 2 at twice the unit distance is a quarter of the force: " + f2s(xFar) + " against " + f2s(xFarHand));
}

static void testWakeAndRemove() {
    AVER_INFO("=== sleepers wake when pushed; a removed area lets go ===");
    aver_phys_init();
    const int32_t fl = floorBox();
    (void)fl;
    const int32_t area = bigArea(0.0f, 2000.0f);   // overrides start disabled
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    Log lg;
    lg.step(300);
    check(aver_phys_body_is_active(box) == 0, "the box sleeps on the floor inside the idle area");
    aver_phys_area_set_gravity(area, OV_REPLACE, 2000.0f, 0, 0, 1);
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    check(posOf(box, 2) > 200.0f, "an area that starts pushing wakes it and lifts it: z = " + f2s(posOf(box, 2)));

    lg.clear();
    aver_phys_remove_body(area);
    lg.step(1);
    check(lg.count(area, box, 0) == 1, "removing the area exits what it held");
    const float z0 = posOf(box, 2);
    float vz0 = 0;
    { float v[3]; aver_phys_body_velocity(box, v); vz0 = v[2]; }
    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);
    float vz1 = 0;
    { float v[3]; aver_phys_body_velocity(box, v); vz1 = v[2]; }
    check(vz1 < vz0 - 200.0f && posOf(box, 2) != z0, "and the world's gravity takes over: vz " + f2s(vz0) + " -> " + f2s(vz1));
    aver_phys_shutdown();
}

static void testRestore() {
    AVER_INFO("=== a snapshot restore replays an area-driven run exactly ===");
    aver_phys_init();
    const int32_t area = bigArea();
    aver_phys_area_set_gravity(area, OV_REPLACE, 300.0f, 0, 0, 1);
    aver_phys_area_set_damping(area, OV_COMBINE, 1.0f, OV_OFF, 0.0f);
    aver_phys_area_set_wind(area, 2000.0f, 1, 0, 0, 0.0f, 0.0f);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 500.0f, 20, 20, 20, 10.0f);
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    float p1[3];
    aver_phys_body_position(box, p1);
    check(aver_phys_snapshot_restore(snap, 0) == 1, "restored");
    for (int i = 0; i < 60; ++i) aver_phys_step(kDt);
    float p2[3];
    aver_phys_body_position(box, p2);
    check(std::memcmp(p1, p2, sizeof p1) == 0, "the same 60 steps land on the same position, bit for bit: " + f2s(p1[2]) + " and " + f2s(p2[2]));
    aver_phys_snapshot_release_all();
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("Areas (G2)");

    testUnchangedAndRefusals();
    testEnterExit();
    testMasksAndAreas();
    testGravity();
    testGravityDynamics();
    testDamping();
    testWind();
    testWakeAndRemove();
    testRestore();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
