// FabricMeshExtraTest -- G4 R1 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/ExtraGame, then the files it wrote read back
// with the engine's own loaders. A glTF with COLOR_0, TANGENT and TEXCOORD_1 must come out as an .ocmesh carrying all three streams (through the
// glTF axis map, with the tangent's handedness re-derived for the engine's UVs), a primitive BoxMesh must carry generated tangents, and a
// StandardMaterial3D with vertex_color_use_as_albedo must become an .ocmat with the vertexcolor flag, rated converted in the report.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/godot/GodotVariant.hpp"
#include "aver/platform/FileSystem.hpp"
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

static std::string meshPathOf(const fmt::OcWorldData& w, int idx) {
    if (idx < 0) return {};
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props")
            if (const std::string* v = c.find("mesh")) {
                gd::GodotVariant var;
                gd::GodotDiagnostics d;
                gd::parseGodotVariant(*v, var, &d, "mesh");
                return var.type == gd::GodotVType::Resource ? var.s : std::string();
            }
    return {};
}

int main() {
    AVER_INFO("FabricMeshExtraTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string dataDir = std::string(AVER_FABRIC_DATA) + "/ExtraGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(dataDir + "/project.godot", ec), "the Godot fixture exists: " + dataDir);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-meshextra-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string out1 = (outRoot / "one").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + dataDir + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string proj = out1 + "/ExtraGame";
    const std::string content = proj + "/Content";
    std::string err;

    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);

    // ---- the glTF model: colour, tangent and UV1 survive into the .ocmesh --------------------------------------------------------------
    AVER_INFO("-- COLOR_0, TANGENT and TEXCOORD_1 -> the .ocmesh extra streams --");
    {
        const std::string quadPath = meshPathOf(w, find(w, {"Main", "Model", "Quad"}));
        fmt::OcMeshData q;
        check(!quadPath.empty() && fmt::loadOcMesh(content + "/" + quadPath, q, &err), "the imported quad loads: " + quadPath + " " + err);
        check(q.vertexCount() == 4 && q.valid(), "four valid vertices");
        check(q.hasColor() && q.hasTangent() && q.hasUv1(), "it carries colour, tangent and UV1");
        check((q.flags & fmt::kOcMeshHasColor) && (q.flags & fmt::kOcMeshHasTangent) && (q.flags & fmt::kOcMeshHasUV1), "and says so in its flags");
        if (q.hasColor() && q.hasTangent() && q.hasUv1()) {
            // The vertices come in glTF order: red, green, blue, white at half alpha.
            check(near(q.colors[0], 1) && near(q.colors[1], 0) && near(q.colors[2], 0) && near(q.colors[3], 1), "vertex 0 is red");
            check(near(q.colors[5], 1) && near(q.colors[10], 1), "vertex 1 is green, vertex 2 blue");
            check(near(q.colors[12], 1) && near(q.colors[15], 128.0 / 255.0, 1e-3), "vertex 3 is white at 50% alpha (quantised to 8 bits)");
            check(near(q.uvs1[0], 0.25) && near(q.uvs1[1], 0.25) && near(q.uvs1[6], 0.25) && near(q.uvs1[7], 0.75), "UV1 is carried through unchanged");
            check(near(q.uvs[1], 1) && near(q.uvs[5], 0), "UV0 is still the first set");
            // glTF tangent +X is engine +Y (the axis map is x = -z, y = x, z = y); the normal +Z is engine -X. The glTF bitangent
            // cross(N, T) points at decreasing v, so the engine's w (toward increasing v) is derived afresh: +1 here.
            for (int i = 0; i < 4; ++i) {
                const f32* t = &q.tangents[static_cast<usize>(i) * 4];
                check(near(t[0], 0, 1e-3) && near(t[1], 1, 1e-3) && near(t[2], 0, 1e-3) && near(t[3], 1), "vertex " + std::to_string(i) + ": tangent (0,1,0), handedness +1");
            }
            const f32* n = &q.normals[0];
            check(near(n[0], -1, 1e-3) && near(n[1], 0, 1e-3) && near(n[2], 0, 1e-3), "the normal went through the same map: -X");
            // The contract: cross(normal, tangent) * w points along increasing v (UV0 v rises toward engine -Z here).
            const f32 cz = n[0] * q.tangents[1] - n[1] * q.tangents[0];
            check(cz * q.tangents[3] < 0, "cross(N, T) * w points along increasing v (-Z)");
        }
    }

    // ---- a primitive mesh gets generated tangents --------------------------------------------------------------------------------------
    AVER_INFO("-- BoxMesh -> generated tangents --");
    {
        const std::string boxPath = meshPathOf(w, find(w, {"Main", "Cube"}));
        fmt::OcMeshData box;
        check(!boxPath.empty() && fmt::loadOcMesh(content + "/" + boxPath, box, &err), "the BoxMesh loads: " + boxPath + " " + err);
        check(box.vertexCount() == 24 && box.hasTangent(), "24 vertices with a tangent each");
        check(!box.hasColor() && !box.hasUv1(), "no colour and no UV1 (Godot's primitives have none)");
        bool unit = true, orthogonal = true, handed = true;
        for (u32 i = 0; box.hasTangent() && i < box.vertexCount(); ++i) {
            const f32* t = &box.tangents[usize(i) * 4];
            const f32* n = &box.normals[usize(i) * 3];
            const f32 len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (!near(len, 1, 1e-3)) unit = false;
            if (std::fabs(t[0] * n[0] + t[1] * n[1] + t[2] * n[2]) > 1e-3) orthogonal = false;
            if (!near(std::fabs(t[3]), 1)) handed = false;
        }
        check(unit, "every tangent is a unit vector");
        check(orthogonal, "and perpendicular to its normal");
        check(handed, "with a handedness of exactly +-1");
    }

#if defined(AVER_FABRIC_TEST_OCMAT)
    // ---- the material flag ------------------------------------------------------------------------------------------------------
    AVER_INFO("-- vertex_color_use_as_albedo -> the .ocmat vertexcolor flag --");
    {
        pbr::MaterialDesc tinted;
        fmt::OcMatExtras ex;
        check(fmt::loadOcmat(content + "/Materials/ExtraGame_tinted.ocmat", tinted, &ex, &err), "the material loads: " + err);
        check(tinted.vertexColor, "MaterialDesc::vertexColor is set");
        check(slurp(content + "/Materials/ExtraGame_tinted.ocmat").find("vertexcolor=1") != std::string::npos, "the file spells it vertexcolor=1");
        check(fmt::writeOcmat(tinted, &ex).find("vertexcolor=1") != std::string::npos, "and a rewrite keeps it");
        pbr::MaterialDesc off = tinted;
        off.vertexColor = false;
        check(fmt::writeOcmat(off, &ex).find("vertexcolor") == std::string::npos, "an .ocmat without it writes nothing new (older files stay byte-identical)");
    }
#endif

    // ---- the report -------------------------------------------------------------------------------------------------------------
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
        check(has("\"property\":\"vertex_color_use_as_albedo\"", "\"rating\":\"converted\"", "vertexcolor"), "vertex_color_use_as_albedo is rated converted");
        check(has("\"property\":\"vertex_color_is_srgb\"", "\"rating\":\"approximated\"", "linear"), "vertex_color_is_srgb is rated approximated");
        check(!has("\"property\":\"vertex_color_use_as_albedo\"", "\"rating\":\"unsupported\"", ""), "and is no longer listed as unsupported");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
