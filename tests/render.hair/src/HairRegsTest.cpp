// HairRegsTest -- the register assignments of the hair compute kernels against the Jolt headers they were generated from. HEADLESS: it reads files.
//
// WHY IT EXISTS (docs/plans/wave11/SOFTBODY_HAIR.md 2.11). Jolt's fifteen hair kernels are compiled UNCHANGED, with explicit registers added by a macro
// override (AverHairMacros.hlsli) and a generated header per kernel (scripts/gen-hair-shader-regs.py). A Jolt update that moves a binding, adds one,
// or adds a macro to ShaderCore.h would otherwise mis-bind buffers on the GPU and show up as hair that explodes on one vendor. This test re-derives
// every table from the vendored headers and fails on any difference:
//   - the C++ table the host builds pipeline layouts from (src/HairRegs.inc) against <Name>Bindings.h
//   - each generated .regs.hlsli against the same
//   - each wrapper against the kernel it must include
//   - AverHairMacros.hlsli against the macros and type aliases of Jolt's ShaderCore.h
//   - the small struct header of the wind kernel against Jolt's JPH_HairVelocity
// Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/render/HairRegsTypes.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef AVER_JOLT_SHADER_DIR
#error "AVER_JOLT_SHADER_DIR must name modules/physics.jolt/Jolt/Shaders"
#endif
#ifndef AVER_HAIR_SHADER_DIR
#error "AVER_HAIR_SHADER_DIR must name modules/render.hair/shaders"
#endif
#ifndef AVER_PHYS_HAIR_SHADER_DIR
#error "AVER_PHYS_HAIR_SHADER_DIR must name modules/physics/shaders/hair"
#endif

namespace hairregs {
using aver::render::HairKernel;
using aver::render::HairKernelBinding;
#include "HairRegs.inc"
} // namespace hairregs

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    return true;
}

static std::string dirJoin(const char* dir, const std::string& name) { return std::string(dir) + "/" + name; }

struct Binding { std::string name; bool rw; unsigned slot; };

// The same parse the generator does: the bindings between BIND_BEGIN and BIND_END in declaration order, three overridable aliases included.
static std::vector<Binding> parseBindings(const std::string& text) {
    std::vector<Binding> out;
    unsigned t = 0, u = 0;
    bool inside = false;
    std::istringstream in(text);
    std::string line;
    const std::regex buf(R"(JPH_SHADER_BIND_(RW_)?BUFFER\(\s*(\w+)\s*,\s*(\w+)\s*\))");
    const std::regex alias(R"((JPH_SHADER_BIND_\w+)\(\s*(\w+)\s*\))");
    while (std::getline(in, line)) {
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const std::string s = line.substr(a);
        if (s[0] == '#' || s.compare(0, 2, "//") == 0) continue;
        if (s.find("JPH_SHADER_BIND_BEGIN") != std::string::npos) { inside = true; continue; }
        if (s.find("JPH_SHADER_BIND_END") != std::string::npos) { inside = false; continue; }
        if (!inside) continue;
        std::smatch m;
        if (std::regex_search(s, m, buf)) {
            const bool rw = m[1].matched;
            out.push_back({m[3].str(), rw, rw ? u++ : t++});
        } else if (std::regex_search(s, m, alias)) {
            const std::string a2 = m[1].str();
            if (a2 == "JPH_SHADER_BIND_SCALP_VERTICES" || a2 == "JPH_SHADER_BIND_SCALP_TRIANGLES") out.push_back({m[2].str(), false, t++});
            else if (a2 == "JPH_SHADER_BIND_RENDER_POSITIONS") out.push_back({m[2].str(), true, u++});
        }
    }
    return out;
}

// `#define AVER_REG_<name> <reg>` lines of a generated header.
static std::map<std::string, std::string> parseRegs(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    const std::regex def(R"(^#define\s+AVER_REG_(\w+)\s+(\w+)\s*$)");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, def)) out[m[1].str()] = m[2].str();
    }
    return out;
}

static const std::vector<std::string>& kernelNames() {
    static const std::vector<std::string> names = {
        "HairTeleport", "HairApplyDeltaTransform", "HairSkinVertices", "HairSkinRoots", "HairApplyGlobalPose",
        "HairCalculateCollisionPlanes", "HairGridClear", "HairGridAccumulate", "HairGridNormalize", "HairIntegrate",
        "HairUpdateRoots", "HairUpdateStrands", "HairUpdateVelocity", "HairUpdateVelocityIntegrate", "HairCalculateRenderPositions"};
    return names;
}

static const hairregs::HairKernel* tableRow(const std::string& name) {
    for (unsigned i = 0; i < hairregs::kHairKernelCount; ++i)
        if (name == hairregs::kHairKernels[i].name) return &hairregs::kHairKernels[i];
    return nullptr;
}

// One kernel: expected bindings from the source header, compared with the table row, the regs header and the wrapper.
static void checkKernel(const std::string& name, const std::string& bindingsText, const std::string& stem, bool wind) {
    const std::vector<Binding> want = parseBindings(bindingsText);
    check(!want.empty(), name + ": bindings parsed from the source header");

    const hairregs::HairKernel* row = tableRow(name);
    check(row != nullptr, name + ": has a row in HairRegs.inc");
    if (row) {
        unsigned t = 0, u = 0;
        for (const Binding& b : want) (b.rw ? u : t)++;
        check(row->bindingCount == want.size() && row->srvCount == t && row->uavCount == u, name + ": the table's binding and slot counts match");
        check(row->bindingCount <= 16, name + ": fits the host's 16 bindings per kernel");
        bool same = row->bindingCount == want.size();
        for (unsigned i = 0; same && i < row->bindingCount; ++i)
            same = want[i].name == row->bindings[i].name && (want[i].rw ? 1 : 0) == row->bindings[i].kind && want[i].slot == row->bindings[i].slot;
        check(same, name + ": every binding's name, kind and slot matches (declaration order, t then u)");
        check(std::string(row->file) == stem + ".hlsl", name + ": the table names the wrapper " + stem + ".hlsl");
    }

    std::string regsText;
    const bool haveRegs = readFile(dirJoin(AVER_HAIR_SHADER_DIR, stem + ".regs.hlsli"), regsText);
    check(haveRegs, name + ": " + stem + ".regs.hlsli exists");
    if (haveRegs) {
        const std::map<std::string, std::string> regs = parseRegs(regsText);
        bool ok = true;
        for (const Binding& b : want) {
            const auto it = regs.find(b.name);
            ok = ok && it != regs.end() && it->second == std::string(b.rw ? "u" : "t") + std::to_string(b.slot);
        }
        check(ok, name + ": the regs header assigns every binding its register");
        const size_t expected = want.size() + (wind ? 1u : 2u);
        check(regs.size() == expected, name + ": and nothing else (" + std::to_string(regs.size()) + " defines, expected " + std::to_string(expected) + ")");
        if (wind) {
            check(regs.count("gWind") && regs.at("gWind") == "b3", name + ": gWind is b3");
        } else {
            check(regs.count("gContext") && regs.at("gContext") == "b3" && regs.count("gIterationContext") && regs.at("gIterationContext") == "b4",
                  name + ": gContext is b3 and gIterationContext b4");
        }
    }

    std::string wrapper;
    const bool haveWrapper = readFile(dirJoin(AVER_HAIR_SHADER_DIR, stem + ".hlsl"), wrapper);
    check(haveWrapper, name + ": the wrapper " + stem + ".hlsl exists");
    if (haveWrapper) {
        check(wrapper.find("#define JPH_SHADER_OVERRIDE_MACROS") != std::string::npos, name + ": the wrapper overrides Jolt's macros");
        check(wrapper.find("#include \"AverHairMacros.hlsli\"") != std::string::npos, name + ": and includes the Aver macro header");
        check(wrapper.find("#include \"" + stem + ".regs.hlsli\"") != std::string::npos, name + ": and its register header");
        const std::string kernel = wind ? "AverHairWind.hlsl" : name + ".hlsl";
        check(wrapper.find("#include \"" + kernel + "\"") != std::string::npos, name + ": and the vendored kernel " + kernel);
        // The macro header must come first: the vendored kernel's own includes would otherwise declare buffers with no register.
        check(wrapper.find("AverHairMacros.hlsli") < wrapper.find("\"" + kernel + "\""), name + ": the macros come before the kernel");
    }
}

int main() {
    AVER_INFO("=== the table, the generated headers and the wrappers against Jolt's binding headers ===");
    check(hairregs::kHairKernelCount == kernelNames().size() + 1, "the table has Jolt's fifteen kernels and the wind kernel");
    for (const std::string& name : kernelNames()) {
        std::string text;
        if (!readFile(dirJoin(AVER_JOLT_SHADER_DIR, name + "Bindings.h"), text)) {
            check(false, name + ": " + name + "Bindings.h is missing from the vendored tree");
            continue;
        }
        checkKernel(name, text, "AverJolt" + name, false);
    }
    {
        std::string text;
        const bool ok = readFile(dirJoin(AVER_PHYS_HAIR_SHADER_DIR, "AverHairWind.hlsl"), text);
        check(ok, "the wind kernel source exists");
        if (ok) checkKernel("AverHairWind", text, "AverHairWindCS", true);
    }

    AVER_INFO("=== AverHairMacros.hlsli against Jolt's ShaderCore.h ===");
    {
        std::string core, macros;
        const bool a = readFile(dirJoin(AVER_JOLT_SHADER_DIR, "ShaderCore.h"), core);
        const bool b = readFile(dirJoin(AVER_HAIR_SHADER_DIR, "AverHairMacros.hlsli"), macros);
        check(a && b, "both headers can be read");
        if (a && b) {
            // Every macro Jolt's header defines (either branch) must exist in ours, except the override switch itself, which ours defines too.
            std::set<std::string> names;
            const std::regex def(R"(#define\s+(JPH_\w+))");
            for (std::sregex_iterator it(core.begin(), core.end(), def), end; it != end; ++it) names.insert((*it)[1].str());
            names.erase("JPH_SHADER_GENERATE_WRAPPER");
            bool all = !names.empty();
            for (const std::string& n : names) {
                const bool has = macros.find("#define " + n) != std::string::npos;
                if (!has) AVER_ERROR("    ShaderCore.h defines {} and AverHairMacros.hlsli does not", n);
                all = all && has;
            }
            check(all, "every macro ShaderCore.h defines is defined by AverHairMacros.hlsli (" + std::to_string(names.size()) + " checked)");

            // Every type alias.
            std::set<std::string> aliases;
            const std::regex use(R"(using\s+(JPH_\w+)\s*=)");
            for (std::sregex_iterator it(core.begin(), core.end(), use), end; it != end; ++it) aliases.insert((*it)[1].str());
            bool allAlias = !aliases.empty();
            for (const std::string& n : aliases) {
                const bool has = std::regex_search(macros, std::regex("\\b" + n + "\\s*(\\[4\\])?;"));
                if (!has) AVER_ERROR("    ShaderCore.h aliases {} and AverHairMacros.hlsli does not", n);
                allAlias = allAlias && has;
            }
            check(allAlias, "every type alias is declared by AverHairMacros.hlsli (" + std::to_string(aliases.size()) + " checked)");

            // The three declarations that carry a binding must carry the register macro, and nothing else may differ in kind.
            check(macros.find("cbuffer name : register(AVER_REG_##name)") != std::string::npos, "constants carry their register macro");
            check(macros.find("JPH_SHADER_BUFFER(type) name : register(AVER_REG_##name);") != std::string::npos, "read-only buffers carry theirs");
            check(macros.find("JPH_SHADER_RW_BUFFER(type) name : register(AVER_REG_##name);") != std::string::npos, "read-write buffers carry theirs");
        }
    }

    AVER_INFO("=== the wind kernel's struct header against Jolt's JPH_HairVelocity ===");
    {
        std::string structs, wind;
        const bool a = readFile(dirJoin(AVER_JOLT_SHADER_DIR, "HairStructs.h"), structs);
        const bool b = readFile(dirJoin(AVER_HAIR_SHADER_DIR, "AverHairWindStructs.hlsli"), wind);
        check(a && b, "both headers can be read");
        if (a && b) {
            const std::regex members(R"(JPH_SHADER_STRUCT_MEMBER\(\s*(\w+)\s*,\s*(\w+)\s*\))");
            auto velocityMembers = [&](const std::string& text) {
                std::vector<std::string> out;
                const size_t at = text.find("JPH_SHADER_STRUCT_BEGIN(JPH_HairVelocity)");
                const size_t endAt = text.find("JPH_SHADER_STRUCT_END(JPH_HairVelocity)");
                if (at == std::string::npos || endAt == std::string::npos) return out;
                const std::string body = text.substr(at, endAt - at);
                for (std::sregex_iterator it(body.begin(), body.end(), members), end; it != end; ++it) out.push_back((*it)[1].str() + " " + (*it)[2].str());
                return out;
            };
            const std::vector<std::string> want = velocityMembers(structs);
            const std::vector<std::string> have = velocityMembers(wind);
            check(!want.empty() && want == have, "JPH_HairVelocity has the same members in both headers");
            const std::regex batch(R"(JPH_SHADER_CONSTANT\(\s*int\s*,\s*cHairPerVertexBatch\s*,\s*(\d+)\s*\))");
            std::smatch m1, m2;
            check(std::regex_search(structs, m1, batch) && std::regex_search(wind, m2, batch) && m1[1].str() == m2[1].str(), "cHairPerVertexBatch is the same number");
        }
    }

    AVER_INFO("=== the ribbon shader ===");
    {
        std::string ribbons;
        check(readFile(dirJoin(AVER_HAIR_SHADER_DIR, "AverHairRibbons.hlsl"), ribbons), "AverHairRibbons.hlsl exists");
        check(ribbons.find("CSRibbons") != std::string::npos && ribbons.find("register(b3)") != std::string::npos &&
                  ribbons.find("register(t0)") != std::string::npos && ribbons.find("register(t1)") != std::string::npos &&
                  ribbons.find("register(u0)") != std::string::npos,
              "it declares the entry point and the t0, t1, u0 and b3 registers HairScene's layout expects");
    }

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures;
}
