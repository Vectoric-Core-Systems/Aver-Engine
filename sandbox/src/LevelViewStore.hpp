// Where the editor camera was when each level was last LEFT -- Unreal's per-map viewport memory
// (EditorPerProjectUserSettings.ini), kept in <project>/Saved/LevelViews.ini.
//
// NOT the level's own CAMERA record (OcWorld.hpp). That one is written only by a level SAVE, so a
// user who flies somewhere and then switches level or closes the editor without saving lost the
// view. This store is written on every way out of a level whether or not it was saved, and it is
// PER USER: Saved/ is local state, so one person's viewpoint never rewrites a level file other
// people share.
//
// A PURE HEADER for ViewportPick.hpp's reason: no SandboxApp, no ImGui, no filesystem calls --
// text in, text out -- so the key and the file grammar can be tested without an editor.
#pragma once
#include "aver/core/Types.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

namespace aver::editor {

// One level's remembered view, in the units the editor camera uses: centimetres and degrees.
struct LevelView {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;
    f32 yawDeg = 0.0f, pitchDeg = 0.0f;
    f32 speed = 0.0f;   // fly speed, cm/s; 0 = unstated, which leaves the user's own speed alone
};

using LevelViewMap = std::map<std::string, LevelView, std::less<>>;

// The key a level is remembered under: its path relative to the project directory, forward
// slashes, lower case. Relative so the entry survives the project moving on disk; lower case
// because Windows paths are case-insensitive and the same level reached as "Content\Maps" and
// "content\maps" must not get two entries. A level outside the project keeps its absolute path.
inline std::string levelViewKey(const std::string& levelPath, const std::string& projectDir) {
    namespace fs = std::filesystem;
    if (levelPath.empty()) return {};
    auto lowered = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    fs::path p = fs::path(lowered(levelPath)).lexically_normal();
    if (!projectDir.empty()) {
        const fs::path rel = p.lexically_relative(fs::path(lowered(projectDir)).lexically_normal());
        // Empty means another drive, "." the project directory itself, ".." somewhere outside it.
        if (!rel.empty() && *rel.begin() != "." && *rel.begin() != "..") p = rel;
    }
    return p.generic_string();
}

// `<key>=<x> <y> <z> <yawDeg> <pitchDeg> <speed>`, one level per line. Split at the LAST '=':
// the numbers never contain one and a path can. Blank lines, `#` comments and malformed rows are
// skipped, not fatal -- losing one remembered view is better than losing the file.
inline LevelViewMap parseLevelViews(std::string_view text) {
    LevelViewMap out;
    usize pos = 0;
    while (pos < text.size()) {
        usize end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view row = text.substr(pos, end - pos);
        pos = end + 1;
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        if (row.empty() || row.front() == '#') continue;
        const usize eq = row.rfind('=');
        if (eq == std::string_view::npos || eq == 0) continue;

        f32 v[6] = {};
        std::string_view nums = row.substr(eq + 1);
        int got = 0;
        while (got < 6) {
            while (!nums.empty() && nums.front() == ' ') nums.remove_prefix(1);
            if (nums.empty()) break;
            // from_chars, not atof: locale-independent, so a comma-locale machine reads what it wrote.
            const auto r = std::from_chars(nums.data(), nums.data() + nums.size(), v[got]);
            if (r.ec != std::errc{}) break;
            nums.remove_prefix(static_cast<usize>(r.ptr - nums.data()));
            ++got;
        }
        if (got < 5) continue;   // position and angles are required; the speed may be missing
        // from_chars accepts "nan" and "inf", and a camera placed at either never recovers.
        bool finite = true;
        for (int k = 0; k < got; ++k) finite = finite && std::isfinite(v[k]);
        if (!finite) continue;
        out[std::string(row.substr(0, eq))] = LevelView{v[0], v[1], v[2], v[3], v[4], got > 5 ? v[5] : 0.0f};
    }
    return out;
}

inline std::string formatLevelViews(const LevelViewMap& views) {
    std::string out =
        "# Aver Engine: where the editor camera was when each level was last left.\n"
        "# Per user, safe to delete -- a level with no entry opens at its saved CAMERA record.\n";
    char buf[32];
    auto num = [&](f32 f) {
        // Shortest round-trip, so a view read back is exactly the view written.
        const auto r = std::to_chars(buf, buf + sizeof buf, f);
        out.append(buf, r.ec == std::errc{} ? static_cast<usize>(r.ptr - buf) : 0);
    };
    for (const auto& [key, v] : views) {
        out += key; out += '=';
        num(v.x); out += ' '; num(v.y); out += ' '; num(v.z); out += ' ';
        num(v.yawDeg); out += ' '; num(v.pitchDeg); out += ' '; num(v.speed);
        out += '\n';
    }
    return out;
}

} // namespace aver::editor
