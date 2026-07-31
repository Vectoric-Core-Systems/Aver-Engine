#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver {

// A borderless, centred, top-most splash window that shows a PNG during startup.
class Splash {
public:
    ~Splash();
    // Decodes and displays the PNG. False if it cannot be loaded.
    bool show(const std::string& pngPath);
    // Drains the splash window's queued messages.
    void pump();
    // Waits out `minVisibleMs` since it was shown, then destroys it.
    void close(u32 minVisibleMs = 900);
private:
    void* hwnd_ = nullptr;   // HWND
    void* memDc_ = nullptr;  // HDC
    void* bitmap_ = nullptr; // HBITMAP
    u64 shownAtMs_ = 0;
};

} // namespace aver
