#include "aver/synapse/synapse_ai_abi.h"

#include "aver/scene/World.hpp"
#include "aver/synapse/SynapseAi.hpp"

using namespace aver;
using namespace aver::synapse;

namespace {

SynapseAi& inst() {
    static SynapseAi ai;
    return ai;
}

scene::World& world() { return scene::World::instance(); }

bool live(int32_t e) { return e > 0 && world().valid(static_cast<scene::Entity>(e)); }
scene::Entity ent(int32_t e) { return static_cast<scene::Entity>(e); }

struct CallbackSink final : IHearingMemorySink {
    aver_syn_memory_fn fn = nullptr;
    void* user = nullptr;

    void emit(u32 listener, const HeardMemory& m, int32_t forgotten) const {
        if (fn)
            fn(static_cast<int32_t>(listener), m.pos.x, m.pos.y, m.pos.z, m.level, static_cast<int32_t>(m.tag),
               static_cast<int32_t>(m.source), m.confidence, forgotten, user);
    }
    void onHeard(u32 listener, const HeardMemory& m) override { emit(listener, m, 0); }
    void onForgotten(u32 listener, const HeardMemory& m) override { emit(listener, m, 1); }
};

CallbackSink& sink() {
    static CallbackSink s;
    return s;
}

CSynapseCrowd* crowdOf(int32_t e) {
    return live(e) ? world().component<CSynapseCrowd>(ent(e), inst().crowd().componentType()) : nullptr;
}

CSynapseHearing* hearingOf(int32_t e) {
    return live(e) ? world().component<CSynapseHearing>(ent(e), inst().hearing().componentType()) : nullptr;
}

CSynapseSquad* squadOf(int32_t e) {
    return live(e) ? world().component<CSynapseSquad>(ent(e), inst().tactics().squadType()) : nullptr;
}

} // namespace

extern "C" {

// ---- lifecycle ------------------------------------------------------------------------------------

int32_t aver_syn_ai_register(void) {
    inst().registerComponents(world());
    return 1;
}

void aver_syn_ai_tick(const void* nav, float dt) {
    inst().tick(world(), static_cast<const fmt::OcNavData*>(nav), dt);
}

void aver_syn_ai_register_behaviors(void* btRegistry) {
    if (btRegistry) inst().registerBehaviors(*static_cast<BtRegistry*>(btRegistry));
}

void* aver_syn_ai_instance(void) { return &inst(); }

void aver_syn_ai_reset(void) { inst().reset(); }

// ---- crowd ----------------------------------------------------------------------------------------

int32_t aver_syn_crowd_attach(int32_t entity) {
    return live(entity) && inst().crowd().attach(world(), ent(entity)) ? 1 : 0;
}

int32_t aver_syn_crowd_configure(int32_t entity, float radiusCm, float maxSpeedCm, float maxAccelCm,
                                 float priority) {
    CSynapseCrowd* c = crowdOf(entity);
    if (!c) return 0;
    c->radiusCm = radiusCm;
    c->maxSpeedCm = maxSpeedCm;
    c->maxAccelCm = maxAccelCm;
    c->priority = priority;
    return 1;
}

int32_t aver_syn_crowd_set_mode(int32_t entity, int32_t mode, float x, float y, float z) {
    CSynapseCrowd* c = crowdOf(entity);
    if (!c || mode < 0 || mode > AVER_SYN_MODE_HOLD) return 0;
    c->mode = mode;
    c->steerXCm = x;
    c->steerYCm = y;
    c->steerZCm = z;
    return 1;
}

int32_t aver_syn_crowd_set_enabled(int32_t entity, int32_t enabled) {
    CSynapseCrowd* c = crowdOf(entity);
    if (!c) return 0;
    c->enabled = enabled ? 1 : 0;
    return 1;
}

int32_t aver_syn_crowd_velocity(int32_t entity, float* vx, float* vy, float* speed) {
    const CSynapseCrowd* c = crowdOf(entity);
    if (!c || !c->active) return 0;
    if (vx) *vx = c->velXCm;
    if (vy) *vy = c->velYCm;
    if (speed) *speed = c->speedCm;
    return 1;
}

int32_t aver_syn_crowd_set_backend(int32_t backend) {
    if (backend != AVER_SYN_BACKEND_CPU && backend != AVER_SYN_BACKEND_GPU) return 0;
    CrowdSystem& c = inst().crowd();
    c.sim().setBackendKind(backend == AVER_SYN_BACKEND_GPU ? CrowdBackendKind::Gpu : CrowdBackendKind::Cpu);
    return 1;
}

int32_t aver_syn_crowd_backend(void) {
    return inst().crowd().backendKind() == CrowdBackendKind::Gpu ? AVER_SYN_BACKEND_GPU : AVER_SYN_BACKEND_CPU;
}

const char* aver_syn_crowd_backend_name(void) { return inst().crowd().activeBackendName(); }

void aver_syn_crowd_set_gpu_backend(void* icrowdBackend) {
    inst().crowd().sim().setGpuBackend(static_cast<ICrowdBackend*>(icrowdBackend));
}

int32_t aver_syn_crowd_set_max_agents(int32_t maxAgents) {
    if (maxAgents < 0) return 0;
    inst().crowd().setMaxAgents(static_cast<u32>(maxAgents));
    return 1;
}

int32_t aver_syn_crowd_max_agents(void) { return static_cast<int32_t>(inst().crowd().maxAgents()); }
int32_t aver_syn_crowd_agent_count(void) { return static_cast<int32_t>(inst().crowd().simulatedCount()); }
int32_t aver_syn_crowd_overflow_count(void) { return static_cast<int32_t>(inst().crowd().overflowCount()); }

int32_t aver_syn_crowd_set_drive(int32_t drive) {
    if (drive != AVER_SYN_DRIVE_ADVISE && drive != AVER_SYN_DRIVE_MOVE) return 0;
    inst().crowd().setDrive(drive == AVER_SYN_DRIVE_MOVE ? CrowdDrive::Move : CrowdDrive::Advise);
    return 1;
}

// ---- hearing --------------------------------------------------------------------------------------

int32_t aver_syn_hearing_attach(int32_t entity) {
    return live(entity) && inst().hearing().attach(world(), ent(entity)) ? 1 : 0;
}

int32_t aver_syn_hearing_configure(int32_t entity, float sensitivity, float maxRangeCm, float memorySec) {
    CSynapseHearing* h = hearingOf(entity);
    if (!h) return 0;
    h->sensitivity = sensitivity;
    h->maxRangeCm = maxRangeCm;
    h->memorySec = memorySec;
    return 1;
}

int32_t aver_syn_emit_noise(float x, float y, float z, float loudnessCm, int32_t tag, int32_t sourceEntity) {
    NoiseEvent ev;
    ev.pos = Vec3{x, y, z};
    ev.loudnessCm = loudnessCm;
    ev.tag = static_cast<u32>(tag);
    ev.source = sourceEntity > 0 ? static_cast<u32>(sourceEntity) : 0u;
    return inst().hearing().emit(ev) ? 1 : 0;
}

int32_t aver_syn_hearing_get(int32_t entity, float* x, float* y, float* z, float* level, int32_t* tag,
                             float* confidence, float* timeSince) {
    const CSynapseHearing* h = hearingOf(entity);
    if (!h || !h->hasMemory) return 0;
    if (x) *x = h->heardXCm;
    if (y) *y = h->heardYCm;
    if (z) *z = h->heardZCm;
    if (level) *level = h->heardLevel;
    if (tag) *tag = h->heardTag;
    if (confidence) *confidence = h->confidence;
    if (timeSince) *timeSince = h->timeSinceHeardSec;
    return 1;
}

int32_t aver_syn_hearing_forget(int32_t entity) {
    if (!live(entity)) return 0;
    inst().hearing().forget(ent(entity));
    return 1;
}

void aver_syn_hearing_set_memory_callback(aver_syn_memory_fn fn, void* user) {
    sink().fn = fn;
    sink().user = user;
    inst().hearing().setMemorySink(fn ? &sink() : nullptr);
}

// ---- cover ----------------------------------------------------------------------------------------

int32_t aver_syn_cover_add(float x, float y, float dirX, float dirY, int32_t height, float arcHalfAngleDeg) {
    return static_cast<int32_t>(inst().tactics().covers().addAuthored(
        V2{x, y}, V2{dirX, dirY}, height ? CoverHeight::High : CoverHeight::Low, arcHalfAngleDeg));
}

int32_t aver_syn_cover_remove(int32_t coverId) {
    if (coverId <= 0) return 0;
    TacticsSystem& t = inst().tactics();
    const u32 id = static_cast<u32>(coverId);
    t.reservations().release(id, t.reservations().ownerOf(id));
    return t.covers().remove(id) ? 1 : 0;
}

int32_t aver_syn_cover_marker_attach(int32_t entity, int32_t height, float arcHalfAngleDeg) {
    if (!live(entity)) return 0;
    CSynapseCoverMarker* m = inst().tactics().attachMarker(world(), ent(entity));
    if (!m) return 0;
    m->height = height ? 1 : 0;
    m->arcHalfAngleDeg = arcHalfAngleDeg;
    return 1;
}

int32_t aver_syn_cover_set_auto_generate(int32_t on, float minSpacingCm) {
    CoverGenParams p;
    if (minSpacingCm > 0.0f) p.minSpacingCm = minSpacingCm;
    inst().tactics().setAutoGenerate(on != 0, p);
    return 1;
}

int32_t aver_syn_cover_count(void) { return static_cast<int32_t>(inst().tactics().covers().points().size()); }

int32_t aver_syn_cover_find(int32_t seeker, float tx, float ty, float tz, float maxSeekCm,
                            float minThreatDistCm, int32_t requireHigh, float* outX, float* outY) {
    if (!live(seeker)) return 0;
    CoverSearch s;
    if (maxSeekCm > 0.0f) s.maxSeekCm = maxSeekCm;
    if (minThreatDistCm > 0.0f) s.minThreatDistCm = minThreatDistCm;
    s.requireHigh = requireHigh != 0;
    CoverResult r;
    if (!inst().tactics().findCover(world(), ent(seeker), Vec3{tx, ty, tz}, s, r)) return 0;
    if (outX) *outX = r.pos.x;
    if (outY) *outY = r.pos.y;
    return static_cast<int32_t>(r.id);
}

int32_t aver_syn_cover_release(int32_t seeker) {
    if (seeker <= 0) return 0;
    inst().tactics().releaseCover(ent(seeker));
    return 1;
}

int32_t aver_syn_cover_is_covered(int32_t seeker, float tx, float ty, float tz) {
    return live(seeker) && inst().tactics().isCovered(world(), ent(seeker), Vec3{tx, ty, tz}) ? 1 : 0;
}

// ---- squads ---------------------------------------------------------------------------------------

int32_t aver_syn_squad_attach(int32_t entity, int32_t squadId, float spacingCm) {
    if (!live(entity)) return 0;
    CSynapseSquad* s = inst().tactics().attachSquad(world(), ent(entity));
    if (!s) return 0;
    s->squadId = squadId;
    if (spacingCm > 0.0f) s->spacingCm = spacingCm;
    return 1;
}

int32_t aver_syn_squad_set_target(int32_t squadId, float x, float y, float z) {
    inst().tactics().setSquadTarget(squadId, Vec3{x, y, z});
    return 1;
}

int32_t aver_syn_squad_clear_target(int32_t squadId) {
    inst().tactics().clearSquadTarget(squadId);
    return 1;
}

int32_t aver_syn_squad_slot(int32_t entity, int32_t* role, float* x, float* y, float* z) {
    const CSynapseSquad* s = squadOf(entity);
    if (!s || !s->hasSlot) return 0;
    if (role) *role = s->role;
    if (x) *x = s->slotXCm;
    if (y) *y = s->slotYCm;
    if (z) *z = s->slotZCm;
    return 1;
}

int32_t aver_syn_squad_spacing_push(int32_t entity, float* dx, float* dy) {
    if (!squadOf(entity)) return 0;
    const V2 p = inst().tactics().spacingPush(world(), ent(entity));
    if (dx) *dx = p.x;
    if (dy) *dy = p.y;
    return 1;
}

} // extern "C"
