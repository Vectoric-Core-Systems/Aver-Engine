#pragma once
// A uniformly partitioned FFT convolver (overlap-save, own radix-2 FFT, no dependency) for convolution reverb: stereo in, stereo out, one
// impulse response of one or two channels. Everything the audio thread touches is allocated when the impulse is set (game thread), so
// process() never allocates, locks or blocks; a new impulse is handed over through an atomic pointer and crossfaded in over one block.
//
// Latency is exactly one partition: out[n] = (in * ir)[n - blockFrames()]. A longer impulse costs more partitions, not more latency.
#include "aver/audio/Sound.hpp"
#include "aver/core/Types.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace aver::audio {

// In-place complex FFT of a fixed power-of-two size on split real / imaginary arrays; the tables are built in the constructor.
class Fft {
public:
    explicit Fft(u32 n);
    u32 size() const { return n_; }
    void forward(f32* re, f32* im) const;
    // Includes the 1/n scale, so inverse(forward(x)) == x.
    void inverse(f32* re, f32* im) const;

private:
    void run(f32* re, f32* im, bool inv) const;
    u32 n_;
    std::vector<f32> cos_, sin_;
    std::vector<u32> rev_;
};

// Resamples interleaved audio from srcRate to dstRate by linear interpolation (a copy when the rates are equal). Game thread.
std::vector<f32> resampleInterleaved(const f32* in, u32 frames, u32 channels, u32 srcRate, u32 dstRate);

class Convolver {
public:
    static constexpr u32 kDefaultBlock = 256;       // frames per partition (power of two, 32 .. 8192)
    static constexpr f32 kMaxImpulseSeconds = 12.0f;  // longer impulses are truncated
    static constexpr f32 kMaxPredelaySeconds = 0.5f;

    // Allocates the FFT tables, the block buffers and the predelay line for a device rate. Game thread.
    explicit Convolver(u32 sampleRate, u32 blockFrames = kDefaultBlock);
    ~Convolver();
    Convolver(const Convolver&) = delete;
    Convolver& operator=(const Convolver&) = delete;

    u32 blockFrames() const { return block_; }
    u32 latencyFrames() const { return block_; }
    u32 sampleRate() const { return rate_; }

    // ---- game thread ----
    //
    // Builds the partitioned spectra of an impulse (resampled to the convolver's rate) and hands it to the audio thread, which crossfades to
    // it at the next block. 1 channel: both ears get the same impulse; 2 channels: left input with channel 0, right input with channel 1.
    // False for empty or malformed input. The source is kept so a rate change can rebuild it.
    bool setImpulse(const f32* interleaved, u32 frames, u32 channels, u32 srcRate);
    bool setImpulse(const SoundData& s) { return setImpulse(s.samples.data(), s.frames(), s.channels, s.sampleRate); }
    // Fades the wet output to silence.
    void clearImpulse();
    bool hasImpulse() const { return has_.load(std::memory_order_acquire); }
    // Takes another convolver's impulse source (kept at its own rate) and builds it for this one's rate. False when it has none.
    bool adoptImpulse(const Convolver& other);
    // Frames of the impulse after resampling and truncation (0 when none).
    u32  impulseFrames() const { return irFrames_.load(std::memory_order_relaxed); }
    u32  partitions() const { return parts_.load(std::memory_order_relaxed); }
    // Predelay of the wet signal, frames (clamped to kMaxPredelaySeconds). A latest-value atomic; a change jumps the delay tap.
    void setPredelayFrames(u32 frames);
    u32  predelayFrames() const { return predelay_.load(std::memory_order_relaxed); }
    // Frees the engines the audio thread has finished with. Also done by setImpulse and the destructor.
    void collect();
    // A new device rate: rebuilds the impulse from the kept source and clears all history. Allocates; the audio thread must not be running.
    void setSampleRate(u32 rate);

    // ---- audio thread ----
    //
    // Convolves `frames` frames (any count, any chunking: the output is the same samples). The wet signal only; in and out may alias.
    void process(const f32* inL, const f32* inR, f32* outL, f32* outR, u32 frames);
    // Zeroes the history (the impulse stays). Audio thread, or when the audio thread is stopped.
    void reset();

private:
    struct Engine;
    static constexpr u32 kRetireSlots = 4;

    std::unique_ptr<Engine> build(const f32* ir, u32 frames, u32 channels) const;
    void computeBlock();
    void runEngine(Engine& e, bool inputZero, f32* outL, f32* outR);
    void takePending();
    void retire(Engine* e);

    u32 rate_, block_, fftSize_, bins_;
    Fft fft_;

    std::atomic<Engine*> pending_{nullptr};
    std::atomic<Engine*> retired_[kRetireSlots];
    Engine* active_ = nullptr;            // audio thread
    Engine* fading_ = nullptr;            // audio thread: leaves with the block being computed
    std::atomic<bool> has_{false};
    std::atomic<u32> irFrames_{0}, parts_{0};

    // The kept source for a rate change (game thread).
    std::vector<f32> src_;
    u32 srcChannels_ = 0, srcRate_ = 0;

    // Block state (audio thread).
    u32 pos_ = 0;
    std::vector<f32> inL_, inR_, prevL_, prevR_, outL_, outR_, fadeL_, fadeR_;
    std::vector<f32> re_, im_, xLr_, xLi_, xRr_, xRi_, yLr_, yLi_, yRr_, yRi_;

    // Predelay line (stereo ring).
    std::atomic<u32> predelay_{0};
    std::vector<f32> preL_, preR_;
    u32 preSize_ = 0, prePos_ = 0;
};

} // namespace aver::audio
