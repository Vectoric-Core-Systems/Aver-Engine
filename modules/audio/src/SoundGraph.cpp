// The sound-graph evaluator: compile a graph once, run it per sample with all state pre-allocated.
// See SoundGraph.hpp for the contract (one evaluator for the offline render and the audio thread).
#include "aver/audio/SoundGraph.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aver::audio {
namespace {

constexpr f32 kPi2 = 6.28318530717958647692f;
constexpr f32 kPi  = kPi2 * 0.5f;
constexpr f32 kDefaultSmoothSec = 0.02f;   // a Parameter whose smoothing time is 0 reads as this, so a jump never clicks
constexpr u32 kNone = 0xFFFFFFFFu;

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// xorshift32, the generator Noise has always used: a render with the same seed is the same audio.
f32 nextNoise(u32& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (static_cast<f32>(s & 0xFFFFFFu) / 8388608.0f) - 1.0f;
}
// [0, 1)
f32 nextUnit(u32& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<f32>(s & 0xFFFFFFu) / 16777216.0f;
}

// One-pole low pass. `cutoff` in Hz; a cutoff at or above Nyquist passes the signal through.
f32 onePole(f32 in, f32& last, f32 cutoffHz, f32 sampleRate) {
    if (cutoffHz <= 0.0f) return 0.0f;
    const f32 nyquist = sampleRate * 0.5f;
    if (cutoffHz >= nyquist) { last = in; return in; }
    const f32 rc = 1.0f / (kPi2 * cutoffHz);
    const f32 dt = 1.0f / sampleRate;
    const f32 a  = dt / (rc + dt);
    last += a * (in - last);
    return last;
}

// Attack/decay/sustain/release at time `t`. The release starts at duration - r: for the offline render that is r seconds before the end;
// the runner passes a shorter `duration` (note-off time + r) once a note-off has arrived, so the SAME curve releases from wherever the
// envelope was, and a graph that never gets a note-off is the closed form it always was.
f32 adsrAt(f32 t, f32 duration, f32 a, f32 d, f32 s, f32 r) {
    if (t < 0.0f || t > duration) return 0.0f;
    a = a < 0.0f ? 0.0f : a;
    d = d < 0.0f ? 0.0f : d;
    r = r < 0.0f ? 0.0f : r;
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);

    const f32 releaseStart = duration - r;
    if (r > 0.0f && t >= releaseStart) {
        // From the level the envelope had AT releaseStart, not from sustain: a release during the attack must not jump up first.
        const f32 atRelease = releaseStart <= a && a > 0.0f ? (releaseStart / a)
                            : (releaseStart <= a + d && d > 0.0f ? 1.0f - (1.0f - s) * ((releaseStart - a) / d)
                                                                  : s);
        const f32 k = (t - releaseStart) / r;
        return atRelease * (1.0f - (k > 1.0f ? 1.0f : k));
    }
    if (a > 0.0f && t < a) return t / a;
    if (d > 0.0f && t < a + d) return 1.0f - (1.0f - s) * ((t - a) / d);
    return s;
}

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// One-pole smoothing coefficient for a time constant in seconds.
f32 smoothCoef(f32 tauSec, f32 sr) {
    return tauSec > 0.0f ? 1.0f - std::exp(-1.0f / (tauSec * sr)) : 1.0f;
}
// Exponential-follower coefficient (0 = instant).
f32 followCoef(f32 sec, f32 sr) {
    return sec > 0.0f ? std::exp(-1.0f / (sec * sr)) : 0.0f;
}

f32 delayMaxSec(f32 p3) {
    return p3 > 0.0f ? (p3 > kSoundMaxDelaySec ? kSoundMaxDelaySec : p3) : 1.0f;   // 0 reads as one second
}

f32 flush(f32 v) { return std::fabs(v) < 1.0e-20f ? 0.0f : v; }

} // namespace

u32 soundNodeInputCount(SoundNodeKind k) {
    switch (k) {
        case SoundNodeKind::Noise:
        case SoundNodeKind::Const:
        case SoundNodeKind::Parameter:
        case SoundNodeKind::Random:     return 0;
        case SoundNodeKind::Sine:       // one input: a frequency offset in Hz, added to the node's own
        case SoundNodeKind::Saw:
        case SoundNodeKind::Square:
        case SoundNodeKind::Gain:
        case SoundNodeKind::Adsr:
        case SoundNodeKind::Lfo:        // rate offset in Hz
        case SoundNodeKind::WavePlayer: // pitch offset in semitones
        case SoundNodeKind::Clamp:
        case SoundNodeKind::MapRange:
        case SoundNodeKind::EnvelopeFollower: return 1;
        case SoundNodeKind::LowPass:    // signal, cutoff offset in Hz
        case SoundNodeKind::HighPass:
        case SoundNodeKind::BandPass:
        case SoundNodeKind::Mix:
        case SoundNodeKind::Multiply:
        case SoundNodeKind::Delay:      return 2;   // signal, delay time offset in seconds
        case SoundNodeKind::Crossfade:  return 1 + kSoundMaxCrossfade;   // position offset, then the signals
        case SoundNodeKind::Count:      break;
    }
    return 0;
}

// ---------------------------------------------------------------- compile

i32 SoundProgram::findParam(const char* name) const {
    if (!name) return -1;
    for (u32 i = 0; i < paramNames_.size(); ++i)
        if (paramNames_[i] == name) return static_cast<i32>(i);
    return -1;
}

std::shared_ptr<const SoundProgram> SoundProgram::compile(const SoundGraphDesc& g, std::string* why) {
    auto bad = [&](std::string m) -> std::shared_ptr<const SoundProgram> { fail(why, "sound graph: " + std::move(m)); return nullptr; };
    const u32 n = static_cast<u32>(g.nodes.size());
    if (n == 0) return bad("it has no nodes");
    if (n > kSoundMaxNodes) return bad("it has " + std::to_string(n) + " nodes; the limit is " + std::to_string(kSoundMaxNodes));
    if (g.outputNode >= n) return bad("the output node is out of range");
    if (!(g.durationSec > 0.0f)) return bad("the duration is not positive");

    auto p = std::make_shared<SoundProgram>();
    p->outputNode_ = g.outputNode;
    p->durationSec_ = g.durationSec;
    p->nodes_.resize(n);

    f32 delayTotal = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        const SoundNodeDesc& d = g.nodes[i];
        if (static_cast<u32>(d.kind) >= static_cast<u32>(SoundNodeKind::Count))
            return bad("node " + std::to_string(i) + " has an unknown kind");
        Node& nd = p->nodes_[i];
        nd.kind = d.kind;
        for (int k = 0; k < 4; ++k) nd.p[k] = d.params[k];
        nd.nIn = soundNodeInputCount(d.kind);

        if (d.kind == SoundNodeKind::Parameter && !d.name.empty()) {
            u32 id = kNone;
            for (u32 j = 0; j < p->paramNames_.size(); ++j) if (p->paramNames_[j] == d.name) { id = j; break; }
            if (id == kNone) {
                if (p->paramNames_.size() >= kSoundMaxParams)
                    return bad("more than " + std::to_string(kSoundMaxParams) + " distinct Parameter names");
                id = static_cast<u32>(p->paramNames_.size());
                p->paramNames_.push_back(d.name);
                p->paramDefaults_.push_back(d.params[0]);
            }
            nd.aux = id;
        } else if (d.kind == SoundNodeKind::WavePlayer && !d.name.empty()) {
            u32 slot = kNone;
            for (u32 j = 0; j < p->waveAssets_.size(); ++j) if (p->waveAssets_[j] == d.name) { slot = j; break; }
            if (slot == kNone) {
                if (p->waveAssets_.size() >= kSoundMaxWaves)
                    return bad("more than " + std::to_string(kSoundMaxWaves) + " distinct WavePlayer assets");
                slot = static_cast<u32>(p->waveAssets_.size());
                p->waveAssets_.push_back(d.name);
            }
            nd.aux = slot;
        } else if (d.kind == SoundNodeKind::Delay) {
            delayTotal += delayMaxSec(d.params[3]);
        } else if (d.kind == SoundNodeKind::Adsr && d.params[3] > p->tailSec_) {
            p->tailSec_ = d.params[3];
        }
    }
    if (delayTotal > kSoundMaxDelayTotal)
        return bad("its Delay nodes need " + std::to_string(delayTotal) + " s of memory; the limit is " + std::to_string(kSoundMaxDelayTotal));

    // Gather form. Inputs sum their sources; the order is by ascending source node, then link order -- the order the original
    // forward-push renderer added them in, so the sum (float addition is not associative) is the same bit for bit.
    struct E { u32 to, input, from; };
    std::vector<E> es;
    es.reserve(g.links.size());
    for (const SoundLinkDesc& l : g.links) {
        if (l.from >= n || l.to >= n) return bad("a link names a node that does not exist");
        if (l.from >= l.to) return bad("a link does not run from a lower node to a higher one");
        if (l.input >= p->nodes_[l.to].nIn) return bad("a link feeds input " + std::to_string(l.input) + " of a node that has " +
                                                       std::to_string(p->nodes_[l.to].nIn));
        es.push_back({l.to, l.input, l.from});
    }
    std::stable_sort(es.begin(), es.end(), [](const E& a, const E& b) {
        if (a.to != b.to) return a.to < b.to;
        if (a.input != b.input) return a.input < b.input;
        return a.from < b.from;
    });
    usize e = 0;
    p->edges_.reserve(es.size());
    for (u32 i = 0; i < n; ++i) {
        Node& nd = p->nodes_[i];
        for (u32 k = 0; k < nd.nIn; ++k) {
            nd.inBegin[k] = static_cast<u32>(p->edges_.size());
            while (e < es.size() && es[e].to == i && es[e].input == k) p->edges_.push_back(es[e++].from);
        }
        nd.inBegin[nd.nIn] = static_cast<u32>(p->edges_.size());
    }
    return p;
}

// ---------------------------------------------------------------- runner

SoundRunner::SoundRunner(std::shared_ptr<const SoundProgram> program, const RunnerOptions& opt)
    : prog_(std::move(program)), opt_(opt) {
    for (u32 i = 0; i < kSoundMaxParams; ++i) {
        paramTarget_[i].store(0.0f, std::memory_order_relaxed);
        paramSet_[i].store(false, std::memory_order_relaxed);
    }
    if (opt_.sampleRate == 0) opt_.sampleRate = 48000;
    sr_ = static_cast<f32>(opt_.sampleRate);
    duration_ = opt_.durationSec > 0.0f ? opt_.durationSec : prog_->durationSec_;
    total_ = opt_.finite ? static_cast<u64>(duration_ * sr_) : ~0ull;
    tailFrames_ = static_cast<u64>(std::ceil(prog_->tailSec_ * sr_));
    declickFrames_ = std::min<u64>(tailFrames_, static_cast<u64>(std::ceil(kSoundMinTailSec * sr_)));
    initState();
}

void SoundRunner::initState() {
    const u32 n = static_cast<u32>(prog_->nodes_.size());
    st_.assign(n, NodeState{});
    val_.assign(n, 0.0f);
    for (u32 i = 0; i < n; ++i) {
        // Seeded per NODE as well as per render, so two Noise nodes in one graph are independent. |1 keeps xorshift off its fixed point.
        st_[i].rng = (opt_.seed * 2654435761u + i * 40503u) | 1u;
        const SoundProgram::Node& nd = prog_->nodes_[i];
        // A Random is drawn once, here: "per trigger" is "per runner".
        if (nd.kind == SoundNodeKind::Random) st_[i].val = nd.p[0] + nextUnit(st_[i].rng) * (nd.p[1] - nd.p[0]);
    }
    rebuildTimeConstants();
}

// Everything derived from the sample rate. Allocates the delay memory.
void SoundRunner::rebuildTimeConstants() {
    const u32 n = static_cast<u32>(prog_->nodes_.size());
    u64 delayTotal = 0;
    for (u32 i = 0; i < n; ++i) {
        const SoundProgram::Node& nd = prog_->nodes_[i];
        NodeState& st = st_[i];
        st.fcLast = st.qLast = -1.0f;
        switch (nd.kind) {
            case SoundNodeKind::Parameter:
                st.coef = smoothCoef(nd.p[1] > 0.0f ? nd.p[1] : kDefaultSmoothSec, sr_);
                break;
            case SoundNodeKind::EnvelopeFollower:
                st.coef = followCoef(nd.p[0], sr_);
                st.coef2 = followCoef(nd.p[1], sr_);
                break;
            case SoundNodeKind::Random:
                st.period = 1u;
                if (nd.p[2] > 0.0f) {
                    const f32 per = sr_ / nd.p[2];
                    st.period = per >= 4.0e9f ? 0xFFFFFFFFu : std::max<u32>(1u, static_cast<u32>(per));
                }
                st.count = 0;
                break;
            case SoundNodeKind::Delay:
                st.delayBase = static_cast<u32>(delayTotal);
                st.delayLen = static_cast<u32>(std::ceil(delayMaxSec(nd.p[3]) * sr_)) + 2u;
                st.delayPos = 0;
                delayTotal += st.delayLen;
                break;
            default: break;
        }
    }
    delayMem_.assign(static_cast<usize>(delayTotal), 0.0f);
}

void SoundRunner::setWave(u32 slot, const SoundData* data) {
    if (slot >= kSoundMaxWaves) return;
    waves_[slot] = data;
    // The start offset (seconds) of every WavePlayer on this slot.
    for (u32 i = 0; i < prog_->nodes_.size(); ++i) {
        const SoundProgram::Node& nd = prog_->nodes_[i];
        if (nd.kind == SoundNodeKind::WavePlayer && nd.aux == slot)
            st_[i].cursor = data && nd.p[2] > 0.0f ? static_cast<f64>(nd.p[2]) * static_cast<f64>(data->sampleRate) : 0.0;
    }
}

bool SoundRunner::setParam(u32 id, f32 value) {
    if (id >= prog_->paramCount() || !std::isfinite(value)) return false;
    paramTarget_[id].store(value, std::memory_order_relaxed);
    paramSet_[id].store(true, std::memory_order_release);
    return true;
}

u64 SoundRunner::remainingFrames() const {
    u64 lim = opt_.finite ? total_ : ~0ull;
    if (offLatched_ && endFrame_ < lim) lim = endFrame_;
    if (lim == ~0ull) return ~0ull;
    return lim > frame_ ? lim - frame_ : 0;
}

void SoundRunner::setSampleRate(u32 rate) {
    if (rate == 0 || rate == opt_.sampleRate) return;
    const f64 k = static_cast<f64>(rate) / static_cast<f64>(opt_.sampleRate);
    auto scale = [k](u64 v) { return static_cast<u64>(static_cast<f64>(v) * k + 0.5); };
    frame_ = scale(frame_);
    offFrame_ = scale(offFrame_);
    opt_.sampleRate = rate;
    sr_ = static_cast<f32>(rate);
    total_ = opt_.finite ? static_cast<u64>(duration_ * sr_) : ~0ull;
    tailFrames_ = static_cast<u64>(std::ceil(prog_->tailSec_ * sr_));
    declickFrames_ = std::min<u64>(tailFrames_, static_cast<u64>(std::ceil(kSoundMinTailSec * sr_)));
    if (offLatched_) endFrame_ = offFrame_ + tailFrames_;
    rebuildTimeConstants();
}

u32 SoundRunner::process(f32* out, u32 frames) {
    if (!out) return 0;
    u32 n = 0;
    if (!done_) {
        if (!offLatched_ && noteOff_.load(std::memory_order_acquire)) {
            offLatched_ = true;
            offFrame_ = frame_;
            endFrame_ = offFrame_ + tailFrames_;
        }
        const u32 np = prog_->paramCount();
        for (u32 i = 0; i < np; ++i) {
            paramLive_[i] = paramSet_[i].load(std::memory_order_acquire);
            if (paramLive_[i]) paramNow_[i] = paramTarget_[i].load(std::memory_order_relaxed);
        }

        u64 limit = opt_.finite ? total_ : ~0ull;
        const bool tailEnds = offLatched_ && endFrame_ <= limit;
        if (tailEnds) limit = endFrame_;

        for (; n < frames; ++n) {
            if (frame_ >= limit) { done_ = true; break; }
            f32 s = evalFrame();
            // The last few milliseconds of a note-off tail ramp to zero, so a graph with nothing to release (no Adsr) does not click.
            if (tailEnds && declickFrames_ > 0 && frame_ + declickFrames_ >= endFrame_)
                s *= static_cast<f32>(endFrame_ - frame_) / static_cast<f32>(declickFrames_);
            // CLAMPED, not normalised: an overloud graph should sound overloud. A NaN (a graph fed an absurd input) is silence, not poison
            // for the mix: both comparisons are false for it.
            out[n] = s != s ? 0.0f : (s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s));
            ++frame_;
        }
        if (frame_ >= limit) done_ = true;
    }
    for (u32 i = n; i < frames; ++i) out[i] = 0.0f;
    return n;
}

f32 SoundRunner::evalFrame() {
    const std::vector<SoundProgram::Node>& nodes = prog_->nodes_;
    const u32 count = static_cast<u32>(nodes.size());
    const f32 sr = sr_;
    const f32 t = static_cast<f32>(frame_) / sr;

    for (u32 i = 0; i < count; ++i) {
        const SoundProgram::Node& nd = nodes[i];
        NodeState& st = st_[i];

        f32 in[kSoundMaxInputs];
        for (u32 k = 0; k < nd.nIn; ++k) {
            f32 s = 0.0f;
            for (u32 e = nd.inBegin[k]; e < nd.inBegin[k + 1]; ++e) s += val_[prog_->edges_[e]];
            in[k] = s;
        }
        const f32 p0 = nd.p[0], p1 = nd.p[1], p2 = nd.p[2], p3 = nd.p[3];
        f32 v = 0.0f;

        switch (nd.kind) {
            case SoundNodeKind::Sine:
                v = std::sin(st.phase * kPi2);
                st.phase += (p0 + in[0]) / sr;
                break;
            case SoundNodeKind::Saw:
                v = st.phase * 2.0f - 1.0f;
                st.phase += (p0 + in[0]) / sr;
                break;
            case SoundNodeKind::Square: {
                const f32 duty = p1 > 0.0f && p1 < 1.0f ? p1 : 0.5f;
                v = st.phase < duty ? 1.0f : -1.0f;
                st.phase += (p0 + in[0]) / sr;
                break;
            }
            case SoundNodeKind::Noise:
                v = nextNoise(st.rng);
                break;
            case SoundNodeKind::Const:
                v = p0;
                break;
            case SoundNodeKind::Gain:
                v = in[0] * p0;
                break;
            case SoundNodeKind::LowPass:
                v = onePole(in[0], st.last, p0 + in[1], sr);
                break;
            case SoundNodeKind::Adsr: {
                f32 dur = opt_.finite ? duration_ : std::numeric_limits<f32>::infinity();
                if (offLatched_) {
                    const f32 rel = p3 < 0.0f ? 0.0f : p3;
                    const f32 tOff = static_cast<f32>(offFrame_) / sr;
                    if (tOff < dur - rel) dur = tOff + rel;
                }
                v = in[0] * adsrAt(t, dur, p0, p1, p2, p3);
                break;
            }
            case SoundNodeKind::Mix:
                v = in[0] + in[1];
                break;
            case SoundNodeKind::Multiply:
                v = in[0] * in[1];
                break;

            case SoundNodeKind::Parameter: {
                f32 tgt = (nd.aux != kNone && paramLive_[nd.aux]) ? paramNow_[nd.aux] : p0;
                if (p3 > p2) tgt = clampf(tgt, p2, p3);
                if (!st.primed) {
                    st.val = tgt;          // the first block snaps, so a voice started at rpm 3000 does not ramp up from the default
                    st.primed = true;
                } else {
                    st.val += (tgt - st.val) * st.coef;
                    if (std::fabs(tgt - st.val) < 1.0e-7f * (1.0f + std::fabs(tgt))) st.val = tgt;
                }
                v = st.val;
                break;
            }
            case SoundNodeKind::Lfo: {
                f32 w;
                switch (p2 > 0.0f && p2 < 100.0f ? static_cast<u32>(p2 + 0.5f) : 0u) {
                    case 1: { f32 x = st.phase + 0.75f; x -= std::floor(x); w = 2.0f * std::fabs(2.0f * x - 1.0f) - 1.0f; break; }   // triangle, from 0 rising
                    case 2: w = st.phase < 0.5f ? 1.0f : -1.0f; break;
                    case 3: w = st.phase * 2.0f - 1.0f; break;
                    default: w = std::sin(st.phase * kPi2); break;
                }
                v = p3 + p1 * w;
                st.phase += (p0 + in[0]) / sr;
                break;
            }
            case SoundNodeKind::HighPass:
            case SoundNodeKind::BandPass: {
                const bool high = nd.kind == SoundNodeKind::HighPass;
                const f32 fc = p0 + in[1];
                if (fc <= 0.0f) { v = high ? in[0] : 0.0f; break; }
                const f32 q = p1 > 0.0f ? p1 : 0.70710678f;
                if (fc != st.fcLast || q != st.qLast) {
                    // Topology-preserving state-variable filter: stable under per-sample cutoff changes.
                    const f32 fcc = clampf(fc, 1.0f, 0.45f * sr);
                    const f32 k = 1.0f / clampf(q, 0.1f, 20.0f);
                    const f32 g = std::tan(kPi * fcc / sr);
                    st.k = k;
                    st.a1 = 1.0f / (1.0f + g * (g + k));
                    st.a2 = g * st.a1;
                    st.a3 = g * st.a2;
                    st.fcLast = fc;
                    st.qLast = q;
                }
                const f32 v3 = in[0] - st.s2;
                const f32 v1 = st.a1 * st.s1 + st.a2 * v3;
                const f32 v2 = st.s2 + st.a2 * st.s1 + st.a3 * v3;
                st.s1 = flush(2.0f * v1 - st.s1);
                st.s2 = flush(2.0f * v2 - st.s2);
                v = high ? in[0] - st.k * v1 - v2 : st.k * v1;   // band-pass peaks at unity gain
                break;
            }
            case SoundNodeKind::Random:
                v = st.val;
                if (p2 > 0.0f && ++st.count >= st.period) {
                    st.count = 0;
                    st.val = p0 + nextUnit(st.rng) * (p1 - p0);
                }
                break;
            case SoundNodeKind::Crossfade: {
                u32 cnt = p1 > 0.0f ? static_cast<u32>(p1 + 0.5f) : 2u;
                cnt = cnt < 1u ? 1u : (cnt > kSoundMaxCrossfade ? kSoundMaxCrossfade : cnt);
                const f32 pos = clampf(p0 + in[0], 0.0f, static_cast<f32>(cnt - 1));
                u32 i0 = static_cast<u32>(pos);
                if (i0 + 1 >= cnt) { v = in[cnt]; break; }
                const f32 fr = pos - static_cast<f32>(i0);
                const bool linear = p2 >= 0.5f;
                const f32 wa = linear ? 1.0f - fr : std::sqrt(1.0f - fr);   // equal power: wa^2 + wb^2 == 1
                const f32 wb = linear ? fr : std::sqrt(fr);
                v = in[1 + i0] * wa + in[2 + i0] * wb;
                break;
            }
            case SoundNodeKind::WavePlayer: {
                const SoundData* w = nd.aux != kNone ? waves_[nd.aux] : nullptr;
                const u32 fr = w ? w->frames() : 0;
                if (fr == 0) break;
                const bool loop = p1 >= 0.5f;
                const u32 loopEnd = (w->loopEnd > 0 && w->loopEnd <= fr) ? w->loopEnd : fr;
                const u32 loopBegin = w->loopBegin < loopEnd ? w->loopBegin : 0;
                if (st.cursor >= static_cast<f64>(loopEnd)) {
                    if (!loop || loopEnd == loopBegin) break;
                    const f64 len = static_cast<f64>(loopEnd - loopBegin);
                    st.cursor = static_cast<f64>(loopBegin) + std::fmod(st.cursor - static_cast<f64>(loopBegin), len);
                }
                const u32 i0 = static_cast<u32>(st.cursor);
                const f32 frac = static_cast<f32>(st.cursor - static_cast<f64>(i0));
                u32 i1 = i0 + 1;
                if (i1 >= loopEnd) i1 = loop ? loopBegin : loopEnd - 1;
                f32 a, b;
                if (w->channels >= 2) {
                    const usize o0 = static_cast<usize>(i0) * w->channels, o1 = static_cast<usize>(i1) * w->channels;
                    a = 0.5f * (w->samples[o0] + w->samples[o0 + 1]);
                    b = 0.5f * (w->samples[o1] + w->samples[o1 + 1]);
                } else {
                    a = w->samples[i0];
                    b = w->samples[i1];
                }
                v = a + (b - a) * frac;
                f64 ratio = p0 > 0.0f ? static_cast<f64>(p0) : 1.0;
                if (in[0] != 0.0f) ratio *= static_cast<f64>(std::exp2(in[0] * (1.0f / 12.0f)));
                st.cursor += static_cast<f64>(w->sampleRate) / static_cast<f64>(sr) * ratio;
                if (!(st.cursor >= 0.0 && st.cursor < 4.0e9)) st.cursor = 0.0;   // a runaway pitch input must not index out of the sample
                break;
            }
            case SoundNodeKind::Delay: {
                f32* buf = delayMem_.data() + st.delayBase;
                const u32 len = st.delayLen;
                const f32 maxD = static_cast<f32>(len - 2);
                const f32 d = clampf((p0 + in[1]) * sr, 1.0f, maxD);   // at least one frame: the current input is not in the line yet
                const u32 d0 = static_cast<u32>(d);
                const f32 frac = d - static_cast<f32>(d0);
                const u32 w = st.delayPos;
                // `d` frames back, interpolating between the two neighbours.
                const f32 a = buf[(w + len - d0) % len];
                const f32 b = buf[(w + len - (d0 + 1u)) % len];
                const f32 delayed = a + (b - a) * frac;
                const f32 fb = clampf(p1, 0.0f, 0.95f);
                const f32 mix = clampf(p2, 0.0f, 1.0f);
                buf[w] = flush(in[0] + delayed * fb);
                st.delayPos = (w + 1u == len) ? 0u : w + 1u;
                v = in[0] * (1.0f - mix) + delayed * mix;
                break;
            }
            case SoundNodeKind::Clamp:
                v = in[0] < p0 ? p0 : (in[0] > p1 ? p1 : in[0]);
                break;
            case SoundNodeKind::MapRange: {
                const f32 lo = p0 < p1 ? p0 : p1, hi = p0 < p1 ? p1 : p0;
                const f32 span = p1 - p0;
                const f32 u = span != 0.0f ? (clampf(in[0], lo, hi) - p0) / span : 0.0f;
                v = p2 + u * (p3 - p2);
                break;
            }
            case SoundNodeKind::EnvelopeFollower: {
                const f32 x = std::fabs(in[0]);
                st.val = flush(x > st.val ? x + st.coef * (st.val - x) : x + st.coef2 * (st.val - x));
                v = st.val;
                break;
            }
            case SoundNodeKind::Count: break;
        }

        // Oscillator phase wraps in TURNS, kept in [0,1) so precision does not decay over a long run. A negative phase can only come from a
        // frequency input driving the rate below zero; wrapping it up is limited to nodes that have one, so a graph without a connected
        // input renders exactly as it did before inputs existed.
        if (st.phase >= 1.0f) st.phase -= std::floor(st.phase);
        else if (st.phase < 0.0f && nd.nIn > 0 && nd.inBegin[1] != nd.inBegin[0]) st.phase -= std::floor(st.phase);

        val_[i] = v;
    }
    return val_[prog_->outputNode_];
}

} // namespace aver::audio
