// `.ocland` reader and writer. See OcLand.hpp for what the format holds and what it deliberately
// leaves out.
#include "aver/formats/OcLand.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace aver::fmt {
namespace {

// LHDR is written and read FIELD BY FIELD through a byte cursor, never as a memcpy of a struct. A
// struct written whole is a struct whose padding is part of the file format, and the day somebody adds
// a field in the middle every asset already on disk becomes silently wrong. Same reasoning as
// OcAudioInfo's, and it is worth repeating rather than cross-referencing.
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

template <typename T> void put(std::vector<u8>& b, T v) {
    const usize at = b.size();
    b.resize(at + sizeof(T));
    std::memcpy(b.data() + at, &v, sizeof(T));
}

bool fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}

// LHDR's layout, in one place so the reader and the writer cannot disagree about it:
//   u32 sampleCount
//   f32 spacingCm
//   f32 originCm[3]
//   f32 heightBiasCm      the height a quantised 0 decodes to
//   f32 heightScaleCm     the span a full-scale 65535 adds to the bias
constexpr usize kLhdrBytes = sizeof(u32) + sizeof(f32) * 6;

} // namespace

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

    // Validated BEFORE anything is sized off it. `n * n` on an attacker-supplied u32 overflows, and a
    // reader that reserved first and checked second would have already tried to allocate 16 exabytes.
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

    // Decode. `heightCm = origin.z + bias + q/65535 * scale`, so a consumer never meets a quantised
    // value. The divide is by 65535 and not 65536 because a full-scale 65535 must land exactly on the
    // maximum -- off by one there and the tallest sample in every file is a fraction low.
    const f32 inv = scale / 65535.0f;
    for (usize i = 0; i < count; ++i) {
        u16 q = 0;
        std::memcpy(&q, hh->data.data() + i * sizeof(u16), sizeof(u16));
        out.heights[i] = origin[2] + bias + static_cast<f32>(q) * inv;
    }

    // Bounds RECOMPUTED, never taken from the file. See the header.
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

bool loadOcLand(const std::string& path, OcLandData& out, std::string* why) {
    Avr1File probe;
    // Loaded through the container's own reader so the CRC and chunk-hash checks happen exactly once,
    // in the code that owns them, rather than being half-repeated here.
    if (!loadAvr1(path, probe, why)) return false;
    std::vector<u8> bytes;
    if (!writeAvr1(probe, bytes, why)) return false;
    return parseOcLand(bytes.data(), bytes.size(), out, why);
}

bool writeOcLand(const OcLandData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid()) return fail(why, "OcLandData is not internally consistent");
    if (in.sampleCount > kOcLandMaxSamples) return fail(why, "sampleCount above the maximum of 4097");

    // The quantisation range is the terrain's OWN relief, so the step is proportional to what the file
    // actually contains. Heights are stored relative to origin.z, matching what the reader adds back.
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
    // A FLAT section has zero relief, and dividing by that span would put every sample at infinity.
    // Encoding it as all-zero is exact: the reader adds bias and gets lo back for every sample.
    const f32 fwd = scale > 0.0f ? 65535.0f / scale : 0.0f;
    for (usize i = 0; i < in.heights.size(); ++i) {
        f32 t = (in.heights[i] - lo) * fwd;
        // Rounded, not truncated, and clamped: rounding halves the worst-case error, and floating
        // point can put the maximum a hair over 65535.
        if (t < 0.0f) t = 0.0f;
        if (t > 65535.0f) t = 65535.0f;
        const u16 q = static_cast<u16>(t + 0.5f);
        std::memcpy(hgt.data() + i * sizeof(u16), &q, sizeof(u16));
    }

    Avr1File file;
    file.subtype = kAvrSubtypeLand;
    // LHDR is the only Required chunk. Everything else this format may ever gain is additive, so an
    // older reader meeting a newer file skips what it does not know and still produces a surface.
    file.add(kOcLandChunkHeader, std::move(hdr), kAvrChunkRequired);
    // GpuUploadable so the container aligns the payload for a straight upload: a heightfield is the
    // one chunk here big enough for that to matter.
    file.add(kOcLandChunkHeights, std::move(hgt), kAvrChunkGpuUploadable);
    return writeAvr1(file, out, why);
}

bool saveOcLand(const std::string& path, const OcLandData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcLand(in, bytes, why)) return false;
    Avr1File file;
    if (!parseAvr1(bytes.data(), bytes.size(), file, why)) return false;
    return saveAvr1(path, file, why);
}

} // namespace aver::fmt
