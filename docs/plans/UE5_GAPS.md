# Aver vs Unreal Engine 5: what is still missing (2026-11-09)

Surveyed at `08-gdint` after `64ec4ab7`, read-only, by grep for callers and UI rather than by file names. Last
updated in `docs/plans/UE_FEATURE_PARITY_PLAN.md` (2026-10-11); much of that plan has landed since.

**Statuses:**
- **Missing:** no code found.
- **Partial:** the code exists but has a clear gap.
- **Unconnected:** the code exists, but nothing calls it.

## Missing or partial, by area

**Rendering**

| UE5 feature | Status |
|---|---|
| Texture streaming / virtual textures | Missing. Only a design exists (`wave4/TEXTURE_STREAMING.md`). |
| Material instances and material functions | Partial. `.ocmati` chains, scalar/vector graph parameters and inlined functions exist (`MATERIALS.md`). Parameters compile as literals per value set, not a cbuffer; no editor panel. |
| Virtual Shadow Maps | Missing. There are cascades and RT shadows; the Vulkan cascades are unfinished. |
| HLOD | Missing. Per-mesh LOD exists. |
| Virtualised geometry | Partial. Clusters, the LOD DAG and a mesh-shader path exist. Per-cluster HiZ culling, cluster streaming and a compute fallback do not. |
| Lighting without DXR | Missing. Every GI path assumes a GPU with ray tracing. |
| Volumetric clouds | Partial. One slab; no layers or cloud shadows. |
| Baked lightmaps | Partial. The format and unwrap exist; there is no bake pipeline. |
| Split-screen | Missing. |
| DLSS / FSR 2-3 / XeSS | Missing. AverSR, FSR1, NeuRAA and NeuraFI exist. |
| First-person viewmodel layer | Removed on 2026-11-08 by the owner's choice. The restore plan is `FIRST_PERSON.md`. |

**Animation and physics**

| UE5 feature | Status |
|---|---|
| Physical animation (powered ragdoll, hit reactions) | Missing. |
| Chaos-style pre-fractured destruction (Voronoi) | Missing. Soft-body tearing exists. |

**Gameplay, network and online**

| UE5 feature | Status |
|---|---|
| Replication, RPCs, client prediction | Partial. Part 1 is built and tested headlessly (`Aver.Framework/Net`: server/client sessions, spawn/destroy, `[Replicated]` delta send, `[Rpc]` Server/Client/Multicast, ownership and authority checks, relevance), see [../NETWORKING.md](../NETWORKING.md). Not wired into `Runtime` or the editor, no actor binding, no prediction (plan written). |
| Sessions, lobby, matchmaking, Steam/EOS, achievements | Missing. |
| GameState / PlayerState split | Missing. GameMode, Pawn and PlayerController exist. |
| Mass-style data-oriented entities | Partial. GPU ORCA crowds only. |

**Audio**

| UE5 feature | Status |
|---|---|
| HRTF / binaural spatialisation | Missing. |

**World building**

| UE5 feature | Status |
|---|---|
| World Partition | Partial. Distance-cell streaming exists. HLOD, data layers and per-cell editor loading do not. |
| PCG node-graph editor | Missing. F# PCG and a GPU density path exist. |
| Modeling mode / Geometry Script | Partial. CSG only. |

**Editor**

| UE5 feature | Status |
|---|---|
| Timeline profiler (Insights) | Partial. `trace <frames> [file.json]` records CPU scopes (AVER_TRACE_SCOPE in aver/core/TraceCapture.hpp) and the averaged GPU passes to a Chrome trace file for chrome://tracing or Perfetto. Only the frame phases in Engine.cpp are scoped so far; no in-editor viewer, no true GPU timestamps, no editor menu entry. |
| Asset data validation | Partial. `AverAssetC validate <project>`, the `validate` console command and Content Browser > Validate Assets run five rules (missing references, ocmat header and GRAPHREF domain, ocgraph links, mesh slots, texture sizes; aver/formats/AssetValidate.hpp). Not yet: rules for sounds, animations, prefabs, level-instance and landscape assets, a save-time hook, a dockable results panel. |
| Live Coding / state-preserving hot reload | Partial and unverified. |
| Screen-reader bridge (UIA) | Missing. |

**Shipping**

| UE5 feature | Status |
|---|---|
| Cooking and pak files | Partial. A staged loose-file player exists. `.ocpak` has no reader and no compression codec. |
| PSO precaching | Partial. A DXIL cache exists. |
| Consoles and mobile | Missing. |
| Linux and macOS | Present on paper. CI is their only compiler. |

## Top 10 for shipping a converted Godot 3D shooter

| # | Gap | Size |
|---|---|---|
| 1 | Replication, RPCs and prediction, wired to the existing transport and to Fabric's Godot multiplayer mapping | XL |
| 2 | First-person viewmodel layer: separate FOV, no clipping into walls (removed for now) | M |
| 3 | `.ocpak` reader, compression and a cook step | L |
| 4 | Texture streaming within a VRAM budget | L |
| 5 | Material instances and functions | M |
| 6 | Virtualised geometry: HiZ cluster culling, compute fallback and Vulkan parity | L |
| 7 | Shadows without ray tracing: finish the Vulkan cascades, then a clipmap or VSM | L |
| 8 | Combat feel: physical-animation hit reactions and pre-fractured destruction | M to L |
| 9 | Online services: sessions, lobby, Steam or EOS | L |
| 10 | World Partition: HLOD and data layers; split-screen | L / M |

Runners-up: HRTF, asset validation, a timeline profiler, a PCG graph editor, GameState and PlayerState,
cloud layers, and DLSS or FSR 3.

**Not verified at runtime**, though the docs mark them as built:
- hair;
- vehicles;
- AI perception;
- cloth tuning;
- blending between post-process volumes;
- breakpoints in the graph debugger;
- navmesh dynamic obstacles.
