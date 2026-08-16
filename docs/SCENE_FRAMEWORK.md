# Aver.Scene and Aver.Framework — the design

Two modules: `Aver.Scene`, a data-oriented entity/component world, and `Aver.Framework`, the
gameplay layer — GameInstance, GameMode, GameActor, Pawn, PlayerController — expressed *over* that
world rather than as a class hierarchy inside it.

> **This is a design record, and it runs well ahead of the tree.** Of section 8's fourteen steps,
> **only step 1 is built** (commit `03b79e1`): both modules exist, both build as SHARED DLLs, both
> are wired into the top-level `CMakeLists.txt` and the sandbox, and `scene_abi.h` and
> `framework_abi.h` exist — containing *only* their version entry points. There is no entity, no
> component pool, no transform, no class registry, no `Scene.cs` and no `Framework.cs`. Every other
> header, C# file and CMake fragment below is a specification to be typed, not a file to be read.
> Section 8 is the order to type them in, section 9 is what this design does not know, and section
> 10 is where the design contradicts itself.

`docs/SCRIPTING.md` §5 already says the honest version of this: there is no scene API, and none was
written on purpose, because an interim object API would have to be replaced wholesale and would break
every script authored against it. This document is what that replacement is meant to be.

---

## 1. What this is, and why it is shaped this way

### 1.1 The constraint that shapes everything

`modules/scene/README.md` describes the module in seven words that decide the rest of the design:

> Data-oriented entity/component world (EnTT-style) … Render/physics-agnostic (**no UObject**).

That is not a stylistic preference to be honoured loosely. It means gameplay concepts cannot be
expressed the way an engine descended from UObject expresses them — as a base class with virtual
functions, reflected properties and a class-default object. So they are expressed three other ways
instead:

| Unreal concept | Here |
|---|---|
| a base class (`AActor`, `APawn`) | **a component signature** — a flattened list of component types |
| a class-default object | **a data class-registry row** — one contiguous blob of default bytes |
| `AGameModeBase*`, `UGameInstance*` | **singleton records** — one entity each, found by a flag |
| `IsA<APawn>()` | **a flag bit**, checked at the seam |
| `NewObject`/`SpawnActor` constructor chain | **a loop of `memcpy`** over the sealed blob |

There is no `AGameActor` type in this design. There is no virtual inheritance anywhere in the C++.
A **class is a row in a registry**; an **actor is an entity that has a row**; and the differences
between Actor, Pawn, PlayerController and GameMode are flag bits on that row.

`aver_fw_class_of(e) != 0` is the complete definition of "this entity runs code". An entity without a
class is a plain piece of scene data with a transform — which is exactly what a static mesh should be,
and exactly what the same thing costs in an engine where a static mesh is an `AActor` carrying a
`UStaticMeshComponent`.

### 1.2 The chosen architecture

A **spawn-class / archetype** model — class defaults with per-instance overrides, a GameMode naming
its `DefaultPawnClass` by name, an Add menu built from the live class registry, and
`Spawn(ActorClass.Find("AN_Spinner"), pos)` producing a complete actor — built on a **two-target
split**: `Aver.Scene` (SHARED, Core + Assets) and `Aver.Framework` (SHARED, Core + Assets + Scene),
with **two separate ABI headers**.

The archetype half is what a user coming from Unreal actually asked for, and its native shape is a
transcription of the existing tree rather than a reinterpretation: the CMakeLists is `render.pbr` with
the names changed, the handle is a raw `u32` with a `k`-constant, `scene_abi.h` is `pbr_abi.h`'s
`#define` blocks and 1/0 setters, and the C# facade lives in the assembly `Pbr.cs` and `Voxi.cs`
already live in.

The two-target split is the repair that model needed. On its own it has no framework module, so
`AVER_SCENE_CLASS_PAWN`, `aver_scene_possess` and `aver_scene_world_begin_play` end up sitting in the
generic storage header, and *"Aver.Scene without the framework"* stops being a question anyone can
ask. Splitting it later is an ABI break; splitting it before the first line costs a link line. So the
whole gameplay vocabulary — class registry, `memcpy` spawn, idempotent-by-name declare, `tick_all` —
moves up one module, and the link line enforces it rather than a comment.

The result: `scene_abi.h` can be read end to end without encountering **actor**, **class**, **spawn**,
**possess** or **play**.

### 1.3 Three decisions that went against the obvious reading

**SHARED links SHARED.** The tree's rule is that a SHARED module depends on Core (+ Assets) only, and
`Aver.Framework` SHARED linking `Aver.Scene` SHARED has no precedent in the tree. The two DLLs stay,
and the reason is the rule's own stated purpose, verbatim in `modules/render.pbr/CMakeLists.txt:5-6`:
no RHI type may sit behind a P/Invoke DLL — *"the moment `aver/rhi/*` appears behind this target, the
scripting boundary is broken"*. `Aver.Framework`'s full transitive closure is {Core, Assets, Scene},
and `Aver.Scene`'s is {Core, Assets}. There is no RHI anywhere behind either boundary, so the property
the constraint protects holds exactly. Collapsing the two into one DLL would satisfy the letter of the
rule and destroy the modularity it exists to produce.

The build-shape risk is real and is **retired by step 1's verification** — an import-lib link plus a
load-order smoke test — before anything depends on it, rather than by this paragraph.

**No function pointers in a P/Invoke header.** Putting a script-hook table in `scene_abi.h` is a real
defect, so there are **three** headers, not two. `scene_abi.h` and `framework_abi.h` are pure P/Invoke
surfaces containing zero function pointers, zero `void*`, zero structs and zero enums — only
`int32_t` / `int64_t` / `float` / `const char*` and pointers to those as out-params. The dispatch
tables live in a third header, `framework_hooks.h`, which no C# code ever marshals and which follows
`scripting_abi.h`'s `structBytes` + `contractVersion` idiom instead of `pbr_abi.h`'s.

**Managed actors are not dispatched per entity.** A per-class vtable is the right answer for *native*
classes and the worst possible answer for managed ones: a reverse-P/Invoke plus a dictionary lookup
per actor per frame. So native classes tick through a hoisted per-class vtable; managed classes are
flagged and skipped by that loop entirely, and the framework makes exactly **one** managed call per
tick group per frame — `tick_all(group, dt)` — letting the bridge walk its own dense list. Three
transitions per frame, not three per actor.

### 1.4 The smaller decisions, and what each one costs

These were taken before the first line specifically because each of them is a v2 ABI break if taken
after it.

- **The exposed-value kind set is wide from the start** — `F32, Vec3, Quat, I32, Bool, I64, Entity,
  String, Mat4`. A narrower set cannot express `[Editable] public bool Clockwise` in this design's own
  first example, and a Details panel that cannot show a toggle, a count or a colour is not a Details
  panel.
- **The visual reference is opaque.** `CMeshRenderer` holds a `u64` mesh ObjectId and an `i32`
  material that Scene never dereferences. Naming `pbr::MaterialHandle` there would be an undeclared
  Scene → Render.PBR edge behind a Core+Assets link line, and the ABI already crosses the same value
  as `int32_t`.
- **Entity is Core's `AvId`** (`Types.hpp:20-23`), which was declared with the anti-UObject comment
  for exactly this job and which a tree-wide grep shows nothing has ever used. Reviving it avoids a
  fourth id family alongside `AvId`, the RHI's nine `u32` aliases and `ObjectId`.
- **The sparse array stores `dense slot + 1`**, so `0` means "absent" inside storage too and the
  tree's one invalid-value rule holds all the way down rather than stopping at the handle.
- **One named, typed field table per component**, terminated by `.verify(sizeof(T))`, serving the
  generic get/set ABI, save/load and the editor's Details panel from a single mechanism. The panel
  stops carrying per-component code and does not know what a light is.
- **Play-in-editor is `snapshot_ = pools_`** — a `memcpy` per flat vector in both directions. No
  snapshot format, no transient-field annotation, no pointer fixup. A delta-from-class-default
  snapshot is smaller and is more code and more failure modes; the delta is dropped.
- **The C# constant surface is generated from the live native registry**, not hand-written. A
  `private const int CompMesh = 4;` in `ClassBuilder` is where drift starts.
- **Strings are UTF-8 in both directions**, stated in the header and marshalled correctly both ways.
  `voxi_abi.h` and `pbr_abi.h` state no encoding at all, and their bindings consequently read strings
  back with `PtrToStringAnsi` and write them as `LPStr`.
- **Batched accessors exist from day one** (`get_locals` / `set_locals` / `get_world_matrices`),
  because they are the correct escape hatch for any generic per-field ABI and retrofitting them after
  ten thousand lines have been written against the per-entity form is not the same work.
- **The gameplay C# ships inside the existing `Aver.Scripting.dll`**, bumped to 1.1.0.
  `HostBridge.cs:243` skips exactly two simple names and `SampleBehaviour.csproj` sets
  `Private="false"` specifically so a second engine identity is never staged next to user code. A
  third assembly reintroduces the two-runtime-identities failure the bridge exists to prevent.
  `CheckApiVersion`'s `referenced.Major != loaded.Major || referenced > loaded` keeps 1.0.0-built
  assemblies accepted.
- **`aver_fw_class_declare` is idempotent by name**, and it must never be optimised into a
  generation-bumped handle later. It is the entire mechanism that makes hot reload survivable with no
  managed bookkeeping.
- **`tick_all` dispatches a whole tick group in one managed transition**, and the header says so, so
  that nobody adds a per-entity tick entry point in a year's time.

---

## 2. Module layout

### 2.1 Directories

```
modules/scene/
  CMakeLists.txt
  README.md                                  (rewritten: this module has no gameplay vocabulary)
  include/aver/scene/Entity.hpp
  include/aver/scene/ComponentPool.hpp
  include/aver/scene/Fields.hpp
  include/aver/scene/World.hpp
  include/aver/scene/scene_abi.h             <- P/Invoke surface
  src/World.cpp  src/ComponentPool.cpp  src/Fields.cpp  src/Builtins.cpp  src/SceneAbi.cpp
  src/SceneRender.cpp                        <- the ONLY file that includes aver/rhi/*

modules/framework/
  CMakeLists.txt
  README.md
  include/aver/framework/ClassRegistry.hpp
  include/aver/framework/Framework.hpp
  include/aver/framework/framework_abi.h     <- P/Invoke surface
  include/aver/framework/framework_hooks.h   <- dispatch tables; NOT a P/Invoke surface
  src/ClassRegistry.cpp  src/Framework.cpp  src/DefaultClasses.cpp  src/FrameworkAbi.cpp

tests/scene/   (new)  src/SceneTest.cpp
tests/framework/ (new) src/FrameworkTest.cpp

scripting/csharp/Aver.Scripting/Scene.cs        (namespace Aver.Scene)
scripting/csharp/Aver.Scripting/Framework.cs    (namespace Aver.Framework)
scripting/csharp/Aver.Scripting/Generated/SceneIds.g.cs   (emitted by the editor from the live registry)
scripting/csharp/Aver.Scripting.Bridge/HostBridge.Actors.cs
```

### 2.2 Targets and link lines

```cmake
  # modules/scene/CMakeLists.txt — render.pbr with the names changed.
  # SHARED for the reason PBR is: the C# layer P/Invokes this DLL, and keeping the world in one
  # binary means the editor and the bindings address the same entities.
  # aver_add_module() is STATIC-only, so a plain add_library is used here.
  add_library(Aver.Scene SHARED
    src/World.cpp src/ComponentPool.cpp src/Fields.cpp src/Builtins.cpp src/SceneAbi.cpp)
  target_include_directories(Aver.Scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(Aver.Scene PUBLIC cxx_std_20)
  target_link_libraries(Aver.Scene PUBLIC Aver.Core Aver.Assets)
  target_compile_definitions(Aver.Scene PRIVATE AVER_SCENE_BUILD)
  target_compile_definitions(Aver.Scene PUBLIC AVER_MODULE_SCENE=1)
  set_target_properties(Aver.Scene PROPERTIES
    FOLDER "modules" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # The GPU half, deliberately a SECOND target, STATIC, linked straight into the executable.
  # Aver.RHI is the GENERIC interface. NEVER Aver.RHI.D3D12.
  add_library(Aver.Scene.Renderer STATIC src/SceneRender.cpp)
  target_include_directories(Aver.Scene.Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(Aver.Scene.Renderer PUBLIC cxx_std_20)
  target_link_libraries(Aver.Scene.Renderer PUBLIC Aver.Core Aver.RHI Aver.Scene)
  set_target_properties(Aver.Scene.Renderer PROPERTIES FOLDER "modules")

  # modules/framework/CMakeLists.txt
  # SHARED for the same P/Invoke-identity reason, and it links Aver.Scene because gameplay is
  # ABOVE storage and the arrow must point down. The rule this shape is tested against is the one
  # render.pbr states: no RHI type behind a P/Invoke DLL. The transitive closure here is
  # {Core, Assets, Scene} — there is no RHI anywhere behind this boundary.
  add_library(Aver.Framework SHARED
    src/ClassRegistry.cpp src/Framework.cpp src/DefaultClasses.cpp src/FrameworkAbi.cpp)
  target_include_directories(Aver.Framework PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(Aver.Framework PUBLIC cxx_std_20)
  target_link_libraries(Aver.Framework PUBLIC Aver.Core Aver.Assets Aver.Scene)
  target_compile_definitions(Aver.Framework PRIVATE AVER_FW_BUILD)
  target_compile_definitions(Aver.Framework PUBLIC AVER_MODULE_FRAMEWORK=1)
  set_target_properties(Aver.Framework PROPERTIES
    FOLDER "modules" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
```

### 2.3 Why each target is what it is

| Target | Kind | Reason |
|---|---|---|
| `Aver.Scene` | **SHARED** | C# P/Invokes it. One binary holding the world means the editor and the bindings address the same entities — the same argument `modules/scripting` already makes about hosting the CLR in-process (`docs/SCRIPTING.md` §6). A static copy per consumer is two worlds that cannot see each other. |
| `Aver.Scene.Renderer` | **STATIC** | It includes `aver/rhi/*`. Keeping it out of the SHARED target is what keeps RHI off the P/Invoke boundary, which is the rule `render.pbr` states and the only reason the SHARED-links-SHARED shape is defensible at all. Linked straight into the executable. |
| `Aver.Framework` | **SHARED** | Same P/Invoke-identity reason. Its transitive closure is {Core, Assets, Scene}; no RHI. |

`aver_add_module()` is STATIC-only, so both DLLs use a plain `add_library`, exactly as `render.pbr`
does.

### 2.4 Top level

Two hand-typed lines, in this order. The DAG is maintained by hand here; there is no lint.

```cmake
  option(AVER_MODULE_SCENE "Build the entity/component world" ON)
  option(AVER_MODULE_FRAMEWORK "Build the gameplay framework" ON)
  if(AVER_MODULE_FRAMEWORK AND NOT AVER_MODULE_SCENE)
    message(STATUS "Aver: the framework stands on the scene and cannot be built without it - forcing AVER_MODULE_FRAMEWORK=OFF")
    set(AVER_MODULE_FRAMEWORK OFF CACHE BOOL "" FORCE)
  endif()
  if(AVER_MODULE_SCENE)     add_subdirectory(modules/scene)     endif()
  if(AVER_MODULE_FRAMEWORK) add_subdirectory(modules/framework) endif()
```

The Sandbox links `Aver.Scene`, `Aver.Scene.Renderer` and `Aver.Framework` by hand under
`if(TARGET ...)`, the way it already names `Aver.Scripting.Host` and the Voxi feature. **The executable
owns the instances and ticks them**; `Aver.Runtime` is not touched, because a subsystem the app
configures is not something the composition root should reach into
(`sandbox/CMakeLists.txt:22-27`).

`modules/platform/CMakeLists.txt` gains two `SOURCES` lines: `src/DirectoryWatcher.cpp` and
`src/win32/Win32DirectoryWatcher.cpp`. They are already on disk and, because `aver_add_module` never
globs, they compile into nothing today and the omission produces no diagnostic. This is step 14.

### 2.5 The documentation delta this design owes

`modules/framework` does not exist in `docs/ARCHITECTURE.md`'s resolved edge list (`:70-94`). The
delta is **`Framework -> Core, Assets, Scene`** at Tier 5, plus a table row stating it holds no RHI.
`ARCHITECTURE.md:142` currently allocates gameplay to C# above the ABI seam; that stays true — the
framework is the *mechanism*, the game classes are C#.

**That delta is applied**, in step 1's commit `03b79e1` alongside the first CMakeLists, where it
belongs. It went in as three edge-list rows rather than one, because `Scene.Renderer` needed naming
too and the `Scene` row had to gain the note that it is SHARED with no RHI behind its P/Invoke
boundary.

`ARCHITECTURE.md` also gained a paragraph the plan did not ask for, recording that the
SHARED-links-SHARED edge is **verified rather than assumed** — and that the first attempt did not
have it at all. See §8, step 1.

---

## 3. The scene layer

### 3.1 Entity — `modules/scene/include/aver/scene/Entity.hpp`

```cpp
#pragma once
#include "aver/core/Types.hpp"

namespace aver::scene {

// Entities ARE Core's AvId — the type Types.hpp:20-23 declared for exactly this purpose with the
// anti-UObject comment attached, and which a tree-wide grep shows nothing has ever used. Reviving
// it is worth more than a fourth id family alongside AvId, the RHI's nine u32 aliases and ObjectId.
//
// Layout: index in bits 0..23, generation in bits 24..30, bit 31 always clear. Three things fall
// out of that and all three are load-bearing rather than decorative. Index 0 is never handed out
// and a live generation starts at 1, so no live handle can equal 0. Bit 31 stays clear, so the same
// value crosses the C ABI as a POSITIVE int32_t and a live entity can never be mistaken for an
// error return. And the whole handle is one u32, so a component pool's owner array is dense.
using Entity = AvId;
inline constexpr Entity kInvalidEntity = kInvalidId;

inline constexpr u32 kEntityIndexBits = 24;
inline constexpr u32 kEntityIndexMask = (1u << kEntityIndexBits) - 1u;
inline constexpr u32 kEntityMaxGen    = 127u;              // 7 bits; 0 means "this slot never lived"
inline constexpr u32 kMaxEntities     = kEntityIndexMask;  // index 0 is reserved, never allocated

inline constexpr u32    entityIndex(Entity e) { return e & kEntityIndexMask; }
inline constexpr u32    entityGen(Entity e)   { return (e >> kEntityIndexBits) & kEntityMaxGen; }
inline constexpr Entity makeEntity(u32 index, u32 gen) {
    return (index & kEntityIndexMask) | ((gen & kEntityMaxGen) << kEntityIndexBits);
}

} // namespace aver::scene
```

**Generation wrap is 7 bits, which is few.** It is handled rather than asserted away: the free list is
FIFO so an index gets maximum distance before reuse, and a slot whose generation would wrap past 127
is **retired** instead of recycled, with a counter surfaced in the status bar. 16.7M indices makes
retiring affordable; widening the field would mean giving up either the index range or the
bit-31-clear rule, and both are load-bearing.

### 3.2 Component storage — `include/aver/scene/ComponentPool.hpp`

```cpp
#pragma once
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"
#include <vector>

namespace aver::scene {

// A sparse set, hand-rolled. Core has no container facility and no SlotMap or SparseSet exists
// anywhere in modules/, so there is nothing to copy and a dependency would be the wrong trade for
// ~200 lines over std::vector. EnTT is out for a harder reason: entt::entity's null is not zero.
//
// A storage archetype is deliberately NOT used. "Archetype" in this design means a SPAWN recipe;
// storage stays per-type, so adding a component to a live entity never moves another entity's
// memory and no chunk allocator has to be written first.
class ComponentPool {
public:
    ComponentPool(u32 typeId, usize stride, usize align);

    bool  has(Entity e) const;
    void* get(Entity e);            // nullptr when absent — this tree does not throw
    void* add(Entity e);            // zero-filled; returns the existing one if already present
    bool  remove(Entity e);         // swap-and-pop; bytes_ and dense_ stay packed

    usize  size() const     { return dense_.size(); }
    Entity entityAt(usize i) const { return dense_[i]; }
    void*  dataAt(usize i)  { return bytes_.data() + i * stride_; }
    usize  stride() const   { return stride_; }
    u32    typeId() const   { return typeId_; }

    // Play-in-editor is a memcpy in both directions because these three vectors are the entire
    // state of a component type. There is no snapshot format and nothing to fix up.
    void snapshotTo(ComponentPool& dst) const;

private:
    u32   typeId_;
    usize stride_;
    // Entity INDEX -> dense slot + 1. The +1 is not a micro-optimisation: it makes 0 mean "absent"
    // inside storage too, so the tree's one invalid-value rule holds all the way down.
    std::vector<u32>    sparse_;
    std::vector<Entity> dense_;
    std::vector<u8>     bytes_;
};

} // namespace aver::scene
```

### 3.3 Field tables — `include/aver/scene/Fields.hpp`

```cpp
#pragma once
#include "aver/core/Types.hpp"
#include <vector>

namespace aver::scene {

// Kinds are wider than a spawn recipe strictly needs, because growing them later is a v2 ABI
// decision rather than a patch, and a Details panel that cannot show a toggle or a colour is not a
// Details panel. Arity is floats-per-value for the float kinds and 0 for the rest.
enum class FieldKind : u32 {
    F32 = 0, Vec3 = 1, Quat = 2, I32 = 3, Bool = 4, I64 = 5, Entity = 6, String = 7, Mat4 = 8
};

struct FieldDesc {
    const char* name;
    FieldKind   kind;
    u16         offset;   // byte offset into the component struct
    u8          arity;    // 1, 3, 4, 16, or 0
};

// Registration is hand-written next to the struct with offsetof — no codegen, so there is nothing
// to keep in sync at build time. One table then serves the generic get/set ABI, scene save/load AND
// the editor's Details panel, so there is no second place to update and therefore no second place
// to forget. The .verify(sizeof(T)) terminator catches drift where it happens rather than three
// layers away: the summed field extents must cover the struct.
class ComponentBuilder {
public:
    ComponentBuilder& field(const char* name, FieldKind kind, u16 offset, u8 arity = 1);
    void              verify(usize structBytes);
private:
    friend class World;
    u32 typeId_ = 0;
};

} // namespace aver::scene
```

This one table is the anti-coupling mechanism of the whole design. It is attached to `Aver.Scene`'s
generic pool while `Aver.Framework` registers its *own* tables through the same API, so the mechanism
is generic and the vocabulary stays upstairs. It also replaces `(componentType, fieldIndex)` magic
integers with resolved dense ids on both sides of the ABI.

### 3.4 Transform and hierarchy, with dirty propagation

Two components, and the split is the point: `CLocal` is authored data the editor, the gizmo and
scripts write; `CWorld` is derived data exactly one pass writes.

```cpp
struct CLocal     { Transform xf; u32 rev = 1; };      // Core's Transform: cm, +Z up, LH
struct CWorld     { Mat4 m; u32 composedLocalRev = 0; u32 composedParentRev = 0; u32 rev = 1; };
struct CHierarchy { Entity parent = 0, firstChild = 0, nextSibling = 0, prevSibling = 0; u32 depth = 0; };
struct CName      { u64 nameId = 0; u32 offset = 0; u32 len = 0; };   // fnv1a64 + slice into the name blob
struct CTags      { u32 bits = 0; };                   // never interpreted here; see the ownership table
```

Composition obeys the contract exactly — row-vector, left-to-right, so a child reaches world space
through its own local matrix first:

```cpp
world = local.toMatrix() * parentWorld;    // v * (L * P)
```

which is the same ordering `Transform::toMatrix()` already uses internally at `Math.hpp:210`. There is
no other correct order under `v*M` and getting it backwards is the single most likely silent bug here.

**Propagation is one linear pass over `order_`**, a topological array in which every parent precedes
its children. It is rebuilt **only** when a parent link changes (a Kahn sweep over the hierarchy pool),
not per frame. Within the pass, dirt is a revision compare rather than a bitset walk: an entity
recomposes when `local.rev != world.composedLocalRev` or
`parentWorld.rev != world.composedParentRev`, and bumps its own `world.rev` when it does. A moved root
therefore recomposes its whole subtree and nothing else — dirt propagates downward for free, because a
child's test reads its parent's revision that the same pass has already updated.

`setParent` walks the ancestor chain and **rejects a cycle** (returns 0) rather than corrupting the
sort, and marks `topoDirty_`. Reparenting itself is four pointer writes into the intrusive links and
allocates nothing.

Gameplay reads a world matrix mid-tick, before the frame's pass has run, so `World::worldMatrix(e)`
has an **on-demand path**: walk up to the nearest ancestor whose revisions agree, then compose down.
It is bounded by depth, it is the same arithmetic as the pass, and it means an actor never sees a
stale matrix just because it happened to tick before the sweep.

### 3.5 Identity

Runtime identity is `Entity` and it is **never serialised** — a generational handle must not reach a
file. Persisted identity is `CName`'s ObjectId (Assets' fnv1a64, the same kind of thing an asset id
is), so a saved world stores `(className, instanceName, overrides)` and the loader hands back a fresh
entity.

### 3.6 World — `include/aver/scene/World.hpp`

```cpp
class World {
public:
    static World& instance();                 // one per process; see scene_abi.h for why no handle

    Entity create(std::string_view name);
    Entity spawn(std::string_view name, Entity parent, const Transform& local);
    bool   destroy(Entity e);                 // deferred to flush(); recursive over CHierarchy
    bool   valid(Entity e) const;
    u32    count() const;
    Entity at(u32 denseIndex) const;

    ComponentBuilder registerComponent(std::string_view name, usize stride, usize align);
    template <class T> ComponentBuilder registerComponent(std::string_view name);
    u32   componentId(std::string_view name) const;      // 0 when unregistered
    i32   fieldId(std::string_view qualifiedName) const; // "CMovement.maxSpeed" -> dense id, 0 invalid

    // The C++ surface Aver.Framework uses. void* never crosses the C ABI; it crosses a header, in
    // process, between two modules that were compiled together — which is a different thing.
    void* addComponent(Entity e, u32 type);
    void* getComponent(Entity e, u32 type);
    bool  removeComponent(Entity e, u32 type);
    ComponentPool* pool(u32 type);

    const Mat4& worldMatrix(Entity e);        // composes on demand if stale; see above
    u32   flush();                            // retire destroys, rebuild order_ if dirty, propagate

    i32   snapshot();                         // PIE: a memcpy per pool. Returns a handle, 0 on failure
    bool  restore(i32 snapshot);
    bool  releaseSnapshot(i32 snapshot);

private:
    std::vector<u8>   generation_;            // per index; 0 == never lived
    std::vector<u32>  freeIndices_;           // FIFO
    std::vector<u32>  retired_;               // indices whose generation would have wrapped
    std::vector<std::unique_ptr<ComponentPool>> pools_;
    std::vector<Entity> order_;
    bool  topoDirty_ = true;
};
```

### 3.7 Built-in components

Registered by Scene's own init before anything else runs, at fixed dense ids **1..8**: `CLocal`,
`CWorld`, `CHierarchy`, `CName`, `CTags`, `CMeshRenderer`, `CLight`, `CCamera`. Every one is
registered through the same public API a script-declared component uses, so **nothing about the
built-ins is privileged**.

```cpp
// `mesh` is an opaque ObjectId and `material` an opaque i32 this module never dereferences.
// Naming pbr::MaterialHandle here would need aver/pbr/Material.hpp behind a Core+Assets link line —
// an undeclared edge — and the ABI already crosses the same value as int32_t.
struct CMeshRenderer { u64 mesh = 0; i32 material = 0; u32 flags = 0; f32 aabbMin[3]; f32 aabbMax[3]; u32 dirty = 1; };
struct CLight  { i32 kind = 0; f32 colour[3]; f32 intensityLux = 100000.0f; f32 rangeCm = 0.0f; f32 innerCos = 1.0f, outerCos = 0.7f; };
struct CCamera { f32 fovYRad = 1.0472f; f32 nearCm = 5.0f; f32 farCm = 500000.0f; i32 priority = 0; };
```

### 3.8 What Scene does not have

This is the entire modularity claim, so it is stated as a list rather than as a principle: **no class,
no actor, no spawn recipe, no possession, no play state, no BeginPlay, no script type name, no CLR
hook, not even a plain function pointer.** `scene_abi.h` contains no symbol with `actor`, `class`,
`spawn`, `possess` or `play` in its name.

Two mechanisms hold the line, and only one of them is documentation:

1. **The link line** — Core + Assets. The moment `aver/framework/*` appears behind it, the design has
   been broken and the build says so.
2. **The framework needs no callback downward.** It sweeps its own instance lists with `valid()` once
   a frame — a `u32` compare per actor — which costs less than the inverted edge would. See open
   question 5 for what that costs in latency.

---

## 4. The gameplay framework

Everything Unreal-shaped lives in `Aver.Framework` and **nothing in it is a C++ base class**. There is
no `AGameActor`, no `APawn`, no virtual inheritance anywhere.

### 4.1 The class registry — `include/aver/framework/ClassRegistry.hpp`

```cpp
#pragma once
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"
#include "aver/framework/framework_hooks.h"
#include <string>
#include <vector>

namespace aver::fw {

using Class = u32;                                  // dense registry index; 0 invalid
inline constexpr Class kInvalidClass = 0u;

// A resolved spawn archetype: the ordered component list of the whole parent chain, plus ONE
// contiguous blob of default values with a per-component offset. Spawning is then a loop of memcpy
// over that blob — no constructor chain, no virtual dispatch, no reflection, no allocation beyond
// growing the pools.
struct SealedArchetype {
    std::vector<u32>   components;   // scene component type ids, parent-first
    std::vector<u32>   blobOffset;   // parallel to components
    std::vector<u8>    blob;
};

struct ClassRecord {
    std::string  name;
    u64          nameHash   = 0;     // fnv1a64(name); the identity a file and a reload carry
    Class        parent     = kInvalidClass;
    i32          flags      = 0;
    i32          tickGroup  = kTickPrePhysics;
    i32          tickOrder  = 0;
    bool         managed    = false; // ticked by the bridge in one batch, not by the loop below
    bool         orphaned   = false; // its type vanished from a rebuilt assembly; entities kept
    bool         sealed     = false;
    Class        defaultPawn       = kInvalidClass;   // GAMEMODE classes only
    Class        playerController  = kInvalidClass;
    std::string  defaultPawnName;                     // resolved at seal, so declaration order is free
    std::string  playerControllerName;
    SealedArchetype     archetype;
    AvActorVTable       vt{};        // BY VALUE, so the registrant's lifetime stops mattering
    std::vector<scene::Entity> instances;             // dense; the tick's inner loop IS this vector
};

class ClassRegistry {
public:
    // IDEMPOTENT BY NAME, and this is the whole of hot-reload identity: a rebuilt assembly
    // redeclares its class, gets back the handle its live entities already store, and only the
    // descriptor behind that handle is rewritten. A hash collision against a DIFFERENT name is
    // refused at declare time, which turns a silent alias into a load-time error.
    Class declare(std::string_view name, std::string_view parentName);
    Class find(std::string_view name) const;
    Class find(u64 nameHash) const;

    bool  reset(Class c);                       // drop components + defaults, keep the handle
    bool  addComponent(Class c, u32 type);
    bool  setFlags(Class c, i32 flags);
    bool  setVTable(Class c, const AvActorVTable& vt);   // copied, never referenced
    bool  setDefaultF32(Class c, u32 type, i32 field, f32 v);
    bool  setDefaultI32(Class c, u32 type, i32 field, i32 v);
    bool  setDefaultI64(Class c, u32 type, i32 field, i64 v);
    bool  setDefaultVec(Class c, u32 type, i32 field, const f32* v);
    bool  setDefaultStr(Class c, u32 type, i32 field, std::string_view v);
    bool  setDefaultPawnByName(Class gameMode, std::string_view pawnClass);
    bool  setPlayerControllerByName(Class gameMode, std::string_view controllerClass);

    // Flatten the parent chain. 0 on a cycle, a missing parent, or a field-kind clash. A name that
    // resolves to nothing produces ONE warning at load naming both sides — not a null at the moment
    // someone presses Play. NULL vtable slots are filled from the parent's record here, which is
    // how "override only OnTick" works without a byte of storage inheritance.
    bool  seal(Class c);

    ClassRecord*       at(Class c);
    const ClassRecord* at(Class c) const;
    u32                count() const;
private:
    std::vector<ClassRecord> classes_;   // index 0 is a permanent dead sentinel
};

} // namespace aver::fw
```

### 4.2 The five framework objects — all the same mechanism, differentiated by a flag

**GameInstance.** Process lifetime, and the only thing that outlives a world. `class Framework` owns
the registry and the world list; the GameInstance *entity* is spawned once from the single class
carrying `AVER_FW_CLASS_GAME_INSTANCE` and is refused destruction by Stop and by level travel. The
registry lives here rather than on a world for a concrete reason: **classes are declared by scripts at
assembly load, before any world exists**, and must still be there after Stop destroys the play world.
A second GAME_INSTANCE class is refused with a log line naming both, never silently overridden.

**GameMode.** A class carrying `AVER_FW_CLASS_GAME_MODE` plus two descriptor fields,
`defaultPawnClass` and `playerControllerClass`, named **by string** in the attribute and resolved to
handles at seal time — so two game classes never acquire compile-time references to each other. One
game-mode class per world; on Play the world spawns it as a normal entity (so it has a script instance
and ticks like anything else) with `tickOrder -1000` in the pre-physics group, so it runs before
everything it governs.

**GameActor.** Not a type. `aver_fw_class_of(e) != 0` is the complete definition of "this entity runs
code", and an entity without a class is a plain piece of scene data with a transform. `Actor`,
`StaticMeshActor`, `PointLight`, `DirectionalLight` and `CameraActor` are four lines of registration
each in `DefaultClasses.cpp`, registered as **native** classes, so a project with no C# and no CLR at
all still has a working Play.

**Pawn.** A class carrying `AVER_FW_CLASS_PAWN`, meaning *possessable*. The flag is not decoration:
`possess()` returns 0 for a pawn whose class lacks it, or a controller whose class lacks
`AVER_FW_CLASS_CONTROLLER`. That is the whole of type safety here, and it is checked at the seam
because **C has no other way to say "this is a pawn"**.

**PlayerController.** A class carrying `AVER_FW_CLASS_CONTROLLER`, holding the possessed pawn and the
view target. It is a normal entity with a transform (it is its own view target when unpossessed) and
it ticks like everything else. Possession writes both halves in one call, so `controlledPawn()` and
`controllerOf()` cannot disagree.

### 4.3 The framework — `include/aver/framework/Framework.hpp`

```cpp
class Framework {
public:
    bool init(scene::World* world);
    void shutdown();

    ClassRegistry&       classes()       { return classes_; }

    scene::Entity spawn(Class c, const Transform& where, std::string_view name = {});
    Class         attachClass(scene::Entity e, Class mixin);  // reclass onto a derived class
    bool          detachClass(scene::Entity e);
    Class         classOf(scene::Entity e) const;
    bool          destroy(scene::Entity e);        // queued; drained at the end of the tick

    void tick(f32 dt);
    bool setPlayState(i32 state);
    i32  playState() const { return play_; }

    bool          possess(scene::Entity controller, scene::Entity pawn);
    bool          unpossess(scene::Entity controller);
    scene::Entity gameInstance() const;
    scene::Entity gameMode() const;
    scene::Entity playerController(i32 index) const;

    bool installManagedDispatch(const AvManagedDispatch* d);
    bool clearManagedDispatch();
    bool reloadBegin();
    bool reloadEnd();

private:
    scene::World*     world_ = nullptr;
    ClassRegistry     classes_;
    std::vector<u32>  tickList_;      // class slots sorted by (tickGroup, tickOrder, nameHash)
    std::vector<u32>  actorSlot_;     // sparse by entity index -> classes_ index. 4 bytes per actor
    AvManagedDispatch managed_{};     // zeroed whenever no CLR is live
    i32               play_ = kPlayEditor;
    std::vector<scene::Entity> spawnQueue_[2], destroyQueue_;
    std::vector<ParkedAttachment> parked_;
};
```

### 4.4 The tick, which is the performance argument written out

```cpp
void Framework::tick(f32 dt) {
    if (play_ != kPlayPlaying) return;
    dt = dt < kMaxFrameDt ? dt : kMaxFrameDt;   // Engine.cpp:101 returns before :112, so the first
                                                // frame after a modal resize carries the whole drag.
                                                // Only the framework knows what a gameplay frame is.
    drainSpawnQueue();      // double-buffered: a spawn during a tick lands at the START of the next
    sweepDestroyed();       // the editor deleted an entity out from under an actor

    for (i32 group = 0; group < kTickGroupCount; ++group) {
        for (u32 slot : tickList_) {
            ClassRecord& c = classes_.at(slot) ? *classes_.at(slot) : kDead;
            if (c.tickGroup != group || c.managed || c.orphaned) continue;
            if (!c.vt.tick || (c.flags & kClassTicks) == 0) continue;
            // Hoisted: ONE indirect call per class per group, then a straight walk of a packed
            // vector whose call target is identical for every element. A per-object vtable would
            // put that call and a cache miss inside this loop instead.
            auto* fn = c.vt.tick; void* user = c.vt.user;
            for (scene::Entity e : c.instances) fn(user, (i32)e, dt);
        }
        // Managed actors are NOT ticked one at a time. One transition per group per frame; the
        // bridge walks its own dense (entity, instance) list in spawn order, each call in its own
        // try, exactly as HostBridge.Update already does.
        if (managed_.tick_all) managed_.tick_all(group, dt);
    }
    drainDestroyQueue();    // endPlay(DESTROY), unbind, world_->destroy
}
```

This is the honest answer to *"why any dispatch at all in a data-oriented engine"*: **20 indirect
calls per group for 10,000 actors across 20 classes**, with 20 stable branch targets instead of a
scattered indirect per object. It costs 4 bytes of class slot per actor (`actorSlot_`) and puts **no
per-object vtable pointer in hot data**.

### 4.5 Tag-bit ownership table

Committed in `Framework.hpp` before the second consumer exists, because an untyped bitfield with no
allocator becomes silently-wrong queries the moment two subsystems want bit 12.

| Bits | Owner | Meaning |
|---|---|---|
| 0..7 | the editor | `STATIC`, `EDITOR_ONLY`, `HIDDEN`, `SELECTED`, `LIGHT`, `CAMERA`, and two reserved |
| 8..15 | the framework | `ACTOR`, `PAWN`, `CONTROLLER`, `GAMEMODE`, `PLAYERSTART`, and three reserved |
| 16..31 | the project | allocated in that project's own header |

`Aver.Scene` states in `scene_abi.h` that it never interprets a tag bit.

---

## 5. The C ABI

Three headers. **Two of them are pure P/Invoke surfaces and one of them deliberately is not.** This is
the contract; it is reproduced here in full because an implementer will follow it literally.

### 5.1 `modules/scene/include/aver/scene/scene_abi.h`

```c
/* ============================================================================================
 * modules/scene/include/aver/scene/scene_abi.h
 * ============================================================================================ */
#ifndef AVER_SCENE_ABI_H
#define AVER_SCENE_ABI_H

/* Scene C ABI — entities, components, transforms, hierarchy. The stable surface C# binds to via
 * P/Invoke, in the exact idiom of pbr_abi.h.
 *
 * NO GAMEPLAY VOCABULARY APPEARS IN THIS FILE, and that is the design rather than a habit. There is
 * no actor, class, spawn, possess or play symbol here, and no script hook of any kind — not even a
 * plain function pointer. Gameplay lives one module up, behind framework_abi.h, and the arrow is
 * held by the link line, which this tree has already established as the only place an architectural
 * rule can actually be enforced (modules/render.pbr/CMakeLists.txt:26-29).
 *
 * Only int32_t / int64_t / float / const char* cross this boundary, plus pointers to those as
 * out-params. No struct, no void*, no function pointer, no enum: everything is a #define, so the
 * header works unchanged for C, C++, C# DllImport and any other FFI.
 *
 * There is no world handle. There is one world per process, for the same reason scripting_abi.h has
 * no module name to P/Invoke into: inventing a handle for a thing that can only have one instance
 * costs every call site a parameter and buys nothing until multi-world exists, at which point it is
 * an additive v2 entry point rather than a break.
 *
 * An entity is int32_t and always POSITIVE when live: 24 bits of index (index 0 never handed out),
 * 7 bits of generation starting at 1, bit 31 clear. So 0 == invalid holds with no signedness
 * argument on the boundary.
 *
 * Components are addressed by NAME, resolved once to a dense id. Fields are addressed by qualified
 * name ("CMovement.maxSpeed"), resolved once to a dense id that internally carries the component,
 * the byte offset and the kind. That is why this header has ~60 functions instead of ~10 per
 * component: adding CVehicle adds no ABI, no P/Invoke line and no bridge change — only a
 * registration call.
 *
 * Strings are UTF-8 IN BOTH DIRECTIONS. Stated here because voxi_abi.h and pbr_abi.h did not, and
 * their bindings consequently read them back with PtrToStringAnsi.
 *
 * Setters return 1 on success and 0 on rejection (stale handle, wrong kind, out of range). Getters
 * return a documented neutral value for a stale handle. Vectors go through a float* OUT-PARAM,
 * never a small struct by value: that return convention differs between compilers and marshallers,
 * and getting it wrong corrupts a register rather than failing to link.
 *
 * Not thread-safe by design. Every entry point is called on the frame thread between flush points;
 * a lock here would be taken millions of times a second to protect against a caller that does not
 * exist.
 */

#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_SCENE_BUILD)
#    define AVER_SCENE_ABI __declspec(dllexport)
#  else
#    define AVER_SCENE_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_SCENE_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_SCENE_ABI_VERSION 1

typedef int32_t aver_entity;    /* 0 invalid; a live entity is always positive */
typedef int32_t aver_component; /* 0 invalid */
typedef int32_t aver_field;     /* 0 invalid */
typedef int32_t aver_query;     /* 0 invalid */
typedef int32_t aver_snapshot;  /* 0 invalid */

/* Field kinds — must match aver::scene::FieldKind */
#define AVER_SCENE_KIND_F32    0
#define AVER_SCENE_KIND_VEC3   1
#define AVER_SCENE_KIND_QUAT   2
#define AVER_SCENE_KIND_I32    3
#define AVER_SCENE_KIND_BOOL   4
#define AVER_SCENE_KIND_I64    5
#define AVER_SCENE_KIND_ENTITY 6
#define AVER_SCENE_KIND_STRING 7
#define AVER_SCENE_KIND_MAT4   8

/* Built-in component type ids, fixed and registered before anything else runs. Script-declared
   types are allocated above these. Consumers should still resolve by name; these exist so the
   generated C# constant file has something stable to assert against. */
#define AVER_SCENE_COMP_LOCAL         1
#define AVER_SCENE_COMP_WORLD         2
#define AVER_SCENE_COMP_HIERARCHY     3
#define AVER_SCENE_COMP_NAME          4
#define AVER_SCENE_COMP_TAGS          5
#define AVER_SCENE_COMP_MESHRENDERER  6
#define AVER_SCENE_COMP_LIGHT         7
#define AVER_SCENE_COMP_CAMERA        8
#define AVER_SCENE_COMP_BUILTIN_MAX   8

AVER_SCENE_ABI int32_t aver_scene_abi_version(void);

/* ---- entities ---- */
AVER_SCENE_ABI aver_entity aver_scene_create(const char* name);
/* Deferred to the next flush and takes the whole subtree with it: an entity destroyed inside a tick
   must not invalidate the array that tick is walking. */
AVER_SCENE_ABI int32_t     aver_scene_destroy(aver_entity e);
AVER_SCENE_ABI int32_t     aver_scene_valid(aver_entity e);
AVER_SCENE_ABI int32_t     aver_scene_count(void);
/* Dense over LIVE entities. Indices SHIFT on destroy — a loop that destroys while iterating skips
   entities. The same hazard aver_pbr_at documents, and nothing enforces it here either. */
AVER_SCENE_ABI aver_entity aver_scene_at(int32_t index);

/* ---- identity ---- */
AVER_SCENE_ABI const char* aver_scene_get_name(aver_entity e);        /* "" for a stale handle */
AVER_SCENE_ABI int32_t     aver_scene_set_name(aver_entity e, const char* name);
AVER_SCENE_ABI int64_t     aver_scene_get_object_id(aver_entity e);   /* 0 if unsaved */
AVER_SCENE_ABI int32_t     aver_scene_set_object_id(aver_entity e, int64_t id);
AVER_SCENE_ABI aver_entity aver_scene_find(const char* name);         /* 0 when no entity carries it */

/* ---- hierarchy ---- */
/* keepWorld == 1 recomputes the child's local transform so it does not visibly move. Returns 0 when
   parent is a descendant of child: a cycle would hang the topological sort, so it is refused here
   rather than detected later. */
AVER_SCENE_ABI int32_t     aver_scene_set_parent(aver_entity e, aver_entity parent, int32_t keepWorld);
AVER_SCENE_ABI aver_entity aver_scene_get_parent(aver_entity e);
AVER_SCENE_ABI aver_entity aver_scene_first_child(aver_entity e);
AVER_SCENE_ABI aver_entity aver_scene_next_sibling(aver_entity e);
AVER_SCENE_ABI int32_t     aver_scene_child_count(aver_entity e);

/* ---- transform ----
   Centimetres, +Z up, +X forward, +Y right, LEFT-handed. A world matrix is 16 floats ROW-MAJOR in
   the row-vector convention (v * M), the same bytes as aver::Mat4::m, translation in the LAST ROW.
   A binding that transposes them is wrong. */
AVER_SCENE_ABI int32_t aver_scene_get_position(aver_entity e, float* out3);
AVER_SCENE_ABI int32_t aver_scene_set_position(aver_entity e, float x, float y, float z);
AVER_SCENE_ABI int32_t aver_scene_get_rotation(aver_entity e, float* out4);   /* quat x,y,z,w */
AVER_SCENE_ABI int32_t aver_scene_set_rotation(aver_entity e, float x, float y, float z, float w);
AVER_SCENE_ABI int32_t aver_scene_get_scale(aver_entity e, float* out3);
AVER_SCENE_ABI int32_t aver_scene_set_scale(aver_entity e, float x, float y, float z);
/* Composes on demand if this entity is stale, so a script never reads a matrix that lags the write
   it just made. Bounded by hierarchy depth. */
AVER_SCENE_ABI int32_t aver_scene_get_world_matrix(aver_entity e, float* out16);
/* Rebased through the parent's inverse, so a script can place an object in world space without
   reimplementing hierarchy maths on the managed side and getting the handedness wrong. */
AVER_SCENE_ABI int32_t aver_scene_set_world_position(aver_entity e, float x, float y, float z);

/* Batched forms, present from day one rather than retrofitted. A local is 10 floats:
   position xyz, rotation xyzw, scale xyz. The framework's own per-frame sweep uses these, so the
   per-actor transition cost is amortised before anyone writes ten thousand lines against the
   per-entity accessors above. Return the number of entities processed. */
AVER_SCENE_ABI int32_t aver_scene_get_locals(const aver_entity* entities, int32_t count, float* out10n);
AVER_SCENE_ABI int32_t aver_scene_set_locals(const aver_entity* entities, int32_t count, const float* in10n);
AVER_SCENE_ABI int32_t aver_scene_get_world_matrices(const aver_entity* entities, int32_t count, float* out16n);

/* ---- components ---- */
AVER_SCENE_ABI aver_component aver_scene_component(const char* name);   /* 0 if unregistered */
AVER_SCENE_ABI const char*    aver_scene_component_name(aver_component c);
AVER_SCENE_ABI int32_t        aver_scene_component_count(void);
AVER_SCENE_ABI aver_component aver_scene_component_at(int32_t index);
AVER_SCENE_ABI int32_t        aver_scene_component_size(aver_component c);  /* bytes; C# asserts this */
AVER_SCENE_ABI int32_t        aver_scene_has(aver_entity e, aver_component c);
AVER_SCENE_ABI int32_t        aver_scene_add(aver_entity e, aver_component c);   /* zero-filled */
AVER_SCENE_ABI int32_t        aver_scene_remove(aver_entity e, aver_component c);

/* ---- fields: one generic accessor set for every component, present and future ---- */
AVER_SCENE_ABI aver_field     aver_scene_field(const char* qualifiedName);  /* "CMovement.maxSpeed" */
AVER_SCENE_ABI const char*    aver_scene_field_name(aver_field f);
AVER_SCENE_ABI aver_component aver_scene_field_component(aver_field f);
AVER_SCENE_ABI int32_t        aver_scene_field_kind(aver_field f);
AVER_SCENE_ABI int32_t        aver_scene_field_arity(aver_field f);    /* floats per value: 1,3,4,16 */
AVER_SCENE_ABI int32_t        aver_scene_field_count(aver_component c);
AVER_SCENE_ABI aver_field     aver_scene_field_at(aver_component c, int32_t index);

AVER_SCENE_ABI float       aver_scene_get_f32(aver_entity e, aver_field f);    /* 0.0f if absent */
AVER_SCENE_ABI int32_t     aver_scene_set_f32(aver_entity e, aver_field f, float v);
AVER_SCENE_ABI int32_t     aver_scene_get_vec(aver_entity e, aver_field f, float* out); /* arity floats */
AVER_SCENE_ABI int32_t     aver_scene_set_vec(aver_entity e, aver_field f, const float* v);
AVER_SCENE_ABI int32_t     aver_scene_get_i32(aver_entity e, aver_field f);    /* also BOOL */
AVER_SCENE_ABI int32_t     aver_scene_set_i32(aver_entity e, aver_field f, int32_t v);
AVER_SCENE_ABI int64_t     aver_scene_get_i64(aver_entity e, aver_field f);
AVER_SCENE_ABI int32_t     aver_scene_set_i64(aver_entity e, aver_field f, int64_t v);
AVER_SCENE_ABI aver_entity aver_scene_get_ref(aver_entity e, aver_field f);
AVER_SCENE_ABI int32_t     aver_scene_set_ref(aver_entity e, aver_field f, aver_entity v);
AVER_SCENE_ABI const char* aver_scene_get_str(aver_entity e, aver_field f);    /* "" if absent */
AVER_SCENE_ABI int32_t     aver_scene_set_str(aver_entity e, aver_field f, const char* v);

/* ---- tags ----
   Uninterpreted bits. This module never reads a meaning into one; the ownership table lives in
   aver/framework/Framework.hpp and must be extended there before a second consumer claims a bit. */
AVER_SCENE_ABI int32_t aver_scene_get_tags(aver_entity e);
AVER_SCENE_ABI int32_t aver_scene_set_tags(aver_entity e, int32_t bits);
/* Fills out with up to max entities satisfying (tags & all) == all && (any == 0 || (tags & any)).
   Returns the number that MATCHED, which may exceed max — the caller decides whether to grow. This
   is how a native subsystem selects a set it is not allowed to understand. */
AVER_SCENE_ABI int32_t aver_scene_query_tags(int32_t all, int32_t any, aver_entity* out, int32_t max);

/* ---- component queries ----
   Takes a typed array, not a comma-separated string: a text grammar inside a C header is a thing
   that must never change meaning, and there is no reason to have one here. */
AVER_SCENE_ABI aver_query  aver_scene_query_create(const aver_component* components, int32_t count);
AVER_SCENE_ABI int32_t     aver_scene_query_destroy(aver_query q);
AVER_SCENE_ABI int32_t     aver_scene_query_count(aver_query q);      /* re-evaluated on call */
AVER_SCENE_ABI aver_entity aver_scene_query_at(aver_query q, int32_t index);

/* ---- render-facing state ---- */
AVER_SCENE_ABI int64_t aver_scene_get_mesh_id(aver_entity e);         /* an ObjectId; 0 unset */
AVER_SCENE_ABI int32_t aver_scene_set_mesh_id(aver_entity e, int64_t id);
/* An aver_pbr_material handle. This module never dereferences it and never links the type: naming
   pbr::MaterialHandle in a Core+Assets module would be an undeclared edge. */
AVER_SCENE_ABI int32_t aver_scene_get_material(aver_entity e);
AVER_SCENE_ABI int32_t aver_scene_set_material(aver_entity e, int32_t material);
AVER_SCENE_ABI int32_t aver_scene_get_visible(aver_entity e);
AVER_SCENE_ABI int32_t aver_scene_set_visible(aver_entity e, int32_t on);
/* 1 when this entity's render-facing state changed since the renderer last looked.
   READING IT CLEARS IT, so exactly one consumer acts on each change — the aver_pbr contract. */
AVER_SCENE_ABI int32_t aver_scene_consume_dirty(aver_entity e);

/* ---- frame ---- */
/* Retires deferred destroys, rebuilds the topological order if a parent link moved, then propagates
   world matrices in one linear pass. Returns the number of entities recomposed. */
AVER_SCENE_ABI int32_t aver_scene_flush(void);
/* Play-in-editor. Every pool is a flat vector, so this is a memcpy per pool in both directions:
   no snapshot format, no transient-field annotation, no pointer fixup, no reference fixing pass. */
AVER_SCENE_ABI aver_snapshot aver_scene_snapshot(void);
AVER_SCENE_ABI int32_t       aver_scene_restore(aver_snapshot s);
AVER_SCENE_ABI int32_t       aver_scene_release_snapshot(aver_snapshot s);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* AVER_SCENE_ABI_H */
```

### 5.2 `modules/framework/include/aver/framework/framework_abi.h`

```c
/* ============================================================================================
 * modules/framework/include/aver/framework/framework_abi.h
 * ============================================================================================ */
#ifndef AVER_FRAMEWORK_ABI_H
#define AVER_FRAMEWORK_ABI_H

/* Gameplay framework C ABI — spawn classes, actors, pawns, controllers, game modes, play state.
 *
 * A CLASS IS DATA. There is no C++ base type behind aver_class and no dispatch behind a spawn: a
 * class is a row in a registry holding a flattened component list and one contiguous blob of
 * defaults, and spawning is a loop of memcpy over that blob. That is why this header can describe
 * Actor, Pawn, PlayerController and GameMode without naming a single type.
 *
 * AN ACTOR IS AN ENTITY. There is no second id space, because two ids for one thing is two things
 * that can disagree, and every entry point below takes an aver_entity where Unreal would take an
 * AActor*. That is also why this header includes scene_abi.h rather than redeclaring aver_entity:
 * a duplicated typedef is a second definition of the same fact.
 *
 * Same idiom as scene_abi.h: only int32_t / int64_t / float / const char* and pointers to those
 * cross. There are no function pointers in this file — the host->module dispatch tables live in
 * framework_hooks.h, which no binding ever marshals, so this stays a pure P/Invoke surface.
 *
 * A class has TWO identities on purpose. int64_t nameHash (fnv1a64 of the class name) is what a
 * scene file and a hot reload carry, because it survives both. aver_class is a dense runtime index,
 * because that is what the tick loop wants and a hash is not.
 */

#include <stdint.h>
#include "aver/scene/scene_abi.h"

#if defined(_WIN32)
#  if defined(AVER_FW_BUILD)
#    define AVER_FW_ABI __declspec(dllexport)
#  else
#    define AVER_FW_ABI __declspec(dllimport)
#  endif
#else
#  define AVER_FW_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_ABI_VERSION 1

typedef int32_t aver_class;   /* 0 invalid; a live class is always positive */

/* Class flags. Checked at the seam because C has no other way to say "this is a pawn". */
#define AVER_FW_CLASS_TICKS             0x0001
#define AVER_FW_CLASS_PAWN              0x0002
#define AVER_FW_CLASS_CONTROLLER        0x0004
#define AVER_FW_CLASS_GAME_MODE         0x0008
#define AVER_FW_CLASS_GAME_INSTANCE     0x0010
#define AVER_FW_CLASS_MANAGED           0x0020   /* ticked in one batch by the bridge, not per class */
#define AVER_FW_CLASS_ABSTRACT          0x0040   /* declarable, never spawnable, hidden from Add */
#define AVER_FW_CLASS_TICK_IN_EDITOR    0x0080   /* rare and opt-in: editor mode does not tick */

/* Tick groups. Physics does not exist yet; the slot does, so adding it later is not a reorder. */
#define AVER_FW_TICK_PRE_PHYSICS   0
#define AVER_FW_TICK_PHYSICS       1
#define AVER_FW_TICK_POST_PHYSICS  2
#define AVER_FW_TICK_GROUP_COUNT   3

/* Play state */
#define AVER_FW_PLAY_EDITOR  0
#define AVER_FW_PLAY_PLAYING 1
#define AVER_FW_PLAY_PAUSED  2

/* Why a lifecycle hook fired. Passed rather than inferred: a script that must distinguish a fresh
   spawn from a hot reload can only do so if it is told, and every engine that omitted this grew a
   bool later — and a bool is an ABI change. */
#define AVER_FW_BEGIN_SPAWN   0
#define AVER_FW_BEGIN_PLAY    1
#define AVER_FW_BEGIN_RELOAD  2
#define AVER_FW_END_DESTROY   0
#define AVER_FW_END_STOP      1
#define AVER_FW_END_RELOAD    2
#define AVER_FW_END_TRAVEL    3

/* Per-entity binding state, for the editor's benefit. A binding that cannot resolve is never
   silently dropped: the entity keeps its transform and its overrides, the state goes FAILED with a
   reason, and the outliner renders that row red with the reason as a tooltip. A dropped attachment
   is data loss at the next save. */
#define AVER_FW_BIND_NONE    0
#define AVER_FW_BIND_PENDING 1
#define AVER_FW_BIND_LIVE    2
#define AVER_FW_BIND_FAILED  3
#define AVER_FW_BIND_PARKED  4   /* class vanished from a rebuilt assembly; hash + overrides kept */

AVER_FW_ABI int32_t aver_fw_abi_version(void);

/* ---- component declaration (script-declared gameplay components) ----
   These forward to the scene's registry; they live here because "a gameplay component" is a
   framework idea, and because the field kinds a Details panel must draw are the framework's
   business. Adding CVehicle is two calls, not an ABI event. */
AVER_FW_ABI aver_component aver_fw_component_declare(const char* name, int32_t sizeBytes, int32_t alignBytes);
AVER_FW_ABI int32_t        aver_fw_component_add_field(aver_component c, const char* name,
                                                       int32_t kind, int32_t byteOffset, int32_t arity);
AVER_FW_ABI int32_t        aver_fw_component_verify(aver_component c, int32_t structBytes);

/* ---- the class registry ----
   declare() is IDEMPOTENT BY NAME and returns the same handle for the life of the process. That is
   what makes hot reload survivable with no managed bookkeeping: a rebuilt script redeclares its
   class, gets back the handle its live entities already reference, and only the descriptor behind
   it is rewritten. A declare whose hash matches an existing entry with a DIFFERENT name is refused,
   which turns a silent alias into a load-time error. */
AVER_FW_ABI aver_class  aver_fw_class_declare(const char* name, const char* parentName);
AVER_FW_ABI aver_class  aver_fw_class_find(const char* name);
AVER_FW_ABI aver_class  aver_fw_class_find_by_hash(int64_t nameHash);
AVER_FW_ABI int64_t     aver_fw_class_name_hash(aver_class c);
AVER_FW_ABI const char* aver_fw_class_name(aver_class c);         /* "" for a stale handle */
AVER_FW_ABI aver_class  aver_fw_class_parent(aver_class c);
AVER_FW_ABI int32_t     aver_fw_class_reset(aver_class c);        /* drop components + defaults */
AVER_FW_ABI int32_t     aver_fw_class_add_component(aver_class c, aver_component type);
AVER_FW_ABI int32_t     aver_fw_class_set_flags(aver_class c, int32_t flags);
AVER_FW_ABI int32_t     aver_fw_class_get_flags(aver_class c);
AVER_FW_ABI int32_t     aver_fw_class_set_tick(aver_class c, int32_t tickGroup, int32_t tickOrder);
AVER_FW_ABI int32_t     aver_fw_class_is_orphaned(aver_class c);

/* Defaults, addressed as (component, field id) — the SAME dense field ids scene_abi.h resolves, so
   there are no hand-copied component/field integers on either side of the boundary. Rejected if the
   class does not carry that component or the kind does not match. */
AVER_FW_ABI int32_t aver_fw_class_set_default_f32(aver_class c, aver_field f, float v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i32(aver_class c, aver_field f, int32_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_i64(aver_class c, aver_field f, int64_t v);
AVER_FW_ABI int32_t aver_fw_class_set_default_vec(aver_class c, aver_field f, const float* v);
AVER_FW_ABI int32_t aver_fw_class_set_default_str(aver_class c, aver_field f, const char* v);

/* A game mode names its pawn and controller BY CLASS NAME, resolved when the class is sealed, so
   two game classes never acquire compile-time references to each other. A name that resolves to
   nothing is ONE warning at load naming both sides — not a null when someone presses Play. */
AVER_FW_ABI int32_t aver_fw_class_set_default_pawn(aver_class gameMode, const char* pawnClassName);
AVER_FW_ABI int32_t aver_fw_class_set_player_controller(aver_class gameMode, const char* controllerClassName);

/* Flatten the parent chain into the resolved archetype. 0 on a cycle, a missing parent or a field
   clash. Spawning auto-seals, so a caller that forgets is slow once, not wrong. */
AVER_FW_ABI int32_t aver_fw_class_seal(aver_class c);

/* Introspection — this generates the editor's Add menu and the Details panel, so a class authored
   purely in C# is placeable from the toolbar with no C++ edit. */
AVER_FW_ABI int32_t        aver_fw_class_count(void);
AVER_FW_ABI aver_class     aver_fw_class_at(int32_t index);
AVER_FW_ABI int32_t        aver_fw_class_component_count(aver_class c);   /* resolved, not local */
AVER_FW_ABI aver_component aver_fw_class_component_at(aver_class c, int32_t index);

/* ---- session ---- */
AVER_FW_ABI int32_t     aver_fw_set_play_state(int32_t state);
AVER_FW_ABI int32_t     aver_fw_play_state(void);
AVER_FW_ABI aver_entity aver_fw_game_instance(void);
AVER_FW_ABI aver_entity aver_fw_game_mode(void);
AVER_FW_ABI aver_entity aver_fw_player_controller(int32_t playerIndex);
AVER_FW_ABI int32_t     aver_fw_tick(float dt);   /* returns actors ticked; 0 outside PLAYING */

/* ---- actors ----
   Rotation is a quaternion in, matching every other rotation on this boundary; pos3/quat4/scale3
   may be null, meaning the class default. Returns 0 for an abstract class, an unsealable class or
   a stale world. */
AVER_FW_ABI aver_entity aver_fw_spawn(aver_class c, const char* name,
                                      const float* pos3, const float* quat4, const float* scale3);
AVER_FW_ABI int32_t     aver_fw_destroy(aver_entity e);      /* deferred to the end of the tick */
AVER_FW_ABI aver_class  aver_fw_class_of(aver_entity e);     /* != 0 IS the definition of "actor" */
/* Attach a class to an entity that already exists by RECLASSING it onto a derived class whose
   parent is its current class, creating that class the first time it is asked for. Idempotent.
   This is the editor's drag-a-script-onto-an-actor gesture; the derived name is canonical
   (parent + sorted mixin names) so two attach orders cannot mint two classes. */
AVER_FW_ABI aver_class  aver_fw_attach_class(aver_entity e, aver_class mixin);
AVER_FW_ABI int32_t     aver_fw_detach_class(aver_entity e);
AVER_FW_ABI int32_t     aver_fw_bind_state(aver_entity e);
AVER_FW_ABI const char* aver_fw_bind_error(aver_entity e);   /* "" unless FAILED */

/* ---- possession (rejected unless the classes carry PAWN / CONTROLLER) ---- */
AVER_FW_ABI int32_t     aver_fw_possess(aver_entity controller, aver_entity pawn);
AVER_FW_ABI int32_t     aver_fw_unpossess(aver_entity controller);
AVER_FW_ABI aver_entity aver_fw_controlled_pawn(aver_entity controller);
AVER_FW_ABI aver_entity aver_fw_controller_of(aver_entity pawn);

/* ---- reload choreography, driven by the editor ----
   begin() ends play with reason RELOAD and unbinds every managed instance WITHOUT destroying an
   entity or clearing a class. end() re-resolves every attachment by name hash and rebinds. An
   attachment whose class did not come back is PARKED, not dropped: its hash and its overrides stay
   on the entity, so fixing the typo restores the actor intact. */
AVER_FW_ABI int32_t     aver_fw_reload_begin(void);
AVER_FW_ABI int32_t     aver_fw_reload_end(void);
AVER_FW_ABI int32_t     aver_fw_parked_count(void);
AVER_FW_ABI aver_entity aver_fw_parked_entity(int32_t index);
AVER_FW_ABI const char* aver_fw_parked_class_name(int32_t index);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* AVER_FRAMEWORK_ABI_H */
```

### 5.3 `modules/framework/include/aver/framework/framework_hooks.h`

**Not a P/Invoke surface.** No C# code marshals anything in this file. It exists so that the other two
headers can stay pure.

```c
/* ============================================================================================
 * modules/framework/include/aver/framework/framework_hooks.h
 * Deliberately NOT part of the P/Invoke surface. No C# code marshals anything in this file; it
 * follows scripting_abi.h's precedent instead — the host hands this module a table of function
 * pointers, guarded by structBytes (the shape really is what we agreed) and contractVersion (we
 * agreed on a shape). Keeping it out of framework_abi.h is what lets that header stay pure.
 * ============================================================================================ */
#ifndef AVER_FRAMEWORK_HOOKS_H
#define AVER_FRAMEWORK_HOOKS_H

#include <stdint.h>
#include "aver/framework/framework_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_VTABLE_VERSION   1
#define AVER_FW_DISPATCH_VERSION 1

/* One per CLASS, never per object, and the framework stores it BY VALUE so the registrant's memory
   may go away without leaving a pointer behind to be called into. A NULL slot is inherited from the
   parent class at seal time, which is how "override only tick" works with no storage inheritance. */
typedef struct AvActorVTable {
    int32_t structBytes;
    int32_t contractVersion;
    void*   user;                                                  /* NULL for managed classes */
    void    (*beginPlay)(void* user, aver_entity e, int32_t reason);
    void    (*tick)     (void* user, aver_entity e, float dt);
    void    (*endPlay)  (void* user, aver_entity e, int32_t reason);
} AvActorVTable;

AVER_FW_ABI aver_class aver_fw_class_set_vtable(aver_class c, const AvActorVTable* vt);

/* The managed side never registers a pointer to user code. It installs ONE table whose entries live
   in Aver.Scripting.Bridge — an assembly hostfxr loads once for the life of the process and which
   is NOT collectible. Every managed class then gets a vtable of fixed thunks compiled into this
   module that route through this table. That is the whole reason unloading a collectible
   AssemblyLoadContext cannot leave a dangling function pointer in the registry, and why clearing
   the table is a NULL store the guards already handle.
   tick_all takes a GROUP, not an entity: a P/Invoke per actor per frame is a cost this design would
   never be able to argue away, and the bridge already keeps its own dense instance list. */
typedef struct AvManagedDispatch {
    int32_t structBytes;
    int32_t contractVersion;
    int32_t (*bind)     (int64_t classNameHash, aver_entity e);   /* 1 == an instance now exists */
    void    (*unbind)   (aver_entity e);
    void    (*beginPlay)(aver_entity e, int32_t reason);
    void    (*tick_all) (int32_t tickGroup, float dt);
    void    (*endPlay)  (aver_entity e, int32_t reason);
    void    (*rebound)  (aver_entity e);
} AvManagedDispatch;

/* Refuses a second non-null install and logs which module holds it: the executable is the only
   thing that may wire the ScriptHost's entry points into this module, and nothing in the build can
   enforce that. */
AVER_FW_ABI int32_t aver_fw_install_managed_dispatch(const AvManagedDispatch* d);
AVER_FW_ABI int32_t aver_fw_clear_managed_dispatch(void);   /* call BEFORE the ALC is unloaded */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* AVER_FRAMEWORK_HOOKS_H */
```

**Why the vtable is stored by value and why every managed class shares five fixed thunks.** A managed
class's vtable is always the *same* five native thunks compiled into `Aver.Framework`, routing through
one `AvManagedDispatch` table that lives in the non-collectible `Aver.Scripting.Bridge` assembly, and
the table is stored **by value** in the class record. Native state can then never hold a function
pointer owned by the collectible `AverScripts` context, so an ALC unload cannot dangle one. Clearing
the table is a NULL store the guards already handle. That makes hot reload safe **by construction**
rather than by ordering discipline, and it is what lets the idempotent-by-name class registry survive
an unload that happens halfway through.

---

## 6. The C# authoring surface

### 6.1 What a user actually writes

`<Project>\Content\Scripts\Spinner.cs`, created by **Content Browser ▸ New C# Actor Script**.
`Scripts.csproj` is `Microsoft.NET.Sdk` and globs `**/*.cs`, so no project file is ever edited and the
IDE and the editor cannot disagree about the file list.

```csharp
using Aver.Scene;
using Aver.Framework;
using Aver.Scripting;

// The attribute NAMES the class and its parent; it never names a hook. Hooks stay compiler-checked
// overrides for exactly the reason Behaviour.cs already argues: a misspelt OnUpate must not compile
// cleanly and then simply never run.
[AverClass("AN_Spinner", Parent = "StaticMeshActor")]
public sealed class Spinner : AverActor
{
    // Editable fields are class defaults with per-instance overrides, and the override is stored
    // NATIVELY. That is why the value a designer typed into Details survives a hot reload even
    // though this object does not — and why the rule is one sentence: state that must survive a
    // reload goes in engine-owned storage, and putting it there is the same amount of typing.
    [Editable] public float DegreesPerSecond = 90.0f;
    [Editable] public bool  Clockwise        = true;
    [Editable] public Vec3  Pivot            = new Vec3(0, 0, 0);
    [Editable] public Entity Target;                 // drag an outliner row onto this field

    private float _accum;   // not [Editable], so it restarts from its initialiser after a reload

    // Runs once per CLASS at load, never per instance. This is the archetype recipe.
    public static void Configure(ClassBuilder b)
    {
        b.Mesh("Content/Meshes/Cube.ocmesh", material: "M_Chrome");
        b.PointLight(intensityLux: 1200.0f, rangeCm: 400.0f);
        b.Ticks(TickGroup.PrePhysics);
    }

    public override void OnBeginPlay(BeginReason reason)
    {
        // A reload restored the native overrides but not _accum; re-latching state that a fresh
        // spawn would latch is exactly what the reason code exists to prevent.
        if (reason != BeginReason.Reload) _accum = 0.0f;
    }

    public override void OnTick(float dt)
    {
        _accum += (Clockwise ? 1.0f : -1.0f) * DegreesPerSecond * dt;
        Self.LocalRotation = Quat.FromAxisAngle(Vec3.Up, _accum * (MathF.PI / 180.0f));
    }

    // Fires instead of OnBeginPlay after a rebind, so cached state is re-derived without re-running
    // spawn logic. Overriding OnBeginPlay without overriding this produces one warning at load.
    public override void OnRebound() => _accum = Self.LocalRotation.YawDegrees();

    public override void OnEndPlay(EndReason reason)
    {
        if (reason == EndReason.Reload) return;   // the entity is staying; say nothing
        Log.Info($"{Self.Name} ended play: {reason}");
    }
}

[AverClass("AN_FlyPawn", Parent = "Pawn")]
public sealed class FlyPawn : AverPawn
{
    [Editable] public float SpeedCmPerSec = 600.0f;   // centimetres, per the coordinate contract

    public static void Configure(ClassBuilder b)
    {
        b.Camera(fovDegrees: 60.0f, nearCm: 5.0f, farCm: 500000.0f);
        b.Ticks(TickGroup.PrePhysics);
    }

    public override void OnPossessed(Entity controller) => Log.Info($"{controller.Name} took {Self.Name}");
    public override void OnTick(float dt) => Self.LocalPosition += Self.Forward * (SpeedCmPerSec * dt);
}

// Pawn and controller are named BY CLASS NAME and resolved at seal time, so these three files never
// reference each other's types and can be authored in any order.
[AverGameMode("GM_Sandbox", DefaultPawnClass = "AN_FlyPawn", PlayerControllerClass = "PlayerController")]
public sealed class SandboxMode : AverGameMode
{
    public override void OnBeginPlay(BeginReason reason)
    {
        Spawn(ActorClass.Find("AN_Spinner"), new Vec3(0, 0, 150));
        Spawn<Spinner>(new Vec3(200, 0, 150));   // sugar: the type carries its own class name
    }
    public override void OnPostLogin(Entity controller) => Log.Info($"player joined: {controller.Name}");
}
```

The attribute names the class and its parent and **never names a hook**. Hooks stay compiler-checked
overrides for exactly the reason `docs/SCRIPTING.md` §2 already argues about `AverBehaviour`: with
name-matched hooks a misspelt `OnUpate` compiles cleanly and then simply never runs, which is the
worst failure mode available to a layer aimed at people who are not engine developers.

`OnRebound` exists as a hook **distinct from** `OnBeginPlay` because a script must re-derive cached
state after a reload without re-running spawn logic, and forgetting to is the most likely source of
"the editor is lying to me" reports. Overriding `OnBeginPlay` without overriding `OnRebound` produces
one warning at load.

### 6.2 What the engine ships — `Aver.Scripting/Scene.cs`

In the exact shape of `Pbr.cs`. It goes in the **existing** `Aver.Scripting` assembly, not a new one:
`HostBridge.cs:243` skips exactly two simple names and `SampleBehaviour.csproj` sets `Private="false"`
so a second identity is never staged next to user code. `Aver.Scripting.csproj` goes to
`<Version>1.1.0</Version>` — additive, same major, so `CheckApiVersion`'s
`referenced.Major != loaded.Major || referenced > loaded` keeps accepting assemblies built against
1.0.0.

```csharp
namespace Aver.Scene;

using System;
using System.Runtime.InteropServices;

/// <summary>A component type id, resolved once from its name.</summary>
public readonly struct Component
{
    public int Id { get; }
    internal Component(int id) => Id = id;
    public static Component Get(string name) => new(Native.aver_scene_component(name));
    public bool IsValid => Id != 0;
}

/// <summary>A field id: component + offset + kind, resolved once from "CMovement.maxSpeed".</summary>
public readonly struct Field
{
    public int Id { get; }
    internal Field(int id) => Id = id;
    public static Field Get(string qualifiedName) => new(Native.aver_scene_field(qualifiedName));
    public bool IsValid => Id != 0;
}

/// <summary>A handle to an entity. 0 is invalid; a live entity is always positive.</summary>
public readonly struct Entity : IEquatable<Entity>
{
    public int Handle { get; }
    internal Entity(int handle) => Handle = handle;
    public static readonly Entity None = default;

    public bool   IsValid => Handle != 0 && Native.aver_scene_valid(Handle) != 0;
    public string Name    => Native.Str(Native.aver_scene_get_name(Handle));

    public Entity Parent
    {
        get => new(Native.aver_scene_get_parent(Handle));
        set => Native.aver_scene_set_parent(Handle, value.Handle, 1);
    }

    public Vec3 LocalPosition
    {
        get { var v = Scratch3; Native.aver_scene_get_position(Handle, v); return new Vec3(v[0], v[1], v[2]); }
        set => Native.aver_scene_set_position(Handle, value.X, value.Y, value.Z);
    }
    public Quat LocalRotation
    {
        get { var v = Scratch4; return Native.aver_scene_get_rotation(Handle, v) != 0
                                        ? new Quat(v[0], v[1], v[2], v[3]) : Quat.Identity; }
        set => Native.aver_scene_set_rotation(Handle, value.X, value.Y, value.Z, value.W);
    }
    public Vec3 Forward => LocalRotation.Rotate(Vec3.Forward);   // +X, per the coordinate contract

    public bool  Has(Component c)    => Native.aver_scene_has(Handle, c.Id) != 0;
    public bool  Add(Component c)    => Native.aver_scene_add(Handle, c.Id) != 0;
    public bool  Remove(Component c) => Native.aver_scene_remove(Handle, c.Id) != 0;

    public float  GetF32(Field f)           => Native.aver_scene_get_f32(Handle, f.Id);
    public bool   SetF32(Field f, float v)  => Native.aver_scene_set_f32(Handle, f.Id, v) != 0;
    public int    GetI32(Field f)           => Native.aver_scene_get_i32(Handle, f.Id);
    public bool   SetI32(Field f, int v)    => Native.aver_scene_set_i32(Handle, f.Id, v) != 0;
    public Entity GetRef(Field f)           => new(Native.aver_scene_get_ref(Handle, f.Id));
    public bool   SetRef(Field f, Entity v) => Native.aver_scene_set_ref(Handle, f.Id, v.Handle) != 0;
    public Vec3   GetVec3(Field f)          { var v = Scratch3; Native.aver_scene_get_vec(Handle, f.Id, v);
                                              return new Vec3(v[0], v[1], v[2]); }
    public bool   SetVec3(Field f, Vec3 v)  { var s = Scratch3; s[0]=v.X; s[1]=v.Y; s[2]=v.Z;
                                              return Native.aver_scene_set_vec(Handle, f.Id, s) != 0; }

    public bool Destroy() => Native.aver_scene_destroy(Handle) != 0;

    public bool Equals(Entity o) => Handle == o.Handle;
    public override bool Equals(object? o) => o is Entity e && Equals(e);
    public override int GetHashCode() => Handle;

    // Reused rather than allocated: a vector write per actor per fixed step would otherwise be GC
    // pressure inside the gameplay tick, and [ThreadStatic] keeps that safe without a lock.
    [ThreadStatic] private static float[]? t_v3;
    [ThreadStatic] private static float[]? t_v4;
    private static float[] Scratch3 => t_v3 ??= new float[3];
    private static float[] Scratch4 => t_v4 ??= new float[4];
}

internal static class Native
{
    // The same simple-name DllImport Pbr.cs and Voxi.cs use: in process this resolves to the DLL
    // the editor already loaded, so the bindings and the editor address the same entities.
    private const string Lib = "Aver.Scene";

    [DllImport(Lib)] internal static extern int    aver_scene_abi_version();
    [DllImport(Lib)] internal static extern int    aver_scene_valid(int e);
    [DllImport(Lib)] internal static extern int    aver_scene_destroy(int e);
    [DllImport(Lib)] internal static extern IntPtr aver_scene_get_name(int e);
    [DllImport(Lib)] internal static extern int    aver_scene_get_parent(int e);
    [DllImport(Lib)] internal static extern int    aver_scene_set_parent(int e, int parent, int keepWorld);
    [DllImport(Lib)] internal static extern int    aver_scene_get_position(int e, float[] out3);
    [DllImport(Lib)] internal static extern int    aver_scene_set_position(int e, float x, float y, float z);
    [DllImport(Lib)] internal static extern int    aver_scene_get_rotation(int e, float[] out4);
    [DllImport(Lib)] internal static extern int    aver_scene_set_rotation(int e, float x, float y, float z, float w);
    [DllImport(Lib)] internal static extern int    aver_scene_has(int e, int c);
    [DllImport(Lib)] internal static extern int    aver_scene_add(int e, int c);
    [DllImport(Lib)] internal static extern int    aver_scene_remove(int e, int c);
    [DllImport(Lib)] internal static extern float  aver_scene_get_f32(int e, int f);
    [DllImport(Lib)] internal static extern int    aver_scene_set_f32(int e, int f, float v);
    [DllImport(Lib)] internal static extern int    aver_scene_get_i32(int e, int f);
    [DllImport(Lib)] internal static extern int    aver_scene_set_i32(int e, int f, int v);
    [DllImport(Lib)] internal static extern int    aver_scene_get_ref(int e, int f);
    [DllImport(Lib)] internal static extern int    aver_scene_set_ref(int e, int f, int v);
    [DllImport(Lib)] internal static extern int    aver_scene_get_vec(int e, int f, float[] outN);
    [DllImport(Lib)] internal static extern int    aver_scene_set_vec(int e, int f, float[] v);

    // UTF-8 IN BOTH DIRECTIONS. Pbr.cs and Voxi.cs read with PtrToStringAnsi and write with LPStr;
    // scene_abi.h states the encoding precisely so this file cannot repeat that.
    [DllImport(Lib)] internal static extern int aver_scene_component([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern int aver_scene_field([MarshalAs(UnmanagedType.LPUTF8Str)] string qualifiedName);
    internal static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "";
}
```

### 6.3 `Aver.Scripting/Framework.cs` — the authoring base classes

Each holds **one** field, the entity handle, and adds no storage, so C#'s inheritance never becomes
the engine's object model: the native side has no idea this hierarchy exists and stores nothing on its
behalf.

```csharp
namespace Aver.Framework;

using System;
using System.Runtime.InteropServices;
using Aver.Scene;

public enum BeginReason { Spawn = 0, Play = 1, Reload = 2 }
public enum EndReason   { Destroy = 0, Stop = 1, Reload = 2, Travel = 3 }
public enum TickGroup   { PrePhysics = 0, Physics = 1, PostPhysics = 2 }

public readonly struct ActorClass : IEquatable<ActorClass>
{
    public int Handle { get; }
    internal ActorClass(int h) => Handle = h;
    public static ActorClass Find(string name) => new(Fw.aver_fw_class_find(name));
    public string Name  => Aver.Scene.Native.Str(Fw.aver_fw_class_name(Handle));
    public bool IsValid => Handle != 0;
    public bool Equals(ActorClass o) => Handle == o.Handle;
    public override bool Equals(object? o) => o is ActorClass c && Equals(c);
    public override int GetHashCode() => Handle;
}

public abstract class AverActor
{
    public Entity Self { get; internal set; }

    public virtual void OnBeginPlay(BeginReason reason) { }
    public virtual void OnTick(float dt) { }
    public virtual void OnEndPlay(EndReason reason) { }
    public virtual void OnRebound() { }

    protected static Entity Spawn(ActorClass c, Vec3 at)
        => new(Fw.aver_fw_spawn(c.Handle, null, new[] { at.X, at.Y, at.Z }, null, null));
    protected static Entity Spawn<T>(Vec3 at) where T : AverActor
        => Spawn(ActorClass.Find(ClassNames.Of(typeof(T))), at);
}

public abstract class AverPawn : AverActor
{
    public virtual void OnPossessed(Entity controller) { }
    public virtual void OnUnpossessed() { }
}

public class AverPlayerController : AverActor
{
    public Entity Possessed => new(Fw.aver_fw_controlled_pawn(Self.Handle));
    public bool Possess(Entity pawn) => Fw.aver_fw_possess(Self.Handle, pawn.Handle) != 0;
}

public abstract class AverGameMode : AverActor
{
    public virtual void OnPostLogin(Entity controller) { }
}

public abstract class AverGameInstance : AverActor { }

[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverClassAttribute : Attribute
{
    public AverClassAttribute(string name) => Name = name;
    public string Name { get; }
    public string Parent { get; init; } = "Actor";
}

[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AverGameModeAttribute : Attribute
{
    public AverGameModeAttribute(string name) => Name = name;
    public string Name { get; }
    public string DefaultPawnClass      { get; init; } = "Pawn";
    public string PlayerControllerClass { get; init; } = "PlayerController";
}

/// <summary>
/// Marks a field as class-default-with-per-instance-override. The value lives in native storage,
/// which is why it appears in Details, serialises into the world, survives Play/Stop and survives
/// a hot reload. Read it as "this belongs to the engine", not as "show this in the inspector".
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class EditableAttribute : Attribute
{
    public float Min { get; init; } = float.NegativeInfinity;
    public float Max { get; init; } = float.PositiveInfinity;
}

/// <summary>Writes class DEFAULTS. Every call is a registry write; nothing is instanced.</summary>
public sealed class ClassBuilder
{
    private readonly int _c;
    internal ClassBuilder(int c) => _c = c;

    // SceneIds is GENERATED from the live native registry by Tools ▸ Generate Script Ids, not
    // hand-maintained, so a component or field index can never drift from scene_abi.h.
    public void Mesh(string path, string material = "")
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CMeshRenderer);
        Fw.aver_fw_class_set_default_str(_c, SceneIds.CMeshRenderer_meshPath, path);
        Fw.aver_fw_class_set_default_str(_c, SceneIds.CMeshRenderer_material, material);
    }
    public void PointLight(float intensityLux, float rangeCm)
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CLight);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.CLight_intensityLux, intensityLux);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.CLight_rangeCm, rangeCm);   // centimetres
    }
    public void Camera(float fovDegrees, float nearCm, float farCm)
    {
        Fw.aver_fw_class_add_component(_c, SceneIds.CCamera);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.CCamera_fovYRad, fovDegrees * (MathF.PI / 180f));
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.CCamera_nearCm, nearCm);
        Fw.aver_fw_class_set_default_f32(_c, SceneIds.CCamera_farCm,  farCm);
    }
    public void Ticks(TickGroup group, int order = 0)
    {
        Fw.aver_fw_class_set_flags(_c, Fw.aver_fw_class_get_flags(_c) | 0x0001);
        Fw.aver_fw_class_set_tick(_c, (int)group, order);
    }
}

internal static class Fw
{
    private const string Lib = "Aver.Framework";
    [DllImport(Lib)] internal static extern int    aver_fw_abi_version();
    [DllImport(Lib)] internal static extern int    aver_fw_class_declare([MarshalAs(UnmanagedType.LPUTF8Str)] string name,
                                                                        [MarshalAs(UnmanagedType.LPUTF8Str)] string parent);
    [DllImport(Lib)] internal static extern int    aver_fw_class_find([MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern IntPtr aver_fw_class_name(int c);
    [DllImport(Lib)] internal static extern int    aver_fw_class_reset(int c);
    [DllImport(Lib)] internal static extern int    aver_fw_class_add_component(int c, int type);
    [DllImport(Lib)] internal static extern int    aver_fw_class_set_flags(int c, int flags);
    [DllImport(Lib)] internal static extern int    aver_fw_class_get_flags(int c);
    [DllImport(Lib)] internal static extern int    aver_fw_class_set_tick(int c, int group, int order);
    [DllImport(Lib)] internal static extern int    aver_fw_class_set_default_f32(int c, int field, float v);
    [DllImport(Lib)] internal static extern int    aver_fw_class_set_default_i32(int c, int field, int v);
    [DllImport(Lib)] internal static extern int    aver_fw_class_set_default_str(int c, int field,
                                                        [MarshalAs(UnmanagedType.LPUTF8Str)] string v);
    [DllImport(Lib)] internal static extern int    aver_fw_class_set_default_pawn(int gm,
                                                        [MarshalAs(UnmanagedType.LPUTF8Str)] string pawn);
    [DllImport(Lib)] internal static extern int    aver_fw_class_set_player_controller(int gm,
                                                        [MarshalAs(UnmanagedType.LPUTF8Str)] string pc);
    [DllImport(Lib)] internal static extern int    aver_fw_class_seal(int c);
    [DllImport(Lib)] internal static extern int    aver_fw_spawn(int c, string? name, float[]? p, float[]? q, float[]? s);
    [DllImport(Lib)] internal static extern int    aver_fw_possess(int controller, int pawn);
    [DllImport(Lib)] internal static extern int    aver_fw_controlled_pawn(int controller);
}
```

### 6.4 Bridge side — `Aver.Scripting.Bridge/HostBridge.Actors.cs`

Contract **v3**. These pointers live in the non-collectible bridge assembly, exactly like today's
`Bootstrap`/`Update`, which have already been proven to survive an unload.

```csharp
private sealed class ActorSlot
{
    public required AverActor Instance;
    public required long      ClassNameHash;
    public required string    Name;
    public bool Disabled;
    public int  TickGroup;
}
private static readonly Dictionary<int, ActorSlot> s_actors = new();       // keyed by entity
private static readonly List<ActorSlot>[] s_byGroup = { new(), new(), new() };
private static readonly Dictionary<long, Type> s_actorTypes = new();

[UnmanagedCallersOnly]
public static int ActorBind(long classNameHash, int entity)
{
    if (!s_actorTypes.TryGetValue(classNameHash, out Type? t))
    {
        // Never silently dropped: the entity keeps its transform and its overrides, and the
        // outliner renders the row red with this reason as a tooltip.
        SetBindFailed(entity, $"no C# type declares class hash 0x{classNameHash:X16}");
        return 0;
    }
    try
    {
        var a = (AverActor)Activator.CreateInstance(t)!;
        a.Self = new Entity(entity);
        var slot = new ActorSlot { Instance = a, ClassNameHash = classNameHash, Name = t.FullName ?? t.Name };
        s_actors[entity] = slot;
        s_byGroup[slot.TickGroup].Add(slot);
        PushOverrides(a, entity);          // native [Editable] values -> the fresh object's fields
        return 1;
    }
    catch (Exception ex) { SetBindFailed(entity, $"{ex.GetType().Name}: {ex.Message}"); return 0; }
}

/// <summary>
/// ONE managed transition per tick group per frame. The bridge walks its own dense list rather than
/// being called per entity, which is the whole reason this design can afford managed actors at all.
/// No try/catch around the loop on purpose — one actor throwing must not stop the ones after it, so
/// the guard is per actor, inside, exactly as HostBridge.Update already does it.
/// </summary>
[UnmanagedCallersOnly]
public static void ActorTickAll(int tickGroup, float dt)
{
    List<ActorSlot> list = s_byGroup[tickGroup];
    for (int i = 0; i < list.Count; ++i)
    {
        ActorSlot s = list[i];
        if (s.Disabled) continue;
        try { s.Instance.OnTick(dt); }
        catch (Exception ex)
        {
            s.Disabled = true;
            Log.Error($"{s.Name}.OnTick threw: {ex.GetType().Name}: {ex.Message} - the actor has been disabled");
        }
    }
}
```

**Class declaration at load**, in `TryLoadAssembly`'s second accept clause — types deriving
`AverActor` are **not** instantiated. For each: `aver_fw_class_declare(attr.Name, attr.Parent)`,
`class_reset`, `class_set_flags(MANAGED|TICKS)`, invoke static `Configure(ClassBuilder)` if present,
walk `[Editable]` fields declaring one native field each with its default read off a freshly
constructed prototype, then for a game mode write `set_default_pawn` / `set_player_controller` by
name, then `class_seal`. Classes are declared before any world exists, which is why the registry lives
on the GameInstance.

**Bind-time sanity check**, in `Bootstrap`: assert `aver_scene_abi_version() == 1` and
`aver_fw_abi_version() == 1` and decline loudly on mismatch. There is no
`NativeLibrary.SetDllImportResolver` anywhere in this tree, so a P/Invoke that silently binds a
private copy of `Aver.Scene` would run every script against an empty world and report nothing at all.

---

## 7. Frame lifecycle

### 7.1 One required reorder, and it cannot be deferred

`handleManip` currently runs as the first statement of `SandboxApp::onRender` (`:569`), after that
frame's camera matrices were computed in `onUpdate` (`:529-533`). **It moves into `onUpdate`,
immediately before the framework tick.** Then a gizmo drag is simply another writer of a local
transform, the tick observes it the same frame, and the flush composes once.

Left where it is, a drag writes *after* the flush and the drawn matrix trails by a frame, and the
tick's output is silently overwritten one phase later.

This will move pixels, so the gates re-baseline is announced **before** the migration starts, with a
delta table and a stated reason — that is the tree's own rule, and the announcement has to precede the
first line, not follow the red.

### 7.2 Process start — `SandboxApp::onInit`

1. `scene_.init()`. `Aver.Scene` registers built-in components 1..8 and their field tables. Index 0 is
   burned so 0 stays invalid.
2. `fw_.init(&scene_)`. `Aver.Framework` registers its **native** default classes first — `Actor`,
   `StaticMeshActor`, `PointLight`, `DirectionalLight`, `CameraActor`, `Pawn`, `PlayerController`,
   `GameMode`, `Aver.Framework.DefaultGameMode` — so a project with no C# and no CLR at all still has
   a working Play. Then it spawns the GameInstance entity and calls `beginPlay(SPAWN)` on it. Play
   state EDITOR.
3. `scripts_.init(desc)`. The existing hostfxr bootstrap, contract bumped **v2 → v3** (three new bound
   methods: `DeclareClasses`, `ActorBind`, `ActorTickAll`). Immediately after a successful
   `Bootstrap` the **executable** — not the framework, not the script host module — calls
   `aver_fw_install_managed_dispatch(&d)`. `Aver.Framework` links Core/Assets/Scene and must never
   learn that a CLR exists; the exe owns both ends, which is the same argument
   `sandbox/CMakeLists.txt:22-27` already makes about Voxi and scripting.
4. `scripts_.loadScripts(dir)`. Classes are declared and sealed. `AverBehaviour` types keep their
   existing one-instance-per-type meaning untouched, so nothing already written breaks.
5. The world is created and populated; the editor's `std::vector<MeshObj>` is gone. `sel_` becomes an
   `Entity` with 0 meaning nothing selected, which finally satisfies the 0==invalid rule that
   `int sel_ = 1` breaks today.
6. `watcher_.start(project.contentDir() + "\\Scripts")` — 150 ms settle, 1000 ms max hold.

**World load.** `NODE` lines create entities and transforms; `CLASS <objectId> <ClassName>` lines call
`aver_fw_attach_class` after resolving by name hash. An attach whose class is not registered is
**PARKED** and shown in Details as "class not found", never dropped.

### 7.3 Per frame — `SandboxApp::onUpdate`, exact order

1. DPI check and font-atlas rebake (must stay outside the ImGui frame, as today).
2. Editor camera.
3. `handleManip(e)` — **moved here**. In PLAYING state the gizmo is disabled entirely, which removes
   the conflict where it would actually matter; in EDITOR state the gizmo *authors* the transform and
   nothing else wrote it, so "the manipulator wins" is true rather than merely asserted.
4. `fw_.tick(t.dt)` — the whole scheduler, and deliberately not a general system graph:
   - **a.** `dt` clamped to `kMaxFrameDt` (0.1 s). The clamp lives here and not in `Engine`, because
     `Engine` has no business deciding what a gameplay frame is, and `Engine.cpp:101` returns before
     the `frameClock_.restart()` at `:112`, so the first frame after a resize drag carries the whole
     drag.
   - **b.** drain the deferred-spawn queue: `memcpy` each component's defaults out of the sealed blob,
     apply the transform override, and for a managed class call `managed_.bind(nameHash, e)`. The
     queue is **double-buffered** and swapped before draining — an actor spawning another actor inside
     a tick is the case that works in testing and crashes in a real scene.
   - **c.** drain the begin-play queue: `beginPlay(SPAWN or PLAY)` in spawn order. An actor spawned
     during another actor's tick lands here at the *start of the next tick*, never inside the tick
     that created it, so spawn is re-entrancy-safe by construction and the ordering is reproducible.
   - **d.** sweep: `aver_scene_valid` over each class's instance vector. This is how the framework
     hears about the editor deleting an entity without Scene owning a callback into a module above it.
     It costs a `u32` compare per actor and means `endPlay(DESTROY)` can fire up to one frame late.
   - **e.** for each tick group: the hoisted per-class native loop, then **one** managed
     `tick_all(group, dt)`.
   - **f.** drain the destroy queue: `endPlay(DESTROY)`, unbind, scene destroy.
5. `scripts_.update(t.dt)` — free-standing `AverBehaviour`s, **after** actors, preserving today's
   semantics.
6. `aver_scene_flush()` — retire destroys, rebuild the topological order if a parent link moved,
   propagate world matrices in one linear pass. **This is the single point in the frame where world
   matrices become correct**, and it is after all gameplay writes and before anything reads. Gameplay
   that needed one mid-tick got it through the on-demand path and did not pay for a second sweep.
7. Viewport rect, camera matrices from the resolved `CCamera`, light, sky
   (`SandboxApp.cpp:525-540`).

**`onRender`.** `Aver.Scene.Renderer` walks the `CMeshRenderer` pool, joins `CWorld` by entity and
issues `drawMesh`. Draws are recorded in `onRender`, never `onUpdate`, per the standing rule.

### 7.4 Stop → Play

The toolbar's dead `ImGui::Button("Play")` at `:1005` finally gets a handler.

1. `aver_scene_snapshot()` — a `memcpy` per pool. **This must be proven working before Play is
   enabled**, not alongside it: a Stop that restores an incomplete snapshot loses the user's level
   edits silently, which is a worse outcome than the buttons staying inert.
2. Spawn the world's GAMEMODE class → spawn `playerControllerClass` → find a PlayerStart entity →
   spawn `defaultPawnClass` there → possess → `OnPossessed`.
3. Every pre-existing attached actor is queued for `beginPlay(PLAY)`, in `tickList_` order so the
   ordering is the one the tick will use.
4. Play state PLAYING. Pause suppresses step 4e only.

### 7.5 Play → Stop

`endPlay(STOP)` in **reverse** `tickList_` order, so a GameMode tears down after the actors it
governs; unbind every managed instance; destroy everything spawned since Play; `aver_scene_restore`.
The scene is bit-identical to the moment Play was pressed, including entities the game created and
destroyed. **The GameInstance is not destroyed** — that is precisely what distinguishes it from a
GameMode.

### 7.6 Hot reload

Main thread, triggered by `DirectoryWatcher` → 250 ms debounce → `dotnet build` on the existing worker
→ **exit code 0 only**. `ToolsMenu::reapCompile`'s rule stands unchanged: a failed build must **not**
unload, or a typo leaves the editor with no scripts at all.

1. `aver_fw_reload_begin()` — for every managed actor in reverse tick order: `endPlay(RELOAD)`,
   unbind. No entity is destroyed and no class is cleared. Class records are marked stale but their
   **slots are kept**, so an `aver_class` held by the Details panel resolves to "same class,
   reloading".
2. `aver_fw_clear_managed_dispatch()` — a NULL store the thunk guards already handle, so a reload that
   fails halfway leaves a framework that ticks nothing rather than one that crashes.
3. `scripts_.unloadScripts()` — the existing `DrainAndUnload` untouched, including the `NoInlining`
   that is load-bearing and the bounded two-cycle collect. **False is not a failure.**
4. `scripts_.loadScripts(binDir)` — every class redeclared. `declare()` is idempotent by name, so each
   gets back the handle its live entities already store; `reset` + `Configure` + `[Editable]` + `seal`
   rewrites the descriptor behind that handle in place.
5. `aver_fw_install_managed_dispatch(&d)`.
6. `aver_fw_reload_end()` — re-resolve each attachment by name hash, bind, push the native
   `[Editable]` overrides back into the fresh object through cached `FieldInfo` setters, call
   `OnRebound()`, then `beginPlay(RELOAD)` only if the world was already playing.

**What survives a reload:** every entity handle, transform, hierarchy link, name, ObjectId and
component byte; every class handle; which entities carry which class, including runtime spawns that
were never in a file; possession pairs; play state; the Play snapshot; and every `[Editable]` value
including per-instance overrides a designer typed into Details.

**What does not:** non-`[Editable]` C# fields, which come back at their initialisers — the same rule
`AverBehaviour` already has, and the escape hatch is a one-word `[Editable]` the user can apply
themselves, which is why no serialisation contract for arbitrary managed state is invented here
(`docs/STATUS.md:822-824` refused to invent one, correctly).

A class whose type vanished is **ORPHANED** and its entities **PARKED**, listed by name, restored
intact when the name comes back.

### 7.7 Shutdown

`aver_fw_set_play_state(EDITOR)` (running the Stop path if needed), `endPlay(DESTROY)` on the
GameInstance, `aver_fw_clear_managed_dispatch()`, `scripts_.shutdown()`, `fw_.shutdown()`,
`scene_.shutdown()`.

The clear **must** precede the ScriptHost shutdown and the world **must** outlive the managed
teardown — `endPlay` reads components, and a world torn down first hands every one of those reads a
dangling pool.

### 7.8 Native C++ classes do not reload

`aver_fw_class_set_vtable` exists for a future plugin DLL, but with the current build shape changing a
native actor class means a relink and a restart. Pretending otherwise would be the one dishonest claim
available here, so it is not made.

---

## 8. Build order

Fourteen steps. Each carries its own verification, and the verification is the point — several of the
steps exist *only* to retire a risk before anything can depend on the answer.

- [x] **1. Prove the build shape before anything depends on it.** — **DONE, commit `03b79e1`.**
  Create `modules/scene` and `modules/framework` with their CMakeLists exactly as specified, each with
  one trivial exported function and one `.cpp`. Wire both `add_subdirectory` lines and the
  `option()`/force-off pair. Link both into Sandbox. Write the `ARCHITECTURE.md` delta adding
  `Framework -> Core, Assets, Scene` at Tier 5 and commit it in the same change.
  **Verify:** configure and build clean with the msvc-ninja preset. `dumpbin /exports`
  `build\bin\Aver.Scene.dll` and `Aver.Framework.dll` each show their one symbol;
  `dumpbin /dependents Aver.Framework.dll` lists `Aver.Scene.dll` and **not** any RHI DLL.
  `Sandbox.exe` launches. This retires the SHARED-links-SHARED risk on day one rather than discovering
  it after ten files exist.

  > **This step justified itself immediately, by failing while looking like it had passed.** The first
  > build was clean: both DLLs linked, both exported their symbols, `Sandbox.exe --frames 10` ran and
  > exited 0 with the probe reading `raw(90,93,108)`, matching the oracle baseline. And
  > `dumpbin /dependents Aver.Framework.dll` listed **no edge to `Aver.Scene.dll` at all** — because
  > the framework's only reference across the boundary was `AVER_SCENE_ABI_VERSION`, a header
  > constant, so the linker emitted no import. The exact risk the step exists to retire was still
  > entirely untested, behind a green build and two DLLs sitting on disk.
  >
  > The repair is `aver_fw_scene_abi_matches()`, which genuinely calls `aver_scene_abi_version()`
  > across the boundary and compares MAJOR only (minor bumps are additive by §5's contract, so
  > refusing one would reject a DLL that is fine). Both the header and `ARCHITECTURE.md` record that
  > it must stay a real call: if it is ever folded back into a constant the check becomes a tautology
  > and the edge disappears again, silently, with nothing failing.
  >
  > The delta landed as three edge-list rows rather than one — `Scene.Renderer` needed naming, and the
  > `Scene` row needed the note that it is SHARED with no RHI behind its P/Invoke boundary.

- [ ] **2. Entity, ComponentPool, World lifetime.**
  `Entity.hpp` on Core's `AvId` with the 24/7/bit-31-clear packing, FIFO free list, slot retirement on
  generation wrap. `ComponentPool` with sparse storing dense+1. World `create`/`destroy`/`valid`/
  `count`/`at`, the name blob and `CName`. New target `tests/scene` building `SceneTest.exe`.
  **Verify:** `SceneTest` asserts: `makeEntity` never yields 0 for any legal (index, gen); a destroyed
  handle fails `valid()` while a fresh one at the same index succeeds; a pool's sparse entry reads 0
  for an absent entity; 100k create/destroy cycles leave `size()` and the free list consistent; a slot
  forced to gen 127 is retired rather than aliased.

- [ ] **3. Field tables and registration.**
  `Fields.hpp`, `ComponentBuilder`, `.verify(sizeof(T))`, the dense field-id space,
  `componentId`/`fieldId` lookup. Register the eight built-ins in `Builtins.cpp` with their tables.
  **Verify:** `SceneTest` enumerates every registered component and field, checks
  `aver_scene_component_size` against `sizeof` for each, and asserts that a table deliberately missing
  a member fails `verify()` with the component named. A wrong `offsetof` is caught here rather than
  three layers away.

- [ ] **4. Transform, hierarchy, dirty propagation.**
  `CLocal`/`CWorld`/`CHierarchy`, intrusive links, `setParent` with cycle rejection, `order_` rebuilt
  only on reparent via a Kahn sweep, the revision-compare propagation pass, and the on-demand
  `worldMatrix(e)` path.
  **Verify:** `SceneTest` builds a three-deep chain and asserts the composed world matrix equals
  `Transform::toMatrix()` composed by hand left-to-right, element for element; asserts translation
  lands in the **last row**; asserts `setParent(child, descendant)` returns false and leaves the sort
  intact; asserts that moving a root recomposes exactly its subtree and no other entity's `CWorld.rev`
  changes; asserts `worldMatrix()` mid-frame equals the value the next flush produces.

- [ ] **5. `scene_abi.h` and `SceneAbi.cpp`.**
  The full header as specified, plus the implementation, the snapshot/restore `memcpy`,
  `query_create` over a typed array, `query_tags`, and `consume_dirty`.
  **Verify:** a three-line **C** (not C++) consumer compiles against the header with `/TC`, proving no
  C++ leaked in. `dumpbin /exports` lists every declared symbol. `SceneTest` drives the ABI end to end
  and asserts snapshot → mutate → restore leaves every pool byte-identical.

- [ ] **6. C# scene binding inside Aver.Scripting 1.1.0.**
  `Scene.cs`, the version bump, the `LPUTF8Str`/`PtrToStringUTF8` pair everywhere, and the
  `abi_version` assert at `Bootstrap`. No framework types yet.
  **Verify:** `Aver.Scripting.Sample` creates an entity, sets a position, reads it back, sets a
  non-ASCII name and reads it back unchanged (this is the test the existing `PtrToStringAnsi` bindings
  would fail). An assembly built against 1.0.0 still loads, proving `CheckApiVersion`'s minor-bump
  behaviour.

- [ ] **7. `Aver.Scene.Renderer`, and migrate the editor's object list.**
  `SceneRender.cpp` walks `CMeshRenderer` joined with `CWorld`. Delete `std::vector<MeshObj>`; `sel_`
  becomes an `Entity`; the Sun and Sky pseudo-entries (`-2`/`-3`) become real entities with `CLight`
  and `CTags`; the outliner walks `CHierarchy`; `handleManip` moves into `onUpdate`.
  **Announce the gates re-baseline before starting this step.**
  **Verify:** `./scripts/gates.ps1` across all nine device configurations, with the stated reason and a
  delta table for every gate that moved. Draw order changes from vector order to dense-array order and
  `handleManip` moves a phase earlier, so movement is expected; what must be argued is which gates held
  **exactly** and why.

- [ ] **8. ClassRegistry: declare, seal, spawn.**
  `ClassRegistry.hpp`/`.cpp` with idempotent-by-name `declare`, hash-collision refusal, parent-chain
  flattening into the `SealedArchetype` blob, vtable-by-value with NULL-slot inheritance at seal, and
  `Framework::spawn` as a `memcpy` loop. New target `tests/framework`.
  **Verify:** `FrameworkTest` asserts: declaring the same name twice returns the same handle and the
  second declare rewrites the descriptor in place; a cycle in the parent chain fails seal with both
  classes named; a spawned entity carries every component of the whole chain with the defaults
  byte-identical to the blob; a name whose hash collides with a different existing name is refused.

- [ ] **9. `framework_abi.h`, native default classes, play state.**
  The full header and implementation, `DefaultClasses.cpp` registering `Actor`/`StaticMeshActor`/
  `PointLight`/`DirectionalLight`/`CameraActor`/`Pawn`/`PlayerController`/`GameMode`/
  `DefaultGameMode` as native rows, possession with flag checks, the tick with hoisted per-class
  dispatch, the double-buffered spawn queue, and the deferred destroy queue.
  **Verify:** `FrameworkTest` runs Play → tick → Stop with **no CLR present at all** and asserts: a
  native spawner ticks; `possess()` rejects a non-PAWN class and a non-CONTROLLER class; an actor
  spawned inside another actor's tick runs its first tick on the *following* frame; an actor destroyed
  inside a tick is still valid for the rest of that tick. Then the same run inside the editor with
  `AVER_MODULE_SCRIPTING=OFF`.

- [ ] **10. `framework_hooks.h`, the fixed thunks, contract v3.**
  `AvActorVTable` and `AvManagedDispatch`, the five fixed native thunks,
  `aver_fw_install`/`clear_managed_dispatch` with second-install refusal, the ScriptHost contract bump
  and the three new bound entry points added to the ordered bind list in `ScriptHost.cpp:185-189`
  **and** to the CMake `DEPENDS` list.
  **Verify:** a stale `Aver.Scripting.Bridge.dll` in `build/bin` declines with one line rather than
  crashing. Installing a dispatch twice returns 0 and logs. Clearing the dispatch mid-frame leaves the
  tick ticking nothing rather than faulting. Verify the `DEPENDS` addition by touching only a `.cs`
  file and confirming a clean build ships the **new** bridge — this is the exact failure mode the
  CMake comments say has already shipped once.

- [ ] **11. C# authoring layer and class declaration at load.**
  `Framework.cs`, the attributes, `ClassBuilder`, the bridge's `DeclareClasses` pass reflecting
  `[AverClass]`/`[AverGameMode]`/`[Editable]`, `ActorBind`, `ActorTickAll` and the per-group dense
  lists. **Tools ▸ Generate Script Ids** emitting `SceneIds.g.cs` from the live registry.
  **Verify:** the `Spinner` example rotates in the viewport with **zero C++ edits**. Deleting
  `SceneIds.g.cs` and regenerating produces a byte-identical file. Renaming a component in C++ makes
  the generated file change and the old `ClassBuilder` call fail to **compile** rather than silently
  writing field 4 of the wrong component.

- [ ] **12. Reload: park, orphan, rebind, `OnRebound`.**
  `aver_fw_reload_begin`/`end`, the parked list, ORPHANED class marking, `PushOverrides` through
  cached `FieldInfo`, the FAILED state with a reason string, and the load-time warning when a type
  overrides `OnBeginPlay` but not `OnRebound`.
  **Verify:** set `DegreesPerSecond = 45` on one of twenty spinners in Details, edit the `.cs`,
  rebuild: that one instance still reads 45 and the other nineteen read 90, and the spinner keeps its
  position. Rename the class, reload, confirm the entity is PARKED and listed by name with its
  overrides intact; rename it back and confirm the actor returns whole. Introduce a compile error and
  confirm **nothing unloads**.

- [ ] **13. Play / Pause / Stop wired to the toolbar.**
  Snapshot on Play, restore on Stop, GameMode → PlayerController → PlayerStart → default pawn →
  possess, `endPlay` in reverse tick order, gizmo disabled while PLAYING.
  **Verify:** Play, let actors spawn and destroy and move for 30 seconds, Stop: assert the restored
  world is byte-identical to the snapshot taken at Play, pool by pool, including entities the game
  created and destroyed. **Do not enable the buttons in the UI until this assertion passes in
  `FrameworkTest`.**

- [ ] **14. DirectoryWatcher wired, and the editor's generic panels.**
  Add `DirectoryWatcher.cpp` and `win32/Win32DirectoryWatcher.cpp` to
  `modules/platform/CMakeLists.txt` `SOURCES` (they are on disk, untracked, and compile into nothing
  today with no diagnostic). Debounce 250 ms into the existing `startCompile` path; `poll()` returning
  true triggers a full rescan. Details panel becomes one generic loop over field tables; the Add menu
  is `class_count()`/`at()` filtered by ABSTRACT; FAILED and PARKED rows render red with the reason as
  a tooltip.
  **Verify:** saving a `.cs` from an external IDE reloads the viewport within a second without
  touching the Tools menu. A newly authored C#-only class appears in **Add ▸** on the next reload with
  no C++ edit. Add an `[Editable] Vec3` to a script, rebuild, and confirm a colour/vector row appears
  in Details **with no editor code written for it** — the panel must not know what the field is.

---

## 9. Open questions

These are the honest limits of the design. None of them is softened, and none has an answer yet.

1. **Nothing serialises a world yet**, and this design assumes `.ocworld` eventually stores
   `(className, instanceName, overrides)`. There is no `.ocworld` reader, no writer for any `.oc*`
   format in `modules/formats`, and `.ocmap`'s `PLACE` has no per-placement object id (both branches
   derive it from the asset name, `OcMap.cpp:95` and `:107`). Everything above works **in memory
   only** until the tree grows its first format writer, and the `CLASS <objectId> <ClassName>` line
   this design needs is a format addition that must be specified before the first save button exists.

2. **The gates oracle is a renderer oracle.** It compares raw 8-bit pixel codes across nine device
   configurations and would notice a scene change only if it moved a pixel. A world tick, possession,
   spawn ordering, PIE snapshot/restore and reattach-after-reload all need their own test target, and
   the tree has no CTest, no `enable_testing()` and exactly one hand-run test executable to copy from.
   `tests/scene` and `tests/framework` follow that precedent, which means **nothing runs them unless a
   human does**.

3. **Whether SHARED-links-SHARED needs anything beyond the import lib on Windows is asserted, not
   known.** Step 1 exists to answer it before anything depends on the answer, but the staging rules
   for `build/bin` have never been exercised for a DLL that another DLL depends on, and a load-order
   surprise would land on the very first configure.

4. **Attach-as-reclass mints derived classes the user did not name** (canonical parent + sorted mixin
   names). They appear in the outliner and the user can be confused by them, and there is no undo
   system anywhere in the tree, so the first request for Ctrl+Z on a script attach has no mechanism
   behind it at all.

5. **The framework sweeps its instance lists with `aver_scene_valid`** rather than taking a destroy
   callback from Scene. That keeps the arrow pointing down and costs a `u32` compare per actor, but
   `endPlay(DESTROY)` fires **up to one frame after** the entity went away, and any resource an actor
   holds on the entity's behalf is released one frame late. Whether that is acceptable is untested and
   cannot be tested until something holds such a resource.

6. **Seven generation bits gives 127 reuses of an index before retirement.** Retiring rather than
   wrapping is affordable at 16.7M indices, but a world that churns entities hard enough to retire
   slots in bulk has a bug worth seeing, and there is currently **no plan for what the editor does
   when the retirement counter climbs**.

7. **Sim determinism is documented as a linkage guarantee** (`ARCHITECTURE.md:372`). Native actor
   classes tick through a fixed vtable and are in scope for it; managed actors are JIT-compiled and
   are not. This design does not extend the guarantee upward and does not pretend to, but the boundary
   needs writing down before a C# class ends up inside something the Rust validator is expected to
   reproduce.

8. **`aver_scene_at` and the query wrapper share the hazard `aver_pbr_at` already documents:** indices
   are dense over live entities and **shift on destroy**, so a loop that destroys while iterating
   skips entities. The header says so; nothing enforces it, and the `Query` wrapper re-evaluating on
   every `Count` call makes the mistake easy to write and hard to see.

9. **A behaviour disabled by a throw stays disabled for the rest of the session** with the entity
   still sitting in the outliner looking fine. There is no API to list or re-enable disabled actors
   short of a full reload, and making Play real means the editor now has a mode where one bad tick
   silently removes an actor from the simulation.

10. **One world per process** removes a parameter from every ABI call and a lifetime question from
    every handle, and it makes multi-world, world previews and PIE-in-a-separate-world impossible
    without additive ABI. That is a deliberate option kept open rather than an oversight, but the
    additive entry points should be sketched before someone asks for a second viewport.

---

## 10. Inconsistencies inside this design, found while writing it down

Recorded here rather than silently repaired, because each one is a decision an implementer will hit in
the first hour and should hit with the trade already visible.

- **`ClassRegistry::setDefault*` takes `(Class, u32 type, i32 field)` but the ABI takes
  `(aver_class, aver_field)`.** The ABI form is the intended one — the whole point of the dense field
  id is that it already carries the component. The C++ signature above still carries the redundant
  `type` parameter; it should lose it, and the ABI is the side that is right.
- **`ClassBuilder.Mesh` writes `set_default_str` into `CMeshRenderer`.** `CMeshRenderer.mesh` is a
  `u64` ObjectId (kind `I64`) and `material` is an `i32`, so those two calls would be **rejected by
  the ABI's own kind check**. Either the builder resolves a path to an ObjectId before writing (which
  needs an Assets lookup on the managed side), or `CMeshRenderer` grows string-kind path fields. This
  is unresolved and it lands in step 11.
- **`ActorSlot.TickGroup` is never assigned** in `ActorBind` before it is used to index `s_byGroup`,
  so every managed actor would land in group 0. The class record knows its tick group; the bind path
  has to read it.
- **`CName` is specified as `{ nameId, offset, len }` but the ABI exposes
  `aver_scene_get_object_id` / `set_object_id`.** Persisted identity is described as "CName's
  ObjectId"; there is no such member. Either `nameId` *is* the ObjectId (in which case the name blob
  slice is the display name and the field should say so), or `CName` needs a fifth member.
- **`aver_fw_class_set_vtable` returns `aver_class`** while its C++ counterpart returns `bool`. A
  setter returning a handle is odd; the C++ form is the sane one and the header should follow it.
- **Two debounces exist for one gesture.** `DirectoryWatcher` settles at 150 ms
  (`modules/platform/README.md`), and hot reload adds a further 250 ms before `startCompile`. That is
  defensible as two layers — one coalesces OS records, one avoids compiling mid-save-burst — but it is
  400 ms of latency from a save and nobody has argued that number.
- **`Framework::tick` references `kDead`**, which no header above declares.
- **`AverActor.Spawn` allocates a `float[3]` per call**, inside the same design that justifies
  `[ThreadStatic]` scratch buffers elsewhere on GC-pressure grounds. Spawn is rarer than a transform
  write, so it may be fine; it is simply inconsistent with the stated reasoning.
