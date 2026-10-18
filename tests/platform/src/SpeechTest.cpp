// Aver.Platform speech (platform/Speech.hpp): the factory never returns null, every call is safe on any platform,
// the POSIX stub is silent, and on Windows a real SAPI voice accepts a (volume 0) utterance. Exits 0, 1, or 77
// when the machine has no voice. Silent on CI: the volume is set to 0 before anything is spoken.
#include "aver/core/Log.hpp"
#include "aver/platform/Speech.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("SpeechTest");
    auto synth = platform::createSpeechSynth();
    check(synth != nullptr, "createSpeechSynth never returns null");
    if (!synth) return 1;

    // Safe on any platform and in any order, voice or not.
    synth->setVolume(0.0f);
    synth->setRate(0);
    synth->speak("", false);
    synth->stop();
    check(!synth->speaking(), "nothing spoken, nothing speaking");
    synth->setRate(99);
    synth->setRate(-99);
    synth->setVolume(5.0f);
    synth->setVolume(0.0f);
    synth->stop();
    (void)platform::osScreenReaderActive();

#if !defined(_WIN32)
    check(!synth->available(), "the POSIX stub has no voice");
    synth->speak("hello", true);
    check(!synth->speaking(), "and never speaks");
    AVER_INFO(g_failures == 0 ? "SpeechTest: PASS" : "SpeechTest: FAIL");
    return g_failures == 0 ? 0 : 1;
#else
    if (g_failures) return 1;
    if (!synth->available()) {
        AVER_INFO("SpeechTest: SKIPPED (no SAPI voice installed)");
        return 77;
    }
    synth->setVolume(0.0f);
    synth->speak("Aver speech test", true);
    check(synth->speaking(), "a queued utterance reports speaking");
    synth->stop();
    for (int i = 0; i < 100 && synth->speaking(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!synth->speaking(), "stop() ends it");
    AVER_INFO(g_failures == 0 ? "SpeechTest: PASS" : "SpeechTest: FAIL");
    return g_failures == 0 ? 0 : 1;
#endif
}
