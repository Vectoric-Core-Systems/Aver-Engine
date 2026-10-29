// The HLSL of converted Godot sky, fog and particles shaders (GodotEnvPrograms.hpp).
#include "aver/rhi/GodotEnvPrograms.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aver::rhi {
namespace {

bool isIdStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdChar(char c) { return isIdStart(c) || (c >= '0' && c <= '9'); }

struct Rename { const char* from; const char* to; };
// Longest prefixes first. The macros that steer the library (guard, derivative and sampler modes) get the private spellings the common file defines.
constexpr Rename kRenames[] = {
    {"AVER_GD_LIBRARY", "AVER_GDE_LIBRARY"}, {"AVER_GD_NO_DERIVATIVES", "AVER_GDE_NO_DERIVATIVES"}, {"AVER_GD_SLOT_SAMPLERS", "AVER_GDE_SLOT_SAMPLERS"},
    {"gdrSlot", "gderSlot"}, {"gdSlot", "gdeSlot"}, {"gdr_", "gder_"}, {"gd_", "gde_"}, {"GD_SMP", "GDE_SMP"},
};

bool renameId(const char* id, usize n, std::string& out) {
    for (const Rename& r : kRenames) {
        const usize len = std::strlen(r.from);
        if (n >= len && std::memcmp(id, r.from, len) == 0) { out = r.to; out.append(id + len, n - len); return true; }
    }
    // Library types: Gd followed by an upper-case letter or digit (GdSampler2D, GdS_foo).
    if (n > 2 && id[0] == 'G' && id[1] == 'd' && ((id[2] >= 'A' && id[2] <= 'Z') || (id[2] >= '0' && id[2] <= '9'))) {
        out = "GdE";
        out.append(id + 2, n - 2);
        return true;
    }
    return false;
}

const std::string& library() { return shaderFile("material_godot_lib.hlsl"); }

std::string registerInit(const std::vector<float>& values, u32 first, u32 count) {
    std::string s;
    const u32 n = std::max(count, 1u);
    for (u32 r = 0; r < n; ++r) {
        s += r ? ", float4(" : "float4(";
        for (u32 c = 0; c < 4; ++c) {
            const usize at = (static_cast<usize>(first) + r) * 4 + c;
            u32 bits = 0;
            if (r < count && at < values.size()) std::memcpy(&bits, &values[at], sizeof bits);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%sasfloat(0x%08Xu)", c ? ", " : "", static_cast<unsigned>(bits));
            s += buf;
        }
        s += ")";
    }
    return s;
}

// Splits a hook file at marker lines. Returns false when a marker is missing.
bool split(const std::string& text, const char* marker, std::string& before, std::string& after) {
    const usize at = text.find(marker);
    if (at == std::string::npos) return false;
    before = text.substr(0, at);
    const usize eol = text.find('\n', at);
    after = eol == std::string::npos ? std::string() : text.substr(eol + 1);
    return true;
}

}  // namespace

std::string godotEnvRename(std::string text) {
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    const usize n = text.size();
    for (usize i = 0; i < n;) {
        const char c = text[i];
        if (!isIdStart(c)) { out += c; ++i; continue; }
        usize j = i + 1;
        while (j < n && isIdChar(text[j])) ++j;
        std::string repl;
        if (renameId(text.data() + i, j - i, repl)) out += repl;
        else out.append(text, i, j - i);
        i = j;
    }
    return out;
}

std::string godotEnvRegisterInit(const GodotEnvProgram& p, u32 first, u32 count) { return registerInit(p.values, first, count); }

std::string godotSkyProgram(const GodotEnvProgram& p) {
    const std::string& common = shaderFile("godot_env_common.hlsl");
    const std::string& lib = library();
    const std::string& hooks = shaderFile("godot_sky.hlsl");
    std::string head, tail;
    if (common.empty() || lib.empty() || hooks.empty() || p.source.empty() || !split(hooks, "//@GD_SOURCE@", head, tail)) {
        AVER_ERROR("[GodotEnv] the sky shader files are missing or the sky program is empty");
        return {};
    }
    std::string s = "// ---- Fabric G6 SH3: Godot sky shader '" + p.name + "' ----\n";
    s += common;
    s += "\n";
    s += godotEnvRename(lib);
    s += "\n";
    s += head;
    s += "static const float4 gGdSkyU[" + std::to_string(std::max(p.regs, 1u)) + "] = { " + registerInit(p.values, 0, p.regs) + " };\n";
    s += "static const float4 gGdSkyG[" + std::to_string(std::max(p.globalRegs, 1u)) + "] = { " + registerInit(p.values, p.regs, p.globalRegs) + " };\n";
    s += "#define AVER_GD_UBUF gGdSkyU\n#define AVER_GD_GBUF gGdSkyG\n";
    s += godotEnvRename(p.source);
    s += "\n#define AVER_GD_SKY_ENTRY " + p.ns + "main_sky\n";
    s += tail;
    s += "\n";
    return s;
}

std::string spliceGodotSky(std::string_view prelude, const std::string& program) {
    if (program.empty()) return std::string(prelude);
    const usize at = prelude.find(kGodotSkyMarker);
    if (at == std::string_view::npos) return std::string(prelude);
    std::string s(prelude.substr(0, at));
    s += program;
    s += prelude.substr(at + std::strlen(kGodotSkyMarker));
    return s;
}

std::string godotFogProgram(const std::vector<const GodotEnvProgram*>& shaders) {
    const std::string& common = shaderFile("godot_env_common.hlsl");
    const std::string& lib = library();
    const std::string& hooks = shaderFile("godot_fog.hlsl");
    std::string head, rest, mid, tail;
    if (shaders.empty() || common.empty() || lib.empty() || hooks.empty() || !split(hooks, "//@GD_SOURCE@", head, rest) || !split(rest, "//@GD_CASES@", mid, tail)) {
        AVER_ERROR("[GodotEnv] the fog shader files are missing or no fog shader was given");
        return {};
    }
    const u32 count = static_cast<u32>(std::min<usize>(shaders.size(), kGodotFogShaderMax));
    std::string s = "// ---- Fabric G6 SH3: Godot fog shaders ----\n#define AVER_GD_FOG_COUNT " + std::to_string(count) + "\n#define AVER_GD_FOG_REG_COUNT " +
                    std::to_string(count * kGodotFogShaderRegs) + "\n";
    s += common;
    s += "\n";
    s += godotEnvRename(lib);
    s += "\n";
    s += head;
    for (u32 k = 0; k < count; ++k) {
        const GodotEnvProgram& p = *shaders[k];
        const std::string id = std::to_string(k + 1);
        s += "static float4 gGdFogU" + id + "[" + std::to_string(std::max(p.regs, 1u)) + "];\n";
        s += "static float4 gGdFogG" + id + "[" + std::to_string(std::max(p.globalRegs, 1u)) + "];\n";
        s += "#define AVER_GD_UBUF gGdFogU" + id + "\n#define AVER_GD_GBUF gGdFogG" + id + "\n";
        s += godotEnvRename(p.source);
        s += "\n#undef AVER_GD_UBUF\n#undef AVER_GD_GBUF\n";
    }
    s += mid;
    s += "    switch (idx) {\n";
    for (u32 k = 0; k < count; ++k) {
        const GodotEnvProgram& p = *shaders[k];
        const std::string id = std::to_string(k + 1), base = std::to_string(k * kGodotFogShaderRegs);
        s += "    case " + id + "u:\n";
        for (u32 i = 0; i < p.regs; ++i) s += "        gGdFogU" + id + "[" + std::to_string(i) + "] = gGdFogRegs[" + base + " + " + std::to_string(i) + "];\n";
        for (u32 i = 0; i < p.globalRegs; ++i) s += "        gGdFogG" + id + "[" + std::to_string(i) + "] = gGdFogRegs[" + base + " + " + std::to_string(p.regs + i) + "];\n";
        s += "        " + p.ns + "main_fog();\n        break;\n";
    }
    s += "    default: break;\n    }\n";
    s += tail;
    s += "\n";
    return s;
}

std::string godotParticlesProgram(const GodotEnvProgram& p) {
    const std::string& common = shaderFile("godot_env_common.hlsl");
    const std::string& lib = library();
    const std::string& hooks = shaderFile("godot_particles.hlsl");
    std::string head, tail;
    if (common.empty() || lib.empty() || hooks.empty() || p.source.empty() || !split(hooks, "//@GD_SOURCE@", head, tail)) {
        AVER_ERROR("[GodotEnv] the particles shader files are missing or the program is empty");
        return {};
    }
    std::string s = "// ---- Fabric G6 SH3: Godot particles shader '" + p.name + "' ----\n";
    s += std::string("#define AVER_GD_HAS_START ") + (p.hasStart ? "1" : "0") + "\n#define AVER_GD_HAS_PROCESS " + (p.hasProcess ? "1" : "0") + "\n";
    s += "#define AVER_GD_PART_REGS " + std::to_string(p.regs) + "\n";
    s += common;
    s += "\n";
    s += godotEnvRename(lib);
    s += "\n";
    s += head;
    s += godotEnvRename(p.source);
    s += "\n";
    if (p.hasStart) s += "#define AVER_GD_START_ENTRY " + p.ns + "main_start\n";
    if (p.hasProcess) s += "#define AVER_GD_PROCESS_ENTRY " + p.ns + "main_process\n";
    s += tail;
    s += "\n";
    return s;
}

}  // namespace aver::rhi
