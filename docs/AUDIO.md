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
- **There is no streaming and no Ogg Vorbis importer.** `modules/audio/README.md`'s own "what it does
  not do" list says so directly: "no streaming — every sound is resident." §4's resident-vs-streamed
  design and §2's `stb_vorbis` choice were never built; only a WAV reader (plus, per
  `Aver.Formats.Audio`, Windows Media Foundation for compressed formats other than Ogg) exists.
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
- Stereo panning first. **HRTF is explicitly not in scope** for the first pass — it is a convolution
  and a data set, and it is a poor use of the effort before anything makes a sound at all.
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
  kind that is written once and never exercised. It needs a deliberate test.
- **The CLR and the audio thread.** §3 forbids managed calls from it. Whether anything in the
  scripting bridge makes that hard to hold is not yet known.
- **Sample-rate mismatch on a device change**: assets resampled at import to 48 kHz are wrong for a
  44.1 kHz device. Either resample on load or keep the source rate in the file and resample per
  voice; not decided.
