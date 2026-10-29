// Godot ShaderMaterial to Aver (docs/plans/fabric/G6_SHADERS.md, SH2).
//
// A shader (a .gdshader file or an inline Shader resource) is prepared once by the front-end (GodotShaderEngine.hpp) and written as a DOMAIN material
// graph, Shaders/<name>.ocgraph, holding the emitted HLSL in one GodotShader node. Each ShaderMaterial that uses it becomes Materials/<name>.ocmat: a
// GRAPHREF to that graph, the FX record the render modes ask for, GDREGS (the uniform table at the material's shader_parameter/* values) and a TEX per
// texture parameter in the material slot the shader's sampler was given. StandardMaterial3D and ORMMaterial3D are not touched (FabricResources.cpp).
#include "FabricInternal.hpp"

#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphGodot.hpp"
#include "aver/formats/godot/GodotShaderEngine.hpp"
#include "aver/formats/godot/GodotShaderEnv.hpp"
#include "aver/formats/godot/GodotVisualShader.hpp"
#include "aver/platform/FileSystem.hpp"

#if AVER_HAVE_MATERIAL_COMPILE
#include "aver/formats/OcMat.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace aver::fabric {

namespace {

using aver::fmt::godot::GdBase;
using aver::fmt::godot::GdEngineShader;
using aver::fmt::godot::GdEngineUniform;
using aver::fmt::godot::GdKind;
using aver::fmt::godot::GdType;
using aver::fmt::godot::GdUniformScope;

bool ignorableResourceProp(const std::string& n) {
    return n == "resource_local_to_scene" || n == "resource_name" || n == "resource_path" || n == "script" || n == "metadata/_custom_type_script";
}

const Var* getProp(const std::vector<gd::GodotProperty>& props, const std::string& name) {
    for (const gd::GodotProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

std::string extLowerOf(const std::string& path) {
    const usize dot = path.find_last_of('.');
    const usize slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(path.substr(dot));
}

// One shader value as the numbers gdPackUniform takes (a matrix column-major, an array element after element).
bool elementNumbers(const Var& v, GdBase base, std::vector<double>& out) {
    const GdKind k = aver::fmt::godot::gdKind(base);
    const int n = aver::fmt::godot::gdDim(base);
    switch (v.type) {
        case VType::Bool: out.push_back(v.b ? 1.0 : 0.0); return n == 1;
        case VType::Int: out.push_back(static_cast<double>(v.i)); return n == 1;
        case VType::Float: out.push_back(v.f); return n == 1;
        default: break;
    }
    if (k == GdKind::Matrix) {
        if (v.type == VType::Basis && n == 3) {   // the nine numbers are rows as written; the shader's mat3 holds the basis columns
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r) out.push_back(v.num(static_cast<usize>(r * 3 + c)));
            return true;
        }
        if (v.type == VType::Transform3D && n == 4) {   // basis rows, then the origin
            for (int c = 0; c < 3; ++c) { for (int r = 0; r < 3; ++r) out.push_back(v.num(static_cast<usize>(r * 3 + c))); out.push_back(0.0); }
            out.push_back(v.num(9)); out.push_back(v.num(10)); out.push_back(v.num(11)); out.push_back(1.0);
            return true;
        }
        if (v.type == VType::Projection && n == 4) { for (int i = 0; i < 16; ++i) out.push_back(v.num(static_cast<usize>(i))); return true; }
        if (v.type == VType::Transform2D && n == 3) {   // xx, xy, yx, yy, ox, oy: columns x, y, origin
            const double m[9] = {v.num(0), v.num(1), 0.0, v.num(2), v.num(3), 0.0, v.num(4), v.num(5), 1.0};
            for (const double d : m) out.push_back(d);
            return true;
        }
        return false;
    }
    switch (v.type) {
        case VType::Vector2: case VType::Vector2i: case VType::Vector3: case VType::Vector3i: case VType::Vector4: case VType::Vector4i:
        case VType::Color: case VType::Quaternion: case VType::Plane:
            for (int i = 0; i < n; ++i) out.push_back(v.num(static_cast<usize>(i)));
            return true;
        default: return false;
    }
}

bool valueNumbers(const Var& v, const GdType& t, std::vector<double>& out) {
    out.clear();
    if (!t.isArray()) return elementNumbers(v, t.base, out);
    const int n = aver::fmt::godot::gdDim(t.base);
    if (v.type == VType::Array) {
        for (const Var& item : v.items) if (!elementNumbers(item, t.base, out)) return false;
        return true;
    }
    if (v.type == VType::PackedFloat32Array || v.type == VType::PackedFloat64Array || v.type == VType::PackedInt32Array || v.type == VType::PackedInt64Array ||
        v.type == VType::PackedVector2Array || v.type == VType::PackedVector3Array || v.type == VType::PackedVector4Array || v.type == VType::PackedColorArray) {
        out = v.nums;
        (void)n;
        return true;
    }
    return false;
}

// Godot's project defaults for a `[shader_globals]` entry: { "type": "float", "value": 1.0 }.
void readGlobals(const gd::GodotProject& project, aver::fmt::godot::GdEngineOptions& opt, std::vector<std::pair<std::string, std::string>>& types) {
    const gd::GodotConfigSection* sec = project.config.section("shader_globals");
    if (!sec) return;
    for (const gd::GodotProperty& p : sec->entries) {
        opt.globalNames.push_back(p.name);
        const Var* type = p.value.find("type");
        const Var* value = p.value.find("value");
        types.emplace_back(p.name, type ? type->asString() : std::string());
        if (!type || !value) continue;
        const GdBase base = aver::fmt::godot::gdBaseFromName(type->asString() == "color" ? "vec4" : type->asString());
        if (base == GdBase::Error) continue;
        aver::fmt::godot::GdConstValue cv;
        cv.type.base = base;
        if (elementNumbers(*value, base, cv.v)) { cv.valid = true; opt.globalDefaults[p.name] = cv; }
    }
}

}  // namespace

std::string Converter::shaderMaterialAsset(const std::string& materialKey, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc,
                                           const std::string& nameHint, const Where& w) {
    if (const auto it = shaderMaterialAssets_.find(materialKey); it != shaderMaterialAssets_.end()) return it->second;
    shaderMaterialAssets_[materialKey] = {};
#if AVER_HAVE_MATERIAL_COMPILE
    namespace gde = aver::fmt::godot;
    const auto fail = [&](const std::string& why) {
        report_.add("resource", doc.file, w.node, "ShaderMaterial", "", Rating::Unsupported, why, 0);
        return std::string();
    };

    // ---- the shader: a .gdshader file, a Shader resource file, or an inline Shader sub-resource ----
    std::string shaderKey, source, srcFile;
    const Var* sv = getProp(props, "shader");
    if (!sv || sv->type == VType::Nil) return fail("the ShaderMaterial has no shader");
    if (sv->type == VType::ExtResource) {
        const gd::GodotExtResource* e = doc.findExt(sv->s);
        const std::string res = e ? fs_.resolve(*e, &diag_, doc.file) : std::string();
        if (res.empty()) return fail("the shader resource " + sv->s + " could not be resolved");
        const std::string ext = extLowerOf(res);
        shaderKey = res;
        srcFile = res;
        if (ext == ".gdshader") {
            if (!fs_.readText(res, source, diag_)) return fail("the shader file " + res + " could not be read");
        } else if (ext == ".tres" || ext == ".res") {
            const gd::GodotResource* r = resourceDoc(res);
            if (r && r->header.type == "VisualShader") {   // G6 SH3
                if (!visualShaderSource(*r, res, source)) return fail("the VisualShader " + res + " did not convert");
            } else {
                const Var* code = r ? getProp(r->props, "code") : nullptr;
                if (!r || r->header.type != "Shader" || !code) return fail(res + " is not a Shader resource with a code property");
                source = code->asString();
            }
        } else {
            return fail("the shader " + res + " has an unknown file type");
        }
    } else if (sv->type == VType::SubResource) {
        const gd::GodotSubResource* sr = doc.findSub(sv->s);
        const Var* code = sr ? getProp(sr->props, "code") : nullptr;
        if (!sr || sr->type != "Shader" || !code) return fail("the shader sub-resource " + sv->s + " is not a Shader with a code property");
        shaderKey = doc.file + "#" + sr->id;
        srcFile = doc.file + "#" + sr->id;
        source = code->asString();
    } else {
        return fail("the shader property holds a " + std::string(gd::godotVTypeName(sv->type)) + ", not a Shader");
    }

    // G6 SH3: a sky, fog or particles shader used by an environment, a fog volume or a particle emitter runs there instead of on a mesh material.
    if (const int envKind = envShaderKindOf(source); envKind >= 0 &&
        (w.type == "WorldEnvironment" || w.type == "Environment" || w.type == "Sky" || w.type == "FogVolume" || w.type == "GPUParticles3D"))
        return envShaderAsset(materialKey, envKind, source, shaderKey, srcFile, props, doc, nameHint, w);

    // ---- prepare it once ----
    auto sit = shaders_.find(shaderKey);
    if (sit == shaders_.end()) {
        ShaderInfo si;
        si.res = shaderKey;
        std::string stem = shaderKey.find('#') != std::string::npos ? stemOf(doc.file) + "_" + shaderKey.substr(shaderKey.find('#') + 1) : stemOf(shaderKey);
        stem = safeName(stem.empty() ? nameHint : stem, "Shader");
        std::string unique = stem;
        for (int n = 2; shaderStemsUsed_.count(unique); ++n) unique = stem + "_" + std::to_string(n);
        shaderStemsUsed_.insert(unique);
        si.stem = unique;
        gde::GdEngineOptions opt;
        opt.name = unique;
        readGlobals(project_, opt, si.globalTypes);
        opt.includeResolver = [&](const std::string& inc, const std::string& from, std::string& text, std::string& resolved) {
            std::string ref = inc;
            if (!gd::GodotProjectFs::isResPath(ref) && !gd::GodotProjectFs::isUid(ref)) {
                std::string dir = from.rfind("res://", 0) == 0 ? from.substr(0, from.find_last_of('/') + 1) : std::string("res://");
                ref = gd::GodotProjectFs::normalizeRes(dir + inc);
            }
            const std::string res = gd::GodotProjectFs::isUid(ref) ? fs_.resolve(ref, {}, &diag_, from) : ref;
            if (res.empty() || !fs_.readText(res, text, diag_)) return false;
            resolved = res;
            return true;
        };
        const std::string fileForDiag = srcFile.rfind("res://", 0) == 0 && srcFile.find('#') == std::string::npos ? srcFile : (unique + ".gdshader");
        si.ok = gde::gdShaderForEngine(source, fileForDiag, opt, si.prepared);
        si.graphAsset = "Shaders/" + unique + ".ocgraph";
        if (si.ok) {
            fmt::GodotShaderPart part;
            part.name = si.prepared.name;
            part.ns = si.prepared.ns;
            part.regsU = static_cast<u32>(si.prepared.uniforms.registerCount);
            part.regsI = static_cast<u32>(si.prepared.instanceUniforms.registerCount);
            part.regsG = static_cast<u32>(si.prepared.globalUniforms.registerCount);
            part.fx = si.prepared.fxBits;
            part.hasFragment = si.prepared.hasFragment;
            part.hasVertex = si.prepared.hasVertex;
            part.fragmentSource = si.prepared.fragmentSource;
            part.vertexSource = si.prepared.vertexSource;
            for (const gde::GdVaryingSlot& v : si.prepared.varyings) part.varyings.push_back(fmt::GodotShaderVarying{v.decl, v.flat, static_cast<u32>(v.rows)});
            fmt::OcGraphData g;
            g.name = unique;
            g.description = "Converted from " + shaderKey + " by AverAssetC fabric";
            fmt::packGodotShader(part, g);
            createDirectories(contentDir_ + "/Shaders");
            if (!writeFileText(contentDir_ + "/" + si.graphAsset, fmt::writeOcgraph(g))) {
                si.ok = false;
                si.prepared.error = "could not write " + si.graphAsset;
            }
        }
        // The report: the shader's own diagnostics once, then what it asks of the engine that the engine does only approximately.
        if (!si.ok) {
            report_.add("resource", srcFile, "", "Shader", "", Rating::Unsupported, "not converted: " + si.prepared.error, 0);
        } else {
            for (const gde::GdShaderDiag& d : si.prepared.diags)
                if (d.severity == gd::GodotSeverity::Warning) report_.add("resource", srcFile, "", "Shader", d.code, Rating::Approximated, d.str(), d.line);
            for (const gde::GdEngineNote& n : si.prepared.notes)
                report_.add("resource", srcFile, "", "Shader", n.subject, n.severity == "unsupported" ? Rating::Unsupported : Rating::Approximated, n.reason, 0);
            report_.add("resource", srcFile, "", "Shader", "", Rating::Converted, "written as " + si.graphAsset + " (" + std::to_string(si.prepared.defaultRegisters.size() / 4) + " uniform registers)", 0);
        }
        sit = shaders_.emplace(shaderKey, std::move(si)).first;
    }
    ShaderInfo& si = sit->second;
    if (!si.ok) return fail("its shader did not convert: " + si.prepared.error);
    const gde::GdEngineShader& ps = si.prepared;

    // ---- the material ----
    pbr::MaterialDesc d;
    std::string name = stringOf(getProp(props, "resource_name"));
    if (name.empty()) name = nameHint.empty() ? si.stem : nameHint;
    name = safeName(name, "ShaderMaterial");
    std::string unique = name;
    for (int n = 2; materialStemsUsed_.count(name_ + "_" + unique); ++n) unique = name + "_" + std::to_string(n);
    materialStemsUsed_.insert(name_ + "_" + unique);
    const std::string stem = name_ + "_" + unique;
    d.name = stem;
    d.baseColorFactor[0] = d.baseColorFactor[1] = d.baseColorFactor[2] = 1.0f;
    d.metallicFactor = 0.0f;
    d.roughnessFactor = 1.0f;
    d.castShadow = true;
    d.twoSided = ps.cull == 3;
    if (ps.writesAlphaScissor) d.alphaMode = pbr::AlphaMode::Mask;
    else if (ps.writesAlpha || ps.blend != 0) d.alphaMode = pbr::AlphaMode::Blend;

    std::string fx;
    const auto addFx = [&](const std::string& kv) { if (!fx.empty()) fx += ' '; fx += kv; };
    addFx(ps.cull == 2 ? "cull=front" : ps.cull == 3 ? "cull=none" : "cull=back");
    static const char* const kBlend[] = {"", "blend=add", "blend=sub", "blend=mul", "blend=premult"};
    if (ps.blend > 0) addFx(kBlend[ps.blend]);
    if (ps.depthDraw == 1) addFx("depthdraw=always");
    else if (ps.depthDraw == 2) addFx("depthdraw=never");
    if (ps.depthTestDisabled) addFx("nodepthtest=1");
    if (ps.shadowsDisabled) addFx("noshadows=1");
    if (ps.hasVertex) addFx("gdvs=1");
    if (ps.usesDiscard || ps.writesAlphaScissor) addFx("gddiscard=1");
    if (const Var* rp = getProp(props, "render_priority"); rp && rp->isNumber()) {
        addFx("priority=" + std::to_string(static_cast<int>(rp->asNumber())));
    }
    fmt::parseOcmatFx(fx, d.fx);

    // ---- shader_parameter/* ----
    std::vector<float> regs = ps.defaultRegisters;
    std::set<std::string> handled = {"shader", "render_priority", "resource_name", "next_pass"};
    for (const gd::GodotProperty& p : props) {
        if (p.name.rfind("shader_parameter/", 0) != 0) continue;
        const std::string pname = p.name.substr(17);
        handled.insert(p.name);
        const gde::GdEngineSampler* sampler = nullptr;
        for (const gde::GdEngineSampler& s : ps.samplers) if (s.name == pname) sampler = &s;
        if (sampler) {
            if (!sampler->bindable) {
                report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported, sampler->reason, p.line);
                continue;
            }
            if (p.value.type == VType::Nil) continue;
            std::string ref;
            if (p.value.type == VType::ExtResource) {
                const gd::GodotExtResource* e = doc.findExt(p.value.s);
                const std::string res = e ? fs_.resolve(*e, &diag_, doc.file) : std::string();
                if (res.empty()) { report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported, "the texture " + p.value.s + " could not be resolved", p.line); continue; }
                ref = copyTexture(res, w);
            } else if (p.value.type == VType::SubResource) {
                ref = viewportReference(&p.value, doc, w, p.name);
                if (ref.empty()) { report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported, "only a texture file or a ViewportTexture reaches a shader texture slot", p.line); continue; }
            } else {
                report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported, "a " + std::string(gd::godotVTypeName(p.value.type)) + " is not a texture", p.line);
                continue;
            }
            if (ref.empty()) continue;
            d.textures[static_cast<usize>(sampler->slot)].path = ref;
            report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Converted, "bound to material texture slot " + std::to_string(sampler->slot), p.line);
            continue;
        }
        const gde::GdEngineUniform* uni = nullptr;
        for (const gde::GdEngineUniform& u : ps.params) if (u.name == pname) uni = &u;
        if (!uni) { report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Approximated, "the shader declares no uniform of this name; the value is ignored", p.line); continue; }
        std::vector<double> nums;
        if (!valueNumbers(p.value, uni->type, nums)) {
            report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported,
                        "a " + std::string(gd::godotVTypeName(p.value.type)) + " does not fit the uniform's type " + gde::gdTypeName(uni->type), p.line);
            continue;
        }
        gde::gdPackUniform(*uni, nums, regs);
        report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Converted, "stored in the material's uniform table", p.line);
    }
    for (const gd::GodotProperty& p : props) {
        if (handled.count(p.name) || ignorableResourceProp(p.name)) continue;
        report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported, "the .ocmat has no equivalent", p.line);
    }
    if (const Var* np = getProp(props, "next_pass"); np && np->type != VType::Nil)
        report_.add("property", doc.file, w.node, "ShaderMaterial", "next_pass", Rating::Unsupported, "the engine draws one pass per material; the next pass is dropped", 0);
    if (!regs.empty()) {
        d.gd.regs = regs;
        d.textures[pbr::kGdTableSlot].path = "@gdu";
    }

    fmt::OcMatExtras ex;
    ex.graphRef = si.graphAsset;
    ex.cull = ps.cull == 3 ? "none" : (ps.cull == 2 ? "front" : "back");
    createDirectories(contentDir_ + "/Materials");
    const std::string asset = "Materials/" + stem + ".ocmat";
    std::string err;
    if (!fmt::saveOcmat(contentDir_ + "/" + asset, d, &ex, &err)) return fail("the material could not be written: " + err);
    si.materials.push_back(asset);
    report_.add("resource", doc.file, "", "ShaderMaterial", "", Rating::Converted, materialKey + " written as " + asset + " running " + si.graphAsset, 0);
    shaderMaterialAssets_[materialKey] = asset;
    return asset;
#else
    (void)props; (void)doc; (void)nameHint;
    report_.add("resource", doc.file, w.node, "ShaderMaterial", "", Rating::Unsupported, "this build of AverAssetC has no material compiler (AVER_MODULE_PBR is off)", 0);
    return {};
#endif
}

// ---- G6 SH3: sky, fog and particles shaders; VisualShader resources -----------------------------------------------------------------------------------

int envShaderKindOf(const std::string& source) {
    // The first `shader_type X` outside comments.
    usize i = 0;
    while (i < source.size()) {
        if (source.compare(i, 2, "//") == 0) { i = source.find('\n', i); if (i == std::string::npos) break; continue; }
        if (source.compare(i, 2, "/*") == 0) { i = source.find("*/", i + 2); if (i == std::string::npos) break; i += 2; continue; }
        if (source.compare(i, 11, "shader_type") == 0) {
            usize j = i + 11;
            while (j < source.size() && (source[j] == ' ' || source[j] == '\t')) ++j;
            usize k = j;
            while (k < source.size() && (std::isalnum(static_cast<unsigned char>(source[k])) || source[k] == '_')) ++k;
            const std::string t = source.substr(j, k - j);
            return t == "sky" ? 0 : t == "fog" ? 1 : t == "particles" ? 2 : -1;
        }
        ++i;
    }
    return -1;
}

bool Converter::visualShaderSource(const gd::GodotResource& r, const std::string& file, std::string& source) {
    const aver::fmt::godot::GdVisualShaderResult v = aver::fmt::godot::gdVisualShaderToCode(r);
    if (!v.ok) {
        report_.add("resource", file, "", "VisualShader", "", Rating::Unsupported, "not converted: " + v.error, 0);
        return false;
    }
    for (const auto& n : v.unsupported)
        report_.add("resource", file, "", "VisualShader", n.function + "/" + std::to_string(n.id) + " " + n.type, Rating::Unsupported, n.reason + "; its outputs read their unconnected defaults", 0);
    for (const auto& n : v.approximated)
        report_.add("resource", file, "", "VisualShader", n.function + "/" + std::to_string(n.id) + " " + n.type, Rating::Approximated, n.reason, 0);
    report_.add("resource", file, "", "VisualShader", "", v.unsupported.empty() ? Rating::Converted : Rating::Approximated,
                "converted to " + v.shaderType + " shader text (" + std::to_string(v.nodeCount) + " nodes)", 0);
    visualShaderCode_[file] = v.code;
    createDirectories(fabricDir_ + "/visualshaders");
    writeFileText(fabricDir_ + "/visualshaders/" + safeName(stemOf(file), "VisualShader") + ".gdshader.txt", v.code);   // Aver.Fabric's VisualShader.Code
    source = v.code;
    return true;
}

std::string Converter::envShaderAsset(const std::string& materialKey, int kindIndex, const std::string& source, const std::string& shaderKey, const std::string& srcFile,
                                      const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const std::string& nameHint, const Where& w) {
    namespace gde = aver::fmt::godot;
    const char* kindName = kindIndex == 0 ? "sky" : kindIndex == 1 ? "fog" : "particles";
    const auto fail = [&](const std::string& why) {
        report_.add("resource", doc.file, w.node, "ShaderMaterial", "", Rating::Unsupported, why, 0);
        return std::string();
    };
    gde::GdEngineOptions globals;
    std::vector<std::pair<std::string, std::string>> globalTypes;
    readGlobals(project_, globals, globalTypes);
    gde::GdEnvOptions opt;
    opt.globalDefaults = globals.globalDefaults;
    opt.globalNames = globals.globalNames;
    std::string name = stringOf(getProp(props, "resource_name"));
    if (name.empty()) name = nameHint.empty() ? stemOf(shaderKey) : nameHint;
    name = safeName(name, "Shader");
    std::string unique = name;
    for (int n = 2; shaderStemsUsed_.count(unique + "." + kindName); ++n) unique = name + "_" + std::to_string(n);
    shaderStemsUsed_.insert(unique + "." + kindName);
    opt.name = unique;
    opt.includeResolver = [&](const std::string& inc, const std::string& from, std::string& text, std::string& resolved) {
        std::string ref = inc;
        if (!gd::GodotProjectFs::isResPath(ref) && !gd::GodotProjectFs::isUid(ref)) {
            std::string dir = from.rfind("res://", 0) == 0 ? from.substr(0, from.find_last_of('/') + 1) : std::string("res://");
            ref = gd::GodotProjectFs::normalizeRes(dir + inc);
        }
        const std::string res = gd::GodotProjectFs::isUid(ref) ? fs_.resolve(ref, {}, &diag_, from) : ref;
        if (res.empty() || !fs_.readText(res, text, diag_)) return false;
        resolved = res;
        return true;
    };
    const std::string fileForDiag = srcFile.rfind("res://", 0) == 0 && srcFile.find('#') == std::string::npos ? srcFile : (unique + ".gdshader");
    gde::GdEnvShader es;
    if (!gde::gdShaderForEnv(source, fileForDiag, opt, es)) {
        report_.add("resource", srcFile, "", "Shader", "", Rating::Unsupported, "not converted: " + es.error, 0);
        return fail("its " + std::string(kindName) + " shader did not convert: " + es.error);
    }
    for (const gde::GdShaderDiag& d : es.diags)
        if (d.severity == gd::GodotSeverity::Warning) report_.add("resource", srcFile, "", "Shader", d.code, Rating::Approximated, d.str(), d.line);
    for (const gde::GdEngineNote& n : es.notes)
        report_.add("resource", srcFile, "", "Shader", n.subject, n.severity == "unsupported" ? Rating::Unsupported : Rating::Approximated, n.reason, 0);

    // shader_parameter/* into the register table (textures are unbound in these shaders).
    std::vector<float> regs = es.defaultRegisters;
    std::set<std::string> handled = {"shader", "render_priority", "resource_name"};
    for (const gd::GodotProperty& p : props) {
        if (p.name.rfind("shader_parameter/", 0) != 0) continue;
        const std::string pname = p.name.substr(17);
        handled.insert(p.name);
        bool isSampler = false;
        for (const gde::GdEngineSampler& s : es.samplers) if (s.name == pname) isSampler = true;
        if (isSampler) { report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Approximated, "a " + std::string(kindName) + " shader has no texture slots; the shader reads the hint's default colour", p.line); continue; }
        const gde::GdEngineUniform* uni = nullptr;
        for (const gde::GdEngineUniform& u : es.params) if (u.name == pname) uni = &u;
        if (!uni) { report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Approximated, "the shader declares no uniform of this name; the value is ignored", p.line); continue; }
        std::vector<double> nums;
        if (!valueNumbers(p.value, uni->type, nums)) {
            report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported,
                        "a " + std::string(gd::godotVTypeName(p.value.type)) + " does not fit the uniform's type " + gde::gdTypeName(uni->type), p.line);
            continue;
        }
        gde::gdPackUniform(*uni, nums, regs);
        report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Converted, "stored in the shader's register table", p.line);
    }
    for (const gd::GodotProperty& p : props) {
        if (handled.count(p.name) || ignorableResourceProp(p.name)) continue;
        report_.add("property", doc.file, w.node, "ShaderMaterial", p.name, Rating::Unsupported, "a " + std::string(kindName) + " shader has no equivalent for this ShaderMaterial property", p.line);
    }

    fmt::GodotEnvPart part;
    part.kind = kindName;
    part.name = unique;
    part.ns = es.ns;
    part.regs = static_cast<u32>(es.registerCount);
    part.globalRegs = static_cast<u32>(es.globalRegisterCount);
    part.hasStart = es.hasStart;
    part.hasProcess = es.hasProcess;
    part.defaults = regs;
    part.source = es.source;
    fmt::OcGraphData g;
    g.name = unique;
    g.description = "Converted from " + shaderKey + " by AverAssetC fabric";
    fmt::packGodotEnvShader(part, g);
    createDirectories(contentDir_ + "/Shaders");
    const std::string asset = "Shaders/" + unique + "." + kindName + ".ocgraph";
    if (!writeFileText(contentDir_ + "/" + asset, fmt::writeOcgraph(g))) return fail("could not write " + asset);

    ShaderInfo si;
    si.ok = true;
    si.res = materialKey;
    si.stem = unique;
    si.graphAsset = asset;
    si.kind = kindName;
    si.prepared.ok = true;
    si.prepared.name = unique;
    si.prepared.params = es.params;
    si.prepared.samplers = es.samplers;
    si.prepared.defaultRegisters = regs;
    si.globalTypes = globalTypes;
    si.materials.push_back(asset);
    shaders_[materialKey + "#" + kindName] = std::move(si);
    report_.add("resource", doc.file, w.node, "ShaderMaterial", "", Rating::Converted,
                materialKey + " written as " + asset + " (" + std::string(kindName) + " shader, " + std::to_string(regs.size() / 4) + " uniform registers)", 0);
    shaderMaterialAssets_[materialKey] = asset;
    return asset;
}

// GPUParticles3D with a particles ShaderMaterial as its process material: a ParticleEmitter component on the placement plus the effect file it plays. Other process
// materials (ParticleProcessMaterial) are not converted: the node stays a carrier with its properties, as before.
void Converter::emitGpuParticles(const FNode& fn, const std::vector<std::pair<std::string, Var>>& props, fmt::OcWorldPlacement& p, bool quiet) {
    const auto note = [&](const std::string& key, Rating r, const std::string& why, int line = 0) {
        if (!quiet) report_.add("property", fn.reportFile, fn.reportPath, fn.type, key, r, why, line);
    };
    std::string asset;
    for (const auto& kv : props) if (kv.first == "process_material" && kv.second.type == VType::Resource) asset = kv.second.s;
    const std::string suffix = ".particles.ocgraph";
    if (asset.size() <= suffix.size() || asset.compare(asset.size() - suffix.size(), suffix.size(), suffix) != 0 || asset.rfind("Shaders/", 0) != 0) {
        note("process_material", Rating::Unsupported, "only a ShaderMaterial over a particles shader is converted (ParticleProcessMaterial is not): the node keeps its properties and emits nothing");
        return;
    }
    const std::string shaderName = asset.substr(8, asset.size() - 8 - suffix.size());
    const ShaderInfo* info = nullptr;
    for (const auto& kv : shaders_) if (kv.second.graphAsset == asset) info = &kv.second;
    const auto prop = [&](const char* k) -> const Prop* { for (const Prop& pr : fn.props) if (pr.key == k) return &pr; return nullptr; };
    const int amount = std::max(1, static_cast<int>(numOf(prop("amount") ? &prop("amount")->value : nullptr, 8.0)));
    const double lifetime = std::max(0.01, numOf(prop("lifetime") ? &prop("lifetime")->value : nullptr, 1.0));
    const bool oneShot = boolOf(prop("one_shot") ? &prop("one_shot")->value : nullptr, false);
    const bool emitting = boolOf(prop("emitting") ? &prop("emitting")->value : nullptr, true);
    double sizeCm = 100.0;
    if (const Prop* dp = prop("draw_pass_1"); dp && dp->doc && dp->value.type == VType::SubResource) {
        if (const gd::GodotSubResource* sr = dp->doc->findSub(dp->value.s); sr && sr->type == "QuadMesh") {
            if (const Var* sz = getProp(sr->props, "size"); sz && sz->nums.size() >= 2) sizeCm = std::max(sz->nums[0], sz->nums[1]) * 100.0;
        } else {
            note("draw_pass_1", Rating::Approximated, "the draw pass is drawn as a camera-facing sprite 1 m across (only a QuadMesh's size is read); its mesh and material are not used", dp->line);
        }
    }
    char buf[96];
    const auto num = [&](double v) { std::snprintf(buf, sizeof buf, "%.6g", v); return std::string(buf); };
    std::string t = "OCPARTICLE 2\nNAME " + fn.name + "\n";
    if (info) {
        const std::vector<float>& regs = info->prepared.defaultRegisters;
        for (usize r = 0; r < regs.size() / 4 && r < 16; ++r)
            t += "PARAM reg" + std::to_string(r) + " color " + num(regs[r * 4]) + " " + num(regs[r * 4 + 1]) + " " + num(regs[r * 4 + 2]) + " " + num(regs[r * 4 + 3]) + "\n";
    }
    t += "EMITTER " + fn.name + "\nSIM gpu\nSHAPE point 0 0 0\nBLEND premultiplied\n";
    t += "EMISSION " + num(oneShot ? 0.0 : amount / lifetime) + " " + std::to_string(oneShot ? amount : 0) + " " + std::to_string(amount) + "\n";
    t += "LIFETIME " + num(lifetime) + " " + num(lifetime) + "\nDIRECTION 0 0 1 0\nSPEED 0 0\nGRAVITY 0 0 0\nDAMPING 0\n";
    t += "SIZE " + num(sizeCm) + " " + num(sizeCm) + "\nCOLOR start 1 1 1 1\nCOLOR end 1 1 1 1\nGDSHADER " + shaderName + "\n";
    createDirectories(contentDir_ + "/Particles");
    const std::string file = safeName(name_ + "_" + fn.name, "Particles") + ".ocparticle";
    if (!writeFileText(contentDir_ + "/Particles/" + file, t)) { note("process_material", Rating::Unsupported, "could not write Particles/" + file); return; }
    fmt::OcPlacedComponent pe;
    pe.type = "ParticleEmitter";
    pe.kv.emplace_back("effect", "Particles/" + file);
    pe.kv.emplace_back("playing", emitting ? "1" : "0");
    pe.kv.emplace_back("sim", "gpu");
    p.components.push_back(std::move(pe));
    note("process_material", Rating::Converted, "a ShaderMaterial over a particles shader runs as the GPU simulation's custom stage (" + asset + "); start() runs at birth, process() every update");
    note("amount", Rating::Converted, "the emitter's particle cap; the rate is amount / lifetime (one burst of amount with one_shot)");
    note("lifetime", Rating::Converted, "seconds; the shader's LIFETIME");
    note("one_shot", Rating::Converted, "one burst of amount particles, then nothing");
    note("emitting", Rating::Converted, "the emitter starts playing or stopped");
    note("explosiveness", Rating::Approximated, "particles are emitted at an even rate; explosiveness is not applied");
    note("randomness", Rating::Approximated, "the shader's RANDOM_SEED is the engine's per-particle seed; randomness is not applied");
    note("speed_scale", Rating::Approximated, "the simulation runs at the game's time scale; this node's speed_scale is not applied");
    note("preprocess", Rating::Unsupported, "the simulation does not run ahead before the first frame");
    note("local_coords", Rating::Approximated, "particles simulate in world space; local_coords is not applied");
    note("visibility_aabb", Rating::Approximated, "particles are not culled by a box");
    note("draw_order", Rating::Approximated, "the engine sorts translucent particles by depth");
    note("trail_enabled", Rating::Unsupported, "ribbon trails are not mapped");
    note("sub_emitter", Rating::Unsupported, "sub emitters are not mapped (emit_subparticle() is not supported)");
}

bool Converter::writeShadersManifest() {
    if (shaders_.empty()) return true;
    std::string j = "{\n  \"schema\": 1,\n  \"shaders\": [";
    bool firstShader = true;
    for (const auto& kv : shaders_) {
        const ShaderInfo& si = kv.second;
        if (!si.ok) continue;
        const aver::fmt::godot::GdEngineShader& p = si.prepared;
        j += std::string(firstShader ? "" : ",") + "\n    {\"source\": " + jsonString(si.res) + ", \"kind\": " + jsonString(si.kind) + ", \"graph\": " + jsonString(si.graphAsset) + ", \"registers\": " +
             std::to_string(p.defaultRegisters.size() / 4) + ", \"uniforms\": [";
        firstShader = false;
        for (usize i = 0; i < p.params.size(); ++i) {
            const GdEngineUniform& u = p.params[i];
            j += std::string(i ? "," : "") + "\n      {\"name\": " + jsonString(u.name) + ", \"type\": " + jsonString(aver::fmt::godot::gdBaseName(u.type.base)) +
                 ", \"array\": " + std::to_string(u.type.arraySize) + ", \"scope\": " + jsonString(u.scope == GdUniformScope::Global ? "global" : u.scope == GdUniformScope::Instance ? "instance" : "normal") +
                 ", \"sourceColor\": " + (u.sourceColor ? "true" : "false") + ", \"reg\": " + std::to_string(u.slot.reg) + ", \"comp\": " + std::to_string(u.slot.comp) +
                 ", \"registers\": " + std::to_string(u.slot.registers) + ", \"stride\": " + std::to_string(u.slot.elementStride) + "}";
        }
        j += p.params.empty() ? "]" : "\n    ]";
        j += ", \"samplers\": [";
        for (usize i = 0; i < p.samplers.size(); ++i)
            j += std::string(i ? ", " : "") + "{\"name\": " + jsonString(p.samplers[i].name) + ", \"slot\": " + std::to_string(p.samplers[i].slot) + ", \"bindable\": " +
                 (p.samplers[i].bindable ? "true" : "false") + "}";
        j += "], \"materials\": [";
        for (usize i = 0; i < si.materials.size(); ++i) j += std::string(i ? ", " : "") + jsonString(si.materials[i]);
        j += "], \"globalTypes\": {";
        for (usize i = 0; i < si.globalTypes.size(); ++i) j += std::string(i ? ", " : "") + jsonString(si.globalTypes[i].first) + ": " + jsonString(si.globalTypes[i].second);
        j += "}}";
    }
    j += "\n  ]\n}\n";
    return writeFileText(fabricDir_ + "/shaders.json", j);
}

}  // namespace aver::fabric
