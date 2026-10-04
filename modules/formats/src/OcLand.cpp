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

    // Pin the AUTHORED quantisation range: the optional LRNG chunk if this file already carries one,
    // or -- a file saved before LRNG existed -- the range this file's own LHDR bias/scale already
    // encode. Either way, from this load onward writeOcLand reuses this range instead of rescanning
    // `heights`, which is the one-time migration ITEM 0.7 asks for: an old file's very next save gains
    // the LRNG chunk (the one change), and every save after that reuses the same pinned numbers.
    f32 quantMin = origin[2] + bias;
    f32 quantMax = quantMin + scale;
    if (const AvrChunk* rng = file.find(kOcLandChunkRange)) {
        if (rng->data.size() >= sizeof(f32) * 2) {
            Cursor rc{rng->data.data(), rng->data.size()};
            const f32 rmin = rc.take<f32>();
            const f32 rmax = rc.take<f32>();
            if (rc.ok && std::isfinite(rmin) && std::isfinite(rmax) && rmax >= rmin) {
                quantMin = rmin;
                quantMax = rmax;
            }
        }
    }
    out.quantMinCm = quantMin;
    out.quantMaxCm = quantMax;
    out.hasQuantRange = true;
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

// Encodes a section into AVR1 bytes, quantising the heights to u16 against the authored range.
bool writeOcLand(const OcLandData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid()) return fail(why, "OcLandData is not internally consistent");
    if (in.sampleCount > kOcLandMaxSamples) return fail(why, "sampleCount above the maximum of 4097");

    f32 liveLo = in.heights[0], liveHi = in.heights[0];
    for (f32 h : in.heights) {
        if (!std::isfinite(h)) return fail(why, "a height is not finite");
        if (h < liveLo) liveLo = h;
        if (h > liveHi) liveHi = h;
    }

    // ITEM 0.7's fix: quantise against the AUTHORED range, not the live extent of `in.heights`.
    // Deriving lo/hi fresh from the live heights on every save was the FIRST bug -- editing one
    // sample could move the whole section's range, which silently re-quantised every OTHER sample. A
    // struct that has never been loaded (hasQuantRange == false) has no prior range to protect, so it
    // still derives one fresh here, exactly as before; that is the migration/first-save case, not the
    // bug.
    //
    // Once a range IS pinned, an in-range live value reuses it exactly -- so an edit that stays inside
    // the range leaves it untouched, and every untouched sample's quantised code (and therefore its
    // decoded height) is bit-for-bit what it was. That is the first bug, still fixed.
    //
    // THE SECOND BUG: reusing the pinned range UNCONDITIONALLY and clamping anything outside it. Before
    // this fix existed, lo/hi always covered the live heights, so that clamp never fired; once the
    // range was pinned, it did -- sculpting terrain above the pinned ceiling was silently flattened on
    // save. Landscape sculpting is the one authoring mode that ships, and raising ground past the
    // original maximum is the most ordinary thing a user does with it, so trading the first bug's
    // drift for the clamp's silent data loss was not an improvement.
    //
    // THE FIX: the range GROWS to cover a live value outside it, and never shrinks. A grow is a real
    // authoring event -- the user deliberately sculpted past where the terrain had ever been -- and it
    // re-quantises every OTHER sample once, by at most one step of the newly grown scale (proven in
    // LandscapeTest's ITEM 0.7 (c2)). That is a bounded, one-time cost, not the unbounded loss of a
    // clamp, and it only happens on an edit that actually needs a wider range -- an in-range edit still
    // costs nothing per case (c1).
    //
    // HEADROOM. Growing to EXACTLY the live extreme means the very next stroke that nudges the same
    // peak higher grows the range AGAIN -- on every single autosave while a user sculpts a rising
    // ridge, each one touching untouched samples by up to a step. Since a grow is the only event that
    // is allowed to move untouched samples at all, fewer grow events is strictly better for a whole
    // sculpting session, and the price is a proportionally larger step size for ALL samples -- at
    // 65536 steps across a section's full relief, a few percent of headroom is nowhere near visible.
    // So a grow overshoots the live extreme by kGrowHeadroomFrac of the (pre-grow) span, floored at
    // kGrowHeadroomMinCm for a flat or near-flat section where a percentage alone would be too small
    // to matter.
    constexpr f32 kGrowHeadroomFrac = 0.05f;   // 5% of the pre-grow span
    constexpr f32 kGrowHeadroomMinCm = 10.0f;  // floor, for a flat/near-flat pinned range
    f32 lo = in.hasQuantRange ? in.quantMinCm : liveLo;
    f32 hi = in.hasQuantRange ? in.quantMaxCm : liveHi;
    if (in.hasQuantRange) {
        const f32 headroom = std::max((hi - lo) * kGrowHeadroomFrac, kGrowHeadroomMinCm);
        if (liveLo < lo) lo = liveLo - headroom;
        if (liveHi > hi) hi = liveHi + headroom;
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

    // lo/hi were just grown (above) to cover every live height, so this clamp should never fire in
    // normal operation -- it stays only as a defensive floor/ceiling against float edge cases (e.g. a
    // value equal to lo/hi landing a hair outside [0, 65535] after the multiply), not as the mechanism
    // that used to silently flatten an above-ceiling sculpt.
    std::vector<u8> hgt(in.heights.size() * sizeof(u16));
    const f32 fwd = scale > 0.0f ? 65535.0f / scale : 0.0f;
    for (usize i = 0; i < in.heights.size(); ++i) {
        f32 t = (in.heights[i] - lo) * fwd;
        if (t < 0.0f) t = 0.0f;
        if (t > 65535.0f) t = 65535.0f;
        const u16 q = static_cast<u16>(t + 0.5f);
        std::memcpy(hgt.data() + i * sizeof(u16), &q, sizeof(u16));
    }

    std::vector<u8> rng;
    rng.reserve(sizeof(f32) * 2);
    put<f32>(rng, lo);
    put<f32>(rng, hi);

    Avr1File file;
    file.subtype = kAvrSubtypeLand;
    file.add(kOcLandChunkHeader, std::move(hdr), kAvrChunkRequired);
    file.add(kOcLandChunkHeights, std::move(hgt), kAvrChunkGpuUploadable);
    file.add(kOcLandChunkRange, std::move(rng), 0);
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
