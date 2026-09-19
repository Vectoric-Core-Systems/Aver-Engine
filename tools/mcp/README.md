# `tools/mcp` — an MCP server for driving the engine

```
aver_build           build; returns whether it built plus compiler errors, Ninja progress dropped
aver_run             run a host for N frames with flags; returns the parsed probe, log, screenshot
aver_inspect_image   crop/scale a screenshot so a region is legible
aver_tests           run the headless suites THROUGH CTest (scripts/test.ps1); failures and fail lines
aver_gates           the render-gate oracle, READ-ONLY
aver_package         stage the engine payload (stage-payload.ps1), optionally verify it
aver_flags           each host's CLI flags, read from source so the list cannot go stale
```

## Two hosts

`aver_run` and `aver_flags` both take, or report, a **host**:

| host | executable | what it is |
|---|---|---|
| `editor` (default) | `build/bin/Sandbox.exe` | the editor — every render-override flag, `--probe`/`--probe-rel`, and what `gates.ps1` drives |
| `runtime` | `build/bin/AverEngineRuntime.exe` | the shipped game host (`Runtime/`) — **no** render-override flags and no probe at all |

They do not parse the same set, and neither one rejects a flag it does not know: `GameApp.cpp`'s
parse loop says "Anything else is deliberately ignored" in as many words. So `--gi --no-rt
--probe-rel 0.5 0.5` aimed at the runtime runs cleanly, exits 0, prints no probe line and means
nothing. `aver_run` reports any flag it cannot find in the chosen host's own source for exactly that
reason — the same silence `binary_provenance` catches one step later, when the flag exists but the
binary predates it.

There is no count of the flags written down anywhere here on purpose. This file used to say 128 and
the module docstring used to say 43; the real number when someone next checked was 183. Ask
`aver_flags`.

## `aver_tests` goes through CTest, and that is the whole point of it

It used to glob `bin/*Test.exe` and run each one directly, reporting pass or fail from the exit code
alone. The root `CMakeLists.txt` registers every `*Test` target with a `FAIL_REGULAR_EXPRESSION`
(`FAIL  ` or `FAILED ===`) precisely because suites in this tree have historically returned 0 however
they went — there is a commit named *"Two skin tests that exited 0 however they went"*. Running the
binaries directly skipped that check, so **a suite that printed `FAIL` and exited 0 was reported
green** by the one test path an agent session could reach. It even collected those `FAIL` lines into
the result and then never let them change the verdict.

The registration walk covers exactly the set the glob covered — 140 registered tests against 140
`bin/*Test.exe`, with no name in either that is missing from the other — so nothing was lost by the
move, and three things were gained: the fail-regex, CTest's `bin/` working directory (several suites
resolve fixtures relative to the executable; this tool had been running them from the repo root), and
one command a human or a build server can run too.

`only` is now a **CTest `-R` name regex and case-sensitive**, where it used to be a case-insensitive
substring. `import` no longer matches `ImportTest`; write `Import`.

Enable it by trusting `.mcp.json` when the editor asks. Pure Python stdlib — nothing to `pip install`
or `npm install`, which also means nothing whose licence has to be vetted against this repo's
permissive-only rule.

## Why

Every visual defect in this tree was found by building, launching with flags, screenshotting, cropping
and looking. Nine in one phase (`docs/STATUS.md` §4u), three more in the session that wrote this. That
loop is entirely mechanical, and being tedious is why it gets skipped — which is how a subsystem ends
up compiling, linking, passing tests and never having been seen.

## It drives the existing CLI and changes nothing in the engine

Both hosts already take the flags and the whole gates oracle is built out of `Sandbox.exe`'s, so
there is no engine-side listener here: no socket, no named pipe, no new thread, and no risk to a
working editor.

That is a deliberate first cut. **It is batch control, not live control** — each call is a fresh process
that runs N frames and exits. You cannot drive a running editor, click a button, or step a frame at a
time. Live control needs an engine-side command channel, which is a real feature with threading and
lifetime concerns, and it should not arrive in the same change that introduces this tooling.

## What it refuses, and why

These are refusals, not gaps:

- **`gates.ps1 -Record` is unreachable.** Re-recording overwrites the only record of what the renderer
  used to do. It is the user's call, made with `./scripts/record-gates.ps1`.
- **No interactive run.** `--frames` is always passed, so every launch terminates. A windowed editor
  left behind holds `bin/*.dll` open and the next build fails `LNK1168` — and the user may have their
  own editor open, which this must not disturb.
- **It never kills a process it did not start.**
- **Gate configuration names are pattern-checked**, because they reach a shell.

## Notes from building it

- `ROOT` is three directories up, not two. Two put it at `tools/` and every `./scripts/...` call would
  have run from the wrong place. It is now asserted against `CMakeLists.txt` at startup, because a wrong
  root fails as "build.ps1 not found", which sends a reader hunting for a missing script.
- A tool that throws returns `{"ok": false, "error": ...}` as a **result**, not a protocol error. A
  build that blew up is an answer about the build; surfacing it as a broken server hides it. That is
  what caught the `ROOT` bug on the first run.
- `stdout` is the protocol channel, so nothing but JSON-RPC may be written to it. Diagnostics go to
  `stderr` and `print()` is never used.
