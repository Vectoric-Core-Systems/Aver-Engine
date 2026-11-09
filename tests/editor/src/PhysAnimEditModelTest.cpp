// PhysAnimEditModelTest -- the physical animation profile tab's rules without a window (sandbox/src/PhysAnimEditModel.hpp, header-only): region
// edits, bone assignment (one owner per entry), value clamping that keeps the file valid, the skeleton as a tree with each bone's region, and the
// offline hit preview against the real envelope.
#include "PhysAnimEditModel.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
namespace pa = aver::editor::physanim;
using aver::ragdoll::PhysAnimProfile;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Root > Pelvis > Spine > Chest > (Neck > Head, Arm > Hand), Pelvis > Thigh
static fmt::OcSkeleton body() {
    struct Def { const char* name; int parent; };
    const Def defs[] = {{"Root", -1}, {"Pelvis", 0}, {"Spine", 1}, {"Chest", 2}, {"Neck", 3}, {"Head", 4}, {"Arm", 3}, {"Hand", 6}, {"Thigh", 1}};
    fmt::OcSkeleton s;
    for (const Def& d : defs) { fmt::OcBone b; b.name = d.name; b.parent = d.parent; s.bones.push_back(b); }
    return s;
}

static void testRegions() {
    AVER_INFO("=== regions ===");
    PhysAnimProfile p;
    p.defaults.strength = 0.3f;
    check(pa::freshRegionName(p) == "region", "the first free name");
    const int a = pa::addRegion(p, "arm");
    check(a == 0 && p.regions[0].drive.strength == 0.3f, "a region starts from the profile defaults");
    check(pa::addRegion(p, "arm") < 0 && pa::addRegion(p, "bad name") < 0 && pa::addRegion(p, "") < 0 && pa::addRegion(p, "x#y") < 0, "duplicate, spaced, empty or '#' names are refused");
    pa::addRegion(p, "head");
    check(pa::freshRegionName(p, "arm") == "arm2", "freshRegionName counts up");
    check(pa::renameRegion(p, 1, "skull") && p.regions[1].name == "skull" && !pa::renameRegion(p, 1, "arm") && !pa::renameRegion(p, 5, "z"), "rename, and its refusals");
    check(pa::moveRegion(p, 0, 1) == 1 && p.regions[1].name == "arm" && p.regions[0].name == "skull", "moving a region changes precedence");
    check(pa::moveRegion(p, 1, 1) < 0 && pa::moveRegion(p, 0, -1) < 0, "moves off either end are refused");
    check(pa::removeRegion(p, 0) && p.regions.size() == 1 && !pa::removeRegion(p, 3), "remove");
}

static void testAssign() {
    AVER_INFO("=== bone assignment ===");
    PhysAnimProfile p;
    pa::addRegion(p, "a"); pa::addRegion(p, "b");
    check(pa::assignBone(p, 0, "Arm", true) && p.regions[0].subtrees.size() == 1, "assign a subtree");
    check(!pa::assignBone(p, 0, "Arm", true), "assigning the same entry again changes nothing");
    check(pa::assignBone(p, 1, "Arm", true) && p.regions[0].subtrees.empty() && p.regions[1].subtrees.size() == 1, "the same entry moves, it is never in two regions");
    check(pa::assignBone(p, 0, "Arm", false) && p.regions[0].bones.size() == 1 && p.regions[1].subtrees.size() == 1, "an exact bone is a different entry from a subtree");
    check(!pa::assignBone(p, 9, "Arm", true) && !pa::assignBone(p, 0, "", true), "a bad region or empty bone is refused");
    check(pa::unassignBone(p, 1, "Arm") && p.regions[1].subtrees.empty() && !pa::unassignBone(p, 1, "Arm"), "unassign");
}

static void testClamp() {
    AVER_INFO("=== clamping ===");
    PhysAnimProfile p;
    pa::addRegion(p, "r");
    p.regions[0].drive.strength = 2.0f; p.regions[0].drive.blend = -1.0f; p.regions[0].drive.decay = 0.0f; p.regions[0].drive.torque = -3.0f;
    p.falloffPerHop = 4.0f; p.maxHops = -2; p.epsilon = 0.0f; p.fullImpulse = -5.0f;
    pa::clampProfile(p);
    std::vector<std::string> problems;
    const bool ok = ragdoll::validatePhysAnimProfile(p, body(), &problems);
    check(ok || (problems.size() == 1 && problems[0].find("names no bone") != std::string::npos), "a clamped profile passes the file validation (bar the empty region)");
    check(p.regions[0].drive.strength == 1.0f && p.regions[0].drive.blend == 0.0f && p.regions[0].drive.torque == 0.0f && p.regions[0].drive.decay > 0.0f, "out-of-range values land on the range");
    check(p.falloffPerHop == 1.0f && p.maxHops == 0 && p.epsilon > 0.0f && p.fullImpulse >= 1.0f, "reach values too");
}

static void testRows() {
    AVER_INFO("=== skeleton rows ===");
    const fmt::OcSkeleton s = body();
    PhysAnimProfile p;
    pa::addRegion(p, "arm");
    pa::addRegion(p, "head");
    pa::assignBone(p, 0, "Arm", true);
    pa::assignBone(p, 1, "Head", false);
    const auto rows = pa::boneRows(p, s);
    check(rows.size() == s.bones.size(), "every bone is listed once");
    check(rows[0].bone == 0 && rows[0].depth == 0 && rows[1].bone == 1 && rows[1].depth == 1, "parents come before children, depth counts links");
    int armRow = -1, handRow = -1, headRow = -1, thighRow = -1;
    for (usize i = 0; i < rows.size(); ++i) {
        if (rows[i].bone == 6) armRow = static_cast<int>(i);
        if (rows[i].bone == 7) handRow = static_cast<int>(i);
        if (rows[i].bone == 5) headRow = static_cast<int>(i);
        if (rows[i].bone == 8) thighRow = static_cast<int>(i);
    }
    check(rows[static_cast<usize>(armRow)].region == 0 && rows[static_cast<usize>(handRow)].region == 0, "a subtree region covers the bone and what hangs below it");
    check(rows[static_cast<usize>(headRow)].region == 1 && rows[static_cast<usize>(thighRow)].region == -1, "an exact bone, and a bone in no region reads -1");
    check(handRow == armRow + 1, "a bone's children follow it (depth-first)");
    check(pa::boneIndexByName(s, "Chest") == 3 && pa::boneIndexByName(s, "Nope") == -1, "boneIndexByName");

    fmt::OcSkeleton loop = s;
    loop.bones[0].parent = 8;   // a cycle: the tab must not hang
    check(pa::boneRows(p, loop).size() <= loop.bones.size(), "a cyclic skeleton terminates");
}

static void testPreview() {
    AVER_INFO("=== hit preview ===");
    const fmt::OcSkeleton s = body();
    PhysAnimProfile p;
    p.fullImpulse = 1000.0f; p.falloffPerHop = 0.5f; p.maxHops = 2;
    pa::addRegion(p, "arm");
    pa::assignBone(p, 0, "Arm", true);
    p.regions[0].drive.attack = 0.1f; p.regions[0].drive.hold = 0.2f; p.regions[0].drive.decay = 0.3f; p.regions[0].drive.blend = 1.0f;

    const pa::HitPreview h = pa::previewHit(p, s, 6, 1000.0f);
    check(h.valid && h.peak.size() == s.bones.size() && !h.curve.empty(), "a preview of a valid bone");
    check(h.peak[6] > 0.99f, "a full-size hit reaches full envelope on the hit bone");
    check(h.peak[7] > 0.4f && h.peak[7] < 0.6f, "one hop away keeps the falloff share (0.5)");
    check(h.peak[8] == 0.0f, "a bone past the hop limit does not react");
    check(h.bonesReached >= 3, "bonesReached counts the bones above epsilon");
    check(h.settleSeconds > 0.4f && h.settleSeconds < 1.0f, "it settles after about attack + hold + decay");

    const pa::HitPreview small = pa::previewHit(p, s, 6, 250.0f);
    check(small.peak[6] > 0.2f && small.peak[6] < 0.3f, "a quarter-size impulse is a quarter-size reaction");
    check(!pa::previewHit(p, s, -1, 1000.0f).valid && !pa::previewHit(p, s, 99, 1000.0f).valid, "an unknown bone gives no preview");
    const pa::HitPreview again = pa::previewHit(p, s, 6, 1000.0f);
    check(again.curve == h.curve && again.peak == h.peak, "the preview is deterministic");
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("Physical animation profile editing model");
    testRegions();
    testAssign();
    testClamp();
    testRows();
    testPreview();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
