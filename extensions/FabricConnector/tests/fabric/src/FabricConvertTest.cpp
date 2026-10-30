// FabricConvertTest -- `AverAssetC fabric` (tools/fabric) on the hand-written Godot project tests/fabric/data/ConvertGame.
//
// The converter is run as a process, and then every file it wrote is read back with the engine's own loaders: the .ocproject,
// .ocinput, .ocworld, .ocprefab, .ocmat and .ocmesh readers. What is checked is what the converter promises in
// docs/plans/fabric/G3_SCENES.md: transforms through the glTF axis map, one class placement per node with its Props, instanced
// scenes flattened with their overrides applied, Godot's physics defaults made explicit, the level environment records, and a
// report that rates what could not be carried over.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/AxisMap.hpp"
#include "aver/formats/OcInput.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcPrefab.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotVariant.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/LevelTransform.hpp"
#if defined(AVER_FABRIC_TEST_OCMAT)
#include "aver/formats/OcMat.hpp"
#endif

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

// As AverAssetCTest does it: cmd.exe strips the first and last quote, so the whole line gets one more pair.
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

// ---- level lookups ------------------------------------------------------------------------------------------------------------

// The placement reached by walking names from the top: find(w, {"Main", "Floor", "Mesh"}).
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

static const std::string* prop(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props")
            if (const std::string* v = c.find(key)) return v;
    return nullptr;
}

static bool propIs(const fmt::OcWorldData& w, int idx, const char* key, const std::string& want) {
    const std::string* v = prop(w, idx, key);
    return v && *v == want;
}

static gd::GodotVariant variantOf(const fmt::OcWorldData& w, int idx, const char* key) {
    gd::GodotVariant v;
    const std::string* t = prop(w, idx, key);
    gd::GodotDiagnostics d;
    if (t) gd::parseGodotVariant(*t, v, &d, "Props");
    return v;
}

static std::vector<std::string> childNames(const fmt::OcWorldData& w, int parent) {
    std::vector<std::string> out;
    for (const fmt::OcWorldPlacement& p : w.placements)
        if (p.parent == parent) out.push_back(p.name);
    return out;
}

// The rotation of a placement as a quaternion in engine space.
static Quat rotationOf(const fmt::OcWorldPlacement& p) { return world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch), static_cast<f32>(p.yaw)}); }

// Sign of how a mesh's triangles face against a reference direction: +1, -1, or 0 for a mix.
static int windingAgainst(const fmt::OcMeshData& m, const Vec3* reference) {
    int sign = 0;
    for (usize t = 0; t + 2 < m.indices.size(); t += 3) {
        const u32 a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        const Vec3 pa{m.positions[a * 3], m.positions[a * 3 + 1], m.positions[a * 3 + 2]};
        const Vec3 pb{m.positions[b * 3], m.positions[b * 3 + 1], m.positions[b * 3 + 2]};
        const Vec3 pc{m.positions[c * 3], m.positions[c * 3 + 1], m.positions[c * 3 + 2]};
        const Vec3 e1 = pb - pa, e2 = pc - pa;
        const Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        if (std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z) < 1e-6f) continue;
        Vec3 ref = reference ? *reference : Vec3{m.normals[a * 3] + m.normals[b * 3] + m.normals[c * 3], m.normals[a * 3 + 1] + m.normals[b * 3 + 1] + m.normals[c * 3 + 1],
                                                 m.normals[a * 3 + 2] + m.normals[b * 3 + 2] + m.normals[c * 3 + 2]};
        const f32 d = n.x * ref.x + n.y * ref.y + n.z * ref.z;
        const int s = d > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        else if (sign != s) return 0;
    }
    return sign;
}

int main() {
    AVER_INFO("FabricConvertTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/ConvertGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-convert-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();
    const std::string out2 = (outRoot / "two").string();

    // ---- the run --------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the converter runs and closes with its summary line --");
    std::string log;
    // --scenes-only: the script step has its own tests (FabricE2ETest, Aver.Fabric.Tests); this one is about scenes, assets and the project.
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "it exits 0 (got " + std::to_string(code) + ")");
    check(log.find("\"summary\":true") != std::string::npos && log.find("\"kind\":\"fabric\"") != std::string::npos &&
              log.find("\"exitCode\":0") != std::string::npos && log.find("\"levels\":2") != std::string::npos,
          "the final JSON line is the fabric summary with 2 levels");
    if (code != 0) AVER_INFO("{}", log);

    const std::string proj = out1 + "/ConvertGame";
    const std::string content = proj + "/Content";

    // ---- the project ------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- project.godot -> ConvertGame.ocproject --");
    fmt::ProjectDesc pd;
    std::string err;
    check(fmt::loadOcproject(proj + "/ConvertGame.ocproject", pd, &err), "the .ocproject loads: " + err);
    check(pd.name == "ConvertGame", "NAME is the Godot project name without its space");
    check(pd.startMap == "Maps/main.ocworld", "STARTMAP is the main scene's level");
    check(pd.windowWidth == 1600 && pd.windowHeight == 900, "WINDOW.SIZE is the viewport size");
    check(pd.hasGravity && near(pd.gravity[0], 0) && near(pd.gravity[1], 0) && near(pd.gravity[2], -981, 1e-2),
          "PHYSICS.GRAVITY is 9.81 m/s^2 down, in centimetres on Aver's +Z up axis");
    check(near(pd.fixedStep, 1.0 / 120.0, 1e-6), "PHYSICS.FIXEDSTEP is 1 / physics_ticks_per_second");
    check(pd.physCombine == 1, "PHYSICS.COMBINE material");
    check(pd.physInterpolation == 1, "PHYSICS.INTERPOLATION follows physics/common/physics_interpolation");
    check(pd.localeDefault == "en", "LOCALE.DEFAULT is the fallback locale");
    check(pd.inputScheme == "Input/ConvertGame.ocinput", "INPUT.SCHEME names the converted map");
    check(near(pd.postBloom, 0.6, 1e-5) && near(pd.postExposure, 1.25, 1e-5), "the start scene's glow and tonemap exposure became RENDER.BLOOM / RENDER.EXPOSURE");
    const std::string projText = slurp(proj + "/ConvertGame.ocproject");
    check(projText.find("FABRIC.LAYER 3d_physics 2 player") != std::string::npos, "layer names are kept as FABRIC.LAYER comments");
    check(fileExists(content + "/Localization/fr/fr.po"), "the .po translation was copied under its culture");

    // ---- the input map ----------------------------------------------------------------------------------------------------------
    AVER_INFO("-- [input] -> ConvertGame.ocinput --");
    fmt::OcInputData in;
    check(fmt::loadOcinput(content + "/Input/ConvertGame.ocinput", in, &err), "the .ocinput loads: " + err);
    check(in.actions.size() == 5, "five actions");
    const auto bindingsOf = [&](const char* action) {
        std::vector<const fmt::OcInputBinding*> v;
        for (const fmt::OcInputBinding& b : in.bindings)
            if (b.action == action) v.push_back(&b);
        return v;
    };
    {
        const auto fwd = bindingsOf("move_forward");
        check(fwd.size() == 1 && fwd[0]->source == fmt::OcInputSource::Key && fwd[0]->key == "W", "move_forward: physical key 87 is W");
        const auto jump = bindingsOf("jump");
        check(jump.size() == 2 && jump[0]->key == "Space" && jump[1]->source == fmt::OcInputSource::GamepadButton && jump[1]->key == "A",
              "jump: Space and gamepad button 0 (A)");
        const auto fire = bindingsOf("fire");
        check(fire.size() == 1 && fire[0]->key == "MouseLeft", "fire: mouse button 1 is MouseLeft");
        const auto left = bindingsOf("move_left");
        check(left.size() == 2 && left[0]->source == fmt::OcInputSource::GamepadAxis && left[0]->key == "LeftX" && near(left[0]->scale, -1.0),
              "move_left: joypad axis 0 at -1 is LeftX with scale -1");
        check(bindingsOf("debug_menu").empty(), "debug_menu: F5 is not in Aver's key set, so nothing is bound");
        for (const fmt::OcInputAction& a : in.actions)
            if (a.name == "move_left")
                check(a.type == fmt::OcInputValueType::Axis1D && a.modifiers.size() == 1 && a.modifiers[0].rfind("deadzone", 0) == 0,
                      "an action with a stick axis is axis1 with the action's deadzone as a modifier");
    }

    // ---- the main level ---------------------------------------------------------------------------------------------------------
    AVER_INFO("-- main.tscn -> Maps/main.ocworld --");
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);
    check(w.placements.size() == 41, "one placement per node, instances flattened: 41 (got " + std::to_string(w.placements.size()) + ")");
    int roots = 0;
    for (const fmt::OcWorldPlacement& p : w.placements) roots += p.parent < 0;
    check(roots == 2, "two top-level nodes: the autoload and the scene root");
    check(!w.placements.empty() && w.placements[0].name == "Global" && w.placements[0].className == "Global" && w.placements[0].asset == "none",
          "the autoload comes first, a class placement of its script's class");
    check(propIs(w, 0, "_persistent", "1") && propIs(w, 0, "_name", "\"Global\""), "autoloads are persistent and named");
    check(find(w, {"Helper"}) < 0, "an autoload without the * flag is not placed");

    const int main = find(w, {"Main"});
    check(main >= 0 && w.placements[static_cast<usize>(main)].className == "Node3D", "Main is the scene root, a Node3D");
    check(prop(w, main, "_owner") == nullptr, "the scene root has no _owner");

    // Transforms: the glTF axis map, metres to centimetres.
    {
        const int crate = find(w, {"Main", "Crate"});
        check(crate >= 0, "Crate (an instanced scene) is there");
        const fmt::OcWorldPlacement& c = w.placements[static_cast<usize>(crate)];
        check(near(c.x, 300) && near(c.y, 400) && near(c.z, 50), "position (4, 0.5, -3) m is (300, 400, 50) cm: (x,y,z) -> (-z,x,y) * 100");
        // A 90 degree turn about Godot's +Y is a quaternion (0, s, 0, s); the map makes it (0, 0, -s, s), a yaw of -90.
        const Quat q = rotationOf(c);
        check(near(q.x, 0, 1e-4) && near(q.y, 0, 1e-4) && near(q.z, -0.70710678, 1e-4) && near(q.w, 0.70710678, 1e-4), "its rotation went through (x,y,z,w) -> (z,-x,-y,w)");
        check(c.className == "RigidBody3D" && c.parent == main, "its class is the instanced scene's root class, under Main");
        check(propIs(w, crate, "scene_file_path", "\"res://props/crate.tscn\""), "the instance root carries scene_file_path");
        check(propIs(w, crate, "_owner", "\"..\""), "an instance root is owned by the outer scene root");

        const int scaled = find(w, {"Main", "Scaled"});
        const fmt::OcWorldPlacement& s = w.placements[static_cast<usize>(scaled)];
        check(near(s.x, -300) && near(s.y, 100) && near(s.z, 200), "position (1, 2, 3) m is (-300, 100, 200) cm");
        check(near(s.sx, 4) && near(s.sy, 2) && near(s.sz, 3) && !s.uniform(), "scale (2, 3, 4) is (z, x, y) = (4, 2, 3), a PLACEG");

        // A node written with position / rotation / scale instead of a transform (rotation in radians, order YXZ).
        const int spin = find(w, {"Main", "Spin"});
        const fmt::OcWorldPlacement& sp = w.placements[static_cast<usize>(spin)];
        const Quat sq = rotationOf(sp);
        check(near(sp.x, 0) && near(sp.y, 100) && near(sp.z, 0) && sp.uniform() && near(sp.sx, 2), "position / rotation / scale properties: (1, 0, 0) m, uniform scale 2");
        check(near(sq.z, -0.70710678, 1e-4) && near(sq.w, 0.70710678, 1e-4), "rotation (0, pi/2, 0) is the same yaw of -90 as the matrix above");

        const int way = find(w, {"Main", "Waypoint"});
        check(way >= 0 && near(w.placements[static_cast<usize>(way)].x, -1000) && near(w.placements[static_cast<usize>(way)].y, 1000), "a Marker3D at (10, 0, 10) m is (-1000, 1000, 0)");
    }

    // The instanced scene with its overrides.
    {
        const int crate = find(w, {"Main", "Crate"});
        const std::vector<std::string> kids = childNames(w, crate);
        check(kids.size() == 3 && kids[0] == "Body" && kids[1] == "Lid" && kids[2] == "Shape", "the crate's children, in the sub-scene's order");
        check(propIs(w, find(w, {"Main", "Crate", "Lid"}), "visible", "false"), "the override of an instanced node's property (Lid.visible) is applied");
        const int extra = find(w, {"Main", "Crate", "Body", "Extra"});
        check(extra >= 0, "a node added inside the instance (Crate/Body/Extra) is placed under it");
        check(extra >= 0 && propIs(w, extra, "_unique", "1") && propIs(w, extra, "_owner", "\"../../..\"") && near(w.placements[static_cast<usize>(extra)].z, 100),
              "and is unique in its owner, owned by the outer scene, at 1 m up");
        check(propIs(w, find(w, {"Main", "Crate", "Body"}), "_owner", "\"..\""), "nodes of the sub-scene are owned by the instance root");
        check(propIs(w, crate, "mass", "5.0") && propIs(w, crate, "linear_damp", "0.0"), "the crate's own mass and its Replace-mode damping");
        check(!prop(w, crate, "linear_damp_mode"), "the damping mode is folded into the value");
    }

    // Physics defaults.
    {
        const int floorIdx = find(w, {"Main", "Floor"});
        check(floorIdx >= 0 && w.placements[static_cast<usize>(floorIdx)].className == "StaticBody3D" && near(w.placements[static_cast<usize>(floorIdx)].z, -50),
              "Floor: a StaticBody3D half a metre down");
        check(propIs(w, floorIdx, "collision_layer", "3") && propIs(w, floorIdx, "collision_mask", "1"), "layer 3 as written, mask 1 made explicit");
        const gd::GodotVariant pm = variantOf(w, floorIdx, "physics_material_override");
        check(pm.type == gd::GodotVType::Dictionary && pm.find("friction") && near(pm.find("friction")->asNumber(), 0.1) && pm.find("bounce") &&
                  near(pm.find("bounce")->asNumber(), 0.4) && pm.find("rough") && pm.find("rough")->asBool(),
              "the PhysicsMaterial's friction, bounce and rough are kept");
        const gd::GodotVariant box = variantOf(w, find(w, {"Main", "Floor", "Shape"}), "shape");
        check(box.type == gd::GodotVType::Object && box.s == "BoxShape3D" && box.find("size") && box.find("size")->type == gd::GodotVType::Vector3 &&
                  near(box.find("size")->num(0), 20) && near(box.find("size")->num(1), 1),
              "a BoxShape3D becomes an Object(BoxShape3D, size) value in metres");

        const int ball = find(w, {"Main", "Ball"});
        check(propIs(w, ball, "mass", "2.0") && propIs(w, ball, "gravity_scale", "1.0"), "Ball: mass as written, gravity_scale 1 made explicit");
        const std::string* ld = prop(w, ball, "linear_damp");
        const std::string* ad = prop(w, ball, "angular_damp");
        check(ld && near(std::atof(ld->c_str()), 0.75) && ad && near(std::atof(ad->c_str()), 0.1),
              "damping is the body's own plus the project default (0.5 + 0.25, 0 + 0.1), as Godot's Combine mode does");
        check(propIs(w, ball, "collision_layer", "4") && propIs(w, ball, "collision_mask", "5"), "layer and mask as written");
        check(propIs(w, ball, "contact_monitor", "true") && propIs(w, ball, "max_contacts_reported", "4"), "contact monitor and max_contacts_reported");
        check(prop(w, ball, "axis_lock_angular_y") != nullptr, "an axis lock is kept as a property");

        const int zone = find(w, {"Main", "Zone"});
        check(zone >= 0 && w.placements[static_cast<usize>(zone)].className == "Area3D" && propIs(w, zone, "monitorable", "false") &&
                  propIs(w, zone, "collision_layer", "1"),
              "Zone: an Area3D with monitorable false and the default layer made explicit");
        const gd::GodotVariant cyl = variantOf(w, find(w, {"Main", "Zone", "Shape"}), "shape");
        check(cyl.s == "CylinderShape3D" && cyl.find("radius") && near(cyl.find("radius")->asNumber(), 2) && cyl.find("height") && near(cyl.find("height")->asNumber(), 4),
              "a CylinderShape3D keeps radius and height");

        const gd::GodotVariant hull = variantOf(w, find(w, {"Main", "Trigger", "Hull"}), "shape");
        check(hull.s == "ConvexPolygonShape3D" && hull.find("points") && hull.find("points")->nums.size() == 12, "a ConvexPolygonShape3D keeps its points");
        const gd::GodotVariant bound = variantOf(w, find(w, {"Main", "Trigger", "Limit"}), "shape");
        check(bound.s == "WorldBoundaryShape3D", "a WorldBoundaryShape3D");
        const gd::GodotVariant hills = variantOf(w, find(w, {"Main", "Trigger", "Hills"}), "shape");
        check(hills.s == "ConcavePolygonShape3D" && hills.find("data") && hills.find("data")->nums.size() == 4 * 2 * 3 * 3,
              "a 3 x 3 HeightMapShape3D becomes 8 triangles");
        const gd::GodotVariant poly = variantOf(w, find(w, {"Main", "Trigger", "Poly"}), "shape");
        check(find(w, {"Main", "Trigger", "Poly"}) >= 0 && w.placements[static_cast<usize>(find(w, {"Main", "Trigger", "Poly"}))].className == "CollisionShape3D" &&
                  poly.s == "ConcavePolygonShape3D" && poly.find("data") && poly.find("data")->nums.size() > 0,
              "a concave CollisionPolygon3D is extruded into a CollisionShape3D with a triangle shape");
    }

    // Script, groups, node-typed exports and connections.
    {
        const int player = find(w, {"Main", "Player"});
        check(player >= 0 && w.placements[static_cast<usize>(player)].className == "Player", "a scripted node is a placement of the script's class");
        check(propIs(w, player, "speed", "6.5") && propIs(w, player, "label", "\"Hero\""), "script exports are Props");
        check(propIs(w, player, "target", "NodePath(\"../Crate\")"), "a node-typed export keeps its path relative to the node");
        bool groups = false;
        for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(player)].components)
            if (c.type == "Groups") groups = c.find("names") && *c.find("names") == "player,actors";
        check(groups, "groups become the Groups component");
        const int trigger = find(w, {"Main", "Trigger"});
        const gd::GodotVariant conns = variantOf(w, trigger, "_connections");
        check(conns.type == gd::GodotVType::Array && conns.items.size() == 1 && conns.items[0].type == gd::GodotVType::Dictionary &&
                  conns.items[0].find("signal") && conns.items[0].find("signal")->asString() == "body_entered" &&
                  conns.items[0].find("to") && conns.items[0].find("to")->asString() == "../Player" &&
                  conns.items[0].find("method") && conns.items[0].find("method")->asString() == "_on_trigger_body_entered" &&
                  conns.items[0].find("flags") && conns.items[0].find("flags")->asInt() == 3 && conns.items[0].find("binds") &&
                  conns.items[0].find("binds")->items.size() == 2,
              "a [connection] rides on its source as _connections, with the target relative to it, flags and binds");
        const gd::GodotVariant crateConn = variantOf(w, find(w, {"Main", "Crate"}), "_connections");
        check(crateConn.items.size() == 1 && crateConn.items[0].find("to") && crateConn.items[0].find("to")->asString() == "..", "a connection to the root is '..' from a child");
    }

    // Lights, camera, environment.
    {
        const int sun = find(w, {"Main", "Sun"});
        check(sun >= 0 && w.placements[static_cast<usize>(sun)].className == "DirectionalLight3D" && near(w.placements[static_cast<usize>(sun)].z, 1000), "Sun: a DirectionalLight3D 10 m up");
        check(propIs(w, sun, "light_energy", "1.5") && propIs(w, sun, "shadow_enabled", "true"), "its energy and shadows are Props");
        check(w.hasSun && near(w.sunLux, 1.5 * 104719.755, 1) && near(w.sunDir[0], -0.70710678, 1e-4) && near(w.sunDir[1], 0, 1e-4) && near(w.sunDir[2], 0.70710678, 1e-4),
              "the SUN record points at the sun (the light's -Z reversed, through the axis map) with energy 1.5 as 1.5 * 104719.755 lux (pi engine units per unit of energy)");
        check(w.sunColor[0] > 0.99 && w.sunColor[1] < 0.95 && w.sunColor[2] < 0.9, "the sun colour is linear");
        check(find(w, {"Main", "Lamp"}) >= 0 && propIs(w, find(w, {"Main", "Lamp"}), "omni_range", "8.0"), "OmniLight3D: omni_range in metres");
        check(find(w, {"Main", "Spot"}) >= 0 && propIs(w, find(w, {"Main", "Spot"}), "spot_angle", "30.0"), "SpotLight3D: spot_angle");
        const int cam = find(w, {"Main", "Camera"});
        check(cam >= 0 && propIs(w, cam, "fov", "60.0") && propIs(w, cam, "near", "0.1") && propIs(w, cam, "far", "500.0") && propIs(w, cam, "current", "true"),
              "Camera3D: fov, near, far and current");
        check(w.hasCamera && near(w.camX, -500) && near(w.camY, 0) && near(w.camZ, 200) && near(w.camYaw, 0, 1e-2), "the level opens at the current camera, looking down Aver's +X");
        check(w.hasSky && !w.skyPhysical && near(w.skyZenith[0], 0.2) && near(w.skyHorizon[1], 0.8) && near(w.skyLight, 0.5),
              "ProceduralSkyMaterial becomes the authored dome, the ambient energy the sky light");
        check(w.hasFog && near(w.fogDensity, 0.0002, 1e-7) && near(w.fogColor[2], 0.7), "fog density is per metre / 100 per centimetre");
        const int env = find(w, {"Main", "World"});
        check(env >= 0 && w.placements[static_cast<usize>(env)].className == "WorldEnvironment", "the WorldEnvironment node is placed as the WorldEnvironment class (G4 R3)");
    }

    // Stand-ins for what the runtime does not have.
    {
        const int hud = find(w, {"Main", "Hud"});
        check(hud >= 0 && w.placements[static_cast<usize>(hud)].className == "Label" && propIs(w, hud, "text", "\"Hello\""), "a Label is placed as Aver.Fabric's Label (G5 U2: no longer a Control placeholder)");
        const int icon = find(w, {"Main", "Icon"});
        check(icon >= 0 && w.placements[static_cast<usize>(icon)].className == "Node" && propIs(w, icon, "_class", "\"Sprite2D\""), "a 2D node is kept as a plain node");
        const int hum = find(w, {"Main", "Hum"});
        check(hum >= 0 && w.placements[static_cast<usize>(hum)].className == "Node3D" && propIs(w, hum, "_class", "\"XROrigin3D\""), "an unsupported 3D class is a Node3D carrier");
        const int route = find(w, {"Main", "Route"});
        const gd::GodotVariant curve = variantOf(w, route, "curve");
        check(route >= 0 && w.placements[static_cast<usize>(route)].className == "Path3D" && curve.type == gd::GodotVType::Object && curve.s == "Curve3D" && curve.find("_data"),
              "a Path3D is a Path3D (G5 A2) whose Curve3D is an Object(Curve3D, ...) value");
        const int hinge = find(w, {"Main", "Hinge"});
        check(hinge >= 0 && propIs(w, hinge, "_class", "\"HingeJoint3D\"") && propIs(w, hinge, "node_a", "NodePath(\"../Ball\")"), "a joint keeps its parameters as properties");
    }

    // Meshes.
    AVER_INFO("-- meshes: primitives, imported model, winding --");
    {
        const auto meshPath = [&](std::initializer_list<const char*> path) {
            const std::string* v = prop(w, find(w, path), "mesh");
            if (!v) return std::string();
            gd::GodotVariant var;
            gd::GodotDiagnostics d;
            gd::parseGodotVariant(*v, var, &d, "mesh");
            return var.type == gd::GodotVType::Resource ? var.s : std::string();
        };
        fmt::OcMeshData floorMesh, sphere, capsule, tri;
        const std::string fp = meshPath({"Main", "Floor", "Mesh"});
        check(!fp.empty() && fmt::loadOcMesh(content + "/" + fp, floorMesh, &err), "the BoxMesh is a generated .ocmesh that loads: " + fp + " " + err);
        floorMesh.computeBounds();
        check(near(floorMesh.boundsMax.x - floorMesh.boundsMin.x, 2000, 0.5) && near(floorMesh.boundsMax.y - floorMesh.boundsMin.y, 2000, 0.5) &&
                  near(floorMesh.boundsMax.z - floorMesh.boundsMin.z, 100, 0.5),
              "20 x 1 x 20 m is 2000 x 2000 x 100 cm");
        check(floorMesh.vertexCount() == 24 && floorMesh.indices.size() == 36, "a plain box has 24 vertices and 12 triangles");
        const std::string sp = meshPath({"Main", "Ball", "Mesh"});
        check(!sp.empty() && fmt::loadOcMesh(content + "/" + sp, sphere, &err), "the SphereMesh loads: " + err);
        sphere.computeBounds();
        check(near(sphere.boundsMax.z, 50, 0.5) && near(sphere.boundsMin.z, -50, 0.5) && near(sphere.boundsMax.x, 50, 0.5), "a 0.5 m sphere is 50 cm in radius");
#if defined(AVER_FABRIC_TEST_OCMAT)
        check(sphere.materialSlots.size() == 1 && sphere.materialSlots[0] == "ConvertGame_red", "its material slot names the cooked material");
#endif
        const std::string cp = meshPath({"Main", "Scaled"});
        check(!cp.empty() && fmt::loadOcMesh(content + "/" + cp, capsule, &err), "the CapsuleMesh loads: " + err);
        capsule.computeBounds();
        check(near(capsule.boundsMax.z, 90, 0.5) && near(capsule.boundsMin.z, -90, 0.5), "a 1.8 m capsule is 180 cm tall");
        for (const char* name : {"Pillar", "Pad", "Wedge", "Ring", "Banner"}) {
            fmt::OcMeshData m;
            const std::string p = meshPath({"Main", name});
            check(!p.empty() && fmt::loadOcMesh(content + "/" + p, m, &err) && m.valid() && m.vertexCount() > 3, std::string("the ") + name + " mesh loads and is valid");
        }

        // The imported model: one triangle at x = 0.5 m inside its glTF node.
        const std::string tp = meshPath({"Main", "Player", "Model", "Tri"});
        check(!tp.empty() && fmt::loadOcMesh(content + "/" + tp, tri, &err), "the model file's mesh was imported: " + err);
        const int triIdx = find(w, {"Main", "Player", "Model", "Tri"});
        check(triIdx >= 0 && near(w.placements[static_cast<usize>(triIdx)].y, 50) && near(w.placements[static_cast<usize>(triIdx)].x, 0), "its node translation (0.5, 0, 0) m is (0, 50, 0) cm");
        // The importer reverses glTF's counter-clockwise winding; the generated meshes must face the same way against their normals.
        const Vec3 gltfFront = fmt::axis::direction(0, 0, 1);   // the triangle's glTF normal (+Z), in engine space
        const int refSign = windingAgainst(tri, &gltfFront);
        check(refSign != 0, "the imported triangle has a consistent facing");
        check(windingAgainst(floorMesh, nullptr) == refSign, "a generated box faces its normals the way the importer's meshes do");
        check(windingAgainst(sphere, nullptr) == refSign, "so does a generated sphere");
        check(windingAgainst(capsule, nullptr) == refSign, "and a capsule");
    }

    // Materials.
#if defined(AVER_FABRIC_TEST_OCMAT)
    AVER_INFO("-- materials --");
    {
        pbr::MaterialDesc red, floorMat;
        fmt::OcMatExtras ex;
        check(fmt::loadOcmat(content + "/Materials/ConvertGame_red.ocmat", red, &ex, &err), "the .tres material loads as an .ocmat: " + err);
        check(near(red.baseColorFactor[0], 1) && near(red.baseColorFactor[1], 0) && near(red.roughnessFactor, 0.4) && near(red.metallicFactor, 0),
              "albedo colour, roughness 0.4 and Godot's metallic default of 0");
        const std::string* mo = prop(w, find(w, {"Main", "Floor", "Mesh"}), "material_override");
        check(mo != nullptr, "the surface override on the floor is its material_override");
        std::string matFile;
        if (mo) {
            gd::GodotVariant var;
            gd::GodotDiagnostics d;
            gd::parseGodotVariant(*mo, var, &d, "material_override");
            matFile = var.s;
        }
        check(!matFile.empty() && fmt::loadOcmat(content + "/" + matFile, floorMat, &ex, &err), "the sub-resource material loads: " + err);
        check(near(floorMat.metallicFactor, 0.25) && near(floorMat.roughnessFactor, 0.6), "metallic and roughness");
        check(floorMat.alphaMode == pbr::AlphaMode::Mask && near(floorMat.alphaCutoff, 0.4) && floorMat.twoSided, "alpha scissor with its threshold, and cull mode disabled as two-sided");
        check(near(floorMat.emissiveFactor[0], 2) && floorMat.emissiveFactor[1] > 0.3 && floorMat.emissiveFactor[1] < 0.5 && near(floorMat.emissiveFactor[2], 0),
              "emission colour (linear) times the energy multiplier");
        const std::string tex = floorMat.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].path;
        check(tex == "Textures/ConvertGame/albedo.png" && fileExists(content + "/" + tex), "the albedo texture was copied through the material cook and is referenced");
    }
#endif

    // The prefab.
    AVER_INFO("-- instanced scenes --");
    {
        fmt::OcWorldData crateLevel;
        check(fmt::loadOcworld(content + "/Maps/props/crate.ocworld", crateLevel, &err), "the sub-scene is a level of its own: " + err);
        check(crateLevel.placements.size() == 4 && crateLevel.placements[0].name == "Crate" && crateLevel.placements[0].className == "RigidBody3D",
              "crate.tscn: four nodes under a RigidBody3D");
        fmt::OcPrefabData prefab;
        check(fmt::loadOcPrefab(content + "/Prefabs/props/crate.ocprefab", prefab, &err), "the instanced scene was also written as a prefab: " + err);
        check(prefab.valid() && prefab.nodes.size() == 4 && prefab.nodes[0].name == "Crate" && prefab.nodes[0].className == "RigidBody3D" && prefab.nodes[1].parent == prefab.nodes[0].uid,
              "a valid prefab: the same nodes, names and classes");
        bool mesh = false;
        for (const fmt::OcSaveComponent& c : prefab.nodes[1].components) mesh = mesh || c.type == "CMeshRenderer";
        check(mesh, "the Body node carries its mesh");
    }

    // The manifests and the report.
    AVER_INFO("-- manifests and the report --");
    {
        const std::string scripts = slurp(proj + "/Fabric/scripts.json");
        check(scripts.find("\"path\": \"res://scripts/player.gd\"") != std::string::npos && scripts.find("\"className\": \"Player\"") != std::string::npos &&
                  scripts.find("\"extends\": \"CharacterBody3D\"") != std::string::npos && scripts.find("\"declaredClassName\": true") != std::string::npos,
              "scripts.json maps player.gd to class Player, extending CharacterBody3D");
        check(scripts.find("\"className\": \"Global\"") != std::string::npos && scripts.find("\"autoload\": true") != std::string::npos, "and marks the autoload script");
        check(scripts.find("res://main.tscn:Player") != std::string::npos, "and lists the node that uses a script");
        const std::string pj = slurp(proj + "/Fabric/project.json");
        check(pj.find("\"startLevel\": \"Maps/main.ocworld\"") != std::string::npos && pj.find("\"tonemapMode\": 3") != std::string::npos,
              "project.json records the start level and the tonemap mode");

        const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
        check(!jsonl.empty(), "FabricReport.jsonl was written");
        bool shape = true;
        usize lines = 0;
        {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line)) {
                ++lines;
                for (const char* key : {"\"schema\":1", "\"kind\":", "\"file\":", "\"node\":", "\"type\":", "\"property\":", "\"rating\":", "\"reason\":", "\"line\":"})
                    if (line.find(key) == std::string::npos) shape = false;
            }
        }
        check(shape && lines > 80, "every report line has the whole schema (" + std::to_string(lines) + " lines)");
        const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(has("\"type\":\"Sprite2D\"", "\"rating\":\"unsupported\"", "2D nodes are out of scope"), "a 2D node is reported unsupported");
        check(has("\"type\":\"HingeJoint3D\"", "\"rating\":\"unsupported\"", "\"kind\":\"node\""), "a joint is reported unsupported");
        check(has("\"type\":\"Label\"", "\"property\":\"text\"", "\"rating\":\"converted\""), "a UI control's text is reported converted (G5 U2: Aver.Fabric's Label reads it)");
        check(has("\"type\":\"StandardMaterial3D\"", "\"property\":\"uv1_scale\"", "\"rating\":\"converted\""), "uv1_scale is converted now (the .ocmat FX record, G4 R6)");
        check(has("\"type\":\"ShaderMaterial\"", "\"rating\":\"unsupported\"", "has no shader"), "a ShaderMaterial without a shader is reported unsupported (G6 SH2: a shader is converted, an empty one has nothing to run)");
        check(has("\"property\":\"ambient_light_sky_contribution\"", "\"type\":\"Environment\"", "\"rating\":\"unsupported\""), "an Environment setting with no record is reported");
        check(has("\"property\":\"ssao_enabled\"", "\"type\":\"Environment\"", "\"rating\":\"approximated\""), "SSAO is approximated by Aver's GI occlusion (G4 R8)");
        check(has("\"kind\":\"input\"", "debug_menu", "\"rating\":\"unsupported\""), "an unmapped key is reported");
        check(has("\"kind\":\"node\"", "\"type\":\"RigidBody3D\"", "\"rating\":\"converted\""), "a converted node is rated converted");
        check(has("\"kind\":\"script\"", "res://scripts/player.gd", "\"rating\":\"unsupported\""), "a script is rated unsupported until its conversion package lands");
        const std::string md = slurp(out1 + "/FabricReport.md");
        check(md.find("## Unsupported") != std::string::npos && md.find("## Approximated") != std::string::npos && md.find("## Converted") != std::string::npos &&
                  md.find("| Kind | Converted | Approximated | Unsupported |") != std::string::npos,
              "FabricReport.md has the summary table and the three sections");
    }

    // The same input again must give the same files.
    AVER_INFO("-- determinism --");
    {
        std::string log2;
        const int code2 = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out2 + "\" --scenes-only", log2);
        check(code2 == 0, "a second run exits 0");
        check(slurp(out1 + "/ConvertGame/Content/Maps/main.ocworld") == slurp(out2 + "/ConvertGame/Content/Maps/main.ocworld"), "main.ocworld is byte for byte the same");
        check(slurp(out1 + "/ConvertGame/Fabric/scripts.json") == slurp(out2 + "/ConvertGame/Fabric/scripts.json"), "and so is scripts.json");
        check(slurp(out1 + "/FabricReport.jsonl") == slurp(out2 + "/FabricReport.jsonl"), "and the report");
    }

    // Fabric/nodes.json: what each scene's nodes became, for the script converter's typed $Path / get_node / %Unique / instantiate; and a CanvasLayer
    // is a UI node of Aver.Fabric, not one of the 2D nodes that are out of scope.
    AVER_INFO("-- nodes.json and CanvasLayer --");
    {
        const std::string out5 = (outRoot / "scriptgame").string();
        std::string log5;
        const int code5 = runCapture("\"" + exe.string() + "\" fabric \"" + std::string(AVER_FABRIC_DATA) + "/ScriptGame\" \"" + out5 + "\" --scenes-only", log5);
        check(code5 == 0, "ScriptGame converts (exit " + std::to_string(code5) + ")");
        const std::string nodes = slurp(out5 + "/ScriptGame/Fabric/nodes.json");
        const auto hasNode = [&](const std::string& text) { return nodes.find(text) != std::string::npos; };
        check(hasNode("{\"scene\": \"res://main.tscn\", \"nodes\": ["), "nodes.json lists main.tscn");
        check(hasNode("{\"path\": \".\", \"base\": \"Node3D\", \"class\": \"Main\", \"script\": \"res://main.gd\", \"owner\": \"\", \"unique\": false, \"packed\": {\"mob_scene\": \"res://mob.tscn\"}}"),
              "the root carries its script and the scene its mob_scene property is set to");
        check(hasNode("{\"path\": \"Title\", \"base\": \"Label3D\", \"class\": \"Label3D\", \"script\": \"\", \"owner\": \".\", \"unique\": true, \"packed\": {}}"),
              "a %Unique node is flagged and owned by the root");
        check(hasNode("{\"path\": \"Hud\", \"base\": \"CanvasLayer\", \"class\": \"CanvasLayer\""), "the CanvasLayer is placed as a CanvasLayer");
        check(hasNode("{\"path\": \"Hud/Panel\", \"base\": \"Control\""), "and its Control below it");
        check(hasNode("{\"path\": \"Mob1\", \"base\": \"CharacterBody3D\", \"class\": \"Mob\", \"script\": \"res://mob.gd\", \"owner\": \".\""),
              "an instanced scene's root is a node of the scene that instances it, with its script's class");
        check(hasNode("{\"path\": \"Mob1/Tag\", \"base\": \"Label3D\", \"class\": \"Label3D\", \"script\": \"\", \"owner\": \"Mob1\", \"unique\": true"),
              "its children are owned by the instance root, where their %Unique names are looked up");
        check(hasNode("{\"scene\": \"res://mob.tscn\", \"nodes\": [\n      {\"path\": \".\", \"base\": \"CharacterBody3D\", \"class\": \"Mob\""), "the instanced scene is listed on its own too");
        const std::string report5 = slurp(out5 + "/FabricReport.jsonl");
        const auto reported = [&](const std::string& a, const std::string& b) {
            std::istringstream is(report5);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) return true;
            return false;
        };
        check(reported("\"type\":\"CanvasLayer\"", "\"rating\":\"approximated\"") && !reported("\"type\":\"CanvasLayer\"", "2D nodes are out of scope"),
              "CanvasLayer is rated approximated (layer order, offset, rotation and scale are kept, not applied), not out of scope");
        std::string log6;
        runCapture("\"" + exe.string() + "\" fabric \"" + std::string(AVER_FABRIC_DATA) + "/ScriptGame\" \"" + (outRoot / "scriptgame2").string() + "\" --scenes-only", log6);
        check(slurp((outRoot / "scriptgame2").string() + "/ScriptGame/Fabric/nodes.json") == nodes, "nodes.json is the same on a second run");
    }

    // Failure modes: nothing to convert, binary scenes.
    AVER_INFO("-- failures are reported, not crashed on --");
    {
        std::string log3;
        const int code3 = runCapture("\"" + exe.string() + "\" fabric \"" + (outRoot / "nothing-here").string() + "\" \"" + (outRoot / "x").string() + "\"", log3);
        check(code3 != 0, "a folder with no project.godot fails (exit " + std::to_string(code3) + ")");
        check(runCapture("\"" + exe.string() + "\" fabric", log3) != 0, "missing arguments are a usage error");

        const std::string mini = std::string(AVER_FABRIC_DATA) + "/MiniGame";
        const std::string out4 = (outRoot / "mini").string();
        std::string log4;
        runCapture("\"" + exe.string() + "\" fabric \"" + mini + "\" \"" + out4 + "\" --scenes-only", log4);
        const std::string report4 = slurp(out4 + "/FabricReport.jsonl");
        check(report4.find("level.scn") != std::string::npos && report4.find("resave it as text") != std::string::npos,
              "a binary .scn is reported as 'resave it as text'");
        check(fileExists(out4 + "/MiniGame/Content/Maps/main.ocworld"), "the rest of that project still converted");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);   // kept on failure, for looking at
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
