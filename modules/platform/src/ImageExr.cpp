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
        const int a = findChannel(h, "A"), y = findChannel(h, "Y");
        const bool rgb = r >= 0 && g >= 0 && b >= 0;
        if (!rgb && y < 0) return fail(err, "OpenEXR: no R, G, B or Y channel");
        const int used[4] = {rgb ? r : y, rgb ? g : y, rgb ? b : y, a};
        for (int c : used) {
            if (c < 0) continue;
            if (h.channels[c].x_sampling != 1 || h.channels[c].y_sampling != 1) return fail(err, "OpenEXR: subsampled channels are not read");
            if (h.pixel_types[c] == TINYEXR_PIXELTYPE_UINT) return fail(err, "OpenEXR: integer colour channels are not read");
        }
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
