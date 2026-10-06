#pragma once
// Music transitions and occlusion measurement: the game-thread logic that drives the mixer's
// per-voice fade and occlusion inputs.
#include "aver/audio/Mixer.hpp"

namespace aver::audio {

// Plays one music track at a time. Starting another crossfades: the old voice fades out and ends,
// the new one fades in, both ramps running sample-accurately on the audio thread.
class MusicCrossfader {
public:
    MusicCrossfader() = default;
    explicit MusicCrossfader(Mixer* mixer) : mixer_(mixer) {}
    void bind(Mixer* mixer) { mixer_ = mixer; current_ = 0; }

    struct Options {
        f32 volume = 1.0f;
        bool looping = true;
        f32 fadeSeconds = 2.0f;                       // crossfade length; 0 cuts
        FadeCurve curve = FadeCurve::EqualPower;
        Bus bus = Bus::Music;
        StreamParams stream{};
    };

    // Starts `source` as the current track. Returns its voice, or 0.
    VoiceHandle play(std::unique_ptr<StreamSource> source, const Options& options);
    // Fades the current track out and ends it. 0 seconds cuts.
    void stop(f32 fadeSeconds = 1.0f, FadeCurve curve = FadeCurve::EqualPower);
    // The current track's voice, or 0 once it has ended.
    VoiceHandle current() const;

private:
    Mixer* mixer_ = nullptr;
    VoiceHandle current_ = 0;
};

// The blocking amount (0 clear .. 1 solid) of the segment from `from` to `to`.
using RayBlockFn = f32 (*)(void* user, const f32 from[3], const f32 to[3]);

struct OcclusionProbe {
    u32 rayCount = 5;             // 1 = a single centre ray; more spread rays around the source
    f32 probeRadiusCm = 50.0f;    // ring radius around the source
};

// Fraction of probe rays from the listener to the source that are blocked, in [0,1], each ray
// weighted by its own blocking amount.
f32 measureOcclusion(RayBlockFn fn, void* user, const f32 listener[3], const f32 source[3],
                     const OcclusionProbe& probe);
// Moves `current` toward `measured`: quickly when it rises, slowly when it clears.
f32 smoothOcclusion(f32 current, f32 measured, f32 dt, f32 riseSec, f32 fallSec);

} // namespace aver::audio
