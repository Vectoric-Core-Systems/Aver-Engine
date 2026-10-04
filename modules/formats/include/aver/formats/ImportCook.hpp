#pragma once
// Turning a whole import's materials and textures into project Content, and pointing the geometry
// at what was written.
//
// WHY THIS IS SEPARATE FROM MaterialCook.hpp. cookMaterials writes the bytes it is handed and knows
// nothing about where those bytes came from; this is the policy layer ABOVE it that a real import
// needs first -- folding a separately-authored opacity map into base-colour alpha (glTF's is already
// there; USD and .mtl name it as its own file) and capping texture size against a budget -- and then
// the bookkeeping every caller of cookMaterials needs afterwards: rewriting each mesh's material
// slots from the source names to the stems the cook actually wrote. MOVED HERE from AverAssetC.cpp,
// where this lived as file-local helpers until a second caller (the editor's own drag-and-drop
// import) needed the identical policy and could not reach into a tool's .cpp to get it.
#include "aver/formats/ImportedMaterial.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// Cooks an import's materials and images into contentDir and rewrites each mesh's material slots to
// the cooked names. Folds opacity maps into base colour and caps image size to maxTexture (0 = no
// cap) first. False only on a hard failure.
//
// THE ORDER IS LOAD-BEARING, and is why this takes `meshes` by reference rather than returning the
// cooked names for the caller to apply. Cooking renames each material to a prefixed, collision-free
// stem, and materialSlots has to be rewritten to match BEFORE `meshes` is serialised -- a rewrite
// afterwards would leave a written .ocmesh naming a material that is not on disk under that name.
//
// `overwriteExisting` is cookMaterials' own MaterialCookOptions::overwriteExisting, threaded through
// rather than fixed: a re-import that always replaces is right for a tool invoked per-asset into a
// scratch or per-asset output directory, and wrong for an editor import that means to leave a file a
// user may have hand-edited alone.
//
// `warnings` collects everything cookMaterials itself reports (an unreadable image, a material name
// collision, a texture format the decoder cannot read) -- never silently. `error` is set only on the
// hard failure that makes this return false. This function itself never logs, unlike the opacity-fold
// and texture-cap notes it drives internally: those stay direct AVER_INFO/AVER_WARN calls, same as
// before this moved here, because two different callers link this and each already owns its own console
// conventions for the ORCHESTRATION-level outcome (did the cook run, how much it wrote) -- which is
// exactly what these out-params carry back.
bool cookImportedMaterials(std::vector<ImportedMaterial>& materials, std::vector<ImportedImage>& images,
                           const std::string& contentDir, const std::string& assetBase,
                           std::vector<OcMeshData>& meshes, u32 maxTexture, bool overwriteExisting,
                           std::vector<std::string>* warnings, std::string* error,
                           u32* materialsWritten, u32* texturesWritten);

} // namespace aver::fmt
