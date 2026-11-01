// GameUiImages: the host half of the UI image service (modules/render.ui UiImageCache, ui_abi.h "images").
// A game names a picture by its content path; this decodes it, uploads it premultiplied and lends the DLL
// the table that turns the path into the texture id aver_ui_textured_rect and the style boxes take. Shared by
// the editor's Play/Preview and the packaged game for the reason GameUiInput.hpp is: two copies drift.
//
//   uiImagesBind(device, r)    once, after UiRenderer::create: makes the table and installs it in the ABI
//   uiImagesSetContent(dir)    when a project opens: its Content folder (a different one drops every image)
//   uiImagesTick()             once per frame, beside UiRenderer::submit: destroys images nobody holds any more
//   uiImagesReleaseAll()       when a Play session ends: drops every reference a game still held
//   uiImagesShutdown()         before the renderer and device go: uninstalls the table and destroys the images
//
// LIFETIME. A texture is destroyed UiImageCache::kGraceFrames ticks after its last reference or use, never
// while the frame in flight or the renderer's copy of the last draw list may still name it, and the UI
// renderer's binding set for it is dropped first. Files are opened through the traced API like project fonts.
//
// HEADER-ONLY, evaluated per host with its own AVER_WITH_UI_ABI.
#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver::rhi { class IDevice; }
namespace aver::render::ui { class UiRenderer; }

#if AVER_WITH_UI_ABI
#  include "aver/core/Log.hpp"
#  include "aver/platform/FileSystem.hpp"
#  include "aver/render/ui/UiImageCache.hpp"
#  include "aver/render/ui/UiRenderer.hpp"
#  include "aver/ui/UiDrawList.hpp"
#  include "aver/ui/ui_abi.h"
#  include <memory>
#endif

namespace aver::game {

#if AVER_WITH_UI_ABI
namespace uiimg_detail {

struct State {
    std::string contentDir;
    rhi::IResourceFactory* res = nullptr;
    render::ui::UiRenderer* renderer = nullptr;
    std::unique_ptr<render::ui::UiImageCache> cache;
};

inline State& state() {
    static State s;
    return s;
}

inline std::string joinPath(const std::string& dir, const std::string& rel) {
    std::string p = dir;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += AVER_PATH_SEP;
    p += rel;
    toNativeSeparators(p);
    return p;
}

// The cache's loader. `path` is already normalised and inside Content.
inline rhi::TextureHandle loadImage(const std::string& path, u32& w, u32& h, std::string& err) {
    State& s = state();
    if (!s.res) { err = "there is no GPU device"; return 0; }
    if (s.contentDir.empty()) { err = "no project is open"; return 0; }
    const std::string abs = joinPath(s.contentDir, path);
    if (!fileExists(abs)) { err = "no such file under Content (" + abs + ")"; return 0; }
    traceFileOpen(abs);
    return render::ui::uploadUiImage(*s.res, abs, w, h, err);
}

// The renderer forgets its binding for the texture first: a handle can be reused, and the binding would then sample the old one.
inline void destroyImage(rhi::TextureHandle t) {
    State& s = state();
    if (s.renderer) s.renderer->dropBinding(t);
    if (s.res) s.res->destroyTexture(t);
}

inline uint64_t acquireCb(const char* path) { State& s = state(); return s.cache && path ? s.cache->acquire(path) : 0; }
inline uint64_t useCb(const char* path) { State& s = state(); return s.cache && path ? s.cache->use(path) : 0; }
inline void releaseCb(uint64_t id) { State& s = state(); if (s.cache) s.cache->release(id); }
inline int32_t sizeCb(uint64_t id, int32_t* outWH) {
    State& s = state();
    u32 w = 0, h = 0;
    if (!s.cache || !outWH || !s.cache->size(id, w, h)) return 0;
    outWH[0] = static_cast<int32_t>(w);
    outWH[1] = static_cast<int32_t>(h);
    return 1;
}

// Destroys every image now. Idle first, and the renderer's copy of the last list emptied: both could still name them.
inline void destroyAll() {
    State& s = state();
    if (!s.cache || s.cache->textureCount() == 0) return;
    if (s.renderer) s.renderer->submit(ui::UiDrawList{});
    if (s.res) s.res->waitIdle();
    s.cache->shutdown();
}

} // namespace uiimg_detail
#endif

// Makes the image table for a device whose UI renderer is `renderer` and installs it in the ABI. A null renderer
// (the UI declined to create) installs nothing: there is nothing to draw an image with.
inline void uiImagesBind(rhi::IDevice* device, render::ui::UiRenderer* renderer) {
#if AVER_WITH_UI_ABI
    using namespace uiimg_detail;
    State& s = state();
    s.renderer = renderer;
    s.res = device ? device->resources() : nullptr;
    if (!renderer || !s.res) return;
    s.cache = std::make_unique<render::ui::UiImageCache>(&loadImage, &destroyImage);
    aver_ui_set_image_host(&acquireCb, &releaseCb, &sizeCb, &useCb);
#else
    (void)device; (void)renderer;
#endif
}

// The project's Content folder. A different one drops every image: the same path now names another file.
inline void uiImagesSetContent(const std::string& contentDir) {
#if AVER_WITH_UI_ABI
    uiimg_detail::State& s = uiimg_detail::state();
    if (s.contentDir == contentDir) return;
    uiimg_detail::destroyAll();
    s.contentDir = contentDir;
#else
    (void)contentDir;
#endif
}

inline void uiImagesTick() {
#if AVER_WITH_UI_ABI
    if (uiimg_detail::state().cache) uiimg_detail::state().cache->tick();
#endif
}

inline void uiImagesReleaseAll() {
#if AVER_WITH_UI_ABI
    if (uiimg_detail::state().cache) uiimg_detail::state().cache->releaseAll();
#endif
}

// Uninstalls the table and destroys the images. BEFORE the UI renderer and the device go.
inline void uiImagesShutdown() {
#if AVER_WITH_UI_ABI
    uiimg_detail::State& s = uiimg_detail::state();
    aver_ui_set_image_host(nullptr, nullptr, nullptr, nullptr);
    uiimg_detail::destroyAll();
    s.cache.reset();
    s.renderer = nullptr;
    s.res = nullptr;
    s.contentDir.clear();
#endif
}

} // namespace aver::game
