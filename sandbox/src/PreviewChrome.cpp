// The asset editors' preview chrome: toolbar, stats and axes drawn over an ActorPreview image.
#include "PreviewChrome.hpp"

namespace aver::editor {

std::string formatCount(u64 n) {
    const std::string digits = std::to_string(n);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (usize i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

} // namespace aver::editor

#if AVER_WITH_IMGUI
#include "imgui_internal.h"   // LayoutSnapshot

#include <algorithm>
#include <cmath>

namespace aver::editor {
namespace {

using render::preview::PreviewViewMode;

constexpr f32 kDegToRad = 3.14159265358979f / 180.0f;
constexpr int kViewModeCount = 4;

const char* viewModeName(PreviewViewMode m) {
    switch (m) {
        case PreviewViewMode::Unlit:     return "Unlit";
        case PreviewViewMode::Wireframe: return "Wireframe";
        case PreviewViewMode::Normals:   return "Normals";
        default:                         return "Lit";
    }
}

// SandboxApp::dropButton's look (it is private to SandboxApp): a button with a drop-down triangle.
f32 dropButtonWidth(const char* label, f32 dpi) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f * dpi;
}
bool dropButton(const char* label, f32 dpi) {
    const f32 extra = 16.0f * dpi;
    const bool clicked = ImGui::Button(label, ImVec2(dropButtonWidth(label, dpi), 0.0f));
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const f32 cx = mx.x - extra * 0.5f - 2.0f * dpi, cy = (mn.y + mx.y) * 0.5f, s = 3.0f * dpi;
    ImGui::GetWindowDrawList()->AddTriangleFilled(
        ImVec2(cx - s, cy - s * 0.55f), ImVec2(cx + s, cy - s * 0.55f), ImVec2(cx, cy + s * 0.8f),
        ImGui::GetColorU32(ImGuiCol_Text));
    return clicked;
}

// The overlay's inset from the image edge, as the level viewport's bars sit from its edge.
f32 overlayInset(f32 dpi) { return 8.0f * dpi; }
// The toolbar strip's height: one frame plus the window padding around it.
f32 toolbarHeight() { return ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f; }

// The parent window's layout and last item, put back after the toolbar child so the overlay adds no
// content size (no stray scrollbar), no line for SameLine to follow, and no item to IsItemHovered.
struct LayoutSnapshot {
    ImVec2 cursorPos, cursorPosPrevLine, cursorMaxPos, idealMaxPos, currLineSize, prevLineSize;
    f32 currLineBase, prevLineBase;
    bool isSameLine, isSetPos;
    ImGuiLastItemData lastItem;

    explicit LayoutSnapshot(const ImGuiWindow& w)
        : cursorPos(w.DC.CursorPos), cursorPosPrevLine(w.DC.CursorPosPrevLine), cursorMaxPos(w.DC.CursorMaxPos),
          idealMaxPos(w.DC.IdealMaxPos), currLineSize(w.DC.CurrLineSize), prevLineSize(w.DC.PrevLineSize),
          currLineBase(w.DC.CurrLineTextBaseOffset), prevLineBase(w.DC.PrevLineTextBaseOffset),
          isSameLine(w.DC.IsSameLine), isSetPos(w.DC.IsSetPos), lastItem(GImGui->LastItemData) {}

    void restore(ImGuiWindow& w) const {
        w.DC.CursorPos = cursorPos;
        w.DC.CursorPosPrevLine = cursorPosPrevLine;
        w.DC.CursorMaxPos = cursorMaxPos;
        w.DC.IdealMaxPos = idealMaxPos;
        w.DC.CurrLineSize = currLineSize;
        w.DC.PrevLineSize = prevLineSize;
        w.DC.CurrLineTextBaseOffset = currLineBase;
        w.DC.PrevLineTextBaseOffset = prevLineBase;
        w.DC.IsSameLine = isSameLine;
        w.DC.IsSetPos = isSetPos;
        GImGui->LastItemData = lastItem;
    }
};

// Text with a dark drop shadow, readable over any backdrop.
void shadowText(ImDrawList* dl, ImVec2 at, f32 dpi, ImU32 col, const char* text) {
    const f32 o = std::max(1.0f, std::round(dpi));
    dl->AddText(ImVec2(at.x + o, at.y + o), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(at, col, text);
}

} // namespace

bool drawPreviewToolbar(const char* id, ImVec2 imageMin, ImVec2 imageMax, f32 dpi,
                        render::preview::PreviewViewMode& mode, render::preview::PreviewShowFlags& show,
                        const PreviewShowItem* extraShow, int extraShowCount) {
    if (imageMax.x - imageMin.x < 80.0f * dpi || imageMax.y - imageMin.y < 60.0f * dpi) return false;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (!win || win->SkipItems) return false;

    const ImGuiStyle& st = ImGui::GetStyle();
    const char* modeLabel = viewModeName(mode);
    const ImVec2 size(dropButtonWidth(modeLabel, dpi) + st.ItemSpacing.x + dropButtonWidth("Show", dpi) +
                          st.WindowPadding.x * 2.0f,
                      toolbarHeight());

    const LayoutSnapshot saved(*win);
    ImGui::SetCursorScreenPos(ImVec2(imageMin.x + overlayInset(dpi), imageMin.y + overlayInset(dpi)));
    ImVec4 bg = st.Colors[ImGuiCol_WindowBg];
    bg.w = 0.62f;   // the level viewport overlay's background alpha
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, st.WindowRounding);
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                            ImGuiWindowFlags_NoNav);
    // Popped before any popup opens, so the dropdowns keep the editor's own style.
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    bool changed = false;
    if (open) {
        if (dropButton(modeLabel, dpi)) ImGui::OpenPopup("viewMode");
        if (ImGui::BeginPopup("viewMode")) {
            for (int i = 0; i < kViewModeCount; ++i) {
                const PreviewViewMode m = static_cast<PreviewViewMode>(i);
                if (ImGui::Selectable(viewModeName(m), mode == m) && mode != m) { mode = m; changed = true; }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (dropButton("Show", dpi)) ImGui::OpenPopup("showFlags");
        if (ImGui::BeginPopup("showFlags")) {
            if (ImGui::Checkbox("Grid", &show.grid)) changed = true;
            if (ImGui::Checkbox("Bounds", &show.bounds)) changed = true;
            if (extraShow && extraShowCount > 0) {
                ImGui::Separator();
                for (int i = 0; i < extraShowCount; ++i)
                    if (extraShow[i].label && extraShow[i].value && ImGui::Checkbox(extraShow[i].label, extraShow[i].value))
                        changed = true;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    saved.restore(*win);
    return changed;
}

void drawPreviewStats(ImVec2 imageMin, f32 dpi, const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 x = imageMin.x + overlayInset(dpi) + 2.0f * dpi;
    f32 y = imageMin.y + overlayInset(dpi) + toolbarHeight() + 6.0f * dpi;
    const f32 lineH = ImGui::GetTextLineHeight() + 1.0f * dpi;
    for (const std::string& s : lines) {
        shadowText(dl, ImVec2(x, y), dpi, IM_COL32(255, 255, 255, 235), s.c_str());
        y += lineH;
    }
}

void drawPreviewAxes(ImVec2 imageMin, ImVec2 imageMax, f32 dpi, const render::preview::PreviewCamera& cam) {
    if (imageMax.x - imageMin.x < 60.0f * dpi || imageMax.y - imageMin.y < 60.0f * dpi) return;

    // The preview camera's basis, as ActorPreview's lookAt builds it: a world axis's screen direction
    // is its dot with right and up, and its dot with forward orders the three back to front.
    const f32 cy = std::cos(cam.yawDeg * kDegToRad), sy = std::sin(cam.yawDeg * kDegToRad);
    const f32 cp = std::cos(cam.pitchDeg * kDegToRad), sp = std::sin(cam.pitchDeg * kDegToRad);
    const f32 right[3] = {-sy, cy, 0.0f};
    const f32 up[3] = {cy * sp, sy * sp, cp};
    const f32 fwd[3] = {cp * cy, cp * sy, -sp};

    struct Axis { f32 x, y, depth; ImU32 col; const char* name; };
    const ImU32 cols[3] = {IM_COL32(232, 72, 60, 255), IM_COL32(112, 204, 64, 255), IM_COL32(72, 132, 244, 255)};
    const char* names[3] = {"X", "Y", "Z"};
    Axis axes[3];
    for (int i = 0; i < 3; ++i) axes[i] = {right[i], up[i], fwd[i], cols[i], names[i]};
    std::sort(axes, axes + 3, [](const Axis& a, const Axis& b) { return a.depth > b.depth; });

    const f32 len = 24.0f * dpi, labelGap = 8.0f * dpi;
    const f32 inset = overlayInset(dpi) + labelGap + 4.0f * dpi;
    const ImVec2 o(imageMin.x + inset + len, imageMax.y - inset - len);
    const f32 th = std::max(1.5f, 2.0f * dpi);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(imageMin, imageMax, true);
    for (const Axis& a : axes) {
        dl->AddLine(o, ImVec2(o.x + a.x * len, o.y - a.y * len), a.col, th);
        const ImVec2 ts = ImGui::CalcTextSize(a.name);
        const f32 r = len + labelGap;
        shadowText(dl, ImVec2(o.x + a.x * r - ts.x * 0.5f, o.y - a.y * r - ts.y * 0.5f), dpi, a.col, a.name);
    }
    dl->AddCircleFilled(o, 2.0f * dpi, IM_COL32(220, 221, 226, 255));
    dl->PopClipRect();
}

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
