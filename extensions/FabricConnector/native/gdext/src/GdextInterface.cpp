// The interface functions of gdextension_interface.h that deal with Variants, strings and the builtin types, and the table
// get_proc_address answers from. The class and object half is in GdextClassDb.cpp.
#include "GdextIf.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <utility>

namespace aver::gdext {

template <class T> static T ld(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }
template <class T> static void st(void* p, const T& t) { std::memcpy(p, &t, sizeof t); }

static Var* VP(GDExtensionVariantPtr p) { return static_cast<Var*>(p); }
static const Var* CVP(GDExtensionConstVariantPtr p) { return static_cast<const Var*>(p); }
static bool isText(int t) { return t == T_STRING || t == T_STRING_NAME || t == T_NODE_PATH; }

static void setStringSlot(void* slot, const std::u32string& s, bool initialised) {
    if (initialised) delete ld<StrData*>(slot);
    st<StrData*>(slot, s.empty() ? nullptr : new StrData{s});
}
static std::u32string stringSlot(const void* slot) {
    auto* s = ld<StrData*>(slot);
    return s ? s->s : std::u32string();
}

// ---------------------------------------------------------------------------------------------------
// core

static void if_get_godot_version(GDExtensionGodotVersion* v) {
    v->major = 4; v->minor = 5; v->patch = 0; v->string = "Godot Engine v4.5.stable.aver.fabric";
}
static void if_get_godot_version2(GDExtensionGodotVersion2* v) {
    v->major = 4; v->minor = 5; v->patch = 0; v->hex = 0x040500; v->status = "stable"; v->build = "aver.fabric";
    v->hash = "0000000000000000000000000000000000000000"; v->timestamp = 0; v->string = "Godot Engine v4.5.stable.aver.fabric";
}
static void* if_mem_alloc(size_t n) { return std::malloc(n); }
static void* if_mem_realloc(void* p, size_t n) { return std::realloc(p, n); }
static void if_mem_free(void* p) { std::free(p); }

static std::string where(const char* fn, const char* file, int32_t line) {
    return std::string(fn ? fn : "?") + " (" + (file ? file : "?") + ":" + std::to_string(line) + ")";
}
static void if_print_error(const char* d, const char* f, const char* file, int32_t line, GDExtensionBool) { hostLog(2, std::string(d ? d : "") + "  at: " + where(f, file, line)); }
static void if_print_error_with_message(const char* d, const char* m, const char* f, const char* file, int32_t line, GDExtensionBool) {
    hostLog(2, std::string(d ? d : "") + ": " + (m ? m : "") + "  at: " + where(f, file, line));
}
static void if_print_warning(const char* d, const char* f, const char* file, int32_t line, GDExtensionBool) { hostLog(1, std::string(d ? d : "") + "  at: " + where(f, file, line)); }
static void if_print_warning_with_message(const char* d, const char* m, const char* f, const char* file, int32_t line, GDExtensionBool) {
    hostLog(1, std::string(d ? d : "") + ": " + (m ? m : "") + "  at: " + where(f, file, line));
}

static uint64_t if_get_native_struct_size(GDExtensionConstStringNamePtr name) {
    const std::string n = nameOfSlot(name);
    auto& api = host().api;
    auto it = api.nativeStructSizes.find(n);
    if (it != api.nativeStructSizes.end()) return uint64_t(it->second);
    if (n == "ObjectID" || n == "AudioFrame") return 8;
    logOnce("native-struct:" + n, "get_native_struct_size: unknown native structure '" + n + "'");
    return 0;
}

// ---------------------------------------------------------------------------------------------------
// Variant

static void if_variant_new_copy(GDExtensionUninitializedVariantPtr d, GDExtensionConstVariantPtr s) { varCopy(VP(d), CVP(s)); }
static void if_variant_new_nil(GDExtensionUninitializedVariantPtr d) { varNil(VP(d)); }
static void if_variant_destroy(GDExtensionVariantPtr p) { varDestroy(VP(p)); }

static void if_variant_call(GDExtensionVariantPtr self, GDExtensionConstStringNamePtr method, const GDExtensionConstVariantPtr* args,
                            GDExtensionInt argc, GDExtensionUninitializedVariantPtr ret, GDExtensionCallError* err) {
    varNil(VP(ret));
    const std::string name = nameOfSlot(method);
    const Var* const* a = reinterpret_cast<const Var* const*>(args);
    if (VP(self)->type == T_OBJECT) callByName(varObject(CVP(self)), name, a, int(argc), VP(ret), err);
    else builtinCallByName(VP(self)->type, VP(self), name, a, int(argc), VP(ret), err);
}

static void if_variant_call_static(GDExtensionVariantType type, GDExtensionConstStringNamePtr method, const GDExtensionConstVariantPtr*,
                                   GDExtensionInt, GDExtensionUninitializedVariantPtr ret, GDExtensionCallError* err) {
    varNil(VP(ret));
    err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD; err->argument = 0; err->expected = 0;
    logOnce(std::string("call-static:") + typeName(type) + "." + nameOfSlot(method), std::string("variant_call_static: ") + typeName(type) + "." + nameOfSlot(method) + " is not implemented");
}

static void if_variant_evaluate(GDExtensionVariantOperator op, GDExtensionConstVariantPtr a, GDExtensionConstVariantPtr b,
                                GDExtensionUninitializedVariantPtr ret, GDExtensionBool* valid) {
    varNil(VP(ret));
    const bool ok = varEvaluate(int(op), CVP(a), CVP(b), VP(ret));
    if (!ok) varNil(VP(ret));
    *valid = ok;
}

static V nameKey(GDExtensionConstStringNamePtr n) { return V::ofName(nameOfSlot(n)); }

static std::string keyText(const Var* key) { V t; varAssign(&t.v, key); return t.toUtf8(); }
static bool setByKey(Var* self, const Var* key, const Var* value) {
    if (self->type == T_OBJECT) return setProperty(varObject(self), keyText(key), value);
    return varSetKeyed(self, key, value);
}
static bool getByKey(const Var* self, const Var* key, Var* out) {
    if (self->type == T_OBJECT) return getProperty(varObject(self), keyText(key), out);
    return varGetKeyed(self, key, out);
}

static void if_variant_set(GDExtensionVariantPtr self, GDExtensionConstVariantPtr key, GDExtensionConstVariantPtr value, GDExtensionBool* valid) { *valid = setByKey(VP(self), CVP(key), CVP(value)); }
static void if_variant_set_named(GDExtensionVariantPtr self, GDExtensionConstStringNamePtr key, GDExtensionConstVariantPtr value, GDExtensionBool* valid) {
    V k = nameKey(key);
    *valid = setByKey(VP(self), k.ptr(), CVP(value));
}
static void if_variant_set_keyed(GDExtensionVariantPtr self, GDExtensionConstVariantPtr key, GDExtensionConstVariantPtr value, GDExtensionBool* valid) { *valid = setByKey(VP(self), CVP(key), CVP(value)); }
static void if_variant_set_indexed(GDExtensionVariantPtr self, GDExtensionInt index, GDExtensionConstVariantPtr value, GDExtensionBool* valid, GDExtensionBool* oob) {
    bool o = false;
    *valid = varSetIndexed(VP(self), index, CVP(value), &o);
    *oob = o;
}
static void if_variant_get(GDExtensionConstVariantPtr self, GDExtensionConstVariantPtr key, GDExtensionUninitializedVariantPtr ret, GDExtensionBool* valid) {
    varNil(VP(ret));
    *valid = getByKey(CVP(self), CVP(key), VP(ret));
}
static void if_variant_get_named(GDExtensionConstVariantPtr self, GDExtensionConstStringNamePtr key, GDExtensionUninitializedVariantPtr ret, GDExtensionBool* valid) {
    varNil(VP(ret));
    V k = nameKey(key);
    *valid = getByKey(CVP(self), k.ptr(), VP(ret));
}
static void if_variant_get_keyed(GDExtensionConstVariantPtr self, GDExtensionConstVariantPtr key, GDExtensionUninitializedVariantPtr ret, GDExtensionBool* valid) {
    varNil(VP(ret));
    *valid = getByKey(CVP(self), CVP(key), VP(ret));
}
static void if_variant_get_indexed(GDExtensionConstVariantPtr self, GDExtensionInt index, GDExtensionUninitializedVariantPtr ret, GDExtensionBool* valid, GDExtensionBool* oob) {
    varNil(VP(ret));
    bool o = false;
    *valid = varGetIndexed(CVP(self), index, VP(ret), &o);
    *oob = o;
}

// iteration: an Array, a Packed*Array and a String walk by index; a Dictionary walks its keys; an int counts from 0
static bool iterCount(const Var* v, int64_t* n) {
    if (v->type == T_ARRAY) { *n = int64_t(ld<ArrayData*>(v->d)->items.size()); return true; }
    if (v->type == T_DICTIONARY) { *n = int64_t(ld<DictData*>(v->d)->items.size()); return true; }
    if (const PackedData* p = packedOf(v)) { *n = int64_t(p->count()); return true; }
    if (isText(v->type)) { *n = int64_t(V::ofNative(v->type, varPtr(v)).toUtf8().size()); return true; }
    if (v->type == T_INT) { *n = ld<int64_t>(v->d); return true; }
    return false;
}
static GDExtensionBool if_variant_iter_init(GDExtensionConstVariantPtr self, GDExtensionUninitializedVariantPtr iter, GDExtensionBool* valid) {
    varNil(VP(iter));
    int64_t n = 0;
    *valid = iterCount(CVP(self), &n);
    if (!*valid || n <= 0) return 0;
    V z = V::ofInt(0);
    varAssign(VP(iter), z.ptr());
    return 1;
}
static GDExtensionBool if_variant_iter_next(GDExtensionConstVariantPtr self, GDExtensionVariantPtr iter, GDExtensionBool* valid) {
    int64_t n = 0;
    *valid = iterCount(CVP(self), &n) && VP(iter)->type == T_INT;
    if (!*valid) return 0;
    int64_t i = ld<int64_t>(VP(iter)->d) + 1;
    if (i >= n) return 0;
    st<int64_t>(VP(iter)->d, i);
    return 1;
}
static void if_variant_iter_get(GDExtensionConstVariantPtr self, GDExtensionVariantPtr iter, GDExtensionUninitializedVariantPtr ret, GDExtensionBool* valid) {
    varNil(VP(ret));
    const Var* s = CVP(self);
    *valid = VP(iter)->type == T_INT;
    if (!*valid) return;
    int64_t i = ld<int64_t>(VP(iter)->d);
    bool oob;
    if (s->type == T_DICTIONARY) {
        const DictData* d = ld<DictData*>(s->d);
        if (i >= 0 && i < int64_t(d->items.size())) varCopy(VP(ret), &d->items[size_t(i)].first); else *valid = 0;
    } else if (s->type == T_INT) {
        V v = V::ofInt(i);
        varAssign(VP(ret), v.ptr());
    } else {
        *valid = varGetIndexed(s, i, VP(ret), &oob);
    }
}

static GDExtensionInt if_variant_hash(GDExtensionConstVariantPtr self) { return varHash(CVP(self)); }
static GDExtensionInt if_variant_recursive_hash(GDExtensionConstVariantPtr self, GDExtensionInt) { return varHash(CVP(self)); }
static GDExtensionBool if_variant_hash_compare(GDExtensionConstVariantPtr a, GDExtensionConstVariantPtr b) {
    const Var* x = CVP(a); const Var* y = CVP(b);
    if (x->type == T_FLOAT && y->type == T_FLOAT) {
        double p = ld<double>(x->d), q = ld<double>(y->d);
        return (p != p && q != q) || p == q;
    }
    return x->type == y->type && varEqual(x, y);
}
static GDExtensionBool if_variant_booleanize(GDExtensionConstVariantPtr self) { return varBooleanize(CVP(self)); }
static void if_variant_duplicate(GDExtensionConstVariantPtr self, GDExtensionVariantPtr ret, GDExtensionBool deep) {
    const Var* s = CVP(self);
    if (s->type == T_ARRAY || s->type == T_DICTIONARY) {
        V flag = V::ofBool(deep);
        const Var* args[1] = {flag.ptr()};
        V tmp;
        GDExtensionCallError e{};
        builtinCallByName(s->type, const_cast<Var*>(s), "duplicate", args, 1, &tmp.v, &e);
        varAssign(VP(ret), tmp.ptr());
    } else {
        varAssign(VP(ret), s);
    }
}
static void if_variant_stringify(GDExtensionConstVariantPtr self, GDExtensionStringPtr ret) { setStringSlot(ret, utf8ToU32(varStringify(CVP(self))), true); }
static GDExtensionVariantType if_variant_get_type(GDExtensionConstVariantPtr self) { return GDExtensionVariantType(CVP(self)->type); }
static GDExtensionBool if_variant_has_method(GDExtensionConstVariantPtr self, GDExtensionConstStringNamePtr method) {
    const Var* s = CVP(self);
    const std::string n = nameOfSlot(method);
    if (s->type == T_OBJECT) { HostObject* o = varObject(s); return o && findMethod(o->def, n) != nullptr; }
    return findBuiltinMethod(s->type, n) != nullptr;
}

static bool hasMember(int type, const std::string& name) {
    if (type == T_OBJECT) return false;
    std::vector<unsigned char> buf(std::max<size_t>(nativeSize(type), 16));
    nativeInit(type, buf.data());
    V v = V::ofNative(type, buf.data());
    nativeDestroy(type, buf.data());
    V out;
    return varGetMember(v.ptr(), name, &out.v);
}
static GDExtensionBool if_variant_has_member(GDExtensionVariantType type, GDExtensionConstStringNamePtr member) { return hasMember(int(type), nameOfSlot(member)); }
static GDExtensionBool if_variant_has_key(GDExtensionConstVariantPtr self, GDExtensionConstVariantPtr key, GDExtensionBool* valid) {
    bool v = true;
    bool has = varHasKey(CVP(self), CVP(key), &v);
    *valid = v;
    return has;
}
static GDObjectInstanceID if_variant_get_object_instance_id(GDExtensionConstVariantPtr self) {
    HostObject* o = varObject(CVP(self));
    return o ? o->id : 0;
}
static void if_variant_get_type_name(GDExtensionVariantType type, GDExtensionUninitializedStringPtr name) { st<StrData*>(name, nullptr); setStringSlot(name, utf8ToU32(typeName(int(type))), false); }
static GDExtensionBool if_variant_can_convert(GDExtensionVariantType from, GDExtensionVariantType to) { return varCanConvert(int(from), int(to), false); }
static GDExtensionBool if_variant_can_convert_strict(GDExtensionVariantType from, GDExtensionVariantType to) { return varCanConvert(int(from), int(to), true); }

static void if_variant_construct(GDExtensionVariantType type, GDExtensionUninitializedVariantPtr base, const GDExtensionConstVariantPtr* args,
                                 int32_t argc, GDExtensionCallError* err) {
    varNil(VP(base));
    varConstruct(int(type), reinterpret_cast<const Var* const*>(args), argc, VP(base), err);
}

static void if_variant_get_constant_value(GDExtensionVariantType type, GDExtensionConstStringNamePtr c, GDExtensionUninitializedVariantPtr ret) {
    varNil(VP(ret));
    logOnce(std::string("const:") + typeName(int(type)) + "." + nameOfSlot(c), std::string("variant_get_constant_value: ") + typeName(int(type)) + "." + nameOfSlot(c) + " is not implemented");
}

// ---------------------------------------------------------------------------------------------------
// per-type pointer-call functions (one template instance per variant type)

static int elemTypeOf(int base) {
    switch (base) {
        case T_VECTOR2: case T_VECTOR3: case T_VECTOR4: case T_QUATERNION: case T_COLOR: case T_PLANE: return T_FLOAT;
        case T_VECTOR2I: case T_VECTOR3I: case T_VECTOR4I: return T_INT;
        case T_BASIS: return T_VECTOR3;
        case T_TRANSFORM2D: return T_VECTOR2;
        case T_PROJECTION: return T_VECTOR4;
        case T_ARRAY: return kAnyType;
        case T_STRING: return T_STRING;
        case T_PACKED_BYTE: case T_PACKED_INT32: case T_PACKED_INT64: return T_INT;
        case T_PACKED_FLOAT32: case T_PACKED_FLOAT64: return T_FLOAT;
        case T_PACKED_STRING: return T_STRING;
        case T_PACKED_VECTOR2: return T_VECTOR2;
        case T_PACKED_VECTOR3: return T_VECTOR3;
        case T_PACKED_COLOR: return T_COLOR;
        case T_PACKED_VECTOR4: return T_VECTOR4;
        default: return -1;
    }
}

// The caller's native value of builtin type T as a borrowed Variant (no reference counts taken; heap types copied).
struct Borrowed {
    Var v;
    bool owned;
    Borrowed(int type, const void* native) {
        varNil(&v);
        v.type = type;
        owned = nativeIsHeap(type);
        if (owned) varSetNative(&v, type, native);
        else std::memcpy(v.d, native, nativeSize(type));
    }
    void writeBack(int type, void* native) const { if (!owned) std::memcpy(native, v.d, nativeSize(type)); }
    ~Borrowed() { if (owned) varDestroy(&v); }
};

template <int T> struct PerType {
    static void fromType(GDExtensionUninitializedVariantPtr dst, GDExtensionTypePtr src) {
        if (T == T_NIL) { varNil(VP(dst)); return; }
        readNative(T, src, VP(dst));
    }
    static void toType(GDExtensionUninitializedTypePtr dst, GDExtensionVariantPtr v) {
        if (T == T_NIL) return;
        const Var* in = VP(v);
        if (T == T_OBJECT) { st<HostObject*>(dst, varObject(in)); return; }
        if (T == T_BOOL) { *static_cast<uint8_t*>(dst) = varBooleanize(in) ? 1 : 0; return; }
        if (in->type == T) { varGetNative(in, dst); return; }
        V conv;
        if (varConvert(in, T, &conv.v)) varGetNative(&conv.v, dst);
        else nativeInit(T, dst);
    }
    static void* internalPtr(GDExtensionVariantPtr v) { return varPtr(VP(v)); }
    static void destroy(GDExtensionTypePtr p) { nativeDestroy(T, p); }
    static void indexedGet(GDExtensionConstTypePtr base, GDExtensionInt idx, GDExtensionTypePtr out) {
        Borrowed b(T, base);
        V res;
        bool oob = false;
        if (varGetIndexed(&b.v, idx, &res.v, &oob)) writeNative(&res.v, elemTypeOf(T) == kAnyType ? kAnyType : res.v.type, out);
        else { logOnce("oob-get", "indexed getter: index out of bounds"); }
    }
    static void indexedSet(GDExtensionTypePtr base, GDExtensionInt idx, GDExtensionConstTypePtr value) {
        Borrowed b(T, base);
        V val;
        readNative(elemTypeOf(T), value, &val.v);
        bool oob = false;
        if (!varSetIndexed(&b.v, idx, &val.v, &oob)) logOnce("oob-set", "indexed setter: index out of bounds");
        b.writeBack(T, base);
    }
    static void keyedGet(GDExtensionConstTypePtr base, GDExtensionConstTypePtr key, GDExtensionTypePtr out) {
        Borrowed b(T, base);
        V res;
        if (varGetKeyed(&b.v, static_cast<const Var*>(key), &res.v)) varAssign(static_cast<Var*>(out), &res.v);
    }
    static void keyedSet(GDExtensionTypePtr base, GDExtensionConstTypePtr key, GDExtensionConstTypePtr value) {
        Borrowed b(T, base);
        varSetKeyed(&b.v, static_cast<const Var*>(key), static_cast<const Var*>(value));
        b.writeBack(T, base);
    }
    static uint32_t keyedCheck(GDExtensionConstVariantPtr base, GDExtensionConstVariantPtr key) {
        bool valid;
        return varHasKey(CVP(base), CVP(key), &valid) ? 1u : 0u;
    }
};

template <size_t... I> static auto perTypeFrom(std::index_sequence<I...>) { return std::array<GDExtensionVariantFromTypeConstructorFunc, sizeof...(I)>{{&PerType<int(I)>::fromType...}}; }
template <size_t... I> static auto perTypeTo(std::index_sequence<I...>) { return std::array<GDExtensionTypeFromVariantConstructorFunc, sizeof...(I)>{{&PerType<int(I)>::toType...}}; }
template <size_t... I> static auto perTypeInternal(std::index_sequence<I...>) { return std::array<GDExtensionVariantGetInternalPtrFunc, sizeof...(I)>{{&PerType<int(I)>::internalPtr...}}; }
template <size_t... I> static auto perTypeDtor(std::index_sequence<I...>) { return std::array<GDExtensionPtrDestructor, sizeof...(I)>{{&PerType<int(I)>::destroy...}}; }
template <size_t... I> static auto perTypeIdxGet(std::index_sequence<I...>) { return std::array<GDExtensionPtrIndexedGetter, sizeof...(I)>{{&PerType<int(I)>::indexedGet...}}; }
template <size_t... I> static auto perTypeIdxSet(std::index_sequence<I...>) { return std::array<GDExtensionPtrIndexedSetter, sizeof...(I)>{{&PerType<int(I)>::indexedSet...}}; }
template <size_t... I> static auto perTypeKeyGet(std::index_sequence<I...>) { return std::array<GDExtensionPtrKeyedGetter, sizeof...(I)>{{&PerType<int(I)>::keyedGet...}}; }
template <size_t... I> static auto perTypeKeySet(std::index_sequence<I...>) { return std::array<GDExtensionPtrKeyedSetter, sizeof...(I)>{{&PerType<int(I)>::keyedSet...}}; }
template <size_t... I> static auto perTypeKeyChk(std::index_sequence<I...>) { return std::array<GDExtensionPtrKeyedChecker, sizeof...(I)>{{&PerType<int(I)>::keyedCheck...}}; }

static const auto kSeq = std::make_index_sequence<size_t(kTypeCount)>{};
static const auto kFromTable = perTypeFrom(kSeq);
static const auto kToTable = perTypeTo(kSeq);
static const auto kInternalTable = perTypeInternal(kSeq);
static const auto kDtorTable = perTypeDtor(kSeq);
static const auto kIdxGetTable = perTypeIdxGet(kSeq);
static const auto kIdxSetTable = perTypeIdxSet(kSeq);
static const auto kKeyGetTable = perTypeKeyGet(kSeq);
static const auto kKeySetTable = perTypeKeySet(kSeq);
static const auto kKeyChkTable = perTypeKeyChk(kSeq);

static bool validType(int t) { return t >= 0 && t < kTypeCount; }

static GDExtensionVariantFromTypeConstructorFunc if_get_variant_from_type_constructor(GDExtensionVariantType type) { const int t = int(type); return validType(t) ? kFromTable[size_t(t)] : nullptr; }
static GDExtensionTypeFromVariantConstructorFunc if_get_variant_to_type_constructor(GDExtensionVariantType type) { const int t = int(type); return validType(t) ? kToTable[size_t(t)] : nullptr; }
static GDExtensionVariantGetInternalPtrFunc if_variant_get_ptr_internal_getter(GDExtensionVariantType type) { const int t = int(type); return validType(t) && t != T_NIL ? kInternalTable[size_t(t)] : nullptr; }
static GDExtensionPtrDestructor if_variant_get_ptr_destructor(GDExtensionVariantType type) {
    const int t = int(type);
    const bool has = t == T_STRING || t == T_STRING_NAME || t == T_NODE_PATH || t == T_CALLABLE || t == T_SIGNAL || t == T_DICTIONARY || t == T_ARRAY ||
                     (t >= T_PACKED_BYTE && t <= T_PACKED_VECTOR4);
    return has ? kDtorTable[size_t(t)] : nullptr;
}
static GDExtensionPtrIndexedGetter if_variant_get_ptr_indexed_getter(GDExtensionVariantType type) { const int t = int(type); return validType(t) && elemTypeOf(t) >= 0 ? kIdxGetTable[size_t(t)] : nullptr; }
static GDExtensionPtrIndexedSetter if_variant_get_ptr_indexed_setter(GDExtensionVariantType type) { const int t = int(type); return validType(t) && elemTypeOf(t) >= 0 && t != T_STRING ? kIdxSetTable[size_t(t)] : nullptr; }
static GDExtensionPtrKeyedGetter if_variant_get_ptr_keyed_getter(GDExtensionVariantType type) { return int(type) == T_DICTIONARY ? kKeyGetTable[size_t(type)] : nullptr; }
static GDExtensionPtrKeyedSetter if_variant_get_ptr_keyed_setter(GDExtensionVariantType type) { return int(type) == T_DICTIONARY ? kKeySetTable[size_t(type)] : nullptr; }
static GDExtensionPtrKeyedChecker if_variant_get_ptr_keyed_checker(GDExtensionVariantType type) { return int(type) == T_DICTIONARY ? kKeyChkTable[size_t(type)] : nullptr; }

// ---------------------------------------------------------------------------------------------------
// slot pools: a pointer-call function carries no user data, so each (type, operator, method ...) gets a numbered thunk

template <class Fn, size_t N, template <size_t> class Thunk, size_t... I>
static std::array<Fn, N> makePool(std::index_sequence<I...>) { return {{&Thunk<I>::call...}}; }

// godot-cpp calls the function pointers it was given without a null check, and asks for every builtin method, operator and member of
// the dump at start. A thing Godot has and the host does not (the dump lists it) therefore gets a stub that reports the call and does
// nothing; a thing the dump does not list (or whose hash differs) gets null.
static void reportStub(const std::string& what) {
    Host& h = host();
    if (std::find(h.unsupportedBuiltins.begin(), h.unsupportedBuiltins.end(), what) == h.unsupportedBuiltins.end()) h.unsupportedBuiltins.push_back(what);
    logOnce("stubcall:" + what, "builtin " + what + " was called and is not implemented by the Aver host (G3_GDEXT.md); the call did nothing");
}

static const char* const kOpNames[] = {"==", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "unary-", "unary+", "%", "**", "<<", ">>",
                                       "&", "|", "^", "~", "and", "or", "xor", "not", "in"};

static bool isUnaryOp(int op) {
    return op == GDEXTENSION_VARIANT_OP_NEGATE || op == GDEXTENSION_VARIANT_OP_POSITIVE || op == GDEXTENSION_VARIANT_OP_BIT_NEGATE || op == GDEXTENSION_VARIANT_OP_NOT;
}

// Does the dump list this operator on builtin type `ta` with right operand `tb` (Nil standing for "Variant" or no operand)?
static bool dumpKnowsOperator(int op, int ta, int tb) {
    const ApiDump& api = host().api;
    if (!api.loaded || op < 0 || op >= int(sizeof kOpNames / sizeof kOpNames[0])) return false;
    auto it = api.builtinOperators.find(ta == T_NIL ? "Nil" : typeName(ta));
    if (it == api.builtinOperators.end()) return false;
    for (const ApiDump::Operator& o : it->second) {
        if (o.name != kOpNames[op]) continue;
        if (isUnaryOp(op) ? o.right.empty() : (o.right == "Variant" ? tb == T_NIL : (!o.right.empty() && typeFromName(o.right) == tb))) return true;
    }
    return false;
}

struct EvalSlot { int op, ta, tb; bool stub = false; };
static std::vector<EvalSlot> g_eval;
static void evalSlot(size_t i, GDExtensionConstTypePtr l, GDExtensionConstTypePtr r, GDExtensionTypePtr ret) {
    const EvalSlot& s = g_eval[i];
    if (s.stub) {
        reportStub(std::string("operator ") + kOpNames[s.op] + " on " + typeName(s.ta) + (isUnaryOp(s.op) ? "" : std::string(" and ") + (s.tb == T_NIL ? "Variant" : typeName(s.tb))));
        return;
    }
    V a, b, res;
    // A Nil operand type in extension_api.json means "any Variant" (right operand of ==, and, or ...); the unary operators have no right side.
    const bool unary = isUnaryOp(s.op);
    readNative(s.ta == T_NIL ? kAnyType : s.ta, l, &a.v);
    if (!unary) readNative(s.tb == T_NIL ? kAnyType : s.tb, r, &b.v);
    if (!varEvaluate(s.op, a.ptr(), b.ptr(), &res.v)) return;
    writeNative(&res.v, res.v.type, ret);
}
template <size_t I> struct EvalThunk { static void call(GDExtensionConstTypePtr l, GDExtensionConstTypePtr r, GDExtensionTypePtr ret) { evalSlot(I, l, r, ret); } };
constexpr size_t kEvalPool = 1024;
static const auto kEvalTable = makePool<GDExtensionPtrOperatorEvaluator, kEvalPool, EvalThunk>(std::make_index_sequence<kEvalPool>{});

// A representative value of a type for asking "is this operator valid for these types?".
static void trialValue(int type, Var* out) {
    std::vector<unsigned char> buf(std::max<size_t>(nativeSize(type), 16));
    nativeInit(type, buf.data());
    if (type == T_INT) st<int64_t>(buf.data(), 3);
    else if (type == T_FLOAT) st<double>(buf.data(), 3.0);
    else if (type == T_VECTOR2I || type == T_VECTOR3I || type == T_VECTOR4I) for (size_t i = 0; i < nativeSize(type); i += 4) st<int32_t>(buf.data() + i, 1);
    varSetNative(out, type, buf.data());
    nativeDestroy(type, buf.data());
}

static GDExtensionPtrOperatorEvaluator if_variant_get_ptr_operator_evaluator(GDExtensionVariantOperator op, GDExtensionVariantType ta, GDExtensionVariantType tb) {
    const int tai = int(ta), tbi = int(tb);
    if (!validType(tai) || !validType(tbi)) return nullptr;
    for (size_t i = 0; i < g_eval.size(); ++i)
        if (g_eval[i].op == int(op) && g_eval[i].ta == tai && g_eval[i].tb == tbi) return kEvalTable[i];
    Var a, b, r;
    if (tai == T_OBJECT) { varNil(&a); a.type = T_OBJECT; } else trialValue(tai, &a);
    if (tbi == T_OBJECT) { varNil(&b); b.type = T_OBJECT; } else if (tbi == T_NIL) varNil(&b); else trialValue(tbi, &b);
    varNil(&r);
    const bool ok = varEvaluate(int(op), &a, &b, &r);
    varDestroy(&a); varDestroy(&b); varDestroy(&r);
    if (!ok && !dumpKnowsOperator(int(op), tai, tbi)) return nullptr;
    if (g_eval.size() >= kEvalPool) { logOnce("eval-pool", "operator evaluator table is full"); return nullptr; }
    g_eval.push_back({int(op), tai, tbi, !ok});
    return kEvalTable[g_eval.size() - 1];
}

struct MethodSlot { int type; const BuiltinMethodInfo* info; std::vector<int> args; int ret; std::string name; };   // info null: a stub
static std::vector<MethodSlot> g_methods;
static std::vector<int> parseTypes(const char* sig) {
    std::vector<int> t;
    std::string s = sig ? sig : "";
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) t.push_back(typeFromName(std::string_view(s).substr(i, j - i)));
        i = j + 1;
    }
    return t;
}
static void methodSlot(size_t i, GDExtensionTypePtr base, const GDExtensionConstTypePtr* args, GDExtensionTypePtr ret, int argc) {
    const MethodSlot& s = g_methods[i];
    if (!s.info) { reportStub(std::string(typeName(s.type)) + "." + s.name); return; }
    Borrowed self(s.type, base);
    std::vector<V> vals(size_t(std::max(argc, 1)));
    std::vector<const Var*> ptrs(size_t(std::max(argc, 1)));
    for (int k = 0; k < argc; ++k) {
        const int t = s.info->vararg || size_t(k) >= s.args.size() ? kAnyType : s.args[size_t(k)];
        readNative(t, args[k], &vals[size_t(k)].v);
        ptrs[size_t(k)] = vals[size_t(k)].ptr();
    }
    V res;
    s.info->fn(&self.v, ptrs.data(), argc, &res.v);
    self.writeBack(s.type, base);
    if (s.ret >= 0 && ret) writeNative(&res.v, s.ret == kAnyType ? kAnyType : s.ret, ret);
}
template <size_t I> struct MethodThunk { static void call(GDExtensionTypePtr b, const GDExtensionConstTypePtr* a, GDExtensionTypePtr r, int n) { methodSlot(I, b, a, r, n); } };
constexpr size_t kMethodPool = 1280;     // the dump lists 998 builtin methods
static const auto kMethodTable = makePool<GDExtensionPtrBuiltInMethod, kMethodPool, MethodThunk>(std::make_index_sequence<kMethodPool>{});

static GDExtensionPtrBuiltInMethod if_variant_get_ptr_builtin_method(GDExtensionVariantType type, GDExtensionConstStringNamePtr method, GDExtensionInt hash) {
    const std::string name = nameOfSlot(method);
    if (!validType(type)) return nullptr;
    const BuiltinMethodInfo* info = findBuiltinMethod(int(type), name);
    // not implemented: a stub when Godot has the method (the dump says so), null when it does not
    if (!info && (!host().api.loaded || host().api.builtins[typeName(int(type))].count(name) == 0)) return nullptr;
    if (host().api.loaded) {
        auto& m = host().api.builtins[typeName(int(type))];
        auto it = m.find(name);
        if (it != m.end()) {
            const uint32_t h = uint32_t(hash);
            bool ok = it->second.hash == h;
            for (uint32_t c : it->second.compat) ok = ok || c == h;
            if (!ok) {
                hostLog(2, std::string("builtin method ") + typeName(int(type)) + "." + name + ": hash " + std::to_string(h) + " does not match the API dump (" + std::to_string(it->second.hash) + ")");
                return nullptr;
            }
        }
    }
    for (size_t i = 0; i < g_methods.size(); ++i)
        if (g_methods[i].type == int(type) && g_methods[i].name == name) return kMethodTable[i];
    if (g_methods.size() >= kMethodPool) { logOnce("method-pool", "builtin method table is full"); return nullptr; }
    g_methods.push_back({int(type), info, info ? parseTypes(info->args) : std::vector<int>{}, info && info->ret && *info->ret ? typeFromName(info->ret) : -1, name});
    return kMethodTable[g_methods.size() - 1];
}

struct CtorSlot { int type, index; std::vector<int> args; };
static std::vector<CtorSlot> g_ctors;
static void ctorSlot(size_t i, GDExtensionUninitializedTypePtr base, const GDExtensionConstTypePtr* args) {
    const CtorSlot& s = g_ctors[i];
    std::vector<V> vals(s.args.size() + 1);
    std::vector<const Var*> ptrs(s.args.size() + 1);
    for (size_t k = 0; k < s.args.size(); ++k) {
        readNative(s.args[k], args[k], &vals[k].v);
        ptrs[k] = vals[k].ptr();
    }
    V out;
    if (!varConstructIndex(s.type, s.index, ptrs.data(), &out.v)) { nativeInit(s.type, base); return; }
    varGetNative(&out.v, base);
}
template <size_t I> struct CtorThunk { static void call(GDExtensionUninitializedTypePtr b, const GDExtensionConstTypePtr* a) { ctorSlot(I, b, a); } };
constexpr size_t kCtorPool = 256;
static const auto kCtorTable = makePool<GDExtensionPtrConstructor, kCtorPool, CtorThunk>(std::make_index_sequence<kCtorPool>{});

static GDExtensionPtrConstructor if_variant_get_ptr_constructor(GDExtensionVariantType type, int32_t index) {
    const char* sig = validType(type) ? constructorArgs(int(type), index) : nullptr;
    if (!sig) {
        logOnce("ctor:" + std::to_string(type) + ":" + std::to_string(index), std::string("constructor ") + typeName(int(type)) + "#" + std::to_string(index) + " is not implemented");
        return nullptr;
    }
    for (size_t i = 0; i < g_ctors.size(); ++i)
        if (g_ctors[i].type == int(type) && g_ctors[i].index == index) return kCtorTable[i];
    if (g_ctors.size() >= kCtorPool) { logOnce("ctor-pool", "constructor table is full"); return nullptr; }
    g_ctors.push_back({int(type), index, parseTypes(sig)});
    return kCtorTable[g_ctors.size() - 1];
}

struct MemberSlot { int type; std::string name; int mtype; bool stub = false; };
static std::vector<MemberSlot> g_members;
static void memberGet(size_t i, GDExtensionConstTypePtr base, GDExtensionTypePtr out) {
    const MemberSlot& s = g_members[i];
    if (s.stub) { reportStub(std::string(typeName(s.type)) + "." + s.name + " (get)"); return; }
    Borrowed b(s.type, base);
    V res;
    if (varGetMember(&b.v, s.name, &res.v)) writeNative(&res.v, s.mtype, out);
}
static void memberSet(size_t i, GDExtensionTypePtr base, GDExtensionConstTypePtr value) {
    const MemberSlot& s = g_members[i];
    if (s.stub) { reportStub(std::string(typeName(s.type)) + "." + s.name + " (set)"); return; }
    Borrowed b(s.type, base);
    V val;
    readNative(s.mtype, value, &val.v);
    varSetMember(&b.v, s.name, val.ptr());
    b.writeBack(s.type, base);
}
template <size_t I> struct MemberGetThunk { static void call(GDExtensionConstTypePtr b, GDExtensionTypePtr o) { memberGet(I, b, o); } };
template <size_t I> struct MemberSetThunk { static void call(GDExtensionTypePtr b, GDExtensionConstTypePtr v) { memberSet(I, b, v); } };
constexpr size_t kMemberPool = 256;
static const auto kMemberGetTable = makePool<GDExtensionPtrGetter, kMemberPool, MemberGetThunk>(std::make_index_sequence<kMemberPool>{});
static const auto kMemberSetTable = makePool<GDExtensionPtrSetter, kMemberPool, MemberSetThunk>(std::make_index_sequence<kMemberPool>{});

static int memberSlotFor(int type, const std::string& name) {
    for (size_t i = 0; i < g_members.size(); ++i)
        if (g_members[i].type == type && g_members[i].name == name) return int(i);
    if (!validType(type) || type == T_OBJECT) return -1;
    std::vector<unsigned char> buf(std::max<size_t>(nativeSize(type), 16));
    nativeInit(type, buf.data());
    V base = V::ofNative(type, buf.data());
    nativeDestroy(type, buf.data());
    V probe;
    bool stub = false;
    if (!varGetMember(base.ptr(), name, &probe.v)) {
        // a member Godot has (the dump lists it) and the host does not: a stub, so the plugin's pointer is never null
        const auto& known = host().api.builtinMembers;
        auto it = known.find(typeName(type));
        if (!host().api.loaded || it == known.end() || std::find(it->second.begin(), it->second.end(), name) == it->second.end()) return -1;
        stub = true;
    }
    if (g_members.size() >= kMemberPool) { logOnce("member-pool", "member accessor table is full"); return -1; }
    g_members.push_back({type, name, stub ? kAnyType : probe.v.type, stub});
    return int(g_members.size() - 1);
}
static GDExtensionPtrSetter if_variant_get_ptr_setter(GDExtensionVariantType t, GDExtensionConstStringNamePtr m) { int i = memberSlotFor(int(t), nameOfSlot(m)); return i < 0 ? nullptr : kMemberSetTable[size_t(i)]; }
static GDExtensionPtrGetter if_variant_get_ptr_getter(GDExtensionVariantType t, GDExtensionConstStringNamePtr m) { int i = memberSlotFor(int(t), nameOfSlot(m)); return i < 0 ? nullptr : kMemberGetTable[size_t(i)]; }

struct UtilSlot { const UtilityInfo* info; std::vector<int> args; int ret; std::string name; };   // info null: a stub
static std::vector<UtilSlot> g_utils;
static void utilSlot(size_t i, GDExtensionTypePtr ret, const GDExtensionConstTypePtr* args, int argc) {
    const UtilSlot& s = g_utils[i];
    if (!s.info) { reportStub("utility function " + s.name); return; }
    std::vector<V> vals(size_t(std::max(argc, 1)));
    std::vector<const Var*> ptrs(size_t(std::max(argc, 1)));
    for (int k = 0; k < argc; ++k) {
        const int t = s.info->vararg || size_t(k) >= s.args.size() ? kAnyType : s.args[size_t(k)];
        readNative(t, args[k], &vals[size_t(k)].v);
        ptrs[size_t(k)] = vals[size_t(k)].ptr();
    }
    V res;
    s.info->fn(nullptr, ptrs.data(), argc, &res.v);
    if (s.ret >= 0 && ret) writeNative(&res.v, s.ret, ret);
}
template <size_t I> struct UtilThunk { static void call(GDExtensionTypePtr r, const GDExtensionConstTypePtr* a, int n) { utilSlot(I, r, a, n); } };
constexpr size_t kUtilPool = 160;      // the dump lists 114
static const auto kUtilTable = makePool<GDExtensionPtrUtilityFunction, kUtilPool, UtilThunk>(std::make_index_sequence<kUtilPool>{});

static GDExtensionPtrUtilityFunction if_variant_get_ptr_utility_function(GDExtensionConstStringNamePtr function, GDExtensionInt hash) {
    const std::string name = nameOfSlot(function);
    const UtilityInfo* info = findUtility(name);
    if (!info && (!host().api.loaded || host().api.utilities.count(name) == 0)) return nullptr;      // Godot has no such function either
    if (host().api.loaded) {
        auto it = host().api.utilities.find(name);
        if (it != host().api.utilities.end() && it->second.hash != uint32_t(hash)) {
            hostLog(2, "utility function " + name + ": hash " + std::to_string(uint32_t(hash)) + " does not match the API dump (" + std::to_string(it->second.hash) + ")");
            return nullptr;
        }
    }
    for (size_t i = 0; i < g_utils.size(); ++i) if (g_utils[i].name == name) return kUtilTable[i];
    if (g_utils.size() >= kUtilPool) return nullptr;
    g_utils.push_back({info, info ? parseTypes(info->args) : std::vector<int>{}, info && info->ret && *info->ret ? typeFromName(info->ret) : -1, name});
    return kUtilTable[g_utils.size() - 1];
}

// ---------------------------------------------------------------------------------------------------
// strings

static void if_string_new_with_latin1_chars(GDExtensionUninitializedStringPtr d, const char* s) { st<StrData*>(d, nullptr); setStringSlot(d, latin1ToU32(s, std::strlen(s)), false); }
static void if_string_new_with_utf8_chars(GDExtensionUninitializedStringPtr d, const char* s) { st<StrData*>(d, nullptr); setStringSlot(d, utf8ToU32(s), false); }
static void if_string_new_with_utf16_chars(GDExtensionUninitializedStringPtr d, const char16_t* s) {
    int64_t n = 0;
    while (s[n]) ++n;
    st<StrData*>(d, nullptr);
    setStringSlot(d, utf16ToU32(s, n, true), false);
}
static void if_string_new_with_utf32_chars(GDExtensionUninitializedStringPtr d, const char32_t* s) { st<StrData*>(d, nullptr); setStringSlot(d, std::u32string(s), false); }
static std::u32string wideToU32(const wchar_t* s, int64_t len) {
    if constexpr (sizeof(wchar_t) == 2) return utf16ToU32(reinterpret_cast<const char16_t*>(s), len, true);
    else { std::u32string o; for (int64_t i = 0; i < len; ++i) o.push_back(char32_t(s[i])); return o; }
}
static void if_string_new_with_wide_chars(GDExtensionUninitializedStringPtr d, const wchar_t* s) { st<StrData*>(d, nullptr); setStringSlot(d, wideToU32(s, int64_t(std::wcslen(s))), false); }
static void if_string_new_with_latin1_chars_and_len(GDExtensionUninitializedStringPtr d, const char* s, GDExtensionInt n) { st<StrData*>(d, nullptr); setStringSlot(d, latin1ToU32(s, n), false); }
static GDExtensionInt if_string_new_with_utf8_chars_and_len2(GDExtensionUninitializedStringPtr d, const char* s, GDExtensionInt n) {
    st<StrData*>(d, nullptr);
    setStringSlot(d, utf8ToU32(std::string_view(s, size_t(n < 0 ? 0 : n))), false);
    return 0;
}
static void if_string_new_with_utf8_chars_and_len(GDExtensionUninitializedStringPtr d, const char* s, GDExtensionInt n) { if_string_new_with_utf8_chars_and_len2(d, s, n); }
static GDExtensionInt if_string_new_with_utf16_chars_and_len2(GDExtensionUninitializedStringPtr d, const char16_t* s, GDExtensionInt n, GDExtensionBool le) {
    st<StrData*>(d, nullptr);
    setStringSlot(d, utf16ToU32(s, n, le != 0), false);
    return 0;
}
static void if_string_new_with_utf16_chars_and_len(GDExtensionUninitializedStringPtr d, const char16_t* s, GDExtensionInt n) { if_string_new_with_utf16_chars_and_len2(d, s, n, 1); }
static void if_string_new_with_utf32_chars_and_len(GDExtensionUninitializedStringPtr d, const char32_t* s, GDExtensionInt n) { st<StrData*>(d, nullptr); setStringSlot(d, std::u32string(s, size_t(n)), false); }
static void if_string_new_with_wide_chars_and_len(GDExtensionUninitializedStringPtr d, const wchar_t* s, GDExtensionInt n) { st<StrData*>(d, nullptr); setStringSlot(d, wideToU32(s, n), false); }

static GDExtensionInt if_string_to_latin1_chars(GDExtensionConstStringPtr self, char* out, GDExtensionInt max) {
    std::u32string s = stringSlot(self);
    if (out) for (GDExtensionInt i = 0; i < max && size_t(i) < s.size(); ++i) out[i] = s[size_t(i)] < 256 ? char(s[size_t(i)]) : '?';
    return GDExtensionInt(s.size());
}
static GDExtensionInt if_string_to_utf8_chars(GDExtensionConstStringPtr self, char* out, GDExtensionInt max) {
    std::string u = u32ToUtf8(stringSlot(self));
    if (out && max > 0) std::memcpy(out, u.data(), std::min<size_t>(u.size(), size_t(max)));
    return GDExtensionInt(u.size());
}
static GDExtensionInt if_string_to_utf16_chars(GDExtensionConstStringPtr self, char16_t* out, GDExtensionInt max) {
    std::u32string s = stringSlot(self);
    std::u16string u;
    for (char32_t c : s) {
        if (c >= 0x10000) { c -= 0x10000; u.push_back(char16_t(0xD800 + (c >> 10))); u.push_back(char16_t(0xDC00 + (c & 0x3FF))); }
        else u.push_back(char16_t(c));
    }
    if (out) for (GDExtensionInt i = 0; i < max && size_t(i) < u.size(); ++i) out[i] = u[size_t(i)];
    return GDExtensionInt(u.size());
}
static GDExtensionInt if_string_to_utf32_chars(GDExtensionConstStringPtr self, char32_t* out, GDExtensionInt max) {
    std::u32string s = stringSlot(self);
    if (out) for (GDExtensionInt i = 0; i < max && size_t(i) < s.size(); ++i) out[i] = s[size_t(i)];
    return GDExtensionInt(s.size());
}
static GDExtensionInt if_string_to_wide_chars(GDExtensionConstStringPtr self, wchar_t* out, GDExtensionInt max) {
    if constexpr (sizeof(wchar_t) == 2) return if_string_to_utf16_chars(self, reinterpret_cast<char16_t*>(out), max);
    else return if_string_to_utf32_chars(self, reinterpret_cast<char32_t*>(out), max);
}
static char32_t* if_string_operator_index(GDExtensionStringPtr self, GDExtensionInt i) {
    auto* s = ld<StrData*>(self);
    return s && i >= 0 && size_t(i) < s->s.size() ? &s->s[size_t(i)] : nullptr;
}
static const char32_t* if_string_operator_index_const(GDExtensionConstStringPtr self, GDExtensionInt i) {
    auto* s = ld<StrData*>(self);
    return s && i >= 0 && size_t(i) < s->s.size() ? &s->s[size_t(i)] : nullptr;
}
static void appendTo(void* self, const std::u32string& add) {
    auto* s = ld<StrData*>(self);
    if (!s) { st<StrData*>(self, new StrData{add}); return; }
    s->s += add;
}
static void if_string_operator_plus_eq_string(GDExtensionStringPtr self, GDExtensionConstStringPtr b) { appendTo(self, stringSlot(b)); }
static void if_string_operator_plus_eq_char(GDExtensionStringPtr self, char32_t c) { appendTo(self, std::u32string(1, c)); }
static void if_string_operator_plus_eq_cstr(GDExtensionStringPtr self, const char* s) { appendTo(self, latin1ToU32(s, int64_t(std::strlen(s)))); }
static void if_string_operator_plus_eq_wcstr(GDExtensionStringPtr self, const wchar_t* s) { appendTo(self, wideToU32(s, int64_t(std::wcslen(s)))); }
static void if_string_operator_plus_eq_c32str(GDExtensionStringPtr self, const char32_t* s) { appendTo(self, std::u32string(s)); }
static GDExtensionInt if_string_resize(GDExtensionStringPtr self, GDExtensionInt n) {
    auto* s = ld<StrData*>(self);
    if (n < 0) return 31;
    if (!s) { if (n == 0) return 0; s = new StrData(); st<StrData*>(self, s); }
    s->s.resize(size_t(n), U'\0');
    return 0;
}
static void if_string_name_new_with_latin1_chars(GDExtensionUninitializedStringNamePtr d, const char* s, GDExtensionBool) { storeName(d, s); }
static void if_string_name_new_with_utf8_chars(GDExtensionUninitializedStringNamePtr d, const char* s) { storeName(d, s); }
static void if_string_name_new_with_utf8_chars_and_len(GDExtensionUninitializedStringNamePtr d, const char* s, GDExtensionInt n) { storeName(d, std::string_view(s, size_t(n < 0 ? 0 : n))); }

// ---------------------------------------------------------------------------------------------------
// arrays and dictionaries

static void* packedAt(void* self, GDExtensionInt i, bool writable) {
    auto* p = ld<PackedData*>(self);
    if (!p || i < 0 || size_t(i) >= p->count()) return nullptr;
    if (writable) { packedDetach(p); st<PackedData*>(self, p); }
    return p->bytes.data() + size_t(i) * p->esize;
}
#define PACKED_INDEX(NAME, TYPE) \
    static TYPE* if_packed_##NAME##_operator_index(GDExtensionTypePtr s, GDExtensionInt i) { return static_cast<TYPE*>(packedAt(s, i, true)); } \
    static const TYPE* if_packed_##NAME##_operator_index_const(GDExtensionConstTypePtr s, GDExtensionInt i) { return static_cast<const TYPE*>(packedAt(const_cast<void*>(s), i, false)); }
PACKED_INDEX(byte_array, uint8_t)
PACKED_INDEX(float32_array, float)
PACKED_INDEX(float64_array, double)
PACKED_INDEX(int32_array, int32_t)
PACKED_INDEX(int64_array, int64_t)
#undef PACKED_INDEX
#define PACKED_INDEX_PTR(NAME) \
    static GDExtensionTypePtr if_packed_##NAME##_operator_index(GDExtensionTypePtr s, GDExtensionInt i) { return packedAt(s, i, true); } \
    static GDExtensionTypePtr if_packed_##NAME##_operator_index_const(GDExtensionConstTypePtr s, GDExtensionInt i) { return packedAt(const_cast<void*>(s), i, false); }
PACKED_INDEX_PTR(string_array)
PACKED_INDEX_PTR(vector2_array)
PACKED_INDEX_PTR(vector3_array)
PACKED_INDEX_PTR(vector4_array)
PACKED_INDEX_PTR(color_array)
#undef PACKED_INDEX_PTR

static GDExtensionVariantPtr arrayAt(void* self, GDExtensionInt i) {
    auto* a = ld<ArrayData*>(self);
    if (!a) return nullptr;
    if (i < 0) i += GDExtensionInt(a->items.size());
    return i >= 0 && size_t(i) < a->items.size() ? &a->items[size_t(i)] : nullptr;
}
static GDExtensionVariantPtr if_array_operator_index(GDExtensionTypePtr s, GDExtensionInt i) { return arrayAt(s, i); }
static GDExtensionVariantPtr if_array_operator_index_const(GDExtensionConstTypePtr s, GDExtensionInt i) { return arrayAt(const_cast<void*>(s), i); }
static void if_array_ref(GDExtensionTypePtr self, GDExtensionConstTypePtr from) {
    nativeDestroy(T_ARRAY, self);
    nativeCopy(T_ARRAY, self, from);
}
static void if_array_set_typed(GDExtensionTypePtr self, GDExtensionVariantType type, GDExtensionConstStringNamePtr cls, GDExtensionConstVariantPtr) {
    auto* a = ld<ArrayData*>(self);
    a->elemType = int(type);
    a->elemClass = nameOfSlot(cls);
}
static GDExtensionVariantPtr if_dictionary_operator_index(GDExtensionTypePtr self, GDExtensionConstVariantPtr key) { return dictSlot(ld<DictData*>(self), CVP(key)); }
static GDExtensionVariantPtr if_dictionary_operator_index_const(GDExtensionConstTypePtr self, GDExtensionConstVariantPtr key) {
    static Var none;
    DictData* d = ld<DictData*>(self);
    int i = dictFind(d, CVP(key));
    if (i < 0) { varDestroy(&none); return &none; }
    return &d->items[size_t(i)].second;
}
static void if_dictionary_set_typed(GDExtensionTypePtr, GDExtensionVariantType, GDExtensionConstStringNamePtr, GDExtensionConstVariantPtr,
                                    GDExtensionVariantType, GDExtensionConstStringNamePtr, GDExtensionConstVariantPtr) {}

// ---------------------------------------------------------------------------------------------------
// functions the host does not really provide. godot-cpp refuses to start when get_proc_address answers null for a name it
// loads, so every name of the interface gets a function: these report the call (once in the log, and in the diagnostics list)
// and return the neutral value.

static void noteStub(const char* name) {
    Host& h = host();
    const std::string n = std::string("stub:") + name;
    if (std::find(h.unsupportedCalls.begin(), h.unsupportedCalls.end(), name) == h.unsupportedCalls.end()) h.unsupportedCalls.push_back(name);
    logOnce(n, std::string("interface function '") + name + "' was called and is not implemented by the Aver host");
}

static GDExtensionInt if_xml_parser_open_buffer(GDExtensionObjectPtr, const uint8_t*, size_t) { noteStub("xml_parser_open_buffer"); return kErrUnavailable; }
static void if_file_access_store_buffer(GDExtensionObjectPtr, const uint8_t*, uint64_t) { noteStub("file_access_store_buffer"); }
static uint64_t if_file_access_get_buffer(GDExtensionConstObjectPtr, uint8_t*, uint64_t) { noteStub("file_access_get_buffer"); return 0; }
static uint8_t* if_image_ptrw(GDExtensionObjectPtr) { noteStub("image_ptrw"); return nullptr; }
static const uint8_t* if_image_ptr(GDExtensionObjectPtr) { noteStub("image_ptr"); return nullptr; }
static GDExtensionScriptInstancePtr if_script_instance_create(const GDExtensionScriptInstanceInfo*, GDExtensionScriptInstanceDataPtr) { noteStub("script_instance_create"); return nullptr; }
static GDExtensionScriptInstancePtr if_script_instance_create2(const GDExtensionScriptInstanceInfo2*, GDExtensionScriptInstanceDataPtr) { noteStub("script_instance_create2"); return nullptr; }
static GDExtensionScriptInstancePtr if_script_instance_create3(const GDExtensionScriptInstanceInfo3*, GDExtensionScriptInstanceDataPtr) { noteStub("script_instance_create3"); return nullptr; }
static GDExtensionScriptInstancePtr if_placeholder_script_instance_create(GDExtensionObjectPtr, GDExtensionObjectPtr, GDExtensionObjectPtr) { noteStub("placeholder_script_instance_create"); return nullptr; }
static void if_placeholder_script_instance_update(GDExtensionScriptInstancePtr, GDExtensionConstTypePtr, GDExtensionConstTypePtr) { noteStub("placeholder_script_instance_update"); }
static GDExtensionScriptInstanceDataPtr if_object_get_script_instance(GDExtensionConstObjectPtr, GDExtensionObjectPtr) { noteStub("object_get_script_instance"); return nullptr; }
static void if_object_set_script_instance(GDExtensionObjectPtr, GDExtensionScriptInstanceDataPtr) { noteStub("object_set_script_instance"); }

// The worker thread pool runs a task on the calling thread, at once, and reports it done (task id 1).
static int64_t if_worker_thread_pool_add_native_group_task(GDExtensionObjectPtr, GDExtensionWorkerThreadPoolGroupTask fn, void* userdata, int elements, int,
                                                           GDExtensionBool, GDExtensionConstStringPtr) {
    for (int i = 0; i < elements; ++i) fn(userdata, uint32_t(i));
    return 1;
}
static int64_t if_worker_thread_pool_add_native_task(GDExtensionObjectPtr, GDExtensionWorkerThreadPoolTask fn, void* userdata, GDExtensionBool, GDExtensionConstStringPtr) {
    fn(userdata);
    return 1;
}

// ---------------------------------------------------------------------------------------------------
// the table

#define E(name) {#name, reinterpret_cast<GDExtensionInterfaceFunctionPtr>(&if_##name)}
static const std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr>& coreTable() {
    static const std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr> t = {
        E(get_godot_version), E(get_godot_version2), E(mem_alloc), E(mem_realloc), E(mem_free),
        E(print_error), E(print_error_with_message), E(print_warning), E(print_warning_with_message),
        {"print_script_error", reinterpret_cast<GDExtensionInterfaceFunctionPtr>(&if_print_error)},
        {"print_script_error_with_message", reinterpret_cast<GDExtensionInterfaceFunctionPtr>(&if_print_error_with_message)},
        E(get_native_struct_size),
        E(variant_new_copy), E(variant_new_nil), E(variant_destroy), E(variant_call), E(variant_call_static), E(variant_evaluate),
        E(variant_set), E(variant_set_named), E(variant_set_keyed), E(variant_set_indexed),
        E(variant_get), E(variant_get_named), E(variant_get_keyed), E(variant_get_indexed),
        E(variant_iter_init), E(variant_iter_next), E(variant_iter_get),
        E(variant_hash), E(variant_recursive_hash), E(variant_hash_compare), E(variant_booleanize), E(variant_duplicate),
        E(variant_stringify), E(variant_get_type), E(variant_has_method), E(variant_has_member), E(variant_has_key),
        E(variant_get_object_instance_id), E(variant_get_type_name), E(variant_can_convert), E(variant_can_convert_strict),
        E(get_variant_from_type_constructor), E(get_variant_to_type_constructor), E(variant_get_ptr_internal_getter),
        E(variant_get_ptr_operator_evaluator), E(variant_get_ptr_builtin_method), E(variant_get_ptr_constructor),
        E(variant_get_ptr_destructor), E(variant_construct), E(variant_get_ptr_setter), E(variant_get_ptr_getter),
        E(variant_get_ptr_indexed_setter), E(variant_get_ptr_indexed_getter), E(variant_get_ptr_keyed_setter),
        E(variant_get_ptr_keyed_getter), E(variant_get_ptr_keyed_checker), E(variant_get_constant_value),
        E(variant_get_ptr_utility_function),
        E(string_new_with_latin1_chars), E(string_new_with_utf8_chars), E(string_new_with_utf16_chars), E(string_new_with_utf32_chars),
        E(string_new_with_wide_chars), E(string_new_with_latin1_chars_and_len), E(string_new_with_utf8_chars_and_len),
        E(string_new_with_utf8_chars_and_len2), E(string_new_with_utf16_chars_and_len), E(string_new_with_utf16_chars_and_len2),
        E(string_new_with_utf32_chars_and_len), E(string_new_with_wide_chars_and_len),
        E(string_to_latin1_chars), E(string_to_utf8_chars), E(string_to_utf16_chars), E(string_to_utf32_chars), E(string_to_wide_chars),
        E(string_operator_index), E(string_operator_index_const), E(string_operator_plus_eq_string), E(string_operator_plus_eq_char),
        E(string_operator_plus_eq_cstr), E(string_operator_plus_eq_wcstr), E(string_operator_plus_eq_c32str), E(string_resize),
        E(string_name_new_with_latin1_chars), E(string_name_new_with_utf8_chars), E(string_name_new_with_utf8_chars_and_len),
        E(packed_byte_array_operator_index), E(packed_byte_array_operator_index_const),
        E(packed_float32_array_operator_index), E(packed_float32_array_operator_index_const),
        E(packed_float64_array_operator_index), E(packed_float64_array_operator_index_const),
        E(packed_int32_array_operator_index), E(packed_int32_array_operator_index_const),
        E(packed_int64_array_operator_index), E(packed_int64_array_operator_index_const),
        E(packed_string_array_operator_index), E(packed_string_array_operator_index_const),
        E(packed_vector2_array_operator_index), E(packed_vector2_array_operator_index_const),
        E(packed_vector3_array_operator_index), E(packed_vector3_array_operator_index_const),
        E(packed_vector4_array_operator_index), E(packed_vector4_array_operator_index_const),
        E(packed_color_array_operator_index), E(packed_color_array_operator_index_const),
        E(array_operator_index), E(array_operator_index_const), E(array_ref), E(array_set_typed),
        E(dictionary_operator_index), E(dictionary_operator_index_const), E(dictionary_set_typed),
        E(xml_parser_open_buffer), E(file_access_store_buffer), E(file_access_get_buffer), E(image_ptrw), E(image_ptr),
        E(worker_thread_pool_add_native_group_task), E(worker_thread_pool_add_native_task),
        E(script_instance_create), E(script_instance_create2), E(script_instance_create3),
        E(placeholder_script_instance_create), E(placeholder_script_instance_update),
        E(object_get_script_instance), E(object_set_script_instance),
    };
    return t;
}
#undef E

GDExtensionInterfaceFunctionPtr procAddress(const char* name) {
    if (!name) return nullptr;
    const auto& core = coreTable();
    if (auto it = core.find(name); it != core.end()) return it->second;
    if (auto fn = classDbFunction(name)) return fn;
    // a name outside the 4.5 interface (an extension built for a later Godot): null, logged once, listed
    Host& h = host();
    const std::string n(name);
    if (std::find(h.unsupportedCalls.begin(), h.unsupportedCalls.end(), n) == h.unsupportedCalls.end()) h.unsupportedCalls.push_back(n);
    logOnce("if:" + n, std::string("interface function '") + n + "' is not part of the 4.5 interface this host serves");
    return nullptr;
}

} // namespace aver::gdext
