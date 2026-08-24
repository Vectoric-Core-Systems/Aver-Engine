// FluidVolume.hpp's seed-shell geometry: generateFluidSeedShell's pure box-lattice arithmetic, and
// the placement/normal bookkeeping FluidVolume::generateSeedShell layers on top of it. CPU-only, no
// RHI device, no window, no physics solver -- this exercises exactly the "(a)" half of
// FluidVolume.hpp's own two-halves comment, never updateFromSimulation's solver-fed half, because
// there is no solver here to hand it simulated vertices from. Links Aver.Fluids because FluidVolume.cpp
// is a real .cpp (unlike header-only GerstnerWave.hpp), and through it Aver.RHI -- the same link shape
// UnderwaterFogTest.cpp uses for the same reason: a struct and some plain arithmetic, never a device.
//
// What is actually worth checking here is whether the solver can hold pressure in this shell at all,
// which is a question about the mesh's TOPOLOGY (closed, consistently wound, no torn seams) at least
// as much as about where its vertices land. FluidVolume.hpp's own header comment is explicit that the
// per-face winding table has been worked out algebraically but "not been checked by running anything",
// and that a wrong entry in it would still pass a closedness check alone -- so closedness and winding
// are checked here as two separate properties over the same edge set, not folded into one.
#include "aver/fluids/FluidVolume.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::fluids;

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

// ---- the derived internal pressure --------------------------------------------------------------
//
// Checked as a FORMULA rather than against remembered numbers, because the two ways this went wrong
// were both invisible in a single value: it was once a constant that happened to suit no pool at all,
// and once right in shape but 10,000x out because the derivation was done in centimetres while Jolt
// integrates in metres. A relationship holds or it does not; a number can be wrong and still look
// plausible.
static void testDerivedPressure() {
    AVER_INFO("-- the pressure a shell needs is derived from the shell --");

    fluids::FluidVolumeDesc d;
    d.halfExtentCm[0] = 300.0f; d.halfExtentCm[1] = 200.0f; d.halfExtentCm[2] = 60.0f;
    d.subdivisions[0] = 8; d.subdivisions[1] = 8; d.subdivisions[2] = 4;

    check(fluids::FluidVolumeDesc{}.pressure == fluids::kFluidPressureAuto,
          "a desc asks for a derived pressure unless its author says otherwise");

    const f32 g = 980.0f;
    const f32 expect = fluids::kFluidPressureHeadroom * fluids::kFluidCmToJolt *
                       2.0f * g * 60.0f * 9.0f * 9.0f;
    check(near(fluids::fluidPressureFor(d, g), expect, 1e-3f),
          "it is 2 g hz (sx+1)(sy+1), in Jolt's metres");

    // IN METRES. Independent of the closed form above: a 6 x 4 x 1.2 m box of particles weighing one
    // unit each needs pressure/V * A to balance gravity * mass, with V and A read in metres. This is
    // the check that would have caught the 10,000x, since it never mentions a centimetre.
    const f32 volumeM3 = 6.0f * 4.0f * 1.2f;
    const f32 topAreaM2 = 6.0f * 4.0f;
    const f32 topMass = 9.0f * 9.0f;
    const f32 fromUnits = fluids::kFluidPressureHeadroom * (g / 100.0f) * volumeM3 * topMass / topAreaM2;
    check(near(fluids::fluidPressureFor(d, g), fromUnits, 1e-2f),
          "and it balances that shell's own weight when everything is read in metres");

    // DEPTH, NOT FOOTPRINT. Widening a pool adds enclosed volume and top area in the same proportion;
    // deepening it adds only volume. A constant could express neither.
    fluids::FluidVolumeDesc wide = d;   wide.halfExtentCm[0] *= 3.0f;
    fluids::FluidVolumeDesc deep = d;   deep.halfExtentCm[2] *= 3.0f;
    check(near(fluids::fluidPressureFor(wide, g), fluids::fluidPressureFor(d, g), 1e-3f),
          "three times as wide needs the same pressure");
    check(near(fluids::fluidPressureFor(deep, g), 3.0f * fluids::fluidPressureFor(d, g), 1e-1f),
          "three times as deep needs three times the pressure");

    // A finer top grid puts more particles -- and so more mass -- on the same footprint.
    fluids::FluidVolumeDesc fine = d;   fine.subdivisions[0] = 17; fine.subdivisions[1] = 17;
    check(fluids::fluidPressureFor(fine, g) > fluids::fluidPressureFor(d, g),
          "a finer top grid weighs more and needs more");

    check(near(fluids::fluidPressureFor(d, 0.0f), 0.0f, 1e-6f),
          "no gravity, no pressure -- there is nothing to hold up");
    fluids::FluidVolumeDesc flat = d;   flat.halfExtentCm[2] = 0.0f;
    check(near(fluids::fluidPressureFor(flat, g), 0.0f, 1e-6f), "and a shell with no depth needs none either");
}

// Case 7: fluidShellParticleCount agrees with what generateFluidSeedShell actually builds --
// FluidVolume.hpp's own comment on fluidShellParticleCount promises this ("FluidVolumeTest checks
// them against each other directly for that reason"), so this is that check, not a re-derivation of
// the same arithmetic that could quietly drift alongside it.
static void testShellParticleCountMatchesGenerator() {
    AVER_INFO("-- fluidShellParticleCount agrees with what generateFluidSeedShell actually builds --");
    const FluidVolumeDesc descs[] = {
        makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 1, 1, 1),
        makeDesc(0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 50.0f, 4, 3, 2),
        makeDesc(0.0f, 0.0f, 0.0f, 300.0f, 200.0f, 60.0f, 8, 8, 4),
    };
    for (const auto& d : descs) {
        std::vector<f32> pos;
        std::vector<i32> idx;
        generateFluidSeedShell(d, pos, idx);
        const i32 built = static_cast<i32>(pos.size() / 3);
        check(fluidShellParticleCount(d) == built,
              "fluidShellParticleCount(" + std::to_string(d.subdivisions[0]) + "," +
              std::to_string(d.subdivisions[1]) + "," + std::to_string(d.subdivisions[2]) +
              ") = " + std::to_string(fluidShellParticleCount(d)) +
              ", the generator actually built " + std::to_string(built));
    }
}

// ---- density: an unset one reproduces the old mass-1 behaviour; a real one scales mass AND pressure
static void testDensityScalesMassAndPressure() {
    AVER_INFO("-- an unset density reproduces mass 1; a real one scales both mass and pressure --");

    fluids::FluidVolumeDesc d;
    d.halfExtentCm[0] = 300.0f; d.halfExtentCm[1] = 200.0f; d.halfExtentCm[2] = 60.0f;
    d.subdivisions[0] = 8; d.subdivisions[1] = 8; d.subdivisions[2] = 4;

    check(near(fluids::fluidParticleMassKg(d), 1.0f, 1e-6f),
          "a desc that never asked for a density gets mass 1, today's exact behaviour");

    const f32 basePressure = fluids::fluidPressureFor(d);

    fluids::FluidVolumeDesc water = d;
    water.densityKgM3 = 1000.0f;
    const f32 massWater = fluids::fluidParticleMassKg(water);
    // mass = density * (8 hx hy hz, in m^3) / particleCount, restated directly here rather than
    // trusting fluidParticleMassKg to check itself against its own formula.
    const f32 volumeM3 = 8.0f * 3.0f * 2.0f * 0.6f;   // hx=3m, hy=2m, hz=0.6m
    const f32 expectMass = 1000.0f * volumeM3 / static_cast<f32>(fluids::fluidShellParticleCount(d));
    check(near(massWater, expectMass, 1e-3f),
          "water's own particle mass matches density * volume / particleCount directly (" +
          std::to_string(massWater) + " vs " + std::to_string(expectMass) + " kg)");
    check(massWater > 1.0f, "water's real particle mass is well above the old mass-1 stand-in (" +
                            std::to_string(massWater) + " kg)");

    fluids::FluidVolumeDesc water2x = d;
    water2x.densityKgM3 = 2000.0f;
    check(near(fluids::fluidParticleMassKg(water2x), 2.0f * massWater, 1e-2f),
          "doubling density doubles the particle mass");

    // THE FIX THIS FILE EXISTS TO PROVE: pressure must scale by the SAME real mass invMasses now
    // carries, or a dense fluid puddles regardless of pressure -- see fluidPressureFor's own "WHY
    // THIS HAD TO CHANGE" comment.
    check(near(fluids::fluidPressureFor(water), basePressure * massWater, 1e-1f),
          "the pressure this shell derives scales by the SAME real mass -- unset density's own "
          "pressure times that mass, not a second, independent number");
    check(fluids::fluidPressureFor(water) > basePressure,
          "so a real (non-unit) density needs MORE pressure to hold the same shell up, matching "
          "the extra weight it now actually carries");

    fluids::FluidVolumeDesc zeroDensity = d;
    zeroDensity.densityKgM3 = 0.0f;
    check(near(fluids::fluidParticleMassKg(zeroDensity), 1.0f, 1e-6f),
          "a density of exactly 0 (nonsensical for a fluid) falls back to mass 1, same as unset");
}

// ---- the material layer: presets, the viscosity->damping fit, and the precedence rule -----------

// Case 8: the presets are just the same struct, filled -- checked against the literal numbers
// FluidMaterial's own static factories return (design brief 5's real order-of-magnitude figures),
// so a future edit to one of them shows up as a diff here rather than silently drifting from what
// this file's own header comment claims they are.
static void testMaterialPresets() {
    AVER_INFO("-- FluidMaterial presets return the design brief's own real figures --");

    const FluidMaterial water = FluidMaterial::Water();
    check(near(water.densityKgM3, 998.0f, 1e-3f) && near(water.viscosityPaS, 1.0e-3f, 1e-9f),
          "Water(): 998 kg/m^3, 1.0e-3 Pa*s");

    const FluidMaterial oil = FluidMaterial::LightOil();
    check(near(oil.densityKgM3, 900.0f, 1e-3f) && near(oil.viscosityPaS, 0.1f, 1e-6f),
          "LightOil(): 900 kg/m^3, 0.1 Pa*s");

    const FluidMaterial honey = FluidMaterial::Honey();
    check(near(honey.densityKgM3, 1420.0f, 1e-3f) && near(honey.viscosityPaS, 10.0f, 1e-6f),
          "Honey(): 1420 kg/m^3, 10.0 Pa*s -- the SAME number as kViscosityAnchorHighPaS, on purpose");

    const FluidMaterial lava = FluidMaterial::Lava();
    check(near(lava.densityKgM3, 2900.0f, 1e-3f) && near(lava.viscosityPaS, 1000.0f, 1e-3f),
          "Lava(): 2900 kg/m^3, 1000 Pa*s");

    // fluidMaterialPreset is the name -> struct lookup every consumer (SandboxApp's two provider
    // call sites) actually uses; checked here against the SAME literal factories above, so a name
    // silently mapping to the wrong preset would show up as a mismatch, not just a missing case.
    auto byName = fluidMaterialPreset("HONEY");   // case-insensitive, deliberately shouted here
    check(byName.has_value() && near(byName->densityKgM3, honey.densityKgM3, 1e-3f),
          "fluidMaterialPreset is case-insensitive and 'HONEY' resolves to Honey()");
    check(fluidMaterialPreset("light oil").has_value() && fluidMaterialPreset("oil").has_value(),
          "...and LightOil answers to both 'light oil' and the bare 'oil' alias");
    check(!fluidMaterialPreset("mercury").has_value(),
          "an unrecognised name resolves to nothing, for the caller to report by name");
}

// Case 9: the viscosity->damping fit -- the two measured anchors land exactly on their own named
// constants, the mapping is monotonic increasing between them (more viscous -> more damping, the
// direction the brief's own water/honey comparison demands), and it CLAMPS rather than
// extrapolates past either anchor -- the specific, acknowledged gap FluidMaterial::Lava()'s own
// comment names (lava's viscosity is far past the honey anchor and must read identically to it).
static void testViscosityDampingFit() {
    AVER_INFO("-- fluidDampingForViscosity: the two measured anchors, monotonic between them, "
              "clamped beyond either --");

    check(near(fluidDampingForViscosity(kViscosityAnchorLowPaS), kDampingAnchorLow, 1e-4f),
          "the water anchor (1.0e-3 Pa*s) maps to exactly kDampingAnchorLow (0.01)");
    check(near(fluidDampingForViscosity(kViscosityAnchorHighPaS), kDampingAnchorHigh, 1e-4f),
          "the honey anchor (10 Pa*s) maps to exactly kDampingAnchorHigh (3.0)");

    // Monotonic across three points spanning the calibrated range -- SAE-10 oil's own 0.1 Pa*s
    // (FluidMaterial::LightOil(), the geometric midpoint of the two anchors) sits strictly between.
    const f32 dWater = fluidDampingForViscosity(1.0e-3f);
    const f32 dOil   = fluidDampingForViscosity(0.1f);
    const f32 dHoney = fluidDampingForViscosity(10.0f);
    check(dWater < dOil && dOil < dHoney,
          "damping rises monotonically from water (" + std::to_string(dWater) + ") through oil (" +
          std::to_string(dOil) + ") to honey (" + std::to_string(dHoney) + ")");

    // CLAMPED, NOT EXTRAPOLATED: lava's own 1000 Pa*s is 100x past the honey anchor, and the
    // calibration's own reliable range ends AT that anchor (see fluidDampingForViscosity's own
    // header comment on why damping=10.0 was excluded) -- so lava must map to the identical damping
    // as honey, not a larger number nothing measured.
    check(near(fluidDampingForViscosity(1000.0f), fluidDampingForViscosity(10.0f), 1e-4f),
          "a viscosity far past the high anchor clamps to that anchor's own damping, matching "
          "FluidMaterial::Lava()'s own acknowledged-gap comment");
    check(near(fluidDampingForViscosity(1.0e-6f), fluidDampingForViscosity(1.0e-3f), 1e-4f),
          "...and symmetrically below the low anchor");

    check(near(fluidDampingForViscosity(0.0f), kDampingAnchorLow, 1e-6f) &&
          near(fluidDampingForViscosity(-5.0f), kDampingAnchorLow, 1e-6f),
          "a non-positive viscosity (not physical) falls back to the least-damped end, not NaN");
}

// Case 10: fluidResolveMaterial -- the one function the design brief's precedence rule actually
// lives in. An unset material is a no-op; a set one overwrites density/damping; and a material
// alongside a HAND-SET raw damping is refused outright, with desc left completely untouched --
// never a partial application of density without damping or vice versa.
static void testResolveMaterialPrecedence() {
    AVER_INFO("-- fluidResolveMaterial: unset is a no-op, a set material overwrites, and a "
              "material + hand-set damping REFUSES rather than picking a winner --");

    {
        FluidVolumeDesc d;
        const FluidVolumeDesc before = d;
        std::string conflict;
        check(fluidResolveMaterial(d, &conflict) && conflict.empty(),
              "no material set: resolves true, no conflict message");
        check(near(d.densityKgM3, before.densityKgM3, 1e-9f) && near(d.damping, before.damping, 1e-9f),
              "...and desc is byte-for-bit unchanged");
    }
    {
        FluidVolumeDesc d;
        d.material = FluidMaterial::Honey();
        check(fluidResolveMaterial(d), "a material with the desc's own default damping resolves true");
        check(near(d.densityKgM3, 1420.0f, 1e-3f), "...and density becomes the material's own");
        check(near(d.damping, kDampingAnchorHigh, 1e-4f),
              "...and damping becomes fluidDampingForViscosity(10.0) == kDampingAnchorHigh");
    }
    {
        // THE PRECEDENCE RULE ITSELF: a material AND a hand-set raw damping on the same desc.
        FluidVolumeDesc d;
        d.material = FluidMaterial::Water();
        d.damping = 0.75f;   // != kDefaultFluidDamping (0.1f): the author explicitly set this
        const FluidVolumeDesc before = d;
        std::string conflict;
        check(!fluidResolveMaterial(d, &conflict),
              "a material alongside a hand-set raw damping REFUSES (returns false)");
        check(!conflict.empty(), "...and explains itself rather than failing silently");
        check(conflict.find("0.75") != std::string::npos,
              "...naming the conflicting raw value in the message (" + conflict + ")");
        check(near(d.densityKgM3, before.densityKgM3, 1e-9f) && near(d.damping, before.damping, 1e-9f),
              "...leaving desc completely UNCHANGED -- no partial application of density without "
              "damping, or of damping without density");
    }
    {
        // The boundary of the rule: kDefaultFluidDamping itself is NOT a conflict, since a desc that
        // never touched damping at all reads identically to one whose author retyped the default.
        FluidVolumeDesc d;
        d.material = FluidMaterial::Honey();
        d.damping = kDefaultFluidDamping;   // exactly the default -- indistinguishable from "unset"
        check(fluidResolveMaterial(d), "damping left at exactly kDefaultFluidDamping is not a "
                                       "conflict, matching kFluidPressureAuto's own sentinel-by-"
                                       "default-value convention");
    }
}

int main() {
    AVER_INFO("FluidVolumeTest");
    testClosedAndConsistentlyWound();
    testNoDuplicatedSeamVertices();
    testExtentMatchesRequest();
    testEveryTriangleFacesOutward();
    testSubdivisionScaling();
    testSeedNormalsAreOutwardAndUnitLength();
    testDerivedPressure();
    testShellParticleCountMatchesGenerator();
    testDensityScalesMassAndPressure();
    testMaterialPresets();
    testViscosityDampingFit();
    testResolveMaterialPrecedence();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
