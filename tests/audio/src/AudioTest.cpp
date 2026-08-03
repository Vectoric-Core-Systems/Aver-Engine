// Headless test for the audio mixer: attenuation, pan, voices, looping, resampling, buses, limits.
// Everything here is decidable from the samples the mixer produces, so it needs no sound card.
#include "aver/audio/Mixer.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// True when two floats agree to within eps.
static bool near(f32 a, f32 b, f32 eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

// A mono sine at `hz`, `frames` long, at the mixer's own rate so nothing resamples it.
static audio::SoundData sine(u32 rate, u32 frames, f32 hz, f32 amplitude = 1.0f) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = rate;
    d.samples.resize(frames);
    for (u32 i = 0; i < frames; ++i)
        d.samples[i] = amplitude * std::sin(6.28318530718f * hz * static_cast<f32>(i) / static_cast<f32>(rate));
    return d;
}

// A mono constant, so whatever comes out of the mixer IS the gain.
static audio::SoundData dc(u32 rate, u32 frames, f32 value) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = rate;
    d.samples.assign(frames, value);
    return d;
}

// The largest absolute sample on one interleaved channel.
static f32 peak(const std::vector<f32>& v, u32 stride, u32 offset) {
    f32 m = 0.0f;
    for (usize i = offset; i < v.size(); i += stride) m = std::fmax(m, std::fabs(v[i]));
    return m;
}

// The root-mean-square of one interleaved channel.
static f32 rms(const std::vector<f32>& v, u32 stride, u32 offset) {
    f64 sum = 0.0; u32 n = 0;
    for (usize i = offset; i < v.size(); i += stride) { sum += double(v[i]) * v[i]; ++n; }
    return n ? static_cast<f32>(std::sqrt(sum / n)) : 0.0f;
}

// Runs the suite. Returns 0 when every check passed.
int main() {
    constexpr u32 kRate = 48000;

    AVER_INFO("=== attenuation ===");
    {
        audio::Attenuation a{100.0f, 2000.0f};
        check(attenuationAt(a, 0.0f) == 1.0f, "silent-close is full volume, exactly");
        check(attenuationAt(a, 100.0f) == 1.0f, "exactly 1.0 AT the inner radius");
        check(attenuationAt(a, 2000.0f) == 0.0f, "exactly 0.0 AT the outer radius");
        check(attenuationAt(a, 5000.0f) == 0.0f, "and beyond it");

        // Swept at 1 cm, because the curve is steepest right at the inner radius.
        f32 prev = 1.0f, biggestStep = 0.0f;
        bool monotone = true;
        for (u32 d = 100; d <= 2000; ++d) {
            const f32 g = attenuationAt(a, static_cast<f32>(d));
            if (g > prev + 1.0e-6f) monotone = false;
            biggestStep = std::fmax(biggestStep, std::fabs(g - prev));
            prev = g;
        }
        check(monotone, "the curve never rises as distance grows");
        check(biggestStep < 0.02f, "and never steps: the largest 1 cm change is under 2%");
        check(attenuationAt(a, 1050.0f) < 0.5f, "inverse-distance, not linear: half way out is under half volume");

        audio::Attenuation bad{500.0f, 100.0f};
        const f32 g = attenuationAt(bad, 300.0f);
        check(g >= 0.0f && g <= 1.0f, "an outer radius inside the inner one still yields a sane gain");
    }

    AVER_INFO("=== constant-power pan ===");
    {
        f32 l = 0, r = 0;
        audio::panGains(0.0f, l, r);
        check(near(l, r), "centre is equal in both ears");
        check(near(l * l + r * r, 1.0f), "and holds unit power");

        audio::panGains(-1.0f, l, r);
        check(near(l, 1.0f) && near(r, 0.0f, 1.0e-3f), "hard left is all left");
        audio::panGains(1.0f, l, r);
        check(near(l, 0.0f, 1.0e-3f) && near(r, 1.0f), "hard right is all right");

        bool power = true;
        for (int i = -20; i <= 20; ++i) {
            audio::panGains(static_cast<f32>(i) / 20.0f, l, r);
            if (!near(l * l + r * r, 1.0f, 1.0e-3f)) power = false;
        }
        check(power, "power is constant across the whole sweep, not just at the ends");
        audio::panGains(9.0f, l, r);
        check(near(r, 1.0f), "a pan past hard right stays hard right");
    }

    AVER_INFO("=== a voice comes out the other side ===");
    {
        audio::Mixer m;
        check(m.init(kRate, 2, 8, 16), "the mixer initialises");

        const audio::SoundHandle s = m.addSound(sine(kRate, kRate, 440.0f, 0.5f));
        check(s != 0, "a sound is registered");
        check(m.soundCount() == 1, "and shows up in the table");

        audio::PlayDesc d; d.sound = s; d.volume = 1.0f;
        const audio::VoiceHandle v = m.play(d);
        check(v != 0, "it plays");
        check(m.playing(v), "and reports playing");
        check(m.activeVoices() == 1, "one active voice");

        std::vector<f32> buf(512 * 2);
        m.mix(buf.data(), 512);

        check(near(peak(buf, 2, 0), 0.5f, 0.02f), "left peak is the authored amplitude");
        check(near(rms(buf, 2, 0), 0.5f / 1.41421356f, 0.02f), "and the RMS of a sine, not of a square");
        check(near(rms(buf, 2, 0), rms(buf, 2, 1)), "a non-positional voice is centred");

        // 440 Hz over 512 frames at 48 kHz is 4.69 cycles, so 9 or 10 crossings.
        u32 crossings = 0;
        for (u32 i = 1; i < 512; ++i)
            if ((buf[(i - 1) * 2] < 0.0f) != (buf[i * 2] < 0.0f)) ++crossings;
        check(crossings == 9 || crossings == 10, "the frequency survives: 9-10 zero crossings in 512 frames");
    }

    AVER_INFO("=== the handle goes stale ===");
    {
        audio::Mixer m;
        m.init(kRate, 2, 4, 8);
        const audio::SoundHandle s = m.addSound(dc(kRate, 100, 1.0f));   // 100 frames, then done
        audio::PlayDesc d; d.sound = s;
        const audio::VoiceHandle v = m.play(d);
        check(m.playing(v), "playing before it ends");

        std::vector<f32> buf(256 * 2);
        m.mix(buf.data(), 256);   // longer than the sound
        check(!m.playing(v), "not playing after it ends");
        check(m.activeVoices() == 0, "the slot came back");

        const audio::VoiceHandle v2 = m.play(d);
        check(v2 != 0 && v2 != v, "a reused slot issues a DIFFERENT handle");
        check(!m.playing(v), "and the old handle is still dead");
        check(m.playing(v2), "while the new one is live");
        m.setVoiceVolume(v, 0.0f);
        check(m.playing(v2), "writing through a stale handle does not touch the live voice");
    }

    AVER_INFO("=== looping is sample-exact ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 4, 8);
        // Seven is coprime with every block size below, so a loop that drifts shows up.
        audio::SoundData d;
        d.channels = 1; d.sampleRate = kRate;
        d.samples = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
        const audio::SoundHandle s = m.addSound(std::move(d));

        audio::PlayDesc p; p.sound = s; p.looping = true;
        const audio::VoiceHandle v = m.play(p);
        check(v != 0, "a looping voice starts");

        std::vector<f32> buf(70);
        m.mix(buf.data(), 70);   // exactly ten passes
        bool exact = true;
        for (u32 i = 0; i < 70; ++i)
            if (!near(buf[i], 0.1f * static_cast<f32>(i % 7 + 1), 1.0e-3f)) exact = false;
        check(exact, "ten passes of a 7-frame loop reproduce the source exactly");
        check(m.playing(v), "and it is still going");

        std::vector<f32> b2(11);
        m.mix(b2.data(), 11);
        bool contiguous = true;
        for (u32 i = 0; i < 11; ++i)
            if (!near(b2[i], 0.1f * static_cast<f32>((70 + i) % 7 + 1), 1.0e-3f)) contiguous = false;
        check(contiguous, "and the next block continues where the last one stopped");
    }

    AVER_INFO("=== resampling and pitch ===");
    {
        audio::Mixer m;
        m.init(48000, 1, 4, 8);
        // A 24 kHz source in a 48 kHz mixer: 100 source frames must last 200 output frames.
        audio::SoundData d = dc(24000, 100, 1.0f);
        const audio::SoundHandle s = m.addSound(std::move(d));
        audio::PlayDesc p; p.sound = s;
        const audio::VoiceHandle v = m.play(p);

        std::vector<f32> buf(150);
        m.mix(buf.data(), 150);
        check(m.playing(v), "a half-rate source is still playing after 150 output frames");
        std::vector<f32> b2(80);
        m.mix(b2.data(), 80);
        check(!m.playing(v), "and has ended by 230, which is 200 plus a little");

        const audio::VoiceHandle v2 = m.play([&]{ audio::PlayDesc q; q.sound = s; q.pitch = 2.0f; return q; }());
        std::vector<f32> b3(120);
        m.mix(b3.data(), 120);
        check(!m.playing(v2), "at pitch 2.0 the same source ends in half the frames");
    }

    AVER_INFO("=== 3D placement ===");
    {
        audio::Mixer m;
        m.init(kRate, 2, 8, 8);
        const audio::SoundHandle s = m.addSound(dc(kRate, kRate, 1.0f));

        audio::Listener l;   // at the origin, +X forward, +Y right
        m.setListener(l);

        audio::PlayDesc p;
        p.sound = s; p.positional = true; p.volume = 1.0f;
        p.position[1] = 50.0f;
        p.attenuation = {100.0f, 2000.0f};
        const audio::VoiceHandle v = m.play(p);

        std::vector<f32> buf(64 * 2);
        m.mix(buf.data(), 64);
        check(rms(buf, 2, 1) > rms(buf, 2, 0) * 4.0f, "a source on +Y is much louder in the RIGHT ear");
        m.stop(v);
        m.mix(buf.data(), 64);

        p.position[1] = -50.0f;
        m.play(p);
        m.mix(buf.data(), 64);
        check(rms(buf, 2, 0) > rms(buf, 2, 1) * 4.0f, "a source on -Y is much louder in the LEFT ear");
        m.stopAll();
        m.mix(buf.data(), 64);

        // FRONT VS BACK at the same distance. Both pan dead centre -- dot(right, d) is zero for
        // each -- so if the listener's forward vector were ignored these two would be identical,
        // which is what they used to be.
        m.stopAll();
        m.mix(buf.data(), 64);
        p.position[1] = 0.0f;
        p.position[0] = 300.0f;              // straight ahead: +X is the listener's forward
        m.play(p);
        m.mix(buf.data(), 64);
        const f32 front = rms(buf, 2, 0) + rms(buf, 2, 1);
        m.stopAll();
        m.mix(buf.data(), 64);
        p.position[0] = -300.0f;             // straight behind, same distance
        m.play(p);
        m.mix(buf.data(), 64);
        const f32 back = rms(buf, 2, 0) + rms(buf, 2, 1);
        check(front > back * 1.2f,
              "a source BEHIND the listener is quieter than the same source in front");
        check(back > front * 0.5f,
              "but only by a cue, not by a wall - behind is attenuated, not muted");

        m.stopAll();
        m.mix(buf.data(), 64);
        p.position[1] = 0.0f;
        p.position[0] = 9000.0f;
        m.play(p);
        m.mix(buf.data(), 64);
        check(peak(buf, 1, 0) < 1.0e-5f, "a source past the outer radius contributes nothing");
    }

    AVER_INFO("=== voice stealing ===");
    {
        audio::Mixer m;
        m.init(kRate, 2, 2, 8);   // two voices only
        const audio::SoundHandle s = m.addSound(dc(kRate, kRate, 1.0f));

        audio::PlayDesc loud; loud.sound = s; loud.volume = 1.0f;
        audio::PlayDesc quiet; quiet.sound = s; quiet.volume = 0.05f;
        const audio::VoiceHandle a = m.play(loud);
        const audio::VoiceHandle b = m.play(quiet);
        check(a && b && m.activeVoices() == 2, "the pool fills");
        check(m.stolenVoices() == 0, "nothing stolen yet");

        const audio::VoiceHandle c = m.play(loud);
        check(c != 0, "a third play still succeeds");
        check(m.stolenVoices() == 1, "by stealing");
        check(!m.playing(b), "and it took the QUIETEST voice");
        check(m.playing(a), "leaving the loud one alone");
    }

    AVER_INFO("=== stopping does not click ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 4, 8);
        const audio::SoundHandle s = m.addSound(dc(kRate, kRate, 1.0f));
        audio::PlayDesc p; p.sound = s;
        const audio::VoiceHandle v = m.play(p);

        std::vector<f32> buf(64);
        m.mix(buf.data(), 64);
        check(near(buf[0], 1.0f, 0.01f), "the first block starts at full level, not faded in");

        m.stop(v);
        m.mix(buf.data(), 64);
        check(buf[0] > 0.5f, "a stopped voice is still audible at the start of its last block");
        check(std::fabs(buf[63]) < 0.05f, "and has ramped to silence by the end of it");
        check(!m.playing(v), "then the slot is released");
    }

    AVER_INFO("=== buses and master ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 4, 8);
        const audio::SoundHandle s = m.addSound(dc(kRate, kRate, 1.0f));
        audio::PlayDesc p; p.sound = s; p.bus = audio::Bus::Music; p.volume = 1.0f;
        m.play(p);

        std::vector<f32> buf(32);
        m.mix(buf.data(), 32);
        check(near(buf[16], 1.0f, 0.01f), "full scale through an untouched bus");

        m.setBusVolume(audio::Bus::Music, 0.25f);
        m.mix(buf.data(), 32);
        check(near(buf[31], 0.25f, 0.02f), "the bus scales it");

        m.setBusVolume(audio::Bus::Sfx, 0.0f);
        m.mix(buf.data(), 32);
        check(near(buf[31], 0.25f, 0.02f), "and a DIFFERENT bus does not");

        m.setMasterVolume(0.5f);
        m.mix(buf.data(), 32);
        check(near(buf[31], 0.125f, 0.02f), "master multiplies on top");
    }

    AVER_INFO("=== clipping is hard, and silence is silent ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 8, 8);
        const audio::SoundHandle s = m.addSound(dc(kRate, kRate, 1.0f));
        audio::PlayDesc p; p.sound = s;
        for (int i = 0; i < 4; ++i) m.play(p);   // 4x full scale

        std::vector<f32> buf(32);
        m.mix(buf.data(), 32);
        check(near(buf[31], 1.0f, 1.0e-6f), "four full-scale voices clamp to 1.0 rather than wrapping");

        audio::Mixer dead;
        std::vector<f32> junk(32, 0.9f);
        dead.mix(junk.data(), 32);
        check(peak(junk, 1, 0) == 0.0f, "an uninitialised mixer writes silence over the caller's buffer");
        check(dead.starvedFrames() == 32, "and counts what it could not render");
    }

    AVER_INFO("=== sound lifetime ===");
    {
        audio::Mixer m;
        m.init(kRate, 1, 4, 8);
        const audio::SoundHandle s = m.addSound(dc(kRate, 200, 1.0f));
        audio::PlayDesc p; p.sound = s;
        const audio::VoiceHandle v = m.play(p);
        check(m.soundCount() == 1, "one sound registered");

        m.removeSound(s);
        check(m.soundCount() == 1, "a retired sound is NOT freed while a voice still reads it");
        check(m.play(p) == 0, "and no new voice may start on it");

        std::vector<f32> buf(300);
        m.mix(buf.data(), 300);   // the voice runs out
        check(!m.playing(v), "the voice ends");
        m.collect();
        check(m.soundCount() == 0, "and then collect() reclaims it");
    }

    AVER_INFO("=== refusals ===");
    {
        audio::Mixer m;
        check(!m.init(kRate, 5), "5 channels is refused, not silently downmixed");
        check(!m.init(0, 2), "a zero sample rate is refused");
        check(m.init(kRate, 2), "stereo is accepted");
        check(m.addSound(audio::SoundData{}) == 0, "empty sound data is refused");
        audio::PlayDesc p; p.sound = 999;
        check(m.play(p) == 0, "playing an unknown sound yields no voice");
        check(!m.playing(0), "handle 0 is never playing");
        m.stop(0); m.setVoiceVolume(0, 1.0f);
        check(true, "and the null handle is safe to pass to every setter");
    }

    if (g_failures == 0) AVER_INFO("=== all audio mixer tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
