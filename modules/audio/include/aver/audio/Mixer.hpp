#pragma once
// The mixer: voices in, one interleaved float buffer out. It never touches a device.
// mix() runs on the audio callback thread and must not allocate, lock, block, log or call
// managed code. Units are the engine's: centimetres, +X forward, +Y right, +Z up, left-handed.
#include "aver/audio/Sound.hpp"

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
    bool init(u32 sampleRate, u32 channels, u32 maxVoices = 64, u32 maxSounds = 1024);
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
    // Reclaims retired sounds whose last voice has ended. Game thread only.
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

    // Moves the listener.
    void setListener(const Listener& l);
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

private:
    // A voice slot's state. Free -> Pending (game thread claims), Pending -> Active (parameters
    // written), Active -> Free (audio thread finished). A Pending slot's parameters are never read.
    enum class State : u32 { Free = 0, Pending, Active };

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

    // Adds one voice into the block. Returns false when the voice has ended.
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

    std::atomic<f32> lisPos_[3];
    std::atomic<f32> lisFwd_[3];
    std::atomic<f32> lisRight_[3];
};

// Distance attenuation: 1 at or inside the inner radius, 0 at or beyond the outer.
f32 attenuationAt(const Attenuation& a, f32 distanceCm);

// Constant-power pan. `pan` is -1 (hard left) to +1 (hard right); l*l + r*r == 1.
void panGains(f32 pan, f32& outL, f32& outR);

} // namespace aver::audio
