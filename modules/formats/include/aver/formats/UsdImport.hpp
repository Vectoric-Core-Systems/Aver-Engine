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

struct UsdImportResult {
    // One per UsdGeomMesh prim in the stage, with the prim's world transform already baked in —
    // the engine has no stage concept to preserve it in, and an importer that dropped the transform
    // would silently pile every mesh at the origin.
    std::vector<OcMeshData>  meshes;
    std::vector<std::string> meshNames;      // the prim path, e.g. "/root/body"

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

bool importUsd(const std::string& path, UsdImportResult& out,
               const UsdImportOptions& opt = {}, std::string* why = nullptr);

bool importUsdFromMemory(const u8* bytes, usize size, UsdImportResult& out,
                         const UsdImportOptions& opt = {}, std::string* why = nullptr);

} // namespace aver::fmt
