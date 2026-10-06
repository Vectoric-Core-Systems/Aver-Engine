#pragma once
// The mixer: voices in, one interleaved float buffer out. It never touches a device.
// mix() runs on the audio callback thread and must not allocate, lock, block, log or call
// managed code. Units are the engine's: centimetres, +X forward, +Y right, +Z up, left-handed.
#include "aver/audio/Dsp.hpp"
#include "aver/audio/Reverb.hpp"
#include "aver/audio/Sound.hpp"
#include "aver/audio/Stream.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace aver::audio {

// How a voice is placed in the world. Distances in CENTIMETRES.
struct Attenuation {
    f32 innerRadius = 100.0f;    // full volume at or inside this
    f32 outerRadius = 2000.0f;   // silent at or beyond this
};

// One request to start a voice.
struct PlayDesc {
    SoundHandle sound  = 0;
    f32         volume = 1.0f;
    f32         pitch  = 1.0f;   // playback rate multiplier; 1.0 is the authored pitch
    bool        looping = false;
    Bus         bus = Bus::Sfx;

    bool        positional = false;
    f32         position[3] = {0.0f, 0.0f, 0.0f};
    Attenuation attenuation{};

    f32         fadeInSeconds = 0.0f;                 // ramps the voice up from silence
    FadeCurve   fadeCurve = FadeCurve::EqualPower;
    f32         reverbSend = -1.0f;                   // < 0: 1 on Sfx and Voice, 0 on Music and Ui
};

// The listener: where the ears are and which way they face.
struct Listener {
    f32 position[3] = {0.0f, 0.0f, 0.0f};
    f32 forward[3]  = {1.0f, 0.0f, 0.0f};   // +X forward
    f32 right[3]    = {0.0f, 1.0f, 0.0f};   // +Y right
};

// Owns the sound table and the fixed voice pool, and renders them into a buffer.
class Mixer {
public:
    Mixer() = default;
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // Sizes the pools for a device of this rate and channel count. 1 or 2 channels only.
    bool init(u32 sampleRate, u32 channels, u32 maxVoices = 64, u32 maxSounds = 1024, u32 maxStreams = 16);
    // Releases the pools. The caller must have stopped the device first.
    void shutdown();
    bool ready() const { return ready_; }

    u32 sampleRate() const { return sampleRate_; }
    u32 channels() const { return channels_; }

    // ---- game thread: content ----
    //
    // Takes ownership of decoded audio and returns its handle. 0 when the table is full.
    SoundHandle addSound(SoundData&& data);
    // Marks the sound for release. collect() frees the bytes once no voice refers to them.
    void removeSound(SoundHandle h);
    // Reclaims retired sounds whose last voice has ended, and finished streams. Game thread only.
    void collect();
    u32  soundCount() const;

    // ---- game thread: playback ----
    // Starts a voice, stealing one if the pool is full. Returns its handle, or 0.
    VoiceHandle play(const PlayDesc& desc);
    // Flags a voice to fade out over one block.
    void stop(VoiceHandle v);
    // Flags every active voice to fade out.
    void stopAll();
    // Whether the handle still names a live voice.
    bool playing(VoiceHandle v) const;

    void setVoiceVolume(VoiceHandle v, f32 volume);
    void setVoicePitch(VoiceHandle v, f32 pitch);
    void setVoicePosition(VoiceHandle v, f32 x, f32 y, f32 z);

    // ---- game thread: streaming, fades, occlusion, reverb ----
    //
    // Plays a streamed source. The voice owns the decoder; collect() frees it after the voice ends.
    // desc.looping and the loop points in `params` decide looping. Returns 0 when no stream or voice is free.
    VoiceHandle playStream(std::unique_ptr<StreamSource> source, const PlayDesc& desc, StreamParams params = {});
    // Decodes up to `frames` for every stream created with threaded = false. For tests and hosts
    // with no decoder thread; threaded streams ignore it.
    void pumpStreams(u32 frames);
    // Ramps a voice's fade gain to `targetGain` over `seconds`, sample-accurately on the audio thread.
    // With stopAtEnd the voice ends when the ramp does.
    void fadeVoice(VoiceHandle v, f32 targetGain, f32 seconds, FadeCurve curve = FadeCurve::EqualPower,
                   bool stopAtEnd = false);
    // 0 clear .. 1 fully occluded: scales volume and low-passes the voice. Smoothed on the audio thread.
    void setVoiceOcclusion(VoiceHandle v, f32 occlusion);
    // How much of the voice feeds the reverb return, 0..1.
    void setVoiceReverbSend(VoiceHandle v, f32 send);
    // Occlusion's cutoff and volume at full occlusion (maxCutoffHz is fixed).
    void setOcclusionCurve(const OcclusionCurve& c);
    OcclusionCurve occlusionCurve() const;
    // The listener-side reverb return. wet 0 disables it.
    void setReverb(const ReverbParams& p);
    ReverbParams reverb() const;

    // Moves the listener.
    void setListener(const Listener& l);
    // Where the listener is now, as last set.
    Listener listener() const;
    void setBusVolume(Bus b, f32 volume);
    f32  busVolume(Bus b) const;
    void setMasterVolume(f32 volume);
    f32  masterVolume() const;

    // ---- audio thread ----
    //
    // Renders `frames` frames of interleaved output, overwriting the buffer.
    void mix(f32* out, u32 frames);

    // ---- diagnostics ----
    u32 activeVoices() const;
    // Voices cut short because the pool was full.
    u32 stolenVoices() const { return stolen_.load(std::memory_order_relaxed); }
    // Frames the mixer was asked for while not ready.
    u32 starvedFrames() const { return starved_.load(std::memory_order_relaxed); }
    // Stream voices currently owning a decoder.
    u32 activeStreams() const;
    // Blocks in which a stream voice ran out of decoded audio.
    u32 streamUnderruns() const { return streamUnderruns_.load(std::memory_order_relaxed); }

private:
    // A voice slot's state. Free -> Pending (game thread claims), Pending -> Active (parameters
    // written), Active -> Free (audio thread finished). A Pending slot's parameters are never read.
    // Free -> Pending -> Active is the control thread's sequence; Active <-> Rendering is the audio
    // thread's claim.
    //
    // RENDERING EXISTS TO CLOSE A USE-AFTER-FREE. The audio thread used to render a voice while it
    // stayed Active for the whole block, so play()'s voice-stealing CAS on Active succeeded
    // MID-RENDER: it then dropped the sound's refcount -- freeing samples the audio thread was still
    // reading -- and overwrote vo.sound and vo.cursor underneath it. The audio thread now takes the
    // voice out of Active for the duration, so that CAS fails and the stealer moves on.
    //
    // ANYTHING ASKING "IS THIS VOICE PLAYING" MUST ACCEPT BOTH. A voice being rendered is playing;
    // treating Rendering as not-Active makes playing() and voiceCount() flicker with the audio
    // callback. Use isLiveState().
    enum class State : u32 { Free = 0, Pending, Active, Rendering };

    // True for a voice that is sounding, whether or not the audio thread is inside it right now.
    static bool isLiveState(u32 s) {
        return s == static_cast<u32>(State::Active) || s == static_cast<u32>(State::Rendering);
    }

    // One playing sound: its published parameters and the audio thread's cursor.
    struct Voice {
        std::atomic<u32> state{static_cast<u32>(State::Free)};
        std::atomic<u32> generation{1};      // 1, never 0: handle 0 must be unissuable
        std::atomic<bool> stopping{false};

        const SoundData* sound = nullptr;
        SoundHandle soundHandle = 0;
        bool  looping = false;
        Bus   bus = Bus::Sfx;
        bool  positional = false;
        Attenuation attenuation{};
        u64   startOrder = 0;

        std::atomic<f32> volume{1.0f};
        std::atomic<f32> pitch{1.0f};
        std::atomic<f32> posX{0.0f}, posY{0.0f}, posZ{0.0f};

        f64 cursor = 0.0;                    // fractional frame position in the source
        f32 gainL = 0.0f, gainR = 0.0f;      // smoothed per block
        bool primed = false;                 // first block snaps the gain instead of ramping

        // Streaming: the decoder (owned by streams_[streamSlot]) and the read cursor relative to its ring.
        StreamDecoder* stream = nullptr;
        u32  streamSlot = 0xFFFFFFFFu;
        bool resumeRamp = false;             // fade back in after an under-run

        // Fade requests are written by the game thread, latched by the audio thread on a new seq.
        std::atomic<u32>  fadeSeq{0};
        std::atomic<f32>  fadeTarget{1.0f};
        std::atomic<u32>  fadeFrames{0};
        std::atomic<u32>  fadeCurveReq{0};
        std::atomic<bool> fadeStopReq{false};
        std::atomic<f32>  fadePub{1.0f};     // current fade gain, readable for voice stealing
        u32  fadeSeen = 0, fadeProgress = 0, fadeLen = 0;
        f32  fadeFrom = 1.0f, fadeTo = 1.0f, fadeCur = 1.0f;
        FadeCurve fadeCurve = FadeCurve::Linear;
        bool fadeActive = false, fadeStopAtEnd = false;

        std::atomic<f32> occlusion{0.0f};
        std::atomic<f32> reverbSend{0.0f};
        f32  occSmooth = 0.0f, sendGain = 0.0f;
        bool occPrimed = false, filterActive = false;
        BiquadState filtL, filtR;
    };

    // One streamed source's decoder. attached is true while a voice reads it.
    struct StreamSlot {
        std::unique_ptr<StreamDecoder> dec;
        std::atomic<bool> attached{false};
    };

    // One entry in the sound table, with the reference count that defers its free.
    struct SoundSlot {
        std::unique_ptr<SoundData> data;
        std::atomic<u32> refs{0};            // voices currently reading it
        std::atomic<bool> retired{false};    // removeSound was called; free when refs hits 0
    };

    static VoiceHandle encode(u32 index, u32 generation) {
        return (generation << 16) | (index & 0xFFFFu);
    }
    static u32 indexOf(VoiceHandle v)      { return v & 0xFFFFu; }
    static u32 generationOf(VoiceHandle v) { return v >> 16; }

    // Live voice for a handle, or nullptr when the handle is stale or free.
    Voice* resolve(VoiceHandle v);
    const Voice* resolve(VoiceHandle v) const;

    // Picks the quietest, then oldest, Active voice. Game thread only.
    u32 stealSlot();
    // Claims a Pending voice slot, stealing if the pool is full. 0xFFFFFFFF when none.
    u32 acquireVoice();
    // Fills a claimed voice's shared fields from a PlayDesc and publishes it Active.
    VoiceHandle activate(u32 index, const PlayDesc& desc);
    // Latches a pending fade request and returns the fade gain at the end of this block.
    f32 advanceFade(Voice& vo, u32 frames, bool& stopNow);
    void requestFade(Voice& vo, f32 target, u32 frames, FadeCurve curve, bool stopAtEnd);

    // Adds one voice into the block. Returns false when the voice has ended.
    bool renderVoice(Voice& vo, f32* out, u32 frames);
    // The pair of gains this voice contributes, after distance, panning and its bus.
    void voiceGains(const Voice& vo, f32& outL, f32& outR) const;

    bool ready_ = false;
    u32  sampleRate_ = 0;
    u32  channels_ = 0;

    std::vector<Voice>     voices_;
    std::vector<SoundSlot> sounds_;
    std::vector<StreamSlot> streams_;

    std::atomic<u64> playCounter_{1};
    std::atomic<u32> stolen_{0};
    std::atomic<u32> starved_{0};
    std::atomic<u32> streamUnderruns_{0};

    std::atomic<f32> master_{1.0f};
    std::atomic<f32> buses_[static_cast<usize>(Bus::Count)];

    std::atomic<f32> lisPos_[3];
    std::atomic<f32> lisFwd_[3];
    std::atomic<f32> lisRight_[3];

    std::atomic<f32> occMinCutoff_{450.0f}, occMinVolume_{0.30f};

    // Reverb: parameters from the game thread; the processor and scratch belong to the audio thread.
    std::atomic<f32> rvWet_{0.0f}, rvDecay_{1.5f}, rvDamp_{0.4f};
    Reverb reverb_;
    std::vector<f32> sendBuf_, rvL_, rvR_;
    f32* sendPtr_ = nullptr;          // non-null only while this block feeds the reverb
    f32  rvPrevWet_ = 0.0f;
    bool rvDirty_ = false;            // the reverb holds a tail that must be cleared when it stops
};

// Distance attenuation: 1 at or inside the inner radius, 0 at or beyond the outer.
f32 attenuationAt(const Attenuation& a, f32 distanceCm);

// Constant-power pan. `pan` is -1 (hard left) to +1 (hard right); l*l + r*r == 1.
void panGains(f32 pan, f32& outL, f32& outR);

} // namespace aver::audio
