#include "aver/platform/Speech.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace aver::platform {
namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One worker thread owns the ISpVoice (COM multithreaded), created on the first command so startup never waits on
// COM. speak() only enqueues.
class SapiSpeech final : public SpeechSynth {
public:
    SapiSpeech() : th_([this] { run(); }) {}
    ~SapiSpeech() override {
        push(Cmd{Cmd::Quit});
        if (th_.joinable()) th_.join();
    }

    bool available() const override {
        if (avail_.load() < 0) {
            const_cast<SapiSpeech*>(this)->push(Cmd{Cmd::Init});
            std::unique_lock<std::mutex> lk(availMutex_);
            availCv_.wait_for(lk, std::chrono::seconds(3), [this] { return avail_.load() >= 0; });
        }
        return avail_.load() > 0;
    }
    void speak(std::string_view utf8, bool interrupt) override {
        if (utf8.empty()) return;
        Cmd c{Cmd::Speak};
        c.text = widen(utf8);
        c.interrupt = interrupt;
        if (c.text.empty()) return;
        pending_.fetch_add(1);
        push(std::move(c), interrupt);
    }
    void stop() override { push(Cmd{Cmd::Stop}, true); }
    bool speaking() const override { return pending_.load() > 0 || speaking_.load(); }
    void setRate(int r) override { Cmd c{Cmd::Rate}; c.ival = r < -10 ? -10 : (r > 10 ? 10 : r); push(std::move(c)); }
    void setVolume(float v) override {
        Cmd c{Cmd::Volume};
        c.ival = static_cast<int>((v < 0 ? 0.0f : (v > 1 ? 1.0f : v)) * 100.0f + 0.5f);
        push(std::move(c));
    }

private:
    struct Cmd {
        enum Type { Init, Speak, Stop, Rate, Volume, Quit } type = Init;
        std::wstring text;
        bool interrupt = false;
        int ival = 0;
    };

    // `purge`: queued speech that has not started is dropped first (interrupt and stop).
    void push(Cmd c, bool purge = false) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (purge) {
                for (auto it = q_.begin(); it != q_.end();) {
                    if (it->type == Cmd::Speak) { pending_.fetch_sub(1); it = q_.erase(it); }
                    else ++it;
                }
            }
            q_.push_back(std::move(c));
        }
        cv_.notify_one();
    }

    void run() {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ISpVoice* voice = nullptr;
        int rate = 0, volume = 100;
        bool tried = false;
        const auto ensure = [&]() {
            if (tried) return voice != nullptr;
            tried = true;
            if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice))) && voice) {
                voice->SetRate(rate);
                voice->SetVolume(static_cast<USHORT>(volume));
            } else {
                voice = nullptr;
            }
            {
                std::lock_guard<std::mutex> lk(availMutex_);
                avail_.store(voice ? 1 : 0);
            }
            availCv_.notify_all();
            return voice != nullptr;
        };
        for (;;) {
            Cmd c;
            {
                std::unique_lock<std::mutex> lk(m_);
                if (speaking_.load()) cv_.wait_for(lk, std::chrono::milliseconds(80), [this] { return !q_.empty(); });
                else cv_.wait(lk, [this] { return !q_.empty(); });
                if (q_.empty()) {   // poll timeout while speaking
                    lk.unlock();
                    SPVOICESTATUS st{};
                    speaking_.store(voice && SUCCEEDED(voice->GetStatus(&st, nullptr)) && (st.dwRunningState & SPRS_IS_SPEAKING) != 0);
                    continue;
                }
                c = std::move(q_.front());
                q_.pop_front();
            }
            if (c.type == Cmd::Quit) break;
            switch (c.type) {
                case Cmd::Init: ensure(); break;
                case Cmd::Speak: {
                    // pending_ drops only after speaking_ is set, so speaking() has no false gap while Speak() runs.
                    if (ensure()) {
                        const DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML | (c.interrupt ? SPF_PURGEBEFORESPEAK : 0);
                        if (SUCCEEDED(voice->Speak(c.text.c_str(), flags, nullptr))) speaking_.store(true);
                    }
                    pending_.fetch_sub(1);
                    break;
                }
                case Cmd::Stop:
                    if (voice) voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                    speaking_.store(false);
                    break;
                case Cmd::Rate: rate = c.ival; if (voice) voice->SetRate(rate); break;
                case Cmd::Volume: volume = c.ival; if (voice) voice->SetVolume(static_cast<USHORT>(volume)); break;
                default: break;
            }
        }
        if (voice) { voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr); voice->Release(); }
        if (com) CoUninitialize();
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Cmd> q_;
    std::atomic<int> pending_{0};
    std::atomic<bool> speaking_{false};
    mutable std::atomic<int> avail_{-1};
    mutable std::mutex availMutex_;
    mutable std::condition_variable availCv_;
    std::thread th_;   // last: starts after the members above exist
};

} // namespace

std::unique_ptr<SpeechSynth> createSpeechSynth() { return std::make_unique<SapiSpeech>(); }

bool osScreenReaderActive() {
    BOOL on = FALSE;
    return SystemParametersInfoW(SPI_GETSCREENREADER, 0, &on, 0) && on;
}

} // namespace aver::platform
