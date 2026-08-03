#pragma once
// Wavefront OBJ / MTL import into the engine's own formats.
//
// Written from scratch rather than vendored. OBJ is a line-oriented text format with about a dozen
// keywords that matter, and a dependency would cost more in build surface and licence review than
// the parser costs to own -- which is the opposite of the FBX decision next door.
//
// THE BASIS CHANGE IS THE SAME ONE GltfImport USES, deliberately. OBJ has no normative handedness in
// its spec, but every exporter in practice writes the glTF convention: right-handed, +Y up, -Z
// forward, and units the author chose (usually metres). This engine is left-handed, +Z up, +X
// forward, in centimetres. Sharing one convention with the glTF path means a model exported to both
// formats lands in the same place, and means there is one basis change in this codebase to get
// wrong rather than two.
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// One material as an .mtl declared it. Deliberately NOT pbr::MaterialDesc: this header lives in the
// base Aver.Formats target, which cannot see the PBR types -- the same split OcMat.hpp explains.
// The caller decides what to do with these; the importer only reports what the file said.
struct ObjMaterial {
    std::string name;
    f32 baseColor[3] = {1.0f, 1.0f, 1.0f};   // Kd
    f32 specular[3]  = {0.0f, 0.0f, 0.0f};   // Ks
    f32 emissive[3]  = {0.0f, 0.0f, 0.0f};   // Ke
    f32 shininess    = 0.0f;                 // Ns, 0..1000
    f32 opacity      = 1.0f;                 // d, or 1-Tr
    f32 ior          = 1.0f;                 // Ni

    // PBR extensions (map_Pr / map_Pm / Pr / Pm). Not in the 1995 spec, but written by Blender,
    // Substance and most modern exporters, and the engine is a PBR renderer -- ignoring them would
    // throw away the only roughness the file has.
    f32 roughness = 1.0f;                    // Pr
    f32 metallic  = 0.0f;                    // Pm
    bool hasPbr   = false;                   // true when Pr/Pm/map_Pr/map_Pm appeared

    // Texture paths EXACTLY as the .mtl wrote them, unresolved. Resolution is the caller's job
    // because only the caller knows the project's content root, and an importer that silently
    // rewrote a path would hide the absolute-path bug docs/PACKAGING.md §116 exists to catch.
    std::string mapBaseColor;                // map_Kd
    std::string mapNormal;                   // map_Bump / bump / norm
    std::string mapRoughness;                // map_Pr / map_Ns
    std::string mapMetallic;                 // map_Pm
    std::string mapEmissive;                 // map_Ke
    std::string mapOpacity;                  // map_d
    std::string mapAmbientOcclusion;         // map_Ka, which is what most exporters use for AO
};

// What an import produced, and what it had to drop.
struct ObjImportResult {
    // One per `o` object, or one for the whole file when it declares none. Submeshes within a mesh
    // are the `usemtl` runs, which is what maps onto the engine's material slots.
    std::vector<OcMeshData>  meshes;
    std::vector<std::string> meshNames;       // parallel to `meshes`; "" where the source had none

    std::vector<ObjMaterial> materials;       // every material from every `mtllib`, in file order
    std::vector<std::string> materialLibs;    // the .mtl names referenced, as written

    std::vector<std::string> unsupported;     // features the file used and this importer cannot carry
};

// Knobs for the conversion. The defaults match GltfImportOptions so the two importers agree.
struct ObjImportOptions {
    f32  scale = 100.0f;                     // source units to centimetres; OBJ is usually metres
    bool convertAxes = true;                 // the basis change and the winding reversal
    bool generateMissingNormals = true;      // flat normals when the source has none
    bool splitByObject = true;               // `o` starts a new mesh; false merges the file into one

    // Load the .mtl files named by `mtllib`. Off means `materials` stays empty and only the slot
    // NAMES survive, which is what a caller wants when it already has its own materials by name.
    bool loadMaterialLibs = true;
};

// Imports a .obj from disk. `why` is set on failure. Sibling .mtl files resolve against the .obj's
// own directory, which is what every exporter assumes and what `mtllib` names are relative to.
bool importObj(const std::string& path, ObjImportResult& out,
               const ObjImportOptions& opt = {}, std::string* why = nullptr);

// The same from memory. `baseDir` resolves `mtllib` references; empty refuses them rather than
// searching the working directory, because a content pipeline that reads whatever happens to be in
// the cwd is a reproducibility bug waiting to happen.
bool importObjFromMemory(const char* text, usize size, const std::string& baseDir,
                         ObjImportResult& out, const ObjImportOptions& opt = {},
                         std::string* why = nullptr);

// Parses a .mtl on its own, for a caller that wants the materials without the geometry.
bool importMtl(const std::string& path, std::vector<ObjMaterial>& out, std::string* why = nullptr);

} // namespace aver::fmt
