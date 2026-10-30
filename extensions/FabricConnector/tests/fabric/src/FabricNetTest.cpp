// FabricNetTest -- G6 NET2 (high-level multiplayer) through the Godot converter: `AverAssetC fabric` on tests/fabric/data/NetGame, then the levels it wrote read back.
// MultiplayerSpawner and MultiplayerSynchronizer must be placed as their own classes with Godot's properties, a SceneReplicationConfig sub-resource must arrive as an
// Object(SceneReplicationConfig, "properties": [...]) value (4.0 sync / watch spellings turned into modes, entries that name no property dropped), the spawnable scene
// list must be written as converted level paths, and every node, property and entry must be rated.
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

// The "properties" array of an Object(SceneReplicationConfig, ...): entry i's path, spawn flag and mode.
struct Entry { std::string path; bool spawn = false; int mode = -1; };

static std::vector<Entry> entriesOf(const gd::GodotVariant& cfg) {
    std::vector<Entry> out;
    const gd::GodotVariant* props = cfg.find("properties");
    if (!props) return out;
    for (const gd::GodotVariant& d : props->items) {
        Entry e;
        if (const gd::GodotVariant* p = d.find("path")) e.path = p->asString();
        if (const gd::GodotVariant* s = d.find("spawn")) e.spawn = s->asBool();
        if (const gd::GodotVariant* m = d.find("replication_mode")) e.mode = static_cast<int>(m->asInt());
        out.push_back(e);
    }
    return out;
}

int main() {
    AVER_INFO("FabricNetTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/NetGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-net-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/NetGame/Content";
    std::string err;
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "main.ocworld loads: " + err);
    fmt::OcWorldData pw;
    check(fmt::loadOcworld(content + "/Maps/player.ocworld", pw, &err), "player.ocworld loads: " + err);

    // ---- the classes -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- classes --");
    const auto classOf = [](const fmt::OcWorldData& d, std::initializer_list<const char*> path) {
        const int i = find(d, path);
        return i < 0 ? std::string("<missing>") : d.placements[static_cast<usize>(i)].className;
    };
    check(classOf(w, {"Main", "Spawner"}) == "MultiplayerSpawner" && classOf(w, {"Main", "Orphan"}) == "MultiplayerSpawner", "MultiplayerSpawner is placed as a MultiplayerSpawner");
    check(classOf(w, {"Main", "WorldSync"}) == "MultiplayerSynchronizer", "MultiplayerSynchronizer is placed as a MultiplayerSynchronizer");
    check(classOf(pw, {"Player", "Sync"}) == "MultiplayerSynchronizer", "so is the one inside the player scene");
    check(propOf(w, find(w, {"Main", "Spawner"}), "_class") == nullptr && propOf(w, find(w, {"Main", "WorldSync"}), "_class") == nullptr, "neither is a carrier");

    // ---- the spawner -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the spawner --");
    const int spawner = find(w, {"Main", "Spawner"});
    const std::string* sp = propOf(w, spawner, "spawn_path");
    check(sp && sp->find("NodePath(\"../Players\")") != std::string::npos, "spawn_path stays a Godot node path");
    check(numProp(w, spawner, "spawn_limit") == 4, "spawn_limit is kept");
    gd::GodotVariant scenes;
    check(parsed(propOf(w, spawner, "_spawnable_scenes"), scenes) && scenes.type == gd::GodotVType::PackedStringArray && scenes.strs.size() == 2 &&
              scenes.strs[0] == "Maps/player.ocworld" && scenes.strs[1].empty(),
          "the spawnable list is the converted level path, a scene the project lacks keeps its position as an empty entry");

    // ---- the synchronizers -------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the synchronizers --");
    const int ws = find(w, {"Main", "WorldSync"});
    const std::string* rp = propOf(w, ws, "root_path");
    check(rp && rp->find("NodePath(\"..\")") != std::string::npos, "root_path stays a Godot node path");
    check(near(numProp(w, ws, "replication_interval"), 0.2) && near(numProp(w, ws, "delta_interval"), 0.5), "replication_interval and delta_interval");
    const std::string* pub = propOf(w, ws, "public_visibility");
    check(pub && *pub == "false" && numProp(w, ws, "visibility_update_mode") == 2, "public_visibility and visibility_update_mode");
    gd::GodotVariant cfg;
    check(parsed(propOf(w, ws, "replication_config"), cfg) && isObject(&cfg, "SceneReplicationConfig"), "replication_config is Object(SceneReplicationConfig, ...)");
    std::vector<Entry> we = entriesOf(cfg);
    check(we.size() == 2 && we[0].path == ".:round_number" && we[0].spawn && we[0].mode == 2 && we[1].path == ".:clock" && !we[1].spawn && we[1].mode == 1,
          "the world's two properties keep their Godot paths, spawn flags and modes");

    const int ps = find(pw, {"Player", "Sync"});
    gd::GodotVariant pcfg;
    check(parsed(propOf(pw, ps, "replication_config"), pcfg) && isObject(&pcfg, "SceneReplicationConfig"), "the player's replication_config arrives too");
    std::vector<Entry> pe = entriesOf(pcfg);
    check(pe.size() == 3, "five entries, two of which name no property or a mode that does not exist, leave three (got " + std::to_string(pe.size()) + ")");
    check(pe.size() >= 3 && pe[0].path == ".:position" && pe[0].spawn && pe[0].mode == 1 && pe[1].path == ".:health" && !pe[1].spawn && pe[1].mode == 2,
          "the player's position (spawn, every sync) and health (on change)");
    check(pe.size() >= 3 && pe[2].path == "Model:visible" && pe[2].spawn && pe[2].mode == 2, "Godot 4.0's sync = false, watch = true becomes mode on-change");

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
        check(has("\"type\":\"MultiplayerSpawner\"", "\"rating\":\"converted\"", "\"kind\":\"node\""), "the spawner is rated converted");
        check(has("\"type\":\"MultiplayerSpawner\"", "\"rating\":\"converted\"", "\"property\":\"spawn_limit\""), "spawn_limit is rated converted");
        check(has("\"type\":\"MultiplayerSpawner\"", "\"rating\":\"converted\"", "\"property\":\"_spawnable_scenes\""), "a spawnable scene that exists is rated converted");
        check(has("\"type\":\"MultiplayerSpawner\"", "\"rating\":\"unsupported\"", "gone.tscn"), "a spawnable scene the project does not have is unsupported, named");
        check(has("\"type\":\"MultiplayerSpawner\"", "\"rating\":\"approximated\"", "names no node of this scene file"), "a spawn_path that names no node of the scene is approximated");
        check(has("\"type\":\"MultiplayerSynchronizer\"", "\"rating\":\"converted\"", "\"property\":\"replication_interval\""), "replication_interval is rated converted");
        check(has("\"type\":\"MultiplayerSynchronizer\"", "\"rating\":\"converted\"", "\"property\":\"visibility_update_mode\""), "visibility_update_mode is rated converted");
        check(has("\"type\":\"MultiplayerSynchronizer\"", "\"rating\":\"unsupported\"", "\"property\":\"bogus_option\""), "an unknown synchronizer property is unsupported");
        check(has("\"type\":\"SceneReplicationConfig\"", "\"kind\":\"resource\"", "\"rating\":\"converted\""), "a clean SceneReplicationConfig is rated converted");
        check(has("\"type\":\"SceneReplicationConfig\"", "\"kind\":\"resource\"", "\"rating\":\"approximated\""), "one with dropped entries is approximated");
        check(has("\"type\":\"SceneReplicationConfig\"", "\"rating\":\"unsupported\"", "names no property"), "an entry with no property part is unsupported, with the reason");
        check(has("\"type\":\"SceneReplicationConfig\"", "\"rating\":\"unsupported\"", "replication mode 7"), "an entry with a mode that does not exist is unsupported");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
