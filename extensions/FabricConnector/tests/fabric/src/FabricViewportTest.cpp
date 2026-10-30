// FabricViewportTest -- G4 R5 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/ViewportGame, then the files it wrote read back with the
// engine's own loaders. A SubViewport keeps Godot's properties and gains the key the engine and the baked materials know it by; a ViewportTexture in a material's
// albedo slot becomes the '@rt:viewport.<key>' texture reference in the .ocmat (and the view counts one consumer per material); a camera below a SubViewport is the
// view's camera and never the level's; a SubViewportContainer keeps its layout and stretch values; every key is rated as docs/plans/fabric/G4_RENDER.md R5 lists.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"
#if defined(AVER_FABRIC_TEST_OCMAT)
#include "aver/formats/OcMat.hpp"
#endif

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

static const std::string* propOf(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props")
            if (const std::string* v = c.find(key)) return v;
    return nullptr;
}

static bool propIs(const fmt::OcWorldData& w, int idx, const char* key, const char* text) {
    const std::string* v = propOf(w, idx, key);
    return v && *v == text;
}

static bool hasProp(const fmt::OcWorldData& w, int idx, const char* key) { return propOf(w, idx, key) != nullptr; }

// The .ocmat a MeshInstance3D placement uses (its material_override Resource("...")), as a path under Content/.
static std::string materialOf(const fmt::OcWorldData& w, int idx) {
    const std::string* v = propOf(w, idx, "material_override");
    if (!v) return {};
    const size_t a = v->find('"'), b = v->rfind('"');
    return a == std::string::npos || b <= a ? std::string{} : v->substr(a + 1, b - a - 1);
}

int main() {
    AVER_INFO("FabricViewportTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/ViewportGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-viewport-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/ViewportGame/Content";
    std::string err;
    fmt::OcWorldData w, only;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "main loads: " + err);
    check(fmt::loadOcworld(content + "/Maps/onlyview.ocworld", only, &err), "onlyview loads: " + err);

    AVER_INFO("-- the SubViewport keeps Godot's properties and gains its key --");
    const int sv = find(w, {"Main", "SubViewport"});
    {
        check(sv >= 0 && w.placements[static_cast<usize>(sv)].className == "SubViewport", "a SubViewport is placed as an Aver.Fabric SubViewport, not a carrier");
        check(propIs(w, sv, "size", "Vector2i(320, 180)") && propIs(w, sv, "render_target_update_mode", "4") && propIs(w, sv, "render_target_clear_mode", "2") &&
                  propIs(w, sv, "transparent_bg", "true") && propIs(w, sv, "own_world_3d", "true") && propIs(w, sv, "msaa_3d", "2") && propIs(w, sv, "scaling_3d_scale", "0.5"),
              "size, update mode, clear mode, transparent background, own world, MSAA and the 3D scale are Props in Godot's own terms");
        check(propIs(w, sv, "_viewport_key", "\"main:Main/SubViewport\""), "the key is the level's name and the node's path from the level's root");
        check(propIs(w, sv, "_material_consumers", "2"), "two materials show the view (the unshaded screen and the shaded one); the two that failed to resolve do not count");
        const int map = find(w, {"Main", "Hud", "MapView"});
        check(propIs(w, map, "_viewport_key", "\"main:Main/Hud/MapView\"") && !hasProp(w, map, "_material_consumers"), "a nested view has its own key and no material consumers");
    }

    AVER_INFO("-- cameras below a SubViewport are the view's --");
    {
        const int main = find(w, {"Main", "MainCamera"}), sec = find(w, {"Main", "SubViewport", "SecurityCam"}), spare = find(w, {"Main", "SubViewport", "Spare"});
        const int map = find(w, {"Main", "Hud", "MapView", "MapCam"});
        check(main >= 0 && propIs(w, main, "current", "true"), "the level's own camera is still current");
        check(sec >= 0 && !hasProp(w, sec, "current") && propIs(w, sec, "_viewport_current", "true") && propIs(w, sec, "fov", "40.0"),
              "the view's camera does not say 'current' (that would take the window's view); it carries the private _viewport_current instead");
        check(spare >= 0 && !hasProp(w, spare, "current") && !hasProp(w, spare, "_viewport_current"), "a camera that was not current stays so");
        check(map >= 0 && propIs(w, map, "projection", "1") && propIs(w, map, "size", "40.0"), "an orthographic camera in a nested view keeps its lens");
        check(w.hasCamera && std::fabs(w.camX + 600.0) < 1e-3 && std::fabs(w.camY) < 1e-3 && std::fabs(w.camZ - 100.0) < 1e-3,
              "the CAMERA record is the level's camera ((0, 1, 6) m is (-600, 0, 100) cm), not the one inside the view");
        check(only.placements.size() == 3 && !only.hasCamera, "a level whose only camera is inside a SubViewport has no CAMERA record");
        const int vc = find(only, {"OnlyView", "View", "ViewCam"});
        check(vc >= 0 && !hasProp(only, vc, "current") && propIs(only, vc, "_viewport_current", "true"), "and that camera is not made current by the first-camera rule");
    }

    AVER_INFO("-- the container --");
    {
        const int hud = find(w, {"Main", "Hud"});
        check(hud >= 0 && w.placements[static_cast<usize>(hud)].className == "SubViewportContainer", "a SubViewportContainer is placed as one");
        check(propIs(w, hud, "stretch", "true") && propIs(w, hud, "stretch_shrink", "2") && propIs(w, hud, "anchor_right", "1.0") && propIs(w, hud, "anchor_bottom", "1.0"),
              "stretch, stretch_shrink and the anchors are Props");
    }

#if defined(AVER_FABRIC_TEST_OCMAT)
    AVER_INFO("-- the materials --");
    {
        const std::string screenFile = materialOf(w, find(w, {"Main", "Screen"}));
        const std::string shadedFile = materialOf(w, find(w, {"Main", "Shaded"}));
        const std::string lostFile = materialOf(w, find(w, {"Main", "Lost"}));
        const std::string wrongFile = materialOf(w, find(w, {"Main", "Wrong"}));
        check(!screenFile.empty() && !shadedFile.empty() && screenFile != shadedFile, "the screen and the shaded box have their own materials: " + screenFile + ", " + shadedFile);
        const char* want = "@rt:viewport.main:Main/SubViewport";
        pbr::MaterialDesc screen, shaded, lost, wrong;
        fmt::OcMatExtras ex;
        check(fmt::loadOcmat(content + "/" + screenFile, screen, &ex, &err), "the screen material loads: " + err);
        check(screen.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].path == want, "its base colour is the view's texture reference");
        check(screen.textures[static_cast<u32>(pbr::TextureSlot::Emissive)].path == want && screen.emissiveFactor[0] == 1.0f,
              "unshaded: the picture is also its emissive map, at factor 1");
        check(fmt::loadOcmat(content + "/" + shadedFile, shaded, &ex, &err), "the shaded material loads: " + err);
        check(shaded.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].path == want && shaded.textures[static_cast<u32>(pbr::TextureSlot::Emissive)].empty(),
              "a shaded one shows it as base colour only");
        check(!screen.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].path.empty() && screen.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].id == 0,
              "the reference is a path, not a content id");
        check(fmt::loadOcmat(content + "/" + lostFile, lost, &ex, &err) && lost.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].empty(),
              "a ViewportTexture whose path names nothing leaves the slot empty");
        check(fmt::loadOcmat(content + "/" + wrongFile, wrong, &ex, &err) && wrong.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].empty(),
              "and so does one that names a Camera3D");
        const std::string text = fmt::writeOcmat(screen, &ex);
        pbr::MaterialDesc back;
        check(text.find("@rt:viewport.main:Main/SubViewport") != std::string::npos, "a rewrite keeps the reference");
        (void)back;
    }
#endif

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
        check(has("\"type\":\"SubViewport\"", "\"rating\":\"approximated\"", "cheap raster configuration"), "the SubViewport node is approximated, and says what a view is");
        check(rated("SubViewport", "size", "converted") && rated("SubViewport", "render_target_update_mode", "converted") && rated("SubViewport", "render_target_clear_mode", "converted") &&
                  rated("SubViewport", "transparent_bg", "converted") && rated("SubViewport", "scaling_3d_scale", "converted"),
              "size, update mode, clear mode, transparent background and the 3D scale are converted");
        check(rated("SubViewport", "own_world_3d", "approximated") && rated("SubViewport", "msaa_3d", "approximated"), "own_world_3d and msaa_3d are approximated (one world, one sample)");
        check(rated("SubViewport", "gui_embed_subwindows", "unsupported") && rated("SubViewport", "handle_input_locally", "unsupported") && rated("SubViewport", "audio_listener_enable_3d", "unsupported"),
              "GUI, input and audio-listener options are unsupported");
        check(rated("Hud", "stretch", "converted") && rated("Hud", "stretch_shrink", "converted") && rated("Hud", "mouse_target", "unsupported"), "the container's stretch is converted, mouse_target is not");
        check(has("\"type\":\"SubViewportContainer\"", "\"rating\":\"approximated\"", "anchors and container layout"), "the container node is approximated");
        check(rated("SecurityCam", "current", "converted") && has("SecurityCam", "\"property\":\"current\"", "never the window's"), "a view camera's 'current' is converted and says it is not the window's");
        check(has("\"node\":\"Screen\"", "\"property\":\"albedo_texture\"", "@rt:viewport.main:Main/SubViewport") && has("\"node\":\"Screen\"", "\"rating\":\"converted\"", "ViewportTexture"),
              "the ViewportTexture in a material is reported converted, naming the reference");
        check(has("\"node\":\"Screen\"", "\"property\":\"shading_mode\"", "\"rating\":\"approximated\""), "unshaded is approximated (the picture is also the emissive map)");
        check(has("\"node\":\"Lost\"", "\"rating\":\"unsupported\"", "does not exist"), "a path that names no node is reported unsupported");
        check(has("\"node\":\"Wrong\"", "\"rating\":\"unsupported\"", "not a SubViewport"), "a path that names another class is reported unsupported");
    }

    AVER_INFO("FabricViewportTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
