#pragma once
// USD import into the engine's own formats — the USDA (ASCII) subset, parsed natively.
//
// WHY A SUBSET AND NOT OpenUSD. Pixar's OpenUSD is Apache-2.0, so the licence is not the objection;
// the size is. It pulls TBB and a hundred-megabyte build for what this engine actually wants from a
// .usd file, which is triangles, normals, UVs and a transform. A native reader for the ASCII
// encoding covers hand-authored files and anything exported as .usda, costs no dependency, and — the
// part that matters — REFUSES what it cannot read BY NAME instead of returning an empty mesh.
//
// USDC (the binary crate encoding) and USDZ (a zip of the above) are detected from their magic bytes
// and rejected with a message saying exactly that and how to convert. A silent empty import is the
// failure mode this design is built to avoid: it looks identical to a model that legitimately has no
// geometry, and it sends the user looking at the renderer.
#include "aver/formats/ImportedMaterial.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// Which USD encoding a file turned out to be. Determined from CONTENT, never the extension: `.usd`
// is legally either ASCII or binary, so trusting the name gets it wrong roughly half the time.
enum class UsdEncoding {
    Unknown,
    Usda,        // `#usda 1.0` — the text encoding, and the only one this importer reads
    Usdc,        // `PXR-USDC` — the binary crate encoding
    Usdz,        // a zip archive (`PK\3\4`) holding any of the above
};

// Sniffs the encoding from the leading bytes. Safe on short buffers.
UsdEncoding usdSniff(const u8* bytes, usize size);
const char* usdEncodingName(UsdEncoding e);

// WHERE A PRIM SAT IN THE STAGE. The USD twin of GltfPlacement, and it exists for the same measured
// reason: baking a prim's TRANSLATION into its vertices puts the mesh's pivot wherever the stage's
// origin happened to be, and every consumer of a pivot then reads that gap as real -- the gizmo, the
// bounds, culling, the GI box, framing the selection.
struct UsdPlacement {
    i32  meshIndex = -1;                     // into UsdImportResult::meshes
    Vec3 position{0, 0, 0};                  // engine space, centimetres
    std::string name;                        // the prim path
};

struct UsdImportResult {
    // One per UsdGeomMesh prim in the stage, with the prim's ROTATION AND SCALE baked in.
    //
    // NOT ITS TRANSLATION, ANY MORE, and the note that used to sit here -- "an importer that dropped
    // the transform would silently pile every mesh at the origin" -- was right about the danger and
    // wrong about the remedy. Dropping the translation with nowhere to put it does pile everything at
    // the origin; baking it instead moves the pivot off the mesh, which is quieter and worse, because
    // nothing looks broken until you try to move or frame the thing. The translation now comes out
    // into `placements` below, so the stage is preserved AND the pivot is on the geometry.
    std::vector<OcMeshData>  meshes;
    std::vector<std::string> meshNames;      // the prim path, e.g. "/root/body"

    // One per mesh-bearing prim, in stage order. A caller that wants the stage back writes these as
    // placements; a caller that only wants the meshes can ignore them.
    std::vector<UsdPlacement> placements;

    // The stage's UsdPreviewSurface materials, and the images they name. SHARED SHAPES, not USD's
    // own -- see ImportedMaterial.hpp -- so the same cook serves glTF, OBJ and USD.
    //
    // A mesh's materialSlots[0] holds the `name` of the material its `rel material:binding` resolved
    // to, and is EMPTY when it had no binding or the binding pointed at a prim this file does not
    // contain. That is the join, and it is the one piece with no glTF equivalent: glTF's
    // mesh-to-material edge is a JSON index the importer resolves as it reads, while USD's is a prim
    // path that may name a Material declared later in the file, so it is resolved after the walk.
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedImage>    images;

    UsdEncoding encoding = UsdEncoding::Unknown;
    f32 sourceMetersPerUnit = 1.0f;          // as the stage declared it
    std::string sourceUpAxis;                // "Y" or "Z", as the stage declared it

    std::vector<std::string> unsupported;    // features the file used and this importer cannot carry
};

struct UsdImportOptions {
    // Applied ON TOP of the stage's own metersPerUnit, which is read and honoured. Leave at 100 to
    // land in centimetres: a stage saying metersPerUnit=1 scales by 100, one saying 0.01 (already
    // centimetres) scales by 1. Setting this to 1 gives the stage's own units unchanged.
    f32  scale = 100.0f;

    // Honour the stage's upAxis. USD records Y-up or Z-up explicitly, so unlike OBJ there is no
    // guessing: a Y-up stage is rebased to this engine's +Z up, a Z-up stage only changes handedness.
    bool convertAxes = true;

    bool generateMissingNormals = true;
};

// Imports a .usda from disk. A material's `inputs:file` paths resolve against the file's own
// directory, which is what USD's asset resolution does for a relative `@path@` in the absence of a
// resolver plugin.
bool importUsd(const std::string& path, UsdImportResult& out,
               const UsdImportOptions& opt = {}, std::string* why = nullptr);

// The same from memory. `baseDir` resolves the `@path@` asset references a UsdUVTexture names; empty
// refuses them, leaving the texture slot unbound and saying so in `unsupported` -- the same contract
// the glTF and OBJ readers use, and for the same reason: a content pipeline that reads whatever
// happens to be in the working directory is a reproducibility bug waiting to happen.
bool importUsdFromMemory(const u8* bytes, usize size, const std::string& baseDir,
                         UsdImportResult& out, const UsdImportOptions& opt = {},
                         std::string* why = nullptr);

} // namespace aver::fmt
