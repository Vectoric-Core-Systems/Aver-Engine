#include "aver/softbody/softbody_abi.h"

#include "aver/softbody/AsyncSolver.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sb = aver::softbody;

namespace {

struct Entry {
    sb::Cage cage;
    sb::StepConfig cfg;
    sb::CrushParams crush;
    std::unique_ptr<sb::AsyncSolver> async;
    sb::Snapshot last;     // newest snapshot taken from the worker
};

std::mutex g_mu;
std::unordered_map<int32_t, std::unique_ptr<Entry>> g_entries;
int32_t g_next = 1;

// Calls are short, so the table lock is held for the whole call: a destroy cannot overlap a use.
Entry* find(int32_t h) {
    const auto it = g_entries.find(h);
    return it == g_entries.end() ? nullptr : it->second.get();
}

bool asyncOn(const Entry& e) { return e.async && e.async->running(); }

} // namespace

extern "C" {

int32_t aver_sb_create(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    const int32_t h = g_next++;
    g_entries.emplace(h, std::make_unique<Entry>());
    return h;
}

void aver_sb_destroy(int32_t h) {
    std::unique_ptr<Entry> dead;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_entries.find(h);
        if (it == g_entries.end()) return;
        dead = std::move(it->second);
        g_entries.erase(it);
    }
    if (dead->async) dead->async->stop();
}

void aver_sb_default_material(AverSbMaterial* out) {
    if (!out) return;
    const sb::Material m;
    out->stiffness = m.stiffness;
    out->axialStiffness = m.axialStiffness;
    out->bendForceN = m.bendForceN;
    out->breakForceN = m.breakForceN;
    out->plasticStiffness = m.plasticStiffness;
    out->maxBend = m.maxBend;
    out->bendAbsorb = m.bendAbsorb;
    out->breakAbsorb = m.breakAbsorb;
    out->hardening = m.hardening;
    out->breakStrain = m.breakStrain;
    out->behavior = static_cast<int32_t>(m.behavior);
}

void aver_sb_default_config(AverSbConfig* out) {
    if (!out) return;
    const sb::StepConfig c;
    out->dt = c.dt;
    out->substeps = c.substeps;
    out->iterations = c.iterations;
    out->velocityDamping = c.velocityDamping;
    out->maxNodeSpeed = c.maxNodeSpeed;
    out->damageRate = c.damageRate;
    out->breakKick = c.breakKick;
    out->enableDamage = c.enableDamage ? 1 : 0;
    out->gravityX = c.gravity.x;
    out->gravityY = c.gravity.y;
    out->gravityZ = c.gravity.z;
    out->gravityAll = c.gravityAll ? 1 : 0;
    out->settleThresholdCm = c.settleThresholdCm;
}

int32_t aver_sb_add_material(int32_t h, const AverSbMaterial* m) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !m || e->cage.built) return -1;
    sb::Material mat;
    mat.stiffness = m->stiffness;
    mat.axialStiffness = m->axialStiffness;
    mat.bendForceN = m->bendForceN;
    mat.breakForceN = m->breakForceN;
    mat.plasticStiffness = m->plasticStiffness;
    mat.maxBend = m->maxBend;
    mat.bendAbsorb = m->bendAbsorb;
    mat.breakAbsorb = m->breakAbsorb;
    mat.hardening = m->hardening;
    mat.breakStrain = m->breakStrain;
    mat.behavior = static_cast<sb::Behavior>(std::clamp(m->behavior, 0, 2));
    return static_cast<int32_t>(sb::addMaterial(e->cage, mat));
}

int32_t aver_sb_add_particle(int32_t h, float x, float y, float z, int32_t pinned) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || e->cage.built) return -1;
    return static_cast<int32_t>(sb::addParticle(e->cage, aver::Vec3{x, y, z}, pinned != 0));
}

int32_t aver_sb_add_beam(int32_t h, int32_t a, int32_t b, int32_t material) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || a < 0 || b < 0 || material < 0) return -1;
    const aver::u32 r = sb::addBeam(e->cage, static_cast<aver::u32>(a), static_cast<aver::u32>(b),
                                    static_cast<aver::u32>(material));
    return r == sb::kNone ? -1 : static_cast<int32_t>(r);
}

int32_t aver_sb_add_triangle(int32_t h, int32_t a, int32_t b, int32_t c, int32_t material) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || a < 0 || b < 0 || c < 0 || material < 0) return -1;
    const aver::u32 r = sb::addTriangle(e->cage, static_cast<aver::u32>(a), static_cast<aver::u32>(b),
                                        static_cast<aver::u32>(c), static_cast<aver::u32>(material));
    return r == sb::kNone ? -1 : static_cast<int32_t>(r);
}

int32_t aver_sb_set_beam_material(int32_t h, int32_t beam, int32_t material) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || asyncOn(*e) || beam < 0 || material < 0) return 0;
    if (static_cast<size_t>(beam) >= e->cage.beams.size() ||
        static_cast<size_t>(material) >= e->cage.materials.size()) return 0;
    e->cage.beams[static_cast<size_t>(beam)].material = static_cast<aver::u32>(material);
    return 1;
}

int32_t aver_sb_build(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e) return 0;
    sb::build(e->cage);
    return 1;
}

int32_t aver_sb_set_config(int32_t h, const AverSbConfig* c) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !c) return 0;
    sb::StepConfig cfg;
    cfg.dt = c->dt > 0.0f ? c->dt : cfg.dt;
    cfg.substeps = c->substeps;
    cfg.iterations = c->iterations;
    cfg.velocityDamping = c->velocityDamping;
    cfg.maxNodeSpeed = c->maxNodeSpeed;
    cfg.damageRate = c->damageRate;
    cfg.breakKick = c->breakKick;
    cfg.enableDamage = c->enableDamage != 0;
    cfg.gravity = aver::Vec3{c->gravityX, c->gravityY, c->gravityZ};
    cfg.gravityAll = c->gravityAll != 0;
    cfg.settleThresholdCm = c->settleThresholdCm;
    e->cfg = cfg;
    if (asyncOn(*e)) e->async->setConfig(cfg);
    return 1;
}

int32_t aver_sb_step(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !e->cage.built || asyncOn(*e)) return -1;
    const sb::StepResult r = sb::step(e->cage, e->cfg);
    e->cage.splitLog.clear();   // the C ABI has no render binding; a host that wants splits uses C++
    return (r.anyBreak ? 1 : 0) | (r.settled ? 2 : 0);
}

int32_t aver_sb_move_particle(int32_t h, int32_t particle, float x, float y, float z) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !e->cage.built || particle < 0) return 0;
    const aver::Vec3 p{x, y, z};
    if (asyncOn(*e)) { e->async->enqueueMove(static_cast<aver::u32>(particle), p); return 1; }
    if (static_cast<size_t>(particle) >= e->cage.particles.size()) return 0;
    e->cage.particles[static_cast<size_t>(particle)].pos = p;
    e->cage.particles[static_cast<size_t>(particle)].prev = p;
    return 1;
}

int32_t aver_sb_impact(int32_t h, float px, float py, float pz, float dx, float dy, float dz,
                       float depthCm, float radiusCm) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !e->cage.built) return 0;
    sb::Impact im;
    im.point = aver::Vec3{px, py, pz};
    im.direction = aver::Vec3{dx, dy, dz};
    im.depthCm = depthCm;
    im.radiusCm = radiusCm;
    if (asyncOn(*e)) { e->async->enqueueImpact(im, e->crush); return 1; }
    sb::applyImpact(e->cage, im, e->crush);
    return 1;
}

int32_t aver_sb_break_beam(int32_t h, int32_t beam) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !e->cage.built || beam < 0) return 0;
    if (asyncOn(*e)) { e->async->enqueueBreak(static_cast<aver::u32>(beam)); return 1; }
    sb::breakBeam(e->cage, static_cast<aver::u32>(beam));
    return 1;
}

int32_t aver_sb_repair(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !e->cage.built) return 0;
    if (asyncOn(*e)) { e->async->enqueueRepair(); return 1; }
    sb::repair(e->cage);
    return 1;
}

int32_t aver_sb_particle_count(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e) return 0;
    if (asyncOn(*e) && !e->last.positions.empty()) return static_cast<int32_t>(e->last.positions.size());
    return static_cast<int32_t>(e->cage.particles.size());
}

int32_t aver_sb_positions(int32_t h, float* out, int32_t maxParticles) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !out || maxParticles <= 0) return 0;
    const bool fromSnapshot = asyncOn(*e) && !e->last.positions.empty();
    const size_t n = fromSnapshot ? e->last.positions.size() : e->cage.particles.size();
    const size_t count = std::min(n, static_cast<size_t>(maxParticles));
    for (size_t i = 0; i < count; ++i) {
        const aver::Vec3& p = fromSnapshot ? e->last.positions[i] : e->cage.particles[i].pos;
        out[i * 3 + 0] = p.x;
        out[i * 3 + 1] = p.y;
        out[i * 3 + 2] = p.z;
    }
    return static_cast<int32_t>(count);
}

int32_t aver_sb_broken_beams(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e) return 0;
    if (asyncOn(*e) && e->last.serial != 0) return static_cast<int32_t>(e->last.brokenBeams);
    return static_cast<int32_t>(sb::brokenBeamCount(e->cage));
}

int32_t aver_sb_pieces(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e) return 0;
    return static_cast<int32_t>(sb::countPieces(e->cage));
}

int32_t aver_sb_async_start(int32_t h, float hz) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !e->cage.built || asyncOn(*e)) return 0;
    if (!e->async) e->async = std::make_unique<sb::AsyncSolver>();
    sb::AsyncOptions opt;
    opt.hz = hz;
    e->last = sb::Snapshot{};
    return e->async->start(e->cage, e->cfg, opt) ? 1 : 0;
}

void aver_sb_async_stop(int32_t h) {
    sb::AsyncSolver* worker = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Entry* e = find(h);
        if (e && e->async) worker = e->async.get();
    }
    if (worker) worker->stop();
}

int32_t aver_sb_async_poll(int32_t h, float* out, int32_t maxParticles, int32_t* settledOut) {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(h);
    if (!e || !asyncOn(*e)) return -1;
    sb::Snapshot snap;
    if (!e->async->tryGetSnapshot(snap)) return -1;
    e->last.positions = snap.positions;
    e->last.brokenBeams = snap.brokenBeams;
    e->last.serial = snap.serial;
    if (settledOut) *settledOut = snap.settled ? 1 : 0;
    if (out && maxParticles > 0) {
        const size_t count = std::min(snap.positions.size(), static_cast<size_t>(maxParticles));
        for (size_t i = 0; i < count; ++i) {
            out[i * 3 + 0] = snap.positions[i].x;
            out[i * 3 + 1] = snap.positions[i].y;
            out[i * 3 + 2] = snap.positions[i].z;
        }
    }
    return static_cast<int32_t>(snap.positions.size());
}

} // extern "C"
