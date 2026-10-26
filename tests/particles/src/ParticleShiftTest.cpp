// The floating origin and particles (docs/plans/wave11/LARGE_WORLD.md 2.2.5, R-4). No device: the CPU tier is shifted and
// read back directly; the GPU tier's kernel (CSShift in particles_sim.hlsl) is held to its CPU mirrors in
// ParticleGpuTypes.hpp, which are in turn held to EmitterSim::shiftWorld, and the HLSL text is read to confirm it is the
// arithmetic the mirrors state. A device run of CSShift is the owner's headed pass (section 6). Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/particles/EmitterSim.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleGpuTypes.hpp"
#include "aver/particles/ParticleModules.hpp"
#include "aver/particles/ParticleSpawn.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/rhi/WorldShift.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::particles;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool bitEqual(f32 a, f32 b) { return std::memcmp(&a, &b, sizeof a) == 0; }

static ParticleEffect restingEffect() {
    ParticleEffect fx;
    fx.emissionRate = 0.0f;
    fx.burstCount = 3;
    fx.maxParticles = 64;
    fx.lifetimeMin = fx.lifetimeMax = 100.0f;
    fx.direction = {0, 0, 1};
    fx.spreadDeg = 0.0f;
    fx.speedMin = fx.speedMax = 0.0f;   // particles sit where they were born
    fx.gravity = {0, 0, 0};
    return fx;
}

static scene::Entity emitterAt(scene::World& world, u64 effectId, const Vec3& p) {
    const scene::Entity e = world.create("ParticleShiftTest.Emitter");
    auto* c = static_cast<scene::CParticleEmitter*>(world.addComponent(e, scene::kComponentParticleEmitter));
    check(c != nullptr, "CParticleEmitter attaches");
    if (c) c->effect = effectId;
    world.setLocalPosition(e, p);
    return e;
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    AVER_INFO("ParticleShiftTest");
    scene::World& world = scene::World::instance();
    const rhi::ShiftVec3d oneRegion{1'638'400.0, 0.0, 0.0};

    // ---- (1) the CPU tier: live particles move by exactly -delta, and the rebased emitter spawns where it stands ----
    {
        ParticleEffectLibrary lib;
        lib.set(1, restingEffect());
        const Vec3 start{2'000'000.0f, 64.0f, 500.0f};   // beyond the first region seam
        const scene::Entity e = emitterAt(world, 1, start);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        sys.tick(world, 0.016f);
        const std::vector<Particle>* ps = sys.particles(e);
        check(ps && ps->size() == 3, "the burst spawned three particles");
        std::vector<Particle> before = ps ? *ps : std::vector<Particle>{};

        sys.shiftWorld(oneRegion);
        world.shiftRoots(Vec3{1'638'400.0f, 0.0f, 0.0f});

        ps = sys.particles(e);
        bool exact = ps && ps->size() == before.size();
        for (usize i = 0; exact && i < before.size(); ++i) {
            exact = bitEqual((*ps)[i].position.x, before[i].position.x - 1'638'400.0f) &&
                    bitEqual((*ps)[i].position.y, before[i].position.y) && bitEqual((*ps)[i].position.z, before[i].position.z) &&
                    bitEqual((*ps)[i].velocity.x, before[i].velocity.x);
        }
        check(exact, "every live particle is exactly `position - delta` in f32; velocity and the other axes are untouched");

        // The emitter itself moved with the scene, so the next tick's particles are born at the new local position.
        sys.burst(e, 4);
        sys.tick(world, 0.016f);
        ps = sys.particles(e);
        bool near = ps && ps->size() == 7;
        for (usize i = 3; near && ps && i < ps->size(); ++i)
            near = std::fabs((*ps)[i].position.x - (start.x - 1'638'400.0f)) < 1e-2f && std::fabs((*ps)[i].position.y - start.y) < 1e-2f;
        check(near, "a burst after the rebase spawns at the emitter's shifted position");
    }

    // ---- (2) velocity continuity: prevOrigin moves with the origin, so a still emitter is not suddenly "moving" ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx = restingEffect();
        fx.burstCount = 0;
        Module inh = makeDefaultModule(ModuleKind::SpawnInheritVelocity);
        inh.p[0].value = 1.0f;   // the whole emitter velocity
        fx.modules.push_back(inh);
        lib.set(2, fx);
        const scene::Entity e = emitterAt(world, 2, Vec3{1000.0f, 0.0f, 0.0f});
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        sys.tick(world, 0.016f);   // havePrev becomes true
        // A delta far below a region step: the emitter's velocity is a difference of two origins, 3125 cm/s if one is stale,
        // and a region step would be thrown away by the teleport guard whether or not prevOrigin were shifted.
        const rhi::ShiftVec3d small{50.0, 0.0, 0.0};
        sys.shiftWorld(small);
        world.shiftRoots(Vec3{50.0f, 0.0f, 0.0f});
        sys.burst(e, 5);
        sys.tick(world, 0.016f);
        const std::vector<Particle>* ps = sys.particles(e);
        bool still = ps && ps->size() == 5;
        for (usize i = 0; still && i < ps->size(); ++i) still = std::fabs((*ps)[i].velocity.x) < 1e-3f;
        check(still, "a particle born after the shift inherits ~0 velocity: prevOrigin was shifted with origin");
    }

    // ---- (3) a queued spawn-at request is in the old frame until shifted ----
    {
        ParticleEffectLibrary lib;
        ParticleEffect fx = restingEffect();
        fx.burstCount = 0;
        lib.set(3, fx);
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        SpawnAtParams sp;
        sp.effect = 3;
        sp.position = Vec3{2'000'000.0f, 0.0f, 100.0f};
        sp.count = 2;
        check(sys.spawnAt(sp) == 2, "spawnAt accepted two particles");
        sys.shiftWorld(oneRegion);   // before the tick that would consume it
        sys.tick(world, 0.016f);
        bool found = false, right = true;
        sys.forEachInstance([&](const InstanceView& v) {
            if (v.entity != scene::kInvalidEntity || !v.particles) return;
            for (const Particle& p : *v.particles) {
                found = true;
                right = right && std::fabs(p.position.x - (2'000'000.0f - 1'638'400.0f)) < 1e-2f;
            }
        });
        check(found && right, "a spawn-at queued before the shift lands at the shifted position");
    }

    // ---- (4) zero delta: nothing changes, bit for bit ----
    {
        EmitterSim sim;
        ParticleEffect fx = restingEffect();
        sim.init(fx, {}, 7);
        SpawnRequest r;
        r.xf = Mat4::translation(Vec3{123.456f, -78.9f, 1.0f});
        r.count = 4;
        sim.spawn(r);
        const std::vector<Particle> a = sim.particles();
        ParticleSystem sys;
        sys.shiftWorld(rhi::ShiftVec3d{});
        sim.shiftWorld(Vec3{0.0f, 0.0f, 0.0f});
        bool same = sim.particles().size() == a.size();
        for (usize i = 0; same && i < a.size(); ++i) same = std::memcmp(&a[i], &sim.particles()[i], sizeof(Particle)) == 0;
        check(same, "a zero shift leaves every particle bit-identical");
        check(sys.gpuShifts().empty(), "a zero shift queues no GPU work");
    }

    // ---- (5) trail points: the position shifts, the age in w does not ----
    {
        ParticleEffect fx = restingEffect();
        fx.render = RenderKind::Ribbon;
        fx.linked = false;
        fx.trailPoints = 4;
        fx.trailSegmentCm = 1.0f;
        fx.speedMin = fx.speedMax = 100.0f;
        EmitterSim sim;
        sim.init(fx, {}, 9);
        SpawnRequest r;
        r.xf = Mat4::translation(Vec3{2'000'000.0f, 0.0f, 0.0f});
        r.count = 2;
        sim.spawn(r);
        for (int i = 0; i < 6; ++i) sim.step(0.05f, 0.05f * static_cast<f32>(i), nullptr);
        std::vector<Vec4> before = sim.trail();
        check(!before.empty(), "a trail ribbon keeps trail points");
        sim.shiftWorld(Vec3{1'638'400.0f, 0.0f, 0.0f});
        bool ok = sim.trail().size() == before.size();
        for (usize i = 0; ok && i < before.size(); ++i)
            ok = bitEqual(sim.trail()[i].x, before[i].x - 1'638'400.0f) && bitEqual(sim.trail()[i].y, before[i].y) &&
                 bitEqual(sim.trail()[i].w, before[i].w);
        check(ok, "every trail point shifts and keeps its age");
    }

    // ---- (6) the GPU kernel's CPU mirrors equal the CPU tier, and the events buffer layout is the one the kernel walks ----
    {
        const f32 delta[3] = {1'638'400.0f, 0.0f, 0.0f};
        std::vector<Particle> gpu(8), cpu;
        for (usize i = 0; i < gpu.size(); ++i) {
            gpu[i].position = Vec3{2'000'000.0f + static_cast<f32>(i) * 0.25f, static_cast<f32>(i), -3.0f};
            gpu[i].velocity = Vec3{1.0f, 2.0f, 3.0f};
        }
        cpu = gpu;
        gpuShiftParticles(gpu.data(), gpu.size(), delta);
        for (Particle& p : cpu) p.position -= Vec3{delta[0], delta[1], delta[2]};
        bool same = true;
        for (usize i = 0; i < gpu.size(); ++i) same = same && std::memcmp(&gpu[i], &cpu[i], sizeof(Particle)) == 0;
        check(same, "gpuShiftParticles equals position - delta on exactly the position fields");

        std::vector<u32> ev(static_cast<usize>(kGpuEventRegionUints) * 2u, 0u);
        const auto putRecord = [&](u32 region, u32 type, u32 index, f32 x, f32 y, f32 z) {
            u32* base = ev.data() + region * kGpuEventRegionUints;
            u32* rec = base + kGpuEventHeaderUints + (type == 0 ? 0u : kGpuEventCapacity * kGpuEventRecordUints) + index * kGpuEventRecordUints;
            std::memcpy(&rec[0], &x, 4); std::memcpy(&rec[1], &y, 4); std::memcpy(&rec[2], &z, 4);
            rec[3] = 77u;
            const f32 vx = 9.0f;
            std::memcpy(&rec[4], &vx, 4);
        };
        putRecord(0, 0, 0, 2'000'000.0f, 1.0f, 2.0f);
        putRecord(0, 0, 1, 2'000'001.0f, 1.0f, 2.0f);
        putRecord(1, 1, 0, 2'000'002.0f, 5.0f, 6.0f);
        putRecord(1, 0, 3, 3.0f, 3.0f, 3.0f);   // beyond the count: stale, must not move
        ev[0 * kGpuEventRegionUints + 0] = 2u;    // region 0: two death records
        ev[1 * kGpuEventRegionUints + 1] = 1u;    // region 1: one collide record
        const std::vector<u32> evBefore = ev;
        gpuShiftEvents(ev.data(), delta);
        const auto pos = [&](const std::vector<u32>& b, u32 region, u32 type, u32 index, int axis) {
            const u32* rec = b.data() + region * kGpuEventRegionUints + kGpuEventHeaderUints +
                             (type == 0 ? 0u : kGpuEventCapacity * kGpuEventRecordUints) + index * kGpuEventRecordUints;
            f32 v;
            std::memcpy(&v, &rec[axis], 4);
            return v;
        };
        check(bitEqual(pos(ev, 0, 0, 0, 0), pos(evBefore, 0, 0, 0, 0) - delta[0]) && bitEqual(pos(ev, 0, 0, 1, 0), pos(evBefore, 0, 0, 1, 0) - delta[0]) &&
              bitEqual(pos(ev, 1, 1, 0, 0), pos(evBefore, 1, 1, 0, 0) - delta[0]),
              "the live event records of both regions and both types shift");
        check(ev[0 * kGpuEventRegionUints + kGpuEventHeaderUints + 3] == 77u &&
              std::memcmp(&ev[1 * kGpuEventRegionUints + kGpuEventHeaderUints + 3 * kGpuEventRecordUints],
                          &evBefore[1 * kGpuEventRegionUints + kGpuEventHeaderUints + 3 * kGpuEventRecordUints], 32) == 0,
              "ids, velocities and records past the count are left alone");

        std::vector<Vec4> trail = {Vec4{2'000'000.0f, 1.0f, 2.0f, 0.5f}, Vec4{2'000'010.0f, 1.0f, 2.0f, 0.25f}};
        gpuShiftTrail(trail.data(), trail.size(), delta);
        check(bitEqual(trail[0].x, 2'000'000.0f - delta[0]) && bitEqual(trail[1].w, 0.25f), "gpuShiftTrail keeps w");
    }

    // ---- (7) the HLSL is the arithmetic the mirrors state ----
#ifdef AVER_REPO_ROOT
    {
        const std::string hlsl = readFile(std::string(AVER_REPO_ROOT) + "/modules/particles/shaders/particles_sim.hlsl");
        const auto has = [&](const char* s) { return hlsl.find(s) != std::string::npos; };
        check(has("void CSShift(uint3 dtid : SV_DispatchThreadID)"), "particles_sim.hlsl declares CSShift");
        check(has("gParticles[i].position = pos - d;") && has("gTrail[i] = float4(t.xyz - d, t.w);"),
              "CSShift subtracts the delta from the particle and the trail point and keeps the trail age");
        check(has("min(gEvents[region + type], PEV_CAP)") && has("asfloat(gEvents[b + 0u]) - d.x"),
              "CSShift walks the live event records of both regions");
        check(has("const float3 d = gS.origin;"), "CSShift reads the delta from the header's origin field");
        const std::string cpp = readFile(std::string(AVER_REPO_ROOT) + "/modules/particles/src/ParticleGpu.cpp");
        check(cpp.find("csShift_ = kernel(sim.c_str(), \"CSShift\", layout)") != std::string::npos &&
              cpp.find("h.origin[0] = s.delta[0]") != std::string::npos,
              "ParticleGpu builds CSShift and passes the delta in the header's origin field");
    }
#endif

    AVER_INFO("ParticleShiftTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
