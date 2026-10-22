// The framework relay's provider (framework_softshape_abi.h): every op forwards to the shape system. The ops are between-step
// ABI calls (an attach, a policy, a buoyancy setting) and run at once, so a call that returns a result (a grab id, a cut's new
// entity) can return it; they apply in arrival order, which is what a replay needs.
#include "aver/softbody/SoftShapeSystem.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

// AVER_FW_SS_OP_* (framework_softshape_abi.h).
constexpr i32 kOpPressure = 1, kOpVolume = 2, kOpGrab = 3, kOpMoveGrab = 4, kOpReleaseGrab = 5, kOpBuoyancy = 6, kOpSubmerged = 7,
              kOpContact = 8, kOpPairMode = 9, kOpCutRope = 10, kOpCreateRope = 11, kOpSensorEvents = 12;

bool allFinite(const float* in, i32 n) {
    for (i32 i = 0; i < n; ++i)
        if (!std::isfinite(in[i])) return false;
    return true;
}

}  // namespace

i32 softShapeProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user) {
    if (!user) return 0;
    SoftShapeSystem& sys = *static_cast<SoftShapeSystem*>(user);
    scene::World& world = scene::World::instance();
    const scene::Entity e = static_cast<scene::Entity>(entity);
    if (inCount > 0 && (!in || !allFinite(in, inCount))) return 0;
    auto arg = [&](i32 i) { return in && i < inCount ? in[i] : 0.0f; };
    switch (op) {
        case kOpPressure:
            return inCount >= 1 && sys.setPressure(e, arg(0)) ? 1 : 0;
        case kOpVolume: {
            f32 rest = 0.0f, now = 0.0f;
            if (!sys.volumeOf(e, rest, now)) return 0;
            if (outF && cap >= 2) { outF[0] = rest; outF[1] = now; }
            return 1;
        }
        case kOpGrab: {
            if (inCount < 7) return 0;
            const i32 id = sys.grab(e, Vec3{arg(0), arg(1), arg(2)}, arg(3), Vec3{arg(4), arg(5), arg(6)});
            if (id <= 0) return 0;
            if (outI && cap > 0) outI[0] = id;
            return 1;
        }
        case kOpMoveGrab:
            return inCount >= 4 && sys.moveGrab(e, static_cast<i32>(std::lround(arg(0))), Vec3{arg(1), arg(2), arg(3)}) ? 1 : 0;
        case kOpReleaseGrab:
            return inCount >= 1 && sys.releaseGrab(e, static_cast<i32>(std::lround(arg(0)))) ? 1 : 0;
        case kOpBuoyancy:
            return inCount >= 2 && sys.setBuoyancy(e, arg(0), arg(1)) ? 1 : 0;
        case kOpSubmerged: {
            f32 fraction = 0.0f, volume = 0.0f;
            if (!sys.submerged(e, fraction, volume)) return 0;
            if (outF && cap >= 2) { outF[0] = fraction; outF[1] = volume; }
            return 1;
        }
        case kOpContact:
            return inCount >= 2 && sys.setContact(e, arg(0), arg(1)) ? 1 : 0;
        case kOpPairMode:
            return inCount >= 2 && sys.setPairMode(e, static_cast<scene::Entity>(std::max(0l, std::lround(arg(0)))), static_cast<i32>(std::lround(arg(1)))) ? 1 : 0;
        case kOpCutRope: {
            if (inCount < 3) return 0;
            scene::Entity other = scene::kInvalidEntity;
            if (!sys.cutRope(world, e, Vec3{arg(0), arg(1), arg(2)}, other)) return 0;
            if (outI && cap > 0) outI[0] = static_cast<i32>(other);
            return 1;
        }
        case kOpCreateRope: {
            if (inCount < 8 || arg(6) < 0.0f || arg(7) < 0.0f) return 0;
            const scene::Entity made = sys.createRope(world, Vec3{arg(0), arg(1), arg(2)}, Vec3{arg(3), arg(4), arg(5)}, arg(6), arg(7));
            if (made == scene::kInvalidEntity) return 0;
            if (outI && cap > 0) outI[0] = static_cast<i32>(made);
            return 1;
        }
        case kOpSensorEvents: {
            if (!outI || cap <= 0) return 0;
            std::vector<SoftSensorEvent> buf(static_cast<usize>(cap));
            const u32 n = sys.drainSensorEvents(buf.data(), static_cast<u32>(cap));
            for (u32 k = 0; k < n; ++k) {
                outI[k * 3 + 0] = static_cast<i32>(buf[k].soft);
                outI[k * 3 + 1] = static_cast<i32>(buf[k].sensor);
                outI[k * 3 + 2] = buf[k].entered ? 1 : 0;
            }
            return static_cast<i32>(n);
        }
        default: return 0;
    }
}

} // namespace aver::softbody
