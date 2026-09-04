#pragma once
// Turning imported materials into .ocmat files and the texture files they name.
//
// FORMAT-NEUTRAL ON PURPOSE. This takes ImportedMaterial/ImportedImage, not any one importer's
// result struct, so glTF, OBJ and USD all feed the same cook. A cook per importer would have meant
// three translations to pbr::MaterialDesc, three collision policies, and -- the concrete cost -- a
// fourth copy of the reserved built-in look names, a list whose existing copies already carry a
// comment saying they must be kept in step by hand.
//
// WHY IT IS A SEPARATE TARGET. The importers live in the base Aver.Formats library, which
// deliberately has no PBR dependency: every headless mesh or animation tool links it, and none of
// them should drag the renderer family in. pbr::MaterialDesc only exists on the other side of that
// line, so the translation lives here, in the AVER_MODULE_PBR-gated Aver.Formats.Material target
// beside OcMat.cpp. ImportedMaterial carries plain PODs across.
//
// NOTHING HERE HAND-FORMATS .ocmat TEXT. writeOcmat/saveOcmat already exist and are covered by
// MaterialTest's round-trip suite; this builds a pbr::MaterialDesc and hands it over. A second
// serialiser would be a second grammar to keep in step, and this format has 20 PARAM keys.
#include "aver/formats/ImportedMaterial.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

struct MaterialCookOptions {
    // The project's content root. Textures land under <contentDir>/Textures/<assetBase>/ and
    // materials under <contentDir>/Materials/, because that is where the engine's own loaders look:
    // materials are found by a NON-recursive scan of Content/Materials keyed on the bare filename
    // stem, so a material's identity is its filename and nothing else.
    std::string contentDir;

    // Prefixes every generated material filename, and names the texture subfolder. NOT optional in
    // practice: material identity is a bare stem, so an unprefixed "Default" or "Material_0" from
    // two different imports would collide -- silently, since the second write would simply repaint
    // every use of the first.
    std::string assetBase;

    // false leaves an existing file alone (the editor's import convention for meshes and skeletons);
    // true replaces it (a scratch or per-asset output directory).
    bool overwriteExisting = false;
};

struct MaterialCookResult {
    // Parallel to the materials passed in. The stem actually written -- which is NOT the source's
    // own material name, because of the prefix and any collision suffix -- or empty where the
    // material could not be cooked. THE CALLER MUST REWRITE OcMeshData::materialSlots THROUGH THIS,
    // before serialising the mesh, or the mesh names a material that is not on disk.
    std::vector<std::string> materialSlotNames;

    // Parallel to the images passed in. The content-relative path written, or empty.
    std::vector<std::string> texturePaths;

    u32 materialsWritten = 0;
    u32 texturesWritten  = 0;
    u32 skippedExisting  = 0;   // left alone because overwriteExisting was false
};

// Writes the textures, then the materials that reference them. Returns false only on an error that
// stopped the whole step; an individual material or image that could not be written is reported
// through `out` (an empty slot name / path) and through `warnings`, never silently.
bool cookMaterials(const std::vector<ImportedMaterial>& materials,
                   const std::vector<ImportedImage>& images,
                   const MaterialCookOptions& opt, MaterialCookResult& out,
                   std::vector<std::string>* warnings = nullptr, std::string* err = nullptr);

// True when `stem` collides with one of the engine's built-in look names. Exposed because an
// importer may want to warn earlier than the cook does; the cook applies it regardless.
bool isReservedLookName(const std::string& stem);

} // namespace aver::fmt
