// Analytic binaural head model; see Binaural.hpp.
#include "aver/audio/Binaural.hpp"

#include <cmath>

namespace aver::audio {
namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kHeadRadius = 0.0875f;                       // metres
constexpr f32 kSpeedOfSound = 343.0f;                      // metres per second
constexpr f32 kShadowCorner = kSpeedOfSound / kHeadRadius; // rad/s; the filter's pole is at twice this
constexpr f32 kEarAzimuth = 100.0f * kPi / 180.0f;         // Brown-Duda put the ears at +-100 degrees
constexpr f32 kShadowMinAlpha = 0.1f;
constexpr f32 kShadowThetaMin = 150.0f * kPi / 180.0f;
constexpr f32 kPinnaGain = 0.3f;                           // the reflection's level against the direct sound
constexpr f32 kPinnaNorm = 1.0f / (1.0f + kPinnaGain);     // keeps the filter at unity at DC
constexpr f32 kPinnaLowS = 83.0e-6f, kPinnaHighS = 50.0e-6f;   // reflection delay below / above the head: its first notch is 1 / (2 tau)
constexpr f32 kMaxDelaySlew = 0.5f;                        // samples of delay change per sample
constexpr f32 kMaxDelaySamples = 200.0f;                   // the ring is 256 long and the pinna tap and interpolation sit above this

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

f32 binauralMaxItd() { return (kHeadRadius / kSpeedOfSound) * (0.5f * kPi + 1.0f); }

f32 binauralItd(f32 az, f32 el) {
    // Woodworth: tau = (a / c) (alpha + sin alpha), alpha the angle from the median plane.
    const f32 sinLat = clampf(std::sin(az) * std::cos(el), -1.0f, 1.0f);
    return (kHeadRadius / kSpeedOfSound) * (std::asin(sinLat) + sinLat);
}

BinauralTargets binauralTargets(f32 az, f32 el, f32 fs) {
    BinauralTargets t;
    const f32 half = 0.5f * binauralMaxItd();
    const f32 itd = binauralItd(az, el);
    t.delay[0] = clampf((half + 0.5f * itd) * fs, 0.0f, kMaxDelaySamples);    // a source on the right reaches the left ear later
    t.delay[1] = clampf((half - 0.5f * itd) * fs, 0.0f, kMaxDelaySamples);

    // The source direction as (ahead, right, up) and each ear's axis.
    const f32 ce = std::cos(el);
    const f32 sx = ce * std::cos(az), sy = ce * std::sin(az);
    const f32 ex = std::cos(kEarAzimuth), ey = std::sin(kEarAzimuth);
    const f32 frontBack = clampf(1.0f + 0.5f * sx, 0.5f, 1.5f);   // behind the head the highs are dulled, ahead they are not
    for (int ear = 0; ear < 2; ++ear) {
        const f32 cosT = clampf(sx * ex + sy * (ear == 1 ? ey : -ey), -1.0f, 1.0f);
        f32 theta = std::acos(cosT);
        if (theta > kShadowThetaMin) theta = kShadowThetaMin;
        const f32 alpha = 1.0f + 0.5f * kShadowMinAlpha + (1.0f - 0.5f * kShadowMinAlpha) * std::cos(theta * (kPi / kShadowThetaMin));
        t.shadow[ear] = alpha * frontBack;
    }

    const f32 up = clampf((el + 0.5f * kPi) / kPi, 0.0f, 1.0f);
    t.pinna = (kPinnaLowS + (kPinnaHighS - kPinnaLowS) * up) * fs;
    return t;
}

void BinauralState::reset() {
    for (u32 i = 0; i < kBinauralRing; ++i) ring[i] = 0.0f;
    write = 0;
    z[0] = z[1] = 0.0f;
    dDelay[0] = dDelay[1] = dShadow[0] = dShadow[1] = dPinna = 0.0f;
}

void BinauralState::begin(const BinauralTargets& t, u32 frames, f32 fs, bool snap) {
    k = fs / kShadowCorner;
    invK1 = 1.0f / (1.0f + k);
    a1 = (1.0f - k) * invK1;
    if (snap || frames == 0) {
        for (int e = 0; e < 2; ++e) { delay[e] = t.delay[e]; shadow[e] = t.shadow[e]; dDelay[e] = dShadow[e] = 0.0f; }
        pinna = t.pinna;
        dPinna = 0.0f;
        return;
    }
    const f32 inv = 1.0f / static_cast<f32>(frames);
    const f32 maxMove = kMaxDelaySlew * static_cast<f32>(frames);
    for (int e = 0; e < 2; ++e) {
        dDelay[e] = clampf(t.delay[e] - delay[e], -maxMove, maxMove) * inv;
        dShadow[e] = (t.shadow[e] - shadow[e]) * inv;
    }
    dPinna = (t.pinna - pinna) * inv;
}

void BinauralState::tick(f32 in, f32& left, f32& right) {
    ring[write & (kBinauralRing - 1)] = in;
    f32 out[2];
    for (int e = 0; e < 2; ++e) {
        // Two taps of the delay line at fractional positions: the head delay, and that plus the pinna reflection.
        const f32 d0 = delay[e] > 0.0f ? delay[e] : 0.0f, d1 = d0 + (pinna > 0.0f ? pinna : 0.0f);
        const u32 i0 = static_cast<u32>(d0), i1 = static_cast<u32>(d1);
        const f32 f0 = d0 - static_cast<f32>(i0), f1 = d1 - static_cast<f32>(i1);
        const f32 a0 = ring[(write - i0) & (kBinauralRing - 1)], b0 = ring[(write - i0 - 1) & (kBinauralRing - 1)];
        const f32 a1v = ring[(write - i1) & (kBinauralRing - 1)], b1v = ring[(write - i1 - 1) & (kBinauralRing - 1)];
        const f32 direct = a0 + (b0 - a0) * f0;
        const f32 reflected = a1v + (b1v - a1v) * f1;
        const f32 h = (direct + kPinnaGain * reflected) * kPinnaNorm;
        // Head shadow: H(z) = ((1 + alpha k) + (1 - alpha k) z^-1) / ((1 + k) + (1 - k) z^-1); unity at DC, alpha at Nyquist's side.
        const f32 ak = shadow[e] * k;
        const f32 y = (1.0f + ak) * invK1 * h + z[e];
        z[e] = (1.0f - ak) * invK1 * h - a1 * y;
        out[e] = y;
        delay[e] += dDelay[e];
        shadow[e] += dShadow[e];
    }
    pinna += dPinna;
    ++write;
    left = out[0];
    right = out[1];
}

} // namespace aver::audio
