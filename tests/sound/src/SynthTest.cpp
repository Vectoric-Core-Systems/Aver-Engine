// Aver Sound: the .ocsnd format and the synthesiser, checked against numbers this file computes
// for itself -- a 440 Hz sine really crossing zero 880 times a second, an envelope really reaching
// its sustain level, a low pass really removing high frequencies.
//
// NO DEVICE, NO MIXER, NO SCENE. That is the whole point of Aver.Sound being pure, and the same
// payoff NavTest.cpp and BtTest.cpp already take: audio correctness is arithmetic, and arithmetic
// can be asserted. Nothing here needs a sound card, so this runs in the ordinary suite.
#include "aver/sound/Synth.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcSound.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// How many times the signal crosses zero going upward -- a frequency measurement that needs no FFT.
static u32 risingZeroCrossings(const std::vector<f32>& s) {
    u32 n = 0;
    for (usize i = 1; i < s.size(); ++i) if (s[i - 1] <= 0.0f && s[i] > 0.0f) ++n;
    return n;
}

static f32 peak(const std::vector<f32>& s) {
    f32 p = 0.0f;
    for (const f32 v : s) p = std::fabs(v) > p ? std::fabs(v) : p;
    return p;
}

static f32 rms(const std::vector<f32>& s, usize from, usize to) {
    if (to <= from || to > s.size()) return 0.0f;
    f64 acc = 0.0;
    for (usize i = from; i < to; ++i) acc += static_cast<f64>(s[i]) * s[i];
    return static_cast<f32>(std::sqrt(acc / static_cast<f64>(to - from)));
}

// A one-node graph emitting `kind` at `freq`.
static fmt::OcSoundData oscillator(fmt::OcSoundNodeKind kind, f32 freq, f32 seconds = 1.0f) {
    fmt::OcSoundData g;
    fmt::OcSoundNode n;
    n.kind = kind;
    n.params[0] = freq;
    g.nodes.push_back(n);
    g.outputNode = 0;
    g.durationSec = seconds;
    return g;
}

int main() {
    AVER_INFO("SynthTest");
    sound::RenderParams rp;
    rp.sampleRate = 48000;
    std::string why;

    AVER_INFO("a sine really is the frequency it says it is");
    {
        const fmt::OcSoundData g = oscillator(fmt::OcSoundNodeKind::Sine, 440.0f, 1.0f);
        std::vector<f32> pcm;
        check(sound::renderSound(g, rp, pcm, &why), "it renders: " + why);
        check(pcm.size() == 48000, "one second at 48 kHz is 48000 frames");
        // 440 Hz over one second = 440 rising zero crossings, +-1 for where the render happens to
        // start and stop.
        const u32 x = risingZeroCrossings(pcm);
        check(x >= 439 && x <= 441, "440 rising zero crossings in one second (got " + std::to_string(x) + ")");
        check(peak(pcm) > 0.98f && peak(pcm) <= 1.0f, "and it reaches full scale without clipping");
    }

    AVER_INFO("the other oscillators are distinguishable, not three names for one waveform");
    {
        std::vector<f32> saw, square, noise;
        fmt::OcSoundData gs = oscillator(fmt::OcSoundNodeKind::Saw, 100.0f, 0.5f);
        fmt::OcSoundData gq = oscillator(fmt::OcSoundNodeKind::Square, 100.0f, 0.5f);
        fmt::OcSoundData gn = oscillator(fmt::OcSoundNodeKind::Noise, 0.0f, 0.5f);
        check(sound::renderSound(gs, rp, saw, &why), "saw renders: " + why);
        check(sound::renderSound(gq, rp, square, &why), "square renders: " + why);
        check(sound::renderSound(gn, rp, noise, &why), "noise renders: " + why);

        check(risingZeroCrossings(saw) >= 49 && risingZeroCrossings(saw) <= 51,
              "the saw is 100 Hz too");
        // A square is +-1 and nothing between; a saw sweeps every value in the range. Counting how
        // many samples sit near zero separates them without any spectral maths.
        u32 sawMid = 0, sqMid = 0;
        for (const f32 v : saw)    if (std::fabs(v) < 0.5f) ++sawMid;
        for (const f32 v : square) if (std::fabs(v) < 0.5f) ++sqMid;
        check(sqMid == 0, "a square is only ever +-1 -- no sample sits between");
        check(sawMid > saw.size() / 3, "a saw sweeps the whole range, so most samples do not");
        check(risingZeroCrossings(noise) > 1000, "noise crosses zero constantly, unlike a tone");
    }

    AVER_INFO("the same seed renders identical audio; a different seed does not");
    {
        const fmt::OcSoundData g = oscillator(fmt::OcSoundNodeKind::Noise, 0.0f, 0.2f);
        std::vector<f32> a, b, c;
        sound::RenderParams p1 = rp; p1.seed = 7;
        sound::RenderParams p2 = rp; p2.seed = 7;
        sound::RenderParams p3 = rp; p3.seed = 8;
        sound::renderSound(g, p1, a, &why);
        sound::renderSound(g, p2, b, &why);
        sound::renderSound(g, p3, c, &why);
        check(a == b, "SEED 7 TWICE IS BIT-IDENTICAL -- without this a .ocsnd is not an asset");
        check(a != c, "and seed 8 differs, which is what makes per-play variation possible at all");
    }

    AVER_INFO("an ADSR envelope reaches its sustain level and falls to silence");
    {
        // Const(1) -> Adsr(a=0.1, d=0.1, s=0.5, r=0.2) over 1 second.
        fmt::OcSoundData g;
        fmt::OcSoundNode src; src.kind = fmt::OcSoundNodeKind::Const; src.params[0] = 1.0f;
        g.nodes.push_back(src);
        fmt::OcSoundNode env; env.kind = fmt::OcSoundNodeKind::Adsr;
        env.params[0] = 0.1f; env.params[1] = 0.1f; env.params[2] = 0.5f; env.params[3] = 0.2f;
        g.nodes.push_back(env);
        g.links.push_back({0, 1, 0});
        g.outputNode = 1;
        g.durationSec = 1.0f;

        std::vector<f32> pcm;
        check(sound::renderSound(g, rp, pcm, &why), "it renders: " + why);
        check(pcm.size() == 48000, "one second of envelope");
        check(std::fabs(pcm[0]) < 0.02f, "it starts at silence");
        // Peak of the attack, at t = 0.1s.
        check(std::fabs(pcm[4800] - 1.0f) < 0.02f, "reaches 1.0 at the end of the attack");
        // Sustain, comfortably after decay and before release.
        check(std::fabs(pcm[24000] - 0.5f) < 0.02f, "holds the 0.5 sustain level in the middle");
        check(std::fabs(pcm[47990]) < 0.02f, "and has fallen to silence by the end of the release");
    }

    AVER_INFO("a low pass actually removes high frequencies and leaves low ones");
    {
        // The SAME cutoff against two tones: one well below it, one well above.
        for (const f32 toneHz : {200.0f, 8000.0f}) {
            fmt::OcSoundData g;
            fmt::OcSoundNode osc; osc.kind = fmt::OcSoundNodeKind::Sine; osc.params[0] = toneHz;
            g.nodes.push_back(osc);
            fmt::OcSoundNode lp; lp.kind = fmt::OcSoundNodeKind::LowPass; lp.params[0] = 1000.0f;
            g.nodes.push_back(lp);
            g.links.push_back({0, 1, 0});
            g.outputNode = 1;
            g.durationSec = 0.25f;

            std::vector<f32> pcm;
            sound::renderSound(g, rp, pcm, &why);
            // Measured after the filter has settled, so the initial ramp is not part of the answer.
            const f32 level = rms(pcm, pcm.size() / 2, pcm.size());
            if (toneHz < 1000.0f)
                check(level > 0.5f, "200 Hz passes a 1 kHz low pass largely intact (rms " +
                                    std::to_string(level) + ")");
            else
                check(level < 0.15f, "8 kHz is strongly attenuated by the same filter (rms " +
                                     std::to_string(level) + ")");
        }
    }

    AVER_INFO("one source feeding two consumers is why this is a DAG and not a tree");
    {
        // Sine -> Gain(0.5), and the SAME Sine -> Multiply, with the Gain also into Multiply.
        // A tree could not express the shared source without duplicating the oscillator, which
        // would give it its own phase and a different sound.
        fmt::OcSoundData g;
        fmt::OcSoundNode osc; osc.kind = fmt::OcSoundNodeKind::Sine; osc.params[0] = 440.0f;
        g.nodes.push_back(osc);                                    // 0
        fmt::OcSoundNode gain; gain.kind = fmt::OcSoundNodeKind::Gain; gain.params[0] = 0.5f;
        g.nodes.push_back(gain);                                   // 1
        fmt::OcSoundNode mul; mul.kind = fmt::OcSoundNodeKind::Multiply;
        g.nodes.push_back(mul);                                    // 2
        g.links.push_back({0, 1, 0});   // sine -> gain
        g.links.push_back({0, 2, 0});   // sine -> multiply in0   (the SAME source, twice)
        g.links.push_back({1, 2, 1});   // gain -> multiply in1
        g.outputNode = 2;
        g.durationSec = 0.1f;
        check(g.valid(), "a shared source is a valid graph");

        std::vector<f32> pcm;
        check(sound::renderSound(g, rp, pcm, &why), "it renders: " + why);
        // sin * (sin*0.5) = 0.5*sin^2, which is never negative and peaks at 0.5.
        f32 lo = 1.0f, hi = -1.0f;
        for (const f32 v : pcm) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
        check(lo >= -0.001f, "0.5*sin^2 never goes negative, proving both inputs saw the SAME phase");
        check(hi > 0.49f && hi < 0.51f, "and peaks at 0.5 (got " + std::to_string(hi) + ")");
    }

    AVER_INFO("the format round-trips, and refuses a graph that could not be evaluated");
    {
        fmt::OcSoundData g;
        fmt::OcSoundNode osc; osc.kind = fmt::OcSoundNodeKind::Saw; osc.params[0] = 220.0f;
        g.nodes.push_back(osc);
        fmt::OcSoundNode lp; lp.kind = fmt::OcSoundNodeKind::LowPass; lp.params[0] = 900.0f;
        g.nodes.push_back(lp);
        g.links.push_back({0, 1, 0});
        g.outputNode = 1;
        g.durationSec = 0.75f;

        std::vector<u8> bytes;
        check(fmt::writeOcSound(g, bytes, &why), "it writes: " + why);
        fmt::OcSoundData back;
        check(fmt::parseOcSound(bytes.data(), bytes.size(), back, &why), "and parses: " + why);
        check(back.nodes.size() == 2 && back.links.size() == 1, "the same node and link counts");
        check(back.nodes[1].params[0] == 900.0f, "a float param survived exactly");
        check(back.outputNode == 1 && back.durationSec == 0.75f, "and the header survived");

        // A BACKWARD link is a cycle waiting to happen, and the format refuses to hold one.
        fmt::OcSoundData bad = g;
        bad.links.push_back({1, 0, 0});
        check(!bad.valid(), "a link from a HIGHER index to a lower one is refused");
        std::vector<u8> ignored;
        check(!fmt::writeOcSound(bad, ignored, &why), "and writing one fails rather than producing it");

        // An input a kind does not have.
        fmt::OcSoundData bad2 = g;
        bad2.links.push_back({0, 1, 1});   // LowPass has ONE input
        check(!bad2.valid(), "a link into an input the target does not have is refused too");
    }

    AVER_INFO("an optimistic duration is bounded rather than allocating a gigabyte");
    {
        fmt::OcSoundData g = oscillator(fmt::OcSoundNodeKind::Sine, 440.0f, 3600.0f);
        std::vector<f32> pcm;
        // CALL FIRST, THEN BUILD THE MESSAGE. `check(fn(&why), "..." + why)` reads `why` and calls
        // `fn` as two arguments to the same call, and C++ does not order them -- MSVC evaluates the
        // string first, so the message shows the PREVIOUS block's reason. That is exactly what
        // happened here on the first run: this line reported writeOcSound's "refusing to write an
        // invalid graph" for a render that had actually failed on the frame cap. The assertion was
        // right and its explanation was a lie, which is the more dangerous half.
        why.clear();
        const bool refused = !sound::renderSound(g, rp, pcm, &why);
        check(refused, "an hour-long render is refused: " + why);
        check(why.find("exceeds the cap") != std::string::npos,
              "AND FOR THE RIGHT REASON -- the frame cap, not some other validity failure");
        check(pcm.empty(), "and nothing was allocated for it");
    }

    AVER_INFO(g_failures ? "SynthTest: {} FAILURES" : "SynthTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
