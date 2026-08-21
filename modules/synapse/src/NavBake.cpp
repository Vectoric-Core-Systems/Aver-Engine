#include "aver/synapse/NavBake.hpp"

#include "aver/core/Log.hpp"
#include "aver/synapse/Nav.hpp"

#include <cmath>

namespace aver::synapse {

bool bakeNav(const BakeParams& p, FloorProbeFn floor, HeadroomProbeFn headroom, void* user,
             fmt::OcNavData& out, BakeStats* stats, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };

    if (!floor) return fail("bakeNav needs a floor probe");
    if (p.widthCells == 0 || p.heightCells == 0) return fail("bake area is empty");
    if (p.cellSizeCm <= 0.0f) return fail("cell size must be positive");
    if (p.sampleDepthCm <= 0.0f) return fail("sample depth must be positive");
    // 64 million cells is 512 MB of grid. The guard exists because the editor computes the extent
    // from the level's own bounds, and one placement dropped at 1e9 makes that extent absurd -- a
    // refusal names the problem, an allocation of that size just dies.
    const u64 total = static_cast<u64>(p.widthCells) * p.heightCells;
    if (total > 64ull * 1024 * 1024) return fail("bake area too large; raise the cell size");

    out = fmt::OcNavData{};
    out.cellSizeCm = p.cellSizeCm;
    out.originXCm = p.originXCm;
    out.originYCm = p.originYCm;
    out.widthCells = p.widthCells;
    out.heightCells = p.heightCells;
    out.agentRadiusCm = p.agentRadiusCm;
    out.agentHeightCm = p.agentHeightCm;
    out.maxSlopeDeg = p.maxSlopeDeg;
    out.maxStepCm = p.maxStepCm;
    out.cells.assign(static_cast<usize>(total), fmt::OcNavCell{});

    // The slope test is a comparison against the normal's UP component, not an acos per cell. Same
    // answer, no trigonometry in the inner loop: a surface at angle t from horizontal has normal.z
    // == cos(t), and cos is monotonically decreasing over [0, 90), so `normal.z >= cos(limit)` is
    // exactly `t <= limit`.
    const f32 minNormalZ = std::cos(p.maxSlopeDeg * 3.14159265358979323846f / 180.0f);

    BakeStats st;
    for (u32 y = 0; y < p.heightCells; ++y) {
        for (u32 x = 0; x < p.widthCells; ++x) {
            fmt::OcNavCell& c = out.cells[static_cast<usize>(y) * p.widthCells + x];
            const f32 wx = p.originXCm + (static_cast<f32>(x) + 0.5f) * p.cellSizeCm;
            const f32 wy = p.originYCm + (static_cast<f32>(y) + 0.5f) * p.cellSizeCm;

            ++st.cellsProbed;
            f32 z = 0.0f, nz = 0.0f;
            if (!floor(user, wx, wy, p.sampleTopZCm, p.sampleDepthCm, &z, &nz)) {
                ++st.rejectedNoFloor;
                continue;   // flags stay 0: nothing under this column
            }
            c.floorZCm = z;

            // A downward ray can legitimately report a normal pointing away from the camera on a
            // back-facing triangle; the sign is not information about the slope, so take the
            // magnitude rather than trusting the winding of whatever geometry was hit.
            if (std::fabs(nz) < minNormalZ) { ++st.rejectedSlope; continue; }

            if (headroom && !headroom(user, wx, wy, z, p.agentRadiusCm, p.agentHeightCm)) {
                ++st.rejectedHeadroom;
                continue;
            }

            c.flags = fmt::kOcNavWalkable;
            ++st.cellsWalkable;
        }
    }

    // Regions are built HERE rather than left to the caller. findPath's O(1) unreachability check
    // reads regionId, and a grid handed over without them is a grid where that check silently
    // answers "reachable" for every pair -- a correctness hole that would look like a slow bake
    // rather than a wrong one. writeOcNav refuses a grid whose walkable cells carry no region, so
    // forgetting this would also make the result unsaveable.
    st.regions = buildRegions(out);

    if (stats) *stats = st;
    AVER_INFO("[Synapse] baked {}x{} cells ({} walkable, {} regions) -- rejected {} no-floor, "
              "{} too steep, {} no headroom",
              out.widthCells, out.heightCells, st.cellsWalkable, st.regions,
              st.rejectedNoFloor, st.rejectedSlope, st.rejectedHeadroom);
    return true;
}

} // namespace aver::synapse
