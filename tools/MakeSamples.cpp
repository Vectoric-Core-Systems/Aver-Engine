// Synthesises four short placeholder sounds as 16-bit mono .wav, for a template project to play.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

namespace {

constexpr u32 kRate = 44100;

// A tiny xorshift PRNG, so the output is byte-identical on every machine.
struct Rng {
    u32 s = 0x9E3779B9u;
    // The next sample.
    f32 next() {   // -1..1
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<f32>(static_cast<i32>(s >> 8)) / 8388608.0f - 1.0f;
    }
};

// Writes mono samples as a 16-bit PCM WAV. False if the file could not be opened.
bool writeWav(const std::string& path, const std::vector<f32>& mono, u32 rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const u32 dataBytes = static_cast<u32>(mono.size()) * 2u;
    const u32 riffSize  = 36u + dataBytes;
    const auto u32le = [&](u32 v) { std::fputc(int(v & 0xFF), f); std::fputc(int((v >> 8) & 0xFF), f);
                                    std::fputc(int((v >> 16) & 0xFF), f); std::fputc(int((v >> 24) & 0xFF), f); };
    const auto u16le = [&](u16 v) { std::fputc(int(v & 0xFF), f); std::fputc(int((v >> 8) & 0xFF), f); };

    std::fwrite("RIFF", 1, 4, f); u32le(riffSize); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32le(16); u16le(1); u16le(1);
    u32le(rate); u32le(rate * 2u); u16le(2); u16le(16);
    std::fwrite("data", 1, 4, f); u32le(dataBytes);
    for (const f32 v : mono) {
        const f32 c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        u16le(static_cast<u16>(static_cast<i16>(c * 32767.0f)));
    }
    std::fclose(f);
    return true;
}

// A gunshot: filtered noise with a fast crack and a slower body.
std::vector<f32> makeShot() {
    const u32 n = kRate / 4;   // 250 ms
    std::vector<f32> out(n);
    Rng rng;
    f32 lp = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kRate);
        const f32 crack = std::exp(-t * 90.0f);
        const f32 body  = std::exp(-t * 14.0f);
        const f32 white = rng.next();
        lp += (white - lp) * 0.35f;
        out[i] = 0.85f * (white * crack * 0.8f + lp * body * 0.6f);
    }
    return out;
}

// An impact: a sine that falls in pitch as it decays.
std::vector<f32> makeImpact() {
    const u32 n = kRate / 5;   // 200 ms
    std::vector<f32> out(n);
    f32 phase = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kRate);
        const f32 hz = 180.0f * std::exp(-t * 9.0f) + 55.0f;
        phase += 6.2831853f * hz / static_cast<f32>(kRate);
        out[i] = 0.7f * std::sin(phase) * std::exp(-t * 11.0f);
    }
    return out;
}

// A footstep: a short quiet scuff of band-limited noise.
std::vector<f32> makeStep() {
    const u32 n = kRate / 12;   // ~83 ms
    std::vector<f32> out(n);
    Rng rng{0x1234567u};
    f32 lp = 0.0f, hp = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kRate);
        const f32 white = rng.next();
        lp += (white - lp) * 0.5f;
        hp = lp - hp * 0.02f;
        out[i] = 0.35f * hp * std::exp(-t * 38.0f);
    }
    return out;
}

// A success sting: two notes, a rising fifth.
std::vector<f32> makeCleared() {
    const u32 n = kRate * 3 / 4;   // 750 ms
    std::vector<f32> out(n);
    const f32 hzA = 440.0f, hzB = 659.25f;
    f32 pa = 0.0f, pb = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kRate);
        pa += 6.2831853f * hzA / static_cast<f32>(kRate);
        pb += 6.2831853f * hzB / static_cast<f32>(kRate);
        const f32 a = std::sin(pa) * std::exp(-t * 4.5f);
        const f32 b = t > 0.12f ? std::sin(pb) * std::exp(-(t - 0.12f) * 4.0f) : 0.0f;
        out[i] = 0.42f * (a + b) * 0.5f;
    }
    return out;
}

} // namespace

// Writes every sample into the directory named on the command line. Returns the failure count.
int main(int argc, char** argv) {
    if (argc < 2) {
        AVER_INFO("usage: MakeSamples <output-directory>");
        AVER_INFO("  writes shot.wav, impact.wav, step.wav and cleared.wav");
        return exitCode(ExitCode::Usage);
    }
    const std::string dir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // One output file and the generator that fills it.
    struct Item { const char* name; std::vector<f32> (*make)(); };
    const Item items[] = {
        {"shot.wav",    &makeShot},
        {"impact.wav",  &makeImpact},
        {"step.wav",    &makeStep},
        {"cleared.wav", &makeCleared},
    };

    int failures = 0;
    for (const Item& it : items) {
        const std::vector<f32> pcm = it.make();
        const std::string path = dir + "/" + it.name;
        if (writeWav(path, pcm, kRate))
            AVER_INFO("[MakeSamples] {} ({} frames, {:.2f} s)", path, pcm.size(),
                      static_cast<f64>(pcm.size()) / static_cast<f64>(kRate));
        else { AVER_ERROR("[MakeSamples] could not write {}", path); ++failures; }
    }
    // A COUNT IS NOT AN EXIT CODE (core/ErrorCodes.hpp). This returned `failures` directly, so
    // four unwritable files exited 4 -- a value the shared table spells Interrupted, and 256 of
    // them would have exited 0. Report the number in the log, where a number belongs, and exit
    // with a code that means exactly one thing.
    if (failures) AVER_ERROR("[MakeSamples] {} of {} sample(s) could not be written",
                             failures, static_cast<int>(std::size(items)));
    return exitCode(failures ? ExitCode::Failed : ExitCode::Ok);
}
