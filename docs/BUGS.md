# Verified bug list — 2026-08-03

Six independent lenses swept the tree at `f0d6ca4` and produced **36 candidates**. Every one was then
given to a separate verifier prompted to *refute* it — to default to "this claim is wrong" and go
looking for the guard, the caller, or the misread convention that would kill it.

| | |
|---|---|
| Candidates | 36 |
| Verified | **36** (none unverified) |
| Refuted and dropped | 12 |
| **Confirmed** | **24** — 8 high, 14 medium, 2 low |

Unlike the stale-code sweep, there is no ambiguity band here: these are not "unused" findings where
"is it dead?" is a judgement call. Each confirmed entry is code that runs and does the wrong thing,
with the triggering input stated.

**Fixed so far: 4 of 24.** The rest are recorded below with enough detail to act on. Nothing here is
speculative — the twelve that could not be positively confirmed were dropped, not downgraded.

---

## Fixed

### 1. `eulerDegFromQuat`'s gimbal branch corrupted saved levels — HIGH
`sandbox/src/SandboxApp.cpp:178` → now `sandbox/src/EditorEuler.hpp`

The gimbal-lock branch divided by `1 - 2(y² + z²)`, which for this ZYX composition is
cos(pitch)·cos(yaw) — **identically zero exactly when the branch fires**. `atan2` with a ~0
denominator returns whatever the float residue's sign says.

Measured before the fix, `|dot|` of the recomposed quaternion against the original:

| input (roll, pitch, yaw) | read back | \|dot\| |
|---|---|---|
| (0, 90, 30) | (0, 90, 90) | 0.866 — 60° wrong |
| (0, 90, 0) | (0, 90, −180) | **0.000 — a full 180° flip** |
| (0, −90, 45) | (0, −90, 90) | 0.924 |

The band is `|sin(pitch)| > 0.99999` — **0.26° around vertical**, reachable by typing 90 into the
inspector or dragging a rotation ring through vertical. And it was not merely a display glitch:
the level save reads rotations back through this function, so any near-vertical entity was
**persisted wrong to the `.ocworld`**. Save and reload silently reoriented it.

Fixed by dividing by `1 - 2(x² + z²)` = cos(yaw − roll), which pairs with the existing numerator.
The pair was extracted to `EditorEuler.hpp` so it could be tested at all — it had been two `static`
functions unreachable from any test. `EditorEulerTest` now sweeps **3601 pitches from −90 to +90**;
worst `|dot|` is 1.000000.

### 2. The game runtime rendered every frame with the previous frame's camera — HIGH
`modules/runtime.game/src/GameApp.cpp:580`

`Engine::frameStep` runs `onUpdate → beginFrame → onRender → endFrame`, and `beginFrame` takes the
**one and only** snapshot of `PerFrameCB` into the GPU-visible buffer. `GameApp` called `pushFrame`
— which is what calls `setCamera` and `setSkyAtmosphere` — as the first line of `onRender`, *after*
that snapshot. So every pixel of frame N was rasterised with frame N−1's view-projection, camera
position, sky, fog and cloud constants.

Worse than a uniform one-frame lag: `viewProj_` is also what `drawWorld` culls against, so culling
used **this** frame's matrix while the GPU drew with the **previous** one — the two disagreed by
exactly one frame of camera motion, so fast panning could cull geometry that was on screen.

The editor never had it: `SandboxApp` sets its camera in `onUpdate`. That is why no gate caught it.
Fixed by moving `pushFrame` to the end of `onUpdate`.

### 3. AVR1 chunk bounds check wrapped — HIGH
`modules/formats/src/Avr1.cpp:263`

`if (off + onDisk > size)` with both operands `u64` read straight from the file. `off = 2^64−1`,
`onDisk = 2` sums to `1`, passes, and the following `c.data.assign(bytes + off, ...)` reads from
`bytes + 2^64−1`. Rearranged to `off > size || onDisk > size - off`, where neither side can overflow.

`.ocmesh`, `.ocanim`, `.ocskel`, `.ocland` and `.octex` are all AVR1 containers, so this was the
front door for every binary asset the engine loads.

### 4. glTF indices were never bounds-checked — HIGH
`modules/formats/src/GltfImport.cpp:322`

The flat-normal generator indexed `m.positions` and `m.normals` directly with index values from the
file. A glTF naming vertex 40000 in a 12-vertex primitive wrote three floats well outside both
vectors — heap corruption reachable by opening a downloaded model. Every index is now validated
before any is used, and an out-of-range one **refuses the import** rather than being clamped: an
index past the end is not one bad triangle, it is an index buffer that does not describe its vertex
buffer.

---

## Confirmed, not yet fixed

### HIGH

- **`scripting/csharp/Aver.Framework/Character.cs:175`** — `AverCharacter` applies view pitch with
  the wrong sign, so mouse-look is vertically inverted and `LookDirection` disagrees with where the
  camera actually points. Affects every game built on the supplied character.
- **`modules/audio/src/Mixer.cpp:181`** — voice stealing drives a voice `Active → Pending` under the
  audio thread, freeing sound data it is still reading. A use-after-free on the realtime thread.
- **`scripting/csharp/Aver.Scripting.Bridge/HostBridge.cs:745`** — an actor destroying an actor
  during `OnTick` makes `DispTickAll` skip the next actor in the bucket. Classic
  mutate-while-iterating; the skipped actor silently loses a frame.

### MEDIUM

- `modules/rhi/src/RHIShaders.cpp:420` — every raster path transforms normals by the plain world
  matrix, so **non-uniform entity scale mis-shades**. Worth noting the USD importer documents the
  same limitation for its own path.
- `modules/formats/src/OcMesh.cpp:405` — stream-descriptor byte offsets used outside the bounds
  check, which only covered the strides.
- `modules/formats/src/OcAnim.cpp:310` — track offset overflows its own bounds check, giving a wild
  pointer to `memcpy`.
- `modules/formats/src/GltfImport.cpp:112` — negative `byteOffset`/`byteLength` wrap to huge `u64`
  and defeat the bufferView bounds check. Same family as the two fixed above.
- `modules/audio/src/Mixer.cpp:196` — voice stealing rewrites a voice mid-render and drops the
  sound's refcount underneath the audio thread.
- `modules/render.pcg/src/PcgVolume.cpp:236` — `VolumeBuilder` reads its readback buffer one frame
  after recording the copy with **no fence wait**; the copy may not have executed.
- `modules/render.voxi/src/VoxiRenderer.cpp:637` — the ray-tracing instance table is a single upload
  buffer rewritten every frame while the previous frame may still be reading it.
- `modules/rhi.d3d12/src/D3D12Device.cpp:1953` — `createSkinTargetMesh` leaves `ibBuffer` and
  `vertexCount` unset, so `meshGeometry()` denies a skinned mesh and one skinned entity turns off
  ray-traced reflections.
- `modules/landscape/src/ChunkMesh.cpp:49` — chunk normals read world-Z zero past the section's
  +X/+Y rim instead of clamping, so every section edge has a wrong normal seam.
- `modules/platform/src/win32/Win32DirectoryWatcher.cpp:148` — the watcher thread dies but
  `watching()` keeps returning true and `poll()` reports nothing. Hot reload silently stops working.
- `modules/mcp/src/McpBridge.cpp:528` — every MCP abi command is dispatched **twice**, once by
  `pump()` and once by the apply callback.
- `modules/mcp/src/McpBridge.cpp:482` — `stop()` joins a worker that can block forever in `recv()`
  on a client socket it never closes.
- `modules/formats.roslyn/src/AverDesign.cpp:96` — the Roslyn actor backend drops the `Controller`
  base-name rule the built-in scanner has, so the two disagree about what is an actor.
- `scripting/csharp/Sample.Game/SampleHud.cs:33` — the reference HUD ignores the viewport origin and
  draws relative to the window.

### LOW

- `modules/formats/src/Avr1.cpp:140` — `AvrStringTable::get` computes its range check in 32-bit, so
  one ref value indexes ~4 GB out of bounds.
- `modules/audio.wasapi/src/AudioDeviceWasapi.cpp:62` — on the 5 s start-handshake timeout the main
  thread closes `readyEvent_` while the render thread may still `SetEvent` it.

---

## Themes worth acting on rather than patching case by case

**Parser bounds arithmetic overflows.** Five of the confirmed bugs are the same mistake in five
files: `a + b > limit` where `a` and `b` are unsigned values read from a file. Two are fixed; the
pattern is `a > limit || b > limit - a`. Every binary reader in `modules/formats` should be swept
for it at once rather than one bug report at a time.

**The audio mixer's voice-stealing path is not thread-safe.** Two confirmed use-after-free style
races (`Mixer.cpp:181` and `:196`) both come from the control thread mutating voice state the audio
callback is reading. This needs a design fix — a lock-free handoff or a retire queue — not two spot
patches.

**Nothing here was caught by the gates**, and that is the honest lesson. The gate oracle samples
probe pixels in the editor; it cannot see a game-runtime frame ordering bug, a C# character
controller, an audio race, or a malformed-asset code path. Bit-exact pixel gates are a strong guard
over exactly the surface they cover, and the surface is narrower than it feels.
