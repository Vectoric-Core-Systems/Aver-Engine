// Image decoding, and the one stb_image implementation TU in the engine.

#include "aver/platform/Image.hpp"

// THE one STB_IMAGE_IMPLEMENTATION in the whole engine; a second one is a duplicate-symbol error.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstring>
#include <fstream>

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

// Names a container stb_image cannot read. See the header for why this is a refusal list.
//
// WHY IT MATTERS: Intel's Jungle Ruins binds `.tif` base colours through UsdPreviewSurface, and
// every one of them carries the leaf cutout in a fourth sample. Copying one into a project and
// writing a TEX record for it produces a material that fails to load at run time, a long way from
// the import that caused it.
const char* undecodableContainer(const u8* b, usize n) {
    const auto has = [&](usize off, const char* magic, usize len) {
        if (!b || n < off + len) return false;
        return std::memcmp(b + off, magic, len) == 0;
    };
    if (has(0, "II*\0", 4) || has(0, "MM\0*", 4))        return "TIFF";
    if (has(0, "RIFF", 4) && has(8, "WEBP", 4))          return "WebP";
    if (has(0, "\x76\x2f\x31\x01", 4))                   return "OpenEXR";
    if (has(0, "DDS ", 4))                               return "DDS";
    if (has(1, "KTX", 3) && n > 0 && b[0] == 0xAB)       return "KTX";
    if (has(4, "ftypavif", 8))                           return "AVIF";
    if (has(4, "ftypheic", 8) || has(4, "ftypheix", 8))  return "HEIF";
    return nullptr;
}

const char* undecodableContainer(const std::vector<u8>& b) {
    return undecodableContainer(b.data(), b.size());
}

std::string findDecodableSibling(const std::string& absPath) {
    const usize slash = absPath.find_last_of("/\\");
    const usize dot = absPath.find_last_of('.');
    // A dot BEFORE the last separator belongs to a directory name, not to this file, so a path like
    // "C:/tex.v2/leaf" has no extension to replace and nothing to look for.
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    const std::string base = absPath.substr(0, dot);

    // THE STEM COMES OFF DISK, not from any sanitised display name an importer may already have
    // derived -- those have had their punctuation replaced and would no longer name a real file.
    for (const char* ext : {".png", ".PNG", ".tga", ".TGA", ".jpg", ".JPG", ".jpeg", ".JPEG"}) {
        const std::string cand = base + ext;
        if (cand == absPath) continue;                   // never "substitute" the file for itself
        std::ifstream f(cand, std::ios::binary);
        if (!f) continue;
        // OPENING IS NOT ENOUGH: a zero-byte placeholder beside a real .tif would otherwise be
        // chosen and then fail to decode, turning one stated limit into a different silent one.
        char probe = 0;
        if (!f.read(&probe, 1)) continue;
        return cand;
    }
    return {};
}

} // namespace aver
