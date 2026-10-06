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
constexpr u32 kUnderrunTail = 64;      // frames faded out when a stream runs dry mid-block
constexpr u32 kNoSlot = 0xFFFFFFFFu;

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
    if (channels != 1 && channels != 2) {
        AVER_ERROR("[Audio] init with {} channels; only mono and stereo are supported", channels);
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

    reverb_.init(sampleRate);
    sendBuf_.assign(kMaxSendFrames, 0.0f);
    rvL_.assign(kMaxSendFrames, 0.0f);
    rvR_.assign(kMaxSendFrames, 0.0f);
    sendPtr_ = nullptr;
    rvPrevWet_ = 0.0f;
    rvDirty_ = false;
    rvWet_.store(0.0f, std::memory_order_relaxed);
    rvDecay_.store(1.5f, std::memory_order_relaxed);
    rvDamp_.store(0.4f, std::memory_order_relaxed);

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
    voices_.clear();
    streams_.clear();   // joins every decoder thread
    sounds_.clear();
    sampleRate_ = channels_ = 0;
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
}

// Number of loaded sounds.
u32 Mixer::soundCount() const {
    u32 n = 0;
    for (const SoundSlot& s : sounds_) if (s.data) ++n;
    return n;
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
            if (v.soundHandle && v.soundHandle <= sounds_.size())
                sounds_[v.soundHandle - 1].refs.fetch_sub(1, std::memory_order_acq_rel);
            if (v.streamSlot != kNoSlot && v.streamSlot < streams_.size())
                streams_[v.streamSlot].attached.store(false, std::memory_order_release);
            v.stream = nullptr;
            v.streamSlot = kNoSlot;
            v.generation.fetch_add(1, std::memory_order_acq_rel);
            index = victim;
            stolen_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return index;
}

// Fills a claimed voice from the description and publishes it Active.
VoiceHandle Mixer::activate(u32 index, const PlayDesc& desc) {
    Voice& vo = voices_[index];
    vo.looping     = desc.looping;
    vo.bus         = desc.bus < Bus::Count ? desc.bus : Bus::Sfx;
    vo.positional  = desc.positional;
    vo.attenuation = desc.attenuation;
    vo.startOrder  = playCounter_.fetch_add(1, std::memory_order_relaxed);
    vo.cursor      = 0.0;
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

    if (desc.fadeInSeconds > 0.0f) {
        vo.fadeCur = 0.0f;
        vo.fadePub.store(0.0f, std::memory_order_relaxed);
        vo.primed = true;     // gains start at zero and ramp, rather than snapping to the first block's value
        requestFade(vo, 1.0f, static_cast<u32>(desc.fadeInSeconds * static_cast<f32>(sampleRate_)),
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
    return activate(index, desc);
}

// Starts a streamed voice. The decoder is owned by a stream slot until collect() frees it.
VoiceHandle Mixer::playStream(std::unique_ptr<StreamSource> source, const PlayDesc& desc, StreamParams params) {
    if (!ready_ || !source) return 0;
    collect();

    u32 slot = kNoSlot;
    for (u32 i = 0; i < streams_.size(); ++i) if (!streams_[i].dec) { slot = i; break; }
    if (slot == kNoSlot) { AVER_WARN("[Audio] all {} stream slots are in use", streams_.size()); return 0; }

    params.looping = desc.looping;
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
    return activate(index, desc);
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
    requestFade(*vo, targetGain, static_cast<u32>(s * static_cast<f32>(sampleRate_)), curve, stopAtEnd);
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
void Mixer::voiceGains(const Voice& vo, f32& outL, f32& outR) const {
    f32 g = vo.volume.load(std::memory_order_relaxed)
          * buses_[static_cast<usize>(vo.bus)].load(std::memory_order_relaxed)
          * master_.load(std::memory_order_relaxed)
          * vo.fadePub.load(std::memory_order_relaxed);

    if (!vo.positional) { outL = outR = g; return; }

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
bool Mixer::renderVoice(Voice& vo, f32* out, u32 frames) {
    StreamDecoder* st = vo.stream;
    const SoundData* snd = vo.sound;
    if (frames == 0) return false;

    u32 srcFrames = 0, srcCh = 0, srcRate = 0;
    if (st) {
        srcCh = st->channels();
        srcRate = st->sampleRate();
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

    const f64 step = (static_cast<f64>(srcRate) / static_cast<f64>(sampleRate_))
                   * static_cast<f64>(vo.pitch.load(std::memory_order_relaxed));

    bool fadeStop = false;
    advanceFade(vo, frames, fadeStop);

    f32 targetL = 0.0f, targetR = 0.0f;
    if (!stopping) voiceGains(vo, targetL, targetR);

    // Occlusion: follow the published value, then low-pass and duck.
    const f32 occTarget = clampf(vo.occlusion.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (!vo.occPrimed) { vo.occSmooth = occTarget; vo.occPrimed = true; }
    else vo.occSmooth += (occTarget - vo.occSmooth)
                       * smoothingCoeff(static_cast<f32>(frames) / static_cast<f32>(sampleRate_), kOcclusionTau);
    const bool filtering = vo.occSmooth > kOcclusionBypass;
    BiquadCoeffs lp;
    if (filtering) {
        OcclusionCurve curve;
        curve.minCutoffHz = occMinCutoff_.load(std::memory_order_relaxed);
        curve.minVolume   = occMinVolume_.load(std::memory_order_relaxed);
        lp = lowpassCoeffs(occlusionCutoffHz(vo.occSmooth, curve), static_cast<f32>(sampleRate_));
        const f32 og = occlusionGain(vo.occSmooth, curve);
        targetL *= og;
        targetR *= og;
    } else if (vo.filterActive) {
        vo.filtL.reset();
        vo.filtR.reset();
    }
    vo.filterActive = filtering;

    const f32 sendTarget = sendPtr_ ? 0.5f * (targetL + targetR) * vo.reverbSend.load(std::memory_order_relaxed) : 0.0f;

    if (!vo.primed) { vo.gainL = targetL; vo.gainR = targetR; vo.sendGain = sendTarget; vo.primed = true; }
    if (vo.resumeRamp) { vo.gainL = vo.gainR = 0.0f; vo.sendGain = 0.0f; vo.resumeRamp = false; }

    const f32 dL = (targetL - vo.gainL) / static_cast<f32>(frames);
    const f32 dR = (targetR - vo.gainR) / static_cast<f32>(frames);
    const f32 dS = (sendTarget - vo.sendGain) / static_cast<f32>(frames);

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
        if (f >= tailStart && tailLen > 0) {
            const f32 tg = static_cast<f32>(nRender - f) / static_cast<f32>(tailLen + 1);
            sl *= tg;
            sr *= tg;
        }

        vo.gainL += dL;
        vo.gainR += dR;

        if (channels_ == 1) {
            out[f] += (sl * vo.gainL + sr * vo.gainR) * 0.5f;
        } else {
            out[f * 2 + 0] += sl * vo.gainL;
            out[f * 2 + 1] += sr * vo.gainR;
        }
        if (sendPtr_) {
            vo.sendGain += dS;
            sendPtr_[f] += 0.5f * (sl + sr) * vo.sendGain;
        }
        vo.cursor += step;
    }

    if (st) {
        u32 consumed = static_cast<u32>(vo.cursor);
        if (consumed > avail) consumed = avail;
        st->consume(consumed);
        vo.cursor -= static_cast<f64>(consumed);
        if (vo.cursor < 0.0) vo.cursor = 0.0;
        if (streamEnd) return false;
        if (st->endReached() && st->available() == 0) return false;
    }

    if (ended) return false;
    if (fadeStop) return false;
    if (stopping && std::fabs(vo.gainL) < kMinGain && std::fabs(vo.gainR) < kMinGain) return false;
    return true;
}

// Renders frames of interleaved output, overwriting the buffer. Audio thread only.
void Mixer::mix(f32* out, u32 frames) {
    if (!out || frames == 0) return;
    const u32 samples = frames * (channels_ ? channels_ : 1);
    if (!ready_) {
        for (u32 i = 0; i < samples; ++i) out[i] = 0.0f;
        starved_.fetch_add(frames, std::memory_order_relaxed);
        return;
    }

    for (u32 i = 0; i < samples; ++i) out[i] = 0.0f;

    // Reverb runs while it is wanted, and for one more block to ramp its return out.
    const f32 wet = rvWet_.load(std::memory_order_relaxed);
    const bool wantSend = wet > 1.0e-4f;
    const bool rvOn = reverb_.ready() && frames <= sendBuf_.size() && (wantSend || rvPrevWet_ > 1.0e-4f);
    sendPtr_ = nullptr;
    if (rvOn) {
        std::fill(sendBuf_.begin(), sendBuf_.begin() + frames, 0.0f);
        if (wantSend) sendPtr_ = sendBuf_.data();
    }

    for (Voice& vo : voices_) {
        // CLAIMED FOR THE DURATION OF THE BLOCK. Reading the state and then rendering left the
        // voice Active throughout, which is the window play()'s stealing CAS used to walk into --
        // dropping the sound's refcount and rewriting vo.sound while this thread was reading them.
        // Taking it out of Active makes that CAS fail instead.
        u32 expected = static_cast<u32>(State::Active);
        if (!vo.state.compare_exchange_strong(expected, static_cast<u32>(State::Rendering),
                                              std::memory_order_acq_rel, std::memory_order_acquire))
            continue;

        if (renderVoice(vo, out, frames)) {
            // Still sounding: hand it back. A plain store is right -- this thread owns the voice
            // while it is Rendering, so nothing else can have changed the state under it.
            vo.state.store(static_cast<u32>(State::Active), std::memory_order_release);
        } else {
            if (vo.soundHandle && vo.soundHandle <= sounds_.size())
                sounds_[vo.soundHandle - 1].refs.fetch_sub(1, std::memory_order_acq_rel);
            if (vo.streamSlot != kNoSlot && vo.streamSlot < streams_.size())
                streams_[vo.streamSlot].attached.store(false, std::memory_order_release);
            vo.sound = nullptr;
            vo.soundHandle = 0;
            vo.stream = nullptr;
            vo.streamSlot = kNoSlot;
            vo.generation.fetch_add(1, std::memory_order_acq_rel);
            vo.state.store(static_cast<u32>(State::Free), std::memory_order_release);
        }
    }
    sendPtr_ = nullptr;

    if (rvOn) {
        reverb_.setParams(rvDecay_.load(std::memory_order_relaxed), rvDamp_.load(std::memory_order_relaxed));
        reverb_.process(sendBuf_.data(), rvL_.data(), rvR_.data(), frames);
        const f32 step = (wet - rvPrevWet_) / static_cast<f32>(frames);
        f32 w = rvPrevWet_;
        for (u32 f = 0; f < frames; ++f) {
            w += step;
            if (channels_ == 1) {
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

    for (u32 i = 0; i < samples; ++i) out[i] = clampf(out[i], -1.0f, 1.0f);
}

} // namespace aver::audio
