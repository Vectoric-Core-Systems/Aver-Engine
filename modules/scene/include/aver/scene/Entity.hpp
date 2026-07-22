#pragma once
#include "aver/core/Types.hpp"

// The module's base header, so the C++ export macro lives here for the same reason
// AVER_PBR_API lives in Material.hpp: the first header every other one includes is the only place
// it can be defined once.
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

// Entities ARE Core's AvId — the type Types.hpp declared for exactly this purpose with the
// anti-UObject contract attached. Reviving it is worth more than a fourth id family alongside AvId,
// the RHI's u32 aliases and ObjectId.
//
// Layout: index in bits 0..23, generation in bits 24..30, bit 31 always clear. Three things fall out
// of that and all three are load-bearing rather than decorative. Index 0 is never handed out and a
// live generation starts at 1, so no live handle can equal 0. Bit 31 stays clear, so the same value
// crosses the C ABI as a POSITIVE int32_t and a live entity can never be mistaken for an error
// return. And the whole handle is one u32, so a component pool's owner array is dense.
using Entity = AvId;
inline constexpr Entity kInvalidEntity = kInvalidId;

inline constexpr u32 kEntityIndexBits = 24;
inline constexpr u32 kEntityGenBits   = 7;
inline constexpr u32 kEntityIndexMask = (1u << kEntityIndexBits) - 1u;
inline constexpr u32 kEntityMaxGen    = (1u << kEntityGenBits) - 1u;  // 0 means "this slot never lived"
inline constexpr u32 kMaxEntities     = kEntityIndexMask;             // index 0 is reserved

inline constexpr u32 entityIndex(Entity e) { return e & kEntityIndexMask; }
inline constexpr u32 entityGen(Entity e)   { return (e >> kEntityIndexBits) & kEntityMaxGen; }

inline constexpr Entity makeEntity(u32 index, u32 gen) {
    return (index & kEntityIndexMask) | ((gen & kEntityMaxGen) << kEntityIndexBits);
}

// Seven generation bits is few, and the design handles the wrap rather than asserting it away: the
// free list is FIFO so an index gets maximum distance before reuse, and a slot whose generation
// would wrap past kEntityMaxGen is RETIRED instead of recycled. 16.7M indices makes retiring
// affordable; widening the field would cost either the index range or the bit-31-clear rule.
static_assert(kEntityIndexBits + kEntityGenBits == 31,
              "bit 31 must stay clear so a live entity crosses the C ABI as a positive int32_t");

} // namespace aver::scene
