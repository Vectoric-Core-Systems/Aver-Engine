#pragma once
// A subset of the Unicode Bidirectional Algorithm (UAX #9), pure and table driven.
//
// Implemented: P2-P3 (paragraph level, skipping isolates), X1-X8 with embeddings, overrides AND isolates
// all treated as plain embeddings (depth cap 125, overflow ignored), W1-W7, N1-N2, I1-I2, L1 (trailing
// whitespace and separators), L2 (reordering), L4 (mirroring, by uiMirror). Not implemented: N0 (bracket
// pair resolution) and true isolating run sequences. LRM, RLM and ALM are strong and zero width.
#include "aver/ui/UiTypes.hpp"
#include "aver/ui/UiUnicode.hpp"

#include <vector>

namespace aver::ui {

struct UiBidiResult {
    u8 paragraphLevel = 0;
    std::vector<u8> levels;               // resolved embedding level per code point (0..126)
    std::vector<UiBidiClass> classes;     // the ORIGINAL classes, for L1
};

// Resolves one paragraph. `dir` Auto takes the first strong letter (none -> left to right).
void uiBidiResolve(const u32* cps, usize n, UiTextDir dir, UiBidiResult& out);

// The paragraph level alone (P2, P3).
u8 uiBidiParagraphLevel(const u32* cps, usize n, UiTextDir dir);

// L1 for one line: separators, and whitespace / formatting characters before them or at the line end,
// return to the paragraph level. `classes` are the original classes of the line's code points.
void uiBidiResetTrailing(const UiBidiClass* classes, u8* levels, usize n, u8 paragraphLevel);

// L2: the visual order of one line. out[v] is the logical index shown at visual position v.
void uiBidiReorderLine(const u8* levels, usize n, std::vector<u32>& out);

} // namespace aver::ui
