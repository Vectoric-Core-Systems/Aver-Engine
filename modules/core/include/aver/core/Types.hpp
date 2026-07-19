#pragma once
#include <cstddef>
#include <cstdint>

namespace aver {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;

// Generic 32-bit runtime handle/id (0 == invalid). Entities/components are handles,
// never base-class objects — this is the anti-UObject contract (docs/ARCHITECTURE.md P2).
using AvId = u32;
inline constexpr AvId kInvalidId = 0u;

} // namespace aver
