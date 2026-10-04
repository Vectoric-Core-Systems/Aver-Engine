# First Person — an Aver Node template

A playable first-person character, three shootable targets and a game mode. **No C# anywhere** — run
`find . -name '*.cs'` in this project and expect nothing back. Everything is six text files:
five `.ocgraph` graphs and one `.ocmap`.

## Controls

| Input | Does |
|---|---|
| **W A S D** | walk |
| **Mouse** | look (first person) |
| **Mouse Left** | shoot |
| **Space** | jump |

Both key codes are from `Aver.Framework/Input.cs`: Space is `Key.Space = 36` and Mouse Left is
`Key.MouseLeft = 47`.

Three targets sit ahead of you along +X (the direction a character faces on spawn). Click
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

`aver_fw_begin_play` spawns both and **possesses** the pawn. Possession is not a detail — the game
camera follows `aver_fw_controlled_pawn()` and gives up when there is none. A character that is merely
*placed* renders from a default camera no matter what `view=firstperson` says.

Both attributes are required. `begin_play` possesses only when it has a controller **and** a pawn, and
the engine's built-in `PlayerController` is abstract, so it cannot be spawned as a fallback. That is
the only reason `AN_FPController.ocgraph` exists: `ABSTRACT` is not inherited, so a graph parented to
`PlayerController` is a concrete, spawnable controller.

Each graph carries its own reasoning inline. Read `AN_FPCharacter.ocgraph` first — the shooting chain
is the interesting part, and its comments explain each decision.

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
the eye sits inside the character's capsule and a ray starting there hits the shooter first. The first
version of this graph shot itself on every tick, and was only noticeable because `FPCharacter` has no
`OnHit` chain. See the graph's own comment on `NODE muzzle`.

`FireEvent` is gated on `Raycast.hit`, never on the entity alone. Entity `0` is not a miss sentinel —
it is a **real hit against collision no entity owns**, like the floor or a crate. Shoot a crate and
you get a genuine hit that simply scores nothing.

## What this template deliberately does not do

Named rather than quietly omitted:

- **Firing is level-triggered, not edge-triggered.** Holding Mouse Left keeps firing. A plain rate
  limit — a `cooldown` VAR set to 0.35 — counts down by `deltaTime`, so held fire is about three shots
  a second. That is a weapon fire rate, not edge detection. As of `160bcdd` (2026-09-01), a node
  (`InputKeyPressed` / `InputKeyReleased`) gives a `triggered` pin true for exactly one frame per press,
  so a real once-per-press weapon no longer needs a VAR to get edge detection.
- **There is no combined score.** Each target owns its `score`. `FireEvent` carries no payload, and
  nothing hands a graph a list of entities by class.
- **Targets are `Character`-parented, which looks odd.** A `class=` placement gets no physics body from
  anywhere in this engine, so a plain `Actor` target would be invisible to the ray. `Character` is the
  one class whose graph-facing node reaches a real Jolt body.
- **The visible sphere and collider are different shapes.** You hit a person-sized capsule spanning
  feet→feet+180; you see a sphere centred on the feet. The sphere is sized (90) to overlap the capsule
  at the player's eye height — which is why the crosshair lands inside the ball.
- **Targets hang where the map puts them.** A `CharacterVirtual` is integrated only when something
  drives it. These are driven once at `OnStart`, so they do not fall.
- **The game mode ticks twice.** `aver_fw_begin_play` spawns a fresh instance of the GameMode class on
  top of the one the map already placed. Harmless, and explained in `AN_FPRules.ocgraph`.

## Verified

Run headlessly against a build of this engine, with the FIRE gate bypassed. Each target's `OnTick`
reports its `score` through the graph's `OUT`:

- the **centre** target scored, both **side** targets stayed at **0**;
- 40,000 headless frames advanced the game clock only 0.095 s, so hits stay in single figures.

The zeroes are the result, not the total. They show the ray hits what it is aimed at rather
than anything near it, and that each target's score is genuinely its own.
