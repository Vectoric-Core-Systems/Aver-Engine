// A Godot spatial shader prepared for the engine's material path (GodotShaderEngine.hpp).
#include "aver/formats/godot/GodotShaderEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace aver::fmt::godot {
namespace {

// Mirrors aver::fmt::gdfx (OcGraphGodot.hpp), which this module cannot include; GodotShaderMaterialTest compares the two.
constexpr unsigned kFxUnshaded = 1u << 0, kFxAmbientDisabled = 1u << 1, kFxSpecularDisabled = 1u << 2, kFxDiffuseWrap = 1u << 3, kFxWorldVertexCoords = 1u << 4,
                   kFxSkipVertexTransform = 1u << 5, kFxUsesNormalWrite = 1u << 6;

std::string namePrefixFor(std::string_view name, std::string_view source) {
    u64 h = 14695981039346656037ull;
    for (const char c : name) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    for (const char c : source) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char buf[32];
    std::snprintf(buf, sizeof buf, "gs%08x_", static_cast<unsigned>(h ^ (h >> 32)));
    return buf;
}

struct HookRule { const char* name; const char* severity; const char* reason; bool onlyWritten; };

// Built-ins the engine path cannot honour. A rule with onlyWritten fires when the shader writes the built-in; otherwise when it reads it.
constexpr HookRule kHookRules[] = {
    {"ALPHA_HASH_SCALE", "approximated", "written, not consumed: the engine's alpha hash scale comes from the material FX record", true},
    {"ALPHA_ANTIALIASING_EDGE", "approximated", "written, not consumed: no alpha-to-coverage", true},
    {"ALPHA_TEXTURE_COORDINATE", "approximated", "written, not consumed: no alpha antialiasing", true},
    {"PREMUL_ALPHA_FACTOR", "approximated", "written, not consumed", true},
    {"AO_LIGHT_AFFECT", "approximated", "written, not consumed: occlusion darkens ambient light only", true},
    {"ANISOTROPY_FLOW", "approximated", "written, not consumed: the shared BRDF is isotropic (the lobe is widened by ANISOTROPY)", true},
    {"SSS_TRANSMITTANCE_BOOST", "approximated", "written, not consumed", true},
    {"BENT_NORMAL_MAP", "unsupported", "no bent normal map in the engine", true},
    {"LIGHT_VERTEX", "approximated", "written, not consumed: lighting uses the real surface position", true},
    {"FOG", "unsupported", "no per-pixel fog override: the engine applies its fog after the material", true},
    {"RADIANCE", "unsupported", "no per-pixel reflection override", true},
    {"IRRADIANCE", "unsupported", "no per-pixel irradiance override", true},
    {"DEPTH", "unsupported", "a custom depth output is not supported", true},
    {"POSITION", "unsupported", "writing POSITION is not supported: the engine's clip space differs from Godot's", true},
    {"POINT_SIZE", "unsupported", "no point rendering", true},
    {"Z_CLIP_SCALE", "unsupported", "no z-clip scale", true},
    {"MODELVIEW_MATRIX", "unsupported", "writing MODELVIEW_MATRIX is not supported (read it, or use skip_vertex_transform)", true},
    {"MODELVIEW_NORMAL_MATRIX", "unsupported", "writing MODELVIEW_NORMAL_MATRIX is not supported", true},
    {"PROJECTION_MATRIX", "approximated", "reads the identity: the per-frame block carries no projection matrix", false},
    {"INV_PROJECTION_MATRIX", "approximated", "reads the identity: the per-frame block carries no projection matrix", false},
    {"VIEWPORT_SIZE", "approximated", "reads 1920 x 1080: the per-frame block carries no resolution", false},
    {"FRAGCOORD", "approximated", "xy is SCREEN_UV times an assumed 1920 x 1080; z and w come from the clip position", false},
    {"POINT_COORD", "approximated", "reads (0.5, 0.5): no point sprites", false},
    {"VERTEX_ID", "approximated", "reads 0: the vertex function is not given the vertex index", false},
    {"INSTANCE_ID", "approximated", "reads 0: draws are not instanced", false},
    {"INSTANCE_CUSTOM", "approximated", "reads 0", false},
    {"CUSTOM0", "approximated", "reads 0: custom vertex data is not carried", false},
    {"CUSTOM1", "approximated", "reads 0: custom vertex data is not carried", false},
    {"CUSTOM2", "approximated", "reads 0: custom vertex data is not carried", false},
    {"CUSTOM3", "approximated", "reads 0: custom vertex data is not carried", false},
    {"BONE_INDICES", "approximated", "reads 0: skinning does not run through the shader", false},
    {"BONE_WEIGHTS", "approximated", "reads 0: skinning does not run through the shader", false},
    {"CAMERA_VISIBLE_LAYERS", "approximated", "reads every layer", false},
    {"OUTPUT_IS_SRGB", "approximated", "reads false: the engine tonemaps after the material", false},
    {"CLEARCOAT", "approximated", "drives the layered BSDF's coat, live only with RENDER.LAYEREDBSDF", true},
    {"CLEARCOAT_GLOSS", "approximated", "drives the layered BSDF's coat roughness, live only with RENDER.LAYEREDBSDF", true},
    {"RIM", "approximated", "the sun's rim term only, added as glow", true},
    {"RIM_TINT", "approximated", "the sun's rim term only", true},
    {"BACKLIGHT", "approximated", "the sun's backlight term only, added as glow", true},
    {"ANISOTROPY", "approximated", "widens the specular lobe; no stretched highlight", true},
    {"SSS_STRENGTH", "approximated", "drives the wrap / transmission terms, not a screen-space blur", true},
    {"SSS_TRANSMITTANCE_COLOR", "approximated", "the scattered light's tint", true},
    {"SSS_TRANSMITTANCE_DEPTH", "approximated", "scales the scatter depth", true},
    {"NORMAL", "approximated", "a written NORMAL becomes a tangent-space perturbation of the geometric normal", true},
    {"AO", "approximated", "drives the ambient occlusion term", true},
};

void addNote(GdEngineShader& r, const char* severity, const std::string& subject, const std::string& reason) {
    for (const GdEngineNote& n : r.notes) if (n.subject == subject && n.reason == reason) return;
    r.notes.push_back(GdEngineNote{severity, subject, reason});
}

}  // namespace

void gdPackUniform(const GdUniformSlot& slot, const GdUniform& u, const std::vector<double>& value, std::vector<float>& regs) {
    const GdKind k = gdKind(u.type.base);
    const int n = gdDim(u.type.base);
    const auto store = [&](usize at, double v) {
        if (at >= regs.size()) return;
        float f;
        if (k == GdKind::Int) { const int i = static_cast<int>(v); std::memcpy(&f, &i, sizeof f); }
        else if (k == GdKind::UInt) { const unsigned i = static_cast<unsigned>(v); std::memcpy(&f, &i, sizeof f); }
        else if (k == GdKind::Bool) { const unsigned i = v != 0.0 ? 1u : 0u; std::memcpy(&f, &i, sizeof f); }
        else f = static_cast<float>(v);
        regs[at] = f;
    };
    const int count = u.type.isArray() ? u.type.arraySize : 1;
    usize cursor = 0;
    for (int e = 0; e < count; ++e) {
        const int baseReg = slot.reg + e * slot.elementStride;
        if (k == GdKind::Matrix) {
            for (int c = 0; c < n; ++c)
                for (int r = 0; r < n; ++r, ++cursor)
                    store(static_cast<usize>(baseReg + c) * 4 + static_cast<usize>(r), cursor < value.size() ? value[cursor] : 0.0);
        } else {
            // A source_color vec3 / vec4 is authored as sRGB and reaches the shader linear (Godot decodes it on upload; the alpha stays as written).
            const bool decode = u.sourceColor && (u.type.base == GdBase::Vec3 || u.type.base == GdBase::Vec4);
            for (int c = 0; c < n; ++c, ++cursor) {
                double v = cursor < value.size() ? value[cursor] : 0.0;
                if (decode && c < 3) {
                    const double s = std::min(1.0, std::max(0.0, v));
                    v = s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
                }
                store(static_cast<usize>(baseReg) * 4 + static_cast<usize>(slot.comp + c), v);
            }
        }
    }
}

void gdPackUniform(const GdEngineUniform& u, const std::vector<double>& value, std::vector<float>& regs) {
    GdUniform g;
    g.name = u.name;
    g.type = u.type;
    g.scope = u.scope;
    g.sourceColor = u.sourceColor;
    gdPackUniform(u.slot, g, value, regs);
}

int gdValueComponents(const GdType& type) {
    const int n = gdDim(type.base);
    const int per = gdKind(type.base) == GdKind::Matrix ? n * n : n;
    return per * (type.isArray() ? type.arraySize : 1);
}

bool gdShaderForEngine(std::string_view source, std::string_view fileName, const GdEngineOptions& opt, GdEngineShader& r) {
    r = GdEngineShader{};
    r.name = opt.name.empty() ? std::string(fileName) : opt.name;
    GdCompileOptions co;
    co.pre.resolver = opt.includeResolver;
    co.pre.defines = {{"RENDERER_FORWARD_PLUS", "1"}};
    co.globalUniformNames = opt.globalNames;
    GdShaderModule m;
    gdShaderCompile(source, fileName, co, m);
    r.diags = m.diags;
    if (!m.ok) {
        for (const GdShaderDiag& d : m.diags) if (d.severity == GodotSeverity::Error) { r.error = d.str(); break; }
        if (r.error.empty()) r.error = "the shader has errors";
        return false;
    }
    if (m.is2D) { r.error = "shader_type canvas_item (2D) is not supported"; return false; }
    if (m.shaderType != GdShaderType::Spatial) {
        r.error = std::string("shader_type ") + gdShaderTypeName(m.shaderType) + " does not run on a mesh material (a particle, sky or fog shader runs as a GPUParticles3D's process material, the Sky's material or a FogVolume's material: G6 SH3)";
        return false;
    }
    r.hasFragment = m.processor(GdProcessor::Fragment) != nullptr;
    r.hasVertex = m.processor(GdProcessor::Vertex) != nullptr;
    r.hasLight = m.processor(GdProcessor::Light) != nullptr;
    if (r.hasLight) addNote(r, "unsupported", "light()", "the per-light function is not run: the material is lit by the engine's own lights (a toon or custom light() shader loses its look)");

    // ---- samplers -> material texture slots ----
    std::vector<int> sRgbPool(std::begin(kGdEngineSrgbSlots), std::end(kGdEngineSrgbSlots));
    std::vector<int> linPool(std::begin(kGdEngineLinearSlots), std::end(kGdEngineLinearSlots));
    std::reverse(sRgbPool.begin(), sRgbPool.end());
    std::reverse(linPool.begin(), linPool.end());
    std::vector<int> samplerSlots;
    // Bindable samplers first (declaration order within each class), so an unbindable screen texture never takes a slot a real texture needs.
    std::vector<usize> order;
    for (usize i = 0; i < m.uniforms.size(); ++i) if (gdIsSampler(m.uniforms[i].type.base)) order.push_back(i);
    std::vector<int> slotOf(m.uniforms.size(), -1);
    const auto bindable = [&](const GdUniform& u, std::string& why) {
        if (u.type.base != GdBase::Sampler2D) { why = std::string(gdBaseName(u.type.base)) + " uniforms need a texture type other than a material slot"; return false; }
        if (u.scope == GdUniformScope::Global) { why = "global sampler uniforms are not carried"; return false; }
        if (u.texHint == GdTexHint::ScreenTexture) { why = "hint_screen_texture: the engine's scene copy is not bound to the material pass"; return false; }
        if (u.texHint == GdTexHint::DepthTexture) { why = "hint_depth_texture: the engine's depth copy is not bound to the material pass"; return false; }
        if (u.texHint == GdTexHint::NormalRoughnessTexture) { why = "hint_normal_roughness_texture: the engine has no normal-roughness copy for the material pass"; return false; }
        return true;
    };
    bool samplerTypeError = false;
    for (const usize i : order) {
        const GdUniform& u = m.uniforms[i];
        std::string why;
        if (u.type.base != GdBase::Sampler2D) {
            r.error = "sampler uniform '" + u.name + "' is a " + gdBaseName(u.type.base) + "; the engine's material takes sampler2D textures only";
            samplerTypeError = true;
            break;
        }
        if (!bindable(u, why)) continue;
        std::vector<int>& pool = u.sourceColor ? sRgbPool : linPool;
        if (pool.empty()) {
            r.error = std::string("the shader has more ") + (u.sourceColor ? "source_color" : "linear") + " textures than the material has " + (u.sourceColor ? "sRGB" : "linear") +
                      " texture slots (sRGB 3, linear 4; slot 7 holds the uniforms)";
            samplerTypeError = true;
            break;
        }
        slotOf[i] = pool.back();
        pool.pop_back();
    }
    if (samplerTypeError) return false;
    // Unbindable samplers take any remaining slot (it stays unbound, so the shader sees the hint's default colour).
    std::vector<int> spare;
    for (const int s : sRgbPool) spare.push_back(s);
    for (const int s : linPool) spare.push_back(s);
    for (const usize i : order) {
        const GdUniform& u = m.uniforms[i];
        std::string why;
        GdEngineSampler s;
        s.name = u.name;
        s.sourceColor = u.sourceColor;
        s.hint = u.texHint;
        s.filter = u.filter == GdFilter::Default ? opt.defaultFilter : u.filter;
        s.repeat = u.repeat == GdRepeat::Default ? opt.defaultRepeat : u.repeat;
        if (slotOf[i] >= 0) {
            s.slot = slotOf[i];
        } else {
            bindable(u, why);
            s.bindable = false;
            s.reason = why;
            if (spare.empty()) { r.error = "no material texture slot is left for sampler uniform '" + u.name + "'"; return false; }
            s.slot = spare.back();
            spare.pop_back();
            addNote(r, "approximated", "uniform " + u.name, why + "; the shader reads the hint's default colour");
        }
        r.samplers.push_back(s);
        samplerSlots.push_back(s.slot);
    }

    // ---- emit the two stages ----
    r.ns = namePrefixFor(r.name, source);
    GdHlslOptions ho;
    ho.entryPrefix = "main";
    ho.namePrefix = r.ns;
    ho.nameMacro = "GDN";
    ho.slotSamplers = true;
    ho.samplerSlots = samplerSlots;
    ho.defaultFilter = opt.defaultFilter;
    ho.defaultRepeat = opt.defaultRepeat;
    ho.varyingSemanticBase = 0;
    std::set<std::string> read, written;
    const auto emit = [&](GdProcessor p, std::string& outSource, bool& has) {
        ho.processorMask = gdStageBit(p);
        const GdHlslResult h = gdShaderEmitHlsl(m, ho);
        for (const GdShaderDiag& d : h.diags) r.diags.push_back(d);
        if (!h.ok) { if (r.error.empty()) r.error = h.error.empty() ? "the HLSL emitter refused the shader" : h.error; return false; }
        outSource = h.source;
        has = true;
        if (p == GdProcessor::Fragment) { r.uniforms = h.uniforms; r.instanceUniforms = h.instanceUniforms; r.globalUniforms = h.globalUniforms; }
        for (const GdHookUse& u : h.hooks) {
            if (u.read) read.insert(u.macro);
            if (u.written) written.insert(u.macro);
            r.hooks.push_back(u);
        }
        return true;
    };
    if (r.hasFragment && !emit(GdProcessor::Fragment, r.fragmentSource, r.hasFragment)) return false;
    if (r.hasVertex && !emit(GdProcessor::Vertex, r.vertexSource, r.hasVertex)) return false;
    if (!r.hasFragment && !r.hasVertex) { r.error = "the shader has neither a vertex() nor a fragment() function"; return false; }
    // The layout when the fragment function was not emitted (a vertex-only shader).
    if (!r.hasFragment) gdShaderLayoutUniforms(m, r.uniforms, r.instanceUniforms, r.globalUniforms);
    {
        GdHlslOptions v = ho;
        v.processorMask = gdStageBit(GdProcessor::Vertex) | gdStageBit(GdProcessor::Fragment);
        const GdHlslResult both = gdShaderEmitHlsl(m, v);   // the varyings are one list for both stages
        r.varyings = both.varyingSlots;
    }
    const int regs = r.uniforms.registerCount + r.instanceUniforms.registerCount + r.globalUniforms.registerCount;
    if (regs > kGdEngineMaxRegisters) {
        r.error = "the shader's uniforms need " + std::to_string(regs) + " registers; a material table holds " + std::to_string(kGdEngineMaxRegisters);
        return false;
    }

    // ---- the default table ----
    r.defaultRegisters.assign(static_cast<usize>(regs) * 4, 0.0f);
    const int instBase = r.uniforms.registerCount, globBase = instBase + r.instanceUniforms.registerCount;
    const auto packBlock = [&](const GdUniformBlock& blk, int base) {
        for (const GdUniformSlot& s : blk.slots) {
            const GdUniform& u = m.uniforms[static_cast<usize>(s.uniformIndex)];
            GdUniformSlot shifted = s;
            shifted.reg += base;
            GdEngineUniform eu;
            eu.name = u.name;
            eu.type = u.type;
            eu.scope = u.scope;
            eu.sourceColor = u.sourceColor;
            eu.slot = shifted;
            eu.def = u.def;
            r.params.push_back(std::move(eu));
            if (u.scope == GdUniformScope::Global) {
                const auto it = opt.globalDefaults.find(u.name);
                if (it != opt.globalDefaults.end()) gdPackUniform(shifted, u, it->second.v, r.defaultRegisters);
                continue;
            }
            if (u.def.valid) gdPackUniform(shifted, u, u.def.v, r.defaultRegisters);
        }
    };
    packBlock(r.uniforms, 0);
    packBlock(r.instanceUniforms, instBase);
    packBlock(r.globalUniforms, globBase);
    if (r.instanceUniforms.registerCount > 0) addNote(r, "approximated", "instance uniforms", "stored per material, not per instance: every instance of the material shares the value");

    // ---- render modes ----
    for (const std::string& rm : m.renderModes) {
        if (rm == "blend_mix") r.blend = 0;
        else if (rm == "blend_add") r.blend = 1;
        else if (rm == "blend_sub") r.blend = 2;
        else if (rm == "blend_mul") r.blend = 3;
        else if (rm == "blend_premul_alpha") { r.blend = 4; addNote(r, "approximated", "render_mode blend_premul_alpha", "drawn with the engine's premultiplied alpha blend"); }
        else if (rm == "depth_draw_opaque") r.depthDraw = 0;
        else if (rm == "depth_draw_always") r.depthDraw = 1;
        else if (rm == "depth_draw_never") r.depthDraw = 2;
        else if (rm == "depth_prepass_alpha") addNote(r, "approximated", "render_mode depth_prepass_alpha", "no alpha depth pre-pass: a masked material discards in the colour pass");
        else if (rm == "depth_test_disabled") r.depthTestDisabled = true;
        else if (rm == "cull_back") r.cull = 1;
        else if (rm == "cull_front") r.cull = 2;
        else if (rm == "cull_disabled") r.cull = 3;
        else if (rm == "unshaded") r.fxBits |= kFxUnshaded;
        else if (rm == "shadows_disabled") r.shadowsDisabled = true;
        else if (rm == "ambient_light_disabled") r.fxBits |= kFxAmbientDisabled;
        else if (rm == "specular_disabled") r.fxBits |= kFxSpecularDisabled;
        else if (rm == "diffuse_lambert_wrap") { r.fxBits |= kFxDiffuseWrap; addNote(r, "approximated", "render_mode diffuse_lambert_wrap", "the subsurface wrap term stands in"); }
        else if (rm == "diffuse_burley") {}
        else if (rm == "diffuse_lambert" || rm == "diffuse_toon") addNote(r, "approximated", "render_mode " + rm, "the engine's diffuse lobe is Burley");
        else if (rm == "specular_schlick_ggx") {}
        else if (rm == "specular_toon" || rm == "specular_blinn" || rm == "specular_phong") addNote(r, "approximated", "render_mode " + rm, "the engine's specular lobe is GGX");
        else if (rm == "world_vertex_coords") r.fxBits |= kFxWorldVertexCoords;
        else if (rm == "skip_vertex_transform") r.fxBits |= kFxSkipVertexTransform;
        else if (rm == "vertex_lighting") addNote(r, "approximated", "render_mode vertex_lighting", "lit per pixel");
        else if (rm == "fog_disabled") addNote(r, "unsupported", "render_mode fog_disabled", "the engine applies its fog after the material");
        else if (rm == "wireframe") addNote(r, "unsupported", "render_mode wireframe", "no wireframe fill mode for materials");
        else if (rm == "shadow_to_opacity") addNote(r, "unsupported", "render_mode shadow_to_opacity", "not supported");
        else if (rm == "alpha_to_coverage" || rm == "alpha_to_coverage_and_one") addNote(r, "approximated", "render_mode " + rm, "no alpha-to-coverage; a hash/discard stands in");
        else if (rm == "particle_trails" || rm == "ensure_correct_normals" || rm == "ambient_light_disabled_unused") addNote(r, "approximated", "render_mode " + rm, "ignored");
        else if (rm == "cull_disabled_unused") {}
        else addNote(r, "approximated", "render_mode " + rm, "no engine counterpart; ignored");
    }

    // ---- what the fragment writes ----
    r.writesAlpha = written.count("AVER_GD_ALPHA") != 0;
    r.writesAlphaScissor = written.count("AVER_GD_ALPHA_SCISSOR_THRESHOLD") != 0;
    if (written.count("AVER_GD_NORMAL") && r.hasFragment) r.fxBits |= kFxUsesNormalWrite;
    if (const GdFunction* f = m.processor(GdProcessor::Fragment)) r.usesDiscard = f->usesDiscard;
    // discard inside a custom function the fragment calls also counts.
    for (const GdFunction& f : m.functions) if (f.usesDiscard) r.usesDiscard = true;

    for (const HookRule& h : kHookRules) {
        const std::string macro = std::string("AVER_GD_") + h.name;
        const bool hit = h.onlyWritten ? written.count(macro) != 0 : read.count(macro) != 0;
        if (hit) addNote(r, h.severity, h.name, h.reason);
    }
    r.ok = true;
    return true;
}

}  // namespace aver::fmt::godot
