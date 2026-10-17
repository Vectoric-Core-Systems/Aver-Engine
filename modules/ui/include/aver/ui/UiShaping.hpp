#pragma once
// Arabic shaping in-house, no GSUB: joining types, isolated / initial / medial / final forms mapped to
// Unicode Presentation Forms (FE70..FEFC and the Persian / Urdu letters in FB50..FDxx), lam-alef
// ligatures, ZWJ / ZWNJ / tatweel. Marks stay as marks and are placed from glyph boxes by the engine.
//
// Font dependency: the shaped code point is looked up through the face's cmap. Arial, Segoe UI and
// Tahoma carry FE70..FEFC; a face that only ships GSUB-driven forms shows the base letters unjoined
// (the engine falls back to UiShapedCp::base and flags the glyph).
#include "aver/ui/UiUnicode.hpp"

#include <vector>

namespace aver::ui {

// ArabicShaping.txt joining types: D dual, R right, L left, U non-joining, C join causing, T transparent.
enum class UiJoin : u8 { U, D, R, L, C, T };
UiJoin uiJoiningType(u32 cp);

// Letters, marks, ZWJ / ZWNJ and tatweel that take part in shaping. Presentation forms do not (so shaping
// already-shaped text is a fixed point).
bool uiIsArabicShapable(u32 cp);

enum UiShapeFlag : u8 {
    kShapeJoined = 1,      // `cp` is a presentation form chosen for the letter
    kShapeLigature = 2,    // lam-alef: `base` is the lam, `base2` the alef
    kShapeNoForm = 4,      // the letter has no presentation form for this position; `cp` is the base letter
};

struct UiShapedCp {
    u32 cp = 0;            // what to look up in the font
    u32 base = 0;          // the original code point (the lam of a ligature)
    u32 base2 = 0;         // the alef of a ligature, else 0
    u32 src = 0;           // index of the first source code point
    u8 srcCount = 1;       // source code points consumed
    u8 flags = 0;
};

// Shapes a whole paragraph in logical order: maximal Arabic ranges are shaped, everything else passes
// through (srcCount 1). The sum of srcCount over the output equals n.
void uiShapeText(const u32* cps, usize n, std::vector<UiShapedCp>& out);

// The presentation-form code point of an Arabic letter in one position (0 isolated, 1 final, 2 initial,
// 3 medial), or 0 when the letter has none.
u32 uiArabicForm(u32 cp, int form);

// The lam-alef ligature for an alef (U+0622, 0623, 0625, 0627): FEF5 / FEF7 / FEF9 / FEFB isolated, the
// next code point final (when the lam joins from the left). 0 for any other alef.
u32 uiArabicLamAlef(u32 alef, bool final);

} // namespace aver::ui
