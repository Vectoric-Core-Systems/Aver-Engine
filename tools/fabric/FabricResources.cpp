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
           t == "PrismMesh" || t == "TorusMesh";
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

std::string Converter::materialAsset(const std::string& key, const std::string& type, const std::vector<gd::GodotProperty>& props,
                                     const gd::GodotDocBase& doc, const std::string& nameHint, const Where& w) {
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

    // Albedo: Godot stores the colour as displayed (sRGB); the .ocmat holds sRGB too.
    m.baseColorFactorLinear = false;
    if (const Var* c = get("albedo_color"); c && c->type == VType::Color && c->nums.size() >= 4)
        for (int k = 0; k < 4; ++k) m.baseColorFactor[k] = static_cast<float>(c->nums[k]);
    bind(m.baseColorTex, tex("albedo_texture"));

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
        m.occlusionStrength = static_cast<float>(numOf(get("ao_light_affect"), 0.0));
        if (m.occlusionTex.imageIndex >= 0 && m.occlusionStrength == 0.0f) m.occlusionStrength = 1.0f;
    }
    if (boolOf(get("emission_enabled"), false)) {
        const double energy = numOf(get("emission_energy_multiplier"), 1.0);
        if (const Var* c = get("emission"); c && c->type == VType::Color && c->nums.size() >= 3)
            for (int k = 0; k < 3; ++k) m.emissiveFactor[k] = srgbToLinear(c->nums[k]) * static_cast<float>(energy);
        else
            for (int k = 0; k < 3; ++k) m.emissiveFactor[k] = static_cast<float>(energy);   // Godot's emission default is black, so this is only for a texture
        bind(m.emissiveTex, tex("emission_texture"));
        report_.add("property", file, w.node, type, "emission_energy_multiplier", Rating::Approximated,
                    "Godot's emission energy is scene-referred; the value is carried over as the .ocmat emissive factor without a unit calibration", 0);
    }

    switch (static_cast<int>(numOf(get("transparency"), 0))) {
        case 1: m.alphaMode = "BLEND"; break;
        case 2: m.alphaMode = "MASK"; break;
        case 3:
            m.alphaMode = "MASK";
            report_.add("property", file, w.node, type, "transparency", Rating::Approximated, "alpha hash is written as alpha scissor", 0);
            break;
        case 4:
            m.alphaMode = "BLEND";
            report_.add("property", file, w.node, type, "transparency", Rating::Approximated, "alpha depth pre-pass is written as plain alpha blend", 0);
            break;
        default: break;
    }
    m.alphaCutoff = static_cast<float>(numOf(get("alpha_scissor_threshold"), 0.5));

    const int cull = static_cast<int>(numOf(get("cull_mode"), 0));
    m.doubleSided = cull == 2;
    if (cull == 1) report_.add("property", file, w.node, type, "cull_mode", Rating::Unsupported, "front-face culling has no .ocmat equivalent; the material culls back faces", 0);

    if (boolOf(get("subsurf_scatter_enabled"), false)) {
        m.subsurfaceWeight = static_cast<float>(std::min(1.0, numOf(get("subsurf_scatter_strength"), 0.0)));
        m.subsurfaceRadius = 0.5f;
        report_.add("property", file, w.node, type, "subsurf_scatter_strength", Rating::Approximated, "mapped to the .ocmat subsurface weight with a fixed radius", 0);
    }

    // Features the .ocmat has no field for; each one is listed rather than dropped silently.
    for (const gd::GodotProperty& p : props) {
        const std::string& n = p.name;
        static const std::set<std::string> handled = {
            "albedo_color", "albedo_texture", "metallic", "roughness", "orm_texture", "roughness_texture", "metallic_texture",
            "roughness_texture_channel", "metallic_texture_channel", "normal_enabled", "normal_scale", "normal_texture",
            "ao_enabled", "ao_texture", "ao_texture_channel", "ao_light_affect", "emission_enabled", "emission", "emission_energy_multiplier",
            "emission_texture", "transparency", "alpha_scissor_threshold", "cull_mode", "subsurf_scatter_enabled", "subsurf_scatter_strength"};
        if (handled.count(n) || ignorableResourceProp(n)) continue;
        if (n == "uv1_scale" || n == "uv1_offset") {
            const V3 v = vec3Of(&p.value, n == "uv1_scale" ? V3{1, 1, 1} : V3{0, 0, 0});
            const bool unchanged = n == "uv1_scale" ? (v.x == 1 && v.y == 1) : (v.x == 0 && v.y == 0);
            if (!unchanged)
                report_.add("property", file, w.node, type, n, Rating::Unsupported,
                            "the .ocmat has no UV scale or offset (only world-aligned tiling); the texture is not tiled or shifted", p.line);
            continue;
        }
        if (n == "shading_mode" && static_cast<int>(numOf(&p.value, 1)) == 1) continue;
        if (n == "blend_mode" && static_cast<int>(numOf(&p.value, 0)) == 0) continue;
        if ((n == "texture_filter" || n == "texture_repeat" || n == "specular_mode" || n == "diffuse_mode" || n == "metallic_specular")) {
            report_.add("property", file, w.node, type, n, Rating::Approximated, "the engine's shading model has no such switch; its default is used", p.line);
            continue;
        }
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
            "inner_radius", "outer_radius", "ring_segments", "subdivide_depth", "material", "flip_faces"};
        if (known.count(p.name) || ignorableResourceProp(p.name)) continue;
        report_.add("property", doc.file, w.node, type, p.name, Rating::Approximated, "not used by the generated mesh", p.line);
    }
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
            if (t == "ShaderMaterial") {
                report_.add("resource", doc.file, w.node, t, "", Rating::Unsupported, "shader materials need the Godot shader front-end (G6)", sr->line);
                return makeNil();
            }
            if (isPrimitiveMeshType(t)) {
                const std::string a = primitiveMeshAsset(t, sr->props, doc, w);
                return a.empty() ? makeNil() : makeResource(a);
            }
            if (t == "ArrayMesh" || t == "ImmediateMesh" || t == "PointMesh") {
                report_.add("resource", doc.file, w.node, t, "", Rating::Unsupported,
                            "the surface data is Godot's packed vertex format and is not decoded; re-export the mesh as .glb", sr->line);
                return makeNil();
            }
            if (isShapeType(t)) return shapeObject(t, sr->props, doc, w);
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
                if (rt == "ShaderMaterial") {
                    report_.add("resource", res, w.node, rt, "", Rating::Unsupported, "shader materials need the Godot shader front-end (G6)", 0);
                    return makeNil();
                }
                if (isPrimitiveMeshType(rt)) {
                    const std::string a = primitiveMeshAsset(rt, r->props, *r, w);
                    return a.empty() ? makeNil() : makeResource(a);
                }
                if (isShapeType(rt)) return shapeObject(rt, r->props, *r, w);
                if (rt == "ArrayMesh") {
                    report_.add("resource", res, w.node, rt, "", Rating::Unsupported,
                                "the surface data is Godot's packed vertex format and is not decoded; re-export the mesh as .glb", 0);
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
            if (t == "ShaderMaterial")
                report_.add("resource", res, "", t, "", Rating::Unsupported, "shader materials need the Godot shader front-end (G6)", 0);
            else if (isShapeType(t))
                report_.add("resource", res, "", t, "", Rating::Converted, "converted where a scene uses it", 0);
            else
                report_.add("resource", res, "", t, "", Rating::Unsupported, "a " + t + " resource has no Aver counterpart yet", 0);
        } else if (isModelExt(ext)) {
            modelFor(res, w);
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
        if (ext == ".gdshader" || ext == ".gdshaderinc")
            report_.add("file", res, "", "Shader", "", Rating::Unsupported, "Godot shaders need the shader front-end (G6)", 0);
        else if (ext == ".gdextension")
            continue;   // FabricExtensions.cpp reports it
        else if (ext == ".wav" || ext == ".ogg" || ext == ".mp3")
            report_.add("file", res, "", "AudioStream", "", Rating::Unsupported, "audio nodes are not in the runtime yet; the file is not imported", 0);
        else if (ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".woff2" || ext == ".fnt")
            report_.add("file", res, "", "Font", "", Rating::Unsupported, "fonts belong to the UI package", 0);
        else if (ext == ".ogv" || ext == ".webm")
            report_.add("file", res, "", "VideoStream", "", Rating::Unsupported, "the engine has no video decoder", 0);
        else if (ext == ".translation" || ext == ".csv")
            report_.add("file", res, "", "Translation", "", Rating::Unsupported, "only .po translations are converted", 0);
        else if (ext == ".theme")
            report_.add("file", res, "", "Theme", "", Rating::Unsupported, "themes belong to the UI package", 0);
        else if (ext == ".anim" || ext == ".animlib")
            report_.add("file", res, "", "Animation", "", Rating::Unsupported, "AnimationPlayer is not in the runtime yet", 0);
    }
}

}  // namespace aver::fabric
