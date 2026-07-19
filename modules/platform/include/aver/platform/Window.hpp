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
    // Display scale factor for this window's monitor (1.0 = 96 DPI, 1.5 = 150%, ...).
    // The process is per-monitor DPI aware, so width()/height() are physical pixels and
    // UI should be scaled by this to stay a consistent physical size across displays.
    f32 dpiScale() const { return dpiScale_; }
    void* nativeHandle() const { return nativeHandle_; } // HWND on Windows
    bool valid() const { return nativeHandle_ != nullptr; }

    void setEventCallback(EventCallback cb, void* user) { callback_ = cb; callbackUser_ = user; }

    // Raw OS message hook (e.g. for Dear ImGui input). Returns true if the message was
    // consumed. Kept as a plain fn-ptr so Platform needs no UI dependency.
    using MessageHook = bool (*)(void* hwnd, u32 msg, u64 wparam, i64 lparam);
    void setMessageHook(MessageHook h) { messageHook_ = h; }
    MessageHook messageHook() const { return messageHook_; }

    // Internal: invoked by the platform message handler.
    void dispatch(const Event& e);
    void setDpiScale(f32 s) { dpiScale_ = s; }

private:
    void* nativeHandle_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    f32 dpiScale_ = 1.0f;
    bool shouldClose_ = false;
    EventCallback callback_ = nullptr;
    void* callbackUser_ = nullptr;
    MessageHook messageHook_ = nullptr;
};

} // namespace aver
