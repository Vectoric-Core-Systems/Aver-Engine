#include "aver/formats/ImportCook.hpp"

#include "aver/formats/MaterialCook.hpp"
#include "aver/formats/Texture.hpp"
#include "aver/platform/Image.hpp"
#include "aver/core/Log.hpp"

// Declarations only -- see the CMakeLists.txt comment on Aver.Formats.Material's stb include
// directory. The IMPLEMENTATION is compiled exactly once per final executable, by whichever TU
// already owns it there (AverAssetC.cpp for AverAssetC.exe, SandboxApp.cpp for the editor): this file
// is a static-library TU that several different executables link, and defining
// STB_IMAGE_WRITE_IMPLEMENTATION here would duplicate whichever definition that executable already
// has the moment both object files reach the same link.
#include "stb_image_write.h"

#include <cstdio>

namespace aver::fmt {
namespace {

// ---- opacity folding and the texture-size cap, ported from AverAssetC.cpp unchanged ----------------

// How a texture is going to be READ, which is what decides how it may be filtered. Colour has to be
// averaged in linear space, a normal map as vectors and then renormalised, and data (roughness,
// metal, occlusion) straight -- getting this wrong does not fail, it just shades subtly wrong, which
// is the worst kind of wrong to ship.
enum class TexRole { Colour, Data, Normal };

// Derives each image's role from the SLOTS that reference it. An image nothing references keeps the
// conservative default (Data, filtered straight): it is about to be dropped by the cook anyway, and
// guessing sRGB on an unreferenced file would only matter if that guess were wrong.
std::vector<TexRole> imageRoles(const std::vector<ImportedMaterial>& materials, usize imageCount) {
    std::vector<TexRole> roles(imageCount, TexRole::Data);
    const auto mark = [&](const ImportedTexture& t, TexRole r) {
        if (t.imageIndex >= 0 && usize(t.imageIndex) < roles.size()) roles[usize(t.imageIndex)] = r;
    };
    for (const ImportedMaterial& m : materials) {
        mark(m.metalRoughTex, TexRole::Data);
        mark(m.occlusionTex,  TexRole::Data);
        mark(m.normalTex,     TexRole::Normal);
        // Colour LAST so that an image serving as both -- an ORM map also wired to base colour, which
        // an exporter can produce -- ends up filtered as colour, the reading that is visible.
        mark(m.baseColorTex,  TexRole::Colour);
        mark(m.emissiveTex,   TexRole::Colour);
    }
    return roles;
}

// Folds a material's SEPARATE opacity map into its base colour's alpha channel, which is the only
// place .ocmat can express cutout.
//
// WHY THIS IS NEEDED AT ALL. glTF puts alpha in the base colour and the engine followed it, but USD
// and .mtl both name opacity as its own file, and Intel's Jungle Ruins trees are authored that way:
// a JPEG albedo -- which cannot carry an alpha channel at all -- plus a greyscale *_opacity.jpg
// beside it. Dropping that map does not fail; it renders every leaf as a solid quad, and the tree
// looks like a bush made of cardboard.
//
// WHY HERE. Same reason as the size cap below: the cook writes the bytes it is handed and has no
// decoder. This one does, and needs one, because folding two encoded images together means decoding
// both.
//
// THE RESULT IS ALWAYS A NEW IMAGE, never a write into the shared base colour: two materials may
// share one albedo and have different opacity maps -- which is exactly how a tree's leaves and its
// dead leaves are authored -- and mutating the shared one would give the second material the first
// one's cutout.
bool foldOpacityInto(std::vector<ImportedImage>& images, ImportedMaterial& m, std::string* note) {
    if (m.opacityTex.empty()) return false;
    const usize oi = usize(m.opacityTex.imageIndex);
    if (oi >= images.size() || !images[oi].ok || images[oi].bytes.empty()) return false;
    if (m.baseColorTex.empty()) {
        // Nothing to fold INTO. Leaving the slot alone is right: an opacity map with no base colour
        // has no channel to live in, and inventing a white base colour to carry it would put a
        // texture on a material the source never gave one.
        if (note) *note = "'" + m.name + "' has an opacity map but no base colour to fold it into; "
                          "the cutout is lost";
        m.opacityTex = {};
        return false;
    }
    const usize bi = usize(m.baseColorTex.imageIndex);
    if (bi >= images.size() || !images[bi].ok || images[bi].bytes.empty()) return false;

    ImageData base, mask;
    std::string derr;
    if (!decodeImage(images[bi].bytes.data(), images[bi].bytes.size(), base, &derr) ||
        !decodeImage(images[oi].bytes.data(), images[oi].bytes.size(), mask, &derr)) {
        if (note) *note = "'" + m.name + "' could not fold its opacity map: " + derr;
        return false;
    }
    if (mask.width != base.width || mask.height != base.height) {
        // A resample would be easy and is deliberately not done: differing sizes mean the two maps
        // were not authored against the same UV layout, and stretching one to fit is a guess that
        // would show up as a cutout that does not follow the leaf.
        if (note) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "'%s' opacity map is %ux%u but its base colour is %ux%u; not folded",
                          m.name.c_str(), mask.width, mask.height, base.width, base.height);
            *note = buf;
        }
        return false;
    }

    // THE CHANNEL THE SOURCE NAMED, not a fixed one.
    //
    // This used to be hardcoded to RED, on the reasoning that `inputs:opacity.connect` names
    // `.outputs:r` and .mtl's map_d is greyscale. That holds for a genuinely separate greyscale
    // mask and is wrong the moment a file names a different channel -- and USD files routinely
    // name `.outputs:a`. Reading red out of an image whose mask lives in alpha produces a cutout
    // shaped like the picture's brightness, which on foliage is close to the worst possible answer.
    //
    // Averaging RGB is still not offered: it would differ on a map that is not actually grey, and
    // would differ from what the authoring tool showed.
    const usize lane = (m.opacityTex.channel == 'g')   ? 1
                     : (m.opacityTex.channel == 'b')   ? 2
                     : (m.opacityTex.channel == 'a')   ? 3
                                                       : 0;   // 'r' and unstated
    const usize n = usize(base.width) * base.height;
    for (usize i = 0; i < n; ++i) base.pixels[i * 4 + 3] = mask.pixels[i * 4 + lane];

    std::vector<u8> encoded;
    const auto sink = [](void* ctx, void* data, int len) {
        auto* v = static_cast<std::vector<u8>*>(ctx);
        const u8* p = static_cast<const u8*>(data);
        v->insert(v->end(), p, p + len);
    };
    if (!stbi_write_png_to_func(sink, &encoded, static_cast<int>(base.width),
                                static_cast<int>(base.height), 4, base.pixels.data(),
                                static_cast<int>(base.width * 4)) || encoded.empty()) {
        if (note) *note = "'" + m.name + "' opacity fold failed to re-encode";
        return false;
    }

    ImportedImage merged;
    merged.bytes = std::move(encoded);
    merged.ext = ".png";                        // PNG because it has to carry alpha; a JPEG cannot
    merged.suggestedName = images[bi].suggestedName + "_cut";
    // NOT the base colour's own path: that is the file this one replaced, and reusing it makes every
    // later diagnostic -- the size cap's line, most visibly -- name a file that is no longer what is
    // being written.
    merged.sourcePath = images[bi].suggestedName + " + " + images[oi].suggestedName + " (merged)";
    merged.ok = true;
    if (note) *note = "'" + m.name + "' folded " +
                      (images[oi].sourcePath.empty() ? images[oi].suggestedName : images[oi].sourcePath) +
                      " into the alpha of " + merged.suggestedName;

    images.push_back(std::move(merged));
    m.baseColorTex.imageIndex = i32(images.size() - 1);
    m.opacityTex = {};                          // consumed; the cook must not see it as unhandled
    if (m.alphaMode == "OPAQUE") m.alphaMode = "MASK";
    return true;
}

// Applies the fold across a whole import. Runs BEFORE the size cap so the cap sees, and shrinks, the
// merged image rather than the original the merge replaced.
void mergeOpacityMaps(std::vector<ImportedImage>& images, std::vector<ImportedMaterial>& materials) {
    u32 folded = 0, lost = 0;
    for (ImportedMaterial& m : materials) {
        if (m.opacityTex.empty()) continue;
        std::string note;
        const bool ok = foldOpacityInto(images, m, &note);
        if (!note.empty()) {
            if (ok) AVER_INFO("opacity {}", note);
            else    AVER_WARN("opacity {}", note);
        }
        if (ok) ++folded; else ++lost;
        m.opacityTex = {};   // whatever happened, it is not the cook's business
    }
    if (folded) AVER_INFO("folded {} separate opacity map(s) into base-colour alpha", folded);
    if (lost)   AVER_WARN("{} material(s) had an opacity map that could NOT be folded; their cutout "
                          "is lost and they will render solid", lost);
}

// One image, halved until neither side exceeds `cap`, and re-encoded as PNG. Returns false and
// leaves `img` untouched when it is already small enough, cannot be decoded, or would not re-encode.
//
// WHY HERE AND NOT THE COOK. cookMaterials' contract is that it writes the bytes it is given; it has
// no decoder and should not grow one, because every caller that wants verbatim bytes would then be
// paying for a decode it does not want. A size cap is a POLICY about a particular import, which is
// this layer's business -- and this is where the decoder and the PNG writer already are.
//
// THE FILTER IS THE ENGINE'S OWN. generateMipChain is the function that builds the mip chain the
// renderer samples, so capping through it means a texture imported at 2048 is bit-identical to mip 1
// of the same texture imported at 4096. A second box filter here would drift from that.
bool capImageSize(ImportedImage& img, u32 cap, TexRole role, std::string* note) {
    if (!img.ok || img.bytes.empty() || cap == 0) return false;

    ImageData src;
    std::string derr;
    if (!decodeImage(img.bytes.data(), img.bytes.size(), src, &derr)) {
        // NOT an error: the undecodable-container check in the cook reports these by name, and a
        // format this build cannot read is not one the cap can do anything about either way.
        return false;
    }
    if (src.width <= cap && src.height <= cap) return false;

    TextureData t;
    t.width  = src.width;
    t.height = src.height;
    t.srgb   = (role == TexRole::Colour);
    t.levels.push_back(std::move(src));
    generateMipChain(t, role == TexRole::Normal);

    usize level = 0;
    while (level + 1 < t.levels.size() &&
           (t.levels[level].width > cap || t.levels[level].height > cap))
        ++level;
    const ImageData& out = t.levels[level];
    if (!out.valid()) return false;

    // stbi_write_png_to_func rather than the file form: the cook still owns where this lands, and a
    // temporary file here would be a second place for an import to fail.
    std::vector<u8> encoded;
    const auto sink = [](void* ctx, void* data, int len) {
        auto* v = static_cast<std::vector<u8>*>(ctx);
        const u8* p = static_cast<const u8*>(data);
        v->insert(v->end(), p, p + len);
    };
    if (!stbi_write_png_to_func(sink, &encoded, static_cast<int>(out.width),
                                static_cast<int>(out.height), 4, out.pixels.data(),
                                static_cast<int>(out.width * 4)) || encoded.empty())
        return false;

    if (note) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s: %ux%u -> %ux%u (%.1f MB -> %.1f MB)",
                      (img.sourcePath.empty() ? img.suggestedName : img.sourcePath).c_str(),
                      t.levels[0].width, t.levels[0].height, out.width, out.height,
                      double(img.bytes.size()) / 1e6, double(encoded.size()) / 1e6);
        *note = buf;
    }
    img.bytes = std::move(encoded);
    // PNG NO MATTER WHAT WENT IN. A JPEG re-encoded as a JPEG would compound its own artefacts, and
    // the extension has to follow the bytes or the engine's decoder is handed a lie.
    img.ext = ".png";
    return true;
}

// Applies the cap across a whole import, in place, and says what it did.
void capImageSizes(std::vector<ImportedImage>& images, const std::vector<ImportedMaterial>& materials,
                   u32 cap) {
    if (cap == 0 || images.empty()) return;
    const std::vector<TexRole> roles = imageRoles(materials, images.size());
    u32 changed = 0;
    for (usize i = 0; i < images.size(); ++i) {
        std::string note;
        if (capImageSize(images[i], cap, roles[i], &note)) {
            AVER_INFO("texture cap {}", note);
            ++changed;
        }
    }
    if (changed) AVER_INFO("capped {} texture(s) at {}px", changed, cap);
}

} // namespace

// ---- the shared cook entry point ---------------------------------------------------------------

bool cookImportedMaterials(std::vector<ImportedMaterial>& materials, std::vector<ImportedImage>& images,
                           const std::string& contentDir, const std::string& assetBase,
                           std::vector<OcMeshData>& meshes, u32 maxTexture, bool overwriteExisting,
                           std::vector<std::string>* warnings, std::string* error,
                           u32* materialsWritten, u32* texturesWritten) {
    if (materialsWritten) *materialsWritten = 0;
    if (texturesWritten)  *texturesWritten  = 0;
    if (materials.empty() && images.empty()) return true;   // nothing to do is not a failure

    // BEFORE the cook, because the cook writes the bytes it is given. Capping afterwards would mean
    // writing the full-size file and then a second one beside it. The opacity fold goes first so the
    // cap shrinks the MERGED image rather than the original it replaced.
    mergeOpacityMaps(images, materials);
    capImageSizes(images, materials, maxTexture);

    MaterialCookOptions copt;
    copt.contentDir         = contentDir;
    copt.assetBase          = assetBase;
    copt.overwriteExisting  = overwriteExisting;
    MaterialCookResult cres;
    if (!cookMaterials(materials, images, copt, cres, warnings, error)) return false;

    for (usize i = 0; i < materials.size() && i < cres.materialSlotNames.size(); ++i) {
        if (cres.materialSlotNames[i].empty()) continue;   // did not cook; keep the old name
        const std::string& from = materials[i].name;
        const std::string& to   = cres.materialSlotNames[i];
        for (OcMeshData& m : meshes)
            for (std::string& slot : m.materialSlots)
                if (slot == from) slot = to;
    }

    if (materialsWritten) *materialsWritten = cres.materialsWritten;
    if (texturesWritten)  *texturesWritten  = cres.texturesWritten;
    return true;
}

} // namespace aver::fmt
