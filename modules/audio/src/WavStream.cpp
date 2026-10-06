// Incremental RIFF/WAVE reader: header scan once, then bounded-memory reads on the decoder thread.
#include "aver/audio/Stream.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace aver::audio {
namespace {

constexpr u16 kFormatPcm = 1, kFormatFloat = 3, kFormatExtensible = 0xFFFE;

u32 rdU32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
u16 rdU16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return v; }

class WavStreamSource final : public StreamSource {
public:
    bool open(const char* utf8Path, std::string* why);
    u32 channels() const override { return channels_; }
    u32 sampleRate() const override { return rate_; }
    u64 totalFrames() const override { return frames_; }
    u32 read(f32* dst, u32 maxFrames) override;
    bool seek(u64 frame) override;

private:
    std::ifstream file_;
    u64 dataStart_ = 0, frames_ = 0, pos_ = 0;
    u16 format_ = 0, bits_ = 0, channels_ = 0;
    u32 rate_ = 0, blockAlign_ = 0;
    std::vector<u8> bytes_;
};

bool failWith(std::string* why, const char* msg) { if (why) *why = msg; return false; }

bool WavStreamSource::open(const char* utf8Path, std::string* why) {
    if (!utf8Path || !*utf8Path) return failWith(why, "empty path");
    const std::u8string pathU8(reinterpret_cast<const char8_t*>(utf8Path));
    file_.open(std::filesystem::path(pathU8), std::ios::binary);
    if (!file_) return failWith(why, "could not open the file");

    file_.seekg(0, std::ios::end);
    const u64 fileSize = static_cast<u64>(file_.tellg());
    file_.seekg(0, std::ios::beg);

    u8 head[12];
    file_.read(reinterpret_cast<char*>(head), 12);
    if (!file_ || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0)
        return failWith(why, "not a RIFF/WAVE file");

    u64 dataBytes = 0;
    bool haveData = false;
    u64 p = 12;
    while (p + 8 <= fileSize) {
        u8 ch[8];
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(p));
        file_.read(reinterpret_cast<char*>(ch), 8);
        if (!file_) break;
        const u32 size = rdU32(ch + 4);
        const u64 body = p + 8;

        if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16 && body + size <= fileSize) {
            std::vector<u8> f(size);
            file_.read(reinterpret_cast<char*>(f.data()), size);
            if (!file_) break;
            format_   = rdU16(f.data());
            channels_ = rdU16(f.data() + 2);
            rate_     = rdU32(f.data() + 4);
            bits_     = rdU16(f.data() + 14);
            if (format_ == kFormatExtensible && size >= 40) format_ = rdU16(f.data() + 24);
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataStart_ = body;
            dataBytes = std::min<u64>(size, fileSize - body);   // streamed WAVs may carry 0 or 0xFFFFFFFF
            haveData = true;
        }
        p = body + size + (size & 1u);
        if (haveData && channels_ != 0) break;
    }

    if (!haveData || dataBytes == 0) return failWith(why, "no data chunk");
    if (channels_ == 0 || rate_ == 0) return failWith(why, "no usable fmt chunk");
    if (channels_ > 2) return failWith(why, "streaming supports mono and stereo only");
    if (format_ != kFormatPcm && format_ != kFormatFloat)
        return failWith(why, "compressed WAV: only integer PCM and 32-bit float are read");
    if (format_ == kFormatFloat && bits_ != 32) return failWith(why, "float WAV that is not 32-bit");
    if (format_ == kFormatPcm && bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32)
        return failWith(why, "PCM WAV with an unsupported bit depth");

    blockAlign_ = static_cast<u32>(channels_) * (bits_ / 8u);
    frames_ = dataBytes / blockAlign_;
    if (frames_ == 0) return failWith(why, "the data chunk holds no complete frame");
    return seek(0);
}

bool WavStreamSource::seek(u64 frame) {
    if (frame > frames_) return false;
    pos_ = frame;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataStart_ + frame * blockAlign_));
    return static_cast<bool>(file_);
}

u32 WavStreamSource::read(f32* dst, u32 maxFrames) {
    if (pos_ >= frames_) return 0;
    const u32 n = static_cast<u32>(std::min<u64>(maxFrames, frames_ - pos_));
    bytes_.resize(static_cast<usize>(n) * blockAlign_);
    file_.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    const usize gotBytes = static_cast<usize>(file_.gcount());
    const u32 gotFrames = static_cast<u32>(gotBytes / blockAlign_);
    if (gotFrames == 0) return 0;

    const u32 bps = bits_ / 8u;
    const usize samples = static_cast<usize>(gotFrames) * channels_;
    for (usize i = 0; i < samples; ++i) {
        const u8* s = bytes_.data() + i * bps;
        f32 v;
        if (format_ == kFormatFloat) {
            std::memcpy(&v, s, 4);
        } else if (bits_ == 8) {
            v = (static_cast<f32>(s[0]) - 128.0f) / 128.0f;
        } else if (bits_ == 16) {
            i16 t; std::memcpy(&t, s, 2); v = static_cast<f32>(t) / 32768.0f;
        } else if (bits_ == 24) {
            const i32 t = static_cast<i32>(static_cast<u32>(s[0]) | (static_cast<u32>(s[1]) << 8) |
                                           (static_cast<u32>(s[2]) << 16) | ((s[2] & 0x80) ? 0xFF000000u : 0u));
            v = static_cast<f32>(t) / 8388608.0f;
        } else {
            i32 t; std::memcpy(&t, s, 4); v = static_cast<f32>(t) / 2147483648.0f;
        }
        dst[i] = v;
    }
    pos_ += gotFrames;
    return gotFrames;
}

} // namespace

std::unique_ptr<StreamSource> openWavStream(const char* utf8Path, std::string* why) {
    auto src = std::make_unique<WavStreamSource>();
    if (!src->open(utf8Path, why)) return nullptr;
    return src;
}

} // namespace aver::audio
