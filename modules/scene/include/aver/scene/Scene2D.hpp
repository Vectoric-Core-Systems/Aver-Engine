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
    kLayerFollowViewport = 2,   // stored for the camera that comes later; today it changes nothing
};
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

static_assert(sizeof(C2DTransform) == 4 * (8 + 9 + 6 + 3), "C2DTransform layout");
static_assert(sizeof(CSprite2D) == 56, "CSprite2D layout");
static_assert(sizeof(C2DLayer) == 32, "C2DLayer layout");

struct Ids {
    u32 transform = 0, sprite = 0, layer = 0;
};

// Registers the three components with their field tables. Idempotent; returns the ids (all 0 on failure).
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

// Recomposes every C2DTransform whose authored state, parent or parent's composition changed. Returns how many it
// recomposed. Parents are handled before children whatever the pool order.
AVER_SCENE_API u32 updateTransforms(scene::World&);

// The entity's composed world affine and z; identity and 0 when it has no C2DTransform. Call updateTransforms first.
AVER_SCENE_API Affine2 worldAffine(scene::World&, Entity, i32* worldZ = nullptr);
// The scale/flag defaults, applied: what the zero-means-unset words read as.
inline f32 effScale(f32 s) { return s == 0.0f ? 1.0f : s; }
// A layer's own transform: translate(offset) * rotate * scale.
AVER_SCENE_API Affine2 layerAffine(const C2DLayer&);

} // namespace aver::scene2d
