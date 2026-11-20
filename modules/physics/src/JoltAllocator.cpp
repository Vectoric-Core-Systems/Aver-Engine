// Jolt's heap, zeroed. Jolt allocates through function pointers the host sets (Jolt/Core/Memory.h); these are its defaults
// (malloc, realloc, free, _aligned_malloc / posix_memalign) with every new byte cleared.
//
// WHY. Jolt's IndependentAxisConstraintPart, the pulley's solver part, leaves its body-1 or body-2 vectors unset when that
// body is static and still reads them in SolveVelocityConstraint, multiplied by the static body's zero angular velocity.
// Memory that happened to hold a NaN makes that a NaN impulse and a NaN velocity: a pulley whose far end is the world made a
// box fly off now and then (Linux Debug CI hit it as Jolt's NaN assert, Windows Debug never, because its debug heap fills
// with 0xCD, a finite float). Upstream master reads them the same way, and the Jolt sources here are upstream's
// (modules/physics.jolt/README.md), so the engine hands Jolt memory with nothing left in it. tests/physics JointTest
// (testPulleyToWorldOnDirtyMemory) dirties the heap with NaNs and fails without this.
//
// The cost is a clear per heap allocation: bodies, shapes, constraints, array growth. Per-step scratch comes from the
// preallocated TempAllocatorImpl, cleared once.
#include "PhysicsInternal.hpp"

#include <Jolt/Core/Memory.h>

#include <cstdlib>
#include <cstring>

namespace aver::physics::detail {

namespace {

void* zeroAllocate(size_t size) { return std::calloc(1, size ? size : 1); }

void* zeroReallocate(void* block, size_t oldSize, size_t newSize) {
    void* grown = std::realloc(block, newSize ? newSize : 1);
    if (grown && newSize > oldSize) std::memset(static_cast<unsigned char*>(grown) + oldSize, 0, newSize - oldSize);
    return grown;
}

void freeBlock(void* block) { std::free(block); }

void* zeroAlignedAllocate(size_t size, size_t alignment) {
    void* block = nullptr;
#if defined(_WIN32)
    block = _aligned_malloc(size ? size : 1, alignment);
#else
    if (posix_memalign(&block, alignment < sizeof(void*) ? sizeof(void*) : alignment, size ? size : 1) != 0) block = nullptr;
#endif
    if (block) std::memset(block, 0, size);
    return block;
}

void alignedFreeBlock(void* block) {
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

}  // namespace

void registerJoltAllocator() {
    JPH::Allocate = zeroAllocate;
    JPH::Reallocate = zeroReallocate;
    JPH::Free = freeBlock;
    JPH::AlignedAllocate = zeroAlignedAllocate;
    JPH::AlignedFree = alignedFreeBlock;
}

}  // namespace aver::physics::detail
