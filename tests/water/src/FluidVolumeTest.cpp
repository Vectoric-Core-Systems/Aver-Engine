// FluidVolume.hpp's seed-shell geometry: generateFluidSeedShell's pure box-lattice arithmetic, and
// the placement/normal bookkeeping FluidVolume::generateSeedShell layers on top of it. CPU-only, no
// RHI device, no window, no physics solver -- this exercises exactly the "(a)" half of
// FluidVolume.hpp's own two-halves comment, never updateFromSimulation's solver-fed half, because
// there is no solver here to hand it simulated vertices from. Links Aver.Water because FluidVolume.cpp
// is a real .cpp (unlike header-only GerstnerWave.hpp), and through it Aver.RHI -- the same link shape
// UnderwaterFogTest.cpp uses for the same reason: a struct and some plain arithmetic, never a device.
//
// What is actually worth checking here is whether the solver can hold pressure in this shell at all,
// which is a question about the mesh's TOPOLOGY (closed, consistently wound, no torn seams) at least
// as much as about where its vertices land. FluidVolume.hpp's own header comment is explicit that the
// per-face winding table has been worked out algebraically but "not been checked by running anything",
// and that a wrong entry in it would still pass a closedness check alone -- so closedness and winding
// are checked here as two separate properties over the same edge set, not folded into one.
#include "aver/water/FluidVolume.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::water;

static int g_checks = 0;
static int g_failures = 0;

// Matches GerstnerWaveTest.cpp's exact idiom: call the condition first, assert second, so a
// message built from values already known ahead of time is never at the mercy of `check`'s own
// unspecified argument-evaluation order.
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

// A FluidVolumeDesc with every field set from arguments, because the struct's members are C arrays
// and so cannot be brace-assigned at a call site the way a struct of scalars could be.
static FluidVolumeDesc makeDesc(f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz, i32 sx, i32 sy, i32 sz) {
    FluidVolumeDesc d;
    d.centreCm[0] = cx; d.centreCm[1] = cy; d.centreCm[2] = cz;
    d.halfExtentCm[0] = hx; d.halfExtentCm[1] = hy; d.halfExtentCm[2] = hz;
    d.subdivisions[0] = sx; d.subdivisions[1] = sy; d.subdivisions[2] = sz;
    return d;
}

using EdgeKey = std::pair<i32, i32>;

// One pass over `indices` builds both maps the CLOSED and CONSISTENTLY WOUND checks need. Keying the
// first on the undirected pair and the second on the directed one, from the SAME walk over the SAME
// triangle list, is what makes the two checks look at literally the same edges rather than two
// structurally similar but separately-computed ones that could quietly drift apart.
struct EdgeStats {
    std::map<EdgeKey, i32> undirectedCounts;
    std::map<EdgeKey, i32> directedCounts;
};

static EdgeStats collectEdgeStats(const std::vector<i32>& indices) {
    EdgeStats stats;
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        const i32 tri[3] = {indices[t + 0], indices[t + 1], indices[t + 2]};
        for (int e = 0; e < 3; ++e) {
            const i32 a = tri[e];
            const i32 b = tri[(e + 1) % 3];
            const EdgeKey undirected = a < b ? EdgeKey(a, b) : EdgeKey(b, a);
            ++stats.undirectedCounts[undirected];
            ++stats.directedCounts[EdgeKey(a, b)];
        }
    }
    return stats;
}

// Case 1: closedness and winding, checked over two different lattices -- a minimal 1x1x1 cube (six
// faces, one quad each, the smallest shape that can be wrong) and a non-uniform 4x3x2 lattice (every
// axis subdivided a different amount, so an interior seam between two faces has real triangles either
// side of it to test rather than a single shared quad edge).
static void checkClosedAndWound(const char* label, const std::vector<i32>& indices) {
    const EdgeStats stats = collectEdgeStats(indices);

    // CLOSED. An edge shared by only one triangle is a hole; shared by three, a fold. Either is
    // invisible to a vertex or triangle count alone -- generateFluidSeedShell's own block-size
    // arithmetic could produce exactly the right totals and still leave one seam unstitched -- which
    // is why this walks every edge instead of trusting the totals.
    i32 notTwo = 0;
    for (const auto& kv : stats.undirectedCounts) {
        if (kv.second != 2) ++notTwo;
    }
    check(notTwo == 0, std::string(label) + ": every one of " +
          std::to_string(stats.undirectedCounts.size()) +
          " undirected edges is shared by exactly two triangles (" + std::to_string(notTwo) +
          " were not)");

    // CONSISTENTLY WOUND. Vertex sharing does not depend on winding at all (FluidVolume.cpp's own
    // comment on the point), so a single inverted face still passes the check above -- this is the
    // only one of the two that would catch it. The two triangles at a shared edge must traverse it in
    // opposite directions, which means the same directed pair (a, b) may never occur twice.
    i32 repeated = 0;
    for (const auto& kv : stats.directedCounts) {
        if (kv.second != 1) ++repeated;
    }
    check(repeated == 0, std::string(label) + ": every one of " +
          std::to_string(stats.directedCounts.size()) +
          " directed edges appears exactly once (" + std::to_string(repeated) + " repeated)");
}

static void testClosedAndConsistentlyWound() {
    std::vector<f32> pos;
    std::vector<i32> idx;

    generateFluidSeedShell(makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 1, 1, 1), pos, idx);
    checkClosedAndWound("minimal cube (1,1,1)", idx);

    generateFluidSeedShell(makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 4, 3, 2), pos, idx);
    checkClosedAndWound("non-uniform lattice (4,3,2)", idx);
}

// Case 2: no two seed vertices land on the same position. Emitting the box face by face without the
// shared-vertex bookkeeping generateFluidSeedShell's own header comment describes would still produce
// a mesh that LOOKS closed at every seam, geometrically -- the coincident vertices sit exactly on top
// of each other -- while tearing the instant the solver treats them as the independent particles they
// actually are. This is a different failure mode than the edge checks above and neither implies it.
static void testNoDuplicatedSeamVertices() {
    std::vector<f32> pos;
    std::vector<i32> idx;
    generateFluidSeedShell(makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 4, 3, 2), pos, idx);

    const usize vertCount = pos.size() / 3;
    // O(n^2) over a few dozen vertices is negligible here, and is the most direct statement of the
    // property under test: no two of the positions this call actually emitted coincide. A spatial
    // hash would only earn its keep if this needed to scale past a test fixture's lattice sizes.
    i32 coincidentPairs = 0;
    for (usize a = 0; a < vertCount; ++a) {
        for (usize b = a + 1; b < vertCount; ++b) {
            const f32 dx = pos[a * 3 + 0] - pos[b * 3 + 0];
            const f32 dy = pos[a * 3 + 1] - pos[b * 3 + 1];
            const f32 dz = pos[a * 3 + 2] - pos[b * 3 + 2];
            if (dx * dx + dy * dy + dz * dz < 1e-6f) ++coincidentPairs;
        }
    }
    check(coincidentPairs == 0, "no two of the " + std::to_string(vertCount) +
          " seed vertices coincide (" + std::to_string(coincidentPairs) + " pairs did)");
}

// Case 3: the extent is what was asked for, on both halves of the local/world split
// generateFluidSeedShell's own header comment insists on. An asymmetric half-extent per axis and a
// non-zero centre are used on purpose: a generator that silently swapped two axes, or a FluidVolume
// that forgot to add centreCm when placing the shell in world space, would both still pass a
// symmetric, origin-centred check.
static void testExtentMatchesRequest() {
    const FluidVolumeDesc desc = makeDesc(500.0f, -200.0f, 30.0f, 120.0f, 75.0f, 40.0f, 5, 4, 3);

    std::vector<f32> localPos;
    std::vector<i32> localIdx;
    generateFluidSeedShell(desc, localPos, localIdx);

    // generateFluidSeedShell is LOCAL space -- desc.centreCm is deliberately never baked in here, see
    // its own header comment -- so this bounding box must sit on the origin, not on centreCm.
    f32 loLocal[3] = {1e30f, 1e30f, 1e30f};
    f32 hiLocal[3] = {-1e30f, -1e30f, -1e30f};
    for (usize v = 0; v < localPos.size() / 3; ++v) {
        for (int a = 0; a < 3; ++a) {
            loLocal[a] = std::min(loLocal[a], localPos[v * 3 + a]);
            hiLocal[a] = std::max(hiLocal[a], localPos[v * 3 + a]);
        }
    }
    for (int a = 0; a < 3; ++a) {
        check(near(loLocal[a], -desc.halfExtentCm[a], 1e-3f) &&
              near(hiLocal[a], desc.halfExtentCm[a], 1e-3f),
              "local seed bounding box axis " + std::to_string(a) + " spans [" +
              std::to_string(loLocal[a]) + ", " + std::to_string(hiLocal[a]) + "], expected [-" +
              std::to_string(desc.halfExtentCm[a]) + ", " + std::to_string(desc.halfExtentCm[a]) + "]");
    }

    // FluidVolume::generateSeedShell places that same local shell at desc().centreCm (LOCAL + centre
    // = WORLD, per FluidVolume.hpp's own comment) -- this is where the "requested centre" half of the
    // property actually gets exercised, since the free function above never sees centreCm at all.
    FluidVolume vol(desc);
    vol.generateSeedShell();
    const auto& worldPos = vol.positionsCm();
    f32 loWorld[3] = {1e30f, 1e30f, 1e30f};
    f32 hiWorld[3] = {-1e30f, -1e30f, -1e30f};
    for (usize v = 0; v < worldPos.size() / 3; ++v) {
        for (int a = 0; a < 3; ++a) {
            loWorld[a] = std::min(loWorld[a], worldPos[v * 3 + a]);
            hiWorld[a] = std::max(hiWorld[a], worldPos[v * 3 + a]);
        }
    }
    for (int a = 0; a < 3; ++a) {
        const f32 expectedLo = desc.centreCm[a] - desc.halfExtentCm[a];
        const f32 expectedHi = desc.centreCm[a] + desc.halfExtentCm[a];
        check(near(loWorld[a], expectedLo, 1e-3f) && near(hiWorld[a], expectedHi, 1e-3f),
              "world seed bounding box axis " + std::to_string(a) + " spans [" +
              std::to_string(loWorld[a]) + ", " + std::to_string(hiWorld[a]) + "], expected [" +
              std::to_string(expectedLo) + ", " + std::to_string(expectedHi) + "]");
    }
}

// Case 4: every triangle faces outward. FluidVolume.hpp's own header comment is explicit that the
// per-face tangent-pair table generateFluidSeedShell uses "has not been checked by running anything",
// and that a wrong entry would still leave the mesh closed -- so this is the check that actually
// stands between that comment and a shell one face turned inside out. The 4x3x2 lattice is used again
// rather than the minimal cube, since a bug in only one face's tangent-pair entry needs more than a
// single quad on that face to show up as anything other than luck.
static void testEveryTriangleFacesOutward() {
    std::vector<f32> pos;
    std::vector<i32> idx;
    generateFluidSeedShell(makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 4, 3, 2), pos, idx);

    // Local space, so the box's own centre is the origin and centroid - centre is just the centroid.
    i32 inward = 0;
    const usize triCount = idx.size() / 3;
    for (usize t = 0; t < triCount; ++t) {
        const i32 ia = idx[t * 3 + 0], ib = idx[t * 3 + 1], ic = idx[t * 3 + 2];
        const f32* a = &pos[static_cast<usize>(ia) * 3];
        const f32* b = &pos[static_cast<usize>(ib) * 3];
        const f32* c = &pos[static_cast<usize>(ic) * 3];
        const f32 abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
        const f32 acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
        // (C-A) x (B-A) -- the exact identity FluidVolume.cpp's own comments derive the winding from,
        // and the same one recomputeNormals uses, so checking outward-ness any other way would be
        // testing this generator against a different convention than the engine's own.
        const f32 nx = acy * abz - acz * aby;
        const f32 ny = acz * abx - acx * abz;
        const f32 nz = acx * aby - acy * abx;
        const f32 cx = (a[0] + b[0] + c[0]) / 3.0f;
        const f32 cy = (a[1] + b[1] + c[1]) / 3.0f;
        const f32 cz = (a[2] + b[2] + c[2]) / 3.0f;
        if (nx * cx + ny * cy + nz * cz <= 0.0f) ++inward;
    }
    check(inward == 0, "all " + std::to_string(triCount) +
          " triangles face outward (normal . (centroid - centre) > 0); " + std::to_string(inward) +
          " did not");
}

// Case 5: vertex and triangle counts at two subdivision levels, hand-computed by counting a box
// rather than by re-deriving generateFluidSeedShell.cpp's own blockA/B/C arithmetic -- the point is
// for a change to that arithmetic to show up as a diff against an independent number written out
// here, not to have this test agree with itself no matter what the generator does.
static void testSubdivisionScaling() {
    std::vector<f32> pos1;
    std::vector<i32> idx1;
    generateFluidSeedShell(makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 1, 1, 1), pos1, idx1);
    // A minimal cube: 8 corners, 6 faces of 2 triangles each.
    check(pos1.size() / 3 == 8, "1x1x1 subdivisions yields 8 vertices, got " +
          std::to_string(pos1.size() / 3));
    check(idx1.size() / 3 == 12, "1x1x1 subdivisions yields 12 triangles, got " +
          std::to_string(idx1.size() / 3));

    std::vector<f32> pos2;
    std::vector<i32> idx2;
    generateFluidSeedShell(makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 2, 3, 1), pos2, idx2);
    // 2x3x1: two X faces of (ny+1)x(nz+1) = 4x2 = 8 vertices each (16), two Y faces of (nx-1)x(nz+1)
    // = 1x2 = 2 each (4), two Z faces of (nx-1)x(ny-1) = 1x2 = 2 each (4) -- 24 total. Triangle count
    // is 2 per quad over 2*(nx*ny + ny*nz + nz*nx) = 2*(6+3+2) = 22 quads, so 44 triangles.
    check(pos2.size() / 3 == 24, "2x3x1 subdivisions yields 24 vertices, got " +
          std::to_string(pos2.size() / 3));
    check(idx2.size() / 3 == 44, "2x3x1 subdivisions yields 44 triangles, got " +
          std::to_string(idx2.size() / 3));
}

// Case 6: normals, fed the seed positions unchanged. FluidVolume::generateSeedShell calls
// recomputeNormals immediately (FluidVolume.hpp's own comment on why: a renderer that draws before
// the solver's first callback still needs correctly-lit positions), so this checks that recomputation
// against the one shape whose correct normals are obvious by inspection -- a box.
static void testSeedNormalsAreOutwardAndUnitLength() {
    const FluidVolumeDesc desc = makeDesc(50.0f, -25.0f, 10.0f, 100.0f, 100.0f, 50.0f, 3, 3, 2);
    FluidVolume vol(desc);
    vol.generateSeedShell();

    const auto& worldPos = vol.positionsCm();
    const auto& nrm = vol.normals();
    check(nrm.size() == worldPos.size(), "normals() is index-parallel with positionsCm(): " +
          std::to_string(nrm.size() / 3) + " normals for " + std::to_string(worldPos.size() / 3) +
          " positions");

    i32 notUnit = 0;
    i32 inward = 0;
    const usize vertCount = worldPos.size() / 3;
    for (usize v = 0; v < vertCount; ++v) {
        const f32 nx = nrm[v * 3 + 0], ny = nrm[v * 3 + 1], nz = nrm[v * 3 + 2];
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!near(len, 1.0f, 1e-3f)) ++notUnit;

        // World space here, so "centre" is desc.centreCm rather than the origin -- this is the seam
        // between recomputeNormals' local-triangle-geometry math (already checked directly, in local
        // space, by testEveryTriangleFacesOutward) and the world-placed positions a renderer reads.
        const f32 px = worldPos[v * 3 + 0] - desc.centreCm[0];
        const f32 py = worldPos[v * 3 + 1] - desc.centreCm[1];
        const f32 pz = worldPos[v * 3 + 2] - desc.centreCm[2];
        if (nx * px + ny * py + nz * pz <= 0.0f) ++inward;
    }
    check(notUnit == 0, "all " + std::to_string(vertCount) +
          " recomputed seed normals are unit length (" + std::to_string(notUnit) + " were not)");
    check(inward == 0, "all " + std::to_string(vertCount) +
          " recomputed seed normals point outward from the box centre (" + std::to_string(inward) +
          " did not)");
}

int main() {
    AVER_INFO("FluidVolumeTest");
    testClosedAndConsistentlyWound();
    testNoDuplicatedSeamVertices();
    testExtentMatchesRequest();
    testEveryTriangleFacesOutward();
    testSubdivisionScaling();
    testSeedNormalsAreOutwardAndUnitLength();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
