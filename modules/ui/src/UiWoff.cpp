// UiWoff: WOFF 1.0 to sfnt. See UiWoff.hpp. The format is the W3C WOFF File Format 1.0 Recommendation.
#include "aver/ui/UiWoff.hpp"

#include <algorithm>

namespace aver::ui {
namespace {

UiInflateFn g_inflate = nullptr;

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }
u32 be32(const u8* p) { return (be16(p) << 16) | be16(p + 2); }

void put16(std::vector<u8>& v, usize at, u32 x) { v[at] = static_cast<u8>(x >> 8); v[at + 1] = static_cast<u8>(x); }
void put32(std::vector<u8>& v, usize at, u32 x) { put16(v, at, x >> 16); put16(v, at + 2, x & 0xFFFF); }

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

constexpr u32 kWoffMagic = 0x774F4646u;    // 'wOFF'
constexpr u32 kWoff2Magic = 0x774F4632u;   // 'wOF2'
constexpr usize kHeader = 44, kEntry = 20;

struct Entry {
    u32 tag, offset, compLength, origLength, checksum;
};

} // namespace

void uiSetInflate(UiInflateFn fn) { g_inflate = fn; }
UiInflateFn uiInflate() { return g_inflate; }

bool uiIsWoff(const u8* data, usize size) { return data && size >= 4 && be32(data) == kWoffMagic; }
bool uiIsWoff2(const u8* data, usize size) { return data && size >= 4 && be32(data) == kWoff2Magic; }

bool uiWoffToSfnt(const u8* data, usize size, std::vector<u8>& out, std::string* err, UiInflateFn inflate) {
    if (!inflate) inflate = g_inflate;
    if (!uiIsWoff(data, size)) return fail(err, "not a WOFF file");
    if (size < kHeader) return fail(err, "WOFF header runs past the file");
    const u32 flavor = be32(data + 4);
    const u32 length = be32(data + 8);
    const u32 numTables = be16(data + 12);
    const u32 reserved = be16(data + 14);
    const u32 totalSfnt = be32(data + 16);
    if (length != size) return fail(err, "WOFF length does not match the file size");
    if (reserved != 0) return fail(err, "WOFF reserved field is not zero");
    if (numTables == 0 || numTables > kUiWoffMaxTables) return fail(err, "bad WOFF table count");
    if (totalSfnt > kUiWoffMaxSfntBytes) return fail(err, "WOFF would rebuild to more than the font size cap");
    if (kHeader + kEntry * static_cast<usize>(numTables) > size) return fail(err, "WOFF table directory runs past the file");

    std::vector<Entry> tables;
    tables.reserve(numTables);
    u64 sfntSize = 12 + 16ull * numTables;
    for (u32 i = 0; i < numTables; ++i) {
        const u8* e = data + kHeader + kEntry * static_cast<usize>(i);
        Entry t{be32(e), be32(e + 4), be32(e + 8), be32(e + 12), be32(e + 16)};
        if (i > 0 && t.tag <= tables.back().tag) return fail(err, "WOFF tables are not in ascending tag order");
        if (t.offset % 4 != 0) return fail(err, "a WOFF table is not four-byte aligned");
        if (static_cast<u64>(t.offset) + t.compLength > size) return fail(err, "a WOFF table runs past the file");
        if (t.offset < kHeader + kEntry * static_cast<u64>(numTables)) return fail(err, "a WOFF table overlaps the table directory");
        if (t.compLength > t.origLength) return fail(err, "a WOFF table is larger compressed than it is");
        if (t.origLength > kUiWoffMaxSfntBytes) return fail(err, "a WOFF table is larger than the font size cap");
        sfntSize += (static_cast<u64>(t.origLength) + 3) & ~3ull;
        if (sfntSize > kUiWoffMaxSfntBytes) return fail(err, "WOFF would rebuild to more than the font size cap");
        tables.push_back(t);
    }
    if (sfntSize != totalSfnt) return fail(err, "WOFF totalSfntSize does not match its tables");
    {   // no two tables share bytes
        std::vector<const Entry*> byOff;
        for (const Entry& t : tables) if (t.compLength > 0) byOff.push_back(&t);
        std::sort(byOff.begin(), byOff.end(), [](const Entry* a, const Entry* b) { return a->offset < b->offset; });
        for (usize i = 1; i < byOff.size(); ++i)
            if (static_cast<u64>(byOff[i - 1]->offset) + byOff[i - 1]->compLength > byOff[i]->offset) return fail(err, "WOFF tables overlap");
    }

    std::vector<u8> sfnt(static_cast<usize>(sfntSize), 0);
    u32 pow2 = 1, sel = 0;
    while (pow2 * 2 <= numTables) { pow2 *= 2; ++sel; }
    put32(sfnt, 0, flavor);
    put16(sfnt, 4, numTables);
    put16(sfnt, 6, pow2 * 16);
    put16(sfnt, 8, sel);
    put16(sfnt, 10, numTables * 16 - pow2 * 16);

    usize at = 12 + 16 * static_cast<usize>(numTables);
    for (u32 i = 0; i < numTables; ++i) {
        const Entry& t = tables[i];
        const usize rec = 12 + 16 * static_cast<usize>(i);
        put32(sfnt, rec, t.tag);
        put32(sfnt, rec + 4, t.checksum);
        put32(sfnt, rec + 8, static_cast<u32>(at));
        put32(sfnt, rec + 12, t.origLength);
        const u8* src = data + t.offset;
        if (t.compLength == t.origLength) {
            std::copy(src, src + t.origLength, sfnt.begin() + static_cast<std::ptrdiff_t>(at));
        } else {
            // zlib: a two-byte header (deflate, no preset dictionary, a valid check), the deflate stream, an adler32 we do not need.
            if (t.compLength < 2 + 4) return fail(err, "a compressed WOFF table is too short to be zlib");
            const u32 cmf = src[0], flg = src[1];
            if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return fail(err, "a compressed WOFF table is not a zlib stream");
            if (!inflate) return fail(err, "a compressed WOFF table needs the host's inflate, and none was installed");
            if (!inflate(src + 2, t.compLength - 2, sfnt.data() + at, t.origLength)) return fail(err, "a WOFF table did not inflate to its declared size");
        }
        at += (static_cast<usize>(t.origLength) + 3) & ~static_cast<usize>(3);
    }
    out = std::move(sfnt);
    return true;
}

} // namespace aver::ui
