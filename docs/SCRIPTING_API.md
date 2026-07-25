# Aver Engine — C# Scripting API

The gameplay you write for Aver Engine is **C# that runs in-process** against the engine's entity/component
world. You derive a base type (`AverActor`, `AverPawn`, `AverCharacter`, …), override lifecycle hooks
(`OnBeginPlay`, `OnTick`, …), and reach the running game through a small, static-first API (`Game`,
`Input`, and the `Entity` handle). There is **no `UObject`**: a "class" is a row in a native registry, and
spawning is a memcpy of that row's defaults — so scripts are thin, and the surface below is all you touch.

This document is the reference for that surface. It lives in `Aver.Framework` (namespace
`Aver.Framework`); vector maths (`Vec3`, `Quat`, `Rot`) lives in `Aver.Scene`; the log (`Log`) lives in
`Aver.Scripting`.

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
14. [Maths — `Vec3`, `Quat`, `Rot`](#14-maths--vec3-quat-rot)
15. [Worked examples](#15-worked-examples)
16. [Not yet available](#16-not-yet-available)

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

[AverClass("BP_Rotator")]
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

**How it runs.** The editor's *Compile C#* button (or **Tools → Compile Scripts**) builds your
`Content/Scripts` into an assembly and hot-swaps it in. At load the host reflects every `AverActor`-derived
type, calls its `static Configure(ClassBuilder)` (if present) to register the class's defaults, and wires
the lifecycle. Pressing **Play** starts a session (below); your actors' hooks then fire.

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
| `void OnRebound()` | After a hot reload rebinds this instance and restores its `[Editable]` state — re-derive cached state here, not in `OnBeginPlay`. |
| `void BuildModels(ActorBuilder builder)` | `protected`. Builds the actor's model tree; the editor overrides it in a generated `.Designer.cs`. Hand code rarely writes this. |

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
| `protected void DriveWithInput(float dt)` | One frame of WASD-walk + mouse-turn control; call from `OnTick` while possessed. Kinematic (no physics yet). |
| `protected void SetYaw(float degrees)` | Snap the facing yaw without the mouse having moved it. |

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
class **handle**, for when the class is chosen at runtime. Get one with `ActorClass.Find("BP_Coin")` (an
invalid handle if no such class is declared — check `IsValid`); `Entity.Class` gives the class an entity is
an instance of. An `ActorClass` is a `readonly struct` with `int Handle`, `bool IsValid`, and `string Name`
(`0 == invalid`).

```csharp
ActorClass coin = ActorClass.Find("BP_Coin");
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

---

## 14. Maths — `Vec3`, `Quat`, `Rot`

In `Aver.Scene` (centimetres; +X forward, +Y right, +Z up; left-handed).

- **`Vec3`** — `float X, Y, Z`; `new Vec3(x, y, z)`; statics `Forward` `(1,0,0)`, `Right` `(0,1,0)`,
  `Up` `(0,0,1)`, `Zero`, `One`; `+ - *` operators.
- **`Quat`** — `float X, Y, Z, W`; static `Identity`; `Quat.FromAxisAngle(Vec3 axis, float radians)`; `*`.
- **`Rot`** — degrees a viewport gizmo edits: `float Yaw` (about up), `Pitch` (about right), `Roll` (about
  forward); `new Rot(yaw, pitch, roll)`; `Rot.Zero`; `Quat ToQuat()`.

---

## 15. Worked examples

### A player character (WASD + mouse, first/third person)

```csharp
[AverClass("BP_Hero")]
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
[AverGameMode("BP_GameMode", DefaultPawnClass = "BP_Hero", PlayerControllerClass = "PlayerController")]
public sealed class MyGameMode : AverGameMode
{
    public override void OnPostLogin(Entity controller) =>
        Log.Info($"{controller.Name} logged in, driving {controller.As<AverPlayerController>()?.Possessed.Name}");
}
```

### A spawner that scatters pickups and tags them

```csharp
[AverClass("BP_Spawner")]
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

[AverClass("BP_Coin")]
public sealed class Coin : AverActor
{
    public static void Configure(ClassBuilder b) => b.Mesh("Meshes/coin.ocmesh");
}
```

---

## 16. Not yet available

The gameplay object model above is complete. These are **separate subsystems** that do not exist yet — a
script cannot use them, and a character does not (for example) collide or fall:

- **Physics / collision** — movement is kinematic; there is no gravity, collision or stepping.
- **Input bindings** — input is raw polling (`Input.GetKey`), not a remappable action/axis map.
- **Audio**, **timers / coroutines**, **UI (in-game)**, **networking**.
- **Hot-reload state migration** beyond `[Editable]` fields.
- A **converting asset importer** — the Content Browser's Import copies files in; it does not convert
  FBX/PNG into engine formats. Meshes referenced by `Mesh("…")` resolve against the built-in primitives and
  (eventually) a real `.ocmesh` loader.

These are the natural next layers; the object model is the foundation they plug into.
