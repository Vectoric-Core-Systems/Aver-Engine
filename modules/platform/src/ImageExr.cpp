// OpenEXR reading through the vendored tinyexr (third_party/tinyexr; the implementation is compiled in
// ThirdPartyTinyExr.cpp). See Image.hpp.
//
// The header is parsed and checked FIRST, so a file that claims an absurd data window is refused before
// tinyexr allocates anything for it. The pixels are assembled here from tinyexr's per-channel planes
// (rather than with its LoadEXRFromMemory convenience call, which leaks on its error paths and does not
// check its allocation).
#include "ImageCodecs.hpp"

#include "tinyexr.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <new>

namespace aver::detail {

namespace {

constexpr u64 kMaxExrAllocBytes = u64(1) << 30;   // what tinyexr may allocate for the samples of one image

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

// Owns the tinyexr header, image and error string for one decode.
struct ExrScope {
    EXRHeader header;
    EXRImage image;
    const char* msg = nullptr;
    bool headerLive = false;
    bool imageLive = false;
    ExrScope() {
        InitEXRHeader(&header);
        InitEXRImage(&image);
    }
    ~ExrScope() {
        if (imageLive) FreeEXRImage(&image);
        if (headerLive) FreeEXRHeader(&header);
        if (msg) FreeEXRErrorMessage(msg);
    }
    ExrScope(const ExrScope&) = delete;
    ExrScope& operator=(const ExrScope&) = delete;
    std::string why(const char* fallback) const { return msg ? std::string("OpenEXR: ") + msg : std::string("OpenEXR: ") + fallback; }
};

int findChannel(const EXRHeader& h, const char* name) {
    for (int c = 0; c < h.num_channels; ++c)
        if (std::strcmp(h.channels[c].name, name) == 0) return c;
    return -1;
}

u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
u64 le64(const u8* p) { return u64(le32(p)) | (u64(le32(p + 4)) << 32); }

// The offset of the first byte after the header's attribute list (where a chunk table starts), or 0 when
// the list does not end inside the file. Multipart files never get here.
usize headerEnd(const u8* d, usize n) {
    usize p = 8;   // magic, version, flags
    while (p < n) {
        if (d[p] == 0) return p + 1;                  // the empty name ends the list
        usize q = p;
        while (q < n && d[q] != 0) ++q;               // attribute name
        if (q >= n) return 0;
        ++q;
        while (q < n && d[q] != 0) ++q;               // attribute type
        if (q >= n) return 0;
        ++q;
        if (n - q < 4) return 0;
        const u32 len = le32(d + q);
        q += 4;
        if (len > n - q) return 0;
        p = q + len;
    }
    return 0;
}

// tinyexr trusts a tiled file's chunk table: it multiplies tile coordinates from the file as ints (signed
// overflow on a hostile file, which an optimising compiler is free to turn into an out-of-bounds write) and
// allocates every tile at full size without checking the result. So a tiled file is read only when its
// whole table is consistent: one resolution level, tile sizes in range, and every table entry pointing at
// a chunk that names its own tile.
bool tiledTableOk(const EXRHeader& h, const u8* d, usize n, u64 w, u64 ht, std::string& why) {
    if (h.tile_level_mode != TINYEXR_TILE_ONE_LEVEL) { why = "mipmapped and ripmapped tiled files are not read"; return false; }
    if (h.tile_size_x < 1 || h.tile_size_y < 1 || u32(h.tile_size_x) > kMaxImageDimension || u32(h.tile_size_y) > kMaxImageDimension) {
        why = "the tile size is outside what the decoder allows";
        return false;
    }
    const u64 nx = (w + u64(h.tile_size_x) - 1) / u64(h.tile_size_x);
    const u64 ny = (ht + u64(h.tile_size_y) - 1) / u64(h.tile_size_y);
    const usize table = headerEnd(d, n);
    if (table == 0 || (n - table) / 8 < nx * ny) { why = "the tile table is cut short"; return false; }
    for (u64 k = 0; k < nx * ny; ++k) {
        const u64 off = le64(d + table + usize(k) * 8);
        if (off > n || n - off < 20) { why = "a tile lies outside the file"; return false; }
        const u8* c = d + usize(off);
        if (le32(c) != u32(k % nx) || le32(c + 4) != u32(k / nx) || le32(c + 8) != 0 || le32(c + 12) != 0) {
            why = "the tile table disagrees with the tiles";
            return false;
        }
    }
    return true;
}

} // namespace

bool decodeExr(const u8* data, usize size, FloatImageData& out, std::string* err) {
    out = FloatImageData{};
    try {
        ExrScope s;
        EXRVersion version;
        if (ParseEXRVersionFromMemory(&version, data, size) != TINYEXR_SUCCESS) return fail(err, "OpenEXR: not an OpenEXR file");
        if (version.multipart || version.non_image) return fail(err, "OpenEXR: multipart and deep images are not read");

        if (ParseEXRHeaderFromMemory(&s.header, &version, data, size, &s.msg) != TINYEXR_SUCCESS) return fail(err, s.why("the header could not be read"));
        s.headerLive = true;

        const EXRHeader& h = s.header;
        const i64 w = i64(h.data_window.max_x) - i64(h.data_window.min_x) + 1;
        const i64 ht = i64(h.data_window.max_y) - i64(h.data_window.min_y) + 1;
        if (w <= 0 || ht <= 0) return fail(err, "OpenEXR: the data window is empty");
        if (w > kMaxImageDimension || ht > kMaxImageDimension) return fail(err, "OpenEXR: the image is larger than the decoder allows");
        if (u64(w) * u64(ht) > kMaxFloatImagePixels) return fail(err, "OpenEXR: the image has more pixels than the decoder allows");
        if (h.num_channels <= 0 || h.num_channels > 256) return fail(err, "OpenEXR: the channel list is unusable");

        const int r = findChannel(h, "R"), g = findChannel(h, "G"), b = findChannel(h, "B");
        const int a = findChannel(h, "A");
        const bool rgb = r >= 0 && g >= 0 && b >= 0;
        // One grey channel: Y, or a lone R (single-channel data files are written that way).
        const int y = findChannel(h, "Y") >= 0 ? findChannel(h, "Y") : (g < 0 && b < 0 ? r : -1);
        if (!rgb && y < 0) return fail(err, "OpenEXR: no R, G, B or Y channel");
        const int used[4] = {rgb ? r : y, rgb ? g : y, rgb ? b : y, a};
        for (int c : used) {
            if (c < 0) continue;
            if (h.channels[c].x_sampling != 1 || h.channels[c].y_sampling != 1) return fail(err, "OpenEXR: subsampled channels are not read");
            if (h.pixel_types[c] == TINYEXR_PIXELTYPE_UINT) return fail(err, "OpenEXR: integer colour channels are not read");
        }
        // tinyexr allocates every plane of the whole image (tiled: of every tile at full size) up front and does
        // not check the result, so the total is bounded here, counting 4 bytes a sample.
        u64 planeSamples = u64(w) * u64(ht);
        if (h.tiled) {
            std::string why;
            if (!tiledTableOk(h, data, size, u64(w), u64(ht), why)) return fail(err, "OpenEXR: " + why);
            const u64 tx = u64(h.tile_size_x), ty = u64(h.tile_size_y);
            planeSamples = ((u64(w) + tx - 1) / tx * tx) * ((u64(ht) + ty - 1) / ty * ty);
        }
        if (planeSamples * 4 * u64(h.num_channels) > kMaxExrAllocBytes) return fail(err, "OpenEXR: the image needs more memory than the decoder allows");

        // Half channels are widened to float by the decoder.
        for (int c = 0; c < h.num_channels; ++c)
            if (s.header.pixel_types[c] == TINYEXR_PIXELTYPE_HALF) s.header.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;

        if (LoadEXRImageFromMemory(&s.image, &s.header, data, size, &s.msg) != TINYEXR_SUCCESS) return fail(err, s.why("the pixel data could not be read"));
        s.imageLive = true;

        const usize uw = usize(w), uh = usize(ht);
        if (usize(s.image.width) != uw || usize(s.image.height) != uh) return fail(err, "OpenEXR: the decoded size disagrees with the header");
        out.rgba.assign(uw * uh * 4, 0.0f);
        for (usize i = 0; i < uw * uh; ++i) out.rgba[i * 4 + 3] = 1.0f;

        const auto channelPlane = [&](const float* const* planes, int c) -> const float* {
            return c >= 0 ? planes[c] : nullptr;
        };
        if (!s.header.tiled) {
            if (!s.image.images) return fail(err, "OpenEXR: no pixel data");
            const float* const* planes = reinterpret_cast<const float* const*>(s.image.images);
            for (int k = 0; k < 4; ++k) {
                const float* p = channelPlane(planes, used[k]);
                if (!p) continue;
                for (usize i = 0; i < uw * uh; ++i) out.rgba[i * 4 + usize(k)] = p[i];
            }
        } else {
            if (!s.image.tiles) return fail(err, "OpenEXR: no tile data");
            const usize tsx = usize(s.header.tile_size_x), tsy = usize(s.header.tile_size_y);
            if (tsx == 0 || tsy == 0) return fail(err, "OpenEXR: the tile size is empty");
            for (int t = 0; t < s.image.num_tiles; ++t) {
                const EXRTile& tile = s.image.tiles[t];
                if (tile.level_x != 0 || tile.level_y != 0 || !tile.images) continue;   // level 0 only
                if (tile.offset_x < 0 || tile.offset_y < 0) continue;
                const float* const* planes = reinterpret_cast<const float* const*>(tile.images);
                for (usize j = 0; j < tsy; ++j) {
                    const usize yy = usize(tile.offset_y) * tsy + j;
                    if (yy >= uh) break;
                    for (usize i = 0; i < tsx; ++i) {
                        const usize xx = usize(tile.offset_x) * tsx + i;
                        if (xx >= uw) break;
                        for (int k = 0; k < 4; ++k) {
                            const float* p = channelPlane(planes, used[k]);
                            if (p) out.rgba[(yy * uw + xx) * 4 + usize(k)] = p[i + j * tsx];
                        }
                    }
                }
            }
        }
        out.width = u32(w);
        out.height = u32(ht);
        out.channels = rgb ? (a >= 0 ? 4u : 3u) : (a >= 0 ? 2u : 1u);
        return true;
    } catch (const std::bad_alloc&) {
        out = FloatImageData{};
        return fail(err, "OpenEXR: out of memory");
    } catch (const std::exception&) {
        out = FloatImageData{};
        return fail(err, "OpenEXR: the file is damaged");
    }
}

} // namespace aver::detail
