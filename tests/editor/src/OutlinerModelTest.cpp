// The Outliner's type filter and folder paths. ImGui- and scene-free.
#include "../../../sandbox/src/OutlinerModel.hpp"

#include "aver/core/Log.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    // type filter
    u32 m = kOutlinerAllTypes;
    check(!outlinerTypeFiltering(m), "all types on is not filtering");
    check(outlinerTypeShown(m, OutlinerType::Light) && outlinerTypeShown(m, OutlinerType::Other), "every type shows by default");
    m = outlinerTypeToggled(m, OutlinerType::Light);
    check(outlinerTypeFiltering(m) && !outlinerTypeShown(m, OutlinerType::Light), "toggling a type off hides just it");
    check(outlinerTypeShown(m, OutlinerType::Mesh), "...and leaves the rest");
    m = outlinerTypeToggled(m, OutlinerType::Light);
    check(m == kOutlinerAllTypes, "toggling it back restores all");
    check(std::string(outlinerTypeName(OutlinerType::Mesh)) == "Meshes", "type names");

    // folder paths
    check(folderParent("A/B/C") == "A/B" && folderParent("A").empty(), "folderParent");
    check(folderLeaf("A/B/C") == "C" && folderLeaf("A") == "A", "folderLeaf");
    check(folderIsWithin("A/B", "A") && folderIsWithin("A", "A") && folderIsWithin("A", ""), "within: below, equal, root");
    check(!folderIsWithin("AB", "A") && !folderIsWithin("A", "A/B"), "within: a name prefix is not a folder prefix");
    check(folderNameValid("Lights") && !folderNameValid("") && !folderNameValid("a/b") && !folderNameValid(" x"), "valid names");
    check(folderRebased("A/B/C", "A/B", "X") == "X/C", "a rename carries the subfolders");
    check(folderRebased("Z", "A", "X") == "Z", "a rename leaves unrelated paths");

    std::vector<std::string> f;
    folderAdd(f, "A/B");
    folderAdd(f, "A");
    folderAdd(f, "C");
    check((f == std::vector<std::string>{"A", "A/B", "C"}), "folderAdd keeps ancestors, sorted, no duplicates");
    check(folderExists(f, "A/B") && !folderExists(f, "A/Z"), "folderExists");
    check(folderUniquePath(f, "", "A") == "A 2", "unique path skips a taken name");
    check(folderUniquePath(f, "A") == "A/New Folder", "unique path under a parent");
    folderAdd(f, "A/New Folder");
    check(folderUniquePath(f, "A") == "A/New Folder 2", "...and numbers the next");

    if (g_failures == 0) AVER_INFO("=== all OutlinerModel checks passed ===");
    else                 AVER_ERROR("=== {} OutlinerModel check(s) FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
