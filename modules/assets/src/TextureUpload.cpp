// Decodes an image file and creates the GPU texture for it.
#include "aver/assets/TextureUpload.hpp"

#include "TextureUploadDetail.hpp"
#include "aver/assets/TextureCache.hpp"
#include "aver/core/HitchMarks.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/Bc7.hpp"
#include "aver/formats/Texture.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace aver::assets {

namespace {

// The alpha-test reference coverage is preserved AT, shared with the BC7 encoder's own cutout
// weighting below (Bc7Options::cutoff defaults to 128, which IS 0.5 in the u8 scale
// generateMipChain's preserveCoverageAt works in) -- one number, so the mip chain and the block
// encoder agree on where "the leaf's edge" actually is.
constexpr f32 kCoverageReferenceAlpha = 0.5f;

// The last level is 1x1 (generateMipChain loops until both extents are 1). Its texel is stored in
// the texture's OWN encoding, so an sRGB texture needs decoding back to linear -- downsample()
// averaged in linear and then re-encoded, and handing the encoded byte straight out would report a
// colour roughly twice as bright as the texture really is. Shared by every upload path (RGBA8 and
// BC7 alike): the mean is read off `tex`'s own decoded chain before either path throws it away.
void averageLinearFromLastMip(const fmt::TextureData& tex, f32 out[3]) {
    const ImageData& last = tex.levels.back();
    if (last.pixels.size() < 4) return;
    for (int c = 0; c < 3; ++c) {
        const f32 v = static_cast<f32>(last.pixels[c]) / 255.0f;
        out[c] = tex.srgb ? (v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f)) : v;
    }
}

// "Any failure falls back to today's RGBA8 path, logged once per file": a texture that fails to
// compress on every resolve (a material preview re-requesting the same fallback icon, say) must
// not spam the log once per resolve. Keyed on the raw path string, which is exactly what the
// caller already deduplicates ON when a hit skips this file entirely next time.
std::mutex g_loggedFailuresMutex;
std::unordered_set<std::string> g_loggedFailures;
void logCompressionFailureOnce(const std::string& path, std::string_view why) {
    {
        std::lock_guard<std::mutex> lock(g_loggedFailuresMutex);
        if (!g_loggedFailures.insert(path).second) return;
    }
    AVER_WARN("[Texture] {} will not compress ({}); using uncompressed RGBA8", path, why);
}

// Uploads an already-built BC7 chain (either just baked or read straight from the cache) as a
// texture. 0 on failure, with nothing to undo either way: the caller still holds whatever it needs
// to fall back.
rhi::TextureHandle uploadCachedTexture(rhi::IResourceFactory& res, const std::string& path,
                                       const CachedTexture& cached, TextureUploadInfo* info) {
    std::vector<const void*> levels;
    levels.reserve(cached.mipBlocks.size());
    usize bytes = 0;
    for (const std::vector<u8>& mip : cached.mipBlocks) {
        levels.push_back(mip.data());
        bytes += mip.size();
    }

    rhi::TextureDesc d;
    d.width  = cached.width;
    d.height = cached.height;
    d.mips   = cached.mips;
    d.format = cached.format;
    d.bind   = rhi::ResourceBind::ShaderResource;
    d.initialState = rhi::ResourceState::ShaderResource;
    d.initialData      = levels.data();
    d.initialDataCount = static_cast<u32>(levels.size());
    d.initialRowPitch  = 0;   // every level is tightly packed BC7 blocks; see packedRowPitch/Vk
    d.debugName = path.c_str();

    const rhi::TextureHandle h = res.createTexture(d);
    if (!h) return 0;

    if (info) {
        *info = TextureUploadInfo{cached.width, cached.height, cached.mips, bytes};
        for (int c = 0; c < 3; ++c) info->averageLinear[c] = cached.averageLinear[c];
    }
    return h;
}

// `tex` already holds a full decoded RGBA8 mip chain (coverage-preserved already, if `cutout` asked
// for it). Encodes every level to BC7 into `cached` and writes the cache entry.
void compressToCache(const std::string& path, TextureUsage usage, const fmt::TextureData& tex, bool cutout,
                     CachedTexture& cached) {
    const auto t0 = std::chrono::steady_clock::now();

    fmt::Bc7Options bc7opt;
    bc7opt.cutout = cutout;   // default cutoff (128) IS kCoverageReferenceAlpha in the u8 scale

    cached.format = tex.srgb ? rhi::Format::BC7UnormSrgb : rhi::Format::BC7Unorm;
    cached.width  = tex.width;
    cached.height = tex.height;
    cached.mips   = static_cast<u32>(tex.levels.size());
    cached.coveragePreserved = cutout;
    averageLinearFromLastMip(tex, cached.averageLinear);

    cached.mipBlocks.resize(tex.levels.size());
    for (usize m = 0; m < tex.levels.size(); ++m)
        fmt::encodeBc7Image(tex.levels[m], cached.mipBlocks[m], bc7opt);

    const auto t1 = std::chrono::steady_clock::now();
    usize totalBytes = 0;
    for (const std::vector<u8>& mip : cached.mipBlocks) totalBytes += mip.size();
    AVER_INFO("[Texture] compressed {} to BC7: {} KiB ({} ms)", path, totalBytes / 1024,
              std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    if (!saveCachedTexture(path, usage, cached))
        AVER_WARN("[Texture] {} compressed but could not be written to the derived-data cache", path);
}

// The miss path: encodes, caches, then uploads. 0 on failure -- the caller falls back to the RGBA8
// chain it already decoded, so nothing here needs to be undone.
rhi::TextureHandle uploadCompressed(rhi::IResourceFactory& res, const std::string& path,
                                    TextureUsage usage, const fmt::TextureData& tex, bool cutout,
                                    TextureUploadInfo* info) {
    CachedTexture cached;
    compressToCache(path, usage, tex, cutout, cached);
    return uploadCachedTexture(res, path, cached, info);
}

// ---- streaming prefetch (prefetchTexture) ----
std::mutex g_heldMutex;
std::condition_variable g_heldCv;
struct Held { CachedTexture tex; u64 order = 0, bytes = 0, size = 0; i64 mtime = 0; std::chrono::steady_clock::time_point at; };
std::unordered_map<std::string, Held> g_held;
std::unordered_set<std::string> g_reading;
// Nothing to prefetch: uploaded already, or not a BC7 texture. A texture released and needed again
// later uploads from disk on the main thread, as before prefetching existed.
std::unordered_set<std::string> g_skip;
u64 g_heldBytes = 0, g_heldOrder = 0;
constexpr u64 kMaxHeldBytes = 1024ull << 20;

std::string heldKey(const std::string& path, TextureUsage usage) {
    std::string k = path;
#ifdef _WIN32
    for (char& c : k) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    k += '|';
    k += static_cast<char>('0' + static_cast<u32>(usage));
    return k;
}

// The file's identity, so a held chain read before a re-import is not used after it.
bool fileStamp(const std::string& path, u64& size, i64& mtime) {
    std::error_code ec;
    size = static_cast<u64>(std::filesystem::file_size(path, ec));
    if (ec) return false;
    mtime = static_cast<i64>(std::filesystem::last_write_time(path, ec).time_since_epoch().count());
    return !ec;
}

bool takeHeld(const std::string& path, TextureUsage usage, CachedTexture& out) {
    std::lock_guard<std::mutex> l(g_heldMutex);
    const auto it = g_held.find(heldKey(path, usage));
    if (it == g_held.end()) return false;
    u64 size = 0;
    i64 mtime = 0;
    const bool same = fileStamp(path, size, mtime) && size == it->second.size && mtime == it->second.mtime;
    if (same) out = std::move(it->second.tex);
    g_heldBytes -= it->second.bytes;
    g_held.erase(it);
    return same;
}

void noteUploaded(const std::string& path, TextureUsage usage) {
    std::lock_guard<std::mutex> l(g_heldMutex);
    g_skip.insert(heldKey(path, usage));
}

} // namespace

// ---- the decode / resize / mip helpers shared with the atlas builder (TextureUploadDetail.hpp) ----
namespace detail {

bool decodeTexture(const std::string& path, TextureUsage usage, fmt::TextureData& out, std::string* err) {
    fmt::TextureLoadOptions opt;
    opt.srgb        = usage == TextureUsage::Colour;
    opt.normalMap   = usage == TextureUsage::NormalMap;
    opt.generateMips = false;   // the caller decides how mips are built
    return fmt::loadTexture(path, out, opt, err);
}

void buildMips(fmt::TextureData& t, TextureUsage usage) {
    fmt::generateMipChain(t, usage == TextureUsage::NormalMap);
}

namespace {
// sRGB <-> linear, exact piecewise curve; a 256-entry table for the decode.
const f32* srgbTable() {
    static const std::array<f32, 256> t = [] {
        std::array<f32, 256> a{};
        for (int i = 0; i < 256; ++i) {
            const f32 c = static_cast<f32>(i) / 255.0f;
            a[static_cast<usize>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return a;
    }();
    return t.data();
}
u8 encodeSrgb(f32 c) {
    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(s * 255.0f + 0.5f);
}
} // namespace

void resizeBox(const ImageData& src, u32 w, u32 h, TextureUsage usage, ImageData& out) {
    out = ImageData{};
    out.width = w; out.height = h; out.channels = src.channels; out.srgb = src.srgb;
    out.pixels.assign(static_cast<usize>(w) * h * 4, 255);
    if (!src.valid() || w == 0 || h == 0) return;
    const bool lin = usage == TextureUsage::Colour;
    const f32* tab = srgbTable();
    const auto dec = [&](u8 v) { return lin ? tab[v] : static_cast<f32>(v) * (1.0f / 255.0f); };
    const auto enc = [&](f32 v) {
        if (lin) return encodeSrgb(v);
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<u8>(v * 255.0f + 0.5f);
    };
    const auto px = [&](i64 x, i64 y) {
        const i64 sx = ((x % src.width) + src.width) % src.width, sy = ((y % src.height) + src.height) % src.height;
        return &src.pixels[(static_cast<usize>(sy) * src.width + static_cast<usize>(sx)) * 4];
    };
    const f32 kx = static_cast<f32>(src.width) / static_cast<f32>(w);
    const f32 ky = static_cast<f32>(src.height) / static_cast<f32>(h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            f32 acc[4] = {0, 0, 0, 0};
            if (kx >= 1.0f && ky >= 1.0f) {   // shrinking: area average with fractional edge weights
                const f32 x0 = static_cast<f32>(x) * kx, x1 = x0 + kx, y0 = static_cast<f32>(y) * ky, y1 = y0 + ky;
                f32 wsum = 0.0f;
                for (i64 sy = static_cast<i64>(std::floor(y0)); sy < static_cast<i64>(std::ceil(y1)); ++sy) {
                    const f32 wy = std::min(static_cast<f32>(sy + 1), y1) - std::max(static_cast<f32>(sy), y0);
                    for (i64 sx = static_cast<i64>(std::floor(x0)); sx < static_cast<i64>(std::ceil(x1)); ++sx) {
                        const f32 wx = std::min(static_cast<f32>(sx + 1), x1) - std::max(static_cast<f32>(sx), x0);
                        const f32 wt = wx * wy;
                        const u8* p = px(sx, sy);
                        for (int c = 0; c < 3; ++c) acc[c] += dec(p[c]) * wt;
                        acc[3] += static_cast<f32>(p[3]) * (1.0f / 255.0f) * wt;
                        wsum += wt;
                    }
                }
                if (wsum > 0.0f) for (f32& a : acc) a /= wsum;
            } else {                          // growing: bilinear, wrapping
                const f32 fx = (static_cast<f32>(x) + 0.5f) * kx - 0.5f, fy = (static_cast<f32>(y) + 0.5f) * ky - 0.5f;
                const i64 ix = static_cast<i64>(std::floor(fx)), iy = static_cast<i64>(std::floor(fy));
                const f32 tx = fx - static_cast<f32>(ix), ty = fy - static_cast<f32>(iy);
                const f32 wts[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                const u8* p[4] = {px(ix, iy), px(ix + 1, iy), px(ix, iy + 1), px(ix + 1, iy + 1)};
                for (int t = 0; t < 4; ++t) {
                    for (int c = 0; c < 3; ++c) acc[c] += dec(p[t][c]) * wts[t];
                    acc[3] += static_cast<f32>(p[t][3]) * (1.0f / 255.0f) * wts[t];
                }
            }
            u8* o = &out.pixels[(static_cast<usize>(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) o[c] = enc(acc[c]);
            o[3] = static_cast<u8>(std::clamp(acc[3], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

} // namespace detail

// Decodes `path` and uploads it. Returns 0 on failure, with the reason in `err`.
rhi::TextureHandle uploadTexture(rhi::IResourceFactory& res, const std::string& path,
                                 TextureUsage usage, std::string* err, TextureUploadInfo* info) {
    if (path.empty()) {
        if (err) *err = "empty texture path";
        return 0;
    }

    // Both gates read once: a project must have pointed the cache somewhere (setTextureCacheDir)
    // AND the A/B switch must be on, or nothing below this file even looks for an entry -- the
    // legacy, always-uncompressed behaviour for a host that never calls setTextureCacheDir at all.
    const bool cacheReady = textureCompressionEnabled() && !textureCacheDir().empty();

    // ---- CACHE HIT: no image decode at all -- see TextureCache.hpp for the whole point of this. ----
    if (cacheReady) {
        CachedTexture cached;
        const auto t0 = std::chrono::steady_clock::now();
        const bool held = takeHeld(path, usage, cached);
        if (held || loadCachedTexture(path, usage, cached)) {
            if (const rhi::TextureHandle h = uploadCachedTexture(res, path, cached, info)) {
                noteUploaded(path, usage);
                if (hitchVerbose())
                    AVER_INFO("[Texture] {} {} in {:.1f} ms", held ? "prefetched" : "read here", path,
                              std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
                return h;
            }
            // A cached entry the RHI refuses (a stale format from an older build, say) is not
            // repaired here -- fall through and decode as if it had been a miss.
            logCompressionFailureOnce(path, "the cached BC7 chain was rejected by the GPU");
        }
    }

    // Decoded with mips OFF, deliberately: this function decides mip generation itself, below, so it can
    // pass alphaLooksLikeCutout's verdict into generateMipChain -- loadTexture's own generateMips path
    // has no way to ask for that.
    const bool normalMap = usage == TextureUsage::NormalMap;

    fmt::TextureData tex;
    if (!detail::decodeTexture(path, usage, tex, err)) return 0;
    if (!tex.valid()) {
        if (err) *err = "decoded texture is empty: " + path;
        return 0;
    }

    // ELIGIBILITY IS DECIDED BEFORE A SINGLE MIP IS BUILT, deliberately: with compression off or
    // this texture ineligible (a small UI icon), everything past here -- including whether
    // coverage-preserving alpha runs at all -- must stay BYTE-IDENTICAL to the plain RGBA8 path
    // this function always had, or AVER_TEXTURE_COMPRESSION=0 would not be the clean A/B switch it
    // claims to be: it would also be silently comparing two different mip chains.
    const bool willCompress = cacheReady && textureCompressionEligible(tex.width, tex.height);

    // COVERAGE-PRESERVING MIPS, COLOUR CUTOUTS ONLY, AND ONLY WHEN THIS TEXTURE WILL ACTUALLY BE
    // COMPRESSED: a normal map's alpha is not a coverage channel (generateMipChain never treats it
    // as one) and Data usage (ORM, height, masks) is sampled directly rather than alpha-tested, so
    // neither has a coverage fraction worth preserving either way.
    const bool cutout = willCompress && usage == TextureUsage::Colour &&
                        fmt::alphaLooksLikeCutout(tex.levels.front());
    fmt::generateMipChain(tex, normalMap, cutout ? kCoverageReferenceAlpha : -1.0f);

    if (willCompress) {
        if (const rhi::TextureHandle h = uploadCompressed(res, path, usage, tex, cutout, info)) {
            noteUploaded(path, usage);
            return h;
        }
        logCompressionFailureOnce(path, "the compressed GPU texture could not be created");
    }

    // ---- the RGBA8 path: unchanged from before this feature existed. ----
    // One CPU pointer per mip, in level order, which is the subresource order TextureDesc documents.
    std::vector<const void*> levels;
    levels.reserve(tex.levels.size());
    usize bytes = 0;
    for (const ImageData& lvl : tex.levels) {
        levels.push_back(lvl.pixels.data());
        bytes += lvl.pixels.size();
    }

    rhi::TextureDesc d;
    d.width  = tex.width;
    d.height = tex.height;
    d.mips   = static_cast<u32>(levels.size());
    d.format = tex.srgb ? rhi::Format::RGBA8UnormSrgb : rhi::Format::RGBA8Unorm;
    d.bind   = rhi::ResourceBind::ShaderResource;
    d.initialState = rhi::ResourceState::ShaderResource;
    d.initialData      = levels.data();
    d.initialDataCount = static_cast<u32>(levels.size());
    d.initialRowPitch  = tex.levels.front().rowPitch();
    d.debugName = path.c_str();

    const rhi::TextureHandle h = res.createTexture(d);
    if (!h) {
        if (err) *err = "the GPU texture could not be created: " + path;
        return 0;
    }
    if (info) {
        *info = TextureUploadInfo{tex.width, tex.height, d.mips, bytes};
        averageLinearFromLastMip(tex, info->averageLinear);
    }
    return h;
}

bool prefetchTexture(const std::string& path, TextureUsage usage) {
    if (path.empty() || !textureCompressionEnabled() || textureCacheDir().empty()) return false;
    const std::string key = heldKey(path, usage);
    {
        std::unique_lock<std::mutex> l(g_heldMutex);
        g_heldCv.wait(l, [&] { return !g_reading.count(key); });
        if (const auto h = g_held.find(key); h != g_held.end()) { h->second.at = std::chrono::steady_clock::now(); return true; }
        if (g_skip.count(key)) return false;
        g_reading.insert(key);
    }
    u64 size = 0;
    i64 mtime = 0;
    CachedTexture cached;
    bool ok = fileStamp(path, size, mtime) && loadCachedTexture(path, usage, cached);
    bool skip = false;
    if (!ok) {
        // A first visit: the decode and BC7 encode the upload's miss path would do on the main thread.
        fmt::TextureData tex;
        if (detail::decodeTexture(path, usage, tex, nullptr) && tex.valid() &&
            textureCompressionEligible(tex.width, tex.height)) {
            const bool cutout = usage == TextureUsage::Colour && fmt::alphaLooksLikeCutout(tex.levels.front());
            fmt::generateMipChain(tex, usage == TextureUsage::NormalMap, cutout ? kCoverageReferenceAlpha : -1.0f);
            compressToCache(path, usage, tex, cutout, cached);
            ok = true;
        } else {
            skip = tex.valid();   // decodes but is not BC7: the upload's RGBA8 path is cheap enough
        }
    }
    std::lock_guard<std::mutex> l(g_heldMutex);
    g_reading.erase(key);
    g_heldCv.notify_all();
    if (skip) g_skip.insert(key);
    if (!ok) return false;
    if (g_skip.count(key)) return true;   // uploaded on the main thread while this read ran
    u64 bytes = 0;
    for (const std::vector<u8>& m : cached.mipBlocks) bytes += m.size();
    g_heldBytes += bytes;
    g_held[key] = Held{std::move(cached), ++g_heldOrder, bytes, size, mtime, std::chrono::steady_clock::now()};
    while (g_heldBytes > kMaxHeldBytes && g_held.size() > 1) {
        auto oldest = g_held.begin();
        for (auto it = g_held.begin(); it != g_held.end(); ++it)
            if (it->second.order < oldest->second.order) oldest = it;
        g_heldBytes -= oldest->second.bytes;
        g_held.erase(oldest);
    }
    return true;
}

void dropPrefetchedTextures() {
    std::lock_guard<std::mutex> l(g_heldMutex);
    g_held.clear();
    g_skip.clear();
    g_heldBytes = 0;
}

void trimPrefetchedTextures() {
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(30);
    std::lock_guard<std::mutex> l(g_heldMutex);
    for (auto it = g_held.begin(); it != g_held.end();) {
        if (it->second.at >= cutoff) { ++it; continue; }
        g_heldBytes -= it->second.bytes;
        it = g_held.erase(it);
    }
}

void forgetUploadedTextures() {
    std::lock_guard<std::mutex> l(g_heldMutex);
    g_skip.clear();
}

u64 prefetchedTextureBytes() {
    std::lock_guard<std::mutex> l(g_heldMutex);
    return g_heldBytes;
}

} // namespace aver::assets
