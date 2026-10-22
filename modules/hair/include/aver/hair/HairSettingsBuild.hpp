#pragma once
// From an .ochair to Jolt's HairSettings, through the plain-C ABI only (physics_hair_abi.h, aver_phys_hair_settings_*). The conversion of a
// groom's authored records into the ABI's parameter blocks is pure and tested without a world; buildHairSettings then drives the builder and
// needs one (aver_phys_init), because finish creates the compute buffers on the installed host.
//
// WHERE THE SCALP COMES FROM. A groom either embeds its scalp (SCLP) or names an .ocmesh (SCALP). Reading that mesh and finding its skeleton's
// joint names belongs to the caller (the scene system has the asset cache); scalpFromMesh turns the decoded mesh into what the builder needs,
// remapping the mesh's bone indices to the groom's SCALPJOINTS order, which is the order set_palette takes its matrices in.
#include "aver/core/Types.hpp"
#include "aver/formats/OcHair.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/hair/HairGenerate.hpp"

#include <string>
#include <vector>

namespace aver::hair {

// The two parameter blocks of the ABI (AVER_PHYS_HAIRM_COUNT and AVER_PHYS_HAIRP_COUNT; HairSettingsBuild.cpp static_asserts them against the header).
inline constexpr int kMaterialBlockSize = 41;
inline constexpr int kParamBlockSize = 10;

// An authored material as a HAIRM block. `simFractionDefault` is used when the material has none of its own.
void materialBlock(const fmt::OcHairMaterial& m, f32 simFractionDefault, f32 out[kMaterialBlockSize]);
// An authored SIM record as a HAIRP block.
void paramBlock(const fmt::OcHairSim& sim, f32 out[kParamBlockSize]);

// The scalp as the builder takes it.
struct BuildScalp {
    std::vector<f32> positions;     // cm, 3 per vertex
    std::vector<i32> triangles;     // 3 per triangle
    std::vector<i32> joints4;       // 4 per vertex, an index into the groom's SCALPJOINTS; empty = one joint
    std::vector<f32> weights4;      // 4 per vertex; empty with joints4
    i32 jointCount = 1;
};

// The scalp a groom embeds (SCLP). False with `why` set when it has none.
bool scalpFromGroom(const fmt::OcHairData& groom, BuildScalp& out, std::string* why = nullptr);

// The scalp of a decoded .ocmesh. `submesh` -1 takes every submesh. A skinned mesh's bone indices are mapped through `meshJointNames` (the name of
// bone i of the skeleton the mesh is bound to) to positions in `scalpJointNames`; an influence that names a joint the groom does not list is dropped
// and the rest renormalised (a vertex left with none follows joint 0). An unskinned mesh follows joint 0.
bool scalpFromMesh(const fmt::OcMeshData& mesh, i32 submesh, const std::vector<std::string>& meshJointNames,
                   const std::vector<std::string>& scalpJointNames, BuildScalp& out, std::string* why = nullptr);

// The generator's view of a scalp (positions, triangles, no mask).
ScalpMesh generatorScalp(const BuildScalp& scalp);

struct BuildOptions {
    f32 maxRootToScalpCm = 2.0f;    // above this the build fails: the roots do not sit on the scalp this groom was given
    f32 warnRootToScalpCm = 0.5f;   // above this the build succeeds and logs
};

struct BuildResult {
    i32 settings = 0;               // the settings handle (aver_phys_hair_settings_finish), 0 on failure
    f32 rootToScalpCm = 0.0f;
    u32 strandCount = 0;            // render strands handed to the physics
    u32 pointCount = 0;
    bool generated = false;         // the strands came from GENERATE and not from STRN
    fmt::OcHairStrands strands;     // the strands used (the generated ones when generated), so the caller can build the render tables from them
};

// Drives aver_phys_hair_settings_* for a groom. Strands: the groom's explicit strands, else GENERATE over the scalp. Materials: the groom's records,
// and the defaults for any index a strand uses and no record describes. A refusal from the physics layer comes back as `why`. On failure
// nothing is left behind (a settings object that failed the root check is destroyed).
bool buildHairSettings(const fmt::OcHairData& groom, const BuildScalp& scalp, BuildResult& out, const BuildOptions& opt = {},
                       std::string* why = nullptr);

} // namespace aver::hair
