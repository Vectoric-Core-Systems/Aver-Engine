# Actor Editor with Live Sync

> **Status: pieces 1-4 of the viewport/sync work are built.** The preview, the designer reader and
> rewriter, the tab, the translate gizmo and source-to-view reload all exist and are in the build.
> What is NOT built: the Roslyn backend (`averdesign`), rotate/scale gizmo handles, and the tab
> infrastructure fixes in §2 Piece 1. Sections below marked *(built)* describe what shipped; the rest
> is still plan.


A tab for a C# actor class: a 3D preview of its authored model tree, a properties panel, and a save
path that rewrites the actor's `.Designer.cs`. Two-way — dragging in the preview changes the source;
changing the source changes the preview.

Nothing here is built. This is the plan, and it exists before the code for the same reason
[docs/AUDIO.md](AUDIO.md) did: the expensive decision is where the second 3D view comes from, and it
is not recoverable once something depends on the wrong answer.

**This was surveyed and then adversarially checked, and the check found ten factual errors in the
first draft.** Four of them changed a decision. Where a claim below cites a file and line, it was
read; where a decision reverses the first draft, it says so and why. That record is kept because the
same mistakes are the ones a reader will otherwise make again.

---

## 1. What already exists

**The tab seam is real and nearly unused.** `AssetEditorHost` (`sandbox/src/AssetEditor.hpp`) has a
five-virtual contract, dedup-by-path, and a first-accepting-factory registry. Its whole footprint is
four references repo-wide: one factory registered, one draw site, one open site, one member. Docking
is on globally; **multi-viewport is not enabled anywhere**, so tear-off OS windows are out of scope.

**The rewrite precedent is tested.** `fmt::rewriteMaterialScript`
(`modules/formats/src/MaterialScript.cpp`) already locates a class by attribute, replaces only its
`Configure` chain, preserves usings, prose and indentation, and refuses rather than corrupts — with
24 assertions behind it. The actor case is the same shape at greater difficulty.

**The authoring contract is implemented, not merely specified.** `ActorBuilder.Place(modelId,
meshPath, material, pos, rot, scale)` builds a child entity, stamps the ObjectId, attaches
`CMeshRenderer`, and sets local TRS *before* `SetParent` — which is correct, because `SetParent`
defaults `keepWorld=false`. `AverActor.BuildModels` is dispatched through the bridge and driven
native-side in a contracted order, with a real test in `tests/framework`.

`ClassBuilder`'s entire surface is four methods — `Mesh`, `PointLight`, `Camera`, `Ticks`. That bounds
what any "class defaults" panel could ever show.

**A `DirectoryWatcher` exists, debounced and coalescing, with a Win32 backend — and zero consumers
and zero tests.** It is a starting point, not a dependency that has been exercised.

---

## 1b. What is built, and where

| Piece | Where | Verified by |
|---|---|---|
| Reading the generated region, and rewriting coordinates | `modules/formats/{include/aver/formats,src}/ActorScript.*` | `ActorScriptTest`, 51 assertions |
| Reading what a class declares (`Configure`) | same | same |
| The preview render feature | `modules/render.actorpreview/` | `ActorPreviewTest`, 51 assertions |
| The preview's own mesh registry | `modules/render.actorpreview/src/PreviewMeshCache.cpp` | — |
| The tab, gizmo and reload | `sandbox/src/ActorEditor.{hpp,cpp}` | not covered by a test |
| The **Live** view — spawn the class, read what it built | same, plus `aver_fw_spawn_preview` | `FrameworkTest` (the ABI edge); `--open-asset … --actor-live` (the tab) |

**It is general, and that was checked against real projects rather than a fixture.** A sweep over
SkyForge's scripts opens `Gun.cs` (2 actors), `FpsGameMode.cs` (5 actors, 2 previewable) and
`Target.cs`, and over the engine's own sample opens `Car.Designer.cs` (5 placements). Files that
declare nothing previewable are skipped rather than opened empty. Two bugs came straight out of that
sweep and neither would have shown up in a fixture:

1. Attribute kinds were searched in turn, so a file naming a `[AverGameMode]` above an `[AverClass]`
   came back named after the wrong one. Attributes are now collected in **file order**.
2. A file was assumed to declare one actor. Real ones declare several, and each entry now carries
   only what appears between its own attribute and the next — so two actors in one file cannot
   borrow each other's mesh. The tab shows a picker.

## 2. The four pieces, in dependency order

| # | Piece | Size |
|---|---|---|
| 1 | Make the editor host capable of authoring | small–medium |
| 2 | `averdesign` — Roslyn read/write of the generated region | medium |
| 3 | The preview viewport | **large** |
| 4 | Live sync | medium |

### Piece 1 — the host

The virtuals are trivial; every one is a call site with an ordering decision.

- `draw()`'s return value is **discarded**, so with an editor focused `Delete` still deletes the level
  selection and `Ctrl+Z` still rewinds the level's undo stack.
- `tick()` does not exist, and is not optional: ImGui skips `draw()` entirely for a collapsed window
  **or an inactive dock tab** — which is the common case for a tab-based editor.
- `anyDirty()` and `save()` have **zero callers**, despite the header claiming the host asks before
  closing. Today a dirty editor is destroyed after a warning.
- A rename leaves an editor keyed on a dead path and lets a second open for the same asset.

Three things the first draft missed, all found by the check:

- **There is no `Ctrl+S` handler anywhere in the editor.** `MenuItem("Save Level", "Ctrl+S")` passes a
  *display string*; it binds nothing. Building the shortcut path — including the level's — is new work.
- **A `.cs` file cannot reach the host at all.** Double-clicking one tests `cbIsSourceFile`, opens the
  IDE, and *returns* before the editor-open call. The actor editor's own file type is short-circuited.
- **The exit-confirm is not implementable as stated.** `Window` exposes `shouldClose()` and
  `requestClose()` and nothing that clears or vetoes it; `WM_CLOSE` calls `requestClose()`
  unconditionally. "Cancel" needs a cancellable close in the platform layer.

### Piece 2 — `averdesign`

Roslyn is **MIT, already in the local NuGet cache, and ships inside the SDK** — so it restores
offline. That removes the only practical objection.

But one Roslyn argument in the first draft was wrong and is withdrawn: the generated region is
delimited by `// <aver-generated region="models" schema="1">` comments, **not** `#region`. Roslyn
models `#region` as directive trivia with matching and nesting; a line comment is just
`SingleLineCommentTrivia` — a span and nothing else. For *locating the region*, Roslyn buys
approximately nothing over a scanner.

What it does buy, for actors specifically:

- **Named and optional arguments.** `Place(0x…UL, "…", material: "M_Rubber", pos: (120f, 80f, 20f))`
  — a scanner matching positionally breaks the moment somebody reorders or omits one. (Note the
  parameter is `modelId`, not `objectId`; a matcher built from the wrong spelling silently misses.)
- **Tuple literals with nested parens**, which brace-counting gets wrong.
- **Trivia-preserving replacement**, so a rewrite is a span swap rather than a reconstruction.
- **Real refusal**: a body it cannot model is a diagnostic, not a mangled file.

Materials did not need this — a flat list of scalars in one fluent chain is honestly hand-scannable,
and it is tested. Actors are structured, and that is where the hand-rolled approach stops being
honest.

**One inherited-code trap:** `MaterialScript.cpp`'s float formatter walks `%.1g`…`%.9g`, so values
outside `%g`'s fixed range come back in exponent form (`1e+07f`). `docs/DESIGNER_REWRITE.md` fixes the
numeric token as `-?[0-9]+(\.[0-9]+)?f` — no exponent. That formatter must be *written*, not reused.
(It is fine where it is: C# accepts exponent literals, and `.ocmat` is not governed by that grammar.)

### Piece 3 — the preview viewport

See §3. This is the piece that decides the shape of everything else.

### Piece 4 — live sync

See §4. Shipped as the **Live** toggle described in §4b.

---

## 3. The viewport decision

There is exactly one 3D view in the process and it is the backbuffer.

**Option A — a second scissored rect in the main pass. Rejected, and it is worth knowing why it
*compiles*.** `setCamera` writes only the CPU-side frame struct; that struct is memcpy'd into the
frame's upload buffer once, at the top of `beginFrame`, and bound as a single root CBV. Called after
`beginFrame` it is a **no-op for the current frame**. So the naive two-viewport implementation draws
both rects with the *previous frame's* camera, and presents as a matrix-maths bug.

Even fixed: depth is one buffer cleared once full-surface, so overlapping rects occlude each other;
bloom is a full-image pyramid with no notion of a rect; and exposure is a single histogram over the
whole target reducing to one scalar. That last is decisive — **two viewports cannot have different
exposure**, and because adaptation is temporal, opening an actor tab would make the *level viewport*
visibly ramp brightness for about a second. That reads as a renderer bug in the level.

**Option B — extend the device to N views. Rejected.** Weeks of work in the one subsystem whose
regressions this project cannot cheaply adjudicate, because the renderer is not bit-deterministic and
the pixel oracle therefore cannot settle a disagreement.

**Option C — draw on the tonemapped backbuffer in `overlayPass`. Closed.** That hook binds no depth
by contract, so even a single cube self-occludes wrongly, and the backbuffer is not a `TextureHandle`
a module can pair with its own depth.

**Option D — a dedicated preview `IRenderFeature`. Chosen.** It owns a colour texture and a depth
texture, its own PSO at sample count 1, its own shaders, tonemapping in its own pixel shader, handed
to ImGui via `uiTextureId`. Structurally this is Voxi's shadow pass with a colour target added.

> **Corrected from the first draft.** That draft had the preview publish its camera at **b0** via
> `setConstantBuffer(0, …)`. That breaks the RHI contract — slot 0 is reserved for the engine's
> per-frame block, and `setPipeline` re-binds it on *every* pipeline change, so the override would
> have to be re-issued after each one. The cited precedent does the opposite: Voxi publishes at
> `kFeatureFrameConstantRegister` (**b4**). The preview must do the same — **and therefore cannot use
> `averSkyAbove`, which reads the sky fields out of b0.** It writes its own sky. This is a different
> shader from the one the draft costed.

**What it draws is not entities** — a flat list of `(meshHandle, materialSlot, worldMatrix)` from the
parsed model rows. No spawn, no world, no bridge, no play state. Spawning a live instance would need a
preview-world isolation the bridge cannot give: its state is process-global statics.

**What it costs, and this must be written in the panel and not only here:** the preview does not match
the level and will not. Fixed exposure, no bloom, no adaptation, no GI, no cascaded shadows, no MSAA.
A second lighting shader to maintain, which drifts when the material model changes. One live preview
at a time.

**What it forecloses:** per-viewport post parity, permanently, short of doing Option B from scratch.
And it is not reusable as a *scene* view — no picture-in-picture camera actor, no reflection probe.

---

## 4. The live-sync contract

**Viewport-edit → source.** *(built)* A drag on a gizmo handle moves the placement; Save rewrites
that row through `fmt::rewriteActorScript`, matched by ObjectId, touching only the three coordinate
tuples.

The gizmo is an **ImGui overlay projected through the preview's own camera**, not geometry in the
pass. Two reasons: the preview feature must stay drivable with no ImGui at all — that is what lets a
test be the device — and a handle has to be pickable at a constant *screen* size, which it cannot be
if it is part of a scene that scales. The axis is latched on mouse-down and held for the whole
gesture, because deciding per frame lets a drag that began on the handle become an orbit the moment
the cursor leaves it, which is exactly when a user is dragging fastest.

**Source-edit → viewport.** *(built, and not with a watcher.)* The open tab compares the file's
last-write time once per frame it is visible and re-reads when it changes. **No rebuild is required**,
because the preview reads the *parsed source* rather than a spawned actor — that is the direct
consequence of the Piece 3 decision and it is what makes this cheap.

A `DirectoryWatcher` was the planned answer and was rejected on contact. It has **zero consumers and
zero tests** in this tree, and its `poll()` returns `true` to mean *the OS dropped records, rescan
yourself* — a case nothing handles. Worse, `dotnet build` runs with its working directory inside
`Content\Scripts`, so a recursive watch covers that project's own `obj/` and `bin/` and **a build is
exactly the burst that overflows it**. One `stat` per visible tab is cheaper than a thread, an OS
handle, a filter list and an overflow path, and it cannot lose an event. If a future need is a
project-wide watch rather than a per-tab one, the watcher is still the right tool and its first test
comes with it.

**When both race**, *(built)* the reload is **refused** while the tab is dirty and says so. Silently
replacing somebody's in-progress drag with what a background tool wrote is the one behaviour a live
sync must never have. Saving, or closing without saving, resolves it. A file mid-write that fails to
parse also leaves the previous good state on screen rather than blanking the tab.

---

## 4b. The Live toggle, as shipped

The tab draws two things, and the toggle picks between them.

**Off (the default) — the PARSED view.** What the source *says*: the class's `Configure` mesh, the
designer region's placements, a character's capsule. It needs nothing loaded, works on a file that
has never compiled and in a build with no CLR, and it is what the gizmo drags — the picture and the
bytes are the same data.

**On — the LIVE view.** What the class *builds*. It resolves the class by its registry name
(`aver_fw_class_find`), spawns it with `aver_fw_spawn_preview`, walks the resulting subtree reading
`CMeshRenderer.mesh` and `aver_scene_world_matrix` off each child, and destroys it with
`aver_fw_destroy_preview` — all within the one call. Nothing is left in the world.

The two disagree exactly when `BuildModels` does something the parser cannot see — a loop, a
constant, a branch on a field — which is precisely when an author needs to look rather than guess.
Live is off by default because the parsed view is the robust one: defaulting to the fragile one
would make the tab look broken in every case where the other had something useful to show.

Four properties are load-bearing, and each is measured rather than asserted:

1. **No `OnBeginPlay`.** `aver_fw_spawn_preview` stops after `build_models`. Opening `Target.cs`
   with Live on produces no `Physics.AddStaticBox` — the log shows the ground body and nothing else,
   where a normal spawn would have added one per open. See `docs/ABI.md` for the full argument.
2. **Nothing leaks into the level.** The spawned entity's `CMeshRenderer` visible bit is cleared
   before anything can draw it, because `World::flush` retires a destroy on the *next* frame
   boundary. Measured: `scene-render: 16 spawned CMeshRenderer entities drawn`, identical with and
   without `--actor-live`.
3. **`unbind` still fires** on `aver_fw_destroy_preview`, or the editor would leak one managed object
   per Refresh.
4. **Mesh ids resolve back to paths.** `CMeshRenderer.mesh` is an `fnv1a64` ObjectId, and
   `Aver.Scene.ObjectIdOf` hashes the path *as written* rather than canonically — so the reverse
   table holds every spelling that could have produced an id: the two built-in primitives, every
   `.ocmesh` under the content root in both bare and `Content/`-prefixed form, and every `.ocmesh`
   string literal in the file itself. The hash is `aver::fnv1a64`, which `FormatTest` already pins
   against the C# side by value.

**What Live cannot show.** `AverCharacter` keeps `Height` and `Radius` as plain managed fields —
no component, no scene field — so a native walk of a spawned character sees nothing. A character
that built no models therefore falls back to the source's capsule, and the panel says so rather than
passing it off as measured. The gizmo is disabled in the live view: what is on screen there was
produced by code, and there is no byte in the file to write a new coordinate back to.

**Refresh is manual.** A Compile C# replaces the class in the registry; the tab does not currently
notice, so the button is there and its tooltip says when to press it.

Measured against SkyForge with `--open-asset <file> --actor-live`:

| File | Class Live spawned | Result |
|---|---|---|
| `Target.cs` | `BP_Target` | 1 model, `Meshes/sphere.ocmesh` |
| `Gun.cs` | `BP_GunPart` | 1 model, `Meshes/cube.ocmesh` |
| `FpsGameMode.cs` | `BP_Block` | 1 model, `Meshes/cube.ocmesh` |
| `FpsCharacter.cs` | `BP_FpsCharacter` | 0 models; capsule 180×34 cm from source |

---

## 5. Open decisions

These block a start; none is answerable from the code alone.

1. **Which mesh-path spelling is canonical.** `Sample.Game/Car.Designer.cs` passes
   `"Content/Meshes/CarBody.ocmesh"`; the engine's mesh registry is keyed on `fnv1a64` of the
   **content-relative** path without that prefix (`"Meshes/sphere.ocmesh"`). *Different hash* — so the
   one conforming sample in the tree places models that resolve to nothing and draw nothing, silently.
   This is a live latent bug, not merely a spec question.
2. **What a `.cs` double-click does now**, given it currently opens the IDE and people rely on that.
3. **How an editor window becomes a tab.** The dock layout is built once, and `DockBuilderDockWindow`
   takes a window *name* — but editor windows are named from their path and do not exist at layout
   time. "Add it to the existing DockBuilder block" is not implementable as written.
4. **Whether the exit-confirm can cancel at all** (see Piece 1).
5. **Where `averdesign` is built, staged and versioned.** It would be the repo's *first* NuGet
   consumer: no project here has a `PackageReference`, and there is no `NuGet.config`,
   `Directory.Build.props` or `.sln`. `Sample.Game` is in no CMakeLists at all.
6. **Whether `DESIGNER_REWRITE.md` is being amended.** It says of the user half: *"The editor never
   reads or writes a byte of it."* `Configure` lives there. A class-defaults panel needs to read it —
   so either the document changes or that panel does not ship.

---

## 6. What gets tested headlessly

In the style `tests/formats` already uses for the material rewriter: golden output for a known
`.Designer.cs`; rewrite-own-output byte equality; a neighbouring model row left untouched; refusal
cases that leave the file unmodified; a tuple literal with nested parens; a named argument out of
order. The preview feature gets the `tests/render.ui` treatment — a recording context asserting the
draw list, with no GPU.

The watcher needs its **first** test, including the overflow return. It has never had one.
