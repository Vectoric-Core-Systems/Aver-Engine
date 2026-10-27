// PhysicsOrigin.cpp -- the floating origin's physics half (physics_origin_abi.h).
// A shift moves bodies, characters and anchors by -toJolt(delta); a snapshot restore moves only what it restored.
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

// More statics moved than this and the next step opens with OptimizeBroadPhase (moving many static leaves loosens the tree).
constexpr int32_t kOptimizeAboveStatics = 4096;
// 1.6e9 cm = 1.6e7 m, under 2^24, so every whole metre of the Jolt-space delta is exact in f32.
constexpr double kMaxDeltaCm = 1.6e9;

// The test seam passes the delta as three doubles; module hooks take a Vec3d.
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

struct Delta {
    Vec3d     cm;   // engine axes, centimetres
    JPH::Vec3 dj;   // Jolt axes, metres; positions change by -dj
};

// toJolt(v) = (v.y, v.z, -v.x) / 100. "+ 0.0f" turns -0 into +0.
JPH::Vec3 joltDelta(double dxCm, double dyCm) {
    return JPH::Vec3(static_cast<float>(dyCm / 100.0), 0.0f, static_cast<float>(-dxCm / 100.0) + 0.0f);
}

bool wholeMetres(double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
    if (std::fabs(dx) > kMaxDeltaCm || std::fabs(dy) > kMaxDeltaCm) return false;
    return std::fmod(dx, 100.0) == 0.0 && std::fmod(dy, 100.0) == 0.0;
}

bool parseDelta(double dx, double dy, double dz, Delta& out) {
    if (!std::isfinite(dz) || dz != 0.0) return false;
    if (dx == 0.0 && dy == 0.0) return false;   // nothing to do is not a shift: no generation, no hooks
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

// Undrained event points are shifted in place rather than drained first.
void shiftPendingEvents(const Vec3d& d) {
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    for (ContactEvent& e : g_world->contacts) shiftPoint(e.point, d);
    for (VehicleImpactRaw& r : g_world->vehicleImpacts) shiftPoint(r.point, d);
    // Only kinds carrying a point in f[0..2]; SHIFT, FLIPPED and ASSIST write zero, meaning none.
    for (VehicleEventRec& r : g_world->vehicleEvents)
        if (r.kind >= AVER_PHYS_VEV_SKID_BEGIN && r.kind <= AVER_PHYS_VEV_IMPACT) shiftPoint(r.f, d);
}

// Moves one body by -dj without waking it. False when the id names no body.
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

// Not aver_phys_character_set_position, which erases characterCarry. All move before any refresh.
void moveCharacters(const std::vector<JPH::CharacterVirtual*>& characters, const JPH::Vec3& dj) {
    for (JPH::CharacterVirtual* c : characters) c->SetPosition(c->GetPosition() - JPH::RVec3(dj));
    for (JPH::CharacterVirtual* c : characters)
        c->RefreshContacts(g_world->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                           g_world->system.GetDefaultLayerFilter(Layers::MOVING),
                           characterBodyFilter(), {}, *g_world->temp);
}

void addCharacter(JPH::CharacterVirtual* c, std::vector<JPH::CharacterVirtual*>& characters, std::unordered_set<JPH::BodyID>& inner) {
    characters.push_back(c);
    if (!c->GetInnerBodyID().IsInvalid()) inner.insert(c->GetInnerBodyID());
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

    // SetPosition carries a character's inner body, which the manifest also lists, so the body loop skips it.
    std::vector<JPH::CharacterVirtual*> characters;
    std::unordered_set<JPH::BodyID> inner;
    for (const int32_t h : characterHandles) {
        if (JPH::CharacterVirtual* c = findCharacter(h)) addCharacter(c, characters, inner);
    }

    int32_t statics = 0;
    for (const uint32_t raw : bodyIds) {
        const JPH::BodyID id(raw);
        if (inner.count(id)) continue;
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

    // Inner bodies move with CharacterVirtual::SetPosition below; the body loop skips them so they never move twice.
    std::vector<JPH::CharacterVirtual*> characters;
    std::unordered_set<JPH::BodyID> inner;
    for (const auto& entry : g_world->characters) addCharacter(entry.second.GetPtr(), characters, inner);

    // Every body kind. DontActivate keeps sleepers asleep and skips the broad-phase notify for unadded bodies.
    for (const auto& entry : g_world->bodies) {
        const JPH::BodyID& id = entry.second;
        if (inner.count(id)) continue;
        bool isStatic = false;
        if (!moveBody(id, d.dj, isStatic)) continue;
        ++st.bodies;
        if (isStatic) ++st.staticsMoved;
    }

    moveCharacters(characters, d.dj);
    st.characters = static_cast<int32_t>(characters.size());

    originJointsShift(d.dj, st);
    shiftPendingEvents(d.cm);
    for (const Hook& h : std::vector<Hook>(hooks())) {   // iterate a copy: a hook may register another
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

// Test seam, in no ABI header on purpose. Registers fn under name (fn null removes it); returns the hook count.
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
