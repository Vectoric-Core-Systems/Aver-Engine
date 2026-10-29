#pragma once
// Sound graphs: the evaluator behind a .ocsnd, on the audio thread and offline, as ONE piece of code.
//
// A SoundProgram is a graph compiled once from plain data (the format layer converts a .ocsnd into a
// SoundGraphDesc; this module reads no files). A SoundRunner is one playing instance of it: every byte
// of per-node state is allocated when the runner is constructed, on the game thread, and process() then
// only does arithmetic -- no allocation, lock, log or managed call, which is Mixer.hpp's rule for mix().
//
// ONE EVALUATOR. Aver.Sound's offline renderSound() constructs a runner and calls process() once for the
// whole sound; the mixer calls it a block at a time. The loop is per sample and every state lives in the
// runner, so how the frames are chunked changes nothing: the two paths produce identical samples for a
// graph with no live inputs (tests/audio/SoundGraphTest pins it).
//
// LIVE INPUTS are latest-value atomics, the mixer's own pattern: setParam() stores, process() snapshots
// them at the start of a block and the Parameter node smooths toward the snapshot per sample, so a jump
// from the game thread is a ramp on the audio thread. noteOff() sets a flag the runner latches at the
// next block; an Adsr then releases from wherever its envelope is.
#include "aver/audio/Sound.hpp"
#include "aver/core/Types.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace aver::audio {

// Numerically identical to fmt::OcSoundNodeKind (the format file stores these values). Aver.Sound
// static_asserts the correspondence.
enum class SoundNodeKind : u32 {
    Sine = 0, Saw, Square, Noise, Const, Gain, LowPass, Adsr, Mix, Multiply,
    Parameter = 10, Lfo, HighPass, BandPass, Random, Crossfade, WavePlayer, Delay, Clamp, MapRange, EnvelopeFollower,
    Count
};

inline constexpr u32 kSoundMaxInputs     = 9;      // Crossfade: a position input and up to eight signals
inline constexpr u32 kSoundMaxCrossfade  = 8;
inline constexpr u32 kSoundMaxNodes      = 256;
inline constexpr u32 kSoundMaxParams     = 32;     // distinct Parameter names in one graph
inline constexpr u32 kSoundMaxWaves      = 8;      // distinct WavePlayer assets in one graph
inline constexpr f32 kSoundMaxDelaySec   = 10.0f;  // one Delay node
inline constexpr f32 kSoundMaxDelayTotal = 30.0f;  // all Delay nodes of one graph together (the runner pre-allocates it)
inline constexpr f32 kSoundMinTailSec    = 0.01f;  // a note-off on a graph with no Adsr still fades out over this

// How many inputs a kind consumes; an unconnected input reads 0.
u32 soundNodeInputCount(SoundNodeKind k);

struct SoundNodeDesc {
    SoundNodeKind kind = SoundNodeKind::Sine;
    f32 params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::string name;                 // Parameter: the live input's name. WavePlayer: the asset path.
};
struct SoundLinkDesc { u32 from = 0, to = 0, input = 0; };
struct SoundGraphDesc {
    std::vector<SoundNodeDesc> nodes;
    std::vector<SoundLinkDesc> links;
    u32 outputNode = 0;
    f32 durationSec = 1.0f;
};

// A compiled graph, immutable and shareable between voices.
class SoundProgram {
public:
    // Null with `why` set when the graph is malformed: a link that does not run from a lower node to a higher one or feeds an
    // input the target lacks, too many nodes / parameter names / wave assets, or more delay memory than kSoundMaxDelayTotal.
    static std::shared_ptr<const SoundProgram> compile(const SoundGraphDesc& g, std::string* why = nullptr);

    u32 nodeCount() const { return static_cast<u32>(nodes_.size()); }
    f32 durationSec() const { return durationSec_; }
    // Longest Adsr release in the graph: how long a note-off keeps the voice alive (at least kSoundMinTailSec).
    f32 releaseTailSec() const { return tailSec_; }

    u32 paramCount() const { return static_cast<u32>(paramNames_.size()); }
    const std::string& paramName(u32 id) const { return paramNames_[id]; }
    f32 paramDefault(u32 id) const { return paramDefaults_[id]; }
    // The id of a named live input, or -1.
    i32 findParam(const char* name) const;

    // The distinct assets WavePlayer nodes name, in first-appearance order. The caller resolves each to sound data.
    u32 waveCount() const { return static_cast<u32>(waveAssets_.size()); }
    const std::string& waveAsset(u32 slot) const { return waveAssets_[slot]; }

private:
    friend class SoundRunner;
    struct Node {
        SoundNodeKind kind = SoundNodeKind::Sine;
        f32 p[4] = {0, 0, 0, 0};
        u32 aux = 0xFFFFFFFFu;                   // Parameter: its id. WavePlayer: its wave slot. 0xFFFFFFFF: none.
        u32 nIn = 0;
        u32 inBegin[kSoundMaxInputs + 1] = {};   // edges_[inBegin[k] .. inBegin[k+1]) feed input k
    };
    std::vector<Node> nodes_;
    std::vector<u32> edges_;                     // source node indices, grouped by input, each group by ascending source
    u32 outputNode_ = 0;
    f32 durationSec_ = 1.0f;
    f32 tailSec_ = kSoundMinTailSec;
    std::vector<std::string> paramNames_;
    std::vector<f32> paramDefaults_;
    std::vector<std::string> waveAssets_;
};

struct RunnerOptions {
    u32  sampleRate = 48000;
    u32  seed = 1;               // Noise and Random; two plays with different seeds differ
    // true: the sound is durationSec long and an Adsr releases `release` seconds before the end (the offline render). false: it runs
    // until noteOff() and then for the release tail (a held engine loop).
    bool finite = true;
    f32  durationSec = 0.0f;     // > 0 overrides the graph's own duration
};

// One playing instance. The constructor allocates everything; process() allocates nothing.
class SoundRunner {
public:
    SoundRunner(std::shared_ptr<const SoundProgram> program, const RunnerOptions& opt);

    const SoundProgram& program() const { return *prog_; }

    // ---- game thread, before the runner is handed to the audio thread ----
    // The sound a WavePlayer slot plays. It must outlive the runner (the mixer holds a reference count). Null: that slot is silent.
    void setWave(u32 slot, const SoundData* data);

    // ---- game thread, any time ----
    // Latest value wins. False for an unknown id or a non-finite value.
    bool setParam(u32 id, f32 value);
    // Starts the release: an Adsr falls from its current level over its own release time, then the voice ends.
    void noteOff() { noteOff_.store(true, std::memory_order_release); }

    // ---- audio thread ----
    // Writes up to `frames` mono samples in [-1, 1] and returns how many are valid (fewer only when the sound ends inside the block;
    // the rest of the buffer is zeroed).
    u32 process(f32* out, u32 frames);
    bool finished() const { return done_; }
    u64  framesDone() const { return frame_; }
    // Frames left before a finite sound ends on its own; ~0 when it runs until noteOff.
    u64  remainingFrames() const;
    bool noteOffLatched() const { return offLatched_; }

    // Moves a runner to another device rate: time-based state keeps its length in seconds, delay memory is rebuilt (and cleared).
    // NOT audio-thread safe and it allocates; the mixer calls it with the device closed.
    void setSampleRate(u32 rate);

private:
    struct NodeState {
        f32 phase = 0.0f;       // oscillators and Lfo, in turns
        f32 last = 0.0f;        // LowPass memory
        u32 rng = 1u;
        f32 val = 0.0f;         // Parameter (smoothed), Random (held), EnvelopeFollower
        f32 coef = 0.0f;        // Parameter: smoothing coefficient
        f32 coef2 = 0.0f;       // EnvelopeFollower: release coefficient (coef is the attack's)
        f32 s1 = 0.0f, s2 = 0.0f;           // state-variable filter
        f32 fcLast = -1.0f, qLast = -1.0f;  // the cutoff and Q the cached filter coefficients belong to
        f32 a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
        bool primed = false;
        u32 count = 0, period = 1;           // Random hold
        f64 cursor = 0.0;                    // WavePlayer, in source frames
        u32 delayBase = 0, delayLen = 0, delayPos = 0;
    };

    void initState();
    void rebuildTimeConstants();
    f32 evalFrame();

    std::shared_ptr<const SoundProgram> prog_;
    RunnerOptions opt_;
    f32 sr_ = 48000.0f;
    f32 duration_ = 1.0f;
    u64 total_ = 0;                          // frames of a finite sound; ~0 otherwise
    u64 frame_ = 0;
    bool done_ = false;

    std::vector<NodeState> st_;
    std::vector<f32> val_;
    std::vector<f32> delayMem_;
    const SoundData* waves_[kSoundMaxWaves] = {};

    std::atomic<f32>  paramTarget_[kSoundMaxParams];
    std::atomic<bool> paramSet_[kSoundMaxParams];
    f32  paramNow_[kSoundMaxParams] = {};
    bool paramLive_[kSoundMaxParams] = {};

    std::atomic<bool> noteOff_{false};
    bool offLatched_ = false;
    u64  offFrame_ = 0, endFrame_ = 0, tailFrames_ = 0, declickFrames_ = 0;
};

} // namespace aver::audio
