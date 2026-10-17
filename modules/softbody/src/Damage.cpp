#include "aver/softbody/Damage.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

Vec3 midpoint(const Cage& c, const Beam& b) {
    return (c.particles[b.a].pos + c.particles[b.b].pos) * 0.5f;
}

} // namespace

DamageResult evaluateStrain(Cage& c, const DamageConfig& cfg) {
    DamageResult res;
    if (!c.built) return res;
    const f32 creepStep = std::clamp(cfg.creepRate * cfg.dt, 0.0f, 1.0f);
    for (u32 bi = 0; bi < c.beams.size(); ++bi) {
        Beam& b = c.beams[bi];
        if (b.broken || b.rest0 <= 0.0f) continue;
        const Material& m = c.materials[b.material];
        const f32 len = dist(c.particles[b.a].pos, c.particles[b.b].pos);
        const f32 strain = (len - b.rest) / b.rest0;
        const f32 absStrain = std::fabs(strain);
        b.peak = std::max(b.peak, absStrain);
        res.maxStrain = std::max(res.maxStrain, absStrain);

        const f32 baseYield = m.yieldStrain > 0.0f ? m.yieldStrain
                                                    : m.bendForceN / (std::max(m.axialStiffness, 1e-6f) * b.rest0);
        // Hardening raises the yield with the set so far, as a strain (set / rest0).
        const f32 yield = baseYield + m.hardening * b.plastic / b.rest0;
        const bool brittle = isBrittle(m.behavior);
        const f32 setCap = std::min(m.maxBend, m.maxSetFrac > 0.0f ? m.maxSetFrac * b.rest0 : 1e30f);

        bool snap = false;
        if (cfg.tearable) {
            if (m.breakStrain > 0.0f && strain > m.breakStrain) snap = true;
            else if (brittle && absStrain > yield) snap = true;
        }
        if (!snap && !brittle && absStrain > yield) {
            const f32 creep = (absStrain - yield) * b.rest0 * creepStep;
            b.rest = std::max(b.rest + (strain > 0.0f ? creep : -creep), 0.1f * b.rest0);
            b.plastic += creep;
            res.dirtyBeams.push_back(bi);
            ++res.yielded;
            if (cfg.tearable && b.plastic > setCap) snap = true;
        }
        if (snap) {
            b.broken = true;
            c.topologyDirty = true;
            res.brokenBeams.push_back(bi);
            ++res.broken;
        }
    }
    return res;
}

DepositResult depositEnergy(Cage& c, const DamageDeposit& d, f32 impulseEfficiency, f32 maxKickCmS) {
    DepositResult res;
    if (!c.built || d.radiusCm <= 0.0f || d.energyJ <= 0.0f) return res;
    const f32 r = d.radiusCm;
    const f32 rSq = r * r;

    // Falloff weights first, so the energy is shared by what is actually in reach.
    std::vector<f32> weight(c.beams.size(), 0.0f);
    f32 total = 0.0f;
    for (u32 bi = 0; bi < c.beams.size(); ++bi) {
        const Beam& b = c.beams[bi];
        if (b.broken) continue;
        const f32 d2 = distSquared(midpoint(c, b), d.point);
        if (d2 >= rSq) continue;
        const f32 f = 1.0f - std::sqrt(d2) / r;
        weight[bi] = f * f;
        total += weight[bi];
    }
    if (total > 0.0f) {
        for (u32 bi = 0; bi < c.beams.size(); ++bi) {
            if (weight[bi] <= 0.0f) continue;
            Beam& b = c.beams[bi];
            b.damage += d.energyJ * weight[bi] / total;
            ++res.beamsDamaged;
            const Material& m = c.materials[b.material];
            const bool breaks = (isBrittle(m.behavior) || m.toughnessJ > 0.0f) && b.damage >= m.toughnessJ;
            if (breaks) {
                b.broken = true;
                c.topologyDirty = true;
                res.brokenBeams.push_back(bi);
                ++res.broken;
            }
        }
    }

    f32 mass = 0.0f, pinnedMass = 0.0f;
    for (const Particle& p : c.particles) {
        if (distSquared(p.pos, d.point) >= rSq) continue;
        (p.pinned ? pinnedMass : mass) += p.mass;
    }
    res.massKg = mass > 0.0f ? mass : pinnedMass;
    if (res.massKg > 0.0f)
        res.kickSpeedCmS = std::min(maxKickCmS, std::sqrt(2.0f * impulseEfficiency * d.energyJ / res.massKg) * 100.0f);
    return res;
}

u32 tearWithin(Cage& c, Vec3 point, f32 radiusCm, std::vector<u32>* brokenBeams) {
    if (!c.built || radiusCm <= 0.0f) return 0;
    const f32 rSq = radiusCm * radiusCm;
    u32 n = 0;
    for (u32 bi = 0; bi < c.beams.size(); ++bi) {
        Beam& b = c.beams[bi];
        if (b.broken || distSquared(midpoint(c, b), point) > rSq) continue;
        breakBeam(c, bi);
        if (brokenBeams) brokenBeams->push_back(bi);
        ++n;
    }
    return n;
}

void repairAll(Cage& c) {
    repair(c);
    recomputeMasses(c);
}

} // namespace aver::softbody
