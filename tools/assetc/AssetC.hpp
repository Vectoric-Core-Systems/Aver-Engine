// AverAssetC's shared declarations: the JSON-Lines result helpers, the mesh/scene writers the model importers share, and one entry
// point per subcommand (each in its own tools/assetc/<Name>.cpp). AverAssetC.cpp parses the first argument and dispatches.
#pragma once

#include "aver/formats/AssetValidate.hpp"
#include "aver/formats/GltfImport.hpp"
#if AVER_HAVE_FBX
#include "aver/formats/FbxImport.hpp"   // .fbx (ufbx), see docs/FBX_IMPORT.md
#endif
#include "aver/formats/MaterialMap.hpp"   // --material-map
#include "aver/formats/ObjImport.hpp"
#include "aver/formats/UsdImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/MorphTarget.hpp"   // appendMorphTargets (mergeAll)
#include "aver/formats/OcWorld.hpp"   // the scene level a multi-node glTF now writes
#include "aver/formats/OcInstances.hpp"   // .ocinst -- where a PointInstancer's instances go by default
#include "aver/formats/OcStream.hpp"      // .ocstream -- `AverAssetC stream`
#include "aver/formats/OcHlodBake.hpp"    // `AverAssetC hlod`: per-cell proxy meshes
#include "aver/world/LevelTransform.hpp"   // its Euler encoding, for rotated USD instances
#include "aver/platform/FileSystem.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Hash.hpp"
#include "aver/extensions/Extensions.hpp"   // subcommands an installed extension offers (docs/EXTENSIONS.md)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace assetc {

using namespace aver;

// The usage text main prints (AverAssetC.cpp); `convert` repeats it for a missing input or --out-dir.
extern const char* const kUsage;

// ---- the JSON-Lines result contract (Common.cpp) ----

struct RunStats { int written = 0; int failed = 0; };

std::string stemOf(const std::string& path);
std::string extOf(const std::string& path);
std::string safe(const std::string& in, const std::string& fallback);
std::string jsonEscape(const std::string& in);
void emit(const std::string& json);
void emitArtifact(const std::string& input, const std::string& output, const char* kind,
                  bool ok, bool verified, const std::string& error, RunStats& stats,
                  const std::string& extra = {});
void emitSummary(const std::string& input, const RunStats& stats, int exitCode);
void splitCommaList(const std::string& list, std::vector<std::string>& out);
std::string cleanContentRelDir(std::string d);

// ---- mesh and scene writing (MeshWrite.cpp), shared by every model importer ----

// One imported mesh plus the skin identity it needs to merge safely. `skinIndex` mirrors
// GltfImportResult::meshSkinIndex: -1 when unskinned, otherwise an index two items share only when
// they are provably bound to the same skeleton (see GltfImport.cpp). OBJ and USD never populate a
// skin, so their items are always -1, which makes the merge below unconditionally safe for them.
struct MeshItem {
    std::string name;
    fmt::OcMeshData data;
    i32 skinIndex = -1;
    // A folder under the output directory to write this item into (one path component, sanitised by
    // writeMeshItems); empty = the output directory itself.
    std::string subdir;
};

// One placement as the level records it, whichever importer found it. glTF and ordinary USD prims
// carry only a translation (their rotation and scale are baked into the mesh); a USD PointInstancer
// instance carries its own rotation and scale, because every instance shares one mesh.
struct ScenePlacement {
    i32  meshIndex = -1;
    Vec3 position{0, 0, 0};
    Quat rotation = Quat::identity();
    Vec3 scale{1, 1, 1};
    bool collide = true;
    // Stem of the object clip this placement plays (an animated glTF node), "" for none. The level
    // writer makes it content-relative and adds the extension, exactly as it does for a mesh stem.
    std::string animClip;
    // A placement a position+rotation+scale cannot carry (shear or a mirror): `matrix` is its engine-space
    // row-vector world matrix (m[r*4+c]) and a PLACE line cannot hold it, so it goes to the .ocinst.
    bool exact = false;
    f32  matrix[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
};

// The viewpoint a level opens at, when the source scene named one (a USD stage's camera).
struct SceneCamera {
    Vec3 position{0, 0, 0};
    f32  yawDeg = 0.0f, pitchDeg = 0.0f;
};

// The sun, when the source scene lit itself with one (a USD DistantLight, or a DomeLight's sun).
struct SceneSun {
    Vec3 direction{0, 0, 1};   // toward the sun, engine space
    f32  angularDeg = 0.53f;
};

bool writeSceneLevel(const std::string& outDir, const std::string& contentDir,
                     const std::string& base,
                     const std::vector<ScenePlacement>& placements,
                     const std::vector<std::string>& stems,
                     const SceneCamera* camera = nullptr, const SceneSun* sun = nullptr,
                     const std::vector<ScenePlacement>* foliageInstances = nullptr,
                     const std::string& instancesDir = "Instances");

bool writeMeshItems(const std::string& input, const std::string& outDir, const std::string& base,
                    const std::vector<MeshItem>& items, bool merge, f32 lodRatio, RunStats& stats,
                    std::vector<std::string>* outStems = nullptr);

void cookAndRewriteSlots(std::vector<fmt::ImportedMaterial>& materials,
                         std::vector<fmt::ImportedImage>& images,
                         const std::string& contentDir, const std::string& base,
                         std::vector<fmt::OcMeshData>& meshes, u32 maxTexture);

// ---- a model file's whole conversion (Convert.cpp), shared by glTF and FBX ----

struct ModelConvertArgs {
    std::string input, outDir, base, contentDir;
    bool merge = false;
    f32 lodRatio = 0.0f;
    u32 maxTexture = 0;
    // An extension's model import (hostImportModel): the caller wants each mesh's stem and placement back instead of a level, and no summary line.
    std::vector<std::string>* stemsOut = nullptr;
    std::vector<ScenePlacement>* placementsOut = nullptr;
};

int convertImportedModel(const ModelConvertArgs& args, fmt::GltfImportResult& res, RunStats& stats);

// ---- extensions (ExtHost.cpp) ----

void openExtensions(ext::Registry& registry);
std::string extensionUsage(const ext::Registry& registry);

// ---- the subcommands: argv[0] is the executable, argv[1] the subcommand ----

int runConvert(int argc, char** argv);
#if AVER_HAVE_MATERIAL_COMPILE
int runMaterial(int argc, char** argv);
#endif
int runStream(int argc, char** argv);
int runHlod(int argc, char** argv);
#if AVER_HAVE_COOK
int runCookChunks(int argc, char** argv);
#endif
int runRetarget(int argc, char** argv);
int runVisBake(int argc, char** argv);
int runValidate(int argc, char** argv);
int runHair(int argc, char** argv);
int runHairText(int argc, char** argv);

} // namespace assetc
