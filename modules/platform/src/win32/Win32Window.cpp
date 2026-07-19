#include "aver/platform/Window.hpp"
#include "aver/core/Log.hpp"

#include <Windows.h>
#include <windowsx.h>
#include <string>

namespace aver {

static const wchar_t* kClassName = L"AverEngineWindow";

// Make the process per-monitor DPI aware (once) so the OS never bitmap-upscales our
// window — that virtualisation is what makes a hi-DPI viewport look soft and feel
// sluggish. Done dynamically so we still link/run on older Windows.
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
    SetProcessDPIAware(); // legacy fallback (system-DPI aware)
}

// Query a window's DPI scale (1.0 == 96 DPI). GetDpiForWindow is Win10+; fall back to
// the device-context DPI otherwise.
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

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    // Let a UI (ImGui) inspect the raw message first; if it fully consumes it, stop.
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
            // A WM_SIZE inside a modal loop means the user is resizing (not moving): latch it
            // so the engine stops presenting until the drag ends (Present mid-resize deadlocks
            // the DWM). Do NOT render here either — rendering from inside a window-state-change
            // message deadlocks the same way (this is the WM_SIZE a maximise sends).
            if (self->inModalSize()) self->setModalResize(true);
            return 0;
        }
        // A modal move/size loop runs its own message pump inside DefWindowProc, starving the
        // engine's frame loop. Drive rendering from a timer for its duration so the viewport
        // keeps updating live instead of freezing. The timer fires at an idle point in the
        // modal loop (unlike WM_SIZE), so Present here does not deadlock with the DWM.
        case WM_NCLBUTTONDOWN: {
            // Remember whether this grab is on a resize border/corner (vs the caption). We can
            // render live during a move but must NOT present during a resize (DWM deadlock).
            const bool resize = (wParam >= HTLEFT && wParam <= HTBOTTOMRIGHT) || wParam == HTGROWBOX;
            self->setResizeGrab(resize);
            break; // let DefWindowProc run the modal loop
        }
        case WM_ENTERSIZEMOVE:
            self->setModalSize(true);
            if (!self->isResizeGrab()) SetTimer(hwnd, 1, USER_TIMER_MINIMUM, nullptr); // live render for moves only
            return 0;
        case WM_EXITSIZEMOVE:
            self->setModalSize(false); self->setResizeGrab(false); KillTimer(hwnd, 1); return 0;
        case WM_TIMER: if (wParam == 1) { self->onRenderTick(); return 0; } break;
        case WM_DPICHANGED: {
            // Monitor changed / DPI changed: adopt the OS-suggested window rect and record
            // the new scale so the UI rescales. A WM_SIZE follows and resizes the swapchain.
            self->setDpiScale(static_cast<f32>(HIWORD(wParam)) / 96.0f);
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
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
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void Window::dispatch(const Event& e) {
    if (e.type == EventType::WindowResize) { width_ = e.width; height_ = e.height; }
    if (callback_) callback_(callbackUser_, e);
}

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
    RegisterClassExW(&wc); // ignore "already registered" on repeat

    DWORD style = desc.resizable ? WS_OVERLAPPEDWINDOW
                                 : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);

    RECT rc = {0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    AdjustWindowRect(&rc, style, FALSE);

    const std::wstring title = utf8ToWide(desc.title);

    HWND hwnd = CreateWindowExW(
        0, kClassName, title.c_str(), style,
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

    // The requested size is a logical (96-DPI) design size. Scale it to the display's DPI
    // so the window opens at a sensible physical size, then clamp to the monitor work area
    // and centre it (a 1600x900 physical window on a 300% display would be unusably small).
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

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    AVER_INFO("[Platform] window '{}' {}x{} created", desc.title, desc.width, desc.height);
    return true;
}

void Window::pumpEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { shouldClose_ = true; continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Window::destroy() {
    if (nativeHandle_) {
        DestroyWindow(static_cast<HWND>(nativeHandle_));
        nativeHandle_ = nullptr;
    }
}

Window::~Window() { destroy(); }

} // namespace aver
