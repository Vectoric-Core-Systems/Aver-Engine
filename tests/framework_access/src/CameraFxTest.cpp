// CameraFx.hpp: shake (trauma, decay, saturation, bounds) and head bob (period, reduce motion). Header-only,
// deterministic. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/CameraFx.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;
using namespace aver::fw;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static void testIdentity() {
    AVER_INFO("=== nothing active ===");
    CameraFx fx;
    Vec3 p{10, 20, 30};
    f32 yaw = 0.5f, pitch = -0.25f;
    check(!fx.apply(p, yaw, pitch, 0.016f, 1.0f, 1.0f) && p.x == 10 && p.y == 20 && p.z == 30 && yaw == 0.5f && pitch == -0.25f,
          "with no trauma and no bob nothing changes, bit for bit");
    fx.addShake(1.0f);
    fx.setBob(1.0f, 600.0f);
    CameraFx before = fx;
    check(!fx.apply(p, yaw, pitch, 0.016f, 0.0f, 0.0f) && p.x == 10 && yaw == 0.5f, "zero scales return before touching anything");
    check(std::memcmp(&before, &fx, sizeof fx) == 0, "including the state: trauma and phase stand still");
}

static void testShake() {
    AVER_INFO("=== shake ===");
    CameraFx fx;
    fx.addShake(0.6f);
    fx.addShake(0.9f);
    check(fx.trauma() == 1.0f, "trauma saturates at 1");
    fx.addShake(-1.0f);
    check(fx.trauma() == 1.0f, "a negative add is ignored");

    f32 maxYaw = 0, maxPitch = 0, maxOff = 0;
    for (int i = 0; i < 400; ++i) {
        CameraFx f;
        f.setSeed(0x1234u + static_cast<u32>(i));
        f.addShake(1.0f);
        // Step the noise clock without letting the trauma decay much: tiny dt.
        for (int k = 0; k < i % 50; ++k) { Vec3 q{}; f32 y = 0, pi = 0; f.apply(q, y, pi, 0.0005f, 1.0f, 0.0f); }
        Vec3 p{0, 0, 0};
        f32 yaw = 0, pitch = 0;
        f.apply(p, yaw, pitch, 0.0005f, 1.0f, 0.0f);
        maxYaw = std::fmax(maxYaw, std::fabs(yaw));
        maxPitch = std::fmax(maxPitch, std::fabs(pitch));
        maxOff = std::fmax(maxOff, std::fmax(std::fabs(p.x), std::fmax(std::fabs(p.y), std::fabs(p.z))));
    }
    check(maxYaw <= radians(1.5f) + 1e-5f && maxPitch <= radians(1.2f) + 1e-5f && maxOff <= 1.5f + 1e-4f, "amplitudes stay inside 1.5 deg yaw, 1.2 deg pitch, 1.5 cm");
    check(maxYaw > radians(0.2f) && maxOff > 0.2f, "and at full trauma the shake is actually there");

    // Decay: 1.5 per second.
    CameraFx d;
    d.addShake(1.0f);
    Vec3 p{};
    f32 y = 0, pi = 0;
    d.apply(p, y, pi, 0.2f, 1.0f, 0.0f);
    check(near(d.trauma(), 0.7f), "trauma decays at 1.5 per second");
    d.apply(p, y, pi, 1.0f, 1.0f, 0.0f);
    check(d.trauma() == 0.0f, "down to zero and no further");

    // Deterministic for a seed and time.
    CameraFx a, b;
    a.setSeed(77); b.setSeed(77);
    a.addShake(0.8f); b.addShake(0.8f);
    bool same = true;
    for (int i = 0; i < 30; ++i) {
        Vec3 pa{1, 2, 3}, pb{1, 2, 3};
        f32 ya = 0.1f, yb = 0.1f, pia = 0, pib = 0;
        a.apply(pa, ya, pia, 0.016f, 1.0f, 0.0f);
        b.apply(pb, yb, pib, 0.016f, 1.0f, 0.0f);
        if (pa.x != pb.x || pa.z != pb.z || ya != yb || pia != pib) same = false;
    }
    check(same, "deterministic for a seed and a time");

    // The slider scales it.
    CameraFx half, full;
    half.setSeed(5); full.setSeed(5);
    half.addShake(1.0f); full.addShake(1.0f);
    Vec3 ph{}, pf{};
    f32 yh = 0, yf = 0, pih = 0, pif = 0;
    half.apply(ph, yh, pih, 0.01f, 0.5f, 0.0f);
    full.apply(pf, yf, pif, 0.01f, 1.0f, 0.0f);
    check(near(std::fabs(yh) * 2.0f, std::fabs(yf), 1e-5f), "a 0.5 shake scale halves the amplitude");
}

static void testBob() {
    AVER_INFO("=== head bob ===");
    CameraFx fx;
    fx.setBob(1.0f, 640.0f);   // 640 / 320 = 2 cycles per second
    Vec3 p{};
    f32 yaw = 0, pitch = 0;
    fx.apply(p, yaw, pitch, 0.125f, 0.0f, 1.0f);   // a quarter cycle: sin(pi/2) = 1
    check(near(fx.phase(), 0.25f) && near(p.z, 2.2f, 1e-3f), "the phase advances speed/320 cycles per second; the vertical peak is 2.2 cm");
    check(near(p.y, std::sin(0.5f * 6.28318530718f * 0.25f) * 1.1f, 1e-3f) && near(p.x, 0.0f, 1e-4f), "and the lateral sway is on the right vector (+Y at yaw 0)");
    fx.apply(p, yaw, pitch, 0.375f, 0.0f, 1.0f);   // phase 0.625 -> wrapped
    check(fx.phase() >= 0.0f && fx.phase() < 1.0f, "the phase wraps");

    CameraFx still;
    still.setBob(1.0f, 0.0f);
    Vec3 q{1, 1, 1};
    check(!still.apply(q, yaw, pitch, 0.1f, 0.0f, 1.0f) && q.z == 1.0f, "no speed, no bob");

    // Reduce motion = both scales 0.
    CameraFx r;
    r.setBob(1.0f, 640.0f);
    r.addShake(1.0f);
    Vec3 pr{5, 5, 5};
    f32 yr = 0.3f, pir = 0.1f;
    check(!r.apply(pr, yr, pir, 0.1f, 0.0f, 0.0f) && pr.z == 5.0f && yr == 0.3f, "reduce motion zeroes both");

    // The bob offset follows the camera's yaw.
    CameraFx turned;
    turned.setBob(1.0f, 640.0f);
    Vec3 pt{};
    f32 yt = 1.5707963f, pit = 0;   // facing +Y: the right vector is -X
    turned.apply(pt, yt, pit, 0.125f, 0.0f, 1.0f);
    check(pt.x < -0.5f && std::fabs(pt.y) < 0.05f, "the lateral sway turns with the camera");
}

int main() {
    AVER_INFO("CameraFxTest");
    testIdentity();
    testShake();
    testBob();
    AVER_INFO(g_failures == 0 ? "CameraFxTest: PASS" : "CameraFxTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
