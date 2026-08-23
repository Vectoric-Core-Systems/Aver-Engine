#include "aver/formats/GiCache.hpp"

#include "aver/formats/Avr1.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace aver::fmt {
namespace {

constexpr u32 kSubtypeGiVolume = avrFourCC("GIVL");
constexpr u32 kChunkKey   = avrFourCC("GKEY");
constexpr u32 kChunkVoxel = avrFourCC("GVOX");

// The key as it sits on disk: fixed width, little-endian, no padding questions. Written field by
// field rather than as a memcpy of GiCacheKey, so adding a member to that struct cannot silently
// change the on-disk layout of files already written.
constexpr usize kKeyBytes = 8 + 8 + 4 * 3 + 4 + 4 + 4;   // drawsKey, skyKey, centre[3], extent, res, mips

void putU32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<u8>((v >> 24) & 0xFF));
}
void putU64(std::vector<u8>& b, u64 v) {
    putU32(b, static_cast<u32>(v & 0xFFFFFFFFull));
    putU32(b, static_cast<u32>(v >> 32));
}
void putF32(std::vector<u8>& b, f32 v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(b, bits);
}
u32 getU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
u64 getU64(const u8* p) { return static_cast<u64>(getU32(p)) | (static_cast<u64>(getU32(p + 4)) << 32); }
f32 getF32(const u8* p) {
    const u32 bits = getU32(p);
    f32 v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

}  // namespace

bool GiCacheKey::operator==(const GiCacheKey& o) const {
    // The floats are compared BIT-EXACTLY, deliberately, exactly as VoxiRenderer::giDrawsKey hashes
    // transforms by their bits: a cache that tolerates "almost the same" volume placement shows the
    // wrong lighting for a while and then stops. Either it is the identical input or it rebuilds.
    return drawsKey == o.drawsKey && skyKey == o.skyKey && extent == o.extent &&
           resolution == o.resolution && mipCount == o.mipCount &&
           centre[0] == o.centre[0] && centre[1] == o.centre[1] && centre[2] == o.centre[2];
}

u64 giCacheMipBytes(const GiCacheKey& k, u32 mip) {
    if (mip >= k.mipCount || k.resolution == 0) return 0;
    const u64 e = static_cast<u64>(k.resolution >> mip);
    if (e == 0) return 0;
    return e * e * e * 8ull;   // RGBA16F
}

u64 giCacheTotalBytes(const GiCacheKey& k) {
    u64 total = 0;
    for (u32 m = 0; m < k.mipCount; ++m) total += giCacheMipBytes(k, m);
    return total;
}

u64 giCacheMipOffset(const GiCacheKey& k, u32 mip) {
    u64 off = 0;
    for (u32 m = 0; m < mip && m < k.mipCount; ++m) off += giCacheMipBytes(k, m);
    return off;
}

std::string giCacheFileName(const GiCacheKey& k) {
    // One 64-bit name mixing every field, so two keys that differ anywhere get different files.
    // FNV-1a over the same bytes the KEY CHUNK stores, so the name is derived from exactly what is
    // compared on load -- and the comparison, not the name, is what decides a hit.
    std::vector<u8> b;
    b.reserve(kKeyBytes);
    putU64(b, k.drawsKey);
    putU64(b, k.skyKey);
    for (f32 c : k.centre) putF32(b, c);
    putF32(b, k.extent);
    putU32(b, k.resolution);
    putU32(b, k.mipCount);

    u64 h = 1469598103934665603ull;
    for (u8 byte : b) { h ^= byte; h *= 1099511628211ull; }

    char name[32] = {};
    std::snprintf(name, sizeof(name), "%016llx.cache", static_cast<unsigned long long>(h));
    return name;
}

std::string giCacheDir(const std::string& projectDir) {
    return projectDir.empty() ? std::string() : projectDir + "\\DerivedDataCache\\GI";
}

bool loadGiCache(const std::string& path, GiCacheEntry& out, std::string* why) {
    Avr1File file;
    if (!loadAvr1(path, file, why)) return false;
    if (file.subtype != kSubtypeGiVolume) {
        if (why) *why = path + ": not a GI volume cache (wrong AVR1 subtype)";
        return false;
    }
    const AvrChunk* key = file.find(kChunkKey);
    const AvrChunk* vox = file.find(kChunkVoxel);
    if (!key || key->data.size() < kKeyBytes) {
        if (why) *why = path + ": the key chunk is missing or short";
        return false;
    }
    if (!vox) {
        if (why) *why = path + ": the volume chunk is missing";
        return false;
    }

    const u8* p = key->data.data();
    GiCacheKey k;
    k.drawsKey = getU64(p);      p += 8;
    k.skyKey   = getU64(p);      p += 8;
    for (f32& c : k.centre) { c = getF32(p); p += 4; }
    k.extent     = getF32(p);    p += 4;
    k.resolution = getU32(p);    p += 4;
    k.mipCount   = getU32(p);

    // A payload whose length disagrees with its own key is not a cache entry, it is a truncated
    // file. Checked against the arithmetic rather than a stored size, so a size field could not
    // agree with a payload that does not.
    const u64 want = giCacheTotalBytes(k);
    if (want == 0 || vox->data.size() != want) {
        if (why)
            *why = path + ": the volume is " + std::to_string(vox->data.size()) + " bytes but its key describes " +
                   std::to_string(want);
        return false;
    }

    out.key = k;
    out.voxels = vox->data;
    return true;
}

bool saveGiCache(const std::string& path, const GiCacheEntry& in, std::string* why) {
    if (in.voxels.size() != giCacheTotalBytes(in.key)) {
        if (why) *why = "refusing to write a GI cache whose volume does not match its own key";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::vector<u8> keyBytes;
    keyBytes.reserve(kKeyBytes);
    putU64(keyBytes, in.key.drawsKey);
    putU64(keyBytes, in.key.skyKey);
    for (f32 c : in.key.centre) putF32(keyBytes, c);
    putF32(keyBytes, in.key.extent);
    putU32(keyBytes, in.key.resolution);
    putU32(keyBytes, in.key.mipCount);

    Avr1File file;
    file.subtype = kSubtypeGiVolume;
    file.add(kChunkKey, std::move(keyBytes), kAvrChunkRequired);
    // GpuUploadable: the payload goes straight into a texture, so the container aligns it to 256
    // bytes rather than the base alignment (FORMAT_SPECS 3.2).
    file.add(kChunkVoxel, in.voxels, kAvrChunkRequired | kAvrChunkGpuUploadable);
    return saveAvr1(path, file, why);
}

u32 giCacheSweep(const std::string& dir, u32 keep) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;

    struct Entry { std::filesystem::path path; std::filesystem::file_time_type when; };
    std::vector<Entry> entries;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".cache") continue;
        entries.push_back({it->path(), it->last_write_time(ec)});
    }
    if (entries.size() <= keep) return 0;

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.when > b.when; });   // newest first
    u32 removed = 0;
    for (usize i = keep; i < entries.size(); ++i)
        if (std::filesystem::remove(entries[i].path, ec)) ++removed;
    return removed;
}

}  // namespace aver::fmt
