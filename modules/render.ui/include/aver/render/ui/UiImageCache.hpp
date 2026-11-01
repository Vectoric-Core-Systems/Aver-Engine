#pragma once
// Aver.Render.UI — UI images: a content path becomes a GPU texture the draw list can name.
//
// The id a game hands aver_ui_textured_rect (or puts in a style box) is UiRenderer::textureId(handle), so a
// UI image is an ordinary texture the cache owns. One cache per host, behind the ABI's image host callbacks
// (ui_abi.h, GameUiImages.hpp). Main thread only, like the draw list.
//
// LIFETIME. Callers acquire() an id and release() it. A texture nobody holds is not destroyed at once: the
// frame in flight, and the draw list the renderer has already copied, may still name it, so it waits
// kGraceFrames ticks first. use() is the borrowing form (a widget tree that draws a path every frame, never
// releasing): the texture lives while it keeps being asked for. Hosts tick() once per frame beside
// UiRenderer::submit, releaseAll() when a Play session ends and shutdown() before the device goes.
//
// PIXELS. The UI shader wants PREMULTIPLIED, display-space texels (no sRGB conversion, a premultiplied
// blend), so the upload is RGBA8Unorm with colour multiplied by alpha and the mip chain built from the
// premultiplied data. A straight-alpha PNG sampled raw would show dark fringes.
#include "aver/platform/Image.hpp"
#include "aver/rhi/RHI.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::render::ui {

// Largest side accepted; a UI image is not a terrain map.
inline constexpr u32 kMaxUiImageDimension = 8192;

// A decoded image as the UI draws it: premultiplied RGBA8, tightly packed, level 0 down to 1x1.
struct UiImageLevels {
    u32 width = 0;
    u32 height = 0;
    std::vector<std::vector<u8>> levels;
};

// Premultiplies `src` and builds its mip chain by box filtering. False for an invalid or oversized image.
bool buildUiImageLevels(const ImageData& src, UiImageLevels& out, std::string* err = nullptr);
// Creates the RGBA8Unorm texture holding `levels`. 0 on failure.
rhi::TextureHandle uploadUiImageLevels(rhi::IResourceFactory& res, const UiImageLevels& levels, const char* debugName);
// Decodes the file at `absPath` and uploads it. 0 on failure, with the reason in `err`; w and h are the source size.
rhi::TextureHandle uploadUiImage(rhi::IResourceFactory& res, const std::string& absPath, u32& w, u32& h, std::string& err);

// The refcounted, path-keyed texture table.
class UiImageCache {
public:
    // Frames an unreferenced texture waits before it is destroyed: the renderer's five-deep buffer ring, plus slack.
    static constexpr u32 kGraceFrames = 8;

    // Makes the texture for a (normalised, content-relative) path; 0 with `err` set on failure. Fills the source size.
    using LoadFn = std::function<rhi::TextureHandle(const std::string& path, u32& w, u32& h, std::string& err)>;
    // Destroys a texture the cache made (the host also forgets the UI renderer's binding for it).
    using DestroyFn = std::function<void(rhi::TextureHandle)>;

    UiImageCache(LoadFn load, DestroyFn destroy);
    ~UiImageCache();
    UiImageCache(const UiImageCache&) = delete;
    UiImageCache& operator=(const UiImageCache&) = delete;

    // `path` as the cache keys it: forward slashes, no "./" or empty segments. Empty for a path that is not
    // strictly inside the content folder (absolute, drive or scheme, "..", control characters, too long).
    static std::string normalizePath(const std::string& path);

    // The id of the texture for `path` with one more reference, loading it on first use. 0 when the path is
    // unsafe or the image does not load (a failure is remembered, so it is logged and tried once, until releaseAll).
    u64 acquire(const std::string& path);
    // The id for `path` without a reference: valid while it keeps being asked for within kGraceFrames ticks.
    u64 use(const std::string& path);
    // Drops one reference. False for an id the cache does not hold or one with none left.
    bool release(u64 id);
    // The source image size in pixels. False for an unknown id.
    bool size(u64 id, u32& w, u32& h) const;

    // Once per frame: destroys what has had no reference and no use for kGraceFrames ticks.
    void tick();
    // Drops every reference (a Play session ending) and forgets remembered failures. Textures go at their grace.
    void releaseAll();
    // Destroys everything now. The device must be idle or about to be destroyed; the destructor does it too.
    void shutdown();

    u32 textureCount() const { return static_cast<u32>(byPath_.size()); }
    // References held on `id`; 0 for an unknown id.
    u32 references(u64 id) const;

private:
    struct Entry {
        std::string path;
        rhi::TextureHandle texture = 0;
        u32 width = 0, height = 0;
        u32 refs = 0;
        u64 lastUse = 0;   // the frame it was last acquired, used or released
    };
    Entry* find(const std::string& path);
    Entry* load(const std::string& path);
    void destroy(Entry& e);

    LoadFn load_;
    DestroyFn destroy_;
    u64 frame_ = 0;
    std::unordered_map<std::string, std::unique_ptr<Entry>> byPath_;
    std::unordered_map<u64, Entry*> byId_;
    std::unordered_set<std::string> failed_;
};

} // namespace aver::render::ui
