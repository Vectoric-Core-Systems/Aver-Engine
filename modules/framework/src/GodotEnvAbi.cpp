// Converted Godot sky, fog and particles shader store behind framework_godotenv_abi.h. File-static, single game thread; the hosts read it every frame.

#include "aver/framework/framework_godotenv_abi.h"

#include "GodotEnvInternal.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphGodot.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Program {
    int32_t kind = 0;
    std::string name;
    std::string graph;                  // the registered .ocgraph text, handed back to the host
    std::vector<float> values;          // the uniform table, four floats a register
    std::vector<float> defaults;
};

struct Store {
    std::vector<Program> programs;
    std::string sky;                    // active sky shader name, empty = the engine's own
    int64_t rev = 0, regRev = 0;
};

Store& store() {
    static Store s;
    return s;
}

bool validKind(int32_t k) { return k >= AVER_FW_GENV_SKY && k <= AVER_FW_GENV_PARTICLES; }

Program* find(int32_t kind, const std::string& name) {
    for (Program& p : store().programs) if (p.kind == kind && p.name == name) return &p;
    return nullptr;
}

// The index-th program of a kind, in registration order.
Program* nth(int32_t kind, int32_t index) {
    if (!validKind(kind) || index < 0) return nullptr;
    int32_t i = 0;
    for (Program& p : store().programs) {
        if (p.kind != kind) continue;
        if (i++ == index) return &p;
    }
    return nullptr;
}

int32_t copyOut(const std::string& s, char* out, int32_t cap) {
    if (out && cap > 0) {
        const size_t n = std::min(s.size(), static_cast<size_t>(cap - 1));
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
    }
    return static_cast<int32_t>(s.size());
}

int32_t kindOf(const std::string& s) { return s == "sky" ? AVER_FW_GENV_SKY : s == "fog" ? AVER_FW_GENV_FOG : AVER_FW_GENV_PARTICLES; }

}  // namespace

uint32_t aver_fw_genv_fog_index(const std::string& name) {
    uint32_t i = 0;
    for (const Program& p : store().programs) {
        if (p.kind != AVER_FW_GENV_FOG) continue;
        ++i;
        if (p.name == name) return i <= 4 ? i : 0;
    }
    return 0;
}

extern "C" {

int32_t aver_fw_genv_abi_version(void) { return AVER_FW_GENV_ABI_VERSION; }

int32_t aver_fw_genv_register(int32_t kind, const char* name, const char* ocgraphText) {
    if (!validKind(kind) || !name || !*name || !ocgraphText) return 0;
    aver::fmt::OcGraphData g;
    std::string err;
    if (!aver::fmt::parseOcgraph(ocgraphText, g, &err)) {
        AVER_WARN("[GodotEnv] shader '{}' refused: the graph does not parse ({})", name, err);
        return 0;
    }
    aver::fmt::GodotEnvPart part;
    if (!aver::fmt::unpackGodotEnvShader(g, part, &err)) {
        AVER_WARN("[GodotEnv] shader '{}' refused: {}", name, err);
        return 0;
    }
    if (kindOf(part.kind) != kind) {
        AVER_WARN("[GodotEnv] shader '{}' refused: it is a {} shader, not the kind asked for", name, part.kind);
        return 0;
    }
    Store& s = store();
    Program* p = find(kind, name);
    if (!p) {
        int32_t same = 0;
        for (const Program& q : s.programs) if (q.kind == kind) ++same;
        if (same >= AVER_FW_GENV_MAX) { AVER_WARN("[GodotEnv] shader '{}' refused: {} {} shaders are already registered", name, same, part.kind); return 0; }
        s.programs.emplace_back();
        p = &s.programs.back();
        p->kind = kind;
        p->name = name;
    }
    p->graph = ocgraphText;
    p->defaults = part.defaults;
    p->values = part.defaults;
    ++s.rev;
    ++s.regRev;
    return 1;
}

int32_t aver_fw_genv_set_registers(int32_t kind, const char* name, const float* regs, int32_t floatCount) {
    if (!validKind(kind) || !name || !regs || floatCount < 0) return 0;
    Program* p = find(kind, name);
    if (!p) return 0;
    const size_t n = std::min(static_cast<size_t>(floatCount), p->values.size());
    bool changed = false;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(regs[i]) && !std::isnan(regs[i])) continue;
        // Bit patterns, not values: int / bool uniforms ride the float slots as raw words, so compare the bits.
        uint32_t a, b;
        std::memcpy(&a, &regs[i], 4);
        std::memcpy(&b, &p->values[i], 4);
        if (a != b) { p->values[i] = regs[i]; changed = true; }
    }
    if (changed) ++store().regRev;
    return 1;
}

int32_t aver_fw_genv_use_sky(const char* name) {
    Store& s = store();
    const std::string want = name ? name : "";
    if (!want.empty() && !find(AVER_FW_GENV_SKY, want)) return 0;
    if (s.sky != want) { s.sky = want; ++s.rev; }
    return 1;
}

int32_t aver_fw_genv_unregister(int32_t kind, const char* name) {
    if (!validKind(kind) || !name) return 0;
    Store& s = store();
    for (size_t i = 0; i < s.programs.size(); ++i) {
        if (s.programs[i].kind != kind || s.programs[i].name != name) continue;
        s.programs.erase(s.programs.begin() + static_cast<std::ptrdiff_t>(i));
        if (kind == AVER_FW_GENV_SKY && s.sky == name) s.sky.clear();
        ++s.rev;
        return 1;
    }
    return 0;
}

int64_t aver_fw_genv_revision(void) { return store().rev; }
int64_t aver_fw_genv_registers_revision(void) { return store().regRev; }

int32_t aver_fw_genv_count(int32_t kind) {
    if (!validKind(kind)) return 0;
    int32_t n = 0;
    for (const Program& p : store().programs) if (p.kind == kind) ++n;
    return n;
}

int32_t aver_fw_genv_name(int32_t kind, int32_t index, char* out, int32_t cap) {
    const Program* p = nth(kind, index);
    return p ? copyOut(p->name, out, cap) : -1;
}

int32_t aver_fw_genv_graph(int32_t kind, int32_t index, char* out, int32_t cap) {
    const Program* p = nth(kind, index);
    return p ? copyOut(p->graph, out, cap) : -1;
}

int32_t aver_fw_genv_registers(int32_t kind, int32_t index, float* out, int32_t capFloats) {
    const Program* p = nth(kind, index);
    if (!p) return -1;
    if (out && capFloats > 0) std::memcpy(out, p->values.data(), std::min(static_cast<size_t>(capFloats), p->values.size()) * sizeof(float));
    return static_cast<int32_t>(p->values.size());
}

int32_t aver_fw_genv_sky_index(void) {
    const Store& s = store();
    if (s.sky.empty()) return -1;
    int32_t i = 0;
    for (const Program& p : s.programs) {
        if (p.kind != AVER_FW_GENV_SKY) continue;
        if (p.name == s.sky) return i;
        ++i;
    }
    return -1;
}

void aver_fw_genv_reset(void) {
    Store& s = store();
    if (s.programs.empty() && s.sky.empty()) return;
    s.programs.clear();
    s.sky.clear();
    ++s.rev;
    ++s.regRev;
}

}  // extern "C"
