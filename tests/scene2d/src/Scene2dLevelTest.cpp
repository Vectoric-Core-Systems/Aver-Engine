// Scene2dLevelTest -- the .ocworld COMPONENT C2DTransform / CSprite2D / C2DLayer records: a golden file that parses and writes
// back byte for byte, the records applied to live entities (no mesh needed) and captured back to the same records, defaults
// omitted, refusals with their reasons, and the composed 2D result of a loaded menu. A real scene::World; no device.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelComponents.hpp"
#include "aver/scene/Scene2D.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
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

static bool near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

static std::string lf(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

static fmt::OcPlacedComponent rec(const char* type, std::initializer_list<std::pair<const char*, const char*>> kv) {
    fmt::OcPlacedComponent r;
    r.type = type;
    for (const auto& [k, v] : kv) r.kv.emplace_back(k, v);
    return r;
}

static u64 idOf(const char* p) { return fnv1a64(std::string_view(p)); }

int main() {
    scene::World& w = scene::World::instance();

    // Before registration the records are refused, naming why.
    {
        const scene::Entity e = w.create("early");
        check(!game::applyPlacedComponent(w, e, rec("C2DTransform", {})), "with the 2D components not registered a record is refused");
        w.destroy(e);
        w.flush();
    }
    scene2d::registerComponents(w);

    // ---- the golden file ----
    std::string golden;
    {
        std::ifstream f(std::string(AVER_SCENE2D_DATA) + "/menu2d.ocworld", std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        golden = lf(ss.str());
    }
    check(!golden.empty(), "the golden file is readable");

    fmt::OcWorldData level;
    std::string err;
    check(fmt::parseOcworld(golden, level, &err), "it parses: " + err);
    check(level.placements.size() == 5, "five placements: canvas, background, button, hidden, empty");
    if (level.placements.size() != 5) return exitCode(ExitCode::Failed);
    check(level.placements[0].components.size() == 1 && level.placements[0].components[0].type == "C2DLayer", "the canvas carries a C2DLayer record");
    check(level.placements[2].components.size() == 2 && level.placements[2].components[1].type == "CSprite2D" &&
              level.placements[2].components[1].kv.size() == 15,
          "the button carries a transform and a sprite with fifteen keys");
    if (fmt::writeOcworld(level) != golden) AVER_ERROR("   the writer produced: {}", fmt::writeOcworld(level));
    check(fmt::writeOcworld(level) == golden, "writing it back reproduces the golden file byte for byte");
    {
        fmt::OcWorldData again;
        check(fmt::parseOcworld(fmt::writeOcworld(level), again, &err) && fmt::writeOcworld(again) == golden && again.placements[2].components == level.placements[2].components,
              "and the round trip is stable, record for record");
    }
    {   // hand-typed case is fixed on write
        fmt::OcWorldData h;
        fmt::parseOcworld("OCWORLD 1\nNAME H\nPLACE a 0 0 0 0 0 0 1\nCOMPONENT c2dtransform X=1\nCOMPONENT CSPRITE2D\nCOMPONENT c2dLAYER\n", h, &err);
        const std::string t = fmt::writeOcworld(h);
        check(t.find("COMPONENT C2DTransform x=1") != std::string::npos && t.find("COMPONENT CSprite2D") != std::string::npos &&
                  t.find("COMPONENT C2DLayer") != std::string::npos,
              "hand-typed type names are written in canonical case and keys lower-cased");
    }

    // ---- apply: build the entities the way the level loader does (a placement per entity, parents by index) ----
    std::vector<scene::Entity> ents;
    std::map<u64, std::string> known;
    for (const char* p : {"UI/menu_bg.png", "UI/buttons.png"}) known[idOf(p)] = p;
    const game::PathOfIdFn pathOf = [&](u64 id) { const auto it = known.find(id); return it == known.end() ? std::string{} : it->second; };
    for (size_t i = 0; i < level.placements.size(); ++i) {
        const auto& p = level.placements[i];
        const scene::Entity parent = p.parent >= 0 ? ents[static_cast<size_t>(p.parent)] : scene::kInvalidEntity;
        const scene::Entity e = parent == scene::kInvalidEntity ? w.create(p.asset) : w.create(p.asset, parent, Transform{});
        ents.push_back(e);
        check(w.valid(e) && !w.hasComponent(e, scene::kComponentMeshRenderer), "placement " + std::to_string(i) + ": an entity with no mesh");
        check(game::applyPlacedComponents(w, e, p, static_cast<i64>(i)) == p.components.size(), "placement " + std::to_string(i) + ": every record applies");
    }

    const scene2d::Ids id = scene2d::ids();
    {
        const auto* lay = w.component<scene2d::C2DLayer>(ents[0], id.layer);
        check(lay && lay->index == 1 && lay->offsetX == 10 && lay->offsetY == 20 && lay->rotation == 0.25f && lay->scaleX == 2 && lay->scaleY == 1.5f && lay->flags == 0,
              "C2DLayer: index, offset, rotation, scale");
        const auto* bg = w.component<scene2d::C2DTransform>(ents[1], id.transform);
        check(bg && bg->posX == 640 && bg->posY == 360 && bg->zIndex == -5 && bg->scaleX == 1.5f && bg->scaleY == 0.0f && bg->flags == 0 && bg->treeOrder == 0,
              "C2DTransform: position, z, scale, and an unset sy stays zero (= 1)");
        const auto* bgs = w.component<scene2d::CSprite2D>(ents[1], id.sprite);
        check(bgs && bgs->texture == static_cast<i64>(idOf("UI/menu_bg.png")) && bgs->flags == 0 && bgs->modulate == 0 && bgs->hframes == 0 && bgs->blend == 0,
              "CSprite2D: the texture is fnv1a64 of the path and everything else is unset");
        const auto* bt = w.component<scene2d::C2DTransform>(ents[2], id.transform);
        check(bt && bt->rotation == -0.5f && bt->scaleY == 2 && bt->zIndex == 3 && bt->treeOrder == 12 && bt->flags == scene2d::kT2DZAbsolute, "the button transform");
        const auto* bs = w.component<scene2d::CSprite2D>(ents[2], id.sprite);
        check(bs && bs->regionX == 8 && bs->regionY == 16 && bs->regionW == 64 && bs->regionH == 32 && bs->offsetX == 4 && bs->offsetY == -2 &&
                  bs->hframes == 2 && bs->vframes == 3 && bs->frame == 4 && bs->blend == scene2d::kBlendAdd,
              "the button sprite: region, offset, frame grid, blend");
        check(bs && bs->flags == (scene2d::kSpriteFlipH | scene2d::kSpriteNotCentered | scene2d::kSpriteNearest), "...and the flags");
        check(bs && bs->modulate == 0xC04080FFu, "...modulate ff8040c0 (RRGGBBAA) is stored as 0xAABBGGRR");
        const auto* hs = w.component<scene2d::CSprite2D>(ents[3], id.sprite);
        check(hs && hs->texture == 0xdeadbeefLL && hs->flags == (scene2d::kSpriteFlipV | scene2d::kSpriteHidden) && hs->blend == scene2d::kBlendMul,
              "texture# names an id with no path; hidden and mul");
        const auto* ht = w.component<scene2d::C2DTransform>(ents[3], id.transform);
        check(ht && ht->flags == (scene2d::kT2DTopLevel | scene2d::kT2DHidden), "toplevel and hidden");
    }

    // ---- the loaded menu composes ----
    scene2d::updateTransforms(w);
    {
        // The background is a child of a plain canvas placement: the layer does not feed the 2D chain.
        i32 z = 0;
        const scene2d::Affine2 m = scene2d::worldAffine(w, ents[1], &z);
        check(near(m.a, 1.5f) && near(m.d, 1.0f) && near(m.tx, 640) && near(m.ty, 360) && z == -5, "the background composes at (640, 360), scale 1.5, z -5");
        const scene2d::Affine2 b = scene2d::worldAffine(w, ents[2], &z);
        check(near(b.ty, -60) && z == 3, "the button's absolute z is its own, its position is its own (the layer resets the chain)");
    }

    // ---- capture: the records come back as they were authored ----
    bool same = true;
    for (size_t i = 0; i < level.placements.size(); ++i) {
        const auto caps = game::capturePlacedComponents(w, ents[i], pathOf);
        std::vector<fmt::OcPlacedComponent> want;
        for (const auto& c : level.placements[i].components) want.push_back(c);
        // capture returns the canonical type order: transform, sprite, layer
        std::vector<fmt::OcPlacedComponent> ordered;
        for (const char* t : {"C2DTransform", "CSprite2D", "C2DLayer"})
            for (const auto& c : want)
                if (c.type == t) ordered.push_back(c);
        if (caps != ordered) {
            same = false;
            AVER_ERROR("   placement {} captured {} record(s), authored {}", i, caps.size(), ordered.size());
            for (const auto& c : caps) {
                std::string s = c.type;
                for (const auto& [k, v] : c.kv) s += " " + k + "=" + v;
                AVER_ERROR("      captured: {}", s);
            }
        }
    }
    check(same, "capturing every entity gives back exactly the authored records (defaults omitted, one spelling of a number)");

    // Through a whole level: capture into placements, write, compare with the golden file.
    {
        fmt::OcWorldData out = level;
        for (size_t i = 0; i < out.placements.size(); ++i) out.placements[i].components = game::capturePlacedComponents(w, ents[i], pathOf);
        check(fmt::writeOcworld(out) == golden, "a level saved from the live entities is the golden file again");
    }

    // ---- defaults, re-apply, remove ----
    {
        const auto empty = game::capturePlacedComponent(w, ents[4], "C2DTransform", pathOf);
        check(empty && empty->kv.empty(), "a bare C2DTransform captures as a bare record: the presence is the information");
        check(game::applyPlacedComponent(w, ents[2], rec("C2DTransform", {{"x", "5"}})), "re-applying a record replaces the fields");
        const auto* t = w.component<scene2d::C2DTransform>(ents[2], id.transform);
        check(t && t->posX == 5 && t->rotation == 0 && t->scaleY == 0 && t->zIndex == 0 && t->flags == 0 && t->treeOrder == 0,
              "...every field not named goes back to its default");
        check(game::removePlacedComponent(w, ents[2], "CSprite2D") && !w.hasComponent(ents[2], id.sprite), "removePlacedComponent takes a 2D component off");
        check(!game::capturePlacedComponent(w, ents[2], "CSprite2D", pathOf).has_value(), "and capture then reports it absent");
        check(game::applyPlacedComponent(w, ents[2], rec("CSprite2D", {{"scale", "9"}, {"junk", "1"}})), "unknown keys are ignored");
    }

    // ---- refusals ----
    {
        const scene::Entity e = w.create("refuse");
        const char* bad[][2] = {{"z", "5000"}, {"z", "-4097"}, {"x", "nan"}, {"order", "-1"}};
        for (const auto& b : bad)
            check(!game::applyPlacedComponent(w, e, rec("C2DTransform", {{b[0], b[1]}})), std::string("C2DTransform refuses ") + b[0] + "=" + b[1]);
        check(!w.hasComponent(e, id.transform), "...and a refused record attaches nothing");
        const char* badS[][2] = {{"modulate", "xyz"}, {"modulate", "ff"}, {"filter", "cubic"}, {"blend", "screen"}, {"rw", "-1"}, {"hframes", "-2"}, {"texture#", "zz"}, {"ox", "inf"}};
        for (const auto& b : badS)
            check(!game::applyPlacedComponent(w, e, rec("CSprite2D", {{b[0], b[1]}})), std::string("CSprite2D refuses ") + b[0] + "=" + b[1]);
        check(!w.hasComponent(e, id.sprite), "...and nothing is attached");
        check(!game::applyPlacedComponent(w, e, rec("C2DLayer", {{"rotation", "nan"}})), "C2DLayer refuses a non-finite rotation");
        check(game::applyPlacedComponent(w, e, rec("CSprite2D", {{"modulate", "FF8040"}})), "an RRGGBB modulate is accepted");
        check(w.component<scene2d::CSprite2D>(e, id.sprite)->modulate == 0xFF4080FFu, "...with alpha FF");
        check(game::applyPlacedComponent(w, e, rec("CSprite2D", {{"filter", "Linear"}, {"blend", "MIX"}})), "linear and mix are spelled out and case-insensitive");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
