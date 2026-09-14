// EditorWidgets.hpp's split-pane arithmetic: clamping to minimums, converting between a pixel width
// and a fraction across a container resize, and the preference value round-tripping through the
// REAL persistence layer (EditorPrefs.cpp) -- not a mock of it, per this item's own verification
// brief.
//
// WHAT THIS DOES NOT COVER, AND WHY. splitterHandle/drawSplitHandle (the actual mouse-driven divider)
// are `#if AVER_WITH_IMGUI`-only and need a live ImGui context this target deliberately does not
// build with -- see tests/editor/CMakeLists.txt's own precedent for every other `*Test.cpp` in this
// directory that says the same thing about its editor's drawing half. The drag gesture and whether a
// given default PROPORTION looks right are VISUAL ONLY: verify those by eye, in a narrow dock, at
// high DPI, matching EditorWidgets.hpp's own top-of-file comment. What is tested here is everything
// that decision was extracted FROM ImGui specifically so a headless target could reach it.
#include "../../../sandbox/src/EditorWidgets.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool nearlyEqual(f32 a, f32 b, f32 eps = 1.0e-4f) { return std::fabs(a - b) < eps; }

int main() {
    AVER_INFO("=== EditorWidgets split-pane arithmetic ===");

    // ---- clampSplitWidth: the ordinary cases -------------------------------------------------
    {
        // Comfortably inside the range: untouched.
        check(clampSplitWidth(300.0f, 1000.0f, 120.0f, 160.0f) == 300.0f,
              "a width already inside [minSelf, avail-minOther] is unchanged");

        // Below the floor: pulled up to minSelf.
        check(clampSplitWidth(10.0f, 1000.0f, 120.0f, 160.0f) == 120.0f,
              "a width below minSelf clamps UP to minSelf");

        // Past the ceiling: pulled down to avail-minOther.
        check(clampSplitWidth(950.0f, 1000.0f, 120.0f, 160.0f) == 840.0f,
              "a width past avail-minOther clamps DOWN to it");

        // Exactly at either bound: stays put (no off-by-one).
        check(clampSplitWidth(120.0f, 1000.0f, 120.0f, 160.0f) == 120.0f,
              "a width exactly at minSelf is left there");
        check(clampSplitWidth(840.0f, 1000.0f, 120.0f, 160.0f) == 840.0f,
              "a width exactly at the ceiling is left there");
    }

    // ---- clampSplitWidth: a handle cannot be dragged to zero and strand content --------------
    //
    // THE PROPERTY THE TASK BRIEF NAMES DIRECTLY. Whatever the caller passes -- a fresh default, or
    // the tail end of a drag that tried to push the divider off the left edge -- the pane this
    // guards never comes back at zero or negative.
    {
        check(clampSplitWidth(0.0f, 1000.0f, 120.0f, 160.0f) == 120.0f,
              "dragged to zero clamps to the floor, not zero");
        check(clampSplitWidth(-500.0f, 1000.0f, 120.0f, 160.0f) == 120.0f,
              "dragged past zero into negative clamps to the floor, not a negative width");
    }

    // ---- clampSplitWidth: a window too small to honour both minimums ------------------------
    //
    // avail - minOther can fall BELOW minSelf when the window is squeezed hard enough (minSelf=120,
    // minOther=160, avail=200 -> a "ceiling" of 40, under the floor). The upper clamp is guarded
    // specifically for this (`maxSelf > minSelf`) so it never fires here -- forcing value down to 40
    // would violate minSelf, which is the one invariant this function exists to guarantee.
    {
        // A value below the floor still gets floored -- minSelf wins outright, even though avail
        // cannot actually fit both minimums at once. This is the property the task brief names:
        // whatever the caller passes, the pane this guards never comes back below its floor.
        check(clampSplitWidth(10.0f, 200.0f, 120.0f, 160.0f) == 120.0f,
              "a value below the floor is still floored when avail cannot fit both minimums");
        // A value already at or above the floor is left untouched rather than force-fit into an
        // impossible ceiling -- the caller's own fold-to-single-column logic (e.g. ActorEditor's
        // `threeColumns` gate before it ever calls the splitter) is what avoids this squeeze
        // upstream; clampSplitWidth's job here is only to never invent a WORSE number.
        check(clampSplitWidth(500.0f, 200.0f, 120.0f, 160.0f) == 500.0f,
              "a value already above the floor is left alone when the ceiling would invert it");
    }

    // ---- splitFractionOf / splitWidthOf: the round trip, and an actual resize -----------------
    {
        check(splitFractionOf(0.0f, 0.0f) == 0.0f, "a non-positive avail cannot divide by zero");
        check(splitFractionOf(100.0f, -5.0f) == 0.0f, "...nor can a negative one");

        const f32 fraction = splitFractionOf(300.0f, 1000.0f);
        check(nearlyEqual(fraction, 0.3f), "300px of 1000 is the fraction 0.3");
        check(nearlyEqual(splitWidthOf(fraction, 1000.0f), 300.0f),
              "converting back at the SAME avail reproduces the original width");

        // THE WHOLE REASON A FRACTION IS WHAT GETS PERSISTED, NOT A PIXEL WIDTH: converting back at
        // a DIFFERENT avail scales the width proportionally, tracking the window instead of staying
        // visually put in pixels -- see SplitPane's own header comment. A container that WIDENS from
        // 1000 to 1600 with the pane still at 30% should put the divider at 480, not still at 300.
        check(nearlyEqual(splitWidthOf(fraction, 1600.0f), 480.0f),
              "a fraction captured at one avail gives the PROPORTIONAL width at a resized one");
        check(nearlyEqual(splitWidthOf(fraction, 500.0f), 150.0f),
              "...shrinking the container scales it back down the same way");
    }

    // ---- the ACTUAL persistence round trip (EditorPrefs), not a mock of it -------------------
    //
    // Same EditorPrefsTest precedent: exercise the real on-disk store, and restore the developer's
    // own file afterwards. loadSplitFraction/storeSplitFraction are thin wrappers over prefFloat/
    // setPrefFloat/flushEditorPrefs specifically so THIS is the call a headless test can make --
    // see EditorWidgets.hpp's own top comment.
    const std::string prefsPath = [] {
        (void)prefFloat("aver.test.splitwidgets.probe", 0.0f);
        return editorPrefsPath();
    }();

    if (prefsPath.empty()) {
        AVER_WARN("=== SKIPPED the persistence section: no user data directory on this machine ===");
    } else {
        AVER_INFO("  store: {}", prefsPath);
        std::string original;
        const bool hadFile = readFileText(prefsPath, original);

        constexpr const char* kKey = "aver.test.splitwidgets.fraction";

        // An absent key takes the caller's default -- the "first run, or Reset Tab Layout since the
        // last save" case every editor's draw() hits on its very first frame.
        check(nearlyEqual(loadSplitFraction(kKey, 0.42f), 0.42f),
              "an absent split preference returns the caller's default fraction");

        // A stored value round-trips EXACTLY through the real store.
        storeSplitFraction(kKey, 0.275f);
        check(nearlyEqual(loadSplitFraction(kKey, 0.0f), 0.275f),
              "a stored fraction round-trips through prefFloat/setPrefFloat exactly");

        std::string text;
        check(readFileText(prefsPath, text) && text.find("aver.test.splitwidgets.fraction=") != std::string::npos,
              "storeSplitFraction actually flushed to the file, not just to memory");

        // ---- SplitPane: seed-once, then track without re-reading the store --------------------
        {
            SplitPane pane;
            check(pane.fraction < 0.0f, "a fresh SplitPane starts with the \"not yet seeded\" sentinel");

            const f32 w1 = splitPaneWidth(pane, kKey, 0.5f, 1000.0f, 100.0f, 100.0f);
            check(nearlyEqual(pane.fraction, 0.275f),
                  "splitPaneWidth seeds fraction from the STORED preference, not the caller's default");
            check(nearlyEqual(w1, 275.0f), "...and returns the matching pixel width for this avail");

            // A second call at a DIFFERENT avail must NOT re-read the store (the pane already has a
            // fraction) -- it should simply re-derive the width at the new avail, proportionally.
            storeSplitFraction(kKey, 0.9f);   // if splitPaneWidth re-seeded, this would leak in
            const f32 w2 = splitPaneWidth(pane, kKey, 0.5f, 2000.0f, 100.0f, 100.0f);
            check(nearlyEqual(w2, 550.0f),
                  "a live SplitPane tracks a container resize by its OWN fraction, not a fresh pref read");
            storeSplitFraction(kKey, 0.275f);  // leave the store as the rest of this block expects

            // Minimums still apply even to an already-seeded pane.
            SplitPane tiny;
            tiny.fraction = 0.01f;
            const f32 clamped = splitPaneWidth(tiny, "aver.test.splitwidgets.unused", 0.5f, 1000.0f,
                                                150.0f, 100.0f);
            check(clamped == 150.0f, "splitPaneWidth clamps an already-seeded pane's width too");
        }

        // ---- resetSplitPane: "Reset Tab Layout"'s own call, end to end -------------------------
        {
            SplitPane pane;
            pane.fraction = 0.9f;   // as if a drag had left it somewhere far from the default
            resetSplitPane(pane, kKey, 0.5f);
            check(nearlyEqual(pane.fraction, 0.5f), "resetSplitPane snaps the in-memory fraction back");
            check(nearlyEqual(loadSplitFraction(kKey, -1.0f), 0.5f),
                  "...and persists the default IMMEDIATELY, not on the next draw's seed check");
        }

        // ---- restore the developer's own file --------------------------------------------------
        if (hadFile) writeFileText(prefsPath, original);
        else         writeFileText(prefsPath, "");
    }

    if (g_failures == 0) AVER_INFO("=== all EditorWidgets split-pane checks passed ===");
    else                 AVER_ERROR("=== {} EditorWidgets split-pane check(s) FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
