#pragma once
// glTF 2.0 / GLB import into the engine's own formats. glTF is right-handed, +Y up, -Z forward, in
// metres; this engine is left-handed, +Z up, +X forward, in centimetres, so positions, normals and
// rotations are rebased, lengths scaled, and triangle winding reversed (the basis change has det -1).
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// An image the file carried, already decoded to its ENCODED bytes -- a PNG stays a PNG, a JPEG stays
// a JPEG. Nothing here re-encodes: a lossy round trip through a decoder would change bytes the
// source had every right to expect back, and for a JPEG it would lose quality for no reason.
//
// glTF has THREE ways to name an image and they all land here: an external file URI, a base64
// `data:` URI, and a bufferView into the binary chunk. Which one it was is not recorded, because
// nothing downstream should care.
struct GltfImage {
    std::vector<u8> bytes;      // the encoded file, verbatim
    std::string ext;            // ".png" or ".jpeg", from the mimeType or the URI
    std::string suggestedName;  // sanitised stem for the file this becomes; never a path
    bool ok = false;            // false when it could not be read -- see GltfImportResult::unsupported
};

// One glTF material, flattened to the subset this engine can express.
//
// EVERY DEFAULT HERE IS glTF'S OWN, and the values are not obvious: metallicFactor and
// roughnessFactor both default to 1.0, so a material that states neither is a fully rough METAL.
// Defaulting them to 0 instead yields a plausible-looking dielectric that is wrong on every asset
// relying on the spec, and wrong in a way that reads as a lighting bug rather than an import bug.
struct GltfMaterial {
    // Which image a texture slot points at, if any. texCoord is carried so the importer can say it
    // ignored a nonzero one rather than silently sampling uv0.
    struct TexRef { i32 imageIndex = -1; u32 texCoord = 0; };

    std::string name;                        // the JSON name, else "Material_<index>"
    f32  baseColorFactor[4] = {1, 1, 1, 1};
    f32  emissiveFactor[3]  = {0, 0, 0};
    f32  metallicFactor     = 1.0f;          // glTF default is ONE, not zero
    f32  roughnessFactor    = 1.0f;          // likewise
    f32  normalScale        = 1.0f;
    f32  occlusionStrength  = 1.0f;
    f32  alphaCutoff        = 0.5f;          // only meaningful when alphaMode is MASK
    std::string alphaMode   = "OPAQUE";      // OPAQUE | MASK | BLEND, verbatim
    bool doubleSided        = false;
    TexRef baseColorTex, metalRoughTex, normalTex, occlusionTex, emissiveTex;
};

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

    // Index-parallel to the file's own materials[] and images[] arrays, so a TexRef's imageIndex is
    // a direct subscript into `images`. Empty when the file declared none.
    //
    // DELIBERATELY POD AND PBR-FREE. Turning these into a pbr::MaterialDesc happens in
    // GltfMaterialCook, which lives in the PBR-gated Aver.Formats.Material target -- see that
    // header. Doing it here would put the render family behind every headless tool that links
    // Aver.Formats for nothing but meshes.
    std::vector<GltfMaterial> materials;
    std::vector<GltfImage>    images;

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
