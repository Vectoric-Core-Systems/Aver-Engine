// The level sequence editor behind the Animate mode (SequenceEditor.hpp).
#include "SequenceEditor.hpp"

#if AVER_MODULE_SCENE
#include "aver/core/Log.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/game/PlayMobility.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/World.hpp"

#if AVER_WITH_IMGUI
#include "EditorKeybinds.hpp"
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace aver::editor {

using fmt::OcSeqInterp;
using fmt::OcSeqKey;
using fmt::OcSeqTrack;
using fmt::OcSeqTrackKind;

namespace {

constexpr f64 kSameKeySec = 1e-3;   // a key this close to the playhead is the same key

i32 targetOf(scene::Entity e) { return static_cast<i32>(static_cast<u32>(e)); }
scene::Entity entityOfTarget(i32 t) { return static_cast<scene::Entity>(static_cast<u32>(t)); }

u64 mix(u64 h, u64 v) { return (h ^ v) * 1099511628211ull; }

#if AVER_WITH_IMGUI
ImU32 kindColor(OcSeqTrackKind k) {
    return k == OcSeqTrackKind::Transform ? IM_COL32(242, 150, 60, 255)
         : k == OcSeqTrackKind::Camera    ? IM_COL32(110, 180, 255, 255)
                                          : IM_COL32(240, 220, 90, 255);
}
#endif

} // namespace

// ---- level lifecycle ---------------------------------------------------------------------------

void SequenceEditor::reset() {
    model_ = fmt::OcSequence{};
    model_.name = "Main";
    player_ = game::SequencePlayer{};
    bases_.clear();
    undo_.clear();
    playRunning_ = false;
    running_ = false;
    evalDirty_ = true;
    editTime_ = 0;
    selTrack_ = -1;
    selKey_ = -1;
    keySel_.clear();
    dragKey_ = dragRuler_ = dragStarted_ = false;
    dragOrig_.clear();
    pxPerSec_ = 0;
    viewStart_ = 0;
    followTime_ = -1;
    status_.clear();
    ++editRev_;
    syncPlayer();
}

void SequenceEditor::load(const std::vector<fmt::OcSequence>& seqs,
                          const std::vector<scene::Entity>& placementEntities) {
    reset();
    if (seqs.empty()) return;
    if (seqs.size() > 1)
        AVER_WARN("[Sequence] the level has {} sequences; the editor keeps the first and drops the rest",
                  (u32)seqs.size());
    scene::World& w = scene::World::instance();
    model_ = seqs.front();
    if (model_.name.empty()) model_.name = "Main";
    u32 dropped = 0;
    bool haveCamera = false;
    std::vector<OcSeqTrack> kept;
    for (OcSeqTrack& tr : model_.tracks) {
        if (tr.kind == OcSeqTrackKind::Camera) {
            if (haveCamera) { ++dropped; continue; }   // one camera track
            haveCamera = true;
            tr.target = -1;
        } else {
            const bool inRange = tr.target >= 0 && static_cast<usize>(tr.target) < placementEntities.size();
            const scene::Entity e = inRange ? placementEntities[static_cast<usize>(tr.target)] : scene::kInvalidEntity;
            if (e == scene::kInvalidEntity || !w.valid(e)) { ++dropped; continue; }
            tr.target = targetOf(e);
        }
        std::stable_sort(tr.keys.begin(), tr.keys.end(),
                         [](const OcSeqKey& a, const OcSeqKey& b) { return a.t < b.t; });
        kept.push_back(std::move(tr));
    }
    model_.tracks = std::move(kept);
    syncPlayer();
    AVER_INFO("[Sequence] loaded '{}': {} track(s), {:.1f} s{}", model_.name, (u32)model_.tracks.size(),
              model_.length, dropped ? " (some tracks named missing placements and were dropped)" : "");
}

void SequenceEditor::save(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf) const {
    w.sequences.clear();
    fmt::OcSequence out = model_;
    out.tracks.clear();
    for (const OcSeqTrack& tr : model_.tracks) {
        OcSeqTrack c = tr;
        if (tr.kind != OcSeqTrackKind::Camera) {
            const auto it = slotOf.find(static_cast<u32>(tr.target));
            if (it == slotOf.end()) continue;
            c.target = it->second;
        } else {
            c.target = -1;
        }
        out.tracks.push_back(std::move(c));
    }
    if (out.tracks.empty()) return;
    w.sequences.push_back(std::move(out));
}

// ---- mode --------------------------------------------------------------------------------------

void SequenceEditor::enter() {
    active_ = true;
    evalDirty_ = true;
    syncPlayer();
}

void SequenceEditor::leave() {
    restoreBases();
    active_ = false;
    dragKey_ = dragRuler_ = false;
    player_.pause();
}

// ---- frame -------------------------------------------------------------------------------------

void SequenceEditor::tick(const SequenceTickCtx& c) {
    playActiveLast_ = c.playActive;
    if (playRunning_) {
        if (!c.playActive) { endPlay(); return; }   // the session never started or ended without a Stop
        if (!c.playPaused) { player_.advance(c.dt); evalDirty_ = true; }
        if (evalDirty_) evaluateNow();
        return;
    }
    if (!active_ || c.playActive) return;
    if (running_) {   // the host sets the time per frame (runFrame); nothing advances here
        if (evalDirty_) { captureMissingBases(); evaluateNow(); }
        return;
    }
    if (player_.playing()) { player_.advance(fixedStep_ ? game::seqStepSeconds(model_) : c.dt); evalDirty_ = true; }
    if (evalDirty_) { captureMissingBases(); evaluateNow(); }
}

void SequenceEditor::evaluateNow() {
    player_.evaluate(scene::World::instance());
    evalDirty_ = false;
}

void SequenceEditor::captureBase(scene::Entity e) {
    scene::World& w = scene::World::instance();
    if (e == scene::kInvalidEntity || !w.valid(e)) return;
    bases_.emplace(static_cast<u32>(e), w.localTransform(e));
}

void SequenceEditor::captureMissingBases() {
    for (const scene::Entity e : player_.transformEntities()) captureBase(e);
}

void SequenceEditor::restoreBases() {
    if (bases_.empty()) return;
    scene::World& w = scene::World::instance();
    for (const auto& kv : bases_) {
        const scene::Entity e = static_cast<scene::Entity>(kv.first);
        if (w.valid(e)) w.setLocalTransform(e, kv.second);
    }
    bases_.clear();
    evalDirty_ = true;
}

// An actor with no transform track left goes back to where it was authored.
void SequenceEditor::releaseUndrivenBases() {
    scene::World& w = scene::World::instance();
    for (auto it = bases_.begin(); it != bases_.end();) {
        const scene::Entity e = static_cast<scene::Entity>(it->first);
        if (drivesTransform(e)) { ++it; continue; }
        if (w.valid(e)) w.setLocalTransform(e, it->second);
        it = bases_.erase(it);
    }
}

// ---- Play --------------------------------------------------------------------------------------

void SequenceEditor::beginPlay(game::PlayMobility& mobility) {
    playRunning_ = false;
    player_.pause();
    if (!model_.autoplay || model_.tracks.empty()) return;
    syncPlayer();
    if (player_.empty()) return;
    editTime_ = player_.time();
    player_.setTime(0);
    player_.play();
    mobility.seedMovable(player_.boundEntities());
    playRunning_ = true;
    evalDirty_ = true;
}

void SequenceEditor::endPlay() {
    if (!playRunning_) return;
    playRunning_ = false;
    player_.pause();
    player_.setTime(editTime_);
    evalDirty_ = true;
}

std::vector<scene::Entity> SequenceEditor::trackEntities() const {
    std::vector<scene::Entity> out;
    for (const OcSeqTrack& tr : model_.tracks)
        if (tr.target >= 0) out.push_back(static_cast<scene::Entity>(tr.target));
    return out;
}

bool SequenceEditor::drivesTransform(scene::Entity e) const {
    for (const OcSeqTrack& tr : model_.tracks)
        if (tr.kind == OcSeqTrackKind::Transform && tr.target == targetOf(e)) return true;
    return false;
}

// ---- view --------------------------------------------------------------------------------------

bool SequenceEditor::viewPose(bool playEjected, game::SeqCameraPose& out) const {
    if (playRunning_) return !playEjected && model_.camera && player_.camera(out);
    if (viewIsPilot()) return player_.camera(out);
    return false;
}

bool SequenceEditor::emissiveScale(scene::Entity e, f32 out[3]) const {
    return previewing() && player_.emissiveScale(e, out);
}

// ---- camera path overlay -----------------------------------------------------------------------

bool SequenceEditor::pathLines(std::vector<rhi::LineVertex>& out) const {
    out.clear();
    const OcSeqTrack* cam = nullptr;
    int camIndex = -1;
    for (usize i = 0; i < model_.tracks.size(); ++i)
        if (model_.tracks[i].kind == OcSeqTrackKind::Camera && !model_.tracks[i].keys.empty()) {
            cam = &model_.tracks[i];
            camIndex = static_cast<int>(i);
            break;
        }
    if (!cam) return false;

    const auto keyPos = [](const OcSeqKey& k) { return Vec3{(f32)k.v[0], (f32)k.v[1], (f32)k.v[2]}; };
    Vec3 lo = keyPos(cam->keys.front()), hi = lo;
    for (const OcSeqKey& k : cam->keys) {
        const Vec3 p = keyPos(k);
        lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
        hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
    }
    const f32 size = std::clamp((hi - lo).size() * 0.012f, 8.0f, 60.0f);   // marker half-extent, cm

    const auto line = [&](const Vec3& a, const Vec3& b, f32 r, f32 g, f32 bl) {
        out.push_back({a.x, a.y, a.z, r, g, bl});
        out.push_back({b.x, b.y, b.z, r, g, bl});
    };
    const auto cross = [&](const Vec3& c, f32 s, f32 r, f32 g, f32 bl) {
        line(c - Vec3{s, 0, 0}, c + Vec3{s, 0, 0}, r, g, bl);
        line(c - Vec3{0, s, 0}, c + Vec3{0, s, 0}, r, g, bl);
        line(c - Vec3{0, 0, s}, c + Vec3{0, 0, s}, r, g, bl);
    };

    // The curve, as the sampler gives it.
    constexpr int kPerSegment = 24;
    game::SeqCameraPose prev, cur;
    for (usize i = 0; i + 1 < cam->keys.size(); ++i) {
        const f64 t0 = cam->keys[i].t, t1 = cam->keys[i + 1].t;
        for (int j = 0; j <= kPerSegment; ++j) {
            const f64 t = t0 + (t1 - t0) * (static_cast<f64>(j) / kPerSegment);
            if (!game::sampleSeqCamera(*cam, t, cur)) continue;
            if (j > 0) line(prev.position, cur.position, 0.45f, 0.75f, 1.0f);
            prev = cur;
        }
    }

    // A marker and a view direction at every key; the selected ones are white.
    const bool trackSel = selTrack_ == camIndex;
    for (usize k = 0; k < cam->keys.size(); ++k) {
        const OcSeqKey& key = cam->keys[k];
        const bool sel = trackSel && k < keySel_.size() && keySel_[k] != 0;
        const f32 r = 1.0f, g = sel ? 1.0f : 0.6f, b = sel ? 1.0f : 0.2f;
        const Vec3 p = keyPos(key);
        cross(p, size * (sel ? 1.4f : 1.0f), r, g, b);
        const f32 yaw = static_cast<f32>(key.v[3] * 3.14159265358979 / 180.0);
        const f32 pitch = static_cast<f32>(key.v[4] * 3.14159265358979 / 180.0);
        line(p, p + game::cameraForward(yaw, pitch) * (size * 4.0f), r, g, b);
    }

    // The playhead's pose.
    if (game::sampleSeqCamera(*cam, player_.time(), cur)) {
        cross(cur.position, size * 1.8f, 0.4f, 1.0f, 0.5f);
        line(cur.position, cur.position + game::cameraForward(cur.yaw, cur.pitch) * (size * 6.0f), 0.4f, 1.0f, 0.5f);
    }
    return !out.empty();
}

u64 SequenceEditor::pathStamp() const {
    u64 h = 1469598103934665603ull;
    h = mix(h, editRev_);
    f64 t = player_.time();
    u64 bits;
    std::memcpy(&bits, &t, sizeof bits);
    return mix(h, bits);
}

// ---- run mode ----------------------------------------------------------------------------------

bool SequenceEditor::hasCameraKeys() const {
    for (const OcSeqTrack& tr : model_.tracks)
        if (tr.kind == OcSeqTrackKind::Camera && !tr.keys.empty()) return true;
    return false;
}

void SequenceEditor::beginRun() {
    running_ = true;
    fixedStep_ = true;
    pilot_ = true;
    player_.pause();
    player_.setTime(0);
    evalDirty_ = true;
}

void SequenceEditor::endRun() {
    running_ = false;
    pilot_ = false;
}

i64 SequenceEditor::runFrameCount() const {
    return std::max<i64>(1, static_cast<i64>(std::ceil(model_.length / game::seqStepSeconds(model_) - 1e-6)));
}

bool SequenceEditor::runFrame(i64 n) {
    if (n < 0 || n >= runFrameCount()) return false;
    player_.setTime(static_cast<f64>(n) * game::seqStepSeconds(model_));
    evalDirty_ = true;
    return true;
}

// ---- model -------------------------------------------------------------------------------------

// The player gets a copy of the model with each target rewritten to an index into a dense entity
// list; a track whose entity is gone is left out of the copy, not out of the model.
void SequenceEditor::syncPlayer() {
    scene::World& w = scene::World::instance();
    std::vector<scene::Entity> dense;
    std::unordered_map<u32, i32> index;
    fmt::OcSequence seq = model_;
    seq.tracks.clear();
    for (const OcSeqTrack& tr : model_.tracks) {
        OcSeqTrack c = tr;
        if (tr.kind != OcSeqTrackKind::Camera) {
            const scene::Entity e = entityOfTarget(tr.target);
            if (!w.valid(e)) continue;
            auto it = index.find(static_cast<u32>(e));
            if (it == index.end()) {
                it = index.emplace(static_cast<u32>(e), static_cast<i32>(dense.size())).first;
                dense.push_back(e);
            }
            c.target = it->second;
        }
        seq.tracks.push_back(std::move(c));
    }
    player_.setSequence(seq);
    player_.bind(dense);
    player_.setTime(player_.time());   // re-wrapped against the (possibly new) length
}

void SequenceEditor::edited() {
    syncPlayer();
    evalDirty_ = true;
    ++editRev_;
    if (host_ && host_->markLevelDirty) host_->markLevelDirty();
}

void SequenceEditor::undoEdit() {
    if (!undo_.undo(model_)) return;
    status_ = "Undo";
    releaseUndrivenBases();
    clampSelection();
    edited();
}

void SequenceEditor::redoEdit() {
    if (!undo_.redo(model_)) return;
    status_ = "Redo";
    releaseUndrivenBases();
    clampSelection();
    edited();
}

int SequenceEditor::findTrack(OcSeqTrackKind kind, scene::Entity e) const {
    for (usize i = 0; i < model_.tracks.size(); ++i) {
        const OcSeqTrack& tr = model_.tracks[i];
        if (tr.kind == kind && (kind == OcSeqTrackKind::Camera || tr.target == targetOf(e)))
            return static_cast<int>(i);
    }
    return -1;
}

fmt::OcSeqKey* SequenceEditor::selectedKey() {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size()) return nullptr;
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    if (selKey_ < 0 || static_cast<usize>(selKey_) >= tr.keys.size()) return nullptr;
    return &tr.keys[static_cast<usize>(selKey_)];
}

void SequenceEditor::selectTrack(int index) {
    if (index == selTrack_) return;
    selTrack_ = index;
    selKey_ = -1;
    keySel_.clear();
    if (index >= 0 && static_cast<usize>(index) < model_.tracks.size())
        keySel_.assign(model_.tracks[static_cast<usize>(index)].keys.size(), 0);
    ++editRev_;
}

void SequenceEditor::selectOnly(int key) {
    std::fill(keySel_.begin(), keySel_.end(), char(0));
    selKey_ = key;
    if (key >= 0 && static_cast<usize>(key) < keySel_.size()) keySel_[static_cast<usize>(key)] = 1;
    ++editRev_;
}

int SequenceEditor::selectedCount() const {
    return static_cast<int>(std::count(keySel_.begin(), keySel_.end(), char(1)));
}

void SequenceEditor::clampSelection() {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size()) {
        selTrack_ = -1;
        selKey_ = -1;
        keySel_.clear();
        return;
    }
    const usize n = model_.tracks[static_cast<usize>(selTrack_)].keys.size();
    if (keySel_.size() != n) { keySel_.assign(n, 0); selKey_ = -1; }
    if (selKey_ >= static_cast<int>(n)) selKey_ = -1;
}

// The key's values from the live scene: the actor's transform, the editor camera, or (for a new
// material key) a neutral 1,1,1 x 1.
void SequenceEditor::fillKey(const OcSeqTrack& tr, OcSeqKey& key) const {
    switch (tr.kind) {
        case OcSeqTrackKind::Transform: {
            scene::World& w = scene::World::instance();
            const scene::Entity e = entityOfTarget(tr.target);
            if (w.valid(e)) game::seqKeyFromTransform(w.localTransform(e), key);
            break;
        }
        case OcSeqTrackKind::Camera:
            if (host_) game::seqKeyFromCamera(host_->editorCamera, key);
            break;
        case OcSeqTrackKind::Material:
            key.v[0] = key.v[1] = key.v[2] = key.v[3] = 1.0;
            break;
    }
}

std::string SequenceEditor::trackLabel(const OcSeqTrack& tr) const {
    if (tr.kind == OcSeqTrackKind::Camera) return "Camera";
    const scene::Entity e = entityOfTarget(tr.target);
    std::string name;
    if (!scene::World::instance().valid(e)) name = "<deleted>";
    else if (host_ && host_->label) name = host_->label(e);
    else name = "Entity " + std::to_string(static_cast<u32>(e));
    return tr.kind == OcSeqTrackKind::Material ? name + " (emissive)" : name;
}

// Insert a key at t, keeping the keys sorted; returns its index.
static int insertKeySorted(OcSeqTrack& tr, const OcSeqKey& k) {
    const auto it = std::upper_bound(tr.keys.begin(), tr.keys.end(), k.t,
                                     [](f64 t, const OcSeqKey& a) { return t < a.t; });
    return static_cast<int>(tr.keys.insert(it, k) - tr.keys.begin());
}

void SequenceEditor::addTrack(OcSeqTrackKind kind, scene::Entity e) {
    scene::World& w = scene::World::instance();
    if (kind != OcSeqTrackKind::Camera && !w.valid(e)) return;
    const int existing = findTrack(kind, e);
    if (existing >= 0) { selectTrack(existing); return; }
    pushUndo();
    OcSeqTrack tr;
    tr.kind = kind;
    tr.target = kind == OcSeqTrackKind::Camera ? -1 : targetOf(e);
    // The base is the actor as authored, taken before this track can pose it.
    if (kind == OcSeqTrackKind::Transform) captureBase(e);
    // Seeded with a key at the playhead so the track does something at once.
    OcSeqKey k;
    k.t = std::clamp(player_.time(), 0.0, model_.length);
    fillKey(tr, k);
    tr.keys.push_back(k);
    model_.tracks.push_back(std::move(tr));
    selTrack_ = -1;
    selectTrack(static_cast<int>(model_.tracks.size()) - 1);
    selectOnly(0);
    edited();
}

void SequenceEditor::removeTrack(int index) {
    if (index < 0 || static_cast<usize>(index) >= model_.tracks.size()) return;
    pushUndo();
    model_.tracks.erase(model_.tracks.begin() + index);
    releaseUndrivenBases();
    if (selTrack_ == index) { selTrack_ = -1; selKey_ = -1; keySel_.clear(); }
    else if (selTrack_ > index) --selTrack_;
    edited();
}

void SequenceEditor::clearTrack(int index) {
    if (index < 0 || static_cast<usize>(index) >= model_.tracks.size()) return;
    if (model_.tracks[static_cast<usize>(index)].keys.empty()) return;
    pushUndo();
    model_.tracks[static_cast<usize>(index)].keys.clear();
    if (selTrack_ == index) { selKey_ = -1; keySel_.clear(); }
    status_ = "Track cleared";
    edited();
}

void SequenceEditor::keyCamera() {
    if (!host_) return;
    if (viewIsPilot()) {
        status_ = player_.playing() ? "Pause playback to key the camera"
                                    : "Untick 'Lock viewport' to key the editor camera";
        return;
    }
    const int ti = findTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity);
    if (ti < 0) {
        addTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity);   // seeds a key at the playhead
        char buf[96];
        std::snprintf(buf, sizeof buf, "Camera key at %.2f s", player_.time());
        status_ = buf;
        return;
    }
    selectTrack(ti);
    addKeyAtPlayhead();
}

void SequenceEditor::keySelectedTrack() { addKeyAtPlayhead(); }

// A transform key at the playhead for each entity, from its live transform; the track is created
// (seeded with that key) when the actor has none.
void SequenceEditor::keyObjects(const std::vector<scene::Entity>& ents) {
    if (playActiveLast_ || player_.playing()) { status_ = "Pause playback to key an object"; return; }
    scene::World& w = scene::World::instance();
    int n = 0;
    std::string last;
    for (const scene::Entity e : ents) {
        if (!w.valid(e) || (host_ && host_->canKey && !host_->canKey(e))) continue;
        const int ti = findTrack(OcSeqTrackKind::Transform, e);
        if (ti < 0) addTrack(OcSeqTrackKind::Transform, e);
        else { selectTrack(ti); addKeyAtPlayhead(); }
        last = trackLabel(model_.tracks[static_cast<usize>(selTrack_ >= 0 ? selTrack_ : 0)]);
        ++n;
    }
    if (n == 0) { status_ = "Select an object in the level to key"; return; }
    char buf[160];
    if (n == 1) std::snprintf(buf, sizeof buf, "Key at %.2f s on %s", player_.time(), last.c_str());
    else        std::snprintf(buf, sizeof buf, "Keyed %d objects at %.2f s", n, player_.time());
    status_ = buf;
}

void SequenceEditor::addKeyAtPlayhead() {
    clampSelection();   // keySel_ must match the keys before one is inserted into it
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size()) {
        status_ = "Select a track to key";
        return;
    }
    pushUndo();
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    const f64 t = std::clamp(player_.time(), 0.0, model_.length);
    int idx = -1;
    for (usize i = 0; i < tr.keys.size(); ++i)
        if (std::fabs(tr.keys[i].t - t) < kSameKeySec) { idx = static_cast<int>(i); break; }
    if (idx >= 0) {
        // Re-keying: new pose, same time and interpolation. A material key keeps its colour; the
        // panel edits that.
        if (tr.kind != OcSeqTrackKind::Material) fillKey(tr, tr.keys[static_cast<usize>(idx)]);
    } else {
        OcSeqKey k;
        k.t = t;
        fillKey(tr, k);
        idx = insertKeySorted(tr, k);
        keySel_.insert(keySel_.begin() + idx, char(0));
    }
    selectOnly(idx);
    char buf[160];
    std::snprintf(buf, sizeof buf, "Key at %.2f s on %s", t, trackLabel(tr).c_str());
    status_ = buf;
    edited();
}

void SequenceEditor::removeSelectedKeys() {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size() || selectedCount() == 0) return;
    pushUndo();
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    for (usize i = tr.keys.size(); i-- > 0;)
        if (i < keySel_.size() && keySel_[i]) tr.keys.erase(tr.keys.begin() + static_cast<std::ptrdiff_t>(i));
    keySel_.assign(tr.keys.size(), 0);
    selKey_ = -1;
    edited();
}

void SequenceEditor::setSelectedInterp(OcSeqInterp interp) {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size() || selectedCount() == 0) return;
    pushUndo();
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    for (usize i = 0; i < tr.keys.size() && i < keySel_.size(); ++i)
        if (keySel_[i]) tr.keys[i].interp = interp;
    edited();
}

// Playhead to the key and, when there is a camera track, the editor camera to the sequence camera
// there, so the viewport is looking where the key was set.
void SequenceEditor::goToKey(int track, int key) {
    if (track < 0 || static_cast<usize>(track) >= model_.tracks.size()) return;
    const OcSeqTrack& tr = model_.tracks[static_cast<usize>(track)];
    if (key < 0 || static_cast<usize>(key) >= tr.keys.size()) return;
    player_.pause();
    player_.setTime(tr.keys[static_cast<usize>(key)].t);
    evalDirty_ = true;
    ++editRev_;
    if (!host_ || !host_->setEditorCamera) return;
    for (const OcSeqTrack& cam : model_.tracks) {
        game::SeqCameraPose pose;
        if (cam.kind == OcSeqTrackKind::Camera && game::sampleSeqCamera(cam, player_.time(), pose)) {
            host_->setEditorCamera(pose);
            break;
        }
    }
}

void SequenceEditor::stepFrames(int frames) {
    player_.pause();
    const f64 step = game::seqStepSeconds(model_);
    const f64 n = std::round(player_.time() / step) + frames;
    player_.setTime(std::clamp(n * step, 0.0, model_.loop ? std::max(0.0, model_.length - step) : model_.length));
    evalDirty_ = true;
    ++editRev_;
}

// Retimes a key; the list is re-sorted, so the key's index can change. Returns the new index.
int SequenceEditor::moveKey(OcSeqTrack& tr, int index, f64 t) {
    OcSeqKey k = tr.keys[static_cast<usize>(index)];
    k.t = std::clamp(t, 0.0, model_.length);
    tr.keys.erase(tr.keys.begin() + index);
    const int at = insertKeySorted(tr, k);
    if (static_cast<usize>(index) < keySel_.size()) {   // the flag follows its key
        const char flag = keySel_[static_cast<usize>(index)];
        keySel_.erase(keySel_.begin() + index);
        keySel_.insert(keySel_.begin() + at, flag);
    }
    return at;
}

// Shifts every selected key to its press-time position + delta. Pure in the press snapshot, so a
// drag that wanders back lands exactly where it started.
void SequenceEditor::applyDrag(OcSeqTrack& tr, f64 delta) {
    f64 lo = -model_.length, hi = model_.length;   // the group stays inside [0, length]
    for (const DragItem& it : dragOrig_)
        if (it.selected) { lo = std::fmax(lo, -it.key.t); hi = std::fmin(hi, model_.length - it.key.t); }
    delta = std::clamp(delta, lo, std::fmax(lo, hi));
    std::vector<DragItem> items = dragOrig_;
    for (DragItem& it : items)
        if (it.selected) it.key.t += delta;
    std::stable_sort(items.begin(), items.end(),
                     [](const DragItem& a, const DragItem& b) { return a.key.t < b.key.t; });
    tr.keys.clear();
    keySel_.clear();
    selKey_ = -1;
    for (usize i = 0; i < items.size(); ++i) {
        tr.keys.push_back(items[i].key);
        keySel_.push_back(items[i].selected ? char(1) : char(0));
        if (items[i].primary) selKey_ = static_cast<int>(i);
    }
}

// ---- UI ----------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

// "Key <name>": a transform key at the playhead for the selected objects the level can save.
void SequenceEditor::drawKeyObjectButton(const char* idLabel, f32 width) {
    constexpr usize kMaxBatch = 256;
    std::vector<scene::Entity> ok;
    if (host_) {
        scene::World& w = scene::World::instance();
        for (const scene::Entity e : host_->selection) {
            if (ok.size() >= kMaxBatch) break;
            if (w.valid(e) && (!host_->canKey || host_->canKey(e))) ok.push_back(e);
        }
    }
    std::string text = "Key object";
    if (ok.size() == 1) text = "Key " + (host_->label ? host_->label(ok[0]) : std::string("object"));
    else if (ok.size() > 1) text = "Key " + std::to_string(ok.size()) + " objects";
    text += std::string("###") + idLabel;
    const bool off = ok.empty() || playActiveLast_ || player_.playing();
    ImGui::BeginDisabled(off);
    if (ImGui::Button(text.c_str(), ImVec2(width, 0))) keyObjects(ok);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", ok.empty() ? "Select an object in the level (lights, decals and prefab instances can't be keyed)."
                                           : "Writes the selected object's transform as a key on its track at the playhead;\n"
                                             "a key already there is replaced. The track is created if needed.");
}

void SequenceEditor::drawModePanel(SequenceHost& host) {
    host_ = &host;
    const std::vector<scene::Entity>& sel = host.selection;

    ImGui::TextDisabled("CAMERA PATH");
    ImGui::BeginDisabled(viewIsPilot());
    if (ImGui::Button("Key camera  (K)", ImVec2(-1, 0))) keyCamera();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes the viewport camera (position, yaw, pitch) as a key on the camera track\n"
                          "at the playhead; a key already there is replaced. The track is created if needed.");
    drawKeyObjectButton("modeKeyObj", -1.0f);
    ImGui::BeginDisabled(!hasCameraKeys());
    if (ImGui::Button("Clear camera keys", ImVec2(-1, 0)))
        clearTrack(findTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity));
    ImGui::EndDisabled();
    ImGui::Checkbox("Lock viewport to sequence camera", &pilot_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Playing and scrubbing always look through the sequence camera; this keeps\n"
                          "doing it when stopped. Untick it to fly the editor camera and key that pose.");
    game::SeqCameraPose pose;
    ImGui::BeginDisabled(!player_.camera(pose));
    if (ImGui::Button("Fly editor camera to playhead", ImVec2(-1, 0)) && host.setEditorCamera)
        host.setEditorCamera(pose);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("SEQUENCE");
    f32 len = static_cast<f32>(model_.length);
    ImGui::TextUnformatted("Length");
    ImGui::SetNextItemWidth(-1);
    const bool lenChanged = ImGui::DragFloat("##seqLen", &len, std::fmax(0.1f, len * 0.004f), 0.5f, 3600.0f, "%.1f s",
                                             ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Up to 3600 s. Ctrl+click to type a value; the timeline scrolls and zooms.");
    if (ImGui::IsItemActivated()) pushUndo();
    if (lenChanged) {
        model_.length = std::fmax(0.5f, len);
        edited();
    }
    int fps = model_.fps;
    ImGui::TextUnformatted("Frames per second");
    ImGui::SetNextItemWidth(-1);
    const bool fpsChanged = ImGui::DragInt("##seqFps", &fps, 0.2f, 1, 240);
    if (ImGui::IsItemActivated()) pushUndo();
    if (fpsChanged) { model_.fps = std::clamp(fps, 1, 240); edited(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The step of fixed-step playback and of --sequence-play");
    ImGui::Checkbox("Fixed step", &fixedStep_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Preview advances exactly 1 / fps seconds per frame instead of by wall-clock time,\n"
                          "so every run gives the same camera on the same frame. Slower than real time\n"
                          "when the editor runs below that frame rate.");
    bool b = model_.loop;
    if (ImGui::Checkbox("Loop", &b)) { pushUndo(); model_.loop = b; edited(); }
    b = model_.autoplay;
    if (ImGui::Checkbox("Autoplay in Play", &b)) { pushUndo(); model_.autoplay = b; edited(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starts from 0 when Play starts, and with the packaged game");
    b = model_.camera;
    if (ImGui::Checkbox("Use sequence camera in Play", &b)) { pushUndo(); model_.camera = b; edited(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The camera track drives the view while the sequence plays");

    ImGui::Spacing();
    ImGui::TextDisabled("TRACKS");
    ImGui::BeginDisabled(sel.empty());
    if (ImGui::Button("+ Transform track (selected actor)", ImVec2(-1, 0)))
        for (const scene::Entity e : sel) addTrack(OcSeqTrackKind::Transform, e);
    if (ImGui::Button("+ Emissive track (selected actor)", ImVec2(-1, 0)))
        for (const scene::Entity e : sel) addTrack(OcSeqTrackKind::Material, e);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(findTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity) >= 0);
    if (ImGui::Button("+ Camera track", ImVec2(-1, 0))) addTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity);
    ImGui::EndDisabled();

    int removeIdx = -1;
    for (usize i = 0; i < model_.tracks.size(); ++i) {
        const OcSeqTrack& tr = model_.tracks[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kindColor(tr.kind)));
        ImGui::TextUnformatted(tr.kind == OcSeqTrackKind::Transform ? "T"
                             : tr.kind == OcSeqTrackKind::Camera ? "C" : "E");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const f32 xW = ImGui::GetFrameHeight();
        const std::string label = trackLabel(tr) + "  (" + std::to_string(tr.keys.size()) + ")";
        if (ImGui::Selectable(label.c_str(), selTrack_ == static_cast<int>(i), 0,
                              ImVec2(ImGui::GetContentRegionAvail().x - xW - 4.0f, 0)))
            selectTrack(static_cast<int>(i));
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIdx = static_cast<int>(i);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this track");
        ImGui::PopID();
    }
    if (removeIdx >= 0) removeTrack(removeIdx);
    if (model_.tracks.empty())
        ImGui::TextWrapped("Fly the camera and press K to key it, or select an actor and add a track. "
                           "Shift+K keys the selected track.");

    // The selected key.
    if (OcSeqKey* key = selectedKey()) {
        OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
        ImGui::Spacing();
        ImGui::TextDisabled("KEY");
        f32 t = static_cast<f32>(key->t);
        ImGui::TextUnformatted("Time");
        ImGui::SetNextItemWidth(-1);
        const bool tChanged = ImGui::DragFloat("##keyT", &t, 0.01f, 0.0f, static_cast<f32>(model_.length), "%.2f s");
        if (ImGui::IsItemActivated()) pushUndo();
        if (tChanged) {
            selKey_ = moveKey(tr, selKey_, t);
            edited();
        }
        key = selectedKey();
        if (key) {
            int interp = static_cast<int>(key->interp);
            ImGui::TextUnformatted("Interpolation to next key");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##keyInterp", &interp, "Smooth\0Linear\0Step\0")) {
                pushUndo();
                key->interp = static_cast<OcSeqInterp>(interp);
                edited();
            }
            if (tr.kind == OcSeqTrackKind::Material) {
                f32 col[3] = {static_cast<f32>(key->v[0]), static_cast<f32>(key->v[1]), static_cast<f32>(key->v[2])};
                ImGui::TextUnformatted("Emissive colour");
                ImGui::SetNextItemWidth(-1);
                const bool cChanged = ImGui::ColorEdit3("##keyCol", col, ImGuiColorEditFlags_Float);
                if (ImGui::IsItemActivated()) pushUndo();
                if (cChanged) {
                    key->v[0] = col[0]; key->v[1] = col[1]; key->v[2] = col[2];
                    edited();
                }
                f32 inten = static_cast<f32>(key->v[3]);
                ImGui::TextUnformatted("Intensity");
                ImGui::SetNextItemWidth(-1);
                const bool iChanged = ImGui::DragFloat("##keyInt", &inten, 0.05f, 0.0f, 1000.0f, "%.2f");
                if (ImGui::IsItemActivated()) pushUndo();
                if (iChanged) {
                    key->v[3] = std::fmax(0.0f, inten);
                    edited();
                }
            } else if (tr.kind == OcSeqTrackKind::Transform) {
                ImGui::TextDisabled("pos %.0f %.0f %.0f", key->v[0], key->v[1], key->v[2]);
                ImGui::TextDisabled("rot %.0f %.0f %.0f", key->v[3], key->v[4], key->v[5]);
                ImGui::TextDisabled("scl %.2f %.2f %.2f", key->v[6], key->v[7], key->v[8]);
                if (ImGui::Button("Set from actor now", ImVec2(-1, 0))) { pushUndo(); fillKey(tr, *key); edited(); }
            } else {
                ImGui::TextDisabled("pos %.0f %.0f %.0f", key->v[0], key->v[1], key->v[2]);
                ImGui::TextDisabled("yaw %.1f  pitch %.1f", key->v[3], key->v[4]);
                ImGui::BeginDisabled(viewIsPilot());
                if (ImGui::Button("Set from viewport camera", ImVec2(-1, 0))) { pushUndo(); fillKey(tr, *key); edited(); }
                ImGui::EndDisabled();
                if (ImGui::Button("Go to key", ImVec2(-1, 0))) goToKey(selTrack_, selKey_);
            }
            if (ImGui::Button("Delete key", ImVec2(-1, 0))) removeSelectedKeys();
        }
    }

    if (!status_.empty()) { ImGui::Spacing(); ImGui::TextDisabled("%s", status_.c_str()); }
    host_ = nullptr;
}

void SequenceEditor::drawTimeline(SequenceHost& host) {
    host_ = &host;
    const f32 ui = ImGui::GetFontSize() / 16.0f;
    const f32 winH = std::fmax(190.0f * ui, host.vpH * 0.3f);
    ImGui::SetNextWindowPos(ImVec2(host.vpX + 8.0f, host.vpY + host.vpH - winH - 8.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::fmax(360.0f * ui, host.vpW - 16.0f), winH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer", nullptr, ImGuiWindowFlags_NoCollapse)) {
        dragKey_ = dragRuler_ = false;   // a hidden window sees no mouse-up; a stuck scrub would pin the pilot view
        ImGui::End();
        host_ = nullptr;
        return;
    }
    const bool winFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGuiIO& io = ImGui::GetIO();

    // ---- transport ----
    const f64 len = std::fmax(model_.length, 0.01);
    ImGui::BeginDisabled(playActiveLast_);
    if (ImGui::Button("|<")) { player_.setTime(0); evalDirty_ = true; ++editRev_; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("To start");
    ImGui::SameLine();
    auto togglePlay = [&]() {
        if (player_.playing()) player_.pause();
        else {
            if (!model_.loop && player_.time() >= len - 1e-6) player_.setTime(0);
            player_.play();
        }
    };
    if (ImGui::Button(player_.playing() ? "Pause" : "Play ")) togglePlay();
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { player_.pause(); player_.setTime(0); evalDirty_ = true; ++editRev_; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause and return to the start; the viewport goes back to the editor camera");
    ImGui::SameLine();
    bool loop = model_.loop;
    if (ImGui::Checkbox("Loop", &loop)) { pushUndo(); model_.loop = loop; edited(); }
    ImGui::SameLine();
    ImGui::Checkbox("Fixed step", &fixedStep_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Advance 1 / fps per frame, not by wall-clock time");
    ImGui::SameLine();
    int fps = model_.fps;
    ImGui::SetNextItemWidth(64.0f * ui);
    const bool fpsChanged = ImGui::DragInt("fps", &fps, 0.2f, 1, 240);
    if (ImGui::IsItemActivated()) pushUndo();
    if (fpsChanged) { model_.fps = std::clamp(fps, 1, 240); edited(); }
    ImGui::SameLine();
    ImGui::Text("%.2f / %.2f s  f%d", player_.time(), model_.length,
                static_cast<int>(std::lround(player_.time() / game::seqStepSeconds(model_))));
    ImGui::SameLine();
    ImGui::BeginDisabled(viewIsPilot());
    if (ImGui::Button("Key camera")) keyCamera();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key the viewport camera at the playhead  (K)");
    ImGui::SameLine();
    drawKeyObjectButton("tlKeyObj", 0.0f);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (playActiveLast_) ImGui::TextDisabled("Play is running");
    else if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    // ---- hotkeys ----
    if (!playActiveLast_ && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.MouseDown[1] &&
        (winFocused || host.levelFocused)) {
        if (ImGui::IsKeyPressed(ImGuiKey_K, false)) {
            if (io.KeyShift) keySelectedTrack(); else keyCamera();
        }
    }
    if (!playActiveLast_ && winFocused && !io.WantTextInput) {
        if (keybinds().pressed(CommandId::EditUndo, io)) undoEdit();
        if (keybinds().pressed(CommandId::EditRedo, io) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))) redoEdit();
        if (!io.KeyCtrl && !io.KeyAlt) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) removeSelectedKeys();
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) { player_.setTime(0); evalDirty_ = true; ++editRev_; }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) stepFrames(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) stepFrames(1);
        }
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false) && selTrack_ >= 0 &&
            static_cast<usize>(selTrack_) < model_.tracks.size()) {
            std::fill(keySel_.begin(), keySel_.end(), char(1));
            ++editRev_;
        }
    }

    // ---- view: zoom, scroll, follow ----
    const f32 nameW = 170.0f * ui;
    const f32 rowH = ImGui::GetTextLineHeightWithSpacing() + 4.0f * ui;
    const f32 laneW = std::fmax(80.0f * ui, ImGui::GetContentRegionAvail().x - nameW - ImGui::GetStyle().ScrollbarSize - 6.0f * ui);
    // pxPerSec_ 0 fits the whole sequence in the lane; viewStart_ is the time at the lane's left edge.
    const f32 minPps = static_cast<f32>(laneW / len);
    const f32 maxPps = std::fmax(minPps, 4000.0f * ui);
    auto ppsNow = [&]() { return pxPerSec_ > minPps ? std::fmin(pxPerSec_, maxPps) : minPps; };
    auto clampView = [&]() { viewStart_ = std::clamp(viewStart_, 0.0, std::fmax(0.0, len - laneW / ppsNow())); };
    // Zoom to pps keeping anchorT at anchorPx from the lane's left edge.
    auto setZoom = [&](f32 pps, f64 anchorT, f32 anchorPx) {
        pps = std::clamp(pps, minPps, maxPps);
        pxPerSec_ = pps <= minPps * 1.001f ? 0.0f : pps;
        viewStart_ = anchorT - anchorPx / ppsNow();
        clampView();
    };
    clampView();
    {   // a playhead that moved out of view (playback, stepping, Go to key) brings the view with it
        const f64 ph = player_.time();
        if (ph != followTime_) {
            followTime_ = ph;
            const f64 vis = laneW / ppsNow();
            if (!dragRuler_ && (ph < viewStart_ || ph > viewStart_ + vis * 0.98)) {
                viewStart_ = player_.playing() ? ph - vis * 0.1 : ph - vis * 0.5;
                clampView();
            }
        }
    }

    // Zoom slider and horizontal scrollbar.
    {
        f32 z = ppsNow();
        ImGui::SetNextItemWidth(nameW - 52.0f * ui);
        if (ImGui::SliderFloat("##seqZoom", &z, minPps, maxPps, "%.0f px/s", ImGuiSliderFlags_Logarithmic))
            setZoom(z, viewStart_ + laneW / ppsNow() * 0.5, laneW * 0.5f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Timeline zoom  (Ctrl+wheel over the timeline)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) { pxPerSec_ = 0; viewStart_ = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the whole sequence");
        ImGui::SameLine(nameW);
        ImGui::InvisibleButton("##seqScroll", ImVec2(laneW, ImGui::GetFrameHeight()));
        const ImVec2 s0 = ImGui::GetItemRectMin(), s1 = ImGui::GetItemRectMax();
        const f64 vis = laneW / ppsNow();
        const f64 travel = len - vis;
        const bool scrollable = travel > 1e-6;
        const f32 thumbW = scrollable ? std::fmax(24.0f * ui, static_cast<f32>(laneW * vis / len)) : laneW;
        const f32 room = std::fmax(0.0f, laneW - thumbW);
        if (scrollable && ImGui::IsItemActivated()) {
            const f32 x0 = s0.x + static_cast<f32>(viewStart_ / travel) * room;
            scrollGrab_ = (io.MousePos.x >= x0 && io.MousePos.x <= x0 + thumbW) ? io.MousePos.x - x0 : thumbW * 0.5f;
        }
        if (scrollable && ImGui::IsItemActive() && room > 0.0f) {
            viewStart_ = std::clamp(static_cast<f64>((io.MousePos.x - scrollGrab_ - s0.x) / room), 0.0, 1.0) * travel;
            clampView();
        }
        const f32 my = (s0.y + s1.y) * 0.5f, th = 4.0f * ui;
        const f32 tx = s0.x + (scrollable ? static_cast<f32>(viewStart_ / travel) * room : 0.0f);
        ImDrawList* sdl = ImGui::GetWindowDrawList();
        sdl->AddRectFilled(ImVec2(s0.x, my - th), ImVec2(s1.x, my + th), IM_COL32(30, 30, 34, 255), th);
        sdl->AddRectFilled(ImVec2(tx, my - th), ImVec2(tx + thumbW, my + th),
                           scrollable && ImGui::IsItemHovered() ? IM_COL32(150, 150, 165, 255) : IM_COL32(100, 100, 112, 255), th);
    }

    // ---- ruler ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%d track(s)", static_cast<int>(model_.tracks.size()));
    ImGui::SameLine(nameW);
    ImGui::InvisibleButton("##ruler", ImVec2(laneW, rowH));
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    // Ctrl+wheel zooms about the mouse, Shift+wheel or a horizontal wheel scrolls, middle-drag pans.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !dragRuler_ && !dragKey_ &&
        io.MousePos.x >= r0.x && io.MousePos.x < r1.x && io.MousePos.y >= r0.y) {
        const f32 mx = io.MousePos.x - r0.x;
        if (io.KeyCtrl && io.MouseWheel != 0.0f) {
            setZoom(ppsNow() * std::pow(1.2f, io.MouseWheel), viewStart_ + mx / ppsNow(), mx);
        } else if (io.MouseWheelH != 0.0f || (io.KeyShift && io.MouseWheel != 0.0f)) {
            const f32 wheel = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
            viewStart_ -= wheel * (laneW / ppsNow()) * 0.1;
            clampView();
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            viewStart_ -= io.MouseDelta.x / ppsNow();
            clampView();
        }
    }
    // Holding a scrub or a key drag past either end of the lane scrolls toward it.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (dragRuler_ || dragKey_)) {
        const f32 over = io.MousePos.x < r0.x ? io.MousePos.x - r0.x : io.MousePos.x > r1.x ? io.MousePos.x - r1.x : 0.0f;
        if (over != 0.0f) {
            viewStart_ += over * 6.0f * io.DeltaTime / ppsNow();
            clampView();
        }
    }
    const f32 pps = ppsNow();
    auto laneX = [&](f64 t) { return r0.x + static_cast<f32>((t - viewStart_) * pps); };
    dl->PushClipRect(r0, r1, true);
    dl->AddRectFilled(r0, r1, IM_COL32(36, 36, 42, 255));
    {
        static const f64 kSteps[] = {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 1800, 3600};
        f64 step = kSteps[std::size(kSteps) - 1];
        for (const f64 s : kSteps)
            if (static_cast<f32>(s * pps) >= 70.0f * ui) { step = s; break; }
        const f64 tEnd = viewStart_ + laneW / pps;
        for (f64 i = std::floor(viewStart_ / step); i * step <= tEnd + step; ++i) {
            const f64 t = i * step;
            if (t < 0) continue;
            if (t > len + 1e-6) break;
            const f32 x = laneX(t);
            dl->AddLine(ImVec2(x, r1.y - 7.0f * ui), ImVec2(x, r1.y), IM_COL32(150, 150, 160, 255));
            if (static_cast<f32>(step * pps) >= 24.0f * ui) {   // a half-step tick
                const f32 hx = x + static_cast<f32>(step * pps) * 0.5f;
                dl->AddLine(ImVec2(hx, r1.y - 4.0f * ui), ImVec2(hx, r1.y), IM_COL32(110, 110, 120, 255));
            }
            char tb[24];
            if (step >= 60.0) std::snprintf(tb, sizeof tb, "%d:%02d", static_cast<int>(t) / 60, static_cast<int>(std::lround(t)) % 60);
            else              std::snprintf(tb, sizeof tb, "%g", t);
            dl->AddText(ImVec2(x + 3.0f * ui, r0.y), IM_COL32(170, 170, 178, 255), tb);
        }
        const f32 ex = laneX(len);   // the sequence's end
        dl->AddLine(ImVec2(ex, r0.y), ImVec2(ex, r1.y), IM_COL32(220, 90, 90, 200), 2.0f);
    }
    if (ImGui::IsItemActivated()) dragRuler_ = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragRuler_ = false;
    if (dragRuler_ && !playActiveLast_) {
        const f64 u = std::clamp(viewStart_ + static_cast<f64>(io.MousePos.x - r0.x) / pps, 0.0, len);
        player_.pause();
        player_.setTime(model_.loop ? std::min(u, len * 0.9999) : u);
        evalDirty_ = true;
        ++editRev_;
    }
    {
        const f32 x = laneX(player_.time());
        const ImVec2 tri[3] = {ImVec2(x - 5.0f * ui, r0.y), ImVec2(x + 5.0f * ui, r0.y), ImVec2(x, r0.y + 9.0f * ui)};
        dl->AddConvexPolyFilled(tri, 3, IM_COL32(255, 190, 70, 255));
    }
    dl->PopClipRect();

    // ---- track lanes ----
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool laneChild = ImGui::BeginChild("##seqLanes", ImVec2(0, 0), false);
    ImGui::PopStyleVar();
    int removeIdx = -1;
    if (laneChild) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        if (model_.tracks.empty())
            ImGui::TextDisabled("No tracks. Fly the camera and press K, or select an actor and add a track from the Mode panel.");
        const f32 hitR = 8.0f * ui;
        for (usize i = 0; i < model_.tracks.size(); ++i) {
            OcSeqTrack& tr = model_.tracks[i];
            const bool trackSel = selTrack_ == static_cast<int>(i);
            ImGui::PushID(static_cast<int>(i));
            const std::string name = trackLabel(tr);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kindColor(tr.kind)));
            if (ImGui::Selectable(name.c_str(), trackSel, 0, ImVec2(nameW - ImGui::GetStyle().WindowPadding.x - 6.0f * ui, rowH)))
                selectTrack(static_cast<int>(i));
            ImGui::PopStyleColor();
            ImGui::SameLine(r0.x - ImGui::GetWindowPos().x);   // the lane starts where the ruler does
            ImGui::InvisibleButton("##lane", ImVec2(laneW, rowH));
            const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
            const f32 cy = (p0.y + p1.y) * 0.5f;
            auto lx = [&](f64 t) { return p0.x + static_cast<f32>((t - viewStart_) * pps); };

            cdl->PushClipRect(p0, p1, true);
            cdl->AddRectFilled(p0, p1, trackSel ? IM_COL32(46, 40, 36, 255) : IM_COL32(30, 30, 34, 255));
            if (tr.keys.size() >= 2)
                cdl->AddLine(ImVec2(lx(tr.keys.front().t), cy), ImVec2(lx(tr.keys.back().t), cy),
                             IM_COL32(120, 120, 130, 140), 2.0f);
            const ImU32 col = kindColor(tr.kind);
            for (usize k = 0; k < tr.keys.size(); ++k) {
                const f32 x = lx(tr.keys[k].t);
                const bool ks = trackSel && k < keySel_.size() && keySel_[k] != 0;
                const f32 r = (ks ? 6.0f : 4.5f) * ui;
                if (tr.keys[k].interp == OcSeqInterp::Step) {
                    cdl->AddRectFilled(ImVec2(x - r * 0.8f, cy - r * 0.8f), ImVec2(x + r * 0.8f, cy + r * 0.8f),
                                       ks ? IM_COL32(255, 255, 255, 255) : col);
                } else {
                    const ImVec2 d[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
                    cdl->AddConvexPolyFilled(d, 4, ks ? IM_COL32(255, 255, 255, 255) : col);
                    cdl->AddPolyline(d, 4, IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed, 1.0f);
                }
            }
            if (trackSel) cdl->AddRect(p0, p1, IM_COL32(255, 220, 140, 150));
            const f32 phx = lx(player_.time());
            cdl->AddLine(ImVec2(phx, p0.y), ImVec2(phx, p1.y), IM_COL32(255, 190, 70, 170), 1.0f);
            cdl->PopClipRect();

            // Press decides once what it landed on; a drag then moves every selected key of the track.
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                int hit = -1;
                f32 best = hitR;
                for (usize k = 0; k < tr.keys.size(); ++k) {
                    const f32 d = std::fabs(io.MousePos.x - lx(tr.keys[k].t));
                    if (d <= best) { best = d; hit = static_cast<int>(k); }
                }
                selectTrack(static_cast<int>(i));
                clampSelection();
                const bool additive = io.KeyCtrl || io.KeyShift;
                if (hit >= 0) {
                    if (additive) {
                        keySel_[static_cast<usize>(hit)] = keySel_[static_cast<usize>(hit)] ? char(0) : char(1);
                        selKey_ = keySel_[static_cast<usize>(hit)] ? hit : -1;
                    } else if (!keySel_[static_cast<usize>(hit)]) {
                        selectOnly(hit);   // pressing a selected key keeps the group, so it can be dragged
                    } else {
                        selKey_ = hit;
                    }
                    dragKey_ = keySel_[static_cast<usize>(hit)] != 0;
                    dragStarted_ = false;
                    dragX0_ = io.MousePos.x;
                    dragView0_ = viewStart_;
                    dragOrig_.clear();
                    for (usize k = 0; k < tr.keys.size(); ++k)
                        dragOrig_.push_back({tr.keys[k], keySel_[k] != 0, static_cast<int>(k) == selKey_});
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) goToKey(static_cast<int>(i), hit);
                } else if (!additive) {
                    selectOnly(-1);
                }
                ++editRev_;
            }
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) selectTrack(static_cast<int>(i));
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragKey_ = false;
            if (dragKey_ && active && trackSel && !dragOrig_.empty() && dragOrig_.size() == tr.keys.size()) {
                // Pixels moved plus the scroll since the press, in seconds; whole 10 ms (1 ms when zoomed in).
                const f64 raw = static_cast<f64>(io.MousePos.x - dragX0_) / pps + (viewStart_ - dragView0_);
                const f64 snap = pps >= 600.0f * ui ? 1000.0 : 100.0;
                const f64 delta = std::round(raw * snap) / snap;
                if (dragStarted_ || std::fabs(delta) > 1e-9) {
                    if (!dragStarted_) { pushUndo(); dragStarted_ = true; }
                    applyDrag(tr, delta);
                    edited();
                }
            }

            if (ImGui::BeginPopupContextItem("##laneCtx")) {
                const bool haveSel = trackSel && selectedCount() > 0;
                if (ImGui::MenuItem("Key at playhead")) { selectTrack(static_cast<int>(i)); addKeyAtPlayhead(); }
                if (ImGui::MenuItem("Go to selected key", nullptr, false, haveSel && selKey_ >= 0))
                    goToKey(selTrack_, selKey_);
                if (ImGui::MenuItem("Select all keys", "Ctrl+A", false, !tr.keys.empty())) {
                    selectTrack(static_cast<int>(i));
                    clampSelection();
                    std::fill(keySel_.begin(), keySel_.end(), char(1));
                    ++editRev_;
                }
                if (ImGui::BeginMenu("Interpolation of selected", haveSel)) {
                    if (ImGui::MenuItem("Smooth")) setSelectedInterp(OcSeqInterp::Smooth);
                    if (ImGui::MenuItem("Linear")) setSelectedInterp(OcSeqInterp::Linear);
                    if (ImGui::MenuItem("Step"))   setSelectedInterp(OcSeqInterp::Step);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Delete selected keys", "Del", false, haveSel)) removeSelectedKeys();
                ImGui::Separator();
                if (ImGui::MenuItem("Clear track", nullptr, false, !tr.keys.empty())) clearTrack(static_cast<int>(i));
                if (ImGui::MenuItem("Remove track")) removeIdx = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (removeIdx >= 0) removeTrack(removeIdx);
    clampSelection();
    ImGui::End();
    host_ = nullptr;
}

#else  // !AVER_WITH_IMGUI

void SequenceEditor::drawModePanel(SequenceHost&) {}
void SequenceEditor::drawTimeline(SequenceHost&) {}

#endif

} // namespace aver::editor

#endif // AVER_MODULE_SCENE
