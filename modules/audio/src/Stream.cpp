// Stream ring buffer, decoder thread and the memory source.
#include "aver/audio/Stream.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace aver::audio {

// ---------------------------------------------------------------- MemoryStreamSource

u32 MemoryStreamSource::read(f32* dst, u32 maxFrames) {
    const u64 total = data_.frames();
    if (pos_ >= total) return 0;
    u32 n = maxFrames;
    if (maxPerRead_ > 0 && n > maxPerRead_) n = maxPerRead_;
    if (static_cast<u64>(n) > total - pos_) n = static_cast<u32>(total - pos_);
    std::memcpy(dst, data_.samples.data() + static_cast<usize>(pos_) * data_.channels,
                static_cast<usize>(n) * data_.channels * sizeof(f32));
    pos_ += n;
    return n;
}

bool MemoryStreamSource::seek(u64 frame) {
    if (frame > data_.frames()) return false;
    pos_ = frame;
    return true;
}

// ---------------------------------------------------------------- StreamDecoder

StreamDecoder::StreamDecoder(std::unique_ptr<StreamSource> source, const StreamParams& params)
    : source_(std::move(source)), params_(params) {
    if (!source_) return;
    channels_   = source_->channels();
    sampleRate_ = source_->sampleRate();
    if ((channels_ != 1 && channels_ != 2) || sampleRate_ == 0) {
        AVER_ERROR("[Audio] stream source with {} channels at {} Hz is not playable", channels_, sampleRate_);
        return;
    }

    capacity_ = params_.ringFrames ? params_.ringFrames : sampleRate_ * 2;
    if (capacity_ < 256) capacity_ = 256;
    prebuffer_ = params_.prebufferFrames ? params_.prebufferFrames : sampleRate_ / 4;
    if (prebuffer_ > capacity_ / 2) prebuffer_ = capacity_ / 2;
    if (params_.chunkFrames == 0) params_.chunkFrames = 2048;

    ring_.assign(static_cast<usize>(capacity_) * channels_, 0.0f);
    scratch_.assign(static_cast<usize>(params_.chunkFrames) * channels_, 0.0f);

    const u64 total = source_->totalFrames();
    loopEnd_ = params_.loopEnd ? params_.loopEnd : (total ? total : ~0ull);
    if (total && loopEnd_ > total) loopEnd_ = total;
    looping_ = params_.looping && params_.loopBegin < loopEnd_;

    valid_ = true;
    if (params_.threaded) thread_ = std::thread([this] { threadMain(); });
}

StreamDecoder::~StreamDecoder() {
    quit_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void StreamDecoder::push(const f32* src, u32 frames) {
    const u64 h = head_.load(std::memory_order_relaxed);
    for (u32 i = 0; i < frames; ++i) {
        const usize slot = static_cast<usize>((h + i) % capacity_) * channels_;
        for (u32 c = 0; c < channels_; ++c) ring_[slot + c] = src[static_cast<usize>(i) * channels_ + c];
    }
    head_.store(h + frames, std::memory_order_release);
}

u32 StreamDecoder::pump(u32 maxFrames) {
    if (!valid_ || eof_.load(std::memory_order_relaxed)) return 0;
    const u64 h = head_.load(std::memory_order_relaxed);
    const u64 t = tail_.load(std::memory_order_acquire);
    const u32 room = capacity_ - static_cast<u32>(h - t);
    u32 budget = std::min(maxFrames, room);
    u32 produced = 0;

    while (budget > 0) {
        if (looping_ && srcPos_ >= loopEnd_) {
            if (!source_->seek(params_.loopBegin)) { eof_.store(true, std::memory_order_release); break; }
            srcPos_ = params_.loopBegin;
            justSeeked_ = true;
        }
        u32 want = std::min(budget, params_.chunkFrames);
        if (looping_ && loopEnd_ != ~0ull) want = static_cast<u32>(std::min<u64>(want, loopEnd_ - srcPos_));
        if (want == 0) break;

        const u32 got = source_->read(scratch_.data(), want);
        if (got == 0) {
            // Ran dry early: loop once more unless a read straight after a seek was empty too.
            if (looping_ && !justSeeked_) { srcPos_ = loopEnd_; continue; }
            eof_.store(true, std::memory_order_release);
            break;
        }
        justSeeked_ = false;
        srcPos_ += got;
        push(scratch_.data(), got);
        produced += got;
        budget -= got;
    }
    return produced;
}

void StreamDecoder::threadMain() {
    while (!quit_.load(std::memory_order_acquire)) {
        if (eof_.load(std::memory_order_relaxed)) break;
        if (pump(params_.chunkFrames) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

u32 StreamDecoder::available() const {
    const u64 h = head_.load(std::memory_order_acquire);
    const u64 t = tail_.load(std::memory_order_relaxed);
    return static_cast<u32>(h - t);
}

bool StreamDecoder::prebuffered() const {
    // eof first: once it reads true, every frame queued before it is visible to available().
    if (eof_.load(std::memory_order_acquire)) return true;
    return available() >= prebuffer_;
}

const f32* StreamDecoder::frameAt(u32 offset) const {
    const u64 t = tail_.load(std::memory_order_relaxed);
    return ring_.data() + static_cast<usize>((t + offset) % capacity_) * channels_;
}

void StreamDecoder::consume(u32 frames) {
    tail_.store(tail_.load(std::memory_order_relaxed) + frames, std::memory_order_release);
}

} // namespace aver::audio
