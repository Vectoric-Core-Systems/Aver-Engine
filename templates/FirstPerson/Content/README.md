# First Person — an Aver Node template

A playable first-person character, three shootable targets and a game mode. **No C# anywhere** — run
`find . -name '*.cs'` in this project and expect nothing back. Everything here is six text files:
five `.ocgraph` graphs and one `.ocmap`.

## Controls

| Input | Does |
|---|---|
| **W A S D** | walk |
| **Mouse** | look (first person, because `AN_FPCharacter`'s CLASS line carries `view=firstperson`) |
| **Mouse Left** | shoot |
| **Space** | jump |

Both are ordinary key codes to `InputKey`: Space is `Key.Space = 36` and Mouse Left is
`Key.MouseLeft = 47`, because the mouse buttons sit at the end of the same enum in
`Aver.Framework/Input.cs`. Neither needed a node of its own.

Three targets sit ahead of you along +X, which is the direction a character faces on spawn. Click
as soon as the level opens and you will hit the middle one.

## The files

| File | What it is |
|---|---|
| `Scripts/AN_FPCharacter.ocgraph` | the player: walks, looks, and shoots what it is looking at |
| `Scripts/AN_FPTarget.ocgraph` | a target: scales itself up, counts hits as its own score |
| `Scripts/AN_FPRules.ocgraph` | the game mode: names the pawn and controller, keeps a round clock |
| `Scripts/AN_FPController.ocgraph` | the player controller. Empty on purpose — see below |
| `Scripts/AN_FPGun.ocgraph` | the viewmodel: a CC0 blaster mesh, parented to the camera. No logic, does not tick |
| `Maps/Default.ocmap` | floor, two crates, three targets. **Not** the player |

## Who spawns the player

The map does not place the player. `AN_FPRules` names it:

    CLASS AN_FPRules GameMode pawn=AN_FPCharacter controller=AN_FPController

`aver_fw_begin_play` spawns both and **possesses** the pawn, the same division Unreal's
`DefaultPawnClass` uses. Possession is not a detail — the game camera follows
`aver_fw_controlled_pawn()` and gives up when there is none, so a character that is merely *placed*
renders the whole level from a default camera no matter what `view=firstperson` says.

Both attributes are required. `begin_play` possesses only when it has a controller **and** a pawn, and
the engine's built-in `PlayerController` is abstract, so it cannot be spawned as a fallback. That is
the only reason `AN_FPController.ocgraph` exists: `ABSTRACT` is not inherited, so a graph parented to
`PlayerController` is a concrete, spawnable controller — the same thing a Blueprint subclass of
`APlayerController` is. Name a pawn with no controller and the engine will warn you that the pawn is
spawned and never possessed.

Each graph carries its own reasoning inline. Read `AN_FPCharacter.ocgraph` first — the shooting chain
is the interesting part, and its comments explain each decision at the point it was made.

## How the shooting works

    OnTick
      -> MoveAxis -> MouseDelta -> CharacterMove          walk and look
      -> SetVar cooldown (tick it down by deltaTime)      rate limit
      -> Branch (InputKey Space held?) -> Jump            both sides continue below
           -> Branch (InputKey MouseLeft held?)
                -> Branch (cooldown expired?)             0.35s between shots
                     -> SetVar cooldown = 0.35
                     -> Raycast, fired from GetForward    aim
                          -> Branch (did it hit?)
                               -> FireEvent OnHit at Raycast.entity   score

`GetForward` returns the character's real look direction **and** its eye position, read from
`AverCharacter` itself rather than rebuilt in the graph — so the shot always agrees with the camera,
including the pitch clamp that `Drive` applies.

The ray does **not** start at the eye. It starts one muzzle-length along the look direction, because
the eye sits inside the character's own capsule and a ray starting there hits the shooter first. This
is not hypothetical: the first version of this graph shot itself on every single tick, and the only
reason it was noticeable is that `FPCharacter` has no `OnHit` chain to receive the event. See the
graph's own comment on `NODE muzzle`.

`FireEvent` is gated on `Raycast.hit`, never on the entity alone. Entity `0` is not a miss sentinel —
it is a **real hit against collision no entity owns**, like the floor or a crate. Shoot a crate and
you get a genuine hit that simply scores nothing, which is worth seeing at least once.

## What this template deliberately does not do

Named rather than quietly omitted:

- **Firing is level-triggered, not edge-triggered.** Holding Mouse Left keeps firing; nothing here
  detects the press itself. What stops it being one shot per tick is a plain rate limit — a
  `cooldown` VAR set to 0.35 and counted down by `deltaTime` — so held fire is about three shots a
  second rather than sixty. That is a weapon fire rate, not edge detection, and the difference
  shows the moment you want a single-shot weapon or a charge-up. This template still doesn't use
  it, but as of `160bcdd` (2026-09-01) a node does reach `aver_fw_input_key_pressed`: `InputKeyPressed`
  (and its twin `InputKeyReleased`) give a `triggered` pin true for exactly one frame per press, so a
  real once-per-press weapon no longer needs a VAR remembering last frame's state to get edge
  detection out of `InputKey`'s level-triggered `down`.
- **There is no combined score.** Each target owns its own `score`. `FireEvent` carries no payload,
  and nothing hands a graph a list of entities by class, so a single total would have to be faked.
- **Targets are `Character`-parented, which looks odd.** A `class=` placement gets no physics body
  from anywhere in this engine, so a plain `Actor` target would be invisible to the very ray meant to
  hit it. `Character` is the one class whose graph-facing node reaches a real Jolt body.
- **The visible sphere and the collider are different shapes.** You hit a person-sized capsule
  spanning feet→feet+180; you see a sphere centred on the feet. They cannot share a centre without a
  child transform, so the sphere is sized (90) to overlap the capsule at the player's eye height —
  which is why the crosshair lands inside the ball rather than above it. Aim well above or below a
  ball and you can still hit its capsule.
- **Targets hang where the map puts them.** A `CharacterVirtual` is integrated only when something
  drives it, and these are driven once at `OnStart`, so they do not fall. Convenient on a range, but a
  consequence of not ticking them rather than anything asking them to hover.
- **The game mode ticks twice.** `aver_fw_begin_play` spawns a fresh instance of the GameMode class on
  top of the one the map already placed. Harmless, and explained in `AN_FPRules.ocgraph`.

## Verified

Run headlessly against a build of this engine, with the FIRE gate bypassed so the chain runs without
input (headless has none at all — `[Game] headless: no window, no input`). Each target's `OnTick`
reports its own `score` through the graph's `OUT`, so the scores are read straight from the log:

- the **centre** target scored, and both **side** targets stayed at **0** for the whole run;
- 40,000 headless frames advanced the game clock only 0.095 s, so the centre target's count stays
  in single figures however long the run is.

**The zeroes are the result, not the total.** They show the ray hits what it is aimed at rather
than anything near it, and that each target's score is genuinely its own. The centre count is not
a throughput measure and should not be read as one: firing is gated on a 0.35 s cooldown counted
in `deltaTime`, and headless frames are far shorter than rendered ones, so the number of shots a
run fits depends on simulated seconds and not on the frame count at all. An earlier version of
this file reported 89 hits in 90 frames, one per tick — that predates the cooldown and cannot
happen now.
