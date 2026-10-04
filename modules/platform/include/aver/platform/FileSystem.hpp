#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// The directory containing the running executable.
std::string executableDir();

// Per-user, per-machine state that is neither project nor engine content.
// `%LOCALAPPDATA%\AverEngine` on Windows; the executable directory if the OS will not name it.
std::string userDataDir();

// The user's Documents folder, as the OS reports it (it is routinely redirected).
std::string documentsDir();

// True if the path exists.
bool fileExists(const std::string& path);
// True if the path exists and is a directory.
bool directoryExists(const std::string& path);
// Creates the directory and every missing parent. True if it exists afterwards.
bool createDirectories(const std::string& path);
// Observes every path the engine reads through readFileBytes/readFileText.
//
// EXISTS FOR ONE PURPOSE: letting a packaged game prove it is not reading the tree that built it.
// A package that quietly falls back to a dev-tree asset runs perfectly on the machine that made it
// and fails on every other, and no exit code says so -- the only way to catch it is to ask what was
// actually opened. scripts/verify-game.ps1 asserts every traced path is under the package root.
//
// This is the WHOLE engine file API, so the trace is complete for aver::platform. It does NOT cover
// LoadLibraryW (dxcompiler, nethost, hostfxr), the CLR's own probing, or stbi_load's internal
// fopen. Those limits are stated in verify-game.ps1 rather than implied.
//
// File::open TRACES TOO, in every mode including Create. Tracing only the reading modes would have
// been defensible -- the trace exists to answer "did the package read outside itself" -- but it
// would mean a new open path that quietly is not covered, and the sentence above would stop being
// true. If runtime-written region files (docs/CHUNKS.md slice 7) ever land somewhere verify-game.ps1
// rejects, that is a real question about where they belong and it should surface loudly there rather
// than be pre-suppressed by an untraced open here.
using FileTraceFn = void (*)(const char* path, void* user);
void setFileTrace(FileTraceFn fn, void* user);

// Reports a file open that did NOT go through readFileBytes/readFileText.
//
// The binary format loaders (Avr1, OcMesh, OcAnim) open their own ifstream, so without this the
// trace would cover the manifest and the level and miss every mesh -- and verify-game.ps1 would
// happily pass a package that reaches into the dev tree for its geometry. Measured: a packaged
// game reported 2 traced opens before these loaders called it.
void traceFileOpen(const std::string& path);

// Reads a whole file into `out`. False if it cannot be read.
bool readFileBytes(const std::string& path, std::vector<u8>& out);
// Reads a whole file into `out` as text. False if it cannot be read.
bool readFileText(const std::string& path, std::string& out);
// Writes `size` bytes to the file, truncating it. False on failure.
//
// TRUNCATES ON OPEN, before a single byte of `data` has landed. That is fine for content this
// process can regenerate on the next run -- OcBt.cpp's saveOcBt and OcNav.cpp's saveOcNav call
// this directly and say why in their own comments -- and it is NOT fine for anything a crash,
// a kill, or a full disk must not be allowed to destroy. Callers in the second category want
// writeFileBytesAtomic/writeFileTextAtomic below, not this.
bool writeFileBytes(const std::string& path, const void* data, usize size);
// Writes `text` to the file, truncating it. False on failure. Same truncate-on-open hazard as
// writeFileBytes, for the same reason (it is implemented in terms of it).
bool writeFileText(const std::string& path, const std::string& text);

// Writes `size` bytes to `path` WITHOUT the truncate-on-open hazard above: the new content is
// written to a temporary beside `path` first, and only a COMPLETE temporary is ever swapped into
// place (see renameFile). A crash, a kill, or a full disk between the two calls this makes leaves
// the ORIGINAL file exactly as it was -- an observer never sees a truncated or half-written file
// at `path`, only the whole old one or the whole new one.
//
// THE SAME PATTERN aver::fmt::saveOcSave (modules/formats/src/OcSave.cpp) and
// aver_settings_flush (modules/settings/src/Settings.cpp) already ship with for the player's save
// and the settings file, lifted here so it has ONE implementation instead of being hand-rolled
// again at every call site that turns out to need it -- which is how the level, material and
// graph writers ended up with no rename step at all in the first place.
//
// A failed write reports false and leaves `path` untouched, including when `path` does not exist
// yet (an ordinary first save): the temporary is either never created (the write into it failed)
// or deleted again (the rename failed), so a save that fails never leaves ".tmp" litter behind.
//
// THE SWAP ITSELF NOW RETRIES. `path` being briefly held open by something outside this process --
// a virus scanner's on-access scan of the file this function just finished writing, an indexer, a
// backup agent -- used to be indistinguishable from "can never be replaced" and failed on the first
// try. It is now retried a small bounded number of times, trying ReplaceFileW as well as
// MoveFileExW on each pass, before giving up; every existing caller gets this for free with no
// signature change, because it only makes a swap that COULD succeed more likely to, and changes
// nothing about what happens when it genuinely cannot.
//
// `fallback` and `outError` are the two things a caller can opt into without changing the default
// behaviour above (both are optional; every existing call site is unaffected):
//   * `outError`, when non-null, is filled in on a failure with which step failed ("write temp",
//     "swap (MoveFileExW)", "swap (ReplaceFileW)") and the platform error code for it, so a caller
//     that wants to say something more useful than "could not write" can.
//   * `fallback` decides what happens when the retried swap STILL cannot complete. The default,
//     `AtomicFallback::Refuse`, is today's behaviour: report failure and leave `path` exactly as it
//     was. `AtomicFallback::DirectWrite` instead falls back to a plain truncating write straight to
//     `path` -- trading away the crash-safety guarantee for "the content reaches disk at all" -- and
//     exists for content a caller has explicitly decided is cheap to lose to a crash mid-write, a
//     narrower risk than the one a stuck external lock already poses to it. A level, a material, a
//     mesh, a save or the project manifest must never pass this: losing the crash-safety guarantee
//     on those is a worse trade than the write occasionally failing outright.
enum class AtomicFallback {
    Refuse,       // leave `path` untouched and report failure -- the default, and the ONLY choice
                  // for anything whose half-written or reverted state would be worse than stale
    DirectWrite,  // if the swap cannot complete, write `path` directly instead of failing outright
};

// True for a platform error code that plausibly means "another process has this file open right
// now" (worth a small bounded retry) rather than "this can never work" (retrying is wasted effort).
// Pulled out as its own testable decision for the same reason aver::editor::prefsShouldRefuseWrite
// is one: ERROR_SHARING_VIOLATION (32), ERROR_LOCK_VIOLATION (33) and ERROR_ACCESS_DENIED (5) are
// part of the stable Win32 error ABI (winerror.h), so the POLICY of which codes are worth retrying
// can be stated and tested as plain integers, with no Windows header and no live failure needed to
// reach it. Used by the swap retry in writeFileBytesAtomic/renameFile; meaningless for the errno
// values the non-Windows build puts in AtomicWriteError::errorCode; that path does not call this.
bool isTransientFileError(u32 platformErrorCode);

// What writeFileBytesAtomic/writeFileTextAtomic can report about a failure, when the caller wants
// more than a bare `false`. `op` is empty and `errorCode` is 0 unless a write step actually failed
// (a no-op call, or one that succeeded, never touches this).
struct AtomicWriteError {
    std::string op;         // "write temp", "swap (MoveFileExW)", or "swap (ReplaceFileW)"
    u32         errorCode = 0;   // GetLastError() on Windows, errno elsewhere; 0 if not applicable
};

bool writeFileBytesAtomic(const std::string& path, const void* data, usize size,
                          AtomicFallback fallback = AtomicFallback::Refuse,
                          AtomicWriteError* outError = nullptr);
// Writes `text` to the file safely; see writeFileBytesAtomic for what "safely" means and why.
bool writeFileTextAtomic(const std::string& path, const std::string& text,
                         AtomicFallback fallback = AtomicFallback::Refuse,
                         AtomicWriteError* outError = nullptr);

// Human-readable text for a platform error code (errorCode above), for a log line that says WHY a
// write failed rather than just that it did. FormatMessage on Windows, strerror elsewhere. Empty if
// the platform has nothing to offer for that code -- a caller should treat that as "no extra detail"
// rather than print an empty parenthetical.
std::string describePlatformError(u32 code);

// Deletes a file. True if it is gone afterwards -- INCLUDING when it never existed, because "make
// sure this is not there" is what every caller actually wants and a missing file already satisfies
// it. False only if it exists and could not be removed.
bool deleteFile(const std::string& path);

// Renames `from` to `to`, REPLACING `to` if it exists. Atomic where the OS provides it (it does on
// Win32 and on POSIX), which is what makes write-to-temp-then-rename a crash-safe way to replace a
// file: an observer sees either the whole old file or the whole new one, never a half-written mix.
bool renameFile(const std::string& from, const std::string& to);

// ---- ranged file access ----------------------------------------------------------------------
//
// EVERYTHING ABOVE IS WHOLE-FILE. That was the entire I/O surface until this type, and it is why a
// streamed world was not expressible: reading one 16 m chunk out of a region archive meant reading
// and hashing the whole archive, and adding a chunk meant rewriting it. See docs/CHUNKS.md B1.
//
// WHY A NATIVE HANDLE AND NOT std::fstream. Two reasons, and the second is the load-bearing one:
//
//   1. Positional. `readAt`/`writeAt` take an offset rather than depending on a stream position,
//      so there is no seek-between-read-and-write dance to get wrong.
//   2. DURABILITY. `std::ostream::flush()` pushes the C++ buffer into the OS page cache and stops
//      there -- it says nothing about the disk. `sync()` below is FlushFileBuffers/fsync. The
//      region format's crash safety is an ORDERING argument ("the payload is on disk before the
//      directory entry that points at it"), and without a real barrier that ordering is fiction:
//      the OS is free to write the two back in either order.
class File {
public:
    enum class Mode {
        Read,       // must already exist; writes are rejected
        ReadWrite,  // must already exist; does NOT truncate
        Create,     // created if missing, opened as-is if present; does NOT truncate
    };

    File() = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Opens the file, closing whatever this held before. False if it could not be opened.
    bool open(const std::string& path, Mode mode);
    void close();
    bool isOpen() const;

    // Length in bytes, or -1 if it cannot be determined.
    i64 size() const;

    // Reads EXACTLY `bytes` at `offset`. A short read is a FAILURE, not a partial success: this is
    // how a region directory entry gets read, and half of one is corruption that would otherwise be
    // indistinguishable from a valid entry pointing somewhere arbitrary.
    bool readAt(u64 offset, void* out, usize bytes) const;

    // Writes EXACTLY `bytes` at `offset`, growing the file if it ends before then. NEVER truncates,
    // which is the whole difference from writeFileBytes. Writing past the end leaves a gap that
    // reads back as zeros.
    bool writeAt(u64 offset, const void* data, usize bytes);

    // Sets the length exactly, growing (zero-filled) or shrinking. Shrinking is how a region is
    // compacted; growing is how its sector space is reserved.
    bool setSize(u64 bytes);

    // Pushes everything written through this handle to the DEVICE, not merely to the OS cache.
    // Returns false if the platform reported the barrier failed -- which must be treated as "the
    // ordering guarantee did not hold", not as a warning.
    bool sync();

private:
#if defined(_WIN32)
    void* handle_ = nullptr;   // HANDLE; INVALID_HANDLE_VALUE is normalised to nullptr
#else
    int fd_ = -1;
#endif
    bool writable_ = false;
};

// Shows the native open-file dialog. `spec` is a semicolon-separated pattern list, e.g. "*.ocproject".
// False if the user cancelled or the shell dialog is unavailable.
bool openFileDialog(const std::string& title, const std::string& filterLabel,
                    const std::string& spec, const std::string& initialDir, std::string& out);

} // namespace aver
