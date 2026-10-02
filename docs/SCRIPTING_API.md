# Aver Engine — C# Scripting API

The gameplay you write for Aver Engine is **C# that runs in-process** against the engine's entity/component
world. You derive a base type (`AverActor`, `AverPawn`, `AverCharacter`, …), override lifecycle hooks
(`OnBeginPlay`, `OnTick`, …), and reach the running game through a small, static-first API (`Game`,
`Input`, and the `Entity` handle). There is **no `UObject`**: a "class" is a row in a native registry, and
spawning is a memcpy of that row's defaults — so scripts are thin, and the surface below is all you touch.

This document is the reference for that surface. It lives in `Aver.Framework` (namespace
`Aver.Framework`); vector maths (`Vec3`, `Quat`, `Rot`) lives in `Aver.Scene`; the log (`Log`) lives in
`Aver.Scripting`. Two further assemblies a game author writes against directly are documented here too:
**`Aver.UI`** (the game's HUD) and **`Aver.Materials`** (surfaces authored in C#). A new project
references all four whether or not it uses them (`sandbox/src/ProjectScaffold.cpp`) — an unused
reference costs nothing, and the alternative is a compile error naming an assembly the author has never
heard of, in a file the editor generated.

---

## Contents

1. [Getting started](#1-getting-started)
2. [The class hierarchy](#2-the-class-hierarchy)
3. [The play lifecycle](#3-the-play-lifecycle)
4. [Lifecycle hooks](#4-lifecycle-hooks)
5. [Actors, Pawns, Characters, Controllers, GameMode](#5-actors-pawns-characters-controllers-gamemode)
6. [`Entity` — the handle to a thing in the world](#6-entity--the-handle-to-a-thing-in-the-world)
7. [`Game` — the running session](#7-game--the-running-session)
8. [`Input`](#8-input)
9. [Spawning and destroying](#9-spawning-and-destroying)
10. [Possession](#10-possession)
11. [Attributes](#11-attributes)
12. [Enums](#12-enums)
13. [`ClassBuilder` and `ActorBuilder`](#13-classbuilder-and-actorbuilder)
14. [`Physics`](#14-physics)
15. [Maths — `Vec3`, `Quat`, `Rot`](#15-maths--vec3-quat-rot)
16. [`Aver.UI` — the game's HUD](#16-averui--the-games-hud)
17. [`Aver.Materials` — surfaces authored in C#](#17-avermaterials--surfaces-authored-in-c)
18. [Worked examples](#18-worked-examples)
19. [Not yet available](#19-not-yet-available)

---

## 1. Getting started

A gameplay type is a C# class that (a) derives an Aver base type, and (b) carries an `[AverClass("Name")]`
(or `[AverGameMode(...)]`) attribute naming it. The editor's **Content Browser → + Add → New C# Script**
scaffolds one; a hand-written one looks like this:

```csharp
using Aver.Framework;
using Aver.Scene;      // Vec3, Rot
using Aver.Scripting;  // Log

namespace MyGame;

[AverClass("AN_Rotator")]
public sealed class Rotator : AverActor
{
    // The class RECIPE — run once per class at load, never per instance. Declares components + tick.
    public static void Configure(ClassBuilder b)
    {
        b.Mesh("Meshes/cube.ocmesh");   // this actor's own entity gets a mesh
        b.Ticks(TickGroup.PrePhysics);  // ask to tick
    }

    private float _angle;

    public override void OnBeginPlay(BeginReason reason) => Log.Info("Rotator started");

    public override void OnTick(float dt)
    {
        _angle += 90f * dt;                              // 90 deg/sec
        Self.SetLocalRotation(new Rot(_angle, 0, 0).ToQuat());
    }
}
```

**How it runs.** The editor's *Compile C#* button builds the project's `Scripts.csproj` and, when the
scripting host is present, swaps the result in live. The two Tools items are the halves of that button
and fail differently, which is why they are separate: **Tools → Compile Scripts** only builds (the
editor keeps running whatever it already loaded), **Tools → Reload Scripts** builds and then unloads and
reloads. At load the host reflects every `AverActor`-derived type, calls its
`static Configure(ClassBuilder)` (if present) to register the class's defaults, and wires the lifecycle.
Pressing **Play** starts a session (below); your actors' hooks then fire.

One assembly, two source roots: `Scripts.csproj` compiles `Content/Scripts` **and** `Content/Materials`
(see [§17](#17-avermaterials--surfaces-authored-in-c)). After a successful build the editor runs
`avermatc` over that assembly to bake the C# materials; a material that fails to bake is reported but
does **not** fail the build, because the C# did compile and a red build light would say otherwise
(`sandbox/src/ToolsMenu.cpp`).

**Two rules worth knowing up front:**

- **Hooks are `virtual` overrides, not attributes** — a misspelt `OnTick` is a *compile error*, not a
  method that silently never runs.
- **`0` is always invalid.** A default `Entity` is invalid; a lookup that finds nothing returns
  `Entity.None`; `IsValid`/`IsAlive` tell you.

**Logging.** `Console.WriteLine` goes nowhere in the editor's windowed process; write to the engine's
Output Log through `Aver.Scripting.Log` instead: `Log.Trace/Info/Warn/Error(string)` (and the explicit
`Log.Write(Log.Level, string)`). It falls back to the console when run outside a host.

---

## 2. The class hierarchy

```
AverActor                     the root: a thing in the world with a transform and a lifecycle
├── AverPawn                  an actor that can be POSSESSED by a controller
│   └── AverCharacter         a first/third-person walking pawn (WASD + mouse)
├── AverPlayerController      the will that drives a pawn (possession control)
├── AverGameMode              the rules of a play session (names its pawn + controller)
└── AverGameInstance          process-lifetime state that spans levels
```

The base type you derive is how the **compiler** decides which capability flag the class gets
(`PAWN`, `CONTROLLER`, `GAME_MODE`, …) and which hooks you may override. `AverPlayerController` is the one
non-abstract base — many games never subclass it and just name `"PlayerController"` as the controller.

---

## 3. The play lifecycle

The world has two lives: **EDITOR** (authoring) and **PLAYING**. Pressing **Play** in the editor calls
`begin_play`, which:

1. moves the state to `Playing` **first** — so every `OnBeginPlay` and `OnPostLogin` fired below already
   observes `Game.State == Playing`, not the editor it is leaving;
2. spawns the **GameInstance** (if the project has one), then the **GameMode** (mandatory — if *it* fails
   to spawn, the state is restored to `Editor` and no session forms);
3. spawns the GameMode's named **PlayerController** and **Pawn**, and **possesses** the pawn (a mode may
   legally name neither — a menu mode, say);
4. fires the GameMode's `OnPostLogin`, after the possess, so it can already see the controller's pawn.

The ordering of step 1 is load-bearing, not incidental: a hook that gates on `Game.IsPlaying` runs during
begin-play and must see the session it is starting.

**Stop** calls `end_play`, which unpossesses, then ends the four session roots in the *reverse* of spawn
order (so `OnEndPlay` mirrors `OnBeginPlay`), then sweeps **every other actor the session spawned** — an
actor a GameMode spawned in a hook is not a root, and would otherwise get `OnBeginPlay` and never
`OnEndPlay`. Everything ends with `EndReason.Stop`, and the world returns to `EDITOR`. Read the current state and
singletons through [`Game`](#7-game--the-running-session).

The hook order within a frame: begin-play queue drains → `OnTick` in tick-group order → destroy queue
drains. Gameplay ticks only while `Playing` (a `Paused` session freezes it without tearing down).

---

## 4. Lifecycle hooks

Override these on your `AverActor` subclass. All are optional; the defaults do nothing.

| Hook | When |
|---|---|
| `void OnBeginPlay(BeginReason reason)` | Once when the actor begins playing. `reason` is `Spawn` (fresh), `Play` (an already-placed actor entering Play), or `Reload` (after a hot reload). |
| `void OnTick(float dt)` | Every frame the class is scheduled to tick, in its tick group. `dt` is clamped seconds. Requires `b.Ticks(...)` in `Configure`. |
| `void OnEndPlay(EndReason reason)` | Once when the actor stops playing. `Stop` = the world stopped; `Destroy` = it was destroyed; `Reload` = the entity stays, only the managed half is rebuilt. |
| `void OnRebound()` | **DECLARED BUT NEVER CALLED.** The hook exists in `framework_hooks.h` and nothing in the engine invokes it; the bridge's own comment records the rebind path as out of scope. Do not put logic here expecting it to run. |
| `bool OnEvent(string name)` | A named event was raised **at this entity**. Two things raise one: an **animation notify** (a marker crossed on a playing clip) and a graph's `FireEvent` node aimed here. Return `true` if you handled it -- the value is what the caller reads as its own success. |
| `void BuildModels(ActorBuilder builder)` | `protected`. Builds the actor's model tree; the editor overrides it in a generated `.Designer.cs`. Hand code rarely writes this. |

**Animation curves** are the value-shaped sibling of notifies: `entity.GetAnimationCurve("ReloadProgress")`
reads a named float off the clip the entity is playing, at its current playhead, and
`TryGetAnimationCurve(name, out value)` says whether the curve was there at all — which matters,
because "not there" and "reads zero" mean opposite things. Poll them from `OnTick`; a curve is a
value that always exists, not an event that fires.

`OnEvent` is **one hook, not a method per event name.** Unreal resolves anim notifies by reflecting on
`AnimNotify_<Name>`; that works there because a notify is its own class. Here an event name is opaque all
the way down -- the format stores a string, `GraphHost.Fire` branches on nothing, and a project invents
names freely -- so a name-based convention would turn a typo into silence, and would still need this method
underneath for names that are computed. Switch on `name`, and return `false` from the default branch.

```csharp
public override bool OnEvent(string name)
{
    switch (name)
    {
        case "OnFootstep": PlayFootstep(); return true;
        default: return false;
    }
}
```

An entity is bound to a graph **or** to a C# actor, never both, so an event reaches exactly one of them.
A graph is checked first; a C# actor is the fallback. Nothing receives it twice.

Pawn-only hooks (`AverPawn`): `OnPossessed(Entity controller)`, `OnUnpossessed()`.
GameMode-only hook (`AverGameMode`): `OnPostLogin(Entity controller)`.

> **Exceptions never cross back into the engine.** A throw out of any hook is caught, logged, and that one
> actor is disabled for the session; the process and every other actor survive.

---

## 5. Actors, Pawns, Characters, Controllers, GameMode

### `AverActor`

The root type. Every actor has `public Entity Self { get; }` — its entity handle (you read it, never
assign it). See [Spawning](#9-spawning-and-destroying) for the `Spawn`/`Destroy` members.

### `AverPawn : AverActor`

Adds possession. `public Entity Controller`, `public T? ControllerAs<T>()`, `public bool IsPossessed`, and
the `OnPossessed`/`OnUnpossessed` hooks.

### `AverCharacter : AverPawn`

A first/third-person walking character.

| Member | Description |
|---|---|
| `public float MoveSpeed` | Ground move speed, cm/s (default 350). |
| `public float TurnSpeed` | Mouse-look sensitivity, degrees of yaw per pixel (default 0.2). |
| `public CameraView CameraViewMode` | `FirstPerson` or `ThirdPerson` (default third). |
| `public float EyeHeight` | First-person eye height, cm (default 160). |
| `public float BoomLength` | Third-person camera distance behind, cm (default 450). |
| `public float Yaw { get; }` | The character's facing yaw, degrees (read-only; turned by `DriveWithInput`, set by `SetYaw`). |
| `public float Height` / `public float Radius` | Capsule size, cm (default 180 / 34). Set before the first tick. |
| `public float JumpSpeed` | Upward speed a jump starts with, cm/s (default 465 — about 110 cm of height). |
| `public bool IsSimulated` | True when backed by the physics world rather than translating directly. |
| `public bool IsGrounded` | True while standing on ground shallow enough to hold. |
| `public Vec3 Velocity` | The character's own velocity, cm/s, relative to the ground it stands on: a moving deck's or lift's motion is added by the physics step and is NOT in this value, so a passenger standing still on a train reads zero. The vertical component is the simulation's. Setting it is a dead stop that also clears the motion a jump off moving ground carried away (`aver_phys_character_inherited_velocity`); world-space velocity is this plus that. |
| `public bool Jump()` | Jump if grounded; returns false when airborne rather than swallowing it. |
| `public void Teleport(Vec3 feet)` | Move the character *and* its capsule. Setting the transform alone leaves the capsule behind. |
| `protected void DriveWithInput(float dt)` | One frame of WASD-walk + mouse-turn control; call from `OnTick` while possessed. Reads the settled result of the last step, then writes this frame's intent. |
| `protected void SetYaw(float degrees)` | Snap the facing yaw without the mouse having moved it. |

The character's origin is its **feet**, not the capsule centre — which is what `EyeHeight` already
assumed, and what makes "put it on the ground" mean setting `z` to the ground height.

### `AverPlayerController : AverActor`

The will that drives a pawn. See [Possession](#10-possession).

### `AverGameMode : AverActor`

The rules for a play session. Names its pawn/controller *by string* on `[AverGameMode]`. Adds
`virtual void OnPostLogin(Entity controller)` — fires once the controller has entered.

### `AverGameInstance : AverActor`

Process-lifetime state that spans levels. Adds nothing over `AverActor`; the concept it contributes is
*lifetime*. Reach it with `Game.Instance`.

---

## 6. `Entity` — the handle to a thing in the world

`Entity` is a `readonly struct` — a 32-bit key into the world's component arrays. `default` is invalid.
Transform **writes are methods** (not property setters) because it is returned by value.

**Identity**

| Member | Description |
|---|---|
| `int Handle` | The raw ABI handle (0 = invalid). |
| `static Entity None` | The invalid handle. |
| `bool IsValid` | `Handle != 0`. |
| `bool IsAlive` | The handle still addresses a live entity. |
| `bool IsActor` | A gameplay class owns this entity (vs a plain scene entity). |
| `ActorClass Class` | The class this entity is an instance of. |
| `string Name` / `void SetName(string)` | The display name. |
| `ulong ObjectId` | The persisted identity (fnv1a64 of the name), survives serialisation. |
| `AverActor? Actor` / `T? As<T>()` | The live managed instance bound to this entity, or null. |
| `void Destroy()` | Destroy this entity (full framework teardown if it's an actor). |

**Transform** (local, centimetres; rotation as a quaternion — compose in degrees via `Rot`)

| Member | Description |
|---|---|
| `Vec3 LocalPosition` / `void SetLocalPosition(Vec3)` / `void Translate(Vec3 delta)` | Local position. |
| `Quat LocalRotation` / `void SetLocalRotation(Quat)` | Local rotation. |
| `Vec3 LocalScale` / `void SetLocalScale(Vec3)` | Local scale. |
| `Vec3 Forward` / `Right` / `Up` | Local axes (+X / +Y / +Z), rotated by the local rotation. |
| `Vec3 WorldPosition` | World-space position, composed through parents. |
| `Vec3 WorldForward` / `WorldRight` / `WorldUp` | World-space axes, normalised. |

**Hierarchy**

| Member | Description |
|---|---|
| `Entity Parent` / `Entity FirstChild` / `Entity NextSibling` / `int ChildCount` | Links. |
| `IEnumerable<Entity> Children` | Immediate children, in order. |
| `bool SetParent(Entity parent)` / `bool Detach()` | Reparent (`Entity.None` → root) / detach. Refuses a cycle. |

**Components** (see the [`Component`](#12-enums) enum)

| Member | Description |
|---|---|
| `bool HasComponent(Component)` / `bool AddComponent(Component)` | Query / attach (idempotent). |
| `bool Visible` / `bool SetVisible(bool)` | Mesh visibility (adds a mesh renderer if absent). |
| `bool SetMesh(string path)` / `bool SetMaterial(string name)` | Set the drawn mesh / material. |

**Tags** (a 32-bit mask the gameplay layer gives meaning to — define your own bit constants)

| Member | Description |
|---|---|
| `uint Tags` / `bool SetTags(uint)` | The whole mask. |
| `bool HasTag(uint)` / `bool HasAnyTag(uint)` | All bits set / any bit set. |
| `bool AddTag(uint)` / `bool RemoveTag(uint)` | Set / clear bits. |

**Generic component fields** (by qualified name, e.g. `"CLight.intensityLux"` — works for `[Editable]`
fields and any component field)

`float GetFloat(string)` · `bool SetFloat(string, float)` · `int GetInt(string)` ·
`bool SetInt(string, int)` · `long GetInt64(string)` · `bool SetInt64(string, long)` ·
`Vec3 GetVec3(string)` · `bool SetVec3(string, Vec3)` · `string GetString(string)` ·
`bool SetString(string, string)`

---

## 7. `Game` — the running session

Static. In EDITOR every handle is `Entity.None` and `State` is `Editor`. **Read-only**: starting/stopping
a session is the editor's job, not a script's.

**State**

| Member | Description |
|---|---|
| `PlayState State` | `Editor` / `Playing` / `Paused`. |
| `bool IsPlaying` / `bool IsPaused` / `bool HasSession` | Playing / paused / a session exists (playing OR paused). |

**Session singletons** (each `Entity`, with a typed `…As<T>()` counterpart)

| Member | Typed |
|---|---|
| `Entity Mode` | `T? ModeAs<T>()` where `T : AverGameMode` |
| `Entity Instance` | `T? InstanceAs<T>()` where `T : AverGameInstance` |
| `Entity LocalPlayerController` / `Entity GetPlayerController(int i = 0)` | `T? PlayerControllerAs<T>(int i = 0)` |
| `Entity GetPlayerPawn(int i = 0)` | `T? PlayerPawnAs<T>(int i = 0)` where `T : AverPawn` |

**Lookup & iteration** (each a linear scan taken this frame — don't spawn/destroy while enumerating)

| Member | Description |
|---|---|
| `Entity Find(string name)` / `T? FindActor<T>(string name)` | Resolve an actor by name. |
| `IEnumerable<Entity> AllActors()` | Every live actor. |
| `IEnumerable<T> ActorsOf<T>()` | Every live actor whose instance is a `T`. |
| `IEnumerable<Entity> WithTag(uint mask)` | Every entity carrying all of `mask`. |

`Actors.Get(Entity)` / `Actors.Get<T>(Entity)` are the lower-level resolvers `Entity.As<T>` is built on.

---

## 8. `Input`

Static, polled, Unity-style. The editor pushes the frame's device state each tick (suppressed while a text
field has focus). Read it from `OnTick`.

| Member | Description |
|---|---|
| `bool GetKey(Key k)` | Held. |
| `bool GetKeyDown(Key k)` / `bool GetKeyUp(Key k)` | Went down / up this frame. |
| `float MouseDeltaX` / `MouseDeltaY` / `MouseWheel` | This frame's mouse movement / wheel. |
| `Vec3 MoveAxis` | WASD/arrows as `(forward, right, 0)`; not normalised. |

`Key` values: `A`–`Z`, `D0`–`D9`, `Space`, `LeftShift`, `LeftCtrl`, `LeftAlt`, `Enter`, `Escape`, `Tab`,
`Left`, `Right`, `Up`, `Down`, `MouseLeft`, `MouseRight`, `MouseMiddle`.

### Action mappings

`Input` is the device. `EnhancedInput` is the layer above it, for gameplay that should name *what the
player is doing* rather than which key they pressed — so a pawn asks whether `Fire` happened and never
mentions a key (`scripting/csharp/Aver.Framework/EnhancedInput.cs`). **Actions are native as of this
build**: `EnhancedInput`/`InputAction`/`InputMappingContext` keep the exact same public shape below, but
every read now goes straight through to the framework ABI's own action layer
(`aver_fw_action_register/find/bind/value2/held/pressed/released` — `framework_abi.h`'s Named Actions
section), not a pure-C# reimplementation. The practical effect is that a native caller (a C++ system, or
an Aver Node graph — see the box at the end of this section) sees the *identical* answer a C# pawn does,
because both read the identical `cur`/`prev` bytes rather than two independently maintained copies.

| Type | Description |
|---|---|
| `InputAction` | A thing the player can do. `InputAction.Digital(name)` / `.Axis1D(name)` / `.Axis2D(name)`. Read `IsHeld`, `WasPressed`, `WasReleased`, `Value1D`, `Value2D`. Declare each **once**, usually as a `static readonly` field on a class shared between the pawn that reads it and the context that binds it — enforced now, not just advised: two declarations sharing a name resolve to the same underlying action, never two independent ones. |
| `InputMappingContext` | A set of key→action bindings pushed and popped as a whole. Derive it and bind in the constructor: `BindKey`, `BindAxis1D(action, positive, negative)`, `BindAxis2D(action, up, down, right, left)`, `BindMouseLook`, `BindMouseWheel`. |
| `EnhancedInput` | The router: `AddContext(context, priority = 0)`, `RemoveContext`, `ClearContexts`, `ContextCount`. |

A **strictly-higher-priority binding consumes a key it shares with a lower one**, so the lower binding
never sees it — pushing a menu or vehicle context at a higher priority suppresses the walking bindings
without anything having to disable them. Consumption compares the raw priority NUMBER passed to
`AddContext`, not the context object: two contexts pushed at the *same* priority never block each
other's shared keys, whether or not they are the same object. Reading an action is a plain function of
this frame's (and, for an edge, last frame's) device state, so it needs no once-per-frame precompute —
every actor in a frame that asks the same question gets the same answer, in whatever order they ask it.

> **Also reachable from an Aver Node graph.** `InputAction` / `InputActionPressed` / `InputActionReleased`
> nodes read the same actions this section describes — see the node reference's Input category. The
> graph-side `action` pin is the numeric **handle** `aver_fw_action_register`/`aver_fw_action_find`
> returned, not the name string this section's `InputAction.Digital("Jump")` takes, so a graph reads an
> action a C# script (or a project's own setup code) has already registered and bound; it does not do
> either itself.

```csharp
static class GameActions
{
    public static readonly InputAction Move = InputAction.Axis2D("Move");
    public static readonly InputAction Jump = InputAction.Digital("Jump");
}

public sealed class OnFoot : InputMappingContext
{
    public OnFoot()
    {
        BindAxis2D(GameActions.Move, Key.W, Key.S, Key.D, Key.A);   // X forward, Y right
        BindKey(GameActions.Jump, Key.Space);
    }
}
```

---

## 9. Spawning and destroying

`Spawn` is `protected static` on `AverActor` (call from within an actor). The typed forms return the **live
instance** (or null if the class isn't declared); the `ActorClass` forms return the `Entity`.

| Member | Description |
|---|---|
| `Entity Spawn(ActorClass c, Vec3 at)` | Spawn a class chosen at runtime. |
| `Entity Spawn(ActorClass c, Vec3 at, Rot rotation)` | …with orientation. |
| `Entity Spawn(ActorClass c, Vec3 at, Rot rotation, Vec3 scale)` | …with a full transform. |
| `T? Spawn<T>(Vec3 at)` | Spawn `T`, return the instance. |
| `T? Spawn<T>(Vec3 at, Rot rotation)` / `T? Spawn<T>(Vec3 at, Rot rotation, Vec3 scale)` | …with orientation / full transform. |
| `T? SpawnAttached<T>(Entity parent, Vec3 localPosition)` | Spawn as a child of `parent`; returns null if the attach is rejected (and cleans up). |
| `void Destroy()` | Destroy this actor now (fires `OnEndPlay`). |
| `static void Destroy(Entity other)` / `static void Destroy(AverActor? other)` | Destroy another. |

**`ActorClass`** — the `Spawn<T>` forms name the class by *type*; the `Spawn(ActorClass, …)` forms take a
class **handle**, for when the class is chosen at runtime. Get one with `ActorClass.Find("AN_Coin")` (an
invalid handle if no such class is declared — check `IsValid`); `Entity.Class` gives the class an entity is
an instance of. An `ActorClass` is a `readonly struct` with `int Handle`, `bool IsValid`, and `string Name`
(`0 == invalid`).

```csharp
ActorClass coin = ActorClass.Find("AN_Coin");
if (coin.IsValid)
    for (int i = 0; i < count; i++)
        Spawn(coin, new Vec3(i * 100f, 0f, 20f));   // class picked at runtime
```

---

## 10. Possession

On `AverPlayerController`:

| Member | Description |
|---|---|
| `Entity Possessed` / `T? PossessedAs<T>()` / `bool HasPawn` | The controlled pawn. |
| `bool Possess(Entity pawn)` / `bool Possess(AverPawn pawn)` | Take control. A pawn already driven by another controller is *stolen* (it gets `OnUnpossessed` then `OnPossessed`); re-possessing the pawn you already drive is a no-op that returns true. |
| `bool Unpossess()` | Release the current pawn. |

On the pawn side, `AverPawn.Controller` / `ControllerAs<T>()` / `IsPossessed` read the inverse, straight
from the world (never cached).

---

## 11. Attributes

| Attribute | Use |
|---|---|
| `[AverClass("Name")]` | Names a gameplay class. Optional `Parent = "…"` (inferred from the base type otherwise). |
| `[AverGameMode("Name")]` | Names a GameMode. `DefaultPawnClass = "…"`, `PlayerControllerClass = "…"` name (by string) the pawn and controller it spawns. |
| `[Editable]` | A field editable in the Details panel and stored in native storage (survives Play/Stop and hot reload). Optional `Min` / `Max`. |
| `[Model]` | Marks a model-tree field the editor manages. |

**`[AverClass]` is not the only door into this registry.** A `.ocgraph` file can declare itself a
class too — a top-level `CLASS <name> [parent]` record, registered through the identical
`aver_fw_class_declare` / `aver_fw_class_seal` sequence an `[AverClass]` type goes through.
`ActorClass.Find("AN_Whatever")` and `Spawn(...)` cannot tell, and do not need to, whether the class
they resolved came from a C# type or a graph with no C# behind it at all. See
[`docs/VISUAL_SCRIPTING.md`](VISUAL_SCRIPTING.md) for that model, and
[`docs/formats/FORMAT_SPECS.md` §10a](formats/FORMAT_SPECS.md) for the `CLASS` record's grammar.

---

## 12. Enums

| Enum | Values |
|---|---|
| `BeginReason` | `Spawn` (0), `Play` (1), `Reload` (2) |
| `EndReason` | `Destroy` (0), `Stop` (1), `Reload` (2), `Travel` (3) |
| `TickGroup` | `PrePhysics` (0), `Physics` (1), `PostPhysics` (2) |
| `PlayState` | `Editor` (0), `Playing` (1), `Paused` (2) |
| `CameraView` | `FirstPerson`, `ThirdPerson` |
| `Component` | `Local` (1), `World` (2), `Hierarchy` (3), `Name` (4), `Tags` (5), `MeshRenderer` (6), `Light` (7), `Camera` (8) |

---

## 13. `ClassBuilder` and `ActorBuilder`

`ClassBuilder` writes a class's **defaults** — the recipe run once per class, in `static void
Configure(ClassBuilder b)`:

| Member | Description |
|---|---|
| `void Mesh(string meshPath, string material = "")` | Give the class's own entity a mesh (and optional material). |
| `void PointLight(float intensityLux, float rangeCm)` | Add a point light. |
| `void Camera(float fovDegrees, float nearCm, float farCm)` | Add a camera. |
| `void Ticks(TickGroup group, int order = 0)` | Ask the class to tick, in `group`. |

`ActorBuilder` writes the editor-owned **model tree** inside `BuildModels`; hand code rarely touches it.
`ModelHandle` is the handle a placed model returns.

**How a mesh path resolves.** `Mesh("Meshes/crate.ocmesh")` stores no string: the path is hashed
(fnv1a64) to the `u64` ObjectId the `CMeshRenderer` field actually holds. Every `.ocmesh` under the
project's `Content` is loaded at project open and registered under the hash of its **content-relative
path with forward slashes** — so that exact spelling is the contract between a C# class, a level
placement and the loader, and a path spelt any other way hashes to a different number and silently draws
nothing. Two primitives are registered without a file behind them: `Meshes/cube.ocmesh` and
`Meshes/sphere.ocmesh`. The `material` argument is a *name*, resolved to its `i32` handle the same way
`Entity.SetMaterial` resolves one — see [§17](#17-avermaterials--surfaces-authored-in-c) for where that
name comes from.

---

## 14. `Physics`

Static, in `Aver.Framework`. Everything is in the engine's contract — centimetres, +X forward,
+Y right, +Z up, left-handed. The backend (Jolt, MIT) is right-handed, +Y up and metric; that
translation happens once behind the native ABI, so no Jolt convention ever reaches a script.

The world is stepped by the frame loop **between the PrePhysics and PostPhysics tick groups**, at a
**fixed step** regardless of frame rate. So set what you want in a `PrePhysics` tick and read what
happened in a `PostPhysics` one.

| Member | Description |
|---|---|
| `bool Ready` | True when the simulation is running (false if physics was compiled out). |
| `float FixedStep` | The fixed step in seconds — 1/60 unless the host changed it. |
| `void SetGravity(Vec3)` | cm/s². Default `(0, 0, -980)`: one g, straight down. |
| `int BodyCount` | How many bodies exist. |
| `Body AddStaticBox(Vec3 centre, Vec3 halfExtents)` | A box that never moves. |
| `Body AddDynamicBox(Vec3 centre, Vec3 halfExtents, float massKg = 0)` | Falls and collides; `massKg <= 0` derives mass from volume. |
| `Body AddDynamicSphere(Vec3 centre, float radius, float massKg = 0)` | Likewise. |
| `RaycastHit Raycast(Vec3 origin, Vec3 direction, float maxDistanceCm)` | Direction need not be unit length. |
| `bool RaycastAny(...)` | Just whether anything is in the way. |

**`Body`** — a handle (`0` invalid): `Handle`, `IsValid`, `Position`, `Rotation`, `Velocity`,
`SetPosition`, `SetVelocity`, `Destroy()`, `SetEntity(Entity)` (stamps which scene entity owns this
body, for a later `Raycast` against it to identify — level placements, characters and editor-dropped
bodies already have this set; a script's own `AddDynamicBox`/`AddDynamicSphere`/`AddStaticBox` does not).
**`RaycastHit`** — `Hit`, `Body`, `Entity`, `Point`, `Normal`. `Entity` is whatever `SetEntity` stamped
the hit body or character with — `Entity.None` (0) means a real hit against something no entity owns
(a landscape heightfield, say), which is not the same thing as `Hit` being false; check `Hit` first.
Only `Raycast` resolves it — `SphereCast`'s `RaycastHit.Entity` is always `Entity.None`.

> The backend documents **broadphase queries as non-deterministic** (the broad phase is modified from
> several threads), and callback ordering likewise. Rely on *whether* something was hit and *where* —
> never on which of several equidistant bodies comes back.

---

## 15. Maths — `Vec3`, `Quat`, `Rot`

In `Aver.Scene` (centimetres; +X forward, +Y right, +Z up; left-handed).

- **`Vec3`** — `float X, Y, Z`; `new Vec3(x, y, z)`; statics `Forward` `(1,0,0)`, `Right` `(0,1,0)`,
  `Up` `(0,0,1)`, `Zero`, `One`; `+ - *` operators.
- **`Quat`** — `float X, Y, Z, W`; static `Identity`; `Quat.FromAxisAngle(Vec3 axis, float radians)`; `*`.
- **`Rot`** — degrees a viewport gizmo edits: `float Yaw` (about up), `Pitch` (about right), `Roll` (about
  forward); `new Rot(yaw, pitch, roll)`; `Rot.Zero`; `Quat ToQuat()`.

---

## 16. `Aver.UI` — the game's HUD

Namespace `Aver.UI`, in the assembly of the same name (`scripting/csharp/Aver.UI/Hud.cs`). It references
nothing else — a HUD is drawn in screen coordinates and knows nothing about entities or the world — and
P/Invokes the native `Aver.UI.Abi` (`modules/ui.abi/include/aver/ui/ui_abi.h`), which is the seam that
exists so a HUD can be authored in the game's language rather than in engine C++.

**Screen pixels, top-left origin, +Y down.** Not the world's centimetres and not normalised coordinates:
a UI is authored against a resolution, and every layout number a designer types is a pixel.

Three rules carry most of the design:

- **You draw from a tick; the HOST owns the frame.** The host clears the list once per frame *before*
  anything ticks and submits it after (`aver_ui_begin_frame` and `submitGameUi` in
  `sandbox/src/SandboxApp.cpp`). There is deliberately no `Begin` for a game to call: a game that
  cleared the list would erase whatever another system had contributed, and the last one to run would
  win with nothing anywhere to say so. One consequence to expect — gameplay ticks only while `Playing`,
  so a HUD drawn from `OnTick` is absent in EDITOR, which is correct rather than a bug to chase.
- **Anchor to `Hud.Viewport`, never to the window.** In the editor the game is drawn into a dockspace
  panel and the two rectangles differ; a HUD laid out against the window sits partly under the editor's
  own chrome. In a shipped build they are the same rectangle and nothing changes.
- **There is no text.** Nothing in the engine can rasterise a glyph, so there is no `Hud.Text` to call.
  A HUD today is rectangles, bars and tinted quads. There is no widget tree above the draw list either:
  a HUD is laid out by the code that draws it.

**`Hud`** — static, and the whole drawing surface:

| Member | Description |
|---|---|
| `Rect Viewport` | The rectangle this frame's UI is laid out against, in backbuffer pixels. |
| `Layer CurrentLayer` | The band subsequent draws land in. **Reset to `Layer.Content` at the start of every frame**, so set it in each frame that wants another band. An out-of-range value is ignored, not clamped — clamping would move a widget to a band its author did not choose and nothing would notice. |
| `void Box(Rect r, Colour c)` / `void Box(float x, float y, float w, float h, Colour c)` | A solid rectangle. |
| `void Frame(Rect r, float thickness, Colour border, Colour fill)` | A border drawn **inside** the bounds: a frame that grew its own bounds would not fit the layout that positioned it, and every caller would subtract the thickness back off by hand. |
| `void Bar(Rect r, float fraction, Colour border, Colour empty, Colour full)` | A 1px-framed bar filled left to right; `fraction` is clamped to 0..1. |
| `void Image(Rect r, ulong texture, Colour tint, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1)` | A textured quad. |
| `void PushClip(Rect r)` / `void PopClip()` | Nested clips **intersect**, so a child can never escape its parent by pushing a larger rectangle. That is containment by construction, not a promise each element keeps. |
| `int DrawCallCount` | Draw calls this frame's UI will cost — the whole list, so it includes anything the host contributed. For a debug readout, not for logic. |

`Image`'s `texture` is `0` for the built-in white texel, or a value the renderer recognises. **There is
no managed way to obtain one of those values yet**, so in practice a game passes `0` and tints; a
texture bound this way must also be premultiplied, because the colour is.

**`Layer`** — `Background` (0), `Content` (1, the default), `Overlay` (2), `Tooltip` (3), `Debug` (4).
Coarse and named on purpose: a widget picks the band it belongs in rather than a depth number that would
make every z decision global, so adding a tooltip does not mean auditing every other widget. Order
*within* a layer is the order you drew in.

**`Colour`** — `Colour.Rgb(232, 228, 220)` (opaque unless a fourth byte is given), `Colour.Argb(0xFFE8E4DC)`,
`WithAlpha(float)` (clamped 0..1), `Colour.Lerp(from, to, t)`. Alpha is **straight**: the
premultiplication the renderer needs happens on the native side, and the packing the vertex actually
holds is internal precisely so a game never carries it around.

**`Rect`** — a `readonly record struct (float X, float Y, float Width, float Height)` with `Right`,
`Bottom`, `Inset(by)` (negative grows), and:

```csharp
Rect.Anchored(Rect container, float ax, float ay, float w, float h,
              float offsetX = 0f, float offsetY = 0f)
```

Anchoring rather than absolute placement, because a HUD outlives the resolution it was authored at: an
element pinned to the bottom-right by subtraction walks off-screen the moment the window is smaller than
the number that was subtracted.

```csharp
using Aver.Framework;
using Aver.UI;

[AverClass("AN_PlayerHud")]
public sealed class PlayerHud : AverActor
{
    // PostPhysics: draw what the frame settled on, not what it was asked for.
    public static void Configure(ClassBuilder b) => b.Ticks(TickGroup.PostPhysics);

    static readonly Colour Ink   = Colour.Argb(0xFFE8E4DC);
    static readonly Colour Panel = Colour.Argb(0xB0141820);   // 69% alpha — proves the blend
    static readonly Colour Hurt  = Colour.Rgb(232, 76, 46);

    [Editable(Min = 0f, Max = 1f)] public float Health = 1f;

    public override void OnTick(float dt)
    {
        Rect vp = Hud.Viewport;                       // NOT the window
        Rect bar = Rect.Anchored(vp, 0f, 1f, 260f, 18f, offsetX: 24f, offsetY: -24f);

        Hud.PushClip(vp);                             // nothing escapes into the editor's chrome
        Hud.Box(bar.Inset(-6f), Panel);               // Layer.Content — the default, every frame
        Hud.Bar(bar, Health, Ink, Panel, Colour.Lerp(Hurt, Ink, Health));
        Hud.PopClip();

        if (Health <= 0f)                             // a full-screen wash, over the HUD
        {
            Hud.CurrentLayer = Layer.Overlay;         // set per frame; it does not persist
            Hud.Box(vp, Hurt.WithAlpha(0.35f));
        }
    }
}
```

---

## 17. `Aver.Materials` — surfaces authored in C#

Namespace `Aver.Materials` (`scripting/csharp/Aver.Materials/`). Like `Aver.UI` it is a leaf: a material
describes a surface and knows nothing about entities, the world or a device.

**The C# is the source; the `.ocmat` is build output.** A `.cs` under `Content/Materials` declares a
surface. *Compile C#* builds it into the project's one script assembly, then `avermatc`
(`scripting/csharp/Aver.MaterialCompiler/`) reflects over that assembly, runs every `Configure`, and
writes one `.ocmat` per material into `<project>/Binaries/Materials`. The engine looks in
`Binaries/Materials` **first**, then `Content/Materials`, then a content-relative path
(`SandboxApp::materialForSurface`) — so a project that has adopted C# materials gets the built file and
one that has not keeps working exactly as it did with hand-authored `.ocmat`.

That ordering is why editing the generated file is a mistake with a delay on it: it is a build artefact,
the next compile overwrites it, and the header it carries says so. The Details panel's **Save to C#**
writes back to the `.cs` for the same reason (`modules/formats/include/aver/formats/MaterialScript.hpp`);
it rewrites only the body of `Configure`, preserves the `.Comment(…)` calls — authored prose that cannot
be re-derived from a material's numbers — and declines outright rather than half-succeeding on a file it
did not parse.

**Declaring one.** `[AverMaterial("M_Crate")]` gives the **bound name** a mesh references. The name is
given in the attribute rather than taken from the class name for the reason `[AverClass]` does it:
renaming a C# class should not silently unbind every mesh that used it. Two classes claiming one name is
refused outright, not last-one-wins, since which of the two won would depend on reflection order.

`Configure` is **`static`**. A material is never instantiated and never ticks — it is a description the
compiler runs once, at build time, and nothing about it exists at run time — so there is no instance for
a virtual call to dispatch on. The compiler finds it by reflection and reports a `[AverMaterial]` class
that lacks one. `Material` is the base the templates derive; it carries nothing and exists so a material
is findable by type rather than only by attribute.

```csharp
using Aver.Materials;

namespace MyGame.Materials;

[AverMaterial("M_Crate")]
public sealed class Crate : Material
{
    public static void Configure(MaterialBuilder b) => b
        .Comment("Painted wood, scuffed. Roughness stays high — a gloss here reads as plastic.")
        .BaseColor(0.62f, 0.48f, 0.31f)
        .Metallic(0f)
        .Roughness(0.78f)
        .Texture(Slot.BaseColor, "Textures/crate_basecolor.png")
        .Texture(Slot.Normal,    "Textures/crate_normal.png");
}
```

Bind it from a class recipe with `b.Mesh("Meshes/crate.ocmesh", "M_Crate")`, or at run time with
`entity.SetMaterial("M_Crate")`.

**`MaterialBuilder`** — every call returns the builder, so a declaration is one chain:

| Member | Description |
|---|---|
| `Comment(string)` | A line written into the output above the parameters. Carried through on purpose: the generated file is what somebody debugging a surface opens, and a bare table of numbers with the reasoning left behind in a `.cs` is how a value becomes mysterious. |
| `Shader(Shading)` | The shading model. `Shading.Standard` — the metallic-roughness BRDF — is the only one. |
| `Blending(Blend, float cutoff = 0.5f)` | `Opaque` / `Masked` (the cutoff applies here only) / `Translucent` / `Additive`. |
| `Culling(Cull)` | `Back` / `Front` / `None`. `None` implies two-sided. |
| `CastShadow(bool)` | On by default. |
| `WorldUv(bool)` / `Tiling(float centimetres)` | Project UVs from world space at N cm per tile — what untextured blockout geometry wants. `Tiling` is written whether or not world UVs are on, so toggling the mode off and back on does not reset it. |
| `BaseColor(r, g, b, a = 1)` | Linear multiplier. |
| `Metallic(v)` / `Roughness(v)` | Multipliers, both defaulting to 1 — that is, "whatever the metalRough texture says". |
| `Emissive(r, g, b)` | Added, not multiplied, so it lights itself and nothing else. |
| `NormalScale(v)` / `OcclusionStrength(v)` | Strength of the normal map / of baked occlusion. |
| `Reflectance(v)` / `F90(v)` | Dielectric reflectance at normal incidence (0.04 is almost every non-metal) and at grazing incidence. |
| `Texture(Slot, string contentRelativePath)` | Bind a texture. The path is relative to the project's content root; backslashes are normalised to forward slashes. |

**`Slot`** — `BaseColor`, `MetalRough` (metallic in blue, roughness in green: the glTF packing),
`Normal`, `Occlusion`, `Emissive`.

**Colour space is a property of the slot and is deliberately not authorable.** `Texture` takes no
colour-space argument. Base colour and emissive are always sRGB, normal is always a normal map, and the
rest are always linear; letting a caller choose would only let a caller choose wrong. The emitter writes
the slot's space into the `TEX` line and the reader validates it, so the two cannot quietly disagree.

> The `.ocmat` grammar has **two writers** — this builder and `modules/formats/src/OcMat.cpp` — which is
> a real cost, paid so that baking a material does not require a loaded engine on the build path. The
> guard against them drifting is weaker than it sounds, and worth knowing before trusting it:
> `testGeneratedByCsharp` in `tests/formats/src/MaterialTest.cpp` parses a **pasted, verbatim** sample
> of `MaterialBuilder.Emit`'s output with the C++ reader. Nothing runs the C# emitter, so a change made
> only to `Emit` goes unnoticed until somebody re-pastes the sample. (This paragraph used to add that
> `MaterialBuilder.cs` carried a doc comment wrongly claiming a `MaterialCompilerTest` did this instead
> — that comment is gone now, fixed in `4c1e111`, and the file makes no such claim today.)

---

## 18. Worked examples

### A player character (WASD + mouse, first/third person)

```csharp
[AverClass("AN_Hero")]
public sealed class Hero : AverCharacter
{
    public static void Configure(ClassBuilder b)
    {
        b.Mesh("Meshes/hero.ocmesh");
        b.Ticks(TickGroup.PrePhysics);
    }

    public override void OnTick(float dt)
    {
        DriveWithInput(dt);                                    // WASD walks, mouse turns
        if (Input.GetKeyDown(Key.V))                           // V toggles the view
            CameraViewMode = CameraViewMode == CameraView.ThirdPerson
                ? CameraView.FirstPerson : CameraView.ThirdPerson;
    }
}
```

### A GameMode that hands out that character

```csharp
[AverGameMode("AN_GameMode", DefaultPawnClass = "AN_Hero", PlayerControllerClass = "PlayerController")]
public sealed class MyGameMode : AverGameMode
{
    public override void OnPostLogin(Entity controller) =>
        Log.Info($"{controller.Name} logged in, driving {controller.As<AverPlayerController>()?.Possessed.Name}");
}
```

### A spawner that scatters pickups and tags them

```csharp
[AverClass("AN_Spawner")]
public sealed class Spawner : AverActor
{
    const uint Pickup = 0x1;

    public override void OnBeginPlay(BeginReason reason)
    {
        for (int i = 0; i < 5; i++)
        {
            Coin? c = Spawn<Coin>(new Vec3(i * 100f, 0f, 20f));
            c?.Self.AddTag(Pickup);
        }
        Log.Info($"{System.Linq.Enumerable.Count(Game.WithTag(Pickup))} pickups in the world");
    }
}

[AverClass("AN_Coin")]
public sealed class Coin : AverActor
{
    public static void Configure(ClassBuilder b) => b.Mesh("Meshes/coin.ocmesh");
}
```

---

## 19. Not yet available

The gameplay object model above is complete. What follows is what a script still **cannot** reach,
stated as absences rather than left to be discovered:

- **Audio.** The pieces exist — a mixer with voices, 3D panning and attenuation, a WASAPI device, the
  `.ocaudio` container, and a C seam (`modules/audio`, `modules/audio.wasapi`, `modules/audio.abi`, and
  `modules/formats`; see `docs/AUDIO.md`). None of them are joined up: there is no managed binding, and
  neither the editor nor the runtime opens a device. A script cannot play a sound.
- **Text, and widgets, in the UI.** `Aver.UI` (§16) draws rectangles and tinted quads. Nothing in the
  engine rasterises a glyph, so there is no `Hud.Text`; and there is no widget tree above the draw list,
  so layout is whatever the drawing code computes. There is also no input routing into the UI — a HUD is
  drawn, not clicked.
- **Timers and coroutines.** No `SetTimer`, no `yield`. Count down in `OnTick`.
- **Networking.** `modules/net` exists; nothing in it is reachable from C#.
- **Hot-reload state migration** beyond `[Editable]` fields.
- **Import beyond glTF.** The Content Browser's Import *converts* `.gltf`/`.glb` into one `.ocmesh` per
  mesh and registers the result immediately (`SandboxApp::importModel`); every other file type is copied
  in unchanged. That is right for a `.png` a material names — a texture is decoded and uploaded straight
  from the source file, with its colour space taken from the slot that binds it — and wrong for an FBX,
  which is not read at all. No texture is converted, compressed or packed at import.

These are the natural next layers; the object model is the foundation they plug into.
