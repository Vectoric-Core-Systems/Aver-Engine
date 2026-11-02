#pragma once
#include "aver/core/Types.hpp"
#include "Event.hpp"

#include <array>
#include <vector>

namespace aver {

// Accumulated keyboard and mouse state, fed from the Window event stream.
//
// WHY THIS EXISTS. Until now the only thing in the tree that turned OS input into gameplay input
// was SandboxApp::pushInput, whose entire body sits inside `#if AVER_WITH_IMGUI` and reads
// ImGui::IsKeyDown / io.MouseDelta. That makes ImGui a *functional* dependency of being able to
// play the game, not merely a UI one -- a build with AVER_ENABLE_UI=OFF has no input at all. A
// standalone game executable cannot be built on that, so the state lives here instead, one layer
// below anyone's UI, and both hosts can read it.
//
// Deliberately knows nothing about the framework: no aver_fw_* calls, no key-code translation.
// Callers map virtual keys to whatever their input contract is. That keeps this testable with no
// window, no device and no CLR -- feed it events, read the state.
//
// FRAME PROTOCOL, and the order matters:
//
//     input.newFrame();      // roll edges, zero the per-frame deltas
//     window.pumpEvents();   // events land in the accumulator
//     ... read input ...
//
// newFrame() BEFORE pumpEvents(), because pressed()/released() report edges seen since the last
// newFrame(). Calling it after pumping throws away the frame's own edges, which reads as "the game
// ignores single taps" and is a genuinely annoying bug to find.
class InputState {
public:
    static constexpr usize kKeyCount = 256;   // Win32 virtual key codes are 0..255
    static constexpr usize kMouseButtons = 5; // 0=L, 1=R, 2=M, 3=X1, 4=X2
    static constexpr usize kTouchCount = 10;  // fingers tracked, by the index the window layer gives them

    // Feed one event. Safe to call with any event type; irrelevant ones are ignored.
    void onEvent(const Event& e);

    // Starts a new frame: clears the press/release edges and the per-frame mouse delta and wheel.
    // Held state and absolute mouse position persist across frames, which is the point.
    void newFrame();

    // Drops everything. Called on EventType::FocusLost, and safe to call directly.
    //
    // This is not tidiness. Windows sends no WM_KEYUP for a key that was down when focus left, so
    // without this a key held during Alt+Tab remains held for the rest of the process.
    void clear();

    // --- keyboard, by Win32 virtual key code ---
    bool keyHeld(i32 vk) const     { return inRange(vk) && held_[static_cast<usize>(vk)]; }
    bool keyPressed(i32 vk) const  { return inRange(vk) && pressed_[static_cast<usize>(vk)]; }
    bool keyReleased(i32 vk) const { return inRange(vk) && released_[static_cast<usize>(vk)]; }

    // --- mouse ---
    bool mouseHeld(i32 b) const     { return inButton(b) && mHeld_[static_cast<usize>(b)]; }
    bool mousePressed(i32 b) const  { return inButton(b) && mPressed_[static_cast<usize>(b)]; }
    bool mouseReleased(i32 b) const { return inButton(b) && mReleased_[static_cast<usize>(b)]; }

    i32 mouseX() const { return mouseX_; }
    i32 mouseY() const { return mouseY_; }
    // Movement since the last newFrame(). Zero on the first frame a cursor is seen rather than the
    // distance from the origin -- an uninitialised delta is a camera that snaps on the first mouse
    // move, which looks like a physics bug and is not one.
    i32 mouseDX() const { return mouseDX_; }
    i32 mouseDY() const { return mouseDY_; }
    // Wheel notches accumulated this frame; one detent is 1.0.
    f32 wheel() const { return wheel_; }
    // The horizontal notches accumulated this frame, positive = right.
    f32 wheelX() const { return wheelX_; }

    // True once any mouse position has been seen. Until then the position is meaningless.
    bool hasMouse() const { return hasMouse_; }

    // --- touch: real fingers from the window layer (EventType::Touch), by finger index ---
    bool touchHeld(i32 f) const { return inTouch(f) && tHeld_[static_cast<usize>(f)]; }
    bool touchPressed(i32 f) const { return inTouch(f) && tPressed_[static_cast<usize>(f)]; }
    bool touchReleased(i32 f) const { return inTouch(f) && tReleased_[static_cast<usize>(f)]; }
    f32 touchX(i32 f) const { return inTouch(f) ? tX_[static_cast<usize>(f)] : 0.0f; }
    f32 touchY(i32 f) const { return inTouch(f) ? tY_[static_cast<usize>(f)] : 0.0f; }
    f32 touchPressure(i32 f) const { return inTouch(f) ? tPressure_[static_cast<usize>(f)] : 0.0f; }
    // True once any finger event has arrived: this machine has a touch screen.
    bool touchSeen() const { return touchSeen_; }

    // --- typed text (E16): the code points the window delivered as EventType::Char since the last newFrame(), in order. At most kMaxTyped
    // are kept in a frame (a stuck key or a paste storm cannot grow the list). Layout, dead keys and the IME's committed text are already applied.
    static constexpr usize kMaxTyped = 64;
    const std::vector<u32>& typedChars() const { return typed_; }

private:
    std::vector<u32> typed_;
    static bool inRange(i32 vk)  { return vk >= 0 && vk < static_cast<i32>(kKeyCount); }
    static bool inButton(i32 b)  { return b >= 0 && b < static_cast<i32>(kMouseButtons); }
    static bool inTouch(i32 f)   { return f >= 0 && f < static_cast<i32>(kTouchCount); }

    std::array<bool, kKeyCount> held_{};
    std::array<bool, kKeyCount> pressed_{};
    std::array<bool, kKeyCount> released_{};

    std::array<bool, kMouseButtons> mHeld_{};
    std::array<bool, kMouseButtons> mPressed_{};
    std::array<bool, kMouseButtons> mReleased_{};

    i32 mouseX_ = 0, mouseY_ = 0;
    i32 mouseDX_ = 0, mouseDY_ = 0;
    f32 wheel_ = 0.0f;
    f32 wheelX_ = 0.0f;
    bool hasMouse_ = false;

    std::array<bool, kTouchCount> tHeld_{};
    std::array<bool, kTouchCount> tPressed_{};
    std::array<bool, kTouchCount> tReleased_{};
    std::array<f32, kTouchCount> tX_{}, tY_{}, tPressure_{};
    bool touchSeen_ = false;
};

} // namespace aver
