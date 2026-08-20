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

// ONE SOCKET: a named place on the rig, offset from a bone.
//
// WHAT IT IS FOR. Hanging something on a character -- a weapon in the hand, a scabbard on the hip,
// a muzzle to spawn a flash at. Without one, every attachment in a project is a bone index plus a
// hand-tuned offset written down somewhere in gameplay code, which means: it is invisible to the
// person posing the rig, it is duplicated in every script that attaches anything, and it silently
// becomes wrong the day the rig is re-exported with a different bone order.
//
// A SOCKET LIVES ON THE SKELETON, NOT THE MESH, and that is the whole reason it is worth having.
// Every clip, every mesh and every character that shares this rig gets the same "Hand_R" without
// being told about it, because the thing they all share is the skeleton.
//
// STORED AS A LOCAL OFFSET FROM ITS BONE -- not a world transform, and not a second bone. A world
// transform would be meaningless the moment the character moved; a bone would be posed by clips
// that know nothing about it. An offset composes with whatever the bone is doing this frame, which
// is exactly what "in the hand" means.
struct OcSocket {
    std::string name;                // what a script asks for; opaque here
    u32  bone = 0;                   // index into OcSkeleton::bones
    Vec3 translation{0, 0, 0};       // offset from the bone, engine centimetres
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};             // rarely anything but 1, and free to carry
};

// A bone hierarchy, parents before children.
struct OcSkeleton {
    std::vector<OcBone> bones;
    u32  rootBone = 0xFFFFFFFFu;     // convenience hint; -1 when there is no single root

    // Sockets, in file order. Empty for every rig written before they existed: the SOCK chunk is
    // optional and its absence is not an error. NAMES ARE NOT CHECKED FOR UNIQUENESS on write --
    // see socket() for what a duplicate means (the first wins, and the editor refuses to create
    // one), because a format that refused would make a rig unopenable over a naming mistake.
    std::vector<OcSocket> sockets;

    // The socket of that name, or nullptr. Linear, because a rig has a handful and a map would
    // cost more to keep in step than the scan saves.
    const OcSocket* socket(const std::string& name) const;

    // The socket whose name hashes to `id` under fnv1a64, or nullptr. Exists because a COMPONENT
    // cannot hold a string -- the scene ABI marshals numbers, so an attachment names its socket by
    // hash exactly as a mesh names its asset by ObjectId. Hashes every name per call rather than
    // caching: a rig has a handful of short names, and a cache would have to be invalidated on
    // every rename the editor performs.
    const OcSocket* socketById(u64 id) const;

    // True when parents are in range, acyclic, and each precedes its children, and every socket
    // names a bone that exists.
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
