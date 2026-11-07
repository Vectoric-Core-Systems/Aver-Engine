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
#include <imgui_internal.h>   // ImHashStr, FindWindowSettingsByID, the dock-node tree

#include "aver/core/Types.hpp"
#include "EditorIcons.hpp"
#include "EditorTheme.hpp"

#include <cmath>
#include <cstring>
#include <string>

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


// ---- icons ------------------------------------------------------------------------------------
// The one place icon and label meet: a single space, so a row of icon-led labels lines up. Compile-time
// strings concatenate (ICON_SAVE " Save"); this is the same thing for a label built at run time.
inline std::string iconLabel(const char* icon, const char* label) {
    std::string s(icon);
    s += ' ';
    s += label;
    return s;
}

// True when `label` already starts with a Private Use Area icon (UTF-8 for U+E000..U+FFFF is EE/EF xx xx).
inline bool startsWithIcon(const char* label) {
    const unsigned char c = static_cast<unsigned char>(label[0]);
    return c == 0xEE || c == 0xEF;
}

// The icon a section header gets from its name, or null when it has none. A name that already begins
// with an icon is left alone by sectionHeader.
inline const char* sectionIcon(const char* label) {
    struct Row { const char* name; const char* icon; };
    static const Row kRows[] = {
        {"Transform", ICON_TRANSFORM}, {"Material", ICON_MATERIAL}, {"Mesh", ICON_ACTOR_MESH},
        {"Light", ICON_LIGHT}, {"Decal", ICON_DECAL}, {"Particle Emitter", ICON_PARTICLES},
        {"Camera", ICON_CAMERA}, {"Editable", ICON_EDIT}, {"Prefab", ICON_PACKAGE},
        {"Level Instance", ICON_LAYERS}, {"Hair", ICON_HAIR}, {"Geometry", ICON_ACTOR_MESH},
        {"Material Slots", ICON_MATERIAL}, {"Bounds", ICON_SELECT_ALL}, {"LODs", ICON_LAYERS},
        {"Content Browser", ICON_FOLDER}, {"Output Log", ICON_LOG}, {"Display", ICON_VISIBILITY},
        {"Physics (Jolt)", ICON_PHYSICS}, {"Keybinds", ICON_CONSOLE}, {"Preview", ICON_VISIBILITY},
    };
    for (const Row& r : kRows) if (std::strcmp(label, r.name) == 0) return r.icon;
    return nullptr;
}

// ---- panel windows ----------------------------------------------------------------------------
// Dock tab titles. The text after ### is the window's identity (ImGui hashes from there), so the
// icon can change without losing a window's size, dock slot or visibility; migrateWindowSettings
// moves a saved layout over from the plain names these replaced.
#define AVER_WIN_OUTLINER ICON_OUTLINER " World Outliner###World Outliner"
#define AVER_WIN_DETAILS  ICON_DETAILS " Details###Details"
#define AVER_WIN_MODE     ICON_TUNE " Mode###Mode"
#define AVER_WIN_LEVEL    ICON_TERRAIN " Level###Level"

// Re-keys the saved settings of the four panel windows from their old plain names (what an
// editor-layout.ini written before the tab icons holds) to the ### names above. Run once, after
// ImGui has read the ini (the first frame) and before any of them is Begin()'d.
inline void migrateWindowSettings() {
    struct Pair { const char* oldName; const char* newName; };
    static const Pair kPairs[] = {
        {"World Outliner", AVER_WIN_OUTLINER}, {"Details", AVER_WIN_DETAILS},
        {"Mode", AVER_WIN_MODE}, {"Level", AVER_WIN_LEVEL},
    };
    for (const Pair& p : kPairs) {
        const ImGuiID newId = ImHashStr(p.newName);
        ImGuiWindowSettings* old = ImGui::FindWindowSettingsByID(ImHashStr(p.oldName));
        if (old && !ImGui::FindWindowSettingsByID(newId)) old->ID = newId;
    }
}

// Draws the dock splitters: the gap between two docked panels is wide (that is the hit area), filled
// with the panel colour, with a 1px rule down its middle. Call after DockSpace with the root node's id.
// Drawn on the background list, which sits behind every window, so only the gaps show it.
inline void drawDockSplitterLines(ImGuiID rootId) {
    const ImGuiDockNode* root = ImGui::DockBuilderGetNode(rootId);
    if (!root) return;
    const EditorTheme& th = editorTheme();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(th.panel.r, th.panel.g, th.panel.b, 1.0f));
    const ImU32 rule = ImGui::ColorConvertFloat4ToU32(ImVec4(th.line.r, th.line.g, th.line.b, 1.0f));
    struct Walk {
        static void run(const ImGuiDockNode* n, ImDrawList* dl, ImU32 fill, ImU32 rule) {
            if (!n || !n->IsSplitNode()) return;
            const ImGuiDockNode* a = n->ChildNodes[0];
            const ImGuiDockNode* b = n->ChildNodes[1];
            if (a && b) {
                if (n->SplitAxis == ImGuiAxis_X) {
                    const float x0 = a->Pos.x + a->Size.x, x1 = b->Pos.x;
                    if (x1 > x0) {
                        dl->AddRectFilled(ImVec2(x0, n->Pos.y), ImVec2(x1, n->Pos.y + n->Size.y), fill);
                        const float mid = std::floor((x0 + x1) * 0.5f);
                        dl->AddRectFilled(ImVec2(mid, n->Pos.y), ImVec2(mid + 1.0f, n->Pos.y + n->Size.y), rule);
                    }
                } else {
                    const float y0 = a->Pos.y + a->Size.y, y1 = b->Pos.y;
                    if (y1 > y0) {
                        dl->AddRectFilled(ImVec2(n->Pos.x, y0), ImVec2(n->Pos.x + n->Size.x, y1), fill);
                        const float mid = std::floor((y0 + y1) * 0.5f);
                        dl->AddRectFilled(ImVec2(n->Pos.x, mid), ImVec2(n->Pos.x + n->Size.x, mid + 1.0f), rule);
                    }
                }
            }
            run(a, dl, fill, rule);
            run(b, dl, fill, rule);
        }
    };
    Walk::run(root, dl, fill, rule);
}

// ---- toolbar ----------------------------------------------------------------------------------
// A capsule: a run of toolbar buttons on one rounded plate with thin dividers between them. The plate
// is drawn behind the buttons (draw-list channels), so its width is whatever its buttons made it.
//
//     ImGui::SameLine();              // the caller's, before the capsule
//     { ToolbarCapsule cap(dpi);
//       ImGui::Button(...);
//       cap.divider();                // a thin vertical rule, then the next button
//       ImGui::Button(...); }         // the destructor closes the plate
//
// Inside, ItemSpacing.x is 0 and Button is transparent (hover and press are white lifts), so a plain
// ImGui::SameLine() between two buttons butts them together and divider() is the only gap. Every
// button is a frame tall: the same height as a dock tab, because FramePadding.y is pinned.
inline f32 capsulePad(f32 dpi) { return 3.0f * dpi; }
inline f32 capsuleDivider(f32 dpi) { return 7.0f * dpi; }

class ToolbarCapsule {
public:
    explicit ToolbarCapsule(f32 dpi) : dpi_(dpi) {
        dl_ = ImGui::GetWindowDrawList();
        pos_ = ImGui::GetCursorScreenPos();
        h_ = ImGui::GetFrameHeight();
        splitter_.Split(dl_, 2);
        splitter_.SetCurrentChannel(dl_, 1);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.18f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(capsulePad(dpi_), h_));
        ImGui::SameLine();
    }
    ~ToolbarCapsule() {
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(capsulePad(dpi_), h_));
        const ImVec2 end(ImGui::GetItemRectMax().x, pos_.y + h_);
        splitter_.SetCurrentChannel(dl_, 0);
        const EditorTheme& th = editorTheme();
        const ImU32 plate = ImGui::ColorConvertFloat4ToU32(ImVec4(th.item.r, th.item.g, th.item.b, 1.0f));
        const ImU32 edge = ImGui::ColorConvertFloat4ToU32(
            ImVec4(th.overlayBorder.r, th.overlayBorder.g, th.overlayBorder.b, th.overlayBorder.a));
        const f32 r = ImGui::GetStyle().FrameRounding + 2.0f * dpi_;
        dl_->AddRectFilled(pos_, end, plate, r);
        dl_->AddRect(pos_, end, edge, r, 0, 1.0f);
        splitter_.Merge(dl_);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }
    ToolbarCapsule(const ToolbarCapsule&) = delete;
    ToolbarCapsule& operator=(const ToolbarCapsule&) = delete;

    // The rule between two buttons; the next item lands after it.
    void divider() {
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(capsuleDivider(dpi_), h_));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const f32 x = std::floor((a.x + b.x) * 0.5f);
        dl_->AddLine(ImVec2(x, a.y + h_ * 0.22f), ImVec2(x, b.y - h_ * 0.22f),
                     ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.14f)), 1.0f);
        ImGui::SameLine();
    }

private:
    f32 dpi_;
    f32 h_ = 0.0f;
    ImVec2 pos_{};
    ImDrawList* dl_ = nullptr;
    ImDrawListSplitter splitter_;
};

// The primary action's pill: Aver orange, dark text (white on this orange is under 4.5:1), fully round.
// `width` 0 sizes it to the label.
inline bool primaryPill(const char* label, f32 width = 0.0f) {
    const EditorTheme& th = editorTheme();
    const auto vec = [](const Rgba& c) { return ImVec4(c.r, c.g, c.b, c.a); };
    ImGui::PushStyleColor(ImGuiCol_Button, vec(th.accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(mixToWhite(th.accent, 0.14f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec(mixToWhite(th.accent, 0.28f)));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(th.sunken));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
    const bool hit = ImGui::Button(label, ImVec2(width, 0.0f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return hit;
}

// The text a section header shows: the label led by its icon when it has one. The identity stays the
// plain label (###), so open/closed state is the same with or without the icon.
inline std::string sectionHeaderText(const char* label) {
    const char* icon = startsWithIcon(label) ? nullptr : sectionIcon(label);
    if (!icon) return label;
    return iconLabel(icon, label) + "###" + label;
}

// A section title: the header font, and a NEUTRAL header bar -- ImGuiCol_Header is the theme's
// selection colour (orange) because Selectable and TreeNode rows use it for "selected", and
// CollapsingHeader paints it at rest.
inline bool sectionHeader(const char* label, ImGuiTreeNodeFlags flags = 0) {
    const std::string shown = sectionHeaderText(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Header, st.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, st.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, st.Colors[ImGuiCol_FrameBgActive]);
    FontScope f(editorFonts().header, editorFonts().headerPx);
    const bool open = ImGui::CollapsingHeader(shown.c_str(), flags);
    ImGui::PopStyleColor(3);
    return open;
}

inline bool sectionHeader(const char* label, bool* pVisible, ImGuiTreeNodeFlags flags = 0) {
    const std::string shown = sectionHeaderText(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Header, st.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, st.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, st.Colors[ImGuiCol_FrameBgActive]);
    FontScope f(editorFonts().header, editorFonts().headerPx);
    const bool open = ImGui::CollapsingHeader(shown.c_str(), pVisible, flags);
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
