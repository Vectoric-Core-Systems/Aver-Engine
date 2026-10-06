# Audio streaming, crossfades, occlusion and reverb zones

Feature 9 of `docs/plans/GAME_SYSTEMS_PLAN.md`. Streamed playback for long files, music crossfades,
raycast occlusion (low-pass plus volume) and reverb zones, in the existing mixer. Nothing here
touches a device: all of it is exercised headlessly by `tests/audio`.

| Layer | Where |
|---|---|
| Decoder thread, ring buffer, WAV streaming | `modules/audio` (`Stream.hpp`, `WavStream.cpp`) |
| Per-voice fade, occlusion filter, reverb send, stream voices | `modules/audio` (`Mixer.*`, `Dsp.*`, `Reverb.*`) |
| Music crossfader, occlusion ray probe | `modules/audio` (`Music.*`) |
| Media Foundation streaming, `.ocaudio` fallback | `modules/audio.abi/src/StreamSources.*` |
| C ABI | `modules/audio.abi` (`audio_abi.h`) |
| `CReverbZone`, `CAudioOcclusion`, `AudioSceneSystem` | `modules/audio.scene` (new target `Aver.Audio.Scene`) |
| C# | `Aver.Framework`: `AudioStreaming.cs`, `AudioStreamGraph.cs`, `AudioVolumes.cs` |
| Tests | `tests/audio`: `AudioStreamTest`, `AudioSceneTest` |

## Streaming

`Mixer::playStream(source, desc, params)` starts a voice that reads from a `StreamDecoder` instead of a
resident `SoundData`. The decoder owns a `StreamSource`, a single-producer single-consumer ring of
decoded frames (default two seconds of source audio) and a thread that keeps the ring full. The audio
thread only reads atomics and the ring: it never waits for the decoder.

- **Prebuffer.** The voice is silent, and does not advance, until a quarter second is queued (or the
  stream is already complete). Waiting is not an underrun, and a fade-in starts counting only when
  sound does.
- **Under-run policy.** If a block needs frames the ring does not have, the mixer renders what it can,
  fades the last up to 64 frames to zero, leaves the rest silent, does not advance the cursor and counts
  one underrun (`Mixer::streamUnderruns`, `aver_audio_stream_underruns`, `StreamDecoder::underruns`).
  The voice stays alive. When data returns the next block ramps in from zero over the block, so neither
  edge steps. A voice being stopped while starved ends at once.
- **End of stream.** Looping is done by the decoder thread (seek to the loop begin, so the ring is
  continuous with no seam); the voice never sees a loop. A non-looping stream ends when the last frame
  has been played. Loop points come from `StreamParams`, or from an `.ocaudio` header.
- **Pitch and rate.** The voice reads the ring with the same linear interpolation as a resident sound,
  so a 44.1 kHz file on a 48 kHz device and a pitch change both work. A stream renders bit-identically
  to the same data played resident (`AudioStreamTest`).
- **Lifetime.** The decoder lives in a fixed stream slot (16 by default, `Mixer::init(..., maxStreams)`).
  When the voice ends, is stopped or is stolen, the audio thread only clears an `attached` flag;
  `collect()` (game thread, once a frame: `aver_audio_collect`) destroys the decoder and joins its thread.
  `playStream` calls `collect()` itself.
- **Hosts without a thread.** `StreamParams::threaded = false` plus `Mixer::pumpStreams(frames)` runs
  the decoder from the caller. The tests use it for deterministic under-runs.

### Decoders

Only decoders the engine already had, used incrementally where they allow it:

| Source | Streams? |
|---|---|
| `.wav` (8/16/24/32-bit PCM, 32-bit float, mono or stereo) | Yes, bounded memory, own reader (`openWavStream`) |
| `.mp3 .m4a .aac .wma .flac` through Media Foundation (Windows) | Yes, `IMFSourceReader::ReadSample` one sample at a time, seek via `SetCurrentPosition`. More than two channels are folded to stereo by the decoder or refused. **Untested**: needs real media and a device. |
| `.ocaudio` | **No.** The container holds raw f32 in one AVR1 chunk with no incremental reader. It is decoded whole and replayed from memory, with a warning. Ship music as WAV, FLAC or MP3 to stream it. |
| anything else, or no Media Foundation | decoded whole by `audioImportFile`, replayed from memory, with a warning |

`.ogg` has no decoder in the engine and still does not.

## Crossfades and fades

Every voice has a fade gain on top of its volume, ramped on the audio thread so a fade is
sample-accurate whatever the game's frame rate. `Mixer::fadeVoice(v, target, seconds, curve, stopAtEnd)`;
`PlayDesc::fadeInSeconds` fades a new voice up from silence; `stopAtEnd` ends the voice when the ramp does.
The gain at the end of each block is computed exactly and the existing per-block gain ramp interpolates
to it.

Curves (`FadeCurve`): `Linear`, and `EqualPower` (sine in, cosine out, so overlapping tracks keep
constant power; each is -3 dB at the midpoint). `crossfadeGains(p, curve, out, in)` is the pure form.

`MusicCrossfader` is the music slot: `play(source, options)` starts the new track fading in and fades the
current one out and ends it; a failed start keeps the old track. C ABI: `aver_audio_music_play/stop/voice`.

## Occlusion

`Mixer::setVoiceOcclusion(v, 0..1)` (`aver_audio_set_voice_occlusion`, `Voice.Occlusion`). On the audio
thread the value is smoothed (80 ms time constant), then the voice is scaled toward a volume floor and
run through a 12 dB/octave low-pass whose cutoff falls from 20 kHz (bypassed below 0.002) to a floor,
log-interpolated. Defaults: 450 Hz and 0.30 at full occlusion, set with `setOcclusionCurve` /
`aver_audio_set_occlusion_curve`. Occluded voices also send less to the reverb.

`measureOcclusion(rayFn, user, listener, source, probe)` is the measurement: one ray from the listener to
the source and a ring of `rayCount - 1` more to points around it (`probeRadiusCm`), each returning a
blocking amount 0..1, averaged. A single ray is a hard switch as someone walks past a door frame; the ring
turns it into a ramp. This is direct-path obstruction only: no diffraction around corners and no
propagation through portals, and every hit blocks equally (the ray function may return less than 1 for a
thin material).

`CAudioOcclusion` (component, `modules/audio.scene`) drives one voice: fields `voice`, `followEntity`
(moves the voice to the entity each tick), `rayCount`, `probeRadiusCm`, `thinkIntervalSec`, `riseSec`,
`fallSec`. The default ray is `aver_phys_raycast` (stopping 10 cm short of the source so its own collider
does not block) when physics is built in. The component forgets the voice once it ends.

## Reverb and zones

A send reverb (8 damped combs into 4 all-passes per channel, feedback derived from an RT60) on the mixer.
`Mixer::setReverb({wet, decaySec, damping})`; wet 0 turns it off and costs nothing. Voices feed it through a
per-voice send (default 1 on the Sfx and Voice buses, 0 on Music and Ui; `setVoiceReverbSend`). The return
ramps over one block when wet changes. A single `mix()` call longer than 16384 frames skips the reverb
rather than allocating.

`CReverbZone` is a box or sphere volume placed by its entity's transform (scale stretches it): weight 1
inside, falling smoothly to 0 `blendDistanceCm` OUTSIDE the surface. Overlaps blend by priority: the
highest-priority zone takes its weight first, lower zones fill what is left, the remainder goes to the
ambient reverb. Wet blends with the ambient; decay and damping blend among the zones only, so a zone
fading out keeps its own tail length. `AudioSceneSystem::tick` reads the listener position back from the
mixer, computes the weights, and pushes the result only when it changed.

`AN_ReverbVolume` (an actor class in `Aver.Framework`) places a box zone from a level with editable
extents, blend, wet, decay, damping and priority.

## C ABI additions (`audio_abi.h`)

`aver_audio_stream_play`, `_stream_play_at`, `_fade_voice`, `_music_play`, `_music_stop`, `_music_voice`,
`_set_voice_occlusion`, `_set_voice_reverb_send`, `_set_occlusion_curve`, `_set_reverb`,
`_stream_underruns`, `_active_streams`, `_get_listener`. Fade curve constants `AVER_AUDIO_FADE_LINEAR` and
`AVER_AUDIO_FADE_EQUAL_POWER` are pinned to the enum by a `static_assert`.

## C#

`Audio.PlayStream/PlayStreamAt`, `Audio.SetReverb`, `Audio.SetOcclusionCurve`, `Audio.StreamUnderruns`,
`Audio.ActiveStreams`; `Voice.FadeTo/FadeOut/Occlusion/ReverbSend`; `Music.Play/Stop/Current`;
`Entity.SetReverbZone`, `Entity.SetAudioOcclusion`. `Audio` and `Voice` became `partial` so the new
members sit in `AudioStreaming.cs`. Silent machines behave as before: every call is a no-op.

## Aver Node nodes

The node vocabulary lives in shared files this feature did not edit. The managed callables exist
(`AudioStreamGraph`, internal, reachable from `Aver.Graph` through its `InternalsVisibleTo`); the
registration is listed under "Wiring for the integration step". Node types:

| Node | Pins (in; out) | Attribute | Calls |
|---|---|---|---|
| `AN_PlayStream` | exec, volume, pitch, looping, bus, fadeInSeconds; then, voice, success | `sound=` | `PlayStreamForGraph` |
| `AN_PlayStreamAt` | exec, x, y, z, volume, pitch, looping, bus, innerCm, outerCm, fadeInSeconds; then, voice, success | `sound=` | `PlayStreamAtForGraph` |
| `AN_PlayMusic` | exec, volume, fadeSeconds, looping, curve; then, voice, success | `sound=` | `PlayMusicForGraph` |
| `AN_StopMusic` | exec, fadeSeconds; then, success | | `StopMusicForGraph` |
| `AN_IsMusicPlaying` (pure) | playing | | `IsMusicPlayingForGraph` |
| `AN_FadeSound` | exec, voice, targetGain, seconds, curve, stopWhenDone; then, success | | `FadeSoundForGraph` |
| `AN_SetVoiceOcclusion` | exec, voice, occlusion; then, success | | `SetVoiceOcclusionForGraph` |
| `AN_SetReverb` | exec, wet, decaySeconds, damping; then, success | | `SetReverbForGraph` |
| `AN_AttachAudioOcclusion` | exec, entity, voice, rayCount, probeRadiusCm; then, success | | `AttachAudioOcclusionForGraph` |
| `AN_SetReverbZone` | exec, entity, halfX, halfY, halfZ, blendDistanceCm, wet, decaySeconds, damping, priority; then, success | | `SetReverbZoneForGraph` |

`curve` is 0 linear, 1 equal-power. An unconnected input pin is 0, as for every node: set a PINVAL for
`volume` (0 is silent, exactly as on `PlaySound`) and for the extents of a zone.

## Wiring for the integration step

1. **`CMakeLists.txt` (root).** After `add_subdirectory(modules/audio.abi)` (line 473, after `modules/scene`,
   `modules/physics`) add `if(AVER_MODULE_SCENE) add_subdirectory(modules/audio.scene) endif()`. It must come
   after `modules/audio.abi` and `modules/physics` because it tests `TARGET Aver.Audio.Abi` and
   `TARGET Aver.Physics`, and before `add_subdirectory(tests/audio)` (line 562), which tests
   `TARGET Aver.Audio.Scene` for `AudioSceneTest`.
2. **`Runtime/CMakeLists.txt`**, next to the `Aver.Synapse.Scene` link (line 175):
   `if(TARGET Aver.Audio.Scene) target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Audio.Scene) endif()`.
   **`sandbox/CMakeLists.txt`**, next to line 266: the same with `Sandbox PRIVATE`. The target carries
   `AVER_MODULE_AUDIO_SCENE=1`.
3. **Registration.** Where `synapse::perceptionSystem().registerComponents(...)` is called
   (`Runtime/src/GameApp.cpp` ~1707, `sandbox/src/SandboxApp.cpp` ~502) add, under
   `#if AVER_MODULE_AUDIO_SCENE` (include `aver/audio/AudioScene.hpp`):
   `aver::audio::audioSceneSystem().registerComponents(scene::World::instance());`
4. **Tick.** In `aver::game::tickGameplayGroups` (`Runtime/include/aver/game/GameTick.hpp`, shared by editor
   and runtime), after the perception tick (~line 158), under the same guard:
   `aver::audio::audioSceneSystem().tick(scene::World::instance(), dt);`
   The listener position is read back from the mixer, so nothing else needs to pass it on.
5. **Graph nodes.** All edits are in shared files:
   - `sandbox/src/GraphNodeDefs.hpp`, after the `SetBusVolume` row (~line 695): one `t.push_back({"AN_...",
     "...", "Audio", {pins}, {attr("sound","Sound")}})` per node in the table above, same shape as the
     `PlaySound` rows (exec pin in, `then` exec out, `voice` int out, `success` bool out).
   - `scripting/csharp/Aver.Graph/OcGraphParser.cs` `AddDefaultPins`, after `case "setbusvolume":`: one case
     per node with the lower-cased type (`"an_playstream"`, ...) and the pins in the table, written like the
     `playsound` case. The `sound=` attribute is already parsed for any node type.
   - `scripting/csharp/Aver.Graph/GraphCompiler.cs`:
     - `IsExecCapableAudioType`: add `"an_playstream" or "an_playstreamat" or "an_playmusic" or "an_stopmusic"
       or "an_fadesound" or "an_setvoiceocclusion" or "an_setreverb" or "an_attachaudioocclusion" or
       "an_setreverbzone"`.
     - `EmitExecAudio`: before the `playsound` branch, route the three path nodes:
       push `Ldstr node.SoundPath` (throw if empty, like `playsound`), then `EmitPullInput` each pin in the
       table's order, `Ldloca` a local int for `voice`, and `Call` `PlayStreamMethod` / `PlayStreamAtMethod` /
       `PlayMusicMethod`; the `success` and `voice` stores are the same tail as `playsound`. Add switch
       cases for the rest, each pulling its input pins in the table's order and calling its method, falling
       into the existing `success` store/pop tail: `an_stopmusic`, `an_fadesound`, `an_setvoiceocclusion`,
       `an_setreverb`, `an_attachaudioocclusion`, `an_setreverbzone`.
     - `EmitPullOutput`, beside `case "issoundplaying":` add `case "an_ismusicplaying": _il.Emit(OpCodes.Call,
       IsMusicPlayingMethod); return;`.
     - `MethodInfo` fields next to `PlaySoundMethod` (~line 4818), by name from `typeof(AudioStreamGraph)`
       with `BindingFlags.NonPublic | BindingFlags.Static`: `PlayStreamMethod` (`PlayStreamForGraph`),
       `PlayStreamAtMethod`, `PlayMusicMethod`, `StopMusicMethod`, `IsMusicPlayingMethod`,
       `FadeSoundMethod`, `SetVoiceOcclusionMethod`, `SetReverbMethod`, `AttachAudioOcclusionMethod`,
       `SetReverbZoneMethod`. Parameter lists are in `AudioStreamGraph.cs`.
   - `docs/AVER_NODE_NODES.md`: a row per node.
6. **Components/Builtins.** None. The components are registered at runtime like `CSynapsePerception`, so
   `modules/scene/include/aver/scene/Components.hpp` and `Builtins.cpp` are untouched and take no id. They are
   captured by the generic save-game walk (`modules/save`) like any registered component. They are NOT
   placed by `.ocworld` PLACE records, which carry no generic component list: a level places
   `class AN_ReverbVolume` (reverb) and attaches occlusion from script or the node.
7. **`docs/ARCHITECTURE.md`**: add `Aver.Audio.Scene` to the module table and the audio dependency list.
8. **Build once.** Nothing in this feature has been built or run. Expect to fix compile errors; the files
   most likely to need it are `modules/audio/src/Mixer.cpp` (the rewritten render loop),
   `modules/audio.abi/src/StreamSources.cpp` (Media Foundation, Windows only) and
   `modules/audio.scene/src/AudioScene.cpp`. `AudioTest` and `OcAudioTest` should pass unchanged: the new
   per-voice state is neutral at its defaults (fade 1, occlusion 0, no reverb, no stream).

## Not done

- No seek or resume for streams (play from the start only); no `.ocaudio` streaming; no Ogg; no more than
  two channels.
- Occlusion is direct-path only (no diffraction or portal propagation). The reverb is one global listener
  reverb: a source in another room is not reverberated by its own room.
- No editor visualisation for zones (wire boxes, gizmos) and no details panel; `AN_ReverbVolume` is
  edited through its class fields.
- The managed layer has no C# unit tests (needs the native DLL beside the host, as for timers).
- Media Foundation streaming, the C ABI and the C# wrappers are unexercised.

## Tests

`AudioStreamTest` (links `Aver.Audio` only): resident-vs-stream equality sample by sample; under-run,
recovery and ramp; a real decoder thread against a ring smaller than the file (skips its exactness check,
saying so, if the host starved the thread); looping seam; WAV 16-bit, 8-bit and a refusal; crossfade gains
and a two-track crossfade through the mixer to 2e-3; per-voice fade-in and fade-out; biquad response and
the mixer's occluded level against `lowpassCoeffs` within 5%; the ray probe; zone distance, weight and
priority blending; reverb tail decay and RT60 ordering. `AudioSceneTest` (links `Aver.Audio.Scene`,
`Aver.Scene`): zones against a real `World` with transform and scale, priority, and occlusion rise and fall
with the mixer and ray faked.
