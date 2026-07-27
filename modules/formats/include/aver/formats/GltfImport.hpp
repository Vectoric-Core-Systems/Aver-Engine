#pragma once
// glTF 2.0 / GLB import — the converter that turns something authored in Blender, Maya or Substance
// into the engine's own formats.
//
// glTF is the target rather than FBX because it is an open specification with a published byte
// layout, needs no vendor SDK, and carries meshes, skeletons and animations in ONE file -- exactly
// the set .ocmesh / .ocskel / .ocanim was built for. FBX would mean a large closed dependency for a
// format that carries no more information.
//
// THE COORDINATE CHANGE IS THE PART THAT MATTERS, and it is the reason this file is not simply a
// buffer copy. glTF is RIGHT-handed, +Y up, -Z forward, and its unit is the METRE. This engine is
// LEFT-handed, +Z up, +X forward, +Y right, and its unit is the CENTIMETRE. So every position,
// every normal and every rotation is rebased, every length is scaled by 100, and -- because the
// basis change has determinant -1 -- every triangle's winding is reversed. Skipping that last step
// is the classic import bug: the model looks correct until backface culling is enabled, at which
// point it is inside out, and by then nobody remembers the importer.
//
// WHAT IS IMPLEMENTED: .gltf (JSON with external or embedded buffers) and .glb (binary), buffers
// from a GLB BIN chunk / a base64 data URI / a sibling file, bufferViews with strides, accessors of
// every component type with normalisation, and meshes with POSITION, NORMAL, TEXCOORD_0 and indices,
// flattened through the node hierarchy so a mesh parented under a rotated node arrives where the
// author put it.
//
// WHAT IS NOT, YET: sparse accessors, skins and animation clips (the formats exist; the extraction
// does not), morph targets, materials and textures, and Draco compression. Each is reported as an
// unsupported feature by name rather than silently producing a mesh with a piece missing -- a
// half-imported asset that looks plausible is worse than a refused one.
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

struct GltfImportResult {
    std::vector<OcMeshData> meshes;          // one per glTF mesh, submeshes per primitive
    std::vector<std::string> meshNames;      // parallel to `meshes`; "" where the source had none
    // Things the file contained that this importer cannot yet carry, named individually so the
    // editor can say what was dropped instead of "some features were ignored".
    std::vector<std::string> unsupported;
};

struct GltfImportOptions {
    // Metres to centimetres. Exposed because a surprising number of exports are authored at the
    // wrong scale and fixing it at import is cheaper than fixing it in the DCC.
    f32  scale = 100.0f;
    // The basis change and the winding reversal that go with it. Off is for a file already authored
    // in engine space -- which the engine's own exporter would produce.
    bool convertAxes = true;
    // Recompute normals from the geometry when the source has none. glTF makes NORMAL optional and
    // says a renderer should compute flat normals; without this such a mesh arrives unlit-looking.
    bool generateMissingNormals = true;
};

// Import from a file. `.glb` and `.gltf` are told apart by content -- the GLB magic -- rather than
// by extension, because an extension is a claim and the magic is evidence.
bool importGltf(const std::string& path, GltfImportResult& out,
                const GltfImportOptions& opt = {}, std::string* why = nullptr);

// The same from memory. `baseDir` resolves relative buffer/image URIs; pass empty to refuse them,
// which is what an importer reading an embedded blob with no directory context should do.
bool importGltfFromMemory(const u8* bytes, usize size, const std::string& baseDir,
                          GltfImportResult& out, const GltfImportOptions& opt = {},
                          std::string* why = nullptr);

} // namespace aver::fmt
