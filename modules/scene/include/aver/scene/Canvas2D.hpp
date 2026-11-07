#pragma once
// The 2D canvas: an ordered submit into the UI draw list (docs/plans/RENDER_2D.md).
//
// Sprites from the scene and external items (Fabric's Control draws) go into ONE queue, sorted by the shared key
// (canvas layer, z, tree order), and flush() emits it into the draw list in that order. So a background sprite sits under
// its child buttons, and a button can sit between two sprites, exactly by layer and z.
//
// The canvas transform is the engine's viewport stretch (uiComputeStretch): content units -> window pixels. Sprites are
// drawn under it. An external item may ask for it (kExternalInCanvas) or draw in raw window pixels.
//
// Anything drawn into the list directly (aver_ui_* calls made outside a submitted item) is NOT part of the queue: it lands
// where it was drawn, before whatever the flush emits.
//
// One canvas per process, not thread-safe. Needs Aver.UI on the consumer's link line for the draw-list types.
#include "aver/scene/Entity.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiStretch.hpp"

namespace aver::scene {
class World;
}

namespace aver::scene2d {

// A sprite's texture as the draw list knows it. The host installs a resolver from the asset id.
struct TextureInfo {
    u64 ui = 0;       // the draw-list texture id (0 would be the white texture, so a resolver returns false instead)
    i32 w = 0, h = 0; // pixels
};
using TextureResolveFn = bool (*)(i64 assetId, TextureInfo* out, void* user);
// Null clears it, and sprites draw nothing.
AVER_SCENE_API void setTextureResolver(TextureResolveFn fn, void* user);

// The rectangle the game is drawn in, in window pixels, and the stretch rules for it.
struct CanvasView {
    f32 x = 0, y = 0, w = 0, h = 0;
};
AVER_SCENE_API void setCanvasView(const ui::UiStretchSettings& stretch, const CanvasView& view);

struct CanvasTransform {
    ui::UiStretchResult stretch;   // as uiComputeStretch returns it for the view's size (screen is view-relative)
    ui::UiAffine toWindow;         // content units -> window pixels: the view origin + the stretch offset and scale
    bool valid = false;            // false until a view with a size is set: then toWindow is the identity
};
AVER_SCENE_API CanvasTransform canvasTransform();

// The shared sort key. Lower draws first (under). `order` is the tree order: the caller's own tree index for an external
// item; for a sprite, C2DTransform::treeOrder when set, else the sprite's rank in the hierarchy.
struct SortKey {
    i32 layer = 0;
    i32 z = 0;
    u64 order = 0;
};

// An external item draws itself when its turn comes. `list` is the ui::UiDrawList being flushed into (a void* so the C ABI's
// callback type is this one); the item must draw into that list.
using ExternalDrawFn = void (*)(void* list, void* user);
enum : u32 {
    kExternalInCanvas = 1,   // the canvas transform is pushed around the callback: draw in content units
};
// Queues one external item. False when the queue is full or `fn` is null.
AVER_SCENE_API bool submitExternal(const SortKey& key, ExternalDrawFn fn, void* user, u32 flags = 0);

// Recomposes the 2D transforms and queues every visible sprite of `world`. Returns how many it queued.
AVER_SCENE_API u32 submitSprites(scene::World& world);
// Items waiting for flush().
AVER_SCENE_API u32 pendingCount();
// Empties the queue without drawing.
AVER_SCENE_API void dropQueue();
// Sorts the queue by SortKey (submission order breaks ties), emits it into `list` and empties the queue.
AVER_SCENE_API void flush(ui::UiDrawList& list);
// submitSprites + flush: the once-a-frame call a host makes after the game's UI scripts have run.
AVER_SCENE_API void frame(scene::World& world, ui::UiDrawList& list);
// Drops the queue, the resolver and the view.
AVER_SCENE_API void reset();

// How a sprite's quad is laid out: the destination rectangle in the entity's local space and the source UVs.
struct SpriteQuad {
    f32 x = 0, y = 0, w = 0, h = 0;
    f32 u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    bool ok = false;   // false when the image or region is unusable
};
AVER_SCENE_API SpriteQuad spriteQuad(const CSprite2D& s, i32 texW, i32 texH);

} // namespace aver::scene2d
