// The aver_gdext_* C ABI (fabric_gdext_abi.h) over the host.
#include "aver/fabric/gdext/fabric_gdext_abi.h"

#include "GdextIf.hpp"

#include "aver/platform/FileSystem.hpp"

#include <algorithm>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

using namespace aver::gdext;

namespace {

thread_local std::string t_error;
thread_local std::vector<unsigned char> t_result;
thread_local std::string t_text;       // storage behind the const char* returns
bool g_apiTried = false;

int32_t fail(const std::string& msg) { t_error = msg; return 0; }
int32_t ok(int32_t v = 1) { t_error.clear(); return v; }

HostObject* obj(int32_t handle) { return objectByHandle(handle); }

const char* text(std::string s) { t_text = std::move(s); return t_text.c_str(); }

std::string callErrorText(const GDExtensionCallError& e, const std::string& what) {
    switch (e.error) {
        case GDEXTENSION_CALL_ERROR_INVALID_METHOD: return what + ": no such method";
        case GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT: return what + ": argument " + std::to_string(e.argument + 1) + " has the wrong type (expected variant type " + std::to_string(e.expected) + ")";
        case GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS: return what + ": too many arguments (at most " + std::to_string(e.expected) + ")";
        case GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS: return what + ": too few arguments (at least " + std::to_string(e.expected) + ")";
        case GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL: return what + ": the instance is null";
        case GDEXTENSION_CALL_ERROR_METHOD_NOT_CONST: return what + ": method is not const";
        default: return what + ": failed";
    }
}

// An Array in wire form to its elements.
bool decodeArgs(const void* args, int32_t len, std::vector<V>& out) {
    out.clear();
    if (!args || len <= 0) return true;
    const unsigned char* p = static_cast<const unsigned char*>(args);
    const unsigned char* end = p + len;
    if (end - p < 5 || *p != T_ARRAY) return false;
    V arr;
    if (!wireDecode(p, end, &arr.v)) return false;
    for (const Var& e : arrayOf(&arr.v)->items) {
        out.emplace_back();
        varCopy(&out.back().v, &e);
    }
    return true;
}

int32_t publish(const Var* v) {
    t_result.clear();
    wireEncode(v, t_result);
    return int32_t(t_result.size());
}

const ClassDef* cls(const char* name) {
    ensureEngineClasses();
    return name ? findClass(name) : nullptr;
}

struct ListenCtx { AverGdextSignalFn fn; void* user; };

void listenTrampoline(void* user, HostObject* from, const char* signal, const Var* const* args, int argc) {
    auto* c = static_cast<ListenCtx*>(user);
    std::vector<unsigned char> buf;
    buf.push_back(T_ARRAY);
    const int32_t n = argc;
    buf.insert(buf.end(), reinterpret_cast<const unsigned char*>(&n), reinterpret_cast<const unsigned char*>(&n) + 4);
    for (int i = 0; i < argc; ++i) wireEncode(args[i], buf);
    c->fn(c->user, from->handle, signal, buf.data(), int32_t(buf.size()));
}

// The directory of the library this code is in (the executable's, when it is linked statically).
std::string moduleDir() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleDir), &module))
        return {};
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(module, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return {};
    const std::wstring w(buf, n);
    const size_t slash = w.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(slash), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len > 0 ? len : 0), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(slash), out.data(), len, nullptr, nullptr);
    return out;
#else
    Dl_info info;
    if (!dladdr(reinterpret_cast<void*>(&moduleDir), &info) || !info.dli_fname) return {};
    const std::string file = info.dli_fname;
    const size_t slash = file.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : file.substr(0, slash);
#endif
}

// extension_api.json: the extension's data/ folder (this library sits in <extension>/native/<platform>/), else beside the executable.
void defaultApi() {
    if (g_apiTried || host().api.loaded) return;
    g_apiTried = true;
    std::string path;
    const std::string dir = moduleDir();
    if (!dir.empty() && aver::fileExists(dir + "/../../data/extension_api.json")) path = dir + "/../../data/extension_api.json";
    else path = aver::executableDir() + "/extension_api.json";
    std::string err;
    if (aver::fileExists(path)) loadApiDump(path, &err);
    else logOnce("api-missing", "extension_api.json was not found in the extension's data folder or beside the executable; method hashes are not checked");
}

} // namespace

extern "C" {

int32_t aver_gdext_abi_version(void) { return AVER_GDEXT_ABI_VERSION; }
const char* aver_gdext_last_error(void) { return t_error.c_str(); }

int32_t aver_gdext_set_api_path(const char* path) {
    std::string err;
    g_apiTried = true;
    if (!path || !loadApiDump(path, &err)) return fail(err.empty() ? "no path" : err);
    return ok();
}

int32_t aver_gdext_set_features(const char* csv) {
    Host& h = host();
    h.features.clear();
    h.featuresSet = false;
    std::string s = csv ? csv : "";
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) { h.features.push_back(s.substr(i, j - i)); h.featuresSet = true; }
        i = j + 1;
    }
    return ok();
}

int32_t aver_gdext_load(const char* path, const char* projectDir) {
    if (!path || !*path) return fail("aver_gdext_load: no path");
    defaultApi();
    std::string err;
    ExtLibrary* lib = loadExtension(path, projectDir ? projectDir : "", &err);
    if (!lib) return fail(err.empty() ? "load failed" : err);
    return ok(lib->handle);
}

int32_t aver_gdext_unload(int32_t library) {
    std::string err;
    if (!unloadExtension(findLibrary(library), &err)) return fail(err);
    flushDeferred();
    return ok();
}

int32_t aver_gdext_is_loaded(const char* path) {
    ExtLibrary* l = path ? findLibraryByPath(path) : nullptr;
    return ok(l ? l->handle : 0);
}
int32_t aver_gdext_library_count(void) { return int32_t(host().libs.size()); }
int32_t aver_gdext_library_at(int32_t i) { return i >= 0 && size_t(i) < host().libs.size() ? host().libs[size_t(i)]->handle : 0; }
const char* aver_gdext_library_path(int32_t library) { ExtLibrary* l = findLibrary(library); return text(l ? l->gdextPath : std::string()); }

int32_t aver_gdext_class_count(int32_t library) {
    if (library == 0) return int32_t(host().classOrder.size());
    ExtLibrary* l = findLibrary(library);
    return l ? int32_t(l->classes.size()) : 0;
}
const char* aver_gdext_class_name(int32_t library, int32_t index) {
    const std::vector<std::string>* list = &host().classOrder;
    if (library != 0) { ExtLibrary* l = findLibrary(library); if (!l) return ""; list = &l->classes; }
    return index >= 0 && size_t(index) < list->size() ? text((*list)[size_t(index)]) : "";
}
int32_t aver_gdext_class_exists(const char* name) { const ClassDef* c = cls(name); return c ? (c->engine ? 2 : 1) : 0; }
const char* aver_gdext_class_parent(const char* name) { const ClassDef* c = cls(name); return text(c ? c->parent : std::string()); }
int32_t aver_gdext_class_is_a(const char* name, const char* base) { const ClassDef* c = cls(name); return c && base && classIsA(c, base) ? 1 : 0; }
int32_t aver_gdext_class_can_instantiate(const char* name) { const ClassDef* c = cls(name); return c && !c->isVirtual && !c->isAbstract ? 1 : 0; }

int32_t aver_gdext_method_count(const char* name) { const ClassDef* c = cls(name); return c ? int32_t(c->methods.size()) : 0; }
const char* aver_gdext_method_name(const char* name, int32_t i) {
    const ClassDef* c = cls(name);
    return c && i >= 0 && size_t(i) < c->methods.size() ? text(c->methods[size_t(i)]->name) : "";
}
int32_t aver_gdext_method_info(const char* name, int32_t i, int32_t* out4) {
    const ClassDef* c = cls(name);
    if (!c || !out4 || i < 0 || size_t(i) >= c->methods.size()) return fail("no such method");
    const MethodDef& m = *c->methods[size_t(i)];
    out4[0] = int32_t(m.args.size());
    out4[1] = int32_t(m.flags);
    out4[2] = m.hasRet ? (m.ret.type == kAnyType ? 0 : m.ret.type) : -1;
    out4[3] = m.vararg ? 1 : 0;
    return ok();
}
int32_t aver_gdext_method_arg_type(const char* name, int32_t i, int32_t arg) {
    const ClassDef* c = cls(name);
    if (!c || i < 0 || size_t(i) >= c->methods.size()) return -1;
    const MethodDef& m = *c->methods[size_t(i)];
    if (arg < 0 || size_t(arg) >= m.args.size()) return -1;
    return m.args[size_t(arg)].type == kAnyType ? 0 : m.args[size_t(arg)].type;
}
int32_t aver_gdext_property_count(const char* name) { const ClassDef* c = cls(name); return c ? int32_t(c->props.size()) : 0; }
const char* aver_gdext_property_name(const char* name, int32_t i) {
    const ClassDef* c = cls(name);
    return c && i >= 0 && size_t(i) < c->props.size() ? text(c->props[size_t(i)].info.name) : "";
}
int32_t aver_gdext_property_type(const char* name, int32_t i) {
    const ClassDef* c = cls(name);
    return c && i >= 0 && size_t(i) < c->props.size() ? c->props[size_t(i)].info.type : -1;
}
int32_t aver_gdext_signal_count(const char* name) { const ClassDef* c = cls(name); return c ? int32_t(c->signals.size()) : 0; }
const char* aver_gdext_signal_name(const char* name, int32_t i) {
    const ClassDef* c = cls(name);
    return c && i >= 0 && size_t(i) < c->signals.size() ? text(c->signals[size_t(i)].name) : "";
}
int32_t aver_gdext_signal_arg_count(const char* name, int32_t i) {
    const ClassDef* c = cls(name);
    return c && i >= 0 && size_t(i) < c->signals.size() ? int32_t(c->signals[size_t(i)].args.size()) : -1;
}
int32_t aver_gdext_constant_count(const char* name) { const ClassDef* c = cls(name); return c ? int32_t(c->consts.size()) : 0; }
const char* aver_gdext_constant_name(const char* name, int32_t i) {
    const ClassDef* c = cls(name);
    return c && i >= 0 && size_t(i) < c->consts.size() ? text(c->consts[size_t(i)].name) : "";
}
int64_t aver_gdext_constant_value(const char* name, int32_t i) {
    const ClassDef* c = cls(name);
    return c && i >= 0 && size_t(i) < c->consts.size() ? c->consts[size_t(i)].value : 0;
}

int32_t aver_gdext_instantiate(const char* name) {
    if (!name) return fail("instantiate: no class");
    ensureEngineClasses();
    const ClassDef* c = findClass(name);
    if (!c) return fail(std::string("instantiate: unknown class '") + name + "'");
    if (c->isVirtual || c->isAbstract) return fail(std::string("instantiate: class '") + name + "' is abstract");
    HostObject* o = instantiate(name);
    if (!o) return fail(std::string("instantiate: '") + name + "' could not be created");
    return ok(o->handle);
}

int32_t aver_gdext_free(int32_t h) {
    HostObject* o = obj(h);
    if (!o) return fail("free: bad object handle");
    destroyObject(o);
    return ok();
}
int32_t aver_gdext_object_is_valid(int32_t h) { return obj(h) ? 1 : 0; }
int64_t aver_gdext_object_instance_id(int32_t h) { HostObject* o = obj(h); return o ? int64_t(o->id) : 0; }
const char* aver_gdext_object_class(int32_t h) { HostObject* o = obj(h); return text(o ? o->cls : std::string()); }
int32_t aver_gdext_object_by_instance_id(int64_t id) { HostObject* o = objectById(uint64_t(id)); return o ? o->handle : 0; }
int64_t aver_gdext_object_get_managed(int32_t h) { HostObject* o = obj(h); return o ? o->managed : 0; }
int32_t aver_gdext_object_set_managed(int32_t h, int64_t v) { HostObject* o = obj(h); if (!o) return fail("bad object handle"); o->managed = v; return ok(); }

int32_t aver_gdext_call(int32_t h, const char* method, const void* args, int32_t len) {
    HostObject* o = obj(h);
    if (!o) { fail("call: bad object handle"); return -1; }
    if (!method) { fail("call: no method"); return -1; }
    std::vector<V> a;
    if (!decodeArgs(args, len, a)) { fail("call: the arguments are not a valid wire-form Array"); return -1; }
    std::vector<const Var*> p;
    for (V& v : a) p.push_back(v.ptr());
    V ret;
    GDExtensionCallError err{};
    if (!callByName(o, method, p.data(), int(p.size()), &ret.v, &err)) { fail(callErrorText(err, o->cls + "." + method)); return -1; }
    t_error.clear();
    return publish(&ret.v);
}

int32_t aver_gdext_get(int32_t h, const char* property) {
    HostObject* o = obj(h);
    if (!o || !property) { fail("get: bad object handle"); return -1; }
    V out;
    if (!getProperty(o, property, &out.v)) { fail(std::string("get: ") + o->cls + " has no property '" + property + "'"); return -1; }
    t_error.clear();
    return publish(&out.v);
}

int32_t aver_gdext_set(int32_t h, const char* property, const void* value, int32_t len) {
    HostObject* o = obj(h);
    if (!o || !property) return fail("set: bad object handle");
    const unsigned char* p = static_cast<const unsigned char*>(value);
    V v;
    if (!value || len <= 0 || !wireDecode(p, p + len, &v.v)) return fail("set: the value is not a valid wire-form Variant");
    if (!setProperty(o, property, v.ptr())) return fail(std::string("set: ") + o->cls + " has no settable property '" + property + "'");
    return ok();
}

int32_t aver_gdext_has_method(int32_t h, const char* method) {
    HostObject* o = obj(h);
    return o && method && findMethod(o->def, method) ? 1 : 0;
}

int32_t aver_gdext_result(void* out, int32_t cap) {
    const int32_t n = std::min<int32_t>(cap, int32_t(t_result.size()));
    if (out && n > 0) std::memcpy(out, t_result.data(), size_t(n));
    return n < 0 ? 0 : n;
}

int32_t aver_gdext_listen(int32_t h, AverGdextSignalFn fn, void* user) {
    HostObject* o = obj(h);
    if (!o || !fn) return fail("listen: bad object handle or callback");
    static std::vector<std::unique_ptr<ListenCtx>> contexts;
    contexts.push_back(std::make_unique<ListenCtx>(ListenCtx{fn, user}));
    addListener(o, Listener{&listenTrampoline, contexts.back().get()});
    return ok();
}

int32_t aver_gdext_emit(int32_t h, const char* signal, const void* args, int32_t len) {
    HostObject* o = obj(h);
    if (!o || !signal) return fail("emit: bad object handle");
    std::vector<V> a;
    if (!decodeArgs(args, len, a)) return fail("emit: the arguments are not a valid wire-form Array");
    std::vector<const Var*> p;
    for (V& v : a) p.push_back(v.ptr());
    const int rc = emitSignal(o, signal, p.data(), int(p.size()));
    if (rc != 0) return fail(std::string("emit: ") + o->cls + " has no signal '" + signal + "'");
    return ok();
}

int32_t aver_gdext_connect_method(int32_t h, const char* signal, int32_t target, const char* method, int32_t flags) {
    HostObject* o = obj(h);
    HostObject* t = obj(target);
    if (!o || !t || !signal || !method) return fail("connect: bad object handle");
    V callable = makeMethodCallable(t, method);
    if (connectSignal(o, signal, callable.ptr(), uint32_t(flags)) != 0) return fail(std::string("connect: cannot connect '") + signal + "' of " + o->cls);
    return ok();
}

int32_t aver_gdext_has_virtual(int32_t h, const char* name) { HostObject* o = obj(h); return o && name && findVirtual(o, name) ? 1 : 0; }
int32_t aver_gdext_enter_tree(int32_t h, int32_t recursive) { HostObject* o = obj(h); if (!o) return fail("bad object handle"); treeEnter(o, recursive != 0); return ok(); }
int32_t aver_gdext_ready(int32_t h, int32_t recursive) { HostObject* o = obj(h); if (!o) return fail("bad object handle"); treeReady(o, recursive != 0); return ok(); }
int32_t aver_gdext_exit_tree(int32_t h, int32_t recursive) { HostObject* o = obj(h); if (!o) return fail("bad object handle"); treeExit(o, recursive != 0); return ok(); }
int32_t aver_gdext_process(int32_t h, double delta, int32_t physics, int32_t recursive) {
    HostObject* o = obj(h);
    if (!o) return fail("bad object handle");
    // a process step stands for the node being in the tree: the embedder that calls this has put it there
    treeProcess(o, delta, physics != 0, recursive != 0);
    return ok();
}
int32_t aver_gdext_notification(int32_t h, int32_t what) { HostObject* o = obj(h); if (!o) return fail("bad object handle"); sendNotification(o, what, false); return ok(); }
int32_t aver_gdext_flush_deferred(void) { flushDeferred(); return ok(); }

int32_t aver_gdext_node_set_name(int32_t h, const char* name) {
    HostObject* o = obj(h);
    if (!o || !name || !classIsA(o->def, "Node")) return fail("set_name: not a node");
    V n = V::ofName(name);
    const Var* a[1] = {n.ptr()};
    V r;
    GDExtensionCallError e{};
    callByName(o, "set_name", a, 1, &r.v, &e);
    return ok();
}
const char* aver_gdext_node_get_name(int32_t h) { HostObject* o = obj(h); return text(o ? o->name : std::string()); }
int32_t aver_gdext_node_set_parent(int32_t child, int32_t parent) {
    HostObject* c = obj(child);
    HostObject* p = parent ? obj(parent) : nullptr;
    if (!c || (parent && !p)) return fail("set_parent: bad object handle");
    if (c->parent == p) return ok();
    if (c->parent) {
        auto& ch = c->parent->children;
        ch.erase(std::remove(ch.begin(), ch.end(), c), ch.end());
    }
    c->parent = p;
    if (p) p->children.push_back(c);
    return ok();
}
int32_t aver_gdext_node_pending_child(int32_t h) {
    HostObject* o = obj(h);
    if (!o) return 0;
    while (!o->pendingAdds.empty()) {
        HostObject* c = o->pendingAdds.front();
        o->pendingAdds.erase(o->pendingAdds.begin());
        if (c && !c->freed) return c->handle;
    }
    return 0;
}
int32_t aver_gdext_node_process_flags(int32_t h) {
    HostObject* o = obj(h);
    return o ? (o->process ? 1 : 0) | (o->physics ? 2 : 0) | (o->ready ? 4 : 0) : 0;
}
int32_t aver_gdext_node3d_get_transform(int32_t h, float* out12) {
    HostObject* o = obj(h);
    if (!o || !out12 || !classIsA(o->def, "Node3D")) return fail("get_transform: not a Node3D");
    std::memcpy(out12, o->xform, sizeof o->xform);
    return ok();
}
int32_t aver_gdext_node3d_set_transform(int32_t h, const float* in12) {
    HostObject* o = obj(h);
    if (!o || !in12 || !classIsA(o->def, "Node3D")) return fail("set_transform: not a Node3D");
    transformPush(o, in12);
    return ok();
}
int32_t aver_gdext_node3d_transform_version(int32_t h) { HostObject* o = obj(h); return o ? int32_t(o->xformVersion) : 0; }

static std::vector<std::string>& unsupportedList() {
    static std::vector<std::string> v;
    v.clear();
    Host& h = host();
    for (const auto& s : h.unsupportedCalls) v.push_back("interface:" + s);
    for (const auto& s : h.unsupportedMethods) v.push_back("method:" + s);
    for (const auto& s : h.unsupportedBuiltins) v.push_back("builtin:" + s);
    return v;
}
int32_t aver_gdext_unsupported_count(void) { return int32_t(unsupportedList().size()); }
const char* aver_gdext_unsupported_name(int32_t i) {
    auto& v = unsupportedList();
    return i >= 0 && size_t(i) < v.size() ? text(v[size_t(i)]) : "";
}

} // extern "C"
