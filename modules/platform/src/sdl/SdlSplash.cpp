// aver::Splash over SDL 3, everywhere but Windows (src/win32/Win32Splash.cpp is the Windows one).
//
// The same picture as the Win32 splash: the PNG, DPI-scaled and centred, a status line in the bottom
// margin with a dark offset behind it, and the progress bar under that. Drawn in software into one
// surface and copied to the window, as the Win32 one composites a DIB, so nothing here needs a GPU.
//
// THE TEXT IS SDL'S BUILT-IN 8x8 FONT (SDL_RenderDebugText), scaled to the size Win32 asks Segoe UI
// for. The status lines are ASCII by construction ("Compiling shaders", "Loading level"), and this
// keeps a font rasteriser and a font file out of the platform layer for one line of loading text.
//
// Wayland does not let a client place its own windows, so there the splash appears wherever the
// compositor puts a new window (usually centred), and "always on top" is a request it may ignore.
#include "aver/platform/Splash.hpp"
#include "aver/platform/Image.hpp"
#include "SdlVideo.hpp"
#include "aver/core/Log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace aver {
namespace {

// Box-filter resample, RGBA8 -- the same filter Win32Splash.cpp applies to its BGRA copy.
void resampleRgba(const u8* src, int sw, int sh, u8* dst, int dw, int dh) {
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
            for (int c = 0; c < 4; ++c) q[c] = static_cast<u8>(acc[c] / n);
        }
    }
}

SDL_Window* win(void* p) { return static_cast<SDL_Window*>(p); }
SDL_Surface* surf(void* p) { return static_cast<SDL_Surface*>(p); }
SDL_Renderer* rend(void* p) { return static_cast<SDL_Renderer*>(p); }

}  // namespace

Splash::~Splash() { close(0); }

bool Splash::show(const std::string& pngPath) {
    ImageData img;
    if (!decodeImage(pngPath, img)) { AVER_TRACE("[Splash] no splash image at {}", pngPath); return false; }
    const int sw = static_cast<int>(img.width), sh = static_cast<int>(img.height);
    if (sw <= 0 || sh <= 0) return false;
    if (!detail::sdlVideoUp()) return false;

    // The Win32 rule: display scale, at most 2x, at most a third of the display's width, at least
    // 0.5x. The display mode is in points; times its pixel density it is the physical width the
    // Win32 rule measures against.
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    f32 scale = display ? SDL_GetDisplayContentScale(display) : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;
    const SDL_DisplayMode* mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
    const f32 density = mode && mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
    scale = std::min(scale, 2.0f);
    if (mode) scale = std::min(scale, static_cast<f32>(mode->w) * density / 3.0f / static_cast<f32>(sw));
    scale = std::max(scale, 0.5f);
    scale_ = scale;
    width_  = std::max(1, static_cast<int>(static_cast<f32>(sw) * scale + 0.5f));
    height_ = std::max(1, static_cast<int>(static_cast<f32>(sh) * scale + 0.5f));

    // Opaque, like the Win32 DIB: the splash art's alpha is ignored.
    std::vector<u8> src(img.pixels);
    for (usize i = 3; i < src.size(); i += 4) src[i] = 255;
    pristine_.assign(static_cast<usize>(width_) * height_ * 4, 0);
    if (width_ == sw && height_ == sh) pristine_ = src;
    else resampleRgba(src.data(), sw, sh, pristine_.data(), width_, height_);

    // UTILITY keeps it out of the task bar, NOT_FOCUSABLE keeps it from taking the keyboard -- the
    // WS_EX_TOOLWINDOW and SW_SHOWNOACTIVATE of the Win32 one.
    const SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_UTILITY |
                                  SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_HIDDEN;
    // Window sizes are in points and the image was scaled in pixels. The density is 1 everywhere but a
    // HiDPI macOS or Wayland display, where a pixel size passed as points would double it a second
    // time; repaint() stretches to whatever backing size the window system then gives it.
    SDL_Window* w = SDL_CreateWindow("Aver Engine", std::max(1, static_cast<int>(width_ / density + 0.5f)),
                                     std::max(1, static_cast<int>(height_ / density + 0.5f)), flags);
    if (!w) { AVER_WARN("[Splash] SDL_CreateWindow failed ({})", SDL_GetError()); return false; }
    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));

    SDL_Surface* canvas = SDL_CreateSurface(width_, height_, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* r = canvas ? SDL_CreateSoftwareRenderer(canvas) : nullptr;
    if (!canvas || !r) {
        AVER_WARN("[Splash] could not create the drawing surface ({})", SDL_GetError());
        if (canvas) SDL_DestroySurface(canvas);
        SDL_DestroyWindow(w);
        return false;
    }
    window_ = w; canvas_ = canvas; renderer_ = r;
    shownAtMs_ = SDL_GetTicks();
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_ShowWindow(w);
    repaint();
    AVER_INFO("[Splash] shown ({}x{} at {:.0f}% scale, source {}x{})", width_, height_, scale * 100.0f, sw, sh);
    return true;
}

void Splash::setStatus(const std::string& text) {
    if (status_ == text) return;
    status_ = text;
    if (!window_) return;   // remembered; drawn when show() runs
    repaint();
    pump();
}

// See the header for the negative-hides-the-bar contract, and Win32Splash.cpp for the 30 Hz throttle.
void Splash::setProgress(f32 fraction) {
    const f32 clamped = fraction < 0.0f ? -1.0f : (fraction > 1.0f ? 1.0f : fraction);
    if (progress_ == clamped) return;
    progress_ = clamped;
    if (!window_) return;
    repaint();
    const u64 now = SDL_GetTicks();
    if (clamped >= 0.0f && lastProgressPumpMs_ != 0 && now - lastProgressPumpMs_ < 33) return;
    lastProgressPumpMs_ = now;
    pump();
}

void Splash::repaint() {
    if (!window_ || !canvas_ || pristine_.empty()) return;
    SDL_Surface* c = surf(canvas_);
    SDL_LockSurface(c);
    for (int y = 0; y < height_; ++y)
        std::memcpy(static_cast<u8*>(c->pixels) + static_cast<usize>(y) * c->pitch,
                    pristine_.data() + static_cast<usize>(y) * width_ * 4, static_cast<usize>(width_) * 4);
    SDL_UnlockSurface(c);

    const int pad = std::max(8, width_ / 40);
    SDL_Renderer* r = rend(renderer_);
    if (!status_.empty()) {
        // Win32 asks Segoe UI for max(11, 13 * scale) px; the 8x8 glyphs are scaled by the whole number
        // that comes closest, so they stay sharp.
        const f32 wantPx = std::max(11.0f, 13.0f * scale_);
        const int glyph = std::max(1, static_cast<int>(wantPx / 8.0f + 0.5f));
        const int maxChars = std::max(0, (width_ - 2 * pad) / (8 * glyph));
        std::string line = status_;
        if (static_cast<int>(line.size()) > maxChars)
            line = maxChars > 3 ? line.substr(0, static_cast<usize>(maxChars - 3)) + "..." : std::string();
        const f32 x = static_cast<f32>(pad) / glyph;
        const f32 y = static_cast<f32>(height_ - pad - 8 * glyph - pad / 2) / glyph;
        SDL_SetRenderScale(r, static_cast<f32>(glyph), static_cast<f32>(glyph));
        // A dark offset behind the text: light-on-light is how a status line vanishes on someone
        // else's splash art.
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderDebugText(r, x + 1.0f / glyph, y + 1.0f / glyph, line.c_str());
        SDL_SetRenderDrawColor(r, 225, 225, 230, 255);
        SDL_RenderDebugText(r, x, y, line.c_str());
        SDL_SetRenderScale(r, 1.0f, 1.0f);
    }
    if (progress_ >= 0.0f) {
        const int barH = std::max(3, pad / 3);
        int barY = height_ - pad + (pad - barH) / 2;
        if (barY + barH > height_ - 1) barY = height_ - 1 - barH;
        if (barY < 0) barY = 0;
        const SDL_FRect track{static_cast<f32>(pad), static_cast<f32>(barY),
                              static_cast<f32>(width_ - 2 * pad), static_cast<f32>(barH)};
        SDL_SetRenderDrawColor(r, 60, 60, 65, 255);
        SDL_RenderFillRect(r, &track);
        SDL_FRect fill = track;
        fill.w = track.w * progress_;
        if (fill.w > 0.0f) {
            SDL_SetRenderDrawColor(r, 225, 225, 230, 255);
            SDL_RenderFillRect(r, &fill);
        }
    }
    SDL_FlushRenderer(r);

    // To the window, stretched to whatever pixel size the window system actually gave it.
    SDL_Window* w = win(window_);
    if (SDL_Surface* screen = SDL_GetWindowSurface(w)) {
        SDL_BlitSurfaceScaled(c, nullptr, screen, nullptr, SDL_SCALEMODE_LINEAR);
        SDL_UpdateWindowSurface(w);
    }
}

// Lets the window system talk to the splash. SDL_PumpEvents only MOVES the OS's events into SDL's
// queue; nothing is taken out, so input meant for the main window is still there for its own
// pumpEvents() -- the Win32 splash gets the same by pumping only its own HWND's messages.
void Splash::pump() {
    if (!window_) return;
    SDL_PumpEvents();
}

void Splash::close(u32 minVisibleMs) {
    if (!window_) return;
    while (SDL_GetTicks() - shownAtMs_ < minVisibleMs) { pump(); SDL_Delay(10); }
    if (renderer_) SDL_DestroyRenderer(rend(renderer_));
    if (canvas_) SDL_DestroySurface(surf(canvas_));
    SDL_DestroyWindow(win(window_));
    window_ = nullptr; canvas_ = nullptr; renderer_ = nullptr;
    pristine_.clear();
}

} // namespace aver
