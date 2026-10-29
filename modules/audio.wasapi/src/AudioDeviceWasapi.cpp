// WASAPI shared-mode output: the OutputBackend for Windows. Opens the default endpoint, fills it from the render thread's step() and
// reports a lost or replaced endpoint to the policy (OutputDriver.hpp), which rebuilds it. Nothing here decides anything about devices.
#include "aver/audio/AudioDevice.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace aver::audio {
namespace {

// The period asked for, in 100-ns units. WASAPI may grant something else.
constexpr REFERENCE_TIME kRequestedPeriod = 100000;   // 10 ms

// Releases a COM pointer and nulls it.
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// True when a mix format is 32-bit IEEE float.
bool isFloatFormat(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// True when a mix format is 16-bit PCM.
bool isPcm16Format(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_PCM) return wf->wBitsPerSample == 16;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && wf->wBitsPerSample == 16;
    }
    return false;
}

// The failures that mean the endpoint is gone and a new one must be opened (unplugged, disabled, format changed, audio service restarted).
bool isInvalidated(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING || hr == AUDCLNT_E_NOT_INITIALIZED;
}

std::string toUtf8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1));
    return s;
}

// PKEY_Device_FriendlyName ({a45c254e-df1c-4efd-8020-67d146a850e0}, 14), spelled out so no key table has to be linked.
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// Default-device and endpoint-state notifications. They arrive on COM threads: they set a flag and wake the render thread, nothing else.
class Notify final : public IMMNotificationClient {
public:
    std::atomic<bool> lost{false};
    std::atomic<bool> defaultChanged{false};
    HANDLE wake = nullptr;
    std::wstring endpointId;      // the open endpoint's id; written before registration, never while registered

    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }          // owned by the backend, unregistered before it dies
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) { *out = static_cast<IMMNotificationClient*>(this); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (state != DEVICE_STATE_ACTIVE && ours(id)) raise(lost);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (ours(id)) raise(lost);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        // Only the playback default the engine opened; a notification for the endpoint already open (it arrives after a loss we handled) is not a change.
        if (flow == eRender && role == eConsole && !ours(id)) raise(defaultChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    bool ours(LPCWSTR id) const { return id && endpointId == id; }
    void raise(std::atomic<bool>& flag) {
        flag.store(true, std::memory_order_release);
        if (wake) SetEvent(wake);
    }
};

class WasapiBackend final : public OutputBackend {
public:
    ~WasapiBackend() override { close(); }

    void threadBegin() override {
        ownsCom_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        DWORD taskIndex = 0;
        mmcss_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    }
    void threadEnd() override {
        if (mmcss_) { AvRevertMmThreadCharacteristics(mmcss_); mmcss_ = nullptr; }
        if (ownsCom_) { CoUninitialize(); ownsCom_ = false; }
    }

    bool open(OutputInfo& info) override {
        close();
        auto fail = [&](const char* why) { lastError_ = why; close(); return false; };

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator_))))
            return fail("no device enumerator");
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_)))
            return fail("no default output endpoint");

        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bufferEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_ || !bufferEvent_) return fail("could not create the wait events");

        // Registered after the endpoint is known so its own default-changed notification is not mistaken for a change.
        LPWSTR id = nullptr;
        notify_.endpointId.clear();
        if (SUCCEEDED(device_->GetId(&id)) && id) { notify_.endpointId = id; CoTaskMemFree(id); }
        notify_.lost.store(false, std::memory_order_relaxed);
        notify_.defaultChanged.store(false, std::memory_order_relaxed);
        notify_.wake = wake_;
        registered_ = SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(&notify_));

        if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_))))
            return fail("could not activate the audio client");
        if (FAILED(client_->GetMixFormat(&format_)) || !format_) return fail("no mix format");

        floatFmt_ = isFloatFormat(format_);
        const bool pcm16 = isPcm16Format(format_);
        if (!floatFmt_ && !pcm16) return fail("the device mix format is neither 32-bit float nor 16-bit PCM");
        deviceChannels_ = format_->nChannels;
        // The speaker mask, when the format carries one: the policy chooses 5.1 / 7.1 from it and start() builds the channel map from it.
        deviceMask_ = format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format_)->dwChannelMask : 0u;

        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        client_->GetDevicePeriod(&defaultPeriod, &minPeriod);
        const REFERENCE_TIME period = kRequestedPeriod > minPeriod ? kRequestedPeriod : minPeriod;
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                       AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       period, 0, format_, nullptr)))
            return fail("IAudioClient::Initialize failed");
        if (FAILED(client_->SetEventHandle(bufferEvent_))) return fail("could not attach the buffer event");

        UINT32 frames = 0;
        if (FAILED(client_->GetBufferSize(&frames)) || frames == 0) return fail("zero buffer size");
        bufferFrames_ = frames;
        if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_))))
            return fail("no render client");

        info = OutputInfo{};
        info.rate = format_->nSamplesPerSec;
        info.deviceChannels = deviceChannels_;
        info.channelMask = deviceMask_;
        info.bufferFrames = frames;
        REFERENCE_TIME latency = 0;
        if (SUCCEEDED(client_->GetStreamLatency(&latency)) && latency > 0)
            info.latencyMs = static_cast<float>(latency) / 10000.0f + 1000.0f * static_cast<float>(frames) / static_cast<float>(info.rate);
        info.name = friendlyName();
        lastError_.clear();
        return true;
    }

    bool start(const OutputInfo& info, OutputSource& source) override {
        mixChannels_ = info.mixChannels;
        // Allocated here, before the first buffer: service() must not allocate.
        scratch_.assign(static_cast<size_t>(bufferFrames_) * mixChannels_, 0.0f);
        map_.assign(deviceChannels_ ? deviceChannels_ : 1u, static_cast<i8>(-1));
        buildDeviceMap(mixChannels_, deviceChannels_, deviceMask_, map_.data(), static_cast<u32>(map_.size()));
        straight_ = true;
        for (u32 c = 0; c < deviceChannels_; ++c) if (map_[c] != static_cast<i8>(c)) straight_ = false;
        source_ = &source;
        if (!client_ || FAILED(client_->Start())) {
            lastError_ = "IAudioClient::Start failed";
            source_ = nullptr;
            return false;
        }
        return true;
    }

    void close() override {
        source_ = nullptr;
        if (client_) client_->Stop();
        if (enumerator_ && registered_) enumerator_->UnregisterEndpointNotificationCallback(&notify_);   // waits for a callback in flight
        registered_ = false;
        notify_.wake = nullptr;
        release(render_);
        if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
        release(client_);
        release(device_);
        release(enumerator_);
        if (bufferEvent_) { CloseHandle(bufferEvent_); bufferEvent_ = nullptr; }
        if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
    }

    ServiceResult service(u32 timeoutMs) override {
        if (!client_ || !render_ || !source_) return ServiceResult::Lost;
        if (const ServiceResult n = pending(); n != ServiceResult::Ok) return n;

        const HANDLE handles[2] = {bufferEvent_, wake_};
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
        if (waited == WAIT_TIMEOUT) return ServiceResult::Idle;
        if (waited == WAIT_OBJECT_0 + 1) return pending();
        if (waited != WAIT_OBJECT_0) return ServiceResult::Lost;
        if (const ServiceResult n = pending(); n != ServiceResult::Ok) return n;

        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) return ServiceResult::Lost;
        const UINT32 want = bufferFrames_ > padding ? bufferFrames_ - padding : 0;
        if (want == 0) return ServiceResult::Ok;

        BYTE* out = nullptr;
        hr = render_->GetBuffer(want, &out);
        if (FAILED(hr) || !out) {
            if (isInvalidated(hr)) return ServiceResult::Lost;
            source_->underrun();
            return ServiceResult::Ok;
        }

        source_->render(scratch_.data(), want);

        // map_[d] is the mixer channel feeding device channel d (-1: silence); same count and no reordering is a straight copy.
        if (floatFmt_ && deviceChannels_ == mixChannels_ && straight_) {
            std::memcpy(out, scratch_.data(), static_cast<size_t>(want) * mixChannels_ * sizeof(float));
        } else if (floatFmt_) {
            float* dst = reinterpret_cast<float*>(out);
            for (UINT32 f = 0; f < want; ++f)
                for (u32 c = 0; c < deviceChannels_; ++c)
                    dst[f * deviceChannels_ + c] = map_[c] >= 0 ? scratch_[f * mixChannels_ + static_cast<u32>(map_[c])] : 0.0f;
        } else {
            short* dst = reinterpret_cast<short*>(out);
            for (UINT32 f = 0; f < want; ++f)
                for (u32 c = 0; c < deviceChannels_; ++c) {
                    const float s = map_[c] >= 0 ? scratch_[f * mixChannels_ + static_cast<u32>(map_[c])] : 0.0f;
                    dst[f * deviceChannels_ + c] = static_cast<short>(s * 32767.0f);
                }
        }
        hr = render_->ReleaseBuffer(want, 0);
        if (FAILED(hr) && isInvalidated(hr)) return ServiceResult::Lost;
        return ServiceResult::Ok;
    }

    const char* lastError() const override { return lastError_.c_str(); }

private:
    // A notification that arrived since the last look.
    ServiceResult pending() {
        if (notify_.lost.exchange(false, std::memory_order_acq_rel)) return ServiceResult::Lost;
        if (notify_.defaultChanged.exchange(false, std::memory_order_acq_rel)) return ServiceResult::DefaultChanged;
        return ServiceResult::Ok;
    }

    std::string friendlyName() {
        std::string name;
        IPropertyStore* props = nullptr;
        if (device_ && SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = toUtf8(v.pwszVal);
            PropVariantClear(&v);
            props->Release();
        }
        return name;
    }

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    WAVEFORMATEX* format_ = nullptr;
    HANDLE bufferEvent_ = nullptr;
    HANDLE wake_ = nullptr;
    HANDLE mmcss_ = nullptr;
    Notify notify_;
    bool registered_ = false;
    bool ownsCom_ = false;
    bool floatFmt_ = false;
    u32 deviceChannels_ = 0, mixChannels_ = 0, bufferFrames_ = 0, deviceMask_ = 0;
    bool straight_ = true;
    OutputSource* source_ = nullptr;
    std::vector<float> scratch_;
    std::vector<i8> map_;
    std::string lastError_;
};

} // namespace

// Opens the default output and starts the render thread. False when there is no output device.
bool AudioDevice::start(u32 maxVoices, u32 maxSounds) {
    OutputConfig config;
    config.maxVoices = maxVoices;
    config.maxSounds = maxSounds;
    return startWith(std::make_unique<WasapiBackend>(), nullptr, config);
}

} // namespace aver::audio
