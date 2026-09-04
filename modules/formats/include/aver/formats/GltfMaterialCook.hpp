#pragma once
// Turning what a glTF said about its surfaces into .ocmat files and the texture files they name.
//
// WHY THIS IS A SEPARATE HEADER AND A SEPARATE TARGET. GltfImport lives in the base Aver.Formats
// library, which deliberately has no PBR dependency -- every headless mesh or animation tool links
// it, and none of them should drag the renderer family in. pbr::MaterialDesc only exists on the
// other side of that line, so the translation lives here, in the AVER_MODULE_PBR-gated
// Aver.Formats.Material target beside OcMat.cpp. GltfImportResult carries plain PODs across.
//
// NOTHING HERE HAND-FORMATS .ocmat TEXT. writeOcmat/saveOcmat already exist and are covered by
// MaterialTest's round-trip suite; this builds a pbr::MaterialDesc and hands it over. A second
// serialiser would be a second grammar to keep in step.
#include "aver/formats/GltfImport.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

struct GltfMaterialCookOptions {
    // The project's content root. Textures land under <contentDir>/Textures/<gltfBase>/ and
    // materials under <contentDir>/Materials/, because that is where the engine's own loaders look:
    // materials are found by a NON-recursive scan of Content/Materials keyed on the bare filename
    // stem, so a material's identity is its filename and nothing else.
    std::string contentDir;

    // Prefixes every generated material filename, and names the texture subfolder. NOT optional in
    // practice: material identity is a bare stem, so an unprefixed "Default" or "Material_0" from
    // two different imports would collide -- silently, since the second write would simply repaint
    // every use of the first.
    std::string gltfBase;

    // false leaves an existing file alone (the editor's import convention for meshes and skeletons);
    // true replaces it (a scratch output directory).
    bool overwriteExisting = false;
};

struct GltfMaterialCookResult {
    // Parallel to GltfImportResult::materials. The stem actually written -- which is NOT the glTF's
    // own material name, because of the prefix and any collision suffix -- or empty where the
    // material could not be cooked. THE CALLER MUST REWRITE OcMeshData::materialSlots THROUGH THIS,
    // before serialising the mesh, or the mesh names a material that is not on disk.
    std::vector<std::string> materialSlotNames;

    // Parallel to GltfImportResult::images. The content-relative path written, or empty.
    std::vector<std::string> texturePaths;

    u32 materialsWritten = 0;
    u32 texturesWritten  = 0;
    u32 skippedExisting  = 0;   // left alone because overwriteExisting was false
};

// Writes the textures, then the materials that reference them. Returns false only on an error that
// stopped the whole step; an individual material or image that could not be written is reported
// through `out` (an empty slot name / path) and through `warnings`, never silently.
bool cookGltfMaterials(const GltfImportResult& res, const GltfMaterialCookOptions& opt,
                       GltfMaterialCookResult& out, std::vector<std::string>* warnings = nullptr,
                       std::string* err = nullptr);

} // namespace aver::fmt
