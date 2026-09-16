// The world draw walk.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/Entity.hpp"

namespace aver::rhi { class IDevice; }
namespace aver::pbr { class MaterialSystem; }
namespace aver::render { class SkinnedScene; }
namespace aver::voxi { class VoxiRenderer; }

namespace aver::game {

class GameContent;

// Draw counters, kept across frames so the log line fires on CHANGE rather than every frame. Start
// at -1 so the first frame always reports, including a first frame that drew nothing -- "0 drawn"
// is the single most useful line when a world fails to appear, and a counter starting at 0 would
// swallow it.
struct SceneDrawStats {
    int lastDrawn = -1;
    int lastCulled = -1;
    int lastOwnerHidden = -1;
};

#if AVER_MODULE_SCENE
// Widens drawWorld's cull/owner-hide behaviour without widening its required arguments -- every
// field here defaults to "off", so a default-constructed DrawWorldOptions reproduces the walk this
// struct was added to change not one bit.
struct DrawWorldOptions {
    // The possessed first-person pawn. Entities flagged kMeshRendererHiddenFromOwner under it (ancestor walk) are not drawn
    // in the raster pass. scene::kInvalidEntity = no owner-hide.
    scene::Entity ownerHideRoot = scene::kInvalidEntity;

    // Where a frustum-culled or owner-hidden entity's draw goes INSTEAD of the raster pass -- mirrors
    // SandboxRender.cpp's "unified direct route" (its emitEntityDraws' else-branch calling
    // voxiRenderer_.submit()): device.drawMesh() is not an option here, because drawMesh both
    // broadcasts to every registered IRenderFeature AND rasterises to the backbuffer, and the whole
    // point of this route is the first half without the second (an off-screen caster must reach
    // Voxi's shadow/GI/TLAS submission without appearing on screen; an owner-hidden mesh is usually
    // sitting squarely IN the frustum, so drawMesh() would draw it regardless of culling). Null (a
    // game with no Voxi feature, or a caller that has not attached one yet) reproduces today's
    // behaviour exactly: a culled or owner-hidden entity is skipped and casts no shadow while it is.
    voxi::VoxiRenderer* voxiRenderer = nullptr;
};

// Draws every live entity carrying a visible CMeshRenderer whose mesh resolves.
//
// Takes the device and the content rather than the app: the walk needs a mesh table and somewhere
// to send triangles, and nothing else. Takes viewProj by value-ref because the frustum is derived
// from it per frame.
// `materials` may be null: a game with no Voxi feature has no material system, and every
// surface then draws with its named-surface look or the neutral fallback.
// `skinning` may be null: init compiles HLSL at runtime and can fail on a machine where the build
// was green, and a null feature means skinned entities draw at their REST POSE rather than not at
// all. A character that fails to skin must still appear.
void drawWorld(rhi::IDevice& device, const Mat4& viewProj, GameContent& content, SceneDrawStats& stats,
               pbr::MaterialSystem* materials, render::SkinnedScene* skinning,
               const DrawWorldOptions& options = {});
#endif

} // namespace aver::game
