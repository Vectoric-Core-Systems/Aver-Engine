// The framework relay's provider: aver_fw_vehicle_* (framework_vehicle_abi.h) lands here. The op numbers, the telemetry
// layouts and the integer-in-a-float packing are that header's. This module does not link the framework, so the signature is
// repeated with plain types; the host passes &vehicleProvider to aver_fw_set_vehicle_provider.
#include "aver/vehicle/VehiclePhysicsSystem.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cstring>

namespace aver::vehicle {

namespace {

// The op numbers of framework_vehicle_abi.h (static-asserted in the framework test against the header).
constexpr i32 kOpSpawn = 1, kOpEnter = 2, kOpExit = 3, kOpShift = 4, kOpSetGear = 5, kOpSetManual = 6, kOpFlip = 7, kOpRepair = 8,
              kOpDamage = 9, kOpTune = 10, kOpTelemetry = 11, kOpWheel = 12, kOpEvents = 13;
constexpr i32 kTelemetryFloats = 19, kTelemetryInts = 13, kWheelFloats = 18, kWheelInts = 4, kEventInts = 6, kEventFloats = 8;

}  // namespace

i32 VehiclePhysicsSystem::relay(i32 op, i32 entity, const float* in, i32 inCount, const char* text, float* outF, i32* outI, i32 cap) {
    Impl& m = *impl_;
    scene::World& w = m.world ? *m.world : scene::World::instance();
    auto argI = [&](i32 k) {
        i32 v = 0;
        if (in && k < inCount) std::memcpy(&v, &in[k], sizeof v);
        return v;
    };
    auto argF = [&](i32 k) { return (in && k < inCount) ? in[k] : 0.0f; };
    const scene::Entity e = static_cast<scene::Entity>(entity);

    switch (op) {
        case kOpSpawn: {
            if (!text || !outI || cap < 2) return 0;
            i32 vh = 0;
            const scene::Entity ent = spawn(w, text, Vec3(argF(0), argF(1), argF(2)), argF(3), &vh);
            if (ent == scene::kInvalidEntity) return 0;
            outI[0] = static_cast<i32>(ent);
            outI[1] = vh;
            return 1;
        }
        case kOpEnter: return enter(e, static_cast<scene::Entity>(argI(0))) ? 1 : 0;
        case kOpExit: {
            const scene::Entity d = driverOf(e);
            if (!exit(e)) return 0;
            if (outI && cap >= 1) outI[0] = static_cast<i32>(d);
            return 1;
        }
        case kOpShift: return shift(e, argI(0)) ? 1 : 0;
        case kOpSetGear: return setGear(e, argI(0)) ? 1 : 0;
        case kOpSetManual: return setManual(e, argI(0) != 0) ? 1 : 0;
        case kOpFlip: return flip(e, argF(0)) ? 1 : 0;
        case kOpRepair: return repair(e) ? 1 : 0;
        case kOpDamage: return damage(e, argF(0)) ? 1 : 0;
        case kOpTune: return (text && tune(e, text, argF(0))) ? 1 : 0;
        case kOpTelemetry: {
            const detail::Entry* en = m.find(e);
            if (!en || en->state != detail::Entry::State::Built || !outF || !outI) return 0;
            const i32 nf = std::min(cap, kTelemetryFloats), ni = std::min(cap, kTelemetryInts);
            for (i32 k = 0; k < nf; ++k) outF[k] = k < 18 ? en->rawVs[k] : en->tel.health01;
            for (i32 k = 0; k < ni; ++k) outI[k] = k < 11 ? en->rawVsi[k] : (k == 11 ? static_cast<i32>(en->outState) : static_cast<i32>(en->driver));
            return 1;
        }
        case kOpWheel: {
            const detail::Entry* en = m.find(e);
            const i32 wheel = argI(0);
            if (!en || en->state != detail::Entry::State::Built || !outF || !outI || wheel < 0 ||
                static_cast<usize>(wheel) >= en->wheels.size() || en->rawWheelF.size() < static_cast<usize>(wheel + 1) * kWheelFloats)
                return 0;
            const i32 nf = std::min(cap, kWheelFloats), ni = std::min(cap, kWheelInts);
            for (i32 k = 0; k < nf; ++k) outF[k] = en->rawWheelF[static_cast<usize>(wheel) * kWheelFloats + k];
            for (i32 k = 0; k < ni; ++k) outI[k] = en->rawWheelI[static_cast<usize>(wheel) * kWheelInts + k];
            return 1;
        }
        case kOpEvents: {
            if (!outF || !outI || cap <= 0) return 0;
            std::vector<VehicleEvent> evs(static_cast<usize>(cap));
            const u32 n = drainEvents(evs.data(), static_cast<u32>(cap));
            for (u32 k = 0; k < n; ++k) {
                const VehicleEvent& ev = evs[k];
                i32* ri = outI + k * kEventInts;
                f32* rf = outF + k * kEventFloats;
                ri[0] = static_cast<i32>(ev.kind);
                ri[1] = static_cast<i32>(ev.vehicle);
                ri[2] = ev.a;
                ri[3] = ev.b;
                ri[4] = static_cast<i32>(ev.seq);
                ri[5] = static_cast<i32>(ev.other);
                rf[0] = ev.point.x; rf[1] = ev.point.y; rf[2] = ev.point.z;
                rf[3] = ev.normal.x; rf[4] = ev.normal.y; rf[5] = ev.normal.z;
                rf[6] = ev.value; rf[7] = ev.value2;
            }
            return static_cast<i32>(n);
        }
        default: return 0;
    }
}

i32 vehicleProvider(i32 op, i32 entity, const float* in, i32 inCount, const char* text, float* outF, i32* outI, i32 cap, void* user) {
    auto* sys = user ? static_cast<VehiclePhysicsSystem*>(user) : &vehiclePhysicsSystem();
    return sys->relay(op, entity, in, inCount, text, outF, outI, cap);
}

}  // namespace aver::vehicle
