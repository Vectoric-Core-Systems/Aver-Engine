#pragma once
// The mixer: voices in, one interleaved float buffer out. It never touches a device.
// mix() runs on the audio callback thread and must not allocate, lock, block, log or call
// managed code. Units are the engine's: centimetres, +X forward, +Y right, +Z up, left-handed.
#include "aver/audio/Binaural.hpp"
#include "aver/audio/BusGraph.hpp"
#include "aver/audio/Convolver.hpp"
#include "aver/audio/Dsp.hpp"
#include "aver/audio/Reverb.hpp"
#include "aver/audio/Sound.hpp"
#include "aver/audio/SoundGraph.hpp"
#include "aver/audio/Spatial.hpp"
#include "aver/audio/Speakers.hpp"
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

    // ---- the bus graph and Godot's 3D player (opt-in; the defaults are the legacy behaviour) ----
    i32         graphBus = -1;                        // >= 0: sum into this BusGraph bus instead of a legacy bus (needs graph().setActive)
    bool        godotSpatial = false;                 // positional with Godot's AudioStreamPlayer3D model (needs `positional`); `attenuation` is ignored
    Spatial3D   spatial{};
    f32         startSeconds = 0.0f;                  // where in the sound to start
    bool        paused = false;
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

    // Sizes the pools for a device of this rate and channel count: 1 (mono), 2 (stereo, the default), 6 (5.1) or 8 (7.1), interleaved in the
    // order of Speakers.hpp. The speaker / binaural / LFE settings below are kept across init, so they may be set before the device opens.
    bool init(u32 sampleRate, u32 channels, u32 maxVoices = 64, u32 maxSounds = 1024, u32 maxStreams = 16);
    // Releases the pools. The caller must have stopped the device first.
    void shutdown();
    bool ready() const { return ready_; }

    u32 sampleRate() const { return sampleRate_.load(std::memory_order_relaxed); }
    u32 channels() const { return channels_.load(std::memory_order_relaxed); }

    // Moves a running mixer to another device format (a default-device change) and keeps everything else: voices, streams, sounds, bus
    // volumes, the bus graph and its effects. Per-asset rates are untouched; every voice resamples from its own source rate to the new
    // `rate` from its next block. Effects and the reverb are rebuilt for the new rate (their delay memory is cleared; parameters, order
    // and effect ids survive); a fade in progress keeps its length in seconds. 1, 2, 6 or 8 channels; a change of channel count makes every
    // voice take its new gains at once instead of ramping.
    //
    // NOT AUDIO-THREAD SAFE: the caller must guarantee no mix() is running (the device layer calls it with the old device closed and the new
    // one not yet started). It allocates, so it is never called from mix(). False on bad arguments or before init().
    bool setOutputFormat(u32 rate, u32 channels);

    // ---- game thread: content ----
    //
    // Takes ownership of decoded audio and returns its handle. 0 when the table is full.
    SoundHandle addSound(SoundData&& data);
    // Marks the sound for release. collect() frees the bytes once no voice refers to them.
    void removeSound(SoundHandle h);
    // Reclaims retired sounds whose last voice has ended, and finished streams. Game thread only.
    void collect();
    u32  soundCount() const;
    // Length in seconds of a loaded sound; 0 for an unknown handle.
    f64  soundLengthSeconds(SoundHandle h) const;
    // Sets a loaded sound's loop points in frames (end 0 = to the end). Voices already playing pick them up at their next block.
    void setSoundLoop(SoundHandle h, u32 beginFrame, u32 endFrame);

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
    // Floating-origin shift (LARGE_WORLD 2.2.5): subtracts (dx, dy, dz) cm from every voice position and from the
    // listener, so a sound keeps its distance and pan. Game thread; relaxed stores like setVoicePosition, so the
    // audio thread may mix one block with the voices moved and the listener not yet.
    void rebase(f32 dx, f32 dy, f32 dz);

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

    // ---- game thread: zone send reverbs (docs/AUDIO.md section 15) ----
    //
    // kMaxZoneReverbs send reverbs are pre-allocated at init, each an algorithmic reverb or a convolution with an impulse response. A voice
    // sends to a slot with setVoiceZoneSend (0 by default, so nothing changes until a scene asks); the slot's return is added to the output
    // at `wet`. A slot is off (and silent, with its tail cleared) while wet is 0 and nothing sends. The listener-side reverb above is
    // separate and unchanged.
    //
    // Sets a slot's reverb: wet 0 turns it off. A non-zero zone.impulse is a sound in this mixer; the slot then convolves with it (the sound
    // can be removed afterwards: the convolver keeps its own copy). 0 returns the slot to the algorithmic reverb. False for a bad slot or an
    // unknown impulse sound.
    bool setZoneReverb(u32 slot, const ZoneReverb& zone);
    ZoneReverb zoneReverb(u32 slot) const;
    // How much of a voice feeds a slot, 0..1 (a latest-value atomic; the audio thread ramps it per block).
    void setVoiceZoneSend(VoiceHandle v, u32 slot, f32 send);
    // The impulse-response effect on the bus graph: builds effect `effectId`'s impulse from a sound in this mixer (see Effect::setImpulse).
    bool setEffectImpulse(u32 effectId, SoundHandle ir);

    // ---- game thread: the bus graph, Godot-spatial voices, pause, position, live rings ----
    //
    // The graph (docs/AUDIO.md). It does nothing until graph().setActive(true); voices with a graphBus are silent before that.
    BusGraph& graph() { return graph_; }
    const BusGraph& graph() const { return graph_; }
    // Re-routes a live voice to a graph bus (an out-of-range index plays on Master).
    void setVoiceGraphBus(VoiceHandle v, i32 bus);
    // Replaces a Godot-spatial voice's parameters (and makes it one).
    void setVoiceSpatial(VoiceHandle v, const Spatial3D& s);
    // A paused voice fades to silence within one block and holds its position.
    void setVoicePaused(VoiceHandle v, bool paused);
    // Seconds played of the source (a looping source wraps); -1 for a stale handle. Streams count from the start frame.
    f64  voicePositionSeconds(VoiceHandle v) const;
    // Length of the source in seconds, 0 when unknown (a live ring, some streams).
    f64  voiceLengthSeconds(VoiceHandle v) const;
    // The listener's velocity, cm/s, for Doppler.
    void setListenerVelocity(f32 x, f32 y, f32 z);
    // Godot's AudioServer.playback_speed_scale: multiplies every voice's rate (1 = unchanged).
    void setPlaybackSpeedScale(f32 s) { speedScale_.store(s > 0.0f ? s : 1.0f, std::memory_order_relaxed); }
    f32  playbackSpeedScale() const { return speedScale_.load(std::memory_order_relaxed); }

    // A voice fed by the game thread (AudioStreamGenerator): a ring of `ringFrames` frames at `rate`, `channels` 1 or 2. Returns the voice.
    VoiceHandle playLive(const PlayDesc& desc, u32 rate, u32 channels, u32 ringFrames);
    // Queues frames (interleaved) on a live voice; returns how many fitted.
    u32  pushLive(VoiceHandle v, const f32* frames, u32 count);
    u32  liveFree(VoiceHandle v) const;
    void liveFlush(VoiceHandle v);
    // Under-runs of a stream or live voice (Godot's get_skips).
    u32  voiceUnderruns(VoiceHandle v) const;

    // ---- game thread: real-time sound graphs (docs/AUDIO.md section 13) ----
    //
    // A live value for a graph's named input, applied before the first block so the voice does not ramp up from the default.
    struct GraphParam { const char* name; f32 value; };
    // Starts a .ocsnd graph as a voice evaluated on the audio thread. The runner's whole state is allocated HERE (game thread); mix() only
    // does arithmetic. `waves` are the sounds the program's WavePlayer slots play, in program->waveAsset() order (0 = silent slot); the voice
    // holds a reference on each until it ends. desc.looping: run until graphNoteOff()/stop() instead of for the graph's duration. desc.pitch
    // does not apply (the graph makes its own pitch; use a Parameter). seed 0 picks a different seed per play, so Noise and Random vary.
    // Steals like any voice when the pool is full. Returns 0 on failure.
    VoiceHandle playGraph(std::shared_ptr<const SoundProgram> program, const SoundHandle* waves, u32 waveCount, const PlayDesc& desc,
                          u32 seed = 0, const GraphParam* initial = nullptr, u32 initialCount = 0);
    // Latest-value store on a graph voice's named input. False for a stale handle, a voice that is not a graph, or an unknown name.
    bool setGraphParam(VoiceHandle v, const char* name, f32 value);
    bool setGraphParamId(VoiceHandle v, u32 id, f32 value);
    // The id of a graph voice's named input, or -1.
    i32  graphParamId(VoiceHandle v, const char* name) const;
    // The note-off: Adsr nodes release from their current level and the voice ends once the longest release has played. stop() instead
    // fades the voice out within one block, like any voice.
    void graphNoteOff(VoiceHandle v);
    bool isGraphVoice(VoiceHandle v) const;

    // ---- game thread: surround output, LFE send, binaural (docs/AUDIO.md section 14) ----
    //
    // All opt-in: with the defaults (speaker request 2, binaural off, LFE send 0) a stereo mixer renders exactly the samples it always did.
    //
    // The channel count the project WANTS: 2 (default), 6 or 8 (anything else is stored as 2). The mixer's own channel count is still the
    // device's decision at open (OutputDriver, chooseMixChannels): a request the device cannot play is quietly stereo. The driver reopens the
    // device when this changes. An offline mixer is sized by init() and ignores it.
    void setSpeakerRequest(u32 channels) { speakerReq_.store(channels == 6 || channels == 8 ? channels : 2u, std::memory_order_relaxed); }
    u32  speakerRequest() const { return speakerReq_.load(std::memory_order_relaxed); }
    // How a stereo source (and the reverb and bus-graph returns) is spread over 5.1 / 7.1. Wrap by default.
    void setStereoUpmix(StereoUpmix m) { upmix_.store(static_cast<u32>(m), std::memory_order_relaxed); }
    StereoUpmix upmixMode() const { return static_cast<StereoUpmix>(upmix_.load(std::memory_order_relaxed)); }
    // How much of every voice is sent to the LFE channel (5.1 / 7.1 only), 0..1; 0 (default) leaves the LFE silent. The send is the voice's
    // mono mix at its final level, low-passed once at 120 Hz.
    void setLfeSend(f32 send) { lfeSend_.store(send < 0.0f ? 0.0f : (send > 1.0f ? 1.0f : send), std::memory_order_relaxed); }
    f32  lfeSend() const { return lfeSend_.load(std::memory_order_relaxed); }
    // Headphone mode: positional voices on a STEREO mixer are placed with the analytic head model of Binaural.hpp (interaural delay, head
    // shadow, a pinna cue) instead of the constant-power pan. Off by default; ignored on mono, 5.1 and 7.1 mixers and for voices that are not
    // positional.
    void setBinaural(bool on) { binaural_.store(on, std::memory_order_relaxed); }
    bool binaural() const { return binaural_.load(std::memory_order_relaxed); }

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

        // Zone send reverbs: the published amount per slot and the audio thread's ramped gain.
        std::atomic<f32> zoneSend[kMaxZoneReverbs];
        f32  zoneGain[kMaxZoneReverbs] = {};

        // Surround and binaural output (audio thread). spk is a positional voice's smoothed gain per speaker; lfeGain its LFE send; bin the
        // head-model filter, primed (cleared and snapped) the first block it is used.
        f32  spk[kMaxSpeakers] = {};
        f32  lfeGain = 0.0f;
        BinauralState bin;
        bool binReady = false;

        // The bus graph, Godot's 3D model, pause and position (all opt-in; the defaults leave a voice exactly as it was).
        std::atomic<i32>  graphBus{-1};
        std::atomic<bool> godot3d{false};
        std::atomic<f32>  sp[Spatial3D::kFloats];
        std::atomic<bool> paused{false};
        std::atomic<f64>  posPub{0.0};       // source frames played (statics: the cursor; streams: start frame + consumed)
        f64  lengthSec = 0.0, srcRateHz = 0.0;
        u64  streamStart = 0, streamFrames = 0;
        bool pauseSilenced = false;
        BiquadState shL, shR;                // the attenuation filter (high shelf)
        bool shelfActive = false;

        // A graph voice: the runner is owned here and only the game thread creates or frees it (play and collect). graphRun is the audio
        // thread's view of it, null for every other kind of voice. waveHandles are the sound-table references the voice holds.
        std::unique_ptr<SoundRunner> runner;
        SoundRunner* graphRun = nullptr;
        SoundHandle waveHandles[kSoundMaxWaves] = {};
        u32 waveCount = 0;
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
    VoiceHandle activate(u32 index, const PlayDesc& desc, f64 startCursor = 0.0, f64 lengthSec = 0.0, f64 srcRate = 0.0, u64 streamStart = 0);
    // Latches a pending fade request and returns the fade gain at the end of this block.
    f32 advanceFade(Voice& vo, u32 frames, bool& stopNow);
    void requestFade(Voice& vo, f32 target, u32 frames, FadeCurve curve, bool stopAtEnd);

    // Drops what a voice holds in the sound table and stream pool (sound, stream slot, graph waves). Audio thread or the stealing game thread.
    void releaseVoiceResources(Voice& vo);

    // Adds one voice into the block (outCh interleaved channels). Returns false when the voice has ended.
    bool renderVoice(Voice& vo, f32* out, u32 frames, u32 outCh);
    // The pair of gains this voice contributes, after distance, panning and its bus. For a Godot-spatial voice `shelf` receives the
    // attenuation filter's linear gain (1 = none) and `dopplerPitch` the pitch multiplier.
    void voiceGains(const Voice& vo, f32& outL, f32& outR, f32* shelf = nullptr, f32* dopplerPitch = nullptr) const;
    // Where a positional voice is for surround and binaural output: its level before any panning (distance, volume, bus, fade; no back
    // attenuation, the speakers and the head model express front and back), the direction in the listener's frame (azimuth 0 ahead, positive
    // right; elevation positive up), the horizontal share of that direction (0 overhead or inside the head, 1 on the horizon) and how much of
    // it a Godot-spatial voice's panning strength lets through (1 for a plain voice).
    struct Direction { f32 level = 0.0f, azimuth = 0.0f, elevation = 0.0f, horizontal = 1.0f, panScale = 1.0f; };
    Direction voiceDirection(const Voice& vo) const;
    // One chunk of the mix: the legacy voices into `out`, graph voices into their buses when graphOn, the reverb return, the graph output.
    void mixChunk(f32* out, u32 frames, u32 outCh, bool graphOn);

    // One zone send reverb: targets from the game thread, processors and scratch for the audio thread.
    struct ZoneSlot {
        std::atomic<f32> wet{0.0f}, decay{1.5f}, damp{0.4f};
        std::atomic<u32> impulse{0};
        std::atomic<f32> predelayMs{0.0f};
        Reverb rv;
        std::unique_ptr<Convolver> conv;
        std::vector<f32> send;            // mono send bus
        f32 prevWet = 0.0f, smDecay = 1.5f, smDamp = 0.4f;
        bool dirty = false;
    };
    // Adds the stereo returns of the zone slots that ran this chunk into `out` (after the legacy reverb), ramping each from last chunk's wet.
    void mixZoneReturns(f32* out, u32 frames, u32 outCh, const UpmixMatrix& um);

    bool ready_ = false;
    // Atomic only because setOutputFormat (the render thread, between devices) can run while the game thread calls play() or fadeVoice().
    std::atomic<u32> sampleRate_{0};
    std::atomic<u32> channels_{0};

    std::vector<Voice>     voices_;
    std::vector<SoundSlot> sounds_;
    std::vector<StreamSlot> streams_;

    std::vector<f32> graphScratch_;   // one chunk of a graph voice's mono output, audio thread only
    std::atomic<u32> graphSeed_{1};

    std::atomic<u64> playCounter_{1};
    std::atomic<u32> stolen_{0};
    std::atomic<u32> starved_{0};
    std::atomic<u32> streamUnderruns_{0};

    std::atomic<f32> master_{1.0f};
    std::atomic<f32> buses_[static_cast<usize>(Bus::Count)];

    std::atomic<f32> lisPos_[3];
    std::atomic<f32> lisFwd_[3];
    std::atomic<f32> lisRight_[3];
    std::atomic<f32> lisVel_[3];
    std::atomic<f32> speedScale_{1.0f};
    BusGraph graph_;

    std::atomic<f32> occMinCutoff_{450.0f}, occMinVolume_{0.30f};

    // Surround / LFE / binaural: settings from the game thread (kept across init); the LFE scratch and filter belong to the audio thread.
    std::atomic<u32>  speakerReq_{2};
    std::atomic<u32>  upmix_{static_cast<u32>(StereoUpmix::Wrap)};
    std::atomic<f32>  lfeSend_{0.0f};
    std::atomic<bool> binaural_{false};
    std::vector<f32>  lfeBuf_;
    f32* lfePtr_ = nullptr;           // non-null only while this block feeds the LFE channel
    bool lfeTail_ = false;            // the LFE ran last block: run one more with every send ramping to 0
    BiquadState lfeLp_[2];

    // Reverb: parameters from the game thread; the processor and scratch belong to the audio thread.
    std::atomic<f32> rvWet_{0.0f}, rvDecay_{1.5f}, rvDamp_{0.4f};
    Reverb reverb_;
    std::vector<f32> sendBuf_, rvL_, rvR_;
    f32* sendPtr_ = nullptr;          // non-null only while this block feeds the reverb
    f32  rvPrevWet_ = 0.0f;
    bool rvDirty_ = false;            // the reverb holds a tail that must be cleared when it stops

    // Zone send reverbs. zonePtr_[s] is non-null only while this chunk's voices feed slot s; zoneRan_[s] says the slot processes this chunk.
    ZoneSlot zones_[kMaxZoneReverbs];
    f32* zonePtr_[kMaxZoneReverbs] = {};
    bool zoneRan_[kMaxZoneReverbs] = {};
    bool zoneAny_ = false;
};

// Distance attenuation: 1 at or inside the inner radius, 0 at or beyond the outer.
f32 attenuationAt(const Attenuation& a, f32 distanceCm);

// Constant-power pan. `pan` is -1 (hard left) to +1 (hard right); l*l + r*r == 1.
void panGains(f32 pan, f32& outL, f32& outR);

} // namespace aver::audio
