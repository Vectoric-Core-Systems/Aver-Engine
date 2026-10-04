# Aver.Formats  (`modules/formats`)

- **Language:** C++
- **Depends on:** Assets, Core, Platform
- **Status:** implemented — the tree's text and binary asset formats, across six targets

Faithful, tolerant text loaders for the carried-over OpenConstructor formats:

- **`OcBeam.hpp` / `.cpp`** — `.ocbeam` (breakable deformable cage): materials (10–13
  fields), nodes, beams, panels, parts; skips GLB/rig/collision blocks; applies
  SCALE/NORMALIZE; adds the new `OBJECTID`. Verified against Ferrari499P (10-field) and
  Puegot9x8EVOChassis (13-field).
- **`OcMap.hpp` / `.cpp`** — `.ocmap` (world container): identity/surface/env +
  PLACE/DEFORM placements, positions as **f64**, per-placement Object IDs. Faithful to
  the authoritative `OcMap.cs`. Verified against demoworld/openworld.

Everything else this module owns, none of which existed when the list above was written:

| Header | Format | Notes |
|---|---|---|
| `OcMesh.hpp` | `.ocmesh` | the engine's own mesh container |
| `OcAnim.hpp` | skeleton + animation | |
| `OcWorld.hpp` | `.ocworld` | levels |
| `OcProject.hpp` | `.ocproject` | the project manifest. **Now writable** — `writeOcproject` preserves comments and unknown keys, so a manifest survives a round trip through an older editor |
| `Json.hpp` | JSON | a DOM, used by the glTF importer and the Roslyn backend |
| `GltfImport.hpp` | `.gltf` / `.glb` | behind the Content Browser's Import button |
| `Texture.hpp` | images | decode + mip chain |
| `Avr1.hpp` | the container | 64-byte header, chunk directory, CRC32C header, xxHash64 payload |

### The five other targets

`Aver.Formats` itself depends on nothing but Core, Platform and Assets, and that is load-bearing —
it is what lets a test link it alone, and what lets the built-in actor-script scanner run on a
machine with no .NET at all. `ActorScript` used to be one of the reasons `Aver.Formats.Material`
existed; it has since moved into this base target (it parses a generated C# region and touches
neither `pbr::` nor `MaterialDesc`, so gating it behind `AVER_MODULE_PBR` was never right — see
`modules/formats/CMakeLists.txt`'s comment on `src/ActorScript.cpp`). Anything with a heavier
dependency than Core/Platform/Assets is a separate target:

| Target | Holds | Why separate |
|---|---|---|
| `Aver.Formats.Material` | `OcMat`, `MaterialScript` | gated on `AVER_MODULE_PBR`; needs `pbr::MaterialDesc` |
| `Aver.Formats.Audio` | `OcAudio` — the `.ocaudio` container, the WAV reader, and import | pulls in Media Foundation for mp3/m4a/flac/wma |
| `Aver.Formats.Roslyn` | `AverDesign` — the `averdesign` escalation for an actor script the scanner declines | spawns a process |
| `Aver.Formats.Particles` | `OcParticle` — the `.ocparticle` reader/writer | needs `particles::ParticleEffect` (`modules/formats.particles/`) |
| `Aver.Assets.Gpu` | decode-to-GPU | needs the RHI |

Golden test: `tests/formats` → `FormatTest.exe`, plus `OcGraphTest`, `OcInputTest`, `JsonTest`,
`GltfTest`, `ImportTest`, `MeshTest`, `LandscapeTest`, and (target-gated) `TrifactorTest`,
`MaterialTest`, `MaterialGraphTest`, `ActorScriptTest`, `OcParticleTest` and `AverAssetCTest`, plus
the sibling suites `OcAudioTest` (`tests/audio`) and `RoslynTest` (`tests/formats.roslyn`). `.ocaero`
and `.scene` are still to come.

See [docs/formats/DECISIONS.md](../../docs/formats/DECISIONS.md).
