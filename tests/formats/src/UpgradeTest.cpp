// Aver.Upgrade: version parsing, the series rule, and the migration chain.
//
// The chain is the part worth testing hardest. A planner that quietly returns an empty plan for a
// project it cannot actually migrate is the worst failure available here: the editor reports
// success, stamps a new version into the manifest, and the project is now lying about what it is.
#include "aver/core/Log.hpp"
#include "aver/upgrade/Upgrade.hpp"

#include <string>
#include <vector>

using namespace aver;

namespace {
int gChecks = 0, gFailed = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (ok) { AVER_INFO("  ok    {}", what); return; }
    ++gFailed;
    AVER_ERROR("  FAIL  {}", what);
}

upgrade::Version V(int ma, int mi, int pa = 0) { return upgrade::Version{ma, mi, pa}; }
} // namespace

int main() {
    AVER_INFO("=== version parsing ===");
    {
        upgrade::Version v;
        check(upgrade::parseVersion("0.2.0", v) && v.major == 0 && v.minor == 2 && v.patch == 0,
              "0.2.0 parses");
        check(upgrade::parseVersion("1.4", v) && v.major == 1 && v.minor == 4 && v.patch == 0,
              "a two-part version parses, patch 0");
        check(upgrade::parseVersion("0.2.0-rc1", v) && v.major == 0 && v.minor == 2 && v.patch == 0,
              "a prerelease suffix is ignored, not rejected");
        check(!upgrade::parseVersion("", v), "EMPTY IS NOT ZERO: no recorded version must not parse "
                                             "as 0.0.0, or every project without one looks ancient");
        check(!upgrade::parseVersion("banana", v), "junk fails");
        check(!upgrade::parseVersion("0.x.1", v), "junk in the middle fails");
    }

    AVER_INFO("=== the series rule: patches do not gate ===");
    {
        check(upgrade::sameSeries(V(0, 1, 0), V(0, 1, 7)), "0.1.0 and 0.1.7 are one series");
        check(!upgrade::sameSeries(V(0, 1, 9), V(0, 2, 0)), "0.1.9 and 0.2.0 are not");
        check(upgrade::olderSeries(V(0, 1, 9), V(0, 2, 0)), "0.1.9 is an older series than 0.2.0");
        check(!upgrade::olderSeries(V(0, 1, 7), V(0, 1, 0)),
              "a HIGHER patch is not a newer series -- this is the comparison that must not use patch");
        check(upgrade::olderSeries(V(0, 9, 0), V(1, 0, 0)), "major wins over minor");
    }

    AVER_INFO("=== planning ===");
    {
        const upgrade::Version now = upgrade::engineVersion();
        std::vector<const upgrade::Step*> plan;
        std::string why;

        check(upgrade::planUpgrade(now, plan, &why) && plan.empty(),
              "a project of this series plans an EMPTY chain and succeeds");

        upgrade::Version patchOff = now;
        patchOff.patch += 3;
        check(upgrade::planUpgrade(patchOff, plan, &why) && plan.empty(),
              "a different PATCH of this series still plans nothing");

        upgrade::Version future = now;
        future.minor += 1;
        check(!upgrade::planUpgrade(future, plan, &why),
              "a project from a NEWER series fails rather than inventing a downgrade");
        check(!why.empty(), "...and says why");

        // The chain this engine actually ships. At 0.2 that is one step from 0.1; as the engine
        // moves on this stays true because the assertion is about REACHING the current series, not
        // about a fixed length.
        if (upgrade::olderSeries(V(0, 1, 0), now)) {
            why.clear();   // or a PASS below reports the previous check's failure text
            const bool ok = upgrade::planUpgrade(V(0, 1, 2), plan, &why);
            check(ok, "a 0.1.2 project plans a chain" + (why.empty() ? std::string() : ": " + why));
            check(ok && !plan.empty(), "...which is not empty");
            if (ok && !plan.empty()) {
                check(upgrade::sameSeries(plan.front()->from, V(0, 1, 0)),
                      "...starting at the project's own series, not at the oldest step");
                check(upgrade::sameSeries(plan.back()->to, now),
                      "...and ending at this engine's series");
                bool contiguous = true;
                for (usize i = 1; i < plan.size(); ++i)
                    if (!upgrade::sameSeries(plan[i - 1]->to, plan[i]->from)) contiguous = false;
                check(contiguous, "...with every link joining the previous step's end");
            }
        }
    }

    AVER_INFO("=== steps are declared in order and join up ===");
    {
        const std::vector<upgrade::Step>& all = upgrade::steps();
        check(!all.empty(), "the engine ships at least one step");
        bool ordered = true, forward = true;
        for (usize i = 0; i < all.size(); ++i) {
            if (!upgrade::olderSeries(all[i].from, all[i].to)) forward = false;
            if (i && !upgrade::olderSeries(all[i - 1].from, all[i].from)) ordered = false;
        }
        check(forward, "every step moves forward");
        check(ordered, "steps are declared oldest first");
        for (const upgrade::Step& s : all)
            check(s.summary && *s.summary && s.apply, "every step has a summary and a function");
    }

    if (gFailed) { AVER_ERROR("=== {} of {} checks FAILED ===", gFailed, gChecks); return 1; }
    AVER_INFO("=== all {} upgrade checks passed ===", gChecks);
    return 0;
}
