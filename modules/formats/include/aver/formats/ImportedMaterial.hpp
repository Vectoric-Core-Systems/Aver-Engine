#pragma once
// The ONE material shape every importer produces and the material cook consumes.
//
// WHY THIS EXISTS. Three importers had arrived at three answers to the same question. ObjImport
// grew ObjMaterial, GltfImport grew GltfMaterial, and USD was about to grow a third -- each with the
// identical header comment explaining that it is "deliberately NOT pbr::MaterialDesc" because it
// lives in the PBR-free base target. Three structs, one rationale, and a cook per importer would
// have meant a fourth copy of the reserved-look-name list as well. This is that shape, once.
//
// STILL NOT pbr::MaterialDesc, for the original reason: GltfImport.cpp, ObjImport.cpp and
// UsdImport.cpp all live in the base Aver.Formats target, which has no PBR dependency so that a
// headless mesh tool does not drag the renderer family in. Translating to MaterialDesc happens in
// the AVER_MODULE_PBR-gated Aver.Formats.Material target -- see MaterialCook.hpp.
//
// THE DEFAULTS ARE glTF'S, and they are not the obvious ones: metallicFactor and roughnessFactor are
// both 1.0, so a material that states neither is a fully rough METAL. They match pbr::MaterialDesc's
// own defaults, whose header says outright that they are glTF's. An importer whose format has
// different defaults -- .mtl's Phong model, say -- must write every field explicitly rather than
// leaning on these.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// An image a source file carried, kept as its ENCODED bytes -- a PNG stays a PNG, a JPEG stays a
// JPEG. Nothing here decodes or re-encodes: a round trip through a codec changes bytes the author is
// entitled to get back, and costs quality on a JPEG for nothing.
struct ImportedImage {
    std::vector<u8> bytes;      // the encoded file, verbatim
    std::string ext;            // ".png" or ".jpeg", from a mimeType or the URI
    std::string suggestedName;  // sanitised stem for the file this becomes; never a path
    std::string sourcePath;     // where the source said it was, for diagnostics; may be empty
    bool ok = false;            // false when it could not be read -- the importer says why, by name
};

// Which image a slot uses. An image the importer could not read leaves this UNSET, so a material
// never carries a texture reference to a file that is not there -- that reads as a renderer fault
// rather than the import failure it is.
struct ImportedTexture {
    i32 imageIndex = -1;   // index into the parallel images array; -1 = unset
    u32 texCoord   = 0;    // always 0 in practice: OcMeshData carries one UV stream

    // WHICH CHANNEL OF THAT IMAGE THE SOURCE ASKED FOR: 'r', 'g', 'b', 'a', or 0 for "the colour
    // channels" / unstated. Only a single-value slot can honour it -- an opacity map is one number
    // per texel and the file has to say which one.
    //
    // WHY IT IS NOT COSMETIC. USD routinely connects opacity and diffuseColor to the SAME
    // UsdUVTexture prim, differing only by `.outputs:a` versus `.outputs:rgb`. With the channel
    // dropped, the two slots become indistinguishable and the opacity fold reads the base colour's
    // RED channel as if it were a mask -- which on green foliage yields nearly transparent leaves.
    char channel = 0;

    bool empty() const { return imageIndex < 0; }
};

// One material, flattened to the subset this engine can express.
struct ImportedMaterial {
    // Kept so existing code written against GltfMaterial::TexRef still names a type that exists.
    using TexRef = ImportedTexture;

    std::string name;
    f32  baseColorFactor[4] = {1, 1, 1, 1};
    // WHAT ENCODING baseColorFactor's RGB IS IN, because the formats disagree and the .ocmat does not.
    // An .ocmat (pbr::MaterialDesc) holds it sRGB-ENCODED -- packMaterial decodes it with pow(x, 2.2),
    // the same as a colour picked in the editor -- while glTF's baseColorFactor and UsdPreviewSurface's
    // diffuseColor are LINEAR; .mtl states no colour space and its Kd is treated as sRGB. The importer records what
    // its file wrote; the cook (MaterialCook.cpp) encodes a linear factor on the way into the .ocmat.
    // Copied across as it was, a linear 0.5 grey rendered at 0.5^2.2 = 0.22. Alpha is linear coverage
    // in every format and is never converted. The default is glTF's, like every other default here.
    bool baseColorFactorLinear = true;
    f32  emissiveFactor[3]  = {0, 0, 0};
    f32  metallicFactor     = 1.0f;    // glTF default is ONE, not zero
    f32  roughnessFactor    = 1.0f;    // likewise
    f32  normalScale        = 1.0f;
    f32  occlusionStrength  = 1.0f;
    f32  alphaCutoff        = 0.5f;    // only meaningful when alphaMode is MASK
    std::string alphaMode   = "OPAQUE";
    bool doubleSided        = false;

    ImportedTexture baseColorTex, metalRoughTex, normalTex, occlusionTex, emissiveTex;

    // A SEPARATE opacity map, which .ocmat has no slot for: cutout there is the base colour's alpha
    // channel, exactly as glTF defines it. USD and .mtl both name opacity as its own file
    // (`inputs:opacity.connect` to a UsdUVTexture, `map_d`), and Intel's Jungle Ruins trees are
    // authored that way -- a JPEG albedo, which cannot carry alpha at all, plus a greyscale
    // `*_opacity.jpg` beside it.
    //
    // RECORDED HERE, FOLDED IN LATER. This layer states what the file said; it does not decode. The
    // fold into baseColorTex's alpha happens in the import policy layer, where a decoder and an
    // encoder already are (mergeOpacityMaps, ImportCook.cpp) -- the same split that keeps --max-texture out of the
    // cook. A material still carrying this by the time it reaches cookMaterials has an opacity map
    // that could not be folded, and the cook ignores it rather than writing a file nothing samples.
    ImportedTexture opacityTex;

    // Subsurface (thin-leaf translucency), in pbr::MaterialDesc's terms; weight 0 = off.
    f32 subsurfaceWeight    = 0.0f;
    f32 subsurfaceRadius    = 0.0f;
    f32 subsurfaceColor[3]  = {1.0f, 1.0f, 1.0f};
    // A TRANSLUCENCY map (grey, R = how much light passes through). .ocmat has no slot for one, so the
    // cook reduces it to subsurfaceWeight -- its mean over the texels the cutout keeps -- and does not
    // write the file. UsdPreviewSurface cannot name one at all: the USD importer finds it on disk
    // beside the base colour (`<stem>_Translucency.*`), where Intel's Jungle Ruins keeps the maps its
    // Blender materials feed into Transmission; without them every leaf is opaque against the sun.
    ImportedTexture translucencyTex;
};

} // namespace aver::fmt
