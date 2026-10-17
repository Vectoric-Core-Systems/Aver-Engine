// UAX #9 subset. See UiBidi.hpp for what is and is not implemented.
#include "aver/ui/UiBidi.hpp"

#include <algorithm>
#include <numeric>

namespace aver::ui {
namespace {

using C = UiBidiClass;

constexpr int kMaxDepth = 125;

bool isIsolateInit(C c) { return c == C::LRI || c == C::RLI || c == C::FSI; }

// First strong direction in [from, to), skipping isolated runs. 0 = L, 1 = R, -1 = none.
int firstStrong(const u32* cps, usize from, usize to) {
    int depth = 0;
    for (usize i = from; i < to; ++i) {
        const C c = uiBidiClassOf(cps[i]);
        if (isIsolateInit(c)) { ++depth; continue; }
        if (c == C::PDI) { if (depth > 0) --depth; continue; }
        if (depth > 0) continue;
        if (c == C::L) return 0;
        if (c == C::R || c == C::AL) return 1;
    }
    return -1;
}

// Resolves W1-W7, N1-N2 and I1-I2 for one level run. `t` holds the run's classes, `idx` the code point each
// came from; resolved levels are written to `levels`.
void resolveRun(std::vector<C>& t, const std::vector<usize>& idx, u8 level, C sos, C eos, std::vector<u8>& levels) {
    const usize m = t.size();

    // W1: a nonspacing mark takes the class of what precedes it.
    for (usize k = 0; k < m; ++k)
        if (t[k] == C::NSM) t[k] = k == 0 ? sos : t[k - 1];

    // W2: European numbers after an Arabic letter become Arabic numbers.
    for (usize k = 0; k < m; ++k) {
        if (t[k] != C::EN) continue;
        C strong = sos;
        for (usize j = k; j-- > 0;)
            if (t[j] == C::L || t[j] == C::R || t[j] == C::AL) { strong = t[j]; break; }
        if (strong == C::AL) t[k] = C::AN;
    }
    // W3
    for (usize k = 0; k < m; ++k) if (t[k] == C::AL) t[k] = C::R;
    // W4: a single separator between two numbers of one type joins them.
    for (usize k = 1; k + 1 < m; ++k) {
        if (t[k] == C::ES && t[k - 1] == C::EN && t[k + 1] == C::EN) t[k] = C::EN;
        else if (t[k] == C::CS && t[k - 1] == C::EN && t[k + 1] == C::EN) t[k] = C::EN;
        else if (t[k] == C::CS && t[k - 1] == C::AN && t[k + 1] == C::AN) t[k] = C::AN;
    }
    // W5: terminators next to a European number become European numbers.
    for (usize k = 0; k < m;) {
        if (t[k] != C::ET) { ++k; continue; }
        usize e = k;
        while (e < m && t[e] == C::ET) ++e;
        const bool before = k > 0 && t[k - 1] == C::EN;
        const bool after = e < m && t[e] == C::EN;
        if (before || after) for (usize j = k; j < e; ++j) t[j] = C::EN;
        k = e;
    }
    // W6
    for (usize k = 0; k < m; ++k) if (t[k] == C::ES || t[k] == C::ET || t[k] == C::CS) t[k] = C::ON;
    // W7: European numbers after a left-to-right letter are letters.
    for (usize k = 0; k < m; ++k) {
        if (t[k] != C::EN) continue;
        C strong = sos;
        for (usize j = k; j-- > 0;)
            if (t[j] == C::L || t[j] == C::R) { strong = t[j]; break; }
        if (strong == C::L) t[k] = C::L;
    }

    // N1, N2: neutrals take the direction they sit in, else the embedding direction.
    const C embedding = (level & 1) ? C::R : C::L;
    const auto isNeutral = [](C c) { return c == C::B || c == C::S || c == C::WS || c == C::ON; };
    const auto dirOf = [](C c) { return (c == C::L) ? C::L : C::R; };   // EN and AN count as R
    for (usize k = 0; k < m;) {
        if (!isNeutral(t[k])) { ++k; continue; }
        usize e = k;
        while (e < m && isNeutral(t[e])) ++e;
        const C lead = k == 0 ? sos : dirOf(t[k - 1]);
        const C trail = e == m ? eos : dirOf(t[e]);
        const C d = lead == trail ? lead : embedding;
        for (usize j = k; j < e; ++j) t[j] = d;
        k = e;
    }

    // I1, I2
    for (usize k = 0; k < m; ++k) {
        u8 lv = level;
        if ((level & 1) == 0) {
            if (t[k] == C::R) lv = static_cast<u8>(level + 1);
            else if (t[k] == C::AN || t[k] == C::EN) lv = static_cast<u8>(level + 2);
        } else if (t[k] == C::L || t[k] == C::EN || t[k] == C::AN) {
            lv = static_cast<u8>(level + 1);
        }
        levels[idx[k]] = lv;
    }
}

} // namespace

u8 uiBidiParagraphLevel(const u32* cps, usize n, UiTextDir dir) {
    if (dir == UiTextDir::LTR) return 0;
    if (dir == UiTextDir::RTL) return 1;
    return firstStrong(cps, 0, n) == 1 ? 1 : 0;
}

void uiBidiResolve(const u32* cps, usize n, UiTextDir dir, UiBidiResult& out) {
    out.paragraphLevel = uiBidiParagraphLevel(cps, n, dir);
    out.levels.assign(n, out.paragraphLevel);
    out.classes.resize(n);
    if (n == 0) return;

    std::vector<C> cls(n);
    for (usize i = 0; i < n; ++i) out.classes[i] = cls[i] = uiBidiClassOf(cps[i]);

    // X1-X8. Embeddings, overrides and isolates are all pushed on one stack.
    struct Entry { u8 level; u8 override_; };   // override_: 0 none, 1 force L, 2 force R
    std::vector<Entry> stack;
    stack.push_back({out.paragraphLevel, 0});
    int overflow = 0;

    for (usize i = 0; i < n; ++i) {
        const C c = cls[i];
        switch (c) {
            case C::RLE: case C::LRE: case C::RLO: case C::LRO: case C::RLI: case C::LRI: case C::FSI: {
                out.levels[i] = stack.back().level;
                bool rtl = c == C::RLE || c == C::RLO || c == C::RLI;
                if (c == C::FSI) {
                    usize end = i + 1;
                    int depth = 0;
                    for (; end < n; ++end) {
                        const C e = uiBidiClassOf(cps[end]);
                        if (isIsolateInit(e)) ++depth;
                        else if (e == C::PDI) { if (depth == 0) break; --depth; }
                    }
                    rtl = firstStrong(cps, i + 1, end) == 1;
                }
                const int cur = stack.back().level;
                const int next = rtl ? ((cur + 1) | 1) : ((cur + 2) & ~1);
                if (next <= kMaxDepth && overflow == 0) {
                    stack.push_back({static_cast<u8>(next), static_cast<u8>(c == C::RLO ? 2 : (c == C::LRO ? 1 : 0))});
                } else {
                    ++overflow;
                }
                cls[i] = C::BN;
                break;
            }
            case C::PDF: case C::PDI:
                if (overflow > 0) --overflow;
                else if (stack.size() > 1) stack.pop_back();
                out.levels[i] = stack.back().level;
                cls[i] = C::BN;
                break;
            case C::B:
                out.levels[i] = out.paragraphLevel;
                break;
            case C::BN:
                out.levels[i] = stack.back().level;
                break;
            default:
                out.levels[i] = stack.back().level;
                if (stack.back().override_ == 1) cls[i] = C::L;
                else if (stack.back().override_ == 2) cls[i] = C::R;
                break;
        }
    }

    // Level runs over the characters that remain after X9 (BN is skipped).
    std::vector<usize> keep;
    keep.reserve(n);
    for (usize i = 0; i < n; ++i) if (cls[i] != C::BN) keep.push_back(i);

    std::vector<u8> explicitLevels = out.levels;
    for (usize r0 = 0; r0 < keep.size();) {
        const u8 level = explicitLevels[keep[r0]];
        usize r1 = r0;
        while (r1 < keep.size() && explicitLevels[keep[r1]] == level) ++r1;
        const u8 prevLevel = r0 > 0 ? explicitLevels[keep[r0 - 1]] : out.paragraphLevel;
        const u8 nextLevel = r1 < keep.size() ? explicitLevels[keep[r1]] : out.paragraphLevel;
        const C sos = (std::max(level, prevLevel) & 1) ? C::R : C::L;
        const C eos = (std::max(level, nextLevel) & 1) ? C::R : C::L;

        std::vector<C> t;
        std::vector<usize> idx;
        t.reserve(r1 - r0);
        idx.reserve(r1 - r0);
        for (usize k = r0; k < r1; ++k) { t.push_back(cls[keep[k]]); idx.push_back(keep[k]); }
        resolveRun(t, idx, level, sos, eos, out.levels);
        r0 = r1;
    }

    // Removed characters sit at the level of what precedes them.
    for (usize i = 0; i < n; ++i)
        if (cls[i] == C::BN) out.levels[i] = i > 0 ? out.levels[i - 1] : out.paragraphLevel;
}

void uiBidiResetTrailing(const UiBidiClass* classes, u8* levels, usize n, u8 paragraphLevel) {
    bool resetting = true;
    for (usize i = n; i-- > 0;) {
        const C c = classes[i];
        if (c == C::B || c == C::S) {
            levels[i] = paragraphLevel;
            resetting = true;
        } else if (c == C::WS || c == C::BN || isIsolateInit(c) || c == C::PDI || c == C::PDF || c == C::LRE ||
                   c == C::RLE || c == C::LRO || c == C::RLO) {
            if (resetting) levels[i] = paragraphLevel;
        } else {
            resetting = false;
        }
    }
}

void uiBidiReorderLine(const u8* levels, usize n, std::vector<u32>& out) {
    out.resize(n);
    std::iota(out.begin(), out.end(), 0u);
    if (n == 0) return;
    u8 highest = 0, lowestOdd = 255;
    for (usize i = 0; i < n; ++i) {
        highest = std::max(highest, levels[i]);
        if ((levels[i] & 1) && levels[i] < lowestOdd) lowestOdd = levels[i];
    }
    if (lowestOdd == 255) return;   // nothing right to left
    for (int lv = highest; lv >= lowestOdd; --lv) {
        for (usize p = 0; p < n;) {
            if (levels[out[p]] < lv) { ++p; continue; }
            usize e = p;
            while (e < n && levels[out[e]] >= lv) ++e;
            std::reverse(out.begin() + static_cast<std::ptrdiff_t>(p), out.begin() + static_cast<std::ptrdiff_t>(e));
            p = e;
        }
    }
}

} // namespace aver::ui
