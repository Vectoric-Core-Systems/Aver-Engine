# Aver.Audio  (`modules/audio`)

The mixer. Voices in, one interleaved float buffer out. **It never touches a device.**

See [docs/AUDIO.md](../../docs/AUDIO.md) for the plan this implements.

> **This supersedes the skeleton that stood here**, which read: *"[opt] Procedural audio graph
> (Metasound successor) on miniaudio (MIT): runtime node graph, live params on playing sources."*
> The node graph is still a reasonable eventual shape and nothing here forecloses it. miniaudio is
> not used: it is a fine library and permissively licensed, but it is also a dependency this
> repository does not yet vendor, and a WASAPI backend needs nothing that is not already installed.
> That is the whole of the reason, and if miniaudio is later vendored it becomes a second backend
> rather than a rewrite — the mixer is Core-only precisely so a backend is replaceable.

## Why it is Core-only

`mix()` fills a buffer the caller supplies; a backend hands that buffer to hardware. Same split as
`Aver.UI`, and audio needs it *more*, not less.

A renderer can be checked by looking at it. Audio cannot. A pan law that dips 3 dB in the middle and
one that does not are the same arithmetic to a reader and the same silence to a screenshot. An
attenuation curve still 2% audible at its own cutoff sounds fine until you walk past the boundary. A
loop point that drifts a fraction of a frame per pass takes minutes to become audible. So the whole
mixer runs headlessly in `tests/audio`, with 70 assertions and no sound card.

That test has already earned its place: it caught a stolen voice failing to bump its generation,
which meant the stolen handle still matched its slot and its owner went on silently driving the voice
that had replaced it.

## The real-time rule

**`mix()` never allocates, locks, blocks, logs, or calls managed code.** Not "tries not to" — one
`malloc` on an audio callback is a click in somebody's headphones, on a machine you do not own.

Everything else follows from that one rule:

- **The voice pool is fixed**, sized at `init`. Full means steal, never grow.
- **Per-slot atomics, not a command queue.** `docs/AUDIO.md` sketched an SPSC ring; a ring is right
  for *ordered* commands, and volume, pitch, position and stop are not ordered with respect to each
  other — latest value wins. A ring would add a bounded queue that can fill, for state with no need
  of one. The deviation from the plan is deliberate and recorded.
- **Voice handles are generational.** A slot comes round again in seconds at 64 voices; a stale
  handle must read dead rather than name somebody else's sound.
- **Sound data is freed on the game thread**, behind a reference count, the way the RHI defers a
  resource destroy behind the GPU fence. `removeSound` retires, `collect` reclaims, the audio thread
  frees nothing.
- **Stopping ramps**, it does not cut. A waveform cut mid-cycle is a step discontinuity, and a step
  discontinuity is a click.

## What it does

Resampling and pitch (one linear-interpolated cursor does both), looping with sample-exact seams,
constant-power panning, inverse-distance attenuation reaching exactly 1.0 at the inner radius and
exactly 0.0 at the outer, four buses plus a master, voice stealing by quietest-then-oldest, and hard
clipping.

Hard clipping deliberately, not a limiter: a limiter pulls down everything below the threshold too,
so an overloud mix would sound *quieter* rather than distorted and nobody would notice it was wrong.
Clipping is meant to be unpleasant.

## Units

The engine's: centimetres, **+X forward, +Y right, +Z up, left-handed**. Named explicitly because a
handedness error puts every sound on the wrong side of the player's head, which reads as a bug in the
pan law rather than as a sign three files away. `tests/audio` asserts both sides.

## What it does not do

No reverb, no occlusion, no HRTF, no DSP chain, and no streaming — every sound is resident. Those are
`docs/AUDIO.md` §5 and §8, and none is worth having before something makes a noise.
