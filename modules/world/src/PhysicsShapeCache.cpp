// PhysicsShapeCache.cpp -- see PhysicsShapeCache.hpp.
#include "aver/world/PhysicsShapeCache.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_state_abi.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <system_error>

namespace aver::world {

namespace {

constexpr u32 kChunkData = fmt::avrFourCC("DATA");

std::mutex g_storeMutex;   // two workers storing the same key would share one temporary
std::atomic<u32> g_hits{0}, g_misses{0}, g_stores{0}, g_unsaveable{0};
std::atomic<u64> g_restoreNs{0}, g_buildNs{0};

} // namespace

std::string physicsShapeCacheDir(const std::string& projectDir) {
    return projectDir.empty() ? std::string()
                              : projectDir + AVER_PATH_SEP "Saved" AVER_PATH_SEP "DerivedDataCache" AVER_PATH_SEP "Physics";
}

u64 physicsShapeKey(u64 sourceKey, u32 shapeCacheVersion, u32 vertexCount, u32 indexCount) {
    const std::string k = std::to_string(sourceKey) + "|" + std::to_string(shapeCacheVersion) + "|" +
                          std::to_string(vertexCount) + "|" + std::to_string(indexCount);
    const u64 h = fnv1a64(std::string_view(k));
    return h != 0 ? h : 1;
}

std::string physicsShapeCachePath(const std::string& dir, u64 key) {
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx.pshp", static_cast<unsigned long long>(key));
    return dir + AVER_PATH_SEP + name;
}

bool loadPhysicsShapeBlob(const std::string& dir, u64 key, std::vector<u8>& outBlob) {
    if (dir.empty()) return false;
    fmt::Avr1File file;
    if (!fmt::loadAvr1(physicsShapeCachePath(dir, key), file, nullptr)) return false;
    if (file.subtype != fmt::kAvrSubtypePshp) return false;
    const fmt::AvrChunk* data = file.find(kChunkData);
    if (!data || data->data.empty()) return false;
    outBlob = data->data;
    return true;
}

bool storePhysicsShapeBlob(const std::string& dir, u64 key, const u8* blob, usize size, std::string* why) {
    auto fail = [&](const char* what) {
        if (why) *why = what;
        return false;
    };
    if (dir.empty()) return fail("no cache directory");
    if (!blob || size == 0) return fail("empty blob");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return fail("cannot create the cache directory");

    fmt::Avr1File file;
    file.subtype = fmt::kAvrSubtypePshp;
    file.flags = fmt::kAvrFlagCooked;
    file.add(kChunkData, std::vector<u8>(blob, blob + size));
    std::lock_guard<std::mutex> lock(g_storeMutex);
    return fmt::saveAvr1(physicsShapeCachePath(dir, key), file, why);
}

PhysicsShapeCacheStats physicsShapeCacheStats() {
    PhysicsShapeCacheStats s;
    s.hits = g_hits.load();
    s.misses = g_misses.load();
    s.stores = g_stores.load();
    s.unsaveable = g_unsaveable.load();
    s.restoreMs = static_cast<double>(g_restoreNs.load()) * 1.0e-6;
    s.buildMs = static_cast<double>(g_buildNs.load()) * 1.0e-6;
    return s;
}

void resetPhysicsShapeCacheStats() {
    g_hits = 0; g_misses = 0; g_stores = 0; g_unsaveable = 0;
    g_restoreNs = 0; g_buildNs = 0;
}

#if AVER_MODULE_PHYSICS
i32 createMeshShapeCached(const f32* verts, i32 vertexCount, const i32* indices, i32 indexCount,
                          const std::string& cacheDir, u64 key) {
    using Clock = std::chrono::steady_clock;
    const bool useCache = !cacheDir.empty() && key != 0 && aver_phys_shape_cache_enabled() != 0;

    if (useCache) {
        std::vector<u8> blob;
        if (loadPhysicsShapeBlob(cacheDir, key, blob) && blob.size() <= 0x7FFFFFFFu) {
            const auto t0 = Clock::now();
            const i32 shape = aver_phys_shape_restore(blob.data(), static_cast<i32>(blob.size()), key);
            const auto t1 = Clock::now();
            if (shape) {
                g_restoreNs += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                ++g_hits;
                return shape;
            }
        }
        ++g_misses;
    }

    const auto t0 = Clock::now();
    const i32 shape = aver_phys_create_mesh_shape(verts, vertexCount, indices, indexCount);
    const auto t1 = Clock::now();
    if (!shape) return 0;
    if (!useCache) return shape;
    g_buildNs += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    const i32 need = aver_phys_shape_save(shape, key, nullptr, 0);
    if (need <= 0) { ++g_unsaveable; return shape; }
    std::vector<u8> blob(static_cast<usize>(need));
    if (aver_phys_shape_save(shape, key, blob.data(), need) != need) { ++g_unsaveable; return shape; }
    std::string why;
    if (storePhysicsShapeBlob(cacheDir, key, blob.data(), blob.size(), &why)) ++g_stores;
    else AVER_WARN("[ShapeCache] could not store {}: {}", physicsShapeCachePath(cacheDir, key), why);
    return shape;
}

i32 createMeshShapeFromSource(const f32* verts, i32 vertexCount, const i32* indices, i32 indexCount,
                              const std::string& cacheDir, u64 sourceKey) {
    const u64 key = (sourceKey != 0 && !cacheDir.empty() && vertexCount > 0 && indexCount > 0)
        ? physicsShapeKey(sourceKey, aver_phys_shape_cache_version(), static_cast<u32>(vertexCount), static_cast<u32>(indexCount))
        : 0;
    return createMeshShapeCached(verts, vertexCount, indices, indexCount, cacheDir, key);
}
#endif

} // namespace aver::world
