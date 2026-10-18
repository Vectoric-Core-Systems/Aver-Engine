#pragma once
// The editor's colour-vision preview (docs/ACCESSIBILITY.md section 2.3): a View-menu combo (Off, Protanopia,
// Deuteranopia, Tritanopia, each Correction or Simulation, plus a strength) that filters the SCENE through the same
// post-chain branch the player's option uses. It lives in editor prefs only, never in the project's Settings.ini,
// and the editor chrome (drawn after the tonemap) is not filtered. Reduce-motion and text-scale previews are
// Play-time only: they are the player's settings page.
//
// THE HOOKS INTO SandboxApp.cpp are three lines: a member `editor::AccessibilityPreview accessPreview_;`, a
// `accessPreview_.load();` at start-up, and `editor::applyPreviewToPost(accessPreview_, post)` right after the
// player's own override on the blended post settings; the menu is `editor::drawColourVisionPreviewMenu(accessPreview_)`
// inside the viewport's View menu.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

namespace aver::editor {

struct AccessibilityPreview {
    int mode = 0;            // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
    bool simulate = false;   // false = correction (what a player with the filter on sees), true = simulation
    f32 strength = 1.0f;     // 0..1

    bool active() const { return mode > 0 && mode <= 3; }
    // rhi::packColourVision of the current choice (0 when off).
    f32 packed() const;
    // Reads / writes the editor prefs ("access.preview.*").
    void load();
    void save() const;
};

// Sets the preview filter on `post` unless a filter is already set there (the player's own option wins in Play).
void applyPreviewToPost(const AccessibilityPreview& p, rhi::PostSettings& post);

// The "Colour vision preview" submenu items, drawn inside an open menu. Returns true when something changed (the
// choice is saved to the prefs). A no-op without ImGui.
bool drawColourVisionPreviewMenu(AccessibilityPreview& p);

} // namespace aver::editor
