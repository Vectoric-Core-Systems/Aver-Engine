#pragma once
// Text outline and shadow as draw passes (docs/plans/fabric/G5_PARITY.md U2). A glyph run is drawn several times: the shadow (and its
// outline) shifted by the shadow offset, the outline as a ring of copies round the origin, then the fill. The geometry is CPU arithmetic
// so a test can decide it without a GPU; the ABI (aver_ui_text_block_fx) walks the passes through the ordinary text path, so the glyph
// atlas, kerning and clipping are the existing ones. Nothing here runs unless a caller asks for an outline or a shadow.
#include "aver/core/Types.hpp"

#include <vector>

namespace aver::ui {

struct UiTextFxStyle {
    f32 outlineSize = 0;          // pixels; 0 = no outline
    u32 outlineRgba = 0xFF000000; // straight 0xAABBGGRR
    u32 shadowRgba = 0;           // alpha 0 = no shadow
    f32 shadowOffX = 0, shadowOffY = 0;
    f32 shadowOutlineSize = 0;    // the outline round the shadow copy
};

// One copy of the text: its offset from the origin and its colour (0 = take the fill colour).
struct UiTextFxPass {
    f32 dx = 0, dy = 0;
    u32 rgba = 0;
    bool fill = false;            // the last pass, in the caller's colour
};

// Sample points of an outline ring of the given radius, evenly spaced from the +X axis. A radius under 0.5 gives none; the count grows with
// the circumference (8 up to 32), and a radius of 3 or more adds an inner ring at half the radius so thick outlines have no holes.
void uiOutlineOffsets(f32 radius, std::vector<UiTextFxPass>& out);

// Every pass in draw order: shadow outline, shadow, outline, fill. `fillRgba` is the text colour; a style with nothing set gives the
// single fill pass at (0, 0).
std::vector<UiTextFxPass> uiTextFxPasses(const UiTextFxStyle& style, u32 fillRgba);

// The bounds a styled run grows to on each side: outline on all sides, the shadow shifted and grown. left, top, right, bottom >= 0.
struct UiTextFxGrow { f32 left = 0, top = 0, right = 0, bottom = 0; };
UiTextFxGrow uiTextFxGrow(const UiTextFxStyle& style);

} // namespace aver::ui
