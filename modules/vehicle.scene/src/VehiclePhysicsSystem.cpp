// The system: registration, the build lifecycle, the per-frame order, pose and telemetry, the physics event stream.
// Input, driving and damage are in VehicleDriver.cpp, wheel visuals in VehicleVisuals.cpp, sound and smoke in
// VehicleAudio.cpp, the framework relay in VehicleProvider.cpp. The frame order is the determinism contract: every loop
// is ascending by entity id, then wheel index.
#include "aver/vehicle/VehiclePhysicsSystem.hpp"

#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_vehicle_abi.h"
#include "aver/physics/physics_vehicle_ext_abi.h"
#include "aver/scene/Components.hpp"
#include "aver/vehicle/OcVehicle.hpp"
#include "aver/vehicle/VehicleBuild.hpp"
#include "aver/vehicle/VehiclePresets.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aver::vehicle {

namespace {

u32 g_type = 0;

f32 snapToOne(f32 v) { return std::fabs(v - 1.0f) < 1e-4f ? 1.0f : v; }

bool finitePose(const Vec3& p, const Quat& q) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

bool samePose(const Vec3& a, const Quat& qa, const Vec3& b, const Quat& qb) {
    return a.x == b.x && a.y == b.y && a.z == b.z && qa.x == qb.x && qa.y == qb.y && qa.z == qb.z && qa.w == qb.w;
}

// `worldXf` (an entity's world transform) expressed as the local transform its parent chain needs.
Transform localFor(scene::World& w, scene::Entity e, const Transform& worldXf) {
    const scene::Entity parent = w.parent(e);
    if (parent == scene::kInvalidEntity) return worldXf;
    // Row-vector matrices: world = local * parentWorld, so local = world * parentWorld^-1.
    return transformFromMatrix(worldXf.toMatrix() * w.worldMatrix(parent).inverse());
}

bool startsWithOcVehicle(std::string_view s) {
    constexpr std::string_view kWord = "OCVEHICLE";
    if (s.size() < kWord.size()) return false;
    for (usize i = 0; i < kWord.size(); ++i) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != kWord[i]) return false;
    }
    return true;
}

// A preset spawned with no mesh is sized like the typical one.
Vec3 defaultSizeOf(int preset) {
    switch (preset) {
        case 1: return Vec3(520, 200, 260);
        case 2: return Vec3(900, 250, 320);
        case 3: return Vec3(1200, 260, 330);
        case 4: return Vec3(430, 190, 120);
        case 5: return Vec3(700, 340, 200);
        default: return Vec3(440, 180, 140);
    }
}

}  // namespace

// ---- the component ----------------------------------------------------------------------------------------

u32 VehiclePhysicsSystem::registerComponents(scene::World& world) {
    using scene::FieldKind;
    auto b = world.registerComponent<CVehicle>("CVehicle");
    b.field("asset", FieldKind::I64, static_cast<u16>(offsetof(CVehicle, asset)))
        .field("preset", FieldKind::I32, static_cast<u16>(offsetof(CVehicle, preset)))
        .field("flags", FieldKind::I32, static_cast<u16>(offsetof(CVehicle, flags)))
        .field("inThrottle", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, inThrottle)))
        .field("inSteer", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, inSteer)))
        .field("inBrake", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, inBrake)))
        .field("inHandbrake", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, inHandbrake)))
        .field("powerScale", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, powerScale)))
        .field("gripScale", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, gripScale)))
        .field("brakeScale", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, brakeScale)))
        .field("damageScale", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, damageScale)))
        .field("driver", FieldKind::Entity, static_cast<u16>(offsetof(CVehicle, driver)), 0, true)
        .field("vehicle", FieldKind::I32, static_cast<u16>(offsetof(CVehicle, vehicle)), 0, true)
        .field("outState", FieldKind::I32, static_cast<u16>(offsetof(CVehicle, outState)), 0, true)
        .field("outGear", FieldKind::I32, static_cast<u16>(offsetof(CVehicle, outGear)), 0, true)
        .field("outSpeedCmS", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, outSpeedCmS)), 0, true)
        .field("outForwardCmS", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, outForwardCmS)), 0, true)
        .field("outRpm", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, outRpm)), 0, true)
        .field("outHealth01", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, outHealth01)), 0, true)
        .field("outThrottle", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, outThrottle)), 0, true)
        .field("outSteer", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, outSteer)), 0, true)
        .field("outBrake", FieldKind::F32, static_cast<u16>(offsetof(CVehicle, outBrake)), 0, true)
        .field("outWheelsOnGround", FieldKind::I32, static_cast<u16>(offsetof(CVehicle, outWheelsOnGround)), 0, true);
    AVER_ASSERTM(b.verify(sizeof(CVehicle)), "CVehicle");
    g_type = b.typeId();
    return g_type;
}

u32 VehiclePhysicsSystem::typeId() { return g_type; }

CVehicle* VehiclePhysicsSystem::attach(scene::World& world, scene::Entity e, u64 asset, u32 preset) {
    if (!g_type || !world.valid(e)) return nullptr;
    auto* c = static_cast<CVehicle*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    *c = CVehicle{};
    c->asset = asset;
    c->preset = preset;
    return c;
}

// ---- lifetime and seams -----------------------------------------------------------------------------------

VehiclePhysicsSystem::VehiclePhysicsSystem() : impl_(std::make_unique<Impl>()) {}
VehiclePhysicsSystem::~VehiclePhysicsSystem() = default;

VehiclePhysicsSystem& vehiclePhysicsSystem() {
    static VehiclePhysicsSystem s;
    return s;
}

void VehiclePhysicsSystem::install(scene::World& world) { impl_->world = &world; }

void VehiclePhysicsSystem::uninstall() {
    clear();
    impl_->world = nullptr;
}

void VehiclePhysicsSystem::setSpecSource(SpecSourceFn fn) { impl_->specSource = std::move(fn); }
void VehiclePhysicsSystem::setBoundsSource(BoundsFn fn) { impl_->boundsSource = std::move(fn); }
void VehiclePhysicsSystem::setActionSource(ActionFn fn) { impl_->actions = std::move(fn); }
void VehiclePhysicsSystem::setAudioPort(AudioPort* port) { impl_->audio = port; }
void VehiclePhysicsSystem::setEffectSink(EffectFn fn, void* user) { impl_->effectFn = fn; impl_->effectUser = user; }
void VehiclePhysicsSystem::setCrashSink(CrashFn fn, void* user) { impl_->crashFn = fn; impl_->crashUser = user; }
void VehiclePhysicsSystem::setCameraSink(CameraFn fn, void* user) { impl_->cameraFn = fn; impl_->cameraUser = user; }
void VehiclePhysicsSystem::setNotifySink(NotifyFn fn, void* user) { impl_->notifyFn = fn; impl_->notifyUser = user; }
void VehiclePhysicsSystem::setRigInputSink(RigInputFn fn, void* user) { impl_->rigFn = fn; impl_->rigUser = user; }
void VehiclePhysicsSystem::setViewPos(const Vec3& position) { impl_->viewPos = position; }
Options& VehiclePhysicsSystem::options() { return impl_->opt; }
const Options& VehiclePhysicsSystem::options() const { return impl_->opt; }

detail::Entry* VehiclePhysicsSystem::Impl::find(scene::Entity e) {
    const auto it = entries.find(e);
    return it == entries.end() ? nullptr : &it->second;
}
const detail::Entry* VehiclePhysicsSystem::Impl::find(scene::Entity e) const {
    const auto it = entries.find(e);
    return it == entries.end() ? nullptr : &it->second;
}

void VehiclePhysicsSystem::Impl::notify(scene::Entity e, const char* name) {
    if (notifyFn && name) notifyFn(e, name, notifyUser);
}

void VehiclePhysicsSystem::Impl::emit(VehicleEventKind kind, scene::Entity vehicle, scene::Entity other, i32 a, i32 b, const Vec3& point,
                                      const Vec3& normal, f32 v1, f32 v2, const char* notifyName) {
    VehicleEvent ev;
    ev.kind = kind;
    ev.vehicle = vehicle;
    ev.other = other;
    ev.a = a;
    ev.b = b;
    ev.point = point;
    ev.normal = normal;
    ev.value = v1;
    ev.value2 = v2;
    ev.seq = seq++;
    if (events.size() >= opt.maxQueuedEvents) events.erase(events.begin());   // the oldest unread event goes
    events.push_back(ev);
    notify(vehicle, notifyName);
}

// ---- building ----------------------------------------------------------------------------------------------

std::shared_ptr<VehicleSpec> VehiclePhysicsSystem::Impl::specFor(scene::World& w, detail::Entry& e, const CVehicle& c) {
    if (c.asset != 0) {
        if (const auto it = inlineSpecs.find(c.asset); it != inlineSpecs.end()) return it->second;
        const VehicleSpec* found = specSource ? specSource(c.asset) : nullptr;
        return found ? std::make_shared<VehicleSpec>(*found) : nullptr;
    }
    const char* name = presetName(c.preset);
    if (!name) return nullptr;
    Vec3 lo, hi;
    bool have = false;
    if (boundsSource) {
        have = boundsSource(e.entity, lo, hi);
    } else if (const auto* mr = w.component<scene::CMeshRenderer>(e.entity, scene::kComponentMeshRenderer)) {
        lo = Vec3(mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]);
        hi = Vec3(mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]);
        have = true;
    }
    if (!have) return nullptr;
    const Vec3 ext = hi - lo;
    auto spec = std::make_shared<VehicleSpec>();
    if (!specFromBounds(name, Vec3(std::fabs(ext.x), std::fabs(ext.y), std::fabs(ext.z)), *spec)) return nullptr;
    return spec;
}

bool VehiclePhysicsSystem::Impl::buildEntry(scene::World& w, detail::Entry& e, CVehicle& c) {
    e.keyAsset = c.asset;
    e.keyPreset = c.preset;
    // The same fights GameLevel::beginVehicles refuses: a rigid body or an object clip on the entity would write its
    // transform against the vehicle's every frame.
    if (w.hasComponent(e.entity, scene::kComponentRigidBody) || w.hasComponent(e.entity, scene::kComponentAnimator)) {
        if (!e.warnedConflict) AVER_WARN("[Vehicle] entity {} has a rigid body or an object animation as well as a Vehicle; it is not built", e.entity);
        e.warnedConflict = true;
        e.state = detail::Entry::State::Refused;
        return false;
    }
    const Transform xf = transformFromMatrix(w.worldMatrix(e.entity));
    if (!finitePose(xf.position, xf.rotation)) { e.state = detail::Entry::State::Refused; return false; }
    // A physics vehicle is unscaled: the wheel and suspension sizes are absolute.
    if (std::fabs(xf.scale.x - 1.0f) > 1e-3f || std::fabs(xf.scale.y - 1.0f) > 1e-3f || std::fabs(xf.scale.z - 1.0f) > 1e-3f) {
        if (!e.warnedScale) AVER_WARN("[Vehicle] entity {} is not at scale 1 ({:.3f}, {:.3f}, {:.3f}); a physics vehicle is not scaled, so it is not built",
                                      e.entity, xf.scale.x, xf.scale.y, xf.scale.z);
        e.warnedScale = true;
        e.state = detail::Entry::State::Refused;
        return false;
    }
    std::shared_ptr<VehicleSpec> spec = specFor(w, e, c);
    if (!spec) {
        // A preset waits for its mesh to have bounds; an asset the host cannot find is refused (retried if the asset field changes).
        if (c.asset != 0) {
            if (!e.warnedNoSpec) AVER_WARN("[Vehicle] entity {}: no .ocvehicle for asset {} (the host's spec source could not find it)", e.entity, c.asset);
            e.warnedNoSpec = true;
            e.state = detail::Entry::State::Refused;
        }
        return false;
    }
    BuiltVehicle b;
    std::string why;
    if (!buildVehicle(*spec, xf, b, why)) {
        AVER_WARN("[Vehicle] entity {}: {}", e.entity, why);
        e.state = detail::Entry::State::Refused;
        return false;
    }
    // So a ray or a contact that hits the chassis can name the entity, as every level body does.
    aver_phys_set_entity(b.body, static_cast<i32>(e.entity));

    e.spec = spec;
    e.vehicle = b.vehicle;
    e.body = b.body;
    e.wheelCount = std::min<u32>(b.wheelCount, detail::kMaxWheelSlots);
    e.scale = Vec3(snapToOne(xf.scale.x), snapToOne(xf.scale.y), snapToOne(xf.scale.z));
    e.pos = e.writtenPos = xf.position;
    e.rot = e.writtenRot = xf.rotation;
    e.wrote = false;
    e.wheels.assign(b.wheelCount, WheelTelemetry{});
    e.health = specHealth(e);
    e.destroyed = false;
    e.appliedPower = e.appliedGrip = e.appliedBrake = -1.0f;
    e.appliedPeriod = 1;
    for (f32& v : e.lastIn) v = -9.0f;
    e.skidEffect = spec->effects.skid.empty() ? 0 : fnv1a64(std::string_view(spec->effects.skid));
    e.impactEffect = spec->effects.impact.empty() ? 0 : fnv1a64(std::string_view(spec->effects.impact));
    e.state = detail::Entry::State::Built;
    e.tel = Telemetry{};
    e.tel.health01 = 1.0f;
    byHandle[e.vehicle] = e.entity;
    c.vehicle = e.vehicle;
    c.outHealth01 = 1.0f;
    resolveWheelNodes(w, e);
    ++builtThisFrame;
    return true;
}

void VehiclePhysicsSystem::Impl::destroyEntry(detail::Entry& e, bool destroyPhysics) {
    if (e.driver != scene::kInvalidEntity && world) exitImpl(*world, e);
    stopVoices(e);
    if (world)
        for (const CameraRigInfo& r : e.rigs)
            if (world->valid(r.rig)) world->destroy(r.rig);
    e.rigs.clear();
    if (e.vehicle != 0) {
        byHandle.erase(e.vehicle);
        if (destroyPhysics && aver_phys_ready()) aver_phys_vehicle_destroy(e.vehicle);
    }
    e.vehicle = 0;
    e.body = 0;
    e.spec.reset();
    e.state = detail::Entry::State::Pending;
    for (bool& b : e.wheelWritten) b = false;
    e.wrote = false;
}

void VehiclePhysicsSystem::Impl::resolveAndBuild(scene::World& w) {
    std::vector<scene::Entity> ents;
    if (scene::ComponentPool* pool = w.pool(g_type))
        for (usize i = 0; i < pool->size(); ++i) ents.push_back(pool->entityAt(i));
    std::sort(ents.begin(), ents.end());

    // An entity or component that went away takes its vehicle with it: left alone it would go on driving, colliding and
    // holding a place in the world, an invisible solid ghost.
    for (auto it = entries.begin(); it != entries.end();) {
        const bool alive = w.valid(it->first) && !w.destroyPending(it->first) && std::binary_search(ents.begin(), ents.end(), it->first);
        if (alive) { ++it; continue; }
        destroyEntry(it->second, true);
        it = entries.erase(it);
    }

    for (const scene::Entity ent : ents) {
        if (!w.valid(ent) || w.destroyPending(ent)) continue;
        auto [it, inserted] = entries.try_emplace(ent);
        detail::Entry& en = it->second;
        if (inserted) en.entity = ent;
        CVehicle* c = w.component<CVehicle>(ent, g_type);
        if (!c) continue;
        // The asset or preset changed under a built (or refused) vehicle: rebuild it.
        if (en.state != detail::Entry::State::Pending && (en.keyAsset != c->asset || (c->asset == 0 && en.keyPreset != c->preset))) {
            destroyEntry(en, true);
            en.warnedNoSpec = en.warnedScale = en.warnedConflict = false;
        }
        if (en.state != detail::Entry::State::Pending) continue;
        if (builtThisFrame >= opt.buildsPerFrame) continue;
        u32 built = 0;
        for (const auto& kv : entries) built += kv.second.state == detail::Entry::State::Built ? 1u : 0u;
        if (built >= opt.maxVehicles) continue;
        buildEntry(w, en, *c);
    }
}

// ---- the frame -----------------------------------------------------------------------------------------------

void VehiclePhysicsSystem::prePhysics(scene::World& w, f32 dt) {
    (void)dt;
    Impl& m = *impl_;
    m.world = &w;
    m.builtThisFrame = 0;
    ++m.frame;
    if (!g_type || !aver_phys_ready()) return;
    m.resolveAndBuild(w);
    for (auto& [ent, en] : m.entries) {
        if (en.state != detail::Entry::State::Built) continue;
        CVehicle* c = w.component<CVehicle>(ent, g_type);
        if (!c) continue;
        m.applyInputs(w, en, *c);
    }
}

void VehiclePhysicsSystem::Impl::writePose(scene::World& w, detail::Entry& e) {
    f32 p[3], q[4];
    if (!aver_phys_vehicle_pose(e.vehicle, p, q)) return;
    const Vec3 pos{p[0], p[1], p[2]};
    const Quat rot = Quat{q[0], q[1], q[2], q[3]}.normalized();
    // A pose Jolt blew up on must not reach the scene, where it would spread to every child and every matrix composed from it.
    if (!finitePose(pos, rot)) return;
    e.pos = pos;
    e.rot = rot;
    // Only when it changed: a parked fleet dirties nothing.
    if (e.wrote && samePose(pos, rot, e.writtenPos, e.writtenRot)) return;
    Transform xf;
    xf.position = pos;
    xf.rotation = rot;
    xf.scale = e.scale;
    w.setLocalTransform(e.entity, localFor(w, e.entity, xf));
    e.writtenPos = pos;
    e.writtenRot = rot;
    e.wrote = true;
}

void VehiclePhysicsSystem::Impl::readTelemetry(scene::World& w, detail::Entry& e, CVehicle& c) {
    (void)w;
    f32 f[AVER_PHYS_VS_COUNT] = {};
    i32 iv[AVER_PHYS_VSI_COUNT] = {};
    if (aver_phys_vehicle_state(e.vehicle, f, AVER_PHYS_VS_COUNT, iv, AVER_PHYS_VSI_COUNT) == 0) return;
    static_assert(AVER_PHYS_VS_COUNT == 18 && AVER_PHYS_VSI_COUNT == 11 && AVER_PHYS_VWS_COUNT == 18 && AVER_PHYS_VWSI_COUNT == 4,
                  "Entry's raw telemetry arrays are sized to the physics blocks");
    for (int k = 0; k < AVER_PHYS_VS_COUNT; ++k) e.rawVs[k] = f[k];
    for (int k = 0; k < AVER_PHYS_VSI_COUNT; ++k) e.rawVsi[k] = iv[k];
    Telemetry& t = e.tel;
    t.speedCmS = f[AVER_PHYS_VS_SPEED_CMS];
    t.forwardSpeedCmS = f[AVER_PHYS_VS_FORWARD_SPEED_CMS];
    t.rpm = f[AVER_PHYS_VS_RPM];
    t.engineTorqueNm = f[AVER_PHYS_VS_ENGINE_TORQUE_NM];
    t.clutch = f[AVER_PHYS_VS_CLUTCH];
    t.yawRateDegS = f[AVER_PHYS_VS_YAW_RATE_DEG_S];
    t.upZ = f[AVER_PHYS_VS_UP_Z];
    t.gear = iv[AVER_PHYS_VSI_GEAR];
    t.gearCount = iv[AVER_PHYS_VSI_GEAR_COUNT];
    t.manual = iv[AVER_PHYS_VSI_MANUAL] != 0;
    t.shifting = iv[AVER_PHYS_VSI_SHIFTING] != 0;
    t.throttle = f[AVER_PHYS_VS_THROTTLE_APPLIED];
    t.brake = f[AVER_PHYS_VS_BRAKE_APPLIED];
    t.steer = f[AVER_PHYS_VS_STEER_APPLIED];
    t.handbrake = f[AVER_PHYS_VS_HANDBRAKE_APPLIED];
    t.wheelsOnGround = iv[AVER_PHYS_VSI_WHEELS_ON_GROUND];
    t.absActive = iv[AVER_PHYS_VSI_ABS_ACTIVE] != 0;
    t.tcsActive = iv[AVER_PHYS_VSI_TCS_ACTIVE] != 0;
    t.airborneSeconds = f[AVER_PHYS_VS_AIRBORNE_SECONDS];
    t.longAccelG = f[AVER_PHYS_VS_LONG_ACCEL_G];
    t.latAccelG = f[AVER_PHYS_VS_LAT_ACCEL_G];
    t.trackLeftCmS = f[AVER_PHYS_VS_TRACK_LEFT_CMS];
    t.trackRightCmS = f[AVER_PHYS_VS_TRACK_RIGHT_CMS];
    t.destroyed = e.destroyed;
    const f32 full = specHealth(e);
    t.health01 = full > 0.0f ? std::clamp(e.health / full, 0.0f, 1.0f) : 1.0f;

    // On its side: the chassis' up against the world's, for long enough (postPhysics accumulates the time).
    t.flipped = e.flippedFor >= opt.flippedSeconds;

    const u32 n = std::min<u32>(static_cast<u32>(e.wheels.size()), detail::kMaxWheelSlots);
    e.rawWheelF.assign(e.wheels.size() * AVER_PHYS_VWS_COUNT, 0.0f);
    e.rawWheelI.assign(e.wheels.size() * AVER_PHYS_VWSI_COUNT, 0);
    bool skidding = false;
    for (u32 i = 0; i < n; ++i) {
        f32 wf[AVER_PHYS_VWS_COUNT] = {};
        i32 wi[AVER_PHYS_VWSI_COUNT] = {};
        if (aver_phys_vehicle_wheel_state(e.vehicle, static_cast<i32>(i), wf, AVER_PHYS_VWS_COUNT, wi, AVER_PHYS_VWSI_COUNT) == 0) continue;
        for (int k = 0; k < AVER_PHYS_VWS_COUNT; ++k) e.rawWheelF[i * AVER_PHYS_VWS_COUNT + k] = wf[k];
        for (int k = 0; k < AVER_PHYS_VWSI_COUNT; ++k) e.rawWheelI[i * AVER_PHYS_VWSI_COUNT + k] = wi[k];
        WheelTelemetry& wt = e.wheels[i];
        wt.contact = wi[AVER_PHYS_VWSI_CONTACT] != 0;
        wt.skidding = wi[AVER_PHYS_VWSI_SKIDDING] != 0;
        wt.slipLongitudinal = wf[AVER_PHYS_VWS_SLIP_LONG];
        wt.slipLateralDeg = wf[AVER_PHYS_VWS_SLIP_LAT_DEG];
        wt.loadN = wf[AVER_PHYS_VWS_LOAD_N];
        wt.compressionCm = wf[AVER_PHYS_VWS_COMPRESSION_CM];
        wt.angularVelocityRadS = wf[AVER_PHYS_VWS_ANG_VEL_RAD_S];
        wt.steerDeg = wf[AVER_PHYS_VWS_STEER_DEG];
        wt.spinRad = wf[AVER_PHYS_VWS_SPIN_RAD];
        wt.contactPoint = Vec3(wf[AVER_PHYS_VWS_CONTACT_POINT_X], wf[AVER_PHYS_VWS_CONTACT_POINT_Y], wf[AVER_PHYS_VWS_CONTACT_POINT_Z]);
        wt.contactNormal = Vec3(wf[AVER_PHYS_VWS_CONTACT_NORMAL_X], wf[AVER_PHYS_VWS_CONTACT_NORMAL_Y], wf[AVER_PHYS_VWS_CONTACT_NORMAL_Z]);
        wt.forceLongitudinalN = wf[AVER_PHYS_VWS_FORCE_LONG_N];
        wt.forceLateralN = wf[AVER_PHYS_VWS_FORCE_LAT_N];
        wt.surfaceFriction = wf[AVER_PHYS_VWS_SURFACE_FRICTION];
        wt.brakeTorqueNm = wf[AVER_PHYS_VWS_BRAKE_TORQUE_NM];
        wt.surfaceBody = wi[AVER_PHYS_VWSI_CONTACT_BODY];
        wt.surfaceEntity = static_cast<scene::Entity>(wi[AVER_PHYS_VWSI_CONTACT_ENTITY]);
        skidding = skidding || wt.skidding;
    }

    u32 state = 0;
    if (t.speedCmS > 2.0f || std::fabs(t.throttle) > 0.01f || e.driver != scene::kInvalidEntity) state |= kVehicleStateAwake;
    if (t.wheelsOnGround > 0) state |= kVehicleStateGrounded;
    if (t.forwardSpeedCmS < -20.0f || t.gear < 0) state |= kVehicleStateReversing;
    if (t.absActive) state |= kVehicleStateAbs;
    if (t.tcsActive) state |= kVehicleStateTcs;
    if (t.flipped) state |= kVehicleStateFlipped;
    if (t.shifting) state |= kVehicleStateShifting;
    if (e.destroyed) state |= kVehicleStateDestroyed;
    if (t.manual) state |= kVehicleStateManual;
    if (skidding) state |= kVehicleStateSkidding;
    e.outState = state;

    c.driver = e.driver;
    c.vehicle = e.vehicle;
    c.outState = state;
    c.outGear = t.gear;
    c.outSpeedCmS = t.speedCmS;
    c.outForwardCmS = t.forwardSpeedCmS;
    c.outRpm = t.rpm;
    c.outHealth01 = t.health01;
    c.outThrottle = t.throttle;
    c.outSteer = t.steer;
    c.outBrake = t.brake;
    c.outWheelsOnGround = static_cast<u32>(std::max(0, t.wheelsOnGround));
}

void VehiclePhysicsSystem::Impl::processPhysicsEvents(scene::World& w, f32 dt) {
    (void)dt;
    const i32 n = aver_phys_vehicle_event_count();
    for (i32 k = 0; k < n; ++k) {
        i32 i4[4] = {};
        f32 f8[8] = {};
        if (!aver_phys_vehicle_event_get(k, i4, f8)) break;
        const auto h = byHandle.find(i4[1]);
        if (h == byHandle.end()) continue;
        detail::Entry* e = find(h->second);
        if (!e || e->state != detail::Entry::State::Built) continue;
        CVehicle* c = w.component<CVehicle>(e->entity, g_type);
        if (!c) continue;
        const Vec3 point(f8[0], f8[1], f8[2]), normal(f8[3], f8[4], f8[5]);
        switch (i4[0]) {
            case AVER_PHYS_VEV_SKID_BEGIN:
                emit(VehicleEventKind::SkidBegin, e->entity, scene::kInvalidEntity, i4[2], i4[3], point, normal, f8[6], f8[7], "VehicleSkid");
                break;
            case AVER_PHYS_VEV_SKID_END:
                emit(VehicleEventKind::SkidEnd, e->entity, scene::kInvalidEntity, i4[2], i4[3], point, normal, f8[6], f8[7], nullptr);
                break;
            case AVER_PHYS_VEV_LAND:
                emit(VehicleEventKind::Land, e->entity, scene::kInvalidEntity, i4[2], i4[3], point, normal, f8[6], f8[7], "VehicleLanded");
                if (e->spec && !e->spec->audio.impact.empty() && !(c->flags & kVehicleNoAudio) && f8[6] > 150.0f)
                    oneShot(*e, e->spec->audio.impact, std::clamp(f8[6] / 3000.0f, 0.1f, 0.6f), 1.0f);
                break;
            case AVER_PHYS_VEV_SHIFT:
                ++e->shifts;
                emit(VehicleEventKind::Shift, e->entity, scene::kInvalidEntity, i4[2], i4[3], point, normal, f8[6], f8[7], "VehicleShift");
                if (e->spec && !e->spec->audio.shift.empty() && !(c->flags & kVehicleNoAudio)) oneShot(*e, e->spec->audio.shift, 0.4f, 1.0f);
                break;
            case AVER_PHYS_VEV_FLIPPED:
                emit(VehicleEventKind::Flipped, e->entity, scene::kInvalidEntity, i4[2], i4[3], point, normal, f8[6], f8[7], "VehicleFlipped");
                break;
            case AVER_PHYS_VEV_ASSIST:
                emit(VehicleEventKind::Assist, e->entity, scene::kInvalidEntity, i4[2], i4[3], point, normal, f8[6], f8[7], nullptr);
                break;
            case AVER_PHYS_VEV_IMPACT: {
                VehicleEvent ev;
                ev.kind = VehicleEventKind::Impact;
                ev.vehicle = e->entity;
                ev.a = i4[2];
                ev.b = i4[3];
                ev.point = point;
                ev.normal = normal;
                ev.value = f8[6];
                ev.value2 = f8[7];
                onImpact(w, *e, *c, ev);
                break;
            }
            default: break;
        }
    }
}

void VehiclePhysicsSystem::postPhysics(scene::World& w, f32 dt) {
    Impl& m = *impl_;
    m.world = &w;
    if (!g_type || !aver_phys_ready()) return;

    for (auto& [ent, en] : m.entries) {
        if (en.state != detail::Entry::State::Built) continue;
        CVehicle* c = w.component<CVehicle>(ent, g_type);
        if (!c) continue;
        m.writePose(w, en);
        // Time on its side, from this frame's dt and the telemetry of the step that just ran.
        {
            f32 vs[AVER_PHYS_VS_COUNT] = {};
            if (aver_phys_vehicle_state(en.vehicle, vs, AVER_PHYS_VS_COUNT, nullptr, 0)) {
                const f32 pr = en.spec ? en.spec->body.pitchRollDeg : 60.0f;
                const f32 limit = (pr > 0.0f && pr < 180.0f) ? std::cos(radians(pr)) + 0.05f : 0.3f;
                en.flippedFor = vs[AVER_PHYS_VS_UP_Z] < limit ? en.flippedFor + dt : 0.0f;
            }
        }
        m.readTelemetry(w, en, *c);
        if (!(c->flags & kVehicleNoWheelVisuals)) m.writeWheelVisuals(w, en, *c);
        m.snapDriver(w, en);
    }
    m.processPhysicsEvents(w, dt);
    for (auto& [ent, en] : m.entries) {
        if (en.state != detail::Entry::State::Built) continue;
        CVehicle* c = w.component<CVehicle>(ent, g_type);
        if (c) m.updateEffects(w, en, *c, dt);
    }
    m.updateAudio(w, dt);

    u32 built = 0, driven = 0, pending = 0;
    for (const auto& kv : m.entries) {
        if (kv.second.state == detail::Entry::State::Built) { ++built; driven += kv.second.driver != scene::kInvalidEntity ? 1u : 0u; }
        else ++pending;
    }
    m.stats.vehicles = built;
    m.stats.driven = driven;
    m.stats.pending = pending;
    m.stats.builtThisFrame = m.builtThisFrame;
}

void VehiclePhysicsSystem::clear() {
    Impl& m = *impl_;
    for (auto& kv : m.entries) m.destroyEntry(kv.second, true);
    m.entries.clear();
    m.byHandle.clear();
    m.inlineSpecs.clear();
    m.sounds.clear();
    m.events.clear();
    m.seq = 0;
    m.stats = Stats{};
    m.builtThisFrame = 0;
}

// ---- reading -----------------------------------------------------------------------------------------------------

bool VehiclePhysicsSystem::isVehicle(scene::Entity e) const {
    const detail::Entry* en = impl_->find(e);
    return en && en->state == detail::Entry::State::Built;
}

bool VehiclePhysicsSystem::telemetry(scene::Entity e, Telemetry& out) const {
    const detail::Entry* en = impl_->find(e);
    if (!en || en->state != detail::Entry::State::Built) return false;
    out = en->tel;
    return true;
}

bool VehiclePhysicsSystem::wheel(scene::Entity e, u32 i, WheelTelemetry& out) const {
    const detail::Entry* en = impl_->find(e);
    if (!en || en->state != detail::Entry::State::Built || i >= en->wheels.size()) return false;
    out = en->wheels[i];
    return true;
}

u32 VehiclePhysicsSystem::wheelCount(scene::Entity e) const {
    const detail::Entry* en = impl_->find(e);
    return (en && en->state == detail::Entry::State::Built) ? static_cast<u32>(en->wheels.size()) : 0u;
}

i32 VehiclePhysicsSystem::physicsHandle(scene::Entity e) const {
    const detail::Entry* en = impl_->find(e);
    return (en && en->state == detail::Entry::State::Built) ? en->vehicle : 0;
}

i32 VehiclePhysicsSystem::collisionPeriodOf(scene::Entity e) const {
    const detail::Entry* en = impl_->find(e);
    return (en && en->state == detail::Entry::State::Built) ? en->appliedPeriod : 1;
}

const VehicleSpec* VehiclePhysicsSystem::specOf(scene::Entity e) const {
    const detail::Entry* en = impl_->find(e);
    return (en && en->state == detail::Entry::State::Built) ? en->spec.get() : nullptr;
}

scene::Entity VehiclePhysicsSystem::driverOf(scene::Entity vehicle) const {
    const detail::Entry* en = impl_->find(vehicle);
    return en ? en->driver : scene::kInvalidEntity;
}

u32 VehiclePhysicsSystem::drainEvents(VehicleEvent* out, u32 max) {
    Impl& m = *impl_;
    const u32 n = std::min<u32>(max, static_cast<u32>(m.events.size()));
    for (u32 i = 0; i < n; ++i) out[i] = m.events[i];
    m.events.erase(m.events.begin(), m.events.begin() + n);
    return n;
}

Stats VehiclePhysicsSystem::stats() const { return impl_->stats; }

std::vector<scene::Entity> VehiclePhysicsSystem::entities() const {
    std::vector<scene::Entity> out;
    for (const auto& [ent, en] : impl_->entries) {
        if (en.state != detail::Entry::State::Built) continue;
        out.push_back(ent);
        for (u32 i = 0; i < en.wheelCount; ++i)
            if (en.wheelNode[i] != scene::kInvalidEntity) out.push_back(en.wheelNode[i]);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

DebugView VehiclePhysicsSystem::debug(scene::Entity ent) const {
    DebugView v;
    const detail::Entry* en = impl_->find(ent);
    if (!en || en->state != detail::Entry::State::Built || !en->spec) return v;
    const VehicleSpec& s = *en->spec;
    v.valid = true;
    v.spec = en->spec.get();
    v.position = en->pos;
    v.rotation = en->rot;
    v.telemetry = en->tel;
    f32 vel[3] = {};
    if (aver_phys_vehicle_velocity(en->vehicle, vel)) v.velocity = Vec3(vel[0], vel[1], vel[2]);
    v.comWorld = en->pos + en->rot.rotate(s.body.com);
    v.wheelCount = std::min<u32>(en->wheelCount, 16u);
    for (u32 i = 0; i < v.wheelCount; ++i) {
        const WheelDef& wd = s.wheels[i];
        const WheelClass* wc = findClass(s, wd.wheelClass);
        DebugView::Wheel& dw = v.wheels[i];
        const f32 suspMax = wc ? wc->suspMax : 30.0f;
        dw.radiusCm = wc ? wc->radius : 33.0f;
        dw.suspMaxCm = suspMax;
        dw.attach = en->pos + en->rot.rotate(Vec3(wd.hub.x, wd.hub.y, wd.hub.z + suspMax - springSagCm(wc ? wc->suspHz : 2.0f)));
        f32 p[3], q[4];
        const bool havePose = aver_phys_vehicle_wheel_pose(en->vehicle, static_cast<i32>(i), p, q);
        dw.hub = havePose ? en->pos + en->rot.rotate(Vec3(p[0], p[1], p[2])) : dw.attach;
        dw.axle = en->rot.rotate(havePose ? Quat(q[0], q[1], q[2], q[3]).rotate(Vec3(0, 1, 0)) : Vec3(0, 1, 0));
        const WheelTelemetry& wt = en->wheels[i];
        dw.inContact = wt.contact;
        dw.skidding = wt.skidding;
        dw.contactPoint = wt.contactPoint;
        dw.normal = wt.contactNormal;
        dw.compressionCm = wt.compressionCm;
        dw.slipLong = wt.slipLongitudinal;
        dw.slipLatDeg = wt.slipLateralDeg;
        dw.forceLongN = wt.forceLongitudinalN;
        dw.forceLatN = wt.forceLateralN;
    }
    return v;
}

// ---- spawning ------------------------------------------------------------------------------------------------------

scene::Entity VehiclePhysicsSystem::spawn(scene::World& w, std::string_view text, const Vec3& position, f32 yawDegrees, i32* outVehicle) {
    Impl& m = *impl_;
    m.world = &w;
    if (!g_type || !aver_phys_ready()) { AVER_WARN("[Vehicle] spawn: the vehicle system or the physics world is not running"); return scene::kInvalidEntity; }
    u64 asset = 0;
    u32 preset = 0;
    if (startsWithOcVehicle(text)) {
        auto spec = std::make_shared<VehicleSpec>();
        std::string err;
        if (!parseOcVehicle(text, *spec, err) || !validate(*spec, err)) { AVER_WARN("[Vehicle] spawn: inline spec: {}", err); return scene::kInvalidEntity; }
        asset = fnv1a64(text) | 0x8000000000000000ull;
        m.inlineSpecs[asset] = std::move(spec);
    } else if (const int idx = presetIndex(text); idx >= 0) {
        auto spec = std::make_shared<VehicleSpec>();
        if (!specFromBounds(presetName(static_cast<u32>(idx)), defaultSizeOf(idx), *spec)) return scene::kInvalidEntity;
        preset = static_cast<u32>(idx);
        asset = fnv1a64(std::string("preset:") + presetName(static_cast<u32>(idx))) | 0x8000000000000000ull;
        m.inlineSpecs[asset] = std::move(spec);
    } else {
        asset = fnv1a64(text);
        const VehicleSpec* found = m.specSource ? m.specSource(asset) : nullptr;
        if (!found) { AVER_WARN("[Vehicle] spawn: no .ocvehicle '{}'", std::string(text)); return scene::kInvalidEntity; }
    }

    Transform xf;
    xf.position = position;
    xf.rotation = Quat::fromAxisAngle(Vec3(0, 0, 1), radians(yawDegrees));
    const scene::Entity e = w.create("Vehicle");
    if (e == scene::kInvalidEntity) return scene::kInvalidEntity;
    w.setLocalTransform(e, xf);
    CVehicle* c = attach(w, e, asset, preset);
    if (!c) { w.destroy(e); return scene::kInvalidEntity; }
    detail::Entry& en = m.entries[e];
    en.entity = e;
    if (!m.buildEntry(w, en, *c)) {
        w.destroy(e);
        m.entries.erase(e);
        return scene::kInvalidEntity;
    }
    if (outVehicle) *outVehicle = en.vehicle;
    return e;
}

}  // namespace aver::vehicle
