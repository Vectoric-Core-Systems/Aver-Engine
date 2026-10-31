// WebP decoding through the vendored libwebp (third_party/libwebp, decoder only). See Image.hpp.
//
// libwebp's simple decoder refuses an animated file, and the demux library is not vendored, so an
// animated file's first ANMF chunk is lifted out and wrapped in a still-image container of its own
// (RIFF + VP8X + the frame's ALPH/VP8/VP8L chunks) before it is handed over. The frame is then placed on
// a transparent canvas of the file's canvas size, which is what a viewer shows for frame 0.
#include "ImageCodecs.hpp"

#include "src/webp/decode.h"

#include <cstring>
#include <new>

namespace aver::detail {

namespace {

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

const char* statusText(VP8StatusCode s) {
    switch (s) {
        case VP8_STATUS_OUT_OF_MEMORY:        return "out of memory";
        case VP8_STATUS_INVALID_PARAM:        return "invalid parameter";
        case VP8_STATUS_BITSTREAM_ERROR:      return "corrupt bitstream";
        case VP8_STATUS_UNSUPPORTED_FEATURE:  return "unsupported feature";
        case VP8_STATUS_NOT_ENOUGH_DATA:      return "the file is cut short";
        default:                              return "decode failed";
    }
}

u32 le24(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16); }
u32 le32(const u8* p) { return le24(p) | (u32(p[3]) << 24); }
void put24(std::vector<u8>& v, u32 x) { v.push_back(u8(x)); v.push_back(u8(x >> 8)); v.push_back(u8(x >> 16)); }
void put32(std::vector<u8>& v, u32 x) { put24(v, x); v.push_back(u8(x >> 24)); }
void putTag(std::vector<u8>& v, const char* t) { for (int i = 0; i < 4; ++i) v.push_back(u8(t[i])); }

bool sizeOk(u64 w, u64 h, std::string* err) {
    if (w == 0 || h == 0) return fail(err, "the image has no size");
    if (w > kMaxImageDimension || h > kMaxImageDimension) return fail(err, "the image is larger than the decoder allows");
    if (w * h > kMaxImagePixels) return fail(err, "the image has more pixels than the decoder allows");
    return true;
}

// Decodes one still image (any WebP without animation) into `px`, which holds w*h RGBA bytes.
bool decodeStill(const u8* data, usize size, u8* px, u32 w, u32 h, std::string* err) {
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) return fail(err, "libwebp version mismatch");
    config.output.colorspace = MODE_RGBA;   // straight (not premultiplied) alpha
    config.output.is_external_memory = 1;
    config.output.width = int(w);
    config.output.height = int(h);
    config.output.u.RGBA.rgba = px;
    config.output.u.RGBA.stride = int(w * 4);
    config.output.u.RGBA.size = usize(w) * h * 4;
    const VP8StatusCode st = WebPDecode(data, size, &config);
    WebPFreeDecBuffer(&config.output);
    if (st != VP8_STATUS_OK) return fail(err, std::string("WebP: ") + statusText(st));
    return true;
}

// The first ANMF frame of an animated file as a standalone WebP, with its offset and size.
struct FirstFrame {
    std::vector<u8> webp;
    u32 x = 0, y = 0, w = 0, h = 0;
};

bool firstFrame(const u8* data, usize size, FirstFrame& out, std::string* err) {
    usize pos = 12;   // past "RIFF" size "WEBP"
    while (pos + 8 <= size) {
        const u32 len = le32(data + pos + 4);
        const usize body = pos + 8;
        if (len > size - body) break;
        if (std::memcmp(data + pos, "ANMF", 4) == 0) {
            if (len < 16 + 8) return fail(err, "WebP: an animation frame is cut short");
            out.x = le24(data + body) * 2;
            out.y = le24(data + body + 3) * 2;
            out.w = le24(data + body + 6) + 1;
            out.h = le24(data + body + 9) + 1;
            if (!sizeOk(out.w, out.h, err)) return false;
            const u8* sub = data + body + 16;
            const usize subLen = len - 16;
            // VP8X (flags: alpha), then the frame's own chunks verbatim.
            std::vector<u8>& v = out.webp;
            putTag(v, "RIFF");
            put32(v, u32(4 + 18 + subLen));
            putTag(v, "WEBP");
            putTag(v, "VP8X");
            put32(v, 10);
            v.push_back(0x10);   // alpha present; harmless when the frame has none
            put24(v, 0);
            put24(v, out.w - 1);
            put24(v, out.h - 1);
            v.insert(v.end(), sub, sub + subLen);
            return true;
        }
        pos = body + len + (len & 1);
    }
    return fail(err, "WebP: an animated file with no frames");
}

} // namespace

bool decodeWebp(const u8* data, usize size, ImageData& out, std::string* err) {
    out = ImageData{};
    try {
        WebPBitstreamFeatures feat;
        const VP8StatusCode fs = WebPGetFeatures(data, size, &feat);
        if (fs != VP8_STATUS_OK) return fail(err, std::string("WebP: ") + statusText(fs));
        if (feat.width <= 0 || feat.height <= 0) return fail(err, "WebP: the image has no size");
        if (!sizeOk(u64(feat.width), u64(feat.height), err)) return false;
        const u32 cw = u32(feat.width), ch = u32(feat.height);

        if (!feat.has_animation) {
            out.pixels.assign(usize(cw) * ch * 4, 0);
            if (!decodeStill(data, size, out.pixels.data(), cw, ch, err)) { out = ImageData{}; return false; }
            out.width = cw;
            out.height = ch;
            out.channels = feat.has_alpha ? 4 : 3;
            return true;
        }

        FirstFrame fr;
        if (!firstFrame(data, size, fr, err)) return false;
        if (u64(fr.x) + fr.w > cw || u64(fr.y) + fr.h > ch) return fail(err, "WebP: the first frame lies outside the canvas");
        WebPBitstreamFeatures ff;
        if (WebPGetFeatures(fr.webp.data(), fr.webp.size(), &ff) != VP8_STATUS_OK || ff.has_animation ||
            u32(ff.width) != fr.w || u32(ff.height) != fr.h)
            return fail(err, "WebP: the first frame is damaged");

        std::vector<u8> frame(usize(fr.w) * fr.h * 4, 0);
        if (!decodeStill(fr.webp.data(), fr.webp.size(), frame.data(), fr.w, fr.h, err)) return false;
        out.pixels.assign(usize(cw) * ch * 4, 0);
        for (u32 row = 0; row < fr.h; ++row)
            std::memcpy(out.pixels.data() + (usize(fr.y + row) * cw + fr.x) * 4, frame.data() + usize(row) * fr.w * 4, usize(fr.w) * 4);
        out.width = cw;
        out.height = ch;
        out.channels = 4;
        return true;
    } catch (const std::bad_alloc&) {
        out = ImageData{};
        return fail(err, "WebP: out of memory");
    }
}

} // namespace aver::detail
