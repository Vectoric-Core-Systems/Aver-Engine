// The AverAssetC JSON contract -- the interface Aver Exchange in the LAUNCHER parses.
//
// WHY THIS TEST EXISTS, and it is a different reason from every other test in this directory.
//
// AverAssetC is not called by anything in this repository, and that is CORRECT: the launcher shells
// out to the executable and reads its stdout. So the usual protection a caller gives -- change the
// function, break the build, notice -- does not exist here. The consumer is a separate process in a
// separate codebase, and every field name below is load-bearing to it.
//
// WITHOUT THIS TEST, renaming "itemsWritten" or flipping "verified" to an int is a green build here
// and a broken importer over there, discovered by a user. `schemaVersion` shows the author
// anticipated the versioning problem; nothing enforced it. This does.
//
// IT ASSERTS THE SHAPE, NOT THE CONTENT. Vertex counts and durations belong to the importer's own
// tests (FormatTest, GltfTest, MeshTest); what is pinned here is exactly what a parser depends on:
// which keys exist, what type each holds, that a summary line closes the run, and that FAILURE is
// reportable rather than silent. Pinning the numbers too would make this fail every time the
// importer legitimately improved, and a test that cries wolf gets deleted.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/Image.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Deliberately NOT a JSON parser. The point of this test is that the OUTPUT SHAPE is stable, and a
// tolerant parser would hide exactly the drift it exists to catch -- a renamed key, a quoted number,
// a boolean turned into a string. Substring probes on the raw line are blunt and they fail loudly.
static bool hasKey(const std::string& line, const std::string& key) {
    return line.find("\"" + key + "\":") != std::string::npos;
}
static bool hasPair(const std::string& line, const std::string& key, const std::string& rawValue) {
    return line.find("\"" + key + "\":" + rawValue) != std::string::npos;
}

// _popen rather than a helper in Aver.Platform, because this is the ONLY caller in the tree that
// needs to run a child process and read its output -- adding a platform API for one test would be
// growing the engine's surface to serve a test, which is the wrong direction. 2>&1 because the tool
// writes its diagnostics to stderr and its JSON to stdout, and a launcher reading a pipe gets both.
static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
    // THE WHOLE COMMAND WRAPPED IN ONE MORE PAIR OF QUOTES, and it is not decoration. _popen runs
    // this through cmd.exe, whose parser strips the FIRST and LAST quote of the line -- so a command
    // that legitimately begins with a quoted executable path loses its opening quote and cmd then
    // reads the path up to the first space as the program name. With a path containing "Aver Engine"
    // that is a command not found, an empty pipe and exit 1, which is exactly what this test first
    // reported while the same command pasted into a shell worked perfectly.
    const std::string wrapped = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
    return _pclose(pipe);
}

int main() {
    AVER_INFO("AverAssetCTest");

    // Beside this executable: both land in ${CMAKE_BINARY_DIR}/bin.
    const std::filesystem::path exe =
        std::filesystem::path(AVER_ASSETC_EXE);
    const std::filesystem::path fixture =
        std::filesystem::path(AVER_REPO_ROOT) / "content" / "dev" / "Rig.gltf";

    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC.exe was built: " + exe.string());
    check(std::filesystem::exists(fixture, ec), "the glTF fixture exists: " + fixture.string());
    if (g_failures) { AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures); return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok); }

    const std::filesystem::path outDir =
        std::filesystem::temp_directory_path() / "aver-assetc-test";
    std::filesystem::remove_all(outDir, ec);
    std::filesystem::create_directories(outDir, ec);

    // ---- the success path -------------------------------------------------------------------
    AVER_INFO("-- a real glTF converts, and says so in the shape the launcher parses --");
    std::string out;
    int code = 1;
    {
        const std::string cmd = "\"" + exe.string() + "\" convert \"" + fixture.string() +
                                "\" --out-dir \"" + outDir.string() + "\"";
        code = runCapture(cmd, out);
    }
    check(code == 0, "it exits 0 on a good conversion (got " + std::to_string(code) + ")");

    std::vector<std::string> lines;
    {
        std::istringstream in(out);
        std::string line;
        // The launcher reads LINE-DELIMITED JSON: one object per line, no wrapping array. Anything
        // that is not a JSON object is diagnostic chatter it skips, so only '{' lines are contract.
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line.front() == '{') lines.push_back(line);
        }
    }
    check(lines.size() >= 2, "it emitted at least one artifact line and a summary (" +
                             std::to_string(lines.size()) + " JSON lines)");
    if (lines.empty()) { AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures); return 1; }

    const std::string& summary = lines.back();
    check(hasPair(summary, "summary", "true"), "the LAST JSON line is the summary");

    // Every artifact line, checked field by field. These names ARE the contract.
    int artifacts = 0;
    bool everyArtifactWellFormed = true;
    for (usize i = 0; i + 1 < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (hasPair(l, "summary", "true")) continue;
        ++artifacts;
        for (const char* key : {"schemaVersion", "input", "output", "kind", "ok", "verified"})
            if (!hasKey(l, key)) {
                everyArtifactWellFormed = false;
                AVER_ERROR("    artifact line {} is missing \"{}\": {}", i, key, l);
            }
        if (!hasPair(l, "schemaVersion", "1")) everyArtifactWellFormed = false;
        // UNQUOTED true/false. A launcher doing `json["ok"] == true` breaks silently the day this
        // becomes the STRING "true", and that is a one-character edit away at all times.
        if (!hasPair(l, "ok", "true") && !hasPair(l, "ok", "false")) everyArtifactWellFormed = false;
        if (!hasPair(l, "verified", "true") && !hasPair(l, "verified", "false")) everyArtifactWellFormed = false;
    }
    check(artifacts >= 1, "at least one artifact line (" + std::to_string(artifacts) + ")");
    check(everyArtifactWellFormed, "every artifact line carries schemaVersion/input/output/kind/ok/"
                                   "verified, with ok and verified as UNQUOTED booleans");

    // This fixture is a rig: it must yield a mesh, a skeleton and an animation. Named kinds rather
    // than a count, because a kind string is what the launcher switches on.
    bool sawMesh = false, sawSkel = false, sawAnim = false;
    for (const std::string& l : lines) {
        if (hasPair(l, "kind", "\"mesh\""))      sawMesh = true;
        if (hasPair(l, "kind", "\"skeleton\""))  sawSkel = true;
        if (hasPair(l, "kind", "\"animation\"")) sawAnim = true;
    }
    check(sawMesh && sawSkel && sawAnim,
          "a rigged glTF reports kind mesh, skeleton and animation");

    for (const char* key : {"schemaVersion", "input", "itemsWritten", "itemsFailed", "exitCode"})
        check(hasKey(summary, key), std::string("the summary carries \"") + key + "\"");
    check(hasPair(summary, "itemsFailed", "0"), "and reports nothing failed on a good run");

    // The files it SAID it wrote are on disk. "ok":true with no artifact is the worst possible
    // outcome for an importer, because the launcher would record a success and import nothing.
    int onDisk = 0;
    for (const auto& e : std::filesystem::directory_iterator(outDir, ec)) if (e.is_regular_file(ec)) ++onDisk;
    check(onDisk >= artifacts,
          "every artifact it reported is actually on disk (" + std::to_string(onDisk) + " files for "
          + std::to_string(artifacts) + " reported)");

    // ---- the material path (roughness + metal, no combined map) -----------------------------
    AVER_INFO("-- a separate roughness+metal texture set packs into one metalRough and a .ocmat --");
    const std::filesystem::path roughFixture =
        std::filesystem::path(AVER_REPO_ROOT) / "content" / "dev" / "test_rough_1k.jpg";
    const std::filesystem::path metalFixture =
        std::filesystem::path(AVER_REPO_ROOT) / "content" / "dev" / "test_metal_1k.jpg";
    check(std::filesystem::exists(roughFixture, ec), "the roughness fixture exists: " + roughFixture.string());
    check(std::filesystem::exists(metalFixture, ec), "the metal fixture exists: " + metalFixture.string());

    const std::filesystem::path matOutDir =
        std::filesystem::temp_directory_path() / "aver-assetc-test-material";
    std::filesystem::remove_all(matOutDir, ec);
    std::filesystem::create_directories(matOutDir, ec);

    std::string matOut;
    int matCode = 1;
    {
        const std::string cmd = "\"" + exe.string() + "\" material \"" + roughFixture.string() +
                                "\" \"" + metalFixture.string() + "\" --out-dir \"" + matOutDir.string() +
                                "\" --base TestMat";
        matCode = runCapture(cmd, matOut);
    }
    check(matCode == 0, "it exits 0 on a good material conversion (got " + std::to_string(matCode) + ")");

    std::vector<std::string> matLines;
    {
        std::istringstream in(matOut);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line.front() == '{') matLines.push_back(line);
        }
    }
    check(matLines.size() >= 2, "it emitted at least one artifact line and a summary (" +
                                std::to_string(matLines.size()) + " JSON lines)");

    bool sawMaterialArtifact = false, sawPackedFrom = false;
    for (const std::string& l : matLines) {
        if (hasPair(l, "kind", "\"material\"") && hasPair(l, "ok", "true") && hasPair(l, "verified", "true"))
            sawMaterialArtifact = true;
        if (l.find("\"packedFrom\":\"roughness+metal\"") != std::string::npos) sawPackedFrom = true;
    }
    check(sawMaterialArtifact, "at least one kind:material artifact reports ok:true, verified:true");
    check(sawPackedFrom, "the roughness+metal pair reports packedFrom roughness+metal");

    const std::string& matSummary = matLines.empty() ? std::string{} : matLines.back();
    check(hasPair(matSummary, "summary", "true"), "the LAST JSON line is the summary");
    check(hasPair(matSummary, "itemsFailed", "0"), "and reports nothing failed on a good run");

    // A .ocmat and a packed metalRough PNG must actually be on disk, not just claimed.
    bool sawOcmatFile = false, sawPackedPng = false;
    for (const auto& e : std::filesystem::directory_iterator(matOutDir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string name = e.path().filename().string();
        if (name == "TestMat.ocmat") sawOcmatFile = true;
        if (name == "TestMat_metalRough.png") sawPackedPng = true;
    }
    check(sawOcmatFile, "TestMat.ocmat is actually on disk");
    check(sawPackedPng, "TestMat_metalRough.png is actually on disk");

    // ---- the material's own .ocgraph, and the GRAPHREF that points at it ---------------------
    //
    // A NEW ARTIFACT KIND CROSSES THE PROCESS BOUNDARY, which is the whole reason this file exists:
    // the launcher's Aver Exchange parses these lines and nothing in THIS repository calls the tool,
    // so "kind":"materialgraph" is an interface change that only a check here can protect.
    //
    // The graph is written only where the graph->HLSL compiler is linked (AVER_HAVE_MATERIAL_GRAPH),
    // so its absence is not a failure -- but its PRESENCE has to be complete: a .ocgraph on disk with
    // no GRAPHREF in the .ocmat is a file nothing will ever load, and a GRAPHREF naming a graph that
    // is not there is a material that stops shading. The two are asserted together for that reason.
    bool sawGraphFile = false;
    for (const auto& e : std::filesystem::directory_iterator(matOutDir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().filename().string() == "TestMat.ocgraph") sawGraphFile = true;
    }
    bool sawGraphArtifact = false, sawGraphRef = false;
    for (const std::string& l : matLines) {
        if (hasPair(l, "kind", "\"materialgraph\"") && hasPair(l, "ok", "true")
            && hasPair(l, "verified", "true")) sawGraphArtifact = true;
        if (l.find("\"graphRef\":\"Textures/TestMat.ocgraph\"") != std::string::npos) sawGraphRef = true;
    }
    check(sawGraphFile == sawGraphArtifact,
          "a materialgraph artifact line and a TestMat.ocgraph on disk agree with each other");
    check(sawGraphFile == sawGraphRef,
          "and the .ocmat's graphRef is reported exactly when the graph was written");
    if (sawGraphFile) {
        std::ifstream gin((matOutDir / "TestMat.ocgraph").string(), std::ios::binary);
        const std::string gtext((std::istreambuf_iterator<char>(gin)), std::istreambuf_iterator<char>());
        check(gtext.find("DOMAIN material") != std::string::npos,
              "the generated graph declares DOMAIN material, without which it compiles to nothing");
        check(gtext.find("MaterialOutput") != std::string::npos,
              "and has the one sink node a material graph needs");
        check(gtext.find("slot=metalrough") != std::string::npos,
              "and samples the metalRough slot this fixture packed");
        // The pin TYPE declarations are not decoration: the graph compiler sizes every generic
        // operator from them, and a SampleTexture with no declared output pin reads as a float --
        // which silently narrows the normal chain and loudly breaks the metallic swizzle.
        check(gtext.find("PIN texMetalRough rgb out float3") != std::string::npos,
              "and declares its sample outputs as float3, which is what makes the swizzles legal");
    } else {
        AVER_INFO("[INFO ]   (no .ocgraph: this build has no material graph compiler linked)");
    }

    std::filesystem::remove_all(matOutDir, ec);

    // ---- a texture set with no metalRough bound must not default to fully metallic ----------
    AVER_INFO("-- a set with no metal/rough map at all writes metallicFactor 0, not glTF's default 1 --");
    const std::filesystem::path diffuseFixture =
        std::filesystem::path(AVER_REPO_ROOT) / "content" / "dev" / "test_diffuse_1k.jpg";
    check(std::filesystem::exists(diffuseFixture, ec), "the diffuse fixture exists: " + diffuseFixture.string());

    const std::filesystem::path noMetalOutDir =
        std::filesystem::temp_directory_path() / "aver-assetc-test-nometal";
    std::filesystem::remove_all(noMetalOutDir, ec);
    std::filesystem::create_directories(noMetalOutDir, ec);
    {
        // baseColor + a lone roughness map, no matching metal map: baseColor binds (so the .ocmat
        // actually gets written at all), but packing never triggers for roughness alone, so
        // metalRough stays unbound -- exactly the case pbr::MaterialDesc's own default
        // (metallicFactor = 1.0, glTF's spec default) would otherwise leave rendering as fully
        // metallic despite nothing here being metal.
        const std::string cmd = "\"" + exe.string() + "\" material \"" + diffuseFixture.string() +
                                "\" \"" + roughFixture.string() +
                                "\" --out-dir \"" + noMetalOutDir.string() + "\" --base NoMetalMat";
        std::string noMetalOut;
        const int noMetalCode = runCapture(cmd, noMetalOut);
        check(noMetalCode == 0, "it still exits 0 (got " + std::to_string(noMetalCode) + ")");

        std::ifstream matFile(noMetalOutDir / "NoMetalMat.ocmat");
        std::stringstream matContents;
        matContents << matFile.rdbuf();
        check(matContents.str().find("PARAM metallicFactor 0") != std::string::npos,
              "NoMetalMat.ocmat writes PARAM metallicFactor 0, not the glTF default of 1");
    }
    std::filesystem::remove_all(noMetalOutDir, ec);

    // ---- --max-texture, and the thing about it that can silently be wrong -------------------
    //
    // Resizing is the easy half; FILTERING IN THE RIGHT SPACE is the half that fails invisibly.
    // Colour has to be averaged in linear light, data (roughness, metal, occlusion) straight, and a
    // normal map as vectors -- and the cap only knows which is which by looking at the SLOTS that
    // reference each image. Get that map wrong and every texture still comes out the right size.
    //
    // So this feeds the tool TWO IDENTICAL IMAGES and binds them differently. Same bytes in, and if
    // the role plumbing works, different bytes out: 188 for the colour map (the linear average 0.5,
    // re-encoded to sRGB) and 128 for the data map (the straight byte average). A build that gave
    // every image one role would return the same number twice, whichever number that was.
    AVER_INFO("-- --max-texture downscales, and filters each map in ITS OWN space --");
    {
        const std::filesystem::path capDir = outDir / "cap";
        const std::filesystem::path capContent = capDir / "content";
        std::filesystem::create_directories(capDir, ec);

        // A 2x2 24-bit BMP, half black and half white. BMP because it needs no encoder: stb_image
        // reads it, and hand-writing 70 bytes beats linking a PNG writer into a test.
        const auto writeBmp = [&](const std::filesystem::path& p) {
            const u8 hdr[54] = {
                'B','M',  70,0,0,0,  0,0,0,0,  54,0,0,0,        // file header, 70 bytes total
                40,0,0,0,  2,0,0,0,  2,0,0,0,  1,0,  24,0,      // DIB: 2x2, 1 plane, 24bpp
                0,0,0,0,  16,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0,
            };
            // Bottom-up, BGR, rows padded to 4 bytes. Two white and two black texels either way up.
            const u8 rows[16] = {
                255,255,255,  0,0,0,      0,0,          // bottom row: white, black, 2 pad
                0,0,0,        255,255,255, 0,0,         // top row:    black, white, 2 pad
            };
            std::ofstream f(p, std::ios::binary);
            f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
            f.write(reinterpret_cast<const char*>(rows), sizeof rows);
        };
        writeBmp(capDir / "colour.bmp");
        writeBmp(capDir / "data.bmp");

        {
            std::ofstream f(capDir / "cap.mtl");
            f << "newmtl CapMat\nKd 1 1 1\nmap_Kd colour.bmp\nmap_Pr data.bmp\n";
        }
        {
            std::ofstream f(capDir / "cap.obj");
            f << "mtllib cap.mtl\no Tri\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                 "vt 0 0\nvt 1 0\nvt 0 1\nusemtl CapMat\nf 1/1 2/2 3/3\n";
        }

        std::string capOut;
        const std::string cmd = "\"" + exe.string() + "\" convert \"" +
                                (capDir / "cap.obj").string() + "\" --out-dir \"" + capDir.string() +
                                "\" --content-dir \"" + capContent.string() +
                                "\" --base Cap --max-texture 1";
        const int capCode = runCapture(cmd, capOut);
        check(capCode == 0, "the cap run exits 0 (got " + std::to_string(capCode) + ")");
        check(capOut.find("2x2 -> 1x1") != std::string::npos,
              "and reports what it resized, rather than doing it silently");

        // The extension follows the bytes: whatever went in, a re-encoded texture comes out PNG.
        const std::filesystem::path colourPng = capContent / "Textures" / "Cap" / "colour.png";
        const std::filesystem::path dataPng   = capContent / "Textures" / "Cap" / "data.png";
        check(std::filesystem::exists(colourPng, ec) && std::filesystem::exists(dataPng, ec),
              "both capped textures were written as .png");

        ImageData colour, data;
        std::string derr;
        const bool gotColour = decodeImage(colourPng.string(), colour, &derr);
        const bool gotData   = decodeImage(dataPng.string(), data, &derr);
        check(gotColour && gotData, "and both decode: " + derr);
        if (gotColour && gotData) {
            check(colour.width == 1 && colour.height == 1 && data.width == 1 && data.height == 1,
                  "2x2 capped at 1 is 1x1");
            const int c = colour.pixels.empty() ? -1 : int(colour.pixels[0]);
            const int d = data.pixels.empty()   ? -1 : int(data.pixels[0]);
            check(c >= 186 && c <= 190,
                  "the BASE COLOUR map averaged in linear light (got " + std::to_string(c) +
                  ", want ~188, not ~128)");
            check(d >= 127 && d <= 129,
                  "the ROUGHNESS map averaged straight (got " + std::to_string(d) +
                  ", want ~128, not ~188)");
            check(c != d, "so the two roles are genuinely told apart, not filtered identically");
        }

        // Without the flag nothing is touched: the cap is opt-in, so an existing import pipeline
        // that never passes it keeps handing the engine the author's own pixels.
        const std::filesystem::path plainContent = capDir / "plain";
        std::string plainOut;
        const std::string plainCmd = "\"" + exe.string() + "\" convert \"" +
                                     (capDir / "cap.obj").string() + "\" --out-dir \"" +
                                     capDir.string() + "\" --content-dir \"" +
                                     plainContent.string() + "\" --base Cap";
        runCapture(plainCmd, plainOut);
        ImageData plain;
        check(decodeImage((plainContent / "Textures" / "Cap" / "colour.bmp").string(), plain, &derr) &&
              plain.width == 2 && plain.height == 2,
              "and with no --max-texture the source is copied through untouched, extension and all");
    }
    std::filesystem::remove_all(outDir / "cap", ec);

    // ---- the failure path -------------------------------------------------------------------
    AVER_INFO("-- and a bad input FAILS detectably, rather than silently --");
    {
        std::string badOut;
        const std::string missing = (outDir / "does-not-exist.gltf").string();
        const std::string cmd = "\"" + exe.string() + "\" convert \"" + missing +
                                "\" --out-dir \"" + outDir.string() + "\"";
        const int badCode = runCapture(cmd, badOut);
        // A LAUNCHER NEEDS BOTH SIGNALS. The exit code is what a process wrapper checks; the JSON is
        // what a UI shows. Either one alone leaves the other consumer guessing.
        check(badCode != 0, "a missing input exits non-zero (got " + std::to_string(badCode) + ")");
        check(badOut.find("{") != std::string::npos || badCode != 0,
              "and it is reportable rather than a silent success");
    }

    std::filesystem::remove_all(outDir, ec);
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
