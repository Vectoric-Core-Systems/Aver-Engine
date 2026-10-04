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

`AverAssetC`, `ActorSweep`, `MakeRig`, `MakeFoliage`, `MakeSamples`, `RelodTool`, `DumpClusterPs`,
`LevelInspect`, every `*Test.exe`, and `scripts/{gates,record-gates,test,stage-game,verify-game}.ps1`.

Two deliberate exceptions:

- **`AverCrashReporter`** restates the table in a comment rather than including the header. It links
  nothing from the engine (see below), and a shared header is shared code.
- **`build.ps1`** passes the toolchain's own exit code straight through. `test.ps1` does *not* —
  ctest has its own vocabulary (`8` for "some tests failed"), and forwarding it would put a foreign
  number where `8` means nothing here.

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

That last row is `CWorld.matrix`, and it is the case that has actually confused people: a write that
fails for a reason no amount of fixing the handle will address.

### Implemented in

`aver_phys_last_error()` (physics) and `aver_scene_last_error()` (scene, ABI minor 5). Mirrored in C#
as `Aver.Physics.Physics.LastError` / `PhysicsError` and `Aver.Scene.Scene.LastError` / `SceneError`,
and the three numberings are compared by `AbiEnumTest`. The other ABIs — audio, ui, framework,
settings, voxi, pbr — do **not** have a channel yet; adding one is per-module work, and the two above
are the worked reference.

Behaviour is covered by `testLastError()` in `tests/physics/src/PhysicsTest.cpp` and
`testSceneAbiErrors()` in `tests/scene/src/SceneTest.cpp`. Both were falsified: collapsing the
read-only case into "wrong kind", and reporting a dead handle where there is no world, each turn the
relevant suite red.

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

## Diagnostic codes (`AVR####`) — reserved, not yet issued

The ranges below are allocated so a code names its owner on sight. The retrofit itself is not done —
`Log.cpp`'s only call to `noteCritical` still passes the literal `0` for every Critical, confirming no
code has actually been issued anywhere yet: `noteCritical` already takes a `u32 code`, and the
intended approach is to attach codes at the few choke points that cover most ground — `hrOk` in
`D3D12Device.cpp`, `fail()` in `Avr1.cpp`, `assertFail` — rather than editing every `AVER_ERROR`/
`AVER_WARN` call site. An uncoded line is still coloured by severity; codes are additive.

| Range | Owner |
|---|---|
| `AVR0001–0999` | core, runtime, asserts |
| `AVR1000–1499` | RHI D3D12 |
| `AVR1500–1999` | RHI Vulkan |
| `AVR2000–2999` | formats, AVR1 container, importers |
| `AVR3000–3999` | rendering (voxi, pbr, pt, skin, particles, fluids) |
| `AVR4000–4999` | scene, framework, scripting |
| `AVR5000–5999` | editor / sandbox |
| `AVR6000–6999` | audio, physics, anim |
