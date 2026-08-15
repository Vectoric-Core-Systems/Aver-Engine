# Aver.Particles

CPU-simulated, camera-facing billboard particles: fire, smoke, steam, dust, rain, snow, sparks,
embers, leaves, bubbles, spray, magic, debris, fog wisps -- general-purpose engine machinery in the
same way the mesh renderer is, not a weapon-effects system. Nothing in this module names what an
effect IS; every effect is DATA (`aver::particles::ParticleEffect`), never a code path.

## Pieces

- `scene::CParticleEmitter` (`modules/scene/include/aver/scene/Components.hpp`) -- the ECS-visible
  half: an opaque effect id plus this emitter's own playback clock. Declared as a scene built-in, not
  in this module, so the editor inspector and the C# scripting surface come for free, matching how
  `CMeshRenderer`/`CAnimator` work.
- `ParticleEffect` / `Particle` (`ParticleTypes.hpp`) -- what an effect IS (authored, shared) and what
  a live particle IS (simulated, per-instance). `Particle`'s layout is written for a future GPU
  compute tier; see its own comment for which of `modules/render.pcg`'s choices that tier would
  follow.
- `ParticleEffectLibrary` -- the way an effect id resolves to a `ParticleEffect` at simulate/render
  time: `set()`/`find()`/`clear()`, nothing more. This module never opens a file and never learns
  whether an effect behind an id came from disk or from a composition root's own test content -- see
  DECIDED 3 below for who fills it and how.
- `ParticleSystem` -- the CPU simulation. No RHI. Ticks every `CParticleEmitter`, exactly the split
  `aver::anim::AnimSystem` (no RHI) and `render.skin`'s `SkinnedScene` (RHI) already use.
- `ParticleRenderer` -- an `rhi::IRenderFeature` that draws what `ParticleSystem` produced: one
  `drawIndexed` call per emitter, sorted back-to-front within that call, through
  `IRenderFeature::transparentPass` (this engine's first depth-tested transparent pass). Unlit by
  default; see the GI seam below.

## DECIDED 3 -- `.ocparticle`, and who loads it

The reader/writer lives in `modules/formats.particles` (`Aver.Formats.Particles`), a separate target
for `Aver.Formats.Material`'s exact reason: it parses straight into `particles::ParticleEffect`, a
type outside `Aver.Formats`, so linking it there would put a particle-module dependency on every
consumer of every OTHER format. This module never includes that header and never opens a file.

Both composition roots walk their project's content root for `.ocparticle` files and call
`particleEffects().set(id, effect)` themselves -- `sandbox/src/SandboxApp.cpp`'s
`loadProjectParticleEffects()` and `modules/runtime.game/src/GameContent.cpp`'s
`GameContent::loadProjectParticleEffects()`, both keyed on `fnv1a64(relative path)`, the SAME id
space `CMeshRenderer::mesh`/`CAnimator::clip` already use. Neither composition root's UI or asset
pipeline is visible from here; this module only ever asks `find(id)`.

## DECIDED 4 -- the GI seam

`ParticleRenderer::GiSeam` (`ParticleRenderer.hpp`) -- a `prepare` function pointer (pipeline-build
time) plus a `bind` function pointer (per-frame) plus an opaque `user` pointer, the same shape
`VoxiRenderer::setDepthProxy`/`setTextureResolver` already use. Installed by whoever owns both sides
(`SandboxApp.cpp`, `GameApp.cpp`); this module never includes a Voxi header and links no Voxi
library. Without a seam installed, every particle draws exactly as it did before this decision
existed. `ParticleEffect::receivesGI` (default true) opts an effect out -- an ember or a spark is its
own light source, not a passive receiver of someone else's.

## Not in this module yet (by design)

- GPU simulation: `Particle`'s layout is ready for it (see its own comment on which of
  `modules/render.pcg`'s choices a compute tier would follow); no compute dispatch exists yet.
- A cross-emitter sort: each emitter sorts its own particles back-to-front; inter-emitter draw order
  is emission order. A general depth sort across every emitter in the scene is a separate slice.
