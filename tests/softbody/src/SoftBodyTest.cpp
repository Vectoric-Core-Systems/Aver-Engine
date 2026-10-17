// The soft-body library's shared pieces: the crush curve and tear radius, cage building, bends,
// masses, repair, and the queries. Damage, islands, the mesh weld and the asset have their own suites.
// Pure arithmetic; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/Cage.hpp"

#include <cmath>
#include <string>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

int main() {
    AVER_INFO("SoftBodyTest");

    AVER_INFO("the crush curve and the tear radius");
    {
        sb::CrushParams p;
        checkNear(sb::crushCurve(20.0f, p), 20.0f, 1e-5f, "below the knee the depth passes through");
        checkNear(sb::crushCurve(50.0f, p), 50.0f, 1e-5f, "at the knee it is still the identity");
        const f32 ceilCm = p.knee * p.ceilingMult;
        check(sb::crushCurve(1000.0f, p) <= ceilCm, "it never exceeds the ceiling");
        check(sb::crushCurve(1000.0f, p) > ceilCm - 0.01f, "and approaches it");
        check(sb::crushCurve(80.0f, p) > sb::crushCurve(60.0f, p), "it is monotone above the knee");
        const f32 slope = (sb::crushCurve(50.01f, p) - sb::crushCurve(50.0f, p)) / 0.01f;
        checkNear(slope, 1.0f, 0.01f, "the slope is continuous at the knee (C1)");
        p.nonlinear = false;
        checkNear(sb::crushCurve(80.0f, p), 50.0f, 1e-5f, "switched off it is a hard clamp at the knee");
        checkNear(sb::impactTearRadius(1.0f, 60.0f, 50.0f), 0.0f, 1e-6f, "a shallow hit tears nothing");
        checkNear(sb::impactTearRadius(50.0f, 60.0f, 50.0f), 30.0f, 1e-4f, "a full-depth hit tears half the radius");
        checkNear(sb::depthFromSpeedKmh(36.0f), 36.0f * 27.78f * 0.01f, 1e-4f, "km/h converts through cm/s");
    }

    AVER_INFO("building a sheet");
    {
        sb::Cage c;
        sb::GridSpec g;
        g.cols = 5; g.rows = 5;
        g.du = Vec3{10, 0, 0}; g.dv = Vec3{0, 0, 10};
        const std::vector<u32> idx = sb::addGrid(c, g);
        check(idx.size() == 25 && c.particles.size() == 25, "25 particles");
        check(c.triangles.size() == 32, "32 triangles");
        check(c.beams.size() == 56, "56 beams (40 axis-aligned, 16 diagonals)");
        check(sb::addBeam(c, idx[0], idx[1]) == sb::addBeam(c, idx[1], idx[0]), "a repeated pair is the same beam");
        check(sb::addBeam(c, 3, 3) == sb::kNone && sb::addBeam(c, 0, 99) == sb::kNone, "bad pairs are refused");
        check(sb::addTriangle(c, 0, 0, 1) == sb::kNone, "a degenerate triangle is refused");
        sb::build(c);
        check(c.built && c.baseParticleCount == 25 && c.triDead.size() == 32, "build freezes it");
        check(c.bends.size() == 40, "40 interior edges make 40 bends");
        check(sb::addParticle(c, Vec3{}) == 25 && c.particles.size() == 26, "a particle can still be appended (not a beam)");
        c.particles.pop_back();
        check(sb::addBeam(c, 0, 24) == sb::kNone, "but a built cage takes no new beams");
        f32 mass = 0.0f;
        sb::recomputeMasses(c);
        for (const sb::Particle& p : c.particles) mass += p.mass;
        checkNear(mass, 0.16f, 1e-4f, "masses sum to 1 kg/m2 x 0.16 m2");
        c.particles[0].pinned = true;
        sb::recomputeMasses(c);
        check(c.particles[0].invMass == 0.0f && c.particles[1].invMass > 0.0f, "a pinned particle has no inverse mass");
        checkNear(sb::maxStrain(c), 0.0f, 1e-6f, "at rest there is no strain");
        c.particles[1].pos.x += 5.0f;
        checkNear(sb::maxStrain(c), 0.5f, 1e-3f, "a beam stretched by half its length reads 0.5");
        c.particles[1].pos.x -= 5.0f;
    }

    AVER_INFO("material classification and the sim-facing material fields");
    {
        sb::Cage c;
        sb::Material glass = sb::presetMaterial(sb::SoftPreset::Glass);
        const u32 m = sb::addMaterial(c, glass);
        const u32 a = sb::addParticle(c, Vec3{0, 0, 0}), b = sb::addParticle(c, Vec3{10, 0, 0}), d = sb::addParticle(c, Vec3{0, 0, 10});
        sb::addTriangle(c, a, b, d, m);
        sb::build(c);
        check(c.particles[a].brittle && c.particles[b].brittle, "a glass cage's particles are brittle");
        check(c.beams[0].material == m, "beams carry their material");
        check(sb::isBrittle(sb::Behavior::Fracture) && !sb::isBrittle(sb::Behavior::Deform), "isBrittle");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
