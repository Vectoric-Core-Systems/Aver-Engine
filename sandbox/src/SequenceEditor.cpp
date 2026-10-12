// The level sequence editor behind the Animate mode (SequenceEditor.hpp).
#include "SequenceEditor.hpp"

#if AVER_MODULE_SCENE
#include "SequenceCurveOps.hpp"
#include "SequenceTrackOps.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcSeq.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/game/PlayMobility.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/Components.hpp"
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
scene::Entity entityOfTarget(i32 t) { return t > 0 ? static_cast<scene::Entity>(static_cast<u32>(t)) : scene::kInvalidEntity; }

u64 mix(u64 h, u64 v) { return (h ^ v) * 1099511628211ull; }

bool hasCameraIn(const fmt::OcSequence& s) {
    for (const OcSeqTrack& tr : s.tracks) {
        if (tr.kind != OcSeqTrackKind::Camera || tr.mute) continue;
        if (tr.target > 0 || !tr.bind.empty() || !tr.keys.empty()) return true;
    }
    return false;
}

// A sequence name no other sequence of the level has: `base`, then "base 2", "base 3" ...
std::string uniqueName(const std::vector<std::string>& taken, const std::string& base) {
    const auto used = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
    if (!base.empty() && !used(base)) return base;
    const std::string stem = base.empty() ? std::string("Sequence") : base;
    for (int i = 2;; ++i) {
        const std::string n = stem + " " + std::to_string(i);
        if (!used(n)) return n;
    }
}

#if AVER_WITH_IMGUI
ImU32 kindColor(OcSeqTrackKind k) {
    switch (k) {
        case OcSeqTrackKind::Transform:   return IM_COL32(242, 150, 60, 255);
        case OcSeqTrackKind::Camera:      return IM_COL32(110, 180, 255, 255);
        case OcSeqTrackKind::Material:    return IM_COL32(240, 220, 90, 255);
        case OcSeqTrackKind::Property:    return IM_COL32(120, 220, 140, 255);
        case OcSeqTrackKind::Event:       return IM_COL32(255, 120, 150, 255);
        case OcSeqTrackKind::Audio:       return IM_COL32(190, 140, 255, 255);
        case OcSeqTrackKind::Animation:   return IM_COL32(100, 220, 220, 255);
        case OcSeqTrackKind::Visibility:  return IM_COL32(200, 200, 200, 255);
        case OcSeqTrackKind::CameraCut:   return IM_COL32(255, 170, 90, 255);
        case OcSeqTrackKind::SubSequence: return IM_COL32(150, 190, 255, 255);
    }
    return IM_COL32(255, 255, 255, 255);
}

// A stable colour per camera name, so cut keys read against the camera they cut to.
ImU32 nameColor(const std::string& n) {
    const u64 h = fnv1a64(std::string_view(n));
    return IM_COL32(110 + static_cast<int>(h & 0x7F), 110 + static_cast<int>((h >> 8) & 0x7F),
                    110 + static_cast<int>((h >> 16) & 0x7F), 255);
}

std::string fileStem(const std::string& path) {
    const usize slash = path.find_last_of("/\\");
    std::string f = slash == std::string::npos ? path : path.substr(slash + 1);
    return f;
}

// A text field backed by a std::string; true on the frame the value changed. A bounded copy, so an
// untouched long value is never truncated by opening the page.
bool inputText(const char* label, std::string& value, usize cap = 256, ImGuiInputTextFlags flags = 0) {
    std::vector<char> buf(cap, '\0');
    const usize n = std::min(value.size(), cap - 1);
    std::memcpy(buf.data(), value.data(), n);
    const bool changed = ImGui::InputText(label, buf.data(), cap, flags);
    if (changed) value.assign(buf.data());
    return changed;
}

std::vector<std::string> payloadPaths(const ImGuiPayload* p) {
    std::vector<std::string> out;
    if (!p || !p->Data) return out;
    const std::string blob(static_cast<const char*>(p->Data), p->DataSize > 0 ? static_cast<usize>(p->DataSize - 1) : usize(0));
    usize from = 0;
    while (from <= blob.size()) {
        const usize nl = blob.find('\n', from);
        const std::string one = blob.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!one.empty()) out.push_back(one);
        if (nl == std::string::npos) break;
        from = nl + 1;
    }
    return out;
}

constexpr const char* kPayloadSet = "AVER_CB_MOVE_SET";
constexpr const char* kPayloadOne = "AVER_ASSET_PATH";
constexpr const char* kPayloadTrack = "AVER_SEQ_TRACK";
#endif

} // namespace

// ---- level lifecycle ---------------------------------------------------------------------------

void SequenceEditor::reset() {
    model_ = fmt::OcSequence{};
    model_.name = "Main";
    others_.clear();
    curSlot_ = 0;
    player_ = game::SequencePlayer{};
    director_.clear();
    library_.clear();
    view_.clear();
    dense_.clear();
    undo_.clear();
    playRunning_ = false;
    running_ = false;
    evalDirty_ = true;
    editTime_ = 0;
    selTrack_ = -1;
    selKey_ = -1;
    keySel_.clear();
    drag_ = LaneDrag::None;
    dragRuler_ = dragStarted_ = false;
    dragOrig_.clear();
    pxPerSec_ = 0;
    viewStart_ = 0;
    followTime_ = -1;
    status_.clear();
    linkedFile_.clear();
    linkedDirty_ = false;
    ++editRev_;
    syncPlayer();
}

void SequenceEditor::load(const std::vector<fmt::OcSequence>& seqs, const std::vector<scene::Entity>& placementEntities,
                          const std::function<scene::Entity(const std::string&)>& findByName) {
    reset();
    if (seqs.empty()) return;
    std::vector<std::string> dropped;
    std::vector<std::string> names;
    std::vector<fmt::OcSequence> all;
    for (const fmt::OcSequence& s : seqs) {
        fmt::OcSequence c = seqFromFile(s, placementEntities, findByName, &dropped);
        c.name = uniqueName(names, c.name.empty() ? (all.empty() ? std::string("Main") : std::string("Sequence")) : c.name);
        names.push_back(c.name);
        all.push_back(std::move(c));
    }
    model_ = std::move(all.front());
    others_.assign(std::make_move_iterator(all.begin() + 1), std::make_move_iterator(all.end()));
    curSlot_ = 0;
    syncPlayer();
    for (const std::string& d : dropped) AVER_WARN("[Sequence] dropped a track: {}", d);
    AVER_INFO("[Sequence] loaded {} sequence(s); '{}': {} track(s), {:.1f} s{}", (u32)others_.size() + 1u, model_.name,
              (u32)model_.tracks.size(), model_.length, dropped.empty() ? "" : " (some tracks named missing placements and were dropped)");
}

void SequenceEditor::save(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf,
                          const std::function<std::string(scene::Entity)>& bindName) const {
    w.sequences.clear();
    std::vector<const fmt::OcSequence*> order;
    for (usize i = 0, o = 0; i < others_.size() + 1; ++i) order.push_back(i == curSlot_ ? &model_ : &others_[o++]);
    for (const fmt::OcSequence* s : order) {
        std::vector<std::string> dropped;
        fmt::OcSequence out = seqToFile(*s, slotOf, bindName, &dropped);
        for (const std::string& d : dropped) AVER_WARN("[Sequence] '{}': dropped a track on save: {}", s->name, d);
        if (out.tracks.empty()) continue;   // no tracks writes nothing
        w.sequences.push_back(std::move(out));
    }
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
    drag_ = LaneDrag::None;
    dragRuler_ = false;
    player_.pause();
}

// ---- frame -------------------------------------------------------------------------------------

bool SequenceEditor::animationTracks() const {
    for (const OcSeqTrack& tr : model_.tracks)
        if (!tr.mute && (tr.kind == OcSeqTrackKind::Animation || tr.kind == OcSeqTrackKind::SubSequence)) return true;
    return false;
}

void SequenceEditor::tickPre(const SequenceTickCtx& c) {
    playActiveLast_ = c.playActive;
    scene::World& w = scene::World::instance();
    if (playRunning_) {
        if (!c.playActive) { endPlay(); return; }   // the session never started or ended without a Stop
        if (!c.playPaused) director_.advance(c.dt);
        director_.evaluate(w, game::SeqPhase::Pre);
        return;
    }
    if (!active_ || c.playActive) return;
    if (!running_ && player_.playing()) {
        player_.advance(fixedStep_ ? game::seqStepSeconds(model_) : c.dt);
        evalDirty_ = true;
    }
    if (evalDirty_) player_.evaluate(w, game::SeqPhase::Pre);
    else if (animationTracks()) player_.evaluateAnimation(w);   // driven players lapse without a call
}

void SequenceEditor::tickPost() {
    scene::World& w = scene::World::instance();
    if (playRunning_) {
        director_.evaluate(w, game::SeqPhase::Post);
        return;
    }
    if (!active_ || playActiveLast_) return;
    if (evalDirty_) {
        player_.evaluate(w, game::SeqPhase::Post);
        evalDirty_ = false;
    }
}

void SequenceEditor::restoreBases() {
    player_.restore(scene::World::instance());
    evalDirty_ = true;
}

// An actor with no unmuted track left goes back to where it was authored.
void SequenceEditor::releaseUndrivenBases() {
    game::SeqBaseStore& store = player_.baseStore();
    if (store.empty()) return;
    std::unordered_set<scene::Entity> driven;
    for (const scene::Entity e : player_.boundEntities()) driven.insert(e);
    store.releaseUndriven(scene::World::instance(), driven);
}

void SequenceEditor::setHooks(const game::SequenceHooks& h) {
    hooks_ = h;
    hooksSet_ = true;
    player_.setHooks(&hooks_);
}

void SequenceEditor::setAudition(bool on) {
    audition_ = on;
    player_.setAudition(on);
}

// ---- Play --------------------------------------------------------------------------------------

void SequenceEditor::beginPlay(game::PlayMobility& mobility, const game::SequenceHooks& hooks) {
    playRunning_ = false;
    player_.pause();
    syncPlayer();
    director_.clear();
    director_.setMode(game::SeqMode::Live);
    director_.setHooks(hooks);
    std::vector<fmt::OcSequence> seqs;
    for (const fmt::OcSequence& s : view_)
        if (!s.tracks.empty()) seqs.push_back(s);
    if (seqs.empty()) return;
    director_.setSequences(std::move(seqs));
    game::SeqBindings b;
    b.byPlacement = dense_;
    b.find = [](const std::string& n) { return scene::World::instance().find(n); };
    director_.setBindings(std::move(b));
    director_.startAutoplay();
    if (!director_.playingAny()) { director_.restore(scene::World::instance()); director_.clear(); return; }
    editTime_ = player_.time();
    mobility.seedMovable(director_.boundEntities());
    playRunning_ = true;
    evalDirty_ = true;
}

void SequenceEditor::endPlay() {
    if (!playRunning_) return;
    playRunning_ = false;
    director_.restore(scene::World::instance());
    director_.clear();
    player_.pause();
    player_.setTime(editTime_);
    evalDirty_ = true;
}

std::vector<scene::Entity> SequenceEditor::trackEntities() const {
    std::vector<scene::Entity> out;
    const auto take = [&](const fmt::OcSequence& s) {
        for (const OcSeqTrack& tr : s.tracks)
            if (tr.target > 0) out.push_back(entityOfTarget(tr.target));
    };
    take(model_);
    for (const fmt::OcSequence& s : others_) take(s);
    return out;
}

bool SequenceEditor::drivesTransform(scene::Entity e) const {
    const auto drives = [&](const fmt::OcSequence& s) {
        for (const OcSeqTrack& tr : s.tracks)
            if (tr.kind == OcSeqTrackKind::Transform && tr.target == targetOf(e)) return true;
        return false;
    };
    if (drives(model_)) return true;
    for (const fmt::OcSequence& s : others_)
        if (drives(s)) return true;
    return false;
}

// ---- view --------------------------------------------------------------------------------------

bool SequenceEditor::viewPose(bool playEjected, game::SeqCameraPose& out) const {
    if (playRunning_) return !playEjected && director_.camera(out);
    if (viewIsPilot()) return player_.camera(out);
    return false;
}

bool SequenceEditor::emissiveScale(scene::Entity e, f32 out[3]) const {
    if (playRunning_) return director_.emissiveScale(e, out);
    return previewing() && player_.emissiveScale(e, out);
}

// ---- camera path overlay -----------------------------------------------------------------------

bool SequenceEditor::pathLines(std::vector<rhi::LineVertex>& out) const {
    out.clear();
    std::vector<int> cams;   // free camera tracks with keys
    for (usize i = 0; i < model_.tracks.size(); ++i) {
        const OcSeqTrack& tr = model_.tracks[i];
        if (tr.kind == OcSeqTrackKind::Camera && tr.target <= 0 && tr.bind.empty() && !tr.keys.empty() && !tr.mute)
            cams.push_back(static_cast<int>(i));
    }
    if (cams.empty()) return false;

    const auto keyPos = [](const OcSeqKey& k) { return Vec3{(f32)k.v[0], (f32)k.v[1], (f32)k.v[2]}; };
    Vec3 lo = keyPos(model_.tracks[static_cast<usize>(cams.front())].keys.front()), hi = lo;
    for (const int ci : cams)
        for (const OcSeqKey& k : model_.tracks[static_cast<usize>(ci)].keys) {
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

    constexpr int kPerSegment = 24;
    for (const int ci : cams) {
        const OcSeqTrack& cam = model_.tracks[static_cast<usize>(ci)];
        const bool trackSel = selTrack_ == ci;
        const f32 dim = trackSel ? 1.0f : 0.55f;
        // The curve, as the sampler gives it.
        game::SeqCameraPose prev, cur;
        for (usize i = 0; i + 1 < cam.keys.size(); ++i) {
            const f64 t0 = cam.keys[i].t, t1 = cam.keys[i + 1].t;
            for (int j = 0; j <= kPerSegment; ++j) {
                const f64 t = t0 + (t1 - t0) * (static_cast<f64>(j) / kPerSegment);
                if (!game::sampleSeqCamera(cam, t, cur)) continue;
                if (j > 0) line(prev.position, cur.position, 0.45f * dim, 0.75f * dim, 1.0f * dim);
                prev = cur;
            }
        }
        // A marker and a view direction at every key; the selected ones are white.
        for (usize k = 0; k < cam.keys.size(); ++k) {
            const OcSeqKey& key = cam.keys[k];
            const bool sel = trackSel && k < keySel_.size() && keySel_[k] != 0;
            const f32 r = dim, g = sel ? 1.0f : 0.6f * dim, b = sel ? 1.0f : 0.2f * dim;
            const Vec3 p = keyPos(key);
            cross(p, size * (sel ? 1.4f : 1.0f), r, g, b);
            const f32 yaw = static_cast<f32>(key.v[3] * 3.14159265358979 / 180.0);
            const f32 pitch = static_cast<f32>(key.v[4] * 3.14159265358979 / 180.0);
            line(p, p + game::cameraForward(yaw, pitch) * (size * 4.0f), r, g, b);
        }
    }

    // Cut boundaries: a magenta marker where the cut key's camera is at that time.
    const int cut = seqCutTrack(model_);
    if (cut >= 0) {
        for (const OcSeqKey& k : model_.tracks[static_cast<usize>(cut)].keys)
            for (const int ci : cams) {
                const OcSeqTrack& cam = model_.tracks[static_cast<usize>(ci)];
                if (game::seqCameraName(cam) != k.text) continue;
                game::SeqCameraPose p;
                if (game::sampleSeqCamera(cam, k.t, p)) cross(p.position, size * 2.2f, 1.0f, 0.2f, 0.8f);
            }
    }

    // The live camera at the playhead.
    game::SeqCameraPose cur;
    if (player_.camera(cur)) {
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
    h = mix(h, bits);
    return mix(h, fnv1a64(std::string_view(player_.cameraName())));
}

// ---- run mode ----------------------------------------------------------------------------------

bool SequenceEditor::hasCamera() const { return hasCameraIn(model_); }

bool SequenceEditor::anyCamera() const {
    if (hasCameraIn(model_)) return true;
    for (const fmt::OcSequence& s : others_)
        if (hasCameraIn(s)) return true;
    return false;
}

std::vector<std::string> SequenceEditor::sequenceNames() const {
    std::vector<std::string> out;
    for (usize i = 0, o = 0; i < others_.size() + 1; ++i) out.push_back(i == curSlot_ ? model_.name : others_[o++].name);
    return out;
}

bool SequenceEditor::selectRunSequence(const std::string& name, std::string* why) {
    const std::vector<std::string> names = sequenceNames();
    int slot = -1;
    for (usize i = 0, o = 0; i < names.size(); ++i) {
        const fmt::OcSequence& s = i == curSlot_ ? model_ : others_[o++];
        if (name.empty() ? hasCameraIn(s) : s.name == name) { slot = static_cast<int>(i); break; }
    }
    if (slot < 0) {
        if (why) {
            *why = name.empty() ? "no sequence of the level has a camera; its sequences are:" : "no sequence is called '" + name + "'; the level's are:";
            for (const std::string& n : names) *why += " '" + n + "'";
            if (names.empty()) *why += " (none)";
        }
        return false;
    }
    if (static_cast<usize>(slot) != curSlot_) switchSequence(static_cast<usize>(slot));
    return true;
}

void SequenceEditor::beginRun() {
    running_ = true;
    fixedStep_ = true;
    pilot_ = true;
    player_.pause();
    player_.setMode(game::SeqMode::Offline);
    player_.setTime(0);
    evalDirty_ = true;
}

void SequenceEditor::endRun() {
    running_ = false;
    pilot_ = false;
    player_.setMode(game::SeqMode::Preview);
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

void SequenceEditor::runAt(f64 seconds) {
    // Just under the end: at length a looping sequence wraps back to 0.
    player_.setTime(std::clamp(seconds, 0.0, std::max(0.0, model_.length - 1e-6)));
    evalDirty_ = true;
}

// ---- model -------------------------------------------------------------------------------------

// The player gets a copy of every sequence with each actor rewritten to an index into one dense entity
// list (so a sub-sequence reads the same table); a track whose entity is gone is left out of the copy,
// not out of the model.
void SequenceEditor::syncPlayer() {
    scene::World& w = scene::World::instance();
    dense_.clear();
    std::unordered_map<u32, i32> index;
    const auto convert = [&](const fmt::OcSequence& src) {
        fmt::OcSequence seq = src;
        seq.tracks.clear();
        for (const OcSeqTrack& tr : src.tracks) {
            OcSeqTrack c = tr;
            if (tr.target > 0) {
                const scene::Entity e = entityOfTarget(tr.target);
                if (!w.valid(e)) continue;
                auto it = index.find(static_cast<u32>(e));
                if (it == index.end()) {
                    it = index.emplace(static_cast<u32>(e), static_cast<i32>(dense_.size())).first;
                    dense_.push_back(e);
                }
                c.target = it->second;
                c.bind.clear();   // the entity is the binding; a name could pick another actor
            }
            seq.tracks.push_back(std::move(c));
        }
        return seq;
    };
    view_.clear();
    for (usize i = 0, o = 0; i < others_.size() + 1; ++i) view_.push_back(convert(i == curSlot_ ? model_ : others_[o++]));
    library_.setLevel(&view_);
    player_.setSequence(view_[curSlot_]);
    game::SeqBindings b;
    b.byPlacement = dense_;
    b.find = [](const std::string& n) { return scene::World::instance().find(n); };
    player_.setBindings(std::move(b));
    player_.setLibrary(&library_);
    player_.setHooks(hooksSet_ ? &hooks_ : nullptr);
    player_.setMode(running_ ? game::SeqMode::Offline : game::SeqMode::Preview);
    player_.setAudition(audition_);
    player_.setTime(player_.time());   // re-wrapped against the (possibly new) length
}

void SequenceEditor::edited() {
    syncPlayer();
    releaseUndrivenBases();
    evalDirty_ = true;
    ++editRev_;
    if (!linkedFile_.empty()) linkedDirty_ = true;
    if (host_ && host_->markLevelDirty) host_->markLevelDirty();
}

void SequenceEditor::undoEdit() {
    if (!undo_.undo(model_)) return;
    status_ = "Undo";
    clampSelection();
    edited();
}

void SequenceEditor::redoEdit() {
    if (!undo_.redo(model_)) return;
    status_ = "Redo";
    clampSelection();
    edited();
}

int SequenceEditor::findTrack(OcSeqTrackKind kind, scene::Entity e) const {
    for (usize i = 0; i < model_.tracks.size(); ++i) {
        const OcSeqTrack& tr = model_.tracks[i];
        if (tr.kind != kind) continue;
        if (kind == OcSeqTrackKind::Camera) {
            if (e == scene::kInvalidEntity ? (tr.target <= 0 && tr.bind.empty()) : tr.target == targetOf(e)) return static_cast<int>(i);
        } else if (kind == OcSeqTrackKind::CameraCut) {
            return static_cast<int>(i);
        } else if (tr.target == targetOf(e)) {
            return static_cast<int>(i);
        }
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
    curve_.fit = true;
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

// The key's values from the live scene: the actor's transform, the editor camera, a property's current
// value, or a neutral key for the kinds that have no live source.
void SequenceEditor::fillKey(const OcSeqTrack& tr, OcSeqKey& key) const {
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOfTarget(tr.target);
    switch (tr.kind) {
        case OcSeqTrackKind::Transform:
            if (w.valid(e)) game::seqKeyFromTransform(w.localTransform(e), key);
            break;
        case OcSeqTrackKind::Camera:
            if (host_ && tr.target <= 0) game::seqKeyFromCamera(host_->editorCamera, key);
            break;
        case OcSeqTrackKind::Material:
            key.v[0] = key.v[1] = key.v[2] = key.v[3] = 1.0;
            break;
        case OcSeqTrackKind::Property:
            seqKeyFromField(w, e, tr.property, key);
            break;
        case OcSeqTrackKind::Visibility: {
            const auto* mr = w.valid(e) ? w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer) : nullptr;
            key.v[0] = (!mr || (mr->flags & scene::kMeshRendererVisible)) ? 1.0 : 0.0;
            key.v[1] = 1.0;
            key.interp = OcSeqInterp::Step;
            break;
        }
        case OcSeqTrackKind::CameraCut:
            for (const OcSeqTrack& c : model_.tracks)
                if (c.kind == OcSeqTrackKind::Camera) { key.text = game::seqCameraName(c); break; }
            key.interp = OcSeqInterp::Step;
            break;
        case OcSeqTrackKind::Event:
            key.text = eventName_;
            key.interp = OcSeqInterp::Step;
            break;
        default: break;
    }
}

std::string SequenceEditor::trackLabel(const OcSeqTrack& tr) const {
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityOfTarget(tr.target);
    std::string actor;
    if (tr.target > 0) {
        if (!w.valid(e)) actor = "<deleted>";
        else if (host_ && host_->label) actor = host_->label(e);
        else actor = "Entity " + std::to_string(static_cast<u32>(e));
    } else if (!tr.bind.empty()) {
        actor = tr.bind;
    }
    switch (tr.kind) {
        case OcSeqTrackKind::Camera:
            return game::seqCameraName(tr) + (actor.empty() ? std::string() : " (" + actor + ")");
        case OcSeqTrackKind::CameraCut:   return "Camera Cuts";
        case OcSeqTrackKind::Event:       return actor.empty() ? tr.name.empty() ? std::string("Events") : tr.name : actor + " (events)";
        case OcSeqTrackKind::Audio:       return actor.empty() ? std::string("Audio") : actor + " (audio)";
        case OcSeqTrackKind::SubSequence: return tr.name.empty() ? std::string("Sub-sequences") : tr.name;
        case OcSeqTrackKind::Material:    return actor + " (emissive)";
        case OcSeqTrackKind::Property:    return actor + " . " + tr.property;
        case OcSeqTrackKind::Visibility:  return actor + " (visible)";
        case OcSeqTrackKind::Animation:   return actor + " (animation)";
        default:                          return actor;
    }
}

const char* SequenceEditor::bindBadge(const OcSeqTrack& tr, bool& red) const {
    red = false;
    const bool alive = tr.target > 0 && scene::World::instance().valid(entityOfTarget(tr.target));
    switch (seqBindState(tr, alive)) {
        case SeqBindState::Free:      return "";
        case SeqBindState::Placement: return "";
        case SeqBindState::Named:     red = true; return "unbound";
        case SeqBindState::Unbound:   red = true; return "no actor";
    }
    return "";
}

// Insert a key at t, keeping the keys sorted; returns its index.
static int insertKeySorted(OcSeqTrack& tr, const OcSeqKey& k) {
    const auto it = std::upper_bound(tr.keys.begin(), tr.keys.end(), k.t,
                                     [](f64 t, const OcSeqKey& a) { return t < a.t; });
    // Two statements -- see SequenceCurveOps.cpp's insertSorted for the evaluation-order bug one hides.
    const auto at = tr.keys.insert(it, k);
    return static_cast<int>(at - tr.keys.begin());
}

int SequenceEditor::addCameraTrack(scene::Entity actor) {
    scene::World& w = scene::World::instance();
    if (actor != scene::kInvalidEntity && !w.valid(actor)) return -1;
    pushUndo();
    OcSeqTrack tr = seqNewTrack(OcSeqTrackKind::Camera);
    if (actor != scene::kInvalidEntity) {
        tr.target = targetOf(actor);   // an actor camera has no keys: its pose is the actor's
        if (host_ && host_->bindName) tr.bind = host_->bindName(actor);
    } else {
        OcSeqKey k;   // a free camera is seeded with a key at the playhead
        k.t = std::clamp(player_.time(), 0.0, model_.length);
        fillKey(tr, k);
        tr.keys.push_back(k);
    }
    const int idx = seqAddTrack(model_, std::move(tr));
    selTrack_ = -1;
    selectTrack(idx);
    if (actor == scene::kInvalidEntity) selectOnly(0);
    edited();
    return idx;
}

int SequenceEditor::addTrack(OcSeqTrackKind kind, scene::Entity e) {
    scene::World& w = scene::World::instance();
    if (kind == OcSeqTrackKind::Camera) {
        if (e == scene::kInvalidEntity) {   // a free camera: reuse the first one when asked from K
            const int existing = findTrack(kind, e);
            if (existing >= 0) { selectTrack(existing); return existing; }
        }
        return addCameraTrack(e);
    }
    if (kind == OcSeqTrackKind::CameraCut) {
        const int existing = seqCutTrack(model_);
        if (existing >= 0) { selectTrack(existing); return existing; }
        pushUndo();
        OcSeqTrack tr = seqNewTrack(kind);
        OcSeqKey k = seqNeutralKey(kind, 0.0);
        fillKey(tr, k);
        tr.keys.push_back(k);
        const int idx = seqAddTrack(model_, std::move(tr));
        selTrack_ = -1;
        selectTrack(idx);
        selectOnly(0);
        edited();
        return idx;
    }
    if (kind == OcSeqTrackKind::Event) {
        pushUndo();
        OcSeqTrack tr = seqNewTrack(kind);
        if (w.valid(e)) {
            tr.target = targetOf(e);
            if (host_ && host_->bindName) tr.bind = host_->bindName(e);
        }
        const int idx = seqAddTrack(model_, std::move(tr));
        selTrack_ = -1;
        selectTrack(idx);
        edited();
        return idx;
    }
    if (!w.valid(e)) return -1;
    const int existing = findTrack(kind, e);
    if (existing >= 0) { selectTrack(existing); return existing; }
    pushUndo();
    OcSeqTrack tr = seqNewTrack(kind);
    tr.target = targetOf(e);
    if (host_ && host_->bindName) tr.bind = host_->bindName(e);
    // Seeded with a key at the playhead so the track does something at once.
    OcSeqKey k = seqNeutralKey(kind, std::clamp(player_.time(), 0.0, model_.length));
    fillKey(tr, k);
    tr.keys.push_back(k);
    const int idx = seqAddTrack(model_, std::move(tr));
    selTrack_ = -1;
    selectTrack(idx);
    selectOnly(0);
    edited();
    return idx;
}

int SequenceEditor::addPropertyTrack(scene::Entity e, const std::string& qualified) {
    scene::World& w = scene::World::instance();
    if (!w.valid(e)) { status_ = "Select an actor first"; return -1; }
    const scene::FieldDesc* d = w.field(w.fieldId(qualified));
    if (!d || !seqFieldSequenceable(*d)) { status_ = "That field cannot be sequenced"; return -1; }
    for (usize i = 0; i < model_.tracks.size(); ++i)
        if (model_.tracks[i].kind == OcSeqTrackKind::Property && model_.tracks[i].target == targetOf(e) && model_.tracks[i].property == qualified) {
            selectTrack(static_cast<int>(i));
            return static_cast<int>(i);
        }
    pushUndo();
    OcSeqTrack tr = seqNewTrack(OcSeqTrackKind::Property);
    tr.target = targetOf(e);
    tr.property = qualified;
    if (host_ && host_->bindName) tr.bind = host_->bindName(e);
    OcSeqKey k;
    k.t = std::clamp(player_.time(), 0.0, model_.length);
    fillKey(tr, k);
    tr.keys.push_back(k);
    const int idx = seqAddTrack(model_, std::move(tr));
    selTrack_ = -1;
    selectTrack(idx);
    selectOnly(0);
    status_ = "Property track " + qualified;
    edited();
    return idx;
}

// A section lane for a dropped or typed asset: audio (optionally positional on `e`), an animation on a
// skeletal actor, or another sequence. The section starts at the playhead.
int SequenceEditor::addAssetTrack(OcSeqTrackKind kind, scene::Entity e, const std::string& assetPath) {
    scene::World& w = scene::World::instance();
    if (assetPath.empty()) { status_ = "Give the asset a path"; return -1; }
    if (kind == OcSeqTrackKind::Animation && !w.valid(e)) { status_ = "Select the skeletal actor to animate"; return -1; }
    pushUndo();
    OcSeqTrack tr = seqNewTrack(kind);
    if (w.valid(e)) {
        tr.target = targetOf(e);
        if (host_ && host_->bindName) tr.bind = host_->bindName(e);
    }
    const int idx = seqAddTrack(model_, std::move(tr));
    selTrack_ = -1;
    selectTrack(idx);
    OcSeqTrack& added = model_.tracks[static_cast<usize>(idx)];
    // Default lengths: a sub-sequence's own, a clip's own, two seconds for a sound or a montage.
    f64 dur = 2.0;
    if (kind == OcSeqTrackKind::SubSequence) {
        for (const fmt::OcSequence& s : view_) if (s.name == assetPath) { dur = s.length; break; }
    } else if (kind == OcSeqTrackKind::Animation && seqClassifyAsset(assetPath) == SeqAssetKind::Clip) {
        if (const fmt::OcAnimation* c = anim::animSystem().clip(fnv1a64(std::string_view(assetPath)))) dur = c->duration;
    }
    const int k = seqAddSection(added, std::clamp(player_.time(), 0.0, model_.length), dur, assetPath, model_.length);
    if (k >= 0 && kind == OcSeqTrackKind::Audio && w.valid(e)) added.keys[static_cast<usize>(k)].v[3] = 1.0;   // positional
    if (k >= 0) selectOnly(k);
    edited();
    return idx;
}

void SequenceEditor::removeTrack(int index) {
    if (index < 0 || static_cast<usize>(index) >= model_.tracks.size()) return;
    pushUndo();
    model_.tracks.erase(model_.tracks.begin() + index);
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

void SequenceEditor::moveTrack(int from, int to) {
    const int n = static_cast<int>(model_.tracks.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    pushUndo();
    OcSeqTrack t = std::move(model_.tracks[static_cast<usize>(from)]);
    model_.tracks.erase(model_.tracks.begin() + from);
    model_.tracks.insert(model_.tracks.begin() + to, std::move(t));
    if (selTrack_ == from) selTrack_ = to;
    else if (from < selTrack_ && to >= selTrack_) --selTrack_;
    else if (from > selTrack_ && to <= selTrack_) ++selTrack_;
    edited();
}

void SequenceEditor::setMute(int index, bool mute) {
    if (index < 0 || static_cast<usize>(index) >= model_.tracks.size()) return;
    if (model_.tracks[static_cast<usize>(index)].mute == mute) return;
    pushUndo();
    model_.tracks[static_cast<usize>(index)].mute = mute;
    edited();   // the player releases the muted track's animation and voices; the store puts back what nothing drives
}

void SequenceEditor::keyCamera() {
    if (!host_) return;
    if (viewIsPilot()) {
        status_ = player_.playing() ? "Pause playback to key the camera"
                                    : "Untick 'Lock viewport' to key the editor camera";
        return;
    }
    int ti = -1;   // the selected free camera, else the first one
    if (selTrack_ >= 0 && static_cast<usize>(selTrack_) < model_.tracks.size()) {
        const OcSeqTrack& s = model_.tracks[static_cast<usize>(selTrack_)];
        if (s.kind == OcSeqTrackKind::Camera && s.target <= 0 && s.bind.empty()) ti = selTrack_;
    }
    if (ti < 0) ti = findTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity);
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
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    if (seqKindIsSection(tr.kind)) { status_ = "Drop an asset on the lane, or use Add section"; return; }
    if (tr.kind == OcSeqTrackKind::Camera && (tr.target > 0 || !tr.bind.empty())) {
        status_ = "An actor camera follows its actor; it has no keys";
        return;
    }
    pushUndo();
    const f64 t = std::clamp(player_.time(), 0.0, model_.length);
    int idx = -1;
    for (usize i = 0; i < tr.keys.size(); ++i)
        if (std::fabs(tr.keys[i].t - t) < kSameKeySec) { idx = static_cast<int>(i); break; }
    if (idx >= 0 && tr.kind != OcSeqTrackKind::Event) {
        // Re-keying: new pose, same time and interpolation. A material key keeps its colour; the
        // panel edits that. An event, cut or visibility key keeps what it says.
        if (tr.kind == OcSeqTrackKind::Transform || tr.kind == OcSeqTrackKind::Camera || tr.kind == OcSeqTrackKind::Property)
            fillKey(tr, tr.keys[static_cast<usize>(idx)]);
    } else {
        OcSeqKey k = seqNeutralKey(tr.kind, t);
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

void SequenceEditor::addEventKey(const std::string& name) {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size() ||
        model_.tracks[static_cast<usize>(selTrack_)].kind != OcSeqTrackKind::Event) {
        int ti = -1;
        for (usize i = 0; i < model_.tracks.size(); ++i)
            if (model_.tracks[i].kind == OcSeqTrackKind::Event) { ti = static_cast<int>(i); break; }
        if (ti < 0) ti = addTrack(OcSeqTrackKind::Event, scene::kInvalidEntity);
        if (ti < 0) return;
        selectTrack(ti);
    }
    eventName_ = name.empty() ? std::string("Event") : name;
    addKeyAtPlayhead();
}

void SequenceEditor::addSectionAtPlayhead(const std::string& assetPath) {
    if (selTrack_ < 0 || static_cast<usize>(selTrack_) >= model_.tracks.size()) { status_ = "Select an audio, animation or sub-sequence track"; return; }
    const OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    if (!seqKindIsSection(tr.kind)) { status_ = "Select an audio, animation or sub-sequence track"; return; }
    if (assetPath.empty()) { status_ = "Give the section an asset path"; return; }
    dropAsset(selTrack_, std::clamp(player_.time(), 0.0, model_.length), assetPath, scene::kInvalidEntity);
}

// An asset dropped on a lane at time t: a section of the lane's kind, or a refusal that says why.
void SequenceEditor::dropAsset(int track, f64 t, const std::string& assetPath, scene::Entity) {
    if (track < 0 || static_cast<usize>(track) >= model_.tracks.size()) return;
    std::string rel = assetPath;
    if (host_ && host_->contentRelative) {
        const std::string r = host_->contentRelative(assetPath);
        if (!r.empty()) rel = r;
    }
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(track)];
    const SeqAssetKind a = seqClassifyAsset(rel);
    if (!seqLaneAccepts(tr.kind, a)) {
        status_ = std::string("This lane takes ") + (tr.kind == OcSeqTrackKind::Audio ? "sounds" :
                  tr.kind == OcSeqTrackKind::Animation ? "animation clips and montages" : "level sequence files (.ocseq)");
        return;
    }
    f64 dur = 2.0;
    if (a == SeqAssetKind::Clip) {
        if (const fmt::OcAnimation* c = anim::animSystem().clip(fnv1a64(std::string_view(rel)))) dur = c->duration;
    }
    pushUndo();
    const f64 step = game::seqStepSeconds(model_);
    const int k = seqAddSection(tr, std::round(t / step) * step, dur, rel, model_.length);
    if (k < 0) { undo_.cancelPush(); status_ = "No room for a section there"; return; }
    selTrack_ = -1;
    selectTrack(track);
    selectOnly(k);
    status_ = "Section " + rel;
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
        if (keySel_[i]) seqSetKeyInterp(tr, i, interp);
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
        if (cam.kind == OcSeqTrackKind::Camera && cam.target <= 0 && cam.bind.empty() && game::sampleSeqCamera(cam, player_.time(), pose)) {
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

// A section moves as one block and stops against its neighbours, never over them.
void SequenceEditor::applySectionDrag(OcSeqTrack& tr, f64 delta) {
    int idx = -1;
    for (usize i = 0; i < dragOrig_.size(); ++i)
        if (dragOrig_[i].primary) idx = static_cast<int>(i);
    if (idx < 0 || dragOrig_.size() != tr.keys.size()) return;
    const OcSeqKey& orig = dragOrig_[static_cast<usize>(idx)].key;
    f64 lo = 0.0, hi = std::fmax(model_.length - orig.dur, 0.0);
    if (idx > 0) {
        const OcSeqKey& p = dragOrig_[static_cast<usize>(idx) - 1].key;
        lo = std::fmax(lo, p.dur > 0.0 ? p.t + p.dur : p.t);
    }
    if (static_cast<usize>(idx) + 1 < dragOrig_.size()) hi = std::fmin(hi, dragOrig_[static_cast<usize>(idx) + 1].key.t - orig.dur);
    tr.keys[static_cast<usize>(idx)].t = std::clamp(orig.t + delta, lo, std::fmax(lo, hi));
}

// ---- sequences ---------------------------------------------------------------------------------

// The level's sequences in order, moved out of model_ / others_ (which are left empty).
std::vector<fmt::OcSequence> SequenceEditor::takeAll() {
    std::vector<fmt::OcSequence> all;
    for (usize i = 0, o = 0; i < others_.size() + 1; ++i) all.push_back(i == curSlot_ ? std::move(model_) : std::move(others_[o++]));
    others_.clear();
    return all;
}

// Makes all[slot] the current sequence and the rest `others_`; selection, undo and the clock start over.
void SequenceEditor::adopt(std::vector<fmt::OcSequence>&& all, usize slot) {
    if (all.empty()) all.emplace_back();
    slot = std::min(slot, all.size() - 1);
    model_ = std::move(all[slot]);
    others_.clear();
    for (usize i = 0; i < all.size(); ++i)
        if (i != slot) others_.push_back(std::move(all[i]));
    curSlot_ = slot;
    undo_.clear();
    selTrack_ = -1;
    selKey_ = -1;
    keySel_.clear();
    drag_ = LaneDrag::None;
    pxPerSec_ = 0;
    viewStart_ = 0;
    player_.pause();
    player_.setTime(0);
    syncPlayer();
    evalDirty_ = true;
    ++editRev_;
}

bool SequenceEditor::switchSequence(usize slot) {
    if (slot >= others_.size() + 1 || slot == curSlot_) return false;
    restoreBases();
    adopt(takeAll(), slot);
    return true;
}

bool SequenceEditor::newSequence(const std::string& name) {
    const std::string n = uniqueName(sequenceNames(), name.empty() ? std::string("Sequence") : name);
    fmt::OcSequence s;
    s.name = n;
    s.autoplay = false;   // a new one waits for a Play call from a script
    restoreBases();
    std::vector<fmt::OcSequence> all = takeAll();
    all.push_back(std::move(s));
    const usize slot = all.size() - 1;
    adopt(std::move(all), slot);
    status_ = "New sequence '" + n + "'";
    edited();
    return true;
}

bool SequenceEditor::duplicateSequence() {
    fmt::OcSequence copy = model_;
    copy.name = uniqueName(sequenceNames(), model_.name + " Copy");
    copy.autoplay = false;
    restoreBases();
    const usize at = curSlot_;
    std::vector<fmt::OcSequence> all = takeAll();
    all.insert(all.begin() + static_cast<std::ptrdiff_t>(at) + 1, std::move(copy));   // right after the original
    adopt(std::move(all), at + 1);
    status_ = "Duplicated as '" + model_.name + "'";
    edited();
    return true;
}

bool SequenceEditor::renameSequence(const std::string& name) {
    if (name.empty()) { status_ = "A sequence needs a name"; return false; }
    if (name == model_.name) return true;
    for (const std::string& n : sequenceNames())
        if (n == name) { status_ = "Another sequence is already called '" + name + "'"; return false; }
    const std::string old = model_.name;
    // Sub-sequence sections that named it follow the rename.
    const auto follow = [&](fmt::OcSequence& s) {
        for (OcSeqTrack& tr : s.tracks)
            if (tr.kind == OcSeqTrackKind::SubSequence)
                for (OcSeqKey& k : tr.keys) if (k.text == old) k.text = name;
    };
    follow(model_);
    for (fmt::OcSequence& s : others_) follow(s);
    model_.name = name;
    status_ = "Renamed to '" + name + "'";
    edited();
    return true;
}

bool SequenceEditor::deleteSequence() {
    restoreBases();
    const usize at = curSlot_;
    std::vector<fmt::OcSequence> all = takeAll();
    if (all.size() <= 1) {   // the level keeps one: it is emptied
        const std::string n = all.front().name;
        all.front() = fmt::OcSequence{};
        all.front().name = n;
        status_ = "Sequence emptied (a level keeps at least one)";
        adopt(std::move(all), 0);
    } else {
        status_ = "Deleted '" + all[at].name + "'";
        all.erase(all.begin() + static_cast<std::ptrdiff_t>(at));
        adopt(std::move(all), std::min(at, all.size() - 1));
    }
    edited();
    return true;
}

bool SequenceEditor::exportOcseq(const std::string& fullPath) {
    if (!host_ || !host_->bindName) { status_ = "Export needs the level open in the editor"; return false; }
    fmt::OcSequence out;
    std::vector<std::string> problems;
    if (!seqToOcseq(model_, host_->bindName, host_->nameCount, out, &problems)) {
        status_ = "Export refused: " + (problems.empty() ? std::string("a bound actor has no unique name") : problems.front());
        for (const std::string& p : problems) AVER_WARN("[Sequence] export refused: {}", p);
        return false;
    }
    fmt::OcSeqFile file;
    file.sequences.push_back(std::move(out));
    if (!writeFileText(fullPath, fmt::writeOcseq(file))) { status_ = "Could not write " + fullPath; return false; }
    linkedFile_ = fullPath;
    linkedDirty_ = false;
    status_ = "Exported to " + fullPath;
    return true;
}

bool SequenceEditor::importOcseq(const std::string& fullPath, const std::function<scene::Entity(const std::string&)>& findByNameArg) {
    std::string text;
    if (!readFileText(fullPath, text)) { status_ = "Could not read " + fullPath; return false; }
    fmt::OcSeqFile file;
    std::string err;
    if (!fmt::parseOcseq(text, file, &err)) { status_ = "Not a sequence file: " + err; return false; }
    if (file.sequences.empty()) { status_ = "The file holds no sequence"; return false; }
    std::vector<std::string> names = sequenceNames();
    const std::function<scene::Entity(const std::string&)> find =
        findByNameArg ? findByNameArg
        : host_ && host_->findByName ? host_->findByName
                                     : std::function<scene::Entity(const std::string&)>([](const std::string& n) {
                                           return scene::World::instance().find(n);
                                       });
    std::vector<std::string> dropped;
    usize added = 0;
    restoreBases();
    std::vector<fmt::OcSequence> all = takeAll();
    const usize first = all.size();
    for (const fmt::OcSequence& s : file.sequences) {
        fmt::OcSequence c = seqFromFile(s, {}, find, &dropped);
        c.name = uniqueName(names, c.name.empty() ? std::string("Imported") : c.name);
        names.push_back(c.name);
        c.autoplay = false;
        all.push_back(std::move(c));
        ++added;
    }
    adopt(std::move(all), first);   // the first imported one becomes current
    linkedFile_ = fullPath;
    linkedDirty_ = false;
    status_ = "Imported " + std::to_string(added) + " sequence(s) from " + fullPath;
    edited();
    return true;
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
        ImGui::SetTooltip("%s", ok.empty() ? "Select a placed object in the level (decals and prefab instances can't be keyed)."
                                           : "Writes the selected object's transform as a key on its track at the playhead;\n"
                                             "a key already there is replaced. The track is created if needed.");
}

// The "+ Track" menu: every kind a sequence can hold.
void SequenceEditor::drawAddTrackMenu(SequenceHost& host) {
    const scene::Entity sel = host.selection.empty() ? scene::kInvalidEntity : host.selection.front();
    const bool haveSel = sel != scene::kInvalidEntity && scene::World::instance().valid(sel);
    const bool canTransform = haveSel && (!host.canKey || host.canKey(sel));
    const bool canBind = haveSel && (!host.canBind || host.canBind(sel));
    if (ImGui::MenuItem("Transform (selected actor)", nullptr, false, canTransform))
        for (const scene::Entity e : host.selection) if (!host.canKey || host.canKey(e)) addTrack(OcSeqTrackKind::Transform, e);
    if (ImGui::MenuItem("Emissive (selected actor)", nullptr, false, canBind)) addTrack(OcSeqTrackKind::Material, sel);
    if (ImGui::MenuItem("Property...", nullptr, false, canBind)) { propPickEntity_ = sel; pendingPopup_ = 1; }
    if (ImGui::MenuItem("Visibility (selected actor)", nullptr, false, canBind)) addTrack(OcSeqTrackKind::Visibility, sel);
    ImGui::Separator();
    if (ImGui::MenuItem("Event")) addTrack(OcSeqTrackKind::Event, scene::kInvalidEntity);
    if (ImGui::MenuItem("Audio...")) { assetKind_ = OcSeqTrackKind::Audio; propPickEntity_ = haveSel ? sel : scene::kInvalidEntity; pendingPopup_ = 2; }
    if (ImGui::MenuItem("Animation... (selected skeletal actor)", nullptr, false, canBind)) { assetKind_ = OcSeqTrackKind::Animation; propPickEntity_ = sel; pendingPopup_ = 2; }
    ImGui::Separator();
    if (ImGui::MenuItem("Camera")) addCameraTrack(scene::kInvalidEntity);
    if (ImGui::MenuItem("Camera from selection (actor camera)", nullptr, false, canBind)) addCameraTrack(sel);
    if (ImGui::MenuItem("Camera Cuts", nullptr, false, seqCutTrack(model_) < 0)) addTrack(OcSeqTrackKind::CameraCut, scene::kInvalidEntity);
    ImGui::Separator();
    if (ImGui::MenuItem("Sub-sequence...")) { assetKind_ = OcSeqTrackKind::SubSequence; propPickEntity_ = scene::kInvalidEntity; pendingPopup_ = 2; }
}

// The property picker: fields of the chosen actor's components, filtered, with kind and current value.
void SequenceEditor::drawPropertyPicker(SequenceHost& host) {
    scene::World& w = scene::World::instance();
    if (pendingPopup_ == 1) { ImGui::OpenPopup("Property track##seqprop"); pendingPopup_ = 0; }
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Property track##seqprop", &open, ImGuiWindowFlags_NoSavedSettings)) return;
    // The actor: the selection's members.
    std::vector<scene::Entity> actors;
    for (const scene::Entity e : host.selection) if (w.valid(e)) actors.push_back(e);
    if (actors.empty()) { ImGui::TextDisabled("Select an actor in the level first."); }
    else {
        if (std::find(actors.begin(), actors.end(), propPickEntity_) == actors.end()) propPickEntity_ = actors.front();
        const std::string cur = host.label ? host.label(propPickEntity_) : std::string("actor");
        if (ImGui::BeginCombo("Actor", cur.c_str())) {
            for (const scene::Entity e : actors)
                if (ImGui::Selectable((host.label ? host.label(e) : std::to_string(static_cast<u32>(e))).c_str(), e == propPickEntity_)) propPickEntity_ = e;
            ImGui::EndCombo();
        }
        inputText("Filter", propFilter_, 64);
        ImGui::BeginChild("##propList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
        const std::vector<SeqFieldInfo> fields = seqSequenceableFields(w, propPickEntity_, propFilter_);
        int shown = 0;
        for (const SeqFieldInfo& f : fields) {
            if (++shown > 400) { ImGui::TextDisabled("... narrow the filter"); break; }
            OcSeqKey k;
            seqKeyFromField(w, propPickEntity_, f.qualified, k);
            char val[96];
            if (f.arity == 1) std::snprintf(val, sizeof val, "%g", k.v[0]);
            else if (f.arity == 3) std::snprintf(val, sizeof val, "%g %g %g", k.v[0], k.v[1], k.v[2]);
            else std::snprintf(val, sizeof val, "%g %g %g %g", k.v[0], k.v[1], k.v[2], k.v[3]);
            const char* kind = f.kind == scene::FieldKind::F32 ? "float" : f.kind == scene::FieldKind::Vec3 ? "vec3" :
                               f.kind == scene::FieldKind::Quat ? "quat" : f.kind == scene::FieldKind::I32 ? "int" : "bool";
            const std::string row = f.qualified + "   [" + kind + "]   " + val;
            if (ImGui::Selectable(row.c_str())) {
                addPropertyTrack(propPickEntity_, f.qualified);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
    }
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SequenceEditor::drawSequenceBar(SequenceHost& host) {
    const std::vector<std::string> names = sequenceNames();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Sequence");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f * (ImGui::GetFontSize() / 16.0f));
    if (ImGui::BeginCombo("##seqPick", model_.name.c_str())) {
        for (usize i = 0; i < names.size(); ++i)
            if (ImGui::Selectable(names[i].c_str(), i == curSlot_)) switchSequence(i);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("New")) { renameBuf_ = "Sequence"; ImGui::OpenPopup("##seqNew"); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate")) duplicateSequence();
    ImGui::SameLine();
    if (ImGui::SmallButton("Rename")) { renameBuf_ = model_.name; ImGui::OpenPopup("##seqRename"); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) ImGui::OpenPopup("##seqDelete");
    ImGui::SameLine();
    if (ImGui::SmallButton("Import .ocseq...")) { ocseqPath_.clear(); ImGui::OpenPopup("##seqImport"); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Export .ocseq...")) { ocseqPath_ = linkedFile_.empty() ? std::string() : (host.contentRelative ? host.contentRelative(linkedFile_) : linkedFile_); if (ocseqPath_.empty()) ocseqPath_ = "Sequences/" + model_.name + ".ocseq"; ImGui::OpenPopup("##seqExport"); }
    if (!linkedFile_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", linkedDirty_ ? "* " : "", fileStem(linkedFile_).c_str());
    }

    if (ImGui::BeginPopup("##seqNew")) {
        ImGui::SetNextItemWidth(180.0f);
        const bool enter = inputText("##nm", renameBuf_, 64, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("Create")) { newSequence(renameBuf_); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##seqRename")) {
        ImGui::SetNextItemWidth(180.0f);
        const bool enter = inputText("##nm", renameBuf_, 64, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("Rename")) { if (renameSequence(renameBuf_)) ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##seqDelete")) {
        ImGui::Text("Delete '%s' and its %d track(s)?", model_.name.c_str(), static_cast<int>(model_.tracks.size()));
        if (ImGui::Button("Delete")) { deleteSequence(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##seqImport")) {
        ImGui::TextUnformatted("Content path of the .ocseq file");
        ImGui::SetNextItemWidth(280.0f);
        const bool enter = inputText("##p", ocseqPath_, 260, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("Import")) {
            if (importOcseq(host.contentFull ? host.contentFull(ocseqPath_) : ocseqPath_)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##seqExport")) {
        ImGui::TextUnformatted("Content path to write (actors are bound by name)");
        ImGui::SetNextItemWidth(280.0f);
        const bool enter = inputText("##p", ocseqPath_, 260, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("Export")) {
            if (exportOcseq(host.contentFull ? host.contentFull(ocseqPath_) : ocseqPath_)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void SequenceEditor::drawModePanel(SequenceHost& host) {
    host_ = &host;
    const std::vector<scene::Entity>& sel = host.selection;

    ImGui::TextDisabled("CAMERA PATH");
    ImGui::BeginDisabled(viewIsPilot());
    if (ImGui::Button("Key camera  (K)", ImVec2(-1, 0))) keyCamera();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes the viewport camera (position, yaw, pitch) as a key on the selected camera track\n"
                          "(or the first one) at the playhead; a key already there is replaced.\n"
                          "The track is created if needed.");
    drawKeyObjectButton("modeKeyObj", -1.0f);
    ImGui::BeginDisabled(!hasCamera());
    if (ImGui::Button("Clear camera keys", ImVec2(-1, 0))) {
        int ti = findTrack(OcSeqTrackKind::Camera, scene::kInvalidEntity);
        if (selTrack_ >= 0 && static_cast<usize>(selTrack_) < model_.tracks.size() && model_.tracks[static_cast<usize>(selTrack_)].kind == OcSeqTrackKind::Camera)
            ti = selTrack_;
        clearTrack(ti);
    }
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its camera tracks drive the view while the sequence plays");
    b = audition_;
    if (ImGui::Checkbox("Audition audio", &b)) setAudition(b);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Audio sections play in the preview while the clock moves forward.\nEvents never fire in the preview.");

    ImGui::Spacing();
    ImGui::TextDisabled("TRACKS");
    if (ImGui::Button("+ Track", ImVec2(-1, 0))) ImGui::OpenPopup("##seqAddTrackMode");
    if (ImGui::BeginPopup("##seqAddTrackMode")) { drawAddTrackMenu(host); ImGui::EndPopup(); }
    (void)sel;

    int removeIdx = -1, dragFrom = -1, dragTo = -1;
    for (usize i = 0; i < model_.tracks.size(); ++i) {
        const OcSeqTrack& tr = model_.tracks[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(tr.mute ? "m" : "M")) setMute(static_cast<int>(i), !tr.mute);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr.mute ? "Unmute this track" : "Mute this track");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tr.mute ? IM_COL32(110, 110, 110, 255) : kindColor(tr.kind)));
        char letter[2] = {seqKindLetter(tr.kind), 0};
        ImGui::TextUnformatted(letter);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const f32 xW = ImGui::GetFrameHeight();
        bool red = false;
        const char* badge = bindBadge(tr, red);
        std::string label = trackLabel(tr) + "  (" + std::to_string(tr.keys.size()) + ")";
        if (*badge) label += std::string("  [") + badge + "]";
        if (red) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 90, 90, 255));
        const bool picked = ImGui::Selectable(label.c_str(), selTrack_ == static_cast<int>(i), 0,
                                              ImVec2(ImGui::GetContentRegionAvail().x - xW - 4.0f, 0));
        if (red) ImGui::PopStyleColor();
        if (picked) selectTrack(static_cast<int>(i));
        if (ImGui::BeginDragDropSource()) {
            const int from = static_cast<int>(i);
            ImGui::SetDragDropPayload(kPayloadTrack, &from, sizeof from);
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kPayloadTrack)) {
                if (pl->DataSize == sizeof(int)) { dragFrom = *static_cast<const int*>(pl->Data); dragTo = static_cast<int>(i); }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIdx = static_cast<int>(i);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this track");
        ImGui::PopID();
    }
    if (dragFrom >= 0) moveTrack(dragFrom, dragTo);
    if (removeIdx >= 0) removeTrack(removeIdx);
    if (model_.tracks.empty())
        ImGui::TextWrapped("Fly the camera and press K to key it, or select an actor and add a track. "
                           "Shift+K keys the selected track.");

    drawKeyDetails(host);

    if (!status_.empty()) { ImGui::Spacing(); ImGui::TextDisabled("%s", status_.c_str()); }
    host_ = nullptr;
}

void SequenceEditor::drawKeyDetails(SequenceHost&) {
    // The selected key (sections and events included).
    OcSeqKey* key = selectedKey();
    if (!key) {
        // A selected event or section track can still be added to.
        if (selTrack_ >= 0 && static_cast<usize>(selTrack_) < model_.tracks.size()) {
            const OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
            if (tr.kind == OcSeqTrackKind::Event) {
                ImGui::Spacing();
                ImGui::TextDisabled("EVENT");
                ImGui::SetNextItemWidth(-1);
                inputText("##evName", eventName_, 96);
                if (ImGui::Button("Fire at playhead", ImVec2(-1, 0))) addEventKey(eventName_);
            } else if (seqKindIsSection(tr.kind)) {
                ImGui::Spacing();
                ImGui::TextDisabled("SECTION");
                ImGui::SetNextItemWidth(-1);
                inputText("##secAsset", assetField_, 260);
                if (ImGui::Button("Add section at playhead", ImVec2(-1, 0))) addSectionAtPlayhead(assetField_);
            }
        }
        return;
    }
    OcSeqTrack& tr = model_.tracks[static_cast<usize>(selTrack_)];
    ImGui::Spacing();
    ImGui::TextDisabled("KEY");
    const f32 ui = ImGui::GetFontSize() / 16.0f;
    (void)ui;
    // Edits go through these: the first change of a gesture pushes the undo step.
    const auto drag = [&](const char* label, f64& v, f32 speed, f32 lo, f32 hi, const char* fmtStr) {
        f32 x = static_cast<f32>(v);
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1);
        const bool ch = ImGui::DragFloat((std::string("##") + label).c_str(), &x, speed, lo, hi, fmtStr);
        if (ImGui::IsItemActivated()) pushUndo();
        if (ch) { v = x; edited(); }
        return ch;
    };
    const auto check = [&](const char* label, f64& v) {
        bool b = v != 0.0;
        if (ImGui::Checkbox(label, &b)) { pushUndo(); v = b ? 1.0 : 0.0; edited(); }
    };
    const auto text = [&](const char* label, std::string& s) {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1);
        std::string v = s;
        const bool ch = inputText((std::string("##") + label).c_str(), v, 260);
        if (ImGui::IsItemActivated()) pushUndo();
        if (ch) { s = v; edited(); }
    };

    f32 t = static_cast<f32>(key->t);
    ImGui::TextUnformatted("Time");
    ImGui::SetNextItemWidth(-1);
    const bool tChanged = ImGui::DragFloat("##keyT", &t, 0.01f, 0.0f, static_cast<f32>(model_.length), "%.2f s");
    if (ImGui::IsItemActivated()) pushUndo();
    if (tChanged) {
        if (seqKindIsSection(tr.kind)) {
            int idx = selKey_;
            if (seqMoveSection(tr, idx, t, model_.length)) { selKey_ = idx; keySel_.assign(tr.keys.size(), 0); if (idx >= 0) keySel_[static_cast<usize>(idx)] = 1; edited(); }
        } else {
            selKey_ = moveKey(tr, selKey_, t);
            edited();
        }
    }
    key = selectedKey();
    if (!key) return;

    const bool sampled = tr.kind == OcSeqTrackKind::Transform || tr.kind == OcSeqTrackKind::Camera ||
                         tr.kind == OcSeqTrackKind::Material || tr.kind == OcSeqTrackKind::Property;
    if (sampled) {
        int interp = static_cast<int>(key->interp);
        ImGui::TextUnformatted("Interpolation to next key");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##keyInterp", &interp, "Smooth\0Linear\0Step\0Cubic\0")) {
            pushUndo();
            key->interp = static_cast<OcSeqInterp>(interp);
            edited();
            key = selectedKey();
            if (!key) return;
        }
    }

    switch (tr.kind) {
        case OcSeqTrackKind::Material: {
            f32 col[3] = {static_cast<f32>(key->v[0]), static_cast<f32>(key->v[1]), static_cast<f32>(key->v[2])};
            ImGui::TextUnformatted("Emissive colour");
            ImGui::SetNextItemWidth(-1);
            const bool cChanged = ImGui::ColorEdit3("##keyCol", col, ImGuiColorEditFlags_Float);
            if (ImGui::IsItemActivated()) pushUndo();
            if (cChanged) { key->v[0] = col[0]; key->v[1] = col[1]; key->v[2] = col[2]; edited(); }
            f32 inten = static_cast<f32>(key->v[3]);
            ImGui::TextUnformatted("Intensity");
            ImGui::SetNextItemWidth(-1);
            const bool iChanged = ImGui::DragFloat("##keyInt", &inten, 0.05f, 0.0f, 1000.0f, "%.2f");
            if (ImGui::IsItemActivated()) pushUndo();
            if (iChanged) { key->v[3] = std::fmax(0.0f, inten); edited(); }
            break;
        }
        case OcSeqTrackKind::Transform:
            ImGui::TextDisabled("pos %.0f %.0f %.0f", key->v[0], key->v[1], key->v[2]);
            ImGui::TextDisabled("rot %.0f %.0f %.0f", key->v[3], key->v[4], key->v[5]);
            ImGui::TextDisabled("scl %.2f %.2f %.2f", key->v[6], key->v[7], key->v[8]);
            if (ImGui::Button("Set from actor now", ImVec2(-1, 0))) { pushUndo(); fillKey(tr, *key); edited(); }
            break;
        case OcSeqTrackKind::Camera:
            ImGui::TextDisabled("pos %.0f %.0f %.0f", key->v[0], key->v[1], key->v[2]);
            ImGui::TextDisabled("yaw %.1f  pitch %.1f", key->v[3], key->v[4]);
            {
                std::string nm = tr.name;
                const std::string before = game::seqCameraName(tr);
                ImGui::TextUnformatted("Camera name");
                ImGui::SetNextItemWidth(-1);
                std::string v = before;
                if (inputText("##camName", v, 64, ImGuiInputTextFlags_EnterReturnsTrue) && v != before) {
                    pushUndo();
                    if (seqRenameCamera(model_, selTrack_, v)) edited(); else undo_.cancelPush();
                }
            }
            ImGui::BeginDisabled(viewIsPilot());
            if (ImGui::Button("Set from viewport camera", ImVec2(-1, 0))) { pushUndo(); fillKey(tr, *key); edited(); }
            ImGui::EndDisabled();
            if (ImGui::Button("Go to key", ImVec2(-1, 0))) goToKey(selTrack_, selKey_);
            break;
        case OcSeqTrackKind::Property: {
            scene::World& w = scene::World::instance();
            const scene::FieldDesc* d = w.field(w.fieldId(tr.property));
            const u32 arity = d ? seqFieldArity(d->kind) : 1;
            const bool integral = d && (d->kind == scene::FieldKind::I32 || d->kind == scene::FieldKind::Bool);
            ImGui::TextDisabled("%s", tr.property.c_str());
            static const char* const kChan[4] = {"X", "Y", "Z", "W"};
            for (u32 c = 0; c < arity && c < 4; ++c) {
                const std::string lab = arity == 1 ? std::string("Value") : kChan[c];
                drag(lab.c_str(), key->v[c], integral ? 1.0f : 0.01f, -1e9f, 1e9f, integral ? "%.0f" : "%.3f");
                key = selectedKey();
                if (!key) return;
            }
            if (ImGui::Button("Set from actor now", ImVec2(-1, 0))) { pushUndo(); fillKey(tr, *key); edited(); }
            break;
        }
        case OcSeqTrackKind::Visibility:
            check("Visible", key->v[0]);
            check("Include children", key->v[1]);
            break;
        case OcSeqTrackKind::Event: {
            text("Event name", key->text);
            text("String argument", key->arg);
            int route = static_cast<int>(std::lround(key->v[0]));
            ImGui::TextUnformatted("Delivery");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##evRoute", &route, "Event bus\0Entity graph\0Both\0")) { pushUndo(); key->v[0] = route; edited(); key = selectedKey(); if (!key) return; }
            int argc = std::clamp(static_cast<int>(std::lround(key->v[1])), 0, 4);
            ImGui::TextUnformatted("Number arguments");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##evArgc", &argc, 0, 4)) { pushUndo(); key->v[1] = argc; edited(); key = selectedKey(); if (!key) return; }
            for (int a = 0; a < argc; ++a) {
                drag((std::string("Arg ") + std::to_string(a + 1)).c_str(), key->v[2 + a], 0.01f, -1e9f, 1e9f, "%.3f");
                key = selectedKey();
                if (!key) return;
            }
            ImGui::TextDisabled("Fires on a forward crossing in Play; never while scrubbing or in a render.");
            break;
        }
        case OcSeqTrackKind::CameraCut: {
            ImGui::TextUnformatted("Cut to camera");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##cutCam", key->text.c_str())) {
                for (const OcSeqTrack& c : model_.tracks)
                    if (c.kind == OcSeqTrackKind::Camera && ImGui::Selectable(game::seqCameraName(c).c_str(), game::seqCameraName(c) == key->text)) {
                        pushUndo(); key->text = game::seqCameraName(c); edited();
                        break;
                    }
                ImGui::EndCombo();
            }
            key = selectedKey();
            if (!key) return;
            drag("Blend (s)", key->v[0], 0.01f, 0.0f, 30.0f, "%.2f");
            break;
        }
        case OcSeqTrackKind::Audio: {
            text("Sound", key->text);
            drag("Duration (s)", key->dur, 0.01f, static_cast<f32>(kSeqMinSection), static_cast<f32>(model_.length), "%.2f");
            drag("Fade in (s)", key->v[0], 0.01f, 0.0f, 30.0f, "%.2f");
            drag("Fade out (s)", key->v[1], 0.01f, 0.0f, 30.0f, "%.2f");
            check("Loop", key->v[2]);
            check("Positional (needs an actor)", key->v[3]);
            drag("Inner radius (cm, 0 = 300)", key->v[4], 1.0f, 0.0f, 1e6f, "%.0f");
            drag("Outer radius (cm, 0 = 3000)", key->v[5], 1.0f, 0.0f, 1e6f, "%.0f");
            drag("Volume", key->v[6], 0.01f, 0.0f, 4.0f, "%.2f");
            drag("Pitch", key->v[7], 0.01f, 0.1f, 4.0f, "%.2f");
            ImGui::TextDisabled("A section entered mid-way (a jump, a resume) stays silent.");
            break;
        }
        case OcSeqTrackKind::Animation: {
            text("Clip or montage", key->text);
            drag("Duration (s)", key->dur, 0.01f, static_cast<f32>(kSeqMinSection), static_cast<f32>(model_.length), "%.2f");
            drag("Start offset (s)", key->v[0], 0.01f, 0.0f, 3600.0f, "%.2f");
            drag("Blend in (s)", key->v[1], 0.01f, 0.0f, 30.0f, "%.2f");
            drag("Blend out (s)", key->v[2], 0.01f, 0.0f, 30.0f, "%.2f");
            check("Loop (clips)", key->v[3]);
            int after = key->v[4] != 0.0 ? 1 : 0;
            ImGui::TextUnformatted("After the section");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##animAfter", &after, "Release\0Hold last pose\0")) { pushUndo(); key->v[4] = after; edited(); key = selectedKey(); if (!key) return; }
            drag("Rate", key->v[6], 0.01f, 0.0f, 8.0f, "%.2f");
            drag("Weight", key->v[7], 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::TextDisabled("Notifies inside a driven clip do not fire; use event tracks.");
            break;
        }
        case OcSeqTrackKind::SubSequence: {
            ImGui::TextUnformatted("Sequence");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##subPick", key->text.c_str())) {
                for (const std::string& n : sequenceNames())
                    if (n != model_.name && ImGui::Selectable(n.c_str(), n == key->text)) { pushUndo(); key->text = n; edited(); break; }
                ImGui::EndCombo();
            }
            key = selectedKey();
            if (!key) return;
            text("Level sequence or .ocseq path", key->text);
            key = selectedKey();
            if (!key) return;
            drag("Duration (s)", key->dur, 0.01f, static_cast<f32>(kSeqMinSection), static_cast<f32>(model_.length), "%.2f");
            drag("Start offset (s)", key->v[0], 0.01f, 0.0f, 3600.0f, "%.2f");
            check("Loop inside the section", key->v[1]);
            check("Its camera is live here", key->v[2]);
            drag("Time scale", key->v[6], 0.01f, 0.01f, 16.0f, "%.2f");
            break;
        }
    }
    key = selectedKey();
    if (key && ImGui::Button("Delete key", ImVec2(-1, 0))) removeSelectedKeys();
}

void SequenceEditor::drawTimeline(SequenceHost& host) {
    host_ = &host;
    const f32 ui = ImGui::GetFontSize() / 16.0f;
    const f32 winH = std::fmax(190.0f * ui, host.vpH * 0.3f);
    ImGui::SetNextWindowPos(ImVec2(host.vpX + 8.0f, host.vpY + host.vpH - winH - 8.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::fmax(360.0f * ui, host.vpW - 16.0f), winH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer", nullptr, ImGuiWindowFlags_NoCollapse)) {
        drag_ = LaneDrag::None;   // a hidden window sees no mouse-up; a stuck scrub would pin the pilot view
        dragRuler_ = false;
        ImGui::End();
        host_ = nullptr;
        return;
    }
    const bool winFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGuiIO& io = ImGui::GetIO();

    drawSequenceBar(host);

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
    bool aud = audition_;
    if (ImGui::Checkbox("Audition", &aud)) setAudition(aud);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play audio sections while the clock moves forward.\nEvents never fire in the preview.");
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
    if (ImGui::Button("+ Track")) ImGui::OpenPopup("##seqAddTrack");
    if (ImGui::BeginPopup("##seqAddTrack")) { drawAddTrackMenu(host); ImGui::EndPopup(); }
    ImGui::SameLine();
    {
        int mode = view_mode_ == View::Curves ? 1 : 0;
        if (ImGui::RadioButton("Dope sheet", mode == 0)) view_mode_ = View::DopeSheet;
        ImGui::SameLine();
        if (ImGui::RadioButton("Curves", mode == 1)) view_mode_ = View::Curves;
    }
    ImGui::SameLine();
    if (playActiveLast_) ImGui::TextDisabled("Play is running");
    else if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    // Popups opened from the menus, at window level so their ids match.
    drawPropertyPicker(host);
    if (pendingPopup_ == 2) { ImGui::OpenPopup("Add track##seqasset"); pendingPopup_ = 0; }
    if (ImGui::BeginPopupModal("Add track##seqasset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(assetKind_ == OcSeqTrackKind::Audio ? "Sound (content path, or drop it on a lane later)"
                             : assetKind_ == OcSeqTrackKind::Animation ? ".ocanim clip or .ocmontage (content path)"
                                                                       : "Level sequence name, or a .ocseq content path");
        ImGui::SetNextItemWidth(320.0f * ui);
        inputText("##asset", assetField_, 260);
        if (assetKind_ == OcSeqTrackKind::SubSequence) {
            ImGui::TextDisabled("This level's sequences:");
            for (const std::string& n : sequenceNames())
                if (n != model_.name && ImGui::Selectable(n.c_str())) assetField_ = n;
        }
        const scene::Entity actor = propPickEntity_;
        if (assetKind_ == OcSeqTrackKind::Audio)
            ImGui::TextDisabled("Select an actor before opening this to make the sound positional.");
        if (ImGui::Button("Add")) {
            const scene::Entity target = (assetKind_ == OcSeqTrackKind::Animation || assetKind_ == OcSeqTrackKind::Audio) ? actor : scene::kInvalidEntity;
            if (addAssetTrack(assetKind_, target, assetField_) >= 0) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

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
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false) && !linkedFile_.empty()) exportOcseq(linkedFile_);
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
    const f32 nameW = 190.0f * ui;
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
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !dragRuler_ && drag_ == LaneDrag::None &&
        io.MousePos.x >= r0.x && io.MousePos.x < r1.x && io.MousePos.y >= r0.y) {
        const f32 mx = io.MousePos.x - r0.x;
        if (io.KeyCtrl && io.MouseWheel != 0.0f && view_mode_ == View::DopeSheet) {
            setZoom(ppsNow() * std::pow(1.2f, io.MouseWheel), viewStart_ + mx / ppsNow(), mx);
        } else if (io.MouseWheelH != 0.0f || (io.KeyShift && io.MouseWheel != 0.0f)) {
            const f32 wheel = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
            viewStart_ -= wheel * (laneW / ppsNow()) * 0.1;
            clampView();
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) && view_mode_ == View::DopeSheet) {
            viewStart_ -= io.MouseDelta.x / ppsNow();
            clampView();
        }
    }
    // Holding a scrub or a key drag past either end of the lane scrolls toward it.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (dragRuler_ || drag_ != LaneDrag::None)) {
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
        // Event keys as ticks on the ruler: they never fire in the preview, so this is where they show.
        for (const OcSeqTrack& tr : model_.tracks)
            if (tr.kind == OcSeqTrackKind::Event && !tr.mute)
                for (const OcSeqKey& k : tr.keys)
                    dl->AddLine(ImVec2(laneX(k.t), r0.y), ImVec2(laneX(k.t), r0.y + 6.0f * ui), kindColor(OcSeqTrackKind::Event), 2.0f);
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

    if (view_mode_ == View::Curves) drawCurveView(host, r0.x, nameW, laneW, ui);
    else drawLanes(host, r0.x, nameW, laneW, rowH, ui);   // r0.x: where the ruler's lane column starts

    clampSelection();
    ImGui::End();
    host_ = nullptr;
}

void SequenceEditor::drawLanes(SequenceHost& host, f32 laneX0, f32 nameW, f32 laneW, f32 rowH, f32 ui) {
    ImGuiIO& io = ImGui::GetIO();
    const f64 len = std::fmax(model_.length, 0.01);
    const f32 minPps = static_cast<f32>(laneW / len);
    const f32 pps = pxPerSec_ > minPps ? std::fmin(pxPerSec_, std::fmax(minPps, 4000.0f * ui)) : minPps;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool laneChild = ImGui::BeginChild("##seqLanes", ImVec2(0, 0), false);
    ImGui::PopStyleVar();
    int removeIdx = -1;
    if (laneChild) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        if (model_.tracks.empty())
            ImGui::TextDisabled("No tracks. Fly the camera and press K, or select an actor and use + Track. "
                                "Drop a sound, clip, montage or .ocseq here to add one.");
        const f32 hitR = 8.0f * ui;
        for (usize i = 0; i < model_.tracks.size(); ++i) {
            OcSeqTrack& tr = model_.tracks[i];
            const bool trackSel = selTrack_ == static_cast<int>(i);
            const bool section = seqKindIsSection(tr.kind);
            ImGui::PushID(static_cast<int>(i));
            // Header: mute, name (red when its actor is missing).
            if (ImGui::SmallButton(tr.mute ? "m" : "M")) setMute(static_cast<int>(i), !tr.mute);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr.mute ? "Unmute" : "Mute");
            ImGui::SameLine(0.0f, 2.0f);
            bool red = false;
            const char* badge = bindBadge(tr, red);
            std::string name = trackLabel(tr);
            if (*badge) name += std::string("  [") + badge + "]";
            ImGui::PushStyleColor(ImGuiCol_Text, red ? ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 90, 90, 255))
                                                     : ImGui::ColorConvertU32ToFloat4(tr.mute ? IM_COL32(110, 110, 110, 255) : kindColor(tr.kind)));
            if (ImGui::Selectable(name.c_str(), trackSel, 0, ImVec2(nameW - ImGui::GetCursorPosX() - 6.0f * ui, rowH)))
                selectTrack(static_cast<int>(i));
            ImGui::PopStyleColor();
            ImGui::SameLine(laneX0 - ImGui::GetWindowPos().x);   // the lane starts where the ruler does
            ImGui::InvisibleButton("##lane", ImVec2(laneW, rowH));
            const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
            const f32 cy = (p0.y + p1.y) * 0.5f;
            auto lx = [&](f64 t) { return p0.x + static_cast<f32>((t - viewStart_) * pps); };
            auto timeAt = [&](f32 x) { return viewStart_ + static_cast<f64>(x - p0.x) / pps; };

            cdl->PushClipRect(p0, p1, true);
            cdl->AddRectFilled(p0, p1, trackSel ? IM_COL32(46, 40, 36, 255) : IM_COL32(30, 30, 34, 255));
            const ImU32 col = tr.mute ? IM_COL32(100, 100, 100, 255) : kindColor(tr.kind);
            if (!section && tr.kind != OcSeqTrackKind::Event && tr.kind != OcSeqTrackKind::Visibility && tr.keys.size() >= 2)
                cdl->AddLine(ImVec2(lx(tr.keys.front().t), cy), ImVec2(lx(tr.keys.back().t), cy), IM_COL32(120, 120, 130, 140), 2.0f);
            if (tr.kind == OcSeqTrackKind::Visibility) {   // a shaded span where the actor is visible
                for (usize k = 0; k < tr.keys.size(); ++k) {
                    const f64 end = k + 1 < tr.keys.size() ? tr.keys[k + 1].t : len;
                    if (tr.keys[k].v[0] != 0.0)
                        cdl->AddRectFilled(ImVec2(lx(tr.keys[k].t), p0.y + 4.0f * ui), ImVec2(lx(end), p1.y - 4.0f * ui),
                                           tr.mute ? IM_COL32(70, 70, 70, 120) : IM_COL32(120, 160, 120, 90));
                }
            }
            for (usize k = 0; k < tr.keys.size(); ++k) {
                const OcSeqKey& key = tr.keys[k];
                const bool ks = trackSel && k < keySel_.size() && keySel_[k] != 0;
                const f32 x = lx(key.t);
                if (section) {
                    const f32 x1 = lx(key.t + key.dur);
                    const ImU32 fill = ks ? IM_COL32(255, 255, 255, 70) : IM_COL32(40, 50, 60, 255);
                    cdl->AddRectFilled(ImVec2(x, p0.y + 2.0f * ui), ImVec2(x1, p1.y - 2.0f * ui), fill, 3.0f);
                    cdl->AddRectFilled(ImVec2(x, p0.y + 2.0f * ui), ImVec2(x1, p0.y + 6.0f * ui), col, 3.0f);
                    cdl->AddRect(ImVec2(x, p0.y + 2.0f * ui), ImVec2(x1, p1.y - 2.0f * ui), ks ? IM_COL32(255, 255, 255, 255) : col, 3.0f);
                    if (tr.kind == OcSeqTrackKind::Animation) {   // the blend ramps
                        const f32 bi = static_cast<f32>(key.v[1] * pps), bo = static_cast<f32>(key.v[2] * pps);
                        if (bi > 1.0f) cdl->AddLine(ImVec2(x, p1.y - 3.0f * ui), ImVec2(x + bi, p0.y + 7.0f * ui), col);
                        if (bo > 1.0f) cdl->AddLine(ImVec2(x1 - bo, p0.y + 7.0f * ui), ImVec2(x1, p1.y - 3.0f * ui), col);
                    }
                    std::string lab = fileStem(key.text);
                    if (tr.kind == OcSeqTrackKind::SubSequence) {
                        char extra[64];
                        std::snprintf(extra, sizeof extra, "  +%.1fs x%.2g", key.v[0], key.v[6]);
                        lab += extra;
                    }
                    cdl->PushClipRect(ImVec2(x, p0.y), ImVec2(std::max(x + 2.0f, x1), p1.y), true);
                    cdl->AddText(ImVec2(x + 4.0f * ui, p0.y + 6.0f * ui), IM_COL32(220, 225, 230, 255), lab.c_str());
                    cdl->PopClipRect();
                } else if (tr.kind == OcSeqTrackKind::Event) {
                    cdl->AddLine(ImVec2(x, p0.y + 2.0f * ui), ImVec2(x, p1.y - 2.0f * ui), ks ? IM_COL32(255, 255, 255, 255) : col, 2.0f);
                    cdl->AddRectFilled(ImVec2(x, p0.y + 2.0f * ui), ImVec2(x + 5.0f * ui + ImGui::CalcTextSize(key.text.c_str()).x, p0.y + 2.0f * ui + ImGui::GetTextLineHeight()),
                                       ks ? IM_COL32(255, 255, 255, 60) : IM_COL32(60, 30, 40, 220));
                    cdl->AddText(ImVec2(x + 3.0f * ui, p0.y + 2.0f * ui), IM_COL32(240, 220, 225, 255), key.text.c_str());
                } else {
                    const f32 r = (ks ? 6.0f : 4.5f) * ui;
                    const ImU32 kc = ks ? IM_COL32(255, 255, 255, 255) : tr.kind == OcSeqTrackKind::CameraCut ? nameColor(key.text) : col;
                    if (key.interp == OcSeqInterp::Step) {
                        cdl->AddRectFilled(ImVec2(x - r * 0.8f, cy - r * 0.8f), ImVec2(x + r * 0.8f, cy + r * 0.8f), kc);
                    } else if (key.interp == OcSeqInterp::Cubic) {
                        cdl->AddCircleFilled(ImVec2(x, cy), r, kc);
                        cdl->AddCircle(ImVec2(x, cy), r, IM_COL32(0, 0, 0, 160));
                    } else {
                        const ImVec2 d[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
                        cdl->AddConvexPolyFilled(d, 4, kc);
                        cdl->AddPolyline(d, 4, IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed, 1.0f);
                    }
                    if (tr.kind == OcSeqTrackKind::CameraCut)
                        cdl->AddText(ImVec2(x + 7.0f * ui, p0.y + 1.0f * ui), IM_COL32(220, 220, 225, 255), key.text.c_str());
                }
            }
            if (trackSel) cdl->AddRect(p0, p1, IM_COL32(255, 220, 140, 150));
            const f32 phx = lx(player_.time());
            cdl->AddLine(ImVec2(phx, p0.y), ImVec2(phx, p1.y), IM_COL32(255, 190, 70, 170), 1.0f);
            cdl->PopClipRect();

            // Press decides once what it landed on; a drag then moves the keys, or moves / trims a section.
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                int hit = -1;
                LaneDrag kind = LaneDrag::Keys;
                f32 best = hitR;
                const f32 edge = 6.0f * ui;
                for (usize k = 0; k < tr.keys.size(); ++k) {
                    if (section) {
                        const f32 xa = lx(tr.keys[k].t), xb = lx(tr.keys[k].t + tr.keys[k].dur);
                        if (io.MousePos.x < xa - edge || io.MousePos.x > xb + edge) continue;
                        hit = static_cast<int>(k);
                        kind = std::fabs(io.MousePos.x - xa) <= edge ? LaneDrag::TrimStart
                             : std::fabs(io.MousePos.x - xb) <= edge ? LaneDrag::TrimEnd : LaneDrag::SectionMove;
                    } else {
                        const f32 d = std::fabs(io.MousePos.x - lx(tr.keys[k].t));
                        if (d <= best) { best = d; hit = static_cast<int>(k); }
                    }
                }
                selectTrack(static_cast<int>(i));
                clampSelection();
                const bool additive = io.KeyCtrl || io.KeyShift;
                if (hit >= 0) {
                    if (additive && !section) {
                        keySel_[static_cast<usize>(hit)] = keySel_[static_cast<usize>(hit)] ? char(0) : char(1);
                        selKey_ = keySel_[static_cast<usize>(hit)] ? hit : -1;
                    } else if (!keySel_[static_cast<usize>(hit)] || section) {
                        selectOnly(hit);   // pressing a selected key keeps the group, so it can be dragged
                    } else {
                        selKey_ = hit;
                    }
                    drag_ = keySel_[static_cast<usize>(hit)] != 0 ? (section ? kind : LaneDrag::Keys) : LaneDrag::None;
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
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = LaneDrag::None;
            if (drag_ != LaneDrag::None && active && trackSel && !dragOrig_.empty() && dragOrig_.size() == tr.keys.size()) {
                // Pixels moved plus the scroll since the press, in seconds; a frame step for sections, whole 10 ms (1 ms when zoomed in) for keys.
                const f64 raw = static_cast<f64>(io.MousePos.x - dragX0_) / pps + (viewStart_ - dragView0_);
                const f64 snap = pps >= 600.0f * ui ? 1000.0 : 100.0;
                f64 delta = std::round(raw * snap) / snap;
                if (section) { const f64 step = game::seqStepSeconds(model_); delta = std::round(raw / step) * step; }
                if (dragStarted_ || std::fabs(delta) > 1e-9) {
                    if (!dragStarted_) { pushUndo(); dragStarted_ = true; }
                    if (drag_ == LaneDrag::Keys) {
                        applyDrag(tr, delta);
                    } else if (drag_ == LaneDrag::SectionMove) {
                        applySectionDrag(tr, delta);
                    } else {
                        // Trims work from the press-time sections so a wandering drag lands where it started.
                        int idx = -1;
                        for (usize k = 0; k < dragOrig_.size(); ++k) if (dragOrig_[k].primary) idx = static_cast<int>(k);
                        if (idx >= 0) {
                            tr.keys.clear();
                            for (const DragItem& it : dragOrig_) tr.keys.push_back(it.key);
                            const OcSeqKey& o = dragOrig_[static_cast<usize>(idx)].key;
                            if (drag_ == LaneDrag::TrimStart) {
                                const f64 floorT = idx > 0 ? std::fmax(0.0, dragOrig_[static_cast<usize>(idx) - 1].key.t + dragOrig_[static_cast<usize>(idx) - 1].key.dur) : 0.0;
                                seqTrimSectionStart(tr, idx, std::clamp(o.t + delta, floorT, o.t + o.dur - kSeqMinSection));
                            } else {
                                const f64 cap = static_cast<usize>(idx) + 1 < dragOrig_.size() ? dragOrig_[static_cast<usize>(idx) + 1].key.t : model_.length;
                                seqTrimSectionEnd(tr, idx, std::clamp(o.t + o.dur + delta, o.t + kSeqMinSection, std::fmax(cap, o.t + kSeqMinSection)), model_.length);
                            }
                        }
                    }
                    edited();
                }
            }

            // An asset dropped on the lane becomes a section at the mouse time.
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayloadSet);
                if (!payload) payload = ImGui::AcceptDragDropPayload(kPayloadOne);
                for (const std::string& one : payloadPaths(payload)) {
                    dropAsset(static_cast<int>(i), timeAt(io.MousePos.x), one, scene::kInvalidEntity);
                    break;
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::BeginPopupContextItem("##laneCtx")) {
                const bool haveSel = trackSel && selectedCount() > 0;
                if (ImGui::MenuItem("Key at playhead", nullptr, false, !section)) { selectTrack(static_cast<int>(i)); addKeyAtPlayhead(); }
                if (ImGui::MenuItem("Go to selected key", nullptr, false, haveSel && selKey_ >= 0))
                    goToKey(selTrack_, selKey_);
                if (ImGui::MenuItem("Select all keys", "Ctrl+A", false, !tr.keys.empty())) {
                    selectTrack(static_cast<int>(i));
                    clampSelection();
                    std::fill(keySel_.begin(), keySel_.end(), char(1));
                    ++editRev_;
                }
                const bool sampled = tr.kind == OcSeqTrackKind::Transform || tr.kind == OcSeqTrackKind::Camera ||
                                     tr.kind == OcSeqTrackKind::Material || tr.kind == OcSeqTrackKind::Property;
                if (ImGui::BeginMenu("Interpolation of selected", haveSel && sampled)) {
                    if (ImGui::MenuItem("Smooth")) setSelectedInterp(OcSeqInterp::Smooth);
                    if (ImGui::MenuItem("Linear")) setSelectedInterp(OcSeqInterp::Linear);
                    if (ImGui::MenuItem("Step"))   setSelectedInterp(OcSeqInterp::Step);
                    if (ImGui::MenuItem("Cubic"))  setSelectedInterp(OcSeqInterp::Cubic);
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Delete selected keys", "Del", false, haveSel)) removeSelectedKeys();
                ImGui::Separator();
                if (ImGui::MenuItem(tr.mute ? "Unmute track" : "Mute track")) setMute(static_cast<int>(i), !tr.mute);
                if (ImGui::MenuItem("Clear track", nullptr, false, !tr.keys.empty())) clearTrack(static_cast<int>(i));
                if (ImGui::MenuItem("Remove track")) removeIdx = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        // Below the lanes: a drop makes a new track for the asset (sound, clip or montage, level sequence).
        ImGui::Dummy(ImVec2(laneW + nameW, rowH));
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayloadSet);
            if (!payload) payload = ImGui::AcceptDragDropPayload(kPayloadOne);
            for (const std::string& one : payloadPaths(payload)) {
                std::string rel = one;
                if (host.contentRelative) { const std::string r = host.contentRelative(one); if (!r.empty()) rel = r; }
                const SeqAssetKind a = seqClassifyAsset(rel);
                if (a == SeqAssetKind::None) { status_ = "Drop a sound, an animation clip or montage, or a .ocseq file"; break; }
                const OcSeqTrackKind kind = seqTrackKindForAsset(a);
                scene::Entity actor = scene::kInvalidEntity;
                if (kind == OcSeqTrackKind::Animation)
                    for (const scene::Entity e : host.selection) if (scene::World::instance().valid(e)) { actor = e; break; }
                addAssetTrack(kind, actor, rel);
                break;
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::EndChild();
    if (removeIdx >= 0) removeTrack(removeIdx);
}

#else  // !AVER_WITH_IMGUI

void SequenceEditor::drawModePanel(SequenceHost&) {}
void SequenceEditor::drawTimeline(SequenceHost&) {}

#endif

} // namespace aver::editor

#endif // AVER_MODULE_SCENE
