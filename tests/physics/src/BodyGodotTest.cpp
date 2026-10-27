// BodyGodotTest -- Godot's rigid-body extras (physics_body_ext_abi.h, physics_shape_builder_abi.h, G2): _integrate_forces and custom_integrator,
// constant velocity on static bodies, SeparationRayShape (as a thin capsule) and per-shape disable.
//
//   1. a world that registers nothing, or registers a callback that changes nothing, simulates bit-identically,
//   2. the direct state: what it reports, the step count, writes taking effect, no calls for a sleeping body, contacts of a monitored body,
//   3. the custom integrator: no gravity or damping, the callback's own integration, switching off restores, the refusals,
//   4. constant velocity: a belt moves what rests on it, both ways, wakes a sleeper, a turntable, a kinematic belt, read-back, the refusals,
//   5. the separation ray: where a body holding it rests, the three directions, the refusals,
//   6. per-shape disable: a static and a dynamic compound, mass kept, the whole shape, index shifts, refusals,
//   7. snapshot restore replays a callback- and belt-driven run exactly.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_areas_abi.h"
#include "aver/physics/physics_body_ext_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "aver/physics/physics_state_abi.h"

#include <cmath>
#include <cstdint>
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
static const int32_t kBadHandle = -1, kNullPointer = -2, kOutOfRange = -4, kUnsupported = -5, kInvalidArgument = -6;

static void stepN(int n) { for (int i = 0; i < n; ++i) aver_phys_step(kDt); }
static float posOf(int32_t b, int axis) { float p[3] = {0, 0, 0}; aver_phys_body_position(b, p); return p[axis]; }
static float velOf(int32_t b, int axis) { float v[3] = {0, 0, 0}; aver_phys_body_velocity(b, v); return v[axis]; }
static float originZ(int32_t b) { float p[3] = {0, 0, 0}; aver_phys_body_origin(b, p); return p[2]; }
static float massOf(int32_t b) { float m = 0; aver_phys_body_mass(b, &m); return m; }
static bool awake(int32_t b) { return aver_phys_body_is_active(b) == 1; }
static int32_t floorBox() { return aver_phys_add_static_box(0, 0, -50.0f, 4000.0f, 4000.0f, 50.0f); }   // top at z = 0

static int32_t boxShape(float hx, float hy, float hz) { return aver_phys_shape_box(hx, hy, hz, 0, nullptr, 0); }

static int32_t mutableOf(const std::vector<int32_t>& kids, const std::vector<float>& pos) {
    return aver_phys_shape_compound(AVER_PHYS_COMPOUND_MUTABLE, kids.data(), pos.data(), nullptr, nullptr, static_cast<int32_t>(kids.size()));
}

static int32_t bodyOf(int32_t shape, float x, float y, float z, int32_t motion, float mass) {
    const float o[3] = {x, y, z};
    return aver_phys_body_from_shape(shape, o, nullptr, motion, mass, nullptr, 0);
}

// ---- callbacks -------------------------------------------------------------------------------------------

struct Probe {
    int calls = 0;
    int maxContacts = 0;
    std::vector<aver_phys_direct_state> states;
    std::vector<float> actualPos, actualVel;   // 3 per call, read through the ordinary getters inside the callback
    bool keep = false;
};

static void probeCb(aver_phys_direct_state* s, int64_t user) {
    Probe* p = reinterpret_cast<Probe*>(user);
    ++p->calls;
    if (s->contactCount > p->maxContacts) p->maxContacts = s->contactCount;
    if (!p->keep) return;
    p->states.push_back(*s);
    float a[3], v[3];
    aver_phys_body_position(s->body, a);
    aver_phys_body_velocity(s->body, v);
    p->actualPos.insert(p->actualPos.end(), a, a + 3);
    p->actualVel.insert(p->actualVel.end(), v, v + 3);
}

static void pushXCb(aver_phys_direct_state* s, int64_t) { s->linearVelocity[0] = 600.0f; }

// Integrates gravity itself, the way a Godot script would with a custom integrator.
static void gravityCb(aver_phys_direct_state* s, int64_t) {
    s->linearVelocity[0] += s->totalGravity[0] * s->dt;
    s->linearVelocity[1] += s->totalGravity[1] * s->dt;
    s->linearVelocity[2] += s->totalGravity[2] * s->dt;
}

// ---- 1 ---------------------------------------------------------------------------------------------------

static std::vector<float> runStack(bool optIn) {
    aver_phys_init();
    floorBox();
    const int32_t a = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    const int32_t b = aver_phys_add_dynamic_box(10.0f, 5.0f, 260.0f, 50, 50, 50, 10.0f);
    const int32_t c = aver_phys_add_dynamic_sphere(-20.0f, 0, 600.0f, 40.0f, 5.0f);
    Probe probe;
    if (optIn) {
        for (int32_t h : {a, b, c}) {
            aver_phys_body_set_integrate_forces(h, probeCb, reinterpret_cast<int64_t>(&probe));
            aver_phys_body_set_custom_integrator(h, 0);
        }
        aver_phys_body_set_constant_velocity(a, nullptr, nullptr);
    }
    stepN(400);
    std::vector<float> out;
    for (int32_t h : {a, b, c}) { float p[3]; aver_phys_body_position(h, p); out.insert(out.end(), p, p + 3); }
    if (optIn) check(probe.calls > 0, "the registered callback ran in the opted-in run");
    aver_phys_shutdown();
    return out;
}

static void testUnchanged() {
    AVER_INFO("=== nothing registered, or a callback that changes nothing: bit-identical ===");
    const std::vector<float> plain = runStack(false), opted = runStack(true);
    bool same = plain.size() == opted.size();
    for (size_t i = 0; same && i < plain.size(); ++i) same = std::memcmp(&plain[i], &opted[i], sizeof(float)) == 0;
    check(same, "every position of a stack and a sphere is bit-identical with a read-only callback registered");
}

// ---- 2 ---------------------------------------------------------------------------------------------------

static void testDirectState() {
    AVER_INFO("=== the direct state ===");
    aver_phys_init();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 3000.0f, 50, 50, 50, 10.0f);
    Probe probe;
    probe.keep = true;
    check(aver_phys_body_set_integrate_forces(box, probeCb, reinterpret_cast<int64_t>(&probe)) == 1, "registered");
    for (int i = 0; i < 30; ++i) aver_phys_step(kDt);
    check(probe.calls == 30, "once per fixed step: " + std::to_string(probe.calls) + " calls in 30 steps");
    bool ok = !probe.states.empty();
    for (size_t i = 0; i < probe.states.size(); ++i) {
        const aver_phys_direct_state& s = probe.states[i];
        ok = ok && s.body == box && near(s.dt, kDt, 1e-6f) && near(s.invMass, 0.1f, 1e-4f) && near(s.gravityFactor, 1.0f, 1e-6f);
        ok = ok && near(s.totalGravity[0], 0.0f, 1e-3f) && near(s.totalGravity[1], 0.0f, 1e-3f) && near(s.totalGravity[2], -980.0f, 0.5f);
        for (int k = 0; k < 3; ++k)
            ok = ok && near(s.position[k], probe.actualPos[i * 3 + k], 1e-3f) && near(s.linearVelocity[k], probe.actualVel[i * 3 + k], 1e-3f);
        ok = ok && near(s.rotation[3], 1.0f, 1e-4f) && s.contactCount == 0;
    }
    check(ok, "body, dt, inverse mass, gravity, position and velocity match what the getters say");
    const aver_phys_direct_state& last = probe.states.back();
    check(last.linearVelocity[2] < -100.0f, "it was falling: vz " + f2s(last.linearVelocity[2]));
    // A cube of 1 m sides and 10 kg: I = m (w^2 + h^2) / 12 = 1.667 kg m^2 = 16667 kg cm^2.
    const float expect = 1.0f / 16667.0f;
    check(near(last.invInertia[0] / expect, 1.0f, 0.02f) && near(last.invInertia[4] / expect, 1.0f, 0.02f) && near(last.invInertia[8] / expect, 1.0f, 0.02f)
              && near(last.invInertia[1], 0.0f, expect * 0.02f),
          "inverse inertia of a cube: diagonal " + std::to_string(last.invInertia[0]) + " vs " + std::to_string(expect));

    // Gravity in another direction reads through the engine axes.
    aver_phys_set_gravity(200.0f, 0.0f, -500.0f);
    probe.states.clear(); probe.actualPos.clear(); probe.actualVel.clear();
    aver_phys_step(kDt);
    check(!probe.states.empty() && near(probe.states[0].totalGravity[0], 200.0f, 0.5f) && near(probe.states[0].totalGravity[2], -500.0f, 0.5f),
          "total gravity follows aver_phys_set_gravity");
    aver_phys_remove_body(box);

    // A long thin bar: the inertia about its long axis (engine X) is the small one.
    aver_phys_set_gravity(0, 0, -980.0f);
    const int32_t bar = aver_phys_add_dynamic_box(0, 0, 3000.0f, 100, 20, 20, 10.0f);
    Probe p2;
    p2.keep = true;
    aver_phys_body_set_integrate_forces(bar, probeCb, reinterpret_cast<int64_t>(&p2));
    aver_phys_step(kDt);
    if (!p2.states.empty()) {
        const aver_phys_direct_state& s = p2.states[0];
        check(near(s.invInertia[0] / 3.75e-4f, 1.0f, 0.03f) && near(s.invInertia[4] / 2.885e-5f, 1.0f, 0.03f) && near(s.invInertia[8] / 2.885e-5f, 1.0f, 0.03f),
              "a bar along X: invInertia xx " + std::to_string(s.invInertia[0]) + " yy " + std::to_string(s.invInertia[4]));
    } else check(false, "the bar's callback ran");
    aver_phys_shutdown();
}

static void testWrites() {
    AVER_INFO("=== writes take effect; sleeping bodies are not called; monitored contacts show ===");
    aver_phys_init();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 2000.0f, 50, 50, 50, 10.0f);
    aver_phys_body_set_integrate_forces(box, pushXCb, 0);
    stepN(60);
    check(near(posOf(box, 0), 600.0f, 40.0f), "a callback that sets vx = 600 each step moves the body about 600 cm in a second: x " + f2s(posOf(box, 0)));
    check(aver_phys_body_set_integrate_forces(box, nullptr, 0) == 1, "cleared");
    aver_phys_body_set_velocity(box, 0, 0, 0);
    stepN(10);
    check(std::fabs(velOf(box, 0)) < 1.0f, "and the push stops with the callback: vx " + f2s(velOf(box, 0)));
    aver_phys_shutdown();

    aver_phys_init();
    floorBox();
    const int32_t rest = aver_phys_add_dynamic_box(0, 0, 60.0f, 50, 50, 50, 10.0f);
    aver_phys_body_set_contact_monitor(rest, 1, 4);
    Probe probe;
    aver_phys_body_set_integrate_forces(rest, probeCb, reinterpret_cast<int64_t>(&probe));
    stepN(20);
    stepN(10);
    check(probe.maxContacts == 1, "a contact-monitored body on the floor sees contactCount 1 (max " + std::to_string(probe.maxContacts) + ")");
    stepN(400);
    check(!awake(rest), "the body fell asleep");
    const int calls = probe.calls;
    stepN(30);
    check(probe.calls == calls, "and a sleeping body is not called");
    aver_phys_shutdown();
}

// ---- 3 ---------------------------------------------------------------------------------------------------

static void testCustomIntegrator() {
    AVER_INFO("=== the custom integrator ===");
    aver_phys_init();
    const int32_t plain = aver_phys_add_dynamic_box(0, 0, 3000.0f, 20, 20, 20, 10.0f);
    const int32_t custom = aver_phys_add_dynamic_box(500.0f, 0, 3000.0f, 20, 20, 20, 10.0f);
    aver_phys_body_set_damping(custom, 0.3f, 0.4f);
    aver_phys_body_set_gravity_factor(custom, 0.5f);
    aver_phys_body_set_velocity(plain, 100.0f, 0, 0);
    aver_phys_body_set_velocity(custom, 100.0f, 0, 0);
    check(aver_phys_body_set_custom_integrator(custom, 1) == 1, "switched on");
    int32_t on = 0;
    check(aver_phys_body_custom_integrator(custom, &on) == 1 && on == 1, "reads back as on");
    float dl = -1, da = -1, gf = -1;
    aver_phys_body_damping(custom, &dl, &da);
    aver_phys_body_gravity_factor(custom, &gf);
    check(dl == 0.0f && da == 0.0f && gf == 0.0f, "damping and gravity factor are zero while it is on");
    stepN(60);
    check(near(posOf(custom, 2), 3000.0f, 0.05f), "no gravity: the custom body stays at height, z " + f2s(posOf(custom, 2)));
    check(near(posOf(custom, 0) - 500.0f, 100.0f, 0.1f), "no damping: it keeps its 100 cm/s exactly, moved " + f2s(posOf(custom, 0) - 500.0f));
    check(posOf(plain, 2) < 2700.0f, "while the ordinary body fell: z " + f2s(posOf(plain, 2)));
    check(aver_phys_body_set_custom_integrator(custom, 0) == 1, "switched off");
    aver_phys_body_damping(custom, &dl, &da);
    aver_phys_body_gravity_factor(custom, &gf);
    check(near(dl, 0.3f, 1e-6f) && near(da, 0.4f, 1e-6f) && near(gf, 0.5f, 1e-6f), "the previous damping and gravity factor are back");
    check(aver_phys_body_custom_integrator(custom, &on) == 1 && on == 0, "reads back as off");
    const float z0 = posOf(custom, 2);
    stepN(30);
    check(posOf(custom, 2) < z0 - 20.0f, "and it falls again");
    aver_phys_shutdown();

    // The callback's own integration matches the closed form for the same scheme: v += g dt, then x += v dt.
    aver_phys_init();
    const int32_t b = aver_phys_add_dynamic_box(0, 0, 3000.0f, 20, 20, 20, 10.0f);
    aver_phys_body_set_custom_integrator(b, 1);
    aver_phys_body_set_integrate_forces(b, gravityCb, 0);
    stepN(60);
    const float expected = 3000.0f - 980.0f * kDt * kDt * (60.0f * 61.0f / 2.0f);
    check(near(posOf(b, 2), expected, 0.6f), "a script integrating gravity falls the closed-form distance: z " + f2s(posOf(b, 2)) + " vs " + f2s(expected));
    aver_phys_shutdown();

    aver_phys_init();
    const int32_t wall = floorBox();
    int32_t o = 0;
    check(aver_phys_body_set_custom_integrator(wall, 1) == 0 && aver_phys_last_error() == kUnsupported, "a static body is refused");
    check(aver_phys_body_set_integrate_forces(wall, probeCb, 0) == 0 && aver_phys_last_error() == kUnsupported, "so is a callback on it");
    check(aver_phys_body_set_custom_integrator(999999, 1) == 0 && aver_phys_body_set_integrate_forces(999999, probeCb, 0) == 0, "a dead handle is refused");
    check(aver_phys_body_custom_integrator(wall, nullptr) == 0 && aver_phys_last_error() == kNullPointer, "a null out pointer is refused");
    check(aver_phys_body_custom_integrator(wall, &o) == 1 && o == 0, "a static body reads as off");
    aver_phys_shutdown();
}

// ---- 4 ---------------------------------------------------------------------------------------------------

static int32_t beltBox(float friction) {
    const int32_t belt = aver_phys_add_static_box(0, 0, -50.0f, 800.0f, 200.0f, 50.0f);   // top at z = 0
    aver_phys_body_set_friction(belt, friction);
    return belt;
}

static int32_t crate(float x, float y, float z) {
    const int32_t c = aver_phys_add_dynamic_box(x, y, z, 25, 25, 25, 10.0f);
    aver_phys_body_set_friction(c, 1.0f);
    return c;
}

static void testConveyor() {
    AVER_INFO("=== constant velocity on a static body ===");
    aver_phys_init();
    const int32_t belt = beltBox(1.0f);
    const int32_t onBelt = crate(0, 0, 27.0f);
    const float bx0 = posOf(belt, 0);
    const float lin[3] = {200.0f, 0, 0};
    check(aver_phys_body_set_constant_velocity(belt, lin, nullptr) == 1, "set on a static box");
    stepN(180);
    check(near(velOf(onBelt, 0), 200.0f, 25.0f), "a crate on it reaches the belt speed: vx " + f2s(velOf(onBelt, 0)));
    check(posOf(onBelt, 0) > 150.0f && near(posOf(onBelt, 1), 0.0f, 15.0f), "and has been carried along +X: x " + f2s(posOf(onBelt, 0)));
    check(posOf(belt, 0) == bx0, "the belt itself did not move");
    float l[3] = {9, 9, 9}, a[3] = {9, 9, 9};
    check(aver_phys_body_constant_velocity(belt, l, a) == 1 && near(l[0], 200.0f, 1e-3f) && l[1] == 0 && l[2] == 0 && a[0] == 0 && a[1] == 0 && a[2] == 0,
          "it reads back");
    const float rev[3] = {-200.0f, 0, 0};
    aver_phys_body_set_constant_velocity(belt, rev, nullptr);
    stepN(150);
    check(velOf(onBelt, 0) < -100.0f, "reversed it carries the crate back: vx " + f2s(velOf(onBelt, 0)));
    aver_phys_body_set_constant_velocity(belt, nullptr, nullptr);
    aver_phys_body_constant_velocity(belt, l, a);
    check(l[0] == 0.0f && a[2] == 0.0f, "NULL and NULL removes it");
    stepN(120);
    check(std::fabs(velOf(onBelt, 0)) < 8.0f, "and the crate slows to rest: vx " + f2s(velOf(onBelt, 0)));
    aver_phys_shutdown();

    // The same crate on a belt with no value stays put.
    aver_phys_init();
    beltBox(1.0f);
    const int32_t still = crate(0, 0, 27.0f);
    stepN(180);
    check(std::fabs(velOf(still, 0)) < 1.0f && std::fabs(posOf(still, 0)) < 3.0f, "without a value nothing moves");
    aver_phys_shutdown();

    // A sleeping crate is woken when the belt starts.
    aver_phys_init();
    const int32_t belt2 = beltBox(1.0f);
    const int32_t sleeper = crate(0, 0, 27.0f);
    stepN(300);
    check(!awake(sleeper), "the crate on a still belt fell asleep");
    const float lin2[3] = {200.0f, 0, 0};
    aver_phys_body_set_constant_velocity(belt2, lin2, nullptr);
    stepN(1);
    check(awake(sleeper), "starting the belt wakes it");
    stepN(120);
    check(velOf(sleeper, 0) > 100.0f, "and carries it: vx " + f2s(velOf(sleeper, 0)));
    float p[3] = {1, 1, 1};
    check(aver_phys_body_set_constant_velocity(sleeper, lin2, p) == 0 && aver_phys_last_error() == kUnsupported, "a dynamic body is refused");
    const float bad[3] = {NAN, 0, 0};
    check(aver_phys_body_set_constant_velocity(belt2, bad, nullptr) == 0 && aver_phys_last_error() == kInvalidArgument, "a non-finite value is refused");
    check(aver_phys_body_set_constant_velocity(999999, lin2, nullptr) == 0 && aver_phys_body_constant_velocity(999999, l, a) == 0, "a dead handle is refused");
    aver_phys_shutdown();

    // A kinematic belt works too.
    aver_phys_init();
    const int32_t kin = aver_phys_add_dynamic_box(0, 0, -50.0f, 800.0f, 200.0f, 50.0f, 1000.0f);
    aver_phys_body_set_motion_type(kin, AVER_PHYS_MOTION_KINEMATIC);
    aver_phys_body_set_friction(kin, 1.0f);
    const int32_t k = crate(0, 0, 27.0f);
    const float linK[3] = {0, 150.0f, 0};
    check(aver_phys_body_set_constant_velocity(kin, linK, nullptr) == 1, "set on a kinematic box");
    stepN(180);
    check(velOf(k, 1) > 100.0f && std::fabs(velOf(k, 0)) < 15.0f, "it carries a crate along +Y: vy " + f2s(velOf(k, 1)));
    aver_phys_shutdown();
}

// Position and velocity of a crate on a plate that turns (static + constant angular velocity, or a kinematic plate), after a second.
static void turntable(bool kinematic, float& angularMomentumZ, float& speed) {
    aver_phys_init();
    const float w[3] = {0, 0, 1.0f};
    int32_t plate = 0;
    if (kinematic) {
        plate = aver_phys_add_dynamic_box(0, 0, -25.0f, 300, 300, 25, 1000.0f);
        aver_phys_body_set_motion_type(plate, AVER_PHYS_MOTION_KINEMATIC);
        aver_phys_body_set_angular_velocity(plate, 0, 0, 1.0f);
    } else {
        plate = aver_phys_add_static_box(0, 0, -25.0f, 300, 300, 25);
        aver_phys_body_set_constant_velocity(plate, nullptr, w);
    }
    aver_phys_body_set_friction(plate, 1.0f);
    const int32_t c = aver_phys_add_dynamic_box(150.0f, 0, 21.0f, 20, 20, 20, 5.0f);
    aver_phys_body_set_friction(c, 1.0f);
    stepN(60);
    float pp[3], v[3];
    aver_phys_body_position(c, pp);
    aver_phys_body_velocity(c, v);
    angularMomentumZ = pp[0] * v[1] - pp[1] * v[0];
    speed = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    aver_phys_shutdown();
}

static void testTurntable() {
    AVER_INFO("=== constant angular velocity: a turntable ===");
    float lzS = 0, spS = 0, lzK = 0, spK = 0;
    turntable(false, lzS, spS);
    turntable(true, lzK, spK);
    check(lzS * lzK > 0.0f, "the crate circles the same way as on a kinematic plate turned at the same rate: Lz " + f2s(lzS) + " and " + f2s(lzK));
    check(spS > 100.0f && spS < 200.0f, "at about w * r = 150 cm/s: " + f2s(spS) + " (kinematic " + f2s(spK) + ")");
}

// ---- 5 ---------------------------------------------------------------------------------------------------

// First hit of a ray, as a distance along it (or -1).
static float rayDistance(float ox, float oy, float oz, float dx, float dy, float dz) {
    float pt[3] = {0, 0, 0}, nrm[3] = {0, 0, 0};
    int32_t e = 0;
    if (!aver_phys_raycast(ox, oy, oz, dx, dy, dz, 2000.0f, pt, nrm, &e)) return -1.0f;
    return std::sqrt((pt[0] - ox) * (pt[0] - ox) + (pt[1] - oy) * (pt[1] - oy) + (pt[2] - oz) * (pt[2] - oz));
}

static void testSeparationRay() {
    AVER_INFO("=== the separation ray (a thin capsule) ===");
    aver_phys_init();
    floorBox();
    const float down[3] = {0, 0, -1.0f};
    const int32_t ray = aver_phys_shape_separation_ray(100.0f, down, 0.0f, 0);
    check(ray > 0, "built");
    float f[32] = {};   // AVER_PHYS_SI_COUNT is defined by both the shape and the snapshot headers; the shape one is 19
    int32_t iv[16] = {};
    aver_phys_shape_info(ray, f, 32, iv, 16);
    check(f[AVER_PHYS_SI_MASS_KG] < 1e-3f, "it weighs next to nothing: " + std::to_string(f[AVER_PHYS_SI_MASS_KG]) + " kg");

    const int32_t box = boxShape(25, 25, 25);
    // Alone the box rests at its half height; with a ray 100 cm long at each corner (four legs: a single one would be a pole to balance on) it rests on the tips.
    const std::vector<int32_t> kids = {box, ray, ray, ray, ray};
    const std::vector<float> pos = {0, 0, 0, 20, 20, 0, -20, 20, 0, 20, -20, 0, -20, -20, 0};
    const int32_t comp = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, kids.data(), pos.data(), nullptr, nullptr, 5);
    const int32_t legged = bodyOf(comp, 0, 0, 400.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    const int32_t bare = bodyOf(box, 300.0f, 0, 400.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    stepN(500);
    check(near(originZ(legged), 100.0f, 3.0f), "a body holding four 100 cm rays down rests with its origin 100 cm up: " + f2s(originZ(legged)));
    check(near(originZ(bare), 25.0f, 3.0f), "the same box without it rests at 25: " + f2s(originZ(bare)));
    check(near(massOf(legged), 10.0f, 0.01f), "the ray added no mass: " + f2s(massOf(legged)) + " kg");
    aver_phys_shutdown();

    // The three directions, read with rays at the tips.
    aver_phys_init();
    const float dx[3] = {1.0f, 0, 0}, dy[3] = {0, 1.0f, 0}, dz[3] = {0, 0, 1.0f}, dm[3] = {0, -2.0f, 0};
    const int32_t rx = aver_phys_shape_separation_ray(200.0f, dx, 0.0f, 0);
    const int32_t ry = aver_phys_shape_separation_ray(100.0f, dy, 0.0f, 0);
    const int32_t rz = aver_phys_shape_separation_ray(100.0f, dz, 0.0f, 0);
    const int32_t rm = aver_phys_shape_separation_ray(100.0f, dm, 0.0f, 0);   // not unit: normalised
    bodyOf(rx, 0, 0, 500.0f, AVER_PHYS_MOTION_STATIC, 0);
    bodyOf(ry, 1000.0f, 0, 500.0f, AVER_PHYS_MOTION_STATIC, 0);
    bodyOf(rz, 0, 1000.0f, 500.0f, AVER_PHYS_MOTION_STATIC, 0);
    bodyOf(rm, 0, -1000.0f, 500.0f, AVER_PHYS_MOTION_STATIC, 0);
    check(near(rayDistance(600.0f, 0, 500.0f, -1, 0, 0), 400.0f, 1.5f), "+X, 200 cm: a ray from x = 600 reaches the tip at x = 200");
    check(near(rayDistance(-300.0f, 0, 500.0f, 1, 0, 0), 300.0f, 1.5f), "and the other end sits at the shape origin");
    check(near(rayDistance(1000.0f, 400.0f, 500.0f, 0, -1, 0), 300.0f, 1.5f), "+Y, 100 cm: the tip is at y = 100");
    check(near(rayDistance(0, 1000.0f, 900.0f, 0, 0, -1), 300.0f, 1.5f), "+Z, 100 cm: the tip is at z = 600");
    check(near(rayDistance(0, -1400.0f, 500.0f, 0, 1, 0), 300.0f, 1.5f), "-Y from a non-unit direction: the tip is at y = -1100");
    aver_phys_shutdown();

    aver_phys_init();
    check(aver_phys_shape_separation_ray(100.0f, nullptr, 0.0f, 0) == 0 && aver_phys_last_error() == kNullPointer, "a null direction is refused");
    check(aver_phys_shape_separation_ray(0.1f, down, 0.0f, 0) == 0 && aver_phys_last_error() == kInvalidArgument, "a ray shorter than its caps is refused");
    const float zero[3] = {0, 0, 0};
    check(aver_phys_shape_separation_ray(100.0f, zero, 0.0f, 0) == 0 && aver_phys_last_error() == kInvalidArgument, "a zero direction is refused");
    aver_phys_shutdown();
}

// ---- 6 ---------------------------------------------------------------------------------------------------

static int32_t disabledOf(int32_t body, int32_t index) {
    int32_t out = -1;
    return aver_phys_body_shape_disabled(body, index, &out) == 1 ? out : -1;
}

static void testDisableStatic() {
    AVER_INFO("=== per-shape disable: a static platform ===");
    aver_phys_init();
    floorBox();
    const int32_t slab = boxShape(100, 100, 10);
    const int32_t comp = mutableOf({slab, slab}, {-150.0f, 0, 0, 150.0f, 0, 0});
    const int32_t plat = bodyOf(comp, 0, 0, 100.0f, AVER_PHYS_MOTION_STATIC, 0);
    check(aver_phys_body_compound_count(plat) == 2 && disabledOf(plat, 1) == 0, "two children, none disabled");
    check(near(rayDistance(150.0f, 0, 500.0f, 0, 0, -1), 390.0f, 1.0f), "a ray from above hits the slab top at z = 110");
    check(aver_phys_body_set_shape_disabled(plat, 1, 1) == 1, "child 1 disabled");
    check(disabledOf(plat, 1) == 1 && disabledOf(plat, 0) == 0 && aver_phys_body_compound_count(plat) == 2, "it reads back, the other is untouched, the count stays");
    check(near(rayDistance(150.0f, 0, 500.0f, 0, 0, -1), 500.0f, 1.0f), "the ray now goes through to the floor");
    check(near(rayDistance(-150.0f, 0, 500.0f, 0, 0, -1), 390.0f, 1.0f), "while the other child is where it was");
    const int32_t s1 = aver_phys_add_dynamic_sphere(150.0f, 0, 400.0f, 15.0f, 5.0f);
    const int32_t s0 = aver_phys_add_dynamic_sphere(-150.0f, 0, 400.0f, 15.0f, 5.0f);
    stepN(300);
    check(near(posOf(s1, 2), 15.0f, 3.0f), "a sphere over the disabled child falls to the floor: z " + f2s(posOf(s1, 2)));
    check(near(posOf(s0, 2), 125.0f, 3.0f), "one over the enabled child rests on it: z " + f2s(posOf(s0, 2)));
    check(aver_phys_body_set_shape_disabled(plat, 1, 1) == 1, "disabling again is a no-op");
    check(aver_phys_body_set_shape_disabled(plat, 1, 0) == 1 && disabledOf(plat, 1) == 0, "enabled again");
    check(near(rayDistance(150.0f, 0, 500.0f, 0, 0, -1), 390.0f, 1.0f), "the slab is back where it was");
    aver_phys_remove_body(s1);
    const int32_t s2 = aver_phys_add_dynamic_sphere(150.0f, 0, 400.0f, 15.0f, 5.0f);
    stepN(300);
    check(near(posOf(s2, 2), 125.0f, 3.0f), "and holds a sphere again: z " + f2s(posOf(s2, 2)));
    check(aver_phys_body_set_shape_disabled(plat, 2, 1) == 0 && aver_phys_last_error() == kOutOfRange, "an index past the end is refused");
    check(aver_phys_body_set_shape_disabled(999999, 0, 1) == 0, "a dead handle is refused");
    aver_phys_shutdown();
}

static void testDisableDynamic() {
    AVER_INFO("=== per-shape disable: a dynamic compound keeps its mass ===");
    aver_phys_init();
    floorBox();
    const int32_t b = boxShape(25, 25, 25);
    // A box with another box hanging 100 cm below it.
    const int32_t comp = mutableOf({b, b}, {0, 0, 0, 0, 0, -100.0f});
    const int32_t body = bodyOf(comp, 0, 0, 500.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    stepN(10);
    const float m0 = massOf(body);
    check(aver_phys_body_set_shape_disabled(body, 1, 1) == 1, "the lower child is disabled in flight");
    check(near(massOf(body), m0, 0.01f) && near(m0, 10.0f, 0.01f), "the body's mass is kept: " + f2s(massOf(body)) + " kg");
    stepN(400);
    check(near(originZ(body), 25.0f, 3.0f), "it lands on the upper box: origin z " + f2s(originZ(body)));
    aver_phys_shutdown();

    aver_phys_init();
    floorBox();
    const int32_t b2 = boxShape(25, 25, 25);
    const int32_t comp2 = mutableOf({b2, b2}, {0, 0, 0, 0, 0, -100.0f});
    const int32_t both = bodyOf(comp2, 0, 0, 500.0f, AVER_PHYS_MOTION_DYNAMIC, 10.0f);
    stepN(400);
    check(near(originZ(both), 125.0f, 3.0f), "enabled, the lower box carries it: origin z " + f2s(originZ(both)));
    aver_phys_shutdown();
}

static void testDisableWhole() {
    AVER_INFO("=== per-shape disable: the whole shape ===");
    aver_phys_init();
    floorBox();
    const int32_t box = aver_phys_add_dynamic_box(0, 0, 500.0f, 25, 25, 25, 10.0f);
    check(disabledOf(box, -1) == 0 && disabledOf(box, 0) == 0, "not disabled");
    stepN(5);
    check(aver_phys_body_set_shape_disabled(box, -1, 1) == 1 && disabledOf(box, 0) == 1, "the whole shape is disabled in flight");
    check(near(massOf(box), 10.0f, 0.01f), "its mass is kept: " + f2s(massOf(box)));
    const int32_t other = boxShape(10, 10, 10);
    check(aver_phys_body_set_shape(box, other, 1) == 0 && aver_phys_last_error() == kUnsupported, "set_shape is refused meanwhile");
    check(aver_phys_body_set_shape_disabled(box, 1, 1) == 0 && aver_phys_last_error() == kOutOfRange, "index 1 of a single shape is out of range");
    stepN(5);
    check(aver_phys_body_set_shape_disabled(box, 0, 0) == 1 && disabledOf(box, -1) == 0, "enabled again (index 0 is the whole shape)");
    stepN(400);
    check(near(posOf(box, 2), 25.0f, 3.0f), "and it lands on the floor: z " + f2s(posOf(box, 2)));
    aver_phys_shutdown();

    // A disabled floor lets things through.
    aver_phys_init();
    const int32_t fl = floorBox();
    const int32_t ball = aver_phys_add_dynamic_sphere(0, 0, 200.0f, 20.0f, 5.0f);
    aver_phys_body_set_shape_disabled(fl, -1, 1);
    stepN(120);
    check(posOf(ball, 2) < -100.0f, "a sphere falls through a disabled static floor: z " + f2s(posOf(ball, 2)));
    aver_phys_shutdown();
}

static void testDisableIndices() {
    AVER_INFO("=== per-shape disable: indices, static compounds, set_shape ===");
    aver_phys_init();
    floorBox();
    const int32_t b = boxShape(20, 20, 20);
    const int32_t comp = mutableOf({b, b, b}, {0, 0, 0, 100.0f, 0, 0, 200.0f, 0, 0});
    const int32_t body = bodyOf(comp, 0, 0, 500.0f, AVER_PHYS_MOTION_STATIC, 0);
    aver_phys_body_set_shape_disabled(body, 2, 1);
    check(aver_phys_body_compound_remove(body, 0) == 1 && aver_phys_body_compound_count(body) == 2, "child 0 removed");
    check(disabledOf(body, 1) == 1 && disabledOf(body, 0) == 0, "the disabled child is index 1 now");
    check(aver_phys_body_compound_remove(body, 1) == 1 && aver_phys_body_compound_count(body) == 1 && disabledOf(body, 0) == 0, "removing the disabled child forgets it");

    // set_shape forgets the set.
    const int32_t comp2 = mutableOf({b, b}, {0, 0, 0, 100.0f, 0, 0});
    const int32_t body2 = bodyOf(comp2, 0, 1000.0f, 500.0f, AVER_PHYS_MOTION_STATIC, 0);
    aver_phys_body_set_shape_disabled(body2, 1, 1);
    const int32_t comp3 = mutableOf({b, b}, {0, 0, 0, 100.0f, 0, 0});
    check(aver_phys_body_set_shape(body2, comp3, 1) == 1 && disabledOf(body2, 1) == 0, "set_shape starts from a clean set");

    // A static compound cannot be edited, but its whole shape can be switched.
    const int32_t sc = aver_phys_shape_compound(AVER_PHYS_COMPOUND_STATIC, std::vector<int32_t>{b, b}.data(), std::vector<float>{0, 0, 0, 100.0f, 0, 0}.data(), nullptr, nullptr, 2);
    const int32_t body3 = bodyOf(sc, 0, 2000.0f, 500.0f, AVER_PHYS_MOTION_STATIC, 0);
    check(aver_phys_body_set_shape_disabled(body3, 0, 1) == 0 && aver_phys_last_error() == kUnsupported, "a child of a static compound is refused");
    check(aver_phys_body_set_shape_disabled(body3, -1, 1) == 1 && disabledOf(body3, -1) == 1, "the whole static compound can be disabled");
    check(near(rayDistance(0, 2000.0f, 900.0f, 0, 0, -1), 900.0f, 1.0f), "and a ray goes through it");
    aver_phys_body_set_shape_disabled(body3, -1, 0);
    check(near(rayDistance(0, 2000.0f, 900.0f, 0, 0, -1), 380.0f, 1.0f), "enabled, the ray hits it at z = 520");
    check(aver_phys_body_shape_disabled(body3, 0, nullptr) == 0 && aver_phys_last_error() == kNullPointer, "a null out pointer is refused");
    (void)kBadHandle;
    aver_phys_shutdown();
}

// ---- 7 ---------------------------------------------------------------------------------------------------

static void testRestore() {
    AVER_INFO("=== a snapshot restore replays a callback- and belt-driven run exactly ===");
    aver_phys_init();
    const int32_t belt = beltBox(1.0f);
    const float lin[3] = {150.0f, 0, 0};
    aver_phys_body_set_constant_velocity(belt, lin, nullptr);
    const int32_t c = crate(0, 0, 40.0f);
    const int32_t floaty = aver_phys_add_dynamic_box(0, 300.0f, 800.0f, 20, 20, 20, 5.0f);
    aver_phys_body_set_custom_integrator(floaty, 1);
    aver_phys_body_set_integrate_forces(floaty, gravityCb, 0);
    stepN(40);
    const int32_t snap = aver_phys_snapshot_capture(0, 0);
    stepN(60);
    float p1[6], p2[6];
    aver_phys_body_position(c, p1); aver_phys_body_position(floaty, p1 + 3);
    check(aver_phys_snapshot_restore(snap, 0) == 1, "restored");
    stepN(60);
    aver_phys_body_position(c, p2); aver_phys_body_position(floaty, p2 + 3);
    check(std::memcmp(p1, p2, sizeof p1) == 0, "the same 60 steps land on the same positions, bit for bit: x " + f2s(p1[0]) + " and " + f2s(p2[0]) + ", z " + f2s(p1[5]) + " and " + f2s(p2[5]));
    aver_phys_snapshot_release_all();
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("Godot rigid-body extras (G2)");

    testUnchanged();
    testDirectState();
    testWrites();
    testCustomIntegrator();
    testConveyor();
    testTurntable();
    testSeparationRay();
    testDisableStatic();
    testDisableDynamic();
    testDisableWhole();
    testDisableIndices();
    testRestore();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
