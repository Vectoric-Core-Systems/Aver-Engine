// Aver.Physics — Jolt behind the engine's plain-C ABI. Everything Jolt-shaped is confined to this
// file and Convert.hpp.
#include "aver/physics/physics_abi.h"
#include "Convert.hpp"   // src-local: it speaks Jolt, and Jolt is PRIVATE to this module
#include "Buoyancy.hpp"  // src-local for the same reason: WaterVolume holds JPH:: types
#include "PhysicsInternal.hpp"  // the world and its handle tables, shared with the sibling ABI files

#include "aver/core/Log.hpp"
// The shared error vocabulary. findBody records the reason a handle lookup failed; aver_phys_last_error
// hands it back. See ErrorCodes.hpp for why it is a separate channel from the return value.
#include "aver/core/ErrorCodes.hpp"

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
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
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
// The world, the handle tables and the lookups now live in PhysicsInternal.hpp so the joint,
// shape and body-dynamics files can reach them too. Pulled in unqualified here so every call
// site below is the same text it was before the extraction -- which is what lets the three
// existing physics suites act as the check that nothing changed.
using namespace aver::physics::detail;

namespace {

// Routes Jolt's diagnostics into the engine log.
void traceImpl(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    AVER_INFO("[Physics] {}", buf);
}

#ifdef JPH_ENABLE_ASSERTS
// Logs a Jolt assertion and asks it to break into the debugger.
bool assertFailedImpl(const char* expr, const char* msg, const char* file, JPH::uint line) {
    AVER_ERROR("[Physics] assert: {}:{} ({}) {}", file, line, expr, msg ? msg : "");
    return true;   // break into the debugger
}
#endif

// Records contacts and sensor overlaps as Jolt finds them.
// Every method here runs on a PHYSICS WORKER THREAD, possibly several at once.
class EventListener final : public JPH::ContactListener {
public:
    // Appends a contact, or a sensor-enter overlap when either body is a sensor.
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
        e.point  = fromJolt(manifold.GetWorldSpaceContactPointOn1(0));
        // fromJoltUnit, not fromJoltDir: a unit normal must not be scaled.
        e.normal = fromJoltUnit(manifold.mWorldSpaceNormal);
        g_world->contacts.push_back(e);
    }

    // Appends a sensor-exit overlap. Two solid bodies ceasing to touch is not reported.
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        if (!g_world) return;
        const int32_t ha = handleOf(pair.GetBody1ID()), hb = handleOf(pair.GetBody2ID());
        if (!ha || !hb) return;
        std::lock_guard<std::mutex> lock(g_world->eventMutex);
        const auto sa = g_world->sensors.find(ha), sb = g_world->sensors.find(hb);
        const bool aIsSensor = sa != g_world->sensors.end() && sa->second;
        const bool bIsSensor = sb != g_world->sensors.end() && sb->second;
        if (!aIsSensor && !bIsSensor) return;
        g_world->overlaps.push_back(OverlapEvent{aIsSensor ? ha : hb, aIsSensor ? hb : ha, 0});
    }

private:
    // Our handle for a Jolt body id, or 0.
    static int32_t handleOf(const JPH::BodyID& id) {
        const auto it = g_world->byId.find(id);
        return it == g_world->byId.end() ? 0 : it->second;
    }
};

EventListener g_listener;

// Jolt's global registration is process-wide, not per-world, so it is done once and never undone.
bool g_joltStarted = false;

// ---- Shared mesh shapes -----------------------------------------------------------------------
// aver_phys_create_mesh_shape's own table: a SEPARATE handle space from World::bodies/nextHandle
// above, file-local because only the three functions next to aver_phys_add_mesh need to see it.
// JPH::Ref keeps a shape alive for as long as either this table or a body's own ScaledShape (built
// from it in aver_phys_add_mesh_shape_body) still points at it -- Jolt's ordinary refcounting, no
// extra bookkeeping needed here for that half.
//
// NOT CLEARED ON aver_phys_shutdown, unlike the body/character tables: a JPH::Shape is bare geometry
// with no reference to the JPH::PhysicsSystem it happens to get used in, so it does not go stale the
// way a body handle or a water-volume override (see aver_phys_shutdown's own comment on that leak)
// would if carried into a freshly created world. A shape handle a caller forgot to release simply
// outlives the world that last used it, exactly as it would if nothing had been shut down at all.
std::unordered_map<int32_t, JPH::Ref<JPH::Shape>> g_meshShapes;
int32_t g_nextMeshShape = 1;

// The shape behind a handle, or nullptr.
JPH::Shape* meshShapeFor(int32_t h) {
    const auto it = g_meshShapes.find(h);
    return it == g_meshShapes.end() ? nullptr : it->second.GetPtr();
}

} // namespace

// ---- what PhysicsInternal.hpp declares ------------------------------------------------------------
// Defined HERE rather than in the header because this file owns the world's lifetime: aver_phys_init
// creates it and aver_phys_shutdown destroys it, and a second definition anywhere else would be a
// second world.
namespace aver::physics::detail {

std::unique_ptr<World> g_world;

JPH::BodyInterface& bi() { return g_world->system.GetBodyInterface(); }

// A REFUSED BODY IS REPORTED ONCE PER WORLD, with the ceiling it hit. It used to be one warning per
// refused body, which on a city-scale level (thousands of colliding placements past the old 4,096
// ceiling) buried the log in identical lines while saying nothing about why. Re-armed by
// aver_phys_init, so a later world that overflows says so too.
bool g_bodyLimitWarned = false;

void warnBodyLimit() {
    if (g_bodyLimitWarned) return;
    g_bodyLimitWarned = true;
    AVER_WARN("[Physics] body limit reached ({} bodies): further bodies are refused and will not collide",
              g_world ? g_world->system.GetMaxBodies() : 0u);
}

int32_t addBody(const JPH::Shape* shape, const Vec3& centreCm, bool dynamic, float massKg,
                bool sensor, u32 userLayer) {
    if (!g_world) return 0;
    if (userLayer >= Layers::kUserLayerCount) {
        AVER_WARN("[Physics] collision layer {} is out of range; using 0", userLayer);
        userLayer = 0;
    }
    // A sensor sits in the MOVING half even though it never moves, so it is told about the world.
    // At userLayer 0 this encodes to exactly the NON_MOVING/MOVING values that were the only two
    // object layers before user layers existed.
    JPH::BodyCreationSettings s(shape, toJolt(centreCm), JPH::Quat::sIdentity(),
                                dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                Layers::encode(userLayer, dynamic || sensor));
    if (dynamic && massKg > 0.0f) {
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = massKg;
    }
    s.mIsSensor = sensor;
    JPH::Body* body = bi().CreateBody(s);
    if (!body) { warnBodyLimit(); return 0; }
    bi().AddBody(body->GetID(), dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    const int32_t h = g_world->nextHandle++;
    g_world->bodies.emplace(h, body->GetID());
    g_world->byId.emplace(body->GetID(), h);
    if (sensor) g_world->sensors.emplace(h, true);
    return h;
}

const JPH::BodyID* findBody(int32_t h) {
    // ---- WHERE THE ERROR CODE IS RECORDED, and why here rather than at 179 call sites ----------
    //
    // Every ABI entry point in this module returns a bare 1/0, so a caller learned THAT a call
    // failed and never WHY: `aver_phys_body_aabb` returning 0 is a dead handle, a null pointer, or
    // no world at all, and nothing told them apart.
    //
    // This function is the choke point nearly every one of those failures passes through, and it
    // ALREADY distinguishes the two interesting cases -- no world versus no such body. Setting the
    // code here covers the whole surface with one edit, where hand-wiring each `return 0` would be
    // 179 chances to instrument a return that is not an error at all (aver_phys_body_count()
    // legitimately returns 0 with no world).
    //
    // The code travels on aver_phys_last_error(), NEVER on these functions' return values -- see
    // ErrorCodes.hpp for why returning a negative code from a function whose callers write
    // `if (aver_phys_...)` would silently invert every existing call site.
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return nullptr; }
    auto it = g_world->bodies.find(h);
    if (it == g_world->bodies.end()) { aver::setAbiError(aver::AbiError::BadHandle); return nullptr; }
    aver::setAbiError(aver::AbiError::Ok);
    return &it->second;
}

JPH::CharacterVirtual* findCharacter(int32_t h) {
    if (!g_world) return nullptr;
    auto it = g_world->characters.find(h);
    return it == g_world->characters.end() ? nullptr : it->second.GetPtr();
}

void writeVec(float* out, const Vec3& v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }

} // namespace aver::physics::detail

// ---- ABI ------------------------------------------------------------------------------------------

extern "C" {

// Starts Jolt (once per process) and creates the world. Idempotent.
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
    g_bodyLimitWarned = false;
    // 10 MB scratch for a frame's contacts, and one worker per core bar the main thread and one spare.
    g_world->temp = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    const int workers = static_cast<int>(std::thread::hardware_concurrency()) - 2;
    g_world->jobs = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers > 1 ? workers : 1);

    // THE CEILINGS, fixed for the world's lifetime: Jolt sizes its body arrays and broad phase here
    // and refuses bodies past them. 4,096 bodies was a room-sized budget; a city level (NeonDistrict:
    // thousands of colliding placements, 169 terrain chunks, a box per building) ran into it and lost
    // over half its collision. 65,536 costs a few MB of bookkeeping. Pairs and contact constraints are
    // only spent on MOVING bodies touching things, which a static-heavy level has few of.
    // (PHYSICS.MAXBODIES in a project manifest is still not read -- see GameTick.hpp.)
    constexpr JPH::uint kMaxBodies = 65536;
    constexpr JPH::uint kMaxBodyPairs = 65536;
    constexpr JPH::uint kMaxContactConstraints = 16384;
    g_world->system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContactConstraints,
                         g_world->bpLayers, g_world->objVsBp, g_world->objPair);
    g_world->system.SetContactListener(&g_listener);
    // One g downward on the engine's up axis.
    g_world->system.SetGravity(toJoltDir(Vec3(0.0f, 0.0f, -980.0f)));
    AVER_INFO("[Physics] Jolt {}.{}.{} started ({} worker threads, {:.4f}s fixed step)",
              JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
              workers > 1 ? workers : 1, g_world->fixedStep);
    return 1;
}

// Destroys every character and body, unhooks the listener, and drops the world.
void aver_phys_shutdown(void) {
    // JOINTS FIRST, and the order is the whole point: a joint holds a reference to a Constraint that
    // points into the PhysicsSystem below and at the bodies in it. Destroying the world first would
    // leave PhysicsJoints.cpp's table holding references into wreckage.
    destroyAllJoints();
    // AND VEHICLES, for the same reason and in the same place: a VehicleConstraint is registered with the
    // system as a step listener and holds a raw pointer to its chassis, which the loop below is about to
    // destroy. The chassis bodies themselves are ordinary entries in `bodies` and go with the rest.
    destroyAllVehicles();
    clearCharacterStairSettings();
    if (!g_world) return;
    // Characters hold refs into the system; drop them before the system goes.
    g_world->characters.clear();
    for (auto& [h, id] : g_world->bodies) bi().RemoveBody(id), bi().DestroyBody(id);
    g_world->bodies.clear();
    g_world->byId.clear();
    g_world->sensors.clear();
    g_world->system.SetContactListener(nullptr);
    // THE WATER TABLE IS A PROCESS-WIDE SINGLETON AND THE WORLD IS NOT. Handles restart at 1 on the
    // next aver_phys_init, so anything left registered here is silently inherited by whatever body
    // happens to be issued that number in the NEXT world -- a body nobody put in water, floating.
    //
    // Found by a test, and only after its assertion was strengthened: BuoyancyTest's "no water
    // registered at all" run was getting the previous run's volume, which made its two runs come
    // back BIT-IDENTICAL while the weaker assertion it had at the time reported green.
    phys::water::waterVolumes().clearAll();
    phys::water::waterVolumes().clearPlane();
    g_world.reset();
    AVER_INFO("[Physics] stopped");
}

// 1 while a world exists.
int32_t aver_phys_ready(void) { return g_world ? 1 : 0; }

// Sets gravity in cm/s^2 on engine axes.
void aver_phys_set_gravity(float x, float y, float z) {
    if (g_world) g_world->system.SetGravity(toJoltDir(Vec3(x, y, z)));
}

// The fixed step in seconds.
float aver_phys_fixed_step(void) { return g_world ? g_world->fixedStep : 1.0f / 60.0f; }

// Changes the fixed step. Rejects anything outside (0, 0.5] seconds.
int32_t aver_phys_set_fixed_step(float seconds) {
    if (!g_world || seconds <= 0.0f || seconds > 0.5f) return 0;
    g_world->fixedStep = seconds;
    return 1;
}

// Advances the simulation in fixed steps, clamping catch-up. Returns how many steps ran.
int32_t aver_phys_step(float dt) {
    if (!g_world || dt <= 0.0f) return 0;
    // Clear last call's events first, so what the caller drains describes only the steps run here.
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
        // CharacterVirtual does not integrate gravity itself, so only the vertical component of its
        // velocity is managed here; the horizontal part belongs to whoever is driving it.
        //
        // AND THE GROUND'S MOTION IS ADDED HERE, which is what makes a character ride a train. A
        // CharacterVirtual is not a rigid body: no friction ever reaches it, and Jolt leaves following
        // the ground to the caller ("velocity = GetGroundVelocity() + horizontal speed as input by
        // player", CharacterVirtual.h's own recipe for ExtendedUpdate). Without it a kinematic deck
        // moved by aver_phys_body_move_kinematic slid out from under anyone standing on it, while the
        // dynamic crates beside them rode along on friction. The ground's velocity -- angular part
        // included, so a turning carriage swings its passengers round -- goes in for the update only
        // and comes back out afterwards (`carry`), so the velocity a driver reads and writes between
        // steps is still its own, relative to what it stands on. Doing it here rather than in each
        // driver is also what keeps it right when one frame runs several fixed steps.
        for (auto& [h, ch] : g_world->characters) {
            const JPH::Vec3 up = ch->GetUp();
            JPH::Vec3 v = ch->GetLinearVelocity();
            const float vUp = v.Dot(up);
            const bool grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            // try_emplace with an explicit zero: JPH::Vec3's default constructor leaves it uninitialised.
            JPH::Vec3& carry = g_world->characterCarry.try_emplace(h, JPH::Vec3::sZero()).first->second;
            if (grounded && vUp <= 0.0f) {
                // Supported: cancel the downward part instead of letting it accumulate.
                v -= up * vUp;
                // The ground as it moves NOW: the kinematic driver set this frame's velocity after the
                // last update measured it.
                ch->UpdateGroundVelocity();
                carry = ch->GetGroundVelocity();
            } else {
                // Airborne, or leaving the ground under a jump. What the ground was doing at that moment
                // is momentum now. Its vertical part joins the character's own velocity, once, so
                // gravity can take it away; the horizontal part stays carried until the next landing,
                // because drivers rewrite the horizontal velocity every frame and would otherwise drop
                // someone stepping off a moving train dead in the air behind it.
                const float carryUp = carry.Dot(up);
                v += up * carryUp;
                carry -= up * carryUp;
                // Fall normally.
                v += g_world->system.GetGravity() * g_world->fixedStep;
            }
            ch->SetLinearVelocity(v + carry);

            // THE STAIR DISTANCES THE CALLER ASKED FOR, rather than Jolt's defaults. Both are
            // arguments to ExtendedUpdate and not state on the character, so this is the only moment
            // aver_phys_character_set_stair_stepping's numbers can be honoured -- see
            // detail::applyCharacterStairSettings.
            JPH::CharacterVirtual::ExtendedUpdateSettings us;
            applyCharacterStairSettings(h, us);
            ch->ExtendedUpdate(g_world->fixedStep,
                               g_world->system.GetGravity(),
                               us,
                               g_world->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                               g_world->system.GetDefaultLayerFilter(Layers::MOVING),
                               {}, {}, *g_world->temp);
            // Back into the driver's frame. ExtendedUpdate may have trimmed the velocity against a
            // steep slope; whatever it left, minus what the ground lent it, is the character's own.
            ch->SetLinearVelocity(ch->GetLinearVelocity() - carry);
        }
        // BUOYANCY, BEFORE Update AND INSIDE THE FIXED LOOP. Both halves matter.
        //
        // Before, because ApplyBuoyancyImpulse is exactly that -- an impulse -- and an impulse
        // applied after the solver has already integrated this step would not be felt until the
        // next one, giving a body that is one whole step behind the water it is floating in.
        //
        // Inside the loop rather than once per aver_phys_step, because the impulse is scaled by dt:
        // applying it once with the FRAME's dt while the solver runs several fixed steps would make
        // buoyancy depend on the frame rate, which is the one thing a fixed step exists to prevent.
        //
        // The two lambdas are how Buoyancy.cpp reaches the world without seeing it: `World` is
        // file-local to this translation unit on purpose, so the table is handed the two questions
        // it needs answered rather than the struct that answers them. See Buoyancy.hpp's own note.
        {
            auto find = [](int32_t h) -> const JPH::BodyID* { return findBody(h); };
            auto each = [](const std::function<void(int32_t, const JPH::BodyID&)>& fn) {
                for (const auto& [handle, id] : g_world->bodies) fn(handle, id);
            };
            phys::water::waterVolumes().evaluate(g_world->system, find, each,
                                                 g_world->system.GetGravity(), g_world->fixedStep);
        }
        g_world->system.Update(g_world->fixedStep, 1, g_world->temp.get(), g_world->jobs.get());
        ++steps;
    }
    return steps;
}

// Adds a box that never moves. Returns its handle, or 0.
int32_t aver_phys_add_static_box(float cx, float cy, float cz, float hx, float hy, float hz) {
    if (!g_world) return 0;
    // Half-extents are a SIZE: the axis permutation applies but the sign flip does not.
    const Vec3 he = fromJoltUnit(toJoltUnit(Vec3(hx, hy, hz)));
    JPH::BoxShapeSettings shape(JPH::Vec3(std::abs(cmToM(he.y)), std::abs(cmToM(he.z)), std::abs(cmToM(he.x))));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] static box: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), false, 0.0f);
}

// Adds a box that falls and collides. Returns its handle, or 0.
int32_t aver_phys_add_dynamic_box(float cx, float cy, float cz,
                                  float hx, float hy, float hz, float massKg) {
    if (!g_world) return 0;
    JPH::BoxShapeSettings shape(JPH::Vec3(std::abs(cmToM(hy)), std::abs(cmToM(hz)), std::abs(cmToM(hx))));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] dynamic box: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), true, massKg);
}

// Adds a sphere that falls and collides. Returns its handle, or 0.
int32_t aver_phys_add_dynamic_sphere(float cx, float cy, float cz, float radius, float massKg) {
    if (!g_world) return 0;
    JPH::SphereShapeSettings shape(cmToM(radius));
    shape.SetEmbedded();
    auto res = shape.Create();
    if (res.HasError()) { AVER_WARN("[Physics] sphere: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), true, massKg);
}

// Removes and destroys a body, and drops it from every handle table.
int32_t aver_phys_remove_body(int32_t body) {
    // Drop any water override FIRST. Handles come from a monotonic counter so they are not reused
    // today, but a stale entry would still make evaluate() call findBody on a dead handle every
    // substep forever -- a slow leak of work rather than a crash, which is the kind that survives.
    phys::water::waterVolumes().clear(body);
    // A VEHICLE'S CHASSIS GOES WITH ITS VEHICLE, not before it: the constraint and its step listener hold
    // a raw pointer to this body, so they come out first. The vehicle handle is dead afterwards -- see
    // physics_vehicle_abi.h. A no-op for any body that is not a chassis.
    releaseVehicleOfBody(body);
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    bi().RemoveBody(*id);
    bi().DestroyBody(*id);
    g_world->byId.erase(*id);
    g_world->sensors.erase(body);
    g_world->bodies.erase(body);
    return 1;
}

// Writes a body's centre-of-mass position into outXyz.
int32_t aver_phys_body_position(int32_t body, float* outXyz) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !outXyz) return 0;
    writeVec(outXyz, fromJolt(bi().GetCenterOfMassPosition(*id)));
    return 1;
}

// Writes a body's WORLD-SPACE bounding box into outMin/outMax.
//
// WHY AN AABB AND NOT THE SHAPE ITSELF. There is no way to see a collider in this editor at all --
// no toggle, no wireframe, nothing -- so "does the collision match the art" has only ever been
// answerable by dropping something on it and watching. A box's world AABB IS the box for the
// axis-aligned case that most level collision actually is, and for a sphere, capsule or mesh it is
// an honest bound rather than a wrong outline. Exposing Jolt's full shape tree would mean an ABI
// that can describe every shape type and a debug renderer that can draw them; this answers the
// question people actually have -- where is the collision, and how big -- for one entry point.
//
// THROUGH THE SHAPE, not Body::GetWorldSpaceBounds(): that accessor is on Body, which would need a
// BodyLockRead to reach, while BodyInterface exposes the shape and the centre-of-mass transform
// without locking. Same answer, computed the way this ABI's other accessors already reach a body.
int32_t aver_phys_body_aabb(int32_t body, float* outMin, float* outMax) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !outMin || !outMax) return 0;
    const JPH::RefConst<JPH::Shape> shape = bi().GetShape(*id);
    if (!shape) return 0;
    const JPH::AABox box =
        shape->GetWorldSpaceBounds(bi().GetCenterOfMassTransform(*id), JPH::Vec3::sOne());
    writeVec(outMin, fromJolt(box.mMin));
    writeVec(outMax, fromJolt(box.mMax));
    return 1;
}

// The body handle at a dense index, or 0. Pairs with aver_phys_body_count, which has been
// answerable since this ABI existed while "which bodies" was not -- so nothing could iterate them.
//
// INDICES SHIFT when a body is added or removed, exactly like scene::World::at: this is for a
// walk that completes within one frame, not a handle to keep.
int32_t aver_phys_body_at(int32_t index) {
    if (!g_world || index < 0) return 0;
    if (static_cast<usize>(index) >= g_world->bodies.size()) return 0;
    // std::unordered_map has no positional access; the walk is O(n) per call and this is a debug
    // path that runs only while the collider overlay is on. Said plainly rather than hidden behind
    // a cached vector that would then need invalidating on every add and remove.
    auto it = g_world->bodies.begin();
    std::advance(it, index);
    return it->first;
}

// Writes a body's rotation into outQuat as xyzw.
int32_t aver_phys_body_rotation(int32_t body, float* outQuat) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !outQuat) return 0;
    const Quat q = fromJolt(bi().GetRotation(*id));
    outQuat[0] = q.x; outQuat[1] = q.y; outQuat[2] = q.z; outQuat[3] = q.w;
    return 1;
}

// Writes a body's linear velocity into outXyz.
int32_t aver_phys_body_velocity(int32_t body, float* outXyz) {
    const JPH::BodyID* id = findBody(body);
    if (!id || !outXyz) return 0;
    writeVec(outXyz, fromJoltDir(bi().GetLinearVelocity(*id)));
    return 1;
}

// Teleports a body and wakes it.
int32_t aver_phys_body_set_position(int32_t body, float x, float y, float z) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    bi().SetPosition(*id, toJolt(Vec3(x, y, z)), JPH::EActivation::Activate);
    return 1;
}

// Sets a body's linear velocity.
int32_t aver_phys_body_set_velocity(int32_t body, float x, float y, float z) {
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    bi().SetLinearVelocity(*id, toJoltDir(Vec3(x, y, z)));
    return 1;
}

// How many bodies are live.
int32_t aver_phys_body_count(void) { return g_world ? static_cast<int32_t>(g_world->bodies.size()) : 0; }

// Every live body handle, in one pass over the map -- the O(n) way to enumerate. aver_phys_body_at(i)
// has to walk the map to its i-th entry on every call, so looping it over [0, count) is O(n^2).
int32_t aver_phys_body_handles(int32_t* out, int32_t cap) {
    if (!g_world || !out || cap <= 0) return 0;
    int32_t n = 0;
    for (const auto& [h, id] : g_world->bodies) {
        (void)id;
        if (n >= cap) break;
        out[n++] = h;
    }
    return n;
}

int32_t aver_phys_max_bodies(void) {
    return g_world ? static_cast<int32_t>(g_world->system.GetMaxBodies()) : 0;
}

// Rebuilds the broad phase's trees for the bodies it now holds. Bodies added one at a time (a level
// load adds one static body per colliding placement) leave the trees unbalanced until this runs, and
// every ray, overlap and character query pays for that. Costly: once after a bulk add, not per frame.
int32_t aver_phys_optimize_broadphase(void) {
    if (!g_world) { aver::setAbiError(aver::AbiError::NotInitialised); return 0; }
    g_world->system.OptimizeBroadPhase();
    aver::setAbiError(aver::AbiError::Ok);
    return 1;
}

// The calling thread's last recorded reason. See the header for why this is a separate channel and
// not a changed return value.
int32_t aver_phys_last_error(void) { return static_cast<int32_t>(aver::lastAbiError()); }

// Creates a character capsule. `height` is the TOTAL height including both caps. 0 if too short.
int32_t aver_phys_character_create(float radius, float height, float x, float y, float z) {
    if (!g_world) return 0;
    // Jolt's capsule is described by the HALF height of its cylinder, so the caps come out first.
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
    // Gives the character a real, broadphase-visible companion body. Without this a CharacterVirtual
    // is invisible to every query in this file -- Jolt's own header says so outright ("cannot collide
    // with CharacterVirtual since it is not added to the broad phase") -- so a ray fired straight
    // through a live character comes back a clean miss. Jolt creates and re-syncs this body itself
    // (every Update/ExtendedUpdate, already called once per fixed step below) from the SAME shape, so
    // there is nothing else to keep in sync here.
    s->mInnerBodyShape = res.Get();
    s->mInnerBodyLayer = Layers::MOVING;
    JPH::Ref<JPH::CharacterVirtual> ch =
        new JPH::CharacterVirtual(s, toJolt(Vec3(x, y, z)), JPH::Quat::sIdentity(), 0, &g_world->system);
    const int32_t h = g_world->nextHandle++;
    g_world->characters.emplace(h, ch);
    return h;
}

// Destroys a character.
int32_t aver_phys_character_destroy(int32_t ch) {
    if (!g_world || !g_world->characters.count(ch)) return 0;
    g_world->characters.erase(ch);
    g_world->characterCarry.erase(ch);
    return 1;
}

// Sets the velocity a character wants, cm/s on engine axes.
int32_t aver_phys_character_set_velocity(int32_t ch, float vx, float vy, float vz) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c) return 0;
    c->SetLinearVelocity(toJoltDir(Vec3(vx, vy, vz)));
    return 1;
}

// Writes a character's velocity into outXyz.
int32_t aver_phys_character_velocity(int32_t ch, float* outXyz) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c || !outXyz) return 0;
    writeVec(outXyz, fromJoltDir(c->GetLinearVelocity()));
    return 1;
}

// Writes a character's position into outXyz.
int32_t aver_phys_character_position(int32_t ch, float* outXyz) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c || !outXyz) return 0;
    writeVec(outXyz, fromJolt(c->GetPosition()));
    return 1;
}

// Teleports a character.
//
// AND IT STOPS RIDING. Until the next update the character would still believe it stands on whatever
// it stood on before, and aver_phys_step would carry it with that ground's velocity -- measured at
// the NEW position, where a turning platform's rotation about a centre now far away comes out as a
// launch. RefreshContacts (Jolt's own answer for "after a character has teleported") finds the
// ground under the new position with the same filters the step uses, and the carried motion goes.
int32_t aver_phys_character_set_position(int32_t ch, float x, float y, float z) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c) return 0;
    c->SetPosition(toJolt(Vec3(x, y, z)));
    c->RefreshContacts(g_world->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                       g_world->system.GetDefaultLayerFilter(Layers::MOVING),
                       {}, {}, *g_world->temp);
    g_world->characterCarry.erase(ch);
    return 1;
}

// 1 while a character is standing on ground steep enough to hold.
int32_t aver_phys_character_grounded(int32_t ch) {
    JPH::CharacterVirtual* c = findCharacter(ch);
    if (!c) return 0;
    return c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround ? 1 : 0;
}

// Stamps a body OR character handle with a scene entity id. Backed by Jolt's own per-body user-data
// field (Body::SetUserData / CharacterVirtual::SetUserData -- the latter already propagates to the
// character's inner body, so one call covers both halves of a character). Returns 0 for a dead handle.
int32_t aver_phys_set_entity(int32_t handle, int32_t entity) {
    if (!g_world) return 0;
    if (const JPH::BodyID* id = findBody(handle)) {
        JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return 0;
        lock.GetBody().SetUserData(static_cast<JPH::uint64>(entity));
        return 1;
    }
    if (JPH::CharacterVirtual* ch = findCharacter(handle)) {
        ch->SetUserData(static_cast<JPH::uint64>(entity));
        return 1;
    }
    return 0;
}

// Casts a ray and returns the hit body handle, or 0 for a miss. Outputs are written only on a hit.
int32_t aver_phys_raycast(float ox, float oy, float oz, float dx, float dy, float dz,
                          float maxDistCm, float* outPoint, float* outNormal, int32_t* outEntity) {
    if (!g_world) return 0;
    const Vec3 dir(dx, dy, dz);
    const float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len <= 1e-6f || maxDistCm <= 0.0f) return 0;

    const JPH::Vec3 from = toJolt(Vec3(ox, oy, oz));
    const JPH::Vec3 d    = toJoltUnit(Vec3(dir.x/len, dir.y/len, dir.z/len)) * cmToM(maxDistCm);
    JPH::RRayCast ray{from, d};
    JPH::RayCastResult hit;
    if (!g_world->system.GetNarrowPhaseQuery().CastRay(ray, hit)) return 0;

    // byId is the reverse of `bodies` (addBody fills both), so this is one lookup -- it used to be a
    // walk of every body per ray, which on a city level (thousands of bodies) made each nav-bake or
    // script raycast pay for the whole level.
    int32_t handle = 0;
    if (const auto it = g_world->byId.find(hit.mBodyID); it != g_world->byId.end()) handle = it->second;
    if (handle == 0) {
        // Not a body this module created through addBody() -- the only other broadphase-visible
        // thing is a character's own inner body (see aver_phys_character_create). g_world->bodies
        // was never going to contain it: Jolt creates and owns that BodyID internally.
        for (const auto& [h, ch] : g_world->characters)
            if (!ch->GetInnerBodyID().IsInvalid() && ch->GetInnerBodyID() == hit.mBodyID) { handle = h; break; }
    }

    if (outPoint) writeVec(outPoint, fromJolt(ray.GetPointOnRay(hit.mFraction)));
    // Gated on outEntity-or-outNormal now (used to be outNormal alone): entity resolution needs this
    // same lock to read GetUserData(), but a caller asking for neither still pays for no lock at all.
    if (outEntity || outNormal) {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            if (outEntity) *outEntity = static_cast<int32_t>(lock.GetBody().GetUserData());
            if (outNormal) {
                const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
                    hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
                writeVec(outNormal, fromJoltUnit(n));
            }
        } else {
            if (outEntity) *outEntity = 0;
            if (outNormal) writeVec(outNormal, Vec3(0, 0, 1));
        }
    }
    return handle;
}

// ---- Arbitrary collision geometry ------------------------------------------------------------------

// Adds a convex hull wrapped around `count` points. Returns its handle, or 0.
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

// Adds a static triangle mesh, skipping triangles with out-of-range indices. Returns its handle, or 0.
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
        // Winding is reversed: the axis map has determinant -1, which mirrors the mesh.
        tris.push_back(JPH::IndexedTriangle(a, c, b));
    }
    if (tris.empty()) { AVER_WARN("[Physics] mesh: no usable triangles"); return 0; }

    JPH::MeshShapeSettings s(vlist, tris);
    s.SetEmbedded();
    auto res = s.Create();
    if (res.HasError()) { AVER_WARN("[Physics] mesh: {}", res.GetError().c_str()); return 0; }
    // Static only: a mesh has no interior, so nothing can resolve a penetration against it.
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f);
}

// ---- Shared mesh shapes -----------------------------------------------------------------------------

// Builds the SAME triangle-mesh shape aver_phys_add_mesh builds internally -- identical vertex
// conversion, identical winding fix, identical out-of-range-index skip -- but hands it back as a
// handle from g_meshShapes' own counter instead of wrapping it in a body. See physics_abi.h's own
// comment on why that counter is not World::nextHandle.
int32_t aver_phys_create_mesh_shape(const float* verts, int32_t vertexCount,
                                    const int32_t* indices, int32_t indexCount) {
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
            AVER_WARN("[Physics] mesh shape: index out of range, triangle skipped");
            continue;
        }
        // Same mirror as aver_phys_add_mesh: the axis map has determinant -1.
        tris.push_back(JPH::IndexedTriangle(a, c, b));
    }
    if (tris.empty()) { AVER_WARN("[Physics] mesh shape: no usable triangles"); return 0; }

    JPH::MeshShapeSettings s(vlist, tris);
    s.SetEmbedded();
    auto res = s.Create();
    if (res.HasError()) { AVER_WARN("[Physics] mesh shape: {}", res.GetError().c_str()); return 0; }

    const int32_t h = g_nextMeshShape++;
    g_meshShapes.emplace(h, res.Get());
    return h;
}

int32_t aver_phys_release_mesh_shape(int32_t shape) {
    return g_meshShapes.erase(shape) ? 1 : 0;
}

// Adds a static body wrapping `shape`, scaled through Jolt's Shape::ScaleShape rather than rebaked
// geometry. See physics_abi.h's own comment on what scales this accepts.
//
// THE SCALE CONVERSION, DERIVED, because getting it wrong is a mesh that collides at the wrong size
// with nothing reporting it. Convert.hpp's toJolt/toJoltUnit convert a POSITION or DIRECTION by a
// basis matrix B (aver (x,y,z) -> jolt (y,z,-x), det(B) = -1) applied to the vector itself -- but a
// SCALE is not a vector, it is the diagonal of a linear map D = diag(sx,sy,sz) that acts on engine-
// space vectors, and what crosses into Jolt's basis is the CONJUGATE B*D*B^-1, not B*D. For a signed
// permutation B (every row and column has exactly one entry, +-1) that conjugate is diagonal again --
// (B*D*B^T)[i,i] = B[i,k]^2 * D[k,k] for the one k where row i of B is non-zero -- and B[i,k]^2 is
// always 1 whichever sign B[i,k] carries. So EVERY SIGN SURVIVES UNCHANGED (a mirror stays a mirror)
// and only the ORDER of the three components changes, by exactly the same permutation toJoltUnit
// already applies to a direction: jolt.x <- engine.y, jolt.y <- engine.z, jolt.z <- engine.x. No
// component is negated here -- unlike a direction, whose z also flips sign -- because that sign came
// from det(B) alone, and det(B) does not appear in a similarity transform's diagonal.
//
// Jolt itself resolves the mirror case (an odd number of negative scale components) from the scale it
// is queried with -- ScaleHelpers::IsInsideOut, read inside MeshShape's own query code -- so nothing
// here needs to re-flip triangle winding the way scaleMeshForBody (LevelInstance.cpp) does for its
// baked path; that correction is Jolt's job once the scale itself is right.
int32_t aver_phys_add_mesh_shape_body(int32_t shape, float px, float py, float pz,
                                      float qx, float qy, float qz, float qw,
                                      float sx, float sy, float sz) {
    if (!g_world) return 0;
    JPH::Shape* base = meshShapeFor(shape);
    if (!base) return 0;

    // Shape::ScaleShape also covers the near-unit case (hands back `base` itself, no wrapper) and the
    // zero-scale refusal (a clean ShapeResult error, not an assert) -- both for free.
    const JPH::Shape::ShapeResult scaled = base->ScaleShape(JPH::Vec3(sy, sz, sx));
    if (scaled.HasError()) {
        AVER_WARN("[Physics] mesh shape body: {}", scaled.GetError().c_str());
        return 0;
    }

    JPH::BodyCreationSettings s(scaled.Get(), toJolt(Vec3(px, py, pz)), toJolt(Quat(qx, qy, qz, qw)),
                               JPH::EMotionType::Static, Layers::encode(0, false));
    JPH::Body* jbody = bi().CreateBody(s);
    if (!jbody) { warnBodyLimit(); return 0; }
    bi().AddBody(jbody->GetID(), JPH::EActivation::DontActivate);
    const int32_t h = g_world->nextHandle++;
    g_world->bodies.emplace(h, jbody->GetID());
    g_world->byId.emplace(jbody->GetID(), h);
    return h;
}

// Adds a static heightfield from a row-major sampleCount x sampleCount grid. Returns its handle, or 0.
int32_t aver_phys_add_heightfield(const float* samples, int32_t sampleCount, float spacingCm,
                                  float cx, float cy, float cz) {
    if (!g_world || !samples || sampleCount < 2 || spacingCm <= 0.0f) return 0;
    // Every sample is passed through: Jolt rounds the count UP to a block multiple and pads the
    // remainder itself with no-collision.
    const int32_t n = sampleCount;

    JPH::Array<float> heights;
    heights.reserve(static_cast<size_t>(n) * static_cast<size_t>(n));
    for (int32_t y = 0; y < n; ++y)
        for (int32_t x = 0; x < n; ++x)
            heights.push_back(cmToM(samples[static_cast<size_t>(y) * sampleCount + x]));

    // Built directly in Jolt's Y-up axes rather than through the axis map.
    JPH::HeightFieldShapeSettings s(heights.data(), JPH::Vec3::sZero(),
                                    JPH::Vec3(cmToM(spacingCm), 1.0f, cmToM(spacingCm)),
                                    static_cast<JPH::uint32>(n));
    s.SetEmbedded();
    auto res = s.Create();
    if (res.HasError()) { AVER_WARN("[Physics] heightfield: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f);
}

// ---- Sensors --------------------------------------------------------------------------------------

// Adds a box-shaped trigger volume. Returns its handle, or 0.
int32_t aver_phys_add_sensor_box(float cx, float cy, float cz, float hx, float hy, float hz) {
    if (!g_world) return 0;
    // Half-extents are a SIZE: the axis map applies but the sign flip does not.
    const Vec3 he = fromJoltUnit(toJoltUnit(Vec3(hx, hy, hz)));
    JPH::BoxShapeSettings box(JPH::Vec3(std::abs(cmToM(he.y)), std::abs(cmToM(he.z)), std::abs(cmToM(he.x))));
    box.SetEmbedded();
    auto res = box.Create();
    if (res.HasError()) { AVER_WARN("[Physics] sensor box: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f, /*sensor*/true);
}

// Adds a sphere-shaped trigger volume. Returns its handle, or 0.
int32_t aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius) {
    if (!g_world || radius <= 0.0f) return 0;
    JPH::SphereShapeSettings sph(cmToM(radius));
    sph.SetEmbedded();
    auto res = sph.Create();
    if (res.HasError()) { AVER_WARN("[Physics] sensor sphere: {}", res.GetError().c_str()); return 0; }
    return addBody(res.Get(), Vec3(cx, cy, cz), /*dynamic*/false, 0.0f, /*sensor*/true);
}

// ---- Event queues ---------------------------------------------------------------------------------

// How many contacts began this step.
int32_t aver_phys_contact_count(void) {
    if (!g_world) return 0;
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    return static_cast<int32_t>(g_world->contacts.size());
}

// Reads one contact. 0 for an out-of-range index.
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

// How many sensor overlaps started or stopped this step.
int32_t aver_phys_overlap_count(void) {
    if (!g_world) return 0;
    std::lock_guard<std::mutex> lock(g_world->eventMutex);
    return static_cast<int32_t>(g_world->overlaps.size());
}

// Reads one sensor overlap. 0 for an out-of-range index.
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

// Writes the handles of every body overlapping a sphere. Returns how many were written.
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
        if (n >= maxBodies) break;   // truncated
        const auto it = g_world->byId.find(hit.mBodyID2);
        if (it != g_world->byId.end()) outBodies[n++] = it->second;
    }
    return n;
}

// Sweeps a sphere and returns the first body hit, or 0 for a clear sweep.
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

// ---- Soft bodies ------------------------------------------------------------------------------------

namespace {

// The soft-body motion properties behind a handle, or nullptr for anything that is not one.
//
// NO SIDE TABLE: Jolt already knows whether a body is soft, so asking it is one branch and cannot
// fall out of step with the handle map the way a parallel "which handles are soft" set would.
JPH::SoftBodyMotionProperties* findSoftBody(int32_t h) {
    const JPH::BodyID* id = findBody(h);
    if (!id) return nullptr;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return nullptr;
    JPH::Body& body = lock.GetBody();
    if (!body.IsSoftBody()) return nullptr;
    return static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
}

// Fills the shared settings both create paths need: particles, faces, and the edge constraints
// generated from them.
bool buildSoftShared(JPH::SoftBodySharedSettings& settings,
                     const float* verticesXyz, int32_t vertexCount,
                     const int32_t* indices, int32_t indexCount,
                     const float* invMasses, float compliance) {
    if (!verticesXyz || vertexCount <= 0 || !indices || indexCount < 3) return false;

    settings.mVertices.reserve(static_cast<size_t>(vertexCount));
    for (int32_t i = 0; i < vertexCount; ++i) {
        const Vec3 p(verticesXyz[i * 3 + 0], verticesXyz[i * 3 + 1], verticesXyz[i * 3 + 2]);
        const JPH::Vec3 jp = toJolt(p);
        JPH::SoftBodySharedSettings::Vertex v;
        v.mPosition = JPH::Float3(jp.GetX(), jp.GetY(), jp.GetZ());
        // INVERSE mass, so 0 means infinitely heavy -- which is how a particle is pinned. A mass
        // here instead would make 0 mean weightless, the exact opposite, so the name of this
        // parameter is load-bearing all the way out to the ABI.
        v.mInvMass = invMasses ? invMasses[i] : 1.0f;
        settings.mVertices.push_back(v);
    }

    for (int32_t t = 0; t + 2 < indexCount; t += 3) {
        const int32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a < 0 || b < 0 || c < 0 || a >= vertexCount || b >= vertexCount || c >= vertexCount)
            continue;
        if (a == b || b == c || a == c) continue;   // Jolt asserts on a degenerate face
        // WOUND STRAIGHT THROUGH, and the swap that used to be here was a double negative.
        //
        // Two sign flips are in play and they cancel. Aver's winding convention is that (C-A)x(B-A)
        // is the OUTWARD normal (see ChunkMesh.cpp, where the landscape's own convention is set) --
        // the opposite hand from the right-handed CCW convention Jolt's volume integral assumes, so
        // an engine mesh reads inside-out before conversion. Then toJolt maps (x,y,z) -> (y,z,-x),
        // whose determinant is -1: it is a mirror, and it flips every triangle's facing by itself.
        // Mirror plus opposite convention is already correct. Swapping two indices on top of that
        // put it back inside-out.
        //
        // WHAT THAT COST, because "the pressure constant is too small" is what it looks like from
        // outside: Jolt's ApplyPressure opens with `if (six_volume > 0.0f)`, so an inside-out shell
        // gets no pressure AT ALL and deflates under its own weight regardless of the coefficient.
        // Measured on a 6x4x1.2 m fluid shell: +172.8 with the indices as-is, -172.8 with them
        // swapped, against a true |6V| of 172.8. Raising the coefficient by a factor of 375,000
        // changed the rendered result by 1317 pixels out of 7 million -- which is what "the code
        // never runs" looks like when it is mistaken for "the number is wrong".
        //
        // Nothing regressed by removing the swap: the only other caller (render.softbody) passes
        // pressure 0, so this had never been exercised, and the comment that used to sit here --
        // "without it a pressurised body inflates INWARDS" -- described a case no code had run.
        settings.AddFace(JPH::SoftBodySharedSettings::Face(static_cast<JPH::uint32>(a),
                                                           static_cast<JPH::uint32>(b),
                                                           static_cast<JPH::uint32>(c)));
    }
    if (settings.mFaces.empty()) return false;

    JPH::SoftBodySharedSettings::VertexAttributes attr;
    attr.mCompliance = compliance < 0.0f ? 0.0f : compliance;
    attr.mShearCompliance = attr.mCompliance;
    attr.mBendCompliance = FLT_MAX;   // no bend constraints: cloth folds, and a solid has volume
    settings.CreateConstraints(&attr, 1);
    return true;
}

// Creates the body, registers the handle, returns it. `damping`/`iterations` are already clamped by
// the caller (aver_phys_softbody_create's own guard) -- this function just assigns them, the same
// division of labour `pressure` already has: this is the one place both create paths converge, and
// aver_phys_softbody_create_skinned's own caller passes Jolt's un-named defaults straight through
// rather than clamping a literal that is already known valid.
int32_t addSoftBody(JPH::SoftBodySharedSettings* shared, const Vec3& centreCm, float pressure,
                    float damping, int32_t iterations) {
    JPH::SoftBodyCreationSettings s(shared, toJolt(centreCm), JPH::Quat::sIdentity(), Layers::MOVING);
    s.mPressure = pressure;
    s.mLinearDamping = damping;
    s.mNumIterations = static_cast<JPH::uint32>(iterations);
    // The body origin stays put and the PARTICLES are what move. With this on, Jolt recentres the
    // body on its particles every step, which would make aver_phys_body_position report a moving
    // target for something the caller never moved.
    s.mUpdatePosition = false;
    JPH::Body* body = g_world->system.GetBodyInterface().CreateSoftBody(s);
    if (!body) { AVER_WARN("[Physics] soft body limit reached"); return 0; }
    g_world->system.GetBodyInterface().AddBody(body->GetID(), JPH::EActivation::Activate);
    const int32_t h = g_world->nextHandle++;
    g_world->bodies.emplace(h, body->GetID());
    g_world->byId.emplace(body->GetID(), h);
    return h;
}

} // namespace

extern "C" {

int32_t aver_phys_softbody_create(const float* verticesXyz, int32_t vertexCount,
                                  const int32_t* indices, int32_t indexCount,
                                  const float* invMasses,
                                  float cx, float cy, float cz,
                                  float compliance, float pressure,
                                  float damping, int32_t iterations) {
    if (!g_world) return 0;
    // Ref-counted and owned by the shape from here on: Jolt keeps it alive as long as the body
    // needs it, which is why this is a Ref and not a local that goes out of scope.
    JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
    if (!buildSoftShared(*shared, verticesXyz, vertexCount, indices, indexCount, invMasses, compliance))
        return 0;
    shared->Optimize();
    // Clamped here, not inside addSoftBody: this is the ABI boundary, the one place an author's own
    // number (however it was authored -- a level file, a graph, a hand-written call) first reaches
    // this translation unit, and the physics_abi.h doc comment on this function is the contract that
    // promises the clamp happens. See that comment for why 0 iterations specifically cannot be let
    // through: Jolt divides the step by it.
    const float clampedDamping = damping < 0.0f ? 0.0f : damping;
    const int32_t clampedIterations = iterations < 1 ? 1 : iterations;
    return addSoftBody(shared, Vec3(cx, cy, cz), pressure, clampedDamping, clampedIterations);
}

int32_t aver_phys_softbody_create_skinned(const float* verticesXyz, int32_t vertexCount,
                                          const int32_t* indices, int32_t indexCount,
                                          const float* invMasses,
                                          const int32_t* jointIndices, const float* jointWeights,
                                          int32_t influences, int32_t jointCount,
                                          float maxDistanceCm, float backStopDistanceCm,
                                          float cx, float cy, float cz, float compliance) {
    if (!g_world) return 0;
    if (!jointIndices || !jointWeights || influences <= 0 || jointCount <= 0) return 0;

    JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
    if (!buildSoftShared(*shared, verticesXyz, vertexCount, indices, indexCount, invMasses, compliance))
        return 0;

    // ONE INVERSE BIND PER JOINT, ALL IDENTITY, and that is the trick that makes this cheap.
    // Jolt computes `jointMatrices[invBind.mJointIndex] * invBind.mInvBind`, and Aver's
    // anim::poseToSkinning ALREADY produces model x inverse-bind composed. Handing that palette in
    // against identity inverse-binds reproduces Aver's own skinning exactly, with no second
    // definition of the bind pose to drift out of step with the renderer's.
    shared->mInvBindMatrices.reserve(static_cast<size_t>(jointCount));
    for (int32_t j = 0; j < jointCount; ++j)
        shared->mInvBindMatrices.push_back(
            JPH::SoftBodySharedSettings::InvBind(static_cast<JPH::uint32>(j), JPH::Mat44::sIdentity()));

    const float maxDist = cmToM(maxDistanceCm < 0.0f ? 0.0f : maxDistanceCm);
    const float backStop = backStopDistanceCm < 0.0f ? FLT_MAX : cmToM(backStopDistanceCm);
    shared->mSkinnedConstraints.reserve(static_cast<size_t>(vertexCount));
    for (int32_t i = 0; i < vertexCount; ++i) {
        JPH::SoftBodySharedSettings::Skinned sk(static_cast<JPH::uint32>(i), maxDist, backStop, 0.0f);
        int slot = 0;
        for (int32_t k = 0; k < influences && slot < 4; ++k) {
            const int32_t joint = jointIndices[i * influences + k];
            const float w = jointWeights[i * influences + k];
            // A ZERO WEIGHT TERMINATES Jolt's per-vertex list (see SkinVertices), so a zero in the
            // middle would silently discard every influence after it. Compacting them out here is
            // what the mesh actually meant.
            if (w <= 0.0f || joint < 0 || joint >= jointCount) continue;
            sk.mWeights[slot++] = JPH::SoftBodySharedSettings::SkinWeight(
                static_cast<JPH::uint32>(joint), w);
        }
        if (slot == 0) continue;   // an unweighted vertex is left free rather than pinned to joint 0
        shared->mSkinnedConstraints.push_back(sk);
    }
    shared->CalculateSkinnedConstraintNormals();
    shared->Optimize();

    // Pressure is deliberately not offered here: a skinned body's shape is governed by the skeleton
    // it hangs off, and inflating it as well fights that. damping/iterations are new PARAMETERS on
    // addSoftBody as of the fluids solver-knobs change, but a skinned soft body (jiggle physics on a
    // skeletal mesh) is out of that change's scope -- passed here as Jolt's own
    // SoftBodyCreationSettings defaults (0.1f, 5), exactly what this call already got before either
    // field existed, so this path's behaviour is unchanged.
    return addSoftBody(shared, Vec3(cx, cy, cz), 0.0f, 0.1f, 5);
}

int32_t aver_phys_softbody_skin(int32_t body, const float* jointMatrices, int32_t jointCount,
                                int32_t hardSkin) {
    if (!g_world || !jointMatrices || jointCount <= 0) return 0;
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return 0;
    JPH::Body& b = lock.GetBody();
    if (!b.IsSoftBody()) return 0;
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());
    // Jolt asserts inside SkinVertices when there is nothing skinned, so an unskinned soft body is
    // refused here rather than being allowed to trip an assert in a release build's absence.
    if (!mp->GetSettings() || mp->GetSettings()->mSkinnedConstraints.empty()) return 0;

    std::vector<JPH::Mat44> palette(static_cast<size_t>(jointCount));
    for (int32_t j = 0; j < jointCount; ++j) {
        Mat4 m;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m.m[r][c] = jointMatrices[j * 16 + r * 4 + c];
        palette[static_cast<size_t>(j)] = toJolt(m);
    }
    mp->SkinVertices(b.GetCenterOfMassTransform(), palette.data(),
                     static_cast<JPH::uint>(jointCount), hardSkin != 0, *g_world->temp);
    return 1;
}

int32_t aver_phys_softbody_vertex_count(int32_t body) {
    const JPH::SoftBodyMotionProperties* mp = findSoftBody(body);
    return mp ? static_cast<int32_t>(mp->GetVertices().size()) : 0;
}

int32_t aver_phys_softbody_vertices(int32_t body, float* outXyz, int32_t maxVertices) {
    if (!outXyz || maxVertices <= 0) return 0;
    if (!g_world) return 0;
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return 0;
    const JPH::Body& b = lock.GetBody();
    if (!b.IsSoftBody()) return 0;
    const auto* mp = static_cast<const JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());

    // Particle positions are stored RELATIVE TO THE CENTRE OF MASS, so they have to be lifted into
    // world space before they mean anything to a caller. Returning them raw would give a mesh that
    // renders correctly only while the body sits at the origin -- exactly the kind of defect that
    // looks perfect in a test scene and is wrong everywhere else.
    const JPH::RMat44 com = b.GetCenterOfMassTransform();
    const auto& verts = mp->GetVertices();
    const int32_t n = std::min(maxVertices, static_cast<int32_t>(verts.size()));
    for (int32_t i = 0; i < n; ++i) {
        const JPH::Vec3 world = JPH::Vec3(com * verts[static_cast<size_t>(i)].mPosition);
        writeVec(outXyz + i * 3, fromJolt(world));
    }
    return n;
}

int32_t aver_phys_softbody_apply_impulse(int32_t body, const float* centreCm, float radiusCm,
                                         const float* velocityCmPerS, float strength) {
    if (!g_world || !centreCm || !velocityCmPerS || radiusCm <= 0.0f) return 0;
    const JPH::BodyID* id = findBody(body);
    if (!id) return 0;
    // WRITE lock, not read: this walks GetVertices() and mutates mVelocity in place, the same access
    // level aver_phys_softbody_skin already takes for the same reason.
    JPH::BodyLockWrite lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return 0;
    JPH::Body& b = lock.GetBody();
    if (!b.IsSoftBody()) return 0;
    auto* mp = static_cast<JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());

    // A vertex's mPosition/mVelocity are stored RELATIVE TO THE CENTRE OF MASS (same fact
    // aver_phys_softbody_vertices above is built around) -- so the query sphere and target velocity,
    // both handed in as world-space engine units, have to cross into that same local frame before
    // they can be compared against or written into a vertex, rather than converting every vertex out
    // to world space and back (this body can hold thousands of particles; the transform below is paid
    // once, not per vertex).
    const JPH::RMat44 com = b.GetCenterOfMassTransform();
    const JPH::RMat44 invCom = com.InversedRotationTranslation();
    const JPH::Vec3 worldCentre = toJolt(Vec3(centreCm[0], centreCm[1], centreCm[2]));
    const JPH::Vec3 localCentre = JPH::Vec3(invCom * worldCentre);
    // Velocity is a FREE VECTOR, not a point -- it takes the rotation only, never the translation.
    // Multiply3x3Transposed is the world-to-local half of that rotation: GetRotation() is orthonormal,
    // so its transpose is its inverse, and Jolt spells that operation as "Transposed" rather than
    // "Inversed" to say so.
    const JPH::Vec3 worldVel = toJoltDir(Vec3(velocityCmPerS[0], velocityCmPerS[1], velocityCmPerS[2]));
    const JPH::Vec3 localVel = com.GetRotation().Multiply3x3Transposed(worldVel);

    const float radiusM = cmToM(radiusCm);
    const float radiusSqM = radiusM * radiusM;
    const float s = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);

    int32_t nudged = 0;
    for (auto& v : mp->GetVertices()) {
        if ((v.mPosition - localCentre).LengthSq() > radiusSqM) continue;
        // A BLEND, not `v.mVelocity += localVel`: see the ABI header for why this has to be bounded
        // under repeated per-frame calls rather than accumulating.
        v.mVelocity += (localVel - v.mVelocity) * s;
        ++nudged;
    }

    // WAKING IS PART OF PUSHING, and leaving it out made this function silently do nothing to
    // exactly the fluids it was written for. Writing mVelocity is a poke at memory, not a physics
    // event: Jolt never sees it, so a body it has already put to sleep stays asleep with its new
    // velocities sitting unread, and the caller gets a positive `nudged` count telling them it
    // worked.
    //
    // WHY THIS SURFACED ONLY WITH REAL DENSITY. Every fluid used to have unit particle mass and the
    // default damping, which together kept a settled pool jittering just above Jolt's sleep
    // threshold, so the bug could not fire. Real density-scaled mass plus the damping the Honey and
    // Lava presets map to settles a volume genuinely still -- it sleeps a few seconds after spawn,
    // and from then on the player could walk through it and nothing would move. Two of the four
    // shipped presets, broken in the one way that matters, and no test above this layer caught it
    // because they all disturb a body that never had time to fall asleep.
    //
    // Activated unconditionally rather than only when something was nudged: a sphere that currently
    // overlaps no vertex may well overlap one a few milliseconds later, and a sleeping body's
    // vertices never move, so declining to wake it here is what would keep it asleep forever.
    //
    // THE NO-LOCK INTERFACE, AND THIS IS NOT AN OPTIMISATION. BodyInterface::ActivateBody takes a
    // BodyLockWrite on the body it is activating (BodyInterface.cpp:213-221) -- and this function is
    // still inside its own BodyLockWrite on that same body, taken at the top to mutate the vertices.
    // Going through the locking interface here would have the thread wait on a per-body mutex it
    // already holds. GetBodyInterfaceNoLock is Jolt's documented answer for exactly this position:
    // "use with great care", meaning use it when you have already established the lock yourself,
    // which is the case here and only here.
    g_world->system.GetBodyInterfaceNoLock().ActivateBody(*id);
    return nudged;
}

} // extern "C"
