// A Zstandard (RFC 8878) frame decoder for Godot's compressed binary resources. See GodotZstd.hpp.
#include "aver/formats/godot/GodotZstd.hpp"

#include <algorithm>
#include <cstring>

namespace aver::fmt::godot {

namespace {

constexpr usize kMaxBlock = 128 * 1024;  // Block_Maximum_Size
constexpr int kMaxHufBits = 11;

bool bad(std::string* why, const char* msg) {
    if (why && why->empty()) *why = msg;
    return false;
}

int highBit(u32 v) {  // v > 0
    int r = 0;
    while (v >>= 1) ++r;
    return r;
}

u64 lowMask(int nb) { return nb >= 64 ? ~u64(0) : ((u64(1) << nb) - 1); }

// Reads bits low to high, as the FSE table descriptions are stored. Bytes past the end read as zero; overrun() says whether any were needed.
struct FwdBits {
    const u8* p;
    usize n;
    usize pos = 0;  // bit position
    u32 peek(int nb) const {
        u64 v = 0;
        const usize byte = pos >> 3;
        for (usize i = 0; i < 8; ++i)
            if (byte + i < n) v |= u64(p[byte + i]) << (8 * i);
        v >>= (pos & 7);
        return u32(v & lowMask(nb));
    }
    void skip(int nb) { pos += usize(nb); }
    bool overrun() const { return pos > n * 8; }
    usize bytesUsed() const { return (pos + 7) >> 3; }
};

// Reads bits backwards from the end of a buffer whose last byte holds a 1 marker above the data (Huffman streams, the weights, the sequences).
// `remaining` is the number of unread data bits; it goes negative when more were asked for than exist, and the missing low bits read as zero.
struct RevBits {
    const u8* p = nullptr;
    usize n = 0;
    i64 remaining = 0;

    bool init(const u8* data, usize size) {
        if (size == 0 || data[size - 1] == 0) return false;
        p = data;
        n = size;
        remaining = i64(size - 1) * 8 + highBit(data[size - 1]);
        return true;
    }
    u64 peek(int nb) const {  // nb <= 32
        if (nb == 0) return 0;
        const i64 start = remaining - nb;
        u64 v = 0;
        if (start >= 0) {
            const usize byte = usize(start >> 3);
            for (usize i = 0; i < 8; ++i)
                if (byte + i < n) v |= u64(p[byte + i]) << (8 * i);
            v >>= (start & 7);
            return v & lowMask(nb);
        }
        if (remaining <= 0) return 0;
        for (usize i = 0; i < 8 && i < n; ++i) v |= u64(p[i]) << (8 * i);
        v &= lowMask(int(remaining));
        return v << (-start);
    }
    u64 read(int nb) {
        const u64 v = peek(nb);
        remaining -= nb;
        return v;
    }
    bool overflow() const { return remaining < 0; }
};

// ---- FSE ----------------------------------------------------------------------------------------------------------------------

struct FseEntry {
    u16 base = 0;
    u8 symbol = 0;
    u8 nbBits = 0;
};

struct FseTable {
    std::vector<FseEntry> t;
    int accLog = 0;
};

// `norm` holds the normalised counts (-1 = "less than one"). The spread step and the state numbering are fixed by the format.
bool buildFse(const i16* norm, int nsym, int accLog, FseTable& out, std::string* why) {
    const int size = 1 << accLog;
    out.t.assign(usize(size), FseEntry{});
    out.accLog = accLog;
    u16 next[256] = {};
    int high = size - 1;
    int total = 0;
    for (int s = 0; s < nsym; ++s) {
        if (norm[s] == -1) {
            if (high < 0) return bad(why, "zstd: FSE table overfull");
            out.t[usize(high--)].symbol = u8(s);
            next[s] = 1;
            total += 1;
        } else if (norm[s] >= 0) {
            next[s] = u16(norm[s]);
            total += norm[s];
        } else {
            return bad(why, "zstd: bad FSE count");
        }
    }
    if (total != size) return bad(why, "zstd: FSE counts do not fill the table");
    const int step = (size >> 1) + (size >> 3) + 3;
    const int mask = size - 1;
    int pos = 0;
    for (int s = 0; s < nsym; ++s) {
        for (int i = 0; i < norm[s]; ++i) {
            out.t[usize(pos)].symbol = u8(s);
            do pos = (pos + step) & mask;
            while (pos > high);
        }
    }
    if (pos != 0) return bad(why, "zstd: FSE spread did not close");
    for (int u = 0; u < size; ++u) {
        FseEntry& e = out.t[usize(u)];
        const u32 ns = next[e.symbol]++;
        const int nb = accLog - highBit(ns);
        if (nb < 0 || nb > accLog) return bad(why, "zstd: bad FSE state");
        e.nbBits = u8(nb);
        e.base = u16((ns << nb) - u32(size));
    }
    return true;
}

// The table description: accuracy log, then the counts with a variable-width code. `used` is the bytes it took.
bool readFseHeader(const u8* p, usize n, int maxSym, int maxLog, FseTable& out, usize& used, std::string* why) {
    FwdBits br{p, n};
    const int accLog = int(br.peek(4)) + 5;
    br.skip(4);
    if (accLog > maxLog) return bad(why, "zstd: FSE accuracy log too large");
    i16 norm[256] = {};
    int remaining = (1 << accLog) + 1;
    int threshold = 1 << accLog;
    int nbBits = accLog + 1;
    int sym = 0;
    bool prev0 = false;
    while (remaining > 1 && sym <= maxSym) {
        if (prev0) {
            // A zero count is followed by 2-bit repeat flags: that many more symbols with count zero; a 3 asks for another flag.
            for (;;) {
                const int r = int(br.peek(2));
                br.skip(2);
                sym += r;
                if (sym > maxSym + 1) return bad(why, "zstd: FSE symbol run past the alphabet");
                if (r != 3) break;
            }
            prev0 = false;
            continue;
        }
        const int mx = (2 * threshold - 1) - remaining;
        const u32 bits = br.peek(nbBits);
        int count;
        if (int(bits & u32(threshold - 1)) < mx) {
            count = int(bits & u32(threshold - 1));
            br.skip(nbBits - 1);
        } else {
            count = int(bits & u32(2 * threshold - 1));
            if (count >= threshold) count -= mx;
            br.skip(nbBits);
        }
        --count;
        remaining -= count < 0 ? -count : count;
        if (remaining < 1) return bad(why, "zstd: FSE counts overshoot");
        norm[sym++] = i16(count);
        prev0 = (count == 0);
        while (remaining < threshold) {
            --nbBits;
            threshold >>= 1;
        }
        if (br.overrun()) return bad(why, "zstd: FSE description runs past its bytes");
    }
    if (remaining != 1 || br.overrun()) return bad(why, "zstd: FSE description is incomplete");
    used = br.bytesUsed();
    return buildFse(norm, sym, accLog, out, why);
}

// ---- Huffman ------------------------------------------------------------------------------------------------------------------

struct HufTable {
    std::vector<u8> sym, nb;
    int maxBits = 0;
};

bool buildHuf(const u8* w, int count, HufTable& out, std::string* why) {
    // The last weight is implied: the weights must sum to a power of two.
    u32 total = 0;
    for (int i = 0; i < count; ++i) {
        if (w[i] > kMaxHufBits) return bad(why, "zstd: Huffman weight too large");
        if (w[i]) total += 1u << (w[i] - 1);
    }
    if (total == 0) return bad(why, "zstd: Huffman weights are all zero");
    const int maxBits = highBit(total) + 1;
    if (maxBits > kMaxHufBits) return bad(why, "zstd: Huffman code too long");
    const u32 rest = (1u << maxBits) - total;
    if (rest == 0 || (rest & (rest - 1)) != 0) return bad(why, "zstd: Huffman weights do not complete a tree");
    u8 weights[256];
    std::memcpy(weights, w, usize(count));
    if (count >= 256) return bad(why, "zstd: too many Huffman symbols");
    weights[count] = u8(highBit(rest) + 1);
    const int nsym = count + 1;

    u32 rankStart[kMaxHufBits + 2] = {};
    u32 perWeight[kMaxHufBits + 2] = {};
    for (int i = 0; i < nsym; ++i)
        if (weights[i]) ++perWeight[weights[i]];
    u32 acc = 0;
    for (int wt = 1; wt <= maxBits; ++wt) {
        rankStart[wt] = acc;
        acc += perWeight[wt] << (wt - 1);
    }
    out.maxBits = maxBits;
    out.sym.assign(usize(1) << maxBits, 0);
    out.nb.assign(usize(1) << maxBits, 0);
    for (int i = 0; i < nsym; ++i) {
        const int wt = weights[i];
        if (!wt) continue;
        const u32 cnt = 1u << (wt - 1);
        const u32 start = rankStart[wt];
        for (u32 k = 0; k < cnt; ++k) {
            out.sym[start + k] = u8(i);
            out.nb[start + k] = u8(maxBits + 1 - wt);
        }
        rankStart[wt] += cnt;
    }
    return true;
}

bool readHufTree(const u8* p, usize n, HufTable& out, usize& used, std::string* why) {
    if (n < 1) return bad(why, "zstd: empty Huffman description");
    const u32 hdr = p[0];
    u8 w[256];
    int nw = 0;
    if (hdr >= 128) {
        nw = int(hdr) - 127;
        const usize bytes = (usize(nw) + 1) / 2;
        if (1 + bytes > n) return bad(why, "zstd: Huffman weights run past the section");
        for (int i = 0; i < nw; ++i) w[i] = (i & 1) ? u8(p[1 + i / 2] & 15) : u8(p[1 + i / 2] >> 4);
        used = 1 + bytes;
    } else {
        const usize size = hdr;
        if (size == 0 || 1 + size > n) return bad(why, "zstd: Huffman weight stream size is wrong");
        FseTable ft;
        usize hb = 0;
        if (!readFseHeader(p + 1, size, 12, 6, ft, hb, why)) return false;
        if (hb >= size) return bad(why, "zstd: Huffman weight stream is missing");
        RevBits rb;
        if (!rb.init(p + 1 + hb, size - hb)) return bad(why, "zstd: Huffman weight stream has no end mark");
        u32 s1 = u32(rb.read(ft.accLog));
        u32 s2 = u32(rb.read(ft.accLog));
        for (;;) {
            if (nw >= 254) return bad(why, "zstd: too many Huffman weights");
            const FseEntry& e1 = ft.t[s1];
            w[nw++] = e1.symbol;
            s1 = e1.base + u32(rb.read(e1.nbBits));
            if (rb.overflow()) {
                w[nw++] = ft.t[s2].symbol;
                break;
            }
            const FseEntry& e2 = ft.t[s2];
            w[nw++] = e2.symbol;
            s2 = e2.base + u32(rb.read(e2.nbBits));
            if (rb.overflow()) {
                w[nw++] = ft.t[s1].symbol;
                break;
            }
        }
        used = 1 + size;
    }
    if (nw < 1) return bad(why, "zstd: no Huffman weights");
    return buildHuf(w, nw, out, why);
}

bool decodeHufStream(const u8* p, usize n, const HufTable& h, u8* dst, usize count, std::string* why) {
    RevBits rb;
    if (!rb.init(p, n)) return bad(why, "zstd: literal stream has no end mark");
    for (usize i = 0; i < count; ++i) {
        const u32 idx = u32(rb.peek(h.maxBits));
        dst[i] = h.sym[idx];
        rb.remaining -= h.nb[idx];
    }
    if (rb.remaining != 0) return bad(why, "zstd: literal stream is not fully used");
    return true;
}

// ---- blocks -------------------------------------------------------------------------------------------------------------------

struct Ctx {
    FseTable fse[3];  // literal lengths, offsets, match lengths
    bool haveFse[3] = {false, false, false};
    HufTable huf;
    bool haveHuf = false;
    u32 rep[3] = {1, 4, 8};
};

bool decodeLiterals(const u8* b, usize n, usize& pos, Ctx& c, std::vector<u8>& lits, std::string* why) {
    if (n < 1) return bad(why, "zstd: empty literals section");
    const u32 b0 = b[0];
    const int type = int(b0 & 3);
    const int sf = int((b0 >> 2) & 3);
    if (type < 2) {
        usize hdr, regen;
        if (sf == 0 || sf == 2) {
            hdr = 1;
            regen = b0 >> 3;
        } else if (sf == 1) {
            if (n < 2) return bad(why, "zstd: literals header runs past the block");
            hdr = 2;
            regen = (b0 >> 4) | (u32(b[1]) << 4);
        } else {
            if (n < 3) return bad(why, "zstd: literals header runs past the block");
            hdr = 3;
            regen = (b0 >> 4) | (u32(b[1]) << 4) | (u32(b[2]) << 12);
        }
        if (regen > kMaxBlock) return bad(why, "zstd: literals larger than a block");
        if (type == 0) {
            if (hdr + regen > n) return bad(why, "zstd: raw literals run past the block");
            lits.assign(b + hdr, b + hdr + regen);
            pos = hdr + regen;
        } else {
            if (hdr + 1 > n) return bad(why, "zstd: RLE literal is missing");
            lits.assign(regen, b[hdr]);
            pos = hdr + 1;
        }
        return true;
    }
    const int hdr = sf <= 1 ? 3 : (sf == 2 ? 4 : 5);
    if (usize(hdr) > n) return bad(why, "zstd: literals header runs past the block");
    u64 v = 0;
    for (int i = 0; i < hdr; ++i) v |= u64(b[i]) << (8 * i);
    v >>= 4;
    const int bits = sf <= 1 ? 10 : (sf == 2 ? 14 : 18);
    const usize regen = usize(v & lowMask(bits));
    const usize comp = usize((v >> bits) & lowMask(bits));
    if (regen > kMaxBlock) return bad(why, "zstd: literals larger than a block");
    if (usize(hdr) + comp > n) return bad(why, "zstd: compressed literals run past the block");
    const u8* s = b + hdr;
    usize sn = comp;
    if (type == 2) {
        usize used = 0;
        if (!readHufTree(s, sn, c.huf, used, why)) return false;
        c.haveHuf = true;
        s += used;
        sn -= used;
    } else if (!c.haveHuf) {
        return bad(why, "zstd: treeless literals before any Huffman table");
    }
    lits.assign(regen, 0);
    if (sf == 0) {
        if (!decodeHufStream(s, sn, c.huf, lits.data(), regen, why)) return false;
    } else {
        if (sn < 6) return bad(why, "zstd: literal jump table is missing");
        const usize s1 = usize(s[0]) | (usize(s[1]) << 8);
        const usize s2 = usize(s[2]) | (usize(s[3]) << 8);
        const usize s3 = usize(s[4]) | (usize(s[5]) << 8);
        if (6 + s1 + s2 + s3 >= sn) return bad(why, "zstd: literal streams run past the section");
        const usize s4 = sn - 6 - s1 - s2 - s3;
        const usize each = (regen + 3) / 4;
        if (each * 3 > regen) return bad(why, "zstd: four literal streams for too few literals");
        const u8* q = s + 6;
        if (!decodeHufStream(q, s1, c.huf, lits.data(), each, why)) return false;
        q += s1;
        if (!decodeHufStream(q, s2, c.huf, lits.data() + each, each, why)) return false;
        q += s2;
        if (!decodeHufStream(q, s3, c.huf, lits.data() + 2 * each, each, why)) return false;
        q += s3;
        if (!decodeHufStream(q, s4, c.huf, lits.data() + 3 * each, regen - 3 * each, why)) return false;
    }
    pos = usize(hdr) + comp;
    return true;
}

constexpr u32 kLlBase[36] = {0,  1,  2,  3,  4,  5,   6,   7,   8,   9,   10,  11,   12,   13,   14,    15,
                             16, 18, 20, 22, 24, 28,  32,  40,  48,  64,  128, 256,  512,  1024, 2048,  4096,
                             8192, 16384, 32768, 65536};
constexpr u8 kLlBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr u32 kMlBase[53] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                             35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
constexpr u8 kMlBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                            1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

constexpr i16 kLlDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
constexpr i16 kMlDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
constexpr i16 kOfDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

bool decodeSequences(const u8* b, usize n, usize pos, const std::vector<u8>& lits, Ctx& c, std::vector<u8>& out, usize frameStart, usize maxOut,
                     std::string* why) {
    const usize blockStart = out.size();
    if (pos >= n) return bad(why, "zstd: the sequences section is missing");
    u32 nseq = b[pos++];
    if (nseq >= 128) {
        if (nseq == 255) {
            if (pos + 2 > n) return bad(why, "zstd: sequence count runs past the block");
            nseq = u32(b[pos]) + (u32(b[pos + 1]) << 8) + 0x7F00;
            pos += 2;
        } else {
            if (pos + 1 > n) return bad(why, "zstd: sequence count runs past the block");
            nseq = ((nseq - 128) << 8) + b[pos++];
        }
    }
    usize litPos = 0;
    if (nseq > 0) {
        if (pos >= n) return bad(why, "zstd: sequence table modes are missing");
        const u32 modes = b[pos++];
        if (modes & 3) return bad(why, "zstd: reserved sequence mode bits are set");
        static const int kMaxSym[3] = {35, 31, 52};
        static const int kMaxLog[3] = {9, 8, 9};
        for (int k = 0; k < 3; ++k) {
            const int mode = int((modes >> (6 - 2 * k)) & 3);
            FseTable& t = c.fse[k];
            if (mode == 0) {
                static const int kDefLog[3] = {6, 5, 6};
                if (k == 0) {
                    if (!buildFse(kLlDefault, 36, kDefLog[0], t, why)) return false;
                } else if (k == 1) {
                    if (!buildFse(kOfDefault, 29, kDefLog[1], t, why)) return false;
                } else {
                    if (!buildFse(kMlDefault, 53, kDefLog[2], t, why)) return false;
                }
            } else if (mode == 1) {
                if (pos >= n) return bad(why, "zstd: RLE sequence symbol is missing");
                const u8 s = b[pos++];
                if (s > kMaxSym[k]) return bad(why, "zstd: RLE sequence symbol out of range");
                t.t.assign(1, FseEntry{});
                t.t[0].symbol = s;
                t.accLog = 0;
            } else if (mode == 2) {
                usize used = 0;
                if (!readFseHeader(b + pos, n - pos, kMaxSym[k], kMaxLog[k], t, used, why)) return false;
                pos += used;
            } else if (!c.haveFse[k]) {
                return bad(why, "zstd: repeated sequence table before any table");
            }
            c.haveFse[k] = true;
        }
        RevBits rb;
        if (!rb.init(b + pos, n - pos)) return bad(why, "zstd: sequence stream has no end mark");
        u32 sLl = u32(rb.read(c.fse[0].accLog));
        u32 sOf = u32(rb.read(c.fse[1].accLog));
        u32 sMl = u32(rb.read(c.fse[2].accLog));
        for (u32 i = 0; i < nseq; ++i) {
            const FseEntry& eLl = c.fse[0].t[sLl];
            const FseEntry& eOf = c.fse[1].t[sOf];
            const FseEntry& eMl = c.fse[2].t[sMl];
            if (eLl.symbol > 35 || eMl.symbol > 52 || eOf.symbol > 31) return bad(why, "zstd: sequence code out of range");
            const u32 ofVal = (1u << eOf.symbol) + u32(rb.read(eOf.symbol));
            const u32 mlVal = kMlBase[eMl.symbol] + u32(rb.read(kMlBits[eMl.symbol]));
            const u32 llVal = kLlBase[eLl.symbol] + u32(rb.read(kLlBits[eLl.symbol]));
            if (i + 1 < nseq) {
                sLl = eLl.base + u32(rb.read(eLl.nbBits));
                sMl = eMl.base + u32(rb.read(eMl.nbBits));
                sOf = eOf.base + u32(rb.read(eOf.nbBits));
            }
            if (rb.overflow()) return bad(why, "zstd: sequence stream runs out");

            u32 offset;
            if (ofVal > 3) {
                offset = ofVal - 3;
                c.rep[2] = c.rep[1];
                c.rep[1] = c.rep[0];
                c.rep[0] = offset;
            } else {
                const u32 idx = (llVal == 0) ? ofVal : ofVal - 1;  // 0..3 into the repeat list; 3 is "first minus one"
                if (idx == 3) {
                    offset = c.rep[0] - 1;
                    if (offset == 0) return bad(why, "zstd: repeat offset of zero");
                    c.rep[2] = c.rep[1];
                    c.rep[1] = c.rep[0];
                    c.rep[0] = offset;
                } else {
                    offset = c.rep[idx];
                    if (idx == 1) {
                        c.rep[1] = c.rep[0];
                        c.rep[0] = offset;
                    } else if (idx == 2) {
                        c.rep[2] = c.rep[1];
                        c.rep[1] = c.rep[0];
                        c.rep[0] = offset;
                    }
                }
            }

            if (llVal > lits.size() - litPos) return bad(why, "zstd: literal length passes the literals");
            if (out.size() - blockStart + llVal + mlVal > kMaxBlock) return bad(why, "zstd: block output passes 128 KiB");
            if (out.size() + llVal + mlVal > maxOut) return bad(why, "zstd: output limit reached");
            out.insert(out.end(), lits.begin() + isize(litPos), lits.begin() + isize(litPos + llVal));
            litPos += llVal;
            if (offset == 0 || offset > out.size() - frameStart) return bad(why, "zstd: match offset reaches before the frame");
            usize from = out.size() - offset;
            for (u32 k = 0; k < mlVal; ++k) out.push_back(out[from + k]);
        }
        if (rb.remaining != 0) return bad(why, "zstd: sequence stream is not fully used");
    } else if (pos != n) {
        return bad(why, "zstd: bytes after an empty sequences section");
    }
    const usize left = lits.size() - litPos;
    if (out.size() - blockStart + left > kMaxBlock) return bad(why, "zstd: block output passes 128 KiB");
    if (out.size() + left > maxOut) return bad(why, "zstd: output limit reached");
    out.insert(out.end(), lits.begin() + isize(litPos), lits.end());
    return true;
}

bool decodeFrame(const u8* src, usize size, usize& pos, std::vector<u8>& out, usize maxOut, std::string* why) {
    // pos is just past the magic number
    if (pos + 1 > size) return bad(why, "zstd: frame header is cut off");
    const u32 fhd = src[pos++];
    const int fcsFlag = int(fhd >> 6);
    const bool single = (fhd >> 5) & 1;
    const bool checksum = (fhd >> 2) & 1;
    const int dictFlag = int(fhd & 3);
    if (fhd & 8) return bad(why, "zstd: reserved frame header bit is set");
    if (!single) {
        if (pos + 1 > size) return bad(why, "zstd: window descriptor is cut off");
        ++pos;
    }
    static const int kDictBytes[4] = {0, 1, 2, 4};
    if (pos + usize(kDictBytes[dictFlag]) > size) return bad(why, "zstd: dictionary id is cut off");
    u32 dictId = 0;
    for (int i = 0; i < kDictBytes[dictFlag]; ++i) dictId |= u32(src[pos + usize(i)]) << (8 * i);
    pos += usize(kDictBytes[dictFlag]);
    if (dictId != 0) return bad(why, "zstd: frames that need a dictionary are not supported");
    int fcsBytes = fcsFlag == 0 ? (single ? 1 : 0) : (fcsFlag == 1 ? 2 : (fcsFlag == 2 ? 4 : 8));
    u64 contentSize = 0;
    bool haveSize = fcsBytes > 0;
    if (pos + usize(fcsBytes) > size) return bad(why, "zstd: content size is cut off");
    for (int i = 0; i < fcsBytes; ++i) contentSize |= u64(src[pos + usize(i)]) << (8 * i);
    pos += usize(fcsBytes);
    if (fcsFlag == 1) contentSize += 256;
    if (haveSize && contentSize > maxOut) return bad(why, "zstd: frame content passes the output limit");

    const usize frameStart = out.size();
    Ctx ctx;
    std::vector<u8> lits;
    for (;;) {
        if (pos + 3 > size) return bad(why, "zstd: block header is cut off");
        const u32 bh = u32(src[pos]) | (u32(src[pos + 1]) << 8) | (u32(src[pos + 2]) << 16);
        pos += 3;
        const bool last = bh & 1;
        const int type = int((bh >> 1) & 3);
        const usize bsize = bh >> 3;
        if (type == 3) return bad(why, "zstd: reserved block type");
        if (type == 0) {
            if (bsize > kMaxBlock || pos + bsize > size) return bad(why, "zstd: raw block runs past the frame");
            if (out.size() + bsize > maxOut) return bad(why, "zstd: output limit reached");
            out.insert(out.end(), src + pos, src + pos + bsize);
            pos += bsize;
        } else if (type == 1) {
            if (bsize > kMaxBlock || pos + 1 > size) return bad(why, "zstd: RLE block is cut off");
            if (out.size() + bsize > maxOut) return bad(why, "zstd: output limit reached");
            out.insert(out.end(), bsize, src[pos]);
            pos += 1;
        } else {
            if (bsize > kMaxBlock || pos + bsize > size) return bad(why, "zstd: compressed block runs past the frame");
            usize lpos = 0;
            if (!decodeLiterals(src + pos, bsize, lpos, ctx, lits, why)) return false;
            if (!decodeSequences(src + pos, bsize, lpos, lits, ctx, out, frameStart, maxOut, why)) return false;
            pos += bsize;
        }
        if (last) break;
    }
    if (checksum) {
        if (pos + 4 > size) return bad(why, "zstd: checksum is cut off");
        pos += 4;  // not verified
    }
    if (haveSize && u64(out.size() - frameStart) != contentSize) return bad(why, "zstd: frame content size does not match what was decoded");
    return true;
}

}  // namespace

bool zstdDecompress(const u8* src, usize size, std::vector<u8>& out, usize maxOut, std::string* why) {
    if (why) why->clear();
    usize pos = 0;
    bool any = false;
    while (pos < size) {
        if (pos + 4 > size) return bad(why, "zstd: trailing bytes after the last frame");
        const u32 magic = u32(src[pos]) | (u32(src[pos + 1]) << 8) | (u32(src[pos + 2]) << 16) | (u32(src[pos + 3]) << 24);
        pos += 4;
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {  // a skippable frame
            if (pos + 4 > size) return bad(why, "zstd: skippable frame is cut off");
            const u32 len = u32(src[pos]) | (u32(src[pos + 1]) << 8) | (u32(src[pos + 2]) << 16) | (u32(src[pos + 3]) << 24);
            pos += 4;
            if (len > size - pos) return bad(why, "zstd: skippable frame runs past the data");
            pos += len;
            continue;
        }
        if (magic != 0xFD2FB528u) return bad(why, "zstd: not a Zstandard frame");
        if (!decodeFrame(src, size, pos, out, maxOut, why)) return false;
        any = true;
    }
    if (!any) return bad(why, "zstd: no frame in the data");
    return true;
}

}  // namespace aver::fmt::godot
