// The damage step on positions the host mirrored in: yield creep, snap, energy deposits, cuts, repair.
// Pure arithmetic; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/Damage.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static sb::Material baseMaterial() {
    sb::Material m;
    m.yieldStrain = 0.01f;
    m.maxBend = 1e9f;
    m.breakStrain = 0.0f;
    m.bendForceN = 1e9f;
    m.breakForceN = 1e9f;
    return m;
}

// One 10 cm beam along +X.
static sb::Cage oneBeam(const sb::Material& m) {
    sb::Cage c;
    const u32 mat = sb::addMaterial(c, m);
    const u32 a = sb::addParticle(c, Vec3{0, 0, 0});
    const u32 b = sb::addParticle(c, Vec3{10, 0, 0});
    sb::addBeam(c, a, b, mat);
    sb::build(c);
    return c;
}

static sb::DamageConfig fastCreep() {
    sb::DamageConfig cfg;
    cfg.dt = 0.125f;       // creepRate 8 x 0.125 = 1: the whole excess per call
    return cfg;
}

static sb::Cage grid(const sb::Material& m, u32 cols = 5, u32 rows = 5) {
    sb::Cage c;
    sb::GridSpec g;
    g.cols = cols; g.rows = rows;
    g.du = Vec3{10, 0, 0}; g.dv = Vec3{0, 0, 10};
    g.material = sb::addMaterial(c, m);
    sb::addGrid(c, g);
    sb::build(c);
    sb::recomputeMasses(c);
    return c;
}

static Vec3 mid(const sb::Cage& c, const sb::Beam& b) { return (c.particles[b.a].pos + c.particles[b.b].pos) * 0.5f; }

int main() {
    AVER_INFO("DamageTest");

    AVER_INFO("yield creep keeps the stretch");
    {
        sb::Cage c = oneBeam(baseMaterial());
        c.particles[1].pos.x = 10.15f;   // 1.5x the 0.01 yield strain
        for (int i = 0; i < 40; ++i) sb::evaluateStrain(c, fastCreep());
        check(approx(c.beams[0].rest, 10.05f), "rest creeps toward the stretched length minus the yield");
        check(approx(c.beams[0].plastic, 0.05f), "the set accumulates (0.05 cm)");
        c.particles[1].pos.x = 10.0f;
        const sb::DamageResult r = sb::evaluateStrain(c, fastCreep());
        check(r.dirtyBeams.empty() && approx(c.beams[0].rest, 10.05f), "after release the dent stays and nothing more changes");
        check(!c.beams[0].broken, "nothing snapped");
    }

    AVER_INFO("below yield nothing changes");
    {
        sb::Cage c = oneBeam(baseMaterial());
        c.particles[1].pos.x = 10.05f;   // strain 0.005
        const sb::DamageResult r = sb::evaluateStrain(c, fastCreep());
        check(r.dirtyBeams.empty() && r.yielded == 0 && c.beams[0].rest == c.beams[0].rest0, "elastic");
        check(approx(r.maxStrain, 0.005f, 1e-4f), "maxStrain reports it");
    }

    AVER_INFO("hardening raises the yield");
    {
        sb::Material hard = baseMaterial();
        hard.hardening = 1.0f;
        sb::Cage c = oneBeam(hard);
        c.beams[0].plastic = 0.05f;
        c.particles[1].pos.x = 10.12f;   // strain 0.012: over 0.01, under 0.01 + 0.05 / 10
        check(sb::evaluateStrain(c, fastCreep()).dirtyBeams.empty(), "hardened: no creep at 0.012");
        sb::Cage soft = oneBeam(baseMaterial());
        soft.beams[0].plastic = 0.05f;
        soft.particles[1].pos.x = 10.12f;
        check(sb::evaluateStrain(soft, fastCreep()).dirtyBeams.size() == 1, "perfectly plastic: creeps at 0.012");
    }

    AVER_INFO("each snap rule");
    {
        sb::Material br = baseMaterial();
        br.breakStrain = 0.1f;
        sb::Cage c = oneBeam(br);
        c.particles[1].pos.x = 12.0f;
        sb::DamageResult r = sb::evaluateStrain(c, fastCreep());
        check(c.beams[0].broken && r.broken == 1 && r.brokenBeams.size() == 1 && c.topologyDirty, "breakStrain snaps and flags the topology");

        sb::Material cap = baseMaterial();
        cap.maxSetFrac = 0.01f;   // 0.1 cm on a 10 cm beam
        c = oneBeam(cap);
        c.particles[1].pos.x = 10.5f;
        r = sb::evaluateStrain(c, fastCreep());
        check(c.beams[0].broken, "accumulated set past maxSetFrac snaps");

        sb::Material abs = baseMaterial();
        abs.maxBend = 0.1f;
        c = oneBeam(abs);
        c.particles[1].pos.x = 10.5f;
        sb::evaluateStrain(c, fastCreep());
        check(c.beams[0].broken, "accumulated set past maxBend (cm) snaps");

        sb::Material brittle = baseMaterial();
        brittle.behavior = sb::Behavior::Fracture;
        brittle.yieldStrain = 0.02f;
        c = oneBeam(brittle);
        c.particles[1].pos.x = 10.1f;   // 0.01: under
        sb::evaluateStrain(c, fastCreep());
        check(!c.beams[0].broken && c.beams[0].rest == c.beams[0].rest0, "brittle under its yield neither snaps nor creeps");
        c.particles[1].pos.x = 10.3f;   // 0.03: over
        sb::evaluateStrain(c, fastCreep());
        check(c.beams[0].broken, "brittle at its yield snaps");

        sb::DamageConfig inert = fastCreep();
        inert.tearable = false;
        sb::Material everything = brittle;
        everything.breakStrain = 0.01f;
        everything.maxSetFrac = 0.001f;
        c = oneBeam(everything);
        c.particles[1].pos.x = 30.0f;
        r = sb::evaluateStrain(c, inert);
        check(!c.beams[0].broken && r.broken == 0, "tearable = false never snaps");

        sb::Material deform = baseMaterial();
        deform.maxSetFrac = 0.001f;
        c = oneBeam(deform);
        c.particles[1].pos.x = 20.0f;
        sb::evaluateStrain(c, inert);
        check(!c.beams[0].broken && c.beams[0].plastic > 0.0f, "an untearable cloth still creeps");
    }

    AVER_INFO("energy deposits");
    {
        sb::Cage c = grid(baseMaterial());
        sb::DamageDeposit d;
        d.point = Vec3{20, 0, 20}; d.radiusCm = 15.0f; d.energyJ = 10.0f;
        const sb::DepositResult r = sb::depositEnergy(c, d, 0.6f, 1e9f);
        f32 mass = 0.0f;
        for (const sb::Particle& p : c.particles)
            if (distSquared(p.pos, d.point) < d.radiusCm * d.radiusCm) mass += p.mass;
        check(approx(r.massKg, mass, 1e-6f) && mass > 0.0f, "the kick mass is the particles inside the radius");
        check(approx(r.kickSpeedCmS, std::sqrt(2.0f * 0.6f * 10.0f / mass) * 100.0f, 0.05f), "the kick is sqrt(2 e E / M) in cm/s");
        f32 sum = 0.0f;
        for (const sb::Beam& b : c.beams) sum += b.damage;
        check(approx(sum, 10.0f, 1e-3f) && r.beamsDamaged > 0, "the energy is shared out whole over the beams in reach");
        check(r.broken == 0 && sb::brokenBeamCount(c) == 0, "a material with no toughness and no brittleness does not break");
        sb::DamageDeposit capped = d;
        const sb::DepositResult low = sb::depositEnergy(c, capped, 0.6f, 5.0f);
        check(approx(low.kickSpeedCmS, 5.0f), "the kick is capped at maxKick");
        const sb::DepositResult none = sb::depositEnergy(c, sb::DamageDeposit{Vec3{500, 0, 500}, Vec3{}, 10.0f, 10.0f, 0}, 0.6f, 1e9f);
        check(none.beamsDamaged == 0 && none.kickSpeedCmS == 0.0f, "a deposit out of reach does nothing");
    }

    AVER_INFO("toughness breaks exactly the beams whose damage passes it");
    {
        sb::Material tough = baseMaterial();
        tough.toughnessJ = 1.0f;
        sb::Cage c = grid(tough);
        sb::DamageDeposit d;
        d.point = Vec3{20, 0, 20}; d.radiusCm = 25.0f; d.energyJ = 40.0f;
        const sb::DepositResult r = sb::depositEnergy(c, d, 0.6f, 1e9f);
        bool exact = true;
        u32 passed = 0;
        for (const sb::Beam& b : c.beams) {
            const bool over = b.damage >= 1.0f;
            passed += over ? 1u : 0u;
            exact = exact && (b.broken == over);
        }
        check(exact && passed > 0 && passed < c.beams.size(), "broken exactly when damage >= toughness (some, not all)");
        check(r.broken == passed && r.brokenBeams.size() == passed && c.topologyDirty, "the result lists them");
        sb::Cage gentle = grid(tough);
        d.energyJ = 0.01f;
        const sb::DepositResult g = sb::depositEnergy(gentle, d, 0.6f, 1e9f);
        check(g.broken == 0 && g.beamsDamaged > 0, "a gentle deposit damages without breaking");
        sb::Material glass = sb::presetMaterial(sb::SoftPreset::Glass);
        sb::Cage pane = grid(glass);
        d.energyJ = 50.0f;
        check(sb::depositEnergy(pane, d, 0.6f, 1e9f).broken > 0, "a brittle preset breaks under a hit");
    }

    AVER_INFO("falloff is monotone");
    {
        sb::Cage c = grid(baseMaterial(), 8, 2);
        sb::DamageDeposit d;
        d.point = Vec3{0, 0, 5}; d.radiusCm = 80.0f; d.energyJ = 1.0f;
        sb::depositEnergy(c, d, 0.6f, 1e9f);
        bool monotone = true;
        u32 compared = 0;
        for (const sb::Beam& i : c.beams)
            for (const sb::Beam& j : c.beams) {
                const f32 di = dist(mid(c, i), d.point), dj = dist(mid(c, j), d.point);
                if (di < dj - 1e-3f && j.damage > 0.0f) { ++compared; monotone = monotone && i.damage >= j.damage - 1e-9f; }
            }
        check(monotone && compared > 10, "a beam nearer the hit never takes less than a farther one");
    }

    AVER_INFO("replays are bit identical");
    {
        auto run = [] {
            sb::Material m = baseMaterial();
            m.toughnessJ = 2.0f;
            sb::Cage c = grid(m, 6, 6);
            sb::DamageDeposit d;
            d.point = Vec3{25, 0, 25}; d.radiusCm = 30.0f; d.energyJ = 30.0f;
            sb::depositEnergy(c, d, 0.6f, 1e9f);
            c.particles[7].pos.x += 2.0f;
            sb::evaluateStrain(c, sb::DamageConfig{});
            std::vector<f32> bits;
            for (const sb::Beam& b : c.beams) { bits.push_back(b.damage); bits.push_back(b.rest); bits.push_back(b.plastic); bits.push_back(b.broken ? 1.0f : 0.0f); }
            return bits;
        };
        const std::vector<f32> a = run(), b = run();
        check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0, "the same inputs give the same bits");
    }

    AVER_INFO("scripted cuts and repair");
    {
        sb::Cage c = grid(baseMaterial());
        const u32 beams = static_cast<u32>(c.beams.size());
        const u32 bends = static_cast<u32>(c.bends.size());
        const Vec3 p{20, 0, 20};
        u32 expect = 0;
        for (const sb::Beam& b : c.beams) expect += distSquared(mid(c, b), p) <= 12.0f * 12.0f ? 1u : 0u;
        std::vector<u32> ids;
        const u32 n = sb::tearWithin(c, p, 12.0f, &ids);
        check(n == expect && n > 0 && ids.size() == n, "tearWithin breaks the beams whose midpoint is in radius");
        check(sb::tearWithin(c, p, 12.0f) == 0, "a second cut finds nothing new");
        sb::resolveTears(c);
        c.particles[3].pos.x += 5.0f;
        sb::repairAll(c);
        bool clean = c.particles.size() == c.baseParticleCount && c.beams.size() == beams && c.bends.size() == bends && !c.topologyDirty;
        for (const sb::Beam& b : c.beams) clean = clean && !b.broken && b.damage == 0.0f && b.rest == b.rest0 && b.plastic == 0.0f;
        for (const u8 dead : c.triDead) clean = clean && dead == 0;
        check(clean, "repairAll restores particles, beams, bends and dead triangles");
        check(approx(c.particles[3].pos.x, c.particles[3].rest.x), "and the positions");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
