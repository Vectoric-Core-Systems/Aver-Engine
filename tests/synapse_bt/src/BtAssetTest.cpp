// The behaviour-tree asset: byte and file round trip, compatibility both ways with plain .ocbt,
// corrupt-chunk refusal, structural edits that carry decorators, key renames and validation.
// Pure: no scene, no world.
#include "aver/synapse/BtAsset.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/formats/OcBt.hpp"

#include <filesystem>
#include <string>

using namespace aver;
using namespace aver::synapse;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static BtAsset richAsset() {
    BtAsset a = btAssetStarter();
    BbKeyDef v;
    v.name = "Home"; v.type = BbType::Vec3; v.defaultValue = BbValue::ofVec3(Vec3{1, 2, 3});
    v.description = "Where to return to";
    a.schema.add(v);
    BbKeyDef s;
    s.name = "Label"; s.type = BbType::String; s.defaultValue = BbValue::ofString("guard");
    a.schema.add(s);
    BbKeyDef f;
    f.name = "Fear"; f.type = BbType::Float; f.defaultValue = BbValue::ofFloat(0.25f);
    a.schema.add(f);
    BbKeyDef n;
    n.name = "Rounds"; n.type = BbType::Int; n.defaultValue = BbValue::ofInt(-9000000000LL);
    a.schema.add(n);

    BtDecorator d;
    d.key = "Fear";
    d.op = BbOp::Ge;
    d.value = BbValue::ofFloat(0.5f);
    d.abort = BtAbortMode::Self;
    a.nodes[4].decorators.push_back(d);
    BtDecorator e;
    e.key = "Label";
    e.op = BbOp::Ne;
    e.value = BbValue::ofString("civilian");
    a.nodes[4].decorators.push_back(e);
    a.nodes[2].comment = "face the intruder";
    return a;
}

int main() {
    AVER_INFO("BtAssetTest");
    const std::string dir = (std::filesystem::temp_directory_path() / "aver-bt-asset-test").string();

    {
        AVER_INFO("the starter and the byte round trip");
        const BtAsset a = btAssetStarter();
        check(a.valid(), "the starter tree is valid");
        check(a.validate().empty(), "and validates clean");

        const BtAsset rich = richAsset();
        std::vector<u8> bytes;
        std::string why;
        check(writeBtAsset(rich, bytes, &why), "an asset with a schema, decorators and comments writes: " + why);
        BtAsset back;
        check(parseBtAsset(bytes.data(), bytes.size(), back, &why), "and parses back: " + why);
        check(btAssetEqual(rich, back), "every field survives: schema, defaults, decorators, comments");
        check(back.schema.keys.size() == rich.schema.keys.size() &&
              back.schema.find("Rounds")->defaultValue.i == -9000000000LL, "64-bit defaults survive");
        check(back.nodes[4].decorators.size() == 2 && back.nodes[4].decorators[0].abort == BtAbortMode::Self,
              "decorators come back on the right node in order");
        check(back.nodes[2].comment == "face the intruder", "comments come back");

        std::vector<u8> again;
        writeBtAsset(back, again);
        check(again == bytes, "a second write is byte-identical");
    }

    {
        AVER_INFO("file round trip");
        const std::string path = dir + "/rich.ocbt";
        const BtAsset rich = richAsset();
        std::string why;
        check(saveBtAsset(path, rich, &why), "saves to disk: " + why);
        BtAsset back;
        check(loadBtAsset(path, back, &why), "loads from disk: " + why);
        check(btAssetEqual(rich, back), "the loaded asset equals the saved one");
        check(!loadBtAsset(dir + "/missing.ocbt", back, &why), "a missing file is refused");
    }

    {
        AVER_INFO("compatibility with plain .ocbt");
        fmt::OcBtData plain;
        fmt::OcBtNode root; root.kind = fmt::OcBtNodeKind::Sequence; root.parent = fmt::kOcBtNoParent;
        fmt::OcBtNode wait; wait.kind = fmt::OcBtNodeKind::Action; wait.parent = 0; wait.name = "Wait"; wait.params[0] = 3.0f;
        plain.nodes = {root, wait};
        std::vector<u8> oldBytes;
        fmt::writeOcBt(plain, oldBytes);
        BtAsset loaded;
        std::string why;
        check(parseBtAsset(oldBytes.data(), oldBytes.size(), loaded, &why), "a file written by the old writer loads: " + why);
        check(loaded.nodes.size() == 2 && loaded.schema.keys.empty() && loaded.nodes[1].decorators.empty(),
              "as an asset with no schema and no decorators");

        BtAsset plainAsset = BtAsset::fromOcBt(plain);
        std::vector<u8> bytes;
        writeBtAsset(plainAsset, bytes);
        check(bytes == oldBytes, "an asset with no extras writes exactly the old bytes");

        std::vector<u8> richBytes;
        writeBtAsset(richAsset(), richBytes);
        fmt::OcBtData viaOld;
        check(fmt::parseOcBt(richBytes.data(), richBytes.size(), viaOld, &why), "the OLD reader still loads a rich file: " + why);
        check(viaOld.nodes.size() == richAsset().nodes.size() && viaOld.valid(), "and sees the structural tree");
    }

    {
        AVER_INFO("corrupt extras are refused, not half-loaded");
        std::vector<u8> bytes;
        writeBtAsset(richAsset(), bytes);
        fmt::Avr1File file;
        fmt::parseAvr1(bytes.data(), bytes.size(), file);
        const u32 bdec = fmt::avrFourCC("BDEC");
        bool corrupted = false;
        for (fmt::AvrChunk& c : file.chunks)
            if (c.id == bdec && c.data.size() > 12) { c.data.resize(c.data.size() - 7); corrupted = true; }
        check(corrupted, "the fixture has a decorator chunk to damage");
        std::vector<u8> bad;
        fmt::writeAvr1(file, bad);
        BtAsset out;
        std::string why;
        check(!parseBtAsset(bad.data(), bad.size(), out, &why), "a truncated BDEC chunk fails the load");

        fmt::Avr1File file2;
        fmt::parseAvr1(bytes.data(), bytes.size(), file2);
        for (fmt::AvrChunk& c : file2.chunks)
            if (c.id == bdec) c.data[4] = 0x7F;   // first record's node index becomes huge
        std::vector<u8> bad2;
        fmt::writeAvr1(file2, bad2);
        check(!parseBtAsset(bad2.data(), bad2.size(), out, &why), "a decorator pointing past the node table fails the load");
    }

    {
        AVER_INFO("structural edits carry decorators and keep the tree valid");
        BtAsset a = richAsset();   // 0 Selector, 1 Sequence(deco), 2 LookAt, 3 Wait, 4 Wait(2 decos)
        const i32 added = btAssetAddChild(a, 1, fmt::OcBtNodeKind::Action, "MoveTo");
        check(added == 4 && a.valid(), "a new child lands last under its parent, in pre-order");
        check(a.nodes[5].decorators.size() == 2, "the decorated sibling that moved down kept its decorators");
        check(a.nodes[5].node.parent == 0, "and still hangs off the root");

        const i32 moved = btAssetMoveSibling(a, 5, -1);
        check(moved == 1 && a.valid(), "moving a sibling earlier reorders it");
        check(a.nodes[1].decorators.size() == 2 && a.nodes[2].decorators.size() == 1,
              "decorators moved with their nodes");
        check(!btAssetCanMoveSibling(a, 1, -1), "the first sibling cannot move earlier");
        check(btAssetMoveSibling(a, 1, -1) == -1, "and a refused move changes nothing");

        const i32 re = btAssetReparent(a, 1, 2);
        check(re >= 0 && a.valid(), "a node can be reparented under a sibling branch");
        check(btAssetReparent(a, 0, 1) == -1, "the root cannot be reparented");
        BtAsset b = a;
        i32 child = btAssetAddChild(b, 1, fmt::OcBtNodeKind::Sequence);
        check(btAssetReparent(b, 1, child) == -1, "a node cannot move under its own descendant");

        const usize before = a.nodes.size();
        const i32 parent = btAssetDeleteSubtree(a, 2);
        check(parent >= 0 && a.valid() && a.nodes.size() < before, "deleting a subtree removes it and its descendants");
        check(btAssetDeleteSubtree(a, 0) == -1, "the root cannot be deleted");

        BtAsset k = btAssetStarter();
        btAssetSetKind(k, 2, fmt::OcBtNodeKind::Inverter);
        check(k.valid() && k.nodes[2].node.name.empty(), "changing a leaf into a structural kind clears its name");
        btAssetSetKind(k, 2, fmt::OcBtNodeKind::Condition);
        check(k.valid() && !k.nodes[2].node.name.empty(), "and the reverse gives it one");
    }

    {
        AVER_INFO("key rename and validation");
        BtAsset a = btAssetStarter();
        a.nodes[4].node.kind = fmt::OcBtNodeKind::Condition;
        a.nodes[4].node.name = "BbCompare";
        a.nodes[4].node.stringParam = "Alert == true";
        check(a.validate().empty(), "a BbCompare on a known key validates");
        check(btAssetRenameKey(a, "Alert", "Alarm"), "a key can be renamed");
        check(a.schema.find("Alarm") && !a.schema.find("Alert"), "the schema follows");
        check(a.nodes[4].node.stringParam == "Alarm == true", "BbCompare text follows");
        check(!btAssetRenameKey(a, "Nope", "X") && !btAssetRenameKey(a, "Alarm", "Target"),
              "a missing source or a taken target is refused");
        btAssetRenameKey(a, "CanSeeTarget", "Visible");
        check(a.nodes[1].decorators[0].key == "Visible", "decorators follow");

        a.nodes[1].decorators[0].key = "Ghost";
        const auto problems = a.validate();
        check(problems.size() == 1 && problems[0].find("Ghost") != std::string::npos,
              "a decorator on an unknown key is reported");
        a.nodes[1].decorators[0].key = "Visible";
        a.nodes[1].decorators[0].op = BbOp::Eq;
        a.nodes[1].decorators[0].value = BbValue::ofString("x");
        check(a.validate().size() == 1, "a value that cannot convert to the key's type is reported");
    }

    {
        AVER_INFO("compiled form ticks");
        const BtAsset a = btAssetStarter();
        const BtRuntimeTree rt = compileBtAsset(a);
        check(rt.tree.valid() && rt.children.size() == a.nodes.size(), "the compiled tree has child lists");
        check(rt.decorators.byNode.size() == 1 && rt.decorators.find(1) != nullptr, "and the decorator table");

        BtRegistry reg;
        struct Fns {
            static BtStatus wait(i32, const fmt::OcBtNode&, f32, f32&, void*) { return BtStatus::Running; }
        };
        reg.registerAction("Wait", &Fns::wait);
        reg.registerAction("LookAt", &Fns::wait);
        Blackboard board;
        board.applySchema(rt.schema);
        BtRunningState st;
        const BtBlackboardCtx ctx{&rt.decorators, &board, &rt.children};
        tickBt(rt.tree, reg, 1, 0.1f, st, &ctx);
        check(st.runningChild.count(0) && st.runningChild[0] == 4, "no target visible: the idle branch runs");
        board.setBool("CanSeeTarget", true);
        tickBt(rt.tree, reg, 1, 0.1f, st, &ctx);
        check(st.runningChild[0] == 1 && st.abortCount == 1, "target seen: the starter's Both decorator pre-empts idle");
    }

    AVER_INFO(g_failures ? "BtAssetTest: {} FAILURES" : "BtAssetTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
