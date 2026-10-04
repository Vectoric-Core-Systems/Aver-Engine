#pragma once
// The exact enum-member-name vocabulary for Aver.Framework's Key, GamepadButton and GamepadAxis
// enums (scripting/csharp/Aver.Framework/Input.cs), as C++ string literals, in the SAME order those
// enums declare them. The Input Scheme editor's key-name combos (InputSchemeEditor.cpp) offer
// EXACTLY these names, so nothing a designer can pick there is a name EnhancedInput.cs's own
// Enum.TryParse + Enum.IsDefined round trip (TryParseRawKey) would refuse to load back.
//
// A HAND-COPIED LIST, DELIBERATELY -- the same choice OcInput.hpp's own header comment makes for
// BIND's key token ("KEY NAMES ARE OPAQUE STRINGS, NOT A VALIDATED ENUM"): C++ has no reflection
// over a C# enum, and the framework's C ABI hands back an already-RESOLVED binding, never the
// enum's own member list -- so this table is the one place that dependency is paid, by a human
// keeping it in sync with Input.cs by eye. If the two ever drift, the symptom is narrow and
// visible: a name this file offers that Input.cs's Enum.TryParse rejects at load time (logged and
// skipped, never a corrupted binding -- see InputScheme.Load's own contract), not silent breakage
// of an existing one.
//
// INPUT.CS IS THE SOURCE OF TRUTH. Re-copy these three lists, in order, whenever that file's Key/
// GamepadButton/GamepadAxis enums change.
#include <cstddef>

namespace aver::editor {

// scripting/csharp/Aver.Framework/Input.cs -- enum Key. A..Z (0-25), D0..D9 (26-35), Space..Tab
// (36-42), Left/Right/Up/Down/MouseLeft/MouseRight/MouseMiddle (43-49). 50 members.
inline constexpr const char* kFwKeyNames[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9",
    "Space", "LeftShift", "LeftCtrl", "LeftAlt", "Enter", "Escape", "Tab",
    "Left", "Right", "Up", "Down", "MouseLeft", "MouseRight", "MouseMiddle",
};
inline constexpr std::size_t kFwKeyNameCount = sizeof(kFwKeyNames) / sizeof(kFwKeyNames[0]);

// Input.cs -- enum GamepadButton. 14 members.
inline constexpr const char* kFwGamepadButtonNames[] = {
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Start", "Back",
    "LeftThumb", "RightThumb", "LeftShoulder", "RightShoulder", "A", "B", "X", "Y",
};
inline constexpr std::size_t kFwGamepadButtonNameCount =
    sizeof(kFwGamepadButtonNames) / sizeof(kFwGamepadButtonNames[0]);

// Input.cs -- enum GamepadAxis. 6 members.
inline constexpr const char* kFwGamepadAxisNames[] = {
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger",
};
inline constexpr std::size_t kFwGamepadAxisNameCount =
    sizeof(kFwGamepadAxisNames) / sizeof(kFwGamepadAxisNames[0]);

} // namespace aver::editor
