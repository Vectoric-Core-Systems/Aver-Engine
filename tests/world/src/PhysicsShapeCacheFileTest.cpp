// PhysicsShapeCacheFileTest -- the disk half of the physics shape binary cache (modules/world PhysicsShapeCache.hpp).
//
//   * the key changes with every input it is made from (path, size, mtime, the caller's version, the shape cache version, both counts),
//   * a blob stored under a key reads back byte for byte, in an AVR1 "PSHP" container, with no temporary left beside it,
//   * a miss is a miss whatever the reason: no directory, no file, a file for another key, a truncated or half-written one, a
//     container of another subtype, a container with no DATA chunk, garbage,
//   * a store creates the directory and replaces an older file under the same key,
//   * with the physics module: createMeshShapeCached builds and stores on the first call and restores on the second (the stats say
//     so), gives a shape that saves to the same bytes, stays uncached while the switch is off, and falls back to a build when the
//     file is damaged.
//
// No GPU, no device, no .NET. Hand-run, exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/world/PhysicsShapeCache.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_state_abi.h"
#endif

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string freshDir() {
    static int n = 0;
    const fs::path p = fs::temp_directory_path() / ("aver_pshp_test_" + std::to_string(++n));
    std::error_code ec;
    fs::remove_all(p, ec);
    return p.string();
}

static std::vector<u8> sampleBlob(size_t n, u8 seed) {
    std::vector<u8> b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<u8>(seed + i * 7);
    return b;
}

static void testKey() {
    AVER_INFO("=== the key ===");
    using world::physicsShapeKey;
    // The caller's source key is opaque to the cache: it changes with whatever the caller folded into it, and the key follows.
    const u64 base = physicsShapeKey(0xA11CE5ull, 1, 40, 120);
    check(base != 0, "never zero");
    check(base == physicsShapeKey(0xA11CE5ull, 1, 40, 120), "stable");
    check(base != physicsShapeKey(0xB0B5ull, 1, 40, 120), "the caller's source key (path, size, mtime, collision version)");
    check(base != physicsShapeKey(0xA11CE5ull, 2, 40, 120), "the shape cache version");
    check(base != physicsShapeKey(0xA11CE5ull, 1, 41, 120), "vertex count (a slot stopped colliding)");
    check(base != physicsShapeKey(0xA11CE5ull, 1, 40, 117), "index count");
    check(physicsShapeKey(0, 0, 0, 0) != 0, "even for all zeros");

    const std::string cd = world::physicsShapeCacheDir("C:/proj");
    check(cd.find("DerivedDataCache") != std::string::npos && cd.find("Physics") != std::string::npos, "the directory sits in DerivedDataCache/Physics");
    check(world::physicsShapeCacheDir("").empty(), "no project, no directory");
    const std::string path = world::physicsShapeCachePath("dir", 0xABCDEF0123456789ull);
    check(path.size() > 16 && path.find("abcdef0123456789.pshp") != std::string::npos, "the file is the key as 16 hex digits: " + path);
}

static void testFiles() {
    AVER_INFO("=== store and load ===");
    const std::string dir = freshDir();
    std::vector<u8> out;
    check(!world::loadPhysicsShapeBlob(dir, 5, out), "no directory: a miss");
    check(!world::loadPhysicsShapeBlob("", 5, out), "empty directory name: a miss");

    const std::vector<u8> blob = sampleBlob(5000, 3);
    std::string why;
    check(world::storePhysicsShapeBlob(dir, 5, blob.data(), blob.size(), &why), "store creates the directory and writes: " + why);
    check(fs::exists(world::physicsShapeCachePath(dir, 5)), "the file is there");
    check(!fs::exists(world::physicsShapeCachePath(dir, 5) + ".tmp"), "and no temporary beside it");
    check(world::loadPhysicsShapeBlob(dir, 5, out) && out == blob, "it reads back byte for byte");
    check(!world::loadPhysicsShapeBlob(dir, 6, out), "another key: a miss");

    fmt::Avr1File f;
    check(fmt::loadAvr1(world::physicsShapeCachePath(dir, 5), f, nullptr) && f.subtype == fmt::kAvrSubtypePshp &&
          f.find(fmt::avrFourCC("DATA")) != nullptr, "it is an AVR1 container of subtype PSHP with a DATA chunk");

    const std::vector<u8> blob2 = sampleBlob(300, 9);
    check(world::storePhysicsShapeBlob(dir, 5, blob2.data(), blob2.size()), "a second store under the same key");
    check(world::loadPhysicsShapeBlob(dir, 5, out) && out == blob2, "replaces the first");

    check(!world::storePhysicsShapeBlob(dir, 7, nullptr, 0) && !world::storePhysicsShapeBlob("", 7, blob.data(), blob.size()), "an empty blob and no directory are refused");

    AVER_INFO("=== damaged files are misses ===");
    const std::string p = world::physicsShapeCachePath(dir, 5);
    {
        // half the file
        const std::vector<u8> whole = [&] {
            std::ifstream in(p, std::ios::binary);
            return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }();
        check(whole.size() > 100, "the file has a size to cut");
        {
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(whole.data()), static_cast<std::streamsize>(whole.size() / 2));
        }
        check(!world::loadPhysicsShapeBlob(dir, 5, out), "a half-written file is a miss");
        {
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
            const char junk[] = "this is not a container";
            o.write(junk, sizeof junk);
        }
        check(!world::loadPhysicsShapeBlob(dir, 5, out), "garbage is a miss");
        {
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
        }
        check(!world::loadPhysicsShapeBlob(dir, 5, out), "an empty file is a miss");
    }
    {
        // a container of another subtype, and one with no DATA chunk
        fmt::Avr1File other;
        other.subtype = fmt::kAvrSubtypeMesh;
        other.add(fmt::avrFourCC("DATA"), sampleBlob(64, 1));
        std::string w;
        check(fmt::saveAvr1(p, other, &w), "write a MESH container under the same name: " + w);
        check(!world::loadPhysicsShapeBlob(dir, 5, out), "another subtype is a miss");
        fmt::Avr1File nodata;
        nodata.subtype = fmt::kAvrSubtypePshp;
        nodata.add(fmt::avrFourCC("XXXX"), sampleBlob(64, 1));
        check(fmt::saveAvr1(p, nodata, &w), "write a PSHP container with no DATA chunk: " + w);
        check(!world::loadPhysicsShapeBlob(dir, 5, out), "a missing DATA chunk is a miss");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if AVER_MODULE_PHYSICS
static std::vector<u8> blobOf(i32 shape, u64 key) {
    const i32 need = aver_phys_shape_save(shape, key, nullptr, 0);
    std::vector<u8> b(need > 0 ? static_cast<size_t>(need) : 0u);
    if (need > 0 && aver_phys_shape_save(shape, key, b.data(), need) != need) b.clear();
    return b;
}

static void testCachedCreate() {
    AVER_INFO("=== createMeshShapeCached ===");
    aver_phys_shutdown();
    aver_phys_init();
    const f32 v[12] = {-200, -200, 0, 200, -200, 0, 200, 200, 0, -200, 200, 0};
    const i32 idx[6] = {0, 1, 2, 0, 2, 3};
    const std::string dir = freshDir();
    const u64 key = world::physicsShapeKey(0x5EEDull, aver_phys_shape_cache_version(), 4, 6);

    // switch off: it is aver_phys_create_mesh_shape and writes nothing
    aver_phys_shape_cache_enable(0);
    world::resetPhysicsShapeCacheStats();
    const i32 plain = world::createMeshShapeCached(v, 4, idx, 6, dir, key);
    check(plain != 0, "off: the shape builds");
    check(!fs::exists(world::physicsShapeCachePath(dir, key)), "off: and no file is written");
    check(world::physicsShapeCacheStats().hits == 0 && world::physicsShapeCacheStats().stores == 0, "off: and the stats stay at zero");

    aver_phys_shape_cache_enable(1);
    world::resetPhysicsShapeCacheStats();
    const i32 first = world::createMeshShapeCached(v, 4, idx, 6, dir, key);
    check(first != 0, "on, cold: the shape builds");
    world::PhysicsShapeCacheStats s = world::physicsShapeCacheStats();
    check(s.misses == 1 && s.stores == 1 && s.hits == 0, "and is stored (1 miss, 1 store)");
    check(fs::exists(world::physicsShapeCachePath(dir, key)), "the file exists");

    const i32 second = world::createMeshShapeCached(v, 4, idx, 6, dir, key);
    s = world::physicsShapeCacheStats();
    check(second != 0 && second != first, "on, warm: a shape comes back (a new handle)");
    check(s.hits == 1 && s.stores == 1, "and it was a hit (1 hit, still 1 store)");
    check(blobOf(second, key) == blobOf(first, key) && !blobOf(first, key).empty(), "it saves to the same bytes as the built one");

    // another key is another entry; no key and no directory skip the cache
    const i32 other = world::createMeshShapeCached(v, 4, idx, 6, dir, key + 1);
    check(other != 0 && world::physicsShapeCacheStats().stores == 2, "another key stores another file");
    world::resetPhysicsShapeCacheStats();
    check(world::createMeshShapeCached(v, 4, idx, 6, dir, 0) != 0 && world::createMeshShapeCached(v, 4, idx, 6, "", key) != 0 &&
          world::physicsShapeCacheStats().misses == 0, "key 0 and an empty directory skip the cache without counting a miss");

    // a damaged file: a miss, rebuilt, and the file is replaced by a good one
    {
        std::ofstream o(world::physicsShapeCachePath(dir, key), std::ios::binary | std::ios::trunc);
        o << "garbage";
    }
    world::resetPhysicsShapeCacheStats();
    const i32 healed = world::createMeshShapeCached(v, 4, idx, 6, dir, key);
    s = world::physicsShapeCacheStats();
    check(healed != 0 && s.misses == 1 && s.stores == 1, "a damaged file is a miss: the shape is built and the file rewritten");
    world::resetPhysicsShapeCacheStats();
    check(world::createMeshShapeCached(v, 4, idx, 6, dir, key) != 0 && world::physicsShapeCacheStats().hits == 1, "and the next call hits");

    // the loaders' entry point: the key is made from the source key, the cache version and the counts
    {
        const std::string dir2 = freshDir();
        const u64 source = 0xABCDEFull;
        world::resetPhysicsShapeCacheStats();
        check(world::createMeshShapeFromSource(v, 4, idx, 6, dir2, source) != 0, "from a source key: the shape builds");
        check(fs::exists(world::physicsShapeCachePath(dir2, world::physicsShapeKey(source, aver_phys_shape_cache_version(), 4, 6))),
              "and is stored under physicsShapeKey(source, version, counts)");
        check(world::createMeshShapeFromSource(v, 4, idx, 6, dir2, source) != 0 && world::physicsShapeCacheStats().hits == 1, "the second call hits");
        world::resetPhysicsShapeCacheStats();
        check(world::createMeshShapeFromSource(v, 4, idx, 6, dir2, 0) != 0 && world::createMeshShapeFromSource(v, 4, idx, 6, "", source) != 0 &&
              world::physicsShapeCacheStats().misses == 0, "source key 0 and an empty directory skip the cache");
        std::error_code ec2;
        fs::remove_all(dir2, ec2);
    }

    aver_phys_shape_cache_enable(0);
    aver_phys_shutdown();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

int main() {
    AVER_INFO("=== PhysicsShapeCacheFileTest ===");
    testKey();
    testFiles();
#if AVER_MODULE_PHYSICS
    testCachedCreate();
#else
    AVER_INFO("   SKIP  createMeshShapeCached: this tree has no physics module");
#endif
    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    return g_failures;
}
