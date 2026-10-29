#pragma once
// Surround output as pure functions: the speaker layouts the mixer can render, the pairwise constant-power pan across them, the upmix of a
// stereo source, and which layout a device gets (and which device channel each mixer channel feeds). The mixer calls these per block; tests
// call them directly.
//
// The mixer's channel order is the standard one that WAVEFORMATEXTENSIBLE (ascending speaker-mask bits) and SDL use for these counts:
//    6 (5.1):  L  R  C  LFE  Ls  Rs                 (surrounds at +-110 degrees)
//    8 (7.1):  L  R  C  LFE  Lb  Rb  Ls  Rs         (backs at +-150, sides at +-90)
// Azimuth is measured from straight ahead, positive to the listener's right, so +Y in the engine's frame is +90.
#include "aver/core/Types.hpp"

namespace aver::audio {

inline constexpr u32 kMaxSpeakers = 8;
inline constexpr u32 kSpeakerLfe = 3;     // the LFE index in both surround layouts

// The channel counts the mixer renders: mono, stereo, 5.1, 7.1.
inline bool validMixChannels(u32 n) { return n == 1 || n == 2 || n == 6 || n == 8; }
inline bool isSurround(u32 n) { return n == 6 || n == 8; }

// Where channel `index` of an N-channel layout sits, in degrees (0 ahead, positive right). The LFE has no direction and reads 0.
f32 speakerAzimuthDeg(u32 channels, u32 index);

// Pairwise constant-power pan. `azimuthRad` is the source direction in the listener's frame; `spread` 0 is a point source, 1 is spread
// equally over every directional speaker (a source overhead or inside the head). The gains satisfy sum(g*g) == 1 for every angle and
// every spread, the LFE gets 0, and a source exactly at a speaker's angle feeds only that speaker. Writes `channels` values.
void speakerGains(u32 channels, f32 azimuthRad, f32 spread, f32 out[kMaxSpeakers]);

// How a stereo signal is placed on the speakers. Front: left and right only. Wrap: the same plus a power-preserving share to the
// surround channels on the same side (centre and LFE stay silent, so a centred image is still a phantom one).
enum class StereoUpmix : u32 { Front = 0, Wrap = 1 };

// Output channel c = l[c] * inLeft + r[c] * inRight. Each input channel's squared coefficients sum to 1 in both modes.
struct UpmixMatrix {
    f32 l[kMaxSpeakers] = {};
    f32 r[kMaxSpeakers] = {};
};
UpmixMatrix stereoUpmix(u32 channels, StereoUpmix mode);

// ---- the device ----

// WAVEFORMATEXTENSIBLE dwChannelMask bits (also what the SDL path reports): spelled out so no platform header is needed.
namespace chmask {
inline constexpr u32 FrontLeft = 0x1, FrontRight = 0x2, FrontCentre = 0x4, Lfe = 0x8, BackLeft = 0x10, BackRight = 0x20;
inline constexpr u32 SideLeft = 0x200, SideRight = 0x400;
}

// The layout a device of `deviceChannels` channels and speaker mask `mask` (0 = the backend did not say) is mixed in when the project
// requests `requested` channels (2, 6 or 8). The mixer never gets more than the project asked for or the device can play: 8 falls back
// to 6, then 2; a device whose mask is not a standard 5.1 or 7.1 stays stereo; a one-channel device is mono.
u32 chooseMixChannels(u32 deviceChannels, u32 mask, u32 requested);

// Which mixer channel feeds device channel d, or -1 for silence, for d < min(deviceChannels, cap). Device channels follow the ascending
// bits of `mask`. A stereo or mono mix goes to the first channels (as it always did); a 5.1 mix on a 7.1 device puts its surrounds on the
// side speakers. Without a usable mask the first mixChannels device channels carry the mix in order.
void buildDeviceMap(u32 mixChannels, u32 deviceChannels, u32 mask, i8* out, u32 cap);

} // namespace aver::audio
