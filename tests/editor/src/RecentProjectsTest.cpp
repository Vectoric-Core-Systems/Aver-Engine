// RecentProjectsTest -- the start screen's list model, and the three defects it exists to prevent.
//
// The browser kept two parallel containers with an implicit index correspondence nothing enforced:
// `recents_` (paths) and `cards_` (rows). The selection indexed one and "Open Selected" indexed the
// other. Three separate bugs came out of that single cause, and each has a section below:
//
//   1. A selection past the end of the recent list could not be opened AT ALL. Half the visible
//      list -- every project found by the folder scan rather than remembered -- greyed the only
//      button labelled "open the selected thing", with no reason shown.
//   2. A FAILED open dropped the path from `recents_` without rebuilding `cards_`, so from that
//      moment every later index addressed a DIFFERENT project than the row the user had clicked.
//   3. An upgrade rebuilt the cards while the selection kept its old integer, so it silently came
//      to mean whichever project now sat in that slot.
//
// None of these are arithmetic slips at a call site; they are one missing invariant. The tests
// below assert the invariant -- selection is identity, not position -- rather than the three
// symptoms, which is why they would also have caught the fourth instance nobody has hit yet.
//
// NO ImGui and NO filesystem, by construction. `parse()` takes its own keep-predicate and
// `rebuild()` takes the results of the caller's directory walk, so this runs with AVER_WITH_IMGUI
// undefined and touches no disk -- the same shape InputOwnershipTest relies on.
#include "RecentProjects.hpp"

#include "aver/core/Log.hpp"

#include <string>
#include <vector>

using namespace aver;
using aver::editor::ProjectCard;
using aver::editor::RecentProjects;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static ProjectCard card(const std::string& path, const std::string& name,
                        const std::string& version = {}) {
    ProjectCard c;
    c.path = path;
    c.name = name;
    c.version = version;
    return c;
}

// Everything is a project; nothing has gone from disk.
static bool keepAll(const std::string&) { return true; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("the start screen's recent-project list");

    AVER_INFO("=== parsing recent.txt ===");
    {
        RecentProjects r;
        r.parse("a.ocproject\r\nb.ocproject  \n\nc.ocproject\n", keepAll);
        check(r.recents().size() == 3, "blank lines are skipped, got " + std::to_string(r.recents().size()));
        check(r.recents()[1] == "b.ocproject", "trailing spaces and CR are trimmed");
        check(r.serialise() == "a.ocproject\nb.ocproject\nc.ocproject\n", "and it round-trips");

        // The keep-predicate is what drops projects whose file has gone.
        RecentProjects pruned;
        pruned.parse("gone.ocproject\nhere.ocproject\n",
                     [](const std::string& p) { return p != "gone.ocproject"; });
        check(pruned.recents().size() == 1 && pruned.recents()[0] == "here.ocproject",
              "a path the predicate rejects is dropped on read");

        // THE CAP IS A CAP. Eleven in, ten out, oldest lost.
        std::string many;
        for (int i = 0; i < 15; ++i) many += "p" + std::to_string(i) + ".ocproject\n";
        RecentProjects capped;
        capped.parse(many, keepAll);
        check(capped.recents().size() == RecentProjects::kMax,
              "the list is capped at " + std::to_string(RecentProjects::kMax) + ", got " +
                  std::to_string(capped.recents().size()));
    }

    AVER_INFO("=== remember() puts a project at the head and never duplicates it ===");
    {
        RecentProjects r;
        r.parse("a\nb\nc\n", keepAll);
        r.remember("c");
        check(r.recents().size() == 3, "re-remembering an existing entry does not grow the list");
        check(r.recents()[0] == "c" && r.recents()[1] == "a" && r.recents()[2] == "b",
              "and moves it to the head, keeping the rest in order");

        for (int i = 0; i < 12; ++i) r.remember("n" + std::to_string(i));
        check(r.recents().size() == RecentProjects::kMax, "remember() also respects the cap");
        check(r.recents()[0] == "n11", "with the newest at the head");
    }

    // ---- BUG 1: a folder-scanned project could not be opened by the button at all --------------
    AVER_INFO("=== every visible card can be opened, not just the remembered ones ===");
    {
        RecentProjects r;
        r.parse("recent.ocproject\n", keepAll);
        r.rebuild({card("recent.ocproject", "Recent"), card("found.ocproject", "Found")});

        check(r.cards().size() == 2, "both the recent and the scanned project have a card");
        check(r.cards()[0].path == "recent.ocproject" && r.cards()[0].recent,
              "the recent one sorts first and is tagged recent");
        check(!r.cards()[1].recent, "and the scanned one is not");

        // Index 1 is PAST the end of the recent list, which is exactly what used to be unopenable.
        r.select(1);
        check(r.selectedPath() == "found.ocproject",
              "selecting a card beyond the recent list still resolves to a project, got '" +
                  r.selectedPath() + "' -- this is the case the Open button used to refuse");
    }

    // ---- BUG 2: a failed open desynchronised the two containers -------------------------------
    AVER_INFO("=== forgetting a project cannot make a later selection address the wrong one ===");
    {
        RecentProjects r;
        r.parse("a\nb\nc\n", keepAll);
        r.rebuild({card("a", "A"), card("b", "B"), card("c", "C")});
        r.select(2);
        check(r.selectedPath() == "c", "card 2 is C");

        // The failure path: an open fails, so the project is dropped from the recent list. This is
        // the exact sequence that used to leave the cards stale and every later index off by one.
        r.forget("a");
        check(r.dirty(), "forget() marks the cards stale -- the missing half of the old bug");
        r.rebuild({card("b", "B"), card("c", "C")});

        check(r.cards().size() == 2, "the dropped project is gone from the cards too");
        check(r.selectedPath() == "c",
              "and the selection still means C, not whatever moved into slot 2, got '" +
                  r.selectedPath() + "'");
        check(r.selection() == 1, "its index moved with it, from 2 to " + std::to_string(r.selection()));
    }

    // ---- BUG 3: a rebuild silently retargeted the selection ------------------------------------
    AVER_INFO("=== a rebuild moves the selection with the project, or clears it ===");
    {
        RecentProjects r;
        r.parse("x\ny\n", keepAll);
        r.rebuild({card("x", "X"), card("y", "Y")});
        r.select(0);
        check(r.selectedPath() == "x", "X is selected");

        // Reordering is what open() does on every success. The selection must follow the project.
        r.remember("y");
        r.rebuild({card("x", "X"), card("y", "Y")});
        check(r.cards()[0].path == "y", "opening Y moved it to the head");
        check(r.selectedPath() == "x",
              "and the selection still means X even though it is now row 1, got '" +
                  r.selectedPath() + "'");

        // A selection whose project has left the list must CLEAR, not slide onto a neighbour --
        // sliding is how a click on one project ends up opening another.
        r.select(1);
        check(r.selectedPath() == "x", "X selected again, at row 1");
        r.forget("x");
        r.rebuild({card("y", "Y")});
        check(r.selection() == -1,
              "with X gone the selection clears rather than landing on Y, got index " +
                  std::to_string(r.selection()));
        check(r.selectedPath().empty(), "and resolves to no project at all");
    }

    AVER_INFO("=== remember() marks the cards stale, which is what makes re-entry safe ===");
    {
        // open() reorders the recents on every success. While the browser could only ever be shown
        // once, leaving the cards stale was invisible. It stops being invisible the moment there is
        // a way back to the start screen, and then it is bug 2 again on the ordinary path.
        RecentProjects r;
        r.parse("a\nb\n", keepAll);
        r.rebuild({card("a", "A"), card("b", "B")});
        check(!r.dirty(), "a fresh rebuild leaves the cards clean");
        r.remember("b");
        check(r.dirty(), "opening a project marks them stale again");
    }

    AVER_INFO("=== what it refuses ===");
    {
        RecentProjects r;
        r.parse("a\n", keepAll);
        r.rebuild({card("a", "A")});
        r.select(7);
        check(r.selection() == -1 && r.selectedPath().empty(), "an out-of-range selection is rejected");
        r.select(-3);
        check(r.selection() == -1, "and so is a negative one");
        r.remember("");
        check(r.recents().size() == 1, "an empty path is not remembered");

        // A project on the recent list but outside the scanned folder still gets a row: recent is
        // per-machine state in AppData, and the project it names can live anywhere.
        RecentProjects outside;
        outside.parse("elsewhere.ocproject\n", keepAll);
        outside.rebuild({card("scanned.ocproject", "Scanned")});
        check(outside.cards().size() == 2, "a recent outside the scan still gets a card");
        check(outside.cards()[0].path == "elsewhere.ocproject" && outside.cards()[0].name.empty(),
              "with its details unknown rather than invented");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return g_failures;
}
