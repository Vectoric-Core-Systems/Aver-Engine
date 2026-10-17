#pragma once
// Private to the FBX importer (FbxImport.cpp, FbxMaterials.cpp, FbxSkeleton.cpp, FbxAnimation.cpp).
// Not a public header: the tests that need it add this folder to their include path.
//
// FRAME. ufbx delivers the scene right-handed, +Y up, in metres (FbxImport.cpp asks for it), which is
// glTF's frame, so the conversion to the engine is the glTF importer's map (GltfImport.cpp toEngine /
// toEngineQuat / toEngineScale) applied to ufbx output, times `scale`.
//
// MATRICES. ufbx_matrix is 4x3 column-vector: cols[0..2] are the images of the X/Y/Z basis vectors and
// cols[3] is the translation. The engine's Mat4 is row-vector row-major with the basis in rows 0..2 and
// the translation in row 3, so a ufbx matrix is copied in with no transpose.
#include "aver/formats/FbxImport.hpp"

#ifndef AVER_HAVE_UFBX
#define AVER_HAVE_UFBX 0
#endif

#if AVER_HAVE_UFBX
#include "ufbx.h"
static_assert(UFBX_HEADER_VERSION >= ufbx_pack_version(0, 23, 0), "FbxImport needs ufbx 0.23.0 or newer");

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::fmt::fbx {

// ---- the engine basis change --------------------------------------------------------------------

struct Basis {
    bool convertAxes = true;
    f32  scale = 100.0f;        // options.scale, times the unit override when one is given

    // A position (scaled) or, with isDirection, a direction (not).
    Vec3 toEngine(f64 x, f64 y, f64 z, bool isDirection) const {
        Vec3 v = convertAxes ? Vec3{f32(-z), f32(x), f32(y)} : Vec3{f32(x), f32(y), f32(z)};
        if (!isDirection) { v.x *= scale; v.y *= scale; v.z *= scale; }
        return v;
    }
    Vec3 pos(const ufbx_vec3& v) const { return toEngine(v.x, v.y, v.z, false); }
    Vec3 dir(const ufbx_vec3& v) const { return toEngine(v.x, v.y, v.z, true); }
    // Not the obvious component shuffle: the map has determinant -1, so a rotation's axis picks up a
    // sign the position vectors do not (see GltfImport.cpp toEngineQuat).
    Quat quat(const ufbx_quat& q) const {
        return convertAxes ? Quat{f32(q.z), f32(-q.x), f32(-q.y), f32(q.w)} : Quat{f32(q.x), f32(q.y), f32(q.z), f32(q.w)};
    }
    Vec3 scl(const ufbx_vec3& s) const {
        return convertAxes ? Vec3{f32(s.z), f32(s.x), f32(s.y)} : Vec3{f32(s.x), f32(s.y), f32(s.z)};
    }
    // The same maps for components that did not come from a ufbx value.
    Quat quat(f64 x, f64 y, f64 z, f64 w) const {
        return convertAxes ? Quat{f32(z), f32(-x), f32(-y), f32(w)} : Quat{f32(x), f32(y), f32(z), f32(w)};
    }
    Vec3 scl(f64 x, f64 y, f64 z) const {
        return convertAxes ? Vec3{f32(z), f32(x), f32(y)} : Vec3{f32(x), f32(y), f32(z)};
    }
};

// The 3x3 rotation*scale basis baked into a mesh copy: rows a, b, c of the row-vector matrix.
struct Basis3 { f32 k[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; };

inline Basis3 basisOf(const ufbx_matrix& m) {
    Basis3 b;
    for (int r = 0; r < 3; ++r) {
        b.k[r * 3 + 0] = f32(m.cols[r].x);
        b.k[r * 3 + 1] = f32(m.cols[r].y);
        b.k[r * 3 + 2] = f32(m.cols[r].z);
    }
    return b;
}

inline f32 det3(const Basis3& b) {
    const f32* a = &b.k[0]; const f32* bb = &b.k[3]; const f32* c = &b.k[6];
    return a[0] * (bb[1] * c[2] - bb[2] * c[1]) - a[1] * (bb[0] * c[2] - bb[2] * c[0]) + a[2] * (bb[0] * c[1] - bb[1] * c[0]);
}

// A ufbx matrix as the engine's row-major row-vector Mat4, no basis change (still the ufbx frame).
inline Mat4 toMat4(const ufbx_matrix& m) {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        r.m[i][0] = f32(m.cols[i].x); r.m[i][1] = f32(m.cols[i].y); r.m[i][2] = f32(m.cols[i].z);
        r.m[i][3] = i == 3 ? 1.0f : 0.0f;
    }
    return r;
}

inline bool isIdentity(const ufbx_matrix& m, f64 eps = 1e-9) {
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) {
            const f64 want = (i == k) ? 1.0 : 0.0;
            if (std::fabs(m.cols[i].v[k] - want) > eps) return false;
        }
    return true;
}

inline std::string str(const ufbx_string& s) { return s.length ? std::string(s.data, s.length) : std::string(); }

// A name safe to use as a filename stem (letters, digits, dash, underscore); edge underscores trimmed.
inline std::string sanitizeStem(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

inline std::string lowerOf(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// ---- the shared import state ---------------------------------------------------------------------

// "Bone b of skeleton s", per scene node. -1 / -1 = not a bone.
struct BoneRef { i32 skeleton = -1; i32 bone = -1; };

struct Ctx {
    const ufbx_scene* scene = nullptr;
    const FbxImportOptions* opt = nullptr;
    FbxImportResult* out = nullptr;
    std::string baseDir;
    std::string sourceStem;   // the .fbx file's stem; empty for a memory import
    Basis basis;

    // Per scene->nodes[typed_id].
    std::vector<BoneRef> boneOfNode;
    std::vector<i32>     placementOfNode;     // into out->placements, -1 for none
    std::vector<Basis3>  bakedBasisOfNode;    // the basis baked into that placement's mesh copy (ufbx frame)
    std::vector<u8>      skinnedMeshNode;     // a mesh node imported with a skin: no placement motion, no object clip
    // Per scene->materials[typed_id]: the material name == the mesh slot name. Empty before importMaterials.
    std::vector<std::string> materialName;

    // Per skeleton, parallel to out->skeletons: the topmost bone's node, for the fold of its parents.
    std::vector<const ufbx_node*> skeletonTop;
    // Per skeleton, per bone: the node.
    std::vector<std::vector<const ufbx_node*>> boneNodes;

    // Records an unsupported feature, once each.
    void note(const std::string& what) {
        for (const std::string& s : out->unsupported) if (s == what) return;
        out->unsupported.push_back(what);
    }
    void line(const std::string& s) { out->report += s; out->report += '\n'; }
};

// ---- FbxMaterials.cpp ---------------------------------------------------------------------------

// Fills out->materials / out->images (materials parallel to scene->materials) and ctx.materialName.
void importMaterials(Ctx& c);
// The mesh slot name for a material element ("M_Default" when null).
std::string slotNameOf(const Ctx& c, const ufbx_material* m);

// ---- FbxSkeleton.cpp -----------------------------------------------------------------------------

// Builds every skeleton the given skinned mesh nodes need (bind-pose rest, re-derived inverse binds,
// sockets), fills ctx.boneOfNode / boneNodes / skeletonTop and out->skeletons / skeletonNames.
void buildSkeletons(Ctx& c, const std::vector<const ufbx_node*>& skinnedMeshNodes);
// The skeleton a skin's clusters resolved to, or -1.
i32 skeletonOfSkin(const Ctx& c, const ufbx_skin_deformer* skin);
// Fills `m.joints` / `m.weights` for every vertex of `m`: cp[v] is vertex v's control point. Top 4 by
// weight, renormalised; a vertex with no usable weight binds to bone 0. Counts truncation into out->stats.
void writeSkinStreams(Ctx& c, const ufbx_skin_deformer* skin, const std::vector<u32>& cp, OcMeshData& m);
// The bind-pose matrix of the mesh (geometry to world, the cluster's bind_to_world * geometry_to_bone).
// False when the skin has no usable cluster. Disagreement between clusters beyond 1e-3 is reported.
bool skinBindMatrix(Ctx& c, const ufbx_node* node, const ufbx_skin_deformer* skin, ufbx_matrix& out);

// ---- FbxAnimation.cpp ----------------------------------------------------------------------------

// Skeletal clips (one per stack and animated skeleton), then object clips. Runs last.
void importAnimations(Ctx& c);
// The blend-channel weights a baked stack animates, as curves named kFbxMorphCurvePrefix + channel,
// 0..1. Only attached to clips once the morph-target feature can read them (FbxAnimation.cpp).
std::vector<OcCurve> bakeMorphCurves(const ufbx_scene* scene, ufbx_baked_anim* baked);

// ---- FbxImport.cpp -------------------------------------------------------------------------------

// The blend channels of `mesh` as sparse engine-space deltas. `vertexCp[v]` is output vertex v's
// control point; `linear` is the 3x3 (rows, ufbx frame) the mesh's positions went through (translation
// excluded); `unit` is the engine-space scale already inside `basis`. Deltas are DIRECTIONS: they take
// the linear part, normal deltas the inverse-transpose, and no translation. In-between keyframes are
// dropped and counted into `inBetweens`.
std::vector<FbxBlendShape> extractFbxBlendShapes(const Basis& basis, const ufbx_mesh* mesh,
                                                 const std::vector<u32>& vertexCp, const Basis3& linear,
                                                 u32* inBetweens = nullptr);

} // namespace aver::fmt::fbx

#endif // AVER_HAVE_UFBX
