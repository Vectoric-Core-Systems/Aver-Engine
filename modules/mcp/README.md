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
`D3D12Device.cpp:217` — so a synthetic click travels the *identical* path as a human one, through the
same handler, in the same order, with no second code path to keep in step. The alternative was calling
ImGui's `io.Add*Event` directly, which would fight the Win32 backend's own `NewFrame` and would exercise
a path no user ever takes.

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

`tests/mcp` covers the parser with no socket at all: `parseCommand` is exposed precisely so the part
where bugs live can be checked without binding a port. 44 assertions, including all ten refusal cases
and the fact that a constructed-but-unstarted bridge is inert.

## Still to wire

**The sandbox does not pump it yet.** The module is built, tested and optional, but nothing in
`SandboxApp.cpp` calls `start()` or `pump()`, so no click has actually been forced. That wiring — a
`--mcp [port]` flag, a guarded `pump()` in `onUpdate`, and the small translation from `InputEvent` to
`PostMessage` — is the remaining step and the point of the whole module.
