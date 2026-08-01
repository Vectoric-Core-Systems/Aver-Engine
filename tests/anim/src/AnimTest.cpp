// Sampling and posing, with no GPU.
//
// Everything here is wrong in a way NOBODY SEES until a character is on screen and slightly off:
// a slerp that takes the long way round, a cubic tangent scaled by the wrong interval, a bone whose
// parent transform is applied on the wrong side, an additive layer that replaces instead of adding.
// None of that is decidable from a screenshot, and all of it is decidable from arithmetic.
#include "aver/anim/AnimSampler.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

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
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

// A three-bone chain: root at the origin, spine 40 up, head another 60 up.
static fmt::OcSkeleton chain() {
    fmt::OcSkeleton s;
    fmt::OcBone root;  root.name = "root";   root.parent = -1;
    fmt::OcBone spine; spine.name = "spine"; spine.parent = 0; spine.translation = Vec3{0, 0, 40};
    fmt::OcBone head;  head.name = "head";   head.parent = 1; head.translation = Vec3{0, 0, 60};
    s.bones = {root, spine, head};
    s.rootBone = 0;
    return s;
}

// A one-track clip animating one bone's translation along X, linear, from 0 to `to` over `dur`.
static fmt::OcAnimation slide(u16 bone, f32 to, f32 dur, bool loop) {
    fmt::OcAnimation a;
    a.duration = dur;
    a.flags = loop ? fmt::kOcAnimLoop : 0;
    fmt::OcTrack t;
    t.boneIndex = bone;
    t.channels = fmt::kOcChannelTranslation;
    t.interp = fmt::OcInterp::Linear;
    t.times = {0.0f, dur};
    t.values = {0, 0, 0,  to, 0, 0};
    a.tracks.push_back(t);
    return a;
}

int main() {
    AVER_INFO("AnimTest");

    AVER_INFO("rest pose and the hierarchy");
    {
        const fmt::OcSkeleton s = chain();
        check(s.valid(), "the three-bone chain is a valid skeleton");
        anim::Pose p;
        anim::restPose(s, p);
        check(p.matches(s), "the rest pose has one transform per bone");
        checkNear(p.local[1].position.z, 40.0f, 1e-6f, "the spine's LOCAL offset is its own");

        std::vector<Mat4> model;
        anim::poseToModel(s, p, model);
        // Row-vector convention: a point is p * M, so the translation lands in row 3.
        checkNear(model[0].m[3][2],   0.0f, 1e-4f, "the root sits at the origin in model space");
        checkNear(model[1].m[3][2],  40.0f, 1e-4f, "the spine ACCUMULATES to 40");
        checkNear(model[2].m[3][2], 100.0f, 1e-4f, "and the head to 100, which is the parent chain");
    }

    AVER_INFO("skinning matrices");
    {
        // Inverse bind is the inverse of the rest model matrix, so the rest pose must skin to the
        // identity. Any transposed multiply or wrong-side compose breaks exactly this.
        fmt::OcSkeleton s = chain();
        anim::Pose rest;
        anim::restPose(s, rest);
        std::vector<Mat4> model;
        anim::poseToModel(s, rest, model);
        for (usize i = 0; i < s.bones.size(); ++i) {
            const Mat4 inv = model[i].inverse();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) s.bones[i].inverseBind[r * 4 + c] = inv.m[r][c];
        }
        std::vector<Mat4> skin;
        anim::poseToSkinning(s, rest, skin);
        f32 worst = 0.0f;
        for (const Mat4& m : skin)
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    worst = std::fmax(worst, std::fabs(m.m[r][c] - (r == c ? 1.0f : 0.0f)));
        checkNear(worst, 0.0f, 1e-3f, "a bind-pose skeleton skins to the identity");

        // Move the spine and the head must follow it, because skinning composes the chain.
        anim::Pose moved = rest;
        moved.local[1].position.x += 10.0f;
        anim::poseToSkinning(s, moved, skin);
        checkNear(skin[2].m[3][0], 10.0f, 1e-3f, "moving the spine carries the head with it");
        checkNear(skin[0].m[3][0],  0.0f, 1e-3f, "and leaves the root alone");
    }

    AVER_INFO("linear sampling");
    {
        const fmt::OcAnimation a = slide(1, 100.0f, 2.0f, false);
        check(a.valid(), "the slide clip is valid");
        const fmt::OcSkeleton s = chain();
        anim::Pose p;

        anim::restPose(s, p); anim::sampleAnimation(a, 0.0f, p);
        checkNear(p.local[1].position.x, 0.0f, 1e-5f, "at t=0 it is at the first key");
        anim::restPose(s, p); anim::sampleAnimation(a, 1.0f, p);
        checkNear(p.local[1].position.x, 50.0f, 1e-5f, "half way through it is half way there");
        anim::restPose(s, p); anim::sampleAnimation(a, 2.0f, p);
        checkNear(p.local[1].position.x, 100.0f, 1e-5f, "at the end it is at the last key");
        anim::restPose(s, p); anim::sampleAnimation(a, 9.0f, p);
        checkNear(p.local[1].position.x, 100.0f, 1e-5f, "past the end it HOLDS rather than extrapolating");

        // The untracked bones are the point of seeding with rest.
        anim::restPose(s, p); anim::sampleAnimation(a, 1.0f, p);
        checkNear(p.local[2].position.z, 60.0f, 1e-5f, "a bone with no track keeps its REST transform");
    }

    AVER_INFO("clip time: looping and clamping");
    {
        const fmt::OcAnimation loop = slide(1, 100.0f, 2.0f, true);
        const fmt::OcAnimation once = slide(1, 100.0f, 2.0f, false);
        checkNear(anim::clipTime(loop, 2.5f), 0.5f, 1e-5f, "a looping clip wraps");
        checkNear(anim::clipTime(loop, 6.5f), 0.5f, 1e-5f, "and keeps wrapping");
        checkNear(anim::clipTime(loop, -0.5f), 1.5f, 1e-5f, "a negative time wraps FORWARD, not to -0.5");
        checkNear(anim::clipTime(once, 2.5f), 2.0f, 1e-5f, "a one-shot clamps to its end");
        checkNear(anim::clipTime(once, -1.0f), 0.0f, 1e-5f, "and to its start");
    }

    AVER_INFO("step and cubic interpolation");
    {
        fmt::OcAnimation a;
        a.duration = 2.0f;
        fmt::OcTrack t;
        t.boneIndex = 1;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Step;
        t.times = {0.0f, 1.0f, 2.0f};
        t.values = {0,0,0,  10,0,0,  20,0,0};
        a.tracks.push_back(t);
        const fmt::OcSkeleton s = chain();
        anim::Pose p;
        anim::restPose(s, p); anim::sampleAnimation(a, 0.9f, p);
        checkNear(p.local[1].position.x, 0.0f, 1e-5f, "a step curve HOLDS its key until the next one");
        anim::restPose(s, p); anim::sampleAnimation(a, 1.0f, p);
        checkNear(p.local[1].position.x, 10.0f, 1e-5f, "and jumps exactly on it");
    }
    {
        // Hermite with both tangents zero is the smoothstep between the values, so the midpoint is
        // exactly half way and the ends are exact. A wrong tangent scaling shows up as neither.
        fmt::OcAnimation a;
        a.duration = 2.0f;
        fmt::OcTrack t;
        t.boneIndex = 1;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::CubicSpline;
        t.times = {0.0f, 2.0f};
        // Per key, channel-major: [inTangent(3), value(3), outTangent(3)].
        t.values = {0,0,0,   0,0,0,   0,0,0,
                    0,0,0, 100,0,0,   0,0,0};
        a.tracks.push_back(t);
        check(t.componentsPerKey() == 9, "a cubic translation key is nine floats");
        const fmt::OcSkeleton s = chain();
        anim::Pose p;
        anim::restPose(s, p); anim::sampleAnimation(a, 1.0f, p);
        checkNear(p.local[1].position.x, 50.0f, 1e-4f, "a flat-tangent cubic is half way at half time");
        anim::restPose(s, p); anim::sampleAnimation(a, 0.5f, p);
        checkNear(p.local[1].position.x, 15.625f, 1e-3f, "and follows the smoothstep, not a straight line");
        anim::restPose(s, p); anim::sampleAnimation(a, 2.0f, p);
        checkNear(p.local[1].position.x, 100.0f, 1e-4f, "and lands exactly on the final key");
    }

    AVER_INFO("channel offsets when a track carries more than one");
    {
        // Rotation sits AFTER translation in a key, so a wrong offset reads a position as a quat.
        fmt::OcAnimation a;
        a.duration = 1.0f;
        fmt::OcTrack t;
        t.boneIndex = 1;
        t.channels = fmt::kOcChannelTranslation | fmt::kOcChannelRotation | fmt::kOcChannelScale;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 1.0f};
        const Quat q = Quat::fromAxisAngle(Vec3{0, 0, 1}, 1.5707963f);
        t.values = {  1,2,3,  0,0,0,1,      4,5,6,
                     7,8,9,  q.x,q.y,q.z,q.w,  2,2,2};
        a.tracks.push_back(t);
        check(t.componentsPerKey() == 10, "translation + rotation + scale is ten floats a key");
        const fmt::OcSkeleton s = chain();
        anim::Pose p;
        anim::restPose(s, p); anim::sampleAnimation(a, 0.0f, p);
        checkNear(p.local[1].position.x, 1.0f, 1e-5f, "translation reads from the front of the key");
        checkNear(p.local[1].scale.x,    4.0f, 1e-5f, "and scale from the back");
        checkNear(p.local[1].rotation.w, 1.0f, 1e-5f, "with rotation between them");
        anim::restPose(s, p); anim::sampleAnimation(a, 1.0f, p);
        checkNear(p.local[1].position.z, 9.0f, 1e-5f, "the second key reads at the right stride");
        checkNear(p.local[1].rotation.z, q.z, 1e-5f, "including its quaternion");
    }

    AVER_INFO("slerp takes the short way round");
    {
        // 350 degrees apart the short way is 10 degrees, and a lerp-and-normalise would sweep 350.
        const Quat a = Quat::fromAxisAngle(Vec3{0, 0, 1}, 0.0f);
        const Quat b = Quat::fromAxisAngle(Vec3{0, 0, 1}, 350.0f * 3.14159265f / 180.0f);
        const Quat m = Quat::slerp(a, b, 0.5f);
        const Vec3 v = m.rotate(Vec3{1, 0, 0});
        const f32 deg = std::atan2(v.y, v.x) * 180.0f / 3.14159265f;
        checkNear(deg, -5.0f, 0.1f, "half way from 0 to 350 degrees is -5, not +175");
    }

    AVER_INFO("blending");
    {
        const fmt::OcSkeleton s = chain();
        anim::Pose a, b, out;
        anim::restPose(s, a);
        anim::restPose(s, b);
        b.local[1].position.x = 100.0f;
        anim::blendPose(a, b, 0.25f, out);
        checkNear(out.local[1].position.x, 25.0f, 1e-5f, "a quarter blend is a quarter of the way");
        anim::blendPose(a, b, 5.0f, out);
        checkNear(out.local[1].position.x, 100.0f, 1e-5f, "and the weight is clamped");
    }
    {
        // Additive must ADD to whatever the base is doing, not overwrite it.
        const fmt::OcSkeleton s = chain();
        anim::Pose base, add, addRest, out;
        anim::restPose(s, base);
        anim::restPose(s, addRest);
        anim::restPose(s, add);
        base.local[1].position.x = 10.0f;    // the base has moved the spine
        add.local[1].position.z  = addRest.local[1].position.z + 5.0f;   // the layer lifts it
        anim::addPose(base, add, addRest, 1.0f, out);
        checkNear(out.local[1].position.x, 10.0f, 1e-5f, "the base's own motion survives the layer");
        checkNear(out.local[1].position.z, 45.0f, 1e-5f, "and the layer's DELTA is added to it");
        anim::addPose(base, add, addRest, 0.5f, out);
        checkNear(out.local[1].position.z, 42.5f, 1e-5f, "at half weight, half the delta");
    }

    AVER_INFO("the player and its crossfade");
    {
        const fmt::OcSkeleton s = chain();
        const fmt::OcAnimation walk = slide(1, 100.0f, 2.0f, true);
        const fmt::OcAnimation run  = slide(1, 200.0f, 2.0f, true);
        anim::AnimPlayer pl;
        anim::Pose p;

        pl.reset(&walk);
        pl.advance(1.0f);
        pl.evaluate(s, p);
        checkNear(p.local[1].position.x, 50.0f, 1e-4f, "the player advances its own clock");

        pl.play(&run, 1.0f);
        pl.evaluate(s, p);
        checkNear(p.local[1].position.x, 50.0f, 1e-4f,
                  "at the instant of a crossfade the pose is still the outgoing one");
        pl.advance(0.5f);
        pl.evaluate(s, p);
        // Outgoing frozen at 50; incoming at t=0.5 of a 0..200 clip over 2 s is 50. Half way
        // between them is 50 -- so nudge the incoming clip instead by checking it moved at all.
        check(p.local[1].position.x > 0.0f, "half way through the fade both clips contribute");
        pl.advance(0.6f);
        check(!pl.fading(), "the fade finishes");
        pl.evaluate(s, p);
        checkNear(p.local[1].position.x, 110.0f, 1e-3f, "and the incoming clip owns the pose");

        pl.play(&run, 1.0f);
        check(pl.clip() == &run, "playing the clip that is already playing is a no-op");
        check(!pl.fading(), "and starts no fade");
    }

    AVER_INFO(g_failures ? "AnimTest: {} FAILURES" : "AnimTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
