#pragma once
// Private to Aver.Audio.Abi: turns a file path into a StreamSource using the decoders the engine
// already has (its WAV reader, the .ocaudio container, Media Foundation).
#include "aver/audio/Stream.hpp"

#include <memory>
#include <string>

namespace aver::audio {

struct OpenedStream {
    std::unique_ptr<StreamSource> source;
    u32 loopBegin = 0;          // from the file, when it carries loop points
    u32 loopEnd = 0;
    std::string decoder;        // "wav", "media foundation" or "resident" (whole file decoded up front)
};

// WAV streams incrementally. Other formats stream incrementally through Media Foundation on
// Windows. .ocaudio holds raw f32 in one AVR1 chunk with no incremental reader, so it is decoded
// whole and replayed from memory; so is anything else on a platform with no streaming decoder.
OpenedStream openStreamSource(const std::string& utf8Path, std::string* why);

#if defined(_WIN32)
// Media Foundation, read incrementally. Null with *why set when no decoder takes the file.
std::unique_ptr<StreamSource> openMfStream(const std::string& utf8Path, std::string* why);
#endif

} // namespace aver::audio
