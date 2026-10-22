// PhysicsStateEngine.cpp -- the parts of a snapshot Jolt does not write: the manifest every restore is checked against, and
// the engine section (the fixed-step accumulator, vehicle driver-aid state, a character's ground carry, and any registered
// participant). See physics_state_abi.h for the contract; PhysicsState.cpp assembles the blob and runs the restore.
//
// WHAT IS STATE AND WHAT IS NOT. The engine section holds what the simulation DERIVES between inputs: the aid smoothing a
// vehicle carries from one step to the next (applied pedals, the traction-control factor, the ABS flags, the gear timers),
// its last-step velocity and accelerations (the next step's aids read them), and the ground velocity a character is carrying.
// Everything a game SETS is an input and is re-issued by a rollback layer for each resimulated tick: tune values, builder
// data, ragdoll mode and strength, wind, forces. (The pedals as set are saved too: Jolt's own controller state holds the
// pedals it applies, so a restore that left the requested ones behind would disagree with it.) The telemetry arrays the vehicle
// step recomputes (VehicleEntry::vs / vsi / live[].f / live[].i) ARE saved: they are derived, but the next step's traction control
// and ABS read the last step's slip and contact flag out of them, so exact replay needs the values the capture saw.
#include "PhysicsStateRecorder.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cstring>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr uint32_t kNoBody = 0xFFFFFFFFu;
// One wheel in the engine section: absFactor, airborne, skidPeak, the flag word, then the last step's f[] and i[].
constexpr size_t kWheelBytes = 16 + sizeof(float) * AVER_PHYS_VWS_COUNT + sizeof(int32_t) * AVER_PHYS_VWSI_COUNT;

std::vector<StateParticipant>& participants() {
    static std::vector<StateParticipant> list;   // function-local: registration may run at static-init time
    return list;
}

uint32_t rawId(const JPH::BodyID& id) { return id.IsInvalid() ? kNoBody : id.GetIndexAndSequenceNumber(); }

bool contains(const std::vector<int32_t>& sorted, int32_t v) { return std::binary_search(sorted.begin(), sorted.end(), v); }

// Is a body of the live system one the capture would have saved (the same filter PhysicsState.cpp hands Jolt)?
bool savedBody(const JPH::Body& b, bool statics) { return b.IsInBroadPhase() && (statics || !b.IsStatic()); }

} // namespace

namespace aver::physics::detail {

void registerStateParticipant(const StateParticipant& p) {
    if (!p.name || !p.save || !p.restore) return;
    for (const StateParticipant& q : participants())
        if (fnv1a64(std::string_view(q.name)) == fnv1a64(std::string_view(p.name))) return;   // already registered
    participants().push_back(p);
}

// ---- the manifest ----------------------------------------------------------------------------------------------------

void stateBuildManifest(uint32_t flags, StateManifest& m) {
    World& w = *g_world;
    m = StateManifest{};
    if (flags & AVER_PHYS_SNAP_BODIES) {
        const bool statics = (flags & AVER_PHYS_SNAP_STATICS) != 0;
        JPH::BodyIDVector ids;
        w.system.GetBodies(ids);
        const JPH::BodyLockInterfaceNoLock& locks = w.system.GetBodyLockInterfaceNoLock();
        m.bodyIds.reserve(ids.size());
        for (const JPH::BodyID& id : ids) {
            const JPH::Body* b = locks.TryGetBody(id);
            if (b && savedBody(*b, statics)) m.bodyIds.push_back(id.GetIndexAndSequenceNumber());
        }
        std::sort(m.bodyIds.begin(), m.bodyIds.end());
    }
    if (flags & (AVER_PHYS_SNAP_CHARACTERS | AVER_PHYS_SNAP_ENGINE)) {
        std::vector<int32_t> handles;
        handles.reserve(w.characters.size());
        for (const auto& [h, c] : w.characters) handles.push_back(h);
        std::sort(handles.begin(), handles.end());
        for (const int32_t h : handles) m.characters.push_back({h, rawId(w.characters.at(h)->GetInnerBodyID())});
    }
    if (flags & (AVER_PHYS_SNAP_CONSTRAINTS | AVER_PHYS_SNAP_ENGINE)) {
        for (const auto& [h, e] : w.vehicles) {   // an ordered map: handle order
            if (!e.constraint) continue;           // a vehicle still being built has nothing to restore
            const auto b = w.bodies.find(e.body);
            m.vehicles.push_back({h, e.body, b == w.bodies.end() ? kNoBody : rawId(b->second),
                                  static_cast<uint32_t>(e.live.size())});
        }
    }
    if (flags & AVER_PHYS_SNAP_CONSTRAINTS) {
        jointHandles(m.joints);
        ragdollHandles(m.ragdolls);
    }
    m.constraintCount = static_cast<uint32_t>(w.system.GetConstraints().size());
}

void stateWriteManifest(ByteWriter& w, const StateManifest& m) {
    w.u32(static_cast<uint32_t>(m.bodyIds.size()));
    for (const uint32_t id : m.bodyIds) w.u32(id);
    w.u32(static_cast<uint32_t>(m.characters.size()));
    for (const StateManifest::Char& c : m.characters) { w.i32(c.handle); w.u32(c.inner); }
    w.u32(static_cast<uint32_t>(m.vehicles.size()));
    for (const StateManifest::Veh& v : m.vehicles) { w.i32(v.handle); w.i32(v.body); w.u32(v.bodyId); w.u32(v.wheels); }
    w.u32(static_cast<uint32_t>(m.joints.size()));
    for (const int32_t h : m.joints) w.i32(h);
    w.u32(static_cast<uint32_t>(m.ragdolls.size()));
    for (const int32_t h : m.ragdolls) w.i32(h);
    w.u32(m.constraintCount);
}

bool stateReadManifest(ByteReader& r, StateManifest& m) {
    m = StateManifest{};
    // A count is checked against the bytes left before anything is allocated: an imported blob is untrusted.
    auto count = [&](size_t perItem, uint32_t& n) {
        n = r.u32();
        return r.ok() && static_cast<uint64_t>(n) * perItem <= r.left();
    };
    uint32_t n = 0;
    if (!count(4, n)) return false;
    m.bodyIds.resize(n);
    for (uint32_t& id : m.bodyIds) id = r.u32();
    if (!count(8, n)) return false;
    m.characters.resize(n);
    for (StateManifest::Char& c : m.characters) { c.handle = r.i32(); c.inner = r.u32(); }
    if (!count(16, n)) return false;
    m.vehicles.resize(n);
    for (StateManifest::Veh& v : m.vehicles) { v.handle = r.i32(); v.body = r.i32(); v.bodyId = r.u32(); v.wheels = r.u32(); }
    if (!count(4, n)) return false;
    m.joints.resize(n);
    for (int32_t& h : m.joints) h = r.i32();
    if (!count(4, n)) return false;
    m.ragdolls.resize(n);
    for (int32_t& h : m.ragdolls) h = r.i32();
    m.constraintCount = r.u32();
    return r.ok();
}

int32_t stateCheckManifest(const StateManifest& m, uint32_t flags, uint32_t options, uint32_t constraintEpoch) {
    World& w = *g_world;
    const bool strict = (options & AVER_PHYS_RESTORE_STRICT) != 0;
    const JPH::BodyLockInterfaceNoLock& locks = w.system.GetBodyLockInterfaceNoLock();

    // Bodies: every id the Jolt section holds must still name a body in the broad phase (the sequence number in the id makes a
    // recycled slot a different id). Under STRICT, no body the capture would now include may be missing from the manifest.
    if (flags & AVER_PHYS_SNAP_BODIES) {
        for (const uint32_t raw : m.bodyIds) {
            const JPH::Body* b = locks.TryGetBody(JPH::BodyID(raw));
            if (!b || !b->IsInBroadPhase()) return AVER_PHYS_RESTORE_TOPOLOGY;
        }
        if (strict) {
            const bool statics = (flags & AVER_PHYS_SNAP_STATICS) != 0;
            JPH::BodyIDVector ids;
            w.system.GetBodies(ids);
            for (const JPH::BodyID& id : ids) {
                const JPH::Body* b = locks.TryGetBody(id);
                if (b && savedBody(*b, statics) &&
                    !std::binary_search(m.bodyIds.begin(), m.bodyIds.end(), id.GetIndexAndSequenceNumber()))
                    return AVER_PHYS_RESTORE_TOPOLOGY;
            }
        }
    }

    // Characters: each must still exist with the same inner body (set_inner_body rebuilds the character under the same handle).
    if (flags & (AVER_PHYS_SNAP_CHARACTERS | AVER_PHYS_SNAP_ENGINE)) {
        for (const StateManifest::Char& c : m.characters) {
            const auto it = w.characters.find(c.handle);
            if (it == w.characters.end() || rawId(it->second->GetInnerBodyID()) != c.inner) return AVER_PHYS_RESTORE_TOPOLOGY;
        }
        if (strict && w.characters.size() != m.characters.size()) return AVER_PHYS_RESTORE_TOPOLOGY;
    }

    // Vehicles: the same finished vehicle, on the same chassis, with the same wheel count.
    if (flags & (AVER_PHYS_SNAP_CONSTRAINTS | AVER_PHYS_SNAP_ENGINE)) {
        size_t finished = 0;
        for (const auto& [h, e] : w.vehicles) finished += e.constraint ? 1 : 0;
        for (const StateManifest::Veh& v : m.vehicles) {
            const auto it = w.vehicles.find(v.handle);
            if (it == w.vehicles.end() || !it->second.constraint || it->second.body != v.body ||
                it->second.live.size() != v.wheels)
                return AVER_PHYS_RESTORE_TOPOLOGY;
            const auto b = w.bodies.find(v.body);
            if (b == w.bodies.end() || rawId(b->second) != v.bodyId) return AVER_PHYS_RESTORE_TOPOLOGY;
        }
        if (strict && finished != m.vehicles.size()) return AVER_PHYS_RESTORE_TOPOLOGY;
    }

    // Constraints: joints and ragdolls by handle, and the constraint set itself. Jolt restores constraints BY ARRAY INDEX, so a
    // single add or remove since the capture (the epoch) would restore the wrong constraint without any error; the count is a
    // backstop for a constraint added by a file that does not call noteConstraintTopology.
    if (flags & AVER_PHYS_SNAP_CONSTRAINTS) {
        std::vector<int32_t> joints, ragdolls;
        jointHandles(joints);
        ragdollHandles(ragdolls);
        for (const int32_t h : m.joints) if (!contains(joints, h)) return AVER_PHYS_RESTORE_TOPOLOGY;
        for (const int32_t h : m.ragdolls) if (!contains(ragdolls, h)) return AVER_PHYS_RESTORE_TOPOLOGY;
        if (strict && (joints.size() != m.joints.size() || ragdolls.size() != m.ragdolls.size())) return AVER_PHYS_RESTORE_TOPOLOGY;
        if (w.constraintEpoch != constraintEpoch) return AVER_PHYS_RESTORE_TOPOLOGY;
        if (w.system.GetConstraints().size() != m.constraintCount) return AVER_PHYS_RESTORE_TOPOLOGY;
    }
    return AVER_PHYS_RESTORE_OK;
}

// ---- the engine section ----------------------------------------------------------------------------------------------

void stateWriteEngine(ByteWriter& w, const StateManifest& m) {
    World& world = *g_world;
    w.f32(world.accumulator);

    w.u32(static_cast<uint32_t>(m.vehicles.size()));
    for (const StateManifest::Veh& mv : m.vehicles) {
        const VehicleEntry& e = world.vehicles.at(mv.handle);   // the manifest was built from this table a moment ago
        w.i32(mv.handle);
        for (const float f : {e.reqT, e.reqS, e.reqB, e.reqH, e.appT, e.appS, e.appB, e.appH, e.tcsFactor}) w.f32(f);
        w.u32((e.absActive ? 1u : 0u) | (e.tcsActive ? 2u : 0u) | (e.flippedSent ? 4u : 0u));
        w.i32(e.gear);
        w.i32(e.lastGear);
        for (const float f : {e.shiftLeft, e.clutchRelease, e.lastRpm, e.lastVel[0], e.lastVel[1], e.lastVel[2], e.longAccelG,
                              e.latAccelG, e.airborneAll, e.sideSeconds})
            w.f32(f);
        for (const float f : e.vs) w.f32(f);
        for (const int32_t v : e.vsi) w.i32(v);
        w.u32(static_cast<uint32_t>(e.live.size()));
        for (const VehicleWheelLive& wl : e.live) {
            w.f32(wl.absFactor);
            w.f32(wl.airborne);
            w.f32(wl.skidPeak);
            w.u32((wl.hadContact ? 1u : 0u) | (wl.skidding ? 2u : 0u));
            for (const float f : wl.f) w.f32(f);
            for (const int32_t v : wl.i) w.i32(v);
        }
    }

    w.u32(static_cast<uint32_t>(m.characters.size()));
    for (const StateManifest::Char& mc : m.characters) {
        const auto it = world.characterCarry.find(mc.handle);
        w.i32(mc.handle);
        w.u32(it != world.characterCarry.end() ? 1u : 0u);
        const JPH::Vec3 v = it != world.characterCarry.end() ? it->second : JPH::Vec3::sZero();
        w.f32(v.GetX());
        w.f32(v.GetY());
        w.f32(v.GetZ());
    }

    w.u32(static_cast<uint32_t>(participants().size()));
    std::vector<uint8_t> scratch;
    for (const StateParticipant& p : participants()) {
        scratch.clear();
        p.save(scratch);
        w.u64(fnv1a64(std::string_view(p.name)));
        w.u32(static_cast<uint32_t>(scratch.size()));
        w.bytes(scratch.data(), scratch.size());
    }
}

bool stateStageEngine(const uint8_t* data, size_t size, const StateManifest& m, EngineStage& out) {
    out = EngineStage{};
    World& world = *g_world;
    ByteReader r(data, size);
    out.accumulator = r.f32();

    uint32_t nVeh = r.u32();
    if (!r.ok() || nVeh != m.vehicles.size()) return false;
    out.vehicles.resize(nVeh);
    for (EngineStage::Veh& v : out.vehicles) {
        v.handle = r.i32();
        float f[9];
        for (float& x : f) x = r.f32();
        v.req[0] = f[0]; v.req[1] = f[1]; v.req[2] = f[2]; v.req[3] = f[3];
        v.app[0] = f[4]; v.app[1] = f[5]; v.app[2] = f[6]; v.app[3] = f[7];
        v.tcsFactor = f[8];
        const uint32_t bits = r.u32();
        v.absActive = (bits & 1u) != 0; v.tcsActive = (bits & 2u) != 0; v.flippedSent = (bits & 4u) != 0;
        v.gear = r.i32();
        v.lastGear = r.i32();
        v.shiftLeft = r.f32(); v.clutchRelease = r.f32(); v.lastRpm = r.f32();
        v.lastVel[0] = r.f32(); v.lastVel[1] = r.f32(); v.lastVel[2] = r.f32();
        v.longAccelG = r.f32(); v.latAccelG = r.f32(); v.airborneAll = r.f32(); v.sideSeconds = r.f32();
        for (float& x : v.vs) x = r.f32();
        for (int32_t& x : v.vsi) x = r.i32();
        const uint32_t nWheels = r.u32();
        if (!r.ok() || static_cast<uint64_t>(nWheels) * kWheelBytes > r.left()) return false;
        v.wheels.resize(nWheels);
        for (EngineStage::Wheel& wl : v.wheels) {
            wl.absFactor = r.f32(); wl.airborne = r.f32(); wl.skidPeak = r.f32();
            const uint32_t wb = r.u32();
            wl.hadContact = (wb & 1u) != 0; wl.skidding = (wb & 2u) != 0;
            for (float& x : wl.f) x = r.f32();
            for (int32_t& x : wl.i) x = r.i32();
        }
        // The vehicle must still be the one captured: finished, with this many wheels.
        const auto it = world.vehicles.find(v.handle);
        if (!r.ok() || it == world.vehicles.end() || it->second.live.size() != v.wheels.size()) return false;
    }

    uint32_t nCarry = r.u32();
    if (!r.ok() || nCarry != m.characters.size()) return false;
    out.carry.resize(nCarry);
    for (EngineStage::Carry& c : out.carry) {
        c.handle = r.i32();
        c.present = r.u32() != 0;
        c.v[0] = r.f32(); c.v[1] = r.f32(); c.v[2] = r.f32();
    }

    uint32_t nPart = r.u32();
    if (!r.ok() || static_cast<uint64_t>(nPart) * 12u > r.left()) return false;
    out.participants.resize(nPart);
    for (EngineStage::Part& p : out.participants) {
        p.nameHash = r.u64();
        const uint32_t n = r.u32();
        const uint8_t* at = nullptr;
        if (!r.take(n, at)) return false;
        p.data.assign(at, at + n);
    }
    return r.ok();
}

void stateApplyEngine(const EngineStage& s) {
    World& world = *g_world;
    world.accumulator = s.accumulator;

    for (const EngineStage::Veh& v : s.vehicles) {
        VehicleEntry& e = world.vehicles.at(v.handle);   // stateStageEngine proved it exists with this many wheels
        e.reqT = v.req[0]; e.reqS = v.req[1]; e.reqB = v.req[2]; e.reqH = v.req[3];
        e.appT = v.app[0]; e.appS = v.app[1]; e.appB = v.app[2]; e.appH = v.app[3];
        e.tcsFactor = v.tcsFactor;
        e.absActive = v.absActive; e.tcsActive = v.tcsActive; e.flippedSent = v.flippedSent;
        e.gear = v.gear; e.lastGear = v.lastGear;
        e.shiftLeft = v.shiftLeft; e.clutchRelease = v.clutchRelease; e.lastRpm = v.lastRpm;
        std::memcpy(e.lastVel, v.lastVel, sizeof e.lastVel);
        e.longAccelG = v.longAccelG; e.latAccelG = v.latAccelG; e.airborneAll = v.airborneAll; e.sideSeconds = v.sideSeconds;
        std::memcpy(e.vs, v.vs, sizeof e.vs);
        std::memcpy(e.vsi, v.vsi, sizeof e.vsi);
        for (size_t i = 0; i < v.wheels.size(); ++i) {
            VehicleWheelLive& wl = e.live[i];
            wl.absFactor = v.wheels[i].absFactor; wl.airborne = v.wheels[i].airborne; wl.skidPeak = v.wheels[i].skidPeak;
            wl.hadContact = v.wheels[i].hadContact; wl.skidding = v.wheels[i].skidding;
            std::memcpy(wl.f, v.wheels[i].f, sizeof wl.f);
            std::memcpy(wl.i, v.wheels[i].i, sizeof wl.i);
        }
    }

    for (const EngineStage::Carry& c : s.carry) {
        if (!world.characters.count(c.handle)) continue;
        if (c.present) world.characterCarry[c.handle] = JPH::Vec3(c.v[0], c.v[1], c.v[2]);
        else world.characterCarry.erase(c.handle);
    }

    for (const EngineStage::Part& p : s.participants) {
        for (const StateParticipant& q : participants()) {
            if (fnv1a64(std::string_view(q.name)) != p.nameHash) continue;
            if (!q.restore(p.data.data(), p.data.size())) AVER_WARN("[Physics] state participant '{}' could not restore its section", q.name);
            break;
        }
    }
}

} // namespace aver::physics::detail
