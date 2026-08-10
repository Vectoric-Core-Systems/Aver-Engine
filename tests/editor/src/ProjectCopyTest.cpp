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
