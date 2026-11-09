// The derived-data cache itself: the .octex container, the key that names an entry, and
// setTextureCacheDir/textureCacheDir's storage. See TextureCache.hpp for the contract and
// TextureUpload.cpp for the caller.
#include "aver/assets/TextureCache.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/Bc7.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace aver::assets {

namespace {

// ---- the cache directory: process-wide, set once per project, read from any thread ---------------
std::mutex g_dirMutex;
std::string g_dir;

// ---- the .octex container: magic, a fixed header, then mip 0..N-1's BC7 blocks, tightly packed ---
constexpr u32 fourCC(const char (&s)[5]) {
    return static_cast<u32>(static_cast<u8>(s[0])) | (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(s[2])) << 16) | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}
constexpr u32 kOctexMagic = fourCC("OCTX");
constexpr u32 kFlagCoveragePreserved = 1u << 0;

// magic, version, format, width, height, mips (6 x u32) + averageLinear (3 x f32) + flags (u32).
constexpr usize kHeaderBytes = 4 * 7 + 4 * 3;

void putU32(std::vector<u8>& b, u32 v) {
    b.push_back(static_cast<u8>(v));
    b.push_back(static_cast<u8>(v >> 8));
    b.push_back(static_cast<u8>(v >> 16));
    b.push_back(static_cast<u8>(v >> 24));
}
void putU64(std::vector<u8>& b, u64 v) {
    putU32(b, static_cast<u32>(v));
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
f32 getF32(const u8* p) {
    const u32 bits = getU32(p);
    f32 v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// The absolute path, lower-cased on Windows. WHY LOWER-CASED: Windows paths are case-insensitive,
// so "Leaf.PNG" and "leaf.png" name the SAME file and must hash to the SAME entry -- a material and
// a re-import of the identical texture under different case would otherwise each bake and keep
// their own copy forever.
std::string normalizedAbsolutePathLower(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    std::string s = ec ? path : abs.lexically_normal().string();
#if defined(_WIN32)
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return s;
}

// stat() only -- the caller relies on this NOT opening or decoding `path`.
bool statTextureFile(const std::string& path, u64& size, u64& mtimeTicks) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) return false;
    const auto wt = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    size = static_cast<u64>(sz);
    mtimeTicks = static_cast<u64>(wt.time_since_epoch().count());
    return true;
}

// THE KEY. Deliberately built from nothing but what a stat() and the caller's own arguments can
// say -- see TextureCache.hpp's top comment for why a decoded pixel could never be allowed in here.
// `usage` already decides on its own whether coverage preservation is ever attempted at all (Colour
// only -- TextureUpload.cpp), so folding that same fact in again as its own byte costs nothing and
// names the condition rather than leaving it implicit in the usage value alone.
u64 cacheKey(const std::string& absPathLower, u64 fileSize, u64 mtimeTicks, TextureUsage usage) {
    std::vector<u8> b;
    b.reserve(absPathLower.size() + 32);
    b.insert(b.end(), absPathLower.begin(), absPathLower.end());
    putU64(b, fileSize);
    putU64(b, mtimeTicks);
    putU32(b, static_cast<u32>(usage));
    putU32(b, fmt::kBc7EncoderVersion);
    putU32(b, kTextureCacheFormatVersion);
    b.push_back(usage == TextureUsage::Colour ? 1 : 0);   // the "coverage flag" -- see above
    // Normal maps only: their mips stopped renormalising (specular AA), so their old entries retire.
    if (usage == TextureUsage::NormalMap) putU32(b, 2u);
    return fnv1a64(b.data(), b.size());
}

std::string cacheFileName(u64 key) {
    char name[24] = {};
    std::snprintf(name, sizeof(name), "%016llx.octex", static_cast<unsigned long long>(key));
    return name;
}

// Validates the fixed header at the front of an .octex and fills `info` (offsets included). Shared by the whole-file
// loader and the header-only stat so the two can never disagree on what a usable entry is.
bool parseHeader(const u8* p, usize size, CachedTextureInfo& info) {
    if (size < kHeaderBytes) return false;
    usize off = 0;
    if (getU32(p + off) != kOctexMagic) return false;
    off += 4;
    if (getU32(p + off) != kTextureCacheFormatVersion) return false;
    off += 4;
    const u32 format = getU32(p + off); off += 4;
    if (format != static_cast<u32>(rhi::Format::BC7Unorm) &&
        format != static_cast<u32>(rhi::Format::BC7UnormSrgb)) return false;
    const u32 width  = getU32(p + off); off += 4;
    const u32 height = getU32(p + off); off += 4;
    const u32 mips   = getU32(p + off); off += 4;
    // kMaxTextureMips GUARDS the offset table and the reader's mipBlocks.resize(mips), not real content -- no texture
    // this engine builds a chain for needs more than ~15 (a 16384-wide level 0), so a `mips` this large can only be a
    // corrupt header, and resizing straight off it would ask for gigabytes of empty vector shells before the first
    // byte-count check ever gets to reject the file.
    if (width == 0 || height == 0 || mips == 0 || mips > kMaxTextureMips) return false;
    info.format = static_cast<rhi::Format>(format);
    info.width = width; info.height = height; info.mips = mips;
    for (f32& c : info.averageLinear) { c = getF32(p + off); off += 4; }
    const u32 flags = getU32(p + off);
    info.coveragePreserved = (flags & kFlagCoveragePreserved) != 0;
    u64 at = kHeaderBytes;
    for (u32 m = 0; m < mips; ++m) {
        info.mipOffset[m] = at;
        at += bc7MipBytes(mipExtent(width, m), mipExtent(height, m));
    }
    info.mipOffset[mips] = at;
    return true;
}

std::string entryPathFor(const std::string& dir, const std::string& path, TextureUsage usage) {
    u64 size = 0, mtime = 0;
    if (!statTextureFile(path, size, mtime)) return {};
    return dir + AVER_PATH_SEP + cacheFileName(cacheKey(normalizedAbsolutePathLower(path), size, mtime, usage));
}

// A small stripe of locks over the cache KEY, not the whole cache: two threads baking DIFFERENT
// textures must never wait on each other (loadCachedTexture takes no lock at all -- a rename is
// already atomic, so a reader sees a whole old or whole new file with no lock needed), but two
// threads baking the SAME one -- two materials that happen to share a texture, resolved
// concurrently -- would otherwise both write "<file>.tmp" at once: writeFileBytesAtomic's temporary
// name is derived from the target path alone, so that race is a real corruption risk, not a
// hypothetical one. 16-way striping keeps the common case (different files) lock-free in practice.
constexpr usize kWriteStripes = 16;
std::mutex g_writeMutex[kWriteStripes];
std::mutex& writeMutexFor(u64 key) { return g_writeMutex[key % kWriteStripes]; }

} // namespace

void setTextureCacheDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_dirMutex);
    g_dir = dir;
}

std::string textureCacheDir() {
    std::lock_guard<std::mutex> lock(g_dirMutex);
    return g_dir;
}

bool textureCompressionEnabled() {
    // A function-local magic static: C++11 guarantees its initialiser runs exactly once even under
    // concurrent first calls, which is what makes "decided once, logged once" safe with no mutex of
    // its own -- the same pattern Texture.cpp's own srgbToLinear table already uses for the same
    // reason (a lazily-built, read-only, process-lifetime constant).
    static const bool enabled = [] {
        const char* v = std::getenv("AVER_TEXTURE_COMPRESSION");
        const bool on = !(v && std::strcmp(v, "0") == 0);
        AVER_INFO("[Texture] BC7 compression + derived-data cache is {}",
                  on ? "ON" : "OFF (AVER_TEXTURE_COMPRESSION=0)");
        return on;
    }();
    return enabled;
}

bool textureCompressionEligible(u32 width, u32 height) {
    if ((width & 3u) || (height & 3u)) return false;   // BC7 needs whole 4x4 blocks
    return (width > height ? width : height) >= 256;   // a small UI icon stays exact RGBA8
}

bool loadCachedTexture(const std::string& path, TextureUsage usage, CachedTexture& out) {
    const std::string dir = textureCacheDir();
    if (dir.empty()) return false;
    const std::string file = entryPathFor(dir, path, usage);
    if (file.empty()) return false;

    std::vector<u8> bytes;
    if (!readFileBytes(file, bytes)) return false;   // no entry -- an ordinary miss
    CachedTextureInfo info;
    if (!parseHeader(bytes.data(), bytes.size(), info)) return false;
    // ANY DISAGREEMENT IS A MISS, per the header's own contract: a truncated file, or trailing bytes (`mips` lied, or
    // a future writer's layout does not match this reader's) -- either way "rebuild it" is the honest answer, not a
    // guess at which prefix of the file might still be good.
    if (info.mipOffset[info.mips] != bytes.size()) return false;

    CachedTexture cand;
    cand.format = info.format;
    cand.width = info.width; cand.height = info.height; cand.mips = info.mips;
    for (int c = 0; c < 3; ++c) cand.averageLinear[c] = info.averageLinear[c];
    cand.coveragePreserved = info.coveragePreserved;
    cand.mipBlocks.resize(info.mips);
    for (u32 m = 0; m < info.mips; ++m)
        cand.mipBlocks[m].assign(bytes.begin() + static_cast<isize>(info.mipOffset[m]),
                                 bytes.begin() + static_cast<isize>(info.mipOffset[m + 1]));
    out = std::move(cand);
    return true;
}

bool statCachedTexture(const std::string& path, TextureUsage usage, CachedTextureInfo& out) {
    out = CachedTextureInfo{};
    const std::string dir = textureCacheDir();
    if (dir.empty()) return false;
    const std::string file = entryPathFor(dir, path, usage);
    if (file.empty()) return false;

    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(file, ec);
    if (ec) return false;
    std::vector<u8> head;
    if (!readFileRange(file, 0, kHeaderBytes, head)) return false;
    CachedTextureInfo info;
    if (!parseHeader(head.data(), head.size(), info)) return false;
    if (info.mipOffset[info.mips] != static_cast<u64>(fileSize)) return false;
    info.file = file;
    info.ok = true;
    out = info;
    return true;
}

bool readCachedMips(const CachedTextureInfo& info, u32 firstMip, u32 endMip, std::vector<u8>& out) {
    if (!info.ok || firstMip >= endMip || endMip > info.mips) return false;
    return readFileRange(info.file, info.mipOffset[firstMip], info.mipBlockBytes(firstMip, endMip), out);
}

bool saveCachedTexture(const std::string& path, TextureUsage usage, const CachedTexture& in) {
    const std::string dir = textureCacheDir();
    if (dir.empty()) return false;
    if (in.mips == 0 || in.mipBlocks.size() != in.mips) return false;

    u64 size = 0, mtime = 0;
    if (!statTextureFile(path, size, mtime)) return false;

    const u64 key = cacheKey(normalizedAbsolutePathLower(path), size, mtime, usage);
    if (!createDirectories(dir)) return false;

    std::vector<u8> bytes;
    usize total = kHeaderBytes;
    for (const std::vector<u8>& mip : in.mipBlocks) total += mip.size();
    bytes.reserve(total);

    putU32(bytes, kOctexMagic);
    putU32(bytes, kTextureCacheFormatVersion);
    putU32(bytes, static_cast<u32>(in.format));
    putU32(bytes, in.width);
    putU32(bytes, in.height);
    putU32(bytes, in.mips);
    for (f32 c : in.averageLinear) putF32(bytes, c);
    putU32(bytes, in.coveragePreserved ? kFlagCoveragePreserved : 0u);
    for (const std::vector<u8>& mip : in.mipBlocks) bytes.insert(bytes.end(), mip.begin(), mip.end());

    const std::string file = dir + AVER_PATH_SEP + cacheFileName(key);
    // See writeMutexFor's own comment for why this needs a lock at all: two threads baking the
    // SAME source concurrently would otherwise race the identical ".tmp" name.
    std::lock_guard<std::mutex> lock(writeMutexFor(key));
    return writeFileBytesAtomic(file, bytes.data(), bytes.size());
}

} // namespace aver::assets
