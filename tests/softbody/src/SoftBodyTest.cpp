// The plastic cage solver, with no GPU, no scene and no Jolt.
//
// Each property is one a screenshot cannot decide: a bar bent past its yield keeps the bend while an
// elastic twin springs back, a strip pulled past its limit tears into exactly two pieces (and only
// along the weak seam), a slit duplicates the particles on it and the render mapping follows, energy
// stays bounded at the fixed rate, and a replay is bitwise identical.
#include "TestCages.hpp"

#include "aver/core/Log.hpp"
#include "aver/softbody/RenderBinding.hpp"
#include "aver/softbody/softbody_abi.h"

#include <cmath>
#include <string>
#include <vector>

using namespace sbtest;

static int g_failures = 0;

// Records one assertion.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Records one assertion that two floats agree to `eps`.
static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

static f32 maxPlastic(const sb::Cage& c) {
    f32 worst = 0.0f;
    for (const sb::Beam& b : c.beams) worst = std::fmax(worst, b.plastic);
    return worst;
}

static u32 countDead(const sb::Cage& c) {
    u32 n = 0;
    for (const u8 d : c.triDead) n += d ? 1u : 0u;
    return n;
}

int main() {
    AVER_INFO("SoftBodyTest");
    const sb::StepConfig cfg;

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

    AVER_INFO("a bar bent past yield keeps its bend");
    {
        // 16 cm held at the tip is well past the steel yield; 4 cm is inside it.
        Truss elastic = makeTruss(elasticMaterial());
        bendTip(elastic, 16.0f, cfg);
        checkNear(elastic.cage.particles[elastic.tipTop()].pos.z - elastic.topZ0, 0.0f, 0.1f,
                  "the elastic control springs back to where it started");

        Truss steel = makeTruss(sb::Material{});
        bendTip(steel, 16.0f, cfg);
        const f32 set = steel.cage.particles[steel.tipTop()].pos.z - steel.topZ0;
        check(set < -3.0f, "the steel bar keeps a permanent set of several cm");
        check(set > -16.0f, "it springs back from the full 16 cm (the elastic part recovers)");
        check(maxPlastic(steel.cage) > 0.5f, "the rest lengths crept to carry it");
        check(sb::brokenBeamCount(steel.cage) == 0, "and nothing tore on the way");
        check(allFinite(steel.cage), "every position is finite");

        Truss gentleSteel = makeTruss(sb::Material{});
        bendTip(gentleSteel, 4.0f, cfg);
        checkNear(gentleSteel.cage.particles[gentleSteel.tipTop()].pos.z - gentleSteel.topZ0, 0.0f, 0.1f,
                  "below yield the same steel bar returns");
        checkNear(maxPlastic(gentleSteel.cage), 0.0f, 1e-6f, "and no beam crept");

        sb::repair(steel.cage);
        checkNear(steel.cage.particles[steel.tipTop()].pos.z - steel.topZ0, 0.0f, 1e-5f, "repair undoes the dent");
        checkNear(maxPlastic(steel.cage), 0.0f, 1e-6f, "and the creep");
        check(steel.cage.beams[0].rest == steel.cage.beams[0].rest0, "rest lengths return to as-authored");
    }

    AVER_INFO("an impact leaves a dent that stays");
    {
        sb::Impact im;
        im.point = Vec3{100, 0, 10};
        im.direction = Vec3{0, 0, -1};
        im.depthCm = 25.0f;
        im.radiusCm = 45.0f;

        Truss elastic = makeTruss(elasticMaterial());
        sb::applyImpact(elastic.cage, im);
        for (int i = 0; i < 600; ++i) sb::step(elastic.cage, cfg);
        const f32 elasticZ = elastic.cage.particles[elastic.tipTop()].pos.z - elastic.topZ0;
        check(elasticZ > -1.0f, "the elastic cage recovers from the hit");

        Truss steel = makeTruss(sb::Material{});
        const sb::ImpactResult r = sb::applyImpact(steel.cage, im);
        check(r.touched > 0, "the impact reached particles");
        for (int i = 0; i < 600; ++i) sb::step(steel.cage, cfg);
        const f32 steelZ = steel.cage.particles[steel.tipTop()].pos.z - steel.topZ0;
        check(steelZ < -5.0f, "the steel cage keeps a dent of more than 5 cm");
        check(allFinite(steel.cage), "and every position is finite");
    }

    AVER_INFO("a strip pulled past its limit tears into two pieces");
    {
        // 7 columns, the seam between 3 and 4 is perforated (12% against 50% elsewhere).
        Strip strip = makeStrip(7, 4, 3, 0.12f, 0.5f);
        check(sb::countPieces(strip.cage) == 1, "it starts as one piece");
        pullStrip(strip, 0.1f, 20, cfg);
        pullStrip(strip, 0.0f, 100, cfg);
        check(sb::brokenBeamCount(strip.cage) == 0 && sb::countPieces(strip.cage) == 1,
              "a 2 cm pull is inside the limit: nothing tears");

        Strip torn = makeStrip(7, 4, 3, 0.12f, 0.5f);
        pullStrip(torn, 0.3f, 150, cfg);
        check(sb::countPieces(torn.cage) == 2, "pulled past the limit it is exactly two pieces");
        check(sb::brokenBeamCount(torn.cage) == torn.seamBeams, "and exactly the seam beams broke");
        u32 offSeam = 0;
        for (const sb::Beam& b : torn.cage.beams)
            if (b.broken && b.material != 2) ++offSeam;
        check(offSeam == 0, "no beam outside the seam broke");
        check(countDead(torn.cage) == 2 * (torn.rows - 1), "the triangles spanning the gap are dead, none else");
        check(allFinite(torn.cage), "every position is finite");

        std::vector<u32> ids;
        sb::countPieces(torn.cage, &ids);
        check(ids[torn.ids[0]] != ids[torn.ids[torn.cols - 1]], "the two pinned ends are in different pieces");
    }

    AVER_INFO("a slit duplicates the particles on it and the render mapping follows");
    {
        sb::Cage c;
        sb::GridSpec g;
        g.cols = 5;
        g.rows = 4;
        const std::vector<u32> ids = sb::addGrid(c, g);
        sb::build(c);
        sb::RenderBinding rb = sb::RenderBinding::identity(c);
        const u32 before = static_cast<u32>(c.particles.size());
        check(before == 20 && rb.vertexCount() == 20, "a 5x4 sheet has 20 particles and 20 render vertices");

        // Cut along row 1.
        for (u32 x = 0; x + 1 < 5; ++x) sb::breakBeam(c, findBeam(c, ids[5 + x], ids[5 + x + 1]));
        check(sb::brokenBeamCount(c) == 4, "four beams cut");
        const u32 made = sb::resolveTears(c);
        check(made == 5, "all five particles on the cut are duplicated");
        check(c.particles.size() == before + 5, "the cage grew by five");
        check(sb::countPieces(c) == 2, "the sheet is two pieces");
        check(countDead(c) == 0, "a clean slit leaves no triangle spanning a gap");
        check(c.splitLog.size() == 5, "five split events were logged");

        std::vector<u32> sources;
        rb.applySplits(c.splitLog, &sources);
        check(rb.vertexCount() == 25 && sources.size() == 5, "five render vertices were duplicated");

        std::vector<u32> pieceOf;
        sb::countPieces(c, &pieceOf);
        bool sameSide = true;
        for (usize r = 0; r < rb.triSim.size(); ++r) {
            const u32 a = pieceOf[rb.vertexParticle[rb.indices[r * 3 + 0]]];
            const u32 b = pieceOf[rb.vertexParticle[rb.indices[r * 3 + 1]]];
            const u32 d = pieceOf[rb.vertexParticle[rb.indices[r * 3 + 2]]];
            if (a != b || b != d) sameSide = false;
        }
        check(sameSide, "every render triangle now lies wholly on one side of the cut");
        std::vector<u32> visible;
        rb.visibleIndices(c.triDead, visible);
        check(visible.size() == 24 * 3, "all 24 triangles are still drawn");

        // The two halves move apart: nothing holds them.
        sb::StepConfig fall;
        fall.gravityAll = true;
        for (const u32 p : ids) c.particles[p].pinned = false;
        for (u32 x = 0; x < 5; ++x) c.particles[ids[x]].pinned = true;   // hang the bottom row
        for (int i = 0; i < 200; ++i) sb::step(c, fall);
        f32 gap = 1e9f;
        for (u32 x = 0; x < 5; ++x) {
            const sb::Particle& lower = c.particles[ids[5 + x]];
            f32 best = 1e9f;
            for (u32 d = 20; d < c.particles.size(); ++d)
                if (c.particles[d].origin == ids[5 + x]) best = std::fmin(best, dist(lower.pos, c.particles[d].pos));
            gap = std::fmin(gap, best);
        }
        check(gap > 1.0f, "after stepping, a particle and its duplicate have separated");

        sb::repair(c);
        rb.reset();
        check(c.particles.size() == before && sb::countPieces(c) == 1, "repair stitches the sheet back");
        check(rb.vertexCount() == 20 && rb.indices == rb.indices0, "and the render mapping returns to its bound state");
    }

    AVER_INFO("a hit on brittle material cracks instead of denting");
    {
        sb::Cage c;
        sb::Material glass;
        glass.behavior = sb::Behavior::Shatter;
        sb::GridSpec g;
        g.cols = 9;
        g.rows = 9;
        g.material = sb::addMaterial(c, glass);
        const std::vector<u32> ids = sb::addGrid(c, g);
        sb::build(c);
        sb::Impact im;
        im.point = c.particles[ids[4 * 9 + 4]].pos;
        im.direction = Vec3{0, 1, 0};
        im.depthCm = 40.0f;
        im.radiusCm = 50.0f;
        const sb::ImpactResult r = sb::applyImpact(c, im);
        check(r.tearRadius > 0.0f && r.broken > 0, "beams within the tear radius snapped");
        checkNear(sb::kineticProxy(c), 0.0f, 1e-9f, "no particle was pushed: glass does not dent");
        sb::resolveTears(c);
        check(c.particles[ids[4 * 9 + 4]].freed, "the particle at the centre lost every beam and is freed");
        check(countDead(c) > 0, "the cracked triangles are dead");
        check(sb::countPieces(c) >= 2, "the hole is its own piece");
    }

    AVER_INFO("energy does not explode at the fixed rate");
    {
        sb::StepConfig fall;
        fall.gravityAll = true;
        sb::Cage flag = makeFlag(sb::Material{});
        f32 peak = 0.0f, worstMove = 0.0f, farthest = 0.0f;
        f32 energyAt256 = 0.0f;
        for (int i = 0; i < 512; ++i) {
            const sb::StepResult r = sb::step(flag, fall);
            peak = std::fmax(peak, sb::kineticProxy(flag));
            worstMove = std::fmax(worstMove, r.maxMoveCm);
            if (i == 255) energyAt256 = sb::kineticProxy(flag);
        }
        for (const sb::Particle& p : flag.particles) farthest = std::fmax(farthest, p.pos.size());
        const f32 maxStep = fall.maxNodeSpeed * fall.dt / static_cast<f32>(fall.substeps);
        check(allFinite(flag), "every position stays finite over eight seconds of swinging");
        check(worstMove < 2.0f * maxStep, "no particle ever exceeds twice the per-substep speed limit");
        check(peak < 200.0f, "the kinetic proxy stays bounded");
        check(energyAt256 < 0.01f * peak, "and it has died away by four seconds");
        check(farthest < 1.05f * 99.0f, "the sheet stays inside its own reach (no stretching away)");
        check(sb::maxStrain(flag) < 0.01f, "the settled sheet is within 1% of its rest lengths");
        check(sb::brokenBeamCount(flag) == 0, "gravity alone breaks nothing");
    }

    AVER_INFO("the step is deterministic");
    {
        sb::StepConfig fall;
        fall.gravityAll = true;
        sb::Cage a = makeFlag(sb::Material{});
        sb::Cage b = makeFlag(sb::Material{});
        for (int i = 0; i < 200; ++i) { sb::step(a, fall); sb::step(b, fall); }
        check(bitwiseEqual(positionsOf(a), positionsOf(b)), "two runs of the same cage give identical bits");

        Truss t1 = makeTruss(sb::Material{});
        Truss t2 = makeTruss(sb::Material{});
        sb::Impact im;
        im.point = Vec3{100, 0, 10};
        im.depthCm = 25.0f;
        sb::applyImpact(t1.cage, im);
        sb::applyImpact(t2.cage, im);
        for (int i = 0; i < 100; ++i) { sb::step(t1.cage, cfg); sb::step(t2.cage, cfg); }
        check(bitwiseEqual(positionsOf(t1.cage), positionsOf(t2.cage)), "and so does a replayed impact");
    }

    AVER_INFO("the C ABI drives the same solver");
    {
        const int32_t h = aver_sb_create();
        check(h > 0, "a handle is returned");
        AverSbMaterial m;
        aver_sb_default_material(&m);
        const int32_t mat = aver_sb_add_material(h, &m);
        const int32_t p0 = aver_sb_add_particle(h, 0, 0, 0, 1);
        const int32_t p1 = aver_sb_add_particle(h, 10, 0, 0, 0);
        const int32_t p2 = aver_sb_add_particle(h, 0, 0, 10, 0);
        check(aver_sb_add_triangle(h, p0, p1, p2, mat) == 0, "a triangle is added with its edges");
        check(aver_sb_step(h) == -1, "stepping before build is refused");
        check(aver_sb_build(h) == 1, "build succeeds");
        check(aver_sb_add_particle(h, 1, 1, 1, 0) == -1, "topology is frozen after build");
        check(aver_sb_step(h) >= 0, "a step runs");
        check(aver_sb_particle_count(h) == 3, "three particles");
        float xyz[9] = {};
        check(aver_sb_positions(h, xyz, 3) == 3 && std::fabs(xyz[3] - 10.0f) < 1e-3f, "positions read back");
        check(aver_sb_break_beam(h, 0) == 1, "a beam can be cut");
        aver_sb_step(h);
        check(aver_sb_broken_beams(h) == 1, "and it stays cut");
        check(aver_sb_repair(h) == 1 && aver_sb_broken_beams(h) == 0, "repair restores it");
        aver_sb_destroy(h);
        check(aver_sb_particle_count(h) == 0, "a destroyed handle is dead");
    }

    AVER_INFO("the C ABI says why a call failed");
    {
        const int32_t h = aver_sb_create();
        check(aver_sb_last_error() == 0, "a successful create leaves Ok");
        AverSbMaterial m;
        aver_sb_default_material(&m);
        const int32_t mat = aver_sb_add_material(h, &m);
        const int32_t p0 = aver_sb_add_particle(h, 0, 0, 0, 1);
        const int32_t p1 = aver_sb_add_particle(h, 10, 0, 0, 0);
        check(aver_sb_add_beam(h, p0, p1, mat) == 0 && aver_sb_last_error() == 0, "a good add leaves Ok");
        check(aver_sb_add_beam(h, p0, 99, mat) == -1 && aver_sb_last_error() == -4, "a particle past the end is OutOfRange");
        check(aver_sb_add_beam(h, p0, p0, mat) == -1 && aver_sb_last_error() == -6, "a beam to itself is InvalidArgument");
        check(aver_sb_step(h) == -1 && aver_sb_last_error() == -3, "step before build is NotInitialised");
        check(aver_sb_impact(h, 0, 0, 0, 0, 0, -1, 5.0f, 10.0f) == 0 && aver_sb_last_error() == -3,
              "an impact before build is NotInitialised");
        check(aver_sb_build(h) == 1 && aver_sb_last_error() == 0, "build leaves Ok");
        check(aver_sb_add_particle(h, 1, 1, 1, 0) == -1 && aver_sb_last_error() == -5, "adding after build is Unsupported");
        check(aver_sb_move_particle(h, 99, 0, 0, 0) == 0 && aver_sb_last_error() == -4, "a particle past the end is OutOfRange");
        check(aver_sb_move_particle(h, p1, 5, 0, 0) == 1 && aver_sb_last_error() == 0, "a good move clears the reason");
        check(aver_sb_positions(h, nullptr, 2) == 0 && aver_sb_last_error() == -2, "a null out buffer is NullPointer");
        check(aver_sb_set_config(h, nullptr) == 0 && aver_sb_last_error() == -2, "a null config is NullPointer");
        check(aver_sb_async_poll(h, nullptr, 0, nullptr) == -1 && aver_sb_last_error() == -3,
              "polling with no worker is NotInitialised");
        aver_sb_destroy(h);
        check(aver_sb_step(h) == -1 && aver_sb_last_error() == -1, "a destroyed handle is BadHandle");
        check(aver_sb_repair(h) == 0 && aver_sb_last_error() == -1, "and so for every other call");
    }

    AVER_INFO(g_failures ? "SoftBodyTest: {} FAILURES" : "SoftBodyTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
