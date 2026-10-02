// The keybind registry: the chord codec, conflict scoping, rebind refusal, and the persisted ids.
//
// THIS IS THE FIRST HEADLESS COVERAGE THE KEYBIND SYSTEM HAS EVER HAD. What existed was
// --keybind-test write|read, a two-process check inside Sandbox.exe that needs a window and a GPU
// and runs under no harness at all -- so chordToString/parseChord, conflictWith and rebind were
// shipped entirely unasserted. The assertions that harness makes about refusal and defaults are
// absorbed below so they finally run in CI.
//
// It links ImGui, and that is worth being honest about: it is the first test target in the tree to
// do so. EditorKeybinds.hpp is whole-file `#if AVER_WITH_IMGUI` because a Chord names an ImGuiKey,
// so the enum values alone require it. NOTHING HERE MAY CALL AN ImGui FUNCTION or create a context
// -- the codec is pure, and if it ever stops being pure this test will start needing a live GImGui,
// which is the signal that the purity was lost.
#include "../../../sandbox/src/EditorKeybinds.hpp"

#include "aver/core/Log.hpp"

#include <set>
#include <string>
#include <vector>

using namespace aver;
using editor::Chord;
using editor::CommandId;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== editor keybinds ===");

    // ---- the codec round-trips every shipped default -------------------------------------------
    //
    // THIS PAIR IS THE PERSISTENCE CONTRACT: saveToPrefs writes chordToString and loadFromPrefs
    // reads parseChord, so a chord that does not survive the pair is a binding that silently
    // reverts to its default on the next launch. Nothing checked it before.
    {
        bool allRoundTrip = true;
        std::string firstBad;
        for (const editor::KeybindDef& d : editor::keybindDefs()) {
            const std::string text = editor::chordToString(d.def);
            if (!(editor::parseChord(text) == d.def)) {
                allRoundTrip = false;
                if (firstBad.empty()) firstBad = std::string(d.strId) + " -> '" + text + "'";
            }
        }
        check(allRoundTrip, "every shipped default chord survives chordToString -> parseChord" +
                            (firstBad.empty() ? "" : " (first failure: " + firstBad + ")"));
    }

    // ---- the codec round-trips the WHOLE vocabulary, in every modifier combination --------------
    //
    // Swept off the real table rather than a copy, which is what makes a newly added key covered the
    // moment it is added. This is the assertion that catches a key whose display name contains a
    // '+': parseChord splits on '+' and takes the last token, so such a name parses back as garbage.
    {
        usize tried = 0;
        bool ok = true;
        std::string firstBad;
        for (usize i = 0; i < editor::keyNameCount(); ++i) {
            const char* name = editor::keyNameAt(i);
            if (!name) { ok = false; continue; }
            for (int mods = 0; mods < 8; ++mods) {
                std::string text;
                if (mods & 1) text += "Ctrl+";
                if (mods & 2) text += "Shift+";
                if (mods & 4) text += "Alt+";
                text += name;
                const Chord parsed = editor::parseChord(text);
                ++tried;
                if (!parsed.isBound() || editor::chordToString(parsed) != text) {
                    ok = false;
                    if (firstBad.empty()) firstBad = text;
                }
            }
        }
        check(tried == editor::keyNameCount() * 8, "the sweep covered every key x every modifier set");
        check(ok, "and all " + std::to_string(tried) + " round-trip exactly" +
                  (firstBad.empty() ? "" : " (first failure: '" + firstBad + "')"));
    }

    // ---- keys that were unbindable until the table was widened ----------------------------------
    //
    // These are the assertion that the widening actually happened, rather than being assumed. Before
    // it, every one of these parsed as unbound and could not be captured in the UI at all.
    {
        for (const char* n : {"Up", "Down", "Left", "Right", "Home", "End",
                              "Enter", "Backspace", "PageUp", "Keypad7", "Comma"})
            check(editor::parseChord(n).isBound(), std::string("'") + n + "' is a bindable key");
    }

    // ---- the unbound token, in both spellings ---------------------------------------------------
    {
        check(editor::chordToString(Chord{}) == "none", "an unbound chord renders as the token 'none'");
        check(!editor::parseChord("none").isBound(), "and 'none' parses back as unbound");
        check(!editor::parseChord("").isBound(), "an empty string is unbound");
        // ACCEPTED IN, NEVER WRITTEN OUT. The header claimed for a long time that chordToString
        // rendered "(unbound)"; it never did. Reading it is charity toward anyone who hand-edited
        // editor.ini after believing the comment. Emitting it would change a file-format token and
        // silently unbind every command a user had cleared, so the codec stays asymmetric on purpose.
        check(!editor::parseChord("(unbound)").isBound(), "'(unbound)' is accepted on the way IN");
        check(editor::chordToString(Chord{}) != "(unbound)",
              "but is never what gets written -- the on-disk token stays 'none'");
        // Garbage must be FULLY unbound, not a chord that kept its modifiers -- a half-parsed
        // Ctrl+<nothing> would silently fire on Ctrl alone.
        const Chord junk = editor::parseChord("Ctrl+Nonsense");
        check(!junk.isBound(), "an unrecognised key name parses as unbound");
        check(!junk.ctrl, "and does NOT keep the modifier it was written with");
    }

    // ---- strIds are the persisted identity ------------------------------------------------------
    //
    // A strId is the suffix of "keybind.<strId>" in editor.ini. Appending a row is safe; RENAMING one
    // silently discards every user's rebind of that command, because the old key is never read
    // again and the new one is absent. The golden list is what makes that impossible to do by
    // accident -- if this fails, either a row was renamed (do not) or a row was added (extend the
    // list deliberately).
    {
        const std::vector<std::string> golden = {
            "tool.select", "tool.move", "tool.rotate", "tool.scale",
            "sculpt.raise", "sculpt.lower", "sculpt.smooth", "sculpt.flatten",
            "mode.toggleLandscape", "view.frameSelected",
            "play.start", "play.simulate", "play.releaseMouse", "play.eject",
            // 2026-09-30: Pawn to Camera, beside Eject because ejected is the only state it fires in.
            "play.pawnToCamera", "play.pause",
            "play.frameSkip", "play.stop",
            "drawer.dismiss", "drawer.toggleContent", "drawer.toggleConsole",
            "edit.delete", "edit.undo", "edit.redo", "edit.copy", "edit.paste",
            "edit.duplicate", "edit.selectAll",
            "asset.save", "level.save", "file.saveAll", "graph.commentBox", "graph.frameAll",
            // 2026-09-16: Play, Save All, Compile/Reload Scripts and Screenshot gained chords. Persistence
            // is by strId, not slot, so inserting play.start and file.saveAll beside their families
            // renames nothing a user has already rebound.
            "scripts.compile", "scripts.reload", "view.screenshot",
            "edit.snapToFloor", "view.hideSelected", "view.isolateSelected", "view.unhideAll",
            "edit.nudgeLeft", "edit.nudgeRight", "edit.nudgeForward", "edit.nudgeBack",
            "edit.nudgeUp", "edit.nudgeDown",
        };
        const auto& defs = editor::keybindDefs();
        check(defs.size() == golden.size(),
              "the command table still has " + std::to_string(golden.size()) + " rows");
        bool match = defs.size() == golden.size();
        std::string firstBad;
        for (usize i = 0; i < defs.size() && i < golden.size(); ++i) {
            if (golden[i] != defs[i].strId) {
                match = false;
                if (firstBad.empty())
                    firstBad = "slot " + std::to_string(i) + ": expected '" + golden[i] +
                               "', found '" + std::string(defs[i].strId) + "'";
            }
        }
        check(match, "and every persisted strId is unchanged" +
                     (firstBad.empty() ? "" : " (" + firstBad + ")"));

        std::set<std::string> seen;
        bool unique = true, wellFormed = true;
        for (const editor::KeybindDef& d : defs) {
            const std::string id = d.strId;
            if (!seen.insert(id).second) unique = false;
            // '=' would split the line in editor.ini and a newline would truncate the value, both
            // of which the preference store documents as refusals rather than escapes.
            if (id.empty() || id.find('=') != std::string::npos ||
                id.find('\n') != std::string::npos) wellFormed = false;
        }
        check(unique, "no two commands share a persisted id");
        check(wellFormed, "and every id is non-empty and safe for a key=value line");
    }

    // ---- the table is indexed by cast, so its ORDER is load-bearing ------------------------------
    {
        bool ordered = true;
        const auto& defs = editor::keybindDefs();
        for (usize i = 0; i < defs.size(); ++i)
            if (defs[i].id != static_cast<CommandId>(i)) ordered = false;
        check(ordered, "kDefs[i].id == CommandId(i) -- a mis-ordered row would bind the wrong command");
    }

    // ---- the shipped defaults do not fight each other ---------------------------------------------
    //
    // Never checked before. A new command whose default collided with an existing one in the same
    // scope would ship silently, and the loser would simply never fire.
    {
        editor::KeybindRegistry reg;
        bool clean = true;
        std::string firstBad;
        for (const editor::KeybindDef& d : editor::keybindDefs()) {
            const CommandId other = reg.conflictWith(d.id, d.def, d.scope);
            if (other != CommandId::Count) {
                clean = false;
                if (firstBad.empty())
                    firstBad = std::string(d.strId) + " vs " +
                               editor::keybindDef(other).strId + " on " + editor::chordToString(d.def);
            }
        }
        check(clean, "no two shipped defaults collide within a shared scope" +
                     (firstBad.empty() ? "" : " (" + firstBad + ")"));
    }

    // ---- conflicts are SCOPED, which is what lets one key serve two modes -------------------------
    {
        editor::KeybindRegistry reg;
        // 1 is both Select Tool (object mode) and Sculpt Raise (landscape mode). Different scopes,
        // so this is deliberately not a conflict -- the modes are never live at once.
        check(reg.conflictWith(CommandId::ToolSelect,
                               editor::keybindDef(CommandId::SculptRaise).def,
                               editor::keybindDef(CommandId::ToolSelect).scope) == CommandId::Count,
              "the same key in two different scopes is not a conflict");
        // Escape is both Stop Play and Dismiss Drawer, for the same reason.
        check(reg.conflictWith(CommandId::PlayStop,
                               editor::keybindDef(CommandId::DrawerDismiss).def,
                               editor::keybindDef(CommandId::PlayStop).scope) == CommandId::Count,
              "and Escape serving both Play-stop and drawer-dismiss is not a conflict");
    }

    // ---- rebind REFUSES rather than steals, and leaves the old binding intact ----------------------
    //
    // Lifted out of --keybind-test, which could only run with a window and a GPU.
    {
        editor::KeybindRegistry reg;
        const Chord ctrlZ = editor::keybindDef(CommandId::EditUndo).def;
        check(!reg.rebind(CommandId::EditPaste, ctrlZ), "rebinding onto a taken chord is refused");
        check(editor::chordToString(reg.chordFor(CommandId::EditPaste)) == "Ctrl+V",
              "and the refused command keeps the chord it had");

        const Chord ctrlK{ImGuiKey_K, true, false, false};
        check(reg.conflictWith(CommandId::EditCopy, ctrlK,
                               editor::keybindDef(CommandId::EditCopy).scope) == CommandId::Count,
              "Ctrl+K is free");
        check(reg.rebind(CommandId::EditCopy, ctrlK), "so rebinding Copy onto it succeeds");
        check(editor::chordToString(reg.chordFor(CommandId::EditCopy)) == "Ctrl+K",
              "and the new chord reads back");

        // Clear, which the Preferences page's own conflict message tells the user to reach for.
        check(reg.rebind(CommandId::EditCopy, Chord{}), "an unbound chord always succeeds");
        check(!reg.chordFor(CommandId::EditCopy).isBound(), "and clears the command");

        reg.resetToDefault(CommandId::EditCopy);
        check(editor::chordToString(reg.chordFor(CommandId::EditCopy)) == "Ctrl+C",
              "reset puts the compiled-in default back");
    }

    if (g_failures == 0) AVER_INFO("=== all {} keybind checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} keybind check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
