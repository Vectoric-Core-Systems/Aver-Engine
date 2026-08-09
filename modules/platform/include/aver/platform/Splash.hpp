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

    int width_ = 0, height_ = 0;   // after DPI scaling
    int posX_ = 0, posY_ = 0;      // top-left, in virtual-screen coordinates
    // The scaled image WITHOUT any text, kept so each status line replaces the last instead of
    // painting over it. Without this the corner accretes overlapping strings.
    std::vector<u8> pristine_;
    std::string status_;
};

} // namespace aver
