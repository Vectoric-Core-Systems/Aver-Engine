#pragma once
// The mixer: voices in, one interleaved float buffer out.
//
// IT NEVER TOUCHES A DEVICE, and that is the load-bearing decision rather than a consequence of one.
// mix() fills a buffer the caller supplies; something else hands that buffer to hardware. It is the
// same split that lets Aver.UI be tested with no GPU, and audio needs it more, not less: a wrong pan
// law and a right one are the same waveform to a reader and the same silence to a screenshot. If the
// mixer cannot be run headlessly and asserted on sample by sample, it cannot be checked at all.
//
// THE REAL-TIME RULE. mix() runs on an audio callback thread and must never allocate, lock, block,
// log, or call managed code. Not "tries not to" -- one malloc there is a click in somebody's
// headphones, on a machine you do not own. Everything below follows from that one rule:
//
//   - the voice pool is FIXED, sized at init. Running out steals; it never grows.
//   - every field the two threads share is a std::atomic, so there is no mutex to contend and no
//     command queue to overflow. Per-slot atomics rather than the SPSC ring docs/AUDIO.md sketched:
//     a ring is the right answer for ORDERED commands, and volume, position and stop are not
//     ordered with respect to each other -- they are the latest value wins. A ring would have added
//     a bounded queue that can fill, for state that has no need of one.
//   - a voice handle is GENERATIONAL. A handle to a voice that has since finished must read as dead
//     rather than as somebody else's sound, and at 64 voices a slot is reused within seconds.
//   - sound data is released on the GAME thread, behind a reference count, the way the RHI defers a
//     resource destroy behind the GPU fence. The audio thread must never free anything.
//
// UNITS are the engine's: centimetres, +X forward, +Y right, +Z up, LEFT-handed. Worth naming here
// because getting the handedness wrong puts every sound on the wrong side of the player's head,
// which reads as a bug in the pan law rather than as a sign error three files away.
#include "aver/audio/Sound.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace aver::audio {

// How a voice is placed in the world. Distances in CENTIMETRES.
struct Attenuation {
    // Full volume at or inside this. Not zero by default: a point source with no inner radius goes
    // infinitely loud as the listener reaches it, and every game clamps it somewhere.
    f32 innerRadius = 100.0f;
    // Silent at or beyond this. The curve reaches EXACTLY zero here rather than merely approaching
    // it, because a sound that is still 2% audible at its cutoff pops when it is finally culled.
    f32 outerRadius = 2000.0f;
};

struct PlayDesc {
    SoundHandle sound  = 0;
    f32         volume = 1.0f;
    // Playback rate multiplier. 1.0 is the authored pitch; 2.0 is an octave up and half as long.
    f32         pitch  = 1.0f;
    bool        looping = false;
    Bus         bus = Bus::Sfx;

    // Positional voices are panned and attenuated; non-positional ones play at `volume` in both
    // ears, which is what music and UI want. A flag rather than "a position of NaN means 2D".
    bool        positional = false;
    f32         position[3] = {0.0f, 0.0f, 0.0f};
    Attenuation attenuation{};
};

// The listener: where the ears are and which way they face.
struct Listener {
    f32 position[3] = {0.0f, 0.0f, 0.0f};
    f32 forward[3]  = {1.0f, 0.0f, 0.0f};   // +X forward, per the engine contract
    f32 right[3]    = {0.0f, 1.0f, 0.0f};   // +Y right
};

class Mixer {
public:
    Mixer() = default;
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // `sampleRate` and `channels` are the DEVICE's, not the content's: every sound is resampled to
    // this as it plays. 1 or 2 channels; anything else is refused rather than downmixed silently.
    bool init(u32 sampleRate, u32 channels, u32 maxVoices = 64, u32 maxSounds = 1024);
    void shutdown();
    bool ready() const { return ready_; }

    u32 sampleRate() const { return sampleRate_; }
    u32 channels() const { return channels_; }

    // ---- game thread: content ----
    //
    // TAKES OWNERSHIP. The capacity is fixed at init so the backing store never reallocates, which
    // is what lets a voice hold a bare pointer to its sound across a mix the game thread is not
    // synchronised with. Returns 0 when the table is full.
    SoundHandle addSound(SoundData&& data);
    // Marks the sound for release. The bytes are freed by collect() once no voice still refers to
    // them -- never by the audio thread, which may not free anything.
    void removeSound(SoundHandle h);
    // Reclaims retired sounds whose last voice has ended. Call from the game thread whenever
    // convenient; doing so never is a leak, not a crash.
    void collect();
    u32  soundCount() const;

    // ---- game thread: playback ----
    VoiceHandle play(const PlayDesc& desc);
    void stop(VoiceHandle v);
    void stopAll();
    // Whether the handle still names a live voice. False for a finished one, and false for a handle
    // whose slot has been reused -- that is what the generation is for.
    bool playing(VoiceHandle v) const;

    void setVoiceVolume(VoiceHandle v, f32 volume);
    void setVoicePitch(VoiceHandle v, f32 pitch);
    void setVoicePosition(VoiceHandle v, f32 x, f32 y, f32 z);

    void setListener(const Listener& l);
    void setBusVolume(Bus b, f32 volume);
    f32  busVolume(Bus b) const;
    void setMasterVolume(f32 volume);
    f32  masterVolume() const;

    // ---- audio thread ----
    //
    // Renders `frames` frames of interleaved output, OVERWRITING whatever was there. The only
    // function in this class that runs on the callback thread, and the only one that must obey the
    // real-time rule.
    void mix(f32* out, u32 frames);

    // ---- diagnostics ----
    u32 activeVoices() const;
    // Voices cut short because the pool was full. Nonzero means maxVoices is too small for the
    // content, which is a tuning fact rather than an error.
    u32 stolenVoices() const { return stolen_.load(std::memory_order_relaxed); }
    // Frames the mixer was asked for while not ready. The DEVICE counts its own underruns; this
    // counts the mixer being asked to do the impossible.
    u32 starvedFrames() const { return starved_.load(std::memory_order_relaxed); }

private:
    // A voice's slot state. The transitions are the whole of the thread protocol:
    //
    //   Free    -> Pending : the GAME thread claims a slot (compare-exchange)
    //   Pending -> Active  : the game thread has finished writing the slot's parameters
    //   Active  -> Free    : the AUDIO thread has finished playing it
    //
    // The audio thread never reads a Pending slot's parameters, which is why they can be plain
    // fields written without synchronisation: the release store of Active is what publishes them.
    enum class State : u32 { Free = 0, Pending, Active };

    struct Voice {
        std::atomic<u32> state{static_cast<u32>(State::Free)};
        std::atomic<u32> generation{1};      // 1, never 0: handle 0 must be unissuable
        std::atomic<bool> stopping{false};

        // Written by the game thread before the slot goes Active, then read only by the audio thread.
        const SoundData* sound = nullptr;
        SoundHandle soundHandle = 0;
        bool  looping = false;
        Bus   bus = Bus::Sfx;
        bool  positional = false;
        Attenuation attenuation{};
        u64   startOrder = 0;                // for choosing which voice to steal

        // Changed while playing, so atomic. Lock-free for 4 bytes on every platform this targets.
        std::atomic<f32> volume{1.0f};
        std::atomic<f32> pitch{1.0f};
        std::atomic<f32> posX{0.0f}, posY{0.0f}, posZ{0.0f};

        // Audio-thread only.
        f64 cursor = 0.0;                    // fractional frame position in the source
        f32 gainL = 0.0f, gainR = 0.0f;      // smoothed, so a volume change does not zipper
        bool primed = false;                 // first block: snap the gain instead of ramping into it
    };

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

    Voice* resolve(VoiceHandle v);
    const Voice* resolve(VoiceHandle v) const;

    // Steals the quietest, then oldest, Active voice. Game thread only.
    u32 stealSlot();

    // Audio thread. Returns false when the voice has ended and its slot should be freed.
    bool renderVoice(Voice& vo, f32* out, u32 frames);
    // The pair of gains this voice contributes, after distance, panning and its bus.
    void voiceGains(const Voice& vo, f32& outL, f32& outR) const;

    bool ready_ = false;
    u32  sampleRate_ = 0;
    u32  channels_ = 0;

    std::vector<Voice>     voices_;
    std::vector<SoundSlot> sounds_;

    std::atomic<u64> playCounter_{1};
    std::atomic<u32> stolen_{0};
    std::atomic<u32> starved_{0};

    std::atomic<f32> master_{1.0f};
    std::atomic<f32> buses_[static_cast<usize>(Bus::Count)];

    // The listener, as nine atomics rather than a struct behind a lock. It is written once per frame
    // by the game thread and read once per block by the audio thread; a torn read costs one block of
    // very slightly wrong panning, which is inaudible, where a lock costs a priority inversion on
    // the one thread that must never wait.
    std::atomic<f32> lisPos_[3];
    std::atomic<f32> lisFwd_[3];
    std::atomic<f32> lisRight_[3];
};

// Distance attenuation: EXACTLY 1 at or inside the inner radius, EXACTLY 0 at or beyond the outer,
// inverse-distance in between. Exposed because it is the one piece of the mixer a test wants to
// assert on directly, and because a game may want to predict it (a sound that will be silent need
// not be started at all).
f32 attenuationAt(const Attenuation& a, f32 distanceCm);

// Constant-power pan. `pan` is -1 (hard left) to +1 (hard right); the two gains satisfy
// l*l + r*r == 1, so a source swept across the field holds its energy. A linear pan law instead
// dips about 3 dB in the middle -- clearly audible, and it looks perfectly reasonable on paper.
void panGains(f32 pan, f32& outL, f32& outR);

} // namespace aver::audio
