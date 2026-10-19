// The GPU tier against the CPU executor, on a real device (WARP when asked for D3D12; `vulkan` for the Vulkan row).
//
//   1. layout probe       every GpuSimHeader field, a user value, a module row and a request row read back by index
//   2. RNG golden table   the 32 pinned (seed, stream) draws, hashed on the GPU
//   3. parity             60 fixed frames of an effect using all 13 modules, EmitterSim vs the kernels, matched by particle id
//   4. dead-list overflow spawns past capacity drop, nothing is corrupted
//   5. sort               5,000 random depths sorted by the bitonic kernels equal std::sort
//   6. events             a death event spawns child particles exactly one frame later
//
// Arguments: `vulkan` selects the Vulkan backend (default D3D12); `hw` uses the adapter instead of WARP (D3D12 only).
// Exits 0, 1, or 77 (ctest SKIPPED) when the backend is absent or lacks compute / indirect support.
#include "aver/core/Log.hpp"
#include "aver/particles/ParticleGpu.hpp"
#include "aver/particles/ParticleRandom.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::particles;

namespace {

int g_failures = 0;
constexpr int kSkip = 77;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok  {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

struct Rig {
    rhi::IDevice* dev = nullptr;
    rhi::IResourceFactory* res = nullptr;
    ParticleGpu gpu;
    ParticleGpu::View view;
    u32 records = 0;   // record() calls so far (the survivor region alternates with it)

    rhi::BufferHandle readbackBuffer(u64 bytes) {
        rhi::BufferDesc bd;
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "ParticleGpuParityTest readback";
        return res->createBuffer(bd);
    }
    // One frame's compute for whatever jobs `sys` produced.
    bool frame(ParticleSystem& sys) {
        const bool ok = dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) { gpu.record(ctx, sys.gpuJobs(), view); });
        sys.gpuJobs().clear();
        ++records;
        return ok;
    }
    template <class T>
    bool dump(u32 handle, ParticleGpu::Dump what, std::vector<T>& out) {
        const u64 bytes = gpu.dumpBytes(handle, what);
        if (!bytes) return false;
        const rhi::BufferHandle rb = readbackBuffer(bytes);
        if (!rb) return false;
        const bool ran = dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) { gpu.dump(ctx, handle, what, rb); });
        out.assign(static_cast<usize>(bytes / sizeof(T)), T{});
        const bool read = ran && res->readBuffer(rb, out.data(), bytes, 0);
        res->destroyBuffer(rb);
        return read;
    }
};

u32 handleOf(const ParticleSystem& sys, usize emitterIndex) {
    u32 h = 0;
    sys.forEachInstance([&](const InstanceView& v) { if (v.emitterIndex == emitterIndex) h = v.gpuHandle; });
    return h;
}

scene::Entity makeEmitter(scene::World& world, u64 effect, u32 seed) {
    const scene::Entity e = world.create("ParticleGpuParityTest.Emitter");
    auto* c = static_cast<scene::CParticleEmitter*>(world.addComponent(e, scene::kComponentParticleEmitter));
    if (c) { c->effect = effect; c->seed = seed; }
    return e;
}

// ---------------------------------------------------------------------------------------------------------- 1 and 2
void testProbes(Rig& rig, scene::World& world) {
    AVER_INFO("--- layout probe and RNG golden table ---");
    ParticleEffectLibrary lib;
    ParticleEffect fx;
    fx.simTier = SimTier::Gpu;
    fx.maxParticles = 256;
    lib.set(1, fx);
    ParticleSystem sys;
    sys.setEffectLibrary(&lib);
    sys.setGpuTier(&rig.gpu);
    makeEmitter(world, 1, 5);
    sys.tick(world, 0.016f);
    const u32 h = handleOf(sys, 0);
    check(h != 0, "the effect runs on the GPU tier");
    if (!h) return;

    GpuSimCB cb;
    u32* words = reinterpret_cast<u32*>(&cb);
    for (usize i = 0; i < sizeof(GpuSimCB) / 4; ++i) words[i] = 0x10000000u + static_cast<u32>(i) * 7u;
    const rhi::BufferHandle rb = rig.readbackBuffer(64 * 4);
    u32 got[64] = {};
    bool ran = rig.dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) { rig.gpu.probe(ctx, h, cb, rb); });
    check(ran && rig.res->readBuffer(rb, got, sizeof got, 0), "CSProbe runs and reads back");
    const u32 expect[] = {
        // the 48 header words in order, then the sampled rows
        words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9], words[10], words[11],
        words[12], words[13], words[14], words[15], words[16], words[17], words[18], words[19], words[20], words[21], words[22], words[23],
        words[24], words[25], words[26], words[27], words[28], words[29], words[30], words[31], words[32], words[33], words[34], words[35],
        words[36], words[37], words[38], words[39], words[40], words[41], words[42], words[43], words[44], words[45], words[46], words[47],
        words[48], words[51], words[108], words[111],
        words[112], words[115], words[300], words[303],
        words[304], words[307], words[2092], words[2095],
        words[351], words[359],
    };
    int firstBad = -1;
    for (int i = 0; i < 62; ++i)
        if (got[i] != expect[i]) { firstBad = i; break; }
    if (firstBad >= 0) AVER_ERROR("  probe word {} read {:#x}, expected {:#x}", firstBad, got[firstBad], expect[firstBad]);
    check(firstBad < 0, "every probed field of GpuSimCB reads back at its C++ offset (62 words)");
    rig.res->destroyBuffer(rb);

    // The RNG: kRandGolden's 32 pairs go in as the first 32 float4s of the request block.
    GpuSimCB rc;
    std::memset(&rc, 0, sizeof rc);
    u32* rw = reinterpret_cast<u32*>(reinterpret_cast<u8*>(&rc) + offsetof(GpuSimCB, req));
    for (u32 i = 0; i < 32; ++i) { rw[i * 4 + 0] = kRandGolden[i].seed; rw[i * 4 + 1] = kRandGolden[i].stream; }
    const rhi::BufferHandle rb2 = rig.readbackBuffer(32 * 4);
    u32 hashes[32] = {};
    ran = rig.dev->runStandaloneCompute([&](rhi::IRenderContext& ctx) { rig.gpu.probeRng(ctx, h, rc, rb2); });
    check(ran && rig.res->readBuffer(rb2, hashes, sizeof hashes, 0), "CSRngProbe runs and reads back");
    int badRng = -1;
    for (u32 i = 0; i < 32; ++i)
        if (hashes[i] != kRandGolden[i].bits24) { badRng = static_cast<int>(i); break; }
    check(badRng < 0, "the GPU draws the same 32 golden random values as the CPU");
    rig.res->destroyBuffer(rb2);
}

// ---------------------------------------------------------------------------------------------------------- 3
ParticleSystemDef parityDef(SimTier tier) {
    ParticleSystemDef def;
    ParticleEffect fx;
    fx.simTier = tier;
    fx.shape = EmitterShape::Sphere;
    fx.shapeSize = {40, 0, 0};
    fx.emissionRate = 1500.0f;
    fx.burstCount = 800;
    fx.maxParticles = 4096;
    fx.lifetimeMin = 1.0f; fx.lifetimeMax = 2.0f;
    fx.direction = {0, 0, 1};
    fx.spreadDeg = 40.0f;
    fx.speedMin = 80.0f; fx.speedMax = 160.0f;
    fx.gravity = {0, 0, -120.0f};
    fx.damping = 0.05f;
    auto add = [&](ModuleKind k, std::initializer_list<std::pair<const char*, f32>> params) {
        Module m = makeDefaultModule(k);
        const ModuleInfo& info = moduleInfo(k);
        for (const auto& kv : params) { const int i = findModuleParam(info, kv.first); if (i >= 0) m.p[i].value = kv.second; }
        fx.modules.push_back(m);
    };
    add(ModuleKind::SpawnSizeScale, {{"min", 0.6f}, {"max", 1.4f}});
    add(ModuleKind::SpawnColorTint, {{"r0", 1.0f}, {"g0", 0.5f}, {"b0", 0.2f}, {"r1", 0.2f}, {"g1", 0.5f}, {"b1", 1.0f}});
    add(ModuleKind::SpawnRotation, {{"minDeg", 0}, {"maxDeg", 180}, {"spinMin", -90}, {"spinMax", 90}});
    add(ModuleKind::SpawnInheritVelocity, {{"scale", 0.5f}});
    add(ModuleKind::UpdateForce, {{"x", 30}, {"y", -10}, {"z", 0}});
    add(ModuleKind::UpdateDrag, {{"k", 0.0005f}});
    add(ModuleKind::UpdateAttractor, {{"x", 0}, {"y", 0}, {"z", 120}, {"strength", 60}, {"radius", 400}, {"falloff", 1.5f}, {"local", 1}});
    add(ModuleKind::UpdateVortex, {{"strength", 40}, {"radius", 300}, {"lift", 10}});
    add(ModuleKind::UpdateCurlNoise, {{"freq", 0.01f}, {"strength", 60}, {"scroll", 0.3f}, {"octaves", 2}, {"seed", 3}});
    add(ModuleKind::UpdateSpeedLimit, {{"max", 400}});
    add(ModuleKind::UpdateCollidePlane, {{"nz", 1}, {"d", -60}, {"restitution", 0.5f}, {"friction", 0.2f}});
    add(ModuleKind::UpdateCollideSphere, {{"x", 0}, {"y", 0}, {"z", 0}, {"r", 25}, {"restitution", 0.6f}});
    add(ModuleKind::UpdateKillVolume, {{"hx", 90}, {"hy", 90}, {"hz", 150}, {"invert", 1}});
    def.emitters.push_back(fx);
    return def;
}

void testParity(Rig& rig, scene::World& world) {
    AVER_INFO("--- 60 frames, every module: EmitterSim vs the kernels ---");
    ParticleEffectLibrary libCpu, libGpu;
    libCpu.setSystem(10, parityDef(SimTier::Cpu));
    libGpu.setSystem(10, parityDef(SimTier::Gpu));
    ParticleSystem cpu, gpuSys;
    cpu.setEffectLibrary(&libCpu);
    gpuSys.setEffectLibrary(&libGpu);
    gpuSys.setGpuTier(&rig.gpu);
    const scene::Entity ec = makeEmitter(world, 10, 4242);
    makeEmitter(world, 10, 4242);   // a second entity ticked by the GPU system only; the CPU system sees both, so give it a twin
    (void)ec;

    // Two entities share the world, so each system sees both; compare totals by id within one emitter instance instead:
    // the CPU system's first instance and the GPU system's first instance were seeded identically (c->seed 4242).
    const f32 dt = 1.0f / 60.0f;
    bool allRan = true;
    // The two systems tick the SAME world, and a tick advances the component's own clock (age) and spawn carry (emitAccum).
    // Left alone the second system would read a clock the first had already moved, and a module that reads time (curl noise
    // scroll) would see 3 dt where the CPU saw 2 dt. Each system gets the component state as it stood before the frame.
    struct Clock { scene::CParticleEmitter* c; f32 age, accum; };
    std::vector<Clock> clocks;
    if (scene::ComponentPool* pool = world.pool(scene::kComponentParticleEmitter))
        for (usize i = 0; i < pool->size(); ++i) {
            auto* c = static_cast<scene::CParticleEmitter*>(pool->dataAt(i));
            if (c && c->effect == 10) clocks.push_back({c, 0.0f, 0.0f});
        }
    for (int f = 0; f < 60; ++f) {
        for (Clock& k : clocks) { k.age = k.c->age; k.accum = k.c->emitAccum; }
        cpu.tick(world, dt);
        for (const Clock& k : clocks) { k.c->age = k.age; k.c->emitAccum = k.accum; }
        gpuSys.tick(world, dt);
        allRan = rig.frame(gpuSys) && allRan;
    }
    check(allRan, "60 GPU frames recorded");

    // Gather the CPU particles of both instances by (instance, id) and the GPU ones the same way.
    std::vector<u32> cpuHandles;
    std::vector<const std::vector<Particle>*> cpuSets;
    cpu.forEachInstance([&](const InstanceView& v) { if (v.particles) cpuSets.push_back(v.particles); });
    std::vector<u32> gpuHandles;
    gpuSys.forEachInstance([&](const InstanceView& v) { if (v.gpuHandle) gpuHandles.push_back(v.gpuHandle); });
    check(cpuSets.size() == 2 && gpuHandles.size() == 2, "both systems hold two instances (CPU and GPU tier respectively)");
    if (cpuSets.size() != 2 || gpuHandles.size() != 2) return;

    f32 worst = 0.0f;
    u32 matched = 0, cpuTotal = 0, gpuTotal = 0;
    for (usize k = 0; k < 2; ++k) {
        std::vector<Particle> gp;
        std::vector<u32> counters, survivors;
        const bool ok = rig.dump(gpuHandles[k], ParticleGpu::Dump::Particles, gp) &&
                        rig.dump(gpuHandles[k], ParticleGpu::Dump::Counters, counters) &&
                        rig.dump(gpuHandles[k], ParticleGpu::Dump::SurvivorList, survivors);
        check(ok, "the GPU particles, counters and survivor list read back");
        if (!ok) return;
        const u32 aliveNext = counters[kCtrAliveNext];
        const u32 C = static_cast<u32>(survivors.size() / 3);
        const u32 region = rig.records % 2;   // after N records the survivors sit in list N mod 2
        std::map<u32, const Particle*> byId;
        for (u32 i = 0; i < aliveNext && i < C; ++i) {
            const u32 slot = survivors[region * C + i];
            if (slot < gp.size()) byId[gp[slot].id] = &gp[slot];
        }
        cpuTotal += static_cast<u32>(cpuSets[k]->size());
        gpuTotal += static_cast<u32>(byId.size());
        for (const Particle& c : *cpuSets[k]) {
            auto it = byId.find(c.id);
            if (it == byId.end()) continue;
            ++matched;
            const Particle& g = *it->second;
            const Vec3 d = g.position - c.position;
            const f32 tol = std::fmax(1e-3f * c.position.size(), 0.01f);
            worst = std::fmax(worst, d.size() / tol);
        }
    }
    AVER_INFO("  CPU alive {}, GPU alive {}, matched by id {}, worst position error {:.3f} x tolerance", cpuTotal, gpuTotal, matched, static_cast<f64>(worst));
    check(cpuTotal > 500, "the effect is alive after 60 frames");
    const f32 countDiff = std::fabs(static_cast<f32>(cpuTotal) - static_cast<f32>(gpuTotal)) / std::fmax(1.0f, static_cast<f32>(cpuTotal));
    check(countDiff <= 0.01f, "alive counts agree within 1% (kill-volume boundary cases may flip on floating-point differences)");
    check(static_cast<f32>(matched) >= 0.99f * static_cast<f32>(std::min(cpuTotal, gpuTotal)), "at least 99% of particles exist on both sides by id");
    check(worst <= 1.0f, "positions agree within max(1e-3 |p|, 0.01) cm");
}

// ---------------------------------------------------------------------------------------------------------- 4
void testOverflow(Rig& rig, scene::World& world) {
    AVER_INFO("--- dead-list overflow ---");
    (void)world;
    ParticleEffectLibrary lib;
    ParticleEffect fx;
    fx.simTier = SimTier::Gpu;
    fx.maxParticles = 256;
    fx.emissionRate = 0.0f;
    fx.lifetimeMin = fx.lifetimeMax = 100.0f;
    fx.speedMin = fx.speedMax = 0.0f;
    lib.set(20, fx);
    ParticleSystem sys;
    sys.setEffectLibrary(&lib);
    sys.setGpuTier(&rig.gpu);
    SpawnAtParams p;
    p.effect = 20;
    p.count = 256;
    sys.spawnAt(p);
    sys.spawnAt(p);   // two requests of 256 into a 256-slot emitter
    sys.tick(scene::World::instance(), 0.016f);
    const u32 h = handleOf(sys, 0);
    check(h != 0, "the spawn-at event instance runs on the GPU tier");
    if (!h) return;
    check(rig.frame(sys), "the overflowing frame records");
    std::vector<u32> counters, survivors;
    std::vector<Particle> parts;
    check(rig.dump(h, ParticleGpu::Dump::Counters, counters) && rig.dump(h, ParticleGpu::Dump::SurvivorList, survivors) &&
              rig.dump(h, ParticleGpu::Dump::Particles, parts), "counters, survivors and particles read back");
    if (counters.size() < kGpuCounterCount) return;
    check(counters[kCtrAliveNext] == 256, "exactly capacity particles survive");
    check(counters[kCtrDropped] == 256, "the other 256 spawns were dropped, not wrapped");
    const u32 C = static_cast<u32>(survivors.size() / 3);
    std::map<u32, int> ids, slots;
    bool inRange = true;
    for (u32 i = 0; i < counters[kCtrAliveNext]; ++i) {
        const u32 slot = survivors[1u * C + i];   // one record ran: the survivors sit in list 1
        inRange = inRange && slot < 256;
        ++slots[slot];
        if (slot < parts.size()) ++ids[parts[slot].id];
    }
    check(inRange && slots.size() == 256, "256 distinct slots in the survivor list");
    check(ids.size() == 256, "256 distinct particle ids: no slot was handed out twice");
}

// ---------------------------------------------------------------------------------------------------------- 5
void testSort(Rig& rig, scene::World& world) {
    AVER_INFO("--- bitonic sort of 5,000 random depths ---");
    ParticleEffectLibrary lib;
    ParticleEffect fx;
    fx.simTier = SimTier::Gpu;
    fx.shape = EmitterShape::Box;
    fx.shapeSize = {500, 500, 500};
    fx.emissionRate = 0.0f;
    fx.burstCount = 5000;
    fx.maxParticles = 6000;
    fx.lifetimeMin = fx.lifetimeMax = 100.0f;
    fx.speedMin = fx.speedMax = 0.0f;
    lib.set(30, fx);
    ParticleSystem sys;
    sys.setEffectLibrary(&lib);
    sys.setGpuTier(&rig.gpu);
    makeEmitter(world, 30, 99);
    sys.tick(world, 0.016f);
    const u32 h = handleOf(sys, 0);
    check(h != 0, "the sorted emitter runs on the GPU tier");
    if (!h) return;
    // depth = x + 1000: viewRow (1, 0, 0, 1000) in the 4th column of a row-major view-projection.
    Rig& r = rig;
    f32 saved[16];
    std::memcpy(saved, r.view.viewProj, sizeof saved);
    std::memset(r.view.viewProj, 0, sizeof r.view.viewProj);
    r.view.viewProj[3] = 1.0f;
    r.view.viewProj[15] = 1000.0f;
    r.view.valid = true;
    check(r.frame(sys), "the sort frame records");
    std::vector<Particle> parts;
    std::vector<u32> counters, survivors;
    check(r.dump(h, ParticleGpu::Dump::Particles, parts) && r.dump(h, ParticleGpu::Dump::Counters, counters) &&
              r.dump(h, ParticleGpu::Dump::SurvivorList, survivors), "particles, counters and the sorted list read back");
    std::memcpy(r.view.viewProj, saved, sizeof saved);
    if (counters.size() < kGpuCounterCount) return;
    const u32 n = counters[kCtrAliveNext];
    check(n == 5000, "5,000 survivors");
    const u32 C = static_cast<u32>(survivors.size() / 3);
    std::vector<std::pair<u32, u32>> want;
    for (u32 slot = 0; slot < parts.size() && want.size() < n; ++slot) {
        if (!particleAlive(parts[slot]) || parts[slot].lifetime <= 0.0f) continue;
        const f32 depth = std::fmax(parts[slot].position.x + 1000.0f, 0.0f);
        u32 bits;
        std::memcpy(&bits, &depth, 4);
        want.push_back({std::min(~bits, 0xFFFFFFFEu), slot});
    }
    std::sort(want.begin(), want.end());
    bool same = want.size() == n;
    u32 firstDiff = ~0u;
    for (u32 i = 0; i < n && same; ++i)
        if (survivors[2 * C + i] != want[i].second) { same = false; firstDiff = i; }
    if (!same) AVER_ERROR("  sorted order differs from std::sort at position {}", firstDiff);
    check(same, "the sorted-order region equals std::sort by (key, slot), farthest first");
}

// ---------------------------------------------------------------------------------------------------------- 6
void testEvents(Rig& rig, scene::World& world) {
    AVER_INFO("--- a death event spawns children exactly one frame later ---");
    ParticleEffectLibrary lib;
    ParticleSystemDef def;
    ParticleEffect parent, child;
    parent.name = "Parent"; child.name = "Child";
    parent.simTier = child.simTier = SimTier::Gpu;
    parent.emissionRate = 0.0f; parent.burstCount = 100; parent.maxParticles = 256;
    parent.lifetimeMin = parent.lifetimeMax = 0.125f;
    parent.speedMin = parent.speedMax = 0.0f;
    child.emissionRate = 0.0f; child.burstCount = 0; child.maxParticles = 1024;
    child.lifetimeMin = child.lifetimeMax = 50.0f;
    child.speedMin = child.speedMax = 0.0f;
    EventSpawn ev; ev.on = 0; ev.emitter = 1; ev.count = 2;
    parent.events.push_back(ev);
    def.emitters = {parent, child};
    lib.setSystem(40, def);
    ParticleSystem sys;
    sys.setEffectLibrary(&lib);
    sys.setGpuTier(&rig.gpu);
    makeEmitter(world, 40, 7);

    sys.tick(world, 0.125f);
    const u32 hp = handleOf(sys, 0), hc = handleOf(sys, 1);
    check(hp != 0 && hc != 0, "parent and child both run on the GPU tier");
    if (!hp || !hc) return;
    check(rig.frame(sys), "frame 1 records");
    std::vector<u32> cc;
    check(rig.dump(hc, ParticleGpu::Dump::Counters, cc), "the child's counters read back");
    check(cc.size() >= kGpuCounterCount && cc[kCtrAliveNext] == 0, "frame 1: the child holds nothing yet");
    sys.tick(world, 0.125f);
    check(rig.frame(sys), "frame 2 records");
    check(rig.dump(hc, ParticleGpu::Dump::Counters, cc), "the child's counters read back again");
    check(cc.size() >= kGpuCounterCount && cc[kCtrAliveNext] == 200, "frame 2: 100 deaths x 2 children = 200 child particles");
}

} // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== ParticleGpuParityTest ===");
    bool wantVulkan = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "vulkan") == 0) wantVulkan = true;
        if (std::strcmp(argv[i], "hw") == 0) hardware = true;
    }
    rhi::DeviceDesc desc;
    desc.useWarp = !hardware;
    desc.preferred[0] = wantVulkan ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no {} device on this machine", wantVulkan ? "Vulkan" : "D3D12");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    const rhi::DeviceCaps caps = dev->caps();
    if (!dev->resources() || !caps.computeShaders || !caps.indirect || !dev->runStandaloneCompute([](rhi::IRenderContext&) {})) {
        AVER_WARN("  SKIP  this backend cannot run standalone compute with indirect dispatch");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {} ({})", dev->adapterName(), rhi::backendName(dev->backend()));

    {
        Rig rig;
        rig.dev = dev;
        rig.res = dev->resources();
        const bool ready = rig.gpu.init(*dev);
        check(ready, "the GPU tier builds its kernels (particles_sim.hlsl, particles_sort.hlsl)");
        if (ready) {
            scene::World& world = scene::World::instance();
            testProbes(rig, world);
            testParity(rig, world);
            testOverflow(rig, world);
            testSort(rig, world);
            testEvents(rig, world);
        }
        rig.res->waitIdle();
        rig.gpu.shutdown();
    }
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== ParticleGpuParityTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== ParticleGpuParityTest PASSED ===");
    return 0;
}
