// HairInternal.hpp: the hair registry shared by PhysicsHairSettings.cpp and PhysicsHair.cpp. Src-local; names JPH:: types.
// Locking: one mutex guards every table; the host table is never entered from two threads at once.
#pragma once
#include "aver/physics/physics_hair_abi.h"

#include "HairComputeHost.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Compute/ComputeBuffer.h>
#include <Jolt/Compute/ComputeQueue.h>
#include <Jolt/Compute/ComputeShader.h>
#include <Jolt/Compute/ComputeSystem.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Physics/Hair/Hair.h>
#include <Jolt/Physics/Hair/HairSettings.h>
#include <Jolt/Physics/Hair/HairShaders.h>

#ifndef JPH_USE_CPU_COMPUTE
#  error "JPH_USE_CPU_COMPUTE must be defined for Aver.Physics: modules/physics.jolt/CMakeLists.txt sets JPH_USE_CPU_COMPUTE ON (hair, design 2.10)."
#endif

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Defined in HairWindWrapper.cpp.
JPH_NAMESPACE_BEGIN
class ComputeSystemCPU;
void AverHairRegisterShaders(ComputeSystemCPU* inComputeSystem);
JPH_NAMESPACE_END

namespace aver::physics::detail {

// A settings builder's staged data, until finish or cancel. Engine units (cm, engine axes).
struct HairBuilderRec {
    std::vector<float> points;                  // 3 per vertex
    std::vector<float> invMass;                 // per vertex, or empty = 1 each
    std::vector<int32_t> strandStart;           // strandCount + 1
    std::vector<int32_t> strandMaterial;        // per strand, or empty = 0
    int32_t vertexCount = 0;
    int32_t strandCount = 0;

    bool hasScalp = false;
    std::vector<float> scalpVerts;              // 3 per vertex
    std::vector<int32_t> scalpTris;             // 3 per triangle
    std::vector<int32_t> scalpJoints;           // 4 per vertex, or empty
    std::vector<float> scalpWeights;            // 4 per vertex, or empty
    int32_t scalpVertexCount = 0;
    int32_t scalpTriCount = 0;
    int32_t scalpJointCount = 1;

    struct MaterialBlock {
        bool described = false;
        float p[AVER_PHYS_HAIRM_COUNT];         // NaN = untouched
    };
    std::vector<MaterialBlock> materials;
    float params[AVER_PHYS_HAIRP_COUNT];        // NaN = default

    HairBuilderRec();
};

struct HairSettingsRec {
    JPH::Ref<JPH::HairSettings> settings;
    int32_t instances = 0;
    int32_t scalpJoints = 0;
    float rootToScalpCm = 0.0f;
    float boundsCm[6] = {};                     // min xyz, max xyz in hair space
    std::vector<int32_t> renderTable;           // {start, count} per render strand
};

struct HairRec {
    std::unique_ptr<JPH::Hair> hair;
    int32_t settings = 0;
    int32_t layer = 0;
    std::vector<JPH::Mat44> palette;            // jointToHair is identity, passed to Hair::Update as given
    JPH::Quat rotation = JPH::Quat::sIdentity(); // rotates the world wind into hair space
    float windCmS[3] = {0, 0, 0};
    float windDrag = 0.0f;
    float windTurbulenceCmS = 0.0f;
    JPH::Ref<JPH::ComputeBuffer> windCB;
    uint32_t updates = 0;
    uint32_t lastIterations = 0;
    uint32_t lastDispatches = 0;
    uint32_t lastWind = 0;
    float lastDt = 0.0f;
};

struct HairGlobals {
    std::mutex mutex;

    AverPhysComputeHostV1 host = {};
    bool hostInstalled = false;

    // Created lazily by hairEnsureCompute (needs a world: Jolt's allocator is registered by aver_phys_init); dropped with the world.
    JPH::Ref<JPH::ComputeSystem> compute;
    JPH::Ref<JPH::HairShaders> shaders;
    JPH::Ref<JPH::ComputeShader> windCS;       // null when the host does not know AverHairWind (wind is then off)
    JPH::Ref<CountingQueue> queue;
    int32_t readbackOk = -1;                    // -1 unknown, 0 cannot map a readback buffer, 1 can
    int32_t pendingUpdates = 0;

    std::map<int32_t, HairBuilderRec> builders;
    std::map<int32_t, HairSettingsRec> settings;
    std::map<int32_t, HairRec> hairs;
    int32_t nextBuilder = 1;
    int32_t nextSettings = 1;
    int32_t nextHair = 1;
};

HairGlobals& hairGlobals();

// Creates the compute system, the fifteen shaders, the wind kernel and the queue if missing. Caller holds the mutex; a world exists.
// False (logged) when the host refused a shader or Jolt's CPU backend lacked one.
bool hairEnsureCompute(HairGlobals& g);

// Applies one AVER_PHYS_HAIRM_* block (engine units) to a Jolt material; NaN entries are skipped. False when a value is not legal.
bool hairApplyMaterialBlock(JPH::HairSettings::Material& m, const float* p, int32_t n);

// Destroys every builder, settings object and hair, then the queue, shaders and compute system, then forgets the host.
// Called by aver_phys_shutdown (PhysicsWorld.cpp) before the world goes. Safe with no world.
void hairDestroyAll();

} // namespace aver::physics::detail
