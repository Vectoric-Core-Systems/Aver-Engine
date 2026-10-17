#pragma once
// A synthetic TrueType builder for the text tests. Every glyph is a filled rectangle with a chosen
// advance, so order, kerning, shaping and fallback are observable by arithmetic and a glyph's rasterised
// coverage is exactly computable. Tables: head hhea maxp hmtx cmap (format 4, optionally format 12) loca
// glyf name post, optionally `kern` (format 0) and GPOS PairPos (format 1 and 2, X advance only), and a
// TTC wrapper of several fonts. Header-only; nothing here is engine code.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace testfont {

using aver::i16;
using aver::u16;
using aver::u32;
using aver::u8;
using aver::usize;

struct GlyphSpec {
    int advance = 600;                       // font units
    int x0 = 50, y0 = 0, x1 = 550, y1 = 700; // the filled rectangle; x1 <= x0 makes a blank glyph (a space)
};

struct KernPair {
    u32 left = 0, right = 0;                 // code points
    int value = 0;                           // font units
};

// Class-based kerning (GPOS PairPos format 2): every glyph in `first` followed by one in `second`.
struct ClassKern {
    std::vector<u32> first, second;          // code points
    int value = 0;
};

struct Spec {
    std::map<u32, GlyphSpec> chars;          // code point -> glyph; glyph ids are 1.. in code point order
    int unitsPerEm = 1000;
    int ascent = 800, descent = -200, lineGap = 0;
    bool format12 = false;                   // add a cmap format 12 subtable (needed above the BMP)
    std::vector<KernPair> kern;              // `kern` table, format 0
    std::vector<KernPair> gposPairs;         // GPOS PairPos format 1
    std::vector<ClassKern> gposClasses;      // GPOS PairPos format 2
    GlyphSpec notdef{500, 50, 0, 450, 700};  // glyph 0
};

// The glyph id the builder assigns to a code point (0 if absent).
inline u32 glyphId(const Spec& s, u32 cp) {
    u32 id = 1;
    for (const auto& kv : s.chars) {
        if (kv.first == cp) return id;
        ++id;
    }
    return 0;
}

// A spec with Latin capitals, lower case and a space, advances that differ per letter.
inline Spec simpleSpec() {
    Spec s;
    for (u32 c = 'A'; c <= 'Z'; ++c) s.chars[c] = GlyphSpec{600 + static_cast<int>(c - 'A') * 10, 50, 0, 550 + static_cast<int>(c - 'A') * 10, 700};
    for (u32 c = 'a'; c <= 'z'; ++c) s.chars[c] = GlyphSpec{500 + static_cast<int>(c - 'a') * 10, 40, 0, 460 + static_cast<int>(c - 'a') * 10, 500};
    s.chars[' '] = GlyphSpec{300, 0, 0, 0, 0};
    for (u32 c = '0'; c <= '9'; ++c) s.chars[c] = GlyphSpec{600, 50, 0, 550, 700};
    return s;
}

namespace detail {

struct W {
    std::vector<u8> b;
    void u8_(u32 v) { b.push_back(static_cast<u8>(v & 0xFF)); }
    void u16_(u32 v) { u8_(v >> 8); u8_(v); }
    void i16_(int v) { u16_(static_cast<u32>(v) & 0xFFFFu); }
    void u32_(u32 v) { u16_(v >> 16); u16_(v & 0xFFFFu); }
    void pad4() { while (b.size() % 4) b.push_back(0); }
    void patch16(usize at, u32 v) { b[at] = static_cast<u8>(v >> 8); b[at + 1] = static_cast<u8>(v); }
    usize size() const { return b.size(); }
};

inline u32 log2floor(u32 v) {
    u32 r = 0;
    while ((1u << (r + 1)) <= v) ++r;
    return r;
}

inline bool blank(const GlyphSpec& g) { return g.x1 <= g.x0 || g.y1 <= g.y0; }

inline std::vector<u8> glyphData(const GlyphSpec& g) {
    W w;
    w.i16_(1);                       // numberOfContours
    w.i16_(g.x0); w.i16_(g.y0); w.i16_(g.x1); w.i16_(g.y1);
    w.u16_(3);                       // endPtsOfContours[0]
    w.u16_(0);                       // instructionLength
    for (int i = 0; i < 4; ++i) w.u8_(0x01);   // on-curve, 16-bit deltas
    // clockwise: (x0,y0) (x0,y1) (x1,y1) (x1,y0)
    w.i16_(g.x0); w.i16_(0); w.i16_(g.x1 - g.x0); w.i16_(0);
    w.i16_(g.y0); w.i16_(g.y1 - g.y0); w.i16_(0); w.i16_(g.y0 - g.y1);
    w.pad4();
    return w.b;
}

inline std::vector<u8> cmapTable(const Spec& s) {
    // format 4: one segment per BMP code point, plus the 0xFFFF terminator.
    struct Seg { u32 cp, gid; };
    std::vector<Seg> segs;
    u32 gid = 1;
    for (const auto& kv : s.chars) {
        if (kv.first <= 0xFFFE) segs.push_back({kv.first, gid});
        ++gid;
    }
    const u32 segCount = static_cast<u32>(segs.size()) + 1;
    W f4;
    f4.u16_(4);
    f4.u16_(16 + segCount * 8);                       // length
    f4.u16_(0);
    f4.u16_(segCount * 2);
    const u32 lg = log2floor(segCount);
    f4.u16_(2 * (1u << lg));
    f4.u16_(lg);
    f4.u16_(segCount * 2 - 2 * (1u << lg));
    for (const Seg& g : segs) f4.u16_(g.cp);
    f4.u16_(0xFFFF);
    f4.u16_(0);                                        // reservedPad
    for (const Seg& g : segs) f4.u16_(g.cp);
    f4.u16_(0xFFFF);
    for (const Seg& g : segs) f4.u16_((g.gid - g.cp) & 0xFFFFu);
    f4.u16_(1);
    for (usize i = 0; i < segCount; ++i) f4.u16_(0);   // idRangeOffset

    W f12;
    if (s.format12) {
        f12.u16_(12); f12.u16_(0);
        f12.u32_(16 + static_cast<u32>(s.chars.size()) * 12);
        f12.u32_(0);
        f12.u32_(static_cast<u32>(s.chars.size()));
        u32 g = 1;
        for (const auto& kv : s.chars) { f12.u32_(kv.first); f12.u32_(kv.first); f12.u32_(g++); }
    }

    W w;
    w.u16_(0);                                         // version
    const u32 n = s.format12 ? 2 : 1;
    w.u16_(n);
    const u32 off4 = 4 + n * 8;
    w.u16_(3); w.u16_(1); w.u32_(off4);                // Windows, Unicode BMP
    if (s.format12) { w.u16_(3); w.u16_(10); w.u32_(off4 + static_cast<u32>(f4.size())); }   // last matching record wins in stb
    w.b.insert(w.b.end(), f4.b.begin(), f4.b.end());
    w.b.insert(w.b.end(), f12.b.begin(), f12.b.end());
    return w.b;
}

inline std::vector<u8> kernTable(const Spec& s) {
    struct P { u32 l, r; int v; };
    std::vector<P> ps;
    for (const KernPair& k : s.kern) {
        const u32 l = glyphId(s, k.left), r = glyphId(s, k.right);
        if (l && r) ps.push_back({l, r, k.value});
    }
    std::sort(ps.begin(), ps.end(), [](const P& a, const P& b) { return (a.l << 16 | a.r) < (b.l << 16 | b.r); });
    W w;
    w.u16_(0); w.u16_(1);                              // version, nTables
    w.u16_(0);                                         // subtable version
    w.u16_(14 + static_cast<u32>(ps.size()) * 6);      // length
    w.u16_(0x0001);                                    // coverage: horizontal, format 0
    w.u16_(static_cast<u32>(ps.size()));
    const u32 n = static_cast<u32>(ps.size());
    const u32 lg = n ? log2floor(n) : 0;
    w.u16_(n ? 6 * (1u << lg) : 0);
    w.u16_(lg);
    w.u16_(n ? n * 6 - 6 * (1u << lg) : 0);
    for (const P& p : ps) { w.u16_(p.l); w.u16_(p.r); w.i16_(p.v); }
    return w.b;
}

inline void coverage1(W& w, std::vector<u32> glyphs) {
    std::sort(glyphs.begin(), glyphs.end());
    glyphs.erase(std::unique(glyphs.begin(), glyphs.end()), glyphs.end());
    w.u16_(1);
    w.u16_(static_cast<u32>(glyphs.size()));
    for (const u32 g : glyphs) w.u16_(g);
}

inline std::vector<u8> pairPos1(const Spec& s) {
    std::map<u32, std::vector<std::pair<u32, int>>> sets;   // first glyph -> (second glyph, value)
    for (const KernPair& k : s.gposPairs) {
        const u32 l = glyphId(s, k.left), r = glyphId(s, k.right);
        if (l && r) sets[l].emplace_back(r, k.value);
    }
    std::vector<u32> firsts;
    for (auto& kv : sets) { std::sort(kv.second.begin(), kv.second.end()); firsts.push_back(kv.first); }

    W cov;
    coverage1(cov, firsts);
    const u32 headerLen = 10 + 2 * static_cast<u32>(sets.size());
    W w;
    w.u16_(1);
    w.u16_(headerLen);                                 // coverageOffset (coverage placed right after the header)
    w.u16_(4); w.u16_(0);
    w.u16_(static_cast<u32>(sets.size()));
    u32 setOff = headerLen + static_cast<u32>(cov.size());
    for (const auto& kv : sets) {
        w.u16_(setOff);
        setOff += 2 + static_cast<u32>(kv.second.size()) * 4;
    }
    w.b.insert(w.b.end(), cov.b.begin(), cov.b.end());
    for (const auto& kv : sets) {
        w.u16_(static_cast<u32>(kv.second.size()));
        for (const auto& p : kv.second) { w.u16_(p.first); w.i16_(p.second); }
    }
    return w.b;
}

inline std::vector<u8> pairPos2(const Spec& s, const ClassKern& c) {
    std::vector<u32> first, second;
    for (const u32 cp : c.first) if (const u32 g = glyphId(s, cp)) first.push_back(g);
    for (const u32 cp : c.second) if (const u32 g = glyphId(s, cp)) second.push_back(g);
    std::sort(second.begin(), second.end());
    W cov;
    coverage1(cov, first);
    // ClassDef format 2 (ranges): each listed glyph is class 1.
    const auto classDef = [](std::vector<u32> g) {
        std::sort(g.begin(), g.end());
        W d;
        d.u16_(2);
        d.u16_(static_cast<u32>(g.size()));
        for (const u32 x : g) { d.u16_(x); d.u16_(x); d.u16_(1); }
        return d;
    };
    const W cd1 = classDef(first), cd2 = classDef(second);
    const u32 headerLen = 16 + 2 * 2 * 2;              // two classes x two classes, one 16-bit value each
    W w;
    w.u16_(2);
    w.u16_(headerLen);
    w.u16_(4); w.u16_(0);
    w.u16_(headerLen + static_cast<u32>(cov.size()));
    w.u16_(headerLen + static_cast<u32>(cov.size() + cd1.size()));
    w.u16_(2); w.u16_(2);
    w.i16_(0); w.i16_(0);                              // class1 0: no kerning
    w.i16_(0); w.i16_(c.value);                        // class1 1: class2 1 gets the value
    w.b.insert(w.b.end(), cov.b.begin(), cov.b.end());
    w.b.insert(w.b.end(), cd1.b.begin(), cd1.b.end());
    w.b.insert(w.b.end(), cd2.b.begin(), cd2.b.end());
    return w.b;
}

inline std::vector<u8> gposTable(const Spec& s) {
    std::vector<std::vector<u8>> subs;
    if (!s.gposPairs.empty()) subs.push_back(pairPos1(s));
    for (const ClassKern& c : s.gposClasses) subs.push_back(pairPos2(s, c));

    // One lookup (type 2) holding every subtable.
    W lookup;
    lookup.u16_(2); lookup.u16_(0);
    lookup.u16_(static_cast<u32>(subs.size()));
    u32 off = 6 + 2 * static_cast<u32>(subs.size());
    for (const auto& sub : subs) { lookup.u16_(off); off += static_cast<u32>(sub.size()); }
    for (const auto& sub : subs) lookup.b.insert(lookup.b.end(), sub.begin(), sub.end());

    W list;
    list.u16_(1);
    list.u16_(4);
    list.b.insert(list.b.end(), lookup.b.begin(), lookup.b.end());

    W w;
    w.u16_(1); w.u16_(0);
    w.u16_(10);                                        // scriptList
    w.u16_(12);                                        // featureList
    w.u16_(14);                                        // lookupList
    w.u16_(0);                                         // empty script list
    w.u16_(0);                                         // empty feature list
    w.b.insert(w.b.end(), list.b.begin(), list.b.end());
    return w.b;
}

struct Tbl {
    char tag[4];
    std::vector<u8> data;
};

} // namespace detail

// The font as bytes laid out for a file offset of `base` (0 for a plain font; the member's position when
// it goes inside a collection, because collection tables use absolute offsets).
inline std::vector<u8> buildFont(const Spec& s, usize base = 0) {
    using namespace detail;
    const u32 numGlyphs = static_cast<u32>(s.chars.size()) + 1;

    std::vector<GlyphSpec> glyphs;
    glyphs.push_back(s.notdef);
    for (const auto& kv : s.chars) glyphs.push_back(kv.second);

    // glyf / loca (long offsets)
    W glyf, loca;
    for (const GlyphSpec& g : glyphs) {
        loca.u32_(static_cast<u32>(glyf.size()));
        if (!blank(g)) { const std::vector<u8> d = glyphData(g); glyf.b.insert(glyf.b.end(), d.begin(), d.end()); }
    }
    loca.u32_(static_cast<u32>(glyf.size()));

    W hmtx;
    for (const GlyphSpec& g : glyphs) { hmtx.u16_(static_cast<u32>(g.advance)); hmtx.i16_(blank(g) ? 0 : g.x0); }

    int xMin = 0, yMin = 0, xMax = 0, yMax = 0, advMax = 0;
    for (const GlyphSpec& g : glyphs) {
        if (!blank(g)) { xMin = std::min(xMin, g.x0); yMin = std::min(yMin, g.y0); xMax = std::max(xMax, g.x1); yMax = std::max(yMax, g.y1); }
        advMax = std::max(advMax, g.advance);
    }

    W head;
    head.u32_(0x00010000); head.u32_(0x00010000); head.u32_(0); head.u32_(0x5F0F3CF5);
    head.u16_(0); head.u16_(static_cast<u32>(s.unitsPerEm));
    for (int i = 0; i < 4; ++i) head.u32_(0);          // created, modified
    head.i16_(xMin); head.i16_(yMin); head.i16_(xMax); head.i16_(yMax);
    head.u16_(0); head.u16_(8); head.i16_(2);
    head.i16_(1);                                      // indexToLocFormat: long
    head.i16_(0);

    W hhea;
    hhea.u32_(0x00010000);
    hhea.i16_(s.ascent); hhea.i16_(s.descent); hhea.i16_(s.lineGap);
    hhea.u16_(static_cast<u32>(advMax)); hhea.i16_(0); hhea.i16_(0); hhea.i16_(xMax);
    hhea.i16_(1); hhea.i16_(0); hhea.i16_(0);
    for (int i = 0; i < 4; ++i) hhea.i16_(0);
    hhea.i16_(0);
    hhea.u16_(numGlyphs);

    W maxp;
    maxp.u32_(0x00010000); maxp.u16_(numGlyphs);
    for (int i = 0; i < 13; ++i) maxp.u16_(0);

    W name;
    name.u16_(0); name.u16_(0); name.u16_(6);
    W post;
    post.u32_(0x00030000);
    for (int i = 0; i < 7; ++i) post.u32_(0);

    std::vector<Tbl> tables;
    const auto add = [&](const char* tag, const std::vector<u8>& d) {
        Tbl t;
        for (int i = 0; i < 4; ++i) t.tag[i] = tag[i];
        t.data = d;
        tables.push_back(std::move(t));
    };
    add("cmap", cmapTable(s));
    add("glyf", glyf.b);
    add("head", head.b);
    add("hhea", hhea.b);
    add("hmtx", hmtx.b);
    add("loca", loca.b);
    add("maxp", maxp.b);
    add("name", name.b);
    add("post", post.b);
    if (!s.kern.empty()) add("kern", kernTable(s));
    if (!s.gposPairs.empty() || !s.gposClasses.empty()) add("GPOS", gposTable(s));
    std::sort(tables.begin(), tables.end(), [](const Tbl& a, const Tbl& b) {
        return std::string(a.tag, 4) < std::string(b.tag, 4);
    });

    const u32 n = static_cast<u32>(tables.size());
    W out;
    out.u32_(0x00010000);
    out.u16_(n);
    const u32 lg = log2floor(n);
    out.u16_(16 * (1u << lg)); out.u16_(lg); out.u16_(n * 16 - 16 * (1u << lg));
    usize off = base + 12 + 16 * static_cast<usize>(n);
    for (const Tbl& t : tables) {
        for (int i = 0; i < 4; ++i) out.u8_(static_cast<u8>(t.tag[i]));
        out.u32_(0);                                   // checksum: not verified
        out.u32_(static_cast<u32>(off));
        out.u32_(static_cast<u32>(t.data.size()));
        off += (t.data.size() + 3) & ~static_cast<usize>(3);
    }
    for (const Tbl& t : tables) {
        out.b.insert(out.b.end(), t.data.begin(), t.data.end());
        out.pad4();
    }
    return out.b;
}

// A TrueType Collection of several fonts (each built at its position inside the file).
inline std::vector<u8> buildCollection(const std::vector<Spec>& members) {
    using namespace detail;
    W head;
    head.u8_('t'); head.u8_('t'); head.u8_('c'); head.u8_('f');
    head.u32_(0x00010000);
    head.u32_(static_cast<u32>(members.size()));
    usize pos = 12 + 4 * members.size();
    std::vector<std::vector<u8>> bodies;
    for (const Spec& m : members) {
        head.u32_(static_cast<u32>(pos));
        bodies.push_back(buildFont(m, pos));
        pos += bodies.back().size();
    }
    for (const auto& b : bodies) head.b.insert(head.b.end(), b.begin(), b.end());
    return head.b;
}

} // namespace testfont
