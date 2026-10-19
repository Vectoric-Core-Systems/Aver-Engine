// CameraRigRecordTest -- the COMPONENT CameraRig record (docs/CAMERAS.md, docs/formats/FORMAT_SPECS.md): parse, apply, capture
// and the text round trip, with golden canonical strings. A real scene::World; no device, no window.
//
// What these pin: `preset` is always written and every other key only when it differs from the PRESET's value (so a
// VehicleChase record stays short), flags are written as 0/1 with the negative-sense ones defaulting to 1, `target` is `self` (the
// rig's own entity, the default) or `parent`, runtime state (ignoreBody, the OUT fields) is never saved, an unknown key survives
// the file, and a hand-written record with only `preset=seat` is a Seat rig.
#include "aver/camera/CameraRigSystem.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelComponents.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <cstdio>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using camera::CCameraRig;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static fmt::OcPlacedComponent rec(const char* type, std::initializer_list<std::pair<const char*, const char*>> kv = {}) {
    fmt::OcPlacedComponent r;
    r.type = type;
    for (const auto& p : kv) r.kv.emplace_back(p.first, p.second);
    return r;
}

// An entity that carries a mesh, like every level placement.
static scene::Entity mesh(scene::World& w, const char* name, scene::Entity parent = scene::kInvalidEntity) {
    const scene::Entity e = w.create(name, parent, Transform{});
    auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
    *mr = scene::CMeshRenderer{};
    return e;
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

static std::string show(const Pairs& p) {
    std::string s;
    for (const auto& kv : p) s += kv.first + "=" + kv.second + " ";
    return s;
}

static bool captured(scene::World& w, scene::Entity e, const Pairs& want, const char* what) {
    const game::PathOfIdFn none = [](u64) { return std::string{}; };
    const auto c = game::capturePlacedComponent(w, e, "CameraRig", none);
    const bool ok = c && c->type == "CameraRig" && c->kv == want;
    check(ok, std::string(what) + (ok ? "" : "  got [" + (c ? show(c->kv) : std::string("nothing")) + "] wanted [" + show(want) + "]"));
    return ok;
}

int main() {
    AVER_INFO("CameraRigRecordTest");
    scene::World& w = scene::World::instance();
    const u32 type = camera::CameraRigSystem::registerComponents(w);
    check(type != 0, "CCameraRig is registered");

    {
        AVER_INFO("a hand-written record with only preset=seat");
        const scene::Entity e = mesh(w, "Seat");
        check(game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "seat"}})), "applies");
        const auto* c = w.component<CCameraRig>(e, type);
        check(c && c->mode == static_cast<u32>(camera::CameraMode::Seat) && c->preset == static_cast<u32>(camera::CameraPreset::Seat) &&
                  c->posLagSpeed == 25.0f && c->seatYawLimitDeg == 110.0f && c->blendTimeS == 0.6f,
              "it is the Seat preset");
        check(c && c->target == e, "target defaults to the rig's own entity");
        const auto* cam = w.component<scene::CCamera>(e, scene::kComponentCamera);
        check(cam && cam->nearCm == 2.0f && cam->farCm == 200000.0f && near(cam->fovYRad, radians(60.0f)), "and CCamera is attached with today's constants");
        captured(w, e, {{"preset", "seat"}}, "capture writes just the preset");
        check(game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "seat"}})), "applying again is an update");
        captured(w, e, {{"preset", "seat"}}, "and leaves the same record");
    }

    {
        AVER_INFO("a key is omitted when it equals the preset's value");
        const scene::Entity e = mesh(w, "Car");
        check(game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "vehiclechase"}, {"arm", "600"}, {"lag", "8"}, {"collision", "1"}})),
              "VehicleChase with keys equal to the preset's applies");
        captured(w, e, {{"preset", "vehiclechase"}}, "...and they are not written back");
        check(game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "vehiclechase"}, {"arm", "700"}, {"socket", "1,2,3"}, {"mask", "ff"}, {"collision", "0"}, {"curve", "linear"}})),
              "overrides apply");
        const auto* c = w.component<CCameraRig>(e, type);
        check(c && c->armLengthCm == 700.0f && c->socketX == 1.0f && c->socketY == 2.0f && c->socketZ == 3.0f && c->collisionMask == 0xFFu &&
                  (c->flags & camera::kRigNoCollision) && c->blendCurve == static_cast<u32>(camera::BlendCurve::Linear),
              "arm, socket, mask, collision and curve land in the component");
        check(c && c->posLagSpeed == 8.0f && c->pivotZ == 120.0f && (c->flags & camera::kRigNoInheritPitch) && (c->flags & camera::kRigNoFade),
              "while the rest stays the preset's");
        captured(w, e, {{"preset", "vehiclechase"}, {"arm", "700"}, {"socket", "1,2,3"}, {"curve", "linear"}, {"mask", "ff"}, {"collision", "0"}},
                 "capture is canonical: preset, arm, socket, curve, mask, then flags, as 0/1");
    }

    {
        AVER_INFO("flags: negative-sense keys default to 1, positive ones to 0");
        const scene::Entity e = mesh(w, "Flags");
        game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "chase"}, {"inheritpitch", "0"}, {"lookat", "1"}, {"fade", "0"}}));
        const auto* c = w.component<CCameraRig>(e, type);
        check(c && (c->flags & camera::kRigNoInheritPitch) && (c->flags & camera::kRigLookAtTarget) && (c->flags & camera::kRigNoFade) &&
                  !(c->flags & camera::kRigNoCollision) && !(c->flags & camera::kRigNoInheritYaw),
              "inheritpitch=0, lookat=1 and fade=0 set their bits, nothing else");
        captured(w, e, {{"preset", "chase"}, {"inheritpitch", "0"}, {"fade", "0"}, {"lookat", "1"}}, "and capture writes them in canonical order");
        game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "chase"}}));
        captured(w, e, {{"preset", "chase"}}, "re-applying the bare record clears them");
    }

    {
        AVER_INFO("target=parent");
        const scene::Entity parent = mesh(w, "Vehicle");
        const scene::Entity e = mesh(w, "Seat", parent);
        check(game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "seat"}, {"target", "parent"}})), "applies");
        const auto* c = w.component<CCameraRig>(e, type);
        check(c && c->target == parent, "the arm follows the parent");
        captured(w, e, {{"preset", "seat"}, {"target", "parent"}}, "and capture writes target=parent");
        game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "seat"}}));
        check(w.component<CCameraRig>(e, type)->target == e, "target=self (absent) points back at the rig's own entity");
    }

    {
        AVER_INFO("runtime state is never saved");
        const scene::Entity e = mesh(w, "Runtime");
        game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "orbit"}}));
        auto* c = w.component<CCameraRig>(e, type);
        c->ignoreBody = 7;
        c->outArmCm = 33.0f;
        c->outState = 0xFF;
        c->outFovDeg = 99.0f;
        c->flags |= camera::kRigCut;
        captured(w, e, {{"preset", "orbit"}}, "ignoreBody, the OUT fields and the one-shot cut bit do not reach the record");
    }

    {
        AVER_INFO("refusals");
        const scene::Entity e = mesh(w, "Bad");
        check(!game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "sideways"}})), "an unknown preset is refused");
        check(!game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "chase"}, {"mode", "hover"}})), "an unknown mode is refused");
        check(!game::applyPlacedComponent(w, e, rec("CameraRig", {{"preset", "chase"}, {"curve", "wobbly"}})), "an unknown curve is refused");
        check(!w.hasComponent(e, type), "and a refusal leaves no half-written component");
        const scene::Entity bare = w.create("NoMesh", scene::kInvalidEntity, Transform{});
        check(!game::applyPlacedComponent(w, bare, rec("CameraRig", {{"preset", "chase"}})), "an entity with no mesh is refused (a rig needs a placement)");
        check(game::removePlacedComponent(w, mesh(w, "Gone"), "CameraRig") == false, "removing from an entity without one is false");
        const scene::Entity r = mesh(w, "Removed");
        game::applyPlacedComponent(w, r, rec("CameraRig", {{"preset", "fixed"}}));
        check(game::removePlacedComponent(w, r, "CameraRig") && !w.hasComponent(r, type), "removePlacedComponent takes it off");
    }

    {
        AVER_INFO("the text form");
        const std::string text =
            "OCWORLD 1\nNAME T\n"
            "PLACE Meshes/Car.ocmesh 0 0 0 0 0 0 1 name Car\n"
            "  COMPONENT camerarig preset=vehiclechase arm=700 zzz=5\n";
        fmt::OcWorldData lvl;
        std::string err;
        check(fmt::parseOcworld(text, lvl, &err), "a level with a CameraRig record parses: " + err);
        const bool one = lvl.placements.size() == 1 && lvl.placements[0].components.size() == 1;
        check(one, "one placement with one component");
        if (one) {
            const fmt::OcPlacedComponent& c = lvl.placements[0].components[0];
            check(c.type == "CameraRig", "the type is written in canonical case whatever case was read");
            check(c.find("preset") && *c.find("preset") == "vehiclechase" && c.find("zzz") && *c.find("zzz") == "5", "keys, including an unknown one, are kept");
        }
        const std::string out = fmt::writeOcworld(lvl);
        check(out.find("COMPONENT CameraRig preset=vehiclechase arm=700 zzz=5\n") != std::string::npos, "the writer keeps the record, unknown key and all");
        fmt::OcWorldData again;
        check(fmt::parseOcworld(out, again, &err) && fmt::writeOcworld(again) == out, "and a second write reproduces the first byte for byte");

        // Through the applier: what the file says is what the component holds.
        const scene::Entity e = mesh(w, "FromFile");
        if (one) {
            check(game::applyPlacedComponent(w, e, lvl.placements[0].components[0]), "the parsed record applies");
            const auto* c = w.component<CCameraRig>(e, type);
            check(c && c->armLengthCm == 700.0f && c->preset == static_cast<u32>(camera::CameraPreset::VehicleChase), "with its override");
            captured(w, e, {{"preset", "vehiclechase"}, {"arm", "700"}}, "and capture drops the key the file carried that no component field holds");
        }
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
