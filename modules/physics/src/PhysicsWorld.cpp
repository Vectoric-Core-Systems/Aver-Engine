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
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <thread>
#include <unordered_map>

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

    float fixedStep = 1.0f / 60.0f;
    float accumulator = 0.0f;
};

std::unique_ptr<World> g_world;

// Jolt's global registration is process-wide, not per-world, so it is done once and never undone
// while the process might still create another world.
bool g_joltStarted = false;

JPH::BodyInterface& bi() { return g_world->system.GetBodyInterface(); }

int32_t addBody(const JPH::Shape* shape, const Vec3& centreCm, bool dynamic, float massKg) {
    if (!g_world) return 0;
    JPH::BodyCreationSettings s(shape, toJolt(centreCm), JPH::Quat::sIdentity(),
                                dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                dynamic ? Layers::MOVING : Layers::NON_MOVING);
    if (dynamic && massKg > 0.0f) {
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = massKg;
    }
    JPH::Body* body = bi().CreateBody(s);
    if (!body) { AVER_WARN("[Physics] body limit reached"); return 0; }
    bi().AddBody(body->GetID(), dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    const int32_t h = g_world->nextHandle++;
    g_world->bodies.emplace(h, body->GetID());
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

} // extern "C"
