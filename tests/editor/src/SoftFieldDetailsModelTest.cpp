// The Soft Field Details section and the soft-body depth subsections without a window: the kind and shape rows and their words,
// the canonical COMPONENT SoftField record (keys at their default are omitted), the attach rule, the helpers (match the
// primitive, make the volume invisible), the Tuning HUD text for a fixed hit, the Caps / Effects / Proxy helpers of the
// soft-body model, and the proxy tool's cooking. Header-only models plus SoftProxyTool.cpp compiled directly with AVER_WITH_IMGUI
// undefined; the widgets themselves are ImGui and covered by the owner's headed run.
#include "PlacedComponentModel.hpp"
#include "SoftBodyDetailsModel.hpp"
#include "SoftFieldDetailsModel.hpp"
#include "SoftProxyTool.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::editor;
namespace fm = aver::editor::fieldmodel;
namespace sm = aver::editor::softmodel;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static fmt::OcPlacedComponent record(std::initializer_list<std::pair<const char*, const char*>> kv) {
    fmt::OcPlacedComponent r;
    r.type = "SoftField";
    for (const auto& [k, v] : kv) r.kv.emplace_back(k, v);
    return r;
}

// A flat n x n quad sheet in the XY plane at z = 0.
static fmt::OcMeshData sheet(u32 n) {
    fmt::OcMeshData m;
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x) {
            m.positions.insert(m.positions.end(), {static_cast<f32>(x), static_cast<f32>(y), 0.0f});
            m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
            m.uvs.insert(m.uvs.end(), {static_cast<f32>(x) / static_cast<f32>(n), static_cast<f32>(y) / static_cast<f32>(n)});
        }
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const u32 a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            for (const u32 i : {a, b, d, a, d, c}) m.indices.push_back(i);
        }
    m.computeBounds();
    return m;
}

int main() {
    AVER_INFO("SoftFieldDetailsModelTest");

    AVER_INFO("the rows and their words");
    {
        check(fm::kKindRows.size() == 6 && fm::kShapeRows.size() == 4, "six kinds and four shapes");
        bool words = true;
        for (const fm::KindRow& r : fm::kKindRows) {
            softbody::FieldKind back;
            words = words && std::string(softbody::fieldKindKey(r.kind)) == r.key && softbody::fieldKindFromKey(r.key, back) && back == r.kind;
        }
        for (const fm::ShapeRow& r : fm::kShapeRows) {
            softbody::FieldShape back;
            words = words && std::string(softbody::fieldShapeKey(r.shape)) == r.key && softbody::fieldShapeFromKey(r.key, back) && back == r.shape;
        }
        check(words, "every row's key is the word the loader reads: radial directional damage release kill sleep / sphere box capsule all");
        softbody::FieldKind k;
        softbody::FieldShape s;
        check(!softbody::fieldKindFromKey("explode", k) && !softbody::fieldShapeFromKey("cone", s), "an unknown word is refused");
        check(std::string(fm::extentLabel(softbody::FieldShape::Capsule)).find("half height") != std::string::npos, "the capsule's extent label names the half height");
        u32 mask = 0;
        check(softbody::fieldTargetsFromText("cloth,debris", mask) && mask == 5, "targets: cloth,debris is 5");
        check(softbody::fieldTargetsFromText("cloth, destructible ,debris", mask) && mask == 7, "spaces around words are fine");
        check(!softbody::fieldTargetsFromText("cloth,glass", mask) && !softbody::fieldTargetsFromText("", mask), "an unknown word or an empty list is refused");
        check(softbody::fieldTargetsToText(7) == "cloth,destructible,debris" && softbody::fieldTargetsToText(4) == "debris", "and the text goes back");
        check(softbody::fieldDefaultTargets(softbody::FieldKind::Kill) == softbody::kFieldTargetDebris && softbody::fieldDefaultTargets(softbody::FieldKind::Damage) == softbody::kFieldTargetAll,
              "Kill defaults to debris, everything else to all");
    }

    AVER_INFO("the canonical record");
    {
        const fmt::OcPlacedComponent fresh = fm::makeNewRecord();
        check(fresh.type == "SoftField" && fresh.kv.empty(), "a new field is a radial sphere of 100 cm: no keys at all");
        const fmt::OcPlacedComponent verbose = record({{"kind", "radial"}, {"shape", "sphere"}, {"extent", "100,100,100"}, {"strength", "0"}, {"energy", "0"}, {"falloff", "1"},
                                                       {"turbulence", "0"}, {"targets", "cloth,destructible,debris"}, {"once", "0"}, {"off", "0"}});
        check(fm::canonicalize(verbose).kv.empty(), "a record spelling out every default canonicalises to nothing");
        check(fm::canonicalize(fm::canonicalize(verbose)) == fm::canonicalize(verbose), "and canonicalising is idempotent");

        fm::FieldValues v;
        v.kind = softbody::FieldKind::DirectionalForce;
        v.shape = softbody::FieldShape::Box;
        v.extent[0] = 300.0f; v.extent[1] = 200.0f; v.extent[2] = 100.0f;
        v.strength = 600.0f;
        v.falloff = 2.0f;
        v.dir[0] = 1.0f; v.dir[1] = 0.0f; v.dir[2] = 0.5f;
        v.turbulence = 0.25f;
        v.targets = softbody::kFieldTargetCloth | softbody::kFieldTargetDebris;
        v.once = true;
        const fmt::OcPlacedComponent rec = fm::write(v);
        check(*rec.find("kind") == "directional" && *rec.find("shape") == "box" && *rec.find("extent") == "300,200,100" && *rec.find("strength") == "600" &&
                  *rec.find("falloff") == "2" && *rec.find("dir") == "1,0,0.5" && *rec.find("turbulence") == "0.25" && *rec.find("targets") == "cloth,debris" && *rec.find("once") == "1",
              "a full field writes every non-default key in the canonical spelling");
        check(rec.find("energy") == nullptr && rec.find("off") == nullptr, "and none of the defaults");
        check(fm::read(rec) == v, "reading it back gives the same values");
        check(fm::write(fm::read(rec)) == rec, "and writing those gives the same record");

        fm::FieldValues kill;
        kill.kind = softbody::FieldKind::Kill;
        check(fm::write(kill).kv.size() == 1 && fm::write(kill).find("targets") == nullptr, "a Kill field with default targets (debris only) writes no targets key");
        kill.targets = softbody::kFieldTargetDebris | softbody::kFieldTargetDestructible;
        check(*fm::write(kill).find("targets") == "destructible,debris", "adding the destructible bit writes it");
        check(fm::read(record({{"kind", "kill"}, {"targets", "debris"}})).targets == 0, "reading targets=debris on a Kill field means the default");
        check(fm::read(record({{"kind", "kill"}})).effectiveTargets() == softbody::kFieldTargetDebris, "and a Kill field without the key targets debris only");
        check(fm::read(record({{"kind", "damage"}})).effectiveTargets() == softbody::kFieldTargetAll, "a damage field targets everything");
    }

    AVER_INFO("extents, direction and bad values");
    {
        fm::FieldValues v = fm::read(record({{"extent", "300,200,150"}, {"dir", "0,1,0"}}));
        check(v.extent[0] == 300.0f && v.extent[1] == 200.0f && v.extent[2] == 150.0f && v.dir[1] == 1.0f, "extent and dir parse as three numbers");
        v = fm::read(record({{"extent", "300,200"}, {"dir", "a,b,c"}, {"strength", "x"}, {"kind", "nonsense"}}));
        check(v.extent[0] == 100.0f && v.dir[1] == 0.0f && v.strength == 0.0f && v.kind == softbody::FieldKind::RadialForce, "malformed values fall back to the defaults");
        v = fm::read(record({{"extent", "1,2,3,4"}}));
        check(v.extent[0] == 100.0f, "four numbers are malformed too");
    }

    AVER_INFO("the attach rule");
    {
        if (const PlacedKind* k = findPlacedKind("SoftField")) {
            AttachContext ctx;
            ctx.levelOwned = true;
            ctx.hasMesh = true;
            check(canAttach(ctx, *k), "a placed mesh (the primitive that holds the volume) can carry a Soft Field");
            ctx.hasMesh = false;
            std::string why;
            check(!canAttach(ctx, *k, &why) && why.find("mesh") != std::string::npos, "a placement with no mesh cannot: " + why);
            ctx.hasMesh = true;
            ctx.alreadyPresent = true;
            check(!canAttach(ctx, *k), "and not twice");
            check(std::string(k->componentName) == "CSoftField", "the registered name is CSoftField");
        } else {
            AVER_INFO("  note  kPlacedKinds has no SoftField row yet (PlacedComponentModel.hpp is a shared file); the attach cases are skipped");
        }
    }

    AVER_INFO("the helpers");
    {
        fm::FieldValues v;
        v.shape = softbody::FieldShape::Box;
        v.extent[0] = 300.0f; v.extent[1] = 20.0f; v.extent[2] = 5.0f;
        fm::matchPrimitive(v);
        check(v.extent[0] == 50.0f && v.extent[1] == 50.0f && v.extent[2] == 50.0f, "match primitive: a box becomes the 100 cm cube's half extents");
        v.shape = softbody::FieldShape::Sphere;
        v.extent[0] = 400.0f;
        fm::matchPrimitive(v);
        check(v.extent[0] == 50.0f && v.extent[1] == 50.0f, "a sphere takes radius 50 and leaves the other two");
        v.shape = softbody::FieldShape::Everywhere;
        v.extent[0] = 7.0f;
        fm::matchPrimitive(v);
        check(v.extent[0] == 7.0f, "everywhere is left alone");

        fm::VolumeFlags f;
        check(!fm::isInvisibleVolume(f), "a fresh primitive is visible and solid");
        check(fm::makeInvisibleVolume(f) && !f.visible && !f.collide && fm::isInvisibleVolume(f), "make invisible turns off visibility and collision");
        check(!fm::makeInvisibleVolume(f), "and a second press changes nothing");
        f = fm::VolumeFlags{true, false};
        check(fm::makeInvisibleVolume(f) && !f.visible, "a primitive that was already non-colliding still gets hidden");

        check(fm::outReadout(3, true, false, false) == "Reaching 3 pieces" && fm::outReadout(1, true, false, false) == "Reaching 1 piece" &&
                  fm::outReadout(0, false, false, true) == "Disabled" && fm::outReadout(0, false, true, true) == "Fired once" && fm::outReadout(0, false, false, false) == "Idle",
              "the OUT readout");
        f32 r, g, b;
        fm::kindColour(softbody::FieldKind::Damage, r, g, b);
        check(r > 0.9f && g < 0.3f, "damage is red");
        fm::kindColour(softbody::FieldKind::DirectionalForce, r, g, b);
        check(b > r && b > 0.9f, "directional is blue");
    }

    AVER_INFO("the Tuning HUD text");
    {
        fm::TuningHit h;
        check(fm::tuningHitText(h) == "last hit: none yet", "no hit yet");
        h.valid = true; h.kind = 0; h.tick = 12; h.closingCmS = 350.0f; h.energyJ = 90.0f; h.radiusCm = 30.0f;
        h.beamsDamaged = 14; h.broken = 3; h.maxStrain = 0.0123f; h.maxSetCm = 4.5f; h.pieces = 5; h.baked = 2;
        check(fm::tuningHitText(h) == "last hit (impact, tick 12): 350.0 cm/s closing, 90.0 J in 30 cm, 14 beams damaged, 3 broken; max strain 0.0123, max set 4.50 cm; 5 pieces (2 baked)",
              "a fixed hit reads in the units the tuning table talks in");
        h.kind = 2;
        check(fm::tuningHitText(h).find("gameplay") != std::string::npos, "a gameplay deposit says so");
        fm::TuningFrame f;
        f.pieces = 9; f.activeParticles = 1200; f.rebuilds = 1; f.drawBuilds = 4; f.fxSent = 6; f.fxDropped = 2;
        check(fm::tuningFrameText(f) == "frame: pieces 9, active particles 1200, rebuilds 1, draw builds 4, fx sent 6, fx dropped 2", "and the frame's counters");
    }

    AVER_INFO("the Caps, Effects and Proxy helpers of the soft-body model");
    {
        softbody::SoftAsset glass = softbody::makeSoftAsset(softbody::SoftKind::Destructible, softbody::SoftPreset::Glass);
        check(sm::capThicknessText(glass) == "0.60 cm (the glass preset)", "an unset thickness shows the preset's");
        glass.caps.thicknessCm = 1.25f;
        check(sm::capThicknessText(glass) == "1.25 cm", "and an explicit one its own");
        check(std::string(sm::capModeLabel(softbody::CapMode::Off)) == "Off" && sm::backSkinRow(softbody::BackSkin::All).mode == softbody::BackSkin::All, "the cap rows");

        softbody::FxSlotParams slot;
        bool all = true;
        for (int i = 0; i < 8; ++i) all = all && sm::addSound(slot, "Audio/S" + std::to_string(i) + ".wav");
        check(all && slot.sounds.size() == 8, "eight sounds fit in a slot");
        check(!sm::addSound(slot, "Audio/Nine.wav"), "a ninth is refused");
        check(!sm::addSound(slot, "Audio/S3.wav") && !sm::addSound(slot, ""), "so are a duplicate and an empty path");
        check(sm::removeSound(slot, 2) && slot.sounds.size() == 7 && slot.sounds[2] == "Audio/S3.wav", "removing shifts the rest down");
        check(!sm::removeSound(slot, 99), "and a bad index is refused");
        check(sm::kFxSlotRows.size() == softbody::kFxSlotCount && sm::kFxSlotRows[3].slot == softbody::FxSlot::Dust, "five slots in the order impact tear detach dust release");

        check(sm::proxyPathForMesh("Meshes/Door.ocmesh") == "Meshes/Door.proxy.ocmesh" && sm::proxyPathForMesh("").empty(), "the proxy sits beside its mesh");
        check(sm::proxyAdvice(false, false, "", true).empty(), "nothing to say about an ordinary mesh");
        check(sm::proxyAdvice(true, false, "", true).find("over the particle cap") != std::string::npos, "an over-cap mesh is told so");
        check(sm::proxyAdvice(true, false, "", false).find("Trifactor") != std::string::npos, "and, without Trifactor, why it cannot cook one");
        check(sm::proxyAdvice(false, true, "Meshes/D.proxy.ocmesh", true).find("Simulating the proxy cage") != std::string::npos, "a running proxy says so");
        check(sm::proxyAdvice(false, false, "Meshes/D.proxy.ocmesh", true).find("not in use") != std::string::npos, "a named proxy that is not running is flagged");
        check(sm::stateText(true, false, true, false, false, true) == "awake, released, on a proxy cage" && sm::stateText(false, true, false, true, true, false) == "frozen, asset not found, over the particle cap",
              "the state readout");
    }

    AVER_INFO("the proxy tool");
    {
        ProxyCookRequest req;
        req.targetParticles = 256;
        fmt::OcMeshData out;
        ProxyCookResult info;
        const fmt::OcMeshData dense = sheet(60);   // 3721 vertices
        const bool ok = cookProxyMesh(dense, req, out, info);
        if (proxyToolAvailable()) {
            check(ok && info.ok, "a dense sheet cooks (" + info.why + ")");
            check(info.weldedVertices == 3721 && info.renderTriangles == 7200, "it welded the 61 x 61 vertices and counted the triangles");
            check(info.proxyVertices > 0 && info.proxyVertices < 1200 && info.proxyTriangles < info.weldedTriangles, "and the proxy is far smaller");
            bool onSurface = true;
            for (usize i = 2; i < out.positions.size(); i += 3) onSurface = onSurface && std::fabs(out.positions[i]) < 1e-3f;
            check(onSurface, "its particles lie on the authored surface (collapses land on existing vertices)");
            check(out.valid(), "and it is a valid mesh");
        } else {
            check(!ok && info.why.find("Trifactor") != std::string::npos, "without Trifactor the tool says why it cannot cook: " + info.why);
        }
        const fmt::OcMeshData small = sheet(10);
        check(!cookProxyMesh(small, req, out, info) && info.why.find("needs no proxy") != std::string::npos, "a mesh already under the target needs no proxy: " + info.why);
        fmt::OcMeshData empty;
        check(!cookProxyMesh(empty, req, out, info), "an empty mesh is refused");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
