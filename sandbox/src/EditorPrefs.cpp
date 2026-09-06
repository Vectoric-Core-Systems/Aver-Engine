// Editor preference store: reads and writes the `key=value` editor.ini under the user data dir.
#include "EditorPrefs.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <map>
#include <string>

namespace aver::editor {
namespace {

// std::map, not unordered_map: the file must write in a stable, diffable order.
std::map<std::string, std::string, std::less<>> g_values;
std::string g_path;
bool g_loaded = false;
bool g_dirty  = false;
// Latched when the file existed and could not be read. See prefsShouldRefuseWrite in the header.
bool g_readOnly = false;

// Reads editor.ini once. A failed read leaves an empty store and does not retry.
//
// THE ONE-SHOT IS CORRECT; THE INFERENCE FROM IT WAS NOT. Every pref accessor calls this, and
// loadEditorPreferences plus keybinds_.loadFromPrefs() make dozens of those calls, so retrying a
// failing read on each one would turn a single stat into a per-access one. What was wrong was
// concluding that "we tried to load" also means "it is safe to overwrite".
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
    if (!readFileText(g_path, text)) {
        // A MISSING FILE IS THE ORDINARY FIRST RUN. An UNREADABLE one is not, and the two arrive
        // here as the same `false` -- so existence is asked separately, and a session that could
        // not read an existing file never writes over it.
        if (prefsShouldRefuseWrite(false, fileExists(g_path))) {
            g_readOnly = true;
            AVER_WARN("[Prefs] {} exists but could not be read; preferences are READ-ONLY for this "
                      "session rather than being overwritten with defaults", g_path);
        }
        return;
    }

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
        g_values.emplace(std::string(row.substr(0, eq)), std::string(row.substr(eq + 1)));
    }
    AVER_INFO("[Prefs] {} setting(s) from {}", g_values.size(), g_path);
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

void flushEditorPrefs() {
    if (!g_dirty) return;
    if (g_path.empty()) { g_dirty = false; return; }   // nowhere to write; stop asking
    // READ-ONLY BEATS DIRTY. The store is empty only because the read failed, so writing it would
    // replace a good file with a header and nothing else -- and writeFileTextAtomic below makes
    // that replacement complete and irreversible. Drop the dirty bit for the same reason the
    // empty-path bail does: there is nothing this session can do about it, so stop asking.
    if (g_readOnly) { g_dirty = false; return; }

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
    if (!writeFileTextAtomic(g_path, out)) {
        AVER_WARN("[Prefs] could not write {}", g_path);
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
