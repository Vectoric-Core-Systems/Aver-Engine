#pragma once
#include "Types.hpp"
#include <string_view>

namespace aver {

// FNV-1a 64-bit. This is the EXACT algorithm OpenConstructor uses for .ocmap/.ocworld
// content ids (verified: fnv1a64("demoworld") == 0x376B85BC4D1A03BA). Preserved so
// carried-over worlds keep their identity. See docs/recon/ocmap-scene.md.
u64 fnv1a64(const void* data, usize len);
u64 fnv1a64(std::string_view s);

// The canonical FNV-1a 64-bit offset basis, written as its hex form (0xcbf29ce484222325) because that
// is how it is universally documented and how the C# side spells it (Aver.Scene/Native.cs). A prior
// decimal transcription dropped a digit (1469598103934665603, 19 digits for a 20-digit constant), which
// silently made every native hash disagree with OpenConstructor's files and with the C# ObjectId — while
// the one test that "verified" it only ever parsed the id out of the file, never recomputed it. The
// recompute check now lives in FormatTest so the constant cannot drift again.
inline constexpr u64 kFnv1a64OffsetBasis = 0xcbf29ce484222325ull;   // == 14695981039346656037
inline constexpr u64 kFnv1a64Prime       = 1099511628211ull;        // == 0x100000001b3

} // namespace aver
