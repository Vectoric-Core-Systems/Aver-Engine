#ifndef AVER_FRAMEWORK_SUBVIEW_ABI_H
#define AVER_FRAMEWORK_SUBVIEW_ABI_H

/* Sub-viewport C ABI (Aver.Framework, G4 R5): render-to-texture views a script or a converted level asks for. Same shape as
 * framework_worldfx_abi.h: the script PUBLISHES, the host READS and draws each frame. Nothing is drawn unless a view exists.
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t/int64_t/uint64_t/float/const char* only; 1 on success and 0 on
 * refusal. All state lives in this DLL (aver/core/SubViewport.hpp), single game thread.
 *
 * A VIEW is named by a KEY, a string that is stable across loads: the node path of the viewport inside its scene, which is
 * also what an imported material's texture reference names ("@rt:viewport.<key>"). Creating a key that already exists returns its id.
 * Modes: update 0 disabled, 1 once, 2 when visible, 3 when parent visible, 4 always; clear 0 always, 1 never, 2 once.
 *
 * Cost guard: at most AVER_FW_SUBVIEW_DEFAULT_MAX_VIEWS views live at once and AVER_FW_SUBVIEW_DEFAULT_RENDERS drawn per frame unless the
 * project says otherwise (RENDER.SUBVIEWPORTS, RENDER.SUBVIEWRENDERS, RENDER.SUBVIEWPIXELS; the host calls aver_fw_subview_configure). */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_SUBVIEW_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_subview_abi_version(void);

#define AVER_FW_SUBVIEW_DEFAULT_MAX_VIEWS 4
#define AVER_FW_SUBVIEW_DEFAULT_RENDERS   2
#define AVER_FW_SUBVIEW_ABSOLUTE_MAX     16

#define AVER_FW_SUBVIEW_UPDATE_DISABLED            0
#define AVER_FW_SUBVIEW_UPDATE_ONCE                1
#define AVER_FW_SUBVIEW_UPDATE_WHEN_VISIBLE        2
#define AVER_FW_SUBVIEW_UPDATE_WHEN_PARENT_VISIBLE 3
#define AVER_FW_SUBVIEW_UPDATE_ALWAYS              4

#define AVER_FW_SUBVIEW_CLEAR_ALWAYS 0
#define AVER_FW_SUBVIEW_CLEAR_NEVER  1
#define AVER_FW_SUBVIEW_CLEAR_ONCE   2

/* consumer kinds for aver_fw_subview_consumer */
#define AVER_FW_SUBVIEW_CONSUMER_MATERIAL 0
#define AVER_FW_SUBVIEW_CONSUMER_UI       1

/* ---- script side ------------------------------------------------------------------------------------------------ */

/* Makes the view named `key` (UTF-8, 1..120 bytes), or returns the id it already has (its description is then replaced by
 * width/height only). Returns 0 when refused: the feature is off (budget of 0), the budget is full, or the key is bad. A new view
 * starts as update WHEN_VISIBLE, clear ALWAYS, opaque, no camera. */
AVER_FW_ABI int32_t aver_fw_subview_create(const char* key, int32_t width, int32_t height);
AVER_FW_ABI int32_t aver_fw_subview_destroy(int32_t id);
/* The id of the view with this key, or 0. */
AVER_FW_ABI int32_t aver_fw_subview_find(const char* key);

AVER_FW_ABI int32_t aver_fw_subview_set_size(int32_t id, int32_t width, int32_t height);
/* Setting ONCE asks for one render now, whatever happened before. */
AVER_FW_ABI int32_t aver_fw_subview_set_update(int32_t id, int32_t mode);
AVER_FW_ABI int32_t aver_fw_subview_set_clear(int32_t id, int32_t mode);
AVER_FW_ABI int32_t aver_fw_subview_set_transparent(int32_t id, int32_t on);
AVER_FW_ABI int32_t aver_fw_subview_set_own_world(int32_t id, int32_t on);
/* The entity (scene::Entity as int32) whose CCamera lens and world matrix draw the view; 0 clears it, and a view with no camera is not drawn. */
AVER_FW_ABI int32_t aver_fw_subview_set_camera(int32_t id, int32_t entity);
/* scale multiplies the rendered size (clamped 0.25..8); msaa (0, 2, 4, 8) is stored and not used. */
AVER_FW_ABI int32_t aver_fw_subview_set_scale(int32_t id, float scale, int32_t msaa);
AVER_FW_ABI int32_t aver_fw_subview_set_parent_visible(int32_t id, int32_t on);
/* The colour the target is cleared to (display values 0..1); alpha is ignored for a transparent view. */
AVER_FW_ABI int32_t aver_fw_subview_set_clear_color(int32_t id, float r, float g, float b, float a);
/* Counts something that shows the view: a material (the converted texture reference) or the UI. WHEN_VISIBLE views are drawn only while the sum
 * is above zero (or the view is on screen). delta +1 / -1. */
AVER_FW_ABI int32_t aver_fw_subview_consumer(int32_t id, int32_t kind, int32_t delta);
AVER_FW_ABI int32_t aver_fw_subview_request_render(int32_t id);
/* Shows the view over the window (a SubViewportContainer): x, y, w, h in window pixels. visible 0 hides it again. */
AVER_FW_ABI int32_t aver_fw_subview_set_screen_rect(int32_t id, float x, float y, float w, float h, int32_t visible);
/* Eight values: update, clear, transparent, camera entity, effective width, effective height, render count (clamped), generation. */
AVER_FW_ABI int32_t aver_fw_subview_info(int32_t id, int32_t* out8);
/* The host's texture for the view: an rhi::TextureHandle widened to 64 bits, 0 until the host has drawn it. Changes when the target is rebuilt
 * (read aver_fw_subview_info's generation). Hand it to the UI as the widget texture. */
AVER_FW_ABI uint64_t aver_fw_subview_texture(int32_t id);

/* ---- host side -------------------------------------------------------------------------------------------------- */

/* Sets the cost guard. maxViews 0 refuses every create (and destroys nothing). rendersPerFrame is clamped 1..16. maxPixelsMega is the summed pixel cap
 * in megapixels (<= 0 keeps the default 8). Called once from the project's RENDER.SUBVIEW* keys. */
AVER_FW_ABI void    aver_fw_subview_configure(int32_t maxViews, int32_t rendersPerFrame, float maxPixelsMega);
AVER_FW_ABI int32_t aver_fw_subview_count(void);
/* The id at a position (creation order), or 0. */
AVER_FW_ABI int32_t aver_fw_subview_id_at(int32_t index);
/* The views to draw this frame (see SubViewRegistry::plan): ids and, per id, whether to clear the target first. Returns the count (<= max). */
AVER_FW_ABI int32_t aver_fw_subview_plan(int64_t frame, int32_t* outIds, int32_t* outClear, int32_t max);
/* Description for the host: width, height (effective, after the scale and the pixel budget), clear colour r g b a (alpha 0 when transparent), flags
 * (bit0 transparent, bit1 linear output, bit2 on screen, bit3 own world), camera entity. out8 (ints) and outColor4 (floats). */
AVER_FW_ABI int32_t aver_fw_subview_host_info(int32_t id, int32_t* out8, float* outColor4);
AVER_FW_ABI int32_t aver_fw_subview_screen(int32_t id, float* outRect4);   /* 1 and the rect when the view is on screen */
/* The view's key, UTF-8, NUL-terminated into out (cap bytes including the NUL). Returns the key's length in bytes (cap - 1 at most are written), or 0 for an
 * unknown id. The host names the view's runtime texture ("viewport.<key>") from it. */
AVER_FW_ABI int32_t aver_fw_subview_key(int32_t id, char* out, int32_t cap);
/* The host's texture for a view, its size in pixels. Bumps the generation when it changed. */
AVER_FW_ABI int32_t aver_fw_subview_host_set_texture(int32_t id, uint64_t texture, int32_t width, int32_t height);
/* How many create calls the budget refused, and renders it postponed, since the process started (for the host's log). */
AVER_FW_ABI int64_t aver_fw_subview_refused(void);
AVER_FW_ABI int64_t aver_fw_subview_deferred(void);
/* Session boundary: forgets every view. The first call after aver_fw_play_state() falls back to AVER_FW_PLAY_EDITOR does it by itself through
 * aver_fw_subview_tick; a host may also call this on a level change. */
AVER_FW_ABI void    aver_fw_subview_reset(void);
AVER_FW_ABI void    aver_fw_subview_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_SUBVIEW_ABI_H */
