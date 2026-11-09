// DataLayerModelTest -- the Data Layers panel's rules without a window (sandbox/src/DataLayerModel.hpp, header-only): create / rename / delete
// with their cascades, assignment, the parent-cycle refusal, undo and redo through Edit, and the editor visible / loaded toggles.
#include "DataLayerModel.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
namespace dl = aver::editor::dl;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void testCreate() {
    AVER_INFO("=== create ===");
    dl::Model m;
    check(m.create("").empty(), "an empty name is refused");
    check(m.create("Props").layersChanged && m.layers().size() == 1, "a layer is created");
    check(m.create("Props").empty() && m.layers().size() == 1, "a duplicate name is refused");
    check(m.create("Kid", "Nope").empty(), "a missing parent is refused");
    check(m.create(" lead").empty() && m.create("trail ").empty(), "leading or trailing space is refused");
    check(m.freshName() == "Layer" && m.freshName("Props") == "Props 2", "freshName finds a free name");
    m.create("Day");
    m.setInitial("Day", fmt::OcDataLayerState::Loaded);
    m.create("Dawn", "Day");
    check(m.find("Dawn")->initial == fmt::OcDataLayerState::Loaded, "a child starts no higher than its parent");
}

static void testRenameDelete() {
    AVER_INFO("=== rename and delete ===");
    dl::Model m;
    m.create("A"); m.create("B", "A"); m.create("C", "B");
    m.assign({1, 2}, "A");
    m.assign({2, 3}, "B");
    m.setEditorVisible("A", false);

    dl::Edit r = m.rename("A", "Root");
    check(r.layersChanged && m.find("Root") && !m.find("A"), "rename changes the layer");
    check(m.find("B")->parent == "Root", "a child's parent follows the rename");
    check(m.layersOf(1) == dl::Names{"Root"} && m.layersOf(2) == (dl::Names{"Root", "B"}), "membership follows the rename");
    check(!m.flagsOf("Root").visible, "editor flags follow the rename");
    check(m.rename("Root", "B").empty() && m.rename("Nope", "X").empty() && m.rename("Root", "Root").empty(), "rename to a taken, missing or same name is refused");

    m.apply(r, true);
    check(m.find("A") && !m.find("Root") && m.layersOf(1) == dl::Names{"A"} && m.find("B")->parent == "A", "undo of a rename restores the lot");
    m.apply(r, false);
    check(m.find("Root") && m.layersOf(1) == dl::Names{"Root"}, "redo applies it again");

    dl::Edit d = m.remove("B");
    check(!m.find("B") && m.find("C")->parent == "Root", "delete moves the children up to the grandparent");
    check(m.layersOf(2) == dl::Names{"Root"} && m.layersOf(3).empty(), "delete strips the name from its members");
    m.apply(d, true);
    check(m.find("B") && m.find("C")->parent == "B" && m.layersOf(3) == dl::Names{"B"}, "undo of a delete restores layers and members");
    check(m.remove("Nope").empty(), "deleting a missing layer is a no-op");
}

static void testAssign() {
    AVER_INFO("=== assignment ===");
    dl::Model m;
    m.create("A"); m.create("B");
    dl::Edit a = m.assign({10, 11, 12}, "A");
    check(a.members.size() == 3 && m.membersOf("A") == (std::vector<dl::Key>{10, 11, 12}), "assign adds the selection");
    check(m.assign({10, 11}, "A").empty(), "assigning again changes nothing");
    check(m.assign({10}, "Nope").empty(), "assigning to a missing layer is refused");
    m.assign({10}, "B");
    check(m.layersOf(10) == (dl::Names{"A", "B"}), "an entity can be in two layers");
    dl::Edit u = m.unassign({10, 11, 99}, "A");
    check(u.members.size() == 2 && m.layersOf(10) == dl::Names{"B"} && m.layersOf(11).empty(), "unassign removes only the members");
    m.apply(u, true);
    check(m.layersOf(11) == dl::Names{"A"}, "undo of an unassign puts them back");
    check(m.memberCount("A") == 3, "memberCount counts members");
    m.forget(12);
    check(m.memberCount("A") == 2, "forget drops a destroyed entity");
}

static void testHierarchy() {
    AVER_INFO("=== parents ===");
    dl::Model m;
    m.create("A"); m.create("B", "A"); m.create("C", "B");
    check(m.setParent("A", "C").empty(), "a layer cannot move under its own descendant");
    check(m.setParent("A", "A").empty(), "nor under itself");
    check(m.setParent("C", "A").layersChanged && m.find("C")->parent == "A", "a legal reparent works");
    check(m.setParent("C", "").layersChanged && m.find("C")->parent.empty(), "an empty parent makes a root");
    check(m.descendants("A") == dl::Names{"B"}, "descendants lists the subtree");
    m.create("Z");
    m.setParent("C", "A");
    const auto rows = m.rows();
    check(rows.size() == 4 && rows[0].name == "A" && rows[0].depth == 0 && rows[1].name == "B" && rows[1].depth == 1 && rows[2].name == "C" && rows[2].depth == 1 && rows[3].name == "Z", "rows lists the tree depth-first");
    m.setParent("C", "B");
    m.setParent("C", "B");
    check(m.descendants("A") == (dl::Names{"B", "C"}), "descendants walks more than one level");
    dl::Edit s = m.setRuntime("A", true);
    check(s.layersChanged && m.find("A")->runtime && m.setRuntime("A", true).empty(), "runtime flag edit, and a no-op is empty");
    m.apply(s, true);
    check(!m.find("A")->runtime, "undo restores it");
}

static void testEditorFlags() {
    AVER_INFO("=== editor visible / loaded ===");
    dl::Model m;
    m.create("A"); m.create("B", "A"); m.create("Other");
    m.assign({1}, "B"); m.assign({2}, "Other"); m.assign({3}, "A"); m.assign({3}, "Other");
    check(!m.entityHidden(1) && m.entityLoaded(1), "everything starts shown and loaded");
    m.setEditorVisible("A", false);
    check(m.entityHidden(1) && m.entityHidden(3) && !m.entityHidden(2), "a hidden layer hides its children's members and its own");
    check(m.entityLoaded(1), "hiding does not unload");
    m.setEditorVisible("A", true);
    m.setEditorLoaded("A", false);
    check(!m.entityLoaded(1) && m.entityHidden(1) && m.entityLoaded(2), "an unloaded layer is unpickable and hidden down its subtree");
    m.setEditorLoaded("Missing", false);
    check(m.flagsOf("Missing").loaded, "a flag on a missing layer is ignored");
    m.remove("A");
    check(m.flagsOf("A").loaded, "deleting drops the layer's flags");
    check(m.allMembers() == (std::vector<dl::Key>{1, 2, 3}) && m.layersOf(3) == dl::Names{"Other"}, "allMembers lists keys still in a layer, and delete stripped A");
}

static void testLoad() {
    AVER_INFO("=== load ===");
    dl::Model m;
    fmt::OcDataLayer l;
    l.name = "Night"; l.runtime = true; l.initial = fmt::OcDataLayerState::Unloaded;
    m.load({l});
    m.setMembers(5, {"Night", "Ghost"});
    check(m.layers().size() == 1 && m.layersOf(5).size() == 2, "load keeps the records, and unknown member names survive for a lossless save");
    m.setMembers(5, {});
    check(m.layersOf(5).empty() && m.allMembers().empty(), "setMembers with nothing clears the row");
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("Data layer model");
    testCreate();
    testRenameDelete();
    testAssign();
    testHierarchy();
    testEditorFlags();
    testLoad();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
