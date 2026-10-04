# The generated-region write-back contract (LOCKED)

This is the locked grammar that lets a viewport edit rewrite coordinates in a `.cs` on save, and the
ledger of how this authoring contract resolves eight contradictions the scene/framework design flagged.
Downstream code commits to these exact shapes.

Companion source (all under `scripting/csharp/`):

- `Aver.Framework/Attributes.cs` — `[AverClass]`, `[AverGameMode]`, `[Editable]`, `[Model]`.
- `Aver.Framework/Actor.cs`, `Pawn.cs`, `PlayerController.cs`, `GameMode.cs` — the base hierarchy.
- `Aver.Framework/ClassBuilder.cs` — the class recipe (`Configure`).
- `Aver.Framework/ActorBuilder.cs` — the model-placement grammar (`Place`, `ModelHandle`).
- `Sample.Game/Car.cs` (user half) + `Car.Designer.cs` (editor half) + `SandboxMode.cs` — the worked example.

The base-class-over-attribute decision, the exception-never-crosses-back rule, and `0 == invalid` are
inherited unchanged from `Aver.Scripting/Behaviour.cs`; they are not re-argued here.

---

## 1. Two halves of an actor

An actor is one `partial class` split across two files:

- **The user half** (`Car.cs`) — hand-written behaviour. `[Editable]` fields, hooks, `Configure`, and any
  code that *computes* placement. The editor reads `Configure` to display declared assets (mesh, camera,
  light) for preview, but writes only the generated region. The write boundary is absolute; what reading
  the `Configure` method enables is preview of actors that are not multi-part models.

- **The editor half** (`Car.Designer.cs`) — the model tree the actor viewport shows. Generated, and
  rewritten on save. It declares the `[Model]` slot properties and one `BuildModels(ActorBuilder)`
  override whose body is a flat list of `Place(...)` statements.

The split is not stylistic. A gizmo drags a *coordinate*; it cannot drag a *formula*. So a looped or
computed placement lives in the user half and is, correctly, not gizmo-editable — the editor only ever
rewrites text it generated.

---

## 2. The generated region

The editor owns exactly the bytes **strictly between** two marker lines, and nothing else:

```
    // <aver-generated region="models" schema="1"> ...
        ...  (editor territory: property declarations + BuildModels body)
    // </aver-generated>
```

Marker match is anchored and exact (leading whitespace ignored):

- open:  `^\s*//\s*<aver-generated region="models" schema="1">`
- close: `^\s*//\s*</aver-generated>`

Rules the editor obeys without exception:

1. It reads and writes **only** the span between the two marker lines. The marker lines themselves, the
   `using`, the `namespace`, the `partial class` header, and every other file — above all `Car.cs` — are
   read-only to the editor.
2. If the open marker is absent, the file has no editor territory: the editor adds a whole region (with
   both markers) or refuses, but never edits loose text.
3. `schema="1"` is the grammar version below. A file at an unknown schema is left untouched and surfaced
   as "open in a newer editor"; the editor never guesses.

### 2.1 The placement statement — fixed shape

Inside `BuildModels`, every model is one statement of exactly this token sequence:

```
    <PropertyName> = b.Place(<ObjectId>, <MeshPath>, material: <Material>, pos: (<n>, <n>, <n>), rot: (<n>, <n>, <n>), scale: (<n>, <n>, <n>));
```

| Token          | Grammar                                             | Meaning                                             |
|----------------|-----------------------------------------------------|-----------------------------------------------------|
| `PropertyName` | a C# identifier declared `[Model] ... { get; private set; }` above | code identity of the slot |
| `ObjectId`     | `0x` + 1–16 hex digits + `UL`                       | **the match key** — per-placement identity, u64     |
| `MeshPath`     | a `"..."` string literal                            | content path; hashed to the mesh I64 ObjectId       |
| `Material`     | a `"..."` string literal                            | material name; resolved to the I32 handle           |
| `n`            | `-?[0-9]+(\.[0-9]+)?f`                               | a C# `float` literal                                |

`pos`/`scale` are centimetres; `rot` is degrees `(yaw, pitch, roll)`. The three coordinate tuples are the
**only** rewritable payload. The argument order is fixed: id, mesh, material, then `pos`, `rot`, `scale`.

The `ObjectId` is also stamped onto the placed child entity (`aver_scene_set_object_id`), so the runtime
entity a gizmo grabs in the viewport carries the very key that finds its line here.

---

## 3. The save-time rewrite rule

Two edit kinds. Both produce byte-identical output for the same model set, so an unchanged save is a
no-op diff.

### 3.1 Coordinate rewrite (the common case: a gizmo drag)

Input: a model's `ObjectId K` and its new `(pos, rot, scale)` from the gizmo.

1. Locate the region (§2). Fail closed if absent.
2. Scan region statements for the **unique** `Place(` whose first argument, parsed as u64 (strip `0x`,
   `UL`), equals `K`.
   - 0 matches → this is an *add*, not a rewrite (§3.2).
   - >1 match → hard error: ObjectIds are unique within a region; the editor stops and reports, it does
     not pick one.
3. On that statement, replace **only** the nine numeric literal tokens inside the `pos:`, `rot:` and
   `scale:` tuples with the new values, each formatted by §3.3.
4. Write the file. Everything else is preserved byte-for-byte: the property name, `MeshPath`, `Material`,
   the `UL` suffix, inter-token whitespace, trailing comments, and the order of statements.

The rewriter **never**: reorders arguments; touches `MeshPath` or `Material` (a gizmo cannot change an
asset — a material swap is a separate explicit command that goes through §3.2); touches a `[Model]`
declaration; edits outside the region; or reformats untouched statements.

### 3.2 Structural change (add / remove / reorder / rename / material swap)

These change the *set* or *names* of models, not just coordinates. The editor regenerates the **entire
region body** deterministically from the live model list:

- Emission order: model creation order (a monotonic per-actor counter), tie-broken by `ObjectId`
  ascending. Reordering in the outliner reassigns the counter.
- Property declarations first (one per model, in emission order), then `BuildModels` with one `Place`
  statement per model in the same order.
- Formatting is canonical and fixed: four-space indent inside the class, one statement per line, a single
  space after each comma, named arguments always present (`material:` emitted even when empty as `""`).
- Property names come from the model's outliner name, sanitised to a valid C# identifier and de-duplicated
  with a numeric suffix. A rename regenerates the declaration and its `Place` line together; the durable
  identity remains the `ObjectId`, so hand code that referenced the old name breaks at compile time
  (visible, not silent) rather than binding to the wrong model.

Because full regen is deterministic, a coordinate-only save (§3.1) and a regen of the same set differ
only in the numeric tokens — never in layout — so the two paths never fight each other in version control.

### 3.3 Number formatting (idempotence)

To guarantee that re-saving an unmoved model yields identical bytes:

- Invariant culture; `.` decimal separator; always a trailing `f`.
- Quantise before formatting: positions/scales to `1e-3` cm, rotations to `1e-3` degrees. This kills
  float dance so a no-op drag is a no-op diff.
- Shortest round-trippable form of the quantised value; an integral value is written **without** a
  decimal point (`45f`, not `45.0f`); negative zero normalises to `0f`.

Two implementers following §3.1–§3.3 produce the same file from the same edit.

---

## 4. Lifecycle ordering and reasons (locked)

Per-frame order (world playing): drain the double-buffered spawn queue → drain the begin-play queue →
sweep invalidated entities → for each tick group in order `PrePhysics, Physics, PostPhysics` run the
native batch then the single managed `tick_all(group, dt)` → drain the destroy queue → flush world
matrices (the one point world transforms become correct). `dt` is clamped to `0.1 s` before any hook sees
it.

Hook order for one actor: `BuildModels` (once, after `Self` is bound) → `OnBeginPlay(reason)` →
`OnTick(dt)` each scheduled frame → `OnEndPlay(reason)`. `OnRebound()` fires on the reload path only,
between rebind and `OnBeginPlay(Reload)`.

Reasons: `BeginReason { Spawn=0, Play=1, Reload=2 }`, `EndReason { Destroy=0, Stop=1, Reload=2,
Travel=3 }` — pinned to the `AVER_FW_BEGIN_*/END_*` ABI values.

**Hot reload, at the C# surface.** Survives: `[Editable]` fields (native storage, parked over the reload
and pushed back) and the generated model placements (the entities are never destroyed). Resets: plain
instance fields (the managed instance is rebuilt from its initialisers). Therefore re-derive cached state
from surviving native state in `OnRebound`, never in `OnBeginPlay`.

---

## 5. One debounce, not two (contradiction #6, decided)

A single save triggers **one** debounce: the platform `DirectoryWatcher`'s **150 ms** settle, whose job is
to coalesce the burst of OS file records one editor-save emits. The hot-reload compile fires on that
settled signal with **no** additional debounce. The design's extra 250 ms compile debounce is dropped: it
was never argued, and 400 ms from keystroke to reload is latency the watcher's own coalescing already
makes unnecessary. If a future editor writes files in multiple un-coalesced bursts, fix the writer to emit
one burst rather than reintroduce a second timer.

---

## 6. The eight contradictions — how this contract resolves each

| # | The flagged contradiction | Resolution in this contract | Where |
|---|---------------------------|-----------------------------|-------|
| 1 | `setDefault*` is `(Class, u32 type, i32 field)` in C++ but `(aver_class, aver_field)` at the ABI. | ABI form wins — the dense field id already carries its component. The `Fw.aver_fw_class_set_default_*` P/Invokes take `(class, field, value)`; there is no `type` parameter. The C++ `type` argument is dropped. | `Native.cs` |
| 2 | `ClassBuilder.Mesh` writes `set_default_str` into `CMeshRenderer.mesh` (u64/I64) and `material` (i32/I32) — both rejected by the ABI kind check. **(most important)** | Mesh **path** is hashed to its I64 ObjectId in managed code (`Assets.ObjectIdOf`) and written via `set_default_i64`; material **name** is resolved to its I32 handle via `aver_scene_material` and written via `set_default_i32`. No string setter touches either field. Same split in `ActorBuilder.Place` for placed models. | `ClassBuilder.Mesh`, `ActorBuilder.Place` |
| 3 | `ActorSlot.TickGroup` is read before assignment, landing every managed actor in group 0. | `ClassBuilder.Ticks` records the group on the class row (`aver_fw_class_set_tick`) alongside the TICKS flag; the bind path reads the tick group off the **class record**, never off a zeroed slot field. The default group is thus assigned explicitly, not inherited from zeroed memory. | `ClassBuilder.Ticks` |
| 4 | `CName` is `{ nameId, offset, len }`, but persisted identity is "CName's ObjectId" and only `get/set_object_id` exist. | The built struct (`Components.hpp`) names the member `objectId` (u64). Persisted identity **is** `CName.objectId` (Assets fnv1a64); `offset/len` slice the display-name blob. `Entity.ObjectId` reads it via `aver_scene_object_id`. The design's `nameId` was the same field mis-named. | `Entity.ObjectId`, `Native.cs` |
| 5 | `aver_fw_class_set_vtable` returns `aver_class`; the C++ counterpart returns `bool`. | A setter returns the tree-wide `int 1/0`. The C# surface binds **no** vtable setter — managed dispatch is installed by the host via `aver_fw_install_managed_dispatch`, not from script — so the surface is unaffected; the header should return `int32_t` 1/0 to match the convention. | (host/header; no C# binding) |
| 6 | Two debounces (150 ms watcher + 250 ms compile) for one save, never argued. | One debounce: the 150 ms watcher settle. Compile fires on the settled event; the 250 ms is dropped. | §5 above |
| 7 | `Framework::tick` references `kDead`, which no header declares. | The tick loop skips empty slots (`if (auto* rec = classes_.at(slot)) { ... } else continue;`) — no sentinel record. There is no `kDead`. | (native `Framework::tick`) |
| 8 | `AverActor.Spawn` allocates a `float[3]` per call, against the design's own `[ThreadStatic]` scratch justification. | `AverActor.Spawn` uses `Entity.Scratch3`, the same `[ThreadStatic]` buffer the transform writes use. No per-call allocation. | `Actor.cs`, `Entity.cs` |

Two further design-internal snags reconciled while locking:

- The doc's own `Fw` class declared only `f32/i32/str` of the five default setters. All five
  (`f32/i32/i64/vec/str`) are declared, because #2's fix needs `i64` and a `Vec3` default needs `vec`.
- **Strings are UTF-8** across the scene and framework ABIs (`LPUTF8Str` + `PtrToStringUTF8`), matching
  `scene_abi.h`/`framework_abi.h`. The in-progress `Aver.Scene/Native.cs` still uses ANSI `LPStr`; that is
  a defect and must be flipped to UTF-8 so the two managed assemblies agree with the header. Non-ASCII
  names corrupt until it is.

---

## 7. Naming note

The design doc had no `ActorBuilder` type (only `ClassBuilder` and the `ActorClass` handle). This contract
adds `ActorBuilder` as a **distinct** type from `ClassBuilder`: `ClassBuilder` writes class-default
components (`Configure`, hand-written), `ActorBuilder` writes the editor-owned model tree (`BuildModels`,
generated). They are siblings, not a rename — one is a recipe, the other is coordinates.
