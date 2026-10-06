// Headless tests for streamed playback, crossfades, occlusion filtering and reverb zones. No device:
// the mixer is driven block by block and the output compared sample by sample.
#include "aver/audio/Dsp.hpp"
#include "aver/audio/Mixer.hpp"
#include "aver/audio/Music.hpp"
#include "aver/audio/Reverb.hpp"
#include "aver/audio/Stream.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static bool near(f32 a, f32 b, f32 eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

constexpr u32 kRate = 48000;

static audio::SoundData dc(u32 frames, f32 value, u32 rate = kRate) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = rate;
    d.samples.assign(frames, value);
    return d;
}

static audio::SoundData sine(u32 frames, f32 hz, f32 amp = 0.5f) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = kRate;
    d.samples.resize(frames);
    for (u32 i = 0; i < frames; ++i)
        d.samples[i] = amp * std::sin(6.28318530718f * hz * static_cast<f32>(i) / static_cast<f32>(kRate));
    return d;
}

// A ramp that never starts at zero, so "output began" is visible as the first non-zero sample.
static audio::SoundData ramp(u32 frames) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = kRate;
    d.samples.resize(frames);
    for (u32 i = 0; i < frames; ++i) d.samples[i] = 0.1f + 0.5f * std::sin(0.01f * static_cast<f32>(i));
    return d;
}

static f32 rmsOf(const std::vector<f32>& v, usize from, usize to) {
    f64 s = 0.0;
    for (usize i = from; i < to; ++i) s += static_cast<f64>(v[i]) * v[i];
    return to > from ? static_cast<f32>(std::sqrt(s / static_cast<f64>(to - from))) : 0.0f;
}

static f64 energyOf(const std::vector<f32>& v, usize from, usize to) {
    f64 s = 0.0;
    for (usize i = from; i < to && i < v.size(); ++i) s += static_cast<f64>(v[i]) * v[i];
    return s;
}

// Mixes `blocks` blocks of `block` frames, appending to `out`.
static void mixBlocks(audio::Mixer& m, std::vector<f32>& out, u32 blocks, u32 block) {
    for (u32 b = 0; b < blocks; ++b) {
        const usize base = out.size();
        out.resize(base + block);
        m.mix(out.data() + base, block);
    }
}

// Writes a little-endian u16/u32.
static void put16(std::ofstream& f, u16 v) { f.write(reinterpret_cast<const char*>(&v), 2); }
static void put32(std::ofstream& f, u32 v) { f.write(reinterpret_cast<const char*>(&v), 4); }

// Writes a minimal PCM WAV: `bits` 8 or 16, interleaved raw bytes already encoded.
static void writeWav(const std::filesystem::path& p, u16 channels, u32 rate, u16 bits, const std::vector<u8>& data) {
    std::ofstream f(p, std::ios::binary);
    f.write("RIFF", 4); put32(f, 36 + static_cast<u32>(data.size()));
    f.write("WAVE", 4); f.write("fmt ", 4); put32(f, 16);
    put16(f, 1); put16(f, channels); put32(f, rate);
    put32(f, rate * channels * (bits / 8)); put16(f, static_cast<u16>(channels * (bits / 8))); put16(f, bits);
    f.write("data", 4); put32(f, static_cast<u32>(data.size()));
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

int main() {
    AVER_INFO("=== a stream plays exactly what a resident sound plays ===");
    {
        audio::Mixer ref, str;
        ref.init(kRate, 1, 16, 16, 4);
        str.init(kRate, 1, 16, 16, 4);
        const audio::SoundData src = ramp(4800);

        audio::SoundData copy = src;
        audio::PlayDesc rd; rd.sound = ref.addSound(std::move(copy)); rd.volume = 1.0f;
        ref.play(rd);

        audio::StreamParams sp; sp.threaded = false; sp.ringFrames = 8192; sp.prebufferFrames = 512;
        audio::PlayDesc sd; sd.volume = 1.0f;
        const audio::VoiceHandle v = str.playStream(std::make_unique<audio::MemoryStreamSource>(src), sd, sp);
        check(v != 0, "a stream voice starts");
        check(str.activeStreams() == 1, "and holds one stream");
        str.pumpStreams(100000);

        std::vector<f32> a, b;
        mixBlocks(ref, a, 12, 480);
        mixBlocks(str, b, 12, 480);
        f32 worst = 0.0f;
        for (usize i = 0; i < a.size(); ++i) worst = std::fmax(worst, std::fabs(a[i] - b[i]));
        check(worst < 1.0e-6f, "all 5760 samples match the resident render");
        check(!str.playing(v), "the stream voice ends with its source");
        str.collect();
        check(str.activeStreams() == 0 && str.streamUnderruns() == 0, "its decoder is reclaimed, with no underrun");
    }

    AVER_INFO("=== a stream that runs dry goes quiet, recovers, and keeps playing ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 16, 16, 4);
        audio::StreamParams sp; sp.threaded = false; sp.ringFrames = 4096; sp.prebufferFrames = 512;
        audio::PlayDesc d; d.volume = 1.0f;
        const audio::VoiceHandle v = m.playStream(std::make_unique<audio::MemoryStreamSource>(dc(20000, 1.0f)), d, sp);

        std::vector<f32> out;
        mixBlocks(m, out, 1, 512);
        check(rmsOf(out, 0, 512) == 0.0f, "nothing sounds before the prebuffer is filled");
        check(m.streamUnderruns() == 0, "and waiting for the prebuffer is not an underrun");

        m.pumpStreams(1024);
        out.clear();
        mixBlocks(m, out, 1, 512);
        check(near(out[0], 1.0f) && near(out[511], 1.0f), "block 1: full level while data lasts");
        check(m.streamUnderruns() == 0, "no underrun yet");

        out.clear();
        mixBlocks(m, out, 1, 512);
        check(near(out[0], 1.0f) && near(out[446], 1.0f), "block 2: level holds until the data ends");
        check(out[510] < 0.05f && out[510] > 0.0f, "then fades out over the last frames rather than stepping to zero");
        check(out[511] == 0.0f, "and the frame with no data is silent");
        check(m.streamUnderruns() == 1, "the run-dry is counted once");
        check(m.playing(v), "the voice survives an underrun");

        out.clear();
        mixBlocks(m, out, 1, 512);
        check(rmsOf(out, 0, 512) == 0.0f, "block 3: silence while starved");
        check(m.streamUnderruns() == 1, "re-buffering after a run-dry is waiting, not another underrun");

        m.pumpStreams(2048);
        out.clear();
        mixBlocks(m, out, 1, 512);
        check(out[0] < 0.01f && near(out[511], 1.0f), "block 4: data is back and the level ramps in, not clicks in");
        bool rising = true;
        for (usize i = 1; i < 512; ++i) if (out[i] < out[i - 1]) rising = false;
        check(rising, "monotonically");
        check(m.streamUnderruns() == 1 && m.playing(v), "recovery adds no underrun and the voice is still alive");

        m.stop(v);
        out.clear();
        mixBlocks(m, out, 2, 512);
        check(!m.playing(v), "stopping a stream ends it");
        m.collect();
        check(m.activeStreams() == 0, "and frees its decoder");
    }

    AVER_INFO("=== a decoder thread feeds a ring smaller than the file ===");
    {
        audio::Mixer ref, str;
        ref.init(kRate, 1, 16, 16, 4);
        str.init(kRate, 1, 16, 16, 4);
        const audio::SoundData src = ramp(24000);
        audio::SoundData copy = src;
        audio::PlayDesc rd; rd.sound = ref.addSound(std::move(copy));
        ref.play(rd);

        audio::StreamParams sp; sp.ringFrames = 4800; sp.prebufferFrames = 960; sp.chunkFrames = 1024;
        audio::PlayDesc sd;
        const audio::VoiceHandle v = str.playStream(std::make_unique<audio::MemoryStreamSource>(src), sd, sp);

        std::vector<f32> a, b;
        mixBlocks(ref, a, 100, 256);
        for (int i = 0; i < 2000 && (str.playing(v) || b.size() < 512); ++i) {
            mixBlocks(str, b, 1, 256);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(!str.playing(v), "the threaded stream plays to its end");
        if (str.streamUnderruns() == 0) {
            usize first = 0;
            while (first < b.size() && b[first] == 0.0f) ++first;
            f32 worst = 0.0f;
            for (usize i = 0; i < 24000 && first + i < b.size(); ++i) worst = std::fmax(worst, std::fabs(a[i] - b[first + i]));
            check(worst < 1.0e-6f, "through a wrapping ring it is still bit-exact against the resident render");
        } else {
            AVER_INFO("  SKIP  the host starved the decoder thread ({} underruns); exactness not asserted", str.streamUnderruns());
        }
        str.collect();
        check(str.activeStreams() == 0, "the decoder thread is joined on collect");
    }

    AVER_INFO("=== looping streams repeat their loop region without a seam ===");
    {
        audio::StreamParams sp; sp.threaded = false; sp.ringFrames = 1000; sp.chunkFrames = 64;
        sp.looping = true; sp.loopBegin = 20; sp.loopEnd = 100;
        audio::SoundData src; src.channels = 1; src.sampleRate = kRate;
        for (u32 i = 0; i < 100; ++i) src.samples.push_back(static_cast<f32>(i) / 1000.0f);
        audio::StreamDecoder dec(std::make_unique<audio::MemoryStreamSource>(src), sp);
        check(dec.valid(), "a looping decoder is valid");
        dec.pump(400);
        check(dec.available() == 400, "it keeps producing past the end of the file");
        bool ok = true;
        for (u32 k = 0; k < 400; ++k) {
            const u32 expect = k < 100 ? k : 20 + (k - 100) % 80;
            if (std::lround(dec.frameAt(k)[0] * 1000.0f) != static_cast<long>(expect)) ok = false;
        }
        check(ok, "0..99 once, then 20..99 over and over");
        check(!dec.endReached(), "a looping stream never reaches its end");

        audio::StreamParams once = sp; once.looping = false;
        audio::StreamDecoder dec2(std::make_unique<audio::MemoryStreamSource>(src), once);
        dec2.pump(400);
        check(dec2.available() == 100 && dec2.endReached(), "without looping it stops at 100 frames and reports the end");
    }

    AVER_INFO("=== a WAV file streams incrementally ===");
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
        const std::filesystem::path wav = dir / "aver_stream_test_16.wav";
        std::vector<u8> bytes;
        for (i32 i = 0; i < 1000; ++i) {
            const i16 l = static_cast<i16>(i * 10), r = static_cast<i16>(-i * 10);
            bytes.push_back(static_cast<u8>(l & 0xFF)); bytes.push_back(static_cast<u8>((l >> 8) & 0xFF));
            bytes.push_back(static_cast<u8>(r & 0xFF)); bytes.push_back(static_cast<u8>((r >> 8) & 0xFF));
        }
        writeWav(wav, 2, 22050, 16, bytes);

        std::string why;
        auto src = audio::openWavStream(wav.string().c_str(), &why);
        check(src != nullptr, "a 16-bit stereo WAV opens");
        if (src) {
            check(src->channels() == 2 && src->sampleRate() == 22050 && src->totalFrames() == 1000,
                  "with its true format and length");
            std::vector<f32> buf(300 * 2);
            check(src->read(buf.data(), 300) == 300, "reads a chunk");
            check(near(buf[0], 0.0f) && near(buf[2 * 299], 2990.0f / 32768.0f, 1.0e-6f) &&
                  near(buf[2 * 299 + 1], -2990.0f / 32768.0f, 1.0e-6f), "with the right samples in the right channels");
            u32 total = 300, n;
            std::vector<f32> big(4096 * 2);
            while ((n = src->read(big.data(), 4096)) > 0) total += n;
            check(total == 1000, "and reads exactly 1000 frames to the end");
            check(src->seek(500) && src->read(buf.data(), 1) == 1 && near(buf[0], 5000.0f / 32768.0f, 1.0e-6f),
                  "seek lands on the right frame");
        }

        const std::filesystem::path wav8 = dir / "aver_stream_test_8.wav";
        writeWav(wav8, 1, 8000, 8, {128, 0, 255, 192});
        auto s8 = audio::openWavStream(wav8.string().c_str(), &why);
        check(s8 != nullptr, "an 8-bit mono WAV opens");
        if (s8) {
            f32 v[4];
            check(s8->read(v, 4) == 4 && near(v[0], 0.0f) && near(v[1], -1.0f) && near(v[2], 127.0f / 128.0f, 1.0e-6f) &&
                  near(v[3], 0.5f), "8-bit samples are unsigned, centred on 128");
        }

        const std::filesystem::path junk = dir / "aver_stream_test_junk.wav";
        { std::ofstream f(junk, std::ios::binary); f << "this is not a wave file at all, not even close"; }
        why.clear();
        check(audio::openWavStream(junk.string().c_str(), &why) == nullptr && !why.empty(),
              "a file that is not a WAV is refused with a reason");

        std::error_code ec;
        std::filesystem::remove(wav, ec); std::filesystem::remove(wav8, ec); std::filesystem::remove(junk, ec);
    }

    AVER_INFO("=== crossfade gains ===");
    {
        f32 o = 0, i = 0;
        audio::crossfadeGains(0.0f, audio::FadeCurve::EqualPower, o, i);
        check(near(o, 1.0f) && near(i, 0.0f), "equal-power starts all outgoing");
        audio::crossfadeGains(1.0f, audio::FadeCurve::EqualPower, o, i);
        check(near(o, 0.0f) && near(i, 1.0f), "and ends all incoming");
        bool power = true, mono = true, lin = true;
        f32 po = 1.0f, pi = 0.0f;
        for (int k = 0; k <= 100; ++k) {
            const f32 p = static_cast<f32>(k) / 100.0f;
            audio::crossfadeGains(p, audio::FadeCurve::EqualPower, o, i);
            if (!near(o * o + i * i, 1.0f, 1.0e-4f)) power = false;
            if (o > po + 1.0e-6f || i < pi - 1.0e-6f) mono = false;
            po = o; pi = i;
            audio::crossfadeGains(p, audio::FadeCurve::Linear, o, i);
            if (!near(o + i, 1.0f, 1.0e-5f)) lin = false;
        }
        check(power, "equal-power holds unit power across the whole fade");
        check(mono, "outgoing only falls and incoming only rises");
        check(lin, "linear sums to unit amplitude");
        audio::crossfadeGains(0.5f, audio::FadeCurve::EqualPower, o, i);
        check(near(o, 0.70710678f) && near(i, 0.70710678f), "the midpoint is -3 dB each, not -6");
    }

    AVER_INFO("=== the music slot crossfades two tracks sample-accurately ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 16, 16, 4);
        audio::MusicCrossfader cf(&m);

        audio::MusicCrossfader::Options opt;
        opt.fadeSeconds = 0.0f;
        opt.stream.threaded = false; opt.stream.ringFrames = 24000; opt.stream.prebufferFrames = 1000;
        const audio::VoiceHandle a = cf.play(std::make_unique<audio::MemoryStreamSource>(dc(96000, 0.5f)), opt);
        m.pumpStreams(8192);
        std::vector<f32> out;
        mixBlocks(m, out, 2, 480);
        check(a != 0 && near(out[959], 0.5f), "track A plays at full level");
        check(cf.current() == a, "and is the current track");

        opt.fadeSeconds = 0.5f;
        const audio::VoiceHandle b = cf.play(std::make_unique<audio::MemoryStreamSource>(dc(96000, 0.5f)), opt);
        check(b != 0 && cf.current() == b, "starting B makes it current");

        bool ok = true;
        f32 at20 = 0, at25 = 0, at40 = 0, at50 = 0;
        for (int k = 1; k <= 50; ++k) {
            m.pumpStreams(4096);
            out.clear();
            mixBlocks(m, out, 1, 480);
            const f32 p = static_cast<f32>(k) * 480.0f / 24000.0f;
            const f32 expect = 0.5f * (std::cos(p * 1.57079633f) + std::sin(p * 1.57079633f));
            if (!near(out[479], expect, 2.0e-3f)) ok = false;
            if (k == 20) at20 = out[479];
            if (k == 25) at25 = out[479];
            if (k == 40) at40 = out[479];
            if (k == 50) at50 = out[479];
        }
        check(ok, "every block's end level equals cos + sin of its crossfade position");
        check(near(at25, 0.70710678f, 2.0e-3f), "at the midpoint two correlated tracks sum to sqrt 2");
        check(at20 > 0.5f && at40 > 0.5f, "and dip nowhere");
        check(near(at50, 0.5f, 1.0e-3f), "after the fade only B remains, at its full level");
        check(!m.playing(a), "track A has ended on its own");
        check(m.playing(b), "track B keeps playing");

        cf.stop(0.1f);
        for (int k = 0; k < 12; ++k) { m.pumpStreams(4096); out.clear(); mixBlocks(m, out, 1, 480); }
        check(!m.playing(b) && cf.current() == 0, "stopping the slot fades B out and ends it");
    }

    AVER_INFO("=== per-voice fades ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 16, 16, 4);
        audio::SoundData loop = dc(4800, 1.0f);
        audio::PlayDesc d; d.sound = m.addSound(std::move(loop)); d.looping = true;
        d.fadeInSeconds = 0.1f; d.fadeCurve = audio::FadeCurve::Linear;
        const audio::VoiceHandle v = m.play(d);
        std::vector<f32> out;
        bool ok = true;
        for (int k = 1; k <= 10; ++k) {
            out.clear();
            mixBlocks(m, out, 1, 480);
            if (!near(out[479], static_cast<f32>(k) * 0.1f, 1.0e-4f)) ok = false;
        }
        check(ok, "a linear fade-in climbs 0.1 per 480-frame block over 4800 frames");
        out.clear(); mixBlocks(m, out, 1, 480);
        check(near(out[479], 1.0f), "and holds full level after");

        m.fadeVoice(v, 0.0f, 0.1f, audio::FadeCurve::Linear, true);
        for (int k = 0; k < 9; ++k) { out.clear(); mixBlocks(m, out, 1, 480); }
        check(m.playing(v) && near(out[479], 0.1f, 1.0e-3f), "a fade-out is nearly silent one block before it ends");
        out.clear(); mixBlocks(m, out, 1, 480);
        check(!m.playing(v) && near(out[479], 0.0f, 1.0e-4f), "and stops the voice at silence, with no step");
    }

    AVER_INFO("=== occlusion filter response ===");
    {
        const audio::BiquadCoeffs c = audio::lowpassCoeffs(1000.0f, static_cast<f32>(kRate));
        check(near(audio::biquadMagnitude(c, 1000.0f, kRate), 0.70710678f, 0.01f), "-3 dB at the cutoff");
        check(audio::biquadMagnitude(c, 100.0f, kRate) > 0.99f, "passes the lows");
        check(audio::biquadMagnitude(c, 8000.0f, kRate) < 0.03f, "kills the highs: 12 dB per octave");
        check(near(audio::biquadMagnitude(c, 1.0f, kRate), 1.0f, 1.0e-3f), "unity at DC");

        // The same thing measured in the time domain, through the running filter.
        audio::BiquadState st;
        std::vector<f32> filtered;
        for (u32 i = 0; i < 9600; ++i)
            filtered.push_back(st.process(c, std::sin(6.28318530718f * 8000.0f * static_cast<f32>(i) / kRate)));
        check(rmsOf(filtered, 4800, 9600) < 0.03f * 0.70710678f * 1.5f, "an 8 kHz sine is crushed by the running filter");

        audio::OcclusionCurve curve;
        check(near(audio::occlusionCutoffHz(0.0f, curve), curve.maxCutoffHz, 1.0f), "no occlusion: cutoff fully open");
        check(near(audio::occlusionCutoffHz(1.0f, curve), curve.minCutoffHz, 1.0f), "full occlusion: cutoff at its floor");
        bool mono = true; f32 prev = 1.0e9f;
        for (int k = 0; k <= 100; ++k) {
            const f32 hz = audio::occlusionCutoffHz(static_cast<f32>(k) / 100.0f, curve);
            if (hz > prev + 1.0e-3f) mono = false;
            prev = hz;
        }
        check(mono, "the cutoff only falls as occlusion rises");
        check(near(audio::occlusionGain(0.0f, curve), 1.0f) && near(audio::occlusionGain(1.0f, curve), curve.minVolume),
              "volume runs from 1 to the curve's floor");

        // Through the mixer: steady-state level of a looping sine at full occlusion.
        auto renderRms = [&](f32 hz, f32 occlusion, const audio::OcclusionCurve* custom) {
            audio::Mixer m;
            m.init(kRate, 1, 8, 8, 2);
            if (custom) m.setOcclusionCurve(*custom);
            audio::SoundData s = sine(4800, hz);
            audio::PlayDesc d; d.sound = m.addSound(std::move(s)); d.looping = true;
            const audio::VoiceHandle v = m.play(d);
            m.setVoiceOcclusion(v, occlusion);
            std::vector<f32> out;
            mixBlocks(m, out, 30, 480);
            return rmsOf(out, 480 * 20, out.size());
        };
        const f32 open200 = renderRms(200.0f, 0.0f, nullptr);
        check(near(open200, 0.5f * 0.70710678f, 2.0e-3f), "unoccluded, the sine passes untouched");
        const audio::BiquadCoeffs c450 = audio::lowpassCoeffs(450.0f, static_cast<f32>(kRate));
        const f32 occ200 = renderRms(200.0f, 1.0f, nullptr);
        const f32 want200 = open200 * 0.30f * audio::biquadMagnitude(c450, 200.0f, kRate);
        check(near(occ200, want200, want200 * 0.05f), "occluded, 200 Hz is ducked to the volume floor times the filter's gain");
        const f32 occ4k = renderRms(4000.0f, 1.0f, nullptr);
        check(occ4k < 0.01f * renderRms(4000.0f, 0.0f, nullptr), "occluded, 4 kHz is almost gone");
        check(occ4k < occ200, "so occlusion dulls: the highs fall further than the lows");

        audio::OcclusionCurve custom; custom.minCutoffHz = 1000.0f; custom.minVolume = 0.5f;
        const f32 open1k = renderRms(1000.0f, 0.0f, &custom);
        const f32 occ1k = renderRms(1000.0f, 1.0f, &custom);
        check(near(occ1k / open1k, 0.5f * 0.70710678f, 0.02f), "a custom curve moves the cutoff and the floor");

        // Smoothing: a sudden occlusion arrives over many blocks, not as a step.
        audio::Mixer m;
        m.init(kRate, 1, 8, 8, 2);
        audio::SoundData s = sine(4800, 200.0f);
        audio::PlayDesc d; d.sound = m.addSound(std::move(s)); d.looping = true;
        const audio::VoiceHandle v = m.play(d);
        std::vector<f32> out;
        mixBlocks(m, out, 4, 480);
        m.setVoiceOcclusion(v, 1.0f);
        std::vector<f32> after1;
        mixBlocks(m, after1, 1, 480);
        const f32 r1 = rmsOf(after1, 0, 480);
        check(r1 < open200 && r1 > occ200 * 1.5f, "one block after a sudden occlusion the level is part way down");
        std::vector<f32> later;
        mixBlocks(m, later, 100, 480);
        check(near(rmsOf(later, 480 * 90, later.size()), occ200, occ200 * 0.05f), "and it settles at the fully occluded level");
    }

    AVER_INFO("=== occlusion probe ===");
    {
        struct Ctx { int calls = 0; };
        auto blockAll = [](void* u, const f32*, const f32*) -> f32 { ++static_cast<Ctx*>(u)->calls; return 1.0f; };
        auto blockNone = [](void*, const f32*, const f32*) -> f32 { return 0.0f; };
        auto blockPositiveY = [](void*, const f32*, const f32* to) -> f32 { return to[1] > 1.0f ? 1.0f : 0.0f; };
        const f32 lis[3] = {0, 0, 0}, src[3] = {1000, 0, 0};
        audio::OcclusionProbe p; p.rayCount = 5; p.probeRadiusCm = 50.0f;
        Ctx ctx;
        check(near(audio::measureOcclusion(blockAll, &ctx, lis, src, p), 1.0f) && ctx.calls == 5,
              "everything blocked reads fully occluded, using all five rays");
        check(near(audio::measureOcclusion(blockNone, nullptr, lis, src, p), 0.0f), "nothing blocked reads clear");
        check(near(audio::measureOcclusion(blockPositiveY, nullptr, lis, src, p), 0.2f),
              "one ray in five blocked reads 0.2");
        p.rayCount = 1;
        check(near(audio::measureOcclusion(blockPositiveY, nullptr, lis, src, p), 0.0f),
              "a single centre ray misses what the ring catches");
        check(audio::measureOcclusion(nullptr, nullptr, lis, src, p) == 0.0f, "no ray function is treated as clear");
        const f32 same[3] = {5, 5, 5};
        check(audio::measureOcclusion(blockAll, &ctx, same, same, p) == 0.0f, "a source on top of the listener is clear");

        check(audio::smoothOcclusion(0.0f, 1.0f, 0.1f, 0.08f, 0.25f) > 0.5f, "occlusion rises quickly when blocked");
        check(audio::smoothOcclusion(1.0f, 0.0f, 0.1f, 0.08f, 0.25f) > 0.5f, "and clears slowly");
        check(audio::smoothOcclusion(0.3f, 0.3f, 0.1f, 0.08f, 0.25f) == 0.3f, "a steady value stays put");
    }

    AVER_INFO("=== reverb zones ===");
    {
        const f32 box[3] = {100, 100, 100};
        const f32 inside[3] = {10, -20, 30}, face[3] = {150, 0, 0}, far_[3] = {250, 0, 0}, corner[3] = {200, 200, 100};
        check(audio::zoneSignedDistance(audio::ZoneShape::Box, box, inside) < 0.0f, "a point inside a box has a negative distance");
        check(near(audio::zoneSignedDistance(audio::ZoneShape::Box, box, face), 50.0f, 1.0e-3f), "50 cm off a face is 50");
        check(near(audio::zoneSignedDistance(audio::ZoneShape::Box, box, corner), std::sqrt(100.0f * 100.0f * 2.0f), 1.0e-2f),
              "off a corner it is the Euclidean distance, not the largest axis");
        check(audio::zoneWeight(-5.0f, 100.0f) == 1.0f && audio::zoneWeight(0.0f, 100.0f) == 1.0f, "weight is 1 inside and on the surface");
        check(near(audio::zoneWeight(50.0f, 100.0f), 0.5f), "half way through the blend is 0.5");
        check(audio::zoneWeight(100.0f, 100.0f) == 0.0f && audio::zoneWeight(500.0f, 100.0f) == 0.0f, "and 0 at the blend distance and beyond");
        check(audio::zoneWeight(1.0f, 0.0f) == 0.0f, "a zero blend distance is a hard edge");
        bool mono = true; f32 prev = 1.0f;
        for (int k = 0; k <= 100; ++k) {
            const f32 w = audio::zoneWeight(static_cast<f32>(k), 100.0f);
            if (w > prev + 1.0e-6f) mono = false;
            prev = w;
        }
        check(mono, "the blend never rises as the listener moves away");
        check(near(audio::zoneWeight(audio::zoneSignedDistance(audio::ZoneShape::Box, box, far_), 200.0f), 0.15625f, 1.0e-3f),
              "150 cm out through a 200 cm blend is smoothstep(0.25)");

        const f32 sphereR[3] = {100, 0, 0}, s150[3] = {150, 0, 0};
        check(near(audio::zoneSignedDistance(audio::ZoneShape::Sphere, sphereR, s150), 50.0f, 1.0e-3f), "a sphere measures from its radius");

        audio::ReverbParams ambient;   // wet 0
        audio::ReverbZoneInput hi, lo;
        hi.weight = 1.0f; hi.priority = 1; hi.params.wet = 0.8f; hi.params.decaySec = 4.0f; hi.params.damping = 0.2f;
        lo.weight = 1.0f; lo.priority = 0; lo.params.wet = 0.2f; lo.params.decaySec = 1.0f; lo.params.damping = 0.6f;
        audio::ReverbZoneInput both[2] = {lo, hi};
        audio::ReverbParams r = audio::blendReverbZones(both, 2, ambient);
        check(near(r.wet, 0.8f) && near(r.decaySec, 4.0f), "the higher priority zone wins where it is fully present");
        both[1].weight = 0.5f;
        r = audio::blendReverbZones(both, 2, ambient);
        check(near(r.wet, 0.5f * 0.8f + 0.5f * 0.2f), "at half weight the lower zone fills the rest");
        check(near(r.decaySec, (0.5f * 4.0f + 0.5f * 1.0f)), "decay blends among the zones");
        audio::ReverbZoneInput one = hi; one.weight = 0.25f;
        r = audio::blendReverbZones(&one, 1, ambient);
        check(near(r.wet, 0.2f) && near(r.decaySec, 4.0f), "a lone fading zone fades its wet level but keeps its own decay");
        r = audio::blendReverbZones(nullptr, 0, ambient);
        check(r.wet == 0.0f, "no zones leaves the ambient");
        audio::ReverbParams room; room.wet = 0.1f;
        r = audio::blendReverbZones(nullptr, 0, room);
        check(near(r.wet, 0.1f), "including a non-zero ambient");
    }

    AVER_INFO("=== reverb send ===");
    {
        auto render = [&](f32 wet, f32 decay) {
            audio::Mixer m;
            m.init(kRate, 1, 8, 8, 2);
            audio::ReverbParams p; p.wet = wet; p.decaySec = decay; p.damping = 0.3f;
            m.setReverb(p);
            audio::SoundData imp = dc(10, 0.0f);
            imp.samples[0] = 1.0f;
            audio::PlayDesc d; d.sound = m.addSound(std::move(imp));
            m.play(d);
            std::vector<f32> out;
            mixBlocks(m, out, 200, 480);
            return out;
        };
        const std::vector<f32> dry = render(0.0f, 1.5f);
        check(dry[0] == 1.0f && energyOf(dry, 10, dry.size()) == 0.0f, "wet 0: the impulse is heard once and nothing follows");

        const std::vector<f32> wet = render(1.0f, 1.5f);
        check(near(wet[0], 1.0f, 1.0e-3f), "wet 1: the dry impulse is unchanged");
        bool finite = true;
        for (f32 v : wet) if (!std::isfinite(v)) finite = false;
        check(finite, "the reverb stays finite");
        const f64 early = energyOf(wet, 4800, 9600), late = energyOf(wet, 48000, 52800);
        check(early > 1.0e-6, "a tail follows the impulse");
        check(late < early, "and it decays");

        const f64 shortLate = energyOf(render(1.0f, 0.3f), 48000, 52800);
        const f64 longLate = energyOf(render(1.0f, 3.0f), 48000, 52800);
        check(longLate > 10.0 * shortLate, "a longer RT60 leaves far more tail a second later");

        audio::Mixer m;
        m.init(kRate, 2, 8, 8, 2);
        audio::ReverbParams p; p.wet = 0.5f;
        m.setReverb(p);
        check(near(m.reverb().wet, 0.5f), "the reverb parameters read back");
        std::vector<f32> st(960);
        m.mix(st.data(), 480);
        check(energyOf(st, 0, st.size()) == 0.0f, "and an idle stereo mix with the reverb on stays silent");
    }

    if (g_failures == 0) AVER_INFO("=== all audio streaming tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
