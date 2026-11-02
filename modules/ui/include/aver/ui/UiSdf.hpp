#pragma once
// Signed-distance glyph bitmaps (parity E14): an A8 coverage glyph turned into an A8 distance field, so one raster size
// scales crisply and an outline or softness is a threshold in the pixel shader. Core-only maths, no GPU.
#include "aver/core/Types.hpp"
#include "aver/ui/UiFontFace.hpp"

namespace aver::ui {

// The most pixels of padding a distance field may add on each side, and the largest bitmap edge it accepts.
constexpr u32 kUiSdfMaxSpread = 16;
constexpr u32 kUiSdfMaxEdge = 320;

// Converts `in` (A8 coverage, the 1 px gutter included) to a distance field with `spread` pixels of padding on every side.
// A texel holds 128 on the glyph's edge (coverage 0.5), rises to 255 at `spread` pixels inside and falls to 0 at `spread`
// pixels outside. offX / offY move by the padding so the glyph stays where it was. A blank glyph stays blank. False (out
// untouched) for a bitmap larger than kUiSdfMaxEdge, a pixel array shorter than w * h, or spread 0.
bool uiBitmapToSdf(const UiGlyphBitmap& in, u32 spread, UiGlyphBitmap& out);

// The shader's reading of a distance texel back as a coverage: smoothstep over [0.5 - softness, 0.5 + softness] of v / 255.
// The CPU twin of the pixel shader's math, for tests and for any code that wants the same edge on the CPU.
f32 uiSdfCoverage(u8 texel, f32 softness);

} // namespace aver::ui
