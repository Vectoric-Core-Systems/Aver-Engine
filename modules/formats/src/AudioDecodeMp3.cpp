// MP3 through the vendored dr_mp3 (third_party/dr_libs/dr_mp3.h), compiled in this one translation
// unit. dr_mp3 reads the Xing/Info and LAME tag itself and trims the encoder delay and padding, so a
// file with that tag plays gapless; a file without one plays its frames as they are.
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_MP3_FLOAT_OUTPUT

#include "AudioDecodeInternal.hpp"

#include "dr_mp3.h"

namespace aver::fmt::detail {
namespace {

size_t mp3Read(void* user, void* dst, size_t n) { return static_cast<ByteInput*>(user)->read(dst, n); }

drmp3_bool32 mp3Seek(void* user, int offset, drmp3_seek_origin origin) {
    const int o = origin == DRMP3_SEEK_SET ? 0 : origin == DRMP3_SEEK_CUR ? 1 : 2;
    return static_cast<ByteInput*>(user)->seek(offset, o) ? DRMP3_TRUE : DRMP3_FALSE;
}

drmp3_bool32 mp3Tell(void* user, drmp3_int64* cursor) {
    i64 p = 0;
    if (!static_cast<ByteInput*>(user)->tell(p)) return DRMP3_FALSE;
    *cursor = p;
    return DRMP3_TRUE;
}

// A scan of the file on the first seek builds this many entry points, so a loop back to the middle of
// a long track decodes a few frames instead of the whole run-up.
constexpr drmp3_uint32 kSeekPoints = 1024;

class Mp3Source final : public audio::StreamSource {
public:
    ~Mp3Source() override { if (inited_) drmp3_uninit(&mp3_); }

    bool open(AudioInput&& in, std::string* why) {
        if (in.memory) input_.openMemory(std::move(in.bytes));
        else if (!input_.openFile(in.path)) return failWith(why, "could not open the file");

        if (!drmp3_init(&mp3_, mp3Read, mp3Seek, mp3Tell, nullptr, &input_, nullptr)) return failWith(why, "not a valid MP3 stream");
        inited_ = true;
        if (mp3_.channels < 1 || mp3_.channels > 2 || mp3_.sampleRate < 1 || mp3_.sampleRate > kMaxDecodeRate)
            return failWith(why, "the MP3 stream declares an unusable channel count or sample rate");
        // The frame count is known up front only when a Xing/Info tag states it; the tag is a claim, so
        // a figure the file is too small to hold (8 kbit/s is the slowest MP3) is dropped.
        if (mp3_.totalPCMFrameCount != DRMP3_UINT64_MAX) {
            const u64 claimed = drmp3_get_pcm_frame_count(&mp3_);
            const u64 limit = input_.size() * mp3_.sampleRate / 1000 + 2304;
            if (claimed != 0 && claimed <= limit) total_ = claimed;
        }
        return true;
    }

    u32 channels() const override { return mp3_.channels; }
    u32 sampleRate() const override { return mp3_.sampleRate; }
    u64 totalFrames() const override { return total_; }

    u32 read(f32* dst, u32 maxFrames) override {
        if (!inited_ || maxFrames == 0) return 0;
        return static_cast<u32>(drmp3_read_pcm_frames_f32(&mp3_, maxFrames, dst));
    }

    bool seek(u64 frame) override {
        if (!inited_ || (total_ != 0 && frame > total_)) return false;
        if (frame != 0 && !tableTried_) {
            tableTried_ = true;
            drmp3_uint32 count = kSeekPoints;
            points_.resize(count);
            if (drmp3_calculate_seek_points(&mp3_, &count, points_.data()) && count > 0)
                drmp3_bind_seek_table(&mp3_, count, points_.data());
        }
        return drmp3_seek_to_pcm_frame(&mp3_, frame) != DRMP3_FALSE;
    }

private:
    ByteInput input_;
    drmp3 mp3_{};
    std::vector<drmp3_seek_point> points_;
    bool inited_ = false, tableTried_ = false;
    u64 total_ = 0;
};

} // namespace

RawDecoder openMp3Decoder(AudioInput&& in, std::string* why) {
    RawDecoder raw;
    raw.order = ChannelOrder::Mp3;
    auto src = std::make_unique<Mp3Source>();
    if (!src->open(std::move(in), why)) return {};
    raw.source = std::move(src);
    return raw;
}

} // namespace aver::fmt::detail
