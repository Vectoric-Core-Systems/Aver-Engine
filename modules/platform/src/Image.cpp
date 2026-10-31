// Image decoding, and the one stb_image implementation TU in the engine.

#include "aver/platform/Image.hpp"
#include "ImageCodecs.hpp"

// THE one STB_IMAGE_IMPLEMENTATION in the whole engine; a second one is a duplicate-symbol error.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

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

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

bool hasMagic(const u8* b, usize n, usize off, const char* magic, usize len) {
    return b && n >= off + len && std::memcmp(b + off, magic, len) == 0;
}

// Skips a UTF-8 byte order mark and white space; returns the offset of the first other byte.
usize skipBomAndSpace(const u8* b, usize n) {
    usize i = 0;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) i = 3;
    while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) ++i;
    return i;
}

// A document that starts like XML (after a BOM and white space).
bool startsLikeXml(const u8* b, usize n) {
    const usize i = skipBomAndSpace(b, n);
    return i < n && b[i] == '<';
}

bool endsWithSvg(const std::string& path) {
    if (path.size() < 4) return false;
    const char* e = path.c_str() + path.size() - 4;
    return e[0] == '.' && (e[1] == 's' || e[1] == 'S') && (e[2] == 'v' || e[2] == 'V') && (e[3] == 'g' || e[3] == 'G');
}

// Reads a whole file, refusing one larger than `cap`.
bool readFile(const std::string& path, usize cap, std::vector<u8>& out, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(err, "the file could not be opened");
    const std::streamoff n = f.tellg();
    if (n < 0) return fail(err, "the file could not be read");
    if (static_cast<u64>(n) > cap) return fail(err, "the file is larger than the decoder will read");
    out.resize(static_cast<usize>(n));
    f.seekg(0);
    if (n > 0 && !f.read(reinterpret_cast<char*>(out.data()), n)) return fail(err, "the file could not be read");
    return true;
}

// A header that states more pixels than the 8-bit output may hold is refused before stb allocates for it.
bool stbSizeOk(bool haveInfo, int w, int h, std::string* err) {
    if (haveInfo && w > 0 && h > 0 && static_cast<u64>(w) * static_cast<u64>(h) > kMaxStbImagePixels)
        return fail(err, "the image has more pixels than the decoder allows");
    return true;
}

bool decodeStb(const u8* bytes, usize size, ImageData& out, std::string* err) {
    if (size > static_cast<usize>(std::numeric_limits<int>::max())) return fail(err, "the image is too large");
    int w = 0, h = 0, comp = 0;
    const bool haveInfo = stbi_info_from_memory(bytes, static_cast<int>(size), &w, &h, &comp) != 0;
    if (!stbSizeOk(haveInfo, w, h, err)) return false;
    stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4);
    if (!px) {
        if (err) { const char* why = stbi_failure_reason(); *err = why ? why : "decode failed"; }
        return false;
    }
    return adopt(px, w, h, comp, out);
}

} // namespace

bool isWebpData(const u8* b, usize n) { return hasMagic(b, n, 0, "RIFF", 4) && hasMagic(b, n, 8, "WEBP", 4); }
bool isOpenExrData(const u8* b, usize n) { return hasMagic(b, n, 0, "\x76\x2f\x31\x01", 4); }

bool isSvgData(const u8* b, usize n) {
    if (!b || !startsLikeXml(b, n)) return false;
    // The root element, found in the first 8 KB: an XML declaration, a doctype and comments may come first.
    const usize limit = n < 8192 ? n : 8192;
    for (usize i = 0; i + 4 <= limit; ++i)
        if (b[i] == '<' && b[i + 1] == 's' && b[i + 2] == 'v' && b[i + 3] == 'g') return true;
    return false;
}

const char* imageExtensionFor(const u8* b, usize n) {
    if (isWebpData(b, n)) return ".webp";
    if (isOpenExrData(b, n)) return ".exr";
    if (isSvgData(b, n)) return ".svg";
    return nullptr;
}

namespace detail {

void floatToRgba8(const FloatImageData& in, ImageData& out) {
    out.width = in.width;
    out.height = in.height;
    out.channels = in.channels;
    const usize count = static_cast<usize>(in.width) * in.height;
    out.pixels.resize(count * 4);
    constexpr f32 kInvGamma = 1.0f / 2.2f;
    for (usize i = 0; i < count; ++i) {
        for (usize c = 0; c < 4; ++c) {
            f32 v = in.rgba[i * 4 + c];
            if (!(v > 0.0f)) v = 0.0f;            // negatives and NaN
            else if (v > 1.0f) v = 1.0f;          // includes +inf
            if (c < 3) v = std::pow(v, kInvGamma);
            out.pixels[i * 4 + c] = static_cast<u8>(v * 255.0f + 0.5f);
        }
    }
}

} // namespace detail

// Decodes an image file into RGBA. False on a missing or corrupt file, with the reason in `err`.
bool decodeImage(const std::string& path, ImageData& out, std::string* err) {
    return decodeImage(path, out, ImageDecodeOptions{}, err);
}

bool decodeImage(const std::string& path, ImageData& out, const ImageDecodeOptions& opt, std::string* err) {
    // The formats chosen by content are read whole; everything else still goes straight to stb_image
    // from the path, as it always did.
    u8 head[8192] = {};
    usize headLen = 0;
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(head), sizeof head);
            headLen = static_cast<usize>(f.gcount());
        }
    }
    const bool viaBytes = isWebpData(head, headLen) || isOpenExrData(head, headLen) ||
                          isSvgData(head, headLen) || (endsWithSvg(path) && startsLikeXml(head, headLen));
    if (viaBytes) {
        std::vector<u8> bytes;
        if (!readFile(path, kMaxImageFileBytes, bytes, err)) return false;
        return decodeImage(bytes.data(), bytes.size(), out, opt, err);
    }
    int w = 0, h = 0, comp = 0;
    const bool haveInfo = stbi_info(path.c_str(), &w, &h, &comp) != 0;
    if (!stbSizeOk(haveInfo, w, h, err)) return false;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) {
        if (err) { const char* why = stbi_failure_reason(); *err = why ? why : "decode failed"; }
        return false;
    }
    return adopt(px, w, h, comp, out);
}

// Decodes an in-memory image into RGBA. False on an empty or corrupt buffer.
bool decodeImage(const u8* bytes, usize size, ImageData& out, std::string* err) {
    return decodeImage(bytes, size, out, ImageDecodeOptions{}, err);
}

bool decodeImage(const u8* bytes, usize size, ImageData& out, const ImageDecodeOptions& opt, std::string* err) {
    if (!bytes || size == 0) return fail(err, "empty buffer");
    if (isWebpData(bytes, size)) return detail::decodeWebp(bytes, size, out, err);
    if (isOpenExrData(bytes, size)) {
        FloatImageData f;
        if (!detail::decodeExr(bytes, size, f, err)) return false;
        detail::floatToRgba8(f, out);
        return true;
    }
    if (isSvgData(bytes, size)) return detail::decodeSvg(bytes, size, opt.svgWidth, opt.svgHeight, out, err);
    return decodeStb(bytes, size, out, err);
}

bool decodeFloatImage(const std::string& path, FloatImageData& out, std::string* err) {
    u8 head[16] = {};
    usize headLen = 0;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return fail(err, "the file could not be opened");
        f.read(reinterpret_cast<char*>(head), sizeof head);
        headLen = static_cast<usize>(f.gcount());
    }
    if (!isOpenExrData(head, headLen)) return fail(err, "not a floating-point image container (only OpenEXR is read)");
    std::vector<u8> bytes;
    if (!readFile(path, kMaxImageFileBytes, bytes, err)) return false;
    return decodeFloatImage(bytes.data(), bytes.size(), out, err);
}

bool decodeFloatImage(const u8* bytes, usize size, FloatImageData& out, std::string* err) {
    out = FloatImageData{};
    if (!bytes || size == 0) return fail(err, "empty buffer");
    if (!isOpenExrData(bytes, size)) return fail(err, "not a floating-point image container (only OpenEXR is read)");
    return detail::decodeExr(bytes, size, out, err);
}

// Names a container stb_image cannot read. See the header for why this is a refusal list.
//
// WHY IT MATTERS: Intel's Jungle Ruins binds `.tif` base colours through UsdPreviewSurface, and
// every one of them carries the leaf cutout in a fourth sample. Copying one into a project and
// writing a TEX record for it produces a material that fails to load at run time, a long way from
// the import that caused it.
const char* undecodableContainer(const u8* b, usize n) {
    const auto has = [&](usize off, const char* magic, usize len) { return hasMagic(b, n, off, magic, len); };
    if (has(0, "II*\0", 4) || has(0, "MM\0*", 4))        return "TIFF";
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
    for (const char* ext : {".png", ".PNG", ".tga", ".TGA", ".webp", ".WEBP", ".jpg", ".JPG", ".jpeg", ".JPEG"}) {
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
