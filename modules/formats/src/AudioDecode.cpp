// Ogg Vorbis / FLAC / MP3: format detection, the shared plumbing around the three decoders (byte
// input, channel fold-down, stream windows, loop points) and the whole-file decode. The decoders
// themselves are one translation unit each (AudioDecodeVorbis/Flac/Mp3.cpp).
#include "AudioDecodeInternal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace aver::fmt {
namespace detail {

bool ByteInput::openFile(const std::string& utf8Path) {
    memory_ = false;
    if (utf8Path.empty()) return false;
    const std::u8string u8(reinterpret_cast<const char8_t*>(utf8Path.c_str()));
    file_.open(std::filesystem::path(u8), std::ios::binary);
    if (!file_) return false;
    file_.seekg(0, std::ios::end);
    const std::streamoff end = file_.tellg();
    if (end < 0) return false;
    size_ = static_cast<u64>(end);
    file_.seekg(0, std::ios::beg);
    pos_ = 0;
    return static_cast<bool>(file_);
}

void ByteInput::openMemory(std::vector<u8> bytes) {
    mem_ = std::move(bytes);
    memory_ = true;
    size_ = mem_.size();
    pos_ = 0;
}

usize ByteInput::read(void* dst, usize n) {
    if (n == 0) return 0;
    if (memory_) {
        if (pos_ >= size_) return 0;
        const usize m = static_cast<usize>(std::min<u64>(n, size_ - pos_));
        std::memcpy(dst, mem_.data() + pos_, m);
        pos_ += m;
        return m;
    }
    file_.clear();
    file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    const usize got = static_cast<usize>(file_.gcount());
    pos_ += got;
    return got;
}

bool ByteInput::seek(i64 offset, int origin) {
    i64 target = 0;
    if (origin == 0)      target = offset;
    else if (origin == 1) target = static_cast<i64>(pos_) + offset;
    else                  target = static_cast<i64>(size_) + offset;
    if (target < 0 || static_cast<u64>(target) > size_) return false;
    pos_ = static_cast<u64>(target);
    if (memory_) return true;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(target), std::ios::beg);
    return static_cast<bool>(file_);
}

bool ByteInput::tell(i64& out) { out = static_cast<i64>(pos_); return true; }

std::FILE* openFileUtf8(const std::string& utf8Path) {
    if (utf8Path.empty()) return nullptr;
#if defined(_WIN32)
    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wide <= 0) return nullptr;
    std::wstring w(static_cast<usize>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, w.data(), wide);
    return _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(utf8Path.c_str(), "rb");
#endif
}

} // namespace detail

namespace {

using namespace detail;

std::string lowerExt(const std::string& path) {
    const usize dot = path.find_last_of('.');
    const usize slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return e;
}

bool parseU64(const char* s, u64& out) {
    while (*s == ' ' || *s == '\t') ++s;
    const char* e = s;
    while (*e >= '0' && *e <= '9') ++e;
    if (e == s) return false;
    u64 v = 0;
    const auto r = std::from_chars(s, e, v);
    if (r.ec != std::errc{}) return false;
    out = v;
    return true;
}

// ---- stereo fold-down for 3 to 8 channels ----------------------------------------------------

enum class Role : u8 { L, R, C, Lfe, BackL, BackR, SideL, SideR, BackC };

// Which speaker each channel is, in the order the codec writes them.
usize rolesFor(ChannelOrder order, u32 n, Role* out) {
    using R = Role;
    auto set = [&](std::initializer_list<R> r) { usize i = 0; for (R x : r) out[i++] = x; return r.size(); };
    if (order == ChannelOrder::Vorbis) {
        switch (n) {
            case 3: return set({R::L, R::C, R::R});
            case 4: return set({R::L, R::R, R::BackL, R::BackR});
            case 5: return set({R::L, R::C, R::R, R::BackL, R::BackR});
            case 6: return set({R::L, R::C, R::R, R::BackL, R::BackR, R::Lfe});
            case 7: return set({R::L, R::C, R::R, R::SideL, R::SideR, R::BackC, R::Lfe});
            case 8: return set({R::L, R::C, R::R, R::SideL, R::SideR, R::BackL, R::BackR, R::Lfe});
            default: break;
        }
    } else {
        switch (n) {
            case 3: return set({R::L, R::R, R::C});
            case 4: return set({R::L, R::R, R::BackL, R::BackR});
            case 5: return set({R::L, R::R, R::C, R::BackL, R::BackR});
            case 6: return set({R::L, R::R, R::C, R::Lfe, R::BackL, R::BackR});
            case 7: return set({R::L, R::R, R::C, R::Lfe, R::BackC, R::SideL, R::SideR});
            case 8: return set({R::L, R::R, R::C, R::Lfe, R::BackL, R::BackR, R::SideL, R::SideR});
            default: break;
        }
    }
    return 0;
}

// More than two channels, folded to stereo: centre and surrounds at -3 dB, the LFE dropped, and the
// two sums normalised so full-scale in every channel cannot clip.
class FoldSource final : public audio::StreamSource {
public:
    FoldSource(std::unique_ptr<audio::StreamSource> in, ChannelOrder order) : in_(std::move(in)), n_(in_->channels()) {
        Role roles[kMaxDecodeChannels] = {};
        const usize known = rolesFor(order, n_, roles);
        const f32 k = 0.70710678f;
        f32 sumL = 0.0f, sumR = 0.0f;
        for (u32 c = 0; c < n_; ++c) {
            f32 l = 0.0f, r = 0.0f;
            if (c < known) {
                switch (roles[c]) {
                    case Role::L: l = 1.0f; break;
                    case Role::R: r = 1.0f; break;
                    case Role::C: case Role::BackC: l = r = k; break;
                    case Role::BackL: case Role::SideL: l = k; break;
                    case Role::BackR: case Role::SideR: r = k; break;
                    case Role::Lfe: break;
                }
            } else if (c == 0) l = 1.0f;
            else if (c == 1) r = 1.0f;
            wl_[c] = l; wr_[c] = r;
            sumL += l; sumR += r;
        }
        const f32 nl = sumL > 0.0f ? 1.0f / sumL : 1.0f, nr = sumR > 0.0f ? 1.0f / sumR : 1.0f;
        for (u32 c = 0; c < n_; ++c) { wl_[c] *= nl; wr_[c] *= nr; }
    }
    u32 channels() const override { return 2; }
    u32 sampleRate() const override { return in_->sampleRate(); }
    u64 totalFrames() const override { return in_->totalFrames(); }
    u32 read(f32* dst, u32 maxFrames) override {
        scratch_.resize(static_cast<usize>(maxFrames) * n_);
        const u32 got = in_->read(scratch_.data(), maxFrames);
        for (u32 f = 0; f < got; ++f) {
            const f32* s = scratch_.data() + static_cast<usize>(f) * n_;
            f32 l = 0.0f, r = 0.0f;
            for (u32 c = 0; c < n_; ++c) { l += s[c] * wl_[c]; r += s[c] * wr_[c]; }
            dst[f * 2] = l;
            dst[f * 2 + 1] = r;
        }
        return got;
    }
    bool seek(u64 frame) override { return in_->seek(frame); }
private:
    std::unique_ptr<audio::StreamSource> in_;
    u32 n_;
    f32 wl_[kMaxDecodeChannels] = {}, wr_[kMaxDecodeChannels] = {};
    std::vector<f32> scratch_;
};

// ---- the window --------------------------------------------------------------------------------

class WindowedSource final : public audio::StreamSource {
public:
    WindowedSource(std::unique_ptr<audio::StreamSource> in, u64 start, u64 duration)
        : in_(std::move(in)), start_(start), duration_(duration) {}
    u32 channels() const override { return in_->channels(); }
    u32 sampleRate() const override { return in_->sampleRate(); }
    u64 totalFrames() const override {
        const u64 t = in_->totalFrames();
        if (t == 0) return duration_;
        const u64 avail = t > start_ ? t - start_ : 0;
        return duration_ ? std::min(duration_, avail) : avail;
    }
    u32 read(f32* dst, u32 maxFrames) override {
        if (!positioned_) { positioned_ = true; if (!moveTo(0)) return 0; }
        u32 n = maxFrames;
        if (duration_) {
            if (pos_ >= duration_) return 0;
            n = static_cast<u32>(std::min<u64>(n, duration_ - pos_));
        }
        const u32 got = in_->read(dst, n);
        pos_ += got;
        return got;
    }
    bool seek(u64 frame) override {
        positioned_ = true;
        if (duration_ && frame > duration_) return false;
        return moveTo(frame);
    }
private:
    // Lands on window frame `frame`. A source that cannot seek is only at its start before the first
    // read, so the start offset is read and discarded; any other move fails.
    bool moveTo(u64 frame) {
        if (in_->seek(start_ + frame)) { pos_ = frame; return true; }
        if (frame != 0 || pos_ != 0 || skipped_) return false;
        skipped_ = true;
        std::vector<f32> sink(static_cast<usize>(4096) * in_->channels());
        for (u64 left = start_; left > 0;) {
            const u32 got = in_->read(sink.data(), static_cast<u32>(std::min<u64>(left, 4096)));
            if (got == 0) break;
            left -= got;
        }
        return true;
    }
    std::unique_ptr<audio::StreamSource> in_;
    u64 start_, duration_, pos_ = 0;
    bool positioned_ = false, skipped_ = false;
};

// Reads the first bytes of a file to sniff its codec. Empty on failure.
std::vector<u8> readHead(const std::string& path, usize n) {
    ByteInput in;
    if (!in.openFile(path)) return {};
    std::vector<u8> head(n);
    head.resize(in.read(head.data(), n));
    return head;
}

AudioCodec chooseCodec(const std::string& path, const u8* head, usize headSize) {
    const AudioCodec bySniff = sniffAudioCodec(head, headSize);
    return bySniff != AudioCodec::None ? bySniff : audioCodecForPath(path);
}

DecodedStream finish(RawDecoder raw, AudioCodec codec, const DecodeOptions& opt, std::string* why) {
    DecodedStream out;
    if (!raw.source) return out;
    const u32 ch = raw.source->channels(), rate = raw.source->sampleRate();
    if (ch < 1 || ch > kMaxDecodeChannels || rate < 1 || rate > kMaxDecodeRate) {
        failWith(why, "the file declares an unusable channel count or sample rate");
        return out;
    }
    std::unique_ptr<audio::StreamSource> src = std::move(raw.source);
    if (ch > 2) src = std::make_unique<FoldSource>(std::move(src), raw.order);

    u64 lb = 0, le = 0;
    bool loop = false;
    if (opt.loopBegin != 0 || opt.loopLength != 0) {
        loop = true;
        lb = opt.loopBegin;
        le = opt.loopLength ? lb + opt.loopLength : 0;
    } else if (raw.hasTagLoop) {
        loop = true;
        lb = raw.tagLoopBegin;
        le = raw.tagLoopLength ? lb + raw.tagLoopLength : 0;
    }

    const u64 start = opt.startFrame, dur = opt.durationFrames;
    if (start != 0 || dur != 0) src = makeWindowedSource(std::move(src), start, dur);
    if (loop && !loopIntoWindow(start, dur, lb, le)) loop = false;
    out.codec = codec;
    out.loopBegin = loop ? static_cast<u32>(lb) : 0;
    out.loopEnd = loop ? static_cast<u32>(le) : 0;
    out.source = std::move(src);
    return out;
}

DecodedStream openCodec(AudioCodec codec, AudioInput&& in, const DecodeOptions& opt, std::string* why) {
    RawDecoder raw;
    switch (codec) {
        case AudioCodec::Vorbis: raw = openVorbisDecoder(std::move(in), why); break;
        case AudioCodec::Flac:   raw = openFlacDecoder(std::move(in), why); break;
        case AudioCodec::Mp3:    raw = openMp3Decoder(std::move(in), why); break;
        default: failWith(why, "not an Ogg Vorbis, FLAC or MP3 file"); return {};
    }
    return finish(std::move(raw), codec, opt, why);
}

} // namespace

AudioCodec audioCodecForPath(const std::string& path) {
    const std::string e = lowerExt(path);
    if (e == ".ogg" || e == ".oga") return AudioCodec::Vorbis;
    if (e == ".flac") return AudioCodec::Flac;
    if (e == ".mp3") return AudioCodec::Mp3;
    return AudioCodec::None;
}

AudioCodec sniffAudioCodec(const u8* b, usize size) {
    if (!b) return AudioCodec::None;
    if (size >= 4 && std::memcmp(b, "OggS", 4) == 0) return AudioCodec::Vorbis;
    if (size >= 4 && std::memcmp(b, "fLaC", 4) == 0) return AudioCodec::Flac;
    if (size >= 3 && std::memcmp(b, "ID3", 3) == 0) return AudioCodec::Mp3;
    if (size >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 && ((b[1] >> 1) & 3) != 0) return AudioCodec::Mp3;
    return AudioCodec::None;
}

const char* audioCodecName(AudioCodec c) {
    switch (c) {
        case AudioCodec::Vorbis: return "vorbis";
        case AudioCodec::Flac:   return "flac";
        case AudioCodec::Mp3:    return "mp3";
        default:                 return "none";
    }
}

bool parseLoopComments(const char* const* comments, int count, u32& loopBegin, u32& loopLength) {
    bool haveStart = false, haveLen = false, haveEnd = false;
    u64 start = 0, len = 0, end = 0;
    for (int i = 0; comments && i < count; ++i) {
        const char* c = comments[i];
        if (!c) continue;
        const char* eq = std::strchr(c, '=');
        if (!eq || eq - c > 16) continue;
        std::string key(c, eq);
        for (char& k : key) k = static_cast<char>(::toupper(static_cast<unsigned char>(k)));
        u64 v = 0;
        if (!parseU64(eq + 1, v) || v > 0xFFFFFFFFull) continue;
        if (key == "LOOPSTART")       { start = v; haveStart = true; }
        else if (key == "LOOPLENGTH") { len = v; haveLen = true; }
        else if (key == "LOOPEND")    { end = v; haveEnd = true; }
    }
    if (!haveStart && !haveLen && !haveEnd) return false;
    loopBegin = haveStart ? static_cast<u32>(start) : 0;
    if (haveLen) loopLength = static_cast<u32>(len);
    else if (haveEnd && end > loopBegin) loopLength = static_cast<u32>(end - loopBegin);
    else loopLength = 0;
    return true;
}

bool loopIntoWindow(u64 startFrame, u64 durationFrames, u64& loopBegin, u64& loopEnd) {
    if (loopEnd != 0 && loopEnd <= startFrame) return false;
    loopBegin = loopBegin > startFrame ? loopBegin - startFrame : 0;
    if (loopEnd != 0) loopEnd -= startFrame;
    if (durationFrames != 0) {
        if (loopBegin >= durationFrames) return false;
        if (loopEnd > durationFrames) loopEnd = durationFrames;
    }
    if (loopEnd != 0 && loopEnd <= loopBegin) return false;
    return loopBegin <= 0xFFFFFFFFull && loopEnd <= 0xFFFFFFFFull;
}

std::unique_ptr<audio::StreamSource> makeWindowedSource(std::unique_ptr<audio::StreamSource> inner, u64 startFrame,
                                                         u64 durationFrames) {
    if (!inner) return nullptr;
    return std::make_unique<WindowedSource>(std::move(inner), startFrame, durationFrames);
}

DecodedStream openDecodedFile(const std::string& utf8Path, const DecodeOptions& opt, std::string* why) {
    const std::vector<u8> head = readHead(utf8Path, 16);
    if (head.empty()) { failWith(why, "could not open the file, or it is empty"); return {}; }
    const AudioCodec codec = chooseCodec(utf8Path, head.data(), head.size());
    AudioInput in;
    in.path = utf8Path;
    return openCodec(codec, std::move(in), opt, why);
}

DecodedStream openDecodedMemory(std::vector<u8> bytes, AudioCodec codec, const DecodeOptions& opt, std::string* why) {
    if (bytes.empty()) { failWith(why, "no data"); return {}; }
    if (codec == AudioCodec::None) codec = sniffAudioCodec(bytes.data(), bytes.size());
    AudioInput in;
    in.memory = true;
    in.bytes = std::move(bytes);
    return openCodec(codec, std::move(in), opt, why);
}

bool decodeSourceToSound(audio::StreamSource& src, audio::SoundData& out, std::string* why, u64 maxSamples) {
    out = audio::SoundData{};
    const u32 ch = src.channels();
    if (ch < 1 || ch > 2) return failWith(why, "the decoder reported an unusable channel count");
    out.channels = ch;
    out.sampleRate = src.sampleRate();
    constexpr u32 kChunk = 4096;
    const u64 hint = src.totalFrames() * ch;
    out.samples.reserve(static_cast<usize>(std::min<u64>({hint, maxSamples, 1ull << 22})));
    std::vector<f32> chunk(static_cast<usize>(kChunk) * ch);
    for (;;) {
        const u32 n = src.read(chunk.data(), kChunk);
        if (n == 0) break;
        const usize count = static_cast<usize>(n) * ch;
        const usize need = out.samples.size() + count;
        if (need > maxSamples) {
            out = audio::SoundData{};
            return failWith(why, "too long to hold in memory; stream it instead");
        }
        // Grow by half again, never past the limit, so the capacity cannot overshoot it.
        if (need > out.samples.capacity())
            out.samples.reserve(static_cast<usize>(std::min<u64>(maxSamples, std::max<u64>(need, out.samples.size() + out.samples.size() / 2))));
        out.samples.insert(out.samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count));
    }
    if (out.samples.empty()) return failWith(why, "the file decoded to no audio");
    return true;
}

namespace {

bool toSound(DecodedStream ds, audio::SoundData& out, std::string* why, AudioCodec* codecOut, u64 maxSamples) {
    if (!ds.source) return false;
    if (codecOut) *codecOut = ds.codec;
    if (!decodeSourceToSound(*ds.source, out, why, maxSamples)) return false;
    const u32 frames = out.frames();
    out.loopBegin = ds.loopBegin;
    out.loopEnd = ds.loopEnd;
    if (out.loopBegin >= frames) out.loopBegin = out.loopEnd = 0;
    if (out.loopEnd > frames) out.loopEnd = frames;
    return true;
}

} // namespace

bool decodeAudioFile(const std::string& utf8Path, audio::SoundData& out, std::string* why, AudioCodec* codecOut,
                     u64 maxSamples) {
    return toSound(openDecodedFile(utf8Path, {}, why), out, why, codecOut, maxSamples);
}

bool decodeAudioMemory(std::vector<u8> bytes, AudioCodec codec, audio::SoundData& out, std::string* why, u64 maxSamples) {
    return toSound(openDecodedMemory(std::move(bytes), codec, {}, why), out, why, nullptr, maxSamples);
}

} // namespace aver::fmt
