#pragma once
// BC7 block compression (mode 6 only), for the texture derived-data cache (TextureCache.hpp).
//
// WHY MODE 6 ALONE. One subset, RGBA interpolated together, 7-bit endpoints plus a p-bit and 4-bit
// indices: the single mode that covers colour, colour+alpha and normal maps without a partition
// search. It is a quarter of RGBA8's memory at a quality between BC1 and a full BC7 encoder, and it
// keeps the encoder short enough to own. A full multi-mode encoder is a drop-in upgrade behind the
// same entry points; the cache key carries kBc7EncoderVersion so a better encoder re-cooks.
#include "aver/core/Types.hpp"
#include "aver/platform/Image.hpp"

#include <vector>

namespace aver::fmt {

inline constexpr u32 kBc7EncoderVersion = 1;

struct Bc7Options {
    // A CUTOUT texture (alpha tested, not blended): a texel below the cut contributes almost nothing
    // to the colour fit -- nobody sees it -- so the endpoints spend their precision on the leaf.
    bool cutout = false;
    u8   cutoff = 128;
};

// One 4x4 block: 16 RGBA texels in row order in, 16 bytes out.
void encodeBc7Block(const u8 rgba[64], u8 out[16], const Bc7Options& opt = {});

// Decodes a block of ANY mode 6 encoding (the modes this encoder writes). Other modes decode to
// opaque magenta so a test can tell. For tests and the cache's own verification.
void decodeBc7Block(const u8 in[16], u8 rgba[64]);

// A whole image (any size; edge blocks clamp-replicate), blocks in row order, 16 bytes each.
// `threads` 0 = hardware concurrency.
void encodeBc7Image(const ImageData& img, std::vector<u8>& out, const Bc7Options& opt = {}, u32 threads = 0);

} // namespace aver::fmt
