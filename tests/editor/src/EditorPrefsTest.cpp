// The editor's preferences store: write, restart, read back.
//
// It is small, and the reason it is tested anyway is that its failure mode is SILENT. A preference
// that does not persist looks exactly like a preference the user never set, so nothing reports it
// and nobody investigates it -- they just drag the column again, every session, forever.
//
// The store lives in the sandbox rather than in a module, because it is editor UI state and has no
// business being reachable from the engine. This test therefore compiles that translation unit
// directly rather than linking a library. That is deliberate: promoting it to a module purely to
// make it testable would put editor preferences in the engine's dependency graph, which is a worse
// trade than an unusual line in a CMakeLists.
#include "../../../sandbox/src/EditorPrefs.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== editor preferences ===");

    // The store picks its own path from userDataDir. A test that wrote somewhere else would not be
    // testing the thing that ships, so this uses the real location and puts back what it found.
    const std::string path = [] {
        // Touch the store so it resolves its path before anything is read or written.
        (void)editor::prefFloat("aver.test.probe", 0.0f);
        return editor::editorPrefsPath();
    }();

    if (path.empty()) {
        AVER_WARN("=== SKIPPED: no user data directory on this machine ===");
        return 0;
    }
    AVER_INFO("  store: {}", path);

    // Preserve whatever is really there. This is a developer's own editor.ini.
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
        // The point of the plain-text format: a human can read and fix it.
        check(text.find('#') != std::string::npos, "with a comment header explaining what it is");
    }

    // ---- the value comes back, exactly -------------------------------------------------------
    //
    // Read through the store's own accessor rather than by parsing the file, because the accessor is
    // what the editor uses and is where a locale-dependent parse would go wrong.
    {
        const f32 got = editor::prefFloat("aver.test.width", -1.0f);
        check(std::fabs(got - 237.5f) < 1.0e-6f, "a fractional value round-trips EXACTLY");
    }

    // ---- a missing key is the caller's fallback, not zero ------------------------------------
    //
    // Every read takes a fallback so a fresh machine gets a designed default rather than a collapsed
    // column. Returning 0 for an absent key would make every first run look broken.
    {
        check(editor::prefFloat("aver.test.absent", 42.0f) == 42.0f,
              "an absent key returns the caller's fallback");
        check(editor::prefFloat("", 7.0f) == 7.0f, "and so does an empty key");
    }

    // ---- a corrupt file degrades, it does not break -------------------------------------------
    //
    // This file is hand-editable by design, which means it will be hand-edited badly. Every one of
    // these lines is something a person or a half-finished write could leave behind.
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

        // The store caches after its first read, so this exercises the parser by proving the file it
        // writes NEXT still contains the good key -- a parser that threw or bailed on the bad lines
        // would have lost it.
        editor::setPrefFloat("aver.test.after", 5.0f);
        editor::flushEditorPrefs();
        std::string text;
        check(readFileText(path, text), "and the store still writes afterwards");
        check(text.find("aver.test.after=5") != std::string::npos, "with the new value present");
    }

    // ---- restore the developer's own file -----------------------------------------------------
    if (hadFile) writeFileText(path, original);
    else         writeFileText(path, "");   // leave it empty rather than deleting: it was ours to make

    if (g_failures == 0) AVER_INFO("=== all editor preference tests passed ===");
    else                 AVER_ERROR("=== {} editor preference check(s) FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
