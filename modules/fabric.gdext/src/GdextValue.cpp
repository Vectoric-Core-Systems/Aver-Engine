// Strings, builtins in raw memory, Variants, containers: the storage half of the GDExtension value model (GdextValue.hpp).
#include "GdextValue.hpp"
#include "GdextHost.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aver::gdext {

// ---------------------------------------------------------------------------------------------------
// strings

std::u32string utf8ToU32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;
        int n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; }
        else if ((c >> 4) == 0xE && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4;
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string u32ToUtf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xC0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18))); out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::u32string utf16ToU32(const char16_t* s, int64_t len, bool littleEndian) {
    (void)littleEndian; // host byte order; the BOM-less default of Godot's callers is little endian on every supported target
    std::u32string out;
    for (int64_t i = 0; i < len; ++i) {
        char32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        out.push_back(c);
    }
    return out;
}

std::u32string latin1ToU32(const char* s, int64_t len) {
    std::u32string out;
    out.reserve(static_cast<size_t>(len));
    for (int64_t i = 0; i < len; ++i) out.push_back(static_cast<unsigned char>(s[i]));
    return out;
}

NameData* internName(std::string_view utf8) {
    static std::mutex mu;
    static std::unordered_map<std::string, NameData*> table;
    std::lock_guard<std::mutex> lock(mu);
    auto it = table.find(std::string(utf8));
    if (it != table.end()) return it->second;
    auto* n = new NameData{std::string(utf8)};
    table.emplace(n->utf8, n);
    return n;
}

std::string floatText(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    std::string s(buf, r.ptr);
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

// ---------------------------------------------------------------------------------------------------
// type tables

static const size_t kSizes[kTypeCount] = {
    0, 1, 8, 8, 8,  8, 8, 16, 16,  12, 12, 24,  16, 16, 16, 16,  24, 36, 48, 64,  16,  8, 8, 8, 8,  16, 16,  8, 8,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16};

static const char* const kNames[kTypeCount] = {
    "Nil", "bool", "int", "float", "String", "Vector2", "Vector2i", "Rect2", "Rect2i", "Vector3", "Vector3i", "Transform2D",
    "Vector4", "Vector4i", "Plane", "Quaternion", "AABB", "Basis", "Transform3D", "Projection", "Color", "StringName", "NodePath",
    "RID", "Object", "Callable", "Signal", "Dictionary", "Array", "PackedByteArray", "PackedInt32Array", "PackedInt64Array",
    "PackedFloat32Array", "PackedFloat64Array", "PackedStringArray", "PackedVector2Array", "PackedVector3Array",
    "PackedColorArray", "PackedVector4Array"};

size_t nativeSize(int type) { return type >= 0 && type < kTypeCount ? kSizes[type] : 0; }
bool nativeIsHeap(int type) { return nativeSize(type) > 16; }
const char* typeName(int type) { return type >= 0 && type < kTypeCount ? kNames[type] : "?"; }

int typeFromName(std::string_view name) {
    if (name.empty()) return -1;
    if (name == "Variant") return kAnyType;
    for (int i = 0; i < kTypeCount; ++i)
        if (name == kNames[i]) return i;
    if (name.rfind("enum::", 0) == 0 || name.rfind("bitfield::", 0) == 0) return T_INT;
    if (name.rfind("typedarray::", 0) == 0) return T_ARRAY;
    if (name == "float" || name == "double") return T_FLOAT;
    if (name[0] >= 'A' && name[0] <= 'Z') return T_OBJECT;
    return -1;
}

size_t packedElemSize(int type) {
    switch (type) {
        case T_PACKED_BYTE: return 1;
        case T_PACKED_INT32: case T_PACKED_FLOAT32: return 4;
        case T_PACKED_INT64: case T_PACKED_FLOAT64: case T_PACKED_STRING: case T_PACKED_VECTOR2: return 8;
        case T_PACKED_VECTOR3: return 12;
        case T_PACKED_COLOR: case T_PACKED_VECTOR4: return 16;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------------------------------
// shared containers

ArrayData::~ArrayData() { for (Var& v : items) varDestroy(&v); }
DictData::~DictData() { for (auto& kv : items) { varDestroy(&kv.first); varDestroy(&kv.second); } }

PackedData::~PackedData() {
    if (type == T_PACKED_STRING)
        for (size_t i = 0; i < count(); ++i) {
            StrData* s;
            std::memcpy(&s, bytes.data() + i * 8, 8);
            delete s;
        }
}

CallableData::~CallableData() {
    if (custom && info.free_func) info.free_func(info.callable_userdata);
}

static PackedData* newPacked(int type) {
    auto* p = new PackedData();
    p->type = type;
    p->esize = packedElemSize(type);
    return p;
}

static PackedData* clonePacked(const PackedData* src) {
    PackedData* p = newPacked(src->type);
    p->bytes = src->bytes;
    if (src->type == T_PACKED_STRING)
        for (size_t i = 0; i < p->count(); ++i) {
            StrData* s;
            std::memcpy(&s, src->bytes.data() + i * 8, 8);
            StrData* c = s ? new StrData{s->s} : nullptr;
            std::memcpy(p->bytes.data() + i * 8, &c, 8);
        }
    return p;
}

void packedDetach(PackedData*& p) {
    if (p->rc > 1) {
        PackedData* c = clonePacked(p);
        --p->rc;
        p = c;
    }
}

template <class T> static T load(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }
template <class T> static void store(void* p, const T& t) { std::memcpy(p, &t, sizeof t); }

// ---------------------------------------------------------------------------------------------------
// builtins in raw memory

void nativeInit(int type, void* p) {
    size_t n = nativeSize(type);
    std::memset(p, 0, n);
    float* f = static_cast<float*>(p);
    switch (type) {
        case T_QUATERNION: f[3] = 1.f; break;
        case T_COLOR: f[3] = 1.f; break;
        case T_BASIS: f[0] = f[4] = f[8] = 1.f; break;
        case T_TRANSFORM2D: f[0] = f[3] = 1.f; break;
        case T_TRANSFORM3D: f[0] = f[4] = f[8] = 1.f; break;
        case T_PROJECTION: f[0] = f[5] = f[10] = f[15] = 1.f; break;
        case T_DICTIONARY: store<DictData*>(p, new DictData()); break;
        case T_ARRAY: store<ArrayData*>(p, new ArrayData()); break;
        default:
            if (type >= T_PACKED_BYTE && type <= T_PACKED_VECTOR4) store<PackedData*>(p, newPacked(type));
            break;
    }
}

void nativeCopy(int type, void* dst, const void* src) {
    switch (type) {
        case T_STRING: case T_NODE_PATH: {
            auto* s = load<StrData*>(src);
            store<StrData*>(dst, s ? new StrData{s->s} : nullptr);
            return;
        }
        case T_CALLABLE: {
            std::memcpy(dst, src, 16);
            if (auto* c = load<CallableData*>(src)) ++c->rc;
            return;
        }
        case T_DICTIONARY: { auto* d = load<DictData*>(src); ++d->rc; store(dst, d); return; }
        case T_ARRAY: { auto* a = load<ArrayData*>(src); ++a->rc; store(dst, a); return; }
        default: break;
    }
    if (type >= T_PACKED_BYTE && type <= T_PACKED_VECTOR4) {
        std::memset(dst, 0, 16);
        auto* p = load<PackedData*>(src);
        ++p->rc;
        store(dst, p);
        return;
    }
    std::memcpy(dst, src, nativeSize(type));
}

void nativeDestroy(int type, void* p) {
    switch (type) {
        case T_STRING: case T_NODE_PATH: delete load<StrData*>(p); store<StrData*>(p, nullptr); return;
        case T_CALLABLE: {
            if (auto* c = load<CallableData*>(p); c && --c->rc == 0) delete c;
            store<CallableData*>(p, nullptr);
            return;
        }
        case T_DICTIONARY: { auto* d = load<DictData*>(p); if (d && --d->rc == 0) delete d; store<DictData*>(p, nullptr); return; }
        case T_ARRAY: { auto* a = load<ArrayData*>(p); if (a && --a->rc == 0) delete a; store<ArrayData*>(p, nullptr); return; }
        default: break;
    }
    if (type >= T_PACKED_BYTE && type <= T_PACKED_VECTOR4) {
        auto* d = load<PackedData*>(p);
        if (d && --d->rc == 0) delete d;
        store<PackedData*>(p, nullptr);
    }
}

// ---------------------------------------------------------------------------------------------------
// Variants

void varNil(Var* v) { v->type = T_NIL; v->pad = 0; std::memset(v->d, 0, sizeof v->d); }

void* varPtr(Var* v) { return nativeIsHeap(v->type) ? load<void*>(v->d) : static_cast<void*>(v->d); }
const void* varPtr(const Var* v) { return nativeIsHeap(v->type) ? load<void*>(v->d) : static_cast<const void*>(v->d); }

void varSetNative(Var* dst, int type, const void* native) {
    varNil(dst);
    dst->type = type;
    if (type == T_NIL) return;
    if (nativeIsHeap(type)) {
        auto* mem = new unsigned char[nativeSize(type)];
        std::memcpy(mem, native, nativeSize(type));
        store<void*>(dst->d, mem);
    } else {
        nativeCopy(type, dst->d, native);
    }
}

void varGetNative(const Var* v, void* native) {
    if (v->type == T_NIL) return;
    if (nativeIsHeap(v->type)) std::memcpy(native, load<void*>(v->d), nativeSize(v->type));
    else nativeCopy(v->type, native, v->d);
}

void varCopy(Var* dst, const Var* src) {
    varNil(dst);
    dst->type = src->type;
    if (src->type == T_NIL) return;
    if (nativeIsHeap(src->type)) {
        auto* mem = new unsigned char[nativeSize(src->type)];
        std::memcpy(mem, load<void*>(src->d), nativeSize(src->type));
        store<void*>(dst->d, mem);
    } else {
        nativeCopy(src->type, dst->d, src->d);
    }
}

void varDestroy(Var* v) {
    if (v->type != T_NIL) {
        if (nativeIsHeap(v->type)) delete[] load<unsigned char*>(v->d);
        else nativeDestroy(v->type, v->d);
    }
    varNil(v);
}

void varAssign(Var* dst, const Var* src) {
    if (dst == src) return;
    Var tmp;
    varCopy(&tmp, src);
    varDestroy(dst);
    *dst = tmp;
}

HostObject* varObject(const Var* v) {
    if (v->type != T_OBJECT) return nullptr;
    HostObject* o = load<HostObject*>(v->d);
    return o && !o->freed ? o : nullptr;
}

ArrayData* arrayOf(const Var* v) { return v->type == T_ARRAY ? load<ArrayData*>(v->d) : nullptr; }
DictData* dictOf(const Var* v) { return v->type == T_DICTIONARY ? load<DictData*>(v->d) : nullptr; }
PackedData* packedOf(const Var* v) {
    return v->type >= T_PACKED_BYTE && v->type <= T_PACKED_VECTOR4 ? load<PackedData*>(v->d) : nullptr;
}

// ---------------------------------------------------------------------------------------------------
// V

V V::ofBool(bool b) { V r; r.v.type = T_BOOL; r.v.d[0] = b ? 1 : 0; return r; }
V V::ofInt(int64_t i) { V r; r.v.type = T_INT; store(r.v.d, i); return r; }
V V::ofFloat(double f) { V r; r.v.type = T_FLOAT; store(r.v.d, f); return r; }
V V::ofString(std::string_view s) {
    V r;
    r.v.type = T_STRING;
    store<StrData*>(r.v.d, s.empty() ? nullptr : new StrData{utf8ToU32(s)});
    return r;
}
V V::ofName(std::string_view s) { V r; r.v.type = T_STRING_NAME; store<NameData*>(r.v.d, s.empty() ? nullptr : internName(s)); return r; }
V V::ofObject(HostObject* o) { V r; if (o) { r.v.type = T_OBJECT; store(r.v.d, o); } return r; }
V V::ofNative(int type, const void* native) { V r; varSetNative(&r.v, type, native); return r; }
V V::ofVec(int type, std::initializer_list<float> comps) {
    V r;
    float tmp[16] = {};
    size_t i = 0;
    for (float f : comps) tmp[i++] = f;
    if (type == T_VECTOR2I || type == T_VECTOR3I || type == T_VECTOR4I)
        for (size_t k = 0; k < i; ++k) store<int32_t>(reinterpret_cast<char*>(tmp) + k * 4, static_cast<int32_t>(tmp[k]));
    varSetNative(&r.v, type, tmp);
    return r;
}

int64_t V::toInt() const {
    switch (v.type) {
        case T_BOOL: return v.d[0];
        case T_INT: return load<int64_t>(v.d);
        case T_FLOAT: return static_cast<int64_t>(load<double>(v.d));
        case T_STRING: case T_STRING_NAME: case T_NODE_PATH: return std::strtoll(toUtf8().c_str(), nullptr, 10);
        case T_RID: return load<int64_t>(v.d);
        default: return 0;
    }
}

double V::toFloat() const {
    switch (v.type) {
        case T_BOOL: return v.d[0];
        case T_INT: return static_cast<double>(load<int64_t>(v.d));
        case T_FLOAT: return load<double>(v.d);
        case T_STRING: case T_STRING_NAME: case T_NODE_PATH: return std::strtod(toUtf8().c_str(), nullptr);
        default: return 0;
    }
}

std::string V::toUtf8() const {
    switch (v.type) {
        case T_STRING: case T_NODE_PATH: { auto* s = load<StrData*>(v.d); return s ? u32ToUtf8(s->s) : std::string(); }
        case T_STRING_NAME: return nameText(load<NameData*>(v.d));
        default: return varStringify(&v);
    }
}

V makeArray() { V r; nativeInit(T_ARRAY, r.v.d); r.v.type = T_ARRAY; return r; }
V makeDict() { V r; nativeInit(T_DICTIONARY, r.v.d); r.v.type = T_DICTIONARY; return r; }
V makePacked(int type) { V r; nativeInit(type, r.v.d); r.v.type = type; return r; }

// ---------------------------------------------------------------------------------------------------
// equality, hash, text

static bool isNumber(const Var* v) { return v->type == T_INT || v->type == T_FLOAT; }
static double asDouble(const Var* v) { return v->type == T_INT ? static_cast<double>(load<int64_t>(v->d)) : load<double>(v->d); }
static bool isTextType(int t) { return t == T_STRING || t == T_STRING_NAME || t == T_NODE_PATH; }

static std::u32string textOf(const Var* v) {
    if (v->type == T_STRING_NAME) return utf8ToU32(nameText(load<NameData*>(v->d)));
    auto* s = load<StrData*>(v->d);
    return s ? s->s : std::u32string();
}

int dictFind(const DictData* d, const Var* key) {
    for (size_t i = 0; i < d->items.size(); ++i)
        if (varEqual(&d->items[i].first, key)) return static_cast<int>(i);
    return -1;
}

Var* dictSlot(DictData* d, const Var* key) {
    int i = dictFind(d, key);
    if (i >= 0) return &d->items[static_cast<size_t>(i)].second;
    d->items.emplace_back();
    varCopy(&d->items.back().first, key);
    varNil(&d->items.back().second);
    return &d->items.back().second;
}

bool varEqual(const Var* a, const Var* b) {
    if (isNumber(a) && isNumber(b)) return asDouble(a) == asDouble(b);
    if (isTextType(a->type) && isTextType(b->type)) return textOf(a) == textOf(b);
    if (a->type != b->type) return false;
    switch (a->type) {
        case T_NIL: return true;
        case T_BOOL: return a->d[0] == b->d[0];
        case T_OBJECT: return load<void*>(a->d) == load<void*>(b->d);
        case T_ARRAY: {
            const ArrayData* x = load<ArrayData*>(a->d); const ArrayData* y = load<ArrayData*>(b->d);
            if (x == y) return true;
            if (x->items.size() != y->items.size()) return false;
            for (size_t i = 0; i < x->items.size(); ++i)
                if (!varEqual(&x->items[i], &y->items[i])) return false;
            return true;
        }
        case T_DICTIONARY: {
            const DictData* x = load<DictData*>(a->d); const DictData* y = load<DictData*>(b->d);
            if (x == y) return true;
            if (x->items.size() != y->items.size()) return false;
            for (const auto& kv : x->items) {
                int i = dictFind(y, &kv.first);
                if (i < 0 || !varEqual(&kv.second, &y->items[static_cast<size_t>(i)].second)) return false;
            }
            return true;
        }
        case T_CALLABLE: {
            auto* x = load<CallableData*>(a->d); auto* y = load<CallableData*>(b->d);
            if (x == y) return true;
            if (!x || !y) return false;
            if (x->custom != y->custom) return false;
            if (x->custom) return x->info.callable_userdata == y->info.callable_userdata && x->info.call_func == y->info.call_func;
            return x->objectId == y->objectId && x->method == y->method;
        }
        case T_SIGNAL: return std::memcmp(a->d, b->d, 16) == 0;
        case T_FLOAT: return load<double>(a->d) == load<double>(b->d);
        default: break;
    }
    if (a->type >= T_PACKED_BYTE && a->type <= T_PACKED_VECTOR4) {
        const PackedData* x = load<PackedData*>(a->d); const PackedData* y = load<PackedData*>(b->d);
        if (x == y) return true;
        if (x->bytes.size() != y->bytes.size()) return false;
        if (a->type == T_PACKED_STRING) {
            for (size_t i = 0; i < x->count(); ++i) {
                auto* s = load<StrData*>(x->bytes.data() + i * 8); auto* t = load<StrData*>(y->bytes.data() + i * 8);
                if ((s ? s->s : std::u32string()) != (t ? t->s : std::u32string())) return false;
            }
            return true;
        }
        return x->bytes == y->bytes;
    }
    const size_t n = nativeSize(a->type);
    const unsigned char* pa = static_cast<const unsigned char*>(varPtr(a));
    const unsigned char* pb = static_cast<const unsigned char*>(varPtr(b));
    if (a->type == T_STRING || a->type == T_NODE_PATH) return textOf(a) == textOf(b);
    if (a->type == T_STRING_NAME) return load<void*>(a->d) == load<void*>(b->d);
    // float structures: compare as numbers so -0 == 0
    if (n % 4 == 0 && a->type != T_RID && a->type != T_VECTOR2I && a->type != T_VECTOR3I && a->type != T_VECTOR4I &&
        a->type != T_RECT2I) {
        for (size_t i = 0; i < n; i += 4)
            if (load<float>(pa + i) != load<float>(pb + i)) return false;
        return true;
    }
    return std::memcmp(pa, pb, n) == 0;
}

bool varLess(const Var* a, const Var* b, bool* valid) {
    *valid = true;
    if (isNumber(a) && isNumber(b)) return asDouble(a) < asDouble(b);
    if (isTextType(a->type) && isTextType(b->type)) return textOf(a) < textOf(b);
    if (a->type == T_BOOL && b->type == T_BOOL) return a->d[0] < b->d[0];
    if (a->type == T_RID && b->type == T_RID) return load<uint64_t>(a->d) < load<uint64_t>(b->d);
    if (a->type == b->type) {
        const bool fl = a->type == T_VECTOR2 || a->type == T_VECTOR3 || a->type == T_VECTOR4;
        const bool in = a->type == T_VECTOR2I || a->type == T_VECTOR3I || a->type == T_VECTOR4I;
        if (fl || in) {
            const size_t n = nativeSize(a->type) / 4;
            for (size_t i = 0; i < n; ++i) {
                if (fl) {
                    float x = load<float>(a->d + i * 4), y = load<float>(b->d + i * 4);
                    if (x != y) return x < y;
                } else {
                    int32_t x = load<int32_t>(a->d + i * 4), y = load<int32_t>(b->d + i * 4);
                    if (x != y) return x < y;
                }
            }
            return false;
        }
        if (a->type == T_OBJECT) return load<HostObject*>(a->d) < load<HostObject*>(b->d);
    }
    *valid = false;
    return false;
}

static uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    return h;
}

int64_t varHash(const Var* v) {
    uint64_t h = 0;
    switch (v->type) {
        case T_NIL: h = 0; break;
        case T_INT: h = fnv(v->d, 8); break;
        case T_FLOAT: {
            double d = load<double>(v->d);
            if (d == std::floor(d) && std::fabs(d) < 9e18) { int64_t i = static_cast<int64_t>(d); h = fnv(&i, 8); } // 1.0 hashes as 1
            else h = fnv(&d, 8);
            break;
        }
        case T_STRING: case T_NODE_PATH: case T_STRING_NAME: {
            std::u32string s = textOf(v);
            h = fnv(s.data(), s.size() * 4);
            break;
        }
        case T_ARRAY: {
            h = 7;
            for (const Var& e : load<ArrayData*>(v->d)->items) {
                uint64_t eh = static_cast<uint64_t>(varHash(&e));
                h = fnv(&eh, 8, h);
            }
            break;
        }
        case T_DICTIONARY: {
            h = 11;
            for (const auto& kv : load<DictData*>(v->d)->items)
                h += static_cast<uint64_t>(varHash(&kv.first)) * 31 + static_cast<uint64_t>(varHash(&kv.second));
            break;
        }
        default:
            if (v->type >= T_PACKED_BYTE && v->type <= T_PACKED_VECTOR4) {
                auto* p = load<PackedData*>(v->d);
                h = p->type == T_PACKED_STRING ? p->bytes.size() : fnv(p->bytes.data(), p->bytes.size());
            } else if (v->type == T_OBJECT || v->type == T_CALLABLE) {
                h = fnv(v->d, 8);
            } else {
                h = fnv(varPtr(v), nativeSize(v->type));
            }
            break;
    }
    return static_cast<int64_t>((h ^ (h >> 32)) & 0x7FFFFFFFULL);
}

// Godot writes the components of vectors, colours and transforms without a trailing ".0": (1, 2.5), unlike a lone float.
static std::string realText(double d) {
    std::string s = floatText(d);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

static std::string vecText(const Var* v, size_t n, bool isInt) {
    std::string s = "(";
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ", ";
        s += isInt ? std::to_string(load<int32_t>(v->d + i * 4)) : realText(load<float>(v->d + i * 4));
    }
    return s + ")";
}

static std::string floatsText(const float* f, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) { if (i) s += ", "; s += realText(f[i]); }
    return s;
}

std::string varStringify(const Var* v) {
    switch (v->type) {
        case T_NIL: return "<null>";
        case T_BOOL: return v->d[0] ? "true" : "false";
        case T_INT: return std::to_string(load<int64_t>(v->d));
        case T_FLOAT: return floatText(load<double>(v->d));
        case T_STRING: case T_NODE_PATH: { auto* s = load<StrData*>(v->d); return s ? u32ToUtf8(s->s) : std::string(); }
        case T_STRING_NAME: return nameText(load<NameData*>(v->d));
        case T_VECTOR2: return vecText(v, 2, false);
        case T_VECTOR2I: return vecText(v, 2, true);
        case T_VECTOR3: return vecText(v, 3, false);
        case T_VECTOR3I: return vecText(v, 3, true);
        case T_VECTOR4: case T_PLANE: case T_QUATERNION: case T_COLOR: return vecText(v, 4, false);
        case T_VECTOR4I: return vecText(v, 4, true);
        case T_RECT2: { const float* f = static_cast<const float*>(varPtr(v)); return "[P: (" + floatsText(f, 2) + "), S: (" + floatsText(f + 2, 2) + ")]"; }
        case T_RECT2I: {
            const int32_t* f = static_cast<const int32_t*>(varPtr(v));
            return "[P: (" + std::to_string(f[0]) + ", " + std::to_string(f[1]) + "), S: (" + std::to_string(f[2]) + ", " + std::to_string(f[3]) + ")]";
        }
        case T_AABB: { const float* f = static_cast<const float*>(varPtr(v)); return "[P: (" + floatsText(f, 3) + "), S: (" + floatsText(f + 3, 3) + ")]"; }
        case T_BASIS: { const float* f = static_cast<const float*>(varPtr(v)); return "[X: (" + floatsText(f, 3) + "), Y: (" + floatsText(f + 3, 3) + "), Z: (" + floatsText(f + 6, 3) + ")]"; }
        case T_TRANSFORM3D: {
            const float* f = static_cast<const float*>(varPtr(v));
            return "[X: (" + floatsText(f, 3) + "), Y: (" + floatsText(f + 3, 3) + "), Z: (" + floatsText(f + 6, 3) + "), O: (" + floatsText(f + 9, 3) + ")]";
        }
        case T_TRANSFORM2D: { const float* f = static_cast<const float*>(varPtr(v)); return "[X: (" + floatsText(f, 2) + "), Y: (" + floatsText(f + 2, 2) + "), O: (" + floatsText(f + 4, 2) + ")]"; }
        case T_PROJECTION: { const float* f = static_cast<const float*>(varPtr(v)); return "[X: (" + floatsText(f, 4) + "), Y: (" + floatsText(f + 4, 4) + "), Z: (" + floatsText(f + 8, 4) + "), W: (" + floatsText(f + 12, 4) + ")]"; }
        case T_RID: return "RID(" + std::to_string(load<uint64_t>(v->d)) + ")";
        case T_OBJECT: {
            HostObject* o = varObject(v);
            return o ? "<" + o->cls + "#" + std::to_string(o->id) + ">" : "<Object#null>";
        }
        case T_CALLABLE: {
            auto* c = load<CallableData*>(v->d);
            if (!c) return "null::null";
            return c->custom ? "<custom callable>" : "#" + std::to_string(c->objectId) + "::" + nameText(c->method);
        }
        case T_SIGNAL: return "Signal";
        case T_ARRAY: {
            std::string s = "[";
            const ArrayData* a = load<ArrayData*>(v->d);
            for (size_t i = 0; i < a->items.size(); ++i) {
                if (i) s += ", ";
                const Var& e = a->items[i];
                s += e.type == T_STRING || e.type == T_STRING_NAME ? "\"" + varStringify(&e) + "\"" : varStringify(&e);
            }
            return s + "]";
        }
        case T_DICTIONARY: {
            const DictData* d = load<DictData*>(v->d);
            if (d->items.empty()) return "{  }";
            std::string s = "{ ";
            for (size_t i = 0; i < d->items.size(); ++i) {
                if (i) s += ", ";
                auto q = [](const Var& e) { return e.type == T_STRING || e.type == T_STRING_NAME ? "\"" + varStringify(&e) + "\"" : varStringify(&e); };
                s += q(d->items[i].first) + ": " + q(d->items[i].second);
            }
            return s + " }";
        }
        default: break;
    }
    if (v->type >= T_PACKED_BYTE && v->type <= T_PACKED_VECTOR4) {
        const PackedData* p = load<PackedData*>(v->d);
        std::string s = "[";
        for (size_t i = 0; i < p->count(); ++i) {
            if (i) s += ", ";
            const unsigned char* e = p->bytes.data() + i * p->esize;
            switch (p->type) {
                case T_PACKED_BYTE: s += std::to_string(*e); break;
                case T_PACKED_INT32: s += std::to_string(load<int32_t>(e)); break;
                case T_PACKED_INT64: s += std::to_string(load<int64_t>(e)); break;
                case T_PACKED_FLOAT32: s += floatText(load<float>(e)); break;
                case T_PACKED_FLOAT64: s += floatText(load<double>(e)); break;
                case T_PACKED_STRING: { auto* t = load<StrData*>(e); s += t ? u32ToUtf8(t->s) : std::string(); break; }
                default: {
                    size_t n = p->esize / 4;
                    s += "(" + floatsText(reinterpret_cast<const float*>(e), n) + ")";
                    break;
                }
            }
        }
        return s + "]";
    }
    return "<?>";
}

bool varBooleanize(const Var* v) {
    switch (v->type) {
        case T_NIL: return false;
        case T_BOOL: return v->d[0] != 0;
        case T_INT: return load<int64_t>(v->d) != 0;
        case T_FLOAT: return load<double>(v->d) != 0.0;
        case T_STRING: case T_NODE_PATH: case T_STRING_NAME: return !textOf(v).empty();
        case T_OBJECT: return varObject(v) != nullptr;
        case T_ARRAY: return !load<ArrayData*>(v->d)->items.empty();
        case T_DICTIONARY: return !load<DictData*>(v->d)->items.empty();
        case T_CALLABLE: return load<CallableData*>(v->d) != nullptr;
        case T_RID: return load<uint64_t>(v->d) != 0;
        default: break;
    }
    if (v->type >= T_PACKED_BYTE && v->type <= T_PACKED_VECTOR4) return !load<PackedData*>(v->d)->bytes.empty();
    // math types: true unless equal to the default value
    std::vector<unsigned char> d(nativeSize(v->type));
    nativeInit(v->type, d.data());
    return std::memcmp(d.data(), varPtr(v), d.size()) != 0;
}

// ---------------------------------------------------------------------------------------------------
// conversion

bool varCanConvert(int from, int to, bool strict) {
    if (from == to) return true;
    if (to == T_NIL || from == T_NIL) return to == T_OBJECT || (!strict && to != T_NIL);
    auto num = [](int t) { return t == T_INT || t == T_FLOAT; };
    if (num(from) && num(to)) return true;
    if (isTextType(from) && isTextType(to)) return true;
    if (strict) {
        if ((from == T_VECTOR2 && to == T_VECTOR2I) || (from == T_VECTOR2I && to == T_VECTOR2)) return true;
        return false;
    }
    if (to == T_STRING) return true;
    if (to == T_BOOL || to == T_INT || to == T_FLOAT) return num(from) || from == T_BOOL || isTextType(from);
    if (to == T_STRING_NAME || to == T_NODE_PATH) return isTextType(from);
    if (isTextType(from) && to == T_COLOR) return true;
    static const int pairs[][2] = {{T_VECTOR2, T_VECTOR2I}, {T_VECTOR2I, T_VECTOR2}, {T_VECTOR3, T_VECTOR3I}, {T_VECTOR3I, T_VECTOR3},
                                   {T_VECTOR4, T_VECTOR4I}, {T_VECTOR4I, T_VECTOR4}, {T_RECT2, T_RECT2I}, {T_RECT2I, T_RECT2},
                                   {T_QUATERNION, T_BASIS}, {T_BASIS, T_QUATERNION}, {T_TRANSFORM3D, T_PROJECTION}, {T_PROJECTION, T_TRANSFORM3D}};
    for (auto& p : pairs) if (p[0] == from && p[1] == to) return true;
    const bool fromPacked = from >= T_PACKED_BYTE && from <= T_PACKED_VECTOR4, toPacked = to >= T_PACKED_BYTE && to <= T_PACKED_VECTOR4;
    if ((fromPacked && to == T_ARRAY) || (from == T_ARRAY && toPacked)) return true;
    return false;
}

bool varConvert(const Var* in, int toType, Var* out) {
    if (in->type == toType) { varCopy(out, in); return true; }
    if (!varCanConvert(in->type, toType, false)) return false;
    const Var* args[1] = {in};
    GDExtensionCallError err{};
    if (toType == T_STRING) { V s = V::ofString(varStringify(in)); varCopy(out, &s.v); return true; }
    return varConstruct(toType, args, 1, out, &err);
}

} // namespace aver::gdext
