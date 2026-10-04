// OBJECT ANIMATION: a transform clip (kOcAnimObject) on an entity with no rig moves that entity's own
// CLocal, and only while the host says object animation is live.
//
// What this pins is the recurring failure of a feature that loads, stores and is never read: a clip
// that resolves and moves nothing, one that moves things in the editor where Unreal would not, a
// playback that composes the placement and the route in the wrong order, and a base that outlives
// the session it belonged to. The world point is checked numerically against the playback rule
// written as plain function composition, B(A(t0)^-1(A(t)(p))), which shares no code with the
// system's matrix chain.
#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

// Records one assertion.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Records one assertion that two floats agree to `eps`.
static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.4f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.5f}, want {:.5f})", what, got, want);
}

// Records one assertion that two points agree to `eps` centimetres.
static void checkPoint(const Vec3& got, const Vec3& want, f32 eps, const std::string& what) {
    if ((got - want).size() <= eps) {
        AVER_INFO("  ok    {} ({:.2f}, {:.2f}, {:.2f})", what, got.x, got.y, got.z);
        return;
    }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.3f}, {:.3f}, {:.3f}; want {:.3f}, {:.3f}, {:.3f})", what,
               got.x, got.y, got.z, want.x, want.y, want.z);
}

static constexpr f32 kEps = 0.05f;

static std::string g_dir;
static u64 kMoveClip  = 0x0B1EC70001ull;   // the route, flagged kOcAnimObject
static u64 kPlainClip = 0x0B1EC70002ull;   // the same track WITHOUT the flag
static u64 kGateClip  = 0x0B1EC70003ull;   // the route plus a notify at 1.0 s

static std::string resolvePath(u64 id, void*) {
    if (id == kMoveClip)  return g_dir + "/move.ocanim";
    if (id == kPlainClip) return g_dir + "/plain.ocanim";
    if (id == kGateClip)  return g_dir + "/gate.ocanim";
    return {};
}

static std::vector<std::string> g_fired;
static void recordNotify(scene::Entity, const char* name, void*) { g_fired.emplace_back(name); }

// The route: two one-second segments. Position slides, and the object turns about Z as it goes.
static constexpr f32 kRouteSeconds = 2.0f;
static const Vec3 kKeyPos[3] = {{1000, 0, 0}, {1000, 300, 0}, {700, 300, 0}};
static const f32  kKeyDeg[3] = {0.0f, 90.0f, 180.0f};

// A rotate-then-translate transform (with an optional uniform scale first), applied to points.
struct Rigid {
    Quat r;
    Vec3 p;
    f32  s = 1.0f;
};
static Vec3 applyRigid(const Rigid& x, const Vec3& v) { return x.r.rotate(v * x.s) + x.p; }
static Vec3 applyRigidInverse(const Rigid& x, const Vec3& v) {
    const Quat c{-x.r.x, -x.r.y, -x.r.z, x.r.w};
    return c.rotate(v - x.p) * (1.0f / x.s);
}

// A(t), worked out here from the key table rather than through the sampler.
static Rigid route(f32 t) {
    t = t < 0.0f ? 0.0f : (t > kRouteSeconds ? kRouteSeconds : t);
    const int k = t >= 1.0f ? 1 : 0;
    const f32 a = t - static_cast<f32>(k);
    // Rotation keys are slerped (AnimSampler): between two turns about Z that is a turn by the
    // linearly interpolated angle (the keys are under 180 degrees apart, the shortest arc).
    Rigid r;
    r.p = lerp(kKeyPos[k], kKeyPos[k + 1], a);
    r.r = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(kKeyDeg[k] + (kKeyDeg[k + 1] - kKeyDeg[k]) * a));
    return r;
}

// Where an animator clock lands inside the clip: wrapped for a loop, clamped for a one-shot.
static f32 wrapTime(f32 clock, bool once) {
    if (once) return clock < 0.0f ? 0.0f : (clock > kRouteSeconds ? kRouteSeconds : clock);
    const f32 t = std::fmod(clock, kRouteSeconds);
    return t < 0.0f ? t + kRouteSeconds : t;
}

// The playback rule: B(A(t0)^-1(A(t)(p))), A(t) acting first.
static Vec3 expectedPoint(const Rigid& base, f32 t0Clock, f32 clock, bool once, const Vec3& p) {
    const Rigid a0 = route(wrapTime(t0Clock, once));
    const Rigid at = route(wrapTime(clock, once));
    return applyRigid(base, applyRigidInverse(a0, applyRigid(at, p)));
}

// The route as an .ocanim: one track on bone 0, translation + rotation, three keys.
static fmt::OcAnimation makeRouteClip(u8 flags) {
    fmt::OcAnimation a;
    a.duration = kRouteSeconds;
    a.flags = flags;
    fmt::OcTrack t;
    t.boneIndex = 0;
    t.channels = static_cast<u8>(fmt::kOcChannelTranslation | fmt::kOcChannelRotation);
    t.interp = fmt::OcInterp::Linear;
    t.times = {0.0f, 1.0f, 2.0f};
    for (int k = 0; k < 3; ++k) {
        const Quat q = Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(kKeyDeg[k]));
        t.values.insert(t.values.end(),
                        {kKeyPos[k].x, kKeyPos[k].y, kKeyPos[k].z, q.x, q.y, q.z, q.w});
    }
    a.tracks.push_back(t);
    return a;
}

// An entity at `base` carrying an animator on `clip`. Speed and blend weight stay zero-filled, which
// the system reads as 1.
static scene::Entity makeObject(scene::World& w, const char* name, const Rigid& base, u64 clip,
                                f32 time, u32 flags = 0) {
    const scene::Entity e = w.create(name);
    Transform xf;
    xf.position = base.p;
    xf.rotation = base.r;
    xf.scale = Vec3{base.s, base.s, base.s};
    w.setLocalTransform(e, xf);
    auto* a = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
    a->clip = clip;
    a->time = time;
    a->flags = flags;
    return e;
}

// Fetched on demand: a pointer held across other entities' component adds can dangle.
static scene::CAnimator* animator(scene::World& w, scene::Entity e) {
    return w.component<scene::CAnimator>(e, scene::kComponentAnimator);
}

// A mesh-space point in the world, through the entity's composed world matrix (row-vector).
static Vec3 worldPoint(scene::World& w, scene::Entity e, const Vec3& p) {
    const Mat4& m = w.worldMatrix(e);
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

int main() {
    AVER_INFO("ObjectAnimTest");

    g_dir = (std::filesystem::temp_directory_path() / "aver-objectanim-test").string();
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);

    {
        std::string why;
        check(fmt::saveOcAnim(g_dir + "/move.ocanim", makeRouteClip(fmt::kOcAnimObject), &why),
              "the object clip writes: " + why);
        check(fmt::saveOcAnim(g_dir + "/plain.ocanim", makeRouteClip(0), &why),
              "the same track without the object flag writes: " + why);
        fmt::OcAnimation gate = makeRouteClip(fmt::kOcAnimObject);
        gate.notifies = {{1.0f, "Gate"}};
        check(fmt::saveOcAnim(g_dir + "/gate.ocanim", gate, &why), "the clip with a notify writes: " + why);
    }

    scene::World& w = scene::World::instance();
    anim::AnimSystem sys;
    sys.setResolver(&resolvePath, nullptr);

    // A non-identity placement, and a point on the mesh that is not at its origin so rotation shows.
    const Rigid placement{Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(20.0f)), Vec3{5000, 2000, 100}};
    const Rigid otherPlacement{Quat::fromAxisAngle(Vec3{0, 0, 1}, radians(-35.0f)), Vec3{-800, 1200, 40}};
    const Vec3 pt{30.0f, -10.0f, 5.0f};

    // `car` lives through the first three sections: not live, then live, then looping.
    const scene::Entity car = makeObject(w, "car", placement, kMoveClip, 0.5f);

    AVER_INFO("not live: an object clip changes nothing, and its clock holds");
    {
        check(!sys.objectAnimationLive(), "object animation is not live by default");
        const u32 rev = w.component<scene::CLocal>(car, scene::kComponentLocal)->rev;
        sys.tick(w, 0.5f);
        sys.tick(w, 0.5f);
        check(sys.loadedClips() == 1, "the clip did load, so a still object is not a missing asset");
        checkNear(animator(w, car)->time, 0.5f, 1e-6f, "the clock did not advance");
        checkPoint(worldPoint(w, car, pt), applyRigid(placement, pt), kEps, "the object stays at its placement");
        check(w.component<scene::CLocal>(car, scene::kComponentLocal)->rev == rev, "CLocal was never written");
        check(sys.objectAnimatedEntities() == 0, "no base is captured");
    }

    AVER_INFO("live: the object plays F(t) = B * A(t0)^-1 * A(t), t0 = 0.5");
    {
        sys.setObjectAnimationLive(true);
        check(sys.objectAnimationLive(), "object animation goes live");

        sys.tick(w, 0.5f);   // clock 0.5 -> 1.0
        checkNear(animator(w, car)->time, 1.0f, 1e-5f, "the clock advanced by dt, speed 0 reading as 1");
        check(sys.objectAnimatedEntities() == 1, "the entity's base is captured");
        const Vec3 at1 = worldPoint(w, car, pt);
        checkPoint(at1, expectedPoint(placement, 0.5f, 1.0f, false, pt), kEps,
                   "at clip time 1.0 the mesh point is where the rule puts it");
        check((at1 - applyRigid(placement, pt)).size() > 50.0f, "and it really moved off its placement");

        sys.tick(w, 0.5f);   // 1.5
        checkPoint(worldPoint(w, car, pt), expectedPoint(placement, 0.5f, 1.5f, false, pt), kEps,
                   "at clip time 1.5, on the second segment");
    }

    AVER_INFO("a looping animator wraps into the clip");
    {
        sys.tick(w, 0.5f);   // clock 2.0, which wraps to 0
        checkNear(animator(w, car)->time, 2.0f, 1e-5f, "the raw clock keeps counting");
        checkPoint(worldPoint(w, car, pt), expectedPoint(placement, 0.5f, 2.0f, false, pt), kEps,
                   "a clock of exactly one duration is back at the start of the route");
        sys.tick(w, 0.25f);  // clock 2.25 -> 0.25
        checkPoint(worldPoint(w, car, pt), expectedPoint(placement, 0.5f, 2.25f, false, pt), kEps,
                   "and past it lands a quarter second into the next lap");
        w.destroy(car);
        w.flush();
    }

    AVER_INFO("a placement is where the object is at its start time");
    {
        const scene::Entity e = makeObject(w, "atStart", placement, kMoveClip, 1.25f);
        sys.tick(w, 0.0f);
        checkPoint(worldPoint(w, e, pt), applyRigid(placement, pt), kEps,
                   "on its first live tick, with no time passed, the object sits exactly at B");

        Rigid scaled = placement;
        scaled.s = 1.5f;
        const scene::Entity s = makeObject(w, "scaled", scaled, kMoveClip, 0.5f);
        sys.tick(w, 0.5f);   // the scaled one: 0.5 -> 1.0
        checkPoint(worldPoint(w, s, pt), expectedPoint(scaled, 0.5f, 1.0f, false, pt), kEps,
                   "a scaled placement composes with the route");
        checkNear(w.localTransform(s).scale.x, 1.5f, 1e-4f, "and keeps its scale");
        w.destroy(e);
        w.destroy(s);
        w.flush();
    }

    AVER_INFO("kAnimatorOnce clamps at the end");
    {
        const scene::Entity e = makeObject(w, "once", placement, kMoveClip, 0.5f, scene::kAnimatorOnce);
        sys.tick(w, 5.0f);   // clock 5.5, far past the 2 s clip
        const Vec3 end = worldPoint(w, e, pt);
        checkPoint(end, expectedPoint(placement, 0.5f, 5.5f, true, pt), kEps, "a one-shot sits on the last key");
        check((end - expectedPoint(placement, 0.5f, 5.5f, false, pt)).size() > 50.0f,
              "which is not where a looping clock of 5.5 would be (1.5 into the route)");
        sys.tick(w, 1.0f);
        checkPoint(worldPoint(w, e, pt), end, 1e-3f, "and holds there");
        w.destroy(e);
        w.flush();
    }

    AVER_INFO("a paused animator holds, and scrubbing it repositions the object");
    {
        const scene::Entity e = makeObject(w, "paused", placement, kMoveClip, 0.5f, scene::kAnimatorPaused);
        sys.tick(w, 1.0f);
        checkNear(animator(w, e)->time, 0.5f, 1e-6f, "the clock does not move");
        checkPoint(worldPoint(w, e, pt), applyRigid(placement, pt), kEps, "so the object stays at B");
        animator(w, e)->time = 1.5f;
        sys.tick(w, 0.0f);
        checkPoint(worldPoint(w, e, pt), expectedPoint(placement, 0.5f, 1.5f, false, pt), kEps,
                   "a scrubbed clock moves it, relative to the start time captured while paused");
        w.destroy(e);
        w.flush();
    }

    AVER_INFO("going not-live drops the base, and the next session captures afresh");
    {
        const scene::Entity e = makeObject(w, "session", placement, kMoveClip, 0.5f);
        sys.tick(w, 0.5f);
        check(sys.objectAnimatedEntities() == 1, "the first session captured a base");

        sys.setObjectAnimationLive(false);
        check(sys.objectAnimatedEntities() == 0, "it is dropped the moment object animation stops");
        const Vec3 left = worldPoint(w, e, pt);
        const f32 timeLeft = animator(w, e)->time;
        sys.tick(w, 0.5f);
        checkPoint(worldPoint(w, e, pt), left, 1e-4f, "not live leaves the object where the session left it");
        checkNear(animator(w, e)->time, timeLeft, 1e-6f, "and holds its clock");

        // What a host does between sessions: put the placement and the authored time back.
        Transform xf;
        xf.position = otherPlacement.p;
        xf.rotation = otherPlacement.r;
        w.setLocalTransform(e, xf);
        animator(w, e)->time = 0.5f;
        sys.setObjectAnimationLive(true);
        sys.tick(w, 0.0f);
        checkPoint(worldPoint(w, e, pt), applyRigid(otherPlacement, pt), kEps,
                   "the second session starts at the restored placement, not the first session's base");
        sys.tick(w, 0.5f);
        checkPoint(worldPoint(w, e, pt), expectedPoint(otherPlacement, 0.5f, 1.0f, false, pt), kEps,
                   "and plays relative to it");
        w.destroy(e);
        w.flush();
    }

    AVER_INFO("only an unrigged entity with an object-flagged clip is driven");
    {
        const scene::Entity rigged = makeObject(w, "rigged", placement, kMoveClip, 0.0f);
        w.addComponent(rigged, scene::kComponentSkeletalMesh);
        const scene::Entity plain = makeObject(w, "plain", placement, kPlainClip, 0.0f);
        sys.tick(w, 0.5f);
        checkPoint(worldPoint(w, rigged, pt), applyRigid(placement, pt), 1e-3f,
                   "a rigged entity is left to the skeletal path even when its clip carries the flag");
        checkNear(animator(w, rigged)->time, 0.5f, 1e-5f, "and its clock advances as it always did");
        checkPoint(worldPoint(w, plain, pt), applyRigid(placement, pt), 1e-3f,
                   "a clip without the object flag never moves an entity");
        check(sys.objectAnimatedEntities() == 0, "neither has a base");
        w.destroy(rigged);
        w.destroy(plain);
        w.flush();
    }

    AVER_INFO("notifies on an object clip run on the same clock");
    {
        sys.setNotifySink(&recordNotify, nullptr);
        sys.setObjectAnimationLive(false);
        const scene::Entity e = makeObject(w, "gate", placement, kGateClip, 0.0f);

        g_fired.clear();
        sys.tick(w, 2.0f);
        check(g_fired.empty(), "not live: the held clock crosses no notify");
        checkNear(animator(w, e)->time, 0.0f, 1e-6f, "and has not moved");

        sys.setObjectAnimationLive(true);
        sys.tick(w, 0.5f);   // 0 -> 0.5
        check(g_fired.empty(), "live: a step short of the marker fires nothing");
        sys.tick(w, 0.75f);  // 0.5 -> 1.25, over Gate at 1.0
        check(g_fired.size() == 1 && g_fired[0] == "Gate", "and the step across it fires it once");
        checkPoint(worldPoint(w, e, pt), expectedPoint(placement, 0.0f, 1.25f, false, pt), kEps,
                   "while the entity moved on the same clock");

        sys.setNotifySink(nullptr, nullptr);
        w.destroy(e);
        w.flush();
    }

    AVER_INFO("a base is dropped when its entity dies, changes clip, or the system clears");
    {
        const scene::Entity a = makeObject(w, "a", placement, kMoveClip, 0.0f);
        const scene::Entity b = makeObject(w, "b", placement, kMoveClip, 0.0f);
        sys.tick(w, 0.1f);
        check(sys.objectAnimatedEntities() == 2, "two entities are driven");

        w.destroy(a);
        w.flush();
        sys.tick(w, 0.1f);
        check(sys.objectAnimatedEntities() == 1, "a destroyed entity's base goes with it");

        animator(w, b)->clip = kPlainClip;
        sys.tick(w, 0.1f);
        check(sys.objectAnimatedEntities() == 0, "pointing the animator at a non-object clip drops it");

        animator(w, b)->clip = kMoveClip;
        sys.tick(w, 0.1f);
        check(sys.objectAnimatedEntities() == 1, "and pointing it back captures again");

        sys.clear();
        check(sys.objectAnimatedEntities() == 0, "clear() drops every base");
        w.destroy(b);
        w.flush();
    }

    AVER_INFO("paused (Play paused): clocks hold, bases are kept, nothing is written, resuming carries on");
    {
        sys.setObjectAnimationLive(true);
        check(!sys.objectAnimationPaused(), "object animation is not paused by default");

        const scene::Entity e = makeObject(w, "pauseObj", placement, kMoveClip, 0.5f);
        const scene::Entity rigged = makeObject(w, "pauseRigged", placement, kMoveClip, 0.0f);
        w.addComponent(rigged, scene::kComponentSkeletalMesh);
        sys.tick(w, 0.5f);   // 0.5 -> 1.0
        check(sys.objectAnimatedEntities() == 1, "the object's base is captured before the pause");
        const Vec3 atPause = worldPoint(w, e, pt);
        const u32 rev = w.component<scene::CLocal>(e, scene::kComponentLocal)->rev;
        const f32 riggedBefore = animator(w, rigged)->time;

        sys.setObjectAnimationPaused(true);
        check(sys.objectAnimationPaused(), "the pause is set");
        sys.tick(w, 0.5f);
        sys.tick(w, 0.5f);
        checkNear(animator(w, e)->time, 1.0f, 1e-6f, "the object's clock holds while paused");
        checkPoint(worldPoint(w, e, pt), atPause, 1e-4f, "so it stays where the pause found it");
        check(w.component<scene::CLocal>(e, scene::kComponentLocal)->rev == rev, "CLocal is never written");
        check(sys.objectAnimatedEntities() == 1, "and the base is kept, not dropped as an undriven one");
        check(animator(w, rigged)->time > riggedBefore + 0.9f, "a rigged animator still runs: the skeletal path is untouched");

        // Something else moving the entity during the pause is not undone by it.
        Transform moved = w.localTransform(e);
        moved.position = moved.position + Vec3{0.0f, 0.0f, 500.0f};
        w.setLocalTransform(e, moved);
        const Vec3 movedAt = worldPoint(w, e, pt);
        sys.tick(w, 0.5f);
        checkPoint(worldPoint(w, e, pt), movedAt, 1e-4f, "an entity moved during the pause is left alone");

        // The kept base is what resuming composes against: a base re-taken from the moved pose would
        // leave the object 500 cm above the route, this puts it back on it.
        sys.setObjectAnimationPaused(false);
        sys.tick(w, 0.5f);   // 1.0 -> 1.5
        checkNear(animator(w, e)->time, 1.5f, 1e-5f, "resuming continues from the held clock");
        checkPoint(worldPoint(w, e, pt), expectedPoint(placement, 0.5f, 1.5f, false, pt), kEps,
                   "and against the base captured at the start of the session");

        sys.setObjectAnimationPaused(true);
        sys.setObjectAnimationLive(false);
        check(!sys.objectAnimationPaused(), "going not-live clears the pause with the bases");
        check(sys.objectAnimatedEntities() == 0, "and drops them");
        sys.setObjectAnimationLive(true);
        w.destroy(e);
        w.destroy(rigged);
        w.flush();
    }

    std::filesystem::remove_all(g_dir, ec);
    AVER_INFO(g_failures ? "ObjectAnimTest: {} FAILURES" : "ObjectAnimTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
