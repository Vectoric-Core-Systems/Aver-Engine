// Ogg Vorbis through the vendored stb_vorbis (third_party/stb/stb_vorbis.c), compiled in this one
// translation unit. Its asserts are off: a hostile file is a decode error here, never an abort.
#ifndef NDEBUG
#  define NDEBUG 1
#endif
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define STB_VORBIS_MAX_CHANNELS 8

#include "AudioDecodeInternal.hpp"

#include "stb_vorbis.c"

// stb_vorbis leaves these single-letter macros behind.
#undef L
#undef C
#undef R

#include <algorithm>
#include <cstring>
#include <new>

namespace aver::fmt::detail {
namespace {

// stb_vorbis allocates its setup (codebooks, windows) from the header it parses, which a hostile file
// controls. Giving it a fixed arena turns "huge declared sizes" into an out-of-memory error at a
// known ceiling; the arena is grown in steps so an ordinary file costs a few megabytes, not the cap.
constexpr int kArenaTiers[] = {6 << 20, 24 << 20, 96 << 20};

u32 le32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

// stb_vorbis reads the comment header with lengths it never checks against the packet: a length field
// that runs past the end reads as a negative int and becomes an allocation of that size, then a write
// before the arena. That is the one place it trusts a 32-bit field from the file (everything else it
// reads goes through bit reads that return 0 at the end of a packet), so the comment packet is
// checked here, against its own size, before the decoder sees the file.
bool commentPacketIsSound(const std::vector<u8>& c) {
    const u64 n = c.size();
    if (n < 11 || c[0] != 3 || std::memcmp(c.data() + 1, "vorbis", 6) != 0) return false;
    u64 p = 7;
    const u64 vendor = le32(c.data() + p);
    p += 4;
    if (vendor > n - p) return false;
    p += vendor;
    if (n - p < 4) return false;
    const u64 count = le32(c.data() + p);
    p += 4;
    if (count > (n - p) / 4) return false;                  // each comment is at least its own length field
    for (u64 i = 0; i < count; ++i) {
        if (n - p < 4) return false;
        const u64 len = le32(c.data() + p);
        p += 4;
        if (len > n - p) return false;
        p += len;
    }
    return p < n && (c[p] & 1) != 0;                        // the framing bit
}

// Walks the Ogg pages from the start, reassembling packet 1 (the comment header) by the lacing values
// and checking it. `readAt(offset, dst, n)` returns the bytes it could read. Bounded: the scan stops
// at kMaxScan bytes whatever the lacing says.
template <class ReadAt>
bool commentHeaderIsSound(ReadAt readAt, u64 size) {
    constexpr u64 kMaxScan = 64ull << 20;
    u64 at = 0;
    u32 packetIndex = 0;
    std::vector<u8> comment;
    while (at + 27 <= size && at < kMaxScan) {
        u8 h[27];
        if (readAt(at, h, 27) != 27 || std::memcmp(h, "OggS", 4) != 0) return false;
        const u32 nseg = h[26];
        u8 lacing[255];
        if (readAt(at + 27, lacing, nseg) != nseg) return false;
        u64 body = at + 27 + nseg;
        for (u32 s = 0; s < nseg; ++s) {
            const u32 len = lacing[s];
            if (packetIndex == 1) {
                if (comment.size() + len > kMaxScan) return false;
                const usize old = comment.size();
                comment.resize(old + len);
                if (len != 0 && readAt(body, comment.data() + old, len) != len) return false;
            }
            body += len;
            if (len < 255) {                                  // a packet ends on any lacing value under 255
                if (packetIndex == 1) return commentPacketIsSound(comment);
                ++packetIndex;
            }
        }
        at = body;
    }
    return false;                                             // the stream ended inside the headers
}

class VorbisSource final : public audio::StreamSource {
public:
    ~VorbisSource() override {
        if (v_) stb_vorbis_close(v_);
        if (fp_) std::fclose(fp_);
    }

    bool open(AudioInput&& in, RawDecoder& raw, std::string* why) {
        bytes_ = std::move(in.bytes);
        if (in.memory) {
            if (bytes_.empty() || bytes_.size() > 0x7FFFFFFFu) return failWith(why, "the Ogg data is empty or too large");
        } else {
            fp_ = openFileUtf8(in.path);
            if (!fp_) return failWith(why, "could not open the file");
        }

        bool sound = false;
        if (in.memory) {
            sound = commentHeaderIsSound([this](u64 off, void* dst, usize n) -> usize {
                if (off >= bytes_.size()) return 0;
                const usize m = static_cast<usize>(std::min<u64>(n, bytes_.size() - off));
                std::memcpy(dst, bytes_.data() + off, m);
                return m;
            }, bytes_.size());
        } else {
            std::fseek(fp_, 0, SEEK_END);
            const long len = std::ftell(fp_);
            sound = len > 0 && commentHeaderIsSound([this](u64 off, void* dst, usize n) -> usize {
                if (n == 0) return 0;
                if (std::fseek(fp_, static_cast<long>(off), SEEK_SET) != 0) return 0;
                return std::fread(dst, 1, n, fp_);
            }, static_cast<u64>(len));
            std::fseek(fp_, 0, SEEK_SET);
        }
        if (!sound) return failWith(why, "not a valid Ogg Vorbis stream (the headers are malformed)");

        int err = 0;
        for (const int tier : kArenaTiers) {
            arena_.reset(new (std::nothrow) char[static_cast<usize>(tier)]);
            if (!arena_) return failWith(why, "out of memory");
            stb_vorbis_alloc alloc;
            alloc.alloc_buffer = arena_.get();
            alloc.alloc_buffer_length_in_bytes = tier;
            err = 0;
            if (in.memory) {
                v_ = stb_vorbis_open_memory(bytes_.data(), static_cast<int>(bytes_.size()), &err, &alloc);
            } else {
                std::fseek(fp_, 0, SEEK_SET);
                v_ = stb_vorbis_open_file(fp_, 0, &err, &alloc);
            }
            if (v_ || err != VORBIS_outofmem) break;
        }
        if (!v_) {
            return failWith(why, err == VORBIS_outofmem ? "the stream asks for more setup memory than the decoder allows"
                                                        : "not a valid Ogg Vorbis stream (Ogg Opus is not supported)");
        }

        const stb_vorbis_info info = stb_vorbis_get_info(v_);
        if (info.channels < 1 || info.channels > static_cast<int>(kMaxDecodeChannels) || info.sample_rate == 0 ||
            info.sample_rate > kMaxDecodeRate)
            return failWith(why, "the Ogg stream declares an unusable channel count or sample rate");
        ch_ = static_cast<u32>(info.channels);
        rate_ = info.sample_rate;

        const stb_vorbis_comment c = stb_vorbis_get_comment(v_);
        raw.hasTagLoop = parseLoopComments(c.comment_list, c.comment_list_length, raw.tagLoopBegin, raw.tagLoopLength);

        total_ = stb_vorbis_stream_length_in_samples(v_);
        stb_vorbis_seek_start(v_);
        return true;
    }

    u32 channels() const override { return ch_; }
    u32 sampleRate() const override { return rate_; }
    u64 totalFrames() const override { return total_; }

    u32 read(f32* dst, u32 maxFrames) override {
        if (!v_ || maxFrames == 0) return 0;
        maxFrames = std::min<u32>(maxFrames, 1u << 16);
        const int n = stb_vorbis_get_samples_float_interleaved(v_, static_cast<int>(ch_), dst,
                                                               static_cast<int>(maxFrames * ch_));
        return n > 0 ? static_cast<u32>(n) : 0;
    }

    bool seek(u64 frame) override {
        if (!v_) return false;
        if (frame == 0) return stb_vorbis_seek_start(v_) != 0;
        if (frame > 0xFFFFFFFFull || (total_ != 0 && frame > total_)) return false;
        return stb_vorbis_seek(v_, static_cast<unsigned int>(frame)) != 0;
    }

private:
    stb_vorbis* v_ = nullptr;
    std::FILE* fp_ = nullptr;
    std::unique_ptr<char[]> arena_;
    std::vector<u8> bytes_;
    u32 ch_ = 0, rate_ = 0;
    u64 total_ = 0;
};

} // namespace

RawDecoder openVorbisDecoder(AudioInput&& in, std::string* why) {
    RawDecoder raw;
    raw.order = ChannelOrder::Vorbis;
    auto src = std::make_unique<VorbisSource>();
    if (!src->open(std::move(in), raw, why)) return {};
    raw.source = std::move(src);
    return raw;
}

} // namespace aver::fmt::detail
