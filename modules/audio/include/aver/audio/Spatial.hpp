#pragma once
// The 3D player model as pure functions: the four attenuation models with unit_size and max_db, the max_distance fade,
// the emission cone, the high-shelf "attenuation filter", the stereo panning law and the Doppler pitch. The mixer applies them per
// block to voices flagged with the spatial model; tests call them directly. Distances here are METRES (the mixer converts from centimetres).
#include "aver/audio/Effects.hpp"

namespace aver::audio {

enum class AttenuationModel : i32 { InverseDistance = 0, InverseSquareDistance = 1, Logarithmic = 2, Disabled = 3 };

struct Spatial3D {
    i32 model = 0;                     // AttenuationModel
    f32 unitSize = 10.0f;              // metres
    f32 maxDb = 3.0f;
    f32 maxDistance = 0.0f;            // metres; 0 = no limit
    f32 panningStrength = 1.0f;
    f32 globalPanningStrength = 0.5f;  // the project-wide 3D panning strength
    bool coneEnabled = false;
    f32 coneAngleDeg = 45.0f;
    f32 coneFilterDb = -12.0f;         // added to the filter attenuation outside the cone
    f32 filterCutoffHz = 5000.0f;      // >= 20500 turns the filter off
    f32 filterDb = -24.0f;
    f32 emitDir[3] = {1.0f, 0.0f, 0.0f};   // the direction the sound is projected along (engine space, unit)
    bool doppler = false;
    f32 velocity[3] = {0.0f, 0.0f, 0.0f};  // the emitter's velocity, engine space, centimetres per second

    static constexpr u32 kFloats = 18;
    void pack(f32 out[kFloats]) const;
    static Spatial3D unpack(const f32 in[kFloats]);
};

struct SpatialResult {
    f32 gainL = 0.0f, gainR = 0.0f;    // linear, including the volume
    f32 distanceGain = 0.0f;           // the multiplier before panning (what the filter reads)
    f32 filterGain = 1.0f;             // linear gain of the high shelf at filterCutoffHz; 1 = no filter
    f32 coneAngleDeg = 0.0f;
    bool silent = false;               // beyond max_distance
};

// Attenuation in dB for a distance (metres) and unit size, before the volume and the max_db clamp.
f32 attenuationDb(AttenuationModel model, f32 distanceM, f32 unitSize);
// The stereo pan law: the left and right gains for cos(x) = lateral / horizontal radius in the listener's frame.
void spatialPanGains(f32 cosx, f32 panningStrength, f32 globalStrength, f32& left, f32& right);
// The gains for a source at srcCm seen by a listener (all vectors engine space, positions in centimetres). volume is linear.
SpatialResult spatialize(const Spatial3D& s, f32 volume, const f32 listenerCm[3], const f32 listenerForward[3], const f32 listenerRight[3],
                         const f32 srcCm[3]);
// Pitch multiplier from the relative motion (velocities in cm/s), speed of sound 343 m/s, clamped to [1/8, 8].
f32 dopplerPitch(const f32 listenerCm[3], const f32 listenerVelCm[3], const f32 srcCm[3], const f32 srcVelCm[3]);

} // namespace aver::audio
