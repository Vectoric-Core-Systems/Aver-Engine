// .ocsoft: the soft-body asset (JSON, "format":"ocsoft", "version":1), and the paint data in it.
//
// A cloth or a destructible, resolved from a preset plus overrides. Pure text and arithmetic: no
// scene, no GPU, no Jolt. See docs/SOFTBODY.md section ".ocsoft" for the schema.
#pragma once

#include "aver/softbody/Caps.hpp"
#include "aver/softbody/Material.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::softbody {

inline constexpr u32 kSoftAssetVersion = 1;

// Jelly and Rope (wave 11, docs/plans/wave11/SOFTBODY_HAIR.md) are simulated by SoftShapeSystem, not SoftBodySystem.
enum class SoftKind : u8 { Cloth = 0, Destructible = 1, Jelly = 2, Rope = 3 };

struct SimParams {
    u32 iterations = 6;
    f32 damping = 0.3f;
    f32 vertexRadiusCm = 0.4f;
    f32 maxSpeedCmS = 2500.0f;
    f32 gravityScale = 1.0f;
    f32 weldCm = 0.01f;
    u32 maxParticles = 4096;
    bool allowSleep = true;
};

// Wave 11. Cage is the wave 9 path (beams and dihedral bends from the mesh: dents, tears). Fast lets Jolt make its own edges,
// shear edges, bends and long range attachments (aver_phys_sbx_builder_auto): the cheap path for flags, curtains and capes,
// with no dents, no tears and no RenderBinding splits. Values of bend and lra match AVER_PHYS_SBX's bendType and lraMode.
enum class ClothSolver : u8 { Cage = 0, Fast = 1 };
enum class ClothBend : u8 { None = 0, Distance = 1, Dihedral = 2 };
enum class ClothLra : u8 { Off = 0, Euclidean = 1, Geodesic = 2 };

struct ClothParams {
    f32 dragCoeff = 1.0f;
    f32 liftCoeff = 0.3f;
    bool tearable = false;
    f32 defaultReachCm = 6.0f;
    f32 backstopCm = -1.0f;    // < 0 off
    // ---- wave 11 (written only when they differ) ----
    ClothSolver solver = ClothSolver::Cage;
    ClothBend bend = ClothBend::Distance;      // Fast only
    ClothLra lra = ClothLra::Off;              // Fast only: long range attachments from the pinned vertices
    f32 lraSlack = 1.02f;                      // Fast only: the LRA distance multiplier, >= 1
};

// A closed cloth with a pressure is an inflatable (a beach ball, a balloon). Pressure is kPa at REST volume.
struct InflateParams {
    f32 pressureKPa = 0.0f;      // 0 = off
    f32 leakKPaPerS = 0.0f;      // the system ramps the pressure down by this much per second (a puncture)
};

// A cloth that never collides with the ragdoll of the entity that owns its skeleton (a cape on a ragdolled character).
struct GroupParams {
    bool ragdollOwner = false;   // "group": { "ragdollOf": "owner" }
};

// Jelly (kind "jelly"): a voxel tetrahedral lattice inside the mesh, the render mesh embedded by barycentric coordinates.
struct JellyParams {
    f32 cellCm = 10.0f;
    f32 snapCm = 3.0f;                 // lattice nodes this close to the surface move onto it
    f32 volumeCompliance = 2e-4f;      // per tetrahedron, 0 = incompressible
    f32 edgeCompliance = 5e-5f;
    f32 vertexRadiusCm = -1.0f;        // < 0: 0.35 x the cell
    f32 densityKgM3 = 300.0f;
    u32 maxNodes = 2048;
};
// The collision radius of a lattice vertex: the asset's, else 0.35 of the cell (hides the half-cell the render mesh may poke out).
inline f32 jellyVertexRadiusCm(const JellyParams& j, f32 cellCm) { return j.vertexRadiusCm >= 0.0f ? j.vertexRadiusCm : 0.35f * cellCm; }

// Rope and cable (kind "rope"): a chain of Cosserat rods drawn as a generated tube.
struct RopeParams {
    f32 radiusCm = 1.2f;
    f32 segmentCm = 12.0f;
    f32 massPerMeterKg = 0.4f;
    f32 slack = 1.02f;                 // rest length = the distance between the ends x slack, unless the component names a length
    u32 sides = 8;
    f32 uvCmPerTile = 20.0f;
    f32 stretchCompliance = 1e-8f;
    f32 bendTwistCompliance = 1e-3f;
    std::string material;              // a material NAME for the tube; empty keeps the entity's
};

struct DestructParams {
    f32 minImpactCmS = 150.0f;
    f32 impulseEfficiency = 0.6f;
    f32 maxKickCmS = 1200.0f;
    u32 minPieceTriangles = 4;
    f32 debrisLifetimeS = 12.0f;
    u32 maxPieces = 16;
    f32 impactRadiusScale = 1.0f;    // multiplies the impact deposit radius
    bool bake = true;                // a debris piece asleep long enough is redrawn as a plain static mesh
};

// ---- the optional depth sections (all written only when they differ from these defaults) -------------

enum class CapMode : u8 { Auto = 0, Off = 1, On = 2 };   // Auto: on for a destructible, off for cloth

struct CapParams {
    CapMode mode = CapMode::Auto;
    f32 thicknessCm = -1.0f;         // < 0: the preset's capThicknessCm
    BackSkin backSkin = BackSkin::Auto;
    f32 uvCmPerTile = 40.0f;
    f32 lodCm = 2500.0f;             // beyond this distance from the camera a piece draws without caps
    std::string interiorMaterial;    // a material NAME for the walls; empty merges them into the surface mesh
};

enum class FxSlot : u8 { Impact = 0, Tear = 1, Detach = 2, Dust = 3, Release = 4 };
inline constexpr u32 kFxSlotCount = 5, kMaxFxSounds = 8;

struct FxSlotParams {
    std::string particle;                 // content-relative path of a GPU_PARTICLES effect; empty = none
    std::vector<std::string> sounds;      // content-relative paths, up to kMaxFxSounds; the variant is hash(seq) % n
    f32 minSpeedCmS = 0.0f;               // Impact only
    f32 count = 1.0f;                     // base particle count
    f32 countPerMs = 0.0f;                // plus this per m/s of the event's value (closing speed, or the broken-edge count)
    f32 countPerParticle = 0.5f;          // Dust: particles = this x the cage particles that became dust
    f32 volume = 1.0f;
    f32 pitchJitter = 0.0f;
    f32 cooldownS = 0.1f;                 // per entry and slot
    f32 innerCm = 300.0f, outerCm = 4000.0f;
    bool bound() const { return !particle.empty() || !sounds.empty(); }
};

struct EffectParams {
    FxSlotParams slot[kFxSlotCount];
    bool any() const {
        for (const FxSlotParams& f : slot)
            if (f.bound()) return true;
        return false;
    }
};

struct ProxyParams {
    std::string mesh;                // content-relative .ocmesh; when set the simulation runs on it
    u32 maxParticles = 0;            // 0 = sim.maxParticles
};

// One rule that pins or limits particles. Positions are MESH-LOCAL (the .ocmesh's own space).
struct PaintRule {
    enum class Type : u8 { Above = 0, Bone = 1, Sphere = 2, Mask = 3 };
    Type type = Type::Above;
    u8 axis = 2;                 // Above: 0 x, 1 y, 2 z
    f32 valueCm = 0.0f;          // Above: pinned when the coordinate is >= this
    std::string bone;            // Bone: pinned when the vertex weight to this bone >= minWeight
    f32 minWeight = 0.5f;
    f32 centreCm[3] = {0, 0, 0}; // Sphere
    f32 radiusCm = 0.0f;
    std::string texture;         // Mask: R below pinBelow pins, G scales the reach
    f32 pinBelow = 0.5f;
};

// Per welded sim particle, keyed by `meshKey` (simMeshKey). pin: 255 = pinned. reach: 0 = hard skin,
// 255 = free, otherwise linear in (0, 2 * defaultReachCm].
struct PaintData {
    u64 meshKey = 0;
    std::vector<u8> pin;
    std::vector<u8> reach;
    std::vector<PaintRule> rules;

    bool hasPin() const { return !pin.empty(); }
    bool hasReach() const { return !reach.empty(); }
    bool empty() const { return pin.empty() && reach.empty() && rules.empty(); }
    // True when painted data exists for a different mesh than `currentKey`.
    bool stale(u64 currentKey) const { return (hasPin() || hasReach()) && meshKey != currentKey; }
};

struct SoftAsset {
    SoftKind kind = SoftKind::Cloth;
    SoftPreset preset = SoftPreset::ClothLight;
    Material material = presetMaterial(SoftPreset::ClothLight);
    SimParams sim;
    ClothParams cloth;
    DestructParams destruct;
    CapParams caps;
    EffectParams effects;
    ProxyParams proxy;
    PaintData paint;
    // ---- wave 11 ----
    InflateParams inflate;
    GroupParams group;
    JellyParams jelly;
    RopeParams rope;

    // False with `why` set for a value the runtime cannot honour.
    bool valid(std::string* why = nullptr) const;
};

// The asset a preset alone describes: material, iterations and damping from the preset table.
SoftAsset makeSoftAsset(SoftKind kind, SoftPreset preset);

const char* fxSlotKey(FxSlot slot);
// Whether this asset's breaks get caps (Auto: a destructible), and their thickness in cm (the preset's when unset).
bool capsEnabled(const SoftAsset& a);
f32 capThicknessCm(const SoftAsset& a);
// Hash of every resolved value that shapes the cage (material, preset, sim, paint, proxy); the saved-damage key.
// Changes to caps, effects, debris lifetime and the other cosmetic values leave it alone.
u64 softAssetKey(const SoftAsset& a);

// Parses .ocsoft text. Unknown keys are ignored, a newer version is refused, missing keys fall back
// to the preset. False with `why` on malformed or invalid input.
bool parseSoftAsset(std::string_view text, SoftAsset& out, std::string* why = nullptr);
bool loadSoftAsset(const std::string& path, SoftAsset& out, std::string* why = nullptr);

// Deterministic text: write -> parse -> write is byte-identical.
std::string writeSoftAsset(const SoftAsset& a);
bool saveSoftAsset(const std::string& path, const SoftAsset& a, std::string* why = nullptr);

// Paint byte arrays as hex run-length pairs (value, count), four hex digits per run.
std::string encodePaintRle(const std::vector<u8>& bytes);
bool decodePaintRle(std::string_view hex, std::vector<u8>& out);

// Skin reach for a painted byte: < 0 free, 0 hard skin.
f32 reachFromPaint(u8 byte, f32 defaultReachCm);

} // namespace aver::softbody
