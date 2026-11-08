// PhysAnimProfile: the ".ocphysanim" asset -- how a skeleton's regions take part in physical animation.
// Pure data and text I/O (no world, no physics), so it round-trips and validates headlessly.
//
// A REGION names bones (exactly, or a bone and everything below it) and carries the drive the region gets:
// motor strength, damping, torque ceiling, how much of the simulated pose shows at full reaction, and the
// envelope a hit gives it. Nothing is keyed on a bone name in the engine; names appear only in the file.
//
// Text format, one directive per line, '#' starts a comment, names have no spaces:
//   physanim <name>
//   reach falloff <per hop> hops <n> epsilon <e> impulse <kg*cm/s for a full reaction>
//   life death <seconds> getupblend <seconds> supine <montageId> prone <montageId> side <montageId>
//   default <region keys...>                       -- the drive of a bone in no region
//   region <name> [bone <n>]... [subtree <n>]... [strength f] [damping f] [torque f] [blend f]
//                 [boost f] [impulse f] [attack s] [hold s] [decay s]
// A later region wins over an earlier one for a bone both name. Montage ids are the .ocmontage ObjectIds.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/OcAnim.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::ragdoll {

// The drive of one region (and, resolved, of one bone).
struct PhysAnimDrive {
    f32 strength = 0.5f;     // powered motor strength 0..1
    f32 damping = 1.0f;      // motor damping ratio
    f32 torque = 1.0f;       // multiplier on the motor's torque ceiling
    f32 blend = 1.0f;        // weight of the simulated pose at a full reaction, 0..1
    f32 boost = 0.4f;        // strength added at a full reaction
    f32 impulse = 1.0f;      // multiplier on a hit's impulse for a hit landing in this region
    f32 attack = 0.04f;      // seconds to reach the peak
    f32 hold = 0.12f;        // seconds held at the peak
    f32 decay = 0.45f;       // seconds back to zero
};

struct PhysAnimRegion {
    std::string name;
    std::vector<std::string> bones;      // exact bone names
    std::vector<std::string> subtrees;   // a bone and all its descendants
    PhysAnimDrive drive;
};

struct PhysAnimProfile {
    std::string name;
    f32 falloffPerHop = 0.55f;           // a neighbour hop along the skeleton keeps this fraction of the reaction
    i32 maxHops = 4;                     // the reaction does not spread further than this
    f32 epsilon = 0.01f;                 // an envelope below this is over
    f32 fullImpulse = 3000.0f;           // |impulse| (kg*cm/s) that gives a full reaction
    f32 deathSeconds = 0.6f;             // powered -> limp ramp
    f32 getUpBlend = 0.5f;               // seconds to blend back to the animation when getting up
    u64 getUpSupine = 0, getUpProne = 0, getUpSide = 0;   // montage ids; 0 = none
    PhysAnimDrive defaults;              // a bone in no region
    std::vector<PhysAnimRegion> regions;
};

// False with `why` on a malformed line; `out` then holds what parsed before it.
bool parsePhysAnimProfile(std::string_view text, PhysAnimProfile& out, std::string* why = nullptr);
bool loadPhysAnimProfile(const std::string& path, PhysAnimProfile& out, std::string* why = nullptr);
// Canonical text; parse(serialize(p)) == p to the printed precision.
std::string serializePhysAnimProfile(const PhysAnimProfile& p);
bool savePhysAnimProfile(const std::string& path, const PhysAnimProfile& p, std::string* why = nullptr);

// Checks the profile against a skeleton. Appends one line per problem (an unknown bone, an empty region, a
// value out of range, a duplicate region name) and returns true when there were none.
bool validatePhysAnimProfile(const PhysAnimProfile& p, const fmt::OcSkeleton& skel, std::vector<std::string>* problems);

// The profile mapped onto a skeleton: a drive per bone and the skeleton's parent links.
struct ResolvedPhysAnim {
    u32 boneCount = 0;
    std::vector<i32> parents;
    std::vector<i32> region;             // per bone: index into the profile's regions, -1 = defaults
    std::vector<PhysAnimDrive> drive;    // per bone
    f32 falloffPerHop = 0.55f, epsilon = 0.01f, fullImpulse = 3000.0f;
    i32 maxHops = 4;
    f32 deathSeconds = 0.6f, getUpBlend = 0.5f;
    u64 getUpSupine = 0, getUpProne = 0, getUpSide = 0;

    // Bones within `hops` links of `bone` along the skeleton tree, with their hop count (the bone itself at 0).
    void withinHops(i32 bone, i32 hops, std::vector<std::pair<i32, i32>>& out) const;
    // The bone and every descendant.
    void subtreeOf(i32 bone, std::vector<i32>& out) const;
};

// Unknown bones are skipped (validate reports them).
ResolvedPhysAnim resolvePhysAnim(const PhysAnimProfile& p, const fmt::OcSkeleton& skel);

} // namespace aver::ragdoll
