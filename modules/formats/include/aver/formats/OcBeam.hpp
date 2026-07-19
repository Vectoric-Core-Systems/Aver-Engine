#pragma once
// .ocbeam — breakable, deformable soft-body "cage" (skeletal mesh). Faithful to the
// authoritative writer (OCCompiler Main.java) and runtime parser (VehicleDamage.cpp
// ParseOcbeam). See docs/recon/ocbeam.md. Engine space: cm, X fwd / Y right / Z up.
//
// Aver addition (per project direction): a single OBJECTID giving every object a
// unique id. Optional in the file; derived from the filename when absent.
#include "aver/core/Types.hpp"
#include "aver/assets/AssetId.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

enum class BeamBehavior { Deform, Fracture, Shatter };
const char* beamBehaviorName(BeamBehavior b);

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
    // Optional fields 10-12 present in newer 13-field files (e.g. Puegot). The OC
    // runtime solver ignores these, but we preserve them so no data is lost.
    f32 tearStrainTension = -1.0f;      // -1 = unset
    f32 tearStrainCompression = -1.0f;  // -1 = unset
    f32 density = 1.0f;                  // relative node mass
};

struct OcBeamNode { i32 id = 0; f32 x = 0, y = 0, z = 0; };      // cm, vehicle-local
struct OcBeamBeam { i32 id = 0; i32 nodeA = 0, nodeB = 0; };     // references NODE ids
struct OcBeamPanel {                                             // triangle of 3 beams
    i32 id = 0; i32 beamA = 0, beamB = 0, beamC = 0;
    std::string materialOverride;                               // empty = use part material
};
struct OcBeamPart {
    std::string name, role, material, meshName;
    f32 detach = 0.3f;               // fraction of beams broken before the panel sheds
    std::vector<i32> panels;         // references PANEL ids
};

struct OcBeamData {
    int version = 1;                 // OCBEAM <n> header (ignored/tolerated)
    ObjectId objectId = kInvalidObjectId;
    f32 rebound = -1.0f;             // -1 = unset (keep default restitution)
    f32 importScale = 1.0f;          // effective SCALE/NORMALIZE factor applied to nodes
    bool hasEmbeddedGlb = false;     // GLB{} present (extracted at import; skipped here)
    bool hasRig = false;             // BONE/SKIN/ANIM present (skipped here)
    bool hasCollision = false;       // COLLISION{} present (skipped here)

    std::vector<OcBeamMaterial> materials;
    std::vector<OcBeamNode> nodes;
    std::vector<OcBeamBeam> beams;
    std::vector<OcBeamPanel> panels;
    std::vector<OcBeamPart> parts;

    // Parse diagnostics (non-fatal skipped rows, mirroring the tolerant runtime).
    u32 skippedRows = 0;
};

// Parse from an in-memory text buffer. Returns false only on a structural failure
// (no NODE rows — the runtime's single success condition). `err` gets a message.
bool parseOcbeam(std::string_view text, OcBeamData& out, std::string* err = nullptr);

// Load from disk; also derives objectId from the filename if the file omits OBJECTID.
bool loadOcbeam(const std::string& path, OcBeamData& out, std::string* err = nullptr);

} // namespace aver::fmt
