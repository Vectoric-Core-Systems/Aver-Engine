#pragma once
// Binaural rendering for headphones from an ANALYTIC head model (no measured HRTF data set is used or shipped):
//   * interaural time difference: Woodworth's spherical head (radius 8.75 cm, c = 343 m/s), so a source at 90 degrees leads one ear by
//     about 0.656 ms; both ears are delayed around a fixed half-maximum offset so the delay is continuous across the median plane;
//   * interaural level difference: the Brown-Duda head-shadow filter, a first-order pole-zero section per ear, 1 at DC and alpha(theta)
//     at high frequencies, alpha running from 2 (source at the ear) down to 0.1 (shadowed, 150 degrees round), plus a front/back tilt;
//   * elevation and front/back: one pinna reflection whose delay (and so its notch frequency) rises with elevation.
// Everything per voice lives in BinauralState, fixed size, so the audio thread allocates nothing; the delays and the shadow gains are
// interpolated per sample from block to block, so a moving source makes no zipper noise.
//
// Angles are in the listener's frame: azimuth 0 ahead and positive to the right, elevation positive up.
#include "aver/core/Types.hpp"

namespace aver::audio {

inline constexpr u32 kBinauralRing = 256;      // frames of delay memory; 256 holds the longest delay up to a 192 kHz device

// Where one voice should be this block.
struct BinauralTargets {
    f32 delay[2] = {0.0f, 0.0f};     // head delay in samples, [0] left ear, [1] right ear
    f32 shadow[2] = {1.0f, 1.0f};    // high-frequency gain of each ear's head-shadow filter
    f32 pinna = 0.0f;                // pinna reflection delay in samples
};

// The largest interaural time difference of the model, seconds (source at +-90 degrees on the horizon).
f32 binauralMaxItd();
// Interaural time difference, seconds: positive when the source is on the right (the left ear hears it later), 0 on the median plane.
f32 binauralItd(f32 azimuthRad, f32 elevationRad);
// The per-ear delays, shadow gains and pinna delay for a source direction at a sample rate.
BinauralTargets binauralTargets(f32 azimuthRad, f32 elevationRad, f32 sampleRate);

// One voice's binaural filter: a delay line, the two shadow filters and the interpolated parameters.
struct BinauralState {
    f32 ring[kBinauralRing] = {};
    u32 write = 0;
    f32 delay[2] = {0.0f, 0.0f}, shadow[2] = {1.0f, 1.0f}, pinna = 0.0f;          // current values
    f32 dDelay[2] = {0.0f, 0.0f}, dShadow[2] = {0.0f, 0.0f}, dPinna = 0.0f;       // per-sample steps for this block
    f32 z[2] = {0.0f, 0.0f};         // shadow filter memory
    f32 k = 1.0f, invK1 = 0.5f, a1 = 0.0f;     // head-shadow filter: k = sample rate / corner, invK1 = 1 / (1 + k), a1 = (1 - k) / (1 + k)

    void reset();
    // Sets up the next `frames` samples: ramp from the current values to `t`, or jump there when `snap`. A delay may move at most half a
    // sample per sample, so even a teleport bends pitch by a bounded amount instead of clicking.
    void begin(const BinauralTargets& t, u32 frames, f32 sampleRate, bool snap);
    // One input sample in, one sample per ear out; advances the ramps.
    void tick(f32 in, f32& left, f32& right);
};

} // namespace aver::audio
