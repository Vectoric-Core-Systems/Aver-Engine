#pragma once
// Decoded audio in the one representation the mixer reads: interleaved f32, with its own rate.
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::audio {

// 0 == invalid, like every handle in this engine.
using SoundHandle = u32;
using VoiceHandle = u32;

// Where a voice's output is summed before the master.
enum class Bus : u8 { Sfx = 0, Music, Voice, Ui, Count };

// One decoded sound: its samples, its format and its loop points.
struct SoundData {
    std::vector<f32> samples;     // interleaved, channels() apart
    u32 channels   = 1;
    u32 sampleRate = 48000;

    u32 loopBegin = 0;            // in FRAMES
    u32 loopEnd   = 0;            // in FRAMES; 0 means "to the end"

    // Frame count, or 0 when the channel count is zero.
    u32 frames() const {
        return channels ? static_cast<u32>(samples.size() / channels) : 0;
    }
    bool valid() const { return channels > 0 && sampleRate > 0 && !samples.empty(); }
};

} // namespace aver::audio
