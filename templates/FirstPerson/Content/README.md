# First Person — an Aver Node template

A playable first-person character, three shootable targets and a game mode. **No C# anywhere** — run
`find . -name '*.cs'` in this project and expect nothing back. Everything here is four text files:
three `.ocgraph` graphs and one `.ocmap`.

## Controls

| Input | Does |
|---|---|
| **W A S D** | walk |
| **Mouse** | look (first person, because `AN_FPCharacter`'s CLASS line carries `view=firstperson`) |
| **Space** | shoot |

Three targets sit ahead of you along +X, which is the direction a character faces on spawn. Press
Space as soon as the level opens and you will hit the middle one.

## The files

| File | What it is |
|---|---|
| `Scripts/AN_FPCharacter.ocgraph` | the player: walks, looks, and shoots what it is looking at |
| `Scripts/AN_FPTarget.ocgraph` | a target: scales itself up, counts hits as its own score |
| `Scripts/AN_FPRules.ocgraph` | the game mode: names the pawn and controller, keeps a round clock |
| `Scripts/AN_FPController.ocgraph` | the player controller. Empty on purpose — see below |
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
      -> Branch (InputKey Space held?)
           -> Raycast, fired from GetForward              aim
                -> Branch (did it hit?)
                     -> FireEvent OnHit at Raycast.entity score

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

- **Firing is level-triggered, not edge-triggered.** Holding Space fires every tick.
  `aver_fw_input_key_pressed` exists at the ABI but no node reaches it yet, so a real
  once-per-press weapon needs either that node or a VAR remembering last frame's state.
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

Run headlessly against a build of this engine, with the Space gate temporarily bypassed (headless has
no input at all — `[Game] headless: no window, no input`):

- the centre target reached **89 hits over 90 frames**, one per tick;
- both side targets stayed at **0** across the same run.

That second number is the one that matters. It shows the ray hits what it is aimed at rather than
anything nearby, and that each target's score is genuinely its own.
