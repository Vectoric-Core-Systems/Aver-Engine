// GiCacheTest -- the .cache derived-data entry that lets a GI volume survive a run.
//
// NO GPU, and that is the point of testing this half separately. Everything asserted here is
// container arithmetic and key comparison: how many bytes a mip chain occupies, that a round trip
// returns exactly what went in, that a key differing anywhere is a MISS, and that a truncated or
// mislabelled file is rejected rather than half-read into a volume. None of that needs a device,
// and all of it is what decides whether a cache hit is safe.
#include "aver/formats/GiCache.hpp"

#include "aver/core/Log.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

namespace {
int gChecks = 0, gFailed = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (ok) { AVER_INFO("  ok    {}", what); return; }
    ++gFailed;
    AVER_ERROR("  FAIL  {}", what);
}

fmt::GiCacheKey sampleKey(u32 res = 8, u32 mips = 4) {
    fmt::GiCacheKey k;
    k.drawsKey = 0xDEADBEEFCAFEF00Dull;
    k.skyKey   = 0x0123456789ABCDEFull;
    k.centre[0] = 1.5f; k.centre[1] = -2.25f; k.centre[2] = 300.0f;
    k.extent = 1200.0f;
    k.resolution = res;
    k.mipCount = mips;
    return k;
}

// A volume whose every byte is a function of its index, so a round trip that shifts or truncates
// anything shows up as a mismatch rather than as plausible-looking noise.
std::vector<u8> fillVolume(const fmt::GiCacheKey& k) {
    std::vector<u8> v(static_cast<usize>(fmt::giCacheTotalBytes(k)));
    for (usize i = 0; i < v.size(); ++i) v[i] = static_cast<u8>((i * 31u + 7u) & 0xFF);
    return v;
}
}  // namespace

int main() {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "aver-gicache-test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    AVER_INFO("=== mip arithmetic ===");
    {
        const fmt::GiCacheKey k = sampleKey(8, 4);
        // 8^3 + 4^3 + 2^3 + 1^3 = 512 + 64 + 8 + 1 = 585 voxels, 8 bytes each (RGBA16F).
        check(fmt::giCacheMipBytes(k, 0) == 512 * 8, "mip 0 of an 8^3 volume is 512 voxels");
        check(fmt::giCacheMipBytes(k, 3) == 1 * 8, "the last mip is a single voxel");
        check(fmt::giCacheMipBytes(k, 4) == 0, "a mip past the chain is zero bytes, not an overrun");
        check(fmt::giCacheTotalBytes(k) == 585 * 8, "the whole chain is 585 voxels");
        check(fmt::giCacheMipOffset(k, 0) == 0, "mip 0 starts at the beginning");
        check(fmt::giCacheMipOffset(k, 1) == 512 * 8, "mip 1 starts after mip 0");
        check(fmt::giCacheMipOffset(k, 3) == (512 + 64 + 8) * 8, "mip 3 starts after the three before it");

        // The shipped default, stated as a number so a change to it is visible here.
        const fmt::GiCacheKey med = sampleKey(128, 8);
        check(fmt::giCacheTotalBytes(med) == 19173960ull,
              "a 128^3 8-mip volume is 19,173,960 bytes (~18.29 MiB)");
    }

    AVER_INFO("=== round trip ===");
    const std::string path = (root / fmt::giCacheFileName(sampleKey())).string();
    {
        fmt::GiCacheEntry in;
        in.key = sampleKey();
        in.voxels = fillVolume(in.key);

        std::string why;
        check(fmt::saveGiCache(path, in, &why), "an entry writes: " + why);
        check(std::filesystem::exists(path, ec), "the file is on disk");

        fmt::GiCacheEntry out;
        check(fmt::loadGiCache(path, out, &why), "it reads back: " + why);
        check(out.key == in.key, "the key survives the round trip");
        check(out.voxels == in.voxels, "every byte of the volume survives the round trip");
    }

    AVER_INFO("=== a key that differs anywhere is a different entry ===");
    {
        const fmt::GiCacheKey base = sampleKey();
        auto differs = [&](fmt::GiCacheKey k, const char* what) {
            check(k != base, std::string("a changed ") + what + " is a different key");
            check(fmt::giCacheFileName(k) != fmt::giCacheFileName(base),
                  std::string("...and lands in a different file, so both can coexist"));
        };
        { fmt::GiCacheKey k = base; k.drawsKey ^= 1ull;    differs(k, "draw list"); }
        { fmt::GiCacheKey k = base; k.skyKey ^= 1ull;      differs(k, "sky"); }
        { fmt::GiCacheKey k = base; k.extent += 1.0f;      differs(k, "volume extent"); }
        { fmt::GiCacheKey k = base; k.centre[1] += 1.0f;   differs(k, "volume centre"); }
        { fmt::GiCacheKey k = base; k.resolution = 256;    differs(k, "resolution"); }

        // The smallest representable move, NOT a tolerance: a cache that accepts "almost the same"
        // placement shows the wrong lighting for a while and then stops.
        fmt::GiCacheKey nudged = base;
        nudged.centre[0] = std::nextafter(nudged.centre[0], 1e30f);
        check(nudged != base, "a one-ULP move of the volume centre is a MISS, not a tolerated match");
    }

    AVER_INFO("=== a bad entry is a miss, never a half-read volume ===");
    {
        std::string why;
        fmt::GiCacheEntry out;
        check(!fmt::loadGiCache((root / "absent.cache").string(), out, &why),
              "a missing file fails cleanly");

        // Truncated payload: the length has to be checked against what the KEY describes, since a
        // stored size could simply agree with a payload that is wrong.
        std::vector<u8> bytes;
        {
            std::ifstream f(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        check(!bytes.empty(), "the good file could be re-read as bytes");
        const std::string cut = (root / "truncated.cache").string();
        {
            std::ofstream f(cut, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size() - 64));
        }
        check(!fmt::loadGiCache(cut, out, &why), "a truncated entry is rejected: " + why);

        // A file that is not a GI cache at all.
        const std::string alien = (root / "alien.cache").string();
        { std::ofstream f(alien, std::ios::binary); f << "not an AVR1 container at all"; }
        check(!fmt::loadGiCache(alien, out, &why), "a non-container is rejected: " + why);
    }

    AVER_INFO("=== the directory stays bounded ===");
    {
        const std::filesystem::path sweepDir = root / "sweep";
        std::filesystem::create_directories(sweepDir, ec);
        for (u32 i = 0; i < 6; ++i) {
            fmt::GiCacheEntry e;
            e.key = sampleKey(4, 2);
            e.key.drawsKey = 1000 + i;         // a distinct entry each time
            e.voxels = fillVolume(e.key);
            fmt::saveGiCache((sweepDir / fmt::giCacheFileName(e.key)).string(), e, nullptr);
        }
        u32 count = 0;
        for (auto& d : std::filesystem::directory_iterator(sweepDir, ec)) { (void)d; ++count; }
        check(count == 6, "six distinct bakes wrote six files");

        const u32 removed = fmt::giCacheSweep(sweepDir.string(), 4);
        check(removed == 2, "sweeping to 4 removed exactly 2");
        u32 after = 0;
        for (auto& d : std::filesystem::directory_iterator(sweepDir, ec)) { (void)d; ++after; }
        check(after == 4, "four remain");
        check(fmt::giCacheSweep(sweepDir.string(), 4) == 0, "sweeping again removes nothing");
    }

    AVER_INFO("=== the cache directory is beside the project, not inside Content ===");
    {
        const std::string d = fmt::giCacheDir("C:\\Projects\\Demo");
        check(d == "C:\\Projects\\Demo\\DerivedDataCache\\GI", "giCacheDir names DerivedDataCache\\GI");
        check(fmt::giCacheDir("").empty(), "no project directory means no cache directory");
    }

    std::filesystem::remove_all(root, ec);
    if (gFailed == 0) {
        AVER_INFO("=== all {} GI-cache checks passed ===", gChecks);
        return 0;
    }
    AVER_ERROR("=== {} of {} GI-cache checks FAILED ===", gFailed, gChecks);
    return 1;
}
