// Surround maths and device layout choice; see Speakers.hpp.
#include "aver/audio/Speakers.hpp"

#include <cmath>

namespace aver::audio {
namespace {

constexpr f32 kPi = 3.14159265358979f;

// The directional speakers of a layout in order of azimuth, with their channel index; the pairwise pan walks this ring.
struct Ring {
    u32 count;
    u8  idx[7];
    f32 deg[7];
};
constexpr Ring k51 = {5, {4, 0, 2, 1, 5}, {-110.0f, -30.0f, 0.0f, 30.0f, 110.0f}};
constexpr Ring k71 = {7, {4, 6, 0, 2, 1, 7, 5}, {-150.0f, -90.0f, -30.0f, 0.0f, 30.0f, 90.0f, 150.0f}};

const Ring* ringFor(u32 channels) { return channels == 6 ? &k51 : (channels == 8 ? &k71 : nullptr); }

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

u32 popcount(u32 v) { u32 n = 0; for (; v; v &= v - 1) ++n; return n; }

// A device mask that really is 5.1 (front pair, centre, LFE, and a surround pair as back or side) or 7.1 (all four pairs).
bool maskSupports(u32 channels, u32 mask) {
    if (mask == 0) return true;
    using namespace chmask;
    const u32 base = FrontLeft | FrontRight | FrontCentre | Lfe;
    if ((mask & base) != base) return false;
    const bool back = (mask & (BackLeft | BackRight)) == (BackLeft | BackRight);
    const bool side = (mask & (SideLeft | SideRight)) == (SideLeft | SideRight);
    return channels == 8 ? (back && side) : (back || side);
}

} // namespace

f32 speakerAzimuthDeg(u32 channels, u32 index) {
    if (channels == 2) return index == 0 ? -90.0f : 90.0f;
    const Ring* r = ringFor(channels);
    if (!r) return 0.0f;
    for (u32 i = 0; i < r->count; ++i)
        if (r->idx[i] == index) return r->deg[i];
    return 0.0f;
}

void speakerGains(u32 channels, f32 azimuthRad, f32 spread, f32 out[kMaxSpeakers]) {
    for (u32 i = 0; i < kMaxSpeakers; ++i) out[i] = 0.0f;
    if (channels <= 1) { out[0] = 1.0f; return; }
    const f32 s = clamp01(spread);
    if (!(azimuthRad == azimuthRad)) azimuthRad = 0.0f;

    f32 p[kMaxSpeakers] = {};     // squared gains of the point source
    f32 omni = 0.0f;              // squared gain of each directional speaker when the source is spread over all of them
    u32 directional[kMaxSpeakers] = {};
    u32 nDir = 0;

    if (channels == 2) {
        // The legacy pan: constant power on the sine of the azimuth (the lateral component), so L/R match the stereo mixer.
        f32 pan = std::sin(azimuthRad);
        pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
        const f32 t = (pan + 1.0f) * 0.25f * kPi;
        const f32 l = std::cos(t), r = std::sin(t);
        p[0] = l * l;
        p[1] = r * r;
        directional[0] = 0; directional[1] = 1; nDir = 2;
    } else {
        const Ring* ring = ringFor(channels);
        if (!ring) { out[0] = 1.0f; return; }
        // Wrap the angle into [first, first + 360) so the arc across the back is an ordinary pair.
        const f32 first = ring->deg[0];
        f32 deg = std::fmod(azimuthRad * (180.0f / kPi), 360.0f);
        if (deg < first) deg += 360.0f;
        if (deg >= first + 360.0f) deg -= 360.0f;
        // The segment holding deg; past the last speaker it is the arc from the last back round to the first.
        u32 a = ring->count - 1, b = 0;
        f32 degA = ring->deg[a], degB = first + 360.0f;
        for (u32 i = 0; i + 1 < ring->count; ++i)
            if (deg >= ring->deg[i] && deg < ring->deg[i + 1]) { a = i; b = i + 1; degA = ring->deg[i]; degB = ring->deg[i + 1]; break; }
        const f32 t = (deg - degA) / (degB - degA);
        const f32 ga = std::cos(t * 0.5f * kPi), gb = std::sin(t * 0.5f * kPi);
        p[ring->idx[a]] = ga * ga;
        p[ring->idx[b]] = gb * gb;
        for (u32 i = 0; i < ring->count; ++i) directional[nDir++] = ring->idx[i];
    }
    omni = 1.0f / static_cast<f32>(nDir);
    for (u32 i = 0; i < nDir; ++i) {
        const u32 c = directional[i];
        out[c] = std::sqrt((1.0f - s) * p[c] + s * omni);
    }
}

UpmixMatrix stereoUpmix(u32 channels, StereoUpmix mode) {
    UpmixMatrix m;
    if (channels == 1) { m.l[0] = 0.5f; m.r[0] = 0.5f; return m; }
    m.l[0] = 1.0f;
    m.r[1] = 1.0f;
    if (!isSurround(channels) || mode != StereoUpmix::Wrap) return m;

    // A quarter of each input channel's power goes to the surrounds on its side: 5.1 has one such speaker, 7.1 has two.
    const f32 front = std::sqrt(0.75f);
    m.l[0] = front;
    m.r[1] = front;
    if (channels == 6) {
        m.l[4] = 0.5f;
        m.r[5] = 0.5f;
    } else {
        const f32 each = 0.5f / std::sqrt(2.0f);
        m.l[4] = each; m.l[6] = each;
        m.r[5] = each; m.r[7] = each;
    }
    return m;
}

u32 chooseMixChannels(u32 deviceChannels, u32 mask, u32 requested) {
    if (deviceChannels < 2) return 1;
    if (requested >= 8 && deviceChannels >= 8 && maskSupports(8, mask)) return 8;
    if (requested >= 6 && deviceChannels >= 6 && maskSupports(6, mask)) return 6;
    return 2;
}

void buildDeviceMap(u32 mixChannels, u32 deviceChannels, u32 mask, i8* out, u32 cap) {
    const u32 n = deviceChannels < cap ? deviceChannels : cap;
    if (mixChannels <= 2 || mask == 0 || popcount(mask) != deviceChannels) {
        for (u32 d = 0; d < n; ++d) out[d] = d < mixChannels ? static_cast<i8>(d) : static_cast<i8>(-1);
        return;
    }
    using namespace chmask;
    const bool sides = (mask & (SideLeft | SideRight)) == (SideLeft | SideRight);
    u32 d = 0;
    for (u32 bit = 0; bit < 32 && d < n; ++bit) {
        const u32 b = 1u << bit;
        if (!(mask & b)) continue;
        i8 m = -1;
        if (b == FrontLeft) m = 0;
        else if (b == FrontRight) m = 1;
        else if (b == FrontCentre) m = 2;
        else if (b == Lfe) m = 3;
        else if (b == BackLeft) m = mixChannels == 8 ? 4 : (sides ? -1 : 4);
        else if (b == BackRight) m = mixChannels == 8 ? 5 : (sides ? -1 : 5);
        else if (b == SideLeft) m = mixChannels == 8 ? 6 : 4;
        else if (b == SideRight) m = mixChannels == 8 ? 7 : 5;
        out[d++] = m;
    }
}

} // namespace aver::audio
