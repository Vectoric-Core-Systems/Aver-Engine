// The editor's material-name resolution: MaterialResolve.hpp, sandbox/src's own copy of the
// precedence GameContent::materialForSurface documents for the runtime (Binaries\Materials wins
// over Content\Materials, which wins over a bare content-relative path).
//
// WHY THIS TEST EXISTS. loadProjectMaterials() used to scan Content\Materials only, so a project
// whose .ocmat files live where the docs say BUILT OUTPUT belongs -- Binaries\Materials -- populated
// an EMPTY surfaceMaterials_ and every mesh fell back to the placeholder. The per-name resolver
// materialForSurface() already tried Binaries first; the PROJECT-WIDE scan that decides which names
// exist at all did not walk that directory, so a name living only there was never even attempted.
//
// HEADER-ONLY AND FILESYSTEM-ONLY, matching LevelListTest's own precedent immediately beside this
// file: the PICKER and the parsed pbr::MaterialDesc it eventually produces need Aver.Pbr, an Engine
// and a MaterialLibrary; the part that decides WHICH FILE WINS needs none of that, so it is what
// this test drives directly, off real files in a temp directory.
#include "MaterialResolve.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
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

static void touch(const std::filesystem::path& p, const char* text = "OCMAT 1\nNAME dummy\n") {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << text;
}

int main() {
    AVER_INFO("MaterialResolveTest");

    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aver_materialresolve_test";
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path content   = root / "Content";
    const std::filesystem::path binaries  = root / "Binaries";
    const std::string contentDir  = content.string();
    const std::string binariesDir = binaries.string();

    // ---- resolveMaterialPath: the four shapes a surface name can be in -----------------------
    AVER_INFO("-- single-name resolution --");

    check(editor::resolveMaterialPath(binariesDir, contentDir, "M_Nowhere").empty(),
          "a name present in neither directory resolves to nothing");

    touch(content / "Materials" / "M_ContentOnly.ocmat");
    {
        const std::string got = editor::resolveMaterialPath(binariesDir, contentDir, "M_ContentOnly");
        check(got == (content / "Materials" / "M_ContentOnly.ocmat").string(),
              "a name present only under Content\\Materials resolves there");
    }

    touch(binaries / "Materials" / "M_BinariesOnly.ocmat");
    {
        const std::string got = editor::resolveMaterialPath(binariesDir, contentDir, "M_BinariesOnly");
        check(got == (binaries / "Materials" / "M_BinariesOnly.ocmat").string(),
              "a name present only under Binaries\\Materials resolves there -- THE BUG THIS FIXES: "
              "before, the per-project scan never even looked here");
    }

    // Present under BOTH: the documented rule is the BUILT one wins.
    touch(content / "Materials" / "M_Both.ocmat", "OCMAT 1\nNAME hand-authored\n");
    touch(binaries / "Materials" / "M_Both.ocmat", "OCMAT 1\nNAME built\n");
    {
        const std::string got = editor::resolveMaterialPath(binariesDir, contentDir, "M_Both");
        check(got == (binaries / "Materials" / "M_Both.ocmat").string(),
              "a name present under BOTH resolves to the Binaries (built) one, not the Content one");
    }

    // The third candidate: a bare content-relative path, tried only once the first two miss.
    touch(content / "M_ContentRelative.ocmat");
    {
        const std::string got = editor::resolveMaterialPath(binariesDir, contentDir, "M_ContentRelative.ocmat");
        check(got == (content / "M_ContentRelative.ocmat").string(),
              "with no Materials-folder candidate, the bare content-relative path still resolves");
    }

    check(editor::resolveMaterialPath(binariesDir, contentDir, "").empty(), "an empty name resolves to nothing");
    check(editor::resolveMaterialPath(std::string(), std::string(), "M_ContentOnly").empty(),
          "empty roots (an unopened project) resolve to nothing rather than throwing");

    // ---- projectMaterialStems: the project-wide enumeration behind loadProjectMaterials() ----
    AVER_INFO("-- project-wide enumeration --");
    {
        std::vector<std::string> stems = editor::projectMaterialStems(binariesDir, contentDir);
        std::sort(stems.begin(), stems.end());
        check(std::find(stems.begin(), stems.end(), "M_ContentOnly") != stems.end(),
              "the enumeration includes a Content-only name");
        check(std::find(stems.begin(), stems.end(), "M_BinariesOnly") != stems.end(),
              "the enumeration includes a Binaries-only name -- the actual EMPTY-surfaceMaterials_ bug: "
              "loadProjectMaterials() used to build its stem list from Content\\Materials alone, so this "
              "name was never on the list to resolve in the first place");
        const usize bothCount =
            static_cast<usize>(std::count(stems.begin(), stems.end(), "M_Both"));
        check(bothCount == 1, "a name present under both directories is listed exactly once, not twice");
        // M_ContentRelative.ocmat sits directly under Content, not Content\Materials -- the
        // enumeration is deliberately non-recursive and folder-scoped, matching the runtime's own
        // loadProjectMaterials(), so it must NOT appear here even though resolveMaterialPath() can
        // still resolve it as a NAMED candidate.
        check(std::find(stems.begin(), stems.end(), "M_ContentRelative") == stems.end(),
              "a bare content-relative file is not picked up by the project-wide scan (non-recursive, "
              "Materials-folder only) even though a direct name lookup can still resolve it");
    }

    check(editor::projectMaterialStems(std::string(), std::string()).empty(),
          "an unopened project (empty roots) enumerates no materials");

    AVER_INFO("{}/{} checks passed", g_checks - g_failures, g_checks);
    if (g_failures) AVER_ERROR("FAILED  {} check(s) failed", g_failures);
    return g_failures ? 1 : 0;
}
