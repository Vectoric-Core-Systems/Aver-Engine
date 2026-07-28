#include "EditorPrefs.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <map>
#include <string>

namespace aver::editor {
namespace {

// std::map rather than unordered_map so the file writes in a STABLE ORDER. A settings file whose
// lines shuffle between runs is one nobody can diff, and diffing it is most of what makes a
// plain-text format worth having.
std::map<std::string, std::string, std::less<>> g_values;
std::string g_path;
bool g_loaded = false;
bool g_dirty  = false;

void ensureLoaded() {
    if (g_loaded) return;
    g_loaded = true;   // set FIRST: a failed read must not retry on every accessor

    // userDataDir is documented as the home for per-user, per-machine state that is neither project
    // content nor engine content -- which is exactly this.
    const std::string dir = userDataDir();
    if (dir.empty()) {
        AVER_WARN("[Prefs] no user data directory; editor preferences will not persist this session");
        return;
    }
    g_path = dir + "/editor.ini";

    std::string text;
    if (!readFileText(g_path, text)) return;   // absent on a first run, which is not a problem

    usize line = 0;
    while (line < text.size()) {
        usize end = text.find('\n', line);
        if (end == std::string::npos) end = text.size();
        std::string_view row(text.data() + line, end - line);
        line = end + 1;
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        if (row.empty() || row.front() == '#') continue;

        const usize eq = row.find('=');
        if (eq == std::string_view::npos) continue;   // a line without one is not a setting
        g_values.emplace(std::string(row.substr(0, eq)), std::string(row.substr(eq + 1)));
    }
    AVER_INFO("[Prefs] {} setting(s) from {}", g_values.size(), g_path);
}

} // namespace

f32 prefFloat(std::string_view key, f32 fallback) {
    ensureLoaded();
    const auto it = g_values.find(key);
    if (it == g_values.end()) return fallback;

    // from_chars, not atof: it is locale-INDEPENDENT. strtof/atof read the decimal point from the C
    // locale, so a machine set to a comma locale would write "230.5" and read back 230 -- a settings
    // file that silently degrades on somebody else's machine and nowhere else.
    f32 v = fallback;
    const char* first = it->second.data();
    const char* last  = first + it->second.size();
    const auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc{}) return fallback;   // a value that is not a number is a value we ignore
    return v;
}

void setPrefFloat(std::string_view key, f32 value) {
    ensureLoaded();
    // to_chars for the same locale reason as from_chars, and shortest round-trip so a value written
    // and read back is bit-identical rather than nearly so.
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof buf, value);
    if (r.ec != std::errc{}) return;
    setPrefString(key, std::string_view(buf, static_cast<usize>(r.ptr - buf)));
}

// Bools are "true"/"false" rather than 1/0. The file is meant to be read and edited by a person, and
// a column of ones and zeros is a column nobody can interpret without the source open.
bool prefBool(std::string_view key, bool fallback) {
    ensureLoaded();
    const auto it = g_values.find(key);
    if (it == g_values.end()) return fallback;
    if (it->second == "true"  || it->second == "1") return true;
    if (it->second == "false" || it->second == "0") return false;
    return fallback;
}

void setPrefBool(std::string_view key, bool value) {
    setPrefString(key, value ? "true" : "false");
}

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

void setPrefInt(std::string_view key, i32 value) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, value);
    if (r.ec != std::errc{}) return;
    setPrefString(key, std::string_view(buf, static_cast<usize>(r.ptr - buf)));
}

std::string prefString(std::string_view key, std::string_view fallback) {
    ensureLoaded();
    const auto it = g_values.find(key);
    return it == g_values.end() ? std::string(fallback) : it->second;
}

void setPrefString(std::string_view key, std::string_view value) {
    ensureLoaded();
    // A newline would split the entry across two lines and silently lose the tail. Refused rather
    // than escaped: see the header for why escaping is not worth a parser here.
    if (value.find('\n') != std::string_view::npos ||
        value.find('\r') != std::string_view::npos) {
        AVER_WARN("[Prefs] refusing to store a multi-line value for '{}'", key);
        return;
    }
    const auto it = g_values.find(key);
    if (it != g_values.end() && it->second == value) return;   // unchanged: not a reason to rewrite
    g_values[std::string(key)] = std::string(value);
    g_dirty = true;
}

void flushEditorPrefs() {
    if (!g_dirty) return;
    g_dirty = false;
    if (g_path.empty()) return;

    // The directory may not exist on a first run; userDataDir names it but does not promise it.
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

const std::string& editorPrefsPath() { return g_path; }

} // namespace aver::editor
