// LevelSequenceTest: the level sequence samplers and the player's clock (LevelSequence.hpp).
// Pure sampling needs nothing; the player's evaluate() needs a scene::World and nothing else.
#include "aver/game/LevelSequence.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE
#include "aver/scene/World.hpp"
#endif

#include <cmath>
#include <string>

using namespace aver;
using aver::fmt::OcSeqInterp;
using aver::fmt::OcSeqKey;
using aver::fmt::OcSeqTrack;
using aver::fmt::OcSeqTrackKind;
using aver::fmt::OcSequence;

static int g_checks   = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f64 a, f64 b, f64 eps = 1e-3) { return std::fabs(a - b) <= eps; }

static OcSeqKey key(f64 t, f64 x, f64 y, f64 z, OcSeqInterp in = OcSeqInterp::Smooth) {
    OcSeqKey k;
    k.t = t; k.v[0] = x; k.v[1] = y; k.v[2] = z; k.interp = in;
    return k;
}

static void testWrap() {
    OcSequence s;
    s.length = 4.0;
    s.loop = true;
    check(approx(game::seqWrapTime(s, 5.0), 1.0), "loop wraps past the end");
    check(approx(game::seqWrapTime(s, -1.0), 3.0), "loop wraps negative time");
    s.length = 0.0;
    check(approx(game::seqWrapTime(s, 3.0), 0.0), "loop with no length is 0");
    s.length = 4.0;
    s.loop = false;
    check(approx(game::seqWrapTime(s, 5.0), 4.0), "non-loop clamps at the end");
    check(approx(game::seqWrapTime(s, -2.0), 0.0), "non-loop clamps at the start");
}

static void testTransform() {
    OcSeqTrack tr;
    tr.kind = OcSeqTrackKind::Transform;
    Transform xf;
    check(!game::sampleSeqTransform(tr, 0.0, xf), "no keys -> false");

    tr.keys.push_back(key(1.0, 0, 0, 0, OcSeqInterp::Linear));
    tr.keys.push_back(key(3.0, 100, 200, 0, OcSeqInterp::Linear));
    check(game::sampleSeqTransform(tr, 0.0, xf) && approx(xf.position.x, 0), "holds before the first key");
    check(game::sampleSeqTransform(tr, 2.0, xf) && approx(xf.position.x, 50) && approx(xf.position.y, 100),
          "linear midpoint");
    check(game::sampleSeqTransform(tr, 9.0, xf) && approx(xf.position.x, 100), "holds after the last key");

    tr.keys[0].interp = OcSeqInterp::Step;
    check(game::sampleSeqTransform(tr, 2.0, xf) && approx(xf.position.x, 0), "step holds the earlier key");

    // Smooth passes through its keys and is straight on an evenly spaced straight run.
    OcSeqTrack sm;
    sm.keys = {key(0, 0, 0, 0), key(1, 10, 0, 0), key(2, 20, 0, 0), key(3, 30, 0, 0)};
    for (int i = 0; i < 4; ++i)
        check(game::sampleSeqTransform(sm, i, xf) && approx(xf.position.x, 10.0 * i),
              "smooth hits key " + std::to_string(i));
    check(game::sampleSeqTransform(sm, 1.5, xf) && approx(xf.position.x, 15.0, 0.01),
          "smooth on a straight run is straight");

    // Rotation: yaw 350 -> 10 degrees takes the 20 degree arc, not the 340 one.
    OcSeqTrack rot;
    OcSeqKey a = key(0, 0, 0, 0, OcSeqInterp::Linear), b = key(1, 0, 0, 0, OcSeqInterp::Linear);
    a.v[3] = 350.0; b.v[3] = 10.0;
    rot.keys = {a, b};
    check(game::sampleSeqTransform(rot, 0.5, xf), "rotation samples");
    const Vec3 fwd = xf.rotation.rotate(Vec3{1, 0, 0});
    check(approx(fwd.x, 1.0, 1e-3) && approx(fwd.y, 0.0, 1e-3), "yaw 350->10 midpoint faces +X (short arc)");

    // Key round trip.
    Transform src;
    src.position = Vec3{1, 2, 3};
    src.scale = Vec3{2, 3, 4};
    OcSeqKey k;
    game::seqKeyFromTransform(src, k);
    OcSeqTrack one;
    one.keys = {k};
    check(game::sampleSeqTransform(one, 0.0, xf) && approx(xf.position.z, 3) && approx(xf.scale.y, 3),
          "transform key round trip");
}

static void testCamera() {
    OcSeqTrack tr;
    tr.kind = OcSeqTrackKind::Camera;
    game::SeqCameraPose p;
    check(!game::sampleSeqCamera(tr, 0.0, p), "no camera keys -> false");
    OcSeqKey a = key(0, 0, 0, 100, OcSeqInterp::Linear), b = key(2, 200, 0, 100, OcSeqInterp::Linear);
    a.v[3] = 170.0; b.v[3] = -170.0;   // 20 degrees the short way, through 180
    a.v[4] = 0.0;   b.v[4] = 30.0;
    tr.keys = {a, b};
    check(game::sampleSeqCamera(tr, 1.0, p), "camera samples");
    check(approx(p.position.x, 100), "camera position lerps");
    check(std::fabs(p.yaw) > 3.1, "camera yaw takes the short way through 180 degrees");
    check(approx(p.pitch, 15.0 * 3.14159265 / 180.0, 1e-3), "camera pitch is radians");

    game::SeqCameraPose back;
    OcSeqKey k;
    game::seqKeyFromCamera(p, k);
    OcSeqTrack one;
    one.kind = OcSeqTrackKind::Camera;
    one.keys = {k};
    check(game::sampleSeqCamera(one, 0.0, back) && approx(back.pitch, p.pitch, 1e-4), "camera key round trip");
}

static OcSeqKey camKey(f64 t, f64 x, f64 y, f64 z, f64 yawDeg, f64 pitchDeg) {
    OcSeqKey k = key(t, x, y, z);
    k.v[3] = yawDeg; k.v[4] = pitchDeg;
    return k;
}

// Smooth camera: the spline passes through the keys, is held outside them, and the orientation never
// flips (yaw crossing 180 degrees, steps between samples stay small).
static void testCameraSpline() {
    OcSeqTrack tr;
    tr.kind = OcSeqTrackKind::Camera;
    tr.keys = {camKey(0, 0, 0, 100, 0, 0), camKey(2, 500, 0, 100, 90, 10), camKey(4, 500, 500, 200, 170, -5),
               camKey(6, 0, 500, 100, -170, 0)};
    game::SeqCameraPose p;
    for (const OcSeqKey& k : tr.keys) {
        check(game::sampleSeqCamera(tr, k.t, p) && approx(p.position.x, k.v[0], 0.01) && approx(p.position.y, k.v[1], 0.01) &&
              approx(p.position.z, k.v[2], 0.01), "the camera passes through its keys");
    }
    check(game::sampleSeqCamera(tr, -3.0, p) && approx(p.position.x, 0, 0.01), "held before the first key");
    check(game::sampleSeqCamera(tr, 99.0, p) && approx(p.position.y, 500, 0.01), "held after the last key");

    game::SeqCameraPose prev;
    game::sampleSeqCamera(tr, 0.0, prev);
    f64 maxStep = 0, maxYawStep = 0;
    for (int i = 1; i <= 600; ++i) {
        game::sampleSeqCamera(tr, i * 0.01, p);
        maxStep = std::fmax(maxStep, std::sqrt(std::pow(p.position.x - prev.position.x, 2) + std::pow(p.position.y - prev.position.y, 2) +
                                                 std::pow(p.position.z - prev.position.z, 2)));
        f64 dy = std::fmod(p.yaw - prev.yaw + 3.14159265358979, 6.28318530717959);
        if (dy < 0) dy += 6.28318530717959;
        maxYawStep = std::fmax(maxYawStep, std::fabs(dy - 3.14159265358979));
        prev = p;
    }
    check(maxStep < 12.0, "the camera path has no jumps");
    check(maxYawStep < 0.05, "the camera yaw has no flips across +-180 degrees");
}

static void testMaterial() {
    OcSeqTrack tr;
    tr.kind = OcSeqTrackKind::Material;
    f32 rgb[3];
    check(!game::sampleSeqMaterial(tr, 0.0, rgb), "no material keys -> false");
    OcSeqKey a, b;
    a.t = 0; a.v[0] = 1; a.v[1] = 0.5; a.v[2] = 0; a.v[3] = 0; a.interp = OcSeqInterp::Linear;
    b.t = 1; b.v[0] = 1; b.v[1] = 0.5; b.v[2] = 0; b.v[3] = 4; b.interp = OcSeqInterp::Linear;
    tr.keys = {a, b};
    check(game::sampleSeqMaterial(tr, 0.5, rgb) && approx(rgb[0], 2.0) && approx(rgb[1], 1.0) && approx(rgb[2], 0.0),
          "material is rgb * intensity");
    tr.kind = OcSeqTrackKind::Camera;
    check(!game::sampleSeqMaterial(tr, 0.0, rgb), "wrong kind -> false");
}

#if AVER_MODULE_SCENE
static void testPlayer() {
    OcSequence s;
    s.length = 2.0;
    s.loop = false;
    OcSeqTrack tr;
    tr.kind = OcSeqTrackKind::Transform;
    tr.target = 0;
    tr.keys = {key(0, 0, 0, 0, OcSeqInterp::Linear), key(2, 200, 0, 0, OcSeqInterp::Linear)};
    s.tracks.push_back(tr);
    OcSeqTrack mat;
    mat.kind = OcSeqTrackKind::Material;
    mat.target = 0;
    OcSeqKey m;
    m.v[0] = 1; m.v[1] = 1; m.v[2] = 1; m.v[3] = 3;
    mat.keys = {m};
    s.tracks.push_back(mat);

    scene::World& w = scene::World::instance();
    const scene::Entity e = w.create("seq-actor");

    game::SequencePlayer p;
    p.setSequence(s);
    p.bind({e});
    check(p.boundEntities().size() == 1 && p.transformEntities().size() == 1, "one entity bound once");
    p.advance(1.0);
    check(approx(p.time(), 0.0), "paused: no time passes");
    p.play();
    p.advance(1.0);
    check(approx(p.time(), 1.0), "playing advances");
    p.evaluate(w);
    check(approx(w.localTransform(e).position.x, 100.0), "evaluate writes the actor's transform");
    f32 rgb[3];
    check(p.emissiveScale(e, rgb) && approx(rgb[0], 3.0), "evaluate caches the emissive multiplier");
    check(!p.emissiveScale(scene::kInvalidEntity, rgb), "no multiplier for an unbound entity");
    p.advance(5.0);
    check(approx(p.time(), 2.0) && !p.playing(), "non-loop clamps at the end and stops");

    s.loop = true;
    p.setSequence(s);
    p.setTime(0);
    p.play();
    p.advance(3.0);
    check(approx(p.time(), 1.0) && p.playing(), "loop wraps and keeps playing");

    w.destroy(e);
    w.flush();
}
#endif

int main() {
    AVER_INFO("LevelSequenceTest");
    testWrap();
    testTransform();
    testCamera();
    testCameraSpline();
    testMaterial();
#if AVER_MODULE_SCENE
    testPlayer();
#endif
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
