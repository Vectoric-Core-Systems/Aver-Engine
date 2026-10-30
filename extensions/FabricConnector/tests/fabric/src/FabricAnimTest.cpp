// FabricAnimTest -- G5 A1 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/AnimGame, then the files it wrote read back with the
// engine's own loaders. The Animation sub-resources and the external .tres of the fixture must become .ocanim files with property tracks (transform
// tracks through the shared axis map, value / method / bezier / audio / animation / blend-shape tracks with their keys), the AnimationPlayer node must be
// placed as an AnimationPlayer with its libraries as { name: Resource("Animations/x.ocanim") }, and each item must be rated in the report.
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

static const std::string* propOf(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props") return c.find(key);
    return nullptr;
}

int main() {
    AVER_INFO("FabricAnimTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/AnimGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-anim-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/AnimGame/Content";
    std::string err;

    // ---- the animation files ----------------------------------------------------------------------------------------------------------
    AVER_INFO("-- idle: every track type --");
    fmt::OcAnimation idle;
    check(fmt::loadOcAnim(content + "/Animations/main_idle.ocanim", idle, &err), "main_idle.ocanim loads: " + err);
    check(near(idle.duration, 2.0) && idle.propLoop == fmt::OcPropLoop::Linear && near(idle.propStep, 0.1), "length 2, loop mode linear, step 0.1");
    check(idle.propClip && idle.tracks.empty() && idle.propTracks.size() == 11, "a pure property clip with 11 tracks (got " + std::to_string(idle.propTracks.size()) + ")");
    if (idle.propTracks.size() == 11) {
        const auto& T = idle.propTracks;
        using TT = fmt::OcPropTrackType;
        using K = fmt::OcPropKind;
        check(T[0].type == TT::Value && T[0].path == "Lamp:light_energy" && T[0].interp == fmt::OcPropInterp::Cubic && T[0].keys.size() == 3, "value track: path, cubic interpolation, 3 keys");
        check(T[0].keys.size() == 3 && near(T[0].keys[1].time, 1) && near(T[0].keys[1].transition, 0.5) && T[0].keys[1].value.kind == K::Float && near(T[0].keys[1].value.n[0], 3), "its middle key: time 1, transition 0.5, value 3");
        check(T[0].update == fmt::OcPropUpdate::Continuous && T[0].loopWrap && T[0].enabled && !T[0].imported, "continuous update, loop wrap, enabled, not imported");

        check(T[1].type == TT::Position3D && T[1].path == "Box" && T[1].keys.size() == 2, "position track on Box");
        // Godot (1, 2, 3) m -> (-z, x, y) * 100 cm
        check(T[1].keys.size() == 2 && T[1].keys[1].value.kind == K::Vec3 && near(T[1].keys[1].time, 2) && near(T[1].keys[1].value.n[0], -300) && near(T[1].keys[1].value.n[1], 100) && near(T[1].keys[1].value.n[2], 200),
              "position (1, 2, 3) m goes through the axis map to (-300, 100, 200) cm");

        check(T[2].type == TT::Rotation3D && T[2].keys.size() == 2 && T[2].keys[1].value.kind == K::Quat, "rotation track");
        // Godot quaternion (0, s, 0, s) -> (z, -x, -y, w) = (0, 0, -s, s)
        check(T[2].keys.size() == 2 && near(T[2].keys[1].value.n[0], 0) && near(T[2].keys[1].value.n[1], 0) && near(T[2].keys[1].value.n[2], -0.7071068) && near(T[2].keys[1].value.n[3], 0.7071068),
              "a 90 degree turn about Godot's Y becomes a turn about the engine's -Z (pseudovector sign)");

        check(T[3].type == TT::Scale3D && T[3].keys.size() == 2 && near(T[3].keys[1].value.n[0], 4) && near(T[3].keys[1].value.n[1], 2) && near(T[3].keys[1].value.n[2], 3),
              "scale (2, 3, 4) permutes to (z, x, y) = (4, 2, 3)");

        check(T[4].type == TT::Method && T[4].path == "." && T[4].keys.size() == 2, "method track with 2 keys");
        check(T[4].keys.size() == 2 && T[4].keys[0].value.s == "hit" && near(T[4].keys[0].time, 0.5) && T[4].keys[0].args.size() == 2 && T[4].keys[0].args[0].kind == K::Int &&
                  near(T[4].keys[0].args[0].n[0], 3) && T[4].keys[0].args[1].kind == K::String && T[4].keys[0].args[1].s == "x",
              "first call: hit(3, \"x\") at 0.5");
        check(T[4].keys.size() == 2 && T[4].keys[1].value.s == "done" && T[4].keys[1].args.empty() && near(T[4].keys[1].time, 1.5), "second call: done() at 1.5");

        check(T[5].type == TT::Bezier && T[5].path == "Lamp:light_size" && T[5].keys.size() == 2, "bezier track");
        check(T[5].keys.size() == 2 && near(T[5].keys[1].value.n[0], 10) && near(T[5].keys[1].handle[0], -0.25) && near(T[5].keys[1].handle[1], 0) && near(T[5].keys[1].handle[2], 0.25) && near(T[5].keys[1].handle[3], 0),
              "its second key: value 10, in handle (-0.25, 0), out handle (0.25, 0)");

        check(T[6].type == TT::Audio && T[6].path == "Sfx" && T[6].keys.size() == 1 && T[6].keys[0].value.s == "res://sfx/hit.wav" && near(T[6].keys[0].startOffset, 0.25) && near(T[6].keys[0].endOffset, 0.5),
              "audio track: the stream's res:// path, start 0.25, end 0.5");
        check(T[7].type == TT::Animation && T[7].path == "Sub" && T[7].keys.size() == 2 && T[7].keys[0].value.s == "run" && T[7].keys[1].value.s == "[stop]" && near(T[7].keys[1].time, 1),
              "animation track: run at 0.25, [stop] at 1");
        check(T[8].type == TT::BlendShape && T[8].path == "Box:smile" && T[8].keys.size() == 2 && near(T[8].keys[1].value.n[0], 1) && near(T[8].keys[1].time, 2), "blend-shape track");
        check(T[9].type == TT::Position3D && T[9].path == "Skel:Hip" && T[9].imported, "a bone track keeps its path and its imported flag");
        check(T[10].type == TT::Value && T[10].update == fmt::OcPropUpdate::Discrete && T[10].keys.size() == 2 && T[10].keys[0].value.kind == K::Bool && T[10].keys[0].value.n[0] == 1 && T[10].keys[1].value.n[0] == 0,
              "a discrete value track on a bool");
    }

    AVER_INFO("-- walk and the external .tres --");
    fmt::OcAnimation walk;
    check(fmt::loadOcAnim(content + "/Animations/main_walk.ocanim", walk, &err), "main_walk.ocanim loads: " + err);
    check(near(walk.duration, 1.0) && walk.propLoop == fmt::OcPropLoop::None && walk.propTracks.size() == 1, "length 1, no loop, one track");
    check(walk.propTracks.size() == 1 && walk.propTracks[0].update == fmt::OcPropUpdate::Capture && walk.propTracks[0].keys.size() == 2, "the track is in capture mode");
    fmt::OcAnimation spin;
    check(fmt::loadOcAnim(content + "/Animations/spin.ocanim", spin, &err), "spin.ocanim (from anims/spin.tres) loads: " + err);
    check(near(spin.duration, 1.0) && spin.propLoop == fmt::OcPropLoop::PingPong && spin.propTracks.size() == 1 && spin.propTracks[0].type == fmt::OcPropTrackType::Rotation3D,
          "length 1, ping-pong, one rotation track");

    // ---- the node ---------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the AnimationPlayer node --");
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);
    const int anim = find(w, {"Main", "Anim"});
    check(anim >= 0 && w.placements[static_cast<usize>(anim)].className == "AnimationPlayer", "Anim is placed as an AnimationPlayer");
    const int sub = find(w, {"Main", "Sub"});
    check(sub >= 0 && w.placements[static_cast<usize>(sub)].className == "AnimationPlayer", "a second player with no libraries is placed too");
    const std::string* libs = propOf(w, anim, "libraries");
    check(libs != nullptr, "the libraries property is written");
    if (libs) {
        gd::GodotVariant v;
        gd::GodotDiagnostics d;
        check(gd::parseGodotVariant(*libs, v, &d, "libraries") && v.type == gd::GodotVType::Dictionary && v.keys.size() == 2, "it is a dictionary of two libraries");
        const gd::GodotVariant* defaultLib = v.find("");
        const gd::GodotVariant* extraLib = v.find("extra");
        check(defaultLib && defaultLib->type == gd::GodotVType::Dictionary && defaultLib->find("idle") && defaultLib->find("idle")->type == gd::GodotVType::Resource &&
                  defaultLib->find("idle")->s == "Animations/main_idle.ocanim" && defaultLib->find("walk") && defaultLib->find("walk")->s == "Animations/main_walk.ocanim",
              "the default library maps idle and walk to their .ocanim files");
        check(extraLib && extraLib->find("spin") && extraLib->find("spin")->s == "Animations/spin.ocanim", "the 'extra' library maps spin to the external animation");
    }
    const std::string* autoplay = propOf(w, anim, "autoplay");
    check(autoplay && autoplay->find("idle") != std::string::npos, "autoplay is kept");
    const std::string* speed = propOf(w, anim, "speed_scale");
    check(speed && near(std::atof(speed->c_str()), 1.5), "speed_scale is kept");
    const std::string* blend = propOf(w, anim, "blend_times");
    check(blend && blend->find("walk") != std::string::npos, "blend_times is kept");

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
        check(has("\"type\":\"AnimationPlayer\"", "\"rating\":\"converted\"", "\"property\":\"libraries\""), "the libraries property is rated converted");
        check(!has("\"type\":\"AnimationPlayer\"", "\"rating\":\"unsupported\"", "no Aver.Fabric class"), "the node is no longer reported as having no class");
        check(has("Animation", "\"rating\":\"converted\"", "position_3d Box"), "the position track is rated converted");
        check(has("Animation", "\"rating\":\"approximated\"", "bone track"), "the bone track is rated approximated, with its reason");
        check(has("Animation", "\"rating\":\"approximated\"", "audio"), "the audio track is rated approximated, with its reason");
        check(has("Animation", "\"rating\":\"approximated\"", "handle modes"), "bezier handle modes are rated approximated");
        check(has("\"property\":\"movie_quit_on_finish\"", "\"rating\":\"unsupported\"", "Movie Maker"), "movie_quit_on_finish is unsupported");
        check(has("\"property\":\"reset_on_save\"", "\"rating\":\"approximated\"", "editor"), "reset_on_save is approximated");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
