#pragma once
// The static GPU data of a mesh's blend shapes, and the C++ twin of the shader's lookup (MORPH_TARGETS.md 2.7).
//
// Pure, no RHI. Each target keeps the file's own sparse delta list plus a RANK BITMAP over the mesh's vertices:
// one {membership bits, bits set in earlier words} pair per 32 vertices. A vertex finds its delta in O(1) with
// no inverse index and no atomics:
//
//     word = v >> 5;  bit = 1 << (v & 31);  {bits, prefix} = rank[target * wordsPerTarget + word]
//     member  = (bits & bit) != 0;   index = deltaBase[target] + prefix + popcount(bits & (bit - 1))
//
// A dispatch applies at most kMorphMaxActive targets, each named by a MorphActiveRecord (where its rank words
// and its deltas start, and the weight). skin.hlsl's CSSkin loop and evalMorphLayout are the same formula.
#include "aver/anim/Morph.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::render {

using anim::MorphTargetWeight;
inline constexpr u32 kMorphMaxActive = anim::kMorphMaxActive;

// Floats per delta entry: dp xyz then dn xyz (dn zero for a target without normal deltas). The shader's
// MorphDelta is two float3, 24 bytes.
inline constexpr u32 kMorphDeltaFloats = 6;
inline constexpr u32 kMorphDeltaStride = kMorphDeltaFloats * sizeof(f32);
inline constexpr u32 kMorphRankStride  = 8;     // uint2 {bits, prefix}
inline constexpr u32 kMorphActiveStride = 16;   // uint rankBase, uint deltaBase, float weight, uint pad
// Above this the layout is refused and the mesh draws unmorphed (logged by the caller).
inline constexpr u64 kMorphMaxBytes = 128ull * 1024 * 1024;

// One entry of a dispatch's active list; the GPU's MorphActive.
struct MorphActiveRecord {
    u32 rankBase = 0;     // first rank word of the target (target * wordsPerTarget)
    u32 deltaBase = 0;    // first delta entry of the target
    f32 weight = 0.0f;
    u32 pad = 0;
};
static_assert(sizeof(MorphActiveRecord) == kMorphActiveStride, "MorphActive is 16 bytes in HLSL");

struct MorphLayout {
    u32 vertexCount = 0, targetCount = 0, wordsPerTarget = 0;
    std::vector<u32> rank;          // targetCount * wordsPerTarget * 2: {bits, prefix}
    std::vector<f32> delta;         // 6 floats per entry, targets back to back in file order
    std::vector<u32> deltaBase;     // first entry of each target
    std::vector<u32> entryCount;
    std::vector<f32> maxLen;        // per target: the longest position delta (active-list importance)

    u64 bytes() const { return u64(rank.size()) * 4 + u64(delta.size()) * 4; }
    u32 totalEntries() const { return static_cast<u32>(delta.size() / kMorphDeltaFloats); }
};

// Builds the layout; false (with `why`) for an empty target list, a bad target, or more than kMorphMaxBytes.
bool buildMorphLayout(const std::vector<fmt::OcMorphTarget>& targets, u32 vertexCount, MorphLayout& out,
                      std::string* why = nullptr);

// Chooses (anim::selectActiveTargets: drops |w| < eps, caps at 64 by |w| * maxLen) and converts to records.
// Returns the count.
u32 buildActiveList(const MorphLayout& layout, const MorphTargetWeight* in, u32 n, MorphActiveRecord* out);

// The shader's per-vertex lookup, on the CPU: sums every active target's delta at `vertex`.
void evalMorphLayout(const MorphLayout& layout, const MorphActiveRecord* active, u32 n, u32 vertex, Vec3& dp, Vec3& dn);

} // namespace aver::render
