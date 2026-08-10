# Changelog

Every entry here is a public release — a tag on
[Vectoric-Core-Systems/Aver-Engine-download](https://github.com/Vectoric-Core-Systems/Aver-Engine-download/releases).
Format loosely follows [Keep a Changelog](https://keepachangelog.com/); versioning is
`MAJOR.MINOR.PATCH` while the engine is pre-1.0 and every release is a beta. Commit hashes in
parentheses are short refs into this repository — `git show <hash>` for the full reasoning, which is
usually longer and more useful than the line here. `docs/BUGS.md` carries the full defect list with
triggering inputs for anything summarised below.

## [0.2.0] — 2026-08-11

### Performance

- **The editor is roughly 30% faster on a streamed scene, and the stutter is gone.** Measured on the
  Electric Dreams demo, same camera, 870 frames, vsync off: `109.6 ms median / 215.9 ms p90` became
  `76.5 ms / 77.4 ms`. p90 now equals the median, which is the real change — frames are even instead
  of hitching every fourth one. Three causes, each of which turned out to be a feature that had never
  actually run:
  - **LOD selection was off by default.** Every instance drew LOD 0 at every distance — the exact
    thing the Cook builds a ladder to avoid. Now on; `--no-lod-select` restores the old behaviour.
    This alone is 26 ms of the 33. (`d5f2829`)
  - **The GI rebuild gate had never once passed.** `rhi::SkyAtmosphere::cloudTime` is an accumulating
    clock and sits inside the struct the gate compares byte-for-byte, so it reported "the sky changed"
    on every tick and rebuilt the whole 128³ volume forever. 77% of rebuilds are now skipped, which is
    the entire p90 fix. Found by making the rejection say WHICH BYTE differed rather than that
    something did. (`3b42099`)
  - **The depth-only passes had no LOD at all.** Four shadow cascades, the GI shadow map and
    voxelisation each drew full-detail meshes — six times a frame. They now draw a coarser proxy,
    resolved through a function pointer so the renderer still does not know Trifactor exists.
    (`d5f2829`)

### Fixed

- **Every project a shipped engine created was born unable to build.** On a Launcher install a
  brand-new project failed its first compile with `FS0039: The namespace 'Pcg' is not defined`, in a
  file the author had never opened. `payload.allowlist` shipped `scripting/csharp/**` — with a long
  comment explaining why that is load-bearing — and never `scripting/fsharp/**`, while
  `ProjectScaffold` writes an F# starter that references `Aver.Pcg`. Every guard did its local job:
  the reference walk correctly returned empty, the `<ProjectReference>` was correctly omitted, and
  `Sky.fs` was written anyway still saying `open Aver.Pcg`. Fixed at both ends — the F# tree ships,
  and the scaffold no longer writes F# it cannot reference. (`0a9c959`)
- **The GPU per-cluster mesh-shader path had never executed.** `onInit` resolved the enabling flag
  *after* `applyProject`, which is what builds the GPU cluster buffers and creates the pipeline, and
  `ensureLodMeshPipeline` latched "already tried" before testing the flag — so one early call burned
  the only attempt the process would ever make. Per-cluster frustum and cone culling have therefore
  never run in this editor. Fixed, but left **off by default**: with the path genuinely live it is 22%
  faster and renders every plant as a black silhouette, because `PSClusterMain` never receives the
  per-draw material binding. The feature is unfinished rather than merely unreachable, and the log
  line now says so. (`4434ef6`)

### Added

- **Project upgrades.** Projects record the engine version that made them (`CREATEDWITH`), which is
  distinct from `ENGINE`'s minimum-version floor. Opening a project made by an older *series* raises a
  prompt: **Upgrade a Copy** (the default — copies to a numbered sibling folder and never touches the
  original), **Convert in Place** (type the project's name to confirm), or **Cancel**. Only
  major.minor gate: `0.1.0 → 0.1.7` opens with no prompt, `0.1.x → 0.2.x` migrates. A project that
  records no version is adopted as current and stamped silently. (`7ccf87f`, `4fd31c3`, `e60926c`)
- **`Aver.Upgrade`**, a migration *chain* rather than a one-shot fix. Each step declares the series it
  moves from and to, and a path is planned through them, so a project from any past version reaches
  the current one and each step only ever knows its own boundary. Refusing is a feature: a project
  from a newer series fails with a reason rather than inventing a downgrade, and a gap in the step
  table is reported rather than skipped. (`7ccf87f`)
- **The start screen scans the projects folder**, not only the recent list — "recent" is per-machine
  state in AppData and projects are not, so a project copied from another machine or surviving a
  reinstall now appears. Each card carries its engine version in the corner; older ones are
  highlighted. (`e60926c`)
- **Editions.** `AVER_EDITION` selects a module preset the Launcher can install — `standard`, `slim`
  (no voxel GI, geometry cooking, deformation or upscaler) or `full`. The distribution side has been
  edition-aware since it was written and had only ever cut one; this is the missing half that says
  what an edition contains. `standard` reproduces the previous defaults value for value.
  (`0edb5ec`)
- **Per-pass GPU timestamps** in the D3D12 backend, riding the markers that already bracket every
  pass and read two frames late so the measurement never creates the stall it reports. The frame now
  attributes to 0.2 ms unmarked. This is what finally ended five rounds of guessing at where the
  frame went. (`1fa6cd1`)

### Tooling

- `tools/RelodTool` reports the LOD ladder Trifactor would build today for an already-cooked
  `.ocmesh`. The demo ships 69 cooked meshes and no source assets, so a simplifier change could not
  previously be evaluated at all. It reports only — rebuilding a ladder in place is a lossy rewrite of
  somebody's art. It immediately killed two cook changes that looked right: `meshopt_SimplifyPrune`
  (0.5% on the worst mesh) and lifting `target_error` to `FLT_MAX` (does not terminate). (`57dab61`)
- Both gate baselines re-recorded: 153 values moved in each config, none added or dropped, verified
  green against the binaries they were recorded from over 328 checks. (`839c24b`)

## [0.1.2] — 2026-08-05

### Fixed

- **Fog and the sky dome's lower hemisphere now blend toward the true ray's atmospheric colour at
  large scale, smoothly, instead of a flat zenith reference at every distance.** 0.1.1's fix was
  correct for the bug it fixed — a bright band and a brown patch at the horizon — but it was a clean
  break: fog always returned the same flat colour regardless of distance or how much real atmosphere
  actually lay in the way. Now blends toward the true view ray's colour, weighted by the same
  aerial-perspective transmittance already computed for ordinary surfaces — continuous, no threshold.
  (`bac1d87`)
- **The fog's own colour now responds to the sun at the range fog is actually visible at, not only at
  kilometres.** The fix above alone wasn't enough: real Rayleigh/Mie extinction only moves visibly
  over tens of kilometres, while a level's authored fog density is normally tuned to read as fog
  within a few hundred metres to a couple of kilometres — so at every distance fog actually does
  anything, the previous fix's blend sat at its unmoved end, and fog never visibly reddened at
  sunset. Fixed by asking a different question: fog scatters whatever light is actually reaching it,
  which depends on how far the sun's own light travelled to get here, not on how far the camera is
  looking. Measured: the same foggy scene reads warm gold with the sun near the horizon and cool
  neutral grey with it high overhead, at ordinary gameplay range. (`d24c979`)

### Tooling

- `scripts/publish-release.ps1`'s feed-resolve check was misdiagnosing its own bug as a GitHub
  propagation delay across two commits (`061a9e2` widened a retry window that didn't need widening)
  before the real cause was found: GitHub serves the feed's `index.json` as
  `application/octet-stream`, and `Invoke-WebRequest` sometimes hands back the response as a raw
  byte array instead of decoded text depending on that content type. Piping bytes into
  `ConvertFrom-Json` doesn't throw — it silently parses nonsense, reading exactly like "the release
  isn't live yet" when it always was. Fixed by decoding explicitly. (`c712e1f`)

## [0.1.1] — 2026-08-04

### Fixed

- **Fog and the sky dome's lower hemisphere, under a physical sky, now derive their colour from the
  atmosphere itself.** They used to come from a separately-authored gradient and a raw, unfiltered
  sun exposure — disagreeing with the physical Rayleigh/Mie model the dome is actually drawn with, so
  ground haze and the horizon read as a visibly different blue than the sky directly overhead. This is
  what Unreal's "Sky Atmosphere affects fog inscattering colour" does. (`6cf7460`, `c28d125`,
  `0a18d23`)
- **A float32 precision bug in the sky's ray/planet intersection turned the world below Z = 0 into
  visible speckle and concentric arcs.** The intersection computed `r0² − R²` directly; both terms
  are ~4×10⁷ at Earth scale while the quantity actually needed is a few km² near ground level, so the
  subtraction was mostly rounding error. Rewritten as a factored form, `(r0−R)(r0+R)`, that keeps its
  precision at any altitude. Camera altitude is also now floored at a metre rather than zero, since a
  camera exactly *on* the planet surface degenerated the same intersection a different way. (`01c141c`)
- **The dome's ground disc, under a physical sky, no longer renders as a flat lit patch that
  disagreed with the sky around it.** It now returns the atmosphere's own zenith colour, attenuated
  by transmittance over the distance to where the ground would have been — continuous with the
  horizon on one side and the fog's colour on the other, no seam. (`01c141c`)
- **Neither the packaged game nor the editor's Play-In-Editor session adds an invisible ground plane
  at Z = 0 anymore.** Every level used to get one whether it authored a floor or not, so a pit, a
  chasm, or open water below a level's own floor silently caught the player at the same height a
  level with nothing below it would. A level's own placements now supply all collision. (`6cf7460`)
- **The editor now draws the clouds a level actually asked for.** A project's sky field
  (`PCGVOLUME name Sky`) already reached the packaged game; the editor only ever carried the record
  through a save without reading it, so a brand-new project — scaffolded with exactly that record —
  opened onto a bare gradient in the editor with no clouds until the checkbox was found by hand.
  (`0a18d23`)
- **Saving a level in the editor deleted every `PCGVOLUME` record it carried**, including the sky
  field a new project is scaffolded with and the one the forest generator writes — silent data loss
  on the two levels the engine itself produces. (`662eac5`)
- **Actor editor:** the rotate gizmo turned about a different axis than the one dragged whenever the
  object already had non-zero rotation (`716a0cd`); the scale gizmo had the same defect for scale
  (`adf0e30`); saving a multi-class actor file corrupted every class after the first, because each
  class's byte-offset edits were applied against text a previous class's edit had already resized
  (`adf0e30`); mesh paths and materials containing a backslash or quote wrote invalid C# escapes back
  to disk (`adf0e30`); the actor preview's colour target was never cleared between selections, so an
  actor with holes left a trail of every earlier frame (`adf0e30`).
- New projects now open on SkyForge's own sky by default — the template previously carried Mie
  scattering strong enough to visibly diverge from the sky the engine is actually tuned against — and
  stop incorrectly reporting themselves as out of date. (`e33f9bc`)

### Added

- **Escape now stops Play-In-Editor**, the same action as clicking Stop.
- **A project's sky can be authored as an F# PCG graph the project owns**, instead of only as literal
  numbers typed into a level file — three additive entry points on the framework ABI (minor version
  1 → 2), read once a frame; a project with no sky script renders exactly what its level authored.
  (`e09691a`)
- `IRenderContext::clearColor`, needed to fix the actor-preview trail above; one production
  implementation (D3D12) and two test-mock implementations. (`adf0e30`)

### Tooling

- `scripts/publish-release.ps1` — packs a staged payload, verifies it, installs it through the real
  Aver Launcher's own code (not just the publisher checking its own manifest) and boots the result
  headlessly, then uploads. Rebuilds `averdist` and `averlauncher-cli` fresh on every run rather than
  trusting whatever is already built, after a stale binary was caught silently producing an obsolete
  pack layout.
- Gate baselines re-recorded against this tree (Debug and Release, all nine configurations). 12 of the
  18 baseline probes move by exactly +1 in one or two colour channels — the direct, measured
  consequence of the fog/sky fix above, confirmed by re-gating the pre-fix shader mid-development:
  all 18 passed unchanged there. Nothing else moved. (`cc84643`)

## [0.1.0] — 2026-08-03

First public build. The editor, the D3D12 renderer (voxel-cone-traced GI, cascaded shadow maps, DXR
inline ray-traced sun shadows, a physical sky), the scene and actor framework, physics, audio, the
C#/F# scripting layer with hot reload, and the asset importers.

### Fixed

Twenty-four verified defects (see `docs/BUGS.md` for the full list with triggering inputs); the ones
most likely to have been visible:

- Near-vertical rotations were saved wrong — the editor's quaternion-to-Euler conversion corrupted
  yaw within 0.26° of vertical, up to a full 180° flip, and level saving went through that path.
- The game runtime rendered every frame with the previous frame's camera — frame constants uploaded
  before the camera was set, so the image lagged the view by a frame while culling used the current one.
- Mouse-look was inverted, and the character's reported look direction disagreed with where the
  camera actually pointed, so aiming and traces disagreed with the picture.
- A realtime audio use-after-free: voice stealing could take a voice while the audio thread was
  rendering it, freeing sound data mid-read.
- Five buffer-overflow checks of the form `offset + length > size` overflowed when both operands came
  from a file — sitting at the front door of every binary asset format.
- Normals ignored non-uniform scale, mis-shading anything scaled unevenly on more than one axis.
- Destroying an actor during a tick silently skipped the next actor for that frame.
- A failed file watch reported itself as healthy, so hot reload stopped without saying so.

### Known limitations

Unchanged since: Windows and D3D12 only. FBX is not supported — use glTF/GLB, OBJ, or ASCII USD
(`.usda`); binary USD (`.usdc`) and `.usdz` are detected and refused by name rather than silently
importing nothing. USD import covers static meshes only — references, payloads, variant sets,
instancing and materials are not composed. No draw-call instancing yet, so dense scenes cost one draw
call per visible object.
