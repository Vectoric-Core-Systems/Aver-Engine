// The level sequence player: what every track kind writes, the base store that undoes it, and the
// sub-sequence library (LevelSequence.hpp, docs/SEQUENCER.md).
#include "aver/game/LevelSequence.hpp"

#if AVER_MODULE_SCENE

#include "aver/anim/AnimSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace aver::game {

namespace {

using fmt::OcSeqKey;
using fmt::OcSeqTrack;
using fmt::OcSeqTrackKind;

constexpr u32 kMaxSubDepth = 4;
constexpr f32 kStopFade = 0.05f;   // seconds: a voice cut by a pause or a jump

u64 nextUid() {
    static u64 n = 0;
    return ++n;
}

i64 g_originX = 0, g_originY = 0;   // SequencePlayer::setSceneOrigin
bool originZero() { return g_originX == 0 && g_originY == 0; }

// A track's position channels (f64, level-absolute) minus the scene origin, narrowed once.
Vec3 originRelative(const OcSeqTrack& tr, f64 t) {
    return Vec3{static_cast<f32>(sampleSeqChannel(tr, 0, t) - static_cast<f64>(g_originX)),
                static_cast<f32>(sampleSeqChannel(tr, 1, t) - static_cast<f64>(g_originY)),
                static_cast<f32>(sampleSeqChannel(tr, 2, t))};
}

bool endsWithCI(const std::string& s, const char* suffix) {
    const usize n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (usize i = 0; i < n; ++i) {
        char a = s[s.size() - n + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool hasBinding(const OcSeqTrack& tr) { return tr.target >= 0 || !tr.bind.empty(); }

// The binding rules in one place: overrides, names, the host's finder, then the placement index.
scene::Entity resolveIn(const SeqBindings& b, const OcSeqTrack& tr) {
    if (!tr.bind.empty()) {
        if (const auto it = b.overrides.find(tr.bind); it != b.overrides.end()) return it->second;
        if (const auto it = b.byName.find(tr.bind); it != b.byName.end() && it->second != scene::kInvalidEntity)
            return it->second;
        if (b.find) {
            const scene::Entity e = b.find(tr.bind);
            if (e != scene::kInvalidEntity) return e;
        }
    }
    if (tr.target >= 0 && static_cast<usize>(tr.target) < b.byPlacement.size())
        return b.byPlacement[static_cast<usize>(tr.target)];
    return scene::kInvalidEntity;
}

const SequenceHooks& noHooks() {
    static const SequenceHooks h;
    return h;
}

// Virtual Property fields: CMorphTargets.morph.<target> and CMorphTargets.pose.<name> drive blend-shape weights
// through MorphSystem's sequencer layer (MORPH_TARGETS.md 2.5). No registered field exists for them.
constexpr const char* kMorphTargetPrefix = "CMorphTargets.morph.";
constexpr const char* kMorphPosePrefix   = "CMorphTargets.pose.";
bool isMorphTrack(const OcSeqTrack& tr) {
    return tr.kind == OcSeqTrackKind::Property &&
           (tr.property.rfind(kMorphTargetPrefix, 0) == 0 || tr.property.rfind(kMorphPosePrefix, 0) == 0);
}

bool movesEntity(const OcSeqTrack& tr) {
    if (tr.kind == OcSeqTrackKind::Transform || tr.kind == OcSeqTrackKind::Material) return true;
    return tr.kind == OcSeqTrackKind::Property && tr.property.rfind("CLocal.", 0) == 0;
}

// Every entity the sequence's tracks (and its sub-sequences') name.
void collectSeq(const fmt::OcSequence& seq, const SeqBindings& b, SeqLibrary* lib, const SequenceHooks* hooks,
                bool movedOnly, std::vector<scene::Entity>& out, std::unordered_set<scene::Entity>& seen,
                std::vector<std::string>& stack) {
    stack.push_back(seq.name);
    for (const OcSeqTrack& tr : seq.tracks) {
        if (tr.mute) continue;   // a muted track drives nothing
        if (tr.kind == OcSeqTrackKind::SubSequence) {
            if (!lib || stack.size() > kMaxSubDepth) continue;
            for (const OcSeqKey& k : tr.keys) {
                const fmt::OcSequence* sub = lib->find(k.text, hooks ? *hooks : noHooks());
                if (!sub || std::find(stack.begin(), stack.end(), sub->name) != stack.end()) continue;
                collectSeq(*sub, b, lib, hooks, movedOnly, out, seen, stack);
            }
            continue;
        }
        if (movedOnly && !movesEntity(tr)) continue;
        const scene::Entity e = resolveIn(b, tr);
        if (e != scene::kInvalidEntity && seen.insert(e).second) out.push_back(e);
    }
    stack.pop_back();
}

f32 wrapPi(f32 a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

SeqCameraPose blendCamera(const SeqCameraPose& a, const SeqCameraPose& b, f32 alpha) {
    SeqCameraPose r;
    r.position = a.position + (b.position - a.position) * alpha;
    r.yaw = a.yaw + wrapPi(b.yaw - a.yaw) * alpha;
    r.pitch = a.pitch + (b.pitch - a.pitch) * alpha;
    return r;
}

// An actor's world pose as a camera: it looks along the actor's local +X.
SeqCameraPose cameraFromActor(scene::World& w, scene::Entity e) {
    const Mat4& m = w.worldMatrix(e);
    Vec3 f{m.m[0][0], m.m[0][1], m.m[0][2]};
    const f32 len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (len > 1e-6f) f = f * (1.0f / len); else f = Vec3{1, 0, 0};
    SeqCameraPose p;
    p.position = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
    p.yaw = std::atan2(f.y, f.x);
    p.pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.z)));
    return p;
}

} // namespace

// ---- SeqLibrary ---------------------------------------------------------------------------------

const fmt::OcSequence* SeqLibrary::find(const std::string& nameOrPath, const SequenceHooks& hooks) {
    const usize hash = nameOrPath.find('#');
    const std::string path = hash == std::string::npos ? nameOrPath : nameOrPath.substr(0, hash);
    const std::string name = hash == std::string::npos ? std::string() : nameOrPath.substr(hash + 1);
    if (!endsWithCI(path, ".ocseq")) {
        if (!level_) return nullptr;
        for (const fmt::OcSequence& s : *level_)
            if (s.name == nameOrPath) return &s;
        return nullptr;
    }
    auto it = files_.find(path);
    if (it == files_.end()) {
        std::string text;
        const bool read = hooks.readFile ? !(text = hooks.readFile(path)).empty() : readFileText(path, text);
        std::unique_ptr<fmt::OcSeqFile> file;
        if (!read) {
            AVER_WARN("[Sequencer] cannot read sequence file '{}'", path);
        } else {
            file = std::make_unique<fmt::OcSeqFile>();
            std::string err;
            if (!fmt::parseOcseq(text, *file, &err)) {
                AVER_WARN("[Sequencer] '{}': {}", path, err);
                file.reset();
            } else if (file->sequences.empty()) {
                AVER_WARN("[Sequencer] '{}' holds no sequence", path);
                file.reset();
            }
        }
        it = files_.emplace(path, std::move(file)).first;
    }
    if (!it->second) return nullptr;
    for (const fmt::OcSequence& s : it->second->sequences)
        if (name.empty() || s.name == name) return &s;
    return nullptr;
}

// ---- SeqBaseStore -------------------------------------------------------------------------------

void SeqBaseStore::capture(scene::World& w, scene::Entity e, u32 component, u16 offset, u16 size) {
    if (size == 0 || size > 16 || !w.valid(e)) return;
    for (const Entry& en : props_)
        if (en.e == e && en.comp == component && en.off == offset) return;
    const void* c = w.getComponent(e, component);
    if (!c) return;
    Entry en;
    en.e = e; en.comp = component; en.off = offset; en.size = size;
    std::memcpy(en.bytes, static_cast<const u8*>(c) + offset, size);
    props_.push_back(en);
}

void SeqBaseStore::captureField(scene::World& w, scene::Entity e, const scene::FieldDesc& f) {
    capture(w, e, f.component, f.offset, static_cast<u16>(scene::fieldByteSize(f.kind)));
}

void SeqBaseStore::captureTransform(scene::World& w, scene::Entity e) {
    if (!w.valid(e) || hasTransform(e)) return;
    xforms_.push_back({e, w.localTransform(e)});
}

bool SeqBaseStore::hasTransform(scene::Entity e) const {
    for (const XfEntry& x : xforms_)
        if (x.e == e) return true;
    return false;
}

void SeqBaseStore::captureComponentAdded(scene::Entity e, u32 component) {
    for (const AddedEntry& a : added_)
        if (a.e == e && a.comp == component) return;
    added_.push_back({e, component});
}

void SeqBaseStore::restoreEntry(scene::World& w, const Entry& en) const {
    if (!w.valid(en.e)) return;
    void* c = w.getComponent(en.e, en.comp);
    if (!c) return;
    std::memcpy(static_cast<u8*>(c) + en.off, en.bytes, en.size);
    if (en.comp == scene::kComponentLocal) w.touchLocal(en.e);
}

void SeqBaseStore::restore(scene::World& w) {
    for (const XfEntry& x : xforms_)
        if (w.valid(x.e)) w.setLocalTransform(x.e, x.xf);
    for (const Entry& en : props_) restoreEntry(w, en);   // after the transforms: a CLocal field wins its own bytes
    for (const AddedEntry& a : added_)
        if (w.valid(a.e) && w.hasComponent(a.e, a.comp)) w.removeComponent(a.e, a.comp);
    props_.clear();
    xforms_.clear();
    added_.clear();
}

void SeqBaseStore::release(scene::World& w, scene::Entity e) {
    const auto mine = [e](const auto& v) { return v.e == e; };
    for (const XfEntry& x : xforms_)
        if (mine(x) && w.valid(x.e)) w.setLocalTransform(x.e, x.xf);
    for (const Entry& en : props_)
        if (mine(en)) restoreEntry(w, en);
    for (const AddedEntry& a : added_)
        if (mine(a) && w.valid(a.e) && w.hasComponent(a.e, a.comp)) w.removeComponent(a.e, a.comp);
    xforms_.erase(std::remove_if(xforms_.begin(), xforms_.end(), mine), xforms_.end());
    props_.erase(std::remove_if(props_.begin(), props_.end(), mine), props_.end());
    added_.erase(std::remove_if(added_.begin(), added_.end(), mine), added_.end());
}

void SeqBaseStore::releaseUndriven(scene::World& w, const std::unordered_set<scene::Entity>& driven) {
    std::vector<scene::Entity> gone;
    const auto note = [&](scene::Entity e) {
        if (!driven.count(e) && std::find(gone.begin(), gone.end(), e) == gone.end()) gone.push_back(e);
    };
    for (const XfEntry& x : xforms_) note(x.e);
    for (const Entry& en : props_) note(en.e);
    for (const AddedEntry& a : added_) note(a.e);
    for (const scene::Entity e : gone) release(w, e);
}

void SeqBaseStore::rebase(const scene::World& w, const Vec3& deltaCm) {
    const auto root = [&w](scene::Entity e) { return w.parent(e) == scene::kInvalidEntity; };
    for (XfEntry& x : xforms_) {
        if (!root(x.e)) continue;
        x.xf.position.x -= deltaCm.x;
        x.xf.position.y -= deltaCm.y;
        x.xf.position.z -= deltaCm.z;
    }
    const f32 d[3] = {deltaCm.x, deltaCm.y, deltaCm.z};
    constexpr u16 kPos = static_cast<u16>(offsetof(scene::CLocal, xf));   // Transform::position is its first member
    for (Entry& en : props_) {
        if (en.comp != scene::kComponentLocal || !root(en.e)) continue;
        for (u16 k = 0; k < 3; ++k) {
            const u16 at = static_cast<u16>(kPos + 4 * k);
            if (at < en.off || at + 4 > en.off + en.size) continue;
            f32 v;
            std::memcpy(&v, en.bytes + (at - en.off), sizeof v);
            v -= d[k];
            std::memcpy(en.bytes + (at - en.off), &v, sizeof v);
        }
    }
}

// ---- SequencePlayer: lifetime -------------------------------------------------------------------

void SequencePlayer::setSceneOrigin(i64 xCm, i64 yCm) { g_originX = xCm; g_originY = yCm; }
void SequencePlayer::sceneOrigin(i64& xCm, i64& yCm) { xCm = g_originX; yCm = g_originY; }

SequencePlayer::SequencePlayer() : uid_(nextUid()) {}
SequencePlayer::~SequencePlayer() = default;
SequencePlayer::SequencePlayer(SequencePlayer&&) = default;
SequencePlayer& SequencePlayer::operator=(SequencePlayer&&) = default;

void SequencePlayer::setSequence(const fmt::OcSequence& seq) {
    releaseTriggered();
    own_ = seq;
    ext_ = nullptr;
    subs_.clear();
    resetTrackState();
}

void SequencePlayer::setSequenceRef(const fmt::OcSequence* seq) {
    if (ext_ == seq && seq) return;
    releaseTriggered();
    ext_ = seq;
    subs_.clear();
    resetTrackState();
}

void SequencePlayer::resetTrackState() {
    const usize n = sequence().tracks.size();
    audio_.assign(n, AudioState{});
    anim_.assign(n, AnimState{});
    prop_.assign(n, PropState{});
}

scene::Entity SequencePlayer::entityOf(i32 placement) const {
    const SeqBindings& b = bindings();
    if (placement < 0 || static_cast<usize>(placement) >= b.byPlacement.size()) return scene::kInvalidEntity;
    return b.byPlacement[static_cast<usize>(placement)];
}

scene::Entity SequencePlayer::resolve(const OcSeqTrack& track) const { return resolveIn(bindings(), track); }

std::vector<scene::Entity> SequencePlayer::boundEntities() const {
    std::vector<scene::Entity> out;
    std::unordered_set<scene::Entity> seen;
    std::vector<std::string> stack = stack_;
    collectSeq(sequence(), bindings(), library_, hooks_, false, out, seen, stack);
    return out;
}

std::vector<scene::Entity> SequencePlayer::movedEntities() const {
    std::vector<scene::Entity> out;
    std::unordered_set<scene::Entity> seen;
    std::vector<std::string> stack = stack_;
    collectSeq(sequence(), bindings(), library_, hooks_, true, out, seen, stack);
    return out;
}

std::vector<scene::Entity> SequencePlayer::transformEntities() const {
    std::vector<scene::Entity> out;
    std::unordered_set<scene::Entity> seen;
    for (const OcSeqTrack& tr : sequence().tracks) {
        if (tr.kind != OcSeqTrackKind::Transform) continue;
        const scene::Entity e = resolve(tr);
        if (e != scene::kInvalidEntity && seen.insert(e).second) out.push_back(e);
    }
    return out;
}

// ---- clock --------------------------------------------------------------------------------------

void SequencePlayer::play() {
    if (!playing_ && time_ <= 0.0) inclusive_ = true;   // t = 0 is crossed by the first step
    playing_ = true;
}

void SequencePlayer::setTime(f64 t) {
    time_ = seqWrapTime(sequence(), t);
    spans_.clear();
    jumped_ = true;
    inclusive_ = time_ <= 0.0;
}

void SequencePlayer::advance(f64 dt) {
    if (!playing_) return;
    const fmt::OcSequence& seq = sequence();
    const f64 prev = time_;
    time_ += dt * rate_;
    if (seq.loop) {
        time_ = seqWrapTime(seq, time_);
    } else if (time_ >= seq.length) {
        time_ = std::max(seq.length, 0.0);
        playing_ = false;
        finished_ = true;
    }
    if (mode_ != SeqMode::Offline) {   // an offline render has no use for crossings
        spans_.push_back({prev, time_, inclusive_});
        if (spans_.size() > 256) spans_.erase(spans_.begin(), spans_.begin() + 128);
    }
    inclusive_ = false;
}

// A sub-sequence's clock is its parent's section time, not its own advance().
void SequencePlayer::followTime(f64 newTime, bool contiguous, bool entered, bool forceLoop) {
    forceLoop_ = forceLoop;
    const f64 prev = time_;
    time_ = newTime;
    if (entered) {
        spans_.clear();
        spans_.push_back({newTime, newTime, true});   // keys sitting exactly on the entry instant fire
        jumped_ = false;
    } else if (contiguous && (newTime >= prev || forceLoop || sequence().loop)) {
        spans_.push_back({prev, newTime, false});
    } else {
        spans_.clear();
        jumped_ = true;
    }
}

bool SequencePlayer::camera(SeqCameraPose& out) const {
    if (!hasCamera_) return false;
    out = camera_;
    return true;
}

bool SequencePlayer::emissiveScale(scene::Entity e, f32 out[3]) const {
    if (emissive_.empty()) return false;
    const auto it = emissive_.find(e);
    if (it == emissive_.end()) return false;
    out[0] = it->second[0]; out[1] = it->second[1]; out[2] = it->second[2];
    return true;
}

// ---- evaluation ---------------------------------------------------------------------------------

void SequencePlayer::evaluate(scene::World& world, SeqPhase phase) {
    if (anim_.size() != sequence().tracks.size()) { releaseTriggered(); resetTrackState(); }
    if (phase == SeqPhase::Pre) evalPre(world); else evalPost(world);
}

void SequencePlayer::evaluateAnimation(scene::World& world) {
    if (anim_.size() != sequence().tracks.size()) { releaseTriggered(); resetTrackState(); }
    animOnly_ = true;
    evalPre(world);
    animOnly_ = false;
}

void SequencePlayer::evalPre(scene::World& w) {
    const fmt::OcSequence& seq = sequence();
    const f64 t = seqWrapTime(seq, time_);
    for (u32 i = 0; i < seq.tracks.size(); ++i) {
        const OcSeqTrack& tr = seq.tracks[i];
        if (tr.mute) {
            releaseAnim(i);
            stopVoice(i, kStopFade);
            if (isMorphTrack(tr)) anim::animSystem().morphs().releaseOwner(ownerOf(i));
            continue;
        }
        if (animOnly_ && tr.kind != OcSeqTrackKind::Animation && tr.kind != OcSeqTrackKind::SubSequence) continue;
        switch (tr.kind) {
            case OcSeqTrackKind::Property:    applyProperty(w, i, tr, t); break;
            case OcSeqTrackKind::Visibility:  applyVisibility(w, tr, t); break;
            case OcSeqTrackKind::Animation:   applyAnimation(w, i, tr, t); break;
            case OcSeqTrackKind::Event:       applyEvents(w, i, tr); break;
            case OcSeqTrackKind::Audio:       applyAudio(w, i, tr, t); break;
            case OcSeqTrackKind::SubSequence: applySub(w, SeqPhase::Pre, i, tr, t); break;
            default: break;
        }
    }
    if (!animOnly_) {   // an animation-only pass leaves the triggered spans for the real one
        spans_.clear();
        jumped_ = false;
    }
}

void SequencePlayer::evalPost(scene::World& w) {
    const fmt::OcSequence& seq = sequence();
    emissive_.clear();
    hasCamera_ = false;
    hasSubCam_ = false;
    cameraName_.clear();
    const f64 t = seqWrapTime(seq, time_);

    for (u32 i = 0; i < seq.tracks.size(); ++i) {
        const OcSeqTrack& tr = seq.tracks[i];
        if (tr.mute) continue;
        switch (tr.kind) {
            case OcSeqTrackKind::Transform: {
                const scene::Entity e = resolve(tr);
                Transform xf;
                if (e != scene::kInvalidEntity && w.valid(e) && sampleSeqTransform(tr, t, xf)) {
                    if (!originZero() && w.parent(e) == scene::kInvalidEntity) xf.position = originRelative(tr, t);
                    baseStore().captureTransform(w, e);
                    w.setLocalTransform(e, xf);
                }
                break;
            }
            case OcSeqTrackKind::Material: {
                const scene::Entity e = resolve(tr);
                f32 rgb[3];
                if (e != scene::kInvalidEntity && sampleSeqMaterial(tr, t, rgb)) emissive_[e] = {rgb[0], rgb[1], rgb[2]};
                break;
            }
            case OcSeqTrackKind::SubSequence: applySub(w, SeqPhase::Post, i, tr, t); break;
            default: break;
        }
    }

    // Cameras, after every actor moved: a camera riding a crane reads the crane's new world matrix.
    struct Cam { const std::string* name; SeqCameraPose pose; };
    std::vector<Cam> cams;
    const OcSeqTrack* cuts = nullptr;
    for (const OcSeqTrack& tr : seq.tracks) {
        if (tr.mute) continue;
        if (tr.kind == OcSeqTrackKind::CameraCut) { if (!cuts) cuts = &tr; continue; }
        if (tr.kind != OcSeqTrackKind::Camera) continue;
        SeqCameraPose pose;
        if (hasBinding(tr)) {
            const scene::Entity e = resolve(tr);
            if (e == scene::kInvalidEntity || !w.valid(e)) continue;
            pose = cameraFromActor(w, e);
        } else if (!sampleSeqCamera(tr, t, pose)) {
            continue;
        } else if (!originZero()) {
            pose.position = originRelative(tr, t);
        }
        cams.push_back({&seqCameraName(tr), pose});
    }
    if (!cams.empty()) {
        const auto byName = [&](const std::string& n) -> const Cam* {
            for (const Cam& c : cams) if (*c.name == n) return &c;
            return nullptr;
        };
        const Cam* live = &cams.front();
        const Cam* prev = nullptr;
        f32 alpha = 1.0f;
        std::string camName, prevName;
        if (cuts && sampleSeqCut(*cuts, t, camName, prevName, alpha)) {
            if (!camName.empty()) if (const Cam* c = byName(camName)) live = c;
            if (!prevName.empty() && alpha < 1.0f) prev = byName(prevName);
        }
        camera_ = prev ? blendCamera(prev->pose, live->pose, alpha) : live->pose;
        cameraName_ = *live->name;
        hasCamera_ = true;
    }
    if (hasSubCam_) { camera_ = subCam_; hasCamera_ = true; }
}

void SequencePlayer::applyProperty(scene::World& w, u32 index, const OcSeqTrack& tr, f64 t) {
    if (tr.property.empty() || tr.keys.empty()) return;
    PropState& st = prop_[index];
    if (isMorphTrack(tr)) {
        const bool target = tr.property.rfind(kMorphTargetPrefix, 0) == 0;
        const std::string_view rest = std::string_view(tr.property).substr(std::strlen(target ? kMorphTargetPrefix : kMorphPosePrefix));
        const scene::Entity e = resolve(tr);
        if (rest.empty() || e == scene::kInvalidEntity || !w.valid(e)) return;
        const u32 mt = anim::MorphSystem::typeId();
        if (mt == 0 || !w.hasComponent(e, mt)) {   // a track never adds the component
            if (!st.warned) AVER_WARN("[Sequencer] property track '{}': the actor has no CMorphTargets", tr.property);
            st.warned = true;
            return;
        }
        f64 v[4];
        if (!sampleSeqProperty(tr, t, false, false, v)) return;
        anim::MorphSystem& ms = anim::animSystem().morphs();
        const u64 id = fnv1a64(rest);
        const f32 x = static_cast<f32>(v[0]);
        if (target) ms.setTarget(w, e, id, x, 0.0f, anim::MorphSystem::Layer::Sequencer, ownerOf(index));
        else        ms.setPose(w, e, id, x, 0.0f, anim::MorphSystem::Layer::Sequencer, ownerOf(index));
        return;
    }
    const u32 fid = w.fieldId(tr.property);
    const scene::FieldDesc* d = fid ? w.field(fid) : nullptr;
    const auto refuse = [&](const char* why) {
        if (!st.warned) AVER_WARN("[Sequencer] property track '{}': {}", tr.property, why);
        st.warned = true;
    };
    if (!d) { refuse("no such field"); return; }
    using scene::FieldKind;
    if (d->readOnly) { refuse("the field is read-only"); return; }
    if (d->kind != FieldKind::F32 && d->kind != FieldKind::Vec3 && d->kind != FieldKind::Quat &&
        d->kind != FieldKind::I32 && d->kind != FieldKind::Bool) {
        refuse("only float, vec3, quat, int and bool fields can be sequenced");
        return;
    }
    const scene::Entity e = resolve(tr);
    if (e == scene::kInvalidEntity || !w.valid(e)) return;
    void* comp = w.getComponent(e, d->component);
    if (!comp) return;   // the actor lacks the component: a track never adds one
    const bool quat = d->kind == FieldKind::Quat;
    const bool integral = d->kind == FieldKind::I32 || d->kind == FieldKind::Bool;
    f64 v[4];
    if (!sampleSeqProperty(tr, t, quat, integral, v)) return;
    baseStore().captureField(w, e, *d);
    u8* dst = static_cast<u8*>(comp) + d->offset;
    switch (d->kind) {
        case FieldKind::F32:  { const f32 x = static_cast<f32>(v[0]); std::memcpy(dst, &x, sizeof x); break; }
        case FieldKind::Vec3: {
            f64 p[3] = {v[0], v[1], v[2]};
            // A ROOT's Local.position key is level-absolute: subtract the scene origin in f64 before narrowing.
            if (d->component == scene::kComponentLocal && d->offset == offsetof(scene::CLocal, xf) && !originZero() &&
                w.parent(e) == scene::kInvalidEntity) {
                p[0] -= static_cast<f64>(g_originX);
                p[1] -= static_cast<f64>(g_originY);
            }
            const f32 x[3] = {static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2])};
            std::memcpy(dst, x, sizeof x);
            break;
        }
        case FieldKind::Quat: { const f32 x[4] = {static_cast<f32>(v[0]), static_cast<f32>(v[1]), static_cast<f32>(v[2]), static_cast<f32>(v[3])}; std::memcpy(dst, x, sizeof x); break; }
        case FieldKind::I32:  { const i32 x = static_cast<i32>(std::lround(v[0])); std::memcpy(dst, &x, sizeof x); break; }
        case FieldKind::Bool: { const i32 x = std::lround(v[0]) != 0 ? 1 : 0; std::memcpy(dst, &x, sizeof x); break; }
        default: break;
    }
    if (d->component == scene::kComponentLocal) w.touchLocal(e);
}

void SequencePlayer::applyVisibility(scene::World& w, const OcSeqTrack& tr, f64 t) {
    usize n = 0;   // the last key at or before t
    while (n < tr.keys.size() && tr.keys[n].t <= t) ++n;
    if (n == 0) return;   // before the first key the authored value stands
    const scene::Entity e = resolve(tr);
    if (e == scene::kInvalidEntity || !w.valid(e)) return;
    const OcSeqKey& k = tr.keys[n - 1];
    const bool visible = k.v[0] != 0.0;
    SeqBaseStore& store = baseStore();
    const auto apply = [&](scene::Entity x) {
        auto* mr = w.component<scene::CMeshRenderer>(x, scene::kComponentMeshRenderer);
        if (!mr) return;
        store.capture(w, x, scene::kComponentMeshRenderer, static_cast<u16>(offsetof(scene::CMeshRenderer, flags)), 4);
        if (visible) mr->flags |= scene::kMeshRendererVisible; else mr->flags &= ~scene::kMeshRendererVisible;
    };
    apply(e);
    if (k.v[1] == 0.0) return;
    std::vector<scene::Entity> stack{e};
    while (!stack.empty()) {
        const scene::Entity p = stack.back();
        stack.pop_back();
        for (scene::Entity c = w.firstChild(p); c != scene::kInvalidEntity; c = w.nextSibling(c)) {
            apply(c);
            stack.push_back(c);
        }
    }
}

void SequencePlayer::releaseAnim(u32 index) {
    if (index >= anim_.size()) return;
    AnimState& st = anim_[index];
    if (st.asset == 0) return;
    anim::animSystem().montages().releaseDriven(st.e, ownerOf(index), 0);
    st.e = scene::kInvalidEntity;
    st.asset = 0;
}

void SequencePlayer::applyAnimation(scene::World& w, u32 index, const OcSeqTrack& tr, f64 t) {
    AnimState& st = anim_[index];
    const scene::Entity e = resolve(tr);
    f64 local = 0.0;
    const OcSeqKey* k = seqActiveSection(tr, t, true, local);
    bool ended = false;
    if (k) {
        ended = local >= k->dur;
        if (ended && k->v[4] != 1.0) k = nullptr;   // after: 0 releases at the section's end
    }
    if (!k || e == scene::kInvalidEntity || !w.valid(e) || k->text.empty()) { releaseAnim(index); return; }

    const bool montage = endsWithCI(k->text, ".ocmontage");
    const u64 asset = fnv1a64(std::string_view(k->text));
    if (st.asset != 0 && (st.e != e || st.asset != asset)) releaseAnim(index);

    const f64 at = ended ? k->dur : local;
    f64 pos = k->v[0] + at * std::fmax(0.0, k->v[6]);
    anim::AnimSystem& as = anim::animSystem();
    if (!montage && k->v[3] != 0.0) {   // a looping clip wraps by its own length
        if (const fmt::OcAnimation* c = as.clip(asset); c && c->duration > 0.0f) pos = std::fmod(pos, static_cast<f64>(c->duration));
    }
    const f32 weight = seqSectionWeight(*k, at, k->v[1], ended ? 0.0 : k->v[2]) * static_cast<f32>(k->v[7]);
    const bool hadAnimator = w.hasComponent(e, scene::kComponentAnimator);
    anim::MontageSystem& ms = as.montages();
    const bool ok = montage ? ms.driveMontage(as, w, e, asset, static_cast<f32>(pos), weight, ownerOf(index))
                            : ms.driveClip(as, w, e, asset, static_cast<f32>(pos), weight, ownerOf(index));
    if (!ok) {
        if (!st.warned) AVER_WARN("[Sequencer] animation track: '{}' cannot play on entity {} (no skeleton, or the asset does not load)",
                                  k->text, static_cast<u32>(e));
        st.warned = true;
        return;
    }
    if (!hadAnimator && w.hasComponent(e, scene::kComponentAnimator))
        baseStore().captureComponentAdded(e, scene::kComponentAnimator);
    st.e = e;
    st.asset = asset;
}

void SequencePlayer::applyEvents(scene::World& w, u32, const OcSeqTrack& tr) {
    if (mode_ != SeqMode::Live || spans_.empty() || !hooks_ || !hooks_->fireEvent) return;
    scene::Entity e = scene::kInvalidEntity;
    if (hasBinding(tr)) {
        e = resolve(tr);
        if (e == scene::kInvalidEntity || !w.valid(e)) return;   // an event addressed to nobody
    }
    const fmt::OcSequence& seq = sequence();
    std::vector<u32> crossed;
    for (const Span& sp : spans_) {
        seqCrossedKeys(tr, sp.t0, sp.t1, sp.inclusive, seq.length, seq.loop || forceLoop_, crossed);
        for (const u32 ki : crossed) {
            const OcSeqKey& k = tr.keys[ki];
            if (k.text.empty()) continue;
            SeqEventCall call;
            call.name = k.text;
            call.arg = k.arg;
            call.sender = call.target = e;
            call.route = static_cast<u8>(std::clamp(static_cast<int>(std::lround(k.v[0])), 0, 2));
            call.argc = static_cast<u8>(std::clamp(static_cast<int>(std::lround(k.v[1])), 0, 4));
            for (int a = 0; a < 4; ++a) call.args[a] = static_cast<f32>(k.v[2 + a]);
            hooks_->fireEvent(call);
        }
    }
}

void SequencePlayer::stopVoice(u32 index, f32 fade) {
    if (index >= audio_.size()) return;
    AudioState& st = audio_[index];
    if (st.voice == 0) return;
    if (hooks_ && hooks_->stopVoice) hooks_->stopVoice(st.voice, fade);
    st = AudioState{};
}

void SequencePlayer::stopAllVoices(f32 fade) {
    for (u32 i = 0; i < audio_.size(); ++i) stopVoice(i, fade);
    for (auto& kv : subs_) kv.second->stopAllVoices(fade);
}

void SequencePlayer::applyAudio(scene::World& w, u32 index, const OcSeqTrack& tr, f64 t) {
    AudioState& st = audio_[index];
    const bool canPlay = mode_ == SeqMode::Live || (mode_ == SeqMode::Preview && audition_);
    const bool haveHooks = hooks_ && hooks_->playVoice && hooks_->loadSound;

    if (st.voice != 0) {
        bool alive = canPlay && playing_ && !jumped_ && st.key >= 0 && static_cast<usize>(st.key) < tr.keys.size();
        if (alive) {
            const OcSeqKey& k = tr.keys[static_cast<usize>(st.key)];
            alive = t >= k.t && t < k.t + k.dur;
            if (alive) {
                const f64 local = t - k.t;
                const f32 vol = static_cast<f32>(k.v[6]) * seqSectionWeight(k, local, k.v[0], k.v[1]);
                Vec3 pos;
                bool spatial = k.v[3] != 0.0 && st.e != scene::kInvalidEntity && w.valid(st.e);
                if (spatial) { const Mat4& m = w.worldMatrix(st.e); pos = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }
                if (hooks_ && hooks_->updateVoice) hooks_->updateVoice(st.voice, spatial ? &pos : nullptr, vol);
            }
        }
        if (!alive) stopVoice(index, kStopFade);
    }

    if (!canPlay || !playing_ || spans_.empty() || !haveHooks) return;
    const fmt::OcSequence& seq = sequence();
    std::vector<u32> crossed;
    for (const Span& sp : spans_) {
        seqCrossedKeys(tr, sp.t0, sp.t1, sp.inclusive, seq.length, seq.loop || forceLoop_, crossed);
        for (const u32 ki : crossed) {
            const OcSeqKey& k = tr.keys[ki];
            if (k.text.empty()) continue;
            const i32 sound = hooks_->loadSound(k.text);
            if (sound == 0) continue;
            stopVoice(index, kStopFade);   // one voice per track
            scene::Entity e = hasBinding(tr) ? resolve(tr) : scene::kInvalidEntity;
            if (e != scene::kInvalidEntity && !w.valid(e)) e = scene::kInvalidEntity;
            SeqVoice v;
            v.pitch = static_cast<f32>(k.v[7]);
            v.loop = k.v[2] != 0.0 && k.dur > 0.0;
            v.spatial = k.v[3] != 0.0 && e != scene::kInvalidEntity;
            v.innerCm = k.v[4] > 0.0 ? static_cast<f32>(k.v[4]) : 300.0f;
            v.outerCm = k.v[5] > 0.0 ? static_cast<f32>(k.v[5]) : 3000.0f;
            v.fadeIn = static_cast<f32>(k.v[0]);
            const f64 local = std::fmax(0.0, t - k.t);
            v.volume = static_cast<f32>(k.v[6]) * (k.dur > 0.0 ? seqSectionWeight(k, local, k.v[0], k.v[1]) : 1.0f);
            if (v.spatial) { const Mat4& m = w.worldMatrix(e); v.pos = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }
            const i32 voice = hooks_->playVoice(sound, v);
            if (voice != 0 && k.dur > 0.0) st = AudioState{voice, static_cast<i32>(ki), e};   // dur 0: let it finish
        }
    }
}

SequencePlayer* SequencePlayer::subFor(u32 track, u32 key, const OcSeqKey& k, const OcSeqTrack&) {
    const u64 id = (static_cast<u64>(track) << 32) | key;
    const auto it = subs_.find(id);
    const SequenceHooks& hooks = hooks_ ? *hooks_ : noHooks();
    const fmt::OcSequence* seq = library_ ? library_->find(k.text, hooks) : nullptr;
    const auto refuse = [&](const char* why) -> SequencePlayer* {
        if (warnedSubs_.insert(id).second) AVER_WARN("[Sequencer] sub-sequence '{}': {}", k.text, why);
        if (it != subs_.end()) subs_.erase(id);
        return nullptr;
    };
    if (!seq) return refuse("no such sequence");
    if (depth_ + 1 > kMaxSubDepth) return refuse("nested too deep");
    if (!seq->name.empty() &&
        (seq->name == sequence().name || std::find(stack_.begin(), stack_.end(), seq->name) != stack_.end()))
        return refuse("it would play itself");
    SequencePlayer* p;
    if (it == subs_.end()) {
        p = (subs_[id] = std::make_unique<SequencePlayer>()).get();
        p->depth_ = depth_ + 1;
        p->stack_ = stack_;
        p->stack_.push_back(sequence().name);
    } else {
        p = it->second.get();
    }
    p->setSequenceRef(seq);   // a no-op while the library still hands out the same one
    p->bindingsRef_ = &bindings();
    p->library_ = library_;
    p->hooks_ = hooks_;
    p->mode_ = mode_;
    p->audition_ = audition_;
    p->store_ = &baseStore();
    p->rate_ = 1.0;
    p->animOnly_ = animOnly_;
    return p;
}

void SequencePlayer::applySub(scene::World& w, SeqPhase phase, u32 index, const OcSeqTrack& tr, f64 t) {
    f64 local = 0.0;
    const OcSeqKey* k = seqActiveSection(tr, t, false, local);
    const i64 active = (k && k->dur > 0.0) ? static_cast<i64>(k - tr.keys.data()) : -1;
    if (phase == SeqPhase::Pre) {   // a section that is no longer the active one lets go of what it drove
        for (auto& kv : subs_) {
            if ((kv.first >> 32) != index || static_cast<i64>(kv.first & 0xFFFFFFFFull) == active) continue;
            if (kv.second->subActive_) { kv.second->releaseTriggered(); kv.second->subActive_ = false; }
        }
    }
    if (active < 0) return;
    SequencePlayer* sub = subFor(index, static_cast<u32>(active), *k, tr);
    if (!sub) return;
    if (animOnly_ && !sub->subActive_) return;   // a re-drive leaves the clocks of sub-sequences alone
    if (phase == SeqPhase::Pre && !animOnly_) {
        const f64 scale = k->v[6] > 0.0 ? k->v[6] : 1.0;
        const f64 raw = k->v[0] + local * scale;
        const f64 len = sub->sequence().length;
        const bool loopSection = k->v[1] != 0.0;
        const f64 subT = loopSection && len > 0.0 ? std::fmod(raw, len) : std::clamp(raw, 0.0, std::fmax(len, 0.0));
        const bool entered = !sub->subActive_;
        sub->followTime(subT, !spans_.empty(), entered, loopSection);
        sub->playing_ = playing_;
        sub->subActive_ = true;
    }
    sub->evaluate(w, phase);
    if (phase == SeqPhase::Post && k->v[2] != 0.0) {
        SeqCameraPose c;
        if (sub->camera(c)) { subCam_ = c; hasSubCam_ = true; }
    }
}

// ---- stopping -----------------------------------------------------------------------------------

void SequencePlayer::releaseTriggered() {
    stopAllVoices(kStopFade);
    for (u32 i = 0; i < anim_.size(); ++i) releaseAnim(i);
    {
        const fmt::OcSequence& seq = sequence();
        for (u32 i = 0; i < seq.tracks.size(); ++i)
            if (isMorphTrack(seq.tracks[i])) anim::animSystem().morphs().releaseOwner(ownerOf(i));
    }
    for (auto& kv : subs_) {
        kv.second->releaseTriggered();
        kv.second->subActive_ = false;
    }
}

void SequencePlayer::restore(scene::World& world) {
    releaseTriggered();
    if (!store_) ownStore_.restore(world);   // a shared store is the director's to restore
}

} // namespace aver::game

#endif
