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
};

// One window or input event. Which fields are meaningful depends on `type`.
struct Event {
    EventType type = EventType::None;
    u32 width = 0, height = 0;
    i32 key = 0;              // virtual key code
    bool pressed = false;
    i32 mouseX = 0, mouseY = 0;
    i32 button = -1; // 0=L,1=R,2=M
};

// Event sink, set on the window and called from pumpEvents.
using EventCallback = void (*)(void* user, const Event&);

} // namespace aver
