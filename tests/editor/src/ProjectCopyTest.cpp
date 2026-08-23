// copyProjectTree: the "Upgrade a Copy" branch of the upgrade prompt.
//
// This is the only path in the editor that DUPLICATES somebody's project, and it is the default
// button on a modal whose other option is destructive -- so it has to be the branch that is proven
// rather than the branch that looks obviously right. The guarantee under test is narrow and total:
// it never writes over anything that already exists.
#include "ProjectScaffold.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <filesystem>
#include <string>

using namespace aver;

namespace {
int gChecks = 0, gFailed = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (ok) { AVER_INFO("  ok    {}", what); return; }
    ++gFailed;
    AVER_ERROR("  FAIL  {}", what);
}

// A minimal but REAL project tree: a manifest, nested content, and a file deep enough to prove the
// copy is recursive rather than a directory-and-manifest shortcut.
std::string makeProject(const std::filesystem::path& root, const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(root / name / "Content" / "Maps", ec);
    const std::string manifest = (root / name / (name + ".ocproject")).string();
    writeFileText(manifest, "OCPROJECT 1\nNAME " + name + "\nCREATEDWITH 0.1.2\nCONTENT Content\n");
    writeFileText((root / name / "Content" / "Maps" / "Default.ocmap").string(), "OCMAP 1\nNAME deep\n");

    // A GENERATED .ocmat UNDER Binaries, holding the kind of record avermatc actually writes: an
    // ABSOLUTE {path:...} into THIS project's own content. That is the shape that used to survive a
    // copy unchanged and leave the copy reading the original's textures.
    std::filesystem::create_directories(root / name / "Binaries" / "Materials", ec);
    std::filesystem::create_directories(root / name / "Content" / "Textures", ec);
    writeFileText((root / name / "Content" / "Textures" / "T_Rock.png").string(), "not-a-real-png");
    const std::string baked = (root / name / "Content" / "Textures" / "T_Rock.png").string();
    writeFileText((root / name / "Binaries" / "Materials" / "M_Rock.ocmat").string(),
                  "OCMAT 1\nNAME M_Rock\nTEX baseColor {path:" + baked + "}\n"
                  "TEX shared {path:D:\\Shared\\Library\\T_Sky.png}\n");
    return manifest;
}
} // namespace

int main() {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "aver-projectcopy-test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    AVER_INFO("=== copying a project ===");
    const std::string src = makeProject(root, "Demo");
    std::string err;
    const std::string copy1 = editor::copyProjectTree(src, "0.2.0", &err);
    check(!copy1.empty(), "a project copies: " + err);
    check(std::filesystem::exists(copy1, ec), "the returned manifest path exists");
    check(std::filesystem::path(copy1).parent_path().filename().string() == "Demo (0.2.0)",
          "the copy is a sibling folder named for the engine version");
    check(std::filesystem::exists(std::filesystem::path(copy1).parent_path() /
                                  "Content" / "Maps" / "Default.ocmap", ec),
          "RECURSIVE: a file two directories deep came with it");
    check(std::filesystem::exists(src, ec), "the ORIGINAL is still there, untouched");

    AVER_INFO("=== baked texture paths under Binaries follow the copy ===");
    {
        // THE BUG THIS PROVES ABSENT: avermatc bakes a texture slot's path verbatim into the
        // generated .ocmat, and resolveAssetPath honours an ABSOLUTE one unchanged -- so before
        // copyProjectTree rewrote them, a copied project loaded its textures out of the ORIGINAL
        // project's folder. Editing the copy's textures changed nothing; deleting the original took
        // the copy's materials with it.
        std::string mat;
        const std::filesystem::path copiedMat =
            std::filesystem::path(copy1).parent_path() / "Binaries" / "Materials" / "M_Rock.ocmat";
        check(readFileText(copiedMat.string(), mat), "the copy has its generated .ocmat");

        const std::string oldAbs = (root / "Demo" / "Content" / "Textures" / "T_Rock.png").string();
        check(mat.find(oldAbs) == std::string::npos,
              "no record still names the ORIGINAL project's absolute path");

        // Rewritten RELATIVE to the content root rather than repointed at the copy's absolute path:
        // an absolute rewrite would fix this copy and break identically on the next move.
        check(mat.find("{path:Textures\\T_Rock.png}") != std::string::npos,
              "the baked path is now relative to the copy's own content root");

        // A path that was never under this project is somebody's deliberate shared asset, not a
        // stale reference, and rewriting it would break a working setup.
        check(mat.find("{path:D:\\Shared\\Library\\T_Sky.png}") != std::string::npos,
              "a path OUTSIDE the project is left exactly as it was");
    }

    AVER_INFO("=== a second copy must not overwrite the first ===");
    {
        std::string text;
        readFileText(copy1, text);
        const std::string copy2 = editor::copyProjectTree(src, "0.2.0", &err);
        check(!copy2.empty(), "a second copy succeeds: " + err);
        check(copy2 != copy1, "...at a DIFFERENT path");
        check(std::filesystem::path(copy2).parent_path().filename().string() == "Demo (0.2.0) 2",
              "...numbered rather than reusing the taken name");
        std::string after;
        readFileText(copy1, after);
        check(after == text, "THE FIRST COPY IS BYTE-IDENTICAL: nothing was written over it");
    }

    AVER_INFO("=== refusing rather than half-doing ===");
    {
        const std::string missing = (root / "NoSuchProject" / "NoSuchProject.ocproject").string();
        err.clear();
        const std::string out = editor::copyProjectTree(missing, "0.2.0", &err);
        check(out.empty(), "a manifest with no project folder copies nothing");
        check(!err.empty(), "...and says why");
    }

    std::filesystem::remove_all(root, ec);
    if (gFailed) { AVER_ERROR("=== {} of {} checks FAILED ===", gFailed, gChecks); return 1; }
    AVER_INFO("=== all {} project-copy checks passed ===", gChecks);
    return 0;
}
