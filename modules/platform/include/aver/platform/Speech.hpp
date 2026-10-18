#pragma once
// OS text to speech: the game talking (narrated menus, captions, script announcements). Not a screen-reader
// bridge: nothing here exposes widgets to NVDA or Narrator. Windows uses SAPI on a worker thread; the POSIX
// build is a silent stub behind the same interface (macOS AVSpeechSynthesizer and speech-dispatcher are
// follow-ups). docs/ACCESSIBILITY.md section 2.7.
#include <memory>
#include <string_view>

namespace aver::platform {

class SpeechSynth {
public:
    virtual ~SpeechSynth() = default;
    virtual bool available() const = 0;                              // a usable voice exists
    virtual void speak(std::string_view utf8, bool interrupt) = 0;   // non-blocking enqueue; empty text is a no-op
    virtual void stop() = 0;                                         // drops queued and current speech
    virtual bool speaking() const = 0;
    virtual void setRate(int sapiRate) = 0;                          // -10..10, the SAPI range
    virtual void setVolume(float v01) = 0;
};

// Never null. The stub reports available() == false and does nothing.
std::unique_ptr<SpeechSynth> createSpeechSynth();

// Windows: SystemParametersInfo(SPI_GETSCREENREADER). Everywhere else false.
bool osScreenReaderActive();

} // namespace aver::platform
