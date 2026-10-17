// .ocsoft: the soft-body asset (JSON, "format":"ocsoft", "version":1), and the paint data in it.
//
// A cloth or a destructible, resolved from a preset plus overrides. Pure text and arithmetic: no
// scene, no GPU, no Jolt. See docs/SOFTBODY.md section ".ocsoft" for the schema.
#pragma once

#include "aver/softbody/Material.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::softbody {

inline constexpr u32 kSoftAssetVersion = 1;

enum class SoftKind : u8 { Cloth = 0, Destructible = 1 };

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

struct ClothParams {
    f32 dragCoeff = 1.0f;
    f32 liftCoeff = 0.3f;
    bool tearable = false;
    f32 defaultReachCm = 6.0f;
    f32 backstopCm = -1.0f;    // < 0 off
};

struct DestructParams {
    f32 minImpactCmS = 150.0f;
    f32 impulseEfficiency = 0.6f;
    f32 maxKickCmS = 1200.0f;
    u32 minPieceTriangles = 4;
    f32 debrisLifetimeS = 12.0f;
    u32 maxPieces = 16;
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
    PaintData paint;

    // False with `why` set for a value the runtime cannot honour.
    bool valid(std::string* why = nullptr) const;
};

// The asset a preset alone describes: material, iterations and damping from the preset table.
SoftAsset makeSoftAsset(SoftKind kind, SoftPreset preset);

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
