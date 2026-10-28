#pragma once
// The value model behind the GDExtension host (docs/plans/fabric/G3_GDEXT.md).
//
// A Godot extension owns the memory of every Variant and builtin it makes (24 bytes for a Variant, 8 for a String, ...)
// and only ever touches it through the function pointers this host hands out, so the layout is ours. Sizes follow the
// "float_64" configuration of extension_api.json (single-precision reals, 64-bit pointers), which is what godot-cpp builds for.
//
//   Var      24 bytes: type, then 16 payload bytes. A builtin of 16 bytes or less lives in the payload as its native bytes;
//            Transform2D, AABB, Basis, Transform3D and Projection are a heap copy and the payload is the pointer.
//   String   8 bytes: StrData* (null = empty), copied deep.      NodePath: the same.
//   StringName 8 bytes: NameData*, interned and immortal, so equality is pointer equality.
//   Array / Dictionary 8 bytes: a reference-counted shared object (Godot's reference semantics).
//   Packed*Array 16 bytes: a reference-counted shared buffer copied on write (value semantics).
//   Callable 16 bytes: CallableData* (null = empty).   Signal 16 bytes: object pointer + StringName.
//   Object 8 bytes: HostObject*. Objects are never freed from under a pointer (a freed one is a tombstone).
#include "gdextension_interface.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::gdext {

constexpr int kTypeCount = GDEXTENSION_VARIANT_TYPE_VARIANT_MAX; // 39

// The "Variant" parameter type of a method signature (a slot that holds a whole Variant), outside the 0..38 range.
constexpr int kAnyType = 100;

enum VType : int32_t {
    T_NIL = GDEXTENSION_VARIANT_TYPE_NIL, T_BOOL, T_INT, T_FLOAT, T_STRING, T_VECTOR2, T_VECTOR2I, T_RECT2, T_RECT2I,
    T_VECTOR3, T_VECTOR3I, T_TRANSFORM2D, T_VECTOR4, T_VECTOR4I, T_PLANE, T_QUATERNION, T_AABB, T_BASIS, T_TRANSFORM3D,
    T_PROJECTION, T_COLOR, T_STRING_NAME, T_NODE_PATH, T_RID, T_OBJECT, T_CALLABLE, T_SIGNAL, T_DICTIONARY, T_ARRAY,
    T_PACKED_BYTE, T_PACKED_INT32, T_PACKED_INT64, T_PACKED_FLOAT32, T_PACKED_FLOAT64, T_PACKED_STRING, T_PACKED_VECTOR2,
    T_PACKED_VECTOR3, T_PACKED_COLOR, T_PACKED_VECTOR4
};

struct Var {
    int32_t type = 0;
    int32_t pad = 0;
    alignas(8) unsigned char d[16] = {};
};
static_assert(sizeof(Var) == 24, "a Variant is 24 bytes in the float_64 configuration");

struct HostObject;

struct StrData  { std::u32string s; };
struct NameData { std::string utf8; };

struct ArrayData {
    int rc = 1;
    std::vector<Var> items;
    int elemType = 0;           // typed arrays: the element variant type (0 = untyped)
    std::string elemClass;
    ~ArrayData();
};

struct DictData {
    int rc = 1;
    std::vector<std::pair<Var, Var>> items; // insertion order, linear lookup
    ~DictData();
};

struct PackedData {
    int rc = 1;
    int type = T_PACKED_BYTE;
    size_t esize = 1;
    std::vector<unsigned char> bytes;
    size_t count() const { return esize ? bytes.size() / esize : 0; }
    ~PackedData();
};

struct CallableData {
    int rc = 1;
    uint64_t objectId = 0;      // method callables: the target
    NameData* method = nullptr;
    bool custom = false;
    GDExtensionCallableCustomInfo2 info{};
    uint32_t hash = 0;
    bool hashed = false;
    ~CallableData();
};

// ---- strings ----

std::u32string utf8ToU32(std::string_view s);
std::string u32ToUtf8(const std::u32string& s);
std::u32string utf16ToU32(const char16_t* s, int64_t len, bool littleEndian);
std::u32string latin1ToU32(const char* s, int64_t len);
NameData* internName(std::string_view utf8);
inline const std::string& nameText(const NameData* n) { static const std::string empty; return n ? n->utf8 : empty; }

// ---- builtins in raw memory ----

size_t nativeSize(int type);
bool nativeIsHeap(int type);        // lives behind a pointer inside a Variant (larger than 16 bytes)
void nativeInit(int type, void* p);                         // default value, p uninitialised
void nativeCopy(int type, void* dst, const void* src);      // dst uninitialised
void nativeDestroy(int type, void* p);
const char* typeName(int type);
int typeFromName(std::string_view name);                    // builtin names, "Variant" = kAnyType, anything else capitalised = T_OBJECT, -1 unknown

// ---- Variants ----

void varNil(Var* v);                                        // v uninitialised
void varCopy(Var* dst, const Var* src);                     // dst uninitialised
void varDestroy(Var* v);                                    // leaves nil
void varAssign(Var* dst, const Var* src);                   // dst initialised
void varSetNative(Var* dst, int type, const void* native);  // dst uninitialised; the native value is copied in
void varGetNative(const Var* v, void* native);              // native uninitialised, of v->type; copied out
void* varPtr(Var* v);                                       // the builtin inside (bool, int and float included)
const void* varPtr(const Var* v);
bool varConvert(const Var* in, int toType, Var* out);       // Godot's implicit construction; false: nothing sensible
bool varCanConvert(int from, int to, bool strict);
bool varEqual(const Var* a, const Var* b);
bool varLess(const Var* a, const Var* b, bool* valid);
int64_t varHash(const Var* v);
std::string varStringify(const Var* v);
bool varBooleanize(const Var* v);
bool varEvaluate(int op, const Var* a, const Var* b, Var* ret); // ret uninitialised; false: invalid operands
HostObject* varObject(const Var* v);                        // null for nil, other types and freed objects

// Godot's `str(float)`: the shortest text that reads back, with ".0" on whole numbers.
std::string floatText(double d);

// ---- owning wrapper ----

class V {
public:
    Var v;
    V() { varNil(&v); }
    V(const V& o) { varCopy(&v, &o.v); }
    V(V&& o) noexcept : v(o.v) { varNil(&o.v); }
    V& operator=(const V& o) { if (this != &o) varAssign(&v, &o.v); return *this; }
    V& operator=(V&& o) noexcept { if (this != &o) { varDestroy(&v); v = o.v; varNil(&o.v); } return *this; }
    ~V() { varDestroy(&v); }

    static V ofBool(bool b);
    static V ofInt(int64_t i);
    static V ofFloat(double f);
    static V ofString(std::string_view utf8);
    static V ofName(std::string_view utf8);
    static V ofObject(HostObject* o);
    static V ofNative(int type, const void* native);
    static V ofVec(int type, std::initializer_list<float> comps);

    int type() const { return v.type; }
    bool isNil() const { return v.type == T_NIL; }
    bool toBool() const { return varBooleanize(&v); }
    int64_t toInt() const;
    double toFloat() const;
    std::string toUtf8() const;                              // String, StringName, NodePath text; anything else stringified
    HostObject* toObject() const { return varObject(&v); }
    const float* floats() const { return static_cast<const float*>(varPtr(&v)); }
    Var* ptr() { return &v; }
    const Var* ptr() const { return &v; }
};

// ---- containers ----

ArrayData* arrayOf(const Var* v);                           // the Array behind an Array Variant (null otherwise)
DictData* dictOf(const Var* v);
PackedData* packedOf(const Var* v);
int dictFind(const DictData* d, const Var* key);            // index or -1
Var* dictSlot(DictData* d, const Var* key);                 // existing or newly inserted (nil) value slot
void packedDetach(PackedData*& p);                          // copy-on-write before a mutation
V makeArray();
V makeDict();
V makePacked(int type);
size_t packedElemSize(int type);

// ---- constructors / operators for the pointer-call tables ----

// `args` are Variants already; the result is built in `out` (uninitialised). False: no such constructor.
bool varConstruct(int type, const Var* const* args, int argc, Var* out, GDExtensionCallError* err);
bool varConstructIndex(int type, int index, const Var* const* args, Var* out);   // the constructor at `index` (extension_api.json order)
// Members of vector-like builtins (x, y, z, w, r, g, b, a, position, size, ...). Types listed in G3_GDEXT.md.
bool varGetMember(const Var* base, std::string_view name, Var* out);
bool varSetMember(Var* base, std::string_view name, const Var* value);
bool varGetIndexed(const Var* base, int64_t index, Var* out, bool* oob);
bool varSetIndexed(Var* base, int64_t index, const Var* value, bool* oob);
bool varGetKeyed(const Var* base, const Var* key, Var* out);
bool varSetKeyed(Var* base, const Var* key, const Var* value);
bool varHasKey(const Var* base, const Var* key, bool* valid);

// Builtin methods by name (String.length(), Array.append() ...). `self` is borrowed and may be mutated (shared containers).
using BuiltinFn = void (*)(Var* self, const Var* const* args, int argc, Var* ret);
struct BuiltinMethodInfo {
    int type;
    const char* name;
    BuiltinFn fn;
    const char* args;      // space separated type names ("int String"), "" none
    const char* ret;       // return type name, "" none
    bool isConst;
    bool vararg;
};
const BuiltinMethodInfo* findBuiltinMethod(int type, std::string_view name);
size_t builtinMethodCount();                                    // for tests: the table, to compare with extension_api.json
const BuiltinMethodInfo* builtinMethodAt(size_t index);
bool builtinCallByName(int type, Var* self, std::string_view name, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err);

struct UtilityInfo {
    const char* name;
    BuiltinFn fn;          // `self` is null
    const char* args;
    const char* ret;
    bool vararg;
};
const UtilityInfo* findUtility(std::string_view name);
size_t utilityCount();
const UtilityInfo* utilityAt(size_t index);
void setPrintHook(void (*fn)(int level, const std::string& text)); // 0 info, 1 warning, 2 error; default: the engine log

// Constructor signatures of the builtin types (extension_api.json order): argument type names of constructor `index`.
const char* constructorArgs(int type, int index);               // null: not implemented

} // namespace aver::gdext
