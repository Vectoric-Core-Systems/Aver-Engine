#pragma once
#include "Types.hpp"
#include <string_view>

namespace aver {

// FNV-1a 64-bit. This is the EXACT algorithm OpenConstructor uses for .ocmap/.ocworld
// content ids (verified: fnv1a64("demoworld") == 0x376B85BC4D1A03BA). Preserved so
// carried-over worlds keep their identity. See docs/recon/ocmap-scene.md.
u64 fnv1a64(const void* data, usize len);
u64 fnv1a64(std::string_view s);

inline constexpr u64 kFnv1a64OffsetBasis = 1469598103934665603ull;
inline constexpr u64 kFnv1a64Prime       = 1099511628211ull;

} // namespace aver
