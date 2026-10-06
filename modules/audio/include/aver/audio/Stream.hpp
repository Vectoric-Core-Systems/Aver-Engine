#pragma once
// Streamed playback: a decoder thread keeps a single-producer/single-consumer ring of decoded
// frames ahead of the audio thread. The audio thread only reads atomics and the ring.
#include "aver/audio/Sound.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace aver::audio {

// Anything that can produce interleaved f32 frames in order. read() may block; it runs on the
// decoder thread, never the audio thread.
class StreamSource {
public:
    virtual ~StreamSource() = default;
    virtual u32 channels() const = 0;                 // 1 or 2
    virtual u32 sampleRate() const = 0;
    virtual u64 totalFrames() const { return 0; }     // 0 when unknown
    // Writes up to maxFrames frames. Returns the number written; 0 means end of stream or failure.
    virtual u32 read(f32* dst, u32 maxFrames) = 0;
    // Repositions to a frame. False when the source cannot seek (looping then ends the stream).
    virtual bool seek(u64 frame) = 0;
};

// A resident SoundData presented as a stream: the fallback for formats with no incremental
// decoder, and the source the tests drive.
class MemoryStreamSource final : public StreamSource {
public:
    explicit MemoryStreamSource(SoundData data, u32 maxFramesPerRead = 0)
        : data_(std::move(data)), maxPerRead_(maxFramesPerRead) {}
    u32 channels() const override { return data_.channels; }
    u32 sampleRate() const override { return data_.sampleRate; }
    u64 totalFrames() const override { return data_.frames(); }
    u32 read(f32* dst, u32 maxFrames) override;
    bool seek(u64 frame) override;
private:
    SoundData data_;
    u32 maxPerRead_ = 0;      // 0 = no cap; a cap models a slow decoder
    u64 pos_ = 0;
};

// Streams a RIFF/WAVE file from disk (8/16/24/32-bit PCM and 32-bit float) with bounded memory.
// Null on failure, with the reason in *why.
std::unique_ptr<StreamSource> openWavStream(const char* utf8Path, std::string* why = nullptr);

struct StreamParams {
    u32  ringFrames      = 0;       // 0: two seconds of source audio
    u32  prebufferFrames = 0;       // 0: a quarter second; the voice stays silent until this is queued
    u32  chunkFrames     = 2048;    // decode granularity
    bool looping         = false;
    u32  loopBegin       = 0;       // frames
    u32  loopEnd         = 0;       // frames; 0 = end of source
    bool threaded        = true;    // false: the owner calls pump() itself (tests)
};

// Owns a source, its ring and (optionally) the decoder thread.
class StreamDecoder {
public:
    StreamDecoder(std::unique_ptr<StreamSource> source, const StreamParams& params);
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    bool valid() const { return valid_; }
    u32  channels() const { return channels_; }
    u32  sampleRate() const { return sampleRate_; }
    u32  ringFrames() const { return capacity_; }
    bool threaded() const { return params_.threaded; }

    // ---- decoder side ----
    // Decodes up to maxFrames into the ring. Returns frames produced; 0 when full or at the end.
    u32 pump(u32 maxFrames);

    // ---- audio thread ----
    // Frames queued and readable.
    u32  available() const;
    // True once the end of the stream has been queued in full.
    bool endReached() const { return eof_.load(std::memory_order_acquire); }
    // True once enough is queued to start (or the stream is already complete).
    bool prebuffered() const;
    // The interleaved frame `offset` frames past the read position. offset must be < available().
    const f32* frameAt(u32 offset) const;
    // Releases frames from the front of the ring.
    void consume(u32 frames);
    void noteUnderrun() { underruns_.fetch_add(1, std::memory_order_relaxed); }

    u32 underruns() const { return underruns_.load(std::memory_order_relaxed); }
    u64 decodedFrames() const { return head_.load(std::memory_order_relaxed); }

private:
    void threadMain();
    // Pushes frames into the ring. Producer only; caller guarantees room.
    void push(const f32* src, u32 frames);

    std::unique_ptr<StreamSource> source_;
    StreamParams params_;
    bool valid_ = false;
    u32  channels_ = 0, sampleRate_ = 0, capacity_ = 0, prebuffer_ = 0;

    std::vector<f32> ring_;                 // capacity_ * channels_
    std::vector<f32> scratch_;              // chunkFrames * channels_ (decoder side)
    std::atomic<u64> head_{0};              // total frames written
    std::atomic<u64> tail_{0};              // total frames consumed
    std::atomic<bool> eof_{false};
    std::atomic<bool> quit_{false};
    std::atomic<u32>  underruns_{0};

    // Producer-owned.
    u64  srcPos_ = 0;
    bool justSeeked_ = false;
    u64  loopEnd_ = 0;
    bool looping_ = false;

    std::thread thread_;
};

} // namespace aver::audio
