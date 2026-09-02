# C# Scripting

Reference for writing C# against Aver Engine. Covers the behaviour lifecycle, the `Pbr` and `Voxi`
APIs, how scripts are built and hot-reloaded, and — just as importantly — **what you cannot do yet**.

The authoritative surface is `scripting/csharp/Aver.Scripting/` and it carries XML doc comments, so
IntelliSense is accurate. This page is the orientation those comments cannot give.

---

## 1. Where scripts live and how they run

Scripts belong to a **project**, not to the engine (`docs/PROJECTS.md`):

```
<project>/Content/Scripts/          your .cs files + Scripts.csproj
<project>/Binaries/Scripts/         where Reload Scripts builds them
```

Create them from the editor: **Tools ▸ New C# Script…** (a behaviour) or **New C# Class…** (a plain
class). The first one also generates `Scripts.csproj` referencing `Aver.Scripting`, so the folder
opens as a real project in an IDE.

| menu item | what it does |
|---|---|
| **Compile Scripts** | `dotnet build` into `Binaries/Scripts`. The editor keeps running whatever it already loaded. |
| **Reload Scripts** | Rebuild, then unload and reload in place. Running behaviours get `OnShutdown`, the new ones get `OnStart`. No editor restart. |
| **Open Scripts In ▸** | Opens `Content/Scripts` in a detected IDE — Visual Studio, VS Code or Rider — falling back to whatever the shell has registered for `.csproj`. |

A failed build lists its diagnostics as **clickable rows**: clicking one opens that file at that
line and column in the detected IDE. Lines the parser did not recognise are still shown verbatim,
so nothing the compiler said is hidden.

Reload works because user assemblies are loaded into a **collectible `AssemblyLoadContext`** from a
memory stream — the DLL is never locked, and the old context is genuinely unloadable. **No state
carries across a reload.** A behaviour is constructed fresh; anything it held is gone.

The dev flag `--scripts <dir>` loads a directory directly, bypassing the project. `--scripts SampleScripts`
runs the bundled `HelloBehaviour`.

### When scripting is unavailable

The host **declines cleanly** — logs once, returns false, editor runs normally — when the .NET
runtime is absent, when `hostfxr` cannot be initialised, or when built with
`-DAVER_MODULE_SCRIPTING=OFF`. A missing runtime is never a startup failure. The Tools items disable
themselves with a tooltip naming the specific reason.

---

## 2. A behaviour

```csharp
using Aver.Scripting;

public sealed class Spinner : AverBehaviour
{
    private float _t;

    public override void OnStart()  => Log.Info("Spinner up");
    public override void OnUpdate(float dt) { _t += dt; }
    public override void OnShutdown() => Log.Info("Spinner down");
}
```

Derive from `AverBehaviour`; the host finds it by reflection when your assembly loads. It needs a
**public parameterless constructor** — the host constructs it, so there is nothing to pass.

All three hooks run on the **engine's main thread, inside the frame loop**. `dt` is seconds since
the last frame.

**It is a base class rather than an `[AverScript]` attribute deliberately.** With an attribute and
name-matched hooks, a misspelt `OnUpate` compiles cleanly and then simply never runs — no error
anywhere, which is the worst failure mode for a layer aimed at people who are not engine developers.
The base class makes the compiler check it. The cost is C#'s single inheritance, which is acceptable:
a behaviour is a leaf type, and shared logic belongs in a plain class it *holds*.

**Exceptions never cross back into the engine.** A throwing hook is logged and that behaviour is
disabled for the session. The process survives and other behaviours keep running.

### Logging

`Log.Trace/Info/Warn/Error` route into the engine's own log, so script output appears in the editor's
Output Log alongside engine messages. With no host (a standalone process) it falls back to `Console`.
`Log.HasHost` tells you which you are in.

---

## 3. `Pbr` — the material system

Materials are **instances**, so the API is handle-based. This is deliberately unlike `Voxi`, which is
a settings singleton.

```csharp
var m = Pbr.Create("Crate");
m.BaseColorFactor = (0.85f, 0.36f, 0.22f, 1f);
m.MetallicFactor  = 0.1f;
m.RoughnessFactor = 0.35f;
m.SetTexturePath(PbrTextureSlot.BaseColor, "Textures/crate_albedo.png");
```

### `PbrMaterial`

| member | notes |
|---|---|
| `Handle` | raw ABI handle; `0` is invalid |
| `IsValid` | **check this after storing a handle** — see below |
| `Destroy()` | releases the material |
| `Name` | |
| `BaseColorFactor` | `(R,G,B,A)` tint |
| `EmissiveFactor` | `(R,G,B)` radiance — **not** a ratio, so not clamped to 1 |
| `MetallicFactor` | |
| `RoughnessFactor` | clamped away from 0: a perfect mirror collapses the GGX denominator |
| `NormalScale`, `OcclusionStrength` | |
| `AlphaMode`, `AlphaCutoff` | cutoff is read only under `PbrAlphaMode.Mask` |
| `TwoSided`, `CastShadow` | |
| `GetTexturePath` / `SetTexturePath` / `ClearTexture` | per `PbrTextureSlot` |
| `GetTextureId` / `SetTextureId` | opaque `long` asset id (an ObjectId or `.octex` GUID); `0` = unset |
| `ConsumeDirty()` | **reading it clears it** — exactly one consumer acts on each change |

Slots: `BaseColor`, `MetalRough`, `Normal`, `Occlusion`, `Emissive`.

### Stale handles

A handle is an index **plus a generation counter**. After `Destroy()`, the slot can be reused by a
later `Create` — and the old handle will *not* match it, because the generation moved. `IsValid`
returns false and every setter fails rather than silently editing someone else's material.

That is the whole reason the generation exists. If you cache a `PbrMaterial` across frames, check
`IsValid` rather than assuming.

### Capability reporting

`Pbr.StatusOf(PbrFeature)` returns `Ready`, `NotImplemented` or `Unsupported`, and
`Pbr.StatusTextOf` explains why in words. Ask before setting: a feature the hardware or the renderer
cannot do reports honestly instead of pretending, and assigning it leaves the value alone.

---

## 4. `Voxi` — renderer settings

A process-wide singleton, not instances.

```csharp
if (Voxi.IsAvailable(VoxiFeature.GlobalIllumination))
    Voxi.GlobalIllumination = VoxiQuality.High;
```

- **Quality**: `GlobalIllumination`, `RayTracing`, `PathTracing` — `Off/Low/Medium/High/Epic`.
- **GI tuning**: `VoxelResolution`, `GiIntensity`, `GiMaxDistance`.
- **Anti-aliasing**: `Msaa`, `SupportedMsaaCounts`.
- **Geometry path**: `MeshShaders`.
- **Device caps, read-only**: `RayTracingTier`, `MaxMsaa`, `MeshShaderTier`, `ShaderModel`.

Same honest-status contract as `Pbr`: `StatusOf` / `StatusTextOf` / `IsAvailable`.

---

## 5. What you cannot do yet

Read this before planning anything. The gap is large and it is not hidden.

**`Aver.Scripting`'s own surface — the one this page documents, `AverBehaviour` plus `Pbr`/`Voxi` —
has no scene API.** An `AverBehaviour` cannot create, find, move, parent or destroy an object, and
cannot *assign* a material to one either — `Pbr` lets you author materials, but nothing in this
assembly binds one to something in the world.

**This used to be explained by "`Aver.Scene` is designed but unbuilt (`modules/scene/` is a
README)" — that stopped being true.** `modules/scene/` now has a real `CMakeLists.txt`, five source
files and a live C ABI (`scene_abi.h`): entities, component pools, transform/hierarchy propagation
and typed field get/set are implemented and exercised by `SceneTest.exe` (`modules/scene/README.md`'s
own status line, `docs/STATUS.md` §9 corrects the same stale claim elsewhere in that document too).
A separate, newer C# surface — `Aver.Framework` (actor classes, `Actors.Spawn`/`Entity`/`SetParent`/
`DestroyEntity`, and Aver Node graphs built on the same layer) — reaches that world today; see
**[`VISUAL_SCRIPTING.md`](VISUAL_SCRIPTING.md)** and **[`AVER_NODE_NODES.md`](AVER_NODE_NODES.md)**.
What remains true, and is this page's actual subject: `Aver.Scripting`'s `AverBehaviour` is not that
surface, references neither `Aver.Scene` nor `Aver.Framework`, and gets no scene binding by simply
existing alongside them — the gap this section describes is `AverBehaviour`'s, not the engine's.

Also absent from `Aver.Scripting` itself: input, physics, audio, asset loading, and any form of
coroutine or timer beyond counting `dt` yourself.

**What scripting genuinely does today:** it runs real managed code in the engine's process, on the
frame loop, against the editor's own live material library and renderer settings — and it hot-reloads.
That is a working foundation, not a demo. It is simply not yet a gameplay layer.

---

## 6. Why in-process hosting matters

Earlier, a standalone C# process could P/Invoke `Aver.Render.Voxi.dll` — but it loaded its **own
copy**, so it got its own settings singleton and empty device caps. It could not affect the editor.

The CLR is now hosted **inside the engine process** (`modules/scripting`, via `nethost`/`hostfxr`).
A P/Invoke from a hosted assembly resolves to the module the editor **has already loaded**, so it is
the same instance with the same state. That single change is what turns these bindings from a
read-only curiosity into something that drives the running editor.

---

## 7. See also

- `scripting/csharp/Aver.Scripting/` — the API, with XML docs
- `modules/scripting/README.md` — how the host works
- `modules/render.pbr/README.md` — the material model and the two-target split
- `docs/PROJECTS.md` — engine ⟂ project, and where code goes
- `docs/STATUS.md` — current state, open work, and the `Aver.Scene` design
