# Aver Engine — The C ABI Seams

**"The Aver ABI" is a collective noun.** It names six *separate* C surfaces, one per module, each exported by its own DLL, each versioned on its own, each with its own export macro. There is no single seam, no `aver_abi_version()`, no `Aver.ABI` target — and there is not going to be. The seams stay separate by decision, because that is what buys the properties they exist for: `scene_abi.h` can be read end to end without meeting the word *actor* (`modules/scene/include/aver/scene/scene_abi.h:14-17`), and `Aver.Render.PBR` can be a P/Invoke DLL that knows nothing of the RHI (`modules/render.pbr/CMakeLists.txt:25-29`).

> **Three README files in this tree are stale and should be read as historical.** `abi/README.md:3` describes "Aver.ABI — stable `extern "C"` interop seam … Everything non-C++ binds here". `modules/abi/README.md:7-11` describes "The flat extern "C" seam: opaque handles, out-pointer returns, aver_abi_version(). The single interop boundary C#/Rust bind against", adding that it will be wired into the build "when Phase 7 implements it". Neither describes anything that exists: both directories contain exactly one file — that README — with no header, no source and no `CMakeLists.txt`, and neither is named by an `add_subdirectory` in the top-level `CMakeLists.txt` (verified by directory listing and by grep over the tree). No symbol named `aver_abi_version` exists anywhere; the only hits are those READMEs and `docs/ARCHITECTURE.md:158, 230, 303, 307`, which carries the same sketch and is stale the same way. `interop/README.md:3` promises "Generated P/Invoke (C#) and bindgen (Rust) bindings derived from abi/ headers"; that directory also holds only its README, the C# bindings under `scripting/csharp/` are hand-written, and a search for `*.rs` and `Cargo.toml` over the whole tree returns nothing — there is no Rust in this repository.

**190 exported C functions declared across six headers**, plus five reverse entry points the script host binds by name. Counts verified by grepping each header for its export macro at line start: scene 35, framework 46, framework hooks 4, physics 36, PBR 48, Voxi 21. Read that number as *declared in headers*: the shipped DLLs export 191, because `aver_scene_debug_string_pool_size` is `AVER_SCENE_ABI`-exported from `modules/scene/src/SceneAbi.cpp:111` and declared in no header (§3). A `dumpbin /exports` will therefore show one more than this document lists, and that is the one.

---

## Contents

1. [Which seam, and when](#1-which-seam-and-when)
2. [The six seams at a glance](#2-the-six-seams-at-a-glance)
3. [`Aver.Scene` — `scene_abi.h`](#3-averscene--scene_abih)
4. [`Aver.Framework` — `framework_abi.h`](#4-averframework--framework_abih)
5. [`Aver.Framework` — `framework_hooks.h` (not a P/Invoke surface)](#5-averframework--framework_hooksh-not-a-pinvoke-surface)
6. [`Aver.Physics` — `physics_abi.h`](#6-averphysics--physics_abih)
7. [`Aver.Render.PBR` — `pbr_abi.h`](#7-averrenderpbr--pbr_abih)
8. [`Aver.Render.Voxi` — `voxi_abi.h`](#8-averrendervoxi--voxi_abih)
9. [`Aver.Scripting` — `scripting_abi.h`](#9-averscripting--scripting_abih)
10. [The type rules](#10-the-type-rules)
11. [Handles and staleness](#11-handles-and-staleness)
12. [The error convention](#12-the-error-convention)
13. [Versioning, and the boundaries each version governs](#13-versioning-and-the-boundaries-each-version-governs)
14. [How a call reaches the DLL](#14-how-a-call-reaches-the-dll)
15. [Adding an entry point — the checklist](#15-adding-an-entry-point--the-checklist)
16. [Where the line between seams falls, and why](#16-where-the-line-between-seams-falls-and-why)
17. [Known gaps](#17-known-gaps)

---

## 1. Which seam, and when

Keyed on the job, not the module. If your job is here, the seam is decided.

| The job | Seam | Start at |
|---|---|---|
| **Spawn an actor** from C# or a tool | Framework | `aver_fw_spawn(class, name, pos3, quat4, scale3)` |
| Destroy an actor (full gameplay teardown) | Framework | `aver_fw_destroy(e)` |
| Ask whether an entity *is* an actor | Framework | `aver_fw_class_of(e)` — non-zero **is** "actor" |
| Declare a gameplay class, give it components, seal it | Framework | `aver_fw_class_declare` → `…_add_component` → `…_seal` |
| Give every instance of a class a starting value | Framework | `aver_fw_class_set_default_*` |
| Possess / release a pawn | Framework | `aver_fw_possess`, `aver_fw_unpossess` |
| Start or stop a play session; pause it | Framework | `aver_fw_begin_play`, `aver_fw_end_play`, `aver_fw_set_paused` |
| Push this frame's keyboard/mouse in | Framework | `aver_fw_input_new_frame` → `…_set_key` / `…_set_mouse` |
| Read whether a key went down this frame | Framework | `aver_fw_input_key_pressed(key)` |
| Publish which node the play camera follows | Framework | `aver_fw_set_view`, `aver_fw_set_view_entity` |
| **Create an entity** with no gameplay meaning (it is *not* bare — `CName`/`CLocal`/`CWorld`/`CHierarchy` are attached at birth, `scene_abi.h:141`) | Scene | `aver_scene_create()` |
| Read or write **a transform** | Scene | `aver_scene_field("CLocal.position")` → `aver_scene_get_vec` / `set_vec` |
| Get a **world** matrix (parents composed) | Scene | `aver_scene_world_matrix(e, out16)` |
| Read or write **any component field by name** | Scene | `aver_scene_field` → the typed family for its kind (and check it is not read-only, §3) |
| Attach a component; reparent; walk the tree | Scene | `aver_scene_add_component`, `…_set_parent`, `…_parent` / `…_first_child` |
| **Reparent** — always through this, never by writing `CHierarchy.parent` | Scene | `aver_scene_set_parent` (the field write is rejected, and §3 says what it would otherwise do) |
| Find a thing by name; sweep every entity | Scene | `aver_scene_find`, `aver_scene_count` + `aver_scene_at` |
| **Bind a material name onto a mesh renderer** | Scene | `aver_scene_material(pack, name)` — an interned token, **not** a PBR call |
| **Read or change a material parameter** | PBR | `aver_pbr_get_*` / `aver_pbr_set_*` on a material handle |
| Point a material's **texture slot** at an image | PBR | `aver_pbr_set_texture_path` / `aver_pbr_set_texture_id` |
| Make something transparent, cut-out, two-sided | PBR | `aver_pbr_set_alpha_mode`, `…_set_two_sided` |
| Upload changed material state to the GPU, once | PBR | `aver_pbr_consume_dirty` (reading **clears** it) |
| **Change GI quality** | Voxi | `aver_voxi_set_quality(AVER_VOXI_FEATURE_GLOBAL_ILLUMINATION, q)` |
| Change MSAA; ask what counts are accepted | Voxi | `aver_voxi_set_msaa`, `aver_voxi_msaa_mask` |
| Tune GI itself (resolution, intensity, reach) | Voxi | `aver_voxi_set_voxel_resolution`, `…_set_gi_intensity`, `…_set_gi_max_distance` |
| Ask what the **GPU** actually supports | Voxi | `aver_voxi_ray_tracing_tier`, `…_max_msaa`, `…_mesh_shader_tier`, `…_shader_model` |
| **Raycast**; sweep; overlap query | Physics | `aver_phys_raycast`, `aver_phys_sphere_cast`, `aver_phys_overlap_sphere` |
| Put a collider in the world | Physics | `aver_phys_add_static_box` … `aver_phys_add_heightfield` |
| Make something that **walks** | Physics | `aver_phys_character_create` and the `aver_phys_character_*` family |
| React to a collision or a trigger | Physics | `aver_phys_contact_count` / `…_get`, `aver_phys_overlap_count` / `…_get` |
| Step the simulation | Physics | `aver_phys_step(dt)` — host-side; not bound in C# |
| **Boot the CLR, load or hot-reload scripts** | Scripting | `Bootstrap` / `LoadScripts` / `UnloadScripts` / `Update` / `Shutdown` |
| Let managed actors receive begin / tick / end | Framework hooks | `aver_fw_install_managed_dispatch`, then `aver_fw_tick(group, dt)` |
| Check the DLLs beside you are the ones you built against | Scene + Framework | `aver_scene_abi_version`, `aver_fw_abi_version`, `aver_fw_scene_abi_matches` — **nothing in shipping code calls these**; see §13 |

**Wrong turns worth naming.** *Material by name* is a scene call, not a PBR call: `aver_scene_material` interns the name into a table local to the scene DLL and never touches the material library, because Aver.Scene must not link Aver.Render.PBR (`scene_abi.h:188-193`). *A character's position* is a physics handle, not a scene entity — they are different handle families and the gameplay layer above keeps them in step. *Feature/status introspection* exists twice, once per render seam (`aver_pbr_feature_*`, `aver_voxi_feature_*`), and they describe different feature sets. *Structural hierarchy edits* are not field writes: `CHierarchy.parent` is read-only over the generic field API and the write is rejected — see §3 for what it would do if it were not.

---

## 2. The six seams at a glance

| Seam | Header | Export macro / build define | Library | Exports | Version | Handles | Direct C ABI test |
|---|---|---|---|---|---|---|---|
| **Aver.Scene** | `modules/scene/include/aver/scene/scene_abi.h` | `AVER_SCENE_ABI` / `AVER_SCENE_BUILD` | SHARED | 35 | **1.0** | generational entity `int32_t` | `tests/scene/src/SceneTest.cpp` |
| **Aver.Framework** | `modules/framework/include/aver/framework/framework_abi.h` | `AVER_FW_ABI` / `AVER_FW_BUILD` | SHARED | 46 | **1.1** | class (non-generational) + entity | `tests/framework/src/FrameworkTest.cpp` |
| **Aver.Framework (hooks)** | `…/framework/framework_hooks.h` | `AVER_FW_ABI` (+ `AVER_FW_CALL` = `__cdecl`) | same DLL | 4 | table contracts **2** / **1** | by-value tables, no handles | partly, in `FrameworkTest.cpp` |
| **Aver.Physics** | `modules/physics/include/aver/physics/physics_abi.h` | `AVER_PHYS_API` / `AVER_PHYS_BUILD` | SHARED | 36 | **none declared** | dense `int32_t`, never reissued | `tests/physics/src/PhysicsTest.cpp` |
| **Aver.Render.PBR** | `modules/render.pbr/include/aver/pbr/pbr_abi.h` | `AVER_PBR_ABI` / `AVER_PBR_BUILD` | SHARED | 48 | **none declared** | generational material `int32_t` | **none** |
| **Aver.Render.Voxi** | `modules/render.voxi/include/aver/voxi/voxi_abi.h` | `AVER_VOXI_ABI` / `AVER_VOXI_BUILD` | SHARED | 21 | **none declared** | **none** — global settings | **none** |
| **Aver.Scripting** | `modules/scripting/include/aver/scripting/scripting_abi.h` | **no export macro at all** | STATIC (`Aver.Scripting.Host`) | 0 exported; 5 bound by name | contract **2** | none — one blittable struct | **none** |

Every export macro follows the same shape: `__declspec(dllexport)` under the module's `*_BUILD` define, `__declspec(dllimport)` otherwise, empty off `_WIN32` (`scene_abi.h:30-38`; `framework_abi.h:36-44`; `physics_abi.h:10-18`; `pbr_abi.h:29-37`; `voxi_abi.h:16-24`). The `*_BUILD` define is set `PRIVATE` on the module target and a `AVER_MODULE_*=1` define is published `PUBLIC` to consumers (`modules/scene/CMakeLists.txt:24-26`; `modules/framework/CMakeLists.txt:20-22`; `modules/physics/CMakeLists.txt:16-17`; `modules/render.pbr/CMakeLists.txt:17-19`; `modules/render.voxi/CMakeLists.txt:14-16`).

Two spellings break the pattern, harmlessly but worth knowing: physics uses `AVER_PHYS_API`, not `_ABI` (`physics_abi.h:12`); and the scripting module's CMake target is `Aver.Scripting.Host`, built STATIC because `aver_add_module()` only makes static libraries (`cmake/AvModule.cmake:14`).

---

## 3. `Aver.Scene` — `scene_abi.h`

**Header** `modules/scene/include/aver/scene/scene_abi.h` · **DLL** `Aver.Scene` (SHARED, links `PUBLIC Aver.Core Aver.Assets` — `modules/scene/CMakeLists.txt:12, 21`) · **Version** `AVER_SCENE_ABI_VERSION` = `(1 << 16) | 0` (`scene_abi.h:54-57`) · **35 entry points.**

**Handle.** A `int32_t` entity. The C++ side is `aver::scene::Entity = AvId`: index in bits 0–23, generation in bits 24–30, bit 31 always clear (`Entity.hpp:23-42`). Index 0 is never handed out and a live generation starts at 1, so no live handle is 0 and every live handle crosses as a **positive** `int32_t`. A `static_assert` pins the bit-31 rule at compile time (`Entity.hpp:48-49`). Field ids and component ids are also `int32_t` with `0 == invalid`.

**Staleness.** Generational. `entityGen(e)` (`Entity.hpp:38`) is compared against the slot's generation; `aver_scene_valid` reports 1 only for a live handle and rejects 0, a stale generation and a freed slot (`scene_abi.h:145-146`). Seven generation bits is few, so the design handles the wrap rather than asserting it away: the free list is FIFO and a slot whose generation would wrap past `kEntityMaxGen` is **retired** rather than recycled (`Entity.hpp:44-47`). Destroy is deferred to the next flush and takes the whole subtree; the handle stays valid for the rest of the frame (`scene_abi.h:143-144`). Every accessor funnels its rejections through one guard, `fieldAddr`, which relies on `getComponent` already returning `nullptr` for a stale handle via the pool's owner-check (`modules/scene/src/SceneAbi.cpp:56-64`).

The `AVER_SCENE_KIND_*` and `AVER_SCENE_COMP_*` constants (`scene_abi.h:70-92`) are not merely documented as matching the C++ enums — they are pinned by `static_assert` in `modules/scene/src/SceneAbi.cpp:25-42`, one per constant. This is the only seam that does that, and even here the pinning is C++-to-C only: nothing pins those same constants to their C# mirrors (§17).

### Version and staleness of the binary itself
*You are bootstrapping and want to know whether the DLL beside the executable is the one your bindings were built against, before a mismatch becomes a wrong pointer rather than a missing symbol.*

```c
AVER_SCENE_ABI int32_t aver_scene_abi_version(void);
```

### Field resolution (name → dense field id)
*You know the name of the thing you want ("CLocal.position") and need its id, its kind, and how many floats it takes before you can touch it.*

```c
AVER_SCENE_ABI int32_t aver_scene_field(const char* qualifiedName);   /* 0 when unknown */
AVER_SCENE_ABI int32_t aver_scene_field_kind(int32_t f);              /* AVER_SCENE_KIND_*, or 0 (== F32) for an unknown id */
AVER_SCENE_ABI int32_t aver_scene_field_arity(int32_t f);             /* floats per value; 0 for non-float kinds */
```

**Read that middle comment carefully — it is the header's own wording (`scene_abi.h:112`) and it is a trap.** `aver_scene_field_kind` returns 0 both for an unknown id *and* for every legitimate `F32` field, because `AVER_SCENE_KIND_F32` **is** 0. It is therefore useless as a validity test: using it as one misclassifies every float field in the engine as unknown. **The only valid test is `aver_scene_field(name) != 0`** — resolve once, at bind time, and keep the id.

### Typed get/set, one family per kind
*You want to read or change a value on an entity — a position, a name, a mesh id, a reference to another entity — and you want a wrong-kind write **rejected** rather than silently reinterpreted. That kind check is the whole reason there is no generic "set bytes" (`scene_abi.h:117-126`).*

```c
AVER_SCENE_ABI float   aver_scene_get_f32(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_f32(int32_t e, int32_t f, float v);
AVER_SCENE_ABI int32_t aver_scene_get_vec(int32_t e, int32_t f, float* outv);
AVER_SCENE_ABI int32_t aver_scene_set_vec(int32_t e, int32_t f, const float* v);
AVER_SCENE_ABI int32_t aver_scene_get_i32(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_i32(int32_t e, int32_t f, int32_t v);
AVER_SCENE_ABI int64_t aver_scene_get_i64(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_i64(int32_t e, int32_t f, int64_t v);
AVER_SCENE_ABI int32_t aver_scene_get_ref(int32_t e, int32_t f);
AVER_SCENE_ABI int32_t aver_scene_set_ref(int32_t e, int32_t f, int32_t v);
AVER_SCENE_ABI const char* aver_scene_get_str(int32_t e, int32_t f);   /* "" for stale/wrong-kind */
AVER_SCENE_ABI int32_t     aver_scene_set_str(int32_t e, int32_t f, const char* v);
```

`vec` serves every float kind — F32 = 1, VEC3 = 3, QUAT = 4, MAT4 = 16 floats — and the buffer must hold `aver_scene_field_arity(f)` floats. An entity reference is its own kind, not an `int32` in disguise, which is why `ref` exists beside `i32`. Writing any `CLocal` field bumps the transform revision so the world-matrix pass sees it (`scene_abi.h:117-126`).

### Read-only fields — the third rejection cause, and the one nobody expects
*You resolved a field, the handle is live, the kind is right, and the setter still returns 0. This is why.*

**Every scene setter rejects a field flagged read-only**, in the same expression as the kind check: `set_f32` (`modules/scene/src/SceneAbi.cpp:146`), `set_vec` (`:169`), `set_i32` (`:193`), `set_i64` (`:215`), `set_ref` (`:243`), `set_str` (`:268`). Getters are unaffected — a read-only field reads normally.

The flag is registered where the built-ins are declared, `modules/scene/src/Builtins.cpp:44, 52, 63, 76, 93`, and covers the world's own bookkeeping: `CLocal.rev` (`:44`), `CWorld.matrix` and the world revisions (`:52` — the source comment beside `set_vec` names it, "CWorld.matrix is a float kind but read-only"), `CHierarchy`'s structural links and depth (`:63`), `CName.offset`/`len` (`:76`), and `CMeshRenderer.dirty` (`:93`). `modules/scene/README.md:15-16` states the rule in prose: internal bookkeeping fields — name-blob cursors, `CWorld` derived data, hierarchy links — are read-only over the generic ABI.

> **The `CHierarchy` case is a hang, not a tidiness rule.** `SceneAbi.cpp:240-242` records exactly what the flag prevents: `World::setParent` keeps the hierarchy acyclic — it refuses self-parenting and walks the ancestor chain — and a raw byte write through `set_ref` bypasses all of it. A single `set_ref(e, CHierarchy.parent, e)`, or a pair of writes forming a two-cycle, "makes `composeChain()`/`worldMatrix()` loop with no visited guard and grow unbounded (hang/`bad_alloc`)". There is no visited guard in the walk, and the read-only flag is what stands in for one. **Structural edits go through `aver_scene_set_parent`**; `get_ref` on those fields is a read and is unaffected. A binding that "helpfully" exposes generic field writes over the whole field table without honouring the rejection is handing scripts a way to hang the process.

There is no ABI entry point that reports whether a field is read-only. A binding cannot ask; it can only observe the 0 and must not treat it as a stale handle.

### Entity lifetime and hierarchy editing
*You are making, unmaking or re-parenting something, or checking whether a handle you kept from last frame still addresses anything.*

```c
AVER_SCENE_ABI int32_t aver_scene_create(void);
AVER_SCENE_ABI int32_t aver_scene_destroy(int32_t e);
AVER_SCENE_ABI int32_t aver_scene_valid(int32_t e);
AVER_SCENE_ABI int32_t aver_scene_add_component(int32_t e, int32_t component);
AVER_SCENE_ABI int32_t aver_scene_set_parent(int32_t child, int32_t parent);
```

`create` returns an **unnamed** entity that already carries `CName`, `CLocal`, `CWorld` and `CHierarchy` — they are "attached at birth" (`scene_abi.h:141`), and 0 comes back if none could be made. Do not add them again; a fresh entity is already transformable, nameable and parentable. `add_component` is documented **idempotent**: it returns 1 for a component that is already present, and 0 only for a bad type or a bad handle (`scene_abi.h:147`), so a re-add is cheap rather than an error. `set_parent` refuses a cycle, a self-parent, or a doomed parent; parent 0 makes the child a root (`scene_abi.h:149-150`).

### Persisted identity and name
*You need the identity that survives a save/load round-trip, or the human-readable name the outliner shows — both reached here rather than as component fields, because `CName.objectId` has no field accessor, and `CName.offset`/`len` are read-only (`scene_abi.h:152-153`; `Builtins.cpp:76`).*

```c
AVER_SCENE_ABI int64_t aver_scene_object_id(int32_t e);               /* 0 for a stale handle */
AVER_SCENE_ABI int32_t aver_scene_set_object_id(int32_t e, int64_t objectId);
AVER_SCENE_ABI const char* aver_scene_name(int32_t e);                /* "" for a stale handle */
AVER_SCENE_ABI int32_t     aver_scene_set_name(int32_t e, const char* name);
```

### Query: find by name, world matrix, component test
*You have a name and want the thing; or you need where something actually is after its parents have had their say.*

```c
AVER_SCENE_ABI int32_t aver_scene_find(const char* name);
AVER_SCENE_ABI int32_t aver_scene_world_matrix(int32_t e, float* out16);
AVER_SCENE_ABI int32_t aver_scene_has_component(int32_t e, int32_t component);
```

`world_matrix` writes row-major, row-vector: basis in rows 0–2, translation in row 3, composed on demand if `e` is stale relative to its parents; it returns 0 and leaves `out16` untouched for a dead handle or a null pointer (`scene_abi.h:163-166`). `find` is a linear scan and is documented as a convenience for tools and scripts, not a per-frame lookup.

### Hierarchy queries (read side)
*You are walking a subtree — drawing an outliner, propagating something to children, finding an ancestor.*

```c
AVER_SCENE_ABI int32_t aver_scene_parent(int32_t e);
AVER_SCENE_ABI int32_t aver_scene_first_child(int32_t e);
AVER_SCENE_ABI int32_t aver_scene_next_sibling(int32_t e);
AVER_SCENE_ABI int32_t aver_scene_child_count(int32_t e);
```

### Enumeration over live entities
*You want to sweep everything once, inside a single frame.*

```c
AVER_SCENE_ABI int32_t aver_scene_count(void);
AVER_SCENE_ABI int32_t aver_scene_at(int32_t index);
```

Indices are dense over live entities and **shift on the next flush**, so a `(count, at)` walk is valid only within the frame it is taken (`scene_abi.h:182-183`).

### Content resolution at bind time
*You are attaching a material to a mesh renderer from a name and need the opaque token the render side will later map back.*

```c
AVER_SCENE_ABI int32_t aver_scene_material(int32_t name0, const char* name);
```

`name0` is the content-pack id (0 = default pack). Materials belong to `Aver.Render.PBR`, which this module must not link, so this is a pure per-name intern into a table local to the scene DLL and **never** a call into the material library (`scene_abi.h:188-193`). What that token means once the PBR material it names has been destroyed is not stated anywhere — see §17.

### One export not declared in this header
`aver_scene_debug_string_pool_size()` is defined at `modules/scene/src/SceneAbi.cpp:111` and exported, but declared in no header. It exists so a same-process test can prove that repeated `set_str` into one field reuses its slot instead of leaking a pool entry per write; `tests/scene/src/SceneTest.cpp:22` declares it itself with `__declspec(dllimport)`. It is not part of the 35, it is the reason a `dumpbin /exports` shows 191 rather than 190, and nothing else should call it.

---

## 4. `Aver.Framework` — `framework_abi.h`

**Header** `modules/framework/include/aver/framework/framework_abi.h` · **DLL** `Aver.Framework` (SHARED, links `PUBLIC Aver.Core Aver.Assets Aver.Scene` — `modules/framework/CMakeLists.txt:12, 17`) · **Version** `AVER_FW_ABI_VERSION` = `(1 << 16) | 1` (`framework_abi.h:53-58`) · **46 entry points.**

The minor is 1 because `aver_fw_set_view_entity` / `aver_fw_view_entity` were added; the header records the justification in place — additive only, every 1.0 entry point unchanged in shape and meaning (`framework_abi.h:54-55`).

**A class is data.** There is no C++ base type behind a class handle and no virtual dispatch behind a spawn: a class is a registry row holding a flattened component list and one contiguous blob of defaults, and spawning is a loop of `memcpy` over that blob into the scene's pools (`framework_abi.h:80-84`).

**Handles — two mechanisms, not interchangeable.** Three `int32_t` families, all with `0 == invalid` (`framework_abi.h:93-95`); the typedefs `aver_class` / `aver_entity` / `aver_field` are documentation only, because every exported signature spells `int32_t` so a C# `[DllImport]` declaring `int` binds with no marshalling surprises (`framework_abi.h:90-91`).

- **Class handles are not generational.** Classes live in a `std::deque` that only grows, index 0 is a reserved dummy, and validity is a bounds test: `bool validClass(int32_t c) { return c > 0 && static_cast<usize>(c) < classes().size(); }` (`modules/framework/src/FrameworkAbi.cpp:169`). A class handle is therefore stable for the life of the process, which is exactly what makes it hot-reload identity (`framework_abi.h:114-118`). A deque rather than a vector because `aver_fw_class_name` hands back a `c_str()` into a record, and a reallocating vector would dangle every such pointer (`FrameworkAbi.cpp:152-155`).
- **Entity handles are the scene's**, checked through the scene with `world().valid(e)`. The entity→class side map stores the **owning** entity handle beside the class, so `class_of` returns non-zero only when the recorded owner equals the queried handle: `return (v[idx].owner == e && world().valid(e)) ? v[idx].cls : 0;` (`FrameworkAbi.cpp:213`). The owner match closes the reused-index hazard; the `valid()` check closes the stale-but-not-reused one, including for a child destroyed through its parent's subtree.

### Version, and the cross-DLL scene check
*You are bootstrapping and want a message rather than a crash when the framework DLL and the scene DLL beside it were built against different majors.*

```c
AVER_FW_ABI int32_t aver_fw_abi_version(void);
AVER_FW_ABI int32_t aver_fw_scene_abi_version(void);
AVER_FW_ABI int32_t aver_fw_scene_abi_matches(void);
```

`aver_fw_scene_abi_version` deliberately returns the **header** constant this binary compiled against (`FrameworkAbi.cpp:365-370`). `aver_fw_scene_abi_matches` deliberately **calls across** into Aver.Scene, comparing majors only, because that is the only way to learn what is really loaded (`FrameworkAbi.cpp:372-378`). Keeping that call is load-bearing twice over: without it the check becomes a tautology, and the header records that on this module's first build the framework's only reference to the scene was a header constant, so the linker emitted no import and `dumpbin /dependents` showed no edge at all (`framework_abi.h:70-77`). Note that premise is weaker today than when it was written: `FrameworkAbi.cpp:13-15` includes `World.hpp` / `Components.hpp` / `Fields.hpp`, and `FrameworkAbi.cpp:36` calls into the scene directly — `World& world() { return World::instance(); }` — and those C++ types are exported via `AVER_SCENE_API` (`Entity.hpp:9-17`, applied at `World.hpp:24`), so the import edge would survive even if this function were gutted.

### Class registry
*You are declaring what a kind of actor **is** — its name, parent, components, whether it ticks, whether it is a pawn — typically once per assembly load, and again after a hot reload.*

```c
AVER_FW_ABI int32_t aver_fw_class_declare(const char* name, const char* parentName);
AVER_FW_ABI int32_t aver_fw_class_find(const char* name);            /* 0 when unknown */
AVER_FW_ABI const char* aver_fw_class_name(int32_t c);               /* "" for an invalid handle */
AVER_FW_ABI int32_t aver_fw_class_parent(int32_t c);                 /* 0 for a root */
AVER_FW_ABI int32_t aver_fw_class_reset(int32_t c);
AVER_FW_ABI int32_t aver_fw_class_add_component(int32_t c, int32_t component);
AVER_FW_ABI int32_t aver_fw_class_set_flags(int32_t c, int32_t flags);
AVER_FW_ABI int32_t aver_fw_class_get_flags(int32_t c);
AVER_FW_ABI int32_t aver_fw_class_set_tick(int32_t c, int32_t tickGroup, int32_t tickOrder);
AVER_FW_ABI int32_t aver_fw_class_seal(int32_t c);
```

`declare` is **idempotent by name**: the same name returns the same handle for the life of the process, which is the whole of hot-reload identity — a rebuilt assembly redeclares its class, gets back the handle its live entities already store, and only the descriptor behind it is rewritten (`framework_abi.h:114-118`). `seal` flattens the parent chain into the resolved archetype and returns 0 on a cycle or a named-but-undeclared parent; spawning auto-seals, so a caller that forgets is slow once, not wrong (`framework_abi.h:128-131`). Flags are `AVER_FW_CLASS_*` (`framework_abi.h:99-106`); tick groups are `AVER_FW_TICK_*` (`:109-112`). Both blocks carry a comment saying they are "pinned to" a C# enum (`:97`, `:108`) — read that as *intended to agree with*, because nothing checks it (§17).

### Class defaults (the archetype blob)
*You want every instance of a class to start with a given value — health 100, a mesh, a colour — without touching any entity that already exists.*

```c
AVER_FW_ABI int32_t aver_fw_class_set_default_f32(int32_t c, int32_t f, float v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i32(int32_t c, int32_t f, int32_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i64(int32_t c, int32_t f, int64_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_vec(int32_t c, int32_t f, const float* v);
AVER_FW_ABI int32_t aver_fw_class_set_default_str(int32_t c, int32_t f, const char* v);
```

Five setters, one per storable kind, addressed by the **same** dense field id the scene resolves. There is no `set_default_bool` (a bool rides an `i32`) and no `set_default_ref` (an entity default is meaningless in an archetype — it is per-instance). The kind is validated here; a wrong-kind default is rejected with 0 and stored nowhere, and a default lands in the class row's blob, never on a live entity (`framework_abi.h:133-137`).

### GameMode wiring by class name
*You are saying which pawn and which controller a game mode spawns, without either game class taking a compile-time reference to the other.*

```c
AVER_FW_ABI int32_t aver_fw_class_set_default_pawn(int32_t gameMode, const char* pawnClassName);
AVER_FW_ABI int32_t aver_fw_class_set_player_controller(int32_t gameMode, const char* controllerClassName);
```

Resolved by name at seal (`framework_abi.h:144-147`).

### Actors: spawn, destroy, class identity
*You want a thing to exist in the world — this is the "spawn an actor" entry point — or you have an entity and want to know whether it is an actor at all.*

```c
AVER_FW_ABI int32_t aver_fw_spawn(int32_t c, const char* name,
                                  const float* pos3, const float* quat4, const float* scale3);
AVER_FW_ABI int32_t aver_fw_destroy(int32_t e);
AVER_FW_ABI int32_t aver_fw_class_of(int32_t e);   /* the entity's class, or 0 — != 0 IS "actor" */
```

Rotation crosses as a **quaternion** though the author writes degrees higher up; a null `pos3`/`quat4`/`scale3` means "use the class default" (`framework_abi.h:149-156`).

### Possession
*You are handing control of a pawn to a controller, taking it away, or asking who drives what.*

```c
AVER_FW_ABI int32_t aver_fw_possess(int32_t controller, int32_t pawn);
AVER_FW_ABI int32_t aver_fw_unpossess(int32_t controller);
AVER_FW_ABI int32_t aver_fw_controlled_pawn(int32_t controller);   /* the pawn, or 0 */
AVER_FW_ABI int32_t aver_fw_controller_of(int32_t pawn);           /* the controller, or 0 */
```

Rejected unless the controller's class carries `CONTROLLER` and the pawn's carries `PAWN`. That flag check **is** the whole of the type safety here, which is why the base types set the flags for you (`framework_abi.h:160-162`).

### Play lifecycle and session singletons
*You are the Play button: starting or stopping a session, pausing it, or asking for the GameMode / GameInstance / player controller the session spawned.*

```c
AVER_FW_ABI int32_t aver_fw_begin_play(int32_t gameInstanceClass, int32_t gameModeClass);
AVER_FW_ABI int32_t aver_fw_end_play(void);
AVER_FW_ABI int32_t aver_fw_set_paused(int32_t paused);
AVER_FW_ABI int32_t aver_fw_find_class_with_flags(int32_t flags);
AVER_FW_ABI int32_t aver_fw_game_instance(void);
AVER_FW_ABI int32_t aver_fw_game_mode(void);
AVER_FW_ABI int32_t aver_fw_player_controller(int32_t playerIndex);
AVER_FW_ABI int32_t aver_fw_play_state(void);
```

`gameModeClass` is mandatory (0 rejects); `gameInstanceClass` is optional. States are `AVER_FW_PLAY_EDITOR` / `_PLAYING` / `_PAUSED` (`framework_abi.h:174-176`); pausing freezes the tick without tearing anything down. Every singleton is 0 in EDITOR. `find_class_with_flags` returns the first declared class carrying **all** of `flags`, which is how the editor's Play button finds a GameMode without a hard-coded name; 0 flags returns 0 (`framework_abi.h:188-190`). The C# view of this lifecycle — ordering, and what fires when — is `docs/SCRIPTING_API.md` §3.

### Input (the app pushes, gameplay reads)
*You are the application feeding this frame's keyboard and mouse in, or a script asking whether the jump key went down. The framework holds no window, so raw input arrives here — and keeping the key codes here rather than in the app is what lets a script name a key without depending on the editor (`framework_abi.h:199-204`).*

```c
AVER_FW_ABI void    aver_fw_input_new_frame(void);
AVER_FW_ABI void    aver_fw_input_set_key(int32_t key, int32_t down);
AVER_FW_ABI void    aver_fw_input_set_mouse(float dx, float dy, float wheel);
AVER_FW_ABI int32_t aver_fw_input_key(int32_t key);
AVER_FW_ABI int32_t aver_fw_input_key_pressed(int32_t key);
AVER_FW_ABI int32_t aver_fw_input_key_released(int32_t key);
AVER_FW_ABI void    aver_fw_input_mouse(float* out3);   /* {dx, dy, wheel} */
```

Call `new_frame` **once** per frame, before the `set_key` calls, so pressed/released are edges. Key codes are the anonymous enum at `framework_abi.h:205-218`, mirrored by hand in `scripting/csharp/Aver.Framework/Input.cs:7-15`; out-of-range keys are ignored on write and read 0.

### Play view (what the play camera should follow)
*You are a possessed character publishing the camera you want, or the editor asking where to put the play view this frame.*

```c
AVER_FW_ABI void aver_fw_set_view(int32_t mode, float eyeHeight, float boomLength);
AVER_FW_ABI void aver_fw_view(int32_t* outMode, float* outEyeHeight, float* outBoomLength);
AVER_FW_ABI void    aver_fw_set_view_entity(int32_t entity);
AVER_FW_ABI int32_t aver_fw_view_entity(void);
```

Modes are `AVER_FW_VIEW_FIRST_PERSON` / `_THIRD_PERSON` (`framework_abi.h:237-238`). The view-entity pair, added at minor 1, exists so the editor can **read** a transform instead of reconstructing one from the pawn's world matrix; the header records the concrete bug that motivated it — a held item swinging on an arc of the character's own height while the camera, pinned along world-up, did not move at all (`framework_abi.h:242-257`). 0 means "no view node published" and the caller falls back to the pawn-matrix path. The handle is a scene entity id, valid only while the scene says so: check `aver_scene_valid` before use, because a pawn can be destroyed between the publish and the read. These two are the newest entry points on this seam and are **not** driven by any test — see §17.

---

## 5. `Aver.Framework` — `framework_hooks.h` (not a P/Invoke surface)

**Header** `modules/framework/include/aver/framework/framework_hooks.h` · same DLL as `framework_abi.h` · **4 exported entry points** plus the function-pointer types the tables carry.

This is the other half of the framework's C surface, split off deliberately. `framework_abi.h` is a P/Invoke surface where nothing but `int32_t` / `int64_t` / `float` / `const char*` crosses. This file holds **C function pointers** and the structs carrying them — the dispatch tables gameplay code installs so the framework can call *up* into it. **No managed code marshals this struct field by field**: the bridge builds one table of native thunks and installs it once (`framework_hooks.h:6-17`). It is the one place in the engine where function pointers are allowed.

Every pointer carries `AVER_FW_CALL`, which is `__cdecl` on `_WIN32` and empty elsewhere (`framework_hooks.h:36-40`), because the bridge compiles its thunks `CallConvCdecl` and a mismatch would corrupt the stack on the first call.

**Contract versions.** `AVER_FW_VTABLE_VERSION` is 1 (`:85`) and `AVER_FW_DISPATCH_VERSION` is 2 (`:128`). Both are **flat integers**, not `(major << 16) | minor` pairs like the two module ABIs, and both open their struct with `int32_t structBytes` then `int32_t contractVersion` — the `AverScriptHostApi` idiom rather than the `pbr_abi.h` one (`framework_hooks.h:18-21`).

**Enforcement, and it is real code.** `aver_fw_install_managed_dispatch` validates `structBytes` **and** `contractVersion` and rejects a mismatch with a logged error (`modules/framework/src/FrameworkAbi.cpp:767-778`); it then refuses a **second** non-null install while one is live, logging so a second wirer is found rather than silently winning or losing (`FrameworkAbi.cpp:782-786`). That refusal is the *only* enforcement of "only the executable may wire the bridge" — the header says so plainly, because nothing in the build can enforce it (`framework_hooks.h:161-164`). The table is stored **by value** (`FrameworkAbi.cpp:787`) and cleared to a null store on unload (`:792-798`), so native state can never hold a function pointer owned by the collectible `AssemblyLoadContext`; after a clear the tick ticks nothing rather than faulting, because every call site already guards the pointer.

### Installing and clearing the managed dispatch table
*You are the executable, right after the script host has bootstrapped, wiring the C# bridge's thunks into the framework — or tearing them out before an assembly load context is unloaded.*

```c
AVER_FW_ABI int32_t aver_fw_install_managed_dispatch(const AvManagedDispatch* d);
AVER_FW_ABI int32_t aver_fw_clear_managed_dispatch(void);
AVER_FW_ABI int32_t aver_fw_managed_dispatch_installed(void);   /* 1 while a table is live */
```

### Driving the tick
*You are the host or app advancing gameplay for one tick group this frame.*

```c
AVER_FW_ABI int32_t aver_fw_tick(int32_t tickGroup, float dt);
```

If a managed dispatch is installed this makes **exactly one** `tick_all(group, dt)` call and the bridge walks its own dense list on the managed side (`FrameworkAbi.cpp:804-814`). A reverse-P/Invoke plus a per-actor dictionary lookup once per actor per frame is a cost this design would never be able to argue away (`framework_hooks.h:100-108`). Returns 1 if a managed `tick_all` fired, else 0.

### The two tables

```c
typedef struct AvActorVTable {          /* ONE per CLASS, never per object */
    int32_t                  structBytes;
    int32_t                  contractVersion;   /* AVER_FW_VTABLE_VERSION */
    void*                    user;              /* opaque per-class cookie; NULL for a managed class */
    aver_fw_vt_begin_play_fn beginPlay;
    aver_fw_vt_tick_fn       tick;
    aver_fw_vt_end_play_fn   endPlay;
} AvActorVTable;                                            /* framework_hooks.h:91-98 */

typedef struct AvManagedDispatch {
    int32_t                 structBytes;
    int32_t                 contractVersion;   /* AVER_FW_DISPATCH_VERSION == 2 */
    aver_fw_bind_fn         bind;
    aver_fw_unbind_fn       unbind;
    aver_fw_begin_play_fn   beginPlay;
    aver_fw_tick_all_fn     tick_all;
    aver_fw_end_play_fn     endPlay;
    aver_fw_rebound_fn      rebound;
    aver_fw_build_models_fn build_models;
    aver_fw_possessed_fn    possessed;      /* v2 */
    aver_fw_unpossessed_fn  unpossessed;    /* v2 */
    aver_fw_post_login_fn   post_login;     /* v2 */
} AvManagedDispatch;                                        /* framework_hooks.h:144-157 */
```

Note the contrast: the vtable's pointers take a `user` cookie, the managed table's do not — they route by entity handle, class-name hash or tick group, because the single table serves *every* managed class (`framework_hooks.h:106-108`).

### The function-pointer types
*You are implementing the bridge side and need the exact shape of each thunk.*

```c
typedef void (AVER_FW_CALL* aver_fw_vt_begin_play_fn)(void* user, aver_entity e, int32_t reason);
typedef void (AVER_FW_CALL* aver_fw_vt_tick_fn)      (void* user, aver_entity e, float   dt);
typedef void (AVER_FW_CALL* aver_fw_vt_end_play_fn)  (void* user, aver_entity e, int32_t reason);

typedef int32_t (AVER_FW_CALL* aver_fw_bind_fn)        (int64_t classNameHash, aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_unbind_fn)      (aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_begin_play_fn)  (aver_entity e, int32_t reason);
typedef void    (AVER_FW_CALL* aver_fw_tick_all_fn)    (int32_t tickGroup, float dt);
typedef void    (AVER_FW_CALL* aver_fw_end_play_fn)    (aver_entity e, int32_t reason);
typedef void    (AVER_FW_CALL* aver_fw_rebound_fn)     (aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_build_models_fn)(aver_entity e);
typedef void    (AVER_FW_CALL* aver_fw_possessed_fn)   (aver_entity pawn, aver_entity controller);
typedef void    (AVER_FW_CALL* aver_fw_unpossessed_fn) (aver_entity pawn);
typedef void    (AVER_FW_CALL* aver_fw_post_login_fn)  (aver_entity gameMode, aver_entity controller);
```

Begin/end reasons are `AVER_FW_BEGIN_SPAWN` / `_PLAY` / `_RELOAD` and `AVER_FW_END_DESTROY` / `_STOP` / `_RELOAD` / `_TRAVEL` (`framework_hooks.h:58-65`). The asymmetry is deliberate: `Travel` has no begin counterpart because the arriving level's actors spawn fresh. The header states at `:51-53` that these pin "integer for integer" to `Aver.Framework`'s `BeginReason`/`EndReason` (`Enums.cs`) — again, an intention, not a check (§17).

> **`AvActorVTable` is PLANNED, not built.** The header states it: the registrar `aver_fw_class_set_vtable` and the native per-class tick loop are a documented follow-up, and "no native class registers a vtable yet, so nothing reads this table" (`framework_hooks.h:80-83`). The struct is declared only because both sides must agree on its shape before either implements it.
>
> **Where the registrar is named.** The only reference in *code* is that comment, `framework_hooks.h:82` — no header declares it and no translation unit defines it. It is, however, named four times in the documentation, and one of those names a contradiction worth knowing before anyone implements it: `docs/SCENE_FRAMEWORK.md:1223` carries the planned declaration, `AVER_FW_ABI aver_class aver_fw_class_set_vtable(aver_class c, const AvActorVTable* vt);` — a **handle** return, where the tree-wide convention is `int32_t` 1/0 (§12). `docs/DESIGNER_REWRITE.md:183` already flags exactly that ("the header should return `int32_t` 1/0 to match the convention") and records that the C# surface binds no vtable setter at all. `docs/SCENE_FRAMEWORK.md:1876` and `:2122` discuss it further. Whoever builds it should build the corrected signature, not the one written down.

---

## 6. `Aver.Physics` — `physics_abi.h`

**Header** `modules/physics/include/aver/physics/physics_abi.h` · **DLL** `Aver.Physics` (SHARED, `PUBLIC Aver.Core`, `PRIVATE Jolt` — `modules/physics/CMakeLists.txt:9, 14`) · **No version macro and no version entry point** · **36 entry points.**

Jolt is linked **private** on purpose: it is an implementation detail, and nothing above should be able to include a `JPH::` header by accident, because the moment something does, swapping the backend stops being a decision about this module and becomes a decision about the whole tree (`modules/physics/CMakeLists.txt:5-8`). Everything here is in the **engine's** contract — centimetres, +X forward, +Y right, +Z up, left-handed — and no caller ever sees a Jolt type, a metre or a +Y-up vector; the translation happens once behind this boundary (`physics_abi.h:5-7`).

**Handles.** `int32_t` for both bodies and characters, drawn from **one** counter so the two families never collide: `int32_t nextHandle = 1;` (`modules/physics/src/PhysicsWorld.cpp:133`). 0 is always invalid. Handles are looked up in `std::unordered_map<int32_t, JPH::BodyID> bodies` and `<int32_t, JPH::Ref<JPH::CharacterVirtual>> characters` (`PhysicsWorld.cpp:131-133`). Jolt's own `BodyID` is a packed index+generation that would satisfy the 0-invalid rule too, but exposing it would leak a Jolt type through an ABI whose entire point is that it does not (`PhysicsWorld.cpp:128-130`).

**Staleness.** Not generational, and it does not need to be: `nextHandle` only ever increments, so a removed body's handle is never reissued and a stale lookup simply misses — reported as 0 with the out-params left untouched (`physics_abi.h:66-67`).

### World lifetime and stepping
*You are starting the simulation, advancing it, or changing the fixed step before any body exists.*

```c
AVER_PHYS_API int32_t aver_phys_init(void);
AVER_PHYS_API void    aver_phys_shutdown(void);
AVER_PHYS_API int32_t aver_phys_ready(void);
AVER_PHYS_API void    aver_phys_set_gravity(float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_step(float dt);
AVER_PHYS_API float   aver_phys_fixed_step(void);
AVER_PHYS_API int32_t aver_phys_set_fixed_step(float seconds);
```

`init` is idempotent. Gravity is cm/s², defaulting to `(0, 0, -980)`. The simulation runs at a **fixed** step regardless of what is passed to `step`, because Jolt's determinism guarantee is stated in terms of the same calls in the same order and a step that follows the frame rate makes the result a function of machine speed; leftover time is carried and a long stall is clamped, so a breakpoint does not fire a hundred steps at once. `step` returns how many fixed steps actually ran (`physics_abi.h:34-47`). Set the fixed step **before** bodies exist — changing it mid-session changes the meaning of every tuned velocity in the game.

### Primitive bodies and their transforms
*You are putting a floor, a wall or a falling crate into the world, or reading back where a simulated body ended up so you can move its visual.*

```c
AVER_PHYS_API int32_t aver_phys_add_static_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);
AVER_PHYS_API int32_t aver_phys_add_dynamic_box(float cx, float cy, float cz,
                                                float hx, float hy, float hz, float massKg);
AVER_PHYS_API int32_t aver_phys_add_dynamic_sphere(float cx, float cy, float cz,
                                                   float radius, float massKg);
AVER_PHYS_API int32_t aver_phys_remove_body(int32_t body);
AVER_PHYS_API int32_t aver_phys_body_position(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_rotation(int32_t body, float* outQuat);
AVER_PHYS_API int32_t aver_phys_body_velocity(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_set_position(int32_t body, float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_body_set_velocity(int32_t body, float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_body_count(void);
```

Half-extents and radii in centimetres. `massKg <= 0` asks Jolt to derive mass from the shape's volume. The three readers return 0 for a dead handle, leaving the outputs untouched (`physics_abi.h:66-67`).

### Character controller
*You want something that **walks** — pushed out of geometry, not falling through the floor — rather than something simulated as a rigid body.*

```c
AVER_PHYS_API int32_t aver_phys_character_create(float radius, float height,
                                                 float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_character_destroy(int32_t ch);
AVER_PHYS_API int32_t aver_phys_character_set_velocity(int32_t ch, float vx, float vy, float vz);
AVER_PHYS_API int32_t aver_phys_character_velocity(int32_t ch, float* outXyz);
AVER_PHYS_API int32_t aver_phys_character_position(int32_t ch, float* outXyz);
AVER_PHYS_API int32_t aver_phys_character_set_position(int32_t ch, float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_character_grounded(int32_t ch);
```

Backed by Jolt's `CharacterVirtual`, which is swept and resolved rather than simulated — which is what makes a character feel controlled (`physics_abi.h:78-81`). `height` is the **total** capsule height including both caps, so a 180 cm character is 180. The velocity you set is the one the character *wants*: horizontal comes from input, the vertical component is managed by the simulation unless you set it, which is how a jump is expressed (`physics_abi.h:89-91`).

### Arbitrary collision geometry
*Boxes and spheres have run out and you need a real level — a hull round a prop, exact triangles for architecture, or terrain — from arrays you already hold.*

```c
AVER_PHYS_API int32_t aver_phys_add_convex_hull(const float* pointsXyz, int32_t count,
                                                float cx, float cy, float cz,
                                                int32_t dynamic, float massKg);
AVER_PHYS_API int32_t aver_phys_add_mesh(const float* verticesXyz, int32_t vertexCount,
                                         const int32_t* indices, int32_t indexCount,
                                         float cx, float cy, float cz);
AVER_PHYS_API int32_t aver_phys_add_heightfield(const float* samples, int32_t sampleCount,
                                                float spacingCm,
                                                float cx, float cy, float cz);
```

Raw arrays, deliberately **not** a mesh handle: the engine has no `.ocmesh` loader yet, so a collider that could only be built from a loaded asset would be a door to nowhere (`physics_abi.h:100-106`). A triangle mesh is **static only** — that is Jolt's rule, not a shortcut, because a mesh has no interior and so nothing to resolve a penetration against (`:114-116`). Heightfield `sampleCount` is rounded down to Jolt's block-size multiple (`:121-123`).

### Sensors (triggers)
*You want a pickup volume, a level exit or a damage zone — something that notices an overlap without blocking movement.*

```c
AVER_PHYS_API int32_t aver_phys_add_sensor_box(float cx, float cy, float cz,
                                               float hx, float hy, float hz);
AVER_PHYS_API int32_t aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius);
```

A sensor is a real body in the broad phase, so it costs what a body costs (`physics_abi.h:128-131`).

### Contact and overlap events — polled, never called back
*You are reacting to a collision or a trigger from gameplay, in a PostPhysics tick, on the main thread.*

```c
AVER_PHYS_API int32_t aver_phys_contact_count(void);
AVER_PHYS_API int32_t aver_phys_contact_get(int32_t index, int32_t* outBodyA, int32_t* outBodyB,
                                            float* outPoint, float* outNormal);
AVER_PHYS_API int32_t aver_phys_overlap_count(void);
AVER_PHYS_API int32_t aver_phys_overlap_get(int32_t index, int32_t* outSensor, int32_t* outBody,
                                            int32_t* outEntered);
```

Polling is a design choice, not a shortcut. Jolt invokes its contact listener from several worker threads during the step in an order it documents as non-deterministic; calling managed code from there would mean marshalling into the CLR from threads it has never seen, mid-simulation, with gameplay then free to mutate the world being stepped. Buffering and draining after the step keeps every reaction on the main thread, in a fixed order, at a point where the world is safe to touch (`physics_abi.h:137-148`). Both queues are cleared at the **start** of each `aver_phys_step`, so what you drain describes the step that just ran. `outEntered` is 1 for an enter, 0 for an exit. The queues themselves are the one piece of genuinely cross-thread state in any seam — they are guarded by a mutex (`PhysicsWorld.cpp:142-146`); see §17 on threading.

### Queries: raycast, overlap, sweep
*You are asking the world a question right now — what is under the crosshair, what is inside the blast radius, what a projectile or camera boom would hit on the way.*

```c
AVER_PHYS_API int32_t aver_phys_raycast(float ox, float oy, float oz,
                                        float dx, float dy, float dz,
                                        float maxDistCm, float* outPoint, float* outNormal);
AVER_PHYS_API int32_t aver_phys_overlap_sphere(float x, float y, float z, float radius,
                                               int32_t* outBodies, int32_t maxBodies);
AVER_PHYS_API int32_t aver_phys_sphere_cast(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            float maxDistCm, float radius,
                                            float* outPoint, float* outNormal);
```

`raycast` and `sphere_cast` return the hit body handle or 0, writing the out-params only on a hit. `overlap_sphere` returns how many handles were **written**, so a result equal to `maxBodies` means the list was truncated and the caller should ask again with a bigger buffer rather than assume it saw everything (`physics_abi.h:173-175`). A sweep has thickness, which is what a projectile or a step-up probe actually needs, because a ray slips through gaps a moving object could never fit through (`:181-183`).

> Jolt documents broadphase queries as **not deterministic** — the broad phase can be modified from several threads. A gate may assert on a hit's existence and position, but must never depend on *which* of several equidistant bodies comes back (`physics_abi.h:166-168`).

---

## 7. `Aver.Render.PBR` — `pbr_abi.h`

**Header** `modules/render.pbr/include/aver/pbr/pbr_abi.h` · **DLL** `Aver.Render.PBR` (SHARED, links `PUBLIC Aver.Core` **only** — `modules/render.pbr/CMakeLists.txt:9, 14`) · **No version macro and no version entry point** · **48 entry points.**

**The two-target split is the rule made physical.** The GPU half is a *second*, static target, `Aver.Render.PBR.Materials`, linking `PUBLIC Aver.Core Aver.RHI Aver.Render.PBR` (`CMakeLists.txt:30-37`). It exists because giving the DLL an RHI dependency would put render-hardware types on the P/Invoke boundary; the comment names `Aver.RHI` (generic) and **never** `Aver.RHI.D3D12`, and states that a link line is the only place that rule can actually be enforced (`CMakeLists.txt:25-29`).

**Handle.** `typedef int32_t aver_pbr_material` (`pbr_abi.h:77`); 0 invalid, a valid handle always positive. Behind it, `aver::pbr::MaterialHandle` is a `u32` with the index in bits 0–19 and an 11-bit generation in bits 20–30, bit 31 left clear — because a negative handle would read as an error code in every FFI that follows the setters-return-1/0 convention (`Material.hpp:53-69`).

**Staleness.** Generational, and necessarily so: materials are **instances** whose slots are reused, so a bare index would let a stale reference silently address a different material (`Material.hpp:53-58`). `materialGeneration(h)` is the check (`Material.hpp:69`), `aver_pbr_valid` reports it across the ABI, and `MaterialLibrary::desc` returns `nullptr` for a stale or never-issued handle (`Material.hpp:156-157`).

**One deliberate deviation from the int32-only idiom:** a texture's opaque asset id is `int64_t`, so it stays forward-compatible with an `ObjectId` or an `.octex` GUID, neither of which fits in 32 bits; splitting it into halves would put the burden of reassembling an identifier on every binding (`pbr_abi.h:10-14`).

### Feature and status introspection
*You are building a settings UI or a capability report and need names, and whether each feature is ready, not implemented, or unsupported.*

```c
AVER_PBR_ABI int32_t     aver_pbr_feature_count(void);
AVER_PBR_ABI const char* aver_pbr_feature_name(int32_t feature);
AVER_PBR_ABI int32_t     aver_pbr_status(int32_t feature);
AVER_PBR_ABI const char* aver_pbr_status_text(int32_t feature);
AVER_PBR_ABI const char* aver_pbr_texture_slot_name(int32_t slot);
AVER_PBR_ABI const char* aver_pbr_alpha_mode_name(int32_t mode);
```

Ids: `AVER_PBR_FEATURE_*` (`pbr_abi.h:44-52`), `AVER_PBR_STATUS_*` (`:55-57`), `AVER_PBR_TEX_*` (`:60-65`), `AVER_PBR_ALPHA_*` (`:72-74`).

### Material lifetime
*You are creating a material to author or assign, throwing one away, or checking that a stored handle still points at the material you think it does.*

```c
AVER_PBR_ABI aver_pbr_material aver_pbr_create(const char* name);
AVER_PBR_ABI int32_t           aver_pbr_destroy(aver_pbr_material m);
AVER_PBR_ABI int32_t           aver_pbr_valid(aver_pbr_material m);
```

`create` starts from the glTF default surface and returns 0 if none could be created.

### Enumeration over live materials
*You are listing every material in the process — a content browser, a save pass.*

```c
AVER_PBR_ABI int32_t           aver_pbr_count(void);
AVER_PBR_ABI aver_pbr_material aver_pbr_at(int32_t index);
```

Indices are dense over live materials and **shift on destroy** (`pbr_abi.h:93`).

### Identity
*You want the display name, or you are renaming one.*

```c
AVER_PBR_ABI const char* aver_pbr_get_name(aver_pbr_material m);   /* "" for a stale handle */
AVER_PBR_ABI int32_t     aver_pbr_set_name(aver_pbr_material m, const char* name);
```

### Surface factors
*You are reading or changing what the surface looks like — colour, metalness, roughness, emission, normal strength — which is the single most common reason anyone opens this seam. Names match the `.ocmat` PARAM names (`pbr_abi.h:101`).*

```c
AVER_PBR_ABI int32_t aver_pbr_get_base_color_factor(aver_pbr_material m, float* out4);
AVER_PBR_ABI int32_t aver_pbr_set_base_color_factor(aver_pbr_material m, float r, float g, float b, float a);
AVER_PBR_ABI int32_t aver_pbr_get_emissive_factor(aver_pbr_material m, float* out3);
AVER_PBR_ABI int32_t aver_pbr_set_emissive_factor(aver_pbr_material m, float r, float g, float b);
AVER_PBR_ABI float   aver_pbr_get_metallic_factor(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_metallic_factor(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_roughness_factor(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_roughness_factor(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_normal_scale(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_normal_scale(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_occlusion_strength(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_occlusion_strength(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_reflectance(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_reflectance(aver_pbr_material m, float v);
AVER_PBR_ABI float   aver_pbr_get_f90(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_f90(aver_pbr_material m, float v);
```

Reflectance is the dielectric base (0.04 for most things, ~0.02 water, ~0.17 gemstone) and `f90` the reflectance at grazing incidence; both in [0,1] (`pbr_abi.h:114-115`).

### Blending and sidedness
*You are making something transparent, cut-out, double-sided, or stopping it casting a shadow.*

```c
AVER_PBR_ABI int32_t aver_pbr_get_alpha_mode(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_alpha_mode(aver_pbr_material m, int32_t mode);
AVER_PBR_ABI float   aver_pbr_get_alpha_cutoff(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_alpha_cutoff(aver_pbr_material m, float v);
AVER_PBR_ABI int32_t aver_pbr_get_two_sided(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_two_sided(aver_pbr_material m, int32_t on);
AVER_PBR_ABI int32_t aver_pbr_get_cast_shadow(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_cast_shadow(aver_pbr_material m, int32_t on);
```

### Texture mapping — mesh UVs vs world-aligned projection
*You are texturing a blockout built from scaled cubes and want constant texel density without unwrapping anything.*

```c
AVER_PBR_ABI int32_t aver_pbr_get_uv_mode(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_uv_mode(aver_pbr_material m, int32_t mode);
AVER_PBR_ABI float   aver_pbr_get_uv_tiling(aver_pbr_material m);
AVER_PBR_ABI int32_t aver_pbr_set_uv_tiling(aver_pbr_material m, float cmPerTile);
AVER_PBR_ABI const char* aver_pbr_uv_mode_name(int32_t mode);
```

`AVER_PBR_UV_WORLD_ALIGNED` projects world position onto the dominant axis of the surface normal instead of using the mesh's own UVs. Tiling is world **centimetres per tile** and is read only in that mode; a value `<= 0` is rejected rather than stored, because it would collapse the projection (`pbr_abi.h:131-135`). It is dominant-axis projection, not triplanar blending: a blockout is axis-aligned boxes, where projection is exact and seamless and blending would cost three samples per map instead of one (`Material.hpp:47-50`).

### Texture references (path and opaque id)
*You are pointing a slot at an image, by authoring path or by opaque asset id — this module interprets **neither**, which is exactly what keeps the DLL Core-only.*

```c
AVER_PBR_ABI const char* aver_pbr_get_texture_path(aver_pbr_material m, int32_t slot); /* "" if unset */
AVER_PBR_ABI int32_t     aver_pbr_set_texture_path(aver_pbr_material m, int32_t slot, const char* path);
AVER_PBR_ABI int64_t     aver_pbr_get_texture_id(aver_pbr_material m, int32_t slot);   /* 0 if unset */
AVER_PBR_ABI int32_t     aver_pbr_set_texture_id(aver_pbr_material m, int32_t slot, int64_t id);
AVER_PBR_ABI int32_t     aver_pbr_clear_texture(aver_pbr_material m, int32_t slot);
```

Resolving either one needs the asset system, which lives a tier up (`Material.hpp:71-74`).

### Upload bookkeeping
*You are the one consumer responsible for pushing changed material state to the GPU this frame.*

```c
AVER_PBR_ABI int32_t aver_pbr_consume_dirty(aver_pbr_material m);
```

**Reading it clears it**, so exactly one consumer acts on each change (`pbr_abi.h:149-152`). The header describes this as the "same contract as `aver_voxi`'s msaa dirty flag" — treat that cross-reference as inaccurate: no such entry point exists on the Voxi ABI, and the actual analogue is the C++-only `Renderer::consumeMsaaDirty` at `modules/render.voxi/include/aver/voxi/Voxi.hpp:95`.

---

## 8. `Aver.Render.Voxi` — `voxi_abi.h`

**Header** `modules/render.voxi/include/aver/voxi/voxi_abi.h` · **DLL** `Aver.Render.Voxi` (SHARED, links `PUBLIC Aver.Core` only — `modules/render.voxi/CMakeLists.txt:6, 11`) · **No version macro and no version entry point** · **21 entry points.**

Same two-target split as PBR: `Aver.Render.Voxi.Renderer` is STATIC and links `Aver.Core Aver.RHI Aver.Render.Voxi Aver.Render.PBR.Materials` (`CMakeLists.txt:25-38`). The dependency on the material system is the architecture on a link line — Voxi supplies visibility and irradiance and calls the shading model; the material owns the BRDF; the arrow runs this way and never the other (`CMakeLists.txt:33-36`). The top-level build forces `AVER_MODULE_VOXI` off if `AVER_MODULE_PBR` is off, because Voxi renders materials and the failure without that line is a C1083 four includes deep that reads as a broken include path rather than as an invalid module combination (root `CMakeLists.txt:85-93`).

**Handles: none.** This is the one seam with no handles at all — a global settings block plus a read-only mirror of device capabilities, which is why every call is a get/set with no instance parameter. `pbr_abi.h:19-21` names this "global-settings shape" as the thing materials deliberately differ from. Nothing can go stale, so the only rejection path is the error convention: setters return 1, or 0 when the request is rejected as an unsupported feature or a bad argument (`voxi_abi.h:10-12`).

### Feature introspection
*You are drawing a graphics-settings panel and need what exists and whether each item is ready, not implemented, or unsupported on this machine.*

```c
AVER_VOXI_ABI int32_t     aver_voxi_feature_count(void);
AVER_VOXI_ABI const char* aver_voxi_feature_name(int32_t feature);
AVER_VOXI_ABI int32_t     aver_voxi_feature_status(int32_t feature);
AVER_VOXI_ABI const char* aver_voxi_feature_status_text(int32_t feature);
```

Ids: `AVER_VOXI_FEATURE_*` (`voxi_abi.h:31-36`), `AVER_VOXI_STATUS_*` (`:39-41`), `AVER_VOXI_QUALITY_OFF/LOW/MEDIUM/HIGH/EPIC` (`:44-48`). Those two `const char*` returns (`:52`, `:54`) are the **only** outbound strings on this seam.

### Anti-aliasing
*You are changing the MSAA sample count, or asking which counts this device will accept before you offer them.*

```c
AVER_VOXI_ABI int32_t aver_voxi_get_msaa(void);         /* sample count: 1, 2, 4, 8 */
AVER_VOXI_ABI int32_t aver_voxi_set_msaa(int32_t samples);
AVER_VOXI_ABI int32_t aver_voxi_msaa_mask(void);        /* bit N set => N samples supported */
```

### Quality ladder (GI, ray tracing, path tracing)
*You want to turn GI down to Low, or read back where a quality-laddered feature sits.*

```c
AVER_VOXI_ABI int32_t aver_voxi_get_quality(int32_t feature);
AVER_VOXI_ABI int32_t aver_voxi_set_quality(int32_t feature, int32_t quality);
```

### Global illumination tunables
*The preset is not enough and you are tuning GI itself — voxel resolution, intensity, or how far in centimetres it reaches.*

```c
AVER_VOXI_ABI int32_t aver_voxi_get_voxel_resolution(void);
AVER_VOXI_ABI int32_t aver_voxi_set_voxel_resolution(int32_t res);
AVER_VOXI_ABI float   aver_voxi_get_gi_intensity(void);
AVER_VOXI_ABI int32_t aver_voxi_set_gi_intensity(float v);
AVER_VOXI_ABI float   aver_voxi_get_gi_max_distance(void);
AVER_VOXI_ABI int32_t aver_voxi_set_gi_max_distance(float cm);
```

### Device capabilities (read-only)
*You are deciding what to offer or fall back to, and need what the GPU reports rather than what was asked for.*

```c
AVER_VOXI_ABI int32_t aver_voxi_ray_tracing_tier(void);  /* 0 none, 10 DXR 1.0, 11 DXR 1.1 */
AVER_VOXI_ABI int32_t aver_voxi_max_msaa(void);
AVER_VOXI_ABI int32_t aver_voxi_mesh_shader_tier(void);  /* 0 none, 1 Tier 1 */
AVER_VOXI_ABI int32_t aver_voxi_shader_model(void);      /* 60 = SM 6.0, 65 = SM 6.5 */
```

### Geometry submission path
*You are switching how geometry reaches the GPU, typically to compare against the classic path.*

```c
AVER_VOXI_ABI int32_t aver_voxi_get_mesh_shaders(void);
AVER_VOXI_ABI int32_t aver_voxi_set_mesh_shaders(int32_t on);
```

---

## 9. `Aver.Scripting` — `scripting_abi.h`

**Header** `modules/scripting/include/aver/scripting/scripting_abi.h` · **Target** `Aver.Scripting.Host`, **STATIC** (`aver_add_module` builds static only — `cmake/AvModule.cmake:14`), linking `Aver.Core Aver.Platform` and deliberately **not** the RHI (`modules/scripting/CMakeLists.txt:3-10`) · **Contract version 2** · **0 exported C functions.**

**There is no export macro, and that is the design.** "There is no dllexport here. The host does not export symbols for the bridge to P/Invoke back into: it hands the bridge a table of function pointers at bootstrap instead" (`scripting_abi.h:12-15`). A P/Invoke would have to name the loaded module — which is the *executable* (`Sandbox.exe` today) — tying a shipped bridge assembly to whatever host embeds it.

`nethost`/`hostfxr` are resolved with `LoadLibraryW` at run time so nothing is linked for them, which is what lets this module build on a machine with no .NET at all (`modules/scripting/CMakeLists.txt:15-16`).

### The host API table the host hands the bridge
*You are embedding the CLR and need the shape of the one struct that goes managed-ward at bootstrap.*

```c
typedef void(__cdecl* aver_script_log_fn)(int32_t level, const char* utf8Message);

typedef struct AverScriptHostApi {
    int32_t structBytes;      /* sizeof(AverScriptHostApi) */
    int32_t contractVersion;  /* AVER_SCRIPTING_CONTRACT_VERSION as the host was built with */
    aver_script_log_fn log;
} AverScriptHostApi;                                        /* scripting_abi.h:45-51 */
```

It is read by C# as `[StructLayout(LayoutKind.Sequential)]`, so only `int32_t` and pointers may appear in it, and strings are UTF-8 `const char*` in **both** directions (`scripting_abi.h:6-10`). Managed code logs through the engine log rather than `Console`, because a hosted CLR in a GUI process has no console attached (`:43-44`). Log levels are `AVER_SCRIPT_LOG_TRACE/INFO/WARN/ERROR` (`:38-41`).

### The bridge's five `[UnmanagedCallersOnly]` entry points
*You are driving the script host — bootstrapping it, loading or hot-reloading a directory of assemblies, ticking behaviours, or shutting down.* These are **not** exported C symbols; the host binds them **by name** through hostfxr at `modules/scripting/src/ScriptHost.cpp:185-189`, declining with a message if any name is missing (`ScriptHost.cpp:194-195`). They are documented as a comment block at `scripting_abi.h:53-69`.

```c
int32_t Bootstrap(const AverScriptHostApi*)  /* install the host API, check the contract */
int32_t LoadScripts(const char* utf8Dir)     /* load a directory of assemblies; live count back */
int32_t UnloadScripts(void)                  /* drain OnShutdown and unload the collectible ALC */
void    Update(float dt)                     /* drive OnUpdate on every live behaviour */
void    Shutdown(void)                       /* drain, unload, drop the host API */
```

Hot reload is `UnloadScripts` → the host rebuilds the assemblies → `LoadScripts`. The rebuild step is deliberately native: the host already owns the `dotnet build` shell-out, and a managed side that spawned compilers would be doing a job it has no business knowing about. `UnloadScripts` returns 1 when the old load context was fully collected and 0 when it is still finalising — **both are success**, because unloading in .NET is a request satisfied only once every reference is dropped and a GC has run; a 0 means "the old context is still costing memory", never "the reload failed" (`scripting_abi.h:61-69`).

### Bootstrap return codes
*You are turning a failed bootstrap into a message that names what is stale, because "scripting failed" sends nobody anywhere (`scripting_abi.h:71-72`).*

```c
#define AVER_SCRIPT_OK                    0
#define AVER_SCRIPT_ERR_CONTRACT         (-1)   /* contractVersion / structBytes disagree */
#define AVER_SCRIPT_ERR_MANAGED_FAULT    (-2)   /* an exception escaped inside the bridge itself */
```

This is the one seam where **negative** return values are meaningful, so it does not follow the 1/0 setter convention.

---

## 10. The type rules

| # | Rule | Stated at | Enforcement |
|---|---|---|---|
| T1 | Only `int32_t`, `int64_t`, `float`, `const char*` (and pointers to those, or `void`) cross a P/Invoke seam | `scene_abi.h:6-10`; `framework_abi.h:6-9`; `pbr_abi.h:6-17`; `voxi_abi.h:6-8`; `physics_abi.h:2-3` | **Convention only** |
| T2 | Vectors return through a `float*` **out-param**, never as a small struct by value | `scene_abi.h:8-10`; `pbr_abi.h:15-17` | **Convention only** |
| T3 | No function pointers, no `void*`, no structs, no enums on a P/Invoke surface | `scene_abi.h:19-21`; `framework_abi.h:7-9`; `framework_hooks.h:6-17` | **Convention only** |
| T4 | Dispatch tables live in `framework_hooks.h`, which no managed code marshals field by field | `framework_hooks.h:11-17` | **Convention only** (a file split and a comment) |
| T5 | Aver.Scene carries no gameplay vocabulary — not *actor*, *pawn*, *spawn*, *possess*, *play* | `scene_abi.h:14-17` | **Build-enforced, in part** — see below |
| T6 | No RHI type may sit behind a P/Invoke DLL; `Aver.RHI` is generic and never `Aver.RHI.D3D12` | `modules/render.pbr/CMakeLists.txt:5-6, 26-29` | **Build-enforced** |
| T7 | The Voxi shared DLL must not gain an RHI dependency; the GPU half is a separate static library | `VoxiRenderer.hpp:19-23`; `Voxi.hpp:8-10` | **Build-enforced** |

**On T1–T3, what would catch a violation: nothing at build time.** There is no lint step, no header-parse check, and no CTest registration anywhere in the tree (a grep for `enable_testing` and `add_test(` across every `CMakeLists.txt` and `cmake/*.cmake` returns no matches). Reading all five P/Invoke headers end to end, the rules currently hold — every declared parameter and return is one of the permitted types or a pointer to one, and no function returns a struct by value — but they hold only because they have been kept by hand. A non-blittable parameter added tomorrow would compile and link; the failure would appear at the first call from C# as a `MarshalDirectiveException` or as silent corruption, in a build the tooling reports as green.

**Two documented cracks in T3.** `framework_abi.h:205-218` declares the key codes as an **anonymous enum** rather than as `#define`s, which every other constant block in every seam uses (`scene_abi.h:70-92`, `pbr_abi.h:44-74`, `voxi_abi.h:31-48`, and `framework_abi.h`'s own `:99-112` and `:174-176`). It breaks the letter of the rule the same header states at `:8`. It is harmless — no enum *type* appears in any signature, and `aver_fw_input_set_key`/`aver_fw_input_key` take `int32_t` — but nothing in the build noticed. Separately, `scripting_abi.h:45-51` contains a function-pointer typedef and a struct that managed code genuinely marshals (`Marshal.PtrToStructure<HostApi>` at `scripting/csharp/Aver.Scripting.Bridge/HostBridge.cs:104`); that is a **documented and deliberate exception**, explained at `scripting_abi.h:12-15`, and is listed here only so nobody is surprised to find a struct in a file named `*_abi.h`.

**On T5, be precise about what is proven.** `modules/scene/CMakeLists.txt:21` is the whole link line: `target_link_libraries(Aver.Scene PUBLIC Aver.Core Aver.Assets)`. `Aver.Framework` does not appear. The arrow the other way is `modules/framework/CMakeLists.txt:17`. The root `CMakeLists.txt:105-108` additionally forces `AVER_MODULE_FRAMEWORK` off when `AVER_MODULE_SCENE` is off, so the dependency can never invert. What the link line **actually** enforces is that Scene cannot *call* framework code — any attempt is an unresolved external. What it does **not** enforce is the vocabulary: a gameplay-flavoured identifier in `scene_abi.h` would compile and link perfectly. A grep over `modules/scene/src` and `modules/scene/include` finds no gameplay identifiers, so the convention holds — but `scene_abi.h:161` uses the word *actor* in prose ("a convenience for tools and scripts resolving an actor by name"), and `:14` names *actor* explicitly as forbidden. Nothing breaks, since it is a comment rather than an identifier; it is exactly the drift a link line cannot see.

**On T6, the closure was checked in full — of `Aver.*` targets.** Reading the CMake files alone: `Aver.Core` has no deps (`modules/core/CMakeLists.txt:1`); `Aver.Platform → Core` (`modules/platform/CMakeLists.txt:9-10`), and additionally links the Windows system libraries `user32 gdi32 shell32 ole32` PUBLIC (`modules/platform/CMakeLists.txt:18`); `Aver.Assets → Core, Platform` (`modules/assets/CMakeLists.txt:4-6`); `Aver.RHI → Core, Platform` (`modules/rhi/CMakeLists.txt:6-8`). Engine-side closures, i.e. no `Aver.*` dependency beyond what is listed: Scene `{Core, Assets, Platform}`; Framework `{Core, Assets, Scene, Platform}`; Render.PBR `{Core}`; Render.Voxi `{Core}`; Physics `{Core}` public with Jolt private. Those are not full link closures — the Win32 libraries above ride along wherever Platform does — but they are the complete `Aver.*` picture, and that is what T6 is about: `Aver.RHI` appears in none of them, and a grep for `aver/rhi` across the scene, framework, physics sources and `Material.cpp` / `Voxi.cpp` returns nothing. This one is build-enforced in the strong sense: RHI symbols used inside a shared target would be unresolved externals, because the import library is not on the line. `modules/assets/CMakeLists.txt:9-20` keeps `Aver.Assets` a leaf by putting the RHI-needing join in a separate `Aver.Assets.Gpu` target, which is what lets Scene depend on Assets without dragging the RHI in.

**One hazard T7 does not close.** `VoxiRenderer.hpp` lives in the same directory the SHARED target publishes as a PUBLIC include dir (`modules/render.voxi/CMakeLists.txt:9`), so an RHI-dependent header is reachable by include path from a Core-only consumer. Including it from such a consumer fails to compile, because no RHI include dirs are on the line — so the mistake is caught, but by a missing-header error rather than by a rule that names the problem.

---

## 11. Handles and staleness

Every handle in every seam is an `int32_t` with `0 == invalid`. Beyond that the seams differ, and the differences are deliberate.

| Seam | Handle | Scheme | Detect staleness with | What a stale handle does |
|---|---|---|---|---|
| Scene | entity | 24-bit index + 7-bit generation, bit 31 clear (`Entity.hpp:23-42`) | `aver_scene_valid(e)` | Getter → neutral (0 / 0.0f / `""` / no write); setter → 0 |
| Scene | field id, component id | dense, 0 invalid | **`aver_scene_field(name) != 0`, and nothing else.** `aver_scene_field_kind` cannot serve: it returns 0 for an unknown id *and* for every `F32` field, because `AVER_SCENE_KIND_F32` is 0 (`scene_abi.h:112`) | Rejected by the shared `fieldAddr` guard (`SceneAbi.cpp:56-64`) |
| Framework | class | **not** generational: deque index, bounds test (`FrameworkAbi.cpp:169`) | never goes stale — stable for the process | n/a (that stability *is* hot-reload identity) |
| Framework | entity | the scene's, checked via `world().valid(e)` | `aver_fw_class_of(e) != 0` for "actor" | owner-match + validity, both (`FrameworkAbi.cpp:213`) |
| Physics | body, character | dense from one counter, **never reissued** (`PhysicsWorld.cpp:133`) | the lookup simply misses | 0, out-params untouched (`physics_abi.h:66-67`) |
| PBR | material | 20-bit index + 11-bit generation (`Material.hpp:53-69`) | `aver_pbr_valid(m)` | Getter → neutral (`""`, 0); setter → 0 |
| Voxi | — | no handles | n/a | n/a |
| Scripting | — | no handles | n/a | n/a |

**A 0 from a scene setter does not mean the handle is stale.** Three causes are indistinguishable across the ABI: a stale or invalid entity, a wrong-kind (or out-of-range) field id, and a **read-only** field (§3). There is no entry point that reports which. A binding that maps 0 to "entity destroyed" will report the wrong thing every time somebody writes `CWorld.matrix` or `CHierarchy.parent`.

**Read a returned `const char*` immediately, before your next call into that DLL.** The headers say only that the caller must not free it (`scene_abi.h:100-104`; `framework_abi.h:87-88`); they do not state how long it stays valid, and the implementations make that load-bearing. `aver_scene_get_str` returns `pool[id].c_str()` into a process-global `std::vector<std::string>` (`SceneAbi.cpp:254-264`, pool at `:93-96`). A `set_str` into a field that already owns a slot overwrites in place (`SceneAbi.cpp:283`) and is the case the source comment is about; a `set_str` into a field that owns **no** slot yet takes the other branch and appends, `pool.emplace_back(v)` (`SceneAbi.cpp:285`; the whole else-branch is `:284-286`) — which can reallocate the vector and move every element. For short strings, where the character data lives inside the `std::string` object via SSO, that moves the very bytes an earlier pointer aimed at. The comment at `SceneAbi.cpp:276` claims stability only across repeated edits of the *same* field, which is a narrower guarantee than a binding author would assume from the header. Same class of hazard, undocumented in any form, one module over: `aver_pbr_get_name` returns `d->name.c_str()` (`modules/render.pbr/src/Material.cpp:281`) and `aver_pbr_get_texture_path` returns `d->textures[slot].path.c_str()` (`Material.cpp:414`), both invalidated by the next setter on that material — and `MaterialLibrary::desc` documents its pointer as invalidated by any `create()` (`Material.hpp:156`).

**And on PBR and Voxi, ownership itself is unstated.** `scene_abi.h:100-104` and `framework_abi.h:87-88` say the caller must not free the pointer. `pbr_abi.h` and `voxi_abi.h` say nothing at all — not who owns the seven and two outbound `const char*` respectively, not how long they live, not what encoding they are in (§17). A C# binding gets away with it by decoding into a managed string at the call; a C++ or Rust binding author has nothing to bind against and must assume borrow-until-next-call, because that is what the implementation does.

**Buffer lengths are never passed and never validated.** Every out-parameter is a bare `float*` whose required size lives only in prose: `out16` (`scene_abi.h:166`), `outv` sized from `aver_scene_field_arity` (`:114-119`), `out3`/`out4` (`pbr_abi.h:102-104`), `out3` (`framework_abi.h:230`), `outXyz`/`outQuat` (`physics_abi.h:66-70`). The C# bindings pass `float[]` with no length. A short array is an out-of-bounds write by native code into the managed heap. Nothing on either side checks it, and nothing could — the ABI does not carry the length.

---

## 12. The error convention

**Setters return 1 on success and 0 on a rejected request — a stale handle, a bad field or slot, a read-only field, or a value out of range. Getters return the current value, or a documented neutral value for a stale handle.** Stated identically in all five P/Invoke headers: `scene_abi.h:23-25`, `framework_abi.h:31` and `:86-88`, `pbr_abi.h:22-24`, `voxi_abi.h:10-11`, `physics_abi.h:3`. Read-only rejection is real code but is *not* in any of those statements — it lives in the implementation (`SceneAbi.cpp:146, 169, 193, 215, 243, 268`) and in prose one directory up (`modules/scene/README.md:15-16`).

It is uniform, and **unenforceable by construction** — an `int32_t` return is an `int32_t` return. Compliance is good but not total: several entry points return `void` and so cannot report rejection at all — `aver_fw_input_new_frame` / `_set_key` / `_set_mouse` / `_input_mouse` (`framework_abi.h:219-230`), `aver_fw_set_view` / `aver_fw_view` (`:239-240`), `aver_fw_set_view_entity` (`:258`), `aver_phys_shutdown` (`physics_abi.h:28`) and `aver_phys_set_gravity` (`:32`). Those are documented as ignoring bad input — "Out-of-range keys are ignored" (`framework_abi.h:221`) — which is a deliberate choice rather than a lapse, but it does mean a caller cannot distinguish *accepted* from *silently dropped*.

**The scripting seam does not follow it.** `Bootstrap` returns 0 for success and negative values for failure (`scripting_abi.h:73-75`). That is the only place in the tree where a negative return is meaningful, and it exists so the host can map each code to a message naming what is stale. One planned entry point would break the convention differently: the vtable registrar written down at `docs/SCENE_FRAMEWORK.md:1223` returns `aver_class` rather than `int32_t` 1/0, which `docs/DESIGNER_REWRITE.md:183` already flags.

**Neutral values are documented unevenly.** `scene_abi.h` is the model: the convention at `:23-25`, the entity rule at `:105-107`, per-family neutrals at `:124-126`, and per-function neutrals at `:137`, `:154`, `:156`, `:172`. `pbr_abi.h` states the convention at `:22-24` and marks neutrals inline at `:98`, `:143`, `:145`. `physics_abi.h` states it for the transform readers (`:66-67`) and for `contact_get` (`:152`), but leaves it unstated for `aver_phys_character_grounded` (`:98`) and the character velocity/position readers (`:93-94`). `framework_abi.h` documents `class_name` (`:121`) and the singleton returns but gives no general getter rule. `voxi_abi.h` says "Getters return the current value" and stops (`:11`) — correct, since it has no handles, but a reader arriving from the other headers will not know that unless told.

---

## 13. Versioning, and the boundaries each version governs

**The scheme.** `(major << 16) | minor`, **per module, versioned independently**. MAJOR changes when an existing entry point changes shape or meaning, and a binding compiled against a different major must refuse to run. MINOR changes when entry points are only **added**, so an older binding still works against a newer engine and checks `minor >= what it needs` (`scene_abi.h:44-57`; `framework_abi.h:50-58`; restated in prose at `modules/scene/README.md:91-95` and `modules/framework/README.md:53-57`). Independence is deliberate: the framework's surface will move while the scene's is still settling, and a single shared number would force a lockstep neither module needs (`framework_abi.h:50-52`).

The encoding is real where it exists — Scene MAJOR 1 / MINOR 0 (`scene_abi.h:54-57`), Framework MAJOR 1 / MINOR 1 (`framework_abi.h:53-58`) — and both DLLs report the compiled-in constant rather than a header value (`modules/scene/src/SceneAbi.cpp:102-106`; `modules/framework/src/FrameworkAbi.cpp:361-363`). The minor-bump discipline is being followed by hand: the framework's bump to 1 carries its own justification in the header beside the two entry points that caused it. But **nothing in the build ties the constant to the surface** — an entry point could change shape with the major untouched and the build would be green. What would catch that: nothing automatic.

**The scheme exists on only two of the six seams.** `physics_abi.h`, `pbr_abi.h` and `voxi_abi.h` declare no version constant and export no version function at all (verified by reading all three headers end to end and by grepping the tree for `abi_version`: the only symbols that exist are `aver_scene_abi_version`, `aver_fw_abi_version`, `aver_fw_scene_abi_version` and `aver_fw_scene_abi_matches`). Three seams therefore have **no way** for a caller to detect a stale binary.

### The boundaries, and what each actually protects

| # | Boundary | Constant | Checked where | On mismatch |
|---|---|---|---|---|
| 1 | Host ↔ managed bridge | `AVER_SCRIPTING_CONTRACT_VERSION` = 2 (`scripting_abi.h:35`), mirrored by hand at `HostBridge.cs:27` | Host stamps it plus `sizeof` into the struct (`ScriptHost.cpp:198-201`); bridge checks **both** halves (`HostBridge.cs:105-109`) | **Clean refusal with a message.** `ScriptHost.cpp:210-213` declines naming what is stale; the four entry-point pointers are nulled at `:205-208` and scripting is simply off. The only boundary of these genuinely defended on both sides. |
| 2 | `Aver.Scripting` / `Aver.Framework` assembly version | `AssemblyVersion` in the csproj | Per loaded **user assembly**: `HostBridge.cs:416-432` reads the candidate's own reference table; `:437-453` rejects a different major or a reference newer than the engine provides | **That assembly is rejected and logged** — "…was built against {name} {referenced} but this engine provides {loaded} - the assembly was rejected." (`HostBridge.cs:447-449`) — and the rest of the scripts folder still loads. |
| 3 | Managed dispatch table | `AVER_FW_DISPATCH_VERSION` = 2 (`framework_hooks.h:128`), mirrored at `ManagedDispatch.cs:27` | `aver_fw_install_managed_dispatch` validates `structBytes` **and** `contractVersion` (`FrameworkAbi.cpp:767-778`) | **Install returns 0 and logs**; actors silently do not tick — a defined failure, unlike a shape mismatch. |
| 4 | Actor vtable table | `AVER_FW_VTABLE_VERSION` = 1 (`framework_hooks.h:85`) | nowhere yet — the path it guards does not exist (`framework_hooks.h:80-83`) | **PLANNED.** |
| 5 | **Per-module C ABI version** | `AVER_SCENE_ABI_VERSION`, `AVER_FW_ABI_VERSION` | **Nowhere in shipping code.** | **Nothing happens.** See below. |
| 6 | **Every hand-mirrored constant block** (component ids, class flags, tick groups, play states, begin/end reasons, key codes, PBR and Voxi enums) | none — they carry no version at all | **Nowhere, in either language.** | **Nothing happens; the numbers just disagree.** See §17. |

> **The per-module version scheme is documentation, not a safeguard.** No shipping code path queries any module ABI version. Four greps over the tree find exactly two callers, and both are test executables: `tests/scene/src/SceneTest.cpp:682` — `check(aver_scene_abi_version() == AVER_SCENE_ABI_VERSION, "the DLL reports the header's ABI version")` — and `tests/framework/src/FrameworkTest.cpp:808` — `check(aver_fw_scene_abi_matches() == 1, …)`. Plus one caller internal to the framework DLL, inside `aver_fw_scene_abi_matches` itself (`FrameworkAbi.cpp:376`). A grep for `abi_version|abi_matches` across `sandbox/src`, `editor/` and `modules/runtime/` returns nothing at all; the sandbox links all five DLLs as import libraries (`sandbox/CMakeLists.txt:13, 22, 32, 38, 44`) and never asks any of them what version they are. The same grep over `scripting/csharp/**/*.cs` also returns nothing: no managed assembly binds either version function.
>
> The cost is precisely the failure the headers describe. `framework_abi.h:64-67` spells it out: a framework built against scene major 1 loaded beside a scene major 2 "is a mismatch the loader will not catch — the import lib resolves by NAME, and every name still exists". The repair function was written, it works, and nobody calls it outside a test. `docs/SCENE_FRAMEWORK.md:1722-1725` specifies the missing check — "Bind-time sanity check, in `Bootstrap`: assert `aver_scene_abi_version() == 1` and `aver_fw_abi_version() == 1` and decline loudly on mismatch" — and it has not been implemented. **Treat that as PLANNED.** (Note the same passage adds "There is no `NativeLibrary.SetDllImportResolver` anywhere in this tree", which is now itself out of date — see §14.)

**One correction to a reassuring log line.** The runtime prints `[Scripting] managed bridge online (contract v2, 10.0.10, API v1.0.0.0)` from `HostBridge.cs:116-118`. Only **two** of its three numbers are Aver version boundaries. `contract v2` is boundary 1 and `API v1.0.0.0` is boundary 2, but the middle number is `Environment.Version` — the .NET runtime the in-process CLR resolved. Nothing compares it to anything; it is diagnostic only. And the boundaries a reader most needs warning about, 5 and 6, are the ones the line omits.

Two further details on boundary 2: the check covers `Aver.Scripting` **and** `Aver.Framework` (`HostBridge.cs:418-431`) but not the managed `Aver.Scene` assembly, whose version is never compared; and the version is taken from the reference table rather than from an attribute precisely so an author cannot forget to opt in (`HostBridge.cs:409-413`).

---

## 14. How a call reaches the DLL

**Two mechanisms, one per consumer — and the difference is why a version check would matter.**

**Native consumers use the import library**, resolved by the Windows loader before `main()` runs. `sandbox/CMakeLists.txt` names each DLL on the link line: `Aver.Render.Voxi` (`:13`), `Aver.Render.PBR` (`:22`), `Aver.Scene` (`:32`), `Aver.Physics` (`:38`), `Aver.Framework` (`:44`). Every DLL is staged next to the executable via `RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin`. `Aver.Framework` links `Aver.Scene` the same way — the tree's only SHARED-links-SHARED edge, flagged as such at `framework_abi.h:24-29`. The test executables load the same way. This is exactly the mechanism `framework_abi.h:64-67` warns about: the import lib resolves by **name**, and every name still exists, so a stale DLL binds cleanly and calls the wrong shape.

**Note that a DLL exports more than its C seam.** `AVER_SCENE_API` (defined at `Entity.hpp:9-17`) puts `__declspec` on the C++ `World` class itself (`World.hpp:24`), which is how `FrameworkAbi.cpp:13-15` includes `World.hpp` and `:36` calls `World::instance()` directly. The C seam is the **P/Invoke** boundary, not the whole DLL boundary.

**Managed consumers use `DllImport` by bare name, intercepted by an explicit resolver.** `scripting/csharp/Aver.Scene/NativeResolver.cs:28-46` and its twin in `Aver.Framework` register a `DllImportResolver` from a `[ModuleInitializer]` and load `<exe dir>\<name>.dll` explicitly via `NativeLibrary.TryLoad`. The reason is a genuine file-name collision: the **managed** contract assemblies `Aver.Scene.dll` and `Aver.Framework.dll` are staged into `bin/Scripting/` while the **native** DLLs of the same names sit in `bin/`, and the default probe would find the managed assembly first and try to load it as a native library (`NativeResolver.cs:12-19`; `modules/scripting/CMakeLists.txt:36-41`). The resolver handles exactly those two names (`NativeResolver.cs:37-38`); `Aver.Render.PBR`, `Aver.Render.Voxi` and `Aver.Physics` have no collision and fall through to the default probe, finding the DLLs the executable already loaded.

**The CLR itself is loaded with `LoadLibraryW`, deliberately, with nothing linked.** `modules/scripting/src/ScriptHost.cpp:135` loads `nethost.dll` by bare name and `:149` loads hostfxr by resolved path. The bridge's five entry points are then bound **by name** through hostfxr (`ScriptHost.cpp:185-189`), with a decline path if any is missing.

---

## 15. Adding an entry point — the checklist

Every file below must change in the same commit. There is no generator and no lint step; the list *is* the mechanism.

1. **Choose the seam** using §16. If you are unsure between Scene and Framework, ask whether the name you want to give the function is gameplay vocabulary. If it is, it belongs above.
2. **Declare it in the header**, in the right group, with a comment saying *when you reach for it*, what the neutral return is for a stale handle, and — if it is a setter over the field table — whether the target can be read-only. Types: `int32_t` / `int64_t` / `float` / `const char*` only; a vector goes out through a `float*` out-param; document how many floats that buffer must hold. If it returns a `const char*`, state the encoding and that the caller must not free it, as `scene_abi.h:100-104` does and `pbr_abi.h`/`voxi_abi.h` do not.
3. **Bump the module's MINOR** if the change is purely additive — `AVER_SCENE_ABI_VERSION_MINOR` (`scene_abi.h:55`) or `AVER_FW_ABI_VERSION_MINOR` (`framework_abi.h:56`) — and write the one-line justification beside it, as `framework_abi.h:54-55` does. Bump **MAJOR** if any existing entry point changed shape or meaning. If the seam is Physics, PBR or Voxi there is no constant to bump; see §17.
4. **Implement it** in the module's ABI translation unit: `modules/scene/src/SceneAbi.cpp`, `modules/framework/src/FrameworkAbi.cpp`, `modules/physics/src/PhysicsWorld.cpp`, `modules/render.pbr/src/Material.cpp`, or `modules/render.voxi/src/Voxi.cpp`. Route every rejection through the module's existing guard rather than adding a new one.
5. **If you added an ABI constant, pin it on both sides — and know that only one side can be pinned by a compiler.**
   - **C++ side:** a `static_assert` in the same style as `SceneAbi.cpp:25-42`. Only the scene does this today; a grep over `modules/render.pbr/src`, `modules/render.voxi/src`, `modules/physics/src` and `modules/framework/src` finds one unrelated assert (`VoxiRenderer.cpp:36`) and no ABI pinning at all. Doing it elsewhere is how that spreads.
   - **C# side:** there is no mechanism. Change the mirror by hand and say so in the commit message. §17 lists every mirror that exists; adding a component id, class flag, tick group, play state, begin/end reason, key code or render enum without editing the matching C# file leaves the managed side silently wrong, and nothing anywhere will say so.
6. **Add the test.** `tests/scene/src/SceneTest.cpp`, `tests/framework/src/FrameworkTest.cpp` or `tests/physics/src/PhysicsTest.cpp`. Cover the success path **and** the stale-handle neutral, and — on the scene — the read-only rejection if the field can be one. For PBR and Voxi there is no test executable to add to; see §17.
7. **Add the C# binding**, in whichever assembly owns that seam: `scripting/csharp/Aver.Scene/Native.cs`, `Aver.Framework/Native.cs` (classes `Fw` and `SceneNative`), `Aver.Framework/Physics.cs`, `Aver.Scripting/Pbr.cs` or `Aver.Scripting/Voxi.cs`. Inbound strings marshal as `[MarshalAs(UnmanagedType.LPUTF8Str)]`; outbound `const char*` binds as `IntPtr` and decodes through the file's `Str` helper. **Match the file you are editing** — `Pbr.cs` and `Voxi.cs` currently use ANSI (§17). Two more files are part of this step and are easy to miss: a new component or a cached field id belongs in `scripting/csharp/Aver.Scene/SceneIds.cs` (ids at `:18-25`), and a new `AVER_FW_KEY_*` belongs in `Aver.Framework/Input.cs` (the `Key` enum at `:7-15`), which `Aver.Framework/EnhancedInput.cs` binds against.
8. **Wrap it in the managed surface** if scripts should see it, and record it in `docs/SCRIPTING_API.md` — that document, not this one, is the C# reference.
9. **If you touched `framework_hooks.h`**, the two contract constants and their C# mirrors must move together: `AVER_FW_DISPATCH_VERSION` (`framework_hooks.h:128`) ↔ `ManagedDispatch.cs:27`, and the struct field order ↔ `ManagedDispatch.cs:35-46`. Nothing generates one from the other.
10. **If you touched `scripting_abi.h`**, bump `AVER_SCRIPTING_CONTRACT_VERSION` (`:35`) and its hand-written mirror at `HostBridge.cs:27`, and say in the header comment what the new version added — as the v2 note at `:32-34` does.
11. **Update the prose that duplicates the surface**, or it becomes the next stale comment this document has to flag. Known duplicates: `modules/scene/README.md:12-16` (what the ABI covers, and the read-only rule) and `:91-95` (the version scheme); `modules/framework/README.md:53-57` (the two version functions); and `docs/SCENE_FRAMEWORK.md`, which contains **full copies** of both headers and of the C# bindings at `:854`, `:1081`, `:1223`, `:1465`, `:1624` — including, at `:1223`, a declaration of `aver_fw_class_set_vtable`, which does not exist in any header (§5).
12. **Update this document.** The routing table in §1 first; a seam whose new capability is not routable from a job is a seam a reader will not find.

---

## 16. Where the line between seams falls, and why

**The rule in one sentence: storage below, vocabulary above, and every arrow points down.**

**Scene versus Framework is the worked example.** `Aver.Scene` stores entities and components; what a component *means* is the framework's business (`scene_abi.h:14-17`). `Aver.Framework` is precisely the vocabulary the scene's own README excludes — actor, pawn, possess, begin play — and keeping it in a second module means `scene_abi.h` can be read end to end without meeting any of those words, so "use Aver.Scene without the framework" is a question answered by a link line rather than by discipline (`framework_abi.h:11-17`).

The arrow points **down**: Framework links Scene, never the reverse. One consequence is easy to miss and is stated in the header — the framework sweeps its instance lists with the scene's own validity check rather than asking the scene for a destroy callback, **because a callback would be an edge pointing back up** (`framework_abi.h:19-22`). If you find yourself wanting the lower module to notify the upper one, the design's answer is that the upper one polls.

The same shape recurs. **PBR versus Voxi:** PBR is a material system, Voxi is the thing that renders it; Voxi supplies visibility and irradiance and calls the shading model, the material owns the BRDF, and the dependency runs that way and never the other (`modules/render.voxi/CMakeLists.txt:33-36`). **Scene versus PBR:** the scene needs to name a material and must not link the material library, so `aver_scene_material` interns the name into a scene-local table and the render side maps that token back (`scene_abi.h:188-193`) — what that token means after the material is destroyed is not stated anywhere (§17). **Physics versus everything:** the ABI's entire point is that no Jolt type, metre or +Y-up vector reaches a caller, which is why Jolt is linked `PRIVATE` and why Jolt's own `BodyID` — which would satisfy the handle rules perfectly well — is deliberately not exposed (`physics_abi.h:5-7`; `PhysicsWorld.cpp:128-130`).

**So, for a new capability, ask in this order:**

1. **Does it need the RHI, a device, or a GPU resource?** Then it belongs in the *static* GPU-half target — `Aver.Render.PBR.Materials` or `Aver.Render.Voxi.Renderer` — and **not** behind any P/Invoke DLL. This is the rule a link line enforces most completely (`modules/render.pbr/CMakeLists.txt:26-29`); T5 and T7 in §10 are also enforced by link lines, but only in part.
2. **Does its name use gameplay vocabulary?** Actor, pawn, spawn, possess, play, controller, game mode. Then it is Framework, however storage-shaped the implementation turns out to be.
3. **Is it about *where* or *what* a thing is, with no opinion about what it means?** Then it is Scene.
4. **Is it a property of a surface?** PBR. **A project-wide render setting or a device capability?** Voxi — and note the shape difference: Voxi is global settings with no handles, PBR is per-instance and everything is by handle (`pbr_abi.h:19-21`).
5. **Is it a simulated body, a character, or a spatial query?** Physics.
6. **Does it need to call *up* into gameplay?** Then it is not an entry point at all — it is a slot in `AvManagedDispatch` in `framework_hooks.h`, which is the one file where function pointers may appear.
7. **Does it concern loading, unloading or driving managed code?** Scripting — and remember that seam exports nothing; it hands a table out and is entered by name.

---

## 17. Known gaps

Everything below is what the audit found unenforced, unchecked, untested or unstated. None of it is speculative; each item names where it can be seen.

### The rules that only hold by hand
- **T1, T2, T3, the error convention and the version scheme are convention-only** (§10, §12, §13). There is no header lint, no CTest registration, and no build step that inspects a signature. A `MarshalDirectiveException` at first call is the earliest failure a T1 violation can produce.
- **The vocabulary half of the Scene rule is not enforced** — only the link line is, and it cannot see identifiers, let alone prose. `scene_abi.h:161` already says *actor* in a comment.
- **The framework's `enum` for key codes** (`framework_abi.h:205-218`) breaks the letter of the header's own no-enums rule. Harmless today; unnoticed by everything.
- **`aver_fw_scene_abi_matches()` must keep genuinely calling into Aver.Scene** or the check becomes a tautology (`framework_abi.h:70-77`). It does today (`FrameworkAbi.cpp:376`), but nothing in the build enforces its presence — and the header's supporting argument, that the framework would otherwise import nothing from the scene, no longer holds: `FrameworkAbi.cpp:13-15` includes the scene's C++ headers and `:36` calls `World::instance()`.
- **Read-only fields are enforced in code but stated in no header.** Six setters reject them (`SceneAbi.cpp:146, 169, 193, 215, 243, 268`); the flags are registered at `Builtins.cpp:44, 52, 63, 76, 93`; the only prose is `modules/scene/README.md:15-16`. `scene_abi.h` never uses the words, so a binding author reading only the header cannot know that a 0 from a setter may mean "this field is not yours to write". See §3 — and note that on `CHierarchy` the flag is what stands between a script and an unbounded loop in `composeChain()`/`worldMatrix()` (`SceneAbi.cpp:240-242`).

### Hand-mirrored constants — at least ten pairs, and nothing checks any of them
The headers themselves claim these are "pinned". They are not pinned by anything executable; the word describes an intention. `static_assert`s exist in exactly one file in the engine, `modules/scene/src/SceneAbi.cpp:25-42`, and they pin C constants to **C++** enums — never to C#. A grep for `static_assert` over `modules/render.pbr/src`, `modules/render.voxi/src`, `modules/physics/src` and `modules/framework/src` turns up one unrelated assert at `VoxiRenderer.cpp:36`.

| # | Native | C# mirror | Checked by |
|---|---|---|---|
| 1 | `AVER_SCRIPTING_CONTRACT_VERSION` (`scripting_abi.h:35`) | `HostBridge.cs:27` | **run-time check** — boundary 1, §13 |
| 2 | `AVER_FW_DISPATCH_VERSION` (`framework_hooks.h:128`) | `ManagedDispatch.cs:27` | **run-time check** — boundary 3 |
| 3 | `AvManagedDispatch` field order (`framework_hooks.h:144-157`) | `ManagedDispatch.cs:35-46` | only indirectly, by the `structBytes` check |
| 4 | `AVER_SCENE_COMP_*` (`scene_abi.h:85-92`) | `SceneIds.cs:18-25` | **nothing** |
| 5 | `AVER_SCENE_KIND_*` (`scene_abi.h:70-83`) | no C# mirror; C++ side pinned at `SceneAbi.cpp:25-42` | C++ only |
| 6 | `AVER_FW_CLASS_*` (`framework_abi.h:99-106`; the comment at `:97` says "pinned to Aver.Framework's ClassFlags (Enums.cs)") | `Enums.cs:69-76` | **nothing** |
| 7 | `AVER_FW_TICK_*` (`framework_abi.h:109-112`; comment at `:108`) | `Enums.cs:48-50` | **nothing** |
| 8 | `AVER_FW_PLAY_*` (`framework_abi.h:174-176`) | `Enums.cs:56-58` | **nothing** |
| 9 | `AVER_FW_BEGIN_*` / `AVER_FW_END_*` (`framework_hooks.h:58-65`; comment at `:51-53` says they pin "integer for integer" to `Enums.cs`) | `Enums.cs:23-25` / `:35-38` | **nothing** |
| 10 | `AVER_FW_KEY_*` (`framework_abi.h:205-218`) | `Input.cs:7-15` (`Key`), consumed by `EnhancedInput.cs` | **nothing** |
| 11 | `AVER_PBR_FEATURE_*` / `_STATUS_*` / `_TEX_*` / `_ALPHA_*` / `_UV_*` (`pbr_abi.h:44-74`) | `Pbr.cs:6, 20, 31, 41, 52` | **nothing** |
| 12 | `AVER_VOXI_FEATURE_*` / `_STATUS_*` / `_QUALITY_*` (`voxi_abi.h:31-48`) | `Voxi.cs:6, 17, 27` | **nothing** |

Rows 4 and 6–12 fail silently and at run time, in whatever feature happens to use the wrong number: a component that is never attached, a pawn flag that never matches, an actor placed in the wrong tick group, a key that never fires. None of it throws.

### Two stale comments in the C# tree
- **`scripting/csharp/Aver.Framework/Native.cs:16-18`** asserts that "The in-progress `Aver.Scene/Native.cs` currently uses ANSI `LPStr`; that is the defect". No longer true — `Aver.Scene/Native.cs` uses `LPUTF8Str` throughout and decodes with `PtrToStringUTF8` (`:36, 53, 67, 71, 76`). The file that still uses ANSI is `Aver.Scripting/Pbr.cs`.
- **`scripting/csharp/Aver.Framework/Enums.cs:3-8`** asserts that "framework_abi.h defines no `AVER_FW_BEGIN_*`/`END_*`/`PLAY_STATE_*` macros yet — their native pinning lands when the lifecycle entry points stop being stubs". It has landed: `framework_abi.h:174-176` defines `AVER_FW_PLAY_*` today and `framework_hooks.h:58-65` defines `AVER_FW_BEGIN_*`/`AVER_FW_END_*`, with `framework_hooks.h:51-53` explicitly calling itself "the native side of that pinning the C# header promised". A binding author reading `Enums.cs` first will believe those three enums are free to renumber. They are not.

### The version boundaries that are not checked
- **No shipping code path queries any module ABI version.** The two callers are tests. See §13; this is the single most consequential gap in this document.
- **Three of the six seams have no version to query.** `physics_abi.h`, `pbr_abi.h` and `voxi_abi.h` declare no constant and export no function.
- **The bind-time sanity check specified at `docs/SCENE_FRAMEWORK.md:1722-1723` is PLANNED, not implemented.**
- **Three contract constants are hand-duplicated across languages** with something checking them at run time (rows 1–3 above); the other nine mirrors have nothing at all.

### Test coverage — real, uneven, and never run automatically
There is genuine C ABI test coverage here, more than this project's history would lead you to expect, and it is worth stating exactly what it is and is not.

| Test | Size | `check()` calls | Distinct entry points driven |
|---|---|---|---|
| `tests/scene/src/SceneTest.cpp` | 971 lines | 274 | 36 `aver_scene_*` — every export in the header, plus the undeclared `aver_scene_debug_string_pool_size` |
| `tests/framework/src/FrameworkTest.cpp` | 825 lines | 219 | 44 `aver_fw_*` — registry, defaults, spawn, possession, play lifecycle, input, the dispatch install/clear pair, `aver_fw_tick` |
| `tests/physics/src/PhysicsTest.cpp` | 279 lines | 43 | 21 `aver_phys_*` |

- **Five framework entry points are not among those 44, and two of them are the newest surface on the seam.** Extracting the distinct `aver_fw_*` names called by `FrameworkTest.cpp` leaves out **`aver_fw_set_view_entity`** and **`aver_fw_view_entity`** — precisely the pair that caused the 1.0 → 1.1 minor bump and that carries the header's longest rationale (`framework_abi.h:242-259`) — along with **`aver_fw_class_get_flags`**, **`aver_fw_class_reset`**, **`aver_fw_abi_version`** and **`aver_fw_scene_abi_version`**. The view pair is bound and used by the editor; it is simply not exercised by any test.
- **The PBR and Voxi seams have no test at all.** A grep for `aver_pbr_` and `aver_voxi_` across the whole `tests/` tree returns **zero** matches. `MaterialTest.cpp` tests the `.ocmat` reader/writer and the mip filter and links `Aver.Render.PBR.Materials` for `packMaterial`, but never calls a C ABI entry point. Those two seams are also the only ones bound by the `Aver.Scripting` assembly.
- **The host ↔ bridge contract has no test either.**
- **The C# side of every seam is covered by nothing.** There is no managed test project. The P/Invoke declarations are checked by nothing — not names, not types, not string encodings, and not one of the twelve constant mirrors above. A misspelt entry point surfaces as an `EntryPointNotFoundException` on first call, at run time, in whatever feature happened to touch it.
- **Nothing runs any of them automatically.** `AVER_BUILD_TESTS` defaults ON (`CMakeLists.txt:32`) and the four test directories are added at `CMakeLists.txt:121-131`, so the executables are **built** by a default build. But there is no `enable_testing()` and no `add_test()` anywhere in the tree, so none is registered with CTest. `scripts/gates.ps1` drives `Sandbox.exe` over the render gates and never invokes them; `scripts/build.ps1` does not reference them. **A built binary is not a run binary. Whether these tests passed on any given commit is not recorded anywhere in this repository.**

### What the C# bindings cover, and where they diverge

| Assembly | Seam | Bound / exported | Not bound |
|---|---|---|---|
| `Aver.Scene/Native.cs` + `Aver.Framework/Native.cs` (`SceneNative`) | Scene | **34 / 35** | `aver_scene_abi_version` |
| `Aver.Framework/Native.cs` + `ManagedDispatch.cs` | Framework (+ hooks) | **41 / 50** | `aver_fw_abi_version`, `aver_fw_scene_abi_version`, `aver_fw_scene_abi_matches`, `aver_fw_begin_play`, `aver_fw_end_play`, `aver_fw_set_paused`, `aver_fw_find_class_with_flags`, `aver_fw_view`, `aver_fw_tick` |
| `Aver.Framework/Physics.cs` | Physics | **29 / 36** | `aver_phys_init`, `_shutdown`, `_step`, `_set_fixed_step`, `_add_convex_hull`, `_add_mesh`, `_add_heightfield` |
| `Aver.Scripting/Pbr.cs` | PBR | **44 / 48** | `aver_pbr_get_reflectance`, `_set_reflectance`, `_get_f90`, `_set_f90` |
| `Aver.Scripting/Voxi.cs` | Voxi | **21 / 21** | — the only seam with complete managed coverage |

(Counts produced by diffing each header's exported names against the `DllImport` names in each assembly.) The unbound lifecycle and session entry points are a coherent split — the native app owns them. The unbound **version** functions are the finding in §13. The three unbound bulk-geometry constructors mean a script cannot build a hull, mesh or heightfield collider today.

**String encoding disagrees across the tree, and the headers caused it.** `Pbr.cs` marshals inbound strings as `[MarshalAs(UnmanagedType.LPStr)]` — ANSI — at lines 73, 80 and 110, and decodes outbound pointers with `Marshal.PtrToStringAnsi` at line 117; `Voxi.cs:62` does the same. Every scene and framework binding uses `LPUTF8Str` / `PtrToStringUTF8`. This is **not** a stack-shape mismatch and will never corrupt anything — a `char*` is a `char*` either way — but it silently mangles any non-ASCII material name or texture path, re-encoding it in the process ANSI code page. The root cause is that `pbr_abi.h` and `voxi_abi.h` are **silent on encoding** and on ownership alike: they say `const char*` and nothing more, so the binding author had nothing to bind against. `scene_abi.h:100-104` is the model of how it should be stated. Voxi's **two** outbound strings (`voxi_abi.h:52, :54`) are fixed English feature and status names, so it is currently harmless there — until one is localised. PBR has seven, and they include user-authored material names and texture paths, where it is not harmless.

**Calling convention is pinned nowhere on the P/Invoke seams.** `framework_hooks.h:32-40` pins `__cdecl` for the dispatch tables and `HostBridge.cs:96` pins `CallConvCdecl` on the reverse entries — both because a mismatch there would corrupt the stack on the first call. The five P/Invoke headers say nothing, and every `DllImport` correspondingly leaves `CallingConvention` at its default of `Winapi` (noted approvingly at `Aver.Scene/Native.cs:8`). This is safe on x64, where there is one convention, **and only on x64**. No csproj sets `PlatformTarget` (they are AnyCPU), so nothing records the assumption.

### What the headers do not say
- **String lifetime and, on two seams, ownership.** Lifetime is stated nowhere; the implementations make it load-bearing (§11). Ownership is stated for scene (`scene_abi.h:100-104`) and framework (`framework_abi.h:87-88`) and **not at all** for PBR's seven and Voxi's two outbound `const char*`. The rule for all seams should be: decode the returned pointer **immediately**, before the next call into that DLL, and never free it.
- **Threading — and it is not uniform, so do not state it uniformly.** Not one of the five P/Invoke headers says which thread its entry points may be called from. Two C++ headers behind them say it outright, and they should be quoted rather than inferred:
  - `modules/scene/include/aver/scene/World.hpp:18-20` — "Not thread-safe, matching the rest of the module tier: every entry point is called on the frame thread between flush points, and a lock here would be taken millions of times a second to protect against a caller that does not exist."
  - `modules/render.pbr/include/aver/pbr/Material.hpp:145-146` — "Not thread-safe, matching the rest of the module tier: the editor and the render thread reach it through the frame's own ordering, not through a lock."

  The rest of the code agrees: the string pool is a function-local `static` vector with no lock (`SceneAbi.cpp:93-96`) and the class registry is in the same style (`FrameworkAbi.cpp:154-166`).

  **Physics is the exception, and it matters.** `modules/physics/src/PhysicsWorld.cpp:142-146` holds a `std::mutex eventMutex` beside the contact and overlap vectors, with the comment "Event queues, written from Jolt's worker threads under the mutex and drained by the caller between steps." So the seam is main-thread-only *for the caller*, but the queues behind it are genuinely cross-thread and that mutex is doing real work. A blanket "everything is main-thread-only, so the locks are dead weight" would be a correct-sounding way to introduce a data race. Within the headers themselves, a grep for "thread" matches only `physics_abi.h:140-148` and `:166-167`, and those passages explain why Jolt's callbacks are not surfaced and warn that broadphase queries are non-deterministic — a rationale for a design, not a statement of the caller's obligation. **Treat every P/Invoke entry point as main-thread-only, know that no P/Invoke header says so, and do not conclude from that anything about the internals.**
- **Reentrancy.** A grep for "reentran" across all six headers returns nothing, and the question is live rather than academic: `framework_hooks.h:174-180` has the framework calling *up* into managed code from `aver_fw_tick`, and that managed code will call straight back down through `aver_scene_*` and `aver_fw_*` while the tick is in progress. Whether it may spawn or destroy during a `tick_all`, and what happens to the dense instance list if it does, is answered nowhere. The nearest thing to an answer is `scene_abi.h:143`, which covers destroy but not the general case.
- **Buffer lengths.** Never passed, never validated, unfixable within the current signatures. See §11.
- **Initialisation order.** `scene_abi.h:95-97` says the World is created lazily on first use, so any call bootstraps it. `physics_abi.h:27` requires an explicit `aver_phys_init` and offers `aver_phys_ready` (`:29`), but does not say what the other 30 entry points return if called first. `pbr_abi.h` and `voxi_abi.h` say nothing about initialisation at all. The orderings are inferable from the sources but are not part of the stated contract.
- **Cross-seam lifetime: what a material token means once the material is gone.** `aver_scene_material` hands back a scene-local interned token that the render side maps to a real material (`scene_abi.h:188-193`), and `aver_pbr_destroy` can retire that material at any time. Whether a `CMeshRenderer.material` token then dangles, falls back to the default material, or renders nothing is stated in no header on either side of the seam, and no test covers it. It is the first question a gameplay author hits at the Scene/PBR boundary, and today the answer has to be read out of the renderer.
- **The `aver_voxi` "msaa dirty flag"** that `pbr_abi.h:151` cross-references does not exist on the Voxi ABI; the analogue is C++-only (`Voxi.hpp:95`).

### Planned, not built
- **`AvActorVTable` and the native per-class tick path.** The struct is declared at `framework_hooks.h:91-98`; the registrar `aver_fw_class_set_vtable` and the tick loop are a documented follow-up, and nothing reads the table today (`framework_hooks.h:80-83`; `FrameworkAbi.cpp:807`). The registrar's only appearance in **code** is the comment at `framework_hooks.h:82` — no header declares it, no source defines it. It is named four times in the documentation: `docs/SCENE_FRAMEWORK.md:1223` carries a planned declaration returning `aver_class` rather than the tree-wide `int32_t` 1/0, `:1876` and `:2122` discuss it, and `docs/DESIGNER_REWRITE.md:183` already records the return-type contradiction and the conclusion that the header should return `int32_t` 1/0. Anyone implementing it should implement the corrected signature.
- **The bind-time version check** (`docs/SCENE_FRAMEWORK.md:1722-1723`).
- **The consolidated `Aver.ABI`** described in `abi/README.md`, `modules/abi/README.md` and `docs/ARCHITECTURE.md` §7 is not planned. It has been **dropped**. Those files are stale.
- **Rust bindings** promised by `interop/README.md:3` do not exist and there is no Rust in this repository.

---

**Related documents.** `docs/SCRIPTING_API.md` is the C# reference for the managed surface these seams sit under — read it, not this, for `AverActor`, `Entity`, `Game`, `Input` and `Physics`. `docs/SCENE_FRAMEWORK.md` covers the scene/framework design in depth, and contains full copies of both headers and of the C# bindings (`:854`, `:1081`, `:1223`, `:1465`, `:1624`) that are not regenerated from anything — treat this document and the headers as authoritative where they disagree. `docs/ARCHITECTURE.md` is the module DAG; its §7, "The C ABI boundary", describes the single consolidated seam and is stale — §1 above supersedes it.