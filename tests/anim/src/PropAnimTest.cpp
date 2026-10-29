// Property-track animation with Godot's AnimationPlayer semantics (modules/anim PropAnim / PropPlayer, the .ocanim PROP chunk).
//
// Every property here belongs to a clock or to arithmetic on keys: which key a playhead met first, how a loop point splits a frame,
// what weight an outgoing clip has, which signal comes before which. None of it shows in a screenshot of one frame, so it is checked as
// numbers and as the order of events. No scene, no GPU.
#include "aver/anim/PropAnim.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::anim;
using fmt::OcAnimation;
using fmt::OcPropInterp;
using fmt::OcPropKey;
using fmt::OcPropKind;
using fmt::OcPropLoop;
using fmt::OcPropTrack;
using fmt::OcPropTrackType;
using fmt::OcPropUpdate;
using fmt::OcPropValue;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static void near(double got, double want, double eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

// ---- builders ----
static OcPropValue F(double v) { OcPropValue r; r.kind = OcPropKind::Float; r.n[0] = v; return r; }
static OcPropValue V3(double x, double y, double z) { OcPropValue r; r.kind = OcPropKind::Vec3; r.n[0] = x; r.n[1] = y; r.n[2] = z; return r; }
static OcPropValue Qt(double x, double y, double z, double w) { OcPropValue r; r.kind = OcPropKind::Quat; r.n[0] = x; r.n[1] = y; r.n[2] = z; r.n[3] = w; return r; }
static OcPropValue S(const std::string& s) { OcPropValue r; r.kind = OcPropKind::String; r.s = s; return r; }
static OcPropKey K(double t, OcPropValue v, double tr = 1.0) { OcPropKey k; k.time = t; k.value = std::move(v); k.transition = tr; return k; }
static OcPropTrack T(OcPropTrackType type, const std::string& path, std::vector<OcPropKey> keys, OcPropInterp in = OcPropInterp::Linear,
                     OcPropUpdate up = OcPropUpdate::Continuous) {
    OcPropTrack t;
    t.type = type; t.path = path; t.keys = std::move(keys); t.interp = in; t.update = up;
    return t;
}
static std::shared_ptr<OcAnimation> clipOf(double len, OcPropLoop loop, std::vector<OcPropTrack> tracks) {
    auto c = std::make_shared<OcAnimation>();
    c->duration = float(len);
    c->propLoop = loop;
    c->propTracks = std::move(tracks);
    return c;
}
static OcPropKey methodKey(double t, const std::string& name, std::vector<OcPropValue> args = {}) {
    OcPropKey k = K(t, S(name));
    k.args = std::move(args);
    return k;
}

static double sampleF(const OcAnimation& c, usize track, double t, bool back = false) {
    PropSample s = propSample(c, c.propTracks[track], t, back);
    return s.ok ? s.value.n[0] : -9999.0;
}

// ---- event helpers ----
static std::string tag(const PropEvent& e) {
    switch (e.type) {
        case PropEvent::Started: return "started:" + e.a;
        case PropEvent::Finished: return "finished:" + e.a;
        case PropEvent::Changed: return "changed:" + e.a + ">" + e.b;
        case PropEvent::CurrentChanged: return "current:" + e.a;
        case PropEvent::Looped: return "looped:" + e.a;
        case PropEvent::Method: return "call:" + e.a;
        case PropEvent::AudioPlay: return "audio:" + e.a;
        case PropEvent::AudioStop: return "audiostop";
        case PropEvent::AnimPlay: return "animplay:" + e.a;
        case PropEvent::AnimStop: return "animstop";
        case PropEvent::Applied: return "applied";
        case PropEvent::MixerApplied: return "mixer";
    }
    return "?";
}
// The pass's events without the bookkeeping markers, joined with ' '.
static std::string seq(PropPlayer& p, bool markers = false) {
    std::string r;
    for (const PropEvent& e : p.events()) {
        if (!markers && (e.type == PropEvent::Applied || e.type == PropEvent::MixerApplied)) continue;
        if (!r.empty()) r += ' ';
        r += tag(e);
    }
    p.clearResults();
    return r;
}
static const PropOutput* out(const PropPlayer& p, const std::string& path) {
    const PropOutput* last = nullptr;
    for (const PropOutput& o : p.outputs()) if (p.trackPath(o.id) == path) last = &o;
    return last;
}
static double outF(const PropPlayer& p, const std::string& path) {
    const PropOutput* o = out(p, path);
    return o ? o->value.n[0] : -9999.0;
}

// ---- tests ----

static void testInterpolation() {
    AVER_INFO("interpolation");
    auto line = clipOf(2, OcPropLoop::None, {
        T(OcPropTrackType::Value, "n:lin", {K(0, F(0)), K(1, F(10)), K(2, F(0))}),
        T(OcPropTrackType::Value, "n:near", {K(0, F(0)), K(1, F(10)), K(2, F(0))}, OcPropInterp::Nearest),
        T(OcPropTrackType::Value, "n:ease", {K(0, F(0), 2.0), K(1, F(10)), K(2, F(0))}),
        T(OcPropTrackType::Value, "n:hold", {K(0, F(0), 0.0), K(1, F(10)), K(2, F(0))}),
    });
    near(sampleF(*line, 0, 0.5), 5, 1e-9, "linear, rising half");
    near(sampleF(*line, 0, 1.5), 5, 1e-9, "linear, falling half");
    near(sampleF(*line, 0, 1.0), 10, 1e-9, "linear, on a key");
    near(sampleF(*line, 0, 2.0), 0, 1e-9, "linear, on the last key");
    near(sampleF(*line, 0, 5.0), 0, 1e-9, "past the end holds the last key");
    near(sampleF(*line, 1, 0.99), 0, 1e-9, "nearest keeps the key before");
    near(sampleF(*line, 1, 1.0), 10, 1e-9, "nearest on the next key");
    near(sampleF(*line, 2, 0.5), 2.5, 1e-9, "transition 2 eases in: ease(0.5, 2) = 0.25");
    near(sampleF(*line, 3, 0.9), 0, 1e-9, "transition 0 holds the key until the next");
    near(sampleF(*line, 0, 0.25, true), 2.5, 1e-9, "backward over a linear segment is the same line");
    near(sampleF(*line, 2, 0.5, true), 5, 1e-9, "backward uses the transition of the key it starts from (the later one, here linear), not the earlier key's ease");

    auto cubic = clipOf(3, OcPropLoop::None, {
        T(OcPropTrackType::Value, "n:c", {K(0, F(0)), K(1, F(0)), K(2, F(10)), K(3, F(30))}, OcPropInterp::Cubic),
        T(OcPropTrackType::Value, "n:ramp", {K(0, F(0)), K(1, F(10)), K(2, F(20)), K(3, F(30))}, OcPropInterp::Cubic),
    });
    near(sampleF(*cubic, 0, 1.5), 3.75, 1e-9, "cubic mid-segment is Catmull-Rom: (-0 + 9*0 + 9*10 - 30) / 16");
    near(sampleF(*cubic, 0, 1.0), 0, 1e-9, "cubic passes through its keys");
    near(sampleF(*cubic, 0, 2.0), 10, 1e-9, "cubic passes through its next key");
    near(sampleF(*cubic, 1, 1.5), 15, 1e-9, "cubic reproduces a straight line");
    near(sampleF(*cubic, 1, 0.5), 3.75, 1e-9, "cubic at the first segment has no earlier neighbour: it is clamped to the key itself, which flattens the start");

    auto ang = clipOf(1, OcPropLoop::None, {
        T(OcPropTrackType::Value, "n:ang", {K(0, F(6.0)), K(1, F(0.5))}, OcPropInterp::LinearAngle),
        T(OcPropTrackType::Value, "n:flat", {K(0, F(6.0)), K(1, F(0.5))}, OcPropInterp::Linear),
    });
    near(sampleF(*ang, 0, 0.5), 0.1084074, 1e-5, "linear_angle takes the short way round (6.0 to 0.5 through 2 pi)");
    near(sampleF(*ang, 1, 0.5), 3.25, 1e-9, "plain linear takes the long way");

    auto vec = clipOf(1, OcPropLoop::None, {
        T(OcPropTrackType::Position3D, "n", {K(0, V3(0, 0, 0)), K(1, V3(10, 20, 30))}),
        T(OcPropTrackType::Rotation3D, "n", {K(0, Qt(0, 0, 0, 1)), K(1, Qt(0, 0, std::sqrt(0.5), std::sqrt(0.5)))}),
    });
    PropSample v = propSample(*vec, vec->propTracks[0], 0.5);
    check(v.ok && std::fabs(v.value.n[0] - 5) < 1e-9 && std::fabs(v.value.n[1] - 10) < 1e-9 && std::fabs(v.value.n[2] - 15) < 1e-9, "position track lerps each axis");
    PropSample q = propSample(*vec, vec->propTracks[1], 0.5);
    near(q.value.n[2], 0.3826834, 1e-6, "rotation track slerps: half of 90 degrees about Z, z");
    near(q.value.n[3], 0.9238795, 1e-6, "rotation track slerps: w");

    // Loop wrap: the segment across the loop point is interpolated.
    auto loop = clipOf(2, OcPropLoop::Linear, {
        T(OcPropTrackType::Value, "n:w", {K(0.5, F(0)), K(1.5, F(10))}),
    });
    near(sampleF(*loop, 0, 0.0), 5, 1e-9, "linear loop: before the first key interpolates from the last key of the previous lap");
    near(sampleF(*loop, 0, 1.0), 5, 1e-9, "linear loop: between the keys");
    near(sampleF(*loop, 0, 1.75), 7.5, 1e-9, "linear loop: after the last key interpolates towards the first key of the next lap");
    loop->propTracks[0].loopWrap = false;
    near(sampleF(*loop, 0, 0.0), 0, 1e-9, "loop_wrap off: before the first key holds it");
    near(sampleF(*loop, 0, 1.75), 10, 1e-9, "loop_wrap off: after the last key holds it");

    // A key past the animation's length is not part of the animation.
    auto cut = clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:cut", {K(0, F(0)), K(0.5, F(4)), K(3, F(100))})});
    near(sampleF(*cut, 0, 0.9), 4, 1e-9, "a key beyond the length is ignored");

    // Bezier.
    OcPropKey b0 = K(0, F(0)); b0.handle[2] = 0.25; b0.handle[3] = 0;
    OcPropKey b1 = K(1, F(10)); b1.handle[0] = -0.25; b1.handle[1] = 0;
    OcPropTrack bz = T(OcPropTrackType::Bezier, "n:b", {b0, b1});
    near(propSampleBezier(bz, 0.5), 5, 1e-3, "bezier with symmetric flat handles passes through the middle");
    check(propSampleBezier(bz, 0.25) < 2.5, "bezier with flat handles is below the straight line early on");
    near(propSampleBezier(bz, 1.0), 10, 1e-9, "bezier ends on its key");
    near(propSampleBezier(bz, 7.0), 10, 1e-9, "bezier past the end holds");
    near(propSampleBezier(bz, -1.0), 0, 1e-9, "bezier before the start holds");
}

static void testKeysInRange() {
    AVER_INFO("keys in range");
    OcPropTrack t = T(OcPropTrackType::Method, "n", {methodKey(0.0, "a"), methodKey(0.5, "b"), methodKey(1.0, "c")});
    std::vector<int> r;
    propKeysInSegments(t, {{0.0, 0.5, false}}, r);
    check(r == std::vector<int>{1}, "forward (0, 0.5]: the start is excluded, the end included");
    r.clear(); propKeysInSegments(t, {{0.0, 0.5, true}}, r);
    check(r == std::vector<int>{0, 1}, "an include-start segment takes the key at its start");
    r.clear(); propKeysInSegments(t, {{1.0, 0.0, false}}, r);
    check(r == std::vector<int>{1, 0}, "backward [0, 1): descending, the far end excluded, the near end included");
    r.clear(); propKeysInSegments(t, {{0.2, 0.9, false}, {0.9, 1.0, false}}, r);
    check(r == std::vector<int>{1, 2}, "consecutive pieces neither miss nor repeat a key on the seam");
    check(propFindKey(t, 0.5, PropFind::Exact, false, 1.0) == 1, "find exact");
    check(propFindKey(t, 0.6, PropFind::Exact, false, 1.0) == -1, "find exact, none");
    check(propFindKey(t, 0.6, PropFind::Nearest, false, 1.0) == 1, "find nearest forward: the last key at or before");
    check(propFindKey(t, 0.6, PropFind::Nearest, true, 1.0) == 2, "find nearest backward: the first key at or after");
    check(propFindKey(t, 0.3, PropFind::Nearest, false, 0.4) == 0, "find ignores keys past the clip length");
}

static void testLoopsAndMethods() {
    AVER_INFO("loops and method calls");
    PropPlayer p;
    p.setClip("loop", clipOf(1, OcPropLoop::Linear, {T(OcPropTrackType::Method, "n", {methodKey(0.0, "m0"), methodKey(0.5, "m5")})}));
    p.play("loop");
    check(seq(p) == "current:loop started:loop", "play: current_animation_changed then animation_started");
    p.advance(0.4);
    check(seq(p) == "call:m0", "the first frame after play() does not advance, and the key at 0 fires");
    p.advance(0.4);
    check(seq(p) == "", "0.4: nothing passed");
    p.advance(0.4);
    check(seq(p) == "call:m5", "0.8: the 0.5 key fires");
    p.advance(0.4);
    check(seq(p) == "looped:loop call:m0", "1.2: wraps to 0.2, the loop signal, then the key at the restart (0)");
    near(p.position(), 0.2, 1e-9, "position after the wrap");
    check(p.isPlaying(), "a looping clip keeps playing");

    // A frame that skips several keys fires them in time order, forward and backward.
    PropPlayer q;
    q.setClip("many", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Method, "n", {methodKey(0.1, "k1"), methodKey(0.2, "k2"), methodKey(0.3, "k3"), methodKey(0.9, "k9")})}));
    q.play("many");
    q.advance(0.0);
    q.clearResults();
    q.advance(0.5);
    check(seq(q) == "call:k1 call:k2 call:k3", "a big step fires every key it passed, in order");
    q.advance(0.2);
    check(seq(q) == "", "the next step starts after the last key it passed (no repeat)");
    q.advance(0.5);
    check(seq(q) == "call:k9 finished:many", "the clip's end: the last key then animation_finished");

    PropPlayer r;
    r.setClip("many", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Method, "n", {methodKey(0.1, "k1"), methodKey(0.2, "k2"), methodKey(0.9, "k9"), methodKey(1.0, "kend")})}));
    r.play("many", -1, -1.0, true);    // play_backwards
    r.advance(0.0);
    check(seq(r) == "current:many started:many call:kend", "from_end: starts at the length, a key there fires");
    r.advance(0.5);
    check(seq(r) == "call:k9", "backward: keys in descending order, only the ones passed (0.5, 1.0)");
    r.advance(0.5);
    check(seq(r) == "call:k2 call:k1 finished:many", "backward to 0: descending, then finished");
    check(!r.isPlaying() && r.position() == 0.0, "stopped at the start");

    // Arguments travel with the call.
    PropPlayer s;
    s.setClip("arg", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Method, "Node/Child", {methodKey(0.0, "hit", {F(3), S("x")})})}));
    s.play("arg");
    s.advance(0.1);
    bool ok = false;
    for (const PropEvent& e : s.events())
        if (e.type == PropEvent::Method) ok = e.a == "hit" && e.path == "Node/Child" && e.args.size() == 2 && e.args[0].n[0] == 3 && e.args[1].s == "x";
    check(ok, "a method call carries its target path, name and arguments");

    // speed_scale: negative runs backwards; zero stands still.
    PropPlayer u;
    u.setClip("l", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:v", {K(0, F(0)), K(1, F(10))})}));
    u.speedScale = 2.0;
    u.play("l");
    u.advance(0.0); u.advance(0.25);
    near(u.position(), 0.5, 1e-9, "speed_scale 2 doubles the step");
    u.speedScale = 0.0;
    u.advance(0.25);
    near(u.position(), 0.5, 1e-9, "speed_scale 0 stands still");
    u.speedScale = -1.0;
    u.advance(0.25);
    near(u.position(), 0.25, 1e-9, "negative speed_scale runs backwards");
}

static void testPingPong() {
    AVER_INFO("ping-pong");
    PropPlayer p;
    p.setClip("pp", clipOf(1, OcPropLoop::PingPong, {
        T(OcPropTrackType::Value, "n:v", {K(0, F(0)), K(1, F(10))}),
        T(OcPropTrackType::Method, "n", {methodKey(0.0, "m0"), methodKey(1.0, "m1")}),
    }));
    p.play("pp");
    p.advance(0.75);
    check(seq(p) == "current:pp started:pp call:m0", "start fires the key at 0");
    p.advance(0.75);
    near(outF(p, "n:v"), 7.5, 1e-9, "forward: 0.75");
    p.advance(0.75);
    check(seq(p) == "looped:pp call:m1", "past the end: the player reports the bounce, and the end key fires once");
    near(p.position(), 0.5, 1e-9, "reflected: 1.5 -> 0.5");
    near(outF(p, "n:v"), 5, 1e-9, "value on the way back");
    near(p.playingSpeed(), -1.0, 1e-9, "the direction flipped");
    p.advance(0.75);
    near(p.position(), 0.25, 1e-9, "reflected at 0: -0.25 -> 0.25");
    near(outF(p, "n:v"), 2.5, 1e-9, "value after the second bounce");
    check(seq(p) == "looped:pp call:m0", "the key at 0 fires once at the bottom bounce");
    check(p.isPlaying(), "ping-pong never finishes");
}

static void testEndAndSeek() {
    AVER_INFO("end, seek, stop, pause");
    PropPlayer p;
    p.setClip("a", clipOf(2, OcPropLoop::None, {
        T(OcPropTrackType::Value, "n:v", {K(0, F(0)), K(2, F(20))}),
        T(OcPropTrackType::Method, "n", {methodKey(0.5, "half"), methodKey(1.5, "late")}),
    }));
    p.play("a");
    p.advance(0.0);
    p.clearResults();

    p.seek(1.0);
    check(seq(p) == "", "seek without update changes nothing visible");
    near(p.position(), 1.0, 1e-9, "seek moves the position");
    p.advance(0.1);
    check(seq(p) == "call:half", "after a seek the next pass skips the events between the old and new position and calls only the key at or before the new one");
    near(outF(p, "n:v"), 11, 1e-9, "the next pass starts from the sought position");
    p.seek(0.75, true);
    check(seq(p) == "call:late", "a backward seek with update calls the next key in its direction of travel");
    near(outF(p, "n:v"), 7.5, 1e-9, "seek with update applies the pose");
    p.seek(0.8, true);
    check(seq(p) == "call:half", "a forward seek with update calls the key at or before the target");
    p.seek(1.25, true, true);
    check(seq(p) == "", "update_only: method tracks are not processed");
    near(outF(p, "n:v"), 12.5, 1e-9, "update_only still applies values");
    p.seek(2.0, true);
    check(seq(p) == "call:late", "seeking to the end with update");
    check(!p.isPlaying(), "seeking to the end stops the player");

    PropPlayer q;
    q.setClip("a", clipOf(2, OcPropLoop::None, {T(OcPropTrackType::Value, "n:v", {K(0, F(0)), K(2, F(20))}), T(OcPropTrackType::Method, "n", {methodKey(0.0, "zero")})}));
    q.play("a");
    q.advance(0.0);
    q.clearResults();
    q.advance(0.5);
    q.pause();
    check(!q.isPlaying() && q.hasCurrent(), "pause keeps the animation");
    q.clearResults();
    q.play();
    check(seq(q) == "started:a", "play() with no name resumes the assigned animation");
    q.advance(0.0);
    near(q.position(), 0.5, 1e-9, "resumed at the paused position");
    q.advance(0.5);
    near(q.position(), 1.0, 1e-9, "and runs on");
    q.clearResults();
    q.stop();
    check(seq(q) == "current:", "stop(): current_animation_changed with an empty name, no method calls");
    check(!q.isPlaying() && q.current().empty() && q.assigned() == "a", "stopped; assigned_animation keeps the last name");
    near(outF(q, "n:v"), 0, 1e-9, "stop() resets the pose to the first frame");
    q.play("a");
    q.advance(0.0);
    check(seq(q) == "current:a started:a call:zero", "play() after stop() restarts from 0");

    // keep_state leaves the pose alone.
    PropPlayer k;
    k.setClip("a", clipOf(2, OcPropLoop::None, {T(OcPropTrackType::Value, "n:v", {K(0, F(0)), K(2, F(20))})}));
    k.play("a"); k.advance(0.0); k.advance(1.0); k.clearResults();
    k.stop(true);
    check(seq(k, true) == "current:", "stop(keep_state) takes no new pass (no applied marker)");
    check(!k.isPlaying(), "stop(keep_state) stops");

    // Replaying a finished clip starts it over; the end does not emit finished twice.
    PropPlayer f;
    f.setClip("a", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:v", {K(0, F(0)), K(1, F(10))})}));
    f.play("a"); f.advance(0.0); f.advance(2.0);
    check(seq(f) == "current:a started:a finished:a", "finished once at the end");
    near(outF(f, "n:v"), 10, 1e-9, "the final pose is applied in full");
    f.play("a");
    near(f.position(), 0.0, 1e-9, "play() on a finished clip rewinds it");
}

static void testBlend() {
    AVER_INFO("crossfade");
    auto mk = [](double v, bool withOnly) {
        std::vector<OcPropTrack> tr = {T(OcPropTrackType::Value, "n:x", {K(0, F(v)), K(10, F(v))})};
        if (withOnly) tr.push_back(T(OcPropTrackType::Value, "n:only", {K(0, F(3)), K(10, F(3))}));
        return clipOf(10, OcPropLoop::None, tr);
    };
    PropPlayer p;
    p.setClip("A", mk(0, true));
    p.setClip("B", mk(10, false));
    p.defaultBlendTime = 1.0;
    p.play("A");
    p.advance(0.1);
    near(outF(p, "n:x"), 0, 1e-9, "A alone");
    p.play("B");
    p.advance(0.0);
    near(outF(p, "n:x"), 0, 1e-9, "start of the crossfade: still entirely A (weight 1 - blend_left = 0)");
    p.advance(0.25);
    near(outF(p, "n:x"), 2.5, 1e-9, "a quarter in: B has weight 0.25");
    near(p.blendLeft(0), 0.75, 1e-9, "blend_left decays by delta / blend_time");
    near(outF(p, "n:only"), 3, 1e-9, "a target only the outgoing clip has keeps its value (weights are normalised per target)");
    p.advance(0.25);
    near(outF(p, "n:x"), 5, 1e-9, "halfway");
    p.advance(0.5);
    near(outF(p, "n:x"), 10, 1e-9, "done: B alone");
    check(p.blendCount() == 0, "the finished blend is dropped");
    p.advance(0.1);
    check(out(p, "n:only") == nullptr, "after the fade nothing drives the old clip's other targets");

    // A custom blend per call beats the default; set_blend_time beats the default; the wildcard works.
    PropPlayer c;
    c.setClip("A", mk(0, false)); c.setClip("B", mk(10, false)); c.setClip("C", mk(20, false));
    c.defaultBlendTime = 1.0;
    c.setBlendTime("B", "C", 4.0);
    c.setBlendTime("", "A", 0.5);
    near(c.explicitBlendTime("B", "C"), 4.0, 1e-9, "explicit pair");
    near(c.explicitBlendTime("C", "A"), 0.5, 1e-9, "wildcard from, to A");
    check(c.explicitBlendTime("A", "B") < 0, "no entry");
    c.play("A"); c.advance(0.0);
    c.play("B", 0.5);
    c.advance(0.0); c.advance(0.25);
    near(outF(c, "n:x"), 5, 1e-9, "custom blend 0.5: halfway after 0.25 s");
    c.advance(1.0);
    c.play("C");
    c.advance(0.0); c.advance(1.0);
    near(outF(c, "n:x"), 12.5, 1e-9, "set_blend_time(B, C, 4): a quarter of the way after 1 s: 10 + 0.25 * 10");

    // A second play() during a crossfade keeps the weights summing to one.
    PropPlayer m;
    m.setClip("A", mk(0, false)); m.setClip("B", mk(10, false)); m.setClip("C", mk(20, false));
    m.defaultBlendTime = 1.0;
    m.play("A"); m.advance(0.0);
    m.play("B"); m.advance(0.0); m.advance(0.5);
    m.play("C"); m.advance(0.0);
    double sum = 0.0;
    for (usize i = 0; i < m.blendCount(); ++i) sum += m.blendLeft(i);
    check(m.blendCount() == 2, "two clips are fading out");
    near(sum, 1.0, 1e-9, "their weights add up to one, so the new clip starts at weight zero");
    near(outF(m, "n:x"), 5, 1e-9, "the value does not jump when a third clip is started mid-fade (A 0.5, B 0.5, C 0)");

    // Deterministic mode does not normalise, so a target only a fading clip has fades towards its RESET value.
    PropPlayer d;
    auto reset = clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:only", {K(0, F(7))})});
    d.setClip("RESET", reset);
    d.setClip("A", mk(0, true));
    d.setClip("B", mk(10, false));
    d.defaultBlendTime = 1.0;
    d.deterministic = true;
    d.play("A"); d.advance(0.0);
    d.play("B"); d.advance(0.0); d.advance(0.25);
    near(outF(d, "n:only"), 7 + (3 - 7) * 0.75, 1e-9, "deterministic: init + (v - init) * weight, with the RESET clip's first key as init");
    d.deterministic = false;
    d.play("A", 0.0); d.advance(0.0);
    d.play("B"); d.advance(0.0); d.advance(0.25);
    near(outF(d, "n:only"), 3, 1e-9, "normalised: the same target stays at the clip's value");
}

static void testDiscreteAndCapture() {
    AVER_INFO("update modes");
    PropPlayer p;
    p.setClip("d", clipOf(2, OcPropLoop::None, {
        T(OcPropTrackType::Value, "n:d", {K(0, F(1)), K(1, F(2)), K(1.5, F(3))}, OcPropInterp::Linear, OcPropUpdate::Discrete),
        T(OcPropTrackType::Value, "n:c", {K(0, F(0)), K(2, F(20))}, OcPropInterp::Linear, OcPropUpdate::Capture),
    }));
    p.play("d");
    p.advance(0.0);
    check(out(p, "n:d") && out(p, "n:d")->discrete && outF(p, "n:d") == 1, "discrete: the first key is written when the clip starts");
    near(outF(p, "n:c"), 0, 1e-9, "capture mode samples like continuous");
    p.advance(0.6);
    check(out(p, "n:d") == nullptr, "discrete: nothing is written between keys");
    near(outF(p, "n:c"), 6, 1e-9, "capture mode, continuous value");
    p.advance(0.5);
    check(out(p, "n:d") && outF(p, "n:d") == 2, "discrete: written when the playhead passes the key");
    p.advance(0.5);
    int n = 0; double last = 0;
    for (const PropOutput& o : p.outputs()) if (p.trackPath(o.id) == "n:d") { ++n; last = o.value.n[0]; }
    check(n == 1 && last == 3, "discrete: the key at 1.5");

    PropPlayer q;
    q.setClip("d", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:d", {K(0.2, F(1)), K(0.4, F(2)), K(0.6, F(3))}, OcPropInterp::Linear, OcPropUpdate::Discrete)}));
    q.play("d"); q.advance(0.0); q.advance(1.0);
    std::vector<double> vals;
    for (const PropOutput& o : q.outputs()) vals.push_back(o.value.n[0]);
    check(vals == std::vector<double>{1, 2, 3}, "discrete: a frame that skips several keys writes each, in order");

    // Non-numeric values are written as they are, never blended.
    PropPlayer s;
    s.setClip("s", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:name", {K(0, S("a")), K(0.5, S("b"))})}));
    s.play("s"); s.advance(0.0);
    check(out(s, "n:name") && out(s, "n:name")->value.s == "a", "a string track holds its key");
    s.advance(0.3);
    check(out(s, "n:name") && out(s, "n:name")->value.s == "a", "a string track between two keys holds the earlier one");
    s.advance(0.3);
    check(out(s, "n:name") && out(s, "n:name")->value.s == "b", "and steps to the next key when it is reached");

    // Dominant discrete suppresses the continuous write of the same target on that pass.
    PropPlayer dm;
    dm.discrete = PropDiscrete::Dominant;
    dm.setClip("m", clipOf(1, OcPropLoop::None, {
        T(OcPropTrackType::Value, "n:x", {K(0, F(5))}, OcPropInterp::Linear, OcPropUpdate::Discrete),
        T(OcPropTrackType::Value, "n:x", {K(0, F(0)), K(1, F(10))}),
    }));
    dm.play("m"); dm.advance(0.0);
    int count = 0;
    for (const PropOutput& o : dm.outputs()) { ++count; (void)o; }
    check(count == 1 && dm.outputs()[0].discrete, "dominant: the discrete write wins over the continuous one");
    PropPlayer rc;
    rc.discrete = PropDiscrete::Recessive;
    rc.setClip("m", clipOf(1, OcPropLoop::None, {
        T(OcPropTrackType::Value, "n:x", {K(0, F(5))}, OcPropInterp::Linear, OcPropUpdate::Discrete),
        T(OcPropTrackType::Value, "n:x", {K(0, F(0)), K(1, F(10))}),
    }));
    rc.play("m"); rc.advance(0.0);
    check(rc.outputs().size() == 2 && rc.outputs()[0].discrete && !rc.outputs()[1].discrete, "recessive: the discrete write is made first and the continuous one lands after it");

    // Capture: a snapshot of the current values fades out while the new clip comes in.
    PropPlayer cp;
    cp.setClip("cap", clipOf(10, OcPropLoop::None, {T(OcPropTrackType::Value, "n:c", {K(0, F(100)), K(10, F(100))}, OcPropInterp::Linear, OcPropUpdate::Capture)}));
    auto tracks = cp.captureTracks("cap");
    check(tracks.size() == 1 && tracks[0].second == "n:c", "capture_tracks lists the targets in capture mode");
    cp.capture({{{OcPropTrackType::Value, "n:c"}, F(40)}}, 1.0, 0, 0);
    cp.play("cap");
    cp.advance(0.0);
    near(outF(cp, "n:c"), 40, 1e-9, "capture: starts from the captured value");
    cp.advance(0.5);
    near(outF(cp, "n:c"), 70, 1e-9, "capture: halfway between the snapshot and the clip");
    cp.advance(0.5);
    near(outF(cp, "n:c"), 100, 1e-9, "capture: the clip alone");

    // With another clip already current and no fade between the clips, the snapshot still fades in over the new one.
    PropPlayer cq;
    cq.setClip("other", clipOf(10, OcPropLoop::None, {T(OcPropTrackType::Value, "n:c", {K(0, F(0))})}));
    cq.setClip("cap", clipOf(10, OcPropLoop::None, {T(OcPropTrackType::Value, "n:c", {K(0, F(100)), K(10, F(100))}, OcPropInterp::Linear, OcPropUpdate::Capture)}));
    cq.play("other");
    cq.advance(0.0);
    cq.capture({{{OcPropTrackType::Value, "n:c"}, F(40)}}, 2.0, 0, 0);
    cq.play("cap");
    cq.advance(0.0);
    cq.advance(1.0);
    near(outF(cq, "n:c"), 70, 1e-9, "capture survives a play() that has no fade of its own: halfway through a 2 s snapshot");
}

static void testQueueAndSignals() {
    AVER_INFO("queue and signals");
    PropPlayer p;
    p.setClip("A", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:x", {K(0, F(0)), K(1, F(1))})}));
    p.setClip("B", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:x", {K(0, F(1)), K(1, F(2))})}));
    p.setClip("L", clipOf(1, OcPropLoop::Linear, {T(OcPropTrackType::Value, "n:x", {K(0, F(1)), K(1, F(2))})}));
    p.queue("A");
    check(seq(p) == "current:A started:A", "queue() on an idle player plays");
    p.queue("B");
    check(p.queued().size() == 1 && p.queued()[0] == "B", "queue() while playing waits");
    p.advance(0.0);
    p.clearResults();
    p.advance(1.0);
    check(seq(p) == "current:B started:B changed:A>B", "the queued animation takes over: started, then animation_changed (no finished for A)");
    p.advance(0.0);
    p.advance(1.0);
    check(seq(p) == "finished:B", "the last one finishes");
    check(p.queued().empty(), "queue consumed");

    p.play("A"); p.clearResults();
    p.queue("B");
    p.clearQueue();
    check(p.queued().empty(), "clear_queue");
    p.advance(0.0); p.clearResults();
    p.advance(1.0);
    check(seq(p) == "finished:A", "with the queue cleared the clip just finishes");

    PropPlayer l;
    l.setClip("L", clipOf(1, OcPropLoop::Linear, {T(OcPropTrackType::Value, "n:x", {K(0, F(1)), K(1, F(2))})}));
    l.setClip("B", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:x", {K(0, F(1)), K(1, F(2))})}));
    l.play("L"); l.queue("B"); l.advance(0.0); l.advance(5.5);
    check(l.current() == "L", "a looping clip blocks the queue");

    // set_next chains clips.
    PropPlayer n;
    n.setClip("A", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:x", {K(0, F(0)), K(1, F(1))})}));
    n.setClip("B", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "n:x", {K(0, F(1)), K(1, F(2))})}));
    n.setNext("A", "B");
    check(n.next("A") == "B", "animation_get_next");
    n.play("A"); n.advance(0.0); n.advance(1.0);
    check(n.current() == "B", "animation_set_next: A runs into B");

    // An unknown name is refused; the same name while playing restarts nothing.
    check(!n.play("nope"), "play() of an unknown animation fails");
    PropPlayer w;
    w.setClip("A", clipOf(2, OcPropLoop::None, {T(OcPropTrackType::Value, "n:x", {K(0, F(0)), K(2, F(1))})}));
    w.play("A"); w.advance(0.0); w.advance(0.5); w.clearResults();
    w.play("A");
    check(seq(w) == "" && std::fabs(w.position() - 0.5) < 1e-9, "play() of the playing animation does nothing");
    w.removeClip("A");
    check(!w.isPlaying() && w.clipKeys().empty(), "removing the playing clip stops it");
}

static void testTrackKinds() {
    AVER_INFO("audio and sub-player tracks, blend shape, rotation blend");
    PropPlayer p;
    OcPropKey a = K(0.5, S("res://s.wav")); a.startOffset = 0.25; a.endOffset = 0.5;
    p.setClip("x", clipOf(2, OcPropLoop::None, {
        T(OcPropTrackType::Audio, "Snd", {a}),
        T(OcPropTrackType::Animation, "Sub", {K(1.0, S("run")), K(1.5, S("[stop]"))}),
        T(OcPropTrackType::BlendShape, "Mesh:smile", {K(0, F(0)), K(2, F(1))}),
    }));
    p.play("x"); p.advance(0.0); p.clearResults();
    p.advance(0.6);
    const PropEvent* au = nullptr;
    for (const PropEvent& e : p.events()) if (e.type == PropEvent::AudioPlay) au = &e;
    check(au && au->path == "Snd" && au->a == "res://s.wav" && std::fabs(au->t0 - 0.25) < 1e-9 && std::fabs(au->t1 - 0.5) < 1e-9, "audio key: stream, target, start and end offset");
    near(outF(p, "Mesh:smile"), 0.3, 1e-9, "blend shape track");
    p.clearResults();
    p.advance(0.5);
    check(seq(p) == "animplay:run", "animation track plays the named animation");
    p.advance(0.5);
    check(seq(p) == "animstop", "the [stop] key stops the sub-player");
    p.seek(0.1, true);
    p.clearResults();
    p.seek(1.2, true);
    check(seq(p) == "audio:res://s.wav animplay:run", "seek with update starts the audio and sub-player animation the playhead is inside");
    bool seekOffset = false;
    p.seek(1.25, true);
    for (const PropEvent& e : p.events()) if (e.type == PropEvent::AnimPlay) seekOffset = std::fabs(e.t0 - 0.25) < 1e-9 && e.t1 == 1.0;
    check(seekOffset, "and reports how far into it");
    p.clearResults();
    p.pause();
    bool stopped = false;
    for (const PropEvent& e : p.events()) if (e.type == PropEvent::AudioStop && e.path == "Snd") stopped = true;
    check(stopped, "pause() stops the audio tracks' streams");

    // Rotation blend: half of each of two clips' rotations about Z.
    auto rot = [](double angle) {
        return clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Rotation3D, "n", {K(0, Qt(0, 0, std::sin(angle / 2), std::cos(angle / 2)))})});
    };
    PropPlayer r;
    r.setClip("A", rot(0));
    r.setClip("B", rot(1.5707963267948966));
    r.defaultBlendTime = 1.0;
    r.play("A"); r.advance(0.0);
    r.play("B"); r.advance(0.0); r.advance(0.5);
    const PropOutput* o = out(r, "n");
    near(o ? o->value.n[2] : 0, 0.3826834, 1e-6, "rotation blend: 45 degrees about Z at half weight, z");
    near(o ? o->value.n[3] : 0, 0.9238795, 1e-6, "rotation blend: w");
}

static void testFormat() {
    AVER_INFO(".ocanim PROP chunk");
    OcAnimation c;
    c.duration = 2.5f;
    c.propLoop = OcPropLoop::PingPong;
    c.propStep = 0.05f;
    OcPropKey bz = K(0.5, F(3), 0.5);
    bz.handle[0] = -0.1; bz.handle[1] = 0.2; bz.handle[2] = 0.3; bz.handle[3] = -0.4;
    OcPropKey au = K(1.0, S("a.wav")); au.startOffset = 0.1; au.endOffset = 0.2;
    c.propTracks = {
        T(OcPropTrackType::Value, "N/M:position:x", {K(0, F(1)), K(1, F(2), 2.0)}, OcPropInterp::CubicAngle, OcPropUpdate::Discrete),
        T(OcPropTrackType::Method, "Player", {methodKey(0.25, "go", {F(1), S("two"), V3(1, 2, 3)})}),
        T(OcPropTrackType::Bezier, ":scale:y", {bz}),
        T(OcPropTrackType::Audio, "Snd", {au}),
        T(OcPropTrackType::Rotation3D, "Skel:bone", {K(0, Qt(0, 0, 0, 1))}),
    };
    c.propTracks[0].loopWrap = false;
    c.propTracks[1].enabled = false;
    c.propTracks[2].imported = true;
    OcPropValue text; text.kind = OcPropKind::Text; text.s = "[1, 2, \"x\"]";
    c.propTracks[0].keys.push_back(K(2, text));
    std::vector<u8> bytes;
    std::string why;
    check(c.valid(), "a clip with only property tracks is valid");
    check(fmt::writeOcAnim(c, bytes, &why), "writes (" + why + ")");
    OcAnimation back;
    check(fmt::parseOcAnim(bytes.data(), bytes.size(), back, &why), "parses (" + why + ")");
    bool same = back.propTracks.size() == c.propTracks.size() && back.propLoop == c.propLoop && back.propStep == c.propStep && back.duration == c.duration;
    for (usize i = 0; same && i < c.propTracks.size(); ++i) {
        const OcPropTrack &x = c.propTracks[i], &y = back.propTracks[i];
        same = x.type == y.type && x.path == y.path && x.interp == y.interp && x.update == y.update && x.loopWrap == y.loopWrap &&
               x.enabled == y.enabled && x.imported == y.imported && x.keys.size() == y.keys.size();
        for (usize k = 0; same && k < x.keys.size(); ++k) {
            const OcPropKey &a2 = x.keys[k], &b2 = y.keys[k];
            same = a2.time == b2.time && a2.transition == b2.transition && a2.value == b2.value && a2.args == b2.args && a2.startOffset == b2.startOffset &&
                   a2.endOffset == b2.endOffset && a2.handle[0] == b2.handle[0] && a2.handle[1] == b2.handle[1] && a2.handle[2] == b2.handle[2] && a2.handle[3] == b2.handle[3];
        }
    }
    check(same, "every track, key, value, argument, handle and flag survives the round trip");

    // A clip without property tracks writes the bytes it always did: no PROP chunk.
    OcAnimation bone;
    bone.duration = 1.0f;
    fmt::OcTrack tr;
    tr.boneIndex = 0; tr.channels = fmt::kOcChannelTranslation; tr.times = {0.0f, 1.0f}; tr.values = {0, 0, 0, 1, 0, 0};
    bone.tracks.push_back(tr);
    std::vector<u8> b1, b2;
    check(fmt::writeOcAnim(bone, b1, &why), "bone clip writes");
    bone.propLoop = OcPropLoop::Linear;     // meaningless without property tracks: still no chunk
    check(fmt::writeOcAnim(bone, b2, &why) && b1 == b2, "a bone-only clip's bytes do not depend on the property fields");
    OcAnimation withProp = bone;
    withProp.propTracks = {T(OcPropTrackType::Value, "n:v", {K(0, F(1))})};
    std::vector<u8> b3;
    check(fmt::writeOcAnim(withProp, b3, &why) && b3.size() > b1.size(), "property tracks add a chunk");
    OcAnimation back2;
    check(fmt::parseOcAnim(b3.data(), b3.size(), back2, &why) && back2.tracks.size() == 1 && back2.propTracks.size() == 1, "bone and property tracks live in one clip");
    OcAnimation back3;
    check(fmt::parseOcAnim(b1.data(), b1.size(), back3, &why) && back3.propTracks.empty(), "an old file has no property tracks");

    // A property animation with no tracks (a Godot Animation can be only a length) round-trips.
    OcAnimation empty;
    empty.duration = 3.5f;
    empty.propClip = true;
    empty.propLoop = OcPropLoop::Linear;
    std::vector<u8> be;
    OcAnimation emptyBack;
    check(fmt::writeOcAnim(empty, be, &why) && fmt::parseOcAnim(be.data(), be.size(), emptyBack, &why) && emptyBack.propClip && emptyBack.propTracks.empty() &&
              emptyBack.duration == 3.5f && emptyBack.propLoop == OcPropLoop::Linear,
          "an animation with no tracks round-trips (" + why + ")");
    OcAnimation notProp;
    notProp.duration = 1.0f;
    check(!fmt::writeOcAnim(notProp, be, &why), "a clip with nothing at all is still refused");

    // Unsorted keys are refused at write, and a truncated PROP chunk at read.
    OcAnimation bad = c;
    bad.propTracks[0].keys = {K(1, F(1)), K(0, F(2))};
    check(!fmt::writeOcAnim(bad, bytes, &why), "unsorted property keys are refused");
    std::vector<u8> cut(bytes.begin(), bytes.begin() + isize(bytes.size() / 2));
    OcAnimation junk;
    check(!fmt::parseOcAnim(cut.data(), cut.size(), junk, &why), "a truncated file is refused");
}

// ---- tree mode (G5 A2): a host-evaluated graph hands animations to the player and mixes them ----
static double outValue(PropPlayer& p, const std::string& path, bool* found = nullptr) {
    for (const PropOutput& o : p.outputs())
        if (p.trackPath(o.id) == path) { if (found) *found = true; return o.value.n[0]; }
    if (found) *found = false;
    return -9999.0;
}

static void testTreeMode() {
    // Weights and per-track weights.
    {
        PropPlayer p;
        p.deterministic = true;
        p.setClip("a", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "N:x", {K(0, F(0)), K(1, F(0))}), T(OcPropTrackType::Value, "N:y", {K(0, F(0)), K(1, F(0))})}));
        p.setClip("b", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "N:x", {K(0, F(10)), K(1, F(10))}), T(OcPropTrackType::Value, "N:y", {K(0, F(10)), K(1, F(10))})}));
        PropTreeStep sa = p.treeAdd("a", 0, 0.1, 1.0, {}, false, false);
        PropTreeStep sb = p.treeAdd("b", 0, 0.1, 0.25, {}, false, false);
        check(sa.ok && sb.ok && !sa.looped && std::fabs(sa.next - 0.1) < 1e-9, "treeAdd moves the playhead and reports the new one");
        p.treeMix(false);
        near(outValue(p, "N:x"), 2.5, 1e-9, "deterministic blend: init + w * (value - init) over both tracks (x)");
        near(outValue(p, "N:y"), 2.5, 1e-9, "deterministic blend (y)");
        check(seq(p, true) == "applied mixer", "a tree pass ends with the applied marker then mixer_applied");
        p.clearResults();

        // A filter: b reaches x only.
        p.treeAdd("a", 0.1, 0.1, 1.0, {}, false, false);
        p.treeAdd("b", 0.1, 0.1, 0.5, {1.0, 0.0}, false, false);
        p.treeMix(false);
        near(outValue(p, "N:x"), 5.0, 1e-9, "a track weight of 1 lets the instance reach its track");
        near(outValue(p, "N:y"), 0.0, 1e-9, "a track weight of 0 keeps the instance off its track");
        p.clearResults();

        // Normalised (the player's default): two instances of the same track average by weight.
        PropPlayer q;
        q.setClip("a", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "N:x", {K(0, F(0)), K(1, F(0))})}));
        q.setClip("b", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "N:x", {K(0, F(10)), K(1, F(10))})}));
        q.treeAdd("a", 0, 0.1, 3.0, {}, false, false);
        q.treeAdd("b", 0, 0.1, 1.0, {}, false, false);
        q.treeMix(false);
        near(outValue(q, "N:x"), 2.5, 1e-9, "without deterministic mode the weights are normalised (3:1 gives a quarter of b)");
        check(!p.treeAdd("missing", 0, 0.1, 1.0, {}, false, false).ok, "an unknown clip key is refused");
    }

    // Loop modes: wrap, bounce, direction flip.
    {
        PropPlayer p;
        p.setClip("loop", clipOf(1, OcPropLoop::Linear, {T(OcPropTrackType::Value, "N:x", {K(0, F(0)), K(1, F(1))})}));
        p.setClip("pp", clipOf(1, OcPropLoop::PingPong, {T(OcPropTrackType::Value, "N:x", {K(0, F(0)), K(1, F(1))})}));
        p.setClip("once", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Value, "N:x", {K(0, F(0)), K(1, F(1))})}));
        PropTreeStep a = p.treeAdd("loop", 0.9, 0.3, 1, {}, false, false);
        check(a.looped && !a.flipped, "a linear loop wraps");
        near(a.next, 0.2, 1e-9, "the playhead wraps to 0.2");
        PropTreeStep b = p.treeAdd("pp", 0.8, 0.5, 1, {}, false, false);
        check(b.looped && b.flipped, "a ping-pong bounce flips the direction");
        near(b.next, 0.7, 1e-9, "the bounce comes back to 0.7");
        PropTreeStep c = p.treeAdd("once", 0.9, 0.5, 1, {}, false, false);
        check(!c.looped && !c.flipped, "a one-shot clip does not wrap");
        near(c.next, 1.0, 1e-9, "it stops at its end");
        PropTreeStep d = p.treeAdd("loop", 0.25, -0.5, 1, {}, false, false);
        near(d.next, 0.75, 1e-9, "a backward step wraps below zero");
        PropTreeStep e = p.treeAdd("loop", 0.4, 7.0, 1, {}, true, false);
        near(e.next, 0.4, 1e-9, "a seeked instance sits at the given time and moves nothing");
        p.treeMix(false);
    }

    // Method keys: the segment of the pass, the start of a restarted playback, a seek.
    {
        PropPlayer p;
        p.setClip("m", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Method, "N", {methodKey(0.0, "begin"), methodKey(0.5, "mid")}, OcPropInterp::Nearest)}));
        const auto run = [&](double prev, double delta, bool seeked, bool inc) {
            p.clearResults();
            p.treeAdd("m", prev, delta, 1.0, {}, seeked, false, inc);
            p.treeMix(false);
            return seq(p);
        };
        check(run(0.4, 0.2, false, false) == "call:mid", "a method key inside the step fires");
        check(run(0.6, 0.2, false, false).empty(), "no key inside the step: no call");
        check(run(0.0, 0.1, false, false).empty(), "the key at the start is skipped on an ordinary step");
        check(run(0.0, 0.1, false, true) == "call:begin", "includeStart fires the key sitting at the start of a restarted playback");
        check(run(0.5, 0.0, true, false) == "call:mid", "a seek onto a key fires it");
    }

    // Root motion: deltas over the step, over a wrap, blended; accumulators; the track stays out of the outputs.
    {
        PropPlayer p;
        p.deterministic = true;
        p.setRootMotionPath("Root");
        p.setClip("walk", clipOf(1, OcPropLoop::Linear,
                                 {T(OcPropTrackType::Position3D, "Root", {K(0, V3(0, 0, 0)), K(1, V3(10, 0, 0))}),
                                  T(OcPropTrackType::Value, "N:x", {K(0, F(0)), K(1, F(1))})}));
        p.setClip("run", clipOf(1, OcPropLoop::Linear, {T(OcPropTrackType::Position3D, "Root", {K(0, V3(0, 0, 0)), K(1, V3(20, 0, 0))})}));
        p.treeAdd("walk", 0, 0.5, 1.0, {}, false, false);
        p.treeMix(false);
        const PropRootMotion& r = p.rootMotion();
        check(r.hasPosition && !r.hasRotation && !r.hasScale, "only the root-motion kinds that took part are reported");
        near(r.position[0], 5.0, 1e-9, "root motion: the movement of the step");
        near(r.positionAccum[0], 5.0, 1e-9, "the accumulator is the blended absolute value");
        bool found = true;
        outValue(p, "Root", &found);
        check(!found, "the root-motion track is not written to its node");
        near(outValue(p, "N:x"), 0.5, 1e-9, "other tracks still play");
        p.clearResults();

        p.treeAdd("walk", 0.8, 0.4, 1.0, {}, false, false);
        p.treeMix(false);
        near(p.rootMotion().position[0], 4.0, 1e-9, "a wrapped step adds the end of the clip and the start of the next cycle (2 + 2)");
        p.clearResults();

        p.treeAdd("walk", 0.0, 0.5, 0.5, {}, false, false);
        p.treeAdd("run", 0.0, 0.5, 0.5, {}, false, false);
        p.treeMix(false);
        near(p.rootMotion().position[0], 7.5, 1e-9, "blended root motion: half of 5 and half of 10");
        p.clearResults();

        p.treeAdd("walk", 0.5, 0.0, 1.0, {}, true, false);
        p.treeMix(false);
        near(p.rootMotion().position[0], 0.0, 1e-9, "a seek moves the root by nothing");
        near(p.rootMotion().positionAccum[0], 5.0, 1e-9, "but the accumulator follows the seek");

        // Rotation and scale tracks.
        PropPlayer q;
        q.setRootMotionPath("R");
        const double s = std::sqrt(0.5);
        q.setClip("spin", clipOf(1, OcPropLoop::None,
                                 {T(OcPropTrackType::Rotation3D, "R", {K(0, Qt(0, 0, 0, 1)), K(1, Qt(0, 0, s, s))}),
                                  T(OcPropTrackType::Scale3D, "R", {K(0, V3(1, 1, 1)), K(1, V3(3, 3, 3))})}));
        q.treeAdd("spin", 0, 0.5, 1.0, {}, false, false);
        q.treeMix(false);
        const PropRootMotion& m = q.rootMotion();
        check(m.hasRotation && m.hasScale && !m.hasPosition, "rotation and scale root tracks are reported");
        near(m.rotation[2], std::sin(3.14159265358979 / 8), 1e-6, "the rotation delta is 45 degrees about Z (z)");
        near(m.rotation[3], std::cos(3.14159265358979 / 8), 1e-6, "the rotation delta (w)");
        near(m.scale[0], 1.0, 1e-9, "the scale delta over half the clip");
        near(m.scaleAccum[0], 2.0, 1e-9, "the scale accumulator");
        q.treeAdd("spin", 0.5, 0.5, 1.0, {}, false, false);
        q.treeMix(false);
        near(q.rootMotion().rotation[2], std::sin(3.14159265358979 / 8), 1e-6, "the second half turns the same angle again");
    }

    // The ordinary player is untouched: the root-motion path on a player takes its track out of the outputs and reports the movement.
    {
        PropPlayer p;
        p.setRootMotionPath("Root");
        p.setClip("walk", clipOf(1, OcPropLoop::None, {T(OcPropTrackType::Position3D, "Root", {K(0, V3(0, 0, 0)), K(1, V3(10, 0, 0))})}));
        p.play("walk");
        p.advance(0.0);
        p.clearResults();
        p.advance(0.25);
        near(p.rootMotion().position[0], 2.5, 1e-9, "AnimationPlayer passes report root motion too");
        bool found = true;
        outValue(p, "Root", &found);
        check(!found, "and keep the track out of the outputs");
    }
}

int main() {
    testTreeMode();
    testInterpolation();
    testKeysInRange();
    testLoopsAndMethods();
    testPingPong();
    testEndAndSeek();
    testBlend();
    testDiscreteAndCapture();
    testQueueAndSignals();
    testTrackKinds();
    testFormat();
    if (g_failures == 0) AVER_INFO("=== all property animation tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
