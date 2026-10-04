// LevelViewStore.hpp: the per-level "where the camera was left" store -- its key and its file grammar.
#include "LevelViewStore.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("LevelViewStoreTest");

    AVER_INFO("key: project-relative, forward slashes, lower case");
    {
        const std::string proj = "C:\\Users\\Me\\Projects\\PTTest";
        check(levelViewKey("C:\\Users\\Me\\Projects\\PTTest\\Content\\Maps\\Main.ocworld", proj) ==
                  "content/maps/main.ocworld", "a level inside the project keys by its relative path");
        check(levelViewKey("c:/users/me/projects/pttest/Content/Maps/Main.ocworld", proj) ==
                  levelViewKey("C:\\Users\\Me\\Projects\\PTTest\\Content\\Maps\\Main.ocworld", proj),
              "case and separator differences reach the same key");
        check(levelViewKey("C:\\Users\\Me\\Projects\\PTTest\\Content\\..\\Content\\Maps\\Main.ocworld", proj) ==
                  "content/maps/main.ocworld", "a path with .. is normalised first");
        const std::string outside = levelViewKey("D:\\Elsewhere\\Other.ocworld", proj);
        check(outside.find("..") == std::string::npos && outside.find("other.ocworld") != std::string::npos,
              "a level outside the project keeps an absolute key, not a ../ one");
        check(levelViewKey("", proj).empty(), "no level path, no key");
    }

    AVER_INFO("format then parse returns the same views");
    {
        LevelViewMap views;
        views["content/maps/main.ocworld"] = LevelView{1234.5f, -678.25f, 90.0f, -135.5f, -22.25f, 6400.0f};
        views["content/maps/a=b.ocworld"]  = LevelView{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 0.0f};
        const LevelViewMap back = parseLevelViews(formatLevelViews(views));
        check(back.size() == 2, "both entries survive");
        const auto it = back.find("content/maps/main.ocworld");
        check(it != back.end() && it->second.x == 1234.5f && it->second.y == -678.25f &&
                  it->second.z == 90.0f && it->second.yawDeg == -135.5f &&
                  it->second.pitchDeg == -22.25f && it->second.speed == 6400.0f,
              "every field round-trips exactly");
        check(back.count("content/maps/a=b.ocworld") == 1, "a key containing '=' splits at the LAST '='");
    }

    AVER_INFO("parse tolerates damage without losing the good rows");
    {
        const LevelViewMap v = parseLevelViews(
            "# comment\r\n"
            "\n"
            "good.ocworld=1 2 3 4 5 6\r\n"
            "nospeed.ocworld=1 2 3 4 5\n"
            "short.ocworld=1 2 3\n"
            "junk.ocworld=a b c d e f\n"
            "nan.ocworld=nan 2 3 4 5 6\n"
            "noequals\n"
            "=1 2 3 4 5 6\n");
        check(v.count("good.ocworld") == 1, "a well-formed row with CRLF parses");
        check(v.count("nospeed.ocworld") == 1 && v.at("nospeed.ocworld").speed == 0.0f,
              "a row with no speed parses as speed 0 (unstated)");
        check(v.count("short.ocworld") == 0, "a row missing the angles is skipped");
        check(v.count("junk.ocworld") == 0, "a row of non-numbers is skipped");
        check(v.count("nan.ocworld") == 0, "a non-finite value is skipped, not applied to the camera");
        check(v.size() == 2, "comments, blank lines and key-less rows add nothing");
    }

    AVER_INFO("LevelViewStoreTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
