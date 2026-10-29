// A Godot sky, fog or particles shader prepared for the engine (GodotShaderEnv.hpp).
#include "aver/formats/godot/GodotShaderEnv.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace aver::fmt::godot {
namespace {

std::string prefixFor(std::string_view name, std::string_view source) {
    u64 h = 14695981039346656037ull;
    for (const char c : name) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    for (const char c : source) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char buf[32];
    std::snprintf(buf, sizeof buf, "ge%08x_", static_cast<unsigned>(h ^ (h >> 32)));
    return buf;
}

void addNote(GdEnvShader& r, const char* severity, const std::string& subject, const std::string& reason) {
    for (const GdEngineNote& n : r.notes) if (n.subject == subject && n.reason == reason) return;
    r.notes.push_back(GdEngineNote{severity, subject, reason});
}

struct HookRule { GdEnvKind kind; const char* name; const char* severity; const char* reason; bool onlyWritten; };

// What the engine cannot honour exactly. onlyWritten: fires when the shader writes the built-in; otherwise when it reads it.
constexpr HookRule kRules[] = {
    {GdEnvKind::Sky, "SCREEN_UV", "approximated", "the background pass supplies the screen position; reflections and ambient light read (0.5, 0.5)", false},
    {GdEnvKind::Sky, "SKY_COORDS", "approximated", "the background pass supplies the screen position; reflections and ambient light read (0.5, 0.5)", false},
    {GdEnvKind::Sky, "HALF_RES_COLOR", "approximated", "there are no half or quarter resolution passes: reads zero", false},
    {GdEnvKind::Sky, "QUARTER_RES_COLOR", "approximated", "there are no half or quarter resolution passes: reads zero", false},
    {GdEnvKind::Sky, "AT_HALF_RES_PASS", "approximated", "always false: the sky is evaluated at full resolution", false},
    {GdEnvKind::Sky, "AT_QUARTER_RES_PASS", "approximated", "always false: the sky is evaluated at full resolution", false},
    {GdEnvKind::Sky, "AT_CUBEMAP_PASS", "approximated", "true for reflections and ambient light, false for the background", false},
    {GdEnvKind::Sky, "LIGHT1_ENABLED", "approximated", "only the sun is a directional light: LIGHT1..3 read as disabled", false},
    {GdEnvKind::Sky, "LIGHT1_DIRECTION", "approximated", "only the sun is a directional light: LIGHT1..3 read as disabled", false},
    {GdEnvKind::Sky, "LIGHT1_COLOR", "approximated", "only the sun is a directional light: LIGHT1..3 read as disabled", false},
    {GdEnvKind::Sky, "LIGHT1_ENERGY", "approximated", "only the sun is a directional light: LIGHT1..3 read as disabled", false},
    {GdEnvKind::Sky, "LIGHT2_ENABLED", "approximated", "only the sun is a directional light: LIGHT1..3 read as disabled", false},
    {GdEnvKind::Sky, "LIGHT3_ENABLED", "approximated", "only the sun is a directional light: LIGHT1..3 read as disabled", false},
    {GdEnvKind::Sky, "LIGHT0_ENERGY", "approximated", "the sun's intensity divided by 104719.755 lux (Godot energy 1)", false},
    {GdEnvKind::Sky, "ALPHA", "approximated", "written, not consumed: the sky is opaque", true},
    {GdEnvKind::Sky, "FOG", "approximated", "blends over the sky colour (COLOR = mix(COLOR, FOG.rgb, FOG.a)); the scene's own fog is applied by the engine", true},
    {GdEnvKind::Fog, "SDF", "approximated", "reads a large positive distance: Aver has no scene signed distance field", false},
    {GdEnvKind::Fog, "SIZE", "approximated", "the volume's size property, in metres (the node's scale is not included)", false},
    {GdEnvKind::Particles, "EMISSION_TRANSFORM", "approximated", "reads the identity in process(); start() reads the emitter's transform", false},
    {GdEnvKind::Particles, "EMITTER_VELOCITY", "approximated", "reads zero: the emitter's own velocity is not tracked", false},
    {GdEnvKind::Particles, "INTERPOLATE_TO_END", "approximated", "reads zero (the emitter's interp_to_end is not carried)", false},
    {GdEnvKind::Particles, "AMOUNT_RATIO", "approximated", "reads one (amount_ratio is not carried)", false},
    {GdEnvKind::Particles, "USERDATA1", "approximated", "reads zero: emit_subparticle() and its user data are not supported", false},
    {GdEnvKind::Particles, "USERDATA2", "approximated", "reads zero: emit_subparticle() and its user data are not supported", false},
    {GdEnvKind::Particles, "USERDATA3", "approximated", "reads zero: emit_subparticle() and its user data are not supported", false},
    {GdEnvKind::Particles, "USERDATA4", "approximated", "reads zero: emit_subparticle() and its user data are not supported", false},
    {GdEnvKind::Particles, "USERDATA5", "approximated", "reads zero: emit_subparticle() and its user data are not supported", false},
    {GdEnvKind::Particles, "USERDATA6", "approximated", "reads zero: emit_subparticle() and its user data are not supported", false},
    {GdEnvKind::Particles, "COLLIDED", "approximated", "reads false: particle collision and attractors are Godot nodes the engine does not feed the shader from", false},
    {GdEnvKind::Particles, "COLLISION_NORMAL", "approximated", "reads zero: particle collision is not fed to the shader", false},
    {GdEnvKind::Particles, "COLLISION_DEPTH", "approximated", "reads zero: particle collision is not fed to the shader", false},
    {GdEnvKind::Particles, "ATTRACTOR_FORCE", "approximated", "reads zero: particle attractors are not fed to the shader", false},
    {GdEnvKind::Particles, "FLAG_EMIT_POSITION", "approximated", "reads true", false},
    {GdEnvKind::Particles, "FLAG_EMIT_ROT_SCALE", "approximated", "reads true", false},
    {GdEnvKind::Particles, "FLAG_EMIT_VELOCITY", "approximated", "reads true", false},
    {GdEnvKind::Particles, "FLAG_EMIT_COLOR", "approximated", "reads true", false},
    {GdEnvKind::Particles, "FLAG_EMIT_CUSTOM", "approximated", "reads true", false},
    {GdEnvKind::Particles, "TRANSFORM", "approximated", "position and velocity are exact; the basis reaches the draw as a uniform scale and a spin about the view axis", true},
    {GdEnvKind::Particles, "COLOR", "approximated", "kept in full precision by the simulation; the draw uses 8 bits per channel clamped to 0..1", true},
};

}  // namespace

const char* gdEnvKindName(GdEnvKind k) {
    switch (k) {
        case GdEnvKind::Sky: return "sky";
        case GdEnvKind::Fog: return "fog";
        case GdEnvKind::Particles: return "particles";
    }
    return "sky";
}

bool gdShaderForEnv(std::string_view source, std::string_view fileName, const GdEnvOptions& opt, GdEnvShader& r) {
    r = GdEnvShader{};
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
    switch (m.shaderType) {
        case GdShaderType::Sky: r.kind = GdEnvKind::Sky; break;
        case GdShaderType::Fog: r.kind = GdEnvKind::Fog; break;
        case GdShaderType::Particles: r.kind = GdEnvKind::Particles; break;
        default:
            r.error = std::string("shader_type ") + gdShaderTypeName(m.shaderType) + " is not a sky, fog or particles shader (spatial shaders are G6 package SH2)";
            return false;
    }
    const GdProcessor entrySky = GdProcessor::Sky, entryFog = GdProcessor::Fog;
    u16 mask = 0;
    if (r.kind == GdEnvKind::Sky) {
        if (!m.processor(entrySky)) { r.error = "the sky shader has no sky() function"; return false; }
        mask = gdStageBit(entrySky);
    } else if (r.kind == GdEnvKind::Fog) {
        if (!m.processor(entryFog)) { r.error = "the fog shader has no fog() function"; return false; }
        mask = gdStageBit(entryFog);
    } else {
        r.hasStart = m.processor(GdProcessor::Start) != nullptr;
        r.hasProcess = m.processor(GdProcessor::Process) != nullptr;
        if (!r.hasStart && !r.hasProcess) { r.error = "the particles shader has neither start() nor process()"; return false; }
        mask = static_cast<u16>((r.hasStart ? gdStageBit(GdProcessor::Start) : 0) | (r.hasProcess ? gdStageBit(GdProcessor::Process) : 0));
    }
    for (const GdUniform& u : m.uniforms) {
        if (u.scope == GdUniformScope::Instance) { r.error = "instance uniform '" + u.name + "': instance uniforms exist for spatial shaders only"; return false; }
        if (gdIsSampler(u.type.base) && u.type.base != GdBase::Sampler2D) {
            r.error = "sampler uniform '" + u.name + "' is a " + gdBaseName(u.type.base) + "; sky, fog and particle shaders take sampler2D uniforms only";
            return false;
        }
    }

    // ---- samplers: all unbound ----
    std::vector<int> slots;
    for (const GdUniform& u : m.uniforms) {
        if (!gdIsSampler(u.type.base)) continue;
        GdEngineSampler s;
        s.name = u.name;
        s.slot = static_cast<int>(slots.size());
        s.sourceColor = u.sourceColor;
        s.hint = u.texHint;
        s.filter = u.filter;
        s.repeat = u.repeat;
        s.bindable = false;
        s.reason = std::string("a ") + gdEnvKindName(r.kind) + " shader has no texture slots";
        r.samplers.push_back(s);
        slots.push_back(s.slot);
        addNote(r, "approximated", "uniform " + u.name, s.reason + "; the shader reads the hint's default colour (white unless hint_default_black / transparent / normal)");
    }

    // ---- emit ----
    r.ns = prefixFor(r.name, source);
    GdHlslOptions ho;
    ho.entryPrefix = r.ns + "main";   // without a name macro the entry names do not take namePrefix
    ho.namePrefix = r.ns;
    ho.slotSamplers = true;
    ho.samplerSlots = slots;
    ho.processorMask = mask;
    const GdHlslResult h = gdShaderEmitHlsl(m, ho);
    for (const GdShaderDiag& d : h.diags) r.diags.push_back(d);
    if (!h.ok) { r.error = h.error.empty() ? "the HLSL emitter refused the shader" : h.error; return false; }
    r.source = h.source;
    r.uniforms = h.uniforms;
    r.globalUniforms = h.globalUniforms;
    r.hooks = h.hooks;
    r.registerCount = h.uniforms.registerCount;
    r.globalRegisterCount = h.globalUniforms.registerCount;
    if (r.registerCount + r.globalRegisterCount > kGdEnvMaxRegisters) {
        r.error = "the shader's uniforms need " + std::to_string(r.registerCount + r.globalRegisterCount) + " registers; a " + gdEnvKindName(r.kind) +
                  " shader holds " + std::to_string(kGdEnvMaxRegisters);
        return false;
    }

    std::set<std::string> read, written;
    for (const GdHookUse& u : h.hooks) {
        if (u.read) read.insert(u.macro);
        if (u.written) written.insert(u.macro);
        if (u.macro == "AVER_GD_TIME") r.usesTime = true;
    }
    if (r.kind == GdEnvKind::Sky && (read.count("AVER_GD_RADIANCE") || written.count("AVER_GD_RADIANCE"))) {
        r.error = "the sky shader samples RADIANCE (the radiance cubemap); the engine renders the sky shader directly and has no cubemap to hand it";
        return false;
    }

    // ---- the default registers: normal block, then global block ----
    const int total = r.registerCount + r.globalRegisterCount;
    r.defaultRegisters.assign(static_cast<usize>(std::max(total, 0)) * 4, 0.0f);
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
    packBlock(r.globalUniforms, r.registerCount);

    // ---- what the engine does not honour ----
    for (const std::string& rm : m.renderModes) addNote(r, "approximated", "render_mode " + rm, "no engine counterpart; ignored");
    for (const HookRule& h2 : kRules) {
        if (h2.kind != r.kind) continue;
        const std::string macro = std::string("AVER_GD_") + h2.name;
        if (h2.onlyWritten ? written.count(macro) != 0 : read.count(macro) != 0) addNote(r, h2.severity, h2.name, h2.reason);
    }
    r.ok = true;
    return true;
}

}  // namespace aver::fmt::godot
