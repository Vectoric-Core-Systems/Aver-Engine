// The framework relays' providers (framework_softbody_abi.h, framework_softfield_abi.h): every op forwards to the system's
// inbox, so a C# or graph call is deferred to the next tick and applies in arrival order.
#include "aver/softbody/SoftBodySystem.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

// Operation ids and argument layout (framework_softbody_abi.h, AVER_FW_SB_OP_*).
constexpr i32 kOpDamage = 1, kOpExplode = 2, kOpTear = 3, kOpRepair = 4, kOpRelease = 5, kOpWind = 6, kOpBudget = 7,
              kOpPreset = 8, kOpState = 9, kOpEvents = 10, kOpAsleep = 11, kOpClearDebris = 12, kOpEffects = 13, kOpDents = 14, kOpRepairAll = 15;

// AVER_FW_SF_OP_* (framework_softfield_abi.h) and the spec array layout.
constexpr i32 kSfPulse = 1, kSfStart = 2, kSfStop = 3, kSfImpact = 4, kSfSpawn = 5;
constexpr i32 kSfSpecCount = 24;

// The 24-float spec of the soft-field ABI as a FieldSpec. False for a short array, a non-finite value, a kind or a shape out
// of range, or a negative extent. `lifetime` receives spec[21].
bool decodeSpec(const float* in, i32 inCount, FieldSpec& out, f32& lifetime) {
    if (!in || inCount < kSfSpecCount) return false;
    for (i32 i = 0; i < kSfSpecCount; ++i)
        if (!std::isfinite(in[i])) return false;
    const i32 kind = static_cast<i32>(std::lround(in[0])), shape = static_cast<i32>(std::lround(in[1]));
    if (kind < 0 || kind >= static_cast<i32>(kFieldKindCount) || shape < 0 || shape >= static_cast<i32>(kFieldShapeCount)) return false;
    if (in[7] < 0.0f || in[8] < 0.0f || in[9] < 0.0f) return false;
    FieldSpec f;
    f.kind = static_cast<FieldKind>(kind);
    f.shape = static_cast<FieldShape>(shape);
    const u32 targets = static_cast<u32>(std::max(0l, std::lround(in[2]))) & kFieldTargetAll;
    f.targets = targets ? targets : (f.kind == FieldKind::Kill ? kFieldTargetDebris : kFieldTargetAll);
    f.perSecond = in[3] > 0.5f;
    f.world = fieldFrameFrom(Vec3{in[4], in[5], in[6]}, Vec3{in[10], in[11], in[12]});
    f.extents = Vec3{in[7], in[8], in[9]};
    f.dir = Vec3{in[13], in[14], in[15]};
    f.strength = in[16];
    f.energyJ = in[17];
    f.falloff = in[18];
    f.turbulence = in[19];
    f.wakeCmS = in[20];
    lifetime = in[21];
    out = f;
    return true;
}

}  // namespace

u32 SoftBodySystem::damageNear(Vec3 point, f32 radiusCm, f32 energyJ, Vec3 dir, u8 kind) {
    u32 n = 0;
    for (const auto& [e, en] : entries_) {
        if (en.failed) continue;
        bool in = false;
        for (const Piece& p : en.pieces) {
            if (!p.alive || p.empty) continue;
            const Vec3 q{std::clamp(point.x, p.boundsMin.x, p.boundsMax.x), std::clamp(point.y, p.boundsMin.y, p.boundsMax.y),
                         std::clamp(point.z, p.boundsMin.z, p.boundsMax.z)};
            in = in || dist(q, point) <= radiusCm;
        }
        if (in && damage(e, point, radiusCm, energyJ, dir, kind)) ++n;
    }
    return n;
}

u32 SoftBodySystem::cutPreview(scene::Entity e, Vec3 point, f32 radiusCm) const {
    const Entry* en = find(e);
    if (!en || en->failed) return 0;
    u32 n = 0;
    for (const Beam& b : en->sim.cage.beams) {
        if (b.broken) continue;
        const Vec3 mid = (en->sim.cage.particles[b.a].pos + en->sim.cage.particles[b.b].pos) * 0.5f;
        n += distSquared(mid, point) <= radiusCm * radiusCm ? 1u : 0u;
    }
    return n;
}

i32 softBodyProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user) {
    if (!user) return 0;
    SoftBodySystem& sys = *static_cast<SoftBodySystem*>(user);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    auto arg = [&](i32 i) { return in && i < inCount ? in[i] : 0.0f; };
    switch (op) {
        case kOpDamage: {
            if (inCount < 9) return 0;
            const Vec3 point{arg(0), arg(1), arg(2)}, dir{arg(3), arg(4), arg(5)};
            const u8 kind = static_cast<u8>(std::clamp(static_cast<i32>(arg(8)), 0, 2));
            if (entity == 0) return sys.damageNear(point, arg(6), arg(7), dir, kind) > 0 ? 1 : 0;
            return sys.damage(e, point, arg(6), arg(7), dir, kind) ? 1 : 0;
        }
        case kOpExplode: {
            if (inCount < 6) return 0;
            const u32 touched = sys.explode(Vec3{arg(0), arg(1), arg(2)}, arg(3), arg(4), arg(5));
            if (outI && cap > 0) outI[0] = static_cast<i32>(touched);
            return 1;
        }
        case kOpTear: {
            if (inCount < 4) return 0;
            const Vec3 point{arg(0), arg(1), arg(2)};
            const u32 n = sys.cutPreview(e, point, arg(3));
            if (!sys.tearAt(e, point, arg(3))) return 0;
            if (outI && cap > 0) outI[0] = static_cast<i32>(n);
            return 1;
        }
        case kOpRepair:  return sys.repair(e) ? 1 : 0;
        case kOpRelease: return sys.release(e) ? 1 : 0;
        case kOpWind:
            if (inCount < 6) return 0;
            sys.setWind(Vec3{arg(0), arg(1), arg(2)}, arg(3), arg(4), arg(5));
            return 1;
        case kOpBudget: {
            if (inCount < 4) return 0;
            Budget b;
            b.maxPieces = static_cast<u32>(std::max(0.0f, arg(0)));
            b.maxParticles = static_cast<u32>(std::max(0.0f, arg(1)));
            b.maxRebuilds = static_cast<u32>(std::max(1.0f, arg(2)));
            b.debrisLifetimeS = arg(3);
            sys.setBudget(b);
            return 1;
        }
        case kOpPreset: return inCount >= 1 && sys.setPreset(e, static_cast<u32>(std::max(0.0f, arg(0)))) ? 1 : 0;
        case kOpState: {
            const SoftBodyState st = sys.state(e);
            if (!st.found) return 0;
            if (outF && cap >= 2) { outF[0] = st.damage01; outF[1] = st.maxStrain; }
            if (outI && cap >= 3) {
                outI[0] = static_cast<i32>(st.pieces);
                outI[1] = static_cast<i32>(st.brokenEdges);
                outI[2] = (st.awake ? 1 : 0) | (st.frozen ? 2 : 0);
            }
            return 1;
        }
        case kOpEvents: {
            if (!outI || !outF || cap <= 0) return 0;
            std::vector<SoftEvent> buf(static_cast<usize>(cap));
            const u32 n = sys.drainEvents(buf.data(), static_cast<u32>(cap));
            for (u32 k = 0; k < n; ++k) {
                const SoftEvent& ev = buf[k];
                outI[k * 5 + 0] = static_cast<i32>(ev.kind);
                outI[k * 5 + 1] = static_cast<i32>(ev.entity);
                outI[k * 5 + 2] = static_cast<i32>(ev.piece);
                outI[k * 5 + 3] = static_cast<i32>(ev.beam);
                outI[k * 5 + 4] = static_cast<i32>(ev.seq);
                outF[k * 4 + 0] = ev.point.x; outF[k * 4 + 1] = ev.point.y; outF[k * 4 + 2] = ev.point.z;
                outF[k * 4 + 3] = ev.value;
            }
            return static_cast<i32>(n);
        }
        case kOpAsleep:
            return inCount >= 1 && sys.setAsleep(e, arg(0) > 0.5f) ? 1 : 0;
        case kOpClearDebris:
            return sys.clearDebris(e) ? 1 : 0;
        case kOpEffects:
            if (inCount < 1) return 0;
            sys.setEffectsEnabled(arg(0) > 0.5f);
            return 1;
        case kOpDents: {
            const DentInfo d = sys.dents(e);
            if (!d.found) return 0;
            if (outF && cap >= 1) outF[0] = d.maxSetCm;
            if (outI && cap >= 2) { outI[0] = static_cast<i32>(d.dentedEdges); outI[1] = static_cast<i32>(d.debrisPieces); }
            return 1;
        }
        case kOpRepairAll: {
            const u32 n = sys.repairAll();
            if (outI && cap > 0) outI[0] = static_cast<i32>(n);
            return 1;
        }
        default: return 0;
    }
}

i32 softFieldProvider(i32 op, i32, const float* in, i32 inCount, float*, i32* outI, i32 cap, void* user) {
    if (!user) return 0;
    SoftBodySystem& sys = *static_cast<SoftBodySystem*>(user);
    FieldSpec spec;
    f32 life = 0.0f;
    switch (op) {
        case kSfPulse: {
            if (!decodeSpec(in, inCount, spec, life)) return 0;
            const u32 reach = sys.pulseField(spec);
            if (outI && cap > 0) outI[0] = static_cast<i32>(reach);
            return 1;
        }
        case kSfStart: {
            if (!decodeSpec(in, inCount, spec, life)) return 0;
            const u32 handle = sys.startField(spec, life);
            if (!handle) return 0;
            if (outI && cap > 0) outI[0] = static_cast<i32>(handle);
            return 1;
        }
        case kSfStop:
            return inCount >= 1 && in[0] >= 1.0f && sys.stopField(static_cast<u32>(in[0])) ? 1 : 0;
        case kSfImpact: {
            if (!in || inCount < 9) return 0;
            for (i32 i = 0; i < 9; ++i)
                if (!std::isfinite(in[i])) return 0;
            const u32 reach = sys.impact(Vec3{in[0], in[1], in[2]}, Vec3{in[3], in[4], in[5]}, in[6], in[7], in[8]);
            if (outI && cap > 0) outI[0] = static_cast<i32>(reach);
            return 1;
        }
        case kSfSpawn: {
            if (!decodeSpec(in, inCount, spec, life)) return 0;
            const scene::Entity e = sys.spawnFieldEntity(scene::World::instance(), spec, life);
            if (e == scene::kInvalidEntity) return 0;
            if (outI && cap > 0) outI[0] = static_cast<i32>(e);
            return 1;
        }
        default: return 0;
    }
}

}  // namespace aver::softbody
