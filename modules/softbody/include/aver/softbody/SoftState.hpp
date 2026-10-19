// Saved damage: the delta of a soft body against the pristine cage the loader rebuilds, as the text payload of the
// `softbody` channel of Aver.Save's `$State` component. This header is the pure codec; the system captures and
// replays (Aver.SoftBody.Scene, SoftBodySave.cpp). See docs/SOFTBODY.md "Saved damage".
//
//   "SB1 <meshKey hex16> <assetKey hex16> <base64 body>"
//
// The body (little endian): flags u8 (bit0 released, bit1 quantised), particleCount u32, the ordered tear log (groups
// of beam ids that broke between two tear-pipeline runs), the beams whose rest, plastic set or damage differ from the
// build, the particles whose piece was already gone, and per particle a position and a velocity. A cage over 512 KB of
// raw state stores them as 16-bit fixed point relative to the entry's box.
#pragma once

#include "aver/core/Math.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::softbody {

inline constexpr u32 kSoftStateMaxText = 1u << 20;               // the $State channel's payload cap
inline constexpr usize kSoftStateRawLimit = 512u * 1024u;        // above this the particle block is quantised

struct SoftStateBeam {
    u32 beam = 0;
    f32 rest = 0.0f, plastic = 0.0f, damage = 0.0f;
};

struct SoftState {
    u64 meshKey = 0, assetKey = 0;
    bool released = false;
    u32 particleCount = 0;                       // live particles including tear duplicates
    std::vector<std::vector<u32>> tearGroups;    // ascending beam ids per group, in the order they broke
    std::vector<SoftStateBeam> beams;            // ascending beam id
    std::vector<u32> gone;                       // ascending particle ids whose piece was already destroyed
    std::vector<f32> pos, vel;                   // 3 per particle: world cm, cm/s (zero for gone ones)
};

enum class SoftStateError : u8 { None = 0, TooLarge, BadHeader, BadBase64, Truncated, Corrupt };
const char* softStateErrorText(SoftStateError e);

// `forceQuantised` takes the 16-bit path regardless of size (tests pin its error). Sorts the id lists it writes.
std::string encodeSoftState(const SoftState& s, bool forceQuantised = false);
// Validates every length against the bytes that remain; never reads past the text. `out` is unspecified on failure.
bool decodeSoftState(std::string_view text, SoftState& out, SoftStateError* err = nullptr);
// True when the saved keys are not the current ones: the mesh or the asset changed since the save.
inline bool softStateStale(const SoftState& s, u64 meshKey, u64 assetKey) { return s.meshKey != meshKey || s.assetKey != assetKey; }

// The beam-id groups merged down to at most `maxGroups` consecutive groups (the same final breaks, fewer replay runs).
std::vector<std::vector<u32>> mergeTearGroups(const std::vector<std::vector<u32>>& groups, u32 maxGroups);

} // namespace aver::softbody
