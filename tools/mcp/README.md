# `tools/mcp` — an MCP server for driving the engine

```
aver_build           build; returns whether it built plus compiler errors, Ninja progress dropped
aver_run             run Sandbox.exe for N frames with flags; returns the parsed probe, log, screenshot
aver_inspect_image   crop/scale a screenshot so a region is legible
aver_tests           run the headless suites; pass/fail with each one's assertion summary
aver_gates           the render-gate oracle, READ-ONLY
aver_package         stage the engine payload (stage-payload.ps1), optionally verify it
aver_flags           the engine's CLI flags (128 today), read from source so the list cannot go stale
```

Enable it by trusting `.mcp.json` when the editor asks. Pure Python stdlib — nothing to `pip install`
or `npm install`, which also means nothing whose licence has to be vetted against this repo's
permissive-only rule.

## Why

Every visual defect in this tree was found by building, launching with flags, screenshotting, cropping
and looking. Nine in one phase (`docs/STATUS.md` §4u), three more in the session that wrote this. That
loop is entirely mechanical, and being tedious is why it gets skipped — which is how a subsystem ends
up compiling, linking, passing tests and never having been seen.

## It drives the existing CLI and changes nothing in the engine

`Sandbox.exe` already takes 128 flags and the whole gates oracle is built out of them, so there is no
engine-side listener here: no socket, no named pipe, no new thread, and no risk to a working editor.

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
