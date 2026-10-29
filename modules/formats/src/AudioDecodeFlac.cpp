// FLAC through the vendored dr_flac (third_party/dr_libs/dr_flac.h), compiled in this one
// translation unit. Native FLAC only (no Ogg encapsulation), read through callbacks so a file streams
// with bounded memory.
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG

#include "AudioDecodeInternal.hpp"

#include "dr_flac.h"

namespace aver::fmt::detail {
namespace {

size_t flacRead(void* user, void* dst, size_t n) { return static_cast<ByteInput*>(user)->read(dst, n); }

drflac_bool32 flacSeek(void* user, int offset, drflac_seek_origin origin) {
    const int o = origin == DRFLAC_SEEK_SET ? 0 : origin == DRFLAC_SEEK_CUR ? 1 : 2;
    return static_cast<ByteInput*>(user)->seek(offset, o) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

drflac_bool32 flacTell(void* user, drflac_int64* cursor) {
    i64 p = 0;
    if (!static_cast<ByteInput*>(user)->tell(p)) return DRFLAC_FALSE;
    *cursor = p;
    return DRFLAC_TRUE;
}

class FlacSource final : public audio::StreamSource {
public:
    ~FlacSource() override { if (f_) drflac_close(f_); }

    bool open(AudioInput&& in, std::string* why) {
        if (in.memory) input_.openMemory(std::move(in.bytes));
        else if (!input_.openFile(in.path)) return failWith(why, "could not open the file");

        f_ = drflac_open(flacRead, flacSeek, flacTell, &input_, nullptr);
        if (!f_) return failWith(why, "not a valid FLAC stream");
        if (f_->channels < 1 || f_->channels > kMaxDecodeChannels || f_->sampleRate < 1 || f_->sampleRate > kMaxDecodeRate)
            return failWith(why, "the FLAC stream declares an unusable channel count or sample rate");
        // STREAMINFO is a claim, not a fact: 0 means unknown, and nothing bigger than 36 bits is real.
        total_ = f_->totalPCMFrameCount < (1ull << 36) ? f_->totalPCMFrameCount : 0;
        return true;
    }

    u32 channels() const override { return f_->channels; }
    u32 sampleRate() const override { return f_->sampleRate; }
    u64 totalFrames() const override { return total_; }

    u32 read(f32* dst, u32 maxFrames) override {
        if (!f_ || maxFrames == 0) return 0;
        return static_cast<u32>(drflac_read_pcm_frames_f32(f_, maxFrames, dst));
    }

    bool seek(u64 frame) override {
        if (!f_ || (total_ != 0 && frame > total_)) return false;
        return drflac_seek_to_pcm_frame(f_, frame) != DRFLAC_FALSE;
    }

private:
    ByteInput input_;
    drflac* f_ = nullptr;
    u64 total_ = 0;
};

} // namespace

RawDecoder openFlacDecoder(AudioInput&& in, std::string* why) {
    RawDecoder raw;
    raw.order = ChannelOrder::Flac;
    auto src = std::make_unique<FlacSource>();
    if (!src->open(std::move(in), why)) return {};
    raw.source = std::move(src);
    return raw;
}

} // namespace aver::fmt::detail
