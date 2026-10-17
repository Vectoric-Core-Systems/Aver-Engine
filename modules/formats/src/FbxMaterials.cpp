// FBX materials and textures -> ImportedMaterial / ImportedImage. See docs/plans/wave9/FBX_IMPORT.md 2.4.
//
// ONE READER FOR EVERY SHADER. ufbx synthesises ufbx_material::pbr from Lambert/Phong, Arnold, 3ds Max,
// glTF-material and OSL shaders, so this reads pbr.* and nothing shader specific, except where the FBX
// builtin models (Lambert/Phong) say less than the PBR slots need.
#include "FbxInternal.hpp"

#if AVER_HAVE_UFBX

#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace aver::fmt::fbx {
namespace {

namespace fs = std::filesystem;

bool isImageExt(const std::string& lowerExt) {
    static const char* const kExts[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tif", ".tiff",
                                        ".dds", ".hdr", ".exr", ".psd", ".gif", ".webp"};
    for (const char* e : kExts) if (lowerExt == e) return true;
    return false;
}

// The last path component of a path written on any platform.
std::string leafOf(std::string p) {
    for (char& ch : p) if (ch == '\\') ch = '/';
    const usize slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string extOfName(const std::string& name) {
    const usize dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : lowerOf(name.substr(dot));
}

std::string stemOfName(const std::string& name) {
    const usize dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool readFileBytes(const fs::path& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize(static_cast<usize>(n));
    f.seekg(0);
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return bool(f);
}

// ".png" or ".jpeg" from the first bytes, else "".
std::string sniffExt(const std::vector<u8>& b) {
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return ".png";
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return ".jpeg";
    if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M') return ".bmp";
    if (b.size() >= 4 && b[0] == 'D' && b[1] == 'D' && b[2] == 'S' && b[3] == ' ') return ".dds";
    return std::string();
}

// Does `p`, normalised, stay under the FBX folder's PARENT? A model may reach a sibling folder
// ("../textures/x.png", which Maya projects do) but not wander the disk: the file is untrusted input.
bool confined(const fs::path& p, const fs::path& parent) {
    const fs::path rel = p.lexically_normal().lexically_relative(parent);
    if (rel.empty()) return false;
    return *rel.begin() != "..";
}

// Case-insensitive lookup of `leaf` inside `dir` (a texture authored on Windows, read on Linux/macOS).
bool findIgnoringCase(const fs::path& dir, const std::string& leaf, fs::path& found) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    const std::string want = lowerOf(leaf);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (lowerOf(it->path().filename().string()) != want) continue;
        if (fs::is_regular_file(it->path(), ec)) { found = it->path(); return true; }
    }
    return false;
}

class ImageStore {
public:
    ImageStore(Ctx& c, const std::string& stem) : c_(c), stem_(stem) {}

    // The index into out->images for this texture element, or -1 when its file could not be read.
    i32 get(const ufbx_texture* t) {
        std::string key = t->has_file ? ("file:" + std::to_string(t->file_index)) : ("tex:" + std::to_string(t->element_id));
        const auto hit = byKey_.find(key);
        if (hit != byKey_.end()) return hit->second;
        ImportedImage img;
        const bool ok = load(t, img, key);
        i32 idx = -1;
        if (ok) {
            // A second element naming the same resolved file shares it.
            const auto same = byPath_.find(img.sourcePath);
            if (!img.sourcePath.empty() && same != byPath_.end()) {
                idx = same->second;
            } else {
                c_.out->images.push_back(std::move(img));
                idx = static_cast<i32>(c_.out->images.size() - 1);
                if (!c_.out->images.back().sourcePath.empty()) byPath_[c_.out->images.back().sourcePath] = idx;
            }
        }
        byKey_[key] = idx;
        return idx;
    }

    // Counts of what was found, for the report.
    u32 embedded = 0, external = 0, missing = 0;

private:
    Ctx& c_;
    std::string stem_;   // the .fbx file's stem, for the <stem>.fbm folder Autodesk tools extract to
    std::unordered_map<std::string, i32> byKey_;
    std::unordered_map<std::string, i32> byPath_;

    bool load(const ufbx_texture* t, ImportedImage& img, const std::string& key) {
        const std::string abs = str(t->absolute_filename);
        const std::string rel = str(t->relative_filename);
        const std::string fn = str(t->filename);
        const std::string named = !rel.empty() ? rel : (!abs.empty() ? abs : fn);
        std::string leaf = leafOf(named);

        // 1. Embedded: the texture's own blob, the video it names, or the scene's file entry.
        const ufbx_blob* content = nullptr;
        if (t->content.size > 0 && t->content.data) content = &t->content;
        else if (t->video && t->video->content.size > 0 && t->video->content.data) content = &t->video->content;
        else if (t->has_file && t->file_index < c_.scene->texture_files.count) {
            const ufbx_texture_file& tf = c_.scene->texture_files.data[t->file_index];
            if (tf.content.size > 0 && tf.content.data) content = &tf.content;
        }
        if (content) {
            const u8* p = static_cast<const u8*>(content->data);
            img.bytes.assign(p, p + content->size);
            std::string ext = extOfName(leaf);
            if (!isImageExt(ext)) ext = sniffExt(img.bytes);
            if (ext.empty()) ext = ".png";
            img.ext = ext;
            const std::string stem = sanitizeStem(stemOfName(leaf));
            img.suggestedName = stem.empty() ? ("texture" + std::to_string(t->element_id)) : stem;
            img.sourcePath = "embedded:" + key + (leaf.empty() ? std::string() : (":" + leaf));
            img.ok = true;
            ++embedded;
            return true;
        }

        // 2. External.
        if (leaf.empty()) {
            c_.note("a texture with no file name and no embedded data (" + str(t->name) + ")");
            ++missing;
            return false;
        }
        fs::path found;
        std::string refused;
        if (findExternal(abs, rel, leaf, found, refused) && readFileBytes(found, img.bytes)) {
            std::string ext = extOfName(found.filename().string());
            if (ext.empty()) ext = sniffExt(img.bytes);
            if (ext.empty()) ext = ".png";
            img.ext = ext;
            const std::string stem = sanitizeStem(stemOfName(found.filename().string()));
            img.suggestedName = stem.empty() ? ("texture" + std::to_string(t->element_id)) : stem;
            img.sourcePath = found.generic_string();
            img.ok = true;
            ++external;
            return true;
        }
        if (!refused.empty()) {
            c_.note("a texture path that leaves the model's folder tree was refused: '" + refused + "'");
            c_.line("texture refused (outside the model folder's parent): " + refused);
        } else {
            c_.note("an image the file names but does not ship: '" + leaf + "'");
            c_.line("texture missing: '" + leaf + "' (searched next to the model, textures/, <name>.fbm/)");
        }
        ++missing;
        return false;
    }

    bool findExternal(const std::string& abs, const std::string& rel, const std::string& leaf, fs::path& found,
                      std::string& refused) const {
        std::error_code ec;
        const auto usable = [&](const fs::path& p) {
            return fs::is_regular_file(p, ec) && isImageExt(extOfName(p.filename().string()));
        };
        // The absolute path the file was authored with: tried as written, but only ever as an image.
        if (!abs.empty()) {
            const fs::path p(abs);
            if (usable(p)) { found = p; return true; }
        }
        if (c_.baseDir.empty()) return false;

        fs::path dir = fs::absolute(fs::path(c_.baseDir), ec).lexically_normal();
        if (!dir.has_filename()) dir = dir.parent_path();
        const fs::path parent = dir.parent_path();

        if (!rel.empty()) {
            const fs::path r(rel);
            if (r.is_absolute()) {
                if (usable(r)) { found = r; return true; }
            } else {
                const fs::path p = dir / r;
                if (!confined(p, parent)) refused = rel;
                else if (usable(p)) { found = p.lexically_normal(); return true; }
            }
        }
        const fs::path leafP(leaf);
        std::vector<fs::path> dirs = {dir, dir / "textures"};
        if (!stem_.empty()) dirs.push_back(dir / (stem_ + ".fbm"));
        for (const fs::path& d : dirs) {
            const fs::path p = d / leafP;
            if (usable(p)) { found = p; return true; }
        }
        for (const fs::path& d : dirs)
            if (findIgnoringCase(d, leaf, found) && usable(found)) return true;
        return false;
    }
};

// The shaders whose base colour is a genuine tint over the base colour texture. FBX Lambert/Phong keep a
// default diffuse colour (0.8 grey) even with a texture on it, and every DCC ignores it then.
bool colourTintsTexture(ufbx_shader_type t) {
    switch (t) {
        case UFBX_SHADER_FBX_LAMBERT: case UFBX_SHADER_FBX_PHONG: case UFBX_SHADER_BLENDER_PHONG:
        case UFBX_SHADER_UNKNOWN: case UFBX_SHADER_WAVEFRONT_MTL: return false;
        default: return true;
    }
}

bool isFbxBuiltin(ufbx_shader_type t) {
    return t == UFBX_SHADER_FBX_LAMBERT || t == UFBX_SHADER_FBX_PHONG || t == UFBX_SHADER_UNKNOWN;
}

f32 clamp01(f64 v) { return f32(std::min(1.0, std::max(0.0, v))); }

class MaterialReader {
public:
    MaterialReader(Ctx& c, ImageStore& images) : c_(c), images_(images) {}

    void convert(const ufbx_material* um, ImportedMaterial& m) {
        const auto& pbr = um->pbr;
        const ufbx_shader_type shader = um->shader_type;
        m.name = c_.materialName[um->typed_id];
        m.baseColorFactorLinear = c_.opt->fileColoursAreLinear;
        name_ = m.name;

        // ---- base colour ----
        m.baseColorTex = tex(pbr.base_color, "base colour", 0);
        f32 rgb[3] = {1, 1, 1};
        if (pbr.base_color.has_value && (m.baseColorTex.empty() || colourTintsTexture(shader))) {
            rgb[0] = f32(pbr.base_color.value_vec4.x); rgb[1] = f32(pbr.base_color.value_vec4.y); rgb[2] = f32(pbr.base_color.value_vec4.z);
        }
        const f32 factor = pbr.base_factor.has_value ? f32(pbr.base_factor.value_real) : 1.0f;
        for (int k = 0; k < 3; ++k) m.baseColorFactor[k] = std::max(0.0f, rgb[k] * factor);

        // ---- opacity ----
        f32 opacity = 1.0f;
        if (pbr.opacity.has_value) {
            opacity = clamp01(pbr.opacity.value_real);
        } else if (isFbxBuiltin(shader) && um->fbx.transparency_factor.has_value) {
            opacity = clamp01(1.0 - um->fbx.transparency_factor.value_real);
        }
        m.baseColorFactor[3] = opacity;
        m.opacityTex = tex(pbr.opacity, "opacity", 0);
        if (!m.opacityTex.empty()) {
            m.alphaMode = "MASK";                                           // the USD rule: cutout, folded into base colour alpha
            if (!m.baseColorTex.empty() && m.opacityTex.imageIndex == m.baseColorTex.imageIndex)
                m.opacityTex = {};                                          // the colour map's own alpha already is the mask
        } else if (opacity < 1.0f) {
            m.alphaMode = "BLEND";
        }

        // ---- roughness and metalness ----
        // A texture makes the factor 1 (it scales the map). A Lambert/Phong material with no PBR term
        // is a dielectric; its Phong exponent, when it has one, gives the roughness (the OBJ rule).
        m.metalRoughTex = tex(pbr.roughness, "roughness", 'r');
        if (!m.metalRoughTex.empty()) {
            m.roughnessFactor = 1.0f;
        } else if (isFbxBuiltin(shader) && um->fbx.specular_exponent.has_value && um->fbx.specular_exponent.value_real > 0.0) {
            // The OBJ rule, and on purpose not ufbx's own Shininess -> roughness curve: a Phong exponent is the same
            // quantity in both formats, and one conversion for both keeps an FBX and an OBJ of one asset alike.
            m.roughnessFactor = std::sqrt(2.0f / (f32(um->fbx.specular_exponent.value_real) + 2.0f));
        } else if (pbr.roughness.has_value) {
            m.roughnessFactor = clamp01(pbr.roughness.value_real);
        } else {
            m.roughnessFactor = 1.0f;
        }
        m.metallicTex = tex(pbr.metalness, "metalness", 'r');
        if (!m.metallicTex.empty()) m.metallicFactor = 1.0f;
        else if (pbr.metalness.has_value) m.metallicFactor = clamp01(pbr.metalness.value_real);
        else m.metallicFactor = 0.0f;                                       // NOT glTF's 1: an FBX with no metalness is a dielectric
        if (!m.metalRoughTex.empty() && m.metallicTex.empty() && m.metallicFactor > 0.0f)
            c_.note("a metalness factor on a material whose roughness is a texture (the map's blue channel also scales metalness): " + name_);

        // ---- normal, emission, occlusion ----
        m.normalTex = tex(pbr.normal_map, "normal", 0);
        if (m.normalTex.empty() && um->fbx.bump.texture && um->fbx.bump.texture_enabled)
            c_.note("bump/height maps are not converted to normal maps (" + name_ + ")");
        m.normalScale = 1.0f;
        m.emissiveTex = tex(pbr.emission_color, "emission", 0);
        if (pbr.emission_color.has_value) {
            const f32 ef = pbr.emission_factor.has_value ? f32(pbr.emission_factor.value_real) : 1.0f;
            m.emissiveFactor[0] = std::max(0.0f, f32(pbr.emission_color.value_vec4.x) * ef);
            m.emissiveFactor[1] = std::max(0.0f, f32(pbr.emission_color.value_vec4.y) * ef);
            m.emissiveFactor[2] = std::max(0.0f, f32(pbr.emission_color.value_vec4.z) * ef);
        }
        if (!m.emissiveTex.empty() && !pbr.emission_color.has_value) { m.emissiveFactor[0] = m.emissiveFactor[1] = m.emissiveFactor[2] = 1.0f; }
        else if (!m.emissiveTex.empty() && m.emissiveFactor[0] + m.emissiveFactor[1] + m.emissiveFactor[2] <= 0.0f)
            m.emissiveFactor[0] = m.emissiveFactor[1] = m.emissiveFactor[2] = 1.0f;     // a map on a black emitter would show nothing
        m.occlusionTex = tex(pbr.ambient_occlusion, "ambient occlusion", 0);

        // ---- sidedness, subsurface ----
        m.doubleSided = um->features.double_sided.enabled;
        if (pbr.subsurface_factor.has_value && pbr.subsurface_factor.value_real > 0.0) {
            m.subsurfaceWeight = clamp01(pbr.subsurface_factor.value_real);
            if (pbr.subsurface_color.has_value) {
                m.subsurfaceColor[0] = f32(pbr.subsurface_color.value_vec4.x);
                m.subsurfaceColor[1] = f32(pbr.subsurface_color.value_vec4.y);
                m.subsurfaceColor[2] = f32(pbr.subsurface_color.value_vec4.z);
            }
            if (pbr.subsurface_radius.has_value) {
                const ufbx_vec3& r = pbr.subsurface_radius.value_vec3;
                m.subsurfaceRadius = std::max(0.0f, f32((r.x + r.y + r.z) / 3.0));
            }
        }

        // ---- named once each: what this importer does not carry ----
        unsupportedIf(pbr.sheen_factor, "sheen");
        unsupportedIf(pbr.coat_factor, "clear coat");
        unsupportedIf(pbr.thin_film_factor, "thin film");
        unsupportedIf(pbr.specular_anisotropy, "specular anisotropy");
        if (!isFbxBuiltin(shader)) unsupportedIf(pbr.transmission_factor, "transmission");
        if (pbr.displacement_map.texture || um->fbx.displacement.texture || um->fbx.vector_displacement.texture)
            c_.note("displacement maps");
    }

private:
    Ctx& c_;
    ImageStore& images_;
    std::string name_;

    void unsupportedIf(const ufbx_material_map& map, const char* what) {
        if (map.texture || (map.has_value && map.value_real > 0.0)) c_.note(std::string("material feature: ") + what);
    }

    ImportedTexture tex(const ufbx_material_map& map, const char* slot, char channel) {
        ImportedTexture ref;
        const ufbx_texture* t = map.texture;
        if (!t || !map.texture_enabled) return ref;
        if (t->type != UFBX_TEXTURE_FILE) {
            c_.note(t->type == UFBX_TEXTURE_LAYERED ? "layered textures (the first file texture is used)"
                  : t->type == UFBX_TEXTURE_SHADER  ? "shader-graph textures (the first file texture is used)"
                                                    : "procedural textures (the first file texture is used)");
            t = t->file_textures.count > 0 ? t->file_textures.data[0] : nullptr;
            if (!t) return ref;
        }
        if (t->has_uv_transform) c_.note("texture UV transforms");
        const i32 img = images_.get(t);
        if (img < 0) return ref;                                            // the image already said why, by name
        ref.imageIndex = img;
        ref.channel = channel;
        (void)slot;
        return ref;
    }
};

} // namespace

void importMaterials(Ctx& c) {
    const ufbx_scene* scene = c.scene;
    // Names first: the mesh slots are built from them whether or not the materials themselves are imported.
    c.materialName.assign(scene->materials.count, std::string());
    std::unordered_set<std::string> used;
    for (usize i = 0; i < scene->materials.count; ++i) {
        const ufbx_material* m = scene->materials.data[i];
        std::string base = str(m->name);
        if (base.empty()) base = "Material_" + std::to_string(m->element_id);
        std::string name = base;
        for (int n = 2; !used.insert(name).second; ++n) name = base + "_" + std::to_string(n);
        c.materialName[m->typed_id] = name;
    }
    if (!c.opt->importMaterials || scene->materials.count == 0) return;

    ImageStore images(c, c.sourceStem);
    MaterialReader reader(c, images);
    c.out->materials.assign(scene->materials.count, ImportedMaterial{});
    for (usize i = 0; i < scene->materials.count; ++i) {
        const ufbx_material* m = scene->materials.data[i];
        reader.convert(m, c.out->materials[m->typed_id]);
    }
    if (images.embedded || images.external || images.missing)
        c.line("textures: " + std::to_string(images.embedded) + " embedded, " + std::to_string(images.external) +
               " external, " + std::to_string(images.missing) + " missing");
}

std::string slotNameOf(const Ctx& c, const ufbx_material* m) {
    if (!m || m->typed_id >= c.materialName.size()) return "M_Default";
    return c.materialName[m->typed_id];
}

} // namespace aver::fmt::fbx

#endif // AVER_HAVE_UFBX
