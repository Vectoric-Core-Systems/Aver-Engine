#include "aver/formats/GltfMaterialCook.hpp"

#include "aver/formats/OcMat.hpp"

#include <filesystem>
#include <fstream>

namespace aver::fmt {
namespace {

// THE BUILT-IN LOOKS, and this is the THIRD copy of this list in the tree. The other two are
// modules/runtime.game/src/GameContent.cpp's `look(...)` table and sandbox/src/SandboxApp.cpp's,
// which that file's own comment already says must be kept in step with each other. A fourth
// dependency edge is not available: Aver.Formats.Material cannot depend on runtime.game, which
// depends on it.
//
// WHY IT MATTERS HERE. An authored .ocmat WINS over a built-in look of the same name, so a
// generated material that happened to be called M_Crate would silently repaint every crate in the
// project -- a scene-wide change with no error and no obvious cause.
constexpr const char* kReservedLookNames[] = {
    "M_Accent", "M_Bark", "M_Concrete", "M_Crate", "M_Floor", "M_Foliage",
    "M_Glass",  "M_Metal", "M_Rock",    "M_Target", "M_Trim",  "M_Wall",
};

bool isReserved(const std::string& stem) {
    for (const char* r : kReservedLookNames) if (stem == r) return true;
    return false;
}

void warn(std::vector<std::string>* out, std::string what) {
    if (out) out->push_back(std::move(what));
}

// Content-relative, forward slashes -- the form a TEX record takes and the form the engine resolves
// against the content root. Never the absolute path: that would bake this machine into the asset.
std::string contentRel(const std::string& sub, const std::string& file) {
    return "Textures/" + sub + "/" + file;
}

} // namespace

bool cookGltfMaterials(const GltfImportResult& res, const GltfMaterialCookOptions& opt,
                       GltfMaterialCookResult& out, std::vector<std::string>* warnings,
                       std::string* err) {
    out.materialSlotNames.assign(res.materials.size(), std::string());
    out.texturePaths.assign(res.images.size(), std::string());

    if (opt.contentDir.empty()) {
        if (err) *err = "cookGltfMaterials: no content directory was given, so there is nowhere to write";
        return false;
    }
    if (res.materials.empty() && res.images.empty()) return true;   // nothing to do is not a failure

    const std::string base = opt.gltfBase.empty() ? std::string("Imported") : opt.gltfBase;
    std::error_code ec;

    // ---- 1. the images ---------------------------------------------------------------------
    // Written first, because a material's TEX record needs the path this produces. An image that
    // could not be read upstream (GltfImage::ok false) is skipped here WITHOUT a second complaint:
    // GltfImport already said which file and why, and repeating it per referencing material would
    // bury the one message that matters.
    const std::filesystem::path texDir =
        std::filesystem::path(opt.contentDir) / "Textures" / base;
    for (usize i = 0; i < res.images.size(); ++i) {
        const GltfImage& img = res.images[i];
        if (!img.ok || img.bytes.empty()) continue;

        std::string stem = img.suggestedName.empty() ? ("image" + std::to_string(i)) : img.suggestedName;
        // Two images in one file may sanitise to the same stem; the index disambiguates rather than
        // one silently overwriting the other.
        for (usize k = 0; k < i; ++k)
            if (!out.texturePaths[k].empty() &&
                out.texturePaths[k] == contentRel(base, stem + img.ext)) {
                stem += "_" + std::to_string(i);
                break;
            }

        const std::filesystem::path dst = texDir / (stem + img.ext);
        std::filesystem::create_directories(texDir, ec);
        if (std::filesystem::exists(dst, ec) && !opt.overwriteExisting) {
            out.texturePaths[i] = contentRel(base, stem + img.ext);
            ++out.skippedExisting;
            continue;   // already there and we were told not to replace it -- still a usable path
        }
        std::ofstream f(dst, std::ios::binary);
        if (!f) {
            warn(warnings, "could not write texture " + dst.string());
            continue;
        }
        f.write(reinterpret_cast<const char*>(img.bytes.data()), std::streamsize(img.bytes.size()));
        if (!f) {
            warn(warnings, "the write of texture " + dst.string() + " did not complete");
            continue;
        }
        out.texturePaths[i] = contentRel(base, stem + img.ext);
        ++out.texturesWritten;
    }

    // ---- 2. the materials ------------------------------------------------------------------
    const std::filesystem::path matDir = std::filesystem::path(opt.contentDir) / "Materials";
    std::filesystem::create_directories(matDir, ec);

    for (usize i = 0; i < res.materials.size(); ++i) {
        const GltfMaterial& gm = res.materials[i];

        pbr::MaterialDesc d;
        d.name = base + "_" + (gm.name.empty() ? ("Material_" + std::to_string(i)) : gm.name);
        for (int k = 0; k < 4; ++k) d.baseColorFactor[k] = gm.baseColorFactor[k];
        for (int k = 0; k < 3; ++k) d.emissiveFactor[k]  = gm.emissiveFactor[k];
        d.metallicFactor    = gm.metallicFactor;
        d.roughnessFactor   = gm.roughnessFactor;
        d.normalScale       = gm.normalScale;
        d.occlusionStrength = gm.occlusionStrength;
        d.alphaCutoff       = gm.alphaCutoff;
        d.twoSided          = gm.doubleSided;
        d.alphaMode = gm.alphaMode == "MASK"  ? pbr::AlphaMode::Mask
                    : gm.alphaMode == "BLEND" ? pbr::AlphaMode::Blend
                                              : pbr::AlphaMode::Opaque;

        // A slot whose image could not be read stays EMPTY. Pointing it at a path that is not there
        // would turn a stated import failure into a material that looks broken for no given reason.
        const auto bind = [&](const GltfMaterial::TexRef& t, pbr::TextureSlot slot) {
            if (t.imageIndex < 0 || usize(t.imageIndex) >= out.texturePaths.size()) return;
            const std::string& p = out.texturePaths[usize(t.imageIndex)];
            if (p.empty()) return;
            d.textures[static_cast<u32>(slot)].path = p;
        };
        bind(gm.baseColorTex,  pbr::TextureSlot::BaseColor);
        bind(gm.metalRoughTex, pbr::TextureSlot::MetalRough);
        bind(gm.normalTex,     pbr::TextureSlot::Normal);
        bind(gm.occlusionTex,  pbr::TextureSlot::Occlusion);
        bind(gm.emissiveTex,   pbr::TextureSlot::Emissive);

        OcMatExtras ex;
        ex.shader = "standard";
        // doubleSided is the ONE glTF fact that has to be said twice: the .ocmat grammar carries it
        // as both a CULL word and a FLAGS bit, and loadOcmat reads them independently.
        ex.cull = gm.doubleSided ? "none" : "back";

        // Sanitise, then avoid the two ways a filename can collide.
        std::string stem;
        for (const char c : d.name) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
            stem.push_back(ok ? c : '_');
        }
        if (stem.empty()) stem = "Material_" + std::to_string(i);
        if (isReserved(stem)) {
            const std::string was = stem;
            stem += "_imported";
            warn(warnings, "material '" + was + "' shares a name with a built-in look; written as '" +
                           stem + "' so it cannot repaint every other use of that name");
        }
        // A stem already claimed by an earlier material in THIS file, or already on disk.
        std::string chosen = stem;
        for (u32 n = 2; n < 1000; ++n) {
            bool taken = false;
            for (usize k = 0; k < i; ++k) if (out.materialSlotNames[k] == chosen) { taken = true; break; }
            if (!taken && !(std::filesystem::exists(matDir / (chosen + ".ocmat"), ec) && !opt.overwriteExisting))
                break;
            if (!taken && opt.overwriteExisting) break;
            chosen = stem + "_" + std::to_string(n);
        }
        if (chosen != stem)
            warn(warnings, "material '" + stem + "' already existed; written as '" + chosen + "'");

        d.name = chosen;   // the file's NAME record and its stem agree, so a reload finds itself
        const std::filesystem::path dst = matDir / (chosen + ".ocmat");
        std::string serr;
        if (!saveOcmat(dst.string(), d, &ex, &serr)) {
            warn(warnings, "could not write material " + dst.string() + ": " + serr);
            continue;
        }
        out.materialSlotNames[i] = chosen;
        ++out.materialsWritten;
    }
    return true;
}

} // namespace aver::fmt
