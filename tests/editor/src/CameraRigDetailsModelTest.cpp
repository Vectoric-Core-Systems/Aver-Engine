// The Camera Rig Details section without a window: the preset / mode / curve rows and what picking a preset writes, the
// canonical COMPONENT CameraRig record (keys at the preset's value are omitted) and its round trip with CCameraRig, the attach
// rules of the Add Component entry, the layer-mask checklist and the near-plane warning. Header-only model
// (sandbox/src/CameraRigDetailsModel.hpp) plus the Add Component table; the widgets themselves are ImGui and covered by the
// owner's headed run.
#include "CameraRigDetailsModel.hpp"
#include "PlacedComponentModel.hpp"

#include "aver/core/Log.hpp"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

using namespace aver;
using namespace aver::editor;
namespace cm = aver::editor::cammodel;
using aver::camera::CCameraRig;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string show(const fmt::OcPlacedComponent& r) {
    std::string s = r.type;
    for (const auto& kv : r.kv) s += " " + kv.first + "=" + kv.second;
    return s;
}

int main() {
    AVER_INFO("CameraRigDetailsModelTest");

    AVER_INFO("the rows");
    {
        check(cm::kPresetRows.size() == 7 && cm::kModeRows.size() == 6 && cm::kCurveRows.size() == 4, "seven presets, six modes, four curves");
        bool ids = true;
        for (usize i = 0; i < cm::kPresetRows.size(); ++i) ids = ids && static_cast<usize>(cm::kPresetRows[i].preset) == i;
        for (usize i = 0; i < cm::kModeRows.size(); ++i) ids = ids && static_cast<usize>(cm::kModeRows[i].mode) == i;
        for (usize i = 0; i < cm::kCurveRows.size(); ++i) ids = ids && static_cast<usize>(cm::kCurveRows[i].curve) == i;
        check(ids, "the rows are in enum order, so a combo index is the value");
        camera::CameraPreset p;
        camera::CameraMode m;
        camera::BlendCurve c;
        check(cm::presetFromKey("VehicleChase", p) && p == camera::CameraPreset::VehicleChase, "keys read case-insensitively: vehiclechase");
        check(cm::modeFromKey("topdown", m) && m == camera::CameraMode::TopDown && cm::curveFromKey("EASEOUT", c) && c == camera::BlendCurve::EaseOut, "...mode and curve too");
        check(!cm::presetFromKey("sideways", p) && !cm::modeFromKey("hover", m) && !cm::curveFromKey("wobbly", c), "unknown words are reported");
        check(std::string(cm::presetRow(camera::CameraPreset::Seat).key) == "seat" && std::string(cm::presetRow(camera::CameraPreset::FirstPerson).label) == "First person", "keys and labels");
    }

    AVER_INFO("Add Component: the kind, its creation default and the attach rules");
    {
        const PlacedKind* k = findPlacedKind("CameraRig");
        check(k && std::string(k->componentName) == "CCameraRig" && k->builtinId == 0 && placedAssetExts(*k).empty(), "CameraRig is a runtime-registered kind with no asset");
        if (k) {
            const fmt::OcPlacedComponent r = makeNewRecord(*k);
            check(r.type == "CameraRig" && r.kv.size() == 1 && r.find("preset") && *r.find("preset") == "chase", "a new record writes preset=chase");
            AttachContext ctx;
            ctx.levelOwned = true;
            std::string why;
            check(!canAttach(ctx, *k, &why) && why.find("Needs a mesh") != std::string::npos, "a rig needs a placed mesh");
            ctx.hasMesh = true;
            check(canAttach(ctx, *k), "a placed mesh may carry one (a car, a prop camera)");
            ctx.alreadyPresent = true;
            check(!canAttach(ctx, *k, &why) && why.find("already") != std::string::npos, "but only one");
            ctx.alreadyPresent = false;
            ctx.legacyOcmap = true;
            check(!canAttach(ctx, *k), "and not in a legacy .ocmap");
            ctx.legacyOcmap = false;
            ctx.hasCloth = true;
            check(canAttach(ctx, *k), "a cloth on the entity is no obstacle");
        }
    }

    AVER_INFO("the canonical record");
    {
        fmt::OcPlacedComponent r;
        r.type = "CameraRig";
        r.kv.emplace_back("preset", "vehiclechase");
        CCameraRig rig;
        cm::TargetKind target = cm::TargetKind::Self;
        const CCameraRig vehicle = camera::presetRig(camera::CameraPreset::VehicleChase);
        check(cm::rigFromRecord(r, rig, target) && std::memcmp(&rig, &vehicle, sizeof rig) == 0, "a bare preset record is exactly the preset's rig");
        check(show(cm::recordFromRig(rig)) == "CameraRig preset=vehiclechase", "and writes back as just the preset");

        rig.armLengthCm = 700.0f;
        rig.socketX = 1.0f; rig.socketY = 2.0f; rig.socketZ = 3.0f;
        rig.collisionMask = 0xFFu;
        rig.flags |= camera::kRigNoCollision;
        rig.blendCurve = static_cast<u32>(camera::BlendCurve::Linear);
        rig.mode = static_cast<u32>(camera::CameraMode::Orbit);
        check(show(cm::recordFromRig(rig, cm::TargetKind::Parent)) == "CameraRig preset=vehiclechase mode=orbit target=parent arm=700 socket=1,2,3 curve=linear mask=ff collision=0",
              "overrides are written in canonical order: " + show(cm::recordFromRig(rig, cm::TargetKind::Parent)));

        CCameraRig back;
        cm::TargetKind tk;
        check(cm::rigFromRecord(cm::recordFromRig(rig, cm::TargetKind::Parent), back, tk) && tk == cm::TargetKind::Parent &&
                  std::memcmp(&back, &rig, sizeof back) == 0,
              "rigFromRecord inverts recordFromRig");

        // The preset's own flags are not keys: a TopDown rig has collision=0 by nature.
        const CCameraRig td = camera::presetRig(camera::CameraPreset::TopDown);
        check(show(cm::recordFromRig(td)) == "CameraRig preset=topdown", "TopDown's built-in flags (no collision, no inherit) are not written");
        CCameraRig td2 = td;
        td2.flags &= ~camera::kRigNoCollision;
        check(show(cm::recordFromRig(td2)) == "CameraRig preset=topdown collision=1", "turning one back on is written as collision=1");

        // A non-preset base.
        CCameraRig chase = camera::presetRig(camera::CameraPreset::Chase);
        chase.flags |= camera::kRigLookAtTarget | camera::kRigDisabled | camera::kRigNoFade;
        check(show(cm::recordFromRig(chase)) == "CameraRig preset=chase fade=0 lookat=1 disabled=1", "positive flags are 1 when set, negative ones 0");
        chase.ignoreBody = 12;
        chase.outArmCm = 5.0f;
        chase.outState = 0xFF;
        chase.flags |= camera::kRigCut;
        check(show(cm::recordFromRig(chase)) == "CameraRig preset=chase fade=0 lookat=1 disabled=1", "ignoreBody, the OUT fields and the one-shot bits are not part of it");
    }

    AVER_INFO("picking a preset and canonicalising");
    {
        fmt::OcPlacedComponent r;
        r.type = "CameraRig";
        r.kv = {{"preset", "chase"}, {"arm", "999"}, {"target", "parent"}, {"mask", "ff"}, {"lag", "3"}};
        const fmt::OcPlacedComponent picked = cm::applyPreset(r, camera::CameraPreset::Orbit);
        check(show(picked) == "CameraRig preset=orbit target=parent mask=ff", "applying a preset replaces the tuning and keeps the target and the mask: " + show(picked));

        fmt::OcPlacedComponent loose;
        loose.type = "CameraRig";
        loose.kv = {{"lag", "8"}, {"preset", "vehiclechase"}, {"arm", "600"}, {"zzz", "5"}, {"collision", "1"}, {"fovkick", "12"}};
        check(show(cm::canonicalize(loose)) == "CameraRig preset=vehiclechase fovkick=12 zzz=5", "canonicalising drops keys at the preset's value, orders the rest and keeps unknown keys: " + show(cm::canonicalize(loose)));
        check(show(cm::canonicalize(cm::canonicalize(loose))) == show(cm::canonicalize(loose)), "and is idempotent");

        fmt::OcPlacedComponent bad;
        bad.type = "CameraRig";
        bad.kv = {{"preset", "sideways"}};
        std::string why;
        CCameraRig rig;
        cm::TargetKind tk;
        check(!cm::rigFromRecord(bad, rig, tk, &why) && why.find("sideways") != std::string::npos, "an unknown preset is reported by name");
        check(show(cm::canonicalize(bad)) == "CameraRig preset=sideways", "and the record is left as written");
        bad.kv = {{"preset", "seat"}, {"mode", "hover"}};
        check(!cm::rigFromRecord(bad, rig, tk, &why) && why.find("hover") != std::string::npos, "an unknown mode too");
        bad.kv = {{"preset", "seat"}, {"socket", "4,,9"}};
        check(cm::rigFromRecord(bad, rig, tk) && rig.socketX == 4.0f && rig.socketY == 0.0f && rig.socketZ == 9.0f, "an axis that does not parse keeps the preset's value");
    }

    AVER_INFO("the layer-mask checklist");
    {
        const u32 sys = ~((1u << 14) | (1u << 15));
        check(cm::effectiveMask(0, sys) == sys && cm::effectiveMask(0x5u, sys) == 0x5u, "a rig mask of 0 shows the system default");
        check(!cm::layerOn(sys, 14) && !cm::layerOn(sys, 15) && cm::layerOn(sys, 0) && cm::layerOn(sys, 31), "which leaves out the cloth and proxy layers");
        const u32 withCloth = cm::toggleLayer(sys, 14);
        check(cm::layerOn(withCloth, 14) && cm::toggleLayer(withCloth, 14) == sys, "ticking a layer writes an explicit mask, and unticking undoes it");
        check(cm::toggleLayer(sys, 40) == sys, "an out-of-range layer changes nothing");
    }

    AVER_INFO("the near-plane warning");
    {
        const float fov = radians(60.0f), aspect = 16.0f / 9.0f;
        const float need = camera::nearCornerRadiusCm(2.0f, fov, aspect);
        check(need > 2.2f && need < 2.4f, "about 2.3 cm at the defaults");
        CCameraRig r = camera::presetRig(camera::CameraPreset::Chase);
        check(!cm::probeTooSmall(r, 2.0f, fov, aspect), "the Chase preset's 12 cm probe is fine");
        r.probeRadiusCm = 1.0f;
        check(cm::probeTooSmall(r, 2.0f, fov, aspect), "a 1 cm probe is below the near-plane corner");
        check(!cm::probeTooSmall(r, 0.5f, fov, aspect), "...unless the near plane is close enough");
        r.flags |= camera::kRigNoCollision;
        check(!cm::probeTooSmall(r, 2.0f, fov, aspect), "a rig with no probe has nothing to warn about");
        CCameraRig fp = camera::presetRig(camera::CameraPreset::FirstPerson);
        fp.probeRadiusCm = 1.0f;
        check(!cm::probeTooSmall(fp, 2.0f, fov, aspect), "and neither does first person");
    }

    AVER_INFO("the Play readout");
    {
        CCameraRig r;
        r.outArmCm = 188.0f;
        r.outFovDeg = 60.0f;
        r.outState = camera::kRigStateColliding | camera::kRigStateFaded;
        check(cm::outLine(r) == "arm 188 cm   fov 60.0   colliding   faded", "arm, FOV and the state words: " + cm::outLine(r));
    }

    if (g_failures == 0) AVER_INFO("CameraRigDetailsModelTest: all checks passed");
    else AVER_ERROR("CameraRigDetailsModelTest: {} FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
