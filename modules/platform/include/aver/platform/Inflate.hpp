#pragma once
#include "aver/core/Types.hpp"

namespace aver {

// Decodes the raw deflate stream (RFC 1951, no zlib header) in `in` into `out`, which the stream must fill with exactly `outSize` bytes. It is
// the decoder of the stb_image the engine already carries, so a zip reader needs no second zlib. False for a malformed stream, one that would
// write past `outSize` or stop short of it, and for sizes the decoder cannot count (2 GiB and up). The caller checks what the bytes are
// (a zip entry carries a CRC-32): a stream cut short can decode to bytes that are only wrong.
bool inflateRaw(const u8* in, usize inSize, u8* out, usize outSize);

}  // namespace aver
