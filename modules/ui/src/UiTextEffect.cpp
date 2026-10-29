#include "aver/ui/UiTextEffect.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

namespace {

constexpr f32 kTwoPi = 6.28318530717958647692f;

void ring(f32 radius, f32 cx, f32 cy, u32 rgba, std::vector<UiTextFxPass>& out) {
    const int n = std::max(8, std::min(32, static_cast<int>(std::ceil(kTwoPi * radius))));
    for (int i = 0; i < n; ++i) {
        const f32 a = kTwoPi * static_cast<f32>(i) / static_cast<f32>(n);
        out.push_back({cx + radius * std::cos(a), cy + radius * std::sin(a), rgba, false});
    }
}

} // namespace

void uiOutlineOffsets(f32 radius, std::vector<UiTextFxPass>& out) {
    if (radius < 0.5f) return;
    ring(radius, 0, 0, 0, out);
    if (radius >= 3.0f) ring(radius * 0.5f, 0, 0, 0, out);
}

std::vector<UiTextFxPass> uiTextFxPasses(const UiTextFxStyle& s, u32 fillRgba) {
    std::vector<UiTextFxPass> passes;
    const bool shadow = (s.shadowRgba >> 24) != 0;
    if (shadow) {
        if (s.shadowOutlineSize >= 0.5f) {
            std::vector<UiTextFxPass> o;
            uiOutlineOffsets(s.shadowOutlineSize, o);
            for (UiTextFxPass& p : o) passes.push_back({p.dx + s.shadowOffX, p.dy + s.shadowOffY, s.shadowRgba, false});
        }
        passes.push_back({s.shadowOffX, s.shadowOffY, s.shadowRgba, false});
    }
    if (s.outlineSize >= 0.5f && (s.outlineRgba >> 24) != 0) {
        std::vector<UiTextFxPass> o;
        uiOutlineOffsets(s.outlineSize, o);
        for (UiTextFxPass& p : o) passes.push_back({p.dx, p.dy, s.outlineRgba, false});
    }
    passes.push_back({0, 0, fillRgba, true});
    return passes;
}

UiTextFxGrow uiTextFxGrow(const UiTextFxStyle& s) {
    UiTextFxGrow g;
    const f32 o = s.outlineSize >= 0.5f && (s.outlineRgba >> 24) != 0 ? s.outlineSize : 0.0f;
    g.left = g.top = g.right = g.bottom = o;
    if ((s.shadowRgba >> 24) != 0) {
        const f32 so = s.shadowOutlineSize >= 0.5f ? s.shadowOutlineSize : 0.0f;
        g.left = std::max(g.left, so - s.shadowOffX);
        g.right = std::max(g.right, so + s.shadowOffX);
        g.top = std::max(g.top, so - s.shadowOffY);
        g.bottom = std::max(g.bottom, so + s.shadowOffY);
    }
    g.left = std::max(0.0f, g.left);
    g.top = std::max(0.0f, g.top);
    g.right = std::max(0.0f, g.right);
    g.bottom = std::max(0.0f, g.bottom);
    return g;
}

} // namespace aver::ui
