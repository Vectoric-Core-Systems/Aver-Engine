// DecalFormatTest -- the .ocworld DECAL record (round trip, defaults left out, unknown tokens skipped,
// a level without decals unchanged) and the editor's CDecal <-> record conversion
// (sandbox/src/DecalLevelIo.hpp, header-only): every field survives file -> component -> file, pooled
// gameplay decals are never saved, and a loaded record becomes an entity with the right pose. No GPU.
#include "DecalLevelIo.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool approx(f64 a, f64 b, f64 tol = 1e-4) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

// The DECAL lines of a written level, joined.
static std::string decalLines(const std::string& text) {
    std::string out;
    usize pos = 0;
    while (pos < text.size()) {
        usize eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        if (text.compare(pos, 6, "DECAL ") == 0) out += text.substr(pos, eol - pos) + "\n";
        pos = eol + 1;
    }
    return out;
}

static bool hasToken(const std::string& line, const std::string& tok) {
    return line.find(" " + tok + " ") != std::string::npos || line.find(" " + tok + "\n") != std::string::npos ||
           (line.size() >= tok.size() + 1 && line.compare(line.size() - tok.size() - 1, tok.size() + 1, " " + tok) == 0);
}

int main() {
    AVER_INFO("=== DecalFormatTest ===");
    std::string err;

    AVER_INFO("=== .ocworld DECAL record ===");
    {
        fmt::OcWorldData w;
        w.name = "DecalRoundTrip";
        fmt::OcDecal a;
        a.name = "Bullet hole #1";
        a.x = 120.5; a.y = -40; a.z = 75; a.yaw = 30; a.pitch = -15; a.roll = 90;
        a.sx = 1; a.sy = 2; a.sz = 1.5;
        a.sizeCm[0] = 60; a.sizeCm[1] = 120; a.sizeCm[2] = 80;
        a.tint[0] = 0.5; a.tint[1] = 0.25; a.tint[2] = 1.0;
        a.opacity = 0.75; a.normalStrength = 0.5; a.roughness = 0.3; a.metallic = 0.1;
        a.edgeFade = 0.2; a.angleStartDeg = 45; a.angleEndDeg = 80; a.fadeDistanceCm = 3000; a.order = -3;
        a.base = "Decals/hole.png"; a.normal = "Decals/hole_n.png"; a.orm = "Decals/hole_orm.png";
        a.uvScale[0] = 2; a.uvScale[1] = 3; a.uvOffset[0] = 0.5; a.uvOffset[1] = 0.25;
        a.noColour = true; a.noNormal = true; a.noRoughness = true; a.disabled = true;
        fmt::OcDecal b;   // all defaults
        b.x = 1; b.y = 2; b.z = 3;
        fmt::OcLight lamp;
        lamp.x = 5;
        fmt::OcWorldPlacement place;
        place.asset = "Meshes/crate.ocmesh";
        place.x = 10;
        w.decals = {a, b};
        w.lights = {lamp};
        w.placements = {place};

        const std::string text = fmt::writeOcworld(w);
        fmt::OcWorldData back;
        check(fmt::parseOcworld(text, back, &err), "written level parses: " + err);
        check(back.decals.size() == 2, "both decals survive, in order");
        if (back.decals.size() == 2) {
            const fmt::OcDecal& x = back.decals[0];
            check(x.name == "Bullet hole #1", "the name (percent-encoded, with a space and a #)");
            check(x.x == 120.5 && x.y == -40 && x.z == 75 && x.yaw == 30 && x.pitch == -15 && x.roll == 90, "pose");
            check(x.sx == 1 && x.sy == 2 && x.sz == 1.5, "scale");
            check(x.sizeCm[0] == 60 && x.sizeCm[1] == 120 && x.sizeCm[2] == 80, "size");
            check(x.tint[0] == 0.5 && x.tint[1] == 0.25 && x.tint[2] == 1.0, "tint");
            check(x.opacity == 0.75 && x.normalStrength == 0.5 && x.roughness == 0.3 && x.metallic == 0.1, "opacity, normal strength, roughness, metallic");
            check(x.edgeFade == 0.2 && x.angleStartDeg == 45 && x.angleEndDeg == 80 && x.fadeDistanceCm == 3000 && x.order == -3,
                  "edge, angles, fade distance, sort order");
            check(x.base == "Decals/hole.png" && x.normal == "Decals/hole_n.png" && x.orm == "Decals/hole_orm.png", "the three images");
            check(x.uvScale[0] == 2 && x.uvScale[1] == 3 && x.uvOffset[0] == 0.5 && x.uvOffset[1] == 0.25, "uv scale and offset");
            check(x.noColour && x.noNormal && x.noRoughness && x.disabled, "channel opt-outs and disabled");
            const fmt::OcDecal& y = back.decals[1];
            check(y.x == 1 && y.y == 2 && y.z == 3 && y.sizeCm[0] == 100 && y.opacity == 1.0 && y.tint[0] == 1.0, "the default decal reads as defaults");
            check(y.base.empty() && !y.noColour && !y.disabled && y.edgeFade == 0.1 && y.angleStartDeg == 60 && y.angleEndDeg == 85,
                  "no images, nothing opted out, default fades");
        }
        check(back.lights.size() == 1 && back.placements.size() == 1 && back.placements[0].asset == "Meshes/crate.ocmesh",
              "lights and placements are untouched by the new record");
        check(decalLines(fmt::writeOcworld(back)) == decalLines(text), "writing it again gives the same DECAL lines");

        // Defaults are left out so a plain decal's line stays short and readable.
        const std::string lines = decalLines(text);
        const usize nl = lines.find('\n');
        const std::string first = lines.substr(0, nl), second = lines.substr(nl + 1);
        check(hasToken(first, "tint") && hasToken(first, "opacity") && hasToken(first, "base") && hasToken(first, "disabled"),
              "a decal with non-default values writes them");
        check(!hasToken(second, "tint") && !hasToken(second, "opacity") && !hasToken(second, "base") && !hasToken(second, "scale") &&
                  !hasToken(second, "roughness") && !hasToken(second, "disabled") && !hasToken(second, "order"),
              "a default decal writes none of them");

        fmt::OcWorldData none;
        check(fmt::parseOcworld("OCWORLD 1\nNAME Old\n", none, &err) && none.decals.empty(),
              "a level written before DECAL existed has no decals");
        check(decalLines(fmt::writeOcworld(none)).empty(), "and writes none back");
        fmt::OcWorldData junk;
        check(fmt::parseOcworld("OCWORLD 1\nDECAL pos 1 2 3 wibble 7 futurekey 1 2 size 10 20 30 nocolour\n", junk, &err) &&
                  junk.decals.size() == 1 && junk.decals[0].x == 1 && junk.decals[0].sizeCm[2] == 30 && junk.decals[0].noColour,
              "unknown tokens are skipped and the rest still reads");
        fmt::OcWorldData shortLine;
        check(fmt::parseOcworld("OCWORLD 1\nDECAL pos 1 2\n", shortLine, &err) && shortLine.decals.size() == 1 && shortLine.decals[0].x == 0,
              "a truncated pos is ignored rather than half-read");
    }

    AVER_INFO("=== CDecal <-> record ===");
    {
        scene::CDecal c = editor::makeNewDecal();
        c.sizeCm[0] = 60; c.sizeCm[1] = 120; c.sizeCm[2] = 80;
        c.tint[0] = 0.5f; c.tint[1] = 0.25f; c.tint[2] = 1.0f;
        c.transparency = 0.25f; c.normalStrength = 0.5f; c.roughness = 0.3f; c.metallic = 0.1f;
        c.edgeFade = 0.2f; c.angleFadeStartDeg = 45; c.angleFadeEndDeg = 80; c.fadeDistanceCm = 3000; c.sortOrder = 5;
        const std::string base = "Decals/hole.png", nrm = "Decals/hole_n.png", orm = "Decals/hole_orm.png";
        c.baseTexture = static_cast<i64>(editor::lightAssetId(base));
        c.normalTexture = static_cast<i64>(editor::lightAssetId(nrm));
        c.ormTexture = static_cast<i64>(editor::lightAssetId(orm));
        c.uvScale[0] = 2; c.uvScale[1] = 3; c.uvOffset[0] = 0.5f; c.uvOffset[1] = 0.25f;
        c.flags = scene::kDecalNoNormal | scene::kDecalDisabled;
        c.lifetimeSec = 9.0f;   // not carried: an authored decal is permanent
        c.age = 4.0f;

        editor::LightAssetPaths assets;
        assets.byId[static_cast<u64>(c.baseTexture)] = base;
        assets.byId[static_cast<u64>(c.normalTexture)] = nrm;
        assets.byId[static_cast<u64>(c.ormTexture)] = orm;

        Transform xf;
        xf.position = Vec3{100.5f, -20.0f, 240.0f};
        xf.rotation = editor::quatFromEulerDeg(Vec3{-20.0f, 10.0f, 30.0f});
        xf.scale = Vec3{1.0f, 2.0f, 1.0f};
        const fmt::OcDecal r = editor::recordFromDecal(c, xf, "Wall stain", assets);
        check(r.name == "Wall stain" && r.base == base && r.normal == nrm && r.orm == orm, "the record carries name and image paths");
        check(r.opacity == 0.75 && r.disabled && r.noNormal && !r.noColour, "opacity is 1 - transparency; flags map");

        // Through the file, not just the struct.
        fmt::OcWorldData w;
        w.decals = {r};
        fmt::OcWorldData back;
        check(fmt::parseOcworld(fmt::writeOcworld(w), back, &err) && back.decals.size() == 1, "the record survives the text form: " + err);
        const scene::CDecal c2 = editor::decalFromRecord(back.decals[0]);
        check(approx(c2.sizeCm[0], 60) && approx(c2.sizeCm[1], 120) && approx(c2.sizeCm[2], 80), "size");
        check(approx(c2.tint[0], 0.5) && approx(c2.tint[1], 0.25) && approx(c2.tint[2], 1.0), "tint");
        check(approx(c2.transparency, 0.25) && approx(c2.normalStrength, 0.5) && approx(c2.roughness, 0.3) && approx(c2.metallic, 0.1),
              "transparency, normal strength, roughness, metallic");
        check(approx(c2.edgeFade, 0.2) && approx(c2.angleFadeStartDeg, 45) && approx(c2.angleFadeEndDeg, 80) &&
                  approx(c2.fadeDistanceCm, 3000) && c2.sortOrder == 5, "edge, angles, fade distance, order");
        check(c2.baseTexture == c.baseTexture && c2.normalTexture == c.normalTexture && c2.ormTexture == c.ormTexture,
              "image paths resolve back to the same ObjectIds");
        check(approx(c2.uvScale[1], 3) && approx(c2.uvOffset[0], 0.5) && approx(c2.uvOffset[1], 0.25), "uv");
        check(c2.flags == (scene::kDecalNoNormal | scene::kDecalDisabled), "flags (and never pooled)");
        check(c2.lifetimeSec == 0.0f && c2.age == 0.0f && c2.serial == 0, "lifetime, age and serial are not carried");

        const Transform t2 = editor::transformFromDecalRecord(back.decals[0]);
        check(approx(t2.position.x, 100.5) && approx(t2.position.y, -20) && approx(t2.position.z, 240), "position");
        check(approx(t2.scale.y, 2) && approx(t2.scale.x, 1), "scale");
        const f32 d = std::fabs(t2.rotation.x * xf.rotation.x + t2.rotation.y * xf.rotation.y + t2.rotation.z * xf.rotation.z +
                                t2.rotation.w * xf.rotation.w);
        check(d > 0.99999f, "rotation");

        // A zero-filled component (every default unset) saves as the defaults it reads as.
        scene::CDecal zero{};
        const fmt::OcDecal rz = editor::recordFromDecal(zero, Transform{}, "", assets);
        check(rz.sizeCm[0] == 100 && rz.tint[0] == 1.0 && rz.opacity == 1.0 && rz.normalStrength == 1.0 && approx(rz.edgeFade, 0.1) &&
                  rz.angleStartDeg == 60 && rz.angleEndDeg == 85 && rz.uvScale[0] == 1.0,
              "an unset component writes the defaults it reads as");
        check(rz.base.empty() && rz.roughness == 0.0 && rz.metallic == 0.0, "and no images, roughness or metallic");
    }

    AVER_INFO("=== load and save through the world ===");
    {
        scene::World& world = scene::World::instance();
        fmt::OcDecal a;
        a.name = "North wall";
        a.x = 10; a.y = 20; a.z = 30; a.yaw = 90;
        a.sizeCm[0] = 40; a.sizeCm[1] = 50; a.sizeCm[2] = 60;
        a.order = 2;
        fmt::OcDecal b;
        b.x = -5;
        const std::vector<scene::Entity> made = editor::spawnDecalsFromRecords(world, {a, b});
        check(made.size() == 2 && world.valid(made[0]) && world.valid(made[1]), "two records make two entities");
        check(std::string(world.name(made[0])) == "North wall" && std::string(world.name(made[1])) == "Decal", "named, and the unnamed one gets a default name");
        const auto* c = world.component<scene::CDecal>(made[0], scene::kComponentDecal);
        check(c && c->sizeCm[1] == 50 && c->sortOrder == 2, "the component carries the record");
        const Vec3 fwd = world.localTransform(made[0]).rotation.rotate({1, 0, 0});
        check(approx(fwd.y, 1.0) && approx(fwd.x, 0.0), "yaw 90 turns the projection toward +Y");

        // A pooled decal and a generic one: only the authored ones are saved.
        scene::CDecal pooled{};
        pooled.flags = scene::kDecalPooled;
        const scene::Entity ep = world.create("pooled");
        *static_cast<scene::CDecal*>(world.addComponent(ep, scene::kComponentDecal)) = pooled;

        const editor::LightAssetPaths none;
        const std::vector<fmt::OcDecal> saved = editor::recordsFromDecalEntities(world, none);
        check(saved.size() == 2, "the pooled decal is not saved");
        if (saved.size() == 2) {
            const fmt::OcDecal* north = saved[0].name == "North wall" ? &saved[0] : &saved[1];
            check(north->name == "North wall" && approx(north->x, 10) && approx(north->yaw, 90) && north->order == 2 &&
                      approx(north->sizeCm[2], 60), "the saved record matches what was loaded");
        }
        for (const scene::Entity e : {made[0], made[1], ep}) world.destroy(e);
        world.flush();
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
