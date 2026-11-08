// PhysAnimProfileTest -- the pure halves of physical animation: the .ocphysanim profile (text round trip,
// validation against a skeleton, region precedence, spread along the skeleton), the hit-reaction envelope
// (attack / hold / decay, falloff by hop, stacking and its cap, return to rest, determinism) and the
// death / recovery logic (state machine, and which get-up montage a body lying a given way gets).
// No world, no physics: a wrong envelope or a wrong montage is decidable here.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/ragdoll/HitReaction.hpp"
#include "aver/ragdoll/PhysAnimProfile.hpp"
#include "aver/ragdoll/RagdollLife.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::ragdoll;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }
static bool near(f32 a, f32 b, f32 tol = 1e-4f) { return std::fabs(a - b) <= tol; }

enum Bone : int { Root, Pelvis, Spine, Chest, Neck, Head, UpperArmL, ForearmL, HandL, ThighL, KneeL, BoneCount };

static fmt::OcSkeleton body() {
    struct Def { const char* name; int parent; };
    const Def defs[BoneCount] = {
        {"Root", -1}, {"Pelvis", Root}, {"Spine", Pelvis}, {"Chest", Spine}, {"Neck", Chest}, {"Head", Neck},
        {"UpperArmL", Chest}, {"ForearmL", UpperArmL}, {"HandL", ForearmL}, {"ThighL", Pelvis}, {"KneeL", ThighL},
    };
    fmt::OcSkeleton s;
    for (const Def& d : defs) {
        fmt::OcBone b;
        b.name = d.name;
        b.parent = d.parent;
        s.bones.push_back(b);
    }
    s.rootBone = 0;
    return s;
}

static const char* kProfileText =
    "# test\n"
    "physanim brawler\n"
    "reach falloff 0.5 hops 3 epsilon 0.02 impulse 2000\n"
    "life death 0.8 getupblend 0.6 supine 0x10 prone 32 side 0\n"
    "default strength 0.4 blend 0.9\n"
    "region arm subtree UpperArmL strength 0.2 blend 1 boost 0.5 attack 0.1 hold 0.1 decay 0.4 impulse 2\n"
    "region head bone Head bone Neck strength 0.7 decay 0.2\n"
    "region hand bone HandL strength 0.1\n";

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("physical animation: profile, hit reaction, death and recovery (pure)");

    const fmt::OcSkeleton skel = body();

    AVER_INFO("=== the profile: parse, round trip, validate ===");
    PhysAnimProfile prof;
    {
        std::string why;
        check(parsePhysAnimProfile(kProfileText, prof, &why), "the sample parses: " + why);
        check(prof.name == "brawler" && prof.maxHops == 3 && near(prof.falloffPerHop, 0.5f) && near(prof.fullImpulse, 2000.0f),
              "reach and name are read");
        check(prof.getUpSupine == 0x10 && prof.getUpProne == 32 && near(prof.deathSeconds, 0.8f), "life keys are read, hex and decimal ids");
        check(prof.regions.size() == 3 && prof.regions[0].subtrees.size() == 1 && prof.regions[1].bones.size() == 2, "three regions with their bones");
        check(near(prof.regions[0].drive.strength, 0.2f) && near(prof.regions[0].drive.impulse, 2.0f), "region drive keys are read");
        check(near(prof.regions[1].drive.blend, 0.9f), "a region starts from the defaults parsed before it");

        const std::string text = serializePhysAnimProfile(prof);
        PhysAnimProfile again;
        check(parsePhysAnimProfile(text, again, &why), "the serialised text parses: " + why);
        check(serializePhysAnimProfile(again) == text, "and serialises to the same text (round trip)");
        check(again.regions.size() == prof.regions.size() && again.regions[0].subtrees == prof.regions[0].subtrees &&
                  near(again.regions[2].drive.strength, 0.1f) && again.getUpSupine == prof.getUpSupine,
              "with the same content");

        const std::string path = (std::filesystem::temp_directory_path() / "aver-physanim-test.ocphysanim").string();
        PhysAnimProfile viaFile;
        check(savePhysAnimProfile(path, prof, &why) && loadPhysAnimProfile(path, viaFile, &why) &&
                  serializePhysAnimProfile(viaFile) == text, "a file round trip is the same: " + why);
        std::error_code ec;
        std::filesystem::remove(path, ec);

        PhysAnimProfile bad;
        check(!parsePhysAnimProfile("region x strength nope\n", bad, &why) && why.find("line 1") == 0, "a bad number names its line: " + why);
        check(!parsePhysAnimProfile("frobnicate\n", bad, &why), "an unknown directive is refused");
        check(!parsePhysAnimProfile("region x colour red\n", bad, &why), "an unknown key is refused");
    }
    {
        std::vector<std::string> problems;
        check(validatePhysAnimProfile(prof, skel, &problems), "the sample is valid for the skeleton");

        PhysAnimProfile p2 = prof;
        p2.regions.push_back(PhysAnimRegion{"ghost", {"NoSuchBone"}, {"NoSuchRoot"}, {}});
        p2.regions.push_back(PhysAnimRegion{"empty", {}, {}, {}});
        p2.regions.push_back(PhysAnimRegion{"arm", {"HandL"}, {}, {}});
        p2.regions[0].drive.strength = 1.5f;
        p2.falloffPerHop = 2.0f;
        problems.clear();
        check(!validatePhysAnimProfile(p2, skel, &problems), "a profile with unknown bones and bad values is refused");
        auto has = [&](const char* needle) {
            return std::any_of(problems.begin(), problems.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
        };
        check(has("unknown bone 'NoSuchBone'") && has("unknown subtree root 'NoSuchRoot'"), "the unknown bones are named");
        check(has("region 'empty' names no bone") && has("region 'arm' is defined twice"), "an empty and a duplicate region are named");
        check(has("strength") && has("falloff"), "out of range values are named");
    }

    AVER_INFO("=== resolving onto a skeleton ===");
    const ResolvedPhysAnim res = resolvePhysAnim(prof, skel);
    {
        check(res.boneCount == BoneCount && res.parents[Head] == Neck, "bone count and parents come from the skeleton");
        check(res.region[UpperArmL] == 0 && res.region[ForearmL] == 0 && res.region[HandL] == 2,
              "a subtree covers its descendants, and a later region wins for a bone both name (hand)");
        check(res.region[Head] == 1 && res.region[Neck] == 1 && res.region[Spine] == -1, "exact bones, and no region elsewhere");
        check(near(res.drive[Spine].strength, 0.4f) && near(res.drive[ForearmL].strength, 0.2f), "bones outside any region take the defaults");

        std::vector<std::pair<i32, i32>> w;
        res.withinHops(Chest, 2, w);
        auto hopsOf = [&](int b) { for (auto& [bone, h] : w) if (bone == b) return h; return -1; };
        check(hopsOf(Chest) == 0 && hopsOf(Spine) == 1 && hopsOf(Neck) == 1 && hopsOf(UpperArmL) == 1, "hops: the bone, its parent and its children");
        check(hopsOf(Pelvis) == 2 && hopsOf(Head) == 2 && hopsOf(ForearmL) == 2 && hopsOf(HandL) == -1 && hopsOf(ThighL) == -1,
              "two hops reach the grandparent and grandchildren, no further");
        std::vector<i32> sub;
        res.subtreeOf(UpperArmL, sub);
        check(sub.size() == 3 && sub[0] == UpperArmL && sub[2] == HandL, "subtreeOf lists the bone and its descendants");
    }

    AVER_INFO("=== the hit reaction envelope ===");
    {
        PhysAnimProfile p;
        p.regions.push_back(PhysAnimRegion{"all", {}, {"Root"}, {}});
        p.regions[0].drive.attack = 0.1f;
        p.regions[0].drive.hold = 0.2f;
        p.regions[0].drive.decay = 0.4f;
        p.regions[0].drive.strength = 0.3f;
        p.regions[0].drive.boost = 0.5f;
        p.regions[0].drive.blend = 0.8f;
        p.regions[0].drive.impulse = 1.5f;
        p.falloffPerHop = 0.5f;
        p.maxHops = 2;
        p.fullImpulse = 1000.0f;
        const ResolvedPhysAnim r = resolvePhysAnim(p, skel);

        HitReaction hr;
        hr.configure(r);
        check(!hr.active() && hr.envelope(Chest) == 0.0f, "nothing reacts before a hit");

        const Vec3 given = hr.trigger(Chest, Vec3{500, 0, 0});
        check(near(given.x, 750.0f), "the hit part's impulse is scaled by its region: " + f2s(given.x));
        check(hr.active() && hr.reacting(Chest) && hr.envelope(Chest) == 0.0f, "reacting from the instant of the hit, value still 0");
        hr.step(0.05f);
        check(near(hr.envelope(Chest), 0.25f, 1e-3f), "half way through the attack the hit bone is at half its peak 0.5: " + f2s(hr.envelope(Chest)));
        hr.step(0.05f);
        check(near(hr.envelope(Chest), 0.5f, 1e-3f), "at the end of the attack it is at the peak");
        hr.step(0.1f);
        check(near(hr.envelope(Chest), 0.5f, 1e-3f), "and holds there");
        hr.step(0.2f);   // hold ends at 0.3; now 0.4 -> 0.1 s into a 0.4 s decay
        check(near(hr.envelope(Chest), 0.5f * 0.75f, 1e-3f), "the decay is linear: " + f2s(hr.envelope(Chest)));
        check(near(hr.blend(Chest), hr.envelope(Chest) * 0.8f, 1e-5f), "blend is the envelope times the region blend");
        check(near(hr.strength(Chest), 0.3f + 0.5f * hr.envelope(Chest), 1e-5f), "strength is the base plus the boost at the envelope");

        // Spread: parent / children one hop at 0.5, two hops at 0.25 of the hit bone's size.
        HitReaction h2;
        h2.configure(r);
        h2.trigger(Chest, Vec3{1000, 0, 0});   // full size 1
        h2.step(0.1f);
        check(near(h2.envelope(Chest), 1.0f, 1e-3f) && near(h2.envelope(Spine), 0.5f, 1e-3f) && near(h2.envelope(UpperArmL), 0.5f, 1e-3f) &&
                  near(h2.envelope(Pelvis), 0.25f, 1e-3f) && near(h2.envelope(HandL), 0.0f) && near(h2.envelope(ThighL), 0.0f),
              "the reaction falls by half per hop along the skeleton and stops at maxHops");

        // Recovery: gone after attack + hold + decay, and active() says so.
        for (int i = 0; i < 60; ++i) h2.step(1.0f / 60.0f);
        check(!h2.active() && h2.envelope(Chest) == 0.0f && !h2.reacting(Spine), "everything has run out after the full envelope time");

        // Stacking: a second hit continues from the current value and is capped at 1.
        HitReaction h3;
        h3.configure(r);
        h3.trigger(Chest, Vec3{600, 0, 0});
        for (int i = 0; i < 12; ++i) h3.step(1.0f / 60.0f);   // 0.2 s: at the 0.6 peak, holding
        const f32 before = h3.envelope(Chest);
        h3.trigger(Chest, Vec3{300, 0, 0});
        check(near(h3.envelope(Chest), before, 1e-3f), "a second hit does not make the envelope jump: " + f2s(h3.envelope(Chest)));
        for (int i = 0; i < 12; ++i) h3.step(1.0f / 60.0f);
        check(near(h3.envelope(Chest), 0.9f, 2e-2f), "it rises to the sum of the two, 0.6 + 0.3: " + f2s(h3.envelope(Chest)));
        h3.trigger(Chest, Vec3{5000, 0, 0});
        for (int i = 0; i < 12; ++i) h3.step(1.0f / 60.0f);
        check(h3.envelope(Chest) <= 1.0f && near(h3.envelope(Chest), 1.0f, 2e-2f), "and caps at 1");

        // Determinism: two reactions fed the same hits agree bit for bit.
        HitReaction a, b;
        a.configure(r);
        b.configure(r);
        for (int step = 0; step < 90; ++step) {
            if (step % 17 == 0) { a.trigger(Head, Vec3{300.0f + step, 0, 0}); b.trigger(Head, Vec3{300.0f + step, 0, 0}); }
            a.step(1.0f / 60.0f);
            b.step(1.0f / 60.0f);
        }
        bool same = true;
        for (i32 i = 0; i < BoneCount; ++i) same = same && a.envelope(i) == b.envelope(i);
        check(same, "the same hits give the same envelopes, exactly");

        // An unknown bone does nothing; a radial contribution is capped too.
        HitReaction h4;
        h4.configure(r);
        const Vec3 none = h4.trigger(99, Vec3{1000, 0, 0});
        check(none.size() == 0.0f && !h4.active(), "a bone that does not exist reacts to nothing");
        h4.triggerRadial(Chest, 0.7f);
        h4.triggerRadial(Chest, 0.7f);
        h4.step(0.1f);
        check(near(h4.envelope(Chest), 1.0f, 1e-3f), "two radial contributions stack and cap");

        // "Recovers within N steps": from a full reaction the envelope is below epsilon within attack+hold+decay.
        HitReaction h5;
        h5.configure(r);
        h5.trigger(Head, Vec3{9000, 0, 0});
        int steps = 0;
        while (h5.active() && steps < 1000) { h5.step(1.0f / 60.0f); ++steps; }
        check(steps <= 44 && !h5.active(), "a full hit has run out within 0.7 s of 60 Hz steps: " + std::to_string(steps));
    }

    AVER_INFO("=== death and recovery logic ===");
    {
        // Facing: the character's up and forward axes after the body has fallen.
        const Vec3 Z{0, 0, 1};
        check(classifyFacing(Vec3{0, 0, 1}, Vec3{1, 0, 0}) == Facing::Upright, "standing is upright");
        check(classifyFacing(Vec3{0.3f, 0, 0.95f}, Vec3{0.95f, 0, -0.3f}) == Facing::Upright, "a stagger is still upright");
        check(classifyFacing(Vec3{1, 0, 0}, Vec3{0, 0, 1}) == Facing::Supine, "head toward +X, face up: supine");
        check(classifyFacing(Vec3{1, 0, 0}, Vec3{0, 0, -1}) == Facing::Prone, "face down: prone");
        check(classifyFacing(Vec3{0, 1, 0}, Vec3{1, 0, 0}) == Facing::Side, "on its side: side");
        check(classifyFacing(Vec3{0, 0, -1}, Vec3{-1, 0, 0}, Z) == Facing::Side, "upside down with the face sideways: side");
        check(classifyFacing(Vec3{0, 1, 0}, Vec3{0, 0, 0.5f}) == Facing::Supine, "mostly face up beats side");

        ResolvedPhysAnim r = resolvePhysAnim(prof, skel);
        check(chooseGetUp(r, Facing::Supine) == 0x10 && chooseGetUp(r, Facing::Prone) == 32, "the montage follows the facing");
        check(chooseGetUp(r, Facing::Side) == 32, "side falls back to prone when the profile names none");
        r.getUpSide = 77;
        check(chooseGetUp(r, Facing::Side) == 77 && chooseGetUp(r, Facing::Upright) == 0, "a named side montage wins; upright needs none");

        LifeMachine m;
        check(m.state() == Life::Alive && near(m.strengthScale(), 1.0f), "alive at full strength");
        check(!m.getUp() && !m.finishGetUp(), "cannot get up or finish while alive");
        check(m.die(0.5f) && m.state() == Life::Dying, "dying starts");
        check(!m.die(1.0f) && !m.getUp(), "cannot die twice or get up while dying");
        m.step(0.25f);
        check(near(m.strengthScale(), 0.5f), "strength ramps down linearly: " + f2s(m.strengthScale()));
        m.step(0.25f);
        check(m.state() == Life::Dead && m.strengthScale() == 0.0f, "dead at zero strength");
        check(m.getUp() && m.state() == Life::GettingUp && m.strengthScale() == 0.0f, "getting up from dead");
        check(!m.getUp() && m.finishGetUp() && m.state() == Life::Alive, "the get-up finishes into alive");
        check(m.die(0.0f) && m.state() == Life::Dead, "dying over zero seconds is dead at once");
        m.revive();
        check(m.state() == Life::Alive, "revive");
        m.die(0.5f);
        m.step(1.0f);
        m.getUp();
        check(m.die(0.5f) && m.state() == Life::Dying, "a character can die again while getting up");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
