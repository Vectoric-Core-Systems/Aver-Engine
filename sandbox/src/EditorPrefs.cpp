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

// Reads editor.ini once. A failed read leaves an empty store and does not retry.
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
    if (!readFileText(g_path, text)) return;

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
void flushEditorPrefs() {
    if (!g_dirty) return;
    g_dirty = false;
    if (g_path.empty()) return;

    const usize slash = g_path.find_last_of("/\\");
    if (slash != std::string::npos) createDirectories(g_path.substr(0, slash));

    std::string out =
        "# Aver Engine editor preferences.\n"
        "# UI geometry and per-user toggles. Cache-grade: safe to delete, and deleting it only\n"
        "# resets the editor's appearance. Nothing about a PROJECT is stored here.\n";
    for (const auto& [k, v] : g_values) { out += k; out += '='; out += v; out += '\n'; }

    if (!writeFileText(g_path, out))
        AVER_WARN("[Prefs] could not write {}", g_path);
}

// The settings file's path. Empty before the first access.
const std::string& editorPrefsPath() { return g_path; }

} // namespace aver::editor
