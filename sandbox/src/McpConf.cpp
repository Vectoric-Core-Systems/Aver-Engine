// Composition-root reader for the optional mcp.conf at the engine root. See McpConf.hpp for the
// contract and mcp.conf.example at the repo root for the file format.
#include "McpConf.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>

namespace aver::editor {

bool readMcpConfPort(const std::string& engineRoot, std::string_view key, u16* outPort) {
    if (engineRoot.empty()) return false;   // no engine tree beside the executable: same as no file

    const std::string path = engineRoot + "/mcp.conf";
    std::string text;
    if (!readFileText(path, text)) return false;   // absent is the normal case; not even a log line

    usize line = 0;
    while (line < text.size()) {
        usize end = text.find('\n', line);
        if (end == std::string::npos) end = text.size();
        std::string_view row(text.data() + line, end - line);
        line = end + 1;
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        if (row.empty() || row.front() == '#') continue;

        const usize eq = row.find('=');
        if (eq == std::string_view::npos) continue;   // a line with no '=': skipped, not fatal

        std::string_view k = row.substr(0, eq);
        while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.remove_prefix(1);
        while (!k.empty() && (k.back()  == ' ' || k.back()  == '\t')) k.remove_suffix(1);
        if (k != key) continue;

        std::string_view v = row.substr(eq + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back()  == ' ' || v.back()  == '\t')) v.remove_suffix(1);

        int parsed = 0;
        const auto r = std::from_chars(v.data(), v.data() + v.size(), parsed);
        // r.ptr must land exactly on the end of the (trimmed) value: "8080x" is a typo, not a port,
        // and from_chars alone would silently accept its leading digits.
        if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || parsed < 1 || parsed > 65535) {
            AVER_WARN("[McpConf] '{}' = '{}' in {} is not a usable port (1-65535); ignoring, "
                      "falling back to the built-in default", key, v, path);
            return false;   // the key WAS present -- this is the one case worth a log line
        }
        *outPort = static_cast<u16>(parsed);
        return true;        // first occurrence wins, mirroring editor.ini's own duplicate-key rule
    }
    return false;   // key not present in an otherwise-fine (or empty) file: normal, silent
}

} // namespace aver::editor
