#pragma once
// The one object both measure and draw read, so a widget is never measured one way and drawn another.
// A layout is positioned glyphs per line (relative to the line origin and the baseline), the lines'
// metrics and one caret stop per cluster edge. Built by a UiTextMetrics (UiText.hpp): the real engine
// (UiTextEngine) or, for the legacy bitmap / estimated path, a simple builder with `legacy` set.
#include "aver/core/Types.hpp"
#include "aver/ui/UiTypes.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

// Face ids in UiGlyphRef::face. Real faces are small indices into the font set.
constexpr u16 kUiFaceBitmap = 0xFFFE;   // the lent baked UiFont; `glyph` is the code point
constexpr u16 kUiFaceNone = 0xFFFF;     // no face: a missing-glyph box (flag set) or a blank

enum UiGlyphFlag : u8 {
    kGlyphBox = 1,        // draw the hollow missing-glyph rectangle (no atlas, no face)
    kGlyphUnjoined = 2,   // an Arabic letter drawn from its base form because the face lacks the form
    kGlyphMark = 4,       // a combining mark placed on its base
    kGlyphSkew = 8,       // synthetic italic: shear the quad
};

struct UiGlyphRef {
    u32 glyph = 0;               // face glyph id (code point for the bitmap face)
    u16 face = kUiFaceNone;
    u16 rasterPx = 0;
    f32 x = 0, y = 0;            // pen x from the line origin; y from the baseline (down is positive)
    f32 quad = 1;                // quad scale: only != 1 when the requested size was clamped
    u16 color = 0;               // index into UiTextLayout::palette; 0 = the caller's colour
    u8 flags = 0;
};

// A caret position: the code point index it sits before, and its x from the line origin.
struct UiCaretStop {
    u32 cp = 0;
    f32 x = 0;
};

struct UiTextLine {
    u32 glyphBegin = 0, glyphEnd = 0;
    u32 srcBegin = 0, srcEnd = 0;       // code point indices into the shown text
    u32 stopBegin = 0, stopEnd = 0;     // caret stops of this line
    f32 width = 0;
    f32 ascent = 0, descent = 0;        // descent is negative
    f32 top = 0;                        // from the block top
    f32 height = 0;                     // line advance
    f32 baseline = 0;                   // from `top` to the baseline
    bool rtl = false;                   // the paragraph direction is right to left
};

struct UiTextLayout {
    std::string source;                          // the shown text (rich tags stripped)
    std::vector<u32> cpToByte;                   // code point index -> byte offset in `source`; one extra entry
    std::vector<UiGlyphRef> glyphs;
    std::vector<UiTextLine> lines;
    std::vector<UiCaretStop> stops;
    std::vector<u32> palette;                    // rich colours (straight RGBA); palette[0] is unused
    std::vector<std::string> legacyLines;        // legacy layouts keep their own line strings
    f32 width = 0, height = 0, px = 0;           // widest line, total height, the requested size
    bool incomplete = false;                     // glyphs missing (budget or atlas overflow): not cached
    bool legacy = false;                         // built from the three old metrics virtuals

    usize cpCount() const { return cpToByte.empty() ? 0 : cpToByte.size() - 1; }
    std::string_view lineText(usize i) const;
    // Which line a code point index falls on (clamped).
    usize lineOfIndex(usize cp) const;
    // x of the caret before code point `cp`, from the line origin; a code point inside a cluster snaps
    // to the cluster start and the line end gives the trailing edge of the last logical cluster.
    f32 caretX(usize cp) const;
    // The code point index whose caret stop is nearest (x, y); y from the block top.
    usize indexAt(f32 x, f32 y) const;
};

// What the caller asks for. `family` is only read during layoutText.
struct UiTextStyle {
    std::string_view family;
    f32 px = 16.0f;
    f32 scale = 1.0f;                // UI scale: rich <size=N> is in design pixels
    bool bold = false, italic = false;
    UiTextDir dir = UiTextDir::Auto;
    f32 lineSpacing = 1.0f;
    f32 letterSpacing = 0.0f;
};

struct UiTextBox {
    f32 maxWidth = 0;
    bool wrap = false, ellipsis = false, rich = false, richColors = true;
    bool carets = false;             // legacy builders only compute caret stops when asked (TextInput)
    u32 maxLines = 0;
};

} // namespace aver::ui
