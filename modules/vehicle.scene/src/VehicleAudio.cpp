// Sound and smoke. Audio goes through an AudioPort the host installs (the C audio ABI in the shipped hosts, a recording fake in
// the tests); nothing here links an audio module. The engine is the classic loop cross-fade: an idle loop and a high-rev loop
// mixed by RPM and pitched between two ratios, louder under load. No synthesis. Skid smoke and sparks go through the effect
// sink (Aver.Core's spawnEffectAt by default).
#include "aver/vehicle/VehiclePhysicsSystem.hpp"

#include "aver/core/EffectSpawn.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>

namespace aver::vehicle {

namespace {

void defaultEffect(u64 effectId, const Vec3& position, const Vec3& direction, u32 count, void*) {
    spawnEffectAt(effectId, position, direction, count);
}

}  // namespace

i32 VehiclePhysicsSystem::Impl::soundFor(const std::string& path) {
    if (!audio || path.empty()) return 0;
    const auto it = sounds.find(path);
    if (it != sounds.end()) return it->second;
    const i32 s = audio->loadSound(path);
    sounds[path] = s;
    return s;
}

void VehiclePhysicsSystem::Impl::stopVoices(detail::Entry& e) {
    for (i32* v : {&e.voiceIdle, &e.voiceHigh, &e.voiceSkid}) {
        if (*v != 0 && audio) audio->stop(*v);
        *v = 0;
    }
    e.audible = false;
}

void VehiclePhysicsSystem::Impl::oneShot(const detail::Entry& e, const std::string& path, f32 volume, f32 pitch) {
    if (!audio || path.empty() || dist(e.pos, viewPos) > opt.audioRangeCm) return;
    const i32 s = soundFor(path);
    if (s != 0) audio->play(s, e.pos, std::clamp(volume, 0.0f, 1.0f), pitch, false);
}

void VehiclePhysicsSystem::Impl::updateAudio(scene::World& w, f32) {
    if (!audio) return;
    struct Cand { f32 d; detail::Entry* e; };
    std::vector<Cand> cands;
    for (auto& [ent, en] : entries) {
        if (en.state != detail::Entry::State::Built) continue;
        const CVehicle* c = w.component<CVehicle>(ent, VehiclePhysicsSystem::typeId());
        const bool wants = c && !(c->flags & kVehicleNoAudio) && en.spec && en.spec->audio.present && !en.destroyed;
        const f32 d = dist(en.pos, viewPos);
        if (wants && d <= opt.audioRangeCm) cands.push_back(Cand{d, &en});
        else stopVoices(en);
    }
    // The nearest few, ties by entity id (the std::map walk is ascending, and stable_sort keeps that order).
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    u32 audible = 0;
    for (usize i = 0; i < cands.size(); ++i) {
        detail::Entry& e = *cands[i].e;
        if (i >= opt.audibleVehicles) { stopVoices(e); continue; }
        e.audible = true;
        ++audible;
        const AudioDef& a = e.spec->audio;

        const f32 span = std::max(1.0f, a.highRpm - a.idleRpm);
        const f32 rpm01 = std::clamp((e.tel.rpm - a.idleRpm) / span, 0.0f, 1.0f);
        const f32 load = std::clamp(std::fabs(e.tel.throttle), 0.0f, 1.0f);
        const f32 volume = 0.5f + 0.5f * load;              // a lift-off is audibly quieter
        const f32 pitch = a.pitchLow + (a.pitchHigh - a.pitchLow) * rpm01;
        const bool two = !a.high.empty();

        if (!a.idle.empty()) {
            if (e.voiceIdle == 0) {
                const i32 s = soundFor(a.idle);
                if (s != 0) e.voiceIdle = audio->play(s, e.pos, 0.0f, pitch, true);
            }
            if (e.voiceIdle != 0) audio->set(e.voiceIdle, e.pos, two ? volume * (1.0f - rpm01) : volume, pitch);
        }
        if (two) {
            if (e.voiceHigh == 0) {
                const i32 s = soundFor(a.high);
                if (s != 0) e.voiceHigh = audio->play(s, e.pos, 0.0f, pitch, true);
            }
            if (e.voiceHigh != 0) audio->set(e.voiceHigh, e.pos, volume * rpm01, pitch);
        }

        // Skid: one looping voice whose volume follows the worst skidding wheel.
        if (!a.skid.empty()) {
            f32 skid = 0.0f;
            for (const WheelTelemetry& wt : e.wheels)
                if (wt.skidding) skid = std::max(skid, std::clamp(std::max(std::fabs(wt.slipLongitudinal), wt.slipLateralDeg / 30.0f), 0.0f, 1.0f));
            if (skid > 0.0f && e.voiceSkid == 0) {
                const i32 s = soundFor(a.skid);
                if (s != 0) e.voiceSkid = audio->play(s, e.pos, 0.0f, 1.0f, true);
            }
            if (e.voiceSkid != 0) audio->set(e.voiceSkid, e.pos, skid * 0.8f, 1.0f);
        }
    }
    stats.audible = audible;
}

void VehiclePhysicsSystem::Impl::updateEffects(scene::World&, detail::Entry& e, CVehicle& c, f32 dt) {
    const bool on = !(c.flags & kVehicleNoEffects) && e.skidEffect != 0 && e.spec && e.spec->effects.present &&
                    dist(e.pos, viewPos) <= opt.lodFarCm;
    if (!on) {
        for (f32& a : e.smokeAcc) a = 0.0f;
        return;
    }
    // Smoke streams opposite the slide, at the contact point.
    f32 vel[3] = {};
    aver_phys_vehicle_velocity(e.vehicle, vel);
    Vec3 dir(-vel[0], -vel[1], 0.0f);
    dir = dir.getSafeNormal();
    if (dir.sizeSquared() < 0.5f) dir = Vec3(0, 0, 1);
    const f32 rate = e.spec->effects.skidRate;
    const EffectFn fx = effectFn ? effectFn : &defaultEffect;
    for (usize i = 0; i < e.wheels.size() && i < detail::kMaxWheelSlots; ++i) {
        const WheelTelemetry& wt = e.wheels[i];
        if (!wt.skidding || !wt.contact) { e.smokeAcc[i] = 0.0f; continue; }
        // An accumulator, so the count follows the TIME: a 30 Hz frame emits twice the count of a 60 Hz one, not twice the bursts.
        e.smokeAcc[i] += rate * dt;
        const u32 n = static_cast<u32>(e.smokeAcc[i]);
        if (n == 0) continue;
        e.smokeAcc[i] -= static_cast<f32>(n);
        fx(e.skidEffect, wt.contactPoint, dir, n, effectUser);
    }
}

}  // namespace aver::vehicle
