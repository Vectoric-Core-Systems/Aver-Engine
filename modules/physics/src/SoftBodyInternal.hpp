// SoftBodyInternal.hpp -- the soft-body registry record, shared by PhysicsSoftBody.cpp (the wave 9 builder),
// PhysicsSoftBodyExt.cpp (physics_softbody_ext_abi.h) and SoftBodyFluid.cpp (buoyancy), and the hooks that
// connect them.
//
// SRC-LOCAL like PhysicsInternal.hpp: it names JPH:: types. SoftRecord used to be file-local to
// PhysicsSoftBody.cpp; the Ext translation units need to see it, so it moved here with the new SoftExt member.
// A body that never calls an aver_phys_sbx_* function keeps a default SoftExt, which every hook treats as
// "do nothing".
#pragma once
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Math.hpp"
#include "aver/physics/physics_softbody_abi.h"
#include "aver/physics/physics_softbody_ext_abi.h"
#include "SoftBodyAero.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace aver::physics::softbody {

inline constexpr int kSoftParamCount = AVER_PHYS_SB_P_COUNT;

inline void defaultSoftParams(float* p) {
    p[AVER_PHYS_SB_P_ITERATIONS] = 5.0f;
    p[AVER_PHYS_SB_P_DAMPING] = 0.1f;
    p[AVER_PHYS_SB_P_FRICTION] = 0.2f;
    p[AVER_PHYS_SB_P_RESTITUTION] = 0.0f;
    p[AVER_PHYS_SB_P_GRAVITY_SCALE] = 1.0f;
    p[AVER_PHYS_SB_P_VERTEX_RADIUS_CM] = 0.0f;
    p[AVER_PHYS_SB_P_MAX_SPEED_CMS] = 50000.0f;
    p[AVER_PHYS_SB_P_ALLOW_SLEEP] = 1.0f;
    p[AVER_PHYS_SB_P_DOUBLE_SIDED] = 0.0f;
    p[AVER_PHYS_SB_P_LAYER] = 0.0f;
    p[AVER_PHYS_SB_P_ENTITY] = 0.0f;
    p[AVER_PHYS_SB_P_IMPACTS] = 0.0f;
}

// One vertex carried to a target each fixed step (aver_phys_sbx_attach*).
struct SoftAttachment {
    int32_t vertex = 0;
    float savedInvMass = 1.0f;         // what the vertex had before it was attached
    float maxSpeedCmS = 1.0e6f;
    int32_t otherBody = 0;             // 0 = a fixed world target
    aver::Vec3 targetCm{0, 0, 0};      // plain: the world target; to a body: the offset in that body's frame
    bool warned = false;
};

struct SoftPairPolicy {
    int32_t mode = AVER_PHYS_SBX_PAIR_NORMAL;
    float soft = 1.0f, other = 1.0f, inertia = 1.0f;
};

// Everything the wave 11 calls keep on a body. Defaults are "as before".
struct SoftExt {
    // pressure
    float pressureKPa = 0.0f;
    float restVolumeM3 = 0.0f;         // signed, from the rest positions: positive = outward winding
    bool analysed = false;             // closed / restVolumeM3 are valid (measured on first use, not at every rebuild)
    bool closed = false;               // every face edge shared by exactly two faces
    // skin
    bool skinEnabled = true;
    float skinScale = 1.0f;
    // collision group (0 = none)
    int32_t group = 0;
    // contact policy
    float softScale = 1.0f, otherScale = 1.0f, inertiaScale = 1.0f;
    std::unordered_map<int32_t, SoftPairPolicy> pairs;   // keyed by the other body's handle; read on worker threads
    // attachments
    std::vector<SoftAttachment> attachments;
    // sensors: the set seen this step (filled on worker threads under the event mutex) and last step's
    std::set<int32_t> sensorNow, sensorPrev;
    // buoyancy
    float fluidDensity = 0.0f, fluidDrag = 1.0f;
    int32_t buoyFlags = AVER_PHYS_SBX_BUOY_WORLD_WATER;
    bool hasOwnPlane = false;
    aver::Vec3 planePointCm{0, 0, 0}, planeNormal{0, 0, 1}, planeCurrentCmS{0, 0, 0};
    float submergedFraction = 0.0f, submergedVolumeCm3 = 0.0f;
    // final Jolt index of each staged item (Optimize reorders them)
    std::vector<int32_t> volumeSlot, lraSlot, rodSlot, rodPairSlot;
};

struct SoftRecord {
    JPH::Ref<JPH::SoftBodySharedSettings> settings;   // non-const: dents write into it between steps
    int32_t vertexCount = 0;
    int32_t jointCount = 0;
    float params[kSoftParamCount];
    bool impacts = false;
    bool frozen = false;                              // set_active(0): the aero loop leaves it alone
    float drag = 0.0f, lift = 0.0f;
    int32_t aeroFlags = 0;
    bool sleepForcedOff = false;
    std::vector<JPH::Vec3> velBefore;                 // vertex velocities at the start of the step, for impacts
    std::vector<float> palette;                       // the last skin palette (engine matrices), kept for rebuilds
    int32_t paletteJoints = 0;
    float entityWorld[16];
    bool hasEntityWorld = false;
    SoftExt ext;
    SoftRecord() { defaultSoftParams(params); for (float& f : entityWorld) f = 0.0f; }
};

// ---- the registry, defined in PhysicsSoftBody.cpp ----------------------------------------------------------
std::map<int32_t, SoftRecord>& records();            // ordered: the loops follow handle order
SoftRecord* recordOf(int32_t body);                  // null for a body this ABI did not build
std::mutex& eventMutex();                            // guards the impact list, the pair slots and the sensor sets
int32_t builderVertexCount(int32_t b);               // the vertex count a live builder was begun with, 0 when dead

// ---- hooks PhysicsSoftBody.cpp calls into PhysicsSoftBodyExt.cpp --------------------------------------------
// finish: a builder with volumes, rods or auto constraints may have no faces.
bool sbxFacelessOk(int32_t b);
// finish, after edges, bends and skin and before Optimize: appends volumes, LRA, rods and auto constraints,
// computes rod properties, and adds the one extra face of a face-less body. Ok or the reason to refuse.
aver::AbiError sbxPrepare(int32_t b, JPH::SoftBodySharedSettings& shared, bool stagedEdgesOrBends);
// finish with replaceBody: the new record keeps the old one's policy, buoyancy, pairs, attachments and pressure.
void sbxCarry(const SoftRecord& old, SoftRecord& next);
// finish, after the body exists and before it is added: builder params over the carried values, pressure, skin,
// group, closed and rest volume, the slot remaps, carried attachments.
void sbxApply(int32_t b, JPH::Body& body, JPH::SoftBodyMotionProperties& mp, SoftRecord& rec,
              const JPH::SoftBodySharedSettings::OptimizationResults& results);
void sbxEnsureAnalysed(SoftRecord& rec);             // measures closed and the rest volume on first use
void sbxBuilderDrop(int32_t b);                      // finish (success) or cancel: the staged Ext data goes
void sbxDestroyAll();                                // shutdown

// the soft-body contact listener
JPH::SoftBodyValidateResult sbxValidate(const SoftRecord& rec, int32_t otherHandle, const JPH::Body& other,
                                        JPH::SoftBodyContactSettings& settings);
void sbxContactAdded(SoftRecord& rec, const JPH::SoftBodyManifold& manifold);   // under the event mutex

// the fixed loop
bool sbxKeepAwake(const SoftRecord& rec);            // attachments or a submerged face: the sleep override keeps the body awake
void sbxStepBegin(int32_t handle, SoftRecord& rec, JPH::Body& body, JPH::SoftBodyMotionProperties& mp, float dt);   // body write-locked
void sbxStepEnd(float dt);                           // after system.Update: diffs the sensor sets into enter and exit events
void sbxBeginCall();                                 // top of aver_phys_step: clears the sensor events

// SoftBodyFluid.cpp: hydrostatic pressure and drag on the faces of a body with a fluid density, before the solver.
void sbxFluidStep(SoftRecord& rec, JPH::Body& body, JPH::SoftBodyMotionProperties& mp, float dt);

// Writes one vertex' inverse mass into the live vertex and the shared settings. After a batch, call
// sbxRefreshRodInvMass (each rod takes the smaller inverse mass of its two vertices, as CalculateRodProperties
// does) and mp.CalculateMassAndInertia().
void sbxSetVertexInvMass(SoftRecord& rec, JPH::SoftBodyMotionProperties& mp, int32_t vertex, float invMass);
void sbxRefreshRodInvMass(SoftRecord& rec, JPH::SoftBodyMotionProperties& mp);

} // namespace aver::physics::softbody
