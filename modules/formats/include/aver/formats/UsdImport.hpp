#pragma once
// USD import into the engine's own formats — the USDA (ASCII) subset, parsed natively.
//
// WHY A SUBSET AND NOT OpenUSD. Pixar's OpenUSD is Apache-2.0, so the licence is not the objection;
// the size is. It pulls TBB and a hundred-megabyte build for what this engine actually wants from a
// .usd file, which is triangles, normals, UVs and a transform. A native reader for the ASCII
// encoding covers hand-authored files and anything exported as .usda, costs no dependency, and — the
// part that matters — REFUSES what it cannot read BY NAME instead of returning an empty mesh.
//
// importUsd reads ONE text layer. USDC (the binary crate encoding) and USDZ (a zip of the above) are
// detected from their magic bytes and rejected with a message saying so; importUsdStage, below, is
// the entry point that reads USDC (UsdCrate.hpp) and composes a multi-layer stage. A silent empty
// import is the failure mode this design is built to avoid: it looks identical to a model that
// legitimately has no geometry, and it sends the user looking at the renderer.
#include "aver/formats/ImportedMaterial.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// Which USD encoding a file turned out to be. Determined from CONTENT, never the extension: `.usd`
// is legally either ASCII or binary, so trusting the name gets it wrong roughly half the time.
enum class UsdEncoding {
    Unknown,
    Usda,        // `#usda 1.0` — the text encoding, and the only one importUsd reads
    Usdc,        // `PXR-USDC` — the binary crate encoding; importUsdStage reads it (UsdCrate.hpp)
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
    std::string name;                        // the prim path; empty for a PointInstancer instance
    // Engine-space rotation (x, y, z, w; the engine's Quat convention) and per-axis scale. Identity
    // for an ordinary mesh prim, whose rotation and scale are baked into its vertices. A
    // PointInstancer instance carries its own here, because every instance shares ONE mesh.
    f32  rotation[4] = {0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};

// A UsdGeomCamera in the stage, as a viewpoint: engine-space position and the direction it looks
// (USD cameras look down their local -Z), as yaw and pitch in the editor camera's own convention --
// forward = (cos pitch cos yaw, cos pitch sin yaw, sin pitch). importUsdStage only.
struct UsdCameraPose {
    std::string name;            // the prim path
    Vec3 position{0, 0, 0};
    f32  yawDeg = 0.0f, pitchDeg = 0.0f;
};

// The stage's sun, from its lights (importUsdStage only). A UsdLux DistantLight is a sun outright;
// a DomeLight whose latlong texture holds a distinct sun (a bright compact peak, not an overcast
// sky) gives one too, found as the centroid of the image's brightest texels.
//
// THE DOME'S LATLONG ORIENTATION, VERIFIED AGAINST A RENDER, NOT ASSUMED: longitude (u - 0.5) * 360
// degrees measured from the dome's +Z toward its +X, latitude (0.5 - v) * 180 toward its +Y pole;
// then the pole is turned to the stage's up axis when `poleAxis` asks for it ("scene" on a Z-up
// stage, or "Z") by a +90 degree turn about X; then the light prim's own transform. Checked on Intel's
// Jungle Ruins: this lights the pyramid faces its Karma renders light, and the mirrored longitude
// (the reading of UsdLux's OpenEXR citation this first used) lights the opposite ones.
struct UsdSun {
    bool found = false;
    Vec3 direction{0, 0, 1};          // engine space, TOWARD the sun (the .ocworld SUN convention)
    f32  angularDiameterDeg = 0.53f;  // a DistantLight's `inputs:angle`; the real sun otherwise
    std::string source;               // the light prim's path
};

// One PointInstancer prototype as built, and what the instance budget did to it.
struct UsdPrototypeStats {
    i32 meshIndex = -1;          // into UsdImportResult::meshes
    u64 triangles = 0;
    u64 instances = 0;           // declared, across every instancer that uses it
    u64 kept = 0;                // became placements
    f32 radius = 0.0f;           // bounding radius at its mean instance scale, engine units
    f32 fullDensityRadius = 0.0f; // focusBySize: kept in full within this distance of the focus, cm
};

// What importUsdStage found under PointInstancers, and how much of it came through.
struct UsdInstancingStats {
    u64 instancers = 0;
    u64 prototypes = 0;          // distinct prototype meshes built
    u64 sourceInstances = 0;     // every instance the stage declares
    u64 keptInstances = 0;       // those that became placements (see UsdImportOptions::maxInstances)
    std::vector<UsdPrototypeStats> perPrototype;
    // The budget's focus, when it had one (UsdImportOptions::focus), and how the disc inside its
    // radius fared: declared there, kept there.
    bool focused = false;
    Vec3 focusPoint{0, 0, 0};    // engine space; z unused -- the focus is horizontal
    f32 focusRadius = 0.0f;
    u64 sourceInFocus = 0;
    u64 keptInFocus = 0;
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
    // importUsdStage only, parallel to `meshes` (may be shorter; missing = ""): the name of the
    // folder holding the layer the mesh's geometry came from -- "Anthurium" for
    // elements/Anthurium/anthurium_classes.usda. A multi-file stage is organised by its folders, and
    // a caller writing one file per mesh can keep that organisation instead of one flat directory.
    std::vector<std::string> meshGroups;

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

    UsdInstancingStats instancing;           // importUsdStage only
    std::vector<UsdCameraPose> cameras;      // importUsdStage only, in stage order
    UsdSun sun;                              // importUsdStage only
};

// Where importUsdStage's instance budget concentrates (UsdImportOptions::focus).
enum class UsdInstanceFocus : u8 {
    Uniform,        // every instance equally likely to be kept, wherever it stands
    FirstCamera,    // around the stage's first UsdGeomCamera; Uniform, with a note, when it has none
    Point,          // around UsdImportOptions::focusPoint
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

    // A mesh whose authored normals are FLAT PER FACE while Blender's `primvars:sharp_face` marks
    // those faces smooth gets smooth normals rebuilt from its geometry, angle-weighted, the way
    // Blender shades a smooth face; faces marked sharp stay flat. That contradiction is an exporter
    // artefact, not an intent: Jungle Ruins' terrain arrives this way (98.6% of faces "smooth", every
    // face's corners within 0.004 degrees of each other) and rendered as a mosaic of triangles once
    // nothing covered the ground. Any other mesh keeps its authored normals untouched.
    bool honourSharpFace = true;

    // ---- importUsdStage's PointInstancer budget; 0 = no limit ----
    //
    // A THINNED COPY, NOT A SUBSAMPLE OF ONE CORNER. Every placement the importer keeps is a real
    // instance at its real transform; what the budget decides is how many of each prototype survive.
    // It is shared out by sqrt(instance count) x prototype size -- a million moss clumps do not crowd
    // out forty trees -- and each prototype's share is picked by a hash of the instance, spread
    // evenly or concentrated around a focus (below). `maxInstanceTriangles` then halves the
    // costliest prototype's share until the kept instances fit, handing what it gave up to the
    // cheaper prototypes, so a heavy tree cannot take the frame alone.
    //
    // THE PROTOTYPE MESHES are built once each regardless, and every static (non-instanced) mesh
    // always comes through.
    u64 maxInstances = 0;
    u64 maxInstanceTriangles = 0;

    // WHERE THE BUDGET GOES. Spread evenly, a forest scattered over kilometres keeps one tree in a
    // thousand everywhere and the place the stage was built to be looked at comes out as bare as
    // the horizon. With a focus, an instance within `focusRadius` (horizontal, engine centimetres)
    // competes at full weight and one beyond it at (radius / distance)^3: full density near the
    // focus, thinning with distance, never cut off. A prototype with at most `keepAllBelow`
    // instances keeps every one of them wherever they stand (0 = none exempt).
    UsdInstanceFocus focus = UsdInstanceFocus::Uniform;
    Vec3 focusPoint{0, 0, 0};    // engine space, for UsdInstanceFocus::Point
    f32  focusRadius = 10000.0f;
    u64  keepAllBelow = 0;
    // THINNING BY WHAT CAN BE SEEN, for instances that will be GPU-instanced (AverAssetC's foliage
    // mode): each prototype keeps full density out to max(focusRadius, K x its drawn radius) and
    // (reach / distance)^3 beyond, with K the largest value whose expected kept count fits
    // maxInstances. A 12 m tree then stays dense a kilometre out while 15 cm moss thins past the
    // focus -- a single falloff for every prototype thinned the forests to 0.2% while leaving a
    // quarter of the budget unspent (a prototype's keep saturates at 1, the falloff did not).
    // Needs a focus; maxInstanceTriangles is not applied.
    bool focusBySize = false;

    // importUsdStage: prims to leave out, each with everything beneath it ("/root/E_X3_Y3_28"). For a
    // source that stacks two versions of the same thing -- Jungle Ruins lays its high-detail
    // "cinematic" terrain tiles exactly over four of its backdrop tiles, and the coarse copy pokes
    // through as faint facets once nothing covers the ground.
    std::vector<std::string> excludePrims;
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

// Imports a whole USD STAGE from its root layer, text or binary (UsdStageImport.cpp):
//   - the root's subLayers, composed as a layer stack (strongest first, merged by prim path);
//   - USDC (binary crate) layers as well as USDA (UsdCrate.hpp);
//   - references, payloads and inherits (class prims), resolved to the geometry they bring in;
//   - PointInstancers, each prototype built ONCE as its own mesh and each kept instance a placement
//     carrying its own rotation and scale (see UsdImportOptions::maxInstances for the budget);
//   - cameras as viewpoints, and a DistantLight or a DomeLight's sun as the level's sun (UsdSun).
// Materials and textures come through exactly as importUsd's do -- the same conversion runs on both.
// Everything is in the ROOT stage's units and up axis, which is how USD itself composes a stage.
//
// Not composed, and reported in `unsupported`: variant sets, specializes, relocates, UsdLux area
// lights, and time-sampled values (the default is used). A file importUsd can read imports identically here
// except that it also gains its sublayers and references.
bool importUsdStage(const std::string& path, UsdImportResult& out,
                    const UsdImportOptions& opt = {}, std::string* why = nullptr);

} // namespace aver::fmt
