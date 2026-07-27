#pragma once
// .ocskel and .ocanim — the skeleton (FORMAT_SPECS.md §8) and the animation clip (§9), both AVR1
// containers.
//
// ONE HEADER FOR TWO FORMATS, deliberately, where the rest of this module is one file per format. A
// clip is not independently meaningful: every track in it is addressed by a bone INDEX, and an index
// without the skeleton that defines it is a number. They are authored together, imported together
// from the same glTF, and read together, and splitting them would put the bone-index contract in
// two files with nothing keeping them honest.
//
// WHAT IS IMPLEMENTED.
//
// .ocskel: the SKEL chunk -- the bone hierarchy, each bone's local rest transform, and its inverse
// bind matrix -- plus STRT for names. That is everything needed to evaluate a pose.
//
// .ocanim: AHDR and TRKS in full, including the three fidelity properties the spec exists to
// protect. No forced 30 fps resample (Storage=0 keeps authored key times), cubicspline tangents
// preserved (three values per key, in-tangent/value/out-tangent), and step curves preserved as a
// distinct interpolation mode rather than being approximated by dense linear keys. Those three are
// named in §9 as the losses of the previous pipeline, so writing a format that silently resampled
// would reproduce the exact defect it was specified to fix.
//
// WHAT IS NOT, and why it is absent rather than stubbed.
//
// The skinned-mesh half of .ocskel -- the JOINTS/WEIGHTS vertex streams and the SKIN chunk -- is not
// written. Those describe how a MESH deforms, and this engine cannot skin: rhi::MeshVertex is 32
// interleaved bytes asserted against the D3D12 input layout, the mesh-shader raw SRV and the BLAS
// stride, with no room for influences, and there is no skinning dispatch anywhere. Writing those
// streams now would be writing bytes nothing can read, into a vertex ABI that has to change first.
// A .ocskel written here is a skeleton, and a reader that later gains SKIN finds it absent and says
// so rather than misreading it -- which is what the container's optional-chunk rule is for.
//
// So this is animation DATA, complete and lossless, ahead of an animation RUNTIME. That order is
// deliberate: the data outlives the runtime, and an importer that lands before the format is settled
// produces files that have to be re-imported later.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// ---- .ocskel ----

inline constexpr i32 kOcBoneNoParent = -1;

struct OcBone {
    std::string name;
    i32  parent = kOcBoneNoParent;   // index into OcSkeleton::bones, or -1 for a root
    Vec3 translation{0, 0, 0};       // local rest pose, engine centimetres
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
    // Row-major, row-vector, engine space -- the same convention as every other matrix in this
    // engine. Stored rather than derived: the inverse of the accumulated rest pose is what a skin
    // needs, and recomputing it at load would turn a rounding difference in the exporter into a
    // slightly wrong bind pose with no way to tell.
    f32  inverseBind[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

struct OcSkeleton {
    std::vector<OcBone> bones;
    u32  rootBone = 0xFFFFFFFFu;     // convenience hint; -1 when there is no single root

    bool valid() const;              // parents in range, no cycles, no bone before its parent
};

bool loadOcSkel(const std::string& path, OcSkeleton& out, std::string* why = nullptr);
bool saveOcSkel(const std::string& path, const OcSkeleton& in, std::string* why = nullptr);
bool parseOcSkel(const u8* bytes, usize size, OcSkeleton& out, std::string* why = nullptr);
bool writeOcSkel(const OcSkeleton& in, std::vector<u8>& out, std::string* why = nullptr);

// ---- .ocanim ----

// Storage (§9.1). Keyframed is the authored form and the default; baked-uniform exists so a runtime
// that wants constant-time sampling can have it without the format losing the original.
enum class OcAnimStorage : u8 { Keyframed = 0, BakedUniform = 1 };

// Interpolation (§9.2). STEP and CUBICSPLINE are carried rather than flattened -- see the header.
enum class OcInterp : u8 { Linear = 0, Step = 1, CubicSpline = 2 };

inline constexpr u8 kOcChannelTranslation = 1 << 0;
inline constexpr u8 kOcChannelRotation    = 1 << 1;
inline constexpr u8 kOcChannelScale       = 1 << 2;

inline constexpr u8 kOcAnimLoop         = 1 << 0;
inline constexpr u8 kOcAnimAdditiveBase  = 1 << 1;
inline constexpr u8 kOcAnimRootMotion    = 1 << 2;

// One animated channel of one bone. Times are seconds from the clip's start and are NOT required to
// be uniformly spaced -- that is the point of Storage::Keyframed.
struct OcTrack {
    u16      boneIndex = 0;
    u8       channels  = 0;                    // kOcChannel* mask; what `values` holds per key
    OcInterp interp    = OcInterp::Linear;
    std::vector<f32> times;                    // KeyCount entries, ascending
    // KeyCount * stride floats, where stride is the sum of the enabled channels' widths
    // (translation 3, rotation 4, scale 3) times 3 for CUBICSPLINE (in-tangent, value, out-tangent)
    // or times 1 otherwise. Laid out key-major.
    std::vector<f32> values;

    u32 componentsPerKey() const;              // stride above; 0 if the mask is empty
    bool valid() const;
};

struct OcAnimation {
    f32  duration = 0.0f;                      // seconds
    OcAnimStorage storage = OcAnimStorage::Keyframed;
    u8   flags = 0;
    u16  sampleRate = 0;                       // only meaningful for BakedUniform
    std::string skeletonRef;                   // which skeleton the bone indices belong to
    std::vector<OcTrack> tracks;

    bool valid() const;
};

bool loadOcAnim(const std::string& path, OcAnimation& out, std::string* why = nullptr);
bool saveOcAnim(const std::string& path, const OcAnimation& in, std::string* why = nullptr);
bool parseOcAnim(const u8* bytes, usize size, OcAnimation& out, std::string* why = nullptr);
bool writeOcAnim(const OcAnimation& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
