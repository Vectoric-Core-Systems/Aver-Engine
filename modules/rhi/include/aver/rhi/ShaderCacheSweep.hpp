// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Bounds the on-disk shader blob cache, and gives something a way to clear it.
//
// THE PROBLEM THIS EXISTS FOR. The DXIL cache under %LOCALAPPDATA%/AverEngine/ShaderCache is keyed
// on the WHOLE compiler input -- source, defines, entry, target, a format version -- which is what
// makes it correct with no invalidation logic: a changed input simply misses. The cost of that
// design is that nothing ever becomes stale, so nothing is ever removed. Every shader edit leaves
// its predecessor behind, permanently, in a machine-wide directory that no UI and no CLI could
// clear. On a machine that has been developing shaders for months that is the whole history of every
// variant ever compiled.
//
// A BUDGET IN BYTES, EVICTED OLDEST-FIRST, rather than a max age or a max count. Age is wrong
// because a blob's usefulness has nothing to do with when it was written -- a shader untouched for a
// year is the one most likely to be needed unchanged at the next launch. Count is wrong because the
// blobs vary enormously in size (a trivial vertex shader against the ray-driven pixel shader), so a
// count bound either wastes the budget or blows past it.
//
// LAST-WRITE TIME, NOT LAST ACCESS. Windows disables last-access-time updates by default
// (NtfsDisableLastAccessUpdate has been on since Vista), so atime is not a signal here -- it would
// often equal mtime and make the eviction order arbitrary rather than merely imperfect. Writing time
// is at least truthful about when a blob entered the cache. The consequence is honest and worth
// stating: a long-lived, frequently-hit blob can be evicted before a recent one-off. It will be
// recompiled once and re-enter the cache, which is a cost of milliseconds, not correctness.
//
// PURE std::filesystem AND HEADER-ONLY so a test can drive it against a temp directory with no
// device, no D3D12 and no backend at all -- which matters because the failure this guards against is
// invisible on any machine whose cache has not yet grown.
#pragma once

#include "aver/core/Types.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace aver::rhi {

struct ShaderCacheSweepResult {
    usize filesRemoved = 0;
    u64   bytesRemoved = 0;
    u64   bytesRemaining = 0;
    usize filesRemaining = 0;
    // False when the directory could not be read at all. Distinct from "swept nothing", which is the
    // ordinary result on a cache under budget -- a caller that cannot tell those apart would report
    // a broken cache directory as a healthy one.
    bool  ok = false;
};

// Deletes the oldest blobs in `dir` until the total size of what remains is at or under
// `budgetBytes`. `budgetBytes == 0` clears the directory entirely, which is what a "clear the shader
// cache" command wants.
//
// Only files matching `extension` are considered, counted or removed: the directory is under the
// user's own data folder, and a sweep that deleted whatever it found there would be a much worse bug
// than an unbounded cache. A missing directory is success with nothing done -- the cache not
// existing yet is the normal state on a first run, not an error.
inline ShaderCacheSweepResult sweepShaderCache(const std::filesystem::path& dir, u64 budgetBytes,
                                               const char* extension = ".dxil") {
    ShaderCacheSweepResult out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) { out.ok = true; return out; }
    if (!std::filesystem::is_directory(dir, ec) || ec) return out;

    struct Entry { std::filesystem::path path; u64 size; std::filesystem::file_time_type when; };
    std::vector<Entry> entries;
    u64 total = 0;

    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
        if (it->path().extension() != extension) continue;
        const u64 sz = static_cast<u64>(it->file_size(ec));
        if (ec) { ec.clear(); continue; }
        const auto when = std::filesystem::last_write_time(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        entries.push_back({it->path(), sz, when});
        total += sz;
    }
    out.ok = true;
    out.bytesRemaining = total;
    out.filesRemaining = entries.size();
    if (total <= budgetBytes) return out;

    // Oldest first, so the loop below removes in eviction order and can stop the moment it is under.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.when < b.when; });

    for (const Entry& e : entries) {
        if (total <= budgetBytes) break;
        std::error_code rmec;
        if (!std::filesystem::remove(e.path, rmec) || rmec) continue;   // locked by another process: skip, do not fail
        total -= e.size;
        ++out.filesRemoved;
        out.bytesRemoved += e.size;
        --out.filesRemaining;
    }
    out.bytesRemaining = total;
    return out;
}

} // namespace aver::rhi
