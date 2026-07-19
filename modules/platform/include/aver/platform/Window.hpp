#pragma once
#include "aver/core/Types.hpp"
#include "Event.hpp"

#include <string>

namespace aver {

struct WindowDesc {
    std::string title = "Aver Engine";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
};

// Minimal OS window (Win32 backend). Returns false from create() on failure so the
// engine can fall back to headless rather than aborting.
class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const WindowDesc& desc);
    void destroy();
    void pumpEvents();

    bool shouldClose() const { return shouldClose_; }
    void requestClose() { shouldClose_ = true; }

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    void* nativeHandle() const { return nativeHandle_; } // HWND on Windows
    bool valid() const { return nativeHandle_ != nullptr; }

    void setEventCallback(EventCallback cb, void* user) { callback_ = cb; callbackUser_ = user; }

    // Internal: invoked by the platform message handler.
    void dispatch(const Event& e);

private:
    void* nativeHandle_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    bool shouldClose_ = false;
    EventCallback callback_ = nullptr;
    void* callbackUser_ = nullptr;
};

} // namespace aver
