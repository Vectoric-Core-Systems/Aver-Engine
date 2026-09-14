// Editor preference store: reads and writes the `key=value` editor.ini under the user data dir.
#include "EditorPrefs.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace aver::editor {
namespace {

// std::map, not unordered_map: the file must write in a stable, diffable order.
std::map<std::string, std::string, std::less<>> g_values;
std::string g_path;
bool g_loaded = false;
bool g_dirty  = false;
// Latched when the file existed and could not be read. See prefsShouldRefuseWrite in the header.
// NO LONGER PERMANENT -- see tryRecoverReadOnly below. It still means exactly what its name says
// for as long as it is set: flushEditorPrefs will not write over a file this session never
// successfully read.
bool g_readOnly = false;

// A handful of retries at load, a few milliseconds apart, for the same reason the swap in
// FileSystem.cpp's writeFileBytesAtomic retries: "the file existed and could not be read" and "a
// virus scanner/indexer/backup agent had it open for a moment" look identical from here, and only
// one of those two should cost the rest of the session. Bounded and cheap either way -- the
// ordinary case is the FIRST read succeeding, and this only runs at all on the failure path.
constexpr int kLoadReadRetries = 3;
constexpr int kLoadReadRetryDelayMs = 10;

// Parses `key=value` lines from `text` into `out`, skipping blanks and `#` comments -- the same
// grammar ensureLoaded and tryRecoverReadOnly both need. `onKey`, when set, is called for every key
// found (used by the merge below to know which keys the file already agreed with this session on).
template <class OnKey>
void parseInto(const std::string& text, std::map<std::string, std::string, std::less<>>& out,
              OnKey onKey) {
    usize line = 0;
    while (line < text.size()) {
        usize end = text.find('\n', line);
        if (end == std::string::npos) end = text.size();
        std::string_view row(text.data() + line, end - line);
        line = end + 1;
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        if (row.empty() || row.front() == '#') continue;

        const usize eq = row.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key(row.substr(0, eq));
        onKey(key, out);
        out.emplace(std::move(key), std::string(row.substr(eq + 1)));
    }
}

// Reads editor.ini once (with the bounded retry above), or leaves the store read-only for the
// session if it never becomes readable. Every pref accessor calls this, and loadEditorPreferences
// plus keybinds_.loadFromPrefs() make dozens of those calls in a row, so retrying on EVERY one of
// them would turn a single stat into a per-access one -- g_loaded still latches after this first
// attempt, same as before. What changed is that "this one attempt" is now a few attempts.
void ensureLoaded() {
    if (g_loaded) return;
    g_loaded = true;

    const std::string dir = userDataDir();
    if (dir.empty()) {
        AVER_WARN("[Prefs] no user data directory; editor preferences will not persist this session");
        return;
    }
    g_path = dir + "/editor.ini";

    std::string text;
    bool ok = readFileText(g_path, text);
    for (int attempt = 1; !ok && attempt < kLoadReadRetries && fileExists(g_path); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kLoadReadRetryDelayMs));
        ok = readFileText(g_path, text);
    }

    if (!ok) {
        // A MISSING FILE IS THE ORDINARY FIRST RUN. An UNREADABLE one is not, and the two arrive
        // here as the same `false` -- so existence is asked separately, and a session that could
        // not read an existing file never writes over it.
        if (prefsShouldRefuseWrite(false, fileExists(g_path))) {
            g_readOnly = true;
            AVER_WARN("[Prefs] {} exists but could not be read after {} attempt(s); preferences are "
                      "READ-ONLY for this session rather than being overwritten with defaults -- a "
                      "later save will retry the read and merge in whatever this session has not "
                      "already changed", g_path, kLoadReadRetries);
        }
        return;
    }

    parseInto(text, g_values, [](const std::string&, const auto&) {});
    AVER_INFO("[Prefs] {} setting(s) from {}", g_values.size(), g_path);
}

// Distinct write-failure signatures already reported THIS SESSION -- keyed on (operation, error
// code) rather than a single bool, so a DIFFERENT failure arriving later (a full disk, say, after
// an antivirus lock clears) is still reported once of its own. Without this, a swap that keeps
// failing under the autosave timer's 0.25s cadence would log four times a second for as long as
// the lock lasted -- Log.cpp's writer has no dedup of its own (every AVER_* call reaches stderr and
// the Output Log unconditionally), so nothing upstream would have caught it either.
std::set<std::pair<std::string, u32>> g_reportedWriteFailures;

// Logs a write failure ONCE per distinct (operation, error code) this session, naming the path, the
// step that failed, and the platform error if one was captured. AVER_ERROR rather than AVER_WARN --
// deliberately, and it is the only change here to what gets logged AT rather than how often:
// pushFromLog (EditorNotifications.cpp) only turns Error/Critical into a toast, by design ("Warn is
// excluded because this editor logs Warn for ordinary outcomes"), so this reaches the user as a
// real notification through the sink SandboxApp already installs, with NO NEW INCLUDE and NO NEW
// COUPLING from this file to EditorNotifications.hpp -- and "a preference silently never reaching
// disk" is exactly the kind of failed-but-the-process-is-fine outcome Log.hpp defines Error to mean.
void reportWriteFailure(const std::string& path, const AtomicWriteError& err) {
    const auto key = std::make_pair(err.op, err.errorCode);
    if (!g_reportedWriteFailures.insert(key).second) return;   // already told this session

    const std::string detail = err.errorCode ? describePlatformError(err.errorCode) : std::string();
    if (err.errorCode != 0 && !detail.empty())
        AVER_ERROR("[Prefs] could not write {} ({}, error {}: {})", path,
                  err.op.empty() ? "unknown step" : err.op, err.errorCode, detail);
    else if (err.errorCode != 0)
        AVER_ERROR("[Prefs] could not write {} ({}, error {})", path,
                  err.op.empty() ? "unknown step" : err.op, err.errorCode);
    else
        AVER_ERROR("[Prefs] could not write {} ({})", path, err.op.empty() ? "unknown step" : err.op);
}

// Re-attempts the read that latched g_readOnly, called lazily from flushEditorPrefs rather than on
// its own timer -- flushEditorPrefs already only runs when there is something worth persisting, so
// this only costs anything on a session that is both read-only AND has a pending change, which is
// exactly the situation worth spending a retry on.
//
// MERGE ON RECOVERY: every key currently in g_values was set by THIS session (the store started
// empty -- the load never succeeded), so session values win simply by never being overwritten; any
// OTHER key the file holds -- settings from a previous run this session never had a chance to load
// -- is adopted, so recovering does not silently drop them the next time this writes.
bool tryRecoverReadOnly() {
    std::string text;
    if (!readFileText(g_path, text)) return false;

    // Called BEFORE parseInto's emplace for that key, so `sessionAlreadyHasKey` reflects the map as
    // it stood before this line -- prefsShouldAdoptFromFile is the actual policy (see the header);
    // parseInto's own emplace enacts it unconditionally (a no-op for a key already present is
    // exactly "do not adopt"), so this lambda's only job is counting for the log line below.
    usize adopted = 0;
    parseInto(text, g_values, [&](const std::string& key, const auto& values) {
        if (prefsShouldAdoptFromFile(values.find(key) != values.end())) ++adopted;
    });

    AVER_INFO("[Prefs] {} became readable again; adopted {} setting(s) this session had not already "
              "changed, keeping every edit this session made", g_path, adopted);
    g_readOnly = false;
    return true;
}

} // namespace

// Reads a float setting, or `fallback` when it is absent or not a number.
f32 prefFloat(std::string_view key, f32 fallback) {
    ensureLoaded();
    const auto it = g_values.find(key);
    if (it == g_values.end()) return fallback;

    // from_chars, not atof: locale-independent, so a comma-locale machine reads back what it wrote.
    f32 v = fallback;
    const char* first = it->second.data();
    const char* last  = first + it->second.size();
    const auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc{}) return fallback;
    return v;
}

// Stores a float, shortest round-trip.
void setPrefFloat(std::string_view key, f32 value) {
    ensureLoaded();
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof buf, value);
    if (r.ec != std::errc{}) return;
    setPrefString(key, std::string_view(buf, static_cast<usize>(r.ptr - buf)));
}

// Reads a bool setting. Accepts "true"/"false" and "1"/"0".
bool prefBool(std::string_view key, bool fallback) {
    ensureLoaded();
    const auto it = g_values.find(key);
    if (it == g_values.end()) return fallback;
    if (it->second == "true"  || it->second == "1") return true;
    if (it->second == "false" || it->second == "0") return false;
    return fallback;
}

// Stores a bool as "true"/"false".
void setPrefBool(std::string_view key, bool value) {
    setPrefString(key, value ? "true" : "false");
}

// Reads an int setting, or `fallback` when it is absent or not a number.
i32 prefInt(std::string_view key, i32 fallback) {
    ensureLoaded();
    const auto it = g_values.find(key);
    if (it == g_values.end()) return fallback;
    i32 v = fallback;
    const char* first = it->second.data();
    const auto r = std::from_chars(first, first + it->second.size(), v);
    if (r.ec != std::errc{}) return fallback;
    return v;
}

// Stores an int.
void setPrefInt(std::string_view key, i32 value) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, value);
    if (r.ec != std::errc{}) return;
    setPrefString(key, std::string_view(buf, static_cast<usize>(r.ptr - buf)));
}

// Reads a string setting, or `fallback` when it is absent.
std::string prefString(std::string_view key, std::string_view fallback) {
    ensureLoaded();
    const auto it = g_values.find(key);
    return it == g_values.end() ? std::string(fallback) : it->second;
}

// Stores a string. Refuses a multi-line value, which the line format cannot hold.
void setPrefString(std::string_view key, std::string_view value) {
    ensureLoaded();
    if (value.find('\n') != std::string_view::npos ||
        value.find('\r') != std::string_view::npos) {
        AVER_WARN("[Prefs] refusing to store a multi-line value for '{}'", key);
        return;
    }
    const auto it = g_values.find(key);
    if (it != g_values.end() && it->second == value) return;
    g_values[std::string(key)] = std::string(value);
    g_dirty = true;
}

// Writes the file if anything changed since the last write.
// Pure, and deliberately so: the branch it encodes is otherwise unreachable from a test, because
// ensureLoaded latches g_loaded once per process and there is no reset hook. Extracting the decision
// is what lets EditorPrefsTest assert all three cases directly. See the header for the reasoning.
bool prefsShouldRefuseWrite(bool readSucceeded, bool fileExists) {
    return !readSucceeded && fileExists;
}

bool editorPrefsReadOnly() { return g_readOnly; }

// See the header: session values win, full stop.
bool prefsShouldAdoptFromFile(bool sessionAlreadyHasKey) {
    return !sessionAlreadyHasKey;
}

void flushEditorPrefs() {
    if (!g_dirty) return;
    if (g_path.empty()) { g_dirty = false; return; }   // nowhere to write; stop asking
    // READ-ONLY NO LONGER BEATS DIRTY UNCONDITIONALLY. It used to: the store was empty only because
    // the read failed, so writing it would replace a good file with a header and nothing else. That
    // guarantee still holds -- tryRecoverReadOnly re-reads before anything below is allowed to write
    // a byte -- but now the read gets ANOTHER chance every time there is something worth persisting,
    // instead of the file being given up on for the rest of the session after one failed attempt at
    // load. See tryRecoverReadOnly's comment for the merge that keeps this session's edits.
    if (g_readOnly && !tryRecoverReadOnly()) {
        // STILL UNREADABLE: stay dirty and try again at the next flush -- but SAY SO, ONCE, AS AN ERROR.
        // This used to return silently, so a file that stayed unreadable for a whole session lost every
        // edit with nothing on screen: the load's AVER_WARN was the only trace, and pushFromLog never
        // turns Warn into a toast. Found while chasing an editor.ini that stopped moving across several
        // sessions with the code reading correct at every layer. reportWriteFailure dedups on (op, code),
        // so the 0.25s autosave timer cannot turn this into a stream.
        AtomicWriteError unreadable;
        unreadable.op        = "re-read before save: the existing file is still unreadable, so nothing was written";
        unreadable.errorCode = 0;
        reportWriteFailure(g_path, unreadable);
        return;
    }

    const usize slash = g_path.find_last_of("/\\");
    if (slash != std::string::npos) createDirectories(g_path.substr(0, slash));

    std::string out =
        "# Aver Engine editor preferences.\n"
        "# UI geometry and per-user toggles. Cache-grade: safe to delete, and deleting it only\n"
        "# resets the editor's appearance. Nothing about a PROJECT is stored here.\n";
    for (const auto& [k, v] : g_values) { out += k; out += '='; out += v; out += '\n'; }

    // TEMP-THEN-RENAME, not a truncate-and-write. This is now called on a timer rather than
    // once at shutdown, so the window in which a crash or a kill can catch a half-written file
    // is no longer vanishing -- and the failure mode of the plain writer is an editor.ini
    // truncated to nothing, which loses every preference rather than the last one.
    //
    // AtomicFallback::DirectWrite: editor.ini says of ITSELF, in the header this loop just wrote,
    // "cache-grade: safe to delete, and deleting it only resets the editor's appearance". A write
    // that cannot complete the crash-safe swap (something else has it open) falling back to a plain
    // write is exactly the trade that comment already signs off on -- the crash-mid-write risk the
    // swap guards against is real but small next to a whole session's preferences never reaching
    // disk at all. See writeFileBytesAtomic's own doc for the general argument and who must NOT do
    // this (a level, a material, a save -- content actually worth crash-protecting).
    AtomicWriteError err;
    if (!writeFileTextAtomic(g_path, out, AtomicFallback::DirectWrite, &err)) {
        reportWriteFailure(g_path, err);
        return;                     // KEEP g_dirty SET, so the next flush retries
    }
    // CLEARED ONLY ON SUCCESS. It used to be cleared before the write was attempted, so a failed
    // write dropped the dirty bit and the values stayed unsaved until something else changed --
    // silently, and for the rest of the session on a directory that could not be written at all.
    g_dirty = false;
}

// The settings file's path. Empty before the first access.
const std::string& editorPrefsPath() { return g_path; }

} // namespace aver::editor
