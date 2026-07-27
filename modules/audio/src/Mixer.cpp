#include "aver/audio/Mixer.hpp"
#include "aver/core/Log.hpp"

#include <cmath>

namespace aver::audio {
namespace {

// How fast a voice's gain may move, as a fraction of a block. Gains are ramped rather than snapped
// because a volume change applied instantly is a step discontinuity, and a step discontinuity is a
// click -- the same reason a stop fades instead of cutting.
constexpr f32 kMinGain = 1.0e-4f;   // below this a stopping voice is finished

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

f32 dot3(const f32 a[3], f32 x, f32 y, f32 z) { return a[0] * x + a[1] * y + a[2] * z; }

} // namespace

f32 attenuationAt(const Attenuation& a, f32 distanceCm) {
    const f32 inner = a.innerRadius > 0.0f ? a.innerRadius : 1.0f;
    const f32 outer = a.outerRadius > inner ? a.outerRadius : inner + 1.0f;
    if (distanceCm <= inner) return 1.0f;
    if (distanceCm >= outer) return 0.0f;

    // Inverse distance, then remapped so the curve reaches EXACTLY zero at the outer radius instead
    // of merely approaching it. Without the remap a sound is still audible at its own cutoff and
    // pops the moment it is culled -- a bug that only ever appears while walking away from something.
    const f32 raw   = inner / distanceCm;      // 1 at the inner radius
    const f32 floor_ = inner / outer;          // what raw would be at the outer radius
    return (raw - floor_) / (1.0f - floor_);
}

void panGains(f32 pan, f32& outL, f32& outR) {
    // -1..+1 mapped onto the first quadrant, so l^2 + r^2 == 1 for every pan. A linear law instead
    // (l = 1-t, r = t) dips 3 dB in the middle: audible on anything that moves across the field, and
    // perfectly reasonable-looking as arithmetic.
    const f32 t = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265358979f;
    outL = std::cos(t);
    outR = std::sin(t);
}

Mixer::~Mixer() { shutdown(); }

bool Mixer::init(u32 sampleRate, u32 channels, u32 maxVoices, u32 maxSounds) {
    if (ready_) shutdown();
    if (sampleRate == 0) { AVER_ERROR("[Audio] init with a zero sample rate"); return false; }
    if (channels != 1 && channels != 2) {
        // Refused rather than downmixed. A mixer that quietly folded 5.1 into stereo would be doing
        // something nobody asked for, at a quality nobody chose.
        AVER_ERROR("[Audio] init with {} channels; only mono and stereo are supported", channels);
        return false;
    }
    if (maxVoices == 0 || maxSounds == 0) { AVER_ERROR("[Audio] init with an empty pool"); return false; }

    sampleRate_ = sampleRate;
    channels_   = channels;

    // Constructed at size and never resized. std::atomic is neither copyable nor movable, so the
    // vector could not grow even if something wanted it to -- which is exactly the guarantee the
    // audio thread needs, since it holds bare pointers into both of these across a mix.
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

void Mixer::shutdown() {
    // The CALLER is responsible for having stopped the device first. Nothing here can make the audio
    // thread stand down, and a mixer torn down under a live callback is a use-after-free however
    // carefully this function is written -- so it does not pretend otherwise.
    ready_ = false;
    voices_.clear();
    sounds_.clear();
    sampleRate_ = channels_ = 0;
}

// ---------------------------------------------------------------- sounds

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

void Mixer::removeSound(SoundHandle h) {
    if (!ready_ || h == 0 || h > sounds_.size()) return;
    sounds_[h - 1].retired.store(true, std::memory_order_release);
    collect();   // free it now when nothing is playing it, which is the common case
}

void Mixer::collect() {
    if (!ready_) return;
    for (SoundSlot& s : sounds_) {
        if (!s.data) continue;
        if (!s.retired.load(std::memory_order_acquire)) continue;
        if (s.refs.load(std::memory_order_acquire) != 0) continue;   // a voice is still reading it
        s.data.reset();
        s.retired.store(false, std::memory_order_release);
    }
}

u32 Mixer::soundCount() const {
    u32 n = 0;
    for (const SoundSlot& s : sounds_) if (s.data) ++n;
    return n;
}

// ---------------------------------------------------------------- voices

Mixer::Voice* Mixer::resolve(VoiceHandle v) {
    if (!ready_ || v == 0) return nullptr;
    const u32 i = indexOf(v);
    if (i >= voices_.size()) return nullptr;
    Voice& vo = voices_[i];
    // The generation is what makes a stale handle read as dead rather than as whoever got the slot
    // next. At 64 voices a slot comes round again in seconds.
    if (vo.generation.load(std::memory_order_acquire) != generationOf(v)) return nullptr;
    if (vo.state.load(std::memory_order_acquire) == static_cast<u32>(State::Free)) return nullptr;
    return &vo;
}

const Mixer::Voice* Mixer::resolve(VoiceHandle v) const {
    return const_cast<Mixer*>(this)->resolve(v);
}

u32 Mixer::stealSlot() {
    // Quietest first, oldest as the tie-break. Stealing the oldest alone cuts the ambient bed that
    // has been running since the level loaded; stealing the quietest alone can cut the same distant
    // voice over and over. Together they take the one least likely to be noticed.
    u32 best = 0xFFFFFFFFu;
    f32 bestGain = 0.0f;
    u64 bestOrder = 0;
    for (u32 i = 0; i < voices_.size(); ++i) {
        Voice& vo = voices_[i];
        if (vo.state.load(std::memory_order_acquire) != static_cast<u32>(State::Active)) continue;
        f32 l = 0.0f, r = 0.0f;
        voiceGains(vo, l, r);
        const f32 g = l > r ? l : r;
        if (best == 0xFFFFFFFFu || g < bestGain || (g == bestGain && vo.startOrder < bestOrder)) {
            best = i; bestGain = g; bestOrder = vo.startOrder;
        }
    }
    return best;
}

VoiceHandle Mixer::play(const PlayDesc& desc) {
    if (!ready_) return 0;
    if (desc.sound == 0 || desc.sound > sounds_.size()) return 0;
    SoundSlot& slot = sounds_[desc.sound - 1];
    if (!slot.data) return 0;
    // A retired sound may not start a NEW voice, or collect() would never see the reference count
    // reach zero and the memory would never come back.
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
        // Pool full: take one. The steal moves Active -> Pending, and the audio thread renders
        // nothing for a Pending slot -- so the worst case is one block of silence from that voice,
        // never a torn read of parameters being rewritten underneath it.
        const u32 victim = stealSlot();
        if (victim == 0xFFFFFFFFu) return 0;
        u32 expected = static_cast<u32>(State::Active);
        if (!voices_[victim].state.compare_exchange_strong(
                expected, static_cast<u32>(State::Pending),
                std::memory_order_acq_rel, std::memory_order_relaxed))
            return 0;   // it ended on its own between the choice and the claim; the next play gets it
        // The stolen voice was holding a reference to its sound; release it here, on the game
        // thread, because the audio thread never got to finish it.
        if (voices_[victim].soundHandle && voices_[victim].soundHandle <= sounds_.size())
            sounds_[voices_[victim].soundHandle - 1].refs.fetch_sub(1, std::memory_order_acq_rel);
        // AND BUMP THE GENERATION, exactly as ending normally does. Without this the stolen voice's
        // handle still matches the slot, so whoever started it goes on believing it is playing and
        // silently drives the voice that REPLACED it -- the precise failure the generation exists to
        // prevent, reintroduced on the one path that does not go through the audio thread.
        voices_[victim].generation.fetch_add(1, std::memory_order_acq_rel);
        index = victim;
        stolen_.fetch_add(1, std::memory_order_relaxed);
    }

    Voice& vo = voices_[index];
    // Plain writes: the slot is Pending, and the audio thread never reads a Pending slot's
    // parameters. The release store at the bottom is what publishes all of this.
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

void Mixer::stop(VoiceHandle v) {
    // Flagged, not freed. The audio thread ramps the gain down over one block and releases the slot
    // itself -- cutting a waveform mid-cycle is a click, and freeing a slot the audio thread is
    // reading is worse than a click.
    if (Voice* vo = resolve(v)) vo->stopping.store(true, std::memory_order_release);
}

void Mixer::stopAll() {
    if (!ready_) return;
    for (Voice& vo : voices_)
        if (vo.state.load(std::memory_order_acquire) == static_cast<u32>(State::Active))
            vo.stopping.store(true, std::memory_order_release);
}

bool Mixer::playing(VoiceHandle v) const {
    const Voice* vo = resolve(v);
    return vo && vo->state.load(std::memory_order_acquire) == static_cast<u32>(State::Active);
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

u32 Mixer::activeVoices() const {
    u32 n = 0;
    for (const Voice& vo : voices_)
        if (vo.state.load(std::memory_order_acquire) == static_cast<u32>(State::Active)) ++n;
    return n;
}

// ---------------------------------------------------------------- the audio thread

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

    // Panned by where the source sits along the listener's RIGHT axis. At the listener's exact
    // position there is no direction to speak of, so it plays centred rather than snapping to a side
    // as the sign of a denominator flips.
    f32 pan = 0.0f;
    if (dist > 1.0e-3f) {
        const f32 rx = lisRight_[0].load(std::memory_order_relaxed);
        const f32 ry = lisRight_[1].load(std::memory_order_relaxed);
        const f32 rz = lisRight_[2].load(std::memory_order_relaxed);
        const f32 right[3] = {rx, ry, rz};
        pan = clampf(dot3(right, dx, dy, dz) / dist, -1.0f, 1.0f);
    }
    f32 pl = 0.0f, pr = 0.0f;
    panGains(pan, pl, pr);
    // Normalised so a centred positional voice is as loud as a non-positional one: constant-power
    // pan puts 0.707 in each ear at centre, and without this every 3D sound would sit 3 dB below
    // every 2D one for no reason a mixer could explain.
    constexpr f32 kCentre = 1.41421356f;   // 1 / cos(45 degrees)
    outL = g * pl * kCentre;
    outR = g * pr * kCentre;
}

bool Mixer::renderVoice(Voice& vo, f32* out, u32 frames) {
    const SoundData* snd = vo.sound;
    if (!snd || frames == 0) return false;

    const u32 srcFrames = snd->frames();
    if (srcFrames == 0) return false;
    const u32 srcCh = snd->channels;

    // One read per output frame at this rate. Resampling and pitch are the same operation: both
    // change how fast the cursor walks the source.
    const f64 step = (static_cast<f64>(snd->sampleRate) / static_cast<f64>(sampleRate_))
                   * static_cast<f64>(vo.pitch.load(std::memory_order_relaxed));

    const bool stopping = vo.stopping.load(std::memory_order_acquire);
    f32 targetL = 0.0f, targetR = 0.0f;
    if (!stopping) voiceGains(vo, targetL, targetR);

    // The first block snaps rather than ramping, or every sound would fade in from silence over its
    // first buffer -- audible on a short percussive one, which is most of them.
    if (!vo.primed) { vo.gainL = targetL; vo.gainR = targetR; vo.primed = true; }

    const f32 dL = (targetL - vo.gainL) / static_cast<f32>(frames);
    const f32 dR = (targetR - vo.gainR) / static_cast<f32>(frames);

    const u32 loopEnd   = (snd->loopEnd > 0 && snd->loopEnd <= srcFrames) ? snd->loopEnd : srcFrames;
    const u32 loopBegin = snd->loopBegin < loopEnd ? snd->loopBegin : 0;

    bool ended = false;
    for (u32 f = 0; f < frames; ++f) {
        if (vo.cursor >= static_cast<f64>(loopEnd)) {
            if (!vo.looping) { ended = true; break; }
            // Wrapped by SUBTRACTING the loop length rather than assigning loopBegin, so a loop
            // point that is not a whole number of output frames stays sample-exact instead of
            // drifting a fraction of a frame every pass.
            const f64 len = static_cast<f64>(loopEnd - loopBegin);
            if (len <= 0.0) { ended = true; break; }
            while (vo.cursor >= static_cast<f64>(loopEnd)) vo.cursor -= len;
        }

        const u32 i0 = static_cast<u32>(vo.cursor);
        const f32 frac = static_cast<f32>(vo.cursor - static_cast<f64>(i0));
        u32 i1 = i0 + 1;
        if (i1 >= loopEnd) i1 = vo.looping ? loopBegin : (loopEnd > 0 ? loopEnd - 1 : 0);

        // Linear interpolation. Not the best resampler there is; it is the one whose cost is two
        // reads and a multiply-add, which is what a per-sample inner loop can afford.
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
    // A stopping voice is finished once its ramp has run out. One block, not an arbitrary timer.
    if (stopping && std::fabs(vo.gainL) < kMinGain && std::fabs(vo.gainR) < kMinGain) return false;
    return true;
}

void Mixer::mix(f32* out, u32 frames) {
    if (!out || frames == 0) return;
    const u32 samples = frames * (channels_ ? channels_ : 1);
    if (!ready_) {
        // Silence, not stale memory. An uninitialised mixer handing back whatever was in the buffer
        // is full-scale noise at whatever volume the user had set.
        for (u32 i = 0; i < samples; ++i) out[i] = 0.0f;
        starved_.fetch_add(frames, std::memory_order_relaxed);
        return;
    }

    for (u32 i = 0; i < samples; ++i) out[i] = 0.0f;

    for (Voice& vo : voices_) {
        // Free and Pending are both skipped: Pending means the game thread is still writing this
        // slot's parameters, and reading them now is the one race this design exists to prevent.
        if (vo.state.load(std::memory_order_acquire) != static_cast<u32>(State::Active)) continue;

        if (!renderVoice(vo, out, frames)) {
            // Ended. Release the sound reference, bump the generation so the handle that started it
            // reads dead, and only then publish the slot as free.
            if (vo.soundHandle && vo.soundHandle <= sounds_.size())
                sounds_[vo.soundHandle - 1].refs.fetch_sub(1, std::memory_order_acq_rel);
            vo.sound = nullptr;
            vo.soundHandle = 0;
            vo.generation.fetch_add(1, std::memory_order_acq_rel);
            vo.state.store(static_cast<u32>(State::Free), std::memory_order_release);
        }
    }

    // Hard clip. Deliberately not a soft knee: a limiter changes the level of everything below the
    // threshold too, so a mix that clips would sound quieter rather than distorted and the problem
    // would go unnoticed. Clipping is meant to be unpleasant.
    for (u32 i = 0; i < samples; ++i) out[i] = clampf(out[i], -1.0f, 1.0f);
}

} // namespace aver::audio
