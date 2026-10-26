// The tagged extras a cooked placement carries in a chunk payload (codec v2; LARGE_WORLD 2.4.3).
//
// ChunkCodec stores `PayloadEntity::extras` as opaque tag + bytes. THIS is the one place that knows what the tags mean, so the cook
// (writes) and the authored host (reads) cannot disagree about a byte. Not scene-gated: Core + Formats only.
//
//   PLID  u64 placementId        stable identity (objectId is the MESH id, not the instance)
//   ANIM  str clip, f32 speed, f32 time, u8 once
//   COLL  u8 collide             the authored flag; the host's ItemLoader picks the shape from the asset, as the flat path does
//   CMPN  str type, u16 n, n x (str key, str value)   one TAG PER COMPONENT, in file order (the host's apply order)
//   LABL  str label              the outliner name
//   SNAP  u8 1                   snapToGround: the stored Z is the offset above ground
//   EULR  f32 roll, pitch, yaw   the authored rotation in degrees, the exact f32 casts instantiate() makes. The payload's
//                                quaternion is quatFromEulerDeg of these; a host that feeds instantiate() (which takes Euler)
//                                rebuilds the placement from this tag and gets the same quaternion back bit for bit. Absent = zero
//
// Unknown tags are never touched by set/get, so a later tag added by a newer cook survives an older tool's round trip.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/world/ChunkPayload.hpp"

#include <string>
#include <vector>

namespace aver::world {

// A four-character code as the u32 the codec stores (first character in the low byte).
constexpr u32 extraTag(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

inline constexpr u32 kExtraPlacementId = extraTag('P', 'L', 'I', 'D');
inline constexpr u32 kExtraAnim        = extraTag('A', 'N', 'I', 'M');
inline constexpr u32 kExtraCollide     = extraTag('C', 'O', 'L', 'L');
inline constexpr u32 kExtraComponent   = extraTag('C', 'M', 'P', 'N');
inline constexpr u32 kExtraLabel       = extraTag('L', 'A', 'B', 'L');
inline constexpr u32 kExtraSnap        = extraTag('S', 'N', 'A', 'P');
inline constexpr u32 kExtraEuler       = extraTag('E', 'U', 'L', 'R');

// Everything a placement adds to a payload entity. A `has*` flag false (or an empty list) means "that tag is not written".
struct PlacementExtras {
    bool hasPlacementId = false;
    u64  placementId = 0;

    bool        hasAnim = false;
    std::string animClip;
    f32         animSpeed = 1.0f;
    f32         animTime = 0.0f;
    bool        animOnce = false;

    bool hasCollide = false;
    bool collide = true;

    std::vector<fmt::OcPlacedComponent> components;

    bool        hasLabel = false;
    std::string label;

    bool snapToGround = false;

    bool hasEuler = false;
    f32  euler[3] = {0, 0, 0};   // roll, pitch, yaw (degrees)
};

// The first extra with `tag`, or nullptr.
const PayloadExtra* findExtra(const PayloadEntity& e, u32 tag);

// Replaces the seven known tags on `e` with `x`; every other extra stays where it was. Known tags are appended in the fixed order
// PLID, ANIM, COLL, CMPN..., LABL, SNAP, EULR, so the encoded bytes are a function of `x` alone. False (and `e` untouched) when a
// value cannot fit the codec's u16 fields; `why` says which.
bool setPlacementExtras(PayloadEntity& e, const PlacementExtras& x, std::string* why = nullptr);

// Reads the seven known tags. A known tag that is the wrong length, or truncated, fails with `why` naming it. A missing tag leaves
// the matching field at its default with its `has*` false.
bool getPlacementExtras(const PayloadEntity& e, PlacementExtras& out, std::string* why = nullptr);

} // namespace aver::world
