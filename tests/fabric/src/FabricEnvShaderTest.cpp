// FabricEnvShaderTest -- G6 SH3 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/EnvShaderGame (a ShaderMaterial over a sky shader on the
// environment's Sky, one over a fog shader on a FogVolume, one over a particles shader on a GPUParticles3D, a ParticleProcessMaterial emitter, a VisualShader on a
// box, and a particles shader wrongly put on a mesh), then what it wrote read back with the engine's own loaders: the godotenv graphs (kind, registers at the
// material's values), the .ocparticle (parsed back into a ParticleSystemDef with its shader name), the ParticleEmitter component, Fabric/shaders.json, the generated
// VisualShader text and the per-item report.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphGodot.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/formats/OcParticle.hpp"
#include "aver/formats/OcWorld.hpp"
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
namespace fs = std::filesystem;

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
    FILE* pipe = _popen(("\"" + cmd + " 2>&1\"").c_str(), "r");
#else
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
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
static unsigned asBits(float f) { unsigned u; std::memcpy(&u, &f, sizeof u); return u; }

// The first file in `dir` whose name ends with `suffix`.
static std::string findFile(const std::string& dir, const std::string& suffix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) return e.path().string();
    }
    return {};
}

int main() {
    AVER_INFO("FabricEnvShaderTest");
    const fs::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/EnvShaderGame";
    std::error_code ec;
    check(fs::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(fs::exists(dataDir + "/main.tscn", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const fs::path outRoot = fs::temp_directory_path() / "aver-fabric-envshader-test";
    fs::remove_all(outRoot, ec);
    fs::create_directories(outRoot, ec);
    const std::string out = (outRoot / "one").string();
    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out + "/EnvShaderGame/Content";
    const std::string shaders = content + "/Shaders";

    AVER_INFO("-- the sky shader --");
    {
        const std::string f = findFile(shaders, ".sky.ocgraph");
        check(!f.empty(), "a sky graph was written");
        fmt::OcGraphData g;
        std::string err;
        fmt::GodotEnvPart p;
        const bool loaded = !f.empty() && fmt::loadOcgraph(f, g, &err) && fmt::unpackGodotEnvShader(g, p, &err);
        check(loaded, "it loads and unpacks (" + err + ")");
        check(loaded && p.kind == "sky" && p.source.find(p.ns + "main_sky") != std::string::npos, "it is a sky graph with the sky entry");
        // top_color vec3 reg 0, horizon vec3 reg 1, sun_size reg 1.w, star_density 2.x, show_stars 2.y, global wind_strength reg 3 (3 normal registers)
        check(loaded && p.regs == 3 && p.globalRegs == 1 && p.defaults.size() == 16, "three normal registers and one global");
        if (loaded && p.defaults.size() == 16) {
            check(near(p.defaults[0], lin(0.2)) && near(p.defaults[1], lin(0.4)) && near(p.defaults[2], lin(0.9)), "top_color: the material's value, source_color decoded to linear");
            check(near(p.defaults[4], lin(0.7)) && near(p.defaults[5], lin(0.8)) && near(p.defaults[6], lin(0.9)), "horizon_color keeps the shader's default");
            check(near(p.defaults[7], 0.05f), "sun_size 0.05 in the fourth component of register 1");
            check(asInt(p.defaults[8]) == 5 && asInt(p.defaults[9]) == 0, "star_density 5 and show_stars false as raw words");
            check(near(p.defaults[12], 0.5f), "the global wind_strength takes the project's default");
        }
    }

    AVER_INFO("-- the fog shader --");
    {
        const std::string f = findFile(shaders, ".fog.ocgraph");
        check(!f.empty(), "a fog graph was written");
        fmt::OcGraphData g;
        fmt::GodotEnvPart p;
        const bool loaded = !f.empty() && fmt::loadOcgraph(f, g, nullptr) && fmt::unpackGodotEnvShader(g, p, nullptr);
        check(loaded && p.kind == "fog" && p.source.find(p.ns + "main_fog") != std::string::npos, "it is a fog graph with the fog entry");
        check(loaded && p.defaults.size() >= 4 && near(p.defaults[0], 2.5f), "density: the material's 2.5 in register 0");
        check(loaded && p.defaults.size() >= 12 && near(p.defaults[8], 1.0f) && near(p.defaults[11], 2.0f), "glow keeps the shader's default");
    }

    AVER_INFO("-- the particles shader and its emitter --");
    {
        const std::string f = findFile(shaders, ".particles.ocgraph");
        check(!f.empty(), "a particles graph was written");
        fmt::OcGraphData g;
        fmt::GodotEnvPart p;
        const bool loaded = !f.empty() && fmt::loadOcgraph(f, g, nullptr) && fmt::unpackGodotEnvShader(g, p, nullptr);
        check(loaded && p.kind == "particles" && p.hasStart && p.hasProcess, "it is a particles graph with start() and process()");
        // gravity_and_drag vec4 reg 0, emit_dir vec3 reg 1, spin reg 1.w
        check(loaded && p.defaults.size() >= 8 && near(p.defaults[0], 0.0f) && near(p.defaults[1], -9.8f) && near(p.defaults[5], 2.0f) && near(p.defaults[7], 4.0f),
              "emit_dir (0, 2, 0) and spin 4 are the material's, gravity_and_drag keeps the shader's default");
        const std::string pf = findFile(content + "/Particles", ".ocparticle");
        check(!pf.empty(), "an .ocparticle was written for the emitter");
        particles::ParticleSystemDef def;
        std::string err;
        const bool parsed = !pf.empty() && fmt::loadOcparticleSystem(pf, def, nullptr, &err);
        check(parsed, "the effect parses (" + err + ")");
        if (parsed && !def.emitters.empty()) {
            const particles::ParticleEffect& e = def.emitters[0];
            check(e.godotShader.size() > 0 && fs::exists(content + "/Shaders/" + e.godotShader + ".particles.ocgraph", ec), "it names the shader whose graph exists");
            check(e.simTier == particles::SimTier::Gpu && e.maxParticles == 64 && std::fabs(e.lifetimeMin - 2.0f) < 1e-5f, "SIM gpu, amount 64, lifetime 2");
            check(std::fabs(e.emissionRate - 32.0f) < 1e-3f && e.burstCount == 0, "the rate is amount / lifetime");
            check(std::fabs(e.sizeStart - 50.0f) < 1e-3f, "the QuadMesh's 0.5 m is the sprite size (50 cm)");
            check(def.params.size() == 3 && def.params[0].name == "reg0" && def.params[0].kind == 2, "the shader's three registers are the colour-kind user parameters reg0 to reg2");
            // reg2 holds the int uniforms as raw words: -1 is a NaN pattern and 100000000 does not survive six digits
            check(def.params.size() == 3 && asBits(def.params[2].v[0]) == 0xFFFFFFFFu && asBits(def.params[2].v[1]) == 100000000u, "int defaults keep their bit patterns through the baked user parameters");
            if (loaded && p.defaults.size() >= 12) check(asBits(p.defaults[8]) == 0xFFFFFFFFu && asBits(p.defaults[9]) == 100000000u, "and the graph holds the same raw words");
        }
        const std::string world = slurp(content + "/Maps/main.ocworld");
        check(world.find("ParticleEmitter") != std::string::npos && world.find("Particles/") != std::string::npos && world.find("sim=gpu") != std::string::npos,
              "the level places a ParticleEmitter component on the GPUParticles3D, forced to the GPU tier");
    }

    AVER_INFO("-- the VisualShader --");
    {
        const std::string vs = slurp(out + "/EnvShaderGame/Fabric/visualshaders/rim.gdshader.txt");
        check(vs.find("shader_type spatial;") != std::string::npos && vs.find("uniform float rim_power") != std::string::npos, "the generated shading-language text was written for Aver.Fabric's VisualShader.GetCode");
        std::error_code e2;
        int mats = 0;
        for (const auto& e : fs::directory_iterator(content + "/Materials", e2)) if (e.path().extension() == ".ocmat") ++mats;
        check(mats == 1, "the box with the VisualShader material got an .ocmat running the converted graph (got " + std::to_string(mats) + ")");
        const std::string m = findFile(content + "/Materials", ".ocmat");
        pbr::MaterialDesc d;
        fmt::OcMatExtras ex;
        check(!m.empty() && fmt::loadOcmat(m, d, &ex, nullptr) && !ex.graphRef.empty() && d.gd.registerCount() >= 1 && near(d.gd.regs[0], 5.0f), "rim_power 5 is in its register table");
    }

    AVER_INFO("-- the manifest and the report --");
    {
        const std::string manifest = slurp(out + "/EnvShaderGame/Fabric/shaders.json");
        check(manifest.find("\"kind\": \"sky\"") != std::string::npos && manifest.find("\"kind\": \"fog\"") != std::string::npos && manifest.find("\"kind\": \"particles\"") != std::string::npos &&
                  manifest.find("\"kind\": \"spatial\"") != std::string::npos && manifest.find("\"name\": \"star_density\"") != std::string::npos,
              "shaders.json lists the four kinds with their uniform layouts");
        const std::string jsonl = slurp(out + "/FabricReport.jsonl");
        const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(has("\"type\":\"ShaderMaterial\"", ".sky.ocgraph", "\"rating\":\"converted\""), "the sky ShaderMaterial is converted");
        check(has("\"type\":\"Shader\"", "uniform noise_tex", "\"rating\":\"approximated\""), "an unbound texture is approximated, with the reason");
        check(has("\"type\":\"Environment\"", "sky_material", "\"rating\":\"approximated\""), "the environment's sky_material is rated approximated");
        check(has("\"type\":\"ShaderMaterial\"", ".fog.ocgraph", "\"rating\":\"converted\""), "the fog ShaderMaterial is converted");
        check(has("\"type\":\"GPUParticles3D\"", "\"property\":\"process_material\"", "\"rating\":\"converted\""), "the shader process material is converted");
        check(has("\"type\":\"GPUParticles3D\"", "\"property\":\"process_material\"", "\"rating\":\"unsupported\""), "the ParticleProcessMaterial one is unsupported");
        check(has("\"type\":\"Shader\"", "\"property\":\"TRANSFORM\"", "\"rating\":\"approximated\""), "TRANSFORM is approximated");
        check(has("\"type\":\"VisualShader\"", "VisualShaderNodeCurveTexture", "\"rating\":\"unsupported\""), "an unknown VisualShader node is unsupported per node");
        check(has("\"type\":\"VisualShader\"", "VisualShaderNodeTexture", "\"rating\":\"approximated\""), "an embedded-texture node is approximated");
        check(has("\"type\":\"Shader\"", "\"rating\":\"unsupported\"", "shader_type particles"), "a particles shader on a mesh is refused with its reason");
    }

    AVER_INFO("FabricEnvShaderTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : exitCode(ExitCode::Failed);
}
