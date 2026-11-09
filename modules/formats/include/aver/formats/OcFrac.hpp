#pragma once
// `.ocfrac`: a pre-fractured mesh (W7). An AVR1 container, subtype 'FRAC': the pieces of one source mesh in a single shared vertex/index pool, each
// piece's convex-hull points, mass data and per-material index ranges, the contact links between pieces with a break threshold each, and the anchor
// flags. Units are the source mesh's own; the runtime (Aver.Destruct) maps them to centimetres. Text-free, no engine type, no device.
#include "aver/core/Types.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/formats/Fracture.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

inline constexpr u32 kAvrSubtypeFrac     = avrFourCC("FRAC");
inline constexpr u32 kOcFracChunkHeader  = avrFourCC("FHDR");
inline constexpr u32 kOcFracChunkVerts   = avrFourCC("FVTX");   // 8 f32 per vertex: position, normal, uv
inline constexpr u32 kOcFracChunkIndices = avrFourCC("FIDX");   // u32 triangle corners
inline constexpr u32 kOcFracChunkRanges  = avrFourCC("FSUB");   // per piece-material range: i32 material, u32 indexStart, u32 indexCount
inline constexpr u32 kOcFracChunkPieces  = avrFourCC("FPCS");
inline constexpr u32 kOcFracChunkHull    = avrFourCC("FHUL");   // 3 f32 per hull point
inline constexpr u32 kOcFracChunkLinks   = avrFourCC("FLNK");
inline constexpr u32 kOcFracChunkAnchors = avrFourCC("FANC");   // one byte per piece

inline constexpr u32 kOcFracMaxPieces = 1u << 16;

struct OcFracRange { i32 material = 0; u32 indexStart = 0, indexCount = 0; };

struct OcFracPiece {
    f32 volume = 0, area = 0;
    f32 centroid[3] = {0, 0, 0};
    u32 rangeStart = 0, rangeCount = 0;    // into OcFracData::ranges
    u32 hullStart = 0, hullCount = 0;      // points into OcFracData::hull
};

struct OcFracLink {
    u32 a = 0, b = 0;                      // piece indices
    f32 area = 0;                          // shared face area, source units squared
    f32 threshold = 0;                     // strain that breaks it; the same units as the HitEvent magnitude the runtime feeds in (kg*cm/s)
};

struct OcFracData {
    std::vector<f32> verts;                // 8 per vertex
    std::vector<u32> indices;
    std::vector<OcFracRange> ranges;
    std::vector<OcFracPiece> pieces;
    std::vector<f32> hull;                 // 3 per point
    std::vector<OcFracLink> links;
    std::vector<u8> anchors;               // 1 = fixed to the world
    i32 insideMaterial = 0;
    f32 boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
    f32 sourceVolume = 0;

    u32 vertexCount() const { return static_cast<u32>(verts.size() / 8); }
};

struct OcFracBuild {
    double snap = 0.0;                     // vertex weld distance in source units (0 = none)
    double toughness = 1.0;                // link threshold = toughness * contact area
};

// Packs a fracture and its connectivity graph (anchors) into the asset. `graph` may be a makeGraph() result with no anchors set.
OcFracData buildOcFrac(const fracture::Result& r, const fracture::Graph& graph, const OcFracBuild& opt = {});

// Checks every index, range and link; false with `why` on the first problem.
bool validateOcFrac(const OcFracData& d, std::string* why = nullptr);

bool writeOcFrac(const OcFracData& d, std::vector<u8>& out, std::string* why = nullptr);
bool saveOcFrac(const std::string& path, const OcFracData& d, std::string* why = nullptr);
bool parseOcFrac(const u8* bytes, usize size, OcFracData& out, std::string* why = nullptr);
bool loadOcFrac(const std::string& path, OcFracData& out, std::string* why = nullptr);

}  // namespace aver::fmt
