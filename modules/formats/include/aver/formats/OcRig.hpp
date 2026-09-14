#pragma once
// .ocrig -- a control rig: a short list of operations that edit a posed skeleton every frame.
//
// A rig is what makes an animation react. The clip says where the arm swings; the rig says the hand
// stays on the grip while it does, or the foot sits on the slope actually under it. It runs in
// AnimSystem's pose-modifier seam, between sampling the clip and computing skinning matrices.
//
// TEXT, NOT AN AVR1 CONTAINER, and that is a deliberate break from .ocbt next door. A behaviour tree
// is BAKED by a tool; a rig is short, hand-authorable and read far more often than it is written --
// four ops is a whole rig, where a .ocmesh is a megabyte of vertices. .ocgraph made the same call for
// the same reason, and it is the one that lets the demo rig in a project be opened and understood in
// a text editor rather than requiring a tool to exist first.
//
// BONES ARE NAMED HERE AND RESOLVED TO INDICES AT LOAD, exactly as OcSocket::bone is. A rig that
// stored indices would break the moment a skeleton gained a bone, and skeletons gain bones.
//
// ---------------------------------------------------------------------------------------------
// OCRIG 1
// NAME ArmReach
// # everything after a # is a comment
// OP twobone root=shoulder mid=elbow tip=wrist goal=80,0,0 pole=0,-100,20 weight=1
// OP aim bone=head at=0,200,150 axis=0,0,1 weight=0.5
// ---------------------------------------------------------------------------------------------
//
// EVERY OP CARRIES ITS OWN WEIGHT, and this is the format's one genuinely open question. The scout
// (docs/recon/control-rig.md) declined to decide whether a rig LAYERS on the sampled pose or
// OVERWRITES it, on the grounds that the answer wants a real rig in hand: `addPose` argues for
// layering, a planted foot argues for overwriting. Per-op weight is how this file avoids choosing --
// weight 1 overwrites that op's bones, weight 0 leaves the clip alone, and between them is a blend.
// **Decided without a real rig to test it against**, and cheap to revisit while nothing has authored
// one.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

enum class OcRigOpKind : u32 {
    // Bends a three-joint chain so `tip` reaches `goal`. `pole` picks which way it bends -- see
    // anim::twoBoneIk, which this is the authored form of.
    TwoBoneIk = 0,
    // Turns one bone so its `axis` points at `target`. A head that watches something, a turret.
    AimAt = 1,
};

// One operation. Which of the fields matter depends on `kind`, and `valid()` below is what says so
// rather than a reader guessing -- an op naming bones it does not use is a rig that will confuse the
// next person to open it.
struct OcRigOp {
    OcRigOpKind kind = OcRigOpKind::TwoBoneIk;

    // TwoBoneIk uses all three; AimAt uses `root` only, as the bone it turns.
    std::string root, mid, tip;

    // TwoBoneIk: the goal, in the skeleton's own model space, centimetres.
    // AimAt:     the point to look at, same space.
    Vec3 target{0, 0, 0};

    // TwoBoneIk: the pole that picks the bend direction.
    // AimAt:     the bone-local axis that should end up pointing at the target.
    Vec3 hint{0, 0, 1};

    // 0 leaves the sampled pose alone, 1 applies the op fully. See the header note on why this is
    // per-op rather than a property of the rig.
    f32 weight = 1.0f;
};

struct OcRigData {
    std::string name;
    std::vector<OcRigOp> ops;

    // Every op names the bones its kind needs, no name is empty, and every weight is in [0,1].
    // Does NOT check that the bones exist -- a rig is validated against a FILE here and against a
    // SKELETON at load, and conflating the two would make a rig unopenable without its skeleton.
    bool valid() const;
};

// Parses `text`. False on a malformed line, with `err` set to what and where.
bool parseOcRig(std::string_view text, OcRigData& out, std::string* err = nullptr);

// Reads `path`. False if it cannot be opened or does not parse.
bool loadOcRig(const std::string& path, OcRigData& out, std::string* err = nullptr);

// Serialises to the text form above. Round-trips through parseOcRig.
std::string writeOcRig(const OcRigData& rig);

// Writes `path`. False on an I/O failure.
bool saveOcRig(const std::string& path, const OcRigData& rig, std::string* err = nullptr);

} // namespace aver::fmt
