// Win32 backend for Splash: a layered PNG window shown during startup, with a status line.

#include "aver/platform/Splash.hpp"
#include "aver/platform/Image.hpp"
#include "aver/core/Log.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace aver {

static const wchar_t* kSplashClass = L"AverSplashWindow";

namespace {

// The DPI of the monitor the cursor is on, or 96 when the OS is too old to say.
//
// GetDpiForMonitor IS RESOLVED DYNAMICALLY rather than linked. It lives in Shcore.dll, which would
// otherwise become a hard link-time dependency of Aver.Platform for one number with a sane default.
u32 monitorDpi(HMONITOR mon) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    u32 dpi = 96;
    if (HMODULE sh = LoadLibraryW(L"Shcore.dll")) {
        auto fn = reinterpret_cast<GetDpiForMonitorFn>(
            reinterpret_cast<void*>(GetProcAddress(sh, "GetDpiForMonitor")));
        UINT x = 96, y = 96;
        if (fn && SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) dpi = x;
        FreeLibrary(sh);
    }
    return dpi;
}

// Box-filtered downscale/upscale of a BGRA image. GDI's StretchBlt would do this, but HALFTONE mode
// on a layered window's DIB gives visibly worse edges on a logo than averaging does, and the whole
// image is a few hundred kilobytes -- this runs once.
void resampleBgra(const u8* src, int sw, int sh, u8* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int sy0 = y * sh / dh, sy1 = std::max(sy0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx0 = x * sw / dw, sx1 = std::max(sx0 + 1, (x + 1) * sw / dw);
            u32 acc[4] = {0, 0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx) {
                    const u8* p = src + (static_cast<usize>(sy) * sw + sx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                    ++n;
                }
            u8* q = dst + (static_cast<usize>(y) * dw + x) * 4;
            q[0] = static_cast<u8>(acc[0] / n); q[1] = static_cast<u8>(acc[1] / n);
            q[2] = static_cast<u8>(acc[2] / n); q[3] = static_cast<u8>(acc[3] / n);
        }
    }
}

} // namespace

Splash::~Splash() { close(0); }

// Decodes the PNG and shows it centred and topmost. False if it cannot be loaded or created.
bool Splash::show(const std::string& pngPath) {
    ImageData img;
    if (!decodeImage(pngPath, img)) { AVER_TRACE("[Splash] no splash image at {}", pngPath); return false; }
    const int sw = static_cast<int>(img.width), sh = static_cast<int>(img.height);
    if (sw <= 0 || sh <= 0) return false;

    // CENTRE ON THE MONITOR THE USER IS LOOKING AT, not on the primary one. SM_CXSCREEN only ever
    // describes the primary display, so on a multi-monitor desktop the splash would open on a
    // different screen from the editor window that follows it.
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // SIZE IT FOR THE DISPLAY. The source PNG is authored at one size; on a 300% display it would
    // otherwise appear a third as large as intended. Scale by DPI, then clamp to a third of the
    // monitor so a very high DPI or a very large source cannot produce a splash that dominates the
    // screen -- and never upscale past 2x, where the logo would start to look soft.
    const u32 dpi = monitorDpi(mon);
    f32 scale = static_cast<f32>(dpi) / 96.0f;
    scale = std::min(scale, 2.0f);
    scale = std::min(scale, static_cast<f32>(monW) / 3.0f / static_cast<f32>(sw));
    scale = std::max(scale, 0.5f);
    width_  = std::max(1, static_cast<int>(static_cast<f32>(sw) * scale + 0.5f));
    height_ = std::max(1, static_cast<int>(static_cast<f32>(sh) * scale + 0.5f));

    // Source RGBA -> BGRA, then resample to the display size. The pristine copy is what every
    // repaint starts from.
    std::vector<u8> srcBgra(static_cast<usize>(sw) * sh * 4);
    for (int i = 0; i < sw * sh; ++i) {
        srcBgra[i * 4 + 0] = img.pixels[i * 4 + 2];
        srcBgra[i * 4 + 1] = img.pixels[i * 4 + 1];
        srcBgra[i * 4 + 2] = img.pixels[i * 4 + 0];
        srcBgra[i * 4 + 3] = 255;
    }
    pristine_.assign(static_cast<usize>(width_) * height_ * 4, 0);
    if (width_ == sw && height_ == sh) pristine_ = srcBgra;
    else resampleBgra(srcBgra.data(), sw, sh, pristine_.data(), width_, height_);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kSplashClass;
    RegisterClassExW(&wc);

    posX_ = mi.rcMonitor.left + (monW - width_) / 2;
    posY_ = mi.rcMonitor.top  + (monH - height_) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSplashClass, L"",
                                WS_POPUP, posX_, posY_, width_, height_, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width_;
    bi.bmiHeader.biHeight = -height_;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, bmp);

    // The status font scales with the splash, so the line stays the same size relative to the logo
    // whatever display it lands on.
    const int fontPx = std::max(11, static_cast<int>(13.0f * scale + 0.5f));
    font_ = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    ReleaseDC(nullptr, screen);
    hwnd_ = hwnd; memDc_ = mem; bitmap_ = bmp; bits_ = bits;
    shownAtMs_ = GetTickCount64();

    repaint();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    AVER_INFO("[Splash] shown ({}x{} at {}%% DPI, source {}x{})", width_, height_,
              dpi * 100 / 96, sw, sh);
    return true;
}

void Splash::setStatus(const std::string& text) {
    if (status_ == text) return;
    status_ = text;
    if (!hwnd_) return;   // remembered; drawn when show() runs
    repaint();
    pump();
}

// Re-composites the pristine image plus the status line and pushes it to the layered window.
void Splash::repaint() {
    if (!hwnd_ || !bits_ || pristine_.empty()) return;
    std::memcpy(bits_, pristine_.data(), pristine_.size());

    HDC mem = static_cast<HDC>(memDc_);
    if (!status_.empty() && font_) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, status_.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<usize>(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, status_.c_str(), -1, w.data(), n);

        HGDIOBJ oldFont = SelectObject(mem, static_cast<HFONT>(font_));
        SetBkMode(mem, TRANSPARENT);
        const int pad = std::max(8, width_ / 40);
        RECT r{ pad, height_ - pad - (height_ / 12), width_ - pad, height_ - pad };

        // A one-pixel dark offset behind the text. The splash art is not guaranteed to be dark in
        // the corner the line sits in, and light-on-light is how a status message becomes invisible
        // on someone else's branding.
        SetTextColor(mem, RGB(0, 0, 0));
        RECT sh1 = r; sh1.left += 1; sh1.top += 1;
        DrawTextW(mem, w.c_str(), -1, &sh1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(mem, RGB(225, 225, 230));
        DrawTextW(mem, w.c_str(), -1, &r, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem, oldFont);
    }

    // The DIB is opaque, so alpha stays 255 everywhere; GDI text drawing leaves the alpha byte
    // untouched anyway, which is exactly the bug that makes text vanish on a per-pixel-alpha layered
    // window. ULW_OPAQUE sidesteps it by ignoring alpha entirely.
    HDC screen = GetDC(nullptr);
    POINT ptDst = { posX_, posY_ }, ptSrc = { 0, 0 };
    SIZE sz = { width_, height_ };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, 0 };
    UpdateLayeredWindow(static_cast<HWND>(hwnd_), screen, &ptDst, &sz, mem, &ptSrc, 0, &bf, ULW_OPAQUE);
    ReleaseDC(nullptr, screen);
}

// Drains the splash window's queued messages.
void Splash::pump() {
    if (!hwnd_) return;
    MSG msg;
    while (PeekMessageW(&msg, static_cast<HWND>(hwnd_), 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Waits out `minVisibleMs` since it was shown, then destroys the window and its GDI objects.
void Splash::close(u32 minVisibleMs) {
    if (!hwnd_) return;
    while (GetTickCount64() - shownAtMs_ < minVisibleMs) { pump(); Sleep(10); }
    DestroyWindow(static_cast<HWND>(hwnd_));
    if (bitmap_) DeleteObject(static_cast<HBITMAP>(bitmap_));
    if (memDc_)  DeleteDC(static_cast<HDC>(memDc_));
    if (font_)   DeleteObject(static_cast<HFONT>(font_));
    hwnd_ = nullptr; bitmap_ = nullptr; memDc_ = nullptr; font_ = nullptr; bits_ = nullptr;
    pristine_.clear();
}

} // namespace aver
