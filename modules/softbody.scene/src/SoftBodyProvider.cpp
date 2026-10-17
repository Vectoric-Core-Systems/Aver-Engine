// The framework relay's provider (framework_softbody_abi.h): every op forwards to the system's inbox, so a
// C# or graph call is deferred to the next tick and applies in arrival order.
#include "aver/softbody/SoftBodySystem.hpp"

#include <algorithm>

namespace aver::softbody {
namespace {

// Operation ids and argument layout (framework_softbody_abi.h, AVER_FW_SB_OP_*).
constexpr i32 kOpDamage = 1, kOpExplode = 2, kOpTear = 3, kOpRepair = 4, kOpRelease = 5, kOpWind = 6, kOpBudget = 7,
              kOpPreset = 8, kOpState = 9, kOpEvents = 10;

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
        default: return 0;
    }
}

}  // namespace aver::softbody
