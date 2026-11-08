// GameScene2D: the host half of 2D scenes (docs/plans/RENDER_2D.md). Sprites are scene components; this feeds the canvas what only a
// host knows (the viewport rectangle, the way a sprite's asset id becomes a UI image) and flushes it into the UI draw list once a
// frame. Shared by the editor's Play and the packaged game for the reason GameUiImages.hpp is: two copies drift.
//
//   scene2dBind(pathOf)      once the content index exists: installs the sprite texture resolver (asset id -> content path -> UI image)
//   scene2dUnbind()          before the UI image table goes
//   scene2dFrame(w, list, v) the testable core: view rectangle, the time step (animations, parallax, camera), then sprites + queued
//                            external items sorted into `list`
//   scene2dSubmit(w)         the host's call, after the game's scripts ran and before the UI renderer takes the list; steps time by the wall
//                            clock between its calls (at most a tenth of a second, so a stall does not skip a camera across the level)
//
// Sprites go through the same draw list as the game UI, so they draw only when the host draws the game UI (Play, not the editor's
// edit mode): a host calls scene2dSubmit only then, and scene2dDrop otherwise so nothing queues forever.
//
// HEADER-ONLY, evaluated per host with its own AVER_WITH_UI_ABI.
#pragma once
#include "aver/core/Types.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace aver::scene { class World; }

#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
#  include "aver/scene/Canvas2D.hpp"
#  include "aver/ui/UiDrawList.hpp"
#  include "aver/ui/ui_abi.h"
#endif

namespace aver::game {

// `abs` made relative to the Content folder with forward slashes; `abs` itself when it is not inside.
inline std::string scene2dRelativePath(const std::string& abs, const std::string& contentDir) {
    if (abs.empty() || contentDir.empty()) return abs;
    std::error_code ec;
    const auto rel = std::filesystem::relative(std::filesystem::path(abs), std::filesystem::path(contentDir), ec);
    return ec || rel.empty() ? abs : rel.generic_string();
}

// An asset id's content-relative path ("Textures/a.png"), "" when the project does not hold it.
using Scene2DPathFn = std::function<std::string(u64)>;

#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
namespace s2d_detail {

struct State {
    Scene2DPathFn pathOf;
    std::unordered_map<i64, std::string> paths;   // asset id -> path, so a frame does not search the index per sprite
};

inline State& state() {
    static State s;
    return s;
}

inline bool resolve(i64 assetId, scene2d::TextureInfo* out, void*) {
    State& s = state();
    if (!s.pathOf || !out) return false;
    auto it = s.paths.find(assetId);
    if (it == s.paths.end()) it = s.paths.emplace(assetId, s.pathOf(static_cast<u64>(assetId))).first;
    if (it->second.empty()) return false;
    const u64 tex = aver_ui_image_use(it->second.c_str());
    int32_t wh[2] = {};
    if (tex == 0 || aver_ui_image_size(tex, wh) == 0 || wh[0] <= 0 || wh[1] <= 0) return false;
    out->ui = tex;
    out->w = wh[0];
    out->h = wh[1];
    return true;
}

} // namespace s2d_detail
#endif

inline void scene2dBind(Scene2DPathFn pathOf) {
#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
    s2d_detail::State& s = s2d_detail::state();
    s.pathOf = std::move(pathOf);
    s.paths.clear();
    scene2d::setTextureResolver(s.pathOf ? &s2d_detail::resolve : nullptr, nullptr);
#else
    (void)pathOf;
#endif
}

// Forgets the cached paths: the project's content changed.
inline void scene2dForgetPaths() {
#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
    s2d_detail::state().paths.clear();
#endif
}

inline void scene2dUnbind() {
#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
    s2d_detail::State& s = s2d_detail::state();
    s.pathOf = nullptr;
    s.paths.clear();
    scene2d::setTextureResolver(nullptr, nullptr);
    scene2d::dropQueue();
#endif
}

// The viewport is {x, y, w, h} in backbuffer pixels (aver_ui_viewport); dt seconds of animation and camera time (0 steps none, but
// transforms and a new camera still settle). Returns the sprites queued.
inline u32 scene2dFrame(scene::World& world, ui::UiDrawList& list, const float viewport[4], float dt = 0.0f) {
#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
    scene2d::setCanvasViewRect({viewport[0], viewport[1], viewport[2], viewport[3]});
    scene2d::tick(world, dt);
    const u32 queued = scene2d::submitSprites(world);
    scene2d::flush(list);
    return queued;
#else
    (void)world; (void)list; (void)viewport; (void)dt;
    return 0;
#endif
}

// The host's once-a-frame call, into the ABI's retained list. Drops the queue when there is no list.
inline u32 scene2dSubmit(scene::World& world) {
#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
    auto* dl = static_cast<ui::UiDrawList*>(const_cast<void*>(aver_ui_draw_list()));
    if (!dl) { scene2d::dropQueue(); return 0; }
    float vp[4] = {};
    aver_ui_viewport(vp);
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    const float dt = last == std::chrono::steady_clock::time_point{} ? 0.0f : std::min(0.1f, std::chrono::duration<float>(now - last).count());
    last = now;
    return scene2dFrame(world, *dl, vp, dt);
#else
    (void)world;
    return 0;
#endif
}

inline void scene2dDrop() {
#if AVER_WITH_UI_ABI && AVER_MODULE_SCENE
    scene2d::dropQueue();
#endif
}

} // namespace aver::game
