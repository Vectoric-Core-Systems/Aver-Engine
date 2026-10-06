#pragma once
// The behaviour-tree asset: a .ocbt that also carries the blackboard schema, per-node decorators and
// editor comments. The file is still an OCBT container whose BNOD/STRT chunks are exactly what older
// readers (fmt::loadOcBt, BtEditor) parse; the extras ride in separate chunks they ignore:
//   BBSC  blackboard schema        BDEC  decorators        BNEX  per-node extras (comment)
// A plain .ocbt loads as an asset with an empty schema and no decorators. Design: docs/BLACKBOARD_BT.md.
#include "aver/formats/OcBt.hpp"
#include "aver/synapse/Bt.hpp"

#include <string>
#include <vector>

namespace aver::synapse {

struct BtAssetNode {
    fmt::OcBtNode node;
    std::string   comment;
    std::vector<BtDecorator> decorators;
};

struct BtAsset {
    BbSchema schema;
    std::vector<BtAssetNode> nodes;   // node 0 is the root, parents precede children (OcBtData rules)

    fmt::OcBtData  toOcBt() const;
    BtDecoratorSet decoratorSet() const;
    bool valid() const { return toOcBt().valid(); }
    // Human-readable problems that do not stop the tree from running (unknown keys, type mismatches,
    // unparsable BbCompare/BbSet text). Empty when clean.
    std::vector<std::string> validate() const;

    static BtAsset fromOcBt(const fmt::OcBtData& data);
};

bool btAssetEqual(const BtAsset& a, const BtAsset& b);

bool writeBtAsset(const BtAsset& in, std::vector<u8>& out, std::string* why = nullptr);
bool parseBtAsset(const u8* bytes, usize size, BtAsset& out, std::string* why = nullptr);
bool saveBtAsset(const std::string& path, const BtAsset& in, std::string* why = nullptr);
bool loadBtAsset(const std::string& path, BtAsset& out, std::string* why = nullptr);

// What the runtime ticks: the structural tree, decorators, schema and precomputed child lists.
struct BtRuntimeTree {
    fmt::OcBtData  tree;
    BtDecoratorSet decorators;
    BbSchema       schema;
    std::vector<std::vector<i32>> children;
    std::string    path;
};
BtRuntimeTree compileBtAsset(const BtAsset& asset);

// ---- structural edits ---------------------------------------------------------------------------
// Each rebuilds the array in pre-order, so "parents before children" holds by construction. Indices
// move across a call; each returns the new index of what it acted on (-1 on refusal).

const char* btNodeKindName(fmt::OcBtNodeKind kind);
std::vector<i32> btAssetChildren(const BtAsset& a, i32 parent);

// Condition/Action nodes get `name` (or "HasTarget" / "Wait" when empty) so the tree stays valid.
i32  btAssetAddChild(BtAsset& a, i32 parent, fmt::OcBtNodeKind kind, const std::string& name = {});
// Removes the node and its subtree; returns the parent's new index. -1 for the root.
i32  btAssetDeleteSubtree(BtAsset& a, i32 index);
// -1 when it would create a cycle or `index` is the root.
i32  btAssetReparent(BtAsset& a, i32 index, i32 newParent);
bool btAssetCanMoveSibling(const BtAsset& a, i32 index, i32 delta);
i32  btAssetMoveSibling(BtAsset& a, i32 index, i32 delta);
void btAssetSetKind(BtAsset& a, i32 index, fmt::OcBtNodeKind kind);
// Renames a key in the schema, in every decorator and in BbCompare/BbSet/BbClear text. False when
// `from` is missing or `to` is empty/taken.
bool btAssetRenameKey(BtAsset& a, const std::string& from, const std::string& to);

// The tree "New Behaviour Tree" writes: a Selector over a decorated Sequence and an idle Wait.
BtAsset btAssetStarter();

} // namespace aver::synapse
