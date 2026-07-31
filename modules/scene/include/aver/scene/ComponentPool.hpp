// Storage for one component type: a hand-rolled sparse set keyed by entity index.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

namespace aver::scene {

// Holds every instance of one component type, packed dense and addressed by Entity.
class AVER_SCENE_API ComponentPool {
public:
    // Creates an empty pool for component `typeId` with elements of `stride` bytes.
    ComponentPool(u32 typeId, usize stride, usize align);
    ~ComponentPool();
    ComponentPool(const ComponentPool&)            = delete;
    ComponentPool& operator=(const ComponentPool&) = delete;

    // True when this pool holds a component for `e`.
    bool        has(Entity e) const;
    // The component bytes for `e`, or nullptr when absent.
    void*       get(Entity e);
    const void* get(Entity e) const;
    // Attaches zero-filled storage to `e`, or returns the bytes it already has.
    void*       add(Entity e);
    // Drops `e`'s component by swap-and-pop, keeping the dense arrays packed.
    bool        remove(Entity e);

    // Dense slot + 1, and 0 when this pool does not hold `e`.
    u32 denseSlotOf(Entity e) const;

    // Number of components stored.
    usize       size() const;
    // The entity owning dense slot `i`.
    Entity      entityAt(usize i) const;
    // The component bytes at dense slot `i`.
    void*       dataAt(usize i);
    const void* dataAt(usize i) const;

    usize stride() const { return stride_; }
    usize align() const  { return align_; }
    u32   typeId() const { return typeId_; }

    // Copies this pool's whole state into `dst`, which must be the same component type.
    void snapshotTo(ComponentPool& dst) const;

private:
    struct Storage;
    Storage* s_;
    u32      typeId_;
    usize    stride_;
    usize    align_;
};

} // namespace aver::scene
