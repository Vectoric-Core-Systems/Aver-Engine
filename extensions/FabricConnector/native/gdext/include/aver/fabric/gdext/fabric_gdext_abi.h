#ifndef AVER_FABRIC_GDEXT_ABI_H
#define AVER_FABRIC_GDEXT_ABI_H

/* Aver.Fabric.GDExt -- the plain-C seam the Aver.Fabric C# runtime hosts Godot GDExtension plugins through
 * (docs/plans/fabric/G3_GDEXT.md). Objects and libraries cross as int32_t handles (0 = invalid); names are UTF-8
 * const char*; values cross as flat Variants (the "wire form" below). Setters and queries return 1 for success and 0
 * for failure unless stated, and a failure records its reason: aver_gdext_last_error() (thread-local, set on success
 * too, to ""). The host is single threaded: call it from the game thread.
 *
 * WIRE FORM of a Variant (little endian): one type byte -- the GDExtension variant type number, 0 Nil ... 38
 * PackedVector4Array -- then the payload:
 *   Nil: none        Bool: 1 byte        Int: 8 (int64)        Float: 8 (double)
 *   String, StringName, NodePath: int32 byte count, UTF-8 bytes
 *   Vector2/2i 8, Rect2/2i 16, Vector3/3i 12, Transform2D 24, Vector4/4i 16, Plane 16, Quaternion 16, AABB 24,
 *   Basis 36 (rows), Transform3D 48 (basis rows, origin), Projection 64 (columns), Color 16, RID 8: the native bytes
 *   Object: uint64 instance id (0 = null)        Callable, Signal: sent as Nil
 *   Array: int32 count, then that many Variants        Dictionary: int32 count, then key, value pairs
 *   Packed*Array: int32 count, then the raw elements (PackedStringArray: int32 byte count + UTF-8 bytes each)
 * A list of arguments is an Array (int32 count then Variants). */

#include <stdint.h>

#if defined(_WIN32) && !defined(AVER_GDEXT_STATIC)
#  if defined(AVER_GDEXT_ABI_BUILD)
#    define AVER_GDEXT_API __declspec(dllexport)
#  else
#    define AVER_GDEXT_API __declspec(dllimport)
#  endif
#else
#  define AVER_GDEXT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_GDEXT_ABI_VERSION_MAJOR 1
#define AVER_GDEXT_ABI_VERSION_MINOR 0
#define AVER_GDEXT_ABI_VERSION ((AVER_GDEXT_ABI_VERSION_MAJOR << 16) | AVER_GDEXT_ABI_VERSION_MINOR)

/* The version this library was built with, so a caller can catch a stale binary. */
AVER_GDEXT_API int32_t aver_gdext_abi_version(void);
/* Why the last call failed; "" if it did not. Valid until the next call on this thread. */
AVER_GDEXT_API const char* aver_gdext_last_error(void);

/* ---- setup ---- */
/* extension_api.json of the Godot version the plugins target, for the method-hash checks. Without it hashes are not checked
 * (on the first load the host looks for it in the extension's data folder, <library dir>/../../data, then beside the executable). */
AVER_GDEXT_API int32_t aver_gdext_set_api_path(const char* extension_api_json);
/* Feature tags used to pick the library from [libraries], comma separated ("windows,debug,x86_64"); "" restores the default. */
AVER_GDEXT_API int32_t aver_gdext_set_features(const char* csv);

/* ---- libraries ---- */
/* Loads a .gdextension file: picks the library for this platform, loads it, runs the entry symbol and the CORE, SERVERS and
 * SCENE initialisation levels. `project_dir` resolves res:// library paths (may be NULL). The library handle, 0 on failure.
 * Loading a path twice returns the same handle. */
AVER_GDEXT_API int32_t aver_gdext_load(const char* gdextension_path, const char* project_dir);
/* Deinitialises in reverse, frees the extension's objects and classes, unloads the library. */
AVER_GDEXT_API int32_t aver_gdext_unload(int32_t library);
AVER_GDEXT_API int32_t aver_gdext_is_loaded(const char* gdextension_path);       /* the handle, 0 */
AVER_GDEXT_API int32_t aver_gdext_library_count(void);
AVER_GDEXT_API int32_t aver_gdext_library_at(int32_t index);                      /* the handle, 0 */
AVER_GDEXT_API const char* aver_gdext_library_path(int32_t library);              /* the .gdextension path, "" */

/* ---- classes (what extensions registered) ---- */
AVER_GDEXT_API int32_t aver_gdext_class_count(int32_t library);                   /* library 0 = every library */
AVER_GDEXT_API const char* aver_gdext_class_name(int32_t library, int32_t index);
AVER_GDEXT_API int32_t aver_gdext_class_exists(const char* cls);                  /* 1 extension class, 2 host (engine) class, 0 */
AVER_GDEXT_API const char* aver_gdext_class_parent(const char* cls);              /* "" unknown or root */
AVER_GDEXT_API int32_t aver_gdext_class_is_a(const char* cls, const char* base);
AVER_GDEXT_API int32_t aver_gdext_class_can_instantiate(const char* cls);
AVER_GDEXT_API int32_t aver_gdext_method_count(const char* cls);                  /* the class's own registered methods */
AVER_GDEXT_API const char* aver_gdext_method_name(const char* cls, int32_t index);
/* out4: argument count, GDExtensionClassMethodFlags, return variant type (-1 none, 0 a Variant), 1 if vararg */
AVER_GDEXT_API int32_t aver_gdext_method_info(const char* cls, int32_t index, int32_t* out4);
AVER_GDEXT_API int32_t aver_gdext_method_arg_type(const char* cls, int32_t index, int32_t arg);   /* variant type, 0 = Variant, -1 bad */
AVER_GDEXT_API int32_t aver_gdext_property_count(const char* cls);
AVER_GDEXT_API const char* aver_gdext_property_name(const char* cls, int32_t index);
AVER_GDEXT_API int32_t aver_gdext_property_type(const char* cls, int32_t index);  /* variant type, -1 bad */
AVER_GDEXT_API int32_t aver_gdext_signal_count(const char* cls);
AVER_GDEXT_API const char* aver_gdext_signal_name(const char* cls, int32_t index);
AVER_GDEXT_API int32_t aver_gdext_signal_arg_count(const char* cls, int32_t index);   /* -1 bad */
AVER_GDEXT_API int32_t aver_gdext_constant_count(const char* cls);
AVER_GDEXT_API const char* aver_gdext_constant_name(const char* cls, int32_t index);
AVER_GDEXT_API int64_t aver_gdext_constant_value(const char* cls, int32_t index);

/* ---- objects ---- */
AVER_GDEXT_API int32_t aver_gdext_instantiate(const char* cls);                   /* the object handle, 0 (abstract or unknown) */
AVER_GDEXT_API int32_t aver_gdext_free(int32_t obj);                              /* frees the object and, for a Node, its children */
AVER_GDEXT_API int32_t aver_gdext_object_is_valid(int32_t obj);
AVER_GDEXT_API int64_t aver_gdext_object_instance_id(int32_t obj);                /* Godot's instance id, 0 */
AVER_GDEXT_API const char* aver_gdext_object_class(int32_t obj);
AVER_GDEXT_API int32_t aver_gdext_object_by_instance_id(int64_t id);              /* the handle, 0 */
AVER_GDEXT_API int64_t aver_gdext_object_get_managed(int32_t obj);                /* a slot the embedder owns */
AVER_GDEXT_API int32_t aver_gdext_object_set_managed(int32_t obj, int64_t value);

/* ---- calls ---- */
/* Calls a method by name (an extension method, an inherited host method such as Object.call or Node.add_child). `args` is an
 * Array in wire form. Returns the size in bytes of the result (a Variant in wire form; 1 for nil), or -1 on failure; fetch the
 * bytes with aver_gdext_result. */
AVER_GDEXT_API int32_t aver_gdext_call(int32_t obj, const char* method, const void* args, int32_t args_len);
AVER_GDEXT_API int32_t aver_gdext_get(int32_t obj, const char* property);        /* result size as aver_gdext_call, -1 */
AVER_GDEXT_API int32_t aver_gdext_set(int32_t obj, const char* property, const void* value, int32_t value_len);   /* one Variant */
AVER_GDEXT_API int32_t aver_gdext_has_method(int32_t obj, const char* method);
/* Copies the bytes of the last aver_gdext_call / aver_gdext_get result (up to `cap`); returns the count copied. */
AVER_GDEXT_API int32_t aver_gdext_result(void* out, int32_t cap);

/* ---- signals ---- */
/* `fn` runs for every signal the object emits, with the arguments as an Array in wire form (valid during the call). */
typedef void (*AverGdextSignalFn)(void* user, int32_t obj, const char* signal, const void* args, int32_t args_len);
AVER_GDEXT_API int32_t aver_gdext_listen(int32_t obj, AverGdextSignalFn fn, void* user);
AVER_GDEXT_API int32_t aver_gdext_emit(int32_t obj, const char* signal, const void* args, int32_t args_len);   /* 1 emitted */
/* Connects `signal` of `obj` to a method of `target` (CONNECT_* flags as in Godot). */
AVER_GDEXT_API int32_t aver_gdext_connect_method(int32_t obj, const char* signal, int32_t target, const char* method, int32_t flags);

/* ---- lifecycle and virtuals ---- */
AVER_GDEXT_API int32_t aver_gdext_has_virtual(int32_t obj, const char* name);     /* "_ready", "_process", ... */
/* Godot's order: enter pre-order, ready post-order, exit children first. recursive = 0 touches one node (the embedder drives
 * the tree itself). 1 on success. */
AVER_GDEXT_API int32_t aver_gdext_enter_tree(int32_t obj, int32_t recursive);
AVER_GDEXT_API int32_t aver_gdext_ready(int32_t obj, int32_t recursive);
AVER_GDEXT_API int32_t aver_gdext_exit_tree(int32_t obj, int32_t recursive);
/* Calls _process (or _physics_process) when the node processes. */
AVER_GDEXT_API int32_t aver_gdext_process(int32_t obj, double delta, int32_t physics, int32_t recursive);
AVER_GDEXT_API int32_t aver_gdext_notification(int32_t obj, int32_t what);
/* Runs call_deferred / queue_free / deferred signal connections queued so far. */
AVER_GDEXT_API int32_t aver_gdext_flush_deferred(void);

/* ---- nodes (Node / Node3D behind the C# peers) ---- */
AVER_GDEXT_API int32_t aver_gdext_node_set_name(int32_t obj, const char* name);
AVER_GDEXT_API const char* aver_gdext_node_get_name(int32_t obj);
AVER_GDEXT_API int32_t aver_gdext_node_set_parent(int32_t child, int32_t parent);   /* parent 0 detaches */
/* A child the extension added natively (add_child) since the last call, 0 when there is none. Each is returned once. */
AVER_GDEXT_API int32_t aver_gdext_node_pending_child(int32_t obj);
AVER_GDEXT_API int32_t aver_gdext_node_process_flags(int32_t obj);                  /* bit 0 process, bit 1 physics process, bit 2 ready */
/* Node3D local transform: 9 basis floats (rows) then the origin; Godot space and metres. The version changes whenever the
 * extension writes the transform, so a peer can tell whether to pull. */
AVER_GDEXT_API int32_t aver_gdext_node3d_get_transform(int32_t obj, float* out12);
AVER_GDEXT_API int32_t aver_gdext_node3d_set_transform(int32_t obj, const float* in12);
AVER_GDEXT_API int32_t aver_gdext_node3d_transform_version(int32_t obj);

/* ---- diagnostics ---- */
/* Interface functions and engine methods extensions asked for that the host does not implement, in first-asked order. */
AVER_GDEXT_API int32_t aver_gdext_unsupported_count(void);
AVER_GDEXT_API const char* aver_gdext_unsupported_name(int32_t index);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FABRIC_GDEXT_ABI_H */
