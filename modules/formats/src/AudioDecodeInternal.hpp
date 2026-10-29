#pragma once
// Private to Aver.Formats.Audio: what the three decoder translation units share with AudioDecode.cpp.
#include "aver/formats/AudioDecode.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace aver::fmt::detail {

// The bytes of a file or a buffer, seekable. Backs the read/seek/tell callbacks of dr_flac and dr_mp3.
class ByteInput {
public:
    bool openFile(const std::string& utf8Path);
    void openMemory(std::vector<u8> bytes);
    usize read(void* dst, usize n);                 // fewer than n only at the end
    bool seek(i64 offset, int origin);              // origin: 0 set, 1 current, 2 end
    bool tell(i64& out);
    u64 size() const { return size_; }
private:
    std::ifstream file_;
    std::vector<u8> mem_;
    bool memory_ = false;
    u64 pos_ = 0, size_ = 0;
};

// Where a decoder reads from: a path (read incrementally) or a buffer.
struct AudioInput {
    std::string path;
    std::vector<u8> bytes;
    bool memory = false;
};

// The speaker order a codec uses beyond stereo, so a 5.1 file folds down to the right pairs.
enum class ChannelOrder : u8 { Vorbis, Flac, Mp3 };

// A decoder before folding and windowing: native channel count 1..kMaxDecodeChannels.
struct RawDecoder {
    std::unique_ptr<audio::StreamSource> source;
    ChannelOrder order = ChannelOrder::Vorbis;
    bool hasTagLoop = false;
    u32 tagLoopBegin = 0, tagLoopLength = 0;
};

RawDecoder openVorbisDecoder(AudioInput&& in, std::string* why);
RawDecoder openFlacDecoder(AudioInput&& in, std::string* why);
RawDecoder openMp3Decoder(AudioInput&& in, std::string* why);

// fopen for a UTF-8 path ("rb"); null on failure.
std::FILE* openFileUtf8(const std::string& utf8Path);

inline bool failWith(std::string* why, const char* msg) { if (why) *why = msg; return false; }

} // namespace aver::fmt::detail
