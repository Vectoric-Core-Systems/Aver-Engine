# Actor Editor with Live Sync

> **Status: all four pieces are built.** The preview, the designer reader and rewriter, the tab, the
> translate gizmo, source-to-view reload, the Live view (§4b), the file watcher (§4c) and the Roslyn
> backend `averdesign` (§4d) all exist and are in the build.
> What is NOT built: rotate/scale gizmo handles, and the tab infrastructure fixes in §2 Piece 1.
> Sections below marked *(built)* describe what shipped; the rest is still plan.


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
| The Components tree, and the camera/light wireframes | `sandbox/src/ActorEditor.cpp` | not covered by a test; verified by screenshot against `Car.Designer.cs` and `FpsCharacter.cs` |
| A resizable, non-square preview target | `modules/render.actorpreview/` | `ActorPreviewTest` — drain/create/destroy order, id re-fetch, idempotence, zero-extent refusal |
| The file watcher, and routing a disk change to a tab | `modules/platform/…/DirectoryWatcher`, `sandbox/src/AssetEditor.cpp` | `WatcherTest`, 33 assertions against a real filesystem |
| The **Roslyn** backend | `scripting/csharp/Aver.Design/` → `bin/Tools/averdesign.exe`; `modules/formats.roslyn/` | `RoslynTest` — agreement with the scanner, and the cases it declines |

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

## 4a. The layout, and the Components panel

**Three columns, the way UE lays a Blueprint editor out:** Components on the left, the viewport in
the middle with everything left over, Details on the right. It was two — viewport, then one column
carrying the class picker, the class defaults, the component tree and the transform editor — so the
tree had to be kept short to leave the others room, and the viewport was squeezed by a column doing
three unrelated jobs.

Below the width where the viewport would be squeezed under ~260 units the left column folds back
into the right one and the tab is two columns again. The test is phrased as *"is there still room
for a viewport"* rather than as a ratio between the side panels — phrased as a ratio it never
reached three columns at 300% DPI at all, because the panels scale with DPI and a ratio between them
ignores how much room there actually is.

**The viewport fills its column.** The preview target used to be square and fixed at creation, so a
wide panel letterboxed — most of a wide monitor's viewport spent on nothing. `ActorPreview::resize`
now matches the target to the panel, and the projection's aspect follows the target (it was
hard-coded to 1, which against a wide target stretches every actor horizontally and reads as a
modelling mistake rather than a projection one).

Resizing destroys a texture the UI is sampling, which needs a `waitIdle` — a whole-GPU stall. The
editor therefore **debounces**: a size must hold still for 250 ms, and must differ by more than 24
pixels, before a resize is asked for. Without the deadband a layout that oscillates by a pixel
between frames — a scrollbar appearing and disappearing — would resize forever. That costs one stall
per resize gesture instead of one per frame.

`ActorPreviewTest` pins the order, because every hazard here is a use-after-free no validation layer
catches: drain first, create the new pair, destroy the old, **re-fetch the UI texture id** (a new
texture is a new descriptor; keeping the old id reintroduces the exact bug the drain prevents), and
do nothing at all when the size is unchanged.

### The Components panel

UE's Blueprint editor shows an actor as a **tree**, not a list, and this now does the same. An actor
is a root with a transform and a set of things attached to it, some of which draw and some of which
do not.

What replaced what: the panel used to be a flat list of `b.Place` rows. That meant a class-level
mesh, a character's capsule, a camera and a light — three of the four things an actor can be made
of — had **no row at all**. A class whose only component was a camera showed an empty panel over an
empty viewport.

The tree is built to one shape from either source, which is what lets Live be a toggle rather than a
second editor:

- **Parsed** — the root, then the class's own declarations (`b.Mesh`, capsule, camera, light), then
  every `b.Place` row.
- **Live** — the root, then the spawned subtree, whose world matrices are read off real entities, so
  nesting that `BuildModels` created survives.

Rows are colour-coded by kind, because in a list of twenty the eye finds "the light" by colour long
before it finds it by name. Selecting a row highlights that component in the viewport and, when the
row came from a `b.Place`, drives the gizmo and the transform editor — the tree index and the model
index are **derived** from one another rather than kept in step by hand.

**Components with no geometry are drawn as wireframes**, projected through the preview's own camera:
a camera as a frustum pointing down +X at a fixed 60 cm (a real far plane is tens of metres and
would fill the preview with lines that say nothing about where the camera is), and a point light as
three orthogonal circles at its range — one circle reads as a disc and hides which plane it is in.

**A stored vector with child indices**, not owned children or pointers: nodes are appended during a
walk, and a vector that reallocates invalidates every pointer taken so far. That is the standard way
this shape gets written and then quietly broken by the first actor with enough parts.

**What it will not show, and should not.** An actor that assembles itself in `OnBeginPlay` — SkyForge's
`Gun` builds its five boxes in `AttachTo()`, from gameplay — appears as its class-level mesh only.
That is not a gap: the preview runs the construction and stops (§4b), and UE's Blueprint viewport
does not run BeginPlay either. Measured against a real designer file, the five-placement `Car`
renders assembled at its authored ±120/±80 cm offsets with all five rows in the tree.

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

## 4c. The file watcher, as shipped

`DirectoryWatcher` is started on the project's **Content** root when a project opens, pumped once a
frame *before* the tabs draw, and routed to whichever editor owns the changed path.

It does not replace the two polls that were already there; it covers what neither can. The tab's own
per-frame `stat` only sees the tab being **drawn**, so a background tab lagged until it was clicked.
The Compile button's half-second directory walk colours one button and would have to cover the whole
content tree at that rate to be a change signal.

The notification **bypasses** the mtime stamp rather than feeding it: a safe save can leave a
modification time the tab has already seen, and the stamp then says nothing happened about a file
whose bytes are entirely different.

`WatcherTest` is new and found a real bug on its first run: `start()` opened the directory handle,
spawned the worker and returned, but the kernel only records changes while a `ReadDirectoryChangesW`
is outstanding — and that call happens on the worker thread. The first file written after `start()`
was silently dropped. `start()` now waits for an event the worker sets once its first read is in
flight.

### Auto-compile on save

**Tools > Auto-compile on Save**, off by default, or `--auto-compile`. With it on, a `.cs` change
anywhere under Content rebuilds and reloads the project's scripts, which closes the loop: save in
Visual Studio, and the Live view updates with nothing pressed.

Two things it must get right, and both are measured:

- **One build per burst, not one per file.** A second debounce sits on top of the watcher's. The
  watcher's 150 ms settle coalesces the burst *one* save produces into one event per path; this
  coalesces events across *many* paths into one build, because a Save All or a branch switch touches
  several files and sequential `dotnet build` runs each lock the script assembly. Measured: two saves
  200 ms apart produced `auto-compile: 2 script change(s) settled` — once — and one build.
- **It must not compile in a loop.** MSBuild regenerates `Scripts.AssemblyInfo.cs`,
  `Scripts.GlobalUsings.g.cs` and `.NETCoreApp,Version=v10.0.AssemblyAttributes.cs` under `obj/` on
  **every** build, and those are `.cs` files inside the watched tree. Any path with a `bin` or `obj`
  segment is therefore ignored — matched on whole segments, so `Scripts/Robots/BinPacker.cs` is
  safe. Measured: no second trigger in the 30 s after a build.

It takes the same path as the Reload Scripts button rather than a quieter private one, so the reload
that bumps the Live views' generation happens here too. A build already running holds the deadline
rather than being dropped: the last edit is the one being waited on.

## 4d. The Roslyn backend, as shipped

`averdesign` (`scripting/csharp/Aver.Design/`, staged to `bin/Tools/`) parses a `.cs` with
`Microsoft.CodeAnalysis.CSharp` and prints what it declares as JSON. It is the repo's **only** NuGet
consumer; Roslyn is MIT and ships inside the .NET SDK, so the restore resolves from the machine's
package cache with no network.

**It runs only on `Malformed`.** That status means the text has left the locked grammar rather than
that the text is wrong. A file the scanner reads is never re-read by the slower parser, and a file
with no region has nothing for either to read.

**A separate process, not a hosted library.** The alternative is loading a compiler into a process
whose job is to draw frames, which the editor's collectible load context would then have to keep
clear of on every script reload.

**A separate module, not part of `Aver.Formats`.** The base module reads bytes and depends on
nothing, which is what lets a test link it alone and what lets the scanner run on a machine with no
.NET. `Aver.Formats.Roslyn` sits above that line; a build without it simply has no escalation.

Two things are easy to get wrong here and both are pinned by `RoslynTest`:

- **Byte offsets, not character offsets.** Roslyn counts UTF-16 chars; the C++ side slices UTF-8
  bytes. They agree only while the file is pure ASCII, and one accented letter in a comment above a
  placement shifts every subsequent span — so a rewrite lands in the middle of another token.
  `averdesign` converts every offset through a prefix table before it leaves.
- **The object id crosses as a string.** It is a full 64 bits and a JSON number is a double, so a
  numeric round trip would silently round the one field every rewrite is matched by.

Measured end to end: a `Car.Designer.cs` with its named arguments reordered — which the scanner
declines — opens in the editor with all five placements, logging
`Rig.Designer.cs left the locked grammar; Roslyn read it: 5 placement(s)`.

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
