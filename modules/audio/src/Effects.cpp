// The bus effects (Godot's AudioEffect* set). Each one is a small class over interleaved stereo blocks; docs/AUDIO.md lists what is
// Godot's algorithm and what is an approximation of it.
#include "aver/audio/Effects.hpp"
#include "aver/audio/Convolver.hpp"
#include "aver/audio/Reverb.hpp"

#include <algorithm>
#include <complex>
#include <cstring>
#include <deque>
#include <string>

namespace aver::audio {
namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kSqrt2 = 1.41421356f;
constexpr u32 kChunk = 2048;   // scratch size of the effects that need one

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
f32 undenorm(f32 x) { return std::fabs(x) < 1.0e-20f ? 0.0f : x; }

// Pan law shared by the delay taps: unity at the centre, constant power across the range.
void panGainsUnity(f32 pan, f32& l, f32& r) {
    const f32 t = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    l = kSqrt2 * std::cos(t);
    r = kSqrt2 * std::sin(t);
}

// ---------------------------------------------------------------- parameter tables

constexpr f32 kEq6[] = {32, 100, 320, 1000, 3200, 10000};
constexpr f32 kEq10[] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
constexpr f32 kEq21[] = {22, 32, 44, 63, 90, 125, 175, 250, 350, 500, 700, 1000, 1400, 2000, 2800, 4000, 5600, 8000, 11000, 16000, 22000};

struct Tables {
    std::vector<ParamInfo> t[static_cast<usize>(EffectKind::Count)];
    std::deque<std::string> names;

    void add(EffectKind k, const std::string& name, f32 def, f32 lo, f32 hi) {
        names.push_back(name);
        t[static_cast<usize>(k)].push_back({names.back().c_str(), def, lo, hi});
    }

    void addEq(EffectKind k, const f32* hz, usize n) {
        for (usize i = 0; i < n; ++i) add(k, "band_db/" + std::to_string(static_cast<int>(hz[i])) + "_hz", 0.0f, -60.0f, 24.0f);
    }

    Tables() {
        add(EffectKind::Amplify, "volume_db", 0.0f, -80.0f, 24.0f);

        add(EffectKind::Delay, "dry", 1.0f, 0.0f, 1.0f);
        add(EffectKind::Delay, "tap1_active", 1.0f, 0.0f, 1.0f);
        add(EffectKind::Delay, "tap1_delay_ms", 250.0f, 0.0f, 1500.0f);
        add(EffectKind::Delay, "tap1_level_db", -6.0f, -60.0f, 0.0f);
        add(EffectKind::Delay, "tap1_pan", 0.2f, -1.0f, 1.0f);
        add(EffectKind::Delay, "tap2_active", 1.0f, 0.0f, 1.0f);
        add(EffectKind::Delay, "tap2_delay_ms", 500.0f, 0.0f, 1500.0f);
        add(EffectKind::Delay, "tap2_level_db", -12.0f, -60.0f, 0.0f);
        add(EffectKind::Delay, "tap2_pan", -0.4f, -1.0f, 1.0f);
        add(EffectKind::Delay, "feedback_active", 0.0f, 0.0f, 1.0f);
        add(EffectKind::Delay, "feedback_delay_ms", 340.0f, 0.0f, 1500.0f);
        add(EffectKind::Delay, "feedback_level_db", -6.0f, -60.0f, 0.0f);
        add(EffectKind::Delay, "feedback_lowpass", 16000.0f, 1.0f, 16000.0f);

        add(EffectKind::Chorus, "voice_count", 2.0f, 1.0f, 4.0f);
        add(EffectKind::Chorus, "dry", 1.0f, 0.0f, 1.0f);
        add(EffectKind::Chorus, "wet", 0.5f, 0.0f, 1.0f);
        const f32 delay[4] = {15, 20, 12, 22}, rate[4] = {0.8f, 1.2f, 1.0f, 0.7f}, depth[4] = {2, 3, 2, 3}, pan[4] = {-0.5f, 0.5f, -0.25f, 0.25f};
        for (int v = 0; v < 4; ++v) {
            const std::string p = "voice/" + std::to_string(v + 1) + "/";
            add(EffectKind::Chorus, p + "delay_ms", delay[v], 0.0f, 50.0f);
            add(EffectKind::Chorus, p + "rate_hz", rate[v], 0.1f, 20.0f);
            add(EffectKind::Chorus, p + "depth_ms", depth[v], 0.0f, 20.0f);
            add(EffectKind::Chorus, p + "level_db", 0.0f, -60.0f, 24.0f);
            add(EffectKind::Chorus, p + "cutoff_hz", 8000.0f, 1.0f, 16000.0f);
            add(EffectKind::Chorus, p + "pan", pan[v], -1.0f, 1.0f);
        }

        add(EffectKind::Compressor, "threshold", 0.0f, -60.0f, 0.0f);
        add(EffectKind::Compressor, "ratio", 4.0f, 1.0f, 48.0f);
        add(EffectKind::Compressor, "gain", 0.0f, -20.0f, 20.0f);
        add(EffectKind::Compressor, "attack_us", 20.0f, 20.0f, 2000.0f);
        add(EffectKind::Compressor, "release_ms", 250.0f, 20.0f, 2000.0f);
        add(EffectKind::Compressor, "mix", 1.0f, 0.0f, 1.0f);

        add(EffectKind::Limiter, "ceiling_db", -0.1f, -20.0f, -0.1f);
        add(EffectKind::Limiter, "threshold_db", 0.0f, -30.0f, 0.0f);
        add(EffectKind::Limiter, "soft_clip_db", 2.0f, 0.0f, 6.0f);
        add(EffectKind::Limiter, "soft_clip_ratio", 10.0f, 3.0f, 20.0f);

        add(EffectKind::HardLimiter, "pre_gain_db", 0.0f, -24.0f, 24.0f);
        add(EffectKind::HardLimiter, "ceiling_db", -0.3f, -24.0f, 0.0f);
        add(EffectKind::HardLimiter, "release", 0.1f, 0.01f, 3.0f);

        addEq(EffectKind::EQ6, kEq6, 6);
        addEq(EffectKind::EQ10, kEq10, 10);
        addEq(EffectKind::EQ21, kEq21, 21);

        for (EffectKind k : {EffectKind::LowPass, EffectKind::HighPass, EffectKind::BandPass, EffectKind::Notch, EffectKind::LowShelf, EffectKind::HighShelf}) {
            add(k, "cutoff_hz", 2000.0f, 1.0f, 20500.0f);
            add(k, "resonance", 0.5f, 0.0f, 1.0f);
            add(k, "gain", 1.0f, 0.0f, 4.0f);
            add(k, "db", 0.0f, 0.0f, 3.0f);
        }

        add(EffectKind::Distortion, "mode", 0.0f, 0.0f, 4.0f);
        add(EffectKind::Distortion, "pre_gain", 0.0f, -60.0f, 60.0f);
        add(EffectKind::Distortion, "keep_hf_hz", 16000.0f, 1.0f, 20500.0f);
        add(EffectKind::Distortion, "drive", 0.0f, 0.0f, 1.0f);
        add(EffectKind::Distortion, "post_gain", 0.0f, -80.0f, 24.0f);

        add(EffectKind::Phaser, "range_min_hz", 440.0f, 10.0f, 10000.0f);
        add(EffectKind::Phaser, "range_max_hz", 1600.0f, 10.0f, 10000.0f);
        add(EffectKind::Phaser, "rate_hz", 0.5f, 0.01f, 20.0f);
        add(EffectKind::Phaser, "feedback", 0.7f, 0.0f, 0.95f);
        add(EffectKind::Phaser, "depth", 1.0f, 0.0f, 4.0f);

        add(EffectKind::PitchShift, "pitch_scale", 1.0f, 0.01f, 16.0f);
        add(EffectKind::PitchShift, "oversampling", 4.0f, 4.0f, 32.0f);
        add(EffectKind::PitchShift, "fft_size", 3.0f, 0.0f, 4.0f);

        add(EffectKind::Reverb, "predelay_msec", 150.0f, 20.0f, 500.0f);
        add(EffectKind::Reverb, "predelay_feedback", 0.4f, 0.0f, 0.98f);
        add(EffectKind::Reverb, "room_size", 0.8f, 0.0f, 1.0f);
        add(EffectKind::Reverb, "damping", 0.5f, 0.0f, 1.0f);
        add(EffectKind::Reverb, "spread", 1.0f, 0.0f, 1.0f);
        add(EffectKind::Reverb, "hipass", 0.0f, 0.0f, 1.0f);
        add(EffectKind::Reverb, "dry", 1.0f, 0.0f, 1.0f);
        add(EffectKind::Reverb, "wet", 0.5f, 0.0f, 1.0f);

        add(EffectKind::ConvolutionReverb, "dry", 1.0f, 0.0f, 1.0f);
        add(EffectKind::ConvolutionReverb, "wet", 0.5f, 0.0f, 1.0f);
        add(EffectKind::ConvolutionReverb, "predelay_msec", 0.0f, 0.0f, 500.0f);
        add(EffectKind::ConvolutionReverb, "gain_db", 0.0f, -24.0f, 24.0f);

        add(EffectKind::Panner, "pan", 0.0f, -1.0f, 1.0f);

        add(EffectKind::StereoEnhance, "pan_pullout", 1.0f, 0.0f, 4.0f);
        add(EffectKind::StereoEnhance, "time_pullout_ms", 0.0f, 0.0f, 50.0f);
        add(EffectKind::StereoEnhance, "surround", 0.0f, 0.0f, 1.0f);

        add(EffectKind::Capture, "buffer_length", 0.1f, 0.01f, 10.0f);

        add(EffectKind::SpectrumAnalyzer, "buffer_length", 2.0f, 0.1f, 4.0f);
        add(EffectKind::SpectrumAnalyzer, "fft_size", 2.0f, 0.0f, 4.0f);
        add(EffectKind::SpectrumAnalyzer, "tap_back_pos", 0.01f, 0.0f, 4.0f);
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

constexpr const char* kClassNames[] = {
    "AudioEffectAmplify", "AudioEffectDelay", "AudioEffectChorus", "AudioEffectCompressor", "AudioEffectLimiter", "AudioEffectHardLimiter",
    "AudioEffectEQ6", "AudioEffectEQ10", "AudioEffectEQ21", "AudioEffectLowPassFilter", "AudioEffectHighPassFilter", "AudioEffectBandPassFilter",
    "AudioEffectNotchFilter", "AudioEffectLowShelfFilter", "AudioEffectHighShelfFilter", "AudioEffectDistortion", "AudioEffectPhaser",
    "AudioEffectPitchShift", "AudioEffectReverb", "AudioEffectPanner", "AudioEffectStereoEnhance", "AudioEffectCapture", "AudioEffectSpectrumAnalyzer",
    "AudioEffectConvolutionReverb"};
static_assert(sizeof(kClassNames) / sizeof(kClassNames[0]) == static_cast<usize>(EffectKind::Count), "one Godot class name per effect kind");

// ---------------------------------------------------------------- Amplify

class Amplify final : public Effect {
public:
    explicit Amplify(u32 rate) : Effect(EffectKind::Amplify, rate) {}
    void process(f32* io, u32 frames, const f32*) override {
        const f32 g = dbToLin(param(0));
        if (last_ < 0.0f) last_ = g;
        const f32 step = frames ? (g - last_) / static_cast<f32>(frames) : 0.0f;
        f32 cur = last_;
        for (u32 i = 0; i < frames; ++i) { cur += step; io[2 * i] *= cur; io[2 * i + 1] *= cur; }
        last_ = g;
    }
    void reset() override { last_ = -1.0f; }
private:
    f32 last_ = -1.0f;
};

// ---------------------------------------------------------------- Delay

class Delay final : public Effect {
public:
    explicit Delay(u32 rate) : Effect(EffectKind::Delay, rate) {
        size_ = static_cast<u32>(rate * 2) + 8;
        buf_.assign(static_cast<usize>(size_) * 2, 0.0f);
    }
    void reset() override { std::fill(buf_.begin(), buf_.end(), 0.0f); w_ = 0; lpL_ = lpR_ = 0.0f; }
    void process(f32* io, u32 frames, const f32*) override {
        const f32 dry = param(0);
        struct Tap { bool on; u32 n; f32 level, gl, gr; } taps[2];
        for (int t = 0; t < 2; ++t) {
            const u32 base = 1 + static_cast<u32>(t) * 4;
            taps[t].on = param(base) > 0.5f;
            taps[t].n = std::min(size_ - 1, static_cast<u32>(param(base + 1) * 0.001f * static_cast<f32>(rate_)));
            taps[t].level = dbToLin(param(base + 2));
            panGainsUnity(param(base + 3), taps[t].gl, taps[t].gr);
        }
        const bool fbOn = param(9) > 0.5f;
        const u32 fbN = std::min(size_ - 1, static_cast<u32>(param(10) * 0.001f * static_cast<f32>(rate_)));
        const f32 fbLevel = dbToLin(param(11));
        const f32 a = std::exp(-2.0f * kPi * clampf(param(12), 1.0f, 16000.0f) / static_cast<f32>(rate_));
        for (u32 i = 0; i < frames; ++i) {
            const f32 inL = io[2 * i], inR = io[2 * i + 1];
            f32 fbL = 0.0f, fbR = 0.0f;
            if (fbOn) {
                const u32 idx = (w_ + size_ - fbN) % size_;
                lpL_ = undenorm(lpL_ * a + buf_[2 * idx] * (1.0f - a));
                lpR_ = undenorm(lpR_ * a + buf_[2 * idx + 1] * (1.0f - a));
                fbL = lpL_ * fbLevel;
                fbR = lpR_ * fbLevel;
            }
            buf_[2 * w_] = inL + fbL;
            buf_[2 * w_ + 1] = inR + fbR;
            f32 outL = inL * dry, outR = inR * dry;
            for (const Tap& t : taps) {
                if (!t.on) continue;
                const u32 idx = (w_ + size_ - t.n) % size_;
                outL += buf_[2 * idx] * t.level * t.gl;
                outR += buf_[2 * idx + 1] * t.level * t.gr;
            }
            io[2 * i] = outL;
            io[2 * i + 1] = outR;
            if (++w_ >= size_) w_ = 0;
        }
    }
private:
    u32 size_ = 0, w_ = 0;
    std::vector<f32> buf_;
    f32 lpL_ = 0.0f, lpR_ = 0.0f;
};

// ---------------------------------------------------------------- Chorus

class Chorus final : public Effect {
public:
    explicit Chorus(u32 rate) : Effect(EffectKind::Chorus, rate) {
        size_ = static_cast<u32>(rate * 0.1f) + 8;
        buf_.assign(static_cast<usize>(size_) * 2, 0.0f);
        for (int v = 0; v < 4; ++v) phase_[v] = static_cast<f32>(v) * 0.25f;
    }
    void reset() override { std::fill(buf_.begin(), buf_.end(), 0.0f); w_ = 0; for (f32& s : lp_) s = 0.0f; }
    void process(f32* io, u32 frames, const f32*) override {
        const u32 voices = static_cast<u32>(clampf(param(0), 1.0f, 4.0f));
        const f32 dry = param(1), wet = param(2);
        struct V { f32 delay, rate, depth, level, a, gl, gr; } v[4];
        for (u32 k = 0; k < voices; ++k) {
            const u32 b = 3 + k * 6;
            v[k].delay = param(b) * 0.001f * static_cast<f32>(rate_);
            v[k].rate = param(b + 1) / static_cast<f32>(rate_);
            v[k].depth = param(b + 2) * 0.001f * static_cast<f32>(rate_);
            v[k].level = dbToLin(param(b + 3));
            v[k].a = std::exp(-2.0f * kPi * clampf(param(b + 4), 1.0f, 16000.0f) / static_cast<f32>(rate_));
            panGainsUnity(param(b + 5), v[k].gl, v[k].gr);
        }
        for (u32 i = 0; i < frames; ++i) {
            const f32 inL = io[2 * i], inR = io[2 * i + 1];
            buf_[2 * w_] = inL;
            buf_[2 * w_ + 1] = inR;
            f32 wl = 0.0f, wr = 0.0f;
            for (u32 k = 0; k < voices; ++k) {
                phase_[k] += v[k].rate;
                if (phase_[k] >= 1.0f) phase_[k] -= 1.0f;
                f32 d = v[k].delay + v[k].depth * std::sin(2.0f * kPi * phase_[k]);
                d = clampf(d, 0.0f, static_cast<f32>(size_ - 2));
                const u32 n0 = static_cast<u32>(d);
                const f32 fr = d - static_cast<f32>(n0);
                const u32 i0 = (w_ + size_ - n0) % size_, i1 = (i0 + size_ - 1) % size_;
                const f32 mono = 0.5f * ((buf_[2 * i0] + buf_[2 * i0 + 1]) * (1.0f - fr) + (buf_[2 * i1] + buf_[2 * i1 + 1]) * fr);
                lp_[k] = undenorm(lp_[k] * v[k].a + mono * (1.0f - v[k].a));
                wl += lp_[k] * v[k].level * v[k].gl;
                wr += lp_[k] * v[k].level * v[k].gr;
            }
            io[2 * i] = inL * dry + wl * wet;
            io[2 * i + 1] = inR * dry + wr * wet;
            if (++w_ >= size_) w_ = 0;
        }
    }
private:
    u32 size_ = 0, w_ = 0;
    std::vector<f32> buf_;
    f32 phase_[4] = {};
    f32 lp_[4] = {};
};

// ---------------------------------------------------------------- Compressor

class Compressor final : public Effect {
public:
    explicit Compressor(u32 rate) : Effect(EffectKind::Compressor, rate) {}
    void reset() override { env_ = 0.0f; }
    void process(f32* io, u32 frames, const f32* sidechain) override {
        const f32 thr = param(0), ratio = std::max(1.0f, param(1)), makeupDb = param(2), mix = param(5);
        const f32 atk = std::exp(-1.0f / (param(3) * 1.0e-6f * static_cast<f32>(rate_)));
        const f32 rel = std::exp(-1.0f / (param(4) * 1.0e-3f * static_cast<f32>(rate_)));
        const f32* det = sidechain ? sidechain : io;
        for (u32 i = 0; i < frames; ++i) {
            const f32 peak = std::max(std::fabs(det[2 * i]), std::fabs(det[2 * i + 1]));
            env_ = peak > env_ ? peak + atk * (env_ - peak) : peak + rel * (env_ - peak);
            env_ = undenorm(env_);
            const f32 inDb = linToDb(env_);
            const f32 reduction = inDb > thr ? (inDb - thr) * (1.0f - 1.0f / ratio) : 0.0f;
            const f32 g = dbToLin(makeupDb - reduction);
            io[2 * i] = io[2 * i] * (1.0f - mix) + io[2 * i] * g * mix;
            io[2 * i + 1] = io[2 * i + 1] * (1.0f - mix) + io[2 * i + 1] * g * mix;
        }
    }
private:
    f32 env_ = 0.0f;
};

// ---------------------------------------------------------------- Limiters

// Soft limiter: a transparent knee, then a tanh curve that approaches the ceiling and never reaches it.
class Limiter final : public Effect {
public:
    explicit Limiter(u32 rate) : Effect(EffectKind::Limiter, rate) {}
    void process(f32* io, u32 frames, const f32*) override {
        const f32 ceiling = dbToLin(param(0));
        const f32 pre = dbToLin(-param(1));
        const f32 knee = ceiling * dbToLin(-param(2));
        const f32 span = std::max(1.0e-6f, ceiling - knee);
        const f32 slope = param(3) * 0.1f;
        for (u32 i = 0; i < 2 * frames; ++i) {
            const f32 x = io[i] * pre;
            const f32 a = std::fabs(x);
            if (a <= knee) { io[i] = x; continue; }
            const f32 y = knee + span * std::tanh((a - knee) / span * slope);
            io[i] = x < 0.0f ? -y : y;
        }
    }
};

// Look-ahead brick-wall limiter: the output never exceeds the ceiling. The gain is the running minimum of the wanted gain over the
// look-ahead window, averaged over the same window (so it is already down when the peak arrives), with a release that rises slowly.
class HardLimiter final : public Effect {
public:
    explicit HardLimiter(u32 rate) : Effect(EffectKind::HardLimiter, rate) {
        n_ = std::max<u32>(2, static_cast<u32>(0.0015f * static_cast<f32>(rate)));
        xl_.assign(n_, 0.0f);
        xr_.assign(n_, 0.0f);
        tg_.assign(n_, 1.0f);
        mg_.assign(n_, 1.0f);
    }
    u32 latencyFrames() const override { return n_ - 1; }
    void reset() override { std::fill(xl_.begin(), xl_.end(), 0.0f); std::fill(xr_.begin(), xr_.end(), 0.0f); std::fill(tg_.begin(), tg_.end(), 1.0f); std::fill(mg_.begin(), mg_.end(), 1.0f); sum_ = static_cast<f32>(n_); gPrev_ = 1.0f; w_ = 0; }
    void process(f32* io, u32 frames, const f32*) override {
        const f32 pre = dbToLin(param(0)), ceiling = dbToLin(param(1));
        const f32 riseK = 1.0f - std::exp(-1.0f / (param(2) * static_cast<f32>(rate_)));
        if (!primed_) { sum_ = static_cast<f32>(n_); primed_ = true; }
        for (u32 i = 0; i < frames; ++i) {
            const f32 l = io[2 * i] * pre, r = io[2 * i + 1] * pre;
            const f32 peak = std::max(std::fabs(l), std::fabs(r));
            const f32 want = peak > ceiling ? ceiling / peak : 1.0f;
            xl_[w_] = l; xr_[w_] = r; tg_[w_] = want;
            // The oldest sample still in the ring is the output: its window (itself and the n_ - 1 after it) is the whole ring.
            const u32 o = (w_ + 1) % n_;
            const f32 outL = xl_[o], outR = xr_[o];
            f32 m = 1.0f;
            for (u32 k = 0; k < n_; ++k) m = std::min(m, tg_[k]);
            sum_ += m - mg_[w_];
            mg_[w_] = m;
            const f32 smooth = sum_ / static_cast<f32>(n_);
            const f32 g = std::min(smooth, gPrev_ + (1.0f - gPrev_) * riseK);
            gPrev_ = g;
            io[2 * i] = outL * g;
            io[2 * i + 1] = outR * g;
            if (++w_ >= n_) w_ = 0;
        }
    }
private:
    u32 n_ = 2, w_ = 0;
    std::vector<f32> xl_, xr_, tg_, mg_;
    f32 sum_ = 0.0f, gPrev_ = 1.0f;
    bool primed_ = false;
};

// ---------------------------------------------------------------- Filters and equalisers

class FilterFx final : public Effect {
public:
    FilterFx(EffectKind k, FilterMode mode, u32 rate) : Effect(k, rate), mode_(mode) {}
    void reset() override { for (auto& c : st_) for (auto& s : c) s.reset(); }
    void process(f32* io, u32 frames, const f32*) override {
        const u32 stages = static_cast<u32>(clampf(param(3), 0.0f, 3.0f)) + 1;
        const BiquadCoeffs c = godotFilterCoeffs(mode_, param(0), param(1), param(2), stages, static_cast<f32>(rate_));
        for (u32 i = 0; i < frames; ++i)
            for (u32 ch = 0; ch < 2; ++ch) {
                f32 x = io[2 * i + ch];
                for (u32 s = 0; s < stages; ++s) x = st_[ch][s].process(c, x);
                io[2 * i + ch] = x;
            }
    }
private:
    FilterMode mode_;
    BiquadState st_[2][4];
};

class Equalizer final : public Effect {
public:
    Equalizer(EffectKind k, const f32* hz, u32 bands, f32 bwOctaves, u32 rate) : Effect(k, rate), hz_(hz), bands_(bands), bw_(bwOctaves) {
        for (u32 i = 0; i < bands_; ++i) lastDb_[i] = 0.0f;
        for (u32 i = 0; i < bands_; ++i) recompute(i, 0.0f);
    }
    void reset() override { for (auto& c : st_) for (auto& s : c) s.reset(); }
    void process(f32* io, u32 frames, const f32*) override {
        u32 active[21];
        u32 n = 0;
        for (u32 b = 0; b < bands_; ++b) {
            const f32 db = param(b);
            if (db != lastDb_[b]) recompute(b, db);
            if (std::fabs(db) > 1.0e-4f) active[n++] = b;
        }
        for (u32 i = 0; i < frames; ++i)
            for (u32 ch = 0; ch < 2; ++ch) {
                f32 x = io[2 * i + ch];
                for (u32 k = 0; k < n; ++k) x = st_[ch][active[k]].process(co_[active[k]], x);
                io[2 * i + ch] = x;
            }
    }
    // RBJ peaking section: exactly `db` at the band centre.
    static BiquadCoeffs peaking(f32 hz, f32 db, f32 bwOctaves, f32 rate) {
        const f32 fc = clampf(hz, 10.0f, 0.45f * rate);
        const f32 w0 = 2.0f * kPi * fc / rate;
        const f32 A = std::pow(10.0f, db / 40.0f);
        const f32 s = std::sin(w0);
        const f32 alpha = s * std::sinh(0.34657359f * bwOctaves * w0 / s);
        const f32 a0 = 1.0f + alpha / A, cw = std::cos(w0);
        BiquadCoeffs c;
        c.b0 = (1.0f + alpha * A) / a0;
        c.b1 = -2.0f * cw / a0;
        c.b2 = (1.0f - alpha * A) / a0;
        c.a1 = -2.0f * cw / a0;
        c.a2 = (1.0f - alpha / A) / a0;
        return c;
    }
private:
    void recompute(u32 b, f32 db) { co_[b] = peaking(hz_[b], db, bw_, static_cast<f32>(rate_)); lastDb_[b] = db; }
    const f32* hz_;
    u32 bands_;
    f32 bw_;
    BiquadCoeffs co_[21];
    f32 lastDb_[21];
    BiquadState st_[2][21];
};

// ---------------------------------------------------------------- Distortion

class Distortion final : public Effect {
public:
    explicit Distortion(u32 rate) : Effect(EffectKind::Distortion, rate) {}
    void reset() override { lp_[0] = lp_[1] = 0.0f; }
    void process(f32* io, u32 frames, const f32*) override {
        const i32 mode = static_cast<i32>(param(0) + 0.5f);
        const f32 pre = dbToLin(param(1)), post = dbToLin(param(4)), drive = param(3);
        const bool all = param(2) >= 20499.0f;      // keep_hf_hz at its maximum: everything is distorted
        const f32 a = std::exp(-2.0f * kPi * clampf(param(2), 1.0f, 20500.0f) / static_cast<f32>(rate_));
        for (u32 i = 0; i < frames; ++i)
            for (u32 ch = 0; ch < 2; ++ch) {
                const f32 x = io[2 * i + ch];
                lp_[ch] = all ? x : undenorm(lp_[ch] * a + x * (1.0f - a));   // the part below keep_hf_hz is distorted; the rest passes
                const f32 hf = x - lp_[ch];
                io[2 * i + ch] = shape(lp_[ch] * pre, mode, drive) * post + hf;
            }
    }
    static f32 shape(f32 x, i32 mode, f32 d) {
        switch (mode) {
            case 0: { const f32 t = 1.0f - 0.95f * d; return clampf(x, -t, t) / t; }                                      // clip
            case 1: { const f32 k = 1.0f + 4.0f * d; return std::atan(x * k * (kPi * 0.5f)) / (kPi * 0.5f); }            // atan
            case 2: { const f32 levels = std::pow(2.0f, 15.0f - d * 14.0f); return std::round(clampf(x, -1.0f, 1.0f) * levels) / levels; }   // lo-fi
            case 3: return std::tanh(x * (1.0f + 6.0f * d));                                                              // overdrive
            default: { const f32 k = 2.0f * d / (1.0001f - d); const f32 c = clampf(x, -1.0f, 1.0f); return (1.0f + k) * c / (1.0f + k * std::fabs(c)); }   // wave shape
        }
    }
private:
    f32 lp_[2] = {};
};

// ---------------------------------------------------------------- Phaser

class Phaser final : public Effect {
public:
    explicit Phaser(u32 rate) : Effect(EffectKind::Phaser, rate) {}
    void reset() override { for (auto& c : z_) for (f32& s : c) s = 0.0f; fb_[0] = fb_[1] = 0.0f; }
    void process(f32* io, u32 frames, const f32*) override {
        const f32 lo = std::min(param(0), param(1)), hi = std::max(param(0), param(1));
        const f32 step = param(2) / static_cast<f32>(rate_), feedback = param(3), depth = param(4);
        for (u32 i = 0; i < frames; ++i) {
            phase_ += step;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            const f32 f = lo + (hi - lo) * (0.5f + 0.5f * std::sin(2.0f * kPi * phase_));
            const f32 t = std::tan(kPi * clampf(f, 5.0f, 0.45f * static_cast<f32>(rate_)) / static_cast<f32>(rate_));
            const f32 c = (t - 1.0f) / (t + 1.0f);
            for (u32 ch = 0; ch < 2; ++ch) {
                const f32 x = io[2 * i + ch];
                f32 y = x + fb_[ch] * feedback;
                for (u32 s = 0; s < 6; ++s) {
                    const f32 o = c * y + z_[ch][s];
                    z_[ch][s] = undenorm(y - c * o);
                    y = o;
                }
                fb_[ch] = undenorm(y);
                io[2 * i + ch] = x + y * depth * 0.5f;
            }
        }
    }
private:
    f32 phase_ = 0.0f;
    f32 z_[2][6] = {};
    f32 fb_[2] = {};
};

// ---------------------------------------------------------------- PitchShift

// Two read heads sweeping a delay line, cross-faded with sin^2 windows. A time-domain shifter: Godot's is FFT-based, so this one
// is approximate (some modulation on sustained tones) but needs no transform and stays in tune.
class PitchShift final : public Effect {
public:
    explicit PitchShift(u32 rate) : Effect(EffectKind::PitchShift, rate) {
        size_ = 8192 + 16;
        buf_.assign(static_cast<usize>(size_) * 2, 0.0f);
    }
    void reset() override { std::fill(buf_.begin(), buf_.end(), 0.0f); w_ = 0; phase_ = 0.0f; }
    u32 latencyFrames() const override { return 0; }
    void process(f32* io, u32 frames, const f32*) override {
        const f32 ratio = param(0);
        static const u32 kSizes[5] = {256, 512, 1024, 2048, 4096};
        const f32 win = static_cast<f32>(kSizes[static_cast<u32>(clampf(param(2), 0.0f, 4.0f))]);
        const f32 dphase = (1.0f - ratio) / win;
        for (u32 i = 0; i < frames; ++i) {
            buf_[2 * w_] = io[2 * i];
            buf_[2 * w_ + 1] = io[2 * i + 1];
            f32 outL = 0.0f, outR = 0.0f;
            for (int h = 0; h < 2; ++h) {
                f32 p = phase_ + 0.5f * static_cast<f32>(h);
                p -= std::floor(p);
                const f32 g = std::sin(kPi * p);
                const f32 d = 1.0f + p * win;            // delay in samples, 1 .. win+1
                const f32 pos = static_cast<f32>(w_) - d;
                const f32 pf = std::floor(pos);
                const f32 fr = pos - pf;
                const i32 i0 = ((static_cast<i32>(pf) % static_cast<i32>(size_)) + static_cast<i32>(size_)) % static_cast<i32>(size_);
                const i32 i1 = (i0 + 1) % static_cast<i32>(size_);
                outL += g * g * (buf_[2 * i0] * (1.0f - fr) + buf_[2 * i1] * fr);
                outR += g * g * (buf_[2 * i0 + 1] * (1.0f - fr) + buf_[2 * i1 + 1] * fr);
            }
            phase_ += dphase;
            phase_ -= std::floor(phase_);
            io[2 * i] = outL;
            io[2 * i + 1] = outR;
            if (++w_ >= size_) w_ = 0;
        }
    }
private:
    u32 size_ = 0, w_ = 0;
    std::vector<f32> buf_;
    f32 phase_ = 0.0f;
};

// ---------------------------------------------------------------- Reverb

class ReverbFx final : public Effect {
public:
    explicit ReverbFx(u32 rate) : Effect(EffectKind::Reverb, rate) {
        rv_.init(rate);
        preSize_ = static_cast<u32>(rate / 2) + 8;
        pre_.assign(preSize_, 0.0f);
        send_.assign(kChunk, 0.0f);
        l_.assign(kChunk, 0.0f);
        r_.assign(kChunk, 0.0f);
    }
    void reset() override { rv_.reset(); std::fill(pre_.begin(), pre_.end(), 0.0f); pw_ = 0; hp_ = 0.0f; }
    void process(f32* io, u32 frames, const f32*) override {
        const u32 preN = std::min(preSize_ - 1, static_cast<u32>(param(0) * 0.001f * static_cast<f32>(rate_)));
        const f32 preFb = param(1), room = param(2), damp = param(3), spread = param(4), hipass = param(5), dry = param(6), wet = param(7);
        rv_.setParams(0.2f + 5.8f * room, damp);
        const f32 hpA = std::exp(-2.0f * kPi * (20.0f + hipass * 980.0f) / static_cast<f32>(rate_));
        u32 done = 0;
        while (done < frames) {
            const u32 n = std::min(kChunk, frames - done);
            for (u32 i = 0; i < n; ++i) {
                const f32 in = 0.5f * (io[2 * (done + i)] + io[2 * (done + i) + 1]);
                const u32 rd = (pw_ + preSize_ - preN) % preSize_;
                const f32 delayed = pre_[rd];
                pre_[pw_] = in + delayed * preFb;
                if (++pw_ >= preSize_) pw_ = 0;
                f32 x = preN > 0 ? delayed : in;
                hp_ = undenorm(hp_ * hpA + x * (1.0f - hpA));
                send_[i] = hipass > 0.0f ? x - hp_ : x;
            }
            rv_.process(send_.data(), l_.data(), r_.data(), n);
            for (u32 i = 0; i < n; ++i) {
                const f32 mid = 0.5f * (l_[i] + r_[i]);
                const f32 wl = mid + (l_[i] - mid) * spread, wr = mid + (r_[i] - mid) * spread;
                io[2 * (done + i)] = io[2 * (done + i)] * dry + wl * wet;
                io[2 * (done + i) + 1] = io[2 * (done + i) + 1] * dry + wr * wet;
            }
            done += n;
        }
    }
private:
    Reverb rv_;
    u32 preSize_ = 0, pw_ = 0;
    std::vector<f32> pre_, send_, l_, r_;
    f32 hp_ = 0.0f;
};

// ---------------------------------------------------------------- Convolution reverb

// dry * input + wet * (input convolved with the impulse). The impulse comes from a decoded sound (setImpulse); with none loaded the wet
// signal is silent. The wet path lags the dry by one partition (latencyFrames), which reads as a few ms of predelay.
class ConvolutionFx final : public Effect {
public:
    explicit ConvolutionFx(u32 rate) : Effect(EffectKind::ConvolutionReverb, rate), conv_(rate) {
        inL_.assign(kChunk, 0.0f); inR_.assign(kChunk, 0.0f);
        wetL_.assign(kChunk, 0.0f); wetR_.assign(kChunk, 0.0f);
    }
    bool setImpulse(const SoundData& ir) override { return conv_.setImpulse(ir); }
    void clearImpulse() override { conv_.clearImpulse(); }
    bool hasImpulse() const override { return conv_.hasImpulse(); }
    void collect() override { conv_.collect(); }
    void adoptFrom(const Effect& old) override {
        if (old.kind() == EffectKind::ConvolutionReverb) conv_.adoptImpulse(static_cast<const ConvolutionFx&>(old).conv_);
    }
    u32 latencyFrames() const override { return conv_.latencyFrames(); }
    void reset() override { conv_.reset(); }
    void process(f32* io, u32 frames, const f32*) override {
        conv_.setPredelayFrames(static_cast<u32>(param(2) * 0.001f * static_cast<f32>(rate_)));
        const f32 dry = param(0), wet = param(1) * dbToLin(param(3));
        u32 done = 0;
        while (done < frames) {
            const u32 n = std::min(kChunk, frames - done);
            f32* p = io + 2 * static_cast<usize>(done);
            for (u32 i = 0; i < n; ++i) { inL_[i] = p[2 * i]; inR_[i] = p[2 * i + 1]; }
            conv_.process(inL_.data(), inR_.data(), wetL_.data(), wetR_.data(), n);
            for (u32 i = 0; i < n; ++i) {
                p[2 * i] = inL_[i] * dry + wetL_[i] * wet;
                p[2 * i + 1] = inR_[i] * dry + wetR_[i] * wet;
            }
            done += n;
        }
    }
private:
    Convolver conv_;
    std::vector<f32> inL_, inR_, wetL_, wetR_;
};

// ---------------------------------------------------------------- Panner and StereoEnhance

class Panner final : public Effect {
public:
    explicit Panner(u32 rate) : Effect(EffectKind::Panner, rate) {}
    void process(f32* io, u32 frames, const f32*) override {
        const f32 pan = param(0);
        const f32 gl = std::min(1.0f, 1.0f - pan), gr = std::min(1.0f, 1.0f + pan);
        for (u32 i = 0; i < frames; ++i) { io[2 * i] *= gl; io[2 * i + 1] *= gr; }
    }
};

class StereoEnhance final : public Effect {
public:
    explicit StereoEnhance(u32 rate) : Effect(EffectKind::StereoEnhance, rate) {
        size_ = static_cast<u32>(rate / 10) + 8;
        buf_.assign(size_, 0.0f);
    }
    void reset() override { std::fill(buf_.begin(), buf_.end(), 0.0f); w_ = 0; }
    void process(f32* io, u32 frames, const f32*) override {
        const f32 wide = param(0), surround = param(2);
        const u32 n = std::min(size_ - 1, static_cast<u32>(param(1) * 0.001f * static_cast<f32>(rate_)));
        for (u32 i = 0; i < frames; ++i) {
            const f32 l = io[2 * i], r = io[2 * i + 1];
            const f32 mid = 0.5f * (l + r), side = 0.5f * (l - r) * wide * (1.0f + surround);
            buf_[w_] = mid - side;
            const f32 rDelayed = buf_[(w_ + size_ - n) % size_];
            io[2 * i] = mid + side;
            io[2 * i + 1] = rDelayed;
            if (++w_ >= size_) w_ = 0;
        }
    }
private:
    u32 size_ = 0, w_ = 0;
    std::vector<f32> buf_;
};

// ---------------------------------------------------------------- Capture and SpectrumAnalyzer

// Audio thread writes, game thread reads; frames that do not fit are dropped and counted (Godot's behaviour).
class Capture final : public Effect {
public:
    // The ring holds two seconds; buffer_length (up to that) is how much of it may be queued.
    explicit Capture(u32 rate) : Effect(EffectKind::Capture, rate) {
        ring_n_ = std::max<u32>(128, rate * 2);
        ring_.assign(static_cast<usize>(ring_n_) * 2, 0.0f);
    }
    void process(f32* io, u32 frames, const f32*) override {
        const u64 h = head_.load(std::memory_order_relaxed), t = tail_.load(std::memory_order_acquire);
        const u32 queued = static_cast<u32>(h - t), cap = captureCapacity();
        const u32 room = queued < cap ? cap - queued : 0;
        const u32 n = std::min(room, frames);
        for (u32 i = 0; i < n; ++i) {
            const usize s = static_cast<usize>((h + i) % ring_n_) * 2;
            ring_[s] = io[2 * i];
            ring_[s + 1] = io[2 * i + 1];
        }
        if (n < frames) discarded_.fetch_add(frames - n, std::memory_order_relaxed);
        head_.store(h + n, std::memory_order_release);
    }
    u32 captureAvailable() const override { return static_cast<u32>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed)); }
    u32 captureCapacity() const override { return std::min(ring_n_, std::max<u32>(64, static_cast<u32>(param(0) * static_cast<f32>(rate_)))); }
    u32 captureDiscarded() const override { return discarded_.load(std::memory_order_relaxed); }
    u32 captureRead(f32* dst, u32 maxFrames) override {
        const u32 n = std::min(maxFrames, captureAvailable());
        const u64 t = tail_.load(std::memory_order_relaxed);
        for (u32 i = 0; i < n; ++i) {
            const usize s = static_cast<usize>((t + i) % ring_n_) * 2;
            dst[2 * i] = ring_[s];
            dst[2 * i + 1] = ring_[s + 1];
        }
        tail_.store(t + n, std::memory_order_release);
        return n;
    }
    void captureClear() override { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }
private:
    u32 ring_n_ = 0;
    std::vector<f32> ring_;
    std::atomic<u64> head_{0}, tail_{0};
    std::atomic<u32> discarded_{0};
};

void fft(std::vector<std::complex<f64>>& a) {
    const usize n = a.size();
    for (usize i = 1, j = 0; i < n; ++i) {
        usize bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (usize len = 2; len <= n; len <<= 1) {
        const f64 ang = -2.0 * 3.14159265358979323846 / static_cast<f64>(len);
        const std::complex<f64> wl(std::cos(ang), std::sin(ang));
        for (usize i = 0; i < n; i += len) {
            std::complex<f64> w(1.0, 0.0);
            for (usize k = 0; k < len / 2; ++k) {
                const std::complex<f64> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

class Spectrum final : public Effect {
public:
    explicit Spectrum(u32 rate) : Effect(EffectKind::SpectrumAnalyzer, rate) {
        cap_ = static_cast<u32>(4.0f * static_cast<f32>(rate)) + 8192;   // the longest buffer_length plus the largest fft
        ring_.assign(static_cast<usize>(cap_) * 2, 0.0f);
    }
    void process(f32* io, u32 frames, const f32*) override {
        u64 h = head_.load(std::memory_order_relaxed);
        for (u32 i = 0; i < frames; ++i, ++h) {
            const usize s = static_cast<usize>(h % cap_) * 2;
            ring_[s] = io[2 * i];
            ring_[s + 1] = io[2 * i + 1];
        }
        head_.store(h, std::memory_order_release);
    }
    bool spectrum(f32 fromHz, f32 toHz, i32 mode, f32& outL, f32& outR) const override {
        outL = outR = 0.0f;
        static const u32 kSizes[5] = {256, 512, 1024, 2048, 4096};
        const u32 n = kSizes[static_cast<u32>(clampf(param(1), 0.0f, 4.0f))];
        const u32 back = static_cast<u32>(param(2) * static_cast<f32>(rate_));
        const u64 h = head_.load(std::memory_order_acquire);
        if (h < static_cast<u64>(n) + back || n + back >= cap_) return false;
        const u64 start = h - back - n;
        f32* out[2] = {&outL, &outR};
        std::vector<std::complex<f64>> a(n);
        f64 wsum = 0.0;
        for (u32 i = 0; i < n; ++i) wsum += 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / n);
        const f32 binHz = static_cast<f32>(rate_) / static_cast<f32>(n);
        const u32 k0 = std::min(n / 2, static_cast<u32>(std::max(0.0f, std::floor(fromHz / binHz))));
        const u32 k1 = std::min(n / 2, std::max(k0, static_cast<u32>(std::ceil(toHz / binHz))));
        for (u32 ch = 0; ch < 2; ++ch) {
            for (u32 i = 0; i < n; ++i) {
                const f64 w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / n);
                a[i] = std::complex<f64>(ring_[static_cast<usize>((start + i) % cap_) * 2 + ch] * w, 0.0);
            }
            fft(a);
            f64 sum = 0.0, mx = 0.0;
            u32 cnt = 0;
            for (u32 k = k0; k <= k1; ++k) {
                const f64 m = std::abs(a[k]) * 2.0 / wsum;
                sum += m;
                mx = std::max(mx, m);
                ++cnt;
            }
            *out[ch] = static_cast<f32>(mode == 1 ? mx : (cnt ? sum / cnt : 0.0));
        }
        return true;
    }
private:
    u32 cap_ = 0;
    std::vector<f32> ring_;
    std::atomic<u64> head_{0};
};

} // namespace

// ---------------------------------------------------------------- Godot's filter maths

BiquadCoeffs godotFilterCoeffs(FilterMode mode, f32 cutoffHz, f32 resonance, f32 gain, u32 stages, f32 sampleRate) {
    f64 fc = cutoffHz;
    const f64 lim = std::floor(sampleRate * 0.5) + 512.0;
    if (fc > lim) fc = lim;
    if (fc > 0.499 * sampleRate) fc = 0.499 * sampleRate;   // keep the section stable
    if (fc < 1.0) fc = 1.0;
    f64 Q = resonance <= 0.0f ? 0.0001 : resonance;
    if (mode == FilterMode::BandPass) Q *= 2.0;
    else if (mode == FilterMode::Peak) Q *= 3.0;
    f64 G = gain < 0.001f ? 0.001 : gain;
    if (stages > 1) {
        if (Q > 1.0) Q = std::pow(Q, 1.0 / stages);
        G = std::pow(G, 1.0 / (stages + 1));
    }
    const f64 w = 2.0 * 3.14159265358979323846 * fc / sampleRate;
    const f64 cw = std::cos(w), sw = std::sin(w);
    const f64 alpha = sw / (2.0 * Q);
    f64 a0 = 1.0 + alpha, b0, b1, b2, a1, a2;
    switch (mode) {
        case FilterMode::LowPass:  b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a1 = -2 * cw; a2 = 1 - alpha; break;
        case FilterMode::HighPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a1 = -2 * cw; a2 = 1 - alpha; break;
        case FilterMode::BandPass: b0 = alpha * std::sqrt(Q + 1); b1 = 0; b2 = -alpha * std::sqrt(Q + 1); a1 = -2 * cw; a2 = 1 - alpha; break;
        case FilterMode::Notch:    b0 = 1; b1 = -2 * cw; b2 = 1; a1 = -2 * cw; a2 = 1 - alpha; break;
        case FilterMode::Peak:     b0 = 1 + alpha * G; b1 = -2 * cw; b2 = 1 - alpha * G; a0 = 1 + alpha / G; a1 = -2 * cw; a2 = 1 - alpha / G; break;
        case FilterMode::LowShelf: case FilterMode::HighShelf: {
            const f64 A = G;
            const f64 sq = std::max(0.001, std::sqrt(Q));
            const f64 beta = std::sqrt(A) / sq;
            if (mode == FilterMode::LowShelf) {
                a0 = (A + 1) + (A - 1) * cw + beta * sw;
                b0 = A * ((A + 1) - (A - 1) * cw + beta * sw);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - beta * sw);
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - beta * sw;
            } else {
                a0 = (A + 1) - (A - 1) * cw + beta * sw;
                b0 = A * ((A + 1) + (A - 1) * cw + beta * sw);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - beta * sw);
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - beta * sw;
            }
            break;
        }
        default: b0 = 1; b1 = b2 = a1 = a2 = 0; break;
    }
    BiquadCoeffs c;
    c.b0 = static_cast<f32>(b0 / a0);
    c.b1 = static_cast<f32>(b1 / a0);
    c.b2 = static_cast<f32>(b2 / a0);
    c.a1 = static_cast<f32>(a1 / a0);
    c.a2 = static_cast<f32>(a2 / a0);
    return c;
}

f32 godotFilterMagnitude(FilterMode mode, f32 cutoffHz, f32 resonance, f32 gain, u32 stages, f32 sampleRate, f32 freqHz) {
    const BiquadCoeffs c = godotFilterCoeffs(mode, cutoffHz, resonance, gain, stages, sampleRate);
    return std::pow(biquadMagnitude(c, freqHz, sampleRate), static_cast<f32>(stages < 1 ? 1 : stages));
}

f32 compressorOutputDb(f32 inputDb, f32 thresholdDb, f32 ratio) {
    if (inputDb <= thresholdDb) return inputDb;
    return thresholdDb + (inputDb - thresholdDb) / (ratio < 1.0f ? 1.0f : ratio);
}

// ---------------------------------------------------------------- Effect

Effect::Effect(EffectKind kind, u32 rate) : kind_(kind), rate_(rate) {
    const auto& t = tables().t[static_cast<usize>(kind)];
    bind(t.data(), static_cast<u32>(t.size()));
}

void Effect::bind(const ParamInfo* table, u32 count) {
    table_ = table;
    count_ = count > kMaxParams ? kMaxParams : count;
    resetParams();
}

void Effect::resetParams() {
    for (u32 i = 0; i < count_; ++i) values_[i].store(table_[i].def, std::memory_order_relaxed);
}

i32 Effect::findParam(const char* name) const {
    if (!name) return -1;
    for (u32 i = 0; i < count_; ++i)
        if (std::strcmp(table_[i].name, name) == 0) return static_cast<i32>(i);
    return -1;
}

void Effect::setParam(u32 index, f32 value) {
    if (index >= count_) return;
    values_[index].store(clampf(value, table_[index].lo, table_[index].hi), std::memory_order_relaxed);
}

const char* Effect::godotClassName(EffectKind k) {
    return k < EffectKind::Count ? kClassNames[static_cast<usize>(k)] : "";
}

EffectKind Effect::kindFromClassName(const char* name) {
    if (!name) return EffectKind::Count;
    for (usize i = 0; i < static_cast<usize>(EffectKind::Count); ++i)
        if (std::strcmp(kClassNames[i], name) == 0) return static_cast<EffectKind>(i);
    return EffectKind::Count;
}

std::unique_ptr<Effect> Effect::create(EffectKind kind, u32 rate) {
    if (rate == 0) return nullptr;
    switch (kind) {
        case EffectKind::Amplify: return std::make_unique<Amplify>(rate);
        case EffectKind::Delay: return std::make_unique<Delay>(rate);
        case EffectKind::Chorus: return std::make_unique<Chorus>(rate);
        case EffectKind::Compressor: return std::make_unique<Compressor>(rate);
        case EffectKind::Limiter: return std::make_unique<Limiter>(rate);
        case EffectKind::HardLimiter: return std::make_unique<HardLimiter>(rate);
        case EffectKind::EQ6: return std::make_unique<Equalizer>(kind, kEq6, 6, 1.64f, rate);
        case EffectKind::EQ10: return std::make_unique<Equalizer>(kind, kEq10, 10, 1.0f, rate);
        case EffectKind::EQ21: return std::make_unique<Equalizer>(kind, kEq21, 21, 0.5f, rate);
        case EffectKind::LowPass: return std::make_unique<FilterFx>(kind, FilterMode::LowPass, rate);
        case EffectKind::HighPass: return std::make_unique<FilterFx>(kind, FilterMode::HighPass, rate);
        case EffectKind::BandPass: return std::make_unique<FilterFx>(kind, FilterMode::BandPass, rate);
        case EffectKind::Notch: return std::make_unique<FilterFx>(kind, FilterMode::Notch, rate);
        case EffectKind::LowShelf: return std::make_unique<FilterFx>(kind, FilterMode::LowShelf, rate);
        case EffectKind::HighShelf: return std::make_unique<FilterFx>(kind, FilterMode::HighShelf, rate);
        case EffectKind::Distortion: return std::make_unique<Distortion>(rate);
        case EffectKind::Phaser: return std::make_unique<Phaser>(rate);
        case EffectKind::PitchShift: return std::make_unique<PitchShift>(rate);
        case EffectKind::Reverb: return std::make_unique<ReverbFx>(rate);
        case EffectKind::ConvolutionReverb: return std::make_unique<ConvolutionFx>(rate);
        case EffectKind::Panner: return std::make_unique<Panner>(rate);
        case EffectKind::StereoEnhance: return std::make_unique<StereoEnhance>(rate);
        case EffectKind::Capture: return std::make_unique<Capture>(rate);
        case EffectKind::SpectrumAnalyzer: return std::make_unique<Spectrum>(rate);
        default: return nullptr;
    }
}

} // namespace aver::audio
