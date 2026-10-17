#pragma once
// One TrueType / OpenType face, rasterised by Dear ImGui's vendored stb_truetype (the one allowed
// rasteriser). Core-only: the bytes arrive as a blob, this module does no file I/O.
//
// SAFETY. stb_truetype states "NO SECURITY GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES". The
// policy (docs/GAME_UI_TEXT.md) is that faces load only from the project's Content, the engine
// directory or the OS fonts directory -- never saves, network or mod archives. uiValidateSfnt runs
// first as defence in depth; it checks the table directory and the headers stb trusts, it cannot make
// glyph-outline parsing safe.
#include "aver/core/Types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::ui {

// Font bytes plus whatever keeps them alive (a memory-mapped file, a buffer).
struct UiFontBlob {
    const u8* data = nullptr;
    usize size = 0;
    std::shared_ptr<void> owner;
};

// Checks an sfnt / TrueType Collection buffer: magic, table directory inside the buffer and not
// overlapping (at most 64 tables), the required tables present (cmap head hhea hmtx maxp plus glyf+loca
// or CFF), table sizes consistent with head / maxp / hhea, cmap subtable offsets inside cmap.
// `index` selects a collection member (0 for a plain font). False with the reason in `err`.
bool uiValidateSfnt(const u8* data, usize size, i32 index, std::string* err = nullptr);

// A rasterised glyph: A8 coverage with a 1 px transparent gutter on every side. offX / offY place the
// bitmap's top-left from the pen (x) and the baseline (y, down), gutter included. w == h == 0 is a blank
// glyph (a space).
struct UiGlyphBitmap {
    u16 w = 0, h = 0;
    i16 offX = 0, offY = 0;
    std::vector<u8> pixels;
};

enum class UiKernSource : u8 { None, Kern, Gpos };

class UiFontFace {
public:
    UiFontFace();
    ~UiFontFace();
    UiFontFace(UiFontFace&&) noexcept;
    UiFontFace& operator=(UiFontFace&&) noexcept;
    UiFontFace(const UiFontFace&) = delete;
    UiFontFace& operator=(const UiFontFace&) = delete;

    // Validates and opens. `index` selects a TTC member.
    bool open(const UiFontBlob& blob, i32 index, std::string* err = nullptr);
    bool valid() const;

    u32 glyphCount() const;
    u32 unitsPerEm() const;
    // The glyph for a code point; 0 is the face's missing glyph.
    u32 glyphFor(u32 cp) const;
    // True for a glyph with no outline (a space).
    bool isBlank(u32 glyph) const;
    // Horizontal advance in pixels at an em size of `px` (stb's "em to pixels" scale).
    f32 advance(u32 glyph, f32 px) const;
    // Kerning in pixels between two glyphs: GPOS PairPos format 1 and 2 X-advance if the font has GPOS,
    // else the `kern` table. Memoised per face.
    f32 kern(u32 g1, u32 g2, f32 px) const;
    UiKernSource kernSource() const;
    // Unscaled glyph bounding box in font units, y up. False for a blank glyph.
    bool glyphBox(u32 glyph, i32& x0, i32& y0, i32& x1, i32& y1) const;
    // Vertical metrics in pixels at em size `px`: hhea ascent / descent (OS/2 typo when hhea has none)
    // and line gap. descent is negative.
    f32 ascent(f32 px) const;
    f32 descent(f32 px) const;
    f32 lineGap(f32 px) const;
    // Rasterises one glyph at an integer em size, origin on the pixel grid, no subpixel shift.
    bool rasterize(u32 glyph, u32 rasterPx, UiGlyphBitmap& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aver::ui
