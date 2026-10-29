#pragma once
// Ogg Vorbis, FLAC and MP3 on every platform, from the vendored stb_vorbis and dr_flac/dr_mp3
// (third_party/stb, third_party/dr_libs). Each decoder is a StreamSource: the decoder thread of a
// stream reads it incrementally with bounded memory, and the import path reads it to the end.
//
// Hostile input is the normal case for a file a player drops in: every open validates the format,
// every allocation is capped, and a truncated or garbage file ends the stream instead of crashing.
#include "aver/audio/Stream.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::fmt {

enum class AudioCodec : u8 { None, Vorbis, Flac, Mp3 };

// By extension: .ogg .oga (Vorbis), .flac, .mp3. None for anything else.
AudioCodec audioCodecForPath(const std::string& path);
// By the first bytes: "OggS", "fLaC", an ID3 tag or an MPEG frame sync. None when unrecognised.
AudioCodec sniffAudioCodec(const u8* bytes, usize size);
const char* audioCodecName(AudioCodec c);       // "vorbis", "flac", "mp3", "none"

// What a stream window and its loop look like. Frames are source-rate frames.
struct DecodeOptions {
    u64 startFrame     = 0;     // the first frame the stream yields; seek(0) lands here
    u64 durationFrames = 0;     // 0 = to the end of the file
    // Loop points in file frames. loopBegin/loopLength both 0 takes the Ogg LOOPSTART/LOOPLENGTH
    // comments when the file has them; FLAC and MP3 carry no such tag, so they loop where this says.
    u32 loopBegin      = 0;
    u32 loopLength     = 0;
};

struct DecodedStream {
    std::unique_ptr<audio::StreamSource> source;     // mono or stereo; more channels are folded down
    AudioCodec codec = AudioCodec::None;
    u32 loopBegin = 0;          // in frames of `source` (after startFrame)
    u32 loopEnd   = 0;          // 0 = to the end
};

// Opens a file for incremental decoding. Null source with *why set on failure. The codec comes from
// the magic bytes, falling back to the extension.
DecodedStream openDecodedFile(const std::string& utf8Path, const DecodeOptions& opt = {}, std::string* why = nullptr);
// The same over bytes already in memory (the buffer is owned by the stream).
DecodedStream openDecodedMemory(std::vector<u8> bytes, AudioCodec codec, const DecodeOptions& opt = {},
                                std::string* why = nullptr);

inline constexpr u64 kMaxResidentSamples = 64ull * 1024 * 1024;     // 256 MB of f32
inline constexpr u32 kMaxDecodeChannels  = 8;
inline constexpr u32 kMaxDecodeRate      = 768000;

// Decodes a whole file into the mixer's resident format, for short clips and the importer. Refuses
// anything over `maxSamples` interleaved samples (stream it instead): a compressed file can declare or
// expand to far more than its size suggests, so the output never grows past the limit.
bool decodeAudioFile(const std::string& utf8Path, audio::SoundData& out, std::string* why = nullptr,
                     AudioCodec* codecOut = nullptr, u64 maxSamples = kMaxResidentSamples);
bool decodeAudioMemory(std::vector<u8> bytes, AudioCodec codec, audio::SoundData& out, std::string* why = nullptr,
                       u64 maxSamples = kMaxResidentSamples);

// Reads any StreamSource to its end into `out`, with the same limit. False when it hit the limit.
bool decodeSourceToSound(audio::StreamSource& src, audio::SoundData& out, std::string* why = nullptr,
                         u64 maxSamples = kMaxResidentSamples);

// The loop points a list of Vorbis comments names (LOOPSTART, LOOPLENGTH; LOOPEND is read too when
// LOOPLENGTH is absent). True when LOOPSTART or a length was present and parsed.
bool parseLoopComments(const char* const* comments, int count, u32& loopBegin, u32& loopLength);

// Moves loop points given in file frames into a window's own frames. False when the loop lies
// outside the window (no loop then). loopEnd 0 stays 0 (to the end of the window).
bool loopIntoWindow(u64 startFrame, u64 durationFrames, u64& loopBegin, u64& loopEnd);

// A window onto another source: yields frames [startFrame, startFrame + durationFrames) of it, and
// seek() is relative to the start. A source that cannot seek is read and discarded up to startFrame.
std::unique_ptr<audio::StreamSource> makeWindowedSource(std::unique_ptr<audio::StreamSource> inner,
                                                         u64 startFrame, u64 durationFrames);

} // namespace aver::fmt
