// Ropes (Rope.hpp): the planned rest curve, the tube drawn around the nodes (radius, rings, twist from the rods, winding,
// closedness, UVs), and the wave 11 .ocsoft sections that describe ropes, jellies and inflatables (SoftAsset.hpp). Pure
// arithmetic and text; no GPU, no scene, no Jolt.
//
// NOTHING IN HERE HAS BEEN RUN. The tolerances are starting values for the first headed pass.
#include "aver/core/Log.hpp"
#include "aver/softbody/Rope.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static f32 maxSpacingError(const sb::RopeBuild& b) {
    f32 worst = 0.0f;
    for (u32 i = 0; i + 1 < b.nodes; ++i) worst = std::max(worst, std::fabs(dist(b.rest[i], b.rest[i + 1]) - b.segmentCm) / std::max(b.segmentCm, 1e-4f));
    return worst;
}

int main() {
    AVER_INFO("RopeTest");

    AVER_INFO("planRope");
    {
        sb::RopeSpec spec;
        spec.segmentCm = 10.0f;
        sb::RopeBuild b;

        // Taut: a length equal to the distance is a straight line.
        check(sb::planRope(Vec3{0, 0, 100}, Vec3{200, 0, 100}, [&] { sb::RopeSpec s = spec; s.lengthCm = 200.0f; return s; }(), b), "a taut rope plans");
        check(b.nodes == 21 && b.rest.size() == 21 && b.invMass.size() == 21, "200 cm in 10 cm segments is 21 nodes");
        check(dist(b.rest.front(), Vec3{0, 0, 100}) < 1e-3f && dist(b.rest.back(), Vec3{200, 0, 100}) < 1e-3f, "the first and last node sit on the two ends");
        f32 worstZ = 0.0f;
        for (const Vec3& p : b.rest) worstZ = std::max(worstZ, std::fabs(p.z - 100.0f));
        check(worstZ < 0.05f, "and nothing sags");
        check(std::fabs(b.restLengthCm - 200.0f) < 0.5f, "the polyline is 200 cm long");

        // Slack: 10% longer than the distance sags and keeps its arc length.
        sb::RopeSpec slack = spec;
        slack.lengthCm = 220.0f;
        check(sb::planRope(Vec3{0, 0, 100}, Vec3{200, 0, 100}, slack, b), "a slack rope plans");
        check(std::fabs(b.restLengthCm - 220.0f) < 220.0f * 0.01f, "its polyline is its length within 1%");
        f32 lowest = 100.0f;
        for (const Vec3& p : b.rest) lowest = std::min(lowest, p.z);
        check(lowest < 100.0f - 20.0f, "it sags well below the ends");
        check(maxSpacingError(b) < 0.03f, "the nodes are evenly spaced (within 3%)");
        check(b.nodeMassKg > 0.0f && std::fabs(b.invMass[0] * b.nodeMassKg - 1.0f) < 1e-4f, "every node has the inverse of its mass");
        check(std::fabs(b.nodeMassKg - slack.massPerMeterKg * (220.0f / static_cast<f32>(b.nodes - 1)) * 0.01f) < 1e-5f, "a node's mass is the rope's mass per metre over its segment");

        // No length: the distance times the slack factor.
        sb::RopeSpec byFactor = spec;
        byFactor.slack = 1.05f;
        check(sb::planRope(Vec3{0, 0, 0}, Vec3{300, 0, 0}, byFactor, b) && std::fabs(b.restLengthCm - 315.0f) < 3.2f, "no length means distance x slack");

        // A vertical chord still sags sideways (its drop has no horizontal component to hang from).
        check(sb::planRope(Vec3{0, 0, 300}, Vec3{0, 0, 0}, [&] { sb::RopeSpec s = spec; s.lengthCm = 330.0f; return s; }(), b), "a vertical rope plans");
        f32 off = 0.0f;
        for (const Vec3& p : b.rest) off = std::max(off, std::sqrt(p.x * p.x + p.y * p.y));
        check(off > 10.0f && std::fabs(b.restLengthCm - 330.0f) < 330.0f * 0.01f, "it bows out sideways and keeps its length");

        // Both ends on one point: a straight drop.
        check(sb::planRope(Vec3{10, 20, 300}, Vec3{10, 20, 300}, [&] { sb::RopeSpec s = spec; s.lengthCm = 100.0f; return s; }(), b), "a rope with both ends on a point plans");
        check(dist(b.rest.back(), Vec3{10, 20, 200}) < 0.5f, "and hangs straight down from it");

        // At least three nodes whatever the segment length.
        sb::RopeSpec coarse = spec;
        coarse.segmentCm = 1000.0f;
        check(sb::planRope(Vec3{0, 0, 0}, Vec3{50, 0, 0}, coarse, b) && b.nodes >= 3, "a rope shorter than a segment still has 3 nodes (Jolt's chain needs a bend-twist pair)");

        // Refusals.
        check(!sb::planRope(Vec3{0, 0, 0}, Vec3{std::nanf(""), 0, 0}, spec, b) && !b.error.empty(), "a non-finite end is refused");
        sb::RopeSpec capped = spec;
        capped.maxNodes = 10;
        check(!sb::planRope(Vec3{0, 0, 0}, Vec3{1000, 0, 0}, capped, b) && b.error.find("over the limit") != std::string::npos, "more nodes than the cap is refused as over the limit");
        sb::RopeSpec bad = spec;
        bad.segmentCm = 0.0f;
        check(!sb::planRope(Vec3{0, 0, 0}, Vec3{100, 0, 0}, bad, b), "a zero segment is refused");
        check(!sb::planRope(Vec3{5, 5, 5}, Vec3{5, 5, 5}, spec, b), "a rope of no length is refused");

        // Determinism.
        sb::RopeBuild b1, b2;
        sb::RopeSpec s2 = spec;
        s2.lengthCm = 250.0f;
        sb::planRope(Vec3{0, 0, 100}, Vec3{200, 40, 120}, s2, b1);
        sb::planRope(Vec3{0, 0, 100}, Vec3{200, 40, 120}, s2, b2);
        bool same = b1.nodes == b2.nodes;
        for (u32 i = 0; same && i < b1.nodes; ++i) same = b1.rest[i].x == b2.rest[i].x && b1.rest[i].y == b2.rest[i].y && b1.rest[i].z == b2.rest[i].z;
        check(same, "planning twice gives the same nodes bit for bit");
    }

    AVER_INFO("the tube");
    {
        const u32 n = 6, sides = 8;
        std::vector<Vec3> nodes;
        for (u32 i = 0; i < n; ++i) nodes.push_back(Vec3{static_cast<f32>(i) * 10.0f, 0, 50});
        std::vector<Vec3> ringPos(sb::tubeVertexCount(n, sides));
        check(sb::tubeVertexCount(n, sides) == n * (sides + 1), "a ring has sides + 1 vertices (the last repeats the first so the texture wraps)");
        sb::tubeVertices(nodes.data(), n, nullptr, 2.0f, sides, ringPos.data());

        f32 worstRadius = 0.0f, worstPerp = 0.0f, worstSeam = 0.0f;
        for (u32 i = 0; i < n; ++i)
            for (u32 k = 0; k <= sides; ++k) {
                const Vec3 d = ringPos[i * (sides + 1) + k] - nodes[i];
                worstRadius = std::max(worstRadius, std::fabs(d.size() - 2.0f));
                worstPerp = std::max(worstPerp, std::fabs(dot(d, Vec3{1, 0, 0})));
            }
        for (u32 i = 0; i < n; ++i) worstSeam = std::max(worstSeam, dist(ringPos[i * (sides + 1)], ringPos[i * (sides + 1) + sides]));
        check(worstRadius < 1e-3f, "every ring vertex is the radius from its node");
        check(worstPerp < 1e-3f, "and every ring is perpendicular to the rope");
        check(worstSeam < 1e-4f, "the last vertex of a ring repeats the first");

        // The twist follows the rods: a rotation about the rope's axis turns the ring by the same angle.
        const f32 theta = 0.7f;
        std::vector<Quat> rods(n - 1, Quat::fromAxisAngle(Vec3{1, 0, 0}, theta));
        std::vector<Vec3> twisted(ringPos.size());
        sb::tubeVertices(nodes.data(), n, rods.data(), 2.0f, sides, twisted.data());
        const Vec3 v0 = twisted[2 * (sides + 1)] - nodes[2];
        check(dist(v0, Vec3{0, 2.0f * std::cos(theta), 2.0f * std::sin(theta)}) < 1e-3f, "a rod rotated 0.7 rad about the axis puts ring vertex 0 at that angle");
        std::vector<Quat> still(n - 1, Quat::identity());
        std::vector<Vec3> flat(ringPos.size());
        sb::tubeVertices(nodes.data(), n, still.data(), 2.0f, sides, flat.data());
        const Vec3 f0 = flat[2 * (sides + 1)] - nodes[2];
        check(std::fabs(f0.y - 2.0f) < 1e-3f && std::fabs(f0.z) < 1e-3f, "with identity rods ring vertex 0 lies along +Y");

        // Bent rope: rings stay perpendicular to the local tangent.
        std::vector<Vec3> bent;
        for (u32 i = 0; i < n; ++i) {
            const f32 a = 0.25f * static_cast<f32>(i);
            bent.push_back(Vec3{30.0f * std::sin(a), 30.0f * (1.0f - std::cos(a)), 50.0f});
        }
        std::vector<Vec3> bentRing(sb::tubeVertexCount(n, sides));
        sb::tubeVertices(bent.data(), n, nullptr, 1.5f, sides, bentRing.data());
        f32 worstBent = 0.0f;
        for (u32 i = 1; i + 1 < n; ++i) {
            const Vec3 t = (bent[i + 1] - bent[i - 1]).getSafeNormal();
            for (u32 k = 0; k < sides; ++k) worstBent = std::max(worstBent, std::fabs(dot(bentRing[i * (sides + 1) + k] - bent[i], t)));
        }
        check(worstBent < 1e-3f, "on a bend the rings are perpendicular to the local tangent");

        // Winding and closedness.
        std::vector<u32> idx;
        sb::tubeIndices(n, sides, idx);
        check(idx.size() == (n - 1) * sides * 6, "two triangles per quad");
        bool inRange = true;
        for (const u32 i : idx) inRange = inRange && i < ringPos.size();
        check(inRange, "every index is a tube vertex");
        u32 inward = 0;
        for (usize t = 0; t + 2 < idx.size(); t += 3) {
            const Vec3 a = ringPos[idx[t]], b = ringPos[idx[t + 1]], c = ringPos[idx[t + 2]];
            const u32 seg = idx[t] / (sides + 1);
            const Vec3 axis = (nodes[seg] + nodes[seg + 1]) * 0.5f;
            const Vec3 centroid = (a + b + c) * (1.0f / 3.0f);
            inward += dot(cross(b - a, c - a), centroid - axis) <= 0.0f ? 1u : 0u;
        }
        check(inward == 0, "cross(B - A, C - A) points out of the tube (the render convention)");

        // Welding the seam, every edge is shared twice except the two open end rings.
        std::map<std::pair<u32, u32>, u32> uses;
        const auto weld = [&](u32 i) { return (i / (sides + 1)) * sides + (i % (sides + 1)) % sides; };
        for (usize t = 0; t + 2 < idx.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                const u32 a = weld(idx[t + static_cast<usize>(e)]), b = weld(idx[t + static_cast<usize>((e + 1) % 3)]);
                ++uses[{std::min(a, b), std::max(a, b)}];
            }
        u32 open = 0, over = 0;
        for (const auto& kv : uses) { open += kv.second == 1; over += kv.second > 2; }
        check(over == 0 && open == 2 * sides, "the tube is a manifold with exactly the two end rings open");

        // UVs.
        std::vector<f32> uv(ringPos.size() * 2);
        sb::tubeUvs(nodes.data(), n, sides, 20.0f, uv.data());
        check(std::fabs(uv[0]) < 1e-6f && std::fabs(uv[(sides + 1) * 2 * (n - 1)] - 50.0f / 20.0f) < 1e-4f, "u runs from 0 to the arc length in tiles");
        check(std::fabs(uv[1]) < 1e-6f && std::fabs(uv[sides * 2 + 1] - 1.0f) < 1e-6f, "v runs from 0 to 1 around a ring, the repeated vertex at 1");
        bool mono = true;
        for (u32 i = 1; i < n; ++i) mono = mono && uv[i * (sides + 1) * 2] > uv[(i - 1) * (sides + 1) * 2];
        check(mono, "u increases along the rope");

        check(sb::nearestNode(nodes.data(), n, Vec3{31, 5, 50}) == 3 && sb::nearestNode(nodes.data(), 0, Vec3{0, 0, 0}) == 0, "nearestNode finds the closest node");
        check(std::fabs(sb::polylineLength(nodes.data(), n) - 50.0f) < 1e-4f, "polylineLength sums the segments");
    }

    AVER_INFO("the wave 11 .ocsoft sections");
    {
        // An older asset writes exactly as it always did: none of the new keys appear.
        const sb::SoftAsset plain = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
        const std::string text = sb::writeSoftAsset(plain);
        check(text.find("solver") == std::string::npos && text.find("inflate") == std::string::npos && text.find("jelly\"") == std::string::npos &&
                  text.find("\"rope\"") == std::string::npos && text.find("\"group\"") == std::string::npos,
              "a default cloth writes none of the new keys (its saved-damage key does not move)");
        const sb::SoftAsset glass = sb::makeSoftAsset(sb::SoftKind::Destructible, sb::SoftPreset::Glass);
        check(sb::writeSoftAsset(glass).find("inflate") == std::string::npos, "nor does a default destructible");

        std::string why;
        sb::SoftAsset a;
        const char* inflatable = R"({"format":"ocsoft","version":1,"kind":"cloth","preset":"cloth_light",
            "cloth":{"solver":"fast","bend":"dihedral","lra":"geodesic","lraSlack":1.05},
            "inflate":{"pressureKPa":3.0,"leakKPaPerS":0.5},"group":{"ragdollOf":"owner"}})";
        check(sb::parseSoftAsset(inflatable, a, &why), "a cloth with a fast solver, inflate and group parses (" + why + ")");
        check(a.cloth.solver == sb::ClothSolver::Fast && a.cloth.bend == sb::ClothBend::Dihedral && a.cloth.lra == sb::ClothLra::Geodesic &&
                  std::fabs(a.cloth.lraSlack - 1.05f) < 1e-6f,
              "the solver, bend, lra and slack are read");
        check(std::fabs(a.inflate.pressureKPa - 3.0f) < 1e-6f && std::fabs(a.inflate.leakKPaPerS - 0.5f) < 1e-6f && a.group.ragdollOwner, "inflate and group are read");
        const std::string once = sb::writeSoftAsset(a);
        sb::SoftAsset b;
        check(sb::parseSoftAsset(once, b, &why) && sb::writeSoftAsset(b) == once, "write -> parse -> write is byte identical with the new cloth keys");
        check(sb::softAssetKey(a) != sb::softAssetKey(plain), "and they move the saved-damage key");

        sb::SoftAsset jelly;
        const char* jellyText = R"({"format":"ocsoft","version":1,"kind":"jelly","preset":"jelly",
            "jelly":{"cellCm":10,"snapCm":3,"volumeCompliance":2e-4,"edgeCompliance":5e-5,"vertexRadiusCm":3.5,"maxNodes":2048}})";
        check(sb::parseSoftAsset(jellyText, jelly, &why), "a jelly asset parses (" + why + ")");
        check(jelly.kind == sb::SoftKind::Jelly && jelly.preset == sb::SoftPreset::Jelly && std::fabs(jelly.jelly.vertexRadiusCm - 3.5f) < 1e-6f &&
                  jelly.jelly.maxNodes == 2048,
              "kind, preset and the jelly section");
        const std::string jOnce = sb::writeSoftAsset(jelly);
        sb::SoftAsset jAgain;
        check(sb::parseSoftAsset(jOnce, jAgain, &why) && sb::writeSoftAsset(jAgain) == jOnce, "a jelly writes and re-writes byte identical");
        sb::SoftAsset jDefault;
        check(sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"kind":"jelly"})", jDefault, &why) && jDefault.preset == sb::SoftPreset::Jelly &&
                  std::fabs(sb::jellyVertexRadiusCm(jDefault.jelly, 10.0f) - 3.5f) < 1e-5f,
              "a minimal jelly resolves to the jelly preset, its vertex radius 0.35 of the cell");

        sb::SoftAsset rope;
        const char* ropeText = R"({"format":"ocsoft","version":1,"kind":"rope","preset":"rope",
            "rope":{"radiusCm":1.2,"segmentCm":12,"massPerMeterKg":0.4,"slack":1.02,"sides":8,"uvCmPerTile":20,
                    "stretchCompliance":1e-8,"bendTwistCompliance":1e-6,"material":"M_Rope"}})";
        check(sb::parseSoftAsset(ropeText, rope, &why), "a rope asset parses (" + why + ")");
        check(rope.kind == sb::SoftKind::Rope && rope.preset == sb::SoftPreset::Rope && rope.rope.material == "M_Rope" && rope.rope.sides == 8, "kind, preset and the rope section");
        const std::string rOnce = sb::writeSoftAsset(rope);
        sb::SoftAsset rAgain;
        check(sb::parseSoftAsset(rOnce, rAgain, &why) && sb::writeSoftAsset(rAgain) == rOnce, "a rope writes and re-writes byte identical");

        // Every preset, every kind, still round-trips.
        bool all = true;
        for (u32 k = 0; k < 4; ++k)
            for (u32 p = 0; p < sb::kSoftPresetCount; ++p) {
                sb::SoftAsset x = sb::makeSoftAsset(static_cast<sb::SoftKind>(k), static_cast<sb::SoftPreset>(p));
                const std::string w = sb::writeSoftAsset(x);
                sb::SoftAsset y;
                all = all && sb::parseSoftAsset(w, y, &why) && sb::writeSoftAsset(y) == w;
            }
        check(all, "all four kinds with all seven presets round trip byte identical");
        sb::SoftPreset pr = sb::SoftPreset::ClothLight;
        check(sb::presetFromKey("jelly", pr) && pr == sb::SoftPreset::Jelly && sb::presetFromKey("rope", pr) && pr == sb::SoftPreset::Rope,
              "jelly and rope are preset keys");

        // Refusals.
        sb::SoftAsset v = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
        v.cloth.solver = sb::ClothSolver::Fast;
        v.cloth.tearable = true;
        check(!v.valid(&why), "a tearable fast cloth is refused");
        v.cloth.tearable = false;
        v.cloth.lraSlack = 0.9f;
        check(!v.valid(&why), "an lraSlack under 1 is refused");
        v = sb::makeSoftAsset(sb::SoftKind::Cloth, sb::SoftPreset::ClothLight);
        v.inflate.pressureKPa = -1.0f;
        check(!v.valid(&why), "a negative pressure is refused");
        sb::SoftAsset j2 = sb::makeSoftAsset(sb::SoftKind::Jelly, sb::SoftPreset::Jelly);
        j2.jelly.cellCm = 0.0f;
        check(!j2.valid(&why), "a zero jelly cell is refused");
        j2 = sb::makeSoftAsset(sb::SoftKind::Jelly, sb::SoftPreset::Jelly);
        j2.jelly.maxNodes = 2;
        check(!j2.valid(&why), "a jelly node cap under 4 is refused");
        sb::SoftAsset r2 = sb::makeSoftAsset(sb::SoftKind::Rope, sb::SoftPreset::Rope);
        r2.rope.sides = 2;
        check(!r2.valid(&why), "a tube with 2 sides is refused");
        r2 = sb::makeSoftAsset(sb::SoftKind::Rope, sb::SoftPreset::Rope);
        r2.rope.slack = 0.5f;
        check(!r2.valid(&why), "a rope slack under 1 is refused");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"cloth":{"solver":"turbo"}})", a, &why), "an unknown solver is refused");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"cloth":{"bend":"origami"}})", a, &why), "an unknown bend is refused");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"cloth":{"lra":"psychic"}})", a, &why), "an unknown lra is refused");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"group":{"ragdollOf":"nobody"}})", a, &why), "an unknown ragdoll owner is refused");
        check(!sb::parseSoftAsset(R"({"format":"ocsoft","version":1,"kind":"liquid"})", a, &why), "an unknown kind is still refused");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
