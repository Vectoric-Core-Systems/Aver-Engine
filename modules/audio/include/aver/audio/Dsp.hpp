#pragma once
// Small, allocation-free DSP pieces the mixer applies on the audio thread: a biquad low-pass, the
// occlusion mapping, and the fade/crossfade curves. Pure arithmetic, so each is checkable headlessly.
#include "aver/core/Types.hpp"

namespace aver::audio {

// Direct-form coefficients of one biquad section (a0 normalised to 1).
struct BiquadCoeffs {
    f32 b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

// One channel's filter memory (transposed direct form II).
struct BiquadState {
    f32 z1 = 0.0f, z2 = 0.0f;
    f32 process(const BiquadCoeffs& c, f32 x) {
        const f32 y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0f; }
};

// Second-order low-pass (RBJ), -3 dB at `cutoffHz` for the default Q. Cutoff is clamped to a stable range.
BiquadCoeffs lowpassCoeffs(f32 cutoffHz, f32 sampleRate, f32 q = 0.70710678f);
// |H| of the section at `freqHz`, for tests and tooling.
f32 biquadMagnitude(const BiquadCoeffs& c, f32 freqHz, f32 sampleRate);

// How a fully occluded sound is darkened and ducked. 0 occlusion is untouched.
struct OcclusionCurve {
    f32 minCutoffHz = 450.0f;    // low-pass cutoff at occlusion 1
    f32 maxCutoffHz = 20000.0f;  // cutoff at occlusion 0 (effectively transparent)
    f32 minVolume   = 0.30f;     // linear gain at occlusion 1
};

// Occlusion below this is treated as clear and the filter is bypassed.
inline constexpr f32 kOcclusionBypass = 0.002f;

// Cutoff for an occlusion in [0,1], log-interpolated between the curve's ends.
f32 occlusionCutoffHz(f32 occlusion, const OcclusionCurve& c);
// Linear gain for an occlusion in [0,1].
f32 occlusionGain(f32 occlusion, const OcclusionCurve& c);
// One-pole smoothing coefficient for a step of dt seconds against time constant tau.
f32 smoothingCoeff(f32 dtSeconds, f32 tauSeconds);

// Shape of a fade between two gains.
enum class FadeCurve : u32 { Linear = 0, EqualPower = 1 };

// Fraction of the way from the start gain to the end gain at progress p in [0,1].
// `rising` picks the sine leg for equal-power so that a rising and a falling fade sum to unit power.
f32 fadeShape(f32 p, FadeCurve curve, bool rising);
// Gains of the outgoing and incoming track at crossfade progress p in [0,1].
void crossfadeGains(f32 p, FadeCurve curve, f32& outGain, f32& inGain);

} // namespace aver::audio
