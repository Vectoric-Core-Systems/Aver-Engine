#pragma once
// .ocaudio — the engine's own audio asset, in the AVR1 container: decoded interleaved f32 with the
// loop points and source rate already settled, plus the editor-side import that produces it.
#include "aver/audio/Sound.hpp"
#include "aver/formats/Avr1.hpp"

#include <string>

namespace aver::fmt {

// AVR1 subtype and chunk ids for the format.
inline constexpr u32 kAvrSubtypeAudio = avrFourCC("AUDI");
inline constexpr u32 kOcAudioChunkHeader  = avrFourCC("AHDR");
inline constexpr u32 kOcAudioChunkSamples = avrFourCC("APCM");

// What the AHDR chunk carries. Written field by field, never as a memcpy of this struct.
struct OcAudioInfo {
    u32 channels   = 0;
    u32 sampleRate = 0;
    u32 frames     = 0;
    u32 loopBegin  = 0;
    u32 loopEnd    = 0;      // 0 = to the end
    std::string sourceName;  // where it came from, for display only
};

// Reads and writes .ocaudio. `why` is set on failure and never on success.
bool loadOcAudio(const std::string& path, audio::SoundData& out, std::string* why = nullptr);
bool saveOcAudio(const std::string& path, const audio::SoundData& in, const std::string& sourceName = {},
                 std::string* why = nullptr);
bool parseOcAudio(const u8* bytes, usize size, audio::SoundData& out, std::string* why = nullptr);
bool writeOcAudio(const audio::SoundData& in, const std::string& sourceName, std::vector<u8>& out,
                  std::string* why = nullptr);

// The outcome of an import, and which decoder handled it.
struct AudioImportResult {
    bool ok = false;
    std::string error;
    std::string decoder;   // "wav" or "media foundation"
};

// Decodes any supported source file into memory. WAV goes through this module's reader; everything
// else through the platform media stack, which makes this Windows-only.
AudioImportResult audioImportFile(const std::string& path, audio::SoundData& out);

// True for an extension audioImportFile can read.
bool isImportableAudio(const std::string& path);

// The conventional output path for an imported source: the same directory and stem, with .ocaudio.
std::string ocAudioPathFor(const std::string& sourcePath);

} // namespace aver::fmt
