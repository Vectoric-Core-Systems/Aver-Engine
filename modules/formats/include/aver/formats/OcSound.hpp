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
// the first shape rather than evaluating on the audio thread, and what it costs.
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
};

// How many INPUTS a kind consumes. An unconnected input reads 0, except where a kind documents
// otherwise -- which is why Gain multiplies by a PARAM rather than a second input: an unconnected
// gain input would silence the graph, and silence is the least useful default there is.
inline constexpr u32 ocSoundInputCount(OcSoundNodeKind k) {
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
    }
    return 0;
}

struct OcSoundNode {
    OcSoundNodeKind kind = OcSoundNodeKind::Sine;
    f32 params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
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
};

bool loadOcSound(const std::string& path, OcSoundData& out, std::string* why = nullptr);
bool saveOcSound(const std::string& path, const OcSoundData& in, std::string* why = nullptr);
bool parseOcSound(const u8* bytes, usize size, OcSoundData& out, std::string* why = nullptr);
bool writeOcSound(const OcSoundData& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
