// Blend-shape arithmetic on the CPU: the reference the GPU skinning pass is checked against, the bounds
// allowance, and the active-list selection the renderer shares with its tests (MORPH_TARGETS.md 2.7, 2.8).
// Pure: OcMorphTarget data in, vertices out. Nothing here knows about a mesh asset, a scene or a device.
#pragma once

#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"

#include <vector>

namespace aver::anim {

// Frozen limits (MORPH_TARGETS.md 2.11).
inline constexpr f32 kMorphWeightMin   = -4.0f;
inline constexpr f32 kMorphWeightMax   = 4.0f;
inline constexpr f32 kMorphActiveEps   = 1.0f / 4096.0f;   // 2^-12: a weight under this is not applied
inline constexpr u32 kMorphMaxActive   = 64;               // targets per dispatch
inline constexpr u32 kMorphIdleFrames  = 8;                // empty-set frames before a morph-only mesh returns to static

// One weight on one target of a mesh (index into OcMeshData::morphTargets).
struct MorphTargetWeight {
    u32 target = 0;
    f32 weight = 0.0f;
};

// position += w * dPos, normal += w * dNrm for every listed target, then every normal renormalised (a
// near-zero one is left as it is) -- the same formula as the shader's CSSkin loop, over the sparse lists
// directly instead of the rank bitmaps. With n == 0 the outputs are the rest values untouched. An invalid
// target index or a mis-sized stream leaves `out*` empty.
void applyMorphTargets(const std::vector<fmt::OcMorphTarget>& targets, const MorphTargetWeight* w, u32 n,
                       const std::vector<f32>& restPositions, const std::vector<f32>& restNormals,
                       std::vector<f32>& outPositions, std::vector<f32>& outNormals);

// The per-axis allowance a set of weights adds to a mesh's bounds: sum over targets of |w| * maxAbs. Deltas
// apply in rest space before skinning, so widening every rest box by this is exact-conservative.
Vec3 morphBoundsPad(const std::vector<fmt::OcMorphTarget>& targets, const MorphTargetWeight* w, u32 n);

// Chooses what one dispatch applies: drops weights with |w| < kMorphActiveEps, indices >= targetCount and
// targets that move nothing (maxLen 0), and when more than kMorphMaxActive remain keeps the largest by
// |w| * maxLen. `out` has room for kMorphMaxActive; the result is in ascending target order (ties: the
// later input entry of a repeated target wins). Returns the count.
u32 selectActiveTargets(const f32* maxLen, u32 targetCount, const MorphTargetWeight* in, u32 n,
                        MorphTargetWeight* out);

inline f32 clampMorphWeight(f32 w) { return w < kMorphWeightMin ? kMorphWeightMin : (w > kMorphWeightMax ? kMorphWeightMax : w); }

} // namespace aver::anim
