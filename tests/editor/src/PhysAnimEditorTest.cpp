// PhysAnimEditorTest -- the Physical Animation Profile tab as an asset, headless (sandbox/src/PhysAnimEditor.cpp compiled without ImGui): the
// factory claims only .ocphysanim, a profile opens, edits go through undo and redo, save writes a file the loader reads back, a skeleton can be
// picked, and the problems list names a bone the skeleton lacks.
#include "PhysAnimEditor.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>

using namespace aver;
namespace ed = aver::editor;
namespace pa = aver::editor::physanim;
namespace fs = std::filesystem;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("Physical animation profile editor");
    const fs::path dir = fs::temp_directory_path() / "aver-physanim-editor-test";
    fs::create_directories(dir);
    const std::string profilePath = (dir / "Brawler.ocphysanim").string();
    const std::string skelPath = (dir / "Body.ocskel").string();

    check(ed::makePhysAnimEditor((dir / "x.ocgraph").string()) == nullptr, "the factory declines other extensions");
    std::string why;
    check(ragdoll::savePhysAnimProfile(profilePath, ed::physAnimStarter("Brawler"), &why), "a starter profile saves");

    fmt::OcSkeleton sk;
    for (const char* n : {"Root", "Spine", "Arm"}) { fmt::OcBone b; b.name = n; b.parent = static_cast<i32>(sk.bones.size()) - 1; sk.bones.push_back(b); }
    check(fmt::saveOcSkel(skelPath, sk, &why), "a skeleton saves");

    auto base = ed::makePhysAnimEditor(profilePath);
    check(base != nullptr, "the factory claims .ocphysanim");
    auto* tab = static_cast<ed::PhysAnimEditor*>(base.get());
    check(tab->loaded() && !tab->dirty() && tab->title() == "Brawler.ocphysanim", "the profile opens clean");

    tab->edit([](ragdoll::PhysAnimProfile& p) { pa::addRegion(p, "arm"); pa::assignBone(p, 0, "Arm", true); });
    check(tab->dirty() && tab->profile().regions.size() == 1 && tab->title().back() == '*', "an edit marks it dirty");
    tab->edit([](ragdoll::PhysAnimProfile& p) { p.regions[0].drive.strength = 0.9f; });
    tab->undo();
    check(tab->profile().regions[0].drive.strength != 0.9f && tab->canRedo(), "undo reverses the last edit");
    tab->redo();
    check(tab->profile().regions[0].drive.strength == 0.9f, "redo applies it again");
    tab->undo(); tab->undo();
    check(tab->profile().regions.empty() && !tab->canUndo(), "undo walks back to the start");
    tab->redo(); tab->redo();

    check(!tab->skeletonLoaded() && tab->problems().empty(), "without a skeleton there is nothing to validate against");
    check(!tab->setSkeleton((dir / "missing.ocskel").string()) && tab->setSkeleton(skelPath) && tab->skeletonLoaded(), "a missing skeleton is refused, a real one loads");
    check(tab->problems().empty(), "a profile naming bones the skeleton has is clean");
    tab->edit([](ragdoll::PhysAnimProfile& p) { pa::assignBone(p, 0, "Tail", false); });
    const auto probs = tab->problems();
    check(probs.size() == 1 && probs[0].find("Tail") != std::string::npos, "the problems list names the bone the skeleton lacks");

    check(tab->save(&why) && !tab->dirty(), "save writes and clears dirty");
    ragdoll::PhysAnimProfile back;
    check(ragdoll::loadPhysAnimProfile(profilePath, back, &why) && back.regions.size() == 1 && back.regions[0].drive.strength == 0.9f &&
          back.regions[0].subtrees.size() == 1 && back.regions[0].bones.size() == 1, "the saved file reads back with the edits");

    // a second tab for the same path remembers the skeleton picked this session
    auto again = ed::makePhysAnimEditor(profilePath);
    check(static_cast<ed::PhysAnimEditor*>(again.get())->skeletonLoaded(), "reopening the tab remembers the previewed skeleton");

    fs::remove_all(dir);
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
