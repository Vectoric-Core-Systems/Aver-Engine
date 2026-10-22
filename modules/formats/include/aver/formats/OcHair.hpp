#pragma once
// `.ochair` -- a strand HAIR GROOM: where the strands are, how they simulate, how they look and how they thin out with distance.
// docs/plans/wave11/SOFTBODY_HAIR.md section 2.12, docs/HAIR.md.
//
// THE AVR1 CONTAINER (Avr1.hpp), following OcLand/OcBt/OcFoliage: a required text chunk plus binary chunks. Strand data is binary because 20k
// strands of 12 points is 2.9 MB of floats; every AUTHORED parameter is text so a diff reads and a hand edit is possible.
//
//   HEAD (required)  the records below, UTF-8 text (flag Text)
//   STRN (optional)  u32 strandCount, u32 pointCount; f32 points[pointCount * 3] (cm, mesh-local of the scalp mesh, engine axes);
//                    u32 start[strandCount + 1]; u8 material[strandCount]; f32 width[strandCount] OPTIONAL (cm; absent = the look's width)
//   SCLP (optional)  an EMBEDDED SCALP so a generated groom is self-contained: u32 vertexCount, u32 triCount; f32 position[vertexCount * 3];
//                    u32 tri[triCount * 3]; u8 joint[vertexCount * 4]; f32 weight[vertexCount * 4]. A joint is an index into SCALPJOINTS.
//
// HEAD records, one per line; a line that begins with white space continues the previous record; '#' starts a comment; keys and numbers are
// separated by white space and a key is followed by as many numbers as it takes. UNKNOWN RECORDS AND KEYS ARE SKIPPED (one warning per file) so a
// groom written by a newer engine still loads; a newer OCHAIR number is refused.
//
//   OCHAIR 1
//   SCALP Meshes/Head.ocmesh [SUBMESH n]        the mesh the roots grow from (triangles, and joints and weights if it is skinned)
//   SCALPJOINTS head neck_01 spine_03           joint names; the order is the skin weight index; resolved against the skeleton at attach
//   SIM iterationsPerSecond 360 maxDeltaTime 0.0333 grid 32 32 32 boundsPaddingCm 10 simFraction 0.1 resample 12 initialGravity 0 0 -981
//   MATERIAL 0 collision 1 lra 1 linearDamping 2 angularDamping 2 maxLinearCmS 1000 maxAngularRadS 50 gravity 0.1 1 0.2 0.8 friction 0.2
//          bendCompliance 1e-7 bendMul 1 100 100 1 stretchCompliance 1e-8 inertiaMul 10 radiusCm 0.1 0.1 0 1 worldInfluence 0 1 0 1
//          gridVelocity 0.05 0.01 0 1 gridDensityForce 0 globalPose 0.01 0 0 0.3 skinGlobalPose 1 0 0 0.1 gravityPreload 0 simFraction 0.1
//   LOOK 0 baseColor 0.35 0.22 0.12 tipColor 0.55 0.4 0.25 colorVariation 0.15 widthRootCm 0.12 widthTipCm 0.04 roughness 0.35 shift 0.07
//          tt 1 trt 1 azimuthRough 0.4 tint 0.9 0.6 0.4 scatter 0.5
//   GENERATE seed 12345 strands 18000 lengthCm 28 42 segments 12 curlCm 1.2 curlFrequency 3 clumps 220 clumpRadiusCm 1.5 clumpTightness 0.6
//          partDirection 0 1 0 partWidthCm 0.8 style 0 0 -1 styleWeight 0.7 mask Textures/HairMask.octex material 0
//   LOD distances 300 700 1500 strandFractions 1 0.5 0.25 0.12
//   PROXY head bone head shape sphere size 11 offset 0 2 4
//   MAXSTRANDS 40000
//
// A gradient (gravity, radiusCm, worldInfluence, gridVelocity, globalPose, skinGlobalPose) is four numbers: min, max, the fraction along the
// strand where the value is min, the fraction where it is max. Units are the engine's: centimetres, seconds, engine axes; the physics ABI
// converts to Jolt's once. A `simFraction` in a MATERIAL overrides the SIM one for that material. LOOK and LOD are render-only; MATERIAL and SIM
// are the simulation. GENERATE is deterministic from its seed: a groom with GENERATE and no STRN is regenerated at load, and with STRN the
// explicit strands win and GENERATE is ignored.
#include "aver/formats/Avr1.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr u32 kAvrSubtypeHair = avrFourCC("HAIR");
inline constexpr u32 kOcHairChunkHead = avrFourCC("HEAD");
inline constexpr u32 kOcHairChunkStrands = avrFourCC("STRN");
inline constexpr u32 kOcHairChunkScalp = avrFourCC("SCLP");

inline constexpr u32 kOcHairVersion = 1;                 // the OCHAIR record number this build writes and the newest it reads
inline constexpr u32 kOcHairMaxStrandVertices = 255;     // Jolt keeps a strand's vertex count in 8 bits
inline constexpr u32 kOcHairMinStrandVertices = 3;
inline constexpr u32 kOcHairMaxMaterials = 255;          // and the material index
inline constexpr u32 kOcHairMaxScalpJoints = 16;
inline constexpr u32 kOcHairDefaultMaxRenderStrands = 40000;

enum class OcHairProxyShape : u8 { Sphere = 0, Capsule = 1, Box = 2 };

// min, max and where along the strand (0 root, 1 tip) each is reached.
struct OcHairGradient {
    f32 min = 0.0f, max = 1.0f, minFraction = 0.0f, maxFraction = 1.0f;
    bool operator==(const OcHairGradient& o) const { return min == o.min && max == o.max && minFraction == o.minFraction && maxFraction == o.maxFraction; }
};

// SIM: the settings shared by every material of the groom.
struct OcHairSim {
    f32 iterationsPerSecond = 360.0f;
    f32 maxDeltaTime = 0.0333f;
    f32 grid[3] = {32.0f, 32.0f, 32.0f};
    f32 boundsPaddingCm = 10.0f;
    f32 simFraction = 0.1f;
    u32 resample = 0;                                    // 0 = strands as they are, else every strand is resampled to this many vertices
    f32 initialGravity[3] = {0.0f, 0.0f, -981.0f};       // cm/s^2, engine axes
};

// MATERIAL: the simulation parameters of one strand material (Jolt's HairSettings::Material, in engine units).
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
    f32 simFraction = -1.0f;                             // < 0: the SIM one
};

// LOOK: how one strand material is shaded (render only).
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

// GENERATE: the procedural groom (HairGenerate.hpp in Aver.Hair reads it).
struct OcHairGenerate {
    bool present = false;
    u32 seed = 1;
    u32 strands = 0;
    f32 lengthCm[2] = {28.0f, 42.0f};
    u32 segments = 12;                                   // points per strand = segments + 1
    f32 curlCm = 0.0f;                                   // helix amplitude
    f32 curlFrequency = 3.0f;                            // turns per 10 cm
    u32 clumps = 0;
    f32 clumpRadiusCm = 1.5f;
    f32 clumpTightness = 0.6f;
    f32 partDirection[3] = {0.0f, 1.0f, 0.0f};
    f32 partWidthCm = 0.0f;
    f32 style[3] = {0.0f, 0.0f, -1.0f};                  // the "fall" direction a long strand bends towards
    f32 styleWeight = 0.7f;
    std::string mask;                                    // a texture whose red channel weights the roots; empty = uniform
    u32 material = 0;
};

// LOD: render strand fractions by distance (cm). Three distances separate four levels.
struct OcHairLod {
    f32 distances[3] = {300.0f, 700.0f, 1500.0f};
    f32 strandFractions[4] = {1.0f, 0.5f, 0.25f, 0.12f};
};

// PROXY: a kinematic convex hull that follows a bone and collides with hair.
struct OcHairProxy {
    std::string name;
    std::string bone;
    OcHairProxyShape shape = OcHairProxyShape::Sphere;
    f32 sizeCm[3] = {10.0f, 0.0f, 0.0f};                 // sphere: radius; capsule: radius, half height; box: half extents
    f32 offsetCm[3] = {0.0f, 0.0f, 0.0f};                // from the bone, in the bone's frame
};

// The explicit strands (STRN).
struct OcHairStrands {
    std::vector<f32> points;                             // cm, scalp-mesh space, engine axes; 3 per point
    std::vector<u32> start;                              // strandCount + 1 entries
    std::vector<u8> material;                            // per strand
    std::vector<f32> width;                              // per strand in cm; empty = the look's
    usize strandCount() const { return start.empty() ? 0 : start.size() - 1; }
    usize pointCount() const { return points.size() / 3; }
    bool empty() const { return start.size() < 2; }
};

// The embedded scalp (SCLP).
struct OcHairScalp {
    std::vector<f32> positions;                          // cm, 3 per vertex
    std::vector<u32> triangles;                          // 3 per triangle
    std::vector<u8> joints;                              // 4 per vertex: an index into scalpJoints
    std::vector<f32> weights;                            // 4 per vertex
    usize vertexCount() const { return positions.size() / 3; }
    usize triangleCount() const { return triangles.size() / 3; }
};

struct OcHairData {
    std::string scalpMesh;                               // SCALP: an .ocmesh path, empty when the groom embeds its scalp
    i32 scalpSubmesh = -1;                               // -1 = every submesh
    std::vector<std::string> scalpJoints;                // SCALPJOINTS
    OcHairSim sim;
    std::vector<OcHairMaterial> materials;               // by index; an index with no record simulates with the defaults
    std::vector<OcHairLook> looks;
    OcHairGenerate generate;
    OcHairLod lod;
    std::vector<OcHairProxy> proxies;
    u32 maxRenderStrands = kOcHairDefaultMaxRenderStrands;
    OcHairStrands strands;                               // STRN, optional
    bool hasScalp = false;
    OcHairScalp scalp;                                   // SCLP, when hasScalp

    // The material or look record with this index, or null.
    const OcHairMaterial* findMaterial(u32 index) const;
    const OcHairLook* findLook(u32 material) const;

    // False with `why` set for: a strand of fewer than 3 or more than 255 points, a material index above 255, more strands than maxRenderStrands,
    // a non-finite number, more than kOcHairMaxScalpJoints scalp joints, malformed tables, a SCALP that is not an .ocmesh path, a gradient whose
    // two fractions coincide, an embedded scalp that does not hold together, and a groom with neither strands nor a GENERATE record.
    bool valid(std::string* why = nullptr) const;
};

// Reads a groom. `why` receives the reason on failure.
bool parseOcHair(const u8* bytes, usize size, OcHairData& out, std::string* why = nullptr);
bool loadOcHair(const std::string& path, OcHairData& out, std::string* why = nullptr);

// Writes a groom. Refuses (false) one that fails valid(), as writeOcFoliage does. Writing, parsing and writing again is byte-identical.
bool writeOcHair(const OcHairData& in, std::vector<u8>& out, std::string* why = nullptr);
bool saveOcHair(const std::string& path, const OcHairData& in, std::string* why = nullptr);

// The HEAD text on its own: what writeOcHair puts in the HEAD chunk, and the reader of it (strands and scalp untouched, so a hand-edited
// text can be applied to a loaded groom). parseOcHairHead fails on a bad record and leaves `out` unchanged.
std::string ocHairHeadText(const OcHairData& in);
bool parseOcHairHead(std::string_view text, OcHairData& out, std::string* why = nullptr);

} // namespace aver::fmt
