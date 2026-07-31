#pragma once
// FNV-1a 64-bit content hashing, and its two constants.
#include "Types.hpp"
#include <string_view>

namespace aver {

// FNV-1a 64-bit over raw bytes. Must match OpenConstructor's .ocmap/.ocworld content ids.
u64 fnv1a64(const void* data, usize len);
// FNV-1a 64-bit over a string.
u64 fnv1a64(std::string_view s);

// Must match the C# side's spelling in Aver.Scene/Native.cs.
inline constexpr u64 kFnv1a64OffsetBasis = 0xcbf29ce484222325ull;   // == 14695981039346656037
inline constexpr u64 kFnv1a64Prime       = 1099511628211ull;        // == 0x100000001b3

} // namespace aver
