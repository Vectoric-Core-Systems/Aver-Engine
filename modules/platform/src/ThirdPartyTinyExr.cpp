// The tinyexr implementation, compiled in exactly this translation unit with warnings off (see
// modules/platform/CMakeLists.txt). The wrapper is ImageExr.cpp.
//
// Deflate comes from the stb_image already in the engine (stbi_zlib_decode_buffer, Image.cpp) instead of a
// second zlib. tinyexr's WRITER also wants stb_image_write's stbi_zlib_compress; the engine only reads
// EXR through this library (the render queue has its own writer, formats/ExrWrite.hpp), and several
// executables define that symbol themselves, so tinyexr is pointed at a name of ours that refuses.
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define stbi_zlib_compress aver_tinyexr_zlib_compress_unused
#include "tinyexr.h"

extern "C" unsigned char* aver_tinyexr_zlib_compress_unused(unsigned char*, int, int* outLen, int) {
    if (outLen) *outLen = 0;
    return nullptr;
}
