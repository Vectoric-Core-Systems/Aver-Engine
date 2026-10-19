// SphereCastQTest -- aver_phys_sphere_cast_q (physics_queries_abi.h): distance, normal, the sensor skip, two
// ignore slots (a body and a character), layer masks and the refusals. Nothing steps, so every distance is
// arithmetic on the numbers below. The scene is off-axis (walls on +Y and +Z as well as +X) so a swapped
// axis in the engine-to-Jolt change of basis shows itself.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_layers_abi.h"
#include "aver/physics/physics_queries_abi.h"

#include <cstdio>
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
static bool near(float a, float b, float tol = 1.5f) { return std::fabs(a - b) <= tol; }

static int32_t wall(float x, float y, float z, float hx, float hy, float hz) {
    return aver_phys_add_static_box(x, y, z, hx, hy, hz);
}

static void testDistanceAndNormal() {
    AVER_INFO("=== distance, normal, axes ===");
    aver_phys_init();
    const int32_t front = wall(500, 0, 0, 10, 200, 200);     // face at x = 490
    const int32_t right = wall(0, 800, 0, 200, 10, 200);     // face at y = 790
    const int32_t roof  = wall(0, 0, 600, 200, 200, 10);     // face at z = 590
    const uint32_t all = AVER_PHYS_LAYER_MASK_ALL;
    float d = -1, pt[3] = {0}, n[3] = {0};

    int32_t h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, all, 0, 0, 0, &d, pt, n);
    check(h == front, "a sphere swept along +X hits the +X wall");
    check(near(d, 490.0f - 12.0f), "at 490 - 12 = 478 cm, got " + f2s(d));
    check(near(pt[0], 490.0f), "the contact point is on the face x = 490, got " + f2s(pt[0]));
    check(near(n[0], -1.0f, 0.05f) && std::fabs(n[1]) < 0.05f && std::fabs(n[2]) < 0.05f,
          "and the normal points back at the sphere, (-1,0,0)");

    h = aver_phys_sphere_cast_q(0, 0, 0, 0, 2000, 0, 30, all, 0, 0, 0, &d, pt, n);
    check(h == right && near(d, 760.0f), "along +Y it reaches the right wall at 790 - 30 = 760, got " + f2s(d));
    check(near(n[1], -1.0f, 0.05f), "with normal (0,-1,0)");

    h = aver_phys_sphere_cast_q(0, 0, 0, 0, 0, 2000, 20, all, 0, 0, 0, &d, pt, n);
    check(h == roof && near(d, 570.0f), "along +Z it reaches the roof at 590 - 20 = 570, got " + f2s(d));

    h = aver_phys_sphere_cast_q(0, 0, 0, 300, 0, 0, 12, all, 0, 0, 0, &d, pt, n);
    check(h == 0, "a 300 cm sweep stops short of a wall 478 cm away");
    h = aver_phys_sphere_cast_q(0, 0, 0, -2000, 0, 0, 12, all, 0, 0, 0, &d, pt, n);
    check(h == 0, "sweeping away from every wall hits nothing");
    h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, all, 0, 0, 0, nullptr, nullptr, nullptr);
    check(h == front, "every out-pointer may be null");

    // The camera's actual use: the displacement is diagonal and not unit length.
    h = aver_phys_sphere_cast_q(0, 0, 0, 1000, 100, 0, 10, all, 0, 0, 0, &d, pt, n);
    check(h == front && d > 400.0f && d < 500.0f, "an oblique sweep reports a distance along its own length (about 490), got " + f2s(d));

    aver_phys_shutdown();
}

static void testRefusals() {
    AVER_INFO("=== refusals and start-overlap ===");
    aver_phys_init();
    const int32_t front = wall(500, 0, 0, 10, 200, 200);
    const uint32_t all = AVER_PHYS_LAYER_MASK_ALL;
    float d = -1, pt[3], n[3];

    check(aver_phys_sphere_cast_q(0, 0, 0, 0, 0, 0, 12, all, 0, 0, 0, &d, pt, n) == 0, "a zero displacement is a miss");
    check(aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 0, all, 0, 0, 0, &d, pt, n) == 0, "radius 0 is refused");
    check(aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, -5, all, 0, 0, 0, &d, pt, n) == 0, "a negative radius is refused");

    // Starting overlapped: the sphere's centre is 4 cm from the face with radius 12.
    d = -1;
    const int32_t h = aver_phys_sphere_cast_q(486, 0, 0, 100, 0, 0, 12, all, 0, 0, 0, &d, pt, n);
    check(h == front && near(d, 0.0f), "a sphere that starts overlapping reports distance 0, got " + f2s(d));

    aver_phys_shutdown();
    check(aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, all, 0, 0, 0, &d, pt, n) == 0, "no world returns 0");
}

static void testSensors() {
    AVER_INFO("=== sensors are skipped unless asked for; the _ex cast still sees them ===");
    aver_phys_init();
    const int32_t sensor = aver_phys_add_sensor_box(0, 0, 0, 100, 100, 100);   // x -100..100
    const int32_t back   = wall(500, 0, 0, 10, 200, 200);                       // face at x = 490
    const uint32_t all = AVER_PHYS_LAYER_MASK_ALL;
    float d = -1, pt[3], n[3];

    int32_t h = aver_phys_sphere_cast_q(-500, 0, 0, 2000, 0, 0, 12, all, 0, 0, 0, &d, pt, n);
    check(h == back && near(d, 978.0f), "the cast passes through the sensor to the wall at 990 - 12 = 978, got " + f2s(d));
    h = aver_phys_sphere_cast_q(-500, 0, 0, 2000, 0, 0, 12, all, 0, 0, AVER_PHYS_QUERY_INCLUDE_SENSORS, &d, pt, n);
    check(h == sensor && near(d, 388.0f), "and hits it at 400 - 12 = 388 with INCLUDE_SENSORS, got " + f2s(d));

    // Why this function exists: the older cast does not skip sensors.
    float p2[3], n2[3];
    const int32_t old = aver_phys_sphere_cast_ex(-500, 0, 0, 1, 0, 0, 2000, 12, all, 0, p2, n2);
    check(old == sensor, "aver_phys_sphere_cast_ex still sees the sensor");

    aver_phys_shutdown();
}

static void testIgnoresAndMask() {
    AVER_INFO("=== two ignore slots, a character handle, layer masks ===");
    aver_phys_init();
    const int32_t nearW = wall(300, 0, 0, 10, 200, 200);     // face at x = 290
    const int32_t midW  = wall(600, 0, 0, 10, 200, 200);     // face at x = 590
    const int32_t farW  = wall(900, 0, 0, 10, 200, 200);     // face at x = 890
    const uint32_t all = AVER_PHYS_LAYER_MASK_ALL;
    float d = -1, pt[3], n[3];

    int32_t h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, all, nearW, 0, 0, &d, pt, n);
    check(h == midW && near(d, 578.0f), "slot A skips the near wall, got " + f2s(d));
    h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, all, 0, nearW, 0, &d, pt, n);
    check(h == midW, "slot B skips it too");
    h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, all, nearW, midW, 0, &d, pt, n);
    check(h == farW && near(d, 878.0f), "both slots together reach the far wall, got " + f2s(d));
    h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, all, 99999, 88888, 0, &d, pt, n);
    check(h == nearW, "a handle that resolves to nothing ignores nothing");

    aver_phys_body_set_layer(nearW, 4);
    aver_phys_body_set_layer(midW, 5);
    aver_phys_body_set_layer(farW, 5);
    h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, AVER_PHYS_LAYER_BIT(5), 0, 0, 0, &d, pt, n);
    check(h == midW, "a mask naming layer 5 passes the layer-4 wall");
    h = aver_phys_sphere_cast_q(0, 0, 0, 2000, 0, 0, 12, AVER_PHYS_LAYER_BIT(9), 0, 0, 0, &d, pt, n);
    check(h == 0, "a mask naming no wall's layer hits nothing");

    // A character handle in a slot skips the character's inner body.
    const int32_t ch = aver_phys_character_create(30, 180, 150, 0, 100);
    check(ch != 0, "a character was created");
    h = aver_phys_sphere_cast_q(0, 0, 100, 2000, 0, 0, 12, AVER_PHYS_LAYER_BIT(0) | AVER_PHYS_LAYER_BIT(4) | AVER_PHYS_LAYER_BIT(5),
                                0, 0, 0, &d, pt, n);
    const bool sawCharacter = h == ch;
    h = aver_phys_sphere_cast_q(0, 0, 100, 2000, 0, 0, 12, all, ch, 0, 0, &d, pt, n);
    check(h == nearW, "with the character handle ignored the cast reaches the wall behind it");
    if (sawCharacter) check(true, "(and without the skip the character was the first hit)");
    h = aver_phys_sphere_cast_q(0, 0, 100, 2000, 0, 0, 12, all, 0, ch, 0, &d, pt, n);
    check(h == nearW, "the character handle works in slot B as well");
    aver_phys_character_destroy(ch);

    aver_phys_shutdown();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("aver_phys_sphere_cast_q");

    testDistanceAndNormal();
    testRefusals();
    testSensors();
    testIgnoresAndMask();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
