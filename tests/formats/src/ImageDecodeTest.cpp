// ImageDecodeTest -- aver::decodeImage and decodeFloatImage for the three formats that arrive through
// vendored decoders: WebP (libwebp), SVG (NanoSVG) and OpenEXR (tinyexr), plus what must not have moved
// (stb_image's formats, the refusal list) and what hostile input does to all of them.
//
// No file here is a copy of anyone's image. The EXR files are built by the small writer below (scanline,
// tiled, uncompressed and stored-deflate ZIPS), the SVGs are text, the lossless WebPs are written bit by
// bit (a VP8L stream whose Huffman codes have one or two symbols), and the lossy / alpha / animated WebPs
// are short vectors made with a reference encoder, kept as byte arrays with the pixels they decode to.
// Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/formats/ExrWrite.hpp"
#include "aver/formats/PanoramaSky.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/platform/Image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---- little-endian helpers ----------------------------------------------------------------------
static void put16(std::vector<u8>& v, u32 x) { v.push_back(u8(x)); v.push_back(u8(x >> 8)); }
static void put24(std::vector<u8>& v, u32 x) { put16(v, x); v.push_back(u8(x >> 16)); }
static void put32(std::vector<u8>& v, u32 x) { put24(v, x); v.push_back(u8(x >> 24)); }
static void put64(std::vector<u8>& v, u64 x) { put32(v, u32(x)); put32(v, u32(x >> 32)); }
static void putStr(std::vector<u8>& v, const char* s) { while (*s) v.push_back(u8(*s++)); }
static void putF32(std::vector<u8>& v, f32 f) { u32 b; std::memcpy(&b, &f, 4); put32(v, b); }

// ======================================================================================================
// WebP builders
// ======================================================================================================

// LSB-first bit writer, the order VP8L reads.
struct BitWriter {
    std::vector<u8> bytes;
    u32 used = 0;
    void bits(u32 value, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            if (used == 0) bytes.push_back(0);
            if ((value >> i) & 1u) bytes.back() = u8(bytes.back() | (1u << used));
            used = (used + 1) & 7u;
        }
    }
};

static std::vector<u8> chunk(const char* tag, const std::vector<u8>& body) {
    std::vector<u8> c;
    putStr(c, tag);
    put32(c, u32(body.size()));
    c.insert(c.end(), body.begin(), body.end());
    if (body.size() & 1) c.push_back(0);
    return c;
}

static std::vector<u8> riffWebp(const std::vector<u8>& chunks) {
    std::vector<u8> f;
    putStr(f, "RIFF");
    put32(f, u32(4 + chunks.size()));
    putStr(f, "WEBP");
    f.insert(f.end(), chunks.begin(), chunks.end());
    return f;
}

// A VP8L stream with no transforms and no colour cache whose five prefix codes each have one symbol, except
// green, which has `greenA` (code 0) and `greenB` (code 1, only when greenB >= 0): `bitsPerPixel` choose.
// The pixel data is one bit per pixel in green, none in the other channels.
static std::vector<u8> vp8lStream(u32 w, u32 h, bool alphaUsed, int greenA, int greenB, int red, int blue, int alpha,
                                  const std::vector<u8>& pixelBits /* one 0/1 per pixel */) {
    BitWriter bw;
    bw.bits(0x2f, 8);
    bw.bits(w - 1, 14);
    bw.bits(h - 1, 14);
    bw.bits(alphaUsed ? 1 : 0, 1);
    bw.bits(0, 3);   // version
    bw.bits(0, 1);   // no transform
    bw.bits(0, 1);   // no colour cache
    bw.bits(0, 1);   // one prefix group for the whole image
    const auto simple = [&](int a, int b) {
        bw.bits(1, 1);                // simple code
        bw.bits(b >= 0 ? 1 : 0, 1);   // number of symbols - 1
        bw.bits(1, 1);                // first symbol is 8 bits
        bw.bits(u32(a), 8);
        if (b >= 0) bw.bits(u32(b), 8);
    };
    simple(greenA, greenB);
    simple(red, -1);
    simple(blue, -1);
    simple(alpha, -1);
    simple(0, -1);   // distance
    if (greenB >= 0)
        for (u8 bit : pixelBits) bw.bits(bit, 1);
    return bw.bytes;
}

static std::vector<u8> vp8lFile(const std::vector<u8>& stream) { return riffWebp(chunk("VP8L", stream)); }

// ======================================================================================================
// Reference-encoder vectors (8x8 colour ramp: r = 32 + 24x, g = 40 + 24y, b = 200 - 10x; alpha 40 + 28y)
// ======================================================================================================
static const unsigned char kLossy[] = {
    0x52, 0x49, 0x46, 0x46, 0x64, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x20,
    0x58, 0x00, 0x00, 0x00, 0x50, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00, 0x08, 0x00, 0x00, 0x80,
    0x08, 0x25, 0xb0, 0x02, 0x74, 0x72, 0x00, 0xab, 0xff, 0x03, 0xd1, 0x32, 0xc0, 0x04, 0x00, 0x00,
    0xfe, 0xfe, 0xc1, 0xeb, 0xbd, 0x5d, 0xe6, 0x77, 0xbc, 0x48, 0x77, 0x6e, 0xef, 0xa6, 0xc6, 0x8f,
    0xfe, 0xcc, 0x6f, 0x75, 0xfe, 0xf8, 0x73, 0x1b, 0xe8, 0xdf, 0xf7, 0x59, 0x0b, 0xff, 0xb7, 0x94,
    0x4b, 0xfc, 0x19, 0xdf, 0xe7, 0xff, 0x2a, 0xff, 0xf0, 0x47, 0x91, 0xd3, 0xff, 0xfc, 0x2b, 0x8f,
    0xff, 0x03, 0xd9, 0xbf, 0xff, 0xd0, 0x47, 0xf2, 0x83, 0x3e, 0x60, 0x00,
};

static const unsigned char kLossless[] = {
    0x52, 0x49, 0x46, 0x46, 0x3a, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c,
    0x2e, 0x00, 0x00, 0x00, 0x2f, 0x07, 0xc0, 0x01, 0x00, 0xb9, 0x32, 0x44, 0xf4, 0x3f, 0x76, 0x11,
    0xd1, 0xff, 0x00, 0x61, 0xb6, 0xd1, 0x30, 0x86, 0x70, 0xfe, 0x8c, 0x7b, 0x1d, 0x81, 0x58, 0x30,
    0xc5, 0xc3, 0xe8, 0x4c, 0x2c, 0x98, 0xcc, 0x9f, 0x2c, 0xa2, 0xee, 0x98, 0x80, 0x1d, 0xdd, 0x02,
    0xfd, 0x07,
};

static const unsigned char kLossyAlpha[] = {
    0x52, 0x49, 0x46, 0x46, 0x94, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x58,
    0x0a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x41, 0x4c,
    0x50, 0x48, 0x16, 0x00, 0x00, 0x00, 0x01, 0xb9, 0x8c, 0xe8, 0x7f, 0x80, 0x20, 0xdb, 0xa6, 0x20,
    0x03, 0xb8, 0xbf, 0xe6, 0x05, 0x22, 0x26, 0x20, 0x16, 0x00, 0xc8, 0x0d, 0x56, 0x50, 0x38, 0x20,
    0x58, 0x00, 0x00, 0x00, 0x10, 0x02, 0x00, 0x9d, 0x01, 0x2a, 0x08, 0x00, 0x08, 0x00, 0x00, 0x80,
    0x08, 0x25, 0xb0, 0x02, 0x74, 0x47, 0xff, 0x81, 0xe8, 0x99, 0x60, 0x12, 0xa0, 0x00, 0xfe, 0xfe,
    0xc1, 0xeb, 0xbd, 0x5d, 0xe6, 0x77, 0xbc, 0x48, 0x77, 0x6e, 0xef, 0xa6, 0xc6, 0x8f, 0xfe, 0xd4,
    0x47, 0xfb, 0xd7, 0xfb, 0xe1, 0xcc, 0x6f, 0x40, 0xbf, 0xce, 0xac, 0xb7, 0xff, 0xed, 0xe5, 0x12,
    0xfe, 0xef, 0xff, 0x9b, 0xff, 0xe5, 0x5f, 0xfe, 0x08, 0xf2, 0x3a, 0x7f, 0xff, 0x85, 0x71, 0xff,
    0xe0, 0x7b, 0x37, 0xff, 0xf9, 0xe0, 0xff, 0xf2, 0x83, 0x3e, 0x60, 0x00,
};

static const unsigned char kLosslessAlpha[] = {
    0x52, 0x49, 0x46, 0x46, 0x40, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c,
    0x34, 0x00, 0x00, 0x00, 0x2f, 0x07, 0xc0, 0x01, 0x10, 0xb9, 0x32, 0x44, 0xf4, 0x3f, 0x76, 0x11,
    0xd1, 0xff, 0x00, 0x61, 0xb6, 0xd1, 0x30, 0x86, 0x70, 0xfe, 0x8c, 0x7b, 0x1d, 0x81, 0x58, 0x30,
    0xc5, 0xc3, 0xe8, 0x4c, 0x2c, 0x98, 0xcc, 0x9f, 0x2c, 0xa2, 0x6e, 0x81, 0x40, 0x8a, 0x03, 0x59,
    0xe0, 0x09, 0xd8, 0x4d, 0x6b, 0x09, 0xf4, 0x1f,
};

// Two frames: the ramp, then flat red. Frame 0 is what a decoder must give.
static const unsigned char kAnimated[] = {
    0x52, 0x49, 0x46, 0x46, 0xda, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x58,
    0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x41, 0x4e,
    0x49, 0x4d, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x4e, 0x4d, 0x46,
    0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x07, 0x00, 0x00,
    0x64, 0x00, 0x00, 0x02, 0x56, 0x50, 0x38, 0x4c, 0x66, 0x00, 0x00, 0x00, 0x2f, 0x07, 0xc0, 0x01,
    0x00, 0xff, 0x41, 0x90, 0x6d, 0xa3, 0x51, 0x84, 0x21, 0xdd, 0x5f, 0xe7, 0x02, 0x62, 0xc1, 0x14,
    0x0f, 0xa3, 0x33, 0xb1, 0x60, 0x32, 0x7f, 0xb2, 0x88, 0xba, 0xe7, 0xbf, 0x0d, 0x5d, 0x00, 0x00,
    0x00, 0x58, 0x7b, 0x01, 0x00, 0x83, 0x00, 0x00, 0x88, 0x46, 0xb3, 0x6d, 0xdb, 0xb6, 0x11, 0xd1,
    0xff, 0x00, 0x08, 0xc1, 0x08, 0x8a, 0xe1, 0x00, 0x49, 0xd1, 0x0c, 0xcb, 0xf1, 0x84, 0x28, 0xc9,
    0x8a, 0xaa, 0xe9, 0x82, 0x69, 0xd9, 0x8e, 0xeb, 0xf9, 0x46, 0x18, 0xc5, 0x49, 0x9a, 0xe5, 0x41,
    0x59, 0xd5, 0x4d, 0xdb, 0xf5, 0xc5, 0x38, 0xcd, 0xcb, 0xba, 0xed, 0xc3, 0x79, 0xdd, 0xcf, 0xfb,
    0xfd, 0x07, 0x41, 0x4e, 0x4d, 0x46, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x07, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x56, 0x50, 0x38, 0x4c, 0x0f, 0x00,
    0x00, 0x00, 0x2f, 0x07, 0xc0, 0x01, 0x00, 0x07, 0x10, 0xfd, 0x8f, 0xfe, 0x07, 0x22, 0xa2, 0xff,
    0x01, 0x00,
};

template <size_t N> static std::vector<u8> vec(const unsigned char (&a)[N]) { return std::vector<u8>(a, a + N); }

static const u8* px(const ImageData& img, u32 x, u32 y) { return &img.pixels[(usize(y) * img.width + x) * 4]; }

static bool near8(const u8* p, int r, int g, int b, int a, int tol) {
    return std::abs(int(p[0]) - r) <= tol && std::abs(int(p[1]) - g) <= tol && std::abs(int(p[2]) - b) <= tol &&
           std::abs(int(p[3]) - a) <= tol;
}

// ---- hostile-input driver -----------------------------------------------------------------------------
// Every prefix of `bytes`, then every single-bit flip. A decode may fail or succeed; what it may not do is
// crash, hang, or report success with an image that does not add up.
static void fuzz(const char* name, const std::vector<u8>& bytes, const ImageDecodeOptions& opt = {}) {
    int ok = 0, refused = 0, bad = 0;
    const auto run = [&](const std::vector<u8>& b) {
        ImageData img;
        std::string err;
        if (decodeImage(b.data(), b.size(), img, opt, &err)) {
            ++ok;
            if (!img.valid() || img.width > kMaxImageDimension || img.height > kMaxImageDimension) ++bad;
        } else {
            ++refused;
            if (err.empty()) ++bad;
        }
    };
    for (usize n = 1; n < bytes.size(); ++n) run(std::vector<u8>(bytes.begin(), bytes.begin() + std::ptrdiff_t(n)));
    for (usize i = 0; i < bytes.size(); ++i)
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<u8> m = bytes;
            m[i] = u8(m[i] ^ (1u << bit));
            run(m);
        }
    check(bad == 0, std::string(name) + ": " + std::to_string(bytes.size() * 9) + " truncated / bit-flipped copies, " +
                        std::to_string(refused) + " refused, " + std::to_string(ok) + " decoded, none inconsistent");
}

// ======================================================================================================
// EXR builder
// ======================================================================================================
struct ExrSpec {
    u32 w = 0, h = 0;
    std::vector<std::string> names;                 // sorted, e.g. {"A","B","G","R"}
    std::vector<std::vector<f32>> planes;           // one w*h plane per name, top row first
    bool half = false;
    int compression = 0;                            // 0 none, 2 ZIPS
    bool tiled = false;
    u32 tile = 2;
    i32 minX = 0, minY = 0;
    i32 declaredMaxX = -1, declaredMaxY = -1;       // override the data window (hostile headers); -1 = honest
};

static void putAttr(std::vector<u8>& f, const char* name, const char* type, const std::vector<u8>& value) {
    putStr(f, name); f.push_back(0);
    putStr(f, type); f.push_back(0);
    put32(f, u32(value.size()));
    f.insert(f.end(), value.begin(), value.end());
}

static void putSample(std::vector<u8>& out, f32 v, bool half) {
    if (half) put16(out, fmt::floatToHalf(v));
    else putF32(out, v);
}

static u32 adler32(const std::vector<u8>& d) {
    u32 a = 1, b = 0;
    for (u8 c : d) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

// EXR ZIP: bytes reordered (even indices first, then odd), delta coded, then deflated. Stored blocks are valid
// deflate and need no compressor.
static std::vector<u8> zipsPack(const std::vector<u8>& raw) {
    const usize n = raw.size();
    std::vector<u8> t(n);
    for (usize i = 0; i < n; ++i) t[(i & 1) ? (n + 1) / 2 + i / 2 : i / 2] = raw[i];
    std::vector<u8> p = t;
    for (usize i = 1; i < n; ++i) p[i] = u8(int(t[i]) - int(t[i - 1]) + 128);
    std::vector<u8> z = {0x78, 0x01, 0x01};
    put16(z, u32(n));
    put16(z, u32(~u32(n)));
    z.insert(z.end(), p.begin(), p.end());
    const u32 ad = adler32(p);
    z.push_back(u8(ad >> 24)); z.push_back(u8(ad >> 16)); z.push_back(u8(ad >> 8)); z.push_back(u8(ad));
    return z;
}

static std::vector<u8> buildExr(const ExrSpec& s) {
    std::vector<u8> f = {0x76, 0x2f, 0x31, 0x01, 2, u8(s.tiled ? 0x02 : 0), 0, 0};
    std::vector<u8> ch;
    for (const std::string& n : s.names) {
        putStr(ch, n.c_str()); ch.push_back(0);
        put32(ch, s.half ? 1u : 2u);
        ch.push_back(0); ch.push_back(0); ch.push_back(0); ch.push_back(0);
        put32(ch, 1); put32(ch, 1);
    }
    ch.push_back(0);
    putAttr(f, "channels", "chlist", ch);
    putAttr(f, "compression", "compression", {u8(s.compression)});
    std::vector<u8> box;
    const i32 maxX = s.declaredMaxX != -1 ? s.declaredMaxX : s.minX + i32(s.w) - 1;
    const i32 maxY = s.declaredMaxY != -1 ? s.declaredMaxY : s.minY + i32(s.h) - 1;
    put32(box, u32(s.minX)); put32(box, u32(s.minY)); put32(box, u32(maxX)); put32(box, u32(maxY));
    putAttr(f, "dataWindow", "box2i", box);
    putAttr(f, "displayWindow", "box2i", box);
    putAttr(f, "lineOrder", "lineOrder", {0});
    std::vector<u8> one; putF32(one, 1.0f);
    putAttr(f, "pixelAspectRatio", "float", one);
    std::vector<u8> ctr; putF32(ctr, 0.0f); putF32(ctr, 0.0f);
    putAttr(f, "screenWindowCenter", "v2f", ctr);
    putAttr(f, "screenWindowWidth", "float", one);
    if (s.tiled) {
        std::vector<u8> td; put32(td, s.tile); put32(td, s.tile); td.push_back(0);
        putAttr(f, "tiles", "tiledesc", td);
    }
    f.push_back(0);   // end of header

    // Chunks
    std::vector<std::vector<u8>> chunks;
    const auto sampleRow = [&](std::vector<u8>& out, usize c, u32 y, u32 x0, u32 x1) {
        for (u32 x = x0; x < x1; ++x) putSample(out, s.planes[c][usize(y) * s.w + x], s.half);
    };
    if (!s.tiled) {
        for (u32 y = 0; y < s.h; ++y) {
            std::vector<u8> raw;
            for (usize c = 0; c < s.names.size(); ++c) sampleRow(raw, c, y, 0, s.w);
            const std::vector<u8> data = s.compression == 2 ? zipsPack(raw) : raw;
            std::vector<u8> chk;
            put32(chk, u32(s.minY + i32(y)));
            put32(chk, u32(data.size()));
            chk.insert(chk.end(), data.begin(), data.end());
            chunks.push_back(chk);
        }
    } else {
        const u32 tx = (s.w + s.tile - 1) / s.tile, ty = (s.h + s.tile - 1) / s.tile;
        for (u32 j = 0; j < ty; ++j)
            for (u32 i = 0; i < tx; ++i) {
                const u32 x0 = i * s.tile, x1 = std::min(s.w, x0 + s.tile);
                const u32 y0 = j * s.tile, y1 = std::min(s.h, y0 + s.tile);
                std::vector<u8> raw;
                for (u32 y = y0; y < y1; ++y)
                    for (usize c = 0; c < s.names.size(); ++c) sampleRow(raw, c, y, x0, x1);
                const std::vector<u8> data = s.compression == 2 ? zipsPack(raw) : raw;
                std::vector<u8> chk;
                put32(chk, i); put32(chk, j); put32(chk, 0); put32(chk, 0);
                put32(chk, u32(data.size()));
                chk.insert(chk.end(), data.begin(), data.end());
                chunks.push_back(chk);
            }
    }
    u64 off = f.size() + chunks.size() * 8;
    for (const auto& c : chunks) { put64(f, off); off += c.size(); }
    for (const auto& c : chunks) f.insert(f.end(), c.begin(), c.end());
    return f;
}

// A w*h RGBA(+) spec whose values are all exactly representable in half: r = x, g = y * 0.5, b = 4 (above 1),
// a = 0.25 + 0.25 * (x & 1), and a negative at (0,0) in R.
static ExrSpec rgbaSpec(u32 w, u32 h, bool withAlpha, bool half) {
    ExrSpec s;
    s.w = w; s.h = h; s.half = half;
    s.names = withAlpha ? std::vector<std::string>{"A", "B", "G", "R"} : std::vector<std::string>{"B", "G", "R"};
    for (size_t i = 0; i < s.names.size(); ++i) s.planes.emplace_back(usize(w) * h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const usize i = usize(y) * w + x;
            f32 r = f32(x), g = f32(y) * 0.5f, b = 4.0f, a = 0.25f + 0.25f * f32(x & 1);
            if (x == 0 && y == 0) r = -0.5f;
            if (withAlpha) { s.planes[0][i] = a; s.planes[1][i] = b; s.planes[2][i] = g; s.planes[3][i] = r; }
            else           { s.planes[0][i] = b; s.planes[1][i] = g; s.planes[2][i] = r; }
        }
    return s;
}

static void expectRgbaSpec(const FloatImageData& f, u32 w, u32 h, bool withAlpha, const std::string& what) {
    bool ok = f.valid() && f.width == w && f.height == h && f.channels == (withAlpha ? 4u : 3u);
    for (u32 y = 0; ok && y < h; ++y)
        for (u32 x = 0; ok && x < w; ++x) {
            const f32* p = &f.rgba[(usize(y) * w + x) * 4];
            const f32 r = (x == 0 && y == 0) ? -0.5f : f32(x);
            const f32 a = withAlpha ? 0.25f + 0.25f * f32(x & 1) : 1.0f;
            ok = p[0] == r && p[1] == f32(y) * 0.5f && p[2] == 4.0f && p[3] == a;
        }
    check(ok, what);
}

// ======================================================================================================
// SVG samples
// ======================================================================================================
static std::vector<u8> text(const std::string& s) { return std::vector<u8>(s.begin(), s.end()); }

static const char* kSvgBox =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"10\" viewBox=\"0 0 20 10\">"
    "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#ff0000\"/></svg>";

int main() {
    // ---------------------------------------------------------------------------------------------
    AVER_INFO("=== WebP: hand-built lossless streams ===");
    {
        // 4x2, green chooses between 40 and 220, red 200, blue 30, opaque.
        const std::vector<u8> bits = {0, 1, 0, 1, 1, 0, 0, 1};
        const std::vector<u8> file = vp8lFile(vp8lStream(4, 2, false, 40, 220, 200, 30, 255, bits));
        ImageData img; std::string err;
        check(decodeImage(file.data(), file.size(), img, &err), "a two-colour lossless stream decodes: " + err);
        check(img.valid() && img.width == 4 && img.height == 2 && img.channels == 3, "4x2, three channels (no alpha used)");
        bool pat = img.valid();
        for (u32 i = 0; pat && i < 8; ++i) {
            const u8* p = px(img, i % 4, i / 4);
            pat = near8(p, 200, bits[i] ? 220 : 40, 30, 255, 0);
        }
        check(pat, "every pixel has the colour its bit selects, alpha 255");
        const std::vector<u8> patternPixels = img.pixels;

        // A solid colour with alpha 128.
        const std::vector<u8> solid = vp8lFile(vp8lStream(3, 3, true, 10, -1, 20, 30, 128, {}));
        check(decodeImage(solid.data(), solid.size(), img, &err) && img.width == 3 && img.height == 3 && img.channels == 4, "a solid stream with alpha decodes");
        check(near8(px(img, 2, 2), 20, 10, 30, 128, 0), "and keeps its straight (not premultiplied) alpha");

        // Decoded through a path whose extension lies, and one that does not exist.
        const std::string dir = (std::filesystem::temp_directory_path() / "aver_imgdec_test").string();
        createDirectories(dir);
        const std::string lie = dir + "/looks_like.png";
        check(writeFileBytes(lie, file.data(), file.size()), "wrote the stream under a .png name");
        ImageData viaPath;
        check(decodeImage(lie, viaPath, &err) && viaPath.width == 4 && viaPath.height == 2 && viaPath.pixels == patternPixels,
              "the container is chosen by its bytes, not by the extension");
        check(!decodeImage(dir + "/missing.webp", viaPath, &err) && !err.empty(), "a missing file is refused with a reason");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("=== WebP: reference-encoder vectors ===");
    {
        ImageData img; std::string err;
        const std::vector<u8> lossless = vec(kLossless);
        check(decodeImage(lossless.data(), lossless.size(), img, &err) && img.width == 8 && img.height == 8 && img.channels == 3, "lossless 8x8 decodes: " + err);
        check(near8(px(img, 0, 0), 32, 40, 200, 255, 0) && near8(px(img, 7, 0), 200, 40, 130, 255, 0) &&
              near8(px(img, 3, 4), 104, 136, 170, 255, 0) && near8(px(img, 7, 7), 200, 208, 130, 255, 0),
              "lossless pixels are exact at four probes");

        const std::vector<u8> lossy = vec(kLossy);
        check(decodeImage(lossy.data(), lossy.size(), img, &err) && img.width == 8 && img.height == 8 && img.channels == 3, "lossy 8x8 decodes: " + err);
        check(near8(px(img, 0, 0), 35, 43, 186, 255, 3) && near8(px(img, 7, 0), 182, 50, 133, 255, 3) &&
              near8(px(img, 3, 4), 105, 136, 169, 255, 3) && near8(px(img, 7, 7), 197, 207, 144, 255, 3),
              "lossy pixels match the reference decode within 3");

        const std::vector<u8> la = vec(kLossyAlpha);
        check(decodeImage(la.data(), la.size(), img, &err) && img.width == 8 && img.channels == 4, "lossy + alpha (VP8X, ALPH, VP8) decodes: " + err);
        check(near8(px(img, 0, 0), 35, 43, 186, 40, 3) && near8(px(img, 3, 4), 105, 136, 169, 152, 3) && near8(px(img, 7, 7), 197, 207, 144, 236, 3),
              "colour and alpha match the reference decode");

        const std::vector<u8> ll = vec(kLosslessAlpha);
        check(decodeImage(ll.data(), ll.size(), img, &err) && img.channels == 4, "lossless + alpha decodes: " + err);
        check(near8(px(img, 0, 0), 32, 40, 200, 40, 0) && near8(px(img, 7, 0), 200, 40, 130, 40, 0) &&
              near8(px(img, 3, 4), 104, 136, 170, 152, 0) && near8(px(img, 7, 7), 200, 208, 130, 236, 0),
              "lossless colour and alpha are exact");

        const std::vector<u8> anim = vec(kAnimated);
        check(isWebpData(anim.data(), anim.size()), "an animated file is still recognised as WebP");
        check(decodeImage(anim.data(), anim.size(), img, &err) && img.width == 8 && img.height == 8, "an animated file decodes: " + err);
        check(near8(px(img, 0, 0), 32, 40, 200, 255, 0) && near8(px(img, 3, 4), 104, 136, 170, 255, 0),
              "and gives frame 0 (the ramp), not the red second frame");
    }

    AVER_INFO("=== WebP: animated frame placement ===");
    {
        // Canvas 8x6, the first frame 4x2 at (2,2), a second frame elsewhere that must be ignored.
        const std::vector<u8> bits = {0, 1, 0, 1, 1, 0, 0, 1};
        const std::vector<u8> frameData = chunk("VP8L", vp8lStream(4, 2, true, 40, 220, 200, 30, 255, bits));
        const std::vector<u8> otherData = chunk("VP8L", vp8lStream(4, 2, true, 1, -1, 2, 3, 255, {}));
        const auto anmf = [&](u32 x, u32 y, const std::vector<u8>& sub) {
            std::vector<u8> b;
            put24(b, x / 2); put24(b, y / 2); put24(b, 3); put24(b, 1); put24(b, 100); b.push_back(0);
            b.insert(b.end(), sub.begin(), sub.end());
            return chunk("ANMF", b);
        };
        std::vector<u8> vp8x; vp8x.push_back(0x12); put24(vp8x, 0); put24(vp8x, 7); put24(vp8x, 5);
        std::vector<u8> anim; for (int i = 0; i < 6; ++i) anim.push_back(0);
        std::vector<u8> body = chunk("VP8X", vp8x);
        const std::vector<u8> a = chunk("ANIM", anim), f0 = anmf(2, 2, frameData), f1 = anmf(0, 0, otherData);
        body.insert(body.end(), a.begin(), a.end());
        body.insert(body.end(), f0.begin(), f0.end());
        body.insert(body.end(), f1.begin(), f1.end());
        const std::vector<u8> file = riffWebp(body);
        ImageData img; std::string err;
        check(decodeImage(file.data(), file.size(), img, &err) && img.width == 8 && img.height == 6, "canvas size 8x6: " + err);
        check(img.valid() && near8(px(img, 0, 0), 0, 0, 0, 0, 0) && near8(px(img, 7, 5), 0, 0, 0, 0, 0) && near8(px(img, 1, 2), 0, 0, 0, 0, 0),
              "outside the frame is transparent");
        check(img.valid() && near8(px(img, 2, 2), 200, 40, 30, 255, 0) && near8(px(img, 3, 2), 200, 220, 30, 255, 0) && near8(px(img, 5, 3), 200, 220, 30, 255, 0),
              "the frame sits at its offset with its own pixels");

        // A frame that is lossy with an ALPH chunk, lifted from the vector above.
        const std::vector<u8> la = vec(kLossyAlpha);
        const std::vector<u8> sub(la.begin() + 30, la.end());   // after RIFF header (12) and the VP8X chunk (18)
        std::vector<u8> vx; vx.push_back(0x12); put24(vx, 0); put24(vx, 11); put24(vx, 7);
        // The offset field is x / 2: this frame sits at x = 2.
        std::vector<u8> fb; put24(fb, 1); put24(fb, 0); put24(fb, 7); put24(fb, 7); put24(fb, 100); fb.push_back(0);
        fb.insert(fb.end(), sub.begin(), sub.end());
        std::vector<u8> b2 = chunk("VP8X", vx);
        const std::vector<u8> c1 = chunk("ANIM", anim), c2 = chunk("ANMF", fb);
        b2.insert(b2.end(), c1.begin(), c1.end());
        b2.insert(b2.end(), c2.begin(), c2.end());
        const std::vector<u8> file2 = riffWebp(b2);
        check(decodeImage(file2.data(), file2.size(), img, &err) && img.width == 12 && img.height == 8, "a lossy+alpha frame on a wider canvas decodes: " + err);
        check(img.valid() && near8(px(img, 2 + 3, 4), 105, 136, 169, 152, 3) && near8(px(img, 2 + 7, 7), 197, 207, 144, 236, 3) &&
              near8(px(img, 0, 0), 0, 0, 0, 0, 0) && near8(px(img, 11, 7), 0, 0, 0, 0, 0),
              "its alpha comes through at the offset and the margins stay transparent");

        // A frame placed outside the canvas is refused.
        std::vector<u8> fbBad; put24(fbBad, 20); put24(fbBad, 0); put24(fbBad, 3); put24(fbBad, 1); put24(fbBad, 100); fbBad.push_back(0);
        fbBad.insert(fbBad.end(), frameData.begin(), frameData.end());
        std::vector<u8> b3 = chunk("VP8X", vp8x);
        const std::vector<u8> bad = chunk("ANMF", fbBad);
        b3.insert(b3.end(), a.begin(), a.end());
        b3.insert(b3.end(), bad.begin(), bad.end());
        const std::vector<u8> file3 = riffWebp(b3);
        check(!decodeImage(file3.data(), file3.size(), img, &err) && !err.empty(), "a first frame outside the canvas is refused");

        // An animation with no frames.
        std::vector<u8> b4 = chunk("VP8X", vp8x);
        b4.insert(b4.end(), a.begin(), a.end());
        const std::vector<u8> file4 = riffWebp(b4);
        check(!decodeImage(file4.data(), file4.size(), img, &err), "an animated file with no frame is refused");
    }

    AVER_INFO("=== WebP: hostile input ===");
    {
        ImageData img; std::string err;
        // A header that claims 16383 x 16383 (268 million pixels) with no pixel data behind it.
        std::vector<u8> huge = vp8lFile(vp8lStream(16383, 16383, false, 5, -1, 6, 7, 255, {}));
        check(!decodeImage(huge.data(), huge.size(), img, &err) && err.find("pixels") != std::string::npos && img.pixels.empty(),
              "a 16383 x 16383 claim is refused before anything is allocated: " + err);
        // VP8X canvas of 2^24 wide.
        std::vector<u8> vx; vx.push_back(0); put24(vx, 0); put24(vx, 0xFFFFFF); put24(vx, 0);
        std::vector<u8> canvas = chunk("VP8X", vx);
        const std::vector<u8> inner = chunk("VP8L", vp8lStream(1, 1, false, 1, -1, 2, 3, 255, {}));
        canvas.insert(canvas.end(), inner.begin(), inner.end());
        const std::vector<u8> bigCanvas = riffWebp(canvas);
        check(!decodeImage(bigCanvas.data(), bigCanvas.size(), img, &err) && img.pixels.empty(), "a 16.7 million pixel wide canvas is refused");
        // Not a WebP at all behind a RIFF header, and a RIFF with a giant chunk length.
        std::vector<u8> junk; putStr(junk, "RIFF"); put32(junk, 4); putStr(junk, "WEBP");
        check(!decodeImage(junk.data(), junk.size(), img, &err) && !err.empty(), "an empty WebP container is refused");
        std::vector<u8> lie = riffWebp(chunk("VP8L", {0x2f}));
        lie[16] = 0xff; lie[17] = 0xff; lie[18] = 0xff; lie[19] = 0x7f;
        check(!decodeImage(lie.data(), lie.size(), img, &err), "a chunk length past the end of the file is refused");

        fuzz("hand-built VP8L", vp8lFile(vp8lStream(4, 2, true, 40, 220, 200, 30, 128, {0, 1, 0, 1, 1, 0, 0, 1})));
        fuzz("lossless vector", vec(kLossless));
        fuzz("lossy vector", vec(kLossy));
        fuzz("lossy + alpha vector", vec(kLossyAlpha));
        fuzz("animated vector", vec(kAnimated));
    }

    // ---------------------------------------------------------------------------------------------
    AVER_INFO("=== OpenEXR ===");
    {
        FloatImageData f; std::string err;
        // Float, RGBA, uncompressed.
        std::vector<u8> a = buildExr(rgbaSpec(5, 3, true, false));
        check(isOpenExrData(a.data(), a.size()), "the magic is recognised");
        check(decodeFloatImage(a.data(), a.size(), f, &err), "float RGBA decodes: " + err);
        expectRgbaSpec(f, 5, 3, true, "float RGBA: values (including > 1 and negative) and layout are exact");

        // Half, RGB.
        std::vector<u8> b = buildExr(rgbaSpec(6, 4, false, true));
        check(decodeFloatImage(b.data(), b.size(), f, &err), "half RGB decodes: " + err);
        expectRgbaSpec(f, 6, 4, false, "half RGB: exact, alpha defaults to 1, three channels reported");

        // Stored-deflate ZIPS.
        ExrSpec z = rgbaSpec(7, 5, true, true);
        z.compression = 2;
        std::vector<u8> zb = buildExr(z);
        check(decodeFloatImage(zb.data(), zb.size(), f, &err), "ZIPS-compressed half RGBA decodes: " + err);
        expectRgbaSpec(f, 7, 5, true, "ZIPS: same pixels as the uncompressed file");
        ExrSpec zf = rgbaSpec(9, 2, false, false);
        zf.compression = 2;
        std::vector<u8> zfb = buildExr(zf);
        check(decodeFloatImage(zfb.data(), zfb.size(), f, &err), "ZIPS-compressed float RGB decodes: " + err);
        expectRgbaSpec(f, 9, 2, false, "ZIPS float: same pixels");

        // Tiled, with partial edge tiles (5x3 in 2x2 tiles).
        ExrSpec t = rgbaSpec(5, 3, true, false);
        t.tiled = true; t.tile = 2;
        std::vector<u8> tb = buildExr(t);
        check(decodeFloatImage(tb.data(), tb.size(), f, &err), "a tiled file decodes: " + err);
        expectRgbaSpec(f, 5, 3, true, "tiled with partial edge tiles: same pixels");
        t.compression = 2; t.half = true;
        tb = buildExr(t);
        check(decodeFloatImage(tb.data(), tb.size(), f, &err), "a tiled ZIPS half file decodes: " + err);
        expectRgbaSpec(f, 5, 3, true, "tiled + ZIPS + half: same pixels");

        // Grey (Y) and Y+A.
        ExrSpec g;
        g.w = 3; g.h = 2; g.names = {"Y"};
        g.planes = {{0.25f, 0.5f, 2.0f, 1.0f, 0.0f, 8.0f}};
        std::vector<u8> gb = buildExr(g);
        check(decodeFloatImage(gb.data(), gb.size(), f, &err) && f.channels == 1 && f.rgba[2 * 4] == 2.0f && f.rgba[2 * 4 + 1] == 2.0f && f.rgba[2 * 4 + 2] == 2.0f && f.rgba[2 * 4 + 3] == 1.0f &&
              f.rgba[5 * 4] == 8.0f, "a Y channel fills R, G and B");
        g.names = {"A", "Y"};
        g.planes = {{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}, {0.25f, 0.5f, 2.0f, 1.0f, 0.0f, 8.0f}};
        gb = buildExr(g);
        check(decodeFloatImage(gb.data(), gb.size(), f, &err) && f.channels == 2 && f.rgba[3] == 0.5f && f.rgba[4 * 4] == 0.0f, "Y + A keeps the alpha");

        // A data window that does not start at the origin.
        ExrSpec o = rgbaSpec(4, 3, true, false);
        o.minX = 10; o.minY = -5;
        std::vector<u8> ob = buildExr(o);
        check(decodeFloatImage(ob.data(), ob.size(), f, &err), "an offset data window decodes: " + err);
        expectRgbaSpec(f, 4, 3, true, "an offset data window gives the same pixels");

        // Not a float container.
        const std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        check(!decodeFloatImage(png.data(), png.size(), f, &err) && !err.empty(), "decodeFloatImage refuses other containers");
        check(!decodeFloatImage(nullptr, 0, f, &err), "and an empty buffer");

        // The 8-bit form.
        ImageData img;
        ExrSpec e;
        e.w = 6; e.h = 1; e.names = {"A", "B", "G", "R"};
        e.planes = {{1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f},     // A
                    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},     // B
                    {0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f},     // G
                    {1.0f, 0.0f, 4.0f, -2.0f, 0.5f, std::nanf("")}};   // R
        const std::vector<u8> eb = buildExr(e);
        check(decodeImage(eb.data(), eb.size(), img, &err) && img.width == 6 && img.height == 1 && img.channels == 4, "decodeImage reads an EXR as 8-bit RGBA: " + err);
        check(near8(px(img, 0, 0), 255, 0, 0, 255, 0), "1.0 is 255");
        check(near8(px(img, 1, 0), 0, 186, 0, 128, 1), "0.5 colour is 186 on the 1/2.2 curve and 0.5 alpha is 128");
        check(near8(px(img, 2, 0), 255, 0, 0, 255, 0), "4.0 clips to 255");
        check(near8(px(img, 3, 0), 0, 0, 0, 255, 0), "a negative clips to 0");
        check(near8(px(img, 4, 0), 186, 0, 0, 255, 1), "0.5 red is 186");
        check(near8(px(img, 5, 0), 0, 0, 0, 255, 0), "NaN becomes 0");

        // Through a path, with a misleading extension; and as a panorama (kept as float).
        const std::string dir = (std::filesystem::temp_directory_path() / "aver_imgdec_exr").string();
        createDirectories(dir);
        const std::string p = dir + "/sky.exr";
        check(writeFileBytes(p, a.data(), a.size()), "wrote an EXR");
        FloatImageData viaPath;
        check(decodeFloatImage(p, viaPath, &err) && viaPath.width == 5, "decodeFloatImage reads a path");
        fmt::PanoramaImage pano;
        check(fmt::loadPanorama(p, pano, &err) && pano.width == 5 && pano.height == 3 && pano.rgb.size() == 5 * 3 * 3, "loadPanorama accepts .exr now: " + err);
        check(pano.valid() && pano.rgb[(0 * 5 + 3) * 3 + 2] == 4.0f && pano.rgb[(2 * 5 + 4) * 3 + 0] == 4.0f && pano.rgb[(2 * 5 + 4) * 3 + 1] == 1.0f,
              "and the panorama keeps values above 1 (4.0 stays 4.0)");
        check(!decodeFloatImage(dir + "/missing.exr", viaPath, &err), "a missing EXR is refused");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("=== OpenEXR: hostile input ===");
    {
        ImageData img; FloatImageData f; std::string err;
        ExrSpec huge = rgbaSpec(2, 2, true, false);
        huge.declaredMaxX = 99999; huge.declaredMaxY = 99999;
        std::vector<u8> hb = buildExr(huge);
        check(!decodeFloatImage(hb.data(), hb.size(), f, &err) && err.find("larger") != std::string::npos && f.rgba.empty(), "a 100000 x 100000 data window is refused before allocating: " + err);
        huge.declaredMaxX = 8191; huge.declaredMaxY = 8191;   // 67 million pixels: inside each side's limit, over the pixel budget
        hb = buildExr(huge);
        check(!decodeFloatImage(hb.data(), hb.size(), f, &err) && err.find("pixels") != std::string::npos, "8192 x 8192 is over the float pixel budget: " + err);
        huge.declaredMaxX = 16000; huge.declaredMaxY = 0;
        hb = buildExr(huge);
        check(!decodeImage(hb.data(), hb.size(), img, &err), "a wide data window with no pixels behind it fails (truncated), not crashes");
        ExrSpec inverted = rgbaSpec(2, 2, true, false);
        inverted.declaredMaxX = -50; inverted.declaredMaxY = -50;
        hb = buildExr(inverted);
        check(!decodeFloatImage(hb.data(), hb.size(), f, &err), "an inverted data window is refused");
        ExrSpec nocol;
        nocol.w = 2; nocol.h = 2; nocol.names = {"Z"}; nocol.planes = {{1, 2, 3, 4}};
        hb = buildExr(nocol);
        check(!decodeFloatImage(hb.data(), hb.size(), f, &err) && err.find("channel") != std::string::npos, "a file with no colour channel is refused: " + err);

        fuzz("EXR float RGBA", buildExr(rgbaSpec(3, 2, true, false)));
        fuzz("EXR half RGB ZIPS", [] { ExrSpec s = rgbaSpec(3, 2, false, true); s.compression = 2; return buildExr(s); }());
        fuzz("EXR tiled", [] { ExrSpec s = rgbaSpec(3, 3, true, false); s.tiled = true; return buildExr(s); }());
    }

    // ---------------------------------------------------------------------------------------------
    AVER_INFO("=== SVG ===");
    {
        ImageData img; std::string err;
        const std::vector<u8> svg = text(kSvgBox);
        check(isSvgData(svg.data(), svg.size()), "an <svg> document is recognised");
        check(decodeImage(svg.data(), svg.size(), img, &err) && img.width == 20 && img.height == 10 && img.channels == 4, "own size is the document's width and height: " + err);
        check(near8(px(img, 5, 5), 255, 0, 0, 255, 0) && px(img, 15, 5)[3] == 0, "the red square is opaque inside and the rest transparent");

        ImageDecodeOptions o;
        o.svgWidth = 40;
        check(decodeImage(svg.data(), svg.size(), img, o, &err) && img.width == 40 && img.height == 20, "one side requested keeps the aspect ratio: " + err);
        check(near8(px(img, 10, 10), 255, 0, 0, 255, 0) && px(img, 30, 10)[3] == 0, "and the drawing scales with it");
        o = {}; o.svgHeight = 30;
        check(decodeImage(svg.data(), svg.size(), img, o, &err) && img.width == 60 && img.height == 30, "height alone: 60 x 30");
        o = {}; o.svgWidth = 40; o.svgHeight = 40;
        check(decodeImage(svg.data(), svg.size(), img, o, &err) && img.width == 40 && img.height == 40, "both requested: exactly that size");
        check(near8(px(img, 10, 20), 255, 0, 0, 255, 0) && px(img, 10, 5)[3] == 0 && px(img, 30, 20)[3] == 0 && px(img, 10, 35)[3] == 0,
              "the drawing is fitted and centred (letterbox bands are transparent)");

        // viewBox only; and units.
        const std::vector<u8> vb = text("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 8\"><rect width=\"16\" height=\"8\" fill=\"#00ff00\"/></svg>");
        check(decodeImage(vb.data(), vb.size(), img, &err) && img.width == 16 && img.height == 8 && near8(px(img, 8, 4), 0, 255, 0, 255, 0), "a viewBox alone gives the size: " + err);
        const std::vector<u8> inch = text("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1in\" height=\"0.5in\"><rect width=\"100\" height=\"50\" fill=\"blue\"/></svg>");
        check(decodeImage(inch.data(), inch.size(), img, &err) && img.width == 96 && img.height == 48, "inches are 96 dpi pixels (96 x 48): " + err);

        // Prologue before the root element, and a byte order mark.
        std::vector<u8> pro = text("<?xml version=\"1.0\"?>\n<!-- made by hand -->\n<!DOCTYPE svg>\n");
        pro.insert(pro.begin(), {0xEF, 0xBB, 0xBF});
        pro.insert(pro.end(), svg.begin(), svg.end());
        check(isSvgData(pro.data(), pro.size()) && decodeImage(pro.data(), pro.size(), img, &err) && img.width == 20, "a BOM, XML declaration, comment and doctype before <svg> are fine");

        // Not SVG.
        const std::vector<u8> html = text("<html><body>hello</body></html>");
        check(!isSvgData(html.data(), html.size()) && !decodeImage(html.data(), html.size(), img, &err), "an HTML page is not an SVG");
        check(!decodeImage(text("").data(), 0, img, &err), "an empty buffer is refused");

        // File on disk: by extension and by content.
        const std::string dir = (std::filesystem::temp_directory_path() / "aver_imgdec_svg").string();
        createDirectories(dir);
        check(writeFileBytes(dir + "/icon.svg", svg.data(), svg.size()), "wrote icon.svg");
        ImageData viaPath;
        check(decodeImage(dir + "/icon.svg", viaPath, &err) && viaPath.width == 20, "a path to an .svg decodes");
        o = {}; o.svgWidth = 100;
        check(decodeImage(dir + "/icon.svg", viaPath, o, &err) && viaPath.width == 100 && viaPath.height == 50, "a path with a requested width decodes at that width");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("=== SVG: hostile input ===");
    {
        ImageData img; std::string err;
        const auto refused = [&](const std::string& doc, const char* what) {
            const std::vector<u8> d = text(doc);
            const bool r = !decodeImage(d.data(), d.size(), img, &err) && !err.empty();
            check(r, what);
        };
        refused("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1e30\" height=\"1e30\"><rect width=\"5\" height=\"5\"/></svg>", "a width of 1e30 is refused");
        refused("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100000\" height=\"100000\"><rect width=\"5\" height=\"5\"/></svg>", "100000 x 100000 is refused");
        refused("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16000\" height=\"16000\"><rect width=\"5\" height=\"5\"/></svg>", "16000 x 16000 is over the pixel budget");
        refused("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"-5\" height=\"10\"></svg>", "a negative width is refused");
        refused("<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>", "an empty drawing with no size is refused");
        {
            const std::vector<u8> d = text(kSvgBox);
            ImageDecodeOptions o; o.svgWidth = 200000;
            check(!decodeImage(d.data(), d.size(), img, o, &err) && !err.empty(), "a requested width of 200000 is refused");
            o = {}; o.svgWidth = 16384; o.svgHeight = 16384;
            check(!decodeImage(d.data(), d.size(), img, o, &err), "a requested 16384 x 16384 is over the pixel budget");
        }
        {
            std::string deep = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">";
            for (int i = 0; i < 20000; ++i) deep += "<g>";
            deep += "<rect width=\"8\" height=\"8\"/>";
            const std::vector<u8> d = text(deep);
            const bool r = decodeImage(d.data(), d.size(), img, &err);
            check(true, std::string("20000 nested groups, unclosed: handled (") + (r ? "decoded" : "refused") + ")");
        }
        {
            std::string many = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\"><path d=\"M0 0";
            for (int i = 0; i < 100000; ++i) many += " L" + std::to_string(i % 64) + " " + std::to_string((i * 7) % 64);
            many += " Z\" fill=\"red\"/></svg>";
            const std::vector<u8> d = text(many);
            check(decodeImage(d.data(), d.size(), img, &err) && img.width == 64, "a 100000-segment path rasterises");
        }
        {
            std::vector<u8> d = text(kSvgBox);
            d.insert(d.begin() + 30, 0);
            check(!decodeImage(d.data(), d.size(), img, &err), "a NUL byte inside the document is refused");
            std::vector<u8> big(kMaxSvgBytes + 1, ' ');
            std::memcpy(big.data(), "<svg ", 5);
            check(!decodeImage(big.data(), big.size(), img, &err), "a document over the size limit is refused");
        }
        fuzz("SVG", text(kSvgBox));
        ImageDecodeOptions req; req.svgWidth = 24; req.svgHeight = 12;
        fuzz("SVG at a requested size", text(kSvgBox), req);
    }

    // ---------------------------------------------------------------------------------------------
    AVER_INFO("=== what did not move ===");
    {
        ImageData img; std::string err;
        // A 2x1 24-bit BMP, bottom-up: pixels (red) and (blue), stored as BGR with 2 bytes of row padding.
        std::vector<u8> bmp = {'B', 'M'};
        put32(bmp, 14 + 40 + 8); put32(bmp, 0); put32(bmp, 14 + 40);
        put32(bmp, 40); put32(bmp, 2); put32(bmp, 1); put16(bmp, 1); put16(bmp, 24);
        put32(bmp, 0); put32(bmp, 8); put32(bmp, 2835); put32(bmp, 2835); put32(bmp, 0); put32(bmp, 0);
        bmp.insert(bmp.end(), {0, 0, 255, 255, 0, 0, 0, 0});
        check(!isWebpData(bmp.data(), bmp.size()) && !isOpenExrData(bmp.data(), bmp.size()) && !isSvgData(bmp.data(), bmp.size()) && imageExtensionFor(bmp.data(), bmp.size()) == nullptr,
              "a BMP is none of the three");
        check(decodeImage(bmp.data(), bmp.size(), img, &err) && img.width == 2 && img.height == 1 && img.channels == 3, "a BMP still decodes through stb_image: " + err);
        check(near8(px(img, 0, 0), 255, 0, 0, 255, 0) && near8(px(img, 1, 0), 0, 0, 255, 255, 0), "with its pixels intact");
        const std::string dir = (std::filesystem::temp_directory_path() / "aver_imgdec_bmp").string();
        createDirectories(dir);
        check(writeFileBytes(dir + "/t.bmp", bmp.data(), bmp.size()) && decodeImage(dir + "/t.bmp", img, &err) && img.width == 2, "and from a path");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);

        // The refusal list: WebP and OpenEXR left it, the rest stayed.
        const std::vector<u8> webp = vec(kLossless);
        check(undecodableContainer(webp) == nullptr, "undecodableContainer no longer names WebP");
        const std::vector<u8> exr = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};
        check(undecodableContainer(exr) == nullptr, "nor OpenEXR");
        const std::vector<u8> tiff = {'I', 'I', '*', 0, 8, 0, 0, 0};
        const std::vector<u8> dds = {'D', 'D', 'S', ' ', 124, 0, 0, 0};
        const std::vector<u8> ktx = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB};
        std::vector<u8> avif(4, 0); putStr(avif, "ftypavif");
        check(std::string(undecodableContainer(tiff) ? undecodableContainer(tiff) : "") == "TIFF" &&
              std::string(undecodableContainer(dds) ? undecodableContainer(dds) : "") == "DDS" &&
              std::string(undecodableContainer(ktx) ? undecodableContainer(ktx) : "") == "KTX" &&
              std::string(undecodableContainer(avif) ? undecodableContainer(avif) : "") == "AVIF",
              "TIFF, DDS, KTX and AVIF are still refused by name");
        check(!decodeImage(tiff.data(), tiff.size(), img, &err) && !decodeImage(dds.data(), dds.size(), img, &err) &&
              !decodeImage(avif.data(), avif.size(), img, &err), "and still not decoded");

        check(std::string(imageExtensionFor(webp.data(), webp.size())) == ".webp" && std::string(imageExtensionFor(exr.data(), exr.size())) == ".exr" &&
              std::string(imageExtensionFor(reinterpret_cast<const u8*>(kSvgBox), std::strlen(kSvgBox))) == ".svg",
              "imageExtensionFor names the three");
    }

    if (g_failures == 0) AVER_INFO("=== all {} image decode checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
