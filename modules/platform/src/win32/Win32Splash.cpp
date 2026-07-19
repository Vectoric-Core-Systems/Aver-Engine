#include "aver/platform/Splash.hpp"
#include "aver/core/Log.hpp"

#include <Windows.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace aver {

static const wchar_t* kSplashClass = L"AverSplashWindow";

Splash::~Splash() { close(0); }

bool Splash::show(const std::string& pngPath) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(pngPath.c_str(), &w, &h, &n, 4);
    if (!px) { AVER_TRACE("[Splash] no splash image at {}", pngPath); return false; }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = kSplashClass;
    RegisterClassExW(&wc);

    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSplashClass, L"",
                                WS_POPUP, (sw - w) / 2, (sh - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) { stbi_image_free(px); return false; }

    // 32-bit top-down DIB; copy RGBA -> BGRA (opaque).
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bits) {
        auto* d = static_cast<unsigned char*>(bits);
        for (int i = 0; i < w * h; ++i) {
            d[i * 4 + 0] = px[i * 4 + 2]; // B
            d[i * 4 + 1] = px[i * 4 + 1]; // G
            d[i * 4 + 2] = px[i * 4 + 0]; // R
            d[i * 4 + 3] = 255;           // A (opaque)
        }
    }
    stbi_image_free(px);
    HGDIOBJ old = SelectObject(mem, bmp);

    POINT ptDst = {(sw - w) / 2, (sh - h) / 2}, ptSrc = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, 0};
    UpdateLayeredWindow(hwnd, screen, &ptDst, &sz, mem, &ptSrc, 0, &bf, ULW_OPAQUE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    SelectObject(mem, old);
    ReleaseDC(nullptr, screen);
    hwnd_ = hwnd; memDc_ = mem; bitmap_ = bmp;
    shownAtMs_ = GetTickCount64();
    AVER_INFO("[Splash] shown ({}x{})", w, h);
    return true;
}

void Splash::pump() {
    if (!hwnd_) return;
    MSG msg;
    while (PeekMessageW(&msg, static_cast<HWND>(hwnd_), 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Splash::close(u32 minVisibleMs) {
    if (!hwnd_) return;
    while (GetTickCount64() - shownAtMs_ < minVisibleMs) { pump(); Sleep(10); }
    DestroyWindow(static_cast<HWND>(hwnd_));
    if (bitmap_) DeleteObject(static_cast<HBITMAP>(bitmap_));
    if (memDc_) DeleteDC(static_cast<HDC>(memDc_));
    hwnd_ = nullptr; bitmap_ = nullptr; memDc_ = nullptr;
}

} // namespace aver
