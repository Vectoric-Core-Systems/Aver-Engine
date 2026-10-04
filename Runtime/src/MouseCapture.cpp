#include "aver/game/MouseCapture.hpp"
#include "aver/platform/Window.hpp"
#include "aver/core/Log.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace aver::game {

void MouseCapture::set(bool on, Window* window, const char* hostTag, const char* captureHint) {
#if defined(_WIN32)
    if (on == captured_) return;
    captured_ = on;
    if (on) {
        ShowCursor(FALSE);
        warpToAnchor(window);
    } else {
        ShowCursor(TRUE);
        ClipCursor(nullptr);
    }
    AVER_INFO("[{}] mouse {} the game{}", hostTag, on ? "captured by" : "released from",
              on ? captureHint : "");
#else
    (void)window; (void)hostTag; (void)captureHint;
    captured_ = on;
#endif
}

// Parks the cursor at the centre of the window, remembers where that was, and confines it there.
void MouseCapture::warpToAnchor(Window* window) {
#if defined(_WIN32)
    HWND hwnd = window ? static_cast<HWND>(window->nativeHandle()) : nullptr;
    if (!hwnd) return;
    // A WINDOW THAT IS NOT FOREGROUND HAS NO BUSINESS MOVING THE POINTER: SetCursorPos/ClipCursor
    // below are global and would drag the cursor to this window's centre while the user works
    // elsewhere. Windows ignores ClipCursor from a background window anyway, so only the cursor
    // theft is lost. Anchoring to where the pointer actually IS keeps the delta honest on refocus.
    if (::GetForegroundWindow() != hwnd) {
        POINT q{};
        if (GetCursorPos(&q)) { anchorX_ = q.x; anchorY_ = q.y; }
        return;
    }
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return;
    POINT c{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
    ClientToScreen(hwnd, &c);
    anchorX_ = c.x; anchorY_ = c.y;
    SetCursorPos(c.x, c.y);
    RECT screen{};
    POINT tl{ rc.left, rc.top }, br{ rc.right, rc.bottom };
    ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &br);
    screen.left = tl.x; screen.top = tl.y; screen.right = br.x; screen.bottom = br.y;
    ClipCursor(&screen);
#else
    (void)window;
#endif
}

// Measures one frame of captured mouse movement, then re-centres for the next.
void MouseCapture::poll(Window* window) {
    dx_ = dy_ = 0.0f;
#if defined(_WIN32)
    if (!captured_) return;
    HWND fg = window ? static_cast<HWND>(window->nativeHandle()) : nullptr;
    // A STALE ANCHOR IS A VIEW SNAP, and losing focus is how the anchor goes stale: Windows drops
    // ClipCursor confinement the moment a window stops being foreground, and nothing re-captures on
    // the way back (set() no-ops if the state hasn't changed), so the next GetCursorPos() would
    // measure against a pre-alt-tab anchor and hand the framework one enormous delta -- the camera
    // whips round exactly once, the frame focus returns.
    // RE-ANCHOR AND REPORT ZERO: one frame of no look input on refocus is imperceptible; a spin
    // is not.
    // ANCHOR TO WHERE THE CURSOR IS, NOT warpToAnchor(): that calls SetCursorPos, and dragging the
    // pointer to this window's centre every frame while the user works elsewhere is a worse bug --
    // it would also steal the cursor during a bounded --frames run with a play session up.
    if (fg && ::GetForegroundWindow() != fg) {
        POINT q{};
        if (GetCursorPos(&q)) { anchorX_ = q.x; anchorY_ = q.y; }
        return;
    }
    POINT p{};
    if (!GetCursorPos(&p)) return;
    dx_ = static_cast<f32>(p.x - anchorX_);
    dy_ = static_cast<f32>(p.y - anchorY_);
    warpToAnchor(window);
#else
    (void)window;
#endif
}

} // namespace aver::game
