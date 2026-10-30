#pragma once
// Builders for tiny hand-made Godot 4 binary resources (.res / .scn / RSCC), shared by GodotBinaryTest and FabricBinaryTest. Nothing here is a
// copy of a real file: the layout is written out field by field from the format description in GodotBinary.hpp.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace gbfix {

using namespace aver;
using Bytes = std::vector<u8>;

// Variant type tags of the binary format.
enum : u32 {
    kNil = 1, kBool = 2, kInt = 3, kFloat = 4, kString = 5, kVector2 = 10, kRect2 = 11, kVector3 = 12, kPlane = 13, kQuaternion = 14, kAabb = 15,
    kBasis = 16, kTransform3D = 17, kTransform2D = 18, kColor = 20, kNodePath = 22, kRid = 23, kObject = 24, kDictionary = 26, kArray = 30,
    kPackedByte = 31, kPackedInt32 = 32, kPackedFloat32 = 33, kPackedString = 34, kPackedVector3 = 35, kPackedColor = 36, kPackedVector2 = 37,
    kInt64 = 40, kDouble = 41, kCallable = 42, kStringName = 44, kVector2i = 45, kRect2i = 46, kVector3i = 47, kPackedInt64 = 48,
    kPackedFloat64 = 49, kVector4 = 50, kVector4i = 51, kProjection = 52, kPackedVector4 = 53,
};

struct Enc {
    Bytes b;
    bool be = false;
    bool real64 = false;
    void raw(const void* p, usize n) {
        const u8* q = static_cast<const u8*>(p);
        b.insert(b.end(), q, q + n);
    }
    void u16v(u16 v) {
        if (be) { b.push_back(u8(v >> 8)); b.push_back(u8(v)); }
        else { b.push_back(u8(v)); b.push_back(u8(v >> 8)); }
    }
    void u32v(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (be ? 24 - 8 * i : 8 * i)));
    }
    void u64v(u64 v) {
        for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (be ? 56 - 8 * i : 8 * i)));
    }
    void i32v(i32 v) { u32v(u32(v)); }
    void i64v(i64 v) { u64v(u64(v)); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
    void f64v(f64 v) { u64 x; std::memcpy(&x, &v, 8); u64v(x); }
    void real(f64 v) { if (real64) f64v(v); else f32v(f32(v)); }
    void str(const std::string& s) {
        u32v(u32(s.size() + 1));
        raw(s.data(), s.size());
        b.push_back(0);
    }
};

// Variant encoders in one file's byte order and real width.
struct Fmt {
    bool be = false, real64 = false;

    Enc enc() const { Enc e; e.be = be; e.real64 = real64; return e; }

    Bytes nil() const { Enc e = enc(); e.u32v(kNil); return e.b; }
    Bytes boolean(bool v) const { Enc e = enc(); e.u32v(kBool); e.u32v(v ? 1 : 0); return e.b; }
    Bytes integer(i32 v) const { Enc e = enc(); e.u32v(kInt); e.i32v(v); return e.b; }
    Bytes int64(i64 v) const { Enc e = enc(); e.u32v(kInt64); e.i64v(v); return e.b; }
    Bytes flt(f32 v) const { Enc e = enc(); e.u32v(kFloat); e.f32v(v); return e.b; }
    Bytes dbl(f64 v) const { Enc e = enc(); e.u32v(kDouble); e.f64v(v); return e.b; }
    Bytes text(const std::string& s, u32 tag = kString) const { Enc e = enc(); e.u32v(tag); e.str(s); return e.b; }
    Bytes reals(u32 tag, std::initializer_list<f64> v) const { Enc e = enc(); e.u32v(tag); for (f64 x : v) e.real(x); return e.b; }
    Bytes ints(u32 tag, std::initializer_list<i32> v) const { Enc e = enc(); e.u32v(tag); for (i32 x : v) e.i32v(x); return e.b; }
    Bytes color(f32 r, f32 g, f32 b, f32 a) const { Enc e = enc(); e.u32v(kColor); e.f32v(r); e.f32v(g); e.f32v(b); e.f32v(a); return e.b; }
    // names / subnames are string-table indices (FileDef::sidx).
    Bytes nodePath(std::vector<u32> names, std::vector<u32> subs, bool absolute) const {
        Enc e = enc();
        e.u32v(kNodePath);
        e.u16v(u16(names.size()));
        e.u16v(u16(subs.size() | (absolute ? 0x8000u : 0u)));
        for (u32 n : names) e.u32v(n);
        for (u32 n : subs) e.u32v(n);
        return e.b;
    }
    Bytes rid(u32 v) const { Enc e = enc(); e.u32v(kRid); e.u32v(v); return e.b; }
    Bytes objectNone() const { Enc e = enc(); e.u32v(kObject); e.u32v(0); return e.b; }
    Bytes extRef(u32 idx) const { Enc e = enc(); e.u32v(kObject); e.u32v(3); e.u32v(idx); return e.b; }
    Bytes subRef(u32 idx) const { Enc e = enc(); e.u32v(kObject); e.u32v(2); e.u32v(idx); return e.b; }
    Bytes tag(u32 t) const { Enc e = enc(); e.u32v(t); return e.b; }   // Callable: no payload
    Bytes dict(const std::vector<std::pair<Bytes, Bytes>>& kv) const {
        Enc e = enc();
        e.u32v(kDictionary);
        e.u32v(u32(kv.size()));
        for (const auto& p : kv) { e.raw(p.first.data(), p.first.size()); e.raw(p.second.data(), p.second.size()); }
        return e.b;
    }
    Bytes array(const std::vector<Bytes>& items) const {
        Enc e = enc();
        e.u32v(kArray);
        e.u32v(u32(items.size()));
        for (const Bytes& i : items) e.raw(i.data(), i.size());
        return e.b;
    }
    Bytes packedBytes(const Bytes& v) const {
        Enc e = enc();
        e.u32v(kPackedByte);
        e.u32v(u32(v.size()));
        e.raw(v.data(), v.size());
        for (usize i = 0; i < ((4 - (v.size() & 3)) & 3); ++i) e.b.push_back(0);
        return e.b;
    }
    Bytes packedI32(const std::vector<i32>& v) const { Enc e = enc(); e.u32v(kPackedInt32); e.u32v(u32(v.size())); for (i32 x : v) e.i32v(x); return e.b; }
    Bytes packedI64(const std::vector<i64>& v) const { Enc e = enc(); e.u32v(kPackedInt64); e.u32v(u32(v.size())); for (i64 x : v) e.i64v(x); return e.b; }
    Bytes packedF32(const std::vector<f32>& v) const { Enc e = enc(); e.u32v(kPackedFloat32); e.u32v(u32(v.size())); for (f32 x : v) e.f32v(x); return e.b; }
    Bytes packedF64(const std::vector<f64>& v) const { Enc e = enc(); e.u32v(kPackedFloat64); e.u32v(u32(v.size())); for (f64 x : v) e.f64v(x); return e.b; }
    Bytes packedStrings(const std::vector<std::string>& v) const { Enc e = enc(); e.u32v(kPackedString); e.u32v(u32(v.size())); for (const std::string& s : v) e.str(s); return e.b; }
    // PackedVector2/3/4Array: `tag` picks it, the numbers are real-width.
    Bytes packedReals(u32 tag, const std::vector<f64>& v) const { Enc e = enc(); e.u32v(tag); e.u32v(u32(v.size() / (tag == kPackedVector2 ? 2 : (tag == kPackedVector3 ? 3 : 4)))); for (f64 x : v) e.real(x); return e.b; }
    Bytes packedColors(const std::vector<f32>& v) const { Enc e = enc(); e.u32v(kPackedColor); e.u32v(u32(v.size() / 4)); for (f32 x : v) e.f32v(x); return e.b; }
};

struct FileDef {
    Fmt f;
    u32 major = 4, minor = 7, format = 6, flags = 3;   // named scene ids + uids
    u64 uid = 0x6f607bd40182c27fULL;
    std::string cls, scriptClass;
    std::vector<std::string> strings;
    struct Ext { std::string type, path; u64 uid = 0; };
    std::vector<Ext> ext;
    struct Res {
        std::string path, type;
        std::vector<std::pair<u32, Bytes>> props;
    };
    std::vector<Res> res;   // the last one is the main resource

    u32 sidx(const std::string& s) {
        for (usize i = 0; i < strings.size(); ++i)
            if (strings[i] == s) return u32(i);
        strings.push_back(s);
        return u32(strings.size() - 1);
    }
    void prop(Res& r, const std::string& name, const Bytes& v) { r.props.emplace_back(sidx(name), v); }

    // With magic=false the stream starts at the endian flag (what an RSCC file holds); offsets are positions in the stream either way.
    Bytes build(bool magic = true) const {
        Enc e = f.enc();
        if (magic) e.raw("RSRC", 4);
        e.b.push_back(f.be ? 1 : 0);
        e.b.push_back(0); e.b.push_back(0); e.b.push_back(0);   // the endian flag is little-endian whatever follows
        e.u32v(f.real64 ? 1 : 0);
        e.u32v(major);
        e.u32v(minor);
        e.u32v(format);
        e.str(cls);
        e.u64v(0);
        e.u32v(flags | (f.real64 ? 4u : 0u) | (scriptClass.empty() ? 0u : 8u));
        e.u64v(uid);
        if (!scriptClass.empty()) e.str(scriptClass);
        for (int i = 0; i < 11; ++i) e.u32v(0);
        e.u32v(u32(strings.size()));
        for (const std::string& s : strings) e.str(s);
        e.u32v(u32(ext.size()));
        for (const Ext& x : ext) {
            e.str(x.type);
            e.str(x.path);
            if (flags & 2u) e.u64v(x.uid);
        }
        e.u32v(u32(res.size()));
        std::vector<usize> patch;
        for (const Res& r : res) {
            e.str(r.path);
            patch.push_back(e.b.size());
            e.u64v(0);
        }
        for (usize i = 0; i < res.size(); ++i) {
            Enc off = f.enc();
            off.u64v(e.b.size());
            std::memcpy(&e.b[patch[i]], off.b.data(), 8);
            e.str(res[i].type);
            e.u32v(u32(res[i].props.size()));
            for (const auto& p : res[i].props) {
                e.u32v(p.first);
                e.raw(p.second.data(), p.second.size());
            }
        }
        e.raw("RSRC", 4);
        return e.b;
    }
};

// A Zstandard frame holding `n` bytes as one raw block (n < 128 KiB): valid, and enough to exercise the RSCC container.
inline Bytes zstdRawFrame(const u8* p, usize n) {
    Bytes o = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};   // magic, single segment + 4-byte content size
    for (int i = 0; i < 4; ++i) o.push_back(u8(n >> (8 * i)));
    const u32 bh = u32(n << 3) | 1u;            // last block, raw
    for (int i = 0; i < 3; ++i) o.push_back(u8(bh >> (8 * i)));
    o.insert(o.end(), p, p + n);
    return o;
}

// "RSCC": mode, block size, total size, one compressed size per block, the blocks; Godot's footer after them. `stream` is a file built without its magic.
inline Bytes wrapRscc(const Bytes& stream, u32 block, u32 mode = 2) {
    Bytes o;
    Enc e;
    e.raw("RSCC", 4);
    e.u32v(mode);
    e.u32v(block);
    e.u32v(u32(stream.size()));
    const usize blocks = stream.size() / block + 1;
    std::vector<Bytes> frames;
    for (usize i = 0; i < blocks; ++i) {
        const usize from = i * block;
        const usize n = from >= stream.size() ? 0 : std::min<usize>(block, stream.size() - from);
        frames.push_back(zstdRawFrame(stream.data() + from, n));
        e.u32v(u32(frames.back().size()));
    }
    for (const Bytes& fr : frames) e.raw(fr.data(), fr.size());
    e.raw("GCMP", 4);
    return e.b;
}

// ---- PackedScene -------------------------------------------------------------------------------------------------------------

struct SceneDef {
    std::vector<std::string> names;
    std::vector<Bytes> variants;
    std::vector<i32> nodes;       // the flat node table
    i32 nodeCount = 0;
    std::vector<i32> nodeIds;
    std::vector<i32> conns;
    i32 connCount = 0;
    std::vector<Bytes> nodePaths;       // NodePath variants
    std::vector<Bytes> editable;
    i32 baseScene = -1;
};

// The "_bundled" dictionary, with the keys in the order Godot writes them.
inline Bytes bundled(const Fmt& f, const SceneDef& s) {
    std::vector<std::pair<Bytes, Bytes>> kv;
    auto key = [&](const char* k) { return f.text(k); };
    kv.emplace_back(key("names"), f.packedStrings(s.names));
    kv.emplace_back(key("variants"), f.array(s.variants));
    kv.emplace_back(key("node_count"), f.integer(s.nodeCount));
    kv.emplace_back(key("nodes"), f.packedI32(s.nodes));
    kv.emplace_back(key("node_ids"), f.packedI32(s.nodeIds));
    kv.emplace_back(key("conn_count"), f.integer(s.connCount));
    kv.emplace_back(key("conns"), f.packedI32(s.conns));
    kv.emplace_back(key("node_paths"), f.array(s.nodePaths));
    kv.emplace_back(key("editable_instances"), f.array(s.editable));
    if (s.baseScene >= 0) kv.emplace_back(key("base_scene"), f.integer(s.baseScene));
    kv.emplace_back(key("version"), f.integer(3));
    return f.dict(kv);
}

// Appends the PackedScene as the file's main (last) resource. Build the NodePath variants with d.sidx first.
inline void addMainScene(FileDef& d, const SceneDef& s) {
    d.cls = "PackedScene";
    FileDef::Res r;
    r.path = "local://PackedScene_test1";
    r.type = "PackedScene";
    d.res.push_back(r);
    d.prop(d.res.back(), "_bundled", bundled(d.f, s));
}

}  // namespace gbfix
