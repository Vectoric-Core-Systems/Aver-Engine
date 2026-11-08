// The .ocparticle editor tab: state, load / save, undo, the field edits and the preview simulation. Every ImGui call lives in
// ParticleEditorUi.cpp; this file reduces to the headless half so tests/editor can compile it with AVER_WITH_IMGUI undefined.
#include "ParticleEditor.hpp"

// GUARDED ON AVER_MODULE_PARTICLES, whole-file -- see ParticleEditor.hpp's own comment. This .cpp is listed unconditionally in
// sandbox/CMakeLists.txt's SOURCES, so it has to reduce to an empty translation unit on its own.
#if AVER_MODULE_PARTICLES
#include "ParticleStackModel.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace aver::editor {

// A starter effect for the Content Browser's "New Particle Effect" -- a small warm ember burst, one of the format test's own
// four expressiveness shapes (smoke, snow, embers, mist; see OcParticleTest.cpp), not a weapon effect. A point-shaped, omni
// (spreadDeg 180) burst with no continuous rate reads immediately as "one puff", the shape an author reaches for most often
// starting from nothing.
particles::ParticleEffect pxStarterEffect(fmt::OcParticleExtras* outExtras) {
    particles::ParticleEffect e;
    e.shape = particles::EmitterShape::Point;
    e.emissionRate = 0.0f;
    e.burstCount = 40;
    e.maxParticles = 128;
    e.lifetimeMin = 0.6f; e.lifetimeMax = 1.1f;
    e.direction = Vec3{0.0f, 0.0f, 1.0f};
    e.spreadDeg = 180.0f;
    e.speedMin = 80.0f; e.speedMax = 220.0f;
    e.gravity = Vec3{0.0f, 0.0f, -150.0f};
    e.damping = 0.3f;
    e.sizeStart = 6.0f; e.sizeEnd = 1.0f;
    e.colorStart[0] = 1.0f;  e.colorStart[1] = 0.55f; e.colorStart[2] = 0.12f; e.colorStart[3] = 1.0f;
    e.colorEnd[0]   = 0.35f; e.colorEnd[1]   = 0.04f; e.colorEnd[2]   = 0.0f;  e.colorEnd[3]   = 0.0f;
    e.blend = rhi::BlendMode::Additive;
    e.receivesGI = false;   // an ember IS its own light source -- see ParticleTypes.hpp's own comment
    if (outExtras) outExtras->name = "New Ember Burst";
    return e;
}

namespace {

// Every open tab's GPU state, so shutdownParticleEditors() can release them before the device goes.
std::vector<std::weak_ptr<ParticleEditorGpu>>& gpuRegistry() {
    static std::vector<std::weak_ptr<ParticleEditorGpu>> r;
    return r;
}

u32 signatureOf(const particles::ParticleEffect& e) {
    return static_cast<u32>(e.render) | (e.linked ? 4u : 0u) | (static_cast<u32>(e.trailPoints) << 8) |
           (static_cast<u32>(e.linkedCapacity) << 16);
}

} // namespace

// ================================================================================== the tab =======

ParticleEditor::ParticleEditor(std::string path) : path_(std::move(path)) {
    def_.emitters.emplace_back();
    loadFromDisk();
}

ParticleEditor::~ParticleEditor() {
    if (gpu_) gpu_->shutdown();
    gpu_.reset();
}

void ParticleEditor::loadFromDisk() {
    std::string why;
    particles::ParticleSystemDef loaded;
    fmt::OcParticleExtras loadedEx;
    if (!fmt::loadOcparticleSystem(path_, loaded, &loadedEx, &why)) {
        loaded_ = false;
        loadError_ = why;
        restartPreview();
        return;
    }
    def_ = std::move(loaded);
    extras_ = loadedEx;
    sel_ = 0;
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();

    syncEditBuffers();

    previewFramed_ = false;   // a freshly (re)loaded effect gets its camera distance picked again
    restartPreview();
}

void ParticleEditor::syncEditBuffers() {
    extras_.name = def_.name;
    if (def_.emitters.empty()) def_.emitters.emplace_back();
    sel_ = std::clamp(sel_, 0, static_cast<int>(def_.emitters.size()) - 1);
    std::snprintf(nameBuf_, sizeof nameBuf_, "%s", def_.name.c_str());
    const particles::ParticleEffect& e = def_.emitters[static_cast<usize>(sel_)];
    if (e.textureId != 0)
        std::snprintf(texBuf_, sizeof texBuf_, "%016llX", static_cast<unsigned long long>(e.textureId));
    else
        texBuf_[0] = '\0';
    std::snprintf(emitterNameBuf_, sizeof emitterNameBuf_, "%s", e.name.c_str());
    std::snprintf(meshBuf_, sizeof meshBuf_, "%s", e.meshPath.c_str());
}

std::string ParticleEditor::title() const {
    // No manual dirty marker: the host applies ImGuiWindowFlags_UnsavedDocument for every editor whose dirty() is true
    // (AssetEditor.cpp) -- a '*' here would double it, exactly the bug Sound/BtEditor's own title() comments document.
    return std::filesystem::path(path_).filename().string() + "###particle:" + path_;
}

bool ParticleEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // saveOcparticleSystem re-reads whatever is already at path_ and MERGES into it (see OcParticle.hpp's round-trip
    // contract) -- comments, blank lines and any record this format does not recognise all survive, at their original position,
    // and a v1 effect is written back as v1 until something v2-only is authored. A no-edit load/save round trip is therefore
    // byte-identical: OcParticleTest proves it at the format layer, ParticleEditorLoadSaveTest again through this call.
    def_.name = extras_.name;
    if (!fmt::saveOcparticleSystem(path_, def_, why)) return false;
    dirty_ = false;
    return true;
}

void ParticleEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS -- Bt/SoundEditor's own rule: reloading here would discard what the author typed because
    // something else touched the file, which an editor must never do on its own.
    if (dirty_) {
        AVER_WARN("[ParticleEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void ParticleEditor::select(int emitter) {
    if (def_.emitters.empty()) return;
    sel_ = std::clamp(emitter, 0, static_cast<int>(def_.emitters.size()) - 1);
    syncEditBuffers();
}

// ---- undo -----------------------------------------------------------------------------------------
//
// Every edit pushes the PRE-edit state, exactly like Bt/Sound/GraphEditor's own call sites, and only after the edit proved to
// change something -- so retyping the same value costs no undo entry.

void ParticleEditor::pushUndo() { history_.push(UndoState{def_, sel_}); }

void ParticleEditor::undo() {
    UndoState s{def_, sel_};
    if (!history_.undo(s)) return;
    def_ = std::move(s.def);
    sel_ = s.sel;
    dirty_ = true;
    lastCoalesceKey_ = 0;
    syncEditBuffers();
}

void ParticleEditor::redo() {
    UndoState s{def_, sel_};
    if (!history_.redo(s)) return;
    def_ = std::move(s.def);
    sel_ = s.sel;
    dirty_ = true;
    lastCoalesceKey_ = 0;
    syncEditBuffers();
}

bool ParticleEditor::editSystem(const std::function<bool(particles::ParticleSystemDef&)>& fn, u32 coalesceKey, u64 frame) {
    if (!loaded_) return false;
    UndoState before{def_, sel_};
    if (!fn(def_)) return false;
    const bool merge = coalesceKey != 0 && coalesceKey == lastCoalesceKey_ && frame >= lastCoalesceFrame_ && frame <= lastCoalesceFrame_ + 2;
    if (!merge) history_.push(std::move(before));
    lastCoalesceKey_ = coalesceKey;
    lastCoalesceFrame_ = frame;
    dirty_ = true;
    syncEditBuffers();
    return true;
}

bool ParticleEditor::editEffect(const std::function<bool(particles::ParticleEffect&)>& fn, u32 coalesceKey, u64 frame) {
    if (!loaded_) return false;
    UndoState before{def_, sel_};
    if (!fn(sel())) return false;
    const bool merge = coalesceKey != 0 && coalesceKey == lastCoalesceKey_ && frame >= lastCoalesceFrame_ && frame <= lastCoalesceFrame_ + 2;
    if (!merge) history_.push(std::move(before));
    lastCoalesceKey_ = coalesceKey;
    lastCoalesceFrame_ = frame;
    dirty_ = true;
    return true;
}

bool ParticleEditor::computeBounds() {
    if (!loaded_) return false;
    const f32 r = pxmodel::computeBoundsRadius(def_, static_cast<usize>(sel_));
    return editEffect([&](particles::ParticleEffect& e) {
        if (!std::isfinite(r) || e.boundsRadiusCm == r) return false;
        e.boundsRadiusCm = r;
        return true;
    });
}

// ---- field edits ----------------------------------------------------------------------------------

void ParticleEditor::setName(std::string name) {
    if (!loaded_ || def_.name == name) return;
    pushUndo();
    def_.name = std::move(name);
    extras_.name = def_.name;
    dirty_ = true;
}

void ParticleEditor::setShape(particles::EmitterShape shape) {
    editEffect([&](particles::ParticleEffect& e) { if (e.shape == shape) return false; e.shape = shape; return true; });
}

void ParticleEditor::setShapeSize(Vec3 size) {
    editEffect([&](particles::ParticleEffect& e) { e.shapeSize = size; return true; });
}

void ParticleEditor::setBlend(rhi::BlendMode blend) {
    editEffect([&](particles::ParticleEffect& e) { if (e.blend == blend) return false; e.blend = blend; return true; });
}

void ParticleEditor::setEmission(f32 rate, u32 burstCount, u32 maxParticles) {
    editEffect([&](particles::ParticleEffect& e) {
        e.emissionRate = std::max(0.0f, rate);
        e.burstCount = burstCount;
        // >=1: a maxParticles of 0 would mean nothing can ever spawn, a degenerate effect the format's parser would still
        // accept but that reads as broken rather than authored.
        e.maxParticles = std::max(1u, maxParticles);
        return true;
    });
}

void ParticleEditor::setLifetime(f32 lifetimeMin, f32 lifetimeMax) {
    editEffect([&](particles::ParticleEffect& e) {
        lifetimeMin = std::max(0.0f, lifetimeMin);
        lifetimeMax = std::max(lifetimeMin, lifetimeMax);   // the parser requires min <= max
        e.lifetimeMin = lifetimeMin;
        e.lifetimeMax = lifetimeMax;
        return true;
    });
}

void ParticleEditor::setDirection(Vec3 direction, f32 spreadDeg) {
    editEffect([&](particles::ParticleEffect& e) {
        e.direction = direction;
        e.spreadDeg = std::clamp(spreadDeg, 0.0f, 180.0f);   // the parser's own range
        return true;
    });
}

void ParticleEditor::setSpeed(f32 speedMin, f32 speedMax) {
    editEffect([&](particles::ParticleEffect& e) {
        speedMin = std::max(0.0f, speedMin);
        speedMax = std::max(speedMin, speedMax);
        e.speedMin = speedMin;
        e.speedMax = speedMax;
        return true;
    });
}

void ParticleEditor::setGravity(Vec3 gravity) {
    editEffect([&](particles::ParticleEffect& e) { e.gravity = gravity; return true; });
}

void ParticleEditor::setDamping(f32 damping) {
    // [0, 1): OcParticle.cpp's own range check -- a negative or >=1 damping is a velocity MULTIPLIER above one, so a mistyped
    // sign would accelerate every particle without bound rather than damp it.
    editEffect([&](particles::ParticleEffect& e) { e.damping = std::clamp(damping, 0.0f, 0.999f); return true; });
}

void ParticleEditor::setSize(f32 sizeStart, f32 sizeEnd) {
    editEffect([&](particles::ParticleEffect& e) {
        e.sizeStart = std::max(0.0f, sizeStart);
        e.sizeEnd = std::max(0.0f, sizeEnd);
        return true;
    });
}

void ParticleEditor::setColorStart(const f32 rgba[4]) {
    editEffect([&](particles::ParticleEffect& e) {
        for (int i = 0; i < 4; ++i) e.colorStart[i] = std::clamp(rgba[i], 0.0f, 1.0f);
        return true;
    });
}

void ParticleEditor::setColorEnd(const f32 rgba[4]) {
    editEffect([&](particles::ParticleEffect& e) {
        for (int i = 0; i < 4; ++i) e.colorEnd[i] = std::clamp(rgba[i], 0.0f, 1.0f);
        return true;
    });
}

void ParticleEditor::setTextureId(u64 id) {
    editEffect([&](particles::ParticleEffect& e) { if (e.textureId == id) return false; e.textureId = id; return true; });
}

void ParticleEditor::setReceivesGI(bool on) {
    editEffect([&](particles::ParticleEffect& e) { if (e.receivesGI == on) return false; e.receivesGI = on; return true; });
}

// ---- the preview simulation -------------------------------------------------------------------

void ParticleEditor::restartPreview() {
    sims_.clear();
    simSignature_.clear();
    accum_.clear();
    inbox_.clear();
    nextInbox_.clear();
    previewBurstFired_ = false;
    previewTime_ = 0.0f;
    emitterPos_ = Vec3{0, 0, 0};
}

void ParticleEditor::ensureSims() {
    const usize n = def_.emitters.size();
    if (sims_.size() > n) {
        sims_.resize(n);
        simSignature_.resize(n);
        accum_.resize(n);
        inbox_.resize(n);
        nextInbox_.resize(n);
    }
    while (sims_.size() < n) {
        sims_.emplace_back();
        simSignature_.push_back(~0u);
        accum_.push_back(0.0f);
        inbox_.emplace_back();
        nextInbox_.emplace_back();
    }
    muted_.resize(n, 0);
    previewFx_ = particles::hasScalability(def_) ? particles::scaledForTier(def_, previewTier_).emitters : def_.emitters;
    for (usize i = 0; i < n; ++i) {
        const u32 sig = signatureOf(previewFx_[i]);
        // A FIXED seed per emitter (12345 + index): Restart reproduces the same spread and lifetimes every time.
        if (sig != simSignature_[i]) {
            sims_[i].init(previewFx_[i], def_.params, 12345u + static_cast<u32>(i));
            simSignature_[i] = sig;
        }
    }
    // The sims run a preview-only copy of the emitters (isolate today), rebuilt every tick so edits show at once.
    if (isoModule_ >= 0 && (static_cast<usize>(isoEmitter_) >= n || static_cast<usize>(isoModule_) >= def_.emitters[static_cast<usize>(isoEmitter_)].modules.size())) {
        isoEmitter_ = isoModule_ = -1;
    }
    if (isoModule_ >= 0) previewFx_[static_cast<usize>(isoEmitter_)] = pxmodel::isolatedModule(previewFx_[static_cast<usize>(isoEmitter_)], isoModule_);
    for (usize i = 0; i < n; ++i) sims_[i].bind(previewFx_[i]);
}

void ParticleEditor::tickPreview(f32 dt) {
    if (!loaded_ || dt <= 0.0f) return;
    ensureSims();
    previewTime_ += dt;
    Vec3 emitterVel{0, 0, 0};
    pxmodel::emitterMotion(motion_, motionRadius_, motionSpeed_, previewTime_, emitterPos_, emitterVel);
    const bool fireBurst = !previewBurstFired_;
    previewBurstFired_ = true;
    const usize n = def_.emitters.size();
    // Events routed during the previous tick become spawns now (one tick of latency, as in ParticleSystem).
    for (usize i = 0; i < n; ++i) {
        inbox_[i].insert(inbox_[i].end(), nextInbox_[i].begin(), nextInbox_[i].end());
        nextInbox_[i].clear();
    }
    std::vector<particles::EventRecord> events;
    for (usize i = 0; i < n; ++i) {
        const particles::ParticleEffect& fx = previewFx_[i];   // def_ with the preview-only tier and isolate applied
        particles::EmitterSim& sim = sims_[i];
        const bool silent = (solo_ >= 0 && static_cast<int>(i) != solo_) || muted_[i] != 0;
        for (usize s = 0; s < def_.params.size() && s < particles::kMaxUserParams; ++s) {
            const f32* lv = liveValue(static_cast<int>(s));
            sim.setUser(static_cast<u32>(s), lv ? lv : def_.params[s].v);
        }
        sim.setOrigin(emitterPos_);

        u32 toSpawn = 0;
        if (!silent) {
            // The stopped -> playing edge, exactly once, matching scene::CParticleEmitter's own semantics: a tab that has just
            // loaded (or just been restarted) is "freshly playing".
            if (fireBurst) toSpawn += fx.burstCount;
            if (fx.emissionRate > 0.0f) {
                accum_[i] += fx.emissionRate * dt;
                const u32 whole = static_cast<u32>(accum_[i]);
                toSpawn += whole;
                accum_[i] -= static_cast<f32>(whole);
            }
        }
        for (const particles::SpawnRequest& r : inbox_[i]) sim.spawn(r);
        inbox_[i].clear();
        if (toSpawn > 0) {
            particles::SpawnRequest r;
            r.xf = Mat4::translation(emitterPos_);
            r.velocity = emitterVel;
            r.count = toSpawn;
            sim.spawn(r);
        }
        events.clear();
        sim.step(dt, previewTime_, fx.events.empty() ? nullptr : &events);
        for (const particles::EventRecord& ev : events) {
            for (usize ri = 0; ri < fx.events.size(); ++ri) {
                const particles::EventSpawn& rule = fx.events[ri];
                if (rule.on != static_cast<u8>(ev.type) || rule.emitter >= n || rule.emitter == i) continue;
                if (rule.on > static_cast<u8>(particles::EventOn::Collide) && ev.rule != ri) continue;
                particles::SpawnRequest cr;
                cr.xf = Mat4::translation(ev.position);
                cr.count = rule.count;
                cr.addVelocity = ev.velocity * rule.inheritVelocity;
                particles::applyEventPayload(rule, ev, cr);
                cr.seed = (ev.id * 2654435761u) ^ (rule.emitter + 1u);
                if (nextInbox_[rule.emitter].size() < 4096) nextInbox_[rule.emitter].push_back(cr);
            }
        }
    }
}

void ParticleEditor::setPlaybackSpeed(f32 speed) { playSpeed_ = std::isfinite(speed) ? std::clamp(speed, 0.05f, 4.0f) : 1.0f; }
void ParticleEditor::setLoopLength(f32 seconds) { loopLength_ = std::isfinite(seconds) ? std::clamp(seconds, 0.5f, 60.0f) : 5.0f; }

void ParticleEditor::advancePreview(f32 wallDt) {
    if (!loaded_ || !(wallDt > 0.0f)) return;
    constexpr f32 kMaxStep = 1.0f / 30.0f;
    f32 dt = wallDt * playSpeed_;
    for (int guard = 0; dt > 1e-6f && guard < 4096; ++guard) {
        if (loop_ && previewTime_ >= loopLength_ - 1e-5f) restartPreview();
        const f32 room = loop_ ? loopLength_ - previewTime_ : dt;
        const f32 step = std::min({dt, kMaxStep, std::max(room, 1e-4f)});
        tickPreview(step);
        dt -= step;
    }
}

void ParticleEditor::stepPreviewFrame() {
    previewPlaying_ = false;
    if (loop_ && previewTime_ >= loopLength_ - 1e-5f) restartPreview();
    tickPreview(kPreviewStep);
}

void ParticleEditor::seekPreview(f32 seconds) {
    if (!loaded_) return;
    if (!std::isfinite(seconds)) seconds = 0.0f;
    seconds = std::clamp(seconds, 0.0f, loop_ ? std::min(loopLength_, kMaxPreviewSeek) : kMaxPreviewSeek);
    restartPreview();
    const int whole = static_cast<int>(std::floor(seconds / kPreviewStep + 1e-3f));
    for (int i = 0; i < whole; ++i) tickPreview(kPreviewStep);
    const f32 rest = seconds - static_cast<f32>(whole) * kPreviewStep;
    if (rest > 1e-4f) tickPreview(rest);
}

const std::vector<particles::Particle>& ParticleEditor::previewParticles() const {
    static const std::vector<particles::Particle> kEmpty;
    if (sel_ < 0 || static_cast<usize>(sel_) >= sims_.size()) return kEmpty;
    return sims_[static_cast<usize>(sel_)].particles();
}

u32 ParticleEditor::previewTotal() const {
    u32 n = 0;
    for (const particles::EmitterSim& s : sims_) n += s.aliveCount();
    return n;
}

void ParticleEditor::setLiveParam(int param, const f32 v[4]) {
    if (param < 0 || static_cast<usize>(param) >= def_.params.size()) return;
    const particles::UserParam& p = def_.params[static_cast<usize>(param)];
    for (LiveParam& l : live_)
        if (l.name == p.name) {
            l.kind = p.kind;
            for (int k = 0; k < 4; ++k) l.v[k] = v[k];
            return;
        }
    LiveParam l;
    l.name = p.name;
    l.kind = p.kind;
    for (int k = 0; k < 4; ++k) l.v[k] = v[k];
    live_.push_back(std::move(l));
}

const f32* ParticleEditor::liveValue(int param) const {
    if (param < 0 || static_cast<usize>(param) >= def_.params.size()) return nullptr;
    const particles::UserParam& p = def_.params[static_cast<usize>(param)];
    for (const LiveParam& l : live_)
        if (l.name == p.name && l.kind == p.kind) return l.v;
    return nullptr;
}

void ParticleEditor::clearLiveParam(int param) {
    if (param < 0 || static_cast<usize>(param) >= def_.params.size()) return;
    const std::string& name = def_.params[static_cast<usize>(param)].name;
    live_.erase(std::remove_if(live_.begin(), live_.end(), [&](const LiveParam& l) { return l.name == name; }), live_.end());
}

void ParticleEditor::resetLiveParams() { live_.clear(); }

int ParticleEditor::liveCount() const {
    int n = 0;
    for (usize i = 0; i < def_.params.size(); ++i) n += liveValue(static_cast<int>(i)) ? 1 : 0;
    return n;
}

bool ParticleEditor::commitLiveParam(int param) {
    const f32* lv = liveValue(param);
    if (!lv) return false;
    f32 v[4] = {lv[0], lv[1], lv[2], lv[3]};
    const bool changed = editSystem([&](particles::ParticleSystemDef& d) { return pxmodel::setUserParamValue(d, param, v); });
    clearLiveParam(param);
    return changed;
}

void ParticleEditor::setPreviewTier(particles::ScalabilityTier tier) {
    if (tier == previewTier_) return;
    previewTier_ = tier;
    restartPreview();
}

void ParticleEditor::setMotion(pxmodel::PreviewMotion kind, f32 radiusCm, f32 speedCmPerSec) {
    motion_ = kind;
    motionRadius_ = std::isfinite(radiusCm) ? std::clamp(radiusCm, 10.0f, 5000.0f) : 150.0f;
    motionSpeed_ = std::isfinite(speedCmPerSec) ? std::clamp(speedCmPerSec, 1.0f, 5000.0f) : 200.0f;
    if (motion_ == pxmodel::PreviewMotion::None) emitterPos_ = Vec3{0, 0, 0};
}

void ParticleEditor::isolateModule(int emitter, int module) {
    if (module < 0 || emitter < 0 || static_cast<usize>(emitter) >= def_.emitters.size() ||
        static_cast<usize>(module) >= def_.emitters[static_cast<usize>(emitter)].modules.size()) {
        if (isoModule_ < 0) return;
        isoEmitter_ = isoModule_ = -1;
    } else {
        if (isoEmitter_ == emitter && isoModule_ == module) return;
        isoEmitter_ = emitter;
        isoModule_ = module;
    }
    restartPreview();
}

void ParticleEditor::setMuted(int emitter, bool muted) {
    if (emitter < 0) return;
    if (muted_.size() <= static_cast<usize>(emitter)) muted_.resize(static_cast<usize>(emitter) + 1, 0);
    muted_[static_cast<usize>(emitter)] = muted ? 1 : 0;
}
bool ParticleEditor::muted(int emitter) const {
    return emitter >= 0 && static_cast<usize>(emitter) < muted_.size() && muted_[static_cast<usize>(emitter)] != 0;
}
void ParticleEditor::setSolo(int emitter) { solo_ = emitter; }

#if !AVER_WITH_IMGUI

// The headless build (and tests/editor's own target, which deliberately leaves AVER_WITH_IMGUI undefined) still gets
// load/save/dirty/every edit/the preview TICK; only the window is absent. ParticleEditorUi.cpp defines these with ImGui.
void ParticleEditor::draw(Engine& e) { (void)e; }
void ParticleEditor::resetLayout() {}

#endif  // !AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeParticleEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocparticle") return nullptr;
    return std::make_unique<ParticleEditor>(path);
}

// Releases the private ParticleRenderer every open tab built for its preview, before the device goes. The tabs themselves may
// be destroyed later; their destructors then find the renderer already shut down.
void shutdownParticleEditors() {
    for (std::weak_ptr<ParticleEditorGpu>& w : gpuRegistry())
        if (std::shared_ptr<ParticleEditorGpu> g = w.lock()) g->shutdown();
    gpuRegistry().clear();
}

namespace detail {
// For ParticleEditorUi.cpp: records a tab's GPU state in the shutdown registry.
void registerParticleEditorGpu(const std::shared_ptr<ParticleEditorGpu>& g) { gpuRegistry().push_back(g); }
} // namespace detail

} // namespace aver::editor
#endif  // AVER_MODULE_PARTICLES
