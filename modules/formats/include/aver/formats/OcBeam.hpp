#pragma once
// .ocbeam — breakable, deformable soft-body "cage": a mass-spring lattice of NODEs, BEAMs and
// PANELs for a vehicle. See docs/recon/ocbeam.md.
//
// NOT A SKELETAL MESH, though this line used to say so in passing and the phrase misleads. A
// skeletal mesh in this engine is a .ocmesh carrying JOINTS/WEIGHTS streams (kOcMeshHasSkin) plus a
// .ocskel skeleton -- that is what CSkeletalMesh names and what SkinnedScene refuses to skin
// without (SkinnedScene.cpp: "has no skin streams"). This format shares none of that path: it is
// read only by the editor's --beam viewer, and its optional BONE/SKIN/ANIM rows are SKIPPED by the
// parser below (hasRig merely records that they were present).
// Engine space: cm, X fwd / Y right / Z up.
#include "aver/core/Types.hpp"
#include "aver/assets/AssetId.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// What a material does when its beams are pushed past their limits.
enum class BeamBehavior { Deform, Fracture, Shatter };
// Name of a beam behaviour, for display.
const char* beamBehaviorName(BeamBehavior b);

// One MATERIAL row: the stiffness, yield and break characteristics of a set of beams.
struct OcBeamMaterial {
    std::string name;
    f32 stiffness = 0.7f;        // 0..1 PBD relaxation feel
    f32 axialStiffness = 3500;   // N/cm
    f32 bendForceN = 3000;       // N — yield onset
    f32 breakForceN = 12000;     // N — catastrophic snap
    f32 plasticStiffness = 800;  // N/cm once yielded
    f32 maxBend = 8;             // cm before tear
    f32 bendAbsorb = 0.3f;       // 0..1
    f32 breakAbsorb = 0.6f;      // 0..1
    BeamBehavior behavior = BeamBehavior::Deform;
    f32 tearStrainTension = -1.0f;      // -1 = unset
    f32 tearStrainCompression = -1.0f;  // -1 = unset
    f32 density = 1.0f;                  // relative node mass
};

// One NODE row: a point mass of the cage.
struct OcBeamNode { i32 id = 0; f32 x = 0, y = 0, z = 0; };      // cm, vehicle-local
// One BEAM row: a link between two nodes.
struct OcBeamBeam { i32 id = 0; i32 nodeA = 0, nodeB = 0; };     // references NODE ids
// One PANEL row: a triangle of three beams.
struct OcBeamPanel {
    i32 id = 0; i32 beamA = 0, beamB = 0, beamC = 0;
    std::string materialOverride;                               // empty = use part material
};
// One PART row: a named, detachable group of panels with its own mesh and material.
struct OcBeamPart {
    std::string name, role, material, meshName;
    f32 detach = 0.3f;               // fraction of beams broken before the panel sheds
    std::vector<i32> panels;         // references PANEL ids
};

// A whole parsed .ocbeam.
struct OcBeamData {
    int version = 1;                 // OCBEAM <n> header (ignored/tolerated)
    ObjectId objectId = kInvalidObjectId;
    f32 rebound = -1.0f;             // -1 = unset (keep default restitution)
    f32 importScale = 1.0f;          // effective SCALE/NORMALIZE factor applied to nodes
    bool hasEmbeddedGlb = false;     // GLB{} present (skipped here)
    bool hasRig = false;             // BONE/SKIN/ANIM present (skipped here)
    bool hasCollision = false;       // COLLISION{} present (skipped here)

    std::vector<OcBeamMaterial> materials;
    std::vector<OcBeamNode> nodes;
    std::vector<OcBeamBeam> beams;
    std::vector<OcBeamPanel> panels;
    std::vector<OcBeamPart> parts;

    u32 skippedRows = 0;             // malformed rows tolerated during the parse
};

// Parses an .ocbeam from memory. Returns false only when the file has no NODE rows.
bool parseOcbeam(std::string_view text, OcBeamData& out, std::string* err = nullptr);

// Loads an .ocbeam from disk, deriving objectId from the filename when OBJECTID is absent.
bool loadOcbeam(const std::string& path, OcBeamData& out, std::string* err = nullptr);

} // namespace aver::fmt
