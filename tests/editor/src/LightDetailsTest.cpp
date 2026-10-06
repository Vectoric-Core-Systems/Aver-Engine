// The Light section of the Details panel (sandbox/src/LightDetails.hpp), its pure half: which files a
// picker lists, the id a CLight stores for one, the cone conversions, and the defaults a freshly added
// light gets. The ImGui half needs a window; the half under test is where a wrong id (a light that
// silently loses its profile on reload) or a wrong cosine would hide.
#include "LightDetails.hpp"
#include "LightLevelIo.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void touch(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p);
    f << "x";
}

int main() {
    AVER_INFO("LightDetailsTest");

    AVER_INFO("-- ids --");
    check(editor::lightAssetId("Lights/downlight.ies") == fnv1a64(std::string_view("Lights/downlight.ies")),
          "an asset id is fnv1a64 of the content-relative path");
    check(editor::lightAssetId("Lights\\downlight.ies") == editor::lightAssetId("Lights/downlight.ies"),
          "backslashes hash as forward slashes, so a Windows path and its project spelling agree");

    AVER_INFO("-- the listing --");
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aver_lightdetails_test";
    std::filesystem::remove_all(root, ec);
    touch(root / "Lights" / "b_spot.IES");
    touch(root / "Lights" / "a_down.ies");
    touch(root / "Lights" / "Deep" / "c.ies");
    touch(root / "Textures" / "gobo.png");
    touch(root / "Textures" / "leaves.JPG");
    touch(root / "Textures" / "notes.txt");
    touch(root / "Meshes" / "cube.ocmesh");

    const auto ies = editor::listLightAssets(root.string(), true);
    check(ies.size() == 3, "three profiles found, recursively, case-insensitively");
    if (ies.size() == 3) {
        check(ies[0].label == "Lights/Deep/c.ies" && ies[1].label == "Lights/a_down.ies" &&
                  ies[2].label == "Lights/b_spot.IES", "sorted by content-relative path, forward slashes");
        check(ies[1].id == editor::lightAssetId("Lights/a_down.ies"), "each carries its id");
    }
    const auto tex = editor::listLightAssets(root.string(), false);
    check(tex.size() == 2, "cookies list only images");
    check(editor::listLightAssets("", true).empty() && editor::listLightAssets((root / "nope").string(), true).empty(),
          "an empty or missing content folder lists nothing");
    std::filesystem::remove_all(root, ec);

    AVER_INFO("-- cones --");
    check(std::fabs(editor::lightConeDegrees(editor::lightConeCos(35.0f)) - 35.0f) < 1e-3f, "degrees round trip");
    check(std::fabs(editor::lightConeCos(0.0f) - 1.0f) < 1e-6f, "0 degrees is cosine 1");
    check(editor::lightConeCos(120.0f) > 0.0f, "angles clamp below 90 so the cone cosine stays positive");
    check(editor::lightConeDegrees(2.0f) == 0.0f, "an out-of-range cosine does not produce NaN");

    AVER_INFO("-- new lights --");
    const scene::CLight pt = editor::makeNewLight(scene::kLightPoint);
    check(pt.kind == scene::kLightPoint && pt.intensityLux > 0.0f && pt.intensityLux < 20000.0f,
          "a point light is room-sized, not the sun-sized struct default");
    check(pt.colour[0] == 1.0f && pt.colour[1] == 1.0f && pt.colour[2] == 1.0f, "white");
    check(pt.iesProfile == 0 && pt.cookie == 0 && pt.flags == 0, "no profile, no cookie, shadows on");
    const scene::CLight sp = editor::makeNewLight(scene::kLightSpot);
    check(sp.innerCos > sp.outerCos && sp.outerCos > 0.0f, "a spot has an inner cone inside its outer");
    const scene::CLight rc = editor::makeNewLight(scene::kLightRect);
    check(rc.widthCm > 0.0f && rc.heightCm > 0.0f && rc.kind == scene::kLightRect, "a rectangle has a size");

    AVER_INFO("-- level records --");
    {
        fmt::OcLight r;
        r.name = "Lamp";
        r.kind = fmt::OcLightKind::Spot;
        r.x = 120; r.y = -40; r.z = 260; r.yaw = 35; r.pitch = -50; r.roll = 0;
        r.colour[0] = 1.0; r.colour[1] = 0.5; r.colour[2] = 0.25;
        r.intensityCd = 4000; r.rangeCm = 900; r.innerDeg = 12; r.outerDeg = 40; r.radiusCm = 3;
        r.ies = "Lights/a_down.ies";
        r.cookie = "Textures/gobo.png";
        r.castShadows = false;
        r.iesPeak = true;

        const scene::CLight c = editor::lightFromRecord(r);
        check(c.kind == scene::kLightSpot && c.intensityLux == 4000.0f && c.rangeCm == 900.0f, "record -> CLight");
        check(c.iesProfile == static_cast<i64>(editor::lightAssetId("Lights/a_down.ies")) &&
                  c.cookie == static_cast<i64>(editor::lightAssetId("Textures/gobo.png")),
              "asset paths become ids");
        check((c.flags & scene::kLightNoShadows) != 0 && (c.flags & scene::kLightIesPeak) != 0, "flags");

        editor::LightAssetPaths paths;
        paths.byId[editor::lightAssetId("Lights/a_down.ies")] = "Lights/a_down.ies";
        paths.byId[editor::lightAssetId("Textures/gobo.png")] = "Textures/gobo.png";
        const fmt::OcLight back = editor::recordFromLight(c, editor::transformFromRecord(r), "Lamp", paths);
        check(std::fabs(back.x - 120) < 1e-3 && std::fabs(back.y + 40) < 1e-3 && std::fabs(back.z - 260) < 1e-3, "position survives");
        check(std::fabs(back.yaw - 35) < 1e-2 && std::fabs(back.pitch + 50) < 1e-2, "yaw and pitch survive the quaternion");
        check(std::fabs(back.innerDeg - 12) < 1e-3 && std::fabs(back.outerDeg - 40) < 1e-3, "cone angles survive the cosines");
        check(back.ies == r.ies && back.cookie == r.cookie && !back.castShadows && back.iesPeak && back.kind == r.kind,
              "assets, flags and kind survive");
        check(editor::recordFromLight(c, Transform{}, "x", editor::LightAssetPaths{}).ies.empty(),
              "an id with no file in the project saves as no profile rather than a bogus path");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
