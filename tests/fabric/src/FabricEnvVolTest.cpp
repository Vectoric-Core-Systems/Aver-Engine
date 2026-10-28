// FabricEnvVolTest -- G4 R8 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/EnvVolGame (volumetric fog, two FogVolumes and a custom-shader
// one, a FogMaterial, a ReflectionProbe, LightmapGI with a baked file, LightmapProbe, VoxelGI, and the screen-space / SDF environment flags), then the files it
// wrote read back with the engine's own loaders. Also EnvGame (no volumetric fog) and LightsGame (no environment): their projects must not gain the fog key.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
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

static int find(const fmt::OcWorldData& w, std::initializer_list<const char*> path) {
    int parent = -1;
    for (const char* name : path) {
        int found = -1;
        for (usize i = 0; i < w.placements.size(); ++i)
            if (w.placements[i].parent == parent && w.placements[i].name == name) { found = static_cast<int>(i); break; }
        if (found < 0) return -1;
        parent = found;
    }
    return parent;
}

static const std::string* prop(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props")
            if (const std::string* v = c.find(key)) return v;
    return nullptr;
}

int main() {
    AVER_INFO("FabricEnvVolTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/EnvVolGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aver-fabric-envvol-test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::string out = (root / "out").string();
    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string proj = out + "/EnvVolGame";
    std::string err;

    AVER_INFO("-- the project --");
    {
        fmt::ProjectDesc pd;
        check(fmt::loadOcproject(proj + "/EnvVolGame.ocproject", pd, &err), "the project loads: " + err);
        check(pd.volFog == 1, "RENDER.VOLFOG 1: the environment enables volumetric fog, where fog volumes draw");
        check(pd.volFogDensity == 0.0f, "RENDER.VOLFOGDENSITY 0: the level's fog already carries the density");
        check(pd.hasRenderSettings(), "and they count as stated render settings");
    }

    AVER_INFO("-- the level --");
    {
        fmt::OcWorldData w;
        check(fmt::loadOcworld(proj + "/Content/Maps/main.ocworld", w, &err), "the level loads: " + err);
        check(w.hasFog && std::fabs(w.fogDensity - 0.001) < 1e-6, "volumetric_fog_density 0.1 per metre is the level's fog at 0.001 per cm");

        const int mist = find(w, {"Main", "Mist"});
        check(mist >= 0 && w.placements[static_cast<usize>(mist)].className == "FogVolume", "Mist is placed as a FogVolume");
        const std::string* sz = prop(w, mist, "size");
        check(sz && sz->find("Vector3(4,") != std::string::npos, "its size is kept in Godot metres");
        const std::string* shape = prop(w, mist, "shape");
        check(shape && *shape == "2", "its shape is Godot's cylinder number");
        const std::string* mat = prop(w, mist, "material");
        check(mat && mat->find("Object(FogMaterial") != std::string::npos && mat->find("density") != std::string::npos && mat->find("height_falloff") != std::string::npos &&
                  mat->find("edge_fade") != std::string::npos && mat->find("emission") != std::string::npos,
              "its FogMaterial is an inline object for Aver.Fabric with every key");
        const fmt::OcWorldPlacement& mp = w.placements[static_cast<usize>(mist)];
        check(std::fabs(mp.x + 300.0) < 1e-3 && std::fabs(mp.y - 200.0) < 1e-3 && std::fabs(mp.z - 100.0) < 1e-3, "at Godot (2, 1, 3) m = engine (-300, 200, 100) cm");
        const int haze = find(w, {"Main", "Haze"});
        const std::string* hs = prop(w, haze, "shape");
        check(hs && *hs == "4", "a world volume keeps shape 4");
        const int custom = find(w, {"Main", "Custom"});
        check(custom >= 0 && prop(w, custom, "material") == nullptr, "a ShaderMaterial is not written as a value");

        const int probe = find(w, {"Main", "Probe"});
        check(probe >= 0 && w.placements[static_cast<usize>(probe)].className == "ReflectionProbe", "the probe is placed as a ReflectionProbe");
        check(prop(w, probe, "box_projection") && *prop(w, probe, "box_projection") == "true" && prop(w, probe, "interior") && *prop(w, probe, "interior") == "true" &&
                  prop(w, probe, "blend_distance") && prop(w, probe, "origin_offset") && prop(w, probe, "ambient_mode") && *prop(w, probe, "ambient_mode") == "2",
              "with its Godot properties");
        const int lm = find(w, {"Main", "Lightmap"});
        check(lm >= 0 && w.placements[static_cast<usize>(lm)].className == "LightmapGI" && prop(w, lm, "bounces") && prop(w, lm, "light_data") == nullptr,
              "LightmapGI keeps its settings and drops the binary bake");
        check(find(w, {"Main", "LMProbe"}) >= 0 && w.placements[static_cast<usize>(find(w, {"Main", "LMProbe"}))].className == "LightmapProbe", "LightmapProbe is placed");
        const int vox = find(w, {"Main", "Voxels"});
        check(vox >= 0 && w.placements[static_cast<usize>(vox)].className == "VoxelGI" && prop(w, vox, "subdiv"), "VoxelGI is placed");
        // None of them is a carrier any more: a carrier would hold the original type in `_class`.
        check(prop(w, mist, "_class") == nullptr && prop(w, probe, "_class") == nullptr && prop(w, lm, "_class") == nullptr && prop(w, vox, "_class") == nullptr,
              "none of them is a Node3D carrier");
    }

    AVER_INFO("-- the report --");
    {
        const std::string jsonl = slurp(out + "/FabricReport.jsonl");
        const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(has("\"type\":\"FogVolume\"", "\"kind\":\"node\"", "\"rating\":\"converted\""), "a FogVolume node is converted");
        check(has("\"type\":\"FogVolume\"", "\"property\":\"size\"", "\"rating\":\"converted\"") && has("\"type\":\"FogVolume\"", "\"property\":\"shape\"", "\"rating\":\"converted\""),
              "its size and shape are converted");
        check(has("\"type\":\"FogMaterial\"", "\"property\":\"density\"", "\"rating\":\"converted\"") && has("\"type\":\"FogMaterial\"", "\"property\":\"albedo\"", "\"rating\":\"converted\""),
              "FogMaterial density and albedo are converted");
        check(has("\"type\":\"FogMaterial\"", "\"property\":\"height_falloff\"", "\"rating\":\"approximated\"") && has("\"type\":\"FogMaterial\"", "\"property\":\"edge_fade\"", "\"rating\":\"approximated\"") &&
                  has("\"type\":\"FogMaterial\"", "\"property\":\"emission\"", "\"rating\":\"approximated\""),
              "height falloff, edge fade and emission are approximated");
        check(has("\"type\":\"ShaderMaterial\"", "\"rating\":\"unsupported\"", "G6"), "a custom fog shader is unsupported");
        check(has("\"type\":\"ReflectionProbe\"", "\"kind\":\"node\"", "\"rating\":\"approximated\"") && has("\"type\":\"ReflectionProbe\"", "\"kind\":\"node\"", "sample"),
              "a ReflectionProbe is approximated and the report says nothing samples a capture");
        check(has("\"type\":\"ReflectionProbe\"", "\"property\":\"box_projection\"", "\"rating\":\"approximated\"") && has("\"type\":\"ReflectionProbe\"", "\"property\":\"size\"", "\"rating\":\"converted\"") &&
                  has("\"type\":\"ReflectionProbe\"", "\"property\":\"enable_shadows\"", "\"rating\":\"unsupported\""),
              "its properties are rated one by one");
        check(has("\"type\":\"LightmapGI\"", "\"kind\":\"node\"", "\"rating\":\"approximated\"") && has("\"type\":\"LightmapGI\"", "\"property\":\"light_data\"", "\"rating\":\"unsupported\"") &&
                  has("\"type\":\"LightmapGI\"", "\"property\":\"directional\"", "\"rating\":\"approximated\"") && has("\"type\":\"LightmapGI\"", "\"property\":\"bounces\"", "\"rating\":\"unsupported\""),
              "LightmapGI is approximated by real-time GI; the baked file and the bake settings are unsupported");
        check(has("\"type\":\"VoxelGI\"", "\"kind\":\"node\"", "\"rating\":\"approximated\"") && has("\"type\":\"LightmapProbe\"", "\"kind\":\"node\"", "\"rating\":\"approximated\""),
              "VoxelGI and LightmapProbe are approximated");
        check(has("\"type\":\"Environment\"", "\"property\":\"volumetric_fog_enabled\"", "\"rating\":\"approximated\""), "volumetric fog is approximated");
        check(has("\"property\":\"ssao_enabled\"", "\"rating\":\"approximated\"", "ray-traced sky occlusion") && has("\"property\":\"ssr_enabled\"", "\"rating\":\"approximated\"", "cone trace") &&
                  has("\"property\":\"ssil_enabled\"", "\"rating\":\"approximated\"", "global illumination") && has("\"property\":\"sdfgi_enabled\"", "\"rating\":\"approximated\"", "voxel"),
              "SSAO, SSR, SSIL and SDFGI are approximated by Aver's GI, and each says what covers it");
        check(!has("\"type\":\"FogVolume\"", "\"rating\":\"unsupported\"", "\"kind\":\"node\"") && !has("\"type\":\"ReflectionProbe\"", "\"rating\":\"unsupported\"", "\"kind\":\"node\""),
              "no environment node is rated unsupported as a whole");
    }

    AVER_INFO("-- projects without volumetric fog --");
    {
        for (const char* name : {"EnvGame", "LightsGame"}) {
            const std::string o = (root / (std::string("out_") + name)).string();
            const int c = runCapture("\"" + exe.string() + "\" fabric \"" + std::string(AVER_FABRIC_DATA) + "/" + name + "\" \"" + o + "\" --scenes-only", log);
            check(c == 0, std::string(name) + " converts");
            const std::string text = slurp(o + "/" + name + "/" + name + ".ocproject");
            check(!text.empty() && text.find("RENDER.VOLFOG") == std::string::npos, std::string(name) + ": the project names neither volumetric-fog key");
        }
    }

    AVER_INFO("FabricEnvVolTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
