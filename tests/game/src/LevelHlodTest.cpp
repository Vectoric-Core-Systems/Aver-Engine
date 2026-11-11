// LevelHlodTest -- game::LevelHlod's rules: which ladder level a cell shows at a distance (and that it does not flip on a threshold),
// the error each level allows, the settings a level's STREAM record gives, and that it stays inert with no project. No GPU.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/game/LevelHlod.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

using namespace aver;
using game::LevelHlod;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("=== LevelHlodTest ===");
    const f32 load = 25000.0f;
    check(LevelHlod::levelFor(0.0, load, -1) == 0 && LevelHlod::levelFor(49000.0, load, -1) == 0, "inside twice the load distance: level 0");
    check(LevelHlod::levelFor(51000.0, load, -1) == 1 && LevelHlod::levelFor(101000.0, load, -1) == 2 &&
          LevelHlod::levelFor(201000.0, load, -1) == 3, "each doubling of distance is one level coarser");
    check(LevelHlod::levelFor(1.0e7, load, -1) == LevelHlod::kLevels - 1, "never past the last level");
    check(LevelHlod::levelFor(52000.0, load, 0) == 0, "hysteresis: 4% past the threshold keeps the finer level");
    check(LevelHlod::levelFor(56000.0, load, 0) == 1, "...12% past it moves to the coarser one");
    check(LevelHlod::levelFor(48000.0, load, 1) == 1, "hysteresis: 4% inside the threshold keeps the coarser level");
    check(LevelHlod::levelFor(44000.0, load, 1) == 0, "...12% inside it moves back to the finer one");

    LevelHlod::Settings s;
    const f32 e1 = LevelHlod::errorCmAt(25000.0f, s), e2 = LevelHlod::errorCmAt(50000.0f, s);
    check(std::fabs(e2 - 2.0f * e1) < 1e-3f, "the error bound grows with distance (doubling)");
    check(e1 > 13.0f && e1 < 13.5f, "one pixel at 250 m in a 2160-high, 60-degree view is ~13.4 cm (" + std::to_string(e1) + ")");

    fmt::OcStreamSettings st;
    st.cellCm = 6400; st.loadCm = 25000; st.evictCm = 30000;
    LevelHlod::Settings a = LevelHlod::settingsFor(st);
    check(a.hlodCm == 100000.0f && a.hlodEvictCm == 106400.0f, "no hlod= on the level: four load distances, hidden a cell further out");
    st.hlodCm = 60000.0f;
    check(LevelHlod::settingsFor(st).hlodCm == 60000.0f, "hlod= is used as given");
    st.hlodCm = -1.0f;
    check(LevelHlod::settingsFor(st).hlodCm == 0.0f, "a negative hlod= turns it off");

    LevelHlod h;
    check(!h.active() && h.cellCount() == 0 && !h.owns(scene::Entity{}), "a fresh LevelHlod owns nothing");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
