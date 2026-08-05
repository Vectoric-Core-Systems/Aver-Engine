# Changelog

Every entry here is a public release — a tag on
[Vectoric-Core-Systems/Aver-Engine-download](https://github.com/Vectoric-Core-Systems/Aver-Engine-download/releases).
Format loosely follows [Keep a Changelog](https://keepachangelog.com/); versioning is
`MAJOR.MINOR.PATCH` while the engine is pre-1.0 and every release is a beta. Commit hashes in
parentheses are short refs into this repository — `git show <hash>` for the full reasoning, which is
usually longer and more useful than the line here. `docs/BUGS.md` carries the full defect list with
triggering inputs for anything summarised below.

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
