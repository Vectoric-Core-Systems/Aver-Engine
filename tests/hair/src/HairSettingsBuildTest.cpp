// An .ochair driven into Jolt's HairSettings through aver_phys_hair_settings_* (HairSettingsBuild.hpp), on the CPU compute backend: the parameter blocks
// an authored record becomes, the scalp taken from an embedded SCLP or an .ocmesh, strands stored or generated, the root-to-scalp check that fails a groom
// whose scalp does not match, a resampled groom, and a short simulation of what was built. Exit code = failure count.
//
// Phase F's exit gate lives here: a generated groom builds settings on the CPU host with root-to-scalp below 0.5 cm.
// NOTHING IN HERE HAS BEEN RUN: thresholds are starting values for the first pass.
#include "aver/hair/HairSettingsBuild.hpp"

#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_hair_abi.h"

#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

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

static void freshWorld() {
    aver_phys_shutdown();
    aver_phys_set_fixed_step(1.0f / 60.0f);
    aver_phys_init();
}

// A groom on a flat 20 x 20 cm embedded scalp at z = 0, strands generated, a little curl, one joint.
static fmt::OcHairData flatGroom(u32 strands) {
    fmt::OcHairData d;
    d.scalpJoints = {"head"};
    d.hasScalp = true;
    d.scalp.positions = {-10, -10, 0, 10, -10, 0, 10, 10, 0, -10, 10, 0};
    d.scalp.triangles = {0, 1, 2, 0, 2, 3};
    d.scalp.joints.assign(16, 0);
    d.scalp.weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    d.sim.simFraction = 0.25f;
    d.sim.grid[0] = d.sim.grid[1] = d.sim.grid[2] = 8.0f;
    d.generate.present = true;
    d.generate.seed = 4242;
    d.generate.strands = strands;
    d.generate.lengthCm[0] = 8.0f; d.generate.lengthCm[1] = 10.0f;
    d.generate.segments = 8;
    d.generate.curlCm = 0.3f;
    d.generate.clumps = 4;
    d.generate.styleWeight = 0.6f;
    fmt::OcHairMaterial m;
    m.index = 0;
    m.bendCompliance = 1.0e-5f;
    d.materials = {m};
    return d;
}

int main() {
    AVER_INFO("=== the parameter blocks ===");
    {
        fmt::OcHairMaterial m;
        m.index = 2;
        m.collision = false;
        m.maxLinearCmS = 750.0f;
        m.gravity = {0.3f, 0.9f, 0.1f, 0.7f};
        m.bendMul[2] = 42.0f;
        m.radiusCm = {0.2f, 0.1f, 0.0f, 1.0f};
        m.globalPose = {0.05f, 0.0f, 0.0f, 0.4f};
        m.gravityPreload = 0.5f;
        float b[hair::kMaterialBlockSize];
        hair::materialBlock(m, 0.15f, b);
        check(b[AVER_PHYS_HAIRM_ENABLE_COLLISION] == 0.0f && b[AVER_PHYS_HAIRM_ENABLE_LRA] == 1.0f, "collision off, lra on");
        check(b[AVER_PHYS_HAIRM_MAX_LINEAR_CMS] == 750.0f, "the speed cap is in cm/s");
        check(b[AVER_PHYS_HAIRM_GRAVITY] == 0.3f && b[AVER_PHYS_HAIRM_GRAVITY + 1] == 0.9f && b[AVER_PHYS_HAIRM_GRAVITY + 2] == 0.1f && b[AVER_PHYS_HAIRM_GRAVITY + 3] == 0.7f,
              "the gravity gradient fills four slots in order");
        check(b[AVER_PHYS_HAIRM_BEND_MULTIPLIER + 2] == 42.0f && b[AVER_PHYS_HAIRM_RADIUS_CM] == 0.2f && b[AVER_PHYS_HAIRM_GLOBAL_POSE + 3] == 0.4f &&
                  b[AVER_PHYS_HAIRM_GRAVITY_PRELOAD] == 0.5f,
              "the multiplier, radius, global pose and preload slots");
        check(b[AVER_PHYS_HAIRM_SIM_FRACTION] == 0.15f, "a material with no simFraction takes the default");
        m.simFraction = 0.6f;
        hair::materialBlock(m, 0.15f, b);
        check(b[AVER_PHYS_HAIRM_SIM_FRACTION] == 0.6f, "and one with its own keeps it");

        fmt::OcHairSim sim;
        sim.iterationsPerSecond = 240.0f;
        sim.grid[1] = 20.0f;
        sim.initialGravity[2] = -500.0f;
        float p[hair::kParamBlockSize];
        hair::paramBlock(sim, p);
        check(p[AVER_PHYS_HAIRP_ITERATIONS_PER_SECOND] == 240.0f && p[AVER_PHYS_HAIRP_GRID_Y] == 20.0f && p[AVER_PHYS_HAIRP_INITIAL_GRAVITY_Z] == -500.0f &&
                  p[AVER_PHYS_HAIRP_SIM_FRACTION] == sim.simFraction,
              "the SIM block lands in its slots");
    }

    AVER_INFO("=== the scalp of a mesh ===");
    {
        fmt::OcMeshData mesh;
        mesh.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        mesh.indices = {0, 1, 2, 0, 2, 3};
        fmt::OcMeshSubmesh sm0;
        sm0.indexStart = 0; sm0.indexCount = 3; sm0.vertexCount = 4;
        fmt::OcMeshSubmesh sm1;
        sm1.indexStart = 3; sm1.indexCount = 3; sm1.vertexCount = 4;
        mesh.submeshes = {sm0, sm1};
        // Bone 0 = "Hips" (not in the groom), bone 1 = "Head", bone 2 = "Neck".
        mesh.joints = {1, 2, 0, 0,  1, 0, 0, 0,  0, 0, 0, 0,  2, 1, 0, 0};
        mesh.weights = {0.6f, 0.4f, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  0.5f, 0.5f, 0, 0};
        const std::vector<std::string> meshBones = {"Hips", "Head", "Neck"};
        const std::vector<std::string> scalpJoints = {"neck", "head"};

        hair::BuildScalp s;
        std::string why;
        check(hair::scalpFromMesh(mesh, -1, meshBones, scalpJoints, s, &why), "a skinned scalp mesh converts (" + why + ")");
        check(s.triangles.size() == 6 && s.jointCount == 2, "both triangles and two joints");
        // Vertex 0: head (scalp joint 1) 0.6 and neck (0) 0.4.
        check(s.joints4[0] == 1 && s.joints4[1] == 0 && std::fabs(s.weights4[0] - 0.6f) < 1e-5f && std::fabs(s.weights4[1] - 0.4f) < 1e-5f,
              "bone names are matched case-insensitively and re-indexed to the groom's joint order");
        // Vertex 2 follows only Hips, which the groom does not list: it falls back to joint 0.
        check(s.joints4[8] == 0 && s.weights4[8] == 1.0f && s.weights4[9] == 0.0f, "a vertex whose bones the groom does not list follows joint 0");

        hair::BuildScalp one;
        check(hair::scalpFromMesh(mesh, 1, meshBones, scalpJoints, one, &why) && one.triangles.size() == 3 && one.triangles[0] == 0 && one.triangles[1] == 2,
              "a SUBMESH takes only its triangles");
        check(!hair::scalpFromMesh(mesh, 5, meshBones, scalpJoints, one, &why), "a SUBMESH that does not exist is refused");

        hair::BuildScalp plain;
        check(hair::scalpFromMesh(mesh, -1, meshBones, {}, plain, &why) && plain.joints4.empty() && plain.jointCount == 1, "no scalp joints: one joint, no skin tables");

        fmt::OcMeshData bare;
        check(!hair::scalpFromMesh(bare, -1, meshBones, scalpJoints, plain, &why), "a mesh with no triangles is refused");

        const fmt::OcHairData g = flatGroom(10);
        hair::BuildScalp emb;
        check(hair::scalpFromGroom(g, emb, &why) && emb.positions.size() == 12 && emb.triangles.size() == 6 && emb.joints4.size() == 16 && emb.jointCount == 1,
              "an embedded scalp converts");
        fmt::OcHairData none = g;
        none.hasScalp = false;
        check(!hair::scalpFromGroom(none, emb, &why), "a groom with no embedded scalp says so");
    }

    AVER_INFO("=== building settings from a generated groom (phase F exit gate) ===");
    {
        freshWorld();
        const fmt::OcHairData groom = flatGroom(120);
        hair::BuildScalp scalp;
        std::string why;
        hair::scalpFromGroom(groom, scalp, &why);

        hair::BuildResult r;
        check(hair::buildHairSettings(groom, scalp, r, {}, &why), "the groom builds (" + why + ")");
        check(r.settings > 0 && r.generated && r.strandCount == 120, "a settings handle, generated strands, 120 of them");
        AVER_INFO("  root to scalp {:.5f} cm", r.rootToScalpCm);
        check(r.rootToScalpCm < 0.5f, "root-to-scalp is below 0.5 cm");

        int32_t info[AVER_PHYS_HAIRI_COUNT] = {};
        aver_phys_hair_settings_info(r.settings, info, AVER_PHYS_HAIRI_COUNT, nullptr, 0);
        check(info[AVER_PHYS_HAIRI_RENDER_STRANDS] == 120 && info[AVER_PHYS_HAIRI_RENDER_VERTICES] == 120 * 9, "120 render strands of 9 points");
        check(info[AVER_PHYS_HAIRI_SIM_STRANDS] == 30, "a quarter of them are simulated (simFraction 0.25)");
        check(info[AVER_PHYS_HAIRI_SCALP_JOINTS] == 1, "one scalp joint");

        // It simulates: finite positions, roots on the scalp.
        const float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const int32_t h = aver_phys_hair_create(r.settings, world, 0);
        check(h > 0, "an instance of it");
        for (int i = 0; i < 30; ++i) { aver_phys_hair_update(h, 1.0f / 60.0f); aver_phys_hair_flush(); }
        std::vector<float> sim(static_cast<size_t>(info[AVER_PHYS_HAIRI_SIM_VERTICES]) * 3, 0.0f);
        check(aver_phys_hair_read_positions(h, sim.data(), info[AVER_PHYS_HAIRI_SIM_VERTICES], nullptr, 0) == 1, "positions read back");
        bool finite = true;
        for (float v : sim) if (!std::isfinite(v)) finite = false;
        check(finite, "all finite after 30 steps");
        aver_phys_hair_destroy(h);
        aver_phys_hair_settings_destroy(r.settings);

        // The same groom with explicit strands (the generated ones, stored) builds to the same counts.
        fmt::OcHairData stored = groom;
        stored.strands = r.strands;
        stored.generate.present = false;
        hair::BuildResult r2;
        check(hair::buildHairSettings(stored, scalp, r2, {}, &why) && !r2.generated && r2.strandCount == 120, "stored strands build too, without generating (" + why + ")");
        if (r2.settings) aver_phys_hair_settings_destroy(r2.settings);

        // Resampled: every strand becomes 6 points.
        fmt::OcHairData resampled = groom;
        resampled.sim.resample = 6;
        hair::BuildResult r3;
        check(hair::buildHairSettings(resampled, scalp, r3, {}, &why), "a resampled groom builds (" + why + ")");
        int32_t info3[AVER_PHYS_HAIRI_COUNT] = {};
        aver_phys_hair_settings_info(r3.settings, info3, AVER_PHYS_HAIRI_COUNT, nullptr, 0);
        check(info3[AVER_PHYS_HAIRI_RENDER_VERTICES] == 120 * 6 && info3[AVER_PHYS_HAIRI_MAX_VERTICES_PER_STRAND] == 6, "every strand has 6 vertices after SIM resample 6");
        if (r3.settings) aver_phys_hair_settings_destroy(r3.settings);
    }

    AVER_INFO("=== refusals ===");
    {
        freshWorld();
        const fmt::OcHairData groom = flatGroom(40);
        hair::BuildScalp scalp;
        std::string why;
        hair::scalpFromGroom(groom, scalp, &why);

        // Stored strands (generated on the real scalp) do not match a scalp that sits 5 cm below them. A GENERATE record would follow the scalp it is given.
        hair::BuildResult gen;
        hair::buildHairSettings(groom, scalp, gen, {}, &why);
        if (gen.settings) aver_phys_hair_settings_destroy(gen.settings);
        fmt::OcHairData stored = groom;
        stored.strands = gen.strands;
        stored.generate.present = false;
        hair::BuildScalp off = scalp;
        for (size_t i = 2; i < off.positions.size(); i += 3) off.positions[i] = -5.0f;
        hair::BuildResult r;
        check(!hair::buildHairSettings(stored, off, r, {}, &why) && why.find("from the scalp") != std::string::npos && r.settings == 0,
              "a scalp 5 cm from the roots fails the build and says so");
        // The same offset passes when the limit is raised, with a warning only.
        hair::BuildOptions loose;
        loose.maxRootToScalpCm = 10.0f;
        hair::BuildResult r2;
        check(hair::buildHairSettings(stored, off, r2, loose, &why) && r2.rootToScalpCm > 4.9f && r2.rootToScalpCm < 5.1f, "and builds when the limit is raised, reporting 5 cm");
        if (r2.settings) aver_phys_hair_settings_destroy(r2.settings);

        hair::BuildScalp noScalp;
        check(!hair::buildHairSettings(groom, noScalp, r, {}, &why), "no scalp at all is refused");

        fmt::OcHairData noStrands = groom;
        noStrands.generate.present = false;
        check(!hair::buildHairSettings(noStrands, scalp, r, {}, &why), "no strands and no GENERATE is refused");

        fmt::OcHairData bad = groom;
        bad.sim.grid[0] = 2.0f;
        check(!hair::buildHairSettings(bad, scalp, r, {}, &why), "an invalid groom is refused before the physics layer sees it");
    }

    aver_phys_shutdown();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
