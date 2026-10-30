/* FabricTestExt -- a GDExtension written in C against gdextension_interface.h only (no godot-cpp), used by GdextHostTest to
 * exercise the Aver GDExtension host the way a godot-cpp plugin does: entry symbol, initialization levels, class and method
 * registration (call and ptrcall forms), properties, a signal, an integer constant, virtual functions, and calls back into the
 * host (method binds with the API-dump hashes, builtin constructors and methods, utility functions).
 *
 * Classes:  FabricTestNode extends Node  (add, greet, speed, pinged, _process/_ready/... counters, self_test, spawn_child)
 *           FabricTestRef extends RefCounted (value)
 *           FabricTestAbstract extends Node (abstract, cannot be created)
 *           FabricTestLegacy extends Node (registered through classdb_register_extension_class2)
 */
#include "gdextension_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define FT_EXPORT __declspec(dllexport)
#else
#define FT_EXPORT __attribute__((visibility("default")))
#endif

/* Opaque builtins: the extension only ever hands these to the host. */
typedef struct { union { unsigned char b[8]; double align; } u; } SN;    /* StringName */
typedef struct { union { unsigned char b[8]; double align; } u; } Str;   /* String */
typedef struct { union { unsigned char b[24]; double align; } u; } Var;  /* Variant */
typedef struct { union { unsigned char b[64]; double align; } u; } Blob; /* any other builtin up to 64 bytes */

/* ---- the interface ---- */
static GDExtensionInterfaceGetProcAddress g_get_proc;
static GDExtensionClassLibraryPtr g_lib;

#define DECL(type, name) static type p_##name
DECL(GDExtensionInterfaceGetGodotVersion, get_godot_version);
DECL(GDExtensionInterfaceStringNameNewWithUtf8Chars, string_name_new_with_utf8_chars);
DECL(GDExtensionInterfaceStringNewWithUtf8Chars, string_new_with_utf8_chars);
DECL(GDExtensionInterfaceStringNewWithUtf32Chars, string_new_with_utf32_chars);
DECL(GDExtensionInterfaceStringToUtf8Chars, string_to_utf8_chars);
DECL(GDExtensionInterfaceStringToUtf32Chars, string_to_utf32_chars);
DECL(GDExtensionInterfaceGetLibraryPath, get_library_path);
DECL(GDExtensionInterfaceClassdbConstructObject2, classdb_construct_object2);
DECL(GDExtensionInterfaceObjectSetInstance, object_set_instance);
DECL(GDExtensionInterfaceClassdbRegisterExtensionClass5, classdb_register_extension_class5);
DECL(GDExtensionInterfaceClassdbRegisterExtensionClass2, classdb_register_extension_class2);
DECL(GDExtensionInterfaceClassdbRegisterExtensionClassMethod, classdb_register_extension_class_method);
DECL(GDExtensionInterfaceClassdbRegisterExtensionClassIntegerConstant, classdb_register_extension_class_integer_constant);
DECL(GDExtensionInterfaceClassdbRegisterExtensionClassProperty, classdb_register_extension_class_property);
DECL(GDExtensionInterfaceClassdbRegisterExtensionClassSignal, classdb_register_extension_class_signal);
DECL(GDExtensionInterfaceClassdbUnregisterExtensionClass, classdb_unregister_extension_class);
DECL(GDExtensionInterfaceClassdbGetMethodBind, classdb_get_method_bind);
DECL(GDExtensionInterfaceObjectMethodBindCall, object_method_bind_call);
DECL(GDExtensionInterfaceObjectMethodBindPtrcall, object_method_bind_ptrcall);
DECL(GDExtensionInterfaceGetVariantFromTypeConstructor, get_variant_from_type_constructor);
DECL(GDExtensionInterfaceGetVariantToTypeConstructor, get_variant_to_type_constructor);
DECL(GDExtensionInterfaceVariantGetPtrConstructor, variant_get_ptr_constructor);
DECL(GDExtensionInterfaceVariantGetPtrDestructor, variant_get_ptr_destructor);
DECL(GDExtensionInterfaceVariantGetPtrBuiltinMethod, variant_get_ptr_builtin_method);
DECL(GDExtensionInterfaceVariantGetPtrUtilityFunction, variant_get_ptr_utility_function);
DECL(GDExtensionInterfaceVariantGetPtrOperatorEvaluator, variant_get_ptr_operator_evaluator);
DECL(GDExtensionInterfaceVariantGetPtrSetter, variant_get_ptr_setter);
DECL(GDExtensionInterfaceVariantGetPtrGetter, variant_get_ptr_getter);
DECL(GDExtensionInterfaceVariantGetPtrIndexedGetter, variant_get_ptr_indexed_getter);
DECL(GDExtensionInterfaceVariantNewNil, variant_new_nil);
DECL(GDExtensionInterfaceVariantDestroy, variant_destroy);
DECL(GDExtensionInterfaceVariantStringify, variant_stringify);
DECL(GDExtensionInterfaceVariantEvaluate, variant_evaluate);
DECL(GDExtensionInterfaceVariantHash, variant_hash);
DECL(GDExtensionInterfaceVariantGetType, variant_get_type);
DECL(GDExtensionInterfaceMemAlloc, mem_alloc);
DECL(GDExtensionInterfaceMemFree, mem_free);
DECL(GDExtensionInterfacePrintWarning, print_warning);

static GDExtensionVariantFromTypeConstructorFunc from_int, from_float, from_string, from_name, from_bool, from_vector3;
static GDExtensionTypeFromVariantConstructorFunc to_int, to_float, to_string, to_vector3;
static GDExtensionPtrDestructor dtor_string, dtor_name, dtor_array;
static GDExtensionPtrConstructor ctor_string_default, ctor_name_from_string, ctor_array_default, ctor_vector3_xyz;

static void make_name(SN* out, const char* s) { p_string_name_new_with_utf8_chars(out, s); }
static void make_string(Str* out, const char* s) { p_string_new_with_utf8_chars(out, s); }
static int name_is(GDExtensionConstStringNamePtr n, const char* s) {
    SN tmp;
    make_name(&tmp, s);
    return memcmp(n, &tmp, 8) == 0; /* equal names share their data pointer */
}

/* ---- what the test reads back ---- */
static int32_t g_init_levels[8], g_init_count;
static int32_t g_deinit_levels[8], g_deinit_count;
static int32_t g_freed, g_created, g_version_major, g_version_minor, g_lib_path_len;

FT_EXPORT void fabric_test_state(int32_t* out) {
    int i;
    memset(out, 0, 24 * sizeof(int32_t));
    out[0] = g_init_count;
    for (i = 0; i < g_init_count && i < 4; ++i) out[1 + i] = g_init_levels[i];
    out[5] = g_deinit_count;
    for (i = 0; i < g_deinit_count && i < 4; ++i) out[6 + i] = g_deinit_levels[i];
    out[10] = g_freed;
    out[11] = g_created;
    out[12] = g_version_major;
    out[13] = g_version_minor;
    out[14] = g_lib_path_len;
}

/* ---- instances ---- */
typedef struct {
    GDExtensionObjectPtr owner;
    double speed;
    int process_count, physics_count, ready_count, enter_count, exit_count, notif_count, last_notif;
    double last_delta;
} Inst;

static Inst* inst_of(GDExtensionClassInstancePtr p) { return (Inst*)p; }

static GDExtensionMethodBindPtr bind_of(const char* cls, const char* method, int64_t hash) {
    SN c, m;
    make_name(&c, cls);
    make_name(&m, method);
    return p_classdb_get_method_bind(&c, &m, hash);
}

/* ---- methods: add(int, int) -> int ---- */
static void add_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                     GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    int64_t a = 0, b = 0, r;
    (void)ud; (void)inst;
    if (argc < 2) { err->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS; err->expected = 2; return; }
    to_int(&a, (GDExtensionVariantPtr)args[0]);
    to_int(&b, (GDExtensionVariantPtr)args[1]);
    r = a + b;
    from_int(r_ret, &r);
    err->error = GDEXTENSION_CALL_OK;
}
static void add_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)inst;
    *(int64_t*)r_ret = *(const int64_t*)args[0] + *(const int64_t*)args[1];
}

/* greet(String) -> String */
static void greet_into(const Str* name, Str* out_uninit) {
    GDExtensionInt n = p_string_to_utf8_chars(name, NULL, 0);
    char* buf = (char*)malloc((size_t)n + 1);
    char* msg = (char*)malloc((size_t)n + 16);
    p_string_to_utf8_chars(name, buf, n);
    buf[n] = 0;
    snprintf(msg, (size_t)n + 16, "Hello, %s!", buf);
    make_string(out_uninit, msg);
    free(buf);
    free(msg);
}
static void greet_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                       GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    Str in, out;
    (void)ud; (void)inst;
    if (argc < 1) { err->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS; err->expected = 1; return; }
    to_string(&in, (GDExtensionVariantPtr)args[0]);
    greet_into(&in, &out);
    from_string(r_ret, &out);
    dtor_string(&in);
    dtor_string(&out);
    err->error = GDEXTENSION_CALL_OK;
}
static void greet_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)inst;
    dtor_string(r_ret); /* the caller hands over an initialised String */
    greet_into((const Str*)args[0], (Str*)r_ret);
}

/* speed */
static void get_speed_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                           GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    double d = inst_of(inst)->speed;
    (void)ud; (void)args; (void)argc;
    from_float(r_ret, &d);
    err->error = GDEXTENSION_CALL_OK;
}
static void get_speed_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)args;
    *(double*)r_ret = inst_of(inst)->speed;
}
static void set_speed_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                           GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    double d = 0;
    (void)ud; (void)r_ret;
    if (argc < 1) { err->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS; err->expected = 1; return; }
    to_float(&d, (GDExtensionVariantPtr)args[0]);
    inst_of(inst)->speed = d;
    err->error = GDEXTENSION_CALL_OK;
}
static void set_speed_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)r_ret;
    inst_of(inst)->speed = *(const double*)args[0];
}

/* ping(int): emits pinged(n) through Object.emit_signal, the way a godot-cpp class does */
static void emit_pinged(Inst* i, int64_t n) {
    static GDExtensionMethodBindPtr bind;
    Var a0, a1, ret;
    SN sig;
    const GDExtensionConstVariantPtr args[2] = {&a0, &a1};
    GDExtensionCallError err;
    if (!bind) bind = bind_of("Object", "emit_signal", 4047867050LL);
    make_name(&sig, "pinged");
    from_name(&a0, &sig);
    from_int(&a1, &n);
    p_variant_new_nil(&ret);
    p_object_method_bind_call(bind, i->owner, args, 2, &ret, &err);
    p_variant_destroy(&a0);
    p_variant_destroy(&a1);
    p_variant_destroy(&ret);
}
static void ping_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                      GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    int64_t n = 0;
    (void)ud; (void)r_ret;
    if (argc >= 1) to_int(&n, (GDExtensionVariantPtr)args[0]);
    emit_pinged(inst_of(inst), n);
    err->error = GDEXTENSION_CALL_OK;
}
static void ping_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)r_ret;
    emit_pinged(inst_of(inst), *(const int64_t*)args[0]);
}

/* counters: int get_xxx() */
#define COUNTER(NAME, FIELD) \
    static void NAME##_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc, \
                            GDExtensionVariantPtr r_ret, GDExtensionCallError* err) { \
        int64_t v = inst_of(inst)->FIELD; \
        (void)ud; (void)args; (void)argc; \
        from_int(r_ret, &v); \
        err->error = GDEXTENSION_CALL_OK; \
    } \
    static void NAME##_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) { \
        (void)ud; (void)args; \
        *(int64_t*)r_ret = inst_of(inst)->FIELD; \
    }
COUNTER(get_process_count, process_count)
COUNTER(get_physics_count, physics_count)
COUNTER(get_ready_count, ready_count)
COUNTER(get_enter_count, enter_count)
COUNTER(get_exit_count, exit_count)
COUNTER(get_notification_count, notif_count)
COUNTER(get_last_notification, last_notif)

/* owner_class() -> String: calls Object.get_class on itself through a ptrcall method bind */
static void owner_class_into(Inst* i, Str* out_initialised) {
    static GDExtensionMethodBindPtr bind;
    if (!bind) bind = bind_of("Object", "get_class", 201670096LL);
    p_object_method_bind_ptrcall(bind, i->owner, NULL, out_initialised);
}
static void owner_class_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                             GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    Str s;
    (void)ud; (void)args; (void)argc;
    ctor_string_default(&s, NULL);
    owner_class_into(inst_of(inst), &s);
    from_string(r_ret, &s);
    dtor_string(&s);
    err->error = GDEXTENSION_CALL_OK;
}
static void owner_class_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)args;
    owner_class_into(inst_of(inst), (Str*)r_ret);
}

/* spawn_child(String name) -> int: makes a Node, names it, add_child()s it, returns get_child_count() */
static int64_t spawn_child(Inst* i, const Str* name) {
    GDExtensionMethodBindPtr set_name = bind_of("Node", "set_name", 3304788590LL);
    GDExtensionMethodBindPtr add_child = bind_of("Node", "add_child", 3863233950LL);
    GDExtensionMethodBindPtr count = bind_of("Node", "get_child_count", 894402480LL);
    SN cls, nm;
    const void* name_args[1];
    const void* add_args[3];
    const void* count_args[1];
    GDExtensionObjectPtr child;
    uint8_t force = 0, internal_only = 0;
    int64_t internal_mode = 0, n = -1;
    const void* sa[1];
    make_name(&cls, "Node");
    child = p_classdb_construct_object2(&cls);
    sa[0] = name;
    ctor_name_from_string(&nm, sa);
    name_args[0] = &nm;
    p_object_method_bind_ptrcall(set_name, child, name_args, NULL);
    add_args[0] = &child; /* an Object argument is passed as the address of the object pointer */
    add_args[1] = &force;
    add_args[2] = &internal_mode;
    p_object_method_bind_ptrcall(add_child, i->owner, add_args, NULL);
    count_args[0] = &internal_only;
    p_object_method_bind_ptrcall(count, i->owner, count_args, &n);
    return n;
}
static void spawn_child_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                             GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    Str in;
    int64_t n;
    (void)ud;
    if (argc < 1) { err->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS; err->expected = 1; return; }
    to_string(&in, (GDExtensionVariantPtr)args[0]);
    n = spawn_child(inst_of(inst), &in);
    dtor_string(&in);
    from_int(r_ret, &n);
    err->error = GDEXTENSION_CALL_OK;
}
static void spawn_child_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud;
    *(int64_t*)r_ret = spawn_child(inst_of(inst), (const Str*)args[0]);
}

/* self_test() -> int: a bit for each thing the host answered correctly when called the way godot-cpp calls it */
static int64_t self_test(void) {
    int64_t bits = 0;
    /* 1: String.length() through a builtin method pointer, with the API-dump hash */
    {
        SN mname;
        GDExtensionPtrBuiltInMethod length;
        Str s;
        int64_t len = -1;
        make_name(&mname, "length");
        length = p_variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_STRING, &mname, 3173160232LL);
        make_string(&s, "caf\xC3\xA9");
        if (length) length(&s, NULL, &len, 0);
        if (len == 4) bits |= 1;
        dtor_string(&s);
    }
    /* 2: a wrong hash is refused */
    {
        SN mname;
        make_name(&mname, "length");
        if (p_variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_STRING, &mname, 12345) == NULL) bits |= 2;
    }
    /* 4: Array.push_back / Array.size */
    {
        SN push_name, size_name;
        GDExtensionPtrBuiltInMethod push, size;
        Blob arr;
        int64_t n = -1, one = 1, two = 2;
        Var v1, v2;
        const void* a1[1];
        const void* a2[1];
        make_name(&push_name, "push_back");
        make_name(&size_name, "size");
        push = p_variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_ARRAY, &push_name, 3316032543LL);
        size = p_variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_ARRAY, &size_name, 3173160232LL);
        ctor_array_default(&arr, NULL);
        from_int(&v1, &one);
        from_int(&v2, &two);
        a1[0] = &v1;
        a2[0] = &v2;
        if (push && size) {
            push(&arr, a1, NULL, 1);
            push(&arr, a2, NULL, 1);
            size(&arr, NULL, &n, 0);
        }
        if (n == 2) bits |= 4;
        p_variant_destroy(&v1);
        p_variant_destroy(&v2);
        dtor_array(&arr);
    }
    /* 8: Vector3 construct + length + the x member */
    {
        SN mname, xname;
        GDExtensionPtrBuiltInMethod length;
        GDExtensionPtrGetter getx;
        float v[3];
        double x = 0, len = 0;
        double fx = 3, fy = 4, fz = 0;
        const void* args[3];
        make_name(&mname, "length");
        make_name(&xname, "x");
        length = p_variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_VECTOR3, &mname, 466405837LL);
        getx = p_variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_VECTOR3, &xname);
        args[0] = &fx; args[1] = &fy; args[2] = &fz;
        ctor_vector3_xyz(v, args);
        if (length) length(v, NULL, &len, 0);
        if (getx) getx(v, &x);
        if (len > 4.99 && len < 5.01 && x > 2.99 && x < 3.01) bits |= 8;
    }
    /* 16: utility function sin(0) == 0 and maxi(2, 7) == 7 */
    {
        SN fname, mname;
        GDExtensionPtrUtilityFunction fsin, fmaxi;
        double zero = 0, sres = 1;
        int64_t two = 2, seven = 7, m = 0;
        const void* sa[1];
        const void* ma[2];
        make_name(&fname, "sin");
        make_name(&mname, "maxi");
        fsin = p_variant_get_ptr_utility_function(&fname, 2140049587LL);
        fmaxi = p_variant_get_ptr_utility_function(&mname, 3133453818LL);
        sa[0] = &zero;
        ma[0] = &two;
        ma[1] = &seven;
        if (fsin) fsin(&sres, sa, 1);
        if (fmaxi) fmaxi(&m, ma, 2);
        if (sres == 0.0 && m == 7) bits |= 16;
    }
    /* 32: variant_evaluate 2 + 3 and 7 > 3 */
    {
        Var a, b, r;
        int64_t two = 2, three = 3, out = 0;
        GDExtensionBool valid = 0;
        from_int(&a, &two);
        from_int(&b, &three);
        p_variant_evaluate(GDEXTENSION_VARIANT_OP_ADD, &a, &b, &r, &valid);
        if (valid) { to_int(&out, &r); if (out == 5) bits |= 32; }
        p_variant_destroy(&r);
        p_variant_destroy(&a);
        p_variant_destroy(&b);
    }
    /* 64: variant_stringify of an int */
    {
        Var a;
        Str s;
        int64_t v = 42;
        char buf[16] = {0};
        from_int(&a, &v);
        ctor_string_default(&s, NULL);
        p_variant_stringify(&a, &s);
        p_string_to_utf8_chars(&s, buf, 15);
        if (strncmp(buf, "42", 2) == 0) bits |= 64;
        dtor_string(&s);
        p_variant_destroy(&a);
    }
    /* 128: operator evaluator Vector3 * float */
    {
        GDExtensionPtrOperatorEvaluator mul = p_variant_get_ptr_operator_evaluator(GDEXTENSION_VARIANT_OP_MULTIPLY, GDEXTENSION_VARIANT_TYPE_VECTOR3, GDEXTENSION_VARIANT_TYPE_FLOAT);
        double fx = 1, fy = 2, fz = 3, k = 2;
        const void* args[3];
        float v[3], r[3] = {0, 0, 0};
        args[0] = &fx; args[1] = &fy; args[2] = &fz;
        ctor_vector3_xyz(v, args);
        if (mul) mul(v, &k, r);
        if (r[0] == 2.0f && r[1] == 4.0f && r[2] == 6.0f) bits |= 128;
    }
    return bits;
}
static void self_test_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                           GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    int64_t b = self_test();
    (void)ud; (void)inst; (void)args; (void)argc;
    from_int(r_ret, &b);
    err->error = GDEXTENSION_CALL_OK;
}
static void self_test_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)inst; (void)args;
    *(int64_t*)r_ret = self_test();
}

/* ---- virtuals and notifications ---- */
static void v_ready(GDExtensionClassInstancePtr i, const GDExtensionConstTypePtr* a, GDExtensionTypePtr r) { (void)a; (void)r; inst_of(i)->ready_count++; }
static void v_enter(GDExtensionClassInstancePtr i, const GDExtensionConstTypePtr* a, GDExtensionTypePtr r) { (void)a; (void)r; inst_of(i)->enter_count++; }
static void v_exit(GDExtensionClassInstancePtr i, const GDExtensionConstTypePtr* a, GDExtensionTypePtr r) { (void)a; (void)r; inst_of(i)->exit_count++; }
static void v_process(GDExtensionClassInstancePtr i, const GDExtensionConstTypePtr* a, GDExtensionTypePtr r) {
    (void)r;
    inst_of(i)->process_count++;
    inst_of(i)->last_delta = *(const double*)a[0];
}
static void v_physics(GDExtensionClassInstancePtr i, const GDExtensionConstTypePtr* a, GDExtensionTypePtr r) { (void)a; (void)r; inst_of(i)->physics_count++; }

static GDExtensionClassCallVirtual get_virtual(void* ud, GDExtensionConstStringNamePtr name, uint32_t hash) {
    (void)ud; (void)hash;
    if (name_is(name, "_ready")) return v_ready;
    if (name_is(name, "_enter_tree")) return v_enter;
    if (name_is(name, "_exit_tree")) return v_exit;
    if (name_is(name, "_process")) return v_process;
    if (name_is(name, "_physics_process")) return v_physics;
    return NULL;
}
static void notification(GDExtensionClassInstancePtr i, int32_t what, GDExtensionBool reversed) {
    (void)reversed;
    inst_of(i)->notif_count++;
    inst_of(i)->last_notif = what;
}
static void to_string_fn(GDExtensionClassInstancePtr i, GDExtensionBool* valid, GDExtensionStringPtr out) {
    char buf[64];
    snprintf(buf, sizeof buf, "FabricTestNode(speed=%g)", inst_of(i)->speed);
    make_string((Str*)out, buf);
    *valid = 1;
}

/* ---- class plumbing ---- */
static GDExtensionObjectPtr create_with(const char* parent, const char* cls, void* ud, GDExtensionBool notify);

static GDExtensionObjectPtr create_node(void* ud, GDExtensionBool notify) { return create_with("Node", "FabricTestNode", ud, notify); }

static GDExtensionObjectPtr create_with(const char* parent, const char* cls, void* ud, GDExtensionBool notify) {
    SN p, c;
    GDExtensionObjectPtr obj;
    Inst* inst;
    (void)ud;
    make_name(&p, parent);
    obj = p_classdb_construct_object2(&p);
    inst = (Inst*)calloc(1, sizeof(Inst));
    inst->owner = obj;
    inst->speed = 1.0;
    make_name(&c, cls);
    p_object_set_instance(obj, &c, inst);
    g_created++;
    if (notify) {
        static GDExtensionMethodBindPtr bind;
        int64_t what = 0;
        uint8_t reversed = 0;
        const void* args[2];
        if (!bind) bind = bind_of("Object", "notification", 4023243586LL);
        args[0] = &what;
        args[1] = &reversed;
        p_object_method_bind_ptrcall(bind, obj, args, NULL);
    }
    return obj;
}
static GDExtensionObjectPtr create_legacy(void* ud) { return create_with("Node", "FabricTestLegacy", ud, 0); }
static void free_instance(void* ud, GDExtensionClassInstancePtr inst) { (void)ud; free(inst); g_freed++; }

static void fill_prop(GDExtensionPropertyInfo* p, GDExtensionVariantType type, SN* name, SN* cls, Str* hint) {
    p->type = type;
    p->name = name;
    p->class_name = cls;
    p->hint = 0;
    p->hint_string = hint;
    p->usage = 6;
}

static void reg_method(const char* cls, const char* name, GDExtensionClassMethodCall call, GDExtensionClassMethodPtrCall ptr, int nargs,
                       const GDExtensionVariantType* argtypes, int rettype, uint32_t flags) {
    SN c, n, an[3], empty;
    Str hint;
    GDExtensionPropertyInfo args[3], ret;
    GDExtensionClassMethodArgumentMetadata argmeta[3] = {GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE, GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE, GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE};
    SN rn;
    GDExtensionClassMethodInfo info;
    int i;
    make_name(&c, cls);
    make_name(&n, name);
    make_name(&empty, "");
    make_string(&hint, "");
    make_name(&rn, "");
    memset(&info, 0, sizeof info);
    for (i = 0; i < nargs; ++i) {
        char nm[8];
        snprintf(nm, sizeof nm, "arg%d", i);
        make_name(&an[i], nm);
        fill_prop(&args[i], argtypes[i], &an[i], &empty, &hint);
    }
    if (rettype >= 0) fill_prop(&ret, (GDExtensionVariantType)rettype, &rn, &empty, &hint);
    info.name = &n;
    info.method_userdata = NULL;
    info.call_func = call;
    info.ptrcall_func = ptr;
    info.method_flags = flags;
    info.has_return_value = rettype >= 0;
    info.return_value_info = rettype >= 0 ? &ret : NULL;
    info.return_value_metadata = GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
    info.argument_count = (uint32_t)nargs;
    info.arguments_info = nargs ? args : NULL;
    info.arguments_metadata = argmeta;
    info.default_argument_count = 0;
    info.default_arguments = NULL;
    p_classdb_register_extension_class_method(g_lib, &c, &info);
}

static void reg_node_class(void) {
    SN cls, parent, empty, pn, sig_arg_name, setter, getter, enum_name, const_name, sig_name;
    Str hint;
    GDExtensionClassCreationInfo5 info;
    GDExtensionPropertyInfo prop, sigarg;
    const GDExtensionVariantType ii[2] = {GDEXTENSION_VARIANT_TYPE_INT, GDEXTENSION_VARIANT_TYPE_INT};
    const GDExtensionVariantType s1[1] = {GDEXTENSION_VARIANT_TYPE_STRING};
    const GDExtensionVariantType f1[1] = {GDEXTENSION_VARIANT_TYPE_FLOAT};
    const GDExtensionVariantType i1[1] = {GDEXTENSION_VARIANT_TYPE_INT};
    const uint32_t C = GDEXTENSION_METHOD_FLAGS_DEFAULT | GDEXTENSION_METHOD_FLAG_CONST;
    const uint32_t N = GDEXTENSION_METHOD_FLAGS_DEFAULT;
    make_name(&cls, "FabricTestNode");
    make_name(&parent, "Node");
    make_name(&empty, "");
    make_string(&hint, "");
    memset(&info, 0, sizeof info);
    info.is_exposed = 1;
    info.notification_func = notification;
    info.to_string_func = to_string_fn;
    info.create_instance_func = create_node;
    info.free_instance_func = free_instance;
    info.get_virtual_func = get_virtual;
    info.class_userdata = (void*)0x1234;
    p_classdb_register_extension_class5(g_lib, &cls, &parent, &info);

    reg_method("FabricTestNode", "add", add_call, add_ptr, 2, ii, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "greet", greet_call, greet_ptr, 1, s1, GDEXTENSION_VARIANT_TYPE_STRING, C);
    reg_method("FabricTestNode", "get_speed", get_speed_call, get_speed_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_FLOAT, C);
    reg_method("FabricTestNode", "set_speed", set_speed_call, set_speed_ptr, 1, f1, -1, N);
    reg_method("FabricTestNode", "ping", ping_call, ping_ptr, 1, i1, -1, N);
    reg_method("FabricTestNode", "get_process_count", get_process_count_call, get_process_count_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "get_physics_count", get_physics_count_call, get_physics_count_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "get_ready_count", get_ready_count_call, get_ready_count_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "get_enter_count", get_enter_count_call, get_enter_count_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "get_exit_count", get_exit_count_call, get_exit_count_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "get_notification_count", get_notification_count_call, get_notification_count_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "get_last_notification", get_last_notification_call, get_last_notification_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);
    reg_method("FabricTestNode", "owner_class", owner_class_call, owner_class_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_STRING, C);
    reg_method("FabricTestNode", "spawn_child", spawn_child_call, spawn_child_ptr, 1, s1, GDEXTENSION_VARIANT_TYPE_INT, N);
    reg_method("FabricTestNode", "self_test", self_test_call, self_test_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, C);

    make_name(&pn, "speed");
    make_name(&setter, "set_speed");
    make_name(&getter, "get_speed");
    fill_prop(&prop, GDEXTENSION_VARIANT_TYPE_FLOAT, &pn, &empty, &hint);
    p_classdb_register_extension_class_property(g_lib, &cls, &prop, &setter, &getter);

    make_name(&sig_name, "pinged");
    make_name(&sig_arg_name, "count");
    fill_prop(&sigarg, GDEXTENSION_VARIANT_TYPE_INT, &sig_arg_name, &empty, &hint);
    p_classdb_register_extension_class_signal(g_lib, &cls, &sig_name, &sigarg, 1);

    make_name(&enum_name, "");
    make_name(&const_name, "MAX_SPEED");
    p_classdb_register_extension_class_integer_constant(g_lib, &cls, &enum_name, &const_name, 42, 0);
}

/* FabricTestRef extends RefCounted: value */
typedef struct { GDExtensionObjectPtr owner; int64_t value; } RefInst;
static GDExtensionObjectPtr create_ref(void* ud, GDExtensionBool notify) {
    SN p, c;
    GDExtensionObjectPtr obj;
    RefInst* r;
    (void)ud; (void)notify;
    make_name(&p, "RefCounted");
    obj = p_classdb_construct_object2(&p);
    r = (RefInst*)calloc(1, sizeof(RefInst));
    r->owner = obj;
    make_name(&c, "FabricTestRef");
    p_object_set_instance(obj, &c, r);
    g_created++;
    return obj;
}
static void get_value_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                           GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    int64_t v = ((RefInst*)inst)->value;
    (void)ud; (void)args; (void)argc;
    from_int(r_ret, &v);
    err->error = GDEXTENSION_CALL_OK;
}
static void get_value_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)args;
    *(int64_t*)r_ret = ((RefInst*)inst)->value;
}
static void set_value_call(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr* args, GDExtensionInt argc,
                           GDExtensionVariantPtr r_ret, GDExtensionCallError* err) {
    int64_t v = 0;
    (void)ud; (void)r_ret;
    if (argc < 1) { err->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS; err->expected = 1; return; }
    to_int(&v, (GDExtensionVariantPtr)args[0]);
    ((RefInst*)inst)->value = v;
    err->error = GDEXTENSION_CALL_OK;
}
static void set_value_ptr(void* ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr* args, GDExtensionTypePtr r_ret) {
    (void)ud; (void)r_ret;
    ((RefInst*)inst)->value = *(const int64_t*)args[0];
}

static void reg_other_classes(void) {
    SN cls, parent, empty, pn, setter, getter;
    Str hint;
    GDExtensionClassCreationInfo5 info;
    GDExtensionClassCreationInfo2 legacy;
    GDExtensionPropertyInfo prop;
    const GDExtensionVariantType i1[1] = {GDEXTENSION_VARIANT_TYPE_INT};
    make_name(&empty, "");
    make_string(&hint, "");

    make_name(&cls, "FabricTestRef");
    make_name(&parent, "RefCounted");
    memset(&info, 0, sizeof info);
    info.is_exposed = 1;
    info.create_instance_func = create_ref;
    info.free_instance_func = free_instance;
    p_classdb_register_extension_class5(g_lib, &cls, &parent, &info);
    reg_method("FabricTestRef", "get_value", get_value_call, get_value_ptr, 0, NULL, GDEXTENSION_VARIANT_TYPE_INT, GDEXTENSION_METHOD_FLAGS_DEFAULT | GDEXTENSION_METHOD_FLAG_CONST);
    reg_method("FabricTestRef", "set_value", set_value_call, set_value_ptr, 1, i1, -1, GDEXTENSION_METHOD_FLAGS_DEFAULT);
    make_name(&pn, "value");
    make_name(&setter, "set_value");
    make_name(&getter, "get_value");
    fill_prop(&prop, GDEXTENSION_VARIANT_TYPE_INT, &pn, &empty, &hint);
    p_classdb_register_extension_class_property(g_lib, &cls, &prop, &setter, &getter);

    make_name(&cls, "FabricTestAbstract");
    make_name(&parent, "Node");
    memset(&info, 0, sizeof info);
    info.is_abstract = 1;
    info.is_exposed = 1;
    info.free_instance_func = free_instance;
    p_classdb_register_extension_class5(g_lib, &cls, &parent, &info);

    /* the older registration call (4.2) and its creation struct */
    make_name(&cls, "FabricTestLegacy");
    make_name(&parent, "Node");
    memset(&legacy, 0, sizeof legacy);
    legacy.is_exposed = 1;
    legacy.create_instance_func = create_legacy;   /* the 4.2 creation signature takes no "notify" flag */
    legacy.free_instance_func = free_instance;
    legacy.notification_func = notification;
    p_classdb_register_extension_class2(g_lib, &cls, &parent, &legacy);
}

static void unreg(const char* name) {
    SN n;
    make_name(&n, name);
    p_classdb_unregister_extension_class(g_lib, &n);
}

/* ---- entry point ---- */
static void init_cb(void* ud, GDExtensionInitializationLevel level) {
    (void)ud;
    if (g_init_count < 8) g_init_levels[g_init_count++] = (int32_t)level;
    if (level == GDEXTENSION_INITIALIZATION_SCENE) {
        reg_node_class();
        reg_other_classes();
    }
}
static void deinit_cb(void* ud, GDExtensionInitializationLevel level) {
    (void)ud;
    if (g_deinit_count < 8) g_deinit_levels[g_deinit_count++] = (int32_t)level;
    if (level == GDEXTENSION_INITIALIZATION_SCENE) {
        unreg("FabricTestLegacy");
        unreg("FabricTestAbstract");
        unreg("FabricTestRef");
        unreg("FabricTestNode");
    }
}

#define LOAD(type, name) p_##name = (type)g_get_proc(#name)

FT_EXPORT GDExtensionBool fabric_test_init(GDExtensionInterfaceGetProcAddress get_proc, GDExtensionClassLibraryPtr lib, GDExtensionInitialization* init) {
    GDExtensionGodotVersion ver;
    Str path;
    g_get_proc = get_proc;
    g_lib = lib;
    g_init_count = g_deinit_count = g_freed = g_created = 0;
    LOAD(GDExtensionInterfaceGetGodotVersion, get_godot_version);
    LOAD(GDExtensionInterfaceStringNameNewWithUtf8Chars, string_name_new_with_utf8_chars);
    LOAD(GDExtensionInterfaceStringNewWithUtf8Chars, string_new_with_utf8_chars);
    LOAD(GDExtensionInterfaceStringNewWithUtf32Chars, string_new_with_utf32_chars);
    LOAD(GDExtensionInterfaceStringToUtf8Chars, string_to_utf8_chars);
    LOAD(GDExtensionInterfaceStringToUtf32Chars, string_to_utf32_chars);
    LOAD(GDExtensionInterfaceGetLibraryPath, get_library_path);
    LOAD(GDExtensionInterfaceClassdbConstructObject2, classdb_construct_object2);
    LOAD(GDExtensionInterfaceObjectSetInstance, object_set_instance);
    LOAD(GDExtensionInterfaceClassdbRegisterExtensionClass5, classdb_register_extension_class5);
    LOAD(GDExtensionInterfaceClassdbRegisterExtensionClass2, classdb_register_extension_class2);
    LOAD(GDExtensionInterfaceClassdbRegisterExtensionClassMethod, classdb_register_extension_class_method);
    LOAD(GDExtensionInterfaceClassdbRegisterExtensionClassIntegerConstant, classdb_register_extension_class_integer_constant);
    LOAD(GDExtensionInterfaceClassdbRegisterExtensionClassProperty, classdb_register_extension_class_property);
    LOAD(GDExtensionInterfaceClassdbRegisterExtensionClassSignal, classdb_register_extension_class_signal);
    LOAD(GDExtensionInterfaceClassdbUnregisterExtensionClass, classdb_unregister_extension_class);
    LOAD(GDExtensionInterfaceClassdbGetMethodBind, classdb_get_method_bind);
    LOAD(GDExtensionInterfaceObjectMethodBindCall, object_method_bind_call);
    LOAD(GDExtensionInterfaceObjectMethodBindPtrcall, object_method_bind_ptrcall);
    LOAD(GDExtensionInterfaceGetVariantFromTypeConstructor, get_variant_from_type_constructor);
    LOAD(GDExtensionInterfaceGetVariantToTypeConstructor, get_variant_to_type_constructor);
    LOAD(GDExtensionInterfaceVariantGetPtrConstructor, variant_get_ptr_constructor);
    LOAD(GDExtensionInterfaceVariantGetPtrDestructor, variant_get_ptr_destructor);
    LOAD(GDExtensionInterfaceVariantGetPtrBuiltinMethod, variant_get_ptr_builtin_method);
    LOAD(GDExtensionInterfaceVariantGetPtrUtilityFunction, variant_get_ptr_utility_function);
    LOAD(GDExtensionInterfaceVariantGetPtrOperatorEvaluator, variant_get_ptr_operator_evaluator);
    LOAD(GDExtensionInterfaceVariantGetPtrSetter, variant_get_ptr_setter);
    LOAD(GDExtensionInterfaceVariantGetPtrGetter, variant_get_ptr_getter);
    LOAD(GDExtensionInterfaceVariantGetPtrIndexedGetter, variant_get_ptr_indexed_getter);
    LOAD(GDExtensionInterfaceVariantNewNil, variant_new_nil);
    LOAD(GDExtensionInterfaceVariantDestroy, variant_destroy);
    LOAD(GDExtensionInterfaceVariantStringify, variant_stringify);
    LOAD(GDExtensionInterfaceVariantEvaluate, variant_evaluate);
    LOAD(GDExtensionInterfaceVariantHash, variant_hash);
    LOAD(GDExtensionInterfaceVariantGetType, variant_get_type);
    LOAD(GDExtensionInterfaceMemAlloc, mem_alloc);
    LOAD(GDExtensionInterfaceMemFree, mem_free);
    LOAD(GDExtensionInterfacePrintWarning, print_warning);
    if (!p_get_godot_version || !p_classdb_register_extension_class5 || !p_variant_get_ptr_builtin_method) return 0;

    p_get_godot_version(&ver);
    g_version_major = (int32_t)ver.major;
    g_version_minor = (int32_t)ver.minor;
    if (ver.major < 4 || (ver.major == 4 && ver.minor < 1)) return 0;

    from_int = p_get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_INT);
    from_float = p_get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_FLOAT);
    from_string = p_get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_STRING);
    from_name = p_get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
    from_bool = p_get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_BOOL);
    from_vector3 = p_get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR3);
    to_int = p_get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_INT);
    to_float = p_get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_FLOAT);
    to_string = p_get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_STRING);
    to_vector3 = p_get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR3);
    dtor_string = p_variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING);
    dtor_name = p_variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
    dtor_array = p_variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_ARRAY);
    ctor_string_default = p_variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_STRING, 0);
    ctor_name_from_string = p_variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME, 2);
    ctor_array_default = p_variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_ARRAY, 0);
    ctor_vector3_xyz = p_variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR3, 3);
    (void)from_bool; (void)from_vector3; (void)to_vector3; (void)dtor_name;

    p_get_library_path(lib, &path);
    g_lib_path_len = (int32_t)p_string_to_utf8_chars(&path, NULL, 0);
    dtor_string(&path);

    init->minimum_initialization_level = GDEXTENSION_INITIALIZATION_CORE;
    init->userdata = NULL;
    init->initialize = init_cb;
    init->deinitialize = deinit_cb;
    return 1;
}

/* An entry that refuses to start, for the failure path of the loader. */
FT_EXPORT GDExtensionBool fabric_test_init_fail(GDExtensionInterfaceGetProcAddress get_proc, GDExtensionClassLibraryPtr lib, GDExtensionInitialization* init) {
    (void)get_proc; (void)lib; (void)init;
    return 0;
}
