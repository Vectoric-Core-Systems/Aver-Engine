// RagdollBuilder: see RagdollBuilder.hpp.
#include "aver/ragdoll/RagdollBuilder.hpp"

#include "aver/anim/Pose.hpp"
#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace aver::ragdoll {

namespace {

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// strtof over a token that must be consumed whole, so "12abc" is an error rather than 12.
bool parseFloat(const std::string& s, f32& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const f32 v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// Reads `count` numbers following tok[at]; false when short or malformed.
bool numbersAfter(const std::vector<std::string>& tok, usize at, usize count, f32* out) {
    if (at + count >= tok.size()) return false;
    for (usize j = 0; j < count; ++j)
        if (!parseFloat(tok[at + 1 + j], out[j])) return false;
    return true;
}

} // namespace

i32 RagdollDef::partOf(u32 bone) const {
    i32 b = static_cast<i32>(bone);
    // Parents precede children, so the walk strictly decreases and terminates.
    while (b >= 0 && static_cast<u32>(b) < boneCount) {
        if (hasPart(static_cast<u32>(b))) return b;
        b = parents[static_cast<usize>(b)];
    }
    return -1;
}

i32 RagdollDef::boneByHash(u64 h) const {
    for (u32 i = 0; i < boneCount; ++i)
        if (nameHash[i] == h) return static_cast<i32>(i);
    return -1;
}

bool parseRagdollOverrides(std::string_view text, RagdollOverrides& out, std::string* why) {
    std::istringstream in{std::string(text)};
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& msg) {
        if (why) *why = "line " + std::to_string(lineNo) + ": " + msg;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNo;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream ls(line);
        std::vector<std::string> tok;
        for (std::string t; ls >> t;) tok.push_back(t);
        if (tok.empty()) continue;

        if (tok[0] == "mass") {
            f32 kg = 0;
            if (tok.size() != 2 || !parseFloat(tok[1], kg) || kg <= 0.0f)
                return fail("mass needs one positive number");
            out.totalMassKg = kg;
        } else if (tok[0] == "bone") {
            if (tok.size() < 3) return fail("bone needs a name and at least one setting");
            BoneOverride o;
            o.name = tok[1];
            for (usize i = 2; i < tok.size();) {
                const std::string& k = tok[i];
                f32 v[3] = {};
                if (k == "on")       { o.enable = 1; ++i; }
                else if (k == "off") { o.enable = 0; ++i; }
                else if (k == "radius" || k == "length") {
                    if (!numbersAfter(tok, i, 1, v) || v[0] <= 0.0f) return fail(k + " needs a positive number");
                    (k == "radius" ? o.radius : o.length) = v[0];
                    i += 2;
                } else if (k == "limits") {
                    if (!numbersAfter(tok, i, 3, v) || v[0] < 0 || v[1] < 0 || v[2] < 0)
                        return fail("limits needs three non-negative numbers");
                    o.hasLimits = true;
                    o.swing1 = v[0]; o.swing2 = v[1]; o.twist = v[2];
                    i += 4;
                } else if (k == "shape" || k == "joint") {
                    if (i + 1 >= tok.size()) return fail(k + " needs a name");
                    const std::string& w = tok[i + 1];
                    if (k == "shape") {
                        if (w == "capsule") o.shape = kShapeCapsule;
                        else if (w == "sphere") o.shape = kShapeSphere;
                        else return fail("unknown shape '" + w + "' (capsule or sphere)");
                    } else {
                        if (w == "swingtwist") o.joint = kJointSwingTwist;
                        else if (w == "hinge") o.joint = kJointHinge;
                        else if (w == "fixed") o.joint = kJointFixed;
                        else if (w == "ball") o.joint = kJointBall;
                        else return fail("unknown joint '" + w + "' (swingtwist, hinge, fixed or ball)");
                    }
                    i += 2;
                } else if (k == "offset") {
                    if (!numbersAfter(tok, i, 3, v)) return fail("offset needs three numbers");
                    o.hasOffset = true;
                    o.offset = Vec3{v[0], v[1], v[2]};
                    i += 4;
                } else if (k == "mass") {
                    if (!numbersAfter(tok, i, 1, v) || v[0] <= 0.0f) return fail("mass needs a positive number");
                    o.massKg = v[0];
                    i += 2;
                } else if (k == "friction" || k == "bounce") {
                    if (!numbersAfter(tok, i, 1, v) || v[0] < 0.0f) return fail(k + " needs a non-negative number");
                    (k == "friction" ? o.friction : o.bounce) = v[0];
                    i += 2;
                } else {
                    return fail("unknown setting '" + k + "'");
                }
            }
            out.bones.push_back(std::move(o));
        } else {
            return fail("unknown directive '" + tok[0] + "'");
        }
    }
    return true;
}

bool loadRagdollOverrides(const std::string& path, RagdollOverrides& out, std::string* why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (why) *why = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return parseRagdollOverrides(ss.str(), out, why);
}

std::string overridePathFor(const std::string& skeletonPath) {
    const auto slash = skeletonPath.find_last_of("/\\");
    const auto dot = skeletonPath.find_last_of('.');
    const usize stem = (dot == std::string::npos || (slash != std::string::npos && dot < slash))
                     ? skeletonPath.size() : dot;
    return skeletonPath.substr(0, stem) + ".ocragdoll";
}

RagdollDef buildRagdoll(const fmt::OcSkeleton& skel, const BuildOptions& opt, const RagdollOverrides* ov) {
    RagdollDef d;
    const usize n = skel.bones.size();
    d.boneCount = static_cast<u32>(n);
    d.parents.resize(n);
    d.capsules.assign(n * 7, 0.0f);
    d.limitsDeg.resize(n * 3);
    d.nameHash.resize(n);
    d.totalMassKg = (ov && ov->totalMassKg > 0.0f) ? ov->totalMassKg : opt.totalMassKg;
    for (usize i = 0; i < n; ++i) {
        d.parents[i] = skel.bones[i].parent;
        d.nameHash[i] = fnv1a64(std::string_view(skel.bones[i].name));
        d.limitsDeg[i * 3 + 0] = opt.swingDeg;
        d.limitsDeg[i * 3 + 1] = opt.swingDeg;
        d.limitsDeg[i * 3 + 2] = opt.twistDeg;
    }
    if (n == 0 || !skel.valid()) return d;

    // The skeleton's size: its largest bind-pose extent, so a tall biped, a long quadruped and a
    // small prop rig all scale their thresholds to themselves.
    anim::Pose rest;
    anim::restPose(skel, rest);
    std::vector<Mat4> model;
    anim::poseToModel(skel, rest, model);
    Vec3 lo{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
    Vec3 hi{-lo.x, -lo.y, -lo.z};
    for (const Mat4& m : model) {
        lo = {std::min(lo.x, m.m[3][0]), std::min(lo.y, m.m[3][1]), std::min(lo.z, m.m[3][2])};
        hi = {std::max(hi.x, m.m[3][0]), std::max(hi.y, m.m[3][1]), std::max(hi.z, m.m[3][2])};
    }
    d.extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    if (d.extent < 1e-3f) return d;

    const f32 tiny = opt.tinyFraction * d.extent;
    const f32 tooLong = opt.maxLengthFraction * d.extent;
    const f32 minR = opt.minRadiusFraction * d.extent;
    const f32 maxR = opt.maxRadiusFraction * d.extent;

    std::vector<std::vector<u32>> kids(n);
    for (usize i = 0; i < n; ++i)
        if (skel.bones[i].parent >= 0)
            kids[static_cast<usize>(skel.bones[i].parent)].push_back(static_cast<u32>(i));

    auto overrideFor = [&](const std::string& name) -> const BoneOverride* {
        if (!ov) return nullptr;
        const BoneOverride* hit = nullptr;
        for (const BoneOverride& o : ov->bones) if (o.name == name) hit = &o;   // a later line wins
        return hit;
    };

    for (usize i = 0; i < n; ++i) {
        const fmt::OcBone& b = skel.bones[i];
        const BoneOverride* o = overrideFor(b.name);
        if (o && o->enable == 0) continue;

        // Segment direction: toward the longest child; a bone with none extends along its own
        // parent->bone direction (only reachable through an override).
        Vec3 dir{1, 0, 0};
        f32 len = 0.0f;
        f32 lateral = 0.0f;
        if (!kids[i].empty()) {
            u32 best = kids[i][0];
            for (u32 c : kids[i])
                if (skel.bones[c].translation.size() > skel.bones[best].translation.size()) best = c;
            const Vec3 t = skel.bones[best].translation;
            len = t.size();
            dir = t.getSafeNormal();
            for (u32 c : kids[i]) {
                if (c == best) continue;
                const Vec3 u = skel.bones[c].translation;
                lateral = std::max(lateral, (u - dir * dot(u, dir)).size());
            }
        } else if (o && o->enable == 1 && o->length > 0.0f) {
            // The parent->bone direction is in the parent's frame; the bone's frame is that rotated
            // by the inverse of the bone's own rest rotation.
            const Vec3 inParent = b.translation.getSafeNormal();
            if (inParent.sizeSquared() > 0.0f) {
                const Quat inv{-b.rotation.x, -b.rotation.y, -b.rotation.z, b.rotation.w};
                dir = inv.rotate(inParent).getSafeNormal();
            }
            len = o->length;
        } else {
            continue;   // a leaf has no direction of its own
        }

        if (o && o->length > 0.0f) len = o->length;
        const bool forced = o && o->enable == 1;
        if (!forced && (len < tiny || len > tooLong)) continue;
        if (len <= 1e-4f) continue;

        f32 r = clampf(std::max(opt.radiusPerLength * len, 0.5f * lateral), minR, maxR);
        if (o && o->radius > 0.0f) r = o->radius;

        const f32 inset = std::min(0.5f * r, 0.25f * len);
        Vec3 a = dir * inset;
        Vec3 e = dir * (len - inset);
        if (o && o->shape == kShapeSphere) a = e = (a + e) * 0.5f;       // a sphere at the middle of the segment
        if (o && o->hasOffset) { a = a + o->offset; e = e + o->offset; }
        f32* c = &d.capsules[i * 7];
        c[0] = a.x; c[1] = a.y; c[2] = a.z;
        c[3] = e.x; c[4] = e.y; c[5] = e.z;
        c[6] = r;
        if (o && o->hasLimits) {
            d.limitsDeg[i * 3 + 0] = o->swing1;
            d.limitsDeg[i * 3 + 1] = o->swing2;
            d.limitsDeg[i * 3 + 2] = o->twist;
        }
        if (o && (o->massKg > 0.0f || o->friction >= 0.0f || o->bounce >= 0.0f || o->joint >= 0)) {
            if (d.massKg.empty()) {
                d.massKg.assign(n, -1.0f);
                d.friction.assign(n, -1.0f);
                d.bounce.assign(n, -1.0f);
                d.joint.assign(n, kJointSwingTwist);
            }
            if (o->massKg > 0.0f) d.massKg[i] = o->massKg;
            if (o->friction >= 0.0f) d.friction[i] = o->friction;
            if (o->bounce >= 0.0f) d.bounce[i] = o->bounce;
            if (o->joint >= 0) {
                d.joint[i] = o->joint;
                f32* lim = &d.limitsDeg[i * 3];
                if (o->joint == kJointHinge) { lim[1] = 0.0f; lim[2] = 0.0f; }
                else if (o->joint == kJointFixed) { lim[0] = lim[1] = lim[2] = 0.0f; }
                else if (o->joint == kJointBall) { lim[0] = lim[1] = lim[2] = 180.0f; }
            }
        }
        ++d.partCount;
        if (d.rootPart < 0) d.rootPart = static_cast<i32>(i);
    }
    return d;
}

} // namespace aver::ragdoll
