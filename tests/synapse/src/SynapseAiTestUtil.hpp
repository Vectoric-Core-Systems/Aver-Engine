#pragma once
// Shared by the steering, crowd, hearing, cover and AI-scene tests: the check() every test in this
// directory uses, and an ASCII-map grid builder (NavTest's, copied so these executables share one).
#include "aver/core/Log.hpp"
#include "aver/formats/OcNav.hpp"
#include "aver/synapse/Nav.hpp"

#include <string>
#include <vector>

namespace aitest {

inline int g_failures = 0;

inline void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// '.' walkable, '#' blocked. Row 0 is y = 0. Cells are `cell` cm square with the origin at (0, 0).
inline aver::fmt::OcNavData gridFrom(const std::vector<std::string>& rows, aver::f32 cell = 50.0f) {
    using namespace aver;
    fmt::OcNavData nav;
    nav.cellSizeCm = cell;
    nav.heightCells = static_cast<u32>(rows.size());
    nav.widthCells = rows.empty() ? 0 : static_cast<u32>(rows[0].size());
    nav.cells.resize(static_cast<usize>(nav.widthCells) * nav.heightCells);
    for (u32 y = 0; y < nav.heightCells; ++y)
        for (u32 x = 0; x < nav.widthCells; ++x) {
            fmt::OcNavCell& c = nav.cells[static_cast<usize>(y) * nav.widthCells + x];
            c.flags = rows[y][x] == '#' ? 0 : fmt::kOcNavWalkable;
        }
    synapse::buildRegions(nav);
    return nav;
}

} // namespace aitest
