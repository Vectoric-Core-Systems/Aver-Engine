// Blend-shape rank bitmaps and delta lists. See MorphLayout.hpp.
#include "aver/render/MorphLayout.hpp"

#include <algorithm>
#include <bit>

namespace aver::render {

bool buildMorphLayout(const std::vector<fmt::OcMorphTarget>& targets, u32 vertexCount, MorphLayout& out,
                      std::string* why) {
    out = MorphLayout{};
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (targets.empty() || vertexCount == 0) return fail("no blend shapes");

    const u32 words = (vertexCount + 31) / 32;
    u64 entries = 0;
    for (const fmt::OcMorphTarget& t : targets) entries += t.vertices.size();
    const u64 bytes = u64(targets.size()) * words * kMorphRankStride + entries * kMorphDeltaStride;
    if (bytes > kMorphMaxBytes)
        return fail("blend-shape data of " + std::to_string(bytes >> 20) + " MiB exceeds the " +
                    std::to_string(kMorphMaxBytes >> 20) + " MiB limit");

    out.vertexCount = vertexCount;
    out.targetCount = static_cast<u32>(targets.size());
    out.wordsPerTarget = words;
    out.rank.assign(usize(targets.size()) * words * 2, 0);
    out.delta.reserve(usize(entries) * kMorphDeltaFloats);
    out.deltaBase.reserve(targets.size());
    out.entryCount.reserve(targets.size());
    out.maxLen.reserve(targets.size());

    for (usize ti = 0; ti < targets.size(); ++ti) {
        const fmt::OcMorphTarget& t = targets[ti];
        const usize n = t.vertices.size();
        if (t.positionDeltas.size() != n * 3 || (!t.normalDeltas.empty() && t.normalDeltas.size() != n * 3))
            return fail("blend shape '" + t.name + "' has mismatched delta arrays");
        out.deltaBase.push_back(static_cast<u32>(out.delta.size() / kMorphDeltaFloats));
        out.entryCount.push_back(static_cast<u32>(n));
        out.maxLen.push_back(t.maxLen);

        u32* rw = &out.rank[ti * words * 2];
        u32 prev = ~0u;
        for (usize k = 0; k < n; ++k) {
            const u32 v = t.vertices[k];
            if (v >= vertexCount || (prev != ~0u && v <= prev))
                return fail("blend shape '" + t.name + "' has vertices that are out of range or not ascending");
            prev = v;
            rw[(v >> 5) * 2] |= 1u << (v & 31);
            for (int c = 0; c < 3; ++c) out.delta.push_back(t.positionDeltas[k * 3 + c]);
            for (int c = 0; c < 3; ++c) out.delta.push_back(t.normalDeltas.empty() ? 0.0f : t.normalDeltas[k * 3 + c]);
        }
        u32 run = 0;
        for (u32 w = 0; w < words; ++w) {
            rw[w * 2 + 1] = run;
            run += static_cast<u32>(std::popcount(rw[w * 2]));
        }
    }
    return true;
}

u32 buildActiveList(const MorphLayout& layout, const MorphTargetWeight* in, u32 n, MorphActiveRecord* out) {
    MorphTargetWeight picked[kMorphMaxActive];
    const u32 count = anim::selectActiveTargets(layout.maxLen.data(), layout.targetCount, in, n, picked);
    for (u32 i = 0; i < count; ++i) {
        const u32 t = picked[i].target;
        out[i] = MorphActiveRecord{t * layout.wordsPerTarget, layout.deltaBase[t], picked[i].weight, 0};
    }
    return count;
}

void evalMorphLayout(const MorphLayout& layout, const MorphActiveRecord* active, u32 n, u32 vertex, Vec3& dp, Vec3& dn) {
    dp = Vec3{0, 0, 0};
    dn = Vec3{0, 0, 0};
    const u32 word = vertex >> 5;
    const u32 bit = 1u << (vertex & 31);
    for (u32 k = 0; k < n; ++k) {
        const MorphActiveRecord& a = active[k];
        const usize ri = usize(a.rankBase + word) * 2;
        if (ri + 1 >= layout.rank.size()) continue;
        const u32 bits = layout.rank[ri], prefix = layout.rank[ri + 1];
        if ((bits & bit) == 0) continue;
        const usize di = usize(a.deltaBase + prefix + static_cast<u32>(std::popcount(bits & (bit - 1u)))) * kMorphDeltaFloats;
        if (di + kMorphDeltaFloats > layout.delta.size()) continue;
        const f32* d = &layout.delta[di];
        dp += Vec3{d[0], d[1], d[2]} * a.weight;
        dn += Vec3{d[3], d[4], d[5]} * a.weight;
    }
}

} // namespace aver::render
