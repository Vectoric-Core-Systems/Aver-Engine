#pragma once
// .ocsnd -- a procedural sound, authored as a node graph, as an AVR1 container.
//
// A DAG, NOT A TREE, and that is the one structural difference from .ocbt beside it: a behaviour
// tree node has exactly one parent, whereas an oscillator's output feeds a gain AND a filter at
// once. So edges are their own records (OcSoundLink) rather than a parent index on the node.
//
// SOURCES BEFORE CONSUMERS. Every link must run from a LOWER node index to a HIGHER one, which
// OcSoundData::valid() enforces. That single comparison buys three things at once: evaluation is a
// forward walk with no topological sort, a cycle is unrepresentable rather than merely rejected,
// and the check is the same shape .ocskel and .ocbt already use for "parents before children".
//
// RENDERED OFFLINE, INTO A BUFFER. Aver.Sound turns one of these into PCM which the ordinary mixer
// then plays as any other sound -- see modules/sound/include/aver/sound/Synth.hpp for why that is
// the first shape rather than evaluating on the audio thread, and what it costs. The same graph can also play in real time:
// modules/audio's SoundGraph is the evaluator on both paths (docs/AUDIO.md section 13).
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// What one node does. Pinned to Aver.Sound's own evaluator (modules/sound/src/Synth.cpp) by a
// static_assert there rather than by this header depending on it.
enum class OcSoundNodeKind : u32 {
    // ---- sources: no inputs, params only ----
    Sine     = 0,   // params[0] = frequency Hz
    Saw      = 1,   // params[0] = frequency Hz
    Square   = 2,   // params[0] = frequency Hz, params[1] = duty 0..1 (0 reads as 0.5)
    Noise    = 3,   // params[0] = unused; the render seed drives it, so each play differs
    Const    = 4,   // params[0] = the value, for feeding a modulation input
    // ---- shapers: one input ----
    Gain     = 5,   // in0 * params[0]
    LowPass  = 6,   // one-pole, params[0] = cutoff Hz
    Adsr     = 7,   // in0 * envelope; params = attack, decay, sustain, release (sec, sec, 0..1, sec)
    // ---- combiners: two inputs ----
    Mix      = 8,   // in0 + in1
    Multiply = 9,   // in0 * in1 -- ring modulation, and how an envelope drives a gain
    // ---- format version 2 (docs/SOUND.md section 7). Evaluated by modules/audio's SoundGraph, the same code on the audio thread. ----
    Parameter = 10, // a live input named by the node's name. params = default, smoothing seconds (0 reads as 0.02), min, max (clamped when max > min)
    Lfo       = 11, // in0 = rate offset Hz. params = rate Hz, depth, shape (0 sine, 1 triangle, 2 square, 3 saw), offset; out = offset + depth * wave
    HighPass  = 12, // state-variable filter. in0 = signal, in1 = cutoff offset Hz. params = cutoff Hz, Q (0 reads as 0.7071)
    BandPass  = 13, // as HighPass; unity gain at the centre
    Random    = 14, // a value in [min, max) drawn once per trigger (per play) from the play's seed. params = min, max, redraw rate Hz (0 = once)
    Crossfade = 15, // in0 = position offset, in1..in8 = signals. params = position (0..N-1), N (0 reads as 2), curve (0 equal power, 1 linear)
    WavePlayer = 16,// plays the sound asset named by the node's name. in0 = pitch offset in semitones. params = pitch ratio (0 reads as 1), loop (>= 0.5), start seconds
    Delay     = 17, // in0 = signal, in1 = time offset seconds. params = time s, feedback 0..0.95, mix 0..1, max length s (0 reads as 1, at most 10)
    Clamp     = 18, // in0 clamped to params[0]..params[1]
    MapRange  = 19, // in0 clamped to params[0]..params[1] and mapped linearly onto params[2]..params[3]
    EnvelopeFollower = 20, // peak follower of |in0|. params = attack seconds, release seconds
};

// The last kind the current format knows.
inline constexpr u32 kOcSoundLastKind = static_cast<u32>(OcSoundNodeKind::EnvelopeFollower);
inline constexpr u32 kOcSoundMaxCrossfade = 8;
inline constexpr u32 kOcSoundMaxName = 512;   // bytes of a Parameter name or WavePlayer asset path

// How many INPUTS a kind consumes. An unconnected input reads 0, except where a kind documents
// otherwise -- which is why Gain multiplies by a PARAM rather than a second input: an unconnected
// gain input would silence the graph, and silence is the least useful default there is.
//
// Version 2 gave the oscillators a frequency-offset input (Hz, added to params[0]) and LowPass a cutoff-offset input; both read 0 when
// unconnected, so every v1 graph renders as it did. ocSoundInputCountV1 is what a v1 reader knew, used to decide the file version.
inline constexpr u32 ocSoundInputCountV1(OcSoundNodeKind k) {
    switch (k) {
        case OcSoundNodeKind::Sine:
        case OcSoundNodeKind::Saw:
        case OcSoundNodeKind::Square:
        case OcSoundNodeKind::Noise:
        case OcSoundNodeKind::Const:    return 0;
        case OcSoundNodeKind::Gain:
        case OcSoundNodeKind::LowPass:
        case OcSoundNodeKind::Adsr:     return 1;
        case OcSoundNodeKind::Mix:
        case OcSoundNodeKind::Multiply: return 2;
        default:                        return 0;
    }
}

inline constexpr u32 ocSoundInputCount(OcSoundNodeKind k) {
    switch (k) {
        case OcSoundNodeKind::Noise:
        case OcSoundNodeKind::Const:
        case OcSoundNodeKind::Parameter:
        case OcSoundNodeKind::Random:   return 0;
        case OcSoundNodeKind::Sine:
        case OcSoundNodeKind::Saw:
        case OcSoundNodeKind::Square:
        case OcSoundNodeKind::Gain:
        case OcSoundNodeKind::Adsr:
        case OcSoundNodeKind::Lfo:
        case OcSoundNodeKind::WavePlayer:
        case OcSoundNodeKind::Clamp:
        case OcSoundNodeKind::MapRange:
        case OcSoundNodeKind::EnvelopeFollower: return 1;
        case OcSoundNodeKind::LowPass:
        case OcSoundNodeKind::HighPass:
        case OcSoundNodeKind::BandPass:
        case OcSoundNodeKind::Mix:
        case OcSoundNodeKind::Multiply:
        case OcSoundNodeKind::Delay:    return 2;
        case OcSoundNodeKind::Crossfade: return 1 + kOcSoundMaxCrossfade;
    }
    return 0;
}

struct OcSoundNode {
    OcSoundNodeKind kind = OcSoundNodeKind::Sine;
    f32 params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // Parameter: the live input's name. WavePlayer: the asset path. Empty for every other kind (and in every v1 file).
    std::string name;
};

// One edge. `toInput` selects which of the target's inputs this feeds, 0-based.
struct OcSoundLink {
    u32 fromNode = 0;
    u32 toNode   = 0;
    u32 toInput  = 0;
};

struct OcSoundData {
    std::vector<OcSoundNode> nodes;
    std::vector<OcSoundLink> links;
    // Which node's output IS the sound. Not implicitly the last node: an author can leave a
    // half-built branch in the graph without it silently becoming the output.
    u32 outputNode = 0;
    // How long a render of this graph runs for, seconds. Carried in the asset because it is a
    // property of the SOUND (a footstep is short, a drone is long), not of whoever plays it.
    f32 durationSec = 1.0f;

    // Non-empty; outputNode in range; every link in range, feeding a real input, and running from a
    // lower index to a higher one.
    bool valid() const;

    // Whether writing this graph needs format version 2 (a new node kind, a name, or a link into an input v1 did not have). A graph that
    // does not is written as version 1, byte for byte what the previous build wrote, so older tools still open it.
    bool needsVersion2() const;
};

inline constexpr u16 kOcSoundVersion = 2;   // the newest content version this build reads and writes

bool loadOcSound(const std::string& path, OcSoundData& out, std::string* why = nullptr);
bool saveOcSound(const std::string& path, const OcSoundData& in, std::string* why = nullptr);
bool parseOcSound(const u8* bytes, usize size, OcSoundData& out, std::string* why = nullptr);
bool writeOcSound(const OcSoundData& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
