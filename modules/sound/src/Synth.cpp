#include "aver/sound/Synth.hpp"

#include <cmath>

namespace aver::sound {
namespace {

constexpr f32 kPi2 = 6.28318530717958647692f;

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// PER-NODE STATE, kept in its own parallel array rather than on the node, for the reason
// AgentSystem keeps a path out of CSynapseAgent: the ASSET is what the author wrote and must stay
// immutable across renders, while phase and filter memory belong to one evaluation of it. Keeping
// them apart is also what would let the real-time path pre-allocate this once per voice and
// evaluate with no allocation at all -- see Synth.hpp's own note.
struct NodeState {
    f32 phase = 0.0f;   // oscillators, in turns [0,1)
    f32 last  = 0.0f;   // LowPass memory
    u32 rng   = 1u;     // Noise, seeded per node so two Noise nodes are not identical
};

// xorshift32. Deterministic, seeded per render, and deliberately NOT a global generator -- two
// renders with the same seed must produce byte-identical audio or the format stops being an asset.
f32 nextNoise(u32& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    // [0,1) -> [-1,1)
    return (static_cast<f32>(s & 0xFFFFFFu) / 8388608.0f) - 1.0f;
}

// One-pole low pass. `cutoff` in Hz; a cutoff at or above Nyquist passes the signal through.
f32 onePole(f32 in, f32& last, f32 cutoffHz, f32 sampleRate) {
    if (cutoffHz <= 0.0f) return 0.0f;
    const f32 nyquist = sampleRate * 0.5f;
    if (cutoffHz >= nyquist) { last = in; return in; }
    // The standard RC one-pole coefficient. dt/(RC+dt), with RC = 1/(2*pi*fc).
    const f32 rc = 1.0f / (kPi2 * cutoffHz);
    const f32 dt = 1.0f / sampleRate;
    const f32 a  = dt / (rc + dt);
    last += a * (in - last);
    return last;
}

// Attack/decay/sustain/release across the WHOLE render. `t` and `duration` in seconds.
//
// RELEASE IS MEASURED BACK FROM THE END, which is the only thing it can mean for an offline render:
// there is no note-off to react to, because the sound's whole length is known before it starts. A
// real-time path would take note-off as an input instead; this is the one place the two designs
// genuinely differ rather than merely sharing less code.
f32 adsrAt(f32 t, f32 duration, f32 a, f32 d, f32 s, f32 r) {
    if (t < 0.0f || t > duration) return 0.0f;
    a = a < 0.0f ? 0.0f : a;
    d = d < 0.0f ? 0.0f : d;
    r = r < 0.0f ? 0.0f : r;
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);

    const f32 releaseStart = duration - r;
    if (r > 0.0f && t >= releaseStart) {
        // Falls from whatever the envelope was AT releaseStart, not from sustain -- a release that
        // begins during the attack must not jump up to 1 first.
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

} // namespace

bool renderSound(const fmt::OcSoundData& graph, const RenderParams& params,
                 std::vector<f32>& out, std::string* why, u32 maxFrames) {
    if (!graph.valid()) return fail(why, "sound: the graph does not hold together");
    if (params.sampleRate == 0) return fail(why, "sound: sample rate is 0");

    const f32 duration = params.durationSec > 0.0f ? params.durationSec : graph.durationSec;
    if (duration <= 0.0f) return fail(why, "sound: duration is not positive");

    const u64 want = static_cast<u64>(duration * static_cast<f32>(params.sampleRate));
    if (want == 0) return fail(why, "sound: the render is zero frames long");
    if (want > maxFrames)
        return fail(why, "sound: a render of " + std::to_string(want) + " frames exceeds the cap of " +
                         std::to_string(maxFrames) + " -- durationSec comes out of an asset file, so "
                         "this is a bound on corrupt or merely optimistic input, not a limit worth raising blindly");

    const u32 frames = static_cast<u32>(want);
    const u32 n = static_cast<u32>(graph.nodes.size());
    const f32 sr = static_cast<f32>(params.sampleRate);

    std::vector<NodeState> state(n);
    for (u32 i = 0; i < n; ++i) {
        // Seeded per NODE as well as per render, so two Noise nodes in one graph are independent
        // rather than phase-locked to each other. |1 keeps xorshift off its fixed point at zero.
        state[i].rng = (params.seed * 2654435761u + i * 40503u) | 1u;
    }

    // Scratch: this sample's output for every node. Rebuilt each frame; nodes are visited in index
    // order, which .ocsnd's own "sources before consumers" rule makes a valid topological order --
    // see OcSound.hpp. That is the whole reason no sort is needed here.
    std::vector<f32> value(n, 0.0f);
    std::vector<f32> in0(n, 0.0f), in1(n, 0.0f);

    out.clear();
    out.resize(frames, 0.0f);

    for (u32 f = 0; f < frames; ++f) {
        const f32 t = static_cast<f32>(f) / sr;

        for (u32 i = 0; i < n; ++i) { in0[i] = 0.0f; in1[i] = 0.0f; }

        for (u32 i = 0; i < n; ++i) {
            const fmt::OcSoundNode& node = graph.nodes[i];
            NodeState& st = state[i];
            const f32 p0 = node.params[0], p1 = node.params[1];
            f32 v = 0.0f;

            switch (node.kind) {
                case fmt::OcSoundNodeKind::Sine:
                    v = std::sin(st.phase * kPi2);
                    st.phase += p0 / sr;
                    break;
                case fmt::OcSoundNodeKind::Saw:
                    // Rises -1 -> 1 across one period.
                    v = st.phase * 2.0f - 1.0f;
                    st.phase += p0 / sr;
                    break;
                case fmt::OcSoundNodeKind::Square: {
                    const f32 duty = p1 > 0.0f && p1 < 1.0f ? p1 : 0.5f;
                    v = st.phase < duty ? 1.0f : -1.0f;
                    st.phase += p0 / sr;
                    break;
                }
                case fmt::OcSoundNodeKind::Noise:
                    v = nextNoise(st.rng);
                    break;
                case fmt::OcSoundNodeKind::Const:
                    v = p0;
                    break;
                case fmt::OcSoundNodeKind::Gain:
                    v = in0[i] * p0;
                    break;
                case fmt::OcSoundNodeKind::LowPass:
                    v = onePole(in0[i], st.last, p0, sr);
                    break;
                case fmt::OcSoundNodeKind::Adsr:
                    v = in0[i] * adsrAt(t, duration, node.params[0], node.params[1],
                                        node.params[2], node.params[3]);
                    break;
                case fmt::OcSoundNodeKind::Mix:
                    v = in0[i] + in1[i];
                    break;
                case fmt::OcSoundNodeKind::Multiply:
                    v = in0[i] * in1[i];
                    break;
            }

            // Oscillator phase wraps in TURNS, kept in [0,1) so precision does not decay over a long
            // render the way an ever-growing radian accumulator would.
            if (st.phase >= 1.0f) st.phase -= std::floor(st.phase);

            value[i] = v;

            // Push this node's value forward along its edges. Forward rather than gathering,
            // because a node may feed several targets and every target is at a HIGHER index -- so
            // by the time the loop reaches them, their inputs are already complete.
            for (const fmt::OcSoundLink& l : graph.links) {
                if (l.fromNode != i) continue;
                if (l.toInput == 0) in0[l.toNode] += v;
                else                in1[l.toNode] += v;
            }
        }

        f32 s = value[graph.outputNode];
        // CLAMPED, not normalised. A graph that sums four oscillators genuinely is too loud, and
        // silently scaling it would hide that from whoever authored it; clipping is audible, which
        // is the feedback they need.
        s = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
        out[f] = s;
    }

    return true;
}

} // namespace aver::sound
