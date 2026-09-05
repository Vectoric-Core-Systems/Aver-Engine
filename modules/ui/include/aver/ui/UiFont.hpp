#pragma once
// A baked bitmap font: the glyph table behind UiDrawList::addText.
//
// WHY THE GAME UI HAD NO TEXT AT ALL. modules/ui exposed exactly two primitives, addRect and
// addTexturedRect. There was no font, no glyph, no addText anywhere in ui/, ui.abi/ or render.ui/,
// and no hit test either -- so `Aver.UI`, which docs/ARCHITECTURE.md calls "the retained GAME UI"
// and names as one of the two assemblies a game authors against, could not draw a single character
// or notice a single click. Its only consumer was a hardcoded demo HUD of coloured rectangles.
//
// BAKED OFFLINE, NOT RASTERISED AT RUNTIME, and that is a deliberate choice rather than a shortcut:
//
//   - Dear ImGui's atlas is NOT reusable. ImGui is deliberately split out of the game build
//     (Aver.RHI.D3D12.ImGui, which only Sandbox links), so reaching for its font machinery would
//     put an editor UI toolkit back into a shipped game -- the exact thing that split undid.
//   - A runtime rasteriser means vendoring a TrueType library. This engine takes permissively
//     licensed dependencies only and vendors deliberately; a new one is a decision, not a detail.
//   - And it matches how every other asset here works. Meshes, materials and sounds are all cooked
//     offline and loaded as data -- .ocsnd's own header says its DAG is "rendered OFFLINE because
//     mix() forbids allocation". A font atlas is the same shape of problem.
//
// PARSE FROM TEXT, NEVER FROM A PATH, which is what keeps this in Aver.UI at all. That module is
// Core-only on purpose -- "the whole UI system is testable with no GPU and no device" -- so this
// header does no file I/O and knows nothing about the filesystem. The caller reads the bytes. It is
// the same split parseOcproject/loadOcproject already uses.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace aver::ui {

// One glyph's place in the atlas and its placement on the baseline. UVs are normalised [0,1].
struct UiGlyph {
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    // Pixel size of the glyph's bitmap, and where it sits relative to the pen: offX is to the right
    // of the pen, offY is DOWN from the baseline (so a glyph above the baseline has a negative offY).
    f32 w = 0, h = 0;
    f32 offX = 0, offY = 0;
    f32 advance = 0;   // how far the pen moves after drawing it
};

// A baked font: one atlas texture plus the glyphs in it.
struct UiFont {
    std::string name;
    f32 pixelSize  = 0;   // the size it was baked at; scaling far from it will look soft
    f32 ascent     = 0;   // above the baseline, positive
    f32 descent    = 0;   // below the baseline, negative
    f32 lineHeight = 0;   // baseline to baseline
    std::string atlasPath;   // content-relative; the CALLER resolves and uploads it
    u64 atlasTexture = 0;    // filled in by whoever uploaded the atlas; 0 until then
    std::unordered_map<u32, UiGlyph> glyphs;

    bool valid() const { return !glyphs.empty() && lineHeight > 0.0f; }
    // The glyph for a codepoint, or nullptr. No fallback box: a caller that wants one draws it.
    const UiGlyph* glyph(u32 cp) const {
        const auto it = glyphs.find(cp);
        return it == glyphs.end() ? nullptr : &it->second;
    }
};

// Parses an .ocfont document. Same OC text grammar as the other formats: `#` comments, KEY value.
//
//   OCFONT 1
//   NAME Roboto-Regular
//   SIZE 16
//   ATLAS Fonts/Roboto-Regular.png
//   METRICS <ascent> <descent> <lineHeight>
//   GLYPH <codepoint> <u0> <v0> <u1> <v1> <w> <h> <offX> <offY> <advance>
//
// False on a missing OCFONT header or a document with no glyphs, with the reason in `err`.
bool parseOcfont(std::string_view text, UiFont& out, std::string* err = nullptr);

// The width in pixels `text` would occupy, and the number of lines it spans. Needed before drawing
// to centre or right-align anything, which is why it is public rather than an internal of addText.
// Newlines advance a line; an unknown codepoint contributes nothing.
f32 uiTextWidth(const UiFont& f, std::string_view text);
u32 uiTextLines(std::string_view text);

} // namespace aver::ui
