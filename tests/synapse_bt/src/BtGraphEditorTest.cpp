// The behaviour-tree editor tab's headless core (layout, labels, edit bookkeeping, undo, save/load)
// and the AI debug overlay's pure parts (collection, line conversion, projection). AVER_WITH_IMGUI
// is undefined here, as in tests/editor's BtEditorTest, so no window is needed.
#include "AiDebugOverlay.hpp"
#include "BtGraphEditor.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static int g_providerCalls = 0;
static void provider(synapse::AiDebugSink& sink, void*) {
    ++g_providerCalls;
    sink.line(Vec3{0, 0, 0}, Vec3{0, 0, 100}, synapse::aiRgba(255, 128, 0), synapse::kAiDebugHearing);
}

int main() {
    AVER_INFO("BtGraphEditorTest");
    const std::string dir = (std::filesystem::temp_directory_path() / "aver-bt-graph-editor-test").string();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const std::string path = dir + "/guard.ocbt";

    {
        AVER_INFO("layout");
        const synapse::BtAsset a = synapse::btAssetStarter();   // 0 Selector, 1 Sequence(deco+comment), 2, 3 under 1, 4 under 0
        const BtLayoutParams p;
        const std::vector<BtNodeBox> boxes = btLayout(a, p);
        check(boxes.size() == a.nodes.size(), "one box per node");
        check(boxes[0].y < boxes[1].y && boxes[1].y < boxes[2].y, "children sit below their parent");
        check(boxes[1].y == boxes[4].y, "siblings share a row");
        check(boxes[2].y == boxes[3].y, "so do cousins at the same depth");
        check(std::fabs((boxes[1].x + boxes[4].x) * 0.5f - boxes[0].x) < 0.01f, "a parent is centred over its first and last child");
        check(std::fabs((boxes[2].x + boxes[3].x) * 0.5f - boxes[1].x) < 0.01f, "at every level");
        check(boxes[2].x + boxes[2].w <= boxes[3].x && boxes[3].x + boxes[3].w <= boxes[4].x,
              "leaves never overlap and keep tree order left to right");
        check(boxes[1].h > boxes[4].h, "a node with a decorator and a comment is taller");
        check(boxes[2].y >= boxes[1].y + boxes[1].h, "the next row starts below the tallest node of the row above");

        const BtNodeBox bounds = btLayoutBounds(boxes);
        bool inside = true;
        for (const BtNodeBox& b : boxes)
            if (b.x < bounds.x || b.y < bounds.y || b.x + b.w > bounds.x + bounds.w + 0.01f || b.y + b.h > bounds.y + bounds.h + 0.01f) inside = false;
        check(inside, "the bounds contain every box");
        check(btLayoutBounds({}).w == 0.0f, "an empty layout has empty bounds");

        check(btHitTest(boxes, boxes[3].x + 3.0f, boxes[3].y + 3.0f) == 3, "a point inside a node hits it");
        check(btHitTest(boxes, -500.0f, -500.0f) == -1, "a point outside every node hits nothing");
        check(btLayout(synapse::BtAsset{}, p).empty(), "an empty asset lays out to nothing");
    }

    {
        AVER_INFO("labels");
        synapse::BtAsset a = synapse::btAssetStarter();
        check(btNodeTitle(a.nodes[0]) == "Selector", "a structural node says its kind");
        check(btNodeTitle(a.nodes[4]) == "Action  Wait (2s)", "Wait shows its duration");
        synapse::BtAssetNode cmp;
        cmp.node.kind = fmt::OcBtNodeKind::Condition;
        cmp.node.name = "BbCompare";
        cmp.node.stringParam = "Alert == true";
        check(btNodeTitle(cmp) == "Condition  BbCompare: Alert == true", "a BbCompare shows its expression");
        check(btDecoratorText(a.nodes[1].decorators[0]) == "CanSeeTarget is set  [Both]", "decorator text names key, test and abort mode");
        synapse::BtDecorator d;
        d.key = "Ammo"; d.op = synapse::BbOp::Lt; d.value = synapse::BbValue::ofInt(3);
        check(btDecoratorText(d) == "Ammo < 3", "a comparison decorator shows its operand and no abort tag");
    }

    {
        AVER_INFO("a new asset opens");
        std::string why;
        check(btGraphWriteStarter(path, &why), "the starter writes: " + why);
        BtGraphEditor ed(path);
        check(ed.loaded() && ed.asset().valid() && !ed.dirty(), "and the tab loads it clean");
        check(ed.title().find("guard.ocbt") != std::string::npos, "the title carries the file name");
        check(ed.path() == path, "and the path is the identity");

        AVER_INFO("edits, undo and redo");
        const usize base = ed.asset().nodes.size();
        ed.select(0);
        check(ed.addChild(fmt::OcBtNodeKind::Action, "MoveTo") && ed.asset().nodes.size() == base + 1, "add child");
        check(ed.dirty() && ed.asset().nodes[static_cast<usize>(ed.selected())].node.name == "MoveTo", "it is selected and the tab is dirty");
        ed.undo();
        check(ed.asset().nodes.size() == base, "undo removes it");
        ed.redo();
        check(ed.asset().nodes.size() == base + 1, "redo brings it back");
        ed.select(static_cast<i32>(ed.asset().nodes.size()) - 1);
        check(ed.deleteSelected() && ed.asset().nodes.size() == base, "delete removes the subtree");
        ed.select(0);
        check(!ed.deleteSelected(), "the root cannot be deleted");

        ed.select(1);
        check(ed.addDecorator() && ed.asset().nodes[1].decorators.size() == 2, "add decorator");
        synapse::BtDecorator d = ed.asset().nodes[1].decorators[1];
        d.key = "Alert"; d.op = synapse::BbOp::NotSet; d.abort = synapse::BtAbortMode::Self;
        check(ed.setDecorator(1, d) && ed.asset().nodes[1].decorators[1].abort == synapse::BtAbortMode::Self, "set decorator");
        check(ed.removeDecorator(1) && ed.asset().nodes[1].decorators.size() == 1, "remove decorator");
        check(!ed.removeDecorator(9), "an out-of-range decorator is refused");
        ed.undo();
        check(ed.asset().nodes[1].decorators.size() == 2, "undo restores a removed decorator");

        check(ed.setNodeComment("hello") && ed.asset().nodes[1].comment == "hello", "comment");
        check(!ed.setNodeComment("hello"), "setting the same comment is not an edit");
        ed.select(4);
        check(ed.setNodeParam(0, 5.0f) && ed.asset().nodes[4].node.params[0] == 5.0f, "node parameter");
        check(!ed.setNodeParam(7, 1.0f), "a bad parameter slot is refused");
        check(ed.setNodeName("LookAt") && ed.asset().nodes[4].node.name == "LookAt", "leaf name");
        check(!ed.setNodeName(""), "an empty leaf name is refused");
        ed.select(0);
        check(!ed.setNodeName("X"), "a structural node has no name");
        check(ed.setSelectedKind(fmt::OcBtNodeKind::Sequence), "kind change");
        check(ed.asset().valid(), "and the tree stays valid");

        AVER_INFO("blackboard keys");
        synapse::BbKeyDef k;
        k.name = "Fear"; k.type = synapse::BbType::Float; k.defaultValue = synapse::BbValue::ofFloat(0.5f);
        check(ed.addKey(k) && ed.asset().schema.find("Fear"), "add key");
        check(!ed.addKey(k), "a duplicate key is refused");
        synapse::BbKeyDef changed = *ed.asset().schema.find("Fear");
        changed.type = synapse::BbType::Int;
        changed.defaultValue = synapse::BbValue::ofFloat(7.9f);
        check(ed.setKey("Fear", changed) && ed.asset().schema.find("Fear")->type == synapse::BbType::Int &&
              ed.asset().schema.find("Fear")->defaultValue.i == 7, "changing a key's type converts its default");
        check(ed.renameKey("Fear", "Dread") && ed.asset().schema.find("Dread") && !ed.asset().schema.find("Fear"), "rename key");
        check(ed.removeKey("Dread") && !ed.asset().schema.find("Dread"), "remove key");

        AVER_INFO("save and reload");
        ed.select(1);
        ed.setNodeComment("saved comment");
        check(ed.dirty(), "the tab is dirty before saving");
        check(ed.save(&why) && !ed.dirty(), "save clears the dirty flag: " + why);
        synapse::BtAsset onDisk;
        check(synapse::loadBtAsset(path, onDisk, &why), "the file loads: " + why);
        check(synapse::btAssetEqual(onDisk, ed.asset()), "and equals what the tab holds");

        AVER_INFO("a changed file");
        ed.select(1);
        ed.setNodeComment("unsaved");
        synapse::BtAsset other = synapse::btAssetStarter();
        other.nodes[0].comment = "from a tool";
        synapse::saveBtAsset(path, other);
        ed.onFileChanged();
        check(ed.dirty() && ed.asset().nodes[1].comment == "unsaved", "a dirty tab keeps its edits");
        ed.save(&why);
        synapse::saveBtAsset(path, other);
        ed.onFileChanged();
        check(!ed.dirty() && ed.asset().nodes[0].comment == "from a tool", "a clean tab reloads");
    }

    {
        AVER_INFO("failures");
        BtGraphEditor missing(dir + "/nope.ocbt");
        std::string why;
        check(!missing.loaded() && !missing.loadError().empty(), "a missing file loads as an error");
        check(!missing.save(&why) && !why.empty(), "and refuses to save");
        check(!missing.addChild(fmt::OcBtNodeKind::Sequence), "and refuses edits");

        check(makeBtGraphEditor(dir + "/x.ocbt") != nullptr, "the factory claims .ocbt");
        check(makeBtGraphEditor(dir + "/x.OCBT") != nullptr, "case-insensitively");
        check(makeBtGraphEditor(dir + "/x.ocgraph") == nullptr, "and nothing else");
    }

    {
        AVER_INFO("AI debug overlay");
        AiDebugOptions opts;
        synapse::AiDebugSink sink;
        sink.line(Vec3{}, Vec3{1, 0, 0}, 0xFFFFFFFFu, synapse::kAiDebugAll);
        aiDebugCollect(opts, sink);
        check(sink.lines().empty() && sink.enabled() == 0, "with no option on, collection only clears");

        opts.hearing = true;
        opts.btState = true;
        check(opts.mask() == (synapse::kAiDebugHearing | synapse::kAiDebugBtState), "options map to categories");
        sink.addProvider(&provider);
        aiDebugCollect(opts, sink);
        check(g_providerCalls == 1 && sink.lines().size() == 1, "an enabled option runs the providers");
        opts.hearing = false;
        aiDebugCollect(opts, sink);
        check(sink.lines().empty(), "a provider's lines in a disabled category are dropped");

        opts.hearing = true;
        aiDebugCollect(opts, sink);
        const std::vector<rhi::LineVertex> verts = aiDebugLineVertices(sink);
        check(verts.size() == 2, "a line becomes two vertices");
        check(verts[1].pz == 100.0f && std::fabs(verts[0].r - 1.0f) < 0.01f && std::fabs(verts[0].g - 128.0f / 255.0f) < 0.01f,
              "positions and 0..1 colours carry over");

        f32 sx = 0, sy = 0;
        // A matrix with w = 1: NDC (0,0) maps to the viewport centre, +y goes up on screen.
        Mat4 m = Mat4::identity();
        check(aiDebugProject(m, Vec3{0, 0, 0}, 100, 50, 800, 600, sx, sy) && std::fabs(sx - 500.0f) < 0.01f && std::fabs(sy - 350.0f) < 0.01f,
              "the origin projects to the viewport centre");
        check(aiDebugProject(m, Vec3{1, 1, 0}, 100, 50, 800, 600, sx, sy) && std::fabs(sx - 900.0f) < 0.01f && std::fabs(sy - 50.0f) < 0.01f,
              "NDC (1,1) is the top-right corner (y down)");
        m.m[3][3] = -1.0f;
        check(!aiDebugProject(m, Vec3{0, 0, 0}, 0, 0, 800, 600, sx, sy), "a point behind the eye is not projected");
    }

    std::filesystem::remove_all(dir, ec);
    AVER_INFO(g_failures ? "BtGraphEditorTest: {} FAILURES" : "BtGraphEditorTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
