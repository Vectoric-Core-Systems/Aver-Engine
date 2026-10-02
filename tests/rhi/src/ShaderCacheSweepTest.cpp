// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The shader blob cache's size bound.
//
// WHAT THIS GUARDS. The DXIL cache is keyed on the whole compiler input, which is what makes it
// correct with no invalidation logic -- a changed input simply misses -- and the price is that
// nothing ever becomes stale, so nothing was ever removed. Every shader edit left its predecessor
// behind, permanently, in a machine-wide directory no UI and no CLI could clear.
//
// It needs a test rather than an eyeball precisely because the failure is invisible: an unbounded
// cache works perfectly, and only ever grows. On a machine whose cache has not filled yet -- which
// is every machine on the day the sweep is written -- a sweeper that deleted everything, or nothing,
// or the newest files instead of the oldest, would look identical to a correct one.
//
// No device, no D3D12, no backend: the sweeper is pure std::filesystem over a temp directory.
#include "aver/core/Log.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/rhi/ShaderCacheSweep.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) AVER_INFO("   PASS  {}", what);
    else      { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static std::filesystem::path scratchDir() {
    return std::filesystem::temp_directory_path() / "AverShaderCacheSweepTest";
}

// Writes `bytes` of filler and stamps the file's write time so eviction order is deterministic.
// STAMPED EXPLICITLY rather than relying on creation order plus a sleep: a filesystem whose
// timestamp granularity is coarser than the loop would give several files the same time, and the
// sort would then be arbitrary -- a test that passes or fails by disk timing is worse than none.
static void writeBlob(const std::filesystem::path& p, usize bytes, int ageSeconds) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    const std::string filler(bytes, 'x');
    f.write(filler.data(), static_cast<std::streamsize>(filler.size()));
    f.close();
    std::error_code ec;
    std::filesystem::last_write_time(
        p, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(ageSeconds), ec);
}

static void resetDir() {
    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);
    std::filesystem::create_directories(scratchDir(), ec);
}

int main() {
    AVER_INFO("======== ShaderCacheSweepTest ========");

    // A cache that has not been created yet is the normal state on a first run, not an error. A
    // sweeper that reported failure here would put a scary line in every clean install's first log.
    {
        std::error_code ec;
        std::filesystem::remove_all(scratchDir(), ec);
        const auto r = rhi::sweepShaderCache(scratchDir(), 1024);
        check(r.ok, "a directory that does not exist is success, not failure");
        check(r.filesRemoved == 0 && r.bytesRemoved == 0, "and nothing is removed");
    }

    // Under budget: touch nothing. The ordinary case, and the one a too-eager sweeper would ruin by
    // making every launch recompile.
    {
        resetDir();
        writeBlob(scratchDir() / "a.dxil", 100, 30);
        writeBlob(scratchDir() / "b.dxil", 100, 20);
        const auto r = rhi::sweepShaderCache(scratchDir(), 1000);
        check(r.ok && r.filesRemoved == 0, "a cache under budget is left completely alone");
        check(r.bytesRemaining == 200 && r.filesRemaining == 2, "and it reports what it kept");
    }

    // OVER BUDGET, EVICTED OLDEST-FIRST. Three blobs of 100 bytes with a budget of 250: exactly one
    // must go, and it must be the oldest. A sweeper that removed the NEWEST would pass a "removed
    // one file" assertion and be exactly wrong, which is why the surviving names are checked.
    {
        resetDir();
        writeBlob(scratchDir() / "old.dxil",    100, 300);
        writeBlob(scratchDir() / "middle.dxil", 100, 200);
        writeBlob(scratchDir() / "new.dxil",    100, 100);
        const auto r = rhi::sweepShaderCache(scratchDir(), 250);
        check(r.ok && r.filesRemoved == 1, "exactly one blob is evicted to get under budget");
        check(r.bytesRemaining == 200 && r.bytesRemaining <= 250, "and what remains is under budget");
        check(!std::filesystem::exists(scratchDir() / "old.dxil"), "THE OLDEST went");
        check(std::filesystem::exists(scratchDir() / "middle.dxil") &&
              std::filesystem::exists(scratchDir() / "new.dxil"),
              "and the two newer ones survived -- not merely 'one file fewer'");
    }

    // A budget of zero is the "clear the cache" command, and must actually clear it.
    {
        resetDir();
        writeBlob(scratchDir() / "a.dxil", 100, 30);
        writeBlob(scratchDir() / "b.dxil", 100, 20);
        writeBlob(scratchDir() / "c.dxil", 100, 10);
        const auto r = rhi::sweepShaderCache(scratchDir(), 0);
        check(r.ok && r.filesRemoved == 3 && r.bytesRemaining == 0, "a budget of 0 clears the cache");
    }

    // ONLY .dxil IS TOUCHED. The cache lives under the user's own data directory, so a sweep that
    // deleted whatever it found there would be a far worse bug than an unbounded cache. Checked with
    // the budget at zero, which is the most destructive setting the sweeper has.
    {
        resetDir();
        writeBlob(scratchDir() / "blob.dxil", 100, 30);
        writeBlob(scratchDir() / "notes.txt", 100, 30);
        writeBlob(scratchDir() / "important.doc", 100, 30);
        const auto r = rhi::sweepShaderCache(scratchDir(), 0);
        check(r.filesRemoved == 1, "only the .dxil blob is removed");
        check(std::filesystem::exists(scratchDir() / "notes.txt") &&
              std::filesystem::exists(scratchDir() / "important.doc"),
              "EVERYTHING ELSE IN THE DIRECTORY SURVIVES, even at a zero budget");
        check(r.bytesRemaining == 0 && r.filesRemaining == 0,
              "and the sizes it reports count only what it manages, not the bystanders");
    }

    // A subdirectory is not descended into, for the same reason: the sweeper owns flat blobs, and
    // recursing would let a future feature's folder placed beside the cache be eaten by it.
    {
        resetDir();
        std::error_code ec;
        std::filesystem::create_directories(scratchDir() / "sub", ec);
        writeBlob(scratchDir() / "sub" / "nested.dxil", 100, 30);
        writeBlob(scratchDir() / "top.dxil", 100, 30);
        const auto r = rhi::sweepShaderCache(scratchDir(), 0);
        check(r.filesRemoved == 1, "only the top-level blob is swept");
        check(std::filesystem::exists(scratchDir() / "sub" / "nested.dxil"),
              "a nested file is not descended into");
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    AVER_INFO(g_failures ? "ShaderCacheSweepTest: {} FAILURES" : "ShaderCacheSweepTest: all checks passed ({})",
              g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
