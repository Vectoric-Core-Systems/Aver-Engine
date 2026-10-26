// buildVehicle: a VehicleSpec as physics ABI calls. See VehicleBuild.hpp for the contract.
//
// THE v1 CALLS COME FIRST AND UNCHANGED (create, add_wheel per wheel, set_engine, finish) with the arguments the
// traffic system's own builder used, so a preset-shaped spec drives exactly as before. The v2 calls are made only
// for what a spec sets beyond the defaults of the physics ABI, which are Jolt's.
#include "aver/vehicle/VehicleBuild.hpp"

#include "aver/vehicle/OcVehicle.hpp"

#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"

#include <algorithm>
#include <vector>

namespace aver::vehicle {

f32 springSagCm(f32 hz) {
    constexpr f32 kGravityCmS2 = 981.0f;
    const f32 omega = 2.0f * kPi * hz;
    return kGravityCmS2 / (omega * omega);
}

namespace {

bool isJoltAxle(const AxleDef& a) {
    return a.finalDrive == 3.42f && a.diff == DiffMode::LimitedSlip && a.lsd == 1.4f && a.split == 0.5f &&
           a.share == 0.0f && a.antiroll == 1000.0f;
}

// The physics ABI's differential mode: 0 open, 1 limited slip, 2 locked.
int diffCode(DiffMode m) {
    return m == DiffMode::Open ? 0 : m == DiffMode::LimitedSlip ? 1 : 2;
}

std::vector<f32> flatten(const std::vector<Vec2>& v, bool second) {
    std::vector<f32> out;
    out.reserve(v.size());
    for (const Vec2& p : v) out.push_back(second ? p.y : p.x);
    return out;
}

}  // namespace

bool buildVehicle(const VehicleSpec& s, const Transform& origin, BuiltVehicle& out, std::string& why) {
    if (!validate(s, why)) return false;
    if (!aver_phys_ready()) { why = "the physics world is not running"; return false; }

    const BodyDef& b = s.body;
    const i32 v = aver_phys_vehicle_create(b.half.x, b.half.y, b.half.z, b.clearance, b.com.x, b.com.y, b.com.z, b.massKg,
                                           origin.position.x, origin.position.y, origin.position.z,
                                           origin.rotation.x, origin.rotation.y, origin.rotation.z, origin.rotation.w);
    if (!v) { why = "the physics module refused the chassis"; return false; }
    auto bail = [&](const std::string& msg) {
        aver_phys_vehicle_destroy(v);
        why = msg;
        return false;
    };

    if (s.kind == Kind::Tracked && !aver_phys_vehicle_set_kind(v, AVER_PHYS_VEH_KIND_TRACKED)) return bail("the physics module refused a tracked vehicle");
    if (!s.boxes.empty()) {
        std::vector<f32> flat;
        for (const BoxDef& x : s.boxes) {
            flat.insert(flat.end(), {x.cx, x.cy, x.cz, x.hx, x.hy, x.hz});
        }
        if (!aver_phys_vehicle_set_chassis_boxes(v, flat.data(), static_cast<i32>(s.boxes.size()))) return bail("the physics module refused the chassis boxes");
    }

    f32 minX = 1.0e30f, maxX = -1.0e30f, maxSteer = 0.0f;
    for (usize wi = 0; wi < s.wheels.size(); ++wi) {
        const WheelDef& w = s.wheels[wi];
        const WheelClass& c = *findClass(s, w.wheelClass);
        // THE WHEEL'S ATTACHMENT IS THE TOP OF ITS SUSPENSION TRAVEL, and a loaded car is never hanging from it fully
        // extended: Jolt rests the spring at its MAX length, so the weight compresses it by the static sag and the wheel
        // centre settles one sag short of full droop. Lowering the attachment by the sag puts the settled wheel centre
        // at the hub height the spec states (the tyre bottom on the origin plane when the hub is one radius up).
        const f32 attachZ = w.hub.z + c.suspMax - springSagCm(c.suspHz);
        const f32 brake = w.brakeNm >= 0.0f ? w.brakeNm : c.brakeNm;
        const i32 added = aver_phys_vehicle_add_wheel(v, w.hub.x, w.hub.y, attachZ, c.radius, c.width, c.suspMin, c.suspMax, c.suspHz,
                                                      c.suspDamping, w.steerDeg, brake, w.handbrake ? c.handbrakeNm : 0.0f, w.driven ? 1 : 0);
        if (added < 0) return bail("the physics module refused wheel " + w.name);
        minX = std::min(minX, w.hub.x);
        maxX = std::max(maxX, w.hub.x);
        maxSteer = std::max(maxSteer, w.steerDeg);

        if (c.inertia != 0.9f || c.spinDamping != 0.2f) {
            const f32 p[5] = {c.inertia, c.spinDamping, 4.0f, 2.0f, 1.0f};
            if (!aver_phys_vehicle_set_wheel_ex(v, added, p, 5)) return bail("the physics module refused the wheel block of " + w.name);
        }
        if (const TyreCurve* t = c.tyre.empty() ? nullptr : findTyre(s, c.tyre)) {
            const std::vector<f32> ls = flatten(t->longitudinal, false), lm = flatten(t->longitudinal, true);
            const std::vector<f32> as = flatten(t->lateral, false), am = flatten(t->lateral, true);
            if (!aver_phys_vehicle_set_wheel_curves(v, added, ls.data(), lm.data(), static_cast<i32>(ls.size()),
                                                    as.data(), am.data(), static_cast<i32>(as.size())))
                return bail("the physics module refused the tyre curves of " + w.name);
        }
    }

    const EngineDef& e = s.engine;
    if (!aver_phys_vehicle_set_engine(v, e.torque, e.minRpm, e.maxRpm)) return bail("the physics module refused the engine");
    if (e.inertia > 0.0f || e.damping != 0.2f) {
        const f32 p[5] = {e.torque, e.minRpm, e.maxRpm, e.inertia, e.damping};
        if (!aver_phys_vehicle_set_engine_ex(v, p, 5)) return bail("the physics module refused the engine block");
    }
    if (!e.curve.empty()) {
        const std::vector<f32> xs = flatten(e.curve, false), ys = flatten(e.curve, true);
        if (!aver_phys_vehicle_set_torque_curve(v, xs.data(), ys.data(), static_cast<i32>(xs.size()))) return bail("the physics module refused the torque curve");
    }
    if (s.gearbox.present) {
        const GearboxDef& g = s.gearbox;
        const f32 p[6] = {g.switchTime, g.clutchRelease, g.latency, g.up, g.down, g.clutch};
        if (!aver_phys_vehicle_set_gearbox(v, g.manual ? 1 : 0, g.forward.data(), static_cast<i32>(g.forward.size()),
                                           g.reverse.data(), static_cast<i32>(g.reverse.size()), p, 6))
            return bail("the physics module refused the gearbox");
    }

    if (s.kind == Kind::Wheeled) {
        const i32 axleCount = aver_phys_vehicle_axle_count(v);
        for (usize i = 0; i < s.axles.size() && static_cast<i32>(i) < axleCount; ++i) {
            const AxleDef& a = s.axles[i];
            if (isJoltAxle(a)) continue;
            const f32 p[6] = {a.finalDrive, static_cast<f32>(diffCode(a.diff)), a.lsd, a.split, a.share, a.antiroll};
            if (!aver_phys_vehicle_set_axle(v, static_cast<i32>(i), p, 6)) return bail("the physics module refused axle " + std::to_string(i));
        }
        if (s.centerPresent && !(s.center == DiffMode::LimitedSlip && s.centerLsd == 1.4f)) {
            if (!aver_phys_vehicle_set_center_diff(v, diffCode(s.center), s.centerLsd)) return bail("the physics module refused the centre differential");
        }
    } else {
        for (int side = 0; side < 2; ++side) {
            const TrackDef& t = s.tracks[side];
            std::vector<i32> wheels;
            for (u32 w : t.wheels) wheels.push_back(static_cast<i32>(w));
            const f32 p[6] = {t.inertia, t.damping, t.brakeNm, t.finalDrive, t.steerGain, t.pivot};
            if (!aver_phys_vehicle_set_track(v, side, wheels.data(), static_cast<i32>(wheels.size()), static_cast<i32>(t.driven), p, 6))
                return bail("the physics module refused a track");
        }
    }

    if (s.assist.present) {
        const AssistDef& a = s.assist;
        const f32 p[AVER_PHYS_VD_COUNT] = {1.0f, a.abs ? 1.0f : 0.0f, a.absSlip, a.absRelease, a.tcs ? 1.0f : 0.0f, a.tcsSlip, a.steerIn, a.steerOut,
                                           0.0f, a.throttleIn, a.throttleOut, a.brakeIn, a.reverseBrake ? 1.0f : 0.0f, a.reverseSpeed, a.holdBrake,
                                           a.holdSpeed, a.skidSlip, a.skidAngle};
        if (!aver_phys_vehicle_set_assists(v, p, AVER_PHYS_VD_COUNT)) return bail("the physics module refused the driver aids");
    }
    if (s.aeroPresent && (s.drag > 0.0f || s.downforce > 0.0f) && !aver_phys_vehicle_set_aero(v, s.drag, s.downforce)) return bail("the physics module refused the aero");
    for (usize i = 0; i < s.steer.points.size() && i < kMaxSteerPoints; ++i) {
        const Vec2& p = s.steer.points[i];
        if (p.x == 0.0f && p.y == 0.0f) break;
        aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_0 + static_cast<i32>(i) * 2, p.x);
        aver_phys_vehicle_tune(v, AVER_PHYS_VU_STEER_CURVE_0 + static_cast<i32>(i) * 2 + 1, p.y);
    }

    if (!aver_phys_vehicle_finish(v, b.pitchRollDeg)) return bail("the physics module refused to finish the vehicle");

    out.vehicle = v;
    out.body = aver_phys_vehicle_body(v);
    out.wheelCount = static_cast<u32>(s.wheels.size());
    out.kind = s.kind;
    out.wheelbase = maxX - minX;
    out.maxSteerRad = radians(maxSteer);
    return true;
}

}  // namespace aver::vehicle
