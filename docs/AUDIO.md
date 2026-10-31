# Aver Engine — Audio: the plan

**This plan is now built.** This document said "nothing here is built yet" for a long time; that
stopped being true once `Aver.Audio`, `Aver.Audio.Wasapi` and `Aver.Audio.Abi` shipped, and
`modules/audio/CMakeLists.txt` and `modules/audio/README.md` now cite this file as "the plan this
implements" rather than a plan waiting to be started. The shape below mostly held — three targets,
the Core-only mixer, the never-touches-a-device split, generational voice handles, fixed voice pool,
constant-power panning, inverse-distance attenuation — but several specifics changed on the way from
plan to code, and this document was never updated to say which. Known divergences, checked against
the tree rather than assumed:

- **The command mechanism is per-slot atomics, not the lock-free SPSC ring §3 specifies.**
  `modules/audio/README.md` names this explicitly: volume, pitch, position and stop are not *ordered*
  with respect to each other — latest value wins — so a ring built for ordered messages was the wrong
  shape, and the deviation is recorded as deliberate rather than as drift.
- **The shipped format is `.ocaudio` with an `AHDR` header chunk and an `APCM` sample chunk**
  (`modules/formats/include/aver/formats/OcAudio.hpp`), not the `.ocsound`/`SNDH` naming §4 proposes.
- **Streaming exists, and so do the Ogg Vorbis, FLAC and MP3 decoders (§12).** A decoder thread and ring
  buffer stream WAV, Ogg Vorbis (`stb_vorbis`, as §2 chose), FLAC (`dr_flac`) and MP3 (`dr_mp3`) on every
  platform, and Media Foundation files on Windows ([AUDIO_STREAMING.md](AUDIO_STREAMING.md)), with
  crossfades, raycast occlusion and reverb zones.
- **A `.ocsnd` sound graph plays in real time (§13).** The mixer evaluates it per block on the audio thread
  (`SoundGraph.hpp`, `Mixer::playGraph`) with named live inputs and a note-off; the offline render
  (`docs/SOUND.md`) runs the same evaluator.
- **The C ABI is shaped differently from §6's sketch.** The real `audio_abi.h` splits a plan-shaped
  `aver_audio_play(path, volume, pitch)` into `aver_audio_load(path)` (returns a *sound* handle,
  decoding once) and `aver_audio_play(sound, volume, pitch, looping, bus)` (returns a *voice* handle)
  — closer to WASAPI's own load/play separation than the single-call sketch here. `aver_audio_init`/
  `_shutdown`/`_ready` also exist and are not in §6's list at all.
- **`tests/audio` runs 73 assertions with no sound card**, matching §7's intent exactly (a stolen
  voice failing to bump its generation was a real bug this suite caught), even though the specific
  list of cases has not been checked one-for-one against §7's bullets.

The rest of this document is the plan as originally written, kept for the reasoning it records — not
because every specific in it shipped unchanged.

---

## 1. The shape, and why it is that shape

Three targets, mirroring what the renderer and the UI already do:

| Target | Kind | Depends on | Owns |
|---|---|---|---|
| `Aver.Audio` | STATIC, **Core-only** | `Aver.Core` | the mixer graph, voices, DSP, 3D panning and attenuation |
| `Aver.Audio.Wasapi` | STATIC | `Aver.Audio` | the device, the render thread, the ring buffer |
| `Aver.Audio.Abi` | SHARED | `Aver.Audio` | the plain-C seam a game calls (the eighth) |

**`Aver.Audio` never touches a device**, and that is the load-bearing decision rather than a
consequence of it. It mixes into a float buffer the caller supplies; something else hands that buffer
to hardware. This is exactly the split that makes `Aver.UI` testable with no GPU, and this session
earned the right to state it as a rule: the UI's renderer had 60 assertions passing against a
recording device before a single pixel existed, and the two real bugs it shipped with — a
double-multiplied blend and a resize that released the back buffers before the call that could fail
— were both found by *looking*, not by reading code. Audio has no equivalent of looking. A wrong pan
law and a right one are the same waveform to a reader and the same silence to a screenshot. If the
mixer cannot be run headlessly and asserted on sample by sample, it cannot be checked at all.

The link line enforces it, as it does for `Aver.Render.UI`: `Aver.Audio` names `Aver.Core` and
nothing else.

---

## 2. The backend: WASAPI, written rather than vendored

Constraint: permissively licensed dependencies only. That removes OpenAL Soft (LGPL) outright, and
FMOD and Wwise are commercial licences per title.

| Option | Licence | Verdict |
|---|---|---|
| **WASAPI direct** | Windows SDK, no dependency at all | **Chosen.** |
| miniaudio | public domain / MIT-0, one header | Keep for a second backend (macOS/Linux) |
| XAudio2 | Windows SDK | Rejected — it owns the mixer, and we want to own the mixer |
| SDL_audio | zlib | Rejected — pulls all of SDL for one file's worth of use |

WASAPI shared mode, `IAudioClient3` where available for a smaller period. The engine already writes
its own D3D12 backend and its own JSON reader rather than vendoring either; a device backend is
roughly 400 lines and buys exact control of the buffer size, which is the only number that decides
latency.

XAudio2 deserves its own sentence, because it is the obvious Windows answer and it is the wrong one
here: it would do the mixing, the 3D and the DSP itself, so `Aver.Audio` would become a wrapper
around a thing it cannot test and cannot port. The point of owning the mixer is that the mixer is the
part with the bugs in it.

---

## 3. The real-time boundary

This is the part that is impossible to add later, so it is stated first and enforced from the first
commit.

**The mix callback never allocates, never locks, never blocks, never logs, never calls managed code.**
Not "tries not to" — a single `malloc` on the audio thread is a click in somebody's headphones, and
it will happen on the machine you do not own.

Consequences that follow from that one rule:

- **Commands cross on a lock-free SPSC ring.** Gameplay writes; the audio thread reads. `PlaySound`,
  `SetVoiceVolume`, `Stop` are all messages, never direct calls into mixer state.
- **Voices are a fixed pool**, sized at init. Running out steals the quietest, oldest voice — a
  decision the mixer makes, with no allocation and no failure path to propagate.
- **Handles are generational `int32_t`, `0 == invalid`**, matching every other seam. A handle to a
  stolen voice must read as dead rather than as somebody else's sound.
- **The audio thread never calls up into C#.** A "sound finished" notification is a flag the game
  polls on its own thread, not a callback. The CLR can garbage-collect; an audio thread cannot wait
  for it.
- **A glitch is a hard failure, not a slow frame.** The renderer may take 40 ms and merely look bad.
  The mixer missing its deadline is an audible artefact, so underruns are counted and reported rather
  than absorbed.

**Latency budget:** 48 kHz, 10 ms period = 480 frames per buffer, triple-buffered. State it, measure
it, and put the underrun count somewhere visible — the engine already has a status bar with the frame
time in it.

---

## 4. The format chain

Mirrors the glTF → `.ocmesh` chain that already exists, for the same reason: the runtime should load
one format it controls, not five it does not.

- **`.ocsound`**, in the AVR1 container that `Avr1.hpp` already provides — a `SNDH` header chunk
  (sample rate, channels, frame count, loop points, whether it is stream-or-resident) and a payload
  chunk. CRC32C on the header and xxHash64 on the payload, like every other AVR1 file.
- **Importers**, beside `GltfImport`: **WAV** written by hand (it is a header and a blob, and the
  engine already writes its own parsers), and **Ogg Vorbis** through `stb_vorbis` (public domain,
  one file) for anything long enough that raw PCM is silly.
- **Resident vs streamed** is a property recorded in the file at import, not decided at runtime: a
  gunshot is decoded once into memory, a music bed is streamed from disk by a worker thread that
  keeps the ring fed. The threshold is a number in the importer, and the runtime never guesses.
- **Resampling happens at import**, to the device rate, once. Not per frame, and not on the audio
  thread.

---

## 5. 3D audio, in the engine's own contract

Centimetres, **+X forward, +Y right, +Z up, left-handed** — the same contract as everything else, and
worth naming explicitly because getting it wrong puts every sound on the wrong side of the player's
head and looks like a bug in the pan law rather than in a sign.

- Distance attenuation over an authored curve, with an inner radius (full volume) and an outer radius
  (silence), in **cm**.
- Speed of sound **34300 cm/s** for Doppler, so the constant is in the engine's units and not
  converted at every use.
- Stereo panning first. **HRTF was explicitly not in scope** for the first pass — it is a convolution
  and a data set, and it is a poor use of the effort before anything makes a sound at all. Later
  (section 14) came 5.1 / 7.1 output and an opt-in headphone mode built on an analytic head model, still
  with no measured data set.
- The listener is a position and an orientation pushed once per frame by whatever owns the camera,
  the same way `setCamera` already works.

---

## 6. The C seam — the eighth

`modules/audio.abi`, following what `Aver.UI.Abi` established last: native `Aver.Audio.Abi`, managed
`Aver.Audio`, **different file names so no `NativeResolver` is needed**. Roughly:

```c
int32_t aver_audio_play(const char* ocsoundPath, float volume, float pitch);   // returns a voice
int32_t aver_audio_play_at(const char* path, float x, float y, float z, float volume, float pitch);
void    aver_audio_stop(int32_t voice);
int32_t aver_audio_playing(int32_t voice);
void    aver_audio_set_voice_volume(int32_t voice, float v);
void    aver_audio_set_voice_position(int32_t voice, float x, float y, float z);
void    aver_audio_set_listener(const float* pos3, const float* forward3, const float* up3);
void    aver_audio_set_bus_volume(int32_t bus, float v);   // sfx / music / voice / ui
int32_t aver_audio_underruns(void);                        // 0 is the only acceptable value
```

Buses are named constants, not handles — there are four and they are the same four in every game.

**AS SHIPPED, and two details of this sketch did not survive contact.** The signatures above take a
path; the real `aver_audio_play` takes a `sound` handle from `aver_audio_load`, so the same file is
decoded once however many call sites play it. And the four buses are **sfx, music, voice and ui** —
the comment above said "master" because master is not a bus at all, it is
`aver_audio_master_volume()`, applied after the four are summed.

They are named on all three sides now, and pinned on all three: `aver::audio::Bus` (`Sound.hpp`),
`AVER_AUDIO_BUS_*` (`audio_abi.h`) and `Aver.Framework.Bus` (`Audio.cs`). `AudioAbi.cpp`
`static_assert`s the first two against each other — including `Bus::Count == 4`, so adding a bus
without its macro fails to compile — and `tests/abi/src/AbiEnumTest.cpp` compares the third.

From a script:

```csharp
Sound step = Audio.Load("Content/Sounds/step.ocsnd");
Audio.Play(step, volume: 0.8f, bus: Bus.Sfx);
Audio.SetBusVolume(Bus.Music, 0.3f);        // what a settings slider drives
Audio.MasterVolume = 0.9f;                  // NOT a bus; applied after the four are summed
```

**Play observer (accessibility captions).** `aver_audio_set_play_observer(fn, user)` registers one optional callback, null by
default, called on the calling (game) thread right after every *successful* start (`voice != 0`) from `aver_audio_play`,
`_play_at`, `_stream_play`, `_stream_play_at` and `_music_play`, with the file path (NULL for `aver_audio_load_pcm` sounds, which have
none), the voice, bus, whether it is positional, its position and whether it loops. With no output device no voice starts, so the
observer is never called. Sub-systems that play through these functions (scripts, graph nodes, sequencer audio tracks) are covered
with no change of their own; `FrameworkAccessAbiTest` scans `AudioAbi.cpp` to check each play function still notifies. See
[ACCESSIBILITY.md](ACCESSIBILITY.md).

An unrecognised bus is folded to `Sfx` by the native side rather than rejected — an ABI must not
trust its caller. The one place an out-of-range value can still arrive is a **Set Bus Volume graph
node**, whose bus pin is an integer someone typed, and that path logs a warning naming the four.

`docs/ABI.md` gains a section 11 and its counts move again.

---

## 7. What gets tested, headlessly

The whole point of §1. A test renders N frames into a buffer and asserts on the samples:

- a 440 Hz sine mixed alone comes back as a 440 Hz sine — amplitude, zero crossings, no DC offset;
- two voices at half volume sum without clipping, and the same two at full volume clip *predictably*
  rather than wrapping;
- the pan law is constant-power: a source swept left to right holds total energy within a tolerance
  (a linear pan dips 3 dB in the middle, which is audible and looks fine on paper);
- attenuation at the inner radius is exactly 1.0 and at the outer exactly 0.0, with no discontinuity
  at either — the failure mode is a sound that pops as you walk past a boundary;
- voice stealing takes the quietest and oldest, and the stolen handle reads dead;
- the command ring survives being filled and drained from two threads, and a full ring drops the
  *newest* command rather than blocking the writer;
- a loop point that is not a multiple of the buffer size still loops sample-exact.

None of these needs a device. All of them are wrong in ways nobody hears until the game ships.

---

## 8. Order of work

1. `Aver.Audio` core: voice pool, command ring, mixer producing a float buffer. **Tests first** — the
   whole list in §7 runs before any device exists.
2. `.ocsound` + the WAV importer, so there is something to play.
3. `Aver.Audio.Wasapi`: device, thread, ring. First actual sound.
4. 3D: listener, attenuation, pan.
5. `Aver.Audio.Abi` + the managed `Aver.Audio`, and a shot in SkyForge that makes a noise.
6. Ogg Vorbis and streaming, for music.

Stages 1 and 2 are worth doing even if the rest slips: they are the parts that cannot be tested later.

---

## 9. Known unknowns

- **Device change while running** — headphones plugged in mid-session invalidates the client. WASAPI
  reports it; the plan is to rebuild the device and keep the voices, but that path is exactly the
  kind that is written once and never exercised. It needs a deliberate test. **Built and tested
  offline; see section 11** (`AudioDeviceTest` drives the policy with a fake device; the real
  device paths still need the listening checks there).
- **The CLR and the audio thread.** §3 forbids managed calls from it. Whether anything in the
  scripting bridge makes that hard to hold is not yet known.
- **Sample-rate mismatch on a device change**: assets resampled at import to 48 kHz are wrong for a
  44.1 kHz device. Either resample on load or keep the source rate in the file and resample per
  voice; not decided. **Decided (section 11): the source rate is kept and every voice resamples to
  the device rate, so §4's "resampling happens at import" does not apply.**

---

## 10. Bus graph, effects and Godot-style players (Fabric G5 S1)

Added for converted Godot projects ([G5_PARITY.md](https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G5_PARITY.md), S1), on the same mixer and under the same real-time rule. **Everything here is opt-in**: the four legacy buses (`audio::Bus`), `PlayDesc` defaults and the `mix()` of a mixer whose graph was never activated are exactly what they were (`AudioTest`, `AudioStreamTest` and `AudioSceneTest` are unchanged and pass).

**The bus graph** (`BusGraph.hpp`, owned by the `Mixer`, `mixer.graph()`). Godot's AudioServer model: a list of named buses, index 0 is `Master`; each bus has `volume_db`, `mute`, `solo`, `bypass_fx`, a send (by name) and an effect chain. Voices started with `PlayDesc::graphBus >= 0` sum into their bus; the buses are processed from the last index down to Master (so a send always reaches a bus that has not been processed yet); the graph's Master block is added to the legacy output and the sum is hard-clipped as before. The rules are Godot's: a send to a bus at the same or a higher index, or to a name that does not exist, goes to Master; solo marks the soloed buses and every bus on their send chains up to Master, silences the rest and makes `mute` irrelevant; peaks are read after effects and volume (`-200 dB` when silent); `add_bus` names buses `New Bus`, `New Bus 2`...; `move_bus(from, to)` numbers `to` before the move; Master cannot be moved or removed; `bus_count` is 1..255. Gains ramp across the block. A voice on a graph bus takes no legacy bus or master gain. `AudioServer.playback_speed_scale` is `Mixer::setPlaybackSpeedScale` (a multiplier on every voice's rate, default 1).

**Threading.** Structure edits (add, remove or move a bus or effect, a send or sidechain change, applying a layout) take a spin lock that the audio thread only `try_lock`s: a block that finds the graph being edited renders without its graph voices, a rare gap of one block while a script rebuilds the layout. Volume, mute, solo, bypass, enable flags and every effect parameter are atomics and never lock; effects and buses are allocated and freed on the game thread.

**Effects** (`Effects.hpp`; Godot's class names and property names, defaults and ranges; `Effect::create`, `findParam`, `setParam`). All allocate only when made and work in place on an interleaved stereo block.

| Effect | Implementation | Against Godot |
|---|---|---|
| Amplify | gain ramp | same |
| LowPass / HighPass / BandPass / Notch / LowShelf / HighShelf | Godot's `AudioFilterSW` coefficients (cutoff clamp, resonance as Q, BandPass Q x2, shelf gain, `db` = 1..4 cascaded stages with Godot's Q and gain adjustment) | same coefficients, tested against the formulas |
| Delay | two taps (delay, level, pan) and a low-passed feedback line | same structure; pan law is constant power, unity at the centre |
| Chorus | up to four LFO-modulated voices, low-passed, panned, from the mono mix | approximated |
| Compressor | peak envelope (attack in microseconds, release in ms) on the static curve (threshold, ratio, makeup, mix); sidechain from a bus with a higher index | approximated detector, exact static curve |
| Limiter | soft clip: transparent below the knee (ceiling minus `soft_clip_db`), tanh above | approximated |
| HardLimiter | 1.5 ms look-ahead brick wall; the output never exceeds the ceiling | approximated release |
| EQ6 / EQ10 / EQ21 | peaking filters at Godot's band centres | exact gain at each centre; band shape approximated |
| Distortion | clip, atan, lo-fi, overdrive, wave shape; `keep_hf_hz` leaves the highs alone | approximated curves |
| Phaser | six all-pass stages, LFO between `range_min_hz` and `range_max_hz` | approximated depth scale |
| PitchShift | time-domain two-head shifter | approximated (some modulation on sustained tones; pitch within a few percent) |
| Reverb | the existing send reverb (combs and all-passes) with a pre-delay line, hi-pass, damping, spread, dry and wet | approximated |
| Panner | linear balance | approximated |
| StereoEnhance | mid/side widening, right channel delayed | approximated |
| Capture | stereo ring the game thread reads (`buffer_length`, up to 2 s; frames that do not fit are dropped and counted) | same behaviour |
| SpectrumAnalyzer | history ring; a Hann-windowed FFT on request (`fft_size` 256..4096, `tap_back_pos`); average or maximum magnitude over a range, a full-scale sine reads about its amplitude | same interface, own scaling |

Not implemented: `AudioEffectRecord` (microphone input), `AudioEffectBandLimitFilter`.

**Players.** `PlayDesc` gained `graphBus`, `spatialModel` + `Spatial3D`, `startSeconds` and `paused`. A Godot-spatial voice (`Spatial.hpp`, pure functions tested in `AudioSpatialTest`) gets, per block: the attenuation model (inverse distance, inverse square, logarithmic, disabled) with `unit_size` and the `max_db` clamp, the `max_distance` linear fade, Godot's stereo panning law (project setting `audio/general/3d_panning_strength`, default 0.5), the emission cone, the attenuation filter (a one-stage high shelf at `attenuation_filter_cutoff_hz` whose gain follows `(1 - distance gain) * attenuation_filter_db`, plus the cone's dB outside it), and the Doppler pitch (`343 / (343 + |v| * cos)`, clamped to 1/8..8, from the emitter's and the camera's tracked velocities). `setVoicePaused` fades a voice out in one block and holds its playhead; `voicePositionSeconds` / `voiceLengthSeconds` read the playhead. A **live** voice (`playLive`, `StreamParams::live`) is a ring the game thread pushes frames into (`AudioStreamGenerator`); running dry is an under-run (counted, `get_skips`), not the end of the voice.

**Streams that pick or mix other streams** (`AudioStreamRandomizer`, `Polyphonic`, `Playlist`, `Synchronized`, `Interactive`) are `Aver.Fabric` objects that start ordinary voices; the engine needs nothing for them. Approximations: a playlist plays its entries back to back (no cross-fade), an interactive stream switches clips at once and keeps its transition rules without timing them.

**What decodes what.** WAV (8/16/24/32-bit PCM and float) by `Wav.cpp` / `WavStream.cpp`, on every platform, with loop points settable on a loaded sound (`aver_audio_sound_set_loop`; ping-pong and backward loops play forward). **Ogg Vorbis, FLAC and MP3** decode with the vendored `stb_vorbis`, `dr_flac` and `dr_mp3` on every platform, for streaming and for import (section 12). **`.m4a`, `.aac` and `.wma`** still go through Windows Media Foundation (`audioImportFile`, `StreamSources.cpp`) and decode on Windows only; Media Foundation is also the fallback for an `.ogg`, `.flac` or `.mp3` the vendored decoders refuse. Ogg Opus is not supported.

**Video.** `VideoStreamPlayer` is **unsupported**: `modules/media` is an H.264/MP4 *encoder* (`VideoEncoder`, Media Foundation `IMFSinkWriter`, for the render queue); the engine has no video decoder or texture-streaming path. The converter places the node as a Control carrier with its properties and reports it unsupported, and `.ogv` / `.webm` files are reported the same way. A decoder dependency would add: a Theora/Ogg decoder (libtheora + libogg + the Vorbis decoder above) for Godot's own `.ogv` format, or Media Foundation's source reader on Windows for H.264 `.mp4` (no new library, Windows only), plus a per-frame texture upload through `render.ui`'s texture path and an audio-track tap into a stream voice.

**Offline mode** (`aver_audio_offline_init` / `_offline_mix`): the same mixer with no device, for tests and tools. `Aver.Fabric` also falls back to it, stepped by the frame loop, when a game has no output device, so players still finish. A game never opens it by choice.

**Bus layouts.** `BusLayout` (`BusLayout.hpp`) is plain data (buses, effects by Godot class name, parameters by Godot property name); `BusGraph::applyLayout` / `exportLayout` apply and read it; `.ocbus` (`docs/formats/FORMAT_SPECS.md`) is its text form (`OcBusLayout.hpp`, in `Aver.Formats.Audio`); the project key `AUDIO.BUSLAYOUT <content path>` names the layout a project starts with (empty = none, no graph). `AverAssetC fabric` converts a Godot `AudioBusLayout` into one.

---

## 11. Device robustness

The engine follows the system's default output: unplug the headphones, switch the default in Windows' sound settings, change its sample rate, disable the device or restart the audio service, and the sound carries on from where it was. With no output device at all it keeps running silently and picks one up when it appears.

**Where it lives.** `OutputDriver.hpp` / `OutputDriver.cpp` in `Aver.Audio` (device-free, so tested offline) hold the policy and the loop; `AudioDevice` is the render thread around the driver; `Aver.Audio.Wasapi` and `Aver.Audio.Sdl` each implement `OutputBackend` (`open`, `start`, `service`, `close`) for one endpoint and decide nothing. WASAPI is a pull backend (it waits on the buffer event and fills the device); SDL is a push backend (its own callback renders, `service` only watches the device).

**The policy.**

| Situation | What happens |
|---|---|
| `AUDCLNT_E_DEVICE_INVALIDATED` / `RESOURCES_INVALIDATED` / `SERVICE_NOT_RUNNING` from `GetBuffer`, `ReleaseBuffer`, `GetCurrentPadding`; endpoint removed or disabled (`IMMNotificationClient`); SDL default device gone | Close, reopen on the current default, move the mixer to its format, log `output device changed (device lost)`. Audio the mixer had already rendered into the block that died is lost; nothing else is. |
| The system default output changes (`OnDefaultDeviceChanged`, render + console role; SDL event or a 250 ms poll of the default device's name and format) | The same, logged as `default device changed`. |
| The device has asked for nothing for 2 s | One underrun is counted and the device is rebuilt (`device stalled`). The old render thread simply stopped. |
| No device to open | State `NoDevice`: the mixer is called at real-time pace into a scratch buffer nobody hears, so playback positions, fades and finishing voices carry on; the open is retried every second; logged once on entry. When one opens the mixer moves to its format (`device back`). |
| A device that fails again within 250 ms of the last rebuild | Not reopened in a loop; it waits for the next retry. |
| The idle thread was starved (a long stall) | It catches up by at most half a second of audio. |
| No device at `start()` | Unchanged: `start()` returns false and a game runs silent, as before. (A later device is not picked up in that case.) |

**Sample-rate rule.** Assets keep their source rate (`SoundData::sampleRate`, `StreamSource::sampleRate`); every voice resamples to the device rate with its own linear cursor (`srcRate / deviceRate` per output frame). There is no import-time resample, so an asset imported at 48 kHz plays at the right pitch and length on 44.1 kHz and 96 kHz devices and survives a change between them without being touched. `Mixer::setOutputFormat(rate, channels)` is the whole "re-init": it changes the output rate and channel count (1, 2, 6 or 8; by default a device with two or more channels is mixed as stereo and extra channels get silence, unless the project asks for 5.1 or 7.1, section 14) and keeps voices, streams, sounds, bus and master volumes, the bus graph and its effects. The reverb and the graph's effects are rebuilt for the new rate (parameters, order, enabled flags, sidechains and effect ids kept; delay lines and filter memory start empty), and a fade in progress is rescaled so it keeps its length in seconds. The linear resampler is not band-limited: a 96 kHz asset on a 44.1 kHz device can alias.

**Real-time rule.** `mix()` and the backend's per-block path (`service`/`feed`) allocate nothing (the scratch buffers are sized in `start()`), take no lock and log nothing. `setOutputFormat`, the log line and the open/close happen on the render thread while no endpoint is running. The device-info snapshot behind `deviceName()` is a mutex the render path never takes.

**Reporting.** `AudioDevice::underruns()`, `state()`, `deviceName()`, `deviceChannels()`, `bufferFrames()`, `latencyMs()`, `deviceChanges()`; through the C ABI `aver_audio_underruns`, `aver_audio_device_state` / `_name` / `_channels` / `_buffer_frames` / `_latency_ms` / `_changes` (`audio_abi.h`); in C# `Audio.Underruns`, `Audio.DeviceState` (`AudioDeviceState`), `DeviceName`, `DeviceChannels`, `DeviceBufferFrames`, `DeviceLatencyMs`, `DeviceChanges`. The figures describe the last device that was open and keep describing it while there is none. `Audio.SampleRate` / `Channels` are the mixer's current rate and channels. WASAPI's latency is the stream latency it reports plus the period; SDL reports the period only.

**Tested offline** (`tests/audio/AudioDeviceTest`, no sound card): loss in the middle of a block then a default at another rate (voices, stream, graph bus and effect, bus and master volumes, playback position all kept); 1 kHz asset pitch at 48, 44.1 and 96 kHz devices and across the changes between them; fade length across a change; no device for 2 s (position advances 2 s, a short voice finishes, retries counted) then a mono 96 kHz device; a starved idle thread; a flapping device; a wedged device; underrun counting; mono and 6-channel devices; the Output Log lines; the render thread (`AudioDevice::startWith`) against a fake device through a change, a loss with no replacement and a return, with a prompt `stop()`; and that a busy scene renders the same samples as the pre-change mixer (recorded golden sums) and bit-identical through the driver as called directly.

**Not tested here, only compile-checked (headed checks for the owner).** The WASAPI and SDL backends were never run: no device is opened by tests. Listen and watch the Output Log for:

1. **Unplug and replug.** Play music and a looping effect, unplug the headphones (or disable the device in Sound settings). Expect a short gap, then sound on the new default with the music at the same point and the effect still looping; one `output device changed (device lost)` line, no crash, no frozen game.
2. **Switch the default** in Windows' Sound settings between speakers and headphones while music plays. Expect `output device changed (default device changed)`, continued playback, no pitch shift (the two devices may run at 44.1 and 48 kHz: the music must keep its pitch and speed).
3. **Change the device's sample rate** (Sound settings, Advanced, 44100 Hz to 96000 Hz, shared mode). The same: pitch and tempo unchanged.
4. **No device.** Disable every output device while a game plays, wait several seconds, enable one. Expect one `no output device` warning, a game that keeps running (a music track's position keeps moving, so after re-enabling it is not where it was paused), and `device back` within a second or two of enabling.
5. **Flapping.** Rapidly unplug and replug: the log should not scroll with changes; a retry follows within a second.
6. **Hold the render thread.** Suspend the game under a debugger for 5 s and resume: sound should recover (one `device stalled` rebuild at most), not stay silent.
7. **Report.** `Audio.DeviceName`, `Audio.DeviceLatencyMs`, `Audio.DeviceChanges` and `Audio.Underruns` in a HUD or the log look right for the device in use and move on a change.
8. **Linux and macOS (SDL).** The same checks 1 to 4 with the system's default-device switch (PipeWire/PulseAudio, Core Audio): the SDL path polls the default device every 250 ms, so the move may take a quarter of a second longer than on Windows.

---

## 12. Decoders: Ogg Vorbis, FLAC, MP3

**What and where.** Three vendored, single-file decoders, each compiled in exactly one translation unit of `Aver.Formats.Audio` with its implementation macro and warnings off for that file only: `third_party/stb/stb_vorbis.c` (v1.22, `AudioDecodeVorbis.cpp`), `third_party/dr_libs/dr_flac.h` and `dr_mp3.h` (`AudioDecodeFlac.cpp`, `AudioDecodeMp3.cpp`). `AudioDecode.hpp` is the API: every decoder is an `audio::StreamSource` (the decoder thread reads it incrementally with bounded memory, exactly like `openWavStream`), `openDecodedFile` / `openDecodedMemory` open one, `decodeAudioFile` / `decodeAudioMemory` decode a whole clip into `SoundData`. The codec is chosen by the magic bytes (`OggS`, `fLaC`, `ID3` or an MPEG frame sync), falling back to the extension. `StreamSources.cpp` (`openStreamSource`) tries the vendored decoders first for `.ogg .oga .flac .mp3`, ahead of Media Foundation; Media Foundation stays for `.m4a .aac .wma` and as the fallback when a vendored decoder refuses a file.

**Import.** `isImportableAudio` / `audioImportFile` accept `.ogg .oga .flac .mp3` on every platform (the Content Browser's Import dialog and its file filter, `AverAssetC` and `aver_audio_load` go through them; the sequencer's sound lane recognises the extensions), cooking to `.ocaudio` like a WAV. Ogg `LOOPSTART` / `LOOPLENGTH` (and `LOOPEND`) comments become the cooked sound's loop points. The packaging scripts copy `Content/` wholesale (they drop only sources and intermediates), so no extension list needed a change.

**Loop points, start offset, duration.** `DecodeOptions { startFrame, durationFrames, loopBegin, loopLength }` windows a stream (seek is relative to the window; the loop is moved into it) and sets loop points in file frames. An Ogg file's own comments are used when the options name none; FLAC and MP3 have no such tag in the format, so they loop where the options say (`openStreamSource(path, why, &options)` for the engine's own callers; the C ABI keeps its signatures, so a game starts a stream from the start with the file's loop points).

**Gapless MP3.** `dr_mp3` reads the Xing/Info and LAME tag: the encoder delay and padding are trimmed (the decoder's own 529-sample delay with them), `totalFrames()` and every seek count the trimmed stream, and the tag frame itself is not audio. A file with no tag plays its frames as they are. The reported length is taken from the tag only if the file could hold it. The first seek away from the start scans the file once for a 1024-entry seek table so a loop back to the middle of a long track decodes a few frames, not the run-up.

**More than two channels.** The mixer takes mono and stereo, so 3 to 8 channels are folded to stereo (centre and surrounds at -3 dB, the LFE dropped, normalised so full scale cannot clip) with each codec's own channel order (Vorbis and FLAC differ).

**Hostile input.** Every open validates the stream and every size is capped; a truncated or garbage file ends the stream or refuses to open, never crashes. Vorbis: set-up memory comes from a fixed arena (6, then 24, then 96 MB, stepped so a normal file costs a few MB), so a header that declares 16 million codebook entries is refused at that ceiling; the comment header is checked against its own packet size before `stb_vorbis` sees it (stb_vorbis 1.22 turns an overrunning comment length into a negative allocation and writes before its arena: the one place it trusts a 32-bit field; the fix is the pre-check, the vendored file is untouched); its asserts are off in this one translation unit. FLAC: `dr_flac` allocates from STREAMINFO's 16-bit block size only; the 36-bit frame count is a claim and is dropped above 2^36; metadata blocks are skipped. MP3: a frame count the file is too small to hold is dropped. Whole-clip decode refuses to grow past `kMaxResidentSamples` (64M samples, 256 MB) whatever a stream expands to, with the capacity never overshooting it (a FLAC of constant blocks is thousands of times smaller than its output). Not covered: Ogg Opus and Ogg FLAC (refused), chained Ogg (not tested).

**Tested** (`tests/audio/AudioDecodeTest`, no device and no audio file in the repo): FLAC streams built in the test (STREAMINFO, verbatim, constant and fixed-predictor subframes, left/side and mid/side, CRC-8 and CRC-16) decode to exactly the samples they were built from, with seeks, windows, loop points through the real ring, 5.1 folding and a streamed FLAC against the same audio as a WAV through the offline mixer (bit-identical); MP3 streams of silent frames with a hand-built Xing/LAME tag pin the frame count, the delay and padding trim, seeking and windows; every decoder is fed truncations at many lengths, random byte overwrites, garbage, huge declared sizes and (Ogg) hostile headers. Ogg Vorbis cannot be built without an encoder, so the in-repo Vorbis tests are malformed input only; a real Ogg file decodes in the optional `AudioDecodeTest --file <path> [damagedCopies]` mode, not registered with ctest.

**Not tested here (headed checks for the owner, with a real device and real files).** None of these can be decided by reading samples:

1. **Ogg music.** Play a real `.ogg` track through `Audio.StreamPlay` / a Godot-converted `AudioStreamPlayer`. Expect it to start within a quarter second, play without clicks or dropouts to the end, and the Output Log to say nothing about a decoder or `held in memory`.
2. **Loop seam.** An Ogg with `LOOPSTART` and `LOOPLENGTH` comments, looping: the join should be inaudible (no click, no gap, no doubled beat). The same file with the comments removed loops from the start of the file.
3. **MP3 gapless.** An album track pair or a drum loop encoded with LAME, looping or played back to back: no silence or click at the join (an encoder that wrote no LAME tag still has the encoder's own gap; that is the file, not the decoder).
4. **FLAC.** A real `.flac` sounds identical to the same track as WAV (level and tone); a hi-res (24-bit, 96 kHz) one plays at the right pitch.
5. **Seek/loop to the middle of a long MP3.** Looping a 5-minute MP3 from 2:30: the first loop seam may be a few milliseconds late while the seek table is built, then none.
6. **Surround files.** A 5.1 Ogg or FLAC plays as a sensible stereo mix (dialogue centred, no loud LFE rumble, no clipping).
7. **Import.** Import a `.ogg`, `.flac` and `.mp3` in the Content Browser: each becomes an `.ocaudio` that plays; a renamed text file or a damaged one reports "could not decode" in the Output Log and nothing else happens.
8. **Linux and macOS.** The same checks 1 to 4 (these formats used to be silent there).

## 13. Real-time sound graphs

A `.ocsnd` was a DAG rendered offline to PCM (`docs/SOUND.md`). It can now also play as a **graph voice**: the mixer evaluates the graph per block on the audio thread, so it answers named float inputs (`rpm`, `intensity`, `surface`) and a note-off while it sounds. The offline render stays; both run **one evaluator**.

**Where.** `modules/audio/include/aver/audio/SoundGraph.hpp` + `src/SoundGraph.cpp` (Core-only, like the rest of `Aver.Audio`): `SoundProgram` (a graph compiled once from plain data, immutable and shareable) and `SoundRunner` (one playing instance). `Aver.Sound`'s `renderSound()` now constructs a runner and calls `process()` once over the whole length; `Mixer::playGraph` hands the same code a block at a time. `Aver.Sound` gained a dependency on `Aver.Audio` for this (and `Aver.Audio.Abi` on `Aver.Sound`); `compileSound(OcSoundData)` is the bridge, and the module manifests and `cmake/modules.catalog.json` say so. The evaluator is per sample and every state lives in the runner, so how frames are chunked changes nothing: `tests/audio/SoundGraphTest` pins that the runner, driven in blocks of 1, 7, 64, 333, 512, 1000 and 3 frames, and a flat mixer voice in uneven blocks, are **bit-identical** to the one-shot render of the same graph (stereo and mono mixers), and `tests/sound/SynthTest` carries a verbatim copy of the pre-change renderer and pins that a graph using none of the new features renders the **same samples as before** (all ten version-1 kinds, several links into one input, a release during the attack; two seeds, 48 and 44.1 kHz).

**The real-time rule, and how it is met.** `mix()` and everything it calls never allocates, locks, blocks, logs or calls managed code:

- A `SoundRunner`'s constructor allocates **everything** - node state, the value table, one pre-sized delay memory for all Delay nodes - and runs on the game thread inside `playGraph`. `process()` is arithmetic over those arrays. The voice pool is the existing fixed one (a graph voice is a voice; its runner hangs off the voice slot), so there is no second pool to exhaust.
- **Live inputs are latest-value atomics.** `setParam` stores a float and a "set" flag; `process()` snapshots them once per block and the `Parameter` node smooths toward the snapshot per sample (a one-pole, 20 ms by default), so a jump from gameplay is a ramp on the audio thread. The first block snaps, so a voice started with a value does not ramp up from the default. Non-finite values are refused.
- **Note-off is an atomic flag** latched at the next block. An `Adsr` then releases from its current level over its own release time and the voice ends after the longest release in the graph (at least 10 ms; the last 10 ms of the tail are a linear ramp, so a graph with nothing to release does not click). A finite (non-looping) graph is unchanged: its release is still counted back from the end, and a note-off earlier than that shortens it.
- **Free on the game thread.** A runner is created in `playGraph`, replaced when its voice slot is next acquired and released in `collect()`; the audio thread only clears its pointer. Wave sounds a `WavePlayer` plays are held by reference count like any voice's sound, so retiring one mid-play does not free it.
- **Stealing** is the existing quietest-then-oldest rule on voices; a stolen graph voice drops its sound references the same way a stolen plain voice does, and its state cannot leak (the next voice in the slot is a fresh runner; the test compares it to a fresh offline render).
- **Counting allocations.** The tree had no allocation-counting hook, so `tests/audio/SoundGraphTest` replaces global `operator new` (with a control that proves it counts) and asserts **0 allocations** across 400 `mix()` calls with two graph voices using every allocating-prone node (Delay, WavePlayer with resampling, filters, Crossfade), parameter writes by name and by id, a note-off, a voice ending and uneven block sizes. It cannot see a raw `malloc` or a lock; those are excluded by reading `process()` and `renderVoice`, which call neither.

**Node kinds (format version 2).** Kinds 0 to 9 are unchanged. New, stored by number, each with a fixed input count (an unconnected input reads 0; several sources into one input are summed):

| Kind | Inputs | Params |
| --- | --- | --- |
| Parameter (10) | none | default, smoothing seconds (0 reads as 0.02), min, max (clamps when max > min); the node's **name** is the live input |
| LFO (11) | rate offset Hz | rate Hz, depth, shape (0 sine, 1 triangle, 2 square, 3 saw), offset; out = offset + depth * wave |
| HighPass (12), BandPass (13) | signal, cutoff offset Hz | cutoff Hz, Q (0 reads as 0.7071); a topology-preserving state-variable filter, stable under per-sample cutoff changes; band pass peaks at unity |
| Random (14) | none | min, max, redraw rate Hz (0 = once); drawn from the play's seed per node, so "per trigger" is "per play" |
| Crossfade (15) | position offset, then 8 signals | position (0..N-1), N (0 reads as 2, max 8), curve (0 equal power, 1 linear) |
| WavePlayer (16) | pitch offset in semitones | pitch ratio (0 reads as 1), loop (>= 0.5), start seconds; the node's **name** is the sound asset; stereo is summed to mono, any asset rate is resampled |
| Delay (17) | signal, time offset seconds | time s, feedback (0..0.95), mix, max length s (0 reads as 1, at most 10); pre-allocated at max length, fractional-delay reads, at least one frame |
| Clamp (18), MapRange (19) | signal | min, max / in min, in max, out min, out max (the input is clamped to its range; ranges may run backwards) |
| EnvelopeFollower (20) | signal | attack s, release s |

The oscillators (Sine, Saw, Square) gained one input, a **frequency offset in Hz added to params[0]**, and LowPass a **cutoff offset**; both read 0 unconnected, so every version-1 graph renders as it did. That is what lets `Parameter -> MapRange -> Sine` be an engine that follows `rpm`. Limits, checked at compile with a reason: 256 nodes, 32 distinct Parameter names, 8 distinct WavePlayer assets, 30 s of delay memory in total.

**Format.** `.ocsnd` content version 2 adds a name chunk (`SNAM`) and the kinds above. A graph that uses nothing new is written as **version 1, byte for byte what the previous build wrote**, so older tools still open it; a graph with a new kind, a name, or a link into an input version 1 did not have is written as version 2. The reader accepts both, refuses a newer version, bounds every count, name length and node index in `SNAM`, and refuses an unknown kind, a duplicate name and truncation. Tested in `tests/sound/SoundNodesTest` (including a version-1 file assembled chunk by chunk as the old writer did, every truncation of a version-2 file, and tampered name chunks).

**Surface.** C++: `Mixer::playGraph` / `setGraphParam` / `setGraphParamId` / `graphParamId` / `graphNoteOff` / `isGraphVoice`. C ABI (`audio_abi.h`, `aver_audio_soundgraph_*`): `load` / `unload`, `param_count` / `param_name` / `param_find` / `param_default` / `duration`, `play` / `play_at` (seed 0 varies per play; initial inputs set before the first block), `set_param` (by name) / `set_param_id`, `note_off`, `is_voice`. C#: `SoundGraph` (`Audio.LoadGraph`), `Audio.PlayGraph` / `PlayGraphAt` (with optional initial inputs), `Voice.SetGraphParam` (name or id), `Voice.GraphNoteOff`, `Voice.IsGraphVoice`. Graph nodes (family `ProceduralSound`, `docs/nodes/ProceduralSound.md`): **Play Sound Graph**, **Play Sound Graph At**, **Set Sound Graph Input** (`param=` names the Parameter), **Note Off Sound Graph**; the voice pin works with the audio-control nodes. The Sound Editor's Kind combo and Add menu list the new kinds (they are tables in `SoundEditor.cpp`); a Parameter or WavePlayer node has a name box. Its Preview is unchanged: it renders offline with Parameters at their defaults and WavePlayers silent, and it does not open the device in any test.

**Semantics worth knowing.** `aver_audio_stop` / `Voice.Stop` still fades a voice out within one block, graph voice or not; the note-off is the separate call that lets an ADSR release. A graph voice ignores the voice **pitch** (the graph makes its own; use a Parameter on an oscillator or a WavePlayer) and the playback speed scale. A graph voice runs at the device rate, and `setOutputFormat` moves it with its time constants kept in seconds; its delay memory is cleared by the move (one audible step in the echo at a device change, like the effects). Pausing a graph voice holds its state.

**Tested** (`tests/audio/SoundGraphTest`, 85 checks; `tests/audio/AudioAbiTest` through the C seam; `tests/sound/SoundNodesTest` and `SynthTest`; `tests/editor/SoundEditorTest`; `Aver.Graph.Tests`; all offline, no device): offline/real-time equality; the legacy pin; parameter changes are click-free (largest sample-to-sample step 0.057 against a 0.057 sine step, with a control where smoothing is off and the same change steps by 0.59); pitch glide with continuous phase (200 to 800 Hz, 200 and 799 crossings per second); a value set at play time is there from the first sample; note-off in the sustain, during the attack (releases from the level reached), on a finite graph, on a graph with no ADSR, and `stop()`; pool exhaustion (8 graph voices into a pool of 4, handles of stolen voices dead, sounds released, plain and graph voices stealing each other, no state carried over); a sample-rate change under a playing graph; volume, fade and pan on a graph voice; no allocation in `mix()`; hostile runner use.

**Headed checks for the owner (what only listening can decide).** Nothing here opens a device; these need a real one:

1. **Engine loop.** Build a graph `Parameter "rpm" (800..6000) -> MapRange (40..300 Hz) -> Sine` plus a low `Noise` through a `LowPass`, play it looping from a script (`Audio.PlayGraph(g, looping: true)`), then sweep `rpm` with `Voice.SetGraphParam`. Expect a smooth glide with no zipper noise or clicks; a sudden jump should sound like a quick (20 ms) swoop, not a step.
2. **Release.** Play an ADSR graph looping, call `GraphNoteOff` while it sustains and again during the attack. Expect the tail to fall smoothly from where it was, with no click at its end, and no stuck voice afterwards.
3. **WavePlayer.** A graph with a `WavePlayer` on a real recording and a Parameter feeding its pitch input: pitch should follow smoothly and the loop seam (loop on) should match what the plain `.ocaudio` loop does.
4. **Filters and Delay.** Sweep the cutoff of `HighPass` / `BandPass` by Parameter: resonance should sing, not ring or blow up; the Delay's echo should repeat and fade, and changing its time by a Parameter should bend the echo (it is interpolated), not crackle.
5. **Crossfade.** Three layers (idle, mid, full) under a Crossfade driven by one Parameter: no level dip or bump between layers (equal power), no click as the Parameter moves.
6. **Load.** Fifty graph voices at once, then a device change (unplug or switch the default output) while some play: no dropout beyond the normal device-change gap, nothing stuck, the Output Log quiet apart from the one device line.
7. **Sound Editor.** Add each new kind from the Add menu, link a Parameter into an oscillator's frequency input, rename the Parameter, save, reopen: the graph and its names come back. Preview plays the offline render at the Parameter defaults.

## 14. Surround output and binaural

Two opt-in outputs on the same mixer and under the same real-time rule. **Both are off by default**: a project that asks for neither is mixed in stereo with the constant-power pan it always had, and `tests/audio` pins that bit for bit.

**Layouts.** The mixer now renders 1, 2, **6 (5.1)** or **8 (7.1)** interleaved channels (`Speakers.hpp`). The order is the standard one that `WAVEFORMATEXTENSIBLE` (ascending speaker-mask bits) and SDL use for these counts: 5.1 is `L R C LFE Ls Rs` (surrounds at +-110 degrees), 7.1 is `L R C LFE Lb Rb Ls Rs` (backs at +-150, sides at +-90, fronts at +-30). `Mixer::init` and `setOutputFormat` take those four counts; a device change between layouts keeps every voice (they take the new gains at once on the next block instead of ramping from the wrong layout's gain).

**Asking for it.** The project says what it wants and the device says what it can do:

| Where | How |
| --- | --- |
| `.ocproject` | `AUDIO.SPEAKERS 6` or `8` (anything else, and the key's absence, is stereo); `AUDIO.BINAURAL 1`. Owned keys, so a re-save keeps or drops them. `GameTick.hpp` `applyProjectAudioMix` passes them to the ABI. |
| C ABI | `aver_audio_set_speakers(2/6/8)` / `aver_audio_speakers`, `aver_audio_set_binaural` / `aver_audio_binaural`, `aver_audio_set_lfe_send` / `_lfe_send`, `aver_audio_set_stereo_upmix` / `_stereo_upmix`. Remembered whether called before or after `aver_audio_init`. |
| C# | `Audio.Speakers` (`SpeakerLayout`), `Audio.Binaural`, `Audio.LfeSend`, `Audio.Upmix` (`StereoUpmix`). `Audio.Channels` is what the mixer really renders. |
| C++ | `Mixer::setSpeakerRequest`, `setBinaural`, `setLfeSend`, `setStereoUpmix`. |

`chooseMixChannels(deviceChannels, mask, requested)` decides at open: a request of 6 or 8 is honoured only on a device with that many channels whose speaker mask is a real 5.1 (front pair, centre, LFE and a back or side pair) or 7.1; 8 on a 5.1 device is 5.1; anything else, and any request of 2, is stereo (a one-channel device is mono). `OutputInfo::channelMask` is how the backend reports the mask (WASAPI reads `WAVEFORMATEXTENSIBLE::dwChannelMask`; SDL reports none and its 6 and 8 channel layouts are the standard order). Changing the request while a device is open makes the output driver reopen it the way a default-device change does (`output device changed (speaker layout changed)`; a short gap, voices and buses kept, and no flapping wait). `buildDeviceMap` says which mixer channel feeds each device channel: a 5.1 mix on a 7.1 device puts its surrounds on the side speakers, a stereo mix goes to the first two channels as it always did; the WASAPI backend builds the map in `start()` and copies with it in `service()`. The idle sink used while there is no device is sized for the widest layout.

**Panning (`speakerGains`).** A positional voice is placed by its azimuth in the listener's frame (0 ahead, positive right, from the listener's `forward` and `right`; up is their cross product) with **pairwise constant-power panning** on the ring of directional speakers sorted by angle: between two neighbours the gains are `cos` and `sin` of the fraction of the way across, so a source exactly at a speaker plays only that speaker, `sum(g * g) == 1` at every angle (checked at every degree from -360 to 360 for both layouts), and no gain moves more than 0.02 per tenth of a degree anywhere, across the back included. The LFE never receives a panned source. A source off the horizon, overhead or inside the head is **spread** in the power domain from the pair toward all directional speakers equally (`spread = 1 - horizontal share`), so it never jumps and still has unit power. The result is scaled by the same 1/cos(45 degrees) as the stereo pan, so a point source at the same level is as loud in 5.1 as at the centre of the stereo pan (total power 2 g squared). There is no back attenuation in surround (the legacy stereo mix dims the rear by up to 3 dB because two speakers cannot say "behind"; five can). A Godot-spatial voice uses its own distance model, max distance, cone and attenuation filter for level and filter; its panning strength scales how far it leaves the spread (Godot's default 0.5 pans fully). Occlusion, the reverb send, fades, pause, stealing and the stop ramp behave as on stereo.

**Stereo assets (`stereoUpmix`).** A non-positional stereo (or mono) source, the reverb return and the bus graph's Master are folded into the speakers by a matrix. **Wrap** (default): left goes to the front left and, at a quarter of its power, to the surround(s) on the left (5.1: Ls; 7.1: Lb and Ls), right likewise; the centre and LFE stay silent so a centred image is still a phantom one; each input channel keeps unit power (`0.866` front, `0.5` surround in 5.1, `0.354` each in 7.1). **Front**: the front pair only. Voices routed through the bus graph (`graphBus`) are mixed in stereo inside the graph and upmixed at the end, so they are not panned across 5.1 and not binaural-capable when the device is surround; legacy-bus voices are.

**LFE send.** Off (0) by default, and then the LFE channel is exactly zero. `lfeSend` 0..1 sends each voice's mono mix at its final level (distance, volume, bus, fade, occlusion) to the LFE, summed and low-passed once by two cascaded sections at 120 Hz (`mix()` keeps one pre-sized scratch buffer; the filter keeps running on silence until it has rung out so switching it off does not click).

**Binaural.** `Mixer::setBinaural(true)` on a **stereo** mixer replaces the pan of every positional voice with an analytic head model (`Binaural.hpp`). No measured HRTF data set is used, downloaded or shipped. Per voice, fixed-size state (a 256-frame delay ring, two filters), nothing allocated in `mix()`:

- **Interaural time difference.** Woodworth's spherical head, radius 8.75 cm, 343 m/s: `(a / c)(alpha + sin alpha)` with alpha the angle from the median plane, so a source at 90 degrees leads one ear by 0.656 ms (31.5 samples at 48 kHz) and the delay is 0 ahead and behind. Both ears are delayed around half the maximum, so the delay is continuous when a source crosses the median plane (a constant 0.33 ms of latency on a binaural voice). Read with linear interpolation.
- **Head shadow (level difference).** Brown and Duda's first-order pole-zero filter per ear, `(1 + alpha s / 2w0) / (1 + s / 2w0)`, `w0 = c / a`, ears at +-100 degrees: unity gain at DC and `alpha(theta)` at high frequencies, from 2 (the ear nearest the source, +6 dB) to 0.1 (shadowed, 150 degrees round), bilinear-transformed per sample from the interpolated alpha. A front/back tilt (`1 + 0.5 * ahead`) makes sources behind duller than sources ahead. The level difference is therefore a high-frequency effect: a low tone has the delay but nearly the same level in both ears, and a white impulse is about 17 times more energetic in the near ear than the far one for a source at 90 degrees.
- **Pinna and elevation.** One reflection (30% of the direct sound, normalised to unity at DC) whose delay runs from 83 microseconds for a source below to 50 above, so its first notch moves from about 6 kHz to 10 kHz with elevation (measured 6.6 kHz at -60 degrees and 9.6 kHz at +60).
- **No zipper noise.** The delays, shadow gains and pinna delay are interpolated per sample across each block from where they were; a delay may move at most half a sample per sample, so even a teleport bends pitch by a bounded amount instead of clicking. The first block a voice uses the model snaps, the model restarts (cleared, snapped) on a device rate change, and a voice whose binaural flag is off keeps no state.

Binaural applies to positional voices only. Non-positional voices, mono, 5.1 and 7.1 mixers are untouched whatever the flag says. It models a generic head: it is not personalised and will not externalise sounds for everybody the way a measured set can.

**Tested offline** (`tests/audio/AudioSurroundTest`, 127 checks, and `AudioAbiTest`, `FormatTest`, `AbiEnumTest`; no device opened): the pan law's power, the exactness at each speaker, spread and continuity for 5.1 and 7.1; sources at known angles through the mixer (0, +-30, +-90, +-110, +-150 and between speakers, overhead and behind, a turning listener) with the loudest channel, the total power (0.5) and a silent LFE; the upmix matrices and a left-only stereo asset in both layouts and both modes; the reverb return reaching the surrounds; the LFE (silent by default, a 40 Hz tone through, a 2 kHz tone filtered out, switch-off ramps to exact silence without a click); `chooseMixChannels` for stereo, mono, 5.1 and 7.1 devices, side and back masks, a quad device and a 5.1+2 device, and `buildDeviceMap` for each; the output driver against a fake device (request honoured, ignored on a stereo device, reopened as 7.1 and back with the voice kept, quick successive changes, no device with an 8-channel mixer then the device returning); `setOutputFormat` between all four layouts; the head model (ITD formula against Woodworth, sign and size through the mixer by cross-correlating the ears, 31.5 samples measured at 32.2 for 90 degrees and the others within 0.7 samples, near ear louder, far ear duller, front brighter than back, the pinna notch moving with elevation); smoothness (a source circling the head every 2 seconds, and a 180 degree teleport, make no larger sample steps than a still source, 0.034 against 0.034 and 0.036; in 5.1 the same); **defaults unchanged** (a stereo mixer with a speaker request of 8, an LFE send and the Front upmix renders bit-identical samples to a default one; binaural switched on and off is identical; with it on, non-positional voices are identical; it is ignored on mono and 5.1; and `AudioDeviceTest`'s recorded golden of the pre-change mixer still matches); and **0 allocations** in 300 blocks of moving voices, LFE changes, reverb and occlusion at 2, 6 and 8 channels, counted with a replacement `operator new`.

**Not tested here, only compile-checked (headed checks for the owner).** No device was opened. The WASAPI channel mask and map, the SDL 6 and 8 channel streams and the whole sound of the head model can only be judged on hardware. SDL's backend change is one assignment and was not compiled on this machine (the SDL headers are only fetched on the platforms that use them). Listen for:

1. **5.1 or 7.1 output.** In Windows' sound settings give the output device 5.1 (or 7.1) and set `AUDIO.SPEAKERS 6` (8) in the project. Expect one `output device changed (...)` line naming `5.1` (`7.1`) when the game starts or when the key is changed with the game running, and `Audio.Channels` reporting 6 (8). If the device is stereo or its speaker configuration is stereo, expect stereo and no change.
2. **Speaker test.** Play a looping noise at a position and circle the listener around it with the camera (or walk the source around the camera): the sound should move smoothly round the room through front, side, back, with no speaker jumping, no dead spot between two speakers (constant loudness) and the centre speaker only in front. A source overhead should come from everywhere equally.
3. **Channel order.** With a real speaker setup, a source ahead must come from the centre and a source on the right from the right speakers (not swapped with the LFE or the surrounds). On a 7.1 device with `AUDIO.SPEAKERS 6` the surrounds must come from the side speakers.
4. **Stereo music in 5.1.** Music should sit in front with a gentle ambience from the surrounds (Wrap) and nothing in the centre or subwoofer; `Audio.Upmix = Front` should leave the surrounds silent. A mono dialogue clip that is not positional should come from the front pair as a phantom centre.
5. **LFE.** With `Audio.LfeSend = 0` the subwoofer is silent. At 0.5 explosions and engines should add bass to the subwoofer, no hiss or clicks on switching it on and off.
6. **Binaural (headphones, stereo output).** `AUDIO.BINAURAL 1` or `Audio.Binaural = true`, headphones on: a looping sound circled around the camera should move left, behind and right with no zipper or clicking as it moves; the left/right position should be clear; front versus back should be audible as brightness (back duller); above versus below is a subtle cue and may be weak. Compare against the plain pan: it should sound more outside the head and more lateral, not harsher. If sounds at the side are too bright or sibilant, that is the +6 dB near-ear shelf and is a tuning constant (`kShadowMinAlpha`, the shadow's alpha range) to retune by ear.
7. **No change by default.** A project without the keys sounds exactly as before on stereo and on a surround device.
8. **Switching the layout live.** Toggle `Audio.Speakers` between stereo and 5.1 while music and a looping effect play: one short gap, then sound continues in the new layout with the music at the same point.

## 15. Environment: zone reverbs, convolution reverb, diffraction

Three additions under the same real-time rule, **all off by default**: a project that uses none of them renders the samples it always did. `tests/audio/AudioEnvironmentTest` pins that (a mixer with every zone slot configured and every voice sending, but at wet 0, is bit-identical to one that never heard of zones) and `AudioSceneTest` pins that a scene without the new component settings makes no new call into the mixer.

### Zone send reverbs

The listener-side reverb (`Mixer::setReverb`, driven by `CReverbZone` as in [AUDIO_STREAMING.md](AUDIO_STREAMING.md)) is unchanged: one reverb, blended by where the **listener** stands. A source in another room was not reverberated by its own room. Zone sends fix that.

- **The mixer pre-allocates `kMaxZoneReverbs` (4) send reverbs** at `init` (`Reverb.hpp` / `Mixer.hpp`): each is the algorithmic reverb or a convolution (below) with a mono send bus, so turning one on later costs the audio thread nothing. `Mixer::setZoneReverb(slot, {wet, decaySec, damping, impulse, predelayMs})`; wet 0 turns a slot off. A slot's decay and damping ease on the audio thread (120 ms) once it is running; wet is ramped over one block like the legacy return. `Mixer::setVoiceZoneSend(voice, slot, 0..1)` is a latest-value atomic per voice and slot, 0 until set; the audio thread ramps each voice's gain linearly across the block (sample-exact, checked against the closed form to 3e-6), so a send never steps. A slot with nothing sending and wet 0 is off, costs nothing and has its tail cleared (a slot turned off and on again rings out exactly as on a fresh mixer).
- **The scene does the assigning** (`modules/audio.scene`). `CAudioOcclusion::zoneSends = 1` makes an emitter's voice send to the zones it is inside; `reverbSendBase` (default 1) is the voice's overall reverb level, what its reverb send would be without zones (1 on Sfx and Voice, 0 on Music and Ui in the mixer's own defaults). Each tick the system takes every such voice's weight in every `CReverbZone` (the same smoothstep over `blendDistanceCm` as the listener blend), gives the **four most relevant zones** a slot (a zone with a voice inside and no slot takes a free slot, then the slot of a zone nobody is in, then, only for a higher priority, the lowest priority slot; a slot is held for the zone's `decaySec` after the last voice leaves so its tail rings out, then released with wet 0), and sets each voice's amount into each slot by **priority blend** (`zoneShares`: the highest priority zone takes its weight first, the next takes what is left, and so on; the same algebra as `blendReverbZones`, checked equal to it along a sweep). What no slot took stays on the listener-side reverb: the tracked voice's listener-side send becomes `reverbSendBase * remainder`. So a voice in a cave rings in the cave wherever the listener stands, a voice in the open heard from inside the cave still gets the listener's room, and the two cross-fade continuously as either moves (the sum of the voice's sends and its remainder is always its level; no send moves by more than 0.12 per 10 cm through two overlapping 200 cm blends, in `AudioSceneTest`).
- **A zone carries its settings** (`CReverbZone`): `wet` is its slot's return level, `decaySec` / `damping` its algorithmic reverb, `impulseSound` (a mixer sound handle; 0 = algorithmic) and `predelayMs` its convolution. The slot's settings follow the component, pushed only when they change. Entities carry no mixer state; a zone with no tracked voice inside has no slot.
- **Smoothing the listener-side blend.** `AudioSceneSystem::setReverbSmoothing(tauSec)` (default 0 = off) makes the applied wet, decay and damping ease toward the blend with that time constant, so a listener crossing a hard zone edge or a priority change eases instead of stepping (0.25 is a reasonable start). Off, the blend is applied as computed, as before.
- **Limits.** Four zones at once; a fifth is dropped until a slot frees (the highest priorities keep theirs). The slot's return is mixed at the listener's ear, not placed at the zone: it does not get quieter as the listener walks away from the room it is in, only through the voice's own attenuation. A zone's wet level is the same however many voices send to it (it is a send reverb, not a room simulation). A convolution slot and an algorithmic slot are not level-matched: the algorithmic reverb has its own input gain, the convolution plays the impulse as authored, so set `wet` per zone by ear.

### Convolution reverb

`Convolver.hpp`: a **uniformly partitioned FFT convolver** with its own radix-2 FFT (`Fft`; no dependency), overlap-save, 256-frame partitions (a power of two from 32 to 8192 at construction). Both channels of a stereo signal go through one complex transform (left in the real part, right in the imaginary, separated by conjugate symmetry), a one-channel impulse feeds both ears and a two-channel impulse convolves left with channel 0 and right with channel 1 (more channels: the first two). The impulse is turned into partition spectra on the game thread when it is set (`setImpulse`, any rate: **linear-interpolation resampling** to the device rate, so a high-rate impulse is not anti-aliased and an up-sampled one is slightly dulled, which a reverb tail hides; NaN and infinity become zero; truncated to 12 s) and handed to the audio thread through an atomic pointer; the audio thread takes it at the next block and **crossfades over one block** from the old one, which goes to a small retire list the game thread frees (`collect()`, called by `Mixer::collect()`; if the list is full the new impulse waits rather than the audio thread freeing anything). `process()` is arithmetic over pre-sized arrays: no allocation, lock, wait or log. **Latency is exactly one partition** (256 frames, 5.3 ms at 48 kHz): `out[n] = (in * ir)[n - 256]`, whatever the length of the impulse; a longer impulse costs more partitions, not latency. Any chunking of `process()` calls renders the same samples bit for bit, and once the tail has played out and the input has been silent for the impulse length the engine skips its work and outputs exact zeros. Wet predelay (up to 0.5 s) is a pre-allocated ring.

- **As a bus effect:** `EffectKind::ConvolutionReverb`, class name `"AudioEffectConvolutionReverb"` (`aver_audio_effect_add(bus, "AudioEffectConvolutionReverb", at)`, an `.ocbus` layout, `AudioEffectConvolutionReverb` in `Aver.Fabric`). Parameters `dry` (1), `wet` (0.5), `predelay_msec` (0..500, 0), `gain_db` (-24..24, 0); the impulse comes from a loaded sound: `aver_audio_effect_set_impulse(effectId, sound)`, `Mixer::setEffectImpulse`, `AudioEffectConvolutionReverb.SetImpulse(sound)`. With no impulse loaded the effect is the dry signal bit for bit. The wet signal lags the dry by one partition (`latencyFrames()` says so; the dry path is not delayed). The effect keeps its own copy of the impulse (unloading the sound is safe) and rebuilds it for the new rate on a device rate change (`BusGraph::setSampleRate`).
- **As a zone's reverb:** `ZoneReverb::impulse` / `aver_audio_set_zone_reverb(..., impulseSound, predelayMs)` / `CReverbZone::impulseSound`. The slot convolves its mono send with the impulse; `setOutputFormat` rebuilds it for the new rate; `impulse 0` returns the slot to the algorithmic reverb (switch while the slot is quiet: the other reverb's tail is not carried over).
- **Where the impulse comes from.** A sound in the mixer, so any file the loaders decode (WAV, `.ocaudio`, Ogg Vorbis, FLAC, MP3, Media Foundation formats on Windows) once it is loaded with `aver_audio_load` / `Audio.Load`. The test round-trips an impulse through `.ocaudio`; the decoders themselves are tested by `AudioDecodeTest`.
- **Cost** (`AudioEnvironmentTest` reports it every run): a 2 s stereo impulse at 48 kHz is 375 partitions; on the build this tree tests in (CMake Debug, `/Od`) one 256-frame block took a mean of **0.46 ms** (p99 0.60 ms, max 0.66 ms) of the 5.33 ms the block lasts, 8.7% of one core, and four partitions in one call 1.85 ms of 21.3 ms. The work is the same however the audio is chunked (one multiply-accumulate per bin per partition per ear), so it scales with impulse length and the number of live convolvers: with four zones and an effect each on a 2 s impulse, budget about five times that. The test only fails if a block takes more than six times real time even unoptimised; it does not assert the figure, which depends on the machine.

### Diffraction

`Music.hpp`: `findDiffractionPath`, `applyDiffraction`, `measureOcclusionDiffracted`. When the direct path reads blocked (occlusion above `trigger`, 0.02) the probe searches for the **shortest one-bend path listener to P to source** whose two legs the ray function reports clear (a leg blocks at 0.5). Waypoints P lie on rings around the line of sight (8 directions by 3 positions along it, ring offsets 100, 200, 400 ... 3200 cm); the first ring with a clear line is finished, the shortest four lines are tightened toward the edge by bisection (3 steps), and the best is used. This finds a path **around an edge** (offset just past the corner) and **through an opening** (the waypoint lands in the gap); it uses the existing `RayBlockFn` hook (the same Jolt raycast), at most `maxRays` calls (256; a fully blocked search over six rings spends about 190, a found one usually 50-100). The result is an occlusion value for the voice's existing gain and low-pass, so the audio thread needs nothing new and no delay is added:

`occlusion = maxOcclusion * (1 - pathGain * bendGain)`, with `pathGain = direct length / bent length` (the inverse-distance ratio of the longer route) and `bendGain = (1 + cos(deflection)) / 2` (1 straight on, 0 doubling back), `maxOcclusion` 0.85. It rises with the detour and with the sharpness of the bend and stays below the cap, so a bent path is always more open than the fully occluded wall. `applyDiffraction(direct, path)` is `min(direct, path.occlusion)`: continuous in the direct measurement, never above it. Smoothing is the emitter's existing rise and fall times (`smoothOcclusion`), so the value changes over `riseSec` / `fallSec`, not in a step. With the wall's half-width going 50, 100, 200, 400, 800 and 1600 cm the test sees the gain fall from 0.99 to 0.42 monotonically, the detour match the geometry of the edge to within the bisection's resolution, and a doorway (a 400 cm gap centred 400 cm off the line of sight, in a wall 6000 cm wide) found at a detour of 97 cm against 5083 cm around the end of the wall.

`CAudioOcclusion::diffraction` (default 0) switches it on per emitter, with `diffractionDirections`, `diffractionMaxRays`, `diffractionMaxOffsetCm` and `diffractionMaxOcclusion` (0 in any of these four means the default, because a component added from script arrives zero-filled); `detourCm` is read-only. `Entity.SetAudioDiffraction(...)` in C#. With it off, the emitter's measurement is the plain ring probe it always was. It is a probe, not an acoustic simulation: one bend, a hard clear-or-blocked test per leg, no frequency-dependent Fresnel model (the low-pass comes from the occlusion curve, `OcclusionCurve`), no second-order paths and no portals. A narrow slit or a corner that needs two bends reads as fully occluded.

### Where each thing is

| Thing | C++ | C ABI | C# | Scene |
| --- | --- | --- | --- | --- |
| Zone send reverb | `Mixer::setZoneReverb`, `setVoiceZoneSend` | `aver_audio_set_zone_reverb`, `_set_voice_zone_send` | `Audio.SetZoneReverb`, `Voice.SetZoneSend` | `CReverbZone`, `CAudioOcclusion::zoneSends`, `Entity.SetAudioZoneSends` |
| Convolution reverb | `Convolver`, `EffectKind::ConvolutionReverb`, `Mixer::setEffectImpulse` | `aver_audio_effect_add(..., "AudioEffectConvolutionReverb")`, `_effect_set_impulse` | `AudioEffectConvolutionReverb` (Fabric), `Entity.SetReverbZoneImpulse` | `CReverbZone::impulseSound` |
| Diffraction | `findDiffractionPath`, `measureOcclusionDiffracted` | (through the scene) | `Entity.SetAudioDiffraction` | `CAudioOcclusion::diffraction` |

### Tested offline

`tests/audio/AudioEnvironmentTest` (new, no device) and the additions to `AudioSceneTest` and `AudioAbiTest`: the FFT against a naive DFT at sizes 2, 8, 64 and 512 and its inverse; the convolver against a **double-precision direct time-domain convolution** for impulse lengths 1, 100, 255, 256, 257, 512, 700 and 4000 on both channels with independent noise (max error 1e-5 on a peak of 39, relative 2.6e-7), a one-channel impulse on both ears, the first block exactly silent (latency = one partition), impulses on every partition boundary of the input and the impulse response, and chunks of 1, 7, 100, 255, 256, 257 and 1000 frames bit-identical to one call; predelay; an impulse swap (the gain goes 1 to 2 monotonically over at most one block, never overshooting); exact silence after the tail and a correct restart after a long silence; NaN and infinity in an impulse, empty and bad arguments, a 13 s impulse cut to 12 s, an impulse at 24 kHz resampled and rebuilt for 44.1 kHz, an `.ocaudio` round trip; the convolution reverb effect (dry bit-exact with no impulse, `out = in + 0.5 in[n-256]`, predelay and gain, kept across `BusGraph::setSampleRate`, through the mixer's graph); the zone slots in the mixer (wet 0 identical to no zones; two slots adding up exactly to their separate renders; independent decays; a convolution zone's pulse at exactly impulse offset + 256 + predelay at its authored level; a send stepped 0 to 1 is a one-block linear ramp checked sample for sample; a re-enabled slot rings as on a fresh mixer; `setOutputFormat` keeping a slot's impulse); **0 allocations in `mix()`** over 120 blocks with two zone slots (one convolving), a convolution effect, an impulse swap picked up on the audio thread and changing sends, counted with a replacement `operator new` (with a control showing the hook sees allocations); `zoneShares` summing to 1, agreeing with `blendReverbZones` along a sweep and moving smoothly; diffraction as above plus the endless wall (no path, search stops at its budget), a budget of 4 casting at most 4, a clear line of sight casting only the five probe rays, and the bent value heard louder and brighter than the fully occluded one through the mixer; and in the scene the slot assignment, four-slot limit, priority eviction, release after the tail, defaults off, smoothing and diffraction through a real `scene::World`.

### Not tested here, only compile-checked (headed checks for the owner)

No device was opened, so what the zones, the convolver and the diffraction **sound** like is untested: the checks above prove the arithmetic and the continuity, not that it sounds good. Listen for:

1. **Convolution reverb on a bus.** Add `AudioEffectConvolutionReverb` to the Sfx bus, load a real room or hall impulse response (a WAV a few seconds long) and give it to the effect. Footsteps and a snap should sound in that room; `wet` 0 should be the dry sound; `predelay_msec` 20 should push the tail back audibly. A very long impulse (8 s) should not glitch or raise the CPU use visibly. The wet signal is 5 ms behind the dry; on a sharp transient (a clap) listen for any flam. Swap the impulse while a sound plays: a short crossfade, no click.
2. **Zone reverbs.** Two rooms with different reverbs side by side (one tiled, one carpeted), a looping sound in each. Walk between them: the room you leave fades out and the other fades in, no pop, no step in loudness as you cross the blend. A sound in the room you left should keep its room's reverb as you walk away. Four overlapping zones should not crackle; a fifth should simply have no reverb of its own.
3. **Convolution zone.** Give one zone an impulse response: it should sound like that space and be audibly different from the algorithmic zone next to it; set its `wet` by ear, it is not level-matched to the algorithmic one. Change the zone while a sound plays inside it.
4. **Diffraction.** An emitter on the far side of a wall with a corner or a doorway nearby, diffraction on: standing behind the wall the sound should be duller and quieter than in the clear but clearly more present than with diffraction off (where it drops to the muffled value); moving toward the corner or the doorway should open the sound up gradually, with no step. A wall with no way round should sound exactly as it did without diffraction.
5. **No change by default.** A project that uses none of this sounds exactly as before.
6. **Cost.** With `AverEditor` or the game running and a few zones and a 2-3 s impulse, the audio thread's use should stay a few percent of one core; if sound crackles the convolver is the first suspect (`Mixer::activeVoices`, the underrun counter).
