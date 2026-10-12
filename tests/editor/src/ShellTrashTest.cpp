// moveToRecycleBin on Linux: the freedesktop.org Trash specification, checked against a trash
// directory of the test's own (XDG_DATA_HOME points into a temporary folder, so nothing the user owns
// is touched). Exit code = failure count.
//
// WHY THIS IS TESTED AT ALL. It is the content browser's Delete. A wrong answer here is a file gone
// from the project and from the trash, or a file "deleted" that is still where it was -- and the
// desktop's own trash view is the only thing that would ever show either.
#include "ShellIntegration.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace aver;
namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "aver-shelltrash-test";
    fs::remove_all(root, ec);
    const fs::path data = root / "data";
    const fs::path work = root / "work dir";   // a space, as project paths have
    fs::create_directories(data, ec);
    fs::create_directories(work, ec);
    setenv("XDG_DATA_HOME", data.c_str(), 1);

    const fs::path a = work / "Rock's Mesh.ocmesh";
    std::ofstream(a) << "one";
    check(editor::moveToRecycleBin(a.string()), "a file in a folder with a space and a quote in its name is trashed");
    check(!fs::exists(a, ec), "...and is gone from where it was");
    check(fs::exists(data / "Trash" / "files" / "Rock's Mesh.ocmesh", ec), "...and is in Trash/files under its own name");
    const std::string info = slurp(data / "Trash" / "info" / "Rock's Mesh.ocmesh.trashinfo");
    check(info.rfind("[Trash Info]\n", 0) == 0, "its .trashinfo opens with the [Trash Info] group");
    check(info.find("\nPath=/") != std::string::npos &&
              info.find("/work%20dir/Rock%27s%20Mesh.ocmesh\n") != std::string::npos,
          "and records the original path, percent-encoded");
    check(info.find("DeletionDate=") != std::string::npos, "and the deletion date");

    std::ofstream(a) << "two";
    check(editor::moveToRecycleBin(a.string()), "a second file of the same name is trashed too");
    check(fs::exists(data / "Trash" / "files" / "Rock's Mesh.ocmesh.1", ec), "...under a new name, not over the first");
    check(slurp(data / "Trash" / "files" / "Rock's Mesh.ocmesh") == "one", "and the first is untouched");

    const fs::path dir = work / "Folder";
    fs::create_directories(dir / "Inner", ec);
    std::ofstream(dir / "Inner" / "x.txt") << "x";
    check(editor::moveToRecycleBin(dir.string()), "a directory is trashed whole");
    check(fs::exists(data / "Trash" / "files" / "Folder" / "Inner" / "x.txt", ec), "...with its contents");

    check(!editor::moveToRecycleBin((work / "nothing-here").string()), "a path that does not exist fails, and says so");
    check(!fs::exists(data / "Trash" / "info" / "nothing-here.trashinfo", ec), "...and leaves no orphan .trashinfo behind");

    fs::remove_all(root, ec);
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
