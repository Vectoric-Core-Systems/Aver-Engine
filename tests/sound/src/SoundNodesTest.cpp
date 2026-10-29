// The version-2 .ocsnd: the new node kinds rendered offline and checked against numbers this file computes for itself, the format's
// versioning (a v1 file loads unchanged, a v2 file round-trips, hostile input is refused), and the evaluator's limits.
//
// Offline only: no device, no mixer. The real-time side (live inputs, note-off, the pool, no allocation in mix()) is tests/audio/SoundGraphTest.
#include "aver/sound/Synth.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/formats/OcSound.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using K = fmt::OcSoundNodeKind;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static fmt::OcSoundNode mk(K k, f32 p0 = 0, f32 p1 = 0, f32 p2 = 0, f32 p3 = 0, std::string name = {}) {
    fmt::OcSoundNode n; n.kind = k; n.params[0] = p0; n.params[1] = p1; n.params[2] = p2; n.params[3] = p3; n.name = std::move(name); return n;
}

static fmt::OcSoundData graph(std::vector<fmt::OcSoundNode> nodes, std::vector<fmt::OcSoundLink> links, f32 seconds = 0.1f) {
    fmt::OcSoundData g;
    g.nodes = std::move(nodes);
    g.links = std::move(links);
    g.outputNode = static_cast<u32>(g.nodes.size()) - 1;
    g.durationSec = seconds;
    return g;
}

static std::vector<f32> render(const fmt::OcSoundData& g, sound::RenderParams p = {}) {
    std::vector<f32> out;
    std::string why;
    if (!sound::renderSound(g, p, out, &why)) AVER_ERROR("render failed: {}", why);
    return out;
}

static u32 risingCrossings(const std::vector<f32>& s) {
    u32 n = 0;
    for (usize i = 1; i < s.size(); ++i) if (s[i - 1] <= 0.0f && s[i] > 0.0f) ++n;
    return n;
}
static f32 rmsTail(const std::vector<f32>& s) {
    f64 acc = 0.0;
    const usize from = s.size() / 2;
    for (usize i = from; i < s.size(); ++i) acc += static_cast<f64>(s[i]) * s[i];
    return static_cast<f32>(std::sqrt(acc / static_cast<f64>(s.size() - from)));
}
static bool allEqual(const std::vector<f32>& s, f32 v, usize from = 0, usize to = ~usize(0), f32 eps = 1.0e-6f) {
    if (to > s.size()) to = s.size();
    for (usize i = from; i < to; ++i) if (std::fabs(s[i] - v) > eps) return false;
    return true;
}
static f32 lo(const std::vector<f32>& s) { f32 m = 9; for (f32 v : s) m = v < m ? v : m; return m; }
static f32 hi(const std::vector<f32>& s) { f32 m = -9; for (f32 v : s) m = v > m ? v : m; return m; }

static void put32(std::vector<u8>& b, u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (i * 8))); }
static void putf(std::vector<u8>& b, f32 f) { u32 x; std::memcpy(&x, &f, 4); put32(b, x); }

static fmt::AvrChunk* chunkOf(fmt::Avr1File& f, const char (&id)[5]) {
    for (fmt::AvrChunk& c : f.chunks) if (c.id == fmt::avrFourCC(id)) return &c;
    return nullptr;
}

int main() {
    AVER_INFO("SoundNodesTest");
    std::string why;

    AVER_INFO("the format and the evaluator agree on how many inputs every kind has");
    for (u32 k = 0; k <= fmt::kOcSoundLastKind; ++k)
        if (fmt::ocSoundInputCount(static_cast<K>(k)) != audio::soundNodeInputCount(static_cast<audio::SoundNodeKind>(k)))
            check(false, "kind " + std::to_string(k) + " has the same input count in both places");
    check(true, "input counts match for kinds 0.." + std::to_string(fmt::kOcSoundLastKind));

    AVER_INFO("oscillators and filters take a frequency / cutoff input; unconnected they are exactly what they were");
    {
        auto a = render(graph({mk(K::Const, 100), mk(K::Sine, 0)}, {{0, 1, 0}}, 1.0f));
        const u32 x = risingCrossings(a);
        check(x >= 99 && x <= 101, "a Const(100) into a Sine(0)'s frequency input makes 100 Hz (" + std::to_string(x) + " crossings)");
        auto b = render(graph({mk(K::Const, 60), mk(K::Sine, 40)}, {{0, 1, 0}}, 1.0f));
        const u32 y = risingCrossings(b);
        check(y >= 99 && y <= 101, "the input is ADDED to the node's own frequency: 40 + 60 = 100 Hz (" + std::to_string(y) + ")");
        auto lp1 = render(graph({mk(K::Noise), mk(K::Const, 1000), mk(K::LowPass, 0)}, {{0, 2, 0}, {1, 2, 1}}));
        auto lp2 = render(graph({mk(K::Noise), mk(K::LowPass, 1000)}, {{0, 1, 0}}));
        check(lp1 == lp2, "LowPass(0) with a Const(1000) cutoff input is bit-identical to LowPass(1000)");
        auto neg = render(graph({mk(K::Const, -100), mk(K::Saw, 0)}, {{0, 1, 0}}, 0.2f));
        check(hi(neg) > 0.9f && lo(neg) >= -1.0f, "a frequency pushed below zero wraps the phase instead of drifting out of range");
    }

    AVER_INFO("Parameter holds its default, takes a value from the render's inputs, and clamps");
    {
        auto d = render(graph({mk(K::Parameter, 0.25f, 0, 0, 0, "x")}, {}));
        check(allEqual(d, 0.25f), "with no live value a Parameter is its default");
        sound::RenderParams p; p.inputs = {{"x", 0.75f}, {"unused", 1.0f}};
        auto s = render(graph({mk(K::Parameter, 0.25f, 0, 0, 0, "x")}, {}), p);
        check(allEqual(s, 0.75f), "a value set before the first sample is used from the first sample, with no ramp from the default");
        p.inputs = {{"x", 5.0f}};
        auto c = render(graph({mk(K::Parameter, 0.5f, 0, 0, 1.0f, "x")}, {}), p);
        check(allEqual(c, 1.0f), "min/max clamp the live value");
        auto u = render(graph({mk(K::Parameter, 0.3f)}, {}), p);
        check(allEqual(u, 0.3f), "an unnamed Parameter is a constant (nothing can address it)");
        auto prog = audio::SoundProgram::compile(sound::describeSound(graph({mk(K::Parameter, 1, 0, 0, 0, "x"), mk(K::Parameter, 2, 0, 0, 0, "y"),
                                                                              mk(K::Parameter, 3, 0, 0, 0, "x")}, {})), &why);
        check(prog && prog->paramCount() == 2 && prog->findParam("x") == 0 && prog->findParam("y") == 1 && prog->findParam("z") == -1,
              "two nodes with one name share one input id");
    }

    AVER_INFO("LFO shapes, depth, offset and a rate input");
    {
        auto sine = render(graph({mk(K::Lfo, 5, 1, 0, 0)}, {}, 1.0f));
        const u32 x = risingCrossings(sine);
        // The f32 phase accumulator runs a hair fast, so a sixth crossing can land inside the last few samples (the Sine test allows +-1 too).
        check(x >= 5 && x <= 6, "a 5 Hz sine LFO crosses zero 5 times a second (" + std::to_string(x) + ")");
        check(hi(sine) > 0.99f && lo(sine) < -0.99f, "and swings the full depth");
        auto sq = render(graph({mk(K::Lfo, 5, 0.5f, 2, 0)}, {}, 1.0f));
        bool onlyTwo = true;
        for (f32 v : sq) if (std::fabs(std::fabs(v) - 0.5f) > 1e-6f) onlyTwo = false;
        check(onlyTwo, "a square LFO is only ever +-depth");
        auto saw = render(graph({mk(K::Lfo, 2, 1, 3, 0)}, {}, 1.0f));
        check(lo(saw) < -0.99f && hi(saw) > 0.99f, "a saw LFO sweeps the range");
        auto tri = render(graph({mk(K::Lfo, 4, 1, 1, 0)}, {}, 1.0f));
        check(std::fabs(tri[0]) < 1e-6f && std::fabs(tri[3000] - 1.0f) < 1e-3f && std::fabs(tri[9000] + 1.0f) < 1e-3f,
              "a triangle LFO starts at 0, peaks a quarter period in and bottoms out at three quarters");
        auto off = render(graph({mk(K::Lfo, 3, 0.25f, 0, 0.5f)}, {}, 1.0f));
        check(lo(off) > 0.249f && hi(off) < 0.751f && hi(off) > 0.74f, "offset moves the centre: 0.5 +- 0.25");
        auto rated = render(graph({mk(K::Const, 5), mk(K::Lfo, 0, 1, 0, 0)}, {{0, 1, 0}}, 1.0f));
        const u32 r = risingCrossings(rated);
        check(r >= 5 && r <= 6, "a Const(5) into the rate input of a Lfo(0) is 5 Hz (" + std::to_string(r) + ")");
    }

    AVER_INFO("high pass and band pass filter what they say they filter");
    {
        for (const f32 tone : {200.0f, 8000.0f}) {
            auto hp = render(graph({mk(K::Sine, tone), mk(K::HighPass, 2000)}, {{0, 1, 0}}, 0.25f));
            const f32 level = rmsTail(hp);
            if (tone < 2000.0f) check(level < 0.05f, "200 Hz is removed by a 2 kHz high pass (rms " + std::to_string(level) + ")");
            else                check(level > 0.6f, "8 kHz passes it (rms " + std::to_string(level) + ")");
        }
        for (const f32 tone : {100.0f, 1000.0f, 10000.0f}) {
            auto bp = render(graph({mk(K::Sine, tone), mk(K::BandPass, 1000, 4)}, {{0, 1, 0}}, 0.25f));
            const f32 level = rmsTail(bp);
            if (tone == 1000.0f) check(level > 0.6f, "a 1 kHz band pass passes 1 kHz at about unity (rms " + std::to_string(level) + ")");
            else                 check(level < 0.1f, std::to_string(static_cast<int>(tone)) + " Hz is well outside it (rms " + std::to_string(level) + ")");
        }
        auto h1 = render(graph({mk(K::Noise), mk(K::Const, 2000), mk(K::HighPass, 0)}, {{0, 2, 0}, {1, 2, 1}}));
        auto h2 = render(graph({mk(K::Noise), mk(K::HighPass, 2000)}, {{0, 1, 0}}));
        check(h1 == h2, "a cutoff input adds to the cutoff, exactly");
        auto pass = render(graph({mk(K::Noise), mk(K::HighPass, 0)}, {{0, 1, 0}}));
        auto raw = render(graph({mk(K::Noise)}, {}));
        check(pass == raw, "a cutoff of 0 passes a high pass's input through untouched");
    }

    AVER_INFO("Random: per trigger, seeded, in range");
    {
        const auto g = graph({mk(K::Random, -0.9f, 0.9f, 0)}, {}, 0.5f);
        sound::RenderParams a; a.seed = 11; sound::RenderParams b; b.seed = 12;
        auto r1 = render(g, a), r2 = render(g, a), r3 = render(g, b);
        check(r1 == r2, "the same seed gives the same value");
        check(r1 != r3, "a different seed gives a different value (per-play variation)");
        check(!r1.empty() && allEqual(r1, r1[0], 0, ~usize(0), 0.0f), "the value is held for the whole play");
        check(r1[0] >= -0.9f && r1[0] < 0.9f && r3[0] >= -0.9f && r3[0] < 0.9f, "and lies in [min, max)");
        bool varied = false;
        for (u32 s = 1; s <= 30; ++s) { sound::RenderParams q; q.seed = s; if (std::fabs(render(g, q)[0] - r1[0]) > 0.01f) varied = true; }
        check(varied, "across seeds it takes many different values");
        // Held at 100 Hz: a new value every 480 frames, scaled into the [-1,1] output so it is observable.
        auto held = render(graph({mk(K::Random, -0.9f, 0.9f, 100)}, {}, 1.0f), a);
        u32 changes = 0;
        for (usize i = 1; i < held.size(); ++i) if (held[i] != held[i - 1]) ++changes;
        check(changes >= 95 && changes <= 101, "redrawn at 100 Hz: about 100 changes in a second (" + std::to_string(changes) + ")");
        check(lo(held) >= -0.9f - 1e-6f && hi(held) < 0.9f, "every draw is inside [min, max)");
    }

    AVER_INFO("Crossfade by position");
    {
        auto cross = [](f32 pos, f32 curve, bool third = false, f32 posInput = 0.0f) {
            std::vector<fmt::OcSoundNode> n = {mk(K::Const, 0.8f), mk(K::Const, 0.2f), mk(K::Const, -0.4f), mk(K::Const, posInput),
                                               mk(K::Crossfade, pos, third ? 3.0f : 2.0f, curve)};
            std::vector<fmt::OcSoundLink> l = {{0, 4, 1}, {1, 4, 2}, {3, 4, 0}};
            if (third) l.push_back({2, 4, 3});
            return render(graph(n, l, 0.01f))[100];
        };
        check(std::fabs(cross(0, 0) - 0.8f) < 1e-6f, "position 0 is the first input exactly");
        check(std::fabs(cross(1, 0) - 0.2f) < 1e-6f, "position 1 is the second exactly");
        check(std::fabs(cross(0.5f, 0) - 0.70710678f) < 1e-4f, "halfway, equal power: (0.8 + 0.2) * sqrt(0.5)");
        check(std::fabs(cross(0.5f, 1) - 0.5f) < 1e-6f, "halfway, linear: 0.5");
        check(std::fabs(cross(2, 0) - 0.2f) < 1e-6f, "a position past the last input holds the last");
        check(std::fabs(cross(1.5f, 0, true) - (0.2f + -0.4f) * 0.70710678f) < 1e-4f, "three inputs: 1.5 is halfway between the second and the third");
        check(std::fabs(cross(0.5f, 0, false, 0.5f) - 0.2f) < 1e-6f, "the position input adds to the position: 0.5 + 0.5 = 1");
    }

    AVER_INFO("Clamp and MapRange");
    {
        auto one = [](std::vector<fmt::OcSoundNode> n) { return render(graph(std::move(n), {{0, 1, 0}}, 0.01f))[10]; };
        check(std::fabs(one({mk(K::Const, 5), mk(K::Clamp, -1, 0.5f)}) - 0.5f) < 1e-6f, "Clamp holds the top");
        check(std::fabs(one({mk(K::Const, -3), mk(K::Clamp, -0.25f, 2)}) + 0.25f) < 1e-6f, "and the bottom");
        check(std::fabs(one({mk(K::Const, 5), mk(K::MapRange, 0, 10, -1, 1)})) < 1e-6f, "MapRange 0..10 -> -1..1 maps 5 to 0");
        check(std::fabs(one({mk(K::Const, 7.5f), mk(K::MapRange, 0, 10, -1, 1)}) - 0.5f) < 1e-6f, "and 7.5 to 0.5");
        check(std::fabs(one({mk(K::Const, 100), mk(K::MapRange, 0, 10, -1, 1)}) - 1.0f) < 1e-6f, "an input above the range holds the top");
        check(std::fabs(one({mk(K::Const, -4), mk(K::MapRange, 0, 10, -1, 1)}) + 1.0f) < 1e-6f, "and below holds the bottom");
        check(std::fabs(one({mk(K::Const, 2.5f), mk(K::MapRange, 0, 10, 1, 0)}) - 0.75f) < 1e-6f, "an output range may run backwards");
        check(std::fabs(one({mk(K::Const, 2.5f), mk(K::MapRange, 10, 0, 0, 1)}) - 0.75f) < 1e-6f, "and so may the input range");
    }

    AVER_INFO("Delay: a step arrives late, feeds back, passes through at mix 0, and its length is bounded");
    {
        auto step = render(graph({mk(K::Const, 0.4f), mk(K::Delay, 0.01f, 0, 1, 0.05f)}, {{0, 1, 0}}, 0.1f));
        check(allEqual(step, 0, 0, 478, 1e-4f) && allEqual(step, 0.4f, 482, ~usize(0), 1e-4f), "a 10 ms delay of a step: silent for 480 frames, then 0.4");
        auto fb = render(graph({mk(K::Const, 0.4f), mk(K::Delay, 0.01f, 0.5f, 1, 0.05f)}, {{0, 1, 0}}, 0.1f));
        check(allEqual(fb, 0.4f, 484, 956, 1e-4f) && allEqual(fb, 0.6f, 964, 1436, 1e-4f) && allEqual(fb, 0.7f, 1444, 1916, 1e-4f),
              "with 0.5 feedback the echoes add up 0.4, 0.6, 0.7 ...");
        auto dry = render(graph({mk(K::Const, 0.4f), mk(K::Delay, 0.01f, 0.5f, 0, 0.05f)}, {{0, 1, 0}}, 0.1f));
        check(allEqual(dry, 0.4f), "at mix 0 the delay is transparent");
        auto timed = render(graph({mk(K::Const, 0.4f), mk(K::Const, 0.005f), mk(K::Delay, 0.005f, 0, 1, 0.05f)}, {{0, 2, 0}, {1, 2, 1}}, 0.1f));
        check(allEqual(timed, 0, 0, 470) && allEqual(timed, 0.4f, 490, ~usize(0), 1e-4f), "the time input adds to the time: 5 ms + 5 ms is 10 ms");
        auto longd = render(graph({mk(K::Const, 0.4f), mk(K::Delay, 5.0f, 0, 1, 0.05f)}, {{0, 1, 0}}, 0.2f));
        check(allEqual(longd, 0, 0, 2399) && allEqual(longd, 0.4f, 2403), "a time past the pre-allocated length is clamped to it (50 ms here)");
    }

    AVER_INFO("EnvelopeFollower tracks the peak");
    {
        auto env = render(graph({mk(K::Sine, 1000), mk(K::Gain, 0.8f), mk(K::EnvelopeFollower, 0.001f, 0.05f)}, {{0, 1, 0}, {1, 2, 0}}, 0.3f));
        const f32 end = env.back();
        check(end > 0.72f && end <= 0.8f + 1e-4f, "a 0.8 sine settles at about 0.8 (" + std::to_string(end) + ")");
        auto silent = render(graph({mk(K::Const, 0), mk(K::EnvelopeFollower, 0.001f, 0.05f)}, {{0, 1, 0}}));
        check(allEqual(silent, 0), "and silence follows to silence");
    }

    AVER_INFO("WavePlayer plays an asset, at a pitch, looping or not");
    {
        audio::SoundData ramp;
        ramp.channels = 1; ramp.sampleRate = 48000;
        for (u32 i = 0; i < 2000; ++i) ramp.samples.push_back(static_cast<f32>(i) / 2000.0f);
        sound::RenderParams p;
        p.resolveWave = [&](const std::string& name) -> const audio::SoundData* { return name == "w" ? &ramp : nullptr; };
        auto plain = render(graph({mk(K::WavePlayer, 0, 0, 0, 0, "w")}, {}, 0.02f), p);
        check(plain.size() == 960 && std::fabs(plain[500] - 0.25f) < 1e-5f && std::fabs(plain[0]) < 1e-6f, "the sample plays back sample for sample");
        auto fast = render(graph({mk(K::Const, 12), mk(K::WavePlayer, 0, 0, 0, 0, "w")}, {{0, 1, 0}}, 0.02f), p);
        check(std::fabs(fast[400] - 0.4f) < 1e-3f, "a pitch input of +12 semitones plays twice as fast");
        auto ratio = render(graph({mk(K::WavePlayer, 0.5f, 0, 0, 0, "w")}, {}, 0.02f), p);
        check(std::fabs(ratio[800] - 0.2f) < 1e-3f, "a pitch ratio of 0.5 plays at half speed");
        auto offset = render(graph({mk(K::WavePlayer, 0, 0, 0.01f, 0, "w")}, {}, 0.02f), p);
        check(std::fabs(offset[0] - 0.24f) < 1e-4f, "a start offset skips into the sample");
        auto once = render(graph({mk(K::WavePlayer, 0, 0, 0, 0, "w")}, {}, 0.0625f), p);
        check(allEqual(once, 0, 2100), "past the end it falls silent");
        auto loop = render(graph({mk(K::WavePlayer, 0, 1, 0, 0, "w")}, {}, 0.0625f), p);
        check(std::fabs(loop[2500] - 0.25f) < 1e-4f, "looping wraps to the start");
        audio::SoundData slow = ramp; slow.sampleRate = 24000;
        sound::RenderParams ps; ps.resolveWave = [&](const std::string&) { return &slow; };
        auto half = render(graph({mk(K::WavePlayer, 0, 0, 0, 0, "w")}, {}, 0.02f), ps);
        check(std::fabs(half[800] - 0.2f) < 1e-3f, "a 24 kHz asset on a 48 kHz render plays at the right pitch");
        audio::SoundData st = ramp; st.channels = 2; st.samples.clear();
        for (u32 i = 0; i < 2000; ++i) { st.samples.push_back(0.2f); st.samples.push_back(0.6f); }
        sound::RenderParams pst; pst.resolveWave = [&](const std::string&) { return &st; };
        check(allEqual(render(graph({mk(K::WavePlayer, 0, 0, 0, 0, "w")}, {}, 0.02f), pst), 0.4f), "a stereo asset is summed to mono");
        auto missing = render(graph({mk(K::WavePlayer, 0, 0, 0, 0, "nothing")}, {}, 0.02f), p);
        check(allEqual(missing, 0), "an asset that does not resolve leaves the player silent, not broken");
        auto unnamed = render(graph({mk(K::WavePlayer)}, {}, 0.02f), p);
        check(allEqual(unnamed, 0), "and so does a player with no asset named");
    }

    AVER_INFO("a graph the evaluator cannot hold is refused with a reason");
    {
        // Call first, then build the message: argument order is unspecified (see SynthTest's frame-cap check).
        auto refused = [&](const std::vector<fmt::OcSoundNode>& nodes, const char* needle, const std::string& label) {
            why.clear();
            const bool r = !audio::SoundProgram::compile(sound::describeSound(graph(nodes, {})), &why);
            check(r && why.find(needle) != std::string::npos, label + ": " + why);
        };
        std::vector<fmt::OcSoundNode> many;
        for (u32 i = 0; i < 33; ++i) many.push_back(mk(K::Parameter, 0, 0, 0, 0, "p" + std::to_string(i)));
        refused(many, "Parameter names", "33 distinct Parameter names");
        std::vector<fmt::OcSoundNode> waves;
        for (u32 i = 0; i < 9; ++i) waves.push_back(mk(K::WavePlayer, 0, 0, 0, 0, "w" + std::to_string(i)));
        refused(waves, "WavePlayer", "9 distinct WavePlayer assets");
        std::vector<fmt::OcSoundNode> delays;
        for (u32 i = 0; i < 4; ++i) delays.push_back(mk(K::Delay, 0.1f, 0, 1, 10.0f));
        refused(delays, "Delay", "40 s of delay memory");
        std::vector<fmt::OcSoundNode> big;
        for (u32 i = 0; i < 257; ++i) big.push_back(mk(K::Const, 0));
        refused(big, "nodes", "257 nodes");
        sound::RenderParams rp;
        std::vector<f32> out;
        check(!sound::renderSound(graph(delays, {}), rp, out, &why), "and renderSound refuses it too rather than allocating it");
    }

    AVER_INFO("format version 2: round trip, versioning, old files, hostile input");
    {
        fmt::OcSoundData g = graph({mk(K::Parameter, 3000, 0.05f, 800, 6000, "rpm"), mk(K::MapRange, 800, 6000, 40, 300), mk(K::Sine, 0),
                                    mk(K::WavePlayer, 1, 1, 0, 0, "sounds/engine.ocaudio"), mk(K::Lfo, 4, 0.2f, 1, 0),
                                    mk(K::Crossfade, 0, 2, 0), mk(K::Delay, 0.2f, 0.4f, 0.3f, 1.0f), mk(K::EnvelopeFollower, 0.01f, 0.2f)},
                                   {{0, 1, 0}, {1, 2, 0}, {2, 5, 1}, {3, 5, 2}, {5, 6, 0}, {6, 7, 0}}, 2.0f);
        check(g.valid() && g.needsVersion2(), "a graph with new kinds is valid and needs version 2");
        std::vector<u8> bytes;
        check(fmt::writeOcSound(g, bytes, &why), "it writes: " + why);
        fmt::Avr1File f;
        check(fmt::parseAvr1(bytes.data(), bytes.size(), f, &why) && f.contentVersion == 2 && f.find(fmt::avrFourCC("SNAM")),
              "as content version 2 with a name chunk");
        fmt::OcSoundData back;
        check(fmt::parseOcSound(bytes.data(), bytes.size(), back, &why), "and parses: " + why);
        bool same = back.nodes.size() == g.nodes.size() && back.links.size() == g.links.size() && back.outputNode == g.outputNode && back.durationSec == g.durationSec;
        for (usize i = 0; same && i < g.nodes.size(); ++i) {
            same = back.nodes[i].kind == g.nodes[i].kind && back.nodes[i].name == g.nodes[i].name &&
                   std::memcmp(back.nodes[i].params, g.nodes[i].params, sizeof(g.nodes[i].params)) == 0;
        }
        check(same && back.nodes[0].name == "rpm" && back.nodes[3].name == "sounds/engine.ocaudio", "kinds, params, names and links all survive");
        std::vector<u8> again;
        fmt::writeOcSound(back, again, &why);
        check(again == bytes, "write(parse(write(x))) is byte-stable");

        // A link into an old kind's new input is a version-2 feature even with no new kind.
        fmt::OcSoundData onlyLink = graph({mk(K::Const, 100), mk(K::Sine, 0)}, {{0, 1, 0}});
        check(onlyLink.needsVersion2(), "a link into a Sine's frequency input needs version 2 even though both kinds are old");

        // A file exactly as the version-1 writer made it (three chunks, content version 1) still loads.
        {
            std::vector<u8> shdr, snod, slnk;
            put32(shdr, 1); putf(shdr, 0.75f);
            put32(snod, 2);
            put32(snod, 1 /*Saw*/); putf(snod, 220); putf(snod, 0); putf(snod, 0); putf(snod, 0);
            put32(snod, 6 /*LowPass*/); putf(snod, 900); putf(snod, 0); putf(snod, 0); putf(snod, 0);
            put32(slnk, 1); put32(slnk, 0); put32(slnk, 1); put32(slnk, 0);
            fmt::Avr1File old;
            old.subtype = fmt::avrFourCC("SND ");
            old.contentVersion = 1;
            old.flags = fmt::kAvrFlagCooked;
            old.add(fmt::avrFourCC("SHDR"), shdr, fmt::kAvrChunkRequired);
            old.add(fmt::avrFourCC("SNOD"), snod, fmt::kAvrChunkRequired);
            old.add(fmt::avrFourCC("SLNK"), slnk);
            std::vector<u8> oldBytes;
            fmt::writeAvr1(old, oldBytes, &why);
            fmt::OcSoundData loaded;
            check(fmt::parseOcSound(oldBytes.data(), oldBytes.size(), loaded, &why) && loaded.nodes.size() == 2 && loaded.nodes[1].params[0] == 900.0f &&
                  loaded.durationSec == 0.75f && loaded.links.size() == 1 && loaded.nodes[0].name.empty(),
                  "a version-1 file built chunk by chunk, as the old writer did, loads unchanged: " + why);
            std::vector<u8> rewritten;
            fmt::writeOcSound(loaded, rewritten, &why);
            check(rewritten == oldBytes, "and writing it back gives the same bytes");
        }

        // A file from a newer build is refused with a reason, not misread.
        {
            fmt::Avr1File nf;
            fmt::parseAvr1(bytes.data(), bytes.size(), nf, &why);
            nf.contentVersion = 3;
            std::vector<u8> nb;
            fmt::writeAvr1(nf, nb, &why);
            fmt::OcSoundData o;
            why.clear();
            check(!fmt::parseOcSound(nb.data(), nb.size(), o, &why) && why.find("newer") != std::string::npos, "content version 3 is refused as newer: " + why);
        }

        // Hostile SNAM chunks and node tables.
        auto tamper = [&](const char* label, auto&& edit) {
            fmt::Avr1File t;
            fmt::parseAvr1(bytes.data(), bytes.size(), t, &why);
            edit(t);
            std::vector<u8> tb;
            fmt::writeAvr1(t, tb, &why);
            fmt::OcSoundData o;
            why.clear();
            check(!fmt::parseOcSound(tb.data(), tb.size(), o, &why) && !why.empty(), std::string(label) + ": " + why);
        };
        tamper("a name count the chunk cannot hold", [](fmt::Avr1File& t) { auto* c = chunkOf(t, "SNAM"); c->data.clear(); put32(c->data, 0x7FFFFFFF); });
        tamper("a name for a node that does not exist", [](fmt::Avr1File& t) { auto* c = chunkOf(t, "SNAM"); c->data.clear(); put32(c->data, 1); put32(c->data, 99); put32(c->data, 1); c->data.push_back('x'); });
        tamper("a name longer than the chunk", [](fmt::Avr1File& t) { auto* c = chunkOf(t, "SNAM"); c->data.clear(); put32(c->data, 1); put32(c->data, 0); put32(c->data, 100); c->data.push_back('x'); });
        tamper("a name over the limit", [](fmt::Avr1File& t) { auto* c = chunkOf(t, "SNAM"); c->data.clear(); put32(c->data, 1); put32(c->data, 0); put32(c->data, 600); c->data.insert(c->data.end(), 600, 'x'); });
        tamper("the same node named twice", [](fmt::Avr1File& t) {
            auto* c = chunkOf(t, "SNAM"); c->data.clear();
            put32(c->data, 2); put32(c->data, 0); put32(c->data, 1); c->data.push_back('a'); put32(c->data, 0); put32(c->data, 1); c->data.push_back('b'); });
        tamper("an unknown node kind", [](fmt::Avr1File& t) { auto* c = chunkOf(t, "SNOD"); u32 k = 21; std::memcpy(c->data.data() + 4, &k, 4); });
        tamper("a truncated name chunk", [](fmt::Avr1File& t) { auto* c = chunkOf(t, "SNAM"); c->data.resize(6); });
        // And every prefix of a good file fails cleanly.
        bool prefixesOk = true;
        for (usize n = 0; n < bytes.size(); ++n) {
            fmt::OcSoundData o;
            if (fmt::parseOcSound(bytes.data(), n, o)) prefixesOk = false;
        }
        check(prefixesOk, "every truncation of a version-2 file is refused");

        // A name over the limit does not write.
        fmt::OcSoundData longName = g;
        longName.nodes[0].name.assign(600, 'x');
        std::vector<u8> ignored;
        check(!longName.valid() && !fmt::writeOcSound(longName, ignored, &why), "a name over 512 bytes is not written");
    }

    AVER_INFO(g_failures ? "SoundNodesTest: {} FAILURES" : "SoundNodesTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
