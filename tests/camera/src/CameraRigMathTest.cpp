// CameraRigMathTest -- the pure camera math (CameraRigMath.hpp, CameraRigPresets.hpp): exponential follow, lag with
// sub-steps, angle follow, the yaw/pitch/roll basis, pose blends, look-ahead, the spring-arm resolve, fade hysteresis,
// FOV composition, the presets, and bit-identical determinism. Nothing here touches a scene or a device.
#include "aver/camera/CameraRigMath.hpp"
#include "aver/camera/CameraRigPresets.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

using namespace aver;
using namespace aver::camera;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(float v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
static bool bitEqual(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }
static bool bitEqual(const Vec3& a, const Vec3& b) { return bitEqual(a.x, b.x) && bitEqual(a.y, b.y) && bitEqual(a.z, b.z); }

static void testExpFollow() {
    AVER_INFO("=== expFollow ===");
    const float speed = 3.0f;
    const float r = expFollow(0.0f, 1.0f, speed, kLn2 / speed);
    check(near(r, 0.5f, 1e-3f), "the error halves after ln2 / speed seconds, got " + f2s(r));
    check(expFollow(2.0f, 9.0f, 0.0f, 0.016f) == 9.0f, "speed <= 0 returns the target exactly");
    check(expFollow(2.0f, 9.0f, -1.0f, 0.016f) == 9.0f, "a negative speed returns the target too");
    check(expFollow(2.0f, 9.0f, 5.0f, 0.0f) == 2.0f, "dt 0 leaves the value alone");
    check(expFollow(0.0f, 1.0f, 1.0f, 100.0f) > 0.999f, "a long step lands on the target without overshoot");
}

// A pivot moving at `v` cm/s along X for `seconds`, followed by lagFollow at `fps`; returns target - camera at the end.
static float steadyLag(float fps, float speed, float maxStepS, float v, float seconds) {
    const float dt = 1.0f / fps;
    Vec3 target{0, 0, 0}, cur{0, 0, 0};
    const int frames = static_cast<int>(seconds * fps);
    for (int i = 0; i < frames; ++i) {
        const Vec3 prev = target;
        target.x += v * dt;
        cur = lagFollow(cur, prev, target, speed, maxStepS, 0.0f, dt);
    }
    return target.x - cur.x;
}

// The same pivot followed by a plain expFollow per frame, to show what lagFollow buys.
static float naiveLag(float fps, float speed, float v, float seconds) {
    const float dt = 1.0f / fps;
    float target = 0, cur = 0;
    const int frames = static_cast<int>(seconds * fps);
    for (int i = 0; i < frames; ++i) {
        target += v * dt;
        cur = expFollow(cur, target, speed, dt);
    }
    return target - cur;
}

static void testLagFollow() {
    AVER_INFO("=== lagFollow ===");
    const float v = 600.0f, speed = 10.0f, expect = v / speed;
    for (const float fps : {30.0f, 60.0f, 144.0f, 240.0f}) {
        const float lag = steadyLag(fps, speed, 1.0f / 60.0f, v, 4.0f);
        check(std::fabs(lag - expect) <= expect * 0.015f, "a pivot at 600 cm/s settles to v / speed = 60 at " + f2s(fps) + " fps, got " + f2s(lag));
    }
    const float huge = steadyLag(30.0f, speed, 100.0f, v, 4.0f);
    const float fine = steadyLag(30.0f, speed, 1.0f / 240.0f, v, 4.0f);
    check(std::fabs(huge - fine) <= 0.5f, "the step cap does not change the exactly integrated result, " + f2s(huge) + " vs " + f2s(fine));
    const float naive = naiveLag(30.0f, speed, v, 4.0f);
    check(std::fabs(naive - expect) > expect * 0.05f, "a plain per-frame expFollow is measurably off at 30 fps (" + f2s(naive) + "), which is why lagFollow exists");

    const Vec3 far = lagFollow(Vec3{0, 0, 0}, Vec3{0, 0, 0}, Vec3{5000, 0, 0}, speed, 1.0f / 60.0f, 80.0f, 1.0f / 60.0f);
    check(near((Vec3{5000, 0, 0} - far).size(), 80.0f, 1e-2f), "lagMaxDist is a hard bound after a 5000 cm jump, gap " + f2s((Vec3{5000, 0, 0} - far).size()));
    const Vec3 t{12, -7, 3};
    check(bitEqual(lagFollow(Vec3{1, 1, 1}, Vec3{2, 2, 2}, t, 0.0f, 0.016f, 0.0f, 0.016f), t), "speed <= 0 returns the target exactly");
    check(bitEqual(lagFollow(Vec3{1, 1, 1}, Vec3{2, 2, 2}, t, -3.0f, 0.016f, 0.0f, 0.016f), t), "a negative speed returns the target too");
    const Vec3 held = lagFollow(Vec3{1, 2, 3}, Vec3{1, 2, 3}, Vec3{1, 2, 3}, speed, 1.0f / 60.0f, 0.0f, 0.0f);
    check(bitEqual(held, Vec3{1, 2, 3}), "dt 0 with a still target changes nothing");
}

static void testAngleFollow() {
    AVER_INFO("=== angleFollow ===");
    const float a179 = radians(179.0f), m179 = radians(-179.0f);
    const float r = angleFollow(a179, m179, 5.0f, 0.05f, 0.0f);
    const float moved = wrapPi(r - a179);
    check(moved > 0.0f && moved < radians(2.0f), "179 toward -179 crosses 180 by the short way, moved " + f2s(degrees(moved)) + " deg");
    const float back = angleFollow(m179, a179, 5.0f, 0.05f, 0.0f);
    check(wrapPi(back - m179) < 0.0f, "and the other way round goes the other way");
    const float clamped = angleFollow(0.0f, radians(90.0f), 0.1f, 0.1f, radians(30.0f));
    check(near(clamped, radians(60.0f), 1e-4f), "maxDist clamps the remaining gap to 30 deg, got " + f2s(degrees(clamped)));
    check(angleFollow(0.3f, 1.2f, 0.0f, 0.016f, 0.0f) == 1.2f, "speed <= 0 returns the target");
    check(near(angleFollow(0.0f, 1.0f, 1000.0f, 1.0f, 0.0f), 1.0f, 1e-4f), "a huge step lands on the target");
}

static void testBasis() {
    AVER_INFO("=== basis ===");
    bool bitFwd = true, round = true, ortho = true;
    float worst = 0;
    for (float yaw = -3.0f; yaw <= 3.0f; yaw += 0.7f)
        for (float pitch = -1.4f; pitch <= 1.4f; pitch += 0.35f)
            for (float roll = -2.0f; roll <= 2.0f; roll += 0.5f) {
                const Basis b = basisFromYawPitchRoll(yaw, pitch, roll);
                const Vec3 f{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
                if (roll == 0.0f) bitFwd = bitFwd && bitEqual(b.fwd, f);
                float y, p, r;
                yawPitchRollFromBasis(b, y, p, r);
                const float e = std::fmax(std::fabs(wrapPi(y - yaw)), std::fmax(std::fabs(p - pitch), std::fabs(wrapPi(r - roll))));
                worst = std::fmax(worst, e);
                if (e > 1e-4f) round = false;
                if (std::fabs(dot(b.fwd, b.right)) > 1e-5f || std::fabs(dot(b.fwd, b.up)) > 1e-5f || std::fabs(dot(b.right, b.up)) > 1e-5f ||
                    std::fabs(b.up.size() - 1.0f) > 1e-5f)
                    ortho = false;
            }
    check(bitFwd, "basis.fwd equals game::cameraForward bit for bit at roll 0");
    check(round, "yaw/pitch/roll round-trip through the basis to 1e-4 rad (worst " + f2s(worst) + ")");
    check(ortho, "the basis is orthonormal over the grid");
    const Basis lean = basisFromYawPitchRoll(0.0f, 0.0f, radians(30.0f));
    check(near(lean.up.y, 0.5f, 1e-4f) && near(lean.up.z, std::cos(radians(30.0f)), 1e-4f), "positive roll tips the camera's top toward its right");
    const Basis flat = basisFromYawPitchRoll(0.0f, 0.0f, 0.0f);
    check(flat.up.z == 1.0f && flat.right.y == 1.0f, "roll 0 keeps world up exactly");
    const Basis q = basisFromQuat(quatFromBasis(basisFromYawPitchRoll(0.9f, -0.4f, 0.3f)));
    const Basis o = basisFromYawPitchRoll(0.9f, -0.4f, 0.3f);
    check((q.fwd - o.fwd).size() < 1e-4f && (q.up - o.up).size() < 1e-4f, "quatFromBasis / basisFromQuat round-trip");
}

static void testBlend() {
    AVER_INFO("=== blendPov / blendAlpha ===");
    CameraPov a, b;
    a.pos = Vec3{0, 0, 0}; a.yaw = 0.2f; a.pitch = 0.1f; a.roll = 0.0f; a.fovYRad = radians(60); a.nearCm = 2; a.farCm = 1000;
    b.pos = Vec3{100, 200, 300}; b.yaw = 1.2f; b.pitch = -0.3f; b.roll = 0.2f; b.fovYRad = radians(90); b.nearCm = 6; b.farCm = 3000;
    const CameraPov p0 = blendPov(a, b, 0.0f), p1 = blendPov(a, b, 1.0f);
    check(bitEqual(p0.pos, a.pos) && bitEqual(p0.yaw, a.yaw) && bitEqual(p0.fovYRad, a.fovYRad), "alpha 0 returns the first pose exactly");
    check(bitEqual(p1.pos, b.pos) && bitEqual(p1.yaw, b.yaw) && bitEqual(p1.roll, b.roll) && bitEqual(p1.farCm, b.farCm), "alpha 1 returns the second pose exactly");
    const CameraPov m = blendPov(a, b, 0.25f);
    check(near(m.pos.x, 25.0f, 1e-3f) && near(m.pos.y, 50.0f, 1e-3f), "position is linear");
    check(near(m.fovYRad, radians(67.5f), 1e-5f) && near(m.nearCm, 3.0f, 1e-4f) && near(m.farCm, 1500.0f, 1e-2f), "FOV and clip are linear");
    CameraPov c = a, d = a;
    c.yaw = radians(170.0f); d.yaw = radians(-170.0f);
    const CameraPov mid = blendPov(c, d, 0.5f);
    check(std::fabs(degrees(mid.yaw)) > 179.0f, "the rotation takes the short path across 180 (yaw " + f2s(degrees(mid.yaw)) + ")");
    check(blendPov(a, b, 5.0f).pos.x == b.pos.x && blendPov(a, b, -1.0f).pos.x == a.pos.x, "alpha outside 0..1 clamps");

    for (const BlendCurve cv : {BlendCurve::EaseInOut, BlendCurve::Linear, BlendCurve::EaseIn, BlendCurve::EaseOut}) {
        bool mono = blendAlpha(0.0f, cv) == 0.0f && blendAlpha(1.0f, cv) == 1.0f;
        float last = 0;
        for (int i = 1; i <= 1000; ++i) {
            const float v = blendAlpha(static_cast<float>(i) / 1000.0f, cv);
            if (v < last) mono = false;
            last = v;
        }
        check(mono, "curve " + std::to_string(static_cast<int>(cv)) + " has exact endpoints and is monotone");
    }
    check(blendAlpha(0.5f, BlendCurve::EaseInOut) == 0.5f, "EaseInOut(0.5) is exactly 0.5");
    check(blendAlpha(-3.0f, BlendCurve::Linear) == 0.0f && blendAlpha(3.0f, BlendCurve::Linear) == 1.0f, "t outside 0..1 clamps");
    check(blendAlpha(0.25f, BlendCurve::EaseIn) < 0.25f && blendAlpha(0.25f, BlendCurve::EaseOut) > 0.25f, "EaseIn is slow first, EaseOut fast first");
}

static Vec3 runLead(Vec3 vel, float aheadS, float maxCm, bool& teleported, float seconds = 3.0f) {
    LeadState s;
    Vec3 p{0, 0, 0}, lead;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
        p = p + vel * dt;
        lead = lookAhead(s, p, aheadS, maxCm, dt, 1000.0f, teleported);
    }
    return lead;
}

static void testLookAhead() {
    AVER_INFO("=== lookAhead ===");
    bool tp = false;
    Vec3 lead = runLead(Vec3{500, 0, 0}, 0.3f, 1000.0f, tp);
    check(near(lead.x, 150.0f, 3.0f), "the lead converges to velocity * aheadS = 150, got " + f2s(lead.x));
    lead = runLead(Vec3{500, 0, 0}, 0.3f, 100.0f, tp);
    check(near(lead.size(), 100.0f, 1.0f), "and is clamped to maxCm, got " + f2s(lead.size()));
    lead = runLead(Vec3{0, 0, 500}, 0.3f, 1000.0f, tp);
    check(lead.size() < 1e-3f, "vertical velocity is ignored");

    LeadState s;
    Vec3 p{0, 0, 0};
    for (int i = 0; i < 120; ++i) { p.x += 8.0f; lookAhead(s, p, 0.3f, 1000.0f, 1.0f / 60.0f, 1000.0f, tp); }
    p.x += 2000.0f;
    const Vec3 after = lookAhead(s, p, 0.3f, 1000.0f, 1.0f / 60.0f, 1000.0f, tp);
    check(tp && after.size() == 0.0f && s.vel.size() == 0.0f, "a 2000 cm jump resets the estimate, returns zero lead and reports teleported");
    p.x += 8.0f;
    lookAhead(s, p, 0.3f, 1000.0f, 1.0f / 60.0f, 1000.0f, tp);
    check(!tp, "the next ordinary frame is not a teleport");
}

static void testResolveArm() {
    AVER_INFO("=== resolveArm / fadeHide / composeFov ===");
    check(resolveArm(450.0f, 150.0f, 400.0f, 20.0f, 0.25f, 0.016f) == 150.0f, "inward is instant");
    check(resolveArm(450.0f, -1.0f, 450.0f, 20.0f, 0.25f, 0.016f) == 450.0f, "a clear sweep keeps the wanted arm");
    const float out = resolveArm(400.0f, -1.0f, 100.0f, 20.0f, 0.25f, 0.25f);
    check(near(out, 250.0f, 250.0f * 0.05f), "outward recovers with the half-life (one half-life: 100 -> 250), got " + f2s(out));
    check(resolveArm(450.0f, 5.0f, 300.0f, 20.0f, 0.25f, 0.016f) == 20.0f, "never below minArm");
    check(resolveArm(450.0f, 0.0f, 300.0f, 20.0f, 0.25f, 0.016f) == 20.0f, "a start-overlapping probe (distance 0) yields minArm, not zero");
    check(resolveArm(300.0f, -1.0f, 450.0f, 20.0f, 0.25f, 0.016f) <= 300.0f, "never above armWanted");
    check(resolveArm(10.0f, -1.0f, -1.0f, 20.0f, 0.25f, 0.016f) == 10.0f, "an arm shorter than minArm is not stretched to it");
    check(resolveArm(450.0f, -1.0f, -1.0f, 20.0f, 0.25f, 0.016f) == 450.0f, "the first frame (no previous arm) takes the allowed length");
    float arm = 100.0f;
    bool mono = true;
    for (int i = 0; i < 600; ++i) {
        const float n = resolveArm(450.0f, -1.0f, arm, 20.0f, 0.25f, 1.0f / 60.0f);
        if (n < arm || n > 450.0f) mono = false;
        arm = n;
    }
    check(mono && arm > 449.0f, "recovery is monotone and arrives");

    check(fadeHide(false, 69.0f, 140.0f, 70.0f), "hides below endCm");
    check(!fadeHide(false, 75.0f, 140.0f, 70.0f), "does not hide between endCm and the return distance when visible");
    check(fadeHide(true, 75.0f, 140.0f, 70.0f), "stays hidden until endCm * 1.15");
    check(!fadeHide(true, 81.0f, 140.0f, 70.0f), "returns above endCm * 1.15");

    check(near(composeFov(radians(60.0f), radians(10.0f), radians(5.0f), 0.5f), radians(37.5f), 1e-5f), "composeFov composes kick, add and scale");
    check(composeFov(radians(60.0f), 0.0f, 0.0f, 0.1f) == kMinFovRad, "and clamps to 10 degrees");
    check(composeFov(radians(60.0f), 0.0f, 0.0f, 5.0f) == kMaxFovRad, "and to 140 degrees");
}

static void testPresets() {
    AVER_INFO("=== presets ===");
    check(sizeof(CCameraRig) == 160, "CCameraRig is 160 bytes");
    const CCameraRig zero{};
    const RigTuning z = resolveTuning(zero);
    check(z.armCm == 450.0f && z.minArmCm == 20.0f && z.probeCm == 12.0f && z.recoverS == 0.25f, "a zero rig reads arm 450, min 20, probe 12, recover 0.25");
    check(z.pitchMinDeg == -80.0f && z.pitchMaxDeg == 80.0f && z.fadeStartCm == 140.0f && z.fadeEndCm == 70.0f, "and pitch limits +-80, fade 140 / 70");
    check(z.lookAheadMaxCm == 150.0f && z.fovKickSpeed == 1500.0f && z.seatYawDeg == 110.0f && z.seatPitchDeg == 50.0f, "and look-ahead 150, kick speed 1500, seat 110 / 50");

    bool ids = true;
    for (u32 i = 0; i < kCameraPresetCount; ++i) ids = ids && presetRig(static_cast<CameraPreset>(i)).preset == i;
    check(ids, "every preset records its own id");
    check(presetRig(CameraPreset::Chase).mode == static_cast<u32>(CameraMode::Chase) && presetRig(CameraPreset::Chase).armLengthCm == 450.0f, "Chase is a 450 cm chase");
    const CCameraRig vc = presetRig(CameraPreset::VehicleChase);
    check(vc.mode == static_cast<u32>(CameraMode::Chase) && vc.armLengthCm == 600.0f && vc.posLagSpeed == 8.0f && vc.lagMaxDistCm == 150.0f &&
              vc.rotLagSpeed == 5.0f && vc.lookAheadS == 0.25f && vc.pivotZ == 120.0f && vc.socketZ == 60.0f && vc.pitchDeg == -12.0f &&
              (vc.flags & kRigNoInheritPitch) && (vc.flags & kRigNoFade) && vc.fovKickDeg == 10.0f && vc.blendTimeS == 0.8f,
          "VehicleChase matches the design table");
    check(presetRig(CameraPreset::Orbit).mode == static_cast<u32>(CameraMode::Orbit) && presetRig(CameraPreset::Orbit).pitchDeg == -20.0f, "Orbit looks down 20");
    check(presetRig(CameraPreset::FirstPerson).mode == static_cast<u32>(CameraMode::FirstPerson) && presetRig(CameraPreset::FirstPerson).bobAmount == 0.5f, "FirstPerson bobs 0.5");
    check(presetRig(CameraPreset::Seat).mode == static_cast<u32>(CameraMode::Seat) && presetRig(CameraPreset::Seat).posLagSpeed == 25.0f, "Seat lags 25");
    const CCameraRig td = presetRig(CameraPreset::TopDown);
    check(td.mode == static_cast<u32>(CameraMode::TopDown) && td.pitchDeg == -62.0f && (td.flags & kRigNoCollision) &&
              (td.flags & (kRigNoInheritYaw | kRigNoInheritPitch | kRigNoInheritRoll)) == (kRigNoInheritYaw | kRigNoInheritPitch | kRigNoInheritRoll),
          "TopDown looks down 62, inherits nothing and does not collide");
    check(presetRig(CameraPreset::Fixed).mode == static_cast<u32>(CameraMode::Fixed), "Fixed is Fixed");
    check(near(nearCornerRadiusCm(2.0f, radians(60.0f), 16.0f / 9.0f), 2.0f * std::tan(radians(30.0f)) * std::sqrt(16.0f / 9.0f * 16.0f / 9.0f + 1.0f), 1e-4f),
          "the near-plane corner radius is about 2.3 cm at the defaults");
}

static void testDeterminism() {
    AVER_INFO("=== determinism ===");
    auto run = [] {
        LeadState s;
        Vec3 pivot{0, 0, 0}, cam{0, 0, 0}, prev{0, 0, 0};
        float arm = 100.0f, yaw = 0.0f;
        bool tp = false;
        for (int i = 0; i < 600; ++i) {
            const float dt = 1.0f / 60.0f;
            prev = pivot;
            pivot = pivot + Vec3{std::sin(static_cast<float>(i) * 0.05f) * 9.0f, 7.0f, 0.0f};
            const Vec3 lead = lookAhead(s, pivot, 0.25f, 200.0f, dt, 1000.0f, tp);
            cam = lagFollow(cam, prev + lead, pivot + lead, 8.0f, 1.0f / 60.0f, 150.0f, dt);
            arm = resolveArm(450.0f, i % 90 < 30 ? 120.0f : -1.0f, arm, 20.0f, 0.25f, dt);
            yaw = angleFollow(yaw, 2.5f, 5.0f, dt, 0.0f);
        }
        return std::pair<Vec3, float>{cam, arm + yaw};
    };
    const auto a = run(), b = run();
    check(bitEqual(a.first, b.first) && bitEqual(a.second, b.second), "the same scripted inputs give bit-identical outputs twice");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("camera rig math");
    testExpFollow();
    testLagFollow();
    testAngleFollow();
    testBasis();
    testBlend();
    testLookAhead();
    testResolveArm();
    testPresets();
    testDeterminism();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
