#pragma once
// glTF 2.0 / GLB import into the engine's own formats. glTF is right-handed, +Y up, -Z forward, in
// metres; this engine is left-handed, +Z up, +X forward, in centimetres, so positions, normals and
// rotations are rebased, lengths scaled, and triangle winding reversed (the basis change has det -1).
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// What an import produced, and what it had to drop.
struct GltfImportResult {
    std::vector<OcMeshData> meshes;          // one per glTF mesh, submeshes per primitive
    std::vector<std::string> meshNames;      // parallel to `meshes`; "" where the source had none

    // Parallel to `meshes`: which `skeletons` entry a mesh's JOINTS_0/WEIGHTS_0 stream addresses,
    // or -1 when the mesh carries no skin. glTF puts the mesh-to-skin edge on the NODE that
    // instances a mesh, not on the mesh itself, so this is resolved while walking the scene graph
    // (Gltf::run()), not read off the mesh JSON directly.
    //
    // TWO MESHES WITH THE SAME meshSkinIndex SHARE ONE BONE-INDEX SPACE: their JOINTS_0 streams
    // have both already been remapped (in importSkins()) into the SAME skeletons[] entry's bone
    // order, so a caller may concatenate those streams directly, with no further remap -- see
    // `skeletons`' own comment below for why two glTF skin OBJECTS can resolve to the same entry.
    std::vector<i32> meshSkinIndex;

    // One entry per DISTINCT resolved skeleton. Bone order IS the skin's (post-sort) joint order,
    // which is what a mesh's JOINTS_0 indices refer to after importSkins()'s remap, so the two are
    // usable together with no further remap.
    //
    // NOT necessarily one entry per glTF skin OBJECT: when two or more skins name the exact same
    // joints in the exact same order -- the shape a Kenney/Blender "one armature, exported as a
    // separate per-mesh Armature-modifier skin" file produces -- they collapse to ONE entry here,
    // because they are provably the same skeleton (see importSkins()'s own comment). `meshSkinIndex`
    // above, not the count of this array, is what tells two meshes' bone spaces apart.
    std::vector<OcSkeleton>  skeletons;
    std::vector<std::string> skeletonNames;

    // Clips, in the file's own order. Bone indices address `skeletons[0]`.
    std::vector<OcAnimation> animations;
    std::vector<std::string> animationNames;

    std::vector<std::string> unsupported;    // features the file used and this importer cannot carry
};

// Knobs for the conversion.
struct GltfImportOptions {
    f32  scale = 100.0f;                     // metres to centimetres
    bool convertAxes = true;                 // the basis change and the winding reversal
    bool generateMissingNormals = true;      // compute flat normals when the source has none
};

// Imports from a file. `.glb` and `.gltf` are told apart by the GLB magic, not the extension.
bool importGltf(const std::string& path, GltfImportResult& out,
                const GltfImportOptions& opt = {}, std::string* why = nullptr);

// The same from memory. `baseDir` resolves relative buffer/image URIs; empty refuses them.
bool importGltfFromMemory(const u8* bytes, usize size, const std::string& baseDir,
                          GltfImportResult& out, const GltfImportOptions& opt = {},
                          std::string* why = nullptr);

} // namespace aver::fmt
