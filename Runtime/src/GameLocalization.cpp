#include "aver/game/GameLocalization.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP

#include "aver/core/Log.hpp"
#include "aver/formats/OcStrings.hpp"
#include "aver/platform/FileSystem.hpp"

#ifndef AVER_MODULE_FRAMEWORK
#  define AVER_MODULE_FRAMEWORK 0
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_loc_abi.h"
#  include "aver/framework/framework_table_abi.h"
#endif
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <algorithm>

namespace aver::game {

namespace {

std::string languageOf(const std::string& tag) { return tag.substr(0, tag.find('-')); }

// "Hans" in "zh-Hans-CN": the 4-letter subtag; "CN": the trailing 2-letter / 3-digit subtag.
std::string scriptOf(const std::string& tag) {
    const usize a = tag.find('-');
    if (a == std::string::npos) return {};
    const usize b = tag.find('-', a + 1);
    const std::string s = tag.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    return s.size() == 4 ? s : std::string();
}
std::string regionOf(const std::string& tag) {
    const usize d = tag.rfind('-');
    if (d == std::string::npos) return {};
    const std::string r = tag.substr(d + 1);
    return r.size() == 4 ? std::string() : r;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    usize a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}

} // namespace

std::string bestCultureMatch(const std::vector<std::string>& available, std::string_view wantedIn) {
    const std::string wanted = fmt::normalizeCulture(wantedIn);
    if (wanted.empty()) return {};
    std::vector<std::string> norm;
    for (const std::string& a : available) {
        std::string n = fmt::normalizeCulture(a);
        if (!n.empty()) norm.push_back(std::move(n));
    }
    for (const std::string& n : norm) if (n == wanted) return n;

    const std::string lang = languageOf(wanted), script = scriptOf(wanted), region = regionOf(wanted);
    std::string best;
    int bestScore = -1;
    for (const std::string& n : norm) {
        if (languageOf(n) != lang) continue;
        int score = 0;
        if (n == lang) score = 1;                                  // the bare language is neutral: better than a stranger's region
        else {
            if (!script.empty() && scriptOf(n) == script) score += 2;
            if (!region.empty() && regionOf(n) == region) score += 2;
        }
        if (score > bestScore) { best = n; bestScore = score; }    // first wins a tie; `available` is sorted by the store
    }
    return best;
}

std::string systemUiCulture() {
#if defined(_WIN32)
    wchar_t w[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH) <= 0) return {};
    char u[LOCALE_NAME_MAX_LENGTH * 3] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, u, static_cast<int>(sizeof(u)), nullptr, nullptr);
    if (n <= 1) return {};
    return fmt::normalizeCulture(u);
#else
    return {};
#endif
}

std::string savedCulture(const std::string& settingsPath) {
    std::string text;
    if (settingsPath.empty() || !readFileText(settingsPath, text)) return {};
    usize i = 0;
    while (i <= text.size()) {
        const usize e = text.find('\n', i);
        const std::string line = trimmed(text.substr(i, (e == std::string::npos ? text.size() : e) - i));
        i = e == std::string::npos ? text.size() + 1 : e + 1;
        if (line.empty() || line[0] == '#') continue;
        const usize eq = line.find('=');
        if (eq == std::string::npos || trimmed(line.substr(0, eq)) != "locale.culture") continue;
        return fmt::normalizeCulture(trimmed(line.substr(eq + 1)));
    }
    return {};
}

std::string initLocalization(const LocalizationInit& in) {
#if AVER_MODULE_FRAMEWORK
    if (!in.project) return "en";
    const std::string contentDir = in.project->contentDir();
    aver_fw_content_set_root(contentDir.c_str());
    const std::string locDir = contentDir + AVER_PATH_SEP "Localization";
    const int32_t found = aver_fw_loc_init(locDir.c_str(), in.project->localeDefault.c_str());

    std::vector<std::string> available;
    for (int32_t i = 0; i < aver_fw_loc_culture_count(); ++i) available.emplace_back(aver_fw_loc_culture_at(i));

    std::string chosen, why;
    if (const std::string o = fmt::normalizeCulture(in.cultureOverride); !o.empty()) { chosen = o; why = "override"; }
    if (chosen.empty() && in.readSavedCulture) {
        const std::string s = savedCulture(in.settingsPath);
        if (!s.empty()) { chosen = s; why = "saved setting"; }
    }
    if (chosen.empty()) {
        const std::string m = bestCultureMatch(available, systemUiCulture());
        if (!m.empty()) { chosen = m; why = "system language"; }
    }
    if (chosen.empty()) { chosen = aver_fw_loc_default_culture(); why = "project default"; }
    // Only a change posts "loc.culture_changed"; the default is already active after init.
    if (chosen != aver_fw_loc_culture()) aver_fw_loc_set_culture(chosen.c_str());

    if (found < 0) AVER_INFO("[Loc] no Localization folder in '{}'; text shows its inline defaults", contentDir);
    else AVER_INFO("[Loc] {} culture(s) in '{}', default '{}', active '{}' ({})", found, locDir,
                   aver_fw_loc_default_culture(), aver_fw_loc_culture(), why);
    return aver_fw_loc_culture();
#else
    (void)in;
    return "en";
#endif
}

usize onContentFilesChanged(const std::string& contentRoot, const std::vector<FileEvent>& events) {
#if AVER_MODULE_FRAMEWORK
    usize touched = 0;
    const auto one = [&](const std::string& rel) {
        if (rel.empty()) return;
        const std::string abs = contentRoot + "/" + rel;
        const int32_t a = aver_fw_loc_reload_path(abs.c_str());
        const int32_t b = aver_fw_table_reload_path(abs.c_str());
        if (a > 0 || b > 0) ++touched;
    };
    for (const FileEvent& e : events) {
        one(e.path);
        if (e.kind == FileChange::Renamed) one(e.oldPath);
    }
    return touched;
#else
    (void)contentRoot; (void)events;
    return 0;
#endif
}

} // namespace aver::game
