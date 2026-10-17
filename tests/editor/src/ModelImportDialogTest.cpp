// The editor side of model import (sandbox/src/ModelImportDialog.cpp, compiled here with AVER_WITH_IMGUI
// undefined -- the AnimGraphEditorTest arrangement): the extension routing table, the options text, the shared
// writer through importFbxToDir (files, names, the level's start camera, the record), Reimport selection from a
// record or from a file it names, overwrite through the recycler hook, and a missing source. No UI, no GPU.
// Exits 0 or 1.
#include "ModelImportDialog.hpp"

#include "FbxFixture.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/OcWorld.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---- the recycler hook: files "go to the recycle bin" by being renamed into a trash folder ----
static fs::path g_trash;
static int g_recycled = 0;
static bool testRecycler(const std::string& path) {
    std::error_code ec;
    fs::create_directories(g_trash, ec);
    fs::rename(path, g_trash / (std::to_string(g_recycled) + "_" + fs::path(path).filename().string()), ec);
    if (ec) return false;
    ++g_recycled;
    return true;
}

static void writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// A skinned triangle with one animation and a camera, in a Y-up metre file.
static std::string rigFile(bool withCamera) {
    fbxfix::Fbx f;
    const int root = f.bone("Root", 0);
    const int mid = f.bone("Mid", root, {0, 1, 0});
    const int node = f.mesh("HeroBody", 0, fbxfix::triangle());
    f.skin(f.geometryOf(node), {{root, {0}, {1.0}, {}, fbxfix::matTRS({0, 0, 0})},
                                {mid, {1, 2}, {1.0, 1.0}, {}, fbxfix::matTRS({0, 1, 0})}});
    fbxfix::Fbx::Curve c;
    c.target = mid; c.prop = "Lcl Translation"; c.axis = 'X'; c.keys = {{0.0, 0.0}, {1.0, 1.0}}; c.base = {0, 1, 0};
    f.animStack("Walk", 1.0, {c});
    if (withCamera) f.camera("Cam", 0, {1, 2, 3});
    return f.toString();
}

static void testRouting() {
    AVER_INFO("=== extension routing ===");
    check(modelFormatOfPath("a/b/Model.GLB") == ModelFormat::Gltf && modelFormatOfPath("x.gltf") == ModelFormat::Gltf, ".gltf and .glb route to glTF, any case");
    check(modelFormatOfPath("Hero.fbx") == ModelFormat::Fbx && modelFormatOfPath("C:/art/Hero.FBX") == ModelFormat::Fbx, ".fbx routes to FBX, any case");
    check(modelFormatOfPath("a.obj") == ModelFormat::None && modelFormatOfPath("a.wav") == ModelFormat::None &&
          modelFormatOfPath("noext") == ModelFormat::None && modelFormatOfPath("a.fbx.txt") == ModelFormat::None, "everything else is a plain copy");
    check(std::string(modelFormatName(ModelFormat::Fbx)) == "fbx" && std::string(modelFormatName(ModelFormat::Gltf)) == "gltf" &&
          std::string(modelFormatName(ModelFormat::None)).empty(), "format names");
    check(modelExtensionRoutes().size() == 3, "the table has three rows");
}

static void testOptionsText() {
    AVER_INFO("=== options struct <-> text ===");
    fmt::FbxImportOptions o;
    o.scale = 2.0f; o.rootMotion = fmt::FbxImportOptions::RootMotion::Off; o.bakeFps = 24; o.importBlendShapes = false;
    fmt::FbxImportOptions back;
    std::string why;
    check(fmt::fbxOptionsFromText(fmt::fbxOptionsToText(o), back, &why) && back.scale == 2.0f &&
          back.rootMotion == fmt::FbxImportOptions::RootMotion::Off && back.bakeFps == 24 && !back.importBlendShapes, "options survive the record's text");
}

static void testWriterAndRecord() {
    AVER_INFO("=== the shared writer, the record, Reimport ===");
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "aver_model_import_test";
    fs::remove_all(root, ec);
    const fs::path project = root / "Project", content = project / "Content", dest = content / "Meshes", source = project / "Source" / "Hero.fbx";
    g_trash = root / "trash";
    g_recycled = 0;
    setModelImportRecycler(&testRecycler);
    writeText(source, rigFile(true));

    ModelImportSummary s1;
    std::string why;
    check(importFbxToDir(source.string(), dest.string(), content.string(), false, fmt::FbxImportOptions{}, s1, &why), "the first import succeeds (" + why + ")");
    check(s1.meshesWritten == 1 && s1.rigsWritten == 1 && s1.clipsWritten == 1, "one mesh, one skeleton, one clip");
    check(fs::exists(dest / "HeroBody.ocmesh", ec), "the mesh is named after its node");
    check(fs::exists(dest / "Root.ocskel", ec), "the skeleton is named after its top bone");
    check(fs::exists(dest / "Walk.ocanim", ec), "the clip is named after its stack");
    check(fs::exists(dest / "Hero.ocworld", ec), "a camera makes the writer lay a scene out");

    fmt::OcAnimation clip;
    check(fmt::loadOcAnim((dest / "Walk.ocanim").string(), clip, &why) && clip.skeletonRef == "Root", "the clip names its skeleton by file stem");

    fmt::OcWorldData world;
    check(fmt::loadOcworld((dest / "Hero.ocworld").string(), world, &why) && world.hasCamera, "the level opens at the file's camera");
    check(world.hasCamera && std::fabs(world.camX + 300.0) < 0.01 && std::fabs(world.camY - 100.0) < 0.01 &&
          std::fabs(world.camZ - 200.0) < 0.01 && std::fabs(world.camYaw - 90.0) < 0.01, "at the camera's engine position and yaw");
    check(world.placements.size() == 1 && world.placements[0].asset == "Meshes/HeroBody.ocmesh", "with the placement content-relative");

    // ---- the record ----
    const fs::path recPath = dest / "Hero.ocimport";
    check(fs::exists(recPath, ec) && s1.recordPath == recPath.string(), "the record is <stem>.ocimport beside the outputs");
    fmt::ImportRecord rec;
    check(fmt::loadImportRecord(recPath.string(), rec, &why), "it loads (" + why + ")");
    check(rec.format == "fbx" && rec.source == "Source/Hero.fbx", "the source is project-relative");
    check(rec.sourceSize == fs::file_size(source, ec), "with the source's size");
    check(rec.options == fmt::fbxOptionsToText(fmt::FbxImportOptions{}), "and the options used");
    check(fmt::importRecordHasOutput(rec, "HeroBody.ocmesh") && fmt::importRecordHasOutput(rec, "Root.ocskel") &&
          fmt::importRecordHasOutput(rec, "Walk.ocanim") && fmt::importRecordHasOutput(rec, "Hero.ocworld"), "it lists the files written");
    check(!importSourceChanged(rec, project.string()), "the source has not changed since");

    // A record keeps lines it does not know when it is rewritten.
    rec.extraLines.push_back("FUTURE keep=me");
    check(fmt::saveImportRecord(recPath.string(), rec, &why), "a record with an unknown line saves");
    {
        fmt::ImportRecord fresh = makeImportRecord("fbx", source.string(), project.string(), rec.options, rec.outputs);
        check(writeModelImportRecord(dest.string(), "Hero", fresh, &why) , "writing over it works");
        fmt::ImportRecord again;
        fmt::loadImportRecord(recPath.string(), again, nullptr);
        check(again.extraLines.size() == 1 && again.extraLines[0] == "FUTURE keep=me", "and the unknown line survived the rewrite");
    }
    check(listImportRecords(dest.string()).size() == 1, "listImportRecords finds the record");

    // ---- overwrite goes through the recycler ----
    ModelImportSummary s2;
    check(importFbxToDir(source.string(), dest.string(), content.string(), false, fmt::FbxImportOptions{}, s2, &why), "a second import without overwrite succeeds");
    check(s2.meshesWritten == 0 && s2.rigsWritten == 0 && s2.clipsWritten == 0 && g_recycled == 0, "and writes nothing new, recycling nothing");
    ModelImportSummary s3;
    const int before = g_recycled;
    check(importFbxToDir(source.string(), dest.string(), content.string(), true, fmt::FbxImportOptions{}, s3, &why), "with overwrite it succeeds");
    check(s3.meshesWritten == 1 && s3.rigsWritten == 1 && s3.clipsWritten == 1, "and writes everything again");
    check(g_recycled - before >= 4, "the replaced files went to the recycler (mesh, skeleton, clip, level)");

    // ---- Reimport selection ----
    ReimportTarget t;
    check(findReimportTarget(recPath.string(), project.string(), t) && t.sourceExists && t.sourcePath.find("Hero.fbx") != std::string::npos, "the record itself is a reimport target");
    ReimportTarget t2;
    check(findReimportTarget((dest / "HeroBody.ocmesh").string(), project.string(), t2) && t2.recordPath == recPath.string(), "so is any file the record names");
    writeText(dest / "Unrelated.ocmesh", "x");
    ReimportTarget t3;
    check(!findReimportTarget((dest / "Unrelated.ocmesh").string(), project.string(), t3), "a file no record names is not");
    ModelImportSummary rs;
    check(reimportModel(t2, content.string(), nullptr, rs, &why) && rs.meshesWritten == 1, "Reimport re-runs the import with the stored options (" + why + ")");

    fmt::FbxImportOptions changed;
    changed.importAnimations = false;
    ModelImportSummary rs2;
    check(reimportModel(t2, content.string(), &changed, rs2, &why) && rs2.clipsWritten == 0, "Import options... can change one option for the reimport");
    fmt::ImportRecord afterChange;
    fmt::loadImportRecord(recPath.string(), afterChange, nullptr);
    fmt::FbxImportOptions stored;
    fmt::fbxOptionsFromText(afterChange.options, stored, nullptr);
    check(!stored.importAnimations, "and the record now stores the changed option");

    // ---- a missing source disables the action and says where it looked ----
    std::error_code rmEc;
    fs::rename(source, source.string() + ".moved", rmEc);
    ReimportTarget t4;
    check(findReimportTarget(recPath.string(), project.string(), t4) && !t4.sourceExists, "a record whose source is gone is still found, flagged");
    ModelImportSummary rs4;
    why.clear();
    check(!reimportModel(t4, content.string(), nullptr, rs4, &why) && why.find("missing") != std::string::npos &&
          why.find("Hero.fbx") != std::string::npos, "and Reimport refuses, naming the source (" + why + ")");

    setModelImportRecycler(nullptr);
    fs::remove_all(root, ec);
}

int main() {
    AVER_INFO("ModelImportDialogTest");
    testRouting();
    testOptionsText();
    testWriterAndRecord();
    AVER_INFO(g_failures == 0 ? "ModelImportDialogTest: PASS" : "ModelImportDialogTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
