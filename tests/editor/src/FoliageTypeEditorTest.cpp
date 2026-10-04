// The .ocfoliage editor tab's headless core: load / save / dirty / undo / every field setter, and
// the starter type the Content Browser's "New Foliage Type" writes.
//
// AVER_WITH_IMGUI IS DELIBERATELY UNDEFINED for this target -- FoliageTypeEditor.cpp's
// `#include "imgui.h"` and its whole drawing half sit behind that macro, matching BtEditorTest's own
// precedent (see tests/editor/CMakeLists.txt): what is left is exactly the part worth testing without
// a window. The panel itself is visual-only and is not exercised here.
#include "FoliageTypeEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcFoliage.hpp"

#include <filesystem>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static fmt::OcFoliageData fixture() {
    fmt::OcFoliageData d;
    d.meshPath = "Meshes/Foliage/Tree_Pine.ocmesh";
    d.material = "M_Tree";
    d.scaleMin = 0.8f;
    d.scaleMax = 1.35f;
    d.weight = 2.0f;
    d.randomizeYaw = true;
    d.collisionRadiusCm = 150.0f;
    d.alignToNormal = true;
    return d;
}

int main() {
    AVER_INFO("FoliageTypeEditorTest");

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-foliagetype-editor").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/Tree_Pine.ocfoliage";

    {
        const fmt::OcFoliageData seed = fixture();
        std::string why;
        check(seed.valid(), "the fixture is valid");
        check(fmt::saveOcFoliage(path, seed, &why), "the fixture writes to disk: " + why);
    }

    AVER_INFO("the tab loads what is on disk");
    {
        editor::FoliageTypeEditor ed(path);
        check(ed.loaded(), "loads: " + ed.loadError());
        check(!ed.dirty(), "and starts clean");
        check(ed.type().meshPath == "Meshes/Foliage/Tree_Pine.ocmesh", "meshPath came through");
        check(ed.type().material == "M_Tree", "material came through");
        check(ed.type().scaleMin == 0.8f && ed.type().scaleMax == 1.35f, "scale range came through");
        check(ed.type().weight == 2.0f, "weight came through");
        check(ed.type().collisionRadiusCm == 150.0f, "collisionRadiusCm came through");
        check(ed.type().alignToNormal, "alignToNormal came through");
    }

    AVER_INFO("a missing file loads as an error, not a crash");
    {
        editor::FoliageTypeEditor ed(dir + "/nope.ocfoliage");
        check(!ed.loaded(), "reports not loaded");
        check(!ed.loadError().empty(), "and says why");
    }

    AVER_INFO("every field setter marks the tab dirty, clamps to what valid() requires, and survives save/reload");
    {
        editor::FoliageTypeEditor ed(path);
        check(ed.loaded(), "loads");

        ed.setMeshPath("Meshes/Foliage/Tree_Oak.ocmesh");
        check(ed.dirty(), "setMeshPath marks dirty");
        check(ed.type().meshPath == "Meshes/Foliage/Tree_Oak.ocmesh", "and takes effect");

        ed.setMaterial("M_Oak");
        check(ed.type().material == "M_Oak", "setMaterial takes effect");

        ed.setScaleRange(0.5f, 0.2f);   // max below min -- must be clamped, not written invalid
        check(ed.type().scaleMax >= ed.type().scaleMin,
              "setScaleRange refuses to leave scaleMax below scaleMin");

        ed.setScaleRange(-1.0f, 2.0f);   // a non-positive min -- must be clamped positive
        check(ed.type().scaleMin > 0.0f, "setScaleRange refuses a non-positive scaleMin");

        ed.setWeight(0.0f);
        check(ed.type().weight == 0.0f, "setWeight allows an explicit zero -- 'never picked' is valid");

        ed.setRandomizeYaw(false);
        check(!ed.type().randomizeYaw, "setRandomizeYaw takes effect");

        ed.setCollisionRadiusCm(-5.0f);
        check(ed.type().collisionRadiusCm == 0.0f, "setCollisionRadiusCm refuses a negative value");

        ed.setAlignToNormal(false);
        check(!ed.type().alignToNormal, "setAlignToNormal takes effect");

        check(ed.type().valid(), "every clamp left the record valid()");

        std::string why;
        check(ed.save(&why), "saves: " + why);
        check(!ed.dirty(), "and is clean again");

        fmt::OcFoliageData reloaded;
        check(fmt::loadOcFoliage(path, reloaded, &why), "reloads from disk: " + why);
        check(reloaded.meshPath == "Meshes/Foliage/Tree_Oak.ocmesh" && reloaded.material == "M_Oak" &&
                  reloaded.weight == 0.0f && !reloaded.randomizeYaw && reloaded.collisionRadiusCm == 0.0f &&
                  !reloaded.alignToNormal,
              "AND EVERY EDIT IS ACTUALLY ON DISK -- not just in the tab's own copy");
    }

    AVER_INFO("undo restores the previous record, and redo puts the edit back");
    {
        editor::FoliageTypeEditor ed(path);
        check(ed.loaded(), "loads");
        const f32 before = ed.type().scaleMax;

        ed.setScaleRange(ed.type().scaleMin, before + 1.0f);
        check(ed.type().scaleMax == before + 1.0f, "the edit took effect");
        check(ed.canUndo(), "and there is something to undo");

        ed.undo();
        check(ed.type().scaleMax == before, "undo restores the previous scaleMax");
        check(ed.canRedo(), "and there is something to redo");

        ed.redo();
        check(ed.type().scaleMax == before + 1.0f, "redo puts it back");
    }

    AVER_INFO("a tab with unsaved edits does not silently lose them when the file changes on disk");
    {
        editor::FoliageTypeEditor ed(path);
        ed.setWeight(9.5f);
        check(ed.dirty(), "the tab is dirty");

        ed.onFileChanged();
        check(ed.type().weight == 9.5f && ed.dirty(),
              "onFileChanged KEPT the unsaved edit rather than reloading over it");

        std::string why;
        check(ed.save(&why), "and it can still be saved: " + why);
        ed.onFileChanged();
        check(!ed.dirty(), "a CLEAN tab reloads instead, which is what the callback is for");
    }

    AVER_INFO("title() carries no manual dirty marker -- the host applies its own");
    {
        editor::FoliageTypeEditor ed(path);
        const std::string clean = ed.title();
        ed.setWeight(ed.type().weight + 1.0f);
        check(ed.title() == clean, "title() is unchanged whether the tab is dirty or not");
    }

    AVER_INFO("the factory claims .ocfoliage and nothing else");
    {
        check(editor::makeFoliageTypeEditor(path) != nullptr, "it accepts a .ocfoliage");
        check(editor::makeFoliageTypeEditor(dir + "/nope.ocparticle") == nullptr, "and declines a .ocparticle");
        check(editor::makeFoliageTypeEditor(dir + "/nope.ocbt") == nullptr, "and a .ocbt");
    }

    // THE STARTER THE CONTENT BROWSER WRITES, CHECKED END TO END -- BtEditorTest's own precedent for
    // the identical trap: "New Foliage Type" writes foliageStarterType() and immediately opens the
    // file in this editor, so a starter that fails OcFoliageData::valid() would be written
    // successfully and refused one step later by the very tab the create path opens for it.
    AVER_INFO("the Content Browser's starter type is valid, saves, loads, and opens");
    {
        const fmt::OcFoliageData starter = editor::foliageStarterType();
        check(starter.valid(), "the starter satisfies OcFoliageData::valid(), which the loader enforces");
        check(!starter.meshPath.empty(), "and names a real placeholder mesh rather than an empty one");

        const std::string starterPath = dir + "/starter.ocfoliage";
        std::string why;
        check(fmt::saveOcFoliage(starterPath, starter, &why), "it writes: " + why);

        fmt::OcFoliageData back;
        check(fmt::loadOcFoliage(starterPath, back, &why), "and loads back: " + why);
        check(back.meshPath == starter.meshPath, "with its mesh intact");

        check(editor::makeFoliageTypeEditor(starterPath) != nullptr,
              "and the editor the create path opens for it ACCEPTS it");
        editor::FoliageTypeEditor ed(starterPath);
        check(ed.loaded() && !ed.dirty(), "and it opens clean rather than showing a load error");
    }

    // THE EMPTY-PALETTE REGRESSION, PINNED: editor::foliageModeGate is SandboxApp::
    // editorModeAvailable's Foliage case extracted to two bools in, one struct out, so this can be
    // checked without an App. Before this fix, an empty palette REFUSED mode entry outright --
    // exactly the "mode that cannot be entered is a dead dropdown entry" complaint this module's own
    // history already recorded -- so the panel's own empty-state text could never be reached to show
    // it. See FoliageTypeEditor.hpp's own comment on foliageModeGate for the full story.
    AVER_INFO("foliageModeGate: an empty palette no longer refuses entry, but no landscape still does");
    {
        const auto noLandscapeFullPalette = editor::foliageModeGate(/*landscapeLoaded=*/false,
                                                                     /*paletteEmpty=*/false);
        check(!noLandscapeFullPalette.available, "no landscape refuses entry even with types authored");
        check(noLandscapeFullPalette.whyNot[0] != '\0', "and says why");
        check(!noLandscapeFullPalette.showEmptyState, "no point flagging an empty state on a refusal");

        const auto noLandscapeEmptyPalette = editor::foliageModeGate(/*landscapeLoaded=*/false,
                                                                      /*paletteEmpty=*/true);
        check(!noLandscapeEmptyPalette.available, "no landscape refuses entry regardless of the palette");

        const auto readyEmptyPalette = editor::foliageModeGate(/*landscapeLoaded=*/true,
                                                                /*paletteEmpty=*/true);
        check(readyEmptyPalette.available,
              "THE FIX: landscape present, palette empty -> the mode OPENS rather than refusing");
        check(readyEmptyPalette.showEmptyState,
              "and says the panel should show its create-a-type empty state");

        const auto readyFullPalette = editor::foliageModeGate(/*landscapeLoaded=*/true,
                                                               /*paletteEmpty=*/false);
        check(readyFullPalette.available, "landscape present, palette non-empty -> opens");
        check(!readyFullPalette.showEmptyState, "and shows the normal palette, not the empty state");
    }

    std::filesystem::remove_all(dir, ec);
    AVER_INFO("FoliageTypeEditorTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
