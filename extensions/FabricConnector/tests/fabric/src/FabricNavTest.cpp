// FabricNavTest -- G5 N1 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/NavGame, then the level it wrote read back. The navigation nodes
// must be placed as their own classes with Godot's properties, a baked NavigationMesh (inline or external) must arrive as an Object(NavigationMesh, ...) value
// with its polygons, a mesh that was never baked must ask the region to bake it when it is ready, and every node and baking parameter must be rated.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotVariant.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;
namespace gd = aver::fmt::godot;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
#if defined(_WIN32)
    const std::string wrapped = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    const std::string wrapped = cmd + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static std::string slurp(const std::string& path) {
    std::string s;
    readFileText(path, s);
    return s;
}

static int find(const fmt::OcWorldData& w, std::initializer_list<const char*> path) {
    int parent = -1;
    for (const char* name : path) {
        int found = -1;
        for (usize i = 0; i < w.placements.size(); ++i)
            if (w.placements[i].parent == parent && w.placements[i].name == name) { found = static_cast<int>(i); break; }
        if (found < 0) return -1;
        parent = found;
    }
    return parent;
}

static std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The level reader may lower-case keys; compare without case.
static const std::string* propOf(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    const std::string want = lowered(key);
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components) {
        if (c.type != "Props") continue;
        if (const std::string* v = c.find(key)) return v;
        for (const auto& kv : c.kv)
            if (lowered(kv.first) == want) return &kv.second;
    }
    return nullptr;
}

static bool parsed(const std::string* text, gd::GodotVariant& v) {
    if (!text) return false;
    gd::GodotDiagnostics d;
    return gd::parseGodotVariant(*text, v, &d, "value");
}

static double numProp(const fmt::OcWorldData& w, int idx, const char* key, double def = -1e30) {
    const std::string* s = propOf(w, idx, key);
    return s ? std::atof(s->c_str()) : def;
}

static bool isObject(const gd::GodotVariant* v, const char* cls) { return v && v->type == gd::GodotVType::Object && v->s == cls; }

int main() {
    AVER_INFO("FabricNavTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/NavGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-nav-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/NavGame/Content";
    std::string err;
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);

    // ---- the classes -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- classes --");
    const auto classOf = [&](std::initializer_list<const char*> path) {
        const int i = find(w, path);
        return i < 0 ? std::string("<missing>") : w.placements[static_cast<usize>(i)].className;
    };
    check(classOf({"Main", "Ground"}) == "NavigationRegion3D", "NavigationRegion3D is placed as a NavigationRegion3D");
    check(classOf({"Main", "Upper"}) == "NavigationRegion3D" && classOf({"Main", "Fresh"}) == "NavigationRegion3D", "so are the other two regions");
    check(classOf({"Main", "Stair"}) == "NavigationLink3D", "NavigationLink3D is placed as a NavigationLink3D");
    check(classOf({"Main", "Crate"}) == "NavigationObstacle3D" && classOf({"Main", "Wall"}) == "NavigationObstacle3D", "NavigationObstacle3D is placed as itself");
    check(classOf({"Main", "Walker", "NavigationAgent3D"}) == "NavigationAgent3D", "NavigationAgent3D is placed as a NavigationAgent3D under its mover");
    check(propOf(w, find(w, {"Main", "Walker", "NavigationAgent3D"}), "_class") == nullptr, "and is not a carrier");

    // ---- the regions -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- regions --");
    const int ground = find(w, {"Main", "Ground"});
    gd::GodotVariant gm;
    check(parsed(propOf(w, ground, "navigation_mesh"), gm) && isObject(&gm, "NavigationMesh"), "Ground's navigation_mesh is Object(NavigationMesh, ...)");
    const gd::GodotVariant* verts = gm.find("vertices");
    const gd::GodotVariant* polys = gm.find("polygons");
    check(verts && verts->type == gd::GodotVType::PackedVector3Array && verts->nums.size() == 12 && near(verts->nums[0], -5) && near(verts->nums[3], 5),
          "the baked vertices stay in Godot's metres and axes (the runtime places them through the axis map)");
    check(polys && polys->type == gd::GodotVType::Array && polys->items.size() == 1 && polys->items[0].nums.size() == 4, "the baked polygon (four indices) is kept");
    check(gm.find("cell_size") && near(gm.find("cell_size")->asNumber(), 0.25) && gm.find("agent_radius") && near(gm.find("agent_radius")->asNumber(), 0.4) &&
              gm.find("agent_height") && near(gm.find("agent_height")->asNumber(), 1.8),
          "the baking parameters are kept");
    check(numProp(w, ground, "navigation_layers") == 3 && near(numProp(w, ground, "enter_cost"), 2.0) && near(numProp(w, ground, "travel_cost"), 1.5),
          "navigation_layers, enter_cost and travel_cost are kept");
    const std::string* edge = propOf(w, ground, "use_edge_connections");
    check(edge && *edge == "false", "use_edge_connections is kept");
    check(propOf(w, ground, "bake_on_ready") == nullptr, "a baked mesh does not ask to be baked");

    const int upper = find(w, {"Main", "Upper"});
    gd::GodotVariant um;
    check(parsed(propOf(w, upper, "navigation_mesh"), um) && isObject(&um, "NavigationMesh") && um.find("vertices") && um.find("vertices")->nums.size() == 12 &&
              um.find("polygons") && um.find("polygons")->items.size() == 1,
          "an external NavigationMesh .tres arrives as the same Object(NavigationMesh, ...) value");
    {
        // Godot (10, 3, 0) m is engine (0, 1000, 300) cm: +X is engine +Y, +Y is engine +Z (AxisMap.hpp).
        const fmt::OcWorldPlacement& p = w.placements[static_cast<usize>(upper)];
        check(near(p.x, 0, 0.05) && near(p.y, 1000, 0.05) && near(p.z, 300, 0.05), "the region's transform goes through the axis map and metres become centimetres");
    }

    const int fresh = find(w, {"Main", "Fresh"});
    gd::GodotVariant fm;
    check(parsed(propOf(w, fresh, "navigation_mesh"), fm) && isObject(&fm, "NavigationMesh") && !fm.find("polygons"), "a never-baked mesh has no polygons");
    const std::string* bor = propOf(w, fresh, "bake_on_ready");
    check(bor && *bor == "true", "and its region is told to bake it when it is ready (bake_on_ready)");
    const std::string* en = propOf(w, fresh, "enabled");
    check(en && *en == "false", "enabled = false is kept");
    check(fm.find("geometry_parsed_geometry_type") && fm.find("geometry_parsed_geometry_type")->asInt() == 1 && fm.find("agent_max_slope") &&
              near(fm.find("agent_max_slope")->asNumber(), 40.0) && !fm.find("bogus_option"),
          "its parameters are kept and an unknown one is dropped");
    check(classOf({"Main", "Fresh", "Floor", "Shape"}) == "CollisionShape3D", "the collision floor it would be baked from is converted");

    // ---- link, obstacles, agent --------------------------------------------------------------------------------------------------------
    AVER_INFO("-- link, obstacles and agent --");
    const int stair = find(w, {"Main", "Stair"});
    const std::string* sb = propOf(w, stair, "bidirectional");
    const std::string* sp = propOf(w, stair, "start_position");
    const std::string* ep = propOf(w, stair, "end_position");
    check(sb && *sb == "false" && sp && sp->find("Vector3(4, 0, 0)") != std::string::npos && ep && ep->find("Vector3(8, 3, 0)") != std::string::npos,
          "the link keeps bidirectional and its end points in Godot's space");
    check(numProp(w, stair, "navigation_layers") == 2 && near(numProp(w, stair, "enter_cost"), 5.0) && near(numProp(w, stair, "travel_cost"), 2.0), "and its layers and costs");

    const int crate = find(w, {"Main", "Crate"});
    const std::string* aff = propOf(w, crate, "affect_navigation_mesh");
    check(near(numProp(w, crate, "radius"), 0.5) && near(numProp(w, crate, "height"), 2.0) && aff && *aff == "true" && numProp(w, crate, "avoidance_layers") == 5,
          "a round obstacle keeps radius, height, affect_navigation_mesh and avoidance_layers");
    gd::GodotVariant wallVerts;
    check(parsed(propOf(w, find(w, {"Main", "Wall"}), "vertices"), wallVerts) && wallVerts.type == gd::GodotVType::PackedVector3Array && wallVerts.nums.size() == 9,
          "a wall obstacle keeps its vertices");

    const int agent = find(w, {"Main", "Walker", "NavigationAgent3D"});
    const std::string* tp = propOf(w, agent, "target_position");
    check(tp && tp->find("Vector3(4, 0, 4)") != std::string::npos, "the agent keeps target_position");
    check(near(numProp(w, agent, "path_desired_distance"), 0.5) && near(numProp(w, agent, "target_desired_distance"), 0.75) && near(numProp(w, agent, "path_max_distance"), 2.5),
          "path_desired_distance, target_desired_distance and path_max_distance");
    check(numProp(w, agent, "path_postprocessing") == 1 && numProp(w, agent, "path_metadata_flags") == 7 && numProp(w, agent, "navigation_layers") == 3, "post-processing, metadata flags and layers");
    const std::string* sim = propOf(w, agent, "simplify_path");
    check(sim && *sim == "true" && near(numProp(w, agent, "simplify_epsilon"), 0.1), "path simplification");
    const std::string* av = propOf(w, agent, "avoidance_enabled");
    check(av && *av == "true" && near(numProp(w, agent, "radius"), 0.4) && near(numProp(w, agent, "max_speed"), 5.0) && numProp(w, agent, "max_neighbors") == 6 &&
              near(numProp(w, agent, "neighbor_distance"), 8.0) && near(numProp(w, agent, "time_horizon_agents"), 1.5) && numProp(w, agent, "avoidance_mask") == 3,
          "the avoidance properties");

    // ---- the report -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the report --");
    {
        const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
        const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(has("\"type\":\"NavigationRegion3D\"", "\"rating\":\"converted\"", "\"property\":\"navigation_layers\""), "region navigation_layers is rated converted");
        check(has("\"type\":\"NavigationRegion3D\"", "\"rating\":\"converted\"", "\"property\":\"enter_cost\""), "region enter_cost is rated converted");
        check(has("\"type\":\"NavigationLink3D\"", "\"rating\":\"converted\"", "\"property\":\"start_position\""), "link start_position is rated converted");
        check(has("\"type\":\"NavigationObstacle3D\"", "\"rating\":\"converted\"", "\"property\":\"affect_navigation_mesh\""), "obstacle affect_navigation_mesh is rated converted");
        check(has("\"type\":\"NavigationObstacle3D\"", "\"rating\":\"approximated\"", "\"property\":\"use_3d_avoidance\""), "use_3d_avoidance is rated approximated, with its reason");
        check(has("\"type\":\"NavigationAgent3D\"", "\"rating\":\"converted\"", "\"property\":\"path_postprocessing\""), "agent path_postprocessing is rated converted");
        check(has("\"type\":\"NavigationAgent3D\"", "\"rating\":\"converted\"", "\"property\":\"avoidance_enabled\""), "agent avoidance_enabled is rated converted");
        check(!has("\"type\":\"NavigationAgent3D\"", "\"rating\":\"unsupported\"", "\"kind\":\"node\""), "the agent is no longer reported as having no class");
        check(!has("\"type\":\"NavigationRegion3D\"", "\"rating\":\"unsupported\"", "\"kind\":\"node\""), "nor the region");
        check(has("\"type\":\"NavigationMesh\"", "\"kind\":\"resource\"", "baked in the editor"), "a baked mesh is reported as such");
        check(has("\"type\":\"NavigationMesh\"", "\"rating\":\"approximated\"", "not baked in the editor"), "a never-baked mesh is reported as needing a run-time bake");
        check(has("\"type\":\"NavigationMesh\"", "\"rating\":\"approximated\"", "\"property\":\"sample_partition_type\""), "sample_partition_type is approximated (stored, not applied)");
        check(has("\"type\":\"NavigationMesh\"", "\"rating\":\"approximated\"", "\"property\":\"region_merge_size\""), "region_merge_size is approximated");
        check(has("\"type\":\"NavigationMesh\"", "\"rating\":\"approximated\"", "\"property\":\"detail_sample_distance\""), "detail_sample_distance is approximated");
        check(has("\"type\":\"NavigationMesh\"", "\"rating\":\"unsupported\"", "\"property\":\"bogus_option\""), "an unknown mesh property is unsupported");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
