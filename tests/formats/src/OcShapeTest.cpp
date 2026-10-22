// OcShapeTest -- the .ocshape collision shape tree (SHAPES_MATERIALS phase E): the example of the design parses, every leaf and
// decorator kind round-trips, write-then-parse is byte-identical, and every refusal says why (undefined reference, cycle, a
// non-finite number, an unknown kind, a mutable compound used twice, a newer version). Unknown records and options warn once.
//
// THE ROUND TRIP IS THE POINT, as for .ocsurf: a writer and a reader that disagree corrupt the file every time the editor saves.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcShape.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string withCrlf(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

static bool refused(const std::string& text, const std::string& reasonPart) {
    fmt::OcShapeData d;
    std::string err;
    const bool ok = fmt::parseOcShape(text, d, &err);
    const bool empty = d.nodes.empty() && d.children.empty() && d.root.empty();
    if (ok || !empty || err.find(reasonPart) == std::string::npos)
        AVER_INFO("      (refusal was: ok={} empty={} err='{}')", ok, empty, err);
    return !ok && empty && err.find(reasonPart) != std::string::npos;
}

static const char* kChair = R"(OCSHAPE 1
# a chair: four legs, a seat and a back
NAME Chair
MASS 12                          # kg
SURFACE Wood
NODE seat  box 40 40 4
NODE back  box 40 3 25
NODE leg   cylinder 3 45 radius 0.5
NODE top   compound static
CHILD top seat at 0 0 45
CHILD top back at -37 0 75 rot 0 0 0 1
CHILD top leg  at 32 32 22
CHILD top leg  at 32 -32 22
CHILD top leg  at -32 32 22
CHILD top leg  at -32 -32 22
ROOT top
)";

static void testParse() {
    AVER_INFO("=== parse ===");
    fmt::OcShapeData d;
    std::string err;
    check(fmt::parseOcShape(kChair, d, &err), "the design's chair parses: " + err);
    check(d.name == "Chair" && d.massKg == 12.0f && d.surface == "Wood" && d.root == "top", "header records");
    check(d.nodes.size() == 4 && d.children.size() == 6, "four nodes, six children");
    if (d.nodes.size() != 4 || d.children.size() != 6) return;
    check(d.nodes[0].kind == fmt::ShapeNodeKind::Box && d.nodes[0].nums.size() == 3 && d.nodes[0].nums[2] == 4.0f, "seat is a box 40 40 4");
    check(d.nodes[2].kind == fmt::ShapeNodeKind::Cylinder && d.nodes[2].radius == 0.5f && d.nodes[2].density < 0.0f, "the leg's convex radius is set, its density is not");
    check(d.nodes[3].kind == fmt::ShapeNodeKind::Compound && !d.nodes[3].mutableCompound, "top is a static compound");
    check(d.children[1].pos[0] == -37.0f && d.children[1].pos[2] == 75.0f && d.children[1].rot[3] == 1.0f, "a child's at and rot read");
    check(d.children[0].rot[3] == 1.0f && d.children[0].user == 0, "a child with no rot is identity, no user data");
    check(d.find("leg") != nullptr && d.find("nope") == nullptr, "find by id");

    fmt::OcShapeData crlf;
    check(fmt::parseOcShape(withCrlf(kChair), crlf, &err) && crlf.nodes.size() == 4, "a CRLF file parses the same");
    fmt::OcShapeData ci;
    check(fmt::parseOcShape("ocshape 1\nnode a SPHERE 3\nroot a\n", ci, &err) && ci.nodes.size() == 1, "record and kind words are case-insensitive");

    const std::vector<u32> order = fmt::ocShapeBuildOrder(d);
    check(order.size() == 4 && order.back() == 3, "the build order has the root last");
    check(std::count(order.begin(), order.end(), 2u) == 1, "a leaf used by four children is built once");
}

static void testEveryKind() {
    AVER_INFO("=== every kind ===");
    const char* all = R"(OCSHAPE 1
NODE b box 1 2 3 radius 0.1 density 500 surface Ice
NODE s sphere 5
NODE c capsule 3 20
NODE y cylinder 4 10
NODE tc tcapsule 3 2 20
NODE ty tcylinder 3 0 10
NODE tr triangle 0 0 0 10 0 0 0 10 0
NODE p plane 0 0 1 40 5000
NODE e empty 1 2 3
NODE h hullpoints 0 0 0  10 0 0  0 10 0  0 0 10  tolerance 0.2
NODE hm hull mesh Meshes/rock.ocmesh
NODE m mesh Meshes/floor.ocmesh edge -1 quality build
NODE r rt b at 1 2 3 rot 0 0.7071068 0 0.7071068
NODE r2 rt s
NODE o com c at 0 0 -5
NODE sc scale y 2 1 0.5
NODE mu compound mutable
CHILD mu r user 7
CHILD mu o at 5 0 0
ROOT mu
)";
    fmt::OcShapeData d;
    std::string err;
    check(fmt::parseOcShape(all, d, &err), "a file using every kind parses: " + err);
    check(d.nodes.size() == 17, "seventeen nodes");
    if (d.nodes.size() != 17) return;
    check(d.nodes[0].density == 500.0f && d.nodes[0].surface == "Ice" && d.nodes[0].radius == 0.1f, "leaf options read");
    check(d.nodes[7].nums.size() == 5 && d.nodes[7].nums[4] == 5000.0f, "plane takes five numbers");
    check(d.nodes[9].nums.size() == 12 && d.nodes[9].tolerance == 0.2f, "hullpoints reads all its numbers and then the option");
    check(d.nodes[10].kind == fmt::ShapeNodeKind::HullMesh && d.nodes[10].path == "Meshes/rock.ocmesh", "hull mesh keeps its path");
    check(d.nodes[11].kind == fmt::ShapeNodeKind::Mesh && d.nodes[11].hasEdge && d.nodes[11].edge == -1.0f && d.nodes[11].quality == 1, "mesh options: edge and quality");
    check(d.nodes[12].child == "b" && d.nodes[12].pos[2] == 3.0f && std::abs(d.nodes[12].rot[1] - 0.7071068f) < 1e-6f, "rt reads at and rot");
    check(d.nodes[13].pos[0] == 0.0f && d.nodes[13].rot[3] == 1.0f, "rt with neither is identity");
    check(d.nodes[14].kind == fmt::ShapeNodeKind::OffsetCom && d.nodes[14].pos[2] == -5.0f, "com reads its offset");
    check(d.nodes[15].kind == fmt::ShapeNodeKind::Scaled && d.nodes[15].scale[2] == 0.5f, "scale reads three numbers");
    check(d.nodes[16].mutableCompound && d.children[0].user == 7, "mutable compound and a child's user data");

    const std::string canon = fmt::writeOcShape(d);
    fmt::OcShapeData back;
    check(fmt::parseOcShape(canon, back, &err) && fmt::writeOcShape(back) == canon, "write-parse-write is byte-identical for every kind");
    bool same = back.nodes.size() == d.nodes.size() && back.children.size() == d.children.size();
    for (size_t i = 0; same && i < d.nodes.size(); ++i)
        same = back.nodes[i].id == d.nodes[i].id && back.nodes[i].kind == d.nodes[i].kind && back.nodes[i].nums == d.nodes[i].nums &&
               back.nodes[i].path == d.nodes[i].path && back.nodes[i].child == d.nodes[i].child && back.nodes[i].surface == d.nodes[i].surface &&
               back.nodes[i].radius == d.nodes[i].radius && back.nodes[i].density == d.nodes[i].density && back.nodes[i].quality == d.nodes[i].quality &&
               back.nodes[i].edge == d.nodes[i].edge && back.nodes[i].hasEdge == d.nodes[i].hasEdge && back.nodes[i].mutableCompound == d.nodes[i].mutableCompound;
    check(same, "every field reads back exactly");
}

static void testRoundTrip() {
    AVER_INFO("=== round trip ===");
    fmt::OcShapeData d;
    std::string err;
    fmt::parseOcShape(kChair, d, &err);
    const std::string canon = fmt::writeOcShape(d);
    check(canon.find("# a chair") == std::string::npos && canon.rfind("OCSHAPE 1\nNAME Chair\nMASS 12\nSURFACE Wood\n", 0) == 0,
          "the canonical form drops comments and keeps the header order");
    check(canon.find("CHILD top seat at 0 0 45\n") != std::string::npos, "a child writes its at");
    check(canon.find("rot 0 0 0 1") == std::string::npos, "an identity rotation is not written");
    fmt::OcShapeData back;
    check(fmt::parseOcShape(canon, back, &err) && fmt::writeOcShape(back) == canon, "write-then-parse-then-write is byte-identical");

    // Not a round decimal: the writer must keep every digit the f32 needs.
    fmt::OcShapeData third;
    third.nodes.push_back({});
    third.nodes[0].id = "a";
    third.nodes[0].kind = fmt::ShapeNodeKind::Sphere;
    third.nodes[0].nums = {1.0f / 3.0f};
    third.root = "a";
    fmt::OcShapeData thirdBack;
    check(fmt::parseOcShape(fmt::writeOcShape(third), thirdBack, &err) && thirdBack.nodes[0].nums[0] == 1.0f / 3.0f, "1/3 survives exactly");

    // A zero mass writes no MASS record.
    fmt::OcShapeData noMass;
    fmt::parseOcShape("OCSHAPE 1\nNODE a sphere 3\nROOT a\n", noMass, &err);
    check(fmt::writeOcShape(noMass) == "OCSHAPE 1\nNODE a sphere 3\nROOT a\n", "the smallest shape writes the smallest file");
}

static void testRefusals() {
    AVER_INFO("=== refusals ===");
    check(refused("", "OCSHAPE"), "an empty file has no header");
    check(refused("NODE a sphere 1\n", "OCSHAPE 1"), "a record before the header");
    check(refused("OCSHAPE 2\nNODE a sphere 1\nROOT a\n", "newer"), "a newer version is refused, saying so");
    check(refused("OCSHAPE 0\n", "version"), "version 0 is refused");
    check(refused("OCSHAPE 1\nNODE a sphere 1\n", "ROOT"), "no ROOT");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nROOT b\n", "undefined node 'b'"), "ROOT names an undefined node");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE a box 1 1 1\nROOT a\n", "declared twice"), "a duplicate id");
    check(refused("OCSHAPE 1\nNODE bad-id sphere 1\nROOT bad-id\n", "bad node id"), "an invalid id");
    check(refused("OCSHAPE 1\nNODE a blob 1\nROOT a\n", "unknown node kind"), "an unknown kind");
    check(refused("OCSHAPE 1\nNODE a sphere\nROOT a\n", "number"), "a sphere with no radius");
    check(refused("OCSHAPE 1\nNODE a sphere nan\nROOT a\n", "bad number"), "a NaN number");
    check(refused("OCSHAPE 1\nNODE a sphere 1x\nROOT a\n", "bad number"), "a numeric prefix is not a number");
    check(refused("OCSHAPE 1\nNODE a sphere inf\nROOT a\n", "bad number"), "an infinite number");
    check(refused("OCSHAPE 1\nNODE a sphere -1\nROOT a\n", "positive radius"), "a negative radius");
    check(refused("OCSHAPE 1\nNODE a capsule 5 8\nROOT a\n", "capsule"), "a capsule shorter than its caps");
    check(refused("OCSHAPE 1\nNODE a tcylinder 0 0 5\nROOT a\n", "tapered cylinder"), "a tapered cylinder with both radii zero");
    check(refused("OCSHAPE 1\nNODE a triangle 0 0 0 1 1 1 2 2 2\nROOT a\n", "collinear"), "a collinear triangle");
    check(refused("OCSHAPE 1\nNODE a plane 0 0 0 5 100\nROOT a\n", "normal"), "a plane with a zero normal");
    check(refused("OCSHAPE 1\nNODE a plane 0 0 1 5 0.1\nROOT a\n", "half extent"), "a plane with a tiny half extent");
    check(refused("OCSHAPE 1\nNODE a hullpoints 0 0 0 1 0 0 0 1 0\nROOT a\n", "four points"), "a hull of three points");
    check(refused("OCSHAPE 1\nNODE a hullpoints 0 0 0 1 0 0 0 1 0 0 0\nROOT a\n", "three numbers per point"), "a hull with a stray number");
    check(refused("OCSHAPE 1\nNODE a sphere 1 surface Bad-Name\nROOT a\n", "surface"), "a bad surface name");
    check(refused("OCSHAPE 1\nNODE a sphere 1 quality fast\nROOT a\n", "quality"), "an unknown quality word");
    check(refused("OCSHAPE 1\nNODE a sphere 1 radius\nROOT a\n", "no value"), "an option with no value");
    check(refused("OCSHAPE 1\nNODE a hull mesh\nROOT a\n", "mesh <path>"), "hull mesh with no path");
    check(refused("OCSHAPE 1\nNODE a rt ghost\nROOT a\n", "undefined child 'ghost'"), "a decorator with an undefined child");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE r rt a at 1 2\nROOT r\n", "'at' needs three"), "a short at");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE r rt a rot 0 0 0 0\nROOT r\n", "rotation"), "a zero rotation");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE s scale a 1 0 1\nROOT s\n", "scale"), "a zero scale component");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE o com a\nROOT o\n", "'at x y z'"), "com without an offset");
    check(refused("OCSHAPE 1\nNODE c compound static\nROOT c\n", "children"), "a compound with no children");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE c compound maybe\nROOT c\n", "static"), "a compound that is neither static nor mutable");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nCHILD ghost a\nROOT a\n", "undefined compound"), "a CHILD of an undefined compound");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nCHILD a a\nROOT a\n", "not a compound"), "a CHILD of a leaf");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE c compound static\nCHILD c ghost\nROOT c\n", "undefined node 'ghost'"), "a CHILD naming an undefined node");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE c compound static\nCHILD c a at 1 2\nROOT c\n", "'at' needs three"), "a CHILD with a short at");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE c compound static\nCHILD c a spin 3\nROOT c\n", "unexpected"), "a CHILD with an unknown word");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nROOT a\nROOT a\n", "second ROOT"), "two ROOT records");
    check(refused("OCSHAPE 1\nMASS -2\nNODE a sphere 1\nROOT a\n", "MASS"), "a negative mass");

    // A cycle: a rt wraps b, b rt wraps a.
    check(refused("OCSHAPE 1\nNODE a rt b\nNODE b rt a\nROOT a\n", "cycle"), "a decorator cycle");
    check(refused("OCSHAPE 1\nNODE c compound static\nCHILD c c\nROOT c\n", "cycle"), "a compound containing itself");

    // A mutable compound belongs to one body.
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE m compound mutable\nCHILD m a\nNODE p compound static\nCHILD p m\nCHILD p m at 10 0 0\nROOT p\n",
                  "mutable compound 'm' is used 2 times"), "a mutable compound used twice");
    fmt::OcShapeData once;
    std::string err;
    check(fmt::parseOcShape("OCSHAPE 1\nNODE a sphere 1\nNODE m compound mutable\nCHILD m a\nROOT m\n", once, &err), "a mutable compound used once is fine: " + err);
    fmt::OcShapeData shared;
    check(fmt::parseOcShape("OCSHAPE 1\nNODE a sphere 1\nNODE c compound static\nCHILD c a\nCHILD c a at 10 0 0\nROOT c\n", shared, &err), "a shared leaf is fine: " + err);

    std::string many = "OCSHAPE 1\n";
    for (int i = 0; i < 4096; ++i) many += "NODE n" + std::to_string(i) + " sphere 1\n";
    many += "ROOT n0\n";
    fmt::OcShapeData d;
    check(fmt::parseOcShape(many, d, &err) && d.nodes.size() == 4096, "4096 nodes are accepted");
    many += "NODE one_more sphere 1\n";
    check(refused(many, "4096"), "4097 are refused");
    check(refused("OCSHAPE 1\nNODE a sphere 1\nNODE b sphere\nROOT a\n", "line 3"), "the reason names the line");

    // Forward compatibility: what a newer engine added is skipped, not fatal.
    fmt::OcShapeData fwd;
    check(fmt::parseOcShape("OCSHAPE 1\nFUTURE thing 1\nNODE a sphere 3 sparkle 9 density 800\nROOT a\n", fwd, &err) && fwd.nodes.size() == 1 &&
          fwd.nodes[0].density == 800.0f, "an unknown record and an unknown option are skipped, the rest of the line still reads");

    // validateOcShape on hand-built data (the editor's path) lists every problem, not just the first.
    fmt::OcShapeData bad;
    bad.nodes.push_back({});
    bad.nodes[0].id = "a";
    bad.nodes[0].kind = fmt::ShapeNodeKind::Box;
    bad.nodes[0].nums = {1.0f, -2.0f};
    bad.massKg = -1.0f;
    const std::vector<std::string> problems = fmt::validateOcShape(bad);
    check(problems.size() >= 3, "a hand-built shape with a bad box, a bad mass and no ROOT lists at least three problems");
}

static void testFiles() {
    AVER_INFO("=== load and save ===");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_ocshape_test" / "Content" / "Physics";
    std::filesystem::remove_all(dir.parent_path().parent_path());
    const std::string path = (dir / "Chair.ocshape").string();

    fmt::OcShapeData d;
    std::string err;
    fmt::parseOcShape(kChair, d, &err);
    check(fmt::saveOcShape(path, d, &err), "saveOcShape creates the folder and writes: " + err);
    fmt::OcShapeData loaded;
    check(fmt::loadOcShape(path, loaded, &err) && fmt::writeOcShape(loaded) == fmt::writeOcShape(d), "loadOcShape reads back what was saved");
    check(!fmt::loadOcShape((dir / "Missing.ocshape").string(), loaded, &err) && err.find("cannot open") != std::string::npos, "a missing file is refused with a reason");

    { std::ofstream bad(path, std::ios::binary | std::ios::trunc); bad << "OCSHAPE 1\nNODE a sphere nope\nROOT a\n"; }
    check(!fmt::loadOcShape(path, loaded, &err), "a damaged file does not load");
    std::filesystem::remove_all(dir.parent_path().parent_path());
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO(".ocshape: the collision shape tree");
    testParse();
    testEveryKind();
    testRoundTrip();
    testRefusals();
    testFiles();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
