// PhysicsAreas.cpp -- per-body contact monitoring and Areas on top of Jolt. Rationale, the sleep rule and what snapshots do
// with all of it are in docs/physics/AREAS.md; the ABI is physics_areas_abi.h.
//
// CONTACT MONITOR. The contact listener (workers) appends raw Added/Removed sub-shape contacts that touch a monitored body to a locked
// list. At the end of each fixed step the game thread folds them, sorted, into a pair table (one entry per body pair, a sub-shape count),
// and decides BEGIN/END per pair. A pair whose count falls to zero does not end while either body sleeps (Jolt reports Removed for every
// contact of a body that falls asleep); after a wake it gets a short grace for the contact to come back, and WereBodiesInContact has the
// last word. AREAS. An area is a sensor body plus a record. Its overlaps are the area's own CollideShape query, so static and sleeping
// bodies are seen; overrides are re-queried at the start of each step from present positions, so they carry no state across a restore.
#include "PhysicsAreas.hpp"

#include "PhysicsLayerFilter.hpp"

#include "aver/physics/physics_areas_abi.h"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace aver::physics::detail {

namespace {

constexpr int kWakeGrace = 3;              // steps after a wake in which a lost touch may come back
constexpr int32_t kMaxMonitorContacts = 1024;

const JPH::BodyID* idOf(int32_t handle) {
    const auto it = g_world->bodies.find(handle);
    return it == g_world->bodies.end() ? nullptr : &it->second;
}

uint64_t keyOf(int32_t lo, int32_t hi) { return (static_cast<uint64_t>(static_cast<uint32_t>(lo)) << 32) | static_cast<uint32_t>(hi); }
int32_t loOf(uint64_t k) { return static_cast<int32_t>(k >> 32); }
int32_t hiOf(uint64_t k) { return static_cast<int32_t>(k & 0xFFFFFFFFu); }

// ---- contact monitor -------------------------------------------------------------------------------

struct ContactInfo { float point[3] = {0, 0, 0}, normal[3] = {0, 0, 0}; };   // normal: from the owning body to its partner

struct MonEvent { int32_t body = 0, other = 0, began = 0; float point[3] = {0, 0, 0}, normal[3] = {0, 0, 0}; };

struct RawContact {
    uint8_t kind = 0;                  // 0 Added, 1 Removed
    int32_t lo = 0, hi = 0;
    uint32_t subLo = 0, subHi = 0;
    float point[3] = {0, 0, 0}, normal[3] = {0, 0, 0};   // normal: lo to hi
};

struct PairState {
    int32_t subCount = 0;              // live sub-shape contacts as the listener has told us
    bool asleep = false;               // lost while a body slept
    int grace = 0;                     // steps left, once nobody sleeps, for the contact to come back
    float point[3] = {0, 0, 0}, normal[3] = {0, 0, 0};   // first touch; normal lo to hi
};

struct Monitor {
    int32_t maxContacts = 0;
    std::map<int32_t, ContactInfo> reported;   // partner handle -> info, at most maxContacts
    bool sawOverflow = false;                  // a pair existed with no room; promoted when a slot frees
};

}  // namespace

struct ContactMonitorState {
    std::mutex mu;                             // guards raw (workers append)
    std::vector<RawContact> raw;
    std::unordered_set<int32_t> monitored;     // read by workers; written between steps
    std::map<int32_t, Monitor> monitors;
    std::map<uint64_t, PairState> pairs;       // ordered: every walk is deterministic
    std::vector<MonEvent> events;              // this call's, polled
    std::vector<MonEvent> pending;             // queued between steps (removals, seeds); join events at the next call
};

namespace {

// ---- areas -----------------------------------------------------------------------------------------

struct AreaEvent { int32_t area = 0, other = 0, entered = 0, isArea = 0; };

struct Area {
    bool monitoring = true, monitorable = true;
    bool layerSet = false;
    uint32_t layerBits = 0, maskBits = 0xFFFFFFFFu;
    int32_t priority = 0;
    int32_t gravMode = 0;
    float gravMag = 0.0f;                      // m/s^2
    JPH::Float3 gravDir{0, 0, 0};              // Jolt axes, unit
    bool point = false;
    JPH::Float3 pointLocal{0, 0, 0};           // metres, area local, Jolt axes
    float pointUnit = 0.0f;                    // metres
    int32_t linMode = 0, angMode = 0;
    float lin = 0.0f, ang = 0.0f;
    float windForce = 0.0f;                    // N
    JPH::Float3 windDir{0, 0, 0};
    float windAtten = 0.0f, windUnit = 0.0f;   // metres
    std::map<int32_t, bool> reported;          // overlapping handle -> is an area

    bool hasOverrides() const { return gravMode != 0 || linMode != 0 || angMode != 0 || windForce != 0.0f; }
};

struct AreaPose { JPH::Float3 pos{0, 0, 0}; JPH::Quat rot = JPH::Quat::sIdentity(); };

struct Signature {
    float v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool operator==(const Signature& o) const {
        for (int i = 0; i < 8; ++i) if (std::fabs(v[i] - o.v[i]) > 1e-4f) return false;
        return true;
    }
};

}  // namespace

struct AreaState {
    std::map<int32_t, Area> areas;             // by sensor body handle
    std::vector<AreaEvent> events, pending;
    struct Damp { float baseLin, baseAng, setLin, setAng; };
    std::map<int32_t, Damp> damped;            // bodies whose Jolt damping we rewrote
    std::map<int32_t, Signature> sig;          // last effect on each body, to wake sleepers only when it changes
};

namespace {

ContactMonitorState& monitorsOrCreate() {
    if (!g_world->monitors) g_world->monitors = std::make_shared<ContactMonitorState>();
    return *g_world->monitors;
}
AreaState& areasOrCreate() {
    if (!g_world->areas) g_world->areas = std::make_shared<AreaState>();
    return *g_world->areas;
}

bool isSleeping(const JPH::BodyID& id) {
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
    return lock.Succeeded() && !lock.GetBody().IsStatic() && !lock.GetBody().IsActive();
}

ContactInfo infoFor(const PairState& ps, int32_t self, int32_t lo) {
    ContactInfo i;
    const float s = self == lo ? 1.0f : -1.0f;
    for (int k = 0; k < 3; ++k) { i.point[k] = ps.point[k]; i.normal[k] = ps.normal[k] * s; }
    return i;
}

void pushEvent(std::vector<MonEvent>& out, int32_t body, int32_t other, int32_t began, const ContactInfo& i) {
    MonEvent e;
    e.body = body; e.other = other; e.began = began;
    for (int k = 0; k < 3; ++k) { e.point[k] = i.point[k]; e.normal[k] = i.normal[k]; }
    out.push_back(e);
}

// A monitored side takes a free slot and reports BEGIN, or notes that it overflowed.
void beginSide(ContactMonitorState& st, int32_t self, int32_t other, int32_t lo, const PairState& ps, std::vector<MonEvent>& out) {
    const auto mi = st.monitors.find(self);
    if (mi == st.monitors.end()) return;
    Monitor& m = mi->second;
    if (static_cast<int32_t>(m.reported.size()) >= m.maxContacts) { m.sawOverflow = true; return; }
    const ContactInfo info = infoFor(ps, self, lo);
    m.reported[other] = info;
    pushEvent(out, self, other, 1, info);
}

void beginPair(ContactMonitorState& st, int32_t lo, int32_t hi, const PairState& ps, std::vector<MonEvent>& out) {
    beginSide(st, lo, hi, lo, ps, out);
    beginSide(st, hi, lo, lo, ps, out);
}

// A slot freed on `self`: the first untracked live pair (ascending key) takes it.
void promote(ContactMonitorState& st, int32_t self, uint64_t skipKey, std::vector<MonEvent>& out) {
    const auto mi = st.monitors.find(self);
    if (mi == st.monitors.end() || !mi->second.sawOverflow) return;
    Monitor& m = mi->second;
    bool leftover = false;
    for (const auto& [key, ps] : st.pairs) {
        if (key == skipKey) continue;
        const int32_t lo = loOf(key), hi = hiOf(key);
        if (lo != self && hi != self) continue;
        const int32_t other = lo == self ? hi : lo;
        if (m.reported.count(other)) continue;
        if (static_cast<int32_t>(m.reported.size()) >= m.maxContacts) { leftover = true; break; }
        const ContactInfo info = infoFor(ps, self, lo);
        m.reported[other] = info;
        pushEvent(out, self, other, 1, info);
    }
    m.sawOverflow = leftover;
}

void endSide(ContactMonitorState& st, int32_t self, int32_t other, uint64_t key, std::vector<MonEvent>& out) {
    const auto mi = st.monitors.find(self);
    if (mi == st.monitors.end()) return;
    const auto ri = mi->second.reported.find(other);
    if (ri == mi->second.reported.end()) return;
    pushEvent(out, self, other, 0, ri->second);
    mi->second.reported.erase(ri);
    promote(st, self, key, out);
}

void endPair(ContactMonitorState& st, uint64_t key, std::vector<MonEvent>& out) {
    const int32_t lo = loOf(key), hi = hiOf(key);
    endSide(st, lo, hi, key, out);
    endSide(st, hi, lo, key, out);
}

// Tests the other bodies a monitored body overlaps within Jolt's contact distance and enters them as touches. `emit` false: silent (a restore).
struct SeedHit { float point[3], normal[3]; };

class SeedCollector final : public JPH::CollideShapeCollector {
public:
    void AddHit(const JPH::CollideShapeResult& r) override {
        const auto it = g_world->byId.find(r.mBodyID2);
        if (it == g_world->byId.end() || hits.count(it->second)) return;
        SeedHit s;
        const Vec3 p = fromJolt(r.mContactPointOn2);
        const Vec3 n = fromJoltUnit(r.mPenetrationAxis.Normalized());
        s.point[0] = p.x; s.point[1] = p.y; s.point[2] = p.z;
        s.normal[0] = n.x; s.normal[1] = n.y; s.normal[2] = n.z;
        hits.emplace(it->second, s);
    }
    std::map<int32_t, SeedHit> hits;   // other handle -> first hit
};

class SeedFilter final : public JPH::BodyFilter {
public:
    SeedFilter(const JPH::BodyID& self, JPH::ObjectLayer layer, const JPH::CollisionGroup& group) : self_(self), layer_(layer), group_(group) {}
    bool ShouldCollide(const JPH::BodyID& other) const override { return other != self_; }
    bool ShouldCollideLocked(const JPH::Body& b) const override {
        if (b.IsSensor() || b.IsSoftBody() || g_world->byId.find(b.GetID()) == g_world->byId.end()) return false;
        return g_world->objPair.ShouldCollide(layer_, b.GetObjectLayer()) && group_.CanCollide(b.GetCollisionGroup());
    }
private:
    JPH::BodyID self_;
    JPH::ObjectLayer layer_;
    JPH::CollisionGroup group_;
};

void seedMonitor(ContactMonitorState& st, int32_t h, std::vector<MonEvent>& out) {
    const JPH::BodyID* id = idOf(h);
    if (!id) return;
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 xf;
    JPH::ObjectLayer layer = 0;
    JPH::CollisionGroup group;
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded() || lock.GetBody().IsSensor()) return;
        const JPH::Body& b = lock.GetBody();
        shape = b.GetShape();
        xf = b.GetCenterOfMassTransform();
        layer = b.GetObjectLayer();
        group = b.GetCollisionGroup();
    }
    SeedCollector coll;
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = 0.02f;   // Jolt's speculative contact distance
    const SeedFilter filter(*id, layer, group);
    g_world->system.GetNarrowPhaseQuery().CollideShape(shape.GetPtr(), JPH::Vec3::sReplicate(1.0f), xf, settings, JPH::RVec3::sZero(), coll,
                                                       {}, {}, filter);
    for (const auto& [other, hit] : coll.hits) {
        const int32_t lo = std::min(h, other), hi = std::max(h, other);
        const uint64_t key = keyOf(lo, hi);
        if (st.pairs.count(key)) continue;
        PairState ps;
        const float s = h == lo ? 1.0f : -1.0f;   // hit normal is h to other; the pair stores lo to hi
        for (int k = 0; k < 3; ++k) { ps.point[k] = hit.point[k]; ps.normal[k] = hit.normal[k] * s; }
        st.pairs.emplace(key, ps);
        beginPair(st, lo, hi, ps, out);
    }
}

void monitorsStepEnd(ContactMonitorState& st) {
    std::vector<RawContact> raw;
    { std::lock_guard<std::mutex> lk(st.mu); raw.swap(st.raw); }
    if (raw.empty() && st.pairs.empty()) return;
    std::sort(raw.begin(), raw.end(), [](const RawContact& a, const RawContact& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.lo != b.lo) return a.lo < b.lo;
        if (a.hi != b.hi) return a.hi < b.hi;
        if (a.subLo != b.subLo) return a.subLo < b.subLo;
        return a.subHi < b.subHi;
    });
    for (const RawContact& r : raw) {
        if (!idOf(r.lo) || !idOf(r.hi)) continue;
        const uint64_t key = keyOf(r.lo, r.hi);
        if (r.kind == 0) {
            const auto [it, fresh] = st.pairs.try_emplace(key);
            PairState& ps = it->second;
            ++ps.subCount;
            if (fresh) {
                for (int k = 0; k < 3; ++k) { ps.point[k] = r.point[k]; ps.normal[k] = r.normal[k]; }
                beginPair(st, r.lo, r.hi, ps, st.events);
            } else if (ps.asleep) {
                ps.asleep = false;
                ps.grace = 0;
            }
        } else {
            const auto it = st.pairs.find(key);
            if (it != st.pairs.end() && it->second.subCount > 0) --it->second.subCount;
        }
    }
    // Pairs with no live sub-shape contact: asleep, still touching, or over.
    for (auto it = st.pairs.begin(); it != st.pairs.end();) {
        PairState& ps = it->second;
        if (ps.subCount > 0) { ++it; continue; }
        const int32_t lo = loOf(it->first), hi = hiOf(it->first);
        const JPH::BodyID *ia = idOf(lo), *ib = idOf(hi);
        if (!ia || !ib) { endPair(st, it->first, st.events); it = st.pairs.erase(it); continue; }
        if (isSleeping(*ia) || isSleeping(*ib)) { ps.asleep = true; ps.grace = kWakeGrace; ++it; continue; }
        if (g_world->system.WereBodiesInContact(*ia, *ib)) { ps.subCount = 1; ps.asleep = false; ps.grace = 0; ++it; continue; }
        if (ps.asleep && --ps.grace > 0) { ++it; continue; }
        endPair(st, it->first, st.events);
        it = st.pairs.erase(it);
    }
}

// ---- area queries ----------------------------------------------------------------------------------

uint32_t areaLayerBits(const Area& a, const JPH::Body& body) {
    return a.layerSet ? a.layerBits : (1u << Layers::userOf(body.GetObjectLayer()));
}

class AreaTargetFilter final : public JPH::BodyFilter {
public:
    AreaTargetFilter(const JPH::BodyID& self, uint32_t mask, const AreaState& st) : self_(self), mask_(mask), st_(st) {}
    bool ShouldCollide(const JPH::BodyID& other) const override { return other != self_; }
    bool ShouldCollideLocked(const JPH::Body& b) const override {
        if (b.IsSoftBody()) return false;
        const auto hi = g_world->byId.find(b.GetID());
        if (hi == g_world->byId.end()) return false;
        if (b.IsSensor()) {
            const auto ai = st_.areas.find(hi->second);
            return ai != st_.areas.end() && ai->second.monitorable && (mask_ & areaLayerBits(ai->second, b)) != 0u;
        }
        return (mask_ & layerBitsOfBody(b)) != 0u;
    }
private:
    JPH::BodyID self_;
    uint32_t mask_;
    const AreaState& st_;
};

class UniqueBodies final : public JPH::CollideShapeCollector {
public:
    void AddHit(const JPH::CollideShapeResult& r) override {
        const auto it = g_world->byId.find(r.mBodyID2);
        if (it != g_world->byId.end()) handles.insert(it->second);
    }
    std::set<int32_t> handles;
};

// Everything `area` overlaps now (handle -> is an area), and where the area is. False if the sensor is gone.
bool queryArea(const AreaState& st, int32_t handle, const Area& a, std::map<int32_t, bool>& out, AreaPose* pose) {
    const JPH::BodyID* id = idOf(handle);
    if (!id) return false;
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 xf;
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return false;
        const JPH::Body& b = lock.GetBody();
        shape = b.GetShape();
        xf = b.GetCenterOfMassTransform();
        if (pose) {
            const JPH::Vec3 p = JPH::Vec3(b.GetPosition());
            pose->pos = JPH::Float3(p.GetX(), p.GetY(), p.GetZ());
            pose->rot = b.GetRotation();
        }
    }
    UniqueBodies coll;
    const AreaTargetFilter filter(*id, a.maskBits, st);
    g_world->system.GetNarrowPhaseQuery().CollideShape(shape.GetPtr(), JPH::Vec3::sReplicate(1.0f), xf, JPH::CollideShapeSettings{},
                                                       JPH::RVec3::sZero(), coll, {}, {}, filter);
    for (const int32_t h : coll.handles) {
        const JPH::BodyID* oid = idOf(h);
        bool isArea = false;
        if (oid) {
            JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *oid);
            isArea = lock.Succeeded() && lock.GetBody().IsSensor();
        }
        out[h] = isArea;
    }
    return true;
}

// ---- overrides -------------------------------------------------------------------------------------

struct InArea { int32_t handle; const Area* area; const AreaPose* pose; };

JPH::Vec3 gravityOf(const Area& a, const AreaPose& pose, const JPH::Vec3& com) {
    if (a.point) {
        const JPH::Vec3 centre = JPH::Vec3(pose.pos) + pose.rot * JPH::Vec3(a.pointLocal);
        const JPH::Vec3 v = centre - com;
        const float len = v.Length();
        if (len <= 1e-6f) return JPH::Vec3::sZero();
        float strength = a.gravMag;
        if (a.pointUnit > 0.0f) {
            const float r = a.pointUnit / std::max(len, 0.001f);
            strength *= r * r;
        }
        return v * (strength / len);
    }
    return JPH::Vec3(a.gravDir) * a.gravMag;
}

struct Resolved {
    JPH::Vec3 gravity = JPH::Vec3::sZero();   // the total, world included
    JPH::Vec3 wind = JPH::Vec3::sZero();      // N
    float lin = 0.0f, ang = 0.0f;             // totals, the body's own damping included
    bool damps = false;                       // some area has a damping mode in play
    bool affects = false;                     // some area changes gravity, damping or wind for it
};

// The areas are sorted here: highest priority first, ties by lowest handle.
Resolved resolve(std::vector<InArea>& list, const JPH::Vec3& com, float baseLin, float baseAng) {
    Resolved r;
    std::stable_sort(list.begin(), list.end(), [](const InArea& a, const InArea& b) {
        if (a.area->priority != b.area->priority) return a.area->priority > b.area->priority;
        return a.handle < b.handle;
    });
    JPH::Vec3 g = JPH::Vec3::sZero();
    bool gDone = false, lDone = false, aDone = false;
    float lin = 0.0f, ang = 0.0f;
    for (const InArea& e : list) {
        const Area& a = *e.area;
        if (!gDone && a.gravMode != 0) {
            const JPH::Vec3 ag = gravityOf(a, *e.pose, com);
            r.affects = true;
            switch (a.gravMode) {
                case AVER_PHYS_AREA_OVERRIDE_COMBINE:         g += ag; break;
                case AVER_PHYS_AREA_OVERRIDE_COMBINE_REPLACE: g += ag; gDone = true; break;
                case AVER_PHYS_AREA_OVERRIDE_REPLACE:         g = ag; gDone = true; break;
                case AVER_PHYS_AREA_OVERRIDE_REPLACE_COMBINE: g = ag; break;
                default: break;
            }
        }
        if (!lDone && a.linMode != 0) {
            r.damps = r.affects = true;
            switch (a.linMode) {
                case AVER_PHYS_AREA_OVERRIDE_COMBINE:         lin += a.lin; break;
                case AVER_PHYS_AREA_OVERRIDE_COMBINE_REPLACE: lin += a.lin; lDone = true; break;
                case AVER_PHYS_AREA_OVERRIDE_REPLACE:         lin = a.lin; lDone = true; break;
                case AVER_PHYS_AREA_OVERRIDE_REPLACE_COMBINE: lin = a.lin; break;
                default: break;
            }
        }
        if (!aDone && a.angMode != 0) {
            r.damps = r.affects = true;
            switch (a.angMode) {
                case AVER_PHYS_AREA_OVERRIDE_COMBINE:         ang += a.ang; break;
                case AVER_PHYS_AREA_OVERRIDE_COMBINE_REPLACE: ang += a.ang; aDone = true; break;
                case AVER_PHYS_AREA_OVERRIDE_REPLACE:         ang = a.ang; aDone = true; break;
                case AVER_PHYS_AREA_OVERRIDE_REPLACE_COMBINE: ang = a.ang; break;
                default: break;
            }
        }
        if (a.windForce != 0.0f) {
            float f = a.windForce;
            if (a.windAtten > 0.0f && a.windUnit > 0.0f) {
                const float d = (com - JPH::Vec3(e.pose->pos)).Length();
                if (d > a.windUnit) f *= std::pow(a.windUnit / d, a.windAtten);
            }
            r.wind += JPH::Vec3(a.windDir) * f;
            r.affects = true;
        }
    }
    r.gravity = gDone ? g : g + g_world->system.GetGravity();
    r.lin = lDone ? lin : lin + baseLin;
    r.ang = aDone ? ang : ang + baseAng;
    return r;
}

// Every override area's overlaps (non-area bodies only), with the area poses kept alive in `poses`.
void collectOverrides(const AreaState& st, std::map<int32_t, AreaPose>& poses, std::map<int32_t, std::vector<InArea>>& perBody) {
    for (const auto& [h, a] : st.areas) {
        if (!a.hasOverrides()) continue;
        std::map<int32_t, bool> inside;
        AreaPose pose;
        if (!queryArea(st, h, a, inside, &pose)) continue;
        const AreaPose& kept = poses.emplace(h, pose).first->second;
        for (const auto& [b, isArea] : inside)
            if (!isArea) perBody[b].push_back(InArea{h, &a, &kept});
    }
}

// What a dynamic body is, for the step: false for anything else.
struct BodyNow { bool dynamic = false, active = false; float invMass = 0.0f, gravityFactor = 1.0f, lin = 0.0f, ang = 0.0f; JPH::Vec3 com = JPH::Vec3::sZero(); };

BodyNow readBody(const JPH::BodyID& id) {
    BodyNow n;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return n;
    const JPH::Body& b = lock.GetBody();
    n.com = JPH::Vec3(b.GetCenterOfMassPosition());
    if (!b.IsDynamic() || b.IsSensor()) return n;
    const JPH::MotionProperties* mp = b.GetMotionProperties();
    n.dynamic = mp != nullptr;
    if (!n.dynamic) return n;
    n.active = b.IsActive();
    n.invMass = mp->GetInverseMass();
    n.gravityFactor = mp->GetGravityFactor();
    n.lin = mp->GetLinearDamping();
    n.ang = mp->GetAngularDamping();
    return n;
}

void setDamping(const JPH::BodyID& id, float lin, float ang) {
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), id);
    if (!lock.Succeeded() || !lock.GetBody().GetMotionPropertiesUnchecked()) return;
    lock.GetBody().GetMotionProperties()->SetLinearDamping(lin);
    lock.GetBody().GetMotionProperties()->SetAngularDamping(ang);
}

void areaEventsPush(std::vector<AreaEvent>& out, int32_t area, int32_t other, int32_t entered, bool isArea) {
    out.push_back(AreaEvent{area, other, entered, isArea ? 1 : 0});
}

}  // namespace

// ---- hooks -----------------------------------------------------------------------------------------

void monitorOnAdded(int32_t ha, int32_t hb, const JPH::ContactManifold& m) {
    ContactMonitorState* st = g_world->monitors.get();
    if (!st || (!st->monitored.count(ha) && !st->monitored.count(hb))) return;
    const bool swap = ha > hb;
    RawContact r;
    r.kind = 0;
    r.lo = swap ? hb : ha;
    r.hi = swap ? ha : hb;
    r.subLo = swap ? m.mSubShapeID2.GetValue() : m.mSubShapeID1.GetValue();
    r.subHi = swap ? m.mSubShapeID1.GetValue() : m.mSubShapeID2.GetValue();
    const Vec3 p = fromJolt(m.GetWorldSpaceContactPointOn1(0));
    Vec3 n = fromJoltUnit(m.mWorldSpaceNormal);
    if (swap) n = Vec3(-n.x, -n.y, -n.z);
    r.point[0] = p.x; r.point[1] = p.y; r.point[2] = p.z;
    r.normal[0] = n.x; r.normal[1] = n.y; r.normal[2] = n.z;
    std::lock_guard<std::mutex> lk(st->mu);
    st->raw.push_back(r);
}

void monitorOnRemoved(int32_t ha, int32_t hb) {
    ContactMonitorState* st = g_world->monitors.get();
    if (!st || (!st->monitored.count(ha) && !st->monitored.count(hb))) return;
    if (g_world->sensors.count(ha) || g_world->sensors.count(hb)) return;   // an overlap, not a touch
    const bool swap = ha > hb;
    RawContact r;
    r.kind = 1;
    r.lo = swap ? hb : ha;
    r.hi = swap ? ha : hb;
    std::lock_guard<std::mutex> lk(st->mu);
    st->raw.push_back(r);
}

void areasBeginCall() {
    if (ContactMonitorState* st = g_world->monitors.get()) {
        st->events.clear();
        st->events.insert(st->events.end(), st->pending.begin(), st->pending.end());
        st->pending.clear();
    }
    if (AreaState* st = g_world->areas.get()) {
        st->events.clear();
        st->events.insert(st->events.end(), st->pending.begin(), st->pending.end());
        st->pending.clear();
    }
}

void areasStepBegin(float) {
    AreaState* st = g_world->areas.get();
    if (!st) return;
    std::map<int32_t, AreaPose> poses;
    std::map<int32_t, std::vector<InArea>> perBody;
    collectOverrides(*st, poses, perBody);
    if (perBody.empty() && st->damped.empty() && st->sig.empty()) return;

    std::set<int32_t> stillDamped;
    const JPH::Vec3 world = g_world->system.GetGravity();
    for (auto& [bh, list] : perBody) {
        const JPH::BodyID* id = idOf(bh);
        if (!id) continue;
        BodyNow n = readBody(*id);
        if (!n.dynamic || n.invMass <= 0.0f) continue;
        // The body's own damping: ours from last step is not the user's.
        float baseLin = n.lin, baseAng = n.ang;
        const auto di = st->damped.find(bh);
        if (di != st->damped.end()) {
            if (n.lin == di->second.setLin) baseLin = di->second.baseLin;
            if (n.ang == di->second.setAng) baseAng = di->second.baseAng;
        }
        const Resolved r = resolve(list, n.com, baseLin, baseAng);
        if (!r.affects) continue;

        const JPH::Vec3 extra = (r.gravity - world) * n.gravityFactor;
        Signature s;
        s.v[0] = extra.GetX(); s.v[1] = extra.GetY(); s.v[2] = extra.GetZ();
        s.v[3] = r.wind.GetX(); s.v[4] = r.wind.GetY(); s.v[5] = r.wind.GetZ();
        s.v[6] = r.damps ? r.lin : 0.0f; s.v[7] = r.damps ? r.ang : 0.0f;
        const auto si = st->sig.find(bh);
        const bool changed = si == st->sig.end() || !(si->second == s);
        if (!n.active) {
            if (!changed) {   // asleep and nothing new: it stays asleep (and keeps its rewritten damping)
                if (r.damps) stillDamped.insert(bh);
                continue;
            }
            g_world->system.GetBodyInterface().ActivateBody(*id);
            n.active = true;
        }
        st->sig[bh] = s;

        const float mass = 1.0f / n.invMass;
        const JPH::Vec3 force = extra * mass + r.wind;
        if (force.LengthSq() > 0.0f) bi().AddForce(*id, force, JPH::EActivation::DontActivate);
        if (r.damps) {
            setDamping(*id, r.lin, r.ang);
            st->damped[bh] = AreaState::Damp{baseLin, baseAng, r.lin, r.ang};
            stillDamped.insert(bh);
        }
    }
    // Bodies that left every area (or whose area went) get their own damping back, if the user did not change it meanwhile.
    for (auto it = st->damped.begin(); it != st->damped.end();) {
        if (stillDamped.count(it->first)) { ++it; continue; }
        if (const JPH::BodyID* id = idOf(it->first)) {
            const BodyNow n = readBody(*id);
            if (n.dynamic) setDamping(*id, n.lin == it->second.setLin ? it->second.baseLin : n.lin, n.ang == it->second.setAng ? it->second.baseAng : n.ang);
        }
        it = st->damped.erase(it);
    }
    for (auto it = st->sig.begin(); it != st->sig.end();) it = perBody.count(it->first) ? std::next(it) : st->sig.erase(it);
}

void areasStepEnd() {
    if (ContactMonitorState* ms = g_world->monitors.get()) monitorsStepEnd(*ms);
    AreaState* st = g_world->areas.get();
    if (!st) return;
    for (auto& [h, a] : st->areas) {
        if (!a.monitoring) {
            for (const auto& [o, isArea] : a.reported) areaEventsPush(st->events, h, o, 0, isArea);
            a.reported.clear();
            continue;
        }
        std::map<int32_t, bool> now;
        if (!queryArea(*st, h, a, now, nullptr)) continue;
        for (const auto& [o, isArea] : a.reported)
            if (!now.count(o)) areaEventsPush(st->events, h, o, 0, isArea);
        for (const auto& [o, isArea] : now)
            if (!a.reported.count(o)) areaEventsPush(st->events, h, o, 1, isArea);
        a.reported = std::move(now);
    }
}

void areasOnBodyRemoved(int32_t handle) {
    if (ContactMonitorState* st = g_world->monitors.get()) {
        std::vector<uint64_t> keys;
        for (const auto& [key, ps] : st->pairs) if (loOf(key) == handle || hiOf(key) == handle) keys.push_back(key);
        for (const uint64_t key : keys) {
            const int32_t other = loOf(key) == handle ? hiOf(key) : loOf(key);
            const auto mi = st->monitors.find(other);
            if (mi != st->monitors.end()) {
                const auto ri = mi->second.reported.find(handle);
                if (ri != mi->second.reported.end()) {
                    pushEvent(st->pending, other, handle, 0, ri->second);
                    mi->second.reported.erase(ri);
                }
            }
            st->pairs.erase(key);
            promote(*st, other, 0, st->pending);
        }
        st->monitors.erase(handle);
        st->monitored.erase(handle);
    }
    if (AreaState* st = g_world->areas.get()) {
        const auto ai = st->areas.find(handle);
        if (ai != st->areas.end()) {
            for (const auto& [o, isArea] : ai->second.reported) areaEventsPush(st->pending, handle, o, 0, isArea);
            st->areas.erase(ai);
        }
    }
}

void areasOnRestore() {
    if (ContactMonitorState* st = g_world->monitors.get()) {
        { std::lock_guard<std::mutex> lk(st->mu); st->raw.clear(); }
        st->events.clear();
        st->pending.clear();
        st->pairs.clear();
        for (auto& [h, m] : st->monitors) { m.reported.clear(); m.sawOverflow = false; }
        std::vector<MonEvent> silent;
        for (const auto& [h, m] : st->monitors) seedMonitor(*st, h, silent);
    }
    if (AreaState* st = g_world->areas.get()) { st->events.clear(); st->pending.clear(); }
}

}  // namespace aver::physics::detail

// ---- the ABI ----------------------------------------------------------------------------------------

namespace {

int32_t refuse(AbiError e) { setAbiError(e); return 0; }

// The area record behind a handle, or null (the error is set).
Area* areaOf(int32_t handle) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return nullptr; }
    if (!g_world->areas) { setAbiError(AbiError::BadHandle); return nullptr; }
    const auto it = g_world->areas->areas.find(handle);
    if (it == g_world->areas->areas.end()) { setAbiError(AbiError::BadHandle); return nullptr; }
    return &it->second;
}

bool finite3(float a, float b, float c) { return std::isfinite(a) && std::isfinite(b) && std::isfinite(c); }

bool validMode(int32_t m) { return m >= AVER_PHYS_AREA_OVERRIDE_DISABLED && m <= AVER_PHYS_AREA_OVERRIDE_REPLACE_COMBINE; }

JPH::Float3 unitJolt(float x, float y, float z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len <= 1e-9f) return JPH::Float3(0, 0, 0);
    const JPH::Vec3 u = toJoltUnit(Vec3(x / len, y / len, z / len));
    return JPH::Float3(u.GetX(), u.GetY(), u.GetZ());
}

}  // namespace

extern "C" {

int32_t aver_phys_areas_abi_version(void) { return AVER_PHYS_AREAS_ABI_VERSION; }

// ---- contact monitor ----

int32_t aver_phys_body_set_contact_monitor(int32_t body, int32_t on, int32_t maxContacts) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (on && maxContacts < 0) return refuse(AbiError::InvalidArgument);
    if (!on) {
        ContactMonitorState* st = g_world->monitors.get();
        if (!st || !st->monitors.erase(body)) return 1;
        st->monitored.erase(body);
        for (auto it = st->pairs.begin(); it != st->pairs.end();)
            it = (st->monitored.count(loOf(it->first)) || st->monitored.count(hiOf(it->first))) ? std::next(it) : st->pairs.erase(it);
        return 1;
    }
    ContactMonitorState& st = monitorsOrCreate();
    maxContacts = std::min(maxContacts, kMaxMonitorContacts);
    const auto mi = st.monitors.find(body);
    if (mi != st.monitors.end()) {
        Monitor& m = mi->second;
        m.maxContacts = maxContacts;
        while (static_cast<int32_t>(m.reported.size()) > maxContacts) { m.reported.erase(std::prev(m.reported.end())); m.sawOverflow = true; }
        return 1;
    }
    st.monitors[body].maxContacts = maxContacts;
    st.monitored.insert(body);
    seedMonitor(st, body, st.pending);   // what it already touches begins now
    return 1;
}

int32_t aver_phys_body_contact_monitor(int32_t body, int32_t* outMaxContacts) {
    if (!findBody(body)) return 0;
    const ContactMonitorState* st = g_world->monitors.get();
    if (!st) return 0;
    const auto it = st->monitors.find(body);
    if (it == st->monitors.end()) return 0;
    if (outMaxContacts) *outMaxContacts = it->second.maxContacts;
    return 1;
}

int32_t aver_phys_body_contact_count(int32_t body) {
    if (!g_world || !g_world->monitors) return 0;
    const auto it = g_world->monitors->monitors.find(body);
    return it == g_world->monitors->monitors.end() ? 0 : static_cast<int32_t>(it->second.reported.size());
}

int32_t aver_phys_body_contact_get(int32_t body, int32_t index, int32_t* outOther, float* outPoint, float* outNormal) {
    if (!g_world || !g_world->monitors) return refuse(AbiError::BadHandle);
    const auto it = g_world->monitors->monitors.find(body);
    if (it == g_world->monitors->monitors.end()) return refuse(AbiError::BadHandle);
    if (index < 0 || index >= static_cast<int32_t>(it->second.reported.size())) return refuse(AbiError::OutOfRange);
    const auto ri = std::next(it->second.reported.begin(), index);
    if (outOther) *outOther = ri->first;
    for (int k = 0; k < 3; ++k) {
        if (outPoint) outPoint[k] = ri->second.point[k];
        if (outNormal) outNormal[k] = ri->second.normal[k];
    }
    return 1;
}

int32_t aver_phys_contact_event_count(void) {
    return g_world && g_world->monitors ? static_cast<int32_t>(g_world->monitors->events.size()) : 0;
}

int32_t aver_phys_contact_event_get(int32_t index, int32_t* outBody, int32_t* outOther, int32_t* outBegan, float* outPoint, float* outNormal) {
    if (!g_world || !g_world->monitors) return 0;
    const auto& ev = g_world->monitors->events;
    if (index < 0 || index >= static_cast<int32_t>(ev.size())) return refuse(AbiError::OutOfRange);
    const MonEvent& e = ev[static_cast<size_t>(index)];
    if (outBody) *outBody = e.body;
    if (outOther) *outOther = e.other;
    if (outBegan) *outBegan = e.began;
    for (int k = 0; k < 3; ++k) {
        if (outPoint) outPoint[k] = e.point[k];
        if (outNormal) outNormal[k] = e.normal[k];
    }
    return 1;
}

// ---- areas ----

int32_t aver_phys_area_create(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!bi().IsSensor(*id)) return refuse(AbiError::InvalidArgument);
    AreaState& st = areasOrCreate();
    st.areas.try_emplace(body);
    return 1;
}

int32_t aver_phys_area_destroy(int32_t area) {
    Area* a = areaOf(area);
    if (!a) return 0;
    for (const auto& [o, isArea] : a->reported) areaEventsPush(g_world->areas->pending, area, o, 0, isArea);
    g_world->areas->areas.erase(area);
    return 1;
}

int32_t aver_phys_area_exists(int32_t area) {
    return g_world && g_world->areas && g_world->areas->areas.count(area) ? 1 : 0;
}

int32_t aver_phys_area_count(void) {
    return g_world && g_world->areas ? static_cast<int32_t>(g_world->areas->areas.size()) : 0;
}

int32_t aver_phys_area_set_flags(int32_t area, int32_t monitoring, int32_t monitorable) {
    Area* a = areaOf(area);
    if (!a) return 0;
    a->monitoring = monitoring != 0;
    a->monitorable = monitorable != 0;
    return 1;
}

int32_t aver_phys_area_flags(int32_t area, int32_t* outMonitoring, int32_t* outMonitorable) {
    Area* a = areaOf(area);
    if (!a) return 0;
    if (outMonitoring) *outMonitoring = a->monitoring ? 1 : 0;
    if (outMonitorable) *outMonitorable = a->monitorable ? 1 : 0;
    return 1;
}

int32_t aver_phys_area_set_collision(int32_t area, uint32_t layerBits, uint32_t maskBits) {
    Area* a = areaOf(area);
    if (!a) return 0;
    a->layerSet = true;
    a->layerBits = layerBits;
    a->maskBits = maskBits;
    return 1;
}

int32_t aver_phys_area_collision(int32_t area, uint32_t* outLayerBits, uint32_t* outMaskBits) {
    Area* a = areaOf(area);
    if (!a) return 0;
    uint32_t layer = a->layerBits;
    if (!a->layerSet) {
        if (const JPH::BodyID* id = idOf(area)) layer = 1u << Layers::userOf(bi().GetObjectLayer(*id));
    }
    if (outLayerBits) *outLayerBits = layer;
    if (outMaskBits) *outMaskBits = a->maskBits;
    return 1;
}

int32_t aver_phys_area_set_priority(int32_t area, int32_t priority) {
    Area* a = areaOf(area);
    if (!a) return 0;
    a->priority = priority;
    return 1;
}

int32_t aver_phys_area_set_gravity(int32_t area, int32_t mode, float magnitudeCmS2, float dx, float dy, float dz) {
    Area* a = areaOf(area);
    if (!a) return 0;
    if (!validMode(mode) || !std::isfinite(magnitudeCmS2) || !finite3(dx, dy, dz)) return refuse(AbiError::InvalidArgument);
    a->gravMode = mode;
    a->gravMag = magnitudeCmS2 / kCmPerMetre;
    a->gravDir = unitJolt(dx, dy, dz);
    return 1;
}

int32_t aver_phys_area_set_gravity_point(int32_t area, int32_t on, float cx, float cy, float cz, float unitDistanceCm) {
    Area* a = areaOf(area);
    if (!a) return 0;
    if (!finite3(cx, cy, cz) || !std::isfinite(unitDistanceCm) || unitDistanceCm < 0.0f) return refuse(AbiError::InvalidArgument);
    a->point = on != 0;
    const JPH::Vec3 c = toJolt(Vec3(cx, cy, cz));
    a->pointLocal = JPH::Float3(c.GetX(), c.GetY(), c.GetZ());
    a->pointUnit = cmToM(unitDistanceCm);
    return 1;
}

int32_t aver_phys_area_set_damping(int32_t area, int32_t linearMode, float linear, int32_t angularMode, float angular) {
    Area* a = areaOf(area);
    if (!a) return 0;
    if (!validMode(linearMode) || !validMode(angularMode) || !std::isfinite(linear) || !std::isfinite(angular)) return refuse(AbiError::InvalidArgument);
    a->linMode = linearMode;
    a->angMode = angularMode;
    a->lin = linear < 0.0f ? 0.0f : linear;
    a->ang = angular < 0.0f ? 0.0f : angular;
    return 1;
}

int32_t aver_phys_area_set_wind(int32_t area, float forceMagnitude, float dx, float dy, float dz, float attenuation, float unitDistanceCm) {
    Area* a = areaOf(area);
    if (!a) return 0;
    if (!std::isfinite(forceMagnitude) || !finite3(dx, dy, dz) || !std::isfinite(attenuation) || !std::isfinite(unitDistanceCm)) return refuse(AbiError::InvalidArgument);
    a->windForce = forceMagnitude / kCmPerMetre;
    a->windDir = unitJolt(dx, dy, dz);
    a->windAtten = attenuation < 0.0f ? 0.0f : attenuation;
    a->windUnit = unitDistanceCm < 0.0f ? 0.0f : cmToM(unitDistanceCm);
    return 1;
}

int32_t aver_phys_area_overlap_count(int32_t area) {
    Area* a = areaOf(area);
    return a ? static_cast<int32_t>(a->reported.size()) : 0;
}

int32_t aver_phys_area_overlap_get(int32_t area, int32_t index, int32_t* outOther, int32_t* outIsArea) {
    Area* a = areaOf(area);
    if (!a) return 0;
    if (index < 0 || index >= static_cast<int32_t>(a->reported.size())) return refuse(AbiError::OutOfRange);
    const auto it = std::next(a->reported.begin(), index);
    if (outOther) *outOther = it->first;
    if (outIsArea) *outIsArea = it->second ? 1 : 0;
    return 1;
}

int32_t aver_phys_area_event_count(void) {
    return g_world && g_world->areas ? static_cast<int32_t>(g_world->areas->events.size()) : 0;
}

int32_t aver_phys_area_event_get(int32_t index, int32_t* outArea, int32_t* outOther, int32_t* outEntered, int32_t* outIsArea) {
    if (!g_world || !g_world->areas) return 0;
    const auto& ev = g_world->areas->events;
    if (index < 0 || index >= static_cast<int32_t>(ev.size())) return refuse(AbiError::OutOfRange);
    const AreaEvent& e = ev[static_cast<size_t>(index)];
    if (outArea) *outArea = e.area;
    if (outOther) *outOther = e.other;
    if (outEntered) *outEntered = e.entered;
    if (outIsArea) *outIsArea = e.isArea;
    return 1;
}

int32_t aver_phys_body_area_gravity(int32_t body, float* outGravityCmS2) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!outGravityCmS2) return refuse(AbiError::NullPointer);
    JPH::Vec3 g = g_world->system.GetGravity();
    if (g_world->areas) {
        std::map<int32_t, AreaPose> poses;
        std::map<int32_t, std::vector<InArea>> perBody;
        collectOverrides(*g_world->areas, poses, perBody);
        const auto it = perBody.find(body);
        if (it != perBody.end()) {
            const BodyNow n = readBody(*id);
            g = resolve(it->second, n.com, n.lin, n.ang).gravity;
        }
    }
    const Vec3 cm = fromJoltDir(g);
    outGravityCmS2[0] = cm.x; outGravityCmS2[1] = cm.y; outGravityCmS2[2] = cm.z;
    return 1;
}

int32_t aver_phys_body_area_damping(int32_t body, float* outLinear, float* outAngular) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    if (!outLinear || !outAngular) return refuse(AbiError::NullPointer);
    const BodyNow n = readBody(*id);
    if (!n.dynamic) return refuse(AbiError::Unsupported);
    float baseLin = n.lin, baseAng = n.ang;
    if (g_world->areas) {
        const auto di = g_world->areas->damped.find(body);
        if (di != g_world->areas->damped.end()) {
            if (n.lin == di->second.setLin) baseLin = di->second.baseLin;
            if (n.ang == di->second.setAng) baseAng = di->second.baseAng;
        }
        std::map<int32_t, AreaPose> poses;
        std::map<int32_t, std::vector<InArea>> perBody;
        collectOverrides(*g_world->areas, poses, perBody);
        const auto it = perBody.find(body);
        if (it != perBody.end()) {
            const Resolved r = resolve(it->second, n.com, baseLin, baseAng);
            if (r.damps) { baseLin = r.lin; baseAng = r.ang; }
        }
    }
    *outLinear = baseLin;
    *outAngular = baseAng;
    return 1;
}

}  // extern "C"
