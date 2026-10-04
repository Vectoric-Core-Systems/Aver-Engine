// Mixer implementation: sound slots, the voice pool, and the block render.
#include "aver/audio/Mixer.hpp"
#include "aver/core/Log.hpp"

#include <cmath>

namespace aver::audio {
namespace {

constexpr f32 kMinGain = 1.0e-4f;   // below this a stopping voice is finished

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

// Sizes the voice and sound pools and makes the mixer ready. False on bad arguments.
bool Mixer::init(u32 sampleRate, u32 channels, u32 maxVoices, u32 maxSounds) {
    if (ready_) shutdown();
    if (sampleRate == 0) { AVER_ERROR("[Audio] init with a zero sample rate"); return false; }
    if (channels != 1 && channels != 2) {
        AVER_ERROR("[Audio] init with {} channels; only mono and stereo are supported", channels);
        return false;
    }
    if (maxVoices == 0 || maxSounds == 0) { AVER_ERROR("[Audio] init with an empty pool"); return false; }

    sampleRate_ = sampleRate;
    channels_   = channels;

    { std::vector<Voice>     v(maxVoices); voices_.swap(v); }
    { std::vector<SoundSlot> s(maxSounds); sounds_.swap(s); }

    master_.store(1.0f, std::memory_order_relaxed);
    for (usize i = 0; i < static_cast<usize>(Bus::Count); ++i)
        buses_[i].store(1.0f, std::memory_order_relaxed);

    Listener l;
    setListener(l);

    playCounter_.store(1, std::memory_order_relaxed);
    stolen_.store(0, std::memory_order_relaxed);
    starved_.store(0, std::memory_order_relaxed);

    ready_ = true;
    AVER_INFO("[Audio] mixer ready: {} Hz, {} channel(s), {} voices, {} sound slots",
              sampleRate, channels, maxVoices, maxSounds);
    return true;
}

// Releases the pools. The caller must have stopped the device first.
void Mixer::shutdown() {
    ready_ = false;
    voices_.clear();
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

// Frees retired sounds whose last voice has ended. Game thread only.
void Mixer::collect() {
    if (!ready_) return;
    for (SoundSlot& s : sounds_) {
        if (!s.data) continue;
        if (!s.retired.load(std::memory_order_acquire)) continue;
        if (s.refs.load(std::memory_order_acquire) != 0) continue;
        s.data.reset();
        s.retired.store(false, std::memory_order_release);
    }
}

// Number of loaded sounds.
u32 Mixer::soundCount() const {
    u32 n = 0;
    for (const SoundSlot& s : sounds_) if (s.data) ++n;
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

// Starts a voice, stealing one if the pool is full. Returns its handle, or 0.
VoiceHandle Mixer::play(const PlayDesc& desc) {
    if (!ready_) return 0;
    if (desc.sound == 0 || desc.sound > sounds_.size()) return 0;
    SoundSlot& slot = sounds_[desc.sound - 1];
    if (!slot.data) return 0;
    if (slot.retired.load(std::memory_order_acquire)) return 0;

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
            if (victim == 0xFFFFFFFFu) return 0;
            u32 expected = static_cast<u32>(State::Active);
            if (!voices_[victim].state.compare_exchange_strong(
                    expected, static_cast<u32>(State::Pending),
                    std::memory_order_acq_rel, std::memory_order_relaxed))
                continue;
            // Won it. From here the voice is Pending, so the audio thread will not enter it.
            if (voices_[victim].soundHandle && voices_[victim].soundHandle <= sounds_.size())
                sounds_[voices_[victim].soundHandle - 1].refs.fetch_sub(1, std::memory_order_acq_rel);
            voices_[victim].generation.fetch_add(1, std::memory_order_acq_rel);
            index = victim;
            stolen_.fetch_add(1, std::memory_order_relaxed);
        }
        if (index == 0xFFFFFFFFu) return 0;
    }

    Voice& vo = voices_[index];
    vo.sound       = slot.data.get();
    vo.soundHandle = desc.sound;
    vo.looping     = desc.looping;
    vo.bus         = desc.bus < Bus::Count ? desc.bus : Bus::Sfx;
    vo.positional  = desc.positional;
    vo.attenuation = desc.attenuation;
    vo.startOrder  = playCounter_.fetch_add(1, std::memory_order_relaxed);
    vo.cursor      = 0.0;
    vo.gainL = vo.gainR = 0.0f;
    vo.primed = false;
    vo.stopping.store(false, std::memory_order_relaxed);
    vo.volume.store(desc.volume < 0.0f ? 0.0f : desc.volume, std::memory_order_relaxed);
    vo.pitch.store(desc.pitch > 0.0f ? desc.pitch : 1.0f, std::memory_order_relaxed);
    vo.posX.store(desc.position[0], std::memory_order_relaxed);
    vo.posY.store(desc.position[1], std::memory_order_relaxed);
    vo.posZ.store(desc.position[2], std::memory_order_relaxed);

    slot.refs.fetch_add(1, std::memory_order_acq_rel);

    const u32 gen = vo.generation.load(std::memory_order_relaxed);
    vo.state.store(static_cast<u32>(State::Active), std::memory_order_release);
    return encode(index, gen);
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

// Moves the listener. Positions and axes in engine space.
void Mixer::setListener(const Listener& l) {
    for (int i = 0; i < 3; ++i) {
        lisPos_[i].store(l.position[i], std::memory_order_relaxed);
        lisFwd_[i].store(l.forward[i], std::memory_order_relaxed);
        lisRight_[i].store(l.right[i], std::memory_order_relaxed);
    }
}

void Mixer::setBusVolume(Bus b, f32 volume) {
    if (b < Bus::Count) buses_[static_cast<usize>(b)].store(volume < 0.0f ? 0.0f : volume, std::memory_order_relaxed);
}

f32 Mixer::busVolume(Bus b) const {
    return b < Bus::Count ? buses_[static_cast<usize>(b)].load(std::memory_order_relaxed) : 0.0f;
}

void Mixer::setMasterVolume(f32 volume) { master_.store(volume < 0.0f ? 0.0f : volume, std::memory_order_relaxed); }
f32  Mixer::masterVolume() const { return master_.load(std::memory_order_relaxed); }

// Number of voices currently playing.
u32 Mixer::activeVoices() const {
    u32 n = 0;
    for (const Voice& vo : voices_)
        if (isLiveState(vo.state.load(std::memory_order_acquire))) ++n;
    return n;
}

// ---------------------------------------------------------------- the audio thread

// The left/right gains this voice contributes, after distance, pan, bus and master.
void Mixer::voiceGains(const Voice& vo, f32& outL, f32& outR) const {
    f32 g = vo.volume.load(std::memory_order_relaxed)
          * buses_[static_cast<usize>(vo.bus)].load(std::memory_order_relaxed)
          * master_.load(std::memory_order_relaxed);

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

// Adds one voice into the output block. Returns false when the voice has ended.
bool Mixer::renderVoice(Voice& vo, f32* out, u32 frames) {
    const SoundData* snd = vo.sound;
    if (!snd || frames == 0) return false;

    const u32 srcFrames = snd->frames();
    if (srcFrames == 0) return false;
    const u32 srcCh = snd->channels;

    const f64 step = (static_cast<f64>(snd->sampleRate) / static_cast<f64>(sampleRate_))
                   * static_cast<f64>(vo.pitch.load(std::memory_order_relaxed));

    const bool stopping = vo.stopping.load(std::memory_order_acquire);
    f32 targetL = 0.0f, targetR = 0.0f;
    if (!stopping) voiceGains(vo, targetL, targetR);

    if (!vo.primed) { vo.gainL = targetL; vo.gainR = targetR; vo.primed = true; }

    const f32 dL = (targetL - vo.gainL) / static_cast<f32>(frames);
    const f32 dR = (targetR - vo.gainR) / static_cast<f32>(frames);

    const u32 loopEnd   = (snd->loopEnd > 0 && snd->loopEnd <= srcFrames) ? snd->loopEnd : srcFrames;
    const u32 loopBegin = snd->loopBegin < loopEnd ? snd->loopBegin : 0;

    bool ended = false;
    for (u32 f = 0; f < frames; ++f) {
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

        f32 sl, sr;
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

        vo.gainL += dL;
        vo.gainR += dR;

        if (channels_ == 1) {
            out[f] += (sl * vo.gainL + sr * vo.gainR) * 0.5f;
        } else {
            out[f * 2 + 0] += sl * vo.gainL;
            out[f * 2 + 1] += sr * vo.gainR;
        }
        vo.cursor += step;
    }

    if (ended) return false;
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
            vo.sound = nullptr;
            vo.soundHandle = 0;
            vo.generation.fetch_add(1, std::memory_order_acq_rel);
            vo.state.store(static_cast<u32>(State::Free), std::memory_order_release);
        }
    }

    for (u32 i = 0; i < samples; ++i) out[i] = clampf(out[i], -1.0f, 1.0f);
}

} // namespace aver::audio
