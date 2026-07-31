#pragma once
// glTF 2.0 / GLB import into the engine's own formats. glTF is right-handed, +Y up, -Z forward, in
// metres; this engine is left-handed, +Z up, +X forward, in centimetres, so positions, normals and
// rotations are rebased, lengths scaled, and triangle winding reversed (the basis change has det -1).
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// What an import produced, and what it had to drop.
struct GltfImportResult {
    std::vector<OcMeshData> meshes;          // one per glTF mesh, submeshes per primitive
    std::vector<std::string> meshNames;      // parallel to `meshes`; "" where the source had none
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
