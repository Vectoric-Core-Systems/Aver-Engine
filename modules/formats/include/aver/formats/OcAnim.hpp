#pragma once
// .ocskel and .ocanim — the skeleton (FORMAT_SPECS.md §8) and the animation clip (§9), both AVR1
// containers. One header, because every track in a clip is addressed by a bone index from a skeleton.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// ---- .ocskel ----

inline constexpr i32 kOcBoneNoParent = -1;

// One bone: its rest transform and its inverse bind matrix.
struct OcBone {
    std::string name;
    i32  parent = kOcBoneNoParent;   // index into OcSkeleton::bones, or -1 for a root
    Vec3 translation{0, 0, 0};       // local rest pose, engine centimetres
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
    // Row-major, row-vector, engine space.
    f32  inverseBind[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

// A bone hierarchy, parents before children.
struct OcSkeleton {
    std::vector<OcBone> bones;
    u32  rootBone = 0xFFFFFFFFu;     // convenience hint; -1 when there is no single root

    // True when parents are in range, acyclic, and each precedes its children.
    bool valid() const;
};

bool loadOcSkel(const std::string& path, OcSkeleton& out, std::string* why = nullptr);
bool saveOcSkel(const std::string& path, const OcSkeleton& in, std::string* why = nullptr);
bool parseOcSkel(const u8* bytes, usize size, OcSkeleton& out, std::string* why = nullptr);
bool writeOcSkel(const OcSkeleton& in, std::vector<u8>& out, std::string* why = nullptr);

// ---- .ocanim ----

// How keys are stored (§9.1).
enum class OcAnimStorage : u8 { Keyframed = 0, BakedUniform = 1 };

// How values are interpolated between keys (§9.2).
enum class OcInterp : u8 { Linear = 0, Step = 1, CubicSpline = 2 };

inline constexpr u8 kOcChannelTranslation = 1 << 0;
inline constexpr u8 kOcChannelRotation    = 1 << 1;
inline constexpr u8 kOcChannelScale       = 1 << 2;

inline constexpr u8 kOcAnimLoop         = 1 << 0;
inline constexpr u8 kOcAnimAdditiveBase  = 1 << 1;
inline constexpr u8 kOcAnimRootMotion    = 1 << 2;

// One animated channel set of one bone.
struct OcTrack {
    u16      boneIndex = 0;
    u8       channels  = 0;                    // kOcChannel* mask; what `values` holds per key
    OcInterp interp    = OcInterp::Linear;
    std::vector<f32> times;                    // KeyCount entries, seconds from the start, ascending
    // KeyCount * componentsPerKey() floats, key-major.
    std::vector<f32> values;

    // Floats per key: the enabled channels' widths (translation 3, rotation 4, scale 3) times 3 for
    // CubicSpline (in-tangent, value, out-tangent) or times 1 otherwise. 0 if the mask is empty.
    u32 componentsPerKey() const;
    // True when `times` is ascending and `values` has the matching length.
    bool valid() const;
};

// One clip: every animated bone's tracks, plus the clip's own timing.
// ONE NOTIFY: a named event the clip raises when playback crosses `time`.
//
// WHAT IT IS FOR. A footstep sound on the frame the foot lands, a muzzle flash on the frame the
// weapon fires, a hit window opening partway through a swing. All of those are "at THIS moment in
// this animation, tell the game" -- and until now an author had exactly one way to express it: read
// the clip time in a graph every tick and compare. That is a polling loop for an event, it drifts
// with frame rate, and it puts a number from the animation into the gameplay code where nobody
// editing the animation will ever find it.
//
// A NAME, NOT AN ENUM OR AN ID. The name is fired as a graph EVENT, and GraphHost.Fire is already
// name-agnostic -- it branches on nothing, so "OnFootstep" needs no registration anywhere in the
// engine (see the CustomEvent node, which exists for exactly this shape). An enum would have made
// every project share one vocabulary and required an engine change to add a footstep.
//
// NO DURATION, deliberately, and this is the one place this diverges from Unreal. Unreal has both a
// Notify (instant) and a Notify State (begin/tick/end over a range), and the state form needs the
// runtime to track which states are open, close them when a clip is interrupted, and decide what
// happens when it loops mid-state. None of that machinery exists here yet, and a half-built version
// that silently fails to close a state on interruption would be worse than not having it: the
// symptom is a hit window that never shuts. Two instant notifies express the same thing today, and
// say out loud that nothing is tracking the span between them.
struct OcNotify {
    f32 time = 0.0f;      // seconds from the clip start
    std::string name;     // the event name fired; opaque here, exactly as OcAnimation::skeletonRef is
};

struct OcAnimation {
    f32  duration = 0.0f;                      // seconds
    OcAnimStorage storage = OcAnimStorage::Keyframed;
    u8   flags = 0;
    u16  sampleRate = 0;                       // only meaningful for BakedUniform
    std::string skeletonRef;                   // which skeleton the bone indices belong to
    std::vector<OcTrack> tracks;

    // Notifies, in whatever order the file lists them -- NOT required to be sorted, because a
    // reader that needs them in time order can sort a handful of entries far more cheaply than
    // every writer can be trusted to have done so. Empty for every clip written before they
    // existed, which is every clip: the NOTF chunk is optional and its absence is not an error.
    std::vector<OcNotify> notifies;

    // True when the duration, storage and every track are consistent.
    bool valid() const;
};

bool loadOcAnim(const std::string& path, OcAnimation& out, std::string* why = nullptr);
bool saveOcAnim(const std::string& path, const OcAnimation& in, std::string* why = nullptr);
bool parseOcAnim(const u8* bytes, usize size, OcAnimation& out, std::string* why = nullptr);
bool writeOcAnim(const OcAnimation& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
