// Which entities move during a play session. See PlayMobility.hpp for the rule and why it exists.
#include "aver/game/PlayMobility.hpp"

#if AVER_MODULE_SCENE
#include "aver/anim/AnimSystem.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cstring>

namespace aver::game {

namespace {

// True for an entity whose own transform an animation moves: a CAnimator on an entity with no rig whose
// clip is an OBJECT clip. An animator on a plain clip, or one whose clip did not load, never moves the
// entity, so seeding it as movable would only keep a static prop out of the GI bake for nothing.
bool objectAnimated(const scene::World& w, scene::Entity e) {
    if (w.hasComponent(e, scene::kComponentSkeletalMesh)) return false;
    const auto* an = w.component<scene::CAnimator>(e, scene::kComponentAnimator);
    if (!an || an->clip == 0) return false;
    const fmt::OcAnimation* c = anim::animSystem().clip(an->clip);
    return c && (c->flags & fmt::kOcAnimObject) != 0;
}

} // namespace

void PlayMobility::begin(const scene::World& w) {
    slots_.clear();
    root_ = scene::kInvalidEntity;
    pawnTree_.clear();
    pawnMark_.clear();
    pawnTreeFrame_ = 0;
    frame_ = 1;
    movableCount_ = 0;
    active_ = true;
    // Seeded, not Static: the rest transform is taken from the walk's own matrix on first sight.
    // Hashing World::worldMatrix here instead would compare two different sources, and a walk that
    // composed its matrix even slightly differently would flag the whole level as moved.
    for (u32 i = 0, n = w.count(); i < n; ++i) {
        const scene::Entity e = w.at(i);
        Slot& s = slotFor(e);
        s.e = e;
        // An object-animated prop (car, boat, fan) moves from its first frame, so it starts Movable and
        // skips the one-time GI rebuild its first move would cost. A rigged entity is excluded: its
        // CAnimator poses bones and leaves the world transform alone.
        if (objectAnimated(w, e)) {
            s.state = State::Movable;
            ++movableCount_;
        } else {
            s.state = State::Seeded;
        }
    }
}

void PlayMobility::seedMovable(const std::vector<scene::Entity>& entities) {
    if (!active_) return;
    for (const scene::Entity e : entities) {
        // Only an entity begin() saw: a slot past the table, or one holding another generation, means
        // the entity was born since, and the provisional rule already covers it.
        const u32 idx = scene::entityIndex(e);
        if (e == scene::kInvalidEntity || idx >= slots_.size() || slots_[idx].e != e) continue;
        Slot& s = slots_[idx];
        if (s.state == State::Movable) continue;
        s.state = State::Movable;
        ++movableCount_;
    }
}

void PlayMobility::end() {
    slots_.clear();
    slots_.shrink_to_fit();
    root_ = scene::kInvalidEntity;
    pawnTree_.clear();
    pawnMark_.clear();
    pawnMark_.shrink_to_fit();
    pawnTreeFrame_ = 0;
    movableCount_ = 0;
    active_ = false;
}

void PlayMobility::beginFrame(scene::Entity movableRoot) {
    if (!active_) return;
    ++frame_;
    root_ = movableRoot;
}

PlayMobility::Slot& PlayMobility::slotFor(scene::Entity e) {
    const u32 idx = scene::entityIndex(e);
    if (idx >= slots_.size()) slots_.resize(static_cast<usize>(idx) + 1);
    return slots_[idx];
}

void PlayMobility::markPawnSubtree(const scene::World& w) {
    pawnTreeFrame_ = frame_;
    for (const scene::Entity m : pawnTree_) pawnMark_[scene::entityIndex(m)] = scene::kInvalidEntity;
    pawnTree_.clear();
    // A root that is not alive marks nothing: no live entity has it as an ancestor.
    if (root_ == scene::kInvalidEntity || !w.valid(root_)) return;
    // Descendant walk; World::setParent refuses cycles, so this terminates.
    pawnStack_.clear();
    pawnStack_.push_back(root_);
    while (!pawnStack_.empty()) {
        const scene::Entity a = pawnStack_.back();
        pawnStack_.pop_back();
        pawnTree_.push_back(a);
        const u32 idx = scene::entityIndex(a);
        if (idx >= pawnMark_.size()) pawnMark_.resize(static_cast<usize>(idx) + 1, scene::kInvalidEntity);
        pawnMark_[idx] = a;
        for (scene::Entity c = w.firstChild(a); c != scene::kInvalidEntity; c = w.nextSibling(c))
            pawnStack_.push_back(c);
    }
}

bool PlayMobility::inPawnSubtree(scene::Entity e) const {
    const u32 idx = scene::entityIndex(e);
    return idx < pawnMark_.size() && pawnMark_[idx] == e;
}

u64 PlayMobility::hashWorld(const Mat4& m) {
    // Raw float bits, the same exactness Voxi's giDrawsKey uses: the question is "would the bake
    // differ", and any bit that changes the matrix changes the bake.
    u64 h = 1469598103934665603ull;
    const f32* f = &m.m[0][0];
    for (u32 i = 0; i < 16; ++i) {
        u32 bits = 0;
        std::memcpy(&bits, &f[i], sizeof(bits));
        h ^= static_cast<u64>(bits);
        h *= 1099511628211ull;
    }
    return h;
}

bool PlayMobility::movable(const scene::World& w, scene::Entity e, const Mat4& world) {
    if (!active_ || e == scene::kInvalidEntity) return false;
    Slot& s = slotFor(e);
    if (s.e != e) {
        // A slot that was empty at begin(), or one whose entity died and was reused under a new
        // generation: either way this entity was born during the session.
        s = Slot{};
        s.e = e;
        s.state = State::Provisional;
    }
    // Movable is terminal for the session, so nothing below could change the answer: no hash, no
    // counting, no ancestor test. Every animated prop and the whole pawn tree take this exit.
    if (s.state == State::Movable) return true;
    // Asked already this frame (the colour walk after the depth prepass): same answer, no counting.
    if (s.lastSeenFrame == frame_) return s.state == State::Provisional;

    // THE MATRIX IS HASHED ONLY WHEN IT CAN HAVE CHANGED. worldRevision is bumped exactly when
    // World recomposes this entity's matrix, so an unchanged revision is an unchanged matrix and the
    // hash would come out the same; a changed one is confirmed against the hash, because a recompose
    // that lands on the same bits (a transform set to the value it already had) is not a move.
    const bool firstSight = s.lastSeenFrame == 0;
    const u32 rev = w.worldRevision(e);
    bool moved = false;
    if (firstSight || rev != s.worldRev) {
        const u64 h = hashWorld(world);
        moved = !firstSight && h != s.worldHash;
        s.worldHash = h;
        s.worldRev = rev;
    }
    s.lastSeenFrame = frame_;

    if (pawnTreeFrame_ != frame_) markPawnSubtree(w);
    if (inPawnSubtree(e)) {
        s.state = State::Movable;
        ++movableCount_;
        return true;
    }
    switch (s.state) {
        case State::Seeded:
        case State::Unknown:
            s.state = State::Static;
            return false;
        case State::Static:
            if (moved) { s.state = State::Movable; ++movableCount_; return true; }
            return false;
        case State::Provisional:
            s.stillFrames = (moved || firstSight) ? u16{0} : static_cast<u16>(s.stillFrames + 1);
            if (static_cast<u32>(s.stillFrames) >= kSettleFrames) { s.state = State::Static; return false; }
            return true;
        case State::Movable:   // returned at the top; named so the switch covers every state
            return true;
    }
    return false;
}

} // namespace aver::game

#endif
