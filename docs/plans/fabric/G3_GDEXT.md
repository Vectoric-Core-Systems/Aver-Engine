# G3 F5: the GDExtension host (`modules/fabric.gdext`)

A converted Godot project keeps its native plugins: a `.gdextension` file names a per-platform library, and the library registers classes with the engine through Godot's C interface (`gdextension_interface.h`). `modules/fabric.gdext` is the host those libraries run against under Aver. It loads them, serves the interface functions godot-cpp based extensions ask for, keeps a small engine class registry behind them, and exposes the result to the `Aver.Fabric` C# runtime through a plain C ABI.

**Read first, the honest status.** The host was written from `gdextension_interface.h`, `extension_api.json` (both vendored from godot-cpp `godot-4.5-stable`, `third_party/godot-gdextension/`) and Godot's documented behaviour. It was exercised by a test extension written in C against the header alone (`tests/fabric/gdext_test/fabric_test_ext.c`), calling the interface the way godot-cpp does. **No real godot-cpp (or Rust/Swift/C#) plugin binary was run**: no sample project or plugin may be downloaded, and godot-cpp is not vendored. Expect gaps the first time a real plugin loads; they surface as a null from `classdb_get_method_bind` or a logged stub call, with the missing name in `aver_gdext_unsupported_*` (section 6), not as a crash. Whether godot-cpp's start-up accepts everything the host answers is unverified. The engine-run ban applies: nothing here ran inside the editor or the game.

## 1. Files and targets

| Path | What |
|---|---|
| `modules/fabric.gdext/` | the module (`module.json`, `CMakeLists.txt`) |
| `include/aver/fabric/gdext/fabric_gdext_abi.h` | the `aver_gdext_*` C ABI (section 5) |
| `src/GdextValue.*`, `GdextOps.cpp`, `GdextBuiltins.cpp` | the value model: Variant and the builtin types in raw memory, operators, members, constructors, builtin methods, utility functions |
| `src/GdextHost.*` | registry, objects, calls, signals, virtuals, library loading, the `extension_api.json` reader |
| `src/GdextEngine.cpp` | the engine classes (section 4) |
| `src/GdextInterface.cpp`, `GdextClassDb.cpp` | the interface functions and the `get_proc_address` table |
| `src/GdextWire.cpp`, `GdextAbi.cpp` | the flat Variant form and the C ABI |
| CMake target `Aver.Fabric.GDExt` | static; what the headless test links |
| CMake target `Aver.Fabric.GDExt.Abi` | SHARED, `bin/Aver.Fabric.GDExt.Abi.dll`; what the C# runtime P/Invokes. `extension_api.json` is staged beside it |
| `scripting/csharp/Aver.Fabric/GDExtension/` | `GDExtensionManager`, `GDExtensionNode`, `GDExtensionNode3D`, `GDExtensionObject`, `VariantWire`, the peer and the P/Invoke file |
| `tests/fabric/gdext_test/`, `tests/fabric/data/gdext/` | the test extension (C) and the `.gdextension` fixtures |

The host is single threaded: call it from the game thread. Extensions that create threads and call back into the interface from them are not supported.

## 2. Loading

`aver_gdext_load(path, projectDir)` / `GDExtensionManager.Load(path, projectDir)`:

1. Parses the `.gdextension` with `Aver.Formats.Godot` (`loadGodotGdExtension`).
2. **Compatibility.** `compatibility_minimum` must be present and at least 4.1; a missing key means a Godot 4.0 extension and is refused with that message. A minimum above 4.5 is refused. `compatibility_maximum`, when present, must not be below 4.5.
3. **Library choice.** The first `[libraries]` entry, in file order, all of whose tags are in the feature set (Godot's rule). The default feature set is the OS (`windows`/`linux`/`macos`), `debug`+`template_debug` or `release`+`template_release`, `x86_64` or `arm64`, `64`, `single`. `aver_gdext_set_features("windows,debug,x86_64")` overrides it.
4. **Path.** `res://x` resolves against `projectDir`; an absolute path is used as is; a relative one is relative to the `.gdextension` file.
5. Loads the library with `aver::loadLibrary`, finds `entry_symbol` and calls it with `get_proc_address`, the library pointer and a `GDExtensionInitialization`.
6. Runs `initialize(userdata, level)` for CORE, SERVERS and SCENE, starting at the extension's `minimum_initialization_level`. The EDITOR level is never run. `register_main_loop_callbacks` is stored and its startup callback runs after initialization (the frame callback is not driven).
7. Loading the same path again returns the same library. Two libraries registering the same class name: the second registration is refused with an error.

Failures return 0 with `aver_gdext_last_error()` set: file unreadable, compatibility, no library for the platform, library missing, library will not load, entry symbol not found, entry returned false. A failed load leaves nothing registered.

`aver_gdext_unload(library)`: `deinitialize` for SCENE, SERVERS, CORE (reverse), then the extension's remaining objects are freed through its `free_instance_func`, its remaining classes are unregistered, the library is unloaded. Engine-class method binds an extension cached stay valid for the process (an extension loaded again in the same process keeps working).

`extension_api.json` is read once, from `aver_gdext_set_api_path` or, on the first load, from beside the executable. Without it hashes are not checked and a warning is logged.

## 3. The value model

Sizes follow the `float_64` configuration of the dump (single-precision reals, 64-bit pointers), which is what godot-cpp builds for: `Variant` 24 bytes, `String`/`StringName`/`NodePath`/`Array`/`Dictionary`/`Object`/`RID` 8, `Callable`/`Signal` 16, `Packed*Array` 16, `Vector3` 12, `Basis` 36, `Transform3D` 48 ...

| Type | Representation (the extension never looks inside) |
|---|---|
| Variant | type + 16 payload bytes; builtins up to 16 bytes inline, `Transform2D`, `AABB`, `Basis`, `Transform3D`, `Projection` behind a heap copy |
| String, NodePath | pointer to a UTF-32 buffer, copied deep on copy |
| StringName | pointer to an interned immortal entry: equal names have equal data pointers |
| Array, Dictionary | reference-counted shared object (Godot's reference semantics). A Dictionary keeps insertion order and finds keys by linear search |
| Packed*Array | reference-counted shared buffer, copied on write (value semantics); `operator_index` returns pointers into the buffer |
| Object | pointer to the host object; freed objects stay readable as tombstones (released when the last library unloads) |
| Callable | shared; a custom callable keeps the extension's `GDExtensionCallableCustomInfo2`, a method callable keeps object id and method |

Strings are UTF-32 inside; the `string_new_with_*` / `string_to_*` functions convert Latin-1, UTF-8, UTF-16 (with surrogates), UTF-32 and wide chars. `to_*` with a null buffer returns the length, with a short buffer fills it and returns the full length (Godot's behaviour). Floats stringify as Godot 4 does: a lone float with `.0` (`2.0`), vector components without it (`(1, 2.5)`).

Pointer-call convention (what `ptrcall` and the builtin tables use): ints are `int64`, floats `double`, bools `uint8`, objects the address of the object pointer, a `Variant` parameter a `Variant*`, everything else its native memory. Pointer-call functions carry no user data, so each (operator, builtin method, constructor, utility function, member) a plugin asks for takes a numbered thunk from a fixed pool (1024 operators, 512 methods, 256 constructors, 192 members each way, 128 utilities); a pool that fills up answers null and logs it.

## 4. What the host implements

### Interface functions

`get_proc_address` answers **all 173** names of the 4.5 interface, because godot-cpp refuses to start when a name it loads comes back null. 159 are real; 14 are stubs or inline (section 6). A name outside the 4.5 interface is null, logged once and listed. Served: `get_godot_version(2)`, `mem_alloc/realloc/free`, `print_error/warning/script_error` (+ `_with_message`), `get_native_struct_size` (sizes computed from the dump's field lists), every `variant_*` listed in the task (new_copy, new_nil, destroy, call, evaluate, set/get in all four forms, iter_*, hash, recursive_hash, hash_compare, booleanize, duplicate, stringify, get_type, has_method/member/key, get_object_instance_id, get_type_name, can_convert(_strict), construct, `get_variant_from/to_type_constructor`, `variant_get_ptr_*`: internal getter, operator evaluator, builtin method, constructor, destructor, setter, getter, indexed and keyed setters/getters, keyed checker, utility function), `string_*` (all `new_with_*`, all `to_*`, `operator_index(_const)`, `operator_plus_eq_*`, `resize`, `string_name_new_with_*`), the `packed_*_array_operator_index(_const)` family, `array_operator_index(_const)`, `array_ref`, `array_set_typed`, `dictionary_operator_index(_const)`, `dictionary_set_typed` (accepted, element types are not enforced), `object_method_bind_call/ptrcall`, `object_destroy`, `global_get_singleton`, `object_get/set/free_instance_binding`, `object_set_instance`, `object_get_class_name`, `object_cast_to`, `object_get_instance_from_id/id`, `object_has_script_method` (always false) and `object_call_script_method` (error), `ref_get_object/ref_set_object` (take and drop a reference), `callable_custom_create(2)` and `callable_custom_get_userdata`, `classdb_construct_object(2)`, `classdb_get_method_bind`, `classdb_get_class_tag`, `classdb_register_extension_class` 1 to 5, `_method`, `_virtual_method`, `_integer_constant`, `_property`, `_property_indexed`, `_property_group`, `_property_subgroup`, `_signal`, `classdb_unregister_extension_class`, `get_library_path`, `editor_add_plugin/remove_plugin/help_load_xml*/register_get_classes_used_callback` (no-ops), `register_main_loop_callbacks`.

**Method binds and hashes.** `classdb_get_method_bind(class, method, hash)` looks the method up on the class, then its ancestors. For a host class the hash is checked against `extension_api.json`: the current `hash` or one of its `hash_compatibility` entries (so extensions built for Godot 4.1 to 4.5 bind), otherwise null and an error naming both hashes. Builtin methods and utility functions get the same check. A method the host has no implementation for is null and is listed (section 6). Extension classes' own methods bind without a hash.

**Class registration.** All five creation-info versions are normalised (create_instance with and without the notify flag, `get_virtual` with and without the hash, `get_virtual_call_data` + `call_virtual_with_data`, notification 1 and 2). Method flags (const, vararg, static, virtual), defaults, argument infos, properties (with setter/getter names and an index), property groups, signals and integer constants are recorded and listed over the ABI. Properties are read and written through the registered setter and getter; `set_func`/`get_func` are the fallback.

### Engine classes

Only these classes exist: `Object`, `RefCounted`, `Resource`, `Node`, `Node3D`, and four singletons (`Engine`, `ClassDB`, `OS`, `ProjectSettings`, with a few methods each). 94 engine methods are implemented, each checked by the test against the dump (declaring class, parameter types, return, vararg).

| Class | Implemented |
|---|---|
| Object | get_class, is_class, get, set, call, call_deferred, has_method, has_signal, connect, disconnect, is_connected, emit_signal, get_instance_id, to_string, notification, set_meta/get_meta/has_meta/remove_meta, get_script/set_script (nil), is_queued_for_deletion; signals `script_changed`, `property_list_changed` |
| RefCounted | init_ref, reference, unreference, get_reference_count |
| Resource | set/get_path, take_over_path, set/get_name, emit_changed; signal `changed` |
| Node | name, add/remove_child, get_child(ren)/get_child_count, get_parent, get_path, get_node(_or_null), has_node, find_child, is_inside_tree, get_tree (null), get_index, move_child, owner, groups, queue_free, set/is_processing and physics, process_mode, is_node_ready; the lifecycle signals (`ready`, `renamed`, `tree_entered/exiting/exited`, `child_entered/exiting_tree`, `child_order_changed`); unique names (`@name@2`) |
| Node3D | position, rotation (Euler YXZ), scale, transform, global transform / position, translate, visible; signal `visibility_changed` |

Node3D transforms are in Godot space (metres, +Y up); the C# peers convert nothing, because `Aver.Fabric` nodes are in Godot space too.

Not implemented, and an extension that needs them gets a null bind (the dump has 971 classes): SceneTree (`get_tree()` is null), Viewport, Window, every Control, Node2D and CanvasItem class, MeshInstance3D, Camera3D, lights, physics bodies, Input, FileAccess, DirAccess, Image, Texture and every other resource class, the servers (RenderingServer, PhysicsServer3D ...), threads, `Object.get_property_list`/`get_method_list`/`get_signal_list`, `Object.tr`, `Node.get_viewport`, `Node.duplicate`, scene instancing and `PackedScene`. **Full Godot ClassDB coverage is out of reach**; this registry is the minimum that classes extending Object, RefCounted, Resource, Node or Node3D and calling the lists above need.

### Builtin types

- **Constructors:** every constructor of every builtin type, by the dump's index; the test compares the argument lists of all 156 with `extension_api.json`.
- **Operators:** comparison for every type, `+ - * / % **`, shifts and bit operators, `and or xor not`, `in`, unary `- + ~`: numbers, Strings (`+`), vectors (2/3/4 and the integer forms), `Color`, `Quaternion` (`*`), `Basis`/`Transform3D` products and vector transforms, Array and Packed concatenation. Not: `String % args` (format), `Transform2D`, `Projection`, `Plane`, `Rect2`, `AABB` arithmetic.
- **Members and indexing:** `x y z w r g b a d`, `normal`, `position`, `size`, `end`, `origin`, `basis`, Basis/Projection/Transform2D columns, `[]` on vectors, Basis, Array, Packed arrays and Strings, keyed access on Dictionary and Object properties. Not: Color `h s v r8 g8 b8 a8`.
- **Builtin methods:** 250 of the 998 the dump lists (String and StringName text methods, Vector2/3/4 and Quaternion math, Array, Dictionary, Packed arrays, Callable, Signal), each verified against the dump's arguments, return and constness. godot-cpp calls builtin function pointers without a null check, so a method Godot has (the dump lists it) and the host lacks gets a **stub**: calling it logs an error once, adds `builtin:Type.method` to the diagnostics list and writes nothing to the result. A method the dump does not list, or whose hash does not match, is null.
- **Utility functions:** 56 of 114 (print family, `str`, `typeof`, trigonometry, `sqrt pow floor ceil round`, `lerpf clamp min max`, `randf randi randomize seed`, `is_instance_valid ...`); the other 58 are stubs, and so are the operators (749 in the dump; the implemented ones are real) and members (62, e.g. `Color.h`) the host cannot evaluate.
- `variant_call_static` and `variant_get_constant_value` are stubs that report the call as unsupported.

## 5. The `aver_gdext` C ABI

Header: `fabric_gdext_abi.h` (`AVER_GDEXT_ABI_VERSION` 1.0). `int32_t` handles (0 = invalid), UTF-8 `const char*`, `aver_gdext_last_error()` for the reason of a failure, per docs/ABI.md section 16. Two deviations from the type rules, both needed to carry values: buffers (`const void* + int32 length`) hold flat Variants, and `aver_gdext_listen` takes a callback.

**Wire form of a Variant** (little endian): one type byte (the GDExtension variant type number, 0 Nil to 38 PackedVector4Array) then the payload: Bool 1 byte, Int 8, Float 8 (double), String/StringName/NodePath `int32` byte count + UTF-8, the math types their native bytes (Vector2 8, Rect2 16, Vector3 12, Transform2D 24, Vector4 16, Plane 16, Quaternion 16, AABB 24, Basis 36 as rows, Transform3D 48 as basis rows then origin, Projection 64 as columns, Color 16), RID 8, Object the `uint64` instance id (0 = null), Array `int32` count + Variants, Dictionary `int32` count + key/value pairs, Packed arrays `int32` count + raw elements (PackedStringArray: count, then byte count + UTF-8 each). Callable and Signal travel as Nil. An argument list is an Array. The test round-trips a nested value byte for byte through `set_meta`/`get_meta`.

| Group | Functions |
|---|---|
| setup | `abi_version`, `last_error`, `set_api_path`, `set_features` |
| libraries | `load`, `unload`, `is_loaded`, `library_count`, `library_at`, `library_path` |
| classes | `class_count`, `class_name`, `class_exists` (1 extension, 2 host, 0), `class_parent`, `class_is_a`, `class_can_instantiate`, `method_count/name/info/arg_type`, `property_count/name/type`, `signal_count/name/arg_count`, `constant_count/name/value` |
| objects | `instantiate`, `free`, `object_is_valid`, `object_instance_id`, `object_class`, `object_by_instance_id`, `object_get_managed`/`set_managed` |
| calls | `call(obj, method, args)` returns the result size (-1 on failure), `get(obj, property)` likewise, `result(buf, cap)` copies the bytes, `set`, `has_method` |
| signals | `listen(obj, fn, user)` (the callback gets the signal name and the arguments), `emit`, `connect_method(obj, signal, target, method, flags)` |
| lifecycle | `has_virtual`, `enter_tree`, `ready`, `exit_tree`, `process(obj, delta, physics, recursive)`, `notification`, `flush_deferred` |
| nodes | `node_set_name/get_name`, `node_set_parent`, `node_pending_child`, `node_process_flags`, `node3d_get/set_transform`, `node3d_transform_version` |
| diagnostics | `unsupported_count/name` |

Lifecycle is Godot's order: enter pre-order (`NOTIFICATION_ENTER_TREE` 10, then `_enter_tree`, then the `tree_entered` signal), ready post-order (13, `_ready`, `ready`), exit children first (`tree_exiting`, 11, `_exit_tree`, `tree_exited`). `recursive` drives the native subtree (the C++ test does); the C# peers pass 0 and drive each node from the managed tree. A node starts processing when ready runs and the extension implements `_process` / `_physics_process` (or calls `set_process`). Deferred work (`call_deferred`, `queue_free`, deferred connections) waits for `flush_deferred`; the C# peers flush after each process call.

## 6. Unsupported, and how it shows

Whatever an extension uses that the host cannot do is logged once and recorded; `aver_gdext_unsupported_name(i)` / `GDExtensionManager.GetUnsupportedCalls()` list them in first-asked order, prefixed `interface:`, `method:Class::name` (an engine method with no implementation: its bind is null) or `builtin:...` (a stub that was called: a builtin method, operator, member or utility function).

Interface functions that are stubs (the call is reported and returns the neutral value): `xml_parser_open_buffer` (ERR_UNAVAILABLE), `file_access_store_buffer`, `file_access_get_buffer` (0), `image_ptr`, `image_ptrw` (null), `script_instance_create`, `script_instance_create2`, `script_instance_create3`, `placeholder_script_instance_create` (null), `placeholder_script_instance_update`, `object_get_script_instance` (null), `object_set_script_instance`. The two `worker_thread_pool_add_native_*` functions run the task at once on the calling thread. (Script instances and the editor are out of scope: no scripting language is hosted, there is no editor.)

Other limits: no hot reload (`reloadable` is ignored); the extension's `recreate_instance_func` is never called; `GDExtensionClassCallVirtualWithData` is supported but input virtuals (`_input` and friends, which take engine event objects) are not driven; `get_property_list` / `validate_property` / `property_can_revert` / `to_string` hooks are stored, only `to_string` is used (by `Object.to_string`); `Object.free` is not an engine method in 4.5, extensions use `object_destroy`; typed Arrays and Dictionaries record their element type but do not enforce it; objects of the host cannot be handed to another extension's classes by class tag unless both are host classes.

## 7. The C# side

```csharp
GDExtensionManager.Load("res://addons/foo/foo.gdextension", projectDir);   // or an absolute / relative path
GodotObject? o = ClassDB.Instantiate("FooNode").AsGodotObject();           // a GDExtensionNode (Node), GDExtensionNode3D or GDExtensionObject
o.Call("do_thing", 1, "x");     o.Set("speed", 3.5);     o.Connect("pinged", Callable.From((long n) => ...));
```

- `GDExtensionManager`: `Load`, `IsLoaded`, `Unload`, `GetLoadedExtensions`, `SetApiPath`, `GetClassList`, `ClassExists`, `GetParentClass`, `CanInstantiate`, `GetMethodList/PropertyList/SignalList/Constants`, `GetUnsupportedCalls`, `Instantiate`, `LastError`, `IsNativeAvailable`. `ClassDB.ClassExists/CanInstantiate/Instantiate/GetParentClass/IsParentClass/GetClassList` include the loaded classes.
- `GDExtensionNode` / `GDExtensionNode3D` are ordinary Fabric nodes (the bridge declares them like any other node class). The Prop `extension_class` (`[Export] ExtensionClass`) names the native class; the native object is made when the node enters the tree (or at once through `ClassDB.Instantiate`). The extension's `_enter_tree`, `_ready`, `_exit_tree`, `_process`, `_physics_process` and notifications run from the node's hooks in Godot's order; `_process` is switched off for a class that does not implement it. The managed name and parent are mirrored into the native tree. For a `GDExtensionNode3D` the managed local transform is pushed to the extension before every call and pulled back when the extension changed it (`set_position` from native code moves the node). Children an extension adds natively (`add_child`) are wrapped and added to the managed tree.
- Methods and properties the C# node lacks are answered by the native class (`GodotObject.Call/Get/Set/HasMethod` fall through to it); a name that is also a C# member (`get_name`, `add_child`, `get_parent`) resolves to the C# one. A native call that fails throws `InvalidOperationException` with the host's message.
- The extension's signals are declared as signals of the node (`HasSignal`, `Connect`, `ToSignal`) and raised when the extension emits them. Connecting a native signal to a managed callable is one-way: a managed `EmitSignal` does not reach native handlers.
- `VariantWire.Encode/Decode/EncodeArguments/DecodeArguments` is the managed half of the wire form. Managed objects cannot cross (they travel as null); a GDExtension peer travels as its native instance id.

**For the scene converter (done in F6, `tools/fabric/FabricExtensions.cpp`, G3_SCENES.md "The whole conversion").** The converter copies each plugin's `.gdextension` and its libraries into `Content/` at their `res://` paths, declares them in `Fabric/project.json`, and writes a node of a class the plugin registers as a class placement `GDExtensionNode` (`GDExtensionNode3D` when a scene gives such a node a 3D transform) with `extension_class="TheClass"`, `_name`, the node's properties as Props (they reach the native class through `Set`), and its transform as usual. The plugin's classes are known only from the `.gdextension`'s `[icons]` section (a plugin with no icons is not recognised; its nodes stay carriers); whether a class is spatial is guessed from the scenes. At run time `GDExtensionManager.LoadProjectExtensions(contentDir?)` loads every `.gdextension` under `Paths.Content`, and `GdextPeer.Create` / `GDExtensionManager.Instantiate` call it once the first time a class is asked for that no loaded plugin has, so no loader node is placed. A plugin can also be loaded by hand with `GDExtensionManager.Load`.

## 8. Tests

- `GdextHostTest` (C++, ctest, 280 checks) over `FabricTestExt`: the loader's refusals (4.0, missing minimum, minimum above the host, maximum, entry symbol, entry failure, missing library, no library for the platform, missing file); loading by relative and `res://` path and loading twice; initialization levels CORE, SERVERS, SCENE and deinitialization in reverse; registration (4 classes, 15 methods, property, signal, integer constant, abstract class, a class registered through the 4.2 call); calls through the wire form, the pointer-call path and the variant-call path with error codes (too few, too many, wrong type, unknown method); property get/set; `Object.call`, `get_class`, `to_string`; the wire form round trip; the extension calling the host (method binds with hashes, builtin methods, constructors, members, operators, utility functions, `variant_evaluate`, `variant_stringify`: `self_test` = 255); signals (extension emits through `Object.emit_signal`, listener, custom callable, one-shot, deferred, `connect_method`); virtuals and notifications; enter/ready/exit order over a subtree; `queue_free`; extension-built child nodes with unique names; Node3D transforms, global composition, Euler round trip; `RefCounted` counting and `ref_set_object`; hash checks incl. `hash_compatibility`; **the host's 94 engine methods, 250 builtin methods, 56 utility functions and 156 constructors against `extension_api.json`**; Variant operators, conversion, hashing, text; String functions in five encodings; Array, Dictionary, Packed array pointer access, iteration, copy-on-write; every name of `gdextension_interface.h` served; the stubs for builtin methods, operators, utility functions and members the dump lists (and null for what it does not); unload (objects freed through the extension, classes gone); reload.
- `Aver.Fabric.Tests` `GdextTests` (C#, ctest `FabricManagedTests`, 89 checks): the wire form of every Variant type (and malformed input), then, when `build/bin/Aver.Fabric.GDExt.Abi` and the test plugin are built, `GDExtensionManager.Load`, `ClassDB.Instantiate`, `Call/Get/Set`, a native signal to a managed handler, the lifecycle inside the managed scene tree (enter/ready once, `_process` per frame, `_physics_process` per step, exit), a native child appearing in the managed tree, Node3D transform sync both ways, a RefCounted class, unload. The host DLL is copied beside the test assembly for the default DllImport probing. Without the native build those checks are skipped with a message, not failed.

## 9. Not verified

A real godot-cpp plugin. Linux and macOS (the host and the test extension compile on all three by construction, the test ran on Windows). Plugins that use `Control`/2D/rendering/physics classes, threads, script instances or the editor. Behaviour under the engine's tick (the C# peers were driven by the managed test loop, `SceneTree.RunManagedFrame`, not by the engine). The release build (the tests ran in Debug).
