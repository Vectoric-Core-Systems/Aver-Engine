// WASAPI shared-mode output: opens the default endpoint and pumps the mixer from a render thread.
#include "aver/audio/AudioDevice.hpp"
#include "aver/core/Log.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <vector>
#include <cstring>

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

} // namespace

// Stops the render thread and tears the mixer down.
AudioDevice::~AudioDevice() { stop(); }

// Starts the render thread and waits for it to report whether a device opened.
bool AudioDevice::start(u32 maxVoices, u32 maxSounds) {
    if (running_.load(std::memory_order_acquire)) return true;
    quit_.store(false, std::memory_order_release);

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;

    readyEvent_ = ready;
    maxVoices_  = maxVoices;
    maxSounds_  = maxSounds;
    thread_ = std::thread([this] { threadMain(); });

    const DWORD waited = WaitForSingleObject(ready, 5000);

    // THE THREAD IS JOINED BEFORE THE HANDLE IS CLOSED. On the timeout path the old order closed
    // `ready` while the render thread was still starting up and might yet SetEvent(readyEvent_) --
    // signalling a closed handle, or worse a handle value the OS had already reused for something
    // else. It only happened when start-up took longer than five seconds, which is exactly when a
    // machine is least able to survive it.
    if (waited != WAIT_OBJECT_0 || !running_.load(std::memory_order_acquire)) {
        quit_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        readyEvent_ = nullptr;
        CloseHandle(ready);
        return false;
    }

    // Success: the thread has signalled and is past its start-up, so the handle is finished with.
    readyEvent_ = nullptr;
    CloseHandle(ready);
    return true;
}

// Joins the render thread, then shuts the mixer down. Safe when never started.
void AudioDevice::stop() {
    quit_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    mixer_.shutdown();
}

// The render thread: opens the endpoint, initialises the mixer at its format, then fills buffers.
void AudioDevice::threadMain() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(co);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioRenderClient*  render     = nullptr;
    WAVEFORMATEX*        format     = nullptr;
    HANDLE               bufferEvent = nullptr;
    HANDLE               mmcss      = nullptr;
    std::vector<f32>     scratch;

    auto finish = [&](bool ok, const char* why) {
        if (!ok && why) AVER_WARN("[Audio] no output device: {}", why);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (client) client->Stop();
        release(render);
        if (format) { CoTaskMemFree(format); format = nullptr; }
        release(client);
        release(device);
        release(enumerator);
        if (bufferEvent) CloseHandle(bufferEvent);
        if (ownsCom) CoUninitialize();
    };

    auto signalReady = [&] { if (readyEvent_) SetEvent(static_cast<HANDLE>(readyEvent_)); };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
        finish(false, "no device enumerator"); signalReady(); return;
    }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        finish(false, "no default output endpoint"); signalReady(); return;
    }
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client)))) {
        finish(false, "could not activate the audio client"); signalReady(); return;
    }
    if (FAILED(client->GetMixFormat(&format)) || !format) {
        finish(false, "no mix format"); signalReady(); return;
    }

    const bool floatFmt = isFloatFormat(format);
    const bool pcm16    = isPcm16Format(format);
    if (!floatFmt && !pcm16) {
        finish(false, "the device mix format is neither 32-bit float nor 16-bit PCM"); signalReady(); return;
    }

    const u32 deviceChannels = format->nChannels;
    const u32 deviceRate     = format->nSamplesPerSec;
    // The mixer works in mono or stereo; extra device channels get silence.
    const u32 mixChannels = deviceChannels >= 2 ? 2u : 1u;

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defaultPeriod, &minPeriod);
    const REFERENCE_TIME period = kRequestedPeriod > minPeriod ? kRequestedPeriod : minPeriod;

    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  period, 0, format, nullptr))) {
        finish(false, "IAudioClient::Initialize failed"); signalReady(); return;
    }

    bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!bufferEvent || FAILED(client->SetEventHandle(bufferEvent))) {
        finish(false, "could not attach the buffer event"); signalReady(); return;
    }

    UINT32 bufferFrames = 0;
    if (FAILED(client->GetBufferSize(&bufferFrames)) || bufferFrames == 0) {
        finish(false, "zero buffer size"); signalReady(); return;
    }
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)))) {
        finish(false, "no render client"); signalReady(); return;
    }

    if (!mixer_.init(deviceRate, mixChannels, maxVoices_, maxSounds_)) {
        finish(false, "the mixer refused the device format"); signalReady(); return;
    }
    // Allocated once, before the loop: the loop below must not allocate.
    scratch.assign(static_cast<usize>(bufferFrames) * mixChannels, 0.0f);
    bufferFrames_ = bufferFrames;

    DWORD taskIndex = 0;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    client->Start();
    running_.store(true, std::memory_order_release);
    AVER_INFO("[Audio] WASAPI output: {} Hz, {} device channel(s), mixing {}, {} frame buffer ({:.1f} ms)",
              deviceRate, deviceChannels, mixChannels == 2 ? "stereo" : "mono", bufferFrames,
              1000.0 * double(bufferFrames) / double(deviceRate));
    signalReady();

    while (!quit_.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(bufferEvent, 2000) != WAIT_OBJECT_0) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 want = bufferFrames > padding ? bufferFrames - padding : 0;
        if (want == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(want, &out)) || !out) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        mixer_.mix(scratch.data(), want);

        if (floatFmt && deviceChannels == mixChannels) {
            std::memcpy(out, scratch.data(), static_cast<usize>(want) * mixChannels * sizeof(f32));
        } else if (floatFmt) {
            f32* dst = reinterpret_cast<f32*>(out);
            for (UINT32 f = 0; f < want; ++f) {
                for (u32 c = 0; c < deviceChannels; ++c)
                    dst[f * deviceChannels + c] = c < mixChannels ? scratch[f * mixChannels + c] : 0.0f;
            }
        } else {
            i16* dst = reinterpret_cast<i16*>(out);
            for (UINT32 f = 0; f < want; ++f) {
                for (u32 c = 0; c < deviceChannels; ++c) {
                    const f32 s = c < mixChannels ? scratch[f * mixChannels + c] : 0.0f;
                    dst[f * deviceChannels + c] = static_cast<i16>(s * 32767.0f);
                }
            }
        }
        render->ReleaseBuffer(want, 0);
    }

    running_.store(false, std::memory_order_release);
    finish(true, nullptr);
}

} // namespace aver::audio
