#pragma once
// .ocbt -- a baked Synapse behaviour tree, as an AVR1 container.
//
// Node records with a kind, a parent index and generic parameters; children are found by scanning
// for nodes whose OWN parent equals a given index (no explicit child-index list is stored -- the
// parent pointer alone is enough, exactly as .ocskel's OcBone::parent is the only structural field
// a bone needs). PARENTS BEFORE CHILDREN in file order, the same ordering contract
// OcSkeleton::valid() already enforces (modules/formats/src/OcAnim.cpp) -- verified here the
// identical way: a parent's index must be strictly less than its own child's index.
//
// The tree's ROOT is always node 0. A behaviour tree has exactly one entry point (unlike a
// skeleton, which can have several root bones for disconnected rigs), so there is no separate
// "rootIndex" field to keep in agreement with the array -- node 0 simply IS the root, and
// OcBtData::valid() enforces that node 0 is the only node with no parent.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

inline constexpr i32 kOcBtNoParent = -1;

// Pinned to Aver.Synapse's own BtStatus/BtNodeKind (modules/synapse/include/aver/synapse/Bt.hpp) --
// this header stays in Aver.Formats and does not itself depend on Aver.Synapse, so the two are kept
// in agreement by a static_assert on each side rather than by one including the other. See that
// header's own comment for why the evaluator lives one tier up instead of here.
enum class OcBtNodeKind : u32 {
    Selector  = 0,   // first child to Succeed wins; Failure only once every child fails
    Sequence  = 1,   // first child to Fail wins; Success only once every child succeeds
    Parallel  = 2,   // every child ticked every tick; Success once ALL succeed, Failure if ANY fails
    Inverter  = 3,   // single child; Success<->Failure swapped, Running passes through
    Succeeder = 4,   // single child; always reports Success once resolved (Running still passes through)
    Cooldown  = 5,   // single child; refuses to re-run for params[0] seconds after a resolved outcome
    Condition = 6,   // a leaf; resolved BY NAME through the host's registry, Success or Failure only
    Action    = 7,   // a leaf; resolved BY NAME through the host's registry, may itself report Running
};

// One node. Structural kinds (Selector/Sequence/Parallel/Inverter/Succeeder) carry no params and no
// name. Cooldown, Condition and Action each interpret `params`/`name` their own way -- documented on
// the kind's own line above and, for the seven built-in Condition/Action names, on
// modules/synapse.scene/include/aver/synapse/SynapseBt.hpp's own registration call.
struct OcBtNode {
    OcBtNodeKind kind = OcBtNodeKind::Selector;
    i32 parent = kOcBtNoParent;   // index into OcBtData::nodes; -1 ONLY for node 0, the root
    // Condition/Action's registered name (e.g. "HasTarget", "MoveTo"); empty for every other kind.
    std::string name;
    // Generic float parameters. Cooldown: params[0] = durationSec. Everything else is Condition/
    // Action-specific -- e.g. the built-in "DistanceToTargetLess" reads params[0] as a threshold in
    // centimetres, "Wait" reads params[0] as a duration in seconds, "MoveTo" reads params[0..2] as a
    // world-space goal. Unused slots are 0.
    f32 params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // A single string parameter, for the one built-in that needs one and floats cannot hold: the
    // built-in "FireEvent" action reads this as the event NAME to raise (distinct from `name`
    // above, which is "FireEvent" itself -- the action being invoked, not its argument). Empty for
    // every node that does not need one.
    std::string stringParam;
};

struct OcBtData {
    std::vector<OcBtNode> nodes;   // node 0 is the root; every parent precedes its children

    // node 0 exists and has no parent; every OTHER node's parent is a valid, EARLIER index;
    // Condition/Action nodes carry a name and every other kind does not.
    bool valid() const;
};

bool loadOcBt(const std::string& path, OcBtData& out, std::string* why = nullptr);
bool saveOcBt(const std::string& path, const OcBtData& in, std::string* why = nullptr);
bool parseOcBt(const u8* bytes, usize size, OcBtData& out, std::string* why = nullptr);
bool writeOcBt(const OcBtData& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
