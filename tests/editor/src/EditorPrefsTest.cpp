// The editor's preferences store: write, restart, read back.
// Compiles the sandbox translation unit directly rather than linking a library.
#include "../../../sandbox/src/EditorPrefs.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;

static int g_failures = 0;

// Records one assertion and logs it.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Exercises the preference store against its real on-disk location, restoring it afterwards.
int main() {
    AVER_INFO("=== editor preferences ===");

    const std::string path = [] {
        (void)editor::prefFloat("aver.test.probe", 0.0f);
        return editor::editorPrefsPath();
    }();

    if (path.empty()) {
        AVER_WARN("=== SKIPPED: no user data directory on this machine ===");
        return 0;
    }
    AVER_INFO("  store: {}", path);

    // Preserve the developer's own editor.ini.
    std::string original;
    const bool hadFile = readFileText(path, original);

    // ---- a value survives a write ----------------------------------------------------------
    {
        editor::setPrefFloat("aver.test.width", 237.5f);
        editor::flushEditorPrefs();
        check(fileExists(path), "flush writes the file");

        std::string text;
        check(readFileText(path, text), "and it can be read back");
        check(text.find("aver.test.width=") != std::string::npos, "the key is in it");
        check(text.find('#') != std::string::npos, "with a comment header explaining what it is");
    }

    // ---- the value comes back, exactly -------------------------------------------------------
    {
        const f32 got = editor::prefFloat("aver.test.width", -1.0f);
        check(std::fabs(got - 237.5f) < 1.0e-6f, "a fractional value round-trips EXACTLY");
    }

    // ---- a missing key is the caller's fallback, not zero ------------------------------------
    {
        check(editor::prefFloat("aver.test.absent", 42.0f) == 42.0f,
              "an absent key returns the caller's fallback");
        check(editor::prefFloat("", 7.0f) == 7.0f, "and so does an empty key");
    }

    // ---- bools read as words, and survive both spellings --------------------------------------
    {
        editor::setPrefBool("aver.test.on", true);
        editor::setPrefBool("aver.test.off", false);
        editor::flushEditorPrefs();
        check(editor::prefBool("aver.test.on", false), "a true bool round-trips");
        check(!editor::prefBool("aver.test.off", true), "and so does a false one");

        std::string text;
        readFileText(path, text);
        check(text.find("aver.test.on=true") != std::string::npos,
              "and it is stored as a WORD, not a 1");
        check(editor::prefBool("aver.test.missing", true), "an absent bool takes the fallback");
        editor::setPrefString("aver.test.nonsense", "maybe");
        check(editor::prefBool("aver.test.nonsense", true), "an unparseable bool takes the fallback");
    }

    // ---- ints, including the negative the IDE choice uses for "Automatic" -----------------------
    {
        editor::setPrefInt("aver.test.count", 7);
        editor::setPrefInt("aver.test.neg", -1);
        editor::flushEditorPrefs();
        check(editor::prefInt("aver.test.count", 0) == 7, "an int round-trips");
        check(editor::prefInt("aver.test.neg", 0) == -1, "and so does a negative one");
        check(editor::prefInt("aver.test.absent", 9) == 9, "an absent int takes the fallback");
    }

    // ---- strings, and the one thing they must refuse -------------------------------------------
    {
        editor::setPrefString("aver.test.name", "Visual Studio 2022");
        editor::flushEditorPrefs();
        check(editor::prefString("aver.test.name", "") == "Visual Studio 2022",
              "a string with spaces round-trips");
        check(editor::prefString("aver.test.nothing", "fallback") == "fallback",
              "an absent string takes the fallback");

        const std::string was = editor::prefString("aver.test.name", "");
        editor::setPrefString("aver.test.name", std::string("two") + '\n' + "lines");
        check(editor::prefString("aver.test.name", "") == was,
              "a multi-line string is REFUSED, leaving the previous value intact");
    }

    // ---- a corrupt file degrades, it does not break -------------------------------------------
    {
        const std::string junk =
            "# a comment\n"
            "\n"
            "no-equals-sign-here\n"
            "aver.test.good=123.25\n"
            "aver.test.bad=not-a-number\n"
            "aver.test.empty=\n"
            "=novalue\n";
        check(writeFileText(path, junk), "a deliberately malformed file is written");

        editor::setPrefFloat("aver.test.after", 5.0f);
        editor::flushEditorPrefs();
        std::string text;
        check(readFileText(path, text), "and the store still writes afterwards");
        check(text.find("aver.test.after=5") != std::string::npos, "with the new value present");
    }

    // ---- a flush that changed nothing writes nothing -------------------------------------------
    //
    // THE PROPERTY THE AUTOSAVE TIMER RESTS ON. saveEditorPreferences() is now called from
    // onUpdate() every couple of seconds, pushing all ~40 members through setPref* every time.
    // That is only free because setPrefString compares before dirtying and flushEditorPrefs
    // early-outs on a clean store. If either stopped holding, the editor would rewrite this file
    // hundreds of times a minute and nothing would fail loudly enough to notice.
    //
    // MEASURED BY MTIME, not by content: a rewrite with identical bytes is still a rewrite, and it
    // is the I/O that matters here, not the result.
    {
        editor::setPrefFloat("aver.test.idle", 42.0f);
        editor::flushEditorPrefs();
        const auto first = std::filesystem::last_write_time(path);

        // Same values, over and over -- exactly what the timer does on an idle editor.
        for (int i = 0; i < 8; ++i) {
            editor::setPrefFloat("aver.test.idle", 42.0f);
            editor::setPrefString("aver.test.name", editor::prefString("aver.test.name", ""));
            editor::flushEditorPrefs();
        }
        check(std::filesystem::last_write_time(path) == first,
              "re-flushing unchanged values does NOT rewrite the file");

        // And a real change still gets through, so the early-out is not simply stuck.
        editor::setPrefFloat("aver.test.idle", 43.0f);
        editor::flushEditorPrefs();
        std::string text;
        check(readFileText(path, text) && text.find("aver.test.idle=43") != std::string::npos,
              "but a genuine change is still written");
    }

    // ---- an UNREADABLE file must never be written over ----------------------------------------
    //
    // THE FAILURE THIS PREVENTS IS TOTAL, NOT PARTIAL. `readFileText` returns false both for a file
    // that is absent and for one that exists and could not be opened -- a lock, a permissions
    // change, a network share that blinked. Before this, both answers left the store empty AND
    // considered loaded, so the very next flush replaced a good editor.ini with a three-line comment
    // header via writeFileTextAtomic: every preference and every keybind gone, atomically, with no
    // way back.
    //
    // Tested through the pure predicate rather than by staging a locked file, because ensureLoaded
    // latches once per process and has no reset hook -- an in-process test can never reach the
    // branch through the front door. The predicate IS the decision; the caller only obeys it.
    {
        check(!editor::prefsShouldRefuseWrite(true, true),
              "a file that was read successfully is writable");
        check(!editor::prefsShouldRefuseWrite(false, false),
              "a MISSING file is the ordinary first run and must stay writable");
        check(editor::prefsShouldRefuseWrite(false, true),
              "but a file that EXISTS and could not be read is never written over");
        check(!editor::prefsShouldRefuseWrite(true, false),
              "and a successful read of a vanished file is not a refusal either");
        // This process read its own file fine, so the latch must be clear -- a test asserting the
        // refusal path must not leave the store poisoned for the checks above it.
        check(!editor::editorPrefsReadOnly(), "this session read its file, so it is not read-only");
    }

    // ---- recovering from read-only must never step on a change the session already made ------
    //
    // THE MERGE RULE, as its own pure decision -- see prefsShouldAdoptFromFile's header comment.
    // tryRecoverReadOnly itself (EditorPrefs.cpp) is not reachable from here for the same reason the
    // block above tests prefsShouldRefuseWrite instead of staging a locked file: ensureLoaded
    // latches once per process with no reset hook, so the read-only path this decision guards can
    // never be driven through the front door in one test binary. The predicate IS the decision; the
    // merge loop only obeys it.
    {
        check(!editor::prefsShouldAdoptFromFile(true),
              "a key THIS SESSION already set is never overwritten by the file's older value");
        check(editor::prefsShouldAdoptFromFile(false),
              "a key this session never touched IS adopted from the file, so recovery loses nothing");
    }

    // ---- restore the developer's own file -----------------------------------------------------
    if (hadFile) writeFileText(path, original);
    else         writeFileText(path, "");

    if (g_failures == 0) AVER_INFO("=== all editor preference tests passed ===");
    else                 AVER_ERROR("=== {} editor preference check(s) FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
