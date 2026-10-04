// OS mouse capture (ClipCursor + hidden cursor + re-centre every frame), shared by the editor's
// play mode and the shipped game -- both hosts hide and confine the cursor identically while a
// session is playing, and each is free to decide independently WHEN that should be true.
#pragma once
#include "aver/core/Types.hpp"

namespace aver {
class Window;
}

namespace aver::game {

// Deltas are measured from the re-centre point rather than from WM_MOUSEMOVE, and a background
// window is never warped or clipped -- see poll()'s own comment.
class MouseCapture {
public:
    // Gives the mouse to the game (hides the cursor, confines it, re-centres it) or hands it back.
    // A no-op when `on` already matches the current state, matching ShowCursor's own paired-call
    // contract (ShowCursor is a counter, so each call must be paired). `hostTag` names the log
    // line's bracket ("Sandbox"/"Game"); `captureHint` is appended to the captured line only (empty
    // by default).
    void set(bool on, Window* window, const char* hostTag, const char* captureHint = "");
    // Measures one frame of captured mouse movement into dx_/dy_, then re-centres for the next.
    // Zeroes both and returns immediately when not currently captured.
    void poll(Window* window);

    bool captured() const { return captured_; }
    f32 dx() const { return dx_; }
    f32 dy() const { return dy_; }

private:
    // Parks the cursor at the window's centre, remembers where that was, and confines it there.
    // Refuses to move a background window's cursor -- see its own definition's comment.
    void warpToAnchor(Window* window);

    bool captured_ = false;
    i32  anchorX_ = 0, anchorY_ = 0;
    f32  dx_ = 0.0f, dy_ = 0.0f;
};

} // namespace aver::game
