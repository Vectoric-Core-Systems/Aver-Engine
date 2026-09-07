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
    // Created but NOT SHOWN. For a startup that puts a loading screen in front: the window is fully
    // real -- WM_SIZE has fired from the SetWindowPos calls in create(), so width/height/dpiScale are
    // established and the swapchain that reads them afterwards is correct -- it simply has no pixels
    // on screen yet. Without this the editor sat behind the splash as a frozen grey rectangle for the
    // whole load, which reads as a hang rather than as progress.
    //
    // The window is revealed with show(), NOT by a second create. Whoever sets this owns calling it.
    bool startHidden = false;
    // False shows the window without taking focus. An automated capture run has no business
    // stealing the keyboard from whatever the machine's owner is doing.
    bool activate = true;
    // BORDERLESS fullscreen -- a WS_POPUP sized to the monitor's full bounds, never DXGI's exclusive
    // mode. Exclusive fullscreen takes ownership of the display mode, breaks alt-tab, and interacts
    // badly with the editor's other windows; borderless costs nothing here because the swapchain is
    // already sized from the client area either way.
    //
    // IGNORED WHEN activate IS FALSE, and that is load-bearing rather than a convenience: a capture
    // run's window is a MEASURING INSTRUMENT. Every recorded gate probe is a pixel at a fixed rect
    // inside a client area of a fixed size (the baselines carry `rect=0,270 2750x1639`), so a capture
    // window that sized itself to whatever monitor it happened to run on would move all 223 baseline
    // rows and mean nothing was comparable across machines. Interactive runs get fullscreen; capture
    // runs keep the exact window the oracle was recorded through.
    bool fullscreen = false;
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
    // Reveals a window created with WindowDesc::startHidden. Idempotent and harmless on a window
    // that is already visible, so a caller does not have to track which case it is in.
    void show(bool activate = true);

    bool shouldClose() const { return shouldClose_; }
    void requestClose() { shouldClose_ = true; }

    // MAY THE WINDOW CLOSE? Asked synchronously from inside WM_CLOSE, before shouldClose_ is set.
    // Returning false vetoes the close and the app stays up.
    //
    // WHY A VETO AND NOT AN EVENT. WM_CLOSE already dispatches a WindowClose event, and an app could
    // in principle react to it -- except that the same handler then sets shouldClose_, and
    // Engine::run tests shouldClose() BEFORE calling frameStep(). So by the time an app could draw
    // anything in response, the frame loop has already broken. Every unsaved-changes prompt the
    // editor has was reachable only from its own File > Exit; the window's own X button and Alt+F4
    // went straight past all of them and took the work with them.
    //
    // The guard runs on the message thread inside the window procedure, so it must only decide --
    // set a flag, return. It must not block, and it must not itself try to close the window.
    using CloseGuard = bool (*)(void* user);
    void setCloseGuard(CloseGuard g, void* user) { closeGuard_ = g; closeGuardUser_ = user; }
    bool mayClose() { return closeGuard_ ? closeGuard_(closeGuardUser_) : true; }

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

    // A file forwarded here by another instance's launch, delivered as WM_COPYDATA (see the Win32
    // single-instance IPC further down, and the sender/receiver gates in SandboxApp.cpp's
    // createApplication/onInit). Runs SYNCHRONOUSLY on the calling thread of the SENDER's blocking
    // SendMessageTimeout, so it must stay cheap -- a string compare against the live project's path
    // is the entire intended budget -- and must not touch per-frame UI state, which may be mid-frame
    // on the receiving side when this fires. Returns true to ACCEPT (the receiver then records the
    // path and takes focus; the sender exits without ever opening its own window) or false to
    // DECLINE (the sender falls through and opens its own instance, exactly as if there were no
    // primary at all).
    //
    // Carries a user pointer, unlike MessageHook: MessageHook's only real implementation
    // (rhi::uiWndProc) is a stateless wrapper around ImGui's single global context, but this always
    // has to reach one specific SandboxApp instance's live project -- the same reason RenderTickFn
    // takes a user pointer instead of assuming a singleton.
    using OpenRequestHook = bool (*)(void* user, const char* path);
    void setOpenRequestHook(OpenRequestHook h, void* user) { openRequestHook_ = h; openRequestHookUser_ = user; }
    OpenRequestHook openRequestHook() const { return openRequestHook_; }
    void* openRequestHookUser() const { return openRequestHookUser_; }

    // Brings this window to the foreground, restoring it first if it is minimized. The only caller
    // is the WM_COPYDATA receive in Win32Window.cpp, once OpenRequestHook has accepted: a forwarded
    // level should not silently load behind whatever else has the user's attention.
    void focus();

    // A path OpenRequestHook accepted, polled once a frame from SandboxApp::onUpdate exactly like
    // fluidWantPending_ (see that member's own comment for the identical reasoning): Event
    // (Event.hpp) is a fixed POD struct with no string field, so a path cannot ride the existing
    // dispatch() callback and is latched here instead.
    bool hasPendingOpenRequest() const { return hasPendingOpenRequest_; }
    std::string takePendingOpenRequest() { hasPendingOpenRequest_ = false; return std::move(pendingOpenRequest_); }
    // Called only from Win32Window.cpp's WM_COPYDATA handler, once OpenRequestHook has accepted.
    void setPendingOpenRequest(std::string path) { pendingOpenRequest_ = std::move(path); hasPendingOpenRequest_ = true; }

    // Win32 single-instance forwarding: a named mutex as the "is a primary alive" existence check,
    // paired with a same-lifetime named file mapping that carries the primary's HWND. Both are
    // kernel objects Windows releases automatically when the owning process exits for ANY reason,
    // crash included -- so "is a primary alive" and "where do I send it" self-heal with zero polling
    // or staleness code. Static because both operate on OS-global named objects rather than on any
    // one Window instance -- forwardToSingleInstancePrimary in particular is called before this
    // process has created a Window of its own. See Win32Window.cpp for why this pair and not a named
    // pipe or the (opt-in, MCP-module-gated) MCP bridge.
    //
    // registerAsSingleInstancePrimary publishes `hwnd` as the target other launches should forward
    // to. It is a silent no-op if a primary already exists: exactly one process ever holds the
    // mutex's true (first) ownership, and this does not contest that.
    static void registerAsSingleInstancePrimary(void* hwnd);
    // Attempts to hand `path` to a running primary. True means a primary ACCEPTED it and the caller
    // should exit without creating its own window. False covers every other outcome -- no primary,
    // a primary that DECLINED (a different project is open), or a timed-out send -- and all of them
    // mean the same thing: proceed and open your own instance, exactly as if this call had not been
    // made at all.
    static bool forwardToSingleInstancePrimary(const std::string& path);

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
    CloseGuard closeGuard_ = nullptr;
    void* closeGuardUser_ = nullptr;
    OpenRequestHook openRequestHook_ = nullptr;
    void* openRequestHookUser_ = nullptr;
    bool hasPendingOpenRequest_ = false;
    std::string pendingOpenRequest_;
    RenderTickFn renderTick_ = nullptr;
    void* renderTickUser_ = nullptr;
    bool modalSize_ = false;
    bool modalResize_ = false;
    bool resizeGrab_ = false;
    i32 mouseCapture_ = 0;
};

} // namespace aver
