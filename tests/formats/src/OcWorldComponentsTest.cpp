// OcWorldComponentsTest -- the .ocworld COMPONENT record (docs/plans/parity_demo/ADD_COMPONENT.md section 3).
//
// The format never interprets a component, so everything here is about text: that a record attaches to the
// right placement, that values with awkward characters survive the one-token-per-value tokenizer, that the
// writer's renumbering of placements (a child written before its parent) cannot detach a component, and that a
// level with no COMPONENT line is written exactly as it was before the record existed.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"

#include <initializer_list>
#include <string>
#include <utility>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

static bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Index of the placement with this asset, or -1.
static int indexOf(const fmt::OcWorldData& w, const std::string& asset) {
    for (usize i = 0; i < w.placements.size(); ++i)
        if (w.placements[i].asset == asset) return static_cast<int>(i);
    return -1;
}

static fmt::OcPlacedComponent comp(const std::string& type,
                                   std::initializer_list<std::pair<std::string, std::string>> kv) {
    fmt::OcPlacedComponent c;
    c.type = type;
    c.kv.assign(kv.begin(), kv.end());
    return c;
}

int main() {
    AVER_INFO("=== OcWorldComponentsTest ===");
    std::string err;

    AVER_INFO("=== 1. two COMPONENT lines attach to the placement above them ===");
    const std::string kChar =
        "PLACE Meshes/KenneyChar.ocmesh 0 0 0 0 0 0 1 nocollide name KenneyChar\n"
        "  COMPONENT SkeletalMesh skeleton=Meshes/KenneyChar.ocskel\n"
        "  COMPONENT Animator clip=Meshes/Idle.ocanim speed=1.5 once=1\n";
    {
        fmt::OcWorldData w;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\n" + kChar, w, &err), "the level parses: " + err);
        check(w.placements.size() == 1 && w.placements[0].components.size() == 2, "one placement with two components");
        if (w.placements.size() == 1 && w.placements[0].components.size() == 2) {
            const fmt::OcPlacedComponent& a = w.placements[0].components[0];
            const fmt::OcPlacedComponent& b = w.placements[0].components[1];
            check(a.type == "SkeletalMesh" && a.kv.size() == 1, "first is SkeletalMesh with one key");
            check(a.kv.size() == 1 && a.kv[0].first == "skeleton" && a.kv[0].second == "Meshes/KenneyChar.ocskel",
                  "its key and decoded value");
            check(b.type == "Animator" && b.kv.size() == 3, "second is Animator with three keys");
            check(b.kv.size() == 3 && b.kv[0].first == "clip" && b.kv[1].first == "speed" && b.kv[2].first == "once",
                  "keys keep file order");
            const std::string* sp = b.find("speed");
            check(sp && *sp == "1.5", "find() returns the value");
            check(b.find("weight") == nullptr, "find() is null for an absent key");
        }
    }

    AVER_INFO("=== 2. golden write and round trip ===");
    {
        fmt::OcWorldData w;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\n" + kChar, w, &err), "parses");
        const std::string text = fmt::writeOcworld(w);
        check(endsWith(text, kChar), "the placement and its COMPONENT lines are written exactly as authored");
        fmt::OcWorldData b;
        check(fmt::parseOcworld(text, b, &err), "what the writer produced parses");
        check(b.placements.size() == 1 && b.placements[0].components == w.placements[0].components,
              "and the components compare equal");
        check(fmt::writeOcworld(b) == text, "a second write reproduces the first byte for byte");
    }

    AVER_INFO("=== 3. a level with no COMPONENT line is written as before ===");
    {
        // The nested fixture of FormatTest (checkOcworldNesting), copied: that test pins its parse, this pins its text.
        const char* kNested =
            "OCWORLD 1\nNAME Nest\n"
            "PLACE  Meshes/table.ocmesh  0 0 0  0 0 0  1  M_Wood\n"
            "BEGIN\n"
            "  CHILD  Meshes/lamp.ocmesh  10 0 80  0 0 0  1  M_Brass\n"
            "  BEGIN\n"
            "    CHILD  Meshes/bulb.ocmesh  0 0 12  0 0 0  1  M_Glass\n"
            "  END\n"
            "  CHILD  Meshes/book.ocmesh  -5 0 80  0 0 0  1  M_Paper\n"
            "END\n";
        const std::string expected =
            "PLACE Meshes/table.ocmesh 0 0 0 0 0 0 1 M_Wood\n"
            "BEGIN\n"
            "  CHILD Meshes/lamp.ocmesh 10 0 80 0 0 0 1 M_Brass\n"
            "  BEGIN\n"
            "    CHILD Meshes/bulb.ocmesh 0 0 12 0 0 0 1 M_Glass\n"
            "  END\n"
            "  CHILD Meshes/book.ocmesh -5 0 80 0 0 0 1 M_Paper\n"
            "END\n";
        fmt::OcWorldData w;
        check(fmt::parseOcworld(kNested, w, &err), "the nested fixture parses: " + err);
        bool none = w.placements.size() == 4;
        for (const fmt::OcWorldPlacement& p : w.placements) none = none && p.components.empty();
        check(none, "no placement carries a component");
        const std::string text = fmt::writeOcworld(w);
        check(!contains(text, "COMPONENT"), "the writer emits no COMPONENT token");
        check(endsWith(text, expected), "and the placement block is the pre-change text");
    }

    AVER_INFO("=== 4. a COMPONENT attaches to the most recent placement, also after an END ===");
    {
        fmt::OcWorldData w;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\n"
                                "PLACE a.ocmesh 0 0 0 0 0 0 1\n"
                                "BEGIN\n"
                                "  CHILD b.ocmesh 0 0 5 0 0 0 1\n"
                                "    COMPONENT Animator clip=c.ocanim\n"
                                "END\n"
                                "  COMPONENT RootMotion mode=apply\n", w, &err), "parses: " + err);
        check(w.placements.size() == 2, "two placements");
        if (w.placements.size() == 2) {
            check(w.placements[0].components.empty(), "the parent has none");
            check(w.placements[1].components.size() == 2, "the child has both, the second one read after the END");
            check(w.placements[1].components.size() == 2 && w.placements[1].components[0].type == "Animator" &&
                      w.placements[1].components[1].type == "RootMotion",
                  "in file order");
            const std::string text = fmt::writeOcworld(w);
            check(contains(text, "\n    COMPONENT Animator clip=c.ocanim\n"),
                  "the writer indents a child's COMPONENT one level below the CHILD line");
            fmt::OcWorldData b;
            check(fmt::parseOcworld(text, b, &err) && b.placements.size() == 2 &&
                      b.placements[1].components == w.placements[1].components,
                  "and it reads back onto the same child");
        }
    }

    AVER_INFO("=== 5. no placement yet, or no type ===");
    {
        fmt::OcWorldData w;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\n"
                                "COMPONENT SkeletalMesh skeleton=x.ocskel\n"
                                "PLACE a.ocmesh 0 0 0 0 0 0 1\n"
                                "COMPONENT\n", w, &err), "parses: " + err);
        check(w.placements.size() == 1 && w.placements[0].components.empty(),
              "a COMPONENT before any placement and a bare COMPONENT attach nothing");
    }

    AVER_INFO("=== 6. awkward values round trip ===");
    {
        fmt::OcWorldData w;
        w.name = "T";
        fmt::OcWorldPlacement p;
        p.asset = "a.ocmesh";
        p.components.push_back(comp("Odd", {{"v1", "a b"}, {"v2", "50%"}, {"v3", "x#y"}, {"v4", "p;q"},
                                            {"v5", "a=b=c"}, {"v6", "\"q\""}, {"v7", ""}, {"v8", "tab\there"}}));
        w.placements.push_back(p);
        const std::string text = fmt::writeOcworld(w);
        check(contains(text, "v1=a%20b") && contains(text, "v3=x%23y") && contains(text, "v5=a=b=c"),
              "spaces and '#' are escaped, a further '=' is left alone");
        fmt::OcWorldData b;
        check(fmt::parseOcworld(text, b, &err), "parses: " + err);
        check(b.placements.size() == 1 && b.placements[0].components.size() == 1 &&
                  b.placements[0].components[0] == w.placements[0].components[0],
              "space, %, #, ;, =, quote, empty and tab values all survive");

        fmt::OcWorldData bare;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\nPLACE a.ocmesh 0 0 0 0 0 0 1\nCOMPONENT Foo flag other=2\n", bare, &err),
              "a bare token parses");
        const std::string* flag = bare.placements.empty() || bare.placements[0].components.empty()
                                      ? nullptr : bare.placements[0].components[0].find("flag");
        check(flag && *flag == "1", "and reads as key=1");
    }

    AVER_INFO("=== 7. a child written before its parent keeps its own components ===");
    {
        fmt::OcWorldData w;
        w.name = "T";
        fmt::OcWorldPlacement child;
        child.asset = "child.ocmesh";
        child.parent = 1;
        child.components.push_back(comp("Animator", {{"clip", "child.ocanim"}}));
        fmt::OcWorldPlacement root;
        root.asset = "root.ocmesh";
        root.components.push_back(comp("SkeletalMesh", {{"skeleton", "root.ocskel"}}));
        root.components.push_back(comp("Brain", {{"asset", "AI/x.ocbt"}, {"team", "guards"}}));
        w.placements.push_back(child);   // index 0, its parent is index 1: the writer renumbers
        w.placements.push_back(root);
        fmt::OcSequence seq;
        seq.name = "Seq";
        fmt::OcSeqTrack tr;
        tr.kind = fmt::OcSeqTrackKind::Transform;
        tr.target = 0;                   // the child
        tr.keys.emplace_back();
        seq.tracks.push_back(tr);
        w.sequences.push_back(seq);

        const std::string text = fmt::writeOcworld(w);
        fmt::OcWorldData b;
        check(fmt::parseOcworld(text, b, &err), "parses: " + err);
        const int ic = indexOf(b, "child.ocmesh"), ir = indexOf(b, "root.ocmesh");
        check(ic >= 0 && ir >= 0 && b.placements.size() == 2, "both placements come back");
        if (ic >= 0 && ir >= 0) {
            check(b.placements[static_cast<usize>(ic)].parent == ir, "the child still hangs from the root");
            check(b.placements[static_cast<usize>(ic)].components == w.placements[0].components,
                  "the child keeps its own component");
            check(b.placements[static_cast<usize>(ir)].components == w.placements[1].components,
                  "the root keeps its two, in order");
            check(b.sequences.size() == 1 && b.sequences[0].tracks.size() == 1 &&
                      b.sequences[0].tracks[0].target == ic,
                  "a SEQTRACK target still names the child");
        }
    }

    AVER_INFO("=== 8. unknown types and keys, case, the id-only asset key ===");
    {
        fmt::OcWorldData w;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\n"
                                "PLACE a.ocmesh 0 0 0 0 0 0 1\n"
                                "  COMPONENT Wibble foo=1 zed=%20q\n"
                                "  component SKELETALMESH skeleton=a.ocskel\n"
                                "  COMPONENT animator skeleton%23=00ff00ff00ff00ff\n", w, &err), "parses: " + err);
        check(w.placements.size() == 1 && w.placements[0].components.size() == 3, "three components");
        if (w.placements.size() == 1 && w.placements[0].components.size() == 3) {
            const auto& cs = w.placements[0].components;
            check(cs[0].type == "Wibble" && cs[0].kv.size() == 2 && *cs[0].find("zed") == " q",
                  "an unknown type keeps its spelling, keys and decoded values");
            check(cs[1].type == "SkeletalMesh" && cs[2].type == "Animator", "known types read in any case");
            const std::string* hex = cs[2].find("skeleton#");
            check(hex && *hex == "00ff00ff00ff00ff", "an escaped '#' in a key reads back as '#'");
            const std::string text = fmt::writeOcworld(w);
            check(contains(text, "COMPONENT SkeletalMesh skeleton=a.ocskel") && !contains(text, "SKELETALMESH"),
                  "known types are written in canonical case");
            check(contains(text, "COMPONENT Wibble foo=1 zed=%20q"), "unknown ones as they were");
            check(contains(text, "skeleton%23=00ff00ff00ff00ff"), "the id-only key is written escaped, so '#' never starts a comment");
            fmt::OcWorldData b;
            check(fmt::parseOcworld(text, b, &err) && b.placements.size() == 1 &&
                      b.placements[0].components == w.placements[0].components,
                  "all of it round trips");
        }
    }

    AVER_INFO("=== 9. trailing ';' and comments ===");
    {
        fmt::OcWorldData w;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\n"
                                "PLACE a.ocmesh 0 0 0 0 0 0 1\n"
                                "  COMPONENT SkeletalMesh skeleton=a.ocskel;\n"
                                "  # a full-line comment\n"
                                "  COMPONENT Animator clip=b.ocanim speed=2 # trailing comment\n", w, &err), "parses: " + err);
        check(w.placements.size() == 1 && w.placements[0].components.size() == 2, "both lines read");
        if (w.placements.size() == 1 && w.placements[0].components.size() == 2) {
            const auto& cs = w.placements[0].components;
            const std::string* sk = cs[0].find("skeleton");
            check(cs[0].kv.size() == 1 && sk && *sk == "a.ocskel", "a trailing ';' is not part of the value");
            check(cs[1].kv.size() == 2 && *cs[1].find("speed") == "2", "a trailing comment is not part of a value or a key");
        }
    }

    AVER_INFO("=== 10. COMPONENT CollisionShape (SHAPES_MATERIALS) ===");
    {
        fmt::OcWorldData w;
        check(fmt::parseOcworld("OCWORLD 1\nNAME T\n"
                                "PLACE a.ocmesh 0 0 0 0 0 0 1\n"
                                "  component COLLISIONSHAPE asset=Physics/Chair.ocshape surface=%237e3a0f12 motion=dynamic noscale=1\n"
                                "PLACE b.ocmesh 0 0 0 0 0 0 1\n"
                                "  COMPONENT CollisionShape asset%23=0123456789abcdef\n", w, &err), "parses: " + err);
        check(w.placements.size() == 2 && w.placements[0].components.size() == 1 && w.placements[1].components.size() == 1, "one record on each placement");
        if (w.placements.size() == 2 && w.placements[0].components.size() == 1 && w.placements[1].components.size() == 1) {
            const fmt::OcPlacedComponent& a = w.placements[0].components[0];
            check(a.type == "CollisionShape", "the type reads in canonical case whatever case was written");
            const std::string* sf = a.find("surface");
            check(sf && *sf == "#7e3a0f12", "an escaped # in a value reads back as #");
            check(a.find("motion") && *a.find("motion") == "dynamic" && a.find("noscale") && *a.find("noscale") == "1", "motion and noscale read");
            check(w.placements[1].components[0].find("asset#") != nullptr, "the id-only asset key reads as asset#");
            const std::string text = fmt::writeOcworld(w);
            check(contains(text, "COMPONENT CollisionShape asset=Physics/Chair.ocshape surface=%237e3a0f12 motion=dynamic noscale=1") && contains(text, "asset%23=0123456789abcdef") && !contains(text, "COLLISIONSHAPE"),
                  "written in canonical case with the # escaped");
            fmt::OcWorldData b;
            check(fmt::parseOcworld(text, b, &err) && b.placements.size() == 2 && b.placements[0].components == w.placements[0].components &&
                      b.placements[1].components == w.placements[1].components,
                  "and round trips");
        }
    }
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
