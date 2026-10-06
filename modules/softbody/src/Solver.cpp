// The step: Verlet -> damage/plasticity -> Gauss-Seidel PBD, per substep, in that order.
//
// Float evaluation order is part of the contract (a replayed impact must give the same bits), so
// edit the arithmetic with the same care as a file format.
#include "aver/softbody/Cage.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {

StepResult step(Cage& c, const StepConfig& cfg) {
    StepResult res;
    if (!c.built) return res;

    const i32 subs  = std::max(1, cfg.substeps);
    const i32 iters = std::max(1, cfg.iterations);
    const f32 subDt = cfg.dt / static_cast<f32>(subs);
    const f32 maxStep = cfg.maxNodeSpeed * subDt;
    const f32 maxStepSq = maxStep * maxStep;
    const f32 rateStep = std::clamp(cfg.damageRate * subDt, 0.0f, 1.0f);
    const Vec3 gravityStep = cfg.gravity * (subDt * subDt);
    f32 maxMoveSq = 0.0f;

    for (i32 sub = 0; sub < subs; ++sub) {
        std::vector<Particle>& ps = c.particles;
        const usize n = ps.size();
        c.scratchStart.resize(n);
        for (usize i = 0; i < n; ++i) c.scratchStart[i] = ps[i].pos;

        // Phase 1: Verlet. A pinned particle is kinematic: the host moved it, so it has no velocity.
        for (Particle& p : ps) {
            if (p.pinned) { p.prev = p.pos; continue; }
            Vec3 vel = (p.pos - p.prev) * cfg.velocityDamping;
            const f32 vsq = vel.sizeSquared();
            if (vsq > maxStepSq) vel = vel.getSafeNormal() * maxStep;
            p.prev = p.pos;
            p.pos += vel;
            if (cfg.gravityAll || p.freed) p.pos += gravityStep;
        }

        // Phase 2: damage. Reads the stretch the integration (or an impact) just caused, before
        // relaxation hides it.
        if (cfg.enableDamage) {
            for (Beam& b : c.beams) {
                if (b.broken) continue;
                Particle& A = ps[b.a];
                Particle& B = ps[b.b];
                const Vec3 delta = B.pos - A.pos;
                const f32 len = delta.size();
                if (len < 1e-4f) continue;
                const Vec3 axis = delta / len;
                const Material& m = c.materials[b.material];
                const f32 stretch = len - b.rest;
                const f32 mag = std::fabs(m.axialStiffness * stretch);
                b.peak = std::max(b.peak, mag);
                const bool brittle = isBrittle(m.behavior);
                const f32 yieldN = m.bendForceN + m.hardening * m.plasticStiffness * b.plastic;

                bool snap = mag > m.breakForceN || (brittle && mag > m.bendForceN);
                if (!snap && m.breakStrain > 0.0f && b.rest0 > 0.0f)
                    snap = (len - b.rest0) / b.rest0 > m.breakStrain;

                if (!snap && !brittle && mag > yieldN) {
                    const f32 target = (mag - yieldN) / std::max(m.plasticStiffness, 1.0f);
                    const f32 creep = target * rateStep;
                    b.rest += (stretch > 0.0f ? creep : -creep);
                    b.rest = std::max(b.rest, 0.1f * b.rest0);
                    b.plastic += creep;
                    const f32 bleed = std::clamp(m.bendAbsorb * creep, 0.0f, 1.0f);
                    if (!A.pinned) A.prev = lerp(A.prev, A.pos, bleed);
                    if (!B.pinned) B.prev = lerp(B.prev, B.pos, bleed);
                    if (b.plastic > m.maxBend) snap = true;
                }

                if (snap) {
                    b.broken = true;
                    const f32 kick = std::clamp(1.0f - m.breakAbsorb, 0.0f, 1.0f) * cfg.breakKick;
                    A.prev += axis * kick;
                    B.prev -= axis * kick;
                    c.topologyDirty = true;
                    res.anyBreak = true;
                    ++res.broken;
                }
            }
        }

        // Phase 3: Gauss-Seidel distance projection.
        for (i32 it = 0; it < iters; ++it) {
            for (const Beam& b : c.beams) {
                if (b.broken) continue;
                Particle& A = ps[b.a];
                Particle& B = ps[b.b];
                const Vec3 delta = B.pos - A.pos;
                const f32 len = delta.size();
                if (len < 1e-4f) continue;
                const f32 diff = (len - b.rest) / len;
                const Vec3 corr = delta * (0.5f * c.materials[b.material].stiffness * diff);
                const f32 wA = A.pinned ? 0.0f : (cfg.useNodeMass ? A.invMass : 1.0f);
                const f32 wB = B.pinned ? 0.0f : (cfg.useNodeMass ? B.invMass : 1.0f);
                const f32 wSum = wA + wB;
                if (wSum < 1e-4f) continue;
                A.pos += corr * (2.0f * wA / wSum);
                B.pos -= corr * (2.0f * wB / wSum);
            }
        }

        for (usize i = 0; i < n; ++i) {
            if (ps[i].pinned) continue;
            maxMoveSq = std::max(maxMoveSq, distSquared(ps[i].pos, c.scratchStart[i]));
        }

        if (c.topologyDirty) res.newParticles += resolveTears(c);
    }

    res.maxMoveCm = std::sqrt(maxMoveSq);
    res.settled = res.maxMoveCm <= cfg.settleThresholdCm;
    return res;
}

ImpactResult applyImpact(Cage& c, const Impact& im, const CrushParams& crush) {
    ImpactResult res;
    const Vec3 dir = im.direction.getSafeNormal();
    if (!c.built || dir.sizeSquared() <= 0.0f || im.radiusCm <= 0.0f) return res;

    res.disp = crushCurve(im.depthCm, crush);
    const f32 radius = im.radiusCm;
    const f32 radiusSq = radius * radius;
    for (Particle& p : c.particles) {
        if (p.pinned) continue;
        const f32 d2 = distSquared(p.pos, im.point);
        if (d2 >= radiusSq) continue;
        ++res.touched;
        if (p.brittle) continue;   // carbon and glass crack, they do not dent
        const f32 falloff = 1.0f - std::sqrt(d2) / radius;
        p.prev -= dir * (res.disp * falloff);
    }

    res.tearRadius = impactTearRadius(res.disp, radius, crush.knee, im.tearRadiusFrac, im.tearMinDisp);
    if (res.tearRadius > 0.0f) {
        const f32 tearSq = res.tearRadius * res.tearRadius;
        for (Beam& b : c.beams) {
            if (b.broken || !isBrittle(c.materials[b.material].behavior)) continue;
            const Vec3 mid = (c.particles[b.a].pos + c.particles[b.b].pos) * 0.5f;
            if (distSquared(mid, im.point) > tearSq) continue;
            b.broken = true;
            c.topologyDirty = true;
            ++res.broken;
        }
    }
    return res;
}

} // namespace aver::softbody
