// Aver.Physics — Jolt behind the engine's plain-C ABI.
//
// Everything Jolt-shaped is confined to this file and Convert.hpp. Callers work in centimetres on
// left-handed +Z-up axes and never see a JPH:: type, which is what allows the physics backend to be
// a decision rather than a dependency of the gameplay layer.
#include "aver/physics/physics_abi.h"
#include "Convert.hpp"   // src-local: it speaks Jolt, and Jolt is PRIVATE to this module

#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::physics;

namespace {

// ---- Layers ---------------------------------------------------------------------------------------
// Two object layers is the whole scheme: things that never move, and things that do. Static bodies
// are not tested against each other, which is most of what a broad-phase layer split buys.
namespace Layers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING     = 1;
static constexpr JPH::uint        NUM        = 2;
}
namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint            NUM = 2;
}

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterface() {
        m_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m_[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(l) == 0 ? "NON_MOVING" : "MOVING";
    }
#endif
private:
    JPH::BroadPhaseLayer m_[Layers::NUM];
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        // Static geometry only needs testing against things that move.
        return a != Layers::NON_MOVING || b == BroadPhaseLayers::MOVING;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a != Layers::NON_MOVING || b == Layers::MOVING;
    }
};

// Route Jolt's diagnostics into the engine log rather than stdout, which nothing in a windowed
// process reads.
void traceImpl(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    AVER_INFO("[Physics] {}", buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertFailedImpl(const char* expr, const char* msg, const char* file, JPH::uint line) {
    AVER_ERROR("[Physics] assert: {}:{} ({}) {}", file, line, expr, msg ? msg : "");
    return true;   // break into the debugger
}
#endif

// ---- Events ---------------------------------------------------------------------------------------
// What the listener records during a step, for the game to drain afterwards.
struct ContactEvent { int32_t a = 0, b = 0; Vec3 point, normal; };
struct OverlapEvent { int32_t sensor = 0, body = 0; int32_t entered = 0; };

// ---- The world ------------------------------------------------------------------------------------

struct World {
    JPH::PhysicsSystem                       system;
    BPLayerInterface                         bpLayers;
    ObjectVsBroadPhaseFilter                 objVsBp;
    ObjectLayerPairFilter                    objPair;
    std::unique_ptr<JPH::TempAllocatorImpl>  temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;

    // Handles are dense int32 starting at 1, because 0 must stay invalid. Jolt's own BodyID is a
    // packed index+generation that would satisfy that too, but exposing it would leak a Jolt type
    // through an ABI whose entire point is that it does not.
    std::unordered_map<int32_t, JPH::BodyID> bodies;
    std::unordered_map<int32_t, JPH::Ref<JPH::CharacterVirtual>> characters;
    int32_t nextHandle = 1;

    // Reverse lookup, so a contact reported as two BodyIDs can be handed back as the handles the
    // caller actually knows. Kept in step with `bodies` on every add and remove.
    std::unordered_map<JPH::BodyID, int32_t> byId;
    // Which handles are sensors, so an overlap can be reported with the sensor named first however
    // Jolt happened to order the pair.
    std::unordered_map<int32_t, bool> sensors;

    // Event queues, written from Jolt's worker threads under the mutex and drained by the caller
    // between steps. See the ABI header for why this is polled rather than called back.
    std::mutex                 eventMutex;
    std::vector<ContactEvent>  contacts;
    std::vector<OverlapEvent>  overlaps;

    float fixedStep = 1.0f / 60.0f;
    float accumulator = 0.0f;
};

std::unique_ptr<World> g_world;

// Records contacts and sensor overlaps as Jolt finds them.
//
// Every method here runs on a PHYSICS WORKER THREAD, possibly several at once, which is the entire
// reason this records rather than dispatches: the only thing safe to do from here is append under a
// lock. Anything that touched gameplay state, allocated through the managed heap, or called back
// across the ABI would be doing it from a thread the CLR has never seen, in the middle of a step.
class EventListener final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        if (!g_world) return;
        const int32_t ha = handleOf(a.GetID()), hb = handleOf(b.GetID());
        if (!ha || !hb) return;

        std::lock_guard<std::mutex> lock(g_world->eventMutex);
        if (a.IsSensor() || b.IsSensor()) {
            const bool aIsSensor = a.IsSensor();
            g_world->overlaps.push_back(OverlapEvent{aIsSensor ? ha : hb, aIsSensor ? hb : ha, 1});
            return;
        }
        ContactEvent e;
        e.a = ha; e.b = hb;
        // The manifold's base point, in engine space. One point per contact is what gameplay wants
        // ("where did it hit"); the full manifold is a solver detail.
        e.point  = fromJolt(manifold.GetWorldSpaceContactPointOn1(0));
        // fromJoltUnit, not fromJoltDir: the direction converters SCALE metres to centimetres, so a
        // unit normal put through one comes back a hundred times too long. The raycast normal uses
        // the same converter for the same reason.
        e.normal = fromJoltUnit(manifold.mWorldSpaceNormal);
        g_world->contacts.push_back(e);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        if (!g_world) return;
        const int32_t ha = handleOf(pair.GetBody1ID()), hb = handleOf(pair.GetBody2ID());
        if (!ha || !hb) return;
        std::lock_guard<std::mutex> lock(g_world->eventMutex);
        // A removal is only interesting for SENSORS, where it is the "left the volume" edge. For two
        // solid bodies, ceasing to touch is not an event gameplay has asked for.
        const auto sa = g_world->sensors.find(ha), sb = g_world->sensors.find(hb);
        const bool aIsSensor = sa != g_world->sensors.end() && sa->second;
        const bool bIsSensor = sb != g_world->sensors.end() && sb->second;
        if (!aIsSensor && !bIsSensor) return;
        g_world->overlaps.push_back(OverlapEvent{aIsSensor ? ha : hb, aIsSensor ? hb : ha, 0});
    }

private:
    static int32_t handleOf(const JPH::BodyID& id) {
        const auto it = g_world->byId.find(id);
        return it == g_world->byId.end() ? 0 : it->second;
    }
};

EventListener g_listener;

// Jolt's global registration is process-wide, not per-world, so it is done once and never undone
// while the process might still create another world.
bool g_joltStarted = false;

JPH::BodyInterface& bi() { return g_world->system.GetBodyInterface(); }

int32_t addBody(const JPH::Shape* shape, const Vec3& centreCm, bool dynamic, float massKg,
                bool sensor = false) {
    if (!g_world) return 0;
    // A sensor sits in the MOVING layer even though it never moves: the layer pair filter skips
    // NON_MOVING against NON_MOVING, so a static-layer sensor would never be told about the static
    // world -- and, more to the point, a trigger that only notices moving things is what is wanted.
    JPH::BodyCreationSettings s(shape, toJolt(centreCm), JPH::Quat::sIdentity(),
                                dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                (dynamic || sensor) ? Layers::MOVING : Layers::NON_MOVING);
    if (dynamic && massKg > 0.0f) {
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = massKg;
    }
    s.mIsSensor = sensor;
    JPH::Body* body = bi().CreateBody(s);
    if (!body) { AVER_WARN("[Physics] body limit reached"); return 0; }
    bi().AddBody(body->GetID(), dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    const int32_t h = g_world->nextHandle++;
    g_world->bodies.emplace(h, body->GetID());
    g_world->byId.emplace(body->GetID(), h);   // the contact listener maps back through this
    if (sensor) g_world->sensors.emplace(h, true);
    return h;
}

const JPH::BodyID* findBody(int32_t h) {
    if (!g_world) return nullptr;
    auto it = g_world->bodies.find(h);
    return it == g_world->bodies.end() ? nullptr : &it->second;
}

JPH::CharacterVirtual* findCharacter(int32_t h) {
    if (!g_world) return nullptr;
    auto it = g_world->characters.find(h);
    return it == g_world->characters.end() ? nullptr : it->second.GetPtr();
}

void writeVec(float* out, const Vec3& v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }

} // namespace

// ---- ABI ------------------------------------------------------------------------------------------

extern "C" {

int32_t aver_phys_init(void) {
    if (g_world) return 1;

    if (!g_joltStarted) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = traceImpl;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assertFailedImpl;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        g_joltStarted = true;
    }

    g_world = std::make_unique<World>();
    // 10 MB scratch for a frame's contacts, and one worker per core bar the main thread and one
    // spare -- the same shape as the engine's other pools, so physics does not starve rendering.
    g_world->temp = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    const int workers = static_cast<int>(std::thread::hardware_concurrency()) - 2;
    g_world->jobs = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers > 1 ? workers : 1);

    g_world->system.Init(4096, 0, 8192, 2048,
                         g_world->bpLayers, g_world->objVsBp, g_world->objPair);
    g_world->system.SetContactListener(&g_listener);
    // One g downward on the engine's up axis. Set through the converter rather than written as a
    // Jolt vector, so the sign convention has exactly one definition.
    g_world->system.SetGravity(toJoltDir(Vec3(0.0f, 0.0f, -980.0f)));
    AVER_INFO("[Physics] Jolt {}.{}.{} started ({} worker threads, {:.4f}s fixed step)",
              JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
              workers > 1 ? workers : 1, g_world->fixedStep);
    return 1;
}

void aver_phys_shutdown(void) {
    if (!g_world) return;
    // Characters hold refs into the system; drop them before the system goes.
    g_world->characters.clear();
    for (auto& [h, id] : g_world->bodies) bi().RemoveBody(id), bi().DestroyBody(id);
    g_world->bodies.clear();
    g_world->byId.clear();
    g_world->sensors.clear();
    // Unhook the listener before the system goes: it holds a pointer to a file-scope object that
    // outlives the world, and a step in flight must not find it half torn down.
    g_world->system.SetContactListener(nullptr);
    g_world.reset();
    AVER_INFO("[Physics] stopped");
}

int32_t aver_phys_ready(void) { return g_world ? 1 : 0; }

void aver_phys_set_gravity(float x, float y, float z) {
    if (g_world) g_world->system.SetGravity(toJoltDir(Vec3(x, y, z)));
}

float aver_phys_fixed_step(void) { return g_world ? g_world->fixedStep : 1.0f / 60.0f; }

int32_t aver_phys_set_fixed_step(float seconds) {
    if (!g_world || seconds <= 0.0f || seconds > 0.5f) return 0;
    g_world->fixedStep = seconds;
    return 1;
}

int32_t aver_phys_step(float dt) {
    if (!g_world || dt <= 0.0f) return 0;
    // Clamp the CATCH-UP, not the frame: a 5-second stall (a breakpoint, a shader compile) must not
    // become 300 steps of simulation that look like the world exploded.
    // Clear last call's events before anything can append to them, so what the caller drains after
    // this returns describes exactly the steps this call ran -- and never a mix of two frames.
    {
        std::lock_guard<std::mutex> lock(g_world->eventMutex);
        g_world->contacts.clear();
        g_world->overlaps.clear();
    }

    const float maxCatchUp = g_world->fixedStep * 8.0f;
    g_world->accumulator += dt < maxCatchUp ? dt : maxCatchUp;

    int32_t steps = 0;
    while (g_world->accumulator >= g_world->fixedStep) {
        g_world->accumulator -= g_world->fixedStep;
        // Characters are integrated before the solver so their swept motion sees this step's world.
        for (auto& [h, ch] : g_world->characters) {
            // CharacterVirtual does NOT integrate gravity itself -- it is a swept shape, not a rigid
            // body, and its velocity is whatever the caller last set. Without this a character hangs
            // in the air at its spawn height, which is exactly what PhysicsTest caught.
            //
            // Only the VERTICAL component is touched: horizontal velocity belongs to whoever is
            // driving the character, and stamping on it here would make input feel like ice.
            const JPH::Vec3 up = ch->GetUp();
            JPH::Vec3 v = ch->GetLinearVelocity();
            const float vUp = v.Dot(up);
            const bool grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            if (grounded && vUp <= 0.0f) {
                // Supported: cancel the downward part instead of letting it accumulate, or a
                // character standing still builds up a huge sink velocity that fires the moment it
                // steps off a ledge.
                v -= up * vUp;
            } else {
                // Airborne, or moving upward under a jump: fall normally.
                v += g_world->system.GetGravity() * g_world->fixedStep;
            }
            ch->SetLinearVelocity(v);

            JPH::CharacterVirtual::ExtendedUpdateSettings us;
            ch->ExtendedUpdate(g_world->fixedStep,
                               g_world->system.GetGravity(),
                               us,
                               g_world->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                               g_world->system.GetDefaultLayerFilter(Layers::MOVING),
                               {}, {}, *g_world->temp);
        }
        g_world->system.Update(g_world->fixedStep, 1, g_world->temp.get(), g_world->jobs.get());
        ++steps;
    }
    return steps;
}

int32_t aver_phys_add_static_box(float cx, float cy, float cz, float hx, float hy, float hz) {
    if (!g_world) return 0;
    // Half-extents are a SIZE, so the axis permutation applies but the sign flip is irrelevant --
    // take absolutes so a mirrored extent cannot produce a degenerate shape Jolt will reject.
    const Vec3 he = fromJoltUnit(toJoltUnit(Vec3(hx, hy, hz)));
    JPH::BoxShapeSettings shape(JPH::Vec3(std::abs(cmToM(he.y)), std::abs(cmToM(he.z)), std::abs(cmToM(he.x))));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] static box: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), false, 0.0f);
}

int32_t aver_phys_add_dynamic_box(float cx, float cy, float cz,
                                  float hx, float hy, float hz, float massKg) {
    if (!g_world) return 0;
    JPH::BoxShapeSettings shape(JPH::Vec3(std::abs(cmToM(hy)), std::abs(cmToM(hz)), std::abs(cmToM(hx))));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] dynamic box: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), true, massKg);
}

int32_t aver_phys_add_dynamic_sphere(float cx, float cy, float cz, float radius, float massKg) {
    if (!g_world) return 0;
    JPH::SphereShapeSettings shape(cmToM(radius));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] sphere: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), true, massKg);
}

int32_t aver_phys_remove_body(int32_t body) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    bi().RemoveBody(*id);
    bi().DestroyBody(*id);
    // Every index that names this body has to go with it. Leaving byId behind would let a contact
    // reported against a RECYCLED BodyID resolve to the dead handle -- a stale-handle bug that only
    // appears once Jolt reuses the slot, which is exactly the kind that survives testing.
    g_world->byId.erase(*id);
    g_world->sensors.erase(body);
    g_world->bodies.erase(body);
    return 1;
}

int32_t aver_phys_body_position(int32_t body, float* outXyz) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !outXyz) return 0;
    writeVec(outXyz, fromJolt(bi().GetCenterOfMassPosition(*id)));
    return 1;
}

int32_t aver_phys_body_rotation(int32_t body, float* outQuat) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !outQuat) return 0;
    const Quat q = fromJolt(bi().GetRotation(*id));
    outQuat[0] = q.x; outQuat[1] = q.y; outQuat[2] = q.z; outQuat[3] = q.w;
    return 1;
}

int32_t aver_phys_body_velocity(int32_t body, float* outXyz) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !outXyz) return 0;
    writeVec(outXyz, fromJoltDir(bi().GetLinearVelocity(*id)));
    return 1;
}

int32_t aver_phys_body_set_position(int32_t body, float x, float y, float z) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    bi().SetPosition(*id, toJolt(Vec3(x, y, z)), JPH::EActivation::Activate);
    return 1;
}

int32_t aver_phys_body_set_velocity(int32_t body, float x, float y, float z) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    bi().SetLinearVelocity(*id, toJoltDir(Vec3(x, y, z)));
    return 1;
}

int32_t aver_phys_body_count(void) { return g_world ? static_cast<int32_t>(g_world->bodies.size()) : 0; }

int32_t aver_phys_character_create(float radius, float height, float x, float y, float z) {
    if (!g_world) return 0;
    // Jolt's capsule is described by the HALF height of its cylinder, so the caps have to come out of
    // the total first. A total shorter than its own diameter is not a capsule.
    const float rM = cmToM(radius);
    const float halfCyl = cmToM(height) * 0.5f - rM;
    if (halfCyl <= 0.0f) {
        AVER_WARN("[Physics] character {}cm tall is too short for radius {}cm", height, radius);
        return 0;
    }
    JPH::CapsuleShapeSettings capsule(halfCyl, rM);
    capsule.SetEmbedded();
    auto res = capsule.Create();
    if (res.HasError()) { AVER_WARN("[Physics] character: {}", res.GetError().c_str()); return 0; }

    JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings();
    s->mShape = res.Get();
    s->mUp = toJoltUnit(Vec3(0, 0, 1));            // the engine's up, in Jolt's axes
    s->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    // The plane below the feet that stops the capsule catching on its own bottom cap.
    s->mSupportingVolume = JPH::Plane(toJoltUnit(Vec3(0, 0, 1)), -rM);
    JPH::Ref<JPH::CharacterVirtual> ch =
        new JPH::CharacterVirtual(s, toJolt(Vec3(x, y, z)), JPH::Quat::sIdentity(), 0, &g_world->system);
    const int32_t h = g_world->nextHandle++;
    g_world->characters.emplace(h, ch);
    return h;
}

int32_t aver_phys_character_destroy(int32_t ch) {
    if (!g_world || !g_world->characters.count(ch)) return 0;
    g_world->characters.erase(ch);
    return 1;
}

int32_t aver_phys_character_set_velocity(int32_t ch, float vx, float vy, float vz) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c) return 0;
    c->SetLinearVelocity(toJoltDir(Vec3(vx, vy, vz)));
    return 1;
}

int32_t aver_phys_character_velocity(int32_t ch, float* outXyz) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c || !outXyz) return 0;
    writeVec(outXyz, fromJoltDir(c->GetLinearVelocity()));
    return 1;
}

int32_t aver_phys_character_position(int32_t ch, float* outXyz) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c || !outXyz) return 0;
    writeVec(outXyz, fromJolt(c->GetPosition()));
    return 1;
}

int32_t aver_phys_character_set_position(int32_t ch, float x, float y, float z) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c) return 0;
    c->SetPosition(toJolt(Vec3(x, y, z)));
    return 1;
}

int32_t aver_phys_character_grounded(int32_t ch) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c) return 0;
    return c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround ? 1 : 0;
}

int32_t aver_phys_raycast(float ox, float oy, float oz, float dx, float dy, float dz,
                          float maxDistCm, float* outPoint, float* outNormal) {
    if (!g_world) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len <= 1e-6f || maxDistCm <= 0.0f) return 0;

    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d    = toJoltUnit(Vec3(dir.x/len, dir.y/len, dir.z/len)) * cmToM(maxDistCm);
    JPH::RRayCast ray{from, d};
    JPH::RayCastResult hit;
    if (!g_world->system.GetNarrowPhaseQuery().CastRay(ray, hit)) return 0;

    // Map the hit body back to OUR handle. Linear, but a raycast that hits is rare compared with the
    // bodies it could have hit, and a second index would be another thing to keep in step.
    int32_t handle = 0;
    for (const auto& [h, id] : g_world->bodies) if (id == hit.mBodyID) { handle = h; break; }

    if (outPoint) writeVec(outPoint, fromJolt(ray.GetPointOnRay(hit.mFraction)));
    if (outNormal) {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
                hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
            writeVec(outNormal, fromJoltUnit(n));
        } else {
            writeVec(outNormal, Vec3(0, 0, 1));
        }
    }
    return handle;
}

// ---- Arbitrary collision geometry ------------------------------------------------------------------

int32_t aver_phys_add_convex_hull(const float* pts, int32_t count, float cx, float cy, float cz,
                                  int32_t dynamic, float massKg) {
    if (!g_world || !pts || count < 4) return 0;   // fewer than four points has no volume
    JPH::Array<JPH::Vec3> hull;
    hull.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
        hull.push_back(toJolt(Vec3(pts[i*3+0], pts[i*3+1], pts[i*3+2])));

    JPH::ConvexHullShapeSettings s(hull);
    s.SetEmbedded();
    auto res = s.Create();
    if (res.HasError()) { AVER_WARN("[Physics] convex hull: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), dynamic != 0, massKg);
}

int32_t aver_phys_add_mesh(const float* verts, int32_t vertexCount,
                           const int32_t* indices, int32_t indexCount,
                           float cx, float cy, float cz) {
    if (!g_world || !verts || !indices || vertexCount < 3 || indexCount < 3) return 0;

    JPH::VertexList vlist;
    vlist.reserve(static_cast<size_t>(vertexCount));
    for (int32_t i = 0; i < vertexCount; ++i) {
        const JPH::Vec3 v = toJolt(Vec3(verts[i*3+0], verts[i*3+1], verts[i*3+2]));
        vlist.push_back(JPH::Float3(v.GetX(), v.GetY(), v.GetZ()));
    }

    JPH::IndexedTriangleList tris;
    tris.reserve(static_cast<size_t>(indexCount / 3));
    for (int32_t i = 0; i + 2 < indexCount; i += 3) {
        const JPH::uint32 a = static_cast<JPH::uint32>(indices[i]);
        const JPH::uint32 b = static_cast<JPH::uint32>(indices[i+1]);
        const JPH::uint32 c = static_cast<JPH::uint32>(indices[i+2]);
        if (a >= static_cast<JPH::uint32>(vertexCount) ||
            b >= static_cast<JPH::uint32>(vertexCount) ||
            c >= static_cast<JPH::uint32>(vertexCount)) {
            AVER_WARN("[Physics] mesh: index out of range, triangle skipped");
            continue;
        }
        // WINDING IS REVERSED. The axis map that takes the engine's left-handed space to Jolt's
        // right-handed one has determinant -1, which mirrors the mesh -- so a triangle that faced
        // outwards now faces in, and a mesh whose normals point inwards collides on the wrong side.
        // Swapping two indices puts the winding back.
        tris.push_back(JPH::IndexedTriangle(a, c, b));
    }
    if (tris.empty()) { AVER_WARN("[Physics] mesh: no usable triangles"); return 0; }

    JPH::MeshShapeSettings s(vlist, tris);
    s.SetEmbedded();
    auto res = s.Create();
    if (res.HasError()) { AVER_WARN("[Physics] mesh: {}", res.GetError().c_str()); return 0; }
    // Static only: a mesh has no interior, so nothing can resolve a penetration against it. Jolt
    // rejects a dynamic one outright, and this is the clearer place to say why.
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f);
}

int32_t aver_phys_add_heightfield(const float* samples, int32_t sampleCount, float spacingCm,
                                  float cx, float cy, float cz) {
    if (!g_world || !samples || sampleCount < 2 || spacingCm <= 0.0f) return 0;
    // Jolt requires the sample count to be a multiple of its block size; round DOWN so a caller's
    // grid is cropped rather than read past the end of.
    const int32_t n = (sampleCount / 8) * 8;
    if (n < 8) { AVER_WARN("[Physics] heightfield needs at least 8x8 samples"); return 0; }
    if (n != sampleCount)
        AVER_WARN("[Physics] heightfield {}x{} cropped to {}x{} (Jolt needs a multiple of 8)",
                  sampleCount, sampleCount, n, n);

    JPH::Array<float> heights;
    heights.reserve(static_cast<size_t>(n) * static_cast<size_t>(n));
    for (int32_t y = 0; y < n; ++y)
        for (int32_t x = 0; x < n; ++x)
            heights.push_back(cmToM(samples[static_cast<size_t>(y) * sampleCount + x]));

    // A heightfield is Y-up in Jolt's own axes by construction, so it is built directly there rather
    // than through the axis map -- the grid's rows are already a horizontal plane.
    JPH::HeightFieldShapeSettings s(heights.data(), JPH::Vec3::sZero(),
                                    JPH::Vec3(cmToM(spacingCm), 1.0f, cmToM(spacingCm)),
                                    static_cast<JPH::uint32>(n));
    s.SetEmbedded();
    auto res = s.Create();
    if (res.HasError()) { AVER_WARN("[Physics] heightfield: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f);
}

// ---- Sensors --------------------------------------------------------------------------------------

int32_t aver_phys_add_sensor_box(float cx, float cy, float cz, float hx, float hy, float hz) {
    if (!g_world) return 0;
    // Same permutation and the same absolutes as aver_phys_add_static_box: half-extents are a SIZE,
    // so the axis map applies but the sign flip does not, and a mirrored extent would otherwise make
    // a degenerate shape Jolt rejects.
    const Vec3 he = fromJoltUnit(toJoltUnit(Vec3(hx, hy, hz)));
    JPH::BoxShapeSettings box(JPH::Vec3(std::abs(cmToM(he.y)), std::abs(cmToM(he.z)), std::abs(cmToM(he.x))));
    box.SetEmbedded();
    auto res = box.Create();
    if (res.HasError()) { AVER_WARN("[Physics] sensor box: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f, /*sensor*/true);
}

int32_t aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius) {
    if (!g_world || radius <= 0.0f) return 0;
    JPH::SphereShapeSettings sph(cmToM(radius));
    sph.SetEmbedded();
    auto res = sph.Create();
    if (res.HasError()) { AVER_WARN("[Physics] sensor sphere: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f, /*sensor*/true);
}

// ---- Event queues ---------------------------------------------------------------------------------

int32_t aver_phys_contact_count(void) {
    if (!g_world) return 0;
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    return static_cast<int32_t>(g_world->contacts.size());
}

int32_t aver_phys_contact_get(int32_t index, int32_t* outA, int32_t* outB,
                              float* outPoint, float* outNormal) {
    if (!g_world || index < 0) return 0;
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    if (index >= static_cast<int32_t>(g_world->contacts.size())) return 0;
    const ContactEvent& e = g_world->contacts[static_cast<size_t>(index)];
    if (outA) *outA = e.a;
    if (outB) *outB = e.b;
    if (outPoint)  writeVec(outPoint, e.point);
    if (outNormal) writeVec(outNormal, e.normal);
    return 1;
}

int32_t aver_phys_overlap_count(void) {
    if (!g_world) return 0;
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    return static_cast<int32_t>(g_world->overlaps.size());
}

int32_t aver_phys_overlap_get(int32_t index, int32_t* outSensor, int32_t* outBody, int32_t* outEntered) {
    if (!g_world || index < 0) return 0;
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    if (index >= static_cast<int32_t>(g_world->overlaps.size())) return 0;
    const OverlapEvent& e = g_world->overlaps[static_cast<size_t>(index)];
    if (outSensor)  *outSensor  = e.sensor;
    if (outBody)    *outBody    = e.body;
    if (outEntered) *outEntered = e.entered;
    return 1;
}

// ---- Shape queries --------------------------------------------------------------------------------

int32_t aver_phys_overlap_sphere(float x, float y, float z, float radius,
                                 int32_t* outBodies, int32_t maxBodies) {
    if (!g_world || radius <= 0.0f || !outBodies || maxBodies <= 0) return 0;
    JPH::SphereShape shape(cmToM(radius));
    shape.SetEmbedded();

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const JPH::RMat44 xf = JPH::RMat44::sTranslation(toJolt(Vec3(x, y, z)));
    g_world->system.GetNarrowPhaseQuery().CollideShape(
        &shape, JPH::Vec3::sReplicate(1.0f), xf, JPH::CollideShapeSettings{},
        JPH::RVec3::sZero(), collector);

    int32_t n = 0;
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        if (n >= maxBodies) break;   // truncated: the caller compares n against maxBodies to notice
        const auto it = g_world->byId.find(hit.mBodyID2);
        if (it != g_world->byId.end()) outBodies[n++] = it->second;
    }
    return n;
}

int32_t aver_phys_sphere_cast(float ox, float oy, float oz, float dx, float dy, float dz,
                              float maxDistCm, float radius, float* outPoint, float* outNormal) {
    if (!g_world || radius <= 0.0f || maxDistCm <= 0.0f) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len <= 1e-6f) return 0;

    JPH::SphereShape shape(cmToM(radius));
    shape.SetEmbedded();
    const JPH::RMat44 start = JPH::RMat44::sTranslation(toJolt(Vec3(ox, oy, oz)));
    const JPH::Vec3 sweep = toJoltUnit(Vec3(dir.x/len, dir.y/len, dir.z/len)) * cmToM(maxDistCm);

    JPH::RShapeCast cast(&shape, JPH::Vec3::sReplicate(1.0f), start, sweep);
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    g_world->system.GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings{},
                                                    JPH::RVec3::sZero(), collector);
    if (!collector.HadHit()) return 0;

    const auto it = g_world->byId.find(collector.mHit.mBodyID2);
    const int32_t handle = it == g_world->byId.end() ? 0 : it->second;
    if (outPoint)  writeVec(outPoint,  fromJolt(collector.mHit.mContactPointOn2));
    if (outNormal) writeVec(outNormal, fromJoltUnit(-collector.mHit.mPenetrationAxis.Normalized()));
    return handle;
}

} // extern "C"
