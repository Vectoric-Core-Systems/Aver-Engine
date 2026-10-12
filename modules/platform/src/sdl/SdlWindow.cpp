// aver::Window over SDL 3, everywhere but Windows (src/win32/Win32Window.cpp is the Windows one).
//
// SAME CLASS, SAME CONTRACT. Everything Window.hpp promises about the Win32 window holds here, with
// three differences worth knowing about, each because the platform does not have the Win32 thing:
//
//   * ONE EVENT QUEUE FOR EVERY WINDOW. Win32 delivers each message to its own window's procedure;
//     SDL hands every event of every window to whoever polls. So pumpEvents() on ANY Window drains the
//     whole queue and routes each event to the Window it belongs to, and offers every event --
//     including those for windows this class never created, such as Dear ImGui's secondary viewports
//     -- to the native event hook first. A second Window's own pumpEvents() then finds nothing left.
//   * NO MODAL MOVE/SIZE LOOP on X11 or Wayland: the frame loop keeps running while a window is
//     dragged, so the render tick and the modal flags are never needed and stay false.
//   * NO SINGLE-INSTANCE FORWARDING. forwardToSingleInstancePrimary answers false, which its contract
//     already defines as "open your own instance".
//
// Sizes: WindowDesc's width/height are logical 96-DPI sizes, as on Windows, and width()/height() are
// physical pixels. SDL measures windows in points; a point is a pixel on X11 at 1x, two on a Retina
// display, 1.5 on a 150% Wayland output. The conversions are SDL_GetWindowPixelDensity (points to
// pixels) and SDL_GetWindowDisplayScale (the user's chosen scale, the dpiScale() a 96-DPI layout is
// multiplied by).
#include "aver/platform/Window.hpp"
#include "SdlKeys.hpp"
#include "SdlVideo.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Version.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <clocale>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace aver {
namespace detail {

bool sdlVideoUp() {
    static const bool up = [] {
        // THE USER'S CHARACTER LOCALE, and only that category. A C++ program starts in the "C" locale,
        // and SDL's X11 backend converts a window title through Xlib's locale-dependent text calls:
        // under "C" the em dash in "Aver Engine \u2014 Editor" is not convertible, so SDL never sets
        // _NET_WM_NAME and the window manager decodes the raw UTF-8 bytes of WM_NAME as Latin-1 --
        // "Aver Engine \u00e2\u20ac\u201d Editor" in the title bar. Xlib's input methods need it too, for
        // typing anything outside ASCII. LC_CTYPE alone leaves LC_NUMERIC at "C", so every strtod and
        // printf in the engine still reads and writes '.' as the decimal point. A host that already
        // chose a character locale keeps it.
        if (const char* ctype = std::setlocale(LC_CTYPE, nullptr); !ctype || std::strcmp(ctype, "C") == 0)
            std::setlocale(LC_CTYPE, "");
        // Before SDL_Init, because SDL reads several of these only then.
        //
        // Closing the last window must NOT post SDL_EVENT_QUIT. Window::setCloseGuard can veto a close
        // (unsaved work), and a quit event behind its back would end the app anyway.
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        // A click on an unfocused window reaches it, as it does on Windows; the editor's first click
        // after alt-tabbing back is otherwise swallowed by the focus change.
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        // SDL inhibits the screensaver by default. Nothing on the Win32 side does, and an editor left
        // open overnight should let the display sleep.
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
        SDL_SetAppMetadata("Aver Engine", AVER_ENGINE_VERSION, "org.vectoric.aver");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            AVER_WARN("[Platform] SDL video unavailable ({}) - no window can be created", SDL_GetError());
            return false;
        }
        AVER_INFO("[Platform] SDL {}.{}.{}, video driver '{}'", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                  SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
        return true;
    }();
    return up;
}

} // namespace detail

namespace {

// Every live aver::Window, so an event polled by one of them can be handed to the one it belongs to.
std::vector<Window*>& liveWindows() {
    static std::vector<Window*> w;
    return w;
}

SDL_Window* sdlWindow(const Window* w) { return static_cast<SDL_Window*>(w->nativeHandle()); }

Window* ownerOf(const SDL_Event& ev) {
    SDL_Window* sw = SDL_GetWindowFromEvent(&ev);
    if (!sw) return nullptr;
    for (Window* w : liveWindows())
        if (sdlWindow(w) == sw) return w;
    return nullptr;
}

// Points (SDL's window coordinates) to the physical pixels every engine coordinate is in.
f32 pixelDensity(SDL_Window* sw) {
    const f32 d = SDL_GetWindowPixelDensity(sw);
    return d > 0.0f ? d : 1.0f;
}

// One event, for one of our windows. The SDL twin of Win32Window.cpp's wndProc switch, case for case.
void route(Window& self, const SDL_Event& ev) {
    SDL_Window* sw = sdlWindow(&self);
    switch (ev.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
            Event e; e.type = EventType::WindowClose;
            self.dispatch(e);
            // ASK BEFORE CLOSING -- see WM_CLOSE in Win32Window.cpp. A veto keeps the loop running so
            // the unsaved-changes prompt can be drawn and answered.
            if (self.mayClose()) self.requestClose();
            break;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            Event e; e.type = EventType::WindowResize;
            e.width = static_cast<u32>(std::max(0, ev.window.data1));
            e.height = static_cast<u32>(std::max(0, ev.window.data2));
            self.dispatch(e);
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            self.setDpiScale(SDL_GetWindowDisplayScale(sw));
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const i32 vk = detail::vkFromSdlScancode(static_cast<int>(ev.key.scancode));
            if (!vk) break;
            // Repeats are delivered, as Win32 delivers auto-repeated WM_KEYDOWNs.
            Event e; e.type = EventType::Key;
            e.key = vk;
            e.pressed = ev.type == SDL_EVENT_KEY_DOWN;
            self.dispatch(e);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            const f32 d = pixelDensity(sw);
            if (SDL_GetWindowRelativeMouseMode(sw)) self.addRelativeMotion(ev.motion.xrel * d, ev.motion.yrel * d);
            Event e; e.type = EventType::MouseMove;
            e.mouseX = static_cast<i32>(ev.motion.x * d);
            e.mouseY = static_cast<i32>(ev.motion.y * d);
            self.dispatch(e);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const int b = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1
                        : ev.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b < 0) break;
            const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            // Counted exactly as on Win32 so Window::mouseCapture() means the same thing. The capture
            // itself is SDL's own (SDL_HINT_MOUSE_AUTO_CAPTURE, on by default): a drag that leaves the
            // window still delivers its button-up here.
            if (down) ++self.mouseCapture();
            else if (self.mouseCapture() > 0) --self.mouseCapture();
            const f32 d = pixelDensity(sw);
            Event e; e.type = EventType::MouseButton;
            e.button = b;
            e.pressed = down;
            e.mouseX = static_cast<i32>(ev.button.x * d);
            e.mouseY = static_cast<i32>(ev.button.y * d);
            self.dispatch(e);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            // Already in notches with the Windows sign (positive = away from the user). FLIPPED means
            // the OS has natural scrolling on and has reversed the numbers; that is the user's choice,
            // and is left as made, the way Windows' own reversed-scroll settings are.
            Event e; e.type = EventType::MouseWheel;
            e.wheel = ev.wheel.y;
            self.dispatch(e);
            break;
        }
        // Key state held across a focus change must be dropped -- see EventType::FocusLost.
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            self.mouseCapture() = 0;
            Event e; e.type = EventType::FocusLost;
            self.dispatch(e);
            break;
        }
        case SDL_EVENT_DROP_FILE:
            if (self.acceptDroppedFiles() && ev.drop.data) self.addPendingDroppedFile(ev.drop.data);
            break;
        default:
            break;
    }
}

} // namespace

void Window::dispatch(const Event& e) {
    if (e.type == EventType::WindowResize) { width_ = e.width; height_ = e.height; }
    if (callback_) callback_(callbackUser_, e);
}

bool Window::create(const WindowDesc& desc) {
    if (!detail::sdlVideoUp()) return false;

    // AND ONLY WHEN THE WINDOW IS INTERACTIVE -- see WindowDesc::fullscreen.
    const bool wantFullscreen = desc.fullscreen && desc.activate;

    // Created HIDDEN in every case and revealed at the end, so it is sized and centred before it is
    // ever on screen (and stays hidden for WindowDesc::startHidden). SDL_WINDOW_VULKAN is what makes
    // SDL load the Vulkan loader and, on macOS, give the window a CAMetalLayer to present into.
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (wantFullscreen) flags |= SDL_WINDOW_FULLSCREEN;   // no display mode set: borderless desktop

    SDL_Window* sw = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                      static_cast<int>(desc.height), flags);
    if (!sw) {
        // No Vulkan loader on the machine: a window that cannot present is still a window.
        AVER_WARN("[Platform] SDL_CreateWindow with Vulkan failed ({}); retrying without", SDL_GetError());
        sw = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width), static_cast<int>(desc.height),
                              flags & ~SDL_WINDOW_VULKAN);
    }
    if (!sw) {
        AVER_WARN("[Platform] SDL_CreateWindow failed ({})", SDL_GetError());
        return false;
    }

    nativeHandle_ = sw;
    quitOnDestroy_ = desc.quitOnDestroy;
    liveWindows().push_back(this);

    // desc.width/height are logical 96-DPI sizes: scale, clamp to 92% of the work area, centre --
    // the Win32 window's rule. SDL sizes are in points, so the scale applied is the display scale
    // divided by the points-to-pixels density SDL already accounts for.
    dpiScale_ = SDL_GetWindowDisplayScale(sw);
    if (dpiScale_ <= 0.0f) dpiScale_ = 1.0f;
    if (!wantFullscreen) {
        const f32 toPoints = dpiScale_ / pixelDensity(sw);
        int cw = static_cast<int>(static_cast<f32>(desc.width) * toPoints + 0.5f);
        int ch = static_cast<int>(static_cast<f32>(desc.height) * toPoints + 0.5f);
        SDL_Rect usable{};
        const SDL_DisplayID display = SDL_GetDisplayForWindow(sw);
        if (display && SDL_GetDisplayUsableBounds(display, &usable)) {
            cw = std::min(cw, usable.w * 92 / 100);
            ch = std::min(ch, usable.h * 92 / 100);
        }
        SDL_SetWindowSize(sw, cw, ch);
        SDL_SetWindowPosition(sw, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        SDL_SyncWindow(sw);
    }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(sw, &pw, &ph);
    width_ = static_cast<u32>(std::max(pw, 0));
    height_ = static_cast<u32>(std::max(ph, 0));

    if (!desc.startHidden) show(desc.activate);
    AVER_INFO("[Platform] window '{}' {}x{} created ({}x{} pixels, scale {:.2f})", desc.title, desc.width,
              desc.height, width_, height_, dpiScale_);
    return true;
}

void Window::show(bool activate) {
    if (!nativeHandle_) return;
    // A capture run must not take the keyboard from whoever owns the machine -- see WindowDesc::activate.
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, activate ? "1" : "0");
    SDL_ShowWindow(sdlWindow(this));
}

void Window::setTitle(const std::string& title) {
    if (nativeHandle_) SDL_SetWindowTitle(sdlWindow(this), title.c_str());
}

bool Window::frameSize(i32& w, i32& h) const {
    if (!nativeHandle_) return false;
    SDL_Window* sw = sdlWindow(this);
    int cw = 0, ch = 0, top = 0, left = 0, bottom = 0, right = 0;
    if (!SDL_GetWindowSize(sw, &cw, &ch)) return false;
    SDL_GetWindowBordersSize(sw, &top, &left, &bottom, &right);   // zeros where the WM draws none, or will not say
    w = cw + left + right;
    h = ch + top + bottom;
    return true;
}

void Window::setFrameSize(i32 w, i32 h) {
    if (!nativeHandle_) return;
    SDL_Window* sw = sdlWindow(this);
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(sw, &top, &left, &bottom, &right);
    SDL_SetWindowSize(sw, std::max(1, w - left - right), std::max(1, h - top - bottom));
}

void Window::pumpEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        // The toolkit sees everything first, whichever window it is for -- see the file header.
        for (Window* w : liveWindows()) {
            if (NativeEventHook hook = w->nativeEventHook()) { hook(&ev); break; }
        }
        if (ev.type == SDL_EVENT_QUIT) {
            // SIGINT/SIGTERM, or the OS asking the app to quit: the WM_QUIT of this backend, and like
            // WM_QUIT it ends the loop without asking the close guard.
            for (Window* w : liveWindows())
                if (w->quitOnDestroy()) w->requestClose();
            continue;
        }
        if (Window* owner = ownerOf(ev)) route(*owner, ev);
    }
}

void Window::destroy() {
    if (!nativeHandle_) return;
    auto& live = liveWindows();
    live.erase(std::remove(live.begin(), live.end(), this), live.end());
    SDL_DestroyWindow(sdlWindow(this));
    nativeHandle_ = nullptr;
    // WM_DESTROY's PostQuitMessage: the app ends when its main window does.
    if (quitOnDestroy_) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
}

Window::~Window() { destroy(); }

void Window::focus() {
    if (!nativeHandle_) return;
    SDL_Window* sw = sdlWindow(this);
    if (SDL_GetWindowFlags(sw) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(sw);
    SDL_RaiseWindow(sw);
}

bool Window::isForeground() const {
    return nativeHandle_ && (SDL_GetWindowFlags(sdlWindow(this)) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

// SDL delivers SDL_EVENT_DROP_FILE to every window; route() keeps or drops it on this flag.
void Window::setAcceptDroppedFiles(bool accept) { acceptDroppedFiles_ = accept; }

void Window::registerAsSingleInstancePrimary(void*) {}
bool Window::forwardToSingleInstancePrimary(const std::string&) { return false; }

void Window::setRelativeMouse(bool on) {
    if (!nativeHandle_) return;
    if (!SDL_SetWindowRelativeMouseMode(sdlWindow(this), on))
        AVER_WARN("[Platform] relative mouse mode {} failed ({})", on ? "on" : "off", SDL_GetError());
    relDx_ = relDy_ = 0.0f;
}

bool keyDownNow(i32 vk) {
    if (vk <= 0 || !SDL_WasInit(SDL_INIT_VIDEO)) return false;
    int count = 0;
    const bool* state = SDL_GetKeyboardState(&count);
    // Every scancode that maps to this code: VK_SHIFT is down when either Shift is, as on Windows.
    for (int sc = 1; state && sc < count; ++sc)
        if (state[sc] && detail::vkFromSdlScancode(sc) == vk) return true;
    return false;
}

void* vulkanGetInstanceProcAddr() {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return nullptr;
    // Null until a window was made with SDL_WINDOW_VULKAN (that is what loads the library), or when
    // there is no Vulkan loader at all; the GPU backend then loads its own.
    return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr());
}

bool createVulkanSurface(void* nativeHandle, void* vkInstance, u64* outSurface) {
    if (!nativeHandle || !vkInstance || !outSurface) return false;
    VkSurfaceKHR surface = 0;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(nativeHandle), static_cast<VkInstance>(vkInstance),
                                  nullptr, &surface)) {
        AVER_ERROR("[Platform] SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return false;
    }
    *outSurface = reinterpret_cast<u64>(surface);
    return true;
}

} // namespace aver
