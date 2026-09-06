// See EditorKeybinds.hpp for what this is. The one thing worth restating here: every `def` row
// below is a transcription of an ACTUAL call site in SandboxApp.cpp as it stood before this file
// existed (cited per row), not a guess at what seemed reasonable -- that is what lets "existing
// hardcoded keys become defaults" mean current behaviour is unchanged for anyone who never opens
// the Keybinds page.
#include "EditorKeybinds.hpp"
#if AVER_WITH_IMGUI
#include "EditorPrefs.hpp"

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
};

const char* keyName(ImGuiKey k) {
    for (const KeyName& kn : kKeyNames) if (kn.key == k) return kn.name;
    return nullptr;
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
//   EditSelectAll             -- Ctrl+A over the OUTLINER's drawn order. It shipped as a menu item
//                                whose "Ctrl+A" hint was a hardcoded string with no key behind it,
//                                so the menu advertised a shortcut that did nothing. The Content
//                                Browser's own Ctrl+A (cbShortcuts) stays hardcoded and out of this
//                                table on purpose: it selects FILES, and is gated on that panel
//                                holding focus -- two meanings on one key, exactly as Delete has.
constexpr u32 kViewportScope = kScopeObjectMode | kScopeLandscapeMode;
constexpr std::array<KeybindDef, kCommandCount> kDefs = {{
    {CommandId::ToolSelect,  "tool.select",  "Select Tool",           {ImGuiKey_1, false,false,false}, kScopeObjectMode,    false,false, true},
    {CommandId::ToolMove,    "tool.move",    "Move Tool",             {ImGuiKey_2, false,false,false}, kScopeObjectMode,    false,false, true},
    {CommandId::ToolRotate,  "tool.rotate",  "Rotate Tool",           {ImGuiKey_3, false,false,false}, kScopeObjectMode,    false,false, true},
    {CommandId::ToolScale,   "tool.scale",   "Scale Tool",            {ImGuiKey_4, false,false,false}, kScopeObjectMode,    false,false, true},
    {CommandId::SculptRaise,   "sculpt.raise",   "Sculpt: Raise",     {ImGuiKey_1, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::SculptLower,   "sculpt.lower",   "Sculpt: Lower",     {ImGuiKey_2, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::SculptSmooth,  "sculpt.smooth",  "Sculpt: Smooth",    {ImGuiKey_3, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::SculptFlatten, "sculpt.flatten", "Sculpt: Flatten",   {ImGuiKey_4, false,false,false}, kScopeLandscapeMode, false,false, true},
    {CommandId::ModeToggleLandscape, "mode.toggleLandscape", "Toggle Landscape Mode", {ImGuiKey_Tab, false,false,false}, kViewportScope, false,false, true},
    {CommandId::ViewFrameSelected,   "view.frameSelected",   "Frame Selected",        {ImGuiKey_F,   false,false,false}, kViewportScope, false,false, true},
    {CommandId::PlayReleaseMouse, "play.releaseMouse", "Release Mouse (Play)", {ImGuiKey_F1,     false,true, false}, kScopePlaySession, false,true,  false},
    {CommandId::PlayStop,         "play.stop",         "Stop Play Session",    {ImGuiKey_Escape, false,false,false}, kScopePlaySession, false,false, false},
    {CommandId::DrawerDismiss,       "drawer.dismiss",       "Dismiss Drawer",          {ImGuiKey_Escape, false,false,false}, kScopeDrawerOpen, false,false, false},
    {CommandId::DrawerToggleContent, "drawer.toggleContent", "Toggle Content Browser",  {ImGuiKey_Space,  true, false,false}, kScopeGlobalUI,   true, false, false},
    {CommandId::DrawerToggleConsole, "drawer.toggleConsole", "Toggle Console",          {ImGuiKey_GraveAccent, false,false,false}, kScopeGlobalUI, false,false, false},
    {CommandId::EditDelete, "edit.delete", "Delete",    {ImGuiKey_Delete, false,false,false}, kViewportScope, false,false, false},
    {CommandId::EditUndo,   "edit.undo",   "Undo",      {ImGuiKey_Z,      true, false,false}, kViewportScope, true, true,  false},
    {CommandId::EditRedo,   "edit.redo",   "Redo",      {ImGuiKey_Y,      true, false,false}, kViewportScope, true, false, false},
    {CommandId::EditCopy,      "edit.copy",      "Copy",      {ImGuiKey_C, true,false,false}, kViewportScope, true, true, false},
    {CommandId::EditPaste,     "edit.paste",     "Paste",     {ImGuiKey_V, true,false,false}, kViewportScope, true, true, false},
    {CommandId::EditDuplicate, "edit.duplicate", "Duplicate", {ImGuiKey_D, true,false,false}, kViewportScope, true, true, false},
    {CommandId::EditSelectAll, "edit.selectAll", "Select All", {ImGuiKey_A, true,false,false}, kViewportScope, true, true, false},
}};

static_assert(kDefs.size() == kCommandCount, "kDefs must have exactly one row per CommandId");

} // namespace

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
    if (text.empty() || text == "none") return Chord{};
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

bool KeybindRegistry::drawPreferencesSection(f32 dpi) {
    bool changed = false;
    ImGui::TextDisabled("Click Rebind, then press the new chord. Esc cancels the capture.");

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
    ImGui::EndDisabled();
    if (!anyChanged && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Every binding is already at its default.");

    if (ImGui::BeginTable("keybindsTable", 4,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Chord", ImGuiTableColumnFlags_WidthFixed, 150.0f * dpi);
        ImGui::TableSetupColumn("##rebind", ImGuiTableColumnFlags_WidthFixed, 84.0f * dpi);
        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 84.0f * dpi);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < kCommandCount; ++i) {
            const KeybindDef& def = kDefs[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(def.label);
            ImGui::TableSetColumnIndex(1);
            if (listening_ == static_cast<int>(i))
                ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.13f, 1.0f), "Press a chord...");
            else
                ImGui::TextUnformatted(chordToString(current_[i]).c_str());
            ImGui::TableSetColumnIndex(2);
            if (listening_ == static_cast<int>(i)) {
                if (ImGui::SmallButton("Cancel")) listening_ = -1;
            } else {
                if (ImGui::SmallButton("Rebind")) { listening_ = static_cast<int>(i); conflictLabel_.clear(); }
            }
            ImGui::TableSetColumnIndex(3);
            // CLEAR, which rebind() has always supported and nothing could reach. The header says
            // outright that "an unbound chord always succeeds and clears the command instead", and
            // the conflict message below this table tells the user to "clear it first" -- but the
            // capture loop can only ever produce a real key, and Escape cancels. So the one
            // instruction the UI gives for resolving a conflict named a control that did not exist.
            ImGui::BeginDisabled(!current_[i].isBound());
            if (ImGui::SmallButton("Clear")) { rebind(def.id, Chord{}); changed = true; }
            ImGui::EndDisabled();
            if (!current_[i].isBound() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Already unbound.");
            ImGui::SameLine();
            ImGui::BeginDisabled(current_[i] == def.def);
            if (ImGui::SmallButton("Reset")) { resetToDefault(def.id); changed = true; }
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
