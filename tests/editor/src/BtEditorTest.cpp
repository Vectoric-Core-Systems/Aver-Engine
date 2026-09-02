// The .ocbt editor tab's headless core: the four structural edits (add / delete / reparent /
// reorder), and the tab's own load / save / dirty / undo bookkeeping on top of them.
//
// AVER_WITH_IMGUI IS DELIBERATELY UNDEFINED for this target -- BtEditor.cpp's `#include "imgui.h"`
// and its whole drawing half sit behind that macro, exactly as GraphEditor.cpp's do, so this
// genuinely exercises the part of the editor that is NOT gated behind a window. That split is the
// whole reason the structural edits are free functions rather than members (see BtEditor.hpp), and
// GraphEditorLoadSaveTest beside this file is the precedent for the arrangement.
#include "BtEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcBt.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Selector(0) -> [ Sequence(1) -> [ Condition"HasTarget"(2), Action"Wait"(3) ], Action"LookAt"(4) ]
static fmt::OcBtData fixture() {
    fmt::OcBtData t;
    fmt::OcBtNode root; root.kind = fmt::OcBtNodeKind::Selector; root.parent = fmt::kOcBtNoParent;
    t.nodes.push_back(root);
    fmt::OcBtNode seq; seq.kind = fmt::OcBtNodeKind::Sequence; seq.parent = 0;
    t.nodes.push_back(seq);
    fmt::OcBtNode cond; cond.kind = fmt::OcBtNodeKind::Condition; cond.parent = 1; cond.name = "HasTarget";
    t.nodes.push_back(cond);
    fmt::OcBtNode wait; wait.kind = fmt::OcBtNodeKind::Action; wait.parent = 1; wait.name = "Wait";
    wait.params[0] = 2.0f;
    t.nodes.push_back(wait);
    fmt::OcBtNode look; look.kind = fmt::OcBtNodeKind::Action; look.parent = 0; look.name = "LookAt";
    t.nodes.push_back(look);
    return t;
}

int main() {
    AVER_INFO("BtEditorTest");

    const std::string dir = (std::filesystem::temp_directory_path() / "aver-bt-editor").string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/fixture.ocbt";

    {
        const fmt::OcBtData seed = fixture();
        std::string why;
        check(seed.valid(), "the fixture tree is valid");
        check(fmt::saveOcBt(path, seed, &why), "the fixture writes to disk: " + why);
    }

    AVER_INFO("every structural edit keeps the tree valid -- the invariant holds by construction");
    {
        std::vector<fmt::OcBtNode> n = fixture().nodes;

        const i32 added = editor::btAddChild(n, 1, fmt::OcBtNodeKind::Cooldown);
        check(added > 0, "add returns the new node's index");
        fmt::OcBtData probe; probe.nodes = n;
        check(probe.valid(), "still valid after an add");
        check(editor::btChildrenOf(n, 1).size() == 3, "the Sequence has three children now");

        const i32 moved = editor::btMoveSibling(n, added, -1);
        check(moved >= 0, "the new node moves up among its siblings");
        probe.nodes = n;
        check(probe.valid(), "still valid after a reorder");

        const i32 reparented = editor::btReparent(n, moved, 0);
        check(reparented > 0, "and reparents onto the root");
        probe.nodes = n;
        check(probe.valid(), "still valid after a reparent");

        const i32 afterDelete = editor::btDeleteSubtree(n, reparented);
        check(afterDelete >= 0, "and deletes again");
        probe.nodes = n;
        check(probe.valid(), "still valid after a delete");
        check(n.size() == 5, "back to the five nodes it started with");
    }

    AVER_INFO("delete takes the WHOLE subtree, never orphaning a child");
    {
        std::vector<fmt::OcBtNode> n = fixture().nodes;
        // Deleting the Sequence must take its Condition and its Wait with it: leaving either behind
        // would produce a second parentless node, which OcBtData::valid() refuses to write at all.
        const i32 next = editor::btDeleteSubtree(n, 1);
        check(next == 0, "deleting the Sequence selects its parent, the root");
        check(n.size() == 2, "THREE nodes went, not one -- the Sequence and both of its children");
        fmt::OcBtData probe; probe.nodes = n;
        check(probe.valid(), "and what is left is a valid tree");
        check(n[1].name == "LookAt", "the root's OTHER child is untouched");
    }

    AVER_INFO("reparent refuses a cycle rather than producing an unloadable tree");
    {
        std::vector<fmt::OcBtNode> n = fixture().nodes;
        check(editor::btReparent(n, 1, 2) < 0,
              "a node REFUSES to be reparented onto its own child");
        check(editor::btReparent(n, 1, 1) < 0, "or onto itself");
        check(editor::btReparent(n, 0, 1) < 0, "and the root refuses to be reparented at all");
        fmt::OcBtData probe; probe.nodes = n;
        check(probe.valid() && n.size() == 5, "every refusal left the tree exactly as it was");
    }

    AVER_INFO("reorder changes EXECUTION order, and survives a save/load round trip");
    {
        editor::BtEditor ed(path);
        check(ed.loaded(), "the tab loads the fixture: " + ed.loadError());
        check(!ed.dirty(), "and starts clean");

        // The root Selector's children are [Sequence, LookAt]. A Selector runs them in order until
        // one succeeds, so swapping them is a real behaviour change, not cosmetics.
        const std::vector<i32> before = editor::btChildrenOf(ed.tree().nodes, 0);
        check(before.size() == 2 && ed.tree().nodes[static_cast<usize>(before[1])].name == "LookAt",
              "LookAt is the root's SECOND child to begin with");

        ed.select(before[1]);
        ed.moveSelected(-1);
        check(ed.dirty(), "moving a node marks the tab dirty");

        const std::vector<i32> after = editor::btChildrenOf(ed.tree().nodes, 0);
        check(after.size() == 2 && ed.tree().nodes[static_cast<usize>(after[0])].name == "LookAt",
              "LookAt is the root's FIRST child now");
        check(ed.selected() == after[0], "and the selection followed it across the rebuild");

        std::string why;
        check(ed.save(&why), "the tab saves: " + why);
        check(!ed.dirty(), "and is clean again afterward");

        fmt::OcBtData reloaded;
        check(fmt::loadOcBt(path, reloaded, &why), "the file reloads: " + why);
        const std::vector<i32> onDisk = editor::btChildrenOf(reloaded.nodes, 0);
        check(onDisk.size() == 2 && reloaded.nodes[static_cast<usize>(onDisk[0])].name == "LookAt",
              "AND THE NEW ORDER IS WHAT IS ACTUALLY ON DISK -- not just in the tab's own copy");
    }

    AVER_INFO("undo restores the previous tree, and redo puts the edit back");
    {
        editor::BtEditor ed(path);
        check(ed.loaded(), "the tab loads");
        const usize startCount = ed.tree().nodes.size();

        ed.select(0);
        ed.addChild(fmt::OcBtNodeKind::Inverter);
        check(ed.tree().nodes.size() == startCount + 1, "an add grows the tree");
        check(ed.canUndo(), "and there is something to undo");

        ed.undo();
        check(ed.tree().nodes.size() == startCount, "undo removes it again");
        check(ed.canRedo(), "and there is something to redo");

        ed.redo();
        check(ed.tree().nodes.size() == startCount + 1, "redo puts it back");
    }

    AVER_INFO("changing a node's kind keeps it SAVEABLE -- the name rule is enforced, not assumed");
    {
        editor::BtEditor ed(path);
        // OcBtData::valid() requires a node be named if and only if it is a Condition or an Action.
        // Flipping a kind without fixing the name would produce a tree that silently refused to
        // save -- the exact failure btSetKind exists to prevent.
        ed.select(0);   // the root Selector: a structural kind, so it carries no name
        ed.setSelectedKind(fmt::OcBtNodeKind::Action);
        check(!ed.tree().nodes[0].name.empty(), "becoming an Action GAVE it a default name");
        std::string why;
        check(ed.save(&why), "so it still saves: " + why);

        ed.setSelectedKind(fmt::OcBtNodeKind::Sequence);
        check(ed.tree().nodes[0].name.empty(), "and leaving those kinds CLEARED the name again");
        check(ed.save(&why), "so it still saves: " + why);
    }

    AVER_INFO("a tab with unsaved edits does not silently lose them when the file changes on disk");
    {
        editor::BtEditor ed(path);
        ed.select(0);
        ed.addChild(fmt::OcBtNodeKind::Succeeder);
        const usize edited = ed.tree().nodes.size();
        check(ed.dirty(), "the tab is dirty");

        ed.onFileChanged();
        check(ed.tree().nodes.size() == edited && ed.dirty(),
              "onFileChanged KEPT the unsaved edits rather than reloading over them");

        std::string why;
        check(ed.save(&why), "and they can still be saved: " + why);
        ed.onFileChanged();
        check(!ed.dirty(), "a CLEAN tab reloads instead, which is what the callback is for");
    }

    AVER_INFO("the factory claims .ocbt and nothing else");
    {
        check(editor::makeBtEditor(path) != nullptr, "it accepts a .ocbt");
        check(editor::makeBtEditor(dir + "/nope.ocgraph") == nullptr, "and declines a .ocgraph");
        check(editor::makeBtEditor(dir + "/nope.ocanim") == nullptr, "and a .ocanim");
    }

    // THE STARTER THE CONTENT BROWSER WRITES, CHECKED END TO END.
    //
    // "New Behaviour Tree" writes btStarterTree() and immediately opens the file in this editor. That
    // is the whole trap: BtEditor's constructor calls loadFromDisk() and, when the load fails, sets
    // loaded_ = false and shows an error instead of an editable tree. A starter that does not satisfy
    // OcBtData::valid() would therefore be WRITTEN SUCCESSFULLY and then refused by the tab opened for
    // it -- the create path reports "Created NewBehaviour.ocbt", and the failure surfaces one step
    // later as an editor that will not open its own new file.
    //
    // So this does exactly what the menu item does, in order, and asserts each step rather than the
    // last one only.
    AVER_INFO("the Content Browser's starter tree is valid, saves, loads, and opens");
    {
        const fmt::OcBtData starter = editor::btStarterTree();
        check(starter.valid(), "the starter satisfies OcBtData::valid(), which the loader enforces");
        check(starter.nodes.size() >= 2,
              "and is not a bare root -- a starter should show what the format is for, got " +
              std::to_string(starter.nodes.size()) + " node(s)");

        bool named = false;
        for (const fmt::OcBtNode& n : starter.nodes) {
            const bool leaf = n.kind == fmt::OcBtNodeKind::Condition || n.kind == fmt::OcBtNodeKind::Action;
            if (leaf && !n.name.empty()) named = true;
            check(!leaf || !n.name.empty(),
                  "every Condition/Action in it carries a registered name, so the tree resolves at load");
        }
        check(named, "and at least one leaf actually does something when ticked");

        const std::string starterPath = dir + "/starter.ocbt";
        std::string why;
        check(fmt::saveOcBt(starterPath, starter, &why), "it writes: " + why);

        fmt::OcBtData back;
        check(fmt::loadOcBt(starterPath, back, &why), "and loads back: " + why);
        check(back.nodes.size() == starter.nodes.size(),
              "with every node intact, got " + std::to_string(back.nodes.size()));

        // The check that actually mirrors the menu item: the tab opens it, rather than reporting a
        // load error into a dead panel.
        check(editor::makeBtEditor(starterPath) != nullptr,
              "and the editor the create path opens for it ACCEPTS it");
    }

    std::filesystem::remove_all(dir, ec);
    AVER_INFO(g_failures ? "BtEditorTest: {} FAILURES" : "BtEditorTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
