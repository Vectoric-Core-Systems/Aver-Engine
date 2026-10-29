# Aver.Audio  (`modules/audio`)

The mixer. Voices in, one interleaved float buffer out. **It never touches a device.**

See [docs/AUDIO.md](../../docs/AUDIO.md) for the plan this implements.

## Why it is Core-only

`mix()` fills a buffer the caller supplies; a backend hands that buffer to hardware. Same split as
`Aver.UI`, and audio needs it *more*, not less.

A renderer can be checked by looking at it. Audio cannot. A pan law that dips 3 dB in the middle and
one that does not are the same arithmetic to a reader and the same silence to a screenshot. An
attenuation curve still 2% audible at its own cutoff sounds fine until you walk past the boundary. A
loop point that drifts a fraction of a frame per pass takes minutes to become audible. So the whole
mixer runs headlessly in `tests/audio`, with 73 assertions and no sound card.

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

## Streaming, fades, occlusion, reverb

Added later, under the same real-time rule; see [docs/AUDIO_STREAMING.md](../../docs/AUDIO_STREAMING.md).
A **stream voice** reads a ring of decoded frames kept full by a decoder thread (`Stream.hpp`; WAV is
streamed from disk by `WavStream.cpp`, the Media Foundation and `.ocaudio` sources live in
`modules/audio.abi`). An under-run is silence plus a counter, never a stall. Every voice carries a fade
gain ramped on the audio thread (crossfades), an occlusion value (smoothed, low-passed and ducked by
`Dsp.hpp`) and a reverb send; the mixer owns one send reverb (`Reverb.hpp`). The pure parts, the
crossfade curves, the occlusion ray probe and the reverb zone blend, are all asserted in
`tests/audio/AudioStreamTest`. The scene components that drive them are in `modules/audio.scene`, so this
module stays Core-only.

## What it does not do

No measured HRTF data set (the headphone mode below is an analytic head model), no room simulation (the
listener-side reverb, plus the opt-in zone send reverbs below) and no portal propagation (diffraction is an
opt-in one-bend probe). Those are `docs/AUDIO.md` §5, §8 and §15.

## Surround and binaural (`Speakers.hpp`, `Binaural.hpp`)

Opt-in, off by default (a stereo mixer renders the same samples as before, pinned in `tests/audio`). The mixer renders 1, 2, 6 (5.1) or
8 (7.1) channels in the `WAVEFORMATEXTENSIBLE` / SDL order (`L R C LFE Ls Rs`, `L R C LFE Lb Rb Ls Rs`). Positional voices are panned over the
speakers with a pairwise constant-power law (`sum(g*g) == 1` at every angle; a source overhead or inside the head spreads over all of them);
stereo sources, the reverb return and the bus graph are upmixed by a power-preserving matrix (`StereoUpmix::Wrap`, or `Front`); an LFE send
(0 by default = silent) low-passes a mono mix at 120 Hz. `Mixer::setSpeakerRequest(2/6/8)` is what the project wants; the device layer
decides with `chooseMixChannels` from the device's channel count and speaker mask (`OutputInfo::channelMask`) and reopens the device when
the request changes, and `buildDeviceMap` says which mixer channel feeds each device channel. `setBinaural(true)` on a stereo mixer places
positional voices with Woodworth's interaural delay, a Brown-Duda head-shadow filter per ear and a pinna reflection that moves with elevation,
interpolated per sample (no zipper noise, delays slew at most half a sample per sample), all in fixed per-voice state. Projects switch it on
with `AUDIO.SPEAKERS 6|8` and `AUDIO.BINAURAL 1`; the C ABI and `Audio.Speakers` / `Binaural` / `LfeSend` / `Upmix` reach the same settings.
`tests/audio/AudioSurroundTest` runs it offline; `docs/AUDIO.md` section 14 has the details and the headed checks.

## Bus graph, effects, Godot-style players

`BusGraph.hpp`, `Effects.hpp` and `Spatial.hpp` (Fabric G5 S1, `docs/AUDIO.md` section 10): Godot's AudioServer
bus list with sends, mute, solo, bypass and peaks; the AudioEffect set (filters, EQs, delay, chorus,
compressor with sidechain, limiters, distortion, phaser, pitch shift, reverb, panner, stereo enhance, capture,
spectrum analyser); Godot's AudioStreamPlayer3D attenuation, panning, cone, filter and Doppler as pure
functions; pause, start offset, playback position, live (generator) voices. Opt-in: the graph does nothing
until `mixer.graph().setActive(true)`, and a mixer that never uses it renders exactly as before.
`tests/audio` (`AudioBusTest`, `AudioSpatialTest`, `AudioAbiTest`) run all of it offline.

## Device robustness (`OutputDriver.hpp`, `AudioDevice.hpp`)

The mixer still never touches a device, but the policy for what a device does is here too, so a test can drive it. A backend
(`modules/audio.wasapi`, `modules/audio.sdl`) implements `OutputBackend` for one open endpoint (`open`, `start`, `service`, `close`); the
`OutputDriver` owns the loop and every decision, and `AudioDevice` is the render thread around it.

- **Loss and default change.** A lost or invalidated device, a changed system default or a device that asks for nothing for two seconds is
  closed and reopened on the new default; `Mixer::setOutputFormat(rate, channels)` moves the mixer to its format with every voice, stream,
  sound, bus, graph bus and effect kept (effects and the reverb are rebuilt for the new rate: parameters, order and effect ids survive, delay
  memory is cleared; a fade in progress keeps its length in seconds). One Output Log line per change.
- **No device.** The mixer keeps mixing into nothing at real-time pace (`idleStep`), so playback positions, fades and finishing voices
  advance, and the open is retried every second. A thread that was starved owes at most half a second of audio, not the whole gap. A device
  that fails again within 250 ms of a rebuild waits for the retry instead of being reopened in a loop.
- **Sample-rate rule.** Assets keep their own rate; every voice resamples to the device rate (`srcRate / deviceRate` per output frame), so a
  48 kHz asset plays at the right pitch and length on a 44.1 or 96 kHz device and across a change between them. Nothing is resampled at
  import.
- **Reporting.** Underruns, state, device name, hardware channels, buffer and latency, and the change count are on `AudioDevice` and through
  `audio_abi.h` (`aver_audio_device_*`) and `Audio.Device*` in C#.

`setOutputFormat` allocates and is not audio-thread safe: the driver calls it only while no endpoint is open (between `close` and `start`),
never from `mix()`. `tests/audio/AudioDeviceTest` runs all of it against a fake device and a fake clock; the real WASAPI and SDL backends
are compile-checked only (docs/AUDIO.md section 11 lists what to listen for).

## Real-time sound graphs (`SoundGraph.hpp`)

A `.ocsnd` node graph played on the audio thread (`docs/AUDIO.md` section 13). `SoundProgram` is a graph compiled once from plain data (the
format layer, `Aver.Sound`'s `compileSound`, converts a `.ocsnd`; this module reads no files); `SoundRunner` is one playing instance. Same rule
as `mix()`: the runner's constructor allocates everything (node state, a value table, one delay memory sized for all Delay nodes) on the game
thread, and `process()` is arithmetic only. `Mixer::playGraph` makes a voice of it; the runner hangs off the voice slot, is replaced when the
slot is next acquired and freed in `collect()`, and the audio thread only clears its pointer.

- **Live inputs are latest-value atomics.** `setParam` (by id; `setGraphParam` by name) stores; `process()` snapshots at the start of a block;
  the Parameter node smooths toward it per sample (20 ms by default), so a jump is a ramp. `noteOff` is a flag latched at the next block: an Adsr
  releases from its current level and the voice ends after the longest release. `Voice.stop` is still the one-block fade.
- **One evaluator.** `Aver.Sound`'s offline `renderSound` constructs a runner and calls `process()` once. The loop is per sample with all state in
  the runner, so any chunking gives identical samples; `tests/audio/SoundGraphTest` pins offline == audio thread bit for bit, and counts
  allocations in `mix()` with a replacement `operator new` (0).
- Kinds added: Parameter, LFO, HighPass, BandPass, Random, Crossfade, WavePlayer, Delay, Clamp, MapRange, EnvelopeFollower; oscillators and LowPass
  gained an additive modulation input. A graph voice ignores voice pitch (a Parameter on an oscillator is its pitch) and the speed scale.

## Decoders (`modules/formats`, not this module)

This module stays Core-only and decodes nothing. The decoders are `audio::StreamSource`s in `Aver.Formats.Audio`
(`aver/formats/AudioDecode.hpp`): Ogg Vorbis (`third_party/stb/stb_vorbis.c`), FLAC (`dr_flac.h`) and MP3 (`dr_mp3.h`)
on every platform, each vendored file compiled in one translation unit. They stream with bounded memory on the decoder
thread (so `mix()` never sees them), seek, take a start offset, a duration and loop points (Ogg `LOOPSTART` /
`LOOPLENGTH` comments; FLAC and MP3 loop where the caller says), trim an MP3's encoder delay and padding (LAME/Xing tag),
fold 3 to 8 channels to stereo, and refuse hostile input instead of crashing. `modules/audio.abi/src/StreamSources.cpp`
picks them ahead of Media Foundation, which keeps `.m4a .aac .wma`; the importer and the Content Browser accept `.ogg
.oga .flac .mp3` wherever `.wav` is accepted. `tests/audio/AudioDecodeTest` builds FLAC and MP3 streams in code and checks
them sample for sample; `docs/AUDIO.md` section 12 lists the headed checks.

## Environment (`Convolver.hpp`, `Reverb.hpp`, `Music.hpp`)

Opt-in, off by default (a mixer that uses none of it renders the same samples as before, pinned in `tests/audio`); `docs/AUDIO.md` section 15 has the
details, the numbers and the headed checks.

- **Zone send reverbs.** `kMaxZoneReverbs` (4) send reverbs are allocated in `Mixer::init`; `setZoneReverb(slot, {wet, decay, damping, impulse,
  predelayMs})` and `setVoiceZoneSend(voice, slot, send)` are latest-value atomics, the audio thread ramps each send across the block. The scene
  (`modules/audio.scene`) gives the four most relevant `CReverbZone`s a slot and sets each tracked voice's sends by priority blend (`zoneShares`), so a
  voice rings in the room it is in; `AudioSceneSystem::setReverbSmoothing` eases the listener-side blend.
- **Convolution.** `Convolver` is a uniformly partitioned (256-frame) overlap-save FFT convolver with its own radix-2 FFT: latency one partition,
  any impulse length (12 s cap), impulse built on the game thread from a decoded sound at any rate and crossfaded in, predelay, no allocation or lock
  in `process()`. It is the `"AudioEffectConvolutionReverb"` bus effect (`dry`, `wet`, `predelay_msec`, `gain_db`) and an alternative reverb for a zone slot.
- **Diffraction.** `findDiffractionPath` searches for the shortest one-bend path around an edge or through an opening with the existing `RayBlockFn`
  and turns it into an occlusion value for the voice's own gain and low-pass (no delay, no new audio-thread code); `CAudioOcclusion::diffraction`
  (default 0) enables it per emitter.
- `tests/audio/AudioEnvironmentTest` runs it all offline, including the convolver against a direct time-domain convolution and its cost per block.
