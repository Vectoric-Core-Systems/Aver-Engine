#include "aver/platform/Speech.hpp"

namespace aver::platform {
namespace {

// No synthesiser on this platform yet: every call is a no-op. macOS (AVSpeechSynthesizer, needs a .mm file) and
// Linux (speech-dispatcher) slot in behind the same interface.
class NullSpeech final : public SpeechSynth {
public:
    bool available() const override { return false; }
    void speak(std::string_view, bool) override {}
    void stop() override {}
    bool speaking() const override { return false; }
    void setRate(int) override {}
    void setVolume(float) override {}
};

} // namespace

std::unique_ptr<SpeechSynth> createSpeechSynth() { return std::make_unique<NullSpeech>(); }

bool osScreenReaderActive() { return false; }

} // namespace aver::platform
