// Sub-viewport store behind framework_subview_abi.h. One file-static registry (aver/core/SubViewport.hpp), single game thread; the hosts read it
// every frame (Runtime GameApp, the editor's Play path) and draw the views it plans.

#include "aver/framework/framework_subview_abi.h"

#include "aver/core/Log.hpp"
#include "aver/core/SubViewport.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace {

struct Store {
    aver::SubViewRegistry reg;
    int32_t lastPlayState = AVER_FW_PLAY_EDITOR;
    bool refusedLogged = false;
};

Store& store() {
    static Store s;
    return s;
}

aver::SubViewRegistry::Entry* entryOf(int32_t id) {
    return id > 0 ? store().reg.get(static_cast<uint32_t>(id)) : nullptr;
}

// Applies `fn` to a copy of the description and stores it back, so the scheduling state is kept.
template <class Fn>
int32_t edit(int32_t id, Fn fn) {
    auto* e = entryOf(id);
    if (!e) return 0;
    aver::SubViewDesc d = e->desc;
    fn(d);
    return store().reg.update(e->id, d) ? 1 : 0;
}

} // namespace

extern "C" {

int32_t aver_fw_subview_abi_version(void) { return AVER_FW_SUBVIEW_ABI_VERSION; }

int32_t aver_fw_subview_create(const char* key, int32_t width, int32_t height) {
    if (!key) return 0;
    Store& s = store();
    aver::SubViewDesc d;
    if (width > 0) d.width = static_cast<uint32_t>(width);
    if (height > 0) d.height = static_cast<uint32_t>(height);
    const std::string k(key);
    if (auto* existing = s.reg.find(k)) {
        aver::SubViewDesc cur = existing->desc;
        cur.width = d.width;
        cur.height = d.height;
        s.reg.update(existing->id, cur);
        return static_cast<int32_t>(existing->id);
    }
    const uint32_t id = s.reg.create(k, d);
    if (id == 0 && !s.refusedLogged) {
        s.refusedLogged = true;
        AVER_WARN("[SubViewport] view '{}' refused: {} of {} views are live (RENDER.SUBVIEWPORTS; 0 turns the feature off)", k, s.reg.count(),
                  s.reg.budget().maxViews);
    }
    return static_cast<int32_t>(id);
}

int32_t aver_fw_subview_destroy(int32_t id) { return id > 0 && store().reg.destroy(static_cast<uint32_t>(id)) ? 1 : 0; }

int32_t aver_fw_subview_find(const char* key) {
    if (!key) return 0;
    const auto* e = store().reg.find(std::string(key));
    return e ? static_cast<int32_t>(e->id) : 0;
}

int32_t aver_fw_subview_set_size(int32_t id, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return 0;
    return edit(id, [&](aver::SubViewDesc& d) { d.width = static_cast<uint32_t>(width); d.height = static_cast<uint32_t>(height); });
}

int32_t aver_fw_subview_set_update(int32_t id, int32_t mode) {
    if (mode < 0 || mode > 4) return 0;
    const int32_t ok = edit(id, [&](aver::SubViewDesc& d) { d.update = aver::viewUpdateFromInt(mode); });
    if (ok && mode == AVER_FW_SUBVIEW_UPDATE_ONCE) entryOf(id)->pendingRender = true;
    return ok;
}

int32_t aver_fw_subview_set_clear(int32_t id, int32_t mode) {
    if (mode < 0 || mode > 2) return 0;
    return edit(id, [&](aver::SubViewDesc& d) { d.clear = aver::viewClearFromInt(mode); });
}

int32_t aver_fw_subview_set_transparent(int32_t id, int32_t on) {
    return edit(id, [&](aver::SubViewDesc& d) { d.transparent = on != 0; });
}

int32_t aver_fw_subview_set_own_world(int32_t id, int32_t on) {
    return edit(id, [&](aver::SubViewDesc& d) { d.ownWorld3d = on != 0; });
}

int32_t aver_fw_subview_set_camera(int32_t id, int32_t entity) {
    return edit(id, [&](aver::SubViewDesc& d) { d.cameraEntity = entity > 0 ? entity : 0; });
}

int32_t aver_fw_subview_set_scale(int32_t id, float scale, int32_t msaa) {
    if (!(scale > 0.0f) || !std::isfinite(scale)) return 0;
    return edit(id, [&](aver::SubViewDesc& d) { d.scale3d = std::min(8.0f, std::max(0.25f, scale)); d.msaa = msaa; });
}

int32_t aver_fw_subview_set_parent_visible(int32_t id, int32_t on) {
    return edit(id, [&](aver::SubViewDesc& d) { d.parentVisible = on != 0; });
}

int32_t aver_fw_subview_set_clear_color(int32_t id, float r, float g, float b, float a) {
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a)) return 0;
    return edit(id, [&](aver::SubViewDesc& d) {
        d.clearColor[0] = std::min(1.0f, std::max(0.0f, r));
        d.clearColor[1] = std::min(1.0f, std::max(0.0f, g));
        d.clearColor[2] = std::min(1.0f, std::max(0.0f, b));
        d.clearColor[3] = std::min(1.0f, std::max(0.0f, a));
    });
}

int32_t aver_fw_subview_consumer(int32_t id, int32_t kind, int32_t delta) {
    return id > 0 && store().reg.consumer(static_cast<uint32_t>(id), kind, delta) ? 1 : 0;
}

int32_t aver_fw_subview_request_render(int32_t id) {
    auto* e = entryOf(id);
    if (!e) return 0;
    e->pendingRender = true;
    return 1;
}

int32_t aver_fw_subview_set_screen_rect(int32_t id, float x, float y, float w, float h, int32_t visible) {
    auto* e = entryOf(id);
    if (!e || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) return 0;
    if (visible != 0 && (w <= 0.0f || h <= 0.0f)) return 0;
    e->onScreen = visible != 0;
    e->screen[0] = x; e->screen[1] = y; e->screen[2] = w; e->screen[3] = h;
    return 1;
}

int32_t aver_fw_subview_info(int32_t id, int32_t* out8) {
    const auto* e = entryOf(id);
    if (!e) return 0;
    if (out8) {
        out8[0] = static_cast<int32_t>(e->desc.update);
        out8[1] = static_cast<int32_t>(e->desc.clear);
        out8[2] = e->desc.transparent ? 1 : 0;
        out8[3] = e->desc.cameraEntity;
        out8[4] = static_cast<int32_t>(e->effW);
        out8[5] = static_cast<int32_t>(e->effH);
        out8[6] = static_cast<int32_t>(std::min<uint64_t>(e->renderCount, 0x7FFFFFFFu));
        out8[7] = static_cast<int32_t>(e->generation);
    }
    return 1;
}

uint64_t aver_fw_subview_texture(int32_t id) {
    const auto* e = entryOf(id);
    return e ? e->texture : 0;
}

// ---- host side ----

void aver_fw_subview_configure(int32_t maxViews, int32_t rendersPerFrame, float maxPixelsMega) {
    Store& s = store();
    aver::SubViewBudget b = s.reg.budget();
    b.maxViews = static_cast<uint32_t>(std::max(0, std::min(maxViews, AVER_FW_SUBVIEW_ABSOLUTE_MAX)));
    b.rendersPerFrame = static_cast<uint32_t>(std::max(1, std::min(rendersPerFrame, AVER_FW_SUBVIEW_ABSOLUTE_MAX)));
    if (maxPixelsMega > 0.0f && std::isfinite(maxPixelsMega))
        b.maxTotalPixels = static_cast<uint64_t>(static_cast<double>(std::min(maxPixelsMega, 256.0f)) * 1024.0 * 1024.0);
    s.reg.setBudget(b);
}

int32_t aver_fw_subview_count(void) { return static_cast<int32_t>(store().reg.count()); }

int32_t aver_fw_subview_id_at(int32_t index) {
    if (index < 0) return 0;
    const auto* e = store().reg.at(static_cast<uint32_t>(index));
    return e ? static_cast<int32_t>(e->id) : 0;
}

int32_t aver_fw_subview_plan(int64_t frame, int32_t* outIds, int32_t* outClear, int32_t max) {
    if (!outIds || max <= 0) return 0;
    aver::SubViewRegistry::Job jobs[AVER_FW_SUBVIEW_ABSOLUTE_MAX];
    const uint32_t cap = static_cast<uint32_t>(std::min(max, AVER_FW_SUBVIEW_ABSOLUTE_MAX));
    const uint32_t n = store().reg.plan(frame < 0 ? 0u : static_cast<uint64_t>(frame), jobs, cap);
    for (uint32_t i = 0; i < n; ++i) {
        outIds[i] = static_cast<int32_t>(jobs[i].id);
        if (outClear) outClear[i] = jobs[i].clear ? 1 : 0;
    }
    return static_cast<int32_t>(n);
}

int32_t aver_fw_subview_host_info(int32_t id, int32_t* out8, float* outColor4) {
    const auto* e = entryOf(id);
    if (!e) return 0;
    if (out8) {
        out8[0] = static_cast<int32_t>(e->effW);
        out8[1] = static_cast<int32_t>(e->effH);
        out8[2] = (e->desc.transparent ? 1 : 0) | (aver::SubViewRegistry::linearOutput(*e) ? 2 : 0) | (e->onScreen ? 4 : 0) |
                  (e->desc.ownWorld3d ? 8 : 0);
        out8[3] = e->desc.cameraEntity;
        out8[4] = static_cast<int32_t>(e->generation);
        out8[5] = e->desc.msaa;
        out8[6] = static_cast<int32_t>(e->desc.clear);
        out8[7] = static_cast<int32_t>(e->desc.update);
    }
    if (outColor4) {
        for (int i = 0; i < 3; ++i) outColor4[i] = e->desc.clearColor[i];
        outColor4[3] = e->desc.transparent ? 0.0f : e->desc.clearColor[3];
    }
    return 1;
}

int32_t aver_fw_subview_screen(int32_t id, float* outRect4) {
    const auto* e = entryOf(id);
    if (!e || !e->onScreen) return 0;
    if (outRect4) for (int i = 0; i < 4; ++i) outRect4[i] = e->screen[i];
    return 1;
}

int32_t aver_fw_subview_key(int32_t id, char* out, int32_t cap) {
    const auto* e = entryOf(id);
    if (!e) return 0;
    if (out && cap > 0) {
        const size_t n = std::min<size_t>(e->key.size(), static_cast<size_t>(cap - 1));
        std::memcpy(out, e->key.data(), n);
        out[n] = '\0';
    }
    return static_cast<int32_t>(e->key.size());
}

int32_t aver_fw_subview_host_set_texture(int32_t id, uint64_t texture, int32_t width, int32_t height) {
    if (id <= 0 || width < 0 || height < 0) return 0;
    return store().reg.setTexture(static_cast<uint32_t>(id), texture, static_cast<uint32_t>(width), static_cast<uint32_t>(height)) ? 1 : 0;
}

int64_t aver_fw_subview_refused(void) { return static_cast<int64_t>(store().reg.refused()); }
int64_t aver_fw_subview_deferred(void) { return static_cast<int64_t>(store().reg.deferred()); }

void aver_fw_subview_reset(void) {
    Store& s = store();
    s.reg.clear();
    s.refusedLogged = false;
}

void aver_fw_subview_tick(void) {
    Store& s = store();
    // A play session ending hands the world back: the editor keeps this DLL loaded across Play presses, so the views a level made are dropped
    // when the state falls back to Editor.
    const int32_t state = aver_fw_play_state();
    if (state == AVER_FW_PLAY_EDITOR && s.lastPlayState != AVER_FW_PLAY_EDITOR) aver_fw_subview_reset();
    s.lastPlayState = state;
}

} // extern "C"
