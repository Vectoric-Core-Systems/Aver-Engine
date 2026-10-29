#pragma once
// Aver Sound: turning a .ocsnd node graph into PCM, or into a program the mixer evaluates on the audio thread.
//
// PURE, and for the same reason Aver.Synapse's pathfinder is: no device, no mixer, no scene. A
// graph and arithmetic go in, a buffer of samples comes out, and the whole synthesiser can be
// checked against numbers a test computes independently -- a 440 Hz sine really crossing zero 440
// times a second, an envelope really reaching its sustain level. None of that needs a sound card.
//
// TWO WAYS TO PLAY A GRAPH, ONE EVALUATOR. renderSound() renders a whole sound up front into a buffer
// that the ordinary mixer then plays like any other: a footstep synthesised fresh per step with a
// new seed, no wav files. It cannot respond to anything once it has started. compileSound() instead
// hands the mixer a program (modules/audio's SoundGraph) that Mixer::playGraph evaluates per block on
// the audio thread, with named live inputs (rpm, intensity, surface) and a note-off. Both run the
// SAME evaluator -- renderSound constructs a SoundRunner and calls process() once -- so a graph with
// no live inputs produces identical samples either way (tests/audio/SoundGraphTest pins it). The
// real-time rule (Mixer.hpp: no allocation, lock, log or managed call in mix()) is met by the runner
// allocating everything in its constructor, on the game thread.
#include "aver/audio/SoundGraph.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcSound.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aver::sound {

struct RenderParams {
    u32 sampleRate = 48000;
    // PER-PLAY VARIATION, and the reason a graph is worth more than a .wav: the same graph rendered
    // with two seeds gives two footsteps that are recognisably the same sound and not identical.
    // Noise and Random consume it; anything stochastic added later should take it from here rather
    // than reaching for a global generator, so a render stays reproducible.
    u32 seed = 1;
    // Overrides the graph's own durationSec when > 0. A caller that wants one graph at two lengths
    // needs this; most callers leave it alone.
    f32 durationSec = 0.0f;
    // WavePlayer nodes name an asset; this turns that name into decoded audio that outlives the render. Unset, or a null result,
    // and the player is silent.
    std::function<const audio::SoundData*(const std::string&)> resolveWave;
    // Parameter nodes held at a value for the whole render (name, value); a name no node uses is ignored. Unset, a Parameter reads its default.
    std::vector<std::pair<std::string, f32>> inputs;
};

// Renders `graph` into `out` as MONO f32, one sample per frame, in [-1, 1] after a final clamp.
//
// False when the graph is invalid, the sample rate is 0, or the render would exceed
// `maxFrames` -- which exists because durationSec comes out of an asset file and a corrupt or
// merely optimistic one should not be able to ask for a gigabyte of samples.
bool renderSound(const fmt::OcSoundData& graph, const RenderParams& params,
                 std::vector<f32>& out, std::string* why = nullptr,
                 u32 maxFrames = 48000u * 60u);

// The format's graph as the evaluator's plain description (kinds are numerically the same; this copies params, names and links).
audio::SoundGraphDesc describeSound(const fmt::OcSoundData& graph);

// Compiles a .ocsnd for Mixer::playGraph. Null with `why` set when the graph is invalid or exceeds an evaluator limit.
std::shared_ptr<const audio::SoundProgram> compileSound(const fmt::OcSoundData& graph, std::string* why = nullptr);

} // namespace aver::sound
