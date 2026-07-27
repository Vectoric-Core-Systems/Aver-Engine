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

// The period asked for, in 100-ns units. 10 ms at 48 kHz is 480 frames, which is the number
// docs/AUDIO.md §3 committed to. WASAPI may grant something else -- it usually grants its own
// default period in shared mode -- so what is actually granted is read back and reported rather than
// assumed.
constexpr REFERENCE_TIME kRequestedPeriod = 100000;   // 10 ms

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Whether a device mix format is 32-bit float, which is what WASAPI hands back on every machine this
// is likely to meet -- but "likely" is not "always", and the fallback below is what stops an
// integer-format device from being handed float samples and reproducing them as full-scale noise.
bool isFloatFormat(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

bool isPcm16Format(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_PCM) return wf->wBitsPerSample == 16;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && wf->wBitsPerSample == 16;
    }
    return false;
}

} // namespace

AudioDevice::~AudioDevice() { stop(); }

bool AudioDevice::start(u32 maxVoices, u32 maxSounds) {
    if (running_.load(std::memory_order_acquire)) return true;
    quit_.store(false, std::memory_order_release);

    // The device is opened ON the render thread, and the mixer initialised there too, because the
    // format is the device's to state: everything downstream needs the rate and channel count, and
    // there is no honest value to give them until WASAPI has been asked.
    //
    // A short handshake so start() can report a real failure rather than "it might work": the thread
    // sets `running_` (or leaves it false) and signals, and this returns whichever it was.
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;

    readyEvent_ = ready;
    maxVoices_  = maxVoices;
    maxSounds_  = maxSounds;
    thread_ = std::thread([this] { threadMain(); });

    // Bounded, so a driver that never answers costs five seconds rather than the process.
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    readyEvent_ = nullptr;

    if (!running_.load(std::memory_order_acquire)) {
        quit_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void AudioDevice::stop() {
    quit_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    // AFTER the thread has joined, never before: the mixer owns memory the render thread reads, and
    // tearing it down under a live callback is a use-after-free however carefully it is written.
    mixer_.shutdown();
}

void AudioDevice::threadMain() {
    // MTA, and initialised on this thread rather than inherited: the render thread outlives whatever
    // called start(), and a COM apartment belongs to the thread that entered it.
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
        // Not an error. A machine with no sound card is a legitimate configuration -- a build server
        // is one -- and the engine runs silent rather than refusing to start.
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
        // Refused rather than guessed. Handing float samples to a device expecting 24-bit packed
        // integers does not sound wrong, it sounds like full-scale noise.
        finish(false, "the device mix format is neither 32-bit float nor 16-bit PCM"); signalReady(); return;
    }

    const u32 deviceChannels = format->nChannels;
    const u32 deviceRate     = format->nSamplesPerSec;
    // The mixer works in mono or stereo; a device with more channels gets the stereo pair in its
    // first two and silence in the rest. Upmixing properly is a spatialiser's job, not a mixer's,
    // and putting a fake one here would be worse than an honest stereo feed.
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
    // Allocated ONCE, here, because the loop below may not allocate. Sized to the whole device
    // buffer so no period can ask for more than is already reserved.
    scratch.assign(static_cast<usize>(bufferFrames) * mixChannels, 0.0f);
    bufferFrames_ = bufferFrames;

    // Pro Audio scheduling. Without it the render thread is an ordinary one, and an ordinary thread
    // preempted for 10 ms is a gap somebody hears.
    DWORD taskIndex = 0;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    client->Start();
    running_.store(true, std::memory_order_release);
    AVER_INFO("[Audio] WASAPI output: {} Hz, {} device channel(s), mixing {}, {} frame buffer ({:.1f} ms)",
              deviceRate, deviceChannels, mixChannels == 2 ? "stereo" : "mono", bufferFrames,
              1000.0 * double(bufferFrames) / double(deviceRate));
    signalReady();

    while (!quit_.load(std::memory_order_acquire)) {
        // The device signals when it wants more. A timeout here means it stopped asking, which is a
        // device change or a driver fault -- either way, ending the loop is better than spinning.
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
            // 16-bit PCM. 32767 rather than 32768 so +1.0 maps to the largest representable positive
            // value instead of wrapping to the largest NEGATIVE one -- which is a full-scale click on
            // exactly the loudest sample in the mix.
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
