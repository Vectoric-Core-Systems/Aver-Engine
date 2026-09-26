#pragma once
// The UE-style chrome the asset editors draw over their 3D preview image: the view-mode and Show
// toolbar, the stats lines and the axes gizmo. Styled after SandboxApp::buildViewportOverlay.
#include "aver/core/Types.hpp"

#include <string>

namespace aver::editor {

// Thousands separators for stats: 84602 -> "84,602".
std::string formatCount(u64 n);

} // namespace aver::editor

#if AVER_WITH_IMGUI
#include "imgui.h"
#include "aver/render/preview/ActorPreview.hpp"

#include <vector>

namespace aver::editor {

// One editor-specific toggle in the Show dropdown, below Grid and Bounds.
struct PreviewShowItem {
    const char* label;
    bool* value;
};

// Draws the translucent UE-style toolbar over a preview image: view-mode dropdown + "Show" dropdown
// with Grid/Bounds plus any editor-specific toggles passed in extraShow (label + bool*). Returns true
// when anything changed.
//
// Call it after ImGui::Image. It leaves the window's layout and last item as the image left them, so
// SameLine and IsItemHovered after it still refer to the image -- and the image reads as not hovered
// while the mouse is over the toolbar, so orbit/zoom there do not fire.
bool drawPreviewToolbar(const char* id, ImVec2 imageMin, ImVec2 imageMax, f32 dpi,
                        render::preview::PreviewViewMode& mode, render::preview::PreviewShowFlags& show,
                        const PreviewShowItem* extraShow = nullptr, int extraShowCount = 0);

// White-with-shadow stats lines at the image's top-left, below the toolbar strip.
void drawPreviewStats(ImVec2 imageMin, f32 dpi, const std::vector<std::string>& lines);

// XYZ axes gizmo (red X, green Y, blue Z -- engine is Z-up) in the image's bottom-left corner,
// oriented by the preview camera.
void drawPreviewAxes(ImVec2 imageMin, ImVec2 imageMax, f32 dpi, const render::preview::PreviewCamera& cam);

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
