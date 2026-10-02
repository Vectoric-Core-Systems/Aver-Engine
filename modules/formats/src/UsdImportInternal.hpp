#pragma once
// What UsdImport.cpp (the USDA text reader) and UsdStageImport.cpp (composition, the USDC crate
// encoding and PointInstancers) share. Private to Aver.Formats: nothing outside this module includes
// it, and nothing here is an API.
//
// THE SPLIT: UsdImport.cpp owns every conversion from USD data into engine data -- a gathered mesh
// into an OcMeshData (buildMesh), a Material's shaders into an ImportedMaterial (buildMaterial), a
// binding into a material slot (resolveBindings). The stage importer only FINDS that data -- in a
// crate, behind a reference, under an instancer -- and hands it to the same functions, so a mesh or
// material read from binary USD cannot come out different from the same one read from text.
#include "aver/formats/UsdImport.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace aver::fmt::usd_detail {

// 4x4, row-major, row-vector (v * M) -- USD's own convention, see UsdImport.cpp.
struct M4 {
    f32 m[16];
    static M4 identity() {
        M4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
};
M4 mul(const M4& a, const M4& b);
M4 translate(f32 x, f32 y, f32 z);
M4 scaleM(f32 x, f32 y, f32 z);
M4 rotateAxis(int axis, f32 deg);
// Inverse of an affine matrix (last column 0,0,0,1). Identity when singular.
M4 inverseAffine(const M4& a);

// A UsdGeomMesh's attributes as gathered, before conversion.
struct MeshAttrs {
    std::vector<f32> points, normals, uvs;
    std::vector<i32> faceVertexIndices, faceVertexCounts;
    std::string subdivisionScheme;
    std::string orientation;
    std::string normalsInterp;      // the `interpolation` metadata on `normals`, when stated
    std::string uvInterp;
    // The prim path a `rel material:binding` named, unresolved. Resolved AFTER the walk, because
    // USD does not require a Material to be declared before the mesh that binds it.
    std::string materialBinding;
    // A GeomSubset's own fields, read through this same struct because a subset body is parsed with
    // the same member reader. `indices` on a face subset are FACE numbers, which is why they cannot
    // share faceVertexIndices.
    std::string      subsetElementType;
    std::vector<i32> subsetIndices;
    // USD puts doubleSided on the GEOMETRY; glTF and .ocmat put it on the material. Carried here and
    // folded into the bound material by resolveBindings.
    bool doubleSided = false;
    bool hasPoints = false;
    // Blender's `primvars:sharp_face` (uniform bool, one per face), when the file has it: nonzero =
    // flat-shaded. Read so buildMesh can catch normals that contradict it (rebuildSmoothNormals).
    std::vector<i32> sharpFace;
};

// One `def GeomSubset` under a Mesh: a named set of FACE indices with its own material binding.
// This is how a DCC exports a single mesh painted with several materials, and it is how every tree
// in Intel's Jungle Ruins is authored -- trunk, branches and leaves are three subsets of one mesh.
// Without it a tree imports entirely as its mesh-level binding, which is the bark: a tree with no
// leaves, and no error to say so.
struct GeomSubsetDef {
    std::string      name;
    std::string      binding;   // the prim path its `rel material:binding` named, unresolved
    std::vector<i32> faces;     // indices into faceVertexCounts, NOT into faceVertexIndices
};

// One Shader prim, flattened: inputs as raw value text (numbers are re-read with allNumbers) or as
// connections to another shader's prim path. `values` and `connects` are small enough that a linear
// scan beats a map: a UsdPreviewSurface has at most a dozen inputs.
struct ShaderPrim {
    std::string path;
    std::string id;                                              // info:id
    std::vector<std::pair<std::string, std::string>> values;     // input name -> raw text
    std::vector<std::pair<std::string, std::string>> connects;   // input name -> source prim path
    std::vector<std::pair<std::string, char>> connectOuts;       // input name -> named channel, or 0
    std::string file;                                            // inputs:file, @@ stripped

    const std::string* value(const char* n) const {
        for (const auto& v : values) if (v.first == n) return &v.second;
        return nullptr;
    }
    const std::string* connect(const char* n) const {
        for (const auto& v : connects) if (v.first == n) return &v.second;
        return nullptr;
    }
    // The channel `n`'s connection named, or 0 if it named none. Kept parallel to `connects` rather
    // than folded into it so every caller that only wants the target prim is untouched.
    char connectOut(const char* n) const {
        for (const auto& v : connectOuts) if (v.first == n) return v.second;
        return 0;
    }
};

// A UsdLux DomeLight or DistantLight in a TEXT layer, unparsed: its path, type and body text. The
// stage importer reads the few attributes it needs from the body; the prim's transform is in
// Ctx::primWorlds under `path`.
struct LightPrim {
    std::string path;
    std::string type;       // "DomeLight" or "DistantLight"
    std::string body;
};

// A mesh found but NOT yet converted: its attributes, its subsets, and its local-to-layer-root
// transform in USD space. What the text walk hands the stage importer instead of building, so the
// stage can decide the transform (a prototype's, an instance's) before the vertices are baked.
struct RawMesh {
    std::string path;
    MeshAttrs attrs;
    std::vector<GeomSubsetDef> subsets;
    M4 world;
};

struct Ctx {
    UsdImportResult* out = nullptr;
    const UsdImportOptions* opt = nullptr;
    f32 unitScale = 100.0f;         // stage metersPerUnit folded with opt->scale
    bool yUp = true;

    std::string baseDir;            // resolves a UsdUVTexture's `@path@`; empty refuses them

    // Parallel to out->meshes, one entry per MATERIAL SLOT of that mesh (a mesh with GeomSubsets has
    // several): the prim path each slot's `rel material:binding` named, or empty. A second array
    // rather than a field on OcMeshData because the binding is a USD concept that does not survive
    // into the engine's mesh -- only the resolved slot name does.
    std::vector<std::vector<std::string>> meshBinding;
    // Parallel to meshBinding: whether the GEOMETRY behind each slot said doubleSided. Per slot, not
    // per mesh, because the stage importer merges a prototype's meshes into one -- a plant's leaf
    // cards are doubleSided and its trunk is not, and the trunk's bark must not inherit that.
    std::vector<std::vector<bool>> slotDoubleSided;
    // Every Material prim path, and the out->materials entry it resolved to. NOT parallel to
    // out->materials: several paths may share one entry -- see buildMaterial's dedup.
    std::vector<std::pair<std::string, usize>> materialPaths;
    // Parallel to out->images: the RESOLVED path each was read from, so two materials naming the
    // same texture share one image rather than writing its bytes twice under two names.
    std::vector<std::string> imageSources;
    bool sawPartialSubsetCover = false;
    bool sawUnreadableTexture = false;
    bool sawTextureSubstituted = false;
    bool sawUnresolvedBinding = false;
    bool sawNonUniformScale = false;
    bool sawTimeSamples = false;
    bool sawReference = false;
    bool sawVariant = false;
    bool sawInstancing = false;
    bool sawMaterial = false;
    bool sawSubdiv = false;
    bool sawSeparateMetallic = false;
    u32  rebuiltNormalMeshes = 0;   // meshes whose flat normals contradicted sharp_face (rebuildSmoothNormals)
    // ---- the stage importer's additions; all off for a plain importUsd ----
    // When set, the text walk collects each mesh here (with its full transform) instead of building
    // it, and records every prim's world transform in primWorlds.
    std::vector<RawMesh>* rawSink = nullptr;
    std::unordered_map<std::string, M4>* primWorlds = nullptr;
    // Every UsdGeomCamera prim's path; its transform is in primWorlds.
    std::vector<std::string>* cameraPaths = nullptr;
    // Every DomeLight/DistantLight prim (LightPrim).
    std::vector<LightPrim>* lights = nullptr;
    // Prefixed onto every Material path this context registers AND every binding buildMesh records,
    // so two layers declaring the same material path cannot bind each other's materials.
    std::string pathPrefix;
};

// A USDA layer's header metadata.
struct UsdaHeader {
    f32 metersPerUnit = 1.0f;
    bool yUp = true;
    std::string upAxis;                     // "Y" or "Z" as declared ("Y" when absent)
    std::string defaultPrim;
    std::vector<std::string> subLayers;     // as authored, strongest first
};

// Converts one gathered mesh (see UsdImport.cpp); pushes onto c.out->meshes.
void buildMesh(Ctx& c, const MeshAttrs& a, const M4& world, const std::string& primPath,
               const std::vector<GeomSubsetDef>& subsets);
// Converts one Material's shaders into c.out->materials (deduplicated), registering
// c.pathPrefix + path for bindings.
void buildMaterial(Ctx& c, const std::string& path, const std::vector<ShaderPrim>& shaders,
                   const std::string& surfacePath);
// Fills every mesh's material slots from its recorded bindings.
void resolveBindings(Ctx& c);
// The Ctx's saw* flags as `unsupported` lines on c.out.
void reportUnsupported(Ctx& c);
// Parses a USDA layer's text: its header into `hdr`, every prim through the walk (building meshes,
// or collecting them when c.rawSink is set). `applyUnits` sets c.yUp/c.unitScale from the header --
// true for a file imported on its own, false for a layer read on behalf of a larger stage, whose
// ROOT decides the units.
void parseUsdaText(const std::string& text, Ctx& c, UsdaHeader& hdr, bool applyUnits);

} // namespace aver::fmt::usd_detail
