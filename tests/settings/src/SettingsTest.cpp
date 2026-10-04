// The store a shipped game keeps its volume in.
//
// The failures worth catching here are all quiet ones: a hand-edited file silencing the game
// because a bad value read as 0 instead of leaving the setting alone, and a newer build's setting
// vanishing because an older build loaded the file and saved it back.
#include "aver/settings/settings_abi.h"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.4f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.5f}, want {:.5f})", what, got, want);
}

int main() {
    AVER_INFO("SettingsTest");

    std::error_code ec;
    const std::string dir = (std::filesystem::temp_directory_path() / "aver-settings-test").string();
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/settings.ini";

    AVER_INFO("a first run has no settings, and that is not an error");
    {
        check(aver_settings_open(path.c_str()) == 1, "opening a path that does not exist succeeds");
        check(aver_settings_count() == 0, "and the store is empty");
        checkNear(aver_settings_get_f32("audio.master", 0.8f), 0.8f, 1e-6f,
                  "an absent key reads its fallback");
        check(aver_settings_has("audio.master") == 0, "and is still absent after being read");
        check(aver_settings_flush() == 1, "flushing an unchanged store succeeds without writing");
        check(!fileExists(path), "and wrote NOTHING -- a game that changed no setting leaves no file");
    }

    AVER_INFO("values survive a flush and a reopen");
    {
        check(aver_settings_set_f32("audio.master", 0.35f) == 1, "a float writes");
        check(aver_settings_set_i32("video.msaa", 4) == 1, "an int writes");
        check(aver_settings_set_bool("video.vsync", 0) == 1, "a bool writes");
        check(aver_settings_set_str("game.language", "en-GB") == 1, "a string writes");
        check(aver_settings_flush() == 1, "the store flushes");
        check(fileExists(path), "and the file is on disk");

        // Reopening is the only honest way to test persistence -- reading back from the live map
        // would prove nothing about the file.
        check(aver_settings_open(path.c_str()) == 1, "it reopens");
        checkNear(aver_settings_get_f32("audio.master", 1.0f), 0.35f, 1e-4f, "the float survived");
        check(aver_settings_get_i32("video.msaa", 1) == 4, "the int survived");
        check(aver_settings_get_bool("video.vsync", 1) == 0,
              "the bool survived AS FALSE -- not as the fallback, which is the value it must be "
              "distinguishable from");
        check(std::string(aver_settings_get_str("game.language", "")) == "en-GB", "the string survived");
    }

    AVER_INFO("a value that does not parse leaves the setting alone");
    {
        // THE ONE THAT MATTERS. A player edits the file, types something wrong, and the game must
        // not silence itself: a bad value has to read as the caller's fallback, never as 0.
        check(writeFileText(path, "audio.master=loud\nvideo.msaa=\ngame.language=fr\n"),
              "a hand-edited file is written");
        check(aver_settings_open(path.c_str()) == 1, "and opens");
        checkNear(aver_settings_get_f32("audio.master", 0.8f), 0.8f, 1e-6f,
                  "'loud' reads as the FALLBACK, not as 0 -- which would silence the game");
        check(aver_settings_get_i32("video.msaa", 2) == 2, "an empty value reads as the fallback");
        check(std::string(aver_settings_get_str("game.language", "en")) == "fr",
              "and a value that IS valid still reads");
    }

    AVER_INFO("an unknown key survives an older build saving over it");
    {
        check(writeFileText(path, "# a comment\naudio.master=0.5\nsome.future.key=42\n"),
              "a file containing a key this build never heard of");
        check(aver_settings_open(path.c_str()) == 1, "opens");
        check(aver_settings_set_f32("audio.master", 0.9f) == 1, "this build changes its own setting");
        check(aver_settings_flush() == 1, "and flushes");

        std::string text;
        check(readFileText(path, text), "the file reads back");
        check(text.find("some.future.key=42") != std::string::npos,
              "AND THE UNKNOWN KEY IS STILL THERE -- a newer build's setting must not be deleted by "
              "an older build loading and saving");
        check(text.find("audio.master=0.9") != std::string::npos, "with this build's change applied");
    }

    AVER_INFO("a multi-line value is refused, not smuggled in");
    {
        check(aver_settings_set_str("bad", "one\ntwo") == 0,
              "a value carrying a newline is REFUSED -- the format has no escaping and it would "
              "read back as a second key");
        check(aver_settings_has("bad") == 0, "and nothing was stored");
        check(aver_settings_set_str("ok", "one two") == 1, "an ordinary value with a space is fine");
    }

    AVER_INFO("bools are written strictly and read generously");
    {
        check(writeFileText(path, "a=1\nb=yes\nc=on\nd=0\ne=no\nf=maybe\n"), "a hand-edited bool file");
        check(aver_settings_open(path.c_str()) == 1, "opens");
        check(aver_settings_get_bool("a", 0) == 1 && aver_settings_get_bool("b", 0) == 1 &&
              aver_settings_get_bool("c", 0) == 1, "1, yes and on all read true");
        check(aver_settings_get_bool("d", 1) == 0 && aver_settings_get_bool("e", 1) == 0,
              "0 and no read false");
        check(aver_settings_get_bool("f", 1) == 1,
              "and something that is neither reads the fallback rather than guessing");
    }

    AVER_INFO("remove, and the default path");
    {
        check(aver_settings_set_i32("gone", 1) == 1, "a key is set");
        check(aver_settings_remove("gone") == 1 && aver_settings_has("gone") == 0, "and removed");
        check(aver_settings_remove("neverExisted") == 1,
              "removing a key that was never there SUCCEEDS -- 'make sure this is not here' is what "
              "every caller means");

        const std::string dp = aver_settings_default_path();
        check(!dp.empty() && dp.find("settings.ini") != std::string::npos,
              "there is a default path, under the user data dir");
    }

    std::filesystem::remove_all(dir, ec);
    AVER_INFO(g_failures ? "SettingsTest: {} FAILURES" : "SettingsTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
