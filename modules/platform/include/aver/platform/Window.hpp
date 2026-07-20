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

    // Retitle a live window. The project browser picks a project AFTER the window exists, so the
    // title cannot come from WindowDesc alone.
    void setTitle(const std::string& title);

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

    // Render-tick callback: invoked while the OS is running a modal move/size loop (which
    // otherwise blocks the engine's frame loop and freezes the viewport). Plain fn-ptr so
    // Platform stays free of any Runtime dependency.
    using RenderTickFn = void (*)(void* user);
    void setRenderTick(RenderTickFn fn, void* user) { renderTick_ = fn; renderTickUser_ = user; }
    void onRenderTick() { if (renderTick_) renderTick_(renderTickUser_); }

    // Modal move/size loop state (between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE). A move is
    // safe to render live; an active resize is NOT — both Present and ResizeBuffers deadlock
    // the DWM mid-resize — so the engine renders during a move and skips rendering during a
    // resize (the view updates once the drag ends). `modalResize_` latches on the first
    // WM_SIZE seen inside the loop, which is what distinguishes a resize from a move.
    bool inModalSize() const { return modalSize_; }
    bool inModalResize() const { return modalResize_; }
    bool isResizeGrab() const { return resizeGrab_; }

    // Internal: invoked by the platform message handler.
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
};

} // namespace aver
