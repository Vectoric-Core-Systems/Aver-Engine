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

    AVER_INFO("CPU linear-blend skinning, which is the contract the GPU pass must match");
    {
        // One bone, a pure translation of +10 cm in x. Every vertex must move by exactly that,
        // which is the simplest statement of "the translation row is read at all".
        std::vector<Mat4> skin(1, Mat4::identity());
        skin[0].m[3][0] = 10.0f;

        std::vector<f32> pos{1.0f, 2.0f, 3.0f};
        std::vector<f32> nrm{0.0f, 0.0f, 1.0f};
        std::vector<u16> j{0, 0, 0, 0};
        std::vector<f32> w{1.0f, 0.0f, 0.0f, 0.0f};
        std::vector<f32> op, on;

        anim::skinVertices(skin, pos, nrm, j, w, op, on);
        check(op.size() == 3 && on.size() == 3, "one vertex in, one vertex out");
        checkNear(op[0], 11.0f, 1e-5f, "the translation row reaches the position");
        checkNear(op[1], 2.0f, 1e-5f, "and only the row it belongs to");
        checkNear(on[2], 1.0f, 1e-5f, "a normal takes the rotation and NOT the translation");

        // Two bones at half weight each: the result is the midpoint, not either endpoint. This is
        // what separates linear BLEND skinning from picking the heaviest influence.
        skin.push_back(Mat4::identity());
        skin[1].m[3][0] = 30.0f;
        j = {0, 1, 0, 0};
        w = {0.5f, 0.5f, 0.0f, 0.0f};
        anim::skinVertices(skin, pos, nrm, j, w, op, on);
        checkNear(op[0], 21.0f, 1e-5f, "two influences blend, they do not compete");

        // An out-of-range bone index and a zero weight are the same case: no influence. Both are
        // skipped, and the surviving weights are used AS AUTHORED rather than renormalised -- so
        // half a vertex's weight going missing halves its displacement.
        j = {0, 99, 0, 0};
        w = {0.5f, 0.5f, 0.0f, 0.0f};
        anim::skinVertices(skin, pos, nrm, j, w, op, on);
        checkNear(op[0], 0.5f * 11.0f, 1e-5f, "an out-of-range bone contributes nothing at all");

        // No surviving influence leaves the vertex where it was, rather than at the origin.
        w = {0.0f, 0.0f, 0.0f, 0.0f};
        anim::skinVertices(skin, pos, nrm, j, w, op, on);
        checkNear(op[0], 1.0f, 1e-5f, "an unrigged vertex keeps its rest position");
        checkNear(op[2], 3.0f, 1e-5f, "in every component");
        checkNear(on[2], 1.0f, 1e-5f, "and its rest normal");

        // Skinning by a rest pose's own matrices is the identity: poseToSkinning's inverse-bind is
        // what makes that true, and it is the property every rig depends on at frame zero.
        //
        // chain() leaves inverseBind at whatever OcBone defaults to, which is fine for the sampling
        // checks above and useless here -- an inverse bind that is not the inverse of the rest
        // model makes the rest pose a transform rather than the identity. So this fixture spells
        // the inverses out: the chain is pure translation, so each is the negation of the bone's
        // accumulated offset.
        fmt::OcSkeleton sk = chain();
        const f32 restZ[3] = {0.0f, 40.0f, 100.0f};
        for (usize b = 0; b < sk.bones.size(); ++b) {
            f32* ib = sk.bones[b].inverseBind;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) ib[r * 4 + c] = (r == c) ? 1.0f : 0.0f;
            ib[3 * 4 + 2] = -restZ[b];
        }
        anim::Pose rest;
        anim::restPose(sk, rest);
        std::vector<Mat4> restSkin;
        anim::poseToSkinning(sk, rest, restSkin);
        std::vector<f32> rp{5.0f, -7.0f, 60.0f};
        std::vector<f32> rn{0.0f, 1.0f, 0.0f};
        std::vector<u16> rj{1, 0, 0, 0};
        std::vector<f32> rw{1.0f, 0.0f, 0.0f, 0.0f};
        anim::skinVertices(restSkin, rp, rn, rj, rw, op, on);
        checkNear(op[0], 5.0f, 1e-3f, "a rest pose skins a vertex to exactly where it started (x)");
        checkNear(op[1], -7.0f, 1e-3f, "(y)");
        checkNear(op[2], 60.0f, 1e-3f, "(z)");

        // Mis-sized inputs are rejected rather than read past their end.
        std::vector<f32> shortWeights{1.0f, 0.0f};
        anim::skinVertices(skin, pos, nrm, j, shortWeights, op, on);
        check(op.empty() && on.empty(), "a mis-sized weight stream is refused, not indexed into");
    }

    AVER_INFO("posed bounds: conservative in O(bones), checked against every posed vertex");
    {
        // A two-bone strip. Bone 0 holds the low half, bone 1 the high half, with a blended band in
        // between -- the same shape as a real limb, and the band is what a naive per-bone union gets
        // wrong, because a blended vertex sits in the gap between two boxes.
        const u32 kV = 41;
        std::vector<f32> pos(kV * 3), nrm(kV * 3, 0.0f);
        std::vector<u16> j(kV * 4, 0);
        std::vector<f32> wt(kV * 4, 0.0f);
        for (u32 v = 0; v < kV; ++v) {
            const f32 t = static_cast<f32>(v) / static_cast<f32>(kV - 1);
            pos[v*3+0] = (v % 2) ? 5.0f : -5.0f;
            pos[v*3+1] = 0.0f;
            pos[v*3+2] = t * 100.0f;
            nrm[v*3+2] = 1.0f;
            const f32 w1 = t < 0.35f ? 0.0f : (t > 0.65f ? 1.0f : (t - 0.35f) / 0.30f);
            j[v*4+0] = 0; wt[v*4+0] = 1.0f - w1;
            j[v*4+1] = 1; wt[v*4+1] = w1;
        }

        // minWeight ZERO, matching skinVertices' rule exactly: it counts every non-zero weight, so
        // a box built with a higher threshold would exclude a vertex the skinning still moves.
        std::vector<Vec3> bmin, bmax; std::vector<u8> used;
        anim::boneRestBounds(pos, j, wt, 2, 0.0f, bmin, bmax, used);
        check(used.size() == 2 && used[0] && used[1], "both bones own geometry");
        check(bmax[0].z < 100.0f && bmin[1].z > 0.0f,
              "and each bone's box covers ITS end of the strip, not the whole thing");

        Vec3 restMin{-5, 0, 0}, restMax{5, 0, 100};

        // Several poses, including one that throws bone 1 a long way out -- which is exactly the
        // case a rest-pose box gets wrong and the whole point of the exercise.
        const f32 angles[4] = {0.0f, 0.6f, 1.5f, 2.9f};
        const f32 lifts[4]  = {0.0f, 40.0f, 300.0f, -220.0f};
        for (u32 c = 0; c < 4; ++c) {
            std::vector<Mat4> skin(2, Mat4::identity());
            const f32 ca = std::cos(angles[c]), sa = std::sin(angles[c]);
            skin[1].m[1][1] = ca;  skin[1].m[1][2] = sa;
            skin[1].m[2][1] = -sa; skin[1].m[2][2] = ca;
            skin[1].m[3][2] = lifts[c];
            skin[0].m[3][0] = lifts[c] * 0.25f;

            Vec3 lo, hi;
            anim::posedBounds(bmin, bmax, used, skin.data(), 2, restMin, restMax, lo, hi);

            // The ground truth: skin every vertex and take its extent. O(vertices), which is what
            // posedBounds exists to avoid -- so this is the reference, not the implementation.
            std::vector<f32> op, on;
            anim::skinVertices(skin, pos, nrm, j, wt, op, on);
            Vec3 tlo{1e30f, 1e30f, 1e30f}, thi{-1e30f, -1e30f, -1e30f};
            for (u32 v = 0; v < kV; ++v) {
                tlo.x = std::fmin(tlo.x, op[v*3+0]); thi.x = std::fmax(thi.x, op[v*3+0]);
                tlo.y = std::fmin(tlo.y, op[v*3+1]); thi.y = std::fmax(thi.y, op[v*3+1]);
                tlo.z = std::fmin(tlo.z, op[v*3+2]); thi.z = std::fmax(thi.z, op[v*3+2]);
            }

            const bool contains = lo.x <= tlo.x + 1e-3f && lo.y <= tlo.y + 1e-3f && lo.z <= tlo.z + 1e-3f &&
                                  hi.x >= thi.x - 1e-3f && hi.y >= thi.y - 1e-3f && hi.z >= thi.z - 1e-3f;
            check(contains, "pose " + std::to_string(c) + ": the O(bones) box CONTAINS every posed vertex");
            if (!contains)
                AVER_ERROR("    box ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}) vs verts "
                           "({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})",
                           lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, tlo.x, tlo.y, tlo.z, thi.x, thi.y, thi.z);

            // Conservative is necessary and not sufficient: an infinite box contains everything and
            // culls nothing. The bound must also be TIGHT enough to be worth computing.
            const f32 vol = (hi.x-lo.x) * (hi.y-lo.y+1.0f) * (hi.z-lo.z);
            const f32 tvol = (thi.x-tlo.x) * (thi.y-tlo.y+1.0f) * (thi.z-tlo.z);
            check(tvol <= 1e-4f || vol <= tvol * 12.0f,
                  "pose " + std::to_string(c) + ": and is within 12x the true volume, so it still culls");
        }

        // A bone no vertex touches must not drag the bounds around. This is what `used` is for.
        std::vector<Vec3> bmin3, bmax3; std::vector<u8> used3;
        anim::boneRestBounds(pos, j, wt, 3, 0.0f, bmin3, bmax3, used3);
        check(used3.size() == 3 && !used3[2], "a bone with no geometry is marked unused");
        std::vector<Mat4> skin3(3, Mat4::identity());
        skin3[2].m[3][0] = 100000.0f;    // flung to the far side of the world
        Vec3 lo3, hi3;
        anim::posedBounds(bmin3, bmax3, used3, skin3.data(), 3, restMin, restMax, lo3, hi3);
        check(hi3.x < 1000.0f, "and contributes NOTHING even when its matrix is enormous");
    }

    AVER_INFO("=== notify crossings ===");
    {
        // A 2 s clip with markers at 0, 0.5, 1.25 and 2.0 -- both ends included on purpose, because
        // the ends are where an off-by-one interval shows up as an event that never fires or one
        // that fires twice per loop.
        fmt::OcAnimation c;
        c.duration = 2.0f;
        c.notifies = {{0.0f, "A"}, {0.5f, "B"}, {1.25f, "C"}, {2.0f, "D"}};

        std::vector<u32> hit;
        auto step = [&](f32 prev, f32 now, bool fwd, bool swept, bool incl) {
            anim::ClipStep st;
            st.prev = prev; st.now = now; st.forward = fwd;
            st.sweptWholeClip = swept; st.inclusiveStart = incl;
            hit.clear();
            anim::notifiesCrossed(c, st, hit);
            std::string names;
            for (const u32 i : hit) names += c.notifies[i].name;
            return names;
        };

        // ---- forward, no wrap
        check(step(0.4f, 0.6f, true, false, false) == "B", "a step over one marker fires exactly it");
        check(step(0.6f, 0.9f, true, false, false).empty(), "a step over none fires none");
        check(step(0.4f, 1.5f, true, false, false) == "BC", "a step over two fires both, in file order");

        // ---- THE DOUBLE-FIRE TEST. The half-open interval is the whole reason a marker landing
        // exactly on a frame boundary fires once rather than on both sides of it.
        check(step(0.0f, 0.5f, true, false, false) == "B", "a marker exactly at `now` fires");
        check(step(0.5f, 1.0f, true, false, false).empty(),
              "AND NOT AGAIN on the next step, which starts exactly on it");

        // ---- the first step of a clip includes its own start
        check(step(0.0f, 0.1f, true, false, false).empty(),
              "a marker at 0 is NOT re-fired by an ordinary step from 0");
        check(step(0.0f, 0.1f, true, false, true) == "A",
              "but IS fired by the first step of the clip (inclusiveStart)");

        // ---- looping
        check(step(1.9f, 0.1f, true, false, false) == "AD",
              "a wrap fires the tail of the clip and then its head, INCLUDING the marker at 0");
        check(step(1.9f, 0.1f, true, false, false).find("D") != std::string::npos,
              "the end-of-clip marker at exactly duration is reachable at all");
        check(step(1.3f, 1.9f, true, false, false).empty(), "and a step short of the end fires nothing");

        // ---- reverse
        check(step(0.6f, 0.4f, false, false, false) == "B", "running backwards fires the marker passed");
        check(step(1.5f, 0.4f, false, false, false) == "BC", "and every marker in the span");
        check(step(0.1f, 1.9f, false, false, false) == "AD",
              "a backward wrap fires off the front and back onto the end");

        // ---- a step that swallows the clip
        check(step(0.4f, 0.4f, true, true, false) == "ABCD", "a step longer than the clip fires everything");
        check(step(0.4f, 0.4f, true, true, false).size() == 4,
              "ONCE EACH, not once per lap -- a stalled frame must not deliver a burst");

        // ---- a marker outside the clip is clamped rather than lost
        fmt::OcAnimation past;
        past.duration = 1.0f;
        past.notifies = {{5.0f, "late"}, {-3.0f, "early"}};
        std::vector<u32> h2;
        anim::ClipStep st;
        st.prev = 0.9f; st.now = 0.05f; st.forward = true;
        anim::notifiesCrossed(past, st, h2);
        check(h2.size() == 2, "markers past both ends of a shortened clip still arrive, clamped");

        // ---- a clip with no notifies costs nothing and appends nothing
        fmt::OcAnimation bare;
        bare.duration = 1.0f;
        std::vector<u32> h3{99u};
        anim::notifiesCrossed(bare, st, h3);
        check(h3.size() == 1 && h3[0] == 99u, "a notify-free clip leaves the output vector alone");
    }

    AVER_INFO(g_failures ? "AnimTest: {} FAILURES" : "AnimTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
