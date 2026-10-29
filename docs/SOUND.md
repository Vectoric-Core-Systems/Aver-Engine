# Aver Sound — procedural audio, as a graph

A sound authored as a node graph rather than recorded as a file: an oscillator into a filter into an
envelope, saved as `.ocsnd`, rendered to PCM and played by the ordinary mixer.

This document describes what is **built and tested**, and says plainly where it stops. `docs/AUDIO.md`
beside it is the audio *plan*; this one is not a plan.

---

## 1. What exists

| Piece | Where | Checked by |
| --- | --- | --- |
| The `.ocsnd` format | `modules/formats/{include,src}/aver/formats/OcSound.*` | `tests/sound` (round-trip, validation) |
| The evaluator | `modules/sound/src/Synth.cpp` — `renderSound()` | `SynthTest`, 36 checks |
| The C seam | `aver_audio_load_pcm` in `modules/audio.abi` | `AudioTest` |
| The C# surface | `Audio.LoadPcm` in `scripting/csharp/Aver.Framework/Audio.cs` | — |
| The editor tab | `sandbox/src/SoundEditor.*` | `SoundEditorTest`, 137 checks |
| Creating one | Content Browser → **+ Add** → **New Sound Graph** | — |

Ten node kinds in version 1: `Sine`, `Saw`, `Square`, `Noise`, `Const` (sources); `Gain`, `LowPass`,
`Adsr` (shapers); `Mix`, `Multiply` (combiners). Version 2 adds eleven (section 7): `Parameter`, `Lfo`,
`HighPass`, `BandPass`, `Random`, `Crossfade`, `WavePlayer`, `Delay`, `Clamp`, `MapRange`,
`EnvelopeFollower`, and a frequency / cutoff modulation input on the oscillators and `LowPass`. A graph
also plays in **real time** on the audio thread, answering live inputs and a note-off
(`docs/AUDIO.md` section 13).

---

## 2. Rendered offline, and what that costs

> **Updated.** The graph can now ALSO be evaluated per block on the audio thread (`Mixer::playGraph`,
> `docs/AUDIO.md` section 13), with the node state pre-allocated at play time, named live inputs and a
> note-off. `renderSound()` below is the same evaluator run once over the whole length, so a graph with
> no live input gives identical samples either way. What follows describes the offline path and the
> reasoning that led to the real-time one; the "cannot respond to a live parameter" cost applies to the
> offline path only.

`renderSound()` turns a graph into a whole buffer up front. The mixer then plays it like any other
sound. The alternative — evaluating the graph per block on the audio thread, which is what Unreal's
MetaSounds does — is a materially harder problem here, because `Mixer.hpp`'s contract is that
`mix()` **must not allocate, lock, block, log or call managed code**. A real-time graph needs every
node's state pre-allocated and its evaluation free of all four.

**What it buys:** a footstep synthesised fresh per step, a different seed each time, no `.wav` files.

**What it costs, stated plainly rather than discovered later:** a sound cannot respond to a live
parameter once it has started. Modulation that varies *during* a note has to be authored into the
graph (an `Adsr`, an LFO through `Multiply`), not driven from gameplay. **Per-play** variation is a
render parameter; **per-instant** variation is not available until a real-time path exists. The
evaluator is deliberately shaped so that path could reuse it — see `RenderParams::seed` and the
per-node state split in `Synth.cpp`.

---

## 3. Sources before consumers

Every link runs from a **lower** node index to a **higher** one, and `OcSoundData::valid()` enforces
exactly that. One comparison buys three things:

- evaluation is a forward walk with **no topological sort** at render time;
- a cycle is **unrepresentable**, not merely rejected;
- it is the same shape `.ocskel` and `.ocbt` already use for "parents before children".

The **editor never makes the author obey this by hand.** `snAddLink` re-sorts the whole node array
topologically when a link needs it, so connecting a node to something earlier in the list just
works; indices move, and the functions that move them return where things went. The sort picks the
smallest ready index first, which makes it the identity permutation on a graph that already holds —
so it only ever moves what had to move. A **cycle** is the one thing genuinely refused, because no
ordering can satisfy it.

---

## 4. Inputs sum

The evaluator does `in0[l.toNode] += v`. Two oscillators into one input is a legal graph that means
something, so the editor shows an input as a **list** of sources and says `(2 sources, summed)`
rather than pretending an input has one. This is also why the editor writes links in a canonical
order: float addition is not associative, so fixing the order fixes the render.

---

## 5. The editor tab

A **list, not a canvas** — deliberately different from the `.ocgraph` node editor even though both
edit a DAG. A `.ocsnd` graph is a handful of nodes deep and one or two wide, and `OcSoundData` stores
no node positions, so a canvas would spend its whole budget on pan/zoom/layout state the format
would then have to grow a chunk for.

- Opens on the **output** node, not node 0 — node 0 is always a source, so opening there always
  lands on a bare oscillator with nothing to look at.
- **Changing a node's kind resets its parameters.** A slot means something different for every kind;
  a `LowPass` cutoff of 2000 becoming an `Adsr` attack of 2000 *seconds* is a graph that renders
  silence from a number nobody typed. Undo covers the rarer case.
- **Preview** renders the graph and plays it, advancing the seed each time — so pressing it twice on
  a graph containing `Noise` gives two different renders. That is the whole argument for a
  procedural sound over a `.wav`, and it should be audible from the editor.
- The **waveform** is drawn from the real render and works with no sound card at all. It takes the
  **peak** of each bucket, not every Nth sample: a stride would alias a 440 Hz tone into whatever
  beat frequency it makes with the stride and draw a shape the sound does not have.

---

## 6. Where this stops

**The real-time route now closes most of this gap.** The `Play Sound Graph`, `Play Sound Graph At`,
`Set Sound Graph Input` and `Note Off Sound Graph` nodes (family `ProceduralSound`) play a `.ocsnd` on the
audio thread from a visual script, and `Audio.PlayGraph` does from C#. What remains is the **offline**
route from a script: `PlaySound` still takes a cooked `.ocaudio` or an importable file and does not
render a `.ocsnd`; render-per-play with a fresh seed (and the caching decision below) is still only
reachable from C# with `Audio.LoadPcm`. The original text of the gap follows.

**A graph author cannot yet play a `.ocsnd` from a visual script** through the offline route. This is the
one real gap there, and it is worth stating precisely rather than leaving to be discovered:

- The Audio nodes (`PlaySound`, `PlaySoundAt`, …) take a path and hand it to `aver_audio_load`,
  which reads cooked `.ocaudio` or imports a `.wav`/`.ogg`. It does **not** know about `.ocsnd`.
- The route that *does* work today is `Audio.LoadPcm` from C#, which means rendering the graph
  yourself and handing over the buffer.

Closing it needs a decision, not just code, and the decision is the interesting part: **does playing
a `.ocsnd` re-render per play with a fresh seed, or render once and cache?** Caching makes it a
`.wav` with extra steps and throws away the only thing the format is for. Re-rendering per play puts
a synthesis pass on whichever thread calls `PlaySound`, which is fine for a 0.6 s footstep and not
fine for a 30 s drone. A per-asset flag is the obvious answer and is not yet written.

Also absent, and smaller:

- **No importer writes a `.ocsnd`.** They are authored, not converted — which is correct, but it
  means the New Sound Graph menu item is the only way one comes into existence.
- **Mono only.** `renderSound` produces one channel; the mixer pans it in 3D, so this matters only
  for a sound meant to be stereo at source.
- **No `.ocsnd` cook step.** The graph is stored, not the audio, and it is re-rendered on every use.
  That is cheap for the node counts involved and has not been measured at scale.

---

## 7. Version 2: the real-time node kinds

`.ocsnd` content version 2 (`OcSound.hpp`) adds node kinds 10 to 20 and a name chunk `SNAM` (a node's
name: the live input a `Parameter` answers to, the asset a `WavePlayer` plays). **A graph that uses nothing
new is still written as version 1, byte for byte as before**, so older tools open it; a graph with a new
kind, a name, or a link into an input version 1 did not have (the oscillators' frequency offset, the
filters' cutoff offset) is version 2. The reader takes both and refuses a newer one. Kinds, inputs and
parameters are tabulated in `docs/AUDIO.md` section 13; the `OcSoundNodeKind` comments in `OcSound.hpp`
are the one-line version.

- **Inputs add, and read 0 unconnected.** `Sine`/`Saw`/`Square` input 0 is a frequency offset in Hz added
  to `params[0]`; `LowPass` input 1 is a cutoff offset. That is how a `Parameter` drives pitch or tone.
- **Offline, a `Parameter` is its default** unless `RenderParams::inputs` names a value (the editor's
  Preview uses the defaults); a `WavePlayer` needs `RenderParams::resolveWave` or it is silent.
- **Random is per play**: drawn from the render seed per node, so two seeds differ and one seed repeats.
- **Limits** (refused at compile with a reason): 256 nodes, 32 distinct Parameter names, 8 distinct
  WavePlayer assets, 30 s of delay memory in total (10 s per node).
- **The editor palette is data.** `snKindName`, `snParamLabel`, `snInputLabel`, `snDefaultParams` and
  `snNameLabel` in `SoundEditor.cpp` name the new kinds; the Kind combo and Add menu list every kind up to
  `kOcSoundLastKind`. A Parameter or WavePlayer node shows a name box. Preview is unchanged and opens no
  device in any test.
