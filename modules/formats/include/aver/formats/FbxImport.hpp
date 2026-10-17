#pragma once
// FBX import (ufbx) into the engine's own formats, at parity with the glTF importer. Design:
// docs/plans/wave9/FBX_IMPORT.md; user-facing: docs/FBX_IMPORT.md.
//
// ufbx is asked to deliver the scene in glTF's frame (right-handed, +Y up, metres), so FBX goes
// through the SAME map as glTF -- x = -z, y = x, z = y, times `scale`, winding reversed once, and once
// more per mirrored node -- and a model exported to both formats lands in the same place.
//
// LIVES IN Aver.Formats.Fbx, NOT THE BASE TARGET: ufbx is a 1 MB C file and the shipped game runtime
// (Aver.Runtime) never links it. Without third_party/ufbx the target still builds (AVER_HAVE_UFBX=0) and
// importFbx fails with "built without ufbx".
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/ImportedCamera.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

struct FbxImportOptions {
    f32  scale = 100.0f;              // metres -> cm, after ufbx has converted the file to metres
    f32  unitMetersOverride = 0.0f;   // >0: ignore the file's unit, treat 1 file unit as this many metres
    bool convertAxes = true;          // false: skip the axis map (still metres*scale), as GltfImportOptions
    bool generateMissingNormals = true;   // ufbx smooth normals first, flat fallback after
    bool importMeshes = true, importSkeletons = true, importAnimations = true, importMaterials = true;
    bool importBlendShapes = true;
    bool fileColoursAreLinear = false;    // ImportedMaterial::baseColorFactorLinear; the OBJ precedent is false
    u32  bakeFps = 30;                    // resample_rate for non-linear curves
    bool reduceKeys = true;               // ufbx key reduction
    enum class RootMotion : u8 { Off, Auto, All } rootMotion = RootMotion::Auto;
    f32  rootMotionMinTravelCm = 10.0f;   // Auto: planar travel of the root bone over the clip
    f32  maxMemoryMiB = 2048.0f;          // handed to ufbx's allocator limits
};

using FbxCamera = ImportedCamera;

// The curve-name prefix of a blend-shape weight on a clip: "morph.<channel>". The single constant the
// morph-target feature has to agree with.
inline constexpr const char* kFbxMorphCurvePrefix = "morph.";

// One blend channel of one mesh, as sparse engine-space deltas.
struct FbxBlendShape {
    std::string name;                 // channel name
    std::vector<u32> vertices;        // LOD-0 vertex indices of the owning OcMeshData
    std::vector<f32> positionDeltas;  // 3 per entry, engine space, cm, the SAME transform as that mesh's positions
    std::vector<f32> normalDeltas;    // 3 per entry or empty
    f32 defaultWeight = 0.0f;         // 0..1
};

struct FbxImportResult : GltfImportResult {
    // Parallel to `meshes`. Filled only once the morph-target feature can carry them (see
    // FbxImport.cpp); until then a blend-shape mesh is reported under `unsupported`.
    std::vector<std::vector<FbxBlendShape>> blendShapes;
    struct Stats {
        u32 nodes = 0, meshes = 0, bones = 0, clips = 0, truncatedInfluenceVertices = 0;
        f32 maxDroppedWeight = 0.0f;
        u32 sockets = 0;
    } stats{};
    std::string report;               // one line per decision worth reading (axis/unit source, bind-pose use, dropped data)
};

// Imports from a file.
bool importFbx(const std::string& path, FbxImportResult& out, const FbxImportOptions& = {},
               std::string* why = nullptr);
// The same from memory. `baseDir` resolves external textures; empty means "there is no folder", and a
// texture that is not embedded is then reported rather than searched for.
bool importFbxFromMemory(const u8* bytes, usize size, const std::string& baseDir, FbxImportResult& out,
                         const FbxImportOptions& = {}, std::string* why = nullptr);
// "Kaydara FBX Binary  \0" or an ASCII FBXHeaderExtension in the first 4 KiB.
bool looksLikeFbx(const u8* bytes, usize size);

// "scale=100;rootMotion=auto;..." -- every field, round-trippable. An unknown key is an error naming it.
std::string fbxOptionsToText(const FbxImportOptions&);
bool fbxOptionsFromText(std::string_view text, FbxImportOptions& out, std::string* why);

// True when this build links ufbx (AVER_HAVE_UFBX).
bool fbxImportAvailable();

} // namespace aver::fmt
