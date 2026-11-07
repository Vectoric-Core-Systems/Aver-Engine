#pragma once
// ImGui-side chrome helpers driven by the editor theme (EditorTheme.hpp): the type scale, section
// headers and the viewport overlay box style.
//
// The editor's type scale: header (Roboto Medium 18), body (Roboto 16, the ImGui default) and small
// (Roboto 13.5), all times the DPI scale, loaded in SandboxApp::applyDpi. Pointers are null until
// then, or when Roboto is missing and ImGui's bitmap fallback is in use -- every helper here treats
// null as "leave the current font alone".
#if AVER_WITH_IMGUI
#include <imgui.h>

#include "aver/core/Types.hpp"
#include "EditorTheme.hpp"

namespace aver::editor {

struct EditorFonts {
    ImFont* header = nullptr;   // panel and section titles
    f32 headerPx = 0.0f;        // PushFont's size: ImGui 1.92 fonts are scalable, so a pushed font keeps the
    f32 secondaryPx = 0.0f;     // CURRENT size unless told otherwise, and the load size alone changes nothing
    ImFont* secondary = nullptr;   // status bar, secondary hints, viewport readouts
};

inline EditorFonts& editorFonts() { static EditorFonts f; return f; }

// Push/pop a type-scale font for the current scope. A null font pushes nothing.
class FontScope {
public:
    FontScope(ImFont* f, f32 px) : on_(f != nullptr) { if (on_) ImGui::PushFont(f, px); }
    ~FontScope() { if (on_) ImGui::PopFont(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
private:
    bool on_;
};

inline FontScope headerFont() { return FontScope(editorFonts().header, editorFonts().headerPx); }
inline FontScope smallFont()  { return FontScope(editorFonts().secondary, editorFonts().secondaryPx); }

// A section title: the header font, and a NEUTRAL header bar -- ImGuiCol_Header is the theme's
// selection colour (orange) because Selectable and TreeNode rows use it for "selected", and
// CollapsingHeader paints it at rest.
inline bool sectionHeader(const char* label, ImGuiTreeNodeFlags flags = 0) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Header, st.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, st.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, st.Colors[ImGuiCol_FrameBgActive]);
    FontScope f(editorFonts().header, editorFonts().headerPx);
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(3);
    return open;
}

inline bool sectionHeader(const char* label, bool* pVisible, ImGuiTreeNodeFlags flags = 0) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Header, st.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, st.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, st.Colors[ImGuiCol_FrameBgActive]);
    FontScope f(editorFonts().header, editorFonts().headerPx);
    const bool open = ImGui::CollapsingHeader(label, pVisible, flags);
    ImGui::PopStyleColor(3);
    return open;
}

// Overlay box style for a child window drawn over a viewport or preview image: `raised` at the theme's
// overlayAlpha with a 1px low-alpha neutral border. Push before BeginChild, pop right after it (the
// child is decorated inside the call), and add ImGuiChildFlags_Borders to the child's flags.
inline void pushOverlayChildStyle() {
    const EditorTheme& th = editorTheme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(th.raised.r, th.raised.g, th.raised.b, th.overlayAlpha));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(th.overlayBorder.r, th.overlayBorder.g, th.overlayBorder.b, th.overlayBorder.a));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
}
inline void popOverlayChildStyle() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

} // namespace aver::editor
#endif
