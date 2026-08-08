// Which physics body belongs to which entity.
//
// THIS ASSOCIATION DOES NOT EXIST ANYWHERE ELSE IN THE ENGINE, and that is a blocker for streaming
// rather than an inconvenience. The physics ABI hands back an opaque int and takes no entity
// (`aver_phys_add_static_box`, physics_abi.h:46-58). Around it, three separate places each kept
// their own half-answer:
//
//   GameLevel   a parallel std::vector<int32_t> with NO entity association at all
//               (GameLevel.hpp:74) -- it can remove every body it made, and cannot say which
//               belongs to any particular entity.
//   SandboxApp  an unordered_map<u32, int32_t> keyed by entity (SandboxApp.cpp:5271) -- the right
//               shape, in the editor only.
//   Character   a private C# field on the actor (Character.cs:50).
//
// So "unload the entities in this chunk and their collision with them" was not expressible: the
// engine could destroy the entities and had no way to find the bodies they had created. Worse, the
// two existing teardown paths both call scene::World::destroy DIRECTLY (GameLevel.cpp:197,
// SandboxApp.cpp:2503), and World::destroy takes the WHOLE SUBTREE -- so a child's body was already
// being leaked today, every time a parent was destroyed.
//
// Deliberately NOT a component. A body id is process-local -- it means nothing in another run -- and
// docs/CHUNKS.md 5.1 is emphatic that a process-local id must never end up somewhere that gets
// serialised. A side table cannot be written to a chunk by accident; a component field can.
#pragma once

#include "aver/core/Types.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

#  include <unordered_map>
#  include <vector>

namespace aver::world {

class BodyRegistry {
public:
    // Records that `body` belongs to `e`. A second body for the same entity REPLACES the first and
    // returns the displaced id, so the caller can remove it rather than leak it -- silently
    // overwriting is how the editor's map loses bodies today.
    i32 attach(scene::Entity e, i32 body) {
        const auto it = map_.find(static_cast<u32>(e));
        i32 displaced = -1;
        if (it != map_.end()) { displaced = it->second; it->second = body; }
        else map_.emplace(static_cast<u32>(e), body);
        return displaced;
    }

    // The body belonging to `e`, or -1.
    i32 bodyOf(scene::Entity e) const {
        const auto it = map_.find(static_cast<u32>(e));
        return it == map_.end() ? -1 : it->second;
    }

    // Forgets `e`'s body and returns it, or -1. The CALLER removes it from the simulation: this
    // module links Aver.Scene and not Aver.Physics, and it stays that way so a build without
    // physics still has a registry that compiles.
    i32 detach(scene::Entity e) {
        const auto it = map_.find(static_cast<u32>(e));
        if (it == map_.end()) return -1;
        const i32 b = it->second;
        map_.erase(it);
        return b;
    }

    // Detaches every entity in `w`'s subtree rooted at `e`, appending the bodies to `out`.
    //
    // THE SUBTREE, because World::destroy retires the subtree (World.cpp:256-280) and anything that
    // walks only the entity it was handed leaks its children's bodies. SandboxApp::destroyEntity
    // does exactly that today (:2500-2513), which is a live leak this exists to stop repeating.
    void detachSubtree(const scene::World& w, scene::Entity e, std::vector<i32>& out) {
        if (const i32 b = detach(e); b >= 0) out.push_back(b);
        const auto* h = w.component<scene::CHierarchy>(e, scene::kComponentHierarchy);
        if (!h) return;
        for (scene::Entity c = h->firstChild; c != scene::kInvalidEntity;) {
            const auto* ch = w.component<scene::CHierarchy>(c, scene::kComponentHierarchy);
            const scene::Entity next = ch ? ch->nextSibling : scene::kInvalidEntity;
            detachSubtree(w, c, out);
            c = next;
        }
    }

    // Every body still registered, so a teardown can prove it accounted for all of them.
    void drainAll(std::vector<i32>& out) {
        out.reserve(out.size() + map_.size());
        for (const auto& kv : map_) out.push_back(kv.second);
        map_.clear();
    }

    usize size() const { return map_.size(); }
    void clear() { map_.clear(); }

private:
    std::unordered_map<u32, i32> map_;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
