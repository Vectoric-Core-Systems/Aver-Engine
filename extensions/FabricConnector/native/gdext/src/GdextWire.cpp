// The flat marshalled form of a Variant that crosses the aver_gdext_* C ABI (documented in docs/plans/fabric/G3_GDEXT.md):
// little endian, a type byte (the GDExtension variant type number) then the payload. Callable and Signal values travel as nil.
#include "GdextIf.hpp"

namespace aver::gdext {

template <class T> static T ld(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }
template <class T> static void st(void* p, const T& t) { std::memcpy(p, &t, sizeof t); }

static void putBytes(std::vector<unsigned char>& out, const void* p, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    out.insert(out.end(), c, c + n);
}
template <class T> static void put(std::vector<unsigned char>& out, T v) { putBytes(out, &v, sizeof v); }
static void putText(std::vector<unsigned char>& out, const std::string& s) { put<int32_t>(out, int32_t(s.size())); putBytes(out, s.data(), s.size()); }

void wireEncode(const Var* v, std::vector<unsigned char>& out) {
    const int t = v->type;
    if (t == T_CALLABLE || t == T_SIGNAL) { out.push_back(T_NIL); return; }
    out.push_back(static_cast<unsigned char>(t));
    switch (t) {
        case T_NIL: return;
        case T_BOOL: out.push_back(v->d[0]); return;
        case T_STRING: case T_STRING_NAME: case T_NODE_PATH: { V s; varAssign(&s.v, v); putText(out, s.toUtf8()); return; }
        case T_OBJECT: { HostObject* o = varObject(v); put<uint64_t>(out, o ? o->id : 0); return; }
        case T_ARRAY: {
            const ArrayData* a = ld<ArrayData*>(v->d);
            put<int32_t>(out, int32_t(a->items.size()));
            for (const Var& e : a->items) wireEncode(&e, out);
            return;
        }
        case T_DICTIONARY: {
            const DictData* d = ld<DictData*>(v->d);
            put<int32_t>(out, int32_t(d->items.size()));
            for (const auto& kv : d->items) { wireEncode(&kv.first, out); wireEncode(&kv.second, out); }
            return;
        }
        default: break;
    }
    if (const PackedData* p = packedOf(v)) {
        put<int32_t>(out, int32_t(p->count()));
        if (p->type == T_PACKED_STRING) {
            for (size_t i = 0; i < p->count(); ++i) {
                auto* s = ld<StrData*>(p->bytes.data() + i * 8);
                putText(out, s ? u32ToUtf8(s->s) : std::string());
            }
        } else {
            putBytes(out, p->bytes.data(), p->bytes.size());
        }
        return;
    }
    putBytes(out, varPtr(v), nativeSize(t));      // INT, FLOAT, RID and every math type: the native bytes
}

namespace {
struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool take(void* dst, size_t n) {
        if (size_t(end - p) < n) return false;
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    bool text(std::string& s) {
        int32_t n;
        if (!take(&n, 4) || n < 0 || size_t(end - p) < size_t(n)) return false;
        s.assign(reinterpret_cast<const char*>(p), size_t(n));
        p += n;
        return true;
    }
};

bool decodeOne(Reader& r, Var* out, int depth) {
    varNil(out);
    if (depth > 64) return false;
    unsigned char t;
    if (!r.take(&t, 1) || t >= kTypeCount) return false;
    switch (t) {
        case T_NIL: return true;
        case T_BOOL: { unsigned char b; if (!r.take(&b, 1)) return false; V v = V::ofBool(b != 0); *out = v.v; varNil(&v.v); return true; }
        case T_STRING: case T_STRING_NAME: case T_NODE_PATH: {
            std::string s;
            if (!r.text(s)) return false;
            V v = t == T_STRING_NAME ? V::ofName(s) : V::ofString(s);
            v.v.type = t;
            *out = v.v; varNil(&v.v);
            return true;
        }
        case T_OBJECT: {
            uint64_t id;
            if (!r.take(&id, 8)) return false;
            V v = V::ofObject(objectById(id));
            *out = v.v; varNil(&v.v);
            return true;
        }
        case T_ARRAY: {
            int32_t n;
            if (!r.take(&n, 4) || n < 0) return false;
            V arr = makeArray();
            ArrayData* a = ld<ArrayData*>(arr.v.d);
            for (int32_t i = 0; i < n; ++i) {
                a->items.emplace_back();
                if (!decodeOne(r, &a->items.back(), depth + 1)) return false;
            }
            *out = arr.v; varNil(&arr.v);
            return true;
        }
        case T_DICTIONARY: {
            int32_t n;
            if (!r.take(&n, 4) || n < 0) return false;
            V dict = makeDict();
            DictData* d = ld<DictData*>(dict.v.d);
            for (int32_t i = 0; i < n; ++i) {
                V k, val;
                if (!decodeOne(r, &k.v, depth + 1) || !decodeOne(r, &val.v, depth + 1)) return false;
                varAssign(dictSlot(d, k.ptr()), val.ptr());
            }
            *out = dict.v; varNil(&dict.v);
            return true;
        }
        case T_CALLABLE: case T_SIGNAL: return false;
        default: break;
    }
    if (t >= T_PACKED_BYTE && t <= T_PACKED_VECTOR4) {
        int32_t n;
        if (!r.take(&n, 4) || n < 0) return false;
        V pk = makePacked(t);
        PackedData* p = ld<PackedData*>(pk.v.d);
        if (t == T_PACKED_STRING) {
            p->bytes.assign(size_t(n) * 8, 0);
            for (int32_t i = 0; i < n; ++i) {
                std::string s;
                if (!r.text(s)) return false;
                st<StrData*>(p->bytes.data() + size_t(i) * 8, s.empty() ? nullptr : new StrData{utf8ToU32(s)});
            }
        } else {
            const size_t bytes = size_t(n) * p->esize;
            if (size_t(r.end - r.p) < bytes) return false;
            p->bytes.assign(r.p, r.p + bytes);
            r.p += bytes;
        }
        *out = pk.v; varNil(&pk.v);
        return true;
    }
    std::vector<unsigned char> buf(nativeSize(t) + 16);
    if (!r.take(buf.data(), nativeSize(t))) return false;
    varSetNative(out, t, buf.data());
    return true;
}
} // namespace

bool wireDecode(const unsigned char*& p, const unsigned char* end, Var* out) {
    Reader r{p, end};
    const bool ok = decodeOne(r, out, 0);
    if (!ok) varDestroy(out);
    p = r.p;
    return ok;
}

} // namespace aver::gdext
