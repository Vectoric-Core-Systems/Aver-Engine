// CameraModifiersTest -- the modifier stack (CameraModifiers.hpp): envelope in / hold / out, hold-until-removed, remove
// from the CURRENT strength, tag clears, exclusivity, the Reduce-motion gate (envelopes still end on schedule), id
// order and the 32-entry cap. Pure: no scene, no device.
#include "aver/camera/CameraModifiers.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cstdio>
#include <cmath>
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

static CameraModifier kick(float deg, float in, float hold, float out, u32 tag = 0, i32 pri = 0, u32 flags = 0) {
    CameraModifier m;
    m.kind = ModKind::FovKick; m.a = deg; m.inS = in; m.holdS = hold; m.outS = out; m.tag = tag; m.priority = pri; m.flags = flags;
    return m;
}

static void testEnvelope() {
    AVER_INFO("=== envelope ===");
    ModifierStack s;
    const u32 id = s.push(kick(10.0f, 1.0f, 2.0f, 1.0f));
    check(id != 0 && s.count() == 1, "push returns an id");
    ModResult r = s.evaluate(0.0f, true);
    check(r.fovAddRad == 0.0f, "at t = 0 the strength is 0");
    r = s.evaluate(0.5f, true);
    check(near(r.fovAddRad, radians(10.0f) * 0.5f, 1e-5f), "halfway through the in phase it is smoothstep(0.5) = 0.5, got " + f2s(degrees(r.fovAddRad)));
    r = s.evaluate(0.5f, true);
    check(near(r.fovAddRad, radians(10.0f), 1e-5f), "at the end of the in phase it is full");
    r = s.evaluate(1.0f, true);
    check(near(r.fovAddRad, radians(10.0f), 1e-5f), "held through the hold phase");
    r = s.evaluate(1.0f, true);
    check(near(r.fovAddRad, radians(10.0f), 1e-5f), "full at the end of the hold");
    r = s.evaluate(0.5f, true);
    check(near(r.fovAddRad, radians(10.0f) * 0.5f, 1e-5f), "halfway through the out phase it is 0.5");
    r = s.evaluate(0.6f, true);
    check(r.fovAddRad == 0.0f && s.count() == 0, "after the out phase it is exactly 0 and the entry is gone");

    ModifierStack t;
    t.push(kick(10.0f, 0.05f, 0.1f, 0.4f));
    r = t.evaluate(1.0f, true);
    check(r.fovAddRad == 0.0f && t.count() == 0, "a single long frame carries leftover time across every phase and finishes");
}

static void testHoldAndRemove() {
    AVER_INFO("=== hold until removed, remove from the current strength ===");
    ModifierStack s;
    CameraModifier z;
    z.kind = ModKind::FovScale; z.a = 0.5f; z.inS = 0.2f; z.holdS = -1.0f; z.outS = 0.4f;
    const u32 id = s.push(z);
    ModResult r;
    for (int i = 0; i < 100; ++i) r = s.evaluate(0.1f, true);
    check(near(r.fovScale, 0.5f, 1e-5f) && s.count() == 1, "holdS < 0 holds until removed (zoom 0.5)");
    check(s.remove(id, -1.0f), "remove finds it");
    r = s.evaluate(0.2f, true);
    check(near(r.fovScale, 0.75f, 1e-4f), "halfway through its own 0.4 s out it is halfway back, scale 0.75, got " + f2s(r.fovScale));

    // Remove mid-in: no jump.
    ModifierStack t;
    const u32 id2 = t.push(kick(10.0f, 1.0f, -1.0f, 1.0f));
    ModResult before = t.evaluate(0.3f, true);
    t.remove(id2, 1.0f);
    ModResult after = t.evaluate(0.0f, true);
    check(near(before.fovAddRad, after.fovAddRad, 1e-6f), "removing mid-blend starts from the CURRENT strength (no jump), " + f2s(degrees(before.fovAddRad)) + " vs " + f2s(degrees(after.fovAddRad)));
    after = t.evaluate(0.5f, true);
    check(after.fovAddRad < before.fovAddRad && after.fovAddRad > 0.0f, "and then fades");
    check(!t.remove(9999, -1.0f), "removing an unknown id is false");
    t.remove(id2, 0.0f);
    r = t.evaluate(0.1f, true);
    check(r.fovAddRad == 0.0f && t.count() == 0, "an explicit out of 0 ends it on the next evaluate");
}

static void testTagsAndIds() {
    AVER_INFO("=== tags and ids ===");
    ModifierStack s;
    const u32 a = s.push(kick(5.0f, 0.0f, -1.0f, 0.1f, 7));
    const u32 b = s.push(kick(5.0f, 0.0f, -1.0f, 0.1f, 8));
    const u32 c = s.push(kick(5.0f, 0.0f, -1.0f, 0.1f, 7));
    check(a < b && b < c && a != 0, "ids are unique and ordered by call order");
    check(s.clear(7) == 2, "clear(tag) counts only matching modifiers");
    s.evaluate(1.0f, true);
    check(s.count() == 1, "only the other tag survives");
    check(s.alphaOf(b) > 0.99f && s.alphaOf(a) < 0.0f, "and it is still at full strength");
    const u32 d = s.push(kick(5.0f, 0.0f, -1.0f, 0.1f, 9));
    check(d > c, "ids are never reused after removals");
    check(s.clear(0) == 2, "clear(0) takes everything");
    s.evaluate(1.0f, true);
    check(s.count() == 0, "and they all end");
}

static void testExclusive() {
    AVER_INFO("=== exclusive ===");
    ModifierStack s;
    s.push(kick(10.0f, 0.0f, -1.0f, 0.1f, 0, 0));                          // low priority
    s.push(kick(3.0f, 0.0f, -1.0f, 0.1f, 0, 5, kModExclusive));            // high priority, exclusive
    ModResult r = s.evaluate(0.1f, true);
    check(near(r.fovAddRad, radians(3.0f), 1e-5f), "an exclusive modifier at full strength silences the lower-priority one, got " + f2s(degrees(r.fovAddRad)));

    ModifierStack t;
    t.push(kick(10.0f, 0.0f, -1.0f, 0.1f, 0, 0));
    t.push(kick(3.0f, 1.0f, -1.0f, 0.1f, 0, 5, kModExclusive));
    r = t.evaluate(0.1f, true);
    check(near(r.fovAddRad, radians(10.0f) + 0.0f, radians(0.5f)), "while the exclusive one is still blending in, the lower one still counts");

    ModifierStack u;
    CameraModifier other;
    other.kind = ModKind::RollLean; other.a = 4.0f; other.holdS = -1.0f; other.priority = 0;
    u.push(other);
    u.push(kick(3.0f, 0.0f, -1.0f, 0.1f, 0, 5, kModExclusive));
    r = u.evaluate(0.1f, true);
    check(near(r.roll, radians(4.0f), 1e-5f), "exclusivity is per kind: a roll lean is unaffected");
}

static void testKinds() {
    AVER_INFO("=== kinds ===");
    ModifierStack s;
    CameraModifier m;
    m.holdS = -1.0f;
    m.kind = ModKind::Punch; m.a = 2.0f; m.b = -1.0f; m.c = 0.5f; s.push(m);
    m.kind = ModKind::RollLean; m.a = 6.0f; m.b = m.c = 0.0f; s.push(m);
    m.kind = ModKind::ArmScale; m.a = 1.5f; s.push(m);
    m.kind = ModKind::SocketOffset; m.a = 10.0f; m.b = 20.0f; m.c = 30.0f; s.push(m);
    m.kind = ModKind::FovScale; m.a = 0.6f; m.b = 0; m.c = 0; s.push(m);
    const ModResult r = s.evaluate(0.016f, true);
    check(near(r.punchPitch, radians(2.0f), 1e-5f) && near(r.punchYaw, radians(-1.0f), 1e-5f), "punch fills pitch and yaw offsets");
    check(near(r.roll, radians(0.5f + 6.0f), 1e-5f), "punch roll and lean roll add");
    check(near(r.armScale, 1.5f, 1e-6f) && near(r.fovScale, 0.6f, 1e-6f), "arm scale and zoom multiply in");
    check(near(r.socketAdd.x, 10.0f, 1e-5f) && near(r.socketAdd.y, 20.0f, 1e-5f) && near(r.socketAdd.z, 30.0f, 1e-5f), "socket offsets add in the camera frame");

    ModifierStack t;
    CameraModifier p;
    p.kind = ModKind::Punch; p.a = 5.0f; p.inS = 0.03f; p.holdS = 0.0f; p.outS = 0.3f;
    t.push(p);
    ModResult q;
    for (int i = 0; i < 60; ++i) q = t.evaluate(1.0f / 60.0f, true);
    check(q.punchPitch == 0.0f && q.punchYaw == 0.0f && q.roll == 0.0f && t.count() == 0, "a punch returns exactly 0 after its out phase");
}

static void testMotionGate() {
    AVER_INFO("=== reduce motion ===");
    ModifierStack s;
    s.push(kick(10.0f, 0.0f, 0.5f, 0.5f));                                     // gated by default
    CameraModifier z;
    z.kind = ModKind::FovScale; z.a = 0.5f; z.holdS = 0.5f; z.outS = 0.5f; s.push(z);   // not gated
    CameraModifier arm;
    arm.kind = ModKind::ArmScale; arm.a = 2.0f; arm.holdS = 0.5f; arm.outS = 0.5f; arm.flags = kModMotion; s.push(arm);   // forced gated
    ModResult r = s.evaluate(0.1f, false);
    check(r.fovAddRad == 0.0f && r.armScale == 1.0f, "gated kinds contribute 0 with motion off (the kick, and an arm scale flagged kModMotion)");
    check(near(r.fovScale, 0.5f, 1e-5f), "zoom is the player's own action and still applies");
    ModifierStack t;
    t.push(kick(10.0f, 0.0f, 0.5f, 0.5f));
    t.evaluate(0.1f, false);
    ModResult on = t.evaluate(0.0f, true);
    check(near(on.fovAddRad, radians(10.0f), 1e-5f), "the envelope kept advancing while gated: still in its hold");
    for (int i = 0; i < 12; ++i) t.evaluate(0.1f, false);
    check(t.count() == 0, "and a gated modifier ends on schedule");

    ModifierStack f;
    CameraModifier ng = kick(10.0f, 0.0f, -1.0f, 0.1f, 0, 0, kModNoMotionGate);
    f.push(ng);
    check(near(f.evaluate(0.1f, false).fovAddRad, radians(10.0f), 1e-5f), "kModNoMotionGate exempts a kick");
}

static void testCap() {
    AVER_INFO("=== cap ===");
    ModifierStack s;
    u32 last = 1;
    for (u32 i = 0; i < kMaxModifiers; ++i) last = s.push(kick(1.0f, 0.0f, -1.0f, 0.1f));
    check(last != 0 && s.count() == kMaxModifiers, "32 modifiers fit");
    check(s.push(kick(1.0f, 0.0f, -1.0f, 0.1f)) == 0, "the 33rd push is refused");
    s.clear(0);
    s.evaluate(1.0f, true);
    check(s.count() == 0 && s.push(kick(1.0f, 0.0f, -1.0f, 0.1f)) != 0, "after clearing, pushes work again");
}

static void testOrderAndDeterminism() {
    AVER_INFO("=== order and determinism ===");
    auto run = [] {
        ModifierStack s;
        s.push(kick(4.0f, 0.1f, 0.3f, 0.2f, 1, 3));
        CameraModifier p;
        p.kind = ModKind::Punch; p.a = 3.0f; p.b = 1.0f; p.inS = 0.02f; p.holdS = 0.0f; p.outS = 0.4f; p.priority = 1;
        s.push(p);
        s.push(kick(2.0f, 0.0f, 0.5f, 0.5f, 2, 2, kModExclusive));
        float acc = 0;
        for (int i = 0; i < 120; ++i) {
            const ModResult r = s.evaluate(1.0f / 60.0f, i % 7 != 0);
            acc += r.fovAddRad * 3.0f + r.punchPitch * 5.0f + r.punchYaw;
        }
        return acc;
    };
    const float a = run(), b = run();
    check(a == b, "the same pushes and frames give bit-identical sums");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("camera modifiers");
    testEnvelope();
    testHoldAndRemove();
    testTagsAndIds();
    testExclusive();
    testKinds();
    testMotionGate();
    testCap();
    testOrderAndDeterminism();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
