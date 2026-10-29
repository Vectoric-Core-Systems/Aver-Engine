// FabricTreeTest -- G5 A2 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/TreeGame, then the level it wrote read back. The
// AnimationTree must be placed as an AnimationTree whose tree_root is an Object(...) value holding the whole graph (state machine, transitions, a blend
// tree, nested animation nodes, curves), the skeleton / path / helper nodes must be placed as their own classes with their properties, a modifier the
// runtime lacks must be a rated carrier, and every node and property must be rated in the report.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
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

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

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

static const gd::GodotVariant* key(const gd::GodotVariant& v, const char* k) { return v.find(k); }

static bool isObject(const gd::GodotVariant* v, const char* cls) { return v && v->type == gd::GodotVType::Object && v->s == cls; }

int main() {
    AVER_INFO("FabricTreeTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/TreeGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-tree-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/TreeGame/Content";
    std::string err;
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);

    // ---- the classes -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- classes --");
    const auto classOf = [&](std::initializer_list<const char*> path) {
        const int i = find(w, path);
        return i < 0 ? std::string("<missing>") : w.placements[static_cast<usize>(i)].className;
    };
    check(classOf({"Main", "AnimationTree"}) == "AnimationTree", "AnimationTree is placed as an AnimationTree");
    check(classOf({"Main", "Skeleton3D"}) == "Skeleton3D", "Skeleton3D is placed as a Skeleton3D");
    check(classOf({"Main", "Skeleton3D", "Grip"}) == "BoneAttachment3D", "a BoneAttachment3D below the skeleton");
    check(classOf({"Main", "Skeleton3D", "Reach"}) == "SkeletonIK3D", "a SkeletonIK3D below the skeleton");
    check(classOf({"Main", "Track"}) == "Path3D" && classOf({"Main", "Track", "Rider"}) == "PathFollow3D", "Path3D with a PathFollow3D below it");
    check(classOf({"Main", "Boom"}) == "SpringArm3D", "SpringArm3D");
    check(classOf({"Main", "Mirror"}) == "RemoteTransform3D", "RemoteTransform3D");
    check(classOf({"Main", "Lamp", "Seen"}) == "VisibleOnScreenEnabler3D", "VisibleOnScreenEnabler3D");
    check(classOf({"Main", "Skeleton3D", "Look"}) == "Node3D" && propOf(w, find(w, {"Main", "Skeleton3D", "Look"}), "_class") != nullptr,
          "a modifier the runtime lacks (LookAtModifier3D) is a Node3D carrier that names its class");

    // ---- the tree ----------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the AnimationTree --");
    const int tree = find(w, {"Main", "AnimationTree"});
    gd::GodotVariant root;
    check(parsed(propOf(w, tree, "tree_root"), root) && isObject(&root, "AnimationNodeStateMachine"), "tree_root is Object(AnimationNodeStateMachine, ...)");
    const gd::GodotVariant* idleState = key(root, "states/Idle/node");
    check(isObject(idleState, "AnimationNodeAnimation") && key(*idleState, "animation") && key(*idleState, "animation")->asString() == "idle", "the Idle state holds an animation node for 'idle'");
    const gd::GodotVariant* idlePos = key(root, "states/Idle/position");
    check(idlePos && idlePos->type == gd::GodotVType::Vector2 && near(idlePos->num(0), 300) && near(idlePos->num(1), 100), "state positions are kept");
    const gd::GodotVariant* trans = key(root, "transitions");
    check(trans && trans->type == gd::GodotVType::Array && trans->items.size() == 9, "nine entries: three transitions as from, to, resource");
    if (trans && trans->items.size() == 9) {
        check(trans->items[0].asString() == "Start" && trans->items[1].asString() == "Idle" && isObject(&trans->items[2], "AnimationNodeStateMachineTransition"), "Start to Idle");
        const gd::GodotVariant& tMove = trans->items[5];
        check(isObject(&tMove, "AnimationNodeStateMachineTransition") && key(tMove, "switch_mode") && key(tMove, "switch_mode")->asInt() == 1 &&
                  key(tMove, "advance_condition") && key(tMove, "advance_condition")->asString() == "running" && key(tMove, "xfade_time") && near(key(tMove, "xfade_time")->asNumber(), 0.2),
              "Idle to Move: sync, condition 'running', xfade 0.2");
        const gd::GodotVariant* curve = key(tMove, "xfade_curve");
        check(isObject(curve, "Curve") && key(*curve, "_data") && key(*curve, "_data")->items.size() == 10 && key(*curve, "point_count") && key(*curve, "point_count")->asInt() == 2,
              "its xfade curve is Object(Curve) with two points of data");
        const gd::GodotVariant& tBack = trans->items[8];
        check(key(tBack, "advance_expression") && key(tBack, "advance_expression")->asString() == "speed < 1.0" && key(tBack, "advance_mode")->asInt() == 2 && key(tBack, "priority")->asInt() == 2,
              "Move to Idle: auto advance with an expression, priority 2");
    }
    const gd::GodotVariant* move = key(root, "states/Move/node");
    check(isObject(move, "AnimationNodeBlendTree"), "the Move state is a blend tree");
    if (move) {
        const gd::GodotVariant* b2 = key(*move, "nodes/Blend2/node");
        check(isObject(b2, "AnimationNodeBlend2") && key(*b2, "filter_enabled") && key(*b2, "filter_enabled")->asBool() && key(*b2, "filters") && key(*b2, "filters")->items.size() == 1,
              "it holds a Blend2 with its filter");
        const gd::GodotVariant* conn = key(*move, "node_connections");
        check(conn && conn->items.size() == 9 && conn->items[6].asString() == "output" && conn->items[8].asString() == "Blend2", "the wiring is kept (output <- Blend2)");
        check(isObject(key(*move, "nodes/Run/node"), "AnimationNodeAnimation"), "the Run node is an animation node");
    }
    const std::string* ap = propOf(w, tree, "anim_player");
    check(ap && ap->find("../AnimationPlayer") != std::string::npos, "anim_player keeps its NodePath");
    const std::string* act = propOf(w, tree, "active");
    check(act && *act == "true", "active is kept");
    const std::string* rm = propOf(w, tree, "root_motion_track");
    check(rm && rm->find("Skeleton3D:Root") != std::string::npos, "root_motion_track keeps its NodePath");
    const std::string* cond = propOf(w, tree, "parameters/conditions/running");
    check(cond && *cond == "false", "a parameters/ key is written under its scene name");
    const std::string* amount = propOf(w, tree, "parameters/Move/Blend2/blend_amount");
    check(amount && near(std::atof(amount->c_str()), 0.5), "a nested parameter keeps its path and value");

    // ---- skeleton, paths, helpers -----------------------------------------------------------------------------------------------------
    AVER_INFO("-- skeleton, path and helper nodes --");
    const int skel = find(w, {"Main", "Skeleton3D"});
    const std::string* b1 = propOf(w, skel, "bones/1/name");
    const std::string* p2 = propOf(w, skel, "bones/2/parent");
    check(b1 && b1->find("Arm") != std::string::npos && p2 && *p2 == "1", "bones/N/name and parent are written");
    check(propOf(w, skel, "bones/2/rest") != nullptr && propOf(w, skel, "bones/0/rotation") != nullptr, "rest and pose values are written");
    const std::string* bone = propOf(w, find(w, {"Main", "Skeleton3D", "Grip"}), "bone_name");
    check(bone && bone->find("Hand") != std::string::npos, "BoneAttachment3D keeps bone_name");

    const int track = find(w, {"Main", "Track"});
    gd::GodotVariant curve3;
    check(parsed(propOf(w, track, "curve"), curve3) && isObject(&curve3, "Curve3D"), "the path's curve is Object(Curve3D, ...)");
    const gd::GodotVariant* data = key(curve3, "_data");
    const gd::GodotVariant* pts = data ? key(*data, "points") : nullptr;
    check(pts && pts->type == gd::GodotVType::PackedVector3Array && pts->nums.size() == 18 && near(pts->nums[15], 10.0) && near(pts->nums[16], 0.0),
          "its points stay in Godot's metres and axes (the runtime resource is Godot-space): the last position is (10, 0, 0)");
    const std::string* ratio = propOf(w, find(w, {"Main", "Track", "Rider"}), "progress_ratio");
    const std::string* rmode = propOf(w, find(w, {"Main", "Track", "Rider"}), "rotation_mode");
    const std::string* loop = propOf(w, find(w, {"Main", "Track", "Rider"}), "loop");
    check(ratio && near(std::atof(ratio->c_str()), 0.5) && rmode && *rmode == "4" && loop && *loop == "false", "PathFollow3D keeps progress_ratio, rotation_mode and loop");

    gd::GodotVariant shape;
    check(parsed(propOf(w, find(w, {"Main", "Boom"}), "shape"), shape) && isObject(&shape, "SphereShape3D") && key(shape, "radius") && near(key(shape, "radius")->asNumber(), 0.25),
          "SpringArm3D's shape is a sphere of radius 0.25");
    const std::string* len = propOf(w, find(w, {"Main", "Boom"}), "spring_length");
    check(len && near(std::atof(len->c_str()), 3.0), "spring_length is kept");
    const std::string* remote = propOf(w, find(w, {"Main", "Mirror"}), "remote_path");
    const std::string* us = propOf(w, find(w, {"Main", "Mirror"}), "update_scale");
    check(remote && remote->find("../Lamp") != std::string::npos && us && *us == "false", "RemoteTransform3D keeps remote_path and update_scale");
    const std::string* aabb = propOf(w, find(w, {"Main", "Lamp", "Seen"}), "aabb");
    check(aabb && aabb->find("AABB") != std::string::npos, "the notifier box is kept");

    // ---- the animation files ----------------------------------------------------------------------------------------------------------
    AVER_INFO("-- animations --");
    fmt::OcAnimation walk;
    check(fmt::loadOcAnim(content + "/Animations/main_walk.ocanim", walk, &err), "main_walk.ocanim loads: " + err);
    check(walk.propTracks.size() == 1 && walk.propTracks[0].path == "Skeleton3D:Root" && walk.propTracks[0].type == fmt::OcPropTrackType::Position3D, "the bone track keeps its path");

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
        check(has("\"type\":\"AnimationTree\"", "\"rating\":\"converted\"", "\"property\":\"tree_root\""), "tree_root is rated converted");
        check(has("\"type\":\"AnimationTree\"", "\"rating\":\"converted\"", "\"property\":\"parameters/Move/Blend2/blend_amount\""), "a parameter is rated converted");
        check(has("\"type\":\"AnimationTree\"", "\"rating\":\"converted\"", "\"property\":\"root_motion_track\""), "root_motion_track is rated converted");
        check(!has("\"type\":\"AnimationTree\"", "\"rating\":\"unsupported\"", "\"kind\":\"node\""), "the node is no longer reported as having no class");
        check(has("\"type\":\"AnimationNodeAnimation\"", "\"rating\":\"approximated\"", "\"property\":\"use_custom_timeline\""), "use_custom_timeline is approximated (stored, not applied)");
        check(has("\"type\":\"AnimationNodeStateMachine\"", "\"kind\":\"resource\"", "\"rating\":\"converted\""), "the state machine resource is rated converted");
        check(has("\"type\":\"AnimationNodeBlendTree\"", "\"kind\":\"resource\"", "\"rating\":\"converted\""), "the blend tree resource is rated converted");
        check(has("\"type\":\"Curve3D\"", "\"kind\":\"resource\"", "\"rating\":\"converted\""), "the curve resource is rated converted");
        check(has("\"type\":\"Skeleton3D\"", "\"rating\":\"approximated\"", "not connected to a skinned mesh"), "Skeleton3D is approximated, with its reason");
        check(has("\"type\":\"Skeleton3D\"", "\"rating\":\"converted\"", "\"property\":\"bones/1/name\""), "a bone property is rated converted");
        check(has("\"type\":\"SkeletonIK3D\"", "\"rating\":\"approximated\"", "FABRIK"), "SkeletonIK3D is approximated, with its reason");
        check(has("\"type\":\"SkeletonIK3D\"", "\"rating\":\"approximated\"", "\"property\":\"override_tip_basis\""), "override_tip_basis is approximated");
        check(has("\"type\":\"LookAtModifier3D\"", "\"rating\":\"unsupported\"", "LookAtModifier3D"), "LookAtModifier3D is unsupported");
        check(has("\"type\":\"VisibleOnScreenEnabler3D\"", "\"rating\":\"approximated\"", "frustum"), "the on-screen enabler is approximated, with its reason");
        check(has("\"type\":\"PathFollow3D\"", "\"rating\":\"converted\"", "\"property\":\"rotation_mode\""), "PathFollow3D properties are rated converted");
        check(has("\"type\":\"SpringArm3D\"", "\"rating\":\"converted\"", "\"property\":\"shape\""), "SpringArm3D shape is rated converted");
        check(has("\"type\":\"RemoteTransform3D\"", "\"rating\":\"converted\"", "\"property\":\"update_scale\""), "RemoteTransform3D properties are rated converted");
        check(has("Animation", "\"rating\":\"approximated\"", "bone track"), "the bone track is rated approximated, with its reason");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
