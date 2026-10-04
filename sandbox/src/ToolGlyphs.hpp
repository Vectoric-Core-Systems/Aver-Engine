#pragma once
// The four transform-tool icons and the button chrome around them, shared by the level viewport and
// the actor editor's.
#if AVER_WITH_IMGUI
#include "imgui.h"

#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::editor {

// The tool identities, in toolbar order and in the order the 1-4 keys select them.
enum ToolKind { ToolSelect = 0, ToolMove = 1, ToolRotate = 2, ToolScale = 3 };

// Draws a compact vector icon into a cell. kind: 0 Select, 1 Move, 2 Rotate, 3 Scale, and -- only
// drawn where AVER_MODULE_LANDSCAPE's sculpt tools reuse this same button -- 4 Sculpt Raise, 5
// Sculpt Lower, 6 Sculpt Smooth, 7 Sculpt Flatten, 8 Sculpt Ramp, 9 Sculpt Noise, all a brush ring
// around a mode glyph.
inline void drawToolGlyph(ImDrawList* dl, ImVec2 p, f32 sz, int kind, ImU32 fg, f32 dpi) {
    auto P = [&](f32 fx, f32 fy){ return ImVec2(p.x+fx*sz, p.y+fy*sz); };
    const f32 th = std::fmax(1.6f, 2.0f*dpi);
    if (kind == 0) { // pointer/cursor
        dl->AddTriangleFilled(P(0.30f,0.20f), P(0.30f,0.70f), P(0.45f,0.56f), fg);
        dl->AddTriangleFilled(P(0.30f,0.20f), P(0.45f,0.56f), P(0.63f,0.49f), fg);
        dl->AddLine(P(0.47f,0.55f), P(0.61f,0.80f), fg, th*1.6f);
    } else if (kind == 1) { // 4-way move
        dl->AddLine(P(0.5f,0.15f), P(0.5f,0.85f), fg, th);
        dl->AddLine(P(0.15f,0.5f), P(0.85f,0.5f), fg, th);
        const f32 a = 0.08f*sz;
        dl->AddTriangleFilled(P(0.5f,0.11f), ImVec2(P(0.5f,0.25f).x-a,P(0.5f,0.25f).y), ImVec2(P(0.5f,0.25f).x+a,P(0.5f,0.25f).y), fg);
        dl->AddTriangleFilled(P(0.5f,0.89f), ImVec2(P(0.5f,0.75f).x-a,P(0.5f,0.75f).y), ImVec2(P(0.5f,0.75f).x+a,P(0.5f,0.75f).y), fg);
        dl->AddTriangleFilled(P(0.11f,0.5f), ImVec2(P(0.25f,0.5f).x,P(0.25f,0.5f).y-a), ImVec2(P(0.25f,0.5f).x,P(0.25f,0.5f).y+a), fg);
        dl->AddTriangleFilled(P(0.89f,0.5f), ImVec2(P(0.75f,0.5f).x,P(0.75f,0.5f).y-a), ImVec2(P(0.75f,0.5f).x,P(0.75f,0.5f).y+a), fg);
    } else if (kind == 2) { // rotate arc + arrowhead
        const ImVec2 c = P(0.5f,0.5f); const f32 r = 0.30f*sz;
        dl->PathArcTo(c, r, -2.30f, 1.15f, 24); dl->PathStroke(fg, 0, th);
        const f32 ea=1.15f; const ImVec2 end(c.x+std::cos(ea)*r, c.y+std::sin(ea)*r); const f32 a=0.07f*sz;
        dl->AddTriangleFilled(ImVec2(end.x-a,end.y-a*0.4f), ImVec2(end.x+a*0.6f,end.y-a), ImVec2(end.x+a*0.2f,end.y+a), fg);
    } else if (kind == 3) { // scale: diagonal + boxes
        dl->AddLine(P(0.26f,0.74f), P(0.74f,0.26f), fg, th);
        const ImVec2 tl=P(0.74f,0.26f); const f32 b=0.10f*sz;
        dl->AddRectFilled(ImVec2(tl.x-b,tl.y-b), ImVec2(tl.x+b,tl.y+b), fg, 1.5f);
        const ImVec2 br=P(0.26f,0.74f); const f32 b2=0.07f*sz;
        dl->AddRect(ImVec2(br.x-b2,br.y-b2), ImVec2(br.x+b2,br.y+b2), fg, 1.0f, 0, th);
    } else if (kind == 4) { // sculpt raise: brush ring, chevron up (a mound rising)
        dl->AddCircle(P(0.5f,0.55f), 0.32f*sz, fg, 20, th);
        dl->PathLineTo(P(0.30f,0.66f)); dl->PathLineTo(P(0.5f,0.40f)); dl->PathLineTo(P(0.70f,0.66f));
        dl->PathStroke(fg, 0, th);
    } else if (kind == 5) { // sculpt lower: brush ring, chevron down (a pit sinking)
        dl->AddCircle(P(0.5f,0.45f), 0.32f*sz, fg, 20, th);
        dl->PathLineTo(P(0.30f,0.34f)); dl->PathLineTo(P(0.5f,0.60f)); dl->PathLineTo(P(0.70f,0.34f));
        dl->PathStroke(fg, 0, th);
    } else if (kind == 6) { // sculpt smooth: brush ring, one wave (levelling a ripple)
        dl->AddCircle(P(0.5f,0.5f), 0.32f*sz, fg, 20, th);
        dl->PathLineTo(P(0.26f,0.56f));
        dl->PathBezierQuadraticCurveTo(P(0.38f,0.34f), P(0.5f,0.56f));
        dl->PathBezierQuadraticCurveTo(P(0.62f,0.78f), P(0.74f,0.56f));
        dl->PathStroke(fg, 0, th);
    } else if (kind == 7) { // sculpt flatten: brush ring, flat bar
        dl->AddCircle(P(0.5f,0.5f), 0.32f*sz, fg, 20, th);
        dl->AddLine(P(0.28f,0.5f), P(0.72f,0.5f), fg, th*1.4f);
    } else if (kind == 8) { // sculpt ramp: brush ring, a rising diagonal (a slope, not a mound)
        dl->AddCircle(P(0.5f,0.5f), 0.32f*sz, fg, 20, th);
        dl->AddLine(P(0.30f,0.66f), P(0.70f,0.34f), fg, th*1.4f);
    } else { // kind 9, sculpt noise (also the fallback for any stray kind): brush ring, scattered dots
        dl->AddCircle(P(0.5f,0.5f), 0.32f*sz, fg, 20, th);
        const f32 r = 0.045f * sz;
        dl->AddCircleFilled(P(0.40f,0.40f), r, fg, 10);
        dl->AddCircleFilled(P(0.62f,0.45f), r, fg, 10);
        dl->AddCircleFilled(P(0.47f,0.62f), r, fg, 10);
        dl->AddCircleFilled(P(0.62f,0.66f), r, fg, 10);
    }
}

// Draws one tool button with its icon and active/hover fill. Returns true when clicked.
inline bool toolButton(const char* id, int kind, bool active, f32 icon, f32 dpi) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(icon, icon));
    const bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
    const ImU32 bg = active ? IM_COL32(232,110,35,235)
                            : (hov ? IM_COL32(74,76,82,255) : IM_COL32(48,49,54,220));
    dl->AddRectFilled(p, ImVec2(p.x+icon, p.y+icon), bg, 4.0f);
    drawToolGlyph(dl, p, icon, kind, IM_COL32(236,237,240,255), dpi);
    return clk;
}

// The human name of a tool, for a tooltip or a status line.
inline const char* toolName(int kind) {
    switch (kind) {
        case ToolSelect: return "Select";
        case ToolMove:   return "Move";
        case ToolRotate: return "Rotate";
        default:         return "Scale";
    }
}

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
