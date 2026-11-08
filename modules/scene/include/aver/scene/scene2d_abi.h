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

#define AVER_SCENE2D_ABI_VERSION 3

/* Returns AVER_SCENE2D_ABI_VERSION as the DLL was built with. */
AVER_SCENE_ABI int32_t aver_scene2d_abi_version(void);

/* ---- components ---- */

/* Registers C2DTransform, CSprite2D, C2DLayer, CSpriteAnim2D, CCamera2D and CParallax2D with the scene world (dynamic ids, no built-in moves). Idempotent. Returns 1,
 * or 0 when the world refused. Call once before any other component call. */
AVER_SCENE_ABI int32_t aver_scene2d_register(void);

/* The dynamic component id of 0 = C2DTransform, 1 = CSprite2D, 2 = C2DLayer, 3 = CSpriteAnim2D, 4 = CCamera2D, 5 = CParallax2D (for the
 * generic scene ABI), or 0 when not registered. */
AVER_SCENE_ABI int32_t aver_scene2d_component_id(int32_t which);

/* The authored state of each component. Field order is part of the ABI: nothing is padded. */
typedef struct AverScene2dTransform {
    float    position[2];
    float    rotation;        /* radians, clockwise on screen (y points down) */
    float    scale[2];        /* 0 = 1 */
    int32_t  zIndex;          /* -4096 .. 4096 */
    uint32_t flags;           /* AVER_SCENE2D_T_* */
    uint32_t treeOrder;       /* 0 = from the hierarchy; else the shared sort key's tree order */
} AverScene2dTransform;

typedef struct AverScene2dSprite {
    int64_t  texture;         /* asset id: fnv1a64 of the content-relative path; 0 = nothing */
    float    region[4];       /* x, y, w, h in pixels; w or h of 0 = the whole image */
    float    offset[2];
    uint32_t modulate;        /* straight RGBA, 0xAABBGGRR; 0 = white */
    uint32_t flags;           /* AVER_SCENE2D_S_* */
    int32_t  hframes;         /* 0 = 1 */
    int32_t  vframes;         /* 0 = 1 */
    int32_t  frame;
    int32_t  blend;           /* AVER_SCENE2D_BLEND_*, 0 = mix */
} AverScene2dSprite;

typedef struct AverScene2dLayer {
    int32_t  index;
    uint32_t flags;           /* AVER_SCENE2D_L_* */
    float    offset[2];
    float    rotation;
    float    scale[2];        /* 0 = 1 */
    uint32_t reserved;        /* zero */
} AverScene2dLayer;

/* Mirror the C++ flag bits in Scene2D.hpp (checked in Scene2DAbi.cpp). */
#define AVER_SCENE2D_T_Z_ABSOLUTE   1   /* z-index is the final z, not an offset from the parent's */
#define AVER_SCENE2D_T_TOP_LEVEL    2   /* ignore the parent's transform */
#define AVER_SCENE2D_T_HIDDEN       4   /* this entity and everything below draws nothing */
#define AVER_SCENE2D_S_FLIP_H       1
#define AVER_SCENE2D_S_FLIP_V       2
#define AVER_SCENE2D_S_NOT_CENTERED 4   /* the offset is the top-left corner, not the centre */
#define AVER_SCENE2D_S_HIDDEN       8
#define AVER_SCENE2D_S_NEAREST      16  /* nearest-neighbour sampling */
#define AVER_SCENE2D_L_HIDDEN       1
#define AVER_SCENE2D_L_FOLLOW_VIEWPORT 2   /* the current camera's view applies to this layer (the default canvas always follows) */
#define AVER_SCENE2D_BLEND_MIX 0
#define AVER_SCENE2D_BLEND_ADD 1
#define AVER_SCENE2D_BLEND_SUB 2
#define AVER_SCENE2D_BLEND_MUL 3

/* Sprite animation (CSpriteAnim2D), 2D camera (CCamera2D) and parallax (CParallax2D). */

typedef struct AverScene2dAnimFrame {
    int64_t  texture;         /* asset id; 0 = keep the sprite's own texture */
    float    region[4];       /* x, y, w, h in pixels; w or h of 0 = the whole image */
    float    weight;          /* relative length, 0 = 1: the frame lasts weight / (fps * speedScale) seconds */
    int32_t  cell;            /* the sprite's hframes x vframes cell while this frame shows */
} AverScene2dAnimFrame;

/* Playback state of the sprite animation on an entity. set_anim reads speedScale and autoplay only: playback is driven by the
 * play / stop / pause / set_frame calls. */
typedef struct AverScene2dAnimState {
    float    speedScale;      /* 0 = 1; negative plays backwards */
    uint32_t flags;           /* AVER_SCENE2D_A_* (read only) */
    int32_t  clip;            /* index of the current clip, -1 with none (read only) */
    int32_t  autoplay;        /* clip index + 1 played on the first frame; 0 = none */
    int32_t  frame;           /* frame within the current clip (read only) */
    float    progress;        /* 0..1 through that frame (read only) */
    int32_t  clipCount;       /* read only */
    int32_t  frameCount;      /* read only */
} AverScene2dAnimState;

#define AVER_SCENE2D_A_PLAYING 1
#define AVER_SCENE2D_ANIM_MAX_CLIPS  16
#define AVER_SCENE2D_ANIM_MAX_FRAMES 128   /* across all the clips of one entity */
#define AVER_SCENE2D_ANIM_NAME_BYTES 24    /* clip names, incl. the terminator */

/* Events aver_scene2d_anim_poll drains. */
typedef struct AverScene2dAnimEvent {
    int32_t entity;
    int32_t kind;             /* AVER_SCENE2D_ANIM_EVENT_* */
    int32_t clip;
    int32_t frame;
} AverScene2dAnimEvent;
#define AVER_SCENE2D_ANIM_EVENT_FRAME_CHANGED     0
#define AVER_SCENE2D_ANIM_EVENT_FINISHED          1   /* a non-looping clip reached its end */
#define AVER_SCENE2D_ANIM_EVENT_LOOPED            2
#define AVER_SCENE2D_ANIM_EVENT_ANIMATION_CHANGED 3

typedef struct AverScene2dCamera {
    float    zoom[2];         /* 0 = 1 */
    float    offset[2];       /* added to the entity's 2D position to get the view centre */
    float    limit[4];        /* left, top, right, bottom in world units; each applies only with its AVER_SCENE2D_C_LIMIT_* flag */
    float    smoothSpeed;     /* 0 = 5 */
    float    drag[4];         /* left, top, right, bottom margins as fractions of the half view */
    uint32_t flags;           /* AVER_SCENE2D_C_* */
} AverScene2dCamera;

#define AVER_SCENE2D_C_DISABLED        1
#define AVER_SCENE2D_C_CURRENT         2    /* preferred as the current camera; aver_scene2d_camera_make_current also clears the others' */
#define AVER_SCENE2D_C_FOLLOW_ROTATION 4    /* the view turns with the camera (default: ignores rotation) */
#define AVER_SCENE2D_C_SMOOTHING       8
#define AVER_SCENE2D_C_DRAG_H          16
#define AVER_SCENE2D_C_DRAG_V          32
#define AVER_SCENE2D_C_ANCHOR_TOP_LEFT 64   /* the position is the view's top-left corner, not its centre */
#define AVER_SCENE2D_C_LIMIT_LEFT      128
#define AVER_SCENE2D_C_LIMIT_TOP       256
#define AVER_SCENE2D_C_LIMIT_RIGHT     512
#define AVER_SCENE2D_C_LIMIT_BOTTOM    1024

/* Godot's Parallax2D. Not zero-means-default: scrollScale is stored as given (1 scrolls with the world, 0 stays on screen). */
typedef struct AverScene2dParallax {
    float    scrollScale[2];
    float    scrollOffset[2];
    float    repeatSize[2];   /* 0 = no tiling on that axis */
    int32_t  repeatTimes;     /* copies each side; 0 = 1 */
    float    autoscroll[2];   /* world units per second */
    float    limitBegin[2];
    float    limitEnd[2];
    uint32_t flags;           /* AVER_SCENE2D_P_* */
} AverScene2dParallax;

#define AVER_SCENE2D_P_NO_FOLLOW   1   /* does not follow the camera: screen space (Godot follow_viewport off) */
#define AVER_SCENE2D_P_LIMIT_BEGIN 2
#define AVER_SCENE2D_P_LIMIT_END   4

/* sizeof of AverScene2dTransform (which 0), AverScene2dSprite (1), AverScene2dLayer (2), AverScene2dAnimState (3), AverScene2dCamera (4),
 * AverScene2dParallax (5), AverScene2dAnimFrame (6) and AverScene2dAnimEvent (7): the managed side compares them with Marshal.SizeOf and
 * turns the API off on a mismatch. 0 for another value. */
AVER_SCENE_ABI int32_t aver_scene2d_struct_size(int32_t which);

/* Adds the component if the entity lacks it and writes the authored state. A non-finite float, an out-of-range z or blend is
 * InvalidArgument (nothing written); a stale entity is BadHandle. */
AVER_SCENE_ABI int32_t aver_scene2d_set_transform(int32_t entity, const AverScene2dTransform* desc);
AVER_SCENE_ABI int32_t aver_scene2d_set_sprite(int32_t entity, const AverScene2dSprite* desc);
AVER_SCENE_ABI int32_t aver_scene2d_set_layer(int32_t entity, const AverScene2dLayer* desc);

/* Reads the authored state. 0 (outDesc untouched) when the entity lacks the component. */
AVER_SCENE_ABI int32_t aver_scene2d_get_transform(int32_t entity, AverScene2dTransform* outDesc);
AVER_SCENE_ABI int32_t aver_scene2d_get_sprite(int32_t entity, AverScene2dSprite* outDesc);
AVER_SCENE_ABI int32_t aver_scene2d_get_layer(int32_t entity, AverScene2dLayer* outDesc);

/* Camera and parallax: the same set/get rules as above (a non-finite number is InvalidArgument). */
AVER_SCENE_ABI int32_t aver_scene2d_set_camera(int32_t entity, const AverScene2dCamera* desc);
AVER_SCENE_ABI int32_t aver_scene2d_get_camera(int32_t entity, AverScene2dCamera* outDesc);
AVER_SCENE_ABI int32_t aver_scene2d_set_parallax(int32_t entity, const AverScene2dParallax* desc);
AVER_SCENE_ABI int32_t aver_scene2d_get_parallax(int32_t entity, AverScene2dParallax* outDesc);

/* Flags the entity's camera current and clears the flag on the others. 0 when it has no camera. */
AVER_SCENE_ABI int32_t aver_scene2d_camera_make_current(int32_t entity);
/* The entity of the camera that applies (the first enabled one flagged current, else the first enabled), 0 for none. */
AVER_SCENE_ABI int32_t aver_scene2d_camera_current(void);
/* The view centre of the entity's camera in world units after easing and limits, for the canvas's current content size. 0 (zeros
 * written) when the entity has no camera. */
AVER_SCENE_ABI int32_t aver_scene2d_camera_center(int32_t entity, float* outXY);
/* The current camera's world -> content-units affine {a, b, c, d, tx, ty}; the identity with no camera or no canvas. Returns 1 when a camera applies. */
AVER_SCENE_ABI int32_t aver_scene2d_camera_view(float* out6);

/* Sprite animation. The component is added by set_anim or the first set_clip. */
AVER_SCENE_ABI int32_t aver_scene2d_set_anim(int32_t entity, const AverScene2dAnimState* desc);
AVER_SCENE_ABI int32_t aver_scene2d_get_anim(int32_t entity, AverScene2dAnimState* outDesc);
/* Defines clip `index` (index == clipCount appends) with frames[0..count). InvalidArgument for a bad index, an empty or too long name
 * (a clip name is unique per entity), a non-finite number, or when the clips or frames would exceed the limits above. */
AVER_SCENE_ABI int32_t aver_scene2d_anim_set_clip(int32_t entity, int32_t index, const char* name, float fps, int32_t loop,
                                                  const AverScene2dAnimFrame* frames, int32_t count);
/* Reads clip `index`: outName must hold AVER_SCENE2D_ANIM_NAME_BYTES; any out pointer may be NULL. Returns 0 for an index past the clips. */
AVER_SCENE_ABI int32_t aver_scene2d_anim_get_clip(int32_t entity, int32_t index, uint8_t* outName, float* outFps, int32_t* outLoop, int32_t* outFrameCount);
/* Copies the first `max` frames of clip `index`; returns how many it wrote, -1 when the clip does not exist. */
AVER_SCENE_ABI int32_t aver_scene2d_anim_get_frames(int32_t entity, int32_t index, AverScene2dAnimFrame* outFrames, int32_t max);
/* The clip index of `name`, -1 for none. */
AVER_SCENE_ABI int32_t aver_scene2d_anim_find(int32_t entity, const char* name);
/* Plays `name` (NULL or empty: the current clip); restart rewinds a clip already playing. 0 for an unknown or empty clip. */
AVER_SCENE_ABI int32_t aver_scene2d_anim_play(int32_t entity, const char* name, int32_t restart);
AVER_SCENE_ABI int32_t aver_scene2d_anim_stop(int32_t entity);    /* stops and rewinds to frame 0 */
AVER_SCENE_ABI int32_t aver_scene2d_anim_pause(int32_t entity);   /* stops where it is */
AVER_SCENE_ABI int32_t aver_scene2d_anim_set_frame(int32_t entity, int32_t frame, float progress);
/* Moves up to max queued events into outEvents, oldest first; returns how many. */
AVER_SCENE_ABI int32_t aver_scene2d_anim_poll(AverScene2dAnimEvent* outEvents, int32_t max);

/* One step of everything time-driven (transforms, animations and their events, parallax autoscroll, camera drag and smoothing). A host
 * calls it once a frame before the canvas frame; dt in seconds. */
AVER_SCENE_ABI void aver_scene2d_tick(float dt);

/* Removes 0 = C2DTransform, 1 = CSprite2D, 2 = C2DLayer, 3 = CSpriteAnim2D, 4 = CCamera2D, 5 = CParallax2D. 1 when it was there. */
AVER_SCENE_ABI int32_t aver_scene2d_remove(int32_t entity, int32_t which);

/* Recomposes the 2D transforms now (the canvas frame does it too). Returns how many entities were recomposed. */
AVER_SCENE_ABI int32_t aver_scene2d_update_transforms(void);

/* The entity's composed world affine {a, b, c, d, tx, ty} and effective z (outZ may be NULL), after recomposing. 0 (identity
 * written) when the entity has no C2DTransform. */
AVER_SCENE_ABI int32_t aver_scene2d_world_affine(int32_t entity, float* outAffine6, int32_t* outZ);

/* The sprite's axis-aligned bounds in world 2D space {x, y, w, h}. Needs the image size from the texture resolver unless the
 * sprite names a region. 0 (zeros written) when the entity has no sprite or the size is unknown. */
AVER_SCENE_ABI int32_t aver_scene2d_sprite_bounds(int32_t entity, float* outRect4);

/* ---- canvas: stretch and the canvas transform ---- */

/* Installs the viewport stretch and the rectangle (window pixels) the game is drawn in.
 * in6 = {mode, aspect, scaleMode, baseW, baseH, scale}: the same numbers aver_ui_stretch_compute takes
 * (mode 0 disabled / 1 canvas_items / 2 viewport; aspect 0 ignore / 1 keep / 2 keep_width / 3 keep_height / 4 expand;
 * scaleMode 0 fractional / 1 integer). NULL in6 = no stretch (the content is the view's size). A view with no size clears
 * the canvas transform to the identity. */
AVER_SCENE_ABI void aver_scene2d_set_canvas(const float* in6, float viewX, float viewY, float viewW, float viewH);

/* Moves the rectangle (window pixels) the game is drawn in and keeps the stretch rules in force. A host calls it every frame with the
 * real viewport. A rectangle with no size, or a non-finite number, clears the canvas transform to the identity. */
AVER_SCENE_ABI void aver_scene2d_set_canvas_view(float viewX, float viewY, float viewW, float viewH);

/* The canvas transform in force. out6 = {a, b, c, d, tx, ty}: content units -> window pixels (x' = a x + c y + tx,
 * y' = b x + d y + ty). out11 (may be NULL) = the stretch result in aver_ui_stretch_compute's layout, for the view's size
 * (screen is relative to the view). Returns 1 when a view with a size is set, else 0 (the identity is written). */
AVER_SCENE_ABI int32_t aver_scene2d_canvas_transform(float* out6, float* out11);

/* ---- canvas: textures ---- */

/* Resolves a sprite's asset id (fnv1a64 of its content path) to the draw-list texture and its size in pixels.
 * Writes *outTexture (never 0 on success) and outWH[2]; returns 1, or 0 when the image is not available (the sprite is
 * skipped this frame). NULL fn clears it. */
typedef int32_t (*aver_scene2d_texture_fn)(int64_t assetId, uint64_t* outTexture, int32_t* outWH, void* user);
AVER_SCENE_ABI void aver_scene2d_set_texture_resolver(aver_scene2d_texture_fn fn, void* user);

/* ---- canvas: the shared ordered submit ---- */

/* The sort key is (layer, z, order), lower first; submission order breaks ties. A sprite's layer is its nearest C2DLayer's
 * index (0 with none), z its composed z-index, order its C2DTransform::treeOrder or, when 0, its hierarchy rank. */
#define AVER_SCENE2D_EXTERNAL_IN_CANVAS 1   /* the canvas transform is pushed around the callback: draw in content units */

/* An external item draws itself, into the draw list handed over, when its turn comes. */
typedef void (*aver_scene2d_draw_fn)(void* drawList, void* user);

/* Queues an external item. Returns 1, or 0 when fn is NULL or the queue is full. */
AVER_SCENE_ABI int32_t aver_scene2d_submit_external(int32_t layer, int32_t z, int64_t order, aver_scene2d_draw_fn fn, void* user,
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
