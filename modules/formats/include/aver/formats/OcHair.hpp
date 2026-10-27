#pragma once
// `.ochair` strand hair groom: AVR1 container with a HEAD text chunk plus optional STRN strands and SCLP embedded scalp.
// Format spec: docs/HAIR.md. Units are engine cm, seconds and engine axes.
#include "aver/formats/Avr1.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr u32 kAvrSubtypeHair = avrFourCC("HAIR");
inline constexpr u32 kOcHairChunkHead = avrFourCC("HEAD");
inline constexpr u32 kOcHairChunkStrands = avrFourCC("STRN");
inline constexpr u32 kOcHairChunkScalp = avrFourCC("SCLP");

inline constexpr u32 kOcHairVersion = 1;                 // OCHAIR record number written; newer numbers are refused
inline constexpr u32 kOcHairMaxStrandVertices = 255;     // Jolt keeps a strand's vertex count in 8 bits
inline constexpr u32 kOcHairMinStrandVertices = 3;
inline constexpr u32 kOcHairMaxMaterials = 255;          // same 8-bit limit for the material index
inline constexpr u32 kOcHairMaxScalpJoints = 16;
inline constexpr u32 kOcHairDefaultMaxRenderStrands = 40000;

enum class OcHairProxyShape : u8 { Sphere = 0, Capsule = 1, Box = 2 };

// Fractions along the strand: 0 = root, 1 = tip.
struct OcHairGradient {
    f32 min = 0.0f, max = 1.0f, minFraction = 0.0f, maxFraction = 1.0f;
    bool operator==(const OcHairGradient& o) const { return min == o.min && max == o.max && minFraction == o.minFraction && maxFraction == o.maxFraction; }
};

// SIM: settings shared by every material.
struct OcHairSim {
    f32 iterationsPerSecond = 360.0f;
    f32 maxDeltaTime = 0.0333f;
    f32 grid[3] = {32.0f, 32.0f, 32.0f};
    f32 boundsPaddingCm = 10.0f;
    f32 simFraction = 0.1f;
    u32 resample = 0;                                    // 0 keeps strands as authored; else each is resampled to this many vertices
    f32 initialGravity[3] = {0.0f, 0.0f, -981.0f};       // cm/s^2
};

// MATERIAL: simulation parameters of one strand material (Jolt's HairSettings::Material, engine units).
struct OcHairMaterial {
    u32 index = 0;
    bool collision = true;
    bool lra = true;
    f32 linearDamping = 2.0f;
    f32 angularDamping = 2.0f;
    f32 maxLinearCmS = 1000.0f;
    f32 maxAngularRadS = 50.0f;
    OcHairGradient gravity{0.1f, 1.0f, 0.2f, 0.8f};
    f32 friction = 0.2f;
    f32 bendCompliance = 1.0e-7f;
    f32 bendMul[4] = {1.0f, 100.0f, 100.0f, 1.0f};
    f32 stretchCompliance = 1.0e-8f;
    f32 inertiaMul = 10.0f;
    OcHairGradient radiusCm{0.1f, 0.1f, 0.0f, 1.0f};
    OcHairGradient worldInfluence{0.0f, 1.0f, 0.0f, 1.0f};
    OcHairGradient gridVelocity{0.05f, 0.01f, 0.0f, 1.0f};
    f32 gridDensityForce = 0.0f;
    OcHairGradient globalPose{0.01f, 0.0f, 0.0f, 0.3f};
    OcHairGradient skinGlobalPose{1.0f, 0.0f, 0.0f, 0.1f};
    f32 gravityPreload = 0.0f;
    f32 simFraction = -1.0f;                             // < 0: the SIM value
};

// LOOK: shading of one strand material (render only).
struct OcHairLook {
    u32 material = 0;
    f32 baseColor[3] = {0.35f, 0.22f, 0.12f};
    f32 tipColor[3] = {0.55f, 0.40f, 0.25f};
    f32 colorVariation = 0.15f;
    f32 widthRootCm = 0.12f;
    f32 widthTipCm = 0.04f;
    f32 roughness = 0.35f;
    f32 shift = 0.07f;
    f32 tt = 1.0f;
    f32 trt = 1.0f;
    f32 azimuthRough = 0.4f;
    f32 tint[3] = {0.9f, 0.6f, 0.4f};
    f32 scatter = 0.5f;
};

// GENERATE: procedural groom, read by HairGenerate.hpp in Aver.Hair.
struct OcHairGenerate {
    bool present = false;
    u32 seed = 1;
    u32 strands = 0;
    f32 lengthCm[2] = {28.0f, 42.0f};
    u32 segments = 12;                                   // points per strand = segments + 1
    f32 curlCm = 0.0f;
    f32 curlFrequency = 3.0f;                            // turns per 10 cm
    u32 clumps = 0;
    f32 clumpRadiusCm = 1.5f;
    f32 clumpTightness = 0.6f;
    f32 partDirection[3] = {0.0f, 1.0f, 0.0f};
    f32 partWidthCm = 0.0f;
    f32 style[3] = {0.0f, 0.0f, -1.0f};                  // direction long strands bend towards
    f32 styleWeight = 0.7f;
    std::string mask;                                    // red channel weights the roots; empty = uniform
    u32 material = 0;
};

// LOD: render strand fractions by distance (cm); three distances separate four levels.
struct OcHairLod {
    f32 distances[3] = {300.0f, 700.0f, 1500.0f};
    f32 strandFractions[4] = {1.0f, 0.5f, 0.25f, 0.12f};
};

// PROXY: kinematic convex hull that follows a bone and collides with hair.
struct OcHairProxy {
    std::string name;
    std::string bone;
    OcHairProxyShape shape = OcHairProxyShape::Sphere;
    f32 sizeCm[3] = {10.0f, 0.0f, 0.0f};                 // sphere: radius; capsule: radius, half height; box: half extents
    f32 offsetCm[3] = {0.0f, 0.0f, 0.0f};                // in the bone's frame
};

struct OcHairStrands {
    std::vector<f32> points;                             // cm, scalp-mesh space; 3 per point
    std::vector<u32> start;                              // strandCount + 1 entries
    std::vector<u8> material;                            // per strand
    std::vector<f32> width;                              // per strand in cm; empty = the look's
    usize strandCount() const { return start.empty() ? 0 : start.size() - 1; }
    usize pointCount() const { return points.size() / 3; }
    bool empty() const { return start.size() < 2; }
};

struct OcHairScalp {
    std::vector<f32> positions;                          // cm
    std::vector<u32> triangles;
    std::vector<u8> joints;                              // 4 per vertex; index into scalpJoints
    std::vector<f32> weights;                            // 4 per vertex
    usize vertexCount() const { return positions.size() / 3; }
    usize triangleCount() const { return triangles.size() / 3; }
};

struct OcHairData {
    std::string scalpMesh;                               // .ocmesh path; empty when the groom embeds its scalp
    i32 scalpSubmesh = -1;                               // -1 = every submesh
    std::vector<std::string> scalpJoints;
    OcHairSim sim;
    std::vector<OcHairMaterial> materials;               // by index; an index with no record uses the defaults
    std::vector<OcHairLook> looks;
    OcHairGenerate generate;
    OcHairLod lod;
    std::vector<OcHairProxy> proxies;
    u32 maxRenderStrands = kOcHairDefaultMaxRenderStrands;
    OcHairStrands strands;
    bool hasScalp = false;
    OcHairScalp scalp;

    // Material or look record with this index, or null.
    const OcHairMaterial* findMaterial(u32 index) const;
    const OcHairLook* findLook(u32 material) const;

    // False with `why` set for malformed data, including a groom with neither strands nor a GENERATE record.
    bool valid(std::string* why = nullptr) const;
};

// Failures set `why` when given.
bool parseOcHair(const u8* bytes, usize size, OcHairData& out, std::string* why = nullptr);
bool loadOcHair(const std::string& path, OcHairData& out, std::string* why = nullptr);

// Refuses a groom that fails valid(). Write, parse, write is byte-identical.
bool writeOcHair(const OcHairData& in, std::vector<u8>& out, std::string* why = nullptr);
bool saveOcHair(const std::string& path, const OcHairData& in, std::string* why = nullptr);

// HEAD text alone, so hand-edited text can be applied to a loaded groom; parseOcHairHead leaves `out` unchanged on failure.
std::string ocHairHeadText(const OcHairData& in);
bool parseOcHairHead(std::string_view text, OcHairData& out, std::string* why = nullptr);

} // namespace aver::fmt
