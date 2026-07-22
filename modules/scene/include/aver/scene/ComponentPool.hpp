#pragma once
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

namespace aver::scene {

// A sparse set, hand-rolled. Core has no container facility and no SlotMap or SparseSet exists
// anywhere in modules/, so there is nothing to copy and a dependency would be the wrong trade for a
// couple of hundred lines. EnTT is out for a harder reason: entt::entity's null is not zero, and
// this tree's one invalid-value rule is worth more than the library.
//
// A storage archetype is deliberately NOT used. Storage stays per-type, so adding a component to a
// live entity never moves another entity's memory and no chunk allocator has to be written first.
//
// The storage lives behind an opaque pointer for the reason MaterialLibrary's does: a dllexported
// class with standard-library members exports their layout too (MSVC C4251), and only this DLL may
// allocate or free them.
class AVER_SCENE_API ComponentPool {
public:
    ComponentPool(u32 typeId, usize stride, usize align);
    ~ComponentPool();
    ComponentPool(const ComponentPool&)            = delete;
    ComponentPool& operator=(const ComponentPool&) = delete;

    bool        has(Entity e) const;
    void*       get(Entity e);              // nullptr when absent — this tree does not throw
    const void* get(Entity e) const;
    void*       add(Entity e);              // zero-filled; returns the existing one if already present
    bool        remove(Entity e);           // swap-and-pop; the dense arrays stay packed

    // Dense slot + 1, and 0 when this pool does not hold `e`. The +1 is not a micro-optimisation:
    // it makes 0 mean "absent" inside the storage too, so the tree's one invalid-value rule holds
    // all the way down rather than only at the handle.
    u32 denseSlotOf(Entity e) const;

    usize       size() const;
    Entity      entityAt(usize i) const;
    void*       dataAt(usize i);
    const void* dataAt(usize i) const;

    usize stride() const { return stride_; }
    usize align() const  { return align_; }
    u32   typeId() const { return typeId_; }

    // Snapshot and restore are a memcpy in both directions because these three arrays are the
    // entire state of one component TYPE. There is no snapshot format and nothing to fix up.
    //
    // This is NOT a world snapshot. It does not carry the World's generation, liveSlot or the FIFO
    // free list, so pools restored on their own would desync generations against live handles. The
    // whole-world snapshot/restore that Play/Stop needs is World's job and is built in step 5; this
    // primitive is one piece of it, not the thing itself.
    void snapshotTo(ComponentPool& dst) const;

private:
    struct Storage;
    Storage* s_;
    u32      typeId_;
    usize    stride_;
    usize    align_;
};

} // namespace aver::scene
