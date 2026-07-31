// Hand-run probe: opens the real default output device and plays generated tones through it.
// Not part of the headless suite -- it needs a sound card and it makes a noise.
//     AudioProbe.exe              a short arpeggio, then a pan sweep
//     AudioProbe.exe --silent     the same run with the master at zero
#include "aver/audio/AudioDevice.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

using namespace aver;

namespace {

// Generates a mono tone with a short attack and an exponential decay.
audio::SoundData tone(u32 rate, f32 hz, f32 seconds, f32 amplitude) {
    const u32 frames = static_cast<u32>(seconds * static_cast<f32>(rate));
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = rate;
    d.samples.resize(frames);
    const u32 attack = rate / 200;   // 5 ms
    for (u32 i = 0; i < frames; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(rate);
        f32 env = 1.0f;
        if (i < attack) env = static_cast<f32>(i) / static_cast<f32>(attack);
        else env = std::exp(-3.0f * (t - static_cast<f32>(attack) / static_cast<f32>(rate)));
        d.samples[i] = amplitude * env * std::sin(6.28318530718f * hz * t);
    }
    return d;
}

// Sleeps the calling thread for a number of milliseconds.
void wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

// Runs the probe. Returns 0 unless the device reported an underrun.
int main(int argc, char** argv) {
    bool silent = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--silent")) silent = true;

    audio::AudioDevice dev;
    if (!dev.start()) {
        AVER_WARN("=== no output device on this machine; the engine would run silent ===");
        return 0;
    }

    const u32 rate = dev.sampleRate();
    AVER_INFO("device: {} Hz, {} channel(s), {} frame buffer ({:.1f} ms latency)",
              rate, dev.channels(), dev.bufferFrames(),
              1000.0 * double(dev.bufferFrames()) / double(rate));

    audio::Mixer& mix = dev.mixer();
    if (silent) { mix.setMasterVolume(0.0f); AVER_INFO("master at zero: this run makes no sound"); }

    // A440, C#5, E5, A5 -- a major triad.
    const f32 notes[4] = {440.0f, 554.37f, 659.26f, 880.0f};
    audio::SoundHandle handles[4] = {};
    for (int i = 0; i < 4; ++i) handles[i] = mix.addSound(tone(rate, notes[i], 0.9f, 0.35f));

    AVER_INFO("--- arpeggio: four overlapping voices ---");
    for (int i = 0; i < 4; ++i) {
        audio::PlayDesc p; p.sound = handles[i];
        if (mix.play(p) == 0) AVER_ERROR("voice {} did not start", i);
        wait(220);
    }
    AVER_INFO("    {} voices active at the end of it", mix.activeVoices());
    wait(900);

    AVER_INFO("--- pan sweep: left to right, 2 seconds ---");
    audio::SoundHandle drone = mix.addSound(tone(rate, 220.0f, 3.0f, 0.30f));
    audio::PlayDesc p;
    p.sound = drone;
    p.positional = true;
    p.attenuation = {200.0f, 5000.0f};
    p.position[1] = -600.0f;          // -Y is the listener's LEFT
    const audio::VoiceHandle v = mix.play(p);
    for (int step = 0; step <= 40; ++step) {
        const f32 y = -600.0f + 1200.0f * (static_cast<f32>(step) / 40.0f);
        mix.setVoicePosition(v, 0.0f, y, 0.0f);
        wait(50);
    }
    mix.stop(v);
    wait(200);

    AVER_INFO("--- distance: near to the outer radius, 1.5 seconds ---");
    p.position[1] = 0.0f;
    const audio::VoiceHandle far = mix.play(p);
    for (int step = 0; step <= 30; ++step) {
        const f32 x = 200.0f + 4800.0f * (static_cast<f32>(step) / 30.0f);
        mix.setVoicePosition(far, x, 0.0f, 0.0f);
        wait(50);
    }
    mix.stopAll();
    wait(200);

    const u32 under = dev.underruns();
    AVER_INFO("=== finished: {} underrun(s), {} voice(s) stolen ===", under, mix.stolenVoices());
    if (under != 0)
        AVER_WARN("an underrun is an audible gap. On an idle machine it is a bug, not load.");

    dev.stop();
    return under == 0 ? 0 : 1;
}
