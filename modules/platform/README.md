# Aver.Platform  (`modules/platform`)

- **Language:** C++
- **Depends on:** Core
- **Status:** implemented

Everything the engine needs from the operating system, and the only place in the tree that is
allowed to want it. Win32 today; the public headers are written so a second backend is a new file
under `src/`, not a change to the interface.

| Piece | Header | What it is |
|---|---|---|
| Window | `Window.hpp` | the OS window, DPI awareness, the message hook, the modal move/size render tick |
| Splash | `Splash.hpp` | the layered pre-init splash window |
| FileSystem | `FileSystem.hpp` | paths, known folders, file read/write, `IFileOpenDialog` |
| Image | `Image.hpp` | image decode for the splash (stb, public domain) |
| **DirectoryWatcher** | `DirectoryWatcher.hpp` | **watches a directory tree and reports what changed** |

---

## DirectoryWatcher

The editor has to notice a file that an IDE wrote behind its back. A script lives in
`<project>/Content/Scripts/`, gets edited in Visual Studio or VS Code, and without this the editor
has no idea anything happened until it is restarted.

Watching a directory is an OS service (`ReadDirectoryChangesW`), so it lives **here**, beside
`Win32Window` and `FileSystem` — not in the sandbox and not in the RHI. What a changed file
*means* is not this module's business: interpreting a path's type belongs with
`assetTypeFromPath()` in `Aver.Assets`, and drawing the result belongs in the editor.

### Shape

```cpp
DirectoryWatcher watcher;
if (!watcher.start(projectContentDir))
    ; // declined, and said why. Carry on with no watch.

std::vector<FileEvent> events;
if (watcher.poll(events))
    rescanTheTreeMyself();      // the OS dropped records; see "Overflow" below
for (const FileEvent& e : events)
    ...                         // Created / Modified / Deleted / Renamed, path relative to the root
```

`poll()` is called once a frame from the main thread and returns immediately. `start()` returns
false — having logged one line — if the directory does not exist or the watch cannot be opened,
and the editor carries on exactly as it does with no project loaded.

The public header contains no `HANDLE`, no `OVERLAPPED` and no `wchar_t`. It is split in two:

| File | Knows about Windows | Owns |
|---|---|---|
| `src/DirectoryWatcher.cpp` | no | coalescing, rename pairing, the frame-side drain |
| `src/win32/Win32DirectoryWatcher.cpp` | yes | the worker thread, `ReadDirectoryChangesW`, record parsing |
| `src/WatchBackend.hpp` | no | the seam between them: raw records, in order, uninterpreted |

A future backend implements `detail::IWatchBackend` and nothing else. All the subtlety is in the
coalescing, and none of it is OS-specific, so it is written once.

### It never blocks the frame

`ReadDirectoryChangesW` in synchronous mode blocks until something changes, which would freeze the
editor between saves. The backend issues it **overlapped** on its own thread and waits on the
completion event and a stop event together, so a stop is immediate rather than deferred until the
next file change. Parsed records go into a mutex-protected queue; `poll()` takes that mutex,
moves the queue out and returns.

Measured with the harness polling at 60 Hz (see "How this was verified"):

| Load | worst single `poll()` (Debug / Release) |
|---|---|
| idle plus ordinary file operations | **0.35 ms** / **0.24 ms** |
| 20,000 files created as fast as the disk allows | **3.62 ms** / **1.14 ms** |

### Debounce — 150 ms settle, 1000 ms maximum hold

**One save from a real editor is a burst, not an event.** These are the actual OS records, read
from the watched directory with the coalescer switched off:

```
create a file             ADDED a.txt / MODIFIED a.txt                     (2 records, same ms)
append to it repeatedly   MODIFIED a.txt × 20                              (20 records, 2 ms apart)
replace it atomically     ADDED c.tmp / MODIFIED c.tmp / REMOVED c.txt /
                          RENAMED_OLD c.tmp / RENAMED_NEW c.txt /
                          MODIFIED c.txt                                   (6 records, 2 ms apart)
```

Forwarding those raw would report a delete-then-create for every save, and a consumer that read
the file on the first record would read one that does not exist yet.

Events are therefore accumulated **per path** and emitted once that path has been quiet for
`settleMs`. The default is **150 ms**: every burst measured on this machine spans under 20 ms end
to end, so 150 ms clears the worst of them by an order of magnitude while staying inside the
~250 ms at which a UI update stops feeling immediate. It is a *settle* timer and not a fixed
window from the first record, so a burst that keeps arriving keeps deferring — which is exactly
what `maxHoldMs` (default **1000 ms**) exists to bound, for a file being appended to continuously.

Coalescing is by accumulated flags rather than "the last thing that happened", because the records
are not independent: `REMOVED` followed by `RENAMED_NEW` is one save, not a deletion and an
arrival. A temporary file that is created and renamed away inside one window is reported as
nothing at all, which is what it is.

**Created vs Modified cannot be made exact.** A watcher has no memory of what was on disk before
it started, and Windows reports an atomic replace as a rename onto the target with no removal
record. The class resolves the ambiguity towards `Created`, and a consumer keyed on path should
treat `Created` for a path it already knows as `Modified`. The opposite choice would make a
genuinely new file invisible, which is the failure this exists to fix.

### Renames arrive as a pair

`RENAMED_OLD_NAME` and `RENAMED_NEW_NAME` are only meaningful together, and the OS emits the
second immediately after the first. The old name is held until its partner arrives — across reads,
because a pair can straddle two buffers — and an old name whose partner never comes is settled as
a deletion once it goes quiet.

The pair is then interpreted against what else is pending. A rename whose **source was itself
created inside the same window** is an editor's temporary file, not a user renaming something, so
it collapses into a `Modified` (if the target had just been removed) or a `Created` (if it had
not) rather than a spurious `Renamed` from a path nobody has ever seen.

### Overflow — "everything may have changed"

If more changes arrive than the buffer holds, Windows reports `ERROR_NOTIFY_ENUM_DIR`, or completes
the read with **zero bytes**, and gives no detail whatsoever about what changed. That is not a
condition to log and ignore: the watcher's contract is broken for that interval and the caller
must rescan the tree itself.

It is therefore surfaced as `poll()`'s `[[nodiscard]] bool` return rather than as a log line,
because a return value is the one thing a caller cannot accidentally skip. Half-accumulated state
is dropped when it happens, since it describes a world that may no longer exist.

Three ways to lose records, all funnelling into that one signal:

| Cause | Where |
|---|---|
| the OS notification buffer overflowed | `ERROR_NOTIFY_ENUM_DIR`, or a zero-byte completion |
| the frame side stopped draining and the 8192-record queue filled | backend |
| more than 512 distinct paths changed in one window | coalescer |

The last is a policy limit, not a failure: 500+ paths at once is a branch checkout or an unzip, and
reporting it path by path is both slower and less useful than telling the caller to rebuild its
view. The bound is small deliberately — it was 2048 first, and a 20,000-file storm then spent
**28 ms inside one `poll()`**, i.e. a dropped frame, building a table it was about to discard. The
cap is now tested *inside* the fold loop rather than after it, and the same storm costs 3.62 ms.

### Paths

Paths come back **relative to the watched root**, with `/` separators. The Win32 records give a
name whose length is in `FileNameLength`, in **bytes**, and which is **not null-terminated** —
treating it as a wide C string yields paths with whatever happened to follow them in the buffer,
which is why the conversion is length-exact.

---

## How this was verified

A scratchpad harness (not committed) drives the watcher at 60 Hz in two modes — raw OS records, and
coalesced events — printing millisecond stamps and the worst poll duration.

| Case | Records in | Events out |
|---|---|---|
| create `a.txt` | `ADDED`, `MODIFIED` | 1 × `Created a.txt` |
| append to it | `MODIFIED` | 1 × `Modified a.txt` |
| rename `a.txt` → `b.txt` | `RENAMED_OLD`, `RENAMED_NEW` | 1 × `Renamed b.txt <- a.txt` |
| delete `b.txt` | `REMOVED` | 1 × `Deleted b.txt` |
| atomic replace of an existing `c.txt` | 6 records incl. a rename pair | 1 × `Modified c.txt` |
| atomic write of a new `d.txt` | 4 records incl. a rename pair | 1 × `Created d.txt` |
| `mkdir Scripts` | `ADDED` | 1 × `Created Scripts` |
| create `Scripts/Spinner.cs` | `ADDED`, `MODIFIED` | 1 × `Created Scripts/Spinner.cs` |
| 20 appends 2 ms apart | `MODIFIED` × 20 | 1 × `Modified a.txt` |
| a temp file created and removed inside one window | 2 records | nothing at all |

Exactly one event per logical change, in every case. 31 assertions, passing in Debug and Release.

**A save from a real editor HAS now been driven through this, and the watcher has a consumer.**

Both were open at the time this file first claimed them. The editor now starts a watch on the
project's Content root when a project opens, pumps it once a frame before the tabs draw, and routes
each event to whichever asset editor owns that path. Measured with the editor running: an external
write to `Scripts/Target.cs` produced one `Modified` event, the owning tab was told, and the tab
re-read the file.

**Wiring it up immediately found a bug this harness had not.** `start()` opened the directory handle,
spawned the worker and returned — but the kernel only records changes for a handle while a
`ReadDirectoryChangesW` is outstanding, and that call happens at the top of the worker's loop. So
`start()` handed back a watcher whose `watching()` said true and which silently dropped everything
until the thread got going: the first file written after `start()` was never reported, the second
always was. `start()` now waits on an event the worker sets the instant its first read is in flight,
fired on every loop exit too, so a first read that fails does not block `start()` for the full
backstop.

That is what `tests/platform/WatcherTest` exists for, and it found it on its first run.

Overflow was exercised three ways, and they are not equally well covered:

- **The 512-path policy limit** and **the 8192-record queue limit** are both driven and confirmed:
  a 20,000-file storm reaches the first, and a stall with the frame side not polling reaches the
  second. Each returns one rescan request and appends nothing.
- **The zero-byte-completion** half of the OS branch is confirmed, but only with the buffer shrunk
  *and* the worker thread stalled. Shrinking it alone is not enough: at a 64-byte buffer a 400-file
  burst still produced 1202 successful 24-byte reads and lost nothing, because the kernel
  drip-feeds one record per read for as long as the worker re-arms promptly.
- **`ERROR_NOTIFY_ENUM_DIR` itself has never been observed.** Both branches that test for it are
  unexercised. They are written to the documented contract, and that is all that can be said.

Declining was driven, not read: a missing directory and a path that is a file both log one line
and return false. Lifetime was driven too — 200 start/stop cycles under live file traffic leak no
handles, and destruction mid-storm completes without hanging.
