#include "AssetC.hpp"

namespace assetc {

// ---- small helpers, ported from ConvertTool.cpp (see that file for the reasoning) ----

std::string stemOf(const std::string& path) {
    usize a = path.find_last_of("/\\");
    a = (a == std::string::npos) ? 0 : a + 1;
    const usize b = path.find_last_of('.');
    return path.substr(a, (b == std::string::npos || b < a) ? std::string::npos : b - a);
}

std::string extOf(const std::string& path) {
    const usize b = path.find_last_of('.');
    if (b == std::string::npos) return {};
    std::string e = path.substr(b);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// A name safe to hang on a file, so an unnamed or oddly-named source node cannot escape into a path.
std::string safe(const std::string& in, const std::string& fallback) {
    std::string out;
    for (char c : in)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out.push_back(c);
    return out.empty() ? fallback : out;
}

// ---- the JSON-Lines result contract ----

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void emit(const std::string& json) {
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);   // a caller reading line-by-line must see this before the process can crash
}

void emitArtifact(const std::string& input, const std::string& output, const char* kind,
                  bool ok, bool verified, const std::string& error, RunStats& stats,
                  const std::string& extra) {
    std::string j = "{\"schemaVersion\":1,\"input\":\"" + jsonEscape(input) + "\",\"output\":\""
        + jsonEscape(output) + "\",\"kind\":\"" + kind + "\",\"ok\":" + (ok ? "true" : "false")
        + ",\"verified\":" + (verified ? "true" : "false");
    if (!error.empty()) j += ",\"error\":\"" + jsonEscape(error) + "\"";
    if (!extra.empty()) j += "," + extra;
    j += "}";
    emit(j);
    if (ok && verified) ++stats.written; else ++stats.failed;
}

void emitSummary(const std::string& input, const RunStats& stats, int exitCode) {
    emit("{\"schemaVersion\":1,\"summary\":true,\"input\":\"" + jsonEscape(input)
        + "\",\"itemsWritten\":" + std::to_string(stats.written)
        + ",\"itemsFailed\":" + std::to_string(stats.failed)
        + ",\"exitCode\":" + std::to_string(exitCode) + "}");
}

// "a,b,,c" -> {a, b, c}; appended to `out`.
void splitCommaList(const std::string& list, std::vector<std::string>& out) {
    usize start = 0;
    while (start <= list.size()) {
        const usize comma = list.find(',', start);
        const std::string one = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!one.empty()) out.push_back(one);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

// A content-relative folder: forward slashes, no trailing slash, nothing absolute and no ".." -- the
// level's FOLIAGE path is stored relative to the content root and must stay inside it. "" = invalid.
std::string cleanContentRelDir(std::string d) {
    std::replace(d.begin(), d.end(), '\\', '/');
    while (!d.empty() && d.back() == '/') d.pop_back();
    if (d.empty() || d.front() == '/' || d.find(':') != std::string::npos) return {};
    for (usize at = 0; at <= d.size();) {
        const usize slash = d.find('/', at);
        const std::string seg = d.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (seg == ".." || seg.empty()) return {};
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return d;
}

} // namespace assetc
