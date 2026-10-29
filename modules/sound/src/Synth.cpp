#include "aver/sound/Synth.hpp"

namespace aver::sound {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

using Fk = fmt::OcSoundNodeKind;
using Ak = audio::SoundNodeKind;

// THE TWO ENUMS, PINNED. The format stores these numbers; the evaluator switches on its own copy (modules/audio cannot include the format).
#define AVER_PIN_KIND(name) static_assert(static_cast<u32>(Fk::name) == static_cast<u32>(Ak::name), "OcSoundNodeKind and SoundNodeKind must agree: " #name)
AVER_PIN_KIND(Sine); AVER_PIN_KIND(Saw); AVER_PIN_KIND(Square); AVER_PIN_KIND(Noise); AVER_PIN_KIND(Const);
AVER_PIN_KIND(Gain); AVER_PIN_KIND(LowPass); AVER_PIN_KIND(Adsr); AVER_PIN_KIND(Mix); AVER_PIN_KIND(Multiply);
AVER_PIN_KIND(Parameter); AVER_PIN_KIND(Lfo); AVER_PIN_KIND(HighPass); AVER_PIN_KIND(BandPass); AVER_PIN_KIND(Random);
AVER_PIN_KIND(Crossfade); AVER_PIN_KIND(WavePlayer); AVER_PIN_KIND(Delay); AVER_PIN_KIND(Clamp); AVER_PIN_KIND(MapRange);
AVER_PIN_KIND(EnvelopeFollower);
#undef AVER_PIN_KIND
static_assert(fmt::kOcSoundLastKind + 1 == static_cast<u32>(Ak::Count), "a new node kind needs its format entry and its evaluator case");
static_assert(fmt::kOcSoundMaxCrossfade == audio::kSoundMaxCrossfade, "the crossfade width is one number in two places");

} // namespace

audio::SoundGraphDesc describeSound(const fmt::OcSoundData& graph) {
    audio::SoundGraphDesc d;
    d.outputNode = graph.outputNode;
    d.durationSec = graph.durationSec;
    d.nodes.resize(graph.nodes.size());
    for (usize i = 0; i < graph.nodes.size(); ++i) {
        d.nodes[i].kind = static_cast<audio::SoundNodeKind>(static_cast<u32>(graph.nodes[i].kind));
        for (int k = 0; k < 4; ++k) d.nodes[i].params[k] = graph.nodes[i].params[k];
        d.nodes[i].name = graph.nodes[i].name;
    }
    d.links.resize(graph.links.size());
    for (usize i = 0; i < graph.links.size(); ++i)
        d.links[i] = {graph.links[i].fromNode, graph.links[i].toNode, graph.links[i].toInput};
    return d;
}

std::shared_ptr<const audio::SoundProgram> compileSound(const fmt::OcSoundData& graph, std::string* why) {
    if (!graph.valid()) { fail(why, "sound: the graph does not hold together"); return nullptr; }
    return audio::SoundProgram::compile(describeSound(graph), why);
}

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

    const std::shared_ptr<const audio::SoundProgram> program = compileSound(graph, why);
    if (!program) return false;

    // The runner is the real-time evaluator; here it just runs once over the whole length, finite, with no note-off.
    audio::RunnerOptions opt;
    opt.sampleRate = params.sampleRate;
    opt.seed = params.seed;
    opt.finite = true;
    opt.durationSec = params.durationSec;
    audio::SoundRunner runner(program, opt);
    for (const auto& kv : params.inputs) {
        const i32 id = program->findParam(kv.first.c_str());
        if (id >= 0) runner.setParam(static_cast<u32>(id), kv.second);
    }
    if (params.resolveWave)
        for (u32 s = 0; s < program->waveCount(); ++s) runner.setWave(s, params.resolveWave(program->waveAsset(s)));

    out.clear();
    out.resize(static_cast<usize>(want), 0.0f);
    runner.process(out.data(), static_cast<u32>(want));
    return true;
}

} // namespace aver::sound
