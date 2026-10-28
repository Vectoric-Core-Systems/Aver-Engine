#pragma once
// The GDExtension host: library loading, the class registry, the object model and the call paths
// (docs/plans/fabric/G3_GDEXT.md). One process-wide Host; the host is single threaded (the game thread).
#include "GdextValue.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::gdext {

struct ClassDef;
struct ExtLibrary;
struct MethodDef;

struct ArgInfo {
    int type = T_NIL;               // variant type; kAnyType is a whole Variant
    std::string name;
    std::string className;
    uint32_t hint = 0;
    uint32_t usage = 6;
    std::string hintString;
};

using NativeFn = void (*)(HostObject* self, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err);

struct MethodDef {
    std::string name;
    ClassDef* owner = nullptr;
    uint32_t flags = GDEXTENSION_METHOD_FLAG_NORMAL;
    bool vararg = false, isConst = false, isStatic = false, isVirtual = false;
    std::vector<ArgInfo> args;
    bool hasRet = false;
    ArgInfo ret;
    std::vector<V> defaults;                                   // for the trailing arguments
    void* userdata = nullptr;                                  // extension methods
    GDExtensionClassMethodCall callFn = nullptr;
    GDExtensionClassMethodPtrCall ptrFn = nullptr;
    NativeFn native = nullptr;                                 // engine methods
};

struct PropDef {
    ArgInfo info;
    std::string setter, getter;
    int64_t index = -1;
    std::string group;                                         // the group/subgroup marker in force when it was registered
};

struct SignalDef { std::string name; std::vector<ArgInfo> args; };
struct ConstDef { std::string enumName, name; int64_t value = 0; bool bitfield = false; };

// The class-creation info of every registration version, normalised.
struct ClassFuncs {
    GDExtensionClassSet set = nullptr;
    GDExtensionClassGet get = nullptr;
    GDExtensionClassGetPropertyList getPropertyList = nullptr;
    GDExtensionClassFreePropertyList freePropertyList = nullptr;
    GDExtensionClassFreePropertyList2 freePropertyList2 = nullptr;
    GDExtensionClassNotification2 notification = nullptr;
    GDExtensionClassNotification notification1 = nullptr;
    GDExtensionClassToString toString = nullptr;
    GDExtensionClassReference reference = nullptr;
    GDExtensionClassUnreference unreference = nullptr;
    GDExtensionClassCreateInstance create = nullptr;
    GDExtensionClassCreateInstance2 create2 = nullptr;
    GDExtensionClassFreeInstance free = nullptr;
    GDExtensionClassRecreateInstance recreate = nullptr;
    GDExtensionClassGetVirtual getVirtual = nullptr;
    GDExtensionClassGetVirtual2 getVirtual2 = nullptr;
    GDExtensionClassGetVirtualCallData getVirtualData = nullptr;
    GDExtensionClassGetVirtualCallData2 getVirtualData2 = nullptr;
    GDExtensionClassCallVirtualWithData callVirtualWithData = nullptr;
    GDExtensionClassGetRID getRid = nullptr;
    void* classUserdata = nullptr;
};

struct VirtualSlot {
    GDExtensionClassCallVirtual fn = nullptr;
    void* data = nullptr;           // call_virtual_with_data
    bool withData = false;
    bool present() const { return fn || withData; }
};

struct ClassDef {
    std::string name, parent;
    bool engine = false;
    bool isVirtual = false, isAbstract = false, isExposed = true, isRuntime = false;
    bool refCounted = false;
    ExtLibrary* lib = nullptr;
    ClassDef* parentDef = nullptr;
    ClassFuncs f;
    std::vector<std::unique_ptr<MethodDef>> methods;
    std::unordered_map<std::string, MethodDef*> methodByName;
    std::vector<PropDef> props;
    std::vector<SignalDef> signals;
    std::vector<ConstDef> consts;
    std::vector<std::string> virtualNames;                     // register_extension_class_virtual_method
    std::unordered_map<std::string, VirtualSlot> virtualCache; // by virtual name
    std::string currentGroup;
    MethodDef* addMethod(std::unique_ptr<MethodDef> m);
};

struct Binding { void* token; void* binding; GDExtensionInstanceBindingCallbacks cb; };
struct Conn { V callable; uint32_t flags = 0; };
struct Listener {
    void (*fn)(void* user, HostObject* from, const char* signal, const Var* const* args, int argc);
    void* user;
};

struct HostObject {
    uint64_t id = 0;
    int32_t handle = 0;
    std::string cls;                                           // most derived class name
    ClassDef* def = nullptr;
    void* instance = nullptr;                                  // the extension's instance for an extension class
    bool freed = false, queuedFree = false;
    int refcount = 0;
    std::vector<Binding> bindings;
    std::map<std::string, std::vector<Conn>> conns;
    std::vector<Listener> listeners;
    std::map<std::string, V> meta;
    int64_t managed = 0;                                       // a slot for the embedding side (the C# peer)
    // Node
    std::string name;
    HostObject* parent = nullptr;
    std::vector<HostObject*> children;
    std::vector<HostObject*> pendingAdds;                      // children added natively since the embedder last asked
    std::vector<std::string> groups;
    HostObject* owner = nullptr;
    bool inTree = false, ready = false, process = false, physics = false;
    int processMode = 0;
    // Node3D: rows of the basis, then the origin
    float xform[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    bool visible = true;
    uint32_t xformVersion = 0;
    // Resource
    std::string resPath, resName;
};

struct ExtLibrary {
    int32_t handle = 0;
    std::string gdextPath, libraryPath, entrySymbol;
    void* module = nullptr;
    GDExtensionInitialization init{};
    int levelInitialized = -1;       // -1: none
    bool open = false;
    std::vector<std::string> classes;                          // registered by this library, in order
    std::vector<int> levelsSeen;                               // the initialize levels called, in order
    GDExtensionMainLoopCallbacks mainLoop{};
    bool hasMainLoop = false;
};

struct ApiMethod {
    uint32_t hash = 0;
    std::vector<uint32_t> compat;
    bool isVirtual = false, vararg = false;
};

// extension_api.json, reduced to what hash checks need.
struct ApiDump {
    bool loaded = false;
    std::string path;
    std::unordered_map<std::string, std::string> parent;                                      // class -> parent
    std::unordered_map<std::string, std::unordered_map<std::string, ApiMethod>> classes;     // class -> method -> info
    std::unordered_map<std::string, std::unordered_map<std::string, ApiMethod>> builtins;    // builtin type name -> method -> info
    std::unordered_map<std::string, ApiMethod> utilities;
    std::unordered_map<std::string, std::vector<std::string>> builtinMembers;                 // builtin type name -> member names
    struct Operator { std::string name, right; };                                              // right: a type name, "Variant", or empty (unary)
    std::unordered_map<std::string, std::vector<Operator>> builtinOperators;
    std::unordered_map<std::string, int> nativeStructSizes;
    bool load(const std::string& file, std::string* err);
};

struct MethodBind {
    ClassDef* cls = nullptr;
    MethodDef* method = nullptr;
    std::string name;
};

// ---- the host ----

struct Host {
    std::unordered_map<std::string, std::unique_ptr<ClassDef>> classes;
    std::vector<std::string> classOrder;                       // extension classes, registration order
    std::vector<std::unique_ptr<ExtLibrary>> libs;
    std::unordered_map<uint64_t, HostObject*> byId;
    std::unordered_map<int32_t, HostObject*> byHandle;
    std::vector<std::unique_ptr<HostObject>> tombstones;       // freed objects, kept so a stale pointer stays safe to read
    std::unordered_map<uint64_t, std::unique_ptr<HostObject>> live;
    std::map<std::string, std::unique_ptr<MethodBind>> binds;
    std::unordered_map<std::string, HostObject*> singletons;
    std::vector<std::function<void()>> deferred;
    std::unordered_set<std::string> loggedOnce;
    std::vector<std::string> unsupportedCalls;                 // interface names asked for and not implemented, first-asked order
    std::vector<std::string> unsupportedBuiltins;
    std::vector<std::string> unsupportedMethods;               // "Class::method" engine methods asked for and not implemented
    ApiDump api;
    std::vector<std::string> features;                         // feature tags for .gdextension library selection
    bool featuresSet = false;
    uint64_t nextId = 0x0200000001ULL;
    int32_t nextHandle = 1;
    int32_t nextLib = 1;
    std::string lastError;
    ExtLibrary* current = nullptr;                             // the library whose entry/initialize/deinitialize is running
    bool engineReady = false;
};

Host& host();
void ensureEngineClasses();                                    // GdextEngine.cpp

void hostError(const std::string& msg);                        // sets lastError and logs
void logOnce(const std::string& key, const std::string& msg);  // a warning, once per key
void hostLog(int level, const std::string& text);              // 0 info 1 warn 2 error

// ---- classes ----
ClassDef* findClass(std::string_view name);
bool classIsA(const ClassDef* c, std::string_view base);
MethodDef* findMethod(const ClassDef* c, std::string_view name);   // walks the parents
const SignalDef* findSignal(const ClassDef* c, std::string_view name);
const PropDef* findProp(const ClassDef* c, std::string_view name);
ClassDef* defineEngineClass(const char* name, const char* parent, bool refCounted);
MethodDef* defineNative(ClassDef* c, const char* name, const char* args, const char* ret, NativeFn fn, bool vararg = false);
// A method signature given as text: "StringName Callable int" -> arg types, "" -> none.
void parseSig(MethodDef& m, const char* args, const char* ret);

// ---- objects ----
HostObject* newObject(ClassDef* def, const std::string& name = {});
HostObject* instantiate(std::string_view className, bool forScript = false);   // engine or extension class by name; null if abstract / unknown
HostObject* objectById(uint64_t id);
HostObject* objectByHandle(int32_t handle);
void destroyObject(HostObject* o);
void flushDeferred();
std::string objectToString(HostObject* o);

// ---- calls ----
// `ret` is an initialised Variant (nil). Returns false and fills err on failure.
bool callMethod(HostObject* self, MethodDef* m, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err);
bool callByName(HostObject* self, std::string_view name, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err);
// Pointer-call convention: `args` are native values (int64, double, uint8, builtin memory, object pointers, Var* for kAnyType).
bool callPtr(HostObject* self, MethodDef* m, const void* const* args, void* ret);
bool callCallable(const Var* callable, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err);
V makeMethodCallable(HostObject* o, std::string_view method);
V makeCustomCallable(const GDExtensionCallableCustomInfo2& info);

// Native <-> Variant in the pointer-call convention (int = int64, float = double, bool = uint8, objects by pointer,
// kAnyType = a Variant). `readNative` builds `out` (uninitialised); `writeNative` stores into an initialised value.
void readNative(int type, const void* p, Var* out);
void writeNative(const Var* v, int type, void* dst);

// ---- properties, signals, virtuals ----
bool getProperty(HostObject* o, std::string_view name, Var* out);   // out uninitialised
bool setProperty(HostObject* o, std::string_view name, const Var* value);
int connectSignal(HostObject* o, std::string_view signal, const Var* callable, uint32_t flags);
int disconnectSignal(HostObject* o, std::string_view signal, const Var* callable);
int emitSignal(HostObject* o, std::string_view signal, const Var* const* args, int argc);
void addListener(HostObject* o, Listener l);
VirtualSlot* findVirtual(HostObject* o, std::string_view name);
// Calls an extension virtual with native arguments; false when the object does not implement it.
bool callVirtual(HostObject* o, std::string_view name, const void* const* args, void* ret);
void sendNotification(HostObject* o, int what, bool reversed);

// ---- libraries ----
ExtLibrary* loadExtension(const std::string& gdextPath, const std::string& projectDir, std::string* err);
bool unloadExtension(ExtLibrary* lib, std::string* err);
ExtLibrary* findLibrary(int32_t handle);
ExtLibrary* findLibraryByPath(const std::string& gdextPath);
std::vector<std::string> defaultFeatures();
GDExtensionInterfaceFunctionPtr procAddress(const char* name);
bool loadApiDump(const std::string& path, std::string* err);
void unregisterClass(ExtLibrary* lib, const std::string& name);

// ---- the native node tree (Node / Node3D behind the C ABI) ----
// Lifecycle in Godot's order: enter pre-order, ready post-order, exit children first. `recursive` false touches one node
// (the embedder drives the tree itself, as the C# peers do).
void treeEnter(HostObject* o, bool recursive);
void treeReady(HostObject* o, bool recursive);
void treeExit(HostObject* o, bool recursive);
void treeProcess(HostObject* o, double delta, bool physics, bool recursive);
std::string nodePath(HostObject* o);
HostObject* nodeAtPath(HostObject* from, const std::string& path);
void transformPush(HostObject* o, const float* xform12);       // Node3D local transform: 9 basis floats (rows), 3 origin

// ---- engine-side helpers shared with the interface layer ----
std::string nameOfSlot(const void* stringNameSlot);            // a StringName / String slot as UTF-8
std::string stringOfSlot(const void* stringSlot);
void storeName(void* uninitSlot, std::string_view s);
void storeString(void* uninitSlot, std::string_view s);
ArgInfo argFromPropertyInfo(const GDExtensionPropertyInfo& p);

constexpr int kErrOk = 0, kErrFailed = 1, kErrUnavailable = 2, kErrInvalidParameter = 31;

} // namespace aver::gdext
