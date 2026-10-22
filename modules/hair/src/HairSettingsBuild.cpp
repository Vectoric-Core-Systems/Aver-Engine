// HairSettingsBuild.cpp -- an .ochair driven into aver_phys_hair_settings_*. See HairSettingsBuild.hpp.
#include "aver/hair/HairSettingsBuild.hpp"

#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_hair_abi.h"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace aver::hair {

static_assert(kMaterialBlockSize == AVER_PHYS_HAIRM_COUNT, "the material block size moved in physics_hair_abi.h");
static_assert(kParamBlockSize == AVER_PHYS_HAIRP_COUNT, "the settings block size moved in physics_hair_abi.h");

namespace {

bool fail(std::string* why, std::string m) {
    if (why) *why = std::move(m);
    return false;
}

void putGradient(f32* dst, const fmt::OcHairGradient& g) {
    dst[0] = g.min;
    dst[1] = g.max;
    dst[2] = g.minFraction;
    dst[3] = g.maxFraction;
}

bool equalsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// The reason the physics layer gave for its last refusal, as text.
std::string physicsReason() {
    return abiErrorNameOf(aver_phys_last_error());
}

} // namespace

void materialBlock(const fmt::OcHairMaterial& m, f32 simFractionDefault, f32 out[kMaterialBlockSize]) {
    out[AVER_PHYS_HAIRM_ENABLE_COLLISION] = m.collision ? 1.0f : 0.0f;
    out[AVER_PHYS_HAIRM_ENABLE_LRA] = m.lra ? 1.0f : 0.0f;
    out[AVER_PHYS_HAIRM_LINEAR_DAMPING] = m.linearDamping;
    out[AVER_PHYS_HAIRM_ANGULAR_DAMPING] = m.angularDamping;
    out[AVER_PHYS_HAIRM_MAX_LINEAR_CMS] = m.maxLinearCmS;
    out[AVER_PHYS_HAIRM_MAX_ANGULAR_RADS] = m.maxAngularRadS;
    putGradient(out + AVER_PHYS_HAIRM_GRAVITY, m.gravity);
    out[AVER_PHYS_HAIRM_FRICTION] = m.friction;
    out[AVER_PHYS_HAIRM_BEND_COMPLIANCE] = m.bendCompliance;
    for (int k = 0; k < 4; ++k) out[AVER_PHYS_HAIRM_BEND_MULTIPLIER + k] = m.bendMul[k];
    out[AVER_PHYS_HAIRM_STRETCH_COMPLIANCE] = m.stretchCompliance;
    out[AVER_PHYS_HAIRM_INERTIA_MULTIPLIER] = m.inertiaMul;
    putGradient(out + AVER_PHYS_HAIRM_RADIUS_CM, m.radiusCm);
    putGradient(out + AVER_PHYS_HAIRM_WORLD_INFLUENCE, m.worldInfluence);
    putGradient(out + AVER_PHYS_HAIRM_GRID_VELOCITY, m.gridVelocity);
    out[AVER_PHYS_HAIRM_GRID_DENSITY_FORCE] = m.gridDensityForce;
    putGradient(out + AVER_PHYS_HAIRM_GLOBAL_POSE, m.globalPose);
    putGradient(out + AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE, m.skinGlobalPose);
    out[AVER_PHYS_HAIRM_SIM_FRACTION] = m.simFraction >= 0.0f ? m.simFraction : simFractionDefault;
    out[AVER_PHYS_HAIRM_GRAVITY_PRELOAD] = m.gravityPreload;
}

void paramBlock(const fmt::OcHairSim& sim, f32 out[kParamBlockSize]) {
    out[AVER_PHYS_HAIRP_ITERATIONS_PER_SECOND] = sim.iterationsPerSecond;
    out[AVER_PHYS_HAIRP_MAX_DELTA_TIME] = sim.maxDeltaTime;
    out[AVER_PHYS_HAIRP_GRID_X] = sim.grid[0];
    out[AVER_PHYS_HAIRP_GRID_Y] = sim.grid[1];
    out[AVER_PHYS_HAIRP_GRID_Z] = sim.grid[2];
    out[AVER_PHYS_HAIRP_BOUNDS_PADDING_CM] = sim.boundsPaddingCm;
    out[AVER_PHYS_HAIRP_SIM_FRACTION] = sim.simFraction;
    out[AVER_PHYS_HAIRP_INITIAL_GRAVITY_X] = sim.initialGravity[0];
    out[AVER_PHYS_HAIRP_INITIAL_GRAVITY_Y] = sim.initialGravity[1];
    out[AVER_PHYS_HAIRP_INITIAL_GRAVITY_Z] = sim.initialGravity[2];
}

bool scalpFromGroom(const fmt::OcHairData& groom, BuildScalp& out, std::string* why) {
    if (!groom.hasScalp) return fail(why, "hair build: the groom embeds no scalp");
    const fmt::OcHairScalp& s = groom.scalp;
    out = BuildScalp{};
    out.positions = s.positions;
    out.triangles.assign(s.triangles.begin(), s.triangles.end());
    out.jointCount = static_cast<i32>(std::max<usize>(groom.scalpJoints.size(), 1));
    out.joints4.assign(s.joints.begin(), s.joints.end());
    out.weights4 = s.weights;
    return true;
}

bool scalpFromMesh(const fmt::OcMeshData& mesh, i32 submesh, const std::vector<std::string>& meshJointNames,
                   const std::vector<std::string>& scalpJointNames, BuildScalp& out, std::string* why) {
    const u32 vc = mesh.vertexCount();
    if (vc < 3 || mesh.indices.size() < 3) return fail(why, "hair build: the scalp mesh has no triangles");
    out = BuildScalp{};
    out.positions = mesh.positions;

    u32 first = 0, last = static_cast<u32>(mesh.indices.size()), base = 0;
    if (submesh >= 0) {
        if (static_cast<usize>(submesh) >= mesh.submeshes.size()) return fail(why, "hair build: the scalp SUBMESH " + std::to_string(submesh) + " does not exist");
        const fmt::OcMeshSubmesh& sm = mesh.submeshes[static_cast<usize>(submesh)];
        first = sm.indexStart;
        last = std::min<u32>(sm.indexStart + sm.indexCount, static_cast<u32>(mesh.indices.size()));
        base = sm.baseVertex;
    }
    out.triangles.reserve(last > first ? last - first : 0);
    for (u32 i = first; i + 3 <= last; i += 3) {
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = mesh.indices[i + k] + base;
            if (v >= vc) return fail(why, "hair build: the scalp mesh has an index past its vertices");
            out.triangles.push_back(static_cast<i32>(v));
        }
    }
    if (out.triangles.empty()) return fail(why, "hair build: the scalp submesh has no triangles");

    out.jointCount = static_cast<i32>(std::max<usize>(scalpJointNames.size(), 1));
    if (mesh.hasSkin() && !scalpJointNames.empty()) {
        std::vector<i32> remap(meshJointNames.size(), -1);
        for (usize i = 0; i < meshJointNames.size(); ++i)
            for (usize j = 0; j < scalpJointNames.size(); ++j)
                if (equalsNoCase(meshJointNames[i], scalpJointNames[j])) { remap[i] = static_cast<i32>(j); break; }
        out.joints4.assign(static_cast<usize>(vc) * 4, 0);
        out.weights4.assign(static_cast<usize>(vc) * 4, 0.0f);
        for (u32 v = 0; v < vc; ++v) {
            f32 sum = 0.0f;
            for (u32 k = 0; k < 4; ++k) {
                const u16 bone = mesh.joints[static_cast<usize>(v) * 4 + k];
                const f32 w = mesh.weights[static_cast<usize>(v) * 4 + k];
                const i32 mapped = bone < remap.size() ? remap[bone] : -1;
                if (mapped < 0 || !(w > 0.0f)) continue;
                out.joints4[static_cast<usize>(v) * 4 + k] = mapped;
                out.weights4[static_cast<usize>(v) * 4 + k] = w;
                sum += w;
            }
            if (sum > 1.0e-6f) {
                for (u32 k = 0; k < 4; ++k) out.weights4[static_cast<usize>(v) * 4 + k] /= sum;
            } else {
                out.joints4[static_cast<usize>(v) * 4] = 0;
                out.weights4[static_cast<usize>(v) * 4] = 1.0f;
            }
        }
    }
    return true;
}

ScalpMesh generatorScalp(const BuildScalp& scalp) {
    ScalpMesh m;
    m.positions = scalp.positions;
    m.triangles.assign(scalp.triangles.begin(), scalp.triangles.end());
    return m;
}

bool buildHairSettings(const fmt::OcHairData& groom, const BuildScalp& scalp, BuildResult& out, const BuildOptions& opt, std::string* why) {
    out = BuildResult{};
    if (!groom.valid(why)) return false;
    if (scalp.positions.size() < 9 || scalp.triangles.size() < 3) return fail(why, "hair build: the groom needs a scalp (SCALP mesh or embedded SCLP)");

    // ---- the strands ----
    if (!groom.strands.empty()) {
        out.strands = groom.strands;
    } else {
        if (!groom.generate.present) return fail(why, "hair build: the groom has no strands and no GENERATE record");
        if (!generateHair(groom.generate, generatorScalp(scalp), out.strands, nullptr, why)) return false;
        out.generated = true;
    }
    const fmt::OcHairStrands& strands = out.strands;
    const i32 vertexCount = static_cast<i32>(strands.pointCount());
    const i32 strandCount = static_cast<i32>(strands.strandCount());

    std::vector<i32> start(strands.start.begin(), strands.start.end());
    std::vector<i32> material(strands.material.begin(), strands.material.end());

    // ---- the builder ----
    const i32 b = aver_phys_hair_settings_begin();
    if (b == 0) return fail(why, "hair build: cannot open a settings builder (" + physicsReason() + ")");
    auto refuseBuild = [&](const char* what) {
        aver_phys_hair_settings_cancel(b);
        return fail(why, std::string("hair build: ") + what + " refused by the physics layer (" + physicsReason() + ")");
    };
    if (!aver_phys_hair_settings_strands(b, strands.points.data(), nullptr, vertexCount, start.data(), material.data(), strandCount)) return refuseBuild("the strands were");

    const bool skinned = !scalp.joints4.empty() && scalp.joints4.size() == scalp.weights4.size();
    const i32 scalpVertices = static_cast<i32>(scalp.positions.size() / 3);
    if (!aver_phys_hair_settings_scalp(b, scalp.positions.data(), scalpVertices, scalp.triangles.data(), static_cast<i32>(scalp.triangles.size() / 3),
                                       skinned ? scalp.joints4.data() : nullptr, skinned ? scalp.weights4.data() : nullptr, scalp.jointCount))
        return refuseBuild("the scalp was");

    f32 block[kMaterialBlockSize];
    for (const fmt::OcHairMaterial& m : groom.materials) {
        materialBlock(m, groom.sim.simFraction, block);
        if (!aver_phys_hair_settings_material(b, static_cast<i32>(m.index), block, kMaterialBlockSize)) return refuseBuild("a material was");
    }
    f32 params[kParamBlockSize];
    paramBlock(groom.sim, params);
    if (!aver_phys_hair_settings_params(b, params, kParamBlockSize)) return refuseBuild("the simulation parameters were");

    f32 root = 0.0f;
    const i32 handle = aver_phys_hair_settings_finish(b, static_cast<i32>(groom.sim.resample), &root);
    if (handle == 0) return fail(why, "hair build: the physics layer could not finish the groom (" + physicsReason() + ")");

    if (root > opt.maxRootToScalpCm) {
        aver_phys_hair_settings_destroy(handle);
        return fail(why, "hair build: the roots are up to " + std::to_string(root) + " cm from the scalp (limit " + std::to_string(opt.maxRootToScalpCm) +
                             " cm); the scalp does not match the strands");
    }
    if (root > opt.warnRootToScalpCm)
        AVER_WARN("[Hair] roots are up to {:.2f} cm from the scalp (expected under {:.2f}); the hair may sit off the head", root, opt.warnRootToScalpCm);

    out.settings = handle;
    out.rootToScalpCm = root;
    out.strandCount = static_cast<u32>(strandCount);
    out.pointCount = static_cast<u32>(vertexCount);
    return true;
}

} // namespace aver::hair
