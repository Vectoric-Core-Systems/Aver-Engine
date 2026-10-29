// Godot resources to Aver assets: textures, materials, primitive meshes, imported models, collision shapes.
#include "FabricInternal.hpp"
#include "FabricMeshGen.hpp"

#include "aver/core/Hash.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/platform/FileSystem.hpp"

#if AVER_HAVE_MATERIAL_COMPILE
#include "aver/formats/ImportCook.hpp"
#include "aver/formats/MaterialCook.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace aver::fabric {

// ---- small helpers -------------------------------------------------------------------------------------------------

Var makeNil() { return Var{}; }

Var makeString(const std::string& s) {
    Var v;
    v.type = VType::String;
    v.s = s;
    return v;
}

Var makeResource(const std::string& assetPath) {
    Var v;
    v.type = VType::Resource;
    v.s = assetPath;
    return v;
}

Var makeInt(long long i) {
    Var v;
    v.type = VType::Int;
    v.i = i;
    return v;
}

Var makeFloat(double f) {
    Var v;
    v.type = VType::Float;
    v.f = f;
    return v;
}

Var makeBool(bool b) {
    Var v;
    v.type = VType::Bool;
    v.b = b;
    return v;
}

Var makeVector3(double x, double y, double z) {
    Var v;
    v.type = VType::Vector3;
    v.nums = {x, y, z};
    return v;
}

Var makeObject(const std::string& cls, std::vector<std::pair<std::string, Var>> props) {
    Var v;
    v.type = VType::Object;
    v.s = cls;
    for (auto& [k, val] : props) {
        v.keys.push_back(makeString(k));
        v.items.push_back(std::move(val));
    }
    return v;
}

Var makeDict(std::vector<std::pair<std::string, Var>> entries) {
    Var v;
    v.type = VType::Dictionary;
    for (auto& [k, val] : entries) {
        v.keys.push_back(makeString(k));
        v.items.push_back(std::move(val));
    }
    return v;
}

double numOf(const Var* v, double def) {
    if (!v) return def;
    if (v->type == VType::Int) return static_cast<double>(v->i);
    if (v->type == VType::Float) return v->f;
    if (v->type == VType::Bool) return v->b ? 1.0 : 0.0;
    return def;
}

bool boolOf(const Var* v, bool def) {
    if (!v) return def;
    if (v->type == VType::Bool) return v->b;
    if (v->type == VType::Int) return v->i != 0;
    return def;
}

V3 vec3Of(const Var* v, const V3& def) {
    if (v && (v->type == VType::Vector3 || v->type == VType::Vector3i) && v->nums.size() >= 3) return {v->nums[0], v->nums[1], v->nums[2]};
    return def;
}

std::string stringOf(const Var* v, const std::string& def) {
    if (v && v->isStringLike()) return v->s;
    return def;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string safeName(const std::string& in, const std::string& fallback) {
    std::string out;
    for (const char c : in)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out.push_back(c);
    return out.empty() ? fallback : out;
}

std::string pascalCase(const std::string& in) {
    std::string out;
    bool up = true;
    for (const char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
            up = false;
        } else {
            up = true;
        }
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "N" + out;
    return out;
}

std::string stemOf(const std::string& path) {
    size_t a = path.find_last_of("/\\");
    a = a == std::string::npos ? 0 : a + 1;
    const size_t b = path.find_last_of('.');
    return path.substr(a, (b == std::string::npos || b < a) ? std::string::npos : b - a);
}

std::string fnvHex(const std::string& s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(fnv1a64(s) & 0xffffffffu));
    return buf;
}

namespace {

std::string extOf(const std::string& path) {
    const size_t b = path.find_last_of('.');
    const size_t s = path.find_last_of("/\\");
    if (b == std::string::npos || (s != std::string::npos && b < s)) return {};
    return lower(path.substr(b));
}

bool isModelExt(const std::string& e) { return e == ".glb" || e == ".gltf" || e == ".fbx" || e == ".obj"; }
bool isImageExt(const std::string& e) { return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp"; }
bool isOtherTextureExt(const std::string& e) {
    return e == ".webp" || e == ".svg" || e == ".exr" || e == ".hdr" || e == ".dds" || e == ".ktx" || e == ".ktx2" || e == ".basis";
}

bool isMaterial3DType(const std::string& t) { return t == "StandardMaterial3D" || t == "ORMMaterial3D"; }

bool isPrimitiveMeshType(const std::string& t) {
    return t == "BoxMesh" || t == "SphereMesh" || t == "CapsuleMesh" || t == "CylinderMesh" || t == "PlaneMesh" || t == "QuadMesh" ||
           t == "PrismMesh" || t == "TorusMesh" || t == "TubeTrailMesh" || t == "RibbonTrailMesh";
}

// The collision shape classes Aver.Fabric's ClassDB can build from an Object(...) value (G3_RUNTIME 1.2).
bool isRuntimeShapeType(const std::string& t) {
    return t == "BoxShape3D" || t == "SphereShape3D" || t == "CapsuleShape3D" || t == "CylinderShape3D" ||
           t == "WorldBoundaryShape3D" || t == "ConvexPolygonShape3D" || t == "ConcavePolygonShape3D";
}

bool isShapeType(const std::string& t) {
    return isRuntimeShapeType(t) || t == "HeightMapShape3D" || t == "SeparationRayShape3D";
}

bool ignorableResourceProp(const std::string& n) {
    return n == "resource_local_to_scene" || n == "resource_name" || n == "resource_path" || n == "script" || n == "metadata/_custom_type_script";
}

float srgbToLinear(double c) {
    c = std::max(0.0, std::min(1.0, c));
    return static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

const Var* findProp(const std::vector<gd::GodotProperty>& props, const char* name) {
    for (const gd::GodotProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

}  // namespace

// ---- textures ------------------------------------------------------------------------------------------------------

std::string Converter::copyTexture(const std::string& resPath, const Where& w) {
    if (const auto it = textureAssets_.find(resPath); it != textureAssets_.end()) return it->second;
    const std::string e = extOf(resPath);
    std::string asset;
    if (isImageExt(e)) {
        const std::string disk = fs_.toDiskPath(resPath);
        std::vector<u8> bytes;
        if (disk.empty() || !readFileBytes(disk, bytes)) {
            report_.add("resource", resPath, w.node, "Texture2D", "", Rating::Unsupported, "the texture file is missing", 0);
        } else {
            std::string rel = resPath.rfind("res://", 0) == 0 ? resPath.substr(6) : resPath;
            asset = "Textures/" + rel;
            const std::string dst = contentDir_ + "/" + asset;
            createDirectories(std::filesystem::path(dst).parent_path().string());
            if (!writeFileBytes(dst, bytes.data(), bytes.size())) {
                report_.add("resource", resPath, w.node, "Texture2D", "", Rating::Unsupported, "could not write " + dst, 0);
                asset.clear();
            } else {
                report_.add("resource", resPath, "", "Texture2D", "", Rating::Converted, "copied to " + asset, 0);
            }
        }
    } else if (isOtherTextureExt(e)) {
        report_.add("resource", resPath, w.node, "Texture2D", "", Rating::Unsupported,
                    "a " + e + " texture is not decoded by the engine's image loader; convert it to PNG", 0);
    } else {
        report_.add("resource", resPath, w.node, "Texture2D", "", Rating::Unsupported, "unknown texture file type " + e, 0);
    }
    textureAssets_[resPath] = asset;
    return asset;
}

namespace {

// A texture file as the material cook wants it: encoded bytes verbatim.
bool loadImage(Converter& c, const std::string& resPath, const Where& w, fmt::ImportedImage& img) {
    const std::string e = extOf(resPath);
    img.ok = false;
    if (!isImageExt(e)) {
        c.report_.add("resource", resPath, w.node, "Texture2D", "", Rating::Unsupported,
                      isOtherTextureExt(e) ? "a " + e + " texture is not decoded by the engine's image loader; convert it to PNG"
                                           : "unknown texture file type " + e,
                      0);
        return false;
    }
    const std::string disk = c.fs_.toDiskPath(resPath);
    if (disk.empty() || !readFileBytes(disk, img.bytes)) {
        c.report_.add("resource", resPath, w.node, "Texture2D", "", Rating::Unsupported, "the texture file is missing", 0);
        return false;
    }
    c.materialTextureSources_.insert(resPath);
    img.ext = e == ".jpg" ? ".jpeg" : e;
    img.suggestedName = safeName(stemOf(resPath), "texture");
    img.sourcePath = resPath;
    img.ok = true;
    return true;
}

char channelOf(double v) {
    switch (static_cast<int>(std::lround(v))) {
        case 0: return 'r';
        case 1: return 'g';
        case 2: return 'b';
        case 3: return 'a';
        default: return 0;
    }
}

}  // namespace

// ---- materials -----------------------------------------------------------------------------------------------------

std::string Converter::materialAsset(const std::string& materialKey, const std::string& type, const std::vector<gd::GodotProperty>& props,
                                     const gd::GodotDocBase& doc, const std::string& nameHint, const Where& w) {
    // G4 R5: a ViewportTexture in a texture slot names the SubViewport of the node that uses the material, so the same material resource used from two
    // nodes (or two instances of a scene) can bake two different views: the views are part of the cache key. Counted on every use, cached or not.
    const std::string vpAlbedo = viewportReference(findProp(props, "albedo_texture"), doc, w, "albedo_texture");
    const std::string vpEmission = viewportReference(findProp(props, "emission_texture"), doc, w, "emission_texture");
    const std::string key = vpAlbedo.empty() && vpEmission.empty() ? materialKey : materialKey + "|" + vpAlbedo + "|" + vpEmission;
    if (const auto it = materialAssets_.find(key); it != materialAssets_.end()) return it->second;
    materialAssets_[key] = {};   // a failure below is final; the entry is replaced on success

#if AVER_HAVE_MATERIAL_COMPILE
    const std::string& file = doc.file;
    const auto get = [&](const char* n) { return findProp(props, n); };
    const auto tex = [&](const char* n) -> std::string {
        const Var* v = get(n);
        if (!v || v->type != VType::ExtResource) return {};
        const gd::GodotExtResource* e = doc.findExt(v->s);
        return e ? fs_.resolve(*e, &diag_, file) : std::string{};
    };

    fmt::ImportedMaterial m;
    std::vector<fmt::ImportedImage> images;
    const auto bind = [&](fmt::ImportedTexture& slot, const std::string& resPath, char channel = 0) {
        if (resPath.empty()) return;
        fmt::ImportedImage img;
        if (!loadImage(*this, resPath, w, img)) return;
        images.push_back(std::move(img));
        slot.imageIndex = static_cast<i32>(images.size() - 1);
        slot.channel = channel;
    };

    std::string name = stringOf(get("resource_name"));
    if (name.empty()) name = nameHint;
    if (name.empty()) name = "Material";
    m.name = safeName(name, "Material");
    // The cook prefixes the project name; two different Godot materials with one name get distinct files.
    {
        std::string unique = m.name;
        for (int n = 2; materialStemsUsed_.count(name_ + "_" + unique); ++n) unique = m.name + "_" + std::to_string(n);
        m.name = unique;
        materialStemsUsed_.insert(name_ + "_" + unique);
    }

    // G4 R6: the BaseMaterial3D features, as the pairs of the .ocmat FX record (OcMat.hpp, parseOcmatFx). Each property below is rated where it is read.
    std::string fx;
    const auto addFx = [&](const std::string& kv) { if (!fx.empty()) fx += ' '; fx += kv; };
    const auto fnum = [](double v) { char b[40]; std::snprintf(b, sizeof b, "%.6g", v); return std::string(b); };
    std::set<std::string> rated;   // properties this block rated, so the leftover loop at the end skips them
    const auto rate = [&](const std::string& prop, Rating r, const std::string& why) { rated.insert(prop); report_.add("property", file, w.node, type, prop, r, why, 0); };
    const auto has = [&](const char* n) { return get(n) != nullptr; };

    // Albedo: Godot stores the colour as displayed (sRGB); the .ocmat holds sRGB too.
    m.baseColorFactorLinear = false;
    if (const Var* c = get("albedo_color"); c && c->type == VType::Color && c->nums.size() >= 4)
        for (int k = 0; k < 4; ++k) m.baseColorFactor[k] = static_cast<float>(c->nums[k]);
    bind(m.baseColorTex, tex("albedo_texture"));
    if (!vpAlbedo.empty()) m.baseColorTex.runtimePath = vpAlbedo;   // G4 R5: the view's picture, supplied by the engine

    m.metallicFactor = static_cast<float>(numOf(get("metallic"), 0.0));
    m.roughnessFactor = static_cast<float>(numOf(get("roughness"), 1.0));

    const std::string ormTex = tex("orm_texture");
    if (!ormTex.empty()) {
        // ORMMaterial3D: R occlusion, G roughness, B metallic, the layout the .ocmat packs.
        bind(m.metalRoughTex, ormTex);
        if (m.metalRoughTex.imageIndex >= 0) {
            m.occlusionTex = m.metalRoughTex;
            m.occlusionTex.channel = 0;
        }
    } else {
        const std::string rough = tex("roughness_texture"), metal = tex("metallic_texture");
        const char rc = channelOf(numOf(get("roughness_texture_channel"), 0)), mc = channelOf(numOf(get("metallic_texture_channel"), 0));
        bind(m.metalRoughTex, rough, rc);
        bind(m.metallicTex, metal, mc);
        if (m.metalRoughTex.imageIndex >= 0 && m.metallicTex.imageIndex < 0 && rc != 'g' && rc != 0)
            report_.add("property", file, w.node, type, "roughness_texture_channel", Rating::Approximated,
                        "the .ocmat reads roughness from the green channel of its packed map; the Godot texture uses another channel", 0);
    }

    if (boolOf(get("normal_enabled"), false)) {
        bind(m.normalTex, tex("normal_texture"));
        m.normalScale = static_cast<float>(numOf(get("normal_scale"), 1.0));
    }
    if (boolOf(get("ao_enabled"), false) && ormTex.empty()) {
        bind(m.occlusionTex, tex("ao_texture"), channelOf(numOf(get("ao_texture_channel"), 0)));
        // The map darkens the ambient light fully; ao_light_affect (how much it also darkens direct light) is the material's `aoaffect`.
        m.occlusionStrength = 1.0f;
    }
    if (const double affect = numOf(get("ao_light_affect"), 0.0); affect > 0.0) {
        addFx("aoaffect=" + fnum(affect));
        rate("ao_light_affect", Rating::Approximated, "the occlusion map also darkens the diffuse light that arrives directly (the diffuse albedo is scaled), not the specular");
    }
    // Godot's unshaded mode shows the texture as it is, lit by nothing. The .ocmat has no unshaded switch, so a view's picture in an unshaded material is also its
    // emissive map: it glows by itself on top of whatever light falls on it.
    const bool unshadedView = !vpAlbedo.empty() && vpEmission.empty() && static_cast<int>(numOf(get("shading_mode"), 1)) == 0;
    if (unshadedView) {
        m.emissiveTex.runtimePath = vpAlbedo;
        for (int k = 0; k < 3; ++k) m.emissiveFactor[k] = 1.0f;
    }
    if (boolOf(get("emission_enabled"), false)) {
        const double energy = numOf(get("emission_energy_multiplier"), 1.0);
        if (const Var* c = get("emission"); c && c->type == VType::Color && c->nums.size() >= 3)
            for (int k = 0; k < 3; ++k) m.emissiveFactor[k] = srgbToLinear(c->nums[k]) * static_cast<float>(energy);
        else
            for (int k = 0; k < 3; ++k) m.emissiveFactor[k] = static_cast<float>(energy);   // Godot's emission default is black, so this is only for a texture
        bind(m.emissiveTex, tex("emission_texture"));
        if (!vpEmission.empty()) m.emissiveTex.runtimePath = vpEmission;
        report_.add("property", file, w.node, type, "emission_energy_multiplier", Rating::Approximated,
                    "Godot's emission energy is scene-referred; the value is carried over as the .ocmat emissive factor without a unit calibration", 0);
    }

    // ---- transparency, blend, cull, depth (G4 R6) ----
    const int transparency = static_cast<int>(numOf(get("transparency"), 0));
    switch (transparency) {
        case 1: m.alphaMode = "BLEND"; break;
        case 2: m.alphaMode = "MASK"; break;
        case 3:   // alpha hash: a noise threshold (opaque pipeline, late depth), not a scissor
            m.alphaMode = "OPAQUE";
            addFx("alphaext=hash");
            if (const double hs = numOf(get("alpha_hash_scale"), 1.0); hs != 1.0) addFx("hashscale=" + fnum(hs));
            rate("transparency", Rating::Approximated, "alpha hash is a stable per-texel noise threshold on an opaque surface (the noise pattern is not Godot's)");
            break;
        case 4:
            m.alphaMode = "BLEND";
            addFx("depthdraw=always");
            rate("transparency", Rating::Approximated, "alpha depth pre-pass is an alpha blend that also writes depth (Godot writes depth for the opaque part only)");
            break;
        default: break;
    }
    if (transparency == 1 || transparency == 2) rate("transparency", Rating::Converted, transparency == 1 ? "alpha blend" : "alpha scissor (the .ocmat masked mode with the threshold)");
    m.alphaCutoff = static_cast<float>(numOf(get("alpha_scissor_threshold"), 0.5));
    if (has("alpha_scissor_threshold")) rate("alpha_scissor_threshold", Rating::Converted, "the .ocmat cut-off");
    if (has("alpha_hash_scale")) rate("alpha_hash_scale", transparency == 3 ? Rating::Converted : Rating::Approximated, transparency == 3 ? "the noise grid scale" : "only alpha hash reads it");
    if (const int aa = static_cast<int>(numOf(get("alpha_antialiasing_mode"), 0)); aa != 0) {
        addFx("alphaext=aa");
        if (transparency == 2 || transparency == 3) {
            if (transparency == 3) m.alphaMode = "MASK";
            rate("alpha_antialiasing_mode", Rating::Approximated, "alpha to coverage needs MSAA; the edge is dithered over the width of the alpha's screen derivative instead");
        } else {
            rate("alpha_antialiasing_mode", Rating::Unsupported, "alpha antialiasing only applies to alpha scissor and alpha hash");
        }
    }
    if (has("alpha_antialiasing_edge")) rate("alpha_antialiasing_edge", Rating::Approximated, "the edge softness is the alpha's screen derivative, not a setting");

    if (const int bm = static_cast<int>(numOf(get("blend_mode"), 0)); bm != 0) {
        static const char* const kModes[] = {"mix", "add", "sub", "mul", "premult"};
        addFx(std::string("blend=") + kModes[std::min(bm, 4)]);
        if (m.alphaMode != "BLEND") m.alphaMode = "BLEND";   // any mode but mix puts the surface in the transparent pass, as in Godot
        rate("blend_mode", bm == 4 ? Rating::Approximated : Rating::Converted,
             bm == 4 ? "premultiplied alpha is drawn with the engine's own premultiplied blend" : std::string("a dedicated blend state (") + kModes[std::min(bm, 4)] + ")");
    }
    if (const int dd = static_cast<int>(numOf(get("depth_draw_mode"), 0)); dd == 1 || dd == 2) {
        addFx(dd == 1 ? "depthdraw=always" : "depthdraw=never");
        rate("depth_draw_mode", Rating::Converted, dd == 1 ? "depth is written for every pixel" : "no depth is written");
    }
    if (boolOf(get("no_depth_test"), false)) { addFx("nodepthtest=1"); rate("no_depth_test", Rating::Converted, "the depth test is off (the surface draws over everything)"); }
    if (has("depth_test")) rate("depth_test", Rating::Unsupported, "an inverted depth test has no pipeline state in the engine");
    if (has("render_priority")) {
        addFx("priority=" + std::to_string(static_cast<int>(numOf(get("render_priority"), 0))));
        rate("render_priority", Rating::Converted, "sorts translucent draws (higher draws later, before the distance sort); opaque draws have no order");
    }
    if (has("next_pass")) rate("next_pass", Rating::Unsupported, "a second material pass has no equivalent");

    const int cull = static_cast<int>(numOf(get("cull_mode"), 0));
    m.doubleSided = cull == 2;
    addFx(cull == 1 ? "cull=front" : (cull == 2 ? "cull=none" : "cull=back"));
    rate("cull_mode", Rating::Converted, cull == 0 ? "back faces are culled (the pipeline's cull state, new for the material variants)"
                                       : (cull == 1 ? "front faces are culled" : "both faces draw"));

    // R1: the mesh's vertex colour multiplies the albedo (the mesh must carry the colour stream; the glTF, FBX and OBJ importers keep it).
    if (boolOf(get("vertex_color_use_as_albedo"), false)) {
        m.vertexColor = true;
        report_.add("property", file, w.node, type, "vertex_color_use_as_albedo", Rating::Converted,
                    "written as the .ocmat vertexcolor flag; the albedo and opacity are multiplied by the mesh's vertex colour in the raster scene pass "
                    "(ray-traced reflections and GI hits do not see it yet)", 0);
        if (boolOf(get("vertex_color_is_srgb"), false))
            report_.add("property", file, w.node, type, "vertex_color_is_srgb", Rating::Approximated,
                        "the vertex colour is multiplied as written (linear); Godot would convert an sRGB colour to linear first", 0);
    }

    // ---- subsurface scattering (existing subsurface fields) ----
    if (boolOf(get("subsurf_scatter_enabled"), false)) {
        m.subsurfaceWeight = static_cast<float>(std::min(1.0, numOf(get("subsurf_scatter_strength"), 0.0)));
        m.subsurfaceRadius = 0.5f;
        rate("subsurf_scatter_strength", Rating::Approximated, "mapped to the .ocmat subsurface weight with a fixed radius");
        rate("subsurf_scatter_enabled", Rating::Approximated, "wrap and thin-surface transmission, not a screen-space blur");
        if (boolOf(get("subsurf_scatter_transmittance_enabled"), false)) {
            if (const Var* c = get("subsurf_scatter_transmittance_color"); c && c->type == VType::Color && c->nums.size() >= 3)
                for (int k = 0; k < 3; ++k) m.subsurfaceColor[k] = static_cast<float>(c->nums[k]);
            const double depthCm = numOf(get("subsurf_scatter_transmittance_depth"), 0.1) * 100.0;
            m.subsurfaceRadius = static_cast<float>(std::max(0.0, std::min(1.0, (depthCm - 0.25) / 4.75)));
            rate("subsurf_scatter_transmittance_enabled", Rating::Approximated, "the transmittance colour tints the scattered light and the depth sets the radius (0.25 to 5 cm)");
        }
    }
    if (has("subsurf_scatter_skin_mode")) rate("subsurf_scatter_skin_mode", Rating::Approximated, "the skin profile is not modelled; the generic wrap is used");
    for (const char* n : {"subsurf_scatter_texture", "subsurf_scatter_transmittance_texture", "subsurf_scatter_transmittance_boost"})
        if (has(n)) rate(n, Rating::Unsupported, "no per-texel or boost control; the scalar subsurface weight is used");

    // ---- reflectance, clearcoat, refraction (existing material fields) ----
    if (has("metallic_specular")) {
        m.reflectance = static_cast<float>(numOf(get("metallic_specular"), 0.5) * 0.08);
        rate("metallic_specular", Rating::Converted, "F0 of the dielectric base is 0.08 * metallic_specular, the rule Godot uses");
    }
    if (boolOf(get("clearcoat_enabled"), false)) {
        m.coatWeight = static_cast<float>(std::max(0.0, std::min(1.0, numOf(get("clearcoat"), 1.0))));
        m.coatRoughness = static_cast<float>(std::max(0.0, std::min(1.0, numOf(get("clearcoat_roughness"), 0.5))));
        rate("clearcoat_enabled", Rating::Approximated, "the layered BSDF's coat lobe; it draws only with RENDER.LAYEREDBSDF on");
        rate("clearcoat", Rating::Approximated, "the coat weight");
        rate("clearcoat_roughness", Rating::Approximated, "the coat roughness");
    }
    if (has("clearcoat_texture")) rate("clearcoat_texture", Rating::Unsupported, "no per-texel coat; the scalar weight is used");
    if (boolOf(get("refraction_enabled"), false)) {
        const double scale = numOf(get("refraction_scale"), 0.05);
        m.transmission = 0.85f;
        m.ior = static_cast<float>(std::max(1.0, std::min(2.0, 1.0 + 5.0 * scale)));
        rate("refraction_enabled", Rating::Approximated, "transmission 0.85 and ior = 1 + 5 * refraction_scale: Aver's screen-space or traced refraction (RENDER.REFRACTION) bends the backdrop; it is not Godot's offset");
        rate("refraction_scale", Rating::Approximated, "the ior above");
    }
    for (const char* n : {"refraction_texture", "refraction_texture_channel"})
        if (has(n)) rate(n, Rating::Unsupported, "no per-texel refraction strength");

    // ---- UV transforms and triplanar (R6) ----
    {
        const V3 s1 = vec3Of(get("uv1_scale"), V3{1, 1, 1}), o1 = vec3Of(get("uv1_offset"), V3{0, 0, 0});
        const V3 s2 = vec3Of(get("uv2_scale"), V3{1, 1, 1}), o2 = vec3Of(get("uv2_offset"), V3{0, 0, 0});
        if (s1.x != 1 || s1.y != 1) addFx("uv1scale=" + fnum(s1.x) + "," + fnum(s1.y));
        if (o1.x != 0 || o1.y != 0) addFx("uv1offset=" + fnum(o1.x) + "," + fnum(o1.y));
        if (s2.x != 1 || s2.y != 1) addFx("uv2scale=" + fnum(s2.x) + "," + fnum(s2.y));
        if (o2.x != 0 || o2.y != 0) addFx("uv2offset=" + fnum(o2.x) + "," + fnum(o2.y));
        if (has("uv1_scale")) rate("uv1_scale", Rating::Converted, "UV * scale + offset in the material variant's pixel shader (x and y; both feed the triplanar position)");
        if (has("uv1_offset")) rate("uv1_offset", Rating::Converted, "UV * scale + offset in the material variant's pixel shader");
        if (has("uv2_scale")) rate("uv2_scale", Rating::Converted, "applied to the maps that read UV2 (detail on UV2, AO and emission on UV2); needs the mesh's second UV set, else UV1 is used");
        if (has("uv2_offset")) rate("uv2_offset", Rating::Converted, "as uv2_scale");
        if (boolOf(get("uv1_triplanar"), false)) {
            addFx("uv1tri=1");
            if (boolOf(get("uv1_world_triplanar"), false)) addFx("uv1world=1");
            if (has("uv1_triplanar_sharpness")) addFx("uv1sharp=" + fnum(numOf(get("uv1_triplanar_sharpness"), 1.0)));
            rate("uv1_triplanar", Rating::Converted, "three projections of the base maps blended by the normal; position in metres (object space, or world with uv1_world_triplanar) * uv1_scale + uv1_offset");
            rate("uv1_world_triplanar", Rating::Converted, "world-space position instead of object space");
            rate("uv1_triplanar_sharpness", Rating::Converted, "the blend exponent");
        } else {
            for (const char* n : {"uv1_world_triplanar", "uv1_triplanar_sharpness"}) if (has(n)) rate(n, Rating::Approximated, "only read with uv1_triplanar");
        }
        if (boolOf(get("uv2_triplanar"), false)) {
            addFx("uv2tri=1");
            rate("uv2_triplanar", Rating::Unsupported, "stored; the engine has one triplanar projection (uv1) and the UV2 maps stay planar");
        }
        for (const char* n : {"uv2_world_triplanar", "uv2_triplanar_sharpness"}) if (has(n)) rate(n, Rating::Unsupported, "see uv2_triplanar");
    }

    // ---- shading, shadows, fog, fades (R6) ----
    {
        const int sm = static_cast<int>(numOf(get("shading_mode"), 1));
        if (sm == 0) {
            addFx("unshaded=1");
            rate("shading_mode", unshadedView ? Rating::Approximated : Rating::Converted, unshadedView ? "unshaded: the view picture is also written as the emissive map; the material variant shows the albedo without lighting" : "unshaded: the albedo and glow reach the target without lighting");
        } else if (sm == 2) {
            rate("shading_mode", Rating::Approximated, "per-vertex shading is drawn per pixel");
        }
        if (boolOf(get("disable_receive_shadows"), false)) { addFx("noshadows=1"); rate("disable_receive_shadows", Rating::Converted, "the sun reaches the surface unshadowed"); }
        if (boolOf(get("disable_ambient_light"), false)) { addFx("noambient=1"); rate("disable_ambient_light", Rating::Approximated, "the sky ambient term and the specular occlusion are zeroed"); }
        if (has("disable_fog")) rate("disable_fog", Rating::Unsupported, "fog is applied to every lit surface");
        if (boolOf(get("shadow_to_opacity"), false)) { addFx("shadowtoopacity=1"); rate("shadow_to_opacity", Rating::Unsupported, "stored; the opacity does not follow the shadow"); }
        if (has("disable_specular_occlusion")) rate("disable_specular_occlusion", Rating::Unsupported, "specular occlusion is not a per-material switch");
        const int dm = static_cast<int>(numOf(get("diffuse_mode"), 0));
        if (has("diffuse_mode")) {
            if (dm == 2) addFx("diffuse=wrap");
            if (dm == 3) addFx("diffuse=toon");
            if (dm == 1) addFx("diffuse=lambert");
            rate("diffuse_mode", dm == 1 ? Rating::Converted : Rating::Approximated,
                 dm == 1 ? "the engine's diffuse lobe is Lambert" : (dm == 2 ? "lambert wrap is the subsurface wrap term" : (dm == 3 ? "toon diffuse is drawn as Lambert" : "burley is drawn as the engine's Lambert")));
        }
        const int spm = static_cast<int>(numOf(get("specular_mode"), 0));
        if (has("specular_mode")) {
            if (spm == 1) addFx("spec=toon");
            if (spm == 2) addFx("spec=off");
            rate("specular_mode", spm == 2 ? Rating::Converted : Rating::Approximated, spm == 2 ? "no specular reflection" : (spm == 1 ? "toon specular is drawn as GGX" : "Schlick-GGX"));
        }
        if (const int fm = static_cast<int>(numOf(get("distance_fade_mode"), 0)); fm != 0) {
            addFx(fm == 1 ? "fade=alpha" : (fm == 2 ? "fade=dither" : "fade=object"));
            addFx("fademin=" + fnum(numOf(get("distance_fade_min_distance"), 0.0) * 100.0));
            addFx("fademax=" + fnum(numOf(get("distance_fade_max_distance"), 10.0) * 100.0));
            rate("distance_fade_mode", fm == 1 ? Rating::Approximated : Rating::Converted,
                 fm == 1 ? "pixel alpha fades the opacity of a blended material; an opaque one dithers instead" : (fm == 2 ? "screen-door dither by the pixel's distance" : "screen-door dither by the object's distance"));
            rate("distance_fade_min_distance", Rating::Converted, "metres to centimetres: invisible closer than this");
            rate("distance_fade_max_distance", Rating::Converted, "metres to centimetres: fully opaque beyond this");
        } else {
            for (const char* n : {"distance_fade_min_distance", "distance_fade_max_distance"}) if (has(n)) rate(n, Rating::Approximated, "only read with distance_fade_mode");
        }
        if (boolOf(get("proximity_fade_enabled"), false)) rate("proximity_fade_enabled", Rating::Unsupported, "fading near other geometry needs the opaque depth, which a translucent draw cannot read");
        if (has("proximity_fade_distance")) rate("proximity_fade_distance", Rating::Unsupported, "see proximity_fade_enabled");
    }

    // ---- vertex stage (R6) ----
    {
        const int bb = static_cast<int>(numOf(get("billboard_mode"), 0));
        if (bb != 0) {
            addFx(bb == 1 ? "billboard=on" : (bb == 2 ? "billboard=y" : "billboard=particles"));
            if (boolOf(get("billboard_keep_scale"), false)) addFx("keepscale=1");
            rate("billboard_mode", bb == 3 ? Rating::Approximated : Rating::Converted,
                 bb == 3 ? "billboard like mode 1; the sprite sheet shows its first frame (the engine's mesh draws carry no particle age)"
                         : "the vertex stage turns the mesh toward the camera (VSMainFx); its shadow and depth prepass are the un-billboarded mesh's");
            if (bb == 3) {
                addFx("frames=" + std::to_string(static_cast<int>(numOf(get("particles_anim_h_frames"), 1))) + "," + std::to_string(static_cast<int>(numOf(get("particles_anim_v_frames"), 1))));
                for (const char* n : {"particles_anim_h_frames", "particles_anim_v_frames", "particles_anim_loop"}) if (has(n)) rate(n, Rating::Approximated, "the sheet's first frame is shown");
            }
        }
        if (has("billboard_keep_scale")) rate("billboard_keep_scale", bb != 0 ? Rating::Converted : Rating::Approximated, "the node's scale survives the billboard");
        if (boolOf(get("fixed_size"), false)) { addFx("fixedsize=1"); rate("fixed_size", Rating::Converted, "the object keeps one apparent size (offsets from its origin scale with view depth; 1 m of depth is the size authored)"); }
        if (boolOf(get("grow"), false)) {
            addFx("grow=1");
            addFx("growcm=" + fnum(numOf(get("grow_amount"), 0.0) * 100.0));
            rate("grow", Rating::Converted, "vertices are pushed along their normals (metres to centimetres); shadows and the depth prepass use the ungrown mesh");
            rate("grow_amount", Rating::Converted, "metres to centimetres");
        } else if (has("grow_amount")) {
            rate("grow_amount", Rating::Approximated, "only read with grow");
        }
        for (const char* n : {"use_point_size", "point_size"}) if (has(n)) rate(n, Rating::Unsupported, "point rendering has no equivalent");
        for (const char* n : {"use_particle_trails"}) if (has(n)) rate(n, Rating::Unsupported, "particle trails have no equivalent");
        for (const char* n : {"use_z_clip_scale", "z_clip_scale", "use_fov_override", "fov_override"}) if (has(n)) rate(n, Rating::Unsupported, "no per-material projection override");
    }

    // ---- surface features (R6) ----
    {
        if (boolOf(get("rim_enabled"), false)) {
            addFx("rim=1");
            addFx("rimamount=" + fnum(numOf(get("rim"), 1.0)));
            addFx("rimtint=" + fnum(numOf(get("rim_tint"), 0.5)));
            rate("rim_enabled", Rating::Approximated, "Godot's per-light rim, added as glow from the sun only (local lights and ambient add none)");
            rate("rim", Rating::Approximated, "the rim amount");
            rate("rim_tint", Rating::Converted, "mix of white and the albedo");
        }
        if (has("rim_texture")) rate("rim_texture", Rating::Unsupported, "no per-texel rim; the scalar amount is used");
        if (boolOf(get("anisotropy_enabled"), false)) {
            addFx("aniso=1");
            addFx("anisoamount=" + fnum(numOf(get("anisotropy"), 0.0)));
            rate("anisotropy_enabled", Rating::Approximated, "the shared BRDF is isotropic: the highlight is widened by the amount, not stretched along the tangent");
            rate("anisotropy", Rating::Approximated, "the widening amount");
        }
        if (has("anisotropy_flowmap")) rate("anisotropy_flowmap", Rating::Unsupported, "no flow direction texture");
        if (boolOf(get("heightmap_enabled"), false)) {
            addFx("height=1");
            addFx("heightscale=" + fnum(numOf(get("heightmap_scale"), 5.0)));
            bind(m.heightTex, tex("heightmap_texture"));
            if (boolOf(get("heightmap_deep_parallax"), false)) {
                addFx("deepparallax=1");
                addFx("layers=" + std::to_string(static_cast<int>(numOf(get("heightmap_min_layers"), 8))) + "," + std::to_string(static_cast<int>(numOf(get("heightmap_max_layers"), 32))));
            }
            if (boolOf(get("heightmap_flip_tangent"), false)) addFx("flipt=1");
            if (boolOf(get("heightmap_flip_binormal"), false)) addFx("flipb=1");
            if (boolOf(get("heightmap_flip_texture"), false)) addFx("invertheight=1");
            rate("heightmap_enabled", Rating::Converted, "parallax offset mapping from the heightmap's red channel (the layer-1 metal-rough texture slot)");
            rate("heightmap_scale", Rating::Approximated, "a UV depth of scale / 100; Godot's scale is in its own units");
            rate("heightmap_deep_parallax", Rating::Approximated, "steep parallax with the layer count by view angle; not Godot's occlusion refinement");
            for (const char* n : {"heightmap_min_layers", "heightmap_max_layers", "heightmap_flip_tangent", "heightmap_flip_binormal", "heightmap_flip_texture", "heightmap_texture"})
                if (has(n)) rate(n, Rating::Converted, "read by the parallax");
        } else {
            for (const char* n : {"heightmap_texture", "heightmap_scale", "heightmap_deep_parallax", "heightmap_min_layers", "heightmap_max_layers", "heightmap_flip_tangent",
                                  "heightmap_flip_binormal", "heightmap_flip_texture"})
                if (has(n)) rate(n, Rating::Approximated, "only read with heightmap_enabled");
        }
        if (boolOf(get("backlight_enabled"), false)) {
            addFx("backlight=1");
            if (const Var* c = get("backlight"); c && c->type == VType::Color && c->nums.size() >= 3)
                addFx("backlightcolor=" + fnum(srgbToLinear(c->nums[0])) + "," + fnum(srgbToLinear(c->nums[1])) + "," + fnum(srgbToLinear(c->nums[2])));
            rate("backlight_enabled", Rating::Approximated, "light from behind the surface, from the sun only, added as glow");
            rate("backlight", Rating::Approximated, "the tint (linear)");
        }
        if (has("backlight_texture")) rate("backlight_texture", Rating::Unsupported, "no per-texel backlight; the colour is used");
        if (boolOf(get("detail_enabled"), false)) {
            addFx("detail=1");
            bind(m.detailAlbedoTex, tex("detail_albedo"));
            bind(m.detailNormalTex, tex("detail_normal"));
            const int db = static_cast<int>(numOf(get("detail_blend_mode"), 0));
            if (db > 0) addFx("detailblend=" + std::to_string(std::min(db, 3)));
            if (static_cast<int>(numOf(get("detail_uv_layer"), 0)) == 1) addFx("detailuv=2");
            rate("detail_enabled", Rating::Converted, "the detail albedo and normal (the layer-1 texture slots) blended over the base maps");
            rate("detail_blend_mode", db == 4 ? Rating::Approximated : Rating::Converted, "mix, add, subtract, multiply; premultiplied alpha is drawn as mix");
            rate("detail_uv_layer", Rating::Converted, "UV1 or UV2 (UV2 needs the mesh's second UV set)");
            rate("detail_albedo", Rating::Converted, "bound to the layer-1 base colour slot");
            rate("detail_normal", Rating::Converted, "bound to the layer-1 normal slot");
            if (has("detail_mask")) rate("detail_mask", Rating::Unsupported, "the mask texture is not read; the detail applies everywhere");
        } else {
            for (const char* n : {"detail_albedo", "detail_normal", "detail_mask", "detail_blend_mode", "detail_uv_layer"}) if (has(n)) rate(n, Rating::Approximated, "only read with detail_enabled");
        }
        if (static_cast<int>(numOf(get("emission_operator"), 0)) == 1) { addFx("emitmul=1"); rate("emission_operator", Rating::Converted, "multiply: the emission is multiplied by the albedo"); }
        else if (has("emission_operator")) rate("emission_operator", Rating::Converted, "add (the engine's emissive)");
        if (boolOf(get("emission_on_uv2"), false)) { addFx("emituv2=1"); rate("emission_on_uv2", Rating::Converted, "the emission map is sampled with UV2 (needs the second UV set)"); }
        if (boolOf(get("ao_on_uv2"), false)) { addFx("aouv2=1"); rate("ao_on_uv2", Rating::Converted, "the occlusion map is sampled with UV2 (needs the second UV set)"); }
        if (has("emission_intensity")) rate("emission_intensity", Rating::Approximated, "physical emission units are not calibrated; emission_energy_multiplier carries the strength");
        if (has("texture_filter")) rate("texture_filter", Rating::Approximated, "the material sampler is shared (linear, anisotropic)");
        if (has("texture_repeat")) rate("texture_repeat", Rating::Approximated, "the material sampler wraps");
        for (const char* n : {"albedo_texture_force_srgb"}) if (has(n)) rate(n, Rating::Approximated, "the albedo slot is always sRGB");
        for (const char* n : {"albedo_texture_msdf", "msdf_pixel_range", "msdf_outline_size"}) if (has(n)) rate(n, Rating::Unsupported, "no MSDF text rendering in materials");
        for (const char* n : {"bent_normal_enabled", "bent_normal_texture"}) if (has(n)) rate(n, Rating::Unsupported, "no bent normal map");
        for (const char* n : {"stencil_mode", "stencil_flags", "stencil_compare", "stencil_reference", "stencil_color", "stencil_outline_thickness"})
            if (has(n)) rate(n, Rating::Unsupported, "no stencil buffer in the engine");
    }
    m.fxRecord = fx;   // `cull=back` alone is a record: the engine culled nothing before

    // Features the material cook has no field for; each one is listed rather than dropped silently.
    for (const gd::GodotProperty& p : props) {
        const std::string& n = p.name;
        static const std::set<std::string> handled = {
            "albedo_color", "albedo_texture", "metallic", "roughness", "orm_texture", "roughness_texture", "metallic_texture",
            "roughness_texture_channel", "metallic_texture_channel", "normal_enabled", "normal_scale", "normal_texture",
            "ao_enabled", "ao_texture", "ao_texture_channel", "emission_enabled", "emission", "emission_energy_multiplier",
            "emission_texture", "vertex_color_use_as_albedo", "vertex_color_is_srgb"};
        if (handled.count(n) || rated.count(n) || ignorableResourceProp(n)) continue;
        report_.add("property", file, w.node, type, n, Rating::Unsupported, "the .ocmat has no equivalent", p.line);
    }

    // Separate roughness and metallic maps become the one packed map the .ocmat has.
    std::vector<fmt::ImportedMaterial> mats{m};
    fmt::packSeparateMetalRough(images, mats);

    fmt::MaterialCookOptions opt;
    opt.contentDir = contentDir_;
    opt.assetBase = name_;
    opt.overwriteExisting = true;
    fmt::MaterialCookResult res;
    std::vector<std::string> warnings;
    std::string err;
    if (!fmt::cookMaterials(mats, images, opt, res, &warnings, &err) || res.materialSlotNames.empty() || res.materialSlotNames[0].empty()) {
        report_.add("resource", file, w.node, type, "", Rating::Unsupported, "the material could not be written: " + err, 0);
        return {};
    }
    for (const std::string& warning : warnings) report_.add("resource", file, w.node, type, "", Rating::Approximated, warning, 0);
    const std::string asset = "Materials/" + res.materialSlotNames[0] + ".ocmat";
    report_.add("resource", file, "", type, "", Rating::Converted, key + " written as " + asset, 0);
    materialAssets_[key] = asset;
    return asset;
#else
    (void)type; (void)props; (void)doc; (void)nameHint;
    report_.add("resource", doc.file, w.node, type, "", Rating::Unsupported, "this build of AverAssetC has no material compiler (AVER_MODULE_PBR is off)", 0);
    return {};
#endif
}

// ---- sub-viewports (docs/plans/fabric/G4_RENDER.md R5) ----------------------------------------------------------------

std::string Converter::viewportReference(const Var* value, const gd::GodotDocBase& doc, const Where& w, const std::string& prop) {
    if (!value || value->type != VType::SubResource) return {};
    const gd::GodotSubResource* sr = doc.findSub(value->s);
    if (!sr || sr->type != "ViewportTexture") return {};
    const std::string path = stringOf(findProp(sr->props, "viewport_path"));
    if (!emitting_ || path.empty() || path[0] == '/') {
        report_.add("property", doc.file, w.node, w.type, prop, Rating::Unsupported,
                    emitting_ ? "the ViewportTexture's viewport_path is empty or absolute; only a path relative to its scene's root is resolved"
                              : "a ViewportTexture outside a node cannot be resolved to a SubViewport",
                    value->line);
        return {};
    }
    // The path is relative to the root of the scene the using node was authored in (for a node of an instanced scene, the instance's root).
    const FNode* cur = emitting_->owner ? emitting_->owner : emitting_;
    size_t at = 0;
    while (cur && at <= path.size()) {
        const size_t slash = path.find('/', at);
        const std::string seg = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        at = slash == std::string::npos ? path.size() + 1 : slash + 1;
        if (seg.empty() || seg == ".") continue;
        cur = seg == ".." ? cur->parent : cur->findChild(seg);
    }
    if (!cur || cur->type != "SubViewport") {
        report_.add("property", doc.file, w.node, w.type, prop, Rating::Unsupported,
                    cur ? "the ViewportTexture's viewport_path '" + path + "' names a " + cur->type + ", not a SubViewport"
                        : "the ViewportTexture's viewport_path '" + path + "' names a node that does not exist",
                    value->line);
        return {};
    }
    const std::string key = viewportKeyOf(emittingLevel_, *cur);
    if (++viewportUsers_[key] == 1)
        report_.add("property", doc.file, w.node, w.type, prop, Rating::Converted,
                    "a ViewportTexture: written as the texture reference '@rt:viewport." + key + "', which the engine resolves to the SubViewport's picture "
                    "(drawn by the cheap raster configuration, linear values; the view is drawn only while a material, a container or a script's UI shows it)",
                    value->line);
    return "@rt:viewport." + key;
}

// ---- primitive meshes ----------------------------------------------------------------------------------------------

std::string Converter::primitiveMeshAsset(const std::string& type, const std::vector<gd::GodotProperty>& props,
                                          const gd::GodotDocBase& doc, const Where& w) {
    // A key from the type and every property's canonical text: equal meshes share one file.
    std::string canon = type;
    for (const gd::GodotProperty& p : props) canon += "|" + p.name + "=" + gd::godotVariantToText(p.value);
    const std::string key = canon + "|" + doc.file;
    if (const auto it = meshAssets_.find(key); it != meshAssets_.end()) return it->second;
    meshAssets_[key] = {};

    std::string slot = "M_Default";
    if (const Var* mat = findProp(props, "material")) {
        const Var cv = convertValue(*mat, doc, w, "material");
        if (cv.type == VType::Resource) slot = stemOf(cv.s);
    }
    canon += "|" + slot;

    SoupMesh soup;
    std::string why;
    const PropGetter get = [&](const char* n) { return findProp(props, n); };
    if (!buildPrimitiveMesh(type, get, soup, &why)) {
        report_.add("resource", doc.file, w.node, type, "", Rating::Unsupported, why, 0);
        return {};
    }
    fmt::OcMeshData mesh = toOcMesh(soup, slot);
    const std::string asset = "Meshes/Generated/" + type + "_" + fnvHex(canon) + ".ocmesh";
    createDirectories(contentDir_ + "/Meshes/Generated");
    std::string err;
    if (!fmt::saveOcMesh(contentDir_ + "/" + asset, mesh, &err)) {
        report_.add("resource", doc.file, w.node, type, "", Rating::Unsupported, "could not write " + asset + ": " + err, 0);
        return {};
    }
    // Properties the generator does not read.
    for (const gd::GodotProperty& p : props) {
        static const std::set<std::string> known = {
            "size", "subdivide_width", "subdivide_height", "subdivide_depth", "radius", "height", "radial_segments", "rings",
            "is_hemisphere", "top_radius", "bottom_radius", "cap_top", "cap_bottom", "orientation", "center_offset", "left_to_right",
            "inner_radius", "outer_radius", "ring_segments", "subdivide_depth", "material", "flip_faces",
            "radial_steps", "sections", "section_length", "section_rings", "shape", "section_segments"};
        if (known.count(p.name) || ignorableResourceProp(p.name)) continue;
        report_.add("property", doc.file, w.node, type, p.name, Rating::Approximated, "not used by the generated mesh", p.line);
    }
    if (type == "TubeTrailMesh" || type == "RibbonTrailMesh")
        report_.add("resource", doc.file, "", type, "", Rating::Approximated,
                    "a trail mesh is bent along a particle's path by the particle system; it is generated as a static straight " + std::string(type == "TubeTrailMesh" ? "tube" : "ribbon") +
                        " along Y, centred, and its curve (width over length) is not applied",
                    0);
    else
        report_.add("resource", doc.file, "", type, "", Rating::Converted, "generated " + asset + " with Godot's dimensions", 0);
    meshAssets_[key] = asset;
    return asset;
}

// ---- imported model files --------------------------------------------------------------------------------------------

const ModelResult* Converter::modelFor(const std::string& resPath, const Where& w) {
    if (const auto it = models_.find(resPath); it != models_.end()) return it->second.ok ? &it->second : nullptr;
    ModelResult r;
    if (!host_.importModel) {
        r.error = "this AverAssetC has no model importer";
    } else {
        const std::string disk = fs_.toDiskPath(resPath);
        std::string rel = resPath.rfind("res://", 0) == 0 ? resPath.substr(6) : resPath;
        const size_t dot = rel.find_last_of('.');
        if (dot != std::string::npos) rel.erase(dot);
        std::string base = rel;
        for (char& c : base) if (c == '/') c = '_';
        base = safeName(base, "model");
        r = host_.importModel(disk, contentDir_, "Meshes/" + rel, base);
    }
    models_[resPath] = r;
    if (!r.ok) {
        report_.add("resource", resPath, w.node, "PackedScene", "", Rating::Unsupported, "the model could not be imported: " + r.error, 0);
        return nullptr;
    }
    for (const std::string& n : r.notes) report_.add("resource", resPath, "", "PackedScene", "", Rating::Approximated, n, 0);
    report_.add("resource", resPath, "", "PackedScene", "", Rating::Converted, std::to_string(r.parts.size()) + " mesh(es) imported", 0);

    // The model's own import settings: the root scale is the one that changes geometry.
    gd::GodotImport imp;
    gd::GodotDiagnostics d;
    if (fs_.loadImport(resPath, imp, d))
        if (const Var* s = imp.param("nodes/root_scale"); s && std::fabs(numOf(s, 1.0) - 1.0) > 1e-9)
            report_.add("resource", resPath, "", "PackedScene", "nodes/root_scale", Rating::Approximated,
                        "the import's root scale is not applied to the converted meshes", 0);
    return &models_[resPath];
}

const gd::GodotResource* Converter::resourceDoc(const std::string& resPath) {
    if (const auto it = resourceDocs_.find(resPath); it != resourceDocs_.end()) return it->second.get();
    auto doc = std::make_unique<gd::GodotResource>();
    gd::GodotDiagnostics d;
    if (!fs_.loadResource(resPath, *doc, d)) {
        for (const gd::GodotDiag& x : d.items) report_.add("resource", resPath, "", "", "", Rating::Unsupported, x.message, x.line);
        resourceDocs_[resPath] = nullptr;
        return nullptr;
    }
    for (const gd::GodotDiag& x : d.items)
        if (x.severity != gd::GodotSeverity::Info) report_.add("resource", resPath, "", "", "", Rating::Approximated, x.message, x.line);
    const gd::GodotResource* p = doc.get();
    resourceDocs_[resPath] = std::move(doc);
    return p;
}

// ---- shapes and other resources -------------------------------------------------------------------------------------------

Var Converter::genericResourceDict(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                                   const Where& w, const char* why) {
    std::vector<std::pair<std::string, Var>> entries;
    entries.emplace_back("__class", makeString(type));
    for (const gd::GodotProperty& p : props) {
        if (ignorableResourceProp(p.name)) continue;
        entries.emplace_back(p.name, convertValue(p.value, doc, w, p.name));
    }
    report_.add("resource", doc.file, w.node, type, "", Rating::Approximated, why, 0);
    return makeDict(std::move(entries));
}

Var Converter::shapeObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w) {
    if (isRuntimeShapeType(type)) {
        std::vector<std::pair<std::string, Var>> entries;
        for (const gd::GodotProperty& p : props) {
            if (ignorableResourceProp(p.name)) continue;
            entries.emplace_back(p.name, convertValue(p.value, doc, w, p.name));
        }
        report_.add("resource", doc.file, w.node, type, "", Rating::Converted, "", 0);
        return makeObject(type, std::move(entries));
    }
    if (type == "HeightMapShape3D") {
        const int width = static_cast<int>(numOf(findProp(props, "map_width"), 2)), depth = static_cast<int>(numOf(findProp(props, "map_depth"), 2));
        const Var* data = findProp(props, "map_data");
        if (width < 2 || depth < 2 || width * depth > 64 * 64 || !data || data->nums.size() < static_cast<size_t>(width) * depth) {
            return genericResourceDict(type, props, doc, w,
                                       "kept as a dictionary: only height maps up to 64 x 64 with all their data become a triangle shape");
        }
        // Godot centres the grid on the origin; one triangle pair per cell.
        Var tri;
        tri.type = VType::PackedVector3Array;
        const auto height = [&](int x, int z) { return data->nums[static_cast<size_t>(z) * width + x]; };
        const auto pt = [&](int x, int z) { return V3{x - (width - 1) / 2.0, height(x, z), z - (depth - 1) / 2.0}; };
        const auto push = [&](const V3& p) { tri.nums.insert(tri.nums.end(), {p.x, p.y, p.z}); };
        for (int z = 0; z + 1 < depth; ++z)
            for (int x = 0; x + 1 < width; ++x) {
                push(pt(x, z)); push(pt(x + 1, z)); push(pt(x, z + 1));
                push(pt(x + 1, z)); push(pt(x + 1, z + 1)); push(pt(x, z + 1));
            }
        report_.add("resource", doc.file, w.node, type, "", Rating::Approximated,
                    "written as a ConcavePolygonShape3D of the height map's triangles (no height-field shape in the runtime class table)", 0);
        return makeObject("ConcavePolygonShape3D", {{"data", std::move(tri)}});
    }
    return genericResourceDict(type, props, doc, w, "this shape class has no Aver.Fabric counterpart yet; its parameters are kept as a dictionary");
}

namespace {

bool isEnvironmentType(const std::string& t) {
    static const std::set<std::string> s = {"Environment", "Sky", "ProceduralSkyMaterial", "PhysicalSkyMaterial", "PanoramaSkyMaterial",
                                            "CameraAttributes", "CameraAttributesPractical", "CameraAttributesPhysical",
                                            "FogMaterial"};   // G4 R8: a FogVolume's material is held as a value like the environment's resources
    return s.count(t) != 0;
}

}  // namespace

Var Converter::environmentObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w) {
    std::vector<std::pair<std::string, Var>> entries;
    for (const gd::GodotProperty& p : props) {
        if (ignorableResourceProp(p.name)) continue;
        if (type == "FogMaterial") {
            // G4 R8: what each FogMaterial key becomes in the engine's fog volume (framework_envfx_abi.h; shader: voxi_froxel.hlsl, AVER_FOG_VOLUMES).
            const char* why = nullptr;
            Rating r = Rating::Converted;
            if (p.name == "density") why = "extinction per metre, per centimetre in the engine (/100); may be negative, which carves fog out of the volumes it overlaps";
            else if (p.name == "albedo") why = "the scattering colour: the sun and the sky light the volume's fog with it (sRGB to linear)";
            else if (p.name == "emission") { why = "radiance the fog adds per unit density (sRGB to linear), not occluded and not lit"; r = Rating::Approximated; }
            else if (p.name == "height_falloff") { why = "density falls by e^(-falloff x height in metres) above the volume's lowest point (Godot's exact reference height is not documented; this one is assumed)"; r = Rating::Approximated; }
            else if (p.name == "edge_fade") { why = "a soft edge: full density from this fraction of the half size inside the shape's surface, ramping linearly to nothing at the surface (Godot's exact falloff curve is not documented)"; r = Rating::Approximated; }
            else if (p.name == "density_texture") { report_.add("resource", doc.file, w.node, type, p.name, Rating::Unsupported, "a 3D density texture is not sampled; the density is uniform inside the shape", p.line); continue; }
            if (why) report_.add("resource", doc.file, w.node, type, p.name, r, why, p.line);
        }
        if (type == "PanoramaSkyMaterial" && p.name == "panorama" && p.value.type == VType::ExtResource) {
            // The file is kept as it is (the renderers do not sample it, but the node holds the reference); .hdr and .exr are not images the engine decodes.
            const gd::GodotExtResource* e = doc.findExt(p.value.s);
            const std::string res = e ? fs_.resolve(*e, &diag_, doc.file) : std::string();
            std::vector<u8> bytes;
            const std::string disk = res.empty() ? std::string() : fs_.toDiskPath(res);
            if (disk.empty() || !readFileBytes(disk, bytes)) {
                report_.add("resource", doc.file, w.node, type, "panorama", Rating::Unsupported, "the panorama file is missing", p.line);
                continue;
            }
            const std::string asset = "Textures/" + (res.rfind("res://", 0) == 0 ? res.substr(6) : res);
            const std::string dst = contentDir_ + "/" + asset;
            createDirectories(std::filesystem::path(dst).parent_path().string());
            if (!writeFileBytes(dst, bytes.data(), bytes.size())) {
                report_.add("resource", doc.file, w.node, type, "panorama", Rating::Unsupported, "could not write " + dst, p.line);
                continue;
            }
            entries.emplace_back(p.name, makeResource(asset));
            continue;
        }
        Var cv = convertValue(p.value, doc, w, p.name);
        if (cv.isNil() && !p.value.isNil()) continue;   // already reported
        entries.emplace_back(p.name, std::move(cv));
    }
    return makeObject(type, std::move(entries));
}

Var Converter::convertValue(const Var& v, const gd::GodotDocBase& doc, const Where& w, const std::string& prop) {
    switch (v.type) {
        case VType::Array: {
            Var out = v;
            for (Var& item : out.items) item = convertValue(item, doc, w, prop);
            return out;
        }
        case VType::Dictionary: {
            Var out = v;
            for (Var& k : out.keys) k = convertValue(k, doc, w, prop);
            for (Var& item : out.items) item = convertValue(item, doc, w, prop);
            return out;
        }
        case VType::Object: {
            Var out = v;
            for (Var& item : out.items) item = convertValue(item, doc, w, prop);
            return out;
        }
        case VType::SubResource: {
            const gd::GodotSubResource* sr = doc.findSub(v.s);
            if (!sr) {
                report_.add("property", doc.file, w.node, w.type, prop, Rating::Unsupported, "the sub-resource " + v.s + " is not declared", v.line);
                return makeNil();
            }
            const std::string& t = sr->type;
            if (isMaterial3DType(t)) {
                const std::string a = materialAsset(doc.file + "#" + sr->id, t, sr->props, doc, stemOf(doc.file) + "_" + sr->id, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (t == "ShaderMaterial") {   // G6 SH2
                const std::string a = shaderMaterialAsset(doc.file + "#" + sr->id, sr->props, doc, stemOf(doc.file) + "_" + sr->id, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (t == "ViewportTexture") {
                // G4 R5: a ViewportTexture held as a node or script value keeps its path for Aver.Fabric's ViewportTexture. (A material's texture slot is
                // handled in materialAsset, which writes the engine's texture reference into the .ocmat.)
                const Var* path = findProp(sr->props, "viewport_path");
                report_.add("resource", doc.file, w.node, t, "", Rating::Converted,
                            "kept as Object(ViewportTexture) with its viewport_path; it names the SubViewport relative to its scene and its picture is the view's", sr->line);
                return makeObject("ViewportTexture", {{"viewport_path", path ? *path : makeNil()}});
            }
            if (isPrimitiveMeshType(t)) {
                const std::string a = primitiveMeshAsset(t, sr->props, doc, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (t == "ArrayMesh") {
                const std::string a = arrayMeshAsset(sr->props, doc, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (t == "MultiMesh") return multiMeshObject(sr->props, doc, w);
            if (t == "Animation") {
                const Var* rn = findProp(sr->props, "resource_name");
                const std::string a = animationAsset(doc.file + "#" + sr->id, sr->props, doc, stemOf(doc.file) + "_" + (rn && !rn->asString().empty() ? rn->asString() : sr->id), w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (t == "AnimationLibrary") return animationLibraryValue(sr->props, doc, w);
            if (isAnimTreeResourceType(t)) return animTreeObject(t, sr->props, doc, w, sr->line);
            if (isNavigationResourceType(t)) return navigationMeshObject(t, sr->props, doc, w, sr->line);
            if (isReplicationResourceType(t)) return replicationConfigObject(sr->props, doc, w, sr->line);   // G6 NET2
            if (isAudioStreamResourceType(t)) return audioStreamObject(t, sr->props, doc, w, sr->line);
            if (t == "Theme") {   // G5 U1: a theme resource held by a scene becomes an .octheme file
                const std::string a = themeAsset(doc.file + "#" + sr->id, sr->props, doc, stemOf(doc.file) + "_" + sr->id, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (isStyleBoxType(t)) return styleBoxObject(t, sr->props, doc, w, sr->line);
            if (isWidgetResourceType(t)) return widgetResource(t, sr->props, doc, w, sr->line, sr->id);   // G5 U2
            if (t == "ImmediateMesh" || t == "PointMesh" || t == "TextMesh") {
                report_.add("resource", doc.file, w.node, t, "", Rating::Unsupported,
                            t == "TextMesh" ? "TextMesh extrudes glyph outlines; the converter has no font tessellator (a Label3D or a UI label shows text instead)"
                            : t == "ImmediateMesh" ? "an ImmediateMesh holds no stored surfaces: a script fills it every frame (Aver.Fabric's ImmediateMesh does the same at run time)"
                                                   : "a point mesh has no triangles",
                            sr->line);
                return makeNil();
            }
            if (isShapeType(t)) return shapeObject(t, sr->props, doc, w);
            if (isEnvironmentType(t)) return environmentObject(t, sr->props, doc, w);
            return genericResourceDict(t, sr->props, doc, w, "no Aver counterpart for this resource class; its properties are kept as a dictionary");
        }
        case VType::ExtResource: {
            const gd::GodotExtResource* e = doc.findExt(v.s);
            if (!e) {
                report_.add("property", doc.file, w.node, w.type, prop, Rating::Unsupported, "the external resource " + v.s + " is not declared", v.line);
                return makeNil();
            }
            const std::string res = fs_.resolve(*e, &diag_, doc.file);
            const std::string ext = extOf(res);
            const std::string& t = e->type;
            if (res.empty() || !fs_.exists(res)) {
                report_.add("property", doc.file, w.node, w.type, prop, Rating::Unsupported, "the file " + e->path + " does not exist", v.line);
                return makeNil();
            }
            if (t == "Script") {
                report_.add("property", doc.file, w.node, w.type, prop, Rating::Unsupported, "a script held as a value is not converted", v.line);
                return makeNil();
            }
            if (isFontExt(ext)) {   // G5 U1
                const std::string a = fontAsset(res, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (isModelExt(ext)) {
                const ModelResult* m = modelFor(res, w);
                if (!m || m->parts.empty()) return makeNil();
                if (m->parts.size() > 1)
                    report_.add("property", doc.file, w.node, w.type, prop, Rating::Approximated,
                                "the model has " + std::to_string(m->parts.size()) + " meshes; a single mesh property takes the first", v.line);
                return makeResource(m->parts[0].meshAsset);
            }
            if (ext == ".tscn" || ext == ".scn") {
                report_.add("property", doc.file, w.node, w.type, prop, Rating::Approximated,
                            "a PackedScene value points at the level written for that scene; PackedScene.Instantiate is not in the runtime yet", v.line);
                return makeResource(levelAssetFor(res));
            }
            if (isAudioExt(ext)) return audioFileValue(res, t, w, prop, v.line);
            if (isImageExt(ext) || isOtherTextureExt(ext) || t.find("Texture") != std::string::npos) {
                const std::string a = copyTexture(res, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (ext == ".tres" || ext == ".res") {
                const gd::GodotResource* r = resourceDoc(res);
                if (!r) return makeNil();
                const std::string& rt = r->header.type;
                if (isMaterial3DType(rt)) {
                    const std::string a = materialAsset(res, rt, r->props, *r, stemOf(res), w);
                    return a.empty() ? makeNil() : makeResource(a);
                }
                if (rt == "ShaderMaterial") {   // G6 SH2
                    const std::string a = shaderMaterialAsset(res, r->props, *r, stemOf(res), w);
                    return a.empty() ? makeNil() : makeResource(a);
                }
                if (isPrimitiveMeshType(rt)) {
                    const std::string a = primitiveMeshAsset(rt, r->props, *r, w);
                    return a.empty() ? makeNil() : makeResource(a);
                }
                if (isShapeType(rt)) return shapeObject(rt, r->props, *r, w);
                if (isEnvironmentType(rt)) return environmentObject(rt, r->props, *r, w);
                if (rt == "ArrayMesh") {
                    const std::string a = arrayMeshAsset(r->props, *r, w);
                    return a.empty() ? makeNil() : makeResource(a);
                }
                if (rt == "MultiMesh") return multiMeshObject(r->props, *r, w);
                if (rt == "Animation") {
                    const std::string a = animationAsset(res, r->props, *r, stemOf(res), w);
                    return a.empty() ? makeNil() : makeResource(a);
                }
                if (rt == "AnimationLibrary") return animationLibraryValue(r->props, *r, w);
                if (isAnimTreeResourceType(rt)) return animTreeObject(rt, r->props, *r, w, 0);
                if (isNavigationResourceType(rt)) return navigationMeshObject(rt, r->props, *r, w, 0);
                if (isReplicationResourceType(rt)) return replicationConfigObject(r->props, *r, w, 0);   // G6 NET2
                if (isAudioStreamResourceType(rt)) return audioStreamObject(rt, r->props, *r, w, 0);
                if (rt == "Theme") {   // G5 U1
                    const std::string a = themeAsset(res, r->props, *r, stemOf(res), w);
                    return a.empty() ? makeNil() : makeResource(a);
                }
                if (isStyleBoxType(rt)) return styleBoxObject(rt, r->props, *r, w, 0);
                if (isWidgetResourceType(rt)) return widgetResource(rt, r->props, *r, w, 0, "");   // G5 U2
                if (rt == "TextMesh" || rt == "ImmediateMesh" || rt == "PointMesh") {
                    report_.add("resource", res, w.node, rt, "", Rating::Unsupported,
                                rt == "TextMesh" ? "TextMesh extrudes glyph outlines; the converter has no font tessellator" : "this mesh class holds no stored triangles to convert", 0);
                    return makeNil();
                }
                return genericResourceDict(rt, r->props, *r, w, "no Aver counterpart for this resource class; its properties are kept as a dictionary");
            }
            report_.add("resource", res, w.node, t, "", Rating::Unsupported, "a " + t + " resource (" + ext + ") is not converted yet", v.line);
            return makeNil();
        }
        default:
            return v;
    }
}

// ---- resources that no scene had to ask for -------------------------------------------------------------------------------

void Converter::convertStandaloneResources() {
    for (const std::string& res : fs_.files()) {
        const std::string ext = extOf(res);
        const Where w{res, "", ""};
        if (ext == ".tres") {
            const gd::GodotResource* r = resourceDoc(res);
            if (!r) continue;
            const std::string& t = r->header.type;
            if (isMaterial3DType(t)) { materialAsset(res, t, r->props, *r, stemOf(res), w); continue; }
            if (isPrimitiveMeshType(t)) { primitiveMeshAsset(t, r->props, *r, w); continue; }
            if (t == "ArrayMesh") { arrayMeshAsset(r->props, *r, w); continue; }
            if (t == "Animation") { animationAsset(res, r->props, *r, stemOf(res), w); continue; }
            if (t == "AnimationLibrary") { animationLibraryValue(r->props, *r, w); continue; }
            if (isAnimTreeResourceType(t)) {
                report_.add("resource", res, "", t, "", Rating::Converted, "converted where a scene uses it", 0);
                continue;
            }
            if (isNavigationResourceType(t) || isReplicationResourceType(t)) {
                report_.add("resource", res, "", t, "", Rating::Converted, "converted where a scene uses it", 0);
                continue;
            }
            if (t == "AudioBusLayout") { convertBusLayout(res, *r); continue; }
            if (t == "Theme") { themeAsset(res, r->props, *r, stemOf(res), w); continue; }   // G5 U1: every theme is written, whether or not a scene names it
            if (isStyleBoxType(t) || isWidgetResourceType(t)) {
                report_.add("resource", res, "", t, "", Rating::Converted, "converted where a scene or theme uses it", 0);
                continue;
            }
            if (isAudioStreamResourceType(t)) {
                report_.add("resource", res, "", t, "", Rating::Converted, "converted where a scene uses it", 0);
                continue;
            }
            if (t.rfind("AudioEffect", 0) == 0) {
                report_.add("resource", res, "", t, "", Rating::Converted, "an effect resource converts inside the bus layout that holds it", 0);
                continue;
            }
            if (t == "MultiMesh" || t == "MeshLibrary") {
                report_.add("resource", res, "", t, "", Rating::Converted, "converted where a scene uses it", 0);
                continue;
            }
            if (t == "ShaderMaterial")
                shaderMaterialAsset(res, r->props, *r, stemOf(res), w);   // G6 SH2: every shader material is written, whether or not a scene names it
            else if (isShapeType(t))
                report_.add("resource", res, "", t, "", Rating::Converted, "converted where a scene uses it", 0);
            else
                report_.add("resource", res, "", t, "", Rating::Unsupported, "a " + t + " resource has no Aver counterpart yet", 0);
        } else if (isModelExt(ext)) {
            modelFor(res, w);
        } else if (isAudioExt(ext)) {
            audioFileAsset(res, w);   // every sound is copied, so a script's load("res://x.wav") finds it too
        } else if (isFontExt(ext)) {
            fontAsset(res, w);        // G5 U1: every font file is copied, so a theme or a script's load("res://ui.ttf") finds it
        }
    }
    // Textures last, so the ones a material already carried into Content/Textures/<Name>/ are not copied a second time.
    for (const std::string& res : fs_.files()) {
        const std::string ext = extOf(res);
        if ((isImageExt(ext) || isOtherTextureExt(ext)) && !materialTextureSources_.count(res)) copyTexture(res, Where{res, "", ""});
    }
}

void Converter::reportProjectFiles() {
    for (const std::string& res : fs_.binaryResources())
        report_.add("file", res, "", "", "", Rating::Unsupported, "a binary Godot resource: resave it as text (.tscn / .tres) and convert again", 0);
    for (const std::string& res : fs_.files()) {
        const std::string ext = extOf(res);
        if (ext == ".gdshader") {   // G6 SH2: the shaders a ShaderMaterial used are reported where they were prepared
            if (!shaders_.count(res))
                report_.add("file", res, "", "Shader", "", Rating::Approximated, "no ShaderMaterial uses this shader, so nothing was written for it", 0);
        } else if (ext == ".gdshaderinc") {
            report_.add("file", res, "", "Shader", "", Rating::Converted, "included into the shaders that name it", 0);
        } else if (ext == ".gdextension")
            continue;   // FabricExtensions.cpp reports it
        else if (ext == ".wav" || ext == ".ogg" || ext == ".mp3")
            continue;   // FabricAudio.cpp copies and reports it
        else if (ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".woff2")
            continue;   // FabricUi.cpp copies and reports it
        else if (ext == ".fnt")
            report_.add("file", res, "", "Font", "", Rating::Unsupported, "bitmap font files (.fnt) are not converted", 0);
        else if (ext == ".ogv" || ext == ".webm")
            report_.add("file", res, "", "VideoStream", "", Rating::Unsupported,
                        "the engine has no video decoder (modules/media only encodes H.264 through Media Foundation); a VideoStreamPlayer needs a decoder dependency, see docs/AUDIO.md", 0);
        else if (ext == ".translation" || ext == ".csv")
            report_.add("file", res, "", "Translation", "", Rating::Unsupported, "only .po translations are converted", 0);
        else if (ext == ".theme")
            report_.add("file", res, "", "Theme", "", Rating::Unsupported, "a binary Godot theme file: resave it as a text .tres and convert again", 0);
        else if (ext == ".anim" || ext == ".animlib")
            report_.add("file", res, "", "Animation", "", Rating::Unsupported, "a binary Godot animation file: resave it as a text .tres and convert again", 0);
    }
}

}  // namespace aver::fabric
