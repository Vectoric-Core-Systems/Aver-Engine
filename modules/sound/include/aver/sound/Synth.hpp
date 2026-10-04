#pragma once
// Aver Sound: turning a .ocsnd node graph into PCM.
//
// PURE, and for the same reason Aver.Synapse's pathfinder is: no device, no mixer, no scene. A
// graph and arithmetic go in, a buffer of samples comes out, and the whole synthesiser can be
// checked against numbers a test computes independently -- a 440 Hz sine really crossing zero 440
// times a second, an envelope really reaching its sustain level. None of that needs a sound card.
//
// WHY OFFLINE, AND WHAT IT COSTS. This renders a whole sound up front into a buffer that the
// ordinary mixer then plays like any other. The alternative -- evaluating the graph per block on
// the audio thread, which is what Unreal's MetaSounds does -- is a materially harder problem here:
// Mixer.hpp's own contract is that mix() "must not allocate, lock, block, log or call managed
// code", so a real-time graph needs every node's state pre-allocated and its evaluation free of
// every one of those. Rendering offline touches none of that machinery and therefore cannot
// introduce an audio-thread race.
//
// What it BUYS: a footstep synthesised fresh per step, a different seed each time, no wav files.
// What it COSTS, stated plainly rather than discovered later: a sound cannot respond to a live
// parameter once it has started. Modulation that varies DURING a note has to be authored into the
// graph (an Adsr, an LFO through Multiply), not driven from gameplay. Per-play variation is a
// render parameter; per-instant variation is not available until the real-time path exists. This
// evaluator is deliberately shaped so that path can reuse it -- see RenderParams::seed and the
// per-node state split in the .cpp.
#include "aver/core/Types.hpp"
#include "aver/formats/OcSound.hpp"

#include <string>
#include <vector>

namespace aver::sound {

struct RenderParams {
    u32 sampleRate = 48000;
    // PER-PLAY VARIATION, and the reason a graph is worth more than a .wav: the same graph rendered
    // with two seeds gives two footsteps that are recognisably the same sound and not identical.
    // Only Noise consumes it today; anything stochastic added later should take it from here rather
    // than reaching for a global generator, so a render stays reproducible.
    u32 seed = 1;
    // Overrides the graph's own durationSec when > 0. A caller that wants one graph at two lengths
    // needs this; most callers leave it alone.
    f32 durationSec = 0.0f;
};

// Renders `graph` into `out` as MONO f32, one sample per frame, in [-1, 1] after a final clamp.
//
// False when the graph is invalid, the sample rate is 0, or the render would exceed
// `maxFrames` -- which exists because durationSec comes out of an asset file and a corrupt or
// merely optimistic one should not be able to ask for a gigabyte of samples.
bool renderSound(const fmt::OcSoundData& graph, const RenderParams& params,
                 std::vector<f32>& out, std::string* why = nullptr,
                 u32 maxFrames = 48000u * 60u);

} // namespace aver::sound
