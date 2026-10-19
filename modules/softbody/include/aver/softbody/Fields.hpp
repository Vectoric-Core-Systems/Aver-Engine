// Soft-body fields: a volume (sphere, box, capsule, everywhere) with something to do to the soft bodies inside it.
// This header is the pure half: the shape test, the weight, the velocity change and the beam and anchor queries.
// The system applies them (Aver.SoftBody.Scene, SoftBodyFields.cpp). Core only. See docs/SOFTBODY.md "Fields".
#pragma once

#include "aver/softbody/Cage.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::softbody {

enum class FieldKind  : u8 { RadialForce = 0, DirectionalForce = 1, Damage = 2, ReleaseAnchors = 3, Kill = 4, Sleep = 5 };
enum class FieldShape : u8 { Sphere = 0, Box = 1, Capsule = 2, Everywhere = 3 };
inline constexpr u32 kFieldKindCount = 6, kFieldShapeCount = 4;
inline constexpr u32 kFieldTargetCloth = 1, kFieldTargetDestructible = 2, kFieldTargetDebris = 4, kFieldTargetAll = 7;

struct FieldSpec {
    FieldKind kind = FieldKind::RadialForce;
    FieldShape shape = FieldShape::Sphere;
    u32  targets = kFieldTargetAll;
    bool perSecond = false;            // strength and energy are rates; a pulse applies them once
    Mat4 world = Mat4::identity();     // shape frame, row-vector convention: translation row = centre
    Vec3 extents{100, 100, 100};       // local cm. Sphere: radius = x. Box: half extents. Capsule: radius = x, half height of the axis segment = z (axis +Z)
    Vec3 dir{0, 0, 1};                 // DirectionalForce: local axis. Damage: direction of travel, zero = radial
    f32  strength = 0.0f;              // RadialForce: velocity change at the centre in cm/s, negative pulls. DirectionalForce: velocity change along dir. Per second when perSecond
    f32  energyJ = 0.0f;               // Damage: joules (total for a pulse, per second when perSecond)
    f32  falloff = 1.0f;               // weight = (1 - n)^falloff, n = 0 at the centre line, 1 at the surface
    f32  turbulence = 0.0f;            // 0..1: deterministic noise on magnitude and direction
    f32  wakeCmS = 5.0f;               // a sleeping piece wakes only if the field would change a vertex velocity by this much
};

// The inverse frame prepared once per field and reused for every point. The spec overloads below build one per call.
struct FieldFrame {
    FieldSpec spec;
    Mat4 inverse = Mat4::identity();
    Vec3 centre{0, 0, 0};
    Vec3 dirWorld{0, 0, 0};            // spec.dir carried to world space, unit; zero when spec.dir is zero
};
FieldFrame makeFieldFrame(const FieldSpec&);

// Normalised distance n in [0, inf): 0 on the centre line, 1 at the surface. Everywhere is always 0.
f32  fieldDistance(const FieldFrame&, const Vec3& worldPos);
f32  fieldWeight(const FieldFrame&, const Vec3& worldPos);
f32  fieldWeight(const FieldSpec&, const Vec3& worldPos);                                         // 0 outside the shape
Vec3 fieldVelocityDelta(const FieldFrame&, const Vec3& worldPos, f32 weight, f32 seconds, u64 step);
Vec3 fieldVelocityDelta(const FieldSpec&, const Vec3& worldPos, f32 weight, f32 seconds, u64 step);   // RadialForce / DirectionalForce
bool fieldOverlapsBounds(const FieldSpec&, const Vec3& lo, const Vec3& hi);                        // broad phase against a piece AABB, never a false negative
void fieldBeamWeights(const Cage&, const FieldSpec&, std::vector<f32>& out);                       // by beam midpoint, 0 for broken beams
u32  anchorsInField(const Cage&, const FieldSpec&, std::vector<u32>& particles);                  // pinned particles inside

// A frame matrix from a centre and (yaw, pitch, roll) degrees, composed Rz * Ry * Rx like the placement convention.
Mat4 fieldFrameFrom(const Vec3& centre, const Vec3& yawPitchRollDeg);

// ---- the words of the COMPONENT SoftField record (docs/formats/FORMAT_SPECS.md) ----------------------------------
// kind: radial directional damage release kill sleep.  shape: sphere box capsule all.  targets: a comma list of
// cloth destructible debris.  Shared by the level loader (Runtime/src/LevelComponents.cpp) and the editor's Details model.
const char* fieldKindKey(FieldKind k);
bool fieldKindFromKey(std::string_view key, FieldKind& out);
const char* fieldShapeKey(FieldShape s);
bool fieldShapeFromKey(std::string_view key, FieldShape& out);
// "cloth,debris" -> 5. False for an empty list or an unknown word.
bool fieldTargetsFromText(std::string_view text, u32& mask);
std::string fieldTargetsToText(u32 mask);
// What a field with no `targets` key touches: everything, except a Kill field, which reads debris only.
inline u32 fieldDefaultTargets(FieldKind k) { return k == FieldKind::Kill ? kFieldTargetDebris : kFieldTargetAll; }

} // namespace aver::softbody
