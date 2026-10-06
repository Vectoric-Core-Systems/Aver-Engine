// File-backed stream sources for the audio ABI.
#include "StreamSources.hpp"
#include "aver/formats/OcAudio.hpp"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mfapi.h>
#  include <mfidl.h>
#  include <mfreadwrite.h>
#  include <mferror.h>
#endif

namespace aver::audio {
namespace {

std::string lowerExt(const std::string& path) {
    const usize dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return e;
}

} // namespace

#if defined(_WIN32)
namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool mfStart() {
    static const bool started = [] { return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)); }();
    return started;
}

// MF wants COM on the thread it runs on. MTA, once per thread; a different mode already set is fine.
void ensureCom() {
    thread_local bool done = false;
    if (!done) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); done = true; }
}

class MfStreamSource final : public StreamSource {
public:
    ~MfStreamSource() override { release(reader_); }
    bool open(const std::string& utf8Path, std::string* why);
    u32 channels() const override { return channels_; }
    u32 sampleRate() const override { return rate_; }
    u32 read(f32* dst, u32 maxFrames) override;
    bool seek(u64 frame) override;

private:
    bool refill();

    IMFSourceReader* reader_ = nullptr;
    u32 channels_ = 0, rate_ = 0;
    std::vector<f32> pending_;
    usize pendingPos_ = 0;       // in samples
    bool endOfStream_ = false;
};

bool MfStreamSource::open(const std::string& utf8Path, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (!mfStart()) return fail("Media Foundation would not start");
    ensureCom();

    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wide <= 0) return fail("the path is not valid UTF-8");
    std::wstring wpath(static_cast<usize>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, wpath.data(), wide);

    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader_)) || !reader_)
        return fail("no decoder for this file");

    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    // First ask for float PCM as the file has it; fold down to stereo if that has more channels.
    for (int attempt = 0; attempt < 2; ++attempt) {
        IMFMediaType* want = nullptr;
        if (FAILED(MFCreateMediaType(&want))) return fail("out of memory");
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (attempt == 1) want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        const HRESULT hr = reader_->SetCurrentMediaType(stream, nullptr, want);
        release(want);
        if (FAILED(hr)) return fail("the file has no audio stream this machine can decode");

        IMFMediaType* got = nullptr;
        UINT32 ch = 0, rate = 0;
        if (SUCCEEDED(reader_->GetCurrentMediaType(stream, &got)) && got) {
            got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            release(got);
        }
        channels_ = ch;
        rate_ = rate;
        if (ch == 1 || ch == 2) break;
        if (attempt == 1) return fail("more than two channels and the decoder would not fold them down");
    }
    if (rate_ == 0) return fail("the decoder reported no sample rate");
    return true;
}

// Decodes the next MF sample into pending_. False at the end of the stream or on failure.
bool MfStreamSource::refill() {
    pending_.clear();
    pendingPos_ = 0;
    while (!endOfStream_) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
                                       &flags, nullptr, &sample))) {
            endOfStream_ = true;
            break;
        }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) {
            release(sample);
            endOfStream_ = true;
            break;
        }
        if (!sample) continue;

        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            BYTE* data = nullptr; DWORD len = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &len)) && data) {
                const usize count = (len / sizeof(f32) / channels_) * channels_;
                pending_.assign(reinterpret_cast<const f32*>(data), reinterpret_cast<const f32*>(data) + count);
                buffer->Unlock();
            }
            release(buffer);
        }
        release(sample);
        if (!pending_.empty()) return true;
    }
    return false;
}

u32 MfStreamSource::read(f32* dst, u32 maxFrames) {
    ensureCom();
    u32 produced = 0;
    while (produced < maxFrames) {
        if (pendingPos_ >= pending_.size() && !refill()) break;
        const usize availFrames = (pending_.size() - pendingPos_) / channels_;
        const u32 n = static_cast<u32>(std::min<usize>(availFrames, maxFrames - produced));
        std::memcpy(dst + static_cast<usize>(produced) * channels_, pending_.data() + pendingPos_,
                    static_cast<usize>(n) * channels_ * sizeof(f32));
        pendingPos_ += static_cast<usize>(n) * channels_;
        produced += n;
    }
    return produced;
}

bool MfStreamSource::seek(u64 frame) {
    ensureCom();
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = static_cast<LONGLONG>(frame * 10000000ull / rate_);
    GUID timeFormat = {};      // GUID_NULL: 100 ns units
    const HRESULT hr = reader_->SetCurrentPosition(timeFormat, var);
    PropVariantClear(&var);
    if (FAILED(hr)) return false;
    pending_.clear();
    pendingPos_ = 0;
    endOfStream_ = false;
    return true;
}

} // namespace

std::unique_ptr<StreamSource> openMfStream(const std::string& utf8Path, std::string* why) {
    auto src = std::make_unique<MfStreamSource>();
    if (!src->open(utf8Path, why)) return nullptr;
    return src;
}
#endif // _WIN32

OpenedStream openStreamSource(const std::string& path, std::string* why) {
    OpenedStream out;
    const std::string ext = lowerExt(path);

    if (ext == ".wav") {
        std::string wavWhy;
        out.source = openWavStream(path.c_str(), &wavWhy);
        if (out.source) { out.decoder = "wav"; return out; }
        if (why) *why = wavWhy;
    }

    if (ext == ".ocaudio") {
        SoundData data;
        std::string e;
        if (!aver::fmt::loadOcAudio(path, data, &e)) { if (why) *why = e; return out; }
        out.loopBegin = data.loopBegin;
        out.loopEnd = data.loopEnd;
        out.source = std::make_unique<MemoryStreamSource>(std::move(data));
        out.decoder = "resident";
        return out;
    }

#if defined(_WIN32)
    out.source = openMfStream(path, why);
    if (out.source) { out.decoder = "media foundation"; return out; }
#endif

    // No incremental decoder took it: decode whole and replay from memory.
    SoundData data;
    const aver::fmt::AudioImportResult r = aver::fmt::audioImportFile(path, data);
    if (!r.ok) { if (why) *why = r.error; return out; }
    if (data.channels > 2) { if (why) *why = "more than two channels"; return out; }
    out.source = std::make_unique<MemoryStreamSource>(std::move(data));
    out.decoder = "resident";
    return out;
}

} // namespace aver::audio
