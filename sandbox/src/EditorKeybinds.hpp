#pragma once
// Rebindable keyboard shortcuts for the editor: what chord means what command, defaults matching
// every hardcoded key the editor used before this file existed, conflict detection, and persistence
// through the existing editor.ini preference store (EditorPrefs.hpp) -- no second config file.
//
// Whole-file `#if AVER_WITH_IMGUI`, the same idiom UiRegistry.hpp already uses: a Chord names an
// ImGuiKey, so this header has nothing to say in a build with no ImGui to name a key from, and every
// call site that would use it (handleManip, buildEditorPrefs, ...) is already inside the same guard.
#if AVER_WITH_IMGUI
#include "imgui.h"

#include "aver/core/Types.hpp"
// For editor::Scope, which this header used to define itself.
#include "InputOwnership.hpp"

#include <array>
#include <string>

namespace aver::editor {

// One chord: a key plus the modifiers this editor's shortcuts ever combine with it. No Super/Cmd --
// nothing here binds it, and Windows reserves most of that row anyway.
struct Chord {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false, alt = false;

    bool operator==(const Chord& o) const {
        return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
    }
    bool isBound() const { return key != ImGuiKey_None; }
};

// Every rebindable command. Declaration order is display order in the Keybinds preferences table.
enum class CommandId : u8 {
    ToolSelect, ToolMove, ToolRotate, ToolScale,
    SculptRaise, SculptLower, SculptSmooth, SculptFlatten,
    ModeToggleLandscape, ViewFrameSelected,
    PlayReleaseMouse, PlayStop,
    DrawerDismiss, DrawerToggleContent, DrawerToggleConsole,
    EditDelete, EditUndo, EditRedo, EditCopy, EditPaste, EditDuplicate, EditSelectAll,
    Count
};
inline constexpr usize kCommandCount = static_cast<usize>(CommandId::Count);

// Scope moved to InputOwnership.hpp -- see that header for why (this file is whole-file
// `#if AVER_WITH_IMGUI`, and the arbitration table that now hands out the live scope mask must be
// testable with no ImGui at all). Same namespace, same values, one definition.

// One command's fixed identity: its persisted key, its label, its default chord, which scope(s) it
// fires in, and how its modifiers and repeat behaviour are checked. checkCtrl/checkShift/repeatAllowed
// mirror each ORIGINAL hardcoded call site's exact behaviour, not a uniform policy -- see
// EditorKeybinds.cpp's kDefs table for the reasoning per row. Getting this right is what lets
// "existing hardcoded keys become defaults" mean current behaviour is byte-for-byte unchanged for
// anyone who never opens the Keybinds page, not just "the same key happens to still work".
struct KeybindDef {
    CommandId id;
    const char* strId;   // stable, persisted key suffix: "keybind.<strId>" in editor.ini
    const char* label;   // shown in the Keybinds preferences table
    Chord def;
    u32 scope;
    bool checkCtrl;       // false: this command's ORIGINAL site never gated on Ctrl at all
    bool checkShift;      // false: ditto for Shift. (Alt is never checked anywhere in this codebase.)
    bool repeatAllowed;
};

// The bindable-key vocabulary, exposed so a test can sweep the WHOLE table rather than re-listing
// it. A key absent from it cannot be displayed, parsed or captured, so this is the one place that
// decides what a user is allowed to bind.
usize       keyNameCount();
const char* keyNameAt(usize i);

// The compiled-in defaults, one row per CommandId, in declaration order.
const std::array<KeybindDef, kCommandCount>& keybindDefs();
// Looks up a command's def by id (keybindDefs()[(size_t)id] with the cast done once, in one place).
const KeybindDef& keybindDef(CommandId id);

// Renders a chord as "Ctrl+Shift+D" / "Delete" / "F" / "1" / "(unbound)". A small hand-written
// table, not ImGui::GetKeyName() -- this needs no live ImGui context, and chordToString/parseChord
// read the SAME table in opposite directions, so the file format and the display text can't drift
// apart from each other the way two independently-maintained tables could.
std::string chordToString(const Chord& c);
// Parses chordToString's own format. Empty or unrecognised text yields an unbound chord.
Chord parseChord(const std::string& text);

// Owns the CURRENT chord for every command (seeded from defaults), persistence, and conflict
// detection. One instance lives on SandboxApp as a plain member; nothing about this class reaches
// into SandboxApp, which is what lets it live in its own file.
class KeybindRegistry {
public:
    KeybindRegistry();   // seeds every chord from keybindDefs()

    // The command's live chord (possibly rebound).
    const Chord& chordFor(CommandId id) const { return current_[static_cast<usize>(id)]; }

    // The other command already holding `chord` in a scope that overlaps `scope`, or Count if free.
    // Comparing against `scope` (the chord's own intended scope) rather than always comparing
    // against every live scope is what lets 1/2/3/4 serve two different tool sets and Escape serve
    // two different dismissals without either pair ever being flagged.
    CommandId conflictWith(CommandId id, const Chord& chord, u32 scope) const;

    // Rebinds `id` to `chord`. Refuses (returns false) on a live conflict in ANY of `id`'s own
    // scopes -- the caller decides what to tell the user. Never silently steals another command's
    // binding; an unbound chord (isBound() == false) always succeeds and clears the command instead.
    bool rebind(CommandId id, const Chord& chord);
    // Restores one command's compiled-in default. Always succeeds: a command's own default can
    // never conflict with ITSELF, and conflictWith() only ever compares against OTHER commands'
    // CURRENT chords, so restoring one command cannot be blocked by another that was rebound onto
    // its slot in the meantime -- the other command simply keeps holding it, and this one goes back
    // to owning the chord it shipped with, which may now read as a (harmless, still-a-conflict) pair
    // until the user resolves it by hand, exactly as a fresh rebind would leave it.
    void resetToDefault(CommandId id);

    // True when the io state matches this command's chord right now, honouring its checkCtrl/
    // checkShift/repeatAllowed. A drop-in replacement for the ImGui::IsKeyPressed(...) call each
    // site used to make directly.
    bool pressed(CommandId id, const ImGuiIO& io) const;

    // Loads every chord from editor.ini (a missing key keeps its compiled-in default, so upgrading
    // from a build without this file needs no migration) / writes every chord back. Mirrors
    // loadEditorPreferences()/saveEditorPreferences()'s own shape in SandboxApp.cpp -- same file,
    // same prefString()/setPrefString() calls, just one more caller of them.
    void loadFromPrefs();
    void saveToPrefs() const;

    // Draws the "Keybinds" section of Editor Preferences: one row per command, a Rebind button that
    // enters a listen-for-next-chord state scoped to THIS call (so a stray keypress elsewhere in the
    // app, e.g. Tab cycling some other widget's focus, can never be mistaken for the new chord), and
    // a Reset-to-default button. Returns true if a rebind or reset changed anything, so the caller
    // knows to persist (mirroring how every other preference in buildEditorPrefs() is saved).
    //
    // `dpi` scales the two button columns the same way every other hardcoded pixel size in this
    // editor is scaled (e.g. SandboxApp.cpp's own `560.0f*dpi_`) -- this file has no dpi_ member of
    // its own, so the caller's is passed in rather than duplicated.
    bool drawPreferencesSection(f32 dpi = 1.0f);

private:
    std::array<Chord, kCommandCount> current_{};
    int listening_ = -1;        // index into CommandId currently capturing a chord, or -1
    std::string conflictLabel_; // label of the command blocking the last capture attempt, or empty
};

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
