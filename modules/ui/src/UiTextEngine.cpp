// The text engine: layout (decode, rich text, bidi, shaping, clusters, faces, kerning, marks, line breaks,
// wrapping, ellipsis, caret stops), the layout cache, atlas residency and glyph quad emission.
// See UiTextEngine.hpp and docs/GAME_UI_TEXT.md for the contract.
#include "aver/ui/UiTextEngine.hpp"

#include "aver/core/Log.hpp"
#include "aver/ui/UiBidi.hpp"
#include "aver/ui/UiLineBreak.hpp"
#include "aver/ui/UiRichText.hpp"
#include "aver/ui/UiShaping.hpp"
#include "aver/ui/UiUnicode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::ui {
namespace {

constexpr usize kMaxCodePoints = 16384;
constexpr usize kMaxLines = 512;
constexpr usize kMaxGlyphs = 32768;
constexpr u32 kMinRaster = 4, kMaxRaster = 160;
constexpr f32 kFitEps = 0.01f;
constexpr f32 kItalicSkew = 0.2f;

u32 clampRaster(f32 px) {
    const long r = std::lround(px);
    return static_cast<u32>(std::clamp<long>(r, kMinRaster, kMaxRaster));
}

// The integer raster size for an em size and the quad scale that maps it back (1 unless clamped).
void rasterFor(f32 px, u32& rasterPx, f32& quad) {
    rasterPx = clampRaster(px);
    quad = (px > static_cast<f32>(kMaxRaster) || px < static_cast<f32>(kMinRaster)) ? px / static_cast<f32>(rasterPx) : 1.0f;
}

// ---- one code point's style ---------------------------------------------------------------------

struct CpStyle {
    bool bold = false, italic = false;
    i32 family = 0;
    f32 px = 16.0f;
    u16 color = 0;
};

// ---- layout scratch -----------------------------------------------------------------------------

struct TGlyph {
    u32 glyph = 0;
    u16 face = kUiFaceNone;
    u16 rasterPx = 0;
    f32 quad = 1;
    f32 x = 0, y = 0;     // from the cluster origin / the baseline
    u16 color = 0;
    u8 flags = 0;
};

struct Cluster {
    u32 srcBegin = 0, srcEnd = 0;       // global code point indices (empty for an ellipsis)
    u32 gBegin = 0, gEnd = 0;           // range in Para::glyphs
    f32 width = 0;
    f32 ascent = 0, descent = 0, gap = 0;
    u8 level = 0;
    bool space = false;                 // a hanging space
    UiBreak breakBefore = UiBreak::No;
    u32 cp0 = 0;                        // the original base code point, for kinsoku
    UiBidiClass cls = UiBidiClass::L;   // the base's original class, for L1
};

struct Para {
    u32 pb = 0, pe = 0;
    u8 level = 0;
    std::vector<Cluster> clusters;
    std::vector<TGlyph> glyphs;
    f32 primAscent = 0, primDescent = 0, primGap = 0;
    f32 gapScale = 1.0f;
};

struct LineSel {
    u32 para = 0;
    std::vector<u32> idx;               // cluster indices in logical order
    bool wrapped = false;               // ended by width, so trailing spaces hang
};

// Marks that sit below their base (the rest go above).
bool isBelowMark(u32 cp) {
    switch (cp) {
        case 0x064D: case 0x0650: case 0x0655: case 0x0656: case 0x061A: case 0x0659:
            return true;
        default:
            break;
    }
    if (cp >= 0x05B0 && cp <= 0x05BD) return true;                                   // Hebrew points
    if (cp >= 0x0316 && cp <= 0x0319) return true;
    if (cp >= 0x031C && cp <= 0x0320) return true;
    if (cp >= 0x0323 && cp <= 0x0326) return true;
    if (cp >= 0x0329 && cp <= 0x0333) return true;
    if (cp >= 0x0339 && cp <= 0x033C) return true;
    return false;
}

// ---- the builder --------------------------------------------------------------------------------

class Builder {
public:
    Builder(UiFontSet& fonts, const UiTextStyle& style, const UiTextBox& box) : fonts_(fonts), style_(style), box_(box) {}

    std::shared_ptr<UiTextLayout> run(std::string_view text, bool& capped);

private:
    struct Metrics {
        f32 adv = 0, ascent = 0, descent = 0, gap = 0, dy = 0;
        u32 rasterPx = 0;
        f32 quad = 1;
        UiFontFace* face = nullptr;
        const UiGlyph* bitmap = nullptr;
    };

    CpStyle styleAt(usize cp) const { return cs_.empty() ? base_ : cs_[std::min(cp, cs_.size() - 1)]; }
    bool metricsFor(const UiFontSet::Resolved& r, u32 cp, const CpStyle& st, Metrics& m);
    void primaryMetrics(const CpStyle& st, Para& p);
    void processParagraph(usize pb, usize pe);
    void buildCluster(Para& P, const std::vector<UiShapedCp>& units, usize u0, usize u1, const UiBidiResult* bidi,
                      const std::vector<UiBreak>& brk, usize pb, bool& havePrev, UiFontFace*& prevFace, u32& prevGlyph,
                      u32& prevPx, u8& prevLevel, bool& prevArabic);
    void addMark(Para& P, Cluster& c, const UiShapedCp& mark, const Metrics& baseM, u32 baseGlyph, u16 baseFace,
                 const CpStyle& st, f32& aboveTop, f32& belowBottom, u8 level);
    void fillLines(u32 paraIndex);
    void truncateWithEllipsis(LineSel& line);
    void finishLines(UiTextLayout& L);
    Cluster makeEllipsisCluster(Para& P, const CpStyle& st, u32 cpOut, u32 srcPos);

    UiFontSet& fonts_;
    const UiTextStyle& style_;
    const UiTextBox& box_;
    std::vector<u32> cps_;
    std::vector<CpStyle> cs_;
    CpStyle base_;
    std::vector<u32> palette_{0};
    std::vector<Para> paras_;
    std::vector<LineSel> lines_;
    usize glyphCount_ = 0;
    bool capped_ = false;
};

bool Builder::metricsFor(const UiFontSet::Resolved& r, u32 cp, const CpStyle& st, Metrics& m) {
    m = Metrics{};
    if (r.face == kUiFaceBitmap) {
        const UiFont* bf = fonts_.bitmapFont();
        const UiGlyph* g = bf ? bf->glyph(cp) : nullptr;
        if (!g) return false;
        rasterFor(st.px, m.rasterPx, m.quad);
        const f32 s = bf->pixelSize > 0.0f ? (static_cast<f32>(m.rasterPx) * m.quad) / bf->pixelSize : 1.0f;
        m.adv = g->advance * s;
        m.ascent = bf->ascent * s;
        m.descent = bf->descent * s;
        m.gap = std::max(0.0f, bf->lineHeight - (bf->ascent - bf->descent)) * s;
        m.bitmap = g;
        return true;
    }
    UiFontFace* f = fonts_.face(r.face);
    if (!f) return false;
    const UiFontEntry& e = fonts_.faceEntry(r.face);
    const f32 px = st.px * e.scale;
    rasterFor(px, m.rasterPx, m.quad);
    const f32 eff = static_cast<f32>(m.rasterPx);
    m.face = f;
    m.adv = f->advance(r.glyph, eff) * m.quad;
    m.ascent = f->ascent(eff) * m.quad;
    m.descent = f->descent(eff) * m.quad;
    m.gap = f->lineGap(eff) * m.quad * fonts_.lineGapScale(st.family);
    m.dy = e.dy * (eff * m.quad) / 16.0f;
    return true;
}

void Builder::primaryMetrics(const CpStyle& st, Para& p) {
    const u16 id = fonts_.primaryFace(st.family, st.bold, st.italic);
    Metrics m;
    UiFontSet::Resolved r;
    r.face = id;
    p.gapScale = fonts_.lineGapScale(st.family);
    if (id != kUiFaceNone && metricsFor(r, 0, st, m)) {
        p.primAscent = m.ascent; p.primDescent = m.descent; p.primGap = m.gap;
        return;
    }
    if (const UiFont* bf = fonts_.bitmapFont()) {
        const f32 s = bf->pixelSize > 0.0f ? st.px / bf->pixelSize : 1.0f;
        p.primAscent = bf->ascent * s;
        p.primDescent = bf->descent * s;
        p.primGap = std::max(0.0f, bf->lineHeight - (bf->ascent - bf->descent)) * s;
        return;
    }
    p.primAscent = st.px * 0.92f;
    p.primDescent = -st.px * 0.25f;
    p.primGap = 0.0f;
}

Cluster Builder::makeEllipsisCluster(Para& P, const CpStyle& st, u32 cpOut, u32 srcPos) {
    Cluster c;
    c.srcBegin = c.srcEnd = srcPos;
    c.level = P.level;
    c.cp0 = cpOut;
    c.cls = UiBidiClass::ON;
    c.gBegin = static_cast<u32>(P.glyphs.size());

    UiFontSet::Resolved r = fonts_.resolve(st.family, st.bold, st.italic, cpOut);
    Metrics m;
    TGlyph g;
    g.color = st.color;
    if (r.face != kUiFaceNone && metricsFor(r, cpOut, st, m)) {
        g.glyph = r.glyph;
        g.face = r.face;
        g.rasterPx = static_cast<u16>(m.rasterPx);
        g.quad = m.quad;
        g.y = m.dy;
        c.width = m.adv;
        c.ascent = m.ascent; c.descent = m.descent; c.gap = m.gap;
        if (m.face && m.face->isBlank(r.glyph)) { /* a blank ellipsis draws nothing */ }
        else if (r.face == kUiFaceBitmap && m.bitmap && (m.bitmap->w <= 0.0f || m.bitmap->h <= 0.0f)) { }
        else P.glyphs.push_back(g);
    } else {
        rasterFor(st.px, m.rasterPx, m.quad);
        g.face = kUiFaceNone;
        g.rasterPx = static_cast<u16>(m.rasterPx);
        g.quad = m.quad;
        g.flags = kGlyphBox;
        c.width = 0.6f * st.px;
        c.ascent = P.primAscent; c.descent = P.primDescent; c.gap = P.primGap;
        P.glyphs.push_back(g);
    }
    c.gEnd = static_cast<u32>(P.glyphs.size());
    return c;
}

// Places one combining mark on its base from glyph bounding boxes: centre to centre horizontally; above
// marks are lifted to clear the base top (and the previous mark's top) by 0.06 em, below marks mirrored.
void Builder::addMark(Para& P, Cluster& c, const UiShapedCp& mark, const Metrics& baseM, u32 baseGlyph, u16 baseFace,
                      const CpStyle& st, f32& aboveTop, f32& belowBottom, u8 level) {
    if (uiIsFormatChar(mark.base)) return;   // ZWJ, ZWNJ, variation selectors: no glyph

    // Prefer the base's own face for the mark, else the chain.
    UiFontSet::Resolved r;
    if (baseFace != kUiFaceNone && baseFace != kUiFaceBitmap) {
        UiFontFace* bf = fonts_.face(baseFace);
        if (bf) {
            const u32 g = bf->glyphFor(mark.cp);
            if (g != 0) { r.face = baseFace; r.glyph = g; }
        }
    }
    if (r.face == kUiFaceNone) r = fonts_.resolve(st.family, st.bold, st.italic, mark.cp);
    if (r.face == kUiFaceNone && mark.cp != mark.base) r = fonts_.resolve(st.family, st.bold, st.italic, mark.base);

    (void)level;
    TGlyph g;
    g.color = st.color;
    g.flags = kGlyphMark;
    Metrics m;
    if (r.face == kUiFaceNone || !metricsFor(r, mark.cp, st, m)) {
        rasterFor(st.px, m.rasterPx, m.quad);
        g.face = kUiFaceNone;
        g.rasterPx = static_cast<u16>(m.rasterPx);
        g.quad = m.quad;
        g.flags = static_cast<u8>(kGlyphMark | kGlyphBox);
        g.x = 0.0f;
        g.y = 0.0f;
        P.glyphs.push_back(g);
        return;
    }
    g.glyph = r.glyph;
    g.face = r.face;
    g.rasterPx = static_cast<u16>(m.rasterPx);
    g.quad = m.quad;
    g.y = m.dy;

    const f32 em = static_cast<f32>(m.rasterPx) * m.quad;
    // Boxes in pixels, y up, from the cluster origin.
    f32 bx0 = 0.0f, bx1 = baseM.adv, by0 = 0.0f, by1 = 0.7f * em;
    if (baseM.face && baseFace != kUiFaceBitmap) {
        i32 x0, y0, x1, y1;
        if (baseM.face->glyphBox(baseGlyph, x0, y0, x1, y1)) {
            const f32 s = static_cast<f32>(baseM.rasterPx) * baseM.quad / static_cast<f32>(baseM.face->unitsPerEm());
            bx0 = static_cast<f32>(x0) * s; bx1 = static_cast<f32>(x1) * s;
            by0 = static_cast<f32>(y0) * s; by1 = static_cast<f32>(y1) * s;
        }
    }
    f32 mx0 = 0.0f, mx1 = 0.0f, my0 = 0.0f, my1 = 0.0f;
    bool haveBox = false;
    if (m.face) {
        i32 x0, y0, x1, y1;
        if (m.face->glyphBox(r.glyph, x0, y0, x1, y1)) {
            const f32 s = static_cast<f32>(m.rasterPx) * m.quad / static_cast<f32>(m.face->unitsPerEm());
            mx0 = static_cast<f32>(x0) * s; mx1 = static_cast<f32>(x1) * s;
            my0 = static_cast<f32>(y0) * s; my1 = static_cast<f32>(y1) * s;
            haveBox = true;
        }
    }
    if (haveBox) {
        g.x = (bx0 + bx1) * 0.5f - (mx0 + mx1) * 0.5f;
        const f32 gapPx = 0.06f * em;
        if (isBelowMark(mark.base)) {
            const f32 refBottom = std::min(by0, belowBottom);
            const f32 drop = std::max(0.0f, my1 - (refBottom - gapPx));
            g.y += drop;                           // y is down
            belowBottom = my0 - drop;
        } else {
            const f32 refTop = std::max(by1, aboveTop);
            const f32 lift = std::max(0.0f, (refTop + gapPx) - my0);
            g.y -= lift;
            aboveTop = my1 + lift;
        }
    }
    P.glyphs.push_back(g);
    (void)c;
}

void Builder::buildCluster(Para& P, const std::vector<UiShapedCp>& units, usize u0, usize u1, const UiBidiResult* bidi,
                           const std::vector<UiBreak>& brk, usize pb, bool& havePrev, UiFontFace*& prevFace,
                           u32& prevGlyph, u32& prevPx, u8& prevLevel, bool& prevArabic) {
    const UiShapedCp& b = units[u0];
    Cluster c;
    c.gBegin = static_cast<u32>(P.glyphs.size());
    u32 srcEnd = b.src + b.srcCount;
    for (usize k = u0 + 1; k < u1; ++k) srcEnd = std::max<u32>(srcEnd, units[k].src + units[k].srcCount);
    c.srcBegin = static_cast<u32>(pb) + b.src;
    c.srcEnd = static_cast<u32>(pb) + srcEnd;
    c.level = bidi ? bidi->levels[b.src] : P.level;
    c.cls = bidi ? bidi->classes[b.src] : UiBidiClass::L;
    c.breakBefore = b.src < brk.size() ? brk[b.src] : UiBreak::No;
    c.cp0 = b.base;
    const CpStyle st = styleAt(pb + b.src);

    const bool odd = (c.level & 1) != 0;
    u32 cp = b.cp;
    if (odd && !(b.flags & kShapeJoined)) cp = uiMirror(cp);
    const bool arabic = uiScriptOf(b.base) == UiScript::Arabic;
    const bool tab = cp == 0x09;
    if (tab) cp = 0x20;
    c.space = uiIsHangingSpace(b.base);

    Metrics m;
    f32 advance = 0.0f;
    bool placed = false;      // a base glyph was resolved with metrics
    u32 baseGlyph = 0;
    u16 baseFace = kUiFaceNone;
    Metrics baseM;

    const bool zeroWidth = (uiIsControlChar(cp) && !tab) || uiIsFormatChar(cp) || cp == 0x2028;
    if (!zeroWidth) {
        UiFontSet::Resolved r = fonts_.resolve(st.family, st.bold, st.italic, cp);
        u8 gflags = 0;
        bool ligatureFallback = false;
        if (r.face == kUiFaceNone && (b.flags & (kShapeJoined | kShapeNoForm))) {
            // The face lacks the presentation form: draw the base letter(s) unjoined.
            if (b.flags & kShapeLigature) {
                ligatureFallback = true;
            } else {
                r = fonts_.resolve(st.family, st.bold, st.italic, b.base);
                gflags |= kGlyphUnjoined;
            }
        }

        if (ligatureFallback) {
            // lam then alef, each from the chain; right to left inside the cluster when the level is odd.
            const u32 first = odd ? b.base2 : b.base, second = odd ? b.base : b.base2;
            f32 x = 0.0f;
            for (const u32 pcp : {first, second}) {
                UiFontSet::Resolved lr = fonts_.resolve(st.family, st.bold, st.italic, pcp);
                Metrics lm;
                TGlyph g;
                g.color = st.color;
                g.flags = kGlyphUnjoined;
                if (lr.face != kUiFaceNone && metricsFor(lr, pcp, st, lm)) {
                    g.glyph = lr.glyph; g.face = lr.face; g.rasterPx = static_cast<u16>(lm.rasterPx); g.quad = lm.quad;
                    g.x = x; g.y = lm.dy;
                    x += lm.adv;
                    c.ascent = std::max(c.ascent, lm.ascent); c.descent = std::min(c.descent, lm.descent);
                    c.gap = std::max(c.gap, lm.gap);
                    if (!(lm.face && lm.face->isBlank(lr.glyph))) P.glyphs.push_back(g);
                } else {
                    rasterFor(st.px, lm.rasterPx, lm.quad);
                    g.face = kUiFaceNone; g.rasterPx = static_cast<u16>(lm.rasterPx); g.quad = lm.quad;
                    g.flags = static_cast<u8>(kGlyphBox | kGlyphUnjoined);
                    g.x = x;
                    x += 0.6f * st.px;
                    P.glyphs.push_back(g);
                }
            }
            advance = x;
            placed = false;
        } else if (r.face != kUiFaceNone && metricsFor(r, cp, st, m)) {
            advance = m.adv * (tab ? 4.0f : 1.0f);
            TGlyph g;
            g.glyph = r.glyph;
            g.face = r.face;
            g.rasterPx = static_cast<u16>(m.rasterPx);
            g.quad = m.quad;
            g.y = m.dy;
            g.color = st.color;
            g.flags = gflags;
            if (st.italic && r.face != kUiFaceBitmap && !fonts_.faceEntry(r.face).italic)
                g.flags = static_cast<u8>(g.flags | kGlyphSkew);   // no italic face: shear the regular one
            const bool blank = tab || (m.face && m.face->isBlank(r.glyph)) ||
                               (m.bitmap && (m.bitmap->w <= 0.0f || m.bitmap->h <= 0.0f));
            if (!blank) P.glyphs.push_back(g);
            c.ascent = m.ascent; c.descent = m.descent; c.gap = m.gap;
            baseM = m;
            baseGlyph = r.glyph;
            baseFace = r.face;
            placed = !blank;
        } else if (uiIsHangingSpace(b.base) || b.base == 0xA0 || b.base == 0x202F) {
            // No face carries a space: advance a quarter em, draw nothing.
            advance = 0.25f * st.px * (tab ? 4.0f : 1.0f);
        } else {
            // The visible answer to "your font set cannot show this": a hollow box.
            u32 rp; f32 q;
            rasterFor(st.px, rp, q);
            TGlyph g;
            g.face = kUiFaceNone;
            g.rasterPx = static_cast<u16>(rp);
            g.quad = q;
            g.color = st.color;
            g.flags = kGlyphBox;
            g.x = 0.05f * st.px;
            P.glyphs.push_back(g);
            advance = 0.6f * st.px;
            placed = false;
        }
    }

    // Fonts disagree about nothing: a cluster without a glyph still carries line metrics.
    if (c.ascent == 0.0f && c.descent == 0.0f) { c.ascent = P.primAscent; c.descent = P.primDescent; c.gap = P.primGap; }

    // Kerning with the previous base glyph (same face and size and direction, no Arabic).
    f32 kern = 0.0f;
    if (placed && baseM.face && havePrev && prevFace == baseM.face && prevPx == baseM.rasterPx && prevLevel == c.level &&
        !prevArabic && !arabic)
        kern = baseM.face->kern(prevGlyph, baseGlyph, static_cast<f32>(baseM.rasterPx)) * baseM.quad;
    if (kern != 0.0f && !P.clusters.empty()) P.clusters.back().width += kern;

    c.width = advance;
    // Letter spacing follows the glyph; not inside Arabic, where it would break the joins.
    if (!zeroWidth && !arabic) c.width += style_.letterSpacing;

    // Marks: zero advance, placed from boxes.
    f32 aboveTop = -1.0e9f, belowBottom = 1.0e9f;
    for (usize k = u0 + 1; k < u1; ++k)
        addMark(P, c, units[k], baseM, baseGlyph, baseFace, st, aboveTop, belowBottom, c.level);

    c.gEnd = static_cast<u32>(P.glyphs.size());
    glyphCount_ += c.gEnd - c.gBegin;

    // Remember the base for the next cluster's kerning.
    if (placed && baseM.face) {
        havePrev = true;
        prevFace = baseM.face;
        prevGlyph = baseGlyph;
        prevPx = baseM.rasterPx;
        prevLevel = c.level;
        prevArabic = arabic;
    } else {
        havePrev = false;
    }
    P.clusters.push_back(c);
}

void Builder::processParagraph(usize pb, usize pe) {
    const usize n = pe - pb;
    Para P;
    P.pb = static_cast<u32>(pb);
    P.pe = static_cast<u32>(pe);
    const CpStyle st0 = n > 0 ? styleAt(pb) : base_;
    primaryMetrics(st0, P);

    if (n == 0) {
        P.level = style_.dir == UiTextDir::RTL ? 1 : 0;
        paras_.push_back(std::move(P));
        LineSel l;
        l.para = static_cast<u32>(paras_.size() - 1);
        lines_.push_back(std::move(l));
        return;
    }

    const u32* data = cps_.data() + pb;
    UiBidiResult bidi;
    uiBidiResolve(data, n, style_.dir, bidi);
    P.level = bidi.paragraphLevel;

    std::vector<UiShapedCp> units;
    uiShapeText(data, n, units);
    std::vector<UiBreak> brk;
    uiLineBreaks(data, n, brk);

    bool havePrev = false, prevArabic = false;
    UiFontFace* prevFace = nullptr;
    u32 prevGlyph = 0, prevPx = 0;
    u8 prevLevel = 0;
    for (usize u = 0; u < units.size();) {
        usize v = u + 1;
        while (v < units.size() && (uiIsCombining(units[v].base) || uiIsFormatChar(units[v].base)) &&
               !(units[v].flags & kShapeLigature))
            ++v;
        buildCluster(P, units, u, v, &bidi, brk, pb, havePrev, prevFace, prevGlyph, prevPx, prevLevel, prevArabic);
        u = v;
    }

    paras_.push_back(std::move(P));
    fillLines(static_cast<u32>(paras_.size() - 1));
}

void Builder::fillLines(u32 pi) {
    Para& P = paras_[pi];
    const usize m = P.clusters.size();
    if (m == 0) {
        LineSel l;
        l.para = pi;
        lines_.push_back(std::move(l));
        return;
    }
    const bool wrap = box_.wrap && box_.maxWidth > 0.0f;
    const f32 maxW = box_.maxWidth;

    LineSel cur;
    cur.para = pi;
    f32 curAll = 0.0f;
    const auto endLine = [&](bool wrapped) {
        cur.wrapped = wrapped;
        if (lines_.size() < kMaxLines) lines_.push_back(std::move(cur)); else capped_ = true;
        cur = LineSel{};
        cur.para = pi;
        curAll = 0.0f;
    };

    usize i = 0;
    while (i < m) {
        usize j = i + 1;
        while (j < m && P.clusters[j].breakBefore == UiBreak::No) ++j;
        f32 segW = 0.0f, segTrail = 0.0f;
        for (usize k = i; k < j; ++k) segW += P.clusters[k].width;
        for (usize k = j; k > i && P.clusters[k - 1].space; --k) segTrail += P.clusters[k - 1].width;
        const bool mandatoryAfter = j < m && P.clusters[j].breakBefore == UiBreak::Mandatory;

        if (!wrap || curAll + segW - segTrail <= maxW + kFitEps) {
            for (usize k = i; k < j; ++k) cur.idx.push_back(static_cast<u32>(k));
            curAll += segW;
            i = j;
        } else if (!cur.idx.empty()) {
            endLine(true);       // retry this segment on a fresh line
            continue;
        } else {
            // One segment wider than the line: break between clusters, at least one per line.
            usize k = i;
            f32 w = 0.0f;
            while (k < j) {
                const Cluster& c = P.clusters[k];
                if (!cur.idx.empty() && !c.space && w + c.width > maxW + kFitEps) break;
                cur.idx.push_back(static_cast<u32>(k));
                w += c.width;
                ++k;
            }
            // Kinsoku push-down: not a closing mark first, not an opening mark last (twice at most).
            for (int pass = 0; pass < 2 && k < j && cur.idx.size() > 1; ++pass) {
                const Cluster& last = P.clusters[cur.idx.back()];
                const Cluster& next = P.clusters[k];
                if (!uiCannotStartLine(next.cp0) && !uiCannotEndLine(last.cp0)) break;
                cur.idx.pop_back();
                --k;
            }
            curAll = 0.0f;
            for (const u32 ci : cur.idx) curAll += P.clusters[ci].width;
            i = k;
            endLine(true);
            continue;
        }
        if (mandatoryAfter) endLine(false);
    }
    if (!cur.idx.empty()) endLine(false);
}

// Replaces the end of a line with U+2026 (three full stops if no face has it) so it fits maxWidth.
// Truncation is in logical order, so the mark lands at the visual end for the paragraph's direction.
void Builder::truncateWithEllipsis(LineSel& line) {
    Para& P = paras_[line.para];
    // Style of the last real code point on the line.
    CpStyle st = base_;
    for (usize k = line.idx.size(); k-- > 0;) {
        const Cluster& c = P.clusters[line.idx[k]];
        if (c.srcEnd > c.srcBegin) { st = styleAt(c.srcBegin); break; }
    }
    // Build the mark first so its width is known.
    std::vector<Cluster> ell;
    const UiFontSet::Resolved r = fonts_.resolve(st.family, st.bold, st.italic, 0x2026);
    f32 ellW = 0.0f;
    if (r.face != kUiFaceNone) {
        ell.push_back(makeEllipsisCluster(P, st, 0x2026, 0));
    } else {
        for (int i = 0; i < 3; ++i) ell.push_back(makeEllipsisCluster(P, st, '.', 0));
    }
    for (const Cluster& c : ell) ellW += c.width;

    while (!line.idx.empty() && P.clusters[line.idx.back()].space) line.idx.pop_back();
    f32 sum = 0.0f;
    for (const u32 ci : line.idx) sum += P.clusters[ci].width;
    const f32 maxW = box_.maxWidth;
    while (!line.idx.empty() && maxW > 0.0f && sum + ellW > maxW + kFitEps) {
        sum -= P.clusters[line.idx.back()].width;
        line.idx.pop_back();
    }
    const u32 end = line.idx.empty() ? P.pb : P.clusters[line.idx.back()].srcEnd;
    for (Cluster& c : ell) {
        c.srcBegin = c.srcEnd = end;
        P.clusters.push_back(c);
        line.idx.push_back(static_cast<u32>(P.clusters.size() - 1));
    }
    line.wrapped = false;
}

void Builder::finishLines(UiTextLayout& L) {
    f32 top = 0.0f;
    for (const LineSel& sel : lines_) {
        Para& P = paras_[sel.para];
        const usize n = sel.idx.size();

        UiTextLine line;
        line.glyphBegin = static_cast<u32>(L.glyphs.size());
        line.stopBegin = static_cast<u32>(L.stops.size());
        line.rtl = (P.level & 1) != 0;

        f32 asc = P.primAscent, desc = P.primDescent, gap = P.primGap;
        f32 width = 0.0f;

        if (n == 0) {
            line.srcBegin = line.srcEnd = P.pb;
            L.stops.push_back(UiCaretStop{P.pb, 0.0f});
        } else {
            std::vector<u8> levels(n);
            std::vector<UiBidiClass> classes(n);
            std::vector<f32> eff(n);
            for (usize k = 0; k < n; ++k) {
                const Cluster& c = P.clusters[sel.idx[k]];
                levels[k] = c.level;
                classes[k] = c.cls;
                eff[k] = c.width;
                asc = std::max(asc, c.ascent);
                desc = std::min(desc, c.descent);
                gap = std::max(gap, c.gap);
            }
            if (sel.wrapped)   // trailing spaces hang: they take no room
                for (usize k = n; k-- > 0 && P.clusters[sel.idx[k]].space;) eff[k] = 0.0f;
            uiBidiResetTrailing(classes.data(), levels.data(), n, P.level);
            std::vector<u32> order;
            uiBidiReorderLine(levels.data(), n, order);

            std::vector<f32> vx(n, 0.0f);
            f32 x = 0.0f;
            for (const u32 k : order) { vx[k] = x; x += eff[k]; }
            width = x;

            for (const u32 k : order) {
                const Cluster& c = P.clusters[sel.idx[k]];
                for (u32 g = c.gBegin; g < c.gEnd; ++g) {
                    if (L.glyphs.size() >= kMaxGlyphs) { capped_ = true; break; }
                    const TGlyph& t = P.glyphs[g];
                    UiGlyphRef r;
                    r.glyph = t.glyph;
                    r.face = t.face;
                    r.rasterPx = t.rasterPx;
                    r.quad = t.quad;
                    r.x = vx[k] + t.x;
                    r.y = t.y;
                    r.color = t.color;
                    r.flags = t.flags;
                    L.glyphs.push_back(r);
                }
            }

            bool haveSrc = false;
            line.srcBegin = line.srcEnd = P.pb;
            for (usize k = 0; k < n; ++k) {
                const Cluster& c = P.clusters[sel.idx[k]];
                if (c.srcEnd <= c.srcBegin) continue;
                if (!haveSrc) { line.srcBegin = c.srcBegin; haveSrc = true; }
                line.srcEnd = std::max(line.srcEnd, c.srcEnd);
                const bool even = (levels[k] & 1) == 0;
                L.stops.push_back(UiCaretStop{c.srcBegin, even ? vx[k] : vx[k] + eff[k]});
            }
            // The end of the line: the trailing edge of the last logical cluster.
            {
                const usize k = n - 1;
                const bool even = (levels[k] & 1) == 0;
                L.stops.push_back(UiCaretStop{line.srcEnd, even ? vx[k] + eff[k] : vx[k]});
            }
        }
        line.glyphEnd = static_cast<u32>(L.glyphs.size());
        line.stopEnd = static_cast<u32>(L.stops.size());
        line.width = width;

        const f32 ascR = std::round(asc), descR = std::round(desc), gapR = std::round(gap * P.gapScale);
        const f32 natural = ascR - descR + gapR;
        const f32 spacing = style_.lineSpacing > 0.0f ? style_.lineSpacing : 1.0f;
        const f32 height = std::max(1.0f, std::round(natural * spacing));
        const f32 extra = height - natural;
        line.ascent = ascR;
        line.descent = descR;
        line.height = height;
        line.baseline = std::max(0.0f, ascR + std::floor(extra * 0.5f));
        line.top = top;
        top += height;
        L.width = std::max(L.width, width);
        L.lines.push_back(line);
    }
    L.height = top;
}

std::shared_ptr<UiTextLayout> Builder::run(std::string_view text, bool& capped) {
    auto out = std::make_shared<UiTextLayout>();
    UiTextLayout& L = *out;
    L.px = style_.px;

    base_.bold = style_.bold;
    base_.italic = style_.italic;
    base_.family = fonts_.familyIndex(style_.family);
    base_.px = style_.px > 0.0f ? style_.px : 16.0f;
    base_.color = 0;

    std::vector<UiRichSpan> spans;
    if (box_.rich) {
        uiParseRich(text, cps_, spans);
        if (cps_.size() > kMaxCodePoints) { cps_.resize(kMaxCodePoints); capped_ = true; }
        L.source.clear();
        L.cpToByte.clear();
        for (const u32 c : cps_) { L.cpToByte.push_back(static_cast<u32>(L.source.size())); uiEncodeUtf8Append(L.source, c); }
        L.cpToByte.push_back(static_cast<u32>(L.source.size()));
        cs_.assign(cps_.size(), base_);
        for (const UiRichSpan& s : spans) {
            CpStyle st = base_;
            st.bold = st.bold || s.bold;
            st.italic = st.italic || s.italic;
            if (!s.font.empty()) st.family = fonts_.familyIndex(s.font);
            if (s.size > 0.0f) st.px = s.size * (style_.scale > 0.0f ? style_.scale : 1.0f);
            if (s.hasColor && box_.richColors) {
                usize found = 0;
                for (usize k = 1; k < palette_.size(); ++k) if (palette_[k] == s.color) { found = k; break; }
                if (found == 0) { palette_.push_back(s.color); found = palette_.size() - 1; }
                st.color = static_cast<u16>(found);
            }
            for (u32 k = s.begin; k < s.end && k < cs_.size(); ++k) cs_[k] = st;
        }
    } else {
        uiDecodeUtf8(text, cps_, &L.cpToByte, kMaxCodePoints + 1);
        if (cps_.size() > kMaxCodePoints) {
            cps_.resize(kMaxCodePoints);
            L.cpToByte.resize(kMaxCodePoints + 1);
            capped_ = true;
        }
        L.source.assign(text);
    }

    // Paragraphs: LF, CR, CR LF, NEL, PS end one; LS stays inside.
    usize pb = 0;
    const usize total = cps_.size();
    while (lines_.size() < kMaxLines) {
        usize pe = pb;
        while (pe < total && !uiIsParagraphBreak(cps_[pe])) ++pe;
        processParagraph(pb, pe);
        if (pe >= total) break;
        usize next = pe + 1;
        if (cps_[pe] == 0x0D && next < total && cps_[next] == 0x0A) ++next;
        pb = next;
    }
    if (lines_.size() >= kMaxLines) capped_ = true;

    // maxLines, then ellipsis.
    const usize maxLines = box_.maxLines;
    if (maxLines > 0 && lines_.size() > maxLines) {
        lines_.resize(maxLines);
        if (box_.ellipsis) truncateWithEllipsis(lines_.back());
    } else if (box_.ellipsis && box_.maxWidth > 0.0f && !(box_.wrap)) {
        for (LineSel& line : lines_) {
            const Para& P = paras_[line.para];
            f32 w = 0.0f;
            for (const u32 ci : line.idx) w += P.clusters[ci].width;
            if (w > box_.maxWidth + kFitEps) truncateWithEllipsis(line);
        }
    }

    finishLines(L);
    L.palette = palette_;
    capped = capped_;
    return out;
}

// ---- hashing ------------------------------------------------------------------------------------

u64 fnvBytes(u64 h, const void* data, usize n) {
    const u8* p = static_cast<const u8*>(data);
    for (usize i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

template <class T>
u64 fnvValue(u64 h, T v) { return fnvBytes(h, &v, sizeof v); }

} // namespace

// ---- engine -------------------------------------------------------------------------------------

UiTextEngine::UiTextEngine() { atlas_.configure({cfg_.pageSize, cfg_.maxPages}); }

void UiTextEngine::configure(const UiTextConfig& cfg) {
    const bool geometry = cfg.pageSize != cfg_.pageSize || cfg.maxPages != cfg_.maxPages;
    cfg_ = cfg;
    atlas_.setCoverageGamma(cfg_.coverageGamma > 0.0f ? cfg_.coverageGamma : 1.0f);
    if (cfg_.legacyPx < 4.0f) cfg_.legacyPx = 4.0f;
    if (cfg_.cacheEntries < 16) cfg_.cacheEntries = 16;
    if (geometry) {
        atlas_.configure({cfg_.pageSize, cfg_.maxPages});
        cfg_.pageSize = atlas_.config().pageSize;
        cfg_.maxPages = atlas_.config().maxPages;
        cache_.clear();
    }
}

bool UiTextEngine::loadFontSet(std::string_view ocfonts, const UiFontLoader& loader, std::string* err) {
    UiFontSetDoc doc;
    if (!UiFontSet::parse(ocfonts, doc, err)) return false;
    return loadFontSet(doc, loader, err);
}

bool UiTextEngine::loadFontSet(const UiFontSetDoc& doc, const UiFontLoader& loader, std::string* err) {
    const bool ok = fonts_.load(doc, loader, err);
    atlas_.clear();
    cache_.clear();
    return ok;
}

void UiTextEngine::setBitmapFont(const UiFont* font) {
    fonts_.setBitmapFont(font);
}

void UiTextEngine::setLanguage(std::string_view bcp47) { fonts_.setLanguage(bcp47); }

bool UiTextEngine::hasGlyph(std::string_view family, u32 codePoint) {
    const i32 fam = fonts_.familyIndex(family);
    return fam >= 0 && fonts_.resolve(fam, false, false, codePoint).face != kUiFaceNone;
}

void UiTextEngine::beginFrame() {
    ++frame_;
    rasterThisFrame_ = 0;
}

u32 UiTextEngine::pendingUploads() const {
    u32 n = 0;
    for (u32 i = 0; i < atlas_.pageCount(); ++i) if (atlas_.pageDirty(i)) ++n;
    return n;
}

void UiTextEngine::clearLayouts() { cache_.clear(); }

u64 UiTextEngine::keyFor(std::string_view text, const UiTextStyle& style, const UiTextBox& box) const {
    u64 h = 0xCBF29CE484222325ull;
    h = fnvBytes(h, text.data(), text.size());
    h = fnvBytes(h, style.family.data(), style.family.size());
    const bool big = style.px > static_cast<f32>(kMaxRaster) || style.px < static_cast<f32>(kMinRaster);
    h = fnvValue<u32>(h, big ? (0x80000000u | static_cast<u32>(std::lround(style.px * 4.0f))) : clampRaster(style.px));
    const u32 flags = (style.bold ? 1u : 0u) | (style.italic ? 2u : 0u) | (box.wrap ? 4u : 0u) | (box.ellipsis ? 8u : 0u) |
                      (box.rich ? 16u : 0u) | (box.richColors ? 32u : 0u) | (box.carets ? 64u : 0u);
    h = fnvValue<u32>(h, flags);
    h = fnvValue<u32>(h, static_cast<u32>(style.dir));
    h = fnvValue<u32>(h, box.maxLines);
    h = fnvValue<u32>(h, fonts_.epoch());
    h = fnvValue<f32>(h, style.lineSpacing);
    h = fnvValue<f32>(h, style.letterSpacing);
    if (box.rich) h = fnvValue<f32>(h, style.scale);
    if (box.wrap || box.ellipsis) h = fnvValue<i32>(h, static_cast<i32>(std::lround(box.maxWidth * 4.0f)));
    return h;
}

bool UiTextEngine::ensureGlyph(u16 face, u32 glyph, u32 rasterPx, bool ignoreBudget) {
    const UiGlyphKey key{face, static_cast<u16>(rasterPx), glyph};
    if (atlas_.lookup(key, frame_)) return true;
    if (!ignoreBudget && cfg_.glyphBudget != 0 && rasterThisFrame_ >= cfg_.glyphBudget) return false;
    UiFontFace* f = fonts_.face(face);
    if (!f) return true;
    UiGlyphBitmap bmp;
    if (!f->rasterize(glyph, rasterPx, bmp)) bmp = UiGlyphBitmap{};   // unrasterisable: remembered as blank
    ++rasterThisFrame_;
    ++rasterTotal_;
    return atlas_.insert(key, bmp, frame_) != nullptr;
}

bool UiTextEngine::ensureGlyphs(const UiTextLayout& l, bool ignoreBudget) {
    bool ok = true;
    for (const UiGlyphRef& g : l.glyphs) {
        if (g.face == kUiFaceNone || g.face == kUiFaceBitmap || (g.flags & kGlyphBox)) continue;
        if (!ensureGlyph(g.face, g.glyph, g.rasterPx, ignoreBudget)) ok = false;
    }
    return ok;
}

std::shared_ptr<UiTextLayout> UiTextEngine::build(std::string_view text, const UiTextStyle& style, const UiTextBox& box,
                                                  bool /*ignoreBudget*/) {
    Builder b(fonts_, style, box);
    bool capped = false;
    std::shared_ptr<UiTextLayout> l = b.run(text, capped);
    if (capped && !loggedCaps_) {
        loggedCaps_ = true;
        AVER_WARN("[UiText] a text exceeded the layout caps (16384 code points, 512 lines, 32768 glyphs): the excess is dropped");
    }
    return l;
}

void UiTextEngine::trimCache() {
    if (cache_.size() <= cfg_.cacheEntries) return;
    std::vector<std::pair<u64, u64>> order;   // (lastFrame, key)
    order.reserve(cache_.size());
    for (const auto& kv : cache_) order.emplace_back(kv.second.lastFrame, kv.first);
    std::sort(order.begin(), order.end());
    const usize drop = std::max<usize>(1, cache_.size() / 4);
    for (usize i = 0; i < drop && i < order.size(); ++i) cache_.erase(order[i].second);
}

std::shared_ptr<const UiTextLayout> UiTextEngine::layout(std::string_view text, const UiTextStyle& style,
                                                         const UiTextBox& box) {
    const u64 key = keyFor(text, style, box);
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        Entry& e = it->second;
        e.lastFrame = frame_;
        if (e.evictionsSeen == atlas_.evictedPages()) { ++hits_; return e.layout; }
        // A page was recycled since this was cached: check its glyphs are still resident.
        if (ensureGlyphs(*e.layout, false)) {
            e.evictionsSeen = atlas_.evictedPages();
            ++hits_;
            return e.layout;
        }
        cache_.erase(it);
    }
    ++misses_;
    std::shared_ptr<UiTextLayout> l = build(text, style, box, false);
    if (!ensureGlyphs(*l, false)) {
        l->incomplete = true;
        ++incomplete_;
        return l;
    }
    Entry e;
    e.layout = l;
    e.lastFrame = frame_;
    e.evictionsSeen = atlas_.evictedPages();
    cache_[key] = std::move(e);
    trimCache();
    return l;
}

void UiTextEngine::prewarm(std::string_view text, const UiTextStyle& style, const UiTextBox& box) {
    std::shared_ptr<UiTextLayout> l = build(text, style, box, true);
    if (!ensureGlyphs(*l, true)) { l->incomplete = true; return; }
    Entry e;
    e.layout = l;
    e.lastFrame = frame_;
    e.evictionsSeen = atlas_.evictedPages();
    cache_[keyFor(text, style, box)] = std::move(e);
    trimCache();
}

f32 UiTextEngine::textWidth(std::string_view text, f32 px) {
    UiTextStyle st;
    st.px = px;
    return layout(text, st, UiTextBox{})->width;
}

f32 UiTextEngine::lineHeight(f32 px) {
    UiTextStyle st;
    st.px = px;
    const std::shared_ptr<const UiTextLayout> l = layout(std::string_view(), st, UiTextBox{});
    return l->lines.empty() ? px * 1.25f : l->lines[0].height;
}

f32 UiTextEngine::ascent(f32 px) {
    UiTextStyle st;
    st.px = px;
    const std::shared_ptr<const UiTextLayout> l = layout(std::string_view(), st, UiTextBox{});
    return l->lines.empty() ? px * 0.95f : l->lines[0].baseline;
}

UiTextStats UiTextEngine::stats() const {
    UiTextStats s;
    s.pages = atlas_.pageCount();
    s.glyphsResident = atlas_.glyphsResident();
    s.rasterThisFrame = rasterThisFrame_;
    s.rasterTotal = rasterTotal_;
    s.evictedPages = atlas_.evictedPages();
    s.overflow = atlas_.overflow();
    s.layoutHits = hits_;
    s.layoutMisses = misses_;
    s.cacheEntries = static_cast<u32>(cache_.size());
    s.incompleteLayouts = incomplete_;
    s.faces = fonts_.openFaceCount();
    s.uploadBytesLastFrame = atlas_.uploadBytes();
    return s;
}

// ---- emission -----------------------------------------------------------------------------------

void UiTextEngine::emitLine(UiDrawList& list, const UiTextLayout& layout, usize line, f32 originX, f32 baselineY,
                            u32 rgba) {
    if (line >= layout.lines.size()) return;
    const UiTextLine& tl = layout.lines[line];
    const f32 pageSize = static_cast<f32>(atlas_.pageSize());
    const UiFont* bitmap = fonts_.bitmapFont();

    for (u32 gi = tl.glyphBegin; gi < tl.glyphEnd && gi < layout.glyphs.size(); ++gi) {
        const UiGlyphRef& g = layout.glyphs[gi];
        u32 color = rgba;
        if (g.color != 0 && g.color < layout.palette.size()) {
            const u32 rich = layout.palette[g.color];
            const u32 a = ((rich >> 24) * (rgba >> 24) + 127) / 255;
            color = (rich & 0x00FFFFFFu) | (a << 24);
        }
        const f32 penX = std::round(originX + g.x);
        const f32 penY = std::round(baselineY + g.y);
        const f32 em = static_cast<f32>(g.rasterPx) * g.quad;

        if (g.flags & kGlyphBox) {
            // A hollow rectangle 0.55 em x 0.7 em, stroke max(1, round(px / 16)).
            const f32 w = std::max(2.0f, std::round(0.55f * em)), h = std::max(3.0f, std::round(0.7f * em));
            const f32 t = std::max(1.0f, std::round(em / 16.0f));
            const f32 x = penX, y = penY - h;
            list.addRect(x, y, w, t, color);
            list.addRect(x, y + h - t, w, t, color);
            list.addRect(x, y + t, t, h - 2 * t, color);
            list.addRect(x + w - t, y + t, t, h - 2 * t, color);
            continue;
        }

        if (g.face == kUiFaceBitmap) {
            const UiGlyph* bg = bitmap ? bitmap->glyph(g.glyph) : nullptr;
            if (!bg || bg->w <= 0.0f || bg->h <= 0.0f) continue;
            const f32 s = bitmap->pixelSize > 0.0f ? em / bitmap->pixelSize : 1.0f;
            list.addTexturedRect(penX + bg->offX * s, penY + bg->offY * s, bg->w * s, bg->h * s, bitmap->atlasTexture,
                                 bg->u0, bg->v0, bg->u1, bg->v1, color);
            continue;
        }

        const UiAtlasGlyph* a = atlas_.lookup(UiGlyphKey{g.face, g.rasterPx, g.glyph}, frame_);
        if (!a || a->w == 0 || a->h == 0 || a->page == kUiNoPage) continue;   // not resident: skipped, never rasterised here
        const f32 q = g.quad;
        const f32 u0 = static_cast<f32>(a->x) / pageSize, v0 = static_cast<f32>(a->y) / pageSize;
        const f32 u1 = static_cast<f32>(a->x + a->w) / pageSize, v1 = static_cast<f32>(a->y + a->h) / pageSize;
        list.addGlyphQuad(penX + static_cast<f32>(a->offX) * q, penY + static_cast<f32>(a->offY) * q,
                          static_cast<f32>(a->w) * q, static_cast<f32>(a->h) * q, kUiAtlasTokenBit | a->page, u0, v0, u1, v1,
                          color, (g.flags & kGlyphSkew) ? kItalicSkew : 0.0f, penY);
    }
}

} // namespace aver::ui
