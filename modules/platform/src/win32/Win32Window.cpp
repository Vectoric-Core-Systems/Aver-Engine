#include "aver/platform/Window.hpp"
#include "aver/core/Log.hpp"

#include <Windows.h>
#include <windowsx.h>
#include <string>

namespace aver {

static const wchar_t* kClassName = L"AverEngineWindow";

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
