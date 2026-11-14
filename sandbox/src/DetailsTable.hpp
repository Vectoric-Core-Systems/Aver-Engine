#pragma once
// The Details panel as a two-column label/value table: labels left, values in a column whose split the
// user drags (persisted as a fraction in the editor prefs), a search box that hides rows by label, a
// modified mark and reset arrow on rows given a default, and X/Y/Z tinted red/green/blue.
//
// The dt:: widgets mirror the ImGui ones they replace (same arguments, same last-item semantics, so
// IsItemActivated / IsItemDeactivatedAfterEdit keep working); outside begin()..end(), in another window,
// or for a "##id" label they are the plain ImGui widget. Reset never writes a component itself: it writes
// the default into the caller's local value and reports it via consumeReset(), and the call site runs the
// same undo bracket an edit would.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace aver::editor {

// ---- pure half (no ImGui; tests/editor/DetailsTableTest) ---------------------------------------------

// The text shown for an ImGui label: everything before "##".
inline std::string_view detailsVisibleLabel(std::string_view label) {
    const auto at = label.find("##");
    return at == std::string_view::npos ? label : label.substr(0, at);
}

// True when every whitespace-separated token of `filter` occurs in `label` (ASCII case-insensitive).
// An empty filter matches everything.
inline bool detailsLabelMatches(std::string_view label, std::string_view filter) {
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    usize at = 0;
    while (at < filter.size()) {
        while (at < filter.size() && std::isspace(static_cast<unsigned char>(filter[at]))) ++at;
        usize end = at;
        while (end < filter.size() && !std::isspace(static_cast<unsigned char>(filter[end]))) ++end;
        if (end == at) break;
        const std::string_view tok = filter.substr(at, end - at);
        at = end;
        bool found = false;
        for (usize i = 0; !found && i + tok.size() <= label.size(); ++i) {
            usize k = 0;
            while (k < tok.size() && lower(label[i + k]) == lower(tok[k])) ++k;
            found = k == tok.size();
        }
        if (!found) return false;
    }
    return true;
}

// Whether a value differs from its default (any component beyond `eps`).
inline bool detailsDiffers(const f32* cur, const f32* def, int n, f32 eps = 1e-4f) {
    for (int i = 0; i < n; ++i)
        if (std::fabs(cur[i] - def[i]) > eps) return true;
    return false;
}

// The label column's width in pixels for a persisted split fraction: at least `minLabel`, and the value
// column keeps at least `minValue` while the panel is wide enough for both.
inline f32 detailsSplitPx(f32 fraction, f32 avail, f32 scale) {
    const f32 minLabel = 70.0f * scale, minValue = 90.0f * scale;
    f32 px = fraction * avail;
    if (px > avail - minValue) px = avail - minValue;
    if (px < minLabel) px = minLabel;
    return px;
}

} // namespace aver::editor

#if AVER_WITH_IMGUI
#include <imgui.h>
#include <imgui_internal.h>   // RenderTextEllipsis

#include "EditorChrome.hpp"
#include "EditorIcons.hpp"
#include "EditorTheme.hpp"
#include "EditorWidgets.hpp"

#include <cfloat>
#include <cstdio>

namespace aver::editor::dt {

inline constexpr const char* kSplitKey = "details.split";

struct State {
    bool active = false;
    std::string filter;
    f32 fraction = -1.0f;        // label column share of the content width; <0 = not loaded yet
    f32 avail = 0.0f, splitPx = 0.0f, scale = 1.0f;
    ImDrawList* dl = nullptr;    // the panel window's draw list: rows in a popup or child take the plain path
    f32 topY = 0.0f;             // screen y under the search box (where the splitter handle starts)
    bool dragging = false;
    int shown = 0;               // rows drawn this frame (the "no match" hint)
    bool resetClicked = false;
    f32 def[4] = {};
    int defN = 0;
    f32 reserveRight = 0.0f;
    f32 labelNeed = 0.0f, labelNeedNext = 0.0f;   // widest label + reset slot: last frame's, this frame's
};
inline State& st() { static State s; return s; }

// Call right after Begin() of the Details window.
inline void begin() {
    State& s = st();
    s.active = true;
    s.shown = 0;
    s.dl = ImGui::GetWindowDrawList();
    s.scale = fontScale();
    s.avail = ImGui::GetContentRegionAvail().x;
    if (s.fraction < 0.0f) s.fraction = loadSplitFraction(kSplitKey, 0.40f);
    s.splitPx = detailsSplitPx(s.fraction, s.avail, s.scale);
    // The label column grows to fit last frame's widest label, up to half the panel, so labels are not cut.
    s.labelNeed = s.labelNeedNext;
    s.labelNeedNext = 0.0f;
    if (s.labelNeed > s.splitPx) s.splitPx = std::max(s.splitPx, std::min(s.labelNeed, s.avail * 0.5f));

    char buf[96];
    std::snprintf(buf, sizeof buf, "%s", s.filter.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##detailsSearch", ICON_SEARCH " Search", buf, sizeof buf)) s.filter = buf;
    forceSectionsOpen() = !s.filter.empty();
    s.topY = ImGui::GetCursorScreenPos().y;
}

// Call before the Details window's End(): the empty-search hint and the split handle.
inline void end() {
    State& s = st();
    if (!s.active) return;
    s.active = false;
    forceSectionsOpen() = false;
    if (!s.filter.empty() && s.shown == 0) ImGui::TextDisabled("No properties match \"%s\"", s.filter.c_str());
    if (s.shown == 0 && !s.dragging) return;

    const ImVec2 wp = ImGui::GetWindowPos();
    const f32 left = wp.x + ImGui::GetCursorStartPos().x - ImGui::GetScrollX();
    const f32 x = left + s.splitPx - 4.0f * s.scale;   // the middle of the gap before the value column
    const f32 y0 = std::max(s.topY, wp.y), y1 = wp.y + ImGui::GetWindowHeight();
    const bool hover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemActive() &&
                       ImGui::IsMouseHoveringRect(ImVec2(x - 3.0f * s.scale, y0), ImVec2(x + 3.0f * s.scale, y1));
    if (!s.dragging && hover && ImGui::IsMouseClicked(0)) s.dragging = true;
    if (hover || s.dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (s.dragging) {
        if (ImGui::IsMouseDown(0)) {
            const f32 px = ImGui::GetIO().MousePos.x - left + 4.0f * s.scale;
            s.fraction = std::clamp(splitFractionOf(px, s.avail), 0.1f, 0.9f);
        } else {
            s.dragging = false;
            storeSplitFraction(kSplitKey, s.fraction);
        }
    }
    if (hover || s.dragging) {
        const Rgba a = editorTheme().accent;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, y0), ImVec2(x, y1),
            ImGui::ColorConvertFloat4ToU32(ImVec4(a.r, a.g, a.b, 0.8f)), 1.0f);
    }
}

inline bool filtering() { return !st().filter.empty(); }

// The default for the NEXT row's value (n floats); a row given one shows the modified mark and the reset arrow.
inline void def(const f32* v, int n = 1) {
    State& s = st();
    s.defN = std::min(n, 4);
    for (int i = 0; i < s.defN; ++i) s.def[i] = v[i];
}
inline void def(f32 v) { def(&v, 1); }
inline void def3(f32 x, f32 y, f32 z) { const f32 v[3] = {x, y, z}; def(v, 3); }

// Room kept free on the right of the NEXT row's value, for a note the caller draws after it with SameLine.
inline void reserveRight(f32 px) { st().reserveRight = px; }

// True once after a reset arrow was clicked; the value was already set to its default.
inline bool consumeReset() {
    State& s = st();
    const bool r = s.resetClicked;
    s.resetClicked = false;
    return r;
}

// One table row: the label (clipped to its column, tooltip when cut), reset arrow and modified mark, then
// `draw("##label")` for the value. `cur`/`n` are the float value the default is compared with and written
// to; tint is 2 or 3 for X/Y(/Z) vector fields. Returns draw's result, or true after a reset.
template <class Draw>
inline bool row(const char* label, f32* cur, int n, int tint, Draw&& draw) {
    State& s = st();
    const int defN = s.defN;
    f32 defv[4];
    for (int i = 0; i < defN; ++i) defv[i] = s.def[i];
    const f32 reserve = s.reserveRight;
    s.defN = 0;
    s.reserveRight = 0.0f;

    const std::string_view vis = detailsVisibleLabel(label);
    if (!s.active || vis.empty() || s.dl != ImGui::GetWindowDrawList()) return draw(label);

    if (!s.filter.empty() && !detailsLabelMatches(vis, s.filter)) {
        // A fresh zero-size item, so the caller's IsItemActivated and friends do not read the row before it.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopStyleVar();
        return false;
    }
    ++s.shown;

    const bool modified = defN > 0 && cur && detailsDiffers(cur, defv, std::min(defN, n));
    const f32 gap = 8.0f * s.scale, btn = ImGui::GetFrameHeight();
    const f32 valueX = ImGui::GetCursorStartPos().x + s.splitPx;
    const f32 labelW = std::max(0.0f, valueX - gap - (defN > 0 ? btn : 0.0f) - ImGui::GetCursorPosX());
    const f32 textW = ImGui::CalcTextSize(vis.data(), vis.data() + vis.size()).x;
    s.labelNeedNext = std::max(s.labelNeedNext, ImGui::GetCursorPosX() - ImGui::GetCursorStartPos().x + textW + gap +
                                                   (defN > 0 ? btn : 0.0f) + 2.0f * s.scale);

    ImGui::AlignTextToFramePadding();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 mn = p, mx(p.x + labelW, p.y + btn);
    if (textW <= labelW) {
        ImGui::TextUnformatted(vis.data(), vis.data() + vis.size());
    } else {   // still too long at the column's cap: end in an ellipsis, the whole label in the tooltip below
        const f32 ty = p.y + (btn - ImGui::GetFontSize()) * 0.5f;
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(p.x, ty), ImVec2(mx.x, ty + ImGui::GetFontSize()), mx.x,
                                  vis.data(), vis.data() + vis.size(), nullptr);
        ImGui::Dummy(ImVec2(labelW, ImGui::GetTextLineHeight()));
    }
    if (modified) {
        const Rgba a = editorTheme().accent;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 5.0f * s.scale, p.y), ImVec2(p.x - 3.0f * s.scale, p.y + btn),
            ImGui::ColorConvertFloat4ToU32(ImVec4(a.r, a.g, a.b, 1.0f)));
    }
    if (textW > labelW && ImGui::IsMouseHoveringRect(mn, mx))
        ImGui::SetTooltip("%.*s", static_cast<int>(vis.size()), vis.data());

    bool reset = false;
    char id[192];
    std::snprintf(id, sizeof id, "##%s", label);
    if (modified) {
        ImGui::SameLine(valueX - gap - btn);
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(ICON_UNDO "##reset", ImVec2(btn, btn))) {
            for (int i = 0; i < std::min(defN, n); ++i) cur[i] = defv[i];
            reset = true;
            s.resetClicked = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
        ImGui::PopID();
    }

    ImGui::SameLine(valueX);
    ImGui::SetNextItemWidth(std::max(40.0f * s.scale, ImGui::GetContentRegionAvail().x - reserve));
    // Several cells share a narrow column: tighter side padding inside each keeps "0.000" and "255" whole.
    const bool cells = std::max(n, tint) >= 2;   // vectors and colours
    if (cells) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * s.scale, ImGui::GetStyle().FramePadding.y));
    const bool changed = draw(id);
    if (cells) ImGui::PopStyleVar();

    if (tint >= 2) {   // red/green/blue bars at the left edge of the X/Y/Z cells (ImGui's DragScalarN cell widths)
        static const ImU32 kAxis[3] = {IM_COL32(222, 66, 66, 255), IM_COL32(107, 189, 69, 255), IM_COL32(69, 133, 235, 255)};
        const ImVec2 imn = ImGui::GetItemRectMin(), imx = ImGui::GetItemRectMax();
        const f32 sp = ImGui::GetStyle().ItemInnerSpacing.x;
        const f32 one = std::max(1.0f, std::floor((imx.x - imn.x - (tint - 1) * sp) / tint));
        const f32 last = std::max(1.0f, std::floor((imx.x - imn.x) - (one + sp) * (tint - 1)));
        const f32 bar = std::max(2.0f, 3.0f * s.scale);
        f32 cx = imn.x;
        for (int i = 0; i < tint; ++i) {
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx, imn.y), ImVec2(cx + bar, imx.y), kAxis[i],
                ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
            cx += (i == tint - 1 ? last : one) + sp;
        }
    }
    return changed || reset;
}

// ---- the widgets, same arguments as ImGui's -------------------------------------------------------------

inline bool DragFloat(const char* l, f32* v, f32 speed = 1.0f, f32 mn = 0.0f, f32 mx = 0.0f,
                      const char* fmt = "%.3f", ImGuiSliderFlags fl = 0) {
    return row(l, v, 1, 0, [&](const char* id) { return ImGui::DragFloat(id, v, speed, mn, mx, fmt, fl); });
}
inline bool DragFloat2(const char* l, f32* v, f32 speed = 1.0f, f32 mn = 0.0f, f32 mx = 0.0f,
                       const char* fmt = "%.3f", ImGuiSliderFlags fl = 0) {
    return row(l, v, 2, 2, [&](const char* id) { return ImGui::DragFloat2(id, v, speed, mn, mx, fmt, fl); });
}
inline bool DragFloat3(const char* l, f32* v, f32 speed = 1.0f, f32 mn = 0.0f, f32 mx = 0.0f,
                       const char* fmt = "%.3f", ImGuiSliderFlags fl = 0) {
    return row(l, v, 3, 3, [&](const char* id) { return ImGui::DragFloat3(id, v, speed, mn, mx, fmt, fl); });
}
inline bool DragInt(const char* l, int* v, f32 speed = 1.0f, int mn = 0, int mx = 0,
                    const char* fmt = "%d", ImGuiSliderFlags fl = 0) {
    return row(l, nullptr, 0, 0, [&](const char* id) { return ImGui::DragInt(id, v, speed, mn, mx, fmt, fl); });
}
inline bool SliderFloat(const char* l, f32* v, f32 mn, f32 mx, const char* fmt = "%.3f", ImGuiSliderFlags fl = 0) {
    return row(l, v, 1, 0, [&](const char* id) { return ImGui::SliderFloat(id, v, mn, mx, fmt, fl); });
}
inline bool SliderInt(const char* l, int* v, int mn, int mx, const char* fmt = "%d", ImGuiSliderFlags fl = 0) {
    return row(l, nullptr, 0, 0, [&](const char* id) { return ImGui::SliderInt(id, v, mn, mx, fmt, fl); });
}
inline bool InputText(const char* l, char* buf, usize size, ImGuiInputTextFlags fl = 0) {
    return row(l, nullptr, 0, 0, [&](const char* id) { return ImGui::InputText(id, buf, size, fl); });
}
inline bool InputFloat(const char* l, f32* v, f32 step = 0.0f, f32 fast = 0.0f, const char* fmt = "%.3f",
                       ImGuiInputTextFlags fl = 0) {
    return row(l, v, 1, 0, [&](const char* id) { return ImGui::InputFloat(id, v, step, fast, fmt, fl); });
}
inline bool InputInt(const char* l, int* v, int step = 1, int fast = 100, ImGuiInputTextFlags fl = 0) {
    return row(l, nullptr, 0, 0, [&](const char* id) { return ImGui::InputInt(id, v, step, fast, fl); });
}
inline bool Checkbox(const char* l, bool* v) {
    return row(l, nullptr, 0, 0, [&](const char* id) { return ImGui::Checkbox(id, v); });
}
inline bool ColorEdit3(const char* l, f32* c, ImGuiColorEditFlags fl = 0) {
    return row(l, c, 3, 0, [&](const char* id) { return ImGui::ColorEdit3(id, c, fl); });
}
inline bool ColorEdit4(const char* l, f32* c, ImGuiColorEditFlags fl = 0) {
    return row(l, c, 4, 0, [&](const char* id) { return ImGui::ColorEdit4(id, c, fl); });
}
inline bool Combo(const char* l, int* cur, const char* const items[], int count, int maxHeight = -1) {
    return row(l, nullptr, 0, 0, [&](const char* id) { return ImGui::Combo(id, cur, items, count, maxHeight); });
}
inline bool Combo(const char* l, int* cur, const char* zeroSeparated, int maxHeight = -1) {
    return row(l, nullptr, 0, 0, [&](const char* id) { return ImGui::Combo(id, cur, zeroSeparated, maxHeight); });
}

} // namespace aver::editor::dt
#endif
