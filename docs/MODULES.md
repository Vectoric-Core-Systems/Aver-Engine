# Modules: the manifest, the dependency rule, the catalog

Every directory under `modules/` carries a `module.json`. That file is the **single source of truth** for
what the module is, which CMake option switches it, what it stands on, and what is lost at run time when
it is off. CMake declares its options from the manifests, the launcher offers them as a checklist, and
nothing else lists them by hand.

```
modules/<dir>/module.json   one per module (70 today)           <- edit these
cmake/modules.catalog.json  all of them, generated              <- python scripts/module-catalog.py --write
cmake/AvModuleManifest.cmake  option(), dependency rule, cross-check
```

## Manifest format (schema 1, UTF-8 JSON)

```json
{
  "schema": 1,
  "name": "Aver.Ragdoll",
  "dir": "ragdoll",
  "displayName": "Ragdolls",
  "description": "A skinned entity's skeleton as a physics ragdoll, blended over the animation.",
  "category": "Physics",
  "option": "AVER_MODULE_RAGDOLL",
  "required": false,
  "default": true,
  "editions": {"standard": true, "slim": true, "full": true},
  "dependsOn": ["Aver.Anim", "Aver.Anim.Scene", "Aver.Core", "Aver.Formats", "Aver.Physics", "Aver.Scene"],
  "consequence": "Skinned entities cannot go limp or be driven as ragdolls."
}
```

| Field | Meaning |
|---|---|
| `name` | The CMake target the module defines (the main one if several). Unique. |
| `dir` | Its folder under `modules/` (must match). |
| `displayName`, `description` | For the launcher; `description` is also the `option()` help text. One sentence. |
| `category` | `Core` `Rendering` `Gameplay` `Physics` `Animation` `Audio` `UI` `Tools` `Platform` `Networking` `Content` |
| `option` | The cache key that enables it (`AVER_MODULE_*`, or `AVER_RHI_*` for a backend), or `null`. |
| `required` | `true` = always built; the launcher shows it locked ON. |
| `default` | Its default when nothing else decides. |
| `editions` | Optional. Per-edition default (`standard` / `slim` / `full`); a missing key falls back to `default`. Presence also marks the module as edition-controlled (see Editions). |
| `dependsOn` | Module **names** whose targets this module links unconditionally (`PUBLIC` and `PRIVATE`, `Aver.*` only). |
| `consequence` | What is lost at run time when it is off; `""` for required modules. |

Three additive fields, not in the original contract; a reader that ignores them is still safe:

| Field | Meaning |
|---|---|
| `optionalDependsOn` | `Aver.*` modules linked only behind `if(TARGET ...)` / `if(AVER_MODULE_...)`. They do **not** take part in the dependency rule (Formats links the PBR material target only when PBR is on). Also where a mutual edge is parked: `Aver.Assets.Gpu` links Formats and Formats links Assets. |
| `derivedFrom` | See below. |
| `skeleton` | `true` for a directory that is only a README (`aero`, `net`, `vehicle`, ...). Nothing is built. |

### Three kinds of module

| Kind | `option` | `required` | `derivedFrom` | Built when |
|---|---|---|---|---|
| required | `null` | `true` | - | always |
| optional | `"AVER_..."` | `false` | - | its option is ON |
| **derived** | `null` | `false` | `["Aver.Scene"]` | every module in `derivedFrom` is ON |

Derived modules have no switch of their own because the build never gave them one: the scene joins
(`anim.scene`, `save`, `prefab`, `synapse.scene`, `synapse.abi`, `audio.scene`) are added exactly when the
scene is, `formats.particles` exactly when particles are, `physics.jolt` with physics, the ImGui backends
with their RHI backend. Making them required would lock the scene on. A derived module never blocks
turning anything off: it simply goes with the modules it derives from.

## Physics build options (`modules/physics.jolt`)

Four CMake options on the Jolt target, each PUBLIC so `Aver.Physics` and the one test that links Jolt directly agree on the class layouts (Jolt checks its configuration string at start-up). `aver_phys_build_flags()` reports what a binary has.

| Option | Default | Effect |
|---|---|---|
| `AVER_PHYSICS_DEBUG_RENDERER` | ON | `JPH_DEBUG_RENDERER` for Debug and Release: the editor's `Show Physics` overlay. Binary size only; OFF makes `aver_phys_debug_available()` answer 0. |
| `AVER_PHYSICS_PROFILER` | ON | `JPH_EXTERNAL_PROFILE` plus `AverProfileHook.cpp` in the Jolt target: Jolt's scopes and the engine's fixed-step stages feed `aver_phys_profile_*`. Off at run time until enabled; a null sink is one relaxed load and a branch per scope. Flip the default to OFF for Release if the idle overhead exceeds 0.3 ms per frame on NeonDistrict (not yet measured). |
| `AVER_PHYSICS_CROSS_PLATFORM` | OFF | Jolt's `CROSS_PLATFORM_DETERMINISTIC`. No cross-platform claim is made until the golden hash test is green on Windows and macOS. |
| `AVER_PHYSICS_DIAGNOSTICS` | OFF | Developer preset, never CI: determinism log, stats tracking and Jolt's internal profiler in place of the external hook (the two are mutually exclusive). |
## The dependency rule

Both CMake and the launcher implement it identically, over `dependsOn` (counting only modules that exist
in the catalog; `optionalDependsOn` is ignored):

1. A module is **ON** if it is required, or its option is ON, or (derived) every `derivedFrom` module is ON.
2. A module can be turned **OFF** only if no ON module depends on it.
3. Turning a module **ON** turns on everything it depends on, transitively (for a derived dependency:
   the modules it derives from).
4. A required module cannot be turned off, so its dependencies cannot be either.
5. The selection becomes `-D<option>=ON|OFF` for every module with a non-null `option`.

At configure time CMake enforces rule 2 itself (`aver_enforce_module_deps`). An enabled module whose
dependency is off is an error naming both and the fix:

```
Aver.Framework (AVER_MODULE_FRAMEWORK=ON) needs Aver.Scene, which is off (AVER_MODULE_SCENE=OFF)
  turn the dependencies on:   -DAVER_MODULE_SCENE=ON
  turn the dependents off:    -DAVER_MODULE_FRAMEWORK=OFF
  or let CMake do the first:  -DAVER_MODULE_AUTO_DEPS=ON
```

`-DAVER_MODULE_AUTO_DEPS=ON` (default OFF) turns the dependencies on instead and prints each one
(`AVER_MODULE_AUTO_DEPS: AVER_MODULE_PBR=ON (Aver.Render.Voxi needs Aver.Render.PBR)`). It wins over an
explicit `-D<dep>=OFF`, and it writes the cache, so the next configure without it succeeds. This replaced
the old silent "forcing AVER_MODULE_X=OFF" guards: dropping a module the caller asked for hid the cause.

## Editions

`-DAVER_EDITION=standard|slim|full` picks each module's default from its `editions` entry (else
`default`). Today only `slim` differs: Voxi, Deform, Trifactor, SR and MCP are off. An explicit
`-DAVER_MODULE_X=...` still beats the edition. Because `option()` never overwrites a cached value,
changing the edition, or changing any `editions` entry, must re-derive the edition-controlled modules
(those with an `editions` object) on the next configure; that is what `AVER_EDITION_REVISION` in
`cmake/AvModuleManifest.cmake` is for. **Bump it in the same commit that edits an `editions` entry.** The
cost is that a re-derive overwrites a deliberate `-D` on that one configure.

## The catalog

`cmake/modules.catalog.json`:

```json
{ "schema": 1, "engineVersion": "0.6.0", "modules": [ ...every built module's manifest, by name... ],
  "planned": [ ...README-only directories, summary fields only... ] }
```

- `python scripts/module-catalog.py --write` regenerates it; `--check` fails when it is stale (this is
  the ctest `ModuleCatalogCheck`); with no flag it only validates.
- Validation: schema and types, unique names and options, every reference resolves, a required module
  depends only on required modules, a derived module depends only on things that are on whenever it is,
  and the dependency graph (`dependsOn` + `derivedFrom`) has no cycle.
- `planned` holds the skeleton directories. They are not in `modules`, so a reader that offers
  everything in `modules` cannot offer something that is not built.
- Commit the catalog with the manifest change. A source checkout is read from `cmake/modules.catalog.json`,
  an installed engine from `<root>/cmake/modules.catalog.json`; when it is absent, read every
  `modules/*/module.json` directly (skip `skeleton: true`).

## How the launcher uses it

1. Load the catalog; list `modules` grouped by `category`. Required modules show locked ON ("always
   included"); derived modules show as included-with their `derivedFrom`, not toggleable.
2. Seed the checkboxes from the chosen edition (`editions[edition]`, else `default`).
3. On a toggle apply rules 2-4 above: refuse (or offer the cascade) when a module has ON dependents;
   switch dependencies on with it. Show `consequence` for anything switched off.
4. Emit `-D<option>=ON|OFF` for each module with an option, plus `-DAVER_EDITION=<edition>`. A prebuilt
   edition's existing option keys are unchanged.

## Configure-time cross-check

After every `add_subdirectory`, `aver_check_module_manifests()` (next to `aver_check_module_dag()`)
fails the configure when a manifest disagrees with reality: the named target is missing while its
option is ON (or present while OFF), or a module's targets link an `Aver.*` module that its
`dependsOn` / `optionalDependsOn` does not list. The message names the manifest to fix. `dependsOn` is
what the dependency rule trusts, so it has to be complete.

## Adding or changing a module

1. Create `modules/<dir>/CMakeLists.txt` and the target as usual; add it to the root `CMakeLists.txt`
   with `add_subdirectory`, guarded by `if(<option>)` if it is optional.
2. Write `modules/<dir>/module.json`. Required: `"option": null, "required": true`. Optional: give it an
   `AVER_MODULE_*` option and **do not** write an `option()` in the root file; CMake declares it. Joined
   to others (only built when they are): `"derivedFrom"`.
3. List every `Aver.*` it links in `dependsOn`; those behind `if(TARGET ...)` in `optionalDependsOn`.
   Guard every consumer of an optional module with `if(TARGET ...)` and `#if AVER_MODULE_X`.
4. `python scripts/module-catalog.py --write`, then configure once: the cross-check tells you what is
   wrong. Run `scripts/module-guard-audit.py`; `scripts/module-matrix.ps1` gets an `<dir>-off` row for
   the new option by itself (it reads the catalog, and also switches off the dependents of a row's module).
5. Changing what an edition contains: edit `editions`, bump `AVER_EDITION_REVISION`.
