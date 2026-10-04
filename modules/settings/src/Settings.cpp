// The durable key/value store a shipped game reads and writes. See settings_abi.h for why this is
// not EditorPrefs and not part of a save.

#include "aver/settings/settings_abi.h"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace {

// A std::map, NOT an unordered one, and for the same reason EditorPrefs.cpp:14 gives: the file must
// write in a stable, diffable order. A settings file that reshuffles itself on every flush is one
// nobody can put under version control or read a diff of.
std::map<std::string, std::string> g_values;
std::string g_path;
bool g_dirty = false;
std::string g_scratch;   // backs the pointer aver_settings_get_str / _default_path return

// Trims ASCII spaces and tabs from both ends. Deliberately not a general whitespace trim: a value
// ending in a non-breaking space is a value, and quietly eating it would be worse than keeping it.
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

void loadFrom(const std::string& path) {
    g_values.clear();
    g_dirty = false;
    std::string text;
    // A MISSING FILE IS NOT AN ERROR. A first run has no settings; that is the ordinary case, not a
    // failure to report.
    if (!aver::readFileText(path, text)) return;

    std::size_t i = 0;
    while (i <= text.size()) {
        const std::size_t e = text.find('\n', i);
        const std::string line = trim(text.substr(i, (e == std::string::npos ? text.size() : e) - i));
        i = (e == std::string::npos) ? text.size() + 1 : e + 1;
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq));
        if (k.empty()) continue;
        // EVERY KEY IS KEPT, including ones this build has never heard of. A newer build writing a
        // setting an older one then loads and flushes must not delete it -- the same
        // unknown-data rule OcProject.cpp's owned-key list follows, reached here for free because
        // the store IS the file rather than a projection of a struct.
        g_values[k] = trim(line.substr(eq + 1));
    }
}

const std::string* find(const char* key) {
    if (!key || !*key) return nullptr;
    const auto it = g_values.find(key);
    return it == g_values.end() ? nullptr : &it->second;
}

bool put(const char* key, std::string v) {
    if (!key || !*key) return false;
    auto it = g_values.find(key);
    // Unchanged is not dirty. A settings screen that writes every slider every frame would
    // otherwise rewrite the file on every flush for no reason.
    if (it != g_values.end() && it->second == v) return true;
    g_values[key] = std::move(v);
    g_dirty = true;
    return true;
}

} // namespace

extern "C" {

int32_t aver_settings_open(const char* utf8Path) {
    if (!utf8Path || !*utf8Path) return 0;
    g_path = utf8Path;
    loadFrom(g_path);
    return 1;
}

const char* aver_settings_default_path(void) {
    g_scratch = aver::userDataDir();
    if (!g_scratch.empty() && g_scratch.back() != '/' && g_scratch.back() != '\\') g_scratch += '/';
    g_scratch += "settings.ini";
    return g_scratch.c_str();
}

int32_t aver_settings_flush(void) {
    if (g_path.empty()) return 0;
    if (!g_dirty) return 1;   // already current; not an error, and not a write

    std::string text =
        "# Aver Engine settings. Written by the game; safe to edit by hand.\n"
        "# One key=value per line. Lines starting with # are comments and are NOT preserved --\n"
        "# unknown KEYS are, so a setting a newer build added survives an older build saving over it.\n";
    for (const auto& [k, v] : g_values) { text += k; text += '='; text += v; text += '\n'; }

    std::error_code ec;
    const std::filesystem::path p(g_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    // TEMP THEN RENAME, for the reason .ocsave does it: settings are small and rewritten often, and
    // half a settings file is a game that will not start. aver::renameFile is MoveFileExW with
    // MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH.
    const std::string tmp = g_path + ".tmp";
    if (!aver::writeFileText(tmp, text)) {
        AVER_WARN("[Settings] could not write {}", tmp);
        return 0;
    }
    if (!aver::renameFile(tmp, g_path)) {
        aver::deleteFile(tmp);
        AVER_WARN("[Settings] could not replace {} (the previous settings are intact)", g_path);
        return 0;
    }
    g_dirty = false;
    return 1;
}

float aver_settings_get_f32(const char* key, float fallback) {
    const std::string* v = find(key);
    if (!v || v->empty()) return fallback;
    // strtof, not stof: a value that does not parse must return the FALLBACK, and stof throws.
    // A hand-edited `volume=loud` should leave the volume alone, not silence the game.
    char* end = nullptr;
    const float out = std::strtof(v->c_str(), &end);
    return (end && *end == '\0') ? out : fallback;
}

int32_t aver_settings_get_i32(const char* key, int32_t fallback) {
    const std::string* v = find(key);
    if (!v || v->empty()) return fallback;
    char* end = nullptr;
    const long out = std::strtol(v->c_str(), &end, 10);
    return (end && *end == '\0') ? static_cast<int32_t>(out) : fallback;
}

int32_t aver_settings_get_bool(const char* key, int32_t fallback) {
    const std::string* v = find(key);
    if (!v || v->empty()) return fallback;
    // Written as true/false, but READ generously: a file people edit by hand will contain 1, 0,
    // yes and no, and refusing those helps nobody.
    if (*v == "true"  || *v == "1" || *v == "yes" || *v == "on")  return 1;
    if (*v == "false" || *v == "0" || *v == "no"  || *v == "off") return 0;
    return fallback;
}

const char* aver_settings_get_str(const char* key, const char* fallback) {
    const std::string* v = find(key);
    if (!v) return fallback ? fallback : "";
    g_scratch = *v;
    return g_scratch.c_str();
}

int32_t aver_settings_set_f32(const char* key, float value) {
    // %g-style shortest round-trip is not available without <format>; to_string gives six decimals,
    // which is more than a settings value ever needs and is stable across runs.
    return put(key, std::to_string(value)) ? 1 : 0;
}
int32_t aver_settings_set_i32(const char* key, int32_t value) {
    return put(key, std::to_string(value)) ? 1 : 0;
}
int32_t aver_settings_set_bool(const char* key, int32_t value) {
    return put(key, value ? "true" : "false") ? 1 : 0;
}
int32_t aver_settings_set_str(const char* key, const char* value) {
    const std::string v = value ? value : "";
    // REFUSED, NOT SANITISED. The format is one key=value per line with no escaping, so a value
    // carrying a newline would silently become a second key -- and a caller that gets `1` back has
    // been told the write worked. EditorPrefs.cpp:122-127 refuses the same thing for the same
    // reason.
    if (v.find('\n') != std::string::npos || v.find('\r') != std::string::npos) {
        AVER_WARN("[Settings] refusing to store a multi-line value for '{}' -- the format has no "
                  "escaping and it would read back as a second key", key ? key : "");
        return 0;
    }
    return put(key, v) ? 1 : 0;
}

int32_t aver_settings_has(const char* key) { return find(key) != nullptr ? 1 : 0; }

int32_t aver_settings_remove(const char* key) {
    if (!key || !*key) return 0;
    if (g_values.erase(key) > 0) g_dirty = true;
    return 1;   // gone afterwards either way, which is what every caller means
}

int32_t aver_settings_count(void) { return static_cast<int32_t>(g_values.size()); }

} // extern "C"
