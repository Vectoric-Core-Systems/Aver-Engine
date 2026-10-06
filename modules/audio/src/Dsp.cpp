// Biquad low-pass, occlusion mapping and fade curves.
#include "aver/audio/Dsp.hpp"

#include <cmath>

namespace aver::audio {
namespace {

constexpr f32 kPi = 3.14159265358979f;

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

BiquadCoeffs lowpassCoeffs(f32 cutoffHz, f32 sampleRate, f32 q) {
    const f32 nyquistCap = 0.45f * sampleRate;
    const f32 fc = cutoffHz < 10.0f ? 10.0f : (cutoffHz > nyquistCap ? nyquistCap : cutoffHz);
    const f32 w0 = 2.0f * kPi * fc / sampleRate;
    const f32 cw = std::cos(w0);
    const f32 alpha = std::sin(w0) / (2.0f * (q > 0.05f ? q : 0.05f));
    const f32 a0 = 1.0f + alpha;
    BiquadCoeffs c;
    c.b0 = (1.0f - cw) * 0.5f / a0;
    c.b1 = (1.0f - cw) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0f * cw / a0;
    c.a2 = (1.0f - alpha) / a0;
    return c;
}

f32 biquadMagnitude(const BiquadCoeffs& c, f32 freqHz, f32 sampleRate) {
    const f64 w = 2.0 * 3.14159265358979 * static_cast<f64>(freqHz) / static_cast<f64>(sampleRate);
    const f64 cw = std::cos(w), sw = std::sin(w);
    const f64 c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    const f64 nr = c.b0 + c.b1 * cw + c.b2 * c2;
    const f64 ni = -(c.b1 * sw + c.b2 * s2);
    const f64 dr = 1.0 + c.a1 * cw + c.a2 * c2;
    const f64 di = -(c.a1 * sw + c.a2 * s2);
    return static_cast<f32>(std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di)));
}

f32 occlusionCutoffHz(f32 occlusion, const OcclusionCurve& c) {
    const f32 o = clamp01(occlusion);
    const f32 lo = c.minCutoffHz > 20.0f ? c.minCutoffHz : 20.0f;
    const f32 hi = c.maxCutoffHz > lo ? c.maxCutoffHz : lo;
    return hi * std::pow(lo / hi, o);
}

f32 occlusionGain(f32 occlusion, const OcclusionCurve& c) {
    const f32 o = clamp01(occlusion);
    const f32 floor_ = clamp01(c.minVolume);
    return 1.0f + (floor_ - 1.0f) * o;
}

f32 smoothingCoeff(f32 dtSeconds, f32 tauSeconds) {
    if (dtSeconds <= 0.0f) return 0.0f;
    if (tauSeconds <= 1.0e-6f) return 1.0f;
    return 1.0f - std::exp(-dtSeconds / tauSeconds);
}

f32 fadeShape(f32 p, FadeCurve curve, bool rising) {
    const f32 t = clamp01(p);
    if (curve == FadeCurve::Linear) return t;
    return rising ? std::sin(t * kPi * 0.5f) : 1.0f - std::cos(t * kPi * 0.5f);
}

void crossfadeGains(f32 p, FadeCurve curve, f32& outGain, f32& inGain) {
    inGain  = fadeShape(p, curve, true);
    outGain = 1.0f - fadeShape(p, curve, false);
}

} // namespace aver::audio
