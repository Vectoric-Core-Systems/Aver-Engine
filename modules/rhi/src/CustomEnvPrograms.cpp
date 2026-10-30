// The HLSL of custom sky, fog and particles programs (CustomEnvPrograms.hpp).
#include "aver/rhi/CustomEnvPrograms.hpp"

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
    {"AVER_CUSTOM_LIBRARY", "AVER_CENV_LIBRARY"}, {"AVER_CUSTOM_NO_DERIVATIVES", "AVER_CENV_NO_DERIVATIVES"}, {"AVER_CUSTOM_SLOT_SAMPLERS", "AVER_CENV_SLOT_SAMPLERS"},
    {"embrSlot", "cenvrSlot"}, {"embSlot", "cenvSlot"}, {"embr_", "cenvr_"}, {"emb_", "cenv_"}, {"EMB_SMP", "CENV_SMP"},
};

bool renameId(const char* id, usize n, std::string& out) {
    for (const Rename& r : kRenames) {
        const usize len = std::strlen(r.from);
        if (n >= len && std::memcmp(id, r.from, len) == 0) { out = r.to; out.append(id + len, n - len); return true; }
    }
    // Library types: Emb followed by an upper-case letter or digit (EmbSampler2D, EmbS_foo).
    if (n > 3 && id[0] == 'E' && id[1] == 'm' && id[2] == 'b' && ((id[3] >= 'A' && id[3] <= 'Z') || (id[3] >= '0' && id[3] <= '9'))) {
        out = "CEnv";
        out.append(id + 3, n - 3);
        return true;
    }
    return false;
}

const std::string& library() { return shaderFile("material_embedded_lib.hlsl"); }

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

std::string customEnvRename(std::string text) {
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

std::string customEnvRegisterInit(const CustomEnvProgram& p, u32 first, u32 count) { return registerInit(p.values, first, count); }

std::string customSkyProgram(const CustomEnvProgram& p) {
    const std::string& common = shaderFile("custom_env_common.hlsl");
    const std::string& lib = library();
    const std::string& hooks = shaderFile("custom_sky.hlsl");
    std::string head, tail;
    if (common.empty() || lib.empty() || hooks.empty() || p.source.empty() || !split(hooks, "//@CUSTOM_SOURCE@", head, tail)) {
        AVER_ERROR("[CustomEnv] the sky shader files are missing or the sky program is empty");
        return {};
    }
    std::string s = "// ---- custom sky program '" + p.name + "' ----\n";
    s += common;
    s += "\n";
    s += customEnvRename(lib);
    s += "\n";
    s += head;
    s += "static const float4 gCustomSkyU[" + std::to_string(std::max(p.regs, 1u)) + "] = { " + registerInit(p.values, 0, p.regs) + " };\n";
    s += "static const float4 gCustomSkyG[" + std::to_string(std::max(p.globalRegs, 1u)) + "] = { " + registerInit(p.values, p.regs, p.globalRegs) + " };\n";
    s += "#define AVER_CUSTOM_UBUF gCustomSkyU\n#define AVER_CUSTOM_GBUF gCustomSkyG\n";
    s += customEnvRename(p.source);
    s += "\n#define AVER_CUSTOM_SKY_ENTRY " + p.ns + "main_sky\n";
    s += tail;
    s += "\n";
    return s;
}

std::string spliceCustomSky(std::string_view prelude, const std::string& program) {
    if (program.empty()) return std::string(prelude);
    const usize at = prelude.find(kCustomSkyMarker);
    if (at == std::string_view::npos) return std::string(prelude);
    std::string s(prelude.substr(0, at));
    s += program;
    s += prelude.substr(at + std::strlen(kCustomSkyMarker));
    return s;
}

std::string customFogProgram(const std::vector<const CustomEnvProgram*>& shaders) {
    const std::string& common = shaderFile("custom_env_common.hlsl");
    const std::string& lib = library();
    const std::string& hooks = shaderFile("custom_fog.hlsl");
    std::string head, rest, mid, tail;
    if (shaders.empty() || common.empty() || lib.empty() || hooks.empty() || !split(hooks, "//@CUSTOM_SOURCE@", head, rest) || !split(rest, "//@CUSTOM_CASES@", mid, tail)) {
        AVER_ERROR("[CustomEnv] the fog shader files are missing or no fog shader was given");
        return {};
    }
    const u32 count = static_cast<u32>(std::min<usize>(shaders.size(), kCustomFogShaderMax));
    std::string s = "// ---- custom fog programs ----\n#define AVER_CUSTOM_FOG_COUNT " + std::to_string(count) + "\n#define AVER_CUSTOM_FOG_REG_COUNT " +
                    std::to_string(count * kCustomFogShaderRegs) + "\n";
    s += common;
    s += "\n";
    s += customEnvRename(lib);
    s += "\n";
    s += head;
    for (u32 k = 0; k < count; ++k) {
        const CustomEnvProgram& p = *shaders[k];
        const std::string id = std::to_string(k + 1);
        s += "static float4 gCustomFogU" + id + "[" + std::to_string(std::max(p.regs, 1u)) + "];\n";
        s += "static float4 gCustomFogG" + id + "[" + std::to_string(std::max(p.globalRegs, 1u)) + "];\n";
        s += "#define AVER_CUSTOM_UBUF gCustomFogU" + id + "\n#define AVER_CUSTOM_GBUF gCustomFogG" + id + "\n";
        s += customEnvRename(p.source);
        s += "\n#undef AVER_CUSTOM_UBUF\n#undef AVER_CUSTOM_GBUF\n";
    }
    s += mid;
    s += "    switch (idx) {\n";
    for (u32 k = 0; k < count; ++k) {
        const CustomEnvProgram& p = *shaders[k];
        const std::string id = std::to_string(k + 1), base = std::to_string(k * kCustomFogShaderRegs);
        s += "    case " + id + "u:\n";
        for (u32 i = 0; i < p.regs; ++i) s += "        gCustomFogU" + id + "[" + std::to_string(i) + "] = gCustomFogRegs[" + base + " + " + std::to_string(i) + "];\n";
        for (u32 i = 0; i < p.globalRegs; ++i) s += "        gCustomFogG" + id + "[" + std::to_string(i) + "] = gCustomFogRegs[" + base + " + " + std::to_string(p.regs + i) + "];\n";
        s += "        " + p.ns + "main_fog();\n        break;\n";
    }
    s += "    default: break;\n    }\n";
    s += tail;
    s += "\n";
    return s;
}

std::string customParticlesProgram(const CustomEnvProgram& p) {
    const std::string& common = shaderFile("custom_env_common.hlsl");
    const std::string& lib = library();
    const std::string& hooks = shaderFile("custom_particles.hlsl");
    std::string head, tail;
    if (common.empty() || lib.empty() || hooks.empty() || p.source.empty() || !split(hooks, "//@CUSTOM_SOURCE@", head, tail)) {
        AVER_ERROR("[CustomEnv] the particles shader files are missing or the program is empty");
        return {};
    }
    std::string s = "// ---- custom particles program '" + p.name + "' ----\n";
    s += std::string("#define AVER_CUSTOM_HAS_START ") + (p.hasStart ? "1" : "0") + "\n#define AVER_CUSTOM_HAS_PROCESS " + (p.hasProcess ? "1" : "0") + "\n";
    s += "#define AVER_CUSTOM_PART_REGS " + std::to_string(p.regs) + "\n";
    s += common;
    s += "\n";
    s += customEnvRename(lib);
    s += "\n";
    s += head;
    s += customEnvRename(p.source);
    s += "\n";
    if (p.hasStart) s += "#define AVER_CUSTOM_START_ENTRY " + p.ns + "main_start\n";
    if (p.hasProcess) s += "#define AVER_CUSTOM_PROCESS_ENTRY " + p.ns + "main_process\n";
    s += tail;
    s += "\n";
    return s;
}

}  // namespace aver::rhi
