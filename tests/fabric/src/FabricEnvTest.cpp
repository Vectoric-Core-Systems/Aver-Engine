// FabricEnvTest -- G4 R3 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/EnvGame (a WorldEnvironment with a procedural sky, depth
// and height fog, glow, a Godot tonemapper with exposure and white, display adjustments, GI flags, and CameraAttributesPractical with a far blur) and on a
// copy of it with an HDRI panorama sky, then the files it wrote read back with the engine's own loaders. Also LightsGame, which has no environment at all:
// its project and level must carry no tonemap, no adjustments and no post volume (the default path is unchanged).
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/GodotEnvironment.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/PanoramaSky.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static bool near(double a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

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

static double postValue(const fmt::OcPostVolume& v, const char* key, double def = -1e30) {
    const fmt::OcPostValue* p = v.find(key);
    return p ? p->v[0] : def;
}

// Radiance RGBE of a linear colour.
static void rgbe(const float c[3], unsigned char out[4]) {
    const float m = std::max(c[0], std::max(c[1], c[2]));
    if (m < 1e-32f) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int e = 0;
    const float f = std::frexp(m, &e) * 256.0f / m;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<unsigned char>(c[i] * f);
    out[3] = static_cast<unsigned char>(e + 128);
}

// A 32 x 16 panorama whose radiance falls from a bright blue-white zenith to a dark floor.
static void writePanorama(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    f << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 16 +X 32\n";
    for (int y = 0; y < 16; ++y) {
        const float up = std::cos((y + 0.5f) / 16.0f * 3.14159265f);   // sin(elevation)
        const float lum = up >= 0.0f ? 0.5f + 3.5f * up : 0.2f;
        const float c[3] = {lum * 0.8f, lum * 0.9f, lum};
        unsigned char px[4];
        rgbe(c, px);
        for (int x = 0; x < 32; ++x) f.write(reinterpret_cast<const char*>(px), 4);
    }
}

int main() {
    AVER_INFO("FabricEnvTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/EnvGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aver-fabric-env-test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    // The panorama scene needs an HDRI; the repository holds no binary fixture, so the test writes one into a copy of the project.
    const std::string src = (root / "EnvGame").string();
    std::filesystem::copy(dataDir, src, std::filesystem::copy_options::recursive, ec);
    check(!ec, "the fixture is copied");
    writePanorama(src + "/sky.hdr");
    const std::string out = (root / "out").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + src + "\" \"" + out + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string proj = out + "/EnvGame";
    const std::string content = proj + "/Content";
    std::string err;

    AVER_INFO("-- the project --");
    {
        fmt::ProjectDesc pd;
        check(fmt::loadOcproject(proj + "/EnvGame.ocproject", pd, &err), "the project loads: " + err);
        check(pd.postTonemap == 7, "RENDER.TONEMAP 7: Godot's AgX (3 + 4)");
        check(near(pd.postTonemapWhite, 6.0), "RENDER.TONEMAPWHITE is tonemap_white");
        check(pd.postAutoExposure == 0, "RENDER.AUTOEXPOSURE 0: Godot's exposure is fixed");
        check(near(pd.postBloom, 0.7), "RENDER.BLOOM is glow_intensity");
        check(pd.postExposure < 0.0f, "tonemap_exposure 2 x exposure_multiplier 0.5 is 1: no RENDER.EXPOSURE is needed");
        check(near(pd.postAdjustBrightness, 1.2) && near(pd.postAdjustContrast, 1.1) && near(pd.postAdjustSaturation, 0.8), "RENDER.ADJUST* carry the adjustments");
        check(pd.hasRenderSettings(), "and they count as stated render settings");
    }

    AVER_INFO("-- the main level --");
    {
        fmt::OcWorldData w;
        check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);
        const double k = std::pow(2.0, 1.0 / 2.2);
        check(w.hasSky && !w.skyPhysical && near(w.skyZenith[0], 0.2 * k, 1e-6) && near(w.skyHorizon[2], 0.9 * k, 1e-6),
              "the procedural sky's colours are scaled by sky_energy_multiplier in linear light");
        check(w.hasFog && near(w.fogStart, 2000.0) && near(w.fogDensity, 3.0 / 10000.0, 1e-6), "depth fog starts at fog_depth_begin (cm) and reaches ~95% at fog_depth_end");
        check(near(w.fogFalloff, 0.005, 1e-6) && near(w.fogHeight, 300.0), "height fog: |fog_height_density| per metre / 100 and fog_height in cm");

        check(w.postVolumes.size() == 1 && w.postVolumes[0].infinite && w.postVolumes[0].name == "WorldEnvironment", "one level-wide post volume carries the environment");
        if (!w.postVolumes.empty()) {
            const fmt::OcPostVolume& v = w.postVolumes[0];
            check(postValue(v, "tonemap") == 7 && near(postValue(v, "tonemapwhite"), 6.0), "tonemap AgX and its white point");
            check(near(postValue(v, "exposure"), 1.0) && postValue(v, "autoexposure") == 0, "exposure is tonemap_exposure x exposure_multiplier, eye adaptation off");
            check(near(postValue(v, "bloom"), 0.7) && near(postValue(v, "bloomthreshold"), 2.0), "glow becomes bloom and, when stated, its threshold");
            check(near(postValue(v, "adjbrightness"), 1.2) && near(postValue(v, "adjcontrast"), 1.1) && near(postValue(v, "adjsaturation"), 0.8), "the display adjustments");
            check(postValue(v, "dof") == 1 && near(postValue(v, "doffocus"), 800.0) && near(postValue(v, "dofstop"), 2.0) && near(postValue(v, "dofmm"), 35.0),
                  "the far blur is a lens focused at dof_blur_far_distance with f/(0.4 / dof_blur_amount)");
        }

        const int env = find(w, {"Main", "Env"});
        const std::string* e = prop(w, env, "environment");
        const std::string* c = prop(w, env, "camera_attributes");
        check(env >= 0 && w.placements[static_cast<usize>(env)].className == "WorldEnvironment", "the node is placed as a WorldEnvironment");
        check(e && e->find("Object(Environment") != std::string::npos && e->find("\"tonemap_mode\": 4") != std::string::npos &&
                  e->find("Object(Sky") != std::string::npos && e->find("Object(ProceduralSkyMaterial") != std::string::npos,
              "its Environment, Sky and sky material are inline objects for Aver.Fabric");
        check(c && c->find("Object(CameraAttributesPractical") != std::string::npos && c->find("\"exposure_multiplier\": 0.5") != std::string::npos, "and its camera attributes");
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
        const auto rated = [&](const char* prop, const char* rating) { return has("\"property\":\"" + std::string(prop) + "\"", std::string("\"rating\":\"") + rating + "\"", "Environment") ||
                                                                             has("\"property\":\"" + std::string(prop) + "\"", std::string("\"rating\":\"") + rating + "\"", "CameraAttributes"); };
        check(rated("tonemap_mode", "converted"), "tonemap_mode is converted");
        check(rated("adjustment_enabled", "converted"), "the adjustments are converted");
        check(rated("glow_enabled", "approximated") && rated("fog_mode", "approximated") && rated("fog_height", "approximated"), "glow, depth fog and height fog are approximated");
        check(rated("sky_material", "approximated"), "the sky material is approximated");
        check(rated("exposure_multiplier", "converted") && rated("dof_blur_far_enabled", "approximated"), "the camera attributes are rated");
        // G4 R8: Aver's GI is the same job done another way, so they are approximated, and each says what covers it.
        check(has("\"property\":\"ssao_enabled\"", "\"rating\":\"approximated\"", "ray-traced sky occlusion") && has("\"property\":\"sdfgi_enabled\"", "\"rating\":\"approximated\"", "voxel"),
              "SSAO and SDFGI are approximated and the report says what Aver's GI covers instead");
        check(!has("\"property\":\"tonemap_white\"", "\"rating\":\"unsupported\"", "") && !has("\"property\":\"fog_depth_end\"", "\"rating\":\"unsupported\"", ""),
              "handled properties are not listed as unsupported");
    }

    AVER_INFO("-- a panorama sky --");
    {
        fmt::OcWorldData w;
        check(fmt::loadOcworld(content + "/Maps/panorama.ocworld", w, &err), "the panorama level loads: " + err);
        fmt::PanoramaImage img;
        fmt::SkyDomeFit fit;
        check(fmt::loadPanorama(src + "/sky.hdr", img, &err) && fmt::fitSkyDome(img, 1.5, fit), "the same panorama fitted directly: " + err);
        check(w.hasSky && !w.skyPhysical, "the panorama becomes the authored dome");
        check(near(w.skyZenith[2], fit.zenith[2], 1e-5) && near(w.skyHorizon[1], fit.horizon[1], 1e-5) && near(w.skyGroundAlbedo[0], fit.groundAlbedo[0], 1e-5),
              "with the zenith, horizon and ground the fit gives, scaled by energy_multiplier");
        check(near(w.skyDomeExponent, fit.domeExponent, 1e-5), "and the fitted blend exponent");
        check(w.skyZenith[2] > w.skyHorizon[2] && w.skyGroundAlbedo[0] < 0.5, "the zenith is brighter than the horizon and the floor darker than the sky");
        check(std::filesystem::exists(content + "/Textures/sky.hdr", ec), "the panorama file is kept under Textures/");
        const int env = find(w, {"Pano", "Env"});
        const std::string* e = prop(w, env, "environment");
        check(e && e->find("Object(PanoramaSkyMaterial") != std::string::npos && e->find("Resource(\"Textures/sky.hdr\")") != std::string::npos, "the node keeps the PanoramaSkyMaterial and its texture reference");
    }

    AVER_INFO("-- no environment, nothing written --");
    {
        const std::string lights = std::string(AVER_FABRIC_DATA) + "/LightsGame";
        const std::string out2 = (root / "lights").string();
        const int code2 = runCapture("\"" + exe.string() + "\" fabric \"" + lights + "\" \"" + out2 + "\" --scenes-only", log);
        check(code2 == 0, "a project without a WorldEnvironment converts");
        fmt::ProjectDesc pd;
        check(fmt::loadOcproject(out2 + "/LightsGame/LightsGame.ocproject", pd, &err), "its project loads");
        check(pd.postTonemap < 0 && pd.postTonemapWhite < 0.0f && pd.postAdjustBrightness < 0.0f && pd.postAdjustContrast < 0.0f &&
                  pd.postAdjustSaturation < 0.0f && pd.postAutoExposure < 0 && pd.postBloom < 0.0f && pd.postExposure < 0.0f,
              "no tonemap, white, adjustments, eye-adaptation or bloom key is written");
        fmt::OcWorldData w;
        check(fmt::loadOcworld(out2 + "/LightsGame/Content/Maps/main.ocworld", w, &err) && w.postVolumes.empty(), "and its level has no post volume");
        const std::string text = slurp(out2 + "/LightsGame/LightsGame.ocproject");
        check(text.find("RENDER.TONEMAP") == std::string::npos && text.find("RENDER.ADJUST") == std::string::npos, "the project text names none of the new keys");
    }

    AVER_INFO("FabricEnvTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
