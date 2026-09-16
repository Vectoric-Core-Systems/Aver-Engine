// See EditorKeybinds.hpp for what this is. The one thing worth restating here: every `def` row
// below is a transcription of an ACTUAL call site in SandboxApp.cpp as it stood before this file
// existed (cited per row), not a guess at what seemed reasonable -- that is what lets "existing
// hardcoded keys become defaults" mean current behaviour is unchanged for anyone who never opens
// the Keybinds page.
#include "EditorKeybinds.hpp"
#if AVER_WITH_IMGUI
#include "EditorPrefs.hpp"
#include "UiRegistry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {
namespace {

// The chord codec's allow-list. Deliberately small and hand-written (not ImGuiKey::GetKeyName()):
// every key any editor shortcut has ever bound, past or future, needs an entry added here -- that
// friction is the point, since a key this table doesn't know is a key a rebind cannot select either
// (see the capture loop in drawPreferencesSection below, which only polls keys from this table).
struct KeyName { ImGuiKey key; const char* name; };
constexpr KeyName kKeyNames[] = {
    {ImGuiKey_A,"A"},{ImGuiKey_B,"B"},{ImGuiKey_C,"C"},{ImGuiKey_D,"D"},{ImGuiKey_E,"E"},
    {ImGuiKey_F,"F"},{ImGuiKey_G,"G"},{ImGuiKey_H,"H"},{ImGuiKey_I,"I"},{ImGuiKey_J,"J"},
    {ImGuiKey_K,"K"},{ImGuiKey_L,"L"},{ImGuiKey_M,"M"},{ImGuiKey_N,"N"},{ImGuiKey_O,"O"},
    {ImGuiKey_P,"P"},{ImGuiKey_Q,"Q"},{ImGuiKey_R,"R"},{ImGuiKey_S,"S"},{ImGuiKey_T,"T"},
    {ImGuiKey_U,"U"},{ImGuiKey_V,"V"},{ImGuiKey_W,"W"},{ImGuiKey_X,"X"},{ImGuiKey_Y,"Y"},
    {ImGuiKey_Z,"Z"},
    {ImGuiKey_0,"0"},{ImGuiKey_1,"1"},{ImGuiKey_2,"2"},{ImGuiKey_3,"3"},{ImGuiKey_4,"4"},
    {ImGuiKey_5,"5"},{ImGuiKey_6,"6"},{ImGuiKey_7,"7"},{ImGuiKey_8,"8"},{ImGuiKey_9,"9"},
    {ImGuiKey_F1,"F1"},{ImGuiKey_F2,"F2"},{ImGuiKey_F3,"F3"},{ImGuiKey_F4,"F4"},
    {ImGuiKey_F5,"F5"},{ImGuiKey_F6,"F6"},{ImGuiKey_F7,"F7"},{ImGuiKey_F8,"F8"},
    {ImGuiKey_F9,"F9"},{ImGuiKey_F10,"F10"},{ImGuiKey_F11,"F11"},{ImGuiKey_F12,"F12"},
    {ImGuiKey_Delete,"Delete"},{ImGuiKey_Tab,"Tab"},{ImGuiKey_Space,"Space"},
    {ImGuiKey_Escape,"Escape"},{ImGuiKey_GraveAccent,"`"},
    // NAVIGATION AND EDITING, added because their absence was a hard ceiling rather than a
    // preference: a key missing from this table cannot be displayed, parsed OR captured, so no
    // amount of UI work could bind one. GraphEditor's Home->frameAll could not be promoted into the
    // registry at all until Home existed here.
    {ImGuiKey_UpArrow,"Up"},{ImGuiKey_DownArrow,"Down"},
    {ImGuiKey_LeftArrow,"Left"},{ImGuiKey_RightArrow,"Right"},
    {ImGuiKey_Enter,"Enter"},{ImGuiKey_KeypadEnter,"KeypadEnter"},
    {ImGuiKey_Backspace,"Backspace"},{ImGuiKey_Insert,"Insert"},
    {ImGuiKey_Home,"Home"},{ImGuiKey_End,"End"},
    {ImGuiKey_PageUp,"PageUp"},{ImGuiKey_PageDown,"PageDown"},
    // The numpad, which is a genuinely separate set of keys: someone binding Keypad1 does not want
    // the row-1 above the letters to fire as well.
    {ImGuiKey_Keypad0,"Keypad0"},{ImGuiKey_Keypad1,"Keypad1"},{ImGuiKey_Keypad2,"Keypad2"},
    {ImGuiKey_Keypad3,"Keypad3"},{ImGuiKey_Keypad4,"Keypad4"},{ImGuiKey_Keypad5,"Keypad5"},
    {ImGuiKey_Keypad6,"Keypad6"},{ImGuiKey_Keypad7,"Keypad7"},{ImGuiKey_Keypad8,"Keypad8"},
    {ImGuiKey_Keypad9,"Keypad9"},
    {ImGuiKey_KeypadAdd,"KeypadAdd"},{ImGuiKey_KeypadSubtract,"KeypadSubtract"},
    {ImGuiKey_KeypadMultiply,"KeypadMultiply"},{ImGuiKey_KeypadDivide,"KeypadDivide"},
    {ImGuiKey_KeypadDecimal,"KeypadDecimal"},
    // PUNCTUATION, and note what is NOT here: no name may contain '+', because parseChord splits on
    // it and takes the last token, so a key called "+" would parse as an empty key name. Plus and
    // Minus are therefore spelled as words on the keypad and as bracket-style names in the row.
    {ImGuiKey_Minus,"Minus"},{ImGuiKey_Equal,"Equal"},
    {ImGuiKey_LeftBracket,"LeftBracket"},{ImGuiKey_RightBracket,"RightBracket"},
    {ImGuiKey_Backslash,"Backslash"},{ImGuiKey_Semicolon,"Semicolon"},
    {ImGuiKey_Apostrophe,"Apostrophe"},{ImGuiKey_Comma,"Comma"},
    {ImGuiKey_Period,"Period"},{ImGuiKey_Slash,"Slash"},
    // DELIBERATELY ABSENT: CapsLock, NumLock, ScrollLock, PrintScreen and Pause, which are OS-level
    // traps on Windows rather than shortcuts; and the bare modifiers, which Chord has no way to
    // represent -- it holds one key plus three booleans, so "Ctrl" alone is not expressible.
};


const char* keyName(ImGuiKey k) {
    for (const KeyName& kn : kKeyNames) if (kn.key == k) return kn.name;
    return nullptr;
}
// "prefs.keybind.<strId>.<action>", so automation can address ONE row rather than hunting for a
// rect. Returned by value; UiRegistry copies the name into a std::string, so a temporary is safe.
std::string trackName(const char* strId, const char* action) {
    return std::string("prefs.keybind.") + strId + action;
}

ImGuiKey nameToKey(std::string_view s) {
    for (const KeyName& kn : kKeyNames) if (s == kn.name) return kn.key;
    return ImGuiKey_None;
}

// The defaults table. Each `def` row transcribes one hardcoded call site SandboxApp.cpp had before
// this file existed:
//   ToolSelect..ToolScale     -- handleManip(), the object-mode branch of the 1..4 dispatch
//                                (was: bare `IsKeyPressed(ImGuiKey_1..4)`, default repeat=true)
//   SculptRaise..SculptFlatten-- handleManip(), the Landscape-mode branch of the SAME 1..4 dispatch
//   ModeToggleLandscape       -- handleManip(), `IsKeyPressed(ImGuiKey_Tab)` (default repeat=true)
//   ViewFrameSelected         -- the F-focus block, `IsKeyPressed(ImGuiKey_F)` (default repeat=true)
//   PlayStart                 -- NEW command (2026-09-16): Play had only its toolbar button and the
//                                Simulate panel's. Alt+P, Unreal's chord for the same thing. Viewport
//                                scope, so it cannot fire during the session it would start.
//   PlayReleaseMouse          -- `IsKeyPressed(ImGuiKey_F1,false) && io.KeyShift` (Ctrl unchecked)
//   PlayStop                  -- `IsKeyPressed(ImGuiKey_Escape,false)`, gated on a play session
//   DrawerDismiss             -- `IsKeyPressed(ImGuiKey_Escape,false)`, gated on a drawer being open
//   DrawerToggleContent       -- `io.KeyCtrl && IsKeyPressed(ImGuiKey_Space,false)`
//   DrawerToggleConsole       -- NEW command, not a transcription: the Console tab has no prior
//                                hardcoded site. Backtick/grave, unmodified -- the Source/Quake/
//                                Unreal console convention, distinct from Ctrl+Space so the two can't
//                                collide.
//   EditDelete                -- `IsKeyPressed(ImGuiKey_Delete,false)`, no modifier gating at all
//   EditUndo                  -- `io.KeyCtrl && IsKeyPressed(ImGuiKey_Z,false) && !io.KeyShift`
//   EditRedo                  -- `io.KeyCtrl && IsKeyPressed(ImGuiKey_Y,false)` (Shift unchecked)
//   EditCopy/Paste/Duplicate  -- new commands; Ctrl+C/V/D, exact-modifier by design (see header)
//   AssetSave                 -- the SAME Ctrl+S transcribed from four places at once:
//                                AnimEditor (sockets and clips), BtEditor and GraphEditor all
//                                wrote `IsWindowFocused(RootAndChildWindows) && io.KeyCtrl &&
//                                IsKeyPressed(S,false)`. checkShift is FALSE because not one of
//                                them gated on Shift -- Ctrl+Shift+S saves today, and this table
//                                transcribes behaviour rather than tidying it. The focus test
//                                stays at each call site: pressed() does not consult live scope.
//   LevelSave                 -- Ctrl+S over the LEVEL, a different command from AssetSave and
//                                deliberately a separate row: its scopes do not overlap (one is
//                                the viewport, the other an asset tab), and its gating differs --
//                                it tests WantTextInput rather than window focus, because saving
//                                is not a viewport gesture but renaming an entity must not save
//                                the level on the "s" of a name.
//   SaveAll                   -- NEW command (2026-09-16): Ctrl+Shift+S saves the level AND every dirty
//                                asset tab (AssetEditorHost::saveAllDirty, which until then ran only
//                                from the quit prompt). It takes Ctrl+Shift+S from LevelSave and
//                                AssetSave, which used to fire on it because neither checked Shift:
//                                both now check Shift, so plain Ctrl+S is unchanged and the shifted
//                                chord means the superset instead of a second, partial save.
//   CompileScripts/ReloadScripts -- NEW commands (2026-09-16): the two most-fired actions of the C#
//                                loop had no chord. Ctrl+Shift+B (Visual Studio's Build) and
//                                Ctrl+Shift+R.
//   Screenshot                -- NEW command (2026-09-16): F9 saves a PNG of the 3D viewport. Also in
//                                the play-session scope, since a running game is the likeliest subject.
//   SnapToFloor..NudgeDown    -- NEW commands (2026-09-16), the viewport placement verbs, object mode
//                                only. End drops the selection onto what is below it. H hides it,
//                                Shift+H hides everything else, Ctrl+H brings back what those hid --
//                                exact modifiers, since the three share a key. Arrows and PageUp/Down
//                                nudge by the move-snap step, repeating while held.
//   EditSelectAll             -- Ctrl+A over the OUTLINER's drawn order. It shipped as a menu item
//                                whose "Ctrl+A" hint was a hardcoded string with no key behind it,
//                                so the menu advertised a shortcut that did nothing. The Content
//                                Browser's own Ctrl+A (cbShortcuts) stays hardcoded and out of this
//                                table on purpose: it selects FILES, and is gated on that panel
//                                holding focus -- two meanings on one key, exactly as Delete has.
constexpr u32 kViewportScope = kScopeObjectMode | kScopeLandscapeMode;
// THE SAME COMMAND, IN THE CANVAS TOO. Delete/Undo/Redo/Copy/Paste/Duplicate and Frame Selected
// were hardcoded a second time inside GraphEditor, so rebinding Copy on the Preferences page
// changed it everywhere EXCEPT the place a node author spends their day. Widening the scope is
// right where adding parallel commands would be wrong: there is one "Copy", and the user
// rebinds it once.
constexpr u32 kEditScope = kViewportScope | kScopeGraphEditor;
// The actor editor has its own viewport with the SAME four tools, hardcoded a second time.
constexpr u32 kToolScope = kScopeObjectMode | kScopeActorEditor;
// Commands that mean the same thing wherever the user is -- the level, any asset tab -- so conflict
// detection compares them against every scope a chord could collide in.
constexpr u32 kAnywhereScope = kViewportScope | kScopeGraphEditor | kScopeActorEditor | kScopeAssetEditor;
constexpr std::array<KeybindDef, kCommandCount> kDefs = {{
    {CommandId::ToolSelect,  "tool.select",  "Select Tool",           {ImGuiKey_1, false,false,false}, kToolScope,    false,false, true},
    {CommandId::ToolMove,    "tool.move",    "Move Tool",             {ImGuiKey_2, false,false,false}, kToolScope,    false,false, true},
    {CommandId::ToolRotate,  "tool.rotate",  "Rotate Tool",           {ImGuiKey_3, false,false,false}, kToolScope,    false,false, true},
    {CommandId::ToolScale,   "tool.scale",   "Scale Tool",            {ImGuiKey_4, false,false,false}, kToolScope,    false,false, true},
    {CommandId::SculptRaise,   "sculpt.raise",   "Sculpt: Raise",     {ImGuiKey_1, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::SculptLower,   "sculpt.lower",   "Sculpt: Lower",     {ImGuiKey_2, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::SculptSmooth,  "sculpt.smooth",  "Sculpt: Smooth",    {ImGuiKey_3, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::SculptFlatten, "sculpt.flatten", "Sculpt: Flatten",   {ImGuiKey_4, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::ModeToggleLandscape, "mode.toggleLandscape", "Toggle Landscape Mode", {ImGuiKey_Tab, false,false,false}, kViewportScope, false,false, true},
    {CommandId::ViewFrameSelected,   "view.frameSelected",   "Frame Selected",        {ImGuiKey_F,   false,false,false}, kEditScope | kScopeActorEditor, false,false, true},
    {CommandId::PlayStart,        "play.start",        "Play",                 {ImGuiKey_P,      false,false,true }, kViewportScope,    true, true,  false},
    {CommandId::PlayReleaseMouse, "play.releaseMouse", "Release Mouse (Play)", {ImGuiKey_F1,     false,true, false}, kScopePlaySession, false,true,  false},
    {CommandId::PlayStop,         "play.stop",         "Stop Play Session",    {ImGuiKey_Escape, false,false,false}, kScopePlaySession, false,false, false},
    {CommandId::DrawerDismiss,       "drawer.dismiss",       "Dismiss Drawer",          {ImGuiKey_Escape, false,false,false}, kScopeDrawerOpen, false,false, false},
    {CommandId::DrawerToggleContent, "drawer.toggleContent", "Toggle Content Browser",  {ImGuiKey_Space,  true, false,false}, kScopeGlobalUI,   true, false, false},
    {CommandId::DrawerToggleConsole, "drawer.toggleConsole", "Toggle Console",          {ImGuiKey_GraveAccent, false,false,false}, kScopeGlobalUI, false,false, false},
    {CommandId::EditDelete, "edit.delete", "Delete",    {ImGuiKey_Delete, false,false,false}, kEditScope, false,false, false},
    {CommandId::EditUndo,   "edit.undo",   "Undo",      {ImGuiKey_Z,      true, false,false}, kEditScope | kScopeAssetEditor, true, true,  false},
    {CommandId::EditRedo,   "edit.redo",   "Redo",      {ImGuiKey_Y,      true, false,false}, kEditScope | kScopeAssetEditor, true, false, false},
    {CommandId::EditCopy,      "edit.copy",      "Copy",      {ImGuiKey_C, true,false,false}, kEditScope, true, true, false},
    {CommandId::EditPaste,     "edit.paste",     "Paste",     {ImGuiKey_V, true,false,false}, kEditScope, true, true, false},
    {CommandId::EditDuplicate, "edit.duplicate", "Duplicate", {ImGuiKey_D, true,false,false}, kEditScope, true, true, false},
    {CommandId::EditSelectAll, "edit.selectAll", "Select All", {ImGuiKey_A, true,false,false}, kViewportScope, true, true, false},
    {CommandId::AssetSave,     "asset.save",     "Save Asset", {ImGuiKey_S, true,false,false}, kScopeAssetEditor, true, true, false},
    {CommandId::LevelSave,     "level.save",     "Save Level",            {ImGuiKey_S, true,false,false}, kViewportScope, true, true, false},
    {CommandId::SaveAll,       "file.saveAll",   "Save All",              {ImGuiKey_S, true,true, false}, kAnywhereScope, true, true, false},
    {CommandId::GraphCommentBox, "graph.commentBox", "Graph: Comment Box", {ImGuiKey_C, false,false,false}, kScopeGraphEditor, true, false, false},
    {CommandId::GraphFrameAll,   "graph.frameAll",   "Graph: Frame All",             {ImGuiKey_Home, false,false,false}, kScopeGraphEditor, false,false, false},
    {CommandId::CompileScripts,  "scripts.compile",  "Compile Scripts",   {ImGuiKey_B,  true, true, false}, kAnywhereScope, true, true, false},
    {CommandId::ReloadScripts,   "scripts.reload",   "Reload Scripts",    {ImGuiKey_R,  true, true, false}, kAnywhereScope, true, true, false},
    {CommandId::Screenshot,      "view.screenshot",  "Screenshot Viewport", {ImGuiKey_F9, false,false,false}, kAnywhereScope | kScopePlaySession, true, true, false},
    {CommandId::SnapToFloor,     "edit.snapToFloor",     "Snap to Floor",       {ImGuiKey_End,       false,false,false}, kScopeObjectMode, true, true, false},
    {CommandId::HideSelected,    "view.hideSelected",    "Hide Selected",       {ImGuiKey_H,         false,false,false}, kScopeObjectMode, true, true, false},
    {CommandId::IsolateSelected, "view.isolateSelected", "Hide Unselected",     {ImGuiKey_H,         false,true, false}, kScopeObjectMode, true, true, false},
    {CommandId::UnhideAll,       "view.unhideAll",       "Unhide All",          {ImGuiKey_H,         true, false,false}, kScopeObjectMode, true, true, false},
    {CommandId::NudgeLeft,       "edit.nudgeLeft",       "Nudge Left",          {ImGuiKey_LeftArrow, false,false,false}, kScopeObjectMode, true, true, true},
    {CommandId::NudgeRight,      "edit.nudgeRight",      "Nudge Right",         {ImGuiKey_RightArrow,false,false,false}, kScopeObjectMode, true, true, true},
    {CommandId::NudgeForward,    "edit.nudgeForward",    "Nudge Forward",       {ImGuiKey_UpArrow,   false,false,false}, kScopeObjectMode, true, true, true},
    {CommandId::NudgeBack,       "edit.nudgeBack",       "Nudge Back",          {ImGuiKey_DownArrow, false,false,false}, kScopeObjectMode, true, true, true},
    {CommandId::NudgeUp,         "edit.nudgeUp",         "Nudge Up",            {ImGuiKey_PageUp,    false,false,false}, kScopeObjectMode, true, true, true},
    {CommandId::NudgeDown,       "edit.nudgeDown",       "Nudge Down",          {ImGuiKey_PageDown,  false,false,false}, kScopeObjectMode, true, true, true},
}};

static_assert(kDefs.size() == kCommandCount, "kDefs must have exactly one row per CommandId");

} // namespace

// Lets a test walk the whole vocabulary instead of duplicating it. Without this the codec sweep
// would assert against a copy of the table, which is the one thing it must not do: a copy agrees
// with itself by construction, including about a name it got wrong.
usize keyNameCount() { return sizeof(kKeyNames) / sizeof(kKeyNames[0]); }
const char* keyNameAt(usize i) { return i < keyNameCount() ? kKeyNames[i].name : nullptr; }

// One instance, reached by everything. A function-local static rather than a namespace-scope object
// so its construction order cannot race the preference store it reads from in loadFromPrefs().
KeybindRegistry& keybinds() {
    static KeybindRegistry r;
    return r;
}

const std::array<KeybindDef, kCommandCount>& keybindDefs() { return kDefs; }
const KeybindDef& keybindDef(CommandId id) { return kDefs[static_cast<usize>(id)]; }

std::string chordToString(const Chord& c) {
    if (!c.isBound()) return "none";
    const char* kn = keyName(c.key);
    if (!kn) return "none";
    std::string out;
    if (c.ctrl)  out += "Ctrl+";
    if (c.shift) out += "Shift+";
    if (c.alt)   out += "Alt+";
    out += kn;
    return out;
}

Chord parseChord(const std::string& text) {
    // "(unbound)" is accepted but never WRITTEN: the header comment claimed for a long time that
    // this was the rendering, so a file hand-edited by someone following it must still load rather
    // than silently reverting that command to its default.
    if (text.empty() || text == "none" || text == "(unbound)") return Chord{};
    std::vector<std::string_view> tokens;
    std::string_view sv(text);
    usize pos = 0;
    while (true) {
        const usize next = sv.find('+', pos);
        tokens.push_back(next == std::string_view::npos ? sv.substr(pos) : sv.substr(pos, next - pos));
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    Chord c;
    for (usize i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "Ctrl") c.ctrl = true;
        else if (tokens[i] == "Shift") c.shift = true;
        else if (tokens[i] == "Alt") c.alt = true;
    }
    c.key = nameToKey(tokens.back());
    if (c.key == ImGuiKey_None) return Chord{};   // unrecognised text -> unbound, not a bad partial
    return c;
}

KeybindRegistry::KeybindRegistry() {
    for (usize i = 0; i < kCommandCount; ++i) current_[i] = kDefs[i].def;
}

CommandId KeybindRegistry::conflictWith(CommandId id, const Chord& chord, u32 scope) const {
    if (!chord.isBound()) return CommandId::Count;
    for (usize i = 0; i < kCommandCount; ++i) {
        const CommandId other = static_cast<CommandId>(i);
        if (other == id) continue;
        if (!(kDefs[i].scope & scope)) continue;
        if (current_[i] == chord) return other;
    }
    return CommandId::Count;
}

bool KeybindRegistry::rebind(CommandId id, const Chord& chord) {
    if (!chord.isBound()) { current_[static_cast<usize>(id)] = chord; return true; }
    if (conflictWith(id, chord, keybindDef(id).scope) != CommandId::Count) return false;
    current_[static_cast<usize>(id)] = chord;
    return true;
}

void KeybindRegistry::resetToDefault(CommandId id) {
    current_[static_cast<usize>(id)] = keybindDef(id).def;
}

bool KeybindRegistry::pressed(CommandId id, const ImGuiIO& io) const {
    const KeybindDef& def = keybindDef(id);
    const Chord& c = current_[static_cast<usize>(id)];
    if (!c.isBound()) return false;
    if (def.checkCtrl  && io.KeyCtrl  != c.ctrl)  return false;
    if (def.checkShift && io.KeyShift != c.shift) return false;
    // ALT ALWAYS MATCHES. The capture loop records Alt, editor.ini persists it and conflictWith() treats
    // Alt+G and G as different chords -- but this test used to ignore Alt, so a command rebound to Alt+G
    // (typically to dodge a conflict with G) fired on bare G too, while the conflict check said the two
    // were distinct. No default carries Alt, and no original site checked it, so the only defaults this
    // changes are "the key while Alt happens to be held" (e.g. Alt+Tab no longer toggles Landscape mode).
    if (io.KeyAlt != c.alt) return false;
    return ImGui::IsKeyPressed(c.key, def.repeatAllowed);
}

void KeybindRegistry::loadFromPrefs() {
    for (usize i = 0; i < kCommandCount; ++i) {
        const std::string stored = prefString(std::string("keybind.") + kDefs[i].strId, "");
        current_[i] = stored.empty() ? kDefs[i].def : parseChord(stored);
    }
}

void KeybindRegistry::saveToPrefs() const {
    for (usize i = 0; i < kCommandCount; ++i)
        setPrefString(std::string("keybind.") + kDefs[i].strId, chordToString(current_[i]));
}

bool KeybindRegistry::drawPreferencesSection(f32 dpi, bool visible, UiRegistry* reg) {
    // CANCEL A CAPTURE THAT CAN NO LONGER BE COMPLETED. When the caller's header is collapsed
    // this function stops being called at all, so the capture loop at the bottom -- the only thing
    // that ever clears listening_ -- never runs again. The row was then stuck reading
    // "Press a chord..." on reopen, with no way out but restarting the editor.
    if (!visible) { listening_ = -1; conflictLabel_.clear(); return false; }

    bool changed = false;
    ImGui::TextDisabled("Click Rebind, then press the new chord. Esc cancels the capture; "
                        "Clear unbinds it.");

    // Reset All. Per-row Reset has always been here; putting every binding back was fifteen
    // clicks. Disabled when nothing differs from the defaults, so it never claims to undo
    // changes that were not made.
    bool anyChanged = false;
    for (usize i = 0; i < kCommandCount; ++i)
        if (current_[i] != kDefs[i].def) { anyChanged = true; break; }
    ImGui::BeginDisabled(!anyChanged);
    if (ImGui::SmallButton("Reset All")) {
        for (usize i = 0; i < kCommandCount; ++i) resetToDefault(kDefs[i].id);
        changed = true;
    }
    if (reg) reg->track("prefs.keybind.resetAll");
    ImGui::EndDisabled();
    if (!anyChanged && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Every binding is already at its default.");

    if (ImGui::BeginTable("keybindsTable", 4,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        // 120, not 150. Widening the button column below to fit Clear AND Reset took that space
        // out of the stretching Command column, which started truncating labels ("Release Mous",
        // "Toggle Consol"). The longest chord this can hold is "Ctrl+Shift+KeypadSubtract", which
        // no default uses and which wraps rather than being lost.
        ImGui::TableSetupColumn("Chord", ImGuiTableColumnFlags_WidthFixed, 120.0f * dpi);
        ImGui::TableSetupColumn("##rebind", ImGuiTableColumnFlags_WidthFixed, 84.0f * dpi);
        // WIDE ENOUGH FOR TWO BUTTONS, because this column holds Clear AND Reset side by side. It
        // was still sized for the one it held before Clear was added, so Reset rendered clipped to
        // "Res" -- visible in any screenshot of this page, and easy to miss precisely because a
        // clipped button still looks like a button.
        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 176.0f * dpi);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < kCommandCount; ++i) {
            const KeybindDef& def = kDefs[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(def.label);
            // The Command column stretches to whatever the Preferences window leaves it, so the two
            // longest labels clip at the default size. A hover tooltip makes that harmless rather
            // than chasing column widths that only hold at one window size and one DPI.
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", def.label);
            ImGui::TableSetColumnIndex(1);
            if (listening_ == static_cast<int>(i))
                ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.13f, 1.0f), "Press a chord...");
            else
                ImGui::TextUnformatted(chordToString(current_[i]).c_str());
            ImGui::TableSetColumnIndex(2);
            if (listening_ == static_cast<int>(i)) {
                if (ImGui::SmallButton("Cancel")) listening_ = -1;
                if (reg) reg->track(trackName(def.strId, ".cancel").c_str());
            } else {
                if (ImGui::SmallButton("Rebind")) { listening_ = static_cast<int>(i); conflictLabel_.clear(); }
                if (reg) reg->track(trackName(def.strId, ".rebind").c_str());
            }
            ImGui::TableSetColumnIndex(3);
            // CLEAR, which rebind() has always supported and nothing could reach. The header says
            // outright that "an unbound chord always succeeds and clears the command instead", and
            // the conflict message below this table tells the user to "clear it first" -- but the
            // capture loop can only ever produce a real key, and Escape cancels. So the one
            // instruction the UI gives for resolving a conflict named a control that did not exist.
            ImGui::BeginDisabled(!current_[i].isBound());
            if (ImGui::SmallButton("Clear")) { rebind(def.id, Chord{}); changed = true; }
            if (reg) reg->track(trackName(def.strId, ".clear").c_str());
            ImGui::EndDisabled();
            if (!current_[i].isBound() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Already unbound.");
            ImGui::SameLine();
            ImGui::BeginDisabled(current_[i] == def.def);
            if (ImGui::SmallButton("Reset")) { resetToDefault(def.id); changed = true; }
            if (reg) reg->track(trackName(def.strId, ".reset").c_str());
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // The capture step: poll the allow-list for whatever the user pressed this frame. Scoped to
    // this call (drawn only while the Preferences window is open), so it can never steal a keypress
    // some other part of the editor needs the same frame.
    if (listening_ >= 0) {
        const ImGuiIO& io = ImGui::GetIO();
        const CommandId id = static_cast<CommandId>(listening_);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            listening_ = -1;   // Escape always cancels; it is therefore never assignable by capture
        } else {
            for (const KeyName& kn : kKeyNames) {
                if (kn.key == ImGuiKey_Escape) continue;
                if (!ImGui::IsKeyPressed(kn.key, false)) continue;
                const Chord candidate{kn.key, io.KeyCtrl, io.KeyShift, io.KeyAlt};
                if (rebind(id, candidate)) {
                    changed = true;
                    conflictLabel_.clear();
                } else {
                    const CommandId c = conflictWith(id, candidate, keybindDef(id).scope);
                    conflictLabel_ = (c != CommandId::Count) ? keybindDef(c).label : std::string();
                }
                listening_ = -1;
                break;
            }
        }
    }
    if (!conflictLabel_.empty())
        ImGui::TextColored(ImVec4(0.9f, 0.25f, 0.25f, 1.0f),
                           "That chord is already used by \"%s\" in an overlapping context -- "
                           "clear it first or pick another.", conflictLabel_.c_str());

    return changed;
}

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
