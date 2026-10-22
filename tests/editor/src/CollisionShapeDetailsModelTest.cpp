// CollisionShapeDetailsModelTest -- the Collision Shape Details section without a window (sandbox/src/CollisionShapeDetailsModel.hpp,
// header-only): when the component may be attached, what "Create .ocshape from the Rigid Body" writes for each of the four body
// shapes (and that it parses back and validates), the asset path, the canonical COMPONENT record (keys at their default omitted),
// the Play-time read-out lines and the surface probe text. The widgets themselves are ImGui and covered by the owner's headed run.
#include "CollisionShapeDetailsModel.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
namespace cm = aver::editor::csmodel;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static void testAttach() {
    AVER_INFO("=== attach ===");
    check(cm::attachRefusal(true) == nullptr, "with a Rigid Body the component may be attached");
    const char* reason = cm::attachRefusal(false);
    check(reason && std::string(reason).find("Rigid Body") != std::string::npos, "without one it is refused, saying why");
}

static void testPaths() {
    AVER_INFO("=== names and paths ===");
    check(cm::sanitiseName("Chair") == "Chair" && cm::sanitiseName("Chair 01") == "Chair_01" && cm::sanitiseName("a/b\\c.d") == "a_b_c_d", "a name keeps letters, digits and underscore");
    check(cm::sanitiseName("") == "Shape" && cm::sanitiseName("  ") == "Shape" && cm::sanitiseName("!!!") == "Shape", "an empty name falls back to Shape");
    check(cm::sanitiseName("(1)Door") == "1_Door", "leading separators are dropped");
    check(cm::assetPathForEntity("Chair 01") == "Physics/Chair_01.ocshape", "the Create path is Physics/<Name>.ocshape");
}

static void testFromRigidBody() {
    AVER_INFO("=== Create .ocshape from the Rigid Body ===");
    std::string why;
    fmt::OcShapeData d;

    scene::CRigidBody box;
    box.shapeKind = scene::kRigidBodyShapeBox;
    box.dims[0] = 50; box.dims[1] = 30; box.dims[2] = 20;
    box.massKg = 40.0f;
    check(cm::shapeFromRigidBody(box, d, why), "a box converts: " + why);
    check(d.nodes.size() == 1 && d.nodes[0].kind == fmt::ShapeNodeKind::Box && d.nodes[0].nums.size() == 3 && d.nodes[0].nums[1] == 30.0f && d.root == "body" && d.massKg == 40.0f,
          "one box node with the body's half extents and mass, rooted");

    scene::CRigidBody sphere;
    sphere.shapeKind = scene::kRigidBodyShapeSphere;
    sphere.dims[0] = 25;
    check(cm::shapeFromRigidBody(sphere, d, why) && d.nodes[0].kind == fmt::ShapeNodeKind::Sphere && d.nodes[0].nums[0] == 25.0f && d.massKg == 0.0f, "a sphere: its radius, and no mass when the body has none");

    scene::CRigidBody capsule;
    capsule.shapeKind = scene::kRigidBodyShapeCapsule;
    capsule.dims[0] = 30; capsule.dims[1] = 60;   // radius, HALF height of the cylindrical part
    check(cm::shapeFromRigidBody(capsule, d, why) && d.nodes[0].kind == fmt::ShapeNodeKind::Capsule && near(d.nodes[0].nums[0], 30.0f) && near(d.nodes[0].nums[1], 180.0f),
          "a capsule's total height is 2 * (half + radius) = 180");

    scene::CRigidBody cylinder;
    cylinder.shapeKind = scene::kRigidBodyShapeCylinder;
    cylinder.dims[0] = 10; cylinder.dims[1] = 45;   // radius, HALF height
    check(cm::shapeFromRigidBody(cylinder, d, why) && d.nodes[0].kind == fmt::ShapeNodeKind::Cylinder && near(d.nodes[0].nums[1], 90.0f), "a cylinder's full height is twice the half");

    // Whatever it writes parses back and validates, and a second write is identical.
    const std::string text = fmt::writeOcShape(d);
    fmt::OcShapeData back;
    std::string err;
    check(fmt::parseOcShape(text, back, &err) && fmt::writeOcShape(back) == text, "the written file parses back and re-writes byte for byte: " + err);

    scene::CRigidBody blank;   // a freshly attached default Rigid Body: a box of zero size
    check(!cm::shapeFromRigidBody(blank, d, why) && why.find("no usable size") != std::string::npos && d.nodes.empty(), "a default Rigid Body with no size is refused, saying so");
    scene::CRigidBody odd;
    odd.shapeKind = 99;
    check(!cm::shapeFromRigidBody(odd, d, why) && why.find("shape kind") != std::string::npos, "an unknown shape kind is refused");
    scene::CRigidBody stubby;
    stubby.shapeKind = scene::kRigidBodyShapeCapsule;
    stubby.dims[0] = 30; stubby.dims[1] = 0;   // a capsule whose cylinder has no length is a sphere, not a capsule the builder accepts
    check(!cm::shapeFromRigidBody(stubby, d, why), "a capsule with no cylindrical part is refused (the builder needs a total height above twice the radius)");
}

static void testRecord() {
    AVER_INFO("=== the record ===");
    auto r = cm::makeRecord("", "", false);
    check(r.type == "CollisionShape" && r.kv.empty(), "an empty component is a record with no keys");
    r = cm::makeRecord("Physics/Chair.ocshape", "Wood", true);
    check(r.kv.size() == 3 && r.kv[0] == std::make_pair(std::string("asset"), std::string("Physics/Chair.ocshape")) &&
          r.kv[1] == std::make_pair(std::string("surface"), std::string("Wood")) && r.kv[2] == std::make_pair(std::string("noscale"), std::string("1")),
          "asset, surface, noscale in that order");
    r = cm::makeRecord("Physics/Chair.ocshape", "Wood", true, "dynamic");
    check(r.kv.size() == 4 && r.kv[2].first == "motion" && r.kv[2].second == "dynamic" && r.kv[3].first == "noscale", "motion goes between surface and noscale");
    r = cm::makeRecord("Physics/Chair.ocshape", "", false);
    check(r.kv.size() == 1 && r.kv[0].first == "asset", "keys at their default are omitted");
    r = cm::makeRecord("", "#7e3a0f12", false);
    check(r.kv.size() == 1 && r.kv[0].second == "#7e3a0f12", "an unknown surface keeps its #hex spelling");
}

static void testReadouts() {
    AVER_INFO("=== read-outs and the probe ===");
    check(std::string(cm::typeLabel(13)) == "Static compound" && std::string(cm::typeLabel(14)) == "Mutable compound" && std::string(cm::typeLabel(1)) == "Box", "type labels follow the ABI's numbers");
    check(std::string(cm::typeLabel(-1)) == "Unknown" && std::string(cm::typeLabel(99)) == "Unknown", "an out-of-range type reads Unknown");
    check(cm::readoutLines(cm::ShapeReadout{}).empty(), "an invalid read-out draws nothing");

    cm::ShapeReadout r;
    r.valid = true;
    r.type = 13;
    r.volumeCm3 = 80289.0f;
    r.massKg = 12.0f;
    r.com[2] = 38.5f;
    r.boundsMin[0] = -40; r.boundsMax[0] = 40; r.boundsMin[1] = -40; r.boundsMax[1] = 40; r.boundsMin[2] = 0; r.boundsMax[2] = 100;
    r.children = 6; r.leaves = 6;
    const std::vector<std::string> lines = cm::readoutLines(r);
    check(lines.size() == 6, "type, volume, mass, centre of mass, bounds and the child counts");
    check(lines.size() == 6 && lines[0] == "Shape: Static compound" && lines[1].find("80289 cm3") != std::string::npos && lines[2] == "Mass: 12.0 kg" &&
          lines[3] == "Centre of mass: (0.0, 0.0, 38.5) cm" && lines[4] == "Bounds: 80.0 x 80.0 x 100.0 cm" && lines[5] == "6 child(ren), 6 leaf(s)", "the lines read as designed");
    r.massKg = 0.0f; r.children = 0; r.triangles = 1200; r.type = 11; r.volumeCm3 = 0.0f;
    const std::vector<std::string> mesh = cm::readoutLines(r);
    check(mesh.size() == 5 && mesh[0] == "Shape: Mesh" && mesh[1].find("none") != std::string::npos && mesh[4] == "1200 triangle(s)", "a mesh has no volume or mass line values, and a triangle count");

    cm::Probe p;
    check(cm::formatProbe(p) == "Nothing hit.", "a miss says so");
    p.hit = true; p.surface = 0; p.distanceCm = 250.0f;
    check(cm::formatProbe(p) == "no surface  friction unset  restitution unset  tag 0  at 250 cm", "a hit with no surface");
    p.surface = aver::surfaceIdFromName("Ice"); p.name = "Ice"; p.friction = 0.03f; p.tag = 2; p.child = 1; p.triangle = 12;
    check(cm::formatProbe(p) == "Ice  friction 0.03  restitution unset  tag 2  (child 1, triangle 12)  at 250 cm", "a named surface with its numbers and where it was hit");
    p.name.clear(); p.child = -1; p.triangle = -1; p.cellX = 3; p.cellY = 4; p.restitution = 0.5f;
    const std::string hex = cm::formatProbe(p);
    check(hex.find("#") == 0 && hex.find("restitution 0.50") != std::string::npos && hex.find("(cell 3,4)") != std::string::npos, "an id the table does not name reads #hex, and a heightfield cell shows");

    check(cm::assetProblem(0, "", false, "").empty(), "no asset is not a problem");
    check(cm::assetProblem(7, "", false, "").find("not under Content") != std::string::npos, "an asset outside Content is called out");
    check(cm::assetProblem(7, "Physics/A.ocshape", false, "line 3").find("line 3") != std::string::npos, "a file that does not parse repeats why");
    check(cm::assetProblem(7, "Physics/A.ocshape", true, "").empty(), "a loaded asset is fine");
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("Collision Shape details model");
    testAttach();
    testPaths();
    testFromRigidBody();
    testRecord();
    testReadouts();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
