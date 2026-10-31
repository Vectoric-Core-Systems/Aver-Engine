// The shipped game's side of the framework's pointer, window and present requests (framework_abi.h,
// POINTER AND CURSOR / WINDOW AND DISPLAY, minor 8). A script asks; GameApp calls these once a frame
// to make it so and to publish what is true. The editor owns its window, cursor and vsync and never
// calls them.
#pragma once
#include "aver/core/Types.hpp"

namespace aver {
class Window;
namespace rhi { class IDevice; }
}

namespace aver::game {

#if AVER_MODULE_FRAMEWORK
// What the pointer should be this frame: held by the game (captured -- hidden, relative motion),
// hidden over the window, confined to it.
struct PointerPlan {
    bool captured = false;
    bool hidden = false;
    bool confined = false;
};

// An AVER_FW_POINTER_* mode against the host's own answer. AUTO is `autoCapture` unchanged (today's rule:
// captured while a session plays). Every other mode replaces it. `cursorFreed` (something asked for a
// free cursor, or an open menu needs one) takes the pointer off the game in every mode: nothing is
// captured, hidden or confined then.
PointerPlan resolvePointerPlan(i32 mode, bool autoCapture, bool cursorFreed);

// The present mode the device ends up with for a wanted AVER_FW_PRESENT_* value: the mode itself where the
// device has it, else the nearest it has (Mailbox to Immediate to Fifo, Adaptive and Immediate to Fifo).
// Returns an AVER_FW_PRESENT_* value.
i32 nearestPresentMode(const rhi::IDevice& dev, i32 wanted);

// Takes a waiting present-mode request and applies it, then publishes the mode in force.
void applyPresentMode(rhi::IDevice& dev);

// Takes the window requests (flags, mode, size, position, title) and applies them, then publishes the
// window's state.
void applyWindowRequests(Window& w);

// Remembers what the window's cursor already shows so each frame touches it only on a change.
class CursorHost {
public:
    // The cursor's plan (hidden, confined), shape and images, and a pending warp.
    void apply(Window& w, const PointerPlan& plan);
private:
    i32 imageSerial_ = 0;
    i32 shape_ = 0;
    bool hidden_ = false;
    bool confined_ = false;
};
#endif

} // namespace aver::game
