// The procedural groom (HairGenerate.hpp): roots on the scalp, minimum spacing, the length range, the direction of growth, the fall towards a style
// direction, clump pull, the mask, the part, determinism and the refusals. No physics, no scene, no device. Exit code = failure count.
//
// NOTHING IN HERE HAS BEEN RUN: the thresholds are starting values for the first pass; a failing one is a question about the number first.
#include "aver/hair/HairGenerate.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

// A flat n x n grid of quads (two triangles each), side `size` cm, on the z = 0 plane, centred on the origin.
static hair::ScalpMesh makeGrid(int n, float size) {
    hair::ScalpMesh s;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            s.positions.push_back((static_cast<float>(i) / static_cast<float>(n) - 0.5f) * size);
            s.positions.push_back((static_cast<float>(j) / static_cast<float>(n) - 0.5f) * size);
            s.positions.push_back(0.0f);
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const u32 a = static_cast<u32>(j * (n + 1) + i), b = a + 1, c = a + static_cast<u32>(n + 1), d = c + 1;
            s.triangles.insert(s.triangles.end(), {a, b, d, a, d, c});
        }
    return s;
}

// A UV sphere of radius r, outward winding by construction irrelevant: the generator orients normals away from the centre.
static hair::ScalpMesh makeSphere(float r, int rings, int segs) {
    hair::ScalpMesh s;
    s.positions = {0, 0, r};
    for (int k = 1; k < rings; ++k) {
        const float phi = kPi * static_cast<float>(k) / static_cast<float>(rings);
        for (int m = 0; m < segs; ++m) {
            const float th = kTwoPi * static_cast<float>(m) / static_cast<float>(segs);
            s.positions.insert(s.positions.end(), {r * std::sin(phi) * std::cos(th), r * std::sin(phi) * std::sin(th), r * std::cos(phi)});
        }
    }
    const u32 south = static_cast<u32>(s.positions.size() / 3);
    s.positions.insert(s.positions.end(), {0, 0, -r});
    auto ring = [&](int k, int m) { return static_cast<u32>(1 + (k - 1) * segs + (m % segs)); };
    for (int m = 0; m < segs; ++m) s.triangles.insert(s.triangles.end(), {0, ring(1, m), ring(1, m + 1)});
    for (int k = 1; k + 1 < rings; ++k)
        for (int m = 0; m < segs; ++m) {
            s.triangles.insert(s.triangles.end(), {ring(k, m), ring(k + 1, m), ring(k + 1, m + 1)});
            s.triangles.insert(s.triangles.end(), {ring(k, m), ring(k + 1, m + 1), ring(k, m + 1)});
        }
    for (int m = 0; m < segs; ++m) s.triangles.insert(s.triangles.end(), {south, ring(rings - 1, m + 1), ring(rings - 1, m)});
    return s;
}

static fmt::OcHairGenerate plain(u32 strands) {
    fmt::OcHairGenerate g;
    g.present = true;
    g.seed = 99;
    g.strands = strands;
    g.lengthCm[0] = 8.0f; g.lengthCm[1] = 12.0f;
    g.segments = 8;
    g.curlCm = 0.0f;
    g.clumps = 0;
    g.partWidthCm = 0.0f;
    g.styleWeight = 0.0f;
    return g;
}

static Vec3 pointOf(const fmt::OcHairStrands& s, u32 strand, u32 k) {
    const usize i = (static_cast<usize>(s.start[strand]) + k) * 3;
    return Vec3(s.points[i], s.points[i + 1], s.points[i + 2]);
}

static float strandLength(const fmt::OcHairStrands& s, u32 strand) {
    float len = 0.0f;
    for (u32 k = 0; k + 1 < s.start[strand + 1] - s.start[strand]; ++k) len += dist(pointOf(s, strand, k), pointOf(s, strand, k + 1));
    return len;
}

static u64 hashOf(const fmt::OcHairStrands& s) {
    return fnv1a64(s.points.data(), s.points.size() * sizeof(f32)) ^ (fnv1a64(s.start.data(), s.start.size() * sizeof(u32)) * 31u);
}

int main() {
    const hair::ScalpMesh grid = makeGrid(4, 10.0f);

    AVER_INFO("=== roots, spacing, shape ===");
    {
        fmt::OcHairStrands s;
        hair::GenerateStats st;
        std::string why;
        check(hair::generateHair(plain(200), grid, s, &st, &why), "200 strands generate (" + why + ")");
        check(s.strandCount() == 200 && s.pointCount() == 200 * 9, "200 strands of 9 points");
        check(s.start.front() == 0 && s.start.back() == s.pointCount(), "the start table covers the points");
        check(s.material.size() == 200 && s.width.empty(), "a material per strand and no width table");

        bool onScalp = true, inside = true;
        for (u32 i = 0; i < 200; ++i) {
            const Vec3 r = pointOf(s, i, 0);
            if (r.z != 0.0f) onScalp = false;
            if (std::fabs(r.x) > 5.0f + 1e-4f || std::fabs(r.y) > 5.0f + 1e-4f) inside = false;
        }
        check(onScalp && inside, "every root lies on the scalp (z = 0, inside the 10 cm square)");

        float nearest = 1.0e9f;
        for (u32 i = 0; i < 200; ++i)
            for (u32 j = i + 1; j < 200; ++j) nearest = std::min(nearest, dist(pointOf(s, i, 0), pointOf(s, j, 0)));
        AVER_INFO("  spacing used {:.3f} cm after {} relax rounds, nearest roots {:.3f} cm", st.spacingCm, st.relaxRounds, nearest);
        check(st.spacingCm == 0.0f || nearest >= st.spacingCm * 0.999f, "no two roots are closer than the spacing the generator used");
        check(st.scalpAreaCm2 > 99.0f && st.scalpAreaCm2 < 101.0f, "the reported area is the scalp's 100 cm^2");

        // Without style, curl or part a strand is a straight line along the normal: z only, length in the asked range.
        bool straight = true, lengthOk = true;
        float shortest = 1.0e9f, longest = 0.0f;
        for (u32 i = 0; i < 200; ++i) {
            const Vec3 a = pointOf(s, i, 0), b = pointOf(s, i, 8);
            if (std::fabs(a.x - b.x) > 1e-4f || std::fabs(a.y - b.y) > 1e-4f) straight = false;
            const float len = strandLength(s, i);
            shortest = std::min(shortest, len);
            longest = std::max(longest, len);
            if (len < 8.0f - 1e-3f || len > 12.0f + 1e-3f) lengthOk = false;
        }
        check(straight, "with no style, curl or part every strand is straight along the scalp normal");
        check(lengthOk, "every strand length is inside lengthCm");
        check(longest - shortest > 1.0f, "and the lengths actually vary across the range (" + std::to_string(shortest) + " to " + std::to_string(longest) + ")");
    }

    AVER_INFO("=== determinism ===");
    {
        fmt::OcHairStrands a, b, c;
        fmt::OcHairGenerate g = plain(300);
        g.curlCm = 0.8f;
        g.clumps = 12;
        g.clumpRadiusCm = 2.0f;
        g.partWidthCm = 1.0f;
        g.styleWeight = 0.5f;
        std::string why;
        check(hair::generateHair(g, grid, a, nullptr, &why) && hair::generateHair(g, grid, b, nullptr, &why), "two runs generate");
        check(a.points.size() == b.points.size() && std::memcmp(a.points.data(), b.points.data(), a.points.size() * sizeof(f32)) == 0 && a.start == b.start,
              "the same seed gives the same bytes");
        g.seed = 100;
        hair::generateHair(g, grid, c, nullptr, &why);
        check(hashOf(a) != hashOf(c), "another seed gives another groom");
    }

    AVER_INFO("=== fall towards the style direction, on a head-shaped scalp ===");
    {
        const hair::ScalpMesh head = makeSphere(9.0f, 16, 24);
        fmt::OcHairGenerate g = plain(400);
        g.lengthCm[0] = 20.0f; g.lengthCm[1] = 24.0f;
        g.segments = 10;
        g.style[0] = 0; g.style[1] = 0; g.style[2] = -1;
        g.styleWeight = 1.0f;
        fmt::OcHairStrands s;
        std::string why;
        check(hair::generateHair(g, head, s, nullptr, &why), "400 strands on a sphere generate (" + why + ")");
        int outward = 0, lower = 0, onSphere = 0;
        float meanDrop = 0.0f;
        for (u32 i = 0; i < 400; ++i) {
            const Vec3 root = pointOf(s, i, 0), p1 = pointOf(s, i, 1), tip = pointOf(s, i, 10);
            if (std::fabs(root.size() - 9.0f) < 0.25f) ++onSphere;      // the sphere is a polyhedron: roots sit a little inside its radius
            if (dot((p1 - root).getSafeNormal(), root.getSafeNormal()) > 0.9f) ++outward;
            if (tip.z < root.z) ++lower;
            meanDrop += tip.z - root.z;
        }
        meanDrop /= 400.0f;
        AVER_INFO("  mean tip drop {:.2f} cm, {} of 400 tips below their root", meanDrop, lower);
        check(onSphere >= 395, "roots are on the scalp surface");
        check(outward >= 390, "the first segment of a strand points away from the middle of the head");
        check(lower >= 340 && meanDrop < -3.0f, "with styleWeight 1 and a downward style the tips hang below the roots (a strand at the crown only levels off)");
    }

    AVER_INFO("=== clumps ===");
    {
        fmt::OcHairGenerate g = plain(150);
        g.clumps = 6;
        g.clumpRadiusCm = 3.0f;
        g.clumpTightness = 1.0f;
        fmt::OcHairStrands s;
        hair::GenerateStats st;
        std::string why;
        check(hair::generateHair(g, grid, s, &st, &why), "a clumped groom generates (" + why + ")");
        check(st.guides == 6 && st.followers > 20, "6 guides drew " + std::to_string(st.followers) + " followers");
        // Tightness 1 sends a follower's tip onto its guide's tip, so tips coincide.
        u32 sharing = 0;
        for (u32 i = 0; i < 150; ++i) {
            const Vec3 ti = pointOf(s, i, 8);
            for (u32 j = 0; j < 150; ++j)
                if (j != i && dist(ti, pointOf(s, j, 8)) < 1.0e-3f) { ++sharing; break; }
        }
        check(sharing >= st.followers, "every follower's tip meets another strand's tip");
        bool rootsKept = true;
        fmt::OcHairStrands plainStrands;
        fmt::OcHairGenerate g0 = g;
        g0.clumps = 0;
        hair::generateHair(g0, grid, plainStrands, nullptr, &why);
        for (u32 i = 0; i < 150; ++i)
            if (dist(pointOf(s, i, 0), pointOf(plainStrands, i, 0)) > 1.0e-5f) rootsKept = false;
        check(rootsKept, "clumping never moves a root");
    }

    AVER_INFO("=== the mask ===");
    {
        hair::ScalpMesh masked = makeGrid(8, 10.0f);
        masked.mask.resize(masked.vertexCount());
        for (usize v = 0; v < masked.vertexCount(); ++v) masked.mask[v] = masked.positions[v * 3] < 0.0f ? 0.0f : 1.0f;
        fmt::OcHairStrands s;
        hair::GenerateStats st;
        std::string why;
        check(hair::generateHair(plain(200), masked, s, &st, &why), "a half-masked scalp still places all roots (" + why + ")");
        u32 left = 0, farLeft = 0;
        for (u32 i = 0; i < 200; ++i) {
            const float x = pointOf(s, i, 0).x;
            if (x < 0.0f) ++left;
            if (x < -1.26f) ++farLeft;      // one cell of the 8 x 8 grid is 1.25 cm
        }
        AVER_INFO("  {} of 200 roots left of the mask edge, {} of them more than a cell away", left, farLeft);
        check(farLeft == 0, "no root grows where the mask is zero over a whole triangle");
        check(left < 45, "and the mask edge is crisp: few roots spill across it");
        check(st.scalpAreaCm2 > 49.0f && st.scalpAreaCm2 < 58.0f, "the weighted area is about half the scalp");

        hair::ScalpMesh none = makeGrid(2, 10.0f);
        none.mask.assign(none.vertexCount(), 0.0f);
        check(!hair::generateHair(plain(10), none, s, nullptr, &why) && why.find("no area") != std::string::npos, "an all-zero mask is refused, and says why");
    }

    AVER_INFO("=== the part ===");
    {
        fmt::OcHairGenerate g = plain(300);
        g.partDirection[0] = 0; g.partDirection[1] = 1; g.partDirection[2] = 0;
        g.partWidthCm = 1.5f;
        fmt::OcHairStrands s;
        std::string why;
        check(hair::generateHair(g, grid, s, nullptr, &why), "a parted groom generates (" + why + ")");
        int wrong = 0, counted = 0;
        for (u32 i = 0; i < 300; ++i) {
            const Vec3 root = pointOf(s, i, 0), p1 = pointOf(s, i, 1);
            if (std::fabs(root.x) < 1.5f) continue;
            ++counted;
            const float lateral = p1.x - root.x;
            if (lateral * root.x <= 0.0f) ++wrong;      // pushed away from the part line (x = 0)
        }
        check(counted > 100 && wrong == 0, "roots away from the part line lean away from it (" + std::to_string(counted) + " checked)");
    }

    AVER_INFO("=== a small scalp, and the refusals ===");
    {
        const hair::ScalpMesh tiny = makeGrid(1, 1.0f);
        fmt::OcHairStrands s;
        hair::GenerateStats st;
        std::string why;
        check(hair::generateHair(plain(500), tiny, s, &st, &why) && s.strandCount() == 500, "500 strands on 1 cm^2 still place (" + why + ")");

        check(!hair::generateHair(plain(0), grid, s, nullptr, &why), "0 strands is refused");
        fmt::OcHairGenerate bad = plain(10);
        bad.segments = 1;
        check(!hair::generateHair(bad, grid, s, nullptr, &why), "1 segment is refused");
        bad = plain(10);
        bad.lengthCm[0] = 5.0f; bad.lengthCm[1] = 4.0f;
        check(!hair::generateHair(bad, grid, s, nullptr, &why), "a length range that runs backwards is refused");
        hair::ScalpMesh empty;
        check(!hair::generateHair(plain(10), empty, s, nullptr, &why), "an empty scalp is refused");
        hair::ScalpMesh broken = grid;
        broken.triangles[0] = 9999;
        check(!hair::generateHair(plain(10), broken, s, nullptr, &why), "a triangle past the vertices is refused");
        check(hair::scalpArea(grid) > 99.0f && hair::scalpArea(empty) == 0.0f, "scalpArea");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
