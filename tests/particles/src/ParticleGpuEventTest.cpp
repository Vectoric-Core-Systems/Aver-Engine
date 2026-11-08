// The GPU tier's event triggers, held without a device: which event channel each rule writes, which systems stay on the GPU (a fake IParticleGpuTier
// stands in for the pool), the cbuffer and events-buffer layout against particles_common.hlsli / particles_sim.hlsl, and the payload arithmetic the
// kernel mirrors against EmitterSim's event record. What the kernels then do on a device is ParticleGpuParityTest's. Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/particles/EmitterSim.hpp"
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleScalability.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace aver;
using namespace aver::particles;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A pool that hands out handles, remembering what each was acquired for.
class FakeTier : public IParticleGpuTier {
public:
    bool usable() const override { return true; }
    u32 acquire(const ParticleEffect&, u32 parent) override { parents.push_back(parent); return static_cast<u32>(parents.size()); }
    void release(u32) override {}
    u32 aliveApprox(u32) const override { return 0; }
    u64 allocatedBytes() const override { return 0; }
    u32 capacityFor(const ParticleEffect& fx) const override { return fx.maxParticles; }
    std::vector<u32> parents;
};

static EventSpawn rule(EventOn on, u32 child, f32 at = 0.0f, u8 payload = 0, f32 user = 0.0f) {
    EventSpawn e;
    e.on = static_cast<u8>(on);
    e.emitter = child;
    e.count = 2;
    e.at = at;
    e.payload = payload;
    e.user = user;
    return e;
}

static ParticleEffect quiet(const char* name) {
    ParticleEffect fx;
    fx.name = name;
    fx.simTier = SimTier::Gpu;
    fx.emissionRate = 0.0f;
    fx.burstCount = 4;
    fx.maxParticles = 64;
    fx.lifetimeMin = fx.lifetimeMax = 2.0f;
    fx.speedMin = fx.speedMax = 0.0f;
    return fx;
}

static int defineOf(const std::string& text, const char* name) {
    const std::string key = std::string("#define ") + name + " ";
    const size_t at = text.find(key);
    if (at == std::string::npos) return -1;
    return std::atoi(text.c_str() + at + key.size());
}

int main() {
    AVER_INFO("ParticleGpuEventTest");
    scene::World& world = scene::World::instance();

    // ---- channels: death 0, collide 1, then one per firing trigger rule in rule order ----
    {
        ParticleEffect fx;
        fx.events = {rule(EventOn::Timer, 1, 0.5f), rule(EventOn::Death, 1), rule(EventOn::Lifetime, 1, 0.0f), rule(EventOn::Spawn, 1),
                     rule(EventOn::Lifetime, 1, 0.3f), rule(EventOn::Collide, 1)};
        check(gpuEventChannel(fx, 0) == 2, "the first trigger rule is channel 2");
        check(gpuEventChannel(fx, 1) == 0 && gpuEventChannel(fx, 5) == 1, "death and collide keep channels 0 and 1");
        check(gpuEventChannel(fx, 2) == kNoEventChannel, "a life fraction of 0 never fires and has no channel");
        check(gpuEventChannel(fx, 3) == 3 && gpuEventChannel(fx, 4) == 4, "a rule that never fires does not take a channel");
        check(gpuEventChannel(fx, 99) == kNoEventChannel, "a rule past the list has none");
        check(!emitterEventsNeedCpu(fx), "four firing trigger rules fit");
        fx.events.push_back(rule(EventOn::Spawn, 1));
        check(gpuEventChannel(fx, 6) == 5 && !emitterEventsNeedCpu(fx), "the last trigger channel is 5");
        fx.events.push_back(rule(EventOn::Timer, 1, 1.0f));
        check(emitterEventsNeedCpu(fx) && gpuEventChannel(fx, 7) == kNoEventChannel, "a fifth firing trigger rule is the CPU-only case");
        check(!eventRuleFires(rule(EventOn::Timer, 1, 0.0005f)) && !eventRuleFires(rule(EventOn::Lifetime, 1, 1.0f)), "out-of-range marks never fire");
    }

    // ---- which systems stay on the GPU ----
    {
        ParticleSystemDef def;
        def.emitters = {quiet("Parent"), quiet("A"), quiet("B")};
        def.emitters[0].events = {rule(EventOn::Spawn, 1, 0, kEventPayloadAll, 2.0f), rule(EventOn::Lifetime, 2, 0.5f)};
        check(!systemEventsNeedCpu(def), "trigger rules and a payload feeding different children are GPU rules");
        def.emitters[0].events.push_back(rule(EventOn::Timer, 1, 0.5f));
        check(systemEventsNeedCpu(def), "a child fed by two rules is a CPU system");
        def.emitters[0].events.pop_back();
        def.emitters[1].events = {rule(EventOn::Death, 1)};   // a rule naming its own emitter is ignored
        check(!systemEventsNeedCpu(def), "a rule naming its own emitter feeds nobody");
    }

    // ---- tier selection through the system, with a fake pool ----
    {
        ParticleEffectLibrary lib;
        ParticleSystemDef def;
        def.emitters = {quiet("Parent"), quiet("A"), quiet("B")};
        def.emitters[0].events = {rule(EventOn::Spawn, 1, 0.0f, kEventPayloadAll, 2.0f), rule(EventOn::Timer, 2, 0.25f)};
        lib.setSystem(1, def);
        const scene::Entity e = world.create("ParticleGpuEventTest.Emitter");
        auto* c = static_cast<scene::CParticleEmitter*>(world.addComponent(e, scene::kComponentParticleEmitter));
        c->effect = 1;
        FakeTier tier;
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        sys.setGpuTier(&tier);
        sys.tick(world, 0.016f);
        u32 gpu = 0;
        sys.forEachInstance([&](const InstanceView& v) { if (v.entity == e && v.gpuHandle != 0) ++gpu; });
        check(gpu == 3, "an event-linked system with trigger rules and a payload runs wholly on the GPU tier");
        check(tier.parents.size() == 3 && tier.parents[0] == 0 && tier.parents[1] == 1 && tier.parents[2] == 1, "each child is acquired against its parent's handle");

        // The parent's job carries its curves and trigger table; each child's the channel and payload of its rule.
        std::vector<GpuJob>& jobs = sys.gpuJobs();
        check(jobs.size() == 3, "one job per emitter");
        if (jobs.size() == 3) {
            const GpuJob* parent = nullptr; const GpuJob* a = nullptr; const GpuJob* b = nullptr;
            for (const GpuJob& j : jobs) { if (j.handle == 1) parent = &j; else if (j.handle == 2) a = &j; else if (j.handle == 3) b = &j; }
            check(parent && a && b, "the three handles have jobs");
            if (parent && a && b) {
                check(parent->cb.ev.ruleCount == 2 && parent->cb.ev.rule[0][0] == static_cast<u32>(EventOn::Spawn) &&
                      parent->cb.ev.rule[1][0] == static_cast<u32>(EventOn::Timer), "the parent's table lists its trigger rules in channel order");
                f32 period;
                std::memcpy(&period, &parent->cb.ev.rule[1][1], sizeof period);
                check(period == 0.25f, "the timer period rides in the table");
                check(parent->cb.h.evActive == 0, "the parent consumes nothing");
                check(a->cb.h.evActive == 1 && a->cb.h.evType == 2 && a->cb.h.evCount == 2, "child A consumes channel 2 (the spawn rule)");
                check(a->cb.ev.payload == kEventPayloadAll && a->cb.ev.user == 2.0f, "child A carries the rule's payload and user scalar");
                check(b->cb.h.evActive == 1 && b->cb.h.evType == 3 && b->cb.ev.payload == 0, "child B consumes channel 3 (the timer rule) with no payload");
            }
        }
    }
    {   // more trigger rules than channels: the whole system stays on the CPU even when forced
        ParticleEffectLibrary lib;
        ParticleSystemDef def;
        def.emitters = {quiet("Parent"), quiet("Child")};
        for (int i = 0; i < 5; ++i) def.emitters[0].events.push_back(rule(EventOn::Timer, 1, 0.1f + 0.1f * static_cast<f32>(i)));
        lib.setSystem(2, def);
        const scene::Entity e = world.create("ParticleGpuEventTest.Overflow");
        auto* c = static_cast<scene::CParticleEmitter*>(world.addComponent(e, scene::kComponentParticleEmitter));
        c->effect = 2;
        FakeTier tier;
        ParticleSystem sys;
        sys.setEffectLibrary(&lib);
        sys.setGpuTier(&tier);
        sys.tick(world, 0.016f);
        u32 gpu = 0;
        sys.forEachInstance([&](const InstanceView& v) { if (v.entity == e && v.gpuHandle != 0) ++gpu; });
        check(gpu == 0 && tier.parents.empty(), "five trigger rules on one emitter keep the system on the CPU tier");
    }

    // ---- layout ----
    {
        check(sizeof(GpuEventData) == 240 && offsetof(GpuSimCB, ev) == 1216 && offsetof(GpuSimCB, req) == 1456 && sizeof(GpuSimCB) == 8624,
              "the cbuffer is header, user, modules, event block, requests");
        check(kGpuEventChannels == 6 && kGpuEventRegionUints == kGpuEventHeaderUints + kGpuEventChannels * kGpuEventCapacity * kGpuEventRecordUints,
              "a region is a header of per-channel counts then the channel record lists");
        check(kGpuEventTint + 1 == kGpuEventSizeRatio && kGpuEventSizeRatio + 1 == kGpuEventAgeFraction && kGpuEventAgeFraction + 1 == kGpuEventRecordUints,
              "the payload words close the record");
#ifdef AVER_REPO_ROOT
        const std::string common = readFile(std::string(AVER_REPO_ROOT) + "/modules/particles/shaders/particles_common.hlsli");
        const std::string sim = readFile(std::string(AVER_REPO_ROOT) + "/modules/particles/shaders/particles_sim.hlsl");
        check(defineOf(common, "PEV_CAP") == static_cast<int>(kGpuEventCapacity) && defineOf(common, "PEV_REC") == static_cast<int>(kGpuEventRecordUints) &&
              defineOf(common, "PEV_HEADER") == static_cast<int>(kGpuEventHeaderUints) && defineOf(common, "PEV_CHANNELS") == static_cast<int>(kGpuEventChannels),
              "the HLSL event-buffer constants equal the C++ ones");
        check(defineOf(common, "PEV_TINT") == static_cast<int>(kGpuEventTint) && defineOf(common, "PEV_SIZE") == static_cast<int>(kGpuEventSizeRatio) &&
              defineOf(common, "PEV_AGE") == static_cast<int>(kGpuEventAgeFraction), "the HLSL payload word indices equal the C++ ones");
        check(defineOf(common, "PEVT_SPAWN") == static_cast<int>(EventOn::Spawn) && defineOf(common, "PEVT_LIFETIME") == static_cast<int>(EventOn::Lifetime) &&
              defineOf(common, "PEVT_TIMER") == static_cast<int>(EventOn::Timer) && defineOf(common, "PEVT_DEATH") == 0 && defineOf(common, "PEVT_COLLIDE") == 1,
              "the HLSL trigger kinds equal EventOn");
        const size_t mod = sim.find("uint4  gMod[48];"), ev = sim.find("float4 gEv[15];"), req = sim.find("float4 gReq[448];");
        check(mod != std::string::npos && ev != std::string::npos && req != std::string::npos && mod < ev && ev < req,
              "the HLSL cbuffer declares the event block between the modules and the requests");
        check(sim.find("InterlockedAdd(gEvents[gS.evWriteBase + channel], 1u, idx)") != std::string::npos &&
              sim.find("pRaiseSpawnEvents(p)") != std::string::npos && sim.find("pRaiseTriggerEvents(p)") != std::string::npos,
              "the kernels raise births in CSSpawn and marks in CSUpdate through the channel counters");
#endif
    }

    // ---- the kernel's payload arithmetic equals EmitterSim's record ----
    {
        ParticleEffect fx;
        fx.maxParticles = 64;
        fx.lifetimeMin = fx.lifetimeMax = 2.0f;
        fx.speedMin = fx.speedMax = 0.0f;
        fx.sizeCurve.n = 3; fx.sizeCurve.t[0] = 0.0f; fx.sizeCurve.t[1] = 0.4f; fx.sizeCurve.t[2] = 1.0f;
        fx.sizeCurve.v[0][0] = 10.0f; fx.sizeCurve.v[1][0] = 40.0f; fx.sizeCurve.v[2][0] = 5.0f;
        fx.colorCurve.n = 3; fx.colorCurve.t[0] = 0.0f; fx.colorCurve.t[1] = 0.5f; fx.colorCurve.t[2] = 1.0f;
        const f32 cols[3][4] = {{1, 0.2f, 0.1f, 1}, {0.5f, 1, 0.3f, 0.8f}, {0.1f, 0.4f, 1, 0}};
        for (int k = 0; k < 3; ++k) for (int c = 0; c < 4; ++c) fx.colorCurve.v[k][c] = cols[k][c];
        fx.events = {rule(EventOn::Lifetime, 0, 0.3f, kEventPayloadAll), rule(EventOn::Timer, 0, 0.5f)};
        fx.events[0].emitter = fx.events[1].emitter = 1;
        GpuEventData d;
        packGpuEventData(fx, d);
        check(d.ruleCount == 2 && d.size[2] == 3.0f && d.size[3] == 3.0f, "the block records the stop counts and rule count");
        EmitterSim sim;
        sim.init(fx, {}, 9);
        SpawnRequest rq;
        rq.count = 5; rq.sizeScale = 1.7f; rq.tint = 0x80FF40C0u;
        sim.spawn(rq);
        std::vector<EventRecord> records;
        std::vector<Particle> before = sim.particles();
        sim.step(0.7f, 0.7f, &records);   // crosses 0.3 * 2 s; a timer mark at 0.5 s
        bool allMatch = !records.empty();
        for (const EventRecord& r : records) {
            // The record is made at the particle's age after the step, 0.7 s.
            const GpuEventPayload g = gpuEventPayload(d, rq.tint, 0.7f, 2.0f, 1.7f);
            const bool colour = r.tint == g.tint;
            const bool size = std::fabs(r.sizeRatio - g.sizeRatio) < 1e-4f;
            const bool age = std::fabs(r.ageFraction - g.ageFraction) < 1e-5f;
            allMatch = allMatch && colour && size && age;
        }
        check(allMatch, "gpuEventPayload equals EmitterSim's colour, size ratio and life fraction through curves and a tint");
        // No curve: the start/end pair applies.
        ParticleEffect plain;
        plain.sizeStart = 8.0f; plain.sizeEnd = 2.0f;
        plain.colorStart[0] = 1; plain.colorEnd[0] = 0; plain.colorEnd[3] = 0.5f;
        GpuEventData pd;
        packGpuEventData(plain, pd);
        const GpuEventPayload pg = gpuEventPayload(pd, 0xFFFFFFFFu, 1.0f, 2.0f, 1.0f);
        f32 want[4];
        evalColor(plain, 0.5f, want);
        check(pg.tint == packRgba8(want[0], want[1], want[2], want[3]) && std::fabs(pg.sizeRatio - 5.0f / 8.0f) < 1e-5f, "without curves the start and end values are used");
    }

    // ---- the Overall Quality rung drives the particle tier ----
    {
        ScalabilityTier t = ScalabilityTier::Epic;
        check(tierForQualityRung(1, t) && t == ScalabilityTier::Low && tierForQualityRung(2, t) && t == ScalabilityTier::Medium &&
              tierForQualityRung(3, t) && t == ScalabilityTier::High && tierForQualityRung(4, t) && t == ScalabilityTier::Epic,
              "rungs 1..4 are Low..Epic");
        t = ScalabilityTier::High;
        check(!tierForQualityRung(0, t) && !tierForQualityRung(5, t) && t == ScalabilityTier::High, "Custom and out-of-range rungs leave the tier alone");
    }

    AVER_INFO("ParticleGpuEventTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
