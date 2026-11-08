// EditorPanelModelsTest -- the headless logic of the Place Actors, Message Log and Undo History panels.
#include "../../../sandbox/src/PanelModels.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== editor panel models ===");
    {
        const std::vector<PlaceItem> all = {
            {"cube", "Cube", "Basic", ""}, {"sphere", "Sphere", "Basic", ""},
            {"light.point", "Point Light", "Lights", ""}, {"light.spot", "Spot Light", "Lights", ""},
            {"playerstart", "Player Start", "Gameplay", ""}};
        check(placeCategories(all) == std::vector<std::string>({"Basic", "Lights", "Gameplay"}), "categories in first-seen order");
        check(filterPlaceItems(all, "", "").size() == 5, "no filter lists everything");
        check(filterPlaceItems(all, "LIGHT", "").size() == 2, "search is case-insensitive");
        check(filterPlaceItems(all, "", "Basic").size() == 2, "category narrows");
        check(filterPlaceItems(all, "spot", "Basic").empty(), "search and category combine");
        check(filterPlaceItems(all, "gameplay", "").size() == 1, "search also matches the category name");
    }
    {
        check(logCategoryOf("[Editor] hello") == "Editor", "category is the bracketed tag");
        check(logCategoryOf("no tag").empty() && logCategoryOf("[unterminated").empty(), "no tag, no category");
        LogFilter f;
        check(logLineMatches(f, LogLevel::Info, "[A] x"), "default filter shows everything");
        f.minLevel = LogLevel::Warn;
        check(!logLineMatches(f, LogLevel::Info, "[A] x") && logLineMatches(f, LogLevel::Error, "[A] x"), "severity floor");
        f.category = "Render";
        check(!logLineMatches(f, LogLevel::Error, "[Editor] x") && logLineMatches(f, LogLevel::Error, "[Render] x"), "category filter");
        f.query = "FOO";
        check(!logLineMatches(f, LogLevel::Error, "[Render] bar") && logLineMatches(f, LogLevel::Error, "[Render] a foo b"), "text search");
    }
    {
        // 3 undoable, 2 redoable.
        check(undoHistorySteps(3, 2, 2) == 0, "clicking the current state does nothing");
        check(undoHistorySteps(3, 2, 0) == -2, "clicking an older row undoes back to it");
        check(undoHistorySteps(3, 2, -1) == -3, "the initial row undoes everything");
        check(undoHistorySteps(3, 2, 4) == 2, "clicking the last redo row redoes through it");
        check(undoHistorySteps(3, 2, 9) == 2 && undoHistorySteps(0, 0, 5) == 0, "out-of-range rows clamp");
    }
    if (g_failures == 0) AVER_INFO("=== all {} panel model checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} panel model check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
