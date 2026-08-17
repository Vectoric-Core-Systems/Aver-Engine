# First Person

You created this project from Aver Engine's **First Person** template. Everything it does lives in
three graphs and one map — there is no C# anywhere in this project, and no imported art: every mesh
and material below is one of the engine's own built-in primitives. Open any `.ocgraph` file in a text
editor (or the Aver Node graph editor) to see exactly how it works; the comments inside each one are
part of what this template is teaching, so read those too, not just this file.

## What each file does

- **`Scripts/AN_FPCharacter.ocgraph`** — the player. `CLASS AN_FPCharacter Character` parents it to
  the framework's real, concrete character type, so it gets a capsule, a view, and mouse-look for
  free. `view=firstperson` on that same `CLASS` line is what makes the camera sit at the character's
  eyes instead of behind it — see the graph's own comment for why that attribute had to be added for
  this template to be honestly named "First Person" at all. `OnStart` spawns the two targets below and
  remembers their entity ids; `OnTick` reads `MoveAxis`/`MouseDelta` into `CharacterMove`, then checks
  the planar distance to each remembered target and fires `OnHit` at whichever one you're standing
  close to.
- **`Scripts/AN_FPTarget.ocgraph`** — the targets. `CLASS AN_FPTarget Actor mesh=Meshes/sphere.ocmesh
  material=M_Target` gives every instance a visible, red-ish sphere, scaled up from the built-in
  primitive's native 1cm radius by its own `OnStart`. A `VAR score` counts the hits this *specific*
  instance has received — two instances of this one file keep two independent counts, which is the
  whole point of `VAR` living on the spawned instance rather than the graph file.
- **`Scripts/AN_FPRules.ocgraph`** — the game mode. `CLASS AN_FPRules GameMode` is what actually
  starts the play session; without a declared `GameMode` class, nothing here would ever tick at all.
  Keeps a simple elapsed-time clock in a `VAR`, logged every tick, as its own small proof that a
  graph's state survives from one tick to the next.
- **`Maps/Default.ocmap`** — a flat ground slab, two decorative crates, and two `class` placements
  (the game mode, the player). The targets are **not** placed here — see "What this deliberately does
  not do yet" below for why they can't be.

## What to change first

- **Where the targets spawn.** `AN_FPCharacter.ocgraph`'s `OnStart` chain has two `Spawn` nodes, each
  fed by three `ConstFloat` nodes (`ax`/`ay`/`az` and `bx`/`by`/`bz`) naming a world-space x/y/z. Move
  a target by editing those three numbers.
- **How close counts as "in range."** The same graph's `rangeSq` node holds a squared distance
  (48400 = 220cm, squared, because the comparison is done in squared units to avoid a square root).
  Bigger number, more forgiving range.
- **What a target looks like.** `AN_FPTarget.ocgraph`'s `CLASS` line names `Meshes/sphere.ocmesh` and
  `M_Target`; swap either for any other built-in primitive/material (see `GameContent.cpp`'s
  `registerBuiltins` for the full list: `Meshes/cube.ocmesh`, and materials `M_Floor`/`M_Wall`/
  `M_Trim`/`M_Crate`/`M_Target`/`M_Metal`/`M_Accent`) — or your own project asset, once you have one.
- **Move speed, turn speed, jump height.** These belong to `AverCharacter` itself
  (`Aver.Framework/Character.cs`), not to the graph — `MoveSpeed`, `TurnSpeed`, `JumpSpeed`, `Height`,
  `Radius`. A graph-parented character uses that C# type's own field defaults; there is no node that
  overrides them per-instance today (the same gap `view=` on the `CLASS` line closed for the camera
  mode specifically — see the character graph's own comment).

## What this deliberately does not do yet

- **No raycast-based aiming.** `Raycast`'s `entity` output pin is a **physics body handle**, and there
  is still no body-to-entity mapping anywhere in the engine — a graph can learn *that* it hit
  something, not *what*. Aiming at a target and shooting it, the way a real first-person shooter
  would, needs that gap closed first. This template substitutes **proximity**: walk up to a target and
  it counts as a hit, every tick, for as long as you're standing close enough. That is an honest
  stand-in, not a placeholder pretending to be the real thing — if raycast hit identification lands in
  a later engine version, replacing the distance check with a real `Raycast` + `Branch` is a
  same-sized change to the character graph, not a redesign.
- **No combined score.** Each target's own `score` VAR is genuinely *its own* count, not a slice of
  some project-wide total, and there is no single number anywhere that adds them together. Two real
  gaps stand in the way of one: `FireEvent` carries no payload at all (the receiving `ENTRY` gets only
  `entity`/`time`/`deltaTime` — nothing an amount or a source could ride on), and the game mode has no
  way to learn a target's entity id in the first place, because it is *placed* by the map rather than
  *spawned* by anything, and spawn-and-remember is the only id-discovery mechanism this vocabulary
  has. If you want one combined total, the straightforward path is reading both targets' `score`
  fields from outside the graph layer (a C# `AverHud`, or a future inspector), not from another graph.
- **No fire button.** There is no `InputKey` node gating the hit test — proximity alone triggers it.
  This is deliberate, not an oversight: a headless capture run of a shipped game presses no key and
  moves no mouse (`AverGame.exe --headless` logs "no window, no input", literally), so a mechanic
  gated behind a keypress would be *unprovable* by the exact kind of automated run this template was
  proven with. Proximity-only means the mechanic runs and its VARs move whether or not a human is
  attached. Add an `InputKey` + `Branch` for Mouse-left (key code 47 — see
  `Aver.Framework/Input.cs`'s `Key` enum; there is no symbolic lookup in the format, only the raw
  number) if you want a real fire button back once a human is playing.
- **No HUD.** Nothing in a shipped game draws 2D UI on its own — there is no `Aver.UI`-reaching node
  in this graph vocabulary at all. The score lives in `AN_FPTarget`'s own `VAR` and is visible in the
  log (`[GraphHost] 'FPTarget' entity <id>: OnTick -> <score>`, once per tick, automatically) but
  nothing draws it on screen. A real HUD is a C# `[AverHud]` reading the same field this project's
  Content already has no C# path to reach — that is real project-specific work, not something this
  template could do generically.
- **Two `FPRules` instances tick, not one.** `Default.ocmap` places exactly one `class AN_FPRules`,
  but `aver_fw_begin_play` also spawns its own fresh GameMode instance automatically the moment play
  starts, with no awareness that the level already placed one. You will see two identical `elapsed`
  clocks in the log, ticking in lockstep. Harmless — neither leaks into the other's state — but real,
  and worth knowing before you assume you miscounted a placement.
