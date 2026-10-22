// RibbonPack.cpp -- the static ribbon layout of a groom and the C++ twin of AverHairRibbons.hlsl. See RibbonPack.hpp.
#include "aver/render/RibbonPack.hpp"

#include <algorithm>
#include <cmath>

namespace aver::render {

namespace {

// A small seeded generator (splitmix64), so the rank shuffle is a pure function of the seed on every platform.
struct SplitMix {
    u64 s;
    u64 next() {
        s += 0x9e3779b97f4a7c15ull;
        u64 z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    // [0, n)
    u32 below(u32 n) { return static_cast<u32>((next() >> 11) % n); }
};

u32 strandHashOf(u64 seed, u32 strand) {
    return ribbonMix(static_cast<u32>(seed) ^ ribbonMix(static_cast<u32>(seed >> 32) + strand * 0x9E3779B9u + 0x7f4a7c15u));
}

} // namespace

bool buildRibbonLayout(const RibbonInput& in, RibbonLayout& out, std::string* why) {
    out = RibbonLayout{};
    if (!in.strandStartCount2 || in.strandCount == 0) {
        if (why) *why = "the groom has no render strands";
        return false;
    }
    u64 totalPoints = 0;
    for (u32 s = 0; s < in.strandCount; ++s) {
        const i32 start = in.strandStartCount2[s * 2];
        const i32 count = in.strandStartCount2[s * 2 + 1];
        if (start < 0 || count < 2) {
            if (why) *why = "render strand " + std::to_string(s) + " has fewer than two points";
            return false;
        }
        totalPoints += static_cast<u64>(count);
    }
    if (totalPoints * 2 > 0xFFFFFFFFull / 2) {
        if (why) *why = "the groom has too many ribbon vertices for 32-bit indices";
        return false;
    }

    // The rank order: a seeded Fisher-Yates over the strand indices.
    out.strandCount = in.strandCount;
    out.rankToStrand.resize(in.strandCount);
    for (u32 i = 0; i < in.strandCount; ++i) out.rankToStrand[i] = i;
    SplitMix rng{in.seed ? in.seed : 1ull};
    for (u32 i = in.strandCount; i > 1; --i) std::swap(out.rankToStrand[i - 1], out.rankToStrand[rng.below(i)]);

    out.points.reserve(static_cast<usize>(totalPoints));
    out.indices.reserve(static_cast<usize>(totalPoints) * 6);

    // The level boundaries in strands, then walk the ranks once, noting the running totals where each level ends.
    u32 levelStrands[kRibbonLodLevels];
    for (u32 l = 0; l < kRibbonLodLevels; ++l) {
        f32 f = in.lodFractions[l];
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        u32 n = static_cast<u32>(std::ceil(static_cast<f64>(in.strandCount) * static_cast<f64>(f)));
        n = std::max<u32>(1u, std::min(n, in.strandCount));
        if (l > 0) n = std::min(n, levelStrands[l - 1]);
        levelStrands[l] = n;
        out.lodStrands[l] = n;
    }

    u32 vertexBase = 0;   // ribbon vertex index of the next point's left vertex
    for (u32 rank = 0; rank < in.strandCount; ++rank) {
        const u32 strand = out.rankToStrand[rank];
        const i32 start = in.strandStartCount2[strand * 2];
        const i32 count = in.strandStartCount2[strand * 2 + 1];
        const f32 wRoot = in.widthRootCm ? in.widthRootCm[strand] : 0.1f;
        const f32 wTip = in.widthTipCm ? in.widthTipCm[strand] : wRoot;
        const u32 hash = strandHashOf(in.seed, strand);
        const f32 rankFrac = static_cast<f32>(rank) / static_cast<f32>(in.strandCount);
        for (i32 k = 0; k < count; ++k) {
            RibbonPoint p;
            p.vtx = static_cast<u32>(start + k);
            p.prev = static_cast<u32>(start + (k > 0 ? k - 1 : 0));
            p.next = static_cast<u32>(start + (k + 1 < count ? k + 1 : count - 1));
            p.fraction = count > 1 ? static_cast<f32>(k) / static_cast<f32>(count - 1) : 0.0f;
            p.widthCm = wRoot + (wTip - wRoot) * p.fraction;
            p.strandHash = hash;
            p.rankFrac = rankFrac;
            out.points.push_back(p);
        }
        for (i32 k = 0; k + 1 < count; ++k) {
            const u32 a = vertexBase + static_cast<u32>(k) * 2u;
            out.indices.push_back(a);
            out.indices.push_back(a + 1);
            out.indices.push_back(a + 2);
            out.indices.push_back(a + 1);
            out.indices.push_back(a + 3);
            out.indices.push_back(a + 2);
        }
        vertexBase += static_cast<u32>(count) * 2u;
        for (u32 l = 0; l < kRibbonLodLevels; ++l)
            if (rank + 1 == levelStrands[l]) {
                out.lodPoints[l] = static_cast<u32>(out.points.size());
                out.lodIndices[l] = static_cast<u32>(out.indices.size());
            }
    }
    return true;
}

RibbonResult ribbonVertices(const RibbonPoint& rp, const f32 pJ[3], const f32 prevJ[3], const f32 nextJ[3], const RibbonCamera& cam,
                            rhi::MeshVertex out[2]) {
    RibbonResult r;
    const Vec3 p = ribbonJoltToLocalCm(pJ);
    const Vec3 a = ribbonJoltToLocalCm(prevJ);
    const Vec3 b = ribbonJoltToLocalCm(nextJ);

    Vec3 T = b - a;
    const f32 tl = T.size();
    T = tl > 1.0e-6f ? T / tl : Vec3(0.0f, 0.0f, -1.0f);

    const Vec3 toCam = cam.camLocal - p;
    const f32 dist = std::max(toCam.size(), 1.0e-3f);

    const f32 w = rp.widthCm * cam.widthScale;
    const f32 wPx = dist * cam.pixelAt1cm * cam.minPixels;
    const f32 W = std::max(w, wPx);
    const f32 cover = w / std::max(W, 1.0e-8f);

    bool dropped = rp.rankFrac >= cam.keepRank;
    if (!dropped && cover < 1.0f) dropped = ribbonUnit(ribbonMix(rp.strandHash ^ (cam.frameSeed * 0x9E3779B9u))) >= cover;

    Vec3 S = cross(T, toCam);
    const f32 sl = S.size();
    if (sl > 1.0e-6f) {
        S = S / sl;
    } else {
        const Vec3 axis = std::fabs(T.z) < 0.9f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
        S = cross(T, axis).getSafeNormal();
    }

    const f32 hw = dropped ? 0.0f : 0.5f * W;
    const f32 v = ribbonEncodeV(rp.strandHash, rp.fraction);
    const Vec3 l = p - S * hw;
    const Vec3 rr = p + S * hw;
    out[0] = rhi::MeshVertex{l.x, l.y, l.z, T.x, T.y, T.z, -1.0f, v};
    out[1] = rhi::MeshVertex{rr.x, rr.y, rr.z, T.x, T.y, T.z, 1.0f, v};
    r.widthDrawn = dropped ? 0.0f : W;
    r.coverage = cover;
    r.dropped = dropped;
    r.tangent = T;
    r.side = S;
    return r;
}

} // namespace aver::render
