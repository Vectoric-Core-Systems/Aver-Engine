#pragma once
// .ocaudio — the engine's own audio asset, in the AVR1 container.
//
// It exists for the reason .ocmesh exists: the runtime should load ONE format it controls, not five
// it does not. A .wav read at load time is a chunk walk and a per-sample conversion; a .mp3 is a
// full decoder. Neither belongs on the path a game takes when it opens a door, and the second is not
// something a shipping build should be doing at all.
//
// So the import happens once, in the editor, and what ships is decoded interleaved f32 with the
// loop points and the source rate already settled. Same shape as the glTF -> .ocmesh chain.
#include "aver/audio/Sound.hpp"
#include "aver/formats/Avr1.hpp"

#include <string>

namespace aver::fmt {

// AVR1 subtype and chunk ids. Distinct four-character codes, so a truncated or mislabelled file is
// refused by the container rather than reinterpreted by a reader that assumed.
inline constexpr u32 kAvrSubtypeAudio = avrFourCC("AUDI");
inline constexpr u32 kOcAudioChunkHeader  = avrFourCC("AHDR");
inline constexpr u32 kOcAudioChunkSamples = avrFourCC("APCM");

// What the header chunk carries. Written field by field rather than as a memcpy of this struct: a
// struct written whole is a struct whose padding is part of the file format, and the day somebody
// adds a field in the middle every asset already on disk becomes silently wrong.
struct OcAudioInfo {
    u32 channels   = 0;
    u32 sampleRate = 0;
    u32 frames     = 0;
    u32 loopBegin  = 0;
    u32 loopEnd    = 0;      // 0 = to the end
    // Where it came from, purely so the editor can say so. Never used to decide anything.
    std::string sourceName;
};

// Read / write. `why` is always set on failure and never on success, matching every other loader here.
bool loadOcAudio(const std::string& path, audio::SoundData& out, std::string* why = nullptr);
bool saveOcAudio(const std::string& path, const audio::SoundData& in, const std::string& sourceName = {},
                 std::string* why = nullptr);
bool parseOcAudio(const u8* bytes, usize size, audio::SoundData& out, std::string* why = nullptr);
bool writeOcAudio(const audio::SoundData& in, const std::string& sourceName, std::vector<u8>& out,
                  std::string* why = nullptr);

// ---------------------------------------------------------------- import
//
// Decode ANY supported source file into memory. WAV goes through this module's own reader, which
// depends on nothing; everything else goes through the platform's media stack, which on Windows
// means Media Foundation and covers mp3, m4a/aac, wma and flac with no vendored decoder at all.
//
// That choice is the same one the audio backend made: an MP3 decoder is two thousand lines of
// Huffman, IMDCT and polyphase synthesis, and the operating system already ships a correct one.
// It is also the choice that makes this Windows-only, which is honest -- the engine's renderer and
// its audio device already are.
struct AudioImportResult {
    bool ok = false;
    std::string error;
    std::string decoder;   // which path handled it, for the log: "wav" or "media foundation"
};

AudioImportResult audioImportFile(const std::string& path, audio::SoundData& out);

// True for an extension this can import. Used by the content browser to decide what to offer, so it
// has ONE owner rather than a list repeated at every call site.
bool isImportableAudio(const std::string& path);

// The conventional output path for an imported source: the same directory and stem, with .ocaudio.
std::string ocAudioPathFor(const std::string& sourcePath);

} // namespace aver::fmt
