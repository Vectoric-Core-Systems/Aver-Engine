// PhysicsOrigin.cpp -- the floating origin's physics half (physics_origin_abi.h; LARGE_WORLD 2.3).
//
// ONE FRAME. Jolt's world is the scene's frame in metres through the unchanged Convert.hpp, so when the host rebases the origin by
// delta (engine centimetres) every body, character and anchor here moves by -toJolt(delta) in the same slot between two steps and
// every aver_phys_* position keeps meaning what it meant. delta is a whole number of regions (1,638,400 cm = 16,384 m), so the
// Jolt-space shift is a whole number of metres and exact in f32; a body 8,192 m or more from the origin on a shifted axis loses
// nothing (Sterbenz), and anything within 1 km of the camera, which is at least that far out when the origin rebases, lands exactly.
//
// WHAT MOVES, in order: (1) the joint seam captures what a pulley rebuild needs, (2) every body in World::bodies through
// BodyInterface::SetPosition with DontActivate, so a sleeper stays asleep and an unadded body moves without a broad-phase notify,
// (3) every character, then RefreshContacts (its active contacts hold world positions), (4) the joint seam's anchors and pulleys,
// (5) the contact, vehicle-impact and vehicle-event points still waiting to be drained, (6) the registered hooks.
// A character's inner body is not in World::bodies (Jolt owns it), CharacterVirtual::SetPosition moves it, and the loop skips it
// by id regardless so it can never move twice.
//
// A SNAPSHOT RESTORE crosses the same code (originReshiftRestored): the blob is in the frame it was taken in, so what it restored is
// moved by the difference of the two origin totals, and nothing else is.
#include "PhysicsOrigin.hpp"
#include "PhysicsCharacterInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_origin_abi.h"

#include <Jolt/Physics/Body/BodyLock.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

// More statics than this moved in one shift and the next step opens with OptimizeBroadPhase: moving many static leaves loosens the tree.
constexpr int32_t kOptimizeAboveStatics = 4096;
// 1.6e9 cm = 1.6e7 m, under 2^24, so every whole metre of the Jolt-space delta is exact in f32.
constexpr double kMaxDeltaCm = 1.6e9;

// The test seam below hands a hook the delta as three doubles and a user pointer; the module's own hooks take the Vec3d.
using SeamHookFn = void (*)(double dxCm, double dyCm, double dzCm, void* user);
struct Hook {
    std::string name;
    OriginHookFn fn = nullptr;
    SeamHookFn   seam = nullptr;
    void*        user = nullptr;
};
std::vector<Hook>& hooks() { static std::vector<Hook> h; return h; }

void putHook(Hook h) {
    for (Hook& e : hooks())
        if (e.name == h.name) { e = std::move(h); return; }   // a second registration under one name replaces the first, in place
    hooks().push_back(std::move(h));
}

OriginStats g_last;
uint64_t    g_lastWorldId = 0;   // the world g_last belongs to: a new world reads as "no shift yet"
bool        g_shifting = false;  // a hook calling back into the shift is refused

// A delta that passed validation, in both frames.
struct Delta {
    Vec3d     cm;   // engine axes, centimetres: the origin's move
    JPH::Vec3 dj;   // the same in Jolt's axes and metres (toJolt): positions change by -dj
};

// toJolt(v) = (v.y, v.z, -v.x) / 100, in double so the metres stay whole; "+ 0.0f" turns -0 into +0.
JPH::Vec3 joltDelta(double dxCm, double dyCm) {
    return JPH::Vec3(static_cast<float>(dyCm / 100.0), 0.0f, static_cast<float>(-dxCm / 100.0) + 0.0f);
}

// A whole number of metres on each axis, finite and not absurd.
bool wholeMetres(double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
    if (std::fabs(dx) > kMaxDeltaCm || std::fabs(dy) > kMaxDeltaCm) return false;
    return std::fmod(dx, 100.0) == 0.0 && std::fmod(dy, 100.0) == 0.0;
}

bool parseDelta(double dx, double dy, double dz, Delta& out) {
    if (!std::isfinite(dz) || dz != 0.0) return false;   // Z is never shifted
    if (dx == 0.0 && dy == 0.0) return false;            // nothing to do is not a shift: no generation, no hooks
    if (!wholeMetres(dx, dy)) return false;
    out.cm = Vec3d{dx, dy, 0.0};
    out.dj = joltDelta(dx, dy);
    return true;
}

AbiError checkShift(double dx, double dy, double dz, Delta& out) {
    if (!g_world) return AbiError::NotInitialised;
    if (g_world->inStep || g_shifting) return AbiError::Unsupported;   // between steps only, and never from a hook
    if (!parseDelta(dx, dy, dz, out)) return AbiError::InvalidArgument;
    return AbiError::Ok;
}

void shiftPoint(float* p, const Vec3d& d) {
    p[0] = static_cast<float>(static_cast<double>(p[0]) - d.x);
    p[1] = static_cast<float>(static_cast<double>(p[1]) - d.y);
    p[2] = static_cast<float>(static_cast<double>(p[2]) - d.z);
}
void shiftPoint(Vec3& p, const Vec3d& d) {
    p.x = static_cast<float>(static_cast<double>(p.x) - d.x);
    p.y = static_cast<float>(static_cast<double>(p.y) - d.y);
    p.z = static_cast<float>(static_cast<double>(p.z) - d.z);
}

// Points the world still holds for the host to drain. The queues are small and drained between steps; shifting them is simpler than draining.
void shiftPendingEvents(const Vec3d& d) {
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    for (ContactEvent& e : g_world->contacts) shiftPoint(e.point, d);
    for (VehicleImpactRaw& r : g_world->vehicleImpacts) shiftPoint(r.point, d);
    // Only the kinds that carry a point in f[0..2]; SHIFT, FLIPPED and ASSIST write a zero point that means "none".
    for (VehicleEventRec& r : g_world->vehicleEvents)
        if (r.kind >= AVER_PHYS_VEV_SKID_BEGIN && r.kind <= AVER_PHYS_VEV_IMPACT) shiftPoint(r.f, d);
}

// Moves one body by -dj without activating it. False when the id names no body. isStatic is set for a body that was found.
bool moveBody(const JPH::BodyID& id, const JPH::Vec3& dj, bool& isStatic) {
    JPH::RVec3 pos;
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return false;
        pos = lock.GetBody().GetPosition();
        isStatic = lock.GetBody().IsStatic();
    }
    bi().SetPosition(id, pos - JPH::RVec3(dj), JPH::EActivation::DontActivate);
    return true;
}

// SetPosition, NOT aver_phys_character_set_position (it erases characterCarry, a velocity that must survive). Every character moves
// before any refreshes, so character-against-character contacts see the new frame.
void moveCharacters(const std::vector<JPH::CharacterVirtual*>& characters, const JPH::Vec3& dj) {
    for (JPH::CharacterVirtual* c : characters) c->SetPosition(c->GetPosition() - JPH::RVec3(dj));
    for (JPH::CharacterVirtual* c : characters)
        c->RefreshContacts(g_world->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                           g_world->system.GetDefaultLayerFilter(Layers::MOVING),
                           characterBodyFilter(), {}, *g_world->temp);
}

} // namespace

namespace aver::physics::detail {

void physicsOriginHook(const char* name, OriginHookFn fn) {
    if (!name || !fn) return;
    Hook h;
    h.name = name;
    h.fn = fn;
    putHook(std::move(h));
}

const OriginStats& lastOriginStats() {
    static const OriginStats kNone;
    return g_world && g_lastWorldId == g_world->id ? g_last : kNone;
}

void originReshiftRestored(const std::vector<uint32_t>& bodyIds, const std::vector<int32_t>& characterHandles, const Vec3d& deltaCm) {
    if (!g_world) return;
    if ((deltaCm.x == 0.0 && deltaCm.y == 0.0) || !wholeMetres(deltaCm.x, deltaCm.y)) return;
    const JPH::Vec3 dj = joltDelta(deltaCm.x, deltaCm.y);

    // A character that was restored moves with SetPosition, which carries its inner body; that body is also in the manifest, so it is skipped there.
    std::vector<JPH::CharacterVirtual*> characters;
    std::unordered_set<JPH::BodyID> inner;
    for (const int32_t h : characterHandles) {
        JPH::CharacterVirtual* c = findCharacter(h);
        if (!c) continue;
        characters.push_back(c);
        if (!c->GetInnerBodyID().IsInvalid()) inner.insert(c->GetInnerBodyID());
    }

    int32_t statics = 0;
    for (const uint32_t raw : bodyIds) {
        const JPH::BodyID id(raw);
        if (!inner.empty() && inner.count(id)) continue;
        bool isStatic = false;
        if (moveBody(id, dj, isStatic) && isStatic) ++statics;
    }
    moveCharacters(characters, dj);
    if (statics > kOptimizeAboveStatics) g_world->originOptimizePending = true;
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_origin_abi_version(void) { return AVER_PHYS_ORIGIN_ABI_VERSION; }

int32_t aver_phys_origin_prepare(double dxCm, double dyCm, double dzCm) {
    Delta d;
    const AbiError e = checkShift(dxCm, dyCm, dzCm, d);
    if (e != AbiError::Ok) { setAbiError(e); return -1; }
    setAbiError(AbiError::Ok);
    const usize n = g_world->bodies.size() + g_world->characters.size() + static_cast<usize>(originJointsCount());
    return n > 0x7fffffffu ? 0x7fffffff : static_cast<int32_t>(n);
}

int32_t aver_phys_origin_shift(double dxCm, double dyCm, double dzCm) {
    Delta d;
    const AbiError e = checkShift(dxCm, dyCm, dzCm, d);
    if (e != AbiError::Ok) { setAbiError(e); return 0; }
    AVER_PHYS_SCOPE("OriginShift");
    const auto t0 = std::chrono::steady_clock::now();
    struct Guard { Guard() { g_shifting = true; } ~Guard() { g_shifting = false; } } const guard;

    OriginStats st;
    originJointsBegin();

    // A character's inner body is moved by CharacterVirtual::SetPosition below; it is not in World::bodies, and this keeps it that way.
    std::vector<JPH::CharacterVirtual*> characters;
    std::unordered_set<JPH::BodyID> inner;
    for (const auto& entry : g_world->characters) {
        characters.push_back(entry.second.GetPtr());
        if (!entry.second->GetInnerBodyID().IsInvalid()) inner.insert(entry.second->GetInnerBodyID());
    }

    // Bodies: dynamic, kinematic, static, sensors, heightfields, chassis, ragdoll parts, soft bodies (COM-local vertices ride along).
    // DontActivate: a sleeper stays asleep; an unadded body is moved without a broad-phase notify.
    for (const auto& entry : g_world->bodies) {
        const JPH::BodyID& id = entry.second;
        if (!inner.empty() && inner.count(id)) continue;
        bool isStatic = false;
        if (!moveBody(id, d.dj, isStatic)) continue;
        ++st.bodies;
        if (isStatic) ++st.staticsMoved;
    }

    moveCharacters(characters, d.dj);
    st.characters = static_cast<int32_t>(characters.size());

    originJointsShift(d.dj, st);
    shiftPendingEvents(d.cm);
    for (const Hook& h : std::vector<Hook>(hooks())) {   // a copy: a hook that registers another must not invalidate the walk
        if (h.fn) h.fn(d.cm);
        else if (h.seam) h.seam(d.cm.x, d.cm.y, d.cm.z, h.user);
    }

    if (st.staticsMoved > kOptimizeAboveStatics) g_world->originOptimizePending = true;

    ++g_world->originGeneration;
    g_world->originTotalCm[0] += d.cm.x;
    g_world->originTotalCm[1] += d.cm.y;
    st.ms = static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    g_last = st;
    g_lastWorldId = g_world->id;
    AVER_INFO("[Physics] origin shift {} of ({:.0f}, {:.0f}) cm: {} bodies ({} static), {} characters, {} anchors, {} pulleys rebuilt, {:.2f} ms",
              g_world->originGeneration, d.cm.x, d.cm.y, st.bodies, st.staticsMoved, st.characters, st.anchors, st.pulleysRebuilt, st.ms);
    setAbiError(AbiError::Ok);
    return 1;
}

uint32_t aver_phys_origin_generation(void) { return g_world ? g_world->originGeneration : 0u; }

void aver_phys_origin_total(double* outXyzCm) {
    if (!outXyzCm) return;
    for (int i = 0; i < 3; ++i) outXyzCm[i] = g_world ? g_world->originTotalCm[i] : 0.0;
}

int32_t aver_phys_origin_stats(float* outMs, int32_t* outBodies, int32_t* outCharacters, int32_t* outAnchors,
                               int32_t* outPulleysRebuilt, int32_t* outStaticsMoved) {
    const OriginStats& s = lastOriginStats();
    if (outMs) *outMs = s.ms;
    if (outBodies) *outBodies = s.bodies;
    if (outCharacters) *outCharacters = s.characters;
    if (outAnchors) *outAnchors = s.anchors;
    if (outPulleysRebuilt) *outPulleysRebuilt = s.pulleysRebuilt;
    if (outStaticsMoved) *outStaticsMoved = s.staticsMoved;
    return g_world && g_world->originGeneration > 0 ? 1 : 0;
}

// A TEST SEAM, deliberately in no ABI header (tests/abi scans the headers, and a function pointer would need a C# mirror nobody wants):
// registers `fn` as a hook under `name` (replacing one of that name), or removes the hook when `fn` is null. PhysicsOriginTest uses it to
// see that hooks run once per shift, in registration order, after the bodies have moved. Returns the number of hooks registered.
AVER_PHYS_API int32_t aver_phys_origin_test_hook(const char* name, SeamHookFn fn, void* user) {
    if (!name) return static_cast<int32_t>(hooks().size());
    if (!fn) {
        auto& list = hooks();
        for (auto it = list.begin(); it != list.end(); ++it)
            if (it->name == name) { list.erase(it); break; }
        return static_cast<int32_t>(list.size());
    }
    Hook h;
    h.name = name;
    h.seam = fn;
    h.user = user;
    putHook(std::move(h));
    return static_cast<int32_t>(hooks().size());
}

} // extern "C"
