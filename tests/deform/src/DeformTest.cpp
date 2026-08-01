// Sub-bone vertex deformation, with no GPU.
//
// A cage bind is wrong in ways that only show up as a mesh that tears, swims, or shrinks slightly
// when nothing should have moved. The properties that catch all of that are arithmetic: weights that
// sum to one, a rigid translation of the whole cage reproducing itself exactly, zero displacement
// reproducing the rest mesh, and influence staying LOCAL. None of those is visible in a screenshot
// until it is far too late.
//
// It also checks the thing the module is named for: two vertices on the SAME BONE moving by
// different amounts, which bone skinning cannot do at all.
#include "aver/deform/Cage.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

// Records one assertion.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Records one assertion that two floats agree to `eps`.
static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

// A straight three-bone chain up +Z: joints at 0, 40 and 100.
static fmt::OcSkeleton chain() {
    fmt::OcSkeleton s;
    fmt::OcBone root;  root.name = "root";   root.parent = -1;
    fmt::OcBone spine; spine.name = "spine"; spine.parent = 0; spine.translation = Vec3{0, 0, 40};
    fmt::OcBone head;  head.name = "head";   head.parent = 1; head.translation = Vec3{0, 0, 60};
    s.bones = {root, spine, head};
    return s;
}

int main() {
    AVER_INFO("DeformTest");

    AVER_INFO("the sub-bone cage subdivides the skeleton");
    {
        const fmt::OcSkeleton s = chain();
        deform::CageOptions opt;
        opt.subdivisionsPerBone = 0;
        deform::Cage c0;
        deform::generateSubBoneCage(s, opt, c0);
        check(c0.nodeCount() == 3, "with no subdivision there is one node per joint");
        checkNear(c0.nodes[2].rest.z, 100.0f, 1e-4f, "and the last sits at the last joint");

        opt.subdivisionsPerBone = 3;
        deform::Cage c3;
        deform::generateSubBoneCage(s, opt, c3);
        // root(1) + spine(4) + head(4)
        check(c3.nodeCount() == 9, "three subdivisions a bone gives nine nodes, not three");
        check(c3.nodeCount() > c0.nodeCount(),
              "which is the whole point: granularity finer than the skeleton");
        // The spine segment runs 0..40, so its four nodes land at 10, 20, 30, 40.
        checkNear(c3.nodes[1].rest.z, 10.0f, 1e-4f, "the first sub-node is a quarter along the bone");
        checkNear(c3.nodes[4].rest.z, 40.0f, 1e-4f, "and the last lands exactly on the joint");
        check(c3.nodes[1].bone == 1, "a sub-node knows which bone it subdivides");
        checkNear(c3.nodes[1].alongBone, 0.25f, 1e-4f, "and how far along it sits");
    }

    AVER_INFO("binding: partition of unity and locality");
    {
        const fmt::OcSkeleton s = chain();
        deform::CageOptions opt;
        opt.subdivisionsPerBone = 3;
        deform::Cage cage;
        deform::CageBinding bind;

        // A column of vertices beside the chain, one every 10 cm.
        std::vector<f32> rest;
        for (int i = 0; i <= 10; ++i) { rest.push_back(5.0f); rest.push_back(0.0f); rest.push_back(i * 10.0f); }
        const u32 vcount = static_cast<u32>(rest.size() / 3);

        deform::buildCageForMesh(&s, rest.data(), vcount, opt, cage, bind);
        check(bind.vertexCount() == vcount, "every vertex gets a binding");
        check(bind.valid(), "and every weight set sums to one");

        // Zero displacement must reproduce the rest mesh EXACTLY, or the mesh shrinks on bind.
        std::vector<f32> disp(cage.nodeCount() * 3, 0.0f);
        std::vector<f32> out(rest.size(), 0.0f);
        deform::DeformParams p;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), vcount, p, out.data());
        f32 worst = 0.0f;
        for (usize i = 0; i < rest.size(); ++i) worst = std::fmax(worst, std::fabs(out[i] - rest[i]));
        checkNear(worst, 0.0f, 1e-6f, "zero displacement reproduces the rest mesh exactly");

        // Move the WHOLE cage: a rigid translation must come through unchanged, which is what the
        // partition of unity buys and what a stray un-normalised weight destroys.
        for (u32 n = 0; n < cage.nodeCount(); ++n) {
            disp[n * 3 + 0] = 7.0f; disp[n * 3 + 1] = -3.0f; disp[n * 3 + 2] = 11.0f;
        }
        deform::deformVertices(cage, bind, disp.data(), rest.data(), vcount, p, out.data());
        worst = 0.0f;
        for (u32 v = 0; v < vcount; ++v) {
            worst = std::fmax(worst, std::fabs(out[v * 3 + 0] - (rest[v * 3 + 0] + 7.0f)));
            worst = std::fmax(worst, std::fabs(out[v * 3 + 1] - (rest[v * 3 + 1] - 3.0f)));
            worst = std::fmax(worst, std::fabs(out[v * 3 + 2] - (rest[v * 3 + 2] + 11.0f)));
        }
        checkNear(worst, 0.0f, 1e-4f, "translating the whole cage translates every vertex the same");

        // Locality: one node moving must not drag a vertex at the far end of the chain.
        std::fill(disp.begin(), disp.end(), 0.0f);
        disp[1 * 3 + 0] = 50.0f;    // node 1 is low on the spine
        deform::deformVertices(cage, bind, disp.data(), rest.data(), vcount, p, out.data());
        const f32 movedNear = std::fabs(out[0 * 3 + 0] - rest[0 * 3 + 0]);
        const f32 movedFar  = std::fabs(out[10 * 3 + 0] - rest[10 * 3 + 0]);
        check(movedNear > 1.0f, "a node's displacement reaches the vertices beside it");
        checkNear(movedFar, 0.0f, 1e-5f, "and does NOT reach the far end of the mesh");
    }

    AVER_INFO("the feature itself: deformation below bone granularity");
    {
        // Two vertices beside the SAME bone, 20 cm apart. Bone skinning would move them
        // identically, because they share one bone transform. A cage need not.
        const fmt::OcSkeleton s = chain();
        deform::CageOptions opt;
        opt.subdivisionsPerBone = 3;
        deform::Cage cage;
        deform::CageBinding bind;
        const std::vector<f32> rest = {5, 0, 10,   5, 0, 30};   // both on the spine, bone 1
        deform::buildCageForMesh(&s, rest.data(), 2, opt, cage, bind);

        std::vector<f32> disp(cage.nodeCount() * 3, 0.0f);
        disp[1 * 3 + 0] = 40.0f;    // push one sub-node of the spine sideways
        std::vector<f32> out(6, 0.0f);
        deform::DeformParams p;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), 2, p, out.data());

        const f32 a = out[0] - rest[0];
        const f32 b = out[3] - rest[3];
        check(a > 1.0f, "the vertex nearest the pushed sub-node moves");
        check(std::fabs(a - b) > 1.0f,
              "and the one further along the SAME BONE moves differently -- which is what bone "
              "skinning cannot express");
    }

    AVER_INFO("the CPU/GPU parity contract: blend -> mirrorY -> gain -> clamp -> rest+disp");
    {
        deform::Cage cage;
        cage.nodes.push_back(deform::CageNode{Vec3{0, 0, 0}, -1, 0.0f});
        deform::CageBinding bind;
        bind.nodeIndex.assign(deform::kCageInfluences, -1);
        bind.weight.assign(deform::kCageInfluences, 0.0f);
        bind.nodeIndex[0] = 0;
        bind.weight[0] = 1.0f;

        const std::vector<f32> rest = {1, 2, 3};
        const std::vector<f32> disp = {10, 20, 30};
        std::vector<f32> out(3, 0.0f);

        deform::DeformParams p;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), 1, p, out.data());
        checkNear(out[0], 11.0f, 1e-5f, "the plain path is rest + displacement");
        checkNear(out[1], 22.0f, 1e-5f, "on every axis");

        p.mirrorY = true;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), 1, p, out.data());
        checkNear(out[1], 2.0f - 20.0f, 1e-5f, "mirrorY negates the displacement's Y, not the rest's");
        p.mirrorY = false;

        p.gain = 0.5f;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), 1, p, out.data());
        checkNear(out[0], 1.0f + 5.0f, 1e-5f, "gain scales the blended displacement");

        p.gain = 1.0f;
        p.maxDisplacement = 7.0f;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), 1, p, out.data());
        const f32 len = std::sqrt((out[0] - 1) * (out[0] - 1) + (out[1] - 2) * (out[1] - 2) +
                                  (out[2] - 3) * (out[2] - 3));
        checkNear(len, 7.0f, 1e-4f, "and MaxD clamps its LENGTH, not each axis separately");

        // The contract says an out-of-range node index is SKIPPED rather than read.
        p.maxDisplacement = 1e30f;
        bind.nodeIndex[1] = 999;
        bind.weight[1] = 0.0f;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), 1, p, out.data());
        checkNear(out[0], 11.0f, 1e-5f, "an out-of-range node index is skipped, not read");
    }

    AVER_INFO("the lattice fallback, for a mesh with no skeleton");
    {
        deform::CageOptions opt;
        opt.latticeX = opt.latticeY = opt.latticeZ = 3;
        deform::Cage cage;
        deform::CageBinding bind;
        const std::vector<f32> rest = {-10, -10, -10,  10, -10, -10,  10, 10, 10,  -10, 10, 10};
        deform::buildCageForMesh(nullptr, rest.data(), 4, opt, cage, bind);
        check(cage.nodeCount() == 27, "a 3x3x3 lattice is 27 nodes");
        check(bind.valid(), "and its binding is a partition of unity too");

        std::vector<f32> disp(cage.nodeCount() * 3, 0.0f);
        for (u32 n = 0; n < cage.nodeCount(); ++n) disp[n * 3 + 2] = 4.0f;
        std::vector<f32> out(rest.size(), 0.0f);
        deform::DeformParams p;
        deform::deformVertices(cage, bind, disp.data(), rest.data(), 4, p, out.data());
        f32 worst = 0.0f;
        for (u32 v = 0; v < 4; ++v)
            worst = std::fmax(worst, std::fabs(out[v * 3 + 2] - (rest[v * 3 + 2] + 4.0f)));
        checkNear(worst, 0.0f, 1e-4f, "a rigid lattice move carries a skeleton-less mesh exactly");
    }

    AVER_INFO(g_failures ? "DeformTest: {} FAILURES" : "DeformTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
