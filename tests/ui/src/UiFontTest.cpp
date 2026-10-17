// Headless test for the font layer: the sfnt validator, UiFontFace over stb_truetype (metrics, cmap,
// kerning from `kern` and GPOS, collections, rasterisation), the .ocfonts format and the fallback chain.
// Fonts are built in memory by TestFont.hpp; Roboto is read from the repo when AVER_REPO_ROOT is set.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiFont.hpp"
#include "aver/ui/UiFontFace.hpp"
#include "aver/ui/UiFontSet.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 0.001f) { return std::fabs(a - b) <= eps; }

static UiFontBlob blobOf(const std::vector<u8>& bytes) {
    auto keep = std::make_shared<std::vector<u8>>(bytes);
    UiFontBlob b;
    b.data = keep->data();
    b.size = keep->size();
    b.owner = keep;
    return b;
}

static u32 be16(const std::vector<u8>& b, usize at) { return (static_cast<u32>(b[at]) << 8) | b[at + 1]; }
static u32 be32(const std::vector<u8>& b, usize at) { return (be16(b, at) << 16) | be16(b, at + 2); }
static void put16(std::vector<u8>& b, usize at, u32 v) { b[at] = static_cast<u8>(v >> 8); b[at + 1] = static_cast<u8>(v); }
static void put32(std::vector<u8>& b, usize at, u32 v) { put16(b, at, v >> 16); put16(b, at + 2, v & 0xFFFF); }

struct DirEntry { std::string tag; usize recordAt; u32 off, len; };

// The table directory of a plain font.
static std::vector<DirEntry> directory(const std::vector<u8>& f) {
    std::vector<DirEntry> out;
    const u32 n = be16(f, 4);
    for (u32 i = 0; i < n; ++i) {
        const usize rec = 12 + 16 * static_cast<usize>(i);
        out.push_back({std::string(reinterpret_cast<const char*>(&f[rec]), 4), rec, be32(f, rec + 8), be32(f, rec + 12)});
    }
    return out;
}

static const DirEntry* findTable(const std::vector<DirEntry>& d, const char* tag) {
    for (const DirEntry& e : d) if (e.tag == tag) return &e;
    return nullptr;
}

static bool rejects(const std::vector<u8>& f, i32 index = 0) {
    std::string why;
    return !uiValidateSfnt(f.data(), f.size(), index, &why);
}

static testfont::Spec twoLetters() {
    testfont::Spec s;
    s.chars['A'] = {600, 50, 0, 550, 700};
    s.chars['V'] = {620, 40, 0, 580, 700};
    s.chars[' '] = {300, 0, 0, 0, 0};
    return s;
}

int main() {
    AVER_INFO("=== the validator accepts good fonts ===");
    {
        const testfont::Spec spec = testfont::simpleSpec();
        const std::vector<u8> f = testfont::buildFont(spec);
        check(uiValidateSfnt(f.data(), f.size(), 0), "the synthetic font validates");
        testfont::Spec wide = spec;
        wide.format12 = true;
        wide.chars[0x1F600] = {1000, 0, 0, 900, 900};
        const std::vector<u8> f12 = testfont::buildFont(wide);
        check(uiValidateSfnt(f12.data(), f12.size(), 0), "and one with a format 12 cmap");
#ifdef AVER_REPO_ROOT
        std::vector<u8> roboto;
        if (readFileBytes(std::string(AVER_REPO_ROOT) + "/third_party/fonts/Roboto-Regular.ttf", roboto)) {
            check(uiValidateSfnt(roboto.data(), roboto.size(), 0), "Roboto Regular validates");
            UiFontFace face;
            check(face.open(blobOf(roboto), 0), "and opens");
            check(face.glyphFor('A') != 0 && face.glyphFor(0x0416) != 0 && face.glyphFor(0x03B1) != 0,
                  "Roboto carries Latin, Cyrillic and Greek");
            check(face.unitsPerEm() == 2048, "Roboto is a 2048 unit font");
            const u32 gA = face.glyphFor('A'), gV = face.glyphFor('V');
            check(face.kern(gA, gV, 32.0f) < 0.0f, "Roboto kerns A then V closer");
        } else {
            AVER_INFO("  (Roboto not found under AVER_REPO_ROOT: skipped)");
        }
#endif
    }

    AVER_INFO("=== the validator rejects damaged fonts ===");
    {
        const testfont::Spec spec = testfont::simpleSpec();
        const std::vector<u8> good = testfont::buildFont(spec);
        const std::vector<DirEntry> dir = directory(good);
        int mutations = 0, rejected = 0;
        const auto expectReject = [&](const std::vector<u8>& f, const std::string& what, i32 index = 0) {
            ++mutations;
            if (rejects(f, index)) ++rejected;
            else AVER_ERROR("    NOT rejected: {}", what);
        };

        // truncation at the start of every table, and inside the directory
        for (const DirEntry& e : dir) {
            std::vector<u8> t(good.begin(), good.begin() + e.off);
            expectReject(t, "truncated before " + e.tag);
        }
        for (const usize cut : {0u, 4u, 11u, 12u, 20u}) {
            std::vector<u8> t(good.begin(), good.begin() + cut);
            expectReject(t, "truncated to " + std::to_string(cut));
        }
        // a table offset or length past the end
        for (const DirEntry& e : dir) {
            std::vector<u8> m = good;
            put32(m, e.recordAt + 8, static_cast<u32>(good.size()) + 100);
            expectReject(m, "offset past the end for " + e.tag);
        }
        {
            std::vector<u8> m = good;
            put32(m, findTable(dir, "glyf")->recordAt + 12, 0x7FFFFFFF);
            expectReject(m, "glyf length huge");
        }
        // a required table renamed away
        for (const char* tag : {"cmap", "head", "hhea", "hmtx", "maxp", "glyf", "loca"}) {
            std::vector<u8> m = good;
            m[findTable(dir, tag)->recordAt] = 'X';
            expectReject(m, std::string("missing ") + tag);
        }
        // head: unitsPerEm and loca format
        const u32 headOff = findTable(dir, "head")->off;
        for (const u32 upem : {0u, 8u, 20000u}) {
            std::vector<u8> m = good;
            put16(m, headOff + 18, upem);
            expectReject(m, "unitsPerEm " + std::to_string(upem));
        }
        {
            std::vector<u8> m = good;
            put16(m, headOff + 50, 2);
            expectReject(m, "indexToLocFormat 2");
        }
        // maxp / hhea
        {
            std::vector<u8> m = good;
            put16(m, findTable(dir, "maxp")->off + 4, 0);
            expectReject(m, "numGlyphs 0");
            m = good;
            put16(m, findTable(dir, "hhea")->off + 34, 0);
            expectReject(m, "numberOfHMetrics 0");
            m = good;
            put16(m, findTable(dir, "hhea")->off + 34, 5000);
            expectReject(m, "numberOfHMetrics above numGlyphs");
            m = good;
            put16(m, findTable(dir, "maxp")->off + 4, 5000);
            expectReject(m, "numGlyphs far above the tables");
        }
        // overlapping tables
        {
            std::vector<u8> m = good;
            put32(m, dir[1].recordAt + 8, dir[0].off);
            expectReject(m, "two tables at one offset");
        }
        // cmap damage
        {
            const u32 cmapOff = findTable(dir, "cmap")->off;
            std::vector<u8> m = good;
            put32(m, cmapOff + 4 + 4, 0x00FFFFFF);
            expectReject(m, "cmap subtable offset outside cmap");
            m = good;
            put16(m, cmapOff + 2, 500);
            expectReject(m, "cmap record count past the table");
        }
        // bad magic, table counts
        {
            std::vector<u8> m = good;
            m[0] = 0x12;
            expectReject(m, "bad magic");
            m = good;
            put16(m, 4, 0);
            expectReject(m, "zero tables");
            m = good;
            put16(m, 4, 65);
            expectReject(m, "65 tables");
        }
        // a collection index out of range, and an index on a plain font
        {
            std::vector<testfont::Spec> two{twoLetters(), testfont::simpleSpec()};
            const std::vector<u8> ttc = testfont::buildCollection(two);
            check(uiValidateSfnt(ttc.data(), ttc.size(), 0) && uiValidateSfnt(ttc.data(), ttc.size(), 1), "a good collection validates for both members");
            expectReject(ttc, "ttcf index 2 of 2", 2);
            expectReject(ttc, "ttcf index -1", -1);
            expectReject(good, "index 1 on a plain font", 1);
            std::vector<u8> m = ttc;
            put32(m, 12, static_cast<u32>(ttc.size()) + 4);
            expectReject(m, "ttcf member offset past the end", 0);
        }
        check(mutations >= 40, "at least 40 targeted mutations were run (" + std::to_string(mutations) + ")");
        check(rejected == mutations, "and every one was rejected (" + std::to_string(rejected) + "/" + std::to_string(mutations) + ")");

        std::string why;
        const std::vector<u8> tiny(8, 0);
        check(!uiValidateSfnt(tiny.data(), tiny.size(), 0, &why) && !why.empty(), "a failure carries a reason");
        check(!uiValidateSfnt(nullptr, 0, 0), "null data is refused");
    }

    AVER_INFO("=== face metrics, cmap, advances ===");
    {
        const testfont::Spec spec = testfont::simpleSpec();
        UiFontFace face;
        std::string err;
        check(face.open(blobOf(testfont::buildFont(spec)), 0, &err), "the face opens: " + err);
        check(face.valid() && face.unitsPerEm() == 1000, "units per em");
        check(face.glyphCount() == spec.chars.size() + 1, "glyph count is the characters plus .notdef");
        check(face.glyphFor('A') == testfont::glyphId(spec, 'A') && face.glyphFor('a') == testfont::glyphId(spec, 'a'),
              "cmap lookups give the builder's glyph ids");
        check(face.glyphFor(0x4E00) == 0, "an absent code point is glyph 0");
        check(face.glyphFor(0x110000) == 0, "and so is a value over U+10FFFF");
        check(approx(face.advance(face.glyphFor('A'), 16.0f), 600 * 16.0f / 1000.0f), "advance of A at 16 px");
        check(approx(face.advance(face.glyphFor('B'), 100.0f), 610 * 100.0f / 1000.0f), "advance of B at 100 px");
        check(approx(face.advance(face.glyphFor(' '), 10.0f), 3.0f), "a space's advance");
        check(face.isBlank(face.glyphFor(' ')) && !face.isBlank(face.glyphFor('A')), "a space is blank, A is not");
        check(approx(face.ascent(1000.0f), 800.0f) && approx(face.descent(1000.0f), -200.0f) && approx(face.lineGap(1000.0f), 0.0f),
              "ascent, descent, line gap from hhea");
        check(approx(face.ascent(20.0f), 16.0f), "and they scale with the size");
        i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        check(face.glyphBox(face.glyphFor('A'), x0, y0, x1, y1) && x0 == 50 && y0 == 0 && x1 == 550 && y1 == 700, "glyph box in font units");
        check(!face.glyphBox(face.glyphFor(' '), x0, y0, x1, y1), "a blank glyph has no box");
        check(face.kernSource() == UiKernSource::None, "no kerning table");
        check(face.kern(face.glyphFor('A'), face.glyphFor('V'), 16.0f) == 0.0f, "so kern is zero");

        testfont::Spec big = spec;
        big.format12 = true;
        big.chars[0x1F600] = {1000, 0, 0, 900, 900};
        UiFontFace f2;
        check(f2.open(blobOf(testfont::buildFont(big)), 0) && f2.glyphFor(0x1F600) == testfont::glyphId(big, 0x1F600) &&
                  f2.glyphFor(0x1F600) != 0,
              "a format 12 cmap reaches above the BMP");

        testfont::Spec tall = spec;
        tall.ascent = 0;
        tall.descent = 0;
        UiFontFace f3;
        check(f3.open(blobOf(testfont::buildFont(tall)), 0) && f3.ascent(1000.0f) == 0.0f, "no OS/2 fallback invented when the font has none");

        UiFontFace bad;
        const std::vector<u8> junk(100, 7);
        check(!bad.open(blobOf(junk), 0) && !bad.valid(), "a junk buffer does not open");
        UiFontFace moved = std::move(face);
        check(moved.valid() && !face.valid(), "a face moves");
    }

    AVER_INFO("=== kerning ===");
    {
        testfont::Spec s = twoLetters();
        s.kern = {{'A', 'V', -80}};
        UiFontFace kf;
        check(kf.open(blobOf(testfont::buildFont(s)), 0) && kf.kernSource() == UiKernSource::Kern, "a font with a kern table");
        const u32 gA = kf.glyphFor('A'), gV = kf.glyphFor('V');
        check(approx(kf.kern(gA, gV, 1000.0f), -80.0f), "kern A,V from the format 0 table");
        check(kf.kern(gV, gA, 1000.0f) == 0.0f, "V,A is not in the table");
        check(approx(kf.kern(gA, gV, 50.0f), -4.0f), "and scales with the size");
        check(approx(kf.kern(gA, gV, 1000.0f), -80.0f), "a second lookup (memoised) agrees");

        testfont::Spec g1 = twoLetters();
        g1.gposPairs = {{'A', 'V', -60}, {'V', 'A', -30}};
        UiFontFace gf1;
        check(gf1.open(blobOf(testfont::buildFont(g1)), 0) && gf1.kernSource() == UiKernSource::Gpos, "a font with GPOS PairPos format 1");
        check(approx(gf1.kern(gf1.glyphFor('A'), gf1.glyphFor('V'), 1000.0f), -60.0f) &&
                  approx(gf1.kern(gf1.glyphFor('V'), gf1.glyphFor('A'), 1000.0f), -30.0f),
              "kern from format 1 pair sets");
        check(gf1.kern(gf1.glyphFor('A'), gf1.glyphFor('A'), 1000.0f) == 0.0f, "a pair not listed is zero");

        testfont::Spec g2 = twoLetters();
        g2.gposClasses = {{{'A'}, {'V', ' '}, -45}};
        UiFontFace gf2;
        check(gf2.open(blobOf(testfont::buildFont(g2)), 0) && gf2.kernSource() == UiKernSource::Gpos, "a font with GPOS PairPos format 2");
        check(approx(gf2.kern(gf2.glyphFor('A'), gf2.glyphFor('V'), 1000.0f), -45.0f) &&
                  approx(gf2.kern(gf2.glyphFor('A'), gf2.glyphFor(' '), 1000.0f), -45.0f),
              "kern from format 2 class pairs");
        check(gf2.kern(gf2.glyphFor('V'), gf2.glyphFor('A'), 1000.0f) == 0.0f, "the reverse order is class 0");

        testfont::Spec both = twoLetters();
        both.kern = {{'A', 'V', -80}};
        both.gposPairs = {{'A', 'V', -20}};
        UiFontFace bf;
        check(bf.open(blobOf(testfont::buildFont(both)), 0), "a font with both");
        check(approx(bf.kern(bf.glyphFor('A'), bf.glyphFor('V'), 1000.0f), -20.0f), "GPOS wins over kern (stb reads one source)");
    }

    AVER_INFO("=== collections ===");
    {
        testfont::Spec a = twoLetters();
        testfont::Spec b = testfont::simpleSpec();
        const std::vector<u8> ttc = testfont::buildCollection({a, b});
        UiFontFace f0, f1;
        check(f0.open(blobOf(ttc), 0) && f1.open(blobOf(ttc), 1), "both members open");
        check(f0.glyphCount() == a.chars.size() + 1 && f1.glyphCount() == b.chars.size() + 1, "each has its own glyphs");
        check(f0.glyphFor('z') == 0 && f1.glyphFor('z') != 0, "member 1 has z, member 0 does not");
        UiFontFace f2;
        check(!f2.open(blobOf(ttc), 2), "an index past the collection does not open");
    }

    AVER_INFO("=== rasterisation ===");
    {
        testfont::Spec s;
        s.unitsPerEm = 1024;
        s.chars['H'] = {1024, 64, 0, 512, 640};   // 8..64 x 0..80 at 128 px: whole pixels
        s.chars[' '] = {512, 0, 0, 0, 0};
        UiFontFace f;
        check(f.open(blobOf(testfont::buildFont(s)), 0), "a 1024 unit font");
        UiGlyphBitmap bmp;
        check(f.rasterize(f.glyphFor('H'), 128, bmp), "H rasterises at 128 px");
        check(bmp.w == 58 && bmp.h == 82, "56 x 80 plus a 1 px gutter on each side");
        check(bmp.offX == 8 - 1 && bmp.offY == -80 - 1, "placed from the pen with the gutter included");
        check(bmp.pixels.size() == static_cast<usize>(bmp.w) * bmp.h, "A8, one byte per pixel");
        bool gutterClear = true, interiorFull = true;
        for (int y = 0; y < bmp.h; ++y)
            for (int x = 0; x < bmp.w; ++x) {
                const u8 v = bmp.pixels[static_cast<usize>(y) * bmp.w + x];
                const bool edge = x == 0 || y == 0 || x == bmp.w - 1 || y == bmp.h - 1;
                if (edge && v != 0) gutterClear = false;
                if (!edge && v < 253) interiorFull = false;
            }
        check(gutterClear, "the gutter is transparent");
        check(interiorFull, "the rectangle's coverage is full inside");

        check(f.rasterize(f.glyphFor(' '), 128, bmp) && bmp.w == 0 && bmp.h == 0 && bmp.pixels.empty(), "a space rasterises to nothing, successfully");
        check(!f.rasterize(f.glyphFor('H'), 0, bmp), "a zero size is refused");

        // half-pixel edges give partial coverage
        testfont::Spec h;
        h.unitsPerEm = 1024;
        h.chars['I'] = {1024, 64, 0, 576, 640};   // x 64..576 is 8..72 at 128 px; at 64 px that is 4..36
        UiFontFace hf;
        check(hf.open(blobOf(testfont::buildFont(h)), 0), "a second font");
        check(hf.rasterize(hf.glyphFor('I'), 64, bmp), "rasterised at half size");
        check(bmp.w == 34 && bmp.h == 42, "32 x 40 plus the gutter");
    }

    AVER_INFO("=== .ocfonts parse / write ===");
    {
        UiFontSetDoc doc;
        doc.defaultFamily = "Body";
        UiFontFamilyDef body;
        body.name = "Body";
        UiFontEntry e1;
        e1.file = "Fonts/Inter-Regular.ttf";
        UiFontEntry e2 = e1;
        e2.file = "Fonts/Inter-Bold.ttf";
        e2.bold = true;
        UiFontEntry e3 = e1;
        e3.file = "Fonts/Inter Italic.ttf";   // a space: needs quoting
        e3.italic = true;
        UiFontEntry e4 = e1;
        e4.file = "Fonts/Mono.ttc";
        e4.index = 1;
        e4.scale = 0.95f;
        e4.dy = 0.5f;
        e4.lang = "ja";
        UiFontEntry sys;
        sys.source = UiFontEntry::Source::System;
        sys.file = "auto";
        UiFontEntry sys2;
        sys2.source = UiFontEntry::Source::System;
        sys2.file = "msyh.ttc";
        sys2.lang = "zh-Hans";
        UiFontEntry bmp;
        bmp.source = UiFontEntry::Source::Bitmap;
        bmp.file = "legacy";
        body.entries = {e1, e2, e3, e4, sys, sys2, bmp};
        body.lineGap = 1.5f;
        UiFontFamilyDef head;
        head.name = "Heading";
        head.entries = {e1};
        doc.families = {body, head};
        doc.overrides = {{"ja", "Body", "BodyJa"}};

        const std::string text = UiFontSet::write(doc);
        UiFontSetDoc back;
        std::string err;
        check(UiFontSet::parse(text, back, &err), "the written document parses: " + err);
        check(back.families.size() == 2 && back.families[0].entries.size() == 7 && back.defaultFamily == "Body", "structure survives");
        check(back.families[0].entries[2].file == "Fonts/Inter Italic.ttf" && back.families[0].entries[2].italic, "a quoted path and the italic flag");
        check(back.families[0].entries[3].index == 1 && approx(back.families[0].entries[3].scale, 0.95f) && approx(back.families[0].entries[3].dy, 0.5f) &&
                  back.families[0].entries[3].lang == "ja",
              "index, scale, dy, lang");
        check(back.families[0].entries[1].bold && back.families[0].entries[4].source == UiFontEntry::Source::System &&
                  back.families[0].entries[6].source == UiFontEntry::Source::Bitmap,
              "bold, SYSTEM, BITMAP");
        check(approx(back.families[0].lineGap, 1.5f) && back.overrides.size() == 1 && back.overrides[0].to == "BodyJa", "LINEGAP and OVERRIDE");
        check(UiFontSet::write(back) == text, "write(parse(write(x))) == write(x)");

        // comments, unknown keys, CRLF
        UiFontSetDoc c;
        const std::string withNoise = "# a comment\r\nOCFONTS 1\r\nFUTUREKEY 1 2 3\r\nFAMILY A   # trailing\r\nFACE Fonts/x.ttf futureattr=1\r\n";
        check(UiFontSet::parse(withNoise, c, &err) && c.families.size() == 1 && c.families[0].entries.size() == 1,
              "comments, CRLF, an unknown key and an unknown attribute are skipped");

        // errors
        UiFontSetDoc bad;
        check(!UiFontSet::parse("FAMILY A\n", bad, &err) && err.find("OCFONTS") != std::string::npos, "no header is an error");
        check(!UiFontSet::parse("OCFONTS 1\nFACE x.ttf\n", bad, &err) && err.find("line 2") != std::string::npos, "FACE before FAMILY names line 2");
        check(!UiFontSet::parse("OCFONTS 1\nFAMILY A\nFACE x.ttf scale=abc\n", bad, &err) && err.find("line 3") != std::string::npos &&
                  err.find("scale") != std::string::npos,
              "a bad scale names line 3");
        check(!UiFontSet::parse("OCFONTS 1\nFAMILY A\nFACE x.ttf weight=heavy\n", bad, &err) && err.find("weight") != std::string::npos, "a bad weight");
        check(!UiFontSet::parse("OCFONTS 1\nFAMILY A\nFACE x.ttf index=-1\n", bad, &err), "a negative index");
        check(!UiFontSet::parse("OCFONTS 1\nFAMILY A\nLINEGAP nope\n", bad, &err) && err.find("LINEGAP") != std::string::npos, "a bad LINEGAP");
        check(!UiFontSet::parse("OCFONTS 1\nFAMILY \"A\n", bad, &err) && err.find("quote") != std::string::npos, "an unterminated quote");
        check(!UiFontSet::parse("OCFONTS 1\nOVERRIDE ja Body\n", bad, &err), "an OVERRIDE with too few fields");
        UiFontSetDoc newer;
        check(UiFontSet::parse("OCFONTS 7\nFAMILY A\n", newer, &err) && newer.version == 7 && !newer.note.empty(), "a newer version parses with a note");

        UiFontSetDoc def;
        check(UiFontSet::parse(uiDefaultFontSetText(), def, &err) && def.families.size() == 1 && def.families[0].entries.size() == 4,
              "the built-in default parses: Roboto regular and medium, SYSTEM auto, BITMAP legacy");
    }

    AVER_INFO("=== the fallback chain ===");
    {
        // Faces: A has a,b; Abold has a; sys has b,c,x; jp and zh both have U+4E00.
        const auto mk = [](std::initializer_list<u32> cps, int adv) {
            testfont::Spec s;
            for (const u32 c : cps) s.chars[c] = {adv, 50, 0, 500, 700};
            return testfont::buildFont(s);
        };
        std::map<std::string, std::vector<u8>> files;
        files["Fonts/A.ttf"] = mk({'a', 'b'}, 600);
        files["Fonts/Abold.ttf"] = mk({'a'}, 650);
        files["b.ttf"] = mk({'b', 'c', 'x'}, 700);
        files["jp.ttf"] = mk({0x4E00}, 800);
        files["zh.ttf"] = mk({0x4E00}, 900);
        files["Fonts/B.ttf"] = mk({'q'}, 500);
        files["exe.ttf"] = mk({'e'}, 500);
        std::map<std::string, int> opens;

        UiFontLoader loader;
        loader.open = [&](UiFontKind kind, const std::string& name, UiFontBlob& out) {
            const auto it = files.find(name);
            ++opens[name];
            if (it == files.end()) return false;
            (void)kind;
            out = blobOf(it->second);
            return true;
        };

        const char* text =
            "OCFONTS 1\n"
            "DEFAULT Body\n"
            "FAMILY Body\n"
            "FACE Fonts/A.ttf\n"
            "FACE Fonts/Abold.ttf weight=bold\n"
            "FACE @exe/exe.ttf\n"
            "SYSTEM b.ttf\n"
            "SYSTEM jp.ttf lang=ja\n"
            "SYSTEM zh.ttf lang=zh-Hans\n"
            "SYSTEM missing.ttf\n"
            "BITMAP legacy\n"
            "FAMILY Other\n"
            "FACE Fonts/B.ttf\n"
            "OVERRIDE ja Body Other\n";
        UiFontSetDoc doc;
        std::string err;
        check(UiFontSet::parse(text, doc, &err), "the test set parses: " + err);

        UiFontSet set;
        check(set.load(doc, loader, &err), "it loads");
        check(set.familyCount() == 2 && set.familyName(0) == "Body" && set.familyName(1) == "Other", "two families");
        check(set.hasTrueType() && set.openFaceCount() == 4, "the four project faces opened at load (A, Abold, exe, B), OS faces did not");
        check(opens["b.ttf"] == 0 && opens["jp.ttf"] == 0, "OS faces are not touched until a code point needs them");

        const i32 body = set.familyIndex("Body");
        check(body == 0 && set.familyIndex("body") == 0 && set.familyIndex("BODY") == 0, "family names are case-insensitive");
        check(set.familyIndex("NoSuchFamily") == 0 && set.familyIndex("") == 0, "an unknown or empty name is the default");

        const UiFontSet::Resolved ra = set.resolve(body, false, false, 'a');
        const UiFontSet::Resolved rab = set.resolve(body, true, false, 'a');
        check(ra.face != kUiFaceNone && rab.face != kUiFaceNone && ra.face != rab.face, "regular and bold 'a' come from different faces");
        check(set.faceName(ra.face) == "Fonts/A.ttf" && set.faceName(rab.face) == "Fonts/Abold.ttf", "A and Abold");
        check(set.faceEntry(rab.face).bold && !set.faceEntry(ra.face).bold, "with the right flags");
        const UiFontSet::Resolved rb = set.resolve(body, false, false, 'b');
        check(set.faceName(rb.face) == "Fonts/A.ttf", "'b' is in the first project face, not the OS one");
        check(opens["b.ttf"] == 0, "and no OS face was opened for it");
        const UiFontSet::Resolved rc = set.resolve(body, false, false, 'c');
        check(rc.face != kUiFaceNone && set.faceName(rc.face) == "b.ttf" && opens["b.ttf"] == 1, "'c' falls through to the OS face, opened once");
        set.resolve(body, false, false, 'c');
        set.resolve(body, false, false, 'x');
        check(opens["b.ttf"] == 1, "which stays open");
        const UiFontSet::Resolved rbold = set.resolve(body, true, false, 'b');
        check(set.faceName(rbold.face) == "Fonts/A.ttf", "a bold request falls to the regular face when the bold one lacks the glyph");
        const UiFontSet::Resolved re = set.resolve(body, false, false, 'e');
        check(set.faceName(re.face) == "exe.ttf", "an @exe/ face is found through the Engine loader");

        const UiFontSet::Resolved miss1 = set.resolve(body, false, false, 0x1F600);
        const UiFontSet::Resolved miss2 = set.resolve(body, false, false, 0x1F600);
        check(miss1.face == kUiFaceNone && miss2.face == kUiFaceNone, "a code point no face has resolves to none, every time");
        check(opens["missing.ttf"] == 1, "a failing OS face is tried once and then remembered as failed");

        // Han unification: the language picks the face
        set.setLanguage("");
        const UiFontSet::Resolved han0 = set.resolve(body, false, false, 0x4E00);
        check(set.faceName(han0.face) == "jp.ttf", "no language: authored order, jp first");
        const u32 epochNeutral = set.epoch();
        set.setLanguage("zh-CN");
        check(set.epoch() != epochNeutral, "a language change moves the epoch");
        const UiFontSet::Resolved hanZh = set.resolve(body, false, false, 0x4E00);
        check(set.faceName(hanZh.face) == "zh.ttf", "zh-CN prefers the Simplified face (zh-Hans)");
        set.setLanguage("ja-JP");
        const UiFontSet::Resolved hanJa = set.resolve(body, false, false, 0x4E00);
        check(set.faceName(hanJa.face) == "jp.ttf", "ja-JP prefers the Japanese face");
        const u32 epochJa = set.epoch();
        set.setLanguage("ja-JP");
        check(set.epoch() == epochJa, "setting the same language again changes nothing");

        // OVERRIDE ja Body -> Other
        check(set.familyIndex("Body") == 1, "with the language ja, OVERRIDE maps Body to Other");
        set.setLanguage("en-US");
        check(set.familyIndex("Body") == 0, "and with en-US it does not");

        check(uiLangMatches("zh-CN", "zh-Hans") && uiLangMatches("zh-TW", "zh-Hant") && !uiLangMatches("zh-TW", "zh-Hans"), "zh regions read as scripts");
        check(uiLangMatches("ja-JP", "ja") && uiLangMatches("JA", "ja") && !uiLangMatches("jam", "ja") && !uiLangMatches("", "ja"), "BCP-47 prefix matching");

        // the bitmap font as the last link
        UiFont bitmapFont;
        bitmapFont.pixelSize = 16;
        bitmapFont.lineHeight = 19;
        UiGlyph glyph;
        glyph.advance = 7;
        bitmapFont.glyphs['z'] = glyph;
        set.setBitmapFont(&bitmapFont);
        const UiFontSet::Resolved rz = set.resolve(body, false, false, 'z');
        check(rz.face == kUiFaceBitmap && rz.glyph == 'z', "BITMAP legacy answers with the lent font");
        set.setBitmapFont(nullptr);
        check(set.resolve(body, false, false, 'z').face == kUiFaceNone, "and stops when the font is withdrawn");

        // primary face
        check(set.primaryFace(body, false, false) != kUiFaceNone && set.faceName(set.primaryFace(body, false, false)) == "Fonts/A.ttf", "the primary face of Body");
        check(set.faceName(set.primaryFace(body, true, false)) == "Fonts/Abold.ttf", "and of bold Body");

        // reload replaces everything
        UiFontSetDoc one;
        one.families.push_back(UiFontFamilyDef{"Solo", {UiFontEntry{}}, 1.0f});
        one.families[0].entries[0].file = "Fonts/B.ttf";
        check(set.load(one, loader, &err) && set.familyCount() == 1 && set.resolve(0, false, false, 'q').face != kUiFaceNone &&
                  set.resolve(0, false, false, 'a').face == kUiFaceNone,
              "a reload replaces the set and drops the memo");
        UiFontSetDoc empty;
        check(!set.load(empty, loader, &err) && !err.empty() && !set.hasTrueType(), "a document with no family fails and empties the set");

        // no face opens at all: only the bitmap remains
        UiFontSet none;
        UiFontSetDoc ghost;
        UiFontFamilyDef gf;
        gf.name = "G";
        UiFontEntry ge;
        ge.file = "nope.ttf";
        gf.entries = {ge};
        ghost.families = {gf};
        check(none.load(ghost, loader, &err) && !none.hasTrueType() && !none.warnings().empty(), "a set whose faces all fail to open reports no TrueType");
    }

    if (g_failures == 0) AVER_INFO("=== all font tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
