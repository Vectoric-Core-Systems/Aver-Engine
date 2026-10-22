// RibbonPackTest -- the ribbon layout of a groom and the C++ twin of AverHairRibbons.hlsl (docs/plans/wave11/SOFTBODY_HAIR.md 2.16). HEADLESS: no device,
// no GPU. The shader itself is decided on a device by HairSelfTest and the headed checks; what is decided here is the arithmetic both sides must share.
// Exit code = failure count. NOTHING IN HERE HAS BEEN RUN: thresholds are starting values for the first pass.
#include "aver/core/Log.hpp"
#include "aver/render/RibbonPack.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::render;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

// A strand table: `n` strands of `pts` points each, laid out one after the other.
static std::vector<i32> table(u32 n, i32 pts) {
    std::vector<i32> t;
    for (u32 s = 0; s < n; ++s) { t.push_back(static_cast<i32>(s) * pts); t.push_back(pts); }
    return t;
}

int main() {
    AVER_INFO("=== the layout ===");
    {
        const u32 n = 100;
        const i32 pts = 7;
        const std::vector<i32> t = table(n, pts);
        std::vector<f32> root(n, 0.12f), tip(n, 0.04f);
        RibbonInput in;
        in.strandStartCount2 = t.data();
        in.strandCount = n;
        in.widthRootCm = root.data();
        in.widthTipCm = tip.data();
        in.seed = 0xC0FFEEull;
        RibbonLayout a, b;
        std::string why;
        check(buildRibbonLayout(in, a, &why) && buildRibbonLayout(in, b, &why), "a plain groom lays out");
        check(a.points.size() == static_cast<usize>(n) * pts, "one ribbon point per render vertex");
        check(a.vertexCount() == a.points.size() * 2, "two ribbon vertices per point");
        check(a.indices.size() == static_cast<usize>(n) * (pts - 1) * 6, "six indices per segment");

        bool same = a.points.size() == b.points.size() && a.indices == b.indices && a.rankToStrand == b.rankToStrand;
        for (usize i = 0; same && i < a.points.size(); ++i)
            same = a.points[i].vtx == b.points[i].vtx && a.points[i].strandHash == b.points[i].strandHash && a.points[i].widthCm == b.points[i].widthCm;
        check(same, "the same input gives the same layout, bit for bit");

        in.seed = 0xC0FFEFull;
        RibbonLayout c;
        buildRibbonLayout(in, c, &why);
        check(c.rankToStrand != a.rankToStrand, "a different seed gives a different rank order");

        std::set<u32> seen(a.rankToStrand.begin(), a.rankToStrand.end());
        check(seen.size() == n && *seen.begin() == 0 && *seen.rbegin() == n - 1, "the rank order is a permutation of the strands");
        // Not the identity: a shuffle that left the order alone would make the LOD prefix a spatial slice of the head.
        u32 fixed = 0;
        for (u32 r = 0; r < n; ++r) fixed += a.rankToStrand[r] == r ? 1u : 0u;
        check(fixed < n / 4, "and it is a shuffle, not the identity (" + std::to_string(fixed) + " fixed points)");

        bool inRange = true;
        for (u32 i : a.indices) inRange = inRange && i < a.vertexCount();
        check(inRange, "every index names a ribbon vertex");

        // Points of one strand are contiguous, root first, with the neighbours the shader reads.
        bool contiguous = true;
        for (u32 r = 0; r < n && contiguous; ++r) {
            const u32 base = r * pts;
            const u32 strand = a.rankToStrand[r];
            for (i32 k = 0; k < pts; ++k) {
                const RibbonPoint& p = a.points[base + k];
                contiguous = contiguous && p.vtx == static_cast<u32>(t[strand * 2] + k) &&
                             p.prev == static_cast<u32>(t[strand * 2] + std::max(k - 1, 0)) &&
                             p.next == static_cast<u32>(t[strand * 2] + std::min(k + 1, pts - 1)) &&
                             std::fabs(p.fraction - static_cast<f32>(k) / static_cast<f32>(pts - 1)) < 1e-6f &&
                             p.strandHash == a.points[base].strandHash;
            }
        }
        check(contiguous, "a strand's points are contiguous in rank order with their root-to-tip fractions and neighbours");

        check(std::fabs(a.points.front().widthCm - 0.12f) < 1e-6f && std::fabs(a.points[pts - 1].widthCm - 0.04f) < 1e-6f, "width tapers from the root's to the tip's");
        check(a.points[0].rankFrac == 0.0f && a.points.back().rankFrac > 0.98f && a.points.back().rankFrac < 1.0f, "rankFrac runs from 0 up to just under 1");

        // The levels.
        check(a.lodStrands[0] == n && a.lodStrands[1] == 50 && a.lodStrands[2] == 25 && a.lodStrands[3] == 12, "the levels draw 100%, 50%, 25% and 12% of the strands");
        check(a.lodPoints[0] == a.points.size() && a.lodIndices[0] == a.indices.size(), "level 0 is the whole groom");
        check(a.lodPoints[1] == 50u * pts && a.lodIndices[1] == 50u * (pts - 1) * 6, "level 1 is the first 50 strands' points and indices");
        bool prefix = true;
        for (u32 l = 1; l < kRibbonLodLevels; ++l) prefix = prefix && a.lodPoints[l] <= a.lodPoints[l - 1] && a.lodIndices[l] <= a.lodIndices[l - 1];
        check(prefix, "each level is a prefix of the one before");
        // A level's indices never name a vertex past its points: the prefix really is self-contained.
        bool selfContained = true;
        for (u32 l = 0; l < kRibbonLodLevels; ++l)
            for (u32 i = 0; i < a.lodIndices[l]; ++i) selfContained = selfContained && a.indices[i] < a.lodPoints[l] * 2;
        check(selfContained, "a level's indices stay inside its own vertices");

        // Thinning is uniform: the first quarter of the ranks holds about a quarter of the strands of any region of the head. Here "region" is the strand
        // index range, which the shuffle should spread evenly.
        u32 inFirstHalfOfIds = 0;
        for (u32 r = 0; r < a.lodStrands[2]; ++r) inFirstHalfOfIds += a.rankToStrand[r] < n / 2 ? 1u : 0u;
        check(inFirstHalfOfIds > a.lodStrands[2] / 4 && inFirstHalfOfIds < a.lodStrands[2] * 3 / 4, "the thinned level takes strands from every part of the groom");

        // Refusals.
        RibbonInput bad = in;
        bad.strandCount = 0;
        check(!buildRibbonLayout(bad, c, &why) && !why.empty(), "no strands is refused with a reason");
        std::vector<i32> shortStrand = {0, 1};
        bad.strandStartCount2 = shortStrand.data();
        bad.strandCount = 1;
        check(!buildRibbonLayout(bad, c, &why), "a one-point strand is refused");
    }

    AVER_INFO("=== the basis and the uv encoding ===");
    {
        const f32 j[3] = {1.0f, 2.0f, 3.0f};   // metres, Jolt (X right, Y up, -Z forward)
        const Vec3 e = ribbonJoltToLocalCm(j);
        check(std::fabs(e.x + 300.0f) < 1e-3f && std::fabs(e.y - 100.0f) < 1e-3f && std::fabs(e.z - 200.0f) < 1e-3f,
              "Jolt (1, 2, 3) m is engine (-300, 100, 200) cm (Convert.hpp's fromJolt)");
        check(ribbonJoltToLocalCm(j).x == -3.0f * 100.0f, "forward in Jolt (-Z) is +X in the engine");

        for (u32 h : {0u, 1u, 77u, 255u}) {
            for (f32 f : {0.0f, 0.25f, 0.5f, 0.999f, 1.0f}) {
                const f32 v = ribbonEncodeV(0xABCDEF00u + h, f);
                u32 h8 = 0;
                f32 fr = 0.0f;
                ribbonDecodeV(v, h8, fr);
                check(h8 == h && std::fabs(fr - std::min(f, 1.0f)) < 2e-3f, "uv.y round trips the hash and the fraction (" + std::to_string(h) + ", " + std::to_string(f) + ")");
            }
        }
    }

    AVER_INFO("=== the ribbon point ===");
    {
        // A strand along +Z in hair space is along +Y in Jolt metres.
        auto jolt = [](f32 yMetres) { static f32 p[3]; p[0] = 0.0f; p[1] = yMetres; p[2] = 0.0f; return p; };
        RibbonPoint rp;
        rp.widthCm = 0.1f;
        rp.fraction = 0.5f;
        rp.strandHash = 0x12345678u;
        rp.rankFrac = 0.0f;

        RibbonCamera cam;
        cam.camLocal = Vec3(200.0f, 0.0f, 10.0f);   // 2 m in front of the strand (+X), level with its middle
        cam.pixelAt1cm = 2.0f * std::tan(0.5f * 1.0f) / 1080.0f;   // 1 rad of vertical field of view on a 1080 line view
        cam.widthScale = 1.0f;
        cam.minPixels = 1.0f;
        cam.frameSeed = 3;
        cam.keepRank = 1.0f;

        f32 a[3], b[3], c[3];
        std::copy(jolt(0.09f), jolt(0.09f) + 3, a);
        std::copy(jolt(0.10f), jolt(0.10f) + 3, b);
        std::copy(jolt(0.11f), jolt(0.11f) + 3, c);

        rhi::MeshVertex out[2];
        const RibbonResult r = ribbonVertices(rp, b, a, c, cam, out);
        const f32 pixelCm = 200.0f * cam.pixelAt1cm;
        check(std::fabs(r.tangent.size() - 1.0f) < 1e-5f, "the tangent is a unit vector");
        check(std::fabs(r.tangent.z - 1.0f) < 1e-4f, "and points up the strand (+Z in the engine)");
        check(std::fabs(r.side.size() - 1.0f) < 1e-5f && std::fabs(dot(r.side, r.tangent)) < 1e-4f, "the side is a unit vector perpendicular to the strand");
        check(std::fabs(dot(r.side, Vec3(1.0f, 0.0f, 0.0f))) < 1e-3f, "the side is perpendicular to the view too (the ribbon faces the camera)");

        // 0.1 cm is far thinner than a pixel at 2 m, so the ribbon is one pixel wide and carries the rest as coverage.
        check(pixelCm > rp.widthCm, "the test strand is sub-pixel at this distance");
        const f32 drawn = std::sqrt((out[1].px - out[0].px) * (out[1].px - out[0].px) + (out[1].py - out[0].py) * (out[1].py - out[0].py) +
                                    (out[1].pz - out[0].pz) * (out[1].pz - out[0].pz));
        check(r.dropped ? drawn < 1e-6f : std::fabs(drawn - pixelCm) < pixelCm * 1e-3f, "a sub-pixel strand is drawn one pixel wide (or collapsed): never in between");
        check(std::fabs(r.coverage - rp.widthCm / pixelCm) < 1e-4f, "coverage is the true width over the drawn width");

        // Close up the strand is wider than a pixel: drawn at its true width, full coverage, never dropped.
        RibbonCamera closeCam = cam;
        closeCam.camLocal = Vec3(2.0f, 0.0f, 10.0f);
        const RibbonResult rn = ribbonVertices(rp, b, a, c, closeCam, out);
        check(!rn.dropped && std::fabs(rn.coverage - 1.0f) < 1e-6f && std::fabs(rn.widthDrawn - rp.widthCm) < 1e-5f, "close up: drawn at its own width, full coverage");

        // The width scale scales the true width; the minimum pixel setting scales the floor.
        RibbonCamera wide = closeCam;
        wide.widthScale = 3.0f;
        check(std::fabs(ribbonVertices(rp, b, a, c, wide, out).widthDrawn - 0.3f) < 1e-5f, "widthScale multiplies the width");

        // The stochastic keep: over many frames the fraction kept equals the coverage.
        RibbonPoint many = rp;
        u32 kept = 0, frames = 4000;
        f32 cover = 0.0f;
        for (u32 f = 0; f < frames; ++f) {
            cam.frameSeed = f;
            many.strandHash = 0x9E3779B1u * 17u;
            const RibbonResult x = ribbonVertices(many, b, a, c, cam, out);
            cover = x.coverage;
            kept += x.dropped ? 0u : 1u;
        }
        const f32 frac = static_cast<f32>(kept) / static_cast<f32>(frames);
        check(std::fabs(frac - cover) < 0.05f, "over many frames the kept fraction matches the coverage (" + std::to_string(frac) + " vs " + std::to_string(cover) + ")");
        // And different strands decide independently.
        u32 keptByHash = 0;
        cam.frameSeed = 11;
        for (u32 h = 0; h < frames; ++h) {
            many.strandHash = ribbonMix(h + 1);
            keptByHash += ribbonVertices(many, b, a, c, cam, out).dropped ? 0u : 1u;
        }
        check(std::fabs(static_cast<f32>(keptByHash) / static_cast<f32>(frames) - cover) < 0.05f, "over many strands in one frame the kept fraction matches too");

        // The rank rule collapses a strand whatever its coverage.
        RibbonPoint late = rp;
        late.rankFrac = 0.8f;
        RibbonCamera thin = closeCam;
        thin.keepRank = 0.5f;
        const RibbonResult rd = ribbonVertices(late, b, a, c, thin, out);
        const f32 gap = std::fabs(out[1].px - out[0].px) + std::fabs(out[1].py - out[0].py) + std::fabs(out[1].pz - out[0].pz);
        check(rd.dropped && gap < 1e-6f, "a strand ranked past the kept fraction collapses to a line");

        // The normal slot carries the tangent and the uv the side and the strand code, on both vertices.
        RibbonCamera keep = closeCam;
        const RibbonResult rk = ribbonVertices(rp, b, a, c, keep, out);
        check(std::fabs(out[0].nx - rk.tangent.x) < 1e-6f && std::fabs(out[0].nz - rk.tangent.z) < 1e-6f && std::fabs(out[1].nz - rk.tangent.z) < 1e-6f,
              "the normal slot of both vertices is the tangent");
        check(out[0].u == -1.0f && out[1].u == 1.0f && out[0].v == out[1].v, "uv.x is the side (-1, +1) and uv.y the same strand code");
        u32 h8 = 0;
        f32 fr = 0.0f;
        ribbonDecodeV(out[0].v, h8, fr);
        check(h8 == (rp.strandHash & 255u) && std::fabs(fr - 0.5f) < 2e-3f, "uv.y decodes to the strand's colour code and its fraction");

        // Looking straight down the strand still gives a unit, perpendicular side (the fallback axis).
        RibbonCamera along = closeCam;
        along.camLocal = Vec3(0.0f, 0.0f, 500.0f);
        const RibbonResult ra = ribbonVertices(rp, b, a, c, along, out);
        check(std::fabs(ra.side.size() - 1.0f) < 1e-4f && std::fabs(dot(ra.side, ra.tangent)) < 1e-3f, "viewed along the strand the side falls back to a perpendicular axis");

        // A degenerate strand (the neighbours coincide) keeps a finite tangent.
        const RibbonResult rz = ribbonVertices(rp, b, b, b, closeCam, out);
        check(std::isfinite(rz.tangent.x) && std::isfinite(out[0].px) && std::isfinite(out[1].px), "coincident neighbours give finite vertices");
    }

    AVER_INFO("=== the width boost and the pixel size ===");
    {
        check(std::fabs(ribbonLodWidthBoost(1.0f) - 1.0f) < 1e-6f && std::fabs(ribbonLodWidthBoost(0.25f) - 2.0f) < 1e-5f, "a level drawing a quarter of the strands is twice as wide");
        check(ribbonLodWidthBoost(0.001f) <= std::sqrt(20.0f) + 1e-4f, "and the boost is capped");
        check(std::fabs(ribbonPixelAt1cm(2.0f * std::atan(0.5f), 1000.0f) - 0.001f) < 1e-6f, "a 90-degree-ish projection: 1 / 1000 per cm on a 1000 line view");
        check(ribbonPixelAt1cm(1.0f, 0.0f) == 0.0f, "no viewport, no footprint");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
