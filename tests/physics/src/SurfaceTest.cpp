// SurfaceTest -- physical surfaces (physics_surface_abi.h, docs/plans/wave11/SHAPES_MATERIALS.md).
//
// THE STANDARD IS OBSERVED MOTION: a surface that is registered and never reaches a contact changes nothing, so the checks
// slide boxes across floors made of different materials and read how far they got, or read what a ray, a contact, a
// character's ground and a wheel say they touched. The margins are tolerances: a failing
// relation is a prompt to look at the physics before loosening a number.
//
//   0. a project that has never interned a surface runs the same bits once one exists (the combine hook's two paths),
//   1. ids: the golden values, the same function in Aver.Core,
//   2. the registry: upsert, info, interned-only ids, enumeration, reset, surviving a shutdown,
//   3. a leaf material changes how far a box slides; a per-triangle palette does too, and a ray names it,
//   4. restitution, "defer", a live tune, the body label and its coexistence with the entity stamp,
//   5. contacts report closing speed, both surfaces and the friction applied,
//   6. the character's ground, a wheel's surface, the surface sampler still winning, compound leaves.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/SurfaceId.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_queries_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_shapes_abi.h"
#include "aver/physics/physics_surface_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/physics/physics_water_abi.h"

#include <cmath>
#include <cstdio>
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

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static const float kDt = 1.0f / 60.0f;
static const int32_t kBadHandle = -1, kOutOfRange = -4, kInvalidArgument = -6;

static void stepN(int n) {
    for (int i = 0; i < n; ++i) aver_phys_step(kDt);
}

static int32_t S(const char* name) { return aver_phys_surface_id(name); }

// The standard surfaces every test below starts from: written before each one, because a test may tune them.
static int32_t g_ice = 0, g_rubber = 0, g_bouncy = 0;
static void standardSurfaces() {
    g_ice = aver_phys_surface_register("Ice", 0.02f, 0.0f, 3);
    g_rubber = aver_phys_surface_register("Rubber", 1.0f, 0.0f, 4);
    g_bouncy = aver_phys_surface_register("Bouncy", -1.0f, 0.9f, 5);
}

// A floor whose TOP SURFACE is exactly z = 0, from the legacy creator (no leaf material, body friction 0.2).
static void plainFloor() {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
}

static int32_t bodyOfShape(int32_t shape, float x, float y, float z, int32_t motion, float mass) {
    const float o[3] = {x, y, z};
    return aver_phys_body_from_shape(shape, o, nullptr, motion, mass, nullptr, 0);
}

static void position(int32_t body, float* p) {
    p[0] = p[1] = p[2] = 0.0f;
    aver_phys_body_position(body, p);
}

// A 40 cm box with a leaf surface, dynamic, dropped just above z = 0 at (x, y), settled, then given +500 cm/s along X.
static int32_t launchBox(int32_t surface, float x, float y) {
    const int32_t shape = aver_phys_shape_box(20, 20, 20, surface, nullptr, 0);
    const int32_t b = bodyOfShape(shape, x, y, 20.5f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    stepN(30);
    aver_phys_body_set_velocity(b, 500.0f, 0, 0);
    return b;
}

// How far a leaf-surface box slides along X on the plain floor (body friction 0.2) in 15 s.
static float slideOnPlainFloor(int32_t surface) {
    plainFloor();
    const int32_t b = launchBox(surface, -3000.0f, 0.0f);
    stepN(900);
    float p[3];
    position(b, p);
    aver_phys_shutdown();
    return p[0] + 3000.0f;
}

// ---- a floor mesh with two surfaces: ice for y < 0, rubber for y > 0 --------------------------------------------------

struct Mesh {
    std::vector<float> v;
    std::vector<int32_t> idx;
    std::vector<uint8_t> pal;
    std::vector<int32_t> user;
};

// A flat quad at z = 0 facing up (mesh winding: (B - A) x (C - A) is outward; a mesh is one-sided to bodies), two triangles with the given palette
// index and user data.
static void addQuad(Mesh& m, float x0, float y0, float x1, float y1, uint8_t palette, int32_t u0, int32_t u1) {
    const int32_t base = static_cast<int32_t>(m.v.size() / 3);
    const float c[4][3] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};   // p00 p10 p11 p01
    for (const auto& p : c) { m.v.push_back(p[0]); m.v.push_back(p[1]); m.v.push_back(p[2]); }
    for (int32_t k : {0, 1, 2, 0, 2, 3}) m.idx.push_back(base + k);   // (p00, p10, p11) and (p00, p11, p01)
    m.pal.push_back(palette); m.pal.push_back(palette);
    m.user.push_back(u0); m.user.push_back(u1);
}

static int32_t twoSurfaceFloorShape() {
    Mesh m;
    addQuad(m, -4000.0f, -2000.0f, 4000.0f, 0.0f, 0, 10, 11);   // ice
    addQuad(m, -4000.0f, 0.0f, 4000.0f, 2000.0f, 1, 20, 21);    // rubber
    const int32_t palette[2] = {g_ice, g_rubber};
    return aver_phys_shape_mesh(m.v.data(), static_cast<int32_t>(m.v.size() / 3), m.idx.data(), static_cast<int32_t>(m.idx.size()),
                                m.pal.data(), palette, 2, m.user.data(), nullptr, 0);
}

static int32_t twoSurfaceFloor() {
    aver_phys_init();
    const int32_t shape = twoSurfaceFloorShape();
    return bodyOfShape(shape, 0, 0, 0, AVER_PHYS_MOTION_STATIC, 0);
}

// Which half a ray lands on.
struct Detail {
    float f[AVER_PHYS_HITF_COUNT] = {};
    int32_t i[AVER_PHYS_HITI_COUNT] = {};
    int32_t body = 0;
};
static Detail rayDown(float x, float y, float fromZ = 100.0f, int32_t ignore = 0, int32_t flags = 0) {
    Detail d;
    d.body = aver_phys_raycast_detail(x, y, fromZ, 0, 0, -1, 1000.0f, 0xFFFFFFFFu, ignore, flags,
                                      d.f, AVER_PHYS_HITF_COUNT, d.i, AVER_PHYS_HITI_COUNT);
    return d;
}

// ---------------------------------------------------------------------------------------------------------------------

// The scene the bit-identity test replays: a box with an initial slide, a sphere, a compound, on a floor.
static std::vector<float> identityScene() {
    aver_phys_init();
    aver_phys_add_static_box(0, 0, -100.0f, 5000.0f, 5000.0f, 100.0f);
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 40.0f, 20, 20, 20, 8.0f);
    aver_phys_body_set_friction(box, 0.5f);
    aver_phys_body_set_velocity(box, 300.0f, 100.0f, 0);
    const int32_t ball = aver_phys_add_dynamic_sphere(600, 0, 200.0f, 25, 5.0f);
    aver_phys_body_set_restitution(ball, 0.4f);
    const float offs[6] = {0, 0, 0, 80, 0, 0};
    const float halves[6] = {30, 10, 10, 10, 10, 30};
    const int32_t comp = aver_phys_add_dynamic_compound_boxes(-600, 0, 150.0f, offs, halves, 2, 6.0f);
    stepN(300);
    std::vector<float> out;
    for (int32_t b : {box, ball, comp}) {
        float p[3], q[4];
        position(b, p);
        aver_phys_body_rotation(b, q);
        out.insert(out.end(), {p[0], p[1], p[2], q[0], q[1], q[2], q[3]});
    }
    aver_phys_shutdown();
    return out;
}

static void testBitIdentity() {
    AVER_INFO("=== a project with no surface runs the same bits once one exists ===");
    // Must run before any surface is interned: until one is, the combine functions evaluate Jolt's own expressions on the body
    // values; once one is, they go through the leaf lookup. The scene touches no surface either time.
    const std::vector<float> before = identityScene();
    aver_phys_init();
    const int32_t unused = aver_phys_shape_sphere(5, S("BitIdentityUnused"), nullptr, 0);
    check(unused != 0, "a shape with a surface interns it");
    aver_phys_shutdown();
    const std::vector<float> after = identityScene();
    check(before.size() == after.size() && std::memcmp(before.data(), after.data(), before.size() * sizeof(float)) == 0,
          "the same scene gives the same bits before and after a surface exists");
    check(before.size() == 21 && before[2] > 0.0f, "and the scene did something (box z = " + f2s(before.empty() ? 0.0f : before[2]) + ")");
}

static void testIds() {
    AVER_INFO("=== surface ids ===");
    check(S("Ice") == -714267872, "\"Ice\" is -714267872, got " + std::to_string(S("Ice")));
    check(S("Grass") == 1479220765, "\"Grass\" is 1479220765");
    check(S("Rubber") == 1606840731, "\"Rubber\" is 1606840731");
    check(S("Wood") == 81868168, "\"Wood\" is 81868168");
    check(S(nullptr) == 0 && S("") == 0, "NULL and empty are 0");
    check(S("ice") != S("Ice"), "names are case-sensitive");
    bool same = true;
    for (const char* n : {"Ice", "Grass", "Rubber", "Wood", "Metal", "caf\xC3\xA9", "a"})
        same = same && S(n) == aver::surfaceIdFromName(n);
    check(same, "aver_phys_surface_id equals the inline in Aver.Core");
    check(aver_phys_surface_abi_version() == AVER_PHYS_SURFACE_ABI_VERSION, "the version call answers");
}

static void testRegistry() {
    AVER_INFO("=== the registry ===");
    aver_phys_init();
    const int32_t before = aver_phys_surface_count();
    const int32_t ice = aver_phys_surface_register("RegIce", 0.02f, 0.1f, 3);
    check(ice == S("RegIce"), "register returns the hash of the name");
    check(aver_phys_surface_register("RegIce", 0.03f, 0.2f, 9) == ice && aver_phys_surface_count() == before + 1,
          "registering the same name again updates it and adds nothing");
    float f = -9, r = -9;
    int32_t tag = -9;
    uint8_t name[32] = {};
    check(aver_phys_surface_info(ice, &f, &r, &tag, name, 32) == 1 && near(f, 0.03f, 1e-5f) && near(r, 0.2f, 1e-5f) && tag == 9 &&
          std::strcmp(reinterpret_cast<const char*>(name), "RegIce") == 0, "info returns the name and the updated numbers");
    uint8_t tiny[3] = {};
    check(aver_phys_surface_info(ice, nullptr, nullptr, nullptr, tiny, 3) == 1 && std::strcmp(reinterpret_cast<const char*>(tiny), "Re") == 0,
          "a short name buffer truncates and stays NUL-terminated");
    check(aver_phys_surface_at(before) == ice, "at() enumerates in registration order");
    check(aver_phys_surface_at(-1) == 0 && aver_phys_surface_at(100000) == 0, "at() is 0 out of range");

    aver_phys_surface_register("RegClamp", 99.0f, -4.0f, 0);
    aver_phys_surface_info(S("RegClamp"), &f, &r, &tag, nullptr, 0);
    check(near(f, 10.0f, 1e-5f) && r == -1.0f, "friction clamps to 10 and a negative restitution defers (-1)");

    // An id interned only by a shape has no name and the unset numbers.
    const int32_t only = S("RegOnlyShape");
    check(aver_phys_shape_sphere(10, only, nullptr, 0) != 0, "a shape names a surface nobody registered");
    name[0] = 'x';
    check(aver_phys_surface_info(only, &f, &r, &tag, name, 32) == 1 && f == -1.0f && r == -1.0f && tag == 0 && name[0] == 0,
          "info of an interned-only id: empty name, -1, -1, 0");
    check(aver_phys_surface_info(S("NeverUsed"), &f, &r, &tag, nullptr, 0) == 0 && aver_phys_last_error() == kBadHandle,
          "an id nothing used is unknown (BadHandle)");
    check(aver_phys_surface_tune(S("NeverUsed"), 1, 1, 1) == 0 && aver_phys_last_error() == kBadHandle, "and cannot be tuned");
    check(aver_phys_surface_tune(ice, 0.5f, 0.25f, 11) == 1, "tune works on a registered id");
    aver_phys_surface_info(ice, &f, &r, &tag, nullptr, 0);
    check(near(f, 0.5f, 1e-5f) && near(r, 0.25f, 1e-5f) && tag == 11, "and changes the numbers");

    check(aver_phys_surface_register(nullptr, 1, 1, 0) == 0 && aver_phys_last_error() == kInvalidArgument, "a NULL name is refused");
    check(aver_phys_surface_register("", 1, 1, 0) == 0, "an empty name is refused");
    check(aver_phys_surface_register("RegNaN", std::nanf(""), 0, 0) == 0, "a non-finite number is refused");

    // The registry outlives the world.
    aver_phys_shutdown();
    aver_phys_init();
    check(aver_phys_surface_info(ice, &f, nullptr, nullptr, nullptr, 0) == 1 && near(f, 0.5f, 1e-5f), "the registry survives shutdown and init");

    // reset forgets names and numbers; a shape holding the surface keeps working.
    const int32_t shape = aver_phys_shape_box(10, 10, 10, ice, nullptr, 0);
    aver_phys_surface_reset();
    check(aver_phys_surface_count() == 0, "reset empties the enumeration");
    check(aver_phys_surface_info(ice, &f, &r, &tag, name, 32) == 1 && f == -1.0f && r == -1.0f && tag == 0 && name[0] == 0,
          "reset returns the numbers to unset and clears the name");
    check(shape != 0 && bodyOfShape(shape, 0, 0, 100, AVER_PHYS_MOTION_DYNAMIC, 1.0f) != 0, "a shape holding it still makes bodies");
    aver_phys_shutdown();
}

static void testLeafFriction() {
    AVER_INFO("=== a leaf material changes how far a box slides ===");
    aver_phys_init();
    standardSurfaces();
    aver_phys_shutdown();
    const float ice = slideOnPlainFloor(g_ice);
    const float rubber = slideOnPlainFloor(g_rubber);
    check(ice > 5.0f * rubber, "500 cm/s on Ice slides at least 5x as far as on Rubber: " + f2s(ice) + " vs " + f2s(rubber));
    check(rubber > 20.0f, "and the rubber box does slide, " + f2s(rubber) + " cm");
}

static void testPerTriangle() {
    AVER_INFO("=== a mesh floor with a palette: sliding and rays ===");
    standardSurfaces();
    const int32_t floorBody = twoSurfaceFloor();
    check(floorBody != 0, "the two-surface floor is built");

    const int32_t onIce = launchBox(0, -3000.0f, -1000.0f);
    const int32_t onRubber = launchBox(0, -3000.0f, 1000.0f);
    stepN(900);
    float a[3], b[3];
    position(onIce, a);
    position(onRubber, b);
    check((a[0] + 3000.0f) > 5.0f * (b[0] + 3000.0f), "boxes slide >= 5x as far on the ice half: " + f2s(a[0] + 3000.0f) + " vs " + f2s(b[0] + 3000.0f));

    const Detail ice = rayDown(2000, -1000);
    check(ice.body == floorBody, "a ray on the ice half hits the floor body");
    check(ice.i[AVER_PHYS_HITI_SURFACE] == g_ice, "it names Ice");
    check(near(ice.f[AVER_PHYS_HITF_FRICTION], 0.02f, 1e-5f) && ice.i[AVER_PHYS_HITI_TAG] == 3, "with Ice's friction and tag");
    check(ice.i[AVER_PHYS_HITI_TRIANGLE] == 10 || ice.i[AVER_PHYS_HITI_TRIANGLE] == 11, "and the triangle user data of its quad, got " + std::to_string(ice.i[AVER_PHYS_HITI_TRIANGLE]));
    check(near(ice.f[AVER_PHYS_HITF_POINT_Z], 0.0f, 0.5f) && near(ice.f[AVER_PHYS_HITF_DISTANCE_CM], 100.0f, 0.5f), "the hit is at z = 0, 100 cm along");
    check(near(ice.f[AVER_PHYS_HITF_NORMAL_Z], 1.0f, 0.01f), "with an upward normal");
    check(ice.i[AVER_PHYS_HITI_CHILD] == -1 && ice.i[AVER_PHYS_HITI_CELL_X] == -1 && ice.i[AVER_PHYS_HITI_CELL_Y] == -1, "no compound child, no heightfield cell");

    const Detail rub = rayDown(2000, 1000);
    check(rub.i[AVER_PHYS_HITI_SURFACE] == g_rubber && near(rub.f[AVER_PHYS_HITF_FRICTION], 1.0f, 1e-5f) && rub.i[AVER_PHYS_HITI_TAG] == 4,
          "the rubber half names Rubber with its numbers");
    check(rub.i[AVER_PHYS_HITI_TRIANGLE] == 20 || rub.i[AVER_PHYS_HITI_TRIANGLE] == 21, "and its own triangle user data");

    // The same detail from any (body, sub-shape) pair.
    float f[AVER_PHYS_HITF_COUNT];
    int32_t iv[AVER_PHYS_HITI_COUNT];
    check(aver_phys_subshape_info(floorBody, ice.i[AVER_PHYS_HITI_SUB_SHAPE], f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 1 &&
          iv[AVER_PHYS_HITI_SURFACE] == g_ice && iv[AVER_PHYS_HITI_BODY] == floorBody, "subshape_info repeats the answer from the raw sub-shape id");
    check(aver_phys_subshape_info(99999, 0, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 0 && aver_phys_last_error() == kBadHandle,
          "and refuses a dead body");

    // Filters: ignoring the floor misses; a sensor is skipped unless asked.
    check(rayDown(2000, -1000, 100.0f, floorBody).body == 0, "ignoreBody skips the floor: a miss");
    const int32_t sensor = aver_phys_add_sensor_box(2000, -1000, 50, 100, 100, 10);
    check(rayDown(2000, -1000, 200.0f).body == floorBody, "a ray passes through a sensor by default");
    const Detail withSensor = rayDown(2000, -1000, 200.0f, 0, AVER_PHYS_QUERY_INCLUDE_SENSORS);
    check(withSensor.body == sensor && near(withSensor.f[AVER_PHYS_HITF_POINT_Z], 60.0f, 1.0f), "and stops on it with INCLUDE_SENSORS");
    check(aver_phys_raycast_detail(0, 0, 100, 0, 0, -1, 1000, 0xFFFFFFFFu, 0, 0, nullptr, 3, nullptr, 0) == 0 && aver_phys_last_error() == -2,
          "a capacity with no buffer is a NullPointer");
    aver_phys_shutdown();
}

static float bouncePeak(bool bouncy) {
    aver_phys_init();
    standardSurfaces();
    const int32_t floorShape = aver_phys_shape_box(5000, 5000, 100, bouncy ? g_bouncy : 0, nullptr, 0);
    bodyOfShape(floorShape, 0, 0, -100.0f, AVER_PHYS_MOTION_STATIC, 0);
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 300.0f, 20.0f, 5.0f);
    float peak = 0.0f;
    bool landed = false;
    int sinceLanding = 0;
    for (int s = 0; s < 400 && sinceLanding < 90; ++s) {
        aver_phys_step(kDt);
        float p[3];
        position(ball, p);
        if (p[2] <= 25.0f) landed = true;
        if (landed) { peak = std::fmax(peak, p[2]); ++sinceLanding; }
    }
    aver_phys_shutdown();
    return peak;
}

static void testRestitution() {
    AVER_INFO("=== restitution: the larger of the two sides ===");
    const float bouncy = bouncePeak(true), plain = bouncePeak(false);
    check(bouncy > 150.0f, "a ball on a 0.9 leaf rebounds high, peak z = " + f2s(bouncy));
    check(plain < 60.0f, "the same ball on a floor with no material barely does, peak z = " + f2s(plain));
}

static void testDefer() {
    AVER_INFO("=== a surface that defers changes nothing ===");
    aver_phys_init();
    const int32_t deferred = aver_phys_surface_register("DeferOnly", -1.0f, -1.0f, 7);
    aver_phys_shutdown();
    auto run = [](int32_t surface) {
        plainFloor();
        const int32_t b = launchBox(surface, -1000.0f, 0.0f);
        stepN(300);
        float p[3];
        position(b, p);
        aver_phys_shutdown();
        return std::vector<float>{p[0], p[1], p[2]};
    };
    const std::vector<float> none = run(0), with = run(deferred);
    check(std::memcmp(none.data(), with.data(), 3 * sizeof(float)) == 0, "positions are bit-equal with and without the surface");
}

static void testTuneIsLive() {
    AVER_INFO("=== tune is live ===");
    aver_phys_init();
    const int32_t id = aver_phys_surface_register("Tunable", 1.0f, 0.0f, 0);
    aver_phys_shutdown();
    const float grippy = slideOnPlainFloor(id);
    aver_phys_surface_tune(id, 0.02f, 0.0f, 0);
    const float slick = slideOnPlainFloor(id);
    check(slick > 3.0f * grippy, "lowering the surface's friction lengthens the next slide: " + f2s(grippy) + " then " + f2s(slick));
}

static void testBodyLabel() {
    AVER_INFO("=== the body label ===");
    plainFloor();
    standardSurfaces();
    const int32_t b = aver_phys_add_dynamic_box(0, 0, 500, 20, 20, 20, 5.0f);
    aver_phys_set_gravity(0, 0, 0);
    check(aver_phys_body_surface(b) == 0, "a fresh body has no label");
    check(aver_phys_set_entity(b, 77) == 1 && aver_phys_body_set_surface(b, g_ice, 0) == 1, "the entity and a label are set");
    check(aver_phys_body_surface(b) == g_ice, "the label reads back");
    float p[3], n[3];
    int32_t entity = 0;
    check(aver_phys_raycast(0, 0, 800, 0, 0, -1, 1000, p, n, &entity) == b && entity == 77, "the entity stamp survives the label");
    aver_phys_set_entity(b, -5);
    entity = 0;
    aver_phys_raycast(0, 0, 800, 0, 0, -1, 1000, p, n, &entity);
    check(entity == -5 && aver_phys_body_surface(b) == g_ice, "a negative entity does not corrupt the label");
    aver_phys_set_entity(b, 12);
    check(aver_phys_body_surface(b) == g_ice, "set_entity keeps the label");

    // A label with no leaf material is what the ray names.
    const Detail d = rayDown(0, 0, 800.0f);
    check(d.body == b && d.i[AVER_PHYS_HITI_SURFACE] == g_ice && d.i[AVER_PHYS_HITI_ENTITY] == 12, "a ray names the label when the shape has no material, and the entity");
    check(near(d.f[AVER_PHYS_HITF_FRICTION], 0.02f, 1e-5f), "with the surface's friction");

    // The label does not change the body's own friction unless asked.
    float fr = 0;
    aver_phys_body_friction(b, &fr);
    check(near(fr, 0.2f, 1e-4f), "a label alone leaves the body friction at 0.2, got " + f2s(fr));
    const int32_t c = aver_phys_add_dynamic_box(2000, 0, 500, 20, 20, 20, 5.0f);
    check(aver_phys_body_set_surface(c, g_rubber, 1) == 1, "set_surface with copyPhysics");
    aver_phys_body_friction(c, &fr);
    check(near(fr, 1.0f, 1e-4f), "copies the registered friction into the body, got " + f2s(fr));
    check(aver_phys_body_set_surface(c, 0, 0) == 1 && aver_phys_body_surface(c) == 0, "label 0 clears it");

    // The label survives a static -> dynamic rebuild.
    const int32_t s = aver_phys_add_static_box(4000, 0, 100, 20, 20, 20);
    aver_phys_set_entity(s, 31);
    aver_phys_body_set_surface(s, g_rubber, 0);
    check(aver_phys_body_set_motion_type(s, AVER_PHYS_MOTION_DYNAMIC) == 1, "a static box is made dynamic");
    check(aver_phys_body_surface(s) == g_rubber, "and keeps its label");

    check(aver_phys_body_set_surface(99999, g_ice, 0) == 0 && aver_phys_last_error() == kBadHandle, "a dead handle is refused");
    check(aver_phys_body_surface(99999) == -1, "and reads -1");
    aver_phys_shutdown();
}

static void testContacts() {
    AVER_INFO("=== contacts: closing speed, surfaces, friction applied ===");
    aver_phys_init();
    const int32_t cIce = aver_phys_surface_register("ContactIce", 0.1f, 0.0f, 0);
    const int32_t cRub = aver_phys_surface_register("ContactRubber", 0.9f, 0.2f, 0);
    const int32_t floorShape = aver_phys_shape_box(5000, 5000, 100, cIce, nullptr, 0);
    const int32_t floorBody = bodyOfShape(floorShape, 0, 0, -100.0f, AVER_PHYS_MOTION_STATIC, 0);
    const int32_t boxShape = aver_phys_shape_box(20, 20, 20, cRub, nullptr, 0);
    const int32_t box = bodyOfShape(boxShape, 0, 0, 220.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);

    bool found = false;
    float closing = 0, friction = 0, restitution = 0;
    int32_t sA = 0, sB = 0, idx = -1;
    for (int s = 0; s < 120 && !found; ++s) {
        aver_phys_step(kDt);
        const int32_t n = aver_phys_contact_count();
        for (int32_t i = 0; i < n && !found; ++i) {
            int32_t a = 0, b = 0;
            float pt[3], nrm[3];
            aver_phys_contact_get(i, &a, &b, pt, nrm);
            if ((a != box && b != box) || (a != floorBody && b != floorBody)) continue;
            float f[AVER_PHYS_CONF_COUNT];
            int32_t iv[AVER_PHYS_CONI_COUNT];
            if (aver_phys_contact_detail(i, f, AVER_PHYS_CONF_COUNT, iv, AVER_PHYS_CONI_COUNT) != 1) continue;
            found = true;
            idx = i;
            closing = f[AVER_PHYS_CONF_CLOSING_SPEED_CMS];
            friction = f[AVER_PHYS_CONF_FRICTION];
            restitution = f[AVER_PHYS_CONF_RESTITUTION];
            sA = iv[AVER_PHYS_CONI_SURFACE_A];
            sB = iv[AVER_PHYS_CONI_SURFACE_B];
        }
    }
    check(found, "the box's first contact with the floor is reported with a detail");
    const float expected = std::sqrt(2.0f * 980.0f * 200.0f);
    check(near(std::fabs(closing), expected, expected * 0.10f), "closing speed is about sqrt(2 g h) = " + f2s(expected) + ", got " + f2s(closing));
    check((sA == cIce && sB == cRub) || (sA == cRub && sB == cIce), "both surfaces are named");
    check(near(friction, std::sqrt(0.1f * 0.9f), 0.01f), "the friction applied is sqrt(0.1 * 0.9) = 0.3, got " + f2s(friction));
    check(near(restitution, 0.2f, 0.01f), "the restitution applied is the larger, 0.2, got " + f2s(restitution));

    // Index-aligned with contact_get, and cleared with it on the next step.
    aver_phys_step(kDt);
    const int32_t now = aver_phys_contact_count();
    float f[AVER_PHYS_CONF_COUNT];
    int32_t iv[AVER_PHYS_CONI_COUNT];
    check(aver_phys_contact_detail(now, f, AVER_PHYS_CONF_COUNT, iv, AVER_PHYS_CONI_COUNT) == 0 && aver_phys_last_error() == kOutOfRange,
          "an index past the current contacts is OutOfRange (the old contact was cleared, idx was " + std::to_string(idx) + ")");
    aver_phys_shutdown();
}

static void testCharacterGround() {
    AVER_INFO("=== the ground under a character ===");
    standardSurfaces();
    const int32_t floorBody = twoSurfaceFloor();
    const int32_t onIce = aver_phys_character_create(30.0f, 180.0f, 0, -1000.0f, 120.0f);
    const int32_t onRubber = aver_phys_character_create(30.0f, 180.0f, 1000.0f, 1000.0f, 120.0f);
    float f[AVER_PHYS_HITF_COUNT];
    int32_t iv[AVER_PHYS_HITI_COUNT];
    check(aver_phys_character_ground_detail(onIce, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 0, "in the air there is no ground: 0, not an error");
    stepN(120);
    check(aver_phys_character_grounded(onIce) == 1 && aver_phys_character_grounded(onRubber) == 1, "both characters landed");
    check(aver_phys_character_ground_detail(onIce, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 1 &&
          iv[AVER_PHYS_HITI_SURFACE] == g_ice && iv[AVER_PHYS_HITI_BODY] == floorBody, "the character on the ice half stands on Ice");
    check(near(f[AVER_PHYS_HITF_FRICTION], 0.02f, 1e-5f) && near(f[AVER_PHYS_HITF_NORMAL_Z], 1.0f, 0.05f), "with its friction and an upward normal");
    check(aver_phys_character_ground_detail(onRubber, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 1 && iv[AVER_PHYS_HITI_SURFACE] == g_rubber,
          "the other stands on Rubber");
    check(aver_phys_character_ground_detail(99999, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 0 && aver_phys_last_error() == kBadHandle,
          "a dead character is refused");

    // A raw heightfield built without a palette answers surface 0.
    std::vector<float> samples(9 * 9, 0.0f);
    aver_phys_add_heightfield(samples.data(), 9, 200.0f, 8000.0f, 2500.0f, 0.0f);
    const int32_t onField = aver_phys_character_create(30.0f, 180.0f, 7200.0f, 3300.0f, 120.0f);
    stepN(120);
    check(aver_phys_character_grounded(onField) == 1, "a character landed on the heightfield");
    check(aver_phys_character_ground_detail(onField, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 1 && iv[AVER_PHYS_HITI_SURFACE] == 0 &&
          f[AVER_PHYS_HITF_FRICTION] == -1.0f, "a heightfield with no cell palette reports surface 0");
    aver_phys_shutdown();
}

static void testVehicleWheels() {
    AVER_INFO("=== a car with one side on ice and one on tarmac ===");
    aver_phys_init();
    const int32_t vIce = aver_phys_surface_register("VehIce", 0.05f, 0.0f, 0);
    const int32_t vTar = aver_phys_surface_register("VehTarmac", 0.9f, 0.0f, 0);
    const int32_t left = aver_phys_shape_box(20000, 5000, 50, vIce, nullptr, 0);
    const int32_t right = aver_phys_shape_box(20000, 5000, 50, vTar, nullptr, 0);
    bodyOfShape(left, 0, -5000.0f, -50.0f, AVER_PHYS_MOTION_STATIC, 0);   // -Y half
    bodyOfShape(right, 0, 5000.0f, -50.0f, AVER_PHYS_MOTION_STATIC, 0);   // +Y half

    const float radius = 33.0f, susp = 30.0f, hz = 2.0f;
    const float sag = 981.0f / ((2.0f * 3.14159265f * hz) * (2.0f * 3.14159265f * hz));
    const float attachZ = radius + susp - sag;
    const int32_t v = aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1);
    for (int i = 0; i < 4; ++i)   // wheels 0 and 2 are on y = -77 (ice), 1 and 3 on y = +77 (tarmac)
        aver_phys_vehicle_add_wheel(v, i < 2 ? 180.0f : -180.0f, (i % 2 == 0) ? -77.0f : 77.0f, attachZ, radius, 22, 5, susp, hz, 0.5f,
                                    i < 2 ? 35.0f : 0.0f, 1500, 2500, i >= 2 ? 1 : 0);
    aver_phys_vehicle_finish(v, 60.0f);
    stepN(240);

    float f[AVER_PHYS_HITF_COUNT];
    int32_t iv[AVER_PHYS_HITI_COUNT];
    bool surfacesRight = true;
    for (int w = 0; w < 4; ++w) {
        const int32_t expected = (w % 2 == 0) ? vIce : vTar;
        const int32_t got = aver_phys_vehicle_wheel_surface(v, w, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT);
        surfacesRight = surfacesRight && got == 1 && iv[AVER_PHYS_HITI_SURFACE] == expected;
    }
    check(surfacesRight, "each wheel names the surface it stands on (left ice, right tarmac)");

    float wf[AVER_PHYS_VWS_COUNT];
    int32_t wi[AVER_PHYS_VWSI_COUNT];
    aver_phys_vehicle_wheel_state(v, 0, wf, AVER_PHYS_VWS_COUNT, wi, AVER_PHYS_VWSI_COUNT);
    const float iceFriction = wf[AVER_PHYS_VWS_SURFACE_FRICTION];
    aver_phys_vehicle_wheel_state(v, 1, wf, AVER_PHYS_VWS_COUNT, wi, AVER_PHYS_VWSI_COUNT);
    const float tarFriction = wf[AVER_PHYS_VWS_SURFACE_FRICTION];
    check(near(iceFriction, 0.05f, 1e-4f) && near(tarFriction, 0.9f, 1e-4f),
          "the telemetry reports the leaf friction under each tyre: " + f2s(iceFriction) + " and " + f2s(tarFriction));
    check(aver_phys_vehicle_wheel_surface(v, 9, f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 0 && aver_phys_last_error() == kOutOfRange,
          "a wheel past the last is OutOfRange");
    aver_phys_shutdown();
}

// A car on a floor whose whole leaf is `friction`, thrown sideways at 300 cm/s: how much of that sideways speed is left after a third of
// a second (a grippy tyre has stopped it by then; a slick one is still sliding, and a longer wait would let both reach zero).
static float sidewaysSpeedLeft(const char* surfaceName, float friction) {
    aver_phys_init();
    const int32_t surface = aver_phys_surface_register(surfaceName, friction, 0.0f, 0);
    const int32_t floorShape = aver_phys_shape_box(20000, 20000, 50, surface, nullptr, 0);
    bodyOfShape(floorShape, 0, 0, -50.0f, AVER_PHYS_MOTION_STATIC, 0);

    const float radius = 33.0f, susp = 30.0f, hz = 2.0f;
    const float sag = 981.0f / ((2.0f * 3.14159265f * hz) * (2.0f * 3.14159265f * hz));
    const float attachZ = radius + susp - sag;
    const int32_t v = aver_phys_vehicle_create(220, 90, 35, 25, 0, 0, 40, 1300, 0, 0, 5, 0, 0, 0, 1);
    for (int i = 0; i < 4; ++i)
        aver_phys_vehicle_add_wheel(v, i < 2 ? 180.0f : -180.0f, (i % 2 == 0) ? -77.0f : 77.0f, attachZ, radius, 22, 5, susp, hz, 0.5f,
                                    i < 2 ? 35.0f : 0.0f, 1500, 2500, i >= 2 ? 1 : 0);
    aver_phys_vehicle_finish(v, 60.0f);
    stepN(240);
    aver_phys_body_set_velocity(aver_phys_vehicle_body(v), 0.0f, 300.0f, 0.0f);
    stepN(20);
    float vel[3] = {0, 0, 0};
    aver_phys_vehicle_velocity(v, vel);
    aver_phys_shutdown();
    return std::fabs(vel[1]);
}

// The "lower lateral grip on the ice side" case: a tyre on a low-friction leaf holds a sliding car far less.
static void testVehicleLateralGrip() {
    AVER_INFO("=== a car thrown sideways keeps more of it on ice than on tarmac ===");
    const float onIce = sidewaysSpeedLeft("LatIce", 0.05f);
    const float onTarmac = sidewaysSpeedLeft("LatTarmac", 0.9f);
    check(onIce > onTarmac + 80.0f && onTarmac < 0.6f * onIce,
          "after a third of a second " + f2s(onIce) + " cm/s is left on ice and " + f2s(onTarmac) + " on tarmac");
}

struct Sampler {
    float friction = 0.0f;
};
static int32_t fixedSampler(float, float, float* fr, float* re, void* user) {
    *fr = static_cast<Sampler*>(user)->friction;
    *re = 0.0f;
    return 1;
}

static float samplerSlide(float samplerFriction) {
    aver_phys_init();
    standardSurfaces();
    Sampler s;
    s.friction = samplerFriction;
    const int32_t floorShape = aver_phys_shape_box(3000, 3000, 100, g_rubber, nullptr, 0);   // a grippy leaf under the sampler
    const int32_t floorBody = bodyOfShape(floorShape, 0, 0, -100.0f, AVER_PHYS_MOTION_STATIC, 0);
    aver_phys_body_set_surface_sampled(floorBody, 1);
    aver_phys_set_surface_sampler(&fixedSampler, &s);
    const int32_t box = aver_phys_add_dynamic_box(-700, 0, 50.5f, 50, 50, 50, 0.0f);
    aver_phys_body_set_friction(box, 0.5f);
    aver_phys_body_set_velocity(box, 400.0f, 0, 0);
    stepN(60);
    float p[3];
    position(box, p);
    aver_phys_shutdown();
    return p[0] + 700.0f;
}

static void testSamplerPrecedence() {
    AVER_INFO("=== the terrain sampler still wins over a leaf material ===");
    const float slick = samplerSlide(0.0f), grippy = samplerSlide(1.0f);
    check(slick > grippy + 50.0f, "under the same Rubber leaf the sampler decides: friction 0 slides " + f2s(slick) + ", friction 1 slides " + f2s(grippy));
}

static void testCompoundLeaves() {
    AVER_INFO("=== a compound of two leaves with different surfaces ===");
    aver_phys_init();
    const int32_t wood = aver_phys_surface_register("Wood", 0.5f, 0.1f, 1);
    const int32_t metal = aver_phys_surface_register("Metal", 0.35f, 0.2f, 2);
    const int32_t a = aver_phys_shape_box(50, 50, 20, wood, nullptr, 0);
    const int32_t b = aver_phys_shape_box(50, 50, 20, metal, nullptr, 0);
    const int32_t kids[2] = {a, b};
    const float pos[6] = {-100, 0, 0, 100, 0, 0};
    const int32_t user[2] = {5, 6};
    const int32_t comp = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, kids, pos, nullptr, user, 2);
    const int32_t body = bodyOfShape(comp, 0, 0, 100.0f, AVER_PHYS_MOTION_STATIC, 0);

    const Detail l = rayDown(-100, 0, 400.0f), r = rayDown(100, 0, 400.0f);
    check(l.body == body && l.i[AVER_PHYS_HITI_SURFACE] == wood && l.i[AVER_PHYS_HITI_CHILD] == 0 && l.i[AVER_PHYS_HITI_CHILD_USER_DATA] == 5,
          "a ray through the first child names Wood, child 0, user data 5");
    check(r.body == body && r.i[AVER_PHYS_HITI_SURFACE] == metal && r.i[AVER_PHYS_HITI_CHILD] == 1 && r.i[AVER_PHYS_HITI_CHILD_USER_DATA] == 6,
          "and through the second, Metal, child 1, user data 6");
    check(near(r.f[AVER_PHYS_HITF_FRICTION], 0.35f, 1e-5f) && r.i[AVER_PHYS_HITI_TAG] == 2, "with Metal's numbers");

    float f[AVER_PHYS_HITF_COUNT];
    int32_t iv[AVER_PHYS_HITI_COUNT];
    check(aver_phys_subshape_info(body, l.i[AVER_PHYS_HITI_SUB_SHAPE], f, AVER_PHYS_HITF_COUNT, iv, AVER_PHYS_HITI_COUNT) == 1 &&
          iv[AVER_PHYS_HITI_SURFACE] == wood && iv[AVER_PHYS_HITI_CHILD] == 0, "subshape_info agrees with the ray");
    check(near(f[AVER_PHYS_HITF_DISTANCE_CM], 0.0f, 1e-6f), "and reports zero distance");
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("physical surfaces: registry, leaf materials, hits, contacts, ground, wheels");

    testBitIdentity();   // first, while no surface has been interned
    testIds();
    testRegistry();
    testLeafFriction();
    testPerTriangle();
    testRestitution();
    testDefer();
    testTuneIsLive();
    testBodyLabel();
    testContacts();
    testCharacterGround();
    testVehicleWheels();
    testVehicleLateralGrip();
    testSamplerPrecedence();
    testCompoundLeaves();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
