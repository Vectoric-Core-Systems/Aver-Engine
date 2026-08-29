// Shader source loaded from files. See ShaderFiles.hpp for why this exists and why the default
// path is the executable directory rather than the source tree.
#include "aver/rhi/ShaderFiles.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <map>
#include <string>

namespace aver::rhi {
namespace {

// std::map, not unordered_map: a handful of entries, and a stable order makes the "which shaders
// did this build actually load" log readable when something is missing.
std::map<std::string, std::string, std::less<>> g_cache;
std::string g_sourceDir;
u64         g_revision = 0;

// CRLF -> LF, in place. See the header's note on why this is not optional.
void normaliseNewlines(std::string& s) {
    usize w = 0;
    for (usize r = 0; r < s.size(); ++r) {
        if (s[r] == '\r') continue;
        s[w++] = s[r];
    }
    s.resize(w);
}

// Joins a directory and a name without caring whether the directory already ends in a separator.
std::string join(std::string_view dir, std::string_view name) {
    std::string p(dir);
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    p += name;
    return p;
}

}   // namespace

void setShaderSourceDir(std::string_view dir) {
    g_sourceDir.assign(dir);
    if (!g_sourceDir.empty())
        AVER_INFO("[RHI.Shaders] source directory: {} (searched before the executable's own)", g_sourceDir);
}

const std::string& shaderSourceDir() { return g_sourceDir; }

usize reloadShaderFiles() {
    const usize n = g_cache.size();
    if (n == 0) return 0;
    g_cache.clear();
    ++g_revision;
    AVER_INFO("[RHI.Shaders] dropped {} cached shader file(s); revision {}", n, g_revision);
    return n;
}

u64 shaderFileRevision() { return g_revision; }

// See the header for what this is for. Two behaviours worth stating, both learned by wiring hot
// reload up against it:
//
// IT KEEPS NO CACHE OF ITS OWN. An earlier version did, and reloadShaderFiles() could not clear it --
// so a reload silently had no effect on any file that had been verified, which is every file that
// matters. shaderFile() owns the one cache; this only decides what to hand back.
//
// IT ONLY COMPARES BEFORE THE FIRST RELOAD. The oracle exists to prove the EXTRACTION was faithful --
// that the file reproduces the literal it replaced, byte for byte, at startup. After a deliberate
// edit, the file is SUPPOSED to differ; that is the entire point of --shader-source. Continuing to
// compare would make every hot-reload edit fail the check and get silently replaced by the embedded
// copy, i.e. hot reload that appears to work and changes nothing. So: verify at revision 0, trust
// the file after that.
const std::string& verifiedShaderFile(std::string_view name, const char* embedded) {
    const std::string& fromFile = shaderFile(name);
    if (g_revision != 0) return fromFile;   // a reload happened: the file is the authority now

    static std::map<std::string, bool, std::less<>> reported;
    if (reported.find(name) != reported.end()) return fromFile;
    reported.emplace(std::string(name), true);

    std::string expected = embedded ? embedded : "";
    normaliseNewlines(expected);
    if (!fromFile.empty() && fromFile == expected) {
        AVER_INFO("[RHI.Shaders] {} matches its embedded copy ({} bytes)", name, expected.size());
        return fromFile;
    }
    // THE FILE WINS, and it is not a close call. The alternative -- fall back to the literal so the
    // engine certainly renders what it did yesterday -- silently discards an edit someone made to
    // the file, which is now the real source. During this migration the literal is the copy, not the
    // original. So: use the file, and say loudly that the literal beside it has gone stale.
    AVER_ERROR("[RHI.Shaders] {} DIFFERS from its embedded copy (file {} bytes, embedded {} bytes). "
               "USING THE FILE. Either the extraction was wrong, or the file was edited and the "
               "literal is now stale -- update the literal or delete it.",
               name, fromFile.size(), expected.size());
    return fromFile;
}

const std::string& shaderFile(std::string_view name) {
    if (const auto it = g_cache.find(name); it != g_cache.end()) return it->second;

    // The source directory first when one was named, the executable's own always. Both are tried
    // before giving up so that a source directory missing ONE file falls back per-file rather than
    // failing wholesale -- which is what you want while moving text out of C++ a file at a time.
    const std::string candidates[2] = {
        g_sourceDir.empty() ? std::string() : join(g_sourceDir, name),
        join(join(executableDir(), "shaders"), name),
    };

    std::string text;
    bool loaded = false;
    std::string tried;
    for (const std::string& path : candidates) {
        if (path.empty()) continue;
        if (!tried.empty()) tried += ", ";
        tried += path;
        if (readFileText(path, text)) { loaded = true; break; }
    }

    if (!loaded) {
        AVER_ERROR("[RHI.Shaders] cannot read '{}' -- tried {}. The shader that needs it will fail "
                   "to compile and DXC will say which declarations are missing.",
                   name, tried);
        text.clear();
    } else {
        normaliseNewlines(text);
    }
    return g_cache.emplace(std::string(name), std::move(text)).first->second;
}

}   // namespace aver::rhi
