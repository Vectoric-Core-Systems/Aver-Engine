// The level enumeration behind File > Open Level and the Content Browser's level activation.
//
// WHY IT HAS A TEST WHEN THE PICKER ITSELF CANNOT. The modal needs ImGui and a window; the LIST does
// not, and the list is where the mistakes live -- a missed extension, a walk that descends into a
// project's streamed-chunk directory, an order that shuffles between runs. Sandbox is
// add_executable-only with no library half, so this compiles the header directly, the same
// arrangement EditorEulerTest and GraphEditorGeometryTest already use.
#include "LevelList.hpp"

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

static void touch(const std::filesystem::path& p, const char* text = "OCWORLD 1\n") {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p);
    f << text;
}

int main() {
    AVER_INFO("LevelListTest");

    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aver_levellist_test";
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path content = root / "Content";

    // ---- what isLevelPath must accept and refuse ------------------------------------------
    AVER_INFO("-- the extension test --");
    check(editor::isLevelPath("Maps/Default.ocmap"), ".ocmap is a level");
    check(editor::isLevelPath("Maps/Arena.ocworld"), ".ocworld is a level too");
    // THE CASE THAT MATTERS: a picker offering only .ocworld would hide every legacy map, and one
    // offering only .ocmap would hide everything Save Level As has ever written.
    check(editor::isLevelPath("A.OCWORLD") && editor::isLevelPath("B.OcMap"),
          "and the test is case-insensitive, because Windows paths are");
    check(!editor::isLevelPath("Meshes/cube.ocmesh"), "a mesh is not a level");
    check(!editor::isLevelPath("Materials/M_Crate.ocmat"), "nor is a material");
    check(!editor::isLevelPath("noextension"), "nor is a file with no extension at all");
    // A file named for its extension alone has no stem, and the picker labels each row with the
    // stem -- so accepting it would put a blank, unclickable-looking row in the list.
    check(!editor::isLevelPath(".ocmap"), "a bare extension with no stem is NOT a level");
    check(editor::isLevelPath("C:/p/Content/Maps/A.b.ocworld"),
          "and a stem containing a dot still resolves on the LAST one");

    // ---- an empty or absent content root is empty, not a crash ------------------------------
    AVER_INFO("-- nothing to list --");
    check(editor::listLevels("").empty(), "no content directory lists nothing");
    check(editor::listLevels((root / "does-not-exist").string()).empty(),
          "a content directory that is not there lists nothing");

    // ---- the walk ----------------------------------------------------------------------------
    AVER_INFO("-- the walk --");
    touch(content / "Maps" / "Default.ocmap");
    touch(content / "Maps" / "Arena.ocworld");
    touch(content / "Maps" / "Sub" / "Nested.ocworld");
    touch(content / "Levels" / "Elsewhere.ocworld");
    touch(content / "Meshes" / "cube.ocmesh");
    touch(content / "Materials" / "M_Crate.ocmat");
    // The two subtrees the walk must refuse. Chunks/ is the one that matters in practice: a project
    // that has ever streamed a scatter has tens of thousands of files under it, and a level file
    // saved in there is derived data, not something to offer.
    touch(content / "Chunks" / "Canopy" / "stale.ocworld");
    touch(content / "DerivedDataCache" / "cached.ocmap");

    const std::vector<editor::LevelEntry> levels = editor::listLevels(content.string());
    check(levels.size() == 4, "four authored levels found, and only four (got " +
          std::to_string(levels.size()) + ")");

    bool sawChunk = false, sawDdc = false;
    for (const editor::LevelEntry& l : levels) {
        if (l.relPath.find("Chunks/") == 0) sawChunk = true;
        if (l.relPath.find("DerivedDataCache/") == 0) sawDdc = true;
    }
    check(!sawChunk, "the streamed-chunk directory is NOT descended into");
    check(!sawDdc, "nor is the derived-data cache");

    if (levels.size() == 4) {
        // Sorted by content-relative path, so the order is the same on every run and on every
        // machine -- directory_iterator's own order is not specified to be anything.
        check(levels[0].relPath == "Levels/Elsewhere.ocworld" &&
              levels[1].relPath == "Maps/Arena.ocworld" &&
              levels[2].relPath == "Maps/Default.ocmap" &&
              levels[3].relPath == "Maps/Sub/Nested.ocworld",
              "sorted by content-relative path, stably");
        check(levels[3].name == "Nested", "the display name is the stem, not the path");
        // FORWARD SLASHES, on Windows, deliberately: this string is shown to a person and compared
        // against the project's STARTMAP, which is written with forward slashes.
        for (const editor::LevelEntry& l : levels)
            check(l.relPath.find('\\') == std::string::npos,
                  "no backslash survives into " + l.relPath);
        // And the absolute path is what loadLevel is actually handed, so it has to name a real file.
        for (const editor::LevelEntry& l : levels)
            check(std::filesystem::exists(l.absPath, ec), "absPath exists for " + l.relPath);
    }

    // ---- a level nested deeply enough to prove the walk recurses ------------------------------
    touch(content / "A" / "B" / "C" / "Deep.ocworld");
    const std::vector<editor::LevelEntry> deep = editor::listLevels(content.string());
    bool foundDeep = false;
    for (const editor::LevelEntry& l : deep) if (l.relPath == "A/B/C/Deep.ocworld") foundDeep = true;
    check(foundDeep, "the walk recurses past one level of nesting");

    std::filesystem::remove_all(root, ec);
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return g_failures;
}
