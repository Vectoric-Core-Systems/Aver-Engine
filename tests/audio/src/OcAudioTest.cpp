// .ocaudio: the container round-trip, and the importer against whatever this machine actually has.
//
// The round-trip needs no decoder and no device, so it runs in the headless suite. The IMPORT half
// needs real files, so it looks for them rather than shipping any -- Windows carries .wav files in
// C:\Windows\Media, and if the machine has an .mp3 anywhere obvious that path gets exercised too.
// A missing file is reported as "not exercised", never as a pass.
#include "aver/formats/OcAudio.hpp"
#include "aver/formats/Wav.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;
static int g_skipped  = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static audio::SoundData makeSound(u32 channels, u32 rate, u32 frames) {
    audio::SoundData d;
    d.channels = channels;
    d.sampleRate = rate;
    d.samples.resize(static_cast<usize>(frames) * channels);
    for (u32 f = 0; f < frames; ++f)
        for (u32 c = 0; c < channels; ++c)
            // A different waveform per channel, so a writer that interleaves wrongly cannot pass by
            // producing something that merely has the right length.
            d.samples[f * channels + c] =
                std::sin(0.01f * static_cast<f32>(f) * static_cast<f32>(c + 1)) * 0.5f;
    return d;
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "aver-ocaudio-test";
    std::error_code ec;
    fs::create_directories(tmp, ec);

    AVER_INFO("=== .ocaudio round-trip ===");
    {
        audio::SoundData in = makeSound(2, 44100, 5000);
        in.loopBegin = 1000;
        in.loopEnd   = 4000;

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcAudio(in, "T_Test.wav", bytes, &why), "it writes");
        check(bytes.size() > 5000 * 2 * 4, "and the payload is in there, not just a header");

        audio::SoundData out;
        check(fmt::parseOcAudio(bytes.data(), bytes.size(), out, &why), "it reads back");
        check(out.channels == 2, "channels survive");
        check(out.sampleRate == 44100, "the SOURCE rate survives, unconverted");
        check(out.frames() == 5000, "the frame count survives");
        check(out.loopBegin == 1000 && out.loopEnd == 4000, "and so do the loop points");

        // Bit-exact, not approximately. The payload is f32 in and f32 out with nothing in between,
        // so anything less than exact means a conversion nobody asked for.
        bool exact = out.samples.size() == in.samples.size();
        if (exact)
            for (usize i = 0; i < in.samples.size(); ++i)
                if (out.samples[i] != in.samples[i]) { exact = false; break; }
        check(exact, "every sample comes back BIT-EXACT");

        const fs::path file = tmp / "round.ocaudio";
        check(fmt::saveOcAudio(file.string(), in, "T_Test.wav", &why), "it saves to disk");
        audio::SoundData fromDisk;
        check(fmt::loadOcAudio(file.string(), fromDisk, &why), "and loads back off it");
        check(fromDisk.frames() == 5000 && fromDisk.channels == 2, "with the same shape");
    }

    AVER_INFO("=== refusals ===");
    {
        std::string why;
        std::vector<u8> bytes;
        check(!fmt::writeOcAudio(audio::SoundData{}, "", bytes, &why), "empty sound data is refused");
        check(!why.empty(), "and says why");

        audio::SoundData out;
        const u8 junk[64] = {};
        check(!fmt::parseOcAudio(junk, sizeof(junk), out, &why), "a buffer of zeroes is not an .ocaudio");

        // A container of the right shape but the WRONG SUBTYPE must be refused rather than read as
        // audio -- the chunks would be missing and the header would be garbage.
        audio::SoundData ok = makeSound(1, 48000, 100);
        std::vector<u8> good;
        fmt::writeOcAudio(ok, "", good, &why);
        fmt::Avr1File f;
        check(fmt::parseAvr1(good.data(), good.size(), f, &why), "the good file parses as a container");
        check(f.subtype == fmt::kAvrSubtypeAudio, "and is marked AUDI");

        // Truncating the payload must be caught by the header/payload cross-check, not read past.
        std::vector<u8> chopped = good;
        chopped.resize(chopped.size() - 64);
        check(!fmt::parseOcAudio(chopped.data(), chopped.size(), out, &why), "a truncated file is refused");
    }

    AVER_INFO("=== extensions and paths ===");
    {
        check(fmt::isImportableAudio("a/b/c.wav"), ".wav is importable");
        check(fmt::isImportableAudio("SHOUT.MP3"), "and the check is case-insensitive");
        check(fmt::isImportableAudio("x.flac") && fmt::isImportableAudio("x.m4a"), "flac and m4a too");
        check(!fmt::isImportableAudio("x.png"), "a texture is not");
        check(!fmt::isImportableAudio("noextension"), "and neither is a file with no extension");

        check(fmt::ocAudioPathFor("Content/Sounds/shot.wav") == "Content/Sounds/shot.ocaudio",
              "the output path keeps the stem and swaps the extension");
        // A dot in a DIRECTORY name is not an extension. Getting this wrong writes the output into a
        // truncated path, which fails somewhere unrelated.
        check(fmt::ocAudioPathFor("My.Game/Sounds/shot") == "My.Game/Sounds/shot.ocaudio",
              "a dot in a directory name is not treated as an extension");
    }

    AVER_INFO("=== importing what this machine has ===");
    {
        // Windows ships these. Looked for rather than shipped, because a test that carries a media
        // file is a test that carries somebody's licence with it.
        const char* candidates[] = {
            "C:/Windows/Media/Windows Background.wav",
            "C:/Windows/Media/Windows Notify System Generic.wav",
            "C:/Windows/Media/chimes.wav",
            "C:/Windows/Media/tada.wav",
        };
        std::string found;
        for (const char* c : candidates) if (fs::exists(c)) { found = c; break; }

        if (found.empty()) {
            ++g_skipped;
            AVER_WARN("  skip  no system .wav found; the import path was NOT exercised");
        } else {
            audio::SoundData d;
            const fmt::AudioImportResult r = fmt::audioImportFile(found, d);
            check(r.ok, "a real .wav imports (" + found + ")");
            if (r.ok) {
                check(r.decoder == "wav", "through this module's own reader, not the platform's");
                check(d.channels >= 1 && d.sampleRate >= 8000, "with a sane format");
                check(d.frames() > 0, "and actual audio in it");

                // The whole point of the chain: import once, then load the cooked asset.
                const fs::path out = tmp / "system.ocaudio";
                std::string why;
                check(fmt::saveOcAudio(out.string(), d, found, &why), "and cooks to .ocaudio");
                audio::SoundData back;
                check(fmt::loadOcAudio(out.string(), back, &why), "which loads back");
                check(back.frames() == d.frames() && back.sampleRate == d.sampleRate,
                      "identical in shape to what was imported");
            }
        }

        // The Media Foundation path, exercised only against a file NAMED ON THE COMMAND LINE.
        //
        // An earlier version of this went looking through the user's Music and Documents folders for
        // something compressed. It worked, and it was the wrong thing for a test to do: a suite that
        // walks personal directories on every run is reading files it was never asked to read, and
        // it makes the result depend on what happens to be lying about. Naming the file is explicit,
        // repeatable, and somebody's decision.
        //
        //     OcAudioTest.exe "C:/path/to/something.mp3"
        std::string compressed = argc > 1 ? argv[1] : std::string{};
        if (compressed.empty()) {
            ++g_skipped;
            AVER_WARN("  skip  Media Foundation NOT exercised: pass a .mp3/.m4a/.flac/.wma path to do it");
        } else if (!fs::exists(compressed)) {
            ++g_failures;
            AVER_ERROR("  FAIL  the file named on the command line does not exist: {}", compressed);
        } else {
            audio::SoundData d;
            const fmt::AudioImportResult r = fmt::audioImportFile(compressed, d);
            check(r.ok, "a compressed file imports (" + compressed + ")");
            if (r.ok) {
                check(r.decoder == "media foundation", "through the platform decoder");
                check(d.frames() > 0 && d.channels >= 1, "and yields real audio");
                check(d.samples.size() % d.channels == 0, "with no trailing partial frame");
            }
        }
    }

    fs::remove_all(tmp, ec);

    if (g_failures == 0 && g_skipped == 0) AVER_INFO("=== all .ocaudio tests passed ===");
    else if (g_failures == 0)              AVER_WARN("=== passed, but {} check(s) were SKIPPED for want of a file ===", g_skipped);
    else                                   AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
