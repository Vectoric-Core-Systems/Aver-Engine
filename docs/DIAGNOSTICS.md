# Diagnostics: log severity, colour, and the crash reporter

There was no document about logging in this engine until now. `docs/ARCHITECTURE.md` mentions
"logging" in a module one-liner and that was the entire written surface, which is how the severity
ladder drifted into meaning whatever each call site felt like at the time.

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

**Where the line is between the top three**, because otherwise "how bad is this" becomes a matter of
taste and the ladder stops meaning anything within a week:

- **Error** — a file did not load, a shader did not compile, an argument was rejected. The next frame
  will be fine.
- **Critical** — a lost GPU device, an exhausted descriptor heap, an allocation the engine needed and
  did not get. It is still running and may well survive, but it is now *a candidate to die*.
- **Fatal** — `AVER_FATAL` does not return. There is deliberately no way to log Fatal and carry on,
  because a "fatal" that execution continues past is how the worst severity quietly becomes a louder
  warning.

**Black on red for Fatal** is legible only because of the row behind it. This editor's panel
background sits at 0.07–0.15 luminance, where black text alone would be invisible — which for the
highest severity in the ladder is the worst possible outcome. The filled row is what makes it work.

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

**And `tools/mcp/aver_mcp.py`.** It scrapes stdout for the bracketed tag: `errors` matches `[ERROR`,
`[CRIT` and `[FATAL`, `warns` matches `[WARN`. Tag spellings are load-bearing — renaming one silently
drops that severity out of every gate summary. (`[FATAL` sat there matching nothing for months before
the level existed.)

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

Kinds: `av` (access violation), `assert` (`AVER_ASSERT`), `fatal` (`AVER_FATAL`), and `critical`
(logs one Critical and exits cleanly — the path where the reporter must stay **silent**). Every kind
but `critical` disables the reporter launch, so an automated run leaves no window behind.

Read a report back without a GUI:

```bash
AverCrashReporter.exe --report <folder> --print
```

**Verified 2026-08-29**, all four paths. The `av` case symbolicates to the exact source line of the
deliberate null write; `assert` carries its expression, file and line; `critical` leaves no folder, no
window and no lingering process.

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
`AVER_WARN` call site (1,313 of them as of this pass — this line said 1,197 before a recount; re-run
`grep -roP "AVER_ERROR\(|AVER_WARN\(" -r modules sandbox tools tests | wc -l` rather than trust either
number, since it moves with every commit). An uncoded line is still coloured by severity; codes are
additive.

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
