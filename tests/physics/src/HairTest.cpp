// Strand hair on Jolt's Hair (physics_hair_abi.h), by simulating a small groom on Jolt's CPU compute backend and on a FAKE HOST that wraps the
// same CPU backend behind the C function table (AverPhysComputeHostV1). The second proves the adapter end to end: the same Jolt kernels,
// reached only through create_shader / create_buffer / bind / dispatch / readback, must give the same positions as the built-in backend.
//
// This is the ONE physics test that links Jolt directly (PhysicsTest is the other): the fake host IS a second Jolt consumer, and it also compiles
// HairWindWrapper.cpp itself so its compute system knows the AverHairWind kernel. Everything else it checks goes through the ABI.
//
// Written against the header with thresholds that name what is being decided (a strand that droops against one that does not, a wind that moves
// the tips by a monotone amount), NOT how close Jolt gets. A failing threshold is a question about the number before it is a question about the code.
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_hair_abi.h"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Compute/CPU/ComputeSystemCPU.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Shaders/HairWrapper.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// Defined in modules/physics/src/HairWindWrapper.cpp, which this target compiles itself.
JPH_NAMESPACE_BEGIN
void AverHairRegisterShaders(ComputeSystemCPU* inComputeSystem);
JPH_NAMESPACE_END

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static int32_t errCode(AbiError e) { return static_cast<int32_t>(e); }

static void freshWorld() {
    aver_phys_shutdown();
    aver_phys_set_fixed_step(1.0f / 60.0f);
    aver_phys_init();
}

// ---- the fake host: the CPU backend behind the function table ----------------------------------------------------

struct FakeHost {
    JPH::Ref<JPH::ComputeSystemCPU> cs;
    std::unordered_map<uint64_t, JPH::Ref<JPH::ComputeShader>> shaders;
    std::unordered_map<uint64_t, JPH::Ref<JPH::ComputeBuffer>> buffers;
    std::unordered_map<uint64_t, JPH::Ref<JPH::ComputeQueue>> queues;
    uint64_t next = 1;
    int shaderCalls = 0, bufferCalls = 0, bindCalls = 0, dispatches = 0, readbacks = 0, executes = 0;

    FakeHost() {
        cs = new JPH::ComputeSystemCPU();
        JPH::HairRegisterShaders(cs);
        JPH::AverHairRegisterShaders(cs);
    }

    static FakeHost& of(void* user) { return *static_cast<FakeHost*>(user); }

    static uint64_t createShader(void* user, const char* name, uint32_t gx, uint32_t gy, uint32_t gz) {
        FakeHost& f = of(user);
        ++f.shaderCalls;
        JPH::ComputeShaderResult r = f.cs->CreateComputeShader(name, gx, gy, gz);
        if (!r.IsValid()) return 0;
        const uint64_t h = f.next++;
        f.shaders[h] = r.Get();
        return h;
    }

    static uint64_t createBuffer(void* user, int32_t type, uint64_t count, uint32_t stride, const void* data) {
        FakeHost& f = of(user);
        ++f.bufferCalls;
        JPH::ComputeBuffer::EType t = JPH::ComputeBuffer::EType::RWBuffer;
        switch (type) {
            case AVER_PHYS_CB_UPLOAD:   t = JPH::ComputeBuffer::EType::UploadBuffer; break;
            case AVER_PHYS_CB_READBACK: t = JPH::ComputeBuffer::EType::ReadbackBuffer; break;
            case AVER_PHYS_CB_CONSTANT: t = JPH::ComputeBuffer::EType::ConstantBuffer; break;
            case AVER_PHYS_CB_BUFFER:   t = JPH::ComputeBuffer::EType::Buffer; break;
            default: break;
        }
        JPH::ComputeBufferResult r = f.cs->CreateComputeBuffer(t, count, stride, data);
        if (!r.IsValid()) return 0;
        const uint64_t h = f.next++;
        f.buffers[h] = r.Get();
        return h;
    }

    static void* map(void* user, uint64_t buffer, int32_t write) {
        FakeHost& f = of(user);
        const auto it = f.buffers.find(buffer);
        if (it == f.buffers.end()) return nullptr;
        return it->second->Map(write ? JPH::ComputeBuffer::EMode::Write : JPH::ComputeBuffer::EMode::Read);
    }

    static void unmap(void* user, uint64_t buffer) {
        FakeHost& f = of(user);
        const auto it = f.buffers.find(buffer);
        if (it != f.buffers.end()) it->second->Unmap();
    }

    static void release(void* user, uint64_t object) {
        FakeHost& f = of(user);
        f.shaders.erase(object);
        f.buffers.erase(object);
        f.queues.erase(object);
    }

    static uint64_t createQueue(void* user) {
        FakeHost& f = of(user);
        JPH::ComputeQueueResult r = f.cs->CreateComputeQueue();
        if (!r.IsValid()) return 0;
        const uint64_t h = f.next++;
        f.queues[h] = r.Get();
        return h;
    }

    static void setShader(void* user, uint64_t queue, uint64_t shader) {
        FakeHost& f = of(user);
        f.queues.at(queue)->SetShader(f.shaders.at(shader));
    }

    static void bind(void* user, uint64_t queue, const char* name, uint64_t buffer, int32_t role, int32_t barrier) {
        FakeHost& f = of(user);
        ++f.bindCalls;
        JPH::ComputeQueue* q = f.queues.at(queue);
        JPH::ComputeBuffer* b = f.buffers.at(buffer);
        if (role == AVER_PHYS_BIND_CONSTANT) q->SetConstantBuffer(name, b);
        else if (role == AVER_PHYS_BIND_READ) q->SetBuffer(name, b);
        else q->SetRWBuffer(name, b, barrier ? JPH::ComputeQueue::EBarrier::Yes : JPH::ComputeQueue::EBarrier::No);
    }

    static void dispatch(void* user, uint64_t queue, uint32_t gx, uint32_t gy, uint32_t gz) {
        FakeHost& f = of(user);
        ++f.dispatches;
        f.queues.at(queue)->Dispatch(gx, gy, gz);
    }

    // The CPU backend's own readback buffer IS the source buffer; here the destination is a separate buffer, so it is copied.
    static void readback(void* user, uint64_t, uint64_t dst, uint64_t src) {
        FakeHost& f = of(user);
        ++f.readbacks;
        JPH::ComputeBuffer* d = f.buffers.at(dst);
        JPH::ComputeBuffer* s = f.buffers.at(src);
        const void* from = s->Map(JPH::ComputeBuffer::EMode::Read);
        void* to = d->Map(JPH::ComputeBuffer::EMode::Write);
        std::memcpy(to, from, static_cast<size_t>(s->GetSize()) * s->GetStride());
        d->Unmap();
        s->Unmap();
    }

    static void execute(void* user, uint64_t queue) {
        FakeHost& f = of(user);
        ++f.executes;
        f.queues.at(queue)->Execute();
    }

    static void wait(void* user, uint64_t queue) { of(user).queues.at(queue)->Wait(); }

    AverPhysComputeHostV1 table() {
        AverPhysComputeHostV1 t = {};
        t.structSize = sizeof(AverPhysComputeHostV1);
        t.version = 1;
        t.user = this;
        t.create_shader = &FakeHost::createShader;
        t.create_buffer = &FakeHost::createBuffer;
        t.map = &FakeHost::map;
        t.unmap = &FakeHost::unmap;
        t.release = &FakeHost::release;
        t.create_queue = &FakeHost::createQueue;
        t.set_shader = &FakeHost::setShader;
        t.bind = &FakeHost::bind;
        t.dispatch = &FakeHost::dispatch;
        t.readback = &FakeHost::readback;
        t.execute = &FakeHost::execute;
        t.wait = &FakeHost::wait;
        return t;
    }
};

// ---- a groom: strands sticking out sideways from a flat scalp, so gravity has something to do ------------------------------------

struct Groom {
    std::vector<float> points;                 // cm
    std::vector<int32_t> start;
    std::vector<int32_t> material;
    std::vector<float> scalp;
    std::vector<int32_t> tris;
    int strands = 0;
    int verts = 0;
    float segCm = 0.0f;
    std::vector<float> rootOf(int s) const { return {points[static_cast<size_t>(s) * static_cast<size_t>(verts) * 3], points[static_cast<size_t>(s) * static_cast<size_t>(verts) * 3 + 1], points[static_cast<size_t>(s) * static_cast<size_t>(verts) * 3 + 2]}; }
};

// nx * ny strands rooted on the z = 0 plane, each `verts` points long along +X in steps of segCm.
static Groom makeGroom(int nx, int ny, int verts, float segCm) {
    Groom g;
    g.strands = nx * ny;
    g.verts = verts;
    g.segCm = segCm;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const float rx = -9.0f + 18.0f * static_cast<float>(i) / static_cast<float>(std::max(nx - 1, 1));
            const float ry = -9.0f + 18.0f * static_cast<float>(j) / static_cast<float>(std::max(ny - 1, 1));
            g.start.push_back(static_cast<int32_t>(g.points.size() / 3));
            g.material.push_back(0);
            for (int v = 0; v < verts; ++v) {
                g.points.push_back(rx + segCm * static_cast<float>(v));
                g.points.push_back(ry);
                g.points.push_back(0.0f);
            }
        }
    g.start.push_back(static_cast<int32_t>(g.points.size() / 3));
    const float e = 12.0f;
    g.scalp = {-e, -e, 0, e, -e, 0, e, e, 0, -e, e, 0};
    g.tris = {0, 1, 2, 0, 2, 3};
    return g;
}

static void nanFill(float* p, int n) {
    for (int i = 0; i < n; ++i) p[i] = std::nanf("");
}

struct Options {
    float globalPose = 0.0f;           // 0 = the pose is not pulled back, the strand simply droops
    float simFraction = 1.0f;
    bool collision = true;
    int grid = 8;
};

// Builds a settings object; 0 on any refusal.
static int32_t buildSettings(const Groom& g, const Options& o, float* rootToScalp = nullptr) {
    const int32_t b = aver_phys_hair_settings_begin();
    if (!b) return 0;
    if (!aver_phys_hair_settings_strands(b, g.points.data(), nullptr, static_cast<int32_t>(g.points.size() / 3), g.start.data(),
                                         g.material.data(), g.strands)) { aver_phys_hair_settings_cancel(b); return 0; }
    if (!aver_phys_hair_settings_scalp(b, g.scalp.data(), static_cast<int32_t>(g.scalp.size() / 3), g.tris.data(),
                                       static_cast<int32_t>(g.tris.size() / 3), nullptr, nullptr, 1)) { aver_phys_hair_settings_cancel(b); return 0; }
    float p[AVER_PHYS_HAIRP_COUNT];
    nanFill(p, AVER_PHYS_HAIRP_COUNT);
    p[AVER_PHYS_HAIRP_SIM_FRACTION] = o.simFraction;
    p[AVER_PHYS_HAIRP_GRID_X] = p[AVER_PHYS_HAIRP_GRID_Y] = p[AVER_PHYS_HAIRP_GRID_Z] = static_cast<float>(o.grid);
    aver_phys_hair_settings_params(b, p, AVER_PHYS_HAIRP_COUNT);
    float m[AVER_PHYS_HAIRM_COUNT];
    nanFill(m, AVER_PHYS_HAIRM_COUNT);
    m[AVER_PHYS_HAIRM_ENABLE_COLLISION] = o.collision ? 1.0f : 0.0f;
    m[AVER_PHYS_HAIRM_BEND_COMPLIANCE] = 5.0e-4f;   // 1e-5 holds a 10 cm strand within 1 cm of level (measured, 120 steps); 5e-4 hangs it down ~6 cm
    m[AVER_PHYS_HAIRM_GLOBAL_POSE + 0] = o.globalPose; m[AVER_PHYS_HAIRM_GLOBAL_POSE + 1] = o.globalPose;
    m[AVER_PHYS_HAIRM_GLOBAL_POSE + 2] = 0.0f;         m[AVER_PHYS_HAIRM_GLOBAL_POSE + 3] = 1.0f;
    m[AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE + 0] = 0.0f;    m[AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE + 1] = 0.0f;
    m[AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE + 2] = 0.0f;    m[AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE + 3] = 1.0f;
    m[AVER_PHYS_HAIRM_SIM_FRACTION] = o.simFraction;
    aver_phys_hair_settings_material(b, 0, m, AVER_PHYS_HAIRM_COUNT);
    return aver_phys_hair_settings_finish(b, 0, rootToScalp);
}

static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static std::vector<float> readSim(int32_t hair, int32_t simVerts) {
    std::vector<float> out(static_cast<size_t>(simVerts) * 3, 0.0f);
    aver_phys_hair_read_positions(hair, out.data(), simVerts, nullptr, 0);
    return out;
}

static void stepHair(int32_t hair, int n, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < n; ++i) {
        aver_phys_hair_update(hair, dt);
        aver_phys_hair_flush();
    }
}

static bool allFinite(const std::vector<float>& v) {
    for (float f : v)
        if (!std::isfinite(f)) return false;
    return true;
}

// Length of strand s in the (padded-free) sim vertex list when every strand is simulated and strands have `verts` points.
static float strandLength(const std::vector<float>& sim, int s, int verts) {
    float len = 0.0f;
    for (int v = 0; v + 1 < verts; ++v) {
        const size_t a = (static_cast<size_t>(s) * static_cast<size_t>(verts) + static_cast<size_t>(v)) * 3;
        const size_t b = a + 3;
        const float dx = sim[b] - sim[a], dy = sim[b + 1] - sim[a + 1], dz = sim[b + 2] - sim[a + 2];
        len += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return len;
}

// ---- the tests ----------------------------------------------------------------------------------------------------------------

static void testSettings() {
    AVER_INFO("settings: refusals, info, the render table");
    freshWorld();
    check(aver_phys_hair_abi_version() == AVER_PHYS_HAIR_ABI_VERSION, "aver_phys_hair_abi_version matches the header");
    check(aver_phys_hair_host_kind() == 0, "no host installed: the CPU backend");

    const Groom g = makeGroom(4, 4, 6, 2.0f);
    const int32_t b = aver_phys_hair_settings_begin();
    check(b > 0, "settings_begin gives a builder id");

    // A strand of two points, and one of 256, are refused; so is a material above 255.
    {
        const float p2[6] = {0, 0, 0, 1, 0, 0};
        const int32_t s2[2] = {0, 2};
        check(aver_phys_hair_settings_strands(b, p2, nullptr, 2, s2, nullptr, 1) == 0, "a strand of 2 points is refused");
        std::vector<float> p256(256 * 3);
        for (int i = 0; i < 256; ++i) { p256[static_cast<size_t>(i) * 3] = static_cast<float>(i); p256[static_cast<size_t>(i) * 3 + 1] = 0; p256[static_cast<size_t>(i) * 3 + 2] = 0; }
        const int32_t s256[2] = {0, 256};
        check(aver_phys_hair_settings_strands(b, p256.data(), nullptr, 256, s256, nullptr, 1) == 0, "a strand of 256 vertices is refused");
        const int32_t badMat[1] = {256};
        const int32_t s3[2] = {0, 3};
        const float p3[9] = {0, 0, 0, 1, 0, 0, 2, 0, 0};
        check(aver_phys_hair_settings_strands(b, p3, nullptr, 3, s3, badMat, 1) == 0, "a material index of 256 is refused");
        check(aver_phys_hair_settings_material(b, 256, nullptr, 0) == 0, "settings_material refuses index 256");
        const float coincident[9] = {0, 0, 0, 0, 0, 0, 1, 0, 0};
        check(aver_phys_hair_settings_strands(b, coincident, nullptr, 3, s3, nullptr, 1) == 0, "two coincident points (a rod of zero length) are refused");
        float badBlock[AVER_PHYS_HAIRM_COUNT];
        nanFill(badBlock, AVER_PHYS_HAIRM_COUNT);
        badBlock[AVER_PHYS_HAIRM_GLOBAL_POSE + 2] = 0.5f; badBlock[AVER_PHYS_HAIRM_GLOBAL_POSE + 3] = 0.5f;
        check(aver_phys_hair_settings_material(b, 0, badBlock, AVER_PHYS_HAIRM_COUNT) == 0, "a gradient whose two fractions coincide is refused");
    }
    check(aver_phys_hair_settings_finish(b, 0, nullptr) == 0, "finish with no strands staged is refused (and consumes the builder)");
    check(aver_phys_hair_settings_cancel(b) == 0, "a consumed builder id is dead");

    float root = -1.0f;
    const int32_t settings = buildSettings(g, Options(), &root);
    check(settings > 0, "a 16-strand groom finishes");
    check(root >= 0.0f && root < 0.05f, "roots on the scalp plane report a root-to-scalp distance below 0.5 mm");

    int32_t info[AVER_PHYS_HAIRI_COUNT] = {};
    float infoF[AVER_PHYS_HAIRF_COUNT] = {};
    check(aver_phys_hair_settings_info(settings, info, AVER_PHYS_HAIRI_COUNT, infoF, AVER_PHYS_HAIRF_COUNT) == 1, "settings_info answers");
    check(info[AVER_PHYS_HAIRI_RENDER_STRANDS] == 16 && info[AVER_PHYS_HAIRI_RENDER_VERTICES] == 16 * 6, "info: 16 render strands of 6 vertices");
    check(info[AVER_PHYS_HAIRI_SIM_STRANDS] == 16, "sim fraction 1 simulates every strand");
    check(info[AVER_PHYS_HAIRI_MAX_VERTICES_PER_STRAND] == 6, "info: 6 vertices per strand at most");
    check(infoF[AVER_PHYS_HAIRF_BOUNDS_MAX_X] > infoF[AVER_PHYS_HAIRF_BOUNDS_MIN_X], "the simulation bounds have a positive width");

    check(aver_phys_hair_settings_render_strands(settings, nullptr, 0) == 16, "render_strands counts 16");
    int32_t table[32] = {};
    check(aver_phys_hair_settings_render_strands(settings, table, 16) == 16, "render_strands writes the table");
    check(table[0] == 0 && table[1] == 6 && table[2] == 6 && table[3] == 6, "the render table lists strands in input order, 6 vertices each");

    // A live material edit is read by the next update; an index past the end is refused.
    float live[AVER_PHYS_HAIRM_COUNT];
    nanFill(live, AVER_PHYS_HAIRM_COUNT);
    live[AVER_PHYS_HAIRM_LINEAR_DAMPING] = 5.0f;
    check(aver_phys_hair_settings_set_material(settings, 0, live, AVER_PHYS_HAIRM_COUNT) == 1, "set_material edits a live settings object");
    check(aver_phys_hair_settings_set_material(settings, 1, live, AVER_PHYS_HAIRM_COUNT) == 0, "set_material refuses a material that does not exist");

    const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
    check(hair > 0, "create makes a hair");
    check(aver_phys_hair_settings_destroy(settings) == 0 && aver_phys_last_error() == errCode(AbiError::Unsupported), "settings_destroy is refused while an instance uses it");
    check(aver_phys_hair_create(settings, kIdentity, 16) == 0, "a layer of 16 is refused");
    check(aver_phys_hair_create(99999, kIdentity, 0) == 0, "an unknown settings handle is refused");
    check(aver_phys_hair_set_compute_host(nullptr) == 0, "the host cannot change while settings exist");
    check(aver_phys_hair_destroy(hair) == 1, "destroy");
    check(aver_phys_hair_destroy(hair) == 0, "destroy twice");
    check(aver_phys_hair_settings_destroy(settings) == 1, "settings_destroy once no instance is left");
}

static void testFalls() {
    AVER_INFO("a groom droops under gravity: lengths, roots, finiteness");
    freshWorld();
    const Groom g = makeGroom(8, 8, 8, 1.5f);
    const int32_t settings = buildSettings(g, Options());
    const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
    check(settings > 0 && hair > 0, "64 strands of 8 vertices build");

    const int32_t simVerts = 64 * 8;
    stepHair(hair, 1);
    readSim(hair, simVerts);   // read back after one step; the values are not checked here
    stepHair(hair, 119);
    const std::vector<float> sim = readSim(hair, simVerts);
    check(allFinite(sim), "no non-finite value after 120 steps");

    int lengthBad = 0, rootBad = 0, tipsDown = 0;
    float worstLen = 0.0f, worstRoot = 0.0f;
    const float restLen = 1.5f * 7.0f;
    for (int s = 0; s < 64; ++s) {
        const float len = strandLength(sim, s, 8);
        worstLen = std::max(worstLen, std::fabs(len - restLen) / restLen);
        if (std::fabs(len - restLen) > 0.03f * restLen) ++lengthBad;
        const std::vector<float> root = g.rootOf(s);
        const size_t r = static_cast<size_t>(s) * 8 * 3;
        const float d = std::sqrt((sim[r] - root[0]) * (sim[r] - root[0]) + (sim[r + 1] - root[1]) * (sim[r + 1] - root[1]) + (sim[r + 2] - root[2]) * (sim[r + 2] - root[2]));
        worstRoot = std::max(worstRoot, d);
        if (d > 0.1f) ++rootBad;
        const size_t tip = (static_cast<size_t>(s) * 8 + 7) * 3;
        if (sim[tip + 2] < -1.0f) ++tipsDown;
    }
    AVER_INFO("  worst strand length error {:.4f}, worst root offset {:.4f} cm, {} of 64 tips below -1 cm", worstLen, worstRoot, tipsDown);
    check(lengthBad == 0, "every strand length stays within 3% of rest");
    check(rootBad == 0, "roots stay on the scalp (within 1 mm)");
    check(tipsDown >= 60, "tips droop below the scalp plane (gravity acts)");

    int32_t ui[AVER_PHYS_HAIRUI_COUNT] = {};
    float uf[AVER_PHYS_HAIRUF_COUNT] = {};
    check(aver_phys_hair_info(hair, ui, AVER_PHYS_HAIRUI_COUNT, uf, AVER_PHYS_HAIRUF_COUNT) == 1, "hair_info answers");
    check(ui[AVER_PHYS_HAIRUI_ITERATIONS] == 6, "1/60 s at 360 iterations per second is 6 iterations");
    check(ui[AVER_PHYS_HAIRUI_DISPATCHES] >= 9 + 4 * 6 - 8, "an update records about 9 + 4N dispatches");
    check(ui[AVER_PHYS_HAIRUI_UPDATES] == 120, "120 updates counted");
    check(ui[AVER_PHYS_HAIRUI_WIND] == 0, "no wind kernel without wind");

    // Render positions are read too, in render buffer order.
    std::vector<float> render(64 * 8 * 3, 0.0f);
    check(aver_phys_hair_read_positions(hair, nullptr, 0, render.data(), 64 * 8) == 1 && allFinite(render), "render positions read back, all finite");
}

static void testTeleportAndTransform() {
    AVER_INFO("teleport restores the rest pose; moving the head makes the tips lag");
    freshWorld();
    const Groom g = makeGroom(4, 4, 8, 1.5f);
    const int32_t settings = buildSettings(g, Options());
    const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
    const int32_t simVerts = 16 * 8;

    stepHair(hair, 120);
    const std::vector<float> drooped = readSim(hair, simVerts);
    const size_t tip0 = 7 * 3;
    check(drooped[tip0 + 2] < -1.0f, "the tip has drooped");

    aver_phys_hair_teleport(hair);
    stepHair(hair, 1);
    const std::vector<float> reset = readSim(hair, simVerts);
    check(std::fabs(reset[tip0 + 2]) < 0.5f && std::fabs(reset[tip0] - (g.rootOf(0)[0] + 1.5f * 7)) < 0.5f,
          "after teleport the tip is back at its rest pose (within 5 mm after one step)");

    // A quick sideways move of the head: free vertices stay in world space, so in hair space they move the other way.
    stepHair(hair, 60);
    const std::vector<float> beforeMove = readSim(hair, simVerts);
    float moved[16];
    std::memcpy(moved, kIdentity, sizeof moved);
    moved[13] = 10.0f;   // +10 cm along engine +Y (row 3 is the translation)
    check(aver_phys_hair_set_transform(hair, moved) == 1, "set_transform");
    // The stretch constraints drag the strand with its root within the first step (the tip even overshoots by ~1 cm), then it swings back: the
    // lag shows within a few frames. Measured at 5e-4 bend compliance: the tip reaches 3 cm behind its rest y by step 13.
    float lowestY = beforeMove[tip0 + 1];
    for (int k = 0; k < 30; ++k) {
        stepHair(hair, 1);
        lowestY = std::min(lowestY, readSim(hair, simVerts)[tip0 + 1]);
    }
    check(lowestY < beforeMove[tip0 + 1] - 0.5f, "a head that moved +10 cm leaves the tip behind in hair space");
    check(aver_phys_hair_set_transform(hair, nullptr) == 0, "set_transform refuses a null matrix");
    float bad[16];
    std::memcpy(bad, kIdentity, sizeof bad);
    bad[0] = std::nanf("");
    check(aver_phys_hair_set_transform(hair, bad) == 0, "set_transform refuses a non-finite matrix");
}

static void testProxy() {
    AVER_INFO("a convex hull stops the droop");
    const Groom g = makeGroom(4, 4, 8, 1.5f);   // 10.5 cm long strands starting at x in [-9, 9]
    auto lowestUnderHull = [&](bool withHull) {
        freshWorld();
        if (withHull) {
            // A 4 x 40 x 4 cm slab, top at z = -2, sitting under the strand tips (x from 2 to 12).
            const float pts[24] = {-2, -20, -2,  2, -20, -2,  2, 20, -2,  -2, 20, -2,
                                   -2, -20, -6,  2, -20, -6,  2, 20, -6,  -2, 20, -6};
            // The hull's points are relative to its centre; the centre is at x = 6, z = 0 so the top face is at z = -2.
            const int32_t body = aver_phys_add_convex_hull(pts, 8, 6.0f, 0.0f, 0.0f, 0, 0.0f);
            check(body > 0, "a static convex hull body");
        }
        const int32_t settings = buildSettings(g, Options());
        const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
        stepHair(hair, 180);
        const std::vector<float> sim = readSim(hair, 16 * 8);
        float lowest = 1.0e9f;
        for (size_t v = 0; v < 16 * 8; ++v)
            if (sim[v * 3] > 4.5f && sim[v * 3] < 7.5f) lowest = std::min(lowest, sim[v * 3 + 2]);
        return lowest;
    };
    const float freeLow = lowestUnderHull(false);
    const float hullLow = lowestUnderHull(true);
    AVER_INFO("  lowest vertex over the slab: {:.3f} cm free, {:.3f} cm with the hull", freeLow, hullLow);
    check(freeLow < -3.0f, "without the hull the strands droop below the slab's top plane");
    check(hullLow > -2.0f - 0.5f, "with the hull no vertex over it sinks more than 5 mm below its top (the hair radius is 1 mm)");
}

static void testWind() {
    AVER_INFO("wind: monotone, and zero wind is the same as none");
    const Groom g = makeGroom(4, 4, 8, 1.5f);
    auto tipY = [&](float windCmS, bool callSetWind, int32_t* windFlag) {
        freshWorld();
        const int32_t settings = buildSettings(g, Options());
        const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
        if (callSetWind) {
            const float w[3] = {0.0f, windCmS, 0.0f};
            aver_phys_hair_set_wind(hair, w, 4.0f, 0.0f);
        }
        stepHair(hair, 90);
        int32_t ui[AVER_PHYS_HAIRUI_COUNT] = {};
        aver_phys_hair_info(hair, ui, AVER_PHYS_HAIRUI_COUNT, nullptr, 0);
        if (windFlag) *windFlag = ui[AVER_PHYS_HAIRUI_WIND];
        const std::vector<float> sim = readSim(hair, 16 * 8);
        float sum = 0.0f;
        for (int s = 0; s < 16; ++s) sum += sim[(static_cast<size_t>(s) * 8 + 7) * 3 + 1];
        return sum / 16.0f;
    };
    int32_t flag = 0;
    const float none = tipY(0.0f, false, nullptr);
    const float zero = tipY(0.0f, true, &flag);
    check(flag == 0, "zero wind records no wind kernel");
    check(std::memcmp(&none, &zero, sizeof none) == 0, "zero wind is bit identical to no wind call");
    const float w300 = tipY(300.0f, true, &flag);
    check(flag == 1, "a wind records the kernel");
    const float w600 = tipY(600.0f, true, nullptr);
    AVER_INFO("  mean tip y: none {:.3f}, 300 cm/s {:.3f}, 600 cm/s {:.3f}", none, w300, w600);
    check(w300 > none + 0.05f, "a wind along +Y moves the tips along +Y");
    check(w600 > w300, "a stronger wind moves them further (monotone)");

    freshWorld();
    const int32_t settings = buildSettings(g, Options());
    const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
    const float nan3[3] = {std::nanf(""), 0, 0};
    check(aver_phys_hair_set_wind(hair, nan3, 1.0f, 0.0f) == 0, "set_wind refuses a non-finite wind");
    const float ok3[3] = {1, 0, 0};
    check(aver_phys_hair_set_wind(hair, ok3, -1.0f, 0.0f) == 0, "set_wind refuses a negative drag");
}

// One run of the groom on the current backend, returning the simulated positions after n steps.
static std::vector<float> runGroom(const Groom& g, int n) {
    const int32_t settings = buildSettings(g, Options());
    const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
    if (!settings || !hair) return {};
    const float w[3] = {0.0f, 200.0f, 0.0f};
    aver_phys_hair_set_wind(hair, w, 3.0f, 50.0f);
    stepHair(hair, n);
    std::vector<float> out = readSim(hair, g.strands * g.verts);
    aver_phys_hair_destroy(hair);
    aver_phys_hair_settings_destroy(settings);
    return out;
}

static void testHostsAgree() {
    AVER_INFO("the fake host (the CPU backend behind the C table) agrees with the built-in CPU backend");
    JPH::RegisterDefaultAllocator();
    const Groom g = makeGroom(4, 4, 8, 1.5f);

    freshWorld();
    const std::vector<float> cpu = runGroom(g, 60);
    freshWorld();
    const std::vector<float> cpu2 = runGroom(g, 60);
    check(!cpu.empty() && cpu.size() == cpu2.size() && std::memcmp(cpu.data(), cpu2.data(), cpu.size() * sizeof(float)) == 0,
          "two runs on the CPU backend are bit identical (determinism)");

    freshWorld();
    FakeHost fake;
    AverPhysComputeHostV1 table = fake.table();
    check(aver_phys_hair_set_compute_host(&table) == 1, "the fake host installs");
    check(aver_phys_hair_host_kind() == 1, "host kind reports an installed host");
    AverPhysComputeHostV1 bad = table;
    bad.version = 2;
    check(aver_phys_hair_set_compute_host(&bad) == 0, "a host table of an unknown version is refused");
    const std::vector<float> viaHost = runGroom(g, 60);
    check(viaHost.size() == cpu.size() && !viaHost.empty(), "the groom ran through the table");
    check(fake.shaderCalls >= 15 && fake.bufferCalls > 0 && fake.bindCalls > 0 && fake.dispatches > 0 && fake.executes > 0,
          "the table saw shader, buffer, bind, dispatch and execute calls");
    check(fake.readbacks > 0, "positions were read back through the table");

    float worst = 0.0f;
    for (size_t i = 0; i < std::min(cpu.size(), viaHost.size()); ++i) worst = std::max(worst, std::fabs(cpu[i] - viaHost[i]));
    AVER_INFO("  worst difference CPU backend vs table: {:.6f} cm", worst);
    check(worst < 0.01f, "the two agree within 1e-4 m after 60 steps");

    // The render buffer handle exists with a host and is the host's own number.
    {
        const Groom small = makeGroom(2, 2, 5, 2.0f);
        const int32_t settings = buildSettings(small, Options());
        const int32_t hair = aver_phys_hair_create(settings, kIdentity, 0);
        check(aver_phys_hair_render_buffer(hair) != 0, "an installed host hands out the render positions buffer");
    }
    aver_phys_shutdown();
    check(aver_phys_hair_host_kind() == 0, "shutdown forgets the installed host");
    aver_phys_init();
}

int main() {
    AVER_INFO("HairTest");
    testSettings();
    testFalls();
    testTeleportAndTransform();
    testProxy();
    testWind();
    testHostsAgree();
    aver_phys_shutdown();
    if (g_failures != 0) {
        AVER_ERROR("HairTest: {} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("HairTest: all checks passed");
    return 0;
}
