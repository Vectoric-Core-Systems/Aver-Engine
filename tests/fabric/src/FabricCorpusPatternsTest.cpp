// FabricCorpusPatternsTest -- the scene and project patterns two Godot demo projects showed, through the converter: `AverAssetC fabric` on
// tests/fabric/data/CorpusGame (hand-written, minimal), then the files it wrote read back with the engine's own loaders.
//   - children and overrides below the nodes of an instanced glTF model (Skeleton3D, its mesh, a bone attachment, the AnimationPlayer) find their parents
//   - an old project's Godot 3 key codes and joypad numbers, empty key events and the keys Aver cannot bind are mapped or reported precisely
//   - libraries/<name> properties of an AnimationPlayer become one `libraries` dictionary; a curve's _limits become its value range and domain
//   - editor-only project settings are not reported as lost
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/AxisMap.hpp"
#include "aver/formats/OcInput.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotVariant.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
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

// The index of a child by name among the children of placement `parent`, in file order; -1 when absent.
static int childOrder(const fmt::OcWorldData& w, int parent, const char* name) {
    int order = 0;
    for (usize i = 0; i < w.placements.size(); ++i) {
        if (w.placements[i].parent != parent) continue;
        if (w.placements[i].name == name) return order;
        ++order;
    }
    return -1;
}

int main() {
    AVER_INFO("FabricCorpusPatternsTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/CorpusGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-corpus-patterns-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string content = out1 + "/CorpusGame/Content";
    std::string err;
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);

    // ---- nodes inside an instanced model ------------------------------------------------------------------------------------------------
    AVER_INFO("-- the model's node tree --");
    const int armature = find(w, {"Main", "Rig", "Armature"});
    const int skel = find(w, {"Main", "Rig", "Armature", "Skeleton3D"});
    const int body = find(w, {"Main", "Rig", "Armature", "Skeleton3D", "Body"});
    const int attach = find(w, {"Main", "Rig", "Armature", "Skeleton3D", "BoneAttachment3D"});
    const int hand = find(w, {"Main", "Rig", "Armature", "Skeleton3D", "BoneAttachment3D", "Hand"});
    const int tool = find(w, {"Main", "Rig", "Armature", "Skeleton3D", "BoneAttachment3D", "Hand", "Tool"});
    const int muzzle = find(w, {"Main", "Rig", "Armature", "Muzzle"});
    const int sign = find(w, {"Main", "Rig", "Armature", "Muzzle", "Sign"});
    const int prop = find(w, {"Main", "Rig", "Prop"});
    const int player = find(w, {"Main", "Rig", "AnimationPlayer"});
    const auto cls = [&](int i) { return i < 0 ? std::string("<missing>") : w.placements[static_cast<usize>(i)].className; };
    check(cls(armature) == "Node3D", "the model's root glTF node is a Node3D below the instance");
    check(cls(skel) == "Skeleton3D", "the skin's skeleton is a Skeleton3D below the node it hangs from (named Skeleton3D)");
    check(cls(body) == "MeshInstance3D" && propOf(w, body, "mesh") != nullptr, "the skinned mesh node moved below the Skeleton3D and shows its mesh");
    check(cls(attach) == "BoneAttachment3D", "a node hung on a bone is held by a BoneAttachment3D below the skeleton");
    check(cls(hand) == "Node3D" && cls(tool) == "Node3D", "the hung node and the child the scene added to it are found");
    check(cls(muzzle) == "Marker3D" && cls(sign) == "Label3D", "a node added below the model's Armature, and its child, are kept");
    check(childOrder(w, armature, "Skeleton3D") == 0 && childOrder(w, armature, "Muzzle") == 1, "the model's Skeleton3D is the Armature's first child, the added node after it");
    check(cls(prop) == "MeshInstance3D" && propOf(w, prop, "mesh") != nullptr, "a plain mesh node of the model keeps its mesh");
    check(cls(player) == "AnimationPlayer", "the importer's AnimationPlayer exists below the instance (the scene overrides it)");
    {
        const std::string* sp = propOf(w, player, "speed_scale");
        check(sp && near(std::atof(sp->c_str()), 2.0), "an override of the AnimationPlayer applies (speed_scale)");
        const std::string* lay = propOf(w, body, "layers");
        check(lay && *lay == "2", "an override of the skinned mesh node applies (layers)");
    }
    {
        // The Armature is scaled 0.5; the mesh has that baked in, so its node scales back by 2 to stay a net 1 and sits at its position.
        const fmt::OcWorldPlacement& a = w.placements[static_cast<usize>(armature)];
        check(near(a.sx, 0.5) && near(a.sy, 0.5) && near(a.sz, 0.5), "the Armature keeps the model's scale of 0.5");
        const fmt::OcWorldPlacement& b = w.placements[static_cast<usize>(body)];
        check(near(b.sx, 2.0) && near(b.sy, 2.0) && near(b.sz, 2.0) && near(b.x, 0) && near(b.y, 0) && near(b.z, 0), "the baked mesh node scales back by 2 and sits at the origin");
        const Vec3 two = fmt::axis::position(2.0f, 0.0f, 0.0f);
        const fmt::OcWorldPlacement& p = w.placements[static_cast<usize>(prop)];
        check(near(p.x, two.x) && near(p.y, two.y) && near(p.z, two.z), "the unparented mesh node sits where the model puts it (2 m along Godot's X)");
        const Vec3 up2 = fmt::axis::position(0.0f, 2.0f, 0.0f);
        const fmt::OcWorldPlacement& m = w.placements[static_cast<usize>(muzzle)];
        check(near(m.x, up2.x) && near(m.y, up2.y) && near(m.z, up2.z), "the added node keeps the transform the scene wrote");
    }
    {
        gd::GodotVariant v;
        check(propOf(w, skel, "bones/0/name") && propOf(w, skel, "bones/0/name")->find("Root") != std::string::npos, "bones/0 is the skeleton's root joint");
        const std::string* n1 = propOf(w, skel, "bones/1/name");
        check(n1 && n1->find("Tip_001") != std::string::npos, "bones/1 is the next joint, its '.' made '_' as Godot's names are");
        const std::string* par = propOf(w, skel, "bones/1/parent");
        check(par && *par == "0", "bones/1/parent is bone 0");
        check(parsed(propOf(w, skel, "bones/1/position"), v) && v.type == gd::GodotVType::Vector3 && near(v.num(1), 3.0), "an override of bones/1/position wins over the model's rest pose");
        check(parsed(propOf(w, skel, "bones/0/rest"), v) && v.type == gd::GodotVType::Transform3D && near(v.num(10), 1.0), "bones/0/rest holds the joint's rest transform (1 m up)");
        const std::string* bn = propOf(w, attach, "bone_name");
        check(bn && bn->find("Tip_001") != std::string::npos, "the attachment names the bone it follows");
    }

    // ---- libraries/ and _limits ---------------------------------------------------------------------------------------------------------
    AVER_INFO("-- libraries/ and _limits --");
    {
        gd::GodotVariant libs;
        const int anim = find(w, {"Main", "Anim"});
        check(parsed(propOf(w, anim, "libraries"), libs) && libs.type == gd::GodotVType::Dictionary && libs.keys.size() == 2, "libraries/ and libraries/extra become one `libraries` dictionary of two libraries");
        const gd::GodotVariant* extra = libs.find("extra");
        check(extra && extra->type == gd::GodotVType::Dictionary && extra->find("blink2") != nullptr, "the named library holds its animation");
        check(propOf(w, anim, "libraries/") == nullptr && propOf(w, anim, "libraries/extra") == nullptr, "no libraries/<name> key is left behind as metadata");

        gd::GodotVariant curve;
        const int holder = find(w, {"Main", "Holder"});
        check(parsed(propOf(w, holder, "scale_curve"), curve) && curve.type == gd::GodotVType::Object && curve.s == "Curve", "the Curve is an Object(Curve, ...) value");
        const gd::GodotVariant* hi = curve.find("max_value");
        const gd::GodotVariant* lo = curve.find("min_value");
        const gd::GodotVariant* dom = curve.find("max_domain");
        check(hi && near(hi->asNumber(), 3.0) && lo && near(lo->asNumber(), 0.0) && dom && near(dom->asNumber(), 1.0), "_limits became min_value 0, max_value 3 and the domain 0 to 1");
        check(curve.find("_limits") == nullptr, "_limits itself is not written");
    }

    // ---- input ----------------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- input --");
    fmt::OcInputData in;
    check(fmt::loadOcinput(content + "/Input/CorpusGame.ocinput", in, &err), "the input scheme loads: " + err);
    const auto bindingsOf = [&](const char* action) {
        std::vector<std::pair<fmt::OcInputSource, std::string>> r;
        for (const fmt::OcInputBinding& b : in.bindings)
            if (b.action == action) r.emplace_back(b.source, b.key);
        return r;
    };
    const auto has = [](const std::vector<std::pair<fmt::OcInputSource, std::string>>& v, fmt::OcInputSource s, const char* key) {
        for (const auto& p : v)
            if (p.first == s && p.second == key) return true;
        return false;
    };
    const auto declared = [&](const char* action) {
        for (const fmt::OcInputAction& a : in.actions)
            if (a.name == action) return true;
        return false;
    };
    {
        const auto accept = bindingsOf("ui_accept");
        check(accept.size() == 3 && has(accept, fmt::OcInputSource::Key, "Enter") && has(accept, fmt::OcInputSource::Key, "Space") && has(accept, fmt::OcInputSource::GamepadButton, "A"),
              "ui_accept: Godot 3's Enter and KP Enter are one Enter binding, with Space and pad A (3 bindings)");
        const auto left = bindingsOf("ui_left");
        check(left.size() == 2 && has(left, fmt::OcInputSource::Key, "Left") && has(left, fmt::OcInputSource::GamepadButton, "DPadLeft"), "ui_left: Left, and joypad button 14 read with Godot 3's numbering as DPadLeft");
        const auto up = bindingsOf("ui_up");
        check(up.size() == 2 && has(up, fmt::OcInputSource::Key, "Up") && has(up, fmt::OcInputSource::GamepadButton, "DPadUp"), "ui_up: Up, and joypad button 12 as DPadUp");
        check(has(bindingsOf("ui_focus_next"), fmt::OcInputSource::Key, "Tab"), "ui_focus_next: Godot 3's Tab");
        check(declared("ui_focus_prev") && bindingsOf("ui_focus_prev").empty(), "ui_focus_prev: an empty key event is declared with no binding");
        check(declared("ui_page_up") && bindingsOf("ui_page_up").empty(), "ui_page_up: PageUp is not an Aver key, so nothing is bound");
        const auto trig = bindingsOf("ui_trigger");
        check(has(trig, fmt::OcInputSource::Key, "Escape") && has(trig, fmt::OcInputSource::GamepadAxis, "LeftTrigger"), "ui_trigger: Godot 3's L2 button is bound as Aver's trigger axis");
        const auto goUp = bindingsOf("go_up");
        check(goUp.size() == 2 && has(goUp, fmt::OcInputSource::Key, "W") && has(goUp, fmt::OcInputSource::GamepadButton, "DPadUp"), "go_up: a physical keycode, and Godot 4's joypad button 11 as DPadUp");
        check(declared("page_down") && bindingsOf("page_down").empty(), "page_down: neither PageDown nor Godot 4's joypad button 15 (Misc 1) is bindable");
    }

    // ---- the report -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the report --");
    {
        const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
        const auto has3 = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(!has3("was not found; the node is dropped", "", ""), "no node is dropped for a missing parent");
        check(!has3("an override of a node that the instanced scene does not have", "", ""), "no override is lost for a node the model lacks");
        check(has3("\"node\":\"ui_accept\"", "\"rating\":\"approximated\"", "Godot 3's encoding"), "the Godot 3 key code is reported as read, not silently");
        check(has3("\"node\":\"ui_left\"", "\"rating\":\"approximated\"", "Godot 3 joypad numbering"), "the Godot 3 joypad numbering is reported");
        check(has3("\"node\":\"ui_page_up\"", "\"rating\":\"unsupported\"", "PageUp key (in Godot 3's key encoding) is not in Aver's Key enum"), "PageUp is named in the report with the reason");
        check(has3("\"node\":\"page_down\"", "\"rating\":\"unsupported\"", "gamepad button 15 (Misc 1)"), "the unmappable joypad button is named");
        check(has3("\"node\":\"ui_focus_prev\"", "\"rating\":\"approximated\"", "empty key event"), "the empty key event is reported as empty");
        check(has3("\"property\":\"application/config/tags\"", "\"rating\":\"approximated\"", "editor or import-time"), "an editor-only setting is not reported as lost");
        check(has3("\"property\":\"debug/gdscript/warnings/untyped_declaration\"", "\"rating\":\"approximated\"", "editor or import-time"), "a script-warning setting is not reported as lost");
        check(has3("\"type\":\"Curve\"", "\"property\":\"_limits\"", "\"rating\":\"converted\""), "_limits is rated converted");
        check(!has3("\"property\":\"libraries/\"", "path-style property", ""), "libraries/ is no longer a path-style property");
        check(has3("\"type\":\"Skeleton3D\"", "\"rating\":\"approximated\"", "not connected to a skinned mesh"), "the model's Skeleton3D is approximated, with its reason");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
