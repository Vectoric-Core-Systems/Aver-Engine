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

// ---- diffraction (docs/AUDIO.md section 15; off unless a caller asks) ----
//
// When the direct path is blocked the sound can still arrive bent around a corner or through an opening. The probe looks for the SHORTEST
// one-bend path listener -> P -> source whose two legs the ray function reports clear, searching waypoints P on rings of growing offset
// around the line of sight (a few directions at a few positions along it), then tightening the offset to the edge by bisection. The bent
// path becomes an occlusion value for the voice's existing gain + low-pass (Dsp.hpp), so it costs nothing extra on the audio thread and
// needs no delay: an occlusion of  maxOcclusion * (1 - pathGain * bendGain)  with
//   pathGain = direct length / bent length        (the inverse-distance ratio of the longer route)
//   bendGain = (1 + cos(deflection)) / 2          (1 straight on, 0 doubling back)
// so it rises with the detour and with the sharpness of the bend, and stays below maxOcclusion: a bent path is never as dull as a wall.
struct DiffractionProbe {
    u32 directions = 8;            // waypoint directions around the line of sight (2 .. 16)
    u32 maxRays = 256;             // ray calls one probe may spend (each leg test is one); a fully blocked search over 6 rings spends ~200
    f32 firstOffsetCm = 100.0f;    // smallest ring; each ring doubles
    f32 maxOffsetCm = 3200.0f;     // largest ring
    f32 maxOcclusion = 0.85f;      // the most a bent path can read
    f32 trigger = 0.02f;           // direct occlusion below this skips the search (the path is clear)
};

struct DiffractionPath {
    bool found = false;
    f32 occlusion = 1.0f;          // the bent path's occlusion; 1 when none was found
    f32 pathLengthCm = 0.0f;       // listener -> waypoint -> source
    f32 directLengthCm = 0.0f;
    f32 detourCm = 0.0f;           // pathLengthCm - directLengthCm
    f32 bendRadians = 0.0f;        // deflection at the waypoint, 0 straight on
    f32 pathGain = 1.0f, bendGain = 1.0f;
    f32 waypoint[3] = {0.0f, 0.0f, 0.0f};
    u32 raysUsed = 0;
};

// The occlusion of a path of the given geometry (the formula above), exposed for tools and tests.
f32 diffractionOcclusion(f32 directLengthCm, f32 pathLengthCm, f32 bendRadians, f32 maxOcclusion);
// Searches for the bent path. A leg counts as blocked when the ray function returns >= 0.5. found = false when no clear path turned up
// within the ray budget (or there is no ray function).
DiffractionPath findDiffractionPath(RayBlockFn fn, void* user, const f32 listener[3], const f32 source[3], const DiffractionProbe& probe);
// What the voice should be told: the direct occlusion, lowered to the bent path's when one was found. Continuous in `directOcclusion`.
f32 applyDiffraction(f32 directOcclusion, const DiffractionPath& path);
// measureOcclusion, then, when the direct occlusion is above probe.trigger, the bent-path search and applyDiffraction. `path` (optional)
// receives the search result, or a default (not found) when the direct line was clear and no search ran.
f32 measureOcclusionDiffracted(RayBlockFn fn, void* user, const f32 listener[3], const f32 source[3], const OcclusionProbe& direct,
                               const DiffractionProbe& probe, DiffractionPath* path = nullptr);

} // namespace aver::audio
