// FabricLightsTest -- G4 R2 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/LightsGame, then the files it wrote read back with
// the engine's own loaders. The sun is the level's SUN record at the documented calibration (Godot energy 1 = pi engine units = 104719.755 lux),
// the omni and spot lights keep Godot's light properties on their placements for Aver.Fabric to push, only the first DirectionalLight3D is the sun,
// and every stored light property is rated in the report (converted / approximated / unsupported) as docs/plans/fabric/G4_RENDER.md R2 lists.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/GodotLight.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
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

static bool near(double a, double b, double rel = 1e-5) { return std::fabs(a - b) <= rel * std::max(1.0, std::fabs(b)); }

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

static bool propIs(const fmt::OcWorldData& w, int idx, const char* key, const char* text) {
    if (idx < 0) return false;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props")
            if (const std::string* v = c.find(key)) return *v == text;
    return false;
}

int main() {
    AVER_INFO("FabricLightsTest");
    // The calibration is one number in three places (this header, Aver.Fabric's Light3D, the renderer's unit); the C# test and RasterLightsTest hold the others.
    check(near(fmt::godotlight::kLuxPerEnergy, 104719.755, 1e-8) && near(fmt::godotlight::kCandelaPerEnergy, fmt::godotlight::kLuxPerEnergy, 1e-12),
          "Godot energy 1 is 104719.755 lux or candela");

    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/LightsGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-lights-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/LightsGame/Content";
    std::string err;
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);

    AVER_INFO("-- the sun --");
    {
        check(w.hasSun, "a SUN record is written");
        check(near(w.sunLux, 2.0 * 104719.755, 1e-6), "the first directional light, energy 2, is 209439.51 lux");
        check(near(w.sunAngularDeg, 0.5, 1e-9), "its angular distance rides along");
        check(w.sunColor[0] > 0.99 && w.sunColor[1] < 0.95, "its colour is linear");
        const int sun = find(w, {"Main", "Sun"}), moon = find(w, {"Main", "Moon"});
        check(sun >= 0 && moon >= 0 && w.placements[static_cast<usize>(moon)].className == "DirectionalLight3D",
              "both directional lights are placed; the second is inert at run time (Aver.Fabric makes the first the sun)");
        check(propIs(w, find(w, {"Main", "Moon"}), "light_energy", "0.1"), "and keeps its own energy");
    }

    AVER_INFO("-- omni and spot placements keep Godot's properties --");
    {
        const int lamp = find(w, {"Main", "Lamp"}), torch = find(w, {"Main", "Torch"});
        check(lamp >= 0 && w.placements[static_cast<usize>(lamp)].className == "OmniLight3D", "Lamp: an OmniLight3D");
        check(propIs(w, lamp, "light_energy", "3.0") && propIs(w, lamp, "omni_range", "6.0") && propIs(w, lamp, "omni_attenuation", "2.0") &&
                  propIs(w, lamp, "shadow_enabled", "true") && propIs(w, lamp, "light_specular", "0.25") && propIs(w, lamp, "light_size", "0.1") &&
                  propIs(w, lamp, "shadow_bias", "0.3") && propIs(w, lamp, "shadow_normal_bias", "4.0") && propIs(w, lamp, "distance_fade_enabled", "true"),
              "its energy, range, attenuation, shadow, specular, size, biases and distance fade are Props, in Godot's own units");
        check(torch >= 0 && w.placements[static_cast<usize>(torch)].className == "SpotLight3D" && propIs(w, torch, "spot_angle", "30.0") &&
                  propIs(w, torch, "spot_attenuation", "1.5") && propIs(w, torch, "spot_angle_attenuation", "0.5") && propIs(w, torch, "light_negative", "true"),
              "Torch: spot angle, both attenuations and light_negative");
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
        const auto rated = [&](const char* node, const char* prop, const char* rating) {
            return has(std::string("\"property\":\"") + prop + "\"", std::string("\"rating\":\"") + rating + "\"", std::string("\"") + node + "\"") ||
                   has(std::string("\"property\":\"") + prop + "\"", std::string("\"rating\":\"") + rating + "\"", node);
        };
        check(rated("Lamp", "light_energy", "converted"), "light_energy is converted");
        check(rated("Lamp", "omni_range", "converted") && rated("Lamp", "omni_attenuation", "converted") && rated("Lamp", "shadow_enabled", "converted"),
              "range, attenuation and the shadow switch are converted");
        check(rated("Lamp", "distance_fade_enabled", "converted"), "distance fade is converted");
        check(rated("Lamp", "light_specular", "approximated") && rated("Lamp", "light_size", "approximated") && rated("Lamp", "shadow_bias", "approximated") &&
                  rated("Lamp", "shadow_normal_bias", "approximated"),
              "specular, size and the shadow biases are approximated");
        check(rated("Lamp", "light_indirect_energy", "approximated"), "light_indirect_energy is approximated (no indirect light from local lights)");
        check(rated("Lamp", "light_cull_mask", "approximated"), "light_cull_mask is approximated (the raster local-light path honours it; G4 R4)");
        check(rated("Torch", "light_negative", "approximated") && rated("Torch", "spot_angle_attenuation", "converted") && rated("Torch", "spot_attenuation", "converted"),
              "light_negative is approximated; the two spot curves are converted");
        check(rated("Sun", "directional_shadow_max_distance", "approximated") && rated("Sun", "light_angular_distance", "converted"),
              "the sun's cascade settings are approximated and its angular distance converted");
        check(has("\"node\"", "\"rating\":\"approximated\"", "only the first DirectionalLight3D"), "the second directional light is reported as inert");
        check(!has("\"property\":\"light_energy\"", "\"rating\":\"unsupported\"", ""), "and no light property that is handled is listed as unsupported");
    }

    AVER_INFO("FabricLightsTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
