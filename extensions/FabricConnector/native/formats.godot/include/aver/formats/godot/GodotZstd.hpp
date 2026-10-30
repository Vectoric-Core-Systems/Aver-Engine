#pragma once
// A Zstandard decoder (RFC 8878), here for one job: Godot's compressed binary resources (RSCC) store each 4 KiB block as a
// Zstandard frame. Written from the format specification. It decodes frames to memory; it is not a streaming API.
// Supported: raw, RLE and compressed blocks, Huffman and treeless literals, predefined / RLE / FSE / repeat sequence tables,
// concatenated and skippable frames. Not supported: dictionaries. The content checksum is skipped, not verified.
// Input is untrusted: every length is checked against the buffer, the output is capped, and a bad stream returns false.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt::godot {

// Appends the decoded bytes of every frame in src[0, size) to `out`. False (with *why) on a malformed stream, an unsupported
// feature, or when the output would pass `maxOut` bytes in total. `out` keeps what was decoded before the failure.
bool zstdDecompress(const u8* src, usize size, std::vector<u8>& out, usize maxOut, std::string* why);

}  // namespace aver::fmt::godot
