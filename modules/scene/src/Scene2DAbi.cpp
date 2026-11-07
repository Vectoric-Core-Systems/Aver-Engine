// The 2D scene C ABI's translation unit (scene2d_abi.h): the canvas over the process-wide Canvas2D.
#include "aver/scene/scene2d_abi.h"

#include "aver/core/ErrorCodes.hpp"
#include "aver/scene/Canvas2D.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstring>
#include <type_traits>

using namespace aver;
using namespace aver::scene2d;

static_assert(AVER_SCENE2D_EXTERNAL_IN_CANVAS == kExternalInCanvas, "AVER_SCENE2D_EXTERNAL_IN_CANVAS drift");
static_assert(std::is_same_v<AverScene2dDrawFn, ExternalDrawFn>, "the C callback type is the C++ one");

namespace {

struct TextureBridge {
    AverScene2dTextureFn fn = nullptr;
    void* user = nullptr;
};
TextureBridge g_tex;

bool resolveThunk(i64 assetId, TextureInfo* out, void*) {
    uint64_t t = 0;
    int32_t wh[2] = {0, 0};
    if (!g_tex.fn || !g_tex.fn(assetId, &t, wh, g_tex.user) || t == 0 || wh[0] <= 0 || wh[1] <= 0) return false;
    out->ui = t;
    out->w = wh[0];
    out->h = wh[1];
    return true;
}

}   // namespace

extern "C" {

int32_t aver_scene2d_abi_version(void) { return AVER_SCENE2D_ABI_VERSION; }

void aver_scene2d_set_canvas(const float* in6, float viewX, float viewY, float viewW, float viewH) {
    ui::UiStretchSettings s;
    if (in6) {
        s.mode = static_cast<ui::UiStretchMode>(static_cast<int>(in6[0]));
        s.aspect = static_cast<ui::UiStretchAspect>(static_cast<int>(in6[1]));
        s.scaleMode = static_cast<ui::UiStretchScaleMode>(static_cast<int>(in6[2]));
        s.baseW = in6[3]; s.baseH = in6[4]; s.scale = in6[5];
    }
    const bool finite = std::isfinite(viewX) && std::isfinite(viewY) && std::isfinite(viewW) && std::isfinite(viewH);
    setCanvasView(s, finite ? CanvasView{viewX, viewY, viewW, viewH} : CanvasView{});
    setAbiError(finite ? AbiError::Ok : AbiError::InvalidArgument);
}

int32_t aver_scene2d_canvas_transform(float* out6, float* out11) {
    const CanvasTransform c = canvasTransform();
    if (out6) {
        const float v[6] = {c.toWindow.a, c.toWindow.b, c.toWindow.c, c.toWindow.d, c.toWindow.tx, c.toWindow.ty};
        std::memcpy(out6, v, sizeof v);
    }
    if (out11) {
        const ui::UiStretchResult& r = c.stretch;
        const float v[11] = {r.screen.x, r.screen.y, r.screen.w, r.screen.h, r.contentSize.x, r.contentSize.y, r.renderSize.x, r.renderSize.y,
                             r.scaleX, r.scaleY, r.letterboxed ? 1.0f : 0.0f};
        std::memcpy(out11, v, sizeof v);
    }
    return c.valid ? 1 : 0;
}

void aver_scene2d_set_texture_resolver(AverScene2dTextureFn fn, void* user) {
    g_tex = {fn, user};
    setTextureResolver(fn ? &resolveThunk : nullptr, nullptr);
}

int32_t aver_scene2d_submit_external(int32_t layer, int32_t z, int64_t order, AverScene2dDrawFn fn, void* user, uint32_t flags) {
    if (!fn) { setAbiError(AbiError::NullPointer); return 0; }
    const bool ok = submitExternal(SortKey{layer, z, static_cast<u64>(order)}, fn, user, flags);
    setAbiError(ok ? AbiError::Ok : AbiError::AllocationFailed);
    return ok ? 1 : 0;
}

int32_t aver_scene2d_pending(void) { return static_cast<int32_t>(pendingCount()); }

void aver_scene2d_flush(void* drawList) {
    if (drawList) flush(*static_cast<ui::UiDrawList*>(drawList));
    else dropQueue();
}

int32_t aver_scene2d_frame(void* drawList) {
    if (!drawList) { setAbiError(AbiError::NullPointer); return 0; }
    const u32 queued = submitSprites(scene::World::instance());
    flush(*static_cast<ui::UiDrawList*>(drawList));
    setAbiError(AbiError::Ok);
    return static_cast<int32_t>(queued);
}

}   // extern "C"
