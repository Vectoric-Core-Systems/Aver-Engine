// PCG core types: the Aver.Pcg hash port against golden vectors generated once from the F# module, the
// Pcg.scatter reference port against F# output, point-set bookkeeping and the spatial grid.
// Exit code = failure count.
#include "../data/PcgGolden.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/pcg/PcgVolume.hpp"
#include "aver/world/PcgGraph.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace aver;
using namespace aver::world;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}
static bool near(f32 a, f32 b, f32 eps = 1e-6f) { return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b)); }

int main() {
    // ---- hashes are the F# values, bit for bit --------------------------------------------------------------------------
    {
        bool ok = true;
        for (const auto& r : pcggolden::kHash) ok &= pcgHash(r.x) == r.h;
        check(ok, "pcgHash equals Aver.Pcg.Rand.hash on every golden input");
        ok = true;
        for (const auto& r : pcggolden::kHash2) ok &= pcgHash2(r.s, r.i) == r.h;
        check(ok, "pcgHash2 equals Rand.hash2, including negative seed and index");
        ok = true;
        for (const auto& r : pcggolden::kHash3) ok &= pcgHash3(r.s, r.x, r.y, r.z) == r.h;
        check(ok, "pcgHash3 equals Rand.hash3, including negative lattice coordinates");
        ok = true;
        for (const auto& r : pcggolden::kFloat01) ok &= pcgFloat01(r.h) == r.v;
        check(ok, "pcgFloat01 equals Rand.float01 exactly (top 24 bits)");
        check(pcgRange(9, 4, -10.0f, 10.0f) == pcggolden::kRange_9_4_m10_10, "pcgRange equals Rand.range");
        check(pcgFloat01(0xFFFFFFFFu) < 1.0f, "float01 never reaches 1");
    }

    // ---- Pcg.scatter reference ------------------------------------------------------------------------------------------
    {
        PcgScatterSpec s;
        s.region.min = Vec3{-500, -300, 0};
        s.region.max = Vec3{1500, 700, 100};
        s.count = 12; s.seed = 31337; s.assetCount = 3; s.scaleMin = 0.5f; s.scaleMax = 2.0f;
        const PcgPoints p = pcgScatterReference(s);
        check(p.size() == std::size(pcggolden::kScatterFree), "free scatter returns the same number of placements as F#");
        bool pos = p.size() == std::size(pcggolden::kScatterFree), rot = pos, scl = pos, ast = pos;
        for (usize i = 0; pos && i < p.size(); ++i) {
            const auto& g = pcggolden::kScatterFree[i];
            pos &= p.pos[i].x == g.x && p.pos[i].y == g.y && p.pos[i].z == g.z;
            rot &= near(p.rot[i].z, g.qz) && near(p.rot[i].w, g.qw);
            scl &= p.scale[i].x == g.s;
            ast &= p.iAttr.at("asset")[i] == g.asset;
        }
        check(pos, "positions are bit-identical to F#");
        check(scl, "scales are bit-identical to F#");
        check(rot, "yaw quaternions match F# to 1e-6");
        check(ast, "asset choices match F#");

        PcgScatterSpec sp = s;
        sp.count = 60; sp.minSpacing = 250.0f; sp.seed = 99;
        const PcgPoints q = pcgScatterReference(sp);
        check(q.size() == std::size(pcggolden::kScatterSpaced), "min-spacing rejection keeps exactly the placements F# keeps");
        bool same = q.size() == std::size(pcggolden::kScatterSpaced);
        for (usize i = 0; same && i < q.size(); ++i)
            same &= q.pos[i].x == pcggolden::kScatterSpaced[i].x && q.pos[i].y == pcggolden::kScatterSpaced[i].y;
        check(same, "and at the same positions, in the same order");
        bool spaced = true;
        for (usize i = 0; i < q.size(); ++i)
            for (usize j = i + 1; j < q.size(); ++j) {
                const Vec3 d = q.pos[i] - q.pos[j];
                spaced &= d.x * d.x + d.y * d.y + d.z * d.z >= 250.0f * 250.0f;
            }
        check(spaced, "no two kept placements are closer than the spacing");
        check(pcgScatterReference(sp).contentHash() == q.contentHash(), "scatter is deterministic");
    }

    // ---- the C++ density mirror agrees with the F# sampleInfinite --------------------------------------------------------
    {
        pcg::InfiniteSpec spec{};
        spec.seed = 1234;
        spec.layerCount = 2;
        for (u32 i = 0; i < 2; ++i) {
            spec.layers[i].frequency = std::pow(2.0f, static_cast<f32>(i));
            spec.layers[i].amplitude = 1.0f / std::pow(2.0f, static_cast<f32>(i));
            spec.layers[i].octaves = 4;
            spec.layers[i].lacunarity = 2.0f;
            spec.layers[i].gain = 0.5f;
            spec.layers[i].seedOffset = static_cast<i32>(i + 1) * 7919;
        }
        spec.cellSizeCm = 1600.0f;
        bool ok = true;
        for (const auto& r : pcggolden::kInfinite) ok &= near(pcg::sampleInfinite(spec, r.x, r.y, r.z), r.v, 1e-5f);
        check(ok, "pcg::sampleInfinite matches Aver.Pcg.sampleInfinite on the golden samples");
    }

    // ---- PcgPoints bookkeeping ------------------------------------------------------------------------------------------
    {
        PcgPoints a;
        PcgPoint p;
        p.pos = Vec3{1, 2, 3}; p.seed = 7; p.density = 0.25f;
        a.push(p);
        a.setScalar("moisture", 0, 0.5f);
        p.pos = Vec3{4, 5, 6}; p.seed = 8;
        a.push(p);
        check(a.size() == 2 && a.fAttr.at("moisture").size() == 2, "a named attribute column grows with the point set");
        check(a.scalar("moisture", 0) == 0.5f && a.scalar("moisture", 1) == 0.0f, "and a new point gets zero for it");
        check(a.scalar("density", 0) == 0.25f && a.scalar("seed", 1) == 8.0f, "core columns answer to their names");

        PcgPoints b;
        b.appendFrom(a, 0);
        PcgPoint q; q.pos = Vec3{9, 9, 9};
        b.push(q);
        check(b.size() == 2 && b.fAttr.at("moisture")[0] == 0.5f && b.fAttr.at("moisture")[1] == 0.0f,
              "appendFrom carries attributes and zero-fills the rest");
        PcgPoints c;
        c.push(q);
        c.appendFrom(a, 0);
        check(c.fAttr.at("moisture").size() == 2 && c.fAttr.at("moisture")[0] == 0.0f && c.fAttr.at("moisture")[1] == 0.5f,
              "a column that appears later is back-filled for the points already there");
        const PcgPoints sel = a.select({1});
        check(sel.size() == 1 && sel.pos[0].x == 4.0f && sel.fAttr.count("moisture"), "select keeps order and schema");
        check(a.contentHash() == a.contentHash() && a.contentHash() != sel.contentHash(), "contentHash tells different sets apart");
        PcgPoints a2 = a;
        a2.pos[0].x += 1.0f;
        check(a2.contentHash() != a.contentHash(), "and notices a one-float change");
    }

    // ---- spatial grid ---------------------------------------------------------------------------------------------------
    {
        std::vector<Vec3> pts;
        for (int i = 0; i < 400; ++i) pts.push_back(Vec3{pcgRange(1, i * 3, -1000, 1000), pcgRange(1, i * 3 + 1, -1000, 1000), pcgRange(1, i * 3 + 2, -50, 50)});
        PcgSpatialGrid grid(120.0f);
        grid.build(pts);
        bool exact = true;
        for (int q = 0; q < 25; ++q) {
            const Vec3 c = pts[static_cast<usize>(q * 13)];
            std::set<u32> viaGrid, brute;
            grid.forEachNear(c, 150.0f, [&](u32 i) {
                const Vec3 d = pts[i] - c;
                if (d.x * d.x + d.y * d.y + d.z * d.z <= 150.0f * 150.0f) viaGrid.insert(i);
            });
            for (u32 i = 0; i < pts.size(); ++i) {
                const Vec3 d = pts[i] - c;
                if (d.x * d.x + d.y * d.y + d.z * d.z <= 150.0f * 150.0f) brute.insert(i);
            }
            exact &= viaGrid == brute;
        }
        check(exact, "grid radius queries find exactly the brute-force neighbours (negative coordinates included)");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
