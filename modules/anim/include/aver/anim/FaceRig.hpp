#pragma once
// .ocface -- a face rig asset: named poses, each a set of morph-target weights and/or bone deltas, plus the
// phoneme-to-viseme overrides and lip-sync envelope. Design: docs/plans/wave9/MORPH_TARGETS.md 2.6.
//
// Text JSON with a format tag and a version, written and parsed like .ocikrig: floats at nine significant
// digits (lossless for f32), unknown keys ignored, a newer version refused, and parse does NOT call valid()
// so a half-authored rig still round-trips. The 52 ARKit shape names and the 15 Oculus viseme names are
// naming conventions only (no code or data from any vendor): the built-in rig below is plain starting art.
//
// FaceRigRuntime resolves a pose name for one entity. First match wins: (1) an explicit pose of that name in
// the entity's rig, (2) a mesh target of the same name (identity, so an ARKit or Ready-Player-Me style mesh
// needs no asset), (3) the built-in rig's pose of that name (which supplies the viseme_* compositions to a
// mesh that has only the ARKit shapes). Pure over structs: no scene, no device.
#include "aver/anim/Pose.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::anim {

inline constexpr int kFaceRigVersion = 1;
inline constexpr u32 kFaceRigMaxPoses   = 256;
inline constexpr u32 kFaceRigMaxTargets = 32;    // per pose
inline constexpr u32 kFaceRigMaxJoints  = 16;    // per pose
inline constexpr usize kFaceNameMax     = 128;

// The 52 ARKit blend-shape names (a naming convention).
inline constexpr u32 kArkitShapeCount = 52;
inline constexpr const char* kArkitShapes[kArkitShapeCount] = {
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight", "cheekPuff",
    "cheekSquintLeft", "cheekSquintRight", "eyeBlinkLeft", "eyeBlinkRight", "eyeLookDownLeft", "eyeLookDownRight",
    "eyeLookInLeft", "eyeLookInRight", "eyeLookOutLeft", "eyeLookOutRight", "eyeLookUpLeft", "eyeLookUpRight",
    "eyeSquintLeft", "eyeSquintRight", "eyeWideLeft", "eyeWideRight", "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight", "mouthFunnel",
    "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft", "mouthPressRight", "mouthPucker",
    "mouthRight", "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper", "mouthSmileLeft",
    "mouthSmileRight", "mouthStretchLeft", "mouthStretchRight", "mouthUpperUpLeft", "mouthUpperUpRight",
    "noseSneerLeft", "noseSneerRight", "tongueOut"};

// The 15 viseme pose names (the Oculus set's names; the compositions are ours).
inline constexpr u32 kVisemeCount = 15;
inline constexpr const char* kVisemeNames[kVisemeCount] = {
    "viseme_sil", "viseme_PP", "viseme_FF", "viseme_TH", "viseme_DD", "viseme_kk", "viseme_CH", "viseme_SS",
    "viseme_nn", "viseme_RR", "viseme_aa", "viseme_E", "viseme_ih", "viseme_oh", "viseme_ou"};

// The lip-sync envelope (seconds). Defaults are the frozen ones (section 2.11).
struct LipSyncParams {
    f32 lead = 0.04f;       // a viseme starts rising this long before its event
    f32 attack = 0.05f;     // and takes this long more to reach full weight
    f32 release = 0.08f;    // falls away over this long after the event ends
    f32 minHold = 0.06f;    // an event is held at least this long
    f32 maxTotal = 1.0f;    // the viseme weights are scaled down to sum to at most this
};

struct FaceTargetRef { std::string name; f32 weight = 1.0f; };
// A bone delta in the bone's LOCAL frame, applied with the pose weight w: position += p * w,
// rotation = rotation * slerp(identity, q, w), scale *= lerp(1, s, w).
struct FaceJoint {
    std::string bone;
    Vec3 position{0, 0, 0};
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};
struct FacePose {
    std::string name;
    std::vector<FaceTargetRef> targets;
    std::vector<FaceJoint> joints;
};

struct FaceRigAsset {
    std::string convention = "arkit52";                 // informational: arkit52 | custom
    std::string mesh, skeleton;                          // editor-preview hints only
    std::vector<FacePose> poses;
    std::vector<std::pair<std::string, std::string>> phonemes;   // symbol -> viseme pose name (overrides)
    LipSyncParams lipsync;

    // Limits and field sanity (256 poses, 32 targets and 16 joints per pose, names <= 128 bytes and unique,
    // weights finite in [-4, 4]).
    bool valid(std::string* why = nullptr) const;
    const FacePose* find(std::string_view name) const;
};

std::string writeFaceRig(const FaceRigAsset& a);
bool parseFaceRig(std::string_view json, FaceRigAsset& out, std::string* why = nullptr);
// Saves through a temporary file and a rename, so a crash cannot leave half a rig.
bool saveFaceRig(const std::string& path, const FaceRigAsset& a, std::string* why = nullptr);
bool loadFaceRig(const std::string& path, FaceRigAsset& out, std::string* why = nullptr);

// The built-in rig: the 52 ARKit names as identity poses plus the 15 viseme compositions. Built once.
const FaceRigAsset& defaultFaceRig();

// ARPAbet (39 symbols) -> viseme pose name ("viseme_aa"); the silence symbols sil, sp and pau map to
// viseme_sil. Null for an unknown symbol.
const char* arpabetViseme(std::string_view symbol);
inline constexpr u32 kArpabetCount = 39;
const char* arpabetSymbol(u32 index);        // 0..38, null beyond

// A viseme pose NAME for any symbol a track or script may use: the rig's phoneme overrides, then ARPAbet,
// then a viseme name as is ("aa", "PP", "viseme_aa"). Empty when unknown. `visemeOnly` skips the phoneme tables.
std::string resolveVisemeName(const FaceRigAsset* rig, std::string_view symbol, bool visemeOnly = false);

// One pose weight turned into mesh target weights.
struct FaceTargetOut { u64 target; f32 weight; };

class FaceRigRuntime {
public:
    // The entity's rig; null = built-in only. Copies nothing: the asset must outlive the binding.
    void bind(const FaceRigAsset* rig);
    const FaceRigAsset* rig() const { return rig_; }

    // The mesh's declared target ids (fnv1a64 of the names), as the renderer reports them. `known` false =
    // nothing declared yet (rule 2 cannot apply).
    void setDeclared(const std::vector<u64>& ids, bool known);

    // Rule order above. Appends (target id, w * pose weight) pairs to `out`; false when no rule matched.
    bool expand(u64 poseId, f32 w, std::vector<FaceTargetOut>& out) const;

    // Applies every joint delta of pose `poseId` (the explicit rig pose, else the built-in one) to `pose`.
    // A bone the skeleton lacks is skipped and warned once. `w` is clamped to [0, 1].
    void applyJoints(u64 poseId, f32 w, const fmt::OcSkeleton& skel, Pose& pose);
    // True when pose `poseId` has joint deltas (so a caller can skip binding bones for pure-morph rigs).
    bool hasJoints(u64 poseId) const;

private:
    struct CompiledPose {
        std::vector<std::pair<u64, f32>> targets;     // (target id, weight)
        const FacePose* src = nullptr;
    };
    static void compile(const FaceRigAsset& a, std::unordered_map<u64, CompiledPose>& out);
    const CompiledPose* lookup(u64 poseId) const;

    const FaceRigAsset* rig_ = nullptr;
    std::unordered_map<u64, CompiledPose> mine_;      // the entity's rig
    std::unordered_set<u64> declared_;
    bool declaredKnown_ = false;
    // Bone name -> index for the last skeleton applyJoints saw.
    const fmt::OcSkeleton* boundSkel_ = nullptr;
    usize boundBones_ = 0;
    std::unordered_map<std::string, i32> boneIndex_;
    std::unordered_set<std::string> warned_;
};

} // namespace aver::anim
