// Shader source loaded from files. See ShaderFiles.hpp for why this exists and why the default
// path is the executable directory rather than the source tree.
#include "aver/rhi/ShaderFiles.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <vector>
#include <string>

namespace aver::rhi {
namespace {

// std::map, not unordered_map: a handful of entries, and a stable order makes the "which shaders
// did this build actually load" log readable when something is missing.
std::map<std::string, std::string, std::less<>> g_cache;
std::string g_sourceDir;
u64         g_revision = 0;
u64         g_corpusHash = 0;   // 0 = not computed yet; see shaderCorpusHash()
// Pipelines are built on a worker as well as the render thread: one lock over everything above. Recursive because
// shaderCorpusHash() reads files through shaderFile(). A reload RETIRES the texts instead of freeing them, since a
// compile in flight may still hold one (a dev-only cost of a few MB per reload).
std::recursive_mutex g_mu;
std::vector<std::map<std::string, std::string, std::less<>>> g_retired;

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
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    const usize n = g_cache.size();
    if (n == 0) return 0;
    g_retired.push_back(std::move(g_cache));
    g_cache.clear();
    g_corpusHash = 0;   // recomputed on demand; the texts it summarised are gone
    ++g_revision;
    AVER_INFO("[RHI.Shaders] dropped {} cached shader file(s); revision {}", n, g_revision);
    return n;
}

u64 shaderFileRevision() { std::lock_guard<std::recursive_mutex> lk(g_mu); return g_revision; }

// See the header for why a blob cache cannot key on one shader's own text alone.
//
// READS THE DIRECTORY RATHER THAN g_cache, deliberately: the cache holds only what has been asked
// for so far, so hashing it would give a different answer depending on which pipeline compiled
// first -- a key that changes with call order is worse than no key at all. The source directory is
// searched first, matching shaderFile()'s own order, so a --shader-source edit invalidates too.
u64 shaderCorpusHash() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (g_corpusHash) return g_corpusHash;
    u64 h = 0xcbf29ce484222325ull;
    const auto mix = [&h](std::string_view sv) {
        for (const char c : sv) { h ^= static_cast<unsigned char>(c); h *= 0x100000001b3ull; }
        h ^= 0xffu; h *= 0x100000001b3ull;
    };
    std::vector<std::string> names;
    const std::string dirs[2] = { g_sourceDir, join(executableDir(), "shaders") };
    for (const std::string& dir : dirs) {
        if (dir.empty()) continue;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        if (ec) continue;
        for (const auto& e : it) {
            std::error_code fec;
            if (!e.is_regular_file(fec) || fec) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".hlsl" && ext != ".hlsli") continue;
            names.push_back(e.path().filename().string());
        }
        if (!names.empty()) break;   // the first directory holding shaders is the one in force
    }
    // Sorted so a directory iteration order change is not an invalidation.
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string& n : names) { mix(n); mix(shaderFile(n)); }
    // Never 0 -- that is the "not computed yet" sentinel, and a corpus hashing to it would be
    // recomputed on every call.
    g_corpusHash = h ? h : 1ull;
    return g_corpusHash;
}

const std::string* shaderFileIfPresent(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (const auto it = g_cache.find(name); it != g_cache.end())
        return it->second.empty() ? nullptr : &it->second;
    const std::string candidates[2] = {
        g_sourceDir.empty() ? std::string() : join(g_sourceDir, name),
        join(join(executableDir(), "shaders"), name),
    };
    std::string text;
    for (const std::string& path : candidates) {
        if (path.empty()) continue;
        if (readFileText(path, text)) {
            normaliseNewlines(text);
            return &g_cache.emplace(std::string(name), std::move(text)).first->second;
        }
    }
    // NOT CACHED AS EMPTY. A miss here is ordinary (the handler is probing suffixes), and caching it
    // would poison the real shaderFile() into reporting the file absent forever afterwards.
    return nullptr;
}

const std::string& shaderFile(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
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
        // NAME THE CAUSE, NOT ONLY THE SYMPTOM. This used to say the shader "will fail to compile
        // and DXC will say which declarations are missing" -- true, and precisely why the real
        // failure hid for so long: a file-not-found surfaced as a COMPILE error about missing
        // declarations, which reads like a broken shader rather than a broken install. A packaged
        // payload shipped dxcompiler.dll and no .hlsl at all (scripts/payload.allowlist had no
        // shaders entry until it was added), and this line described the consequence rather than
        // the cause every time.
        AVER_ERROR("[RHI.Shaders] cannot read '{}' -- tried {}. The next DXC error will blame "
                   "missing declarations; the real cause is this file. Either the build tree never "
                   "deployed it (aver_deploy_shaders), a --shader-source directory is missing it, "
                   "or a packaged payload did not ship shaders/.",
                   name, tried);
        text.clear();
    } else {
        normaliseNewlines(text);
    }
    return g_cache.emplace(std::string(name), std::move(text)).first->second;
}

}   // namespace aver::rhi
