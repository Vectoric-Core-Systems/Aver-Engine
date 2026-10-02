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
// An OBJECT clip moves a placed mesh instead of posing a skeleton: skeletonRef is empty and ONE track on
// boneIndex 0 holds the object's transform A(t) (engine space, cm, Z up) in some fixed frame. Playback
// uses only relative motion, F(t) = B * A(t0)^-1 * A(t), where B is the entity's own placement and t0
// the animator's start time, so the fixed frame never matters.
inline constexpr u8 kOcAnimObject        = 1 << 3;

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
// A NOTIFY ITSELF IS STILL INSTANT -- `time` is the only moment it names, exactly as it always was.
// A hit window that OPENS, STAYS OPEN and CLOSES is a NOTIFY STATE, and it is layered on top of this
// record rather than folded into it: see OcAnimation::notifyDurations and notifyDuration() below.
// It stayed unbuilt for a long time on purpose -- the runtime needs real open/close tracking (a
// clock that remembers what is open, not a second point in time) before a duration means anything,
// and a half-built version that sometimes fails to close is a hit window that never shuts, which is
// worse than not having the feature. AnimSystem now carries that tracking; see its own comment on
// the four ways an open state leaks and how each is closed.
struct OcNotify {
    f32 time = 0.0f;      // seconds from the clip start
    std::string name;     // the event name fired; opaque here, exactly as OcAnimation::skeletonRef is
};

// ONE CURVE: a named float that varies over the clip.
//
// WHAT IT IS FOR. "How far through the reload am I", "how hard is the foot planted", "how much
// should the cloth billow" -- a number the animator authors ALONGSIDE the pose, in the same file,
// so it stays in step with the animation when the animation is re-timed. Without one, every such
// number is either a constant in gameplay code (and wrong the moment the clip changes length) or a
// hand-written comparison against the clip time (which is the same polling loop notifies replaced,
// with the same drift).
//
// A CURVE IS NOT A NOTIFY, and the two are deliberately different records. A notify is an EVENT --
// it happens once, at an instant, and something runs. A curve is a VALUE -- it always has one,
// wherever the playhead is, and nothing runs. Trying to express one with the other gives either an
// event that fires every frame or a value that only exists at three moments.
//
// SCALAR, NOT A VECTOR, and that is a real limit rather than an oversight. Unreal's curves are
// float curves too, and every vector case decomposes into named components without needing the
// format to grow a width field that every reader must then branch on.
//
// CUBICSPLINE USES REAL TANGENTS when the curve carries them -- see inTangents/outTangents below.
// A curve authored before tonight, or one whose tangent arrays do not line up with its keys, has
// none: sampleCurve (aver/anim/AnimSampler.hpp) reads it as LINEAR in that case, exactly as every
// CubicSpline-tagged curve always has, rather than inventing a flat-tangent Hermite for data nobody
// authored. That is what makes the format change purely additive -- see inTangents' own comment.
struct OcCurve {
    std::string name;            // what a script asks for; opaque here
    OcInterp interp = OcInterp::Linear;   // Step holds; CubicSpline uses tangents below when present
    std::vector<f32> times;      // seconds from the clip start, ascending
    std::vector<f32> values;     // one per time

    // Per-key tangents for CubicSpline interpolation -- PARALLEL to times/values (index i is the
    // in/out tangent pair for keys[i]), NOT a widening of the CRVE layout itself. A widened CRVE
    // would change the byte layout -- and so the rewrite -- of every clip that already has a curve,
    // Linear or Step, tangents or not; a separate optional chunk (CTAN, see OcAnim.cpp) leaves an
    // untouched curve's bytes alone.
    //
    // EMPTY, NOT ZERO-FILLED, is how a curve says "nobody authored a tangent here" -- exactly the
    // convention OcAnimation::notifyDurations uses and for the same reason: the CTAN chunk is
    // omitted whenever every tangent in the clip is zero, checked BY VALUE rather than by these
    // vectors being empty, so a zero-filled tangent array and an absent one write the identical
    // file. See writeOcAnim's CTAN block for the full rule, including why a curve with no authored
    // tangents of its own can still come back from disk with a dense (zero-filled) pair once ANOTHER
    // curve in the same clip needs the chunk -- the chunk's granularity is the whole clip, matching
    // NTFD's.
    //
    // Either both are empty or both are exactly times.size() long -- writeOcAnim refuses a curve
    // where only one side was set, or where either is a different length than its keys, or carries
    // a non-finite value, the same three refusals NTFD applies to notifyDurations.
    std::vector<f32> inTangents;
    std::vector<f32> outTangents;
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

    // How long each notify STAYS OPEN, in seconds -- one entry per notify, in the SAME order as
    // `notifies` (entry i belongs to notifies[i]). 0 means that notify is instantaneous, which is
    // every entry in every clip written before notify states existed.
    //
    // EMPTY, NOT ZERO-FILLED, is how a clip says "nothing here is a state" -- and that distinction is
    // the whole reason this is a separate optional array rather than a field added to OcNotify. A
    // clip with a plain notify and no state has an EMPTY vector, writes NO chunk for it (see the
    // NTFD chunk in OcAnim.cpp), and rewrites byte-identically to a file saved before states existed.
    // Widening OcNotify itself would have changed NOTF's own byte layout for every clip that has ever
    // had a notify, state or not.
    //
    // When non-empty its size equals notifies.size(); see writeOcAnim, which refuses to write a
    // mismatched pair rather than truncating or padding one of them.
    std::vector<f32> notifyDurations;

    // The duration of notifies[index], in seconds -- 0.0 (instant) when notifyDurations is empty or
    // too short to cover it, which includes every out-of-range index a stale caller might hold.
    f32 notifyDuration(u32 index) const;

    // Named float curves. Optional in the same way notifies are: a clip with none emits no chunk,
    // so a file written now is byte-identical to one written before curves existed.
    std::vector<OcCurve> curves;

    // The curve of that name, or nullptr. Linear, for the reason OcSkeleton::socket is.
    const OcCurve* curve(const std::string& name) const;
    // By fnv1a64 of the name, for a caller holding a hash rather than a string -- a component or a
    // graph node, which cannot carry one.
    const OcCurve* curveById(u64 id) const;

    // True when the duration, storage and every track are consistent, and an object clip is exactly
    // one track on bone 0.
    bool valid() const;
};

bool loadOcAnim(const std::string& path, OcAnimation& out, std::string* why = nullptr);
bool saveOcAnim(const std::string& path, const OcAnimation& in, std::string* why = nullptr);
bool parseOcAnim(const u8* bytes, usize size, OcAnimation& out, std::string* why = nullptr);
bool writeOcAnim(const OcAnimation& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
