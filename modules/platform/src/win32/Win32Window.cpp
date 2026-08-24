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

// Converts a wide string to UTF-8. The other direction of utf8ToWide, needed only for decoding a
// forwarded path off WM_COPYDATA -- everything else in this file only ever produces wide strings,
// never consumes them.
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<usize>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// SINGLE-INSTANCE FORWARDING (Win32Window.cpp/Window.hpp half; the sender/receiver GATES that
// decide when any of this runs at all live in SandboxApp.cpp's createApplication/onInit -- this
// file only implements the mechanism once something upstream has already decided to use it).
//
// A named mutex is the "is a primary alive" existence check: CreateMutexW's ERROR_ALREADY_EXISTS
// is atomic across processes, so exactly one launch ever becomes the primary even if several start
// within the same instant. It carries no data, so it needs a same-lifetime companion for the actual
// payload -- a named file mapping holding the primary's HWND, big enough for nothing else. Both are
// kernel objects the OS closes automatically when the owning process's last handle goes away,
// including on a crash, so a later launch's OpenMutexW/OpenFileMappingW simply fail (cleanly, no
// exception, no stale data) once the primary is gone. That is the entire staleness story: there is
// no heartbeat, no timestamp, and nothing here polls anything.
//
// "Local\\" rather than a bare name: this editor is a per-user desktop tool with no session-0
// service anywhere near it, but namespacing costs nothing and documents the intent -- this is not
// meant to be reachable across a Remote Desktop session boundary or from a different user's login.
static const wchar_t* kSingleInstanceMutexName = L"Local\\AverEngineEditor.SingleInstance.Mutex";
static const wchar_t* kSingleInstanceMappingName = L"Local\\AverEngineEditor.SingleInstance.Hwnd";

// Holds the primary's own handles open for the process's lifetime. Never closed explicitly and
// never needs to be: process exit (however it happens) is what releases them, which is the whole
// point -- see the block comment above. A process that is NOT the primary (declined the mutex
// because one already existed) leaves these null and never touches this pair again.
static HANDLE g_singleInstanceMutex = nullptr;
static HANDLE g_singleInstanceMapping = nullptr;

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
        // A file forwarded from another instance's launch (see the single-instance block above and
        // SandboxApp.cpp's createApplication for the sender). dwData==1 is the only message kind
        // this channel carries today ("open this path"); anything else is refused outright rather
        // than guessed at, since a WM_COPYDATA can in principle arrive from ANY process on the
        // system, not only this engine's own sender.
        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (!cds || cds->dwData != 1 || !cds->lpData || cds->cbData < sizeof(wchar_t)) return 0;
            const wchar_t* wtext = reinterpret_cast<const wchar_t*>(cds->lpData);
            const usize wlen = static_cast<usize>(cds->cbData) / sizeof(wchar_t);
            // The sender always counts its own terminator into cbData (see
            // Window::forwardToSingleInstancePrimary) -- requiring it here too means a truncated or
            // malformed payload is refused rather than read for a NUL that may not be there.
            if (wtext[wlen - 1] != L'\0') return 0;
            const std::string path = wideToUtf8(wtext);
            // The hook is the ENTIRE decision. This wndProc does only the mechanical half -- decode,
            // ask, and on accept latch the path and take focus -- exactly the same division MessageHook
            // uses just above: the hook returns a verdict, the window procedure acts on it.
            auto hook = self->openRequestHook();
            if (!hook || !hook(self->openRequestHookUser(), path.c_str())) return 0;
            self->setPendingOpenRequest(path);
            self->focus();
            return 1;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Brings this window to the foreground, restoring it first if it is minimized. IsIconic/SW_RESTORE
// is needed specifically because SetForegroundWindow alone does not un-minimize a window -- it only
// changes which window is active, and an active-but-minimized window is invisible all the same.
void Window::focus() {
    if (!nativeHandle_) return;
    HWND hwnd = static_cast<HWND>(nativeHandle_);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

// Publishes `hwnd` as the primary other launches should forward to (see the block comment above
// kSingleInstanceMutexName). A silent no-op if a primary already exists: CreateMutexW still hands
// this process a valid (closeable) handle even when ERROR_ALREADY_EXISTS fires, but that handle is
// not the mutex's first/true owner, so it is closed immediately rather than kept -- keeping it would
// cost nothing functionally (existence is what matters, not which handle), but a second live handle
// to a mutex this process does not own would misstate, to a future reader of this code, which
// process the file mapping's HWND actually belongs to.
void Window::registerAsSingleInstancePrimary(void* hwnd) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (!mutex) return;   // creation failed outright; this launch is simply not a primary
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return;
    }
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(HWND), kSingleInstanceMappingName);
    if (!mapping) { CloseHandle(mutex); return; }
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(HWND));
    if (!view) { CloseHandle(mapping); CloseHandle(mutex); return; }
    *reinterpret_cast<HWND*>(view) = static_cast<HWND>(hwnd);
    UnmapViewOfFile(view);
    // Deliberately never closed -- see the block comment above kSingleInstanceMutexName. The OS
    // closing both on process exit, for any reason including a crash, is the entire self-healing
    // mechanism this feature relies on.
    g_singleInstanceMutex = mutex;
    g_singleInstanceMapping = mapping;
}

// The other half of the same handshake, for a process that has not created a Window of its own yet
// (see createApplication, which calls this before constructing a SandboxApp at all). Every failure
// path here -- no mutex, no mapping, a dead HWND, a declined or timed-out send -- returns false and
// leaves the caller to open its own instance, exactly as the brief's failure-mode list requires.
bool Window::forwardToSingleInstancePrimary(const std::string& path) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kSingleInstanceMutexName);
    if (!mutex) return false;   // no primary alive; nothing to forward to
    // Existence check only, not a lock: the mutex is never acquired (WaitForSingleObject is never
    // called on it) by either side of this feature. Its only job is to exist for exactly as long as
    // the primary process does.
    CloseHandle(mutex);

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kSingleInstanceMappingName);
    if (!mapping) return false;   // the mutex exists but the mapping raced or is gone; fall through
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(HWND));
    HWND target = nullptr;
    if (view) { target = *reinterpret_cast<HWND*>(view); UnmapViewOfFile(view); }
    CloseHandle(mapping);
    if (!target || !IsWindow(target)) return false;

    const std::wstring wpath = utf8ToWide(path);
    COPYDATASTRUCT cds{};
    cds.dwData = 1;   // "open this path" -- the only message kind this channel carries
    cds.cbData = static_cast<DWORD>((wpath.size() + 1) * sizeof(wchar_t));   // +1: the sender counts
                                                                              // its own NUL in; the
                                                                              // receiver requires it.
    cds.lpData = const_cast<wchar_t*>(wpath.c_str());

    // SMTO_ABORTIFHUNG, not a bare SendMessage: the receiver's OpenRequestHook is meant to be a
    // cheap string compare, but if the primary is genuinely wedged at the OS level (not merely
    // showing an ImGui modal -- those are not native modal loops and do not block PeekMessageW/
    // WM_COPYDATA delivery at all, see OpenRequestHook's own comment) this must not hang the
    // launch that is only trying to open a file. 3000ms is generous for the intended cost and short
    // enough that nobody waits out a truly stuck process before their own window opens.
    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                                              SMTO_ABORTIFHUNG, 3000, &result);
    if (!sent) return false;   // timed out, or the primary was gone by the time this arrived
    return result != 0;        // the OpenRequestHook verdict, round-tripped through the message return
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
