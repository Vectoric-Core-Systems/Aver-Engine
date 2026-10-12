// RetargetTest -- carrying poses and clips between skeletons with different bones, proportions, rests and
// bone frames.
//
// The standard is geometry again: bone DIRECTIONS in model space, foot positions, scaled translations. Every
// skeleton here is built from MODEL-space rest transforms (bone frames independent of the geometry), so a
// retarget that confused one skeleton's local axes with the other's lands in the wrong place.
#include "aver/anim/AnimSampler.hpp"
#include "RetargetFixture.hpp"

#include "aver/anim/Pose.hpp"
#include "aver/anim/Retarget.hpp"
#include "aver/anim/RootMotion.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
static f32 gap(const Vec3& a, const Vec3& b) { return (a - b).size(); }
static f32 angleDeg(const Quat& a, const Quat& b) {
    const Quat r = a * Quat{-b.x, -b.y, -b.z, b.w};
    return 2.0f * std::atan2(std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z), std::fabs(r.w)) * kRadToDeg;
}

using namespace rtfix;

static u32 g_seed = 12345u;
static f32 rnd() { g_seed = g_seed * 1664525u + 1013904223u; return static_cast<f32>((g_seed >> 8) & 0xFFFF) / 65535.0f; }
static Quat randomRot(f32 maxDeg) {
    const Vec3 axis = Vec3{rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f}.getSafeNormal();
    return Quat::fromAxisAngle(axis.size() > 0.5f ? axis : Vec3{0, 0, 1}, radians(maxDeg) * rnd());
}


static anim::RetargetAsset autoAsset(const Rig& a, const Rig& b, const std::string& srcPath = "src.ocskel", const std::string& dstPath = "dst.ocskel") {
    anim::AutoMapResult m = anim::autoMapChains(a.skel, b.skel);
    m.asset.source = srcPath;
    m.asset.target = dstPath;
    return m.asset;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("retargeting");

    AVER_INFO("=== the asset: round trip and validation ===");
    {
        const Rig a = makeRig(Params{}), b = makeRig(Params{});
        anim::RetargetAsset as = autoAsset(a, b);
        as.name = "Self";
        as.chains[0].translation = anim::RtgTranslation::Scaled;
        as.chains[1].scale = anim::RtgScale{false, 1.25f};
        const std::string text = anim::writeRetarget(as);
        anim::RetargetAsset back;
        std::string why;
        check(anim::parseRetarget(text, back, &why), "it parses: " + why);
        check(anim::writeRetarget(back) == text, "and writes back byte-identical");
        check(as.valid(a.skel, b.skel, &why), "an auto-mapped asset validates against its own skeletons: " + why);
        check(!anim::parseRetarget("{\"format\":\"ocretarget\",\"version\":3}", back, &why), "a newer version is refused");
        check(!anim::parseRetarget("{\"format\":\"ocikrig\",\"version\":1}", back, &why), "another asset is refused");

        anim::RetargetAsset bad = as;
        bad.chains[0].dstEnd = "no_such_bone";
        check(!bad.valid(a.skel, b.skel, &why) && !why.empty(), "an unknown bone is refused: " + why);
        bad = as;
        bad.chains.push_back(bad.chains[0]);
        bad.chains.back().name = "Again";
        check(!bad.valid(a.skel, b.skel, &why), "a bone in two chains is refused: " + why);
        bad = as;
        bad.chains[0].rotation = anim::RtgRotation::OneToOne;
        for (auto& c : bad.chains) if (c.name == "Spine") c.rotation = anim::RtgRotation::OneToOne;
        const Rig four = makeRig(Params{Naming::Ue, 1.0f, false, 4});
        anim::RetargetAsset oto = autoAsset(a, four);
        for (auto& c : oto.chains) if (c.name == "Spine") c.rotation = anim::RtgRotation::OneToOne;
        check(!oto.valid(a.skel, four.skel, &why) && why.find("oneToOne") != std::string::npos, "oneToOne with 3 against 4 bones is a validation error: " + why);
        bad = as;
        bad.chains[0].srcEnd = bad.chains[0].srcStart + "x";
        check(!bad.valid(a.skel, b.skel, &why), "a name that does not exist on the source is refused");
    }

    AVER_INFO("=== identity: the same skeleton reproduces every pose ===");
    {
        const Rig a = makeRig(Params{Naming::Ue, 1.0f, false, 3, true});
        anim::RetargetAsset as = autoAsset(a, a);
        anim::Retargeter rt;
        std::vector<anim::RetargetIssue> issues;
        check(rt.bind(as, a.skel, a.skel, &issues), "it binds");
        check(std::fabs(rt.scaleH() - 1.0f) < 1e-4f && std::fabs(rt.scaleV() - 1.0f) < 1e-4f, "the auto scale is 1");
        f32 worstRot = 0.0f, worstPos = 0.0f;
        for (int t = 0; t < 20; ++t) {
            anim::Pose p;
            anim::restPose(a.skel, p);
            for (usize i = 0; i < p.local.size(); ++i) p.local[i].rotation = (p.local[i].rotation * randomRot(35.0f)).normalized();
            p.local[static_cast<usize>(a.pelvis)].position = p.local[static_cast<usize>(a.pelvis)].position + Vec3{rnd() * 20 - 10, rnd() * 20 - 10, rnd() * 10 - 5};
            p.local[static_cast<usize>(a.root)].position = p.local[static_cast<usize>(a.root)].position + Vec3{rnd() * 50, rnd() * 50, 0};
            anim::Pose out;
            rt.retarget(p, out);
            for (const u32 b : rt.mappedDstBones()) {
                worstRot = std::fmax(worstRot, angleDeg(p.local[b].rotation, out.local[b].rotation));
                worstPos = std::fmax(worstPos, gap(p.local[b].position, out.local[b].position));
            }
        }
        check(worstRot < 0.01f, "20 random poses: every mapped bone within 0.01 degrees, worst " + f2s(worstRot));
        check(worstPos < 0.01f, "and every position within 0.01 cm, worst " + f2s(worstPos));
    }

    AVER_INFO("=== a 1.3x skeleton: translation scales, rotations are equal ===");
    {
        const Rig a = makeRig(Params{Naming::Ue, 1.0f, false, 3, true});
        const Rig b = makeRig(Params{Naming::Ue, 1.3f, false, 3, false});     // different bone frames too
        anim::Retargeter rt;
        check(rt.bind(autoAsset(a, b), a.skel, b.skel), "it binds");
        check(std::fabs(rt.scaleH() - 1.3f) < 1e-3f, "the auto scale is the pelvis height ratio, " + f2s(rt.scaleH()));
        anim::Pose p;
        anim::restPose(a.skel, p);
        rotateBone(a.skel, p, a.pelvis, randomRot(20.0f));
        for (int k : a.spine) rotateBone(a.skel, p, k, randomRot(30.0f));
        rotateBone(a.skel, p, a.upper[0], Quat::fromAxisAngle(Vec3{1, 0, 0}, -1.2f));
        rotateBone(a.skel, p, a.lower[0], Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.5f));
        p.local[static_cast<usize>(a.pelvis)].position = p.local[static_cast<usize>(a.pelvis)].position + Vec3{10, -6, -4};
        anim::Pose out;
        rt.retarget(p, out);
        const Vec3 sp = posOf(a.skel, p, a.pelvis), dp = posOf(b.skel, out, b.pelvis);
        check(gap(dp, sp * 1.3f) < 0.2f, "the pelvis lands at 1.3x the source, off by " + f2s(gap(dp, sp * 1.3f)) + " cm");
        // Model-space rotation DELTAS match for every mapped chain bone (frames differ, so compare deltas).
        anim::Pose ra, rb;
        anim::restPose(a.skel, ra);
        anim::restPose(b.skel, rb);
        f32 worst = 0.0f;
        const std::pair<int, int> pairs[] = {{a.pelvis, b.pelvis}, {a.upper[0], b.upper[0]}, {a.lower[0], b.lower[0]},
                                             {a.spine[0], b.spine[0]}, {a.spine[2], b.spine[2]}, {a.neck, b.neck}};
        for (const auto& pr : pairs) {
            const Quat da = rotOf(a.skel, p, pr.first) * conjq(rotOf(a.skel, ra, pr.first));
            const Quat db = rotOf(b.skel, out, pr.second) * conjq(rotOf(b.skel, rb, pr.second));
            worst = std::fmax(worst, angleDeg(da, db));
        }
        check(worst < 0.3f, "world-space rotation deltas agree across different bone frames, worst " + f2s(worst) + " degrees");
        // Feet stay on the (scaled) ground: the IK legs reach the scaled goal.
        check(gap(posOf(b.skel, out, b.foot[1]), posOf(a.skel, p, a.foot[1]) * 1.3f) < 0.6f,
              "the right ankle is at 1.3x the source ankle, off by " + f2s(gap(posOf(b.skel, out, b.foot[1]), posOf(a.skel, p, a.foot[1]) * 1.3f)));
    }

    AVER_INFO("=== T-pose source, A-pose target ===");
    {
        const Rig a = makeRig(Params{Naming::Ue, 1.0f, false, 3, true});
        const Rig b = makeRig(Params{Naming::Ue, 1.0f, true, 3, false});
        anim::Retargeter rt;
        check(rt.bind(autoAsset(a, b), a.skel, b.skel), "it binds");
        anim::Pose rest;
        anim::restPose(a.skel, rest);
        anim::Pose out;
        rt.retarget(rest, out);
        const Vec3 d0 = dirModel(b.skel, out, b.upper[0], b.lower[0]);
        check(dot(d0, Vec3{0, -1, 0}) > 0.999f, "the source's rest (arms out) puts the target's arms out too, cos " + f2s(dot(d0, Vec3{0, -1, 0})));

        anim::Pose p = rest;
        rotateBone(a.skel, p, a.clav[0], Quat::identity());
        rotateBone(a.skel, p, a.upper[0], Quat::fromAxisAngle(Vec3{1, 0, 0}, -radians(90.0f)));   // raise the arm to +Z
        rotateBone(a.skel, p, a.lower[0], Quat::fromAxisAngle(Vec3{1, 0, 0}, -radians(90.0f)));
        const Vec3 want = dirModel(a.skel, p, a.upper[0], a.lower[0]);
        check(dot(want, Vec3{0, 0, 1}) > 0.999f, "(the source arm points up)");
        anim::Pose out2;
        rt.retarget(p, out2);
        const Vec3 got = dirModel(b.skel, out2, b.upper[0], b.lower[0]);
        check(dot(got, want) > 0.995f, "the raised arm maps to the same model-space direction on the target, cos " + f2s(dot(got, want)));
        check(dot(dirModel(b.skel, out2, b.lower[0], b.hand[0]), dirModel(a.skel, p, a.lower[0], a.hand[0])) > 0.99f, "and so does the forearm");
        const Vec3 other = dirModel(b.skel, out2, b.upper[1], b.lower[1]);
        check(dot(other, Vec3{0, 1, 0}) > 0.999f, "the other arm, untouched, stays out to its own side");
    }

    AVER_INFO("=== chain resampling: 3 -> 4 bones and 4 -> 3 ===");
    {
        for (int dir = 0; dir < 2; ++dir) {
            const int ns = dir == 0 ? 3 : 4, nd = dir == 0 ? 4 : 3;
            const Rig a = makeRig(Params{Naming::Ue, 1.0f, false, ns, true});
            const Rig b = makeRig(Params{Naming::Ue, 1.0f, false, nd, false});
            anim::Retargeter rt;
            check(rt.bind(autoAsset(a, b), a.skel, b.skel), std::to_string(ns) + " -> " + std::to_string(nd) + ": it binds");
            anim::Pose p;
            anim::restPose(a.skel, p);
            // A progressive bend about +Y: each source spine bone adds the same increment.
            for (int k = 0; k < ns; ++k) rotateBone(a.skel, p, a.spine[static_cast<usize>(k)], Quat::fromAxisAngle(Vec3{0, 1, 0}, radians(12.0f) * static_cast<f32>(k + 1)));
            anim::Pose out;
            rt.retarget(p, out);
            anim::Pose ra, rb;
            anim::restPose(a.skel, ra);
            anim::restPose(b.skel, rb);
            auto bendDeg = [&](const Rig& r, const anim::Pose& pp, const anim::Pose& rest, int bone) {
                return angleDeg(rotOf(r.skel, pp, bone), rotOf(r.skel, rest, bone));
            };
            const f32 endSrc = bendDeg(a, p, ra, a.spine.back()), endDst = bendDeg(b, out, rb, b.spine.back());
            check(std::fabs(endSrc - endDst) < 0.2f, "the end bones carry the same bend: " + f2s(endSrc) + " vs " + f2s(endDst));
            bool monotone = true;
            f32 prev = -1.0f;
            for (const int k : b.spine) {
                const f32 v = bendDeg(b, out, rb, k);
                if (v < prev - 1e-3f) monotone = false;
                prev = v;
            }
            check(monotone, "and the intermediate target bones increase monotonically");
        }
    }

    AVER_INFO("=== IK legs: reachable goals land, an unreachable one straightens without stretching ===");
    {
        const Rig a = makeRig(Params{Naming::Ue, 1.0f, false, 3, true});
        const Rig b = makeRig(Params{Naming::Ue, 1.0f, false, 3, false, 0.8f});      // shorter legs
        anim::RetargetAsset as = autoAsset(a, b);
        // Explicit scales so the lifted foot goal is out of the shorter leg's reach.
        as.root.scaleH = anim::RtgScale{false, 1.0f};
        as.root.scaleV = anim::RtgScale{false, 1.0f};
        anim::Retargeter rt;
        check(rt.bind(as, a.skel, b.skel), "it binds");
        anim::Pose p;
        anim::restPose(a.skel, p);
        // Lift the source's right leg forward and up, nearly straight.
        rotateBone(a.skel, p, a.thigh[1], Quat::fromAxisAngle(Vec3{0, 1, 0}, -radians(55.0f)));
        const Vec3 srcFoot = posOf(a.skel, p, a.foot[1]);
        anim::Pose out;
        rt.retarget(p, out);
        const Vec3 hip = posOf(b.skel, out, b.thigh[1]);
        const Vec3 foot = posOf(b.skel, out, b.foot[1]);
        anim::Pose restB;
        anim::restPose(b.skel, restB);
        const f32 r1 = gap(posOf(b.skel, restB, b.calf[1]), posOf(b.skel, restB, b.thigh[1]));
        const f32 r2 = gap(posOf(b.skel, restB, b.foot[1]), posOf(b.skel, restB, b.calf[1]));
        const f32 reach = r1 + r2;
        const f32 l1 = gap(posOf(b.skel, out, b.calf[1]), hip), l2 = gap(foot, posOf(b.skel, out, b.calf[1]));
        check(std::fabs(l1 - r1) < 0.2f && std::fabs(l2 - r2) < 0.2f, "the shorter bones kept their lengths: " + f2s(l1) + " / " + f2s(l2) + " (rest " + f2s(r1) + " / " + f2s(r2) + ")");
        check(gap(foot, hip) > reach - 1.0f && gap(foot, hip) <= reach + 0.2f, "the leg is fully extended, hip-ankle " + f2s(gap(foot, hip)) + " of " + f2s(reach));
        (void)srcFoot;

        // A reachable pose with proportional legs: the foot lands on the scaled goal.
        const Rig c = makeRig(Params{Naming::Ue, 1.0f, false, 3, false, 1.0f});
        anim::Retargeter rt2;
        check(rt2.bind(autoAsset(a, c), a.skel, c.skel), "(the same-length target binds)");
        anim::Pose q;
        anim::restPose(a.skel, q);
        rotateBone(a.skel, q, a.thigh[0], Quat::fromAxisAngle(Vec3{0, 1, 0}, -radians(25.0f)));
        rotateBone(a.skel, q, a.calf[0], Quat::fromAxisAngle(Vec3{0, 1, 0}, radians(35.0f)));
        anim::Pose out2;
        rt2.retarget(q, out2);
        check(gap(posOf(c.skel, out2, c.foot[0]), posOf(a.skel, q, a.foot[0])) < 0.5f, "a reachable pose: the ankle lands within 0.5 cm of the goal");
    }

    AVER_INFO("=== tip rotation, scaled translation, unmapped bones ===");
    {
        const Rig a = makeRig(Params{Naming::Ue, 1.0f, false, 3, true, 1.0f, true, true, false});
        const Rig b = makeRig(Params{Naming::Ue, 1.0f, false, 3, false, 1.0f, true, true, true});
        // The prop bone sits 10 cm out on the source and 20 cm on the target: ratio 2.
        Rig bs = b;
        bs.skel.bones[static_cast<usize>(bs.gun)].translation = bs.skel.bones[static_cast<usize>(bs.gun)].translation * 2.0f;
        anim::RetargetAsset as = autoAsset(a, bs);
        anim::RetargetChainDef gun;
        gun.name = "Gun";
        gun.srcStart = gun.srcEnd = a.skel.bones[static_cast<usize>(a.gun)].name;
        gun.dstStart = gun.dstEnd = bs.skel.bones[static_cast<usize>(bs.gun)].name;
        gun.translation = anim::RtgTranslation::Scaled;
        as.chains.push_back(gun);
        for (auto& c : as.chains) if (c.name == "RightLeg") c.tipRotation = true;
        anim::Retargeter rt;
        std::vector<anim::RetargetIssue> issues;
        check(rt.bind(as, a.skel, bs.skel, &issues), "it binds");
        anim::Pose p;
        anim::restPose(a.skel, p);
        p.local[static_cast<usize>(a.gun)].position = p.local[static_cast<usize>(a.gun)].position + Vec3{3, 0, 0};
        rotateBone(a.skel, p, a.foot[1], Quat::fromAxisAngle(Vec3{0, 1, 0}, radians(25.0f)));
        anim::Pose out;
        rt.retarget(p, out);
        anim::Pose rb;
        anim::restPose(bs.skel, rb);
        const i32 gunParent = bs.skel.bones[static_cast<usize>(bs.gun)].parent;
        // The gun's local offset moved by 3 cm in the SOURCE's frame; carried through model space and scaled 2x.
        const Vec3 dLocal = out.local[static_cast<usize>(bs.gun)].position - rb.local[static_cast<usize>(bs.gun)].position;
        const Quat qp = rotOf(bs.skel, out, gunParent);
        const Vec3 dModel = qp.rotate(dLocal);
        // The slide is along the SOURCE hand's local X, which is the arm direction in its aligned frame.
        const Vec3 srcModel = rotOf(a.skel, p, a.hand[1]).rotate(Vec3{3, 0, 0});
        check(gap(dModel, srcModel * 2.0f) < 0.1f, "translation:scaled carries a 3 cm slide as 6 cm in the same model direction, got " +
              f2s(dModel.x) + "," + f2s(dModel.y) + "," + f2s(dModel.z));
        // Orientation compared as deltas from each rest, since the two skeletons' bone frames differ.
        anim::Pose ra2;
        anim::restPose(a.skel, ra2);
        const Quat dSrc = rotOf(a.skel, p, a.foot[1]) * conjq(rotOf(a.skel, ra2, a.foot[1]));
        const Quat dDst = rotOf(bs.skel, out, bs.foot[1]) * conjq(rotOf(bs.skel, rb, bs.foot[1]));
        check(angleDeg(dSrc, dDst) < 1.5f, "tipRotation: the foot's world-space rotation matches the source's, off by " + f2s(angleDeg(dSrc, dDst)) + " degrees");
        const usize ex = static_cast<usize>(bs.extra);
        check(out.local[ex].position.x == rb.local[ex].position.x && out.local[ex].position.z == rb.local[ex].position.z &&
              angleDeg(out.local[ex].rotation, rb.local[ex].rotation) < 1e-4f, "an unmapped target bone keeps its rest local transform");
    }

    AVER_INFO("=== auto-map: three naming schemes ===");
    {
        const Rig mix = makeRig(Params{Naming::Mixamo, 1.0f, false, 3, false, 1.0f, false});
        const Rig ue = makeRig(Params{Naming::Ue, 1.0f, false, 3, true});
        const Rig bip = makeRig(Params{Naming::Bip, 1.0f, false, 2, false, 1.0f, true});
        auto find = [](const anim::AutoMapResult& m, const std::string& name) -> int {
            for (usize i = 0; i < m.asset.chains.size(); ++i) if (m.asset.chains[i].name == name) return static_cast<int>(i);
            return -1;
        };
        const anim::AutoMapResult m = anim::autoMapChains(mix.skel, ue.skel);
        const int arm = find(m, "LeftArm"), leg = find(m, "RightLeg"), spine = find(m, "Spine"), pel = find(m, "Pelvis");
        check(arm >= 0 && m.asset.chains[static_cast<usize>(arm)].srcStart == "mixamorig:LeftArm" && m.asset.chains[static_cast<usize>(arm)].srcEnd == "mixamorig:LeftHand" &&
              m.asset.chains[static_cast<usize>(arm)].dstStart == "upperarm_l" && m.asset.chains[static_cast<usize>(arm)].dstEnd == "hand_l", "Mixamo LeftArm..LeftHand -> upperarm_l..hand_l");
        check(leg >= 0 && m.asset.chains[static_cast<usize>(leg)].srcStart == "mixamorig:RightUpLeg" && m.asset.chains[static_cast<usize>(leg)].srcEnd == "mixamorig:RightFoot" &&
              m.asset.chains[static_cast<usize>(leg)].dstStart == "thigh_r" && m.asset.chains[static_cast<usize>(leg)].dstEnd == "foot_r" &&
              m.asset.chains[static_cast<usize>(leg)].mode == anim::RtgMode::Ik, "RightUpLeg..RightFoot -> thigh_r..foot_r, as an IK chain");
        check(spine >= 0 && m.asset.chains[static_cast<usize>(spine)].srcStart == "mixamorig:Spine" && m.asset.chains[static_cast<usize>(spine)].srcEnd == "mixamorig:Spine2" &&
              m.asset.chains[static_cast<usize>(spine)].dstStart == "spine_01" && m.asset.chains[static_cast<usize>(spine)].dstEnd == "spine_03", "Spine..Spine2 -> spine_01..spine_03");
        check(pel >= 0 && m.asset.root.srcBone == "mixamorig:Hips" && m.asset.root.dstBone == "pelvis", "Hips -> pelvis");
        check(arm >= 0 && m.confidence[static_cast<usize>(arm)] == anim::RtgConfidence::Synonym, "different schemes are marked synonym");
        check(find(m, "LeftClavicle") >= 0 && find(m, "RightToe") >= 0 && find(m, "Head") >= 0 && find(m, "Neck") >= 0, "clavicles, toes, neck and head are paired too");
        std::string why;
        anim::RetargetAsset full = m.asset;
        full.source = "mix.ocskel";
        full.target = "ue.ocskel";
        check(full.valid(mix.skel, ue.skel, &why), "and the draft validates: " + why);

        const anim::AutoMapResult m2 = anim::autoMapChains(bip.skel, ue.skel);
        const int arm2 = find(m2, "RightArm"), leg2 = find(m2, "LeftLeg"), spine2 = find(m2, "Spine");
        check(arm2 >= 0 && m2.asset.chains[static_cast<usize>(arm2)].srcStart == "Bip01 R UpperArm" && m2.asset.chains[static_cast<usize>(arm2)].srcEnd == "Bip01 R Hand" &&
              m2.asset.chains[static_cast<usize>(arm2)].dstStart == "upperarm_r", "Bip01 R UpperArm..Hand -> upperarm_r..hand_r");
        check(leg2 >= 0 && m2.asset.chains[static_cast<usize>(leg2)].srcEnd == "Bip01 L Foot" && m2.asset.chains[static_cast<usize>(leg2)].dstStart == "thigh_l", "Bip01 L Thigh..Foot -> thigh_l..foot_l");
        check(spine2 >= 0 && m2.asset.chains[static_cast<usize>(spine2)].srcStart == "Bip01 Spine" && m2.asset.chains[static_cast<usize>(spine2)].srcEnd == "Bip01 Spine1", "a 2-bone Bip spine maps onto a 3-bone one");
        check(m2.asset.rootMotion.srcBone == "Bip01" && m2.asset.rootMotion.dstBone == "root", "the root bones above the pelvis become the root-motion pair");

        const anim::AutoMapResult m3 = anim::autoMapChains(ue.skel, ue.skel);
        bool allExact = !m3.confidence.empty();
        for (const auto c : m3.confidence) if (c != anim::RtgConfidence::Exact) allExact = false;
        check(allExact, "the same naming scheme on both sides is all exact");

        // A creature the table cannot read: structural chains, paired by side and order.
        auto creature = [](const char* prefix, f32 s) {
            fmt::OcSkeleton k;
            auto add = [&](const std::string& n, i32 p, Vec3 pos, i32 parentIdx) {
                fmt::OcBone b;
                b.name = n; b.parent = p;
                const Vec3 pp = parentIdx >= 0 ? k.bones[static_cast<usize>(parentIdx)].translation : Vec3{0, 0, 0};
                b.translation = pos;
                (void)pp;
                k.bones.push_back(b);
            };
            const std::string pf = prefix;
            add(pf + "core", -1, Vec3{0, 0, 50 * s}, -1);
            add(pf + "tail1", 0, Vec3{-20 * s, 0, 0}, 0);
            add(pf + "tail2", 1, Vec3{-20 * s, 0, 0}, 1);
            add(pf + "limbA1", 0, Vec3{15 * s, -10 * s, 0}, 0);
            add(pf + "limbA2", 3, Vec3{0, 0, -20 * s}, 3);
            add(pf + "limbB1", 0, Vec3{15 * s, 10 * s, 0}, 0);
            add(pf + "limbB2", 5, Vec3{0, 0, -20 * s}, 5);
            k.rootBone = 0;
            return k;
        };
        const fmt::OcSkeleton ca = creature("w_", 1.0f), cb = creature("zz", 2.0f);
        const anim::AutoMapResult m4 = anim::autoMapChains(ca, cb);
        bool structural = !m4.asset.chains.empty();
        for (const auto c : m4.confidence) if (c != anim::RtgConfidence::Structural) structural = false;
        check(structural && m4.asset.chains.size() >= 3, "an unreadable creature falls back to structural chains, " + std::to_string(m4.asset.chains.size()) + " of them");
    }

    AVER_INFO("=== clips: retargetClip ===");
    const std::string tmp = (std::filesystem::temp_directory_path() / "aver-retarget-test").string();
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    {
        const Rig a = makeRig(Params{Naming::Ue, 1.0f, false, 3, true});
        const Rig b = makeRig(Params{Naming::Ue, 1.3f, true, 3, false});
        anim::RetargetAsset as = autoAsset(a, b, "Characters/mannequin.ocskel", "Characters/hero.ocskel");
        anim::Retargeter rt;
        check(rt.bind(as, a.skel, b.skel), "it binds");

        // A 2 second clip: the spine sways, the right arm swings, the root travels 100 cm forward.
        fmt::OcAnimation clip;
        clip.duration = 2.0f;
        clip.flags = static_cast<u8>(fmt::kOcAnimLoop | fmt::kOcAnimRootMotion);
        clip.skeletonRef = "mannequin";
        auto track = [&](int bone, bool pos) {
            fmt::OcTrack t;
            t.boneIndex = static_cast<u16>(bone);
            t.channels = static_cast<u8>(fmt::kOcChannelRotation | (pos ? fmt::kOcChannelTranslation : 0));
            return t;
        };
        anim::Pose restA;
        anim::restPose(a.skel, restA);
        for (int bone : {a.spine[1], a.upper[1], a.root}) {
            fmt::OcTrack t = track(bone, bone == a.root);
            for (int k = 0; k <= 8; ++k) {
                const f32 tt = 0.25f * static_cast<f32>(k);
                t.times.push_back(tt);
                if (bone == a.root) {
                    t.values.push_back(50.0f * tt); t.values.push_back(0.0f); t.values.push_back(0.0f);
                    t.values.push_back(0); t.values.push_back(0); t.values.push_back(0); t.values.push_back(1);
                } else {
                    const Quat q = (restA.local[static_cast<usize>(bone)].rotation * Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.5f * std::sin(tt * 3.0f))).normalized();
                    t.values.push_back(q.x); t.values.push_back(q.y); t.values.push_back(q.z); t.values.push_back(q.w);
                }
            }
            clip.tracks.push_back(std::move(t));
        }
        clip.notifies.push_back({0.5f, "Step"});
        clip.notifyDurations.push_back(0.25f);
        fmt::OcCurve curve;
        curve.name = "Plant";
        curve.times = {0.0f, 1.0f, 2.0f};
        curve.values = {0.0f, 1.0f, 0.0f};
        clip.curves.push_back(curve);
        std::string why;
        check(clip.valid(), "(the source clip is valid)");

        anim::RetargetOptions opt;
        fmt::OcAnimation out;
        std::vector<anim::RetargetIssue> issues;
        check(anim::retargetClip(rt, clip, a.skel, out, opt, &issues), "it retargets");
        check(out.valid(), "the result is a valid .ocanim");
        check(out.skeletonRef == "hero", "skeletonRef is the target's file stem, got '" + out.skeletonRef + "'");
        check(std::fabs(out.duration - 2.0f) < 1e-6f && (out.flags & fmt::kOcAnimLoop) && (out.flags & fmt::kOcAnimRootMotion) && !(out.flags & fmt::kOcAnimAdditiveBase),
              "duration, loop and root-motion flags are copied");
        check(out.notifies.size() == 1 && out.notifies[0].name == "Step" && out.notifyDurations.size() == 1 && out.curves.size() == 1 && out.curves[0].name == "Plant",
              "notifies, their durations and curves are copied");

        // The baked clip, sampled at a key time, equals the retargeter's own output for the source at that time.
        bool poseOk = true;
        f32 worstRot = 0.0f, worstPos = 0.0f;
        for (const f32 t : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
            anim::Pose sp, direct, baked;
            anim::restPose(a.skel, sp);
            anim::sampleAnimation(clip, t, sp);
            rt.retarget(sp, direct);
            anim::restPose(b.skel, baked);
            anim::sampleAnimation(out, t, baked);
            for (const u32 bone : rt.mappedDstBones()) {
                worstRot = std::fmax(worstRot, angleDeg(direct.local[bone].rotation, baked.local[bone].rotation));
                if (std::binary_search(rt.translatedDstBones().begin(), rt.translatedDstBones().end(), bone))
                    worstPos = std::fmax(worstPos, gap(direct.local[bone].position, baked.local[bone].position));
            }
        }
        poseOk = worstRot < 0.2f && worstPos < 0.1f;
        check(poseOk, "the baked clip matches the live retarget at sampled times, worst " + f2s(worstRot) + " degrees / " + f2s(worstPos) + " cm");

        // Key reduction stays under the tolerances at every original frame.
        anim::RetargetOptions full = opt;
        full.reduceKeys = false;
        fmt::OcAnimation dense;
        anim::retargetClip(rt, clip, a.skel, dense, full, nullptr);
        usize keysDense = 0, keysReduced = 0;
        for (const auto& t : dense.tracks) keysDense += t.times.size();
        for (const auto& t : out.tracks) keysReduced += t.times.size();
        check(keysReduced < keysDense, "reduceKeys drops keys: " + std::to_string(keysReduced) + " of " + std::to_string(keysDense));
        f32 worstReduce = 0.0f, worstReducePos = 0.0f;
        for (usize k = 0; k < 60; ++k) {
            const f32 t = static_cast<f32>(k) / 30.0f;
            anim::Pose x, y;
            anim::restPose(b.skel, x);
            anim::restPose(b.skel, y);
            anim::sampleAnimation(dense, t, x);
            anim::sampleAnimation(out, t, y);
            for (const u32 bone : rt.mappedDstBones()) {
                worstReduce = std::fmax(worstReduce, angleDeg(x.local[bone].rotation, y.local[bone].rotation));
                worstReducePos = std::fmax(worstReducePos, gap(x.local[bone].position, y.local[bone].position));
            }
        }
        check(worstReduce <= opt.rotToleranceDeg + 0.02f && worstReducePos <= opt.posToleranceCm + 0.01f,
              "reduced and dense agree within the tolerances: " + f2s(worstReduce) + " degrees / " + f2s(worstReducePos) + " cm");

        // Root motion survives, scaled by the horizontal scale.
        anim::RootSpec srcSpec, dstSpec;
        srcSpec.bone = static_cast<u32>(a.root);
        srcSpec.rest = Transform{a.skel.bones[static_cast<usize>(a.root)].translation, a.skel.bones[static_cast<usize>(a.root)].rotation, Vec3{1, 1, 1}};
        dstSpec.bone = static_cast<u32>(b.root);
        dstSpec.rest = Transform{b.skel.bones[static_cast<usize>(b.root)].translation, b.skel.bones[static_cast<usize>(b.root)].rotation, Vec3{1, 1, 1}};
        const anim::RootDelta ds = anim::rootDeltaBetween(clip, srcSpec, 0.5f, 1.5f, false);
        const anim::RootDelta dd = anim::rootDeltaBetween(out, dstSpec, 0.5f, 1.5f, false);
        check(std::fabs(ds.move.x - 50.0f) < 0.1f && std::fabs(dd.move.x - 50.0f * rt.scaleH()) < 0.3f,
              "root motion on the baked clip is the source's times scaleH: " + f2s(dd.move.x) + " vs " + f2s(ds.move.x * rt.scaleH()));

        // What it refuses.
        fmt::OcAnimation additive = clip;
        additive.flags = fmt::kOcAnimAdditiveBase;
        fmt::OcAnimation object = clip;
        object.flags = fmt::kOcAnimObject;
        fmt::OcAnimation sink;
        std::vector<anim::RetargetIssue> bad;
        check(!anim::retargetClip(rt, additive, a.skel, sink, opt, &bad) && !bad.empty(), "an additive clip is refused with an issue");
        check(!anim::retargetClip(rt, object, a.skel, sink, opt, nullptr), "and an object clip");

        // retargetFolder: skip and overwrite rules.
        const std::string dirIn = tmp + "/in", dirOut = tmp + "/out";
        std::filesystem::create_directories(dirIn, ec);
        check(fmt::saveOcSkel(tmp + "/mannequin.ocskel", a.skel, &why) && fmt::saveOcSkel(tmp + "/hero.ocskel", b.skel, &why), "(skeleton files written)");
        fmt::OcAnimation other = clip;
        other.skeletonRef = "somebody_else";
        fmt::OcAnimation add2 = clip;
        add2.flags = fmt::kOcAnimAdditiveBase;
        check(fmt::saveOcAnim(dirIn + "/walk.ocanim", clip, &why) && fmt::saveOcAnim(dirIn + "/alien.ocanim", other, &why) &&
              fmt::saveOcAnim(dirIn + "/add.ocanim", add2, &why), "(clips written)");
        std::vector<std::string> paths = {dirIn + "/walk.ocanim", dirIn + "/alien.ocanim", dirIn + "/add.ocanim", dirIn + "/missing.ocanim"};
        anim::RetargetBatchResult r1 = anim::retargetFolder(as, tmp + "/mannequin.ocskel", tmp + "/hero.ocskel", paths, dirOut, "_hero", opt, false);
        check(r1.written == 1 && r1.skipped == 2 && r1.failed == 1, "one written, the other-skeleton and additive clips skipped, the missing one failed: " +
              std::to_string(r1.written) + "/" + std::to_string(r1.skipped) + "/" + std::to_string(r1.failed));
        check(std::filesystem::exists(dirOut + "/walk_hero.ocanim"), "the output is <stem><suffix>.ocanim in the output folder");
        anim::RetargetBatchResult r2 = anim::retargetFolder(as, tmp + "/mannequin.ocskel", tmp + "/hero.ocskel", {paths[0]}, dirOut, "_hero", opt, false);
        check(r2.written == 1, "a previous retarget output is overwritten without the flag");
        // A file that is NOT a retarget output must survive unless overwrite is set.
        check(fmt::saveOcAnim(dirOut + "/walk_hero.ocanim", clip, &why), "(replace the output with a source-skeleton clip)");
        anim::RetargetBatchResult r3 = anim::retargetFolder(as, tmp + "/mannequin.ocskel", tmp + "/hero.ocskel", {paths[0]}, dirOut, "_hero", opt, false);
        check(r3.written == 0 && r3.skipped == 1, "a foreign file in the way is not overwritten");
        anim::RetargetBatchResult r4 = anim::retargetFolder(as, tmp + "/mannequin.ocskel", tmp + "/hero.ocskel", {paths[0]}, dirOut, "_hero", opt, true);
        check(r4.written == 1, "unless overwrite is set");
    }
    std::filesystem::remove_all(tmp, ec);

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
