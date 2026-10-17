// UAX #14 subset, CJK break-anywhere and kinsoku. See UiLineBreak.hpp for what is and is not implemented.
#include "aver/ui/UiLineBreak.hpp"

namespace aver::ui {
namespace {

// LB1: CJ is NS in the strict tailoring, so a small kana cannot start a line. Unknown classes are AL.
UiLb classOf(u32 cp) {
    const UiLb c = uiLineBreakClassOf(cp);
    return c == UiLb::CJ ? UiLb::NS : c;
}

bool eastAsian(u32 cp) { return cp >= 0x2E80; }

// The break between a previous character (effective class p, the class `base` of the last non-space
// before any spaces, the one before that p2) and the next (class c). prevWasZwj is LB8a.
UiBreak decide(UiLb p, UiLb base, UiLb p2, UiLb c, bool prevWasZwj, u32 prevCp, u32 cp) {
    // LB4, LB5: hard breaks
    if (p == UiLb::BK) return UiBreak::Mandatory;
    if (p == UiLb::CR) return c == UiLb::LF ? UiBreak::No : UiBreak::Mandatory;
    if (p == UiLb::LF || p == UiLb::NL) return UiBreak::Mandatory;
    // LB6
    if (c == UiLb::BK || c == UiLb::CR || c == UiLb::LF || c == UiLb::NL) return UiBreak::No;
    // LB7
    if (c == UiLb::SP || c == UiLb::ZW) return UiBreak::No;
    // LB8: ZW SP* breaks
    if (base == UiLb::ZW) return UiBreak::Allowed;
    // LB8a
    if (prevWasZwj) return UiBreak::No;
    // LB9, LB10: a combining mark attaches to its base; after a space or ZW it is a letter
    if (c == UiLb::CM || c == UiLb::ZWJ) {
        if (p != UiLb::SP && p != UiLb::ZW) return UiBreak::No;
        c = UiLb::AL;
    }
    // LB11
    if (c == UiLb::WJ || p == UiLb::WJ) return UiBreak::No;
    // LB12, LB12a
    if (p == UiLb::GL) return UiBreak::No;
    if (c == UiLb::GL && p != UiLb::SP && p != UiLb::BA && p != UiLb::HY) return UiBreak::No;
    // LB13
    if (c == UiLb::CL || c == UiLb::CP || c == UiLb::EX || c == UiLb::IS || c == UiLb::SY) return UiBreak::No;
    // LB14: OP SP* x
    if (base == UiLb::OP) return UiBreak::No;
    // LB15: QU SP* x OP
    if (base == UiLb::QU && c == UiLb::OP) return UiBreak::No;
    // LB16: (CL | CP) SP* x NS
    if ((base == UiLb::CL || base == UiLb::CP) && c == UiLb::NS) return UiBreak::No;
    // LB17: B2 SP* x B2
    if (base == UiLb::B2 && c == UiLb::B2) return UiBreak::No;
    // LB18: SP breaks
    if (p == UiLb::SP) return UiBreak::Allowed;
    // LB19: quotation marks bind both sides
    if (c == UiLb::QU || p == UiLb::QU) return UiBreak::No;
    // LB21
    if (c == UiLb::BA || c == UiLb::HY || c == UiLb::NS || p == UiLb::BB) return UiBreak::No;
    // LB21a, LB21b
    if (p2 == UiLb::HL && (p == UiLb::HY || p == UiLb::BA)) return UiBreak::No;
    if (p == UiLb::SY && c == UiLb::HL) return UiBreak::No;
    // LB22
    if (c == UiLb::IN) return UiBreak::No;
    // LB23, LB23a
    if ((p == UiLb::AL || p == UiLb::HL) && c == UiLb::NU) return UiBreak::No;
    if (p == UiLb::NU && (c == UiLb::AL || c == UiLb::HL)) return UiBreak::No;
    if (p == UiLb::PR && c == UiLb::ID) return UiBreak::No;
    if (p == UiLb::ID && c == UiLb::PO) return UiBreak::No;
    // LB24
    if ((p == UiLb::PR || p == UiLb::PO) && (c == UiLb::AL || c == UiLb::HL)) return UiBreak::No;
    if ((p == UiLb::AL || p == UiLb::HL) && (c == UiLb::PR || c == UiLb::PO)) return UiBreak::No;
    // LB25: numbers with prefixes and postfixes
    if ((p == UiLb::CL || p == UiLb::CP || p == UiLb::NU) && (c == UiLb::PO || c == UiLb::PR)) return UiBreak::No;
    if ((p == UiLb::PO || p == UiLb::PR) && (c == UiLb::OP || c == UiLb::NU)) return UiBreak::No;
    if ((p == UiLb::HY || p == UiLb::IS || p == UiLb::NU || p == UiLb::SY) && c == UiLb::NU) return UiBreak::No;
    // LB28, LB29
    if ((p == UiLb::AL || p == UiLb::HL) && (c == UiLb::AL || c == UiLb::HL)) return UiBreak::No;
    if (p == UiLb::IS && (c == UiLb::AL || c == UiLb::HL)) return UiBreak::No;
    // LB30 (the non-East-Asian brackets)
    if ((p == UiLb::AL || p == UiLb::HL || p == UiLb::NU) && c == UiLb::OP && !eastAsian(cp)) return UiBreak::No;
    if (p == UiLb::CP && !eastAsian(prevCp) && (c == UiLb::AL || c == UiLb::HL || c == UiLb::NU)) return UiBreak::No;
    // LB31
    return UiBreak::Allowed;
}

} // namespace

void uiLineBreaks(const u32* cps, usize n, std::vector<UiBreak>& out) {
    out.assign(n + 1, UiBreak::No);
    if (n == 0) { out[0] = UiBreak::Mandatory; return; }

    UiLb p = classOf(cps[0]);
    if (p == UiLb::CM || p == UiLb::ZWJ) p = UiLb::AL;   // LB10 at the start of text
    UiLb p2 = UiLb::AL;
    UiLb base = p == UiLb::SP ? UiLb::AL : p;
    bool prevWasZwj = uiLineBreakClassOf(cps[0]) == UiLb::ZWJ;

    for (usize i = 1; i < n; ++i) {
        const UiLb raw = classOf(cps[i]);
        out[i] = decide(p, base, p2, raw, prevWasZwj, cps[i - 1], cps[i]);

        prevWasZwj = raw == UiLb::ZWJ;
        if (raw == UiLb::CM || raw == UiLb::ZWJ) {
            if (p == UiLb::SP || p == UiLb::ZW || p == UiLb::BK || p == UiLb::CR || p == UiLb::LF || p == UiLb::NL) {   // LB10: a stray mark is a letter
                p2 = p;
                p = UiLb::AL;
                base = UiLb::AL;
            }
            continue;   // otherwise it is absorbed into its base
        }
        p2 = p;
        p = raw;
        if (raw != UiLb::SP) base = raw;
    }
    out[n] = UiBreak::Mandatory;
}

bool uiCannotStartLine(u32 cp) {
    switch (cp) {
        case 0x29: case 0x5D: case 0x7D:                                   // ) ] }
        case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E:                // ideographic comma and stop, fullwidth , .
        case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F:                // fullwidth : ; ! ?
        case 0x300D: case 0x300F: case 0xFF09: case 0x3011: case 0x3015:   // closing corner brackets and parentheses
        case 0x3009: case 0x300B: case 0x201D: case 0x2019:                // closing angle brackets, curly quotes
        case 0x30FC:                                                       // long vowel mark
        case 0x3005: case 0x303B:                                          // iteration marks
        case 0x2026: case 0x2025:                                          // ellipses
        case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:  // small hiragana
        case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E:
        case 0x3095: case 0x3096:
        case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:   // small katakana
        case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE:
        case 0x30F5: case 0x30F6:
            return true;
        default:
            return cp >= 0x31F0 && cp <= 0x31FF;                           // katakana phonetic extensions
    }
}

bool uiCannotEndLine(u32 cp) {
    switch (cp) {
        case 0x28: case 0x5B: case 0x7B:                                   // ( [ {
        case 0x300C: case 0x300E: case 0xFF08: case 0x3010: case 0x3014:   // opening corner brackets and parentheses
        case 0x3008: case 0x300A: case 0x201C: case 0x2018:                // opening angle brackets, curly quotes
        case 0xFF3B: case 0xFF5B:
            return true;
        default:
            return false;
    }
}

} // namespace aver::ui
