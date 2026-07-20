#include "aver/formats/OcProject.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/core/Version.hpp"

#include <filesystem>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// The rest of the line after the key, comment-stripped. AUTHOR and NAME are prose, so they must
// not be split on whitespace the way a numeric row is.
std::string_view restOfLine(std::string_view line, std::string_view key) {
    std::string_view r = line.substr(key.size());
    return trim(r);
}

} // namespace

bool parseOcproject(std::string_view text, ProjectDesc& out, std::string* err) {
    const std::string dir = out.dir, manifest = out.manifestPath; // preserved across the reset
    out = ProjectDesc{};
    out.dir = dir;
    out.manifestPath = manifest;

    // A manifest is the one .oc* file people hand-author in a text editor, and Notepad and
    // PowerShell's `Out-File -Encoding utf8` both prepend a UTF-8 BOM. Left in place it becomes
    // part of the first token, so OCPROJECT stops matching and the file reads as "not a project".
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF &&
        static_cast<u8>(text[1]) == 0xBB && static_cast<u8>(text[2]) == 0xBF)
        text = text.substr(3);

    bool sawHeader = false;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string_view key = t[0];

        if (equalsCI(key, "OCPROJECT")) {
            sawHeader = true;
            if (t.size() > 1) out.version = parseI32(t[1], 1);
        } else if (equalsCI(key, "NAME")) {
            out.name = std::string(restOfLine(line, key));
        } else if (equalsCI(key, "ENGINE")) {
            if (t.size() > 1) out.engineName = std::string(t[1]);
            if (t.size() > 2) out.engineMinVersion = std::string(t[2]);
        } else if (equalsCI(key, "CONTENT")) {
            if (t.size() > 1) out.contentRoot = std::string(t[1]);
        } else if (equalsCI(key, "STARTMAP")) {
            if (t.size() > 1) out.startMap = std::string(t[1]);
        } else if (equalsCI(key, "AUTHOR")) {
            out.author = std::string(restOfLine(line, key));
        }
        // Everything else is ignored on purpose — the format is forward-compatible, so a key this
        // build has never heard of (dependencies, plugins, cook targets) must not fail the load.
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocproject: no OCPROJECT header line";
        return false;
    }
    if (out.name.empty()) {
        if (err) *err = "missing NAME";
        return false;
    }
    return true;
}

bool loadOcproject(const std::string& path, ProjectDesc& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "cannot read file: " + path;
        return false;
    }

    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    out = ProjectDesc{};
    out.manifestPath = ec ? path : abs.string();
    out.dir = std::filesystem::path(out.manifestPath).parent_path().string();

    if (!parseOcproject(text, out, err)) return false;

    // ENGINE is the compatibility gate. A project asking for a build we are not is a refusal, not
    // a warning: loading it anyway would silently give the author an engine missing whatever they
    // depended on, and the failure would surface later as broken content.
    if (!out.engineName.empty() && !equalsCI(out.engineName, kEngineName)) {
        if (err)
            *err = "project targets engine '" + out.engineName + "', this is " + std::string(kEngineName);
        return false;
    }
    if (!out.engineMinVersion.empty() && compareVersions(out.engineMinVersion, kEngineVersion) > 0) {
        if (err)
            *err = "project needs " + std::string(kEngineName) + " " + out.engineMinVersion +
                   " or newer; this build is " + std::string(kEngineVersion);
        return false;
    }
    return true;
}

} // namespace aver::fmt
