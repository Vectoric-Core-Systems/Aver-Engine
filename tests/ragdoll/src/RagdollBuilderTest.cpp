// RagdollBuilderTest -- a skeleton's bind pose to per-bone capsules and limits, with no world and no
// physics. The claims are about the RULE (what gets a part, how big, how it is limited), checked on a
// hand-made humanoid whose finger and twist bones are the cases the rule exists to skip.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/ragdoll/RagdollBuilder.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

namespace {

enum Bone : int {
    Root, Pelvis, Spine, Chest, Neck, Head, HeadTop, ThighL, KneeL, FootL, ToeL,
    UpperArmL, ForearmL, HandL, FingerA, FingerB, TwistL, TwistTip, BoneCount
};

// Z up, cm, ~175 cm tall. Root is a transform node standing on the floor over the pelvis; FingerA/B and
// TwistL/TwistTip are the tiny helpers; HeadTop and ToeL are leaves.
fmt::OcSkeleton humanoid(f32 scale = 1.0f) {
    struct Def { const char* name; int parent; Vec3 t; };
    const Def defs[BoneCount] = {
        {"Root", -1, {0, 0, 0}},         {"Pelvis", Root, {0, 0, 95}},   {"Spine", Pelvis, {0, 0, 12}},
        {"Chest", Spine, {0, 0, 18}},    {"Neck", Chest, {0, 0, 20}},    {"Head", Neck, {0, 0, 10}},
        {"HeadTop", Head, {0, 0, 20}},   {"ThighL", Pelvis, {0, -9, -4}}, {"KneeL", ThighL, {0, 0, -42}},
        {"FootL", KneeL, {0, 0, -40}},   {"ToeL", FootL, {14, 0, -6}},
        {"UpperArmL", Chest, {0, -16, -2}}, {"ForearmL", UpperArmL, {0, -28, 0}}, {"HandL", ForearmL, {0, -25, 0}},
        {"FingerA", HandL, {0, -9, 0}},  {"FingerB", FingerA, {0, -3, 0}},
        {"TwistL", UpperArmL, {0, -10, 0}}, {"TwistTip", TwistL, {0, -2, 0}},
    };
    fmt::OcSkeleton s;
    for (const Def& d : defs) {
        fmt::OcBone b;
        b.name = d.name;
        b.parent = d.parent;
        b.translation = d.t * scale;
        s.bones.push_back(b);
    }
    s.rootBone = 0;
    return s;
}

f32 capLen(const ragdoll::RagdollDef& d, int bone) {
    const f32* c = &d.capsules[static_cast<usize>(bone) * 7];
    return Vec3{c[3] - c[0], c[4] - c[1], c[5] - c[2]}.size();
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("ragdoll builder: skeleton -> capsules and limits");

    const fmt::OcSkeleton skel = humanoid();
    const ragdoll::RagdollDef d = ragdoll::buildRagdoll(skel);

    AVER_INFO("=== which bones get a part ===");
    check(d.boneCount == BoneCount, "one entry per bone");
    check(d.partCount == 11, "11 parts, got " + std::to_string(d.partCount));
    for (int b : {Pelvis, Spine, Chest, Neck, Head, ThighL, KneeL, FootL, UpperArmL, ForearmL, HandL})
        check(d.hasPart(static_cast<u32>(b)), "'" + skel.bones[static_cast<usize>(b)].name + "' has a part");
    check(!d.hasPart(Root), "the Root transform node (longer than a limb can be) has none");
    check(!d.hasPart(FingerA) && !d.hasPart(FingerB), "fingers are skipped as tiny");
    check(!d.hasPart(TwistL) && !d.hasPart(TwistTip), "twist helpers are skipped as tiny");
    check(!d.hasPart(HeadTop) && !d.hasPart(ToeL), "a leaf has no direction, so no part");
    check(d.rootPart == Pelvis, "the root part is the first bone with one");

    AVER_INFO("=== riding bones resolve to their ancestor's part ===");
    check(d.partOf(ToeL) == FootL, "the toe rides the foot");
    check(d.partOf(HeadTop) == Head, "the head top rides the head");
    check(d.partOf(FingerB) == HandL, "a finger rides the hand");
    check(d.partOf(Root) == -1, "a bone with no part above it resolves to nothing");

    AVER_INFO("=== geometry ===");
    {
        const f32 lo = 0.012f * d.extent, hi = 0.07f * d.extent;
        bool inRange = true;
        for (u32 b = 0; b < d.boneCount; ++b)
            if (d.hasPart(b)) {
                const f32 r = d.capsules[b * 7 + 6];
                inRange = inRange && r >= lo - 1e-3f && r <= hi + 1e-3f;
            }
        check(inRange, "every radius is inside [" + f2s(lo) + ", " + f2s(hi) + "] cm, scaled to the skeleton");

        // The thigh's segment runs down its own bone toward the knee, shortened by the cap inset.
        const f32* c = &d.capsules[ThighL * 7];
        check(c[2] < 0 && c[5] < c[2] && std::fabs(c[0]) < 1e-3f && std::fabs(c[1]) < 1e-3f,
              "the thigh capsule points down toward the knee");
        check(capLen(d, ThighL) < 42.0f && capLen(d, ThighL) > 30.0f,
              "and is about the thigh's length, " + f2s(capLen(d, ThighL)) + " cm of 42");
        // The pelvis has three children; the hip joints sit 9 cm off its axis, which its radius covers.
        check(d.capsules[Pelvis * 7 + 6] >= 4.4f, "the pelvis radius covers its hip joints");
    }

    AVER_INFO("=== hierarchy and limits ===");
    {
        bool parentsMatch = true;
        for (usize i = 0; i < skel.bones.size(); ++i) parentsMatch = parentsMatch && d.parents[i] == skel.bones[i].parent;
        check(parentsMatch, "parents are the skeleton's, parents before children");
        const f32* l = &d.limitsDeg[KneeL * 3];
        check(l[0] == 45.0f && l[1] == 45.0f && l[2] == 30.0f, "default limits are 45/45 swing, 30 twist");
        check(l[2] < l[0], "twist is tighter than swing");
        check(d.boneByHash(fnv1a64(std::string_view("Head"))) == Head, "bones are findable by name hash");
        check(d.boneByHash(fnv1a64(std::string_view("Nope"))) == -1, "and an unknown hash finds nothing");
    }

    AVER_INFO("=== the rule is relative to the skeleton, not to centimetres ===");
    {
        const ragdoll::RagdollDef small = ragdoll::buildRagdoll(humanoid(0.1f));
        check(small.partCount == d.partCount, "a tenth-size copy gets the same parts");
        check(std::fabs(small.extent * 10.0f - d.extent) < 0.5f, "its extent is a tenth");
        check(std::fabs(small.capsules[ThighL * 7 + 6] * 10.0f - d.capsules[ThighL * 7 + 6]) < 0.2f,
              "and its radii scale with it");
    }

    AVER_INFO("=== unusable skeletons yield no parts ===");
    {
        check(ragdoll::buildRagdoll(fmt::OcSkeleton{}).partCount == 0, "an empty skeleton has none");
        fmt::OcSkeleton bad = humanoid();
        bad.bones[Spine].parent = Chest;            // a parent that comes later
        check(ragdoll::buildRagdoll(bad).partCount == 0, "a skeleton that is not parents-first has none");
    }

    AVER_INFO("=== override file ===");
    {
        const char* text =
            "# tune one rig\n"
            "mass 90\n"
            "bone Head off            # no head part\n"
            "bone HeadTop on length 15 radius 6\n"
            "bone ThighL limits 80 70 10\n"
            "bone NotABone radius 3\n";
        ragdoll::RagdollOverrides ov;
        std::string why;
        check(ragdoll::parseRagdollOverrides(text, ov, &why), "it parses: " + why);
        check(ov.totalMassKg == 90.0f && ov.bones.size() == 4, "mass and four bone lines");

        const ragdoll::RagdollDef o = ragdoll::buildRagdoll(skel, {}, &ov);
        check(o.totalMassKg == 90.0f, "mass overrides the default");
        check(!o.hasPart(Head), "'off' removes a part");
        check(o.hasPart(HeadTop) && o.capsules[HeadTop * 7 + 6] == 6.0f, "'on' + radius gives a leaf a part");
        check(std::fabs(o.capsules[HeadTop * 7 + 5]) > 5.0f && o.capsules[HeadTop * 7 + 5] < 15.0f &&
              std::fabs(o.capsules[HeadTop * 7 + 3]) < 1e-3f,
              "running up the bone from the leaf's own direction, about 'length' cm long");
        check(o.limitsDeg[ThighL * 3 + 0] == 80.0f && o.limitsDeg[ThighL * 3 + 1] == 70.0f &&
              o.limitsDeg[ThighL * 3 + 2] == 10.0f, "limits are replaced for the named bone");
        check(o.limitsDeg[KneeL * 3 + 0] == 45.0f, "and not for the others");
        check(o.partCount == 11, "a name that matches no bone is ignored, 11 parts, got " + std::to_string(o.partCount));

        ragdoll::RagdollOverrides bad;
        why.clear();
        check(!ragdoll::parseRagdollOverrides("bone Head frobnicate\n", bad, &why) && why.find("line 1") == 0,
              "an unknown setting is an error naming the line: " + why);
        check(!ragdoll::parseRagdollOverrides("bone Head radius -3\n", bad, &why), "a negative radius is an error");
        check(!ragdoll::parseRagdollOverrides("bone Head limits 1 2\n", bad, &why), "short limits are an error");
        check(!ragdoll::parseRagdollOverrides("mass\n", bad, &why), "a bare mass is an error");
        check(!ragdoll::parseRagdollOverrides("wibble 3\n", bad, &why), "an unknown directive is an error");

        // From a file on disk, by the path rule.
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / "aver-ragdoll-builder-test";
        std::filesystem::create_directories(dir, ec);
        const std::string skelPath = (dir / "Hero.ocskel").string();
        const std::string path = ragdoll::overridePathFor(skelPath);
        check(std::filesystem::path(path).filename() == "Hero.ocragdoll", "'<skeleton>.ocskel' maps to '<skeleton>.ocragdoll'");
        { std::ofstream f(path, std::ios::binary); f << text; }
        ragdoll::RagdollOverrides fromFile;
        check(ragdoll::loadRagdollOverrides(path, fromFile, &why) && fromFile.bones.size() == 4, "and it loads from disk");
        check(!ragdoll::loadRagdollOverrides((dir / "missing.ocragdoll").string(), fromFile, &why), "a missing file reports failure");
        std::filesystem::remove_all(dir, ec);
        check(ragdoll::overridePathFor("noext") == "noext.ocragdoll", "a path with no extension just gains one");
        check(ragdoll::overridePathFor("a.b/noext") == "a.b/noext.ocragdoll", "a dot in a directory is not an extension");
    }

    AVER_INFO("=== authored per-part data: shape, offset, mass, material, joint ===");
    {
        const char* text =
            "bone Head shape sphere mass 5.5 friction 0.9 bounce 0.2\n"
            "bone ThighL offset 1 2 3 joint hinge limits 60 40 20\n"
            "bone KneeL joint fixed\n"
            "bone UpperArmL joint ball\n";
        ragdoll::RagdollOverrides ov;
        std::string why;
        check(ragdoll::parseRagdollOverrides(text, ov, &why), "it parses: " + why);
        const ragdoll::RagdollDef plain = ragdoll::buildRagdoll(skel);
        check(plain.massKg.empty() && plain.friction.empty() && plain.joint.empty(), "without data the per-part arrays stay empty");
        const ragdoll::RagdollDef d = ragdoll::buildRagdoll(skel, {}, &ov);
        check(d.massKg.size() == d.boneCount && d.massKg[Head] == 5.5f && d.massKg[KneeL] < 0.0f, "a part's own mass, the others by volume");
        check(d.friction[Head] == 0.9f && d.bounce[Head] == 0.2f && d.friction[ThighL] < 0.0f, "friction and bounce of one part");
        const f32* hc = &d.capsules[Head * 7];
        check(std::fabs(hc[0] - hc[3]) < 1e-5f && std::fabs(hc[1] - hc[4]) < 1e-5f && std::fabs(hc[2] - hc[5]) < 1e-5f && hc[6] > 0.0f,
              "a sphere part has both ends at one point");
        const f32* pc = &plain.capsules[ThighL * 7];
        const f32* tc = &d.capsules[ThighL * 7];
        check(std::fabs(tc[0] - pc[0] - 1.0f) < 1e-4f && std::fabs(tc[1] - pc[1] - 2.0f) < 1e-4f && std::fabs(tc[5] - pc[5] - 3.0f) < 1e-4f,
              "an offset moves both ends of the segment");
        check(d.joint[ThighL] == ragdoll::kJointHinge && d.limitsDeg[ThighL * 3 + 0] == 60.0f && d.limitsDeg[ThighL * 3 + 1] == 0.0f &&
                  d.limitsDeg[ThighL * 3 + 2] == 0.0f,
              "a hinge keeps swing1 and locks the other two");
        check(d.limitsDeg[KneeL * 3] == 0.0f && d.limitsDeg[KneeL * 3 + 1] == 0.0f && d.limitsDeg[KneeL * 3 + 2] == 0.0f, "a fixed joint locks all three");
        check(d.limitsDeg[UpperArmL * 3] == 180.0f && d.limitsDeg[UpperArmL * 3 + 2] == 180.0f, "a ball joint is free");
        check(d.partCount == plain.partCount, "none of it adds or removes a part");

        ragdoll::RagdollOverrides bad;
        check(!ragdoll::parseRagdollOverrides("bone Head shape cube\n", bad, &why), "an unknown shape is an error: " + why);
        check(!ragdoll::parseRagdollOverrides("bone Head joint spring\n", bad, &why), "an unknown joint is an error");
        check(!ragdoll::parseRagdollOverrides("bone Head offset 1 2\n", bad, &why), "a short offset is an error");
        check(!ragdoll::parseRagdollOverrides("bone Head mass -2\n", bad, &why), "a negative mass is an error");
        check(!ragdoll::parseRagdollOverrides("bone Head friction -1\n", bad, &why), "a negative friction is an error");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
