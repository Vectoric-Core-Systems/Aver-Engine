#pragma once
// 2D scenes: transforms, sprites and canvas layers as ordinary entity components (docs/plans/RENDER_2D.md).
//
// REGISTERED DYNAMICALLY, exactly as CControlRig is, so no built-in id moves and scene_abi.h is untouched. Call
// registerComponents once per world before attaching; it is idempotent.
//
// ZERO MEANS UNSET. The pools zero-fill and run no constructor, so every field's zero is its default: scale 0 reads
// as 1, a 0 modulate as white, 0 hframes/vframes as 1, a 0 region size as "the whole image", flag bits are the
// non-default behaviour (hidden, not centered, absolute z). A freshly attached component is therefore valid as is.
//
// The derived world affine of a C2DTransform is recomposed by updateTransforms, apart from the 3D transform pass. Only
// an ancestor that is itself a C2DTransform contributes (a C2DLayer or a plain entity resets the chain, as a Godot
// CanvasLayer does).
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

namespace aver::scene {
class World;
}

namespace aver::scene2d {

using scene::Entity;

// C2DTransform::flags
enum : u32 {
    kT2DZAbsolute = 1,   // z-index is the final z, not an offset from the parent's (Godot's z_as_relative off)
    kT2DTopLevel  = 2,   // ignore the parent's transform (Godot's top_level)
    kT2DHidden    = 4,   // this entity and everything below it draw nothing
};
// CSprite2D::flags
enum : u32 {
    kSpriteFlipH       = 1,
    kSpriteFlipV       = 2,
    kSpriteNotCentered = 4,   // the offset is the top-left corner instead of the centre
    kSpriteHidden      = 8,
    kSpriteNearest     = 16,  // nearest-neighbour sampling instead of linear
};
// C2DLayer::flags
enum : u32 {
    kLayerHidden         = 1,
    kLayerFollowViewport = 2,   // the current camera's view applies to this layer (the default canvas always follows)
};
// CCamera2D::flags. Zero is a valid enabled camera centred on its transform.
enum : u32 {
    kCamDisabled       = 1,     // never the current camera
    kCamCurrent        = 2,     // preferred as the current camera (makeCameraCurrent sets it and clears the others')
    kCamFollowRotation = 4,     // the view turns with the camera's transform (the default ignores rotation)
    kCamSmoothing      = 8,     // the centre eases toward its goal at smoothSpeed
    kCamDragH          = 16,    // horizontal drag margins are active
    kCamDragV          = 32,
    kCamAnchorTopLeft  = 64,    // the transform is the view's top-left corner, not its centre
    kCamLimitLeft      = 128,   // limitLeft etc. apply (a limit of 0 is a real limit, so presence is a bit)
    kCamLimitTop       = 256,
    kCamLimitRight     = 512,
    kCamLimitBottom    = 1024,
};
// CParallax2D::flags
enum : u32 {
    kParallaxNoFollow   = 1,    // does not follow the camera: the layer stays in screen space (Godot's follow_viewport off)
    kParallaxLimitBegin = 2,    // limitBeginX/Y clamp the camera position the scroll uses
    kParallaxLimitEnd   = 4,
};
// CSpriteAnim2D::flags
enum : u32 {
    kAnimPlaying = 1,
    kAnimStarted = 2,    // the autoplay clip has been taken (it plays once, not again after stop())
};
// AnimClip2D::flags
enum : u32 { kClipNoLoop = 1 };
// CSprite2D::blend (the UiBlend values the draw list takes)
enum : i32 { kBlendMix = 0, kBlendAdd = 1, kBlendSub = 2, kBlendMul = 3 };

constexpr i32 kZMin = -4096;
constexpr i32 kZMax = 4096;

// Position, rotation (radians, clockwise on screen: y points down), scale and z-index of a 2D entity.
// The first eight words are the authored state; `seen` is the copy the last composition used.
struct C2DTransform {
    f32 posX, posY;
    f32 rotation;
    f32 scaleX, scaleY;       // 0 = 1
    i32 zIndex;
    u32 flags;
    u32 treeOrder;            // 0 = derive from the hierarchy; else the tree-order key (the caller's own tree)
    // ---- derived, read-only ----
    u32 seen[9];              // the authored words of the last composition (the 8 above, then the parent's id)
    f32 a, b, c, d, tx, ty;   // world affine: x' = a x + c y + tx, y' = b x + d y + ty
    i32 worldZ;               // effective z after the relative/absolute rule
    u32 worldRev;             // bumped each time the affine or z changed; 0 = never composed
    u32 parentRevSeen;        // the parent's worldRev at the last composition
};

// One sprite: a texture (or a region/frame of it) drawn at the entity's transform.
struct CSprite2D {
    i64 texture;              // asset id (fnv1a64 of the content path); 0 = nothing to draw
    f32 regionX, regionY, regionW, regionH;   // pixels; W or H of 0 = the whole image
    f32 offsetX, offsetY;
    u32 modulate;             // straight RGBA, 0xAABBGGRR; 0 = white
    u32 flags;
    i32 hframes, vframes;     // 0 = 1; the frame grid inside the region (or the whole image)
    i32 frame;
    i32 blend;                // kBlend*
};

// A canvas layer: sprites below it (nearest ancestor-or-self) sort by `index` and take its transform.
struct C2DLayer {
    i32 index;
    u32 flags;
    f32 offsetX, offsetY;
    f32 rotation;
    f32 scaleX, scaleY;       // 0 = 1
    u32 reserved;             // keeps the struct a multiple of 8; zero
};

constexpr i32 kMaxAnimClips = 16;
constexpr i32 kMaxAnimFrames = 128;   // across all the clips of one component
constexpr i32 kClipNameBytes = 24;    // incl. the terminator

// One frame: a texture and region (texture 0 = keep the sprite's own texture; region size 0 = the whole image), and a cell of the
// sprite's hframes x vframes. The frame owns the sprite's texture (when set), region and frame while the clip shows it.
struct AnimFrame2D {
    i64 texture;
    f32 regionX, regionY, regionW, regionH;
    f32 weight;               // relative length; 0 = 1. The frame lasts weight / (fps * speed scale) seconds
    i32 cell;                 // the sprite's `frame` while this one shows
};
// A named clip: frames [first, first + count) of the component's frame table.
struct AnimClip2D {
    char name[kClipNameBytes];
    i32 first, count;
    f32 fps;                  // 0 = 5 (Godot's SpriteFrames default)
    u32 flags;                // kClip*; zero loops
};
// Sprite animation: drives the CSprite2D on the same entity. Advanced by tick().
struct CSpriteAnim2D {
    f32 speedScale;           // 0 = 1; negative plays backwards
    u32 flags;                // kAnim*
    i32 clipCount, frameCount;
    i32 current;              // clip index
    i32 autoplay;             // clip index + 1 to start on the first tick; 0 = none
    i32 frame;                // frame within the current clip
    f32 progress;             // 0..1 through the current frame
    AnimClip2D clips[kMaxAnimClips];
    AnimFrame2D frames[kMaxAnimFrames];
};

// A 2D camera on an entity with a C2DTransform. See cameraView() for the maths.
struct CCamera2D {
    f32 zoomX, zoomY;         // 0 = 1; larger shows less
    f32 offsetX, offsetY;     // added to the transform's position to get the view centre
    f32 limitLeft, limitTop, limitRight, limitBottom;   // world units; each applies only with its flag
    f32 smoothSpeed;          // 0 = 5; the share of the way to the goal covered per second (capped at 1 per frame)
    f32 dragLeft, dragTop, dragRight, dragBottom;       // fractions of the half view around the centre inside which the target moves freely
    u32 flags;                // kCam*
    // ---- derived, read-only ----
    f32 centerX, centerY;     // the eased centre before the limits
    u32 centerValid;          // 1 once centerX/Y hold a value (else the next update snaps)
    u32 reserved;
};

// Parallax scrolling for the sprites below this entity (Godot 4.3 Parallax2D). The children are drawn displaced by
// cameraCentre * pin + scrollOffset + autoscroll * time, and tiled when a repeat size is set.
struct CParallax2D {
    f32 pinX, pinY;           // 1 - scroll_scale: 0 scrolls with the world (scale 1), 1 stays fixed on screen (scale 0), negative runs ahead
    f32 offsetX, offsetY;     // scroll_offset
    f32 repeatX, repeatY;     // repeat_size; 0 = no tiling on that axis
    i32 repeatTimes;          // copies on each side of the original; 0 = 1
    f32 autoX, autoY;         // autoscroll, world units per second
    f32 limitBeginX, limitBeginY, limitEndX, limitEndY;   // clamp the camera position the scroll uses (with their flags)
    u32 flags;                // kParallax*
    // ---- derived, read-only ----
    f32 timeX, timeY;         // the autoscroll accumulated so far
    u32 reserved, spare;      // zero
};

static_assert(sizeof(C2DTransform) == 4 * (8 + 9 + 6 + 3), "C2DTransform layout");
static_assert(sizeof(CSprite2D) == 56, "CSprite2D layout");
static_assert(sizeof(C2DLayer) == 32, "C2DLayer layout");
static_assert(sizeof(AnimFrame2D) == 32 && sizeof(AnimClip2D) == 40, "animation table layout");
static_assert(sizeof(CSpriteAnim2D) == 32 + 40 * kMaxAnimClips + 32 * kMaxAnimFrames, "CSpriteAnim2D layout");
static_assert(sizeof(CCamera2D) == 4 * 18, "CCamera2D layout");
static_assert(sizeof(CParallax2D) == 4 * 18, "CParallax2D layout");

struct Ids {
    u32 transform = 0, sprite = 0, layer = 0;
    u32 anim = 0, camera = 0, parallax = 0;
};

// Registers the six components with their field tables. Idempotent; returns the ids (all 0 on failure).
AVER_SCENE_API Ids registerComponents(scene::World& world);
// The ids as last registered in this process (0 until registerComponents ran).
AVER_SCENE_API Ids ids();

// 2D affine in the draw list's column form: x' = a x + c y + tx, y' = b x + d y + ty.
struct Affine2 {
    f32 a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    Affine2 operator*(const Affine2& o) const {   // o applies first
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }
};

// Attach helpers: the component is valid zero-filled, so these only add it. Return the component or nullptr.
AVER_SCENE_API C2DTransform* attachTransform(scene::World&, Entity);
AVER_SCENE_API CSprite2D* attachSprite(scene::World&, Entity, i64 texture);
AVER_SCENE_API C2DLayer* attachLayer(scene::World&, Entity, i32 index);
AVER_SCENE_API CSpriteAnim2D* attachAnim(scene::World&, Entity);
AVER_SCENE_API CCamera2D* attachCamera(scene::World&, Entity);
AVER_SCENE_API CParallax2D* attachParallax(scene::World&, Entity);

// Recomposes every C2DTransform whose authored state, parent or parent's composition changed. Returns how many it
// recomposed. Parents are handled before children whatever the pool order.
AVER_SCENE_API u32 updateTransforms(scene::World&);

// The entity's composed world affine and z; identity and 0 when it has no C2DTransform. Call updateTransforms first.
AVER_SCENE_API Affine2 worldAffine(scene::World&, Entity, i32* worldZ = nullptr);
// The scale/flag defaults, applied: what the zero-means-unset words read as.
inline f32 effScale(f32 s) { return s == 0.0f ? 1.0f : s; }
// A layer's own transform: translate(offset) * rotate * scale.
AVER_SCENE_API Affine2 layerAffine(const C2DLayer&);

// ---- sprite animation ----

// Replaces clip `index` (index == clipCount appends). Copies at most kClipNameBytes - 1 name bytes. False when the index, the
// name or the frame budget (kMaxAnimClips clips, kMaxAnimFrames frames in all) does not allow it. The frames of the other clips
// keep their order; the current clip and frame stay valid.
AVER_SCENE_API bool setClip(CSpriteAnim2D&, i32 index, const char* name, f32 fps, bool loop, const AnimFrame2D* frames, i32 count);
// The clip index of `name` (case-sensitive), or -1.
AVER_SCENE_API i32 findClip(const CSpriteAnim2D&, const char* name);
// Starts a clip from its first frame (its last when the speed scale is negative); a clip already playing keeps its frame unless
// `restart`. False for an unknown name or an empty clip. A null or empty name resumes the current clip.
AVER_SCENE_API bool playClip(scene::World&, Entity, const char* name, bool restart = false);
AVER_SCENE_API void stopClip(scene::World&, Entity);         // stops and rewinds to frame 0
AVER_SCENE_API void pauseClip(scene::World&, Entity);        // stops where it is
// Shows frame `frame` of the current clip (clamped), progress in [0,1). Queues a frame-changed event when it differs.
AVER_SCENE_API void setClipFrame(scene::World&, Entity, i32 frame, f32 progress = 0.0f);

enum : i32 { kAnimEventFrameChanged = 0, kAnimEventFinished = 1, kAnimEventLooped = 2, kAnimEventAnimationChanged = 3 };
struct AnimEvent {
    Entity entity;
    i32 kind;                 // kAnimEvent*
    i32 clip;
    i32 frame;
};
// Moves up to `max` queued events (oldest first) into `out`. The queue keeps the last 4096.
AVER_SCENE_API u32 drainAnimEvents(AnimEvent* out, u32 max);
AVER_SCENE_API void clearAnimEvents();

// ---- the frame step ----

// One step of everything time-driven: the 2D transforms, sprite animations (events queued), the parallax autoscroll and the
// current camera's drag and smoothing. dt <= 0 steps no time but still snaps an uninitialised camera. The hosts call it from the
// canvas frame; tests call it with their own dt.
AVER_SCENE_API void tick(scene::World&, f32 dt);
// The sprite-animation part of tick, alone: advances every playing CSpriteAnim2D by dt and writes the shown frame to its CSprite2D.
AVER_SCENE_API void stepAnimations(scene::World&, f32 dt);

// ---- camera ----

// The camera that applies: the first enabled one flagged current, else the first enabled one in pool order. kInvalidEntity with none.
AVER_SCENE_API Entity currentCamera(scene::World&);
// Flags `e` current and clears the flag on every other camera. False when `e` has no CCamera2D.
AVER_SCENE_API bool makeCameraCurrent(scene::World&, Entity e);
// The view centre of `e`'s camera in world units (eased centre, then the limits) for a view of contentW x contentH content units.
AVER_SCENE_API void cameraCenter(scene::World&, Entity e, f32 contentW, f32 contentH, f32 out[2]);
// World -> canvas content units for the current camera: translate(content/2) * rotate * scale(zoom) * translate(-centre).
// Identity with no camera or an empty view.
AVER_SCENE_API Affine2 cameraView(scene::World&, f32 contentW, f32 contentH);

// ---- parallax ----

// Where the sprites below a CParallax2D are drawn for a camera centred at (camX, camY): a shift in world units, the tiling
// (repeat size, and the copies on each side per axis; 0 = none on that axis) and whether the current camera's view applies to them.
struct ParallaxShift {
    f32 dx = 0, dy = 0;
    f32 repeatX = 0, repeatY = 0;
    i32 timesX = 0, timesY = 0;
    bool followsCamera = true;
};
AVER_SCENE_API ParallaxShift parallaxShift(const CParallax2D&, f32 camX, f32 camY);

} // namespace aver::scene2d
