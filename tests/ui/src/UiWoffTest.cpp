// Parity P37: WOFF 1.0 web fonts. A WOFF is built here from a synthetic TrueType font (one table stored, one run-length compressed
// with a small fixed-Huffman deflate encoder written for the test), then read back through uiWoffToSfnt and UiFontFace::open,
// and a set of malformed files must each be refused cleanly. The inflate is Aver.Platform's, lent the way a host lends it.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/Inflate.hpp"
#include "aver/ui/UiFontFace.hpp"
#include "aver/ui/UiWoff.hpp"

#include <algorithm>
#include <cstring>
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

namespace {

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }
u32 be32(const u8* p) { return (be16(p) << 16) | be16(p + 2); }
void w16(std::vector<u8>& v, u32 x) { v.push_back(static_cast<u8>(x >> 8)); v.push_back(static_cast<u8>(x)); }
void w32(std::vector<u8>& v, u32 x) { w16(v, x >> 16); w16(v, x & 0xFFFF); }

bool inflateAdapter(const u8* in, usize inSize, u8* out, usize outSize) { return inflateRaw(in, inSize, out, outSize); }

// ---- a tiny deflate encoder: fixed Huffman, literals and runs of the previous byte (distance 1) ----
struct Bits {
    std::vector<u8> bytes;
    u32 acc = 0, n = 0;
    void put(u32 value, u32 count) {   // least significant bit first
        for (u32 i = 0; i < count; ++i) {
            acc |= ((value >> i) & 1u) << n;
            if (++n == 8) { bytes.push_back(static_cast<u8>(acc)); acc = 0; n = 0; }
        }
    }
    void code(u32 value, u32 count) {   // a Huffman code, most significant bit first
        for (u32 i = 0; i < count; ++i) put((value >> (count - 1 - i)) & 1u, 1);
    }
    void flush() { if (n) { bytes.push_back(static_cast<u8>(acc)); acc = 0; n = 0; } }
};

void litLen(Bits& b, u32 sym) {
    if (sym < 144) b.code(0x30 + sym, 8);
    else if (sym < 256) b.code(0x190 + (sym - 144), 9);
    else if (sym < 280) b.code(sym - 256, 7);
    else b.code(0xC0 + (sym - 280), 8);
}

void runOfPrevious(Bits& b, u32 length) {   // 3..258 bytes equal to the byte before, distance 1
    static const u16 base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const u8 extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    u32 s = 28;
    while (base[s] > length) --s;
    litLen(b, 257 + s);
    b.put(length - base[s], extra[s]);
    b.code(0, 5);   // distance code 0 = 1
}

// A zlib stream of `bytes`, which must start with a literal and then repeat it: first byte literal, the rest runs.
std::vector<u8> zlibOfRun(u8 value, u32 count) {
    Bits b;
    b.put(1, 1);    // final block
    b.put(1, 2);    // fixed Huffman
    litLen(b, value);
    u32 left = count - 1;
    while (left >= 3) { const u32 n = std::min<u32>(left, 258); runOfPrevious(b, n); left -= n; }
    while (left > 0) { litLen(b, value); --left; }
    litLen(b, 256);
    b.flush();
    std::vector<u8> z = {0x78, 0x01};
    z.insert(z.end(), b.bytes.begin(), b.bytes.end());
    u32 s1 = 1, s2 = 0;
    for (u32 i = 0; i < count; ++i) { s1 = (s1 + value) % 65521; s2 = (s2 + s1) % 65521; }
    w32(z, (s2 << 16) | s1);
    return z;
}

struct Table { u32 tag; std::vector<u8> data; };

std::vector<Table> tablesOf(const std::vector<u8>& sfnt) {
    std::vector<Table> t;
    const u32 n = be16(sfnt.data() + 4);
    for (u32 i = 0; i < n; ++i) {
        const u8* rec = sfnt.data() + 12 + 16 * i;
        const u32 off = be32(rec + 8), len = be32(rec + 12);
        t.push_back({be32(rec), std::vector<u8>(sfnt.begin() + off, sfnt.begin() + off + len)});
    }
    return t;
}

// The sfnt a WOFF of these tables must rebuild to (tag order, four-byte aligned).
std::vector<u8> sfntOf(u32 flavor, std::vector<Table> tables) {
    std::sort(tables.begin(), tables.end(), [](const Table& a, const Table& b) { return a.tag < b.tag; });
    std::vector<u8> out;
    const u32 n = static_cast<u32>(tables.size());
    u32 pow2 = 1, sel = 0;
    while (pow2 * 2 <= n) { pow2 *= 2; ++sel; }
    w32(out, flavor); w16(out, n); w16(out, pow2 * 16); w16(out, sel); w16(out, n * 16 - pow2 * 16);
    u32 at = 12 + 16 * n;
    for (const Table& t : tables) {
        w32(out, t.tag); w32(out, 0); w32(out, at); w32(out, static_cast<u32>(t.data.size()));
        at += (static_cast<u32>(t.data.size()) + 3) & ~3u;
    }
    for (const Table& t : tables) {
        out.insert(out.end(), t.data.begin(), t.data.end());
        while (out.size() % 4) out.push_back(0);
    }
    return out;
}

// A WOFF of these tables; `compress` picks the tables stored zlib-compressed (only a run table can be here).
std::vector<u8> woffOf(u32 flavor, std::vector<Table> tables, u32 compressTag, u8 runValue) {
    std::sort(tables.begin(), tables.end(), [](const Table& a, const Table& b) { return a.tag < b.tag; });
    const u32 n = static_cast<u32>(tables.size());
    std::vector<std::vector<u8>> stored;
    for (const Table& t : tables) stored.push_back(t.tag == compressTag ? zlibOfRun(runValue, static_cast<u32>(t.data.size())) : t.data);
    u32 total = 12 + 16 * n;
    for (const Table& t : tables) total += (static_cast<u32>(t.data.size()) + 3) & ~3u;
    std::vector<u8> out;
    w32(out, 0x774F4646u); w32(out, flavor); w32(out, 0); w16(out, n); w16(out, 0); w32(out, total);
    w16(out, 1); w16(out, 0); w32(out, 0); w32(out, 0); w32(out, 0); w32(out, 0); w32(out, 0);
    u32 at = 44 + 20 * n;
    for (u32 i = 0; i < n; ++i) {
        w32(out, tables[i].tag); w32(out, at); w32(out, static_cast<u32>(stored[i].size())); w32(out, static_cast<u32>(tables[i].data.size())); w32(out, 0);
        at += (static_cast<u32>(stored[i].size()) + 3) & ~3u;
    }
    for (const auto& s : stored) { out.insert(out.end(), s.begin(), s.end()); while (out.size() % 4) out.push_back(0); }
    const u32 len = static_cast<u32>(out.size());
    out[8] = static_cast<u8>(len >> 24); out[9] = static_cast<u8>(len >> 16); out[10] = static_cast<u8>(len >> 8); out[11] = static_cast<u8>(len);
    return out;
}

UiFontBlob blobOf(const std::vector<u8>& v) {
    auto keep = std::make_shared<std::vector<u8>>(v);
    UiFontBlob b;
    b.data = keep->data(); b.size = keep->size(); b.owner = keep;
    return b;
}

constexpr u32 kZero = 0x5A45524Fu;   // 'ZERO'

} // namespace

int main() {
    uiSetInflate(&inflateAdapter);
    testfont::Spec spec = testfont::simpleSpec();
    const std::vector<u8> ttf = testfont::buildFont(spec);
    std::vector<Table> tables = tablesOf(ttf);
    tables.push_back({kZero, std::vector<u8>(3001, 0x41)});   // an odd length, compressed to a few bytes
    const u32 flavor = be32(ttf.data());

    AVER_INFO("=== the container ===");
    {
        const std::vector<u8> woff = woffOf(flavor, tables, kZero, 0x41);
        check(uiIsWoff(woff.data(), woff.size()) && !uiIsWoff2(woff.data(), woff.size()), "the signature is recognised");
        check(!uiIsWoff(ttf.data(), ttf.size()), "and a TrueType file is not mistaken for it");
        check(woff.size() < ttf.size() + 3001, "the run table is stored compressed (the file is smaller than the sum of its tables)");
        std::vector<u8> out;
        std::string err;
        const bool ok = uiWoffToSfnt(woff.data(), woff.size(), out, &err);
        check(ok, "it rebuilds: " + err);
        const std::vector<u8> expect = sfntOf(flavor, tables);
        check(out == expect, "to exactly the sfnt of its tables (directory, alignment and table bytes)");

        UiFontBlob blob = blobOf(woff);
        UiFontFace face;
        std::string ferr;
        check(face.open(blob, 0, &ferr), "UiFontFace opens the WOFF directly: " + ferr);
        UiFontFace plain;
        check(plain.open(blobOf(ttf), 0), "and the TrueType it came from");
        check(face.valid() && face.glyphCount() == plain.glyphCount() && face.unitsPerEm() == plain.unitsPerEm(), "the same face");
        check(face.glyphFor('A') == plain.glyphFor('A') && face.advance(face.glyphFor('A'), 20.0f) == plain.advance(plain.glyphFor('A'), 20.0f),
              "with the same glyphs and advances");
        UiGlyphBitmap a, b;
        check(face.rasterize(face.glyphFor('B'), 20, a) && plain.rasterize(plain.glyphFor('B'), 20, b) && a.pixels == b.pixels && a.w > 0,
              "and the same pixels");

        // Every table stored uncompressed is a WOFF too.
        const std::vector<u8> raw = woffOf(flavor, tables, 0, 0);
        std::vector<u8> out2;
        check(uiWoffToSfnt(raw.data(), raw.size(), out2) && out2 == expect, "a WOFF with no compressed table needs no inflate and rebuilds the same");
    }

    AVER_INFO("=== without the host's inflate, and WOFF2 ===");
    {
        const std::vector<u8> woff = woffOf(flavor, tables, kZero, 0x41);
        uiSetInflate(nullptr);
        std::vector<u8> out;
        std::string err;
        check(!uiWoffToSfnt(woff.data(), woff.size(), out, &err) && err.find("inflate") != std::string::npos, "a compressed WOFF with no inflate installed says so");
        UiFontFace face;
        std::string ferr;
        check(!face.open(blobOf(woff), 0, &ferr) && !ferr.empty(), "and the face refuses it");
        uiSetInflate(&inflateAdapter);

        std::vector<u8> w2 = woff;
        w2[3] = '2';
        UiFontFace f2;
        std::string e2;
        check(uiIsWoff2(w2.data(), w2.size()) && !f2.open(blobOf(w2), 0, &e2) && e2.find("WOFF2") != std::string::npos && e2.find("convert") != std::string::npos,
              "WOFF2 is recognised and refused, saying to convert it");
    }

    AVER_INFO("=== malformed files are refused ===");
    {
        const std::vector<u8> good = woffOf(flavor, tables, kZero, 0x41);
        std::vector<u8> out;
        std::string err;
        const auto refused = [&](const std::string& what, std::vector<u8> bad) {
            err.clear();
            check(!uiWoffToSfnt(bad.data(), bad.size(), out, &err) && !err.empty(), what + " (" + err + ")");
        };
        refused("empty", {});
        refused("a header cut short", std::vector<u8>(good.begin(), good.begin() + 30));
        refused("the directory cut short", std::vector<u8>(good.begin(), good.begin() + 60));
        {   // length field wrong
            std::vector<u8> b = good; b[11] ^= 1; refused("a length that is not the file size", b);
        }
        {   std::vector<u8> b = good; b[15] = 1; refused("a non-zero reserved field", b); }
        {   std::vector<u8> b = good; b[12] = 0; b[13] = 0; refused("zero tables", b); }
        {   std::vector<u8> b = good; b[12] = 0; b[13] = 200; refused("more tables than the cap", b); }
        {   std::vector<u8> b = good; b[19] ^= 4; refused("a totalSfntSize that does not match the tables", b); }
        {   std::vector<u8> b = good; b[16] = 0x7F; refused("a totalSfntSize over the cap", b); }
        {   // first table's offset past the end
            std::vector<u8> b = good; b[44 + 4] = 0x7F; refused("a table offset past the file", b);
        }
        {   std::vector<u8> b = good; b[44 + 7] = 4; refused("a table inside the directory", b); }
        {   std::vector<u8> b = good; b[44 + 7] |= 1; refused("an unaligned table offset", b); }
        {   // swap the first two tags: out of order
            std::vector<u8> b = good;
            for (int i = 0; i < 4; ++i) std::swap(b[44 + i], b[44 + 20 + i]);
            refused("tables out of tag order", b);
        }
        {   // compLength > origLength for the first table
            std::vector<u8> b = good; b[44 + 8] = 0x10; refused("a table bigger compressed than it is", b);
        }
        {   // the compressed table (the one with tag ZERO) gets a bad zlib header
            std::vector<u8> b = good;
            usize ent = 0;
            for (u32 i = 0; i < tables.size(); ++i) if (be32(b.data() + 44 + 20 * i) == kZero) ent = 44 + 20 * i;
            const u32 off = be32(b.data() + ent + 4);
            b[off] = 0x79;
            refused("a compressed table that is not zlib", b);
            b = good; b[off + 2] ^= 0xFF; b[off + 3] ^= 0xFF; b[off + 4] ^= 0xFF;
            refused("a corrupt deflate stream", b);
            b = good;
            const u32 origOff = static_cast<u32>(ent) + 12;   // declared size too big for what the stream produces
            const u32 orig = be32(b.data() + origOff);
            b[origOff + 2] = static_cast<u8>((orig + 400) >> 8); b[origOff + 3] = static_cast<u8>(orig + 400);
            refused("a stream that stops short of the declared size", b);
        }
        // Truncating anywhere never crashes.
        bool survived = true;
        for (usize cut = 0; cut < good.size(); cut += 7) {
            std::vector<u8> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
            std::vector<u8> o;
            survived = survived && !uiWoffToSfnt(t.data(), t.size(), o);
        }
        check(survived, "cut at any length it is refused, not read past the end");
        // Corrupting any byte never crashes (it may still be a valid font).
        for (usize i = 0; i < good.size(); i += 5) {
            std::vector<u8> t = good;
            t[i] ^= 0x5A;
            std::vector<u8> o;
            (void)uiWoffToSfnt(t.data(), t.size(), o);
        }
        check(true, "flipping bytes anywhere never reads out of bounds (a clean run under the harness)");
    }

    if (g_failures == 0) AVER_INFO("=== all WOFF tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
