#pragma once
// Speech front end: gates by source (ui, captions, script) and the master switch, resolves localization
// references, drops repeats, interrupts on focus changes, and holds at most 3 queued utterances. The synthesiser
// is injected (platform/Speech.hpp) so the queue is testable with a fake. Header-only, game thread only.
// docs/ACCESSIBILITY.md section 2.7.

#include "aver/core/Types.hpp"
#include "aver/platform/Speech.hpp"

#include <deque>
#include <functional>
#include <string>
#include <string_view>

namespace aver::fw::access {

enum class SpeechSource : i32 { Ui = 0, Captions = 1, Script = 2 };

struct SpeechSettings {
    bool enabled = false, ui = true, captions = false;
    i32 rate = 0;
    f32 volume = 1.0f;
};

class SpeechQueue {
public:
    // Returns true and writes `out` when `raw` is a text reference; false means it is literal text.
    using Resolver = std::function<bool(const std::string& raw, std::string& out)>;

    static constexpr usize kMaxPending = 3;
    static constexpr f32 kRepeatWindow = 1.0f;

    explicit SpeechQueue(platform::SpeechSynth& synth) : synth_(synth) {}

    void setResolver(Resolver r) { resolve_ = std::move(r); }

    // Rate and volume go to the synthesiser once per settings revision (and on the first call).
    void setSettings(const SpeechSettings& s, u32 revision) {
        const bool first = !haveRev_;
        s_ = s;
        if (first || revision != rev_) {
            synth_.setRate(s.rate);
            synth_.setVolume(s.volume);
            rev_ = revision;
            haveRev_ = true;
        }
    }
    const SpeechSettings& settings() const { return s_; }

    // Whether the OS has a voice. Asked lazily (it can block briefly the first time), cached.
    bool available() {
        if (avail_ < 0) avail_ = synth_.available() ? 1 : 0;
        return avail_ > 0;
    }
    void refreshAvailability() { avail_ = -1; }

    // True when the text was spoken or queued. `interrupt` drops everything pending and cuts the current speech.
    bool announce(std::string_view raw, bool interrupt, SpeechSource src) {
        if (!s_.enabled) return false;
        if (src == SpeechSource::Ui && !s_.ui) return false;
        if (src == SpeechSource::Captions && !s_.captions) return false;
        std::string text = resolveText(raw);
        text = trimmed(text);
        if (text.empty() || isMask(text)) return false;
        if (!available()) return false;
        if (text == last_ && now_ - lastAt_ < kRepeatWindow) return false;
        last_ = text;
        lastAt_ = now_;
        if (interrupt) {
            pending_.clear();
            synth_.speak(text, true);
            return true;
        }
        if (pending_.empty() && !synth_.speaking()) { synth_.speak(text, false); return true; }
        pending_.push_back(std::move(text));
        while (pending_.size() > kMaxPending) pending_.pop_front();
        return true;
    }

    // Per frame: advances the repeat clock and feeds the synthesiser the next queued utterance when it is idle.
    void update(f32 dt) {
        if (dt > 0) now_ += dt;
        if (!pending_.empty() && !synth_.speaking()) {
            synth_.speak(pending_.front(), false);
            pending_.pop_front();
        }
    }

    void stop() {
        pending_.clear();
        synth_.stop();
    }
    bool speaking() const { return !pending_.empty() || synth_.speaking(); }
    usize pending() const { return pending_.size(); }

private:
    std::string resolveText(std::string_view raw) const {
        std::string r(raw), out;
        if (resolve_ && resolve_(r, out)) return out;
        return r;
    }
    static std::string trimmed(const std::string& s) {
        usize a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        return s.substr(a, b - a);
    }
    // A masked field's bullets or asterisks are never read out.
    static bool isMask(const std::string& s) {
        for (usize i = 0; i < s.size();) {
            if (s[i] == '*') { ++i; continue; }
            if (i + 2 < s.size() + 0 && static_cast<unsigned char>(s[i]) == 0xE2 && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                static_cast<unsigned char>(s[i + 2]) == 0xA2) { i += 3; continue; }   // U+2022
            return false;
        }
        return true;
    }

    platform::SpeechSynth& synth_;
    Resolver resolve_;
    SpeechSettings s_;
    std::deque<std::string> pending_;
    std::string last_;
    f32 now_ = 0, lastAt_ = -10.0f;
    u32 rev_ = 0;
    bool haveRev_ = false;
    int avail_ = -1;
};

} // namespace aver::fw::access
