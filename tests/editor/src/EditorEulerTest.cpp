// The editor's Euler <-> quaternion round trip, and the gimbal-lock branch that was wrong.
//
// The real assertion here is not "the numbers look right" but "composing the returned Euler angles
// gives back the SAME ROTATION". Comparing Euler triples directly would fail on representations that
// are equally correct -- (0, 90, 0) and (90, 90, 90) are the same orientation -- and would pass on a
// pair of errors that cancel. Quaternion |dot| is the honest measure: 1 means identical rotation,
// and q and -q are the same rotation, so the absolute value is what matters.
#include "EditorEuler.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// |dot| between two quaternions: 1.0 when they are the same rotation.
static f32 sameness(const Quat& a, const Quat& b) {
    return std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
}

// Composes `e`, reads it back, recomposes, and reports how close the two rotations are.
static f32 roundTrip(const Vec3& e, Vec3* readBack = nullptr) {
    const Quat q = editor::quatFromEulerDeg(e);
    const Vec3 back = editor::eulerDegFromQuat(q);
    if (readBack) *readBack = back;
    return sameness(q, editor::quatFromEulerDeg(back));
}

static void one(const Vec3& e, const char* label) {
    Vec3 back{};
    const f32 s = roundTrip(e, &back);
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "%s (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)  |dot|=%.6f",
                  label, static_cast<double>(e.x), static_cast<double>(e.y), static_cast<double>(e.z),
                  static_cast<double>(back.x), static_cast<double>(back.y), static_cast<double>(back.z),
                  static_cast<double>(s));
    check(s > 0.9999f, buf);
}

int main() {
    AVER_INFO("=== ordinary orientations round-trip ===");
    one({  0,   0,   0}, "identity");
    one({ 30,   0,   0}, "roll only");
    one({  0,  45,   0}, "pitch only");
    one({  0,   0,  60}, "yaw only");
    one({ 15,  30,  45}, "all three");
    one({-20, -35, -50}, "all three negative");
    one({179,  10, -179}, "near the wrap");

    // THE REGRESSION. Every one of these sits inside the |sin(pitch)| > 0.99999 branch, which is a
    // 0.26 degree band around vertical, and every one of them used to come back wrong. (0, 90, 0)
    // came back as (0, 90, -180): |dot| = 0.000000, a full 180 degree flip, which a level save then
    // wrote to disk.
    AVER_INFO("=== gimbal lock: within 0.26 degrees of vertical ===");
    one({  0,  90,   0}, "straight up, no yaw");
    one({  0,  90,  30}, "straight up, yawed");
    one({  0,  90, 179}, "straight up, yawed far");
    one({  0, -90,   0}, "straight down, no yaw");
    one({  0, -90,  45}, "straight down, yawed");
    one({  0,  89.9f, 30}, "just inside the band");
    one({  0, -89.8f, 45}, "just inside the band, negative");
    one({  0,  89.5f, 30}, "just OUTSIDE the band, for contrast");

    AVER_INFO("=== a sweep through vertical, which is what dragging a rotation ring does ===");
    {
        f32 worst = 1.0f;
        f32 worstAt = 0.0f;
        for (int i = -1800; i <= 1800; ++i) {
            const f32 pitch = static_cast<f32>(i) * 0.05f;      // -90 .. +90 in 0.05 steps
            const f32 s = roundTrip({0.0f, pitch, 37.0f});
            if (s < worst) { worst = s; worstAt = pitch; }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "3601 pitches from -90 to +90 all round-trip; worst |dot|=%.6f at pitch %.2f",
                      static_cast<double>(worst), static_cast<double>(worstAt));
        check(worst > 0.9999f, buf);
    }

    AVER_INFO("=== pitch itself is still recovered exactly at the poles ===");
    {
        const Vec3 up = editor::eulerDegFromQuat(editor::quatFromEulerDeg({0, 90, 30}));
        check(std::fabs(up.y - 90.0f) < 0.01f, "pitch reads back as +90 at the top");
        const Vec3 dn = editor::eulerDegFromQuat(editor::quatFromEulerDeg({0, -90, 30}));
        check(std::fabs(dn.y + 90.0f) < 0.01f, "pitch reads back as -90 at the bottom");
        // Roll is deliberately pinned to zero there: only yaw - roll is observable at gimbal lock,
        // so putting it all in yaw is a choice, and it is the one the branch documents.
        check(std::fabs(up.x) < 0.01f, "and roll is pinned to zero, which is what makes yaw unique");
    }

    if (g_failures == 0) AVER_INFO("=== all editor euler tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
