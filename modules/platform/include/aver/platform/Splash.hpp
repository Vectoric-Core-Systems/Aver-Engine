#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver {

// A borderless, centered, top-most splash window that shows a PNG during startup.
class Splash {
public:
    ~Splash();
    bool show(const std::string& pngPath); // decode + display; false if it can't load
    void pump();                            // keep it responsive
    void close(u32 minVisibleMs = 900);     // ensure it showed for a bit, then destroy
private:
    void* hwnd_ = nullptr;   // HWND
    void* memDc_ = nullptr;  // HDC
    void* bitmap_ = nullptr; // HBITMAP
    u64 shownAtMs_ = 0;
};

} // namespace aver
