# Aver.Mcp  (`modules/mcp`)

- **Language:** C++
- **Depends on:** Core. Nothing else.
- **Switch:** `AVER_MODULE_MCP`, **default OFF**
- **Platform:** Windows only, guarded the way `modules/audio.wasapi` is

A control channel into a **running** editor, so a tool can press its buttons and then look at what
happened.

## The editor works without it. That is the requirement, not a nicety

With `AVER_MODULE_MCP=OFF` — which is the default — the target is not built, the header is not included,
no thread starts, no socket is opened, no port is bound, and the editor has no idea it ever existed.
**Verified both ways**: the default build produces no `Aver.Mcp` and no `McpTest`, and `Sandbox.exe`
runs and exits 0; the `-DAVER_MODULE_MCP=ON` build also runs and exits 0 with all 20 suites passing.

Nobody should ship a game editor whose UI depends on a listening socket. And even when it *is* built it
stays inert: `start()` is called only when the app is asked to, because a build that silently listens on
a port has opened a hole in somebody's machine without telling them. Loopback is hard-coded, not
configurable — making the address an option would be offering remote control as a feature.

## How it forces a click

It **posts real Win32 messages** to the window: `WM_MOUSEMOVE`, `WM_LBUTTONDOWN`/`UP`, `WM_KEYDOWN`/`UP`.

The editor's input already arrives that way — `ImGui_ImplWin32_WndProcHandler`, see
`modules/rhi.d3d12.imgui/src/ImGuiUiBackend.cpp` — so a synthetic click travels the *identical* path
as a human one, through the same handler, in the same order, with no second code path to keep in step.
The alternative was calling ImGui's `io.Add*Event` directly, which would fight the Win32 backend's own
`NewFrame` and would exercise a path no user ever takes.

It also means this module knows nothing about ImGui, the RHI, or the editor. It holds a socket and a
queue. That is exactly why it can be optional.

## Pacing is per event, and that is load-bearing

`pump()` delivers **one event per frame**, called from the thread that owns the window.

A click is a move, a press and a release. ImGui registers a click only when one frame saw the press and
a *later* frame saw the release — so delivering all three between two `NewFrame` calls means nothing is
ever clicked. The first version popped a whole command per frame and would have done precisely that:
three events, one frame, no click, and a very confusing screenshot. A click now takes three frames,
which at 60 Hz is 50 ms.

## Protocol

One JSON object per line, over `127.0.0.1:45123`:

```
{"id":1,"cmd":"move","x":100,"y":200}
{"id":2,"cmd":"click","x":100,"y":200,"button":"left"}
{"id":3,"cmd":"key","key":"F"}          keys by NAME: "F" and "f1" are different keys
{"id":4,"cmd":"text","text":"hello"}
{"id":5,"cmd":"ping"}
{"id":6,"cmd":"shot","path":"C:/tmp/a.png"}
```

Unknown commands are **refused with a reason**, never ignored — a client that misspelled `click` should
be told, not left waiting for a button that was never pressed. Malformed lines are answered immediately
from the socket thread rather than queued, since there is nothing for the main thread to do with them.

**Strings are real JSON strings.** A `text` (or `path`, `key`, `module`, `fn`) may hold `\n`, `\r`, `\t`,
`\"`, `\\`, `\/` and `\uXXXX` (surrogate pairs included), and comes out the other side as the characters
they name. JSON escapes were fully supported from 2026-09-29 onwards. A key is found at its **first
occurrence** in the line, so a client puts free-form content (`text`) last, after the keys the reader
looks up by name; a `"` inside a string is always escaped, so it cannot spell a key.

**Replies** are one line each: `{"id":N,"ok":true,"result":"..."}` or `{"id":N,"ok":false,"error":"..."}`.
`result` and `error` are JSON-escaped strings, so an ABI that answers with a JSON document (the `level`
ABI does) is a string inside the reply, and a client parses twice. An `abi` request is waited on until
the main thread has run it, or 60 s: a request that times out **still runs** later, so it is worth
asking again whether it took effect rather than sending it twice.

**The wait is taken in 100 ms slices**, so it can end early for the two reasons that are not an answer.
If the client **hangs up** while a call is pending, the connection is dropped at once and the next client
is accepted: the bridge serves one connection, and it used to stay held for the rest of the 60 s. The call
is already queued, so it **still runs** and its answer has nowhere to go. A client must therefore keep its
end open until the reply arrives; closing only its write side (a half-close, as a one-shot `nc` does) reads
as hanging up. And when `stop()` runs, the wait ends within a slice instead of holding `join()` for the
timeout, and any call still queued is failed with "the editor is shutting down" and discarded rather than
left to run after a restart.

`tests/mcp` covers the parser with no socket at all: `parseCommand` is exposed precisely so the part
where bugs live can be checked without binding a port. 67 assertions (was 44; the ABI-registry section
grew), including all ten refusal cases and the fact that a constructed-but-unstarted bridge is inert.

## Calls route to the module's own ABI

A client sees one surface and calls it **the Aver ABI**. There is no such single thing. It is the union
of the plain-C seams where each module meets the engine core, and a call is routed to the seam that owns
it:

```
{"cmd":"abi","module":"physics","fn":"bodyCount"}                -> physics_abi.h    (aver_phys_body_count)
{"cmd":"abi","module":"world","fn":"stream_on"}                  -> chunk streaming, this app's own state
{"cmd":"abi","module":"graph","fn":"attach","args":[7]}          -> ScriptHost::graphLoad, via the entity id
```

The modules registered by `SandboxApp::registerMcpAbis()` are `editor`, `physics`, `world`, `graph`
and `level`. `fn` is the entry point **without** its module prefix — `spawn`, not `aver_fw_spawn` —
because the prefix is already implied by `module`, and making a client repeat it is inviting the two
to disagree.

### It is a registry, not a switch, and that is what keeps this module Core-only

`Aver.Mcp` links `Aver.Core` and nothing else. It cannot call `Aver.Framework`, and it must not: a
control channel that dragged in half the engine could not be the optional module it is required to be.

So the app registers dispatchers — `registerAbi("framework", ...)` — exactly the way `ActorEditorHooks`
keeps an asset editor from reaching into the application. A `switch` on module names would mean this
file knew every module by name, and knowing them is one step from linking them.

**A pleasant consequence: the registry IS the build configuration.** A module switched off registers
nothing, so `modules()` reports what this binary can actually reach, and a call to a missing one is
refused with *"no ABI registered for 'voxi' -- either the name is wrong or that module was not built
into this binary"*. The two causes a caller could plausibly have are both named, because "no ABI for
voxi" alone sends a reader hunting for a typo.

A module's own refusal survives the trip: the ABI decides what its entry points are, not the bridge. An
unknown `fn` comes back in the module's words.

Dispatchers are called **outside** the queue lock — a dispatcher runs module code of unknown duration,
and holding the mutex across it would stall the socket thread for as long as the engine took to answer.

## Wiring

`SandboxApp` takes a `--mcp [port]` flag, calls `mcp_.start(port)`, and pumps one event per frame from
`onUpdate` whenever `mcp_.listening()` — the click-forcing path this module exists for actually runs today.
`registerMcpAbis()` registers `editor`, `physics`, `world`, `graph` and `level`; `framework` and `scene`
are not among them, so a client should not assume every module in the engine has an ABI seam exposed here.

`{"cmd":"modules"}` exists: `McpBridge::modules()` is reachable over the wire, so a client can ask what
this build's registry currently holds instead of trusting a list in this file.

## The `level` ABI

Registered by `SandboxApp::registerMcpAbis()`, implemented in `sandbox/src/SandboxMcp.cpp`
(`mcpLevelAbi` and its helpers). It exists so a level can be **assembled** over the channel — placed,
moved, animated, removed, saved — without hand-editing a `.ocworld`. `tools/mcp/aver_mcp.py`'s
`aver_level` tool is the friendly client; this section is what it speaks.

**Every op goes through the editor's own code**, so what a script does is what a click does: it is
undoable where the UI's is, marks the level dirty, and saves through `saveLevel`. Nothing here keeps a
second copy of "how a level entity is made".

Ids are the numeric `scene::Entity`. Positions are centimetres, Z up; rotations are degrees, `[yaw,
pitch, roll]`; transforms are **world space** (the gizmo's and the Details panel's). Every reply is a
JSON document in the reply's `result` string.

```
{"cmd":"abi","module":"level","fn":"place","text":"PLACE Meshes/cube.ocmesh 0 0 0  0 0 0  100\nPLACE ..."}
{"cmd":"abi","module":"level","fn":"set_transform","args":[42, 0,0,50, 90,0,0]}
```

| fn | `args` | `text` | reply | editor path it reuses |
|---|---|---|---|---|
| `open` | `[discard?]` | level path | `{queued, path}` | `requestOpenLevel` → the pending-open drain (`applyPendingOpen`) |
| `info` | | | `{level, name, entities, dirty, format, playing, loading, pendingOpen, pendingOpenPrompt, content, playerStart, camera, selection}` | |
| `list` | `[max?]` (500) | substring of asset or label, case-insensitive | `{total, returned, entities:[{id, asset, label, pos, rot, scale, material, collide, visible, parent, anim}]}` | `levelEntities_`, `worldTransformOf`, `authoredVisible`, `entityAnim` |
| `place` | | `.ocworld` PLACE/PLACEG/CHILD lines, `\n`-separated | `{ids, placed, requested, parsed, ignored, ignoredLines, animated, undoEntries}` | `parseOcworld`, `world::instantiate`, then `onLevelInstantiated`'s bookkeeping; `describeEntity` + `pushEdit(Create)` |
| `set_transform` | `[id, x,y,z, yaw,pitch,roll (, sx,sy,sz)]` | | `{id, pos, rot, scale}` | `beginTransformEdit` / `setSelectedXform` / `endTransformEdit` (the gizmo) |
| `set_material` | `[id]` | material (an `.ocmat` stem) | `{id, material}` | `assignMaterialToken` (the Details picker) |
| `set_collide` | `[id, 0\|1]` | | `{id, collide, changed}` | `setEntityCollide` + `Kind::Collision` |
| `set_visible` | `[id, 0\|1]` | | `{id, visible, changed}` | `setAuthoredVisible` + `Kind::Visibility` |
| `set_anim` | `[id (, speed (, time (, once)))]` | clip (`""` clears) | `{id, anim, changed}` | `applyEntityAnim` + `Kind::Animation` |
| `remove` | `[id, ...]` | | `{removed, requested}` | the selection, then `deleteSelection` |
| `select` | `[id, ...]` (none clears) | `noframe` to leave the camera | `{selected, framed}` | `multiSetSingle`/`multiToggle`, then F's framing |
| `save` | | none: Save. A bare name or a path: Save As | `{path, saved, savedAs, name?}` | `saveLevel`; Save As repeats `drawSaveLevelAsPrompt`'s steps |
| `player_start` | `[x, y, z, yaw]` | | `{id, pos, yaw, created}` | `makePlayerStart` + `Kind::Create`, or the transform edit above |

**`place`** wraps the lines in the smallest document the level parser accepts (`OCWORLD 1`), so every
token it reads means what it means in a file: scale (`PLACE` uniform, `PLACEG` per axis), a material
name, `nocollide`, `hidden`, `snap`, `name <percent-encoded>`, and the `anim <clip.ocanim> animspeed
<f> animtime <f> animonce` tokens; `BEGIN`/`END` with `CHILD` lines parents. `ids` has one entry per
placement in order, `0` where the world refused it. The placement's asset must already be a loaded
mesh: one whose file is under the project's Content directory but was imported after the project's
meshes loaded is picked up (the reload a Content Browser drop does); anything else is refused by name
before any entity is made. An `anim` clip is checked **the way the runtime will resolve it**: through the
content index (id = `fnv1a64` of the clip's spelling) and `AnimSystem::clip()`, which must hand back an
**object** clip (`kOcAnimObject`); anything else is refused. A clip written after the project opened is
not in the index, since nothing rescans short of reopening the project, so that one file is indexed on
the spot, and only when the clip is spelled exactly as the file is named under Content, case included,
because the runtime finds a clip by that exact path. A clip *re-imported over an existing one* is served
from `AnimSystem`'s cache until the project is reopened: it has no per-clip invalidation.

**`place` counts three things**, because they are not the same: `requested` is the record lines the text
held (BEGIN/END, blanks and `#` comments do not count), `parsed` the placements the level parser read from
them, `placed` the entities made. The parser skips a record it does not know without a word, so a
misspelt `PLCAE` (or a pasted `SUN` line) comes back in `ignoredLines` (`[{line, text}]`, the first 8,
line numbers counting the text the ABI received) with the total in `ignored`, and is logged as a warning.
The lines that were placements still land: the reply reports the ignored ones, it does not refuse them.
`aver_level` adds a `warning` string.

**`open` only queues.** The editor drains a pending open on its own frame, past its unsaved-changes
check, so the reply is `{queued: true}` and a client waits by asking `info` until `pendingOpen` is empty,
`loading` is false and `level` is the file it asked for. `open` over unsaved edits is refused unless
`args[0]` is non-zero, because the prompt the editor would show cannot be answered from here.

**Refusals**, each with its reason in `error` and no change made: an unknown `fn`; a non-finite
argument; while the editor is playing (`info` and `list` still answer); with no level open; while an
`open` is pending or the level is still loading; an id that is not a live entity of the open level
(streamed scatter, class instances and the drone are not written by a save, so editing one would
silently vanish); `place` naming an unloaded asset, an invalid clip, or a `class` (a save pairs a class
instance with the level file's own class record, which a placement made here would lack).

Also refused, each with its reason:

- **A NaN, infinite or out-of-range number inside `place` text.** The parser reads `nan` and `inf` as
  numbers and the finite-argument rule above sees only `args`, so a position, rotation, scale, `animspeed`
  or `animtime` that is not finite, or past what a 32-bit float holds (every consumer narrows to `f32`,
  where it turns infinite), would reach physics, the renderer and the saved file. Nothing is placed.
  `set_anim`'s speed and time must fit a 32-bit float for the same reason.
- **An animation on a legacy `.ocmap` level** (`place` with `anim`, `set_anim` with a clip): the legacy save
  has no record for one, so it would play in the session and be gone from the file. Clearing one is fine.
- **`set_transform` (and `player_start` moving an existing marker) while a drag is in flight** in the
  editor: a gizmo drag, a Details-panel field drag or a held nudge. The ABI borrows the gizmo's edit state
  (`beginTransformEdit`/`endTransformEdit`), and doing that mid-drag overwrote the person's before-state
  and closed their gesture, so their own undo entry was lost. Retry once they let go.
- **`set_transform` on a `snap` placement.** A save writes a snapped placement's *authored offset above
  the ground* back verbatim (the gizmo lives with the same rule), so a move reported here would be silently
  undone in the file, and undo does not carry the offset. Remove it and `place` it again with the offset you
  want.
- **The Player Start marker for anything but `set_transform`, `select`, `remove` and `player_start`.** It
  is not a placement, so `set_material`, `set_visible`, `set_collide` and `set_anim` would write state no
  save reads. (`set_transform` moves it and, like `player_start`, also sets its heading, which the editor
  keeps apart from the entity's rotation and outside its undo entry.)

**What is not undoable, and what is not one entry:** `set_material` is not undoable, as the Details
picker is not. `place` and `remove` make one undo entry per entity (the undo stack has no compound
command) and the editor keeps 128, so a larger batch can only be undone in part; `place` builds the
snapshot for the last 128 only. The Player Start's heading is kept apart from its entity by the editor
and is not part of its undo entry.

**Two places repeat editor code instead of calling it**, because the original is private or inside an
ImGui modal, and both name their source in a comment so they can be folded back: `place` rebuilds the
`world::InstantiateOptions` that `game::GameLevel::load` builds (its hooks are private), and Save As
repeats `drawSaveLevelAsPrompt`'s name/ID handling (SandboxShell.cpp).
