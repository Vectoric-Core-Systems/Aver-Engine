#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// A borderless, centred, top-most splash window that shows a PNG during startup, with a line of
// status text in its bottom-left corner.
//
// THE STATUS LINE IS THE POINT OF IT. Startup does several things that take real time -- creating a
// device, compiling the shader preludes, loading a project's meshes and materials -- and with a
// static image there is no way to tell a slow start from a hung one. Naming the current stage costs
// almost nothing and turns "it froze" into "it is compiling shaders".
//
// NO Windows.h HERE. The handles stay void* so a public platform header does not drag the Win32
// world into everything that includes it; the .cpp casts them back.
class Splash {
public:
    ~Splash();

    // Decodes and displays the PNG, scaled for the display's DPI and centred on the monitor the
    // cursor is on. False if it cannot be loaded.
    bool show(const std::string& pngPath);

    // Replaces the status line and repaints. Safe to call before show() (it is remembered and drawn
    // when the window appears) and after close() (it does nothing).
    //
    // Repaints SYNCHRONOUSLY, because the caller is usually about to block on the very thing the
    // text describes -- a message that only appears after the work finishes is worse than none.
    void setStatus(const std::string& text);

    // Draws (or hides) a thin progress bar under the status line. `fraction` is clamped to [0, 1];
    // NEGATIVE HIDES THE BAR, the same "no value" convention setStatus's empty string already gets
    // for text -- a caller that never calls this shows no bar at all, so a plain status-only splash
    // (startup, an indeterminate wait) looks exactly as it always has.
    //
    // Same "safe before show(), no-op after close()" contract as setStatus, and the same synchronous
    // repaint -- a bar that only moves once the work it describes has finished is worse than none.
    void setProgress(f32 fraction);

    // Drains the splash window's queued messages.
    void pump();

    // Waits out `minVisibleMs` since it was shown, then destroys it.
    void close(u32 minVisibleMs = 900);

private:
    // Re-composites the pristine image plus the current status text and pushes it to the window.
    void repaint();

    void* hwnd_ = nullptr;   // HWND
    void* memDc_ = nullptr;  // HDC
    void* bitmap_ = nullptr; // HBITMAP
    void* font_ = nullptr;   // HFONT, sized for the display's DPI
    void* bits_ = nullptr;   // the DIB's pixels, owned by the bitmap
    u64 shownAtMs_ = 0;
    // setProgress's own pump() throttle -- see that method for why repaint() runs on every call
    // regardless (it reaches the screen with no message loop involved) while pump() itself is
    // capped at ~30 Hz. 0 means "never pumped for a progress update yet".
    u64 lastProgressPumpMs_ = 0;

    int width_ = 0, height_ = 0;   // after DPI scaling
    int posX_ = 0, posY_ = 0;      // top-left, in virtual-screen coordinates
    // The scaled image WITHOUT any text, kept so each status line replaces the last instead of
    // painting over it. Without this the corner accretes overlapping strings.
    std::vector<u8> pristine_;
    std::string status_;
    // See setProgress. Negative = no bar drawn -- the state show() and every ctor leave this in, so
    // a splash nobody calls setProgress on paints exactly as it did before this field existed.
    f32 progress_ = -1.0f;
};

} // namespace aver
