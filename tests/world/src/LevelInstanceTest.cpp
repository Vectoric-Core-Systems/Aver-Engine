// Aver.World: turning parsed .ocworld placements into entities. Exit code = failure count.
//
// WHY THIS EXISTS. The placement loop lived in two places -- Runtime/src/GameLevel.cpp
// and sandbox/src/SandboxApp.cpp -- and NEITHER was covered by a test: there is no .ocworld anywhere
// in this repository, every real level lives in an out-of-tree project, and the only thing that ever
// exercised either copy was a human opening the editor. The two had already drifted apart by the
// time they were merged.
//
// So the merge needed an oracle that does not need a level file, a GPU, a device or .NET. This is
// it: the world text is a string literal, the assertions are on entity state, and the whole thing
// runs headless. It is also the direct precursor to the "chunked equals flat" test docs/CHUNKS.md
// names as the strongest check in the streaming plan -- that one compares two instantiations, and
// this one establishes what a single instantiation is supposed to produce.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/world/LevelInstance.hpp"
#include "aver/world/LevelTransform.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    ++g_checks;
    if (std::fabs(got - want) <= tol) return;
    AVER_ERROR("   FAIL  {}: got {} want {}", what, got, want);
    ++g_failures;
}

// A world exercising the cases that matter: a shared surface, a distinct surface, no surface at all,
// nocollide, a negative coordinate, and a rotation on every axis.
//
// THE NEGATIVE COORDINATE IS NOT DECORATION. docs/CHUNKS.md puts the region -> chunk mapping on a
// floor division, and the tree already has one truncate-vs-floor bug of exactly this shape
// (PcgVolume.cpp:50-52 truncates where :78-80 floors). A placement at a negative position is the
// cheapest standing guard against the next one.
static const char* kWorldText = R"(OCWORLD 1
ID 0x00000000000000AB
NAME UnitLevel
BUILD 7
ALGO 3
SPAWN -600 0 20 90
FOG exp density 0.00002 color 0.7 0.78 0.88
PCGVOLUME name Sky seed 3 cell 1600 octaves 4 floor 0.45 bias 1.6 infinite
PLACEG Meshes/cube.ocmesh 0 0 -10 0 0 0 800 800 10 M_Floor
PLACEG Meshes/cube.ocmesh -1600 -3200 150 0 0 0 20 800 150 M_Wall
PLACEG Meshes/cube.ocmesh 100 200 300 10 20 30 1 2 3 M_Floor
PLACEG Meshes/cube.ocmesh 5 5 5 0 0 0 2 2 2 M_Accent nocollide hidden
PLACEG Meshes/cube.ocmesh 7 8 9 0 0 0 1 1 1
)";

int main() {
    fmt::OcWorldData w;
    std::string why;
    check(fmt::parseOcworld(kWorldText, w, &why), "the test world parses: " + why);
    check(w.placements.size() == 5, "five placements parsed");
    if (w.placements.size() != 5) {
        AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
        return g_failures ? g_failures : 1;
    }

    // ---- the header the editor's save must carry, not rebuild -------------------------------------
    // Every one of these was silently reset by SandboxApp::saveLevel before slice 0: SPAWN was
    // deleted, BUILD went to 0, ID was recomputed from NAME. They are asserted here because the fix
    // is "start the save from what was parsed", so what was parsed has to be right.
    check(w.hasSpawn, "SPAWN survived the parse");
    checkNear(static_cast<f32>(w.spawnX), -600.0f, 0.0f, "spawn x");
    checkNear(static_cast<f32>(w.spawnYaw), 90.0f, 0.0f, "spawn yaw");
    check(w.build == 7, "BUILD survived the parse");
    check(w.contentId == 0xABull, "ID survived the parse and was not recomputed from NAME");

    // ---- CAMERA: the editor viewpoint, which is NOT the player spawn ------------------------------
    // The world text above deliberately has no CAMERA line, because that is every level authored
    // before the record existed and the absence has to stay meaningful: hasCamera false means "no
    // opinion, frame it the way you always did", and a level that silently gained a stored origin
    // would send the camera to 0,0,0 on open.
    check(!w.hasCamera, "a level with no CAMERA line does NOT claim to have one");
    {
        fmt::OcWorldData cw;
        std::string cwhy;
        check(fmt::parseOcworld(
                  "OCWORLD 1\nNAME cam\n"
                  "CAMERA 1200 -3400 950 -135.5 -22.25 6400\n"
                  "PLACEG Meshes/cube.ocmesh 0 0 0 0 0 0 1 1 1\n", cw, &cwhy),
              "a world with a CAMERA line parses: " + cwhy);
        check(cw.hasCamera, "CAMERA sets hasCamera");
        checkNear(static_cast<f32>(cw.camX), 1200.0f, 0.0f, "camera x");
        checkNear(static_cast<f32>(cw.camY), -3400.0f, 0.0f, "camera y");
        checkNear(static_cast<f32>(cw.camZ), 950.0f, 0.0f, "camera z");
        checkNear(static_cast<f32>(cw.camYaw), -135.5f, 0.0f, "camera yaw survives a negative");
        checkNear(static_cast<f32>(cw.camPitch), -22.25f, 0.0f, "camera pitch survives a fraction");
        checkNear(static_cast<f32>(cw.camSpeed), 6400.0f, 0.0f, "camera speed");

        // ROUND TRIP, which is the assertion that actually protects the feature: a viewpoint that
        // parses but does not write back is the same to a user as one that was never saved.
        const std::string out = fmt::writeOcworld(cw);
        fmt::OcWorldData back;
        std::string bwhy;
        check(fmt::parseOcworld(out, back, &bwhy), "the written world re-parses: " + bwhy);
        check(back.hasCamera, "CAMERA survives a write/parse round trip");
        checkNear(static_cast<f32>(back.camX), 1200.0f, 0.0f, "round-tripped camera x");
        checkNear(static_cast<f32>(back.camYaw), -135.5f, 0.0f, "round-tripped camera yaw");
        checkNear(static_cast<f32>(back.camPitch), -22.25f, 0.0f, "round-tripped camera pitch");
        checkNear(static_cast<f32>(back.camSpeed), 6400.0f, 0.0f, "round-tripped camera speed");

        // A SHORT LINE MUST NOT FAIL THE PARSE. The speed is optional, so a five-token CAMERA has to
        // degrade to "unstated" rather than rejecting the whole level -- an editor that refuses to
        // open a file over a trailing number is worse than one that forgets a fly speed.
        fmt::OcWorldData sw;
        std::string swhy;
        check(fmt::parseOcworld("OCWORLD 1\nNAME s\nCAMERA 1 2 3 4 5\n", sw, &swhy),
              "a CAMERA line with no speed still parses: " + swhy);
        check(sw.hasCamera, "the short CAMERA line still sets hasCamera");
        checkNear(static_cast<f32>(sw.camSpeed), 0.0f, 0.0f, "a missing speed reads as 0 = unstated");

        // AND A LEVEL WITHOUT ONE MUST NOT GROW ONE. writeOcworld is what saveLevel calls, so if it
        // emitted CAMERA unconditionally every untouched level in the repo would gain a line the
        // first time it was opened and saved.
        fmt::OcWorldData nw;
        check(fmt::parseOcworld("OCWORLD 1\nNAME n\n", nw, &swhy), "a world with no CAMERA parses");
        check(fmt::writeOcworld(nw).find("CAMERA") == std::string::npos,
              "a level with no camera writes NO CAMERA line");
    }

    // ---- instantiation ---------------------------------------------------------------------------
    std::vector<std::pair<i32, std::string>> bound;
    world::InstantiateOptions opt;
    opt.bindMaterial = [&bound](i32 token, const std::string& surface) {
        bound.emplace_back(token, surface);
    };

    const world::LevelInstance inst = world::instantiate(w, opt);

    check(inst.entities.size() == 5, "one entity per placement");
    check(inst.placementIndex.size() == inst.entities.size(), "placementIndex is parallel to entities");
    check(inst.entityBody.size() == inst.entities.size(), "entityBody is parallel to entities");
    for (usize k = 0; k < inst.placementIndex.size(); ++k)
        check(inst.placementIndex[k] == static_cast<u32>(k),
              "placementIndex is the identity when every placement succeeds");

    // Physics is not initialised in a headless test, so aver_phys_ready() is false and NO body is
    // created -- including for the four colliding placements. That is the documented behaviour and
    // not a failure; the point of asserting it is that `bodies` stays empty rather than filling with
    // whatever aver_phys_add_static_box returns when the world does not exist.
    check(inst.bodies.empty(), "no static bodies without aver_phys_init");
    for (const i32 b : inst.entityBody) check(b == -1, "every entityBody is -1 without physics");

    scene::World& world = scene::World::instance();

    // ---- transforms ------------------------------------------------------------------------------
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const scene::Entity e = inst.entities[k];
        const fmt::OcWorldPlacement& p = w.placements[inst.placementIndex[k]];
        const auto* loc = world.component<scene::CLocal>(e, scene::kComponentLocal);
        check(loc != nullptr, "the entity has a CLocal");
        if (!loc) continue;
        checkNear(loc->xf.position.x, static_cast<f32>(p.x), 0.0f, "position x narrows exactly");
        checkNear(loc->xf.position.y, static_cast<f32>(p.y), 0.0f, "position y narrows exactly");
        checkNear(loc->xf.position.z, static_cast<f32>(p.z), 0.0f, "position z narrows exactly");
        checkNear(loc->xf.scale.x, static_cast<f32>(p.sx), 0.0f, "scale x narrows exactly");
        checkNear(loc->xf.scale.z, static_cast<f32>(p.sz), 0.0f, "scale z narrows exactly");

        // The rotation contract, against the one definition of it there now is.
        const Quat want = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll),
                                                       static_cast<f32>(p.pitch),
                                                       static_cast<f32>(p.yaw)});
        checkNear(loc->xf.rotation.x, want.x, 1e-6f, "rotation x matches quatFromEulerDeg");
        checkNear(loc->xf.rotation.y, want.y, 1e-6f, "rotation y matches quatFromEulerDeg");
        checkNear(loc->xf.rotation.z, want.z, 1e-6f, "rotation z matches quatFromEulerDeg");
        checkNear(loc->xf.rotation.w, want.w, 1e-6f, "rotation w matches quatFromEulerDeg");

        check(std::string(world.name(e)) == p.asset, "the entity's CName is the asset path");
    }

    // The rotated placement is the one that would catch a transposed composition; assert it is
    // actually rotated rather than trusting the loop above to have had something to compare.
    {
        const auto* loc = world.component<scene::CLocal>(inst.entities[2], scene::kComponentLocal);
        check(loc && std::fabs(loc->xf.rotation.w - 1.0f) > 1e-4f,
              "the 10/20/30-degree placement really is rotated");
    }

    // ---- mesh renderer and material interning ----------------------------------------------------
    const auto* mr0 = world.component<scene::CMeshRenderer>(inst.entities[0], scene::kComponentMeshRenderer);
    const auto* mr1 = world.component<scene::CMeshRenderer>(inst.entities[1], scene::kComponentMeshRenderer);
    const auto* mr2 = world.component<scene::CMeshRenderer>(inst.entities[2], scene::kComponentMeshRenderer);
    const auto* mr3 = world.component<scene::CMeshRenderer>(inst.entities[3], scene::kComponentMeshRenderer);
    const auto* mr4 = world.component<scene::CMeshRenderer>(inst.entities[4], scene::kComponentMeshRenderer);
    check(mr0 && mr1 && mr2 && mr3 && mr4, "every placement got a CMeshRenderer");
    if (!(mr0 && mr1 && mr2 && mr3 && mr4)) {
        AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
        return g_failures ? g_failures : 1;
    }

    check((mr0->flags & scene::kMeshRendererVisible) != 0, "the mesh renderer is visible");
    check(mr0->mesh == w.placements[0].objectId, "mesh id is the placement's objectId");

    // `hidden` (placement 3, alongside its own `nocollide`) must NOT set the bit -- instantiate()
    // used to OR it in unconditionally regardless of what the placement asked for.
    check((mr3->flags & scene::kMeshRendererVisible) == 0,
          "a placement authored `hidden` instantiates with the visible bit CLEAR");

    check(mr0->material != 0, "an authored surface interns to a non-zero token");
    check(mr0->material == mr2->material, "the same surface name interns to the same token");
    check(mr0->material != mr1->material, "different surface names intern to different tokens");
    check(mr4->material == 0, "a placement with no surface gets token 0");

    // ---- the new inverse, which is what lets a save write the NAME and never the token ------------
    check(std::string(aver_scene_material_name(mr0->material)) == "M_Floor",
          "aver_scene_material_name returns the name the token was interned under");
    check(std::string(aver_scene_material_name(mr1->material)) == "M_Wall",
          "...and does so per token, not just for the first");
    check(std::string(aver_scene_material_name(0)).empty(), "token 0 maps to the empty name");
    check(std::string(aver_scene_material_name(999999)).empty(), "an unknown token maps to the empty name");
    check(aver_scene_material_name(-1) != nullptr, "a negative token returns \"\" and never NULL");

    // ---- the bindMaterial hook -------------------------------------------------------------------
    // Four placements name a surface, one does not. The hook must fire for exactly the four, in
    // placement order, because both hosts intern further materials from inside it and a different
    // order would hand out different tokens -- which the render gates compare bit-exactly.
    check(bound.size() == 4, "bindMaterial fired once per placement with a surface");
    if (bound.size() == 4) {
        check(bound[0].second == "M_Floor" && bound[1].second == "M_Wall" &&
              bound[2].second == "M_Floor" && bound[3].second == "M_Accent",
              "bindMaterial fired in placement order");
        check(bound[0].first == mr0->material, "bindMaterial got the token the component carries");
    }

    // ---- collide, which has no component and must be read off the placement -----------------------
    check(w.placements[3].collide == false, "nocollide parsed");
    check(w.placements[0].collide == true, "an unmarked placement collides");

    // ---- visible, off the placement too, mirroring collide exactly ---------------------------------
    check(w.placements[3].visible == false, "hidden parsed");
    check(w.placements[0].visible == true, "an unmarked placement is visible");

    // ---- determinism: a second instantiation of the same data agrees ------------------------------
    // Entity handles differ (the first five are still live), so this compares STATE, which is what a
    // region file would have to reproduce. Material tokens must NOT differ: the names are already
    // interned, so the second pass has to find them rather than mint new ones.
    const world::LevelInstance again = world::instantiate(w, world::InstantiateOptions{});
    check(again.entities.size() == inst.entities.size(), "the second instantiation makes as many entities");
    for (usize k = 0; k < again.entities.size() && k < inst.entities.size(); ++k) {
        const auto* a = world.component<scene::CLocal>(inst.entities[k], scene::kComponentLocal);
        const auto* b = world.component<scene::CLocal>(again.entities[k], scene::kComponentLocal);
        check(a && b, "both instantiations produced a CLocal");
        if (!a || !b) continue;
        check(a->xf.position.x == b->xf.position.x && a->xf.position.y == b->xf.position.y &&
              a->xf.position.z == b->xf.position.z, "positions are bit-identical across instantiations");
        check(a->xf.rotation.w == b->xf.rotation.w, "rotations are bit-identical across instantiations");
    }
    {
        const auto* a = world.component<scene::CMeshRenderer>(inst.entities[0], scene::kComponentMeshRenderer);
        const auto* b = world.component<scene::CMeshRenderer>(again.entities[0], scene::kComponentMeshRenderer);
        check(a && b && a->material == b->material,
              "an already-interned surface reuses its token rather than minting a new one");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
