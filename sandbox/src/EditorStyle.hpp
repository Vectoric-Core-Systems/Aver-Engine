// Aver orange accents and the ImGui metrics/colour appliers (values come from editor::editorTheme()).
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

#if AVER_WITH_IMGUI
// Aver orange and dim variant.
static constexpr ImVec4 kAverOrange   (242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 1.00f);
static constexpr ImVec4 kAverOrangeDim(242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 0.55f);

// Editor dark scheme colours and metrics (split from applyDpi for DPI scaling order). The values come
// from editor::editorTheme() -- EditorTheme.hpp's defaults, overridden by editor-theme.json.
static inline ImVec4 toVec4(const editor::Rgba& c) { return ImVec4(c.r, c.g, c.b, c.a); }

static inline void applyEditorMetrics() {
    ImGuiStyle& s = ImGui::GetStyle();
    const editor::EditorTheme& t = editor::editorTheme();

    // Unscaled; applyDpi applies DPI scaling after.
    s.WindowRounding    = t.rounding;  s.ChildRounding  = t.rounding;  s.FrameRounding  = t.rounding;
    s.PopupRounding     = t.rounding;  s.GrabRounding   = t.grabRounding; s.TabRounding = t.tabRounding;
    s.ScrollbarRounding = t.rounding;
    s.WindowBorderSize  = 1;  s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.ChildBorderSize   = 1;
    s.WindowPadding     = ImVec2(t.windowPaddingX, t.windowPaddingY);
    // FramePadding.y stays at 4 -- gates constraint: docked tab bar height = FontSize + FramePadding.y*2,
    // and --probe-rel (vpX_/vpY_/vpW_/vpH_) survives a resize but not an aspect change. The x half is free.
    // It is deliberately not a theme entry.
    s.FramePadding      = ImVec2(t.framePaddingX, 4);
    s.ItemSpacing       = ImVec2(t.itemSpacingX, t.itemSpacingY);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.CellPadding       = ImVec2(6, 4);
    s.ScrollbarSize     = t.scrollbarSize;
    s.GrabMinSize       = 10;
    s.IndentSpacing     = t.indentSpacing;
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    // One close button per tab: the dock node's own X (it closes the whole node) and the window-menu
    // arrow (a collapse triangle that does nothing useful on a docked panel) are both off.
    s.DockingNodeHasCloseButton = false;
    s.WindowMenuButtonPosition  = ImGuiDir_None;
    // A wide splitter gap with a 1px line drawn in it (editor::drawDockSplitterLines); the gap is the hit area.
    s.DockingSeparatorSize = t.splitterSize;
    // A 1px rule under the tab bar rather than ImGui's default; the selected tab's overline stays.
    s.TabBarBorderSize = 1;
    s.TabBarOverlineSize = 2;
}

// Editor colour palette (safe to re-run outside widget draw). Orange is for selection, checkmarks, the
// active tab's top edge and Play; hover and press are neutral lifts of the item colour.
static inline void applyEditorColors() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    const editor::EditorTheme& th = editor::editorTheme();

    const ImVec4 sunken  = toVec4(th.sunken);      // behind content: viewports, child frames
    const ImVec4 panel   = toVec4(th.panel);       // window bodies
    const ImVec4 raised  = toVec4(th.raised);      // title bars, menu bar, inactive tabs
    const ImVec4 item    = toVec4(th.item);        // buttons, frames, headers at rest
    const ImVec4 itemHot = toVec4(editor::itemHot(th));
    const ImVec4 itemOn  = toVec4(editor::itemOn(th));
    const ImVec4 line    = toVec4(th.line);        // borders and separators
    const ImVec4 text    = toVec4(th.text);
    const ImVec4 textDim = toVec4(th.textDim);
    const ImVec4 accent  = toVec4(th.accent);
    const ImVec4 select  = toVec4(editor::selection(th));
    const ImVec4 accentDim = toVec4(editor::withAlpha(th.accent, 0.55f));
    const ImVec4 gripHot = toVec4(editor::mixToWhite(th.item, th.activeLift * 2.0f));   // scrollbar grab hovered, a step above itemOn

    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = textDim;
    c[ImGuiCol_WindowBg]              = panel;
    c[ImGuiCol_ChildBg]               = sunken;
    c[ImGuiCol_PopupBg]               = toVec4(th.popup);
    c[ImGuiCol_Border]                = line;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = item;
    c[ImGuiCol_FrameBgHovered]        = itemHot;
    c[ImGuiCol_FrameBgActive]         = itemOn;

    c[ImGuiCol_TitleBg]               = raised;
    c[ImGuiCol_TitleBgActive]         = raised;
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(sunken.x, sunken.y, sunken.z, 0.85f);
    c[ImGuiCol_MenuBarBg]             = raised;

    c[ImGuiCol_ScrollbarBg]           = ImVec4(sunken.x, sunken.y, sunken.z, 0.60f);
    c[ImGuiCol_ScrollbarGrab]         = itemHot;
    c[ImGuiCol_ScrollbarGrabHovered]  = gripHot;
    c[ImGuiCol_ScrollbarGrabActive]   = textDim;

    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_SliderGrab]            = textDim;
    c[ImGuiCol_SliderGrabActive]      = text;

    c[ImGuiCol_Button]                = item;
    c[ImGuiCol_ButtonHovered]         = itemHot;
    c[ImGuiCol_ButtonActive]          = itemOn;

    // Header is the SELECTED row for Selectable and TreeNode: orange at selectionAlpha. Hover is a
    // translucent white lift, so it reads as neutral over any row. CollapsingHeader also paints Header
    // at rest, which is why every section header goes through editor::sectionHeader (EditorChrome.hpp),
    // which keeps them neutral.
    c[ImGuiCol_Header]                = select;
    c[ImGuiCol_HeaderHovered]         = ImVec4(1, 1, 1, th.hoverLift + 0.01f);
    c[ImGuiCol_HeaderActive]          = toVec4(editor::withAlpha(th.accent, th.selectionAlpha + 0.15f));

    c[ImGuiCol_Separator]             = line;
    c[ImGuiCol_SeparatorHovered]      = textDim;
    c[ImGuiCol_SeparatorActive]       = text;

    c[ImGuiCol_ResizeGrip]            = ImVec4(itemHot.x, itemHot.y, itemHot.z, 0.60f);
    c[ImGuiCol_ResizeGripHovered]     = textDim;
    c[ImGuiCol_ResizeGripActive]      = text;

    // Orange top edge on a panel-coloured body reads as "this one" without shouting (a fully orange
    // tab would).
    c[ImGuiCol_Tab]                   = raised;
    c[ImGuiCol_TabHovered]            = itemHot;
    c[ImGuiCol_TabActive]             = panel;
    c[ImGuiCol_TabSelectedOverline]   = accent;
    c[ImGuiCol_TabUnfocused]          = sunken;
    c[ImGuiCol_TabUnfocusedActive]    = raised;
    c[ImGuiCol_TabDimmedSelectedOverline] = accentDim;

    c[ImGuiCol_DockingPreview]        = select;
    c[ImGuiCol_DockingEmptyBg]        = sunken;

    c[ImGuiCol_PlotLines]             = textDim;
    c[ImGuiCol_PlotLinesHovered]      = text;
    c[ImGuiCol_PlotHistogram]         = accent;      // progress bars: the one non-selection use, kept so progress reads at a glance
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.000f, 0.478f, 0.208f, 1.00f);

    c[ImGuiCol_TableHeaderBg]         = raised;
    c[ImGuiCol_TableBorderStrong]     = line;
    c[ImGuiCol_TableBorderLight]      = ImVec4(line.x + 0.03f, line.y + 0.03f, line.z + 0.03f, 1.0f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.022f);   // banding, barely there on purpose

    c[ImGuiCol_TextLink]              = text;
    c[ImGuiCol_InputTextCursor]       = text;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget]        = accent;
    c[ImGuiCol_NavHighlight]          = accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(sunken.x, sunken.y, sunken.z, 0.60f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.65f);
}
#endif
