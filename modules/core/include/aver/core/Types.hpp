#pragma once
// The engine's fixed-width scalar aliases and its generic runtime handle.
#include <cstddef>
#include <cstdint>

// The platform's own path separator as a string literal, for building a path out of pieces:
// `dir + AVER_PATH_SEP "Saved" AVER_PATH_SEP "Crashes"`. A macro and not a constant so it joins the
// literals beside it at compile time -- on Windows the result is byte-for-byte the "\\Saved\\Crashes"
// this tree always wrote, so prefix checks and equality between paths built here and paths the
// Windows shell hands back keep matching. Everywhere else it is '/', the only separator there is.
#if defined(_WIN32)
#define AVER_PATH_SEP "\\"
#else
#define AVER_PATH_SEP "/"
#endif

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

// Generic 32-bit runtime handle/id (0 == invalid). Entities and components are handles,
// never base-class objects.
using AvId = u32;
inline constexpr AvId kInvalidId = 0u;

} // namespace aver
