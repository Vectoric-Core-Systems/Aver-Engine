// The world draw walk: every live entity carrying a visible CMeshRenderer.
//
// Lifted from SandboxApp.cpp:1386-1510. What is NOT here is as deliberate as what is: no selection
// latch, no selection outline, no grid, no gizmo, no skin-scene-test recolour, no objects_
// placeholder pass, and no capture/gate harness. Those are the editor looking at a world; this is a
// game showing one.
#include "aver/game/GameRender.hpp"

#if AVER_MODULE_SCENE

#include "aver/game/GameContent.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#if AVER_MODULE_PBR
#  include "aver/pbr/MaterialSystem.hpp"
#endif
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"
#include "GameMath.hpp"

#include <cmath>

namespace aver::game {

void drawWorld(rhi::IDevice& device, const Mat4& viewProj, GameContent& content, SceneDrawStats& stats,
               pbr::MaterialSystem* materials) {
    scene::World& w = scene::World::instance();
    int drawn = 0, culled = 0;

    // The six frustum planes, from the camera's viewProj. ENGINE convention: row-vector, so a clip
    // coordinate is a dot with a COLUMN, and each plane is a sum or difference of two columns.
    // Derived per frame rather than cached: it is two dozen adds, and a stale frustum culls things
    // that are on screen. Left UNNORMALISED -- only the sign of the distance is ever read.
    f32 pl[6][4];
    {
        const Mat4& m = viewProj;
        for (int i = 0; i < 4; ++i) {
            pl[0][i] = m.m[i][3] + m.m[i][0];   // left
            pl[1][i] = m.m[i][3] - m.m[i][0];   // right
            pl[2][i] = m.m[i][3] + m.m[i][1];   // bottom
            pl[3][i] = m.m[i][3] - m.m[i][1];   // top
            pl[4][i] = m.m[i][2];               // near   ([0,1] depth, so no m[i][3] term)
            pl[5][i] = m.m[i][3] - m.m[i][2];   // far
        }
    }

    const u32 n = w.count();
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity ent = w.at(i);
        if (w.destroyPending(ent)) continue;
        const scene::CMeshRenderer* mr =
            w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
        if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;

        const rhi::MeshHandle handle = content.meshFor(mr->mesh);
        if (!handle) continue;

        // worldMatrix is non-const on World, which is why this takes a non-const World&.
        const Mat4& wm = w.worldMatrix(ent);

        // A STATIC entity gets its bounds from the asset. A skinned one would already have had them
        // written this frame by SkinnedScene from its ACTUAL POSE -- overwriting those with the rest
        // box is exactly the popping the posed-bounds work exists to stop. Skinning is not wired
        // into the game yet, so every entity here is static; the guard is written now so that
        // turning skinning on later cannot silently reintroduce the bug.
        if (const auto* b = content.boundsFor(mr->mesh)) {
            auto* mw = const_cast<scene::CMeshRenderer*>(mr);
            mw->aabbMin[0] = b->first.x;  mw->aabbMin[1] = b->first.y;  mw->aabbMin[2] = b->first.z;
            mw->aabbMax[0] = b->second.x; mw->aabbMax[1] = b->second.y; mw->aabbMax[2] = b->second.z;
        }

        // Frustum cull on the world-space extent of the entity's own box. A DEGENERATE box is DRAWN
        // rather than culled: an entity whose bounds were never filled in must not vanish, and being
        // conservative costs a draw call where being wrong costs a character.
        {
            const Vec3 lo{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            const Vec3 hi{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) {
                Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
                for (u32 c = 0; c < 8; ++c) {
                    const Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
                    const Vec3 t = xformPoint(wm, p);
                    wlo.x = std::fmin(wlo.x, t.x); whi.x = std::fmax(whi.x, t.x);
                    wlo.y = std::fmin(wlo.y, t.y); whi.y = std::fmax(whi.y, t.y);
                    wlo.z = std::fmin(wlo.z, t.z); whi.z = std::fmax(whi.z, t.z);
                }
                bool outside = false;
                for (u32 pi = 0; pi < 6 && !outside; ++pi) {
                    // The corner FURTHEST along the plane normal. If even that one is behind, every
                    // corner is, and only then is the box definitely out.
                    const f32 d = pl[pi][0] * (pl[pi][0] > 0 ? whi.x : wlo.x)
                                + pl[pi][1] * (pl[pi][1] > 0 ? whi.y : wlo.y)
                                + pl[pi][2] * (pl[pi][2] > 0 ? whi.z : wlo.z)
                                + pl[pi][3];
                    if (d < 0.0f) outside = true;
                }
                if (outside) { ++culled; continue; }
            }
        }

        // Colour: the named-surface look, or a neutral default. The authored-material half
        // (surfaceMaterials_ and MaterialSystem) is C8's; until then a level's M_* surfaces already
        // render with the colours the editor gives them.
        f32 col[4] = {0.80f, 0.80f, 0.85f, 1.0f};
        f32 metallic = 0.0f, roughness = 0.5f;

        u32 authored = 0;
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
        authored = content.authoredFor(mr->material);
#endif
        if (authored) {
            // An AUTHORED material supplies its own colour and its own metal/rough through the
            // binding set below, so the per-draw values are neutralised to 1 rather than left as
            // the fallback. Multiplying an authored albedo by 0.8 grey is the classic way to get a
            // world that looks correct but uniformly dingy.
            col[0] = col[1] = col[2] = 1.0f;
            metallic = roughness = 1.0f;
        } else if (const GameContent::SurfaceLook* look = content.lookFor(mr->material)) {
            col[0] = look->col[0]; col[1] = look->col[1]; col[2] = look->col[2];
            metallic = look->metallic; roughness = look->roughness;
        }

#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // Guarded on ready(): binding a descriptor table the material system has not built is not a
        // wrong colour on this renderer, it is a GPU hang. This project has already lost a session
        // to an unbound root CBV that presented as "slow geometry shaders".
        if (materials && materials->ready()) {
            device.setDrawBinding(materials->bindingSet(authored), &materials->constants(authored),
                                  sizeof(pbr::MaterialConstants));
        }
#else
        (void)materials;
#endif

        device.drawMesh(handle, &wm.m[0][0], col, metallic, roughness);
        ++drawn;
    }

    if (drawn != stats.lastDrawn || culled != stats.lastCulled) {
        AVER_INFO("[Game] scene-render: {} drawn, {} frustum-culled", drawn, culled);
        stats.lastDrawn = drawn;
        stats.lastCulled = culled;
    }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
