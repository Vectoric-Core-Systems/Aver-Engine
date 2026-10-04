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
};

// One window or input event. Which fields are meaningful depends on `type`.
struct Event {
    EventType type = EventType::None;
    u32 width = 0, height = 0;
    i32 key = 0;              // virtual key code
    bool pressed = false;
    i32 mouseX = 0, mouseY = 0;
    i32 button = -1; // 0=L,1=R,2=M
    // MouseWheel only. Notches, already divided by WHEEL_DELTA, so one detent is 1.0 and the sign
    // follows the OS convention (positive = away from the user).
    f32 wheel = 0.0f;
};

// Event sink, set on the window and called from pumpEvents.
using EventCallback = void (*)(void* user, const Event&);

} // namespace aver
