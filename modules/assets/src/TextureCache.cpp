// The derived-data cache itself: the .octex container, the key that names an entry, and
// setTextureCacheDir/textureCacheDir's storage. See TextureCache.hpp for the contract and
// TextureUpload.cpp for the caller.
#include "aver/assets/TextureCache.hpp"

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

// BC7 is ALWAYS 16 bytes/block (Bc7.hpp: mode 6 only, one size for every texel format it writes),
// so this needs no format switch the way the RHI backends' own blockBytes() has for five formats --
// and those live file-local in D3D12Device.cpp/VulkanCommon.hpp, unreachable from here regardless.
u64 bc7MipBytes(u32 w, u32 h) {
    const u64 bw = (static_cast<u64>(w) + 3) / 4;
    const u64 bh = (static_cast<u64>(h) + 3) / 4;
    return bw * bh * 16;
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

    u64 size = 0, mtime = 0;
    if (!statTextureFile(path, size, mtime)) return false;

    const u64 key = cacheKey(normalizedAbsolutePathLower(path), size, mtime, usage);
    const std::string file = dir + "\\" + cacheFileName(key);

    std::vector<u8> bytes;
    if (!readFileBytes(file, bytes)) return false;   // no entry -- an ordinary miss
    if (bytes.size() < kHeaderBytes) return false;

    usize off = 0;
    const u32 magic = getU32(&bytes[off]); off += 4;
    if (magic != kOctexMagic) return false;
    const u32 version = getU32(&bytes[off]); off += 4;
    if (version != kTextureCacheFormatVersion) return false;
    const u32 format = getU32(&bytes[off]); off += 4;
    if (format != static_cast<u32>(rhi::Format::BC7Unorm) &&
        format != static_cast<u32>(rhi::Format::BC7UnormSrgb)) return false;
    const u32 width  = getU32(&bytes[off]); off += 4;
    const u32 height = getU32(&bytes[off]); off += 4;
    const u32 mips   = getU32(&bytes[off]); off += 4;
    // kMaxSaneMips GUARDS mipBlocks.resize(mips) BELOW, not real content -- no texture this engine
    // builds a chain for needs more than ~15 (a 16384-wide level 0), so a `mips` this large can only
    // be a corrupt header, and resizing straight off it would ask for gigabytes of empty vector
    // shells before the very first byte-count check below ever gets to reject the file.
    constexpr u32 kMaxSaneMips = 32;
    if (width == 0 || height == 0 || mips == 0 || mips > kMaxSaneMips) return false;

    CachedTexture cand;
    cand.format = static_cast<rhi::Format>(format);
    cand.width = width; cand.height = height; cand.mips = mips;
    for (f32& c : cand.averageLinear) { c = getF32(&bytes[off]); off += 4; }
    const u32 flags = getU32(&bytes[off]); off += 4;
    cand.coveragePreserved = (flags & kFlagCoveragePreserved) != 0;

    cand.mipBlocks.resize(mips);
    u32 w = width, h = height;
    for (u32 m = 0; m < mips; ++m) {
        const u64 need = bc7MipBytes(w, h);
        if (off + need > bytes.size()) return false;   // truncated -- a miss, not an error
        cand.mipBlocks[m].assign(bytes.begin() + static_cast<isize>(off),
                                 bytes.begin() + static_cast<isize>(off + need));
        off += need;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    // ANY DISAGREEMENT IS A MISS, per the header's own contract: trailing bytes mean `mips` lied,
    // or a future writer's layout does not match this reader's -- either way "rebuild it" is the
    // honest answer, not a guess at which prefix of the file might still be good.
    if (off != bytes.size()) return false;

    out = std::move(cand);
    return true;
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

    const std::string file = dir + "\\" + cacheFileName(key);
    // See writeMutexFor's own comment for why this needs a lock at all: two threads baking the
    // SAME source concurrently would otherwise race the identical ".tmp" name.
    std::lock_guard<std::mutex> lock(writeMutexFor(key));
    return writeFileBytesAtomic(file, bytes.data(), bytes.size());
}

} // namespace aver::assets
