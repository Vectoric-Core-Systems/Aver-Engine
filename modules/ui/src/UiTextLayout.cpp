// UiTextLayout queries: line text and the caret stops.
#include "aver/ui/UiTextLayout.hpp"

#include <cmath>

namespace aver::ui {

std::string_view UiTextLayout::lineText(usize i) const {
    if (i >= lines.size()) return {};
    if (legacy) return i < legacyLines.size() ? std::string_view(legacyLines[i]) : std::string_view();
    if (cpToByte.empty()) return {};
    const UiTextLine& l = lines[i];
    const usize n = cpCount();
    const usize b = l.srcBegin <= n ? cpToByte[l.srcBegin] : source.size();
    const usize e = l.srcEnd <= n ? cpToByte[l.srcEnd] : source.size();
    if (e < b || e > source.size()) return {};
    return std::string_view(source).substr(b, e - b);
}

usize UiTextLayout::lineOfIndex(usize cp) const {
    const usize n = lines.size();
    if (n == 0) return 0;
    for (usize i = 0; i < n; ++i) {
        if (cp < lines[i].srcEnd) return i;
        if (cp == lines[i].srcEnd && (i + 1 == n || lines[i + 1].srcBegin != cp)) return i;
    }
    return n - 1;
}

f32 UiTextLayout::caretX(usize cp) const {
    if (lines.empty()) return 0.0f;
    const UiTextLine& l = lines[lineOfIndex(cp)];
    if (l.stopEnd <= l.stopBegin || l.stopEnd > stops.size()) return 0.0f;
    f32 x = stops[l.stopBegin].x;
    for (u32 j = l.stopBegin; j < l.stopEnd; ++j) {
        if (stops[j].cp > cp) break;
        x = stops[j].x;
    }
    return x;
}

usize UiTextLayout::indexAt(f32 x, f32 y) const {
    if (lines.empty()) return 0;
    usize li = lines.size() - 1;
    for (usize i = 0; i < lines.size(); ++i) {
        if (y < lines[i].top + lines[i].height) { li = i; break; }
    }
    const UiTextLine& l = lines[li];
    if (l.stopEnd <= l.stopBegin || l.stopEnd > stops.size()) return l.srcBegin;
    usize best = stops[l.stopBegin].cp;
    f32 bestD = std::fabs(stops[l.stopBegin].x - x);
    for (u32 j = l.stopBegin + 1; j < l.stopEnd; ++j) {
        const f32 d = std::fabs(stops[j].x - x);
        if (d <= bestD) { bestD = d; best = stops[j].cp; }   // ties go to the later stop, like a midpoint test
    }
    return best;
}

} // namespace aver::ui
