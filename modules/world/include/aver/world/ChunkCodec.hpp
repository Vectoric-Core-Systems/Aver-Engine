// A ChunkPayload as bytes, and back.
//
// SEPARATE FROM THE CONTAINER on purpose. The region file decides where a chunk's bytes live; this
// decides what they are. Keeping them apart means the encoding can be tested without a file and the
// container without a scene -- and it is the encoding, not the container, that carries the rule
// nothing may violate: no process-local value, ever (see ChunkPayload.hpp).
//
// DETERMINISTIC BY CONSTRUCTION: fixed field order, no padding, no pointers, no timestamps, no
// hash-map iteration. Encoding the same payload twice must produce identical bytes, which is what
// makes "cook the same level twice and memcmp" a real test rather than a hopeful one.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/world/ChunkPayload.hpp"

#if AVER_MODULE_SCENE
#  include <string>
#  include <vector>

namespace aver::world {

// Version of the chunk encoding itself, independent of the container's. A region file records this
// so a reader can refuse a payload it does not understand instead of misreading it.
inline constexpr u32 kChunkCodecVersion = 1;

// Encodes one chunk's entities. The coord and chunk size are NOT encoded -- the container already
// knows both, and storing them twice is two things that can disagree.
std::vector<u8> encodeChunk(const ChunkPayload& p);

// Decodes into `out`, whose coord and chunkSizeCm the caller sets from the container.
// False on a truncated or malformed buffer, with `why` set.
bool decodeChunk(const u8* data, usize size, ChunkPayload& out, std::string* why = nullptr);

} // namespace aver::world

#endif // AVER_MODULE_SCENE
