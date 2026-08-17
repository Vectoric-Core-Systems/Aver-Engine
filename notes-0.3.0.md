**Aver Engine 0.3.0 Beta.** Pre-1.0, and every release is a beta.

Install through the Aver Launcher, which reads this release directly, or take the zip below to unpack by hand.

---

The release where Aver Node stops being a demo. A graph can now aim, remember, possess and jump — the
things a first-person game is made of — and the node editor can finally open the graphs people
actually write.

### Aver Node

- **Variables are first class in the editor.** `VAR` records are modelled end to end: a Variables
  panel declares, renames, retypes and deletes them, and `GetVar`/`SetVar` pick from a list instead of
  a free-text field. Renaming offers to rewrite every node addressing the old name rather than
  silently orphaning them. Previously, dragging `SetVar` out of the palette produced a graph that
  could not compile — validation rejects an undeclared variable, and nothing in the editor could
  declare one.
- **`GetForward`** — a character's real look direction *and* eye position. `AverCharacter` keeps its
  yaw and pitch private and writes a Quat to the scene, which neither field reader can see, so a graph
  could drive a look and had no way to ask where it ended up pointing.
- **`GetViewEntity`** — the camera node a character looks through. A first-person viewmodel parents to
  this, not to the pawn; parented to the pawn it stands still while the camera pitches around it.
- **`Jump`** — declines in mid-air on its own, so wiring it straight to a key gives single jumps and no
  flight. Reports whether the jump actually happened, which "the key was pressed" is not.
- **`Raycast` reports the scene entity it hit**, not a raw physics body handle. A graph could learn
  *that* it hit something and never *what*; spawn-and-remember was the only way to know, and it is no
  longer needed.
- **`CLASS … GameMode pawn= controller=`** — a graph game mode names its pawn and controller, and
  begin-play spawns and **possesses** them. Without possession the camera follows nothing, so a
  graph-only project rendered from a default camera however loudly its character asked for first
  person. Both attributes are required together, and naming only one now warns.
- **`CLASS … view=firstperson|thirdperson`** on a Character-parented class.
- **A missing input pin is refused by name.** One hand-written `PIN` record suppresses a node's
  defaults, so omitting one left the emitter asking for a pin that no longer existed — and both
  compilers produced IL the runtime refused, reported as "Common Language Runtime detected an invalid
  program", naming neither the node nor the pin.
- **`ConstFloat value=100`** compiles to 100.0, not 0. Literals are now parsed by the node's declared
  type rather than by the shape of the text.

### Node editor

- **It can open real graphs.** `ENTRY` and `OUT` may name a node declared later in the file — the
  style every graph here uses. The C++ reader validated that inline while the C# reader deferred, so
  the editor could not open the sample projects, the demo, or any template graph.
- **Saving keeps every record where its author put it.** Records are matched individually rather than
  a whole kind being re-emitted at that kind's first line, which used to haul every `NODE` up to the
  first one and separate it from the comment explaining it. Line endings are preserved too.
- **Panning by right-drag**, alongside the existing Space-drag and middle-drag, with click and drag
  separated by a movement threshold so right-click still opens Add Node.

### Rendering

- **Ray-traced sun shadows are on by default**, at Medium, **with no denoiser**: every pixel traces
  every frame — no tiling, no reprojected history, no temporal blend. A denoiser hides its own
  artefacts as readily as the tracer's, which is the wrong default for a renderer being evaluated.
  The two knobs behind the tier now derive from it, so Medium cannot silently run Epic's cost.
- **Lighting no longer trails the camera.** Both halves of that had the same shape — a cheaper rung
  reusing last frame's work — and both were on the default tier. Shadows reused a reprojected history
  sample for three pixels in four, and the GI volume was re-voxelised only every fourth frame, so
  indirect light lagged movement by up to three. Medium now traces every pixel and re-voxelises every
  frame; measured, neither costs anything on the scene it was benchmarked against, and Low keeps both
  amortisations for anyone who wants the frame time back.
- **`--no-rt`** reaches the Off rung from the command line. `--rt` cannot express it — `0` is that
  override's "not given" — so the moment ray tracing became the default, the baseline row of the
  published cost ladder stopped being reproducible by the flags that produced the other rows.
- **Path tracing can be switched on from the editor.** The Project Settings control existed but was
  wired to a capability query that answered "not implemented" unconditionally, leaving it permanently
  greyed and the setting clamped off on every device.
- **The path-traced reference view is lit by the sun**, not only the sky, so an interior no longer
  renders honestly black.
- **DXR no longer depends on a window.** Acceleration structures initialised from swapchain creation,
  so every headless run reported no ray-tracing support directly beneath a capability line saying
  tier 11.

### Projects and the editor

- **Saving a level no longer deletes its graph classes.** Placements were rebuilt from scene
  components, which a `class=` placement does not have — so opening a level and saving it removed
  every graph class in it. For a graph-only project that is the entire game.
- **A project with no C# is no longer told it needs a `.csproj`.**
- **A project that failed halfway through creation can be created again.** The scaffolder refused an
  existing folder and then created one, so any failure after the first directory left a folder behind
  and made that project name permanently unusable.
- **New Project can start from a template**, with a `.octemplate` manifest format; scaffolding copies
  `Content/` byte for byte and never rewrites class or graph names.
- **The engine ships one: First Person.** A playable character with a CC0 viewmodel, three scorable
  targets, a game mode and a controller — five Aver Node graphs, one map, and **no C# anywhere**. It
  is the demonstration that the vocabulary above is enough to build with, not just to read about; a
  project made from it contains no `.cs` file and none is generated for it.
- **A 0.2 project opens in 0.3.** Nothing on disk needs repairing — every project-visible change this
  release is additive — but the upgrade chain reports a missing series rather than skipping it, so a
  0.1-era project would otherwise have failed to open at all.

### Removed

- **`AverGame.exe` and the packaged-game path**, including the staging and verification scripts and
  the editor's Package Project item. A second host that rendered a different subset of the scene was a
  standing source of confusion about what "the game" shows. The editor is how a project runs; the
  runtime library survives, driven by its tests.
- **The old bundled starter.** It was a project that happened to live in the engine tree, with no
  manifest, no description and no way to choose it. What replaces it is not the same thing wearing a
  new name: First Person is declared by an `.octemplate`, listed in New Project, and scaffolded
  through a path that copies content verbatim.

### Known issues

- **Global illumination does not clamp what it gathers.** Enough large, saturated, brightly-lit
  geometry and the bounce runs away and floods the frame with that surface's colour. Turning ray
  tracing on makes it easier to reach, because it lights the offending geometry more brightly. Not
  hardware-specific.
- **No 2D HUD from a graph.** Nothing in the node vocabulary draws text or 2D, so a game's score can
  only be read from the Output Log.
- **Firing is level-triggered.** Holding the fire key fires every tick unless a graph implements its
  own cooldown. The edge-triggered "pressed this frame" call exists at the ABI but no node reaches it.

### Measurements

Release build, ElectricDreams, windowed at the editor's default 1600×900, `--no-vsync`, `--frames 200`,
whole-frame median, on one machine with one GPU (RX 7800 XT) — the relative ladder should hold
anywhere, the absolute milliseconds are that card's and they move with resolution:

| ray tracing | median |
|---|---|
| Off (`--no-rt`) | 11.86 ms |
| Low (1 ray, tile 4) | 18.26 ms |
| **Medium (1 ray, tile 1 — no denoiser)** | **18.44 ms** |
| High (2 rays, tile 1) | 19.99 ms |
| Epic (4 rays, tile 1) | 23.39 ms |

The three tile widths at one ray span 0.23 ms — the same number inside the run-to-run noise — while
one ray to four costs 4.95 ms. Amortisation saturates immediately, so the temporal history was buying
no frame time in exchange for the latency it introduced. Re-voxelising GI every frame rather than
every fourth is free on the same scene: intervals 1, 2, 4 and 8 measure 18.54, 18.47, 18.50 and
18.46 ms.

Reproduce with:

```bash
build-release\bin\Sandbox.exe <path>\ElectricDreams.ocproject --frames 200 --no-vsync --frame-time --rt-rays 1 --rt-pixels-per-ray 1
```

The `.ocproject` path is **positional** — there is no `--project` flag — and a project whose
`CREATEDWITH` names an older series stops at a modal. Either mistake leaves the run scoring an empty
editor at plausible-looking milliseconds, so check the log says `scene walk … over 14 entities` before
believing a number.
