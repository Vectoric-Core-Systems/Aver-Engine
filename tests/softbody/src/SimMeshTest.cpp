// buildSimMesh: welding render vertices into particles, the cage over them, masses, pins and reach.
// Pure arithmetic over in-memory meshes; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/SimMesh.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Two 10x10 quads side by side in the XZ plane, UV-seamed: the seam vertices exist twice in the
// render mesh (4/7 repeat 1/2), so 8 render vertices weld to 6 particles.
struct SeamMesh {
    std::vector<f32> pos = {0,0,0,  10,0,0,  10,0,10,  0,0,10,   10,0,0,  20,0,0,  20,0,10,  10,0,10};
    std::vector<f32> uv  = {0,0,  0.5f,0,  0.5f,1,  0,1,         0.5f,0,  1,0,  1,1,  0.5f,1};
    std::vector<u32> idx = {0,1,2, 0,2,3,  4,5,6, 4,6,7};
    sb::SimMeshInput input() const {
        sb::SimMeshInput in;
        in.positions = pos.data(); in.uvs = uv.data(); in.indices = idx.data();
        in.vertexCount = 8; in.indexCount = static_cast<u32>(idx.size());
        return in;
    }
};

static sb::SimBuildSpec clothSpec() {
    sb::SimBuildSpec s;
    s.material = sb::presetMaterial(sb::SoftPreset::ClothLight);
    return s;
}

int main() {
    AVER_INFO("SimMeshTest");

    AVER_INFO("a UV-seamed quad pair welds across the seam");
    {
        SeamMesh m;
        sb::SimMesh sim;
        const bool ok = sb::buildSimMesh(m.input(), clothSpec(), sim);
        check(ok && sim.error.empty(), "it builds");
        check(sim.cage.particles.size() == 6, "8 render vertices weld to 6 particles");
        check(sim.binding.vertexParticle.size() == 8 && sim.binding.vertexParticle[1] == sim.binding.vertexParticle[4] &&
              sim.binding.vertexParticle[2] == sim.binding.vertexParticle[7], "the binding is many-to-one across the seam");
        check(sim.binding.vertexParticle[0] != sim.binding.vertexParticle[5], "far vertices stay apart");
        check(sim.cage.beams.size() == 9, "one beam per undirected edge (9)");
        check(sim.cage.triangles.size() == 4 && sim.binding.triSim.size() == 4, "four triangles, each render triangle mapped");
        check(sim.cage.bends.size() == 3, "three interior edges make three bends (two diagonals and the seam)");
        bool layout = !sim.cage.bends.empty();
        for (const sb::Bend& b : sim.cage.bends) {
            const sb::Beam& beam = sim.cage.beams[b.beam];
            layout = layout && b.v[0] == beam.a && b.v[1] == beam.b && b.v[2] != b.v[3] && b.v[2] != b.v[0] && b.v[2] != b.v[1] &&
                     b.v[3] != b.v[0] && b.v[3] != b.v[1];
        }
        check(layout, "a bend is the shared edge first, then two distinct opposite corners");
        check(sim.repVertex.size() == 6 && sim.repVertex[sim.binding.vertexParticle[4]] == 1, "the lowest render vertex represents a particle");
        f32 total = 0.0f;
        for (const sb::Particle& p : sim.cage.particles) total += p.mass;
        check(approx(total, 0.25f * 0.02f, 1e-5f), "masses sum to areaDensity x area (0.25 kg/m2 x 200 cm2)");
        check(sim.meshKey != 0 && sim.meshKey == sb::simMeshKey(m.input(), 0.01f), "the mesh key is recorded");
    }

    AVER_INFO("degenerate and duplicate triangles");
    {
        SeamMesh m;
        m.idx.insert(m.idx.end(), {0, 1, 1});        // collapsed edge
        m.idx.insert(m.idx.end(), {2, 1, 0});        // the first triangle, reversed
        sb::SimMeshInput in = m.input();
        sb::SimMesh sim;
        check(sb::buildSimMesh(in, clothSpec(), sim), "it builds");
        check(sim.cage.triangles.size() == 4, "the degenerate and the duplicate triangles are not simulated");
        check(sim.binding.triSim.size() == 6 && sim.binding.triSim[4] == sb::kNone, "a degenerate render triangle has no sim triangle");
        check(sim.binding.triSim[5] == sim.binding.triSim[0], "a duplicate follows the triangle it repeats");
    }

    AVER_INFO("the entity transform is baked into the particles");
    {
        SeamMesh m;
        sb::SimBuildSpec s = clothSpec();
        s.entityWorld = Mat4::fromQuat(Quat::fromAxisAngle(Vec3{0, 0, 1}, kPi * 0.5f)) * Mat4::translation(Vec3{100, 0, 0});
        sb::SimMesh sim;
        check(sb::buildSimMesh(m.input(), s, sim), "it builds");
        // The mesh-local point (10, 0, 0) is the particle of render vertex 1.
        const Vec3 local = sim.bindLocal[sim.binding.vertexParticle[1]];
        const Vec3 world = sim.cage.particles[sim.binding.vertexParticle[1]].pos;
        check(approx(local.x, 10.0f) && approx(local.y, 0.0f), "the bind pose stays mesh-local");
        const Vec3 rel = world - Vec3{100, 0, 0};
        check(approx(rel.size(), 10.0f, 1e-3f), "the length from the entity origin is preserved");
        check(std::fabs(rel.x) < 1e-3f && approx(std::fabs(rel.y), 10.0f, 1e-3f), "a 90 degree yaw turns +X onto the Y axis");
        const sb::Beam& b0 = sim.cage.beams[0];
        check(approx(b0.rest0, dist(sim.cage.particles[b0.a].pos, sim.cage.particles[b0.b].pos), 1e-3f) && b0.rest0 > 9.99f,
              "rest lengths are measured in world space");
    }

    AVER_INFO("refusals");
    {
        SeamMesh m;
        sb::SimBuildSpec s = clothSpec();
        s.maxParticles = 5;
        sb::SimMesh sim;
        check(!sb::buildSimMesh(m.input(), s, sim) && !sim.error.empty(), "maxParticles overflow errors");
        sb::SimMeshInput bad = m.input();
        bad.indexCount = 4;
        check(!sb::buildSimMesh(bad, clothSpec(), sim) && !sim.error.empty(), "an index count that is not a multiple of three");
        std::vector<u32> wild = {0, 1, 99};
        bad = m.input(); bad.indices = wild.data(); bad.indexCount = 3;
        check(!sb::buildSimMesh(bad, clothSpec(), sim) && !sim.error.empty(), "an index out of range");
    }

    AVER_INFO("the mesh key");
    {
        SeamMesh m;
        const u64 a = sb::simMeshKey(m.input(), 0.01f);
        check(a == sb::simMeshKey(m.input(), 0.01f), "it is deterministic");
        m.pos[3 * 2 + 2] += 0.5f;   // move vertex 2
        check(sb::simMeshKey(m.input(), 0.01f) != a, "it changes when a vertex moves");
    }

    AVER_INFO("pins from the default, from paint, from rules and from a mask");
    {
        SeamMesh m;
        sb::SimMesh sim;
        // Cloth with nothing painted: the top row (z = 10) is pinned.
        check(sb::buildSimMesh(m.input(), clothSpec(), sim), "it builds");
        u32 pinned = 0;
        bool topOnly = true;
        for (u32 p = 0; p < sim.cage.particles.size(); ++p) {
            const sb::Particle& q = sim.cage.particles[p];
            if (!q.pinned) continue;
            ++pinned;
            topOnly = topOnly && approx(sim.bindLocal[p].z, 10.0f) && q.reachCm == 0.0f && q.invMass == 0.0f;
        }
        check(pinned == 3 && topOnly, "a cloth with no paint hangs from its top edge (3 particles, hard skin, no inverse mass)");

        sb::SimBuildSpec dest = clothSpec();
        dest.destructible = true;
        check(sb::buildSimMesh(m.input(), dest, sim), "a destructible builds");
        pinned = 0;
        for (const sb::Particle& q : sim.cage.particles) pinned += q.pinned ? 1u : 0u;
        check(pinned == 0, "a destructible with no paint pins nothing");

        // Painted: pin particle 0 only; reach on particle 1.
        sb::SimBuildSpec painted = clothSpec();
        painted.paint.meshKey = sb::simMeshKey(m.input(), 0.01f);
        painted.paint.pin.assign(6, 0);
        painted.paint.pin[0] = 255;
        painted.paint.reach.assign(6, 255);
        painted.paint.reach[1] = 127;
        check(sb::buildSimMesh(m.input(), painted, sim) && !sim.paintStale, "painted data for this mesh builds, not stale");
        check(sim.cage.particles[0].pinned && !sim.cage.particles[1].pinned && sim.cage.particles[2].reachCm < 0.0f,
              "the painted pin applies and the rest are free");
        check(sim.cage.particles[1].reachCm > 5.9f && sim.cage.particles[1].reachCm < 6.1f, "reach 127/254 of twice 6 cm is about 6 cm");

        // Stale paint falls back to the rules.
        sb::SimBuildSpec stale = painted;
        stale.paint.meshKey ^= 1;
        sb::PaintRule sphere;
        sphere.type = sb::PaintRule::Type::Sphere;
        sphere.centreCm[0] = 20.0f; sphere.centreCm[2] = 5.0f; sphere.radiusCm = 6.0f;
        stale.paint.rules.push_back(sphere);
        check(sb::buildSimMesh(m.input(), stale, sim) && sim.paintStale, "paint for another mesh is reported stale");
        u32 sphereHits = 0;
        for (u32 p = 0; p < sim.cage.particles.size(); ++p)
            if (sim.cage.particles[p].pinned) { ++sphereHits; check(approx(sim.bindLocal[p].x, 20.0f), "the sphere rule pins at x = 20"); }
        check(sphereHits == 2, "two particles lie in the sphere");

        // Above rule.
        sb::SimBuildSpec above = clothSpec();
        sb::PaintRule r;
        r.type = sb::PaintRule::Type::Above; r.axis = 0; r.valueCm = 15.0f;
        above.paint.rules.push_back(r);
        check(sb::buildSimMesh(m.input(), above, sim), "an above rule builds");
        pinned = 0;
        for (const sb::Particle& q : sim.cage.particles) pinned += q.pinned ? 1u : 0u;
        check(pinned == 2, "x >= 15 pins the two particles on the far edge");

        // Mask rule against an in-memory texture: R is 0.2 left of u = 0.5 and 0.9 from there.
        sb::SimBuildSpec mask = clothSpec();
        sb::PaintRule mr;
        mr.type = sb::PaintRule::Type::Mask; mr.texture = "mem"; mr.pinBelow = 0.5f;
        mask.paint.rules.push_back(mr);
        mask.mask = [](const std::string& tex, f32 u, f32, f32& rr, f32& gg) {
            if (tex != "mem") return false;
            rr = u < 0.5f ? 0.2f : 0.9f;
            gg = 1.0f;
            return true;
        };
        check(sb::buildSimMesh(m.input(), mask, sim), "a mask rule builds");
        pinned = 0;
        bool leftOnly = true;
        for (u32 p = 0; p < sim.cage.particles.size(); ++p)
            if (sim.cage.particles[p].pinned) { ++pinned; leftOnly = leftOnly && sim.bindLocal[p].x < 5.0f; }
        check(pinned == 2 && leftOnly, "the mask pins the two particles whose UV reads below the threshold");
    }

    AVER_INFO("skin influences follow the first render vertex");
    {
        SeamMesh m;
        std::vector<u16> joints(8 * 4, 0);
        std::vector<f32> weights(8 * 4, 0.0f);
        for (u32 v = 0; v < 8; ++v) { joints[v * 4] = static_cast<u16>(v + 1); weights[v * 4] = 1.0f; }
        sb::SimMeshInput in = m.input();
        in.joints = joints.data(); in.weights = weights.data();
        sb::SimMesh sim;
        check(sb::buildSimMesh(in, clothSpec(), sim) && sim.joint4.size() == 24 && sim.weight4.size() == 24, "joint and weight arrays per particle");
        check(sim.joint4[sim.binding.vertexParticle[4] * 4] == 2, "the seam particle takes render vertex 1's joint, not 4's");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
