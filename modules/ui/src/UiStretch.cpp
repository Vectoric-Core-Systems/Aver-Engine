// Stretch and letterbox arithmetic (UiStretch.hpp).
#include "aver/ui/UiStretch.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

namespace {
bool nearEq(f32 a, f32 b) { return std::fabs(a - b) <= 1e-5f * std::max(1.0f, std::max(std::fabs(a), std::fabs(b))); }
}

UiStretchResult uiComputeStretch(const UiStretchSettings& s, f32 windowW, f32 windowH) {
    UiStretchResult out;
    if (windowW <= 0.0f || windowH <= 0.0f) return out;
    const f32 factor = s.scale > 0.0f ? s.scale : 1.0f;

    if (s.mode == UiStretchMode::Disabled || s.baseW <= 0.0f || s.baseH <= 0.0f) {
        out.screen = {0, 0, windowW, windowH};
        out.contentSize = {windowW / factor, windowH / factor};
        out.renderSize = {windowW, windowH};
        out.scaleX = out.scaleY = factor;
        return out;
    }

    const UiVec2 desired{s.baseW, s.baseH};
    const f32 viewportAspect = desired.x / desired.y, videoAspect = windowW / windowH;
    UiVec2 viewport, screen;
    if (s.aspect == UiStretchAspect::Ignore || nearEq(viewportAspect, videoAspect)) {
        viewport = desired;
        screen = {windowW, windowH};
    } else if (viewportAspect < videoAspect) {
        // The window is wider than the base: bars at the sides, or grow the width.
        if (s.aspect == UiStretchAspect::KeepHeight || s.aspect == UiStretchAspect::Expand) {
            viewport = {desired.y * videoAspect, desired.y};
            screen = {windowW, windowH};
        } else {
            viewport = desired;
            screen = {windowH * viewportAspect, windowH};
        }
    } else {
        if (s.aspect == UiStretchAspect::KeepWidth || s.aspect == UiStretchAspect::Expand) {
            viewport = {desired.x, desired.x / videoAspect};
            screen = {windowW, windowH};
        } else {
            viewport = desired;
            screen = {windowW, windowW / viewportAspect};
        }
    }

    // Integer scaling: the picture is a whole multiple of the base size (at least 1).
    if (s.scaleMode == UiStretchScaleMode::Integer) {
        const f32 k = std::max(1.0f, std::floor(std::min(screen.x / viewport.x, screen.y / viewport.y)));
        if (screen.x >= viewport.x && screen.y >= viewport.y) screen = {viewport.x * k, viewport.y * k};
    }

    screen = {std::floor(screen.x), std::floor(screen.y)};
    viewport = {std::floor(viewport.x), std::floor(viewport.y)};

    UiVec2 margin{0, 0};
    if (s.aspect != UiStretchAspect::Expand && screen.x < windowW) margin.x = std::round((windowW - screen.x) / 2.0f);
    if (s.aspect != UiStretchAspect::Expand && screen.y < windowH) margin.y = std::round((windowH - screen.y) / 2.0f);
    out.letterboxed = margin.x > 0.0f || margin.y > 0.0f;
    out.screen = {margin.x, margin.y, screen.x, screen.y};

    if (s.mode == UiStretchMode::CanvasItems) {
        out.contentSize = {viewport.x / factor, viewport.y / factor};
        out.renderSize = screen;
        out.scaleX = screen.x / out.contentSize.x;
        out.scaleY = screen.y / out.contentSize.y;
    } else {
        const UiVec2 fin{std::floor(viewport.x / factor), std::floor(viewport.y / factor)};
        if (fin.x < 1.0f || fin.y < 1.0f) {   // the factor shrank the picture to nothing: keep it drawable
            out.contentSize = {1, 1};
            out.renderSize = {1, 1};
        } else {
            out.contentSize = fin;
            out.renderSize = fin;
        }
        out.scaleX = screen.x / out.contentSize.x;
        out.scaleY = screen.y / out.contentSize.y;
    }
    return out;
}

UiVec2 uiStretchWindowToContent(const UiStretchResult& r, f32 x, f32 y) {
    return {(x - r.screen.x) / (r.scaleX > 0 ? r.scaleX : 1.0f), (y - r.screen.y) / (r.scaleY > 0 ? r.scaleY : 1.0f)};
}

UiVec2 uiStretchContentToWindow(const UiStretchResult& r, f32 x, f32 y) {
    return {x * r.scaleX + r.screen.x, y * r.scaleY + r.screen.y};
}

} // namespace aver::ui
