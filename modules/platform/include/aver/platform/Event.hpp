#pragma once
#include "aver/core/Types.hpp"

namespace aver {

// The kind of a window/input event.
enum class EventType {
    None,
    WindowClose,
    WindowResize,
    Key,
    MouseMove,
    MouseButton,
    MouseWheel,
    // The window lost keyboard focus. Whoever accumulates key state MUST clear it here: Windows
    // sends no WM_KEYUP for a key that was held when focus left, so without this a key held during
    // an Alt+Tab stays held forever and the pawn walks into a wall until the user presses and
    // releases it again.
    FocusLost,
    // A finger on a touch screen (only where the window layer has real touch: SDL). `finger` is a
    // stable small index while the finger stays down, `pressed` true for down and moves, false for up,
    // mouseX/mouseY the position in window pixels, `pressure` 0..1.
    Touch,
    // A typed character (E16): `codepoint` is one Unicode scalar value, already through the keyboard layout, dead keys and the IME's committed
    // text, never a control character. Auto-repeated while the key is held, like the key events. Key events still report the key; this reports the text.
    Char,
};

// One window or input event. Which fields are meaningful depends on `type`.
struct Event {
    EventType type = EventType::None;
    u32 width = 0, height = 0;
    i32 key = 0;              // virtual key code
    bool pressed = false;
    i32 mouseX = 0, mouseY = 0;
    i32 button = -1; // 0=L,1=R,2=M,3=X1,4=X2
    // MouseWheel only. Notches, already divided by WHEEL_DELTA, so one detent is 1.0 and the sign
    // follows the OS convention (positive = away from the user).
    f32 wheel = 0.0f;
    // MouseWheel only: the horizontal notches, positive = right; 0 on a vertical-only event.
    f32 wheelX = 0.0f;
    // Touch only.
    i32 finger = 0;
    f32 pressure = 1.0f;
    // Char only: the code point.
    u32 codepoint = 0;
    // Key and Char: true for an auto-repeat of a key that was already down (E16; Win32 reports it, SDL builds set it from the OS repeat flag).
    bool repeat = false;
};

// Event sink, set on the window and called from pumpEvents.
using EventCallback = void (*)(void* user, const Event&);

} // namespace aver
