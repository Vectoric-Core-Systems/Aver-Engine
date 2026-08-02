#pragma once
#include "aver/core/Types.hpp"
#include "Event.hpp"

#include <string>

namespace aver {

// How a window should be created.
struct WindowDesc {
    std::string title = "Aver Engine";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
    // False shows the window without taking focus. An automated capture run has no business
    // stealing the keyboard from whatever the machine's owner is doing.
    bool activate = true;
};

// Minimal OS window (Win32 backend). create() returns false rather than aborting.
class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates the OS window. False on failure.
    bool create(const WindowDesc& desc);
    // Destroys the OS window.
    void destroy();
    // Drains every queued OS message without blocking.
    void pumpEvents();

    // Retitles a live window.
    void setTitle(const std::string& title);

    bool shouldClose() const { return shouldClose_; }
    void requestClose() { shouldClose_ = true; }

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    // Display scale factor for this window's monitor (1.0 = 96 DPI). width()/height() are physical.
    f32 dpiScale() const { return dpiScale_; }
    void* nativeHandle() const { return nativeHandle_; } // HWND on Windows
    bool valid() const { return nativeHandle_ != nullptr; }

    void setEventCallback(EventCallback cb, void* user) { callback_ = cb; callbackUser_ = user; }

    // Raw OS message hook (e.g. Dear ImGui input). Returns true if the message was consumed.
    using MessageHook = bool (*)(void* hwnd, u32 msg, u64 wparam, i64 lparam);
    void setMessageHook(MessageHook h) { messageHook_ = h; }
    MessageHook messageHook() const { return messageHook_; }

    // Render-tick callback, invoked while the OS is running a modal move/size loop.
    using RenderTickFn = void (*)(void* user);
    void setRenderTick(RenderTickFn fn, void* user) { renderTick_ = fn; renderTickUser_ = user; }
    void onRenderTick() { if (renderTick_) renderTick_(renderTickUser_); }

    // Modal move/size loop state. A move is safe to render live; an active resize is not — both
    // Present and ResizeBuffers deadlock the DWM mid-resize.
    bool inModalSize() const { return modalSize_; }
    bool inModalResize() const { return modalResize_; }
    bool isResizeGrab() const { return resizeGrab_; }

    // Depth of the mouse-button capture, owned by the platform message handler. A COUNT and not a
    // flag: pressing left, then right, then releasing left must not drop the capture while a
    // button is still down. Exposed as a reference because the Win32 handler is the only writer.
    i32& mouseCapture() { return mouseCapture_; }

    // Forwards an event to the callback, caching a resize. Called by the platform message handler.
    void dispatch(const Event& e);
    void setDpiScale(f32 s) { dpiScale_ = s; }
    void setModalSize(bool b) { modalSize_ = b; modalResize_ = false; }
    void setModalResize(bool b) { modalResize_ = b; }
    void setResizeGrab(bool b) { resizeGrab_ = b; }

private:
    void* nativeHandle_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    f32 dpiScale_ = 1.0f;
    bool shouldClose_ = false;
    EventCallback callback_ = nullptr;
    void* callbackUser_ = nullptr;
    MessageHook messageHook_ = nullptr;
    RenderTickFn renderTick_ = nullptr;
    void* renderTickUser_ = nullptr;
    bool modalSize_ = false;
    bool modalResize_ = false;
    bool resizeGrab_ = false;
    i32 mouseCapture_ = 0;
};

} // namespace aver
