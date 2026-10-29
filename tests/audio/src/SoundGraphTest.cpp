// Real-time sound graphs (docs/AUDIO.md section 13), with no sound card: a .ocsnd evaluated by the mixer's audio-thread path and checked against
// the offline render of the same graph, live inputs that move a running voice, the note-off, the voice pool, and the real-time rule itself
// (nothing in mix() allocates -- counted with a replacement global operator new).
//
// Everything renders into buffers this file owns; no device is opened.
#include "aver/audio/Mixer.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcSound.hpp"
#include "aver/sound/Synth.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

using namespace aver;
using K = fmt::OcSoundNodeKind;

// ---- the allocation hook: every operator new made while a window is open is counted -----------------------------------------------------------
static std::atomic<bool> g_counting{false};
static std::atomic<u64>  g_allocs{0};
void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static fmt::OcSoundNode mk(K k, f32 p0 = 0, f32 p1 = 0, f32 p2 = 0, f32 p3 = 0, std::string name = {}) {
    fmt::OcSoundNode n; n.kind = k; n.params[0] = p0; n.params[1] = p1; n.params[2] = p2; n.params[3] = p3; n.name = std::move(name); return n;
}
static fmt::OcSoundData graph(std::vector<fmt::OcSoundNode> nodes, std::vector<fmt::OcSoundLink> links, f32 seconds) {
    fmt::OcSoundData g;
    g.nodes = std::move(nodes);
    g.links = std::move(links);
    g.outputNode = static_cast<u32>(g.nodes.size()) - 1;
    g.durationSec = seconds;
    return g;
}
static std::shared_ptr<const audio::SoundProgram> compile(const fmt::OcSoundData& g) {
    std::string why;
    auto p = sound::compileSound(g, &why);
    if (!p) AVER_ERROR("compile failed: {}", why);
    return p;
}

constexpr u32 kRate = 48000;

// Mixes `frames` frames in blocks of `block` and returns the LEFT channel (the mixer is stereo and these voices are flat, so L == R).
static std::vector<f32> mixLeft(audio::Mixer& m, u32 frames, u32 block = 480) {
    std::vector<f32> left;
    left.reserve(frames);
    std::vector<f32> buf(static_cast<usize>(block) * 2);
    u32 done = 0;
    while (done < frames) {
        const u32 n = std::min(block, frames - done);
        m.mix(buf.data(), n);
        for (u32 i = 0; i < n; ++i) left.push_back(buf[i * 2]);
        done += n;
    }
    return left;
}
static f32 maxDelta(const std::vector<f32>& s) {
    f32 d = 0.0f;
    for (usize i = 1; i < s.size(); ++i) d = std::max(d, std::fabs(s[i] - s[i - 1]));
    return d;
}
static f32 peakOf(const std::vector<f32>& s, usize from, usize to) {
    f32 p = 0.0f;
    for (usize i = from; i < to && i < s.size(); ++i) p = std::max(p, std::fabs(s[i]));
    return p;
}
static u32 crossings(const std::vector<f32>& s, usize from, usize to) {
    u32 n = 0;
    for (usize i = std::max<usize>(from, 1); i < to && i < s.size(); ++i) if (s[i - 1] <= 0.0f && s[i] > 0.0f) ++n;
    return n;
}
static bool finiteAll(const std::vector<f32>& s) { for (f32 v : s) if (!std::isfinite(v)) return false; return true; }

static audio::PlayDesc flat(bool looping) { audio::PlayDesc d; d.looping = looping; d.volume = 1.0f; d.bus = audio::Bus::Sfx; return d; }

// An engine-shaped graph using every new kind except WavePlayer, with no live input set: the reference for the equality tests.
static fmt::OcSoundData engineGraph() {
    return graph({mk(K::Parameter, 3000, 0.05f, 800, 6000, "rpm"),   // 0
                  mk(K::MapRange, 800, 6000, 40, 300),                // 1
                  mk(K::Sine, 0),                                     // 2  (rpm drives its frequency)
                  mk(K::Lfo, 6, 0.1f, 1, 0),                          // 3
                  mk(K::Noise),                                       // 4
                  mk(K::Gain, 0.05f),                                 // 5
                  mk(K::LowPass, 2000),                               // 6
                  mk(K::Mix),                                         // 7
                  mk(K::HighPass, 60, 0.7f),                          // 8
                  mk(K::Delay, 0.013f, 0.3f, 0.25f, 0.1f),            // 9
                  mk(K::EnvelopeFollower, 0.005f, 0.05f),             // 10
                  mk(K::Crossfade, 0.3f, 2, 0),                       // 11
                  mk(K::Adsr, 0.01f, 0.05f, 0.7f, 0.1f),              // 12
                  mk(K::Random, -0.1f, 0.1f, 0),                      // 13
                  mk(K::Mix),                                         // 14
                  mk(K::Gain, 0.8f)},                                 // 15
                 {{0, 1, 0}, {1, 2, 0}, {2, 7, 0}, {4, 5, 0}, {5, 6, 0}, {6, 7, 1}, {7, 8, 0}, {8, 9, 0}, {9, 10, 0}, {3, 11, 0}, {9, 11, 1},
                  {8, 11, 2}, {11, 12, 0}, {12, 14, 0}, {13, 14, 1}, {14, 15, 0}},
                 0.4f);
}

int main() {
    AVER_INFO("SoundGraphTest");
    std::string why;

    AVER_INFO("one evaluator: the audio-thread path and the offline render give identical samples for a graph with no live inputs");
    {
        const fmt::OcSoundData g = engineGraph();
        sound::RenderParams rp; rp.sampleRate = kRate; rp.seed = 77;
        std::vector<f32> ref;
        check(sound::renderSound(g, rp, ref, &why) && ref.size() == static_cast<usize>(0.4f * kRate), "the offline render produces the graph's duration (" + why + ")");
        check(peakOf(ref, 0, ref.size()) > 0.05f, "and it is not silent");

        // (a) the runner alone, in blocks of awkward sizes.
        audio::RunnerOptions ro; ro.sampleRate = kRate; ro.seed = 77; ro.finite = true;
        audio::SoundRunner run(compile(g), ro);
        std::vector<f32> viaRunner(ref.size());
        usize at = 0;
        const u32 sizes[] = {1, 7, 64, 333, 512, 1000, 3};
        for (u32 i = 0; at < viaRunner.size(); ++i) {
            const u32 n = static_cast<u32>(std::min<usize>(sizes[i % 7], viaRunner.size() - at));
            const u32 got = run.process(viaRunner.data() + at, n);
            at += got;
            if (got < n) break;
        }
        check(viaRunner == ref, "a SoundRunner driven in blocks of 1, 7, 64, 333, 512, 1000 and 3 frames is bit-identical to the one-shot render");
        check(run.finished(), "and it knows it is finished");

        // (b) through the mixer: a flat voice at volume 1 on the Sfx bus is the graph's samples, exactly.
        for (const u32 channels : {2u, 1u}) {
            audio::Mixer m;
            m.init(kRate, channels, 8, 16, 2);
            audio::PlayDesc d = flat(false);
            const audio::VoiceHandle v = m.playGraph(compile(g), nullptr, 0, d, 77);
            check(v != 0 && m.isGraphVoice(v), "playGraph returns a graph voice (" + std::to_string(channels) + " channel mixer)");
            std::vector<f32> out(ref.size() * channels);
            u32 done = 0;
            const u32 blocks[] = {480, 100, 777, 1};
            for (u32 i = 0; done < ref.size(); ++i) {
                const u32 n = std::min<u32>(blocks[i % 4], static_cast<u32>(ref.size()) - done);
                m.mix(out.data() + static_cast<usize>(done) * channels, n);
                done += n;
            }
            bool same = true;
            for (usize i = 0; i < ref.size(); ++i)
                for (u32 c = 0; c < channels; ++c) if (out[i * channels + c] != ref[i]) same = false;
            check(same, "the mixer's output is the offline render sample for sample (" + std::to_string(channels) + " channel(s), uneven block sizes)");
            std::vector<f32> tail(64 * channels);
            m.mix(tail.data(), 64);
            check(!m.playing(v), "and the voice has ended at the graph's duration");
        }

        // A different seed is a different sound; the default (0) varies per play.
        audio::Mixer m;
        m.init(kRate, 2, 8, 16, 2);
        const auto prog = compile(g);
        const audio::VoiceHandle v1 = m.playGraph(prog, nullptr, 0, flat(false), 1);
        const std::vector<f32> a = mixLeft(m, 4800);
        m.stopAll(); mixLeft(m, 960);
        const audio::VoiceHandle v2 = m.playGraph(prog, nullptr, 0, flat(false), 1);
        const std::vector<f32> b = mixLeft(m, 4800);
        m.stopAll(); mixLeft(m, 960);
        const audio::VoiceHandle v3 = m.playGraph(prog, nullptr, 0, flat(false), 2);
        const std::vector<f32> c = mixLeft(m, 4800);
        (void)v1; (void)v2; (void)v3;
        check(a == b, "the same seed twice is the same sound");
        check(a != c, "and another seed is not");
        m.stopAll(); mixLeft(m, 960);
        m.playGraph(prog, nullptr, 0, flat(false), 0);
        const std::vector<f32> d1 = mixLeft(m, 4800);
        m.stopAll(); mixLeft(m, 960);
        m.playGraph(prog, nullptr, 0, flat(false), 0);
        const std::vector<f32> d2 = mixLeft(m, 4800);
        check(d1 != d2, "seed 0 picks a different seed for every play");
    }

    AVER_INFO("a graph with a WavePlayer plays the sound the mixer holds, and the voice keeps it alive");
    {
        audio::SoundData ramp;
        ramp.channels = 1; ramp.sampleRate = kRate;
        for (u32 i = 0; i < 4000; ++i) ramp.samples.push_back(std::sin(static_cast<f32>(i) * 0.05f) * 0.5f);
        const fmt::OcSoundData g = graph({mk(K::WavePlayer, 1, 0, 0, 0, "clip"), mk(K::Gain, 0.8f)}, {{0, 1, 0}}, 0.05f);
        sound::RenderParams rp; rp.sampleRate = kRate;
        rp.resolveWave = [&](const std::string& n) -> const audio::SoundData* { return n == "clip" ? &ramp : nullptr; };
        std::vector<f32> ref;
        sound::renderSound(g, rp, ref, &why);

        audio::Mixer m;
        m.init(kRate, 2, 8, 16, 2);
        audio::SoundData copy = ramp;
        const audio::SoundHandle h = m.addSound(std::move(copy));
        const auto prog = compile(g);
        check(prog->waveCount() == 1 && prog->waveAsset(0) == "clip", "the program reports the asset it needs");
        const audio::VoiceHandle v = m.playGraph(prog, &h, 1, flat(false));
        check(v != 0, "it plays");
        m.removeSound(h);
        check(m.soundCount() == 1, "retiring the sound while the voice plays does not free it (the voice holds a reference)");
        const std::vector<f32> out = mixLeft(m, static_cast<u32>(ref.size()));
        check(out == ref, "and the voice plays it exactly as the offline render does");
        std::vector<f32> more = mixLeft(m, 480);
        m.collect();
        check(!m.playing(v) && m.soundCount() == 0, "once the voice ends the sound is released and collected");
        const audio::VoiceHandle v2 = m.playGraph(prog, &h, 1, flat(false));
        check(v2 != 0, "playing against a retired sound still plays (that slot is silent)");
        check(peakOf(mixLeft(m, 480), 0, 480) == 0.0f, "and the silent slot is silent");
    }

    AVER_INFO("a live input moves a running voice, and the move is a ramp, not a click");
    {
        // 437 Hz: 0.2 s is 87.4 turns, so the block boundary where the level changes lands mid-wave -- a hard switch there would be a big step.
        auto run = [&](f32 smoothing, f32* jumpOut) {
            audio::Mixer m;
            m.init(kRate, 2, 8, 16, 2);
            const auto prog = compile(graph({mk(K::Sine, 437), mk(K::Parameter, 0, smoothing, 0, 0, "level"), mk(K::Multiply)}, {{0, 2, 0}, {1, 2, 1}}, 1.0f));
            const audio::VoiceHandle v = m.playGraph(prog, nullptr, 0, flat(true));
            std::vector<f32> s = mixLeft(m, 9600);
            check(peakOf(s, 0, s.size()) == 0.0f, "at its default (0) the voice is silent");
            check(m.setGraphParam(v, "level", 1.0f), "setGraphParam finds the input by name");
            const std::vector<f32> after = mixLeft(m, 14400);
            s.insert(s.end(), after.begin(), after.end());
            *jumpOut = maxDelta(s);
            return s;
        };
        f32 smooth = 0, hard = 0;
        const std::vector<f32> s = run(0.0f, &smooth);          // 0 reads as the default 20 ms
        run(0.000001f, &hard);                                  // effectively no smoothing: the negative control
        check(finiteAll(s) && peakOf(s, 19000, s.size()) > 0.99f, "after the change the voice reaches full level");
        check(smooth < 0.075f, "with the default smoothing the largest sample-to-sample step is bounded (" + std::to_string(smooth) + " <= 0.075; the sine itself steps up to 0.057)");
        check(hard > 0.4f, "and with smoothing off the same change IS a click (" + std::to_string(hard) + "), so the bound above means something");
    }

    AVER_INFO("a live input can drive pitch: rpm 200 Hz to 800 Hz");
    {
        audio::Mixer m;
        m.init(kRate, 2, 8, 16, 2);
        const auto prog = compile(graph({mk(K::Parameter, 200, 0.02f, 0, 0, "hz"), mk(K::Sine, 0)}, {{0, 1, 0}}, 1.0f));
        const audio::VoiceHandle v = m.playGraph(prog, nullptr, 0, flat(true));
        const std::vector<f32> lowPart = mixLeft(m, 48000);
        const u32 id = static_cast<u32>(m.graphParamId(v, "hz"));
        check(id == 0 && m.graphParamId(v, "nope") == -1, "graphParamId finds a name and refuses an unknown one");
        check(m.setGraphParamId(v, id, 800.0f), "set by id");
        const std::vector<f32> sweep = mixLeft(m, 4800);
        const std::vector<f32> highPart = mixLeft(m, 48000);
        const u32 lo = crossings(lowPart, 0, lowPart.size()), hi = crossings(highPart, 0, highPart.size());
        check(lo >= 199 && lo <= 201, "one second at 200 Hz (" + std::to_string(lo) + " crossings)");
        check(hi >= 799 && hi <= 801, "one second at 800 Hz after the ramp (" + std::to_string(hi) + " crossings)");
        std::vector<f32> all = lowPart;
        all.insert(all.end(), sweep.begin(), sweep.end());
        all.insert(all.end(), highPart.begin(), highPart.end());
        check(maxDelta(all) < 0.13f, "and the pitch glide has no discontinuity: phase carries across (max step " + std::to_string(maxDelta(all)) + ")");
        check(!m.setGraphParam(v, "nope", 1.0f), "an unknown name is refused");
        check(!m.setGraphParam(0, "hz", 1.0f) && !m.setGraphParam(v + 12345, "hz", 1.0f), "and so is a stale or null handle");
        check(!m.setGraphParamId(v, 99, 1.0f), "and an id out of range");
        m.setGraphParam(v, "hz", std::nanf(""));
        const std::vector<f32> still = mixLeft(m, 4800);
        check(finiteAll(still), "a NaN is ignored rather than poisoning the voice");
    }

    AVER_INFO("a value set at play time is there from the first sample, with no ramp up from the default");
    {
        audio::Mixer m;
        m.init(kRate, 2, 8, 16, 2);
        const auto prog = compile(graph({mk(K::Sine, 437), mk(K::Parameter, 0, 0, 0, 0, "level"), mk(K::Multiply)}, {{0, 2, 0}, {1, 2, 1}}, 1.0f));
        const audio::Mixer::GraphParam init[] = {{"level", 1.0f}, {"unknown", 5.0f}};
        m.playGraph(prog, nullptr, 0, flat(true), 1, init, 2);
        const std::vector<f32> s = mixLeft(m, 480);
        check(peakOf(s, 0, 480) > 0.99f, "the first block is already at full level");
    }

    AVER_INFO("note-off: an ADSR releases from where it is, on stop rather than counting back from the end");
    {
        auto adsrGraph = [](f32 a, f32 d, f32 s, f32 r, f32 seconds) {
            return graph({mk(K::Sine, 300), mk(K::Adsr, a, d, s, r)}, {{0, 1, 0}}, seconds);
        };
        {
            audio::Mixer m;
            m.init(kRate, 2, 8, 16, 2);
            const audio::VoiceHandle v = m.playGraph(compile(adsrGraph(0.01f, 0.05f, 0.6f, 0.3f, 1.0f)), nullptr, 0, flat(true));
            const std::vector<f32> hold = mixLeft(m, 24000);
            const f32 sustain = peakOf(hold, 20000, 24000);
            check(sustain > 0.57f && sustain < 0.62f, "a held (looping) graph sits at its 0.6 sustain (" + std::to_string(sustain) + ")");
            m.graphNoteOff(v);
            const std::vector<f32> rel = mixLeft(m, 480 * 40);
            f32 prev = 1.0f;
            bool falling = true;
            for (usize w = 0; w + 480 <= rel.size() && w < 480 * 30; w += 480) {
                const f32 p = peakOf(rel, w, w + 480);
                if (p > prev + 0.02f) falling = false;
                prev = p;
            }
            check(falling, "after the note-off the level falls window by window");
            check(peakOf(rel, 480, 960) > 0.5f && peakOf(rel, 480 * 12, 480 * 13) < 0.4f && peakOf(rel, 480 * 12, 480 * 13) > 0.1f,
                  "at 10 ms it is still near sustain, at 120 ms it is part way down the 300 ms release");
            check(peakOf(rel, 480 * 31, rel.size()) < 0.01f, "and it is silent once the release has played");
            check(!m.playing(v), "the voice ends by itself after the longest release");
            m.collect();
            check(m.activeVoices() == 0, "leaving no voice behind");
        }
        {
            // The finite graph keeps the old behaviour: release counted back from the end.
            audio::Mixer m;
            m.init(kRate, 2, 8, 16, 2);
            const audio::VoiceHandle v = m.playGraph(compile(adsrGraph(0.01f, 0.05f, 0.6f, 0.3f, 2.0f)), nullptr, 0, flat(false));
            const std::vector<f32> a = mixLeft(m, 48000 + 48000 / 2);   // to 1.5 s
            check(peakOf(a, 70000, 72000) > 0.57f, "a finite graph is still at sustain at 1.5 s");
            const std::vector<f32> b = mixLeft(m, 16800);               // to 1.85 s: the release began at 1.7 s, 300 ms long
            const f32 mid = peakOf(b, 16800 - 480, 16800);
            check(std::fabs(mid - 0.32f) < 0.04f, "and about half-released at 1.84 s: the release was scheduled from the end (" + std::to_string(mid) + ")");
            mixLeft(m, 7200 + 960);
            check(!m.playing(v), "and it ends at its duration");
        }
        {
            // A note-off before that shortens the sound.
            audio::Mixer m;
            m.init(kRate, 2, 8, 16, 2);
            const audio::VoiceHandle v = m.playGraph(compile(adsrGraph(0.01f, 0.05f, 0.6f, 0.3f, 2.0f)), nullptr, 0, flat(false));
            mixLeft(m, 24000);
            m.graphNoteOff(v);
            mixLeft(m, 14400 + 960);
            check(!m.playing(v), "a note-off at 0.5 s ends a 2 s sound at about 0.8 s");
        }
        {
            // A note-off during the attack releases from the level reached, not from sustain, and not from 1.
            audio::Mixer m;
            m.init(kRate, 2, 8, 16, 2);
            const audio::VoiceHandle v = m.playGraph(compile(adsrGraph(0.4f, 0.1f, 0.5f, 0.2f, 1.0f)), nullptr, 0, flat(true));
            mixLeft(m, 4800);        // 0.1 s: the attack is a quarter of the way up
            m.graphNoteOff(v);
            const std::vector<f32> rel = mixLeft(m, 480 * 25);
            check(peakOf(rel, 0, rel.size()) < 0.29f, "released from a quarter of the way up the attack, it never rises above that (" + std::to_string(peakOf(rel, 0, rel.size())) + ")");
            check(!m.playing(v), "and ends after its 200 ms release");
        }
        {
            // A graph with nothing to release still fades on a note-off instead of cutting.
            audio::Mixer m;
            m.init(kRate, 2, 8, 16, 2);
            const audio::VoiceHandle v = m.playGraph(compile(graph({mk(K::Sine, 100)}, {}, 1.0f)), nullptr, 0, flat(true));
            std::vector<f32> s = mixLeft(m, 4800);
            m.graphNoteOff(v);
            const std::vector<f32> rel = mixLeft(m, 480 * 4);
            s.insert(s.end(), rel.begin(), rel.end());
            check(!m.playing(v), "a graph with no ADSR ends within a few ms of a note-off");
            check(maxDelta(s) < 0.03f, "without a click: the end is a ramp (max step " + std::to_string(maxDelta(s)) + ")");
            check(std::fabs(s.back()) < 0.01f, "and it ends at zero");
        }
        {
            // stop() is not the note-off: it fades the voice within a block, whatever the envelope says.
            audio::Mixer m;
            m.init(kRate, 2, 8, 16, 2);
            const audio::VoiceHandle v = m.playGraph(compile(adsrGraph(0.01f, 0.05f, 0.6f, 0.5f, 1.0f)), nullptr, 0, flat(true));
            mixLeft(m, 9600);
            m.stop(v);
            mixLeft(m, 480 * 2);
            check(!m.playing(v), "stop() ends the voice within a couple of blocks, well inside the 500 ms release");
        }
    }

    AVER_INFO("a looping graph outlives its duration; a one-shot does not");
    {
        audio::Mixer m;
        m.init(kRate, 2, 8, 16, 2);
        const auto prog = compile(graph({mk(K::Sine, 300)}, {}, 0.1f));
        const audio::VoiceHandle held = m.playGraph(prog, nullptr, 0, flat(true));
        const audio::VoiceHandle once = m.playGraph(prog, nullptr, 0, flat(false));
        check(std::fabs(m.voiceLengthSeconds(once) - 0.1) < 1e-6 && m.voiceLengthSeconds(held) == 0.0, "voiceLengthSeconds is the duration for a one-shot and unknown for a held one");
        mixLeft(m, 24000);
        check(m.playing(held) && !m.playing(once), "after 0.5 s the held voice plays and the one-shot has ended");
        check(std::fabs(m.voicePositionSeconds(held) - 0.5) < 0.02, "and its position counts the time it has played");
    }

    AVER_INFO("voice pool exhaustion steals cleanly");
    {
        audio::Mixer m;
        m.init(kRate, 2, 4, 16, 2);
        audio::SoundData tone;
        tone.channels = 1; tone.sampleRate = kRate;
        for (u32 i = 0; i < 48000; ++i) tone.samples.push_back(std::sin(static_cast<f32>(i) * 0.1f) * 0.3f);
        const audio::SoundHandle sh = m.addSound(std::move(tone));
        const fmt::OcSoundData wg = graph({mk(K::WavePlayer, 1, 1, 0, 0, "tone")}, {}, 5.0f);
        const auto waveProg = compile(wg);
        const auto sineProg = compile(graph({mk(K::Sine, 220), mk(K::Gain, 0.2f)}, {{0, 1, 0}}, 5.0f));

        std::vector<audio::VoiceHandle> first;
        for (int i = 0; i < 4; ++i) {
            const audio::VoiceHandle h = (i % 2) ? m.playGraph(sineProg, nullptr, 0, flat(true)) : m.playGraph(waveProg, &sh, 1, flat(true));
            first.push_back(h);
        }
        check(std::all_of(first.begin(), first.end(), [](audio::VoiceHandle h) { return h != 0; }) && m.activeVoices() == 4, "four graph voices fill the pool of four");
        mixLeft(m, 4800);
        m.removeSound(sh);
        check(m.soundCount() == 1, "the WavePlayer voices hold the sound");
        std::vector<audio::VoiceHandle> second;
        for (int i = 0; i < 4; ++i) second.push_back(m.playGraph(sineProg, nullptr, 0, flat(true)));
        check(std::all_of(second.begin(), second.end(), [](audio::VoiceHandle h) { return h != 0; }), "four more all start: each takes a voice from the pool");
        check(m.activeVoices() == 4 && m.stolenVoices() == 4, "by stealing the four that were there (" + std::to_string(m.stolenVoices()) + " stolen)");
        bool oldDead = true;
        for (audio::VoiceHandle h : first) if (m.playing(h) || m.isGraphVoice(h) || m.setGraphParam(h, "x", 1.0f)) oldDead = false;
        check(oldDead, "the stolen handles read dead and refuse parameter writes");
        bool newAlive = true;
        for (audio::VoiceHandle h : second) if (!m.playing(h) || !m.isGraphVoice(h)) newAlive = false;
        check(newAlive, "the new ones are alive");
        const std::vector<f32> out = mixLeft(m, 9600);
        check(finiteAll(out) && peakOf(out, 0, out.size()) <= 1.0f, "the mix stays finite and in range through the steal");
        m.collect();
        check(m.soundCount() == 0, "the stolen WavePlayer voices released the sound: collect frees it");

        // A plain sound steals a graph voice, and a graph voice steals a plain one.
        audio::SoundData blip;
        blip.channels = 1; blip.sampleRate = kRate;
        blip.samples.assign(48000, 0.25f);
        const audio::SoundHandle bh = m.addSound(std::move(blip));
        audio::PlayDesc pd; pd.sound = bh; pd.looping = true;
        const audio::VoiceHandle plain = m.play(pd);
        check(plain != 0 && !m.isGraphVoice(plain) && !m.setGraphParam(plain, "x", 1.0f), "a plain voice that stole a graph voice's slot is not a graph voice");
        const std::vector<f32> mixed = mixLeft(m, 4800);
        check(finiteAll(mixed), "and the two kinds mix together");
        // A graph voice stealing the quietest voice: make every other voice loud, so the quiet plain one goes.
        for (audio::VoiceHandle h : second) m.setVoiceVolume(h, 2.0f);
        m.setVoiceVolume(plain, 0.01f);
        const audio::VoiceHandle g2 = m.playGraph(sineProg, nullptr, 0, flat(true));
        check(g2 != 0 && !m.playing(plain) && m.isGraphVoice(g2), "a graph voice can steal a plain voice too");

        // A stolen slot's state does not leak into the next graph voice: it starts exactly as a fresh offline render does.
        audio::Mixer m2;
        m2.init(kRate, 2, 1, 4, 1);
        const fmt::OcSoundData noisy = graph({mk(K::Noise), mk(K::LowPass, 3000), mk(K::Delay, 0.01f, 0.5f, 0.5f, 0.05f)}, {{0, 1, 0}, {1, 2, 0}}, 0.2f);
        const auto noisyProg = compile(noisy);
        m2.playGraph(noisyProg, nullptr, 0, flat(true), 5);
        mixLeft(m2, 4800);
        m2.playGraph(noisyProg, nullptr, 0, flat(false), 9);   // steals the only voice
        const std::vector<f32> stolenStart = mixLeft(m2, 9600);
        sound::RenderParams rp; rp.sampleRate = kRate; rp.seed = 9;
        std::vector<f32> ref;
        sound::renderSound(noisy, rp, ref, &why);
        check(stolenStart == ref, "the voice that replaced a stolen one is the offline render exactly: no delay line, filter or phase carried over");
        check(m2.stolenVoices() == 1, "and the single-voice pool counted one steal");
    }

    AVER_INFO("the sample rate can change under a playing graph");
    {
        audio::Mixer m;
        m.init(kRate, 2, 8, 16, 2);
        const auto prog = compile(graph({mk(K::Sine, 500), mk(K::Delay, 0.01f, 0.3f, 0.3f, 0.05f)}, {{0, 1, 0}}, 1.0f));
        m.playGraph(prog, nullptr, 0, flat(true));
        const std::vector<f32> before = mixLeft(m, 48000);
        check(m.setOutputFormat(44100, 2), "the mixer moves to 44.1 kHz");
        const std::vector<f32> after = mixLeft(m, 44100);
        const u32 a = crossings(before, 0, before.size()), b = crossings(after, 0, after.size());
        check(a >= 499 && a <= 501 && b >= 499 && b <= 501, "the graph keeps its 500 Hz across the change (" + std::to_string(a) + ", " + std::to_string(b) + " crossings per second)");
        // The delay line is cleared by the move (its memory is sized in frames), so the echo part steps once; the bound is for "no blow-up".
        check(finiteAll(after) && maxDelta(after) < 0.5f, "and nothing blows up");
    }

    AVER_INFO("the voice's own volume, position and fade apply to a graph voice like any other");
    {
        audio::Mixer m;
        m.init(kRate, 2, 8, 16, 2);
        const auto prog = compile(graph({mk(K::Sine, 300), mk(K::Gain, 0.5f)}, {{0, 1, 0}}, 1.0f));
        const audio::VoiceHandle v = m.playGraph(prog, nullptr, 0, flat(true));
        const f32 full = peakOf(mixLeft(m, 4800), 0, 4800);
        m.setVoiceVolume(v, 0.5f);
        mixLeft(m, 480);
        const f32 half = peakOf(mixLeft(m, 4800), 0, 4800);
        check(std::fabs(half - full * 0.5f) < 0.02f, "setVoiceVolume halves it (" + std::to_string(full) + " -> " + std::to_string(half) + ")");
        m.fadeVoice(v, 0.0f, 0.05f, audio::FadeCurve::Linear, true);
        mixLeft(m, 4800);
        check(!m.playing(v), "fadeVoice with stop-at-end ends it");

        audio::PlayDesc pd = flat(true);
        pd.positional = true;
        pd.position[0] = 0; pd.position[1] = 500; pd.position[2] = 0;   // to the listener's right
        pd.attenuation.innerRadius = 2000; pd.attenuation.outerRadius = 4000;
        m.playGraph(prog, nullptr, 0, pd);
        std::vector<f32> buf(960);
        m.mix(buf.data(), 480);
        f32 l = 0, r = 0;
        for (u32 i = 0; i < 480; ++i) { l = std::max(l, std::fabs(buf[i * 2])); r = std::max(r, std::fabs(buf[i * 2 + 1])); }
        check(r > l * 3.0f, "a positional graph voice pans to the right when it is on the right (L " + std::to_string(l) + ", R " + std::to_string(r) + ")");
    }

    AVER_INFO("mix() allocates nothing: counted with a replacement operator new while graph voices run, change inputs and end");
    {
        // The hook itself works: an allocation inside the window is seen.
        g_allocs = 0; g_counting = true;
        { std::vector<int> v(100, 1); (void)v; }
        g_counting = false;
        check(g_allocs.load() >= 1, "the counting hook sees an ordinary allocation (control)");

        audio::SoundData clip;
        clip.channels = 2; clip.sampleRate = 44100;
        for (u32 i = 0; i < 20000; ++i) { clip.samples.push_back(std::sin(static_cast<f32>(i) * 0.03f)); clip.samples.push_back(std::cos(static_cast<f32>(i) * 0.02f)); }
        audio::Mixer m;
        m.init(kRate, 2, 16, 16, 2);
        const audio::SoundHandle ch = m.addSound(std::move(clip));
        const fmt::OcSoundData g = graph({mk(K::Parameter, 0.5f, 0.02f, 0, 1, "intensity"),     // 0
                                          mk(K::WavePlayer, 1, 1, 0, 0, "clip"),                 // 1
                                          mk(K::Sine, 180), mk(K::Noise), mk(K::Lfo, 3, 0.2f, 0, 0), // 2, 3, 4
                                          mk(K::Mix),                                            // 5
                                          mk(K::HighPass, 80),                                   // 6
                                          mk(K::Delay, 0.05f, 0.4f, 0.3f, 0.2f),                 // 7
                                          mk(K::BandPass, 900, 2),                               // 8
                                          mk(K::Crossfade, 0, 2, 0),                             // 9
                                          mk(K::EnvelopeFollower, 0.01f, 0.1f), mk(K::Random, 0, 1, 20), // 10, 11
                                          mk(K::Adsr, 0.01f, 0.05f, 0.8f, 0.2f),                 // 12
                                          mk(K::Gain, 0.3f)},                                    // 13
                                         {{1, 5, 0}, {2, 5, 0}, {5, 6, 0}, {6, 7, 0}, {7, 8, 0}, {0, 9, 0}, {8, 9, 1}, {6, 9, 2}, {9, 10, 0}, {9, 12, 0}, {12, 13, 0}, {4, 13, 0}},
                                         6.0f);
        const auto prog = compile(g);
        const audio::VoiceHandle a = m.playGraph(prog, &ch, 1, flat(true), 3);
        const audio::VoiceHandle b = m.playGraph(prog, &ch, 1, flat(true), 4);
        const audio::VoiceHandle fin = m.playGraph(compile(graph({mk(K::Sine, 700)}, {}, 0.1f)), nullptr, 0, flat(false));   // ends inside the window
        std::vector<f32> buf(1024 * 2);
        m.mix(buf.data(), 512);   // warm up: first blocks of every voice

        g_allocs = 0; g_counting = true;
        for (int i = 0; i < 400; ++i) {
            if (i == 50)  m.setGraphParam(a, "intensity", 0.9f);
            if (i == 100) m.setGraphParamId(b, 0, 0.1f);
            if (i == 150) m.graphNoteOff(b);
            if (i == 200) m.setVoiceVolume(a, 0.7f);
            m.mix(buf.data(), (i % 3 == 0) ? 1024 : (i % 3 == 1 ? 480 : 77));
        }
        g_counting = false;
        check(g_allocs.load() == 0, "400 blocks with parameter changes, a note-off and a voice ending: " + std::to_string(g_allocs.load()) + " allocations in mix()");
        check(!m.playing(b) && m.playing(a) && !m.playing(fin), "(the note-off voice and the one-shot ended inside that window; the held one still plays)");
    }

    AVER_INFO("hostile use of the runner");
    {
        audio::RunnerOptions ro;
        audio::SoundRunner r(compile(graph({mk(K::Sine, 440)}, {}, 0.01f)), ro);
        check(r.process(nullptr, 100) == 0, "a null buffer produces nothing");
        f32 one = 1.0f;
        check(r.process(&one, 0) == 0, "zero frames produces nothing");
        r.setWave(99, nullptr);
        check(!r.setParam(0, 1.0f), "setParam on a graph with no inputs refuses");
        std::vector<f32> buf(4000, 9.0f);
        const u32 got = r.process(buf.data(), 4000);
        check(got == 480 && buf[479] != 9.0f && buf[480] == 0.0f && r.finished(), "a block that runs past the end returns what was valid and zeroes the rest");
        check(r.process(buf.data(), 10) == 0, "and a finished runner stays finished");
        audio::Mixer m;
        check(m.playGraph(nullptr, nullptr, 0, flat(false)) == 0, "a null program does not play");
        m.init(kRate, 2, 4, 4, 1);
        check(m.playGraph(nullptr, nullptr, 0, flat(false)) == 0, "a null program does not play on a ready mixer either");
    }

    AVER_INFO(g_failures ? "SoundGraphTest: {} FAILURES" : "SoundGraphTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
