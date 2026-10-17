// uiValidateSfnt: structural checks on a font buffer before stb_truetype trusts its offsets.
// The face itself (stb) lives in UiTrueType.cpp, the only translation unit that includes stb.
#include "aver/ui/UiFontFace.hpp"

#include <algorithm>

namespace aver::ui {
namespace {

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }
u32 be32(const u8* p) { return (be16(p) << 16) | be16(p + 2); }

constexpr u32 tag4(char a, char b, char c, char d) {
    return (static_cast<u32>(static_cast<u8>(a)) << 24) | (static_cast<u32>(static_cast<u8>(b)) << 16) |
           (static_cast<u32>(static_cast<u8>(c)) << 8) | static_cast<u32>(static_cast<u8>(d));
}

struct Table {
    u32 tag = 0;
    u32 off = 0, len = 0;
};

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

const Table* find(const std::vector<Table>& t, u32 tag) {
    for (const Table& x : t) if (x.tag == tag) return &x;
    return nullptr;
}

} // namespace

bool uiValidateSfnt(const u8* data, usize size, i32 index, std::string* err) {
    if (!data || size < 12) return fail(err, "font file too small");

    usize dir = 0;
    const u32 magic = be32(data);
    if (magic == tag4('t', 't', 'c', 'f')) {
        const u32 n = be32(data + 8);
        if (n == 0 || n > 256) return fail(err, "bad TrueType Collection member count");
        if (index < 0 || static_cast<u32>(index) >= n) return fail(err, "collection index out of range");
        if (12 + 4ull * n > size) return fail(err, "collection header runs past the file");
        dir = be32(data + 12 + 4 * static_cast<usize>(index));
        if (dir > size || size - dir < 12) return fail(err, "collection member offset past the file");
    } else {
        if (index != 0) return fail(err, "a plain font has only index 0");
        if (magic != 0x00010000u && magic != tag4('O', 'T', 'T', 'O') && magic != tag4('t', 'r', 'u', 'e'))
            return fail(err, "not a TrueType or OpenType font");
    }

    const u32 numTables = be16(data + dir + 4);
    if (numTables == 0 || numTables > 64) return fail(err, "bad table count");
    if (dir + 12 + 16ull * numTables > size) return fail(err, "table directory runs past the file");

    std::vector<Table> tables;
    tables.reserve(numTables);
    for (u32 i = 0; i < numTables; ++i) {
        const u8* rec = data + dir + 12 + 16 * static_cast<usize>(i);
        Table t;
        t.tag = be32(rec);
        t.off = be32(rec + 8);
        t.len = be32(rec + 12);
        if (t.off > size || t.len > size - t.off) return fail(err, "a table runs past the end of the file");
        tables.push_back(t);
    }
    {
        std::vector<Table> byOff;
        for (const Table& t : tables) if (t.len > 0) byOff.push_back(t);
        std::sort(byOff.begin(), byOff.end(), [](const Table& a, const Table& b) { return a.off < b.off; });
        for (usize i = 1; i < byOff.size(); ++i)
            if (static_cast<u64>(byOff[i - 1].off) + byOff[i - 1].len > byOff[i].off) return fail(err, "tables overlap");
    }

    const Table* cmap = find(tables, tag4('c', 'm', 'a', 'p'));
    const Table* head = find(tables, tag4('h', 'e', 'a', 'd'));
    const Table* hhea = find(tables, tag4('h', 'h', 'e', 'a'));
    const Table* hmtx = find(tables, tag4('h', 'm', 't', 'x'));
    const Table* maxp = find(tables, tag4('m', 'a', 'x', 'p'));
    const Table* glyf = find(tables, tag4('g', 'l', 'y', 'f'));
    const Table* loca = find(tables, tag4('l', 'o', 'c', 'a'));
    const Table* cff = find(tables, tag4('C', 'F', 'F', ' '));
    if (!cmap || !head || !hhea || !hmtx || !maxp) return fail(err, "a required table is missing");
    if (!(glyf && loca) && !cff) return fail(err, "no outlines (glyf+loca or CFF)");

    if (head->len < 54) return fail(err, "head table too short");
    const u32 upem = be16(data + head->off + 18);
    if (upem < 16 || upem > 16384) return fail(err, "unitsPerEm out of range");
    const u32 locFormat = be16(data + head->off + 50);

    if (maxp->len < 6) return fail(err, "maxp table too short");
    const u32 numGlyphs = be16(data + maxp->off + 4);
    if (numGlyphs == 0) return fail(err, "font has no glyphs");

    if (hhea->len < 36) return fail(err, "hhea table too short");
    const u32 numHMetrics = be16(data + hhea->off + 34);
    if (numHMetrics == 0 || numHMetrics > numGlyphs) return fail(err, "numberOfHMetrics inconsistent with numGlyphs");
    if (hmtx->len < 4ull * numHMetrics + 2ull * (numGlyphs - numHMetrics)) return fail(err, "hmtx table too short");

    if (glyf && loca) {
        if (locFormat > 1) return fail(err, "bad indexToLocFormat");
        const u64 need = (static_cast<u64>(numGlyphs) + 1) * (locFormat ? 4 : 2);
        if (loca->len < need) return fail(err, "loca table too short for numGlyphs");
    }

    if (cmap->len < 4) return fail(err, "cmap table too short");
    const u32 subCount = be16(data + cmap->off + 2);
    if (4ull + 8ull * subCount > cmap->len) return fail(err, "cmap subtable records run past the table");
    for (u32 i = 0; i < subCount; ++i) {
        const u8* rec = data + cmap->off + 4 + 8 * static_cast<usize>(i);
        const u32 sub = be32(rec + 4);
        if (sub > cmap->len || cmap->len - sub < 4) return fail(err, "cmap subtable offset outside cmap");
        const u8* st = data + cmap->off + sub;
        const u32 fmt = be16(st);
        // Checked against what is left of cmap, not the declared length: some shipped fonts write a
        // bogus 16-bit length in format 4, and rejecting them would drop a usable system font.
        const u64 room = cmap->len - sub;
        if (fmt == 4) {
            if (room < 16) return fail(err, "cmap format 4 header outside cmap");
            if (14ull + 4ull * be16(st + 6) > room) return fail(err, "cmap format 4 segments run past cmap");
        } else if (fmt == 6) {
            if (room < 10) return fail(err, "cmap format 6 header outside cmap");
            if (10ull + 2ull * be16(st + 8) > room) return fail(err, "cmap format 6 entries run past cmap");
        } else if (fmt == 0) {
            if (room < 262) return fail(err, "cmap format 0 table outside cmap");
        } else if (fmt == 12 || fmt == 13) {
            if (room < 16) return fail(err, "cmap format 12 header outside cmap");
            if (16ull + 12ull * be32(st + 12) > room) return fail(err, "cmap format 12 groups run past cmap");
        }
    }
    return true;
}

} // namespace aver::ui
