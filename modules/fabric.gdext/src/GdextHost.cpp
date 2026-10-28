// The GDExtension host: registry, objects, calls, signals, virtuals, library loading (GdextHost.hpp).
#include "GdextHost.hpp"

#include "aver/core/DynamicLibrary.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/Json.hpp"
#include "aver/formats/godot/GodotProject.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace aver::gdext {

template <class T> static T ld(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }
template <class T> static void st(void* p, const T& t) { std::memcpy(p, &t, sizeof t); }

Host& host() {
    static Host* h = new Host(); // process lifetime: extension code may still call in during static destruction
    return *h;
}

void hostError(const std::string& msg) {
    host().lastError = msg;
    hostLog(2, msg);
}

void logOnce(const std::string& key, const std::string& msg) {
    if (host().loggedOnce.insert(key).second) hostLog(1, msg);
}

std::string nameOfSlot(const void* slot) { return slot ? nameText(ld<NameData*>(slot)) : std::string(); }
std::string stringOfSlot(const void* slot) {
    if (!slot) return {};
    auto* s = ld<StrData*>(slot);
    return s ? u32ToUtf8(s->s) : std::string();
}
void storeName(void* slot, std::string_view s) { st<NameData*>(slot, s.empty() ? nullptr : internName(s)); }
void storeString(void* slot, std::string_view s) { st<StrData*>(slot, s.empty() ? nullptr : new StrData{utf8ToU32(s)}); }

ArgInfo argFromPropertyInfo(const GDExtensionPropertyInfo& p) {
    ArgInfo a;
    a.type = p.type;
    if (p.type == GDEXTENSION_VARIANT_TYPE_NIL) a.type = kAnyType;      // "Variant" arguments are NIL in the registration structs
    a.name = nameOfSlot(p.name);
    a.className = nameOfSlot(p.class_name);
    a.hint = p.hint;
    a.usage = p.usage;
    a.hintString = stringOfSlot(p.hint_string);
    return a;
}

// ---------------------------------------------------------------------------------------------------
// classes

MethodDef* ClassDef::addMethod(std::unique_ptr<MethodDef> m) {
    m->owner = this;
    MethodDef* raw = m.get();
    methodByName[m->name] = raw;
    methods.push_back(std::move(m));
    return raw;
}

ClassDef* findClass(std::string_view name) {
    auto it = host().classes.find(std::string(name));
    return it == host().classes.end() ? nullptr : it->second.get();
}

bool classIsA(const ClassDef* c, std::string_view base) {
    for (; c; c = c->parentDef)
        if (c->name == base) return true;
    return false;
}

MethodDef* findMethod(const ClassDef* c, std::string_view name) {
    for (; c; c = c->parentDef) {
        auto it = c->methodByName.find(std::string(name));
        if (it != c->methodByName.end()) return it->second;
    }
    return nullptr;
}

const SignalDef* findSignal(const ClassDef* c, std::string_view name) {
    for (; c; c = c->parentDef)
        for (const SignalDef& s : c->signals)
            if (s.name == name) return &s;
    return nullptr;
}

const PropDef* findProp(const ClassDef* c, std::string_view name) {
    for (; c; c = c->parentDef)
        for (const PropDef& p : c->props)
            if (p.info.name == name) return &p;
    return nullptr;
}

ClassDef* defineEngineClass(const char* name, const char* parent, bool refCounted) {
    auto c = std::make_unique<ClassDef>();
    c->name = name;
    c->parent = parent ? parent : "";
    c->engine = true;
    c->refCounted = refCounted;
    c->parentDef = parent && *parent ? findClass(parent) : nullptr;
    ClassDef* raw = c.get();
    host().classes[c->name] = std::move(c);
    return raw;
}

void parseSig(MethodDef& m, const char* args, const char* ret) {
    std::string s = args ? args : "";
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) {
            ArgInfo a;
            a.type = typeFromName(std::string_view(s).substr(i, j - i));
            a.name = "arg" + std::to_string(n++);
            m.args.push_back(a);
        }
        i = j + 1;
    }
    if (ret && *ret) { m.hasRet = true; m.ret.type = typeFromName(ret); }
}

MethodDef* defineNative(ClassDef* c, const char* name, const char* args, const char* ret, NativeFn fn, bool vararg) {
    auto m = std::make_unique<MethodDef>();
    m->name = name;
    m->native = fn;
    m->vararg = vararg;
    parseSig(*m, args, ret);
    return c->addMethod(std::move(m));
}

// ---------------------------------------------------------------------------------------------------
// objects

HostObject* newObject(ClassDef* def, const std::string& name) {
    Host& h = host();
    auto o = std::make_unique<HostObject>();
    o->id = h.nextId++;
    o->handle = h.nextHandle++;
    o->def = def;
    o->cls = def->name;
    o->name = name;
    HostObject* raw = o.get();
    h.byId[raw->id] = raw;
    h.byHandle[raw->handle] = raw;
    h.live[raw->id] = std::move(o);
    return raw;
}

HostObject* instantiate(std::string_view className, bool forScript) {
    ensureEngineClasses();
    ClassDef* c = findClass(className);
    if (!c) return nullptr;
    if (!forScript && (c->isVirtual || c->isAbstract)) return nullptr;
    if (c->engine) {
        return newObject(c);
    }
    GDExtensionObjectPtr p = nullptr;
    if (c->f.create2) p = c->f.create2(c->f.classUserdata, 1);
    else if (c->f.create) p = c->f.create(c->f.classUserdata);
    if (!p) { hostError("class '" + c->name + "' did not create an instance"); return nullptr; }
    return static_cast<HostObject*>(p);
}

HostObject* objectById(uint64_t id) {
    auto it = host().byId.find(id);
    return it == host().byId.end() ? nullptr : it->second;
}

HostObject* objectByHandle(int32_t handle) {
    auto it = host().byHandle.find(handle);
    return it == host().byHandle.end() ? nullptr : it->second;
}

std::string objectToString(HostObject* o) {
    if (!o) return "<null>";
    if (o->def && o->instance && o->def->f.toString) {
        GDExtensionBool valid = 0;
        void* slot = nullptr;
        storeString(&slot, "");
        o->def->f.toString(o->instance, &valid, &slot);
        std::string s = stringOfSlot(&slot);
        delete ld<StrData*>(&slot);
        if (valid) return s;
    }
    return "<" + o->cls + "#" + std::to_string(o->id) + ">";
}

static void detachFromParent(HostObject* o) {
    if (!o->parent) return;
    auto& ch = o->parent->children;
    ch.erase(std::remove(ch.begin(), ch.end(), o), ch.end());
    auto& pa = o->parent->pendingAdds;
    pa.erase(std::remove(pa.begin(), pa.end(), o), pa.end());
    o->parent = nullptr;
}

void destroyObject(HostObject* o) {
    if (!o || o->freed) return;
    Host& h = host();
    if (o->def && !o->def->engine && o->instance) sendNotification(o, 1, false); // NOTIFICATION_PREDELETE
    if (o->inTree) emitSignal(o, "tree_exiting", nullptr, 0);
    // children go first (Godot frees a node's subtree with it)
    std::vector<HostObject*> kids = o->children;
    for (HostObject* c : kids) { c->parent = nullptr; destroyObject(c); }
    o->children.clear();
    detachFromParent(o);
    if (o->def && !o->def->engine && o->instance && o->def->f.free) {
        void* inst = o->instance;
        o->def->f.free(o->def->f.classUserdata, inst);
    }
    for (Binding& b : o->bindings)
        if (b.cb.free_callback) b.cb.free_callback(b.token, o, b.binding);
    o->bindings.clear();
    o->instance = nullptr;
    o->conns.clear();
    o->listeners.clear();
    o->meta.clear();
    o->freed = true;
    h.byId.erase(o->id);
    h.byHandle.erase(o->handle);
    for (auto& s : h.singletons)
        if (s.second == o) { h.singletons.erase(s.first); break; }
    auto it = h.live.find(o->id);
    if (it != h.live.end()) {
        h.tombstones.push_back(std::move(it->second));
        h.live.erase(it);
    }
}

void flushDeferred() {
    Host& h = host();
    // a deferred call may queue more; run until the queue is dry (bounded)
    for (int guard = 0; guard < 64 && !h.deferred.empty(); ++guard) {
        std::vector<std::function<void()>> run;
        run.swap(h.deferred);
        for (auto& f : run) f();
    }
}

// ---------------------------------------------------------------------------------------------------
// calls

static bool argAccepts(const ArgInfo& want, const Var* got) {
    if (want.type == kAnyType || want.type == got->type) return true;
    if (want.type == T_OBJECT) return got->type == T_NIL;
    if (got->type == T_NIL) return false;
    return varCanConvert(got->type, want.type, true);
}

bool callMethod(HostObject* self, MethodDef* m, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err) {
    err->error = GDEXTENSION_CALL_OK; err->argument = 0; err->expected = 0;
    if (!m->isStatic && !self) { err->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL; return false; }
    const int n = int(m->args.size()), nd = int(m->defaults.size());
    if (!m->vararg && argc > n) { err->error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS; err->expected = n; return false; }
    if (argc < n - nd) { err->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS; err->expected = n - nd; return false; }
    std::vector<const Var*> full(args, args + argc);
    for (int i = argc; i < n; ++i) full.push_back(m->defaults[size_t(i - (n - nd))].ptr());
    for (int i = 0; i < std::min(n, argc); ++i)
        if (!argAccepts(m->args[size_t(i)], args[i])) {
            err->error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT; err->argument = i; err->expected = m->args[size_t(i)].type;
            return false;
        }
    if (m->native) {
        m->native(self, full.data(), int(full.size()), ret, err);
    } else if (m->callFn) {
        m->callFn(m->userdata, self ? self->instance : nullptr, reinterpret_cast<const GDExtensionConstVariantPtr*>(full.data()),
                  GDExtensionInt(full.size()), ret, err);
    } else {
        err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    }
    return err->error == GDEXTENSION_CALL_OK;
}

bool callByName(HostObject* self, std::string_view name, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err) {
    err->error = GDEXTENSION_CALL_OK; err->argument = 0; err->expected = 0;
    if (!self) { err->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL; return false; }
    MethodDef* m = findMethod(self->def, name);
    if (!m) { err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD; return false; }
    return callMethod(self, m, args, argc, ret, err);
}

void readNative(int type, const void* p, Var* out) {         // out uninitialised
    if (type == kAnyType) { varCopy(out, static_cast<const Var*>(p)); return; }
    if (type == T_OBJECT) {
        HostObject* o = ld<HostObject*>(p);
        varNil(out);
        if (o && !o->freed) { out->type = T_OBJECT; st(out->d, o); }
        return;
    }
    varSetNative(out, type, p);
}

void writeNative(const Var* v, int type, void* dst) {         // dst holds an initialised value of `type`
    if (type == kAnyType) { varAssign(static_cast<Var*>(dst), v); return; }
    if (type == T_OBJECT) { st<HostObject*>(dst, varObject(v)); return; }
    nativeDestroy(type, dst);
    V conv;
    const Var* src = v;
    if (v->type != type) {
        if (!varConvert(v, type, &conv.v)) { nativeInit(type, dst); return; }
        src = &conv.v;
    }
    varGetNative(src, dst);
}

bool callPtr(HostObject* self, MethodDef* m, const void* const* args, void* ret) {
    if (m->ptrFn && !m->native) {
        m->ptrFn(m->userdata, self ? self->instance : nullptr, args, ret);
        return true;
    }
    const int n = int(m->args.size());
    std::vector<V> vars(size_t(std::max(n, 1)));
    std::vector<const Var*> ptrs(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        readNative(m->args[size_t(i)].type, args[i], &vars[size_t(i)].v);
        ptrs[size_t(i)] = &vars[size_t(i)].v;
    }
    V result;
    GDExtensionCallError err{};
    const bool ok = callMethod(self, m, ptrs.data(), n, &result.v, &err);
    if (ok && m->hasRet && ret) writeNative(&result.v, m->ret.type, ret);
    return ok;
}

V makeMethodCallable(HostObject* o, std::string_view method) {
    V r;
    auto* c = new CallableData();
    c->objectId = o ? o->id : 0;
    c->method = internName(method);
    r.v.type = T_CALLABLE;
    st<CallableData*>(r.v.d, c);
    return r;
}

V makeCustomCallable(const GDExtensionCallableCustomInfo2& info) {
    V r;
    auto* c = new CallableData();
    c->custom = true;
    c->info = info;
    r.v.type = T_CALLABLE;
    st<CallableData*>(r.v.d, c);
    return r;
}

bool callCallable(const Var* callable, const Var* const* args, int argc, Var* ret, GDExtensionCallError* err) {
    err->error = GDEXTENSION_CALL_OK; err->argument = 0; err->expected = 0;
    auto* c = callable->type == T_CALLABLE ? ld<CallableData*>(callable->d) : nullptr;
    if (!c) { err->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL; return false; }
    if (c->custom) {
        if (c->info.is_valid_func && !c->info.is_valid_func(c->info.callable_userdata)) { err->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL; return false; }
        c->info.call_func(c->info.callable_userdata, reinterpret_cast<const GDExtensionConstVariantPtr*>(args), argc, ret, err);
        return err->error == GDEXTENSION_CALL_OK;
    }
    HostObject* o = objectById(c->objectId);
    return callByName(o, nameText(c->method), args, argc, ret, err);
}

// ---------------------------------------------------------------------------------------------------
// properties

bool getProperty(HostObject* o, std::string_view name, Var* out) {
    varNil(out);
    if (!o) return false;
    if (const PropDef* p = findProp(o->def, name)) {
        if (!p->getter.empty()) {
            V idx = V::ofInt(p->index);
            const Var* a[1] = {idx.ptr()};
            GDExtensionCallError err{};
            return callByName(o, p->getter, a, p->index >= 0 ? 1 : 0, out, &err);
        }
    }
    for (const ClassDef* c = o->def; c; c = c->parentDef)
        if (!c->engine && c->f.get && o->instance) {
            void* slot = nullptr;
            storeName(&slot, name);
            Var tmp; varNil(&tmp);
            const bool ok = c->f.get(o->instance, &slot, &tmp) != 0;
            if (ok) { varDestroy(out); *out = tmp; } else varDestroy(&tmp);
            if (ok) return true;
            break;
        }
    return false;
}

bool setProperty(HostObject* o, std::string_view name, const Var* value) {
    if (!o) return false;
    if (const PropDef* p = findProp(o->def, name)) {
        if (!p->setter.empty()) {
            V idx = V::ofInt(p->index);
            const Var* a[2] = {idx.ptr(), value};
            GDExtensionCallError err{};
            V ret;
            return p->index >= 0 ? callByName(o, p->setter, a, 2, &ret.v, &err) : callByName(o, p->setter, a + 1, 1, &ret.v, &err);
        }
    }
    for (const ClassDef* c = o->def; c; c = c->parentDef)
        if (!c->engine && c->f.set && o->instance) {
            void* slot = nullptr;
            storeName(&slot, name);
            if (c->f.set(o->instance, &slot, value)) return true;
            break;
        }
    return false;
}

// ---------------------------------------------------------------------------------------------------
// signals

int connectSignal(HostObject* o, std::string_view signal, const Var* callable, uint32_t flags) {
    if (!o || callable->type != T_CALLABLE) return kErrInvalidParameter;
    if (!findSignal(o->def, signal)) { hostLog(2, "connect: object of class " + o->cls + " has no signal '" + std::string(signal) + "'"); return kErrInvalidParameter; }
    auto& list = o->conns[std::string(signal)];
    for (const Conn& c : list)
        if (varEqual(c.callable.ptr(), callable)) return kErrInvalidParameter;       // already connected
    Conn c;
    c.callable = V();
    varAssign(&c.callable.v, callable);
    c.flags = flags;
    list.push_back(std::move(c));
    return kErrOk;
}

int disconnectSignal(HostObject* o, std::string_view signal, const Var* callable) {
    if (!o) return kErrInvalidParameter;
    auto it = o->conns.find(std::string(signal));
    if (it == o->conns.end()) return kErrInvalidParameter;
    auto& list = it->second;
    for (size_t i = 0; i < list.size(); ++i)
        if (varEqual(list[i].callable.ptr(), callable)) { list.erase(list.begin() + long(i)); return kErrOk; }
    return kErrInvalidParameter;
}

void addListener(HostObject* o, Listener l) { if (o) o->listeners.push_back(l); }

int emitSignal(HostObject* o, std::string_view signal, const Var* const* args, int argc) {
    if (!o || o->freed) return kErrUnavailable;
    if (!findSignal(o->def, signal)) return kErrUnavailable;
    const std::string key(signal);
    std::vector<Conn> list;
    if (auto it = o->conns.find(key); it != o->conns.end()) list = it->second;
    std::vector<Listener> listeners = o->listeners;
    // the connected callables first (connection order), then the embedder's listeners
    for (Conn& c : list) {
        if (c.flags & 4) disconnectSignal(o, signal, c.callable.ptr());           // CONNECT_ONE_SHOT
        if (c.flags & 1) {                                                         // CONNECT_DEFERRED
            std::vector<V> copy(size_t(std::max(argc, 1)));
            for (int i = 0; i < argc; ++i) varAssign(&copy[size_t(i)].v, args[i]);
            V cb = c.callable;
            host().deferred.push_back([cb, copy, argc]() {
                std::vector<const Var*> p;
                for (int i = 0; i < argc; ++i) p.push_back(copy[size_t(i)].ptr());
                V r; GDExtensionCallError e{};
                callCallable(cb.ptr(), p.data(), argc, &r.v, &e);
            });
            continue;
        }
        V r;
        GDExtensionCallError e{};
        if (!callCallable(c.callable.ptr(), args, argc, &r.v, &e) && e.error != GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL)
            hostLog(2, "signal '" + key + "': connected callable failed (error " + std::to_string(int(e.error)) + ")");
    }
    for (Listener& l : listeners) l.fn(l.user, o, key.c_str(), args, argc);
    return kErrOk;
}

// ---------------------------------------------------------------------------------------------------
// virtuals and notifications

static uint32_t virtualHash(const ClassDef* c, const std::string& name) {
    const Host& h = host();
    if (!h.api.loaded) return 0;
    for (; c; c = c->parentDef) {
        std::string cn = c->name;
        for (std::string p = cn; !p.empty();) {
            auto it = h.api.classes.find(p);
            if (it != h.api.classes.end()) {
                auto m = it->second.find(name);
                if (m != it->second.end()) return m->second.hash;
            }
            auto pit = h.api.parent.find(p);
            p = pit == h.api.parent.end() ? std::string() : pit->second;
        }
    }
    return 0;
}

VirtualSlot* findVirtual(HostObject* o, std::string_view name) {
    if (!o || !o->instance) return nullptr;
    const std::string key(name);
    for (ClassDef* c = o->def; c; c = c->parentDef) {
        if (c->engine) continue;
        auto it = c->virtualCache.find(key);
        if (it == c->virtualCache.end()) {
            VirtualSlot slot;
            void* nameSlot = nullptr;
            storeName(&nameSlot, name);
            const uint32_t hash = virtualHash(c, key);
            if (c->f.getVirtual2) slot.fn = c->f.getVirtual2(c->f.classUserdata, &nameSlot, hash);
            else if (c->f.getVirtual) slot.fn = c->f.getVirtual(c->f.classUserdata, &nameSlot);
            if (!slot.fn && c->f.callVirtualWithData) {
                if (c->f.getVirtualData2) slot.data = c->f.getVirtualData2(c->f.classUserdata, &nameSlot, hash);
                else if (c->f.getVirtualData) slot.data = c->f.getVirtualData(c->f.classUserdata, &nameSlot);
                slot.withData = slot.data != nullptr;
            }
            it = c->virtualCache.emplace(key, slot).first;
        }
        if (it->second.present()) return &it->second;
    }
    return nullptr;
}

bool callVirtual(HostObject* o, std::string_view name, const void* const* args, void* ret) {
    VirtualSlot* s = findVirtual(o, name);
    if (!s) return false;
    if (s->fn) { s->fn(o->instance, args, ret); return true; }
    for (ClassDef* c = o->def; c; c = c->parentDef) {
        if (c->engine || !c->f.callVirtualWithData) continue;
        void* nameSlot = nullptr;
        storeName(&nameSlot, name);
        c->f.callVirtualWithData(o->instance, &nameSlot, s->data, args, ret);
        return true;
    }
    return false;
}

void sendNotification(HostObject* o, int what, bool reversed) {
    if (!o || !o->instance) return;
    for (ClassDef* c = o->def; c; c = c->parentDef) {
        if (c->engine) continue;
        if (c->f.notification) { c->f.notification(o->instance, what, reversed ? 1 : 0); return; }
        if (c->f.notification1) { c->f.notification1(o->instance, what); return; }
    }
}

// ---------------------------------------------------------------------------------------------------
// extension_api.json

bool ApiDump::load(const std::string& file, std::string* err) {
    std::string text;
    if (!readFileText(file, text)) { if (err) *err = "cannot read " + file; return false; }
    fmt::JsonValue root;
    std::string why;
    if (!fmt::parseJson(text, root, &why)) { if (err) *err = file + ": " + why; return false; }
    if (!root.isObject() || !root["classes"].isArray()) { if (err) *err = file + ": not an extension API dump"; return false; }
    *this = ApiDump();
    path = file;
    auto readMethods = [](const fmt::JsonValue& arr, std::unordered_map<std::string, ApiMethod>& out) {
        for (const fmt::JsonValue& m : arr.elements()) {
            ApiMethod am;
            am.hash = uint32_t(m["hash"].asDouble());
            for (const fmt::JsonValue& c : m["hash_compatibility"].elements()) am.compat.push_back(uint32_t(c.asDouble()));
            am.isVirtual = m["is_virtual"].asBool();
            am.vararg = m["is_vararg"].asBool();
            out[std::string(m["name"].asString())] = am;
        }
    };
    for (const fmt::JsonValue& c : root["classes"].elements()) {
        const std::string name(c["name"].asString());
        parent[name] = std::string(c["inherits"].asString());
        readMethods(c["methods"], classes[name]);
    }
    for (const fmt::JsonValue& c : root["builtin_classes"].elements()) {
        const std::string bn(c["name"].asString());
        readMethods(c["methods"], builtins[bn]);
        for (const fmt::JsonValue& m : c["members"].elements()) builtinMembers[bn].push_back(std::string(m["name"].asString()));
        for (const fmt::JsonValue& o : c["operators"].elements())
            builtinOperators[bn].push_back({std::string(o["name"].asString()), std::string(o["right_type"].asString())});
    }
    for (const fmt::JsonValue& u : root["utility_functions"].elements()) {
        ApiMethod am;
        am.hash = uint32_t(u["hash"].asDouble());
        am.vararg = u["is_vararg"].asBool();
        utilities[std::string(u["name"].asString())] = am;
    }
    // native struct sizes from the field list (C layout, natural alignment)
    struct Sz { const char* t; int size; };
    static const Sz kTypes[] = {{"int", 4}, {"float", 4}, {"bool", 1}, {"uint8_t", 1}, {"int8_t", 1}, {"uint16_t", 2}, {"int16_t", 2},
                                {"uint32_t", 4}, {"int32_t", 4}, {"uint64_t", 8}, {"int64_t", 8}, {"double", 8}, {"RID", 8},
                                {"ObjectID", 8}, {"Vector2", 8}, {"Vector2i", 8}, {"Vector3", 12}, {"Rect2", 16}, {"Rect2i", 16},
                                {"Color", 16}, {"Vector4", 16}};
    for (const fmt::JsonValue& n : root["native_structures"].elements()) {
        std::string format(n["format"].asString());
        int offset = 0, maxAlign = 1;
        bool ok = true;
        size_t i = 0;
        while (i < format.size() && ok) {
            size_t j = format.find(';', i);
            if (j == std::string::npos) j = format.size();
            std::string field = format.substr(i, j - i);
            i = j + 1;
            if (size_t eq = field.find('='); eq != std::string::npos) field.erase(eq);
            while (!field.empty() && field.back() == ' ') field.pop_back();
            size_t sp = field.find_last_of(' ');
            std::string type = sp == std::string::npos ? field : field.substr(0, sp);
            int size = -1;
            if (type.find('*') != std::string::npos) size = 8;
            else if (type.find("::") != std::string::npos) size = 4;                    // an enum
            else for (const Sz& s : kTypes) if (type == s.t) size = s.size;
            if (size < 0) { ok = false; break; }
            int align = std::min(size, 8);
            if (size == 12 || size == 16) align = 4;
            if (size == 8 && (type == "Vector2" || type == "Vector2i")) align = 4;
            offset = (offset + align - 1) / align * align + size;
            maxAlign = std::max(maxAlign, align);
        }
        if (ok) nativeStructSizes[std::string(n["name"].asString())] = (offset + maxAlign - 1) / maxAlign * maxAlign;
    }
    loaded = true;
    return true;
}

bool loadApiDump(const std::string& path, std::string* err) {
    ApiDump d;
    if (!d.load(path, err)) return false;
    host().api = std::move(d);
    return true;
}

// ---------------------------------------------------------------------------------------------------
// libraries

std::vector<std::string> defaultFeatures() {
    std::vector<std::string> f;
#if defined(_WIN32)
    f.push_back("windows");
#elif defined(__APPLE__)
    f.push_back("macos");
#else
    f.push_back("linux");
#endif
#if defined(NDEBUG)
    f.push_back("release"); f.push_back("template_release");
#else
    f.push_back("debug"); f.push_back("template_debug");
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    f.push_back("arm64");
#else
    f.push_back("x86_64");
#endif
    f.push_back("64");
    f.push_back("single");
    return f;
}

ExtLibrary* findLibrary(int32_t handle) {
    for (auto& l : host().libs) if (l->handle == handle) return l.get();
    return nullptr;
}

ExtLibrary* findLibraryByPath(const std::string& gdextPath) {
    std::string norm = std::filesystem::path(gdextPath).lexically_normal().generic_string();
    for (auto& l : host().libs) if (l->gdextPath == norm) return l.get();
    return nullptr;
}

static bool parseVersion(const std::string& s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (s.empty()) return false;
    int n = 0;
    const char* p = s.c_str();
    while (*p && n < 3) {
        char* end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p) return false;
        out[n++] = int(v);
        p = end;
        if (*p == '.') ++p; else break;
    }
    return true;
}

static int cmpVersion(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

void unregisterClass(ExtLibrary* lib, const std::string& name) {
    Host& h = host();
    auto it = h.classes.find(name);
    if (it == h.classes.end() || it->second->engine) return;
    // instances of the class (or a class that derives from it) go with it
    std::vector<HostObject*> victims;
    for (auto& kv : h.live)
        if (classIsA(kv.second->def, name)) victims.push_back(kv.second.get());
    for (HostObject* o : victims) destroyObject(o);
    for (auto& kv : h.classes)
        if (kv.second->parentDef == it->second.get()) kv.second->parentDef = nullptr;
    for (auto bit = h.binds.begin(); bit != h.binds.end();) {
        if (bit->second->cls == it->second.get()) bit = h.binds.erase(bit); else ++bit;
    }
    h.classes.erase(it);
    h.classOrder.erase(std::remove(h.classOrder.begin(), h.classOrder.end(), name), h.classOrder.end());
    if (lib) lib->classes.erase(std::remove(lib->classes.begin(), lib->classes.end(), name), lib->classes.end());
}

ExtLibrary* loadExtension(const std::string& gdextPath, const std::string& projectDir, std::string* err) {
    Host& h = host();
    auto fail = [&](const std::string& m) -> ExtLibrary* { if (err) *err = m; h.lastError = m; hostLog(2, m); return nullptr; };
    ensureEngineClasses();
    if (ExtLibrary* have = findLibraryByPath(gdextPath)) return have;

    fmt::godot::GodotGdExtension ext;
    fmt::godot::GodotDiagnostics diag;
    if (!fmt::godot::loadGodotGdExtension(gdextPath, ext, diag, gdextPath)) {
        std::string m = gdextPath + ": cannot read the .gdextension file";
        for (const auto& d : diag.items) if (d.severity == fmt::godot::GodotSeverity::Error) { m = d.str(); break; }
        return fail(m);
    }
    if (ext.entrySymbol.empty()) return fail(gdextPath + ": [configuration] has no entry_symbol");

    // compatibility_minimum must be 4.1 or later (a missing key means 4.0, which the interface cannot serve)
    int minV[3], ourV[3] = {4, 5, 0}, maxV[3];
    if (ext.compatibilityMinimum.empty() || !parseVersion(ext.compatibilityMinimum, minV))
        return fail(gdextPath + ": compatibility_minimum is missing; extensions for Godot 4.0 are not supported (4.1 or later is required)");
    const int need41[3] = {4, 1, 0};
    if (cmpVersion(minV, need41) < 0)
        return fail(gdextPath + ": compatibility_minimum " + ext.compatibilityMinimum + " is older than 4.1; GDExtensions for Godot 4.0 are not supported");
    if (cmpVersion(minV, ourV) > 0)
        return fail(gdextPath + ": needs Godot " + ext.compatibilityMinimum + " or later; this host implements the 4.5 interface");
    if (!ext.compatibilityMaximum.empty() && parseVersion(ext.compatibilityMaximum, maxV) && cmpVersion(ourV, maxV) > 0)
        return fail(gdextPath + ": compatibility_maximum " + ext.compatibilityMaximum + " is older than the 4.5 interface this host implements");

    const std::vector<std::string>& features = h.featuresSet ? h.features : (h.features = defaultFeatures());
    const auto* sel = ext.selectLibrary(features);
    if (!sel) {
        std::string tags;
        for (const auto& f : features) tags += (tags.empty() ? "" : ",") + f;
        return fail(gdextPath + ": no [libraries] entry matches this platform (features: " + tags + ")");
    }

    namespace fs = std::filesystem;
    fs::path libPath;
    const std::string& raw = sel->path;
    if (raw.rfind("res://", 0) == 0) libPath = fs::path(projectDir.empty() ? "." : projectDir) / raw.substr(6);
    else if (fs::path(raw).is_absolute()) libPath = raw;
    else libPath = fs::path(gdextPath).parent_path() / raw;
    libPath = libPath.lexically_normal();
    std::error_code ec;
    if (!fs::exists(libPath, ec)) return fail(gdextPath + ": library '" + libPath.generic_string() + "' not found");

    void* mod = loadLibrary(libPath.string().c_str());
    if (!mod) return fail(gdextPath + ": cannot load '" + libPath.generic_string() + "': " + libraryError());
    void* sym = librarySymbol(mod, ext.entrySymbol.c_str());
    if (!sym) { freeLibrary(mod); return fail(gdextPath + ": entry symbol '" + ext.entrySymbol + "' not found in the library"); }

    auto lib = std::make_unique<ExtLibrary>();
    lib->handle = h.nextLib++;
    lib->gdextPath = fs::path(gdextPath).lexically_normal().generic_string();
    lib->libraryPath = libPath.generic_string();
    lib->entrySymbol = ext.entrySymbol;
    lib->module = mod;
    ExtLibrary* L = lib.get();
    h.libs.push_back(std::move(lib));

    ExtLibrary* prev = h.current;
    h.current = L;
    auto entry = reinterpret_cast<GDExtensionInitializationFunction>(sym);
    GDExtensionBool ok = entry(&procAddress, L, &L->init);
    h.current = prev;
    if (!ok) {
        std::string name = L->gdextPath;
        for (const std::string& c : std::vector<std::string>(L->classes)) unregisterClass(L, c);
        freeLibrary(mod);
        for (size_t i = 0; i < h.libs.size(); ++i) if (h.libs[i].get() == L) { h.libs.erase(h.libs.begin() + long(i)); break; }
        return fail(name + ": the entry function '" + ext.entrySymbol + "' reported failure");
    }
    L->open = true;

    // CORE, SERVERS, SCENE from the extension's minimum; the editor level is never run
    int first = std::max<int>(0, int(L->init.minimum_initialization_level));
    if (first > GDEXTENSION_INITIALIZATION_SCENE) first = GDEXTENSION_INITIALIZATION_SCENE;
    h.current = L;
    for (int level = first; level <= GDEXTENSION_INITIALIZATION_SCENE; ++level) {
        if (L->init.initialize) L->init.initialize(L->init.userdata, GDExtensionInitializationLevel(level));
        L->levelsSeen.push_back(level);
        L->levelInitialized = level;
    }
    h.current = prev;
    if (L->hasMainLoop && L->mainLoop.startup_func) L->mainLoop.startup_func();
    return L;
}

bool unloadExtension(ExtLibrary* lib, std::string* err) {
    Host& h = host();
    if (!lib) { if (err) *err = "no such library"; return false; }
    ExtLibrary* prev = h.current;
    h.current = lib;
    if (lib->hasMainLoop && lib->mainLoop.shutdown_func) lib->mainLoop.shutdown_func();
    for (int level = lib->levelInitialized; level >= 0; --level) {
        if (level < int(lib->init.minimum_initialization_level)) break;
        if (lib->init.deinitialize) lib->init.deinitialize(lib->init.userdata, GDExtensionInitializationLevel(level));
        lib->levelInitialized = level - 1;
    }
    // what the extension left behind: its objects, then its classes
    std::vector<HostObject*> victims;
    for (auto& kv : h.live)
        if (kv.second->def && kv.second->def->lib == lib) victims.push_back(kv.second.get());
    for (HostObject* o : victims) destroyObject(o);
    for (const std::string& c : std::vector<std::string>(lib->classes)) unregisterClass(lib, c);
    h.current = prev;
    if (lib->module) freeLibrary(lib->module);
    for (size_t i = 0; i < h.libs.size(); ++i)
        if (h.libs[i].get() == lib) { h.libs.erase(h.libs.begin() + long(i)); break; }
    // Method binds of engine classes stay: an extension that is loaded again in the same process keeps the ones it cached.
    if (h.libs.empty()) h.tombstones.clear();
    return true;
}

} // namespace aver::gdext
