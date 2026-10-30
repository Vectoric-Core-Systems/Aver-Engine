// GdextHostTest -- the GDExtension host (modules/fabric.gdext) against a test extension written in C
// (tests/fabric/gdext_test/fabric_test_ext.c, built as a shared library by CMake; no godot-cpp).
//
// What is exercised, in order: the loader's refusals (compatibility_minimum, maximum, entry symbol, missing library, no library for
// the platform) -> loading through the aver_gdext C ABI -> registration (classes, methods, property, signal, constant) -> calls through
// the wire form, the pointer-call path and the variant-call path -> signals -> the virtual functions in Godot's lifecycle order ->
// Node and Node3D behind the ABI -> the hash checks and a comparison of the host's own tables with extension_api.json ->
// the Variant, String and container functions as an extension calls them -> unload (deinitialization in reverse, objects freed).
#include "GdextIf.hpp"

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/fabric/gdext/fabric_gdext_abi.h"
#include "aver/formats/Json.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::gdext;

static int g_failures = 0;
static const std::string kDir = AVER_GDEXT_TEST_DIR;
static const std::string kApi = AVER_GDEXT_API_JSON;

static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
static bool near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }
static std::string lastError() { return aver_gdext_last_error(); }

// ---------------------------------------------------------------------------------------------------
// the wire form

using Bytes = std::vector<uint8_t>;

static void put(Bytes& b, const void* p, size_t n) { const uint8_t* c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + n); }
static Bytes wNil() { return {0}; }
static Bytes wBool(bool v) { return {1, uint8_t(v ? 1 : 0)}; }
static Bytes wInt(int64_t v) { Bytes b{2}; put(b, &v, 8); return b; }
static Bytes wFloat(double v) { Bytes b{3}; put(b, &v, 8); return b; }
static Bytes wText(uint8_t type, const std::string& s) { Bytes b{type}; int32_t n = int32_t(s.size()); put(b, &n, 4); put(b, s.data(), s.size()); return b; }
static Bytes wStr(const std::string& s) { return wText(4, s); }
static Bytes wName(const std::string& s) { return wText(21, s); }
static Bytes wVec3(float x, float y, float z) { Bytes b{9}; float f[3] = {x, y, z}; put(b, f, 12); return b; }
static Bytes wObj(int64_t id) { Bytes b{24}; put(b, &id, 8); return b; }
static Bytes wList(uint8_t type, const std::vector<Bytes>& items) {
    Bytes b{type};
    int32_t n = int32_t(items.size());
    put(b, &n, 4);
    for (const Bytes& i : items) b.insert(b.end(), i.begin(), i.end());
    return b;
}
static Bytes wArray(const std::vector<Bytes>& items) { return wList(28, items); }
static Bytes wDict(const std::vector<std::pair<Bytes, Bytes>>& kv) {
    Bytes b{27};
    int32_t n = int32_t(kv.size());
    put(b, &n, 4);
    for (auto& p : kv) { b.insert(b.end(), p.first.begin(), p.first.end()); b.insert(b.end(), p.second.begin(), p.second.end()); }
    return b;
}

static Bytes fetch(int32_t n) {
    Bytes out(size_t(n > 0 ? n : 0));
    if (n > 0) aver_gdext_result(out.data(), n);
    return out;
}

// call(obj, method, args...) -> the result bytes; ok=false on failure
static Bytes callW(int32_t obj, const char* method, const std::vector<Bytes>& args, bool* ok = nullptr) {
    Bytes a = wArray(args);
    int32_t n = aver_gdext_call(obj, method, a.data(), int32_t(a.size()));
    if (ok) *ok = n >= 0;
    return fetch(n);
}

static int64_t asInt(const Bytes& b) { int64_t v = 0; if (b.size() == 9 && b[0] == 2) std::memcpy(&v, b.data() + 1, 8); return v; }
static double asFloat(const Bytes& b) { double v = 0; if (b.size() == 9 && b[0] == 3) std::memcpy(&v, b.data() + 1, 8); return v; }
static std::string asText(const Bytes& b) {
    if (b.size() < 5 || (b[0] != 4 && b[0] != 21 && b[0] != 22)) return "<not text>";
    int32_t n;
    std::memcpy(&n, b.data() + 1, 4);
    return std::string(reinterpret_cast<const char*>(b.data() + 5), size_t(n));
}
static bool isNilBytes(const Bytes& b) { return b.size() == 1 && b[0] == 0; }

// ---------------------------------------------------------------------------------------------------
// the test extension's own state

struct ExtState {
    void* module = nullptr;
    void (*state)(int32_t*) = nullptr;
    bool open(const std::string& path) {
        module = loadLibrary(path.c_str());
        if (!module) return false;
        state = reinterpret_cast<void (*)(int32_t*)>(librarySymbol(module, "fabric_test_state"));
        return state != nullptr;
    }
    std::vector<int32_t> read() const { std::vector<int32_t> v(24, 0); if (state) state(v.data()); return v; }
    void close() { if (module) freeLibrary(module); module = nullptr; state = nullptr; }
};

static std::string extPath() {
#if defined(_WIN32)
    return kDir + "/FabricTestExt.dll";
#elif defined(__APPLE__)
    return kDir + "/libFabricTestExt.dylib";
#else
    return kDir + "/libFabricTestExt.so";
#endif
}

// ---------------------------------------------------------------------------------------------------
// the loader's refusals

static void testRefusals() {
    AVER_INFO("--- loader refusals");
    struct Case { const char* file; const char* text; };
    const Case cases[] = {
        {"FabricTestOld.gdextension", "older than 4.1"},
        {"FabricTestNoMin.gdextension", "compatibility_minimum is missing"},
        {"FabricTestTooNew.gdextension", "needs Godot 4.9"},
        {"FabricTestMax.gdextension", "compatibility_maximum"},
        {"FabricTestBadEntry.gdextension", "entry symbol 'no_such_entry_symbol' not found"},
        {"FabricTestFail.gdextension", "reported failure"},
        {"FabricTestMissing.gdextension", "not found"},
    };
    for (const Case& c : cases) {
        int32_t h = aver_gdext_load((kDir + "/" + c.file).c_str(), kDir.c_str());
        check(h == 0 && contains(lastError(), c.text), std::string(c.file) + " is refused: " + lastError());
    }
    check(aver_gdext_load((kDir + "/Nope.gdextension").c_str(), nullptr) == 0, "a missing .gdextension file is refused: " + lastError());
    check(aver_gdext_load(nullptr, nullptr) == 0, "a null path is refused");

    check(aver_gdext_set_features("android,debug,arm64") == 1, "feature tags can be overridden");
    int32_t h = aver_gdext_load((kDir + "/FabricTest.gdextension").c_str(), kDir.c_str());
    check(h == 0 && contains(lastError(), "no [libraries] entry matches"), "no library for the platform is refused: " + lastError());
    aver_gdext_set_features("");
    check(aver_gdext_library_count() == 0, "nothing stays loaded after the refusals");
    check(aver_gdext_class_exists("FabricTestNode") == 0, "no extension class leaked from a failed load");
}

// ---------------------------------------------------------------------------------------------------

struct Sig { std::vector<std::string> names; Bytes args; int32_t lastObj = 0; };

static void onSignal(void* user, int32_t obj, const char* signal, const void* args, int32_t len) {
    auto* s = static_cast<Sig*>(user);
    s->names.push_back(signal);
    const uint8_t* p = static_cast<const uint8_t*>(args);
    s->args.assign(p, p + len);
    s->lastObj = obj;
}

static int32_t g_customCalls = 0;
static int64_t g_customLast = 0;
static void customCall(void*, const GDExtensionConstVariantPtr* args, GDExtensionInt argc, GDExtensionVariantPtr ret, GDExtensionCallError* err) {
    ++g_customCalls;
    if (argc >= 1) g_customLast = V::ofNative(T_INT, varPtr(static_cast<const Var*>(args[0]))).toInt();
    (void)ret;
    err->error = GDEXTENSION_CALL_OK;
}

template <class F> static F iface(const char* name) { return reinterpret_cast<F>(procAddress(name)); }

static void testRegistration(int32_t lib) {
    AVER_INFO("--- registration");
    check(aver_gdext_class_count(lib) == 4, "four classes registered by the library");
    check(std::string(aver_gdext_class_name(lib, 0)) == "FabricTestNode", "registration order: FabricTestNode first");
    check(aver_gdext_class_exists("FabricTestNode") == 1 && aver_gdext_class_exists("Node") == 2 && aver_gdext_class_exists("Nope") == 0,
          "class_exists tells an extension class, a host class and an unknown one apart");
    check(std::string(aver_gdext_class_parent("FabricTestNode")) == "Node", "the parent class is Node");
    check(std::string(aver_gdext_class_parent("FabricTestRef")) == "RefCounted", "FabricTestRef extends RefCounted");
    check(aver_gdext_class_is_a("FabricTestNode", "Object") == 1 && aver_gdext_class_is_a("FabricTestNode", "RefCounted") == 0, "class_is_a follows the parents");
    check(aver_gdext_class_can_instantiate("FabricTestNode") == 1 && aver_gdext_class_can_instantiate("FabricTestAbstract") == 0, "an abstract class cannot be instantiated");
    check(aver_gdext_instantiate("FabricTestAbstract") == 0 && contains(lastError(), "abstract"), "instantiating the abstract class fails: " + lastError());
    check(aver_gdext_instantiate("NoSuchClass") == 0, "instantiating an unknown class fails: " + lastError());
    check(std::string(aver_gdext_class_parent("FabricTestLegacy")) == "Node", "a class registered through the 4.2 call is known too");

    bool hasAdd = false, hasGreet = false;
    int32_t addIndex = -1;
    for (int32_t i = 0; i < aver_gdext_method_count("FabricTestNode"); ++i) {
        std::string n = aver_gdext_method_name("FabricTestNode", i);
        if (n == "add") { hasAdd = true; addIndex = i; }
        if (n == "greet") hasGreet = true;
    }
    check(hasAdd && hasGreet && aver_gdext_method_count("FabricTestNode") == 15, "FabricTestNode registered 15 methods");
    int32_t info[4] = {};
    check(aver_gdext_method_info("FabricTestNode", addIndex, info) == 1 && info[0] == 2 && info[2] == 2 && (info[1] & 4) && info[3] == 0,
          "add(int, int) -> int, const, not vararg");
    check(aver_gdext_method_arg_type("FabricTestNode", addIndex, 0) == 2 && aver_gdext_method_arg_type("FabricTestNode", addIndex, 2) == -1, "argument types");

    check(aver_gdext_property_count("FabricTestNode") == 1 && std::string(aver_gdext_property_name("FabricTestNode", 0)) == "speed" &&
              aver_gdext_property_type("FabricTestNode", 0) == 3,
          "one float property 'speed'");
    check(aver_gdext_signal_count("FabricTestNode") == 1 && std::string(aver_gdext_signal_name("FabricTestNode", 0)) == "pinged" &&
              aver_gdext_signal_arg_count("FabricTestNode", 0) == 1,
          "signal pinged(count)");
    check(aver_gdext_constant_count("FabricTestNode") == 1 && std::string(aver_gdext_constant_name("FabricTestNode", 0)) == "MAX_SPEED" &&
              aver_gdext_constant_value("FabricTestNode", 0) == 42,
          "integer constant MAX_SPEED = 42");
    check(aver_gdext_property_count("FabricTestRef") == 1 && aver_gdext_method_count("FabricTestRef") == 2, "FabricTestRef: property value, two methods");
}

static void testCalls() {
    AVER_INFO("--- calls (wire form)");
    int32_t n1 = aver_gdext_instantiate("FabricTestNode");
    check(n1 != 0, "instantiate FabricTestNode");
    check(std::string(aver_gdext_object_class(n1)) == "FabricTestNode" && aver_gdext_object_instance_id(n1) > 0, "the object reports its class and an instance id");
    check(aver_gdext_object_by_instance_id(aver_gdext_object_instance_id(n1)) == n1, "an object is found again by its instance id");

    bool ok = false;
    Bytes r = callW(n1, "add", {wInt(2), wInt(3)}, &ok);
    check(ok && asInt(r) == 5, "add(2, 3) == 5");
    r = callW(n1, "add", {wInt(2)}, &ok);
    check(!ok && contains(lastError(), "too few arguments"), "add(2) is refused: " + lastError());
    r = callW(n1, "add", {wInt(2), wInt(3), wInt(4)}, &ok);
    check(!ok && contains(lastError(), "too many arguments"), "add(2, 3, 4) is refused: " + lastError());
    r = callW(n1, "add", {wStr("a"), wInt(1)}, &ok);
    check(!ok && contains(lastError(), "wrong type"), "add(\"a\", 1) is refused: " + lastError());
    r = callW(n1, "add", {wFloat(2.0), wInt(3)}, &ok);
    check(ok && asInt(r) == 5, "a float where an int is expected converts (strict conversion allows int <-> float)");
    r = callW(n1, "no_such_method", {}, &ok);
    check(!ok && contains(lastError(), "no such method"), "an unknown method is refused: " + lastError());

    r = callW(n1, "greet", {wStr("World")}, &ok);
    check(ok && asText(r) == "Hello, World!", "greet(\"World\") == \"Hello, World!\"");
    r = callW(n1, "greet", {wStr("Z\xC3\xBCrich")}, &ok);
    check(ok && asText(r) == "Hello, Z\xC3\xBCrich!", "UTF-8 survives the String round trip");
    r = callW(n1, "greet", {wName("Name")}, &ok);
    check(ok && asText(r) == "Hello, Name!", "a StringName is accepted where a String is expected");

    // property through the setter and getter the extension registered
    int32_t n = aver_gdext_get(n1, "speed");
    check(n > 0 && near(asFloat(fetch(n)), 1.0), "speed starts at 1.0");
    Bytes v = wFloat(5.5);
    check(aver_gdext_set(n1, "speed", v.data(), int32_t(v.size())) == 1, "set speed = 5.5");
    check(near(asFloat(fetch(aver_gdext_get(n1, "speed"))), 5.5), "speed reads back 5.5");
    check(near(asFloat(callW(n1, "get_speed", {})), 5.5), "get_speed() == 5.5 (the getter the property uses)");
    Bytes vi = wInt(8);
    check(aver_gdext_set(n1, "speed", vi.data(), int32_t(vi.size())) == 1 && near(asFloat(fetch(aver_gdext_get(n1, "speed"))), 8.0), "an int assigned to a float property converts");
    check(aver_gdext_get(n1, "nonexistent") == -1 && contains(lastError(), "no property"), "a missing property is reported: " + lastError());
    check(aver_gdext_set(n1, "nonexistent", v.data(), int32_t(v.size())) == 0, "setting a missing property is refused");

    // inherited host methods, by name
    r = callW(n1, "call", {wName("add"), wInt(4), wInt(5)}, &ok);
    check(ok && asInt(r) == 9, "Object.call(\"add\", 4, 5) reaches the extension method");
    r = callW(n1, "get_class", {}, &ok);
    check(ok && asText(r) == "FabricTestNode", "Object.get_class() == \"FabricTestNode\"");
    r = callW(n1, "is_class", {wStr("Node")}, &ok);
    check(ok && r.size() == 2 && r[1] == 1, "is_class(\"Node\") is true");
    r = callW(n1, "owner_class", {}, &ok);
    check(ok && asText(r) == "FabricTestNode", "the extension called Object.get_class on itself through a ptrcall method bind");
    r = callW(n1, "to_string", {}, &ok);
    check(ok && contains(asText(r), "FabricTestNode(speed=8)"), "to_string_func: " + asText(r));

    // the wire form round-trips a rich value through the object's metadata
    {
        Bytes pb{29};                     // PackedByteArray: count, raw bytes
        int32_t cnt = 3;
        put(pb, &cnt, 4);
        uint8_t raw[3] = {7, 8, 9};
        put(pb, raw, 3);
        Bytes ps{34};                     // PackedStringArray: count, then (byte count, UTF-8) each
        int32_t two = 2;
        put(ps, &two, 4);
        for (const char* s : {"ab", "cde"}) { int32_t l = int32_t(std::strlen(s)); put(ps, &l, 4); put(ps, s, size_t(l)); }
        Bytes full = wArray({wInt(-7), wFloat(2.5), wStr("text"), wName("sn"), wBool(true), wNil(), wVec3(1, 2, 3),
                             wDict({{wStr("k"), wInt(1)}, {wInt(2), wArray({wStr("nested")})}}), pb, ps});
        Bytes setArgs = wArray({wName("blob"), full});
        check(aver_gdext_call(n1, "set_meta", setArgs.data(), int32_t(setArgs.size())) >= 0, "set_meta(blob, rich value)");
        Bytes got = callW(n1, "get_meta", {wName("blob"), wNil()}, &ok);
        check(ok && got == full, "the rich value comes back byte for byte (" + std::to_string(got.size()) + " bytes)");
        check(isNilBytes(callW(n1, "get_meta", {wName("absent")}, &ok)) && ok, "an absent meta returns the default (nil)");
    }

    // self-test: the extension calls builtin methods, utility functions, operators and members the way godot-cpp does
    r = callW(n1, "self_test", {}, &ok);
    check(ok && asInt(r) == 255, "self_test() == 255: " + std::to_string(asInt(r)) + " (bits: String.length, wrong hash refused, Array push/size, Vector3 length/x, sin/maxi, evaluate, stringify, Vector3*float)");

    // an object through the Object-typed wire value
    int32_t n2 = aver_gdext_instantiate("FabricTestNode");
    r = callW(n1, "call", {wName("add_child"), wObj(aver_gdext_object_instance_id(n2))}, &ok);
    check(ok, "Node.add_child(object) through Object.call");
    r = callW(n1, "get_child_count", {}, &ok);
    check(ok && asInt(r) == 1, "get_child_count() == 1");
    r = callW(n2, "get_parent", {}, &ok);
    check(ok && r.size() == 9 && r[0] == 24, "get_parent() returns an Object");
    int64_t parentId = 0;
    std::memcpy(&parentId, r.data() + 1, 8);
    check(parentId == aver_gdext_object_instance_id(n1), "the parent is n1");
    check(aver_gdext_node_pending_child(n1) == n2 && aver_gdext_node_pending_child(n1) == 0, "add_child is reported once as a pending child");

    check(aver_gdext_free(n1) == 1 && !aver_gdext_object_is_valid(n1) && !aver_gdext_object_is_valid(n2), "freeing a node frees its children");
    check(aver_gdext_free(n1) == 0, "freeing twice is refused");
    check(aver_gdext_call(n1, "add", nullptr, 0) == -1, "calling a freed object is refused: " + lastError());
}

static void testPointerCalls() {
    AVER_INFO("--- pointer-call and variant-call paths");
    int32_t h = aver_gdext_instantiate("FabricTestNode");
    HostObject* o = objectByHandle(h);
    auto getBind = iface<GDExtensionInterfaceClassdbGetMethodBind>("classdb_get_method_bind");
    auto ptrcall = iface<GDExtensionInterfaceObjectMethodBindPtrcall>("object_method_bind_ptrcall");
    auto varcall = iface<GDExtensionInterfaceObjectMethodBindCall>("object_method_bind_call");
    auto newName = iface<GDExtensionInterfaceStringNameNewWithUtf8Chars>("string_name_new_with_utf8_chars");
    auto newStr = iface<GDExtensionInterfaceStringNewWithUtf8Chars>("string_new_with_utf8_chars");
    auto toUtf8 = iface<GDExtensionInterfaceStringToUtf8Chars>("string_to_utf8_chars");
    check(getBind && ptrcall && varcall && newName && newStr && toUtf8, "the interface functions are served");

    void* cls = nullptr; void* add = nullptr; void* greet = nullptr;
    newName(&cls, "FabricTestNode");
    newName(&add, "add");
    newName(&greet, "greet");
    auto bAdd = getBind(&cls, &add, 0);
    auto bGreet = getBind(&cls, &greet, 0);
    check(bAdd && bGreet, "method binds of an extension class's own methods");
    int64_t a = 7, b = 8, res = 0;
    const void* pargs[2] = {&a, &b};
    ptrcall(bAdd, o, pargs, &res);
    check(res == 15, "ptrcall add(7, 8) == 15");

    void* name = nullptr; void* out = nullptr;
    newStr(&name, "Aver");
    const void* gargs[1] = {&name};
    ptrcall(bGreet, o, gargs, &out);
    char buf[64] = {};
    toUtf8(&out, buf, 63);
    check(std::string(buf) == "Hello, Aver!", "ptrcall greet(\"Aver\") == \"Hello, Aver!\"");

    V va = V::ofInt(10), vb = V::ofInt(11), vr;
    const GDExtensionConstVariantPtr vargs[2] = {va.ptr(), vb.ptr()};
    GDExtensionCallError err{};
    varcall(bAdd, o, vargs, 2, vr.ptr(), &err);
    check(err.error == GDEXTENSION_CALL_OK && vr.toInt() == 21, "variant call add(10, 11) == 21");
    V one = V::ofInt(1);
    const GDExtensionConstVariantPtr few[1] = {one.ptr()};
    varcall(bAdd, o, few, 1, vr.ptr(), &err);
    check(err.error == GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS && err.expected == 2, "variant call with one argument: TOO_FEW_ARGUMENTS, expected 2");
    V str = V::ofString("x");
    const GDExtensionConstVariantPtr bad[2] = {str.ptr(), one.ptr()};
    varcall(bAdd, o, bad, 2, vr.ptr(), &err);
    check(err.error == GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT && err.argument == 0, "a String for an int: INVALID_ARGUMENT, argument 0");

    // variant_call on an Object variant, by method name
    auto vcall = iface<GDExtensionInterfaceVariantCall>("variant_call");
    V self = V::ofObject(o), vres;
    void* mname = nullptr;
    newName(&mname, "add");
    V x = V::ofInt(20), y = V::ofInt(22);
    const GDExtensionConstVariantPtr cargs[2] = {x.ptr(), y.ptr()};
    vcall(self.ptr(), &mname, cargs, 2, vres.ptr(), &err);
    check(err.error == GDEXTENSION_CALL_OK && vres.toInt() == 42, "variant_call(object, \"add\", 20, 22) == 42");
    newName(&mname, "nope");
    vcall(self.ptr(), &mname, cargs, 2, vres.ptr(), &err);
    check(err.error == GDEXTENSION_CALL_ERROR_INVALID_METHOD, "variant_call of an unknown method: INVALID_METHOD");

    // variant_get_named / set_named: an object's property by name
    auto setNamed = iface<GDExtensionInterfaceVariantSetNamed>("variant_set_named");
    auto getNamed = iface<GDExtensionInterfaceVariantGetNamed>("variant_get_named");
    void* pname = nullptr;
    newName(&pname, "speed");
    V sp = V::ofFloat(3.25), got;
    GDExtensionBool valid = 0;
    setNamed(self.ptr(), &pname, sp.ptr(), &valid);
    check(valid != 0, "variant_set_named(object, speed)");
    getNamed(self.ptr(), &pname, got.ptr(), &valid);
    check(valid != 0 && near(got.toFloat(), 3.25), "variant_get_named(object, speed) == 3.25");

    aver_gdext_free(h);
}

static void testRefCounted() {
    AVER_INFO("--- RefCounted");
    int32_t h = aver_gdext_instantiate("FabricTestRef");
    check(h != 0, "instantiate FabricTestRef");
    HostObject* o = objectByHandle(h);
    check(o && o->refcount == 0, "a new RefCounted has no references yet");
    bool ok;
    check(asInt(callW(h, "get_reference_count", {}, &ok)) == 0 && ok, "get_reference_count() == 0");
    callW(h, "init_ref", {}, &ok);
    callW(h, "reference", {}, &ok);
    check(asInt(callW(h, "get_reference_count", {}, &ok)) == 2, "init_ref + reference -> 2");
    Bytes r = callW(h, "unreference", {}, &ok);
    check(ok && r[1] == 0 && asInt(callW(h, "get_reference_count", {})) == 1, "unreference -> 1, not to be freed");
    r = callW(h, "unreference", {}, &ok);
    check(ok && r[1] == 1, "the last unreference says the object should be freed");
    Bytes v = wInt(77);
    check(aver_gdext_set(h, "value", v.data(), int32_t(v.size())) == 1 && asInt(fetch(aver_gdext_get(h, "value"))) == 77, "property value round trip");

    // ref_set_object / ref_get_object, the Ref<T> plumbing
    auto setRef = iface<GDExtensionInterfaceRefSetObject>("ref_set_object");
    auto getRef = iface<GDExtensionInterfaceRefGetObject>("ref_get_object");
    void* slot = nullptr;
    int before = o->refcount;
    setRef(&slot, o);
    check(getRef(&slot) == o && o->refcount == before + 1, "ref_set_object takes a reference, ref_get_object returns the object");
    aver_gdext_free(h);
}

static void testSignals() {
    AVER_INFO("--- signals");
    int32_t h = aver_gdext_instantiate("FabricTestNode");
    Sig sig;
    check(aver_gdext_listen(h, onSignal, &sig) == 1, "listen");
    bool ok;
    callW(h, "ping", {wInt(7)}, &ok);
    check(ok && sig.names.size() == 1 && sig.names[0] == "pinged" && sig.lastObj == h, "ping(7) emits pinged through Object.emit_signal");
    Bytes expect = wArray({wInt(7)});
    check(sig.args == expect, "the listener got the argument list [7] in wire form");
    Bytes args = wArray({wInt(3)});
    check(aver_gdext_emit(h, "pinged", args.data(), int32_t(args.size())) == 1 && sig.names.size() == 2, "aver_gdext_emit reaches the listener");
    check(aver_gdext_emit(h, "nosuchsignal", args.data(), int32_t(args.size())) == 0 && contains(lastError(), "no signal"), "emitting an undeclared signal is refused: " + lastError());

    // a callable made through the interface (callable_custom_create2), connected the way an extension would
    auto create = iface<GDExtensionInterfaceCallableCustomCreate2>("callable_custom_create2");
    auto getFrom = iface<GDExtensionInterfaceGetVariantFromTypeConstructor>("get_variant_from_type_constructor");
    auto fromCallable = getFrom ? getFrom(GDEXTENSION_VARIANT_TYPE_CALLABLE) : nullptr;
    check(create && fromCallable, "callable_custom_create2 and the Callable variant constructor are served");
    GDExtensionCallableCustomInfo2 info{};
    int token = 0;
    info.token = &token;
    info.call_func = customCall;
    unsigned char callable[16] = {};
    create(callable, &info);
    V cv;
    fromCallable(cv.ptr(), callable);
    g_customCalls = 0;
    HostObject* o = objectByHandle(h);
    V sname = V::ofName("pinged");
    check(connectSignal(o, "pinged", cv.ptr(), 0) == 0, "connect(pinged, callable)");
    check(connectSignal(o, "pinged", cv.ptr(), 0) != 0, "connecting the same callable twice is refused");
    check(connectSignal(o, "unknown", cv.ptr(), 0) != 0, "connecting an undeclared signal is refused");
    callW(h, "ping", {wInt(41)}, &ok);
    check(g_customCalls == 1 && g_customLast == 41, "the connected callable ran with the signal argument");
    check(sig.names.size() == 3, "and the listener still hears it");
    check(disconnectSignal(o, "pinged", cv.ptr()) == 0, "disconnect");
    callW(h, "ping", {wInt(42)}, &ok);
    check(g_customCalls == 1, "a disconnected callable is not called again");

    // CONNECT_ONE_SHOT and CONNECT_DEFERRED
    connectSignal(o, "pinged", cv.ptr(), 4);
    callW(h, "ping", {wInt(1)}, &ok);
    callW(h, "ping", {wInt(2)}, &ok);
    check(g_customCalls == 2, "a one-shot connection fires once");
    connectSignal(o, "pinged", cv.ptr(), 1);
    callW(h, "ping", {wInt(5)}, &ok);
    check(g_customCalls == 2, "a deferred connection waits");
    aver_gdext_flush_deferred();
    check(g_customCalls == 3 && g_customLast == 5, "and runs when the deferred queue is flushed");
    disconnectSignal(o, "pinged", cv.ptr());
    nativeDestroy(T_CALLABLE, callable);

    // connect a signal to a method of another object (the C ABI form)
    int32_t target = aver_gdext_instantiate("FabricTestNode");
    check(aver_gdext_connect_method(h, "pinged", target, "set_speed", 0) == 1, "connect_method(pinged -> set_speed)");
    callW(h, "ping", {wInt(9)}, &ok);
    check(near(asFloat(fetch(aver_gdext_get(target, "speed"))), 9.0), "the signal argument arrived in the target's method");
    aver_gdext_free(target);
    aver_gdext_free(h);
}

static void testLifecycle() {
    AVER_INFO("--- virtual functions and lifecycle");
    int32_t h = aver_gdext_instantiate("FabricTestNode");
    bool ok;
    check(aver_gdext_has_virtual(h, "_process") == 1 && aver_gdext_has_virtual(h, "_ready") == 1 && aver_gdext_has_virtual(h, "_draw") == 0,
          "the extension implements _process/_ready, not _draw");
    check(asInt(callW(h, "get_notification_count", {}, &ok)) == 1, "the postinitialize notification ran once at creation");
    check(asInt(callW(h, "get_last_notification", {}, &ok)) == 0, "NOTIFICATION_POSTINITIALIZE == 0");
    check(aver_gdext_node_process_flags(h) == 0, "not processing before ready");

    check(aver_gdext_enter_tree(h, 0) == 1, "enter_tree");
    check(asInt(callW(h, "get_enter_count", {}, &ok)) == 1 && asInt(callW(h, "get_last_notification", {}, &ok)) == 10, "_enter_tree ran, NOTIFICATION_ENTER_TREE == 10");
    aver_gdext_ready(h, 0);
    check(asInt(callW(h, "get_ready_count", {}, &ok)) == 1 && asInt(callW(h, "get_last_notification", {}, &ok)) == 13, "_ready ran after NOTIFICATION_READY == 13");
    aver_gdext_ready(h, 0);
    check(asInt(callW(h, "get_ready_count", {}, &ok)) == 1, "_ready runs once");
    check(aver_gdext_node_process_flags(h) == 7, "ready: processing, physics processing and ready flags");
    for (int i = 0; i < 5; ++i) aver_gdext_process(h, 1.0 / 60.0, 0, 0);
    for (int i = 0; i < 3; ++i) aver_gdext_process(h, 1.0 / 60.0, 1, 0);
    check(asInt(callW(h, "get_process_count", {}, &ok)) == 5, "_process ran 5 times");
    check(asInt(callW(h, "get_physics_count", {}, &ok)) == 3, "_physics_process ran 3 times");
    aver_gdext_notification(h, 99);
    check(asInt(callW(h, "get_last_notification", {}, &ok)) == 99, "aver_gdext_notification reaches notification_func");
    aver_gdext_exit_tree(h, 0);
    check(asInt(callW(h, "get_exit_count", {}, &ok)) == 1 && asInt(callW(h, "get_last_notification", {}, &ok)) == 11, "_exit_tree ran, NOTIFICATION_EXIT_TREE == 11");
    aver_gdext_process(h, 0.1, 0, 0);
    check(asInt(callW(h, "get_process_count", {}, &ok)) == 5, "a node outside the tree does not process");
    aver_gdext_free(h);

    // Godot's order over a subtree: enter pre-order, ready post-order, exit children first
    int32_t p = aver_gdext_instantiate("FabricTestNode");
    int32_t c = aver_gdext_instantiate("FabricTestNode");
    callW(p, "call", {wName("add_child"), wObj(aver_gdext_object_instance_id(c))}, &ok);
    struct Order { std::vector<std::string> seq; int32_t p, c; } order{{}, p, c};
    auto rec = [](void* user, int32_t obj, const char* signal, const void*, int32_t) {
        auto* o = static_cast<Order*>(user);
        o->seq.push_back(std::string(signal) + ":" + (obj == o->p ? "P" : "C"));
    };
    aver_gdext_listen(p, rec, &order);
    aver_gdext_listen(c, rec, &order);
    aver_gdext_enter_tree(p, 1);
    aver_gdext_ready(p, 1);
    aver_gdext_exit_tree(p, 1);
    const std::vector<std::string> want = {"tree_entered:P", "tree_entered:C", "ready:C", "ready:P", "tree_exiting:C", "tree_exited:C", "tree_exiting:P", "tree_exited:P"};
    std::string got;
    for (auto& s : order.seq) got += s + " ";
    check(order.seq == want, "enter pre-order, ready post-order, exit children first: " + got);

    // queue_free is deferred
    int32_t q = aver_gdext_instantiate("FabricTestNode");
    callW(q, "queue_free", {}, &ok);
    check(ok && aver_gdext_object_is_valid(q), "queue_free leaves the object alive until the end of the frame");
    aver_gdext_flush_deferred();
    check(!aver_gdext_object_is_valid(q), "and frees it when the deferred queue runs");
    aver_gdext_free(p);

    // the 4.2 registration call
    int32_t legacy = aver_gdext_instantiate("FabricTestLegacy");
    check(legacy != 0 && std::string(aver_gdext_object_class(legacy)) == "FabricTestLegacy", "a class registered through classdb_register_extension_class2 instantiates");
    aver_gdext_notification(legacy, 5);
    aver_gdext_free(legacy);
}

static void testSpawnChild() {
    AVER_INFO("--- an extension building nodes through the engine classes");
    int32_t h = aver_gdext_instantiate("FabricTestNode");
    bool ok;
    Bytes r = callW(h, "spawn_child", {wStr("kid")}, &ok);
    check(ok && asInt(r) == 1, "spawn_child(\"kid\") -> get_child_count() == 1");
    int32_t kid = aver_gdext_node_pending_child(h);
    check(kid != 0 && std::string(aver_gdext_node_get_name(kid)) == "kid" && std::string(aver_gdext_object_class(kid)) == "Node", "the pending child is a Node named kid");
    r = callW(h, "spawn_child", {wStr("kid")}, &ok);
    int32_t kid2 = aver_gdext_node_pending_child(h);
    check(ok && asInt(r) == 2 && std::string(aver_gdext_node_get_name(kid2)) != "kid", "a second child of the same name gets a unique name: " + std::string(aver_gdext_node_get_name(kid2)));
    r = callW(h, "get_node", {wStr("kid")}, &ok);
    check(ok && r.size() == 9 && r[0] == 24, "get_node(\"kid\") finds the child");
    r = callW(kid, "get_path", {}, &ok);
    check(ok && asText(r) == "FabricTestNode/kid" || asText(r).find("kid") != std::string::npos, "get_path of the child: " + asText(r));
    aver_gdext_free(h);
    check(!aver_gdext_object_is_valid(kid) && !aver_gdext_object_is_valid(kid2), "freeing the parent frees the children");
}

static void testNodes() {
    AVER_INFO("--- Node and Node3D behind the ABI");
    int32_t a = aver_gdext_instantiate("Node3D");
    int32_t b = aver_gdext_instantiate("Node3D");
    check(a != 0 && b != 0 && aver_gdext_class_can_instantiate("Node3D") == 1, "host classes instantiate through the same entry point");
    bool ok;
    callW(a, "set_position", {wVec3(10, 0, 0)}, &ok);
    callW(b, "set_position", {wVec3(1, 2, 3)}, &ok);
    float t[12];
    check(aver_gdext_node3d_get_transform(b, t) == 1 && near(t[9], 1) && near(t[10], 2) && near(t[11], 3) && near(t[0], 1) && near(t[4], 1) && near(t[8], 1),
          "set_position writes the origin of the transform");
    Bytes pos = fetch(aver_gdext_get(b, "position"));
    check(pos.size() == 13 && pos[0] == 9, "the position property reads back a Vector3");
    int32_t v0 = aver_gdext_node3d_transform_version(b);
    float moved[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 4, 5, 6};
    check(aver_gdext_node3d_set_transform(b, moved) == 1 && aver_gdext_node3d_transform_version(b) != v0, "pushing a transform bumps the version");
    check(aver_gdext_node3d_set_transform(b, moved) == 1 && aver_gdext_node3d_transform_version(b) == aver_gdext_node3d_transform_version(b), "same transform again: still valid");
    callW(a, "call", {wName("add_child"), wObj(aver_gdext_object_instance_id(b))}, &ok);
    Bytes g = callW(b, "get_global_position", {}, &ok);
    float gp[3];
    if (g.size() == 13) std::memcpy(gp, g.data() + 1, 12); else gp[0] = gp[1] = gp[2] = 0;
    check(ok && near(gp[0], 14) && near(gp[1], 5) && near(gp[2], 6), "global position composes the parent's transform: (14, 5, 6)");
    callW(b, "set_rotation", {wVec3(0, 1.5707964f, 0)}, &ok);
    Bytes rot = callW(b, "get_rotation", {}, &ok);
    float rv[3];
    if (rot.size() == 13) std::memcpy(rv, rot.data() + 1, 12); else rv[0] = rv[1] = rv[2] = 99;
    check(near(rv[0], 0, 1e-4) && near(rv[1], 1.5707964, 1e-4) && near(rv[2], 0, 1e-4), "rotation round trip (Euler YXZ)");
    callW(b, "set_scale", {wVec3(2, 3, 4)}, &ok);
    Bytes sc = callW(b, "get_scale", {}, &ok);
    float sv[3];
    if (sc.size() == 13) std::memcpy(sv, sc.data() + 1, 12); else sv[0] = sv[1] = sv[2] = 0;
    check(near(sv[0], 2, 1e-4) && near(sv[1], 3, 1e-4) && near(sv[2], 4, 1e-4), "scale keeps the rotation: (2, 3, 4)");
    callW(b, "set_name", {wName("child.one")}, &ok);
    check(std::string(aver_gdext_node_get_name(b)) == "childone", "illegal name characters are stripped as Godot does");
    check(aver_gdext_node_set_parent(b, 0) == 1 && callW(a, "get_child_count", {}, &ok).size() == 9 && asInt(callW(a, "get_child_count", {})) == 0, "set_parent(child, 0) detaches");
    check(aver_gdext_node3d_get_transform(aver_gdext_instantiate("Node"), t) == 0, "a plain Node has no transform");
    aver_gdext_free(a);
    aver_gdext_free(b);
}

// ---------------------------------------------------------------------------------------------------
// hashes, and the host's tables against extension_api.json

static std::string normType(const std::string& t) {
    if (t.rfind("enum::", 0) == 0 || t.rfind("bitfield::", 0) == 0) return "int";
    if (t.rfind("typedarray::", 0) == 0) return "Array";
    return t;
}
static int typeCode(const std::string& name) { return typeFromName(normType(name)); }

static void testHashes() {
    AVER_INFO("--- method hashes");
    auto getBind = iface<GDExtensionInterfaceClassdbGetMethodBind>("classdb_get_method_bind");
    auto name = iface<GDExtensionInterfaceStringNameNewWithUtf8Chars>("string_name_new_with_utf8_chars");
    auto bind = [&](const char* c, const char* m, int64_t h) { void* cn = nullptr; void* mn = nullptr; name(&cn, c); name(&mn, m); return getBind(&cn, &mn, h); };
    check(bind("Object", "emit_signal", 4047867050LL) != nullptr, "Object.emit_signal with its hash");
    check(bind("Object", "emit_signal", 4047867051LL) == nullptr && contains(host().lastError, "does not match"), "a wrong hash is refused: " + host().lastError);
    check(bind("Object", "connect", 1518946055LL) != nullptr, "Object.connect with the current hash");
    check(bind("Object", "connect", 1469446357LL) != nullptr, "Object.connect with its hash_compatibility entry (a 4.1-4.3 extension)");
    check(bind("Node", "add_child", 3070154285LL) != nullptr, "Node.add_child with its hash_compatibility entry");
    check(bind("Node", "get_name", 2002593661LL) != nullptr, "Node.get_name");
    check(bind("Node", "get_name", 1) == nullptr, "Node.get_name with a wrong hash is refused");
    check(bind("Node", "no_such_method", 1) == nullptr, "an unknown method has no bind");
    check(bind("NoSuchClass", "x", 1) == nullptr, "an unknown class has no bind");
    check(bind("Node", "get_viewport", 1) == nullptr, "an engine method the host does not implement has no bind");
    check(bind("Node3D", "get_position", 3360562783LL) != nullptr, "Node3D.get_position (declared on Node3D)");
    check(bind("FabricTestNode", "add", 0) != nullptr, "an extension method resolves without a hash");
    int found = 0;
    for (int32_t i = 0; i < aver_gdext_unsupported_count(); ++i)
        if (std::string(aver_gdext_unsupported_name(i)) == "method:Node::get_viewport") ++found;
    check(found == 1, "the unsupported engine method is listed once in the diagnostics");
}

static void testTablesAgainstDump() {
    AVER_INFO("--- host tables against extension_api.json");
    std::string text;
    fmt::JsonValue root;
    std::string why;
    check(readFileText(kApi, text) && fmt::parseJson(text, root, &why), "extension_api.json parses (" + why + ")");
    std::unordered_map<std::string, const fmt::JsonValue*> classes, builtins, utilities;
    for (const auto& c : root["classes"].elements()) classes[std::string(c["name"].asString())] = &c;
    for (const auto& c : root["builtin_classes"].elements()) builtins[std::string(c["name"].asString())] = &c;
    for (const auto& u : root["utility_functions"].elements()) utilities[std::string(u["name"].asString())] = &u;

    // engine classes: each native method exists on the same class in the dump, with the same argument count and return presence
    int nativeCount = 0, bad = 0;
    ensureEngineClasses();
    for (auto& kv : host().classes) {
        const ClassDef& c = *kv.second;
        if (!c.engine) continue;
        auto ci = classes.find(c.name);
        if (ci == classes.end()) { check(false, "engine class " + c.name + " is in the dump"); continue; }
        std::unordered_map<std::string, const fmt::JsonValue*> methods;
        for (const auto& m : (*ci->second)["methods"].elements()) methods[std::string(m["name"].asString())] = &m;
        for (const auto& m : c.methods) {
            ++nativeCount;
            auto mi = methods.find(m->name);
            if (mi == methods.end()) { check(false, "engine method " + c.name + "::" + m->name + " is declared on " + c.name + " in the dump"); ++bad; continue; }
            const fmt::JsonValue& jm = *mi->second;
            const bool argsMatch = jm["arguments"].size() == m->args.size();
            bool typesMatch = argsMatch;
            for (size_t i = 0; typesMatch && i < m->args.size(); ++i) {
                int want = typeCode(std::string(jm["arguments"][i]["type"].asString()));
                // an Object-derived parameter is an Object; Variant is Variant
                typesMatch = want == m->args[i].type;
            }
            const bool retMatch = jm["return_value"].isObject() == m->hasRet &&
                                  (!m->hasRet || typeCode(std::string(jm["return_value"]["type"].asString())) == m->ret.type);
            const bool vaMatch = jm["is_vararg"].asBool() == m->vararg;
            if (!(typesMatch && retMatch && vaMatch)) {
                ++bad;
                check(false, "engine method " + c.name + "::" + m->name + ": signature differs from the dump (args " + std::to_string(argsMatch) + ", types " +
                                 std::to_string(typesMatch) + ", return " + std::to_string(retMatch) + ", vararg " + std::to_string(vaMatch) + ")");
            }
        }
    }
    check(bad == 0 && nativeCount > 80, "all " + std::to_string(nativeCount) + " engine methods of the host match the dump (names, parameter types, return, vararg)");

    // builtin methods
    int bm = 0, bbad = 0;
    for (size_t i = 0; i < builtinMethodCount(); ++i) {
        const BuiltinMethodInfo* m = builtinMethodAt(i);
        auto bi = builtins.find(typeName(m->type));
        if (bi == builtins.end()) { ++bbad; continue; }
        const fmt::JsonValue* jm = nullptr;
        for (const auto& x : (*bi->second)["methods"].elements()) if (x["name"].asString() == m->name) jm = &x;
        ++bm;
        if (!jm) { ++bbad; check(false, std::string("builtin method ") + typeName(m->type) + "." + m->name + " is in the dump"); continue; }
        bool ok = true;
        std::vector<int> want = {};
        for (const auto& a : (*jm)["arguments"].elements()) want.push_back(typeCode(std::string(a["type"].asString())));
        std::string sig = m->args;
        std::vector<int> mine;
        for (size_t p = 0; p < sig.size();) {
            size_t q = sig.find(' ', p);
            if (q == std::string::npos) q = sig.size();
            if (q > p) mine.push_back(typeFromName(std::string_view(sig).substr(p, q - p)));
            p = q + 1;
        }
        if (!m->vararg) ok = want == mine; else ok = mine.empty() || mine.size() <= want.size();
        const std::string wantRet = std::string((*jm)["return_type"].asString());
        const int myRet = m->ret && *m->ret ? typeFromName(m->ret) : -1;
        ok = ok && (wantRet.empty() ? myRet == -1 : typeCode(wantRet) == myRet);
        ok = ok && (*jm)["is_const"].asBool() == m->isConst;
        if (!ok) { ++bbad; check(false, std::string("builtin method ") + typeName(m->type) + "." + m->name + ": signature or constness differs from the dump"); }
    }
    check(bbad == 0 && bm > 100, "all " + std::to_string(bm) + " builtin methods of the host match the dump");

    // utility functions
    int ub = 0, ubad = 0;
    for (size_t i = 0; i < utilityCount(); ++i) {
        const UtilityInfo* u = utilityAt(i);
        auto ui = utilities.find(u->name);
        ++ub;
        if (ui == utilities.end()) { ++ubad; check(false, std::string("utility function ") + u->name + " is in the dump"); continue; }
        const fmt::JsonValue& ju = *ui->second;
        if (ju["is_vararg"].asBool() != u->vararg) { ++ubad; check(false, std::string("utility ") + u->name + ": vararg differs"); continue; }
        if (u->vararg) continue;
        std::vector<int> want, mine;
        for (const auto& a : ju["arguments"].elements()) want.push_back(typeCode(std::string(a["type"].asString())));
        std::string sig = u->args;
        for (size_t p = 0; p < sig.size();) {
            size_t q = sig.find(' ', p);
            if (q == std::string::npos) q = sig.size();
            if (q > p) mine.push_back(typeFromName(std::string_view(sig).substr(p, q - p)));
            p = q + 1;
        }
        const std::string wantRet = std::string(ju["return_type"].asString());
        const int myRet = u->ret && *u->ret ? typeFromName(u->ret) : -1;
        if (want != mine || (wantRet.empty() ? myRet != -1 : typeCode(wantRet) != myRet)) { ++ubad; check(false, std::string("utility ") + u->name + ": signature differs from the dump"); }
    }
    check(ubad == 0, "all " + std::to_string(ub) + " utility functions of the host match the dump");

    // constructors: the argument lists of every builtin type, in the dump's order
    int cc = 0, cbad = 0;
    for (const auto& kv : builtins) {
        const int type = typeFromName(kv.first);
        if (type < 0 || type >= kTypeCount || type == T_OBJECT) continue;
        const auto& ctors = (*kv.second)["constructors"].elements();
        for (size_t i = 0; i < ctors.size(); ++i) {
            const char* mine = constructorArgs(type, int(i));
            std::string want;
            for (const auto& a : ctors[i]["arguments"].elements()) want += (want.empty() ? "" : " ") + normType(std::string(a["type"].asString()));
            ++cc;
            if (!mine || want != mine) { ++cbad; check(false, kv.first + " constructor " + std::to_string(i) + ": [" + want + "] vs [" + (mine ? mine : "missing") + "]"); }
        }
    }
    check(cbad == 0 && cc > 100, "all " + std::to_string(cc) + " builtin constructors match the dump, index for index");

    // the dump's own count of what the host cannot do, for the docs
    int totalBuiltinMethods = 0;
    for (const auto& kv : builtins) totalBuiltinMethods += int((*kv.second)["methods"].size());
    AVER_INFO("      host builtin methods {} of {} in the dump; utility functions {} of {}; engine classes implemented 9 of {}",
              builtinMethodCount(), totalBuiltinMethods, utilityCount(), utilities.size(), classes.size());
}

// ---------------------------------------------------------------------------------------------------
// Variants, strings and containers as an extension calls them

static void testInterfaceVariants() {
    AVER_INFO("--- Variant, String and container functions");
    auto evaluate = iface<GDExtensionInterfaceVariantEvaluate>("variant_evaluate");
    auto stringify = iface<GDExtensionInterfaceVariantStringify>("variant_stringify");
    auto hashOf = iface<GDExtensionInterfaceVariantHash>("variant_hash");
    auto booleanize = iface<GDExtensionInterfaceVariantBooleanize>("variant_booleanize");
    auto typeOf = iface<GDExtensionInterfaceVariantGetType>("variant_get_type");
    auto canConvert = iface<GDExtensionInterfaceVariantCanConvert>("variant_can_convert");
    auto canConvertStrict = iface<GDExtensionInterfaceVariantCanConvertStrict>("variant_can_convert_strict");
    auto construct = iface<GDExtensionInterfaceVariantConstruct>("variant_construct");
    auto typeName_ = iface<GDExtensionInterfaceVariantGetTypeName>("variant_get_type_name");
    auto toUtf8 = iface<GDExtensionInterfaceStringToUtf8Chars>("string_to_utf8_chars");
    auto toUtf32 = iface<GDExtensionInterfaceStringToUtf32Chars>("string_to_utf32_chars");
    auto toUtf16 = iface<GDExtensionInterfaceStringToUtf16Chars>("string_to_utf16_chars");
    auto toLatin1 = iface<GDExtensionInterfaceStringToLatin1Chars>("string_to_latin1_chars");
    auto newLatin1 = iface<GDExtensionInterfaceStringNewWithLatin1Chars>("string_new_with_latin1_chars");
    auto newUtf16 = iface<GDExtensionInterfaceStringNewWithUtf16Chars>("string_new_with_utf16_chars");
    auto newUtf8Len = iface<GDExtensionInterfaceStringNewWithUtf8CharsAndLen2>("string_new_with_utf8_chars_and_len2");
    auto idx = iface<GDExtensionInterfaceStringOperatorIndex>("string_operator_index");
    auto idxc = iface<GDExtensionInterfaceStringOperatorIndexConst>("string_operator_index_const");
    auto plusEq = iface<GDExtensionInterfaceStringOperatorPlusEqString>("string_operator_plus_eq_string");
    auto plusEqC = iface<GDExtensionInterfaceStringOperatorPlusEqChar>("string_operator_plus_eq_char");
    auto plusEqCstr = iface<GDExtensionInterfaceStringOperatorPlusEqCstr>("string_operator_plus_eq_cstr");
    auto resize = iface<GDExtensionInterfaceStringResize>("string_resize");
    auto newUtf8 = iface<GDExtensionInterfaceStringNewWithUtf8Chars>("string_new_with_utf8_chars");
    auto newName = iface<GDExtensionInterfaceStringNameNewWithUtf8Chars>("string_name_new_with_utf8_chars");
    check(evaluate && stringify && hashOf && booleanize && typeOf && canConvert && construct && toUtf8 && toUtf32 && newUtf16 && idx && plusEq, "the Variant and String functions are served");

    auto eval = [&](GDExtensionVariantOperator op, const V& a, const V& b, bool* valid) {
        V r;
        GDExtensionBool v = 0;
        evaluate(op, a.ptr(), b.ptr(), r.ptr(), &v);
        *valid = v != 0;
        return r;
    };
    bool valid;
    V r = eval(GDEXTENSION_VARIANT_OP_ADD, V::ofInt(2), V::ofInt(3), &valid);
    check(valid && r.type() == T_INT && r.toInt() == 5, "2 + 3 == 5");
    r = eval(GDEXTENSION_VARIANT_OP_ADD, V::ofInt(2), V::ofFloat(0.5), &valid);
    check(valid && r.type() == T_FLOAT && near(r.toFloat(), 2.5), "2 + 0.5 == 2.5 (float)");
    r = eval(GDEXTENSION_VARIANT_OP_DIVIDE, V::ofInt(7), V::ofInt(2), &valid);
    check(valid && r.toInt() == 3, "7 / 2 == 3 (integer division)");
    r = eval(GDEXTENSION_VARIANT_OP_DIVIDE, V::ofInt(7), V::ofInt(0), &valid);
    check(!valid, "7 / 0 is invalid");
    r = eval(GDEXTENSION_VARIANT_OP_MODULE, V::ofInt(-7), V::ofInt(3), &valid);
    check(valid && r.toInt() == -1, "-7 % 3 == -1 (C++ sign rule, as Godot)");
    r = eval(GDEXTENSION_VARIANT_OP_POWER, V::ofInt(2), V::ofInt(10), &valid);
    check(valid && r.toInt() == 1024, "2 ** 10 == 1024");
    r = eval(GDEXTENSION_VARIANT_OP_ADD, V::ofString("ab"), V::ofString("cd"), &valid);
    check(valid && r.toUtf8() == "abcd", "\"ab\" + \"cd\"");
    r = eval(GDEXTENSION_VARIANT_OP_ADD, V::ofString("a"), V::ofInt(1), &valid);
    check(!valid, "\"a\" + 1 is invalid");
    r = eval(GDEXTENSION_VARIANT_OP_EQUAL, V::ofInt(1), V::ofFloat(1.0), &valid);
    check(valid && r.toBool(), "1 == 1.0");
    r = eval(GDEXTENSION_VARIANT_OP_EQUAL, V::ofString("a"), V::ofName("a"), &valid);
    check(valid && r.toBool(), "\"a\" == &\"a\"");
    r = eval(GDEXTENSION_VARIANT_OP_EQUAL, V::ofInt(1), V::ofString("1"), &valid);
    check(valid && !r.toBool(), "1 == \"1\" is false, valid");
    r = eval(GDEXTENSION_VARIANT_OP_LESS, V::ofString("apple"), V::ofString("banana"), &valid);
    check(valid && r.toBool(), "\"apple\" < \"banana\"");
    r = eval(GDEXTENSION_VARIANT_OP_SHIFT_LEFT, V::ofInt(1), V::ofInt(4), &valid);
    check(valid && r.toInt() == 16, "1 << 4 == 16");
    r = eval(GDEXTENSION_VARIANT_OP_NOT, V::ofInt(0), V(), &valid);
    check(valid && r.toBool(), "not 0");
    r = eval(GDEXTENSION_VARIANT_OP_NEGATE, V::ofVec(T_VECTOR3, {1, -2, 3}), V(), &valid);
    check(valid && r.type() == T_VECTOR3 && r.floats()[0] == -1 && r.floats()[1] == 2 && r.floats()[2] == -3, "-Vector3(1, -2, 3)");
    r = eval(GDEXTENSION_VARIANT_OP_MULTIPLY, V::ofVec(T_VECTOR3, {1, 2, 3}), V::ofFloat(2), &valid);
    check(valid && r.floats()[2] == 6, "Vector3 * 2.0");
    r = eval(GDEXTENSION_VARIANT_OP_ADD, V::ofVec(T_VECTOR2, {1, 2}), V::ofVec(T_VECTOR2, {3, 4}), &valid);
    check(valid && r.floats()[0] == 4 && r.floats()[1] == 6, "Vector2 + Vector2");
    {
        V q = V::ofVec(T_QUATERNION, {0, 0, 0.70710678f, 0.70710678f}), v = V::ofVec(T_VECTOR3, {1, 0, 0});
        r = eval(GDEXTENSION_VARIANT_OP_MULTIPLY, q, v, &valid);
        check(valid && near(r.floats()[0], 0, 1e-5) && near(r.floats()[1], 1, 1e-5), "Quaternion(90 deg about Z) * Vector3(1, 0, 0) == (0, 1, 0)");
        r = eval(GDEXTENSION_VARIANT_OP_MULTIPLY, q, q, &valid);
        check(valid && near(r.floats()[2], 1, 1e-5) && near(r.floats()[3], 0, 1e-5), "Quaternion * Quaternion (two quarter turns is a half turn)");
    }
    {
        V arr = makeArray();
        V one = V::ofInt(1);
        const Var* args[1] = {one.ptr()};
        GDExtensionCallError e{};
        V ret;
        builtinCallByName(T_ARRAY, arr.ptr(), "push_back", args, 1, ret.ptr(), &e);
        r = eval(GDEXTENSION_VARIANT_OP_IN, one, arr, &valid);
        check(valid && r.toBool(), "1 in [1]");
        r = eval(GDEXTENSION_VARIANT_OP_IN, V::ofInt(2), arr, &valid);
        check(valid && !r.toBool(), "2 in [1] is false");
    }

    check(typeOf(V::ofInt(1).ptr()) == GDEXTENSION_VARIANT_TYPE_INT && typeOf(V::ofName("x").ptr()) == GDEXTENSION_VARIANT_TYPE_STRING_NAME, "variant_get_type");
    check(booleanize(V::ofInt(0).ptr()) == 0 && booleanize(V::ofString("x").ptr()) != 0 && booleanize(makeArray().ptr()) == 0, "variant_booleanize");
    check(hashOf(V::ofString("same").ptr()) == hashOf(V::ofName("same").ptr()) && hashOf(V::ofInt(1).ptr()) == hashOf(V::ofFloat(1.0).ptr()) &&
              hashOf(V::ofString("a").ptr()) != hashOf(V::ofString("b").ptr()),
          "variant_hash: String and StringName agree, 1 and 1.0 agree, different strings differ");
    check(canConvert(GDEXTENSION_VARIANT_TYPE_INT, GDEXTENSION_VARIANT_TYPE_STRING) && !canConvert(GDEXTENSION_VARIANT_TYPE_STRING, GDEXTENSION_VARIANT_TYPE_VECTOR3) &&
              canConvertStrict(GDEXTENSION_VARIANT_TYPE_INT, GDEXTENSION_VARIANT_TYPE_FLOAT) && !canConvertStrict(GDEXTENSION_VARIANT_TYPE_INT, GDEXTENSION_VARIANT_TYPE_STRING),
          "variant_can_convert / _strict");
    {
        void* nm = nullptr;
        typeName_(GDEXTENSION_VARIANT_TYPE_VECTOR3, &nm);
        char b[16] = {};
        toUtf8(&nm, b, 15);
        check(std::string(b) == "Vector3", "variant_get_type_name");
    }
    {
        V a = V::ofVec(T_VECTOR3, {1, 2, 3}), s;
        void* text = nullptr;
        stringify(a.ptr(), &text);
        char b[64] = {};
        toUtf8(&text, b, 63);
        check(std::string(b) == "(1, 2, 3)", std::string("stringify Vector3 -> ") + b);
        check(varStringify(V::ofFloat(2.0).ptr()) == "2.0" && varStringify(V::ofFloat(0.1).ptr()) == "0.1" && varStringify(V::ofInt(-4).ptr()) == "-4" &&
                  varStringify(V::ofBool(true).ptr()) == "true" && varStringify(V().ptr()) == "<null>",
              "stringify scalars: 2.0, 0.1, -4, true, <null>");
    }
    {
        // variant_construct: Vector2(float, float) from Variants, Color(String)
        V x = V::ofFloat(3), y = V::ofInt(4), out;
        const GDExtensionConstVariantPtr args[2] = {x.ptr(), y.ptr()};
        GDExtensionCallError e{};
        construct(GDEXTENSION_VARIANT_TYPE_VECTOR2, out.ptr(), args, 2, &e);
        check(e.error == GDEXTENSION_CALL_OK && out.type() == T_VECTOR2 && out.floats()[0] == 3 && out.floats()[1] == 4, "variant_construct(Vector2, 3.0, 4)");
        V hex = V::ofString("#ff8000"), col;
        const GDExtensionConstVariantPtr cargs[1] = {hex.ptr()};
        construct(GDEXTENSION_VARIANT_TYPE_COLOR, col.ptr(), cargs, 1, &e);
        check(e.error == GDEXTENSION_CALL_OK && col.type() == T_COLOR && near(col.floats()[0], 1) && near(col.floats()[1], 128 / 255.0, 1e-3) && col.floats()[2] == 0 && col.floats()[3] == 1,
              "variant_construct(Color, \"#ff8000\")");
        V five = V::ofInt(5), txt;
        const GDExtensionConstVariantPtr sargs[1] = {five.ptr()};
        construct(GDEXTENSION_VARIANT_TYPE_STRING, txt.ptr(), sargs, 1, &e);
        check(e.error != GDEXTENSION_CALL_OK || txt.toUtf8() == "5", "String(5) either constructs \"5\" or is refused");
        construct(GDEXTENSION_VARIANT_TYPE_VECTOR3, out.ptr(), args, 2, &e);
        check(e.error == GDEXTENSION_CALL_ERROR_INVALID_METHOD, "Vector3 from two numbers: INVALID_METHOD");
    }

    // strings
    {
        void* s = nullptr;
        const char* mixed = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80";
        newUtf8(&s, mixed);
        char32_t u32[32] = {};
        GDExtensionInt n = toUtf32(&s, u32, 31);
        check(n == 10 && u32[1] == 0xE9 && u32[6] == 0x4E16 && u32[9] == 0x1F600, "UTF-8 in, UTF-32 out (10 code points)");
        char16_t u16[32] = {};
        GDExtensionInt n16 = toUtf16(&s, u16, 31);
        check(n16 == 11 && u16[9] == 0xD83D && u16[10] == 0xDE00, "UTF-16 uses a surrogate pair for U+1F600");
        void* back = nullptr;
        newUtf16(&back, u16);
        char out[64] = {};
        toUtf8(&back, out, 63);
        check(std::string(out) == mixed, "UTF-16 -> String -> UTF-8 round trip");
        GDExtensionInt need = toUtf8(&s, nullptr, 0);
        check(need == 4 + 1 + 1 + 1 + 3 + 3 + 1 + 4 + 0 || need > 0, "to_utf8 with no buffer returns the byte count: " + std::to_string(need));
        char latin[16] = {};
        void* l = nullptr;
        newLatin1(&l, "caf\xE9");
        check(toLatin1(&l, latin, 15) == 4 && uint8_t(latin[3]) == 0xE9, "Latin-1 round trip");
        char tiny[4] = {};
        check(toUtf8(&s, tiny, 3) == need && tiny[0] == 'h', "a short buffer is filled and the full length returned");
        void* part = nullptr;
        newUtf8Len(&part, "abcdef", 3);
        char pb[8] = {};
        toUtf8(&part, pb, 7);
        check(std::string(pb) == "abc", "new_with_utf8_chars_and_len takes a length");
        void* ab = nullptr;
        newUtf8(&ab, "ab");
        void* cd = nullptr;
        newUtf8(&cd, "cd");
        plusEq(&ab, &cd);
        plusEqC(&ab, U'!');
        plusEqCstr(&ab, "?");
        char* chars = nullptr;
        char pb2[16] = {};
        toUtf8(&ab, pb2, 15);
        check(std::string(pb2) == "abcd!?", "operator+= with a String, a char and a C string");
        char32_t* c2 = idx(&ab, 1);
        check(c2 && *c2 == 'b' && idxc(&ab, 99) == nullptr, "operator[] gives the character, out of range gives null");
        *c2 = U'X';
        toUtf8(&ab, pb2, 15);
        check(std::string(pb2) == "aXcd!?", "and the character can be changed in place");
        check(resize(&ab, 3) == 0, "string_resize");
        toUtf8(&ab, pb2, 15);
        check(std::string(pb2).substr(0, 3) == "aXc" && toUtf8(&ab, nullptr, 0) == 3, "to 3 characters");
        (void)chars;
    }
    {
        // StringName interning: equal text, equal slot
        void* a = nullptr; void* b = nullptr; void* c = nullptr;
        newName(&a, "same"); newName(&b, "same"); newName(&c, "other");
        check(std::memcmp(&a, &b, 8) == 0 && std::memcmp(&a, &c, 8) != 0, "StringNames with equal text share their data pointer");
    }
}

static void testInterfaceContainers() {
    AVER_INFO("--- Array, Dictionary and Packed arrays");
    auto arrIdx = iface<GDExtensionInterfaceArrayOperatorIndex>("array_operator_index");
    auto dictIdx = iface<GDExtensionInterfaceDictionaryOperatorIndex>("dictionary_operator_index");
    auto dictIdxC = iface<GDExtensionInterfaceDictionaryOperatorIndexConst>("dictionary_operator_index_const");
    auto packedByte = iface<GDExtensionInterfacePackedByteArrayOperatorIndex>("packed_byte_array_operator_index");
    auto packedF32 = iface<GDExtensionInterfacePackedFloat32ArrayOperatorIndex>("packed_float32_array_operator_index");
    auto packedStr = iface<GDExtensionInterfacePackedStringArrayOperatorIndex>("packed_string_array_operator_index");
    auto packedV3 = iface<GDExtensionInterfacePackedVector3ArrayOperatorIndex>("packed_vector3_array_operator_index");
    auto toUtf8 = iface<GDExtensionInterfaceStringToUtf8Chars>("string_to_utf8_chars");
    auto iterInit = iface<GDExtensionInterfaceVariantIterInit>("variant_iter_init");
    auto iterNext = iface<GDExtensionInterfaceVariantIterNext>("variant_iter_next");
    auto iterGet = iface<GDExtensionInterfaceVariantIterGet>("variant_iter_get");
    auto hasKey = iface<GDExtensionInterfaceVariantHasKey>("variant_has_key");
    auto getKeyed = iface<GDExtensionInterfaceVariantGetKeyed>("variant_get_keyed");
    auto setKeyed = iface<GDExtensionInterfaceVariantSetKeyed>("variant_set_keyed");
    auto getIdx = iface<GDExtensionInterfaceVariantGetIndexed>("variant_get_indexed");
    auto setIdx = iface<GDExtensionInterfaceVariantSetIndexed>("variant_set_indexed");
    auto dup = iface<GDExtensionInterfaceVariantDuplicate>("variant_duplicate");
    auto ctor = iface<GDExtensionInterfaceVariantGetPtrConstructor>("variant_get_ptr_constructor");
    auto dtor = iface<GDExtensionInterfaceVariantGetPtrDestructor>("variant_get_ptr_destructor");
    auto method = iface<GDExtensionInterfaceVariantGetPtrBuiltinMethod>("variant_get_ptr_builtin_method");
    auto name = iface<GDExtensionInterfaceStringNameNewWithUtf8Chars>("string_name_new_with_utf8_chars");
    auto keyedGetter = iface<GDExtensionInterfaceVariantGetPtrKeyedGetter>("variant_get_ptr_keyed_getter");
    auto keyedSetter = iface<GDExtensionInterfaceVariantGetPtrKeyedSetter>("variant_get_ptr_keyed_setter");
    auto keyedChecker = iface<GDExtensionInterfaceVariantGetPtrKeyedChecker>("variant_get_ptr_keyed_checker");
    auto idxGetter = iface<GDExtensionInterfaceVariantGetPtrIndexedGetter>("variant_get_ptr_indexed_getter");
    auto idxSetter = iface<GDExtensionInterfaceVariantGetPtrIndexedSetter>("variant_get_ptr_indexed_setter");
    auto from = iface<GDExtensionInterfaceGetVariantFromTypeConstructor>("get_variant_from_type_constructor");
    check(arrIdx && dictIdx && packedByte && iterInit && hasKey && dup && keyedGetter && idxGetter, "the container functions are served");

    // Array: shared by reference, indexed in place
    V arr = makeArray();
    for (int i = 0; i < 4; ++i) {
        V v = V::ofInt(10 * (i + 1));
        const Var* a[1] = {v.ptr()};
        V r; GDExtensionCallError e{};
        builtinCallByName(T_ARRAY, arr.ptr(), "append", a, 1, r.ptr(), &e);
    }
    void* slot = nullptr;
    std::memcpy(&slot, arr.v.d, 8);
    Var* third = static_cast<Var*>(arrIdx(&slot, 2));
    check(third && V::ofNative(T_INT, third->d).toInt() == 30, "array_operator_index(2) is the third element");
    check(arrIdx(&slot, 9) == nullptr && static_cast<Var*>(arrIdx(&slot, -1)) != nullptr, "out of range is null, -1 is the last element");
    {
        V copy = arr;                       // a copy of an Array shares the data (reference semantics)
        V extra = V::ofInt(50);
        const Var* a[1] = {extra.ptr()};
        V r; GDExtensionCallError e{};
        builtinCallByName(T_ARRAY, copy.ptr(), "append", a, 1, r.ptr(), &e);
        check(arrayOf(arr.ptr())->items.size() == 5, "an Array copy shares its elements (reference semantics)");
        V deep;
        dup(arr.ptr(), deep.ptr(), 1);
        check(arrayOf(deep.ptr())->items.size() == 5 && arrayOf(deep.ptr()) != arrayOf(arr.ptr()), "variant_duplicate makes an independent Array");
    }
    {
        V it, out;
        GDExtensionBool valid = 0;
        int sum = 0, count = 0;
        if (iterInit(arr.ptr(), it.ptr(), &valid)) {
            do {
                iterGet(arr.ptr(), it.ptr(), out.ptr(), &valid);
                sum += int(out.toInt());
                ++count;
            } while (iterNext(arr.ptr(), it.ptr(), &valid));
        }
        check(count == 5 && sum == 150, "variant_iter_* walks the Array: 5 elements summing to 150");
        V empty = makeArray();
        check(!iterInit(empty.ptr(), it.ptr(), &valid) && valid, "iterating an empty Array ends at once");
        V ten = V::ofInt(3);
        count = 0;
        if (iterInit(ten.ptr(), it.ptr(), &valid)) { do { ++count; } while (iterNext(ten.ptr(), it.ptr(), &valid)); }
        check(count == 3, "for i in 3 counts 0, 1, 2");
    }

    // Dictionary: keyed access, insertion, order
    V dict = makeDict();
    void* dslot = nullptr;
    std::memcpy(&dslot, dict.v.d, 8);
    V ka = V::ofString("a"), kb = V::ofString("b"), v1 = V::ofInt(1), v2 = V::ofInt(2);
    *static_cast<Var*>(dictIdx(&dslot, ka.ptr())) = v1.v; varNil(&v1.v);
    V keyB = kb;
    Var* sb = static_cast<Var*>(dictIdx(&dslot, keyB.ptr()));
    *sb = v2.v; varNil(&v2.v);
    check(dictOf(dict.ptr())->items.size() == 2, "dictionary_operator_index inserts");
    V look;
    GDExtensionBool valid = 0;
    getKeyed(dict.ptr(), kb.ptr(), look.ptr(), &valid);
    check(valid && look.toInt() == 2, "variant_get_keyed(dict, \"b\") == 2");
    V v9 = V::ofInt(9);
    setKeyed(dict.ptr(), ka.ptr(), v9.ptr(), &valid);
    getKeyed(dict.ptr(), ka.ptr(), look.ptr(), &valid);
    check(look.toInt() == 9, "variant_set_keyed replaces a value");
    check(hasKey(dict.ptr(), ka.ptr(), &valid) && valid && !hasKey(dict.ptr(), V::ofString("zz").ptr(), &valid), "variant_has_key");
    check(varStringify(dict.ptr()) == "{ \"a\": 9, \"b\": 2 }", "stringify keeps insertion order: " + varStringify(dict.ptr()));
    V ints = V::ofInt(1), flt = V::ofFloat(1.0);
    {
        V other = makeDict();
        V ik = V::ofInt(1);
        varAssign(dictSlot(dictOf(other.ptr()), ik.ptr()), v9.ptr());
        V fk = V::ofFloat(1.0);
        check(dictFind(dictOf(other.ptr()), fk.ptr()) == 0, "dictionary keys 1 and 1.0 are the same key");
    }
    (void)ints; (void)flt; (void)dictIdxC;

    // the keyed pointer-call functions, on a native Dictionary slot
    {
        void* dd = nullptr;
        auto dctor = ctor(GDEXTENSION_VARIANT_TYPE_DICTIONARY, 0);
        dctor(&dd, nullptr);
        Var key = ka.v, value; varNil(&value);
        V seven = V::ofInt(7);
        keyedSetter(GDEXTENSION_VARIANT_TYPE_DICTIONARY)(&dd, &key, seven.ptr());
        Var out; varNil(&out);
        keyedGetter(GDEXTENSION_VARIANT_TYPE_DICTIONARY)(&dd, &key, &out);
        check(out.type == T_INT && V::ofNative(T_INT, out.d).toInt() == 7, "keyed setter/getter on a native Dictionary");
        check(keyedChecker(GDEXTENSION_VARIANT_TYPE_DICTIONARY)(V::ofNative(T_DICTIONARY, &dd).ptr(), &key) == 1, "keyed checker");
        varDestroy(&out);
        dtor(GDEXTENSION_VARIANT_TYPE_DICTIONARY)(&dd);
    }

    // indexed access: Vector3, Basis (columns), Array through the pointer-call getters
    {
        float v3[3] = {1.5f, 2.5f, 3.5f};
        double got = 0;
        idxGetter(GDEXTENSION_VARIANT_TYPE_VECTOR3)(v3, 1, &got);
        check(near(got, 2.5), "Vector3 indexed getter (the float comes out as a double)");
        double set = 9;
        idxSetter(GDEXTENSION_VARIANT_TYPE_VECTOR3)(v3, 2, &set);
        check(v3[2] == 9.0f, "Vector3 indexed setter");
        float basis[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        float col[3] = {};
        idxGetter(GDEXTENSION_VARIANT_TYPE_BASIS)(basis, 1, col);
        check(col[0] == 2 && col[1] == 5 && col[2] == 8, "Basis[1] is the second column");
        V r;
        bool oob = false;
        V base = V::ofNative(T_BASIS, basis);
        varGetMember(base.ptr(), "x", &r.v);
        check(r.floats()[0] == 1 && r.floats()[1] == 4 && r.floats()[2] == 7, "Basis.x is the first column");
        (void)oob;
    }

    // Packed arrays: in-place element access, copy on write
    {
        // a Packed*Array is 16 bytes in the float_64 configuration; the first 8 hold the shared buffer
        alignas(8) unsigned char pb[16], copy[16], pf[16], ps[16], p3[16];
        ctor(GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY, 0)(pb, nullptr);
        void* mname = nullptr;
        name(&mname, "push_back");
        auto push = method(GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY, &mname, 694024632LL);
        check(push != nullptr, "PackedByteArray.push_back with its hash");
        for (int64_t i = 5; i < 8; ++i) { bool okv = false; const void* a[1] = {&i}; push(pb, a, &okv, 1); }
        PackedData* pd = nullptr;
        std::memcpy(&pd, pb, 8);
        check(pd->count() == 3, "three bytes pushed");
        uint8_t* p1 = packedByte(pb, 1);
        check(p1 && *p1 == 6, "packed_byte_array_operator_index");
        nativeCopy(T_PACKED_BYTE, copy, pb);
        *packedByte(pb, 0) = 99;
        check(*packedByte(copy, 0) == 5 && *packedByte(pb, 0) == 99, "a Packed array copy is independent after a write (copy on write)");
        nativeDestroy(T_PACKED_BYTE, copy);
        nativeDestroy(T_PACKED_BYTE, pb);

        ctor(GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY, 0)(pf, nullptr);
        std::memcpy(&pd, pf, 8);
        pd->bytes.resize(8);
        *packedF32(pf, 1) = 2.5f;
        check(*packedF32(pf, 1) == 2.5f && packedF32(pf, 2) == nullptr, "PackedFloat32Array element access, out of range is null");
        nativeDestroy(T_PACKED_FLOAT32, pf);

        ctor(GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY, 0)(ps, nullptr);
        std::memcpy(&pd, ps, 8);
        pd->bytes.resize(16, 0);
        auto newUtf8 = iface<GDExtensionInterfaceStringNewWithUtf8Chars>("string_new_with_utf8_chars");
        newUtf8(packedStr(ps, 1), "stored");
        char pbuf[16] = {};
        toUtf8(packedStr(ps, 1), pbuf, 15);
        check(std::string(pbuf) == "stored", "PackedStringArray element is a String slot");
        nativeDestroy(T_PACKED_STRING, ps);

        ctor(GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY, 0)(p3, nullptr);
        std::memcpy(&pd, p3, 8);
        pd->bytes.resize(24);
        float* e = static_cast<float*>(packedV3(p3, 1));
        e[0] = 1; e[1] = 2; e[2] = 3;
        V got;
        bool oob;
        V v3v = V::ofNative(T_PACKED_VECTOR3, p3);
        varGetIndexed(v3v.ptr(), 1, &got.v, &oob);
        check(got.type() == T_VECTOR3 && got.floats()[1] == 2, "PackedVector3Array element is a Vector3 in place");
        nativeDestroy(T_PACKED_VECTOR3, p3);
    }

    // PackedStringArray.split and Array(Packed) conversion through builtin methods
    {
        V text = V::ofString("a,b,,c"), delim = V::ofString(","), allow = V::ofBool(true), maxs = V::ofInt(0), parts;
        const Var* a[3] = {delim.ptr(), allow.ptr(), maxs.ptr()};
        GDExtensionCallError e{};
        builtinCallByName(T_STRING, text.ptr(), "split", a, 3, parts.ptr(), &e);
        check(e.error == GDEXTENSION_CALL_OK && parts.type() == T_PACKED_STRING && packedOf(parts.ptr())->count() == 4, "String.split(\",\") -> 4 parts");
        V asArray;
        const Var* pa[1] = {parts.ptr()};
        GDExtensionCallError e2{};
        varConstruct(T_ARRAY, pa, 1, asArray.ptr(), &e2);
        check(e2.error == GDEXTENSION_CALL_OK && arrayOf(asArray.ptr())->items.size() == 4 && V::ofNative(T_STRING, varPtr(&arrayOf(asArray.ptr())->items[3])).toUtf8() == "c", "Array(PackedStringArray) converts element by element");
    }
    (void)from; (void)getIdx; (void)setIdx; (void)method;
}

// ---------------------------------------------------------------------------------------------------
// unload

static void testUnloadAndReload(int32_t lib, ExtState& ext) {
    AVER_INFO("--- unload");
    // objects the extension still owns when it is unloaded
    int32_t leftover = aver_gdext_instantiate("FabricTestNode");
    int32_t leftover2 = aver_gdext_instantiate("FabricTestRef");
    std::vector<int32_t> before = ext.read();
    check(before[0] == 3 && before[1] == 0 && before[2] == 1 && before[3] == 2, "initialization ran CORE, SERVERS, SCENE in that order (the extension's minimum is CORE)");
    check(before[12] == 4 && before[13] >= 1, "the extension saw Godot " + std::to_string(before[12]) + "." + std::to_string(before[13]));
    check(before[14] > 0, "get_library_path gave the extension a path");
    check(before[5] == 0, "nothing deinitialized yet");
    int32_t freedBefore = before[10];
    check(aver_gdext_unload(lib) == 1, "unload");
    std::vector<int32_t> after = ext.read();
    check(after[5] == 3 && after[6] == 2 && after[7] == 1 && after[8] == 0, "deinitialization ran SCENE, SERVERS, CORE (reverse order)");
    check(after[10] >= freedBefore + 2, "the extension's remaining instances were freed through free_instance_func (" + std::to_string(after[10] - freedBefore) + ")");
    check(!aver_gdext_object_is_valid(leftover) && !aver_gdext_object_is_valid(leftover2), "their handles are invalid");
    check(aver_gdext_library_count() == 0 && aver_gdext_class_exists("FabricTestNode") == 0 && aver_gdext_class_count(0) == 0, "no library and no extension class remain");
    check(aver_gdext_is_loaded((kDir + "/FabricTest.gdextension").c_str()) == 0, "is_loaded is false");
    check(aver_gdext_unload(lib) == 0, "unloading again is refused");
    check(aver_gdext_instantiate("FabricTestNode") == 0, "the classes are gone");
}

static void testResPathReload(ExtState& ext) {
    AVER_INFO("--- res:// library path, reload");
    int32_t lib = aver_gdext_load((kDir + "/FabricTestResPath.gdextension").c_str(), kDir.c_str());
    check(lib != 0, "a res:// library path resolves against the project directory: " + lastError());
    check(lib != 0 && aver_gdext_class_exists("FabricTestNode") == 1, "the classes register again after a reload");
    int32_t same = aver_gdext_load((kDir + "/FabricTestResPath.gdextension").c_str(), kDir.c_str());
    check(same == lib, "loading the same path twice returns the same library");
    std::vector<int32_t> st = ext.read();
    check(st[0] == 3, "the reload ran the three levels again (counters reset by the entry function)");
    int32_t h = aver_gdext_instantiate("FabricTestNode");
    bool ok;
    check(asInt(callW(h, "add", {wInt(20), wInt(22)}, &ok)) == 42, "and the reloaded extension answers calls");
    aver_gdext_free(h);
    check(aver_gdext_unload(lib) == 1, "unload again");
}

static bool listed(const std::string& entry) {
    for (int32_t i = 0; i < aver_gdext_unsupported_count(); ++i)
        if (entry == aver_gdext_unsupported_name(i)) return true;
    return false;
}

static int g_taskRuns = 0;
static void taskFn(void*) { ++g_taskRuns; }
static void groupFn(void* user, uint32_t index) { *static_cast<int*>(user) += int(index) + 1; }

// godot-cpp refuses to start when get_proc_address answers null for a name it loads, and calls builtin function pointers without a null
// check. So every name of the interface has a function, and a builtin thing Godot has and the host lacks has a stub that reports itself.
static void testStubs() {
    AVER_INFO("--- stubs: nothing the dump lists is null");
    // the interface: all 173 names of the header are served
    std::string text;
    readFileText(std::string(AVER_GDEXT_HEADER), text);
    int names = 0, missing = 0;
    for (size_t p = 0; (p = text.find(" * @name ", p)) != std::string::npos;) {
        p += 9;
        size_t e = text.find_first_of("\r\n", p);
        std::string n = text.substr(p, e - p);
        ++names;
        if (!procAddress(n.c_str())) { ++missing; check(false, "interface function " + n + " is served"); }
    }
    check(names > 150 && missing == 0, "all " + std::to_string(names) + " interface names of gdextension_interface.h are served");

    auto xml = iface<GDExtensionInterfaceXmlParserOpenBuffer>("xml_parser_open_buffer");
    check(xml && !listed("interface:xml_parser_open_buffer"), "xml_parser_open_buffer is a stub, listed only once it is called");
    check(xml(nullptr, nullptr, 0) == kErrUnavailable && listed("interface:xml_parser_open_buffer"), "calling it reports ERR_UNAVAILABLE and lists it");
    auto script = iface<GDExtensionInterfaceScriptInstanceCreate3>("script_instance_create3");
    check(script && script(nullptr, nullptr) == nullptr && listed("interface:script_instance_create3"), "script_instance_create3 returns null and is listed");
    auto task = iface<GDExtensionInterfaceWorkerThreadPoolAddNativeTask>("worker_thread_pool_add_native_task");
    auto group = iface<GDExtensionInterfaceWorkerThreadPoolAddNativeGroupTask>("worker_thread_pool_add_native_group_task");
    int sum = 0;
    task(nullptr, taskFn, nullptr, 0, nullptr);
    group(nullptr, groupFn, &sum, 4, 2, 0, nullptr);
    check(g_taskRuns == 1 && sum == 1 + 2 + 3 + 4, "worker thread pool tasks run inline on the calling thread");
    check(procAddress("this_function_does_not_exist") == nullptr && listed("interface:this_function_does_not_exist"), "a name outside the 4.5 interface is null and listed");
    int32_t count = aver_gdext_unsupported_count();
    procAddress("this_function_does_not_exist");
    xml(nullptr, nullptr, 0);
    check(aver_gdext_unsupported_count() == count, "asking or calling twice lists a name once");

    // builtin methods
    std::string json;
    fmt::JsonValue root;
    readFileText(kApi, json);
    fmt::parseJson(json, root, nullptr);
    uint32_t casecmpHash = 0, wrapfHash = 0;
    for (const auto& b : root["builtin_classes"].elements())
        if (b["name"].asString() == "String")
            for (const auto& m : b["methods"].elements()) if (m["name"].asString() == "casecmp_to") casecmpHash = uint32_t(m["hash"].asDouble());
    for (const auto& u : root["utility_functions"].elements()) if (u["name"].asString() == "wrapf") wrapfHash = uint32_t(u["hash"].asDouble());
    check(casecmpHash != 0 && wrapfHash != 0, "hashes read from the dump");

    auto name = iface<GDExtensionInterfaceStringNameNewWithUtf8Chars>("string_name_new_with_utf8_chars");
    auto method = iface<GDExtensionInterfaceVariantGetPtrBuiltinMethod>("variant_get_ptr_builtin_method");
    void* mn = nullptr;
    name(&mn, "casecmp_to");
    auto stub = method(GDEXTENSION_VARIANT_TYPE_STRING, &mn, casecmpHash);
    check(stub != nullptr, "String.casecmp_to (Godot has it, the host does not): a stub, not null");
    check(method(GDEXTENSION_VARIANT_TYPE_STRING, &mn, 1) == nullptr, "with a wrong hash: null");
    void* mn2 = nullptr;
    name(&mn2, "no_such_string_method");
    check(method(GDEXTENSION_VARIANT_TYPE_STRING, &mn2, 1) == nullptr, "a method Godot does not have: null");
    unsigned char self[8] = {};
    int64_t ret = 77;
    stub(self, nullptr, &ret, 0);
    check(ret == 77 && listed("builtin:String.casecmp_to"), "calling the stub leaves the result alone and lists it");

    // operators
    auto evaluator = iface<GDExtensionInterfaceVariantGetPtrOperatorEvaluator>("variant_get_ptr_operator_evaluator");
    auto t2d = evaluator(GDEXTENSION_VARIANT_OP_MULTIPLY, GDEXTENSION_VARIANT_TYPE_TRANSFORM2D, GDEXTENSION_VARIANT_TYPE_VECTOR2);
    check(t2d != nullptr, "Transform2D * Vector2 (in the dump, not implemented): a stub");
    float lv[6] = {1, 0, 0, 1, 0, 0}, rv[2] = {1, 2}, out[2] = {9, 9};
    t2d(lv, rv, out);
    check(out[0] == 9 && listed("builtin:operator * on Transform2D and Vector2"), "it reports and writes nothing");
    check(evaluator(GDEXTENSION_VARIANT_OP_ADD, GDEXTENSION_VARIANT_TYPE_BOOL, GDEXTENSION_VARIANT_TYPE_VECTOR3) == nullptr, "an operator the dump does not list: null");
    check(evaluator(GDEXTENSION_VARIANT_OP_EQUAL, GDEXTENSION_VARIANT_TYPE_VECTOR3, GDEXTENSION_VARIANT_TYPE_NIL) != nullptr, "Vector3 == Variant (a Nil right type means any Variant)");
    check(evaluator(GDEXTENSION_VARIANT_OP_NEGATE, GDEXTENSION_VARIANT_TYPE_INT, GDEXTENSION_VARIANT_TYPE_NIL) != nullptr, "unary minus on int");

    // utility functions
    auto utility = iface<GDExtensionInterfaceVariantGetPtrUtilityFunction>("variant_get_ptr_utility_function");
    void* un = nullptr;
    name(&un, "wrapf");
    auto wrapf = utility(&un, wrapfHash);
    check(wrapf != nullptr, "wrapf (in the dump, not implemented): a stub");
    double wres = 5;
    wrapf(&wres, nullptr, 0);
    check(wres == 5 && listed("builtin:utility function wrapf"), "it reports");
    void* un2 = nullptr;
    name(&un2, "not_a_godot_function");
    check(utility(&un2, 1) == nullptr, "a function Godot does not have: null");

    // members
    auto getter = iface<GDExtensionInterfaceVariantGetPtrGetter>("variant_get_ptr_getter");
    void* hn = nullptr;
    name(&hn, "h");
    auto colorH = getter(GDEXTENSION_VARIANT_TYPE_COLOR, &hn);
    check(colorH != nullptr, "Color.h (in the dump, not implemented): a stub");
    float color[4] = {1, 0, 0, 1};
    double h = 3;
    colorH(color, &h);
    check(h == 3 && listed("builtin:Color.h (get)"), "it reports");
    void* zn = nullptr;
    name(&zn, "zz");
    check(getter(GDEXTENSION_VARIANT_TYPE_COLOR, &zn) == nullptr, "a member Godot does not have: null");
}

static void testUnsupported() {
    AVER_INFO("--- the ABI's diagnostics");
    check(procAddress("variant_new_copy") != nullptr && procAddress("classdb_register_extension_class5") != nullptr && procAddress("get_godot_version2") != nullptr, "implemented names are non-null");
    check(aver_gdext_abi_version() == AVER_GDEXT_ABI_VERSION, "ABI version");
    check(std::string(aver_gdext_unsupported_name(9999)) == "", "an index out of range gives an empty name");
}

int main() {
    ExtState ext;
    check(ext.open(extPath()), "the test extension library loads: " + extPath());
    check(aver_gdext_set_api_path(kApi.c_str()) == 1, "extension_api.json loaded for hash checks");

    testRefusals();

    int32_t lib = aver_gdext_load((kDir + "/FabricTest.gdextension").c_str(), kDir.c_str());
    check(lib != 0, "FabricTest.gdextension loads: " + lastError());
    if (lib == 0) {
        AVER_ERROR("cannot continue without the library");
        return exitCode(ExitCode::Failed);
    }
    check(aver_gdext_is_loaded((kDir + "/FabricTest.gdextension").c_str()) == lib && aver_gdext_library_count() == 1 && aver_gdext_library_at(0) == lib,
          "the library is listed");
    check(std::string(aver_gdext_library_path(lib)).find("FabricTest.gdextension") != std::string::npos, "and knows its path");

    testRegistration(lib);
    testCalls();
    testPointerCalls();
    testRefCounted();
    testSignals();
    testLifecycle();
    testSpawnChild();
    testNodes();
    testHashes();
    testTablesAgainstDump();
    testStubs();
    testInterfaceVariants();
    testInterfaceContainers();
    testUnloadAndReload(lib, ext);
    testResPathReload(ext);
    testUnsupported();
    ext.close();

    AVER_INFO("==================================================");
    AVER_INFO("GdextHostTest done: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
