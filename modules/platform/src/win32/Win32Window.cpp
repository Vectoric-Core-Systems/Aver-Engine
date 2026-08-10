// Win32 backend for Window: class registration, the window procedure, and the message pump.

#include "aver/platform/Window.hpp"
#include "aver/core/Log.hpp"

#include <Windows.h>
#include <windowsx.h>
#include <string>

namespace aver {

static const wchar_t* kClassName = L"AverEngineWindow";

// Makes the process per-monitor DPI aware, once. Resolved dynamically to still run on older Windows.
static void enableDpiAwareness() {
    static bool done = false;
    if (done) return;
    done = true;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL(WINAPI*)(void*); // SetProcessDpiAwarenessContext
        if (auto set = reinterpret_cast<SetCtxFn>(reinterpret_cast<void*>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext")))) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            if (set(reinterpret_cast<void*>(static_cast<intptr_t>(-4)))) return;
            set(reinterpret_cast<void*>(static_cast<intptr_t>(-3))); // ..._PER_MONITOR_AWARE
            return;
        }
    }
    SetProcessDPIAware();
}

// Returns a window's DPI scale (1.0 == 96 DPI).
static f32 queryDpiScale(HWND hwnd) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        if (auto get = reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
                GetProcAddress(user32, "GetDpiForWindow")))) {
            const UINT dpi = get(hwnd);
            if (dpi) return static_cast<f32>(dpi) / 96.0f;
        }
    }
    HDC dc = GetDC(hwnd);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi ? static_cast<f32>(dpi) / 96.0f : 1.0f;
}

// Converts a UTF-8 string to a wide string.
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The window procedure: translates OS messages into Events and drives the modal-loop state.
static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (auto hook = self->messageHook()) {
        if (hook(hwnd, static_cast<u32>(msg), static_cast<u64>(wParam), static_cast<i64>(lParam))) {
            return 0;
        }
    }

    switch (msg) {
        case WM_CLOSE: {
            Event e; e.type = EventType::WindowClose;
            self->dispatch(e);
            self->requestClose();
            return 0;
        }
        case WM_SIZE: {
            Event e; e.type = EventType::WindowResize;
            e.width = LOWORD(lParam); e.height = HIWORD(lParam);
            self->dispatch(e);
            // Present and ResizeBuffers deadlock the DWM mid-resize; do neither from here.
            if (self->inModalSize()) self->setModalResize(true);
            return 0;
        }
        case WM_NCLBUTTONDOWN: {
            const bool resize = (wParam >= HTLEFT && wParam <= HTBOTTOMRIGHT) || wParam == HTGROWBOX;
            self->setResizeGrab(resize);
            // Must reach DefWindowProc: every caption/border interaction is driven from here.
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ENTERSIZEMOVE:
            self->setModalSize(true);
            if (!self->isResizeGrab()) SetTimer(hwnd, 1, USER_TIMER_MINIMUM, nullptr); // live render for moves only
            return 0;
        case WM_EXITSIZEMOVE:
            self->setModalSize(false); self->setResizeGrab(false); KillTimer(hwnd, 1); return 0;
        case WM_TIMER:
            if (wParam == 1) { self->onRenderTick(); return 0; }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_DPICHANGED: {
            self->setDpiScale(static_cast<f32>(HIWORD(wParam)) / 96.0f);
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        // WM_SYSKEYDOWN/UP carry the Alt combinations and F10. Without them, Alt is invisible to
        // the event stream -- and it is not enough to translate them and return 0, because
        // swallowing WM_SYSKEYDOWN breaks Alt+F4 and the system menu. Dispatch, then fall through
        // to DefWindowProc.
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            Event e; e.type = EventType::Key;
            e.key = static_cast<i32>(wParam);
            e.pressed = (msg == WM_SYSKEYDOWN);
            self->dispatch(e);
            break;   // NOT return 0 -- the OS still needs this message
        }
        case WM_KEYDOWN:
        case WM_KEYUP: {
            Event e; e.type = EventType::Key;
            e.key = static_cast<i32>(wParam);
            e.pressed = (msg == WM_KEYDOWN);
            self->dispatch(e);
            return 0;
        }
        case WM_MOUSEMOVE: {
            Event e; e.type = EventType::MouseMove;
            e.mouseX = GET_X_LPARAM(lParam);
            e.mouseY = GET_Y_LPARAM(lParam);
            self->dispatch(e);
            return 0;
        }
        // EventType::MouseButton has existed in Event.hpp since it was written and nothing has
        // ever produced one. Capture on press and release on the last button up, so a drag that
        // leaves the client area still delivers its button-up: without capture the up is sent to
        // whatever window is under the cursor and this one believes the button is still down.
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP: {
            const bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
            Event e; e.type = EventType::MouseButton;
            e.button  = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0
                      : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
            e.pressed = down;
            e.mouseX  = GET_X_LPARAM(lParam);
            e.mouseY  = GET_Y_LPARAM(lParam);
            if (down) {
                if (self->mouseCapture()++ == 0) SetCapture(hwnd);
            } else if (self->mouseCapture() > 0) {
                if (--self->mouseCapture() == 0) ReleaseCapture();
            }
            self->dispatch(e);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            Event e; e.type = EventType::MouseWheel;
            e.wheel = static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<f32>(WHEEL_DELTA);
            self->dispatch(e);
            return 0;
        }
        // No WM_KEYUP is ever sent for a key that was held when focus left, so anything
        // accumulating key state has to be told to drop it here or the key stays held forever.
        case WM_KILLFOCUS: {
            if (self->mouseCapture() > 0) { self->mouseCapture() = 0; ReleaseCapture(); }
            Event e; e.type = EventType::FocusLost;
            self->dispatch(e);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Records a resize into the cached size and forwards the event to the callback.
void Window::dispatch(const Event& e) {
    if (e.type == EventType::WindowResize) { width_ = e.width; height_ = e.height; }
    if (callback_) callback_(callbackUser_, e);
}

// Registers the class and creates the window, DPI-scaled and centred. False on failure.
bool Window::create(const WindowDesc& desc) {
    enableDpiAwareness(); // must precede any window creation
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));   // app icon resource (Sandbox.rc)
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = desc.resizable ? WS_OVERLAPPEDWINDOW
                                 : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);

    RECT rc = {0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    AdjustWindowRect(&rc, style, FALSE);

    const std::wstring title = utf8ToWide(desc.title);

    // WS_EX_NOACTIVATE on a non-interactive run: a capture or gate window must never take the
    // keyboard focus away from whatever the person is actually doing.
    //
    // SW_SHOWNOACTIVATE ALONE WAS NOT ENOUGH, which is the part worth writing down. It stops the
    // window taking FOCUS, but a freshly shown top-level window still lands at the top of the
    // Z-ORDER -- so it sits in front of the editor, unfocused, and has to be clicked away. The
    // ex-style stops the activation; the HWND_BOTTOM push after ShowWindow below stops the
    // stacking. Both are needed.
    //
    // NOT WS_EX_TOOLWINDOW, tempting as it is for keeping this out of the taskbar: a tool window
    // has a different caption height, AdjustWindowRect would hand back a different client size,
    // and every recorded gate probe is a pixel at a fixed rect in that client area. Keeping the
    // frame byte-identical between interactive and capture runs is what makes the two comparable.
    const DWORD exStyle = desc.activate ? 0 : WS_EX_NOACTIVATE;

    HWND hwnd = CreateWindowExW(
        exStyle, kClassName, title.c_str(), style,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, inst, this);

    if (!hwnd) {
        AVER_WARN("[Platform] CreateWindowExW failed (err={})", static_cast<u32>(GetLastError()));
        return false;
    }

    nativeHandle_ = hwnd;
    width_ = desc.width;
    height_ = desc.height;
    dpiScale_ = queryDpiScale(hwnd);

    // desc.width/height are logical 96-DPI sizes: scale, clamp to the work area, centre.
    {
        int cw = static_cast<int>(desc.width * dpiScale_ + 0.5f);
        int ch = static_cast<int>(desc.height * dpiScale_ + 0.5f);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            const int waW = mi.rcWork.right - mi.rcWork.left;
            const int waH = mi.rcWork.bottom - mi.rcWork.top;
            if (cw > waW * 92 / 100) cw = waW * 92 / 100;
            if (ch > waH * 92 / 100) ch = waH * 92 / 100;
            RECT wr = {0, 0, cw, ch};
            AdjustWindowRect(&wr, style, FALSE);
            const int winW = wr.right - wr.left, winH = wr.bottom - wr.top;
            const int x = mi.rcWork.left + (waW - winW) / 2;
            const int y = mi.rcWork.top + (waH - winH) / 2;
            SetWindowPos(hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE); // WM_SIZE updates width_/height_
        }
    }

    ShowWindow(hwnd, desc.activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    // Push a capture window to the BOTTOM of the stack. SWP_NOMOVE|SWP_NOSIZE is load-bearing: the
    // centring SetWindowPos above already placed it, and a gate probe is a pixel at a fixed rect in
    // a client area of a fixed size, so this call may change the Z order and nothing else.
    if (!desc.activate)
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(hwnd);
    AVER_INFO("[Platform] window '{}' {}x{} created", desc.title, desc.width, desc.height);
    return true;
}

// Retitles a live window.
void Window::setTitle(const std::string& title) {
    if (nativeHandle_) SetWindowTextW(static_cast<HWND>(nativeHandle_), utf8ToWide(title).c_str());
}

// Drains every queued OS message without blocking.
void Window::pumpEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { shouldClose_ = true; continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Destroys the OS window.
void Window::destroy() {
    if (nativeHandle_) {
        DestroyWindow(static_cast<HWND>(nativeHandle_));
        nativeHandle_ = nullptr;
    }
}

Window::~Window() { destroy(); }

} // namespace aver
