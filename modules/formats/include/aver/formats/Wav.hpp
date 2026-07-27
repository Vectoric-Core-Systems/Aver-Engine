#pragma once
// RIFF/WAVE, read into the one representation the mixer wants: interleaved f32.
//
// Written rather than vendored, like the JSON reader and for the same reason: a WAV file is a
// four-character chunk id, a length, and a blob, and a dependency for that is a dependency to
// audit, ship and keep current for no work it saves.
//
// It reads what tools actually WRITE -- 8/16/24/32-bit integer PCM and 32-bit float -- and refuses
// everything else by name rather than guessing. A compressed WAV decoded as PCM is not quiet or
// distorted; it is full-scale noise.
#include "aver/audio/Sound.hpp"

#include <string>
#include <vector>

namespace aver::formats {

struct WavResult {
    bool ok = false;
    std::string error;      // empty when ok
};

// Decode a whole .wav into `out`. Sample rate and channel count come from the file and are NOT
// converted here: resampling belongs to whatever knows the device rate, and doing it at load time
// against a guess is how a library ends up resampling twice.
WavResult wavRead(const std::vector<u8>& bytes, audio::SoundData& out);
WavResult wavReadFile(const std::string& path, audio::SoundData& out);

} // namespace aver::formats
