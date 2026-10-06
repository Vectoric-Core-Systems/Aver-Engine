// Send reverb (Freeverb topology, RT60-derived feedback) and reverb zone math.
#include "aver/audio/Reverb.hpp"

#include <algorithm>
#include <cmath>

namespace aver::audio {
namespace {

// Delay lengths at 44.1 kHz, scaled to the device rate. The right channel is offset to decorrelate.
constexpr u32 kCombTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr u32 kAllpassTuning[4] = {556, 441, 341, 225};
constexpr u32 kStereoSpread = 23;
constexpr f32 kInputGain = 0.03f;

u32 scaled(u32 samples44k, u32 rate) {
    const u32 n = static_cast<u32>(static_cast<f64>(samples44k) * static_cast<f64>(rate) / 44100.0 + 0.5);
    return n < 1 ? 1 : n;
}

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

void Reverb::init(u32 sampleRate) {
    rate_ = sampleRate;
    for (u32 i = 0; i < kCombs; ++i) {
        combL_[i].buf.assign(scaled(kCombTuning[i], rate_), 0.0f);
        combR_[i].buf.assign(scaled(kCombTuning[i] + kStereoSpread, rate_), 0.0f);
    }
    for (u32 i = 0; i < kAllpass; ++i) {
        apL_[i].buf.assign(scaled(kAllpassTuning[i], rate_), 0.0f);
        apR_[i].buf.assign(scaled(kAllpassTuning[i] + kStereoSpread, rate_), 0.0f);
    }
    decay_ = damping_ = -1.0f;
    setParams(1.5f, 0.4f);
    reset();
}

void Reverb::reset() {
    auto clear = [](Line& l) { std::fill(l.buf.begin(), l.buf.end(), 0.0f); l.pos = 0; l.store = 0.0f; };
    for (u32 i = 0; i < kCombs; ++i) { clear(combL_[i]); clear(combR_[i]); }
    for (u32 i = 0; i < kAllpass; ++i) { clear(apL_[i]); clear(apR_[i]); }
}

void Reverb::setParams(f32 decaySec, f32 damping) {
    if (rate_ == 0) return;
    const f32 d = clampf(decaySec, 0.1f, 20.0f);
    const f32 dm = clampf(damping, 0.0f, 1.0f);
    if (d == decay_ && dm == damping_) return;
    decay_ = d; damping_ = dm;
    damp_ = dm * 0.9f;
    // A comb of N samples must lose 60 dB over d seconds: g = 10^(-3 N / (rate d)).
    auto feedbackFor = [&](usize delay) {
        const f32 g = std::pow(10.0f, -3.0f * static_cast<f32>(delay) / (static_cast<f32>(rate_) * d));
        return g > 0.995f ? 0.995f : g;
    };
    for (u32 i = 0; i < kCombs; ++i) {
        combL_[i].feedback = feedbackFor(combL_[i].buf.size());
        combR_[i].feedback = feedbackFor(combR_[i].buf.size());
    }
}

void Reverb::process(const f32* in, f32* outL, f32* outR, u32 frames) {
    for (u32 f = 0; f < frames; ++f) {
        const f32 x = in[f] * kInputGain;
        f32 l = 0.0f, r = 0.0f;
        for (u32 i = 0; i < kCombs; ++i) {
            Line& a = combL_[i];
            const f32 ya = a.buf[a.pos];
            a.store = ya * (1.0f - damp_) + a.store * damp_;
            a.buf[a.pos] = x + a.store * a.feedback;
            if (++a.pos >= a.buf.size()) a.pos = 0;
            l += ya;

            Line& b = combR_[i];
            const f32 yb = b.buf[b.pos];
            b.store = yb * (1.0f - damp_) + b.store * damp_;
            b.buf[b.pos] = x + b.store * b.feedback;
            if (++b.pos >= b.buf.size()) b.pos = 0;
            r += yb;
        }
        for (u32 i = 0; i < kAllpass; ++i) {
            Line& a = apL_[i];
            const f32 ta = a.buf[a.pos];
            a.buf[a.pos] = l + ta * 0.5f;
            l = ta - l;
            if (++a.pos >= a.buf.size()) a.pos = 0;

            Line& b = apR_[i];
            const f32 tb = b.buf[b.pos];
            b.buf[b.pos] = r + tb * 0.5f;
            r = tb - r;
            if (++b.pos >= b.buf.size()) b.pos = 0;
        }
        outL[f] = l;
        outR[f] = r;
    }
}

// ---------------------------------------------------------------- zones

f32 zoneSignedDistance(ZoneShape shape, const f32 halfExtents[3], const f32 p[3]) {
    if (shape == ZoneShape::Sphere) {
        const f32 len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        return len - halfExtents[0];
    }
    const f32 q0 = std::fabs(p[0]) - halfExtents[0];
    const f32 q1 = std::fabs(p[1]) - halfExtents[1];
    const f32 q2 = std::fabs(p[2]) - halfExtents[2];
    const f32 m0 = q0 > 0.0f ? q0 : 0.0f, m1 = q1 > 0.0f ? q1 : 0.0f, m2 = q2 > 0.0f ? q2 : 0.0f;
    const f32 outside = std::sqrt(m0 * m0 + m1 * m1 + m2 * m2);
    const f32 inside = std::min(std::max(q0, std::max(q1, q2)), 0.0f);
    return outside + inside;
}

f32 zoneWeight(f32 sd, f32 blend) {
    if (sd <= 0.0f) return 1.0f;
    if (blend <= 0.0f || sd >= blend) return 0.0f;
    const f32 t = 1.0f - sd / blend;
    return t * t * (3.0f - 2.0f * t);
}

ReverbParams blendReverbZones(const ReverbZoneInput* zones, usize count, const ReverbParams& ambient) {
    std::vector<const ReverbZoneInput*> order;
    order.reserve(count);
    for (usize i = 0; i < count; ++i) if (zones[i].weight > 0.0f) order.push_back(&zones[i]);
    std::stable_sort(order.begin(), order.end(),
                     [](const ReverbZoneInput* a, const ReverbZoneInput* b) { return a->priority > b->priority; });

    f32 remaining = 1.0f, sumC = 0.0f, wet = 0.0f, decay = 0.0f, damping = 0.0f;
    for (const ReverbZoneInput* z : order) {
        const f32 w = clampf(z->weight, 0.0f, 1.0f);
        const f32 c = w * remaining;
        remaining *= (1.0f - w);
        sumC += c;
        wet += c * z->params.wet;
        decay += c * z->params.decaySec;
        damping += c * z->params.damping;
    }
    ReverbParams out;
    out.wet = wet + remaining * ambient.wet;
    if (sumC > 1.0e-6f) { out.decaySec = decay / sumC; out.damping = damping / sumC; }
    else { out.decaySec = ambient.decaySec; out.damping = ambient.damping; }
    return out;
}

} // namespace aver::audio
