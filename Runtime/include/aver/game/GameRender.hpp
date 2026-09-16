// The world draw walk.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

namespace aver::rhi { class IDevice; }
namespace aver::pbr { class MaterialSystem; }
namespace aver::render { class SkinnedScene; }

namespace aver::game {

class GameContent;

// Draw counters, kept across frames so the log line fires on CHANGE rather than every frame. Start
// at -1 so the first frame always reports, including a first frame that drew nothing -- "0 drawn"
// is the single most useful line when a world fails to appear, and a counter starting at 0 would
// swallow it.
struct SceneDrawStats {
    int lastDrawn = -1;
    int lastCulled = -1;
};

#if AVER_MODULE_SCENE
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
               pbr::MaterialSystem* materials, render::SkinnedScene* skinning);
#endif

} // namespace aver::game
