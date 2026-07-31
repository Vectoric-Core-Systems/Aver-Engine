// Image decoding, and the one stb_image implementation TU in the engine.

#include "aver/platform/Image.hpp"

// THE one STB_IMAGE_IMPLEMENTATION in the whole engine; a second one is a duplicate-symbol error.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstring>

namespace aver {

namespace {

// Copies stb's RGBA output into `out` and frees it. False if `px` is null.
bool adopt(stbi_uc* px, int w, int h, int comp, ImageData& out) {
    if (!px) return false;
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.channels = static_cast<u32>(comp);
    out.pixels.resize(static_cast<usize>(w) * static_cast<usize>(h) * 4);
    std::memcpy(out.pixels.data(), px, out.pixels.size());
    stbi_image_free(px);
    return true;
}

} // namespace

// Decodes an image file into RGBA. False on a missing or corrupt file, with the reason in `err`.
bool decodeImage(const std::string& path, ImageData& out, std::string* err) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) {
        if (err) { const char* why = stbi_failure_reason(); *err = why ? why : "decode failed"; }
        return false;
    }
    return adopt(px, w, h, comp, out);
}

// Decodes an in-memory image into RGBA. False on an empty or corrupt buffer.
bool decodeImage(const u8* bytes, usize size, ImageData& out, std::string* err) {
    if (!bytes || size == 0) { if (err) *err = "empty buffer"; return false; }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4);
    if (!px) {
        if (err) { const char* why = stbi_failure_reason(); *err = why ? why : "decode failed"; }
        return false;
    }
    return adopt(px, w, h, comp, out);
}

} // namespace aver
