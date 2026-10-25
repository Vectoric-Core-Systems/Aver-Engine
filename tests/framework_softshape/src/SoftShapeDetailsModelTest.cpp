// The ImGui-free half of the Jelly and Rope Details sections (sandbox/src/SoftShapeDetailsModel.hpp) and the wave 11 Solver / Inflate rows of
// SoftBodyDetailsModel.hpp: the record keys and their defaults, the advice and state lines, the rope end handle arithmetic and the overlay
// colours. Header-only models, so this needs no editor, no scene and no GPU. Exits 0 or 1.
#include "aver/core/Log.hpp"

#include "SoftShapeDetailsModel.hpp"

#include <cmath>
#include <string>

using namespace aver;
namespace shm = aver::editor::shapemodel;
namespace sm = aver::editor::softmodel;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("SoftShapeDetailsModelTest");

    AVER_INFO("add component rows");
    {
        check(shm::kShapeKinds.size() == 2 && shm::shapeKind("Jelly") && shm::shapeKind("Jelly")->needsMesh, "Jelly needs a mesh placement");
        check(shm::shapeKind("Rope") && !shm::shapeKind("Rope")->needsMesh, "Rope needs none");
        check(shm::shapeKind("Cloth") == nullptr, "and nothing else is a shape");
    }

    AVER_INFO("records");
    {
        fmt::OcPlacedComponent j;
        j.type = "Jelly";
        j.kv = {{"stiffness", "1"}, {"cell", "0"}, {"gravity", "1.0"}, {"damping", "2"}, {"world", "1"}, {"gravityon", "0"}, {"asset", "Soft/Blob.ocsoft"}};
        shm::canonicalize(j);
        check(j.find("stiffness") == nullptr && j.find("cell") == nullptr && j.find("gravity") == nullptr && j.find("world") == nullptr,
              "a jelly's default keys are dropped");
        check(j.find("damping") && j.find("gravityon") && j.find("asset"), "the others stay");
        fmt::OcPlacedComponent again = j;
        shm::canonicalize(again);
        check(again.kv == j.kv, "canonicalize is idempotent");

        fmt::OcPlacedComponent r = shm::newRopeRecord(Vec3{0, 0, 300}, Vec3{200, 0, 300});
        check(r.type == "Rope" && r.find("start") && r.find("end") && !r.find("length"), "a new rope record has its two points and no length");
        r.kv.emplace_back("length", "0");
        r.kv.emplace_back("startpin", "1");
        r.kv.emplace_back("endpin", "0");
        shm::canonicalize(r);
        check(!r.find("length") && !r.find("startpin") && r.find("endpin") && r.find("start") && r.find("end"), "a rope drops length 0 and a pinned end, keeps a free one and its points");

        Vec3 v;
        check(shm::parseVec3("1,2.5,-3", v) && v.x == 1.0f && v.y == 2.5f && v.z == -3.0f, "x,y,z parses");
        check(!shm::parseVec3("1,2", v) && !shm::parseVec3("1,2,x", v) && !shm::parseVec3("", v), "a short or malformed vector does not");
        check(shm::parseVec3(shm::vec3Text(Vec3{12.5f, -4.0f, 100.0f}), v) && v.x == 12.5f && v.z == 100.0f, "vec3Text round trips");
        const fmt::OcPlacedComponent lengthy = shm::newRopeRecord(Vec3{0, 0, 0}, Vec3{1, 0, 0}, 250.0f, 2.0f);
        check(lengthy.find("length") && lengthy.find("radius"), "a named length and radius are written");
    }

    AVER_INFO("assets and views");
    {
        check(shm::starterJelly().valid() && shm::starterJelly().kind == softbody::SoftKind::Jelly, "the starter jelly is valid");
        check(shm::starterRope().valid() && shm::starterRope().kind == softbody::SoftKind::Rope, "and so is the starter rope");
        check(shm::jellyAssetPath("Meshes/Blob.ocmesh") == "Meshes/Blob.ocsoft", "a jelly's asset sits beside its mesh");
        check(shm::ropeAssetPath("Crane Cable (2)") == "Soft/Crane_Cable__2_.ocsoft" && shm::ropeAssetPath("") == "Soft/Rope.ocsoft", "a rope's asset is named after the entity, safely");

        const shm::JellyView jv = shm::viewOf(shm::starterJelly());
        check(std::fabs(jv.cellCm - 10.0f) < 1e-6f && std::fabs(jv.vertexRadiusCm - 3.5f) < 1e-5f && jv.maxNodes == 2048, "the jelly view reads the asset (vertex radius 0.35 of the cell)");
        check(std::fabs(shm::viewOf(shm::starterJelly(), 20.0f).vertexRadiusCm - 7.0f) < 1e-5f, "and a cell override scales it");
        const shm::RopeView rv = shm::ropeViewOf(shm::starterRope(), 3.0f);
        check(std::fabs(rv.radiusCm - 3.0f) < 1e-6f && rv.sides == 8 && std::fabs(rv.segmentCm - 12.0f) < 1e-6f, "the rope view reads the asset, the radius overridden");

        check(std::fabs(shm::ropeRestLength(Vec3{0, 0, 0}, Vec3{200, 0, 0}, 0.0f, 1.05f) - 210.0f) < 1e-3f, "no length: distance x slack");
        check(std::fabs(shm::ropeRestLength(Vec3{0, 0, 0}, Vec3{200, 0, 0}, 150.0f, 1.05f) - 200.0f) < 1e-3f, "a length shorter than the distance is the distance");
        check(shm::ropeNodeCount(240.0f, 12.0f) == 21 && shm::ropeNodeCount(1.0f, 1000.0f) == 3, "node counts match the planner (21 for 240 cm in 12 cm segments, never under 3)");
    }

    AVER_INFO("advice");
    {
        check(shm::jellyAdvice(true, false, false, false, 2048).empty(), "a healthy jelly has nothing to say");
        check(shm::jellyAdvice(false, false, false, false, 2048).find("needs a mesh") != std::string::npos, "no mesh");
        check(shm::jellyAdvice(true, true, false, false, 2048).find("not closed") != std::string::npos, "an open mesh");
        check(shm::jellyAdvice(true, true, true, false, 2048).find("2048") != std::string::npos, "over the cap says the cap");
        check(shm::jellyAdvice(true, false, false, true, 2048).find("asset") != std::string::npos, "a stale asset");
        check(shm::ropeAdvice(false, false, false, false, false, 20, 4000).find("falls") != std::string::npos, "a rope pinned nowhere falls");
        check(shm::ropeAdvice(false, false, false, true, false, 20, 4000).empty(), "one pin is fine");
        check(shm::ropeAdvice(false, true, false, true, true, 0, 4000).find("4000") != std::string::npos, "over the cap says the cap");
        check(!shm::ropeAdvice(false, false, false, true, true, 3000, 4000).empty(), "a very long rope is warned about");
        check(shm::jellyStateText(true, false, false, 216, 0.98f, 0.12f).find("216 nodes") != std::string::npos &&
                  shm::jellyStateText(false, false, true, 0, 0, 0) == "not simulated",
              "jelly state text");
        check(shm::ropeStateText(true, false, false, true, 20, 221.0f, 0.004f).find("cut") != std::string::npos &&
                  shm::ropeStateText(false, true, false, false, 20, 221.0f, 0.0f).find("frozen") != std::string::npos,
              "rope state text");
    }

    AVER_INFO("cloth solver rows");
    {
        check(sm::kSolverRows.size() == 2 && std::string(sm::solverLabel(softbody::ClothSolver::Fast)) == "Fast", "the solver rows");
        check(std::string(sm::bendLabel(softbody::ClothBend::Dihedral)) == "Dihedral" && std::string(sm::lraLabel(softbody::ClothLra::Geodesic)) == "Geodesic", "bend and lra labels");
        check(!sm::solverAdvice(softbody::ClothSolver::Fast, true).empty() && sm::solverAdvice(softbody::ClothSolver::Cage, false).empty(), "a tearable fast cloth is called out");
        check(sm::inflateAdvice(3.0f, true, false, true).find("closed") != std::string::npos, "inflate over an open mesh says so");
        check(sm::inflateAdvice(3.0f, false, false, true).find("kPa") != std::string::npos, "before the mesh is looked at it gives the units");
        check(sm::inflateAdvice(0.0f, true, true, true).empty(), "no pressure, nothing to say");
        check(sm::inflateAdvice(3.0f, true, true, false).find("asset") != std::string::npos, "pressure needs an asset");
        check(sm::pressureOk(0.0f) && sm::pressureOk(12.0f) && !sm::pressureOk(-1.0f) && !sm::pressureOk(std::nanf("")), "pressureOk");
    }

    AVER_INFO("rope end handles");
    {
        Mat4 anchor = Mat4::translation(Vec3{100, 0, 50});
        const Vec3 off{0, 20, 0};
        const Vec3 world = shm::ropeEndWorld(&anchor, off);
        check(dist(world, Vec3{100, 20, 50}) < 1e-4f, "an offset in an anchor's space lands at the anchor's matrix");
        check(dist(shm::ropeEndWorld(nullptr, off), off) < 1e-6f, "with no anchor the offset is the world point");
        const Vec3 back = shm::ropeEndOffsetFor(&anchor, Vec3{130, 20, 50});
        check(dist(back, Vec3{30, 20, 0}) < 1e-3f, "and the inverse turns a dragged world point back into an offset");
        Mat4 rot = Mat4::fromQuat(Quat::fromAxisAngle(Vec3{0, 0, 1}, 0.9f)) * Mat4::translation(Vec3{5, 6, 7});
        const Vec3 p{12, -3, 9};
        check(dist(shm::ropeEndOffsetFor(&rot, shm::ropeEndWorld(&rot, p)), p) < 1e-3f, "world and offset round trip through a rotated anchor");

        const Vec3 handles[2] = {{0, 0, 100}, {300, 0, 100}};
        check(shm::pickHandle(handles, 2, Vec3{-100, 0, 100}, Vec3{1, 0, 0}, 10.0f) == 0, "a ray along the rope picks the near handle");
        check(shm::pickHandle(handles, 2, Vec3{300, -200, 100}, Vec3{0, 1, 0}, 10.0f) == 1, "a ray at the far one picks it");
        check(shm::pickHandle(handles, 2, Vec3{150, -200, 100}, Vec3{0, 1, 0}, 10.0f) == -1, "a ray between them picks nothing");
        check(shm::pickHandle(handles, 2, Vec3{100, 0, 100}, Vec3{1, 0, 0}, 10.0f) == 1, "a handle behind the origin is not picked");
    }

    AVER_INFO("overlay colours");
    {
        f32 r0, g0, b0, r1, g1, b1, r2, g2, b2;
        shm::stretchColour(0.0f, r0, g0, b0);
        shm::stretchColour(0.3f, r1, g1, b1);
        shm::stretchColour(-0.3f, r2, g2, b2);
        check(g0 > r0 && g0 > b0, "at rest an edge is green");
        check(r1 > g1 && r1 > b1, "stretched 30% it is red");
        check(b2 > r2 && b2 > g2, "compressed 30% it is blue");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
