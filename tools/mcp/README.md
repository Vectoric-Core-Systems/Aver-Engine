# `tools/mcp` — an MCP server for driving the engine

```
aver_build           build; returns whether it built plus compiler errors, Ninja progress dropped
aver_run             run a host for N frames with flags; returns the parsed probe, log, screenshot
aver_inspect_image   crop/scale a screenshot so a region is legible
aver_tests           run the headless suites THROUGH CTest (scripts/test.ps1); failures and fail lines
aver_gates           the render-gate oracle, READ-ONLY
aver_package         stage the engine payload (stage-payload.ps1), optionally verify it
aver_flags           each host's CLI flags, read from source so the list cannot go stale
aver_editor          one raw command to a RUNNING editor's control channel (abi calls, ping, modules, ...)
aver_level           assemble a level in a RUNNING editor: open, place, move, animate, remove, save
```

The first seven start a process, run it and exit. The last two are the odd ones out: they talk to an
editor somebody already has open. See [Driving a running editor](#driving-a-running-editor).

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

## The batch tools drive the existing CLI and change nothing in the engine

Both hosts already take the flags and the whole gates oracle is built out of `Sandbox.exe`'s, so
`aver_build`, `aver_run`, `aver_tests`, `aver_gates`, `aver_package` and `aver_flags` need no
engine-side listener: no socket, no named pipe, no new thread, and no risk to a working editor.

That was the deliberate first cut, and those six are still **batch control** — each call is a fresh
process that runs N frames and exits. Live control needed an engine-side command channel, a real
feature with threading and lifetime concerns, which arrived separately as
[`modules/mcp`](../../modules/mcp/README.md) (`McpBridge`). `aver_editor` and `aver_level` are its
client.

## Driving a running editor

`aver_editor` and `aver_level` connect to `127.0.0.1` and talk to an editor that is **already running
with its control channel on**: start it with `Sandbox.exe --mcp` (or `--mcp <port>`), or click the MCP
button at the right of the status bar and choose Start. They never launch an editor and never turn the
channel on; with nothing listening they say how to. The channel is loopback-only and drives the real
window, so it is the user's to switch on.

**The port** is the one `--mcp` opened: an explicit `port` argument, else `editor_bridge.port` in
`mcp.conf` (the same file and key the editor reads for a bare `--mcp`; see `mcp.conf.example`), else
45123.

**One connection per call.** The bridge serves one client at a time and takes the next only after the
current one disconnects, so each call connects, sends one line, reads one line and closes. Two calls at
once queue behind each other rather than failing. A call that gets no answer says so after `timeout`
seconds (default 90; the bridge itself gives up on the editor's main thread after 60).

### `aver_editor`

Sends one raw command and returns the reply: `{module, fn, args, text}` for an `abi` call (the default),
or `cmd` = `ping`, `modules`, `widgets`, `move`, `click`, `key`, `text`, `shot` with their own fields in
`extra`. Ask `cmd: "modules"` for the ABIs this build registered (`editor`, `physics`, `world`, `graph`,
`level`). `text` may hold newlines, quotes and non-ASCII: the bridge undoes every JSON escape.

### `aver_level`

Assembles a level out of the editor's own code: the same spawn, transform, delete, visibility and save
functions the UI calls, so an edit dirties the level and (where the UI's is) undoes with Ctrl+Z.
`op` picks the operation; parameters an op does not take are refused, not ignored. Full schema and
replies are in the tool's own description (`tools/list`); the ABI behind it is in
[`modules/mcp`'s README](../../modules/mcp/README.md#the-level-abi).

| op | parameters | what it does |
|---|---|---|
| `open` | `path`, `discard?` | opens a level and waits for the load; refused over unsaved edits unless `discard` (a real boolean, or 0/1; any other value is refused, never read as yes) |
| `info` | | level, entity count, dirty, playing, loading, player start, camera, selection |
| `list` | `filter?`, `max?` | entities: id, asset, label, world pos/rot/scale, material, collide, visible, anim |
| `place` | `placements` | `.ocworld` PLACE/PLACEG/CHILD lines, read by the real level parser; returns the new ids, plus `requested` (record lines sent), `parsed`, and any line it skipped in `ignoredLines` with a `warning` |
| `set_transform` | `id`, `position`, `rotation`, `scale?` | the gizmo's undoable move, world space |
| `set_material` | `id`, `material` | the Details panel's material picker (not undoable there either) |
| `set_collide` / `set_visible` | `id`, `collide` / `visible` | the Details panel's two checkboxes |
| `set_anim` | `id`, `clip`, `speed?`, `time?`, `once?` | the object animation a placed mesh plays in Play |
| `remove` | `ids` | Delete, one undo entry per entity |
| `select` | `ids`, `frame?` | selects and frames the camera on them |
| `save` | `path?` | Save, or Save As with a name or path |
| `player_start` | `position`, `yaw?` | moves or creates the level's Player Start |

Positions are centimetres, Z up; rotations are degrees `[yaw, pitch, roll]`; transforms are world space.

**When it refuses**, it says why and changes nothing: while the editor is playing (`list` and `info`
still answer), with no level open, while an `open` is still pending, on an id that is not in the open
level (streamed scatter, class instances and the drone are not written by a save), and on an asset
that is not loaded. Every mesh a `place` names must exist under the project's Content directory; one
imported after the project's meshes loaded is picked up, one that does not exist is refused by name.

Also refused, with the reason and nothing changed:

- `place` text holding a NaN, infinite or out-of-range number (position, rotation, scale, `animspeed`,
  `animtime`); `nan` and `inf` parse as numbers, so they would otherwise reach physics and the saved file.
- an `anim` clip (in `place` or `set_anim`) the runtime could not resolve: it is checked through the content
  index and the animation system, not just for a file on disk, and must be an object clip. A clip added
  after the project opened is indexed on the spot if its path is spelled exactly as the file is named
  (case included); one **re-imported over an existing clip** is served from a cache until the project is
  reopened.
- any animation on a legacy `.ocmap` level, whose save has no record for one.
- `set_transform` while a person is mid-drag in the editor (gizmo, a Details field, a held nudge), so it
  cannot overwrite their edit's undo entry, and on a `snap` placement, whose saved position is its authored
  ground offset (remove and `place` it again with the offset you want).
- the Player Start marker for `set_material`, `set_collide`, `set_visible` and `set_anim`; it takes only
  `set_transform`, `select`, `remove` and `player_start`.

**Keep the connection open until the reply arrives.** If a client hangs up while a call is pending, the
editor drops the connection at once (so the next client is not locked out for the rest of the 60 s) and the
call still runs with its answer discarded. Closing only the write side of the socket counts as hanging up.
This tool never does: it sends one line, waits for the reply, then closes.

**Known limits, so nobody discovers them by surprise:**

- `place` refuses a `class` placement. A save pairs a class instance with the level file's own class
  record, which a placement made over the channel would not have, so it would be lost on save. Put that
  line in the `.ocworld` and `open` it.
- `place` makes one undo entry per entity and the editor keeps the last 128, so a bigger batch can only
  be undone in part. `remove` has the same shape (the UI's own Delete does).
- There is no `new level` op: `open` a file that exists (write a minimal `.ocworld` first).
- The Player Start's heading is not part of its undo entry (the editor keeps it apart from the entity's
  transform); its position is.

## What it refuses, and why

These are refusals, not gaps:

- **`gates.ps1 -Record` is unreachable.** Re-recording overwrites the only record of what the renderer
  used to do. It is the user's call, made with `./scripts/record-gates.ps1`.
- **No interactive run.** `--frames` is always passed, so every launch terminates. A windowed editor
  left behind holds `bin/*.dll` open and the next build fails `LNK1168` — and the user may have their
  own editor open, which this must not disturb.
- **It never kills a process it did not start.**
- **Gate configuration names are pattern-checked**, because they reach a shell.
- **`aver_editor` and `aver_level` never start an editor or its control channel**, and `aver_level`
  edits only the level that editor has open, and not while it is playing.

## Notes from building it

- `ROOT` is three directories up, not two. Two put it at `tools/` and every `./scripts/...` call would
  have run from the wrong place. It is now asserted against `CMakeLists.txt` at startup, because a wrong
  root fails as "build.ps1 not found", which sends a reader hunting for a missing script.
- A tool that throws returns `{"ok": false, "error": ...}` as a **result**, not a protocol error. A
  build that blew up is an answer about the build; surfacing it as a broken server hides it. That is
  what caught the `ROOT` bug on the first run.
- `stdout` is the protocol channel, so nothing but JSON-RPC may be written to it. Diagnostics go to
  `stderr` and `print()` is never used.
