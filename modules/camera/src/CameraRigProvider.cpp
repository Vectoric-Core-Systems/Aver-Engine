// The framework relay's provider (framework_camera_abi.h): every op forwards to the system, so a C# or graph call is
// deferred to the next drive and applies in arrival order. Op ids and the argument packing mirror the header; the
// module does not link the framework, so they are restated here (AbiEnumTest pins the C# side; the packing is in the
// header: integer arguments travel as the bit pattern of an int32).
#include "aver/camera/CameraRigSystem.hpp"

#include <cstring>

namespace aver::camera {
namespace {

constexpr i32 kOpAttach = 1, kOpSetViewTarget = 2, kOpClearViewTarget = 3, kOpPushModifier = 4, kOpRemoveModifier = 5,
              kOpClearModifiers = 6, kOpShakeAt = 7, kOpPose = 8, kOpRigState = 9, kOpCut = 10;

i32 intAt(const float* in, i32 count, i32 i) {
    if (!in || i >= count) return 0;
    i32 v;
    std::memcpy(&v, &in[i], sizeof v);
    return v;
}
f32 floatAt(const float* in, i32 count, i32 i) { return in && i < count ? in[i] : 0.0f; }

BlendCurve curveFrom(i32 v) { return v >= 0 && v <= static_cast<i32>(BlendCurve::EaseOut) ? static_cast<BlendCurve>(v) : BlendCurve::EaseInOut; }

}  // namespace

i32 cameraRigProvider(i32 op, i32 entity, const float* in, i32 inCount, float* outF, i32* outI, i32 cap, void* user) {
    if (!user) return 0;
    CameraRigSystem& sys = *static_cast<CameraRigSystem*>(user);
    const scene::Entity e = static_cast<scene::Entity>(static_cast<u32>(entity));
    switch (op) {
        case kOpAttach: {
            if (inCount < 2 || !scene::World::instance().valid(e)) return 0;
            const i32 preset = intAt(in, inCount, 0);
            if (preset < 0 || preset >= static_cast<i32>(kCameraPresetCount)) return 0;
            const scene::Entity target = static_cast<scene::Entity>(static_cast<u32>(intAt(in, inCount, 1)));
            return CameraRigSystem::attach(scene::World::instance(), e, static_cast<CameraPreset>(preset), target) ? 1 : 0;
        }
        case kOpSetViewTarget: {
            if (inCount < 3) return 0;
            ViewBlend vb;
            vb.seconds = floatAt(in, inCount, 0);
            vb.curve = curveFrom(intAt(in, inCount, 1));
            vb.cut = intAt(in, inCount, 2) != 0;
            return sys.setViewTarget(entity == 0 ? scene::kInvalidEntity : e, vb) ? 1 : 0;
        }
        case kOpClearViewTarget: {
            if (inCount < 2) return 0;
            ViewBlend vb;
            vb.seconds = floatAt(in, inCount, 0);
            vb.curve = curveFrom(intAt(in, inCount, 1));
            return sys.clearViewTarget(vb) ? 1 : 0;
        }
        case kOpPushModifier: {
            if (inCount < 11) return 0;
            const i32 kind = intAt(in, inCount, 0);
            if (kind < 0 || kind >= static_cast<i32>(kModKindCount)) return 0;
            CameraModifier m;
            m.kind = static_cast<ModKind>(kind);
            m.priority = intAt(in, inCount, 1);
            m.inS = floatAt(in, inCount, 2);
            m.holdS = floatAt(in, inCount, 3);
            m.outS = floatAt(in, inCount, 4);
            m.a = floatAt(in, inCount, 5);
            m.b = floatAt(in, inCount, 6);
            m.c = floatAt(in, inCount, 7);
            m.d = floatAt(in, inCount, 8);
            m.tag = static_cast<u32>(intAt(in, inCount, 9));
            m.flags = static_cast<u32>(intAt(in, inCount, 10));
            const u32 id = sys.pushModifier(m);
            if (id == 0) return 0;
            if (outI && cap > 0) outI[0] = static_cast<i32>(id);
            return 1;
        }
        case kOpRemoveModifier: {
            if (inCount < 2) return 0;
            return sys.removeModifier(static_cast<u32>(intAt(in, inCount, 0)), floatAt(in, inCount, 1)) ? 1 : 0;
        }
        case kOpClearModifiers: {
            if (inCount < 1) return 0;
            const u32 n = sys.clearModifiers(static_cast<u32>(intAt(in, inCount, 0)));
            if (outI && cap > 0) outI[0] = static_cast<i32>(n);
            return 1;
        }
        case kOpShakeAt: {
            if (inCount < 6) return 0;
            sys.shakeAt(Vec3{floatAt(in, inCount, 0), floatAt(in, inCount, 1), floatAt(in, inCount, 2)},
                        floatAt(in, inCount, 3), floatAt(in, inCount, 4), floatAt(in, inCount, 5));
            return 1;
        }
        case kOpPose: {
            if (!sys.hasPose() || !outF || !outI || cap < 9) return 0;
            const CameraPov& p = sys.pose();
            const float v[9] = {p.pos.x, p.pos.y, p.pos.z, p.yaw, p.pitch, p.roll, p.fovYRad, p.nearCm, p.farCm};
            for (i32 i = 0; i < 9; ++i) outF[i] = v[i];
            outI[0] = static_cast<i32>(sys.activeRig());
            outI[1] = sys.blending() ? 1 : 0;
            return 1;
        }
        case kOpRigState: {
            f32 arm = 0;
            u32 state = 0;
            if (!sys.rigState(e, arm, state)) return 0;
            if (!outF || !outI || cap < 2) return 0;
            outF[0] = arm;
            outF[1] = sys.activeRig() == e ? sys.blendAlpha() : 0.0f;
            outI[0] = static_cast<i32>(state);
            return 1;
        }
        case kOpCut:
            return sys.cut(entity == 0 ? scene::kInvalidEntity : e) ? 1 : 0;
        default:
            return 0;
    }
}

}  // namespace aver::camera
