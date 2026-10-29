#pragma once
// Private to Aver.Audio.Abi: turns a file path into a StreamSource using the decoders the engine
// already has (its WAV reader, the vendored Vorbis/FLAC/MP3 decoders, the .ocaudio container,
// Media Foundation).
#include "aver/audio/Stream.hpp"
#include "aver/formats/AudioDecode.hpp"

#include <memory>
#include <string>

namespace aver::audio {

struct OpenedStream {
    std::unique_ptr<StreamSource> source;
    u32 loopBegin = 0;          // from the file (or the options), when it carries loop points
    u32 loopEnd = 0;
    // "wav", "vorbis", "flac", "mp3", "media foundation" or "resident" (whole file decoded up front)
    std::string decoder;
};

// WAV, Ogg Vorbis, FLAC and MP3 stream incrementally on every platform. Other formats stream
// incrementally through Media Foundation on Windows. .ocaudio holds raw f32 in one AVR1 chunk with
// no incremental reader, so it is decoded whole and replayed from memory; so is anything else on a
// platform with no streaming decoder. `opt` windows the stream (start offset, duration) and sets
// loop points in file frames; null plays the whole file with the loop points the file carries.
OpenedStream openStreamSource(const std::string& utf8Path, std::string* why, const aver::fmt::DecodeOptions* opt = nullptr);

#if defined(_WIN32)
// Media Foundation, read incrementally. Null with *why set when no decoder takes the file.
std::unique_ptr<StreamSource> openMfStream(const std::string& utf8Path, std::string* why);
#endif

} // namespace aver::audio
