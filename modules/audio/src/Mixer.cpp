// Mixer implementation: sound slots, the voice pool, streams, and the block render.
#include "aver/audio/Mixer.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace aver::audio {
namespace {

constexpr f32 kMinGain = 1.0e-4f;   // below this a stopping voice is finished
constexpr f32 kOcclusionTau = 0.08f;   // seconds; how fast a voice follows a new occlusion value
constexpr u32 kMaxSendFrames = 16384;  // reverb scratch; a longer mix() call skips the reverb
constexpr f32 kZoneParamTau = 0.12f;   // seconds; how fast a running zone reverb follows a new decay / damping
constexpr u32 kUnderrunTail = 64;      // frames faded out when a stream runs dry mid-block
constexpr u32 kNoSlot = 0xFFFFFFFFu;
constexpr u32 kGraphChunk = 512;       // frames of a graph voice evaluated at a time
constexpr f32 kCentreGain = 1.41421356f;   // 1 / cos(45 degrees): a mono source at the centre of the stereo pan is unity on each side
constexpr f32 kLfeCutoffHz = 120.0f;       // the LFE send is low-passed here (two cascaded sections)

// Clamps v to [lo, hi].
f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Dot of a 3-element array with three scalars.
f32 dot3(const f32 a[3], f32 x, f32 y, f32 z) { return a[0] * x + a[1] * y + a[2] * z; }

} // namespace

// Distance attenuation: 1 at or inside the inner radius, 0 at or beyond the outer.
f32 attenuationAt(const Attenuation& a, f32 distanceCm) {
    const f32 inner = a.innerRadius > 0.0f ? a.innerRadius : 1.0f;
    const f32 outer = a.outerRadius > inner ? a.outerRadius : inner + 1.0f;
    if (distanceCm <= inner) return 1.0f;
    if (distanceCm >= outer) return 0.0f;

    const f32 raw   = inner / distanceCm;
    const f32 floor_ = inner / outer;
    return (raw - floor_) / (1.0f - floor_);
}

// Constant-power pan. pan is -1 (left) to +1 (right); l*l + r*r == 1.
void panGains(f32 pan, f32& outL, f32& outR) {
    const f32 t = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265358979f;
    outL = std::cos(t);
    outR = std::sin(t);
}

// Shuts the mixer down.
Mixer::~Mixer() { shutdown(); }

// Sizes the voice, sound and stream pools and makes the mixer ready. False on bad arguments.
bool Mixer::init(u32 sampleRate, u32 channels, u32 maxVoices, u32 maxSounds, u32 maxStreams) {
    if (ready_) shutdown();
    if (sampleRate == 0) { AVER_ERROR("[Audio] init with a zero sample rate"); return false; }
    if (!validMixChannels(channels)) {
        AVER_ERROR("[Audio] init with {} channels; only mono, stereo, 5.1 and 7.1 (1, 2, 6, 8) are supported", channels);
        return false;
    }
    if (maxVoices == 0 || maxSounds == 0) { AVER_ERROR("[Audio] init with an empty pool"); return false; }

    sampleRate_ = sampleRate;
    channels_   = channels;

    { std::vector<Voice>      v(maxVoices); voices_.swap(v); }
    { std::vector<SoundSlot>  s(maxSounds); sounds_.swap(s); }
    { std::vector<StreamSlot> t(maxStreams); streams_.swap(t); }

    master_.store(1.0f, std::memory_order_relaxed);
    for (usize i = 0; i < static_cast<usize>(Bus::Count); ++i)
        buses_[i].store(1.0f, std::memory_order_relaxed);

    Listener l;
    setListener(l);

    occMinCutoff_.store(450.0f, std::memory_order_relaxed);
    occMinVolume_.store(0.30f, std::memory_order_relaxed);

    for (int i = 0; i < 3; ++i) lisVel_[i].store(0.0f, std::memory_order_relaxed);
    speedScale_.store(1.0f, std::memory_order_relaxed);
    graph_.init(sampleRate);

    reverb_.init(sampleRate);
    sendBuf_.assign(kMaxSendFrames, 0.0f);
    rvL_.assign(kMaxSendFrames, 0.0f);
    rvR_.assign(kMaxSendFrames, 0.0f);
    sendPtr_ = nullptr;
    lfeBuf_.assign(kMaxSendFrames, 0.0f);
    lfePtr_ = nullptr;
    lfeTail_ = false;
    lfeLp_[0].reset();
    lfeLp_[1].reset();
    graphScratch_.assign(kGraphChunk, 0.0f);
    graphSeed_.store(1, std::memory_order_relaxed);
    rvPrevWet_ = 0.0f;
    rvDirty_ = false;
    rvWet_.store(0.0f, std::memory_order_relaxed);
    rvDecay_.store(1.5f, std::memory_order_relaxed);
    rvDamp_.store(0.4f, std::memory_order_relaxed);

    // The zone send reverbs are allocated now, so a scene turning one on later costs the audio thread nothing.
    for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
        ZoneSlot& z = zones_[s];
        z.rv.init(sampleRate);
        z.conv = std::make_unique<Convolver>(sampleRate);
        z.send.assign(kMaxSendFrames, 0.0f);
        z.wet.store(0.0f, std::memory_order_relaxed);
        z.decay.store(1.5f, std::memory_order_relaxed);
        z.damp.store(0.4f, std::memory_order_relaxed);
        z.impulse.store(0, std::memory_order_relaxed);
        z.predelayMs.store(0.0f, std::memory_order_relaxed);
        z.prevWet = 0.0f;
        z.smDecay = 1.5f;
        z.smDamp = 0.4f;
        z.dirty = false;
        zonePtr_[s] = nullptr;
        zoneRan_[s] = false;
    }
    zoneAny_ = false;

    playCounter_.store(1, std::memory_order_relaxed);
    stolen_.store(0, std::memory_order_relaxed);
    starved_.store(0, std::memory_order_relaxed);
    streamUnderruns_.store(0, std::memory_order_relaxed);

    ready_ = true;
    AVER_INFO("[Audio] mixer ready: {} Hz, {} channel(s), {} voices, {} sound slots, {} stream slots",
              sampleRate, channels, maxVoices, maxSounds, maxStreams);
    return true;
}

// Releases the pools. The caller must have stopped the device first.
void Mixer::shutdown() {
    ready_ = false;
    graph_.shutdown();
    voices_.clear();
    streams_.clear();   // joins every decoder thread
    sounds_.clear();
    sampleRate_.store(0, std::memory_order_relaxed);
    channels_.store(0, std::memory_order_relaxed);
}

// Moves the mixer to a new device rate and channel count, keeping every voice, stream, sound and bus. No mix() may be running.
bool Mixer::setOutputFormat(u32 rate, u32 channels) {
    if (!ready_ || rate == 0 || !validMixChannels(channels)) return false;
    const u32 oldRate = sampleRate();
    // Another channel count means other gains per voice, so each takes its new gains at once on its next block; another rate means other
    // delay lengths in samples, so the head model restarts (nothing is rendering now).
    const bool channelsChange = channels != this->channels();
    if (channelsChange || rate != oldRate)
        for (Voice& vo : voices_) {
            if (channelsChange) { vo.primed = false; vo.lfeGain = 0.0f; }
            vo.binReady = false;
        }
    lfeTail_ = false;
    lfeLp_[0].reset();
    lfeLp_[1].reset();
    if (rate != oldRate) {
        // Voices read their source at srcRate / deviceRate per output frame, so the new rate needs nothing from them except a fade in
        // progress, which counts in output frames: rescale it so it keeps its length in seconds.
        const f64 k = static_cast<f64>(rate) / static_cast<f64>(oldRate);
        for (Voice& vo : voices_) {
            if (!vo.fadeActive) continue;
            vo.fadeLen = static_cast<u32>(static_cast<f64>(vo.fadeLen) * k + 0.5);
            vo.fadeProgress = static_cast<u32>(static_cast<f64>(vo.fadeProgress) * k + 0.5);
        }
        // A graph voice runs at the device rate: its time-based state keeps its length in seconds and its delay lines are rebuilt.
        for (Voice& vo : voices_)
            if (vo.runner) {
                vo.runner->setSampleRate(rate);
                vo.srcRateHz = static_cast<f64>(rate);
            }
        graph_.setSampleRate(rate);
        reverb_.init(rate);          // delay lines are sized in frames; the tail it held is dropped
        rvPrevWet_ = 0.0f;
        rvDirty_ = false;
        for (ZoneSlot& z : zones_) {
            z.rv.init(rate);
            if (z.conv) z.conv->setSampleRate(rate);   // the impulse is rebuilt for the new rate from its kept source
            z.prevWet = 0.0f;
            z.dirty = false;
        }
    }
    sampleRate_.store(rate, std::memory_order_relaxed);
    channels_.store(channels, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------- sounds

// Takes ownership of decoded audio and returns its handle. 0 when the table is full.
SoundHandle Mixer::addSound(SoundData&& data) {
    if (!ready_) return 0;
    if (!data.valid()) { AVER_ERROR("[Audio] addSound with empty or malformed data"); return 0; }
    for (usize i = 0; i < sounds_.size(); ++i) {
        SoundSlot& s = sounds_[i];
        if (s.data) continue;
        s.data = std::make_unique<SoundData>(std::move(data));
        s.refs.store(0, std::memory_order_relaxed);
        s.retired.store(false, std::memory_order_release);
        return static_cast<SoundHandle>(i + 1);
    }
    AVER_ERROR("[Audio] the sound table is full ({} slots)", sounds_.size());
    return 0;
}

// Marks a sound for release; the bytes go once no voice refers to them.
void Mixer::removeSound(SoundHandle h) {
    if (!ready_ || h == 0 || h > sounds_.size()) return;
    sounds_[h - 1].retired.store(true, std::memory_order_release);
    collect();
}

// Frees retired sounds whose last voice has ended, and stream decoders no voice reads. Game thread only.
void Mixer::collect() {
    if (!ready_) return;
    for (SoundSlot& s : sounds_) {
        if (!s.data) continue;
        if (!s.retired.load(std::memory_order_acquire)) continue;
        if (s.refs.load(std::memory_order_acquire) != 0) continue;
        s.data.reset();
        s.retired.store(false, std::memory_order_release);
    }
    for (StreamSlot& t : streams_) {
        if (!t.dec) continue;
        if (t.attached.load(std::memory_order_acquire)) continue;
        t.dec.reset();
    }
    // Convolution engines the audio thread has swapped out (zone impulses, convolution reverb effects).
    for (ZoneSlot& z : zones_) if (z.conv) z.conv->collect();
    graph_.collect();
    // A graph voice's runner (its state and delay memory) goes once the voice is Free: freed here, never on the audio thread.
    for (Voice& vo : voices_)
        if (vo.runner && vo.state.load(std::memory_order_acquire) == static_cast<u32>(State::Free)) vo.runner.reset();
}

// Drops the references a voice holds. The steal path (game thread, voice Pending) and the end of a voice (audio thread) share it.
void Mixer::releaseVoiceResources(Voice& v) {
    if (v.soundHandle && v.soundHandle <= sounds_.size())
        sounds_[v.soundHandle - 1].refs.fetch_sub(1, std::memory_order_acq_rel);
    if (v.streamSlot != kNoSlot && v.streamSlot < streams_.size())
        streams_[v.streamSlot].attached.store(false, std::memory_order_release);
    for (u32 i = 0; i < v.waveCount; ++i)
        if (v.waveHandles[i] && v.waveHandles[i] <= sounds_.size())
            sounds_[v.waveHandles[i] - 1].refs.fetch_sub(1, std::memory_order_acq_rel);
    v.waveCount = 0;
    v.graphRun = nullptr;
}

// Number of loaded sounds.
u32 Mixer::soundCount() const {
    u32 n = 0;
    for (const SoundSlot& s : sounds_) if (s.data) ++n;
    return n;
}

f64 Mixer::soundLengthSeconds(SoundHandle h) const {
    if (!ready_ || h == 0 || h > sounds_.size() || !sounds_[h - 1].data) return 0.0;
    const SoundData& d = *sounds_[h - 1].data;
    return d.sampleRate ? static_cast<f64>(d.frames()) / static_cast<f64>(d.sampleRate) : 0.0;
}

void Mixer::setSoundLoop(SoundHandle h, u32 beginFrame, u32 endFrame) {
    if (!ready_ || h == 0 || h > sounds_.size() || !sounds_[h - 1].data) return;
    SoundData& d = *sounds_[h - 1].data;
    d.loopBegin = beginFrame;
    d.loopEnd = endFrame;
}

// Number of stream voices owning a decoder.
u32 Mixer::activeStreams() const {
    u32 n = 0;
    for (const StreamSlot& t : streams_)
        if (t.dec && t.attached.load(std::memory_order_acquire)) ++n;
    return n;
}

// ---------------------------------------------------------------- voices

// Live voice for a handle, or nullptr when the handle is stale or free.
Mixer::Voice* Mixer::resolve(VoiceHandle v) {
    if (!ready_ || v == 0) return nullptr;
    const u32 i = indexOf(v);
    if (i >= voices_.size()) return nullptr;
    Voice& vo = voices_[i];
    if (vo.generation.load(std::memory_order_acquire) != generationOf(v)) return nullptr;
    if (vo.state.load(std::memory_order_acquire) == static_cast<u32>(State::Free)) return nullptr;
    return &vo;
}

const Mixer::Voice* Mixer::resolve(VoiceHandle v) const {
    return const_cast<Mixer*>(this)->resolve(v);
}

// Picks the quietest, then oldest, Active voice to steal. Returns 0xFFFFFFFF when there is none.
u32 Mixer::stealSlot() {
    u32 best = 0xFFFFFFFFu;
    f32 bestGain = 0.0f;
    u64 bestOrder = 0;
    for (u32 i = 0; i < voices_.size(); ++i) {
        Voice& vo = voices_[i];
        if (!isLiveState(vo.state.load(std::memory_order_acquire))) continue;
        f32 l = 0.0f, r = 0.0f;
        voiceGains(vo, l, r);
        const f32 g = l > r ? l : r;
        if (best == 0xFFFFFFFFu || g < bestGain || (g == bestGain && vo.startOrder < bestOrder)) {
            best = i; bestGain = g; bestOrder = vo.startOrder;
        }
    }
    return best;
}

// Claims a voice slot (Pending), stealing the quietest voice if the pool is full.
u32 Mixer::acquireVoice() {
    auto claim = [&](u32 i) -> bool {
        u32 expected = static_cast<u32>(State::Free);
        return voices_[i].state.compare_exchange_strong(
            expected, static_cast<u32>(State::Pending),
            std::memory_order_acq_rel, std::memory_order_relaxed);
    };

    u32 index = 0xFFFFFFFFu;
    for (u32 i = 0; i < voices_.size(); ++i) if (claim(i)) { index = i; break; }

    if (index == 0xFFFFFFFFu) {
        // RETRIED, because a failed CAS here no longer means "someone else took it". It now also
        // means "the audio thread is inside this voice right now" (State::Rendering), which is a
        // transient the caller should not be punished for -- returning 0 there would drop a sound
        // for the duration of one audio block, at random, only under load. A few attempts is
        // enough: the render claim lasts one callback.
        for (u32 attempt = 0; attempt < 4 && index == 0xFFFFFFFFu; ++attempt) {
            const u32 victim = stealSlot();
            if (victim == 0xFFFFFFFFu) return 0xFFFFFFFFu;
            u32 expected = static_cast<u32>(State::Active);
            if (!voices_[victim].state.compare_exchange_strong(
                    expected, static_cast<u32>(State::Pending),
                    std::memory_order_acq_rel, std::memory_order_relaxed))
                continue;
            // Won it. From here the voice is Pending, so the audio thread will not enter it.
            Voice& v = voices_[victim];
            releaseVoiceResources(v);
            v.stream = nullptr;
            v.streamSlot = kNoSlot;
            v.generation.fetch_add(1, std::memory_order_acq_rel);
            index = victim;
            stolen_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // The slot is ours (Pending), so its previous graph runner, if any, is dead weight: freed here on the game thread, and what is left
    // of `runner` afterwards is the answer to "is this voice a graph".
    if (index != 0xFFFFFFFFu) voices_[index].runner.reset();
    return index;
}

// Fills a claimed voice from the description and publishes it Active.
VoiceHandle Mixer::activate(u32 index, const PlayDesc& desc, f64 startCursor, f64 lengthSec, f64 srcRate, u64 streamStart) {
    Voice& vo = voices_[index];
    vo.looping     = desc.looping;
    vo.bus         = desc.bus < Bus::Count ? desc.bus : Bus::Sfx;
    vo.positional  = desc.positional;
    vo.attenuation = desc.attenuation;
    vo.startOrder  = playCounter_.fetch_add(1, std::memory_order_relaxed);
    vo.cursor      = startCursor;
    vo.lengthSec   = lengthSec;
    vo.srcRateHz   = srcRate;
    vo.streamStart = streamStart;
    vo.streamFrames = 0;
    vo.posPub.store(startCursor, std::memory_order_relaxed);
    vo.graphBus.store(desc.graphBus, std::memory_order_relaxed);
    vo.paused.store(desc.paused, std::memory_order_relaxed);
    vo.pauseSilenced = false;
    vo.shelfActive = false;
    vo.shL.reset();
    vo.shR.reset();
    vo.spatial3d.store(false, std::memory_order_relaxed);
    if (desc.spatialModel) {
        f32 packed[Spatial3D::kFloats];
        desc.spatial.pack(packed);
        for (u32 i = 0; i < Spatial3D::kFloats; ++i) vo.sp[i].store(packed[i], std::memory_order_relaxed);
        vo.spatial3d.store(true, std::memory_order_relaxed);
    }
    vo.gainL = vo.gainR = 0.0f;
    vo.primed = false;
    vo.resumeRamp = false;
    vo.stopping.store(false, std::memory_order_relaxed);
    vo.volume.store(desc.volume < 0.0f ? 0.0f : desc.volume, std::memory_order_relaxed);
    vo.pitch.store(desc.pitch > 0.0f ? desc.pitch : 1.0f, std::memory_order_relaxed);
    vo.posX.store(desc.position[0], std::memory_order_relaxed);
    vo.posY.store(desc.position[1], std::memory_order_relaxed);
    vo.posZ.store(desc.position[2], std::memory_order_relaxed);

    vo.fadeSeen = vo.fadeSeq.load(std::memory_order_relaxed);
    vo.fadeProgress = vo.fadeLen = 0;
    vo.fadeFrom = vo.fadeTo = vo.fadeCur = 1.0f;
    vo.fadeActive = vo.fadeStopAtEnd = false;
    vo.fadePub.store(1.0f, std::memory_order_relaxed);

    vo.occlusion.store(0.0f, std::memory_order_relaxed);
    vo.occSmooth = 0.0f;
    vo.occPrimed = false;
    vo.filterActive = false;
    vo.filtL.reset();
    vo.filtR.reset();
    const f32 send = desc.reverbSend >= 0.0f ? desc.reverbSend
                   : (vo.bus == Bus::Sfx || vo.bus == Bus::Voice ? 1.0f : 0.0f);
    vo.reverbSend.store(send, std::memory_order_relaxed);
    vo.sendGain = 0.0f;
    for (u32 s = 0; s < kMaxZoneReverbs; ++s) { vo.zoneSend[s].store(0.0f, std::memory_order_relaxed); vo.zoneGain[s] = 0.0f; }

    if (desc.fadeInSeconds > 0.0f) {
        vo.fadeCur = 0.0f;
        vo.fadePub.store(0.0f, std::memory_order_relaxed);
        vo.primed = true;     // gains start at zero and ramp, rather than snapping to the first block's value
        requestFade(vo, 1.0f, static_cast<u32>(desc.fadeInSeconds * static_cast<f32>(sampleRate())),
                    desc.fadeCurve, false);
    }

    const u32 gen = vo.generation.load(std::memory_order_relaxed);
    vo.state.store(static_cast<u32>(State::Active), std::memory_order_release);
    return encode(index, gen);
}

// Starts a voice, stealing one if the pool is full. Returns its handle, or 0.
VoiceHandle Mixer::play(const PlayDesc& desc) {
    if (!ready_) return 0;
    if (desc.sound == 0 || desc.sound > sounds_.size()) return 0;
    SoundSlot& slot = sounds_[desc.sound - 1];
    if (!slot.data) return 0;
    if (slot.retired.load(std::memory_order_acquire)) return 0;

    const u32 index = acquireVoice();
    if (index == 0xFFFFFFFFu) return 0;

    Voice& vo = voices_[index];
    vo.sound       = slot.data.get();
    vo.soundHandle = desc.sound;
    vo.stream      = nullptr;
    vo.streamSlot  = kNoSlot;

    slot.refs.fetch_add(1, std::memory_order_acq_rel);
    const f64 rate = static_cast<f64>(slot.data->sampleRate);
    const f64 frames = static_cast<f64>(slot.data->frames());
    f64 start = desc.startSeconds > 0.0f ? static_cast<f64>(desc.startSeconds) * rate : 0.0;
    if (start >= frames) start = 0.0;
    return activate(index, desc, start, rate > 0.0 ? frames / rate : 0.0, rate);
}

// Starts a streamed voice. The decoder is owned by a stream slot until collect() frees it.
VoiceHandle Mixer::playStream(std::unique_ptr<StreamSource> source, const PlayDesc& desc, StreamParams params) {
    if (!ready_ || !source) return 0;
    collect();

    u32 slot = kNoSlot;
    for (u32 i = 0; i < streams_.size(); ++i) if (!streams_[i].dec) { slot = i; break; }
    if (slot == kNoSlot) { AVER_WARN("[Audio] all {} stream slots are in use", streams_.size()); return 0; }

    params.looping = desc.looping;
    const f64 srcRate = static_cast<f64>(source->sampleRate());
    const u64 total = source->totalFrames();
    if (desc.startSeconds > 0.0f && params.startFrame == 0) params.startFrame = static_cast<u64>(static_cast<f64>(desc.startSeconds) * srcRate);
    if (total && params.startFrame >= total) params.startFrame = 0;
    const u64 startFrame = params.startFrame;
    auto dec = std::make_unique<StreamDecoder>(std::move(source), params);
    if (!dec->valid()) return 0;

    const u32 index = acquireVoice();
    if (index == 0xFFFFFFFFu) return 0;

    streams_[slot].dec = std::move(dec);
    streams_[slot].attached.store(true, std::memory_order_release);

    Voice& vo = voices_[index];
    vo.sound       = nullptr;
    vo.soundHandle = 0;
    vo.stream      = streams_[slot].dec.get();
    vo.streamSlot  = slot;
    return activate(index, desc, 0.0, srcRate > 0.0 ? static_cast<f64>(total) / srcRate : 0.0, srcRate, startFrame);
}

// A voice fed by the game thread. The ring is a live StreamDecoder; running dry is an under-run, not the end.
VoiceHandle Mixer::playLive(const PlayDesc& desc, u32 rate, u32 channels, u32 ringFrames) {
    if (!ready_ || rate == 0 || (channels != 1 && channels != 2)) return 0;
    collect();
    u32 slot = kNoSlot;
    for (u32 i = 0; i < streams_.size(); ++i) if (!streams_[i].dec) { slot = i; break; }
    if (slot == kNoSlot) { AVER_WARN("[Audio] all {} stream slots are in use", streams_.size()); return 0; }

    StreamParams params;
    params.live = true;
    params.threaded = false;
    params.looping = false;
    params.ringFrames = ringFrames ? ringFrames : rate / 2;
    auto dec = std::make_unique<StreamDecoder>(std::make_unique<LiveSource>(channels, rate), params);
    if (!dec->valid()) return 0;

    const u32 index = acquireVoice();
    if (index == 0xFFFFFFFFu) return 0;
    streams_[slot].dec = std::move(dec);
    streams_[slot].attached.store(true, std::memory_order_release);

    Voice& vo = voices_[index];
    vo.sound       = nullptr;
    vo.soundHandle = 0;
    vo.stream      = streams_[slot].dec.get();
    vo.streamSlot  = slot;
    return activate(index, desc, 0.0, 0.0, static_cast<f64>(rate), 0);
}

u32 Mixer::pushLive(VoiceHandle v, const f32* frames, u32 count) {
    Voice* vo = resolve(v);
    return (vo && vo->stream && frames) ? vo->stream->pushLive(frames, count) : 0;
}

u32 Mixer::liveFree(VoiceHandle v) const {
    const Voice* vo = resolve(v);
    return (vo && vo->stream) ? vo->stream->freeFrames() : 0;
}

void Mixer::liveFlush(VoiceHandle v) {
    if (Voice* vo = resolve(v)) if (vo->stream) vo->stream->requestFlush();
}

u32 Mixer::voiceUnderruns(VoiceHandle v) const {
    const Voice* vo = resolve(v);
    return (vo && vo->stream) ? vo->stream->underruns() : 0;
}

// A graph voice. Everything that allocates (the runner, its delay memory) happens here, before the voice is published.
VoiceHandle Mixer::playGraph(std::shared_ptr<const SoundProgram> program, const SoundHandle* waves, u32 waveCount, const PlayDesc& desc,
                             u32 seed, const GraphParam* initial, u32 initialCount) {
    if (!ready_ || !program) return 0;
    RunnerOptions ro;
    ro.sampleRate = sampleRate();
    ro.seed = seed ? seed : graphSeed_.fetch_add(1, std::memory_order_relaxed) * 2246822519u + 3266489917u;
    ro.finite = !desc.looping;
    const f32 lengthSec = ro.finite ? program->durationSec() : 0.0f;
    const u32 slots = program->waveCount();
    auto run = std::make_unique<SoundRunner>(std::move(program), ro);
    for (u32 i = 0; initial && i < initialCount; ++i) {
        const i32 id = run->program().findParam(initial[i].name);
        if (id >= 0) run->setParam(static_cast<u32>(id), initial[i].value);
    }

    const u32 index = acquireVoice();
    if (index == 0xFFFFFFFFu) return 0;

    Voice& vo = voices_[index];
    u32 held = 0;
    for (u32 s = 0; s < slots && s < waveCount && waves; ++s) {
        const SoundHandle h = waves[s];
        if (h == 0 || h > sounds_.size()) continue;
        SoundSlot& slot = sounds_[h - 1];
        if (!slot.data || slot.retired.load(std::memory_order_acquire)) continue;
        slot.refs.fetch_add(1, std::memory_order_acq_rel);
        run->setWave(s, slot.data.get());
        vo.waveHandles[held++] = h;
    }
    vo.waveCount = held;
    vo.runner = std::move(run);          // the previous runner of this slot dies here, on the game thread
    vo.graphRun = vo.runner.get();
    vo.sound       = nullptr;
    vo.soundHandle = 0;
    vo.stream      = nullptr;
    vo.streamSlot  = kNoSlot;
    return activate(index, desc, 0.0, static_cast<f64>(lengthSec), static_cast<f64>(sampleRate()), 0);
}

bool Mixer::setGraphParam(VoiceHandle v, const char* name, f32 value) {
    Voice* vo = resolve(v);
    if (!vo || !vo->runner) return false;
    const i32 id = vo->runner->program().findParam(name);
    return id >= 0 && vo->runner->setParam(static_cast<u32>(id), value);
}

bool Mixer::setGraphParamId(VoiceHandle v, u32 id, f32 value) {
    Voice* vo = resolve(v);
    return vo && vo->runner && vo->runner->setParam(id, value);
}

i32 Mixer::graphParamId(VoiceHandle v, const char* name) const {
    const Voice* vo = resolve(v);
    return (vo && vo->runner) ? vo->runner->program().findParam(name) : -1;
}

void Mixer::graphNoteOff(VoiceHandle v) {
    if (Voice* vo = resolve(v)) if (vo->runner) vo->runner->noteOff();
}

bool Mixer::isGraphVoice(VoiceHandle v) const {
    const Voice* vo = resolve(v);
    return vo && vo->runner;
}

void Mixer::setVoiceGraphBus(VoiceHandle v, i32 bus) {
    if (Voice* vo = resolve(v)) vo->graphBus.store(bus, std::memory_order_relaxed);
}

void Mixer::setVoiceSpatial(VoiceHandle v, const Spatial3D& s) {
    Voice* vo = resolve(v);
    if (!vo) return;
    f32 packed[Spatial3D::kFloats];
    s.pack(packed);
    for (u32 i = 0; i < Spatial3D::kFloats; ++i) vo->sp[i].store(packed[i], std::memory_order_relaxed);
    vo->spatial3d.store(true, std::memory_order_release);
}

void Mixer::setVoicePaused(VoiceHandle v, bool paused) {
    if (Voice* vo = resolve(v)) vo->paused.store(paused, std::memory_order_release);
}

f64 Mixer::voicePositionSeconds(VoiceHandle v) const {
    const Voice* vo = resolve(v);
    if (!vo || vo->srcRateHz <= 0.0) return -1.0;
    f64 s = vo->posPub.load(std::memory_order_relaxed) / vo->srcRateHz;
    if (vo->looping && vo->lengthSec > 0.0) s = std::fmod(s, vo->lengthSec);
    return s;
}

f64 Mixer::voiceLengthSeconds(VoiceHandle v) const {
    const Voice* vo = resolve(v);
    return vo ? vo->lengthSec : 0.0;
}

void Mixer::setListenerVelocity(f32 x, f32 y, f32 z) {
    lisVel_[0].store(x, std::memory_order_relaxed);
    lisVel_[1].store(y, std::memory_order_relaxed);
    lisVel_[2].store(z, std::memory_order_relaxed);
}

// Flags a voice to fade out over one block. The audio thread frees the slot.
void Mixer::stop(VoiceHandle v) {
    if (Voice* vo = resolve(v)) vo->stopping.store(true, std::memory_order_release);
}

// Flags every active voice to fade out.
void Mixer::stopAll() {
    if (!ready_) return;
    for (Voice& vo : voices_)
        if (isLiveState(vo.state.load(std::memory_order_acquire)))
            vo.stopping.store(true, std::memory_order_release);
}

// Whether the handle still names a live voice.
bool Mixer::playing(VoiceHandle v) const {
    const Voice* vo = resolve(v);
    return vo && isLiveState(vo->state.load(std::memory_order_acquire));
}

void Mixer::setVoiceVolume(VoiceHandle v, f32 volume) {
    if (Voice* vo = resolve(v)) vo->volume.store(volume < 0.0f ? 0.0f : volume, std::memory_order_relaxed);
}

void Mixer::setVoicePitch(VoiceHandle v, f32 pitch) {
    if (Voice* vo = resolve(v)) vo->pitch.store(pitch > 0.0f ? pitch : 1.0f, std::memory_order_relaxed);
}

void Mixer::setVoicePosition(VoiceHandle v, f32 x, f32 y, f32 z) {
    if (Voice* vo = resolve(v)) {
        vo->posX.store(x, std::memory_order_relaxed);
        vo->posY.store(y, std::memory_order_relaxed);
        vo->posZ.store(z, std::memory_order_relaxed);
    }
}

void Mixer::rebase(f32 dx, f32 dy, f32 dz) {
    for (Voice& vo : voices_) {
        if (vo.state.load(std::memory_order_relaxed) == static_cast<u32>(State::Free)) continue;
        vo.posX.store(vo.posX.load(std::memory_order_relaxed) - dx, std::memory_order_relaxed);
        vo.posY.store(vo.posY.load(std::memory_order_relaxed) - dy, std::memory_order_relaxed);
        vo.posZ.store(vo.posZ.load(std::memory_order_relaxed) - dz, std::memory_order_relaxed);
    }
    const f32 d[3] = {dx, dy, dz};
    for (int i = 0; i < 3; ++i) lisPos_[i].store(lisPos_[i].load(std::memory_order_relaxed) - d[i], std::memory_order_relaxed);
}

// Publishes a fade request; the audio thread latches it at its next block.
void Mixer::requestFade(Voice& vo, f32 target, u32 frames, FadeCurve curve, bool stopAtEnd) {
    vo.fadeTarget.store(target < 0.0f ? 0.0f : target, std::memory_order_relaxed);
    vo.fadeFrames.store(frames, std::memory_order_relaxed);
    vo.fadeCurveReq.store(static_cast<u32>(curve), std::memory_order_relaxed);
    vo.fadeStopReq.store(stopAtEnd, std::memory_order_relaxed);
    vo.fadeSeq.fetch_add(1, std::memory_order_release);
}

void Mixer::fadeVoice(VoiceHandle v, f32 targetGain, f32 seconds, FadeCurve curve, bool stopAtEnd) {
    Voice* vo = resolve(v);
    if (!vo) return;
    const f32 s = seconds > 0.0f ? seconds : 0.0f;
    requestFade(*vo, targetGain, static_cast<u32>(s * static_cast<f32>(sampleRate())), curve, stopAtEnd);
}

void Mixer::setVoiceOcclusion(VoiceHandle v, f32 occlusion) {
    if (Voice* vo = resolve(v)) vo->occlusion.store(clampf(occlusion, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Mixer::setVoiceReverbSend(VoiceHandle v, f32 send) {
    if (Voice* vo = resolve(v)) vo->reverbSend.store(clampf(send, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Mixer::setOcclusionCurve(const OcclusionCurve& c) {
    occMinCutoff_.store(c.minCutoffHz, std::memory_order_relaxed);
    occMinVolume_.store(clampf(c.minVolume, 0.0f, 1.0f), std::memory_order_relaxed);
}

OcclusionCurve Mixer::occlusionCurve() const {
    OcclusionCurve c;
    c.minCutoffHz = occMinCutoff_.load(std::memory_order_relaxed);
    c.minVolume   = occMinVolume_.load(std::memory_order_relaxed);
    return c;
}

void Mixer::setReverb(const ReverbParams& p) {
    rvWet_.store(p.wet < 0.0f ? 0.0f : p.wet, std::memory_order_relaxed);
    rvDecay_.store(p.decaySec, std::memory_order_relaxed);
    rvDamp_.store(p.damping, std::memory_order_relaxed);
}

ReverbParams Mixer::reverb() const {
    ReverbParams p;
    p.wet      = rvWet_.load(std::memory_order_relaxed);
    p.decaySec = rvDecay_.load(std::memory_order_relaxed);
    p.damping  = rvDamp_.load(std::memory_order_relaxed);
    return p;
}

// ---------------------------------------------------------------- zone send reverbs

bool Mixer::setZoneReverb(u32 slot, const ZoneReverb& zone) {
    if (!ready_ || slot >= kMaxZoneReverbs) return false;
    ZoneSlot& z = zones_[slot];
    if (zone.impulse != 0) {
        if (zone.impulse > sounds_.size()) return false;
        const SoundSlot& s = sounds_[zone.impulse - 1];
        if (!s.data || s.retired.load(std::memory_order_acquire)) return false;
        // Rebuild only when the sound changed: building the partitions is the expensive part and a scene re-sends the same slot every tick.
        if (z.impulse.load(std::memory_order_relaxed) != zone.impulse || !z.conv->hasImpulse()) {
            if (!z.conv->setImpulse(*s.data)) return false;
            z.impulse.store(zone.impulse, std::memory_order_relaxed);
        }
        const f32 pd = zone.predelayMs < 0.0f ? 0.0f : zone.predelayMs;
        z.predelayMs.store(pd, std::memory_order_relaxed);
    } else if (z.impulse.load(std::memory_order_relaxed) != 0) {
        z.conv->clearImpulse();
        z.impulse.store(0, std::memory_order_relaxed);
    }
    z.decay.store(zone.reverb.decaySec, std::memory_order_relaxed);
    z.damp.store(zone.reverb.damping, std::memory_order_relaxed);
    z.wet.store(zone.reverb.wet < 0.0f ? 0.0f : zone.reverb.wet, std::memory_order_relaxed);
    return true;
}

ZoneReverb Mixer::zoneReverb(u32 slot) const {
    ZoneReverb r;
    if (slot >= kMaxZoneReverbs) return r;
    const ZoneSlot& z = zones_[slot];
    r.reverb.wet = z.wet.load(std::memory_order_relaxed);
    r.reverb.decaySec = z.decay.load(std::memory_order_relaxed);
    r.reverb.damping = z.damp.load(std::memory_order_relaxed);
    r.impulse = z.impulse.load(std::memory_order_relaxed);
    r.predelayMs = z.predelayMs.load(std::memory_order_relaxed);
    return r;
}

void Mixer::setVoiceZoneSend(VoiceHandle v, u32 slot, f32 send) {
    if (slot >= kMaxZoneReverbs) return;
    if (Voice* vo = resolve(v)) vo->zoneSend[slot].store(clampf(send, 0.0f, 1.0f), std::memory_order_relaxed);
}

bool Mixer::setEffectImpulse(u32 effectId, SoundHandle ir) {
    if (!ready_ || ir == 0 || ir > sounds_.size()) return false;
    const SoundSlot& s = sounds_[ir - 1];
    if (!s.data || s.retired.load(std::memory_order_acquire)) return false;
    Effect* fx = graph_.effect(effectId);
    return fx && fx->setImpulse(*s.data);
}

// Moves the listener. Positions and axes in engine space.
void Mixer::setListener(const Listener& l) {
    for (int i = 0; i < 3; ++i) {
        lisPos_[i].store(l.position[i], std::memory_order_relaxed);
        lisFwd_[i].store(l.forward[i], std::memory_order_relaxed);
        lisRight_[i].store(l.right[i], std::memory_order_relaxed);
    }
}

Listener Mixer::listener() const {
    Listener l;
    for (int i = 0; i < 3; ++i) {
        l.position[i] = lisPos_[i].load(std::memory_order_relaxed);
        l.forward[i]  = lisFwd_[i].load(std::memory_order_relaxed);
        l.right[i]    = lisRight_[i].load(std::memory_order_relaxed);
    }
    return l;
}

void Mixer::setBusVolume(Bus b, f32 volume) {
    if (b < Bus::Count) buses_[static_cast<usize>(b)].store(volume < 0.0f ? 0.0f : volume, std::memory_order_relaxed);
}

f32 Mixer::busVolume(Bus b) const {
    return b < Bus::Count ? buses_[static_cast<usize>(b)].load(std::memory_order_relaxed) : 0.0f;
}

void Mixer::setMasterVolume(f32 volume) { master_.store(volume < 0.0f ? 0.0f : volume, std::memory_order_relaxed); }
f32  Mixer::masterVolume() const { return master_.load(std::memory_order_relaxed); }

// Decodes into every manually pumped stream.
void Mixer::pumpStreams(u32 frames) {
    for (StreamSlot& t : streams_)
        if (t.dec && !t.dec->threaded()) t.dec->pump(frames);
}

// Number of voices currently playing.
u32 Mixer::activeVoices() const {
    u32 n = 0;
    for (const Voice& vo : voices_)
        if (isLiveState(vo.state.load(std::memory_order_acquire))) ++n;
    return n;
}

// ---------------------------------------------------------------- the audio thread

// The left/right gains this voice contributes, after distance, pan, bus, fade and master.
void Mixer::voiceGains(const Voice& vo, f32& outL, f32& outR, f32* shelf, f32* dopplerOut) const {
    if (shelf) *shelf = 1.0f;
    if (dopplerOut) *dopplerOut = 1.0f;
    // A voice on a graph bus takes its volume from the bus, not from the legacy bus and master gains.
    const bool graphVoice = vo.graphBus.load(std::memory_order_relaxed) >= 0;
    const f32 busGain = graphVoice ? 1.0f : buses_[static_cast<usize>(vo.bus)].load(std::memory_order_relaxed);
    const f32 masterGain = graphVoice ? 1.0f : master_.load(std::memory_order_relaxed);
    const f32 route = busGain * masterGain;
    f32 g = vo.volume.load(std::memory_order_relaxed) * busGain * masterGain * vo.fadePub.load(std::memory_order_relaxed);   // the legacy order of products

    if (!vo.positional) { outL = outR = g; return; }

    if (vo.spatial3d.load(std::memory_order_acquire)) {
        f32 packed[Spatial3D::kFloats];
        for (u32 i = 0; i < Spatial3D::kFloats; ++i) packed[i] = vo.sp[i].load(std::memory_order_relaxed);
        const Spatial3D s = Spatial3D::unpack(packed);
        const f32 lp[3] = {lisPos_[0].load(std::memory_order_relaxed), lisPos_[1].load(std::memory_order_relaxed), lisPos_[2].load(std::memory_order_relaxed)};
        const f32 lf[3] = {lisFwd_[0].load(std::memory_order_relaxed), lisFwd_[1].load(std::memory_order_relaxed), lisFwd_[2].load(std::memory_order_relaxed)};
        const f32 lr[3] = {lisRight_[0].load(std::memory_order_relaxed), lisRight_[1].load(std::memory_order_relaxed), lisRight_[2].load(std::memory_order_relaxed)};
        const f32 sp[3] = {vo.posX.load(std::memory_order_relaxed), vo.posY.load(std::memory_order_relaxed), vo.posZ.load(std::memory_order_relaxed)};
        const SpatialResult r = spatialize(s, vo.volume.load(std::memory_order_relaxed), lp, lf, lr, sp);
        const f32 base = route * vo.fadePub.load(std::memory_order_relaxed);
        outL = r.gainL * base;
        outR = r.gainR * base;
        if (shelf) *shelf = r.filterGain;
        if (dopplerOut && s.doppler) {
            const f32 lv[3] = {lisVel_[0].load(std::memory_order_relaxed), lisVel_[1].load(std::memory_order_relaxed), lisVel_[2].load(std::memory_order_relaxed)};
            *dopplerOut = dopplerPitch(lp, lv, sp, s.velocity);
        }
        return;
    }

    const f32 dx = vo.posX.load(std::memory_order_relaxed) - lisPos_[0].load(std::memory_order_relaxed);
    const f32 dy = vo.posY.load(std::memory_order_relaxed) - lisPos_[1].load(std::memory_order_relaxed);
    const f32 dz = vo.posZ.load(std::memory_order_relaxed) - lisPos_[2].load(std::memory_order_relaxed);
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    g *= attenuationAt(vo.attenuation, dist);

    f32 pan = 0.0f;
    if (dist > 1.0e-3f) {
        const f32 rx = lisRight_[0].load(std::memory_order_relaxed);
        const f32 ry = lisRight_[1].load(std::memory_order_relaxed);
        const f32 rz = lisRight_[2].load(std::memory_order_relaxed);
        const f32 right[3] = {rx, ry, rz};
        pan = clampf(dot3(right, dx, dy, dz) / dist, -1.0f, 1.0f);

        // FRONT AND BACK, which panning alone cannot express. A source directly ahead and one
        // directly behind both give dot(right, d) == 0 and pan dead centre, so without this the
        // mixer is physically unable to tell you something is behind you -- and the listener's
        // `forward` vector, which every caller supplies, was stored and never read.
        //
        // A flat gain cut rather than a filter: a real head shadows the far ear and dulls the high
        // end, and that needs per-voice filter state this mixer does not keep. 3 dB directly behind
        // is the conventional stand-in, audible as a cue without sounding like a volume bug.
        const f32 fx = lisFwd_[0].load(std::memory_order_relaxed);
        const f32 fy = lisFwd_[1].load(std::memory_order_relaxed);
        const f32 fz = lisFwd_[2].load(std::memory_order_relaxed);
        const f32 fwd[3] = {fx, fy, fz};
        const f32 ahead = clampf(dot3(fwd, dx, dy, dz) / dist, -1.0f, 1.0f);
        constexpr f32 kBackAttenuation = 0.30f;      // directly behind keeps 0.70 -> about -3.1 dB
        if (ahead < 0.0f) g *= 1.0f + kBackAttenuation * ahead;
    }
    f32 pl = 0.0f, pr = 0.0f;
    panGains(pan, pl, pr);
    constexpr f32 kCentre = 1.41421356f;   // 1 / cos(45 degrees)
    outL = g * pl * kCentre;
    outR = g * pr * kCentre;
}

// Where a positional voice is, for the outputs that place sounds in space (surround speakers, the binaural head model). The same products
// as voiceGains (volume, bus, master, fade, distance) without the stereo pan and without its back attenuation.
Mixer::Direction Mixer::voiceDirection(const Voice& vo) const {
    Direction r;
    const bool graphVoice = vo.graphBus.load(std::memory_order_relaxed) >= 0;
    const f32 busGain = graphVoice ? 1.0f : buses_[static_cast<usize>(vo.bus)].load(std::memory_order_relaxed);
    const f32 masterGain = graphVoice ? 1.0f : master_.load(std::memory_order_relaxed);
    const f32 fade = vo.fadePub.load(std::memory_order_relaxed);
    const f32 lp[3] = {lisPos_[0].load(std::memory_order_relaxed), lisPos_[1].load(std::memory_order_relaxed), lisPos_[2].load(std::memory_order_relaxed)};
    const f32 lf[3] = {lisFwd_[0].load(std::memory_order_relaxed), lisFwd_[1].load(std::memory_order_relaxed), lisFwd_[2].load(std::memory_order_relaxed)};
    const f32 lr[3] = {lisRight_[0].load(std::memory_order_relaxed), lisRight_[1].load(std::memory_order_relaxed), lisRight_[2].load(std::memory_order_relaxed)};
    const f32 sp[3] = {vo.posX.load(std::memory_order_relaxed), vo.posY.load(std::memory_order_relaxed), vo.posZ.load(std::memory_order_relaxed)};
    const f32 dx = sp[0] - lp[0], dy = sp[1] - lp[1], dz = sp[2] - lp[2];
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (vo.spatial3d.load(std::memory_order_acquire)) {
        f32 packed[Spatial3D::kFloats];
        for (u32 i = 0; i < Spatial3D::kFloats; ++i) packed[i] = vo.sp[i].load(std::memory_order_relaxed);
        const Spatial3D s = Spatial3D::unpack(packed);
        const SpatialResult sr = spatialize(s, vo.volume.load(std::memory_order_relaxed), lp, lf, lr, sp);
        r.level = sr.distanceGain * busGain * masterGain * fade;
        r.panScale = clampf(s.panningStrength * s.globalPanningStrength * 2.0f, 0.0f, 1.0f);   // the default 0.5 pans fully
    } else {
        r.level = vo.volume.load(std::memory_order_relaxed) * busGain * masterGain * fade * attenuationAt(vo.attenuation, dist);
    }

    if (dist > 1.0e-3f) {
        const f32 x = clampf(dot3(lf, dx, dy, dz) / dist, -1.0f, 1.0f);
        const f32 y = clampf(dot3(lr, dx, dy, dz) / dist, -1.0f, 1.0f);
        // up = forward x right: with +X ahead and +Y right this is +Z.
        const f32 up[3] = {lf[1] * lr[2] - lf[2] * lr[1], lf[2] * lr[0] - lf[0] * lr[2], lf[0] * lr[1] - lf[1] * lr[0]};
        const f32 z = clampf(dot3(up, dx, dy, dz) / dist, -1.0f, 1.0f);
        r.azimuth = std::atan2(y, x);
        r.elevation = std::asin(z);
        r.horizontal = clampf(std::sqrt(x * x + y * y), 0.0f, 1.0f);
    } else {
        r.horizontal = 0.0f;       // inside the head: no direction, so every speaker equally
    }
    return r;
}

// Latches a new fade request, advances the ramp by one block and returns the gain at its end.
f32 Mixer::advanceFade(Voice& vo, u32 frames, bool& stopNow) {
    const u32 seq = vo.fadeSeq.load(std::memory_order_acquire);
    if (seq != vo.fadeSeen) {
        vo.fadeSeen = seq;
        vo.fadeFrom = vo.fadeCur;
        vo.fadeTo = vo.fadeTarget.load(std::memory_order_relaxed);
        vo.fadeLen = vo.fadeFrames.load(std::memory_order_relaxed);
        vo.fadeCurve = vo.fadeCurveReq.load(std::memory_order_relaxed) == 0 ? FadeCurve::Linear
                                                                            : FadeCurve::EqualPower;
        vo.fadeStopAtEnd = vo.fadeStopReq.load(std::memory_order_relaxed);
        vo.fadeProgress = 0;
        vo.fadeActive = true;
    }
    stopNow = false;
    if (!vo.fadeActive) return vo.fadeCur;

    vo.fadeProgress += frames;
    if (vo.fadeLen == 0 || vo.fadeProgress >= vo.fadeLen) {
        vo.fadeCur = vo.fadeTo;
        vo.fadeActive = false;
        stopNow = vo.fadeStopAtEnd;
    } else {
        const f32 p = static_cast<f32>(vo.fadeProgress) / static_cast<f32>(vo.fadeLen);
        const f32 s = fadeShape(p, vo.fadeCurve, vo.fadeTo >= vo.fadeFrom);
        vo.fadeCur = vo.fadeFrom + (vo.fadeTo - vo.fadeFrom) * s;
    }
    vo.fadePub.store(vo.fadeCur, std::memory_order_relaxed);
    return vo.fadeCur;
}

// Adds one voice into the output block. Returns false when the voice has ended.
bool Mixer::renderVoice(Voice& vo, f32* out, u32 frames, u32 outCh) {
    StreamDecoder* st = vo.stream;
    const SoundData* snd = vo.sound;
    SoundRunner* gr = vo.graphRun;
    if (frames == 0) return false;

    // Pause: one block ramps the voice to silence (the cursor is put back afterwards), then it holds and renders nothing.
    const bool stoppingNow = vo.stopping.load(std::memory_order_acquire);
    const bool pausedReq = vo.paused.load(std::memory_order_acquire) && !stoppingNow;
    if (stoppingNow && vo.pauseSilenced) return false;
    if (pausedReq && vo.pauseSilenced) return true;
    if (!pausedReq && vo.pauseSilenced) { vo.pauseSilenced = false; vo.resumeRamp = true; }
    const f64 cursorAtEntry = vo.cursor;
    if (st && st->takeFlush()) { st->consume(st->available()); vo.cursor = 0.0; }

    u32 srcFrames = 0, srcCh = 0, srcRate = 0;
    if (st) {
        srcCh = st->channels();
        srcRate = st->sampleRate();
    } else if (gr) {
        srcCh = 1;                   // a graph makes mono at the device rate
        srcRate = sampleRate();
    } else {
        if (!snd) return false;
        srcFrames = snd->frames();
        if (srcFrames == 0) return false;
        srcCh = snd->channels;
        srcRate = snd->sampleRate;
    }

    const bool stopping = vo.stopping.load(std::memory_order_acquire);
    // A stream still filling its first buffer is silent and does not advance.
    if (st && !st->prebuffered()) return !stopping;

    bool fadeStop = false;
    advanceFade(vo, frames, fadeStop);

    f32 targetL = 0.0f, targetR = 0.0f, shelfGain = 1.0f, doppler = 1.0f;
    if (!stopping) voiceGains(vo, targetL, targetR, &shelfGain, &doppler);
    if (pausedReq) targetL = targetR = 0.0f;

    const u32 deviceRate = sampleRate();
    const f64 step = gr ? 1.0                       // a graph is evaluated at the device rate; its pitch is in the graph
                   : (static_cast<f64>(srcRate) / static_cast<f64>(deviceRate))
                   * static_cast<f64>(vo.pitch.load(std::memory_order_relaxed))
                   * static_cast<f64>(speedScale_.load(std::memory_order_relaxed)) * static_cast<f64>(doppler);

    // The attenuation filter: a one-stage high shelf at the voice's cutoff, only while it has something to take off.
    const bool shelving = shelfGain < 0.999f;
    BiquadCoeffs shelf;
    if (shelving) {
        shelf = filterCoeffs(FilterMode::HighShelf, vo.sp[9].load(std::memory_order_relaxed), 1.0f, shelfGain, 1, static_cast<f32>(deviceRate));
    } else if (vo.shelfActive) {
        vo.shL.reset();
        vo.shR.reset();
    }
    vo.shelfActive = shelving;

    // Where this voice goes in the output. Mono and stereo keep the legacy pan above; a 5.1 / 7.1 mixer pans positional voices over the
    // speakers; a stereo mixer in headphone mode runs positional voices through the head model. `scalar` is the voice's level before any pan.
    const bool multi = outCh > 2;
    const bool bin = outCh == 2 && vo.positional && binaural_.load(std::memory_order_relaxed) && channels() == 2;
    const bool silent = stopping || pausedReq;
    f32 scalar = targetL;
    f32 tgt[kMaxSpeakers] = {};
    BinauralTargets binT;
    if ((multi || bin) && vo.positional) {
        const Direction dir = voiceDirection(vo);
        scalar = silent ? 0.0f : dir.level;
        if (bin) {
            binT = binauralTargets(dir.azimuth, dir.elevation, static_cast<f32>(deviceRate));
            targetL = targetR = scalar;
        } else {
            speakerGains(outCh, dir.azimuth, 1.0f - dir.horizontal * dir.panScale, tgt);
            for (u32 c = 0; c < outCh; ++c) tgt[c] *= scalar * kCentreGain;
        }
    }
    UpmixMatrix um;
    if (multi && !vo.positional) um = stereoUpmix(outCh, static_cast<StereoUpmix>(upmix_.load(std::memory_order_relaxed)));

    // Occlusion: follow the published value, then low-pass and duck.
    const f32 occTarget = clampf(vo.occlusion.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (!vo.occPrimed) { vo.occSmooth = occTarget; vo.occPrimed = true; }
    else vo.occSmooth += (occTarget - vo.occSmooth)
                       * smoothingCoeff(static_cast<f32>(frames) / static_cast<f32>(deviceRate), kOcclusionTau);
    const bool filtering = vo.occSmooth > kOcclusionBypass;
    BiquadCoeffs lp;
    if (filtering) {
        OcclusionCurve curve;
        curve.minCutoffHz = occMinCutoff_.load(std::memory_order_relaxed);
        curve.minVolume   = occMinVolume_.load(std::memory_order_relaxed);
        lp = lowpassCoeffs(occlusionCutoffHz(vo.occSmooth, curve), static_cast<f32>(deviceRate));
        const f32 og = occlusionGain(vo.occSmooth, curve);
        targetL *= og;
        targetR *= og;
        scalar *= og;
        for (u32 c = 0; c < kMaxSpeakers; ++c) tgt[c] *= og;
    } else if (vo.filterActive) {
        vo.filtL.reset();
        vo.filtR.reset();
    }
    vo.filterActive = filtering;

    const bool pointMulti = multi && vo.positional;
    const f32 sendBase = pointMulti ? scalar : 0.5f * (targetL + targetR);
    const f32 sendTarget = sendPtr_ ? sendBase * vo.reverbSend.load(std::memory_order_relaxed) : 0.0f;
    const f32 lfeTarget = (multi && lfePtr_) ? scalar * lfeSend_.load(std::memory_order_relaxed) : 0.0f;

    // Zone send reverbs: only slots that run this chunk are fed; a slot that is off restarts the voice's send from silence.
    f32 zTarget[kMaxZoneReverbs] = {}, dZone[kMaxZoneReverbs] = {};
    bool zoneRun = false;
    if (zoneAny_)
        for (u32 s = 0; s < kMaxZoneReverbs; ++s)
            if (zonePtr_[s]) zTarget[s] = sendBase * clampf(vo.zoneSend[s].load(std::memory_order_relaxed), 0.0f, 1.0f);

    if (!vo.primed) {
        vo.gainL = targetL; vo.gainR = targetR; vo.sendGain = sendTarget; vo.primed = true;
        for (u32 c = 0; c < kMaxSpeakers; ++c) vo.spk[c] = tgt[c];
        vo.lfeGain = lfeTarget;
        for (u32 s = 0; s < kMaxZoneReverbs; ++s) vo.zoneGain[s] = zTarget[s];
    }
    if (vo.resumeRamp) {
        vo.gainL = vo.gainR = 0.0f; vo.sendGain = 0.0f; vo.resumeRamp = false;
        for (u32 c = 0; c < kMaxSpeakers; ++c) vo.spk[c] = 0.0f;
        vo.lfeGain = 0.0f;
        for (u32 s = 0; s < kMaxZoneReverbs; ++s) vo.zoneGain[s] = 0.0f;
    }
    if (zoneAny_)
        for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
            if (!zonePtr_[s]) { vo.zoneGain[s] = 0.0f; continue; }
            dZone[s] = (zTarget[s] - vo.zoneGain[s]) / static_cast<f32>(frames);
            if (zTarget[s] > 0.0f || vo.zoneGain[s] > 0.0f) zoneRun = true;
        }

    const f32 dL = (targetL - vo.gainL) / static_cast<f32>(frames);
    const f32 dR = (targetR - vo.gainR) / static_cast<f32>(frames);
    const f32 dS = (sendTarget - vo.sendGain) / static_cast<f32>(frames);
    f32 dSpk[kMaxSpeakers] = {};
    f32 dLfe = 0.0f;
    if (multi) {
        if (pointMulti) for (u32 c = 0; c < outCh; ++c) dSpk[c] = (tgt[c] - vo.spk[c]) / static_cast<f32>(frames);
        dLfe = (lfeTarget - vo.lfeGain) / static_cast<f32>(frames);
    }
    if (bin) {
        if (!vo.binReady) vo.bin.reset();
        vo.bin.begin(binT, frames, static_cast<f32>(deviceRate), !vo.binReady);
        vo.binReady = true;
    } else {
        vo.binReady = false;
    }

    // How many frames this block can render. A static sound always renders all of them.
    u32 nRender = frames;
    bool underrun = false, streamEnd = false;
    u32 avail = 0;
    if (st) {
        const bool eof = st->endReached();     // before available(): see StreamDecoder::prebuffered
        avail = st->available();
        // A frame needs its own sample and the next one, unless the stream is complete.
        const f64 limit = eof ? static_cast<f64>(avail) : (avail > 0 ? static_cast<f64>(avail - 1) : 0.0);
        if (vo.cursor >= limit) {
            nRender = 0;
        } else {
            const f64 n = std::ceil((limit - vo.cursor) / step);
            if (n < static_cast<f64>(frames)) nRender = static_cast<u32>(n);
        }
        if (nRender < frames) { if (eof) streamEnd = true; else underrun = true; }
        if (underrun) {
            st->noteUnderrun();
            streamUnderruns_.fetch_add(1, std::memory_order_relaxed);
            if (stopping) return false;
            vo.resumeRamp = true;
        }
    }
    // A graph voice renders what the runner has left: a finite sound ends on its length, a held one on its note-off tail.
    bool graphEnd = false;
    u32 gi = 0, gn = 0;
    if (gr) {
        const u64 rem = gr->remainingFrames();
        if (rem < nRender) { nRender = static_cast<u32>(rem); graphEnd = true; }
    }
    const u32 tailLen = underrun ? std::min(nRender, kUnderrunTail) : 0;
    const u32 tailStart = nRender - tailLen;

    const u32 loopEnd   = (snd && snd->loopEnd > 0 && snd->loopEnd <= srcFrames) ? snd->loopEnd : srcFrames;
    const u32 loopBegin = (snd && snd->loopBegin < loopEnd) ? snd->loopBegin : 0;

    bool ended = false;
    for (u32 f = 0; f < nRender; ++f) {
        f32 sl, sr;
        if (st) {
            u32 i0 = static_cast<u32>(vo.cursor);
            if (i0 >= avail) i0 = avail - 1;
            const f32 frac = static_cast<f32>(vo.cursor - static_cast<f64>(i0));
            u32 i1 = i0 + 1;
            if (i1 >= avail) i1 = avail - 1;
            const f32* a = st->frameAt(i0);
            const f32* b = st->frameAt(i1);
            if (srcCh == 1) {
                sl = sr = a[0] + (b[0] - a[0]) * frac;
            } else {
                sl = a[0] + (b[0] - a[0]) * frac;
                sr = a[1] + (b[1] - a[1]) * frac;
            }
        } else if (gr) {
            if (gi == gn) {
                gn = std::min(kGraphChunk, nRender - f);
                const u32 got = gr->process(graphScratch_.data(), gn);
                gi = 0;
                if (got < gn) { nRender = f + got; graphEnd = true; }
                if (got == 0) break;
            }
            sl = sr = graphScratch_[gi++];
        } else {
            if (vo.cursor >= static_cast<f64>(loopEnd)) {
                if (!vo.looping) { ended = true; break; }
                const f64 len = static_cast<f64>(loopEnd - loopBegin);
                if (len <= 0.0) { ended = true; break; }
                while (vo.cursor >= static_cast<f64>(loopEnd)) vo.cursor -= len;
            }

            const u32 i0 = static_cast<u32>(vo.cursor);
            const f32 frac = static_cast<f32>(vo.cursor - static_cast<f64>(i0));
            u32 i1 = i0 + 1;
            if (i1 >= loopEnd) i1 = vo.looping ? loopBegin : (loopEnd > 0 ? loopEnd - 1 : 0);

            if (srcCh == 1) {
                const f32 a = snd->samples[i0], b = snd->samples[i1];
                sl = sr = a + (b - a) * frac;
            } else {
                const usize o0 = static_cast<usize>(i0) * srcCh;
                const usize o1 = static_cast<usize>(i1) * srcCh;
                const f32 al = snd->samples[o0], bl = snd->samples[o1];
                const f32 ar = snd->samples[o0 + 1], br = snd->samples[o1 + 1];
                sl = al + (bl - al) * frac;
                sr = ar + (br - ar) * frac;
            }
        }

        if (filtering) { sl = vo.filtL.process(lp, sl); sr = vo.filtR.process(lp, sr); }
        if (shelving) { sl = vo.shL.process(shelf, sl); sr = vo.shR.process(shelf, sr); }
        if (f >= tailStart && tailLen > 0) {
            const f32 tg = static_cast<f32>(nRender - f) / static_cast<f32>(tailLen + 1);
            sl *= tg;
            sr *= tg;
        }

        vo.gainL += dL;
        vo.gainR += dR;

        if (multi) {
            f32* o = out + static_cast<usize>(f) * outCh;
            if (pointMulti) {
                const f32 m = 0.5f * (sl + sr);
                for (u32 c = 0; c < outCh; ++c) { vo.spk[c] += dSpk[c]; o[c] += m * vo.spk[c]; }
            } else {
                for (u32 c = 0; c < outCh; ++c) o[c] += vo.gainL * (um.l[c] * sl + um.r[c] * sr);
            }
            if (lfePtr_) { vo.lfeGain += dLfe; lfePtr_[f] += 0.5f * (sl + sr) * vo.lfeGain; }
        } else if (bin) {
            f32 hl, hr;
            vo.bin.tick(0.5f * (sl + sr) * vo.gainL, hl, hr);
            out[f * 2 + 0] += hl;
            out[f * 2 + 1] += hr;
        } else if (outCh == 1) {
            out[f] += (sl * vo.gainL + sr * vo.gainR) * 0.5f;
        } else {
            out[f * 2 + 0] += sl * vo.gainL;
            out[f * 2 + 1] += sr * vo.gainR;
        }
        if (sendPtr_) {
            vo.sendGain += dS;
            sendPtr_[f] += 0.5f * (sl + sr) * vo.sendGain;
        }
        if (zoneRun) {
            const f32 m = 0.5f * (sl + sr);
            for (u32 s = 0; s < kMaxZoneReverbs; ++s)
                if (zonePtr_[s]) { vo.zoneGain[s] += dZone[s]; zonePtr_[s][f] += m * vo.zoneGain[s]; }
        }
        vo.cursor += step;
    }
    if (gr && (graphEnd || gr->finished())) ended = true;

    if (st) {
        u32 consumed = static_cast<u32>(vo.cursor);
        if (consumed > avail) consumed = avail;
        st->consume(consumed);
        vo.cursor -= static_cast<f64>(consumed);
        if (vo.cursor < 0.0) vo.cursor = 0.0;
        vo.streamFrames += consumed;
        vo.posPub.store(static_cast<f64>(vo.streamStart + vo.streamFrames), std::memory_order_relaxed);
        if (streamEnd) return false;
        if (st->endReached() && st->available() == 0) return false;
    } else if (!ended) {
        if (pausedReq) vo.cursor = cursorAtEntry;       // the ramp-out block did not move the playhead
        vo.posPub.store(vo.cursor, std::memory_order_relaxed);
    }
    if (pausedReq) vo.pauseSilenced = true;

    if (ended) return false;
    if (fadeStop) return false;
    if (stopping && std::fabs(vo.gainL) < kMinGain && std::fabs(vo.gainR) < kMinGain) return false;
    return true;
}

// Renders frames of interleaved output, overwriting the buffer. Audio thread only.
void Mixer::mix(f32* out, u32 frames) {
    if (!out || frames == 0) return;
    const u32 outCh = channels() ? channels() : 1;
    const u32 samples = frames * outCh;
    if (!ready_) {
        for (u32 i = 0; i < samples; ++i) out[i] = 0.0f;
        starved_.fetch_add(frames, std::memory_order_relaxed);
        return;
    }

    for (u32 i = 0; i < samples; ++i) out[i] = 0.0f;

    // Without the bus graph this is one chunk, the whole call: exactly the mixer that existed before the graph.
    const bool graphOn = graph_.active();
    u32 done = 0;
    while (done < frames) {
        const u32 n = graphOn ? std::min(frames - done, BusGraph::kBlock) : frames;
        mixChunk(out + static_cast<usize>(done) * outCh, n, outCh, graphOn);
        done += n;
    }

    for (u32 i = 0; i < samples; ++i) out[i] = clampf(out[i], -1.0f, 1.0f);
}

// The zone send reverbs' returns, added into the output after the legacy reverb. Audio thread.
void Mixer::mixZoneReturns(f32* out, u32 frames, u32 outCh, const UpmixMatrix& um) {
    zoneAny_ = false;
    const f32 rate = static_cast<f32>(sampleRate());
    for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
        ZoneSlot& z = zones_[s];
        zonePtr_[s] = nullptr;
        if (!zoneRan_[s]) {
            if (z.dirty) { z.rv.reset(); if (z.conv) z.conv->reset(); z.dirty = false; }
            z.prevWet = 0.0f;
            continue;
        }
        zoneRan_[s] = false;

        const f32 wet = z.wet.load(std::memory_order_relaxed);
        const f32 decayT = z.decay.load(std::memory_order_relaxed), dampT = z.damp.load(std::memory_order_relaxed);
        if (!z.dirty) { z.smDecay = decayT; z.smDamp = dampT; }     // a slot starting from silence takes its settings at once
        else {
            const f32 k = smoothingCoeff(static_cast<f32>(frames) / rate, kZoneParamTau);
            z.smDecay += (decayT - z.smDecay) * k;
            z.smDamp += (dampT - z.smDamp) * k;
        }

        if (z.conv->hasImpulse()) {
            z.conv->setPredelayFrames(static_cast<u32>(z.predelayMs.load(std::memory_order_relaxed) * 0.001f * rate));
            z.conv->process(z.send.data(), z.send.data(), rvL_.data(), rvR_.data(), frames);
        } else {
            z.rv.setParams(z.smDecay, z.smDamp);
            z.rv.process(z.send.data(), rvL_.data(), rvR_.data(), frames);
        }

        const f32 step = (wet - z.prevWet) / static_cast<f32>(frames);
        f32 w = z.prevWet;
        for (u32 f = 0; f < frames; ++f) {
            w += step;
            if (outCh > 2) {
                const f32 rl = rvL_[f] * w, rr = rvR_[f] * w;
                for (u32 c = 0; c < outCh; ++c) out[static_cast<usize>(f) * outCh + c] += um.l[c] * rl + um.r[c] * rr;
            } else if (outCh == 1) {
                out[f] += 0.5f * (rvL_[f] + rvR_[f]) * w;
            } else {
                out[f * 2 + 0] += rvL_[f] * w;
                out[f * 2 + 1] += rvR_[f] * w;
            }
        }
        z.prevWet = wet;
        z.dirty = true;
    }
}

// One chunk: voices into the output (legacy) or into their graph buses, the reverb return, then the graph's Master folded in.
void Mixer::mixChunk(f32* out, u32 frames, u32 outCh, bool graphOn) {
    // Reverb runs while it is wanted, and for one more block to ramp its return out.
    const f32 wet = rvWet_.load(std::memory_order_relaxed);
    const bool wantSend = wet > 1.0e-4f;
    const bool rvOn = reverb_.ready() && frames <= sendBuf_.size() && (wantSend || rvPrevWet_ > 1.0e-4f);
    sendPtr_ = nullptr;
    if (rvOn) {
        std::fill(sendBuf_.begin(), sendBuf_.begin() + frames, 0.0f);
        if (wantSend) sendPtr_ = sendBuf_.data();
    }

    // Each zone send reverb runs while it is wanted and for one more block to ramp its return out; voices feed only the wanted ones.
    zoneAny_ = false;
    for (u32 s = 0; s < kMaxZoneReverbs; ++s) {
        ZoneSlot& z = zones_[s];
        const bool want = z.wet.load(std::memory_order_relaxed) > 1.0e-4f;
        const bool run = z.conv && z.rv.ready() && frames <= z.send.size() && (want || z.prevWet > 1.0e-4f);
        zoneRan_[s] = run;
        zonePtr_[s] = nullptr;
        if (!run) continue;
        std::fill(z.send.begin(), z.send.begin() + frames, 0.0f);
        if (want) { zonePtr_[s] = z.send.data(); zoneAny_ = true; }
    }

    // The LFE send runs while it is wanted and for one more block so every voice's send ramps out (5.1 / 7.1 only).
    const bool lfeWanted = outCh > 2 && lfeSend_.load(std::memory_order_relaxed) > 1.0e-6f;
    const bool lfeRun = outCh > 2 && frames <= lfeBuf_.size() && (lfeWanted || lfeTail_);
    lfePtr_ = nullptr;
    if (lfeRun) {
        std::fill(lfeBuf_.begin(), lfeBuf_.begin() + frames, 0.0f);
        lfePtr_ = lfeBuf_.data();
    }

    // The graph's structure lock is held for the chunk; when a script is editing the layout the graph voices wait one block.
    const bool graphLocked = graphOn && graph_.begin();
    if (graphLocked) graph_.clear(frames);

    for (Voice& vo : voices_) {
        // CLAIMED FOR THE DURATION OF THE BLOCK. Reading the state and then rendering left the
        // voice Active throughout, which is the window play()'s stealing CAS used to walk into --
        // dropping the sound's refcount and rewriting vo.sound while this thread was reading them.
        // Taking it out of Active makes that CAS fail instead.
        u32 expected = static_cast<u32>(State::Active);
        if (!vo.state.compare_exchange_strong(expected, static_cast<u32>(State::Rendering),
                                              std::memory_order_acq_rel, std::memory_order_acquire))
            continue;

        const i32 gb = graphOn ? vo.graphBus.load(std::memory_order_relaxed) : -1;
        if (gb >= 0 && !graphLocked) {
            vo.state.store(static_cast<u32>(State::Active), std::memory_order_release);
            continue;
        }
        const bool alive = gb >= 0 ? renderVoice(vo, graph_.busBuffer(gb), frames, 2)
                                   : renderVoice(vo, out, frames, outCh);
        if (alive) {
            // Still sounding: hand it back. A plain store is right -- this thread owns the voice
            // while it is Rendering, so nothing else can have changed the state under it.
            vo.state.store(static_cast<u32>(State::Active), std::memory_order_release);
        } else {
            releaseVoiceResources(vo);
            vo.sound = nullptr;
            vo.soundHandle = 0;
            vo.stream = nullptr;
            vo.streamSlot = kNoSlot;
            vo.generation.fetch_add(1, std::memory_order_acq_rel);
            vo.state.store(static_cast<u32>(State::Free), std::memory_order_release);
        }
    }
    sendPtr_ = nullptr;

    // Stereo returns (the reverb, the bus graph's Master) reach a surround mixer through the stereo upmix.
    UpmixMatrix um;
    if (outCh > 2) um = stereoUpmix(outCh, static_cast<StereoUpmix>(upmix_.load(std::memory_order_relaxed)));

    if (lfePtr_) {
        const BiquadCoeffs lfeLp = lowpassCoeffs(kLfeCutoffHz, static_cast<f32>(sampleRate()));
        for (u32 f = 0; f < frames; ++f)
            out[static_cast<usize>(f) * outCh + kSpeakerLfe] += lfeLp_[1].process(lfeLp, lfeLp_[0].process(lfeLp, lfePtr_[f]));
        // After the send is switched off the filter keeps running on silence until its ring-down is inaudible, so the LFE ends without a step.
        auto ringing = [](const BiquadState& s) { return std::fabs(s.z1) > 1.0e-6f || std::fabs(s.z2) > 1.0e-6f; };
        lfeTail_ = lfeWanted || ringing(lfeLp_[0]) || ringing(lfeLp_[1]);
        lfePtr_ = nullptr;
    } else {
        lfeTail_ = false;
        lfeLp_[0].reset();
        lfeLp_[1].reset();
    }

    if (rvOn) {
        reverb_.setParams(rvDecay_.load(std::memory_order_relaxed), rvDamp_.load(std::memory_order_relaxed));
        reverb_.process(sendBuf_.data(), rvL_.data(), rvR_.data(), frames);
        const f32 step = (wet - rvPrevWet_) / static_cast<f32>(frames);
        f32 w = rvPrevWet_;
        for (u32 f = 0; f < frames; ++f) {
            w += step;
            if (outCh > 2) {
                const f32 rl = rvL_[f] * w, rr = rvR_[f] * w;
                for (u32 c = 0; c < outCh; ++c) out[static_cast<usize>(f) * outCh + c] += um.l[c] * rl + um.r[c] * rr;
            } else if (outCh == 1) {
                out[f] += 0.5f * (rvL_[f] + rvR_[f]) * w;
            } else {
                out[f * 2 + 0] += rvL_[f] * w;
                out[f * 2 + 1] += rvR_[f] * w;
            }
        }
        rvPrevWet_ = wet;
        rvDirty_ = true;
    } else {
        if (rvDirty_) { reverb_.reset(); rvDirty_ = false; }
        rvPrevWet_ = 0.0f;
    }

    mixZoneReturns(out, frames, outCh, um);

    if (graphLocked) {
        graph_.process(frames);
        const f32* m = graph_.busBuffer(0);
        const f32 mg = master_.load(std::memory_order_relaxed);
        if (outCh == 1) for (u32 f = 0; f < frames; ++f) out[f] += 0.5f * (m[2 * f] + m[2 * f + 1]) * mg;
        else if (outCh > 2) {
            for (u32 f = 0; f < frames; ++f)
                for (u32 c = 0; c < outCh; ++c) out[static_cast<usize>(f) * outCh + c] += (um.l[c] * m[2 * f] + um.r[c] * m[2 * f + 1]) * mg;
        }
        else for (u32 f = 0; f < frames * 2; ++f) out[f] += m[f] * mg;
        graph_.end();
    }
}

} // namespace aver::audio
