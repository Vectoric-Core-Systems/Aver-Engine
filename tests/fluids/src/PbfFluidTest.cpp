// The particle fluid: PbfFluid (solver), SolidField (static-collider distance field), FluidSurfaceMesher
// (density splat + marching tetrahedra) and the desc -> solver-settings mapping. CPU only: no device,
// no window, no physics world -- scene colliders are stood in for by an analytic overlap function.
#include "aver/fluids/FluidSurface.hpp"
#include "aver/fluids/FluidVolume.hpp"
#include "aver/fluids/PbfFluid.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::fluids;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static f32 axisMax(const PbfFluid& f, int a) {
    f32 m = -1.0e30f;
    for (u32 i = 0; i < f.count(); ++i) m = std::max(m, f.positions()[i * 3 + a]);
    return m;
}
static f32 axisMin(const PbfFluid& f, int a) {
    f32 m = 1.0e30f;
    for (u32 i = 0; i < f.count(); ++i) m = std::min(m, f.positions()[i * 3 + a]);
    return m;
}
// Height of the water: the 95th-percentile particle plus half a spacing (the top layer's own extent),
// measured from the floor. Robust to a stray splash particle.
static f32 depthOf(const PbfFluid& f, f32 floorZ) {
    std::vector<f32> z;
    for (u32 i = 0; i < f.count(); ++i) z.push_back(f.positions()[i * 3 + 2]);
    std::sort(z.begin(), z.end());
    return z[static_cast<usize>(static_cast<f32>(z.size()) * 0.95f)] + 0.5f * f.spacing() - floorZ;
}

// A 1 x 1 x 0.8 m open tank, filled to the brim.
static PbfSettings tankSettings() {
    PbfSettings s;
    s.bounds.lo[0] = -0.5f; s.bounds.hi[0] = 0.5f;
    s.bounds.lo[1] = -0.5f; s.bounds.hi[1] = 0.5f;
    s.bounds.lo[2] = -0.4f; s.bounds.hi[2] = 0.4f;
    s.spacingM = 0.1f;
    return s;
}

static void testTankHoldsItsDepth() {
    AVER_INFO("-- a tank of water settles and holds its depth --");
    PbfFluid f;
    const u32 n = f.init(tankSettings());
    check(n == 800, "10 x 10 x 8 particles seeded (" + std::to_string(n) + ")");
    const f32 d0 = depthOf(f, -0.4f);
    for (int i = 0; i < 360; ++i) f.step(kPbfFixedDt);   // 6 s
    const f32 d1 = depthOf(f, -0.4f);
    check(f.count() == n, "the particle count is preserved");
    check(d1 >= 0.9f * d0, "it keeps >= 90% of its initial depth (" + std::to_string(d1) + " of " +
                           std::to_string(d0) + " m)");
    check(d1 <= 1.05f * d0, "and does not balloon (" + std::to_string(d1) + " m)");
    check(axisMin(f, 2) >= -0.4f, "nothing fell through the floor (zmin " + std::to_string(axisMin(f, 2)) + ")");
    check(axisMax(f, 0) <= 0.5f && axisMin(f, 0) >= -0.5f && axisMax(f, 1) <= 0.5f && axisMin(f, 1) >= -0.5f,
          "nothing left through a side wall");
    f32 maxSpeed = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        const f32* v = &f.velocities()[i * 3];
        maxSpeed = std::max(maxSpeed, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
    check(maxSpeed < 1.0f, "and it is at rest, not still churning (max speed " + std::to_string(maxSpeed) + " m/s)");
}

static void testDamBreakSpreads() {
    AVER_INFO("-- a dam-break column spreads along the floor --");
    PbfSettings s;
    s.bounds.lo[0] = -1.0f; s.bounds.hi[0] = 1.0f;
    s.bounds.lo[1] = -0.2f; s.bounds.hi[1] = 0.2f;
    s.bounds.lo[2] = 0.0f;  s.bounds.hi[2] = 1.0f;
    s.spacingM = 0.05f;
    s.useFill = true;
    s.fillLo[0] = -1.0f; s.fillHi[0] = -0.6f;
    s.fillLo[1] = -0.2f; s.fillHi[1] = 0.2f;
    s.fillLo[2] = 0.0f;  s.fillHi[2] = 0.8f;
    PbfFluid f;
    const u32 n = f.init(s);
    const f32 front0 = axisMax(f, 0);
    const f32 top0 = axisMax(f, 2);
    for (int i = 0; i < 90; ++i) f.step(kPbfFixedDt);   // 1.5 s
    check(f.count() == n, "particle count preserved (" + std::to_string(n) + ")");
    check(axisMax(f, 0) > front0 + 1.0f, "the front travelled > 1 m (" + std::to_string(front0) + " -> " +
                                         std::to_string(axisMax(f, 0)) + ")");
    check(axisMax(f, 2) < top0 - 0.2f, "while the column dropped (" + std::to_string(top0) + " -> " +
                                       std::to_string(axisMax(f, 2)) + ")");
    check(axisMin(f, 2) >= 0.0f, "and nothing went below the floor");
}

// An axis-aligned box and a sphere as stand-in colliders.
static bool sphereHitsBox(const f32 c[3], f32 r, const f32 lo[3], const f32 hi[3]) {
    f32 d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const f32 v = std::max(lo[a] - c[a], std::max(0.0f, c[a] - hi[a]));
        d2 += v * v;
    }
    return d2 <= r * r;
}

static void testSolidFieldAndFloorCollision() {
    AVER_INFO("-- scene colliders: the distance field is signed, and a tank on a solid floor holds --");

    // A solid slab below z = -0.3 with a pillar on it.
    const f32 floorLo[3] = {-9, -9, -9}, floorHi[3] = {9, 9, -0.3f};
    const f32 pilLo[3] = {-0.1f, -0.1f, -0.3f}, pilHi[3] = {0.1f, 0.1f, -0.1f};
    auto overlap = [&](const f32 c[3], f32 r) {
        return sphereHitsBox(c, r, floorLo, floorHi) || sphereHitsBox(c, r, pilLo, pilHi);
    };
    SolidField sf;
    const f32 glo[3] = {-0.6f, -0.6f, -0.5f}, ghi[3] = {0.6f, 0.6f, 0.6f};
    sf.build(glo, ghi, 0.04f, overlap);
    check(!sf.empty() && sf.solidCellCount() > 0, "voxelised some solid cells (" + std::to_string(sf.solidCellCount()) + ")");

    f32 g[3];
    const f32 above[3] = {0.4f, 0.4f, 0.0f};
    const f32 dAbove = sf.sample(above, g);
    check(std::fabs(dAbove - 0.3f) < 0.06f, "0.3 m above the floor reads ~0.3 (" + std::to_string(dAbove) + ")");
    check(g[2] > 0.9f, "with the gradient pointing up (" + std::to_string(g[2]) + ")");
    const f32 inside[3] = {0.4f, 0.4f, -0.4f};
    check(sf.sample(inside) < 0.0f, "inside the slab is negative");
    const f32 outside[3] = {5.0f, 0.0f, 0.0f};
    check(sf.sample(outside) > 1.0e6f, "beyond the grid reads far (no collision)");

    // Water poured on it: container walls on the sides only, the slab is the floor.
    PbfSettings s;
    s.bounds.lo[0] = -0.5f; s.bounds.hi[0] = 0.5f;
    s.bounds.lo[1] = -0.5f; s.bounds.hi[1] = 0.5f;
    s.bounds.lo[2] = -0.3f; s.bounds.hi[2] = 0.5f;
    s.bounds.wallLo[2] = false;
    s.spacingM = 0.08f;
    PbfFluid f;
    const u32 n = f.init(s, &sf);
    check(n > 0 && n < 12 * 12 * 10, "seeding skipped particles inside the pillar (" + std::to_string(n) + ")");
    for (int i = 0; i < 360; ++i) f.step(kPbfFixedDt);
    check(f.count() == n, "particle count preserved");
    check(axisMin(f, 2) > -0.3f, "no particle fell into the floor slab (zmin " + std::to_string(axisMin(f, 2)) + ")");
    u32 inPillar = 0;
    for (u32 i = 0; i < n; ++i) {
        const f32* p = &f.positions()[i * 3];
        if (std::fabs(p[0]) < 0.09f && std::fabs(p[1]) < 0.09f && p[2] < -0.12f) ++inPillar;
    }
    check(inPillar == 0, "and none inside the pillar (" + std::to_string(inPillar) + ")");
}

static void testDeterminism() {
    AVER_INFO("-- the same input gives the same output --");
    PbfSettings s;
    s.bounds.lo[0] = -0.4f; s.bounds.hi[0] = 0.4f;
    s.bounds.lo[1] = -0.4f; s.bounds.hi[1] = 0.4f;
    s.bounds.lo[2] = -0.4f; s.bounds.hi[2] = 0.4f;
    s.useFill = true;
    s.fillLo[0] = -0.4f; s.fillHi[0] = 0.0f;
    s.fillLo[1] = -0.4f; s.fillHi[1] = 0.4f;
    s.fillLo[2] = -0.4f; s.fillHi[2] = 0.4f;
    s.spacingM = 0.08f;
    s.vorticity = 0.1f;
    PbfFluid a, b;
    a.init(s);
    b.init(s);
    for (int i = 0; i < 120; ++i) { a.step(kPbfFixedDt); b.step(kPbfFixedDt); }
    check(a.count() == b.count() && a.positions() == b.positions() && a.velocities() == b.velocities(),
          "two runs match bit for bit (" + std::to_string(a.count()) + " particles, 120 steps, vorticity on)");

    // advance() accumulates real time into whole fixed steps.
    PbfFluid c;
    c.init(s);
    check(c.advance(0.01f) == 0, "advance(10 ms) is not yet a step");
    check(c.advance(0.01f) == 1, "advance(+10 ms) crosses 1/60 s and runs one");
    check(c.advance(1.0f) == kPbfMaxStepsPerAdvance, "a long hitch is capped at the per-call maximum");
    check(c.advance(0.0f) == 0 && c.stepsRun() == 1u + kPbfMaxStepsPerAdvance, "and the backlog is dropped, not repaid");
}

static void testImpulse() {
    AVER_INFO("-- an impulse moves the particles it reaches and only those --");
    PbfFluid f;
    f.init(tankSettings());
    const f32 c[3] = {0.0f, 0.0f, 0.0f}, dv[3] = {2.0f, 0.0f, 0.0f};
    f.addImpulse(c, 0.2f, dv);
    u32 inside = 0, moved = 0, strayed = 0;
    for (u32 i = 0; i < f.count(); ++i) {
        const f32* p = &f.positions()[i * 3];
        const f32 vx = f.velocities()[i * 3];
        if (std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) < 0.2f) {
            ++inside;
            if (vx > 0.0f) ++moved;
        } else if (vx != 0.0f) {
            ++strayed;
        }
    }
    check(inside > 0 && moved > 0, "particles inside the radius got a velocity (" + std::to_string(moved) + " of " +
                                   std::to_string(inside) + ")");
    check(strayed == 0, "and none outside it did");
}

static void testSurfaceMesh() {
    AVER_INFO("-- the surface mesher: closed to the eye, outward, inside the box --");
    PbfFluid f;
    const PbfSettings s = tankSettings();
    f.init(s);
    for (int i = 0; i < 120; ++i) f.step(kPbfFixedDt);

    FluidSurfaceMesher m;
    m.configure(s.bounds, f.spacing(), 60000);
    m.build(f.positions().data(), f.count());
    check(m.triangleCount() > 200 && !m.truncated(), "a tank meshes to a few hundred triangles or more (" +
                                                       std::to_string(m.triangleCount()) + ")");

    const f32* P = m.positions().data();
    const f32* N = m.normals().data();
    u32 notUnit = 0, outside = 0, inward = 0, topFacing = 0, topTris = 0;
    for (u32 t = 0; t < m.triangleCount(); ++t) {
        f32 centroid[3] = {0, 0, 0}, nsum[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            const f32* n = &N[(t * 3 + k) * 3];
            const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (std::fabs(len - 1.0f) > 1.0e-3f) ++notUnit;
            for (int a = 0; a < 3; ++a) { centroid[a] += P[(t * 3 + k) * 3 + a] / 3.0f; nsum[a] += n[a]; }
            for (int a = 0; a < 3; ++a) {
                const f32 v = P[(t * 3 + k) * 3 + a];
                if (v < s.bounds.lo[a] - 3.0f * f.spacing() || v > s.bounds.hi[a] + 3.0f * f.spacing()) ++outside;
            }
        }
        // Geometric normal (C-A) x (B-A) must agree with the field normal: the engine's front face.
        const f32* A = &P[(t * 3 + 0) * 3];
        const f32* B = &P[(t * 3 + 1) * 3];
        const f32* C = &P[(t * 3 + 2) * 3];
        const f32 e1[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        const f32 e2[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
        const f32 gn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        if (gn[0] * nsum[0] + gn[1] * nsum[1] + gn[2] * nsum[2] < 0.0f) ++inward;
        if (centroid[2] > s.bounds.hi[2] - 1.5f * f.spacing() && std::fabs(centroid[0]) < 0.3f && std::fabs(centroid[1]) < 0.3f) {
            ++topTris;
            if (nsum[2] > 0.0f) ++topFacing;
        }
    }
    check(notUnit == 0, "every vertex normal is unit length");
    check(outside == 0, "no vertex strays more than 3 spacings outside the volume");
    check(inward == 0, "every triangle winds along its own surface normal");
    check(topTris > 0 && topFacing == topTris, "the free surface faces up (" + std::to_string(topFacing) + "/" +
                                               std::to_string(topTris) + ")");

    // The top of the mesh is where the water is.
    f32 zmax = -1.0e30f;
    for (u32 v = 0; v < m.triangleCount() * 3; ++v) zmax = std::max(zmax, P[v * 3 + 2]);
    check(std::fabs(zmax - axisMax(f, 2)) < 1.0f * f.spacing(),
          "the mesh top agrees with the particle top to within a spacing (" + std::to_string(zmax) + " vs " +
          std::to_string(axisMax(f, 2)) + ")");

    FluidSurfaceMesher cap;
    cap.configure(s.bounds, f.spacing(), 50);
    cap.build(f.positions().data(), f.count());
    check(cap.truncated() && cap.triangleCount() == 50, "a triangle cap truncates and says so");

    FluidSurfaceMesher none;
    none.configure(s.bounds, f.spacing(), 1000);
    none.build(f.positions().data(), 0);
    check(none.triangleCount() == 0, "no particles, no triangles");
}

static void testDescMapping() {
    AVER_INFO("-- a FluidVolumeDesc maps to solver settings; the retired pressure fields still parse --");
    FluidVolumeDesc d;
    d.halfExtentCm[0] = 300.0f; d.halfExtentCm[1] = 200.0f; d.halfExtentCm[2] = 60.0f;
    d.subdivisions[0] = 14; d.subdivisions[1] = 14; d.subdivisions[2] = 4;
    const PbfSettings s = fluidPbfSettings(d);
    check(std::fabs(s.bounds.hi[0] - 3.0f) < 1e-5f && std::fabs(s.bounds.lo[1] + 2.0f) < 1e-5f &&
          std::fabs(s.bounds.hi[2] - 0.6f) < 1e-5f, "half-extents in cm become a box in metres about the centre");
    check(s.bounds.wallLo[0] && s.bounds.wallHi[1] && s.bounds.wallLo[2] && !s.bounds.wallHi[2],
          "sides and floor are walls, the top is open by default");
    FluidVolumeDesc closed = d;
    closed.containTop = true; closed.containFloor = false; closed.containSides = false;
    const PbfSettings sc = fluidPbfSettings(closed);
    check(sc.bounds.wallHi[2] && !sc.bounds.wallLo[2] && !sc.bounds.wallLo[0], "the contain flags pick the walls");

    const f32 sp = fluidParticleSpacingCm(d);
    check(sp > 5.0f && sp < 40.0f, "the spacing derived from the subdivisions is a sensible lattice (" + std::to_string(sp) + " cm)");
    FluidVolumeDesc fixed = d;
    fixed.particleSpacingCm = 20.0f;
    check(fluidParticleSpacingCm(fixed) == 20.0f, "an authored spacing wins");

    PbfFluid f;
    FluidVolumeDesc big = d;
    big.particleSpacingCm = 1.0f;   // would be ~48 million particles
    const u32 n = f.init(fluidPbfSettings(big));
    check(n > 0 && n <= big.maxParticles, "the particle cap coarsens the fill (" + std::to_string(n) + " <= " +
                                          std::to_string(big.maxParticles) + ")");

    check(fluidXsphForDamping(0.0f) == 0.01f && fluidXsphForDamping(1000.0f) == 0.5f &&
          fluidXsphForDamping(3.0f) > fluidXsphForDamping(0.1f), "the viscosity coefficient rises with damping and is clamped");
    check(FluidVolumeDesc{}.pressure == kFluidPressureAuto && FluidVolumeDesc{}.compliance == kFluidLegacyCompliance,
          "the retired fields keep their old defaults, so unauthored records spawn unchanged");
}

int main() {
    AVER_INFO("PbfFluidTest");
    testTankHoldsItsDepth();
    testDamBreakSpreads();
    testSolidFieldAndFloorCollision();
    testDeterminism();
    testImpulse();
    testSurfaceMesh();
    testDescMapping();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
