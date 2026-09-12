# Headless vs. editor: every way a `--frames` capture is not the image you see

The complaint, verbatim: "the headless you said is different from the one I see." It is correct,
it has been correct multiple times, and it bit this session too — a `--frames` capture showed a
darkening the live editor does not show, and the reverse has also happened. This document exists so
the next divergence gets checked against a list instead of re-discovered by surprise.

Everything below is read from the source, with a `file:line`, not inferred from behaviour. Where a
divergence has no workaround, that is stated plainly rather than papered over.

---

## 1. Auto exposure is forced OFF in every capture, unless asked for

`sandbox/src/SandboxApp.cpp:6991-6993`:

```cpp
// Disables auto-exposure for a capture run unless the run asked for it.
void applyCaptureExposureRule(bool explicitlyRequested) {
    if (maxFrames_ != 0 && !explicitlyRequested) post_.autoExposure = false;
}
```

Called from `main()` right after the post settings are assigned, `SandboxApp.cpp:26827-26828`:

```cpp
app->setPost(exposure, exposureSet, bloom, bloomSet, autoExposure);
app->applyCaptureExposureRule(autoExposure);
```

`maxFrames_ != 0` is this file's own established test for "this is a bounded capture run, not an
interactive session" — the same predicate `loadEditorPreferences`/`saveEditorPreferences` use (see
§3). So: any `--frames N` run — every gate, every benchmark, every screenshot taken by a script —
renders with auto-exposure **off** unless `--auto-exposure` is passed explicitly on that same
command line. The interactive editor has no such rule; it runs with whatever `post_.autoExposure`
last was (compiled default `true`, or whatever `editor.ini` has, see §3).

**What it invalidates:** any capture that was not passed `--auto-exposure` is measuring a
fixed-exposure image. A brightness/darkness difference between a capture and the live editor in a
scene with any exposure range at all (day/night, indoor/outdoor, HDR skies) is expected, not a
rendering bug, unless both sides are known to be in the same exposure mode. This is very likely
what produced this session's "headless is darker/brighter than the editor" observation.

**Workaround:** pass `--auto-exposure` on the capture — but note that then the two still won't
usually match on a **specific frame**, because auto-exposure adapts over time (`post_.exposureSpeed`)
and a short bounded run does not get as long to converge as a human sitting in the editor. There is
no flag that makes a *bounded* run reach the same converged exposure a human eventually sees; the
honest comparison point is `--auto-exposure` plus enough frames for the histogram to settle, not a
default one.

---

## 2. Chunk streaming: it is not "on differently" — it is a race a short run loses

Streaming is not itself an editor-vs-headless switch. `chunkStreamAutoFrames_` defaults to `5` in
**both** modes (`SandboxApp.cpp:9840-9846`, comment: "ON BY DEFAULT") and fires
`setChunkStreamingEnabled(true)` at frame 5 unconditionally (`SandboxApp.cpp:3657-3659`) unless
`--no-chunk-stream` was passed. So a `.ocworld`-backed level starts streaming in exactly the same
way for a `--frames` run and for the interactive editor. The divergence is in how much of it has
arrived by the time either one is looked at.

### 2a. Total failure — same in both modes, but only the editor lets you notice

`SandboxApp.cpp:23727-23730` and `:23895-23898`:

```cpp
if (!project_.valid()) {
    AVER_WARN("[ChunkWorld] cannot enable streaming: no project is open");
    return;
}
...
if (built.empty()) {
    AVER_WARN("[ChunkWorld] cannot enable streaming: no density field produced a world");
    return;
}
```

`built` ends up empty when every declared `PCGVOLUME` either has no `SCATTER` species pointed at it
(`:23779-23783`, `AVER_INFO("[ChunkWorld] field '{}' has no SCATTER species; not streamed", ...)`)
or fails to open (`:23877-23883`). This is a **project-authoring** condition, not a headless-only
one — an interactive session hits exactly the same warning. The difference is entirely about who
notices: a person looking at the viewport sees an empty world and goes looking for why; a `--frames`
run just produces an image (or a screenshot) with most of the level missing and exits 0, and unless
someone reads the log line, the capture reads as "the renderer is broken" instead of "the level
declared no usable scatter field."

**What it invalidates:** any capture from a `.ocworld` project whose log was not checked for this
line. A frame comparison (before/after a shader or renderer change) done against such a capture is
comparing two images of the *fallback single-species cube* world, not the level.

**Check:** grep the run's log for `cannot enable streaming` before trusting a capture of a
`.ocworld` project at all.

### 2b. Partial population — the real "only a fraction of the scene" case

Even when streaming *does* turn on, it is budget-limited by design, `modules/world/include/aver/world/ChunkStreamer.hpp:41-44`:

```cpp
// The only thing bounding the frame hitch, since the load is synchronous. Zero means unbounded,
// which is what a "load everything now" test wants and no running game should use.
u32 loadBudget = 2;
u32 evictBudget = 4;
```

and the default radius, `ChunkStreamer.hpp:37`: `i32 loadRadius = 3;` (a 7×7×`verticalRadius` cube of
chunks, Chebyshev distance) — but a level can declare a much wider field: the multi-field PCG path
(`SandboxApp.cpp:23732-23843`) clamps radius only at `kMaxRadiusChunks = 24` (`:23836`), and this
codebase's own PCG notes describe a real canopy field at radius 10. At the shipped `loadBudget = 2`
chunks/frame, a radius-3 slab (up to ~150 chunks depending on `verticalRadius`) needs on the order
of 70+ frames after streaming turns on to fully populate; a radius-10 field is 10× that.

Add the 5-frame delay before streaming even starts (`chunkStreamAutoFrames_`, §above), and the
capture-frame offset (§4: the image a `--frames N` run actually captures is from frame `N-3`, not
frame `N`), and a modest `--frames 128` run can easily capture its image while the streamer still
has hundreds of chunks in `pendingLoads` (`ChunkStreamer.hpp:76`, `StreamStats::pendingLoads`). An
interactive session run for the tens of seconds it actually takes a person to look around does not
have this problem — the backlog drains and stays drained (see the in-file measurement note at
`SandboxApp.cpp:3673-3675`: "the backlog drains and this settles to 0.7ms at pending=0").

**What it invalidates:** any capture of a `.ocworld` level compared against what a human sees after
looking around for a few seconds. Two captures at different `--frames N` values of the *same*
level are not directly comparable either, if `N` is small enough that streaming hasn't converged —
a later capture can legitimately show more geometry than an earlier one for no reason other than
having had more frames to stream, which looks exactly like a renderer regression.

**Workaround:** either run long enough for `pendingLoads` to hit 0 (check the stream-stats log line
before trusting the frame — `world stream_stats` is also exposed over the MCP `world` ABI,
`SandboxApp.cpp:25361`), or shrink the field's declared radius for the capture, or accept that a
bounded capture of a large streaming world is a capture of a *partially streamed* world and say so
in whatever the capture is used to justify.

---

## 3. Editor preferences: a capture run and a person can be reading two different `editor.ini`s — and even the same file is honoured differently

### 3a. Two different files, when run from the MCP tool

`tools/mcp/aver_mcp.py:279-299` (comment block above `tool_run`):

> When this server is hosted inside a packaged (MSIX) app, every process it spawns inherits that
> package identity, and Windows redirects `%LOCALAPPDATA%` for the whole subtree. `Sandbox.exe`
> then reads and writes `AppData/Local/Packages/<package>/LocalCache/Local/AverEngine/editor.ini`
> instead of `AppData/Local/AverEngine/editor.ini` — a copy-on-write shadow of the real file... It
> only diverges once the two are edited apart — and then a preference the user has set is simply
> absent from every run made here, with no warning and no diff to notice.

`userDataDir()` itself (`modules/platform/src/FileSystem.cpp:72-78`) just asks Windows for
`FOLDERID_LocalAppData` and appends `\AverEngine` — it does no redirecting itself and has no idea
it is being redirected; the redirection is done by Windows to the whole process tree because of
*how* the process was launched (from inside the MSIX-packaged host), not by anything in this
engine. The log line printed at load time (`"[Prefs] N setting(s) from ...AverEngine/editor.ini"`)
prints the **unredirected** path it asked for, so the log itself gives no indication a shadow copy
was actually used.

**What it invalidates:** anything render-affecting that lives in `editor.ini` and was checked or
changed by the user through Windows Explorer / a text editor / the editor's own Preferences panel
running outside the packaged host — vsync, render scale, AverSR quality, exposure, bloom, view
flags — compared against a run launched through this MCP server. They can silently disagree
indefinitely.

**Workaround, per `aver_mcp.py`'s own rule:** "ANYTHING THAT DEPENDS ON A PERSISTED EDITOR
PREFERENCE MUST BE RUN FROM A SHELL, not from here. Flags are unaffected — they are argv, and argv
is not redirected." I.e. run from a plain PowerShell/terminal instance when the question involves a
stored preference, and prefer explicit CLI flags (§3b) over "whatever is saved" when the two need to
agree.

### 3b. Even from the same file, a capture and the editor apply different subsets of it

`loadEditorPreferences()`, `SandboxApp.cpp:20716-20850`, is not read uniformly. Several groups are
explicitly gated `if (maxFrames_ == 0)` — i.e. **skipped entirely on a `--frames` run**, comment at
`:20738-20742`:

> THE VIEW FLAGS ARE INTERACTIVE-ONLY, both directions... they change the IMAGE, `--unlit` and the
> Show menu can set them, and a stored one reaching a `--frames` run would move all twenty gate
> probes on one machine and not another.

Gated the same way (`:20743`, `:20766`, all inside `if (maxFrames_ == 0)`):

- `viewport.unlit`, `viewport.showStaticMeshes`, `viewport.showAtmosphere`
- `post.exposure`, `post.autoExposure`, `post.bloomIntensity` (only when the CLI didn't already set
  them — `postExposureFromCli_`/`postAutoExpFromCli_`/`postBloomFromCli_`), plus
  `post.exposureKey/Speed/Min/Max`, `post.bloomThreshold/Knee`, `post.histogramLow/High`

**NOT gated** — applied identically whether interactive or captured (`:20793-20847`):

- `display.vsync` (if the device supports disabling it)
- `display.renderScale` (behind the crash-safety cookie described at `:20795-20825` — a stored
  scale below 1 that previously bricked a launch resets itself to 1 rather than being trusted twice)
- `display.aversr` (upscaler quality)

So: a stored **exposure/bloom/unlit/atmosphere** preference from the editor is invisible to any
`--frames` run — which is generally the *right* behaviour for a gate (it is what keeps the twenty
gate probes reproducible across machines with different saved editor state), but it means "what the
capture shows" and "what my editor.ini says my view should look like" are two different sources of
truth on those specific fields even outside the MSIX-redirect case above. A stored **vsync/
render-scale/AverSR** preference, by contrast, *does* reach a capture — which matters because it
means a capture run's render scale or upscaler quality is NOT necessarily 1.0/Off by default; it is
whatever was last saved, unless a CLI flag pins it.

The write side has the mirror-image guard and a documented history of NOT having it
(`SandboxApp.cpp:20916-20933`): `onShutdown()` calls `saveEditorPreferences()` **unconditionally**,
so before the `if (maxFrames_ == 0)` guard was added around `display.vsync`/`display.renderScale`/
`display.aversr`, *every* `--frames N --render-scale F` or `--no-vsync` capture run wrote its
measurement-only settings back into the same `editor.ini` an interactive session reads — "so every
capture run wrote its measurement settings into the SAME `%LOCALAPPDATA%/AverEngine/editor.ini` an
interactive session reads back... Measured against this session's own history: `--no-vsync` is
passed by every capture harness in `scripts/`, so the user's vsync preference has been decided by
whichever measurement ran last." This is fixed now (the guard is in place at `:20934`), but it is
exactly the shape of bug this document exists to keep from recurring, and any build older than that
guard, or any code path that adds a third pref without matching the guard, reopens it.

**What it invalidates:** comparing a capture's exposure/bloom/unlit state to "what I have configured
in the editor" is meaningless — captures never see those. Comparing render scale/vsync/AverSR
quality between the two is meaningful, but only if you know what was last saved and by what.

**Workaround:** for exposure/bloom/view flags, always pass the value explicitly on the capture's
command line (`--exposure`, `--bloom`, `--auto-exposure`, `--unlit`) rather than relying on
`editor.ini` — that's the only source a capture reads for these. For vsync/render-scale/AverSR,
either pass `--no-vsync`/`--render-scale`/`--aversr` explicitly on **every** capture (CLI always
wins over the stored value — `renderScaleOverride_ == 1.0f` and `!averSrFromCli_` are the sentinel
checks at `:20807` and `:20839`), or accept that they track whatever an interactive session (or a
prior capture, if somehow run before this fix) last saved.

---

## 4. The captured frame is not frame `N` — it is `N-3`

`SandboxApp.cpp:22536-22572`, `captureCheck()`:

```cpp
void captureCheck(Engine& e) {
    const u64 f = e.time().frame;
    const u64 sf = maxFrames_>8?maxFrames_-3:4;
    ...
    if (f==sf) {
        e.device()->requestCapture(px_, py_);
        capX_=px_; capY_=py_; capVpX_=vpX_; capVpY_=vpY_; capVpW_=vpW_; capVpH_=vpH_;
    }
    if (f>sf && !capDone_){
        ...
        f32 px[4]; if (e.device()->getCapture(px)) { ... }
        if (!shot_.empty()){ ... stbi_write_png(shot_.c_str(), ...) ... }
        capDone_=true;
    }
}
```

For `--frames N` with `N > 8`, the pixel probe and the `--shot` PNG are both taken from the frame
**requested at `f == N-3`**, whose GPU readback is collected and written out on the very next frame
(`f > sf`, and `capDone_` latches true the first time that fires — no retry loop). So a
`--frames 124` run captures the render state of **frame 121**, not frame 124, and the file/pixel
values are actually read back and written during frame 122's tick. (For `N <= 8` it's pinned to a
fixed `sf = 4` instead, so very short runs capture frame 4 regardless of `N`.) The same file also
uses an `N-2`/`N-1` convention for `--ray-probe` and `--gpu-timing` (`:22478-22480`,
`:22498-22500`, `want = maxFrames_ > 8 ? maxFrames_ - 2 : maxFrames_ - 1`) — a **different** offset
from the screenshot/pixel-probe's `N-3`. These two offsets do not agree with each other, so a
`--frames N --gpu-timing --shot out.png` run's GPU-timing print and its screenshot are not
describing the same frame either.

**What it invalidates:** any claim of the form "at frame N, the image was X" made by reading a
`--shot`/probe result and quoting `N` — it describes frame `N-3` (or a fixed frame 4, for `N ≤ 8`).
This matters most for anything with an N-frame ramp or settle (temporal accumulation, streaming
population from §2, an animation timed to a specific frame) — the memory note on this exact offset
records a matched-pose comparison needing `30000+4` frames specifically to land the capture on the
intended pose once this offset is accounted for.

**Workaround:** when a specific in-game frame must be captured, request `--frames` as
`(desired_frame + 3)` (or `+2`/`+1` respectively when reasoning about `--gpu-timing`/`--ray-probe`
instead of the pixel/shot path), not the desired frame number itself. There is no flag that lets a
capture name the frame it wants directly.

---

## 5. Render scale, vsync, WARP: not force-defaulted by `--frames` itself — but easy to get wrong

Unlike auto-exposure, none of render scale, vsync, or the WARP software rasteriser are *forced* to a
different value by `maxFrames_ != 0` — each is a plain opt-in flag:

- `--render-scale F` (`SandboxApp.cpp:20807`, `renderScaleOverride_`)
- `--no-vsync` (`:6816`, `vsyncOffRequested_`, applied at `:15822-15826`)
- `--warp` (`:23119`, `useWarp_`, runs the D3D12 software rasteriser instead of the GPU —
  `:26568`)

So the divergence here is not "headless defaults differently," it is that **every capture harness
in this repo passes `--no-vsync`** (per the comment at `SandboxApp.cpp:20928-20930`) while a human
using the editor normally has vsync on — meaning frame-timing numbers from a capture and from
"how it feels to use" are never on the same footing unless the person also runs `--no-vsync`
interactively for the comparison. See `docs/` frame-timing notes for why vsync must be off when
*measuring*: a vsync-locked frame time quantises to multiples of the monitor's refresh interval and
silently launders real timing variance into a much smoother-looking number.

**What it invalidates:** any frame-time comparison between a `--frames` capture (vsync off, by every
harness's own convention) and "does it feel smooth in the editor" (vsync on, by Windows' default).
Neither number is wrong; they are not measuring the same thing.

**Workaround:** none needed beyond being explicit — pass `--no-vsync` (or don't) on both sides of
whatever comparison is being made, and say in the writeup which one was used.

---

## 6. The window itself behaves differently under `--frames`

A few structural differences, each already deliberate and each still worth knowing about because
they change what a screenshot can and cannot show:

- **No splash screen.** `SandboxApp.cpp:7754-7769`: the loading screen is constructed with
  `e.window() != nullptr && maxFrames_ == 0` — i.e. never shown during a capture, "a top-most splash
  must not land in a screenshot." An interactive launch always sees it; a capture never does, so
  captured startup timing/frame-count does not include splash time the way a person's experience of
  launching does.
- **The window opens unactivated.** (See `aver-no-focus-steal` in engine notes / the fullscreen
  comment below) — a capture's window never takes OS focus, so anything gated on window-activation
  state (some input paths, some OS-level compositor behaviour) will not exercise the same code path
  a focused interactive window does.
- **`--fullscreen` is silently overridden off during a capture.** `SandboxApp.cpp:1356-1369`:
  `c.fullscreen = fullscreenOverride_ && maxFrames_ == 0;` — even an explicit `--fullscreen` on a
  bounded run is ignored, "Even an explicit `--fullscreen` must not reshape the window under a
  measurement," because every gate probe is a pixel at a fixed rect in a fixed client area. So a
  fullscreen-specific bug (a different backbuffer size, a different DPI/monitor scaling path) simply
  cannot be captured by `--frames` at all, by design — there is no flag that gets around this one.
- **Resizing.** The window is never resized by anything during a normal capture; `--resize-cycle N`
  (`:22508-22534`) exists specifically because ordinary render-scale/AverSR-cycle flags all go
  through `rebuildSceneTargets`, a different path from an actual OS window resize
  (`D3D12Device::resize`) — so a capture that never passes `--resize-cycle` has exercised none of
  the real-resize code path a person dragging the window's edge exercises every time they do it.

---

## Before you trust a capture — checklist

1. **Was `--auto-exposure` passed?** If not, the image is fixed-exposure and any brightness
   difference from the live editor may be nothing but that (§1).
2. **Is this a `.ocworld`/streaming project?** Grep the run's log for `cannot enable streaming`
   (§2a). If streaming did turn on, was the run long enough — accounting for the 5-frame startup
   delay and the `loadBudget = 2` chunks/frame default — for `pendingLoads` to reach 0 before the
   capture frame (§2b)? A short `--frames N` on a wide-radius field is very likely showing a
   partially streamed world.
3. **Was this run launched through the MCP server inside a packaged (MSIX) host?** If a stored
   `editor.ini` preference matters to the question being asked, re-run from a plain shell instead —
   the packaged host reads a redirected shadow copy of `editor.ini` with no warning that it did
   (§3a).
4. **Which preferences were relied on?** Exposure/bloom/unlit/atmosphere are never read from
   `editor.ini` by a `--frames` run — pass them explicitly or they're whatever the CLI/compiled
   default is, not what the editor shows. Vsync/render-scale/AverSR quality ARE read from
   `editor.ini` by a capture, same as the editor, unless a CLI flag overrides them (§3b).
5. **What frame does the number/pixel/screenshot actually describe?** `--frames N` captures frame
   `N-3` (or a fixed frame 4 when `N ≤ 8`); `--gpu-timing`/`--ray-probe` report from `N-2` instead.
   If a specific frame matters, request `N+3` (or `+2`), not `N` (§4).
6. **Is vsync in the same state on both sides of the comparison?** Every capture harness in
   `scripts/` passes `--no-vsync`; the editor does not by default (§5).
7. **Does the question involve fullscreen, a window resize, focus/activation, or the startup
   splash?** None of those can be observed from a `--frames` capture — fullscreen is force-disabled,
   the window never activates or resizes on its own, and the splash never shows. There is no flag
   that changes this for fullscreen; `--resize-cycle` is the only lever that reaches a real resize
   (§6).

If the answer to more than one of these is "unknown," the capture and the editor were never
measuring the same thing, and the right next step is to re-run the capture with the missing flags
made explicit — not to trust either image over the other.
