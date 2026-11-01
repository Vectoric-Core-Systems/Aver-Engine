// UiImageCache: UI images by content path - the premultiplied upload and the refcounted texture table.
#include "aver/render/ui/UiImageCache.hpp"
#include "aver/render/ui/UiRenderer.hpp"

#include "aver/core/Log.hpp"

#include <utility>

namespace aver::render::ui {
namespace {

constexpr usize kMaxPathBytes = 1024;

// Straight to premultiplied, rounded.
inline u8 premul(u32 c, u32 a) { return static_cast<u8>((c * a + 127u) / 255u); }

// One box-filter halving of premultiplied RGBA8. An odd side repeats its last row or column.
std::vector<u8> halve(const std::vector<u8>& src, u32 w, u32 h, u32& nw, u32& nh) {
    nw = w > 1 ? w / 2 : 1;
    nh = h > 1 ? h / 2 : 1;
    std::vector<u8> dst(static_cast<usize>(nw) * nh * 4);
    for (u32 y = 0; y < nh; ++y) {
        const u32 y0 = y * 2 < h ? y * 2 : h - 1, y1 = y0 + 1 < h ? y0 + 1 : y0;
        for (u32 x = 0; x < nw; ++x) {
            const u32 x0 = x * 2 < w ? x * 2 : w - 1, x1 = x0 + 1 < w ? x0 + 1 : x0;
            const u8* p[4] = {&src[(static_cast<usize>(y0) * w + x0) * 4], &src[(static_cast<usize>(y0) * w + x1) * 4],
                              &src[(static_cast<usize>(y1) * w + x0) * 4], &src[(static_cast<usize>(y1) * w + x1) * 4]};
            u8* o = &dst[(static_cast<usize>(y) * nw + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = static_cast<u8>((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2u) / 4u);
        }
    }
    return dst;
}

} // namespace

bool buildUiImageLevels(const ImageData& src, UiImageLevels& out, std::string* err) {
    out = UiImageLevels{};
    if (!src.valid()) { if (err) *err = "the image is empty"; return false; }
    if (src.width > kMaxUiImageDimension || src.height > kMaxUiImageDimension) {
        if (err) *err = "the image is " + std::to_string(src.width) + "x" + std::to_string(src.height) + ", over the UI limit of " + std::to_string(kMaxUiImageDimension);
        return false;
    }
    out.width = src.width;
    out.height = src.height;
    std::vector<u8> base(src.pixels.size());
    for (usize i = 0; i < src.pixels.size(); i += 4) {
        const u32 a = src.pixels[i + 3];
        base[i + 0] = premul(src.pixels[i + 0], a);
        base[i + 1] = premul(src.pixels[i + 1], a);
        base[i + 2] = premul(src.pixels[i + 2], a);
        base[i + 3] = static_cast<u8>(a);
    }
    u32 w = src.width, h = src.height;
    out.levels.push_back(std::move(base));
    while (w > 1 || h > 1) {
        u32 nw = 0, nh = 0;
        std::vector<u8> next = halve(out.levels.back(), w, h, nw, nh);
        out.levels.push_back(std::move(next));
        w = nw;
        h = nh;
    }
    return true;
}

rhi::TextureHandle uploadUiImageLevels(rhi::IResourceFactory& res, const UiImageLevels& levels, const char* debugName) {
    if (levels.levels.empty() || levels.width == 0 || levels.height == 0) return 0;
    std::vector<const void*> ptrs;
    ptrs.reserve(levels.levels.size());
    for (const std::vector<u8>& l : levels.levels) ptrs.push_back(l.data());

    rhi::TextureDesc d;
    d.width = levels.width;
    d.height = levels.height;
    d.mips = static_cast<u32>(ptrs.size());
    d.format = rhi::Format::RGBA8Unorm;   // NOT sRGB: the shader blends in display space (see ui.hlsl)
    d.bind = rhi::ResourceBind::ShaderResource;
    d.initialState = rhi::ResourceState::ShaderResource;
    d.initialData = ptrs.data();
    d.initialDataCount = static_cast<u32>(ptrs.size());
    d.initialRowPitch = 0;   // every level tightly packed
    d.debugName = debugName;
    return res.createTexture(d);
}

rhi::TextureHandle uploadUiImage(rhi::IResourceFactory& res, const std::string& absPath, u32& w, u32& h, std::string& err) {
    ImageData img;
    if (!decodeImage(absPath, img, &err)) return 0;
    UiImageLevels levels;
    if (!buildUiImageLevels(img, levels, &err)) return 0;
    w = img.width;
    h = img.height;
    const rhi::TextureHandle t = uploadUiImageLevels(res, levels, absPath.c_str());
    if (!t) err = "the GPU texture could not be created";
    return t;
}

// ---- the cache ----

UiImageCache::UiImageCache(LoadFn load, DestroyFn destroy) : load_(std::move(load)), destroy_(std::move(destroy)) {}

UiImageCache::~UiImageCache() { shutdown(); }

std::string UiImageCache::normalizePath(const std::string& path) {
    if (path.empty() || path.size() > kMaxPathBytes) return {};
    std::string out;
    out.reserve(path.size());
    usize i = 0;
    while (i <= path.size()) {
        usize j = i;
        while (j < path.size() && path[j] != '/' && path[j] != '\\') ++j;
        const std::string seg = path.substr(i, j - i);
        if (seg == "..") return {};
        for (char c : seg) if (static_cast<unsigned char>(c) < 0x20 || c == ':') return {};
        if (!seg.empty() && seg != ".") {
            if (!out.empty()) out += '/';
            out += seg;
        } else if (seg.empty() && i == 0 && j < path.size()) {
            return {};   // a leading separator: absolute
        }
        i = j + 1;
    }
    return out;
}

UiImageCache::Entry* UiImageCache::find(const std::string& path) {
    const auto it = byPath_.find(path);
    return it == byPath_.end() ? nullptr : it->second.get();
}

UiImageCache::Entry* UiImageCache::load(const std::string& path) {
    if (failed_.count(path) || !load_) return nullptr;
    u32 w = 0, h = 0;
    std::string err;
    const rhi::TextureHandle t = load_(path, w, h, err);
    if (!t) {
        AVER_WARN("[UI] image '{}': {}", path, err.empty() ? "it could not be loaded" : err);
        failed_.insert(path);
        return nullptr;
    }
    auto e = std::make_unique<Entry>();
    e->path = path;
    e->texture = t;
    e->width = w;
    e->height = h;
    e->lastUse = frame_;
    Entry* raw = e.get();
    byId_[UiRenderer::textureId(t)] = raw;
    byPath_[path] = std::move(e);
    return raw;
}

u64 UiImageCache::acquire(const std::string& path) {
    const std::string key = normalizePath(path);
    if (key.empty()) return 0;
    Entry* e = find(key);
    if (!e) e = load(key);
    if (!e) return 0;
    ++e->refs;
    e->lastUse = frame_;
    return UiRenderer::textureId(e->texture);
}

u64 UiImageCache::use(const std::string& path) {
    const std::string key = normalizePath(path);
    if (key.empty()) return 0;
    Entry* e = find(key);
    if (!e) e = load(key);
    if (!e) return 0;
    e->lastUse = frame_;
    return UiRenderer::textureId(e->texture);
}

bool UiImageCache::release(u64 id) {
    const auto it = byId_.find(id);
    if (it == byId_.end() || it->second->refs == 0) return false;
    Entry* e = it->second;
    if (--e->refs == 0) e->lastUse = frame_;
    return true;
}

bool UiImageCache::size(u64 id, u32& w, u32& h) const {
    const auto it = byId_.find(id);
    if (it == byId_.end()) return false;
    w = it->second->width;
    h = it->second->height;
    return true;
}

u32 UiImageCache::references(u64 id) const {
    const auto it = byId_.find(id);
    return it == byId_.end() ? 0 : it->second->refs;
}

void UiImageCache::destroy(Entry& e) {
    byId_.erase(UiRenderer::textureId(e.texture));
    if (destroy_) destroy_(e.texture);
    e.texture = 0;
}

void UiImageCache::tick() {
    ++frame_;
    for (auto it = byPath_.begin(); it != byPath_.end();) {
        Entry& e = *it->second;
        if (e.refs == 0 && frame_ - e.lastUse >= kGraceFrames) {
            destroy(e);
            it = byPath_.erase(it);
        } else {
            ++it;
        }
    }
}

void UiImageCache::releaseAll() {
    for (auto& kv : byPath_) {
        kv.second->refs = 0;
        kv.second->lastUse = frame_;
    }
    failed_.clear();
}

void UiImageCache::shutdown() {
    for (auto& kv : byPath_) destroy(*kv.second);
    byPath_.clear();
    byId_.clear();
    failed_.clear();
}

} // namespace aver::render::ui
