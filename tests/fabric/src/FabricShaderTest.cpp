// FabricShaderTest -- G6 SH2 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/ShaderGame (five ShaderMaterials over .gdshader files, an inline
// Shader, a .tres material, a shader with an error and a particle shader), then what it wrote read back with the engine's own loaders: the .ocmat (GRAPHREF, FX
// record, GDREGS, TEX slots), the .ocgraph (the GodotShader node, compiled by pbr::compileMaterialGraph), Fabric/shaders.json and the per-property report.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphGodot.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/pbr/MaterialGraphHlsl.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
#if defined(_WIN32)
    const std::string wrapped = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    const std::string wrapped = cmd + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static std::string slurp(const std::string& path) {
    std::string s;
    readFileText(path, s);
    return s;
}

static float lin(double c) { return static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)); }
static bool near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }
static int asInt(float f) { int i; std::memcpy(&i, &f, sizeof i); return i; }

int main() {
    AVER_INFO("FabricShaderTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/ShaderGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/main.tscn", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-shader-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();
    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/ShaderGame/Content";
    const auto load = [&](const std::string& stem, pbr::MaterialDesc& d, fmt::OcMatExtras& ex) {
        std::string err;
        const bool ok = fmt::loadOcmat(content + "/Materials/" + stem + ".ocmat", d, &ex, &err);
        if (!ok) AVER_ERROR("  cannot load {}: {}", stem, err);
        return ok;
    };

    AVER_INFO("-- what was written --");
    {
        int mats = 0, graphs = 0;
        for (const auto& e : std::filesystem::directory_iterator(content + "/Materials", ec)) if (e.path().extension() == ".ocmat") ++mats;
        for (const auto& e : std::filesystem::directory_iterator(content + "/Shaders", ec)) if (e.path().extension() == ".ocgraph") ++graphs;
        check(mats == 5, "five shader materials were written (water, ocean, dissolve, toon, inline); the broken and particle shaders wrote none (got " + std::to_string(mats) + ")");
        check(graphs == 4, "four shader graphs were written, one per shader that converted, shared by the materials that use it (got " + std::to_string(graphs) + ")");
        check(std::filesystem::exists(out1 + "/ShaderGame/Fabric/shaders.json", ec), "Fabric/shaders.json was written");
    }

    AVER_INFO("-- the water shader through two materials --");
    {
        pbr::MaterialDesc d;
        fmt::OcMatExtras ex;
        check(load("ShaderGame_main_M_Water", d, ex), "the inline-parameter material loads");
        check(ex.graphRef == "Shaders/water.ocgraph", "its GRAPHREF names the shared graph");
        check(d.fx.cull == pbr::FxCull::None && d.twoSided && d.fx.depthDraw == pbr::FxDepthDraw::Always && d.fx.gdVertex && !d.fx.gdDiscard,
              "render_mode cull_disabled, depth_draw_always and the shader's vertex() become the FX record");
        check(d.alphaMode == pbr::AlphaMode::Blend, "fragment() writes ALPHA, so the material is blended");
        // layout: shallow (vec4) reg 0; deep (vec3) reg 1; amplitude reg 1.w; speed reg 2.x; octaves 2.y; foam 2.z; flow reg 3.xy; global wind_strength reg 4
        check(d.gd.registerCount() == 5, "five uniform registers (got " + std::to_string(d.gd.registerCount()) + ")");
        if (d.gd.registerCount() == 5) {
            const std::vector<float>& r = d.gd.regs;
            check(near(r[0], lin(0.1)) && near(r[1], lin(0.5)) && near(r[2], lin(0.6)) && near(r[3], 0.7f), "shallow: the shader's default, source_color decoded to linear, alpha as written");
            check(near(r[4], 0.0f) && near(r[5], lin(0.1)) && near(r[6], lin(0.3)) && near(r[7], 0.25f), "deep (default) in register 1 with amplitude 0.25 in its w");
            check(near(r[8], 1.5f) && asInt(r[9]) == 2 && asInt(r[10]) == 1, "speed 1.5, octaves 2 and foam true (the int and bool bit patterns)");
            check(near(r[12], 0.1f) && near(r[13], 0.0f), "flow (vec2) in register 3");
            check(near(r[16], 0.75f), "the global uniform wind_strength takes the project's [shader_globals] default (0.75)");
        }
        check(d.textures[pbr::kGdTableSlot].path == "@gdu" && d.textures[0].empty() && d.textures[1].empty(), "slot 7 marks the uniform table; unset texture parameters leave their slots empty");

        pbr::MaterialDesc o;
        fmt::OcMatExtras ox;
        check(load("ShaderGame_ocean", o, ox), "the .tres material loads");
        check(ox.graphRef == "Shaders/water.ocgraph" && o.fx.renderPriority == 2, "it shares the graph; render_priority 2 is the FX priority");
        if (o.gd.registerCount() == 5) {
            const std::vector<float>& r = o.gd.regs;
            check(near(r[0], lin(0.2)) && near(r[1], lin(0.7)) && near(r[2], lin(0.8)) && near(r[3], 0.6f), "its shallow overrides the default");
            check(near(r[7], 0.5f) && asInt(r[9]) == 3 && asInt(r[10]) == 0, "amplitude 0.5, octaves 3, foam false");
            check(near(r[12], 0.25f) && near(r[13], -0.5f), "flow (0.25, -0.5)");
        }
        check(o.textures[0].path == "Textures/textures/mask.png" && o.textures[1].path == "Textures/textures/detail_n.png",
              "foam_tex (source_color) is in slot 0, normal_tex (linear) in slot 1, as the textures copied into Content");
    }

    AVER_INFO("-- the .ocmat text round-trips --");
    {
        pbr::MaterialDesc d;
        fmt::OcMatExtras ex;
        load("ShaderGame_ocean", d, ex);
        const std::string text = fmt::writeOcmat(d, &ex);
        pbr::MaterialDesc back;
        fmt::OcMatExtras bx;
        std::string err;
        check(fmt::parseOcmat(text, back, &bx, &err), "the written text parses again (" + err + ")");
        check(back.gd.regs.size() == d.gd.regs.size() && std::memcmp(back.gd.regs.data(), d.gd.regs.data(), d.gd.regs.size() * sizeof(float)) == 0, "GDREGS round-trips bit for bit");
        check(bx.graphRef == ex.graphRef && back.fx.gdVertex == d.fx.gdVertex && back.fx.renderPriority == d.fx.renderPriority && back.textures[pbr::kGdTableSlot].path == "@gdu" &&
                  back.textures[0].path == d.textures[0].path,
              "GRAPHREF, the FX gdvs flag, the priority, the uniform-table slot and the textures round-trip");
        check(text.find("TEX layer1normal") == std::string::npos, "the uniform-table slot is not written as a TEX record");
        // an ordinary material is written exactly as before: no GDREGS, no gd keys
        pbr::MaterialDesc plain;
        plain.name = "Plain";
        const std::string plainText = fmt::writeOcmat(plain, nullptr);
        check(plainText.find("GDREGS") == std::string::npos && plainText.find("gdvs") == std::string::npos && plainText.find("gddiscard") == std::string::npos, "an ordinary material writes none of the shader records");
    }

    AVER_INFO("-- dissolve: render modes, discard, a matrix --");
    {
        pbr::MaterialDesc d;
        fmt::OcMatExtras ex;
        check(load("ShaderGame_main_M_Dissolve", d, ex), "the dissolve material loads");
        check(d.fx.cull == pbr::FxCull::Front && d.fx.blend == pbr::FxBlend::Add && d.fx.noReceiveShadows && d.fx.gdDiscard && !d.fx.gdVertex,
              "cull_front, blend_add, shadows_disabled, and a discarding fragment compiled without forced early depth");
        check(d.alphaMode == pbr::AlphaMode::Mask, "ALPHA_SCISSOR_THRESHOLD makes the material masked");
        check(d.textures[1].path == "Textures/textures/height.png", "the linear noise texture takes slot 1");
        // layout: threshold reg 0.x; edge_color (vec4) reg 1; uv_xform (mat3) regs 2-4
        check(d.gd.registerCount() == 5, "five registers");
        if (d.gd.registerCount() == 5) {
            const std::vector<float>& r = d.gd.regs;
            check(near(r[0], 0.35f), "threshold 0.35");
            check(near(r[4], lin(0.5)) && near(r[5], lin(0.25)) && near(r[6], lin(0.1)) && near(r[7], 1.0f), "edge_color decoded to linear");
            check(near(r[8], 2.0f) && near(r[13], 2.0f) && near(r[18], 1.0f) && near(r[9], 0.0f), "uv_xform (a Basis) is the mat3 whose columns are the basis columns, one per register");
        }
    }

    AVER_INFO("-- toon, and the inline shader --");
    {
        pbr::MaterialDesc d;
        fmt::OcMatExtras ex;
        check(load("ShaderGame_main_M_Toon", d, ex), "the toon material loads");
        check(d.fx.cull == pbr::FxCull::Back && !d.fx.gdVertex && d.alphaMode == pbr::AlphaMode::Opaque, "default cull back, no vertex(), opaque");
        // layout: color reg 0.xyz, bands 0.w, instance tint_amount reg 1
        check(d.gd.registerCount() == 2, "an instance uniform has its own register (got " + std::to_string(d.gd.registerCount()) + ")");
        if (d.gd.registerCount() == 2) {
            check(near(d.gd.regs[0], lin(0.1)) && near(d.gd.regs[1], lin(0.6)) && near(d.gd.regs[2], lin(0.2)) && near(d.gd.regs[3], 4.0f), "color decoded, bands 4");
            check(near(d.gd.regs[4], 0.5f), "tint_amount 0.5 in the instance register");
        }
        pbr::MaterialDesc i;
        fmt::OcMatExtras ix;
        check(load("ShaderGame_main_M_Inline", i, ix), "the inline-Shader material loads");
        check(ix.graphRef == "Shaders/main_Shader_inline.ocgraph" && i.gd.registerCount() == 1, "its graph is named after the scene and the sub-resource");
        if (i.gd.registerCount() == 1) check(near(i.gd.regs[0], 0.0f) && near(i.gd.regs[2], 1.0f) && near(i.gd.regs[3], 1.0f), "col = blue, alpha 1");
    }

    AVER_INFO("-- the graphs and the manifest --");
    {
        const char* graphs[] = {"water", "dissolve", "toon", "main_Shader_inline"};
        bool allOk = true;
        for (const char* g : graphs) {
            fmt::OcGraphData data;
            std::string err;
            if (!fmt::loadOcgraph(content + "/Shaders/" + g + ".ocgraph", data, &err)) { allOk = false; AVER_ERROR("  cannot load {}: {}", g, err); continue; }
            const pbr::MaterialGraphBody body = pbr::compileMaterialGraph(data);
            if (!body.ok || !body.isGodot || body.gd.ns.empty() || (!body.gd.hasFragment && !body.gd.hasVertex)) { allOk = false; AVER_ERROR("  {} did not compile: {}", g, body.error); }
        }
        check(allOk, "every written graph loads and compiles into a Godot shader entry for the registry");
        fmt::OcGraphData water;
        fmt::loadOcgraph(content + "/Shaders/water.ocgraph", water, nullptr);
        fmt::GodotShaderPart p;
        fmt::unpackGodotShader(water, p, nullptr);
        check(p.hasFragment && p.hasVertex && p.varyings.size() == 1 && p.regsU == 4 && p.regsG == 1 && p.fragmentSource.find("GDN(main_fragment)") != std::string::npos &&
                  p.vertexSource.find("GDN(main_vertex)") != std::string::npos && p.fragmentSource.find("WAVE_SCALE") == std::string::npos,
              "the water graph carries fragment and vertex sources with the include expanded, one varying, 4 normal registers and 1 global");
        const std::string manifest = slurp(out1 + "/ShaderGame/Fabric/shaders.json");
        check(manifest.find("\"name\": \"wind_strength\"") != std::string::npos && manifest.find("\"scope\": \"global\"") != std::string::npos &&
                  manifest.find("\"name\": \"foam_tex\"") != std::string::npos && manifest.find("\"slot\": 0") != std::string::npos &&
                  manifest.find("ShaderGame_ocean") != std::string::npos,
              "shaders.json lists the uniform layout, the sampler slots, the globals and the materials");
    }

    AVER_INFO("-- the report --");
    {
        const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
        const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(has("\"type\":\"ShaderMaterial\"", "\"property\":\"shader_parameter/amplitude\"", "\"rating\":\"converted\""), "a shader parameter is converted");
        check(has("\"type\":\"ShaderMaterial\"", "shader_parameter/no_such_uniform", "\"rating\":\"approximated\""), "a parameter the shader does not declare is approximated (ignored), with the reason");
        check(has("\"type\":\"Shader\"", "\"rating\":\"unsupported\"", "this_does_not_exist"), "a shader error is unsupported with the compiler's message");
        check(has("\"type\":\"Shader\"", "\"rating\":\"unsupported\"", "shader_type particles"), "a particles shader is unsupported with its reason");
        check(has("\"type\":\"Shader\"", "\"property\":\"light()\"", "\"rating\":\"unsupported\""), "light() is unsupported");
        check(has("\"type\":\"Shader\"", "\"property\":\"RIM\"", "\"rating\":\"approximated\""), "RIM is approximated");
        check(has("\"type\":\"Shader\"", "render_mode vertex_lighting", "\"rating\":\"approximated\""), "vertex_lighting is approximated");
        check(has("\"type\":\"Shader\"", "instance uniforms", "\"rating\":\"approximated\""), "instance uniforms are approximated (per material, not per instance)");
        check(has("\"kind\":\"setting\"", "shader_globals/wind_strength", "\"rating\":\"approximated\""), "the project's shader global is approximated: its default rides in the table");
        check(has("\"type\":\"ShaderMaterial\"", "Materials/ShaderGame_ocean.ocmat", "\"rating\":\"converted\""), "the .tres material is converted");
    }

    AVER_INFO("FabricShaderTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
