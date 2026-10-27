// SequenceDirector (SequenceDirector.hpp).
#include "aver/game/SequenceDirector.hpp"

#if AVER_MODULE_SCENE

#include "aver/core/Hash.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <memory>
#include <unordered_set>

namespace aver::game {

namespace {

using fmt::OcSeqTrack;
using fmt::OcSeqTrackKind;

// Every `bind` name the sequence and its sub-sequences use.
void collectBindNames(const fmt::OcSequence& seq, SeqLibrary& lib, const SequenceHooks& hooks,
                      std::vector<std::string>& out, std::vector<std::string>& stack) {
    if (stack.size() > 4) return;
    stack.push_back(seq.name);
    for (const OcSeqTrack& tr : seq.tracks) {
        if (!tr.bind.empty()) out.push_back(tr.bind);
        if (tr.kind != OcSeqTrackKind::SubSequence) continue;
        for (const fmt::OcSeqKey& k : tr.keys) {
            const fmt::OcSequence* sub = lib.find(k.text, hooks);
            if (sub && (sub->name.empty() || std::find(stack.begin(), stack.end(), sub->name) == stack.end()))
                collectBindNames(*sub, lib, hooks, out, stack);
        }
    }
    stack.pop_back();
}

} // namespace

SequenceDirector::SequenceDirector() : store_(std::make_unique<SeqBaseStore>()) {}
SequenceDirector::~SequenceDirector() = default;

void SequenceDirector::configure(Entry& e) {
    SeqBindings b = bindings_;
    for (const auto& kv : e.overrides) b.overrides[kv.first] = kv.second;
    e.player.setBindings(std::move(b));
    e.player.setLibrary(&library_);
    e.player.setHooks(&hooks_);
    e.player.setMode(mode_);
    e.player.setAudition(audition_);
    e.player.setBaseStore(store_.get());
}

void SequenceDirector::setSequences(std::vector<fmt::OcSequence> seqs) {
    std::vector<std::unique_ptr<Entry>> old = std::move(entries_);
    for (auto& e : old) e->player.releaseTriggered();
    seqs_ = std::move(seqs);
    library_.setLevel(&seqs_);
    library_.clear();
    entries_.clear();
    for (const fmt::OcSequence& s : seqs_) {
        auto e = std::make_unique<Entry>();
        e->player.setSequence(s);
        configure(*e);
        for (auto& o : old) {
            if (!o || o->player.sequence().name != s.name) continue;
            e->player.setTime(o->player.time());
            e->player.setRate(o->player.rate());
            if (o->player.playing()) e->player.play();
            e->serial = o->serial;
            e->engaged = o->engaged;
            e->started = o->started;
            e->overrides = o->overrides;
            configure(*e);
            o.reset();
            break;
        }
        entries_.push_back(std::move(e));
    }
}

void SequenceDirector::setBindings(SeqBindings b) {
    bindings_ = std::move(b);
    for (auto& e : entries_) configure(*e);
}

void SequenceDirector::setHooks(SequenceHooks h) { hooks_ = std::move(h); }

void SequenceDirector::setMode(SeqMode m) {
    mode_ = m;
    for (auto& e : entries_) e->player.setMode(m);
}

void SequenceDirector::setAudition(bool on) {
    audition_ = on;
    for (auto& e : entries_) e->player.setAudition(on);
}

i32 SequenceDirector::find(u64 nameHash) const {
    for (usize i = 0; i < entries_.size(); ++i)
        if (fnv1a64(std::string_view(entries_[i]->player.sequence().name)) == nameHash) return static_cast<i32>(i);
    return -1;
}

i32 SequenceDirector::find(const std::string& name) const { return find(fnv1a64(std::string_view(name))); }

void SequenceDirector::raise(const Entry& e, const char* suffix) const {
    if (mode_ != SeqMode::Live || !hooks_.fireEvent || e.player.sequence().name.empty()) return;
    SeqEventCall call;
    call.name = e.player.sequence().name + suffix;
    hooks_.fireEvent(call);   // route 0, no sender: a broadcast on the bus
}

bool SequenceDirector::play(i32 seq, f64 startTime, f64 rate) {
    Entry* e = at(seq);
    if (!e) return false;
    SequencePlayer& p = e->player;
    if (rate > 0.0) p.setRate(rate);
    if (startTime >= 0.0) p.setTime(startTime);
    else if (!e->engaged || (!p.sequence().loop && p.time() >= p.sequence().length)) p.setTime(0.0);
    e->engaged = true;
    p.play();
    if (!e->started) {
        e->started = true;
        e->serial = ++serial_;
        raise(*e, "_Started");
    }
    return true;
}

bool SequenceDirector::pause(i32 seq) {
    Entry* e = at(seq);
    if (!e) return false;
    e->player.pause();
    return true;
}

void SequenceDirector::stopOne(Entry& e, scene::World& w) {
    e.player.pause();
    e.player.setTime(0.0);
    e.player.releaseTriggered();
    e.engaged = false;
    e.started = false;
    e.serial = 0;
    (void)w;
}

bool SequenceDirector::stop(i32 seq) {
    scene::World& w = scene::World::instance();
    if (seq < 0) {
        for (auto& e : entries_) stopOne(*e, w);
        store_->restore(w);
        return true;
    }
    Entry* e = at(seq);
    if (!e) return false;
    stopOne(*e, w);
    // What the others still drive stays; everything else the store holds goes back.
    std::unordered_set<scene::Entity> keep;
    for (const auto& o : entries_)
        if (o.get() != e && o->engaged)
            for (const scene::Entity x : o->player.boundEntities()) keep.insert(x);
    store_->releaseUndriven(w, keep);
    return true;
}

bool SequenceDirector::setTime(i32 seq, f64 t) {
    Entry* e = at(seq);
    if (!e) return false;
    e->player.setTime(t);
    e->engaged = true;
    return true;
}

bool SequenceDirector::setRate(i32 seq, f64 r) {
    Entry* e = at(seq);
    if (!e) return false;
    e->player.setRate(r);
    return true;
}

bool SequenceDirector::state(i32 seq, f64& time, f64& length, f64& rate, bool& playing) const {
    const Entry* e = at(seq);
    if (!e) return false;
    time = e->player.time();
    length = e->player.sequence().length;
    rate = e->player.rate();
    playing = e->player.playing();
    return true;
}

bool SequenceDirector::bindOverride(i32 seq, u64 nameHash, scene::Entity ent) {
    Entry* e = at(seq);
    if (!e) return false;
    std::vector<std::string> names, stack;
    collectBindNames(e->player.sequence(), library_, hooks_, names, stack);
    bool any = false;
    for (const std::string& n : names) {
        if (fnv1a64(std::string_view(n)) != nameHash) continue;
        any = true;
        if (ent == scene::kInvalidEntity) e->overrides.erase(n); else e->overrides[n] = ent;
    }
    if (any) configure(*e);
    return any;
}

void SequenceDirector::startAutoplay() {
    for (usize i = 0; i < entries_.size(); ++i)
        if (entries_[i]->player.sequence().autoplay) play(static_cast<i32>(i), 0.0, 0.0);
}

void SequenceDirector::advance(f64 dt) {
    for (auto& e : entries_) {
        if (!e->engaged || !e->player.playing()) continue;
        e->player.advance(dt);
        if (e->player.takeFinished()) {
            e->started = false;   // the pose stays; a later play() raises _Started again
            raise(*e, "_Finished");
        }
    }
}

void SequenceDirector::evaluate(scene::World& world, SeqPhase phase) {
    for (auto& e : entries_)
        if (e->engaged) e->player.evaluate(world, phase);
}

bool SequenceDirector::camera(SeqCameraPose& out) const {
    const Entry* best = nullptr;
    SeqCameraPose pose;
    for (const auto& e : entries_) {
        if (!e->engaged || e->serial == 0 || !e->player.sequence().camera) continue;
        SeqCameraPose p;
        if (!e->player.camera(p)) continue;
        if (!best || e->serial > best->serial) { best = e.get(); pose = p; }
    }
    if (!best) return false;
    out = pose;
    return true;
}

bool SequenceDirector::emissiveScale(scene::Entity ent, f32 out[3]) const {
    for (const auto& e : entries_)
        if (e->engaged && e->player.emissiveScale(ent, out)) return true;
    return false;
}

std::vector<scene::Entity> SequenceDirector::boundEntities() const {
    std::vector<scene::Entity> out;
    std::unordered_set<scene::Entity> seen;
    for (const auto& e : entries_)
        for (const scene::Entity x : e->player.movedEntities())
            if (seen.insert(x).second) out.push_back(x);
    return out;
}

bool SequenceDirector::empty() const {
    for (const auto& e : entries_)
        if (!e->player.empty()) return false;
    return true;
}

bool SequenceDirector::playingAny() const {
    for (const auto& e : entries_)
        if (e->player.playing()) return true;
    return false;
}

SequencePlayer* SequenceDirector::player(i32 seq) { Entry* e = at(seq); return e ? &e->player : nullptr; }
const SequencePlayer* SequenceDirector::player(i32 seq) const { const Entry* e = at(seq); return e ? &e->player : nullptr; }

void SequenceDirector::restore(scene::World& world) {
    for (auto& e : entries_) {
        e->player.releaseTriggered();
        e->player.pause();
        e->engaged = false;
        e->started = false;
        e->serial = 0;
    }
    store_->restore(world);
}

void SequenceDirector::rebase(const scene::World& world, const Vec3& deltaCm) {
    store_->rebase(world, deltaCm);   // every player writes into this one store
}

void SequenceDirector::clear() {
    entries_.clear();
    seqs_.clear();
    library_.setLevel(nullptr);
    library_.clear();
    bindings_ = SeqBindings{};
}

} // namespace aver::game

#endif
