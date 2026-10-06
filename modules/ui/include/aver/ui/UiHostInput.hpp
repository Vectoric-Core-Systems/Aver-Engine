#pragma once
// Turns raw device state (Win32 virtual keys, gamepad) into the UI's navigation, editing and text
// input, with key repeat. Pure logic with no platform dependency, so it is testable and every host
// (the standalone runtime, the editor's Play mode) maps input the same way.
//
// Typed text: the platform window does not forward character messages yet, so letters, digits and
// punctuation come from key presses on a US layout. A window that forwards WM_CHAR should feed
// aver_ui_input_char instead and leave the character table here unused.
#include "aver/ui/UiTree.hpp"

namespace aver::ui {

// One frame of raw device state. Indices are Win32 virtual-key codes; the gamepad arrays follow
// AVER_FW_GAMEPAD_* (buttons) and AVER_FW_GAMEPAD_AXIS_* (axes), with sticks up-positive.
struct UiHostSnapshot {
    bool keyPressed[256] = {};   // went down this frame
    bool keyHeld[256] = {};
    bool padConnected = false;
    bool padButton[14] = {};
    f32  padAxis[6] = {};
};

class UiHostInputMapper {
public:
    // Adds this frame's nav bits, typed characters and edit keys to `out` and returns the lowest
    // virtual key pressed this frame (for rebinding capture), or -1.
    i32 map(const UiHostSnapshot& s, f32 dt, UiInputFrame& out);

    static constexpr f32 kRepeatDelay = 0.40f;     // seconds a key is held before it repeats
    static constexpr f32 kRepeatInterval = 0.06f;  // seconds between repeats
    static constexpr f32 kStickThreshold = 0.60f;  // stick deflection that counts as a direction

private:
    struct Repeater {
        f32 t = 0, next = 0;
        bool prev = false;
        // True on the press and on each repeat while held.
        bool step(bool pressedEdge, bool held, f32 dt);
        // True once per press, never on repeat.
        bool press(bool held);
    };
    Repeater key_[256];
    Repeater pad_[14];
    Repeater stick_[4];   // left, right, up, down
};

// The character a key types on a US layout, or 0.
u32 uiVkToChar(i32 vk, bool shift);

} // namespace aver::ui
