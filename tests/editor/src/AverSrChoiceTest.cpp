// AverSrChoiceTest -- the pure decisions behind U2/3.3 A's AverSR "choice" abstraction
// (AverSrChoice.hpp): name<->enum round-trip, the one-time migration off the pre-Auto
// display.aversr/display.renderScale pair, userLevelFor's -1 sentinel for the two choices with no
// single sr::Quality level of their own, and renderScaleToPersist's Manual-only rule.
//
// Header-only and dependency-free, for PtRenderConflictTest.cpp's exact reason (see that file's own
// top comment): no ImGui, no SandboxApp, no AVER_INFO/AVER_CRITICAL, no editor::prefString --
// AverSrChoice.hpp takes plain bools/strings/floats in and returns plain data out, so the migration
// rule SandboxApp.cpp's loadEditorPreferences() applies is reachable without a window, a device, or an
// editor.ini on disk. This is COMPILED, not run, by this lane's own build -- see the task's own
// verification rule.
#include "AverSrChoice.hpp"

#include <cstdio>

using namespace aver::editor;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("[INFO ]   ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("[ERROR]   FAIL  %s\n", what);
    }
}

} // namespace

int main() {
    std::printf("[INFO ] === AverSR choice rules ===\n");

    // ---- name <-> enum round-trip, all six choices ----
    {
        static const AverSrChoice kAll[] = {AverSrChoice::Auto, AverSrChoice::Off, AverSrChoice::Quality,
                                            AverSrChoice::Balanced, AverSrChoice::Performance,
                                            AverSrChoice::Manual};
        for (AverSrChoice c : kAll) {
            AverSrChoice back;
            const bool parsed = parseAverSrChoice(averSrChoiceName(c), back);
            check(parsed && back == c, averSrChoiceName(c));
        }
        // Case-insensitive, matching every other name parser in this codebase.
        AverSrChoice c;
        check(parseAverSrChoice("PERFORMANCE", c) && c == AverSrChoice::Performance,
              "parse is case-insensitive (PERFORMANCE)");
        check(parseAverSrChoice("auto", c) && c == AverSrChoice::Auto, "parse is case-insensitive (auto)");
        // Never partial.
        check(!parseAverSrChoice("qual", c), "'qual' does not partially match 'quality'");
        check(!parseAverSrChoice("", c), "empty string does not parse");
    }

    // ---- migration (3.3 A) ----
    {
        check(migrateAverSrChoice(false, "", 0.0f, 1.0f) == AverSrChoice::Auto,
              "no key, legacy level 0, scale 1: Auto");
    }
    {
        check(migrateAverSrChoice(false, "", 2.0f, 1.0f) == AverSrChoice::Balanced,
              "no key, legacy level 2: Balanced");
        check(migrateAverSrChoice(false, "", 1.0f, 1.0f) == AverSrChoice::Quality,
              "no key, legacy level 1: Quality");
        check(migrateAverSrChoice(false, "", 3.0f, 1.0f) == AverSrChoice::Performance,
              "no key, legacy level 3: Performance");
        // Out-of-range clamps rather than indexing off the level table's end (editor.ini is a text
        // file a person can hand-edit) -- same defensive clamp the old inline load code carried.
        check(migrateAverSrChoice(false, "", 99.0f, 1.0f) == AverSrChoice::Performance,
              "no key, legacy level far above 3: clamps to Performance, not undefined");
    }
    {
        check(migrateAverSrChoice(false, "", 0.0f, 0.75f) == AverSrChoice::Manual,
              "no key, legacy level 0 (Off), scale 0.75: Manual");
        // 0 must NOT read as an explicit Off (plan 10.1 item 1): display.aversr is written from the
        // live quality every session regardless of what the user actually did, so treating a stored 0
        // as a deliberate choice would make U2's whole "AverSR defaults on" decision a no-op on every
        // pre-existing install.
        check(migrateAverSrChoice(false, "", 0.4f, 1.0f) == AverSrChoice::Auto,
              "legacy level below the 0.5 noise floor reads as unset, not as Off");
    }
    {
        // AN EXISTING KEY WINS, regardless of what the two legacy floats say -- migration only
        // consults them on the absent-key path.
        check(migrateAverSrChoice(true, "Off", 3.0f, 0.5f) == AverSrChoice::Off,
              "existing key 'Off' wins over legacy level 3 and legacy scale 0.5");
        check(migrateAverSrChoice(true, "manual", 0.0f, 1.0f) == AverSrChoice::Manual,
              "existing key 'manual' wins even though the legacy pair alone would have said Auto");
    }
    {
        // A GARBAGE KEY GIVES AUTO, not a crash and not a silent fall-through to the legacy pair --
        // Auto is always a safe, working choice.
        check(migrateAverSrChoice(true, "purple", 3.0f, 1.0f) == AverSrChoice::Auto,
              "garbage key text: Auto, not the legacy level it ignores");
    }

    // ---- userLevelFor ----
    {
        check(userLevelFor(AverSrChoice::Auto) == -1, "userLevelFor(Auto) == -1");
        check(userLevelFor(AverSrChoice::Manual) == -1, "userLevelFor(Manual) == -1");
        check(userLevelFor(AverSrChoice::Off) == 0, "userLevelFor(Off) == 0, sr::Quality's own numbering");
        check(userLevelFor(AverSrChoice::Quality) == 1, "userLevelFor(Quality) == 1");
        check(userLevelFor(AverSrChoice::Balanced) == 2, "userLevelFor(Balanced) == 2");
        check(userLevelFor(AverSrChoice::Performance) == 3, "userLevelFor(Performance) == 3");
    }

    // ---- renderScaleToPersist ----
    {
        check(renderScaleToPersist(AverSrChoice::Manual, 0.83f) == 0.83f,
              "Manual persists the live scale");
        static const AverSrChoice kNotManual[] = {AverSrChoice::Auto, AverSrChoice::Off,
                                                   AverSrChoice::Quality, AverSrChoice::Balanced,
                                                   AverSrChoice::Performance};
        for (AverSrChoice c : kNotManual) {
            char what[96];
            std::snprintf(what, sizeof what, "%s persists 1.0, not the live scale", averSrChoiceName(c));
            check(renderScaleToPersist(c, 0.5f) == 1.0f, what);
        }
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
