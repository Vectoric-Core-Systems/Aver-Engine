// Godot 4 binary resources (RSRC / RSCC) into the text parsers' document models. See GodotBinary.hpp for the format and the contract.
#include "aver/formats/godot/GodotBinary.hpp"

#include "GodotSceneInternal.hpp"
#include "aver/formats/godot/GodotZstd.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace aver::fmt::godot {

namespace {

constexpr int kMaxDepth = 64;
constexpr usize kMaxValues = 500'000;   // Variants (array / dictionary elements included) one file may hold; each costs a few hundred bytes of memory
constexpr int kLastKnownFormat = 6;
constexpr u32 kBlockLimit = 1u << 24;

// The PackedScene node table's flag bits.
constexpr i32 kTypeInstantiated = 0x7FFFFFFF;
constexpr i32 kFlagIdIsPath = 1 << 30;
constexpr i32 kFlagInstancePlaceholder = 1 << 30;
constexpr i32 kFlagPathPropertyIsNodePath = 1 << 30;
constexpr i32 kNameMask = (1 << 18) - 1;

const char* const kCode = "godot.binary_resource";

// Variant type tags of the binary format.
enum Tag : u32 {
    kNil = 1, kBool = 2, kInt = 3, kFloat = 4, kString = 5, kVector2 = 10, kRect2 = 11, kVector3 = 12, kPlane = 13, kQuaternion = 14, kAabb = 15,
    kBasis = 16, kTransform3D = 17, kTransform2D = 18, kColor = 20, kNodePath = 22, kRid = 23, kObject = 24, kInputEvent = 25, kDictionary = 26,
    kArray = 30, kPackedByte = 31, kPackedInt32 = 32, kPackedFloat32 = 33, kPackedString = 34, kPackedVector3 = 35, kPackedColor = 36,
    kPackedVector2 = 37, kInt64 = 40, kDouble = 41, kCallable = 42, kSignal = 43, kStringName = 44, kVector2i = 45, kRect2i = 46, kVector3i = 47,
    kPackedInt64 = 48, kPackedFloat64 = 49, kVector4 = 50, kVector4i = 51, kProjection = 52, kPackedVector4 = 53,
};

// ---- the byte cursor and the Variant reader ---------------------------------------------------------------------------------

class Reader {
public:
    Reader(const u8* d, usize n) : d_(d), n_(n) {}

    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }
    usize pos() const { return pos_; }
    usize size() const { return n_; }
    usize left() const { return n_ - pos_; }
    void setBigEndian(bool be) { be_ = be; }
    void fail(const std::string& m) {
        if (!ok_) return;
        ok_ = false;
        why_ = m + " (at byte " + std::to_string(pos_) + ")";
    }
    bool seek(u64 p) {
        if (p > n_) {
            fail("an offset points past the end of the file");
            return false;
        }
        pos_ = usize(p);
        return true;
    }
    bool need(u64 k) {
        if (!ok_) return false;
        if (k > left()) {
            fail("the file ends early");
            return false;
        }
        return true;
    }

    u8 u8v() { return need(1) ? d_[pos_++] : 0; }
    u16 u16v() {
        if (!need(2)) return 0;
        u16 v = u16(d_[pos_] | (d_[pos_ + 1] << 8));
        pos_ += 2;
        return be_ ? u16((v >> 8) | (v << 8)) : v;
    }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = u32(d_[pos_]) | (u32(d_[pos_ + 1]) << 8) | (u32(d_[pos_ + 2]) << 16) | (u32(d_[pos_ + 3]) << 24);
        pos_ += 4;
        return be_ ? swap32(v) : v;
    }
    u32 u32raw() {  // the endian flag itself is always little-endian
        const bool be = be_;
        be_ = false;
        const u32 v = u32v();
        be_ = be;
        return v;
    }
    u64 u64v() {
        const u32 a = u32v();
        const u32 b = u32v();
        return be_ ? (u64(a) << 32) | b : (u64(b) << 32) | a;
    }
    i32 i32v() { return i32(u32v()); }
    i64 i64v() { return i64(u64v()); }
    f32 f32v() {
        const u32 b = u32v();
        f32 f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    f64 f64v() {
        const u64 b = u64v();
        f64 f;
        std::memcpy(&f, &b, 8);
        return f;
    }
    f64 real() { return real64_ ? f64v() : f64(f32v()); }

    // A length-prefixed string: u32 byte count including the NUL, then the bytes.
    bool str(std::string& out) {
        const u32 len = u32v();
        if (!need(len)) return false;
        usize l = len;
        while (l > 0 && d_[pos_ + l - 1] == 0) --l;
        out.assign(reinterpret_cast<const char*>(d_ + pos_), l);
        pos_ += len;
        return true;
    }
    // A string-table reference (a NodePath's names, a property name): an index, or an inline string when the top bit is set.
    bool stringRef(std::string& out) {
        const u32 id = u32v();
        if (!ok_) return false;
        if (id & 0x80000000u) {
            const u32 len = id & 0x7FFFFFFFu;
            if (!need(len)) return false;
            out.assign(reinterpret_cast<const char*>(d_ + pos_), len);
            pos_ += len;
            return true;
        }
        if (id >= strings.size()) {
            fail("a string index (" + std::to_string(id) + ") is past the string table");
            return false;
        }
        out = strings[id];
        return true;
    }

    bool variant(GodotVariant& v, int depth);

    std::vector<std::string> strings;
    std::vector<std::string> extIds;
    std::vector<std::string> subIds;
    bool real64_ = false;

private:
    static u32 swap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
    bool nums(GodotVariant& v, GodotVType t, int count);
    bool intNums(GodotVariant& v, GodotVType t, int count);
    bool packedNums(GodotVariant& v, GodotVType t, int stride, int elem);   // elem: 4 = f32, 8 = f64, -4 = i32, -8 = i64

    const u8* d_;
    usize n_;
    usize pos_ = 0;
    bool ok_ = true;
    bool be_ = false;
    std::string why_;
    usize values_ = 0;
};

bool Reader::nums(GodotVariant& v, GodotVType t, int count) {
    v.type = t;
    if (!need(u64(count) * (real64_ ? 8 : 4))) return false;
    v.nums.reserve(usize(count));
    for (int i = 0; i < count; ++i) v.nums.push_back(real());
    return ok_;
}

bool Reader::intNums(GodotVariant& v, GodotVType t, int count) {
    v.type = t;
    if (!need(u64(count) * 4)) return false;
    for (int i = 0; i < count; ++i) v.nums.push_back(f64(i32v()));
    return ok_;
}

// A packed array of numbers: u32 count, then count * stride elements.
bool Reader::packedNums(GodotVariant& v, GodotVType t, int stride, int elem) {
    v.type = t;
    const u32 len = u32v();
    if (!ok_) return false;
    const u64 width = elem < 0 ? u64(-elem) : u64(elem);
    if (u64(len) * u64(stride) * width > left()) {
        fail("a packed array is longer than the file");
        return false;
    }
    const usize total = usize(len) * usize(stride);
    v.nums.reserve(total);
    for (usize i = 0; i < total; ++i) {
        switch (elem) {
            case 4: v.nums.push_back(f64(f32v())); break;
            case 8: v.nums.push_back(f64v()); break;
            case -4: v.nums.push_back(f64(i32v())); break;
            default: v.nums.push_back(f64(i64v())); break;
        }
    }
    return ok_;
}

bool Reader::variant(GodotVariant& v, int depth) {
    if (!ok_) return false;
    if (depth > kMaxDepth) {
        fail("values nest too deeply");
        return false;
    }
    if (++values_ > kMaxValues) {
        fail("the file holds too many values");
        return false;
    }
    const u32 tag = u32v();
    if (!ok_) return false;
    switch (tag) {
        case kNil: v.type = GodotVType::Nil; return true;
        case kBool: v.type = GodotVType::Bool; v.b = u32v() != 0; return ok_;
        case kInt: v.type = GodotVType::Int; v.i = i32v(); return ok_;
        case kInt64: v.type = GodotVType::Int; v.i = i64v(); return ok_;
        case kFloat: v.type = GodotVType::Float; v.f = f32v(); return ok_;
        case kDouble: v.type = GodotVType::Float; v.f = f64v(); return ok_;
        case kString: v.type = GodotVType::String; return str(v.s);
        case kStringName: v.type = GodotVType::StringName; return str(v.s);
        case kVector2: return nums(v, GodotVType::Vector2, 2);
        case kRect2: return nums(v, GodotVType::Rect2, 4);
        case kVector3: return nums(v, GodotVType::Vector3, 3);
        case kPlane: return nums(v, GodotVType::Plane, 4);
        case kQuaternion: return nums(v, GodotVType::Quaternion, 4);
        case kAabb: return nums(v, GodotVType::AABB, 6);
        case kBasis: return nums(v, GodotVType::Basis, 9);
        case kTransform3D: return nums(v, GodotVType::Transform3D, 12);
        case kTransform2D: return nums(v, GodotVType::Transform2D, 6);
        case kVector4: return nums(v, GodotVType::Vector4, 4);
        case kProjection: return nums(v, GodotVType::Projection, 16);
        case kColor: {
            v.type = GodotVType::Color;
            if (!need(16)) return false;
            for (int i = 0; i < 4; ++i) v.nums.push_back(f64(f32v()));
            return ok_;
        }
        case kVector2i: return intNums(v, GodotVType::Vector2i, 2);
        case kRect2i: return intNums(v, GodotVType::Rect2i, 4);
        case kVector3i: return intNums(v, GodotVType::Vector3i, 3);
        case kVector4i: return intNums(v, GodotVType::Vector4i, 4);
        case kNodePath: {
            const u32 nameCount = u16v();
            u32 subCount = u16v();
            const bool absolute = (subCount & 0x8000u) != 0;
            subCount &= 0x7FFFu;
            std::string s = absolute ? "/" : "";
            for (u32 i = 0; i < nameCount && ok_; ++i) {
                std::string part;
                if (!stringRef(part)) return false;
                if (i) s += '/';
                s += part;
            }
            for (u32 i = 0; i < subCount && ok_; ++i) {
                std::string part;
                if (!stringRef(part)) return false;
                s += ':';
                s += part;
            }
            v.type = GodotVType::NodePath;
            v.s = std::move(s);
            return ok_;
        }
        case kRid: v.type = GodotVType::Rid; v.i = u32v(); return ok_;
        case kCallable: v.type = GodotVType::Callable; return true;   // Godot writes no payload for these two
        case kSignal: v.type = GodotVType::Signal; return true;
        case kObject: {
            const u32 sub = u32v();
            if (!ok_) return false;
            switch (sub) {
                case 0: v.type = GodotVType::Nil; return true;
                case 1: {   // an external resource written out in full (older files): type, then path
                    std::string type, path;
                    if (!str(type) || !str(path)) return false;
                    v.type = GodotVType::Resource;
                    v.s = std::move(path);
                    return true;
                }
                case 2: {
                    const u32 idx = u32v();
                    if (!ok_) return false;
                    if (idx >= subIds.size()) {
                        fail("an internal resource index (" + std::to_string(idx) + ") is past the resource table");
                        return false;
                    }
                    v.type = GodotVType::SubResource;
                    v.s = subIds[idx];
                    return true;
                }
                case 3: {
                    const u32 idx = u32v();
                    if (!ok_) return false;
                    if (idx >= extIds.size()) {
                        fail("an external resource index (" + std::to_string(idx) + ") is past the resource table");
                        return false;
                    }
                    v.type = GodotVType::ExtResource;
                    v.s = extIds[idx];
                    return true;
                }
                default: fail("unknown object kind " + std::to_string(sub)); return false;
            }
        }
        case kDictionary: {
            const u32 count = u32v() & 0x7FFFFFFFu;   // the top bit marks a shared dictionary
            if (!ok_) return false;
            if (u64(count) * 8 > left()) {
                fail("a dictionary is longer than the file");
                return false;
            }
            v.type = GodotVType::Dictionary;
            v.keys.reserve(count);
            v.items.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                GodotVariant k, val;
                if (!variant(k, depth + 1) || !variant(val, depth + 1)) return false;
                v.keys.push_back(std::move(k));
                v.items.push_back(std::move(val));
            }
            return true;
        }
        case kArray: {
            const u32 count = u32v() & 0x7FFFFFFFu;
            if (!ok_) return false;
            if (u64(count) * 4 > left()) {
                fail("an array is longer than the file");
                return false;
            }
            v.type = GodotVType::Array;
            v.items.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                GodotVariant e;
                if (!variant(e, depth + 1)) return false;
                v.items.push_back(std::move(e));
            }
            return true;
        }
        case kPackedByte: {
            const u32 len = u32v();
            if (!need(len)) return false;
            v.type = GodotVType::PackedByteArray;
            v.bytes.assign(d_ + pos_, d_ + pos_ + len);
            pos_ += len;
            const u32 pad = (4 - (len & 3)) & 3;   // byte arrays are padded to a 4-byte boundary
            if (!need(pad)) return false;
            pos_ += pad;
            return true;
        }
        case kPackedInt32: return packedNums(v, GodotVType::PackedInt32Array, 1, -4);
        case kPackedInt64: return packedNums(v, GodotVType::PackedInt64Array, 1, -8);
        case kPackedFloat32: return packedNums(v, GodotVType::PackedFloat32Array, 1, 4);
        case kPackedFloat64: return packedNums(v, GodotVType::PackedFloat64Array, 1, 8);
        case kPackedVector2: return packedNums(v, GodotVType::PackedVector2Array, 2, real64_ ? 8 : 4);
        case kPackedVector3: return packedNums(v, GodotVType::PackedVector3Array, 3, real64_ ? 8 : 4);
        case kPackedVector4: return packedNums(v, GodotVType::PackedVector4Array, 4, real64_ ? 8 : 4);
        case kPackedColor: return packedNums(v, GodotVType::PackedColorArray, 4, 4);
        case kPackedString: {
            const u32 len = u32v();
            if (!ok_) return false;
            if (u64(len) * 4 > left()) {
                fail("a string array is longer than the file");
                return false;
            }
            v.type = GodotVType::PackedStringArray;
            v.strs.reserve(len);
            for (u32 i = 0; i < len; ++i) {
                std::string s;
                if (!str(s)) return false;
                v.strs.push_back(std::move(s));
            }
            return true;
        }
        case kInputEvent: fail("a legacy input-event value is not supported"); return false;
        default: fail("unknown value type " + std::to_string(tag)); return false;
    }
}

// ---- the container ----------------------------------------------------------------------------------------------------------

u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

// "RSCC": u32 mode, u32 block size, u32 total size, one u32 compressed size per block, then the blocks. Each block is a Zstandard frame.
// With firstOnly only the leading blocks that hold the first KiB are decoded (enough for the header).
bool unpackRscc(const u8* d, usize n, bool firstOnly, bool complete, std::vector<u8>& out, std::string& why, bool& needMore) {
    needMore = false;
    if (n < 16) {
        needMore = !complete;
        why = "the compressed header is cut off";
        return false;
    }
    const u32 mode = le32(d + 4), block = le32(d + 8), total = le32(d + 12);
    if (mode != 2) {
        why = "compression mode " + std::to_string(mode) + " is not supported (Godot 4 writes Zstandard, mode 2)";
        return false;
    }
    if (block == 0 || block > kBlockLimit) {
        why = "the compressed block size is not valid";
        return false;
    }
    if (total > kGodotBinaryMaxBytes) {
        why = "the file decompresses to more than the " + std::to_string(kGodotBinaryMaxBytes >> 20) + " MiB limit";
        return false;
    }
    const u64 blocks = u64(total) / block + 1;
    const u64 tableEnd = 16 + blocks * 4;
    if (tableEnd > n) {
        needMore = !complete;
        why = "the block table runs past the file";
        return false;
    }
    out.reserve(std::min<usize>(total, usize(64) << 20));
    usize ofs = usize(tableEnd);
    for (u64 i = 0; i < blocks; ++i) {
        const u32 cs = le32(d + 16 + 4 * i);
        if (cs > n - ofs) {
            needMore = !complete && firstOnly;
            why = "a compressed block runs past the file";
            return false;
        }
        const usize want = (i + 1 < blocks) ? block : usize(total - (blocks - 1) * block);
        const usize before = out.size();
        std::string zwhy;
        if (!zstdDecompress(d + ofs, cs, out, before + want, &zwhy)) {
            why = "block " + std::to_string(i) + ": " + zwhy;
            return false;
        }
        if (out.size() - before != want) {
            why = "block " + std::to_string(i) + " decodes to " + std::to_string(out.size() - before) + " bytes, not " + std::to_string(want);
            return false;
        }
        ofs += cs;
        if (firstOnly && out.size() >= 1024) break;   // the header is a few hundred bytes
    }
    return true;
}

// The header up to the reserved words. `flags` and the format version drive the tables that follow.
bool readHeader(Reader& r, GodotBinaryHeader& h, u32& flags, std::string& why) {
    const u32 be = r.u32raw();   // always little-endian; everything after it is in the file's own byte order
    if (!r.ok()) {
        why = r.why();
        return false;
    }
    if (be > 1) {
        why = "not a Godot resource (the endian flag is " + std::to_string(be) + ")";
        return false;
    }
    r.setBigEndian(be == 1);
    h.bigEndian = be == 1;
    const u32 r64 = r.u32v();
    h.majorVersion = int(r.u32v());
    h.minorVersion = int(r.u32v());
    h.formatVersion = int(r.u32v());
    if (!r.ok()) {
        why = r.why();
        return false;
    }
    if (h.majorVersion != 4) {
        why = "a Godot " + std::to_string(h.majorVersion) + " binary resource (engine version " + std::to_string(h.majorVersion) + "." + std::to_string(h.minorVersion) +
              "); only Godot 4 binaries are read";
        return false;
    }
    if (h.formatVersion < 3) {
        why = "resource format version " + std::to_string(h.formatVersion) + " is older than Godot 4's";
        return false;
    }
    r.str(h.className);
    r.u64v();   // the import-metadata offset
    flags = r.u32v();
    const u64 uid = r.u64v();
    if (flags & 8u) r.str(h.scriptClass);
    for (int i = 0; i < 11; ++i) r.u32v();
    if (!r.ok()) {
        why = r.why();
        return false;
    }
    h.realIsDouble = r64 != 0 || (flags & 4u) != 0;
    r.real64_ = h.realIsDouble;
    if (flags & 2u) h.uid = godotUidText(uid);
    return true;
}

struct IntRes {
    std::string path;
    std::string id;
    u64 offset = 0;
};

struct Parsed {
    GodotDocBase base;
    GodotBinaryHeader hdr;
    std::string mainType;
    std::vector<GodotProperty> mainProps;
    bool newer = false;
};

bool readProps(Reader& r, std::vector<GodotProperty>& props) {
    const u32 count = r.u32v();
    if (!r.ok()) return false;
    if (u64(count) * 8 > r.left()) {
        r.fail("a resource declares more properties than the file can hold");
        return false;
    }
    props.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        GodotProperty p;
        if (!r.stringRef(p.name)) return false;
        if (!r.variant(p.value, 0)) return false;
        // The text form omits an unset script; the binary one stores it as nil.
        if (p.name == "script" && p.value.isNil()) continue;
        props.push_back(std::move(p));
    }
    return true;
}

// Runs the whole container. On failure `diag` holds an error and false is returned.
bool readRsrc(const u8* data, usize size, std::string_view file, Parsed& out, GodotDiagnostics& diag) {
    auto fail = [&](const std::string& why) {
        diag.error(file, 0, "binary Godot resource that could not be read: " + why, kCode);
        return false;
    };
    const u8* payload = data;
    usize payloadSize = size;
    usize start = 0;   // offsets in the file count from its first byte, "RSRC" included; the compressed stream has no such magic
    std::vector<u8> plain;
    if (size >= 4 && std::memcmp(data, "RSRC", 4) == 0) {
        start = 4;
    } else if (size >= 4 && std::memcmp(data, "RSCC", 4) == 0) {
        std::string why;
        bool more = false;
        if (!unpackRscc(data, size, false, true, plain, why, more)) return fail(why);
        out.hdr.compressed = true;
        payload = plain.data();
        payloadSize = plain.size();
    } else {
        return fail("it does not start with RSRC or RSCC");
    }

    Reader r(payload, payloadSize);
    r.seek(start);
    u32 flags = 0;
    std::string why;
    const bool compressed = out.hdr.compressed;
    if (!readHeader(r, out.hdr, flags, why)) return fail(why);
    out.hdr.compressed = compressed;
    if (out.hdr.formatVersion > kLastKnownFormat) {
        out.newer = true;
        diag.warn(file, 0, "resource format version " + std::to_string(out.hdr.formatVersion) + " is newer than the newest known (" + std::to_string(kLastKnownFormat) + "); read as far as it matches",
                  kCode);
    }

    // string table
    const u32 stringCount = r.u32v();
    if (!r.ok()) return fail(r.why());
    if (u64(stringCount) * 4 > r.left()) return fail("the string table is longer than the file");
    r.strings.reserve(stringCount);
    for (u32 i = 0; i < stringCount; ++i) {
        std::string s;
        if (!r.str(s)) return fail(r.why());
        r.strings.push_back(std::move(s));
    }

    // external resources
    const u32 extCount = r.u32v();
    if (!r.ok()) return fail(r.why());
    if (u64(extCount) * 8 > r.left()) return fail("the external resource table is longer than the file");
    for (u32 i = 0; i < extCount; ++i) {
        GodotExtResource e;
        if (!r.str(e.type) || !r.str(e.path)) return fail(r.why());
        if (flags & 2u) {
            const u64 uid = r.u64v();
            e.uid = godotUidText(uid);
        }
        e.id = std::to_string(i + 1);
        r.extIds.push_back(e.id);
        out.base.extResources.push_back(std::move(e));
    }

    // internal resources
    const u32 intCount = r.u32v();
    if (!r.ok()) return fail(r.why());
    if (intCount == 0) return fail("it holds no resource");
    if (u64(intCount) * 12 > r.left()) return fail("the internal resource table is longer than the file");
    std::vector<IntRes> ints(intCount);
    std::set<std::string> used;
    for (u32 i = 0; i < intCount; ++i) {
        IntRes& ir = ints[i];
        if (!r.str(ir.path)) return fail(r.why());
        ir.offset = r.u64v();
        std::string id = ir.path.rfind("local://", 0) == 0 ? ir.path.substr(8) : ir.path;
        if (id.empty()) id = std::to_string(i);
        while (!used.insert(id).second) id += "_" + std::to_string(i);
        ir.id = id;
        r.subIds.push_back(id);
    }
    if (!r.ok()) return fail(r.why());

    for (u32 i = 0; i < intCount; ++i) {
        if (!r.seek(ints[i].offset)) return fail(r.why());
        std::string type;
        if (!r.str(type)) return fail(r.why());
        std::vector<GodotProperty> props;
        if (!readProps(r, props)) return fail("resource " + std::to_string(i) + " (" + type + "): " + r.why());
        if (i + 1 == intCount) {
            out.mainType = type;
            out.mainProps = std::move(props);
        } else {
            GodotSubResource s;
            s.id = ints[i].id;
            s.type = std::move(type);
            s.props = std::move(props);
            out.base.subResources.push_back(std::move(s));
        }
    }

    // The file ends with "RSRC" straight after the last resource's properties; anything else means a value was read with the wrong width.
    if (payloadSize < 4 || std::memcmp(payload + payloadSize - 4, "RSRC", 4) != 0)
        diag.warn(file, 0, "the end marker (RSRC) is missing; the file may be cut short", kCode);
    else if (r.pos() + 4 != payloadSize)
        diag.warn(file, 0, "the last resource ends " + std::to_string(i64(payloadSize) - 4 - i64(r.pos())) + " byte(s) before the end marker; a value may have been read with the wrong width", kCode);

    out.base.file = std::string(file);
    out.base.header.type = out.mainType;
    out.base.header.scriptClass = out.hdr.scriptClass;
    out.base.header.uid = out.hdr.uid;
    out.base.header.format = 3;
    return true;
}

const GodotVariant* findProp(const std::vector<GodotProperty>& props, std::string_view name) {
    for (const GodotProperty& p : props)
        if (p.name == name) return &p.value;
    return nullptr;
}

// ---- PackedScene ------------------------------------------------------------------------------------------------------------

bool buildScene(Parsed& p, std::string_view file, GodotScene& scene, GodotDiagnostics& diag) {
    auto fail = [&](const std::string& why) {
        diag.error(file, 0, "binary scene that could not be read: " + why, kCode);
        return false;
    };
    if (p.mainType != "PackedScene") return fail("the main resource is a " + p.mainType + ", not a PackedScene");
    const GodotVariant* bundled = findProp(p.mainProps, "_bundled");
    if (!bundled || bundled->type != GodotVType::Dictionary) return fail("the PackedScene has no _bundled table");
    const GodotVariant* names = bundled->find("names");
    const GodotVariant* variants = bundled->find("variants");
    const GodotVariant* nodes = bundled->find("nodes");
    if (!names || names->type != GodotVType::PackedStringArray || !variants || variants->type != GodotVType::Array || !nodes ||
        nodes->type != GodotVType::PackedInt32Array)
        return fail("the _bundled table lacks names, variants or nodes");
    const GodotVariant* nodePaths = bundled->find("node_paths");
    const GodotVariant* nodeIds = bundled->find("node_ids");
    const i64 nodeCount = bundled->find("node_count") ? bundled->find("node_count")->asInt(0) : 0;
    if (nodeCount < 0 || usize(nodeCount) > nodes->nums.size() / 6) return fail("node_count is larger than the node table");

    scene.header.kind = "gd_scene";
    scene.header.format = 3;
    scene.header.uid = p.hdr.uid;
    scene.file = std::string(file);
    scene.extResources = std::move(p.base.extResources);
    scene.subResources = std::move(p.base.subResources);

    auto nameAt = [&](i64 idx, std::string& out) {
        if (idx < 0 || usize(idx) >= names->strs.size()) return false;
        out = names->strs[usize(idx)];
        return true;
    };
    auto pathAt = [&](i32 idx, std::string& out) {
        if (!nodePaths || nodePaths->type != GodotVType::Array || idx < 0 || usize(idx) >= nodePaths->items.size()) return false;
        out = nodePaths->items[usize(idx)].s;
        return true;
    };

    const std::vector<f64>& ints = nodes->nums;
    usize k = 0;
    auto next = [&](i32& out) {
        if (k >= ints.size()) return false;
        out = i32(ints[k++]);
        return true;
    };
    std::unordered_map<std::string, int> pathIndex;
    for (i64 i = 0; i < nodeCount; ++i) {
        i32 parent = 0, owner = 0, type = 0, nameWord = 0, instance = 0, propCount = 0;
        if (!next(parent) || !next(owner) || !next(type) || !next(nameWord) || !next(instance) || !next(propCount))
            return fail("the node table ends inside node " + std::to_string(i));
        GodotNode n;
        n.isRoot = i == 0;
        if (!nameAt(nameWord & kNameMask, n.name)) return fail("node " + std::to_string(i) + " names a string that is not in the name table");
        if (type != kTypeInstantiated && type >= 0 && !nameAt(type, n.type)) return fail("node " + std::to_string(i) + " has a class name that is not in the name table");
        if (instance >= 0) {
            if (instance & kFlagInstancePlaceholder) {
                const i32 vi = instance & ~kFlagInstancePlaceholder;
                if (usize(vi) >= variants->items.size() || !variants->items[usize(vi)].isStringLike()) return fail("node " + std::to_string(i) + " has a bad instance placeholder");
                n.instancePlaceholder = variants->items[usize(vi)].s;
            } else {
                if (usize(instance) >= variants->items.size()) return fail("node " + std::to_string(i) + " instances a value that is not in the variant table");
                const GodotVariant& iv = variants->items[usize(instance)];
                if (iv.type == GodotVType::ExtResource) n.instance = iv.s;
                else diag.warn(file, 0, "node '" + n.name + "' instances a scene that is not an external resource; the instance is dropped", kCode);
            }
        }
        // Parent: a node index, or (an override / a node added below an instance) a path in node_paths.
        if (!n.isRoot) {
            if (parent == 0x7FFFFFFF) return fail("node " + std::to_string(i) + " has no saved parent");
            if (parent & kFlagIdIsPath) {
                if (!pathAt(parent & ~kFlagIdIsPath, n.parentPath)) return fail("node " + std::to_string(i) + " has a parent path that is not in node_paths");
                if (n.parentPath.empty()) n.parentPath = ".";
            } else {
                if (parent < 0 || parent >= i) return fail("node " + std::to_string(i) + " has a parent (" + std::to_string(parent) + ") that is not an earlier node");
                n.parentPath = scene.nodes[usize(parent)].path;
            }
        }
        (void)owner;
        if (propCount < 0 || usize(propCount) > (ints.size() - k) / 2) return fail("node " + std::to_string(i) + " declares more properties than the table holds");
        for (i32 j = 0; j < propCount; ++j) {
            i32 ni = 0, vi = 0;
            next(ni);
            next(vi);
            std::string pname;
            if (!nameAt(ni & ~kFlagPathPropertyIsNodePath, pname)) return fail("node " + std::to_string(i) + " has a property name that is not in the name table");
            if (vi < 0 || usize(vi) >= variants->items.size()) return fail("node " + std::to_string(i) + " has a property value that is not in the variant table");
            if (ni & kFlagPathPropertyIsNodePath) n.nodePaths.push_back(pname);
            GodotProperty gp;
            gp.name = std::move(pname);
            gp.value = variants->items[usize(vi)];
            n.props.push_back(std::move(gp));
        }
        i32 groupCount = 0;
        if (!next(groupCount) || groupCount < 0 || usize(groupCount) > ints.size() - k) return fail("node " + std::to_string(i) + " has a bad group list");
        for (i32 j = 0; j < groupCount; ++j) {
            i32 gi = 0;
            next(gi);
            std::string g;
            if (!nameAt(gi, g)) return fail("node " + std::to_string(i) + " has a group that is not in the name table");
            n.groups.push_back(std::move(g));
        }
        if (nodeIds && nodeIds->type == GodotVType::PackedInt32Array && usize(i) < nodeIds->nums.size())
            n.uniqueId = std::to_string(u32(i32(nodeIds->nums[usize(i)])));
        n.index = -1;
        godotPlaceNode(scene, pathIndex, std::move(n), file, diag);
    }
    if (scene.nodes.empty()) diag.warn(file, 0, "the scene has no nodes", kCode);

    // An inherited scene keeps its base scene beside the node table; the root is that scene's instance.
    if (const GodotVariant* bs = bundled->find("base_scene"); bs && bs->isNumber() && bs->asInt(-1) >= 0 && !scene.nodes.empty()) {
        const i64 bi = bs->asInt(-1);
        if (usize(bi) < variants->items.size() && variants->items[usize(bi)].type == GodotVType::ExtResource && scene.nodes[0].instance.empty()) {
            scene.nodes[0].instance = variants->items[usize(bi)].s;
            scene.nodes[0].type.clear();
        }
    }

    // Connections: from, to, signal, method, flags, then either (unbinds, bind count, binds) or (bind count, binds, unbinds).
    const GodotVariant* conns = bundled->find("conns");
    const i64 connCount = bundled->find("conn_count") ? bundled->find("conn_count")->asInt(0) : 0;
    if (conns && conns->type == GodotVType::PackedInt32Array && connCount > 0) {
        const std::vector<f64>& c = conns->nums;
        auto nodePathOf = [&](i32 idx, std::string& out) {
            if (idx & kFlagIdIsPath) return pathAt(idx & ~kFlagIdIsPath, out);
            if (idx < 0 || usize(idx) >= scene.nodes.size()) return false;
            out = scene.nodes[usize(idx)].path;
            return true;
        };
        for (int layout = 0; layout < 2; ++layout) {
            std::vector<GodotConnection> parsed;
            usize q = 0;
            bool good = true;
            for (i64 i = 0; i < connCount && good; ++i) {
                if (c.size() - q < 7) { good = false; break; }
                GodotConnection cn;
                std::string sig, method;
                const i32 from = i32(c[q]), to = i32(c[q + 1]), si = i32(c[q + 2]), mi = i32(c[q + 3]);
                cn.flags = int(i32(c[q + 4]));
                q += 5;
                i32 unbinds, bindCount;
                if (layout == 0) {
                    unbinds = i32(c[q]);
                    bindCount = i32(c[q + 1]);
                    q += 2;
                } else {
                    bindCount = i32(c[q]);
                    unbinds = 0;
                    q += 1;
                }
                if (bindCount < 0 || usize(bindCount) > c.size() - q) { good = false; break; }
                for (i32 b = 0; b < bindCount; ++b) {
                    const i32 vi = i32(c[q++]);
                    if (vi < 0 || usize(vi) >= variants->items.size()) { good = false; break; }
                    cn.binds.push_back(variants->items[usize(vi)]);
                }
                if (layout == 1) {
                    if (q >= c.size()) { good = false; break; }
                    unbinds = i32(c[q++]);
                }
                cn.unbinds = int(unbinds);
                if (!good || unbinds < 0 || !nodePathOf(from, cn.from) || !nodePathOf(to, cn.to) || !nameAt(si, cn.signal) || !nameAt(mi, cn.method)) { good = false; break; }
                parsed.push_back(std::move(cn));
            }
            if (good && q == c.size()) {
                scene.connections = std::move(parsed);
                break;
            }
            if (layout == 1) diag.warn(file, 0, "the signal connection table could not be read; the connections are dropped", kCode);
        }
    }

    if (const GodotVariant* ed = bundled->find("editable_instances"); ed && ed->type == GodotVType::Array)
        for (const GodotVariant& e : ed->items)
            if (e.isStringLike() && !e.s.empty()) scene.editablePaths.push_back(e.s);
    return true;
}

}  // namespace

std::string godotUidText(u64 id) {
    if (id == 0 || (id >> 63) != 0) return {};
    std::string txt;
    while (id) {
        const u32 c = u32(id % 34);
        txt.insert(txt.begin(), c < 25 ? char('a' + c) : char('0' + (c - 25)));
        id /= 34;
    }
    return "uid://" + txt;
}

bool readGodotBinaryHeader(const u8* data, usize size, bool complete, GodotBinaryHeader& out, std::string* why, bool* needMore) {
    out = GodotBinaryHeader{};
    if (needMore) *needMore = false;
    std::string w;
    auto done = [&](bool ok) {
        if (!ok && why) *why = w;
        return ok;
    };
    const u8* payload = data;
    usize payloadSize = size;
    usize start = 0;   // offsets in the file count from its first byte, "RSRC" included; the compressed stream has no such magic
    std::vector<u8> plain;
    if (size >= 4 && std::memcmp(data, "RSRC", 4) == 0) {
        start = 4;
    } else if (size >= 4 && std::memcmp(data, "RSCC", 4) == 0) {
        bool more = false;
        if (!unpackRscc(data, size, true, complete, plain, w, more)) {
            if (needMore) *needMore = more;
            return done(false);
        }
        out.compressed = true;
        payload = plain.data();
        payloadSize = plain.size();
    } else {
        w = "it does not start with RSRC or RSCC";
        return done(false);
    }
    Reader r(payload, payloadSize);
    r.seek(start);
    u32 flags = 0;
    const bool compressed = out.compressed;
    if (!readHeader(r, out, flags, w)) {
        if (needMore && !complete && !r.ok()) *needMore = true;
        return done(false);
    }
    out.compressed = compressed;
    return true;
}

bool parseGodotBinaryResource(const u8* data, usize size, std::string_view file, GodotResource& out, GodotDiagnostics& diag) {
    out = GodotResource{};
    Parsed p;
    if (!readRsrc(data, size, file, p, diag)) return false;
    static_cast<GodotDocBase&>(out) = std::move(p.base);
    out.header.kind = "gd_resource";
    out.header.type = p.mainType;
    out.props = std::move(p.mainProps);
    return true;
}

bool parseGodotBinaryScene(const u8* data, usize size, std::string_view file, GodotScene& out, GodotDiagnostics& diag) {
    out = GodotScene{};
    Parsed p;
    if (!readRsrc(data, size, file, p, diag)) return false;
    return buildScene(p, file, out, diag);
}

}  // namespace aver::fmt::godot
