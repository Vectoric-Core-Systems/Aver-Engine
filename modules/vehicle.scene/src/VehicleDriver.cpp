// Driving: reading the Drive.* actions, sending the pedals, getting in and out, parking the driver, the camera seam,
// crash damage, and the operations a script or the relay asks for (shift, flip, repair, tune).
#include "aver/vehicle/VehiclePhysicsSystem.hpp"

#include "aver/core/EffectSpawn.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_character_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>

namespace aver::vehicle {

namespace {

// The named actions a driver is read from (the template's one .ocinput binds them; a C# project can push DrivingContext).
constexpr const char* kThrottle = "Drive.Throttle";
constexpr const char* kBrake = "Drive.Brake";
constexpr const char* kSteer = "Drive.Steer";
constexpr const char* kHandbrake = "Drive.Handbrake";
constexpr const char* kShiftUp = "Drive.ShiftUp";
constexpr const char* kShiftDown = "Drive.ShiftDown";
constexpr const char* kReset = "Drive.Reset";
constexpr const char* kExit = "Drive.Exit";
constexpr const char* kCameraNext = "Drive.CameraNext";

constexpr f32 kDefaultCrashCmS = 600.0f;       // when a spec has no DAMAGE record: a hit this hard raises VehicleCrash
constexpr f32 kShakeCmS = 300.0f;              // an impact this hard asks the camera sink for a shake
constexpr f32 kFlipLiftCm = 50.0f;

f32 orOne(f32 v) { return v > 0.0f ? v : 1.0f; }

// Sends a scale to the physics vehicle when it changed. `applied` keeps the last value the physics accepted.
void sendScale(i32 vehicle, i32 key, f32 value, f32& applied) {
    if (value != applied && aver_phys_vehicle_tune(vehicle, key, value)) applied = value;
}

// The camera sink's view of a vehicle's rigs, with `current` as the live one.
VehicleCameraInfo cameraInfoOf(const detail::Entry& e, u32 current) {
    VehicleCameraInfo info;
    info.chassisBody = e.body;
    info.rigCount = static_cast<u32>(e.rigs.size());
    for (u32 i = 0; i < info.rigCount; ++i) info.rigs[i] = e.rigs[i];
    info.current = current;
    return info;
}

// The default effect sink: Aver.Core's seam, which does nothing until a particle system installs a spawner.
void defaultEffect(u64 effectId, const Vec3& position, const Vec3& direction, u32 count, void*) {
    spawnEffectAt(effectId, position, direction, count);
}

Transform localFor(scene::World& w, scene::Entity e, const Transform& worldXf) {
    const scene::Entity parent = w.parent(e);
    if (parent == scene::kInvalidEntity) return worldXf;
    return transformFromMatrix(worldXf.toMatrix() * w.worldMatrix(parent).inverse());
}

scene::Entity findDescendant(scene::World& w, scene::Entity root, const std::string& name) {
    if (name.empty()) return scene::kInvalidEntity;
    std::vector<scene::Entity> stack{root};
    while (!stack.empty()) {
        const scene::Entity cur = stack.back();
        stack.pop_back();
        for (scene::Entity c = w.firstChild(cur); c != scene::kInvalidEntity; c = w.nextSibling(c)) {
            if (name == w.name(c)) return c;
            stack.push_back(c);
        }
    }
    return scene::kInvalidEntity;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

}  // namespace

f32 VehiclePhysicsSystem::Impl::specHealth(const detail::Entry& e) const {
    return (e.spec && e.spec->damage.present) ? e.spec->damage.health : 0.0f;
}

// ---- the frame's driving half --------------------------------------------------------------------------------------

void VehiclePhysicsSystem::Impl::readActions(scene::World&, detail::Entry& e, CVehicle& c) {
    auto act = [&](const char* name, ActionState& s) {
        s = ActionState{};
        return actions && actions(name, s) && s.found;
    };
    ActionState a;
    c.inThrottle = act(kThrottle, a) ? std::clamp(a.value, -1.0f, 1.0f) : 0.0f;
    c.inBrake = act(kBrake, a) ? std::clamp(a.value, 0.0f, 1.0f) : 0.0f;
    c.inSteer = act(kSteer, a) ? std::clamp(a.value, -1.0f, 1.0f) : 0.0f;
    c.inHandbrake = act(kHandbrake, a) && a.held ? 1.0f : 0.0f;
    const i32 vh = e.vehicle;
    // In a manual box the shift keys change gear; in an automatic one they are ignored.
    if (act(kShiftUp, a) && a.pressed && e.tel.manual) aver_phys_vehicle_shift(vh, 1);
    if (act(kShiftDown, a) && a.pressed && e.tel.manual) aver_phys_vehicle_shift(vh, -1);
    if (act(kReset, a) && a.pressed) c.flags |= kVehicleFlipRequest;
    if (act(kCameraNext, a) && a.pressed) cameraNext(e);
    if (act(kExit, a) && a.pressed) exitImpl(*world, e);
}

bool VehiclePhysicsSystem::Impl::applyAssistOverrides(detail::Entry& e) {
    if (!e.spec) return false;
    const AssistDef& d = e.spec->assist;
    const bool abs = e.absOverride >= 0 ? e.absOverride != 0 : d.abs;
    const bool tcs = e.tcsOverride >= 0 ? e.tcsOverride != 0 : d.tcs;
    const bool enabled = d.present || abs || tcs;
    const f32 p[AVER_PHYS_VD_COUNT] = {enabled ? 1.0f : 0.0f, abs ? 1.0f : 0.0f, d.absSlip, d.absRelease, tcs ? 1.0f : 0.0f, d.tcsSlip, d.steerIn, d.steerOut,
                                       0.0f, d.throttleIn, d.throttleOut, d.brakeIn, d.reverseBrake ? 1.0f : 0.0f, d.reverseSpeed, d.holdBrake,
                                       d.holdSpeed, d.skidSlip, d.skidAngle};
    return aver_phys_vehicle_set_assists(e.vehicle, p, AVER_PHYS_VD_COUNT) != 0;
}

void VehiclePhysicsSystem::Impl::updateLod(detail::Entry& e, bool driven) {
    i32 period = 1;
    if (!driven) {
        const f32 d = dist(e.pos, viewPos);
        period = d > opt.lodFarCm ? 4 : (d > opt.lodMidCm ? 2 : 1);
    }
    if (period == e.appliedPeriod) return;
    if (aver_phys_vehicle_set_collision_period(e.vehicle, period)) e.appliedPeriod = period;
}

void VehiclePhysicsSystem::Impl::applyInputs(scene::World& w, detail::Entry& e, CVehicle& c) {
    const bool driven = e.driver != scene::kInvalidEntity;
    if (driven && !(c.flags & kVehicleNoActions)) readActions(w, e, c);
    // readActions may have exited the vehicle: the rest is the same either way.

    // The scales. A destroyed vehicle has no engine; repair gives it back.
    sendScale(e.vehicle, AVER_PHYS_VU_ENGINE_TORQUE_SCALE, e.destroyed ? 0.0f : orOne(c.powerScale), e.appliedPower);
    sendScale(e.vehicle, AVER_PHYS_VU_GRIP_SCALE, orOne(c.gripScale), e.appliedGrip);
    sendScale(e.vehicle, AVER_PHYS_VU_BRAKE_SCALE, orOne(c.brakeScale), e.appliedBrake);

    if (c.flags & kVehicleFlipRequest) {
        c.flags &= ~kVehicleFlipRequest;
        aver_phys_vehicle_flip(e.vehicle, kFlipLiftCm);
        e.flippedFor = 0.0f;
    }
    if (c.flags & kVehicleRepairRequest) {
        c.flags &= ~kVehicleRepairRequest;
        const f32 full = specHealth(e);
        if (full > 0.0f) applyDamage(w, e, c, -full, false);
        else { e.destroyed = false; e.appliedPower = -1.0f; }
    }

    updateLod(e, e.driver != scene::kInvalidEntity);

    // The pedals: sent when they changed (the physics world holds them between calls).
    const f32 in[4] = {c.inThrottle, c.inSteer, c.inBrake, c.inHandbrake};
    bool changed = false;
    for (int i = 0; i < 4; ++i) changed = changed || in[i] != e.lastIn[i];
    if (changed) {
        aver_phys_vehicle_set_input(e.vehicle, in[0], in[1], in[2], in[3]);
        for (int i = 0; i < 4; ++i) e.lastIn[i] = in[i];
    }
    if (e.driver != scene::kInvalidEntity) snapDriver(w, e);
}

// ---- the driver ------------------------------------------------------------------------------------------------------

void VehiclePhysicsSystem::Impl::hideDriver(scene::World& w, detail::Entry& e) {
    e.hiddenMeshes.clear();
    std::vector<scene::Entity> stack{e.driver};
    while (!stack.empty()) {
        const scene::Entity cur = stack.back();
        stack.pop_back();
        if (auto* mr = w.component<scene::CMeshRenderer>(cur, scene::kComponentMeshRenderer)) {
            e.hiddenMeshes.emplace_back(cur, mr->flags);
            mr->flags &= ~scene::kMeshRendererVisible;
            mr->dirty = 1;
        }
        for (scene::Entity c = w.firstChild(cur); c != scene::kInvalidEntity; c = w.nextSibling(c)) stack.push_back(c);
    }
}

void VehiclePhysicsSystem::Impl::showDriver(scene::World& w, detail::Entry& e) {
    for (const auto& [ent, flags] : e.hiddenMeshes) {
        if (!w.valid(ent)) continue;
        if (auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer)) {
            mr->flags = flags;
            mr->dirty = 1;
        }
    }
    e.hiddenMeshes.clear();
}

void VehiclePhysicsSystem::Impl::buildRigs(scene::World& w, detail::Entry& e) {
    e.rigs.clear();
    e.rigCurrent = 0;
    if (!cameraFn || !e.spec || !e.spec->camera.present) return;
    const CameraDef& cam = e.spec->camera;
    for (const std::string& raw : cam.modes) {
        if (e.rigs.size() >= 3) break;
        const std::string mode = lower(raw);
        CameraRigInfo info;
        const char* name = nullptr;
        if (mode == "chase") { info.kind = CameraRigKind::Chase; info.localPoint = Vec3(0, 0, 0); info.armCm = cam.arm; name = "VehicleChaseRig"; }
        else if (mode == "hood") { info.kind = CameraRigKind::Hood; info.localPoint = cam.hood; name = "VehicleHoodRig"; }
        else if (mode == "cockpit") { info.kind = CameraRigKind::Cockpit; info.localPoint = cam.eye; name = "VehicleCockpitRig"; }
        else continue;
        Transform local;
        local.position = info.localPoint;
        info.rig = w.create(name, e.entity, local);
        if (info.rig != scene::kInvalidEntity) e.rigs.push_back(info);
    }
}

void VehiclePhysicsSystem::Impl::cameraNext(detail::Entry& e) {
    if (!cameraFn || e.rigs.size() < 2) return;
    e.rigCurrent = (e.rigCurrent + 1) % static_cast<u32>(e.rigs.size());
    cameraFn(e.entity, e.driver, CameraEvent::Next, cameraInfoOf(e, e.rigCurrent), cameraUser);
}

bool VehiclePhysicsSystem::Impl::enterImpl(scene::World& w, detail::Entry& e, scene::Entity driver) {
    if (e.state != detail::Entry::State::Built || e.driver != scene::kInvalidEntity) return false;
    if (driver == scene::kInvalidEntity || !w.valid(driver) || driver == e.entity) return false;
    for (const auto& kv : entries)
        if (kv.second.driver == driver) return false;   // already driving something
    e.driver = driver;
    hideDriver(w, e);

    // A physics character left inside the chassis must neither fall nor shove the car: no gravity, no strength, both
    // given back on exit.
    e.driverCharacter = aver_phys_character_of_entity(static_cast<i32>(driver));
    if (e.driverCharacter != 0) {
        f32 g = 1.0f, s = 0.0f;
        aver_phys_character_gravity_factor(e.driverCharacter, &g);
        aver_phys_character_max_strength(e.driverCharacter, &s);
        e.driverGravity = g;
        e.driverStrength = s;
        aver_phys_character_set_gravity_factor(e.driverCharacter, 0.0f);
        aver_phys_character_set_max_strength(e.driverCharacter, 0.0f);
    }

    buildRigs(w, e);
    if (CVehicle* c = w.component<CVehicle>(e.entity, VehiclePhysicsSystem::typeId())) c->driver = driver;
    snapDriver(w, e);
    if (cameraFn) cameraFn(e.entity, driver, CameraEvent::Enter, cameraInfoOf(e, 0), cameraUser);
    emit(VehicleEventKind::Entered, e.entity, driver, 0, 0, e.pos, Vec3(), 0.0f, 0.0f, "VehicleEntered");
    return true;
}

bool VehiclePhysicsSystem::Impl::exitImpl(scene::World& w, detail::Entry& e) {
    if (e.driver == scene::kInvalidEntity) return false;
    const scene::Entity driver = e.driver;
    const bool alive = w.valid(driver) && !w.destroyPending(driver);

    if (e.driverCharacter != 0) {
        aver_phys_character_set_gravity_factor(e.driverCharacter, e.driverGravity);
        aver_phys_character_set_max_strength(e.driverCharacter, e.driverStrength);
    }
    if (alive) {
        showDriver(w, e);
        // Set down beside the vehicle: the spec's exit point, else its left side.
        Vec3 local = e.spec && e.spec->seat.present ? e.spec->seat.exit : Vec3();
        if (local.sizeSquared() < 1e-6f && e.spec) local = Vec3(0, -(e.spec->body.half.y + 80.0f), 0);
        const Vec3 at = e.pos + e.rot.rotate(local);
        Transform xf = w.localTransform(driver);
        xf.position = at;
        xf.rotation = e.rot;
        w.setLocalTransform(driver, localFor(w, driver, xf));
        if (e.driverCharacter != 0) {
            aver_phys_character_set_position(e.driverCharacter, at.x, at.y, at.z);
            aver_phys_character_set_velocity(e.driverCharacter, 0.0f, 0.0f, 0.0f);
        }
    } else {
        e.hiddenMeshes.clear();
    }

    if (cameraFn) cameraFn(e.entity, driver, CameraEvent::Exit, cameraInfoOf(e, e.rigCurrent), cameraUser);
    for (const CameraRigInfo& r : e.rigs)
        if (w.valid(r.rig)) w.destroy(r.rig);
    e.rigs.clear();

    e.driver = scene::kInvalidEntity;
    e.driverCharacter = 0;
    if (CVehicle* c = w.component<CVehicle>(e.entity, VehiclePhysicsSystem::typeId())) c->driver = 0;
    emit(VehicleEventKind::Exited, e.entity, driver, 0, 0, e.pos, Vec3(), 0.0f, 0.0f, "VehicleExited");
    return true;
}

void VehiclePhysicsSystem::Impl::snapDriver(scene::World& w, detail::Entry& e) {
    if (e.driver == scene::kInvalidEntity) return;
    if (!w.valid(e.driver) || w.destroyPending(e.driver)) { exitImpl(w, e); return; }
    const Vec3 seat = e.spec && e.spec->seat.present ? e.spec->seat.driver : Vec3(0, 0, 55);
    const Vec3 at = e.pos + e.rot.rotate(seat);
    Transform cur = w.localTransform(e.driver);
    Transform want = cur;
    want.position = at;
    want.rotation = e.rot;
    const Transform wantLocal = localFor(w, e.driver, want);
    const Transform& have = w.localTransform(e.driver);
    if (have.position.x != wantLocal.position.x || have.position.y != wantLocal.position.y || have.position.z != wantLocal.position.z ||
        have.rotation.x != wantLocal.rotation.x || have.rotation.y != wantLocal.rotation.y || have.rotation.z != wantLocal.rotation.z ||
        have.rotation.w != wantLocal.rotation.w) {
        Transform t = wantLocal;
        t.scale = cur.scale;
        w.setLocalTransform(e.driver, t);
    }
    if (e.driverCharacter != 0) {
        aver_phys_character_set_position(e.driverCharacter, at.x, at.y, at.z);
        aver_phys_character_set_velocity(e.driverCharacter, 0.0f, 0.0f, 0.0f);
    }
}

// ---- damage ------------------------------------------------------------------------------------------------------------

// `amountHealth` is in the spec's health units; negative restores.
void VehiclePhysicsSystem::Impl::applyDamage(scene::World& w, detail::Entry& e, CVehicle& c, f32 amountHealth, bool announce) {
    (void)w;
    const f32 full = specHealth(e);
    if (full <= 0.0f) return;
    if (amountHealth > 0.0f && (c.flags & kVehicleIndestructible)) return;
    e.health = std::clamp(e.health - amountHealth, 0.0f, full);
    c.outHealth01 = e.health / full;
    if (amountHealth < 0.0f && e.health > 0.0f && e.destroyed) {
        e.destroyed = false;
        e.appliedPower = -1.0f;   // re-applied from the scale next frame
    }
    if (e.health <= 0.0f && !e.destroyed) {
        e.destroyed = true;
        aver_phys_vehicle_tune(e.vehicle, AVER_PHYS_VU_ENGINE_TORQUE_SCALE, 0.0f);
        e.appliedPower = 0.0f;
        if (announce) emit(VehicleEventKind::Destroyed, e.entity, scene::kInvalidEntity, 0, 0, e.pos, Vec3(), 0.0f, 0.0f, nullptr);
    }
}

void VehiclePhysicsSystem::Impl::onImpact(scene::World& w, detail::Entry& e, CVehicle& c, const VehicleEvent& ev) {
    const f32 closing = ev.value;
    const f32 otherMass = ev.value2;
    emit(VehicleEventKind::Impact, e.entity, scene::kInvalidEntity, ev.a, 0, ev.point, ev.normal, closing, otherMass, nullptr);
    const VehicleSpec* spec = e.spec.get();
    if (!spec) return;

    if (!(c.flags & kVehicleNoEffects) && e.impactEffect != 0) {
        const EffectFn fx = effectFn ? effectFn : &defaultEffect;
        fx(e.impactEffect, ev.point, ev.normal, std::max<u32>(1u, spec->effects.impactCount), effectUser);
    }
    if (!(c.flags & kVehicleNoAudio) && !spec->audio.impact.empty())
        oneShot(e, spec->audio.impact, std::clamp(closing / 1500.0f, 0.15f, 1.0f), 1.0f);

    if (cameraFn && e.driver != scene::kInvalidEntity && closing > kShakeCmS) {
        VehicleCameraInfo info = cameraInfoOf(e, e.rigCurrent);
        info.impactSpeedCmS = closing;
        info.impactPoint = ev.point;
        cameraFn(e.entity, e.driver, CameraEvent::Impact, info, cameraUser);
    }

    const f32 threshold = spec->damage.present ? spec->damage.crash : kDefaultCrashCmS;
    if (closing <= threshold) return;
    emit(VehicleEventKind::Crash, e.entity, scene::kInvalidEntity, ev.a, 0, ev.point, ev.normal, closing, otherMass, "VehicleCrash");
    if (!spec->damage.present) return;

    const f32 frac = std::clamp((closing - threshold) / (3.0f * std::max(threshold, 1.0f)), 0.0f, 1.0f);
    applyDamage(w, e, c, spec->damage.health * orOne(c.damageScale) * frac, true);

    if (crashFn) {
        const f32 m1 = spec->body.massKg;
        const f32 mReduced = otherMass > 0.0f ? (m1 * otherMass) / (m1 + otherMass) : m1;
        const f32 v = closing * 0.01f;   // m/s
        CrashInfo info;
        info.vehicle = e.entity;
        info.shell = findDescendant(w, e.entity, spec->damage.shell);
        info.point = ev.point;
        info.dir = ev.normal;
        info.radiusCm = spec->damage.radius;
        info.energyJ = 0.5f * mReduced * v * v * spec->damage.energy;
        info.closingCmS = closing;
        crashFn(info, crashUser);
    }
}

// ---- the operations ------------------------------------------------------------------------------------------------------

bool VehiclePhysicsSystem::enter(scene::Entity vehicle, scene::Entity driver) {
    Impl& m = *impl_;
    detail::Entry* e = m.find(vehicle);
    return e && m.world && m.enterImpl(*m.world, *e, driver);
}

bool VehiclePhysicsSystem::exit(scene::Entity vehicle) {
    Impl& m = *impl_;
    detail::Entry* e = m.find(vehicle);
    return e && m.world && m.exitImpl(*m.world, *e);
}

bool VehiclePhysicsSystem::shift(scene::Entity v, i32 delta) {
    const detail::Entry* e = impl_->find(v);
    return e && e->state == detail::Entry::State::Built && aver_phys_vehicle_shift(e->vehicle, delta) != 0;
}

bool VehiclePhysicsSystem::setGear(scene::Entity v, i32 gear) {
    const detail::Entry* e = impl_->find(v);
    return e && e->state == detail::Entry::State::Built && aver_phys_vehicle_set_gear(e->vehicle, gear) != 0;
}

bool VehiclePhysicsSystem::setManual(scene::Entity v, bool manual) {
    const detail::Entry* e = impl_->find(v);
    return e && e->state == detail::Entry::State::Built && aver_phys_vehicle_set_gearbox_mode(e->vehicle, manual ? 1 : 0) != 0;
}

bool VehiclePhysicsSystem::flip(scene::Entity v, f32 liftCm) {
    detail::Entry* e = impl_->find(v);
    if (!e || e->state != detail::Entry::State::Built || !aver_phys_vehicle_flip(e->vehicle, liftCm)) return false;
    e->flippedFor = 0.0f;
    return true;
}

bool VehiclePhysicsSystem::repair(scene::Entity v) {
    Impl& m = *impl_;
    detail::Entry* e = m.find(v);
    if (!e || e->state != detail::Entry::State::Built || !m.world) return false;
    CVehicle* c = m.world->component<CVehicle>(v, typeId());
    if (!c) return false;
    const f32 full = m.specHealth(*e);
    if (full > 0.0f) m.applyDamage(*m.world, *e, *c, -full, false);
    else { e->destroyed = false; e->appliedPower = -1.0f; }
    return true;
}

bool VehiclePhysicsSystem::damage(scene::Entity v, f32 amount01) {
    Impl& m = *impl_;
    detail::Entry* e = m.find(v);
    if (!e || e->state != detail::Entry::State::Built || !m.world || !std::isfinite(amount01)) return false;
    CVehicle* c = m.world->component<CVehicle>(v, typeId());
    const f32 full = m.specHealth(*e);
    if (!c || full <= 0.0f) return false;
    // A negative amount gives health back (the Aver Node repair row restores part of it).
    m.applyDamage(*m.world, *e, *c, full * std::clamp(amount01, -1.0f, 1.0f), true);
    return true;
}

bool VehiclePhysicsSystem::tune(scene::Entity v, std::string_view keyIn, f32 value) {
    Impl& m = *impl_;
    detail::Entry* e = m.find(v);
    if (!e || e->state != detail::Entry::State::Built || !std::isfinite(value)) return false;
    const std::string key = lower(keyIn);
    CVehicle* c = m.world ? m.world->component<CVehicle>(v, typeId()) : nullptr;
    // Power, brake and grip are the component's own fields, so a Details slider, a script and this call are one control.
    if (key == "power") { if (c) c->powerScale = value; return c != nullptr; }
    if (key == "brake") { if (c) c->brakeScale = value; return c != nullptr; }
    if (key == "grip") { if (c) c->gripScale = value; return c != nullptr; }
    if (key == "suspension") return aver_phys_vehicle_tune(e->vehicle, AVER_PHYS_VU_SUSPENSION_STIFFNESS_SCALE, value) != 0;
    if (key == "steer") return aver_phys_vehicle_tune(e->vehicle, AVER_PHYS_VU_STEER_SCALE, value) != 0;
    if (key == "drag") return aver_phys_vehicle_tune(e->vehicle, AVER_PHYS_VU_DRAG_SCALE, value) != 0;
    if (key == "downforce") return aver_phys_vehicle_tune(e->vehicle, AVER_PHYS_VU_DOWNFORCE_SCALE, value) != 0;
    if (key == "abs") { e->absOverride = value != 0.0f ? 1 : 0; return m.applyAssistOverrides(*e); }
    if (key == "tcs") { e->tcsOverride = value != 0.0f ? 1 : 0; return m.applyAssistOverrides(*e); }
    if (key == "manual") return setManual(v, value != 0.0f);
    if (key == "collisionperiod") {
        const i32 n = static_cast<i32>(value);
        if (n < 1 || n > 8 || !aver_phys_vehicle_set_collision_period(e->vehicle, n)) return false;
        e->appliedPeriod = n;
        return true;
    }
    AVER_WARN("[Vehicle] tune: unknown key '{}'", std::string(keyIn));
    return false;
}

}  // namespace aver::vehicle
