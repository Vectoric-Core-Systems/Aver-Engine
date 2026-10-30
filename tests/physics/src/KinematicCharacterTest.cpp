// KinematicCharacterTest -- move_and_slide as a controller on a kinematic rigid body (physics_kinematic_abi.h). Through the
// ABI only. Scenes are built from static boxes, off-axis (walls on +X and +Y, slopes tilted about Y) and the character is driven the way a script
// drives one: set the horizontal velocity, add gravity while not on the floor, call move_and_slide. Expected numbers are arithmetic on the scene.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_kinematic_abi.h"
#include "aver/physics/physics_layers_abi.h"
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
static bool near(float a, float b, float tol = 0.6f) { return std::fabs(a - b) <= tol; }

static const float kDt = 1.0f / 60.0f;
static const float kGravity = 980.0f;
static const float kRadius = 30.0f, kHeight = 180.0f;   // total height: the centre rests 90 cm above the floor

static int32_t box(float x, float y, float z, float hx, float hy, float hz) { return aver_phys_add_static_box(x, y, z, hx, hy, hz); }

// A static box tilted by `deg` about the engine's +Y axis, its top surface passing through (cx, 0, cz).
static int32_t tiltedBox(float cx, float cz, float deg) {
    const float a = deg * 3.14159265f / 180.0f;
    const float q[4] = {0, std::sin(a * 0.5f), 0, std::cos(a * 0.5f)};
    const int32_t shape = aver_phys_shape_box(600, 400, 10, 0, nullptr, 0);
    const float o[3] = {cx, 0, cz};
    return aver_phys_body_from_shape(shape, o, q, AVER_PHYS_MOTION_STATIC, 0, nullptr, 0);
}

struct Char { int32_t body = 0, kc = 0; };

static Char makeChar(float x, float y, float z) {
    Char c;
    const int32_t shape = aver_phys_shape_capsule(kRadius, kHeight, 0, nullptr, 0);
    const float o[3] = {x, y, z};
    c.body = aver_phys_body_from_shape(shape, o, nullptr, AVER_PHYS_MOTION_KINEMATIC, 0, nullptr, 0);
    c.kc = aver_phys_kc_create(c.body);
    return c;
}

static void pos(const Char& c, float* p) { aver_phys_body_origin(c.body, p); }
static int32_t state(const Char& c) { int32_t s = 0; aver_phys_kc_state(c.kc, &s); return s; }
static bool onFloor(const Char& c) { return (state(c) & AVER_PHYS_KCS_FLOOR) != 0; }
static bool onWall(const Char& c) { return (state(c) & AVER_PHYS_KCS_WALL) != 0; }
static bool onCeiling(const Char& c) { return (state(c) & AVER_PHYS_KCS_CEILING) != 0; }
static void vel(const Char& c, float* v) { aver_phys_kc_velocity(c.kc, v); }

// One frame: horizontal input, gravity while airborne, move_and_slide.
static void tick(const Char& c, float vx, float vy, bool gravity = true) {
    float v[3];
    vel(c, v);
    v[0] = vx;
    v[1] = vy;
    if (gravity && !onFloor(c)) v[2] -= kGravity * kDt;
    aver_phys_kc_set_velocity(c.kc, v);
    aver_phys_kc_move_and_slide(c.kc, kDt, nullptr);
}

// A script that adds gravity every frame and never touches the horizontal velocity (the common idiom).
static void idle(const Char& c, int n) {
    for (int i = 0; i < n; ++i) {
        float v[3];
        vel(c, v);
        v[2] -= kGravity * kDt;
        aver_phys_kc_set_velocity(c.kc, v);
        aver_phys_kc_move_and_slide(c.kc, kDt, nullptr);
    }
}

static void ticks(const Char& c, int n, float vx, float vy, bool gravity = true) { for (int i = 0; i < n; ++i) tick(c, vx, vy, gravity); }

static void info(const Char& c, float* f, int32_t* i) { aver_phys_kc_info(c.kc, f, AVER_PHYS_KCI_F_COUNT, i, AVER_PHYS_KCI_I_COUNT); }

// ---------------------------------------------------------------------------------------------------

static void testFloorWallCeiling() {
    AVER_INFO("=== floor / wall / ceiling detection ===");
    aver_phys_init();
    const int32_t ground = box(0, 0, -10, 2000, 2000, 10);          // top at z = 0
    const int32_t wallX = box(520, 0, 200, 10, 600, 200);           // face at x = 510
    const int32_t wallY = box(0, 1200, 200, 800, 10, 200);          // face at y = 1190
    Char c = makeChar(0, 0, 130);                                     // 40 cm above the rest height
    check(c.kc != 0, "a controller is created on a kinematic capsule");

    ticks(c, 30, 0, 0);
    float p[3];
    pos(c, p);
    check(onFloor(c) && !onWall(c) && !onCeiling(c), "falling onto the ground: is_on_floor only");
    check(near(p[2], 90.0f, 0.3f), "the centre rests 90 cm up (plus the margin), got " + f2s(p[2]));
    float f[AVER_PHYS_KCI_F_COUNT];
    int32_t iv[AVER_PHYS_KCI_I_COUNT];
    info(c, f, iv);
    check(near(f[AVER_PHYS_KCI_F_FLOOR_NORMAL + 2], 1.0f, 0.01f) && iv[AVER_PHYS_KCI_I_PLATFORM_BODY] == ground, "floor normal (0,0,1), the ground is the platform");
    check(near(f[AVER_PHYS_KCI_F_WALL_NORMAL], 0.0f, 1e-4f), "no wall normal");
    float v[3];
    vel(c, v);
    check(std::fabs(v[2]) < 0.01f, "the fall velocity is gone once on the floor, got " + f2s(v[2]));

    // Walking on the ground at 300 cm/s for half a second.
    float x0 = p[0];
    ticks(c, 30, 300, 0);
    pos(c, p);
    check(near(p[0] - x0, 150.0f, 1.0f) && onFloor(c), "walking 30 frames at 300 cm/s covers 150 cm and stays on the floor, got " + f2s(p[0] - x0));
    check(near(p[2], 90.0f, 0.3f), "at a constant height");

    // The wall on +X: face at 510, so the centre stops at 480 (the radius is 30).
    ticks(c, 100, 300, 0);
    pos(c, p);
    check(near(p[0], 480.0f, 0.5f), "walking into the wall stops at x = 480, got " + f2s(p[0]));
    check(onWall(c) && onFloor(c) && !onCeiling(c), "is_on_wall and is_on_floor");
    info(c, f, iv);
    check(near(f[AVER_PHYS_KCI_F_WALL_NORMAL], -1.0f, 0.02f), "wall normal (-1,0,0)");
    check(iv[AVER_PHYS_KCI_I_COLLISIONS] >= 1, "a slide collision was recorded");

    // Sliding along the wall: pushing +X and +Y keeps the +Y motion and loses the +X.
    float y0 = p[1];
    ticks(c, 30, 300, 200);
    pos(c, p);
    check(near(p[0], 480.0f, 0.5f), "sliding along the wall does not enter it, x = " + f2s(p[0]));
    check(near(p[1] - y0, 100.0f, 2.0f), "and keeps the lateral 200 cm/s: 100 cm in half a second, got " + f2s(p[1] - y0));
    check(onWall(c) && onFloor(c), "still on the wall and the floor");
    aver_phys_kc_collision(c.kc, 0, f, AVER_PHYS_KCC_F_COUNT, iv, AVER_PHYS_KCC_I_COUNT);
    check(iv[AVER_PHYS_KCC_I_COLLIDER] == wallX && near(f[AVER_PHYS_KCC_F_NORMAL], -1.0f, 0.02f), "collision 0 is the +X wall with its normal");
    check(near(f[AVER_PHYS_KCC_F_ANGLE], 3.14159265f / 2.0f, 0.02f), "whose angle to up is 90 degrees, got " + f2s(f[AVER_PHYS_KCC_F_ANGLE]));

    // The wall on +Y, reached from the side.
    ticks(c, 240, 0, 300);
    pos(c, p);
    check(near(p[1], 1160.0f, 0.5f), "walking +Y stops at the second wall (face 1190): y = " + f2s(p[1]));
    info(c, f, iv);
    check(near(f[AVER_PHYS_KCI_F_WALL_NORMAL + 1], -1.0f, 0.02f), "its normal is (0,-1,0)");
    (void)wallY;

    // Ceiling: a slab over the spot at x = 200, y = 0; its underside is at z = 200, the capsule top at 180.
    box(200, -300, 210, 100, 100, 10);
    Char d = makeChar(200, -300, 90.2f);
    ticks(d, 4, 0, 0);
    check(onFloor(d), "(second character) starts on the floor");
    float vv[3] = {0, 0, 600};
    aver_phys_kc_set_velocity(d.kc, vv);
    bool sawCeiling = false;
    for (int i = 0; i < 10 && !sawCeiling; ++i) {
        aver_phys_kc_move_and_slide(d.kc, kDt, nullptr);
        sawCeiling = onCeiling(d);
    }
    pos(d, p);
    check(sawCeiling && !onFloor(d), "jumping into the slab: is_on_ceiling and not on the floor");
    check(p[2] < 110.05f && p[2] > 108.0f, "the head stops at the slab: centre z = 110, got " + f2s(p[2]));
    vel(d, v);
    check(v[2] < 1.0f, "the upward velocity is killed, got " + f2s(v[2]));
    aver_phys_shutdown();
}

static void testSlopes() {
    AVER_INFO("=== slopes: stop on slope, max angle, constant speed, block on wall ===");
    aver_phys_init();
    tiltedBox(0, -10.0f, 30.0f);   // the top surface passes through (0,0,0) tilted 30 degrees
    Char c = makeChar(0, 0, 130);
    ticks(c, 40, 0, 0);
    float p[3], f[AVER_PHYS_KCI_F_COUNT];
    int32_t iv[AVER_PHYS_KCI_I_COUNT];
    pos(c, p);
    info(c, f, iv);
    check(onFloor(c) && !onWall(c), "a 30 degree slope is floor (max angle 45)");
    check(near(f[AVER_PHYS_KCI_F_FLOOR_NORMAL + 2], std::cos(30.0f * 3.14159265f / 180.0f), 0.02f), "floor normal z = cos 30 = 0.866, got " + f2s(f[AVER_PHYS_KCI_F_FLOOR_NORMAL + 2]));

    // Stop on slope: standing with the script's gravity, the body does not slide.
    float p0[3] = {p[0], p[1], p[2]};
    idle(c, 90);
    pos(c, p);
    float drift = std::sqrt((p[0] - p0[0]) * (p[0] - p0[0]) + (p[2] - p0[2]) * (p[2] - p0[2]));
    check(drift < 0.5f && onFloor(c), "floor_stop_on_slope on: 1.5 s on the slope moves it " + f2s(drift) + " cm");

    // Off: it slides downhill. Gravity that is not cancelled by the stop keeps it moving.
    aver_phys_kc_set_param(c.kc, AVER_PHYS_KCP_FLOOR_STOP_ON_SLOPE, 0);
    pos(c, p0);
    idle(c, 60);
    pos(c, p);
    drift = std::sqrt((p[0] - p0[0]) * (p[0] - p0[0]) + (p[2] - p0[2]) * (p[2] - p0[2]));
    check(drift > 10.0f, "floor_stop_on_slope off: gravity carries it downhill, moved " + f2s(drift) + " cm");
    aver_phys_shutdown();

    // Max angle: 60 degrees is a wall at 45 and a floor at 70.
    aver_phys_init();
    tiltedBox(0, -10.0f, 60.0f);
    Char s = makeChar(0, 0, 130);
    ticks(s, 30, 0, 0);
    check(!onFloor(s) && onWall(s), "a 60 degree slope is a wall with floor_max_angle 45");
    aver_phys_kc_set_param(s.kc, AVER_PHYS_KCP_FLOOR_MAX_ANGLE, 70.0f * 3.14159265f / 180.0f);
    aver_phys_kc_clear(s.kc);
    aver_phys_body_set_position(s.body, 0, 0, 130);
    float zero[3] = {0, 0, 0};
    aver_phys_kc_set_velocity(s.kc, zero);
    ticks(s, 40, 0, 0);
    check(onFloor(s), "and a floor with floor_max_angle 70");
    aver_phys_shutdown();

    // Constant speed: walking uphill covers the same distance as on the flat only when asked.
    float speedOff = 0, speedOn = 0;
    for (int mode = 0; mode < 2; ++mode) {
        for (int dir = -1; dir <= 1; dir += 2) {
            aver_phys_init();
            tiltedBox(0, -10.0f, 30.0f);
            Char u = makeChar(0, 0, 130);
            aver_phys_kc_set_param(u.kc, AVER_PHYS_KCP_FLOOR_CONSTANT_SPEED, static_cast<float>(mode));
            ticks(u, 40, 0, 0);
            float a[3], b[3];
            pos(u, a);
            ticks(u, 30, 300.0f * dir, 0);
            pos(u, b);
            if (b[2] - a[2] > 20.0f) {   // this direction is uphill
                const float dist = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]));
                (mode ? speedOn : speedOff) = dist / 0.5f;
            }
            aver_phys_shutdown();
        }
    }
    // Block on wall: walking from the ground into a 60 degree ramp does not climb it; with the option off the body slides up.
    float downhill = 0;
    {
        aver_phys_init();
        tiltedBox(0, -10.0f, 60.0f);
        Char u = makeChar(0, 0, 130);
        ticks(u, 40, 0, 0);
        float q[3];
        pos(u, q);
        downhill = q[0] < 0 ? -1.0f : 1.0f;
        aver_phys_shutdown();
    }
    float climb[2] = {0, 0};
    for (int blockOn = 0; blockOn < 2; ++blockOn) {
        aver_phys_init();
        box(0, 0, -10, 2000, 2000, 10);
        tiltedBox(0, -10.0f, 60.0f);
        Char u = makeChar(downhill * 250.0f, 0, 90.2f);
        aver_phys_kc_set_param(u.kc, AVER_PHYS_KCP_FLOOR_BLOCK_ON_WALL, static_cast<float>(blockOn));
        ticks(u, 10, 0, 0);
        ticks(u, 60, -downhill * 300.0f, 0);
        float q[3];
        pos(u, q);
        climb[blockOn] = q[2] - 90.0f;
        aver_phys_shutdown();
    }
    check(climb[1] < 3.0f, "floor_block_on_wall on: the body stays on the ground at the foot of a 60 degree ramp, height gain " + f2s(climb[1]));
    check(climb[0] > climb[1] + 10.0f, "off: it climbs the ramp, height gain " + f2s(climb[0]));
    check(speedOn > 0 && speedOff > 0, "an uphill direction exists on the 30 degree slope");
    check(near(speedOn, 300.0f, 15.0f), "floor_constant_speed on: 300 cm/s along the slope, got " + f2s(speedOn));
    check(speedOff < speedOn - 15.0f, "off: slower uphill (" + f2s(speedOff) + ")");
}

static void testSnap() {
    AVER_INFO("=== floor snap down a step ===");
    float airFrames[2] = {0, 0};
    float endZ[2] = {0, 0};
    for (int run = 0; run < 2; ++run) {
        aver_phys_init();
        box(-400, 0, -10, 600, 500, 10);     // x < 200, top at z = 0
        box(1000, 0, -18, 800, 500, 10);     // x > 200, top at z = -8
        Char c = makeChar(0, 0, 90.2f);
        aver_phys_kc_set_param(c.kc, AVER_PHYS_KCP_FLOOR_SNAP_LENGTH, run == 0 ? 10.0f : 0.0f);
        ticks(c, 10, 0, 0);
        for (int i = 0; i < 90; ++i) {
            tick(c, 300, 0);
            if (!onFloor(c)) airFrames[run] += 1;
        }
        float p[3];
        pos(c, p);
        endZ[run] = p[2];
        if (run == 0) check(p[0] > 400.0f, "(snap run) the character crossed the step, x = " + f2s(p[0]));
        aver_phys_shutdown();
    }
    check(airFrames[0] == 0, "snap length 10 keeps it on the floor over an 8 cm step down, airborne frames: " + f2s(airFrames[0]));
    check(near(endZ[0], 82.0f, 0.4f), "and it ends 8 cm lower: z = 82, got " + f2s(endZ[0]));
    check(airFrames[1] > 0, "without snapping it leaves the floor for " + f2s(airFrames[1]) + " frames");
}

static void testPlatform() {
    AVER_INFO("=== riding a moving platform ===");
    for (int variant = 0; variant < 3; ++variant) {
        aver_phys_init();
        box(0, -2000, -50, 100, 100, 10);   // something static far away so the world is not empty
        const int32_t plat = box(0, 0, -10, 300, 300, 10);   // top at z = 0
        aver_phys_body_set_motion_type(plat, AVER_PHYS_MOTION_KINEMATIC);
        Char c = makeChar(0, 0, 90.2f);
        if (variant == 1) aver_phys_kc_set_platform_layers(c.kc, 0, 0);
        if (variant == 2) aver_phys_kc_set_param(c.kc, AVER_PHYS_KCP_PLATFORM_ON_LEAVE, AVER_PHYS_KCL_DO_NOTHING);
        float platY = 0;
        auto frame = [&](float vx, float vy, bool moveDeck) {
            if (moveDeck) {
                platY += 100.0f * kDt;
                aver_phys_body_move_kinematic(plat, 0, platY, -10, 0, 0, 0, 1, kDt);
            }
            tick(c, vx, vy);
            aver_phys_step(kDt);
        };
        for (int i = 0; i < 10; ++i) frame(0, 0, false);
        float p0[3];
        pos(c, p0);
        for (int i = 0; i < 60; ++i) frame(0, 0, true);
        float p[3];
        pos(c, p);
        if (variant == 0) {
            check(near(p[1] - p0[1], 100.0f, 3.0f), "the platform carries a standing character: it moved " + f2s(p[1] - p0[1]) + " of the platform's 100 cm");
            check(onFloor(c), "and it is on the floor");
            float f[AVER_PHYS_KCI_F_COUNT];
            int32_t iv[AVER_PHYS_KCI_I_COUNT];
            info(c, f, iv);
            check(iv[AVER_PHYS_KCI_I_PLATFORM_BODY] == plat && near(f[AVER_PHYS_KCI_F_PLATFORM_VELOCITY + 1], 100.0f, 2.0f),
                  "the platform is reported with its velocity (0,100,0), got " + f2s(f[AVER_PHYS_KCI_F_PLATFORM_VELOCITY + 1]));
            // Jump off: the platform's velocity goes to the body.
            float v[3] = {0, 0, 400};
            aver_phys_kc_set_velocity(c.kc, v);
            platY += 100.0f * kDt;
            aver_phys_body_move_kinematic(plat, 0, platY, -10, 0, 0, 0, 1, kDt);
            aver_phys_kc_move_and_slide(c.kc, kDt, nullptr);
            vel(c, v);
            check(!onFloor(c) && near(v[1], 100.0f, 3.0f), "leaving the platform in the air adds its velocity: vy = " + f2s(v[1]));
        } else if (variant == 1) {
            check(near(p[1] - p0[1], 0.0f, 3.0f), "platform floor layers 0: the platform does not carry it, moved " + f2s(p[1] - p0[1]));
        } else {
            float v[3] = {0, 0, 400};
            aver_phys_kc_set_velocity(c.kc, v);
            platY += 100.0f * kDt;
            aver_phys_body_move_kinematic(plat, 0, platY, -10, 0, 0, 0, 1, kDt);
            aver_phys_kc_move_and_slide(c.kc, kDt, nullptr);
            vel(c, v);
            check(near(p[1] - p0[1], 100.0f, 3.0f) && std::fabs(v[1]) < 1.0f, "on-leave do nothing: carried, but no velocity added on leaving, vy = " + f2s(v[1]));
        }
        aver_phys_shutdown();
    }
}

static void testFloatingAndAccessors() {
    AVER_INFO("=== floating mode, readbacks and refusals ===");
    aver_phys_init();
    const int32_t ground = box(0, 0, -10, 2000, 2000, 10);
    box(520, 0, 200, 10, 600, 200);
    Char c = makeChar(0, 0, 130);
    aver_phys_kc_set_param(c.kc, AVER_PHYS_KCP_MOTION_MODE, AVER_PHYS_KCM_FLOATING);
    float v[3] = {0, 0, -600};
    aver_phys_kc_set_velocity(c.kc, v);
    for (int i = 0; i < 20; ++i) aver_phys_kc_move_and_slide(c.kc, kDt, nullptr);
    float p[3];
    pos(c, p);
    check(near(p[2], 90.0f, 0.3f), "floating: it still stops at the ground, z = " + f2s(p[2]));
    check(onWall(c) && !onFloor(c), "but every contact is a wall: not on the floor");
    int32_t hit = 0;
    float f[AVER_PHYS_KCC_F_COUNT];
    int32_t iv[AVER_PHYS_KCC_I_COUNT];
    aver_phys_kc_collision(c.kc, 0, f, AVER_PHYS_KCC_F_COUNT, iv, AVER_PHYS_KCC_I_COUNT);
    check(iv[AVER_PHYS_KCC_I_COLLIDER] == ground, "the collider is the ground");
    // Head-on into a wall within wall_min_slide_angle stops instead of sliding; at a steep angle it slides.
    aver_phys_body_set_position(c.body, 0, 0, 90.2f);
    float vx[3] = {600, 0, 0};
    aver_phys_kc_set_velocity(c.kc, vx);
    for (int i = 0; i < 60; ++i) aver_phys_kc_move_and_slide(c.kc, kDt, &hit);
    pos(c, p);
    check(near(p[0], 480.0f, 0.5f), "floating, head on into the wall: x = 480, got " + f2s(p[0]));
    aver_phys_body_set_position(c.body, 0, -400, 90.2f);
    float vd[3] = {600, 600, 0};
    aver_phys_kc_set_velocity(c.kc, vd);
    for (int i = 0; i < 60; ++i) aver_phys_kc_move_and_slide(c.kc, kDt, &hit);
    pos(c, p);
    check(hit == 1 && p[1] > 150.0f && near(p[0], 480.0f, 0.5f), "floating, 45 degrees into the wall: it slides along it, y = " + f2s(p[1]));

    // Parameters round-trip and are validated.
    float out = -1;
    check(aver_phys_kc_set_param(c.kc, AVER_PHYS_KCP_MAX_SLIDES, 3) == 1 && aver_phys_kc_param(c.kc, AVER_PHYS_KCP_MAX_SLIDES, &out) == 1 && out == 3.0f, "max_slides reads back");
    check(aver_phys_kc_set_param(c.kc, AVER_PHYS_KCP_MAX_SLIDES, 0) == 0, "max_slides 0 is refused");
    check(aver_phys_kc_set_param(c.kc, AVER_PHYS_KCP_SAFE_MARGIN, -1) == 0, "a negative margin is refused");
    check(aver_phys_kc_set_param(c.kc, 99, 1) == 0, "an unknown parameter is refused");
    float zero[3] = {0, 0, 0}, up[3] = {0, 1, 0};
    check(aver_phys_kc_set_up(c.kc, zero) == 0, "a zero up direction is refused");
    float big[3] = {0, 5, 0};
    check(aver_phys_kc_set_up(c.kc, big) == 1 && aver_phys_kc_up(c.kc, up) == 1 && near(up[1], 1.0f, 1e-4f), "up is normalised");
    uint32_t fl = 0, wl = 0;
    aver_phys_kc_set_platform_layers(c.kc, 0xF0F0F0F0u, 5u);
    aver_phys_kc_platform_layers(c.kc, &fl, &wl);
    check(fl == 0xF0F0F0F0u && wl == 5u, "platform layers keep all 32 bits");
    check(aver_phys_kc_move_and_slide(c.kc, 0.0f, nullptr) == 0, "dt 0 is refused");

    // Refusals at creation and after death.
    const int32_t dyn = aver_phys_add_dynamic_box(900, 900, 50, 20, 20, 20, 10);
    check(aver_phys_kc_create(dyn) == 0, "a dynamic body cannot be a kinematic character");
    check(aver_phys_kc_create(ground) == 0, "nor a static one");
    check(aver_phys_kc_create(c.body) == 0, "a body has at most one controller");
    check(aver_phys_kc_count() == 1 && aver_phys_kc_body(c.kc) == c.body, "count and body read back");
    aver_phys_remove_body(c.body);
    check(aver_phys_kc_exists(c.kc) == 0 && aver_phys_kc_move_and_slide(c.kc, kDt, nullptr) == 0, "removing the body kills the controller");
    check(aver_phys_kc_destroy(c.kc) == 1 && aver_phys_kc_count() == 0, "and it can be destroyed");
    aver_phys_shutdown();
    check(aver_phys_kc_count() == 0, "a new world has no controllers");
}

static void testUpDirection() {
    AVER_INFO("=== an up_direction that is not +Z ===");
    aver_phys_init();
    aver_phys_set_gravity(0, 0, 0);
    box(0, 0, -10, 2000, 2000, 10);            // the +Z floor, ignored by this character
    const int32_t sideFloor = box(600, 0, 300, 10, 2000, 2000);   // a wall at x = 590 acting as a floor whose up is -X
    Char c = makeChar(500, 0, 300);
    const float up[3] = {-1, 0, 0};
    aver_phys_kc_set_up(c.kc, up);
    // Gravity along +X (towards the "floor"), driven by the script.
    for (int i = 0; i < 40; ++i) {
        float v[3];
        vel(c, v);
        v[1] = 0;
        v[2] = 0;
        if (!onFloor(c)) v[0] += kGravity * kDt;
        aver_phys_kc_set_velocity(c.kc, v);
        aver_phys_kc_move_and_slide(c.kc, kDt, nullptr);
    }
    float p[3], f[AVER_PHYS_KCI_F_COUNT];
    int32_t iv[AVER_PHYS_KCI_I_COUNT];
    pos(c, p);
    info(c, f, iv);
    check(onFloor(c) && iv[AVER_PHYS_KCI_I_PLATFORM_BODY] == sideFloor, "with up = -X the wall at x = 590 is the floor");
    check(near(p[0], 560.0f, 0.5f) && near(f[AVER_PHYS_KCI_F_FLOOR_NORMAL], -1.0f, 0.02f), "the capsule's side stops 30 cm from the face: x = 560, normal (-1,0,0), got x = " + f2s(p[0]));
    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("Kinematic character: CharacterBody3D.move_and_slide");

    testFloorWallCeiling();
    testSlopes();
    testSnap();
    testPlatform();
    testFloatingAndAccessors();
    testUpDirection();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
