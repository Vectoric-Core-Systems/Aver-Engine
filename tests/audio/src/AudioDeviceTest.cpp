// Device robustness, with no sound card: the output policy (OutputDriver) driven by a fake device. Device loss in the middle of a block, a
// default-device change to another rate, no device at all, the device coming back, a wedged device, a flapping one; and the sample-rate
// rule (an asset keeps its own rate and is resampled per voice to whatever the device runs at). The real WASAPI and SDL backends only
// implement OutputBackend, so everything they decide is decided here.
#include "aver/audio/AudioDevice.hpp"
#include "aver/audio/Mixer.hpp"
#include "aver/audio/OutputDriver.hpp"
#include "aver/core/Log.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f64 a, f64 b, f64 eps) { return std::fabs(a - b) <= eps; }

// A mono sine at `hz` in an asset recorded at `rate`, `frames` long.
static audio::SoundData sine(u32 rate, u32 frames, f32 hz, f32 amplitude = 0.5f) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = rate;
    d.samples.resize(frames);
    for (u32 i = 0; i < frames; ++i)
        d.samples[i] = amplitude * std::sin(6.28318530718f * hz * static_cast<f32>(i) / static_cast<f32>(rate));
    return d;
}

static audio::SoundData dc(u32 rate, u32 frames, f32 value) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = rate;
    d.samples.assign(frames, value);
    return d;
}

// Rising zero crossings on one channel of an interleaved buffer: for a clean sine, the number of cycles.
static u32 risingCrossings(const std::vector<f32>& v, u32 stride, u32 offset = 0) {
    u32 n = 0;
    for (usize i = offset + stride; i < v.size(); i += stride)
        if (v[i - stride] < 0.0f && v[i] >= 0.0f) ++n;
    return n;
}

static f64 rms(const std::vector<f32>& v, usize from, usize count, u32 stride) {
    f64 s = 0.0; usize n = 0;
    for (usize i = from; i < v.size() && n < count; i += stride, ++n) s += double(v[i]) * v[i];
    return n ? std::sqrt(s / static_cast<f64>(n)) : 0.0;
}

// ---- the fakes ----

struct FakeClock final : audio::OutputClock {
    f64 t = 100.0;
    f64 now() const override { return t; }
    void sleep(f64 s) override { if (s > 0.0) t += s; }
};

// A scripted device. Each service() call plays the next scripted event; with an empty script it reports Idle after "waiting" the timeout.
struct FakeBackend final : audio::OutputBackend {
    enum class Ev { Serve, LoseAfter, DefaultChange, Idle, Underrun };
    struct Event { Ev kind; u32 frames = 0; };

    explicit FakeBackend(FakeClock& c) : clock(c) {}

    FakeClock& clock;
    bool exists = true;                 // is there a default device right now
    audio::OutputInfo fmt;              // what it looks like
    std::deque<Event> script;
    audio::OutputSource* src = nullptr;
    u32 rate = 0, mixCh = 0;
    int opens = 0, starts = 0, closes = 0;
    std::vector<f32> captured;          // every sample delivered to the "hardware", at the mix channels of the moment
    std::vector<f32> buf;
    std::string err = "no default device";

    bool open(audio::OutputInfo& info) override {
        ++opens;
        if (!exists) return false;
        info = fmt;
        rate = fmt.rate;
        return true;
    }
    bool start(const audio::OutputInfo& info, audio::OutputSource& source) override {
        ++starts;
        src = &source;
        mixCh = info.mixChannels;
        return true;
    }
    void close() override { ++closes; src = nullptr; }
    audio::ServiceResult service(u32 timeoutMs) override {
        if (script.empty()) { clock.sleep(timeoutMs / 1000.0); return audio::ServiceResult::Idle; }
        const Event e = script.front();
        script.pop_front();
        switch (e.kind) {
            case Ev::Serve:
                buf.assign(static_cast<usize>(e.frames) * mixCh, 0.0f);
                src->render(buf.data(), e.frames);
                captured.insert(captured.end(), buf.begin(), buf.end());
                clock.sleep(static_cast<f64>(e.frames) / rate);
                return audio::ServiceResult::Ok;
            case Ev::LoseAfter:        // the device dies after taking `frames` of a block: the mixer rendered them, the hardware never got them
                buf.assign(static_cast<usize>(e.frames) * mixCh, 0.0f);
                src->render(buf.data(), e.frames);
                clock.sleep(static_cast<f64>(e.frames) / rate);
                return audio::ServiceResult::Lost;
            case Ev::DefaultChange: return audio::ServiceResult::DefaultChanged;
            case Ev::Idle: clock.sleep(timeoutMs / 1000.0); return audio::ServiceResult::Idle;
            case Ev::Underrun: src->underrun(); return audio::ServiceResult::Ok;
        }
        return audio::ServiceResult::Ok;
    }
    const char* lastError() const override { return err.c_str(); }

    void serve(u32 frames, u32 blocks = 1) { for (u32 i = 0; i < blocks; ++i) script.push_back({Ev::Serve, frames}); }
};

static audio::OutputInfo device(const char* name, u32 rate, u32 channels, u32 bufferFrames = 480, f32 latencyMs = 0.0f) {
    audio::OutputInfo i;
    i.name = name; i.rate = rate; i.deviceChannels = channels; i.bufferFrames = bufferFrames; i.latencyMs = latencyMs;
    return i;
}

static void stepN(audio::OutputDriver& d, int n) { for (int i = 0; i < n; ++i) d.step(); }

// ---- log capture ----

static std::vector<std::string> g_log;
static void sink(void*, LogLevel, std::string_view msg) { g_log.emplace_back(msg); }
static bool logged(const char* needle) {
    for (const std::string& l : g_log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

// Measures the pitch an asset comes out at on a device of `deviceRate`: rising zero crossings over one second of output.
static f64 measureHz(u32 deviceRate, u32 assetRate, f32 hz) {
    audio::Mixer m;
    m.init(deviceRate, 2, 8, 8, 2);
    // Whole cycles in the asset so the loop seam is clean: one second of it.
    audio::PlayDesc d;
    d.sound = m.addSound(sine(assetRate, assetRate, hz));
    d.looping = true;
    m.play(d);
    std::vector<f32> out;
    std::vector<f32> block(static_cast<usize>(480) * 2);
    u32 done = 0;
    while (done < deviceRate) {
        const u32 n = std::min<u32>(480, deviceRate - done);
        m.mix(block.data(), n);
        out.insert(out.end(), block.begin(), block.begin() + static_cast<usize>(n) * 2);
        done += n;
    }
    return static_cast<f64>(risingCrossings(out, 2));
}

// A busy scene touching the legacy path and every opt-in one (positional + occlusion, fades, buses, reverb, a graph bus with effects, a
// stream, mixed rates). Its output was recorded from the mixer as it stood BEFORE device robustness; see the golden check in main().
static audio::VoiceHandle pinScene(audio::Mixer& m) {
    audio::PlayDesc a; a.sound = m.addSound(sine(48000, 48000, 440.0f)); a.looping = true; a.volume = 0.8f;
    const audio::VoiceHandle first = m.play(a);
    audio::PlayDesc b; b.sound = m.addSound(sine(44100, 20000, 1234.0f)); b.positional = true; b.position[1] = 300.0f; b.pitch = 1.3f;
    const audio::VoiceHandle vb = m.play(b);
    m.setVoiceOcclusion(vb, 0.4f);
    audio::PlayDesc c; c.sound = m.addSound(sine(22050, 22050, 90.0f)); c.bus = audio::Bus::Music; c.fadeInSeconds = 0.2f; c.looping = true; m.play(c);
    m.setReverb({0.3f, 1.2f, 0.4f});
    m.graph().setActive(true);
    const i32 bus = m.graph().addBus();
    m.graph().addEffect(bus, audio::EffectKind::Compressor);
    const u32 delay = m.graph().addEffect(bus, audio::EffectKind::Delay);
    m.graph().effect(delay)->setParamByName("tap1_delay_ms", 123.0f);
    audio::PlayDesc g; g.sound = m.addSound(sine(32000, 32000, 700.0f)); g.looping = true; g.graphBus = bus; m.play(g);
    audio::StreamParams sp; sp.threaded = false; sp.ringFrames = 16384; sp.prebufferFrames = 512;
    audio::PlayDesc sd; sd.bus = audio::Bus::Voice;
    m.playStream(std::make_unique<audio::MemoryStreamSource>(sine(32000, 32000 * 2, 300.0f)), sd, sp);
    m.pumpStreams(100000);
    return first;
}

int main() {
    setLogSink(&sink, nullptr);

    AVER_INFO("=== the sample-rate rule: an asset keeps its rate and plays at the right pitch on any device ===");
    {
        const u32 deviceRates[] = {44100, 48000, 96000};
        const u32 assetRates[] = {48000, 44100, 22050};
        for (u32 dr : deviceRates)
            for (u32 ar : assetRates) {
                const f64 hz = measureHz(dr, ar, 1000.0f);
                check(near(hz, 1000.0, 2.0), "a 1 kHz asset recorded at " + std::to_string(ar) + " Hz comes out at 1 kHz on a " +
                                                  std::to_string(dr) + " Hz device (" + std::to_string(hz) + ")");
            }
        // Length too: a one-second 48 kHz asset lasts one second on every device, not a different number of frames.
        for (u32 dr : deviceRates) {
            audio::Mixer m;
            m.init(dr, 2, 4, 4, 2);
            audio::PlayDesc d;
            d.sound = m.addSound(dc(48000, 48000, 0.25f));
            const audio::VoiceHandle v = m.play(d);
            std::vector<f32> blk(static_cast<usize>(dr) * 2);
            m.mix(blk.data(), dr - 200);
            const bool stillPlaying = m.playing(v);
            m.mix(blk.data(), 400);
            check(stillPlaying && !m.playing(v), "a one-second 48 kHz asset ends after one second on a " + std::to_string(dr) + " Hz device");
        }
    }

    AVER_INFO("=== the driver brings the mixer up at the device's format and reports it ===");
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2, 480);
        audio::OutputDriver d(m, be, clk);
        check(d.state() == audio::OutputState::Stopped, "stopped before begin");
        check(d.begin() && d.state() == audio::OutputState::Running, "begin opens the device");
        check(m.ready() && m.sampleRate() == 48000 && m.channels() == 2, "the mixer runs at the device's rate and channels");
        const audio::OutputInfo i = d.info();
        check(i.name == "Speakers" && i.rate == 48000 && i.deviceChannels == 2 && i.mixChannels == 2 && i.bufferFrames == 480,
              "name, rate, channels and buffer are reported");
        check(near(d.latencyMs(), 10.0, 0.01), "latency is derived from the buffer when the backend gives none (480 / 48000 = 10 ms)");
        check(d.deviceChanges() == 0 && d.underruns() == 0, "no changes and no underruns yet");
        d.end();
        check(d.state() == audio::OutputState::Stopped && be.closes == 1, "end closes the device");
    }
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Mono speaker", 44100, 1, 441, 17.5f);
        audio::OutputDriver d(m, be, clk);
        check(d.begin() && m.channels() == 1 && m.sampleRate() == 44100, "a mono device gets a mono mixer");
        check(near(d.latencyMs(), 17.5, 1.0e-3), "a latency the backend reports is kept");
        be.fmt = device("Surround", 48000, 6, 480);
        be.script.push_back({FakeBackend::Ev::DefaultChange});
        d.step();
        check(m.channels() == 2 && d.info().deviceChannels == 6 && d.info().mixChannels == 2, "a 6-channel device is mixed as stereo");
        d.end();
    }
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.exists = false;
        audio::OutputDriver d(m, be, clk);
        check(!d.begin() && d.state() == audio::OutputState::Stopped && !m.ready(), "begin with no device is false and starts nothing");
    }

    AVER_INFO("=== loss in the middle of a block, then a default device at another rate: nothing is dropped ===");
    {
        g_log.clear();
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2, 480);
        audio::OutputDriver d(m, be, clk);
        d.begin();

        // State that must survive: a long 1 kHz voice, a looping one on a graph bus with an effect, a stream, bus volumes, the master.
        audio::PlayDesc pd;
        pd.sound = m.addSound(sine(48000, 480000, 1000.0f));
        const audio::VoiceHandle voice = m.play(pd);
        m.setBusVolume(audio::Bus::Music, 0.5f);
        m.setMasterVolume(0.8f);
        m.graph().setActive(true);
        const i32 fxBus = m.graph().addBus();
        const u32 fxId = m.graph().addEffect(fxBus, audio::EffectKind::Delay);
        m.graph().effect(fxId)->setParamByName("tap1_delay_ms", 321.0f);
        audio::PlayDesc gd;
        gd.sound = m.addSound(sine(48000, 48000, 440.0f));
        gd.looping = true;
        gd.graphBus = fxBus;
        const audio::VoiceHandle graphVoice = m.play(gd);
        audio::StreamParams sp; sp.threaded = false; sp.ringFrames = 16384; sp.prebufferFrames = 512;
        audio::PlayDesc sd;
        const audio::VoiceHandle streamVoice = m.playStream(std::make_unique<audio::MemoryStreamSource>(sine(22050, 22050 * 8, 300.0f)), sd, sp);
        m.pumpStreams(100000);

        be.serve(480, 10);                                  // 100 ms
        stepN(d, 10);
        const f64 posBefore = m.voicePositionSeconds(voice);
        check(near(posBefore, 0.1, 0.002), "the voice has played 100 ms (" + std::to_string(posBefore) + ")");

        be.fmt = device("Headphones", 44100, 2, 441, 12.0f);
        be.script.push_back({FakeBackend::Ev::LoseAfter, 100});     // dies 100 frames into a block
        stepN(d, 1);
        check(d.state() == audio::OutputState::Running && d.deviceChanges() == 1, "the loss is followed by a rebuild on the new default");
        check(be.closes == 1 && be.opens == 2 && be.starts == 2, "the old endpoint was closed, the new one opened and started");
        check(m.sampleRate() == 44100 && m.channels() == 2, "the mixer follows the device's rate");
        check(d.info().name == "Headphones" && near(d.latencyMs(), 12.0, 1.0e-3), "the report names the new device");

        check(m.playing(voice) && m.playing(graphVoice) && m.playing(streamVoice), "every voice survived the change, on its original handle");
        check(near(m.busVolume(audio::Bus::Music), 0.5, 1.0e-6) && near(m.masterVolume(), 0.8, 1.0e-6), "bus and master volumes survived");
        check(m.graph().active() && m.graph().effect(fxId) != nullptr && m.graph().effect(fxId)->sampleRate() == 44100 &&
              near(m.graph().effect(fxId)->paramByName("tap1_delay_ms"), 321.0, 1.0e-3),
              "the graph and its effect survived, rebuilt for the new rate with its id and parameter");

        be.captured.clear();
        be.serve(441, 100);                                 // one second at the new rate
        stepN(d, 100);
        const f64 posAfter = m.voicePositionSeconds(voice);
        check(posAfter > posBefore + 0.99 && posAfter < posBefore + 1.03, "playback position kept advancing in real time across the change (" + std::to_string(posAfter) + ")");
        check(d.underruns() == 0, "no underrun was counted");
        check(logged("output device changed") && logged("Headphones"), "one Output Log line names the change");
    }

    AVER_INFO("=== pitch is right on both sides of a rate change ===");
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("A", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin();
        audio::PlayDesc pd;
        pd.sound = m.addSound(sine(48000, 48000, 1000.0f));    // an asset imported at 48 kHz
        pd.looping = true;
        m.play(pd);
        be.serve(480, 100);
        stepN(d, 100);
        check(near(risingCrossings(be.captured, 2), 1000.0, 2.0), "1 kHz on the 48 kHz device");
        be.captured.clear();
        be.fmt = device("B", 44100, 2, 441);
        be.script.push_back({FakeBackend::Ev::DefaultChange});
        be.serve(441, 100);
        stepN(d, 101);
        check(m.sampleRate() == 44100 && near(risingCrossings(be.captured, 2), 1000.0, 2.0), "still 1 kHz after the move to the 44.1 kHz device");
        // 0.5 amplitude sine: rms 0.354. Ten cycles from the first frame after the move; a voice restarted or ramped in would read lower.
        check(near(rms(be.captured, 0, 441, 2), 0.3536, 0.02), "and at full level from the first frame, not ramping up from silence");
        be.captured.clear();
        be.fmt = device("C", 96000, 2, 960);
        be.script.push_back({FakeBackend::Ev::DefaultChange});
        be.serve(960, 100);
        stepN(d, 101);
        check(m.sampleRate() == 96000 && near(risingCrossings(be.captured, 2), 1000.0, 2.0), "and 1 kHz after the move to the 96 kHz device");
        check(d.deviceChanges() == 2, "two rebuilds, counted");
    }

    AVER_INFO("=== a fade in progress keeps its length in seconds across a rate change ===");
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("A", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin();
        audio::PlayDesc pd;
        pd.sound = m.addSound(dc(48000, 48000 * 20, 0.5f));
        const audio::VoiceHandle v = m.play(pd);
        m.fadeVoice(v, 0.0f, 1.0f, audio::FadeCurve::Linear, true);   // out over one second, then the voice ends
        be.serve(480, 50);                                              // 0.5 s in
        stepN(d, 50);
        check(m.playing(v), "half way through the fade the voice is still sounding");
        be.fmt = device("B", 96000, 2, 960);
        be.script.push_back({FakeBackend::Ev::DefaultChange});
        stepN(d, 1);
        u32 frames = 0;
        while (m.playing(v) && frames < 96000 * 2) { std::vector<f32> tmp(960 * 2); m.mix(tmp.data(), 960); frames += 960; }
        const f64 remaining = static_cast<f64>(frames) / 96000.0;
        check(near(remaining, 0.5, 0.06), "the other half of the fade took half a second on the new device (" + std::to_string(remaining) + " s)");
    }

    AVER_INFO("=== no device: the mixer keeps mixing into nothing at real-time pace, then picks the device back up ===");
    {
        g_log.clear();
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin();
        audio::PlayDesc pd;
        pd.sound = m.addSound(sine(48000, 48000 * 10, 500.0f));
        const audio::VoiceHandle voice = m.play(pd);
        audio::PlayDesc shortD;
        shortD.sound = m.addSound(dc(48000, 24000, 0.2f));              // half a second
        const audio::VoiceHandle shortVoice = m.play(shortD);
        be.serve(480, 10);
        stepN(d, 10);

        be.exists = false;
        be.script.push_back({FakeBackend::Ev::LoseAfter, 0});
        d.step();
        check(d.state() == audio::OutputState::NoDevice, "a lost device with no replacement leaves the driver in NoDevice");
        check(m.ready() && m.playing(voice), "the mixer and its voices are untouched");
        check(logged("no output device"), "the loss is logged once");

        const int opensAtLoss = be.opens;
        const f64 p0 = m.voicePositionSeconds(voice);
        stepN(d, 400);                                          // 400 idle turns of 5 ms = 2 s of fake time
        const f64 p1 = m.voicePositionSeconds(voice);
        check(near(p1 - p0, 2.0, 0.03), "playback position advanced 2 s while there was no device (" + std::to_string(p1 - p0) + ")");
        check(!m.playing(shortVoice), "a half-second voice finished while there was no device");
        check(be.opens - opensAtLoss >= 1 && be.opens - opensAtLoss <= 3, "the open was retried about once a second, not in a loop (" + std::to_string(be.opens - opensAtLoss) + " tries in 2 s)");
        check(d.state() == audio::OutputState::NoDevice && d.deviceChanges() == 0, "still no device, and no change counted");
        check(d.info().name == "Speakers", "the report keeps the last device's description");

        be.exists = true;
        be.fmt = device("USB headset", 96000, 1, 960);
        for (int i = 0; i < 400 && d.state() != audio::OutputState::Running; ++i) d.step();
        check(d.state() == audio::OutputState::Running && m.sampleRate() == 96000 && m.channels() == 1, "when a device returns the mixer moves to it (96 kHz mono)");
        check(d.deviceChanges() == 1 && d.info().name == "USB headset", "counted and reported");
        check(m.playing(voice) && m.voicePositionSeconds(voice) > p1, "the long voice is still playing, further along");
        check(logged("device back") && logged("USB headset"), "the return is logged");
        be.captured.clear();
        be.serve(960, 5);
        stepN(d, 5);
        check(be.captured.size() == 4800u && rms(be.captured, 0, 4800, 1) > 0.05, "and audio flows to it, mono");
    }

    AVER_INFO("=== a thread starved for seconds does not owe seconds of audio ===");
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin();
        audio::PlayDesc pd;
        pd.sound = m.addSound(dc(48000, 48000 * 30, 0.1f));
        const audio::VoiceHandle voice = m.play(pd);
        be.exists = false;
        be.script.push_back({FakeBackend::Ev::LoseAfter, 0});
        d.step();
        clk.t += 10.0;
        d.step();
        const f64 p = m.voicePositionSeconds(voice);
        check(p > 0.0 && p <= 0.6, "after a 10 s stall the idle mixer catches up by at most half a second (" + std::to_string(p) + " s)");
    }

    AVER_INFO("=== a device that flaps is not reopened in a loop ===");
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin();
        be.script.push_back({FakeBackend::Ev::LoseAfter, 0});
        be.script.push_back({FakeBackend::Ev::LoseAfter, 0});
        d.step();
        check(d.state() == audio::OutputState::Running && be.opens == 2, "the first loss reopens at once");
        d.step();
        check(d.state() == audio::OutputState::NoDevice && be.opens == 2, "a second loss within the gap waits for the retry instead of reopening");
        for (int i = 0; i < 400 && d.state() != audio::OutputState::Running; ++i) d.step();
        check(d.state() == audio::OutputState::Running && be.opens == 3, "and the retry brings it back");
    }

    AVER_INFO("=== a wedged device is rebuilt, not waited on forever ===");
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin();
        int steps = 0;
        while (d.deviceChanges() == 0 && steps < 60) { d.step(); ++steps; }       // the script is empty: every service() times out (100 ms)
        check(steps >= 19 && steps <= 22, "the device is rebuilt after about two seconds of silence (" + std::to_string(steps) + " x 100 ms)");
        check(d.underruns() == 1 && be.opens == 2 && d.state() == audio::OutputState::Running, "the stall counts as an underrun and a new endpoint is running");
    }

    AVER_INFO("=== underruns the backend reports are counted ===");
    {
        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin();
        for (int i = 0; i < 3; ++i) be.script.push_back({FakeBackend::Ev::Underrun});
        stepN(d, 3);
        check(d.underruns() == 3, "three underruns");
    }

    AVER_INFO("=== defaults unchanged: through the driver the mixer renders the very same samples ===");
    {
        auto scene = [](audio::Mixer& m) {
            audio::PlayDesc a; a.sound = m.addSound(sine(48000, 48000, 440.0f)); a.looping = true; m.play(a);
            audio::PlayDesc b; b.sound = m.addSound(sine(44100, 20000, 1234.0f)); b.positional = true; b.position[1] = 300.0f; b.pitch = 1.3f; m.play(b);
            m.setReverb({0.3f, 1.2f, 0.4f});
        };
        audio::Mixer direct; direct.init(48000, 2, 64, 1024, 16); scene(direct);
        std::vector<f32> ref;
        for (int i = 0; i < 50; ++i) { std::vector<f32> blk(480 * 2); direct.mix(blk.data(), 480); ref.insert(ref.end(), blk.begin(), blk.end()); }

        audio::Mixer m; FakeClock clk; FakeBackend be(clk);
        be.fmt = device("Speakers", 48000, 2);
        audio::OutputDriver d(m, be, clk);
        d.begin(); scene(m);
        be.serve(480, 50);
        stepN(d, 50);
        bool same = be.captured.size() == ref.size();
        for (usize i = 0; same && i < ref.size(); ++i) same = be.captured[i] == ref[i];
        check(same, "24000 samples through the driver are bit-identical to the mixer called directly");

        // And a mixer that was never moved renders the same as it always did when setOutputFormat is asked for what it already has.
        audio::Mixer same2; same2.init(48000, 2, 64, 1024, 16); scene(same2);
        same2.setOutputFormat(48000, 2);
        std::vector<f32> again;
        for (int i = 0; i < 50; ++i) { std::vector<f32> blk(480 * 2); same2.mix(blk.data(), 480); again.insert(again.end(), blk.begin(), blk.end()); }
        check(again == ref, "setOutputFormat to the current format changes nothing");
    }
    {
        // GOLDEN, recorded from the mixer at the commit before this package (Mixer.cpp, BusGraph.cpp and headers as in HEAD): 100 blocks of
        // 480 frames of pinScene, with a fade requested on the first voice after block 40. The sums are over 96000 samples.
        audio::Mixer m;
        m.init(48000, 2, 64, 1024, 16);
        const audio::VoiceHandle first = pinScene(m);
        std::vector<f32> out;
        for (int i = 0; i < 100; ++i) {
            std::vector<f32> blk(480 * 2);
            m.mix(blk.data(), 480);
            out.insert(out.end(), blk.begin(), blk.end());
            if (i == 40) m.fadeVoice(first, 0.3f, 0.1f);
        }
        f64 s = 0.0, s2 = 0.0;
        for (f32 x : out) { s += x; s2 += static_cast<f64>(x) * x; }
        check(out.size() == 96000u && near(s, -104.686836, 1.0e-3) && near(s2, 38301.5482, 0.05),
              "a busy scene's output sums match the pre-change mixer (sum " + std::to_string(s) + ", sum of squares " + std::to_string(s2) + ")");
        check(near(out[1000], 0.619705975, 1.0e-5) && near(out[17001], -0.514766634, 1.0e-5) &&
              near(out[30002], -0.789573252, 1.0e-5) && near(out[47999], -0.28781414, 1.0e-5),
              "and so do four sample values");
    }

    AVER_INFO("=== Mixer::setOutputFormat ===");
    {
        audio::Mixer m;
        check(!m.setOutputFormat(48000, 2), "refused before init");
        m.init(48000, 2, 8, 8, 2);
        check(!m.setOutputFormat(0, 2) && !m.setOutputFormat(48000, 3) && m.sampleRate() == 48000, "refused for a zero rate or three channels, and nothing moved");
        check(m.setOutputFormat(44100, 1) && m.sampleRate() == 44100 && m.channels() == 1, "stereo to mono at another rate");
        audio::PlayDesc pd; pd.sound = m.addSound(dc(44100, 44100, 0.4f));
        const audio::VoiceHandle v = m.play(pd);
        std::vector<f32> out(1024);
        m.mix(out.data(), 1024);
        check(m.playing(v) && out[800] > 0.2f && out[800] <= 1.0f, "a mono mixer renders (" + std::to_string(out[800]) + ")");
        check(m.setOutputFormat(48000, 2) && m.channels() == 2, "and back to stereo");
        std::vector<f32> st(2048);
        m.mix(st.data(), 1024);
        check(m.playing(v) && st[1200] > 0.2f && st[1201] > 0.2f, "a stereo render of the same voice");

        // A live ring (AudioStreamGenerator) keeps its own rate and its frames.
        audio::Mixer l; l.init(48000, 2, 8, 8, 4);
        audio::PlayDesc ld;
        const audio::VoiceHandle live = l.playLive(ld, 22050, 1, 8192);
        std::vector<f32> push(4000, 0.3f);
        l.pushLive(live, push.data(), 4000);
        std::vector<f32> blk(480 * 2);
        l.mix(blk.data(), 480);
        l.setOutputFormat(96000, 2);
        l.mix(blk.data(), 480);
        check(l.playing(live) && l.liveFree(live) > 0, "a live voice survives the change");
    }

    AVER_INFO("=== AudioDevice: the render thread against a fake device ===");
    {
        struct LiveFake final : audio::OutputBackend {
            std::atomic<bool> exists{true}, loseNext{false}, defaultNext{false};
            std::mutex mu;
            audio::OutputInfo fmt = device("Thread device", 48000, 2);
            std::atomic<u32> rendered{0};
            audio::OutputSource* src = nullptr;
            u32 mixCh = 0;
            std::vector<f32> buf;
            bool open(audio::OutputInfo& info) override {
                std::lock_guard<std::mutex> g(mu);
                if (!exists.load()) return false;
                info = fmt;
                return true;
            }
            bool start(const audio::OutputInfo& info, audio::OutputSource& s) override {
                src = &s; mixCh = info.mixChannels; buf.assign(2048u * 2u, 0.0f);
                return true;
            }
            void close() override { src = nullptr; }
            audio::ServiceResult service(u32) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (loseNext.exchange(false)) return audio::ServiceResult::Lost;
                if (defaultNext.exchange(false)) return audio::ServiceResult::DefaultChanged;
                if (src) { src->render(buf.data(), 96); rendered.fetch_add(1); }
                return audio::ServiceResult::Ok;
            }
        };
        auto waitFor = [](auto pred, int ms) {
            for (int i = 0; i < ms / 2; ++i) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            return pred();
        };

        auto fake = std::make_unique<LiveFake>();
        LiveFake* fk = fake.get();
        audio::OutputConfig cfg;
        cfg.serviceMs = 10; cfg.retrySeconds = 0.05; cfg.minRebuildGap = 0.01;
        audio::AudioDevice dev;
        check(dev.startWith(std::move(fake), nullptr, cfg) && dev.running() && dev.deviceOpen(), "the render thread starts and reports a device");
        check(dev.sampleRate() == 48000 && dev.deviceName() == "Thread device", "rate and name through AudioDevice");

        audio::PlayDesc pd;
        pd.sound = dev.mixer().addSound(sine(48000, 480000, 700.0f));
        const audio::VoiceHandle v = dev.mixer().play(pd);
        check(waitFor([&] { return fk->rendered.load() > 20; }, 2000), "buffers flow");

        { std::lock_guard<std::mutex> g(fk->mu); fk->fmt = device("Second device", 44100, 2, 441); }
        fk->defaultNext = true;
        check(waitFor([&] { return dev.deviceChanges() == 1; }, 3000) && dev.sampleRate() == 44100 && dev.deviceName() == "Second device",
              "a default-device change moves the running device to the new one");
        check(dev.mixer().playing(v), "the voice kept playing");

        fk->exists = false;
        fk->loseNext = true;
        check(waitFor([&] { return dev.state() == audio::OutputState::NoDevice; }, 3000), "losing the device with no replacement is survived");
        check(dev.running() && !dev.deviceOpen(), "the render thread is alive without a device");
        const f64 p0 = dev.mixer().voicePositionSeconds(v);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const f64 p1 = dev.mixer().voicePositionSeconds(v);
        check(p1 - p0 > 0.2 && p1 - p0 < 0.45, "and real time keeps passing in the mixer (" + std::to_string(p1 - p0) + " s in 0.3 s)");

        { std::lock_guard<std::mutex> g(fk->mu); fk->fmt = device("Back", 96000, 2, 960); }
        fk->exists = true;
        check(waitFor([&] { return dev.deviceOpen() && dev.sampleRate() == 96000; }, 3000), "the device returns and the mixer follows it to 96 kHz");
        check(dev.deviceChanges() == 2 && dev.mixer().playing(v), "two changes counted, the voice still playing");

        const auto t0 = std::chrono::steady_clock::now();
        dev.stop();
        const f64 stopMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(!dev.running() && stopMs < 500.0, "stop joins the render thread promptly (" + std::to_string(stopMs) + " ms)");
        check(dev.deviceChanges() == 2, "the counters stay readable after stop");

        // A device with nothing to open fails start() cleanly, with no thread left behind.
        auto none = std::make_unique<LiveFake>();
        none->exists = false;
        audio::AudioDevice dev2;
        check(!dev2.startWith(std::move(none), nullptr, cfg) && !dev2.running(), "starting with no device returns false and leaves nothing running");
    }

    setLogSink(nullptr, nullptr);
    if (g_failures == 0) AVER_INFO("AudioDeviceTest: all checks passed");
    else AVER_ERROR("AudioDeviceTest: {} FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
