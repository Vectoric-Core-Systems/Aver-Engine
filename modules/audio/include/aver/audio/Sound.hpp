#pragma once
// Decoded audio, in the one representation the mixer reads.
//
// INTERLEAVED f32, and the sample rate carried rather than assumed. A mixer that assumed its own
// rate would be right until the first device that runs at 44.1 kHz, and the symptom of getting it
// wrong is not silence -- it is everything playing slightly fast, which sounds like a stylistic
// choice until somebody measures it.
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::audio {

// 0 == invalid, like every handle in this engine.
using SoundHandle = u32;
using VoiceHandle = u32;

// Where a voice's output is summed before the master. Four, because it is the same four in every
// game and a fifth has never justified itself: a mixer with arbitrary buses is a mixer whose graph
// has to be authored, and nothing here authors one yet.
enum class Bus : u8 { Sfx = 0, Music, Voice, Ui, Count };

struct SoundData {
    std::vector<f32> samples;     // interleaved, channels() apart
    u32 channels   = 1;
    u32 sampleRate = 48000;

    // Loop points in FRAMES, not samples or bytes. A loop authored in samples is off by a factor of
    // the channel count on the day somebody converts a mono asset to stereo, and the artefact is a
    // click at the seam rather than an error.
    //
    // loopEnd == 0 means "to the end", so a zeroed SoundData loops the whole thing.
    u32 loopBegin = 0;
    u32 loopEnd   = 0;

    u32 frames() const {
        return channels ? static_cast<u32>(samples.size() / channels) : 0;
    }
    bool valid() const { return channels > 0 && sampleRate > 0 && !samples.empty(); }
};

} // namespace aver::audio
