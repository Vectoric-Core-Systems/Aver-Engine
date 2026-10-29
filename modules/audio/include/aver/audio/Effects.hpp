#pragma once
// Bus effects: the Godot AudioEffect* set implemented in the mixer's own DSP (no library). Every effect works in place on an
// interleaved stereo block, allocates only when it is made, and keeps its parameters as atomics so the game thread can change them
// while the audio thread runs. Parameter names, defaults and ranges are Godot's (docs/AUDIO.md section "Bus graph").
// Used by BusGraph; nothing here knows about voices or devices.
#include "aver/audio/Dsp.hpp"
#include "aver/audio/Sound.hpp"
#include "aver/core/Types.hpp"

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace aver::audio {

enum class EffectKind : u32 {
    Amplify = 0, Delay, Chorus, Compressor, Limiter, HardLimiter, EQ6, EQ10, EQ21,
    LowPass, HighPass, BandPass, Notch, LowShelf, HighShelf,
    Distortion, Phaser, PitchShift, Reverb, Panner, StereoEnhance, Capture, SpectrumAnalyzer,
    ConvolutionReverb,
    Count
};

inline f32 dbToLin(f32 db) { return db <= -200.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }
inline f32 linToDb(f32 lin) { return lin <= 1.0e-10f ? -200.0f : 20.0f * std::log10(lin); }

// One named parameter: Godot's property name, its default and the range it is clamped to.
struct ParamInfo {
    const char* name;
    f32 def, lo, hi;
};

// Godot's filter modes (AudioFilterSW) and the coefficients it computes for them, in the engine's biquad layout.
enum class FilterMode : u32 { LowPass = 0, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf };
// `stages` > 1 cascades the same section; Godot lowers Q (to Q^(1/stages)) and the gain (to gain^(1/(stages+1))) to keep the total response.
BiquadCoeffs godotFilterCoeffs(FilterMode mode, f32 cutoffHz, f32 resonance, f32 gain, u32 stages, f32 sampleRate);
// |H| of `stages` identical sections at freqHz.
f32 godotFilterMagnitude(FilterMode mode, f32 cutoffHz, f32 resonance, f32 gain, u32 stages, f32 sampleRate, f32 freqHz);

// The compressor's static curve: the level (dB) it settles to for a steady input level.
f32 compressorOutputDb(f32 inputDb, f32 thresholdDb, f32 ratio);

class Effect {
public:
    virtual ~Effect() = default;

    EffectKind kind() const { return kind_; }
    // "AudioEffectReverb" ...
    static const char* godotClassName(EffectKind k);
    // EffectKind from a Godot class name; Count when unknown.
    static EffectKind kindFromClassName(const char* name);

    static std::unique_ptr<Effect> create(EffectKind kind, u32 sampleRate);

    u32 paramCount() const { return count_; }
    const ParamInfo& paramInfo(u32 i) const { return table_[i]; }
    // -1 when the effect has no parameter of that name.
    i32 findParam(const char* name) const;
    // Clamped to the parameter's range.
    void setParam(u32 index, f32 value);
    f32 param(u32 index) const { return values_[index].load(std::memory_order_relaxed); }
    void setParamByName(const char* name, f32 value) { const i32 i = findParam(name); if (i >= 0) setParam(static_cast<u32>(i), value); }
    f32 paramByName(const char* name, f32 fallback = 0.0f) const { const i32 i = findParam(name); return i >= 0 ? param(static_cast<u32>(i)) : fallback; }
    void resetParams();

    // In place on `frames` interleaved stereo frames. `sidechain` is another bus's block (or null); only the compressor reads it.
    virtual void process(f32* io, u32 frames, const f32* sidechain) = 0;
    // Clears the effect's memory (delay lines, filter state).
    virtual void reset() {}
    // Frames of latency the effect adds (the hard limiter's look-ahead, the convolution reverb's partition).
    virtual u32 latencyFrames() const { return 0; }

    // ---- impulse-response effects (ConvolutionReverb); other kinds answer false / zero ----
    // Game thread: builds the effect's impulse from a decoded sound (any rate; resampled) and hands it to the audio thread, which crossfades.
    virtual bool setImpulse(const SoundData& ir) { (void)ir; return false; }
    virtual void clearImpulse() {}
    virtual bool hasImpulse() const { return false; }
    // Game thread: frees what the audio thread has finished with.
    virtual void collect() {}
    // Game thread, on a device rate change: takes the impulse source of the effect this one replaces, so it is rebuilt for the new rate.
    virtual void adoptFrom(const Effect& old) { (void)old; }

    // ---- readable effects (Capture, SpectrumAnalyzer); other kinds answer zero ----
    virtual u32  captureAvailable() const { return 0; }
    // Pops up to maxFrames stereo frames (interleaved) into dst; returns the frames written.
    virtual u32  captureRead(f32* dst, u32 maxFrames) { (void)dst; (void)maxFrames; return 0; }
    virtual u32  captureCapacity() const { return 0; }
    virtual void captureClear() {}
    virtual u32  captureDiscarded() const { return 0; }
    // Magnitude (linear, a full-scale sine reads about its amplitude) over [fromHz, toHz]: mode 0 average of the bins, 1 maximum.
    virtual bool spectrum(f32 fromHz, f32 toHz, i32 mode, f32& outL, f32& outR) const { (void)fromHz; (void)toHz; (void)mode; outL = outR = 0.0f; return false; }

    // The bus whose block drives a compressor's detector; -1 none. Set by BusGraph when links are resolved.
    std::atomic<i32> sidechainBus{-1};
    // The block-start gain/parameter smoothing state is the effect's own; this is the sample rate it was built for.
    u32 sampleRate() const { return rate_; }

protected:
    Effect(EffectKind kind, u32 rate);
    void bind(const ParamInfo* table, u32 count);

    EffectKind kind_;
    u32 rate_;
    const ParamInfo* table_ = nullptr;
    u32 count_ = 0;
    static constexpr u32 kMaxParams = 48;
    std::atomic<f32> values_[kMaxParams];
};

} // namespace aver::audio
