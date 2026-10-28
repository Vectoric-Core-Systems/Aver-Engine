// FabricCameraTest -- G4 R4 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/CameraGame, then the files it wrote read back with the
// engine's own loaders. Camera projection, keep_aspect, offsets and cull masks, render layers and visibility ranges stay on the placements as Godot's own
// properties for Aver.Fabric to push; the first camera is made current when none says so; every key is rated as docs/plans/fabric/G4_RENDER.md R4 lists.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
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

static bool hasProp(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return false;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props" && c.find(key)) return true;
    return false;
}

int main() {
    AVER_INFO("FabricCameraTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/CameraGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-camera-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/CameraGame/Content";
    std::string err;
    fmt::OcWorldData w, w2;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "main loads: " + err);
    check(fmt::loadOcworld(content + "/Maps/nocurrent.ocworld", w2, &err), "nocurrent loads: " + err);

    AVER_INFO("-- cameras keep Godot's properties --");
    {
        const int o = find(w, {"Main", "Ortho"}), f = find(w, {"Main", "Frust"}), p = find(w, {"Main", "Persp"});
        check(o >= 0 && w.placements[static_cast<usize>(o)].className == "Camera3D", "Ortho: a Camera3D");
        check(propIs(w, o, "projection", "1") && propIs(w, o, "size", "12.0") && propIs(w, o, "keep_aspect", "0") && propIs(w, o, "near", "0.1") &&
                  propIs(w, o, "far", "200.0") && propIs(w, o, "cull_mask", "6") && propIs(w, o, "h_offset", "0.5") && propIs(w, o, "v_offset", "-0.25"),
              "its projection, size, keep_aspect, clip planes, cull mask and offsets are Props, in Godot's own units");
        check(propIs(w, f, "projection", "2") && propIs(w, f, "frustum_offset", "Vector2(0.3, -0.1)") && propIs(w, f, "size", "2.0"), "Frust: a frustum with its offset");
        check(propIs(w, p, "fov", "90.0") && propIs(w, p, "keep_aspect", "0"), "Persp: a horizontal 90 degree field of view");
        check(!hasProp(w, p, "current") && !hasProp(w, f, "current"), "cameras that are not current stay so when another one is");
        check(w.hasCamera, "a CAMERA record is written");
    }

    AVER_INFO("-- the first camera is current when none is marked --");
    {
        const int a = find(w2, {"Second", "First"}), b = find(w2, {"Second", "Later"});
        check(a >= 0 && b >= 0, "both cameras are placed");
        check(propIs(w2, a, "current", "true"), "the first camera gets current = true");
        check(!hasProp(w2, b, "current"), "the other does not");
        check(w2.hasCamera && std::fabs(w2.camX + 400.0) < 1e-3 && std::fabs(w2.camY - 300.0) < 1e-3, "and the CAMERA record is at the first camera (Godot (3, 1, 4) m is (-400, 300, 100) cm)");
    }

    AVER_INFO("-- layers, visibility ranges and light masks --");
    {
        const int n = find(w, {"Main", "Near"}), pl = find(w, {"Main", "Plain"}), lamp = find(w, {"Main", "Lamp"});
        check(propIs(w, n, "layers", "2") && propIs(w, n, "visibility_range_begin", "5.0") && propIs(w, n, "visibility_range_begin_margin", "1.0") &&
                  propIs(w, n, "visibility_range_end", "40.0") && propIs(w, n, "visibility_range_end_margin", "2.0") && propIs(w, n, "visibility_range_fade_mode", "1"),
              "Near: layers and the whole visibility range are Props");
        check(propIs(w, pl, "visibility_range_end", "100.0") && !hasProp(w, pl, "layers"), "Plain: only the end bound; no layers stated, so Aver.Fabric's default (layer 1) applies");
        check(propIs(w, lamp, "layers", "4") && propIs(w, lamp, "light_cull_mask", "2") && propIs(w, lamp, "shadow_caster_mask", "4"), "Lamp: its masks are Props");
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
            return has(std::string("\"property\":\"") + prop + "\"", std::string("\"rating\":\"") + rating + "\"", node);
        };
        check(rated("Ortho", "projection", "approximated"), "an orthographic projection is approximated (rasteriser only)");
        check(rated("Frust", "projection", "approximated"), "a frustum projection is approximated (sky and fog rays)");
        check(rated("Persp", "fov", "converted") && rated("Persp", "keep_aspect", "converted"), "fov and keep_aspect are converted");
        check(rated("Ortho", "size", "converted") && rated("Frust", "frustum_offset", "converted") && rated("Ortho", "h_offset", "converted") &&
                  rated("Ortho", "v_offset", "converted") && rated("Ortho", "cull_mask", "converted") && rated("Ortho", "current", "converted"),
              "size, frustum_offset, h/v offset, cull_mask and current are converted");
        check(rated("Frust", "doppler_tracking", "unsupported"), "doppler tracking is unsupported");
        check(rated("Near", "layers", "converted") && rated("Near", "visibility_range_begin", "converted") && rated("Near", "visibility_range_end_margin", "converted"),
              "layers and the visibility range are converted");
        check(rated("Near", "visibility_range_fade_mode", "approximated"), "a fade mode other than disabled is approximated (hard cut)");
        check(rated("Lamp", "light_cull_mask", "approximated") && rated("Lamp", "shadow_caster_mask", "approximated"), "the light masks are approximated (raster local lights only)");
        check(has("\"property\":\"current\"", "\"rating\":\"converted\"", "no camera in this scene is marked current"), "the made-current camera is reported");
    }

    AVER_INFO("FabricCameraTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
