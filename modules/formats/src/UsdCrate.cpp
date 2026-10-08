#include "aver/formats/UsdCrate.hpp"

#include <cmath>
#include <cstring>
#include <fstream>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// ---- bounded reading ----------------------------------------------------------------------------
//
// EVERY READ IS CHECKED against the buffer and latches `ok` false on the first overrun, so a
// truncated or hostile file yields a refusal instead of a read past the end: counts, offsets and
// sizes in a crate are all file data, and a single wrong one would otherwise index anywhere.
struct Rd {
    const u8* p = nullptr;
    usize n = 0;
    usize at = 0;
    bool ok = true;

    bool need(usize k) {
        if (!ok || at > n || k > n - at) { ok = false; return false; }
        return true;
    }
    template <class T> T get() {
        T v{};
        if (need(sizeof(T))) { std::memcpy(&v, p + at, sizeof(T)); at += sizeof(T); }
        return v;
    }
    const u8* take(usize k) {
        if (!need(k)) return nullptr;
        const u8* r = p + at;
        at += k;
        return r;
    }
    void seek(u64 to) {
        if (to > n) { ok = false; at = n; } else at = static_cast<usize>(to);
    }
};

// ---- LZ4 block decoding, appended to `dst`, never past `cap` bytes in total ---------------------
//
// A plain LZ4 BLOCK (not the frame format): token, literals, 2-byte offset, match. A match may only
// reach back into what THIS block produced -- TfFastCompression compresses each chunk on its own --
// which is why `base` bounds the offset rather than the start of `dst`.
bool lz4Append(const u8* src, usize n, std::vector<u8>& dst, usize cap) {
    const usize base = dst.size();
    usize i = 0;
    while (i < n) {
        const u32 token = src[i++];
        usize lit = token >> 4;
        if (lit == 15) {
            for (;;) {
                if (i >= n) return false;
                const u8 b = src[i++];
                lit += b;
                if (b != 255) break;
            }
        }
        if (lit > n - i || dst.size() > cap || lit > cap - dst.size()) return false;
        dst.insert(dst.end(), src + i, src + i + lit);
        i += lit;
        if (i == n) break;                      // the last sequence carries literals only
        if (n - i < 2) return false;
        const usize off = static_cast<usize>(src[i]) | (static_cast<usize>(src[i + 1]) << 8);
        i += 2;
        if (off == 0 || off > dst.size() - base) return false;
        usize len = token & 15u;
        if (len == 15) {
            for (;;) {
                if (i >= n) return false;
                const u8 b = src[i++];
                len += b;
                if (b != 255) break;
            }
        }
        len += 4;
        if (len > cap - dst.size()) return false;
        const usize at = dst.size();
        dst.resize(at + len);
        u8* d = dst.data();
        // An overlapping match (offset shorter than the length) repeats the tail byte by byte,
        // which is how LZ4 encodes runs; a memcpy there would read bytes not yet written.
        if (off >= len) std::memcpy(d + at, d + at - off, len);
        else for (usize k = 0; k < len; ++k) d[at + k] = d[at + k - off];
    }
    return true;
}

// TfFastCompression: a leading chunk count, 0 meaning "one LZ4 block follows", otherwise that many
// (int32 size, block) pairs whose outputs concatenate.
bool fastDecompress(const u8* src, usize n, std::vector<u8>& out, usize cap) {
    out.clear();
    if (n == 0) return false;
    // Reserved up front: `cap` is the exact decoded size for every caller here, and growing a
    // multi-megabyte vector by doubling would copy it a dozen times.
    out.reserve(cap);
    const u32 chunks = src[0];
    if (chunks == 0) return lz4Append(src + 1, n - 1, out, cap);
    usize p = 1;
    for (u32 k = 0; k < chunks; ++k) {
        if (n - p < 4) return false;
        i32 sz = 0;
        std::memcpy(&sz, src + p, 4);
        p += 4;
        if (sz < 0 || static_cast<usize>(sz) > n - p) return false;
        if (!lz4Append(src + p, static_cast<usize>(sz), out, cap)) return false;
        p += static_cast<usize>(sz);
    }
    return true;
}

// ---- Usd_IntegerCompression: delta-coded integers with a 2-bit width code per value ------------
//
// Layout of the decompressed block: the most common delta (int32, or int64 for the wide form), then
// two bits per value -- 0 "the common delta", 1/2/3 "a delta of 8/16/32 bits follows" (16/32/64 for
// the wide form), low bits first within each byte -- then those deltas packed back to back. Each
// value is the running sum of its deltas, which is what makes sorted index lists nearly free.
template <bool Wide>
bool decodeInts(const u8* w, usize wn, usize count, std::vector<i64>& out) {
    const usize cs = Wide ? 8 : 4;
    const usize codeBytes = (count * 2 + 7) / 8;
    if (wn < cs || wn - cs < codeBytes) return false;
    i64 common = 0;
    if (Wide) { i64 c; std::memcpy(&c, w, 8); common = c; }
    else      { i32 c; std::memcpy(&c, w, 4); common = c; }
    const u8* codes = w + cs;
    usize vp = cs + codeBytes;
    out.resize(count);
    i64 prev = 0;
    for (usize k = 0; k < count; ++k) {
        const u32 code = (codes[k >> 2] >> ((k & 3) * 2)) & 3u;
        i64 d = common;
        if (code != 0) {
            const usize sz = Wide ? (code == 1 ? 2u : code == 2 ? 4u : 8u)
                                  : (code == 1 ? 1u : code == 2 ? 2u : 4u);
            if (wn - vp < sz) return false;
            if (sz == 1)      { i8 v;  std::memcpy(&v, w + vp, 1); d = v; }
            else if (sz == 2) { i16 v; std::memcpy(&v, w + vp, 2); d = v; }
            else if (sz == 4) { i32 v; std::memcpy(&v, w + vp, 4); d = v; }
            else              { i64 v; std::memcpy(&v, w + vp, 8); d = v; }
            vp += sz;
        }
        // The narrow form accumulates in 32 bits and wraps there, exactly as the writer's int32
        // arithmetic did; accumulating wider would diverge on the first wrap.
        if (Wide) prev = static_cast<i64>(static_cast<u64>(prev) + static_cast<u64>(d));
        else      prev = static_cast<i32>(static_cast<u32>(prev) + static_cast<u32>(d));
        out[k] = prev;
    }
    return true;
}

// `u64 compressedSize, bytes` -> `count` integers.
bool readCompressedInts(Rd& r, usize count, std::vector<i64>& out, bool wide) {
    const u64 cz = r.get<u64>();
    if (!r.ok || cz > r.n) return false;
    const u8* comp = r.take(static_cast<usize>(cz));
    if (!comp) return false;
    const usize cs = wide ? 8 : 4;
    const usize cap = cs + (count * 2 + 7) / 8 + count * (wide ? 8u : 4u);
    std::vector<u8> work;
    if (!fastDecompress(comp, static_cast<usize>(cz), work, cap)) return false;
    return wide ? decodeInts<true>(work.data(), work.size(), count, out)
                : decodeInts<false>(work.data(), work.size(), count, out);
}

f32 halfToFloat(u16 h) {
    const u32 s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    if (e == 0) {
        const f32 v = std::ldexp(static_cast<f32>(m), -24);   // zero or subnormal
        return s ? -v : v;
    }
    u32 bits;
    if (e == 31) bits = (s << 31) | 0x7f800000u | (m << 13);
    else         bits = (s << 31) | ((e + 112u) << 23) | (m << 13);
    f32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// How a type's elements are stored: kind, numbers per element, bytes per number.
enum Kind : u8 { kNone, kInt, kUInt, kHalf, kFloat, kDouble, kToken, kString };
struct TypeInfo { Kind kind; u8 comps; u8 bytes; };

TypeInfo typeInfo(u8 t) {
    using namespace UsdCrateType;
    switch (t) {
        case Bool: case UChar:                return {kUInt, 1, 1};
        case Int:                             return {kInt, 1, 4};
        case UInt:                            return {kUInt, 1, 4};
        case Int64:                           return {kInt, 1, 8};
        case UInt64:                          return {kUInt, 1, 8};
        case Half:                            return {kHalf, 1, 2};
        case Float:                           return {kFloat, 1, 4};
        case Double: case TimeCode:           return {kDouble, 1, 8};
        case String:                          return {kString, 1, 4};
        case Token: case AssetPath:           return {kToken, 1, 4};
        case Matrix2d:                        return {kDouble, 4, 8};
        case Matrix3d:                        return {kDouble, 9, 8};
        case Matrix4d:                        return {kDouble, 16, 8};
        case Quatd:                           return {kDouble, 4, 8};
        case Quatf:                           return {kFloat, 4, 4};
        case Quath:                           return {kHalf, 4, 2};
        case Vec2d:                           return {kDouble, 2, 8};
        case Vec2f:                           return {kFloat, 2, 4};
        case Vec2h:                           return {kHalf, 2, 2};
        case Vec2i:                           return {kInt, 2, 4};
        case Vec3d:                           return {kDouble, 3, 8};
        case Vec3f:                           return {kFloat, 3, 4};
        case Vec3h:                           return {kHalf, 3, 2};
        case Vec3i:                           return {kInt, 3, 4};
        case Vec4d:                           return {kDouble, 4, 8};
        case Vec4f:                           return {kFloat, 4, 4};
        case Vec4h:                           return {kHalf, 4, 2};
        case Vec4i:                           return {kInt, 4, 4};
        case Specifier: case Permission: case Variability: return {kUInt, 1, 4};
        default:                              return {kNone, 1, 0};
    }
}

// `elems` elements of `ti` read straight from the file into the matching vector of `v`.
bool readRaw(Rd& r, const TypeInfo& ti, usize elems, UsdCrateValue& v) {
    const usize total = elems * ti.comps;
    if (ti.bytes == 0 || (total && total / ti.comps != elems)) return false;
    const u8* src = r.take(total * ti.bytes);
    if (!src) return false;
    switch (ti.kind) {
        case kFloat:
            v.f.resize(total);
            std::memcpy(v.f.data(), src, total * 4);
            return true;
        case kDouble:
            v.d.resize(total);
            std::memcpy(v.d.data(), src, total * 8);
            return true;
        case kHalf:
            v.f.resize(total);
            for (usize k = 0; k < total; ++k) { u16 h; std::memcpy(&h, src + k * 2, 2); v.f[k] = halfToFloat(h); }
            return true;
        case kInt: case kUInt:
            v.i.resize(total);
            for (usize k = 0; k < total; ++k) {
                const u8* e = src + k * ti.bytes;
                if (ti.bytes == 1)      v.i[k] = *e;
                else if (ti.bytes == 4) {
                    if (ti.kind == kInt) { i32 x; std::memcpy(&x, e, 4); v.i[k] = x; }
                    else                 { u32 x; std::memcpy(&x, e, 4); v.i[k] = x; }
                } else                  { i64 x; std::memcpy(&x, e, 8); v.i[k] = x; }
            }
            return true;
        default:
            return false;
    }
}

bool versionAtLeast(const u8 v[3], u8 a, u8 b) { return v[0] > a || (v[0] == a && v[1] >= b); }

} // namespace

// ---- UsdCrateValue ------------------------------------------------------------------------------

std::vector<f32> UsdCrateValue::asFloats() const {
    if (!f.empty()) return f;
    std::vector<f32> out;
    out.reserve(d.size() + i.size());
    for (const f64 x : d) out.push_back(static_cast<f32>(x));
    for (const i64 x : i) out.push_back(static_cast<f32>(x));
    return out;
}

std::vector<i32> UsdCrateValue::asInts() const {
    std::vector<i32> out;
    out.reserve(numberCount());
    for (const i64 x : i) out.push_back(static_cast<i32>(x));
    for (const f32 x : f) out.push_back(static_cast<i32>(x));
    for (const f64 x : d) out.push_back(static_cast<i32>(x));
    return out;
}

f64 UsdCrateValue::number(usize k, f64 fallback) const {
    if (k < f.size()) return f[k];
    if (k < d.size()) return d[k];
    if (k < i.size()) return static_cast<f64>(i[k]);
    return fallback;
}

// ---- UsdCrate -----------------------------------------------------------------------------------

bool UsdCrate::loadFile(const std::string& path, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "USDC: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, "USDC: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, "USDC: short read on " + path);
    return loadMemory(std::move(bytes), why);
}

bool UsdCrate::loadMemory(std::vector<u8> bytes, std::string* why) {
    bytes_ = std::move(bytes);
    tokens_.clear(); strings_.clear(); fields_.clear(); fieldSets_.clear();
    paths_.clear(); specs_.clear(); specByPath_.clear(); tokenIndex_.clear();
    return parse(why);
}

std::string UsdCrate::version() const {
    return std::to_string(ver_[0]) + "." + std::to_string(ver_[1]) + "." + std::to_string(ver_[2]);
}

bool UsdCrate::parse(std::string* why) {
    if (bytes_.size() < 88 || std::memcmp(bytes_.data(), "PXR-USDC", 8) != 0)
        return fail(why, "USDC: no 'PXR-USDC' header");
    ver_[0] = bytes_[8]; ver_[1] = bytes_[9]; ver_[2] = bytes_[10];
    // 0.4.0 compressed the structural sections, and every reader path below assumes it.
    if (!versionAtLeast(ver_, 0, 4))
        return fail(why, "USDC: crate version " + version() + " predates 0.4.0 (uncompressed "
                         "structural sections), which this reader does not read -- re-save the "
                         "layer with a current USD, e.g. `usdcat -o new.usdc old.usd`");
    Rd r{bytes_.data(), bytes_.size()};
    r.seek(16);
    const i64 toc = r.get<i64>();
    if (!r.ok || toc < 0) return fail(why, "USDC: bad table-of-contents offset");
    r.seek(static_cast<u64>(toc));
    const u64 nsec = r.get<u64>();
    if (!r.ok || nsec > 64) return fail(why, "USDC: bad section count");

    struct Sec { u64 start = 0, size = 0; bool found = false; };
    Sec tokens, strings, fields, fieldSets, paths, specs;
    for (u64 k = 0; k < nsec; ++k) {
        const u8* name = r.take(16);
        const i64 start = r.get<i64>(), size = r.get<i64>();
        if (!r.ok || !name || start < 0 || size < 0) return fail(why, "USDC: truncated section table");
        char nm[17] = {};
        std::memcpy(nm, name, 16);
        const std::string s = nm;
        Sec* sec = s == "TOKENS" ? &tokens : s == "STRINGS" ? &strings : s == "FIELDS" ? &fields
                 : s == "FIELDSETS" ? &fieldSets : s == "PATHS" ? &paths : s == "SPECS" ? &specs : nullptr;
        if (sec) { sec->start = static_cast<u64>(start); sec->size = static_cast<u64>(size); sec->found = true; }
    }
    if (!tokens.found || !strings.found || !fields.found || !fieldSets.found || !paths.found || !specs.found)
        return fail(why, "USDC: a structural section is missing");

    // ---- TOKENS: count, uncompressed size, compressed size, then NUL-separated strings ----
    r.seek(tokens.start);
    {
        const u64 count = r.get<u64>(), raw = r.get<u64>(), cz = r.get<u64>();
        if (!r.ok || count > (u64(1) << 28) || raw > (u64(1) << 32) || cz > r.n)
            return fail(why, "USDC: bad TOKENS header");
        const u8* comp = r.take(static_cast<usize>(cz));
        std::vector<u8> text;
        if (!comp || !fastDecompress(comp, static_cast<usize>(cz), text, static_cast<usize>(raw)))
            return fail(why, "USDC: TOKENS did not decompress");
        tokens_.reserve(static_cast<usize>(count));
        usize s = 0;
        for (usize k = 0; k < text.size() && tokens_.size() < count; ++k)
            if (text[k] == 0) {
                tokens_.emplace_back(reinterpret_cast<const char*>(text.data() + s), k - s);
                s = k + 1;
            }
        if (tokens_.size() != count) return fail(why, "USDC: TOKENS count does not match its data");
        for (usize k = 0; k < tokens_.size(); ++k) tokenIndex_.emplace(tokens_[k], static_cast<u32>(k));
    }

    // ---- STRINGS: string index -> token index ----
    r.seek(strings.start);
    {
        const u64 count = r.get<u64>();
        if (!r.ok || count > r.n / 4) return fail(why, "USDC: bad STRINGS count");
        strings_.resize(static_cast<usize>(count));
        for (auto& s : strings_) s = r.get<u32>();
        if (!r.ok) return fail(why, "USDC: truncated STRINGS");
        for (const u32 s : strings_) if (s >= tokens_.size()) return fail(why, "USDC: a string names no token");
    }

    // ---- FIELDS: compressed name-token indices, then LZ4 value reps ----
    r.seek(fields.start);
    {
        const u64 count = r.get<u64>();
        if (!r.ok || count > r.n) return fail(why, "USDC: bad FIELDS count");
        std::vector<i64> names;
        if (!readCompressedInts(r, static_cast<usize>(count), names, false))
            return fail(why, "USDC: FIELDS names did not decompress");
        const u64 rz = r.get<u64>();
        const u8* comp = r.ok && rz <= r.n ? r.take(static_cast<usize>(rz)) : nullptr;
        std::vector<u8> reps;
        if (!comp || !fastDecompress(comp, static_cast<usize>(rz), reps, static_cast<usize>(count) * 8) ||
            reps.size() != count * 8)
            return fail(why, "USDC: FIELDS values did not decompress");
        fields_.resize(static_cast<usize>(count));
        for (usize k = 0; k < fields_.size(); ++k) {
            if (names[k] < 0 || static_cast<u64>(names[k]) >= tokens_.size())
                return fail(why, "USDC: a field names no token");
            fields_[k].first = static_cast<u32>(names[k]);
            std::memcpy(&fields_[k].second, reps.data() + k * 8, 8);
        }
    }

    // ---- FIELDSETS: runs of field indices, each ended by 0xFFFFFFFF ----
    r.seek(fieldSets.start);
    {
        const u64 count = r.get<u64>();
        if (!r.ok || count > r.n) return fail(why, "USDC: bad FIELDSETS count");
        std::vector<i64> v;
        if (!readCompressedInts(r, static_cast<usize>(count), v, false))
            return fail(why, "USDC: FIELDSETS did not decompress");
        fieldSets_.resize(v.size());
        for (usize k = 0; k < v.size(); ++k) fieldSets_[k] = static_cast<u32>(v[k]);
    }

    // ---- PATHS: the path tree, as parallel (path index, element token, jump) arrays ----
    //
    // Walked depth-first. A jump of -1 means "has a child, no sibling", 0 "has a sibling, no child",
    // >0 "both: the sibling is this many entries on", -2 "neither". A negative element token is a
    // PROPERTY of the current parent rather than a child prim. Iterative, with an explicit stack of
    // pending siblings, so a deep scene cannot exhaust the thread's stack.
    r.seek(paths.start);
    {
        const u64 numPaths = r.get<u64>();
        const u64 numEncoded = r.get<u64>();
        if (!r.ok || numPaths > r.n || numEncoded > r.n) return fail(why, "USDC: bad PATHS header");
        std::vector<i64> pidx, etok, jumps;
        if (!readCompressedInts(r, static_cast<usize>(numEncoded), pidx, false) ||
            !readCompressedInts(r, static_cast<usize>(numEncoded), etok, false) ||
            !readCompressedInts(r, static_cast<usize>(numEncoded), jumps, false))
            return fail(why, "USDC: PATHS did not decompress");
        paths_.assign(static_cast<usize>(numPaths), std::string{});
        struct Pending { usize index; std::string parent; bool root; };
        std::vector<Pending> stack;
        if (numEncoded) stack.push_back(Pending{0, std::string{}, true});
        usize visited = 0;
        while (!stack.empty()) {
            Pending t = std::move(stack.back());
            stack.pop_back();
            usize cur = t.index;
            std::string parent = std::move(t.parent);
            bool atRoot = t.root;
            for (;;) {
                if (cur >= numEncoded || ++visited > numEncoded) return fail(why, "USDC: malformed path tree");
                const usize self = cur++;
                const i64 pi = pidx[self];
                if (pi < 0 || static_cast<u64>(pi) >= numPaths) return fail(why, "USDC: a path index is out of range");
                std::string path;
                if (atRoot) {
                    path = "/";
                    atRoot = false;
                } else {
                    i64 ti = etok[self];
                    const bool prop = ti < 0;
                    if (prop) ti = -ti;
                    if (static_cast<u64>(ti) >= tokens_.size()) return fail(why, "USDC: a path element names no token");
                    const std::string& tok = tokens_[static_cast<usize>(ti)];
                    if (prop)                           path = parent + "." + tok;
                    else if (!tok.empty() && tok[0] == '{') path = parent + tok;   // variant selection
                    else if (parent == "/")             path = "/" + tok;
                    else                                path = parent + "/" + tok;
                }
                paths_[static_cast<usize>(pi)] = path;
                const i64 j = jumps[self];
                const bool child = j > 0 || j == -1;
                const bool sib = j >= 0;
                if (child) {
                    if (sib) stack.push_back(Pending{self + static_cast<usize>(j), parent, false});
                    parent = std::move(path);
                }
                if (!child && !sib) break;
            }
        }
    }

    // ---- SPECS: (path, field set, spec type) triples ----
    r.seek(specs.start);
    {
        const u64 count = r.get<u64>();
        if (!r.ok || count > r.n) return fail(why, "USDC: bad SPECS count");
        std::vector<i64> sp, sf, st;
        if (!readCompressedInts(r, static_cast<usize>(count), sp, false) ||
            !readCompressedInts(r, static_cast<usize>(count), sf, false) ||
            !readCompressedInts(r, static_cast<usize>(count), st, false))
            return fail(why, "USDC: SPECS did not decompress");
        specs_.resize(static_cast<usize>(count));
        specByPath_.reserve(specs_.size());
        for (usize k = 0; k < specs_.size(); ++k) {
            if (sp[k] < 0 || static_cast<u64>(sp[k]) >= paths_.size() ||
                sf[k] < 0 || static_cast<u64>(sf[k]) >= fieldSets_.size())
                return fail(why, "USDC: a spec points outside the path or field-set tables");
            specs_[k] = Spec{static_cast<u32>(sp[k]), static_cast<u32>(sf[k]),
                             static_cast<UsdSpecType>(st[k] & 0xFF)};
            specByPath_.emplace(paths_[static_cast<usize>(sp[k])], static_cast<i32>(k));
        }
    }
    return true;
}

i32 UsdCrate::specIndex(std::string_view path) const {
    const auto it = specByPath_.find(std::string(path));
    return it == specByPath_.end() ? -1 : it->second;
}

UsdSpecType UsdCrate::specType(i32 spec) const {
    return spec >= 0 && static_cast<usize>(spec) < specs_.size() ? specs_[static_cast<usize>(spec)].type
                                                                  : UsdSpecType::Unknown;
}

const std::string& UsdCrate::specPath(i32 spec) const {
    static const std::string kEmpty;
    return spec >= 0 && static_cast<usize>(spec) < specs_.size()
               ? paths_[specs_[static_cast<usize>(spec)].path] : kEmpty;
}

i32 UsdCrate::fieldRep(i32 spec, std::string_view name, u64& rep) const {
    if (spec < 0 || static_cast<usize>(spec) >= specs_.size()) return -1;
    const auto it = tokenIndex_.find(std::string(name));
    if (it == tokenIndex_.end()) return -1;
    for (usize k = specs_[static_cast<usize>(spec)].fieldSet; k < fieldSets_.size(); ++k) {
        const u32 fi = fieldSets_[k];
        if (fi == 0xFFFFFFFFu || fi >= fields_.size()) break;
        if (fields_[fi].first == it->second) { rep = fields_[fi].second; return 1; }
    }
    return -1;
}

bool UsdCrate::hasField(i32 spec, std::string_view name) const {
    u64 rep = 0;
    return fieldRep(spec, name, rep) > 0;
}

bool UsdCrate::field(i32 spec, std::string_view name, UsdCrateValue& out, std::string* why) const {
    u64 rep = 0;
    if (fieldRep(spec, name, rep) <= 0) { out = UsdCrateValue{}; return fail(why, "no such field"); }
    return decode(rep, out, why);
}

bool UsdCrate::decode(u64 rep, UsdCrateValue& v, std::string* why) const {
    v = UsdCrateValue{};
    const bool isArray = ((rep >> 63) & 1u) != 0;
    const bool inl     = ((rep >> 62) & 1u) != 0;
    const bool comp    = ((rep >> 61) & 1u) != 0;
    const u8 type      = static_cast<u8>((rep >> 48) & 0xFFu);
    const u64 payload  = rep & 0xFFFFFFFFFFFFull;
    v.type = type;
    v.isArray = isArray;
    const TypeInfo ti = typeInfo(type);
    v.components = ti.comps;

    const auto token = [&](u64 idx, std::string& into) {
        if (idx >= tokens_.size()) return false;
        into = tokens_[static_cast<usize>(idx)];
        return true;
    };
    const auto stringAt = [&](u64 idx, std::string& into) {
        return idx < strings_.size() && token(strings_[static_cast<usize>(idx)], into);
    };
    const auto pathAt = [&](u64 idx, std::string& into) {
        if (idx >= paths_.size()) return false;
        into = paths_[static_cast<usize>(idx)];
        return true;
    };
    const auto typeName = [&]() { return std::string("value type ") + std::to_string(type); };

    Rd r{bytes_.data(), bytes_.size()};

    // ---- arrays ----
    if (isArray) {
        if (payload == 0) return true;                      // an empty array
        r.seek(payload);
        const u64 count = versionAtLeast(ver_, 0, 7) ? r.get<u64>() : static_cast<u64>(r.get<u32>());
        if (!r.ok || count > r.n) return fail(why, "USDC: bad array count");
        const usize n = static_cast<usize>(count);
        const bool integral = type == UsdCrateType::Int || type == UsdCrateType::UInt ||
                              type == UsdCrateType::Int64 || type == UsdCrateType::UInt64;
        const bool floating = type == UsdCrateType::Half || type == UsdCrateType::Float ||
                              type == UsdCrateType::Double;
        if (integral && comp) {
            const bool wide = type == UsdCrateType::Int64 || type == UsdCrateType::UInt64;
            if (!readCompressedInts(r, n, v.i, wide)) return fail(why, "USDC: an integer array did not decompress");
            if (type == UsdCrateType::UInt) for (i64& x : v.i) x = static_cast<u32>(x);
            return true;
        }
        // Compressed floating arrays (0.6.0+): below 16 elements they are stored raw regardless of
        // the flag; above, a code byte says "whole numbers, packed as ints" ('i') or "a lookup table
        // of distinct values plus packed indices" ('t').
        if (floating && comp && versionAtLeast(ver_, 0, 6) && n >= 16) {
            const i8 code = r.get<i8>();
            std::vector<f64> vals;
            if (code == 'i') {
                std::vector<i64> ints;
                if (!readCompressedInts(r, n, ints, false)) return fail(why, "USDC: a float array did not decompress");
                vals.reserve(ints.size());
                for (const i64 x : ints) vals.push_back(static_cast<f64>(x));
            } else if (code == 't') {
                const u32 lutSize = r.get<u32>();
                UsdCrateValue lut;
                if (!r.ok || lutSize > r.n || !readRaw(r, ti, lutSize, lut)) return fail(why, "USDC: bad float lookup table");
                std::vector<i64> idx;
                if (!readCompressedInts(r, n, idx, false)) return fail(why, "USDC: float lookup indices did not decompress");
                const std::vector<f32> lf = lut.asFloats();
                vals.resize(n);
                for (usize k = 0; k < n; ++k) {
                    if (idx[k] < 0 || static_cast<usize>(idx[k]) >= lutSize) return fail(why, "USDC: float lookup index out of range");
                    vals[k] = lut.d.empty() ? static_cast<f64>(lf[static_cast<usize>(idx[k])]) : lut.d[static_cast<usize>(idx[k])];
                }
            } else {
                return fail(why, "USDC: unknown float-array compression code");
            }
            if (type == UsdCrateType::Double) v.d = std::move(vals);
            else { v.f.resize(vals.size()); for (usize k = 0; k < vals.size(); ++k) v.f[k] = static_cast<f32>(vals[k]); }
            return true;
        }
        if (ti.kind == kToken || ti.kind == kString) {
            v.s.resize(n);
            for (usize k = 0; k < n; ++k) {
                const u32 idx = r.get<u32>();
                if (!r.ok || !(ti.kind == kToken ? token(idx, v.s[k]) : stringAt(idx, v.s[k])))
                    return fail(why, "USDC: bad token/string array");
            }
            return true;
        }
        if (ti.kind == kNone) return fail(why, "USDC: arrays of " + typeName() + " are not decoded");
        if (!readRaw(r, ti, n, v)) return fail(why, "USDC: truncated array");
        return true;
    }

    // ---- inlined scalars: the value lives in the rep's own 48 payload bits ----
    if (inl) {
        const u32 lo = static_cast<u32>(payload);
        using namespace UsdCrateType;
        switch (type) {
            case Bool: case UChar: case UInt: case UInt64:
            case Specifier: case Permission: case Variability:
                v.i = {static_cast<i64>(lo)}; return true;
            case Int: case Int64:
                v.i = {static_cast<i64>(static_cast<i32>(lo))}; return true;
            case Half:   v.f = {halfToFloat(static_cast<u16>(lo))}; return true;
            case Float:  { f32 x; std::memcpy(&x, &lo, 4); v.f = {x}; return true; }
            // A double is inlined only when a float holds it exactly, and is stored as that float.
            case Double: case TimeCode: { f32 x; std::memcpy(&x, &lo, 4); v.d = {static_cast<f64>(x)}; return true; }
            case Token: case AssetPath: v.s.resize(1); return token(lo, v.s[0]) || fail(why, "USDC: bad token");
            case String: v.s.resize(1); return stringAt(lo, v.s[0]) || fail(why, "USDC: bad string");
            case ValueBlock: return true;
            default: break;
        }
        // Vectors whose components are all small integers are inlined as one int8 per component.
        // Matrices likewise, but only their DIAGONAL is stored -- an inlined matrix is diagonal.
        if (type >= Vec2d && type <= Vec4i) {
            u8 b[8];
            std::memcpy(b, &payload, 8);
            for (u32 k = 0; k < ti.comps; ++k) {
                const f64 x = static_cast<i8>(b[k]);
                if (ti.kind == kDouble) v.d.push_back(x);
                else if (ti.kind == kInt) v.i.push_back(static_cast<i64>(x));
                else v.f.push_back(static_cast<f32>(x));
            }
            return true;
        }
        if (type == Matrix2d || type == Matrix3d || type == Matrix4d) {
            const u32 dim = type == Matrix2d ? 2u : type == Matrix3d ? 3u : 4u;
            u8 b[8];
            std::memcpy(b, &payload, 8);
            v.d.assign(dim * dim, 0.0);
            for (u32 k = 0; k < dim; ++k) v.d[k * dim + k] = static_cast<i8>(b[k]);
            return true;
        }
        return fail(why, "USDC: inlined " + typeName() + " is not decoded");
    }

    // ---- out-of-line values ----
    r.seek(payload);
    using namespace UsdCrateType;
    switch (type) {
        case Int64: case UInt64: case Double: case TimeCode:
        case Matrix2d: case Matrix3d: case Matrix4d: case Quatd: case Quatf: case Quath:
        case Vec2d: case Vec2f: case Vec2h: case Vec2i: case Vec3d: case Vec3f: case Vec3h: case Vec3i:
        case Vec4d: case Vec4f: case Vec4h: case Vec4i:
        case Half: case Float: case Int: case UInt: case Bool: case UChar:
            return readRaw(r, ti, 1, v) || fail(why, "USDC: truncated value");
        case Token: case AssetPath: {
            v.s.resize(1);
            return (token(r.get<u32>(), v.s[0]) && r.ok) || fail(why, "USDC: bad token");
        }
        case String: {
            v.s.resize(1);
            return (stringAt(r.get<u32>(), v.s[0]) && r.ok) || fail(why, "USDC: bad string");
        }
        case TokenVector: case StringVector: case PathVector: {
            const u64 n = r.get<u64>();
            if (!r.ok || n > r.n / 4) return fail(why, "USDC: bad vector count");
            v.s.resize(static_cast<usize>(n));
            for (auto& s : v.s) {
                const u32 idx = r.get<u32>();
                const bool ok = type == TokenVector ? token(idx, s) : type == StringVector ? stringAt(idx, s) : pathAt(idx, s);
                if (!ok || !r.ok) return fail(why, "USDC: bad vector element");
            }
            return true;
        }
        case DoubleVector: {
            const u64 n = r.get<u64>();
            if (!r.ok || n > r.n / 8) return fail(why, "USDC: bad vector count");
            return readRaw(r, TypeInfo{kDouble, 1, 8}, static_cast<usize>(n), v) || fail(why, "USDC: truncated vector");
        }
        case TokenListOp: case StringListOp: case PathListOp:
        case ReferenceListOp: case PayloadListOp: {
            // ListOpHeader bits: 1 explicit, 2 has explicit items, 4 added, 8 deleted, 16 ordered,
            // 32 prepended, 64 appended -- read in the order explicit, added, prepended, appended,
            // deleted, ordered. The composed result keeps explicit, prepended, added and appended
            // items in that order; deleted and ordered only reorder or remove opinions from WEAKER
            // layers, which a single layer's reader has none of.
            const u8 h = r.get<u8>();
            if (!r.ok) return fail(why, "USDC: truncated list op");
            std::vector<std::string> expl, added, prep, app;
            std::vector<UsdCrateRef> rexpl, radded, rprep, rapp;
            const auto readList = [&](std::vector<std::string>& items, std::vector<UsdCrateRef>& refs) {
                const u64 n = r.get<u64>();
                if (!r.ok || n > r.n) return false;
                for (u64 k = 0; k < n; ++k) {
                    if (type == ReferenceListOp || type == PayloadListOp) {
                        UsdCrateRef ref;
                        if (!stringAt(r.get<u32>(), ref.assetPath) || !pathAt(r.get<u32>(), ref.primPath)) return false;
                        if (ref.primPath == "/") ref.primPath.clear();
                        // SdfLayerOffset (offset, scale). A payload only carries one from 0.8.0.
                        if (type == ReferenceListOp || versionAtLeast(ver_, 0, 8)) { r.get<f64>(); r.get<f64>(); }
                        if (type == ReferenceListOp) {
                            // customData. An EMPTY dictionary is a bare zero count; a populated one
                            // stores nested values this reader does not decode, and stepping past
                            // it without decoding would lose sync with the rest of the list.
                            const u64 dictCount = r.get<u64>();
                            if (dictCount != 0) return false;
                        }
                        if (!r.ok) return false;
                        refs.push_back(std::move(ref));
                    } else {
                        std::string s;
                        const u32 idx = r.get<u32>();
                        const bool ok = type == TokenListOp ? token(idx, s) : type == StringListOp ? stringAt(idx, s) : pathAt(idx, s);
                        if (!ok || !r.ok) return false;
                        items.push_back(std::move(s));
                    }
                }
                return true;
            };
            bool ok = true;
            if (ok && (h & 2))  ok = readList(expl, rexpl);
            if (ok && (h & 4))  ok = readList(added, radded);
            if (ok && (h & 32)) ok = readList(prep, rprep);
            if (ok && (h & 64)) ok = readList(app, rapp);
            if (!ok) return fail(why, type == ReferenceListOp
                                          ? "USDC: a reference carries customData, which is not decoded"
                                          : "USDC: bad list op");
            for (auto* l : {&expl, &prep, &added, &app}) v.s.insert(v.s.end(), l->begin(), l->end());
            for (auto* l : {&rexpl, &rprep, &radded, &rapp}) v.refs.insert(v.refs.end(), l->begin(), l->end());
            return true;
        }
        case Payload: {
            UsdCrateRef ref;
            if (!stringAt(r.get<u32>(), ref.assetPath) || !pathAt(r.get<u32>(), ref.primPath) || !r.ok)
                return fail(why, "USDC: bad payload");
            if (ref.primPath == "/") ref.primPath.clear();
            v.refs.push_back(std::move(ref));
            return true;
        }
        case VariantSelectionMap: {   // map<string, string>: count, then (key, value) string indices
            const u64 n = r.get<u64>();
            if (!r.ok || n > r.n / 8) return fail(why, "USDC: bad variant selection map");
            for (u64 k = 0; k < n; ++k) {
                std::string key, val;
                if (!stringAt(r.get<u32>(), key) || !stringAt(r.get<u32>(), val) || !r.ok)
                    return fail(why, "USDC: bad variant selection map");
                v.s.push_back(std::move(key));
                v.s.push_back(std::move(val));
            }
            return true;
        }
        case TimeSamples:
            return fail(why, "USDC: time-sampled value (not decoded; use the attribute's default)");
        case Dictionary:
            return fail(why, "USDC: dictionaries are not decoded");
        default:
            return fail(why, "USDC: " + typeName() + " is not decoded");
    }
}

} // namespace aver::fmt
