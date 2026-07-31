// ComponentPool: the sparse set backing one component type.
#include "aver/scene/ComponentPool.hpp"

#include "aver/core/Assert.hpp"

#include <cstring>
#include <vector>

namespace aver::scene {

// Entity index -> dense slot + 1, the dense owner array, and the component bytes.
struct ComponentPool::Storage {
    std::vector<u32>    sparse;
    std::vector<Entity> dense;
    std::vector<u8>     bytes;
};

// Creates an empty pool and rejects a layout this pool cannot store.
ComponentPool::ComponentPool(u32 typeId, usize stride, usize align)
    : s_(new Storage()), typeId_(typeId), stride_(stride), align_(align) {
    AVER_ASSERTM(stride > 0, "a component with no bytes has nothing to store");
    // std::vector's allocator guarantees alignment for max_align_t and no more.
    AVER_ASSERTM(align <= alignof(std::max_align_t),
                 "over-aligned components need an allocator this pool does not have");
    AVER_ASSERTM(stride % align == 0, "stride must be a whole number of aligned elements");
}

ComponentPool::~ComponentPool() { delete s_; }

// Dense slot + 1 for `e`, or 0 when absent or the handle is stale.
u32 ComponentPool::denseSlotOf(Entity e) const {
    const u32 idx = entityIndex(e);
    if (e == kInvalidEntity || idx >= s_->sparse.size()) return 0;
    const u32 slot = s_->sparse[idx];
    if (slot == 0 || s_->dense[slot - 1] != e) return 0;
    return slot;
}

// True when this pool holds a component for `e`.
bool ComponentPool::has(Entity e) const { return denseSlotOf(e) != 0; }

// The component bytes for `e`, or nullptr when absent.
void* ComponentPool::get(Entity e) {
    const u32 slot = denseSlotOf(e);
    return slot ? dataAt(slot - 1) : nullptr;
}

const void* ComponentPool::get(Entity e) const {
    const u32 slot = denseSlotOf(e);
    return slot ? dataAt(slot - 1) : nullptr;
}

// Attaches zero-filled storage to `e`, or returns the bytes it already has.
void* ComponentPool::add(Entity e) {
    if (e == kInvalidEntity) return nullptr;
    if (const u32 slot = denseSlotOf(e)) return dataAt(slot - 1);

    const u32 idx = entityIndex(e);
    if (idx >= s_->sparse.size()) s_->sparse.resize(idx + 1, 0);

    const usize slot = s_->dense.size();
    s_->dense.push_back(e);
    s_->bytes.resize((slot + 1) * stride_, 0);
    s_->sparse[idx] = static_cast<u32>(slot + 1);
    return dataAt(slot);
}

// Drops `e`'s component by swap-and-pop. False when absent.
bool ComponentPool::remove(Entity e) {
    const u32 slot = denseSlotOf(e);
    if (slot == 0) return false;

    const usize dead = slot - 1;
    const usize last = s_->dense.size() - 1;
    if (dead != last) {
        std::memcpy(dataAt(dead), dataAt(last), stride_);
        s_->dense[dead]                          = s_->dense[last];
        s_->sparse[entityIndex(s_->dense[dead])] = static_cast<u32>(dead + 1);
    }
    s_->dense.pop_back();
    s_->bytes.resize(last * stride_);
    s_->sparse[entityIndex(e)] = 0;
    return true;
}

usize  ComponentPool::size() const              { return s_->dense.size(); }
Entity ComponentPool::entityAt(usize i) const   { return s_->dense[i]; }
void*  ComponentPool::dataAt(usize i)           { return s_->bytes.data() + i * stride_; }
const void* ComponentPool::dataAt(usize i) const{ return s_->bytes.data() + i * stride_; }

// Copies all three arrays into `dst`, which must be the same component type.
void ComponentPool::snapshotTo(ComponentPool& dst) const {
    AVER_ASSERTM(dst.typeId_ == typeId_ && dst.stride_ == stride_,
                 "a snapshot must be taken into a pool of the same component type");
    dst.s_->sparse = s_->sparse;
    dst.s_->dense  = s_->dense;
    dst.s_->bytes  = s_->bytes;
}

} // namespace aver::scene
