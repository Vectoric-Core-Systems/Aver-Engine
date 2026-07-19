#pragma once
#include "aver/core/Types.hpp"

namespace aver {

enum class EventType {
    None,
    WindowClose,
    WindowResize,
    Key,
    MouseMove,
    MouseButton,
};

struct Event {
    EventType type = EventType::None;
    // WindowResize
    u32 width = 0, height = 0;
    // Key: key = virtual key code; pressed = down/up
    i32 key = 0;
    bool pressed = false;
    // Mouse
    i32 mouseX = 0, mouseY = 0;
    i32 button = -1; // 0=L,1=R,2=M
};

// Lightweight callback (no std::function) — set on the window; called from pumpEvents.
using EventCallback = void (*)(void* user, const Event&);

} // namespace aver
