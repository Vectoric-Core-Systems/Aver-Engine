// Raw deflate decoding over stb_image's inflater (the one STB_IMAGE_IMPLEMENTATION lives in Image.cpp; this file only calls it).
#include "aver/platform/Inflate.hpp"

#include "stb_image.h"

#include <limits>

namespace aver {

bool inflateRaw(const u8* in, usize inSize, u8* out, usize outSize) {
    constexpr usize kMax = static_cast<usize>(std::numeric_limits<int>::max());
    if (!in || inSize == 0 || inSize > kMax || outSize > kMax || (outSize != 0 && !out)) return false;
    char scratch[1];   // stb wants a buffer even for an empty result
    const int n = stbi_zlib_decode_noheader_buffer(outSize ? reinterpret_cast<char*>(out) : scratch, outSize ? static_cast<int>(outSize) : 1,
                                                   reinterpret_cast<const char*>(in), static_cast<int>(inSize));
    return n >= 0 && static_cast<usize>(n) == outSize;
}

}  // namespace aver
