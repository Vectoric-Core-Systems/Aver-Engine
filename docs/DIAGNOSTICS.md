# Diagnostics: log severity, exit codes, ABI reasons, and the crash reporter

---

## The severity ladder

`aver::LogLevel` — `modules/core/include/aver/core/Log.hpp`.

| Level | Macro | Means | Editor colour |
|---|---|---|---|
| `Trace` | `AVER_TRACE` | Firehose detail, off by default in the filter | dim grey |
| `Info` | `AVER_INFO` | Normal narration | near-white |
| `Warn` | `AVER_WARN` | Something is off but nothing failed | **bright orange** |
| `Error` | `AVER_ERROR` | An operation failed. The process is healthy. | **red** |
| `Critical` | `AVER_CRITICAL` | The failure threatens the PROCESS. Wakes the crash reporter. | **dark red**, on a dark red row |
| `Fatal` | `AVER_FATAL` | Death is imminent. Writes a report and terminates. | **black on red** |

**Where the line is**, to keep the ladder meaningful:

- **Error** — a file did not load, a shader did not compile, an argument was rejected. The next frame will be fine.
- **Critical** — a lost GPU device, an exhausted descriptor heap, an allocation the engine needed and did not get. It is still running and may well survive, but it is now *a candidate to die*.
- **Fatal** — `AVER_FATAL` does not return. There is deliberately no way to log Fatal and carry on, because a "fatal" that execution continues past is how the worst severity quietly becomes a louder warning.

**Black on red for Fatal** is legible only because of the row behind it. This editor's panel background sits at 0.07–0.15 luminance, where black text alone would be invisible — which for the highest severity in the ladder is the worst possible outcome. The filled row is what makes it work.

### Adding a level

Six places must move together. A missed one is a silent bug, not a build error:

1. `LogLevel` and the macros — `modules/core/include/aver/core/Log.hpp`
2. `levelTag()` and the stderr threshold — `modules/core/src/Log.cpp`
3. `AVER_SCRIPT_LOG_*` — `modules/scripting/include/aver/scripting/scripting_abi.h`
4. `Log.Level` and its convenience methods — `scripting/csharp/Aver.Scripting/Log.cs`
5. `managedLog()`'s switch — `modules/scripting/src/ScriptHost.cpp`
6. `logLineStyle()` and the filter combo — `sandbox/src/SandboxApp.cpp`

**Append, never insert.** Every one of those mirrors is a bare integer, and the editor persists the
filter index in `editor.ini`. A value added in the middle renumbers all of them with no diagnostic.

**Three of those six are checked by automated test.** `tests/abi/src/AbiEnumTest.cpp` reads `Log.hpp`,
`scripting_abi.h` and `Log.cs` and compares all three numberings — the C++ enum by the position of
each name. Reorder the ladder in `Log.hpp` alone and the suite goes red. The other three (`managedLog()`'s switch and the editor's filter combo) remain on the honour system.

**And `tools/mcp/aver_mcp.py` scrapes the bracket tags**: `errors` matches `[ERROR`, `[CRIT` and `[FATAL`; `warns` matches `[WARN`. Tag spellings are load-bearing — renaming one silently drops that severity out of every gate summary.

---

## Exit codes — what a process returns

`aver::ExitCode` — `modules/core/include/aver/core/ErrorCodes.hpp`.

| Code | Name | Means |
|---|---|---|
| 0 | `Ok` | it did the thing |
| 1 | `Failed` | it ran, and the thing did not work: tests failed, gates moved, a build broke |
| 2 | `Usage` | the caller is wrong: a missing argument, an unknown flag, a bad path |
| 3 | `Environment` | the *machine* is wrong: no build tree, no compiler, no GPU, a missing SDK |
| 4 | `Interrupted` | stopped by a signal, a timeout, or a user |

**0–15 is reserved**; above that band a tool may define its own.

`Environment` is separate from `Usage` on purpose: it is not the caller's fault and re-reading the
help text will not fix it, and CI wants to report the two differently.

### A count is not an exit code

This rule was broken in three places at once. `gates.ps1`, `verify-game.ps1` and `stage-game.ps1` all exited **the number of failures**, and 55 of the 100 test suites returned `g_failures` from `main`. Two consequences:

- Two failures exited `2`, which this table spells *"you invoked me wrong"* — so nothing downstream could tell a renderer regression from a typo in a `-Config` name.
- A shell truncates an exit code to a byte, so **256 failures exit 0**. The count that says the most is the one that vanishes.

A tool with a count **prints** it and returns `Failed`. Every count in this repo is now printed; only the code changed.

### Who follows it

C++ tools: `AverAssetC` (`convert`, `material`, `stream`, `--material-map`), `ActorSweep`, `MakeRig`,
`MakeFoliage`, `MakeSamples`, `RelodTool`, `DumpClusterPs`, `LevelInspect` and `OcmeshDiff`. Every
`*Test.exe` returns 0 or 1 (77 for a skip, below): many write the literals, and a failure count is never
returned.

Scripts: `scripts/{gates,record-gates,test,stage-game,verify-game,module-matrix,pt-compare,rt-spread,
stage-payload,verify-payload}.ps1`, and the Python drivers `seq-ab.py`, `shots.py`, `tools/neuraa/*` and
`tools/mcp/aver_mcp.py`. Python and PowerShell cannot include the header, so they restate the numbers
0/1/2/3/4 with a comment naming `ExitCode`.

Deliberate exceptions:

- **`AverCrashReporter`** restates the table in a comment rather than including the header. It links
  nothing from the engine (see below), and a shared header is shared code.
- **`build.ps1`** passes the toolchain's own exit code straight through. `test.ps1` does *not* —
  ctest has its own vocabulary (`8` for "some tests failed"), and forwarding it would put a foreign
  number where `8` means nothing here.
- **Scripts with their own small vocabularies.** Three Python scripts keep their own
  small vocabularies: `claim-audit.py` (0 caught, 1 the edit survived, 2 an unusable edit),
  `docs-check.py` (1 when a doc names a missing path) and `module-guard-audit.py` (2 for an unknown
  argument). These are known deviations, not the standard.

### Skipped is 77

A test that cannot run on this machine (no GPU or device, `averdesign` not staged, no user data
directory, no `AVER_REPO_ROOT`) returns **77**, and the root `CMakeLists.txt` registers every `*Test`
target with `SKIP_RETURN_CODE 77`, so ctest shows *Skipped* and never *Passed*. A skip that returned 0
would read as coverage that did not happen. 77 sits above the reserved 0–15 band, so it does not conflict
with the table. Several suites define it as a local `kSkip` constant.

### Traps in scripts

- `throw`, and `Write-Error` under `$ErrorActionPreference = 'Stop'`, exit **1**. A PowerShell script that
  wants to report Usage or Environment prints with `Write-Host` and then calls an explicit `exit 2` / `exit 3`.
- `sys.exit("text")` in Python prints the text and exits **1**. Scripts use a small helper that prints
  and exits with the right code (`die(code, msg)` in `seq-ab.py` and `shots.py`).

---

## ABI result codes — why a call failed

`aver::AbiError` — `modules/core/include/aver/core/ErrorCodes.hpp`.

| Code | Name | Means |
|---|---|---|
| 0 | `Ok` | no error recorded since the last call that records one |
| −1 | `BadHandle` | a handle that is zero, out of range, or names a destroyed object |
| −2 | `NullPointer` | a required out-parameter was null |
| −3 | `NotInitialised` | the module's world/device/host does not exist yet |
| −4 | `OutOfRange` | an index or count past the end of what exists |
| −5 | `Unsupported` | a real request this build cannot serve |
| −6 | `InvalidArgument` | a value that is not a handle and is not legal |
| −7 | `AllocationFailed` | the request was legal and the memory was not there |

**Every code but `Ok` is negative**, so a caller can test `code < 0` for "failed" without knowing the
whole list — including codes added after its binding was built.

### It travels on a separate entry point, and that is the whole design

Every ABI in this engine returns `1` for success and `0` for failure, and every caller — C++ and C#
alike — writes `if (aver_phys_...)`. Returning a negative code from those functions would make
`if (r)` **true on failure**, silently inverting every existing call site without one compile error.

So the reason travels on `aver_<module>_last_error()`. Existing entry points keep their 1/0 contract
exactly; a caller that wants the reason asks for it. That is additive in the sense these headers
already use.

**Thread-local**, so two threads failing at once do not overwrite each other's reason — the physics
module runs a worker pool. **Per module**, which falls out of how the engine is built rather than
being a choice: `Aver.Core` is a static library linked into each ABI DLL, so each DLL has its own
slot. `aver_phys_last_error()` therefore reports physics failures and nothing else.

**Set on success too.** A slot written only on failure reports a stale reason forever after one bad
call, and the first caller to trust it is misled.

### What it separates

| Call | Returned 0 because… | Now says |
|---|---|---|
| `aver_phys_body_position` | there is no world at all | `NotInitialised` |
| `aver_phys_body_position` | the handle was removed | `BadHandle` |
| `aver_scene_set_vec` | no field has that id | `BadHandle` |
| `aver_scene_set_vec` | the entity lacks that component | `BadHandle` |
| `aver_scene_set_vec` | the value pointer was null | `NullPointer` |
| `aver_scene_set_i32` | the field is real, and a `Vec3` | `InvalidArgument` |
| `aver_scene_set_vec` | the field is real, right kind, **read-only** | `Unsupported` |
| `aver_phys_character_set_shape` | the new shape is blocked by geometry | `Unsupported` |
| `aver_phys_body_move_kinematic` | the body is not kinematic | `Unsupported` |
| `aver_decal_spawn` | the pool capacity is 0 | `Unsupported` |
| `aver_syn_crowd_configure` | the entity has no such component | `BadHandle` |
| `aver_prefab_spawn` | no host is installed | `NotInitialised` |
| `aver_sb_step` | `aver_sb_build` has not run | `NotInitialised` |

That last row is `CWorld.matrix`, and it is the case that has actually confused people: a write that
fails for a reason no amount of fixing the handle will address.

### Implemented in

Channels:

- `aver_phys_last_error()` (physics: bodies, characters and vehicles — `physics_vehicle_abi.h` records
  into the same slot) and `aver_scene_last_error()` (scene, ABI minor 5) are the two reference
  implementations. `aver_decal_*` lives in `Aver.Scene.dll`, so decals share the scene channel.
- `aver_syn_last_error()` (`Synapse.Abi`) and `aver_prefab_last_error()` (`Prefab.Abi`, ABI minor 1).
- `aver_sb_last_error()` (soft body): a static library with no DLL decoration, so it reports through
  whichever DLL re-exports it and shares that DLL's single Core slot (`Aver.Physics.dll` per the header).

Mirrored in C# as `Aver.Physics.Physics.LastError` / `PhysicsError`, `Aver.Scene.Scene.LastError` /
`SceneError`, `SynapseError` and `PrefabError`; `AbiEnumTest` compares five numberings (the core
`AbiError` and the four mirrors). The soft body has no managed binding yet.

Audio, ui (`ui_abi` and `ui_widget_abi`), framework (including timers/events and the blackboard),
settings, voxi, scripting and pbr do **not** have a channel yet. They are older modules, and adding a
channel is per-module work. The first candidates are the blackboard ("missing key" vs "wrong type" vs
"no provider") and the timers ("NaN delay" vs "table full"), where one `0` hides several causes. A new
entry point that lands in a module that already has a channel must set `AbiError` (`Ok` on success).

Behaviour is covered by `testLastError()` in `tests/physics/src/PhysicsTest.cpp`,
`testSceneAbiErrors()` in `tests/scene/src/SceneTest.cpp`, the extended `CharacterTest` and
`PhysicsTest` cases, `SynapseAbiTest`, `DecalAbiTest` and `SoftBodyTest`. The prefab channel has no
behavioural test yet (its reasons come from the host it forwards to). The first two were
falsified: collapsing the read-only case into "wrong kind", and reporting a dead handle where there is
no world, each turn the relevant suite red.

---

## The crash reporter

`modules/core/include/aver/core/CrashReport.hpp`, `tools/AverCrashReporter.cpp`.

### Shape

Modelled on Unreal's CrashReportClient, including the folder layout:

```
<exe dir>/Saved/Crashes/AverCrash-<timestamp>-<pid>/
    CrashContext.runtime-xml   type, message, versions, GPU, command line, breadcrumbs, callstack
    AverMinidump.dmp           MiniDumpWriteDump, for a debugger later
    CrashLog.log               the tail of the engine log leading up to the fault
```

### Kinds

`aver::crash::Kind`. **The values are explicit and frozen** for the same reason the log levels are:
the number is written into every report as `CrashTypeCode`, so it outlives the build that produced
it and a kind inserted in the middle silently renumbers every report ever written. Append, never
insert — `AbiEnumTest` pins all of them and round-trips `kindNameOf`.

| Code | Kind | Raised by |
|---|---|---|
| 0 | `Crash` | the structured-exception filter: access violation, divide by zero, stack overflow |
| 1 | `Assert` | `AVER_ASSERT` |
| 2 | `Fatal` | `AVER_FATAL`, pure-virtual call, CRT invalid parameter |
| 3 | `GpuCrash` | *nothing yet* — see below |
| 4 | `Terminate` | `std::terminate`: an exception escaped, or a `noexcept` function threw |
| 5 | `OutOfMemory` | `STATUS_NO_MEMORY` (0xC0000017), and a `std::bad_alloc` that reaches `std::terminate` |

`OutOfMemory` is not a `Crash` even though it usually arrives as one: an allocation failure has a
completely different first question — what was the working set, and what asked for how much — and
triaging it as an access violation sends the reader hunting a dangling pointer that does not exist.
`terminateHandler` rethrows the in-flight exception to recover its type, which is also how a
`Terminate` report gained the escaping exception's `what()`.

**`GpuCrash` has no raiser, and that is correct.** Device loss is a *Critical*, not a crash:
`noteDeviceRemoved` logs one and the process keeps running with the last frame on screen. That
Critical wakes the reporter to **watch** — which is what `GpuCrash` is for, if the process then dies
— rather than to file a report of its own.

**There is no `ShaderCompile` kind.** A failed compile is not a crash here: `createShader` logs
`AVER_ERROR` and returns 0, and the caller degrades. Nothing dies, so there is no report to file a
kind on. It was written and then removed for exactly that reason.

### Why it is a separate process

**The reporter links nothing from the engine — not `Aver.Core`, not one header.** Win32 and the CRT
only. A crash handler that puts a window on screen inside the process that just faulted is asking a
broken process to run a message pump, allocate, and talk to the GPU driver — the three things most
likely to be exactly what broke. It has to still work when the engine does not, and the only way to
be sure is to share no code with it.

It is also declared **unconditionally in the root `CMakeLists.txt`**, not with the other `tools/`
executables. Those all live inside `tests/formats/CMakeLists.txt` behind `if(AVER_BUILD_TESTS)`;
building with tests off would otherwise produce an engine that writes crash folders and then launches
a reporter that was never built — silence at exactly the moment the user most needs to be told
something. It must also be listed in `scripts/payload.allowlist` under `[bin]` or it does not ship.

### What a Critical does

Logging one **wakes the reporter**: `AverCrashReporter.exe --watch <pid>` is launched, detached, and
waits. If the engine then exits cleanly it says nothing and goes away. If the engine dies, it shows
the newest report — and if the engine died *without* writing one (killed, or a fault the handler
could not reach), it says that too, because a silent disappearance is the worst outcome for a user.

The point is that the process spawns its own witness **while it is still healthy enough to spawn
anything**, rather than trying to launch a helper mid-collapse.

Only the first Critical launches it; later ones become breadcrumbs in the report, so a storm costs one
process launch rather than thousands.

### Handlers installed

`SetUnhandledExceptionFilter`, `std::set_terminate`, `_set_purecall_handler`,
`_set_invalid_parameter_handler`, plus `_set_abort_behavior` and `SetErrorMode` to suppress the CRT
and Windows Error Reporting dialogs that would otherwise race ours.

Installed as the **first statement of `main()`** (`EntryPoint.hpp`). Anything that faults before that
gets the operating system's own dialog and no report — and startup, where a missing DLL or a driver
that will not initialise lives, is where a first-run crash is most likely and least self-explanatory.

### Testing it

A crash reporter that has never been seen to fire is not a feature, it is a hope.

```bash
Sandbox.exe --crash-test av        # null write -> the structured-exception filter
```

Kinds: `av` (access violation), `assert` (`AVER_ASSERT`), `fatal` (`AVER_FATAL`), `oom` (an
allocation the process cannot satisfy), `throw` (an escaping C++ exception), and `critical` (logs one
Critical and exits cleanly — the path where the reporter must stay **silent**). Every kind but
`critical` disables the reporter launch, so an automated run leaves no window behind.

Read a report back without a GUI:

```bash
AverCrashReporter.exe --report <folder> --print
```

**Verified 2026-09-05**, all six paths, by reading `CrashTypeCode` back out of the folder each one
wrote:

| `--crash-test` | Kind | Code |
|---|---|---|
| `av` | `Crash` | 0 |
| `assert` | `Assert` | 1 |
| `fatal` | `Fatal` | 2 |
| `throw` | `Terminate` | 4 |
| `oom` | `OutOfMemory` | 5 |
| `critical` | *(no folder, exit 0)* | — |

The `av` case symbolicates to the exact source line of the deliberate null write; `assert` carries its
expression, file and line; `critical` leaves no folder, no window and no lingering process.

### What `oom` found, and why the handler moved

`OutOfMemory` was first wired into `terminateHandler`, on the reasoning that an uncaught
`std::bad_alloc` reaches `std::terminate`. The first `--crash-test oom` threw one and the report came
back as **`Crash`, code 0**.

On Windows an escaping C++ exception raises SEH code `0xE06D7363`, and
`SetUnhandledExceptionFilter` takes it *before* `std::terminate` ever runs. Two fixes fell out, and
neither would have been found by reading the code:

- `OutOfMemory` moved to `std::set_new_handler`, which `operator new` calls **before** constructing a
  `bad_alloc` — so it does not depend on the throw reaching anybody.
- `0xE06D7363` is now recognised in the filter and reported as `Terminate` rather than `Crash`. That
  kind had been effectively unreachable for the case it is named after, and nothing said so.

`terminateHandler` keeps its exception-type recovery for the case it really owns — `terminate` called
directly, such as a `noexcept` function that threw — and now carries the escaping exception's
`what()` into the report.

One real bug the test found immediately: `AVER_DEBUGBREAK()` in `assertFail` raised
`EXCEPTION_BREAKPOINT` with no debugger attached, which our own filter caught and misfiled as a
generic `Crash` — throwing away the assert's message, file and line. It is now guarded on
`crash::debuggerAttached()`.

---

## Stutter hunting (`AVER_HITCH_MS`)

The editor's log is also written to `%LOCALAPPDATA%\AverEngine\Logs\Sandbox.log` (the previous session
kept as `Sandbox-prev.log`). Every frame longer than `AVER_HITCH_MS` (250 ms when unset, 0 turns it off)
logs `[FrameHitch]` with its phases (update, beginFrame, render, endFrame, present). Scoped
`aver::HitchMarks` (`modules/core/include/aver/core/HitchMarks.hpp`) break a phase down further and log
`[HitchMarks] <where> <total>: <stretch> <ms> | ...` when their own scope runs long: the editor's update,
D3D12 `endFrame` (late scene pass vs the rest), Voxi's acceleration-structure build and geometry table,
the level-stream tick (streamer / evict / prefetch / pinned / loads, then mesh releases), a foliage
cell change (acquire / rebuild / release, and `setFoliage`), and each streamed root's load (meshes /
instantiate / hook), eviction and mesh upload, named by asset. With the variable set, a mesh read on
the main thread logs `[Mesh] ... read on the main thread (reason, tag)`, a streamed mesh made without a
staged buffer `[Mesh] ... not staged`, a BLAS over 20 ms to make `[Voxi] a bottom-level structure took`,
and a D3D12 background release over 20 ms `[RHI.D3D12] releasing one ... (background)`. Unset, each costs
one cached `getenv`. `--cam-fly DX DY` (cm per frame) flies the editor camera in a straight line, which
is what exercises level streaming; a third value turns the camera that many degrees a frame.

The D3D12 backend adds, under the same variable: `[GpuHitch]` (a GPU frame over the threshold, its spans
of 2 ms or more), `[PresentStall]` (a `Present` or a present-allocator wait over 50 ms) and
`[PresentPacing]` (only with `AVER_HITCH_MS` set; every 240 presents: interval percentiles and the first 32 intervals -- the cadence
images reach the display at). A GPU span much longer than its marked children is usually a queue wait,
not work.

## Diagnostic codes (`AVR####`) — reserved, not yet issued

The ranges below are allocated so a code names its owner on sight. The retrofit itself is not done —
`Log.cpp`'s only call to `noteCritical` still passes the literal `0` for every Critical, confirming no
code has actually been issued anywhere yet: `noteCritical` already takes a `u32 code`, and the
intended approach is to attach codes at the few choke points that cover most ground — `hrOk` in
`D3D12Device.cpp`, `fail()` in `Avr1.cpp`, `assertFail` — rather than editing every `AVER_ERROR`/
`AVER_WARN` call site. An uncoded line is still coloured by severity; codes are additive.

| Range | Owner |
|---|---|
| `AVR0001–0999` | core, runtime, asserts: `0001–0099` core and asserts; `0100–0299` runtime, game content and level loading; `0300–0399` packaging and payload; `0400–0999` reserved |
| `AVR1000–1499` | RHI D3D12: `1000–1099` device and `hrOk`; `1100–1199` shader and pipeline creation, including async pipeline batches; `1200–1299` device loss and DRED; `1300–1499` reserved |
| `AVR1500–1999` | RHI Vulkan: `1500–1599` device; `1600–1699` descriptors and bindless; `1700–1799` `DeviceCaps` gating of neural features; `1800–1999` reserved |
| `AVR2000–2999` | formats, AVR1, importers: `2000–2199` AVR1 container; `2200–2399` glTF, OBJ and texture importers; `2400–2599` USD import (stage, instancers, variants, purpose and visibility) and `AverAssetC --material-map` and mesh merge; `2600–2799` native asset formats (`.ocsequence`, `.ocstream`, `.ocworld`, `.ocui`, decal and light formats) and `AverAssetC stream`; `2800–2999` reserved |
| `AVR3000–3999` | rendering: `3000–3199` voxi (GI, RT, lights, atmosphere, async scene and pipeline sets); `3200–3299` pbr, materials and material graphs; `3300–3399` path tracing (render.pt, ReSTIR PT, reference PT); `3400–3499` render.neural core (conv and MLP modules, weights loading); `3500–3599` NRD2 and render.denoise (FFX temporal, despeckle, converge); `3600–3699` NeuRAA and AverSR (render.sr); `3700–3799` NeuraFI frame interpolation and NeuRaC; `3800–3849` skin, deform and softbody render; `3850–3899` particles; `3900–3949` fluids; `3950–3999` decal rendering |
| `AVR4000–4999` | scene, framework, scripting: `4000–4199` scene, world and decal ABI; `4200–4299` framework (timers and events, blackboard, prefabs); `4300–4399` synapse AI (behaviour trees, steering, ORCA crowds, hearing, cover, squads); `4400–4599` scripting; `4600–4699` sequencer and level sequences; `4700–4799` game UI; `4800–4899` level streaming and the world module (PlacementStreamer, LevelInstance, chunks); `4900–4999` reserved |
| `AVR5000–5999` | editor / sandbox: `5000–5399` editor panels and viewport; `5400–5599` project browser, projects and migration; `5600–5799` Animate mode, sequence editor and camera paths; `5800–5899` packaging and stage tools; `5900–5999` reserved |
| `AVR6000–6999` | audio, physics, anim: `6000–6299` audio (WASAPI, sound design, streamed audio, fades, reverb zones); `6300–6599` physics (Jolt, vehicles, soft body, fracture); `6600–6999` anim (blend spaces, state machines, IK, control rigs) |

The top-level ranges are fixed. The sub-ranges are allocations only: codes are appended within a
sub-range, never renumbered. Async shader-batch failures, device loss and DRED reports are the first
candidates for codes, since `hrOk` and the DRED path are the existing choke points.
