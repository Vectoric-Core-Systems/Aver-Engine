// SpeechQueue.hpp over a fake synthesiser: interrupt vs queue, the 3-deep drop-oldest queue, duplicate suppression,
// per-source gating, reference resolution, rate/volume pushed once per revision, silence without a voice, and a masked
// marker never spoken. Header-only logic, no OS speech. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/SpeechQueue.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::fw::access;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

struct Fake final : platform::SpeechSynth {
    struct Call { std::string text; bool interrupt; };
    std::vector<Call> spoken;
    bool hasVoice = true, busy = false;
    int rateCalls = 0, volumeCalls = 0, stops = 0, availableCalls = 0;
    int lastRate = 0;
    float lastVolume = -1;

    bool available() const override { const_cast<Fake*>(this)->availableCalls++; return hasVoice; }
    void speak(std::string_view t, bool i) override { spoken.push_back({std::string(t), i}); busy = true; }
    void stop() override { ++stops; busy = false; }
    bool speaking() const override { return busy; }
    void setRate(int r) override { ++rateCalls; lastRate = r; }
    void setVolume(float v) override { ++volumeCalls; lastVolume = v; }
};

static SpeechSettings on() {
    SpeechSettings s;
    s.enabled = true;
    s.ui = true;
    s.captions = true;
    return s;
}

static void testGating() {
    AVER_INFO("=== gating ===");
    Fake f;
    SpeechQueue q(f);
    SpeechSettings s;   // disabled
    q.setSettings(s, 1);
    check(!q.announce("hello", false, SpeechSource::Script) && f.spoken.empty(), "the master switch gates everything");
    s = on();
    s.ui = false;
    q.setSettings(s, 2);
    check(!q.announce("menu", false, SpeechSource::Ui) && q.announce("script", false, SpeechSource::Script), "ui narration can be off on its own");
    s.ui = true;
    s.captions = false;
    q.setSettings(s, 3);
    check(!q.announce("cap", false, SpeechSource::Captions) && q.announce("menu", false, SpeechSource::Ui), "and so can caption narration");
    check(!q.announce("   ", false, SpeechSource::Script) && !q.announce("", false, SpeechSource::Script), "blank text is not spoken");
}

static void testQueue() {
    AVER_INFO("=== interrupt and queue ===");
    Fake f;
    SpeechQueue q(f);
    q.setSettings(on(), 1);
    q.announce("one", false, SpeechSource::Script);
    check(f.spoken.size() == 1 && !f.spoken[0].interrupt, "an idle synthesiser speaks at once");
    q.announce("two", false, SpeechSource::Script);
    q.announce("three", false, SpeechSource::Script);
    q.announce("four", false, SpeechSource::Script);
    q.announce("five", false, SpeechSource::Script);
    check(f.spoken.size() == 1 && q.pending() == 3, "while it talks, announcements queue (3 deep)");
    q.announce("six", false, SpeechSource::Script);
    check(q.pending() == 3, "the oldest is dropped past 3");
    f.busy = false;
    q.update(0.1f);
    check(f.spoken.size() == 2 && f.spoken[1].text == "four", "when it goes idle the next queued one is spoken (two and three were dropped)");
    q.announce("urgent", true, SpeechSource::Ui);
    check(f.spoken.back().text == "urgent" && f.spoken.back().interrupt && q.pending() == 0, "interrupt cuts the current speech and clears the queue");
    q.stop();
    check(f.stops == 1 && q.pending() == 0, "stop clears and stops");
}

static void testRepeats() {
    AVER_INFO("=== duplicate suppression ===");
    Fake f;
    SpeechQueue q(f);
    q.setSettings(on(), 1);
    check(q.announce("Play", true, SpeechSource::Ui), "first time");
    check(!q.announce("Play", true, SpeechSource::Ui), "the same text within a second is dropped");
    q.update(0.5f);
    check(!q.announce("Play", true, SpeechSource::Ui), "still dropped at half a second");
    q.update(0.6f);
    check(q.announce("Play", true, SpeechSource::Ui), "and spoken again after a second");
}

static void testResolve() {
    AVER_INFO("=== references and masks ===");
    Fake f;
    SpeechQueue q(f);
    q.setSettings(on(), 1);
    q.setResolver([](const std::string& raw, std::string& out) {
        if (raw.size() > 1 && raw[0] == '@') { out = "resolved:" + raw.substr(1); return true; }
        return false;
    });
    q.announce("@UI:play", true, SpeechSource::Ui);
    check(f.spoken.back().text == "resolved:UI:play", "a reference is resolved before it is spoken");
    q.update(2.0f);
    q.announce("plain text", true, SpeechSource::Ui);
    check(f.spoken.back().text == "plain text", "literal text goes through untouched");
    const usize before = f.spoken.size();
    q.update(2.0f);
    check(!q.announce("*****", true, SpeechSource::Ui) && !q.announce("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2", true, SpeechSource::Ui) && f.spoken.size() == before,
          "a masked field's bullets are never spoken");
    check(q.announce("2 * 3", true, SpeechSource::Ui), "but text that merely contains an asterisk is");
}

static void testSettings() {
    AVER_INFO("=== rate, volume, availability ===");
    Fake f;
    SpeechQueue q(f);
    SpeechSettings s = on();
    s.rate = 3;
    s.volume = 0.5f;
    q.setSettings(s, 7);
    check(f.rateCalls == 1 && f.volumeCalls == 1 && f.lastRate == 3 && f.lastVolume == 0.5f, "rate and volume are pushed on the first settings");
    q.setSettings(s, 7);
    check(f.rateCalls == 1 && f.volumeCalls == 1, "not again for the same revision");
    s.rate = -2;
    q.setSettings(s, 8);
    check(f.rateCalls == 2 && f.lastRate == -2, "but once per new revision");

    Fake none;
    none.hasVoice = false;
    SpeechQueue n(none);
    n.setSettings(on(), 1);
    check(!n.announce("hello", true, SpeechSource::Script) && none.spoken.empty(), "no voice: silent");
    n.announce("hello again", true, SpeechSource::Script);
    check(none.availableCalls == 1, "and availability is asked once, then cached");
}

int main() {
    AVER_INFO("SpeechQueueTest");
    testGating();
    testQueue();
    testRepeats();
    testResolve();
    testSettings();
    AVER_INFO(g_failures == 0 ? "SpeechQueueTest: PASS" : "SpeechQueueTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
