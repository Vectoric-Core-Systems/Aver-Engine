// PhysicsKinematicCharacter.cpp -- Godot's CharacterBody3D.move_and_slide as a controller on a kinematic rigid body (G2). The ABI is
// physics_kinematic_abi.h; the rules, the order of the steps and the places this differs from Godot are in docs/physics/GODOT_KINEMATIC.md.
// Written from Godot's documented behaviour. Every move is aver_phys_body_test_motion (recovery, cast, contacts) followed by a teleport of the
// body, so all filtering (layers, masks, exceptions) is the body's own. The controller table is configuration and results, not simulation state.
#include "aver/physics/physics_kinematic_abi.h"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_queries_abi.h"
#include "aver/physics/physics_shape_builder_abi.h"
#include "Convert.hpp"
#include "PhysicsGodotFilter.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr float kFloorAngleThreshold = 0.01f;   // radians added to every floor / ceiling angle test, as Godot does
constexpr float kEpsCm = 1e-3f;                 // length comparisons (Godot's CMP_EPSILON is 1e-5 m)
constexpr int   kMaxHits = 6;                   // 4 contacts between two walls plus 2 for the floor

struct V3 { float x = 0, y = 0, z = 0; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float lenSq(V3 a) { return dot(a, a); }
float len(V3 a) { return std::sqrt(lenSq(a)); }
V3 norm(V3 a) { const float l = len(a); return l > 1e-12f ? a * (1.0f / l) : V3{}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 slideAlong(V3 v, V3 n) { return v - n * dot(v, n); }     // the part of v perpendicular to the unit n
V3 projectOn(V3 v, V3 n) { return n * dot(v, n); }
bool zeroApprox(V3 a) { return lenSq(a) < 1e-6f; }
float angleBetween(V3 a, V3 b) { return std::acos(std::clamp(dot(a, b), -1.0f, 1.0f)); }   // unit vectors
V3 fromArr(const float* p) { return {p[0], p[1], p[2]}; }
void toArr(float* p, V3 v) { p[0] = v.x; p[1] = v.y; p[2] = v.z; }

struct Collision {
    V3 point, normal, colliderVel, remainder, travel;
    float depth = 0;
    int32_t collider = 0, entity = 0, subshape = 0;
};

struct Kc {
    int32_t body = 0;
    int mode = AVER_PHYS_KCM_GROUNDED;
    int maxSlides = 6;
    float floorMaxAngle = 0.78539816f, wallMinSlideAngle = 0.26179939f, snapLength = 10.0f, margin = 0.1f;
    bool stopOnSlope = true, constantSpeed = false, blockOnWall = true, slideOnCeiling = true;
    int onLeave = AVER_PHYS_KCL_ADD_VELOCITY;
    uint32_t platformFloorLayers = 0xFFFFFFFFu, platformWallLayers = 0;
    V3 up{0, 0, 1}, velocity;
    // results
    uint32_t state = 0;
    V3 floorNormal, wallNormal, platformVel, platformCeilVel, realVel, lastMotion, posDelta;
    int32_t platformBody = 0;
    uint32_t platformLayer = 0;
    std::vector<Collision> collisions;
};

std::map<int32_t, Kc> g_kcs;
int32_t g_nextKc = 1;
uint64_t g_kcWorld = 0;

int32_t refuse(AbiError e) { setAbiError(e); return 0; }

// The controller, or null (and an error) for a dead handle, a missing world, or a body that has been removed. A new world drops the old table.
Kc* resolve(int32_t h) {
    if (!g_world) { setAbiError(AbiError::NotInitialised); return nullptr; }
    if (g_kcWorld != g_world->id) { g_kcs.clear(); g_nextKc = 1; g_kcWorld = g_world->id; }
    const auto it = g_kcs.find(h);
    if (it == g_kcs.end() || !findBody(it->second.body)) { setAbiError(AbiError::BadHandle); return nullptr; }
    return &it->second;
}

// ---- body access ---------------------------------------------------------------------------------------

V3 originOf(const Kc& k) {
    float o[3] = {0, 0, 0};
    aver_phys_body_origin(k.body, o);
    return fromArr(o);
}

void moveBy(Kc& k, V3 d) {
    if (lenSq(d) == 0.0f) return;
    const V3 o = originOf(k) + d;
    aver_phys_body_set_position(k.body, o.x, o.y, o.z);
}

// A platform's velocity at a point (Godot's get_velocity_at_local_position); zero for a static or dead body.
V3 platformVelocityAt(int32_t handle, V3 pointCm) {
    const JPH::BodyID* id = findBody(handle);
    if (!id) return {};
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    if (!lock.Succeeded()) return {};
    const JPH::Body& b = lock.GetBody();
    if (b.IsStatic() || b.IsSensor()) return {};
    const Vec3 v = fromJoltDir(b.GetPointVelocity(JPH::RVec3(toJolt(Vec3(pointCm.x, pointCm.y, pointCm.z)))));
    return {v.x, v.y, v.z};
}

uint32_t layerBitsOf(int32_t handle) {
    const JPH::BodyID* id = findBody(handle);
    if (!id) return 0;
    JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
    return lock.Succeeded() ? godotLayerBitsOf(lock.GetBody()) : 0u;
}

// ---- move_and_collide ------------------------------------------------------------------------------------

struct Move {
    V3 travel, remainder;
    float safe = 1, unsafe = 1;
    std::vector<Collision> hits;   // primary first, then deepest first
};

// Godot's move_and_collide: recovery, then the cast, then the body is moved by `travel` unless testOnly. cancelSliding drops the sideways part of
// a recovery that is within the margin, so a body resting on a slope is not nudged downhill by the push out of the surface.
bool moveAndCollide(Kc& k, V3 motion, bool testOnly, bool cancelSliding, bool recoveryAsCollision, int maxHits,
                    const int32_t* exclude, int excludeN, Move& r) {
    r = Move{};
    float f[AVER_PHYS_TM_F_COUNT];
    int32_t iv[AVER_PHYS_TM_I_COUNT];
    float cf[kMaxHits * AVER_PHYS_TC_F_COUNT];
    int32_t ci[kMaxHits * AVER_PHYS_TC_I_COUNT];
    const float m3[3] = {motion.x, motion.y, motion.z};
    const int32_t flags = AVER_PHYS_SQ_BODIES | (recoveryAsCollision ? AVER_PHYS_SQ_RECOVERY_AS_COLLISION : 0);
    const int32_t rc = aver_phys_body_test_motion(k.body, nullptr, m3, k.margin, flags, exclude, excludeN,
                                                  std::min(maxHits, kMaxHits), f, iv, cf, ci);
    if (rc < 0) return false;
    const bool collided = rc == 1;
    r.travel = fromArr(f + AVER_PHYS_TM_F_TRAVEL);
    r.remainder = fromArr(f + AVER_PHYS_TM_F_REMAINDER);
    r.safe = f[AVER_PHYS_TM_F_SAFE];
    r.unsafe = f[AVER_PHYS_TM_F_UNSAFE];
    if (collided) {
        const int n = std::min(iv[AVER_PHYS_TM_I_CONTACTS], kMaxHits);
        for (int i = 0; i < n; ++i) {
            const float* c = cf + i * AVER_PHYS_TC_F_COUNT;
            const int32_t* ic = ci + i * AVER_PHYS_TC_I_COUNT;
            Collision h;
            h.point = fromArr(c + AVER_PHYS_TC_F_POINT);
            h.normal = norm(fromArr(c + AVER_PHYS_TC_F_NORMAL));
            h.colliderVel = fromArr(c + AVER_PHYS_TC_F_VELOCITY);
            h.depth = c[AVER_PHYS_TC_F_DEPTH];
            h.collider = ic[AVER_PHYS_TC_I_BODY];
            h.entity = ic[AVER_PHYS_TC_I_ENTITY];
            h.subshape = ic[AVER_PHYS_TC_I_SUBSHAPE];
            r.hits.push_back(h);
        }
    }

    if (cancelSliding) {
        const float motionLen = len(motion);
        float precision = 0.1f;
        bool cancel = true;
        if (collided) {
            precision += motionLen * (r.unsafe - r.safe);
            if (f[AVER_PHYS_TM_F_DEPTH] > k.margin + precision) cancel = false;
        }
        if (cancel) {
            const V3 mn = motionLen > kEpsCm ? motion * (1.0f / motionLen) : V3{};
            const float projected = dot(r.travel, mn);
            const V3 recovery = r.travel - mn * projected;
            if (len(recovery) < k.margin + precision) {
                r.travel = mn * projected;
                r.remainder = motion - r.travel;
            }
        }
    }

    for (Collision& h : r.hits) { h.remainder = r.remainder; h.travel = r.travel; }
    if (!testOnly) moveBy(k, r.travel);
    return collided && !r.hits.empty();
}

// ---- classification --------------------------------------------------------------------------------------

void setPlatform(Kc& k, const Collision& h) {
    k.platformBody = h.collider;
    k.platformVel = h.colliderVel;
    k.platformLayer = layerBitsOf(h.collider);
}

bool isFloorNormal(const Kc& k, V3 n) { return angleBetween(n, k.up) <= k.floorMaxAngle + kFloorAngleThreshold; }
bool isCeilingNormal(const Kc& k, V3 n) { return angleBetween(n, -k.up) <= k.floorMaxAngle + kFloorAngleThreshold; }

// Godot's _set_collision_direction over every contact of a move. The platform is the floor's, else a wall's; a ceiling gives its velocity.
void setCollisionDirection(Kc& k, const Move& m) {
    const Collision* floorHit = nullptr;
    const Collision* wallHit = nullptr;
    for (const Collision& h : m.hits) {
        if (k.mode == AVER_PHYS_KCM_GROUNDED && isFloorNormal(k, h.normal)) {
            k.state |= AVER_PHYS_KCS_FLOOR;
            if (!floorHit) floorHit = &h;
        } else if (k.mode == AVER_PHYS_KCM_GROUNDED && isCeilingNormal(k, h.normal)) {
            k.state |= AVER_PHYS_KCS_CEILING;
            k.platformCeilVel = h.colliderVel;
        } else {
            k.state |= AVER_PHYS_KCS_WALL;
            if (!wallHit) wallHit = &h;
        }
    }
    if (floorHit) { k.floorNormal = floorHit->normal; setPlatform(k, *floorHit); }
    if (wallHit) {
        k.wallNormal = wallHit->normal;
        if (!floorHit) setPlatform(k, *wallHit);
    }
}

// ---- floor snap --------------------------------------------------------------------------------------------

void applyFloorSnap(Kc& k, bool wallAsFloor) {
    if (k.state & AVER_PHYS_KCS_FLOOR) return;
    const float length = std::max(k.snapLength, k.margin);
    Move r;
    if (!moveAndCollide(k, -k.up * length, true, false, true, 4, nullptr, 0, r)) return;
    const V3 n = r.hits.front().normal;
    if (isFloorNormal(k, n) || (wallAsFloor && !isCeilingNormal(k, n))) {
        k.state |= AVER_PHYS_KCS_FLOOR;
        k.floorNormal = n;
        setPlatform(k, r.hits.front());
        if (k.stopOnSlope) r.travel = len(r.travel) > k.margin ? projectOn(r.travel, k.up) : V3{};
        moveBy(k, r.travel);
    }
}

void snapOnFloor(Kc& k, bool wasOnFloor, bool velFacingUp, bool wallAsFloor) {
    if ((k.state & AVER_PHYS_KCS_FLOOR) || !wasOnFloor || velFacingUp) return;
    applyFloorSnap(k, wallAsFloor);
}

// ---- the two slide loops -------------------------------------------------------------------------------------

void slideGrounded(Kc& k, float dt, bool wasOnFloor) {
    V3 motion = k.velocity * dt;
    const V3 motionSlideUp = slideAlong(motion, k.up);
    const V3 prevFloorNormal = k.floorNormal;
    k.platformBody = 0; k.platformLayer = 0; k.platformVel = {}; k.platformCeilVel = {}; k.floorNormal = {};

    bool slidingEnabled = !k.stopOnSlope;
    bool canApplyConstantSpeed = slidingEnabled;
    const bool velFacingUp = dot(k.velocity, k.up) > 0.0f;
    V3 lastTravel;

    for (int iteration = 0; iteration < k.maxSlides; ++iteration) {
        Move r;
        const bool collided = moveAndCollide(k, motion, false, !slidingEnabled, true, kMaxHits, nullptr, 0, r);
        k.lastMotion = r.travel;

        if (collided) {
            k.collisions.push_back(r.hits.front());
            setCollisionDirection(k, r);
            const V3 n = r.hits.front().normal;

            // A ceiling platform pushing down gives the body at least its downward velocity.
            if ((k.state & AVER_PHYS_KCS_CEILING) && !zeroApprox(k.platformCeilVel) && dot(k.platformCeilVel, k.up) < 0.0f) {
                if (!k.slideOnCeiling || dot(motion, k.up) < 0.0f || len(n + k.up) < 0.01f) {
                    const V3 ceilV = projectOn(k.platformCeilVel, k.up);
                    const V3 motV = projectOn(k.velocity, k.up);
                    if (dot(motV, k.up) > 0.0f || lenSq(ceilV) > lenSq(motV)) k.velocity = ceilV + slideAlong(k.velocity, k.up);
                }
            }

            // Standing on a slope with only gravity pulling: undo the sliding that this step would add.
            if ((k.state & AVER_PHYS_KCS_FLOOR) && k.stopOnSlope && len(norm(k.velocity) + k.up) < 0.01f) {
                if (len(r.travel) <= k.margin + kEpsCm) moveBy(k, -r.travel);
                k.velocity = {};
                motion = {};
                k.lastMotion = {};
                break;
            }

            if (zeroApprox(r.remainder)) { motion = {}; break; }

            bool defaultSliding = true;
            if (k.blockOnWall && (k.state & AVER_PHYS_KCS_WALL) && dot(motionSlideUp, k.wallNormal) <= 0.0f) {
                // A body on the floor does not move forward into a wall (a steep surface); only the lateral motion goes on.
                defaultSliding = false;
                if (wasOnFloor && !velFacingUp) {
                    const float travelTotal = len(r.travel);
                    const float cancelMax = std::min(10.0f, k.margin * 20.0f);
                    if (travelTotal <= k.margin + kEpsCm) {
                        moveBy(k, -r.travel);
                        r.travel = {};
                    } else if (travelTotal < cancelMax) {
                        moveBy(k, -slideAlong(r.travel, k.up));
                        motion = slideAlong(motion, k.up);
                        r.travel = {};
                    } else {
                        r.travel = slideAlong(r.travel, k.up);
                        motion = r.remainder;
                    }
                    snapOnFloor(k, true, false, true);
                } else {
                    motion = r.remainder;
                }
                const V3 forward = norm(slideAlong(k.wallNormal, k.up));
                motion = slideAlong(motion, forward);
                // Keep the direction along the old floor when walking diagonally into an inclined wall.
                if (wasOnFloor && !velFacingUp && dot(k.velocity, n) > 0.0f && lenSq(prevFloorNormal) > 0.0f) {
                    const float motionLength = len(motion);
                    motion = norm(cross(cross(k.up, motion), prevFloorNormal)) * motionLength;
                }
                k.velocity = slideAlong(k.velocity, forward);
            } else if (!slidingEnabled) {
                // First attempt with stop-on-slope: only reach the surface, so the next attempt sees the motion head-on and can keep its speed.
                motion = r.remainder;
                defaultSliding = false;
            } else if (k.constantSpeed && (k.state & 7u) == AVER_PHYS_KCS_FLOOR && canApplyConstantSpeed && wasOnFloor && dot(motion, n) < 0.0f) {
                // The same speed along the slope as on the flat.
                canApplyConstantSpeed = false;
                const V3 dir = norm(slideAlong(r.remainder, n));
                const float want = len(motionSlideUp) - len(slideAlong(r.travel, k.up)) - len(slideAlong(lastTravel, k.up));
                motion = dir * std::max(want, 0.0f);
                defaultSliding = false;
            }

            if (defaultSliding) {
                const V3 oldVelocity = k.velocity;
                motion = slideAlong(r.remainder, n);
                if ((k.state & AVER_PHYS_KCS_CEILING) && !k.slideOnCeiling && velFacingUp) {
                    k.velocity = slideAlong(k.velocity, k.up);
                    motion = slideAlong(motion, k.up);
                } else {
                    k.velocity = slideAlong(k.velocity, n);
                }
                if (dot(motion, oldVelocity) <= 0.0f) motion = {};
            }
        }

        canApplyConstantSpeed = !canApplyConstantSpeed && !slidingEnabled;
        slidingEnabled = true;
        lastTravel = r.travel;
        if (!collided || zeroApprox(motion)) break;
    }

    snapOnFloor(k, wasOnFloor, velFacingUp, false);

    // Leaning on a wall only: the horizontal velocity follows the wall's slope.
    if ((k.state & 7u) == AVER_PHYS_KCS_WALL && !k.collisions.empty() && dot(motionSlideUp, k.collisions.front().normal) < 0.0f) {
        const V3 slideMotion = slideAlong(k.velocity, k.collisions.front().normal);
        if (dot(motionSlideUp, slideMotion) < 0.0f) k.velocity = projectOn(k.velocity, k.up);
        else k.velocity = projectOn(k.velocity, k.up) + slideAlong(slideMotion, k.up);
    }
    // Gravity does not accumulate on the ground.
    if ((k.state & AVER_PHYS_KCS_FLOOR) && !velFacingUp) k.velocity = slideAlong(k.velocity, k.up);
}

void slideFloating(Kc& k, float dt) {
    V3 motion = k.velocity * dt;
    k.platformBody = 0; k.platformLayer = 0; k.platformVel = {}; k.platformCeilVel = {}; k.floorNormal = {};
    bool firstSlide = true;
    for (int iteration = 0; iteration < k.maxSlides; ++iteration) {
        Move r;
        const bool collided = moveAndCollide(k, motion, false, false, true, kMaxHits, nullptr, 0, r);
        k.lastMotion = r.travel;
        if (collided) {
            k.collisions.push_back(r.hits.front());
            setCollisionDirection(k, r);
            if (zeroApprox(r.remainder)) { motion = {}; break; }
            const V3 n = r.hits.front().normal;
            if (k.wallMinSlideAngle != 0.0f && angleBetween(n, -norm(k.velocity)) < k.wallMinSlideAngle + kFloorAngleThreshold) {
                motion = {};
                if (len(r.travel) < k.margin + kEpsCm) moveBy(k, -r.travel);
            } else if (firstSlide) {
                motion = norm(slideAlong(r.remainder, n)) * (len(motion) - len(r.travel));
            } else {
                motion = slideAlong(r.remainder, n);
            }
            if (dot(motion, k.velocity) <= 0.0f) motion = {};
        }
        if (!collided || zeroApprox(motion)) break;
        firstSlide = false;
    }
}

} // namespace

extern "C" {

int32_t aver_phys_kinematic_abi_version(void) { return AVER_PHYS_KINEMATIC_ABI_VERSION; }

int32_t aver_phys_kc_create(int32_t body) {
    if (!g_world) return refuse(AbiError::NotInitialised);
    if (g_kcWorld != g_world->id) { g_kcs.clear(); g_nextKc = 1; g_kcWorld = g_world->id; }
    const JPH::BodyID* id = findBody(body);
    if (!id) return refuse(AbiError::BadHandle);
    if (aver_phys_body_motion_type(body) != AVER_PHYS_MOTION_KINEMATIC) {
        AVER_WARN("[Physics] kinematic character: body {} is not kinematic", body);
        return refuse(AbiError::Unsupported);
    }
    {
        JPH::BodyLockRead lock(g_world->system.GetBodyLockInterface(), *id);
        if (!lock.Succeeded()) return refuse(AbiError::BadHandle);
        const JPH::EShapeType t = lock.GetBody().GetShape()->GetType();
        if (lock.GetBody().IsSoftBody() || t == JPH::EShapeType::Mesh || t == JPH::EShapeType::HeightField) {
            AVER_WARN("[Physics] kinematic character: body {} has a soft, mesh or heightfield shape", body);
            return refuse(AbiError::Unsupported);
        }
    }
    for (const auto& [h, c] : g_kcs)
        if (c.body == body) { (void)h; return refuse(AbiError::InvalidArgument); }
    const int32_t h = g_nextKc++;
    g_kcs[h].body = body;
    return h;
}

int32_t aver_phys_kc_destroy(int32_t kc) {
    if (!g_world || g_kcWorld != g_world->id) return refuse(AbiError::BadHandle);
    return g_kcs.erase(kc) ? 1 : refuse(AbiError::BadHandle);
}

int32_t aver_phys_kc_exists(int32_t kc) {
    if (!g_world || g_kcWorld != g_world->id) return 0;
    const auto it = g_kcs.find(kc);
    return it != g_kcs.end() && findBody(it->second.body) ? 1 : 0;
}

int32_t aver_phys_kc_count(void) { return g_world && g_kcWorld == g_world->id ? static_cast<int32_t>(g_kcs.size()) : 0; }

int32_t aver_phys_kc_body(int32_t kc) {
    Kc* k = resolve(kc);
    return k ? k->body : 0;
}

int32_t aver_phys_kc_set_param(int32_t kc, int32_t param, float value) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (!std::isfinite(value)) return refuse(AbiError::InvalidArgument);
    const bool on = value != 0.0f;
    switch (param) {
    case AVER_PHYS_KCP_MOTION_MODE:
        if (value != 0.0f && value != 1.0f) return refuse(AbiError::InvalidArgument);
        k->mode = static_cast<int>(value);
        break;
    case AVER_PHYS_KCP_MAX_SLIDES:
        if (value < 1.0f || value > 64.0f) return refuse(AbiError::InvalidArgument);
        k->maxSlides = static_cast<int>(value);
        break;
    case AVER_PHYS_KCP_FLOOR_MAX_ANGLE:
        if (value < 0.0f || value > kPi) return refuse(AbiError::InvalidArgument);
        k->floorMaxAngle = value;
        break;
    case AVER_PHYS_KCP_WALL_MIN_SLIDE_ANGLE:
        if (value < 0.0f || value > kPi) return refuse(AbiError::InvalidArgument);
        k->wallMinSlideAngle = value;
        break;
    case AVER_PHYS_KCP_FLOOR_SNAP_LENGTH:
        if (value < 0.0f) return refuse(AbiError::InvalidArgument);
        k->snapLength = value;
        break;
    case AVER_PHYS_KCP_SAFE_MARGIN:
        if (value < 0.0f) return refuse(AbiError::InvalidArgument);
        k->margin = value;
        break;
    case AVER_PHYS_KCP_FLOOR_STOP_ON_SLOPE: k->stopOnSlope = on; break;
    case AVER_PHYS_KCP_FLOOR_CONSTANT_SPEED: k->constantSpeed = on; break;
    case AVER_PHYS_KCP_FLOOR_BLOCK_ON_WALL: k->blockOnWall = on; break;
    case AVER_PHYS_KCP_SLIDE_ON_CEILING: k->slideOnCeiling = on; break;
    case AVER_PHYS_KCP_PLATFORM_ON_LEAVE:
        if (value < 0.0f || value > 2.0f || value != std::floor(value)) return refuse(AbiError::InvalidArgument);
        k->onLeave = static_cast<int>(value);
        break;
    default: return refuse(AbiError::OutOfRange);
    }
    return 1;
}

int32_t aver_phys_kc_param(int32_t kc, int32_t param, float* outValue) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (!outValue) return refuse(AbiError::NullPointer);
    switch (param) {
    case AVER_PHYS_KCP_MOTION_MODE: *outValue = static_cast<float>(k->mode); break;
    case AVER_PHYS_KCP_MAX_SLIDES: *outValue = static_cast<float>(k->maxSlides); break;
    case AVER_PHYS_KCP_FLOOR_MAX_ANGLE: *outValue = k->floorMaxAngle; break;
    case AVER_PHYS_KCP_WALL_MIN_SLIDE_ANGLE: *outValue = k->wallMinSlideAngle; break;
    case AVER_PHYS_KCP_FLOOR_SNAP_LENGTH: *outValue = k->snapLength; break;
    case AVER_PHYS_KCP_SAFE_MARGIN: *outValue = k->margin; break;
    case AVER_PHYS_KCP_FLOOR_STOP_ON_SLOPE: *outValue = k->stopOnSlope ? 1.0f : 0.0f; break;
    case AVER_PHYS_KCP_FLOOR_CONSTANT_SPEED: *outValue = k->constantSpeed ? 1.0f : 0.0f; break;
    case AVER_PHYS_KCP_FLOOR_BLOCK_ON_WALL: *outValue = k->blockOnWall ? 1.0f : 0.0f; break;
    case AVER_PHYS_KCP_SLIDE_ON_CEILING: *outValue = k->slideOnCeiling ? 1.0f : 0.0f; break;
    case AVER_PHYS_KCP_PLATFORM_ON_LEAVE: *outValue = static_cast<float>(k->onLeave); break;
    default: return refuse(AbiError::OutOfRange);
    }
    return 1;
}

int32_t aver_phys_kc_set_up(int32_t kc, const float* up3) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (!up3) return refuse(AbiError::NullPointer);
    const V3 u = fromArr(up3);
    if (!std::isfinite(u.x) || !std::isfinite(u.y) || !std::isfinite(u.z) || len(u) < 1e-6f) return refuse(AbiError::InvalidArgument);
    k->up = norm(u);
    return 1;
}

int32_t aver_phys_kc_up(int32_t kc, float* outUp3) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (!outUp3) return refuse(AbiError::NullPointer);
    toArr(outUp3, k->up);
    return 1;
}

int32_t aver_phys_kc_set_platform_layers(int32_t kc, uint32_t floorLayers, uint32_t wallLayers) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    k->platformFloorLayers = floorLayers;
    k->platformWallLayers = wallLayers;
    return 1;
}

int32_t aver_phys_kc_platform_layers(int32_t kc, uint32_t* outFloorLayers, uint32_t* outWallLayers) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (outFloorLayers) *outFloorLayers = k->platformFloorLayers;
    if (outWallLayers) *outWallLayers = k->platformWallLayers;
    return 1;
}

int32_t aver_phys_kc_set_velocity(int32_t kc, const float* velocity3) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (!velocity3) return refuse(AbiError::NullPointer);
    const V3 v = fromArr(velocity3);
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return refuse(AbiError::InvalidArgument);
    k->velocity = v;
    return 1;
}

int32_t aver_phys_kc_velocity(int32_t kc, float* outVelocity3) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (!outVelocity3) return refuse(AbiError::NullPointer);
    toArr(outVelocity3, k->velocity);
    return 1;
}

int32_t aver_phys_kc_move_and_slide(int32_t kc, float dt, int32_t* outCollided) {
    if (outCollided) *outCollided = 0;
    Kc* kp = resolve(kc);
    if (!kp) return 0;
    if (!std::isfinite(dt) || dt <= 0.0f) return refuse(AbiError::InvalidArgument);
    Kc& k = *kp;

    // The platform that carried the body last time, and how fast it moves now where the body is.
    V3 platformVelocity = k.platformVel;
    const V3 prevPos = originOf(k);
    if ((k.state & (AVER_PHYS_KCS_FLOOR | AVER_PHYS_KCS_WALL)) && k.platformBody != 0) {
        const uint32_t layers = (k.state & AVER_PHYS_KCS_FLOOR) ? k.platformFloorLayers : k.platformWallLayers;
        if ((layers & k.platformLayer) != 0u) platformVelocity = platformVelocityAt(k.platformBody, prevPos);
        else platformVelocity = {};
    }

    k.collisions.clear();
    k.lastMotion = {};
    const bool wasOnFloor = (k.state & AVER_PHYS_KCS_FLOOR) != 0;
    const int32_t carrier = k.platformBody;
    k.state = 0;

    // Carried along first: the body moves with its platform, which itself is not a candidate for this move.
    if (!zeroApprox(platformVelocity) && carrier != 0) {
        Move r;
        if (moveAndCollide(k, platformVelocity * dt, false, false, false, kMaxHits, &carrier, 1, r)) {
            k.collisions.push_back(r.hits.front());
            setCollisionDirection(k, r);
        }
    }

    if (k.mode == AVER_PHYS_KCM_GROUNDED) slideGrounded(k, dt, wasOnFloor);
    else slideFloating(k, dt);

    k.posDelta = originOf(k) - prevPos;
    k.realVel = k.posDelta * (1.0f / dt);

    // Leaving a moving platform in the air: its velocity goes to the body.
    if (k.onLeave != AVER_PHYS_KCL_DO_NOTHING && !(k.state & (AVER_PHYS_KCS_FLOOR | AVER_PHYS_KCS_WALL))) {
        if (k.onLeave == AVER_PHYS_KCL_ADD_UPWARD_VELOCITY && dot(platformVelocity, k.up) < 0.0f) platformVelocity = slideAlong(platformVelocity, k.up);
        k.velocity = k.velocity + platformVelocity;
    }
    if (outCollided) *outCollided = k.collisions.empty() ? 0 : 1;
    return 1;
}

int32_t aver_phys_kc_clear(int32_t kc) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    k->state = 0;
    k->floorNormal = k->wallNormal = k->platformVel = k->platformCeilVel = k->realVel = k->lastMotion = k->posDelta = V3{};
    k->platformBody = 0;
    k->platformLayer = 0;
    k->collisions.clear();
    return 1;
}

int32_t aver_phys_kc_state(int32_t kc, int32_t* outFlags) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (!outFlags) return refuse(AbiError::NullPointer);
    *outFlags = static_cast<int32_t>(k->state);
    return 1;
}

int32_t aver_phys_kc_info(int32_t kc, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (outF) {
        float f[AVER_PHYS_KCI_F_COUNT];
        toArr(f + AVER_PHYS_KCI_F_FLOOR_NORMAL, (k->state & AVER_PHYS_KCS_FLOOR) ? k->floorNormal : V3{});
        toArr(f + AVER_PHYS_KCI_F_WALL_NORMAL, (k->state & AVER_PHYS_KCS_WALL) ? k->wallNormal : V3{});
        toArr(f + AVER_PHYS_KCI_F_PLATFORM_VELOCITY, k->platformVel);
        toArr(f + AVER_PHYS_KCI_F_REAL_VELOCITY, k->realVel);
        toArr(f + AVER_PHYS_KCI_F_LAST_MOTION, k->lastMotion);
        toArr(f + AVER_PHYS_KCI_F_POSITION_DELTA, k->posDelta);
        for (int32_t i = 0; i < std::min(nF, static_cast<int32_t>(AVER_PHYS_KCI_F_COUNT)); ++i) outF[i] = f[i];
    }
    if (outI) {
        const int32_t v[AVER_PHYS_KCI_I_COUNT] = {static_cast<int32_t>(k->state), k->platformBody, static_cast<int32_t>(k->collisions.size())};
        for (int32_t i = 0; i < std::min(nI, static_cast<int32_t>(AVER_PHYS_KCI_I_COUNT)); ++i) outI[i] = v[i];
    }
    return 1;
}

int32_t aver_phys_kc_collision(int32_t kc, int32_t index, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    Kc* k = resolve(kc);
    if (!k) return 0;
    if (index < 0 || index >= static_cast<int32_t>(k->collisions.size())) return refuse(AbiError::OutOfRange);
    const Collision& c = k->collisions[static_cast<size_t>(index)];
    if (outF) {
        float f[AVER_PHYS_KCC_F_COUNT];
        toArr(f + AVER_PHYS_KCC_F_POINT, c.point);
        toArr(f + AVER_PHYS_KCC_F_NORMAL, c.normal);
        toArr(f + AVER_PHYS_KCC_F_COLLIDER_VELOCITY, c.colliderVel);
        toArr(f + AVER_PHYS_KCC_F_REMAINDER, c.remainder);
        toArr(f + AVER_PHYS_KCC_F_TRAVEL, c.travel);
        f[AVER_PHYS_KCC_F_DEPTH] = c.depth;
        f[AVER_PHYS_KCC_F_ANGLE] = angleBetween(c.normal, k->up);
        for (int32_t i = 0; i < std::min(nF, static_cast<int32_t>(AVER_PHYS_KCC_F_COUNT)); ++i) outF[i] = f[i];
    }
    if (outI) {
        const int32_t v[AVER_PHYS_KCC_I_COUNT] = {c.collider, c.entity, c.subshape};
        for (int32_t i = 0; i < std::min(nI, static_cast<int32_t>(AVER_PHYS_KCC_I_COUNT)); ++i) outI[i] = v[i];
    }
    return 1;
}

} // extern "C"
