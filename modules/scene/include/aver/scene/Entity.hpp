// The scene module's base header: the Entity handle, its bit layout, and the module export macro.
#pragma once
#include "aver/core/Types.hpp"

namespace aver::scene {

#if defined(_WIN32)
#  if defined(AVER_SCENE_BUILD)
#    define AVER_SCENE_API __declspec(dllexport)
#  else
#    define AVER_SCENE_API __declspec(dllimport)
#  endif
#else
#  define AVER_SCENE_API
#endif

// A generational entity handle: index in bits 0..23, generation in bits 24..30, bit 31 always clear.
using Entity = AvId;
inline constexpr Entity kInvalidEntity = kInvalidId;

inline constexpr u32 kEntityIndexBits = 24;
inline constexpr u32 kEntityGenBits   = 7;
inline constexpr u32 kEntityIndexMask = (1u << kEntityIndexBits) - 1u;
inline constexpr u32 kEntityMaxGen    = (1u << kEntityGenBits) - 1u;  // 0 means "this slot never lived"
inline constexpr u32 kMaxEntities     = kEntityIndexMask;             // index 0 is reserved

// Index part of a handle.
inline constexpr u32 entityIndex(Entity e) { return e & kEntityIndexMask; }
// Generation part of a handle.
inline constexpr u32 entityGen(Entity e)   { return (e >> kEntityIndexBits) & kEntityMaxGen; }

// Packs an index and a generation into a handle.
inline constexpr Entity makeEntity(u32 index, u32 gen) {
    return (index & kEntityIndexMask) | ((gen & kEntityMaxGen) << kEntityIndexBits);
}

// Bit 31 must stay clear so a live entity crosses the C ABI as a positive int32_t.
static_assert(kEntityIndexBits + kEntityGenBits == 31,
              "bit 31 must stay clear so a live entity crosses the C ABI as a positive int32_t");

} // namespace aver::scene
