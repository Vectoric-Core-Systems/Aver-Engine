// `.ocland` reader and writer: the LHDR/HGHT chunks of a landscape section in an AVR1 container.
#include "aver/formats/OcLand.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace aver::fmt {
namespace {

// Byte cursor over a chunk payload. Reads field by field; `ok` goes false on underrun.
struct Cursor {
    const u8* p = nullptr;
    usize left = 0;
    bool ok = true;

    template <typename T> T take() {
        T v{};
        if (left < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        left -= sizeof(T);
        return v;
    }
};

// Appends the raw bytes of `v` to `b`.
template <typename T> void put(std::vector<u8>& b, T v) {
    const usize at = b.size();
    b.resize(at + sizeof(T));
    std::memcpy(b.data() + at, &v, sizeof(T));
}

// Sets `why` and returns false.
bool fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}

// LHDR layout, which the reader and writer must agree on:
//   u32 sampleCount, f32 spacingCm, f32 originCm[3], f32 heightBiasCm, f32 heightScaleCm
constexpr usize kLhdrBytes = sizeof(u32) + sizeof(f32) * 6;

} // namespace

// Parses an .ocland from memory, dequantising the heights and recomputing the bounds.
bool parseOcLand(const u8* bytes, usize size, OcLandData& out, std::string* why) {
    Avr1File file;
    if (!parseAvr1(bytes, size, file, why)) return false;
    if (file.subtype != kAvrSubtypeLand) return fail(why, "not an .ocland container (wrong subtype)");

    const AvrChunk* hdr = file.find(kOcLandChunkHeader);
    if (!hdr) return fail(why, "no LHDR chunk");
    if (hdr->data.size() < kLhdrBytes) return fail(why, "LHDR is too short");

    Cursor c{hdr->data.data(), hdr->data.size()};
    const u32 n        = c.take<u32>();
    const f32 spacing  = c.take<f32>();
    f32 origin[3];
    origin[0] = c.take<f32>();
    origin[1] = c.take<f32>();
    origin[2] = c.take<f32>();
    const f32 bias     = c.take<f32>();
    const f32 scale    = c.take<f32>();
    if (!c.ok) return fail(why, "LHDR ended early");

    if (n < kOcLandMinSamples) return fail(why, "sampleCount below the minimum of 2");
    if (n > kOcLandMaxSamples) return fail(why, "sampleCount above the maximum of 4097");
    if (!(spacing > 0.0f) || !std::isfinite(spacing)) return fail(why, "spacingCm must be positive and finite");
    if (!std::isfinite(bias) || !std::isfinite(scale)) return fail(why, "height quantisation is not finite");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(origin[i])) return fail(why, "originCm is not finite");

    const AvrChunk* hh = file.find(kOcLandChunkHeights);
    if (!hh) return fail(why, "no HGHT chunk");

    const usize count = static_cast<usize>(n) * static_cast<usize>(n);
    if (hh->data.size() < count * sizeof(u16))
        return fail(why, "HGHT is smaller than sampleCount^2 -- the header and the payload disagree");

    out = OcLandData{};
    out.sampleCount = n;
    out.spacingCm = spacing;
    out.originCm[0] = origin[0];
    out.originCm[1] = origin[1];
    out.originCm[2] = origin[2];
    out.heights.resize(count);

    const f32 inv = scale / 65535.0f;
    for (usize i = 0; i < count; ++i) {
        u16 q = 0;
        std::memcpy(&q, hh->data.data() + i * sizeof(u16), sizeof(u16));
        out.heights[i] = origin[2] + bias + static_cast<f32>(q) * inv;
    }

    f32 lo = out.heights[0], hi = out.heights[0];
    for (f32 h : out.heights) { if (h < lo) lo = h; if (h > hi) hi = h; }
    out.boundsMin[0] = origin[0];
    out.boundsMin[1] = origin[1];
    out.boundsMin[2] = lo;
    out.boundsMax[0] = origin[0] + out.extentCm();
    out.boundsMax[1] = origin[1] + out.extentCm();
    out.boundsMax[2] = hi;
    return true;
}

// Loads an .ocland from disk.
bool loadOcLand(const std::string& path, OcLandData& out, std::string* why) {
    Avr1File probe;
    if (!loadAvr1(path, probe, why)) return false;
    std::vector<u8> bytes;
    if (!writeAvr1(probe, bytes, why)) return false;
    return parseOcLand(bytes.data(), bytes.size(), out, why);
}

// Encodes a section into AVR1 bytes, quantising the heights to u16 across their own range.
bool writeOcLand(const OcLandData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid()) return fail(why, "OcLandData is not internally consistent");
    if (in.sampleCount > kOcLandMaxSamples) return fail(why, "sampleCount above the maximum of 4097");

    f32 lo = in.heights[0], hi = in.heights[0];
    for (f32 h : in.heights) {
        if (!std::isfinite(h)) return fail(why, "a height is not finite");
        if (h < lo) lo = h;
        if (h > hi) hi = h;
    }
    const f32 bias  = lo - in.originCm[2];
    const f32 scale = hi - lo;

    std::vector<u8> hdr;
    hdr.reserve(kLhdrBytes);
    put<u32>(hdr, in.sampleCount);
    put<f32>(hdr, in.spacingCm);
    put<f32>(hdr, in.originCm[0]);
    put<f32>(hdr, in.originCm[1]);
    put<f32>(hdr, in.originCm[2]);
    put<f32>(hdr, bias);
    put<f32>(hdr, scale);

    std::vector<u8> hgt(in.heights.size() * sizeof(u16));
    const f32 fwd = scale > 0.0f ? 65535.0f / scale : 0.0f;
    for (usize i = 0; i < in.heights.size(); ++i) {
        f32 t = (in.heights[i] - lo) * fwd;
        if (t < 0.0f) t = 0.0f;
        if (t > 65535.0f) t = 65535.0f;
        const u16 q = static_cast<u16>(t + 0.5f);
        std::memcpy(hgt.data() + i * sizeof(u16), &q, sizeof(u16));
    }

    Avr1File file;
    file.subtype = kAvrSubtypeLand;
    file.add(kOcLandChunkHeader, std::move(hdr), kAvrChunkRequired);
    file.add(kOcLandChunkHeights, std::move(hgt), kAvrChunkGpuUploadable);
    return writeAvr1(file, out, why);
}

// Writes a section to disk.
bool saveOcLand(const std::string& path, const OcLandData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcLand(in, bytes, why)) return false;
    Avr1File file;
    if (!parseAvr1(bytes.data(), bytes.size(), file, why)) return false;
    return saveAvr1(path, file, why);
}

} // namespace aver::fmt
