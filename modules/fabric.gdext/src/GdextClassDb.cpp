// Interface functions about classes and objects: registration of extension classes, method binds (with the hash check against
// extension_api.json), instance bindings, signals' callables, singletons.
#include "GdextIf.hpp"

namespace aver::gdext {

template <class T> static T ld(const void* p) { T t; std::memcpy(&t, p, sizeof t); return t; }
template <class T> static void st(void* p, const T& t) { std::memcpy(p, &t, sizeof t); }

static ExtLibrary* L(GDExtensionClassLibraryPtr p) { return static_cast<ExtLibrary*>(p); }
static HostObject* O(GDExtensionConstObjectPtr p) { return static_cast<HostObject*>(const_cast<void*>(p)); }

// ---------------------------------------------------------------------------------------------------
// class registration

static ClassDef* registerClass(GDExtensionClassLibraryPtr library, GDExtensionConstStringNamePtr name, GDExtensionConstStringNamePtr parent,
                               const ClassFuncs& f, bool isVirtual, bool isAbstract, bool isExposed, bool isRuntime) {
    Host& h = host();
    ensureEngineClasses();
    const std::string n = nameOfSlot(name), p = nameOfSlot(parent);
    if (n.empty()) { hostError("register_extension_class: empty class name"); return nullptr; }
    if (findClass(n)) { hostError("register_extension_class: class '" + n + "' is already registered"); return nullptr; }
    ClassDef* pd = findClass(p);
    if (!pd) { hostError("register_extension_class: parent class '" + p + "' of '" + n + "' is not known to the host (G3_GDEXT.md lists the engine classes)"); return nullptr; }
    auto c = std::make_unique<ClassDef>();
    c->name = n;
    c->parent = p;
    c->parentDef = pd;
    c->lib = L(library);
    c->f = f;
    c->isVirtual = isVirtual;
    c->isAbstract = isAbstract;
    c->isExposed = isExposed;
    c->isRuntime = isRuntime;
    c->refCounted = pd->refCounted;
    if (!f.free && !isVirtual && !isAbstract) hostLog(1, "class '" + n + "' registered without free_instance_func");
    ClassDef* raw = c.get();
    h.classes[n] = std::move(c);
    h.classOrder.push_back(n);
    if (L(library)) L(library)->classes.push_back(n);
    return raw;
}

static void if_register1(GDExtensionClassLibraryPtr lib, GDExtensionConstStringNamePtr name, GDExtensionConstStringNamePtr parent, const GDExtensionClassCreationInfo* i) {
    ClassFuncs f;
    f.set = i->set_func; f.get = i->get_func; f.getPropertyList = i->get_property_list_func; f.freePropertyList = i->free_property_list_func;
    f.notification1 = i->notification_func; f.toString = i->to_string_func; f.reference = i->reference_func; f.unreference = i->unreference_func;
    f.create = i->create_instance_func; f.free = i->free_instance_func; f.getVirtual = i->get_virtual_func; f.getRid = i->get_rid_func;
    f.classUserdata = i->class_userdata;
    registerClass(lib, name, parent, f, i->is_virtual, i->is_abstract, true, false);
}
static void if_register2(GDExtensionClassLibraryPtr lib, GDExtensionConstStringNamePtr name, GDExtensionConstStringNamePtr parent, const GDExtensionClassCreationInfo2* i) {
    ClassFuncs f;
    f.set = i->set_func; f.get = i->get_func; f.getPropertyList = i->get_property_list_func; f.freePropertyList = i->free_property_list_func;
    f.notification = i->notification_func; f.toString = i->to_string_func; f.reference = i->reference_func; f.unreference = i->unreference_func;
    f.create = i->create_instance_func; f.free = i->free_instance_func; f.recreate = i->recreate_instance_func; f.getVirtual = i->get_virtual_func;
    f.getVirtualData = i->get_virtual_call_data_func; f.callVirtualWithData = i->call_virtual_with_data_func; f.getRid = i->get_rid_func;
    f.classUserdata = i->class_userdata;
    registerClass(lib, name, parent, f, i->is_virtual, i->is_abstract, i->is_exposed, false);
}
static void if_register3(GDExtensionClassLibraryPtr lib, GDExtensionConstStringNamePtr name, GDExtensionConstStringNamePtr parent, const GDExtensionClassCreationInfo3* i) {
    ClassFuncs f;
    f.set = i->set_func; f.get = i->get_func; f.getPropertyList = i->get_property_list_func; f.freePropertyList2 = i->free_property_list_func;
    f.notification = i->notification_func; f.toString = i->to_string_func; f.reference = i->reference_func; f.unreference = i->unreference_func;
    f.create = i->create_instance_func; f.free = i->free_instance_func; f.recreate = i->recreate_instance_func; f.getVirtual = i->get_virtual_func;
    f.getVirtualData = i->get_virtual_call_data_func; f.callVirtualWithData = i->call_virtual_with_data_func; f.getRid = i->get_rid_func;
    f.classUserdata = i->class_userdata;
    registerClass(lib, name, parent, f, i->is_virtual, i->is_abstract, i->is_exposed, i->is_runtime);
}
static void if_register5(GDExtensionClassLibraryPtr lib, GDExtensionConstStringNamePtr name, GDExtensionConstStringNamePtr parent, const GDExtensionClassCreationInfo5* i) {
    ClassFuncs f;
    f.set = i->set_func; f.get = i->get_func; f.getPropertyList = i->get_property_list_func; f.freePropertyList2 = i->free_property_list_func;
    f.notification = i->notification_func; f.toString = i->to_string_func; f.reference = i->reference_func; f.unreference = i->unreference_func;
    f.create2 = i->create_instance_func; f.free = i->free_instance_func; f.recreate = i->recreate_instance_func; f.getVirtual2 = i->get_virtual_func;
    f.getVirtualData2 = i->get_virtual_call_data_func; f.callVirtualWithData = i->call_virtual_with_data_func; f.classUserdata = i->class_userdata;
    registerClass(lib, name, parent, f, i->is_virtual, i->is_abstract, i->is_exposed, i->is_runtime);
}

static void if_register_method(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, const GDExtensionClassMethodInfo* mi) {
    ClassDef* c = findClass(nameOfSlot(cls));
    if (!c || c->engine) { hostError("register_extension_class_method: unknown extension class '" + nameOfSlot(cls) + "'"); return; }
    auto m = std::make_unique<MethodDef>();
    m->name = nameOfSlot(mi->name);
    m->flags = mi->method_flags;
    m->vararg = (mi->method_flags & GDEXTENSION_METHOD_FLAG_VARARG) != 0;
    m->isConst = (mi->method_flags & GDEXTENSION_METHOD_FLAG_CONST) != 0;
    m->isStatic = (mi->method_flags & GDEXTENSION_METHOD_FLAG_STATIC) != 0;
    m->isVirtual = (mi->method_flags & GDEXTENSION_METHOD_FLAG_VIRTUAL) != 0;
    m->userdata = mi->method_userdata;
    m->callFn = mi->call_func;
    m->ptrFn = mi->ptrcall_func;
    for (uint32_t i = 0; i < mi->argument_count; ++i) m->args.push_back(argFromPropertyInfo(mi->arguments_info[i]));
    m->hasRet = mi->has_return_value != 0;
    if (m->hasRet && mi->return_value_info) m->ret = argFromPropertyInfo(*mi->return_value_info);
    for (uint32_t i = 0; i < mi->default_argument_count; ++i) {
        V d;
        varAssign(&d.v, static_cast<const Var*>(mi->default_arguments[i]));
        m->defaults.push_back(std::move(d));
    }
    c->addMethod(std::move(m));
}

static void if_register_virtual_method(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, const GDExtensionClassVirtualMethodInfo* mi) {
    if (ClassDef* c = findClass(nameOfSlot(cls))) c->virtualNames.push_back(nameOfSlot(mi->name));
}

static void if_register_integer_constant(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, GDExtensionConstStringNamePtr en,
                                         GDExtensionConstStringNamePtr name, GDExtensionInt value, GDExtensionBool bitfield) {
    ClassDef* c = findClass(nameOfSlot(cls));
    if (!c) { hostError("register_extension_class_integer_constant: unknown class '" + nameOfSlot(cls) + "'"); return; }
    c->consts.push_back({nameOfSlot(en), nameOfSlot(name), value, bitfield != 0});
}

static void addProperty(GDExtensionConstStringNamePtr cls, const GDExtensionPropertyInfo* info, GDExtensionConstStringNamePtr setter,
                        GDExtensionConstStringNamePtr getter, int64_t index) {
    ClassDef* c = findClass(nameOfSlot(cls));
    if (!c) { hostError("register_extension_class_property: unknown class '" + nameOfSlot(cls) + "'"); return; }
    PropDef p;
    p.info = argFromPropertyInfo(*info);
    p.info.type = info->type;                    // a property keeps its declared type (NIL stays NIL)
    p.setter = nameOfSlot(setter);
    p.getter = nameOfSlot(getter);
    p.index = index;
    p.group = c->currentGroup;
    c->props.push_back(std::move(p));
}
static void if_register_property(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, const GDExtensionPropertyInfo* info,
                                 GDExtensionConstStringNamePtr setter, GDExtensionConstStringNamePtr getter) { addProperty(cls, info, setter, getter, -1); }
static void if_register_property_indexed(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, const GDExtensionPropertyInfo* info,
                                         GDExtensionConstStringNamePtr setter, GDExtensionConstStringNamePtr getter, GDExtensionInt index) { addProperty(cls, info, setter, getter, index); }
static void if_register_property_group(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, GDExtensionConstStringPtr group, GDExtensionConstStringPtr) {
    if (ClassDef* c = findClass(nameOfSlot(cls))) c->currentGroup = stringOfSlot(group);
}
static void if_register_property_subgroup(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, GDExtensionConstStringPtr sub, GDExtensionConstStringPtr) {
    if (ClassDef* c = findClass(nameOfSlot(cls))) c->currentGroup = c->currentGroup + "/" + stringOfSlot(sub);
}
static void if_register_signal(GDExtensionClassLibraryPtr, GDExtensionConstStringNamePtr cls, GDExtensionConstStringNamePtr name,
                               const GDExtensionPropertyInfo* args, GDExtensionInt argc) {
    ClassDef* c = findClass(nameOfSlot(cls));
    if (!c) { hostError("register_extension_class_signal: unknown class '" + nameOfSlot(cls) + "'"); return; }
    SignalDef s;
    s.name = nameOfSlot(name);
    for (GDExtensionInt i = 0; i < argc; ++i) s.args.push_back(argFromPropertyInfo(args[i]));
    c->signals.push_back(std::move(s));
}
static void if_unregister(GDExtensionClassLibraryPtr lib, GDExtensionConstStringNamePtr cls) { unregisterClass(L(lib), nameOfSlot(cls)); }

static void if_get_library_path(GDExtensionClassLibraryPtr lib, GDExtensionUninitializedStringPtr out) { storeString(out, L(lib) ? L(lib)->gdextPath : std::string()); }
static void if_editor_add_plugin(GDExtensionConstStringNamePtr) {}
static void if_editor_remove_plugin(GDExtensionConstStringNamePtr) {}
static void if_editor_help_load_xml(const char*) {}
static void if_editor_help_load_xml_len(const char*, GDExtensionInt) {}
static void if_editor_register_get_classes_used_callback(GDExtensionClassLibraryPtr, GDExtensionEditorGetClassesUsedCallback) {}
static void if_register_main_loop_callbacks(GDExtensionClassLibraryPtr lib, const GDExtensionMainLoopCallbacks* cb) {
    if (L(lib) && cb) { L(lib)->mainLoop = *cb; L(lib)->hasMainLoop = true; }
}

// ---------------------------------------------------------------------------------------------------
// method binds

static GDExtensionObjectPtr if_classdb_construct_object(GDExtensionConstStringNamePtr name) { return instantiate(nameOfSlot(name), true); }

static void* if_classdb_get_class_tag(GDExtensionConstStringNamePtr name) {
    ensureEngineClasses();
    return findClass(nameOfSlot(name));
}

static GDExtensionMethodBindPtr if_classdb_get_method_bind(GDExtensionConstStringNamePtr cls, GDExtensionConstStringNamePtr method, GDExtensionInt hash) {
    Host& h = host();
    ensureEngineClasses();
    const std::string cn = nameOfSlot(cls), mn = nameOfSlot(method);
    const std::string key = cn + "::" + mn + "::" + std::to_string(uint32_t(hash));
    if (auto it = h.binds.find(key); it != h.binds.end()) return it->second.get();
    ClassDef* c = findClass(cn);
    if (!c) {
        logOnce("bind-class:" + cn, "classdb_get_method_bind: class '" + cn + "' is not known to the host");
        return nullptr;
    }
    MethodDef* m = nullptr;
    if (auto mi = c->methodByName.find(mn); mi != c->methodByName.end()) m = mi->second;
    else m = findMethod(c, mn);
    if (!m) {
        const std::string what = cn + "::" + mn;
        if (std::find(h.unsupportedMethods.begin(), h.unsupportedMethods.end(), what) == h.unsupportedMethods.end()) h.unsupportedMethods.push_back(what);
        logOnce("bind:" + what, "engine method " + what + " is not implemented by the Aver host (G3_GDEXT.md)");
        return nullptr;
    }
    if (c->engine && h.api.loaded) {
        // hash and hash_compatibility from extension_api.json, looked up on the declaring class (or an ancestor)
        for (std::string p = cn; !p.empty();) {
            auto ci = h.api.classes.find(p);
            if (ci != h.api.classes.end()) {
                auto mi = ci->second.find(mn);
                if (mi != ci->second.end()) {
                    const uint32_t want = uint32_t(hash);
                    bool ok = mi->second.hash == want;
                    for (uint32_t cmp : mi->second.compat) ok = ok || cmp == want;
                    if (!ok) {
                        hostError("classdb_get_method_bind: " + cn + "::" + mn + " hash " + std::to_string(want) + " does not match this host's API (" +
                                  std::to_string(mi->second.hash) + "); the extension was built against an incompatible Godot version");
                        return nullptr;
                    }
                    break;
                }
            }
            auto pi = h.api.parent.find(p);
            p = pi == h.api.parent.end() ? std::string() : pi->second;
        }
    }
    auto mb = std::make_unique<MethodBind>();
    mb->cls = c;
    mb->method = m;
    mb->name = mn;
    MethodBind* raw = mb.get();
    h.binds[key] = std::move(mb);
    return raw;
}

static void if_object_method_bind_call(GDExtensionMethodBindPtr bind, GDExtensionObjectPtr instance, const GDExtensionConstVariantPtr* args,
                                       GDExtensionInt argc, GDExtensionUninitializedVariantPtr ret, GDExtensionCallError* err) {
    varNil(static_cast<Var*>(ret));
    const MethodBind* mb = static_cast<const MethodBind*>(bind);
    if (!mb) { err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD; err->argument = 0; err->expected = 0; return; }
    HostObject* o = O(instance);
    if (o && o->freed) { err->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL; err->argument = 0; err->expected = 0; return; }
    callMethod(o, mb->method, reinterpret_cast<const Var* const*>(args), int(argc), static_cast<Var*>(ret), err);
}

static void if_object_method_bind_ptrcall(GDExtensionMethodBindPtr bind, GDExtensionObjectPtr instance, const GDExtensionConstTypePtr* args, GDExtensionTypePtr ret) {
    const MethodBind* mb = static_cast<const MethodBind*>(bind);
    HostObject* o = O(instance);
    if (!mb || (o && o->freed)) return;
    callPtr(o, mb->method, args, ret);
}

// ---------------------------------------------------------------------------------------------------
// objects

static void if_object_destroy(GDExtensionObjectPtr o) { destroyObject(O(o)); }

static GDExtensionObjectPtr if_global_get_singleton(GDExtensionConstStringNamePtr name) {
    ensureEngineClasses();
    auto it = host().singletons.find(nameOfSlot(name));
    return it == host().singletons.end() ? nullptr : it->second;
}

static void* if_object_get_instance_binding(GDExtensionObjectPtr o, void* token, const GDExtensionInstanceBindingCallbacks* cb) {
    HostObject* obj = O(o);
    if (!obj) return nullptr;
    for (Binding& b : obj->bindings) if (b.token == token) return b.binding;
    void* binding = cb && cb->create_callback ? cb->create_callback(token, o) : nullptr;
    obj->bindings.push_back({token, binding, cb ? *cb : GDExtensionInstanceBindingCallbacks{}});
    return binding;
}

static void if_object_set_instance_binding(GDExtensionObjectPtr o, void* token, void* binding, const GDExtensionInstanceBindingCallbacks* cb) {
    HostObject* obj = O(o);
    if (!obj) return;
    for (Binding& b : obj->bindings)
        if (b.token == token) { b.binding = binding; if (cb) b.cb = *cb; return; }
    obj->bindings.push_back({token, binding, cb ? *cb : GDExtensionInstanceBindingCallbacks{}});
}

static void if_object_free_instance_binding(GDExtensionObjectPtr o, void* token) {
    HostObject* obj = O(o);
    if (!obj) return;
    for (size_t i = 0; i < obj->bindings.size(); ++i)
        if (obj->bindings[i].token == token) {
            Binding b = obj->bindings[i];
            obj->bindings.erase(obj->bindings.begin() + long(i));
            if (b.cb.free_callback) b.cb.free_callback(b.token, o, b.binding);
            return;
        }
}

static void if_object_set_instance(GDExtensionObjectPtr o, GDExtensionConstStringNamePtr name, GDExtensionClassInstancePtr instance) {
    HostObject* obj = O(o);
    ClassDef* c = findClass(nameOfSlot(name));
    if (!obj || !c || c->engine) { hostError("object_set_instance: unknown extension class '" + nameOfSlot(name) + "'"); return; }
    obj->def = c;
    obj->cls = c->name;
    obj->instance = instance;
}

static GDExtensionBool if_object_get_class_name(GDExtensionConstObjectPtr o, GDExtensionClassLibraryPtr, GDExtensionUninitializedStringNamePtr out) {
    HostObject* obj = O(o);
    if (!obj) return 0;
    storeName(out, obj->cls);
    return 1;
}

static GDExtensionObjectPtr if_object_cast_to(GDExtensionConstObjectPtr o, void* tag) {
    HostObject* obj = O(o);
    const ClassDef* want = static_cast<const ClassDef*>(tag);
    return obj && want && !obj->freed && classIsA(obj->def, want->name) ? obj : nullptr;
}
static GDExtensionObjectPtr if_object_get_instance_from_id(GDObjectInstanceID id) { return objectById(id); }
static GDObjectInstanceID if_object_get_instance_id(GDExtensionConstObjectPtr o) { return O(o) ? O(o)->id : 0; }
static GDExtensionBool if_object_has_script_method(GDExtensionConstObjectPtr, GDExtensionConstStringNamePtr) { return 0; }
static void if_object_call_script_method(GDExtensionObjectPtr, GDExtensionConstStringNamePtr, const GDExtensionConstVariantPtr*, GDExtensionInt,
                                         GDExtensionUninitializedVariantPtr ret, GDExtensionCallError* err) {
    varNil(static_cast<Var*>(ret));
    err->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD; err->argument = 0; err->expected = 0;
}

static GDExtensionObjectPtr if_ref_get_object(GDExtensionConstRefPtr ref) { return ref ? ld<HostObject*>(ref) : nullptr; }
static void if_ref_set_object(GDExtensionRefPtr ref, GDExtensionObjectPtr object) {
    if (!ref) return;
    HostObject* old = ld<HostObject*>(ref);
    HostObject* nw = O(object);
    if (old == nw) return;
    st<HostObject*>(ref, nw);
    if (nw) ++nw->refcount;
    if (old && --old->refcount <= 0 && !old->freed) destroyObject(old);
}

// ---------------------------------------------------------------------------------------------------
// callables

static void makeCallable(GDExtensionUninitializedTypePtr out, const GDExtensionCallableCustomInfo2& info) {
    std::memset(out, 0, 16);
    V c = makeCustomCallable(info);
    st<CallableData*>(out, ld<CallableData*>(c.v.d));
    ++ld<CallableData*>(out)->rc;                 // the caller's Callable holds its own reference; `c` drops one on scope exit
}
static void if_callable_custom_create2(GDExtensionUninitializedTypePtr out, GDExtensionCallableCustomInfo2* info) { makeCallable(out, *info); }
static void if_callable_custom_create(GDExtensionUninitializedTypePtr out, GDExtensionCallableCustomInfo* info) {
    GDExtensionCallableCustomInfo2 i2{};
    i2.callable_userdata = info->callable_userdata; i2.token = info->token; i2.object_id = info->object_id; i2.call_func = info->call_func;
    i2.is_valid_func = info->is_valid_func; i2.free_func = info->free_func; i2.hash_func = info->hash_func; i2.equal_func = info->equal_func;
    i2.less_than_func = info->less_than_func; i2.to_string_func = info->to_string_func;
    makeCallable(out, i2);
}
static void* if_callable_custom_get_userdata(GDExtensionConstTypePtr callable, void* token) {
    auto* c = ld<CallableData*>(callable);
    return c && c->custom && c->info.token == token ? c->info.callable_userdata : nullptr;
}

// ---------------------------------------------------------------------------------------------------

#define E(name, fn) {name, reinterpret_cast<GDExtensionInterfaceFunctionPtr>(&fn)}
GDExtensionInterfaceFunctionPtr classDbFunction(const char* name) {
    static const std::unordered_map<std::string, GDExtensionInterfaceFunctionPtr> t = {
        E("classdb_construct_object", if_classdb_construct_object), E("classdb_construct_object2", if_classdb_construct_object),
        E("classdb_get_method_bind", if_classdb_get_method_bind), E("classdb_get_class_tag", if_classdb_get_class_tag),
        E("classdb_register_extension_class", if_register1), E("classdb_register_extension_class2", if_register2),
        E("classdb_register_extension_class3", if_register3), E("classdb_register_extension_class4", if_register5),
        E("classdb_register_extension_class5", if_register5),
        E("classdb_register_extension_class_method", if_register_method),
        E("classdb_register_extension_class_virtual_method", if_register_virtual_method),
        E("classdb_register_extension_class_integer_constant", if_register_integer_constant),
        E("classdb_register_extension_class_property", if_register_property),
        E("classdb_register_extension_class_property_indexed", if_register_property_indexed),
        E("classdb_register_extension_class_property_group", if_register_property_group),
        E("classdb_register_extension_class_property_subgroup", if_register_property_subgroup),
        E("classdb_register_extension_class_signal", if_register_signal),
        E("classdb_unregister_extension_class", if_unregister),
        E("get_library_path", if_get_library_path),
        E("editor_add_plugin", if_editor_add_plugin), E("editor_remove_plugin", if_editor_remove_plugin),
        E("editor_help_load_xml_from_utf8_chars", if_editor_help_load_xml), E("editor_help_load_xml_from_utf8_chars_and_len", if_editor_help_load_xml_len),
        E("editor_register_get_classes_used_callback", if_editor_register_get_classes_used_callback),
        E("register_main_loop_callbacks", if_register_main_loop_callbacks),
        E("object_method_bind_call", if_object_method_bind_call), E("object_method_bind_ptrcall", if_object_method_bind_ptrcall),
        E("object_destroy", if_object_destroy), E("global_get_singleton", if_global_get_singleton),
        E("object_get_instance_binding", if_object_get_instance_binding), E("object_set_instance_binding", if_object_set_instance_binding),
        E("object_free_instance_binding", if_object_free_instance_binding), E("object_set_instance", if_object_set_instance),
        E("object_get_class_name", if_object_get_class_name), E("object_cast_to", if_object_cast_to),
        E("object_get_instance_from_id", if_object_get_instance_from_id), E("object_get_instance_id", if_object_get_instance_id),
        E("object_has_script_method", if_object_has_script_method), E("object_call_script_method", if_object_call_script_method),
        E("ref_get_object", if_ref_get_object), E("ref_set_object", if_ref_set_object),
        E("callable_custom_create", if_callable_custom_create), E("callable_custom_create2", if_callable_custom_create2),
        E("callable_custom_get_userdata", if_callable_custom_get_userdata),
    };
    auto it = t.find(name);
    return it == t.end() ? nullptr : it->second;
}
#undef E

} // namespace aver::gdext
