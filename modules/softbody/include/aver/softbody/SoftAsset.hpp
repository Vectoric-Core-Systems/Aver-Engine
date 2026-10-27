// .ocsoft soft-body asset: a cloth or destructible resolved from a preset plus overrides. Schema: docs/SOFTBODY.md.
#pragma once

#include "aver/softbody/Caps.hpp"
#include "aver/softbody/Material.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::softbody {

inline constexpr u32 kSoftAssetVersion = 1;

// Jelly and Rope are simulated by SoftShapeSystem, not SoftBodySystem.
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

// Cage: beams and dihedral bends from the mesh (dents, tears). Fast: Jolt builds its own edges
// (aver_phys_sbx_builder_auto). Bend and lra values match AVER_PHYS_SBX's bendType and lraMode.
enum class ClothSolver : u8 { Cage = 0, Fast = 1 };
enum class ClothBend : u8 { None = 0, Distance = 1, Dihedral = 2 };
enum class ClothLra : u8 { Off = 0, Euclidean = 1, Geodesic = 2 };

struct ClothParams {
    f32 dragCoeff = 1.0f;
    f32 liftCoeff = 0.3f;
    bool tearable = false;
    f32 defaultReachCm = 6.0f;
    f32 backstopCm = -1.0f;    // < 0 off
    // Written only when they differ from the defaults; bend, lra and lraSlack are Fast only (lraSlack >= 1).
    ClothSolver solver = ClothSolver::Cage;
    ClothBend bend = ClothBend::Distance;
    ClothLra lra = ClothLra::Off;
    f32 lraSlack = 1.02f;
};

// A closed cloth with a pressure. Pressure is kPa at rest volume.
struct InflateParams {
    f32 pressureKPa = 0.0f;
    f32 leakKPaPerS = 0.0f;
};

// Cloth that never collides with its skeleton owner's ragdoll.
struct GroupParams {
    bool ragdollOwner = false;
};

// Jelly: a voxel tetrahedral lattice inside the mesh; the render mesh is embedded by barycentric coordinates.
struct JellyParams {
    f32 cellCm = 10.0f;
    f32 snapCm = 3.0f;
    f32 volumeCompliance = 2e-4f;      // per tetrahedron, 0 = incompressible
    f32 edgeCompliance = 5e-5f;
    f32 vertexRadiusCm = -1.0f;        // < 0: 0.35 x the cell
    f32 densityKgM3 = 300.0f;
    u32 maxNodes = 2048;
};
// Collision radius of a lattice vertex: the asset's, else 0.35 of the cell.
inline f32 jellyVertexRadiusCm(const JellyParams& j, f32 cellCm) { return j.vertexRadiusCm >= 0.0f ? j.vertexRadiusCm : 0.35f * cellCm; }

// Rope and cable: a chain of Cosserat rods drawn as a generated tube.
struct RopeParams {
    f32 radiusCm = 1.2f;
    f32 segmentCm = 12.0f;
    f32 massPerMeterKg = 0.4f;
    f32 slack = 1.02f;                 // rest length = distance between the ends x slack, unless a length is given
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
    f32 impactRadiusScale = 1.0f;
    bool bake = true;                  // a debris piece asleep long enough becomes a plain static mesh
};

enum class CapMode : u8 { Auto = 0, Off = 1, On = 2 };

struct CapParams {
    CapMode mode = CapMode::Auto;      // Auto: on for a destructible, off for cloth
    f32 thicknessCm = -1.0f;
    BackSkin backSkin = BackSkin::Auto;
    f32 uvCmPerTile = 40.0f;
    f32 lodCm = 2500.0f;               // beyond this distance from the camera a piece draws without caps
    std::string interiorMaterial;
};

enum class FxSlot : u8 { Impact = 0, Tear = 1, Detach = 2, Dust = 3, Release = 4 };
inline constexpr u32 kFxSlotCount = 5, kMaxFxSounds = 8;

struct FxSlotParams {
    std::string particle;              // content-relative path of a GPU_PARTICLES effect; empty = none
    std::vector<std::string> sounds;   // content-relative paths, up to kMaxFxSounds; variant is hash(seq) % n
    f32 minSpeedCmS = 0.0f;
    f32 count = 1.0f;
    f32 countPerMs = 0.0f;             // plus this per m/s of the event's value
    f32 countPerParticle = 0.5f;       // Dust: particles = this x the cage particles that became dust
    f32 volume = 1.0f;
    f32 pitchJitter = 0.0f;
    f32 cooldownS = 0.1f;
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
    std::string mesh;                  // content-relative .ocmesh; when set the simulation runs on it
    u32 maxParticles = 0;
};

// One rule that pins or limits particles. Positions are mesh-local.
struct PaintRule {
    enum class Type : u8 { Above = 0, Bone = 1, Sphere = 2, Mask = 3 };
    Type type = Type::Above;
    u8 axis = 2;
    f32 valueCm = 0.0f;
    std::string bone;
    f32 minWeight = 0.5f;
    f32 centreCm[3] = {0, 0, 0};
    f32 radiusCm = 0.0f;
    std::string texture;               // Mask: R below pinBelow pins, G scales the reach
    f32 pinBelow = 0.5f;
};

// Per welded sim particle. pin: 255 = pinned. reach: 0 = hard skin, 255 = free, otherwise linear in (0, 2 * defaultReachCm].
struct PaintData {
    u64 meshKey = 0;
    std::vector<u8> pin;
    std::vector<u8> reach;
    std::vector<PaintRule> rules;

    bool hasPin() const { return !pin.empty(); }
    bool hasReach() const { return !reach.empty(); }
    bool empty() const { return pin.empty() && reach.empty() && rules.empty(); }
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
    InflateParams inflate;
    GroupParams group;
    JellyParams jelly;
    RopeParams rope;

    // False with `why` set for a value the runtime cannot honour.
    bool valid(std::string* why = nullptr) const;
};

SoftAsset makeSoftAsset(SoftKind kind, SoftPreset preset);

const char* fxSlotKey(FxSlot slot);
bool capsEnabled(const SoftAsset& a);
f32 capThicknessCm(const SoftAsset& a);
// Hash of every value that shapes the cage (material, preset, sim, paint, proxy): the saved-damage key.
// Caps, effects, debris lifetime and other cosmetic values leave it alone.
u64 softAssetKey(const SoftAsset& a);

// Unknown keys are ignored, a newer version is refused, missing keys fall back to the preset.
bool parseSoftAsset(std::string_view text, SoftAsset& out, std::string* why = nullptr);
bool loadSoftAsset(const std::string& path, SoftAsset& out, std::string* why = nullptr);

// Deterministic text: write -> parse -> write is byte-identical.
std::string writeSoftAsset(const SoftAsset& a);
bool saveSoftAsset(const std::string& path, const SoftAsset& a, std::string* why = nullptr);

std::string encodePaintRle(const std::vector<u8>& bytes);
bool decodePaintRle(std::string_view hex, std::vector<u8>& out);

// Skin reach for a painted byte: < 0 free, 0 hard skin.
f32 reachFromPaint(u8 byte, f32 defaultReachCm);

} // namespace aver::softbody
