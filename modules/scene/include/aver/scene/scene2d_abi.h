#ifndef AVER_SCENE2D_ABI_H
#define AVER_SCENE2D_ABI_H

/* 2D scene C ABI -- the sprite / 2D transform / canvas-layer components and the ordered canvas the C# layer binds to
 * (Aver.Framework/Scene2D.cs). Lives in Aver.Scene.dll beside scene_abi.h and shares its export macro.
 * docs/plans/RENDER_2D.md.
 *
 * The components are ordinary dynamically registered components (C2DTransform, CSprite2D, C2DLayer); everything here is
 * a typed convenience over them plus the canvas: the stretch transform and the ONE ordered submit that sprites and
 * external items (Fabric's Control draws) share.
 *
 * Zero in any field means "default", as on the components: scale 0 = 1, modulate 0 = white, hframes/vframes 0 = 1, a
 * region size of 0 = the whole image. Entities cross as int32_t; 0 is never a live entity. Setters and getters return 1
 * on success and 0 on rejection, with the reason on aver_scene_last_error. */

#include "aver/scene/scene_abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_SCENE2D_ABI_VERSION 1

/* Returns AVER_SCENE2D_ABI_VERSION as the DLL was built with. */
AVER_SCENE_ABI int32_t aver_scene2d_abi_version(void);

/* ---- canvas: stretch and the canvas transform ---- */

/* Installs the viewport stretch and the rectangle (window pixels) the game is drawn in.
 * in6 = {mode, aspect, scaleMode, baseW, baseH, scale}: the same numbers aver_ui_stretch_compute takes
 * (mode 0 disabled / 1 canvas_items / 2 viewport; aspect 0 ignore / 1 keep / 2 keep_width / 3 keep_height / 4 expand;
 * scaleMode 0 fractional / 1 integer). NULL in6 = no stretch (the content is the view's size). A view with no size clears
 * the canvas transform to the identity. */
AVER_SCENE_ABI void aver_scene2d_set_canvas(const float* in6, float viewX, float viewY, float viewW, float viewH);

/* The canvas transform in force. out6 = {a, b, c, d, tx, ty}: content units -> window pixels (x' = a x + c y + tx,
 * y' = b x + d y + ty). out11 (may be NULL) = the stretch result in aver_ui_stretch_compute's layout, for the view's size
 * (screen is relative to the view). Returns 1 when a view with a size is set, else 0 (the identity is written). */
AVER_SCENE_ABI int32_t aver_scene2d_canvas_transform(float* out6, float* out11);

/* ---- canvas: textures ---- */

/* Resolves a sprite's asset id (fnv1a64 of its content path) to the draw-list texture and its size in pixels.
 * Writes *outTexture (never 0 on success) and outWH[2]; returns 1, or 0 when the image is not available (the sprite is
 * skipped this frame). NULL fn clears it. */
typedef int32_t (*AverScene2dTextureFn)(int64_t assetId, uint64_t* outTexture, int32_t* outWH, void* user);
AVER_SCENE_ABI void aver_scene2d_set_texture_resolver(AverScene2dTextureFn fn, void* user);

/* ---- canvas: the shared ordered submit ---- */

/* The sort key is (layer, z, order), lower first; submission order breaks ties. A sprite's layer is its nearest C2DLayer's
 * index (0 with none), z its composed z-index, order its C2DTransform::treeOrder or, when 0, its hierarchy rank. */
#define AVER_SCENE2D_EXTERNAL_IN_CANVAS 1   /* the canvas transform is pushed around the callback: draw in content units */

/* An external item draws itself, into the draw list handed over, when its turn comes. */
typedef void (*AverScene2dDrawFn)(void* drawList, void* user);

/* Queues an external item. Returns 1, or 0 when fn is NULL or the queue is full. */
AVER_SCENE_ABI int32_t aver_scene2d_submit_external(int32_t layer, int32_t z, int64_t order, AverScene2dDrawFn fn, void* user,
                                                    uint32_t flags);

/* Items waiting for the next flush. */
AVER_SCENE_ABI int32_t aver_scene2d_pending(void);

/* Sorts the queue and emits it into drawList (an aver::ui::UiDrawList*, e.g. the one aver_ui_draw_list returns), then
 * empties it. NULL drawList drops the queue without drawing. */
AVER_SCENE_ABI void aver_scene2d_flush(void* drawList);

/* Recomposes the 2D transforms, queues every visible sprite of the scene world, then flushes into drawList. The once-a-frame
 * call a host makes after the game's UI scripts have run. Returns the number of sprites queued. */
AVER_SCENE_ABI int32_t aver_scene2d_frame(void* drawList);

#ifdef __cplusplus
}
#endif

#endif
