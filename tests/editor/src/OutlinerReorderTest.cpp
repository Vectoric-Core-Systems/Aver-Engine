// OutlinerReorder.hpp -- drop-zone thirds and where moved rows land among their siblings. Pure, header-only.
#include "../../../sandbox/src/OutlinerReorder.hpp"

#include "aver/core/Log.hpp"

#include <string>
#include <vector>

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
    AVER_INFO("=== OutlinerReorder ===");
    check(outlinerDropZone(100.0f, 100.0f, 120.0f) == OutlinerDropZone::Before, "the top edge of a row is Before");
    check(outlinerDropZone(104.0f, 100.0f, 120.0f) == OutlinerDropZone::Before, "the top quarter is Before");
    check(outlinerDropZone(110.0f, 100.0f, 120.0f) == OutlinerDropZone::Into, "the middle is Into");
    check(outlinerDropZone(116.0f, 100.0f, 120.0f) == OutlinerDropZone::After, "the bottom quarter is After");
    check(outlinerDropZone(110.0f, 100.0f, 100.0f) == OutlinerDropZone::Into, "a zero-height row is Into");

    using V = std::vector<int>;
    {
        V order{1, 2, 3, 4, 5};
        check(placeBeside(order, V{4}, 2, false) && order == (V{1, 4, 2, 3, 5}), "one row moves before an earlier sibling");
    }
    {
        V order{1, 2, 3, 4, 5};
        check(placeBeside(order, V{2}, 4, true) && order == (V{1, 3, 4, 2, 5}), "one row moves after a later sibling");
    }
    {
        V order{1, 2, 3, 4, 5};
        check(placeBeside(order, V{5, 1}, 3, true) && order == (V{2, 3, 5, 1, 4}), "several rows keep their given sequence at the anchor");
    }
    {
        V order{1, 2, 3};
        check(placeBeside(order, V{9}, 2, false) && order == (V{1, 9, 2, 3}), "a row from another parent is inserted too");
    }
    {
        V order{1, 2, 3};
        check(!placeBeside(order, V{2}, 2, false) && order == (V{1, 2, 3}), "the anchor among the moved rows is refused");
        check(!placeBeside(order, V{1}, 7, false) && order == (V{1, 2, 3}), "an anchor that is not a sibling is refused");
    }
    {
        V order{1, 2, 3};
        check(placeBeside(order, V{1}, 2, false) && order == (V{1, 2, 3}), "dropping a row where it already is leaves the order alone");
    }

    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
