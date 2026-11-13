// VideoEncoder over Windows Media Foundation's sink writer (H.264 into MP4). Elsewhere a stub that
// says Unsupported, so a caller picks a different output instead of failing to link.
#include "aver/media/VideoEncoder.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace aver::media {

namespace {

using Microsoft::WRL::ComPtr;

std::string hr(const char* what, HRESULT code) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed (0x%08lX)", what, static_cast<unsigned long>(code));
    return buf;
}

// The codes Media Foundation gives when no H.264 encoder (or no usable format) is installed.
bool noEncoder(HRESULT code) {
    return code == MF_E_TOPO_CODEC_NOT_FOUND || code == MF_E_NOT_FOUND || code == MF_E_INVALIDMEDIATYPE ||
           code == MF_E_UNSUPPORTED_FORMAT || code == MF_E_TRANSFORM_TYPE_NOT_SET || code == E_NOTIMPL ||
           code == MF_E_TRANSFORM_NOT_POSSIBLE_FOR_CURRENT_MEDIATYPE_COMBINATION;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

class MfEncoder final : public VideoEncoder {
public:
    ~MfEncoder() override {
        std::string ignored;
        if (!finished_) finish(ignored);
    }

    OpenResult init(const std::string& path, const VideoEncoderDesc& d, std::string& why) {
        d_ = d;
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInit_ = SUCCEEDED(co);   // S_FALSE counts: it still has to be balanced. RPC_E_CHANGED_MODE does not.
        HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(h)) { why = hr("MFStartup", h); release(); return OpenResult::Unsupported; }
        mfStarted_ = true;

        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 2);
        if (attrs) attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);   // offline: never drop to keep up
        h = MFCreateSinkWriterFromURL(widen(path).c_str(), nullptr, attrs.Get(), &writer_);
        if (FAILED(h)) { why = hr("MFCreateSinkWriterFromURL", h); release(); return OpenResult::Failed; }

        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, d.bitrateKbps * 1000u);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, d.width, d.height);
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, d.fpsNum, d.fpsDen);
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        h = writer_->AddStream(out.Get(), &stream_);
        if (FAILED(h)) { why = hr("AddStream (H.264)", h); release(); return noEncoder(h) ? OpenResult::Unsupported : OpenResult::Failed; }

        // RGB32 is bottom-up unless a stride says otherwise, so addFrame() flips; the sink writer's own
        // converter turns it into the encoder's NV12.
        ComPtr<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, d.width, d.height);
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, d.fpsNum, d.fpsDen);
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        h = writer_->SetInputMediaType(stream_, in.Get(), nullptr);
        if (FAILED(h)) { why = hr("SetInputMediaType", h); release(); return noEncoder(h) ? OpenResult::Unsupported : OpenResult::Failed; }

        h = writer_->BeginWriting();
        if (FAILED(h)) { why = hr("BeginWriting", h); release(); return noEncoder(h) ? OpenResult::Unsupported : OpenResult::Failed; }
        return OpenResult::Ok;
    }

    bool addFrame(const u8* rgba, u32 strideBytes) override {
        if (!writer_ || finished_ || !rgba) return false;
        const u32 w = d_.width, h = d_.height;
        const u32 stride = strideBytes ? strideBytes : w * 4u;
        const u64 bytes64 = u64(w) * h * 4u;
        if (bytes64 > 0x7FFFFFFFull) return false;
        const DWORD bytes = static_cast<DWORD>(bytes64);
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return false;
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
        for (u32 y = 0; y < h; ++y) {   // bottom-up BGRX from top-down RGBA
            const u8* s = rgba + static_cast<size_t>(y) * stride;
            u8* o = dst + static_cast<size_t>(h - 1 - y) * w * 4u;
            for (u32 x = 0; x < w; ++x) {
                o[x * 4 + 0] = s[x * 4 + 2];
                o[x * 4 + 1] = s[x * 4 + 1];
                o[x * 4 + 2] = s[x * 4 + 0];
                o[x * 4 + 3] = 0xFF;
            }
        }
        buf->Unlock();
        buf->SetCurrentLength(bytes);

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) return false;
        sample->AddBuffer(buf.Get());
        const auto at = [&](u64 n) {   // 100 ns units, rounded from the exact fraction
            return static_cast<LONGLONG>(n * 10000000ull * d_.fpsDen / d_.fpsNum);
        };
        const LONGLONG t = at(frames_);
        sample->SetSampleTime(t);
        sample->SetSampleDuration(at(frames_ + 1) - t);
        if (FAILED(writer_->WriteSample(stream_, sample.Get()))) return false;
        ++frames_;
        return true;
    }

    bool finish(std::string& why) override {
        if (finished_) return true;
        finished_ = true;
        bool ok = true;
        if (writer_) {
            const HRESULT h = writer_->Finalize();
            if (FAILED(h)) { why = hr("Finalize", h); ok = false; }
        }
        release();
        return ok;
    }

    u32 framesWritten() const override { return frames_; }

private:
    void release() {
        writer_.Reset();
        if (mfStarted_) { MFShutdown(); mfStarted_ = false; }
        if (comInit_) { CoUninitialize(); comInit_ = false; }
    }

    VideoEncoderDesc d_;
    ComPtr<IMFSinkWriter> writer_;
    DWORD stream_ = 0;
    u32 frames_ = 0;
    bool mfStarted_ = false, comInit_ = false, finished_ = false;
};

} // namespace

OpenResult VideoEncoder::open(const std::string& path, const VideoEncoderDesc& desc,
                              std::unique_ptr<VideoEncoder>& out, std::string& why) {
    out.reset();
    if (desc.width < 16 || desc.height < 16 || (desc.width & 1u) || (desc.height & 1u)) {
        why = "H.264 needs even dimensions of at least 16 pixels";
        return OpenResult::Failed;
    }
    if (desc.fpsNum == 0 || desc.fpsDen == 0 || desc.bitrateKbps == 0) {
        why = "bad frame rate or bitrate";
        return OpenResult::Failed;
    }
    auto enc = std::make_unique<MfEncoder>();
    const OpenResult r = enc->init(path, desc, why);
    if (r == OpenResult::Ok) out = std::move(enc);
    return r;
}

} // namespace aver::media

#else  // !_WIN32

namespace aver::media {

OpenResult VideoEncoder::open(const std::string&, const VideoEncoderDesc&, std::unique_ptr<VideoEncoder>& out,
                              std::string& why) {
    out.reset();
    why = "video output needs Windows Media Foundation";
    return OpenResult::Unsupported;
}

} // namespace aver::media

#endif
