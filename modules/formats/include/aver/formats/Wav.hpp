#pragma once
// RIFF/WAVE, read into the one representation the mixer wants: interleaved f32. Reads 8/16/24/32-bit
// integer PCM and 32-bit float, and refuses everything else by name.
#include "aver/audio/Sound.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// The outcome of a decode.
struct WavResult {
    bool ok = false;
    std::string error;      // empty when ok
};

// Decodes a whole .wav into `out`. Sample rate and channel count come from the file and are not
// converted here.
WavResult wavRead(const std::vector<u8>& bytes, audio::SoundData& out);
// The same, reading the file from disk first.
WavResult wavReadFile(const std::string& path, audio::SoundData& out);

} // namespace aver::fmt
