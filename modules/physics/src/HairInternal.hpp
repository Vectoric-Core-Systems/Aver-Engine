// HairInternal.hpp -- the hair registry shared by PhysicsHairSettings.cpp (settings builders and settings objects) and PhysicsHair.cpp
// (host install, compute system, instances, update).
//
// SRC-LOCAL, like PhysicsInternal.hpp: it names JPH:: types. JPH::Hair, HairSettings and HairShaders live inside Aver.Physics, so the module
// still links Jolt PRIVATE and depends on Core only; the GPU side reaches the compute work only through the function table in
// physics_hair_abi.h (HairComputeHost.hpp), and with no table installed Jolt's CPU compute backend runs it.
//
// LOCKING. One mutex guards every table below. Every ABI entry point takes it; settings_finish holds it only for the registry work and runs the
// quadratic strand split unlocked, so it can sit on a worker thread while the main thread keeps updating other hair. The host table is NOT
// re-entered from two threads at once: everything that calls into it (buffer creation at finish and create, the queue at update and flush)
// is under the lock.
#pragma once
#include "aver/physics/physics_hair_abi.h"

#include "Convert.hpp"
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

// Defined in HairWindWrapper.cpp: registers the AverHairWind kernel with Jolt's CPU backend, beside Jolt's own HairRegisterShaders.
JPH_NAMESPACE_BEGIN
class ComputeSystemCPU;
void AverHairRegisterShaders(ComputeSystemCPU* inComputeSystem);
JPH_NAMESPACE_END

namespace aver::physics::detail {

// What a settings builder staged, until finish or cancel. Everything is in the ABI's units (cm, engine axes).
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
    std::vector<MaterialBlock> materials;       // by material index, grown on demand
    float params[AVER_PHYS_HAIRP_COUNT];        // NaN = default

    HairBuilderRec();
};

// A finished settings object and what the ABI reports about it.
struct HairSettingsRec {
    JPH::Ref<JPH::HairSettings> settings;
    int32_t instances = 0;                      // hairs that use it
    int32_t scalpJoints = 0;
    float rootToScalpCm = 0.0f;
    float boundsCm[6] = {};                     // min xyz, max xyz in hair space, engine axes
    std::vector<int32_t> renderTable;           // {start, count} per render strand
};

// One hair instance.
struct HairRec {
    std::unique_ptr<JPH::Hair> hair;
    int32_t settings = 0;
    int32_t layer = 0;
    std::vector<JPH::Mat44> palette;            // jointToHair is identity, so these go to Hair::Update as given
    JPH::Quat rotation = JPH::Quat::sIdentity(); // hair rotation in Jolt space: rotates the world wind into hair space
    float windCmS[3] = {0, 0, 0};
    float windDrag = 0.0f;
    float windTurbulenceCmS = 0.0f;
    JPH::Ref<JPH::ComputeBuffer> windCB;        // the wind kernel's constants
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

    // Created lazily by hairEnsureCompute (needs a world: Jolt's allocator is registered by aver_phys_init) and dropped with the world.
    JPH::Ref<JPH::ComputeSystem> compute;
    JPH::Ref<JPH::HairShaders> shaders;
    JPH::Ref<JPH::ComputeShader> windCS;        // null when the host does not know AverHairWind (wind is then off)
    JPH::Ref<CountingQueue> queue;
    int32_t readbackOk = -1;                    // -1 unknown, 0 the host cannot map a readback buffer, 1 it can
    int32_t pendingUpdates = 0;                 // updates recorded since the last flush

    std::map<int32_t, HairBuilderRec> builders;
    std::map<int32_t, HairSettingsRec> settings;
    std::map<int32_t, HairRec> hairs;
    int32_t nextBuilder = 1;
    int32_t nextSettings = 1;
    int32_t nextHair = 1;
};

HairGlobals& hairGlobals();

// Creates the compute system, the fifteen shaders plus the wind kernel and the queue if they do not exist yet. The caller holds the mutex and
// a world exists. False (logged) when the host refused a shader or Jolt's CPU backend lacked one.
bool hairEnsureCompute(HairGlobals& g);

// Applies one AVER_PHYS_HAIRM_* block (engine units) to a Jolt material; NaN entries are skipped. False when a value is not legal.
bool hairApplyMaterialBlock(JPH::HairSettings::Material& m, const float* p, int32_t n);

// Destroys every builder, settings object and hair, then the queue, shaders and compute system, then forgets the installed host. Called by
// aver_phys_shutdown before the world goes (a shared one-line edit in PhysicsWorld.cpp). Safe with no world.
void hairDestroyAll();

} // namespace aver::physics::detail
