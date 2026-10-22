// PhysicsHair.cpp -- strand hair on Jolt 5.6's Hair (physics_hair_abi.h): the registry, the compute host install, instances, the wind kernel,
// update and flush. The settings builder is PhysicsHairSettings.cpp; the adapter between Jolt's ComputeSystem and the host's function table is
// HairComputeHost.cpp.
//
// HOW AN UPDATE RUNS (design 2.14). The caller (HairSystem) calls aver_phys_hair_update once per hair per tick, then aver_phys_hair_flush once.
// An update only RECORDS: the wind kernel (if any wind is set), then Jolt's Hair::Update, which maps and rewrites its materials, constants and
// collision shapes and records about 9 + 4 N dispatches into the one shared queue. Nothing executes until flush. With Jolt's CPU backend every
// dispatch runs at once, on the calling thread, which is why the CPU host is the reference for the tests and the only one a headless run has.
#include "aver/physics/physics_hair_abi.h"
#include "Convert.hpp"
#include "HairInternal.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Compute/CPU/ComputeSystemCPU.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Shaders/HairWrapper.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

int32_t refuse(AbiError e) {
    setAbiError(e);
    return 0;
}

int32_t succeed() {
    setAbiError(AbiError::Ok);
    return 1;
}

// The wind kernel's cbuffer, 32 bytes, in the order AverHairWind.hlsl declares it.
struct WindConstants {
    uint32_t numStrands;
    uint32_t numVertices;
    uint32_t step;
    float dragDt;
    float wind[3];
    float turbulence;
};
static_assert(sizeof(WindConstants) == 32, "AverHairWind.hlsl declares 32 bytes of constants");

bool allFinite(const float* p, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

// A hair-space-to-world matrix as Jolt wants it: position, and a rotation with any scale stripped (Gram-Schmidt on the converted axes).
bool decomposeWorld(const float* w16, JPH::Vec3& pos, JPH::Quat& rot) {
    if (!allFinite(w16, 16)) return false;
    Mat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.m[r][c] = w16[r * 4 + c];
    const JPH::Mat44 j = toJolt(m);
    JPH::Vec3 x = j.GetAxisX();
    JPH::Vec3 y = j.GetAxisY();
    if (x.LengthSq() < 1.0e-12f) return false;
    x = x.Normalized();
    y = y - x * x.Dot(y);
    if (y.LengthSq() < 1.0e-12f) return false;
    y = y.Normalized();
    const JPH::Vec3 z = x.Cross(y);
    rot = JPH::Mat44(JPH::Vec4(x, 0.0f), JPH::Vec4(y, 0.0f), JPH::Vec4(z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)).GetQuaternion().Normalized();
    pos = j.GetTranslation();
    return true;
}

HairRec* hairOf(HairGlobals& g, int32_t h) {
    const auto it = g.hairs.find(h);
    if (it == g.hairs.end()) { setAbiError(AbiError::BadHandle); return nullptr; }
    return &it->second;
}

// Whether the installed host can map a readback buffer; the answer is remembered. Always yes on Jolt's CPU backend.
bool readbackWorks(HairGlobals& g) {
    if (!g.hostInstalled) return true;
    if (g.readbackOk >= 0) return g.readbackOk == 1;
    g.readbackOk = 0;
    JPH::ComputeBufferResult r = g.compute->CreateComputeBuffer(JPH::ComputeBuffer::EType::ReadbackBuffer, 1, 16);
    if (r.IsValid()) {
        void* p = r.Get()->Map(JPH::ComputeBuffer::EMode::Read);
        if (p) {
            g.readbackOk = 1;
            r.Get()->Unmap();
        }
    }
    return g.readbackOk == 1;
}

// Records the wind kernel for one hair ahead of Hair::Update. False when it cannot (no kernel, no constants buffer).
bool recordWind(HairGlobals& g, HairRec& r, const JPH::HairSettings& hs, float dtUsed) {
    if (!g.windCS) return false;
    if (!r.windCB) {
        JPH::ComputeBufferResult cb = g.compute->CreateComputeBuffer(JPH::ComputeBuffer::EType::ConstantBuffer, 1, sizeof(WindConstants));
        if (!cb.IsValid()) { AVER_WARN("[Hair] the compute host refused the wind constants buffer; wind is off for this hair"); r.windDrag = 0.0f; return false; }
        r.windCB = cb.Get();
    }
    const JPH::uint32 numStrands = static_cast<JPH::uint32>(hs.mSimStrands.size());
    const JPH::uint32 numVertices = hs.GetNumVerticesPadded();
    if (numVertices <= numStrands) return false;

    // The world wind into hair space: the rotation of the hair undone, then centimetres to metres.
    const JPH::Vec3 local = r.rotation.Conjugated() * toJoltDir(aver::Vec3(r.windCmS[0], r.windCmS[1], r.windCmS[2]));
    WindConstants c;
    c.numStrands = numStrands;
    c.numVertices = numVertices;
    c.step = r.updates;
    c.dragDt = r.windDrag * dtUsed;
    JPH::Float3 lw;
    local.StoreFloat3(&lw);
    c.wind[0] = lw.x; c.wind[1] = lw.y; c.wind[2] = lw.z;
    c.turbulence = r.windTurbulenceCmS / kCmPerMetre;
    void* dst = r.windCB->Map(JPH::ComputeBuffer::EMode::Write);
    if (!dst) return false;
    std::memcpy(dst, &c, sizeof c);
    r.windCB->Unmap();

    const JPH::uint32 batch = static_cast<JPH::uint32>(cHairPerVertexBatch);
    g.queue->SetShader(g.windCS);
    g.queue->SetConstantBuffer("gWind", r.windCB);
    g.queue->SetBuffer("gVerticesFixed", hs.mVerticesFixedCB);
    g.queue->SetBuffer("gStrandFractions", hs.mVerticesStrandFractionCB);
    g.queue->SetRWBuffer("gVelocities", r.hair->GetVelocitiesCB());
    g.queue->Dispatch((numVertices - numStrands + batch - 1) / batch);
    return true;
}

} // namespace

namespace aver::physics::detail {

HairGlobals& hairGlobals() {
    // Never destroyed: a static destructor running after Jolt's allocator is gone would free compute buffers into nothing. Everything inside
    // is emptied by hairDestroyAll at aver_phys_shutdown.
    static HairGlobals* g = new HairGlobals();
    return *g;
}

bool hairEnsureCompute(HairGlobals& g) {
    if (g.compute && g.shaders && g.queue) return true;
    g.queue = nullptr;
    g.windCS = nullptr;
    g.shaders = nullptr;
    g.compute = nullptr;

    JPH::Ref<JPH::ComputeSystem> cs;
    if (g.hostInstalled) {
        AverHostComputeSystem* host = new AverHostComputeSystem();
        host->setHost(g.host);
        cs = host;
    } else {
        JPH::ComputeSystemCPU* cpu = new JPH::ComputeSystemCPU();
        cs = cpu;
        JPH::HairRegisterShaders(cpu);
        JPH::AverHairRegisterShaders(cpu);
    }

    JPH::Ref<JPH::HairShaders> shaders = new JPH::HairShaders();
    shaders->Init(cs);
    const JPH::ComputeShader* all[] = {
        shaders->mTeleportCS, shaders->mApplyDeltaTransformCS, shaders->mSkinVerticesCS, shaders->mSkinRootsCS, shaders->mApplyGlobalPoseCS,
        shaders->mCalculateCollisionPlanesCS, shaders->mGridClearCS, shaders->mGridAccumulateCS, shaders->mGridNormalizeCS,
        shaders->mIntegrateCS, shaders->mUpdateRootsCS, shaders->mUpdateStrandsCS, shaders->mUpdateVelocityCS,
        shaders->mUpdateVelocityIntegrateCS, shaders->mCalculateRenderPositionsCS};
    for (const JPH::ComputeShader* s : all)
        if (!s) {
            AVER_ERROR("[Hair] the {} compute host does not provide all fifteen Jolt hair kernels; hair is off",
                       g.hostInstalled ? "installed" : "CPU");
            return false;
        }

    JPH::ComputeQueueResult qr = cs->CreateComputeQueue();
    if (!qr.IsValid()) {
        AVER_ERROR("[Hair] cannot create a compute queue: {}", qr.GetError().c_str());
        return false;
    }

    JPH::ComputeShaderResult wr = cs->CreateComputeShader("AverHairWind", static_cast<JPH::uint32>(cHairPerVertexBatch));
    if (wr.IsValid()) g.windCS = wr.Get();
    else AVER_WARN("[Hair] the {} compute host has no AverHairWind kernel; hair wind is off", g.hostInstalled ? "installed" : "CPU");

    g.compute = cs;
    g.shaders = shaders;
    g.queue = new CountingQueue(qr.Get());
    g.pendingUpdates = 0;
    return true;
}

void hairDestroyAll() {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    // Instances first (their buffers go back to the host), then settings, then the objects every instance shared, then the host itself.
    g.hairs.clear();
    g.settings.clear();
    g.builders.clear();
    g.queue = nullptr;
    g.windCS = nullptr;
    g.shaders = nullptr;
    g.compute = nullptr;
    g.host = {};
    g.hostInstalled = false;
    g.readbackOk = -1;
    g.pendingUpdates = 0;
    g.nextBuilder = 1;
    g.nextSettings = 1;
    g.nextHair = 1;
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_hair_abi_version(void) { return AVER_PHYS_HAIR_ABI_VERSION; }

// ---- the compute host ----

int32_t aver_phys_hair_set_compute_host(const AverPhysComputeHostV1* host) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    // The host is a property of the world's hair: buffers made by one host cannot be read by another.
    if (!g.settings.empty() || !g.hairs.empty()) return refuse(AbiError::Unsupported);
    if (host) {
        if (host->structSize != sizeof(AverPhysComputeHostV1) || host->version != 1) return refuse(AbiError::InvalidArgument);
        if (!host->create_shader || !host->create_buffer || !host->map || !host->unmap || !host->release || !host->create_queue ||
            !host->set_shader || !host->bind || !host->dispatch || !host->execute || !host->wait)
            return refuse(AbiError::InvalidArgument);
        g.host = *host;
        g.hostInstalled = true;
    } else {
        g.host = {};
        g.hostInstalled = false;
    }
    // Whatever was created for the old host goes; the next settings_finish makes the new one.
    g.queue = nullptr;
    g.windCS = nullptr;
    g.shaders = nullptr;
    g.compute = nullptr;
    g.readbackOk = -1;
    return succeed();
}

int32_t aver_phys_hair_host_kind(void) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.hostInstalled ? 1 : 0;
}

// ---- instances ----

int32_t aver_phys_hair_create(int32_t settings, const float* world16, int32_t layer) {
    if (!world16) return refuse(AbiError::NullPointer);
    if (layer < 0 || static_cast<u32>(layer) >= Layers::kUserLayerCount) return refuse(AbiError::OutOfRange);
    if (!g_world) return refuse(AbiError::NotInitialised);
    JPH::Vec3 pos;
    JPH::Quat rot;
    if (!decomposeWorld(world16, pos, rot)) return refuse(AbiError::InvalidArgument);

    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    const auto sit = g.settings.find(settings);
    if (sit == g.settings.end()) return refuse(AbiError::BadHandle);
    if (!hairEnsureCompute(g)) return refuse(AbiError::Unsupported);

    HairRec r;
    r.hair = std::make_unique<JPH::Hair>(sit->second.settings, JPH::RVec3(pos.GetX(), pos.GetY(), pos.GetZ()), rot,
                                         Layers::encode(static_cast<u32>(layer), true));
    r.hair->Init(g.compute);
    r.settings = settings;
    r.layer = layer;
    r.rotation = rot;
    r.palette.assign(static_cast<size_t>(std::max(sit->second.scalpJoints, 0)), JPH::Mat44::sIdentity());
    const int32_t handle = g.nextHair++;
    g.hairs.emplace(handle, std::move(r));
    ++sit->second.instances;
    setAbiError(AbiError::Ok);
    return handle;
}

int32_t aver_phys_hair_destroy(int32_t hair) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    const auto sit = g.settings.find(r->settings);
    if (sit != g.settings.end() && sit->second.instances > 0) --sit->second.instances;
    g.hairs.erase(hair);
    return succeed();
}

int32_t aver_phys_hair_set_transform(int32_t hair, const float* world16) {
    if (!world16) return refuse(AbiError::NullPointer);
    JPH::Vec3 pos;
    JPH::Quat rot;
    if (!decomposeWorld(world16, pos, rot)) return refuse(AbiError::InvalidArgument);
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    r->hair->SetPosition(JPH::RVec3(pos.GetX(), pos.GetY(), pos.GetZ()));
    r->hair->SetRotation(rot);
    r->rotation = rot;
    return succeed();
}

int32_t aver_phys_hair_teleport(int32_t hair) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    r->hair->OnTeleported();
    return succeed();
}

int32_t aver_phys_hair_set_palette(int32_t hair, const float* joints16, int32_t jointCount) {
    if (!joints16) return refuse(AbiError::NullPointer);
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    if (jointCount != static_cast<int32_t>(r->palette.size())) return refuse(AbiError::OutOfRange);
    if (!allFinite(joints16, jointCount * 16)) return refuse(AbiError::InvalidArgument);
    for (int32_t j = 0; j < jointCount; ++j) {
        Mat4 m;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) m.m[row][col] = joints16[j * 16 + row * 4 + col];
        r->palette[static_cast<size_t>(j)] = toJolt(m);
    }
    return succeed();
}

int32_t aver_phys_hair_set_wind(int32_t hair, const float* windCmS3, float dragPerS, float turbulenceCmS) {
    if (!windCmS3) return refuse(AbiError::NullPointer);
    if (!allFinite(windCmS3, 3) || !std::isfinite(dragPerS) || !std::isfinite(turbulenceCmS) || dragPerS < 0.0f || turbulenceCmS < 0.0f)
        return refuse(AbiError::InvalidArgument);
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    r->windCmS[0] = windCmS3[0]; r->windCmS[1] = windCmS3[1]; r->windCmS[2] = windCmS3[2];
    r->windDrag = dragPerS;
    r->windTurbulenceCmS = turbulenceCmS;
    return succeed();
}

int32_t aver_phys_hair_update(int32_t hair, float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return refuse(AbiError::InvalidArgument);
    if (!g_world) return refuse(AbiError::NotInitialised);
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    if (!hairEnsureCompute(g)) return refuse(AbiError::Unsupported);
    const JPH::HairSettings& hs = *g.settings.at(r->settings).settings;

    const float dtUsed = std::min(dt, hs.mMaxDeltaTime);
    const uint32_t before = g.queue->dispatches();
    r->lastWind = 0;
    const bool windOn = r->windDrag > 0.0f &&
                        (r->windCmS[0] != 0.0f || r->windCmS[1] != 0.0f || r->windCmS[2] != 0.0f || r->windTurbulenceCmS != 0.0f);
    if (windOn && recordWind(g, *r, hs, dtUsed)) r->lastWind = 1;

    r->hair->Update(dt, JPH::Mat44::sIdentity(), r->palette.empty() ? nullptr : r->palette.data(), g_world->system, *g.shaders,
                    g.compute, g.queue);

    r->lastDispatches = g.queue->dispatches() - before;
    r->lastDt = dtUsed;
    r->lastIterations = static_cast<uint32_t>(std::round(dtUsed * static_cast<float>(hs.mNumIterationsPerSecond)));
    ++r->updates;
    ++g.pendingUpdates;
    return succeed();
}

int32_t aver_phys_hair_flush(void) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.queue || g.pendingUpdates == 0) return succeed();
    g.queue->Execute();
    g.queue->Wait();
    g.pendingUpdates = 0;
    return succeed();
}

uint64_t aver_phys_hair_render_buffer(int32_t hair) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    const auto it = g.hairs.find(hair);
    if (it == g.hairs.end() || !g.hostInstalled) return 0;
    const JPH::ComputeBuffer* cb = it->second.hair->GetRenderPositionsCB();
    return cb ? static_cast<const HostBuffer*>(cb)->handle() : 0;
}

int32_t aver_phys_hair_info(int32_t hair, int32_t* outI, int32_t nI, float* outF, int32_t nF) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    if (outI) {
        const int32_t v[AVER_PHYS_HAIRUI_COUNT] = {static_cast<int32_t>(r->lastIterations), static_cast<int32_t>(r->lastDispatches),
                                                   static_cast<int32_t>(r->updates), static_cast<int32_t>(r->lastWind)};
        for (int32_t i = 0; i < nI && i < AVER_PHYS_HAIRUI_COUNT; ++i) outI[i] = v[i];
    }
    if (outF && nF > 0) outF[AVER_PHYS_HAIRUF_DT_USED] = r->lastDt;
    return succeed();
}

int32_t aver_phys_hair_read_positions(int32_t hair, float* outSimXyz, int32_t maxSim, float* outRenderXyz, int32_t maxRender) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairRec* r = hairOf(g, hair);
    if (!r) return 0;
    if (!g.queue || !g.compute) return refuse(AbiError::NotInitialised);
    if (!readbackWorks(g)) return refuse(AbiError::Unsupported);
    const JPH::HairSettings& hs = *g.settings.at(r->settings).settings;

    // ReadBackGPUState executes and waits on the shared queue, so everything recorded so far runs first.
    r->hair->ReadBackGPUState(g.queue);
    g.pendingUpdates = 0;
    r->hair->LockReadBackBuffers();
    if (outSimXyz) {
        const JPH::Float3* sim = r->hair->GetPositions();
        const int32_t n = std::min(maxSim, static_cast<int32_t>(hs.mSimVertices.size()));
        for (int32_t i = 0; i < n; ++i) {
            const aver::Vec3 e = fromJolt(JPH::Vec3(sim[i]));
            outSimXyz[i * 3] = e.x; outSimXyz[i * 3 + 1] = e.y; outSimXyz[i * 3 + 2] = e.z;
        }
    }
    if (outRenderXyz) {
        const JPH::Float3* render = r->hair->GetRenderPositions();
        const int32_t n = std::min(maxRender, static_cast<int32_t>(hs.mRenderVertices.size()));
        for (int32_t i = 0; i < n; ++i) {
            const aver::Vec3 e = fromJolt(JPH::Vec3(render[i]));
            outRenderXyz[i * 3] = e.x; outRenderXyz[i * 3 + 1] = e.y; outRenderXyz[i * 3 + 2] = e.z;
        }
    }
    r->hair->UnlockReadBackBuffers();
    return succeed();
}

} // extern "C"
